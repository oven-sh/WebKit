/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowUnseenCode.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowParts.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPropertyState.h"
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// A namespace object is read-only. Object 0 is never queued for a sink: JavaScript writes to the escaped objects it stands for, and each of those is opened itself.
static bool isOpenedByJavaScript(FlowObjectKind kind)
{
    switch (kind) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::ModuleNamespace:
        return false;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasElementParts(const FlowSolver& solver, FlowObjectID object)
{
    const FlowObject& record = solver.objects().at(object);
    return record.numberOfPositions || indexedElementSlotOf(solver.builtins(), record.builtinClass);
}

// The static fields of a class are variables.
static Vector<FlowNodeID, 8> collectLocationsOf(const FlowSolver& solver, FlowObjectID object)
{
    const FlowGraph& graph = solver.graph();
    const FlowObject& record = solver.objects().at(object);
    bool hasCaptures = solver.objects().hasCaptures(object);

    Vector<FlowNodeID, 8> locations;
    graph.forEachNodeOf(object, [&](FlowNodeID node) {
        if (!hasCaptures || graph.kindOf(node) != FlowNodeKind::DeclaredPart)
            locations.append(node);
    });
    if (record.kind != FlowObjectKind::Closure)
        return locations;

    const Program& program = solver.program();
    if (LayoutID constructedClass = program.at(record.function).constructedClass) {
        for (const StaticField& field : program.at(constructedClass).staticFields)
            locations.append(graph.variable(field.variable));
    }
    return locations;
}

static void exposeMembersOf(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    FlowObject record = solver.objects().at(object);
    const BuiltinTable& builtins = solver.builtins();
    const Builtin& holder = builtins.at(record.builtin);
    // The run time refuses a built-in whose application is an event unless the build has seen such an event, so such a member stays behind until unknown JavaScript can run.
    bool refusesEvents = !solver.records().mayRunUnknownJavaScript();
    bool hasRefusedMembers = false;

    for (BuiltinID member : builtins.membersOf(record.builtin)) {
        if (refusesEvents && letsInUnseenCode(builtins, builtins.canonicalID(member))) {
            hasRefusedMembers = true;
            continue;
        }
        if (isConstructorLink(holder, builtins.at(member))) {
            // Native code does not follow the link. Seen code does, and what it reads there under a key nobody can name is a member of the constructor.
            exposeMembersOf(solver, ensureBuiltinObject(solver, member), witness);
            continue;
        }
        if (!isObjectKind(builtins.at(member).kind)) {
            noteBuiltinUse(solver, member, BuiltinUse::Escaped, witness);
            // What follows the holder is the value that a read of the member answers. A lookup through the unknown object leaves the member to native code from here on.
            if (builtins.dispatchOf(member, BuiltinCallMode::Get))
                callBuiltinAccessorFromNativeCode(solver, member, BuiltinCallMode::Get);
            continue;
        }
        FlowObjectID memberObject = record.kind == FlowObjectKind::ModuleBuiltin
            ? ensureModuleBuiltinObject(solver, member, record.module)
            : ensureBuiltinObject(solver, member);
        solver.joinObject(solver.graph().nativeCode(), memberObject, witness);
    }
    if (hasRefusedMembers)
        solver.callState().unseenCode.holdersOfRefusedMembers.append(object);
}

static void revisitLookupsThroughUnknownObject(FlowSolver& solver, BuiltinID holder)
{
    const BuiltinTable& builtins = solver.builtins();
    for (BuiltinID member : builtins.membersOf(holder)) {
        solver.revisitPropertyWaiters(unknownFlowObject, solver.program().keyOf(builtins.at(member).name));
        if (isConstructorLink(builtins.at(holder), builtins.at(member)))
            revisitLookupsThroughUnknownObject(solver, builtins.canonicalID(member));
    }
}

static FlowCallRequest requestOfNativeCode(FlowSolver& solver)
{
    FlowNodeID unknownValue = unknownValueNode(solver);
    return {
        nativeCodeSite(solver),
        ValueID(),
        FlowCallLinkKind::Host,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .thisValue = unknownValue, .argumentList = unknownValue, .result = solver.graph().nativeCode() },
    };
}

