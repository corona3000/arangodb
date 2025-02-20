////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
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
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
////////////////////////////////////////////////////////////////////////////////

#include "shared.hpp"
#include "formats_10_attributes.hpp"

NS_ROOT
NS_BEGIN(version10)

// -----------------------------------------------------------------------------
// --SECTION--                                                         documents
// -----------------------------------------------------------------------------

REGISTER_ATTRIBUTE(documents);
DEFINE_ATTRIBUTE_TYPE(documents)

// -----------------------------------------------------------------------------
// --SECTION--                                                         term_meta
// -----------------------------------------------------------------------------

NS_END // version10
NS_END // ROOT

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
