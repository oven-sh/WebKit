/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSolution.h"
#include <memory>
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

void createAllocationClasses(FlowObjectTable&, std::span<const FlowSummary> summariesByFunction, BuildContext&);

std::unique_ptr<FlowObjectState, FlowObjectStateDeleter> createFlowObjectState(FlowSolver&);

Vector<FlowObjectID> createObjectsOfInstance(FlowSolver&, FlowInstanceID);

FlowObjectID ensureBuiltinObject(FlowSolver&, BuiltinID);
FlowObjectID ensureModuleBuiltinObject(FlowSolver&, BuiltinID, ModuleID);
FlowObjectID ensureNamespaceObject(FlowSolver&, ModuleID);
FlowObjectID ensurePrototypeObject(FlowSolver&, FlowObjectID closure);
bool hasPrototypeProperty(const Program&, const Function&);

struct FlowCallSiteAllocation {
    FlowStatementSite site;
    FlowSiteDescription description;
};

FlowObjectID allocateAtCallSite(FlowSolver&, const FlowCallSiteAllocation&);

struct FlowPlainDataObjects {
    FlowObjectID object;
    FlowObjectID array;
};

FlowPlainDataObjects allocatePlainData(FlowSolver&, FlowStatementSite, ValueID origin);
FlowObjectID allocateCoroutineResult(FlowSolver&, FlowStatementSite caller, ValueID origin, FlowInstanceID calleeInstance);
void allocateTypedResult(FlowSolver&, FlowStatementSite, ValueID origin, ExpectedResultID, FlowNodeID uncopiedValues, FlowNodeID result);
void allocateTypedResult(FlowSolver&, FlowStatementSite, ValueID origin, TypeID type, std::span<const LayoutID> layouts, FlowNodeID result);

void runAllocation(FlowSolver&, FlowStatementSite, const FlowAllocation&);
void runFieldAccess(FlowSolver&, FlowStatementSite, const FlowFieldAccess&);
void runSlotAccess(FlowSolver&, FlowStatementSite, const FlowSlotAccess&);
void runClassDefinition(FlowSolver&, FlowStatementSite, const FlowClassDefinition&);

FlowNodeID contentsNode(const FlowGraph&, FlowObjectID, ContentsSlot);
void forEachElementPart(const FlowGraph&, FlowObjectID, Range indexes, NOESCAPE const ScopedLambda<void(FlowNodeID)>&);
void joinPositions(FlowSolver&, FlowObjectID, const Witness&);
// A store into a slot, an element or an internal field, which no key names. On the unknown object the mutation alone would not tell it from a store under a name, which reaches only the escaped objects with that name.
void noteContentsStore(FlowSolver&, FlowObjectID, const Witness&);

enum class FlowFieldLookup : uint8_t {
    Unknown,
    Absent,
    Found,
};

struct FlowFieldLocation {
    FlowFieldLookup lookup { FlowFieldLookup::Unknown };
    FlowNodeID node;
    uint32_t fieldIndex { 0 };
};

FlowFieldLocation findFieldLocation(const FlowGraph&, FlowObjectID, PropertyKeyID);
FlowFieldLocation findFieldLocation(const FlowGraph&, FlowObjectID, LayoutID declaringLayout, uint32_t fieldIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
