////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_DISTINCT_COLLECT_EXECUTOR_H
#define ARANGOD_AQL_DISTINCT_COLLECT_EXECUTOR_H

#include "Aql/AqlCall.h"
#include "Aql/AqlItemBlockInputRange.h"
#include "Aql/AqlValue.h"
#include "Aql/AqlValueGroup.h"
#include "Aql/ExecutionState.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/types.h"

#include <memory>
#include <unordered_set>

namespace arangodb {
namespace transaction {
class Methods;
}
namespace aql {

class InputAqlItemRow;
class OutputAqlItemRow;
class NoStats;
class ExecutorInfos;
template <BlockPassthrough>
class SingleRowFetcher;

class DistinctCollectExecutorInfos : public ExecutorInfos {
 public:
  DistinctCollectExecutorInfos(RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
                               std::unordered_set<RegisterId> registersToClear,
                               std::unordered_set<RegisterId> registersToKeep,
                               std::unordered_set<RegisterId>&& readableInputRegisters,
                               std::unordered_set<RegisterId>&& writeableInputRegisters,
                               std::pair<RegisterId, RegisterId> groupRegister,
                               transaction::Methods* trxPtr);

  DistinctCollectExecutorInfos() = delete;
  DistinctCollectExecutorInfos(DistinctCollectExecutorInfos&&) = default;
  DistinctCollectExecutorInfos(DistinctCollectExecutorInfos const&) = delete;
  ~DistinctCollectExecutorInfos() = default;

 public:
  [[nodiscard]] std::pair<RegisterId, RegisterId> const& getGroupRegister() const;
  transaction::Methods* getTransaction() const;

 private:
  /// @brief pairs, consisting of out register and in register
  std::pair<RegisterId, RegisterId> _groupRegister;

  /// @brief the transaction for this query
  transaction::Methods* _trxPtr;
};

/**
 * @brief Implementation of Distinct Collect Executor
 */

class DistinctCollectExecutor {
 public:
  struct Properties {
    static constexpr bool preservesOrder = false;
    static constexpr BlockPassthrough allowsBlockPassthrough = BlockPassthrough::Disable;
    static constexpr bool inputSizeRestrictsOutputSize = true;
  };
  using Fetcher = SingleRowFetcher<Properties::allowsBlockPassthrough>;
  using Infos = DistinctCollectExecutorInfos;
  using Stats = NoStats;

  DistinctCollectExecutor() = delete;
  DistinctCollectExecutor(DistinctCollectExecutor&&) = default;
  DistinctCollectExecutor(DistinctCollectExecutor const&) = delete;
  DistinctCollectExecutor(Fetcher& fetcher, Infos&);
  ~DistinctCollectExecutor();

  void initializeCursor();

  /**
   * @brief produce the next Row of Aql Values.
   *
   * @return ExecutionState, and if successful exactly one new Row of AqlItems.
   */
  std::pair<ExecutionState, Stats> produceRows(OutputAqlItemRow& output);

  /**
   * @brief produce the next Rows of Aql Values.
   *
   * @return ExecutorState, the stats, and a new Call that needs to be send to upstream
   */
  [[nodiscard]] auto produceRows(AqlItemBlockInputRange& input, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, Stats, AqlCall>;

  [[nodiscard]] auto skipRowsRange(AqlItemBlockInputRange& inputRange, AqlCall& call)
      -> std::tuple<ExecutorState, Stats, size_t, AqlCall>;

  std::pair<ExecutionState, size_t> expectedNumberOfRows(size_t atMost) const;

  [[nodiscard]] auto expectedNumberOfRowsNew(AqlItemBlockInputRange const& input,
                                             AqlCall const& call) const noexcept -> size_t;

 private:
  Infos const& infos() const noexcept;
  void destroyValues();

 private:
  Infos const& _infos;
  Fetcher& _fetcher;
  std::unordered_set<AqlValue, AqlValueGroupHash, AqlValueGroupEqual> _seen;
};

}  // namespace aql
}  // namespace arangodb

#endif
