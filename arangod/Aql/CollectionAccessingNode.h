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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_COLLECTION_ACCESSING_NODE_H
#define ARANGOD_AQL_COLLECTION_ACCESSING_NODE_H 1

#include "Basics/debugging.h"

#include <string>

struct TRI_vocbase_t;

namespace arangodb {
namespace velocypack {
class Builder;
class Slice;
}
namespace aql {
struct Collection;
class ExecutionPlan;
struct Variable;

class CollectionAccessingNode {
 public:
  explicit CollectionAccessingNode(aql::Collection const* collection);
  CollectionAccessingNode(ExecutionPlan* plan, arangodb::velocypack::Slice slice);

 public:
  void toVelocyPack(arangodb::velocypack::Builder& builder, unsigned flags) const;

  /// @brief dumps the primary index
  void toVelocyPackHelperPrimaryIndex(arangodb::velocypack::Builder& builder) const;

  /// @brief return the database
  TRI_vocbase_t* vocbase() const;

  /// @brief return the collection
  aql::Collection const* collection() const;

  /// @brief modify collection after cloning
  /// should be used only in smart-graph context!
  void collection(aql::Collection const* collection);

  void setUsedShard(std::string const& shardName) {
    // We can only use the shard we are restricted to
    TRI_ASSERT(shardName.empty() || _restrictedTo.empty() || _restrictedTo == shardName);
    _usedShard = shardName;
  }

  /**
   * @brief Restrict this Node to a single Shard (cluster only)
   *
   * @param shardId The shard restricted to
   */
  void restrictToShard(std::string const& shardId);

  /**
   * @brief Check if this Node is restricted to a single Shard (cluster only)
   *
   * @return True if we are restricted, false otherwise
   */
  bool isRestricted() const;

  /**
   * @brief Get the Restricted shard for this Node
   *
   * @return The Shard this node is restricted to
   */
  std::string const& restrictedShard() const;

  /// @brief set the prototype collection when using distributeShardsLike
  void setPrototype(arangodb::aql::Collection const* prototypeCollection,
                    arangodb::aql::Variable const* prototypeOutVariable);

  aql::Collection const* prototypeCollection() const;
  aql::Variable const* prototypeOutVariable() const;

  bool isUsedAsSatellite() const { return _isSatellite; }

  void useAsSatellite();

  void cloneInto(CollectionAccessingNode& c) const {
    c._prototypeCollection = _prototypeCollection;
    c._prototypeOutVariable = _prototypeOutVariable;
    c._restrictedTo = _restrictedTo;
    c._isSatellite = _isSatellite;
    c._usedShard = _usedShard;
  }

 protected:
  aql::Collection const* _collection;

  /// @brief A shard this node is restricted to, may be empty
  std::string _restrictedTo;

  /// @brief prototype collection when using distributeShardsLike
  aql::Collection const* _prototypeCollection;
  aql::Variable const* _prototypeOutVariable;

  std::string _usedShard;

  bool _isSatellite;
};

}  // namespace aql
}  // namespace arangodb

#endif
