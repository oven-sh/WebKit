/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPrototypes.h"

#if ENABLE(AOT)

#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyAccessInternal.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowPropertyWalkInternal.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr FlowDefaultPrototype nullPrototype { FlowDefaultPrototypeKind::Null, { } };

static FlowDefaultPrototype toDefaultPrototype(const BuiltinTable& table, BuiltinID prototype)
{
    if (!prototype)
        return { };
    return { FlowDefaultPrototypeKind::Builtin, table.canonicalID(prototype) };
}

static FlowDefaultPrototype defaultPrototypeOfClass(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    if (!builtinClass)
        return { };
    return toDefaultPrototype(table, table.at(builtinClass).prototype);
}

static FlowDefaultPrototype defaultPrototypeOfLayout(const Program& program, LayoutID id)
{
    const BuiltinTable& table = program.builtins();
    const Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return defaultPrototypeOfClass(table, BuiltinClasses::Object);
    case LayoutKind::Instance:
        return { };
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return nullPrototype;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinID prototypeOfFunctionObject(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    BuiltinClassID builtinClass = isAsync ? BuiltinClasses::AsyncFunction : BuiltinClasses::Function;
    if (function.has(FunctionFlags::IsGenerator))
        builtinClass = isAsync ? BuiltinClasses::AsyncGeneratorFunction : BuiltinClasses::GeneratorFunction;
    return BuiltinTable::singleton().at(builtinClass).prototype;
}

static BuiltinClassID instanceClassOf(const Program& program, const FlowObjectTable& objects, FlowObjectID object)
{
    const FlowObject& record = objects.at(object);
    if (record.kind == FlowObjectKind::Closure)
        return program.builtins().at(prototypeOfFunctionObject(program.at(record.function))).instanceClass;
    if (objects.isMadeByBuiltinConstructor(object))
        return objects.at(record.site).description.builtinClass;
    return record.builtinClass;
}

void forEachKeyCreatedWithObject(const Program& program, const FlowObjectTable& objects, FlowObjectID object, NOESCAPE const ScopedLambda<void(PropertyKeyID)>& functor)
{
    const BuiltinTable& table = program.builtins();
    for (BuiltinClassID current = instanceClassOf(program, objects, object); current; current = table.at(current).parent) {
        BuiltinID prototype = table.at(current).prototype;
        if (!prototype)
            continue;
        for (BuiltinID member : table.membersOf(prototype)) {
            if (table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
                functor(program.keyOf(table.at(member).name));
        }
    }
}

BuiltinID prototypeOfPrototypeProperty(const Function& function)
{
    BuiltinClassID builtinClass = BuiltinClasses::Object;
    if (function.has(FunctionFlags::IsGenerator))
        builtinClass = function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
    return BuiltinTable::singleton().at(builtinClass).prototype;
}

FlowDefaultPrototype defaultPrototypeOf(const BuiltinTable& table, BuiltinID builtin)
{
    const Builtin& record = table.at(builtin);
    switch (record.prototypeLink) {
    case BuiltinPrototypeLink::Unknown:
        return { };
    case BuiltinPrototypeLink::Null:
        return nullPrototype;
    case BuiltinPrototypeLink::Builtin:
        return toDefaultPrototype(table, record.inheritsFrom);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool constructsClassWithHeritage(const Program& program, FunctionID function)
{
    LayoutID constructedClass = program.at(function).constructedClass;
    return constructedClass && program.at(constructedClass).has(LayoutFlags::HasHeritage);
}

FlowDefaultPrototype defaultPrototypeOf(const Program& program, const FlowObjectTable& objects, FlowObjectID object)
{
    const BuiltinTable& table = program.builtins();
    const FlowObject& record = objects.at(object);
    switch (record.kind) {
    case FlowObjectKind::Unknown:
        return { };
    case FlowObjectKind::Allocation:
        return defaultPrototypeOfLayout(program, record.layout);
    case FlowObjectKind::Closure:
        if (constructsClassWithHeritage(program, record.function))
            return { };
        return toDefaultPrototype(table, prototypeOfFunctionObject(program.at(record.function)));
    case FlowObjectKind::Prototype:
        if (constructsClassWithHeritage(program, record.function))
            return { };
        return toDefaultPrototype(table, prototypeOfPrototypeProperty(program.at(record.function)));
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return defaultPrototypeOfClass(table, record.builtinClass);
    case FlowObjectKind::ModuleNamespace:
        return nullPrototype;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return defaultPrototypeOf(table, record.builtin);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void joinDefaultPrototype(FlowSolver& solver, FlowNodeID target, const FlowDefaultPrototype& prototype)
{
    switch (prototype.kind) {
    case FlowDefaultPrototypeKind::Unknown:
        return;
    case FlowDefaultPrototypeKind::Null:
        solver.joinPrimitives(target, Primitive::Null);
        return;
    case FlowDefaultPrototypeKind::Builtin:
        solver.joinObject(target, ensureBuiltinObject(solver, prototype.builtin));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void joinUnknownPrototype(FlowSolver& solver, FlowNodeID target, const Witness& witness)
{
    solver.joinUnknownObject(target, witness);
    solver.joinPrimitives(target, Primitive::Null);
}

FlowNodeID ensurePrototypeLink(FlowSolver& solver, FlowObjectID object)
{
    if (FlowNodeID existing = solver.graph().find(object, FlowOpenPart::PrototypeLink))
        return existing;
    FlowNodeID link = solver.ensureOpenPart(object, FlowOpenPart::PrototypeLink);
    joinDefaultPrototype(solver, link, defaultPrototypeOf(solver.program(), solver.objects(), object));
    return link;
}

static FlowPrototypeSet prototypesInLink(const FlowWalkInputs& inputs, FlowNodeID link, const FlowObjectSet* leftOut)
{
    FlowPrototypeSet set;
    const FlowValue& value = inputs.graph.valueOf(link);
    auto appendHolder = [&](FlowObjectID prototype) {
        if (prototype != unknownFlowObject)
            set.holders.append(holderOf(inputs.objects, prototype));
    };
    if (leftOut)
        value.objects.forEachNotIn(*leftOut, appendHolder);
    else
        value.objects.forEach(appendHolder);
    set.numberOfHolders = value.objects.size() - value.hasUnknownObject();
    set.isNeverNull = !(value.primitives & Primitive::Null);
    set.isNeverUnknown = !value.hasUnknownObject();
    return set;
}

static FlowPrototypeSet prototypesByDefault(const FlowWalkInputs& inputs, const FlowDefaultPrototype& prototype)
{
    FlowPrototypeSet set;
    switch (prototype.kind) {
    case FlowDefaultPrototypeKind::Unknown:
        return set;
    case FlowDefaultPrototypeKind::Null:
        set.isNeverUnknown = true;
        return set;
    case FlowDefaultPrototypeKind::Builtin:
        set.holders.append(holderOf(inputs, prototype.builtin));
        set.numberOfHolders = 1;
        set.isNeverNull = true;
        set.isNeverUnknown = true;
        return set;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowPrototypeSet prototypesOf(const FlowWalkInputs& inputs, const FlowHolder& holder, FlowStatementSite waiter, const FlowObjectSet* leftOut)
{
    RELEASE_ASSERT(holder.object || holder.builtin);
    if (holder.object == unknownFlowObject)
        return { };

    FlowNodeID link = holder.object ? inputs.graph.find(holder.object, FlowOpenPart::PrototypeLink) : FlowNodeID();
    if (!link && inputs.isSolving() && mutabilityOfPrototypeLink(inputs, holder) == FlowHolderMutability::Ordinary) {
        if (!holder.builtin)
            link = ensurePrototypeLink(*inputs.solver, holder.object);
        else if (holder.object)
            inputs.solver->waitOnMutations(holder.object, waiter, ObjectMutation::ChangePrototype);
    }

    if (link) {
        if (inputs.isSolving())
            inputs.solver->waitOn(link, waiter, FlowWakeUp::Revisit);
        return prototypesInLink(inputs, link, leftOut);
    }

    if (holder.object)
        return prototypesByDefault(inputs, defaultPrototypeOf(inputs.program, inputs.objects, holder.object));
    return prototypesByDefault(inputs, defaultPrototypeOf(inputs.builtins(), holder.builtin));
}

FlowPrototypeSet prototypesFixedAtStartUp(const FlowWalkInputs& inputs, const FlowHolder& holder, FlowStatementSite waiter, const FlowObjectSet* leftOut)
{
    const BuiltinTable& builtins = inputs.builtins();
    if (!canFixPrototypeLinkAtStartUp(builtins, holder.builtin))
        return prototypesOf(inputs, holder, waiter, leftOut);
    if (inputs.isSolving()) {
        inputs.solver->waitOnMutations(holder.object, waiter, ObjectMutation::ChangePrototype);
        inputs.solver->propertyState().takeLinkForConstant(builtins.canonicalID(holder.builtin));
    }
    return prototypesByDefault(inputs, defaultPrototypeOf(builtins, holder.builtin));
}

static void visitPrototypes(const FlowWalkInputs& inputs, FlowObjectID object, FlowStatementSite waiter, NOESCAPE const ScopedLambda<void(FlowObjectID)>& functor)
{
    FlowPrototypeSet set = prototypesOf(inputs, holderOf(inputs.objects, object), waiter);
    bool mayBeUnknown = !set.isNeverUnknown;
    for (const FlowHolder& holder : set.holders) {
        if (holder.object)
            functor(holder.object);
        else
            mayBeUnknown = true;
    }
    if (mayBeUnknown)
        functor(unknownFlowObject);
}

void forEachPrototype(FlowSolver& solver, FlowObjectID object, FlowStatementSite waiter, NOESCAPE const ScopedLambda<void(FlowObjectID)>& functor)
{
    visitPrototypes(FlowWalkInputs::of(solver), object, waiter, functor);
}

void forEachPrototype(const FlowSolution& solution, FlowObjectID object, NOESCAPE const ScopedLambda<void(FlowObjectID)>& functor)
{
    visitPrototypes(FlowWalkInputs::of(solution), object, { }, functor);
}

static bool isCreatedWithKey(const FlowSolver& solver, FlowObjectID object, PropertyKeyID key)
{
    bool isFound = false;
    forEachKeyCreatedWithObject(solver.program(), solver.objects(), object, [&](PropertyKeyID created) {
        isFound |= created == key;
    });
    return isFound;
}

void forEachHolderOfInstanceProperty(FlowSolver& solver, FlowObjectID start, PropertyKeyID key, FlowObjectID listingPrototype, FlowStatementSite waiter, NOESCAPE const ScopedLambda<void(FlowObjectID)>& functor)
{
    FlowObjectSet visited(start);
    Vector<FlowObjectID, 4> worklist { start };
    bool hasHolder = false;
    while (!worklist.isEmpty()) {
        FlowObjectID object = worklist.takeLast();
        if (object == listingPrototype || object == unknownFlowObject || isCreatedWithKey(solver, object, key)) {
            hasHolder = true;
            functor(object);
            continue;
        }
        forEachPrototype(solver, object, waiter, [&](FlowObjectID prototype) {
            if (visited.add(prototype))
                worklist.append(prototype);
        });
    }
    if (!hasHolder)
        functor(listingPrototype);
}

FlowPrototypeEnd prototypeEndOf(const FlowSolution& solution, FlowObjectID object)
{
    FlowWalkInputs inputs = FlowWalkInputs::of(solution);
    Vector<FlowHolder, 8> worklist;
    FlowObjectSet visitedObjects;
    BitVector visitedBuiltins;
    auto enqueue = [&](const FlowHolder& holder) {
        bool isNew = holder.object ? visitedObjects.add(holder.object) : visitedBuiltins.add(holder.builtin.index());
        if (isNew)
            worklist.append(holder);
    };

    enqueue(holderOf(solution.objects, object));
    for (size_t index = 0; index < worklist.size(); ++index) {
        FlowPrototypeSet set = prototypesOf(inputs, worklist[index], { });
        if (!set.isNeverUnknown || (set.holders.isEmpty() && set.isNeverNull))
            return FlowPrototypeEnd::Unknown;
        for (const FlowHolder& prototype : set.holders)
            enqueue(prototype);
    }
    return FlowPrototypeEnd::Null;
}

void connectCreationPrototype(FlowSolver& solver, FlowObjectID object, FlowNodeID prototypes)
{
    if (!object || !prototypes)
        return;
    FlowNodeID link = solver.ensureOpenPart(object, FlowOpenPart::PrototypeLink);
    solver.noteLocationEvent(link, FlowLocationEvent::InitializedAtAllocation);
    solver.connect(prototypes, link, solver.propertyState().objectOrNull);
}

static void noteNewPrototypes(FlowSolver& solver, FlowNodeID prototypes, const Witness& witness)
{
    FlowObjectSet newPrototypes = solver.valueOf(prototypes).objects;
    // Beside the unknown object, which is marked for all of them, an escaped object is a prediction, and nobody waits for one. Running every rule again marks none.
    if (solver.isCheckingFixpoint() && newPrototypes.containsUnknownObject()) {
        newPrototypes.subtract(solver.escapedObjects());
        newPrototypes.add(unknownFlowObject);
    }
    solver.noteMutationOf(newPrototypes, ObjectMutation::BecomePrototype, witness);
}

void connectCreationPrototypeOfUnknownObject(FlowSolver& solver, FlowNodeID prototypes, const Witness& witness)
{
    if (!prototypes)
        return;
    solver.connect(prototypes, solver.graph().nativeCode(), { }, witness);
    noteNewPrototypes(solver, prototypes, witness);
}

void changePrototype(FlowSolver& solver, const FlowObjectSet& objects, FlowNodeID prototypes, const Witness& witness)
{
    if (!prototypes)
        return;

    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    objects.forEach([&](FlowObjectID object) {
        if (mutabilityOfPrototypeLink(inputs, holderOf(inputs.objects, object)) == FlowHolderMutability::Immutable)
            return;
        if (BuiltinID builtin = holderOf(inputs.objects, object).builtin)
            solver.propertyState().builtinsWithPrototypeChangedByProgram.set(solver.builtins().canonicalID(builtin).index());
        solver.noteMutation(object, ObjectMutation::ChangePrototype, witness);
        solver.connect(prototypes, ensurePrototypeLink(solver, object), inputs.state.objectOrNull);
    });

    noteNewPrototypes(solver, prototypes, witness);
}

static void loadPrototype(FlowSolver& solver, FlowStatementSite site, FlowObjectID object, FlowNodeID result, const Witness& witness)
{
    if (object == unknownFlowObject) {
        joinUnknownPrototype(solver, result, witness);
        return;
    }

    FlowHolder holder = holderOf(solver.objects(), object);
    bool isImmutable = mutabilityOfPrototypeLink(FlowWalkInputs::of(solver), holder) == FlowHolderMutability::Immutable;
    if ((!holder.builtin && !isImmutable) || solver.graph().find(object, FlowOpenPart::PrototypeLink)) {
        solver.connect(ensurePrototypeLink(solver, object), result);
        return;
    }

    if (!isImmutable)
        solver.waitOnMutations(object, site, ObjectMutation::ChangePrototype);
    FlowDefaultPrototype prototype = defaultPrototypeOf(solver.program(), solver.objects(), object);
    if (prototype.kind == FlowDefaultPrototypeKind::Unknown)
        joinUnknownPrototype(solver, result, witness);
    else
        joinDefaultPrototype(solver, result, prototype);
}

void loadPrototypes(FlowSolver& solver, FlowStatementSite site, const FlowNewObjects& objects, FlowNodeID result, const Witness& witness)
{
    solver.forEachMember(objects, [&](FlowObjectID object) {
        loadPrototype(solver, site, object, result, witness);
    });
}

void loadPrototypes(FlowSolver& solver, FlowPrimitiveReceiver receiver, FlowNodeID result)
{
    for (const FlowPrimitiveWrapper& wrapper : primitiveWrappers()) {
        if (receiver.primitives & wrapper.primitives)
            joinDefaultPrototype(solver, result, defaultPrototypeOfClass(solver.builtins(), wrapper.builtinClass));
    }
}

static bool standsForSeveralSites(const FlowObjectTable& objects, FlowObjectID object)
{
    const FlowObject& record = objects.at(object);
    return record.standsForLayout || !record.allocationClass || objects.at(record.allocationClass).sites.size() > 1;
}

static void storeLiteralPrototype(FlowSolver& solver, FlowObjectID object, FlowNodeID prototypes)
{
    if (object == unknownFlowObject)
        return;

    FlowNodeID link = solver.ensureOpenPart(object, FlowOpenPart::PrototypeLink);
    solver.connect(prototypes, link, solver.propertyState().objectOrNull);

    // Another literal of the same allocation class may name no prototype.
    bool ignoresStoredValue = prototypes && (solver.valueOf(prototypes).primitives & ~Primitive::Null);
    if (ignoresStoredValue || standsForSeveralSites(solver.objects(), object))
        joinDefaultPrototype(solver, link, defaultPrototypeOf(solver.program(), solver.objects(), object));
}

void runPrototypeAccess(FlowSolver& solver, FlowStatementSite site, const FlowPrototypeAccess& statement)
{
    FlowNodeID objectNode = solver.node(site, statement.object);
    FlowNodeID valueNode = solver.node(site, statement.value);
    noteDeclaredOperandUses(solver, site, statement.origin);
    if (!objectNode)
        return;

    FlowObjectSet objects = solver.valueOf(objectNode).objects;
    switch (statement.kind) {
    case FlowAccessKind::Load:
        loadPrototypes(solver, site, objects, valueNode, solver.witnessAt(site, statement.origin));
        return;
    case FlowAccessKind::Store:
        objects.forEach([&](FlowObjectID object) {
            storeLiteralPrototype(solver, object, valueNode);
        });
        return;
    case FlowAccessKind::Define:
    case FlowAccessKind::DefineGetter:
    case FlowAccessKind::DefineSetter:
    case FlowAccessKind::Delete:
    case FlowAccessKind::Test:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasUnknownCreationPrototype(const FlowSolver& solver, FlowObjectID object, FlowNodeID link)
{
    if (solver.valueOf(link).isEmpty())
        return true;
    if (solver.records().recordOf(link).events.contains(FlowLocationEvent::InitializedAtAllocation))
        return false;
    return defaultPrototypeOf(solver.program(), solver.objects(), object).kind == FlowDefaultPrototypeKind::Unknown;
}

FlowProgress refinePrototypeUses(FlowSolver& solver)
{
    for (uint32_t index = 0; index < solver.objects().size(); ++index) {
        FlowObjectID object(index);
        FlowNodeID link = solver.graph().find(object, FlowOpenPart::PrototypeLink);
        if (!link)
            continue;
        if (object != unknownFlowObject && hasUnknownCreationPrototype(solver, object, link))
            joinUnknownPrototype(solver, link, noWitness);
        solver.noteMutationOf(FlowObjectSet(solver.valueOf(link).objects), ObjectMutation::BecomePrototype, noWitness);
    }
    return solver.hasQueuedWork() ? FlowProgress::QueuedWork : FlowProgress::Finished;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
