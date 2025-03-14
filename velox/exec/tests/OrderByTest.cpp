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
#include <re2/re2.h>

#include "folly/experimental/EventCount.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/file/FileSystems.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/core/QueryConfig.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/Spiller.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/QueryAssertions.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;
using namespace facebook::velox::common::testutil;
using namespace facebook::velox::core;
using namespace facebook::velox::exec::test;

namespace {
// Returns aggregated spilled stats by 'task'.
SpillStats spilledStats(const exec::Task& task) {
  SpillStats spilledStats;
  auto stats = task.taskStats();
  for (auto& pipeline : stats.pipelineStats) {
    for (auto op : pipeline.operatorStats) {
      spilledStats.spilledInputBytes += op.spilledInputBytes;
      spilledStats.spilledBytes += op.spilledBytes;
      spilledStats.spilledRows += op.spilledRows;
      spilledStats.spilledPartitions += op.spilledPartitions;
      spilledStats.spilledFiles += op.spilledFiles;
    }
  }
  return spilledStats;
}

void abortPool(memory::MemoryPool* pool) {
  try {
    VELOX_FAIL("Manual MemoryPool Abortion");
  } catch (const VeloxException& error) {
    pool->abort(std::current_exception());
  }
}
} // namespace

class OrderByTest : public OperatorTestBase {
 protected:
  void SetUp() override {
    filesystems::registerLocalFileSystem();
    if (!isRegisteredVectorSerde()) {
      this->registerVectorSerde();
    }
    rng_.seed(123);
  }

  void testSingleKey(
      const std::vector<RowVectorPtr>& input,
      const std::string& key) {
    core::PlanNodeId orderById;
    auto keyIndex = input[0]->type()->asRow().getChildIdx(key);
    auto plan = PlanBuilder()
                    .values(input)
                    .orderBy({fmt::format("{} ASC NULLS LAST", key)}, false)
                    .capturePlanNodeId(orderById)
                    .planNode();
    runTest(
        plan,
        orderById,
        fmt::format("SELECT * FROM tmp ORDER BY {} NULLS LAST", key),
        {keyIndex});

    plan = PlanBuilder()
               .values(input)
               .orderBy({fmt::format("{} DESC NULLS FIRST", key)}, false)
               .planNode();
    runTest(
        plan,
        orderById,
        fmt::format("SELECT * FROM tmp ORDER BY {} DESC NULLS FIRST", key),
        {keyIndex});
  }

  void testSingleKey(
      const std::vector<RowVectorPtr>& input,
      const std::string& key,
      const std::string& filter) {
    core::PlanNodeId orderById;
    auto keyIndex = input[0]->type()->asRow().getChildIdx(key);
    auto plan = PlanBuilder()
                    .values(input)
                    .filter(filter)
                    .orderBy({fmt::format("{} ASC NULLS LAST", key)}, false)
                    .capturePlanNodeId(orderById)
                    .planNode();
    runTest(
        plan,
        orderById,
        fmt::format(
            "SELECT * FROM tmp WHERE {} ORDER BY {} NULLS LAST", filter, key),
        {keyIndex});

    plan = PlanBuilder()
               .values(input)
               .filter(filter)
               .orderBy({fmt::format("{} DESC NULLS FIRST", key)}, false)
               .capturePlanNodeId(orderById)
               .planNode();
    runTest(
        plan,
        orderById,
        fmt::format(
            "SELECT * FROM tmp WHERE {} ORDER BY {} DESC NULLS FIRST",
            filter,
            key),
        {keyIndex});
  }

  void testTwoKeys(
      const std::vector<RowVectorPtr>& input,
      const std::string& key1,
      const std::string& key2) {
    auto rowType = input[0]->type()->asRow();
    auto keyIndices = {rowType.getChildIdx(key1), rowType.getChildIdx(key2)};

    std::vector<core::SortOrder> sortOrders = {
        core::kAscNullsLast, core::kDescNullsFirst};
    std::vector<std::string> sortOrderSqls = {"NULLS LAST", "DESC NULLS FIRST"};

    for (int i = 0; i < sortOrders.size(); i++) {
      for (int j = 0; j < sortOrders.size(); j++) {
        core::PlanNodeId orderById;
        auto plan = PlanBuilder()
                        .values(input)
                        .orderBy(
                            {fmt::format("{} {}", key1, sortOrderSqls[i]),
                             fmt::format("{} {}", key2, sortOrderSqls[j])},
                            false)
                        .capturePlanNodeId(orderById)
                        .planNode();
        runTest(
            plan,
            orderById,
            fmt::format(
                "SELECT * FROM tmp ORDER BY {} {}, {} {}",
                key1,
                sortOrderSqls[i],
                key2,
                sortOrderSqls[j]),
            keyIndices);
      }
    }
  }

