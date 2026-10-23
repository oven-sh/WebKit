/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowOrder.h"
#include "AOTThreads.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <tuple>
#include <wtf/Atomics.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The order of execution is computed from who calls whom and which module loads which.
class OrderInputs final : public FlowOrderInputs {
public:
    OrderInputs(const ValueFlowResultInputs&, const FlowResultTables&);

    unsigned numberOfInstances() const final { return m_numberOfFunctions + 1; }
    bool isHost(FlowInstanceID instance) const final { return instance.index() == m_numberOfFunctions; }
    bool isHost(FunctionID function) const final { return function.index() == m_numberOfFunctions; }
    FunctionID functionOf(FlowInstanceID instance) const final { return FunctionID(instance.index()); }
    const FlowCallSiteKey& callSiteOf(FlowInstanceID) const LIFETIME_BOUND final { RELEASE_ASSERT_NOT_REACHED(); }
    bool isReached(FunctionID function) const final { return m_isCompiled.quickGet(function.index()); }
    std::span<const FlowModuleOperation> moduleOperationsOf(FunctionID function) const LIFETIME_BOUND final { return m_moduleOperations[function.index()].span(); }

    const BitVector& evaluatedModules() const LIFETIME_BOUND final { return m_evaluatedModules; }
    std::span<const FlowModuleLoadRecord> moduleLoads() const LIFETIME_BOUND final { return m_moduleLoads.span(); }
    bool mayBeLoadedByName(ModuleID module) const final
    {
        bool isEntryPoint = std::ranges::contains(program().entries, module, &Entry::module);
        return (isEntryPoint ? m_filesOfProgramAreNamedAtRunTime : m_modulesAreNamedAtRunTime) && mayAnswerToName(program(), module);
    }
    bool isPastWorkLimit(FunctionID) const final { return false; }
    std::span<const FlowCallLink> callLinks() const LIFETIME_BOUND final { return m_callLinks.span(); }
    std::span<const FlowBuiltinCallLink> builtinCallLinks() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowUnknownCallee> unknownCallees() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowStaticFieldAccess> staticFieldAccesses() const LIFETIME_BOUND final { return m_staticFieldAccesses.span(); }
    // Asked about the constructor of a class with static fields.
    bool hasClassObjectEscaped(FunctionID constructor) const final
    {
        LayoutID constructedClass = m_inputs.program.at(constructor).constructedClass;
        return !constructedClass || std::ranges::any_of(m_inputs.program.at(constructedClass).staticFields, [&](const StaticField& field) {
            uint32_t variable = field.variable.index();
            return m_inputs.variablesOnObjects->staticFieldsInUnknownHands.quickGet(variable) || m_inputs.variablesOnObjects->staticFieldsOfEscapedClassObjects.quickGet(variable)
                || m_staticFieldsLookedAtByNobodyListed.get(variable);
        });
    }

