////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_GRAPH_NODE_H
#define ARANGOD_AQL_GRAPH_NODE_H 1

#include "Aql/Condition.h"
#include "Aql/ExecutionNode.h"
#include "Aql/GraphNode.h"
#include "Aql/Graphs.h"
#include "Cluster/ClusterTypes.h"
#include "Cluster/TraverserEngineRegistry.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/voc-types.h"

#include <velocypack/Builder.h>

#include <map>
#include <string>
#include <vector>

namespace arangodb {

namespace graph {
struct BaseOptions;
class Graph;
}  // namespace graph

namespace aql {

// @brief This is a pure virtual super-class for all AQL graph operations
//        It does the generally required:
//        * graph info parsing
//        * traverser-engine creation
//        * option preparation
//        * Smart Graph Handling

class GraphNode : public ExecutionNode {
 protected:
  /// @brief constructor with a vocbase and a collection name
  GraphNode(ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase, AstNode const* direction,
            AstNode const* graph, std::unique_ptr<graph::BaseOptions> options);

  GraphNode(ExecutionPlan* plan, arangodb::velocypack::Slice const& base);

 public:
  bool isUsedAsSatellite() const;

  bool isEligibleAsSatelliteTraversal() const;

 protected:
  /// @brief Internal constructor to clone the node.
  GraphNode(ExecutionPlan* plan, size_t id, TRI_vocbase_t* vocbase,
            std::vector<std::unique_ptr<Collection>> const& edgeColls,
            std::vector<std::unique_ptr<Collection>> const& vertexColls,
            TRI_edge_direction_e defaultDirection, std::vector<TRI_edge_direction_e> directions,
            std::unique_ptr<graph::BaseOptions> options);

  /// @brief Clone constructor, used for constructors of derived classes.
  /// Does not clone recursively, does not clone properties (`other.plan()` is
  /// expected to be the same as `plan)`, and does not register this node in the
  /// plan.
  GraphNode(ExecutionPlan& plan, GraphNode const& other,
            std::unique_ptr<graph::BaseOptions> options);

  struct THIS_THROWS_WHEN_CALLED{};
  explicit GraphNode(THIS_THROWS_WHEN_CALLED);

  std::string const& collectionToShardName(std::string const& collName) const;

 public:
  ~GraphNode() override = default;

  void toVelocyPackHelper(arangodb::velocypack::Builder& nodes, unsigned flags,
                          std::unordered_set<ExecutionNode const*>& seen) const override;

  /// @brief the cost of a graph node
  CostEstimate estimateCost() const override;

  /// @brief flag, if smart traversal (enterprise edition only!) is done
  bool isSmart() const;

  /// @brief return the database
  TRI_vocbase_t* vocbase() const;

  /// @brief return the vertex out variable
  Variable const* vertexOutVariable() const;

  /// @brief checks if the vertex out variable is used
  bool usesVertexOutVariable() const;

  /// @brief set the vertex out variable
  void setVertexOutput(Variable const* outVar);

  /// @brief return the edge out variable
  Variable const* edgeOutVariable() const;

  /// @brief checks if the edge out variable is used
  bool usesEdgeOutVariable() const;

  /// @brief set the edge out variable
  void setEdgeOutput(Variable const* outVar);

  /// @brief Compute the shortest path options containing the expressions
  ///        MUST! be called after optimization and before creation
  ///        of blocks.
  virtual void prepareOptions() = 0;

  graph::BaseOptions* options() const;

  /// @brief Get the AstNode containing the temporary variable
  AstNode* getTemporaryRefNode() const;

  /// @brief Get the temporary variable
  Variable const* getTemporaryVariable() const;

  /// @brief Add a traverser engine Running on a DBServer to this node.
  ///        The block will communicate with them (CLUSTER ONLY)
  void addEngine(traverser::TraverserEngineID const&, ServerID const&);

  /// @brief enhance the TraversalEngine with all necessary info
  ///        to be send to DBServers (CLUSTER ONLY)
  void enhanceEngineInfo(arangodb::velocypack::Builder&) const;

  /// @brief Returns a reference to the engines. (CLUSTER ONLY)
  std::unordered_map<ServerID, traverser::TraverserEngineID> const* engines() const;

  std::vector<std::unique_ptr<aql::Collection>> const& edgeColls() const;

  std::vector<std::unique_ptr<aql::Collection>> const& vertexColls() const;

  virtual void getConditionVariables(std::vector<Variable const*>&) const;

  /// @brief return any of the collections
  Collection const* collection() const;

  void injectVertexCollection(aql::Collection const* other);

  std::vector<aql::Collection const*> const collections() const;
  void setCollectionToShard(std::map<std::string, std::string> const& map) {
    _collectionToShard = map;
  }
  void addCollectionToShard(std::string const& coll, std::string const& shard) {
    _collectionToShard.emplace(coll, shard);
  }

 public:
  graph::Graph const* graph() const noexcept;

 private:
  void addEdgeCollection(std::string const& n, TRI_edge_direction_e dir);

  void setGraphInfoAndCopyColls(std::vector<std::unique_ptr<Collection>> const& edgeColls,
                                std::vector<std::unique_ptr<Collection>> const& vertexColls);

 protected:
  /// @brief the database
  TRI_vocbase_t* _vocbase;

  /// @brief vertex output variable
  Variable const* _vertexOutVariable;

  /// @brief vertex output variable
  Variable const* _edgeOutVariable;

  /// @brief our graph...
  graph::Graph const* _graphObj;

  /// @brief Temporary pseudo variable for the currently traversed object.
  Variable const* _tmpObjVariable;

  /// @brief Reference to the pseudo variable
  AstNode* _tmpObjVarNode;

  /// @brief Pseudo string value node to hold the last visited vertex id.
  AstNode* _tmpIdNode;

  /// @brief input graphInfo only used for serialization & info
  arangodb::velocypack::Builder _graphInfo;

  /// @brief the edge collection names
  std::vector<std::unique_ptr<aql::Collection>> _edgeColls;

  /// @brief the vertex collection names (can also be edge collections
  /// as an edge can also point to another edge, instead of a vertex).
  std::vector<std::unique_ptr<aql::Collection>> _vertexColls;

  /// @brief The default direction given in the query
  TRI_edge_direction_e const _defaultDirection;

  /// @brief The directions edges are followed
  std::vector<TRI_edge_direction_e> _directions;

  /// @brief Options for traversals
  std::unique_ptr<graph::BaseOptions> _options;

  /// @brief Flag if the options have been built.
  /// Afterwards this class is not copyable anymore.
  bool _optionsBuilt;

  /// @brief The list of traverser engines grouped by server.
  std::unordered_map<ServerID, traverser::TraverserEngineID> _engines;

  /// @brief flag, if graph is smart (enterprise edition only!)
  bool _isSmart;

  /// @brief list of shards involved, required for one-shard-databases
  std::map<std::string, std::string> _collectionToShard;
};

}  // namespace aql
}  // namespace arangodb
#endif