  void runTest(
      core::PlanNodePtr planNode,
      const core::PlanNodeId& orderById,
      const std::string& duckDbSql,
      const std::vector<uint32_t>& sortingKeys) {
    {
      SCOPED_TRACE("run without spilling");
      assertQueryOrdered(planNode, duckDbSql, sortingKeys);
    }
    {
      SCOPED_TRACE("run with spilling");
      auto spillDirectory = exec::test::TempDirectoryPath::create();
      auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
      queryCtx->testingOverrideConfigUnsafe({
          {core::QueryConfig::kTestingSpillPct, "100"},
          {core::QueryConfig::kSpillEnabled, "true"},
          {core::QueryConfig::kOrderBySpillEnabled, "true"},
      });
      CursorParameters params;
      params.planNode = planNode;
      params.queryCtx = queryCtx;
      params.spillDirectory = spillDirectory->path;
      auto task = assertQueryOrdered(params, duckDbSql, sortingKeys);
      auto inputRows = toPlanStats(task->taskStats()).at(orderById).inputRows;
      const uint64_t peakSpillMemoryUsage =
          memory::spillMemoryPool()->stats().peakBytes;
      ASSERT_EQ(memory::spillMemoryPool()->stats().currentBytes, 0);
      if (inputRows > 0) {
        EXPECT_LT(0, spilledStats(*task).spilledInputBytes);
        EXPECT_LT(0, spilledStats(*task).spilledBytes);
        EXPECT_EQ(1, spilledStats(*task).spilledPartitions);
        EXPECT_LT(0, spilledStats(*task).spilledFiles);
        EXPECT_EQ(inputRows, spilledStats(*task).spilledRows);
        ASSERT_EQ(memory::spillMemoryPool()->stats().currentBytes, 0);
        if (memory::spillMemoryPool()->trackUsage()) {
          ASSERT_GT(memory::spillMemoryPool()->stats().peakBytes, 0);
          ASSERT_GE(
              memory::spillMemoryPool()->stats().peakBytes,
              peakSpillMemoryUsage);
        }
      } else {
        EXPECT_EQ(0, spilledStats(*task).spilledInputBytes);
        EXPECT_EQ(0, spilledStats(*task).spilledBytes);
      }
      OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
    }
  }

  static void reclaimAndRestoreCapacity(
      const Operator* op,
      uint64_t targetBytes,
      memory::MemoryReclaimer::Stats& reclaimerStats) {
    const auto oldCapacity = op->pool()->capacity();
    op->pool()->reclaim(targetBytes, reclaimerStats);
    dynamic_cast<memory::MemoryPoolImpl*>(op->pool())
        ->testingSetCapacity(oldCapacity);
  }

  folly::Random::DefaultGenerator rng_;
  memory::MemoryReclaimer::Stats reclaimerStats_;
};

TEST_F(OrderByTest, selectiveFilter) {
  vector_size_t batchSize = 1000;
  std::vector<RowVectorPtr> vectors;
  for (int32_t i = 0; i < 3; ++i) {
    auto c0 = makeFlatVector<int64_t>(
        batchSize,
        [&](vector_size_t row) { return batchSize * i + row; },
        nullEvery(5));
    auto c1 = makeFlatVector<int64_t>(
        batchSize, [&](vector_size_t row) { return row; }, nullEvery(5));
    auto c2 = makeFlatVector<double>(
        batchSize, [](vector_size_t row) { return row * 0.1; }, nullEvery(11));
    vectors.push_back(makeRowVector({c0, c1, c2}));
  }
  createDuckDbTable(vectors);

  // c0 values are unique across batches
  testSingleKey(vectors, "c0", "c0 % 333 = 0");

  // c1 values are unique only within a batch
  testSingleKey(vectors, "c1", "c1 % 333 = 0");
}

