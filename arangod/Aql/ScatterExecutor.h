////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_SCATTER_EXECUTOR_H
#define ARANGOD_AQL_SCATTER_EXECUTOR_H

#include "Aql/BlocksWithClients.h"
#include "Aql/ClusterNodes.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutorInfos.h"

namespace arangodb {
namespace aql {

class SkipResult;
class ExecutionEngine;
class ScatterNode;

class ScatterExecutorInfos : public ExecutorInfos, public ClientsExecutorInfos {
 public:
  ScatterExecutorInfos(std::shared_ptr<std::unordered_set<RegisterId>> readableInputRegisters,
                       std::shared_ptr<std::unordered_set<RegisterId>> writeableOutputRegisters,
                       RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
                       std::unordered_set<RegisterId> registersToClear,
                       std::unordered_set<RegisterId> registersToKeep,
                       std::vector<std::string> clientIds);
};

// The ScatterBlock is actually implemented by specializing ExecutionBlockImpl,
// so this class only exists to identify the specialization.
class ScatterExecutor {
 public:
  using Infos = ScatterExecutorInfos;

  class ClientBlockData {
   public:
    ClientBlockData(ExecutionEngine& engine, ScatterNode const* node,
                    ExecutorInfos const& scatterInfos);

    auto clear() -> void;
    auto addBlock(SharedAqlItemBlockPtr block, SkipResult skipped) -> void;
    auto hasDataFor(AqlCall const& call) -> bool;

    auto execute(AqlCallStack callStack, ExecutionState upstreamState)
        -> std::tuple<ExecutionState, SkipResult, SharedAqlItemBlockPtr>;

   private:
    std::deque<std::tuple<SharedAqlItemBlockPtr, SkipResult>> _queue;
    // This is unique_ptr to get away with everything beeing forward declared...
    std::unique_ptr<ExecutionBlock> _executor;
    bool _executorHasMore;
  };

  ScatterExecutor(ExecutorInfos const&);
  ~ScatterExecutor() = default;

  auto distributeBlock(SharedAqlItemBlockPtr block, SkipResult skipped,
                       std::unordered_map<std::string, ClientBlockData>& blockMap) const
      -> void;
};

/**
 * @brief See ExecutionBlockImpl.h for documentation.
 */
template <>
class ExecutionBlockImpl<ScatterExecutor> : public BlocksWithClientsImpl<ScatterExecutor> {
 public:
  // TODO Even if it's not strictly necessary here, for consistency's sake the
  // non-standard argument (shardIds) should probably be moved into some
  // ScatterExecutorInfos class.
  ExecutionBlockImpl(ExecutionEngine* engine, ScatterNode const* node,
                     ScatterExecutorInfos&& infos);

  ~ExecutionBlockImpl() override = default;
};

}  // namespace aql
}  // namespace arangodb

#endif  // ARANGOD_AQL_SCATTER_EXECUTOR_H
