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
#include <wtf/IterationStatus.h>

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

struct FlowScopeCallOfList {
    FunctionID caller;
    ValueID site;
    VariableID subject;
    uint32_t calleeList { 0 };
};

struct FlowScopeCalls {
    Vector<uint32_t> numberOfMentions;
    Vector<FlowCallSite> soleCallSites;
    Vector<FlowScopeCall> byCaller;
    Vector<FlowScopeCall> byCallee;
    Vector<uint32_t> firstByCaller;
    Vector<uint32_t> firstByCallee;
    // The calls with many callees, by caller. A callee finds them through the lists that hold it.
    Vector<FlowScopeCallOfList> callsOfLists;
    Vector<uint32_t> firstCallOfListByCaller;
    Vector<uint32_t> callsOfListsByList;
    Vector<uint32_t> firstCallOfList;
    Vector<uint32_t> listsByCallee;
    Vector<uint32_t> firstListByCallee;
    BitVector functionsEnteredByHost;

    void forEachCallBy(const Vector<Vector<FunctionID>>& calleeLists, FunctionID caller, NOESCAPE const Invocable<IterationStatus(const FlowScopeCall&)> auto& functor) const
    {
        for (const FlowScopeCall& call : entriesOfGroup(firstByCaller, byCaller, caller.index())) {
            if (functor(call) == IterationStatus::Done)
                return;
        }
        for (const FlowScopeCallOfList& call : entriesOfGroup(firstCallOfListByCaller, callsOfLists, caller.index())) {
            for (FunctionID callee : calleeLists[call.calleeList]) {
                if (functor(FlowScopeCall { call.caller, callee, call.site, call.subject }) == IterationStatus::Done)
                    return;
            }
        }
    }

    void forEachCallOf(FunctionID callee, NOESCAPE const Invocable<IterationStatus(const FlowScopeCall&)> auto& functor) const
    {
        for (const FlowScopeCall& call : entriesOfGroup(firstByCallee, byCallee, callee.index())) {
            if (functor(call) == IterationStatus::Done)
                return;
        }
        for (uint32_t list : entriesOfGroup(firstListByCallee, listsByCallee, callee.index())) {
            for (uint32_t position : entriesOfGroup(firstCallOfList, callsOfListsByList, list)) {
                const FlowScopeCallOfList& call = callsOfLists[position];
                if (functor(FlowScopeCall { call.caller, callee, call.site, call.subject }) == IterationStatus::Done)
                    return;
            }
        }
    }
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
