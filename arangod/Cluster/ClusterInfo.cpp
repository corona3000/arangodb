////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Max Neunhoeffer
/// @author Jan Steemann
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "ClusterInfo.h"

#include "Agency/TimeString.h"
#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ConditionLocker.h"
#include "Basics/Exceptions.h"
#include "Basics/MutexLocker.h"
#include "Basics/NumberUtils.h"
#include "Basics/RecursiveLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/VelocyPackHelper.h"
#include "Basics/WriteLocker.h"
#include "Basics/hashes.h"
#include "Basics/system-functions.h"
#include "Cluster/AgencyPaths.h"
#include "Cluster/ClusterCollectionCreationInfo.h"
#include "Cluster/ClusterHelpers.h"
#include "Cluster/RebootTracker.h"
#include "Cluster/ServerState.h"
#include "Logger/Logger.h"
#include "Random/RandomGenerator.h"
#include "Rest/CommonDefines.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "Scheduler/SchedulerFeature.h"
#include "Sharding/ShardingInfo.h"
#include "StorageEngine/PhysicalCollection.h"
#include "Utils/Events.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/VocbaseInfo.h"

#ifdef USE_ENTERPRISE
#include "Enterprise/VocBase/SmartVertexCollection.h"
#include "Enterprise/VocBase/VirtualCollection.h"
#endif

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace {

static inline arangodb::AgencyOperation IncreaseVersion() {
  return arangodb::AgencyOperation{"Plan/Version",
                                   arangodb::AgencySimpleOperationType::INCREMENT_OP};
}

static inline std::string collectionPath(std::string const& dbName,
                                         std::string const& collection) {
  return "Plan/Collections/" + dbName + "/" + collection;
}

static inline arangodb::AgencyOperation CreateCollectionOrder(std::string const& dbName,
                                                              std::string const& collection,
                                                              VPackSlice const& info) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  if (!info.get("shards").isEmptyObject() &&
      !arangodb::basics::VelocyPackHelper::getBooleanValue(info, arangodb::StaticStrings::IsSmart,
                                                           false)) {
    TRI_ASSERT(info.hasKey(arangodb::StaticStrings::AttrIsBuilding));
    TRI_ASSERT(info.get(arangodb::StaticStrings::AttrIsBuilding).isBool());
    TRI_ASSERT(info.get(arangodb::StaticStrings::AttrIsBuilding).getBool() == true);
  }
#endif
  arangodb::AgencyOperation op{collectionPath(dbName, collection),
                               arangodb::AgencyValueOperationType::SET, info};
  return op;
}

static inline arangodb::AgencyPrecondition CreateCollectionOrderPrecondition(
    std::string const& dbName, std::string const& collection, VPackSlice const& info) {
  arangodb::AgencyPrecondition prec{collectionPath(dbName, collection),
                                    arangodb::AgencyPrecondition::Type::VALUE, info};
  return prec;
}

static inline arangodb::AgencyOperation CreateCollectionSuccess(
    std::string const& dbName, std::string const& collection, VPackSlice const& info) {
  TRI_ASSERT(!info.hasKey(arangodb::StaticStrings::AttrIsBuilding));
  return arangodb::AgencyOperation{collectionPath(dbName, collection),
                                   arangodb::AgencyValueOperationType::SET, info};
}

}  // namespace

#ifdef _WIN32
// turn off warnings about too long type name for debug symbols blabla in MSVC
// only...
#pragma warning(disable : 4503)
#endif

using namespace arangodb;
using namespace arangodb::cluster;
using namespace arangodb::methods;

////////////////////////////////////////////////////////////////////////////////
/// @brief a local helper to report errors and messages
////////////////////////////////////////////////////////////////////////////////

