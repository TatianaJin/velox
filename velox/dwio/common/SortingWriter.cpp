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

#include "velox/dwio/common/SortingWriter.h"

namespace facebook::velox::dwio::common {

SortingWriter::SortingWriter(
    std::unique_ptr<Writer> writer,
    std::unique_ptr<exec::SortBuffer> sortBuffer)
    : outputWriter_(std::move(writer)),
      sortPool_(sortBuffer->pool()),
      canReclaim_(sortBuffer->canSpill()),
      sortBuffer_(std::move(sortBuffer)) {
  if (sortPool_->parent()->reclaimer() != nullptr) {
    sortPool_->setReclaimer(MemoryReclaimer::create(this));
  }
  setState(State::kRunning);
}

void SortingWriter::write(const VectorPtr& data) {
  checkRunning();
  sortBuffer_->addInput(data);
}

void SortingWriter::flush() {
  checkRunning();
  outputWriter_->flush();
}

void SortingWriter::close() {
  setState(State::kClosed);

  sortBuffer_->noMoreInput();
  RowVectorPtr output = sortBuffer_->getOutput();
  while (output != nullptr) {
    outputWriter_->write(output);
    output = sortBuffer_->getOutput();
  }
  sortBuffer_.reset();
  sortPool_->release();
  outputWriter_->close();
}

void SortingWriter::abort() {
  setState(State::kAborted);

  sortBuffer_.reset();
  sortPool_->release();
  outputWriter_->abort();
}

bool SortingWriter::canReclaim() const {
  return canReclaim_;
}

uint64_t SortingWriter::reclaim(
    uint64_t targetBytes,
    memory::MemoryReclaimer::Stats& stats) {
  if (!canReclaim_) {
    return 0;
  }

  if (!isRunning()) {
    LOG(WARNING) << "Can't reclaim from a not running hive sort writer pool: "
                 << sortPool_->name() << ", state: " << state()
                 << "used memory: " << succinctBytes(sortPool_->currentBytes())
                 << ", reserved memory: "
                 << succinctBytes(sortPool_->reservedBytes());
    ++stats.numNonReclaimableAttempts;
    return 0;
  }
  VELOX_CHECK_NOT_NULL(sortBuffer_);

  auto reclaimBytes = memory::MemoryReclaimer::run(
      [&]() {
        sortBuffer_->spill();
        sortPool_->release();
        return sortPool_->shrink(targetBytes);
      },
      stats);

  return reclaimBytes;
}

std::unique_ptr<memory::MemoryReclaimer> SortingWriter::MemoryReclaimer::create(
    SortingWriter* writer) {
  return std::unique_ptr<memory::MemoryReclaimer>(new MemoryReclaimer(writer));
}

bool SortingWriter::MemoryReclaimer::reclaimableBytes(
    const memory::MemoryPool& pool,
    uint64_t& reclaimableBytes) const {
  VELOX_CHECK_EQ(pool.name(), writer_->sortPool_->name());

  reclaimableBytes = 0;
  if (!writer_->canReclaim()) {
    return false;
  }
  reclaimableBytes = pool.currentBytes();
  return true;
}

uint64_t SortingWriter::MemoryReclaimer::reclaim(
    memory::MemoryPool* pool,
    uint64_t targetBytes,
    memory::MemoryReclaimer::Stats& stats) {
  VELOX_CHECK_EQ(pool->name(), writer_->sortPool_->name());

  return writer_->reclaim(targetBytes, stats);
}
} // namespace facebook::velox::dwio::common