TEST_F(OrderByTest, singleKey) {
  vector_size_t batchSize = 1000;
  std::vector<RowVectorPtr> vectors;
  for (int32_t i = 0; i < 2; ++i) {
    auto c0 = makeFlatVector<int64_t>(
        batchSize, [&](vector_size_t row) { return row; }, nullEvery(5));
    auto c1 = makeFlatVector<double>(
        batchSize, [](vector_size_t row) { return row * 0.1; }, nullEvery(11));
    vectors.push_back(makeRowVector({c0, c1}));
  }
  createDuckDbTable(vectors);

  testSingleKey(vectors, "c0");

  // parser doesn't support "is not null" expression, hence, using c0 % 2 >= 0
  testSingleKey(vectors, "c0", "c0 % 2 >= 0");

  core::PlanNodeId orderById;
  auto plan = PlanBuilder()
                  .values(vectors)
                  .orderBy({"c0 DESC NULLS LAST"}, false)
                  .capturePlanNodeId(orderById)
                  .planNode();
  runTest(
      plan, orderById, "SELECT * FROM tmp ORDER BY c0 DESC NULLS LAST", {0});

  plan = PlanBuilder()
             .values(vectors)
             .orderBy({"c0 ASC NULLS FIRST"}, false)
             .capturePlanNodeId(orderById)
             .planNode();
  runTest(plan, orderById, "SELECT * FROM tmp ORDER BY c0 NULLS FIRST", {0});
}

TEST_F(OrderByTest, multipleKeys) {
  vector_size_t batchSize = 1000;
  std::vector<RowVectorPtr> vectors;
  for (int32_t i = 0; i < 2; ++i) {
    // c0: half of rows are null, a quarter is 0 and remaining quarter is 1
    auto c0 = makeFlatVector<int64_t>(
        batchSize, [](vector_size_t row) { return row % 4; }, nullEvery(2, 1));
    auto c1 = makeFlatVector<int32_t>(
        batchSize, [](vector_size_t row) { return row; }, nullEvery(7));
    auto c2 = makeFlatVector<double>(
        batchSize, [](vector_size_t row) { return row * 0.1; }, nullEvery(11));
    vectors.push_back(makeRowVector({c0, c1, c2}));
  }
  createDuckDbTable(vectors);

  testTwoKeys(vectors, "c0", "c1");

  core::PlanNodeId orderById;
  auto plan = PlanBuilder()
                  .values(vectors)
                  .orderBy({"c0 ASC NULLS FIRST", "c1 ASC NULLS LAST"}, false)
                  .capturePlanNodeId(orderById)
                  .planNode();
  runTest(
      plan,
      orderById,
      "SELECT * FROM tmp ORDER BY c0 NULLS FIRST, c1 NULLS LAST",
      {0, 1});

  plan = PlanBuilder()
             .values(vectors)
             .orderBy({"c0 DESC NULLS LAST", "c1 DESC NULLS FIRST"}, false)
             .capturePlanNodeId(orderById)
             .planNode();
  runTest(
      plan,
      orderById,
      "SELECT * FROM tmp ORDER BY c0 DESC NULLS LAST, c1 DESC NULLS FIRST",
      {0, 1});
}

TEST_F(OrderByTest, multiBatchResult) {
  vector_size_t batchSize = 5000;
  std::vector<RowVectorPtr> vectors;
  for (int32_t i = 0; i < 10; ++i) {
    auto c0 = makeFlatVector<int64_t>(
        batchSize,
        [&](vector_size_t row) { return batchSize * i + row; },
        nullEvery(5));
    auto c1 = makeFlatVector<double>(
        batchSize, [](vector_size_t row) { return row * 0.1; }, nullEvery(11));
    vectors.push_back(makeRowVector({c0, c1, c1, c1, c1, c1}));
  }
  createDuckDbTable(vectors);

  testSingleKey(vectors, "c0");
}

TEST_F(OrderByTest, varfields) {
  vector_size_t batchSize = 1000;
  std::vector<RowVectorPtr> vectors;
  for (int32_t i = 0; i < 5; ++i) {
    auto c0 = makeFlatVector<int64_t>(
        batchSize,
        [&](vector_size_t row) { return batchSize * i + row; },
        nullEvery(5));
    auto c1 = makeFlatVector<double>(
        batchSize, [](vector_size_t row) { return row * 0.1; }, nullEvery(11));
    auto c2 = makeFlatVector<StringView>(
        batchSize,
        [](vector_size_t row) {
          return StringView::makeInline(std::to_string(row));
        },
        nullEvery(17));
    // TODO: Add support for array/map in createDuckDbTable and verify
    // that we can sort by array/map as well.
    vectors.push_back(makeRowVector({c0, c1, c2}));
  }
  createDuckDbTable(vectors);

  testSingleKey(vectors, "c2");
}

TEST_F(OrderByTest, unknown) {
  vector_size_t size = 1'000;
  auto vector = makeRowVector({
      makeFlatVector<int64_t>(size, [](auto row) { return row % 7; }),
      BaseVector::createNullConstant(UNKNOWN(), size, pool()),
  });

  // Exclude "UNKNOWN" column as DuckDB doesn't understand UNKNOWN type
  createDuckDbTable(
      {makeRowVector({vector->childAt(0)}),
       makeRowVector({vector->childAt(0)})});

  core::PlanNodeId orderById;
  auto plan = PlanBuilder()
                  .values({vector, vector})
                  .orderBy({"c0 DESC NULLS LAST"}, false)
                  .capturePlanNodeId(orderById)
                  .planNode();
  runTest(
      plan,
      orderById,
      "SELECT *, null FROM tmp ORDER BY c0 DESC NULLS LAST",
      {0});
}

