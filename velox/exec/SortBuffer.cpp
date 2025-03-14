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

#include "SortBuffer.h"
#include "velox/exec/MemoryReclaimer.h"
#include "velox/vector/BaseVector.h"

namespace facebook::velox::exec {

SortBuffer::SortBuffer(
    const RowTypePtr& input,
    const std::vector<column_index_t>& sortColumnIndices,
    const std::vector<CompareFlags>& sortCompareFlags,
    uint32_t outputBatchSize,
    velox::memory::MemoryPool* pool,
    tsan_atomic<bool>* nonReclaimableSection,
    uint32_t* numSpillRuns,
    const common::SpillConfig* spillConfig,
    uint64_t spillMemoryThreshold)
    : input_(input),
      sortCompareFlags_(sortCompareFlags),
      outputBatchSize_(outputBatchSize),
      pool_(pool),
      nonReclaimableSection_(nonReclaimableSection),
      numSpillRuns_(numSpillRuns),
      spillConfig_(spillConfig),
      spillMemoryThreshold_(spillMemoryThreshold) {
  VELOX_CHECK_GE(input_->size(), sortCompareFlags_.size());
  VELOX_CHECK_GT(sortCompareFlags_.size(), 0);
  VELOX_CHECK_EQ(sortColumnIndices.size(), sortCompareFlags_.size());
  VELOX_CHECK_GT(outputBatchSize_, 0);
  VELOX_CHECK_NOT_NULL(nonReclaimableSection_);
  VELOX_CHECK_NOT_NULL(numSpillRuns_);

  std::vector<TypePtr> sortedColumnTypes;
  std::vector<TypePtr> nonSortedColumnTypes;
  std::vector<std::string> sortedSpillColumnNames;
  std::vector<TypePtr> sortedSpillColumnTypes;
  sortedColumnTypes.reserve(sortColumnIndices.size());
  nonSortedColumnTypes.reserve(input->size() - sortColumnIndices.size());
  sortedSpillColumnNames.reserve(input->size());
  sortedSpillColumnTypes.reserve(input->size());
  std::unordered_set<column_index_t> sortedChannelSet;
  // Sorted key columns.
  for (column_index_t i = 0; i < sortColumnIndices.size(); ++i) {
    columnMap_.emplace_back(IdentityProjection(i, sortColumnIndices.at(i)));
    sortedColumnTypes.emplace_back(input_->childAt(sortColumnIndices.at(i)));
    sortedSpillColumnTypes.emplace_back(
        input_->childAt(sortColumnIndices.at(i)));
    sortedSpillColumnNames.emplace_back(input->nameOf(sortColumnIndices.at(i)));
    sortedChannelSet.emplace(sortColumnIndices.at(i));
  }
  // Non-sorted key columns.
  for (column_index_t i = 0, nonSortedIndex = sortCompareFlags_.size();
       i < input_->size();
       ++i) {
    if (sortedChannelSet.count(i) != 0) {
      continue;
    }
    columnMap_.emplace_back(nonSortedIndex++, i);
    nonSortedColumnTypes.emplace_back(input_->childAt(i));
    sortedSpillColumnTypes.emplace_back(input_->childAt(i));
    sortedSpillColumnNames.emplace_back(input->nameOf(i));
  }

  data_ = std::make_unique<RowContainer>(
      sortedColumnTypes, nonSortedColumnTypes, pool_);
  spillerStoreType_ =
      ROW(std::move(sortedSpillColumnNames), std::move(sortedSpillColumnTypes));
}

void SortBuffer::addInput(const VectorPtr& input) {
  VELOX_CHECK(!noMoreInput_);
  ensureInputFits(input);

  SelectivityVector allRows(input->size());
  std::vector<char*> rows(input->size());
  for (int row = 0; row < input->size(); ++row) {
    rows[row] = data_->newRow();
  }
  auto* inputRow = input->as<RowVector>();
  for (const auto& columnProjection : columnMap_) {
    DecodedVector decoded(
        *inputRow->childAt(columnProjection.outputChannel), allRows);
    for (int i = 0; i < input->size(); ++i) {
      data_->store(decoded, i, rows[i], columnProjection.inputChannel);
    }
  }
  numInputRows_ += allRows.size();
}

void SortBuffer::noMoreInput() {
  VELOX_CHECK(!noMoreInput_);
  noMoreInput_ = true;

  // No data.
  if (numInputRows_ == 0) {
    return;
  }

  if (spiller_ == nullptr) {
    VELOX_CHECK_EQ(numInputRows_, data_->numRows());
    // Sort the pointers to the rows in RowContainer (data_) instead of sorting
    // the rows.
    sortedRows_.resize(numInputRows_);
    RowContainerIterator iter;
    data_->listRows(&iter, numInputRows_, sortedRows_.data());
    std::sort(
        sortedRows_.begin(),
        sortedRows_.end(),
        [this](const char* leftRow, const char* rightRow) {
          for (vector_size_t index = 0; index < sortCompareFlags_.size();
               ++index) {
            if (auto result = data_->compare(
                    leftRow, rightRow, index, sortCompareFlags_[index])) {
              return result < 0;
            }
          }
          return false;
        });
  } else {
    // Spill the remaining in-memory state to disk if spilling has been
    // triggered on this sort buffer. This is to simplify query OOM prevention
    // when producing output as we don't support to spill during that stage as
    // for now.
    spill();

    // Finish spill, and we shouldn't get any rows from non-spilled partition as
    // there is only one hash partition for SortBuffer.
    spiller_->finalizeSpill();
    VELOX_CHECK_LE(spiller_->stats().spilledPartitions, 1);

    VELOX_CHECK_NULL(spillMerger_);
    spillMerger_ = spiller_->startMerge();
    spillSources_.resize(outputBatchSize_);
    spillSourceRows_.resize(outputBatchSize_);
  }
}

RowVectorPtr SortBuffer::getOutput() {
  VELOX_CHECK(noMoreInput_);

  if (numOutputRows_ == numInputRows_) {
    return nullptr;
  }

  prepareOutput();
  if (spiller_ != nullptr) {
    getOutputWithSpill();
  } else {
    getOutputWithoutSpill();
  }
  return output_;
}

void SortBuffer::spill() {
  VELOX_CHECK_NOT_NULL(
      spillConfig_, "spill config is null when SortBuffer spill is called");

  // Check if sort buffer is empty or not, and skip spill if it is empty.
  if (data_->numRows() == 0) {
    return;
  }

  ++(*numSpillRuns_);
  if (spiller_ == nullptr) {
    spiller_ = std::make_unique<Spiller>(
        Spiller::Type::kOrderBy,
        data_.get(),
        spillerStoreType_,
        data_->keyTypes().size(),
        sortCompareFlags_,
        spillConfig_->filePath,
        spillConfig_->writeBufferSize,
        spillConfig_->compressionKind,
        memory::spillMemoryPool(),
        spillConfig_->executor);
    VELOX_CHECK_EQ(spiller_->state().maxPartitions(), 1);
  }

  spiller_->spill();
  data_->clear();
}

void SortBuffer::ensureInputFits(const VectorPtr& input) {
  // Check if spilling is enabled or not.
  if (spillConfig_ == nullptr) {
    return;
  }

  const int64_t numRows = data_->numRows();
  if (numRows == 0) {
    // 'data_' is empty. Nothing to spill.
    return;
  }

  auto [freeRows, outOfLineFreeBytes] = data_->freeSpace();
  const auto outOfLineBytes =
      data_->stringAllocator().retainedSize() - outOfLineFreeBytes;
  const int64_t outOfLineBytesPerRow = outOfLineBytes / numRows;
  const int64_t flatInputBytes = input->estimateFlatSize();

  // Test-only spill path.
  if (numRows > 0 && spillConfig_->testSpillPct &&
      (folly::hasher<uint64_t>()(++spillTestCounter_)) % 100 <=
          spillConfig_->testSpillPct) {
    spill();
    return;
  }

  // If current memory usage exceeds spilling threshold, trigger spilling.
  const auto currentMemoryUsage = pool_->currentBytes();
  if (spillMemoryThreshold_ != 0 &&
      currentMemoryUsage > spillMemoryThreshold_) {
    spill();
    return;
  }

  // If we have enough free rows for input rows and enough variable length
  // free space for the vector's flat size, no need for spilling.
  if (freeRows > input->size() &&
      (outOfLineBytes == 0 || outOfLineFreeBytes >= flatInputBytes)) {
    return;
  }

  // For variable length data, we take the flat size of the input as the cap.
  const int64_t estimatedIncrementalBytes =
      data_->sizeIncrement(input->size(), outOfLineBytes ? flatInputBytes : 0);

  // If the current available reservation in memory pool is 2X the
  // estimatedIncrementalBytes, no need to spill.
  if (pool_->availableReservation() > 2 * estimatedIncrementalBytes) {
    return;
  }

  // Try reserving targetIncrementBytes more in memory pool, if succeed, no
  // need to spill.
  const auto targetIncrementBytes = std::max<int64_t>(
      estimatedIncrementalBytes * 2,
      currentMemoryUsage * spillConfig_->spillableReservationGrowthPct / 100);
  {
    exec::ReclaimableSectionGuard guard(nonReclaimableSection_);
    if (pool_->maybeReserve(targetIncrementBytes)) {
      return;
    }
  }

  spill();
}

void SortBuffer::prepareOutput() {
  VELOX_CHECK_GT(numInputRows_, numOutputRows_);

  const vector_size_t batchSize =
      std::min<vector_size_t>(numInputRows_ - numOutputRows_, outputBatchSize_);
  if (output_ != nullptr) {
    VectorPtr output = std::move(output_);
    BaseVector::prepareForReuse(output, batchSize);
    output_ = std::static_pointer_cast<RowVector>(output);
  } else {
    output_ = std::static_pointer_cast<RowVector>(
        BaseVector::create(input_, batchSize, pool_));
  }

  for (auto& child : output_->children()) {
    child->resize(batchSize);
  }
}

void SortBuffer::getOutputWithoutSpill() {
  VELOX_CHECK_GT(output_->size(), 0);
  VELOX_DCHECK_LE(output_->size(), outputBatchSize_);
  VELOX_CHECK_LE(output_->size() + numOutputRows_, numInputRows_);
  VELOX_DCHECK_EQ(numInputRows_, sortedRows_.size());

  for (const auto& columnProjection : columnMap_) {
    data_->extractColumn(
        sortedRows_.data() + numOutputRows_,
        output_->size(),
        columnProjection.inputChannel,
        output_->childAt(columnProjection.outputChannel));
  }
  numOutputRows_ += output_->size();
}

void SortBuffer::getOutputWithSpill() {
  VELOX_CHECK_NOT_NULL(spillMerger_);
  VELOX_DCHECK_EQ(sortedRows_.size(), 0);
  VELOX_DCHECK_EQ(spillSources_.size(), outputBatchSize_);
  VELOX_DCHECK_EQ(spillSourceRows_.size(), outputBatchSize_);

  int32_t outputRow = 0;
  int32_t outputSize = 0;
  bool isEndOfBatch = false;
  while (outputRow + outputSize < output_->size()) {
    SpillMergeStream* stream = spillMerger_->next();
    VELOX_CHECK_NOT_NULL(stream);

    spillSources_[outputSize] = &stream->current();
    spillSourceRows_[outputSize] = stream->currentIndex(&isEndOfBatch);
    ++outputSize;
    if (FOLLY_UNLIKELY(isEndOfBatch)) {
      // The stream is at end of input batch. Need to copy out the rows before
      // fetching next batch in 'pop'.
      gatherCopy(
          output_.get(),
          outputRow,
          outputSize,
          spillSources_,
          spillSourceRows_,
          columnMap_);
      outputRow += outputSize;
      outputSize = 0;
    }
    // Advance the stream.
    stream->pop();
  }
  VELOX_CHECK_EQ(outputRow + outputSize, output_->size());

  if (FOLLY_LIKELY(outputSize != 0)) {
    gatherCopy(
        output_.get(),
        outputRow,
        outputSize,
        spillSources_,
        spillSourceRows_,
        columnMap_);
  }

  numOutputRows_ += output_->size();
}

} // namespace facebook::velox::exec
