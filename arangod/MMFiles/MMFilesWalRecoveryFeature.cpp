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

#include "MMFilesWalRecoveryFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/Exceptions.h"
#include "Basics/application-exit.h"
#include "FeaturePhases/BasicFeaturePhaseServer.h"
#include "Logger/Logger.h"
#include "MMFiles/MMFilesEngine.h"
#include "MMFiles/MMFilesLogfileManager.h"
#include "MMFiles/MMFilesPersistentIndexFeature.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/ServerIdFeature.h"
#include "RestServer/SystemDatabaseFeature.h"

using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

namespace arangodb {

MMFilesWalRecoveryFeature::MMFilesWalRecoveryFeature(application_features::ApplicationServer& server)
    : ApplicationFeature(server, "MMFilesWalRecovery") {
  setOptional(true);
  startsAfter<BasicFeaturePhaseServer>();

  startsAfter<DatabaseFeature>();
  startsAfter<MMFilesLogfileManager>();
  startsAfter<MMFilesPersistentIndexFeature>();
  startsAfter<ServerIdFeature>();
  startsAfter<SystemDatabaseFeature>();

  onlyEnabledWith<MMFilesEngine>();
  onlyEnabledWith<MMFilesLogfileManager>();
}

/// @brief run the recovery procedure
/// this is called after the logfiles have been scanned completely and
/// recovery state has been build. additionally, all databases have been
/// opened already so we can use collections
void MMFilesWalRecoveryFeature::start() {
  MMFilesLogfileManager& logfileManager = server().getFeature<MMFilesLogfileManager>();

  TRI_ASSERT(!logfileManager.allowWrites());

  int res = logfileManager.runRecovery();

  if (res != TRI_ERROR_NO_ERROR) {
    LOG_TOPIC("c6422", FATAL, arangodb::Logger::ENGINES)
        << "unable to finish WAL recovery: " << TRI_errno_string(res);
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
    FATAL_ERROR_ABORT();
#endif
    FATAL_ERROR_EXIT();
  }

  if (!logfileManager.open()) {
    // if we got here, the MMFilesLogfileManager has already logged a fatal
    // error and we can simply abort
    FATAL_ERROR_EXIT();
  }

  // notify everyone that recovery is now done
  auto& databaseFeature = server().getFeature<DatabaseFeature>();
  databaseFeature.recoveryDone();

  LOG_TOPIC("8767f", INFO, arangodb::Logger::ENGINES)
      << "DB recovery finished successfully";
}

}  // namespace arangodb