static inline int setErrormsg(int ourerrno, std::string& errorMsg) {
  errorMsg = TRI_errno_string(ourerrno);
  return ourerrno;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief check whether the JSON returns an error
////////////////////////////////////////////////////////////////////////////////

static inline bool hasError(VPackSlice const& slice) {
  return arangodb::basics::VelocyPackHelper::getBooleanValue(slice, StaticStrings::Error, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief extract the error message from a JSON
////////////////////////////////////////////////////////////////////////////////

static std::string extractErrorMessage(std::string const& shardId, VPackSlice const& slice) {
  std::string msg = " shardID:" + shardId + ": ";

  // add error message text
  msg += arangodb::basics::VelocyPackHelper::getStringValue(slice,
                                                            StaticStrings::ErrorMessage, "");

  // add error number
  if (slice.hasKey(StaticStrings::ErrorNum)) {
    VPackSlice const errorNum = slice.get(StaticStrings::ErrorNum);
    if (errorNum.isNumber()) {
      msg += " (errNum=" +
             arangodb::basics::StringUtils::itoa(errorNum.getNumericValue<uint32_t>()) +
             ")";
    }
  }

  return msg;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief creates an empty collection info object
////////////////////////////////////////////////////////////////////////////////

CollectionInfoCurrent::CollectionInfoCurrent(uint64_t currentVersion)
    : _currentVersion(currentVersion) {}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a collection info object
////////////////////////////////////////////////////////////////////////////////

CollectionInfoCurrent::~CollectionInfoCurrent() = default;

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a cluster info object
////////////////////////////////////////////////////////////////////////////////

ClusterInfo::ClusterInfo(application_features::ApplicationServer& server,
                         AgencyCallbackRegistry* agencyCallbackRegistry)
    : _server(server),
      _agency(server),
      _agencyCallbackRegistry(agencyCallbackRegistry),
      _rebootTracker(SchedulerFeature::SCHEDULER),
      _planVersion(0),
      _currentVersion(0),
      _planLoader(std::thread::id()),
      _uniqid() {
  _uniqid._currentValue = 1ULL;
  _uniqid._upperValue = 0ULL;
  _uniqid._nextBatchStart = 1ULL;
  _uniqid._nextUpperValue = 0ULL;
  _uniqid._backgroundJobIsRunning = false;
  // Actual loading into caches is postponed until necessary
}
////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a cluster info object
////////////////////////////////////////////////////////////////////////////////

ClusterInfo::~ClusterInfo() = default;

////////////////////////////////////////////////////////////////////////////////
/// @brief cleanup method which frees cluster-internal shared ptrs on shutdown
////////////////////////////////////////////////////////////////////////////////

void ClusterInfo::cleanup() {
  while (true) {
    {
      MUTEX_LOCKER(mutexLocker, _idLock);
      if (!_uniqid._backgroundJobIsRunning) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  MUTEX_LOCKER(mutexLocker, _planProt.mutex);

  TRI_ASSERT(_newPlannedViews.empty());  // only non-empty during loadPlan()
  _plannedViews.clear();
  _plannedCollections.clear();
  _shards.clear();
  _shardIds.clear();
  _currentCollections.clear();
}

void ClusterInfo::triggerBackgroundGetIds() {
  // Trigger a new load of batches
  _uniqid._nextBatchStart = 1ULL;
  _uniqid._nextUpperValue = 0ULL;

  try {
    _idLock.assertLockedByCurrentThread();
    if (_uniqid._backgroundJobIsRunning) {
      return;
    }
    _uniqid._backgroundJobIsRunning = true;
    std::thread([this] {
      auto guardRunning = scopeGuard([this] {
        MUTEX_LOCKER(mutexLocker, _idLock);
        _uniqid._backgroundJobIsRunning = false;
      });

      uint64_t result;
      try {
        result = _agency.uniqid(MinIdsPerBatch, 0.0);
      } catch (std::exception const&) {
        return;
      }

      {
        MUTEX_LOCKER(mutexLocker, _idLock);

        if (1ULL == _uniqid._nextBatchStart) {
          // Invalidate next batch
          _uniqid._nextBatchStart = result;
          _uniqid._nextUpperValue = result + MinIdsPerBatch - 1;
        }
        // If we get here, somebody else tried succeeded in doing the same,
        // so we just try again.
      }
    }).detach();
  } catch (std::exception const& e) {
    LOG_TOPIC("adef4", WARN, Logger::CLUSTER)
        << "Failed to trigger background get ids. " << e.what();
  }
}

/// @brief produces an agency dump and logs it
void ClusterInfo::logAgencyDump() const {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  AgencyComm ac(_server);
  AgencyCommResult ag = ac.getValues("/");

  if (ag.successful()) {
    LOG_TOPIC("fe8ce", INFO, Logger::CLUSTER) << "Agency dump:\n"
                                              << ag.slice().toJson();
  } else {
    LOG_TOPIC("e7e30", WARN, Logger::CLUSTER) << "Could not get agency dump!";
  }
#endif
}

////////////////////////////////////////////////////////////////////////////////
/// @brief increase the uniqid value. if it exceeds the upper bound, fetch a
/// new upper bound value from the agency
////////////////////////////////////////////////////////////////////////////////

uint64_t ClusterInfo::uniqid(uint64_t count) {
  MUTEX_LOCKER(mutexLocker, _idLock);

  if (_uniqid._currentValue + count - 1 <= _uniqid._upperValue) {
    uint64_t result = _uniqid._currentValue;
    _uniqid._currentValue += count;
    return result;
  }

  // Try if we can use the next batch
  if (_uniqid._nextBatchStart + count - 1 <= _uniqid._nextUpperValue) {
    uint64_t result = _uniqid._nextBatchStart;
    _uniqid._currentValue = _uniqid._nextBatchStart + count;
    _uniqid._upperValue = _uniqid._nextUpperValue;
    triggerBackgroundGetIds();

    return result;
  }

  // We need to fetch from the agency

  uint64_t fetch = count;

  if (fetch < MinIdsPerBatch) {
    fetch = MinIdsPerBatch;
  }

  uint64_t result = _agency.uniqid(2 * fetch, 0.0);

  _uniqid._currentValue = result + count;
  _uniqid._upperValue = result + fetch - 1;
  // Invalidate next batch
  _uniqid._nextBatchStart = _uniqid._upperValue + 1;
  _uniqid._nextUpperValue = _uniqid._upperValue + fetch - 1;

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief flush the caches (used for testing)
////////////////////////////////////////////////////////////////////////////////

void ClusterInfo::flush() {
  loadServers();
  loadCurrentDBServers();
  loadCurrentCoordinators();
  loadCurrentMappings();
  loadPlan();
  loadCurrent();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ask whether a cluster database exists
////////////////////////////////////////////////////////////////////////////////

bool ClusterInfo::doesDatabaseExist(DatabaseID const& databaseID, bool reload) {
  int tries = 0;

  if (reload || !_planProt.isValid || !_currentProt.isValid || !_DBServersProt.isValid) {
    loadPlan();
    loadCurrent();
    loadCurrentDBServers();
    ++tries;  // no need to reload if the database is not found
  }

  // From now on we know that all data has been valid once, so no need
  // to check the isValid flags again under the lock.

  while (true) {
    {
      size_t expectedSize;
      {
        READ_LOCKER(readLocker, _DBServersProt.lock);
        expectedSize = _DBServers.size();
      }

      // look up database by name:

      READ_LOCKER(readLocker, _planProt.lock);
      // _plannedDatabases is a map-type<DatabaseID, VPackSlice>
      auto it = _plannedDatabases.find(databaseID);

      if (it != _plannedDatabases.end()) {
        // found the database in Plan
        READ_LOCKER(readLocker, _currentProt.lock);
        // _currentDatabases is
        //     a map-type<DatabaseID, a map-type<ServerID, VPackSlice>>
        auto it2 = _currentDatabases.find(databaseID);

        if (it2 != _currentDatabases.end()) {
          // found the database in Current

          return ((*it2).second.size() >= expectedSize);
        }
      }
    }

    if (++tries >= 2) {
      break;
    }

    loadPlan();
    loadCurrent();
    loadCurrentDBServers();
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get list of databases in the cluster
////////////////////////////////////////////////////////////////////////////////

std::vector<DatabaseID> ClusterInfo::databases(bool reload) {
  std::vector<DatabaseID> result;

  if (_clusterId.empty()) {
    loadClusterId();
  }

  if (reload || !_planProt.isValid || !_currentProt.isValid || !_DBServersProt.isValid) {
    loadPlan();
    loadCurrent();
    loadCurrentDBServers();
  }

  // From now on we know that all data has been valid once, so no need
  // to check the isValid flags again under the lock.

  size_t expectedSize;
  {
    READ_LOCKER(readLocker, _DBServersProt.lock);
    expectedSize = _DBServers.size();
  }

  {
    READ_LOCKER(readLockerPlanned, _planProt.lock);
    READ_LOCKER(readLockerCurrent, _currentProt.lock);
    // _plannedDatabases is a map-type<DatabaseID, VPackSlice>
    auto it = _plannedDatabases.begin();

    while (it != _plannedDatabases.end()) {
      // _currentDatabases is:
      //   a map-type<DatabaseID, a map-type<ServerID, VPackSlice>>
      auto it2 = _currentDatabases.find((*it).first);

      if (it2 != _currentDatabases.end()) {
        if ((*it2).second.size() >= expectedSize) {
          result.push_back((*it).first);
        }
      }

      ++it;
    }
  }
  return result;
}

/// @brief Load cluster ID
void ClusterInfo::loadClusterId() {
  // Contact agency for /<prefix>/Cluster
  AgencyCommResult result = _agency.getValues("Cluster");

  // Parse
  if (result.successful()) {
    VPackSlice slice = result.slice()[0].get(
        std::vector<std::string>({AgencyCommManager::path(), "Cluster"}));
    if (slice.isString()) {
      _clusterId = slice.copyString();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief (re-)load the information about our plan
/// Usually one does not have to call this directly.
////////////////////////////////////////////////////////////////////////////////

static std::string const prefixPlan = "Plan";

void ClusterInfo::loadPlan() {
  DatabaseFeature& databaseFeature = _server.getFeature<DatabaseFeature>();

  ++_planProt.wantedVersion;  // Indicate that after *NOW* somebody has to
                              // reread from the agency!

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto tStart = TRI_microtime();
  auto longPlanWaitLogger = scopeGuard([&tStart]() {
    auto tExit = TRI_microtime();
    if (tExit - tStart > 0.5) {
      LOG_TOPIC("66666", WARN, Logger::CLUSTER) <<
        "Loading the new plan took: " << (tExit - tStart);
    }
  });
#endif

  MUTEX_LOCKER(mutexLocker, _planProt.mutex);  // only one may work at a time

  // For ArangoSearch views we need to get access to immediately created views
  // in order to allow links to be created correctly.
  // For the scenario above, we track such views in '_newPlannedViews' member
  // which is supposed to be empty before and after 'ClusterInfo::loadPlan()'
  // execution. In addition, we do the following "trick" to provide access to
  // '_newPlannedViews' from outside 'ClusterInfo': in case if
  // 'ClusterInfo::getView' has been called from within 'ClusterInfo::loadPlan',
  // we redirect caller to search view in
  // '_newPlannedViews' member instead of '_plannedViews'

  // set plan loader
  TRI_ASSERT(_newPlannedViews.empty());
  _planLoader = std::this_thread::get_id();

  // ensure we'll eventually reset plan loader
  auto resetLoader = scopeGuard([this]() {
    _planLoader = std::thread::id();
    _newPlannedViews.clear();
  });

  bool planValid = true;  // has the loadPlan compleated without skipping valid objects
  uint64_t storedVersion = _planProt.wantedVersion;  // this is the version
                                                     // we will set in the end

  LOG_TOPIC("eb0e4", DEBUG, Logger::CLUSTER)
      << "loadPlan: wantedVersion=" << storedVersion
      << ", doneVersion=" << _planProt.doneVersion;

  if (_planProt.doneVersion == storedVersion) {
    // Somebody else did, what we intended to do, so just return
    return;
  }

  // Now contact the agency:
  AgencyCommResult result = _agency.getValues(prefixPlan);

  if (!result.successful()) {
    LOG_TOPIC("989d5", DEBUG, Logger::CLUSTER)
        << "Error while loading " << prefixPlan
        << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
        << " errorMessage: " << result.errorMessage() << " body: " << result.body();

    return;
  }

  auto resultSlice = result.slice();

  if (!resultSlice.isArray() || resultSlice.length() != 1) {
    LOG_TOPIC("e089b", DEBUG, Logger::CLUSTER)
        << "Error while loading " << prefixPlan << " response structure is not an array of size 1"
        << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
        << " errorMessage: " << result.errorMessage() << " body: " << result.body();

    return;
  }

  auto slice = resultSlice[0].get(  // get slice
      std::vector<std::string>({AgencyCommManager::path(), "Plan"})  // args
  );

  auto planBuilder = std::make_shared<velocypack::Builder>(slice);
  auto planSlice = planBuilder->slice();

  if (!planSlice.isObject()) {
    LOG_TOPIC("bc8e1", ERR, Logger::CLUSTER)
        << "\"Plan\" is not an object in agency";

    return;
  }

  uint64_t newPlanVersion = 0;
  auto planVersionSlice = planSlice.get("Version");

  if (planVersionSlice.isNumber()) {
    try {
      newPlanVersion = planVersionSlice.getNumber<uint64_t>();
    } catch (...) {
    }
  }

  LOG_TOPIC("c6303", TRACE, Logger::CLUSTER) << "loadPlan: newPlanVersion=" << newPlanVersion;

  if (newPlanVersion == 0) {
    LOG_TOPIC("d68ae", WARN, Logger::CLUSTER)
        << "Attention: /arango/Plan/Version in the agency is not set or not a "
           "positive number.";
  }

  {
    READ_LOCKER(guard, _planProt.lock);

    if (_planProt.isValid && newPlanVersion <= _planVersion) {
      LOG_TOPIC("20450", DEBUG, Logger::CLUSTER)
          << "We already know this or a later version, do not update. "
          << "newPlanVersion=" << newPlanVersion << " _planVersion=" << _planVersion;

      return;
    }
  }

  decltype(_plannedDatabases) newDatabases;
  std::set<std::string> buildingDatabases;
  decltype(_plannedCollections) newCollections;  // map<string /*database id*/
                                                 //    ,map<string /*collection id*/
                                                 //        ,shared_ptr<LogicalCollection>
                                                 //        >
                                                 //    >
  decltype(_shards) newShards;
  decltype(_shardServers) newShardServers;
  decltype(_shardToName) newShardToName;

  bool swapDatabases = false;
  bool swapCollections = false;
  bool swapViews = false;

  auto planDatabasesSlice = planSlice.get("Databases");

  if (planDatabasesSlice.isObject()) {
    swapDatabases = true;  // mark for swap even if no databases present to ensure dangling datasources are removed

    std::string name;

    for (auto const& database : velocypack::ObjectIterator(planDatabasesSlice)) {
      try {
        name = database.key.copyString();
      } catch (arangodb::velocypack::Exception const& e) {
        LOG_TOPIC("adc82", ERR, Logger::AGENCY)
            << "Failed to get database name from json, error '" << e.what()
            << "'. VelocyPack: " << database.key.toJson();

        throw;
      }

      // We create the database object on the coordinator here, because
      // it is used to create LogicalCollection instances further
      // down in this function.
      if (ServerState::instance()->isCoordinator() &&
          !database.value.hasKey(StaticStrings::AttrIsBuilding)) {
        TRI_vocbase_t* vocbase = databaseFeature.lookupDatabase(name);
        if (vocbase == nullptr) {
          // database does not yet exist, create it now

          // create a local database object...
          arangodb::CreateDatabaseInfo info(_server);
          Result res = info.load(database.value, VPackSlice::emptyArraySlice());
          if (res.fail()) {
            LOG_TOPIC("94357", ERR, arangodb::Logger::AGENCY)
                << "validating data for local database '" << name
                << "' failed: " << res.errorMessage();
          } else {
            res = databaseFeature.createDatabase(std::move(info), vocbase);

            if (res.fail()) {
              LOG_TOPIC("91870", ERR, arangodb::Logger::AGENCY)
                  << "creating local database '" << name
                  << "' failed: " << res.errorMessage();
            }
          }
        }
      }

      // On a coordinator we can only see databases that have been fully created
      if (!(ServerState::instance()->isCoordinator() &&
            database.value.hasKey(StaticStrings::AttrIsBuilding))) {
        newDatabases.try_emplace(std::move(name), database.value);
      } else {
        buildingDatabases.emplace(std::move(name));
      }
    }
  }

  // Ensure views are being created BEFORE collections to allow
  // links find them
  // Immediate children of "Views" are database names, then ids
  // of views, then one JSON object with the description:

  // "Plan":{"Views": {
  //  "_system": {
  //    "654321": {
  //      "id": "654321",
  //      "name": "v",
  //      "collections": [
  //        <list of cluster-wide collection IDs of linked collections>
  //      ]
  //    },...
  //  },...
  //  }}

  // Now the same for views:
  auto planViewsSlice = planSlice.get("Views");  // format above

  if (planViewsSlice.isObject()) {
    swapViews = true;  // mark for swap even if no databases present to ensure dangling datasources are removed

    for (auto const& databasePairSlice : velocypack::ObjectIterator(planViewsSlice)) {
      auto const& viewsSlice = databasePairSlice.value;

      if (!viewsSlice.isObject()) {
        LOG_TOPIC("0ee7f", INFO, Logger::AGENCY)
            << "Views in the plan is not a valid json object."
            << " Views will be ignored for now and the invalid information"
            << " will be repaired. VelocyPack: " << viewsSlice.toJson();

        continue;
      }

      auto const databaseName = databasePairSlice.key.copyString();
      auto* vocbase = databaseFeature.lookupDatabase(databaseName);

      if (!vocbase) {
        // No database with this name found.
        // We have an invalid state here.
        LOG_TOPIC("f105f", WARN, Logger::AGENCY)
            << "No database '" << databaseName << "' found,"
            << " corresponding view will be ignored for now and the "
            << "invalid information will be repaired. VelocyPack: "
            << viewsSlice.toJson();
        planValid &= !viewsSlice.length();  // cannot find vocbase for defined views (allow empty views for missing vocbase)

        continue;
      }

      for (auto const& viewPairSlice : velocypack::ObjectIterator(viewsSlice)) {
        auto const& viewSlice = viewPairSlice.value;

        if (!viewSlice.isObject()) {
          LOG_TOPIC("2487b", INFO, Logger::AGENCY)
              << "View entry is not a valid json object."
              << " The view will be ignored for now and the invalid "
              << "information will be repaired. VelocyPack: " << viewSlice.toJson();

          continue;
        }

        auto const viewId = viewPairSlice.key.copyString();

        try {
          LogicalView::ptr view;
          auto res = LogicalView::instantiate(  // instantiate
              view, *vocbase, viewPairSlice.value, newPlanVersion  // args
          );

          if (!res.ok() || !view) {
            LOG_TOPIC("b0d48", ERR, Logger::AGENCY)
                << "Failed to create view '" << viewId
                << "'. The view will be ignored for now and the invalid "
                << "information will be repaired. VelocyPack: " << viewSlice.toJson();
            planValid = false;  // view creation failure

            continue;
          }

          auto& views = _newPlannedViews[databaseName];

          // register with guid/id/name
          views.reserve(views.size() + 3);
          views[viewId] = view;
          views[view->name()] = view;
          views[view->guid()] = view;
        } catch (std::exception const& ex) {
          // The Plan contains invalid view information.
          // This should not happen in healthy situations.
          // If it happens in unhealthy situations the
          // cluster should not fail.
          LOG_TOPIC("ec9e6", ERR, Logger::AGENCY)
              << "Failed to load information for view '" << viewId
              << "': " << ex.what() << ". invalid information in Plan. The "
              << "view will be ignored for now and the invalid "
              << "information will be repaired. VelocyPack: " << viewSlice.toJson();

          TRI_ASSERT(false);
          continue;
        } catch (...) {
          // The Plan contains invalid view information.
          // This should not happen in healthy situations.
          // If it happens in unhealthy situations the
          // cluster should not fail.
          LOG_TOPIC("660bf", ERR, Logger::AGENCY)
              << "Failed to load information for view '" << viewId
              << ". invalid information in Plan. The view will "
              << "be ignored for now and the invalid information will "
              << "be repaired. VelocyPack: " << viewSlice.toJson();

          TRI_ASSERT(false);
          continue;
        }
      }
    }
  }

  // Immediate children of "Collections" are database names, then ids
  // of collections, then one JSON object with the description:

  // "Plan":{"Collections": {
  //  "_system": {
  //    "3010001": {
  //      "deleted": false,
  //      DO_COMPACT: true,
  //      "id": "3010001",
  //      INDEX_BUCKETS: 8,
  //      "indexes": [
  //        {
  //          "fields": [
  //            "_key"
  //          ],
  //          "id": "0",
  //          "sparse": false,
  //          "type": "primary",
  //          "unique": true
  //        }
  //      ],
  //      "isSmart": false,
  //      "isSystem": true,
  //      "isVolatile": false,
  //      JOURNAL_SIZE: 1048576,
  //      "keyOptions": {
  //        "allowUserKeys": true,
  //        "lastValue": 0,
  //        "type": "traditional"
  //      },
  //      "name": "_graphs",
  //      "numberOfSh ards": 1,
  //      "path": "",
  //      "replicationFactor": 2,
  //      "shardKeys": [
  //        "_key"
  //      ],
  //      "shards": {
  //        "s3010002": [
  //          "PRMR-bf44d6fe-e31c-4b09-a9bf-e2df6c627999",
  //          "PRMR-11a29830-5aca-454b-a2c3-dac3a08baca1"
  //        ]
  //      },
  //      "status": 3,
  //      "statusString": "loaded",
  //      "type": 2,
  //      StaticStrings::WaitForSyncString: false
  //    },...
  //  },...
  // }}

  auto planCollectionsSlice = planSlice.get("Collections");  // format above

  if (planCollectionsSlice.isObject()) {
    swapCollections = true;  // mark for swap even if no databases present to ensure dangling datasources are removed

    bool const isCoordinator = ServerState::instance()->isCoordinator();

    for (auto const& databasePairSlice : velocypack::ObjectIterator(planCollectionsSlice)) {
      auto const& collectionsSlice = databasePairSlice.value;

      if (!collectionsSlice.isObject()) {
        LOG_TOPIC("e2e7a", INFO, Logger::AGENCY)
            << "Collections in the plan is not a valid json object."
            << " Collections will be ignored for now and the invalid "
            << "information will be repaired. VelocyPack: " << collectionsSlice.toJson();

        continue;
      }

      DatabaseCollections databaseCollections;
      auto const databaseName = databasePairSlice.key.copyString();

      // Skip databases that are still building.
      if (buildingDatabases.find(databaseName) != buildingDatabases.end()) {
        continue;
      }
      auto* vocbase = databaseFeature.lookupDatabase(databaseName);

      if (!vocbase) {
        // No database with this name found.
        // We have an invalid state here.
        LOG_TOPIC("83d4c", WARN, Logger::AGENCY)
            << "No database '" << databaseName << "' found,"
            << " corresponding collection will be ignored for now and the "
            << "invalid information will be repaired. VelocyPack: "
            << collectionsSlice.toJson();
        planValid &= !collectionsSlice.length();  // cannot find vocbase for defined collections (allow empty collections for missing vocbase)

        continue;
      }

      for (auto const& collectionPairSlice : velocypack::ObjectIterator(collectionsSlice)) {
        auto const& collectionSlice = collectionPairSlice.value;

        if (!collectionSlice.isObject()) {
          LOG_TOPIC("0f689", WARN, Logger::AGENCY)
              << "Collection entry is not a valid json object."
              << " The collection will be ignored for now and the invalid "
              << "information will be repaired. VelocyPack: " << collectionSlice.toJson();

          continue;
        }

        auto const collectionId = collectionPairSlice.key.copyString();

        try {
          std::shared_ptr<LogicalCollection> newCollection;

#ifdef USE_ENTERPRISE
          auto isSmart = collectionSlice.get(StaticStrings::IsSmart);

          if (isSmart.isTrue()) {
            auto type = collectionSlice.get(StaticStrings::DataSourceType);

            if (type.isInteger() && type.getUInt() == TRI_COL_TYPE_EDGE) {
              newCollection = std::make_shared<VirtualSmartEdgeCollection>(  // create collection
                  *vocbase, collectionSlice, newPlanVersion  // args
              );
            } else {
              newCollection = std::make_shared<SmartVertexCollection>(  // create collection
                  *vocbase, collectionSlice, newPlanVersion  // args
              );
            }
          } else
#endif
          {
            newCollection = std::make_shared<LogicalCollection>(  // create collection
                *vocbase, collectionSlice, true, newPlanVersion  // args
            );
          }

          auto& collectionName = newCollection->name();

          bool isBuilding = isCoordinator &&
                            arangodb::basics::VelocyPackHelper::getBooleanValue(
                                collectionSlice, StaticStrings::AttrIsBuilding, false);
          if (isCoordinator) {
            // copying over index estimates from the old version of the
            // collection into the new one
            LOG_TOPIC("7a884", TRACE, Logger::CLUSTER)
                << "copying index estimates";

            // it is effectively safe to access _plannedCollections in
            // read-only mode here, as the only places that modify
            // _plannedCollections are the shutdown and this function
            // itself, which is protected by a mutex
            auto it = _plannedCollections.find(databaseName);
            if (it != _plannedCollections.end()) {
              auto it2 = (*it).second.find(collectionId);

              if (it2 != (*it).second.end()) {
                try {
                  auto estimates = (*it2).second->clusterIndexEstimates(false);

                  if (!estimates.empty()) {
                    // already have an estimate... now copy it over
                    newCollection->setClusterIndexEstimates(std::move(estimates));
                  }
                } catch (...) {
                  // this may fail during unit tests, when mocks are used
                }
              }
            }
          }

          // NOTE: This is building has the following feature. A collection needs to be working on
          // all DBServers to allow replication to go on, also we require to have the shards planned.
          // BUT: The users should not be able to detect these collections.
          // Hence we simply do NOT add the collection to the coordinator local vocbase, which happens
          // inside the IF

          if (!isBuilding) {
            // register with name as well as with id:
            databaseCollections.try_emplace(collectionName, newCollection);
            databaseCollections.try_emplace(collectionId, newCollection);
          }

          auto shardIDs = newCollection->shardIds();
          auto shards = std::make_shared<std::vector<std::string>>();
          shards->reserve(shardIDs->size());
          newShardToName.reserve(shardIDs->size());

          for (auto const& p : *shardIDs) {
            TRI_ASSERT(p.first.size() >= 2);
            shards->push_back(p.first);
            newShardServers.try_emplace(p.first, p.second);
            newShardToName.try_emplace(p.first, newCollection->name());
          }

          // Sort by the number in the shard ID ("s0000001" for example):
          ShardingInfo::sortShardNamesNumerically(*shards);
          newShards.try_emplace(collectionId, std::move(shards));
        } catch (std::exception const& ex) {
          // The plan contains invalid collection information.
          // This should not happen in healthy situations.
          // If it happens in unhealthy situations the
          // cluster should not fail.
          LOG_TOPIC("359f3", ERR, Logger::AGENCY)
              << "Failed to load information for collection '" << collectionId
              << "': " << ex.what() << ". invalid information in plan. The "
              << "collection will be ignored for now and the invalid "
              << "information will be repaired. VelocyPack: " << collectionSlice.toJson();

          TRI_ASSERT(false);
          continue;
        } catch (...) {
          // The plan contains invalid collection information.
          // This should not happen in healthy situations.
          // If it happens in unhealthy situations the
          // cluster should not fail.
          LOG_TOPIC("5f3d5", ERR, Logger::AGENCY)
              << "Failed to load information for collection '" << collectionId
              << ". invalid information in plan. The collection will "
              << "be ignored for now and the invalid information will "
              << "be repaired. VelocyPack: " << collectionSlice.toJson();

          TRI_ASSERT(false);
          continue;
        }
      }

      newCollections.try_emplace(std::move(databaseName), std::move(databaseCollections));
    }
    LOG_TOPIC("12dfa", DEBUG, Logger::CLUSTER)
        << "loadPlan done: wantedVersion=" << storedVersion
        << ", doneVersion=" << _planProt.doneVersion;
  }

  if (ServerState::instance()->isCoordinator()) {
    auto systemDB = _server.getFeature<arangodb::SystemDatabaseFeature>().use();
    if (systemDB && systemDB->shardingPrototype() == ShardingPrototype::Undefined) {
      // sharding prototype of _system database defaults to _users nowadays
      systemDB->setShardingPrototype(ShardingPrototype::Users);
      // but for "old" databases it may still be "_graphs". we need to find out!
      // find _system database in Plan
      auto it = newCollections.find(StaticStrings::SystemDatabase);
      if (it != newCollections.end()) {
        // find _graphs collection in Plan
        auto it2 = (*it).second.find(StaticStrings::GraphCollection);
        if (it2 != (*it).second.end()) {
          // found!
          if ((*it2).second->distributeShardsLike().empty()) {
            // _graphs collection has no distributeShardsLike, so it is
            // the prototype!
            systemDB->setShardingPrototype(ShardingPrototype::Graphs);
          }
        }
      }
    }
  }

  WRITE_LOCKER(writeLocker, _planProt.lock);

  _plan = std::move(planBuilder);
  _planVersion = newPlanVersion;

  if (swapDatabases) {
    _plannedDatabases.swap(newDatabases);
  }

  if (swapCollections) {
    _plannedCollections.swap(newCollections);
    _shards.swap(newShards);
    _shardServers.swap(newShardServers);
    _shardToName.swap(newShardToName);
  }

  if (swapViews) {
    _plannedViews.swap(_newPlannedViews);
  }

  // mark plan as fully loaded only if all incoming objects were fully loaded
  // must still swap structures to allow creation of new vocbases and removal of stale datasources
  if (planValid) {
    _planProt.doneVersion = storedVersion;
    _planProt.isValid = true;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief (re-)load the information about current databases
/// Usually one does not have to call this directly.
////////////////////////////////////////////////////////////////////////////////

static std::string const prefixCurrent = "Current";

void ClusterInfo::loadCurrent() {
  // We need to update ServersKnown to notice rebootId changes for all servers.
  // To keep things simple and separate, we call loadServers here instead of
  // trying to integrate the servers upgrade code into loadCurrent, even if that
  // means small bits of the plan are read twice.
  loadServers();

  ++_currentProt.wantedVersion;  // Indicate that after *NOW* somebody has to
                                 // reread from the agency!

  MUTEX_LOCKER(mutexLocker, _currentProt.mutex);  // only one may work at a time
  uint64_t storedVersion = _currentProt.wantedVersion;  // this is the version

  // we will set at the end
  if (_currentProt.doneVersion == storedVersion) {
    // Somebody else did, what we intended to do, so just return
    return;
  }

  LOG_TOPIC("54789", DEBUG, Logger::CLUSTER)
      << "loadCurrent: wantedVersion: " << _currentProt.wantedVersion;

  // Now contact the agency:
  AgencyCommResult result = _agency.getValues(prefixCurrent);

  if (!result.successful()) {
    LOG_TOPIC("5d4e4", DEBUG, Logger::CLUSTER)
        << "Error while loading " << prefixCurrent
        << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
        << " errorMessage: " << result.errorMessage() << " body: " << result.body();

    return;
  }

  auto resultSlice = result.slice();

  if (!resultSlice.isArray() || resultSlice.length() != 1) {
    LOG_TOPIC("b020c", DEBUG, Logger::CLUSTER)
        << "Error while loading " << prefixCurrent << " response structure is not an array of size 1"
        << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
        << " errorMessage: " << result.errorMessage() << " body: " << result.body();

    return;
  }

  auto slice = resultSlice[0].get(  // get slice
      std::vector<std::string>({AgencyCommManager::path(), "Current"})  // args
  );
  auto currentBuilder = std::make_shared<velocypack::Builder>();

  currentBuilder->add(slice);

  auto currentSlice = currentBuilder->slice();

  if (!currentSlice.isObject()) {
    LOG_TOPIC("b8410", ERR, Logger::CLUSTER) << "Current is not an object!";

    LOG_TOPIC("eed43", DEBUG, Logger::CLUSTER) << "loadCurrent done.";

    return;
  }

  uint64_t newCurrentVersion = 0;
  auto currentVersionSlice = currentSlice.get("Version");

  if (currentVersionSlice.isNumber()) {
    try {
      newCurrentVersion = currentVersionSlice.getNumber<uint64_t>();
    } catch (...) {
    }
  }

  if (newCurrentVersion == 0) {
    LOG_TOPIC("e088e", WARN, Logger::CLUSTER)
        << "Attention: /arango/Current/Version in the agency is not set or not "
           "a positive number.";
  }

  {
    READ_LOCKER(guard, _currentProt.lock);

    if (_currentProt.isValid && newCurrentVersion <= _currentVersion) {
      LOG_TOPIC("00d58", DEBUG, Logger::CLUSTER)
          << "We already know this or a later version, do not update. "
          << "newCurrentVersion=" << newCurrentVersion
          << " _currentVersion=" << _currentVersion;

      return;
    }
  }

  decltype(_currentDatabases) newDatabases;
  decltype(_currentCollections) newCollections;
  decltype(_shardIds) newShardIds;

  bool swapDatabases = false;
  bool swapCollections = false;

  auto currentDatabasesSlice = currentSlice.get("Databases");

  if (currentDatabasesSlice.isObject()) {
    swapDatabases = true;

    for (auto const& databaseSlicePair : velocypack::ObjectIterator(currentDatabasesSlice)) {
      auto const database = databaseSlicePair.key.copyString();

      if (!databaseSlicePair.value.isObject()) {
        continue;
      }

      std::unordered_map<ServerID, velocypack::Slice> serverList;

      for (auto const& serverSlicePair :
           velocypack::ObjectIterator(databaseSlicePair.value)) {
        serverList.try_emplace(serverSlicePair.key.copyString(), serverSlicePair.value);
      }

      newDatabases.try_emplace(database, serverList);
    }
  }

  auto currentCollectionsSlice = currentSlice.get("Collections");

  if (currentCollectionsSlice.isObject()) {
    swapCollections = true;

    for (auto const& databaseSlice : velocypack::ObjectIterator(currentCollectionsSlice)) {
      auto const databaseName = databaseSlice.key.copyString();
      DatabaseCollectionsCurrent databaseCollections;

      for (auto const& collectionSlice :
           velocypack::ObjectIterator(databaseSlice.value)) {
        auto const collectionName = collectionSlice.key.copyString();

        auto collectionDataCurrent =
            std::make_shared<CollectionInfoCurrent>(newCurrentVersion);

        for (auto const& shardSlice : velocypack::ObjectIterator(collectionSlice.value)) {
          auto const shardID = shardSlice.key.copyString();

          collectionDataCurrent->add(shardID, shardSlice.value);

          // Note that we have only inserted the CollectionInfoCurrent under
          // the collection ID and not under the name! It is not possible
          // to query the current collection info by name. This is because
          // the correct place to hold the current name is in the plan.
          // Thus: Look there and get the collection ID from there. Then
          // ask about the current collection info.

          // Now take note of this shard and its responsible server:
          auto servers = std::make_shared<std::vector<ServerID>>(
              collectionDataCurrent->servers(shardID)  // args
          );

          newShardIds.try_emplace(shardID, servers);
        }

        databaseCollections.try_emplace(collectionName, collectionDataCurrent);
      }

      newCollections.try_emplace(databaseName, databaseCollections);
    }
  }

  // Now set the new value:
  WRITE_LOCKER(writeLocker, _currentProt.lock);

  _current = currentBuilder;
  _currentVersion = newCurrentVersion;

  if (swapDatabases) {
    _currentDatabases.swap(newDatabases);
  }

  if (swapCollections) {
    LOG_TOPIC("b4059", TRACE, Logger::CLUSTER)
        << "Have loaded new collections current cache!";
    _currentCollections.swap(newCollections);
    _shardIds.swap(newShardIds);
  }

  _currentProt.doneVersion = storedVersion;
  _currentProt.isValid = true;
}

/// @brief ask about a collection
/// If it is not found in the cache, the cache is reloaded once
/// if the collection is not found afterwards, this method will throw an
/// exception

std::shared_ptr<LogicalCollection> ClusterInfo::getCollection(DatabaseID const& databaseID,
                                                              CollectionID const& collectionID) {
  auto c = getCollectionNT(databaseID, collectionID);

  if (c == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                   getCollectionNotFoundMsg(databaseID, collectionID)  // message
    );
  } else {
    return c;
  }
}

std::shared_ptr<LogicalCollection> ClusterInfo::getCollectionNT(DatabaseID const& databaseID,
                                                                CollectionID const& collectionID) {
  int tries = 0;

  if (!_planProt.isValid) {
    loadPlan();
    ++tries;
  }

  while (true) {  // left by break
    {
      READ_LOCKER(readLocker, _planProt.lock);
      // look up database by id
      AllCollections::const_iterator it = _plannedCollections.find(databaseID);

      if (it != _plannedCollections.end()) {
        // look up collection by id (or by name)
        DatabaseCollections::const_iterator it2 = (*it).second.find(collectionID);

        if (it2 != (*it).second.end()) {
          return (*it2).second;
        }
      }
    }
    if (++tries >= 2) {
      break;
    }

    // must load collections outside the lock
    loadPlan();
  }
  return nullptr;
}

std::string ClusterInfo::getCollectionNotFoundMsg(DatabaseID const& databaseID,
                                                  CollectionID const& collectionID) {
  return "Collection not found: " + collectionID + " in database " + databaseID;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ask about all collections
////////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<LogicalCollection>> const ClusterInfo::getCollections(DatabaseID const& databaseID) {
  std::vector<std::shared_ptr<LogicalCollection>> result;

  // always reload
  loadPlan();

  READ_LOCKER(readLocker, _planProt.lock);
  // look up database by id
  AllCollections::const_iterator it = _plannedCollections.find(databaseID);

  if (it == _plannedCollections.end()) {
    return result;
  }

  // iterate over all collections
  DatabaseCollections::const_iterator it2 = (*it).second.begin();
  while (it2 != (*it).second.end()) {
    char c = (*it2).first[0];

    if (c < '0' || c > '9') {
      // skip collections indexed by id
      result.push_back((*it2).second);
    }

    ++it2;
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ask about a collection in current. This returns information about
/// all shards in the collection.
/// If it is not found in the cache, the cache is reloaded once.
////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<CollectionInfoCurrent> ClusterInfo::getCollectionCurrent(
    DatabaseID const& databaseID, CollectionID const& collectionID) {
  int tries = 0;

  if (!_currentProt.isValid) {
    loadCurrent();
    ++tries;
  }

  while (true) {
    {
      READ_LOCKER(readLocker, _currentProt.lock);
      // look up database by id
      AllCollectionsCurrent::const_iterator it = _currentCollections.find(databaseID);

      if (it != _currentCollections.end()) {
        // look up collection by id
        DatabaseCollectionsCurrent::const_iterator it2 = (*it).second.find(collectionID);

        if (it2 != (*it).second.end()) {
          return (*it2).second;
        }
      }
    }

    if (++tries >= 2) {
      break;
    }

    // must load collections outside the lock
    loadCurrent();
  }

  return std::make_shared<CollectionInfoCurrent>(0);
}

RebootTracker& ClusterInfo::rebootTracker() noexcept { return _rebootTracker; }

RebootTracker const& ClusterInfo::rebootTracker() const noexcept {
  return _rebootTracker;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief ask about a view
/// If it is not found in the cache, the cache is reloaded once. The second
/// argument can be a view ID or a view name (both cluster-wide).
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<LogicalView> ClusterInfo::getView(DatabaseID const& databaseID,
                                                  ViewID const& viewID) {
  if (viewID.empty()) {
    return nullptr;
  }

  auto lookupView = [](AllViews const& dbs, DatabaseID const& databaseID,
                       ViewID const& viewID) noexcept -> std::shared_ptr<LogicalView> {
    // look up database by id
    auto const db = dbs.find(databaseID);

    if (db != dbs.end()) {
      // look up view by id (or by name)
      auto& views = db->second;
      auto const view = views.find(viewID);

      if (view != views.end()) {
        return view->second;
      }
    }

    return nullptr;
  };

  if (std::this_thread::get_id() == _planLoader) {
    // we're loading plan, lookup inside immediately created planned views
    // already protected by _planProt.mutex, don't need to lock there
    return lookupView(_newPlannedViews, databaseID, viewID);
  }

  int tries = 0;

  if (!_planProt.isValid) {
    loadPlan();
    ++tries;
  }

  while (true) {  // left by break
    {
      READ_LOCKER(readLocker, _planProt.lock);
      auto const view = lookupView(_plannedViews, databaseID, viewID);

      if (view) {
        return view;
      }
    }
    if (++tries >= 2) {
      break;
    }

    // must load plan outside the lock
    loadPlan();
  }

  LOG_TOPIC("a227e", DEBUG, Logger::CLUSTER)
      << "View not found: '" << viewID << "' in database '" << databaseID << "'";

  return nullptr;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief ask about all views of a database
//////////////////////////////////////////////////////////////////////////////

std::vector<std::shared_ptr<LogicalView>> const ClusterInfo::getViews(DatabaseID const& databaseID) {
  std::vector<std::shared_ptr<LogicalView>> result;

  // always reload
  loadPlan();

  READ_LOCKER(readLocker, _planProt.lock);
  // look up database by id
  AllViews::const_iterator it = _plannedViews.find(databaseID);

  if (it == _plannedViews.end()) {
    return result;
  }

  // iterate over all collections
  DatabaseViews::const_iterator it2 = (*it).second.begin();
  while (it2 != it->second.end()) {
    char c = it2->first[0];
    if (c >= '0' && c <= '9') {
      // skip views indexed by name
      result.emplace_back(it2->second);
    }
    ++it2;
  }

  return result;
}

// Build the VPackSlice that contains the `isBuilding` entry
void ClusterInfo::buildIsBuildingSlice(CreateDatabaseInfo const& database,
                                       VPackBuilder& builder) {
  VPackObjectBuilder guard(&builder);
  database.toVelocyPack(builder);

  builder.add(StaticStrings::AttrCoordinator,
              VPackValue(ServerState::instance()->getId()));
  builder.add(StaticStrings::AttrCoordinatorRebootId,
              VPackValue(ServerState::instance()->getRebootId().value()));
  builder.add(StaticStrings::AttrIsBuilding, VPackValue(true));
}

// Build the VPackSlice that does not contain  the `isBuilding` entry
void ClusterInfo::buildFinalSlice(CreateDatabaseInfo const& database,
                                  VPackBuilder& builder) {
  VPackObjectBuilder guard(&builder);
  database.toVelocyPack(builder);
}

// This waits for the database described in `database` to turn up in `Current`
// and no DBServer is allowed to report an error.
Result ClusterInfo::waitForDatabaseInCurrent(CreateDatabaseInfo const& database) {
  AgencyComm ac(_server);
  AgencyCommResult res;

  auto DBServers = std::make_shared<std::vector<ServerID>>(getCurrentDBServers());
  auto dbServerResult = std::make_shared<std::atomic<int>>(-1);
  std::shared_ptr<std::string> errMsg = std::make_shared<std::string>();

  std::function<bool(VPackSlice const& result)> dbServerChanged = [=](VPackSlice const& result) {
    size_t numDbServers = DBServers->size();
    if (result.isObject() && result.length() >= numDbServers) {
      // We use >= here since the number of DBservers could have increased
      // during the creation of the database and we might not yet have
      // the latest list. Thus there could be more reports than we know
      // servers.
      VPackObjectIterator dbs(result);

      std::string tmpMsg = "";
      bool tmpHaveError = false;

      for (VPackObjectIterator::ObjectPair dbserver : dbs) {
        VPackSlice slice = dbserver.value;
        if (arangodb::basics::VelocyPackHelper::getBooleanValue(slice, StaticStrings::Error, false)) {
          tmpHaveError = true;
          tmpMsg += " DBServer:" + dbserver.key.copyString() + ":";
          tmpMsg += arangodb::basics::VelocyPackHelper::getStringValue(slice, StaticStrings::ErrorMessage,
                                                                       "");
          if (slice.hasKey(StaticStrings::ErrorNum)) {
            VPackSlice errorNum = slice.get(StaticStrings::ErrorNum);
            if (errorNum.isNumber()) {
              tmpMsg += " (errorNum=";
              tmpMsg += basics::StringUtils::itoa(errorNum.getNumericValue<uint32_t>());
              tmpMsg += ")";
            }
          }
        }
      }
      if (tmpHaveError) {
        *errMsg = "Error in creation of database:" + tmpMsg;
        dbServerResult->store(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE,
                              std::memory_order_release);
        return true;
      }
      dbServerResult->store(setErrormsg(TRI_ERROR_NO_ERROR, *errMsg), std::memory_order_release);
    }
    return true;
  };

  // ATTENTION: The following callback calls the above closure in a
  // different thread. Nevertheless, the closure accesses some of our
  // local variables. Therefore we have to protect all accesses to them
  // by a mutex. We use the mutex of the condition variable in the
  // AgencyCallback for this.
  auto agencyCallback =
      std::make_shared<AgencyCallback>(_server, "Current/Databases/" + database.getName(),
                                       dbServerChanged, true, false);
  _agencyCallbackRegistry->registerCallback(agencyCallback);
  auto cbGuard = scopeGuard(
      [&] { _agencyCallbackRegistry->unregisterCallback(agencyCallback); });

  // Waits for the database to turn up in Current/Databases
  {
    double const interval = getPollInterval();

    CONDITION_LOCKER(locker, agencyCallback->_cv);

    int count = 0;  // this counts, when we have to reload the DBServers
    while (true) {
      if (++count >= static_cast<int>(getReloadServerListTimeout() / interval)) {
        // We update the list of DBServers every minute in case one of them
        // was taken away since we last looked. This also helps (slightly)
        // if a new DBServer was added. However, in this case we report
        // success a bit too early, which is not too bad.
        loadCurrentDBServers();
        *DBServers = getCurrentDBServers();
        count = 0;
      }

      int tmpRes = dbServerResult->load(std::memory_order_acquire);

      // An error was detected on one of the DBServers
      if (tmpRes >= 0) {
        cbGuard.fire();  // unregister cb before accessing errMsg
        loadCurrent();   // update our cache

        return Result(tmpRes, *errMsg);
      }

      agencyCallback->executeByCallbackOrTimeout(getReloadServerListTimeout() / interval);

      if (_server.isStopping()) {
        return Result(TRI_ERROR_SHUTTING_DOWN);
      }
    }
  }
}

// Start creating a database in a coordinator by entering it into Plan/Databases with,
// status flag `isBuilding`; this makes the database invisible to the outside world.
Result ClusterInfo::createIsBuildingDatabaseCoordinator(CreateDatabaseInfo const& database) {
  AgencyComm ac(_server);
  AgencyCommResult res;

  // Instruct the Agency to enter the creation of the new database
  // by entering it into Plan/Databases/ but with the fields
  // isBuilding: true, and coordinator and rebootId set to our respective
  // id and rebootId
  VPackBuilder builder;
  buildIsBuildingSlice(database, builder);

  AgencyWriteTransaction trx(
      {AgencyOperation("Plan/Databases/" + database.getName(),
                       AgencyValueOperationType::SET, builder.slice()),
       AgencyOperation("Plan/Version", AgencySimpleOperationType::INCREMENT_OP)},
      AgencyPrecondition("Plan/Databases/" + database.getName(),
                         AgencyPrecondition::Type::EMPTY, true));

  // TODO: Should this never timeout?
  res = ac.sendTransactionWithFailover(trx, 0.0);

  if (!res.successful()) {
    if (res._statusCode == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
      return Result(TRI_ERROR_ARANGO_DUPLICATE_NAME, std::string("duplicate database name '") + database.getName() + "'");
    }

    return Result(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE_IN_PLAN);
  }

  // Now update our own cache of planned databases:
  loadPlan();

  // And wait for our database to show up in `Current/Databases`
  auto waitresult = waitForDatabaseInCurrent(database);

  if (waitresult.fail()) {
    // cleanup: remove database from plan
    auto ret = cancelCreateDatabaseCoordinator(database);

    if (ret.ok()) {
      // Creation failed
      return Result(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE,
                    "database creation failed");
    } else {
      // Cleanup failed too
      return ret;
    }
  }
  return Result();
}

// Finalize creation of database in cluster by removing isBuilding, coordinator, and coordinatorRebootId;
// as precondition that the entry we put in createIsBuildingDatabaseCoordinator is still in Plan/ unchanged.
Result ClusterInfo::createFinalizeDatabaseCoordinator(CreateDatabaseInfo const& database) {
  AgencyComm ac(_server);

  VPackBuilder pcBuilder;
  buildIsBuildingSlice(database, pcBuilder);

  VPackBuilder entryBuilder;
  buildFinalSlice(database, entryBuilder);

  AgencyWriteTransaction trx(
      {AgencyOperation("Plan/Databases/" + database.getName(),
                       AgencyValueOperationType::SET, entryBuilder.slice()),
       AgencyOperation("Plan/Version", AgencySimpleOperationType::INCREMENT_OP)},
      AgencyPrecondition("Plan/Databases/" + database.getName(),
                         AgencyPrecondition::Type::VALUE, pcBuilder.slice()));

  auto res = ac.sendTransactionWithFailover(trx, 0.0);

  if (!res.successful()) {
    if (res._statusCode == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
      return Result(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE,
                    "Could not finish creation of database: Plan/Databases/ "
                    "entry was modified in Agency");
    }
    // Something else went wrong.
    return Result(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_DATABASE);
  }

  // Make sure we're all aware of collections that have been created
  loadPlan();

  // The transaction was successful and the database should
  // now be visible and usable.
  return Result();
}

// This function can only return on success or when the cluster is shutting down.
Result ClusterInfo::cancelCreateDatabaseCoordinator(CreateDatabaseInfo const& database) {
  AgencyComm ac(_server);

  VPackBuilder builder;
  buildIsBuildingSlice(database, builder);

  // delete all collections and the database itself from the agency plan
  AgencyOperation delPlanCollections("Plan/Collections/" + database.getName(),
                                     AgencySimpleOperationType::DELETE_OP);
  AgencyOperation delPlanDatabase("Plan/Databases/" + database.getName(),
                                  AgencySimpleOperationType::DELETE_OP);
  AgencyOperation incrPlan("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);
  AgencyPrecondition preCondition("Plan/Databases/" + database.getName(),
                                  AgencyPrecondition::Type::VALUE, builder.slice());

  AgencyWriteTransaction trx({delPlanCollections, delPlanDatabase, incrPlan}, preCondition);

  size_t tries = 0;
  // TODO: Hard coded timeout, also the retry numbers below are all
  //       pulled out of thin air.
  double nextTimeout = 0.5;

  AgencyCommResult res;
  do {
    tries++;
    res = ac.sendTransactionWithFailover(trx, nextTimeout);

    if (!res.successful()) {
      if (tries == 1) {
        events::CreateDatabase(database.getName(), res.errorCode());
      }
      if (tries >= 5) {
        nextTimeout = 5.0;
      }
      LOG_TOPIC("b47aa", WARN, arangodb::Logger::CLUSTER)
        << "failed to cancel creation of database " << database.getName() << " with error "
        << res.errorMessage() << ". Retrying.";
    }

    if (_server.isStopping()) {
      return Result(TRI_ERROR_SHUTTING_DOWN);
    }
  } while(!res.successful());

  return Result();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief drop database in coordinator, the return value is an ArangoDB
/// error code and the errorMsg is set accordingly. One possible error
/// is a timeout, a timeout of 0.0 means no timeout.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::dropDatabaseCoordinator(  // drop database
    std::string const& name,                  // database name
    double timeout                            // request timeout
) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  if (name == TRI_VOC_SYSTEM_DATABASE) {
    return Result(TRI_ERROR_FORBIDDEN);
  }

  AgencyComm ac(_server);

  double const realTimeout = getTimeout(timeout);
  double const endTime = TRI_microtime() + realTimeout;
  double const interval = getPollInterval();

  auto dbServerResult = std::make_shared<std::atomic<int>>(-1);
  std::function<bool(VPackSlice const& result)> dbServerChanged = [=](VPackSlice const& result) {
    if (result.isNone() || result.isEmptyObject()) {
      dbServerResult->store(TRI_ERROR_NO_ERROR, std::memory_order_release);
    }
    return true;
  };

  std::string where("Current/Databases/" + name);

  // ATTENTION: The following callback calls the above closure in a
  // different thread. Nevertheless, the closure accesses some of our
  // local variables. Therefore we have to protect all accesses to them
  // by a mutex. We use the mutex of the condition variable in the
  // AgencyCallback for this.
  auto agencyCallback =
      std::make_shared<AgencyCallback>(_server, where, dbServerChanged, true, false);
  _agencyCallbackRegistry->registerCallback(agencyCallback);
  auto cbGuard = scopeGuard([this, &agencyCallback]() -> void {
    _agencyCallbackRegistry->unregisterCallback(agencyCallback);
  });

  // Transact to agency
  AgencyOperation delPlanDatabases("Plan/Databases/" + name,
                                   AgencySimpleOperationType::DELETE_OP);
  AgencyOperation delPlanCollections("Plan/Collections/" + name,
                                     AgencySimpleOperationType::DELETE_OP);
  AgencyOperation delPlanViews("Plan/Views/" + name, AgencySimpleOperationType::DELETE_OP);
  AgencyOperation incrementVersion("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);
  AgencyPrecondition databaseExists("Plan/Databases/" + name,
                                    AgencyPrecondition::Type::EMPTY, false);
  AgencyWriteTransaction trans({delPlanDatabases, delPlanCollections, delPlanViews, incrementVersion},
                               databaseExists);
  AgencyCommResult res = ac.sendTransactionWithFailover(trans);

  if (!res.successful()) {
    if (res._statusCode == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
      return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    }

    return Result(TRI_ERROR_CLUSTER_COULD_NOT_REMOVE_DATABASE_IN_PLAN);
  }

  // Load our own caches:
  loadPlan();

  // Now wait stuff in Current to disappear and thus be complete:
  {
    CONDITION_LOCKER(locker, agencyCallback->_cv);

    while (true) {
      if (dbServerResult->load(std::memory_order_acquire) >= 0) {
        cbGuard.fire();  // unregister cb before calling ac.removeValues(...)
        res = ac.removeValues(where, true);

        if (res.successful()) {
          return Result(TRI_ERROR_NO_ERROR);
        }

        return Result(TRI_ERROR_CLUSTER_COULD_NOT_REMOVE_DATABASE_IN_CURRENT);
      }

      if (TRI_microtime() > endTime) {
        logAgencyDump();
        return Result(TRI_ERROR_CLUSTER_TIMEOUT);
      }

      agencyCallback->executeByCallbackOrTimeout(interval);

      if (_server.isStopping()) {
        return Result(TRI_ERROR_SHUTTING_DOWN);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create collection in coordinator, the return value is an ArangoDB
/// error code and the errorMsg is set accordingly. One possible error
/// is a timeout, a timeout of 0.0 means no timeout.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::createCollectionCoordinator(  // create collection
    std::string const& databaseName, std::string const& collectionID, uint64_t numberOfShards,
    uint64_t replicationFactor, uint64_t writeConcern, bool waitForReplication,
    velocypack::Slice const& json,  // collection definition
    double timeout,                 // request timeout,
    bool isNewDatabase, std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  auto serverState = ServerState::instance();
  std::vector<ClusterCollectionCreationInfo> infos{ClusterCollectionCreationInfo{
      collectionID, numberOfShards, replicationFactor, writeConcern, waitForReplication,
      json, serverState->getId(), serverState->getRebootId()}};
  double const realTimeout = getTimeout(timeout);
  double const endTime = TRI_microtime() + realTimeout;
  return createCollectionsCoordinator(databaseName, infos, endTime,
                                      isNewDatabase, colToDistributeShardsLike);
}

/// @brief this method does an atomic check of the preconditions for the
/// collections to be created, using the currently loaded plan. it populates the
/// plan version used for the checks
Result ClusterInfo::checkCollectionPreconditions(std::string const& databaseName,
                                                 std::vector<ClusterCollectionCreationInfo> const& infos,
                                                 uint64_t& planVersion) {
  for (auto const& info : infos) {
    // Check if name exists.
    if (info.name.empty() || !info.json.isObject() || !info.json.get("shards").isObject()) {
      return TRI_ERROR_BAD_PARAMETER;  // must not be empty
    }

    // Validate that the collection does not exist in the current plan
    {
      AllCollections::const_iterator it = _plannedCollections.find(databaseName);
      if (it != _plannedCollections.end()) {
        DatabaseCollections::const_iterator it2 = (*it).second.find(info.name);

        if (it2 != (*it).second.end()) {
          // collection already exists!
          events::CreateCollection(databaseName, info.name, TRI_ERROR_ARANGO_DUPLICATE_NAME);
          return Result(TRI_ERROR_ARANGO_DUPLICATE_NAME, std::string("duplicate collection name '") + info.name + "'");
        }
      } else {
        // no collection in plan for this particular database... this may be true for
        // the first collection created in a db
        // now check if there is a planned database at least
        if (_plannedDatabases.find(databaseName) == _plannedDatabases.end()) {
          // no need to create a collection in a database that is not there (anymore)
          events::CreateCollection(databaseName, info.name, TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
          return TRI_ERROR_ARANGO_DATABASE_NOT_FOUND;
        }
      }
    }

    // Validate that there is no view with this name either
    {
      // check against planned views as well
      AllViews::const_iterator it = _plannedViews.find(databaseName);
      if (it != _plannedViews.end()) {
        DatabaseViews::const_iterator it2 = (*it).second.find(info.name);

        if (it2 != (*it).second.end()) {
          // view already exists!
          events::CreateCollection(databaseName, info.name, TRI_ERROR_ARANGO_DUPLICATE_NAME);
          return Result(TRI_ERROR_ARANGO_DUPLICATE_NAME, std::string("duplicate collection name '") + info.name + "'");
        }
      }
    }
  }

  return {};
}

Result ClusterInfo::createCollectionsCoordinator(
    std::string const& databaseName, std::vector<ClusterCollectionCreationInfo>& infos,
    double endTime, bool isNewDatabase,
    std::shared_ptr<LogicalCollection> const& colToDistributeShardsLike) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  using arangodb::velocypack::Slice;

  double const interval = getPollInterval();

  // The following three are used for synchronization between the callback
  // closure and the main thread executing this function. Note that it can
  // happen that the callback is called only after we return from this
  // function!
  auto dbServerResult = std::make_shared<std::atomic<int>>(-1);
  auto nrDone = std::make_shared<std::atomic<size_t>>(0);
  auto errMsg = std::make_shared<std::string>();
  auto cacheMutex = std::make_shared<Mutex>();
  auto cacheMutexOwner = std::make_shared<std::atomic<std::thread::id>>();
  auto isCleaned = std::make_shared<bool>(false);

  AgencyComm ac(_server);
  std::vector<std::shared_ptr<AgencyCallback>> agencyCallbacks;

  auto cbGuard = scopeGuard([&] {
    // We have a subtle race here, that we try to cover against:
    // We register a callback in the agency.
    // For some reason this scopeguard is executed (e.g. error case)
    // While we are in this cleanup, and before a callback is removed from the
    // agency. The callback is triggered by another thread. We have the
    // following guarantees: a) cacheMutex|Owner are valid and locked by cleanup
    // b) isCleaned is valid and now set to true
    // c) the closure is owned by the callback
    // d) info might be deleted, so we cannot use it.
    // e) If the callback is ongoing during cleanup, the callback will
    //    hold the Mutex and delay the cleanup.
    RECURSIVE_MUTEX_LOCKER(*cacheMutex, *cacheMutexOwner);
    *isCleaned = true;
    for (auto& cb : agencyCallbacks) {
      _agencyCallbackRegistry->unregisterCallback(cb);
    }
  });

  std::vector<AgencyOperation> opers({IncreaseVersion()});
  std::vector<AgencyPrecondition> precs;
  std::unordered_set<std::string> conditions;
  std::unordered_set<ServerID> allServers;

  // current thread owning 'cacheMutex' write lock (workaround for non-recursive Mutex)
  for (auto& info : infos) {
    TRI_ASSERT(!info.name.empty());

    if (info.state == ClusterCollectionCreationState::DONE) {
      // This is possible in Enterprise / Smart Collection situation
      (*nrDone)++;
    }

    std::map<ShardID, std::vector<ServerID>> shardServers;
    for (auto pair : VPackObjectIterator(info.json.get("shards"))) {
      ShardID shardID = pair.key.copyString();
      std::vector<ServerID> serverIds;

      for (auto const& serv : VPackArrayIterator(pair.value)) {
        auto const sid = serv.copyString();
        serverIds.emplace_back(sid);
        allServers.emplace(sid);
      }
      shardServers.try_emplace(shardID, serverIds);
    }

    // The AgencyCallback will copy the closure will take responsibilty of it.
    auto closure = [cacheMutex, cacheMutexOwner, &info, dbServerResult, errMsg,
                    nrDone, isCleaned, shardServers, this](VPackSlice const& result) {
      // NOTE: This ordering here is important to cover against a race in cleanup.
      // a) The Guard get's the Mutex, sets isCleaned == true, then removes the callback
      // b) If the callback is acquired it is saved in a shared_ptr, the Mutex will be acquired first, then it will check if it isCleaned
      RECURSIVE_MUTEX_LOCKER(*cacheMutex, *cacheMutexOwner);
      if (*isCleaned) {
        return true;
      }
      TRI_ASSERT(!info.name.empty());
      if (info.state != ClusterCollectionCreationState::INIT) {
        // All leaders have reported either good or bad
        // We might be called by followers if they get in sync fast enough
        // In this IF we are in the followers case, we can safely ignore
        return true;
      }

      // result is the object at the path
      if (result.isObject() && result.length() == (size_t)info.numberOfShards) {
        std::string tmpError = "";

        for (auto const& p : VPackObjectIterator(result)) {
          if (arangodb::basics::VelocyPackHelper::getBooleanValue(p.value,
                                                                  StaticStrings::Error, false)) {
            tmpError += " shardID:" + p.key.copyString() + ":";
            tmpError += arangodb::basics::VelocyPackHelper::getStringValue(
                p.value, StaticStrings::ErrorMessage, "");
            if (p.value.hasKey(StaticStrings::ErrorNum)) {
              VPackSlice const errorNum = p.value.get(StaticStrings::ErrorNum);
              if (errorNum.isNumber()) {
                tmpError += " (errNum=";
                tmpError +=
                    basics::StringUtils::itoa(errorNum.getNumericValue<uint32_t>());
                tmpError += ")";
              }
            }
          }

          // wait that all followers have created our new collection
          if (tmpError.empty() && info.waitForReplication) {
            std::vector<ServerID> plannedServers;
            {
              READ_LOCKER(readLocker, _planProt.lock);
              auto it = shardServers.find(p.key.copyString());
              if (it != shardServers.end()) {
                plannedServers = (*it).second;
              } else {
                LOG_TOPIC("9ed54", ERR, Logger::CLUSTER)
                    << "Did not find shard in _shardServers: " << p.key.copyString()
                    << ". Maybe the collection is already dropped.";
                *errMsg = "Error in creation of collection: " + p.key.copyString() +
                          ". Collection already dropped. " + __FILE__ + ":" +
                          std::to_string(__LINE__);
                dbServerResult->store(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION,
                                      std::memory_order_release);
                TRI_ASSERT(info.state != ClusterCollectionCreationState::DONE);
                info.state = ClusterCollectionCreationState::FAILED;
                return true;
              }
            }
            if (plannedServers.empty()) {
              READ_LOCKER(readLocker, _planProt.lock);
              LOG_TOPIC("a0a76", DEBUG, Logger::CLUSTER)
                  << "This should never have happened, Plan empty. Dumping "
                     "_shards in Plan:";
              for (auto const& p : _shards) {
                LOG_TOPIC("60c7d", DEBUG, Logger::CLUSTER) << "Shard: " << p.first;
                for (auto const& q : *(p.second)) {
                  LOG_TOPIC("c7363", DEBUG, Logger::CLUSTER) << "  Server: " << q;
                }
              }
              TRI_ASSERT(false);
            }
            std::vector<ServerID> currentServers;
            VPackSlice servers = p.value.get("servers");
            if (!servers.isArray()) {
              return true;
            }
            for (auto const& server : VPackArrayIterator(servers)) {
              if (!server.isString()) {
                return true;
              }
              currentServers.push_back(server.copyString());
            }
            if (!ClusterHelpers::compareServerLists(plannedServers, currentServers)) {
              TRI_ASSERT(!info.name.empty());
              LOG_TOPIC("16623", DEBUG, Logger::CLUSTER)
                  << "Still waiting for all servers to ACK creation of " << info.name
                  << ". Planned: " << plannedServers << ", Current: " << currentServers;
              return true;
            }
          }
        }
        if (!tmpError.empty()) {
          *errMsg = "Error in creation of collection:" + tmpError + " " +
                    __FILE__ + std::to_string(__LINE__);
          dbServerResult->store(TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION,
                                std::memory_order_release);
          // We cannot get into bad state after a collection was created
          TRI_ASSERT(info.state != ClusterCollectionCreationState::DONE);
          info.state = ClusterCollectionCreationState::FAILED;
        } else {
          // We can have multiple calls to this callback, one per leader and one per follower
          // As soon as all leaders are done we are either FAILED or DONE, this cannot be altered later.
          TRI_ASSERT(info.state != ClusterCollectionCreationState::FAILED);
          info.state = ClusterCollectionCreationState::DONE;
          (*nrDone)++;
        }
      }
      return true;
    };
    // ATTENTION: The following callback calls the above closure in a
    // different thread. Nevertheless, the closure accesses some of our
    // local variables. Therefore we have to protect all accesses to them
    // by a mutex. We use the mutex of the condition variable in the
    // AgencyCallback for this.

    auto agencyCallback =
        std::make_shared<AgencyCallback>(_server,
                                         "Current/Collections/" + databaseName +
                                             "/" + info.collectionID,
                                         closure, true, false);
    _agencyCallbackRegistry->registerCallback(agencyCallback);
    agencyCallbacks.emplace_back(std::move(agencyCallback));
    opers.emplace_back(CreateCollectionOrder(databaseName, info.collectionID,
                                             info.isBuildingSlice()));

    // Ensure preconditions on the agency
    std::shared_ptr<ShardMap> otherCidShardMap = nullptr;
    auto const otherCidString =
        basics::VelocyPackHelper::getStringValue(info.json, StaticStrings::DistributeShardsLike,
                                                 StaticStrings::Empty);
    if (!otherCidString.empty() && conditions.find(otherCidString) == conditions.end()) {
      // Distribute shards like case.
      // Precondition: Master collection is not moving while we create this
      // collection We only need to add these once for every Master, we cannot
      // add multiple because we will end up with duplicate entries.
      // NOTE: We do not need to add all collections created here, as they will not succeed
      // In callbacks if they are moved during creation.
      // If they are moved after creation was reported success they are under protection by Supervision.
      conditions.emplace(otherCidString);
      if (colToDistributeShardsLike != nullptr) {
        otherCidShardMap = colToDistributeShardsLike->shardIds();
      } else {
        otherCidShardMap = getCollection(databaseName, otherCidString)->shardIds();
      }
      // Any of the shards locked?
      for (auto const& shard : *otherCidShardMap) {
        precs.emplace_back(AgencyPrecondition("Supervision/Shards/" + shard.first,
                                              AgencyPrecondition::Type::EMPTY, true));
      }
    }

    // additionally ensure that no such collectionID exists yet in Plan/Collections
    precs.emplace_back(AgencyPrecondition("Plan/Collections/" + databaseName + "/" + info.collectionID,
                                          AgencyPrecondition::Type::EMPTY, true));
  }

  // We need to make sure our plan is up to date.
  LOG_TOPIC("f4b14", DEBUG, Logger::CLUSTER)
      << "createCollectionCoordinator, loading Plan from agency...";

  // load the plan, so we are up-to-date
  loadPlan();
  uint64_t planVersion = 0;  // will be populated by following function call
  {
    READ_LOCKER(readLocker, _planProt.lock);
    planVersion = _planVersion;
    if (!isNewDatabase) {
      Result res = checkCollectionPreconditions(databaseName, infos, planVersion);
      if (res.fail()) {
        return res;
      }
    }
  }

  auto deleteCollectionGuard = scopeGuard([&infos, &databaseName, this, &ac]() {
    using namespace arangodb::cluster::paths;
    using namespace arangodb::cluster::paths::aliases;
    // We need to check isBuilding as a precondition.
    // If the transaction removing the isBuilding flag results in a timeout, the
    // state of the collection is unknown; if it was actually removed, we must
    // not drop the collection, but we must otherwise.

    auto precs = std::vector<AgencyPrecondition>{};
    auto opers = std::vector<AgencyOperation>{};

    // Note that we trust here that either all isBuilding flags are removed in
    // a single transaction, or none is.

    for (auto const& info : infos) {
      using namespace std::string_literals;
      auto const collectionPlanPath = "Plan/Collections/"s + databaseName + "/" + info.collectionID;
      precs.emplace_back(collectionPlanPath + "/" + StaticStrings::AttrIsBuilding,
          AgencyPrecondition::Type::EMPTY, false);
      opers.emplace_back(collectionPlanPath, AgencySimpleOperationType::DELETE_OP);
    }
    auto trx = AgencyWriteTransaction{opers, precs};

    using namespace std::chrono;
    using namespace std::chrono_literals;
    auto const begin = steady_clock::now();
    // After a shutdown, the supervision will clean the collections either due
    // to the coordinator going into FAIL, or due to it changing its rebootId.
    // Otherwise we must under no circumstance give up here, because noone else
    // will clean this up.
    while(!_server.isStopping()) {
      auto res = ac.sendTransactionWithFailover(trx);
      // If the collections were removed (res.ok()), we may abort. If we run
      // into precondition failed, the collections were successfully created, so
      // we're fine too.
      if (res.successful() || res.httpCode() == TRI_ERROR_HTTP_PRECONDITION_FAILED) {
        return;
      }
      // exponential backoff, just to be safe,
      auto const durationSinceStart = steady_clock::now() - begin;
      auto constexpr maxWaitTime = 2min;
      auto const waitTime =
          std::min<std::common_type_t<decltype(durationSinceStart), decltype(maxWaitTime)>>(
              durationSinceStart, maxWaitTime);
      std::this_thread::sleep_for(waitTime);
    }

  });

  // now try to update the plan in the agency, using the current plan version as
  // our precondition
  {
    // create a builder with just the version number for comparison
    VPackBuilder versionBuilder;
    versionBuilder.add(VPackValue(planVersion));

    VPackBuilder serversBuilder;
    {
      VPackArrayBuilder a(&serversBuilder);
      for (auto const & i : allServers) {
        serversBuilder.add(VPackValue(i));
      }
    }

    // Preconditions:
    // * plan version unchanged
    precs.emplace_back(
      AgencyPrecondition(
        "Plan/Version", AgencyPrecondition::Type::VALUE, versionBuilder.slice()));
    // * not in to be cleaned server list
    precs.emplace_back(
      AgencyPrecondition(
        "Target/ToBeCleanedServers", AgencyPrecondition::Type::INTERSECTION_EMPTY, serversBuilder.slice()));
    // * not in cleaned server list
    precs.emplace_back(
      AgencyPrecondition(
        "Target/CleanedServers", AgencyPrecondition::Type::INTERSECTION_EMPTY, serversBuilder.slice()));

    AgencyWriteTransaction transaction(opers, precs);

    {  // we hold this mutex from now on until we have updated our cache
      // using loadPlan, this is necessary for the callback closure to
      // see the new planned state for this collection. Otherwise it cannot
      // recognize completion of the create collection operation properly:
      RECURSIVE_MUTEX_LOCKER(*cacheMutex, *cacheMutexOwner);
      auto res = ac.sendTransactionWithFailover(transaction);
      // Only if not precondition failed
      if (!res.successful()) {
        if (res.httpCode() == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
          // use this special error code to signal that we got a precondition failure
          // in this case the caller can try again with an updated version of the plan change
          return {TRI_ERROR_REQUEST_CANCELED,
                  "operation aborted due to precondition failure"};
        }

        std::string errorMsg = "HTTP code: " + std::to_string(res.httpCode());
        errorMsg += " error message: " + res.errorMessage();
        errorMsg += " error details: " + res.errorDetails();
        errorMsg += " body: " + res.body();
        for (auto const& info : infos) {
          events::CreateCollection(databaseName, info.name,
                                   TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION_IN_PLAN);
        }
        return {TRI_ERROR_CLUSTER_COULD_NOT_CREATE_COLLECTION_IN_PLAN, std::move(errorMsg)};
      }

      // Update our cache:
      loadPlan();
    }
  }

  TRI_IF_FAILURE("ClusterInfo::createCollectionsCoordinator") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  LOG_TOPIC("98bca", DEBUG, Logger::CLUSTER)
      << "createCollectionCoordinator, Plan changed, waiting for success...";

  do {
    int tmpRes = dbServerResult->load(std::memory_order_acquire);
    if (TRI_microtime() > endTime) {
      for (auto const& info : infos) {
        LOG_TOPIC("f6b57", ERR, Logger::CLUSTER)
            << "Timeout in _create collection"
            << ": database: " << databaseName << ", collId:" << info.collectionID
            << "\njson: " << info.json.toString();
      }

      // Get a full agency dump for debugging
      logAgencyDump();

      if (tmpRes <= TRI_ERROR_NO_ERROR) {
        tmpRes = TRI_ERROR_CLUSTER_TIMEOUT;
      }
    }

    if (nrDone->load(std::memory_order_acquire) == infos.size()) {
      // We do not need to lock all condition variables
      // we are save by cacheMutex
      cbGuard.fire();
      // Now we need to remove the AttrIsBuilding flag and the creator in the Agency
      opers.clear();
      precs.clear();
      opers.push_back(IncreaseVersion());
      for (auto const& info : infos) {
        opers.emplace_back(
            CreateCollectionSuccess(databaseName, info.collectionID, info.json));
        // NOTE:
        // We cannot do anything better than: "noone" has modified our collections while
        // we tried to create them...
        // Preconditions cover against supervision jobs injecting other leaders / followers during failovers.
        // If they are it is not valid to confirm them here. (bad luck we were almost there)
        precs.emplace_back(CreateCollectionOrderPrecondition(databaseName, info.collectionID,
                                                             info.isBuildingSlice()));
      }

      AgencyWriteTransaction transaction(opers, precs);

      // This is a best effort, in the worst case the collection stays, but will
      // be cleaned out by deleteCollectionGuard respectively the supervision.
      // This removes *all* isBuilding flags from all collections. This is
      // important so that the creation of all collections is atomic, and
      // the deleteCollectionGuard relies on it, too.
      auto res = ac.sendTransactionWithFailover(transaction);

      if (res.successful()) {
        // Note that this is not strictly necessary, just to avoid an
        // unneccessary request when we're sure that we don't need it anymore.
        deleteCollectionGuard.cancel();
      }

      // Report if this operation worked, if it failed collections will be
      // cleaned up by deleteCollectionGuard.
      for (auto const& info : infos) {
        TRI_ASSERT(info.state == ClusterCollectionCreationState::DONE);
        events::CreateCollection(databaseName, info.name, res.errorCode());
      }
      loadCurrent();
      return res.errorCode();
    }
    if (tmpRes > TRI_ERROR_NO_ERROR) {
      // We do not need to lock all condition variables
      // we are safe by using cacheMutex
      cbGuard.fire();

      // report error
      for (auto const& info : infos) {
        // Report first error.
        // On timeout report it on all not finished ones.
        if (info.state == ClusterCollectionCreationState::FAILED ||
            (tmpRes == TRI_ERROR_CLUSTER_TIMEOUT &&
             info.state == ClusterCollectionCreationState::INIT)) {
          events::CreateCollection(databaseName, info.name, tmpRes);
        }
      }
      loadCurrent();
      return {tmpRes, *errMsg};
    }

    // If we get here we have not tried anything.
    // Wait on callbacks.

    if (_server.isStopping()) {
      // Report shutdown on all collections
      for (auto const& info : infos) {
        events::CreateCollection(databaseName, info.name, TRI_ERROR_SHUTTING_DOWN);
      }
      return TRI_ERROR_SHUTTING_DOWN;
    }

    // Wait for Callbacks to be triggered, it is sufficent to wait for the first non, done
    TRI_ASSERT(agencyCallbacks.size() == infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
      if (infos[i].state == ClusterCollectionCreationState::INIT) {
        bool gotTimeout;
        {
          // This one has not responded, wait for it.
          CONDITION_LOCKER(locker, agencyCallbacks[i]->_cv);
          gotTimeout = agencyCallbacks[i]->executeByCallbackOrTimeout(interval);
        }
        if (gotTimeout) {
          ++i;
          // We got woken up by waittime, not by  callback.
          // Let us check if we skipped other callbacks as well
          for (; i < infos.size(); ++i) {
            if (infos[i].state == ClusterCollectionCreationState::INIT) {
              agencyCallbacks[i]->refetchAndUpdate(true, false);
            }
          }
        }
        break;
      }
    }

  } while (!_server.isStopping());
  // If we get here we are not allowed to retry.
  // The loop above does not contain a break
  TRI_ASSERT(_server.isStopping());
  for (auto const& info : infos) {
    events::CreateCollection(databaseName, info.name, TRI_ERROR_SHUTTING_DOWN);
  }
  return Result{TRI_ERROR_SHUTTING_DOWN};
}

////////////////////////////////////////////////////////////////////////////////
/// @brief drop collection in coordinator, the return value is an ArangoDB
/// error code and the errorMsg is set accordingly. One possible error
/// is a timeout, a timeout of 0.0 means no timeout.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::dropCollectionCoordinator(  // drop collection
    std::string const& dbName,                  // database name
    std::string const& collectionID,
    double timeout  // request timeout
) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  if (dbName.empty() || (dbName[0] > '0' && dbName[0] < '9')) {
    events::DropCollection(dbName, collectionID, TRI_ERROR_ARANGO_DATABASE_NAME_INVALID);
    return Result(TRI_ERROR_ARANGO_DATABASE_NAME_INVALID);
  }

  AgencyComm ac(_server);
  AgencyCommResult res;

  // First check that no other collection has a distributeShardsLike
  // entry pointing to us:
  auto coll = getCollection(dbName, collectionID);
  auto colls = getCollections(dbName);  // reloads plan
  std::vector<std::string> clones;
  for (std::shared_ptr<LogicalCollection> const& p : colls) {
    if (p->distributeShardsLike() == coll->name() || p->distributeShardsLike() == collectionID) {
      clones.push_back(p->name());
    }
  }

  if (!clones.empty()) {
    std::string errorMsg("Collection '");
    errorMsg += coll->name();
    errorMsg += "' must not be dropped while '";
    errorMsg += arangodb::basics::StringUtils::join(clones, "', '");
    if (clones.size() == 1) {
      errorMsg += "' has ";
    } else {
      errorMsg += "' have ";
    };
    errorMsg += "distributeShardsLike set to '";
    errorMsg += coll->name();
    errorMsg += "'.";

    events::DropCollection(dbName, collectionID,
                           TRI_ERROR_CLUSTER_MUST_NOT_DROP_COLL_OTHER_DISTRIBUTESHARDSLIKE);
    return Result(  // result
        TRI_ERROR_CLUSTER_MUST_NOT_DROP_COLL_OTHER_DISTRIBUTESHARDSLIKE,  // code
        errorMsg  // message
    );
  }

  double const realTimeout = getTimeout(timeout);
  double const endTime = TRI_microtime() + realTimeout;
  double const interval = getPollInterval();
  auto dbServerResult = std::make_shared<std::atomic<int>>(-1);
  std::function<bool(VPackSlice const& result)> dbServerChanged = [=](VPackSlice const& result) {
    if (result.isNone() || result.isEmptyObject()) {
      dbServerResult->store(TRI_ERROR_NO_ERROR, std::memory_order_release);
    }
    return true;
  };

  // monitor the entry for the collection
  std::string const where = "Current/Collections/" + dbName + "/" + collectionID;

  // ATTENTION: The following callback calls the above closure in a
  // different thread. Nevertheless, the closure accesses some of our
  // local variables. Therefore we have to protect all accesses to them
  // by a mutex. We use the mutex of the condition variable in the
  // AgencyCallback for this.
  auto agencyCallback =
      std::make_shared<AgencyCallback>(_server, where, dbServerChanged, true, false);
  _agencyCallbackRegistry->registerCallback(agencyCallback);
  auto cbGuard = scopeGuard([this, &agencyCallback]() -> void {
    _agencyCallbackRegistry->unregisterCallback(agencyCallback);
  });

  size_t numberOfShards = 0;
  res = ac.getValues("Plan/Collections/" + dbName + "/" + collectionID +
                     "/shards");

  if (res.successful()) {
    velocypack::Slice databaseSlice = res.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Plan", "Collections", dbName}));

    if (!databaseSlice.isObject()) {
      // database dropped in the meantime
      events::DropCollection(dbName, collectionID, TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
      return TRI_ERROR_ARANGO_DATABASE_NOT_FOUND;
    }

    velocypack::Slice collectionSlice = databaseSlice.get(collectionID);
    if (!collectionSlice.isObject()) {
      // collection dropped in the meantime
      events::DropCollection(dbName, collectionID, TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
      return TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND;
    }

    velocypack::Slice shardsSlice = collectionSlice.get("shards");
    if (shardsSlice.isObject()) {
      numberOfShards = shardsSlice.length();
    } else {
      LOG_TOPIC("d340d", ERR, Logger::CLUSTER)
          << "Missing shards information on dropping " << dbName << "/" << collectionID;

      events::DropCollection(dbName, collectionID, TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
      return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
    }
  }

  // Transact to agency
  AgencyOperation delPlanCollection("Plan/Collections/" + dbName + "/" + collectionID,
                                    AgencySimpleOperationType::DELETE_OP);
  AgencyOperation incrementVersion("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);
  AgencyPrecondition precondition =
      AgencyPrecondition("Plan/Databases/" + dbName, AgencyPrecondition::Type::EMPTY, false);
  AgencyWriteTransaction trans({delPlanCollection, incrementVersion}, precondition);
  res = ac.sendTransactionWithFailover(trans);

  if (!res.successful()) {
    if (res.httpCode() == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
      LOG_TOPIC("279c5", ERR, Logger::CLUSTER)
          << "Precondition failed for this agency transaction: " << trans.toJson()
          << ", return code: " << res.httpCode();
    }

    logAgencyDump();

    // TODO: this should rather be TRI_ERROR_ARANGO_DATABASE_NOT_FOUND, as the
    // precondition is that the database still exists
    events::DropCollection(dbName, collectionID, TRI_ERROR_CLUSTER_COULD_NOT_DROP_COLLECTION);
    return Result(TRI_ERROR_CLUSTER_COULD_NOT_DROP_COLLECTION);
  }

  // Update our own cache:
  loadPlan();

  if (numberOfShards == 0) {
    loadCurrent();

    events::DropCollection(dbName, collectionID, TRI_ERROR_NO_ERROR);
    return Result(TRI_ERROR_NO_ERROR);
  }

  {
    CONDITION_LOCKER(locker, agencyCallback->_cv);

    while (true) {
      if (*dbServerResult >= 0) {
        cbGuard.fire();  // unregister cb before calling ac.removeValues(...)
        // ...remove the entire directory for the collection
        ac.removeValues("Current/Collections/" + dbName + "/" + collectionID, true);
        loadCurrent();

        events::DropCollection(dbName, collectionID, *dbServerResult);
        return Result(*dbServerResult);
      }

      if (TRI_microtime() > endTime) {
        LOG_TOPIC("76ea6", ERR, Logger::CLUSTER)
            << "Timeout in _drop collection (" << realTimeout << ")"
            << ": database: " << dbName << ", collId:" << collectionID
            << "\ntransaction sent to agency: " << trans.toJson();

        logAgencyDump();

        events::DropCollection(dbName, collectionID, TRI_ERROR_CLUSTER_TIMEOUT);
        return Result(TRI_ERROR_CLUSTER_TIMEOUT);
      }

      agencyCallback->executeByCallbackOrTimeout(interval);

      if (_server.isStopping()) {
        events::DropCollection(dbName, collectionID, TRI_ERROR_SHUTTING_DOWN);
        return Result(TRI_ERROR_SHUTTING_DOWN);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set collection properties in coordinator
////////////////////////////////////////////////////////////////////////////////

Result ClusterInfo::setCollectionPropertiesCoordinator(std::string const& databaseName,
                                                       std::string const& collectionID,
                                                       LogicalCollection const* info) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  AgencyComm ac(_server);
  AgencyCommResult res;

  AgencyPrecondition databaseExists("Plan/Databases/" + databaseName,
                                    AgencyPrecondition::Type::EMPTY, false);
  AgencyOperation incrementVersion("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);

  res = ac.getValues("Plan/Collections/" + databaseName + "/" + collectionID);

  if (!res.successful()) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  velocypack::Slice collection = res.slice()[0].get(std::vector<std::string>(
      {AgencyCommManager::path(), "Plan", "Collections", databaseName, collectionID}));

  if (!collection.isObject()) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  VPackBuilder temp;
  temp.openObject();
  temp.add(StaticStrings::WaitForSyncString, VPackValue(info->waitForSync()));
  temp.add(StaticStrings::ReplicationFactor, VPackValue(info->replicationFactor()));
  temp.add(StaticStrings::MinReplicationFactor, VPackValue(info->writeConcern())); // deprecated in 3.6
  temp.add(StaticStrings::WriteConcern, VPackValue(info->writeConcern()));
  temp.add(VPackValue(StaticStrings::Validation));
  info->validatorsToVelocyPack(temp);
  info->getPhysical()->getPropertiesVPack(temp);
  temp.close();

  VPackBuilder builder = VPackCollection::merge(collection, temp.slice(), true);

  res.clear();

  AgencyOperation setColl("Plan/Collections/" + databaseName + "/" + collectionID,
                          AgencyValueOperationType::SET, builder.slice());

  AgencyWriteTransaction trans({setColl, incrementVersion}, databaseExists);

  res = ac.sendTransactionWithFailover(trans);

  if (res.successful()) {
    loadPlan();
    return Result();
  }

  return Result(TRI_ERROR_CLUSTER_AGENCY_COMMUNICATION_FAILED, res.errorMessage());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create view in coordinator, the return value is an ArangoDB
/// error code and the errorMsg is set accordingly. One possible error
/// is a timeout, a timeout of 0.0 means no timeout.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::createViewCoordinator(  // create view
    std::string const& databaseName,        // database name
    std::string const& viewID,
    velocypack::Slice json  // view definition
) {
  // TRI_ASSERT(ServerState::instance()->isCoordinator());
  // FIXME TODO is this check required?
  auto const typeSlice = json.get(arangodb::StaticStrings::DataSourceType);

  if (!typeSlice.isString()) {
    std::string name;
    if (json.isObject()) {
      name = basics::VelocyPackHelper::getStringValue(json, StaticStrings::DataSourceName,
                                                      "");
    }
    events::CreateView(databaseName, name, TRI_ERROR_BAD_PARAMETER);
    return Result(TRI_ERROR_BAD_PARAMETER);
  }

  auto const name =
      basics::VelocyPackHelper::getStringValue(json, arangodb::StaticStrings::DataSourceName,
                                               StaticStrings::Empty);

  if (name.empty()) {
    events::CreateView(databaseName, name, TRI_ERROR_BAD_PARAMETER);
    return Result(TRI_ERROR_BAD_PARAMETER);  // must not be empty
  }

  {
    // check if a view with the same name is already planned
    loadPlan();
    READ_LOCKER(readLocker, _planProt.lock);
    {
      AllViews::const_iterator it = _plannedViews.find(databaseName);
      if (it != _plannedViews.end()) {
        DatabaseViews::const_iterator it2 = (*it).second.find(name);

        if (it2 != (*it).second.end()) {
          // view already exists!
          events::CreateView(databaseName, name, TRI_ERROR_ARANGO_DUPLICATE_NAME);
          return Result(TRI_ERROR_ARANGO_DUPLICATE_NAME, std::string("duplicate view name '") + name + "'");
        }
      }
    }
    {
      // check against planned collections as well
      AllCollections::const_iterator it = _plannedCollections.find(databaseName);
      if (it != _plannedCollections.end()) {
        DatabaseCollections::const_iterator it2 = (*it).second.find(name);

        if (it2 != (*it).second.end()) {
          // collection already exists!
          events::CreateCollection(databaseName, name, TRI_ERROR_ARANGO_DUPLICATE_NAME);
          return Result(TRI_ERROR_ARANGO_DUPLICATE_NAME, std::string("duplicate view name '") + name + "'");
        }
      }
    }
  }

  AgencyComm ac(_server);

  // mop: why do these ask the agency instead of checking cluster info?
  if (!ac.exists("Plan/Databases/" + databaseName)) {
    events::CreateView(databaseName, name, TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
    return Result(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  if (ac.exists("Plan/Views/" + databaseName + "/" + viewID)) {
    events::CreateView(databaseName, name, TRI_ERROR_CLUSTER_VIEW_ID_EXISTS);
    return Result(TRI_ERROR_CLUSTER_VIEW_ID_EXISTS);
  }

  AgencyWriteTransaction const transaction{
      // operations
      {{"Plan/Views/" + databaseName + "/" + viewID, AgencyValueOperationType::SET, json},
       {"Plan/Version", AgencySimpleOperationType::INCREMENT_OP}},
      // preconditions
      {{"Plan/Views/" + databaseName + "/" + viewID, AgencyPrecondition::Type::EMPTY, true}}};

  auto const res = ac.sendTransactionWithFailover(transaction);

  // Only if not precondition failed
  if (!res.successful()) {
    if (res.httpCode() == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
      // Dump agency plan:

      logAgencyDump();

      events::CreateView(databaseName, name, TRI_ERROR_CLUSTER_COULD_NOT_CREATE_VIEW_IN_PLAN);
      return Result(                                        // result
          TRI_ERROR_CLUSTER_COULD_NOT_CREATE_VIEW_IN_PLAN,  // code
          std::string("Precondition that view ") + name + " with ID " + viewID +
              " does not yet exist failed. Cannot create view.");
    }

    events::CreateView(databaseName, name, TRI_ERROR_CLUSTER_COULD_NOT_CREATE_VIEW_IN_PLAN);
    return Result(                                        // result
        TRI_ERROR_CLUSTER_COULD_NOT_CREATE_VIEW_IN_PLAN,  // code
        std::string("file: ") + __FILE__ + " line: " + std::to_string(__LINE__) +
            " HTTP code: " + std::to_string(res.httpCode()) +
            " error message: " + res.errorMessage() +
            " error details: " + res.errorDetails() + " body: " + res.body());
  }

  // Update our cache:
  loadPlan();

  events::CreateView(databaseName, name, TRI_ERROR_NO_ERROR);
  return Result(TRI_ERROR_NO_ERROR);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief drop view in coordinator, the return value is an ArangoDB
/// error code and the errorMsg is set accordingly.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::dropViewCoordinator(  // drop view
    std::string const& databaseName,      // database name
    std::string const& viewID             // view identifier
) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  // Transact to agency
  AgencyWriteTransaction const trans{
      // operations
      {{"Plan/Views/" + databaseName + "/" + viewID, AgencySimpleOperationType::DELETE_OP},
       {"Plan/Version", AgencySimpleOperationType::INCREMENT_OP}},
      // preconditions
      {{"Plan/Databases/" + databaseName, AgencyPrecondition::Type::EMPTY, false},
       {"Plan/Views/" + databaseName + "/" + viewID, AgencyPrecondition::Type::EMPTY, false}}};

  AgencyComm ac(_server);
  auto const res = ac.sendTransactionWithFailover(trans);

  // Update our own cache
  loadPlan();

  Result result;

  if (!res.successful()) {
    if (res.errorCode() == int(rest::ResponseCode::PRECONDITION_FAILED)) {
      result = Result(                                            // result
          TRI_ERROR_CLUSTER_COULD_NOT_REMOVE_COLLECTION_IN_PLAN,  // FIXME COULD_NOT_REMOVE_VIEW_IN_PLAN
          std::string("Precondition that view  with ID ") + viewID +
              " already exist failed. Cannot create view.");

      // Dump agency plan:
      logAgencyDump();
    } else {
      result = Result(                                            // result
          TRI_ERROR_CLUSTER_COULD_NOT_REMOVE_COLLECTION_IN_PLAN,  // FIXME COULD_NOT_REMOVE_VIEW_IN_PLAN
          std::string("file: ") + __FILE__ + " line: " + std::to_string(__LINE__) +
              " HTTP code: " + std::to_string(res.httpCode()) +
              " error message: " + res.errorMessage() +
              " error details: " + res.errorDetails() + " body: " + res.body());
    }
  }

  events::DropView(databaseName, viewID, result.errorNumber());

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set view properties in coordinator
////////////////////////////////////////////////////////////////////////////////

Result ClusterInfo::setViewPropertiesCoordinator(std::string const& databaseName,
                                                 std::string const& viewID,
                                                 VPackSlice const& json) {
  // TRI_ASSERT(ServerState::instance()->isCoordinator());
  AgencyComm ac(_server);

  auto res = ac.getValues("Plan/Views/" + databaseName + "/" + viewID);

  if (!res.successful()) {
    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND};
  }

  auto const view = res.slice()[0].get<std::string>(
      {AgencyCommManager::path(), "Plan", "Views", databaseName, viewID});

  if (!view.isObject()) {
    logAgencyDump();

    return {TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND};
  }

  res.clear();

  AgencyWriteTransaction const trans{
      // operations
      {{"Plan/Views/" + databaseName + "/" + viewID, AgencyValueOperationType::SET, json},
       {"Plan/Version", AgencySimpleOperationType::INCREMENT_OP}},
      // preconditions
      {"Plan/Databases/" + databaseName, AgencyPrecondition::Type::EMPTY, false}};

  res = ac.sendTransactionWithFailover(trans);

  if (!res.successful()) {
    return {TRI_ERROR_CLUSTER_AGENCY_COMMUNICATION_FAILED, res.errorMessage()};
  }

  loadPlan();
  return {};
}

////////////////////////////////////////////////////////////////////////////////
/// @brief set collection status in coordinator
////////////////////////////////////////////////////////////////////////////////

Result ClusterInfo::setCollectionStatusCoordinator(std::string const& databaseName,
                                                   std::string const& collectionID,
                                                   TRI_vocbase_col_status_e status) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  AgencyComm ac(_server);
  AgencyCommResult res;

  AgencyPrecondition databaseExists("Plan/Databases/" + databaseName,
                                    AgencyPrecondition::Type::EMPTY, false);

  res = ac.getValues("Plan/Collections/" + databaseName + "/" + collectionID);

  if (!res.successful()) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  VPackSlice col = res.slice()[0].get(std::vector<std::string>(
      {AgencyCommManager::path(), "Plan", "Collections", databaseName, collectionID}));

  if (!col.isObject()) {
    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  TRI_vocbase_col_status_e old = static_cast<TRI_vocbase_col_status_e>(
      arangodb::basics::VelocyPackHelper::getNumericValue<int>(
          col, "status", static_cast<int>(TRI_VOC_COL_STATUS_CORRUPTED)));

  if (old == status) {
    // no status change
    return Result();
  }

  VPackBuilder builder;
  try {
    VPackObjectBuilder b(&builder);
    for (auto const& entry : VPackObjectIterator(col)) {
      std::string key = entry.key.copyString();
      if (key != "status") {
        builder.add(key, entry.value);
      }
    }
    builder.add("status", VPackValue(status));
  } catch (...) {
    return Result(TRI_ERROR_OUT_OF_MEMORY);
  }
  res.clear();

  AgencyOperation setColl("Plan/Collections/" + databaseName + "/" + collectionID,
                          AgencyValueOperationType::SET, builder.slice());
  AgencyOperation incrementVersion("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);

  AgencyWriteTransaction trans({setColl, incrementVersion}, databaseExists);

  res = ac.sendTransactionWithFailover(trans);

  if (res.successful()) {
    loadPlan();
    return Result();
  }

  return Result(TRI_ERROR_CLUSTER_AGENCY_COMMUNICATION_FAILED, res.errorMessage());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensure an index in coordinator.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::ensureIndexCoordinator(LogicalCollection const& collection,
                                           VPackSlice const& slice, bool create,
                                           VPackBuilder& resultBuilder, double timeout) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  // check index id
  uint64_t iid = 0;
  VPackSlice const idSlice = slice.get(StaticStrings::IndexId);

  if (idSlice.isString()) {  // use predefined index id
    iid = arangodb::basics::StringUtils::uint64(idSlice.copyString());
  }

  if (iid == 0) {  // no id set, create a new one!
    iid = uniqid();
  }

  std::string const idString = arangodb::basics::StringUtils::itoa(iid);
  Result res;

  try {
    auto start = std::chrono::steady_clock::now();

    // Keep trying for 2 minutes, if it's preconditions, which are stopping us
    do {
      resultBuilder.clear();
      res = ensureIndexCoordinatorInner(  // creat index
          collection, idString, slice, create, resultBuilder, timeout);

      // Note that this function sets the errorMsg unless it is precondition
      // failed, in which case we retry, if this times out, we need to set
      // it ourselves, otherwise all is done!
      if (TRI_ERROR_HTTP_PRECONDITION_FAILED == res.errorNumber()) {
        auto diff = std::chrono::steady_clock::now() - start;

        if (diff < std::chrono::seconds(120)) {
          uint32_t wt = RandomGenerator::interval(static_cast<uint32_t>(1000));
          std::this_thread::sleep_for(std::chrono::steady_clock::duration(wt));
          continue;
        }

        res = Result(                                          // result
            TRI_ERROR_CLUSTER_COULD_NOT_CREATE_INDEX_IN_PLAN,  // code
            res.errorMessage()                                 // message
        );
      }

      break;
    } while (true);
  } catch (basics::Exception const& ex) {
    res = Result(   // result
        ex.code(),  // code
        TRI_errno_string(ex.code()) + std::string(", exception: ") + ex.what());
  } catch (...) {
    res = Result(TRI_ERROR_INTERNAL);
  }

  // We get here in any case eventually, regardless of whether we have
  //   - succeeded with lookup or index creation
  //   - failed because of a timeout and rollback
  //   - some other error
  // There is nothing more to do here.

  if (!_server.isStopping()) {
    loadPlan();
  }

  return res;
}

// The following function does the actual work of index creation: Create
// in Plan, watch Current until all dbservers for all shards have done their
// bit. If this goes wrong with a timeout, the creation operation is rolled
// back. If the `create` flag is false, this is actually a lookup operation.
// In any case, no rollback has to happen in the calling function
// ClusterInfo::ensureIndexCoordinator. Note that this method here
// sets the `isBuilding` attribute to `true`, which leads to the fact
// that the index is not yet used by queries. There is code in the
// Agency Supervision which deletes this flag once everything has been
// built successfully. This is a more robust and self-repairing solution
// than if we would take out the `isBuilding` here, since it survives a
// coordinator crash and failover operations.
// Finally note that the retry loop for the case of a failed precondition
// is outside this function here in `ensureIndexCoordinator`.
Result ClusterInfo::ensureIndexCoordinatorInner(LogicalCollection const& collection,
                                                std::string const& idString,
                                                VPackSlice const& slice, bool create,
                                                VPackBuilder& resultBuilder, double timeout) {
  AgencyComm ac(_server);

  using namespace std::chrono;

  double const realTimeout = getTimeout(timeout);
  double const endTime = TRI_microtime() + realTimeout;
  double const interval = getPollInterval();

  TRI_ASSERT(resultBuilder.isEmpty());

  auto type = slice.get(arangodb::StaticStrings::IndexType);
  if (!type.isString()) {
    return Result(TRI_ERROR_INTERNAL,
                  "expecting string value for \"type\" attribute");
  }

  const size_t numberOfShards = collection.numberOfShards();

  // Get the current entry in Plan for this collection
  PlanCollectionReader collectionFromPlan(collection);
  if (!collectionFromPlan.state().ok()) {
    return collectionFromPlan.state();
  }

  VPackSlice indexes = collectionFromPlan.indexes();
  for (auto const& other : VPackArrayIterator(indexes)) {
    TRI_ASSERT(other.isObject());
    if (true == arangodb::Index::Compare(slice, other)) {
      {  // found an existing index... Copy over all elements in slice.
        VPackObjectBuilder b(&resultBuilder);
        resultBuilder.add(VPackObjectIterator(other));
        resultBuilder.add("isNewlyCreated", VPackValue(false));
      }
      return Result(TRI_ERROR_NO_ERROR);
    }

    if (true == arangodb::Index::CompareIdentifiers(slice, other)) {
      // found an existing index with a same identifier (i.e. name)
      // but different definition, throw an error
      return Result(TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER,
                    "duplicate value for `" + arangodb::StaticStrings::IndexId +
                        "` or `" + arangodb::StaticStrings::IndexName + "`");
    }
  }

  // no existing index found.
  if (!create) {
    TRI_ASSERT(resultBuilder.isEmpty());
    return Result(TRI_ERROR_NO_ERROR);
  }

  // will contain the error number and message
  std::shared_ptr<std::atomic<int>> dbServerResult =
      std::make_shared<std::atomic<int>>(-1);
  std::shared_ptr<std::string> errMsg = std::make_shared<std::string>();

  std::function<bool(VPackSlice const& result)> dbServerChanged = [=](VPackSlice const& result) {
    if (!result.isObject() || result.length() != numberOfShards) {
      return true;
    }

    size_t found = 0;
    for (auto const& shard : VPackObjectIterator(result)) {
      VPackSlice const slice = shard.value;
      if (slice.hasKey("indexes")) {
        VPackSlice const indexes = slice.get("indexes");
        if (!indexes.isArray()) {
          break;  // no list, so our index is not present. we can abort
                  // searching
        }

        for (VPackSlice v : VPackArrayIterator(indexes)) {
          VPackSlice const k = v.get(StaticStrings::IndexId);
          if (!k.isString() || idString != k.copyString()) {
            continue;  // this is not our index
          }

          // check for errors
          if (hasError(v)) {
            // Note that this closure runs with the mutex in the condition
            // variable of the agency callback, which protects writing
            // to *errMsg:
            *errMsg = extractErrorMessage(shard.key.copyString(), v);
            *errMsg = "Error during index creation: " + *errMsg;
            // Returns the specific error number if set, or the general
            // error otherwise
            int errNum = arangodb::basics::VelocyPackHelper::getNumericValue<int>(
                v, StaticStrings::ErrorNum, TRI_ERROR_ARANGO_INDEX_CREATION_FAILED);
            dbServerResult->store(errNum, std::memory_order_release);
            return true;
          }

          found++;  // found our index
          break;
        }
      }
    }

    if (found == (size_t)numberOfShards) {
      dbServerResult->store(setErrormsg(TRI_ERROR_NO_ERROR, *errMsg), std::memory_order_release);
    }

    return true;
  };

  VPackBuilder newIndexBuilder;
  {
    VPackObjectBuilder ob(&newIndexBuilder);
    // Add the new index ignoring "id"
    for (auto const& e : VPackObjectIterator(slice)) {
      TRI_ASSERT(e.key.isString());
      std::string const& key = e.key.copyString();
      if (key != StaticStrings::IndexId && key != StaticStrings::IndexIsBuilding) {
        ob->add(e.key);
        ob->add(e.value);
      }
    }
    if (numberOfShards > 0 &&
        !slice.get(StaticStrings::IndexType).isEqualString("arangosearch")) {
      ob->add(StaticStrings::IndexIsBuilding, VPackValue(true));
    }
    ob->add(StaticStrings::IndexId, VPackValue(idString));
  }

  // ATTENTION: The following callback calls the above closure in a
  // different thread. Nevertheless, the closure accesses some of our
  // local variables. Therefore we have to protect all accesses to them
  // by a mutex. We use the mutex of the condition variable in the
  // AgencyCallback for this.
  std::string databaseName = collection.vocbase().name();
  std::string collectionID = std::to_string(collection.id());

  std::string where = "Current/Collections/" + databaseName + "/" + collectionID;
  auto agencyCallback =
      std::make_shared<AgencyCallback>(_server, where, dbServerChanged, true, false);

  _agencyCallbackRegistry->registerCallback(agencyCallback);
  auto cbGuard = scopeGuard(
      [&] { _agencyCallbackRegistry->unregisterCallback(agencyCallback); });

  std::string const planCollKey = "Plan/Collections/" + databaseName + "/" + collectionID;
  std::string const planIndexesKey = planCollKey + "/indexes";
  AgencyOperation newValue(planIndexesKey, AgencyValueOperationType::PUSH,
                           newIndexBuilder.slice());
  AgencyOperation incrementVersion("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);

  AgencyPrecondition oldValue(planCollKey, AgencyPrecondition::Type::VALUE,
                              collectionFromPlan.slice());
  AgencyWriteTransaction trx({newValue, incrementVersion}, oldValue);

  AgencyCommResult result = ac.sendTransactionWithFailover(trx, 0.0);

  // This object watches whether the collection is still present in Plan
  // It assumes that the collection *is* present and only changes state
  // if the collection disappears
  CollectionWatcher collectionWatcher(_agencyCallbackRegistry, collection);

  if (!result.successful()) {
    if (result.httpCode() == (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
      // Retry loop is outside!
      return Result(TRI_ERROR_HTTP_PRECONDITION_FAILED);
    }

    return Result(                                         // result
        TRI_ERROR_CLUSTER_COULD_NOT_CREATE_INDEX_IN_PLAN,  // code
        std::string(" Failed to execute ") + trx.toJson() +
            " ResultCode: " + std::to_string(result.errorCode()) +
            " HttpCode: " + std::to_string(result.httpCode()) + " " +
            std::string(__FILE__) + ":" + std::to_string(__LINE__));
  }

  // From here on we want to roll back the index creation if we run into
  // the timeout. If this coordinator crashes, the worst that can happen
  // is that the index stays in some state. In most cases, it will converge
  // against the planned state.
  loadPlan();

  if (numberOfShards == 0) {  // smart "dummy" collection has no shards
    TRI_ASSERT(collection.isSmart());

    {
      // Copy over all elements in slice.
      VPackObjectBuilder b(&resultBuilder);
      resultBuilder.add(StaticStrings::IsSmart, VPackValue(true));
    }

    loadCurrent();

    return Result(TRI_ERROR_NO_ERROR);
  }

  {
    while (!_server.isStopping()) {
      int tmpRes = dbServerResult->load(std::memory_order_acquire);

      if (tmpRes < 0) {
        // index has not shown up in Current yet,  follow up check to
        // ensure it is still in plan (not dropped between iterations)

        AgencyCommResult result = _agency.sendTransactionWithFailover(
            AgencyReadTransaction(AgencyCommManager::path(planIndexesKey)));

        if (result.successful()) {
          auto indexes = result.slice()[0].get(
              std::vector<std::string>{AgencyCommManager::path(), "Plan",
                                       "Collections", databaseName,
                                       collectionID, "indexes"});

          bool found = false;
          if (indexes.isArray()) {
            for (VPackSlice v : VPackArrayIterator(indexes)) {
              VPackSlice const k = v.get(StaticStrings::IndexId);
              if (k.isString() && k.isEqualString(idString)) {
                // index is still here
                found = true;
                break;
              }
            }
          }

          if (!found) {
            return Result(TRI_ERROR_ARANGO_INDEX_CREATION_FAILED,
                          "index was dropped during creation");
          }
        }
      }

      if (tmpRes == 0) {
        // Finally, in case all is good, remove the `isBuilding` flag
        // check that the index has appeared. Note that we have to have
        // a precondition since the collection could have been deleted
        // in the meantime:
        VPackBuilder finishedPlanIndex;
        {
          VPackObjectBuilder o(&finishedPlanIndex);
          for (auto const& entry : VPackObjectIterator(newIndexBuilder.slice())) {
            auto const key = entry.key.copyString();
            if (key != StaticStrings::IndexIsBuilding &&
                key != "isNewlyCreated") {
              finishedPlanIndex.add(entry.key.copyString(), entry.value);
            }
          }
        }

        AgencyWriteTransaction trx(
            {AgencyOperation(planIndexesKey, AgencyValueOperationType::REPLACE,
                             finishedPlanIndex.slice(), newIndexBuilder.slice()),
             AgencyOperation("Plan/Version", AgencySimpleOperationType::INCREMENT_OP)},
            AgencyPrecondition(planIndexesKey, AgencyPrecondition::Type::EMPTY, false));
        TRI_idx_iid_t indexId = arangodb::basics::StringUtils::uint64(
            newIndexBuilder.slice().get("id").copyString());
        if (!_agency.sendTransactionWithFailover(trx, 0.0).successful()) {
          // We just log the problem and move on, the Supervision will repair
          // things in due course:
          LOG_TOPIC("d9420", INFO, Logger::CLUSTER)
              << "Could not remove isBuilding flag in new index " << indexId
              << ", this will be repaired automatically.";
        }

        loadPlan();

        if (!collectionWatcher.isPresent()) {
          return Result(TRI_ERROR_ARANGO_INDEX_CREATION_FAILED,
                        "Collection " + collectionID +
                            " has gone from database " + databaseName +
                            ". Aborting index creation");
        }

        {
          // Copy over all elements in slice.
          VPackObjectBuilder b(&resultBuilder);
          resultBuilder.add(VPackObjectIterator(finishedPlanIndex.slice()));
          resultBuilder.add("isNewlyCreated", VPackValue(true));
        }
        CONDITION_LOCKER(locker, agencyCallback->_cv);

        return Result(tmpRes, *errMsg);
      }

      if (tmpRes > 0 || TRI_microtime() > endTime) {
        // At this time the index creation has failed and we want to
        // roll back the plan entry, provided the collection still exists:
        AgencyWriteTransaction trx(
            std::vector<AgencyOperation>(
                {AgencyOperation(planIndexesKey, AgencyValueOperationType::ERASE,
                                 newIndexBuilder.slice()),
                 AgencyOperation("Plan/Version", AgencySimpleOperationType::INCREMENT_OP)}),
            AgencyPrecondition(planCollKey, AgencyPrecondition::Type::EMPTY, false));

        int sleepFor = 50;
        auto rollbackEndTime = steady_clock::now() + std::chrono::seconds(10);

        while (true) {
          AgencyCommResult update = _agency.sendTransactionWithFailover(trx, 0.0);

          if (update.successful()) {
            loadPlan();

            if (tmpRes < 0) {                 // timeout
              return Result(                  // result
                  TRI_ERROR_CLUSTER_TIMEOUT,  // code
                  "Index could not be created within timeout, giving up and "
                  "rolling back index creation.");
            }

            // The mutex in the condition variable protects the access to
            // *errMsg:
            CONDITION_LOCKER(locker, agencyCallback->_cv);
            return Result(tmpRes, *errMsg);
          }

          if (update._statusCode == TRI_ERROR_HTTP_PRECONDITION_FAILED) {
            // Collection was removed, let's break here and report outside
            break;
          }

          if (steady_clock::now() > rollbackEndTime) {
            LOG_TOPIC("db00b", ERR, Logger::CLUSTER)
                << "Couldn't roll back index creation of " << idString
                << ". Database: " << databaseName << ", Collection " << collectionID;

            if (tmpRes < 0) {                 // timeout
              return Result(                  // result
                  TRI_ERROR_CLUSTER_TIMEOUT,  // code
                  "Timed out while trying to roll back index creation failure");
            }

            // The mutex in the condition variable protects the access to
            // *errMsg:
            CONDITION_LOCKER(locker, agencyCallback->_cv);
            return Result(tmpRes, *errMsg);
          }

          if (sleepFor <= 2500) {
            sleepFor *= 2;
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(sleepFor));
        }
        // We only get here if the collection was dropped just in the moment
        // when we wanted to roll back the index creation.
      }

      if (!collectionWatcher.isPresent()) {
        return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                      "collection " + collectionID +
                          "appears to have been dropped from database " +
                          databaseName + " during ensureIndex");
      }

      {
        CONDITION_LOCKER(locker, agencyCallback->_cv);
        agencyCallback->executeByCallbackOrTimeout(interval);
      }
    }
  }

  return Result(TRI_ERROR_SHUTTING_DOWN);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief drop an index in coordinator.
////////////////////////////////////////////////////////////////////////////////
Result ClusterInfo::dropIndexCoordinator(  // drop index
    std::string const& databaseName,       // database name
    std::string const& collectionID, TRI_idx_iid_t iid,
    double timeout  // request timeout
) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
  AgencyComm ac(_server);

  double const realTimeout = getTimeout(timeout);
  double const endTime = TRI_microtime() + realTimeout;
  double const interval = getPollInterval();
  std::string const idString = arangodb::basics::StringUtils::itoa(iid);

  std::string const planCollKey = "Plan/Collections/" + databaseName + "/" + collectionID;
  std::string const planIndexesKey = planCollKey + "/indexes";

  AgencyCommResult previous = ac.getValues(planCollKey);

  if (!previous.successful()) {
    events::DropIndex(databaseName, collectionID, idString, TRI_ERROR_CLUSTER_READING_PLAN_AGENCY);

    return Result(TRI_ERROR_CLUSTER_READING_PLAN_AGENCY);
  }

  velocypack::Slice collection = previous.slice()[0].get(std::vector<std::string>(
      {AgencyCommManager::path(), "Plan", "Collections", databaseName, collectionID}));
  if (!collection.isObject()) {
    events::DropIndex(databaseName, collectionID, idString,
                      TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);

    return Result(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND);
  }

  TRI_ASSERT(VPackObjectIterator(collection).size() > 0);
  size_t const numberOfShards =
      basics::VelocyPackHelper::getNumericValue<size_t>(collection,
                                                         StaticStrings::NumberOfShards, 1);

  VPackSlice indexes = collection.get("indexes");
  if (!indexes.isArray()) {
    LOG_TOPIC("63178", DEBUG, Logger::CLUSTER) << "Failed to find index " << databaseName
                                               << "/" << collectionID << "/" << iid;
    events::DropIndex(databaseName, collectionID, idString, TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
    return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
  }

  VPackSlice indexToRemove;

  for (VPackSlice indexSlice : VPackArrayIterator(indexes)) {
    auto idSlice = indexSlice.get(arangodb::StaticStrings::IndexId);
    auto typeSlice = indexSlice.get(arangodb::StaticStrings::IndexType);

    if (!idSlice.isString() || !typeSlice.isString()) {
      continue;
    }

    if (idSlice.isEqualString(idString)) {
      Index::IndexType type = Index::type(typeSlice.copyString());

      if (type == Index::TRI_IDX_TYPE_PRIMARY_INDEX || type == Index::TRI_IDX_TYPE_EDGE_INDEX) {
        events::DropIndex(databaseName, collectionID, idString, TRI_ERROR_FORBIDDEN);
        return Result(TRI_ERROR_FORBIDDEN);
      }

      indexToRemove = indexSlice;

      break;
    }
  }

  if (!indexToRemove.isObject()) {
    LOG_TOPIC("95fe6", DEBUG, Logger::CLUSTER) << "Failed to find index " << databaseName
                                               << "/" << collectionID << "/" << iid;
    events::DropIndex(databaseName, collectionID, idString, TRI_ERROR_ARANGO_INDEX_NOT_FOUND);

    return Result(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
  }

  std::string where = "Current/Collections/" + databaseName + "/" + collectionID;

  auto dbServerResult = std::make_shared<std::atomic<int>>(-1);
  std::function<bool(VPackSlice const& result)> dbServerChanged = [=](VPackSlice const& current) {
    if (numberOfShards == 0) {
      return false;
    }

    if (!current.isObject()) {
      return true;
    }

    VPackObjectIterator shards(current);

    if (shards.size() == numberOfShards) {
      bool found = false;
      for (auto const& shard : shards) {
        VPackSlice const indexes = shard.value.get("indexes");

        if (indexes.isArray()) {
          for (VPackSlice v : VPackArrayIterator(indexes)) {
            if (v.isObject()) {
              VPackSlice const k = v.get(StaticStrings::IndexId);
              if (k.isString() && k.isEqualString(idString)) {
                // still found the index in some shard
                found = true;
                break;
              }
            }

            if (found) {
              break;
            }
          }
        }
      }

      if (!found) {
        dbServerResult->store(TRI_ERROR_NO_ERROR, std::memory_order_release);
      }
    }
    return true;
  };

  // ATTENTION: The following callback calls the above closure in a
  // different thread. Nevertheless, the closure accesses some of our
  // local variables. Therefore we have to protect all accesses to them
  // by a mutex. We use the mutex of the condition variable in the
  // AgencyCallback for this.
  auto agencyCallback =
      std::make_shared<AgencyCallback>(_server, where, dbServerChanged, true, false);
  _agencyCallbackRegistry->registerCallback(agencyCallback);
  auto cbGuard = scopeGuard(
      [&] { _agencyCallbackRegistry->unregisterCallback(agencyCallback); });

  AgencyOperation planErase(planIndexesKey, AgencyValueOperationType::ERASE, indexToRemove);
  AgencyOperation incrementVersion("Plan/Version", AgencySimpleOperationType::INCREMENT_OP);
  AgencyPrecondition prec(planCollKey, AgencyPrecondition::Type::VALUE, collection);
  AgencyWriteTransaction trx({planErase, incrementVersion}, prec);
  AgencyCommResult result = ac.sendTransactionWithFailover(trx, 0.0);

  if (!result.successful()) {
    events::DropIndex(databaseName, collectionID, idString,
                      TRI_ERROR_CLUSTER_COULD_NOT_DROP_INDEX_IN_PLAN);

    return Result(                                       // result
        TRI_ERROR_CLUSTER_COULD_NOT_DROP_INDEX_IN_PLAN,  // code
        std::string(" Failed to execute ") + trx.toJson() +
            " ResultCode: " + std::to_string(result.errorCode()));
  }

  // load our own cache:
  loadPlan();
  if (numberOfShards == 0) {  // smart "dummy" collection has no shards
    TRI_ASSERT(collection.get(StaticStrings::IsSmart).getBool());
    loadCurrent();

    return Result(TRI_ERROR_NO_ERROR);
  }

  {
    CONDITION_LOCKER(locker, agencyCallback->_cv);

    while (true) {
      if (*dbServerResult >= 0) {
        cbGuard.fire();  // unregister cb
        loadCurrent();
        events::DropIndex(databaseName, collectionID, idString, *dbServerResult);

        return Result(*dbServerResult);
      }

      if (TRI_microtime() > endTime) {
        events::DropIndex(databaseName, collectionID, idString, TRI_ERROR_CLUSTER_TIMEOUT);

        return Result(TRI_ERROR_CLUSTER_TIMEOUT);
      }

      agencyCallback->executeByCallbackOrTimeout(interval);

      if (_server.isStopping()) {
        return Result(TRI_ERROR_SHUTTING_DOWN);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief (re-)load the information about servers from the agency
/// Usually one does not have to call this directly.
////////////////////////////////////////////////////////////////////////////////

static std::string const prefixServersRegistered = "Current/ServersRegistered";
static std::string const prefixServersKnown = "Current/ServersKnown";
static std::string const mapUniqueToShortId = "Target/MapUniqueToShortID";

void ClusterInfo::loadServers() {
  ++_serversProt.wantedVersion;  // Indicate that after *NOW* somebody has to
                                 // reread from the agency!
  MUTEX_LOCKER(mutexLocker, _serversProt.mutex);
  uint64_t storedVersion = _serversProt.wantedVersion;  // this is the version
  // we will set in the end
  if (_serversProt.doneVersion == storedVersion) {
    // Somebody else did, what we intended to do, so just return
    return;
  }

  AgencyCommResult result = _agency.sendTransactionWithFailover(AgencyReadTransaction(
      std::vector<std::string>({AgencyCommManager::path(prefixServersRegistered),
                                AgencyCommManager::path(mapUniqueToShortId),
                                AgencyCommManager::path(prefixServersKnown)})));

  if (result.successful()) {
    velocypack::Slice serversRegistered = result.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Current", "ServersRegistered"}));

    velocypack::Slice serversAliases = result.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Target", "MapUniqueToShortID"}));

    velocypack::Slice serversKnownSlice = result.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Current", "ServersKnown"}));

    if (serversRegistered.isObject()) {
      decltype(_servers) newServers;
      decltype(_serverAliases) newAliases;
      decltype(_serverAdvertisedEndpoints) newAdvertisedEndpoints;
      decltype(_serverTimestamps) newTimestamps;

      std::unordered_set<ServerID> serverIds;

      for (auto const& res : VPackObjectIterator(serversRegistered)) {
        velocypack::Slice slice = res.value;

        if (slice.isObject() && slice.hasKey("endpoint")) {
          std::string server =
              arangodb::basics::VelocyPackHelper::getStringValue(slice,
                                                                 "endpoint", "");
          std::string advertised = arangodb::basics::VelocyPackHelper::getStringValue(
              slice, "advertisedEndpoint", "");

          std::string serverId = res.key.copyString();
          try {
            velocypack::Slice serverSlice;
            serverSlice = serversAliases.get(serverId);
            if (serverSlice.isObject()) {
              std::string alias = arangodb::basics::VelocyPackHelper::getStringValue(
                  serverSlice, "ShortName", "");
              newAliases.try_emplace(std::move(alias), serverId);
            }
          } catch (...) {
          }
          std::string serverTimestamp =
              arangodb::basics::VelocyPackHelper::getStringValue(slice,
                                                                 "timestamp",
                                                                 "");
          newServers.try_emplace(serverId, server);
          newAdvertisedEndpoints.try_emplace(serverId, advertised);
          serverIds.emplace(serverId);
          newTimestamps.try_emplace(serverId, serverTimestamp);
        }
      }

      decltype(_serversKnown) newServersKnown(serversKnownSlice, serverIds);

      // Now set the new value:
      {
        WRITE_LOCKER(writeLocker, _serversProt.lock);
        _servers.swap(newServers);
        _serverAliases.swap(newAliases);
        _serverAdvertisedEndpoints.swap(newAdvertisedEndpoints);
        _serversKnown = std::move(newServersKnown);
        _serverTimestamps.swap(newTimestamps);
        _serversProt.doneVersion = storedVersion;
        _serversProt.isValid = true;
      }
      // RebootTracker has its own mutex, and doesn't strictly need to be in
      // sync with the other members.
      rebootTracker().updateServerState(_serversKnown.rebootIds());
      return;
    }
  }

  LOG_TOPIC("449e0", DEBUG, Logger::CLUSTER)
      << "Error while loading " << prefixServersRegistered
      << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
      << " errorMessage: " << result.errorMessage() << " body: " << result.body();
}

////////////////////////////////////////////////////////////////////////
/// @brief Hand out copy of reboot ids
////////////////////////////////////////////////////////////////////////////////

std::unordered_map<ServerID, RebootId> ClusterInfo::rebootIds() const {
  MUTEX_LOCKER(mutexLocker, _serversProt.mutex);
  return _serversKnown.rebootIds();
}

////////////////////////////////////////////////////////////////////////
/// @brief find the endpoint of a server from its ID.
/// If it is not found in the cache, the cache is reloaded once, if
/// it is still not there an empty string is returned as an error.
////////////////////////////////////////////////////////////////////////////////

std::string ClusterInfo::getServerEndpoint(ServerID const& serverID) {
#ifdef DEBUG_SYNC_REPLICATION
  if (serverID == "debug-follower") {
    return "tcp://127.0.0.1:3000";
  }
#endif
  int tries = 0;

  if (!_serversProt.isValid) {
    loadServers();
    tries++;
  }

  std::string serverID_ = serverID;

  while (true) {
    {
      READ_LOCKER(readLocker, _serversProt.lock);

      // _serversAliases is a map-type <Alias, ServerID>
      auto ita = _serverAliases.find(serverID_);

      if (ita != _serverAliases.end()) {
        serverID_ = (*ita).second;
      }

      // _servers is a map-type <ServerId, std::string>
      auto it = _servers.find(serverID_);

      if (it != _servers.end()) {
        return (*it).second;
      }
    }

    if (++tries >= 2) {
      break;
    }

    // must call loadServers outside the lock
    loadServers();
  }

  return std::string();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief find the advertised endpoint of a server from its ID.
/// If it is not found in the cache, the cache is reloaded once, if
/// it is still not there an empty string is returned as an error.
////////////////////////////////////////////////////////////////////////////////

std::string ClusterInfo::getServerAdvertisedEndpoint(ServerID const& serverID) {
#ifdef DEBUG_SYNC_REPLICATION
  if (serverID == "debug-follower") {
    return "tcp://127.0.0.1:3000";
  }
#endif
  int tries = 0;

  if (!_serversProt.isValid) {
    loadServers();
    tries++;
  }

  std::string serverID_ = serverID;

  while (true) {
    {
      READ_LOCKER(readLocker, _serversProt.lock);

      // _serversAliases is a map-type <Alias, ServerID>
      auto ita = _serverAliases.find(serverID_);

      if (ita != _serverAliases.end()) {
        serverID_ = (*ita).second;
      }

      // _serversAliases is a map-type <ServerID, std::string>
      auto it = _serverAdvertisedEndpoints.find(serverID_);
      if (it != _serverAdvertisedEndpoints.end()) {
        return (*it).second;
      }
    }

    if (++tries >= 2) {
      break;
    }

    // must call loadServers outside the lock
    loadServers();
  }

  return std::string();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief find the ID of a server from its endpoint.
/// If it is not found in the cache, the cache is reloaded once, if
/// it is still not there an empty string is returned as an error.
////////////////////////////////////////////////////////////////////////////////

std::string ClusterInfo::getServerName(std::string const& endpoint) {
  int tries = 0;

  if (!_serversProt.isValid) {
    loadServers();
    tries++;
  }

  while (true) {
    {
      READ_LOCKER(readLocker, _serversProt.lock);
      for (auto const& it : _servers) {
        if (it.second == endpoint) {
          return it.first;
        }
      }
    }

    if (++tries >= 2) {
      break;
    }

    // must call loadServers outside the lock
    loadServers();
  }

  return std::string();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief (re-)load the information about all coordinators from the agency
/// Usually one does not have to call this directly.
////////////////////////////////////////////////////////////////////////////////

static std::string const prefixCurrentCoordinators = "Current/Coordinators";

void ClusterInfo::loadCurrentCoordinators() {
  ++_coordinatorsProt.wantedVersion;  // Indicate that after *NOW* somebody
                                      // has to reread from the agency!
  MUTEX_LOCKER(mutexLocker, _coordinatorsProt.mutex);
  uint64_t storedVersion = _coordinatorsProt.wantedVersion;  // this is the
  // version we will set in the end
  if (_coordinatorsProt.doneVersion == storedVersion) {
    // Somebody else did, what we intended to do, so just return
    return;
  }

  // Now contact the agency:
  AgencyCommResult result = _agency.getValues(prefixCurrentCoordinators);

  if (result.successful()) {
    velocypack::Slice currentCoordinators = result.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Current", "Coordinators"}));

    if (currentCoordinators.isObject()) {
      decltype(_coordinators) newCoordinators;

      for (auto const& coordinator : VPackObjectIterator(currentCoordinators)) {
        newCoordinators.try_emplace(coordinator.key.copyString(), coordinator.value.copyString());
      }

      // Now set the new value:
      {
        WRITE_LOCKER(writeLocker, _coordinatorsProt.lock);
        _coordinators.swap(newCoordinators);
        _coordinatorsProt.doneVersion = storedVersion;
        _coordinatorsProt.isValid = true;
      }
      return;
    }
  }

  LOG_TOPIC("5ee6d", DEBUG, Logger::CLUSTER)
      << "Error while loading " << prefixCurrentCoordinators
      << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
      << " errorMessage: " << result.errorMessage() << " body: " << result.body();
}

static std::string const prefixMappings = "Target/MapUniqueToShortID";

void ClusterInfo::loadCurrentMappings() {
  ++_mappingsProt.wantedVersion;  // Indicate that after *NOW* somebody
                                  // has to reread from the agency!
  MUTEX_LOCKER(mutexLocker, _mappingsProt.mutex);
  uint64_t storedVersion = _mappingsProt.wantedVersion;  // this is the
                                                         // version we will
                                                         // set in the end
  if (_mappingsProt.doneVersion == storedVersion) {
    // Somebody else did, what we intended to do, so just return
    return;
  }

  // Now contact the agency:
  AgencyCommResult result = _agency.getValues(prefixMappings);

  if (result.successful()) {
    velocypack::Slice mappings = result.slice()[0].get(std::vector<std::string>(
        {AgencyCommManager::path(), "Target", "MapUniqueToShortID"}));

    if (mappings.isObject()) {
      decltype(_coordinatorIdMap) newCoordinatorIdMap;

      for (auto const& mapping : VPackObjectIterator(mappings)) {
        ServerID fullId = mapping.key.copyString();
        auto mapObject = mapping.value;
        if (mapObject.isObject()) {
          ServerShortName shortName = mapObject.get("ShortName").copyString();

          ServerShortID shortId =
              mapObject.get("TransactionID").getNumericValue<ServerShortID>();
          static std::string const expectedPrefix{"Coordinator"};
          if (shortName.size() > expectedPrefix.size() &&
              shortName.substr(0, expectedPrefix.size()) == expectedPrefix) {
            newCoordinatorIdMap.try_emplace(shortId, fullId);
          }
        }
      }

      // Now set the new value:
      {
        WRITE_LOCKER(writeLocker, _mappingsProt.lock);
        _coordinatorIdMap.swap(newCoordinatorIdMap);
        _mappingsProt.doneVersion = storedVersion;
        _mappingsProt.isValid = true;
      }
      return;
    }
  }

  LOG_TOPIC("36f2e", DEBUG, Logger::CLUSTER)
      << "Error while loading " << prefixMappings
      << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
      << " errorMessage: " << result.errorMessage() << " body: " << result.body();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief (re-)load the information about all DBservers from the agency
/// Usually one does not have to call this directly.
////////////////////////////////////////////////////////////////////////////////

static std::string const prefixCurrentDBServers = "Current/DBServers";
static std::string const prefixTarget = "Target";

void ClusterInfo::loadCurrentDBServers() {
  ++_DBServersProt.wantedVersion;  // Indicate that after *NOW* somebody has to
                                   // reread from the agency!
  MUTEX_LOCKER(mutexLocker, _DBServersProt.mutex);
  uint64_t storedVersion = _DBServersProt.wantedVersion;  // this is the version
  // we will set in the end
  if (_DBServersProt.doneVersion == storedVersion) {
    // Somebody else did, what we intended to do, so just return
    return;
  }

  // Now contact the agency:
  AgencyCommResult result = _agency.getValues(prefixCurrentDBServers);
  AgencyCommResult target = _agency.getValues(prefixTarget);

  if (result.successful() && target.successful()) {
    velocypack::Slice currentDBServers;
    velocypack::Slice failedDBServers;
    velocypack::Slice cleanedDBServers;
    velocypack::Slice toBeCleanedDBServers;

    if (result.slice().length() > 0) {
      currentDBServers = result.slice()[0].get(std::vector<std::string>(
          {AgencyCommManager::path(), "Current", "DBServers"}));
    }
    if (target.slice().length() > 0) {
      failedDBServers = target.slice()[0].get(std::vector<std::string>(
          {AgencyCommManager::path(), "Target", "FailedServers"}));
      cleanedDBServers = target.slice()[0].get(std::vector<std::string>(
          {AgencyCommManager::path(), "Target", "CleanedServers"}));
      toBeCleanedDBServers = target.slice()[0].get(std::vector<std::string>(
          {AgencyCommManager::path(), "Target", "ToBeCleanedServers"}));
    }
    if (currentDBServers.isObject() && failedDBServers.isObject()) {
      decltype(_DBServers) newDBServers;

      for (auto const& dbserver : VPackObjectIterator(currentDBServers)) {
        bool found = false;
        if (failedDBServers.isObject()) {
          for (auto const& failedServer : VPackObjectIterator(failedDBServers)) {
            if (basics::VelocyPackHelper::equal(dbserver.key, failedServer.key, false)) {
              found = true;
              break;
            }
          }
        }
        if (found) {
          continue;
        }

        if (cleanedDBServers.isArray()) {
          found = false;
          for (auto const& cleanedServer : VPackArrayIterator(cleanedDBServers)) {
            if (basics::VelocyPackHelper::equal(dbserver.key, cleanedServer, false)) {
              found = true;
              break;
            }
          }
          if (found) {
            continue;
          }
        }

        if (toBeCleanedDBServers.isArray()) {
          found = false;
          for (auto const& toBeCleanedServer : VPackArrayIterator(toBeCleanedDBServers)) {
            if (basics::VelocyPackHelper::equal(dbserver.key, toBeCleanedServer, false)) {
              found = true;
              break;
            }
          }
          if (found) {
            continue;
          }
        }

        newDBServers.try_emplace(dbserver.key.copyString(),
                                            dbserver.value.copyString());
      }

      // Now set the new value:
      {
        WRITE_LOCKER(writeLocker, _DBServersProt.lock);
        _DBServers.swap(newDBServers);
        _DBServersProt.doneVersion = storedVersion;
        _DBServersProt.isValid = true;
      }
      return;
    }
  }

  LOG_TOPIC("5a7e1", DEBUG, Logger::CLUSTER)
      << "Error while loading " << prefixCurrentDBServers
      << " httpCode: " << result.httpCode() << " errorCode: " << result.errorCode()
      << " errorMessage: " << result.errorMessage() << " body: " << result.body();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a list of all DBServers in the cluster that have
/// currently registered
////////////////////////////////////////////////////////////////////////////////

std::vector<ServerID> ClusterInfo::getCurrentDBServers() {
  std::vector<ServerID> result;

  if (!_DBServersProt.isValid) {
    loadCurrentDBServers();
  }
  // return a consistent state of servers
  READ_LOCKER(readLocker, _DBServersProt.lock);

  result.reserve(_DBServers.size());

  for (auto& it : _DBServers) {
    result.emplace_back(it.first);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief find the servers who are responsible for a shard (one leader
/// and multiple followers)
/// If it is not found in the cache, the cache is reloaded once, if
/// it is still not there an empty string is returned as an error.
////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<std::vector<ServerID>> ClusterInfo::getResponsibleServer(ShardID const& shardID) {
  int tries = 0;

  if (!_currentProt.isValid) {
    loadCurrent();
    tries++;
  }

  while (true) {
    {
      READ_LOCKER(readLocker, _currentProt.lock);
      // _shardIds is a map-type <ShardId,
      // std::shared_ptr<std::vector<ServerId>>>
      auto it = _shardIds.find(shardID);

      if (it != _shardIds.end()) {
        auto serverList = (*it).second;
        if (serverList != nullptr && serverList->size() > 0 &&
            (*serverList)[0].size() > 0 && (*serverList)[0][0] == '_') {
          // This is a temporary situation in which the leader has already
          // resigned, let's wait half a second and try again.
          --tries;
          LOG_TOPIC("b1dc5", INFO, Logger::CLUSTER)
              << "getResponsibleServer: found resigned leader,"
              << "waiting for half a second...";
        } else {
          return (*it).second;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (++tries >= 2) {
      break;
    }

    // must load collections outside the lock
    loadCurrent();
  }

  return std::make_shared<std::vector<ServerID>>();
}

//////////////////////////////////////////////////////////////////////////////
/// @brief atomically find all servers who are responsible for the given
/// shards (only the leaders).
/// will throw an exception if no leader can be found for any
/// of the shards. will return an empty result if the shards couldn't be
/// determined after a while - it is the responsibility of the caller to
/// check for an empty result!
//////////////////////////////////////////////////////////////////////////////

std::unordered_map<ShardID, ServerID> ClusterInfo::getResponsibleServers(
    std::unordered_set<ShardID> const& shardIds) {
  TRI_ASSERT(!shardIds.empty());

  std::unordered_map<ShardID, ServerID> result;
  int tries = 0;

  if (!_currentProt.isValid) {
    loadCurrent();
    tries++;
  }

  while (true) {
    TRI_ASSERT(result.empty());
    {
      READ_LOCKER(readLocker, _currentProt.lock);
      for (auto const& shardId : shardIds) {
        auto it = _shardIds.find(shardId);

        if (it == _shardIds.end()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND,
                                         "no servers found for shard " + shardId);
        }

        auto serverList = (*it).second;
        if (serverList == nullptr || serverList->empty()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                         "no servers found for shard " + shardId);
        }

        if ((*serverList)[0].size() > 0 && (*serverList)[0][0] == '_') {
          // This is a temporary situation in which the leader has already
          // resigned, let's wait half a second and try again.
          --tries;
          break;
        }

        // put leader into result
        result.try_emplace(shardId, (*it).second->front());
      }
    }

    if (result.size() == shardIds.size()) {
      // result is complete
      break;
    }

    // reset everything we found so far for the next round
    result.clear();

    if (++tries >= 2 || _server.isStopping()) {
      break;
    }

    LOG_TOPIC("31428", INFO, Logger::CLUSTER)
        << "getResponsibleServers: found resigned leader,"
        << "waiting for half a second...";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // must load collections outside the lock
    loadCurrent();
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief find the shard list of a collection, sorted numerically
////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<std::vector<ShardID>> ClusterInfo::getShardList(CollectionID const& collectionID) {
  if (!_planProt.isValid) {
    loadPlan();
  }

  int tries = 0;
  while (true) {
    {
      // Get the sharding keys and the number of shards:
      READ_LOCKER(readLocker, _planProt.lock);
      // _shards is a map-type <CollectionId, shared_ptr<vector<string>>>
      auto it = _shards.find(collectionID);

      if (it != _shards.end()) {
        return it->second;
      }
    }
    if (++tries >= 2) {
      return std::make_shared<std::vector<ShardID>>();
    }
    loadPlan();
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the list of coordinator server names
////////////////////////////////////////////////////////////////////////////////

std::vector<ServerID> ClusterInfo::getCurrentCoordinators() {
  std::vector<ServerID> result;

  if (!_coordinatorsProt.isValid) {
    loadCurrentCoordinators();
  }

  // return a consistent state of servers
  READ_LOCKER(readLocker, _coordinatorsProt.lock);

  result.reserve(_coordinators.size());

  for (auto& it : _coordinators) {
    result.emplace_back(it.first);
  }

  return result;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief lookup full coordinator ID from short ID
////////////////////////////////////////////////////////////////////////////////

ServerID ClusterInfo::getCoordinatorByShortID(ServerShortID shortId) {
  ServerID result;

  if (!_mappingsProt.isValid) {
    loadCurrentMappings();
  }

  // return a consistent state of servers
  READ_LOCKER(readLocker, _mappingsProt.lock);

  auto it = _coordinatorIdMap.find(shortId);
  if (it != _coordinatorIdMap.end()) {
    result = it->second;
  }

  return result;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief invalidate plan
//////////////////////////////////////////////////////////////////////////////

void ClusterInfo::invalidatePlan() {
  {
    WRITE_LOCKER(writeLocker, _planProt.lock);
    _planProt.isValid = false;
  }
}

//////////////////////////////////////////////////////////////////////////////
/// @brief invalidate current coordinators
//////////////////////////////////////////////////////////////////////////////

void ClusterInfo::invalidateCurrentCoordinators() {
  {
    WRITE_LOCKER(writeLocker, _coordinatorsProt.lock);
    _coordinatorsProt.isValid = false;
  }
}

//////////////////////////////////////////////////////////////////////////////
/// @brief invalidate current mappings
//////////////////////////////////////////////////////////////////////////////

void ClusterInfo::invalidateCurrentMappings() {
  {
    WRITE_LOCKER(writeLocker, _mappingsProt.lock);
    _mappingsProt.isValid = false;
  }
}

//////////////////////////////////////////////////////////////////////////////
/// @brief invalidate current
//////////////////////////////////////////////////////////////////////////////

void ClusterInfo::invalidateCurrent() {
  {
    WRITE_LOCKER(writeLocker, _serversProt.lock);
    _serversProt.isValid = false;
  }
  {
    WRITE_LOCKER(writeLocker, _DBServersProt.lock);
    _DBServersProt.isValid = false;
  }
  {
    WRITE_LOCKER(writeLocker, _currentProt.lock);
    _currentProt.isValid = false;
  }
  invalidateCurrentCoordinators();
  invalidateCurrentMappings();
}

//////////////////////////////////////////////////////////////////////////////
/// @brief get current "Plan" structure
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<VPackBuilder> ClusterInfo::getPlan() {
  if (!_planProt.isValid) {
    loadPlan();
  }
  READ_LOCKER(readLocker, _planProt.lock);
  return _plan;
}

//////////////////////////////////////////////////////////////////////////////
/// @brief get current "Current" structure
//////////////////////////////////////////////////////////////////////////////

std::shared_ptr<VPackBuilder> ClusterInfo::getCurrent() {
  if (!_currentProt.isValid) {
    loadCurrent();
  }
  READ_LOCKER(readLocker, _currentProt.lock);
  return _current;
}

std::unordered_map<ServerID, std::string> ClusterInfo::getServers() {
  if (!_serversProt.isValid) {
    loadServers();
  }
  READ_LOCKER(readLocker, _serversProt.lock);
  std::unordered_map<ServerID, std::string> serv = _servers;
  return serv;
}

std::unordered_map<ServerID, std::string> ClusterInfo::getServerAliases() {
  READ_LOCKER(readLocker, _serversProt.lock);
  std::unordered_map<std::string, std::string> ret;
  for (const auto& i : _serverAliases) {
    ret.try_emplace(i.second, i.first);
  }
  return ret;
}

std::unordered_map<ServerID, std::string> ClusterInfo::getServerAdvertisedEndpoints() {
  READ_LOCKER(readLocker, _serversProt.lock);
  std::unordered_map<std::string, std::string> ret;
  for (const auto& i : _serverAdvertisedEndpoints) {
    ret.try_emplace(i.second, i.first);
  }
  return ret;
}

std::unordered_map<ServerID, std::string> ClusterInfo::getServerTimestamps() {
  READ_LOCKER(readLocker, _serversProt.lock);
  return _serverTimestamps;
}

arangodb::Result ClusterInfo::getShardServers(ShardID const& shardId,
                                              std::vector<ServerID>& servers) {
  READ_LOCKER(readLocker, _planProt.lock);

  auto it = _shardServers.find(shardId);
  if (it != _shardServers.end()) {
    servers = (*it).second;
    return arangodb::Result();
  }

  LOG_TOPIC("16d14", DEBUG, Logger::CLUSTER)
      << "Strange, did not find shard in _shardServers: " << shardId;
  return arangodb::Result(TRI_ERROR_FAILED);
}

CollectionID ClusterInfo::getCollectionNameForShard(ShardID const& shardId) {
  READ_LOCKER(readLocker, _planProt.lock);

  auto it = _shardToName.find(shardId);
  if (it != _shardToName.end()) {
    return it->second;
  }
  return StaticStrings::Empty;
}

arangodb::Result ClusterInfo::agencyDump(std::shared_ptr<VPackBuilder> body) {
  AgencyCommResult dump = _agency.dump();

  if (!dump.successful()) {
    LOG_TOPIC("93c0e", ERR, Logger::CLUSTER)
        << "failed to acquire agency dump: " << dump.errorMessage();
    return Result(dump.errorCode(), dump.errorMessage());
  }

  body->add(dump.slice());
  return Result();
}

arangodb::Result ClusterInfo::agencyPlan(std::shared_ptr<VPackBuilder> body) {
  AgencyCommResult dump = _agency.getValues("Plan");

  if (!dump.successful()) {
    LOG_TOPIC("ce937", ERR, Logger::CLUSTER)
        << "failed to acquire agency dump: " << dump.errorMessage();
    return Result(dump.errorCode(), dump.errorMessage());
  }

  body->add(dump.slice());
  return Result();
}

arangodb::Result ClusterInfo::agencyReplan(VPackSlice const plan) {
  // Apply only Collections and DBServers
  AgencyWriteTransaction planTransaction(std::vector<AgencyOperation>{
      AgencyOperation("Plan/Collections", AgencyValueOperationType::SET,
                      plan.get(std::vector<std::string>{"arango", "Plan",
                                                        "Collections"})),
      AgencyOperation("Plan/Databases", AgencyValueOperationType::SET,
                      plan.get(std::vector<std::string>{"arango", "Plan",
                                                        "Databases"})),
      AgencyOperation("Plan/Views", AgencyValueOperationType::SET,
                      plan.get(
                          std::vector<std::string>{"arango", "Plan", "Views"})),
      AgencyOperation("Plan/Version", AgencySimpleOperationType::INCREMENT_OP),
      AgencyOperation("Sync/UserVersion", AgencySimpleOperationType::INCREMENT_OP)});

  AgencyCommResult r = _agency.sendTransactionWithFailover(planTransaction);
  if (!r.successful()) {
    arangodb::Result result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                            std::string(
                                "Error reporting to agency: _statusCode: ") +
                                std::to_string(r.errorCode()));
    return result;
  }

  return arangodb::Result();
}

std::string const backupKey = "/arango/Target/HotBackup/Create/";
std::string const maintenanceKey = "/arango/Supervision/Maintenance";
std::string const supervisionMode = "/arango/Supervision/State/Mode";
std::string const toDoKey = "/arango/Target/ToDo";
std::string const pendingKey = "/arango/Target/Pending";
std::string const writeURL = "_api/agency/write";
std::vector<std::string> modepv = {"arango", "Supervision", "State", "Mode"};

arangodb::Result ClusterInfo::agencyHotBackupLock(std::string const& backupId,
                                                  double const& timeout,
                                                  bool& supervisionOff) {
  using namespace std::chrono;

  auto const endTime =
      steady_clock::now() + milliseconds(static_cast<uint64_t>(1.0e3 * timeout));
  supervisionOff = false;

  LOG_TOPIC("e74e5", DEBUG, Logger::BACKUP)
      << "initiating agency lock for hot backup " << backupId;

  auto const timeouti = static_cast<long>(std::ceil(timeout));

  VPackBuilder builder;
  {
    VPackArrayBuilder trxs(&builder);
    {
      VPackArrayBuilder trx(&builder);

      // Operations
      {
        VPackObjectBuilder o(&builder);
        builder.add(                                      // Backup lock
          backupKey + backupId,
          VPackValue(
            timepointToString(
              std::chrono::system_clock::now() + std::chrono::seconds(timeouti))));
        builder.add(                                      // Turn off supervision
          maintenanceKey,
          VPackValue(
            timepointToString(
              std::chrono::system_clock::now() + std::chrono::seconds(timeouti))));
      }

      // Preconditions
      {
        VPackObjectBuilder precs(&builder);
        builder.add(VPackValue(backupKey));  // Backup key empty
        {
          VPackObjectBuilder oe(&builder);
          builder.add("oldEmpty", VPackValue(true));
        }
        builder.add(VPackValue(pendingKey));  // No jobs pending
        {
          VPackObjectBuilder oe(&builder);
          builder.add("old", VPackSlice::emptyObjectSlice());
        }
        builder.add(VPackValue(toDoKey));  // No jobs to do
        {
          VPackObjectBuilder oe(&builder);
          builder.add("old", VPackSlice::emptyObjectSlice());
        }
        builder.add(VPackValue(supervisionMode));  // Supervision on
        {
          VPackObjectBuilder old(&builder);
          builder.add("old", VPackValue("Normal"));
        }
      }
    }

    {
      VPackArrayBuilder trx(&builder);

      // Operations
      {
        VPackObjectBuilder o(&builder);
        builder.add(                                      // Backup lock
          backupKey + backupId,
          VPackValue(
            timepointToString(
              std::chrono::system_clock::now() + std::chrono::seconds(timeouti))));
        builder.add(                                      // Turn off supervision
          maintenanceKey,
          VPackValue(
            timepointToString(
              std::chrono::system_clock::now() + std::chrono::seconds(timeouti))));
      }

      // Prevonditions
      {
        VPackObjectBuilder precs(&builder);
        builder.add(VPackValue(backupKey));  // Backup key empty
        {
          VPackObjectBuilder oe(&builder);
          builder.add("oldEmpty", VPackValue(true));
        }
        builder.add(VPackValue(pendingKey));  // No jobs pending
        {
          VPackObjectBuilder oe(&builder);
          builder.add("old", VPackSlice::emptyObjectSlice());
        }
        builder.add(VPackValue(toDoKey));  // No jobs to do
        {
          VPackObjectBuilder oe(&builder);
          builder.add("old", VPackSlice::emptyObjectSlice());
        }
        builder.add(VPackValue(supervisionMode));  // Supervision off
        {
          VPackObjectBuilder old(&builder);
          builder.add("old", VPackValue("Maintenance"));
        }
      }
    }
  }

  // Try to establish hot backup lock in agency.
  auto result = _agency.sendWithFailover(arangodb::rest::RequestType::POST,
                                         timeout, writeURL, builder.slice());

  LOG_TOPIC("53a93", DEBUG, Logger::BACKUP)
      << "agency lock for hot backup " << backupId << " scheduled with "
      << builder.toJson();

  // *** ATTENTION ***: Result will always be 412.
  // So we're going to fail, if we have an error OTHER THAN 412:
  if (!result.successful() &&
      result.httpCode() != (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
    return arangodb::Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                            "failed to acquire backup lock in agency");
  }

  auto rv = VPackParser::fromJson(result.bodyRef());

  LOG_TOPIC("a94d5", DEBUG, Logger::BACKUP)
      << "agency lock response for backup id " << backupId << ": " << rv->toJson();

  if (!rv->slice().isObject() || !rv->slice().hasKey("results") ||
      !rv->slice().get("results").isArray() || rv->slice().get("results").length() != 2) {
    return arangodb::Result(
      TRI_ERROR_HOT_BACKUP_INTERNAL,
      "invalid agency result while acquiring backup lock");
  }
  auto ar = rv->slice().get("results");

  uint64_t first = ar[0].getNumber<uint64_t>();
  uint64_t second = ar[1].getNumber<uint64_t>();

  if (first == 0 && second == 0) {  // tough luck
    return arangodb::Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                            "preconditions failed while trying to acquire "
                            "backup lock in the agency");
  }

  if (first > 0) {  // Supervision was on
    LOG_TOPIC("b6c98", DEBUG, Logger::BACKUP)
        << "agency lock found supervision on before";
    supervisionOff = false;
  } else {
    LOG_TOPIC("bbb55", DEBUG, Logger::BACKUP)
        << "agency lock found supervision off before";
    supervisionOff = true;
  }

  double wait = 0.1;
  while (!_server.isStopping() && std::chrono::steady_clock::now() < endTime) {
    result = _agency.getValues("Supervision/State/Mode");
    if (result.successful()) {
      if (!result.slice().isArray() || result.slice().length() != 1) {
        return arangodb::Result(
            TRI_ERROR_HOT_BACKUP_INTERNAL,
            std::string(
                "invalid JSON from agency, when acquiring supervision mode: ") +
                result.slice().toJson());
      }
      if (result.slice()[0].hasKey(modepv) && result.slice()[0].get(modepv).isString()) {
        if (result.slice()[0].get(modepv).isEqualString("Maintenance")) {
          LOG_TOPIC("76a2c", DEBUG, Logger::BACKUP)
              << "agency hot backup lock acquired";
          return arangodb::Result();
        }
      }
    }

    LOG_TOPIC("ede54", DEBUG, Logger::BACKUP)
        << "agency hot backup lock waiting: " << result.slice().toJson();

    if (wait < 2.0) {
      wait *= 1.1;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
  }

  agencyHotBackupUnlock(backupId, timeout, supervisionOff);

  return arangodb::Result(
      TRI_ERROR_HOT_BACKUP_INTERNAL,
      "timeout waiting for maintenance mode to be activated in agency");
}

arangodb::Result ClusterInfo::agencyHotBackupUnlock(std::string const& backupId,
                                                    double const& timeout,
                                                    const bool& supervisionOff) {
  using namespace std::chrono;

  auto const endTime =
      steady_clock::now() + milliseconds(static_cast<uint64_t>(1.0e3 * timeout));

  LOG_TOPIC("6ae41", DEBUG, Logger::BACKUP)
      << "unlocking backup lock for backup " + backupId + "  in agency";

  VPackBuilder builder;
  {
    VPackArrayBuilder trxs(&builder);
    {
      VPackArrayBuilder trx(&builder);
      {
        VPackObjectBuilder o(&builder);
        builder.add(VPackValue(backupKey));  // Remove backup key
        {
          VPackObjectBuilder oo(&builder);
          builder.add("op", VPackValue("delete"));
        }
        if (!supervisionOff) {  // Turn supervision on, if it was on before
          builder.add(VPackValue(maintenanceKey));
          VPackObjectBuilder d(&builder);
          builder.add("op", VPackValue("delete"));
        }
      }
    }
  }

  // Try to establish hot backup lock in agency. Result will always be 412.
  // Question is: How 412?
  auto result = _agency.sendWithFailover(arangodb::rest::RequestType::POST,
                                         timeout, writeURL, builder.slice());
  if (!result.successful() &&
      result.httpCode() != (int)arangodb::rest::ResponseCode::PRECONDITION_FAILED) {
    return arangodb::Result(TRI_ERROR_HOT_BACKUP_INTERNAL,
                            "failed to release backup lock in agency");
  }

  auto rv = VPackParser::fromJson(result.bodyRef());

  if (!rv->slice().isObject() || !rv->slice().hasKey("results") ||
      !rv->slice().get("results").isArray()) {
    return arangodb::Result(
        TRI_ERROR_HOT_BACKUP_INTERNAL,
        "invalid agency result while releasing backup lock");
  }

  auto ar = rv->slice().get("results");
  if (!ar[0].isNumber()) {
    return arangodb::Result(
        TRI_ERROR_HOT_BACKUP_INTERNAL,
        "invalid agency result while releasing backup lock");
  }

  if (supervisionOff) {
    return arangodb::Result();
  }

  double wait = 0.1;
  while (!_server.isStopping() && std::chrono::steady_clock::now() < endTime) {
    result = _agency.getValues("Supervision/State/Mode");
    if (result.successful()) {
      if (!result.slice().isArray() || result.slice().length() != 1 ||
          !result.slice()[0].hasKey(modepv) || !result.slice()[0].get(modepv).isString()) {
        return arangodb::Result(
          TRI_ERROR_HOT_BACKUP_INTERNAL,
          std::string("invalid JSON from agency, when deactivating supervision mode:") +
          result.slice().toJson());
      }

      if (result.slice()[0].get(modepv).isEqualString("Normal")) {
        return arangodb::Result();
      }
    }

    if (wait < 2.0) {
      wait *= 1.1;
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
  }

  return arangodb::Result(
      TRI_ERROR_HOT_BACKUP_INTERNAL,
      "timeout waiting for maintenance mode to be deactivated in agency");
}

application_features::ApplicationServer& ClusterInfo::server() const {
  return _server;
}

ClusterInfo::ServersKnown::ServersKnown(VPackSlice const serversKnownSlice,
                                        std::unordered_set<ServerID> const& serverIds)
    : _serversKnown() {
  TRI_ASSERT(serversKnownSlice.isNone() || serversKnownSlice.isObject());
  if (serversKnownSlice.isObject()) {
    for (auto const it : VPackObjectIterator(serversKnownSlice)) {
      std::string serverId = it.key.copyString();
      VPackSlice const knownServerSlice = it.value;
      TRI_ASSERT(knownServerSlice.isObject());
      if (knownServerSlice.isObject()) {
        VPackSlice const rebootIdSlice = knownServerSlice.get("rebootId");
        TRI_ASSERT(rebootIdSlice.isInteger());
        if (rebootIdSlice.isInteger()) {
          auto const rebootId = RebootId{rebootIdSlice.getNumericValue<uint64_t>()};
          _serversKnown.try_emplace(std::move(serverId), rebootId);
        }
      }
    }
  }

  // For backwards compatibility / rolling upgrades, add servers that aren't in
  // ServersKnown but in ServersRegistered with a reboot ID of 0 as a fallback.
  // We should be able to remove this in 3.6.
  for (auto const& serverId : serverIds) {
    auto const rv = _serversKnown.try_emplace(serverId, RebootId{0});
    LOG_TOPIC_IF("0acbd", INFO, Logger::CLUSTER, rv.second)
        << "Server " << serverId
        << " is in Current/ServersRegistered, but not in "
           "Current/ServersKnown. This is expected to happen "
           "during a rolling upgrade.";
  }
}

std::unordered_map<ServerID, ClusterInfo::ServersKnown::KnownServer> const&
ClusterInfo::ServersKnown::serversKnown() const noexcept {
  return _serversKnown;
}

std::unordered_map<ServerID, RebootId> ClusterInfo::ServersKnown::rebootIds() const {
  std::unordered_map<ServerID, RebootId> rebootIds;
  for (auto const& it : _serversKnown) {
    rebootIds.try_emplace(it.first, it.second.rebootId());
  }
  return rebootIds;
}

VPackSlice PlanCollectionReader::indexes() {
  VPackSlice res = _collection.get("indexes");
  if (res.isNone()) {
    return VPackSlice::emptyArraySlice();
  } else {
    TRI_ASSERT(res.isArray());
    return res;
  }
}

CollectionWatcher::~CollectionWatcher() {
  try {
    _agencyCallbackRegistry->unregisterCallback(_agencyCallback);
  } catch (std::exception const& ex) {
    LOG_TOPIC("42af2", WARN, Logger::CLUSTER) << "caught unexpected exception in CollectionWatcher: " << ex.what();
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
