/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "velox/common/memory/MemoryArbitrator.h"

#include "velox/common/future/VeloxPromise.h"
#include "velox/common/memory/Memory.h"

using namespace facebook::velox::memory;

namespace facebook::velox::exec {

/// Used to achieve dynamic memory sharing among running queries. When a
/// memory pool exceeds its current memory capacity, the arbitrator tries to
/// grow its capacity by reclaim the overused memory from the query with
/// more memory usage. We can configure memory arbitrator the way to reclaim
/// memory. For Prestissimo, we can configure it to reclaim memory by
/// aborting a query. For Prestissimo-on-Spark, we can configure it to
/// reclaim from a running query through techniques such as disk-spilling,
/// partial aggregation or persistent shuffle data flushes.
class SharedArbitrator : public memory::MemoryArbitrator {
 public:
  explicit SharedArbitrator(const Config& config);

  ~SharedArbitrator() override;

  static void registerFactory();

  static void unregisterFactory();

  void reserveMemory(MemoryPool* pool, uint64_t /*unused*/) final;

  void releaseMemory(MemoryPool* pool) final;

  bool growMemory(
      MemoryPool* pool,
      const std::vector<std::shared_ptr<MemoryPool>>& candidatePools,
      uint64_t targetBytes) final;

  uint64_t shrinkMemory(
      const std::vector<std::shared_ptr<MemoryPool>>& /*unused*/,
      uint64_t /*unused*/) override final {
    VELOX_NYI("shrinkMemory is not supported by SharedArbitrator");
  }

  Stats stats() const final;

  std::string kind() const override;

  std::string toString() const final;

  // The candidate memory pool stats used by arbitration.
  struct Candidate {
    bool reclaimable{false};
    uint64_t reclaimableBytes{0};
    uint64_t freeBytes{0};
    MemoryPool* pool;

    std::string toString() const;
  };

 private:
  // The kind string of shared arbitrator.
  inline static const std::string kind_{"SHARED"};

  class ScopedArbitration {
   public:
    ScopedArbitration(MemoryPool* requestor, SharedArbitrator* arbitrator);

    ~ScopedArbitration();

   private:
    MemoryPool* const requestor_;
    SharedArbitrator* const arbitrator_;
    const std::chrono::steady_clock::time_point startTime_;
    const ScopedMemoryArbitrationContext arbitrationCtx_;
  };

  // Invoked to check if the memory growth will exceed the memory pool's max
  // capacity limit or the arbitrator's node capacity limit.
  bool checkCapacityGrowth(const MemoryPool& pool, uint64_t targetBytes) const;

  // Invoked to ensure the memory growth request won't exceed the requestor's
  // max capacity as well as the arbitrator's node capacity. If it does, then we
  // first need to reclaim the used memory from the requestor itself to ensure
  // the memory growth won't exceed the capacity limit, and then proceed with
  // the memory arbitration process. The reclaimed memory capacity returns to
  // the arbitrator, and let the memory arbitration process to grow the
  // requestor capacity accordingly.
  bool ensureCapacity(MemoryPool* requestor, uint64_t targetBytes);

  // Invoked to capture the candidate memory pools stats for arbitration.
  static std::vector<Candidate> getCandidateStats(
      const std::vector<std::shared_ptr<MemoryPool>>& pools);

  void sortCandidatesByReclaimableMemory(
      std::vector<Candidate>& candidates) const;

  void sortCandidatesByFreeCapacity(std::vector<Candidate>& candidates) const;

  // Finds the candidate with the largest capacity. For 'requestor', the
  // capacity for comparison including its current capacity and the capacity to
  // grow.
  const Candidate& findCandidateWithLargestCapacity(
      MemoryPool* requestor,
      uint64_t targetBytes,
      const std::vector<Candidate>& candidates) const;

  bool arbitrateMemory(
      MemoryPool* requestor,
      std::vector<Candidate>& candidates,
      uint64_t targetBytes);

  // Invoked to start next memory arbitration request, and it will wait for the
  // serialized execution if there is a running or other waiting arbitration
  // requests.
  void startArbitration(MemoryPool* requestor);

  // Invoked by a finished memory arbitration request to kick off the next
  // arbitration request execution if there are any ones waiting.
  void finishArbitration();

  // Invoked to reclaim free memory capacity from 'candidates' without actually
  // freeing used memory.
  //
  // NOTE: the function might sort 'candidates' based on each candidate's free
  // capacity internally.
  uint64_t reclaimFreeMemoryFromCandidates(
      std::vector<Candidate>& candidates,
      uint64_t targetBytes);

  // Invoked to reclaim used memory capacity from 'candidates'.
  //
  // NOTE: the function might sort 'candidates' based on each candidate's
  // reclaimable memory internally.
  uint64_t reclaimUsedMemoryFromCandidates(
      MemoryPool* requestor,
      std::vector<Candidate>& candidates,
      uint64_t targetBytes);

  // Invoked to reclaim used memory from 'pool' with specified 'targetBytes'.
  // The function returns the actually freed capacity.
  uint64_t reclaim(MemoryPool* pool, uint64_t targetBytes) noexcept;

  // Invoked to abort memory 'pool'.
  void abort(MemoryPool* pool, const std::exception_ptr& error);

  // Invoked to handle the memory arbitration failure to abort the memory pool
  // with the largest capacity to free up memory. The function returns true on
  // success and false if the requestor itself has been selected as the victim.
  // We don't abort the requestor itself but just fails the arbitration to let
  // the user decide to either proceed with the query or fail it.
  bool handleOOM(
      MemoryPool* requestor,
      uint64_t targetBytes,
      std::vector<Candidate>& candidates);

  // Decrement free capacity from the arbitrator with up to 'bytes'. The
  // arbitrator might have less free available capacity. The function returns
  // the actual decremented free capacity bytes.
  uint64_t decrementFreeCapacity(uint64_t bytes);
  uint64_t decrementFreeCapacityLocked(uint64_t bytes);

  // Increment free capacity by 'bytes'.
  void incrementFreeCapacity(uint64_t bytes);
  void incrementFreeCapacityLocked(uint64_t bytes);

  std::string toStringLocked() const;

  Stats statsLocked() const;

  mutable std::mutex mutex_;
  uint64_t freeCapacity_{0};
  // Indicates if there is a running arbitration request or not.
  bool running_{false};

  // The promises of the arbitration requests waiting for the serialized
  // execution.
  std::vector<ContinuePromise> waitPromises_;

  tsan_atomic<uint64_t> numRequests_{0};
  std::atomic<uint64_t> numSucceeded_{0};
  tsan_atomic<uint64_t> numAborted_{0};
  tsan_atomic<uint64_t> numFailures_{0};
  tsan_atomic<uint64_t> queueTimeUs_{0};
  tsan_atomic<uint64_t> arbitrationTimeUs_{0};
  tsan_atomic<uint64_t> numShrunkBytes_{0};
  tsan_atomic<uint64_t> numReclaimedBytes_{0};
  tsan_atomic<uint64_t> reclaimTimeUs_{0};
  tsan_atomic<uint64_t> numNonReclaimableAttempts_{0};
  tsan_atomic<uint64_t> numReserveRequest_{0};
  tsan_atomic<uint64_t> numReleaseRequest_{0};
};
} // namespace facebook::velox::exec
