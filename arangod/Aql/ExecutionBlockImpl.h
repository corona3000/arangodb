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

#ifndef ARANGOD_AQL_EXECUTION_BLOCK_IMPL_H
#define ARANGOD_AQL_EXECUTION_BLOCK_IMPL_H 1

#include "Aql/AqlCall.h"
#include "Aql/AqlCallSet.h"
#include "Aql/ConstFetcher.h"
#include "Aql/DependencyProxy.h"
#include "Aql/ExecutionBlock.h"

#include <functional>
#include <memory>
#include <queue>

namespace arangodb::aql {

template <BlockPassthrough passThrough>
class SingleRowFetcher;

template <class Fetcher>
class IdExecutor;

struct AqlCall;
class AqlItemBlock;
class ExecutionEngine;
class ExecutionNode;
class InputAqlItemRow;
class OutputAqlItemRow;
class Query;
class ShadowAqlItemRow;
class SkipResult;
class ParallelUnsortedGatherExecutor;
class MultiDependencySingleRowFetcher;

template <typename T, typename... Es>
constexpr bool is_one_of_v = (std::is_same_v<T, Es> || ...);

template <typename Executor>
static constexpr bool isMultiDepExecutor =
    std::is_same_v<typename Executor::Fetcher, MultiDependencySingleRowFetcher>;

/**
 * @brief This is the implementation class of AqlExecutionBlocks.
 *        It is responsible to create AqlItemRows for subsequent
 *        Blocks, also it will fetch new AqlItemRows from preceeding
 *        Blocks whenever it is necessary.
 *        For performance reasons this will all be done in batches
 *        of 1000 Rows each.
 *
 * @tparam Executor A class that needs to have the following signature:
 *         Executor {
 *           using Fetcher = xxxFetcher;
 *           using Infos = xxxExecutorInfos;
 *           using Stats = xxxStats;
 *           static const struct {
 *             // Whether input rows stay in the same order.
 *             const bool preservesOrder;
 *
 *             // Whether input blocks can be reused as output blocks. This
 *             // can be true if:
 *             // - There will be exactly one output row per input row.
 *             // - produceRows() for row i will be called after fetchRow() of
 *             //   row i.
 *             // - The order of rows is preserved. i.e. preservesOrder
 *             // - The register planning must reserve the output register(s),
 *             //   if any, already in the input blocks.
 *             //   (This one is not really a property of the Executor, but it
 *             //   still must be true).
 *             // - It must use the SingleRowFetcher. This is not really a
 *             //   necessity, but it should always already be possible due to
 *             //   the properties above.
 *             const bool allowsBlockPassthrough;
 *
 *           } properties;
 *           Executor(Fetcher&, Infos&);
 *           std::pair<ExecutionState, Stats> produceRows(OutputAqlItemRow& output);
 *         }
 *         The Executor is the implementation of one AQLNode.
 *         It may produce zero, one, or multiple outputRows at a time. The
 *         OutputAqlItemRow imposes a restriction (e.g. atMost) on how many.
 *         Currently, this is just the size of the block (because this is always
 *         restricted by atMost anyway). Later this may be replaced by a
 *         separate limit. It can fetch as many rows from above as it likes.
 *         It can only follow the xxxFetcher interface to get AqlItemRows from
 *         Upstream.
 */
template <class Executor>
class ExecutionBlockImpl final : public ExecutionBlock {
  using Fetcher = typename Executor::Fetcher;
  using ExecutorStats = typename Executor::Stats;
  using Infos = typename Executor::Infos;
  using DataRange = typename Executor::Fetcher::DataRange;

  using DependencyProxy =
      typename aql::DependencyProxy<Executor::Properties::allowsBlockPassthrough>;

  static_assert(
      Executor::Properties::allowsBlockPassthrough == BlockPassthrough::Disable ||
          Executor::Properties::preservesOrder,
      "allowsBlockPassthrough must imply preservesOrder, but does not!");

 private:
  // Used in getSome/skipSome implementation. deprecated
  enum class InternalState { FETCH_DATA, FETCH_SHADOWROWS, DONE };

  // Used in execute implmentation
  // Defines the internal state this executor is in.
  enum class ExecState {
    // We need to check the client call to define the next state (inital state)
    CHECKCALL,
    // We are skipping rows in offset
    SKIP,
    // We are producing rows
    PRODUCE,
    // We are done producing (limit reached) and drop all rows that are unneeded, might count.
    FASTFORWARD,
    // We need more information from dependency
    UPSTREAM,
    // We are done with a subquery, we need to pass forward ShadowRows
    SHADOWROWS,
    // Locally done, ready to return, will set state to resetted
    DONE
  };

  // Where Executors with a single dependency return an AqlCall, Executors with
  // multiple dependencies return a partial map depIndex -> AqlCall.
  // It may be empty. If the cardinality is greater than one, the calls will be
  // executed in parallel.
  using AqlCallType = std::conditional_t<isMultiDepExecutor<Executor>, AqlCallSet, AqlCall>;

