/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationData.h"
#include "JSCJSValue.h"

namespace JSC {

class JSArray;
class JSGlobalObject;
class JSMap;
class JSObject;
class JSSet;

namespace AOT {

struct State;

JSValue constantOfDataWord(State&, uint64_t word);
JSObject* tryBuildData(JSGlobalObject*, State&, const DataRequest&);
JSSet* tryBuildSet(JSGlobalObject*, State&, const CollectionDataRequest&);
JSMap* tryBuildMap(JSGlobalObject*, State&, const CollectionDataRequest&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