// Native code reads nothing out of a bound function. It calls it, and an instanceof test asks the target.
static void exposeBoundFunction(FlowSolver& solver, FlowObjectID boundFunction)
{
    callObjects(solver, requestOfNativeCode(solver), FlowObjectSet(boundFunction));

    FlowStatementSite site = nativeCodeSite(solver);
    FlowNodeID boundFunctions = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::EscapedBoundFunctions));
    solver.joinObject(boundFunctions, boundFunction);
    runProtocolsInOwnStatement(solver, { site, ValueID(), boundFunctions, Protocol::HasInstance, { }, FlowNodeID(), FlowNodeID() });
}

// A namespace object also serves the names the run time captures from the value of its module and of the targets of its dynamic star exports.
void exposeCapturedNameSources(FlowSolver& solver, ModuleID root, const Witness& witness)
{
    const Program& program = solver.program();
    BitVector visited;
    Vector<ModuleID, 8> pending { root };

    while (!pending.isEmpty()) {
        ModuleID id = pending.takeLast();
        if (visited.set(id.index()))
            continue;

        const Module& module = program.at(id);
        if (module.exportRule != SyntheticExportRule::None)
            connectRequiredValueOf(solver, id, solver.graph().nativeCode(), witness);
        for (const ModuleRequest& request : module.requests) {
            if (request.isDynamicStarExport)
                pending.append(request.target);
        }
    }
}

// Unseen JavaScript is refused a write to a member that start-up has made immutable, and start-up does so with each member that seen code takes for a constant.
// It is refused a change of what the loader answers too, in every image in which no seen code makes one.
static void noteWritesToMembers(FlowSolver& solver, BuiltinID holder, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    const BitVector& membersLockedWhileConstant = solver.propertyState().membersLockedWhileConstant;
    bool isWholeProgramOpen = solver.records().mayRunUnknownJavaScript();
    for (BuiltinID member : builtins.membersOf(holder)) {
        if (!isWholeProgramOpen && isLoaderLocation(member))
            continue;
        if (builtins.at(member).mutability != BuiltinMutability::AlwaysLocked && !membersLockedWhileConstant.get(member.index()))
            noteBuiltinUse(solver, member, BuiltinUse::Written, witness);
    }
}

FlowStatementSite nativeCodeSite(FlowSolver& solver)
{
    const FlowUnseenCodeState& state = solver.callState().unseenCode;
    RELEASE_ASSERT(state.hostInstance);
    return { state.hostInstance, state.nativeCodeStatement };
}

FlowNodeID unknownValueNode(FlowSolver& solver)
{
    FlowNodeID node = solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::UnknownValue));
    if (solver.valueOf(node).isEmpty())
        solver.joinUnknown(node, noWitness);
    return node;
}

Witness reachWitnessOf(const FlowSolver& solver, FlowObjectID object, ObjectMutation mutation)
{
    return solver.records().witnessOf(object, mutation);
}

