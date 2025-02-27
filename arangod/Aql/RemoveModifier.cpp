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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "RemoveModifier.h"

#include "Aql/AqlValue.h"
#include "Aql/Collection.h"
#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutorAccumulator.h"
#include "Aql/ModificationExecutorHelpers.h"
#include "Aql/OutputAqlItemRow.h"
#include "Basics/Common.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Collection.h>
#include <velocypack/velocypack-aliases.h>

#include "Logger/LogMacros.h"

class CollectionNameResolver;

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::aql::ModificationExecutorHelpers;

ModifierOperationType RemoveModifierCompletion::accumulate(ModificationExecutorAccumulator& accu,
                                                           InputAqlItemRow& row) {
  std::string key{}, rev{};
  Result result;

  RegisterId const inDocReg = _infos._input1RegisterId;

  // The document to be REMOVEd
  AqlValue const& inDoc = row.getValue(inDocReg);

  if (writeRequired(_infos, inDoc.slice(), StaticStrings::Empty)) {
    TRI_ASSERT(_infos._trx->resolver() != nullptr);
    CollectionNameResolver const& collectionNameResolver{*_infos._trx->resolver()};

    result = getKeyAndRevision(collectionNameResolver, inDoc, key, rev);
    if (!result.ok()) {
      if (!_infos._ignoreErrors) {
        THROW_ARANGO_EXCEPTION_MESSAGE(result.errorNumber(), result.errorMessage());
      }
      return ModifierOperationType::SkipRow;
    }

    if (_infos._options.ignoreRevs) {
      rev.clear();
    }

    VPackBuilder keyDocBuilder;
    buildKeyAndRevDocument(keyDocBuilder, key, rev);
    // This deletes _rev if rev is empty or ignoreRevs is set in options.
    accu.add(keyDocBuilder.slice());
    return ModifierOperationType::ReturnIfAvailable;
  } else {
    return ModifierOperationType::CopyRow;
  }
}

OperationResult RemoveModifierCompletion::transact(VPackSlice const& data) {
  return _infos._trx->remove(_infos._aqlCollection->name(), data, _infos._options);
}
