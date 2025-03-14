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

#include "velox/common/memory/MemoryArbitrator.h"

#include <utility>

#include "velox/common/memory/Memory.h"

namespace facebook::velox::memory {

namespace {
class FactoryRegistry {
 public:
  bool registerFactory(
      const std::string& kind,
      MemoryArbitrator::Factory factory) {
    std::lock_guard<std::mutex> l(mutex_);
    if (map_.find(kind) != map_.end()) {
      return false;
    }
    map_[kind] = std::move(factory);
    return true;
  }

  MemoryArbitrator::Factory& getFactory(const std::string& kind) {
    std::lock_guard<std::mutex> l(mutex_);
    VELOX_USER_CHECK(
        map_.find(kind) != map_.end(),
        "Arbitrator factory for kind {} not registered",
        kind)
    return map_[kind];
  }

  bool unregisterFactory(const std::string& kind) {
    std::lock_guard<std::mutex> l(mutex_);
    VELOX_USER_CHECK(
        map_.find(kind) != map_.end(),
        "Arbitrator factory for kind {} not registered",
        kind)
    return map_.erase(kind);
  }

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, MemoryArbitrator::Factory> map_;
};

FactoryRegistry& arbitratorFactories() {
  static FactoryRegistry registry;
  return registry;
}

// Used to enforce the fixed query memory isolation across running queries.
// When a memory pool exceeds the fixed capacity limit, the query just
// fails with memory capacity exceeded error without arbitration. This is
// used to match the current memory isolation behavior adopted by
// Prestissimo.
//
// TODO: deprecate this legacy policy with kShared policy for Prestissimo
// later.
class NoopArbitrator : public MemoryArbitrator {
 public:
  explicit NoopArbitrator(const Config& config) : MemoryArbitrator(config) {
    VELOX_CHECK(config.kind.empty());
    if (capacity_ != kMaxMemory) {
      LOG(WARNING) << "Query memory capacity[" << succinctBytes(capacity_)
                   << "] is set for " << kind()
                   << " arbitrator which has no capacity enforcement";
    }
  }

  std::string kind() const override {
    return "NOOP";
  }

  // Noop arbitrator has no memory capacity limit so no operation needed for
  // memory pool capacity reserve.
  void reserveMemory(MemoryPool* pool, uint64_t /*unused*/) override {
    pool->grow(pool->maxCapacity());
  }

  // Noop arbitrator has no memory capacity limit so no operation needed for
  // memory pool capacity release.
  void releaseMemory(MemoryPool* /*unused*/) override {
    // No-op
  }

  // Noop arbitrator has no memory capacity limit so no operation needed for
  // memory pool capacity grow.
  bool growMemory(
      MemoryPool* /*unused*/,
      const std::vector<std::shared_ptr<MemoryPool>>& /*unused*/,
      uint64_t /*unused*/) override {
    return false;
  }

  // Noop arbitrator has no memory capacity limit so no operation needed for
  // memory pool capacity shrink.
  uint64_t shrinkMemory(
      const std::vector<std::shared_ptr<MemoryPool>>& /*unused*/,
      uint64_t /*unused*/) override {
    return 0;
  }

  Stats stats() const override {
    Stats stats;
    stats.maxCapacityBytes = kMaxMemory;
    return stats;
  }

