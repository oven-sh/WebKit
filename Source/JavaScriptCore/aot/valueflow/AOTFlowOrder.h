/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowIDs.h"
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowObjectKind : uint8_t {
    // Stands for objects that nobody follows: every holder that has escaped may be meant.
    Unknown,
    Allocation,
    Closure,
    // The prototype object that a function object is created with.
    Prototype,
    Instance,
    Opaque,
    ModuleNamespace,
    Builtin,
    ModuleBuiltin,
};

// The one operation that creates an object. The function is the host for the function object of a declaration, which is created once, with its module.
struct FlowObjectCreation {
    FunctionID function;
    ValueID operation;
};

// The call that an instance stands for the calls of. The outer instance is named where the caller is such an instance too.
struct FlowCallSiteKey {
    FunctionID caller;
    ValueID call;
    FlowInstanceID outer;

    friend bool operator==(const FlowCallSiteKey&, const FlowCallSiteKey&) = default;
};

enum class FlowModuleOperationKind : uint8_t {
    Namespace,
    Import,
    Require,
    // The module is evaluated where the operation stands, and nobody is handed its value.
    Evaluate,
    ImportDynamic,
    RequireDynamic,
    RequireBuiltinModule,
    ResolveDynamic,
    Use,
};

struct FlowModuleOperation {
    FlowModuleOperationKind kind { FlowModuleOperationKind::Namespace };
    ValueID origin;
    ModuleID module;
    RuntimeModuleID runtimeModule;
};

struct FlowModuleLoadRecord {
    FlowInstanceID caller;
    ValueID origin;
    ModuleID module;

    friend bool operator==(const FlowModuleLoadRecord&, const FlowModuleLoadRecord&) = default;
};

enum class FlowCallLinkKind : uint8_t {
    Call,
    Construct,
    Accessor,
    BuiltinCallback,
    Protocol,
    Host,
};

struct FlowCallLink {
    FlowInstanceID callerInstance;
    ValueID site;
    FlowInstanceID calleeInstance;
    FunctionID caller;
    FunctionID callee;
    FlowCallLinkKind kind { FlowCallLinkKind::Call };
};

struct FlowBuiltinCallLink {
    FlowInstanceID callerInstance;
    ValueID site;

    friend auto operator<=>(const FlowBuiltinCallLink&, const FlowBuiltinCallLink&) = default;
};

struct FlowUnknownCallee {
    FlowInstanceID callerInstance;
    ValueID site;

    friend auto operator<=>(const FlowUnknownCallee&, const FlowUnknownCallee&) = default;
};

// An operation reaches the variable of a static field through the class object, so it does not name the variable. The function is null for an access of the host.
struct FlowStaticFieldAccess {
    VariableID variable;
    FunctionID function;
    ValueID origin;
    bool isStore { false };

    friend bool operator==(const FlowStaticFieldAccess&, const FlowStaticFieldAccess&) = default;
};

// An operation as the order of execution names it. The function is null for the host. The instance is named where it stands for the calls of one site.
struct FlowMemberOperation {
    FunctionID function;
    ValueID origin;
    FlowInstanceID callSiteInstance;

    friend bool operator==(const FlowMemberOperation&, const FlowMemberOperation&) = default;
};

// A named property of an object whose members the order of execution may follow. A lookup that starts at an object of the kind Unknown names that object, which stands for the holders that have escaped.
struct FlowMemberAccess {
    FlowObjectID holder;
    PropertyKeyID key;
    FlowMemberOperation operation;
    bool isStore { false };

    friend bool operator==(const FlowMemberAccess&, const FlowMemberAccess&) = default;
};

// All that the order of execution is computed from beside the program. The value of a node is not offered: what the order has to know of one, whoever solved the flow lists.
class FlowOrderInputs {
public:
    virtual ~FlowOrderInputs() = default;

    const Program& program() const { return m_program; }
    BuildContext& build() const { return m_build; }

    virtual unsigned numberOfInstances() const = 0;
    virtual bool isHost(FlowInstanceID) const = 0;
    virtual bool isHost(FunctionID) const = 0;
    virtual FunctionID functionOf(FlowInstanceID) const = 0;
    virtual const FlowCallSiteKey& callSiteOf(FlowInstanceID) const LIFETIME_BOUND = 0;
    virtual bool isReached(FunctionID) const = 0;
    virtual std::span<const FlowModuleOperation> moduleOperationsOf(FunctionID) const LIFETIME_BOUND = 0;