/// Verifies output batch rows of OrderBy
TEST_F(OrderByTest, outputBatchRows) {
  struct {
    int numRowsPerBatch;
    int preferredOutBatchBytes;
    int expectedOutputVectors;

    std::string debugString() const {
      return fmt::format(
          "numRowsPerBatch:{}, preferredOutBatchSize:{}, expectedOutputVectors:{}",
          numRowsPerBatch,
          preferredOutBatchBytes,
          expectedOutputVectors);
    }
    // Output kPreferredOutputBatchRows by default and thus include all rows in
    // a single vector.
    // TODO(gaoge): change after determining output batch rows adaptively.
  } testSettings[] = {{1024, 1, 1}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    const vector_size_t batchSize = testData.numRowsPerBatch;
    std::vector<RowVectorPtr> rowVectors;
    auto c0 = makeFlatVector<int64_t>(
        batchSize, [&](vector_size_t row) { return row; }, nullEvery(5));
    auto c1 = makeFlatVector<double>(
        batchSize, [&](vector_size_t row) { return row; }, nullEvery(11));
    std::vector<VectorPtr> vectors;
    vectors.push_back(c0);
    for (int i = 0; i < 256; ++i) {
      vectors.push_back(c1);
    }
    rowVectors.push_back(makeRowVector(vectors));
    createDuckDbTable(rowVectors);

    core::PlanNodeId orderById;
    auto plan = PlanBuilder()
                    .values(rowVectors)
                    .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                    .capturePlanNodeId(orderById)
                    .planNode();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideConfigUnsafe(
        {{core::QueryConfig::kPreferredOutputBatchBytes,
          std::to_string(testData.preferredOutBatchBytes)}});
    CursorParameters params;
    params.planNode = plan;
    params.queryCtx = queryCtx;
    auto task = assertQueryOrdered(
        params, "SELECT * FROM tmp ORDER BY c0 ASC NULLS LAST", {0});
    EXPECT_EQ(
        testData.expectedOutputVectors,
        toPlanStats(task->taskStats()).at(orderById).outputVectors);
  }
}

TEST_F(OrderByTest, spill) {
  const int kNumBatches = 3;
  const int kNumRows = 100'000;
  std::vector<RowVectorPtr> batches;
  for (int i = 0; i < kNumBatches; ++i) {
    batches.push_back(makeRowVector(
        {makeFlatVector<int64_t>(kNumRows, [](auto row) { return row * 3; }),
         makeFlatVector<StringView>(kNumRows, [](auto row) {
           return StringView::makeInline(std::to_string(row * 3));
         })}));
  }
  createDuckDbTable(batches);

  auto plan = PlanBuilder()
                  .values(batches)
                  .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                  .planNode();
  auto spillDirectory = exec::test::TempDirectoryPath::create();
  auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
  constexpr int64_t kMaxBytes = 20LL << 20; // 20 MB
  queryCtx->testingOverrideMemoryPool(
      memory::defaultMemoryManager().addRootPool(
          queryCtx->queryId(), kMaxBytes));
  // Set 'kSpillableReservationGrowthPct' to an extreme large value to trigger
  // disk spilling by failed memory growth reservation.
  queryCtx->testingOverrideConfigUnsafe({
      {core::QueryConfig::kSpillEnabled, "true"},
      {core::QueryConfig::kOrderBySpillEnabled, "true"},
      {core::QueryConfig::kSpillableReservationGrowthPct, "1000"},
  });
  CursorParameters params;
  params.planNode = plan;
  params.queryCtx = queryCtx;
  params.spillDirectory = spillDirectory->path;
  auto task = assertQueryOrdered(
      params, "SELECT * FROM tmp ORDER BY c0 ASC NULLS LAST", {0});
  auto stats = task->taskStats().pipelineStats[0].operatorStats[1];
  ASSERT_GT(stats.spilledRows, 0);
  ASSERT_EQ(stats.spilledRows, kNumBatches * kNumRows);
  ASSERT_GT(stats.spilledBytes, 0);
  ASSERT_GT(stats.spilledInputBytes, 0);
  ASSERT_EQ(stats.spilledPartitions, 1);
  ASSERT_EQ(stats.spilledFiles, 3);
  ASSERT_GT(stats.runtimeStats["spillRuns"].count, 0);
  ASSERT_GT(stats.runtimeStats["spillFillTime"].sum, 0);
  ASSERT_GT(stats.runtimeStats["spillSortTime"].sum, 0);
  ASSERT_GT(stats.runtimeStats["spillSerializationTime"].sum, 0);
  ASSERT_GT(stats.runtimeStats["spillFlushTime"].sum, 0);
  ASSERT_EQ(
      stats.runtimeStats["spillSerializationTime"].count,
      stats.runtimeStats["spillFlushTime"].count);
  ASSERT_GT(stats.runtimeStats["spillDiskWrites"].sum, 0);
  ASSERT_GT(stats.runtimeStats["spillWriteTime"].sum, 0);
  ASSERT_EQ(
      stats.runtimeStats["spillDiskWrites"].count,
      stats.runtimeStats["spillWriteTime"].count);

  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
}