    std::span<const FlowMemberAccess> memberAccesses() const LIFETIME_BOUND final { return { }; }
    std::span<const FlowMemberOperation> uncertainMemberStores() const LIFETIME_BOUND final { return { }; }
    FlowObjectKind kindOf(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    FlowObjectID findPrototype(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    std::optional<FlowObjectCreation> creationOf(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    bool isPrototype(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    bool hasEscaped(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }
    bool mayChangeIntegrity(FlowObjectID) const final { RELEASE_ASSERT_NOT_REACHED(); }

private:
    FlowInstanceID hostInstance() const { return FlowInstanceID(m_numberOfFunctions); }
    FunctionID host() const { return FunctionID(m_numberOfFunctions); }
    FlowCallLink linkFromHost(FunctionID callee) const;
    void collectOperationsOf(FunctionID, Vector<FlowCallLink>&);
    void collectOperations();
    void collectCallsOfClosures();
    void collectEntriesThatNobodyLists(const FlowResultTables&);
    void collectModules();

    const ValueFlowResultInputs& m_inputs;
    uint32_t m_numberOfFunctions;
    // Reached by the solve, or unsolved code.
    BitVector m_isCompiled;
    Vector<Vector<FlowModuleOperation>> m_moduleOperations;
    Vector<FlowCallLink> m_callLinks;
    // By function. The threads of a loop set them.
    Vector<uint8_t> m_hasEntryThatNobodyLists;
    Vector<FlowModuleLoadRecord> m_moduleLoads;
    BitVector m_evaluatedModules;
    bool m_modulesAreNamedAtRunTime;
    bool m_filesOfProgramAreNamedAtRunTime;
    bool m_someCallEntersAnything { false };
    Vector<FlowStaticFieldAccess> m_staticFieldAccesses;
    // By variable.
    BitVector m_staticFieldsLookedAtByNobodyListed;
};

} // anonymous namespace

static FlowCallLink linkBetween(FunctionID caller, ValueID site, FunctionID callee, FlowCallLinkKind kind)
{
    return { .callerInstance = FlowInstanceID(caller.index()), .site = site, .calleeInstance = FlowInstanceID(callee.index()), .caller = caller, .callee = callee, .kind = kind };
}

FlowCallLink OrderInputs::linkFromHost(FunctionID callee) const
{
    return linkBetween(host(), { }, callee, FlowCallLinkKind::Host);
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
    case Opcode::EvaluateModule:
        return FlowModuleOperationKind::Evaluate;
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

void OrderInputs::collectOperationsOf(FunctionID id, Vector<FlowCallLink>& links)
{
    const Program& program = m_inputs.program;
    const Function& function = program.at(id);
    Vector<FlowModuleOperation>& moduleOperations = m_moduleOperations[id.index()];
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID site(index);
        const Value& value = function.values[index];
        if (value.opcode == Opcode::CallDirect) {
            links.append(linkBetween(id, site, value.function(), FlowCallLinkKind::Call));
            continue;
        }
        if (value.opcode == Opcode::Builtin) {
            // The operation names the object of a module of the library, which loads the module.
            if (RuntimeModuleID runtimeModule = findCompiledModuleWithObject(program, program.builtins().canonicalID(value.builtin())))
                moduleOperations.append(FlowModuleOperation { .kind = FlowModuleOperationKind::RequireBuiltinModule, .origin = site, .runtimeModule = runtimeModule });
            continue;
        }
        std::optional<FlowModuleOperationKind> kind = moduleOperationKindOf(value);
        if (!kind)
            continue;
        FlowModuleOperation operation { .kind = *kind, .origin = site };
        if (PayloadWord::module(value.opcode))
            operation.module = value.module();
        if (value.opcode == Opcode::RequireBuiltinModule)
            operation.runtimeModule = value.runtimeModule();
        moduleOperations.append(operation);
    }
}

void OrderInputs::collectOperations()
{
    m_callLinks = collectInParallel<FlowCallLink>(m_inputs.loops(), m_numberOfFunctions, [&](unsigned index, Vector<FlowCallLink>& links) {
        if (m_isCompiled.quickGet(index))
            collectOperationsOf(FunctionID(index), links);
    });
}

static FlowCallLinkKind linkKindOf(ValueFlowCallKind kind)
{
    switch (kind) {
    case ValueFlowCallKind::Call:
        return FlowCallLinkKind::Call;
    case ValueFlowCallKind::Construct:
        return FlowCallLinkKind::Construct;
    case ValueFlowCallKind::Get:
    case ValueFlowCallKind::Set:
        return FlowCallLinkKind::Accessor;
    case ValueFlowCallKind::CallByNativeCode:
    case ValueFlowCallKind::ConstructByNativeCode:
    case ValueFlowCallKind::CallByHost:
        return FlowCallLinkKind::BuiltinCallback;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Every rule that enters a closure asks for the group of a set that holds it: a call for its callees, an access for the accessors that it finds, and any operation for what its protocols or the built-ins that it calls enter.
void OrderInputs::collectCallsOfClosures()
{
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueFlowUniverse& universe = m_inputs.universe();
    std::atomic<bool> someCallEntersAnything { false };
    m_callLinks.appendVector(collectInParallel<FlowCallLink>(m_inputs.loops(), solver.numberOfStatements(), [&](unsigned index, Vector<FlowCallLink>& links) {
        ValueFlowStatementID id(index);
        m_inputs.prefetchMadeNodesAskedForBy(index + 16, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false);
        const ValueFlowStatement& statement = solver.statementAt(id);
        bool hasRun = solver.isMadeByRule(id) ? !solver.isAtRest(id) : !statement.function || m_inputs.isReached(statement.function);
        if (!hasRun)
            return;

        bool isOperationOfFunction = statement.kind == ValueFlowStatementKind::Operation && statement.function && statement.site;
        m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            // The statement of a group asks for the nodes of that group.
            if (made == id)
                return;
            if (ValueSets::isComplement(key.set)) {
                someCallEntersAnything.store(true, std::memory_order_relaxed);
                return;
            }
            m_inputs.sets().forEachMember(key.set, [&](ValueFlowObjectID callee) {
                FunctionID function = universe.closureFunction(callee);
                if (!function)
                    return;
                if (isOperationOfFunction)
                    links.append(linkBetween(statement.function, statement.site, function, linkKindOf(key.callKind())));
                else
                    AtomicRef(m_hasEntryThatNobodyLists[function.index()]).store(1, std::memory_order_relaxed);
            });
        });
    }));
    m_someCallEntersAnything = someCallEntersAnything.load(std::memory_order_relaxed);
}

// An entry by a statement that a rule has made, any entry of unsolved code or of a function with callers that nobody knows, and the entry of a closure that a placeholder among the callees of a call stands for.
void OrderInputs::collectEntriesThatNobodyLists(const FlowResultTables& tables)
{
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_isCompiled.quickGet(index))
            continue;
        FunctionID function(index);
        ValueFlowObjectID closure = m_inputs.universe().closureOf(function);
        bool isEveryEntryListed = !m_someCallEntersAnything && !m_hasEntryThatNobodyLists[index] && m_inputs.isReached(function)
            && tables.functions.functions[index].callers == CallerState::AllKnown && !m_inputs.hasEscaped(closure) && !m_inputs.isLost(closure);
        if (!isEveryEntryListed)
            m_callLinks.append(linkFromHost(function));
    }
}

static bool namesModuleAtRunTime(const FlowModuleOperation& operation)
{
    return operation.kind == FlowModuleOperationKind::ImportDynamic || operation.kind == FlowModuleOperationKind::RequireDynamic;
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
    BitVector isLoadedByNativeCode(program.modules.size());
    // The program's own code that names a module at run time has raised the flag. A function of the library names modules of the library.
    bool libraryLoadsItsModulesByName = std::ranges::any_of(m_moduleOperations, [](const Vector<FlowModuleOperation>& operations) {
        return std::ranges::any_of(operations, namesModuleAtRunTime);
    });
    if (libraryLoadsItsModulesByName) {
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            if (program.modules[index].kind == ModuleKind::RuntimeSource)
                isLoadedByNativeCode.quickSet(index);
        }
    }
    Vector<ModuleID> moduleOfRuntimeModule = indexRuntimeModules(program);
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

OrderInputs::OrderInputs(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
    : FlowOrderInputs(inputs.program, inputs.build)
    , m_inputs(inputs)
    , m_numberOfFunctions(inputs.program.functions.size())
    , m_isCompiled(inputs.program.functions.size())
    , m_moduleOperations(inputs.program.functions.size())
    , m_hasEntryThatNobodyLists(FillWith { }, inputs.program.functions.size(), static_cast<uint8_t>(0))
    , m_evaluatedModules(inputs.program.modules.size())
    , m_modulesAreNamedAtRunTime(unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution))
    , m_filesOfProgramAreNamedAtRunTime(filesOfProgramAreLoadedByName(inputs.solution))
{
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (inputs.isReached(FunctionID(index)) || isUnsolvedCode(inputs, tables, FunctionID(index)))
            m_isCompiled.quickSet(index);
    }
    collectOperations();
    collectCallsOfClosures();
    collectEntriesThatNobodyLists(tables);