void didReachNativeCode(FlowSolver& solver, FlowObjectID object)
{
    RELEASE_ASSERT(object != unknownFlowObject);
    Witness witness = reachWitnessOf(solver, object, ObjectMutation::ReachNativeCode);

    didEscape(solver, object);
    for (FlowObjectUse use : { FlowObjectUse::ReadByName, FlowObjectUse::ReadByUnknownKey, FlowObjectUse::Enumerated })
        solver.noteUse(object, use);
    exposeNodesOf(solver, object);
    exposePrototypesOf(solver, object, witness);

    switch (solver.objects().kindOf(object)) {
    case FlowObjectKind::Unknown:
        break;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Opaque:
        return;
    case FlowObjectKind::Prototype:
        solver.joinObject(solver.graph().nativeCode(), solver.objects().at(object).owner, witness);
        return;
    case FlowObjectKind::Closure:
        exposeClosure(solver, object, witness);
        return;
    case FlowObjectKind::Instance:
        exposeCoroutineResult(solver, object, witness);
        exposeBuiltinIterator(solver, object, witness);
        if (solver.objects().isInstanceOf(object, BuiltinClasses::BoundFunction))
            exposeBoundFunction(solver, object);
        return;
    case FlowObjectKind::ModuleNamespace:
        exposeNamespace(solver, object, witness);
        return;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        exposeBuiltin(solver, object, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void exposeNodesOf(FlowSolver& solver, FlowObjectID object)
{
    for (FlowNodeID location : collectLocationsOf(solver, object))
        exposeLocation(solver, object, location);
}

void exposeLocation(FlowSolver& solver, FlowObjectID object, FlowNodeID node)
{
    solver.connect(node, solver.graph().nativeCode(), { }, reachWitnessOf(solver, object, ObjectMutation::ReachNativeCode));
    solver.noteLocationEvent(node, FlowLocationEvent::ReadByUnknownCode);
    if (solver.hasReachedUnknownJavaScript(object) && isOpenedByJavaScript(solver.objects().kindOf(object)))
        openLocation(solver, object, node, reachWitnessOf(solver, object, ObjectMutation::ReachUnknownJavaScript));
}

void exposePrototypesOf(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    // No waiter: exposeLocation() connects the prototype link, so a prototype that arrives later follows that edge.
    forEachPrototype(solver, object, FlowStatementSite(), [&](FlowObjectID prototype) {
        solver.joinObject(solver.graph().nativeCode(), prototype, witness);
    });
}

void exposeStartupModules(FlowSolver& solver, const Witness& witness)
{
    if (std::exchange(solver.callState().unseenCode.startupModulesAreExposed, true))
        return;
    for (const BuiltinModuleLoad& load : solver.builtins().startupLoads())
        loadRuntimeModule(solver, load, witness);
    for (RuntimeFunctionID id : solver.builtins().functionsCalledWithStartupLoads()) {
        if (const RuntimeFunctionEntry* entry = findRuntimeFunction(solver.program(), id))
            enterFromNativeCode(solver, entry->function, FlowObjectID(), witness);
    }
}

void exposeClosure(FlowSolver& solver, FlowObjectID closure, const Witness& witness)
{
    FunctionID function = solver.objects().at(closure).function;
    enterFromNativeCode(solver, function, closure, witness);

    solver.noteUse(closure, FlowObjectUse::Called);
    solver.noteUse(closure, FlowObjectUse::CalledRepeatedly);
    if (solver.program().at(function).constructorForm() != ConstructorForm::NotConstructor)
        solver.noteUse(closure, FlowObjectUse::Constructed);

    if (FlowObjectID prototype = ensurePrototypeObject(solver, closure))
        solver.joinObject(solver.graph().nativeCode(), prototype, witness);
}

void exposeBuiltin(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    BuiltinID builtin = solver.objects().at(object).builtin;
    const BuiltinTable& builtins = solver.builtins();
    FlowUnseenCodeState& state = solver.callState().unseenCode;

    noteBuiltinUse(solver, builtin, BuiltinUse::Escaped, witness);

    // Native code looks up no member of a built-in object that its entry does not declare, and a lookup by name through the unknown object finds the member in the table.
    bool membersAreFoundByName = builtins.at(builtin).scope == BuiltinScope::Realm;
    // A store through the unknown object finds a setter whether or not it has escaped, so such a lookup waits for the holder in both cases.
    if (membersAreFoundByName)
        revisitLookupsThroughUnknownObject(solver, builtin);
    if (membersAreFoundByName && !state.membersFollowHolders)
        state.holdersWithoutMembers.append(object);
    else
        exposeMembersOf(solver, object, witness);

    bool isCallable = builtins.dispatchOf(builtin, BuiltinCallMode::Call) || builtins.dispatchOf(builtin, BuiltinCallMode::Construct);
    if (isCallable && !letsInUnseenCode(builtins, builtin))
        callBuiltinFromNativeCode(solver, object);
}

void exposeMembersOfEscapedBuiltins(FlowSolver& solver, const Witness& witness)
{
    FlowUnseenCodeState& state = solver.callState().unseenCode;
    if (std::exchange(state.membersFollowHolders, true))
        return;
    letConstructorsShareLocationsWithUnknownObject(solver);
    for (FlowObjectID holder : std::exchange(state.holdersWithoutMembers, { }))
        exposeMembersOf(solver, holder, witness);
}

void exposeNamespace(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    ModuleID id = solver.objects().at(object).module;
    const Module& module = solver.program().at(id);

    auto exposeExport = [&](const ModuleExport& entry) {
        switch (entry.kind) {
        case ModuleExportKind::Variable:
            exposeLocation(solver, object, solver.graph().variable(entry.variable));
            return;
        case ModuleExportKind::Namespace:
            connectNamespaceOf(solver, entry.module, solver.graph().nativeCode(), witness);
            return;
        case ModuleExportKind::Indirect:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    useModulesBehindNamespace(solver, id, witness);
    for (const ModuleExport& entry : module.exports)
        exposeExport(entry);
    for (const SyntheticExport& entry : module.syntheticExports) {
        if (entry.variable)
            exposeLocation(solver, object, solver.graph().variable(entry.variable));
    }
    exposeCapturedNameSources(solver, id, witness);
}

void exposeCoroutineResult(FlowSolver& solver, FlowObjectID object, const Witness&)
{
    resumeGenerator(solver, { nativeCodeSite(solver), ValueID(), ResumeMode::Next, unknownValueNode(solver), solver.graph().nativeCode(), { } }, object);
}

static void openNativeCell(FlowSolver& solver, FlowNodeID cell, const Witness& witness)
{
    solver.connect(cell, solver.graph().unknownJavaScript(), { }, witness);
    solver.joinUnknown(cell, witness);
}

void noteNativeCellReachedThrough(FlowSolver& solver, BuiltinClassID builtinClass, FlowNodeID cell)
{
    FlowUnseenCodeState& state = solver.callState().unseenCode;
    if (state.nativeCellsReachedThroughClass.size() <= builtinClass.index())
        state.nativeCellsReachedThroughClass.grow(builtinClass.index() + 1);
    Vector<FlowNodeID>& cells = state.nativeCellsReachedThroughClass[builtinClass.index()];
    if (cells.contains(cell))
        return;
    cells.append(cell);
    if (state.classesWithInstanceInUnknownJavaScript.get(builtinClass.index()))
        openNativeCell(solver, cell, noWitness);
}

static void forEachNativeCellNamedBy(const BuiltinFlow& flow, NOESCAPE const Invocable<void(BuiltinName)> auto& functor)
{
    auto visit = [&](const BuiltinPlace& place) {
        if (place.kind == BuiltinPlaceKind::NativeCell)
            functor(place.nativeCell);
    };
    for (const BuiltinTransfer& transfer : flow.transfers) {
        visit(transfer.from);
        visit(transfer.to);
    }
    for (const BuiltinCallback& callback : flow.callbacks) {
        visit(callback.newTarget);
        visit(callback.thisValue);
        for (const BuiltinPlace& argument : callback.arguments)
            visit(argument);
        visit(callback.spreadArguments);
        visit(callback.resultTo);
    }
    for (const BuiltinPropertyAction& action : flow.propertyActions)
        visit(action.value);
    for (const BuiltinHandlerCall& call : flow.handlerCalls) {
        visit(call.callee);
        visit(call.thisValue);
        for (const BuiltinPlace& argument : call.arguments)
            visit(argument);
    }
}

// The members of the prototype are there for whoever holds an instance, whether or not seen code applies them.
static void openNativeCellsReachedThrough(FlowSolver& solver, BuiltinClassID builtinClass, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    FlowUnseenCodeState& state = solver.callState().unseenCode;
    for (; builtinClass; builtinClass = builtins.at(builtinClass).parent) {
        if (state.classesWithInstanceInUnknownJavaScript.set(builtinClass.index()))
            continue;
        if (BuiltinID prototype = builtins.at(builtinClass).prototype) {
            for (BuiltinID member : builtins.membersOf(builtins.canonicalID(prototype))) {
                for (const BuiltinOverload& overload : builtins.overloadsOf(member)) {
                    forEachNativeCellNamedBy(overload.flow, [&](BuiltinName cell) {
                        openNativeCell(solver, nativeCellNode(solver, cell), witness);
                    });
                }
            }
        }
        if (state.nativeCellsReachedThroughClass.size() <= builtinClass.index())
            continue;
        Vector<FlowNodeID> cells = state.nativeCellsReachedThroughClass[builtinClass.index()];
        for (FlowNodeID cell : cells)
            openNativeCell(solver, cell, witness);
    }
}

void didReachUnknownJavaScript(FlowSolver& solver, FlowObjectID object)
{
    RELEASE_ASSERT(object != unknownFlowObject);
    Witness witness = reachWitnessOf(solver, object, ObjectMutation::ReachUnknownJavaScript);
    FlowObjectKind kind = solver.objects().kindOf(object);
    if (BuiltinClassID builtinClass = solver.objects().at(object).builtinClass)
        openNativeCellsReachedThrough(solver, builtinClass, witness);

    for (FlowObjectUse use : { FlowObjectUse::Serialized, FlowObjectUse::Awaited, FlowObjectUse::Iterated, FlowObjectUse::TestedByInstanceOf })
        solver.noteUse(object, use);
    if (!isOpenedByJavaScript(kind))
        return;

    // Start-up fixes each member of a built-in object that compiled code takes as a constant, so unseen code replaces none. A walk answers for what such code adds.
    if (kind == FlowObjectKind::Builtin && !solver.records().mayRunUnknownJavaScript()) {
        for (FlowNodeID location : collectLocationsOf(solver, object))
            openLocation(solver, object, location, witness);
        return;
    }

    solver.noteMutation(object, allObjectMutations - reachMutations, witness);
    solver.noteEvent(object, FlowObjectEvent::StoredByUnknownKey, witness);
    solver.noteEvent(object, FlowObjectEvent::DeletedByUnknownKey, witness);
    if (hasElementParts(solver, object))
        joinPositions(solver, object, witness);

    for (FlowNodeID location : collectLocationsOf(solver, object))
        openLocation(solver, object, location, witness);
    openUnnamedParts(solver, object, witness);

    switch (kind) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::ModuleNamespace:
        break;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return;
    case FlowObjectKind::Closure:
        solver.noteEvent(object, FlowObjectEvent::OtherNewTarget, witness);
        passOtherNewTarget(solver, object, witness);
        return;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        noteWritesToMembers(solver, solver.objects().at(object).builtin, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The property "prototype" of a class is neither writable nor configurable.
static bool isFixedPropertyOfClosure(FlowSolver& solver, FlowObjectID object, FlowNodeID node)
{
    if (solver.objects().kindOf(object) != FlowObjectKind::Closure || solver.graph().kindOf(node) != FlowNodeKind::Property)
        return false;
    for (const FlowProperty& property : solver.graph().propertiesOf(object)) {
        if (property.value == node)
            return mutabilityOf(FlowWalkInputs::of(solver), holderOf(solver.objects(), object), property.key) == FlowHolderMutability::Immutable;
    }
    return false;
}

void openLocation(FlowSolver& solver, FlowObjectID object, FlowNodeID node, const Witness& witness)
{
    solver.connect(node, solver.graph().unknownJavaScript(), { }, witness);
    if (isFixedPropertyOfClosure(solver, object, node))
        return;

    TypeID type = solver.graph().typeAsRead(node);
    solver.join(node, type ? solver.values().valueOfType(type) : solver.values().unknownValue(), witness);

    solver.noteLocationEvent(node, {
        FlowLocationEvent::WrittenByUnknownCode,
        FlowLocationEvent::ReadByUnknownCode,
        FlowLocationEvent::Deleted,
        FlowLocationEvent::Redefined,
        FlowLocationEvent::PresenceObserved,
    });
    keepLowerWitness(solver.records().ensureLocationRecord(node).firstLoss, witness);
}

void openUnnamedParts(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    if (!isOpenedByJavaScript(solver.objects().kindOf(object)))
        return;

    for (FlowOpenPart part : { FlowOpenPart::StringProperties, FlowOpenPart::SymbolProperties, FlowOpenPart::IndexedProperties })
        solver.joinUnknown(solver.ensureOpenPart(object, part), witness);
    changePrototype(solver, FlowObjectSet(object), unknownValueNode(solver), witness);
}

static bool isCalledBySeenCode(FlowSolver& solver, FlowInstanceID instance)
{
    if (solver.graph().isHost(instance))
        return false;
    const Function& function = solver.program().at(solver.graph().at(instance).function);
    if (function.role != FunctionRole::Library && function.role != FunctionRole::Model)
        return true;
    return solver.callState().unseenCode.libraryInstancesCalledBySeenCode.get(instance.index());
}

static void noteCallBySeenCode(FlowSolver& solver, FlowInstanceID callee)
{
    FlowUnseenCodeState& state = solver.callState().unseenCode;
    Vector<FlowInstanceID, 8> pending { callee };
    while (!pending.isEmpty()) {
        FlowInstanceID instance = pending.takeLast();
        if (state.libraryInstancesCalledBySeenCode.set(instance.index()))
            continue;
        // A statement asks for each callee and each method that its cursors hand out, so those come out again.
        for (FlowStatementSite site : state.statementsAwaitingSeenCaller.take(instance))
            solver.revisit(site);
        pending.appendVector(state.calleesOfInstancesAwaitingSeenCaller.take(instance));
    }
}

void didLinkCall(FlowSolver& solver, FlowInstanceID caller, FlowInstanceID callee)
{
    if (isCalledBySeenCode(solver, callee))
        return;

    FlowUnseenCodeState& state = solver.callState().unseenCode;
    if (!isCalledBySeenCode(solver, caller)) {
        if (!solver.graph().isHost(caller))
            state.calleesOfInstancesAwaitingSeenCaller.add(caller, Vector<FlowInstanceID>()).iterator->value.appendIfNotContains(callee);
        return;
    }
    noteCallBySeenCode(solver, callee);
}

void didRequestModule(FlowSolver& solver, FlowInstanceID requester, ModuleID module)
{
    FunctionID body = solver.program().at(module).body;
    if (!body)
        return;
    if (FlowInstanceID instance = solver.enter(body, FlowContext::Proven))
        didLinkCall(solver, requester, instance);
}

void didRequestModuleOfProgram(FlowSolver& solver, ModuleID module)
{
    FunctionID body = solver.program().at(module).body;
    if (!body)
        return;
    FlowInstanceID instance = solver.enter(body, FlowContext::Proven);
    if (instance && !isCalledBySeenCode(solver, instance))
        noteCallBySeenCode(solver, instance);
}

bool isInSeenCode(FlowSolver& solver, FlowStatementSite site)
{
    if (isCalledBySeenCode(solver, site.instance))
        return true;
    if (!solver.graph().isHost(site.instance))
        solver.callState().unseenCode.statementsAwaitingSeenCaller.add(site.instance, Vector<FlowStatementSite>()).iterator->value.appendIfNotContains(site);
    return false;
}

FlowNodeID valuesHandedToUnknownRecipients(FlowSolver& solver)
{
    FlowNodeID node = solver.temporaryFor(callTemporaryKey(nativeCodeSite(solver), FlowCallTemporaryRole::HandedToUnknownRecipients));
    if (solver.valueOf(node).isEmpty()) {
        solver.keepObjectsLettingInUnseenCodeOutOf(node);
        solver.join(node, solver.values().unknownValue().withoutObjects());
    }
    return node;
}

void handToUnknownRecipient(FlowSolver& solver, FlowNodeID node, const Witness& witness)
{
    if (!node)
        return;
    solver.connect(node, valuesHandedToUnknownRecipients(solver));
    solver.connect(node, solver.graph().nativeCode(), { }, witness);
    handToUnseenCode(solver, node, witness);
}

void handToUnseenCode(FlowSolver& solver, FlowNodeID node, const Witness& witness)
{
    if (!node)
        return;
    if (solver.records().unseenCodeCanRun())
        solver.connect(node, solver.graph().unknownJavaScript(), { }, witness);
    else
        solver.callState().unseenCode.nodesHandedToUnknownRecipients.append({ node, witness });
}

void handToUnknownRecipient(FlowSolver& solver, FlowObjectID object, const Witness& witness)
{
    solver.joinObject(valuesHandedToUnknownRecipients(solver), object);
    if (solver.records().unseenCodeCanRun()) {
        solver.joinObject(solver.graph().unknownJavaScript(), object, witness);
        return;
    }
    solver.joinObject(solver.graph().nativeCode(), object, witness);
    solver.callState().unseenCode.objectsHandedToUnknownRecipients.append({ object, witness });
}

static bool hasCommonJSEntry(const Program& program)
{
    return std::ranges::any_of(program.entries, [&](const auto& entry) {
        return program.at(entry.module).kind == ModuleKind::CommonJs;
    });
}

static void letUnseenCodeRun(FlowSolver& solver, const Witness& witness)
{
    solver.records().setUnseenCodeCanRun();

    FlowUnseenCodeState& state = solver.callState().unseenCode;
    FlowNodeID unknownJavaScript = solver.graph().unknownJavaScript();
    for (auto& [node, handed] : std::exchange(state.nodesHandedToUnknownRecipients, { }))
        solver.connect(node, unknownJavaScript, { }, handed);
    for (auto& [object, handed] : std::exchange(state.objectsHandedToUnknownRecipients, { }))
        solver.joinObject(unknownJavaScript, object, handed);

    // Such code evaluates text without the program naming an evaluator, and the interpreter calls functions of the library for that text.
    const BuiltinTable& builtins = solver.builtins();
    for (unsigned index = 0, count = builtins.builtinCount(); index < count; ++index) {
        if (builtins.at(BuiltinID(index)).flags.contains(BuiltinFlag::EvaluatesText))
            noteBuiltinUse(solver, BuiltinID(index), BuiltinUse::Escaped, witness);
    }

    exposeStartupModules(solver, witness);
    // In an image that loads modules by name it loads them too.
    solver.connect(answersOfModulesLoadedByName(solver, FlowModuleAnswer::RequiredValue), solver.graph().unknownJavaScript(), { }, witness);
    solver.connect(answersOfModulesLoadedByName(solver, FlowModuleAnswer::Namespace), solver.graph().unknownJavaScript(), { }, witness);

    // It constructs a built-in whose code is a function of the library for any new.target.
    passOtherNewTargetToEnteredInstances(solver, witness);

    // process.mainModule is the module object of such an entry, and its children lead to every other one.
    if (hasCommonJSEntry(solver.program()))
        solver.connect(commonJSModuleObjectsNode(solver), unknownJavaScript, { }, witness);

    // Unseen code names the global object, and from there every built-in object of the realm: a lookup that has missed on one may find what such code puts there.
    for (uint32_t index = 0, count = solver.objects().size(); index < count; ++index) {
        if (solver.objects().kindOf(FlowObjectID(index)) == FlowObjectKind::Builtin)
            solver.revisitMutationWaiters(FlowObjectID(index));
    }

    if (solver.valueOf(unknownJavaScript).objects.containsUnknownObject())
        solver.connect(solver.graph().nativeCode(), unknownJavaScript, { }, witness);
}

void raiseUnseenCode(FlowSolver& solver, UnseenCodeCause cause, BuiltinID builtin, const Witness& witness)
{
    if (cause == UnseenCodeCause::EvaluatesText && solver.build().options().excludesEvaluatedText())
        return;

    UnseenCodeRecord record { .cause = cause, .witness = witness };
    if (builtin)
        record.builtin = builtin.index();
    solver.records().addUnseenCode(record);

    // What was given up may hold a call or a read that makes native code read them.
    if (cause == UnseenCodeCause::LimitReached)
        exposeStartupModules(solver, witness);
    // So may it hold a call that nothing describes, and code that nobody has seen makes such calls.
    if (cause == UnseenCodeCause::LimitReached || admitsUnknownJavaScript(cause))
        letUndeclaredNativeCodeRun(solver, witness);
    if (admitsUnknownJavaScript(cause) && !solver.records().unseenCodeCanRun())
        letUnseenCodeRun(solver, witness);
    if (opensWholeProgram(cause) && !solver.records().mayRunUnknownJavaScript()) {
        solver.records().setMayRunUnknownJavaScript();
        // A built-in object that reached unknown JavaScript while no such code could run was left as start-up fixes it.
        FlowObjectSet reachedBefore = solver.valueOf(solver.graph().unknownJavaScript()).objects;
        reachedBefore.forEach([&](FlowObjectID object) {
            if (solver.objects().kindOf(object) == FlowObjectKind::Builtin)
                didReachUnknownJavaScript(solver, object);
        });
        passOtherNewTargetToEnteredInstances(solver, witness);
        solver.connect(solver.graph().nativeCode(), solver.graph().unknownJavaScript(), { }, witness);
        // Such code names the global object without being handed it, and from there every built-in.
        solver.joinObject(solver.graph().unknownJavaScript(), ensureBuiltinObject(solver, Builtins::GlobalThis), witness);
        exposeMembersOfEscapedBuiltins(solver, witness);
        for (FlowObjectID holder : std::exchange(solver.callState().unseenCode.holdersOfRefusedMembers, { }))
            exposeMembersOf(solver, holder, witness);
        for (size_t member : std::exchange(solver.callState().unseenCode.membersWithRefusedWrites, { }))
            noteBuiltinUse(solver, BuiltinID(static_cast<uint32_t>(member)), BuiltinUse::Written, witness);
    }
    if (requiresLibraryModules(cause))
        openLibraryModulesToUnseenCode(solver, witness);
    if (loadsModulesByName(cause))
        exposeEveryModule(solver, witness);
    if (opensWholeProgram(cause))
        exposeEveryModuleObject(solver, witness);
}

// A closure that evaluated text leaves behind reads and assigns the variables of its scope during any later call of unknown code. The environments of the scope are operands, so they reach unknownJavaScript themselves.
static void openVariablesOf(FlowSolver& solver, const EvalScope& scope, const Witness& witness)
{
    for (const EvalBinding& binding : scope.bindings) {
        if (!binding.variable)
            continue;
        FlowNodeID variable = solver.graph().variable(binding.variable);
        solver.noteLocationEvent(variable, { FlowLocationEvent::WrittenByUnknownCode, FlowLocationEvent::ReadByUnknownCode });
        keepLowerWitness(solver.records().ensureLocationRecord(variable).firstLoss, witness);
    }
}

void runUnseenCodeEvent(FlowSolver& solver, FlowStatementSite site, const FlowUnseenCodeEvent& event)
{
    // The statement of native code only lends its site: each call that native code makes is a statement of its own.
    if (!isInSeenCode(solver, site))
        return;

    Witness witness = solver.witnessAt(site, event.origin);
    raiseUnseenCode(solver, event.cause, BuiltinID(), witness);
    if (!event.origin || solver.build().options().excludesEvaluatedText())
        return;
    const Value& operation = solver.operationAt(site, event.origin);
    if (operation.opcode == Opcode::DirectEval)
        openVariablesOf(solver, solver.program().at(operation.evalScope()), witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