TEST_F(OrderByTest, spillWithMemoryLimit) {
  constexpr int32_t kNumRows = 2000;
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({}, pool());
  const int32_t numBatches = 5;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }
  struct {
    uint64_t orderByMemLimit;
    bool expectSpill;

    std::string debugString() const {
      return fmt::format(
          "orderByMemLimit:{}, expectSpill:{}", orderByMemLimit, expectSpill);
    }
  } testSettings[] = {// Memory limit is disabled so spilling is not triggered.
                      {0, false},
                      // Memory limit is too small so always trigger spilling.
                      {1, true},
                      // Memory limit is too large so spilling is not triggered.
                      {1'000'000'000, false}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto results =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());
    auto task =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kOrderBySpillEnabled, "true")
            .config(
                QueryConfig::kOrderBySpillMemoryThreshold,
                std::to_string(testData.orderByMemLimit))
            .assertResults(results);

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(
        testData.expectSpill, stats[0].operatorStats[1].spilledInputBytes > 0);
    ASSERT_EQ(testData.expectSpill, stats[0].operatorStats[1].spilledBytes > 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
}

DEBUG_ONLY_TEST_F(OrderByTest, reclaimDuringInputProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  struct {
    // 0: trigger reclaim with some input processed.
    // 1: trigger reclaim after all the inputs processed.
    int triggerCondition;
    bool spillEnabled;
    bool expectedReclaimable;

    std::string debugString() const {
      return fmt::format(
          "triggerCondition {}, spillEnabled {}, expectedReclaimable {}",
          triggerCondition,
          spillEnabled,
          expectedReclaimable);
    }
  } testSettings[] = {
      {0, true, true}, {1, true, true}, {0, false, false}, {1, false, false}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<int> numInputs{0};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "OrderBy") {
            ASSERT_FALSE(testOp->canReclaim());
            return;
          }
          op = testOp;
          ++numInputs;
          if (testData.triggerCondition == 0) {
            if (numInputs != 2) {
              return;
            }
          }
          if (testData.triggerCondition == 1) {
            if (numInputs != numBatches) {
              return;
            }
          }
          ASSERT_EQ(op->canReclaim(), testData.expectedReclaimable);
          uint64_t reclaimableBytes{0};
          const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
          ASSERT_EQ(reclaimable, testData.expectedReclaimable);
          if (testData.expectedReclaimable) {
            ASSERT_GT(reclaimableBytes, 0);
          } else {
            ASSERT_EQ(reclaimableBytes, 0);
          }
          testWait.notify();
          driverWait.wait(driverWaitKey);
        })));

    std::thread taskThread([&]() {
      if (testData.spillEnabled) {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kOrderBySpillEnabled, "true")
            .maxDrivers(1)
            .assertResults(expectedResult);
      } else {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    driverWait.notify();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), testData.expectedReclaimable);
    ASSERT_EQ(reclaimable, testData.expectedReclaimable);
    if (testData.expectedReclaimable) {
      ASSERT_GT(reclaimableBytes, 0);
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
    }

    if (testData.expectedReclaimable) {
      reclaimAndRestoreCapacity(
          op,
          folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_),
          reclaimerStats_);
      ASSERT_GT(reclaimerStats_.reclaimedBytes, 0);
      ASSERT_GT(reclaimerStats_.reclaimExecTimeUs, 0);
      reclaimerStats_.reset();
      ASSERT_EQ(op->pool()->currentBytes(), 0);
    } else {
      VELOX_ASSERT_THROW(
          op->reclaim(
              folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_),
              reclaimerStats_),
          "");
    }

    Task::resume(task);

    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    if (testData.expectedReclaimable) {
      ASSERT_GT(stats[0].operatorStats[1].spilledBytes, 0);
      ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 1);
    } else {
      ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
      ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    }
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
  ASSERT_EQ(reclaimerStats_, memory::MemoryReclaimer::Stats{});
}

