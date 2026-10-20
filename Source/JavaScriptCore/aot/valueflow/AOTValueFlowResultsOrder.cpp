/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowModules.h"
#include "AOTFlowObjects.h"
#include "AOTFlowOrder.h"
#include "AOTThreads.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The order of execution is computed from who calls whom and which module loads which. A function has one instance, with the number of the function, and the host has the one after the last.
// The members of objects are not followed, so every list about them is empty and no object is ever asked about.
class OrderInputs final : public FlowOrderInputs {
public:
    OrderInputs(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, const FlowResultTables&);

    unsigned numberOfInstances() const final { return m_numberOfFunctions + 1; }
    bool isHost(FlowInstanceID instance) const final { return instance.index() == m_numberOfFunctions; }
    bool isHost(FunctionID function) const final { return function.index() == m_numberOfFunctions; }
    FunctionID functionOf(FlowInstanceID instance) const final { return FunctionID(instance.index()); }
    const FlowCallSiteKey& callSiteOf(FlowInstanceID) const LIFETIME_BOUND final { RELEASE_ASSERT_NOT_REACHED(); }
    bool isReached(FunctionID function) const final { return m_isCompiled.quickGet(function.index()); }
    std::span<const FlowModuleOperation> moduleOperationsOf(FunctionID function) const LIFETIME_BOUND final { return m_moduleOperations[function.index()].span(); }
    const FlowObjectTable& objects() const LIFETIME_BOUND final { return m_noObjects; }
    bool isAllocated(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }

    const BitVector& evaluatedModules() const LIFETIME_BOUND final { return m_evaluatedModules; }
    std::span<const FlowModuleLoadRecord> moduleLoads() const LIFETIME_BOUND final { return m_moduleLoads.span(); }
    bool mayLoadAnyModule() const final { return m_mayLoadAnyModule; }
    bool isPastWorkLimit(FunctionID) const final { return false; }
    std::span<const FlowCallLink> callLinks() const LIFETIME_BOUND final { return m_callLinks.span(); }
    std::span<const FlowBuiltinCallLink> builtinCallLinks() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowUnknownCallee> unknownCallees() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowStaticFieldAccess> staticFieldAccesses() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowMemberAccess> memberAccesses() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowMemberOperation> uncertainMemberStores() const LIFETIME_BOUND final { return { }; }
    bool isPrototype(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    bool hasEscaped(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    // No access through a class object is listed, so every static field counts as one of an escaped class.
    bool hasClassObjectEscaped(FunctionID) const final { return true; }
    bool mayChangeIntegrity(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }

private:
    FlowInstanceID hostInstance() const { return FlowInstanceID(m_numberOfFunctions); }
    FunctionID host() const { return FunctionID(m_numberOfFunctions); }
    void addLink(FunctionID caller, ValueID site, FunctionID callee, FlowCallLinkKind);
    void addLinkFromHost(FunctionID callee);
    void collectOperationsOf(FunctionID, const FlowBodyUpdate&);
    void collectEntriesWithoutStamp();
    void collectModules();

    const ValueFlowResultInputs& m_inputs;
    uint32_t m_numberOfFunctions;
    // Reached by the solve, or unsolved code.
    BitVector m_isCompiled;
    FlowObjectTable m_noObjects;
    Vector<Vector<FlowModuleOperation>> m_moduleOperations;
    Vector<FlowCallLink> m_callLinks;
    Vector<FlowModuleLoadRecord> m_moduleLoads;
    BitVector m_evaluatedModules;
    bool m_mayLoadAnyModule;
    bool m_libraryLoadsItsModulesByName { false };
};

} // anonymous namespace

void OrderInputs::addLink(FunctionID caller, ValueID site, FunctionID callee, FlowCallLinkKind kind)
{
    m_callLinks.append(FlowCallLink { .callerInstance = FlowInstanceID(caller.index()), .site = site, .calleeInstance = FlowInstanceID(callee.index()), .caller = caller, .callee = callee, .kind = kind });
}

void OrderInputs::addLinkFromHost(FunctionID callee)
{
    addLink(host(), { }, callee, FlowCallLinkKind::Host);
}

static std::optional<FlowModuleOperationKind> moduleOperationKindOf(const Value& value)
{
    switch (value.opcode) {
    case Opcode::ModuleNamespace:
        return FlowModuleOperationKind::Namespace;
    case Opcode::ImportModule:
        return FlowModuleOperationKind::Import;
    case Opcode::RequireModule:
        return FlowModuleOperationKind::Require;
    case Opcode::ImportDynamic:
        return FlowModuleOperationKind::ImportDynamic;
    case Opcode::RequireDynamic:
        return FlowModuleOperationKind::RequireDynamic;
    case Opcode::RequireBuiltinModule:
        return FlowModuleOperationKind::RequireBuiltinModule;
    default:
        return std::nullopt;
    }
}

void OrderInputs::collectOperationsOf(FunctionID id, const FlowBodyUpdate& update)
{
    const Program& program = m_inputs.program;
    const Function& function = program.at(id);
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID site(index);
        const Value& value = function.values[index];
        if (value.opcode == Opcode::CallDirect) {
            addLink(id, site, value.function(), FlowCallLinkKind::Call);
            continue;
        }
        if (value.opcode == Opcode::Builtin) {
            // The operation names the object of a module of the library, which loads the module.
            if (RuntimeModuleID runtimeModule = findCompiledModuleWithObject(program, program.builtins().canonicalID(value.builtin())))
                m_moduleOperations[id.index()].append(FlowModuleOperation { .kind = FlowModuleOperationKind::RequireBuiltinModule, .origin = site, .runtimeModule = runtimeModule });
            continue;
        }
        std::optional<FlowModuleOperationKind> kind = moduleOperationKindOf(value);
        if (!kind)
            continue;
        // The program's own code that names a module at run time has raised the flag. A function of the library names modules of the library.
        if (value.opcode == Opcode::ImportDynamic || value.opcode == Opcode::RequireDynamic)
            m_libraryLoadsItsModulesByName = true;
        FlowModuleOperation operation { .kind = *kind, .origin = site };
        if (PayloadWord::module(value.opcode))
            operation.module = value.module();
        if (value.opcode == Opcode::RequireBuiltinModule)
            operation.runtimeModule = value.runtimeModule();
        m_moduleOperations[id.index()].append(operation);
    }
    for (const FlowCallStamp& stamp : update.calls) {
        FlowCallLinkKind kind = OperandPosition::newTarget(function.at(stamp.value).opcode) ? FlowCallLinkKind::Construct : FlowCallLinkKind::Call;
        for (FunctionID callee : program.at(stamp.callees).functions)
            addLink(id, stamp.value, callee, kind);
    }
}