 public:
  /**
   * @brief Construct a new ExecutionBlock
   *        This API is subject to change, we want to make it as independent of
   *        AQL / Query interna as possible.
   *
   * @param engine The AqlExecutionEngine holding the query and everything
   *               required for the execution.
   * @param node The Node used to create this ExecutionBlock
   */
  ExecutionBlockImpl(ExecutionEngine* engine, ExecutionNode const* node,
                     typename Executor::Infos);

  ~ExecutionBlockImpl() override;

  /// @brief Must be called exactly once after the plan is instantiated (i.e.,
  ///        all blocks are created and dependencies are injected), but before
  ///        the first execute() call.
  ///        Is currently called conditionally in execute() itself, but should
  ///        better be called in instantiateFromPlan and similar methods.
  void init();

  /**
   * @brief Produce atMost many output rows, or less.
   *        May return waiting if I/O has to be performed
   *        so we can release this thread.
   *        Is required to return DONE if it is guaranteed
   *        that this block does not produce more rows,
   *        Returns HASMORE if the DONE guarantee cannot be given.
   *        HASMORE does not give strict guarantee, it maybe that
   *        HASMORE is returned but no more rows can be produced.
   *
   * @param atMost Upper bound of AqlItemRows to be returned.
   *               Target is to get as close to this upper bound
   *               as possible.
   *
   * @return A pair with the following properties:
   *         ExecutionState:
   *           WAITING => IO going on, immediately return to caller.
   *           DONE => No more to expect from Upstream, if you are done with
   *                   this row return DONE to caller.
   *           HASMORE => There is potentially more from above, call again if
   *                      you need more input.
   *         AqlItemBlock:
   *           A matrix of result rows.
   *           Guaranteed to be non nullptr in the HASMORE case, maybe a nullptr
   *           in DONE. Is a nullptr in WAITING.
   */
  [[nodiscard]] std::pair<ExecutionState, SharedAqlItemBlockPtr> getSome(size_t atMost) override;

  /**
   * @brief Like get some, but lines are skipped and not returned.
   *        This can use optimizations to not actually create the data.
   *
   * @param atMost Upper bound of AqlItemRows to be skipped.
   *               Target is to get as close to this upper bound
   *               as possible.
   *
   * @return A pair with the following properties:
   *         ExecutionState:
   *           WAITING => IO going on, immediatly return to caller.
   *           DONE => No more to expect from Upstream, if you are done with
   *                   this row return DONE to caller.
   *           HASMORE => There is potentially more from above, call again if
   *                   you need more input. size_t: Number of rows effectively
   *                   skipped. On WAITING this is always 0. On any other state
   *                   this is between 0 and atMost.
   */
  [[nodiscard]] std::pair<ExecutionState, size_t> skipSome(size_t atMost) override;

  [[nodiscard]] std::pair<ExecutionState, Result> initializeCursor(InputAqlItemRow const& input) override;

  template <class exec = Executor, typename = std::enable_if_t<std::is_same_v<exec, IdExecutor<ConstFetcher>>>>
  auto injectConstantBlock(SharedAqlItemBlockPtr block, SkipResult skipped) -> void;

  [[nodiscard]] Infos const& infos() const;

  /// @brief shutdown, will be called exactly once for the whole query
  /// Special implementation for all Executors that need to implement Shutdown
  /// Most do not, we might be able to move their shutdown logic to a more
  /// central place.
  [[nodiscard]] std::pair<ExecutionState, Result> shutdown(int) override;

  /// @brief main function to produce data in this ExecutionBlock.
  ///        It gets the AqlCallStack defining the operations required in every
  ///        subquery level. It will then perform the requested amount of offset, data and fullcount.
  ///        The AqlCallStack is copied on purpose, so this block can modify it.
  ///        Will return
  ///        1. state:
  ///          * WAITING: We have async operation going on, nothing happend, please call again
  ///          * HASMORE: Here is some data in the request range, there is still more, if required call again
  ///          * DONE: Here is some data, and there will be no further data available.
  ///        2. SkipResult: Amount of documents skipped.
  ///        3. SharedAqlItemBlockPtr: The next data block.
  std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> execute(AqlCallStack stack) override;

  template <class exec = Executor, typename = std::enable_if_t<std::is_same_v<exec, IdExecutor<SingleRowFetcher<BlockPassthrough::Enable>>>>>
  [[nodiscard]] RegisterId getOutputRegisterId() const noexcept;

 private:
  /**
   * @brief Inner execute() part, without the tracing calls.
   */
  std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr> executeWithoutTrace(AqlCallStack stack);

  std::tuple<ExecutionState, SkipResult, typename Fetcher::DataRange> executeFetcher(
      AqlCallStack& stack, AqlCallType const& aqlCall);

  std::tuple<ExecutorState, typename Executor::Stats, AqlCallType> executeProduceRows(
      typename Fetcher::DataRange& input, OutputAqlItemRow& output);

  // execute a skipRowsRange call
  auto executeSkipRowsRange(typename Fetcher::DataRange& inputRange, AqlCall& call)
      -> std::tuple<ExecutorState, typename Executor::Stats, size_t, AqlCallType>;