DEBUG_ONLY_TEST_F(OrderByTest, reclaimDuringReserve) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    const size_t size = i == 0 ? 100 : 40000;
    VectorFuzzer fuzzer({.vectorSize = size}, pool());
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  auto tempDirectory = exec::test::TempDirectoryPath::create();
  auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
  queryCtx->testingOverrideMemoryPool(
      memory::defaultMemoryManager().addRootPool(
          queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
  auto expectedResult =
      AssertQueryBuilder(
          PlanBuilder()
              .values(batches)
              .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
              .planNode())
          .queryCtx(queryCtx)
          .copyResults(pool_.get());

  folly::EventCount driverWait;
  auto driverWaitKey = driverWait.prepareWait();
  folly::EventCount testWait;
  auto testWaitKey = testWait.prepareWait();

  Operator* op;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::addInput",
      std::function<void(Operator*)>(([&](Operator* testOp) {
        if (testOp->operatorType() != "OrderBy") {
          ASSERT_FALSE(testOp->canReclaim());
          return;
        }
        op = testOp;
      })));

  std::atomic<bool> injectOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::common::memory::MemoryPoolImpl::maybeReserve",
      std::function<void(memory::MemoryPoolImpl*)>(
          ([&](memory::MemoryPoolImpl* pool) {
            ASSERT_TRUE(op != nullptr);
            const std::string re(".*OrderBy");
            if (!RE2::FullMatch(pool->name(), re)) {
              return;
            }
            if (!injectOnce.exchange(false)) {
              return;
            }
            ASSERT_TRUE(op->canReclaim());
            uint64_t reclaimableBytes{0};
            const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
            ASSERT_TRUE(reclaimable);
            ASSERT_GT(reclaimableBytes, 0);
            auto* driver = op->testingOperatorCtx()->driver();
            SuspendedSection suspendedSection(driver);
            testWait.notify();
            driverWait.wait(driverWaitKey);
          })));

  std::thread taskThread([&]() {
    AssertQueryBuilder(
        PlanBuilder()
            .values(batches)
            .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
            .planNode())
        .queryCtx(queryCtx)
        .spillDirectory(tempDirectory->path)
        .config(core::QueryConfig::kSpillEnabled, "true")
        .config(core::QueryConfig::kOrderBySpillEnabled, "true")
        .maxDrivers(1)
        .assertResults(expectedResult);
  });

  testWait.wait(testWaitKey);
  ASSERT_TRUE(op != nullptr);
  auto task = op->testingOperatorCtx()->task();
  auto taskPauseWait = task->requestPause();
  taskPauseWait.wait();

  uint64_t reclaimableBytes{0};
  const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
  ASSERT_TRUE(op->canReclaim());
  ASSERT_TRUE(reclaimable);
  ASSERT_GT(reclaimableBytes, 0);

  reclaimAndRestoreCapacity(
      op,
      folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_),
      reclaimerStats_);
  ASSERT_GT(reclaimerStats_.reclaimedBytes, 0);
  ASSERT_GT(reclaimerStats_.reclaimExecTimeUs, 0);
  reclaimerStats_.reset();
  ASSERT_EQ(op->pool()->currentBytes(), 0);

  driverWait.notify();
  Task::resume(task);

  taskThread.join();

  auto stats = task->taskStats().pipelineStats;
  ASSERT_GT(stats[0].operatorStats[1].spilledBytes, 0);
  ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 1);
  OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  ASSERT_EQ(reclaimerStats_, memory::MemoryReclaimer::Stats{});
}

