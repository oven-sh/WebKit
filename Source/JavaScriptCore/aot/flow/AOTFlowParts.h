/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowObjectOperations.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

BuiltinSlotID indexedElementSlotOf(const BuiltinTable&, BuiltinClassID);

struct FlowPartAccess {
    FlowStatementSite site;
    ValueID origin;
    FlowAccessKind kind { FlowAccessKind::Load };
    FlowReadUse use { FlowReadUse::Value };
    FlowLocalID storedLocal;
    FlowNodeID value;
    FlowStoreNaming naming { FlowStoreNaming::Unknown };
};

struct FlowRecordedStore {
    FlowStatementSite site;
    ValueID origin;
    FlowLocalID storedLocal;
    FlowStoreNaming naming { FlowStoreNaming::Unknown };
};

void accessPart(FlowSolver&, const FlowPartAccess&, FlowNodeID part);

// How many leading elements have a part each. Nothing for an object that keeps no elements in parts.
std::optional<uint32_t> numberOfFilledElements(const FlowSolver&, FlowObjectID);
// Loads every part that may hold an element at one of the indexes, and undefined where the indexes pass the filled elements.
void loadElementParts(FlowSolver&, const FlowPartAccess&, FlowObjectID list, Range indexes);

void recordStore(FlowSolver&, const FlowRecordedStore&, FlowNodeID location);
void recordStoredValue(FlowLocationRecord&, const FlowClosureCreation*);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
