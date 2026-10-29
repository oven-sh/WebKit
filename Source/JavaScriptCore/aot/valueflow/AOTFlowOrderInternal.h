/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowOrder.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include <compare>
#include <type_traits>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

using FlowScopeLocation = uint32_t;

template<typename Subject>
struct FlowOperationOn {
    Subject subject;
    FunctionID function;
    ValueID value;

    friend auto operator<=>(const FlowOperationOn&, const FlowOperationOn&) = default;
};
using FlowScopeLocationAccess = FlowOperationOn<FlowScopeLocation>;
using FlowEnvironmentAllocation = FlowOperationOn<LayoutID>;
using FlowModuleValueRead = FlowOperationOn<ModuleID>;

template<typename Subject>
std::span<const FlowOperationOn<Subject>> operationsOn(const Vector<FlowOperationOn<Subject>>& sorted LIFETIME_BOUND, std::type_identity_t<Subject> subject)
{
    return recordsWith(sorted.span(), subject, &FlowOperationOn<Subject>::subject);
}

struct FlowFunctionMention {
    FunctionID function;
    FunctionReferenceKind kind;
    ValueID value;
};

using FlowCallSite = std::pair<FunctionID, ValueID>;

struct FlowScopeCall {
    FunctionID caller;
    FunctionID callee;
    ValueID site;
    VariableID subject;

    friend auto operator<=>(const FlowScopeCall&, const FlowScopeCall&) = default;
};

VariableID variableBehindSubjectOf(const Function&, ValueID operation);

struct FlowScopeCalls {
    Vector<uint32_t> numberOfMentions;
    Vector<FlowCallSite> soleCallSites;
    Vector<FlowScopeCall> byCaller;
    Vector<FlowScopeCall> byCallee;
    Vector<uint32_t> firstByCaller;
    Vector<uint32_t> firstByCallee;
    BitVector functionsEnteredByHost;

    std::span<const FlowScopeCall> callsBy(FunctionID caller) const LIFETIME_BOUND { return entriesOfGroup(firstByCaller, byCaller, caller.index()); }
    std::span<const FlowScopeCall> callsOf(FunctionID callee) const LIFETIME_BOUND { return entriesOfGroup(firstByCallee, byCallee, callee.index()); }
};

struct FlowExportedVariable {
    VariableID variable;
    ModuleID exporter;

    friend auto operator<=>(const FlowExportedVariable&, const FlowExportedVariable&) = default;
};

struct FlowScopeOwner {
    FunctionID function;
    ModuleID module;
    Vector<VariableID> variables;
    Vector<LayoutID> environments;
    Vector<FlowExportedVariable> exports;
    Vector<ModuleID> exporters;
};

struct FlowScopeIndex {
    const FlowOrderInputs& inputs;
    const FlowModuleOrder& moduleOrder;
    BitVector bodies;
    Vector<FlowFunctionMention> mentions;
    Vector<uint32_t> firstMention;
    FlowScopeLocationNumbering numbering;
    Vector<FlowScopeLocationAccess> reads;
    Vector<FlowScopeLocationAccess> stores;
    Vector<FlowScopeLocationAccess> accessesThroughClassObjects;
    Vector<FlowScopeLocationAccess> storesThroughClassObjects;
    Vector<FlowEnvironmentAllocation> allocations;
    Vector<FlowModuleValueRead> moduleValueReads;
    BitVector opaqueLocations;
    BitVector syntheticVariables;
    BitVector functionsEnteredWithoutMention;
    BitVector modulesOfFunctionsEnteredWithoutMention;
    // A file of a cycle runs before files that another file of the cycle asks for, and may call their functions through that file.
    BitVector functionsThatFilesOfCyclesMayEnter;
    Vector<FunctionID> locationOwners;
    Vector<FunctionID> environmentOwners;
    FlowScopeCalls calls;
    Vector<FlowScopeOwner> owners;

    const Program& program() const { return inputs.program; }
    std::span<const FlowFunctionMention> mentionsIn(FunctionID function) const LIFETIME_BOUND { return entriesOfGroup(firstMention, mentions, function.index()); }
};

FlowScopeIndex createScopeIndex(const FlowOrderInputs&, const FlowModuleOrder&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