DEBUG_ONLY_TEST_F(OrderByTest, reclaimDuringAllocation) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  std::vector<bool> enableSpillings = {false, true};
  for (const auto enableSpilling : enableSpillings) {
    SCOPED_TRACE(fmt::format("enableSpilling {}", enableSpilling));
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "OrderBy") {
            ASSERT_FALSE(testOp->canReclaim());
            return;
          }
          op = testOp;
        })));

    std::atomic<bool> injectOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::common::memory::MemoryPoolImpl::allocateNonContiguous",
        std::function<void(memory::MemoryPoolImpl*)>(
            ([&](memory::MemoryPoolImpl* pool) {
              ASSERT_TRUE(op != nullptr);
              const std::string re(".*OrderBy");
              if (!RE2::FullMatch(pool->name(), re)) {
                return;
              }
              if (!injectOnce.exchange(false)) {
                return;
              }
              ASSERT_EQ(op->canReclaim(), enableSpilling);
              uint64_t reclaimableBytes{0};
              const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
              ASSERT_EQ(reclaimable, enableSpilling);
              if (enableSpilling) {
                ASSERT_GE(reclaimableBytes, 0);
              } else {
                ASSERT_EQ(reclaimableBytes, 0);
              }
              auto* driver = op->testingOperatorCtx()->driver();
              SuspendedSection suspendedSection(driver);
              testWait.notify();
              driverWait.wait(driverWaitKey);
            })));

    std::thread taskThread([&]() {
      if (enableSpilling) {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kOrderBySpillEnabled, "true")
            .maxDrivers(1)
            .assertResults(expectedResult);
      } else {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), enableSpilling);
    ASSERT_EQ(reclaimable, enableSpilling);
    if (enableSpilling) {
      ASSERT_GE(reclaimableBytes, 0);
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
    }

    VELOX_ASSERT_THROW(
        op->reclaim(
            folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_),
            reclaimerStats_),
        "");

    driverWait.notify();
    Task::resume(task);

    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
    ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
  ASSERT_EQ(reclaimerStats_, memory::MemoryReclaimer::Stats{0});
}

DEBUG_ONLY_TEST_F(OrderByTest, reclaimDuringOutputProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  std::vector<bool> enableSpillings = {false, true};
  for (const auto enableSpilling : enableSpillings) {
    SCOPED_TRACE(fmt::format("enableSpilling {}", enableSpilling));
    auto tempDirectory = exec::test::TempDirectoryPath::create();
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<bool> injectOnce{true};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::noMoreInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "OrderBy") {
            ASSERT_FALSE(testOp->canReclaim());
            return;
          }
          op = testOp;
          if (!injectOnce.exchange(false)) {
            return;
          }
          ASSERT_EQ(op->canReclaim(), enableSpilling);
          uint64_t reclaimableBytes{0};
          const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
          ASSERT_EQ(reclaimable, enableSpilling);
          if (enableSpilling) {
            ASSERT_GT(reclaimableBytes, 0);
          } else {
            ASSERT_EQ(reclaimableBytes, 0);
          }
          testWait.notify();
          driverWait.wait(driverWaitKey);
        })));

    std::thread taskThread([&]() {
      if (enableSpilling) {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .spillDirectory(tempDirectory->path)
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kOrderBySpillEnabled, "true")
            .maxDrivers(1)
            .assertResults(expectedResult);
      } else {
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .assertResults(expectedResult);
      }
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    auto taskPauseWait = task->requestPause();
    driverWait.notify();
    taskPauseWait.wait();

    uint64_t reclaimableBytes{0};
    const bool reclaimable = op->reclaimableBytes(reclaimableBytes);
    ASSERT_EQ(op->canReclaim(), enableSpilling);
    ASSERT_EQ(reclaimable, enableSpilling);

    if (enableSpilling) {
      ASSERT_GT(reclaimableBytes, 0);
      const auto usedMemoryBytes = op->pool()->currentBytes();
      reclaimAndRestoreCapacity(
          op,
          folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_),
          reclaimerStats_);
      ASSERT_GT(reclaimerStats_.reclaimedBytes, 0);
      ASSERT_GT(reclaimerStats_.reclaimExecTimeUs, 0);
      // No reclaim as the operator has started output processing.
      ASSERT_EQ(usedMemoryBytes, op->pool()->currentBytes());
    } else {
      ASSERT_EQ(reclaimableBytes, 0);
      VELOX_ASSERT_THROW(
          op->reclaim(
              folly::Random::oneIn(2) ? 0 : folly::Random::rand32(rng_),
              reclaimerStats_),
          "");
    }

    Task::resume(task);
    taskThread.join();

    auto stats = task->taskStats().pipelineStats;
    ASSERT_EQ(stats[0].operatorStats[1].spilledBytes, 0);
    ASSERT_EQ(stats[0].operatorStats[1].spilledPartitions, 0);
    OperatorTestBase::deleteTaskAndCheckSpillDirectory(task);
  }
  ASSERT_EQ(reclaimerStats_.numNonReclaimableAttempts, 1);
}