  std::string toString() const override {
    return fmt::format(
        "ARBIRTATOR[{} CAPACITY[{}]]",
        kind(),
        capacity_ == kMaxMemory ? "UNLIMITED" : succinctBytes(capacity_));
  }
};

thread_local MemoryArbitrationContext* arbitrationCtx{nullptr};
} // namespace

std::unique_ptr<MemoryArbitrator> MemoryArbitrator::create(
    const Config& config) {
  if (config.kind.empty()) {
    // if kind is not set, return noop arbitrator.
    return std::make_unique<NoopArbitrator>(config);
  }
  auto& factory = arbitratorFactories().getFactory(config.kind);
  return factory(config);
}

bool MemoryArbitrator::registerFactory(
    const std::string& kind,
    MemoryArbitrator::Factory factory) {
  return arbitratorFactories().registerFactory(kind, std::move(factory));
}

void MemoryArbitrator::unregisterFactory(const std::string& kind) {
  arbitratorFactories().unregisterFactory(kind);
}

std::unique_ptr<MemoryReclaimer> MemoryReclaimer::create() {
  return std::unique_ptr<MemoryReclaimer>(new MemoryReclaimer());
}

// static
uint64_t MemoryReclaimer::run(
    const std::function<uint64_t()>& func,
    Stats& stats) {
  uint64_t execTimeUs{0};
  uint64_t bytes{0};
  {
    MicrosecondTimer timer{&execTimeUs};
    bytes = func();
  }
  stats.reclaimExecTimeUs += execTimeUs;
  stats.reclaimedBytes += bytes;
  return bytes;
}

bool MemoryReclaimer::reclaimableBytes(
    const MemoryPool& pool,
    uint64_t& reclaimableBytes) const {
  reclaimableBytes = 0;
  if (pool.kind() == MemoryPool::Kind::kLeaf) {
    return false;
  }
  bool reclaimable{false};
  pool.visitChildren([&](MemoryPool* pool) {
    uint64_t poolReclaimableBytes{0};
    reclaimable |= pool->reclaimableBytes(poolReclaimableBytes);
    reclaimableBytes += poolReclaimableBytes;
    return true;
  });
  VELOX_CHECK(reclaimable || reclaimableBytes == 0);
  return reclaimable;
}

uint64_t
MemoryReclaimer::reclaim(MemoryPool* pool, uint64_t targetBytes, Stats& stats) {
  if (pool->kind() == MemoryPool::Kind::kLeaf) {
    return 0;
  }

  // Sort the child pools based on their reserved memory and reclaim from the
  // child pool with most reservation first.
  struct Candidate {
    std::shared_ptr<memory::MemoryPool> pool;
    int64_t reservedBytes;
  };
  std::vector<Candidate> candidates;
  {
    folly::SharedMutex::ReadHolder guard{pool->poolMutex_};
    candidates.reserve(pool->children_.size());
    for (auto& entry : pool->children_) {
      auto child = entry.second.lock();
      if (child != nullptr) {
        const int64_t reservedBytes = child->reservedBytes();
        candidates.push_back(Candidate{std::move(child), reservedBytes});
      }
    }
  }

  std::sort(
      candidates.begin(),
      candidates.end(),
      [](const auto& lhs, const auto& rhs) {
        return lhs.reservedBytes > rhs.reservedBytes;
      });

  uint64_t reclaimedBytes{0};
  for (const auto& candidate : candidates) {
    const auto bytes = candidate.pool->reclaim(targetBytes, stats);
    reclaimedBytes += bytes;
    if (targetBytes != 0) {
      if (bytes >= targetBytes) {
        break;
      }
      targetBytes -= bytes;
    }
  }
  return reclaimedBytes;
}

void MemoryReclaimer::abort(MemoryPool* pool, const std::exception_ptr& error) {
  if (pool->kind() == MemoryPool::Kind::kLeaf) {
    VELOX_UNSUPPORTED(
        "Don't support to abort a leaf memory pool {}", pool->name());
  }
  pool->visitChildren([&](MemoryPool* child) {
    // NOTE: we issue abort request through the child pool's reclaimer directly
    // instead of the child pool as the latter always forwards the abort to its
    // root first.
    auto* reclaimer = child->reclaimer();
    if (reclaimer != nullptr) {
      reclaimer->abort(child, error);
    }
    return true;
  });
}

void MemoryReclaimer::Stats::reset() {
  numNonReclaimableAttempts = 0;
  reclaimExecTimeUs = 0;
  reclaimedBytes = 0;
  reclaimWaitTimeUs = 0;
}

bool MemoryReclaimer::Stats::operator==(
    const MemoryReclaimer::Stats& other) const {
  return numNonReclaimableAttempts == other.numNonReclaimableAttempts &&
      reclaimExecTimeUs == other.reclaimExecTimeUs &&
      reclaimedBytes == other.reclaimedBytes &&
      reclaimWaitTimeUs == other.reclaimWaitTimeUs;
}

bool MemoryReclaimer::Stats::operator!=(
    const MemoryReclaimer::Stats& other) const {
  return !(*this == other);
}

MemoryArbitrator::Stats::Stats(
    uint64_t _numRequests,
    uint64_t _numSucceeded,
    uint64_t _numAborted,
    uint64_t _numFailures,
    uint64_t _queueTimeUs,
    uint64_t _arbitrationTimeUs,
    uint64_t _numShrunkBytes,
    uint64_t _numReclaimedBytes,
    uint64_t _maxCapacityBytes,
    uint64_t _freeCapacityBytes,
    uint64_t _reclaimTimeUs,
    uint64_t _numNonReclaimableAttempts,
    uint64_t _numReserveRequest,
    uint64_t _numReleaseRequest)
    : numRequests(_numRequests),
      numSucceeded(_numSucceeded),
      numAborted(_numAborted),
      numFailures(_numFailures),
      queueTimeUs(_queueTimeUs),
      arbitrationTimeUs(_arbitrationTimeUs),
      numShrunkBytes(_numShrunkBytes),
      numReclaimedBytes(_numReclaimedBytes),
      maxCapacityBytes(_maxCapacityBytes),
      freeCapacityBytes(_freeCapacityBytes),
      reclaimTimeUs(_reclaimTimeUs),
      numNonReclaimableAttempts(_numNonReclaimableAttempts),
      numReserveRequest(_numReserveRequest),
      numReleaseRequest(_numReleaseRequest) {}

std::string MemoryArbitrator::Stats::toString() const {
  return fmt::format(
      "STATS[numRequests {} numSucceeded {} numAborted {} numFailures {} "
      "numNonReclaimableAttempts {} numReserveRequest {} numReleaseRequest {} "
      "queueTime {} arbitrationTime {} reclaimTime {} shrunkMemory {} "
      "reclaimedMemory {} maxCapacity {} freeCapacity {}]",
      numRequests,
      numSucceeded,
      numAborted,
      numFailures,
      numNonReclaimableAttempts,
      numReserveRequest,
      numReleaseRequest,
      succinctMicros(queueTimeUs),
      succinctMicros(arbitrationTimeUs),
      succinctMicros(reclaimTimeUs),
      succinctBytes(numShrunkBytes),
      succinctBytes(numReclaimedBytes),
      succinctBytes(maxCapacityBytes),
      succinctBytes(freeCapacityBytes));
}

MemoryArbitrator::Stats MemoryArbitrator::Stats::operator-(
    const Stats& other) const {
  Stats result;
  result.numRequests = numRequests - other.numRequests;
  result.numSucceeded = numSucceeded - other.numSucceeded;
  result.numAborted = numAborted - other.numAborted;
  result.numFailures = numFailures - other.numFailures;
  result.queueTimeUs = queueTimeUs - other.queueTimeUs;
  result.arbitrationTimeUs = arbitrationTimeUs - other.arbitrationTimeUs;
  result.numShrunkBytes = numShrunkBytes - other.numShrunkBytes;
  result.numReclaimedBytes = numReclaimedBytes - other.numReclaimedBytes;
  result.maxCapacityBytes = maxCapacityBytes;
  result.freeCapacityBytes = freeCapacityBytes;
  result.reclaimTimeUs = reclaimTimeUs - other.reclaimTimeUs;
  result.numNonReclaimableAttempts =
      numNonReclaimableAttempts - other.numNonReclaimableAttempts;
  result.numReserveRequest = numReserveRequest - other.numReserveRequest;
  result.numReleaseRequest = numReleaseRequest - other.numReleaseRequest;
  return result;
}

bool MemoryArbitrator::Stats::operator==(const Stats& other) const {
  return std::tie(
             numRequests,
             numSucceeded,
             numAborted,
             numFailures,
             queueTimeUs,
             arbitrationTimeUs,
             numShrunkBytes,
             numReclaimedBytes,
             maxCapacityBytes,
             freeCapacityBytes,
             reclaimTimeUs,
             numNonReclaimableAttempts,
             numReserveRequest,
             numReleaseRequest) ==
      std::tie(
             other.numRequests,
             other.numSucceeded,
             other.numAborted,
             other.numFailures,
             other.queueTimeUs,
             other.arbitrationTimeUs,
             other.numShrunkBytes,
             other.numReclaimedBytes,
             other.maxCapacityBytes,
             other.freeCapacityBytes,
             other.reclaimTimeUs,
             other.numNonReclaimableAttempts,
             other.numReserveRequest,
             other.numReleaseRequest);
}

bool MemoryArbitrator::Stats::operator!=(const Stats& other) const {
  return !(*this == other);
}

bool MemoryArbitrator::Stats::operator<(const Stats& other) const {
  uint32_t eqCount{0};
  uint32_t gtCount{0};
  uint32_t ltCount{0};
#define UPDATE_COUNTER(counter)           \
  do {                                    \
    if (counter < other.counter) {        \
      ++ltCount;                          \
    } else if (counter > other.counter) { \
      ++gtCount;                          \
    } else {                              \
      ++eqCount;                          \
    }                                     \
  } while (0);

  UPDATE_COUNTER(numRequests);
  UPDATE_COUNTER(numSucceeded);
  UPDATE_COUNTER(numAborted);
  UPDATE_COUNTER(numFailures);
  UPDATE_COUNTER(queueTimeUs);
  UPDATE_COUNTER(arbitrationTimeUs);
  UPDATE_COUNTER(numShrunkBytes);
  UPDATE_COUNTER(numReclaimedBytes);
  UPDATE_COUNTER(reclaimTimeUs);
  UPDATE_COUNTER(numNonReclaimableAttempts);
  UPDATE_COUNTER(numReserveRequest);
  UPDATE_COUNTER(numReleaseRequest);
#undef UPDATE_COUNTER
  VELOX_CHECK(
      !((gtCount > 0) && (ltCount > 0)),
      "gtCount {} ltCount {}",
      gtCount,
      ltCount);
  return ltCount > 0;
}

bool MemoryArbitrator::Stats::operator>(const Stats& other) const {
  return !(*this < other) && (*this != other);
}

bool MemoryArbitrator::Stats::operator>=(const Stats& other) const {
  return !(*this < other);
}

bool MemoryArbitrator::Stats::operator<=(const Stats& other) const {
  return !(*this > other);
}

ScopedMemoryArbitrationContext::ScopedMemoryArbitrationContext(
    const MemoryPool& requestor)
    : savedArbitrationCtx_(arbitrationCtx),
      currentArbitrationCtx_({.requestor = requestor}) {
  arbitrationCtx = &currentArbitrationCtx_;
}

ScopedMemoryArbitrationContext::~ScopedMemoryArbitrationContext() {
  arbitrationCtx = savedArbitrationCtx_;
}

MemoryArbitrationContext* memoryArbitrationContext() {
  return arbitrationCtx;
}

bool underMemoryArbitration() {
  return memoryArbitrationContext() != nullptr;
}
} // namespace facebook::velox::memory
