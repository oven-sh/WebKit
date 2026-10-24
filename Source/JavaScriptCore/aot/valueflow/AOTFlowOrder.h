/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowIDs.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowModuleLoad {
    FunctionID function;
    ModuleID target;
    bool isSynchronous { false };
};

struct FlowCall {
    FunctionID caller;
    ValueID site;
    FunctionID callee;

    friend auto operator<=>(const FlowCall&, const FlowCall&) = default;
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
    Vector<FlowModuleLoad> moduleLoads;
    Vector<FlowCall> calls;
    Vector<FlowStaticFieldAccess> staticFieldAccesses;
    BitVector staticFieldsOfEscapedClasses;

    FunctionID host() const { return FunctionID(program.functions.size()); }
};

struct FlowModuleOrder {
    Vector<uint32_t> componentOf;
    BitVector modulesInCycles;
    Vector<BitVector> componentsCompletedBefore;

    bool completesBefore(ModuleID first, ModuleID second) const;
    bool runsAfterRequests(ModuleID module) const { return module.index() < componentOf.size() && !modulesInCycles.get(module.index()); }
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