DEBUG_ONLY_TEST_F(OrderByTest, abortDuringOutputProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  struct {
    bool abortFromRootMemoryPool;
    int numDrivers;

    std::string debugString() const {
      return fmt::format(
          "abortFromRootMemoryPool {} numDrivers {}",
          abortFromRootMemoryPool,
          numDrivers);
    }
  } testSettings[] = {{true, 1}, {false, 1}, {true, 4}, {false, 4}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<bool> injectOnce{true};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::noMoreInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "OrderBy") {
            return;
          }
          op = testOp;
          if (!injectOnce.exchange(false)) {
            return;
          }
          auto* driver = op->testingOperatorCtx()->driver();
          ASSERT_EQ(
              driver->task()->enterSuspended(driver->state()),
              StopReason::kNone);
          testWait.notify();
          driverWait.wait(driverWaitKey);
          ASSERT_EQ(
              driver->task()->leaveSuspended(driver->state()),
              StopReason::kAlreadyTerminated);
          VELOX_MEM_POOL_ABORTED("Memory pool aborted");
        })));

    std::thread taskThread([&]() {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(
              PlanBuilder()
                  .values(batches)
                  .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                  .planNode())
              .queryCtx(queryCtx)
              .maxDrivers(1)
              .assertResults(expectedResult),
          "");
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    testData.abortFromRootMemoryPool ? abortPool(queryCtx->pool())
                                     : abortPool(op->pool());
    ASSERT_TRUE(op->pool()->aborted());
    ASSERT_TRUE(queryCtx->pool()->aborted());
    ASSERT_EQ(queryCtx->pool()->currentBytes(), 0);
    driverWait.notify();
    taskThread.join();
    task.reset();
    waitForAllTasksToBeDeleted();
  }
}

DEBUG_ONLY_TEST_F(OrderByTest, abortDuringInputgProcessing) {
  constexpr int64_t kMaxBytes = 1LL << 30; // 1GB
  auto rowType = ROW({"c0", "c1", "c2"}, {INTEGER(), INTEGER(), INTEGER()});
  VectorFuzzer fuzzer({.vectorSize = 1000}, pool());
  const int32_t numBatches = 10;
  std::vector<RowVectorPtr> batches;
  for (int32_t i = 0; i < numBatches; ++i) {
    batches.push_back(fuzzer.fuzzRow(rowType));
  }

  struct {
    bool abortFromRootMemoryPool;
    int numDrivers;

    std::string debugString() const {
      return fmt::format(
          "abortFromRootMemoryPool {} numDrivers {}",
          abortFromRootMemoryPool,
          numDrivers);
    }
  } testSettings[] = {{true, 1}, {false, 1}, {true, 4}, {false, 4}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    auto queryCtx = std::make_shared<core::QueryCtx>(executor_.get());
    queryCtx->testingOverrideMemoryPool(
        memory::defaultMemoryManager().addRootPool(
            queryCtx->queryId(), kMaxBytes, memory::MemoryReclaimer::create()));
    auto expectedResult =
        AssertQueryBuilder(
            PlanBuilder()
                .values(batches)
                .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                .planNode())
            .queryCtx(queryCtx)
            .copyResults(pool_.get());

    folly::EventCount driverWait;
    auto driverWaitKey = driverWait.prepareWait();
    folly::EventCount testWait;
    auto testWaitKey = testWait.prepareWait();

    std::atomic<int> numInputs{0};
    Operator* op;
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* testOp) {
          if (testOp->operatorType() != "OrderBy") {
            return;
          }
          op = testOp;
          ++numInputs;
          if (numInputs != 2) {
            return;
          }
          auto* driver = op->testingOperatorCtx()->driver();
          ASSERT_EQ(
              driver->task()->enterSuspended(driver->state()),
              StopReason::kNone);
          testWait.notify();
          driverWait.wait(driverWaitKey);
          ASSERT_EQ(
              driver->task()->leaveSuspended(driver->state()),
              StopReason::kAlreadyTerminated);
          // Simulate the memory abort by memory arbitrator.
          VELOX_MEM_POOL_ABORTED("Memory pool aborted");
        })));

    std::thread taskThread([&]() {
      VELOX_ASSERT_THROW(
          AssertQueryBuilder(
              PlanBuilder()
                  .values(batches)
                  .orderBy({fmt::format("{} ASC NULLS LAST", "c0")}, false)
                  .planNode())
              .queryCtx(queryCtx)
              .maxDrivers(1)
              .assertResults(expectedResult),
          "");
    });

    testWait.wait(testWaitKey);
    ASSERT_TRUE(op != nullptr);
    auto task = op->testingOperatorCtx()->task();
    testData.abortFromRootMemoryPool ? abortPool(queryCtx->pool())
                                     : abortPool(op->pool());
    ASSERT_TRUE(op->pool()->aborted());
    ASSERT_TRUE(queryCtx->pool()->aborted());
    ASSERT_EQ(queryCtx->pool()->currentBytes(), 0);
    driverWait.notify();
    taskThread.join();
    task.reset();
    waitForAllTasksToBeDeleted();
  }
}
