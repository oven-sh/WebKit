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

// Direct calls, and the modules that operations load.
static void collectOperations(const ValueFlowResultInputs& inputs, FlowOrderInputs& order, bool& libraryLoadsItsModulesByName)
{
    struct Row {
        FlowCall call;
        ModuleID target;
        bool isSynchronous;
    };
    const Program& program = inputs.program;
    Vector<ModuleID> moduleOfRuntimeModule = indexRuntimeModules(program);
    std::atomic<bool> namesModuleAtRunTime { false };
    Vector<Row> rows = collectInParallel<Row>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Row>& rows) {
        if (!order.reachedFunctions.quickGet(index) || !inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::CallDirect, ValueFlowNotedOperation::Builtin, ValueFlowNotedOperation::ModuleOperation })))
            return;
        const Function& function = program.functions[index];
        for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            const Value& value = function.values[valueIndex];
            auto load = [&](ModuleID target, bool isSynchronous) {
                if (target)
                    rows.append({ { FunctionID(index), ValueID(valueIndex), { } }, target, isSynchronous });
            };
            auto loadRuntimeModule = [&](RuntimeModuleID module) {
                if (module && module.index() < moduleOfRuntimeModule.size())
                    load(moduleOfRuntimeModule[module.index()], true);
            };
            switch (value.opcode) {
            case Opcode::CallDirect:
                rows.append({ { FunctionID(index), ValueID(valueIndex), value.function() }, { }, false });
                break;
            case Opcode::Builtin:
                // The operation names the object of a module of the library, which loads the module.
                loadRuntimeModule(findCompiledModuleWithObject(program, program.builtins().canonicalID(value.builtin())));
                break;
            case Opcode::RequireBuiltinModule:
                loadRuntimeModule(value.runtimeModule());
                break;
            case Opcode::RequireModule:
            case Opcode::EvaluateModule:
                load(value.module(), true);
                break;
            case Opcode::ImportModule:
                load(value.module(), false);
                break;
            case Opcode::ImportDynamic:
            case Opcode::RequireDynamic:
                namesModuleAtRunTime.store(true, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }
    });
    for (const Row& row : rows) {
        if (row.target)
            order.moduleLoads.append({ row.call.caller, row.target, row.isSynchronous });
        else
            order.calls.append(row.call);
    }
    libraryLoadsItsModulesByName = namesModuleAtRunTime.load(std::memory_order_relaxed);
}

// Every rule that enters a closure asks for the group of a set that holds it: a call for its callees, an access for the accessors that it finds, and any operation for what its protocols or the built-ins that it calls enter.
// An entry by a statement that a rule has made, any entry of unsolved code or of a function with callers that nobody knows, and the entry of a closure that a placeholder among the callees of a call stands for, is the host's.
static void collectCallsOfClosures(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FlowOrderInputs& order)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    uint32_t numberOfFunctions = inputs.program.functions.size();
    std::atomic<bool> someCallEntersAnything { false };
    Vector<uint8_t> hasEntryThatNobodyLists(FillWith { }, numberOfFunctions, static_cast<uint8_t>(0));
    order.calls.appendVector(collectInParallel<FlowCall>(inputs.loops(), solver.numberOfStatements(), [&](unsigned index, Vector<FlowCall>& calls) {
        ValueFlowStatementID id(index);
        inputs.prefetchMadeNodesAskedForBy(index + 16, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false);
        const ValueFlowStatement& statement = solver.statementAt(id);
        bool hasRun = solver.isMadeByRule(id) ? !solver.isAtRest(id) : !statement.function || inputs.isReached(statement.function);
        if (!hasRun)
            return;

        bool isOperationOfFunction = statement.kind == ValueFlowStatementKind::Operation && statement.function && statement.site;
        inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            // The statement of a group asks for the nodes of that group.
            if (made == id)
                return;
            if (ValueSets::isComplement(key.set)) {
                someCallEntersAnything.store(true, std::memory_order_relaxed);
                return;
            }
            inputs.sets().forEachMember(key.set, [&](ValueFlowObjectID callee) {
                FunctionID function = universe.closureFunction(callee);
                if (!function)
                    return;
                if (isOperationOfFunction)
                    calls.append({ statement.function, statement.site, function });
                else
                    AtomicRef(hasEntryThatNobodyLists[function.index()]).store(1, std::memory_order_relaxed);
            });
        });
    }));
    for (size_t index : order.reachedFunctions) {
        FunctionID function(static_cast<uint32_t>(index));
        ValueFlowObjectID closure = universe.closureOf(function);
        bool isEveryEntryListed = !someCallEntersAnything.load(std::memory_order_relaxed) && !hasEntryThatNobodyLists[index] && inputs.isReached(function)
            && tables.functions.functions[index].callers == CallerState::AllKnown && !inputs.hasEscaped(closure) && !inputs.isLost(closure);
        if (!isEveryEntryListed)
            order.calls.append({ order.host(), { }, function });
    }
    sortAndRemoveDuplicates(inputs.loops(), order.calls);
}