// Where and when such an entry happens nobody says, so the host makes it.
void OrderInputs::collectEntriesWithoutStamp()
{
    const Program& program = m_inputs.program;
    BitVector isCalledByHost = functionsCalledByHost(program, m_inputs.loops());
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        FunctionID function(index);
        if (!m_isCompiled.quickGet(index))
            continue;
        if (!m_inputs.isReached(function) || isCalledByHost.get(index) || m_inputs.hasEscaped(m_inputs.universe().closureOf(function)))
            addLinkFromHost(function);
    }
}

void OrderInputs::collectModules()
{
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.body && m_inputs.isReached(module.body))
            m_evaluatedModules.quickSet(index);
    }

    // Native code loads a module of the library whenever it likes once a built-in is used that does so.
    Vector<ModuleID> moduleOfRuntimeModule(builtins.moduleCount());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        if (RuntimeModuleID runtimeModule = program.modules[index].runtimeModule)
            moduleOfRuntimeModule[runtimeModule.index()] = ModuleID(index);
    }
    BitVector isLoadedByNativeCode(program.modules.size());
    if (m_libraryLoadsItsModulesByName) {
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            if (program.modules[index].kind == ModuleKind::RuntimeSource)
                isLoadedByNativeCode.quickSet(index);
        }
    }
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (m_inputs.solver().usesOf(BuiltinID(index)).isEmpty())
            continue;
        for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(BuiltinID(index))) {
            if (ModuleID module = moduleOfRuntimeModule[load.module.index()])
                isLoadedByNativeCode.quickSet(module.index());
        }
    }
    for (size_t module : isLoadedByNativeCode)
        m_moduleLoads.append(FlowModuleLoadRecord { hostInstance(), { }, ModuleID(static_cast<uint32_t>(module)) });
}

OrderInputs::OrderInputs(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, const FlowResultTables& tables)
    : FlowOrderInputs(inputs.program, inputs.build)
    , m_inputs(inputs)
    , m_numberOfFunctions(inputs.program.functions.size())
    , m_isCompiled(inputs.program.functions.size())
    , m_noObjects(inputs.program)
    , m_moduleOperations(inputs.program.functions.size())
    , m_evaluatedModules(inputs.program.modules.size())
    , m_mayLoadAnyModule(unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution))
{
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index)))
            continue;
        m_isCompiled.quickSet(index);
        collectOperationsOf(FunctionID(index), tables.application->bodies[index]);
    }
    collectEntriesWithoutStamp();
    for (size_t function : calls.functionsEnteredWithoutStamp) {
        if (function < m_numberOfFunctions && inputs.isReached(FunctionID(static_cast<uint32_t>(function))))
            addLinkFromHost(FunctionID(static_cast<uint32_t>(function)));
    }
    collectModules();
}

void computeOrderResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    // A call that may enter anything enters it at a time nobody knows.
    if (calls.someCallEntersAnything)
        return;

    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    OrderInputs orderInputs(inputs, calls, tables);
    FlowModuleOrder moduleOrder = computeModuleOrder(orderInputs);
    FlowScopeAccessesByFunction accessesByFunction(program.functions.size());
    FlowScopeOrders orders = computeScopeOrders(orderInputs, moduleOrder, accessesByFunction);

    // The order counts the function that a variable starts with as a store, and the flow of a location counts operations only.
    // It follows an environment activation by activation, so it knows of stores that run once for each environment.
    auto apply = [&](LocationFlow& flow, FlowLocationOrder order, uint32_t switchSubject, bool startsAsFunction) {
        if (flow.writers != WriterState::AllKnown)
            return;
        if (inputs.build.isEnabled(FlowResultKind::Initialization, switchSubject))
            flow.initialization = order.initialization;
        if (flow.stores == StoreCount::Unknown && order.stores == StoreCount::AtMostOnce && !startsAsFunction)
            flow.stores = StoreCount::AtMostOnce;
    };
    for (uint32_t index = 0; index < program.variables.size(); ++index)
        apply(application.variables[index].flow, orders.orderOf(VariableID(index)), index, program.variables[index].initialization == VariableInitialization::Function);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!program.layouts[index].isEnvironment())
            continue;
        Vector<FlowFieldUpdate>& fields = application.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field)
            apply(fields[field].flow.location, orders.orderOf(LayoutID(index), field), index, false);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