    virtual const BitVector& evaluatedModules() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowModuleLoadRecord> moduleLoads() const LIFETIME_BOUND = 0;
    // Somebody names modules at run time, a load under a name nobody knows or evaluated text, and the name may find this one.
    virtual bool mayBeLoadedByName(ModuleID) const = 0;
    virtual bool isPastWorkLimit(FunctionID) const = 0;
    virtual std::span<const FlowCallLink> callLinks() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowBuiltinCallLink> builtinCallLinks() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowUnknownCallee> unknownCallees() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowStaticFieldAccess> staticFieldAccesses() const LIFETIME_BOUND = 0;
    virtual bool hasClassObjectEscaped(FunctionID constructor) const = 0;

    // Only the holders that the accesses of members name are asked about.
    virtual std::span<const FlowMemberAccess> memberAccesses() const LIFETIME_BOUND = 0;
    virtual std::span<const FlowMemberOperation> uncertainMemberStores() const LIFETIME_BOUND = 0;
    virtual FlowObjectKind kindOf(FlowObjectID) const = 0;
    // Null for a function object that is created without a prototype object.
    virtual FlowObjectID findPrototype(FlowObjectID functionObject) const = 0;
    // Of a prototype object: what creates its function object. Nothing for an object that stands for the objects of several operations or of several calls, or that no operation creates.
    virtual std::optional<FlowObjectCreation> creationOf(FlowObjectID) const = 0;
    virtual bool isPrototype(FlowObjectID) const = 0;
    virtual bool hasEscaped(FlowObjectID) const = 0;
    // What is done to the unknown object is done to every object that has escaped.
    virtual bool mayChangeIntegrity(FlowObjectID) const = 0;

protected:
    FlowOrderInputs(const Program& program, BuildContext& build)
        : m_program(program)
        , m_build(build)
    {
    }

private:
    const Program& m_program;
    BuildContext& m_build;
};

Vector<ModuleID> indexRuntimeModules(const Program&);
// By function: the module whose body the function is. A file may hold the bodies of several modules, and every function that is written in the file names the module of the file.
Vector<ModuleID> indexModuleBodies(const Program&);
// The object of a module of the host that is written in JavaScript is what the body of the module makes. The table only lists its names.
RuntimeModuleID findCompiledModuleWithObject(const Program&, BuiltinID);

// CompletesBefore: the first module has run to its end, or has thrown for good, before any code of the second one reads a variable of it. An import runs its target in front of the body of the importer.
// A `require` runs it where the operation stands, and whoever names a variable of the target stands behind such an operation or behind a check of what it has initialized.
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
    void dump(PrintStream&) const;
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

struct FlowMemberLocation {
    FlowObjectID holder;
    PropertyKeyID key;

    friend auto operator<=>(const FlowMemberLocation&, const FlowMemberLocation&) = default;
};

// Variables, then the fields of environments, then the members in ascending order.
struct FlowScopeLocationNumbering {
    uint32_t numberOfVariables { 0 };
    Vector<uint32_t> firstField;
    Vector<FlowMemberLocation> members;

    uint32_t firstMember() const { return firstField.isEmpty() ? 0 : firstField.last(); }
    uint32_t numberOfLocations() const { return firstMember() + members.size(); }
    // An environment has a location per activation of its owner.
    bool isOneLocation(uint32_t location) const { return location < numberOfVariables || location >= firstMember(); }
    std::optional<uint32_t> locationOf(VariableID) const;
    std::optional<uint32_t> locationOf(LayoutID environment, uint32_t fieldIndex) const;
    std::optional<uint32_t> locationOf(FlowObjectID holder, PropertyKeyID) const;
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
    // The answer holds for a holder that is created at most once.
    FlowLocationOrder orderOf(FlowObjectID holder, PropertyKeyID) const;
    void dump(PrintStream&) const;
};

struct FlowScopeAccesses;

struct FlowScopeAccessesDeleter {
    void operator()(FlowScopeAccesses*) const;
};

using FlowScopeAccessesByFunction = Vector<std::unique_ptr<FlowScopeAccesses, FlowScopeAccessesDeleter>>;

FlowScopeOrders computeScopeOrders(const FlowOrderInputs&, const FlowModuleOrder&, FlowScopeAccessesByFunction&);

// Whether the order of execution is followed for the named properties of the object. An object of the program counts once it is the prototype of another,
// and of its properties only the lookups that start below it are listed.
enum class FlowMemberHolder : uint8_t { No, WhenPrototype, Always };
FlowMemberHolder memberHolderKindOf(FlowObjectKind);
bool followsMembersOf(const FlowOrderInputs&, FlowObjectID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