static void collectModules(const ValueFlowResultInputs& inputs, FlowOrderInputs& order, bool libraryLoadsItsModulesByName)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    bool modulesAreNamedAtRunTime = unseenCodeCanRun(inputs.solution) || modulesAreLoadedByName(inputs.solution);
    bool filesOfProgramAreNamedAtRunTime = filesOfProgramAreLoadedByName(inputs.solution);
    // Native code loads a module of the library whenever it likes once a built-in is used that does so.
    // The program's own code that names a module at run time has raised the flag. A function of the library names modules of the library.
    BitVector isLoadedByNativeCode(program.modules.size());
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.body && inputs.isReached(module.body))
            order.evaluatedModules.quickSet(index);
        bool isEntryPoint = std::ranges::contains(program.entries, ModuleID(index), &Entry::module);
        if ((isEntryPoint ? filesOfProgramAreNamedAtRunTime : modulesAreNamedAtRunTime) && mayAnswerToName(program, ModuleID(index)))
            order.modulesLoadedByName.quickSet(index);
        if (libraryLoadsItsModulesByName && module.kind == ModuleKind::RuntimeSource)
            isLoadedByNativeCode.quickSet(index);
    }
    Vector<ModuleID> moduleOfRuntimeModule = indexRuntimeModules(program);
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (inputs.solver().usesOf(BuiltinID(index)).isEmpty())
            continue;
        for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(BuiltinID(index))) {
            if (ModuleID module = moduleOfRuntimeModule[load.module.index()])
                isLoadedByNativeCode.quickSet(module.index());
        }
    }
    for (size_t module : isLoadedByNativeCode)
        order.modulesLoadedByHost.append(ModuleID(static_cast<uint32_t>(module)));
}

// Whoever looks at a static field holds the class object or an object below it, whether an operation spells the look out or not.
static void collectStaticFields(const ValueFlowResultInputs& inputs, FlowOrderInputs& order)
{
    const Program& program = inputs.program;
    const ValueFlowVariablesOnObjects& variables = *inputs.variablesOnObjects;
    BitVector isLookedAtByNobodyListed;
    for (const ValueFlowAccessThroughObject& access : variables.accesses) {
        if (!program.at(access.variable).owner)
            continue;
        bool isStore = access.kind == ValueFlowAccessThroughObjectKind::Store;
        if (access.function || isStore)
            order.staticFieldAccesses.append({ access.variable, access.function, access.site, isStore });
        else
            isLookedAtByNobodyListed.set(access.variable.index());
    }
    for (const Layout& layout : program.layouts) {
        if (layout.staticFields.isEmpty() || !layout.constructor)
            continue;
        LayoutID constructedClass = program.at(layout.constructor).constructedClass;
        bool hasEscaped = !constructedClass || std::ranges::any_of(program.at(constructedClass).staticFields, [&](const StaticField& field) {
            uint32_t variable = field.variable.index();
            return variables.staticFieldsInUnknownHands.quickGet(variable) || variables.staticFieldsOfEscapedClassObjects.quickGet(variable) || isLookedAtByNobodyListed.get(variable);
        });
        if (!hasEscaped)
            continue;
        for (const StaticField& field : layout.staticFields)
            order.staticFieldsOfEscapedClasses.set(field.variable.index());
    }
}

static FlowOrderInputs collectOrderInputs(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowOrderInputs order { program, inputs.build };
    order.reachedFunctions = BitVector(program.functions.size());
    order.evaluatedModules = BitVector(program.modules.size());
    order.modulesLoadedByName = BitVector(program.modules.size());
    order.staticFieldsOfEscapedClasses = BitVector(program.variables.size());
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (inputs.isReached(FunctionID(index)) || isUnsolvedCode(inputs, tables, FunctionID(index)))
            order.reachedFunctions.quickSet(index);
    }
    bool libraryLoadsItsModulesByName = false;
    collectOperations(inputs, order, libraryLoadsItsModulesByName);
    collectCallsOfClosures(inputs, tables, order);
    collectModules(inputs, order, libraryLoadsItsModulesByName);
    collectStaticFields(inputs, order);
    return order;
}

void computeOrderResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls&, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    FlowOrderInputs orderInputs = collectOrderInputs(inputs, tables);
    FlowModuleOrder moduleOrder = computeModuleOrder(orderInputs);
    FlowScopeOrders orders = computeScopeOrders(orderInputs, moduleOrder);

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