  auto executeFastForward(typename Fetcher::DataRange& inputRange, AqlCall& clientCall)
      -> std::tuple<ExecutorState, typename Executor::Stats, size_t, AqlCallType>;

  /**
   * @brief Inner getSome() part, without the tracing calls.
   */
  [[nodiscard]] std::pair<ExecutionState, SharedAqlItemBlockPtr> getSomeWithoutTrace(size_t atMost);

  /**
   * @brief Inner skipSome() part, without the tracing calls.
   */
  [[nodiscard]] std::pair<ExecutionState, size_t> skipSomeOnceWithoutTrace(size_t atMost);

  /**
   * @brief Allocates a new AqlItemBlock and returns it, with the specified
   *        number of rows (nrItems) and columns (nrRegs).
   *        In case the Executor supports pass-through of blocks (i.e. reuse the
   *        input blocks as output blocks), it returns such an input block. In
   *        this case, the number of columns must still match - this has to be
   *        guaranteed by register planning.
   *        The state will be HASMORE if and only if it returns an actual block,
   *        which it always will in the non-pass-through case (modulo
   *        exceptions). If it is WAITING or DONE, the returned block is always
   *        a nullptr. This happens only if upstream is WAITING, or
   *        respectively, if it is DONE and did not return a new block.
   */
  [[nodiscard]] std::pair<ExecutionState, SharedAqlItemBlockPtr> requestWrappedBlock(
      size_t nrItems, RegisterId nrRegs);

  [[nodiscard]] std::unique_ptr<OutputAqlItemRow> createOutputRow(SharedAqlItemBlockPtr& newBlock,
                                                                  AqlCall&& call);

  [[nodiscard]] Query const& getQuery() const;

  [[nodiscard]] Executor& executor();

  /// @brief request an AqlItemBlock from the memory manager
  [[nodiscard]] SharedAqlItemBlockPtr requestBlock(size_t nrItems, RegisterCount nrRegs);

  [[nodiscard]] ExecutionState fetchShadowRowInternal();

  // Allocate an output block and install a call in it
  [[nodiscard]] auto allocateOutputBlock(AqlCall&& call, DataRange const& inputRange)
      -> std::unique_ptr<OutputAqlItemRow>;

  // Ensure that we have an output block of the desired dimensions
  // Will as a side effect modify _outputItemRow
  void ensureOutputBlock(AqlCall&& call, DataRange const& inputRange);

  // Compute the next state based on the given call.
  // Can only be one of Skip/Produce/FullCount/FastForward/Done
  [[nodiscard]] auto nextState(AqlCall const& call) const -> ExecState;

  // Executor is done, we need to handle ShadowRows of subqueries.
  // In most executors they are simply copied, in subquery executors
  // there needs to be actions applied here.
  [[nodiscard]] auto shadowRowForwarding() -> ExecState;

  [[nodiscard]] auto outputIsFull() const noexcept -> bool;

  [[nodiscard]] auto lastRangeHasDataRow() const noexcept -> bool;

  void resetExecutor();

  // Forwarding of ShadowRows if the executor has SideEffects.
  // This skips over ShadowRows, and counts them in the correct
  // position of the callStack as "skipped".
  // as soon as we reach a place where there is no skip
  // ordered in the outer shadow rows, this call
  // will fall back to shadowRowForwardning.
  [[nodiscard]] auto sideEffectShadowRowForwarding(AqlCallStack& stack,
                                                   SkipResult& skipResult) -> ExecState;

  /**
   * @brief Transition to the next state after shadowRows
   *
   * @param state the state returned by the getShadowRowCall
   * @param range the current data range
   * @return ExecState The next state
   */
  [[nodiscard]] auto nextStateAfterShadowRows(ExecutorState const& state,
                                              DataRange const& range) const
      noexcept -> ExecState;

  void initOnce();

  [[nodiscard]] auto executorNeedsCall(AqlCallType& call) const noexcept -> bool;

 private:
  /**
   * @brief Used to allow the row Fetcher to access selected methods of this
   *        ExecutionBlock object.
   */
  DependencyProxy _dependencyProxy;

  /**
   * @brief Fetcher used by the Executor. Calls this->fetchBlock() and handles
   *        memory management of AqlItemBlocks as needed by Executor.
   */
  Fetcher _rowFetcher;

  /**
   * @brief This is the working party of this implementation
   *        the template class needs to implement the logic
   *        to produce a single row from the upstream information.
   */
  Infos _infos;

  Executor _executor;

  std::unique_ptr<OutputAqlItemRow> _outputItemRow;

  Query const& _query;

  InternalState _state;

  SkipResult _skipped{};

  DataRange _lastRange;

  ExecState _execState;

  AqlCallType _upstreamRequest;

  AqlCall _clientRequest;

  // Only used in passthrough variant.
  // We track if we have reference the range's block
  // into an output block.
  // If so we are not allowed to reuse it.
  bool _hasUsedDataRangeBlock;

  bool _executorReturnedDone = false;

  bool _initialized = false;
};

}  // namespace arangodb::aql
#endif
