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
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>

namespace JSC { namespace AOT { namespace Typed {

// A function loads a module where the operation stands. An import is asynchronous: only a body that may suspend waits for it.
struct FlowModuleLoad {
    FunctionID function;
    ModuleID target;
    bool isSynchronous { false };
};

// The caller is host() and the site none for an entry that no operation of the program spells out.
struct FlowCall {
    FunctionID caller;
    ValueID site;
    FunctionID callee;

    friend auto operator<=>(const FlowCall&, const FlowCall&) = default;
};

// An operation reaches the variable of a static field through the class object, so it does not name the variable. The function is null for an access of the host.
struct FlowStaticFieldAccess {
    VariableID variable;
    FunctionID function;
    ValueID origin;
    bool isStore { false };
};

// All that the order of execution is computed from beside the program.
struct FlowOrderInputs {
    const Program& program;
    BuildContext& build;
    BitVector reachedFunctions;
    BitVector evaluatedModules;
    // Somebody names modules at run time, a load under a name nobody knows or evaluated text, and the name may find one of these.
    BitVector modulesLoadedByName;
    Vector<ModuleID> modulesLoadedByHost;
    // Ascending by function.
    Vector<FlowModuleLoad> moduleLoads;
    // Ascending, without duplicates.
    Vector<FlowCall> calls;
    Vector<FlowStaticFieldAccess> staticFieldAccesses;
    // By variable. Code that the build cannot see hands a class object back as an unknown object, and an access to that one is in no list.
    BitVector staticFieldsOfEscapedClasses;

    FunctionID host() const;
};

Vector<ModuleID> indexRuntimeModules(const Program&);
// By function: the module whose body the function is. A file may hold the bodies of several modules, and every function that is written in the file names the module of the file.
Vector<ModuleID> indexModuleBodies(const Program&);
// The object of a module of the host that is written in JavaScript is what the body of the module makes. The table only lists its names.
RuntimeModuleID findCompiledModuleWithObject(const Program&, BuiltinID);

// CompletesBefore: the first module has run to its end, or has thrown for good, before any code of the second one reads a variable of it.
enum class FlowModuleOrdering : uint8_t {
    Unknown,
    CompletesBefore,
};

struct FlowModuleOrder {
    Vector<uint32_t> componentOf;
    BitVector modulesInCycles;
    Vector<BitVector> componentsCompletedBefore;

    FlowModuleOrdering orderingOf(ModuleID first, ModuleID second) const;
    bool runsAfterRequests(ModuleID) const;
};

FlowModuleOrder computeModuleOrder(const FlowOrderInputs&);

enum class FlowScopeEntryState : uint8_t {
    Unknown,
    AfterPoint,
    Never,
};

// The point lies in the body of the owner, or of a function that runs only as a part of the owner's activation: its one mention in reached code is a direct call there.
struct FlowScopeEntry {
    FlowScopeEntryState state { FlowScopeEntryState::Unknown };
    FunctionID function;
    ProgramPoint point;

    friend bool operator==(const FlowScopeEntry&, const FlowScopeEntry&) = default;
};

// The allocation of an environment lists a `var` with the `undefined` that it starts as. No code of the program has stored that value.
inline bool listsStartOfVar(const Function& function, const Value& allocation, size_t index)
{
    return function.at(allocation.variadicOperands()[index]).opcode == Opcode::Undefined;
}

struct FlowLocationOrder {
    InitializationState initialization { InitializationState::Unknown };
    StoreCount stores { StoreCount::Unknown };
    // Of a field of an environment that every allocation lists with the `undefined` that a `var` starts as: a store comes before every read, so nobody sees that `undefined`.
    bool undefinedAtStartIsNeverRead { false };

    friend bool operator==(const FlowLocationOrder&, const FlowLocationOrder&) = default;
};

// Variables, then the fields of environments.
struct FlowScopeLocationNumbering {
    uint32_t numberOfVariables { 0 };
    Vector<uint32_t> firstField;

    uint32_t numberOfLocations() const { return firstField.isEmpty() ? 0 : firstField.last(); }
    // An environment has a location per activation of its owner.
    bool isOneLocation(uint32_t location) const { return location < numberOfVariables; }
    std::optional<uint32_t> locationOf(VariableID) const;
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
    // Of the variables and the fields of environments that start absent and that some read may find so: the reads that a store comes before. Ascending.
    Vector<FlowReadOfLocation> readsAfterInitialization;

    FlowLocationOrder orderOf(VariableID) const;
    FlowLocationOrder orderOf(LayoutID environment, uint32_t fieldIndex) const;
};

FlowScopeOrders computeScopeOrders(const FlowOrderInputs&, const FlowModuleOrder&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
