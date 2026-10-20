/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "JSExportMacros.h"
#include <wtf/Forward.h>

namespace JSC {

class JSCell;
class VM;

namespace AOT {

struct Instance;

namespace HeapHooks {

void finalize(VM&, CollectionScope);
void didChangeBarrierThreshold(VM&, unsigned threshold, bool mutatorShouldBeFenced);
void willAnalyzeHeap(VM&);
String nameOfCompiledCallee(JSCell*);
[[noreturn]] void crashOnCellVisitMismatch(VM&, const JSCell*);

} // namespace HeapHooks

JS_EXPORT_PRIVATE void addMarkingConstraints(VM&);
JS_EXPORT_PRIVATE void initializeCollectorWords(VM&, Instance&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