    auto keyOf = [](const FlowCallLink& link) {
        return std::tuple(link.caller, link.site, link.callee, link.kind);
    };
    sortAndRemoveDuplicates(inputs.loops(), m_callLinks, [&](const FlowCallLink& a, const FlowCallLink& b) {
        return keyOf(a) < keyOf(b);
    }, [&](const FlowCallLink& a, const FlowCallLink& b) {
        return keyOf(a) == keyOf(b);
    });
    collectModules();

    // Whoever looks at a static field holds the class object or an object below it, whether an operation spells the look out or not.
    for (const ValueFlowAccessThroughObject& access : inputs.variablesOnObjects->accesses) {
        if (!inputs.program.at(access.variable).owner)
            continue;
        bool isStore = access.kind == ValueFlowAccessThroughObjectKind::Store;
        if (access.function || isStore)
            m_staticFieldAccesses.append({ access.variable, access.function, access.site, isStore });
        else
            m_staticFieldsLookedAtByNobodyListed.set(access.variable.index());
    }
}

void computeOrderResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls&, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    OrderInputs orderInputs(inputs, tables);
    FlowModuleOrder moduleOrder = computeModuleOrder(orderInputs);
    FlowScopeAccessesByFunction accessesByFunction(program.functions.size());
    FlowScopeOrders orders = computeScopeOrders(orderInputs, moduleOrder, accessesByFunction);

    // The order counts the function that a variable starts with as a store, and the flow of a location counts operations only.
    auto apply = [&](LocationFlow& flow, FlowLocationOrder order, uint32_t switchSubject, bool startsAsFunction, bool isStaticField) {
        bool areWritersKnown = flow.writers == WriterState::AllKnown;
        if (!areWritersKnown && !isStaticField)
            return;
        if (inputs.build.isEnabled(FlowResultKind::Initialization, switchSubject))
            flow.initialization = order.initialization;
        if (areWritersKnown && flow.stores == StoreCount::Unknown && order.stores == StoreCount::AtMostOnce && !startsAsFunction)
            flow.stores = StoreCount::AtMostOnce;
    };
    for (uint32_t index = 0; index < program.variables.size(); ++index)
        apply(application.variables[index].flow, orders.orderOf(VariableID(index)), index, program.variables[index].initialization == VariableInitialization::Function, !!program.variables[index].owner);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!program.layouts[index].isEnvironment())
            continue;
        Vector<FlowFieldUpdate>& fields = application.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            FlowLocationOrder order = orders.orderOf(LayoutID(index), field);
            // Without the switch the field stays set at creation, which is what the allocation says.
            if (order.undefinedAtStartIsNeverRead && (fields[field].flow.location.writers != WriterState::AllKnown || !inputs.build.isEnabled(FlowResultKind::Initialization, index)))
                order = { InitializationState::SetAtCreation, StoreCount::Unknown };
            fields[field].undefinedAtStartIsNeverRead = order.undefinedAtStartIsNeverRead;
            apply(fields[field].flow.location, order, index, false, false);
        }
    }

    for (const FlowReadOfLocation& read : orders.readsAfterInitialization) {
        const Value& value = program.at(read.function).at(read.value);
        bool isVariable = value.opcode == Opcode::GetModuleVariable;
        const LocationFlow& flow = isVariable ? application.variables[value.variable().index()].flow : application.layouts[value.layout().index()].fields[value.field()].flow.location;
        uint32_t switchSubject = isVariable ? value.variable().index() : value.layout().index();
        if (flow.writers == WriterState::AllKnown && inputs.build.isEnabled(FlowResultKind::Initialization, switchSubject))
            application.bodies[read.function.index()].readsAfterInitialization.append(read.value);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
