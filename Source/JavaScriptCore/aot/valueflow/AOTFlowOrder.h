/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowIDs.h"
#include "AOTFlowModuleOrder.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <tuple>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

#define FOR_EACH_AOT_WAY_INTO_FILE(v) \
    v(EntryPoint) \
    v(StaticImport) \
    v(DeferredImport) \
    v(Require) \
    v(ImportCall) \
    v(NameMadeAtRunTime) \
    v(Host)

#define FOR_EACH_AOT_RUNNER_OF_JOBS(v) \
    v(EndOfTurn) \
    v(RequireOfESModule) \
    v(NameMadeAtRunTime) \
    v(Builtin)

#define AOT_LIST_ENUMERATOR(name) name,
// A file whose body runs and that no way in has spoken for may begin at any time.
enum class FlowWayIntoFile : uint8_t { FOR_EACH_AOT_WAY_INTO_FILE(AOT_LIST_ENUMERATOR) };
enum class FlowRunnerOfJobs : uint8_t { FOR_EACH_AOT_RUNNER_OF_JOBS(AOT_LIST_ENUMERATOR) };
#undef AOT_LIST_ENUMERATOR

#define AOT_LIST_WAY(name) FlowWayIntoFile::name,
#define AOT_LIST_RUNNER(name) FlowRunnerOfJobs::name,
inline constexpr std::array allWaysIntoFile { FOR_EACH_AOT_WAY_INTO_FILE(AOT_LIST_WAY) };
inline constexpr std::array allRunnersOfJobs { FOR_EACH_AOT_RUNNER_OF_JOBS(AOT_LIST_RUNNER) };
#undef AOT_LIST_WAY
#undef AOT_LIST_RUNNER

struct FlowModuleLoad {
    FunctionID function;
    ModuleID target;
    FlowWayIntoFile way { FlowWayIntoFile::NameMadeAtRunTime };
};

struct FlowCall {
    FunctionID caller;
    ValueID site;
    FunctionID callee;

    friend auto operator<=>(const FlowCall&, const FlowCall&) = default;
};

// A call with many callees. Calls with the same callees share the list: n calls of any of n functions would be n * n FlowCalls.
struct FlowCallOfList {
    FunctionID caller;
    ValueID site;
    uint32_t calleeList { 0 };

    friend auto operator<=>(const FlowCallOfList&, const FlowCallOfList&) = default;
};

struct FlowStaticFieldAccess {
    VariableID variable;
    FunctionID function;
    ValueID origin;
    bool isStore { false };
};

struct FlowOrderInputs {
    const Program& program;
    BuildContext& build;
    BitVector reachedFunctions;
    BitVector evaluatedModules;
    BitVector modulesLoadedByName;
    BitVector modulesLoadedByHost;
    BitVector modulesNamedByOperationsNobodyKnows;
    bool hostRunsWaitingJobs { false };
    Vector<FlowModuleLoad> moduleLoads;
    Vector<FlowCall> calls;
    Vector<FlowCallOfList> callsOfLists;
    Vector<Vector<FunctionID>> calleeLists;
    Vector<FlowStaticFieldAccess> staticFieldAccesses;
    BitVector staticFieldsOfEscapedClasses;
    BitVector membersOfFunctionsDefinedByStores;

    FunctionID host() const { return FunctionID(program.functions.size()); }

    // Every call, by caller and site, but those of a list that the second functor leaves out.
    void forEachCall(NOESCAPE const Invocable<void(const FlowCall&)> auto& functor, NOESCAPE const Invocable<bool(const FlowCallOfList&)> auto& visitsCallees) const
    {
        size_t next = 0;
        auto visitCallsOfListsBefore = [&](const FlowCall* call) {
            for (; next < callsOfLists.size(); ++next) {
                const FlowCallOfList& callOfList = callsOfLists[next];
                if (call && std::tuple(call->caller, call->site) <= std::tuple(callOfList.caller, callOfList.site))
                    return;
                if (!visitsCallees(callOfList))
                    continue;
                for (FunctionID callee : calleeLists[callOfList.calleeList])
                    functor(FlowCall { callOfList.caller, callOfList.site, callee });
            }
        };
        for (const FlowCall& call : calls) {
            visitCallsOfListsBefore(&call);
            functor(call);
        }
        visitCallsOfListsBefore(nullptr);
    }
};

FlowModuleOrder computeModuleOrder(const FlowOrderInputs&);

inline bool listsStartOfVar(const Function& function, const Value& allocation, size_t index)
{
    return function.at(allocation.variadicOperands()[index]).opcode == Opcode::Undefined;
}

struct FlowLocationOrder {
    InitializationState initialization { InitializationState::Unknown };
    StoreCount stores { StoreCount::Unknown };
    bool undefinedAtStartIsNeverRead { false };
    bool isStoredBeforeCallersOfReaders { false };
    bool isStoredBeforeCapturersRun { false };
    bool holdsClosureOverOwnEnvironment { false };
};

struct FlowScopeLocationNumbering {
    uint32_t numberOfVariables { 0 };
    Vector<uint32_t> firstField;

    uint32_t numberOfLocations() const { return firstField.isEmpty() ? 0 : firstField.last(); }
    bool isOneLocation(uint32_t location) const { return location < numberOfVariables; }
    std::optional<uint32_t> locationOf(VariableID variable) const { return variable.index() < numberOfVariables ? std::optional(variable.index()) : std::nullopt; }
    std::optional<uint32_t> locationOf(LayoutID environment, uint32_t fieldIndex) const;
};

struct FlowReadOfLocation {
    FunctionID function;
    ValueID value;

    friend auto operator<=>(const FlowReadOfLocation&, const FlowReadOfLocation&) = default;
};

struct FlowScopeOrders {
    FlowScopeLocationNumbering numbering;
    Vector<FlowLocationOrder> orders;
    Vector<FlowReadOfLocation> readsAfterInitialization;

    FlowLocationOrder orderAt(std::optional<uint32_t> location) const { return location ? orders[*location] : FlowLocationOrder { }; }
    FlowLocationOrder orderOf(VariableID variable) const { return orderAt(numbering.locationOf(variable)); }
    FlowLocationOrder orderOf(LayoutID environment, uint32_t fieldIndex) const { return orderAt(numbering.locationOf(environment, fieldIndex)); }
};

FlowScopeOrders computeScopeOrders(const FlowOrderInputs&, const FlowModuleOrder&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
