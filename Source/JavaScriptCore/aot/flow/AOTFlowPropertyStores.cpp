/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyAccessInternal.h"

#if ENABLE(AOT)

#include "AOTFlowBuiltinCalls.h"
#include "AOTFlowModules.h"
#include "AOTFlowOrder.h"
#include "AOTFlowParts.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using FlowSetterSources = Vector<FlowPropertySource, 8>;

static constexpr OptionSet<ObjectMutation> unknownKeyStoreMutations { ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation };

static FlowPropertySource unknownSourceAt(const FlowHolder& holder)
{
    return { FlowPropertySourceKind::Unknown, FlowCertainty::Predicted, true, holder.object, { }, { }, { } };
}

static bool isOrAliasesUnknownObject(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    return holder.object == unknownFlowObject ? inputs.hasReachedNativeCode(unknownFlowObject) : aliasesUnknownObject(inputs, holder);
}

static void noteLoss(const FlowAccessContext& context, FlowNodeID location, FlowLocationEvent event)
{
    if (!location)
        return;

    FlowSolver& solver = context.solver;
    solver.noteLocationEvent(location, event);
    keepLowerWitness(solver.records().ensureLocationRecord(location).firstLoss, context.witness);
}

// Native code reaches the built-in objects of the realm without being handed them, and it reads and calls what the program puts there.
static void exposeValuePutIntoBuiltin(const FlowAccessContext& context, FlowObjectID holder)
{
    FlowSolver& solver = context.solver;
    if (holder && solver.objects().kindOf(holder) == FlowObjectKind::Builtin)
        handToUnknownRecipient(solver, context.access.value, context.witness);
}

static FlowCertainty certaintyOfOwnName(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID name)
{
    if (mutabilityOf(inputs, holder, name) == FlowHolderMutability::Immutable)
        return FlowCertainty::Proven;
    if (inputs.hasReachedUnknownJavaScript(holder.object))
        return FlowCertainty::Predicted;
    std::optional<FlowOpenPart> part = unnamedKeyPartOf(classOf(inputs.program, name));
    if (part && hasUnnamedKeyStores(inputs, holder.object, *part))
        return FlowCertainty::Predicted;
    return FlowCertainty::Proven;
}

static void appendBuiltinSetter(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowOwnProperty& own, FlowCertainty certainty, FlowSetterSources& setters)
{
    const BuiltinTable& builtins = inputs.builtins();
    switch (builtins.at(own.builtin).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
        return;
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        if (builtins.dispatchOf(own.builtin, BuiltinCallMode::Set))
            setters.append(FlowPropertySource { FlowPropertySourceKind::BuiltinSetter, certainty, true, holder.object, own.key, { }, own.builtin });
        return;
    case BuiltinKind::NativeBinding:
        setters.append(unknownSourceAt(holder));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void appendOwnSetters(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowOwnProperty& own, FlowCertainty certainty, FlowSetterSources& setters)
{
    if (own.lookup == FlowOwnLookup::Unknown)
        setters.append(unknownSourceAt(holder));
    if (own.setter)
        setters.append(FlowPropertySource { FlowPropertySourceKind::Setter, certainty, true, holder.object, own.key, own.setter, { } });
    if (own.builtin)
        appendBuiltinSetter(inputs, holder, own, certainty, setters);
}

static void appendAliasSetter(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID name, FlowSetterSources& setters)
{
    if (classOf(inputs.program, name) == FlowKeyClass::PrivateName)
        return;
    const FlowProperty* entry = inputs.graph.findProperty(unknownFlowObject, name);
    if (entry && entry->setter)
        setters.append(FlowPropertySource { FlowPropertySourceKind::Setter, FlowCertainty::Predicted, true, holder.object, name, entry->setter, { } });
}

// Escaping gives the unknown object an entry per name of the escaped object, so only this mutation says that an accessor was defined through it.
static bool mayHaveAccessorsDefinedThroughUnknownObject(const FlowAccessContext& context, const FlowWalkInputs& inputs)
{
    context.solver.waitOnMutations(unknownFlowObject, context.access.site);
    return inputs.records.possibleMutations(unknownFlowObject).contains(ObjectMutation::RedefineProperty);
}

// Before it writes anything a store looks the name up on the object it starts at. An accessor there takes the value or refuses it. A data property is the target, or for another receiver one more member that the store passes.
static void noteLookupAtStart(const FlowAccessContext& context, const FlowHolder& start, const FlowOwnProperty& own)
{
    FlowSolver& solver = context.solver;
    bool passesDataProperty = context.access.thisValue && own.value && solver.graph().kindOf(own.value) == FlowNodeKind::Property;
    if (!own.getter && !own.setter && !passesDataProperty)
        return;
    if (memberHolderKindOf(solver.objects(), start.object) != FlowMemberHolder::No)
        noteMemberRead(solver, context.access.site, start.object, own.key);
}

static void collectOwnSetters(const FlowAccessContext& context, const FlowWalkInputs& inputs, const FlowHolder& start, FlowSetterSources& setters)
{
    const FlowKeySet& keys = context.keys;
    bool isAlias = isOrAliasesUnknownObject(inputs, start);

    if (inputs.objects.kindOf(start.object) == FlowObjectKind::Opaque)
        setters.append(unknownSourceAt(start));

    for (PropertyKeyID name : keys.names) {
        FlowOwnProperty own = findOwnProperty(inputs, start, name);
        own.key = name;
        bool hasAccessor = own.setter || own.builtin;
        appendOwnSetters(inputs, start, own, hasAccessor ? certaintyOfOwnName(inputs, start, name) : FlowCertainty::Predicted, setters);
        noteLookupAtStart(context, start, own);
        // Nobody defines an accessor under a name that the holder is locked under, through whatever alias, and the walk waits for nothing at such a holder.
        if (mutabilityOf(inputs, start, name) == FlowHolderMutability::Immutable)
            continue;
        if (!isAlias) {
            // Where the receiver itself answers for the name no walk goes above it, so none waits for the receiver to reach native code.
            context.solver.waitOnMutations(start.object, context.access.site, ObjectMutation::ReachNativeCode);
            continue;
        }
        context.solver.waitOnProperty(unknownFlowObject, name, context.access.site);
        appendAliasSetter(inputs, start, name, setters);
    }

    if (!keys.hasUnlistedNames())
        return;
    FlowOwnListing listing = forEachOwnProperty(inputs, start, [&](const FlowOwnProperty& own) {
        if (!own.key || keys.names.contains(own.key) || !keys.mayName(own.key, inputs.program))
            return;
        appendOwnSetters(inputs, start, own, FlowCertainty::Predicted, setters);
        noteLookupAtStart(context, start, own);
    });
    bool mayHaveAliasedSetters = isAlias && mayHaveAccessorsDefinedThroughUnknownObject(context, inputs);
    if (listing == FlowOwnListing::MayHaveOthers || mayHaveAliasedSetters)
        setters.append(unknownSourceAt(start));
}

static void callSetter(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        callAccessor(context, receiver, source);
        return;
    case FlowPropertySourceKind::Unknown:
        passToUnknownAccessor(context, receiver, source);
        return;
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Value:
    case FlowPropertySourceKind::Getter:
    case FlowPropertySourceKind::BuiltinMember:
    case FlowPropertySourceKind::BuiltinGetter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const FlowPropertySource* findNearestSetter(std::span<const FlowPropertySource> setters, PropertyKeyID name)
{
    for (const FlowPropertySource& source : setters) {
        if (source.kind == FlowPropertySourceKind::Unknown)
            return nullptr;
        if (source.key == name)
            return &source;
    }
    return nullptr;
}

static bool isPresentFromCreation(const FlowWalkInputs& inputs, const FlowPropertySource& setter)
{
    if (setter.kind == FlowPropertySourceKind::BuiltinSetter)
        return setter.holder && mutabilityOf(inputs, holderOf(inputs.objects, setter.holder), setter.key) == FlowHolderMutability::Immutable;

    OptionSet<FlowLocationEvent> events = inputs.records.recordOf(setter.node).events;
    return events.contains(FlowLocationEvent::InitializedAtAllocation) && !events.containsAny({ FlowLocationEvent::Deleted, FlowLocationEvent::Redefined });
}

// One operation defines the accessor, as far as anybody has seen. It is taken to have completed before every lookup of the name until the order of execution says otherwise.
static bool isAssumedPresent(const FlowWalkInputs& inputs, FlowStatementSite waiter, const FlowPropertySource& setter)
{
    if (setter.kind != FlowPropertySourceKind::Setter)
        return false;
    OptionSet<FlowLocationEvent> events = inputs.records.recordOf(setter.node).events;
    if (!events.contains(FlowLocationEvent::StoredByOperation) || events.containsAny(eventsChangingDefinedAccessor))
        return false;

    FlowSolver& solver = *inputs.solver;
    solver.waitOnProperty(setter.holder, setter.key, waiter);
    solver.propertyState().accessorsAssumedPresent.add(setter.holder, setter.key);
    return true;
}

static bool isStoredUnderUnnamedKeys(const FlowWalkInputs& inputs, FlowStatementSite waiter, const FlowHolder& holder, FlowOpenPart part)
{
    if (mutabilityOf(inputs, holder) == FlowHolderMutability::Immutable)
        return false;
    if (hasUnnamedKeyStores(inputs, holder.object, part))
        return true;

    FlowSolver& solver = *inputs.solver;
    solver.waitOn(solver.ensureOpenPart(holder.object, part), waiter, FlowWakeUp::Revisit);
    return false;
}

static bool mayChangeThroughUnknownObject(const FlowWalkInputs& inputs, FlowStatementSite waiter, PropertyKeyID name)
{
    FlowSolver& solver = *inputs.solver;
    solver.waitOnMutations(unknownFlowObject, waiter);
    if (inputs.records.possibleMutations(unknownFlowObject).contains(ObjectMutation::DeleteField))
        return true;

    solver.waitOnProperty(unknownFlowObject, name, waiter);
    const FlowProperty* entry = inputs.graph.findProperty(unknownFlowObject, name);
    if (!entry)
        return false;
    if (entry->getter || entry->setter)
        return true;

    FlowNodeID alias = entry->value;
    solver.waitOn(alias, waiter, FlowWakeUp::Revisit);
    return !solver.valueOf(alias).isEmpty();
}

static bool runsOnlySetter(const FlowWalkInputs& inputs, FlowStatementSite waiter, const FlowHolder& start, PropertyKeyID name, std::span<const FlowPropertySource> setters)
{
    if (!waiter.instance)
        return false;

    const FlowPropertySource* first = findNearestSetter(setters, name);
    if (!first || first->certainty != FlowCertainty::Proven)
        return false;
    if (!isPresentFromCreation(inputs, *first) && !isAssumedPresent(inputs, waiter, *first))
        return false;

    std::optional<FlowOpenPart> part = unnamedKeyPartOf(classOf(inputs.program, name));
    FlowObjectSet visited;
    FlowHolder current = start;
    while (current.object && visited.add(current.object)) {
        FlowOwnProperty own = findOwnProperty(inputs, current, name);
        if (own.value)
            return false;
        bool canChange = mutabilityOf(inputs, current, name) == FlowHolderMutability::Ordinary;
        if (canChange && part && isStoredUnderUnnamedKeys(inputs, waiter, current, *part))
            return false;

        bool isAlias = canChange && isOrAliasesUnknownObject(inputs, current);
        if (current.object == first->holder)
            return !isAlias || !mayChangeThroughUnknownObject(inputs, waiter, name);
        if (own.lookup != FlowOwnLookup::Absent || isAlias)
            return false;

        FlowPrototypeSet prototypes = prototypesOf(inputs, current, waiter);
        if (prototypes.holders.size() != 1 || !prototypes.isNeverNull || !prototypes.isNeverUnknown)
            return false;
        current = prototypes.holders[0];
    }
    return false;
}

// The table lists an own property that every instance of a built-in class is created with under the prototype of the class.
// A receiver that is created without it finds it as a data property of an object it inherits from, and a store defines a property of its own.
static bool isOwnPropertyOfAnotherObject(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    if (source.kind != FlowPropertySourceKind::BuiltinSetter || context.access.thisValue)
        return false;

    const FlowSolver& solver = context.solver;
    if (!solver.builtins().at(source.builtin).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
        return false;

    bool isCreatedWithReceiver = false;
    forEachKeyCreatedWithObject(solver.program(), solver.objects(), receiver, [&](PropertyKeyID key) {
        isCreatedWithReceiver |= key == source.key;
    });
    return !isCreatedWithReceiver;
}

static void noteLockedBuiltinWrites(const FlowAccessContext& context, FlowObjectID target, const FlowKeySet& keys)
{
    FlowSolver& solver = context.solver;
    FlowHolder holder = holderOf(solver.objects(), target);
    if (!holder.builtin)
        return;
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    for (PropertyKeyID name : keys.names) {
        if (mutabilityOf(inputs, holder, name) == FlowHolderMutability::Immutable)
            solver.records().addLockedBuiltinWrite(holder.builtin, name, context.witness);
    }
}

static bool mayNameInstanceMember(const FlowAccessContext& context, BuiltinClassID builtinClass)
{
    const Program& program = context.solver.program();
    const BuiltinTable& builtins = program.builtins();
    BuiltinID prototype = builtinClass ? builtins.at(builtinClass).prototype : BuiltinID();
    if (!prototype)
        return false;

    for (BuiltinID member : builtins.membersOf(prototype)) {
        const Builtin& entry = builtins.at(member);
        if (entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) && context.keys.mayName(program.keyOf(entry.name), program))
            return true;
    }
    return false;
}

// An escaped list comes back as the unknown object, and a store to its length empties positions or adds empty ones.
static void moveElementsOfEscapedLists(const FlowAccessContext& context)
{
    FlowSolver& solver = context.solver;
    bool mayNameOwnKeyOfList = std::ranges::any_of(solver.propertyState().ownKeysOfLists, [&](PropertyKeyID key) {
        return context.keys.mayName(key, solver.program());
    });
    if (!mayNameOwnKeyOfList)
        return;
    joinPositions(solver, unknownFlowObject, context.witness);
    solver.noteEvent(unknownFlowObject, FlowObjectEvent::HoleMade, context.witness);
}

static BuiltinClassID builtinClassOfInstance(const FlowObjectTable& objects, FlowObjectID object)
{
    const FlowObject& record = objects.at(object);
    if (record.builtinClass)
        return record.builtinClass;
    return objects.isMadeByBuiltinConstructor(object) ? objects.at(record.site).description.builtinClass : BuiltinClassID();
}

static bool mayBeArray(const FlowObjectTable& objects, FlowObjectID object)
{
    return object == unknownFlowObject || builtinClassOfInstance(objects, object) == BuiltinClasses::Array;
}

// A typed array converts what is stored under a number to a number or a big integer, whether or not it has the index. From its creation on its elements hold every value that comes of that.
static bool convertsStoredElements(const FlowSolver& solver, FlowObjectID holder)
{
    const BuiltinTable& builtins = solver.builtins();
    BuiltinSlotID slot = indexedElementSlotOf(builtins, builtinClassOfInstance(solver.objects(), holder));
    return slot && builtins.at(slot).heap == AbstractHeap::TypedArrayElements;
}

static void defineKeys(const FlowAccessContext& context, FlowObjectID target, const FlowKeySet& keys)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;

    noteLockedBuiltinWrites(context, target, keys);
    if (access.value && keys.mayNameIndex() && convertsStoredElements(solver, target))
        runProtocolsInOwnStatement(solver, { access.site, access.origin, access.value, Protocol::ToPrimitive, { }, FlowNodeID(), access.thrown });
    forEachPropertySource(solver, target, keys, FlowPropertyWalk { FlowWalkOperation::DefineOwnProperty, false, access.site }, [&](const FlowPropertySource& source) {
        switch (source.kind) {
        case FlowPropertySourceKind::Value:
            storeIntoTarget(context, source);
            return;
        case FlowPropertySourceKind::Unknown:
            handToUnknownRecipient(solver, access.value, context.witness);
            return;
        case FlowPropertySourceKind::Absent:
        case FlowPropertySourceKind::Getter:
        case FlowPropertySourceKind::Setter:
        case FlowPropertySourceKind::BuiltinMember:
        case FlowPropertySourceKind::BuiltinGetter:
        case FlowPropertySourceKind::BuiltinSetter:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static constexpr OptionSet<ObjectMutation> mutationsRemovingMembers { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty };

static bool isDataMember(const BuiltinTable& builtins, BuiltinID member)
{
    switch (builtins.at(member).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
        return true;
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A data member that the table lists is on its holder from the start, so a store finds it there and asks no object further up for a setter.
// Whatever takes the member away records an event at its location or one of these mutations, on the holder or on the unknown object that it aliases, and each of them runs the store again.
static bool findsDataMemberOnReceiver(const FlowAccessContext& context, const FlowWalkInputs& inputs, const FlowHolder& receiver, PropertyKeyID name)
{
    FlowSolver& solver = context.solver;
    FlowStatementSite site = context.access.site;
    if (!receiver.builtin || !site.instance)
        return false;
    FlowOwnProperty own = findOwnProperty(inputs, receiver, name);
    if (!own.builtin || own.getter || own.setter || !isDataMember(inputs.builtins(), own.builtin))
        return false;
    if (inputs.builtins().at(own.builtin).mutability == BuiltinMutability::ChangesOnItsOwn)
        return false;

    solver.waitOnProperty(receiver.object, name, site);
    solver.waitOnMutations(receiver.object, site, mutationsRemovingMembers);
    if (inputs.records.possibleMutations(receiver.object).containsAny(mutationsRemovingMembers))
        return false;
    if (own.value && inputs.records.recordOf(own.value).events.containsAny({ FlowLocationEvent::Deleted, FlowLocationEvent::Redefined }))
        return false;
    if (!aliasesUnknownObject(inputs, receiver))
        return true;
    solver.waitOnMutations(unknownFlowObject, site, mutationsRemovingMembers);
    return !inputs.records.possibleMutations(unknownFlowObject).containsAny(mutationsRemovingMembers);
}

// An element keeps its own location: what the program puts into a list predicts what it takes out.
static bool isOpenedByUnseenJavaScript(const FlowAccessContext& context, const FlowWalkInputs& inputs, const FlowHolder& receiver)
{
    const FlowAccess& access = context.access;
    if (receiver.builtin || access.thisValue || access.flags.contains(FlowAccessFlag::StartsAtPrototype))
        return false;
    if (context.keys.mayNameIndex() && !context.keys.hasUnnamedKeys())
        return false;
    return inputs.hasReachedUnknownJavaScript(receiver.object) && mutabilityOf(inputs, receiver) == FlowHolderMutability::Ordinary;
}

// Unseen JavaScript may have defined a setter under any key, on the receiver or above it, and each location of the receiver holds the unknown value and has every mutation.
// So the store hands its operands to that code, and what it puts under a name beside that only predicts.
static void storeIntoOpenedReceiver(const FlowAccessContext& context, const FlowHolder& receiver)
{
    FlowSolver& solver = context.solver;
    passToUnknownAccessor(context, receiver.object, unknownSourceAt(receiver));
    for (PropertyKeyID name : context.keys.names) {
        if (FlowNodeID location = ensureOwnValue(solver, receiver.object, name))
            solver.connect(context.access.value, location, enforcedTypeOf(solver.graph(), location));
    }
}

void storeProperties(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;

    if (receiver == unknownFlowObject) {
        forEachPropertySource(solver, receiver, context.keys, walkOf(access), [&](const FlowPropertySource& source) {
            if (source.kind == FlowPropertySourceKind::Value)
                storeIntoTarget(context, source);
            else
                callSetter(context, receiver, source);
        });
        moveElementsOfEscapedLists(context);
        return;
    }

    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    FlowHolder start = holderOf(solver.objects(), receiver);
    if (isOpenedByUnseenJavaScript(context, inputs, start)) {
        storeIntoOpenedReceiver(context, start);
        return;
    }

    FlowSetterSources setters;
    if (!access.flags.contains(FlowAccessFlag::StartsAtPrototype))
        collectOwnSetters(context, inputs, start, setters);
    FlowKeySet keysLookedUpAbove = context.keys;
    if (!access.flags.contains(FlowAccessFlag::StartsAtPrototype)) {
        for (PropertyKeyID name : context.keys.names) {
            if (findsDataMemberOnReceiver(context, inputs, start, name) || isPrototypePropertyOfFunction(inputs, start, name))
                keysLookedUpAbove.names.remove(name);
        }
    }
    if (!keysLookedUpAbove.isEmpty()) {
        forEachPropertySource(solver, receiver, keysLookedUpAbove, FlowPropertyWalk { FlowWalkOperation::Set, true, access.site }, [&](const FlowPropertySource& source) {
            if (!isOwnPropertyOfAnotherObject(context, receiver, source))
                setters.append(source);
        });
    }
    for (const FlowPropertySource& source : setters)
        callSetter(context, receiver, source);

    FlowKeySet defined = context.keys;
    for (PropertyKeyID name : context.keys.names) {
        if (runsOnlySetter(inputs, access.site, start, name, setters.span()))
            defined.names.remove(name);
    }
    if (defined.isEmpty())
        return;

    // A setter that is defined above the receiver later takes the value in place of the receiver.
    bool makesLocationForLackOfSetter = std::ranges::any_of(defined.names, [&](PropertyKeyID name) {
        return !findOwnProperty(inputs, start, name).value && std::ranges::none_of(setters, [&](const FlowPropertySource& source) {
            return source.kind == FlowPropertySourceKind::Unknown || source.key == name;
        });
    });
    if (makesLocationForLackOfSetter && !solver.mayRelyOn(FlowAbsence::SetterAboveReceiver, access.site)) {
        context.storeWaitsForQuietQueues = true;
        return;
    }

    if (!access.thisValue) {
        defineKeys(context, receiver, defined);
        // One of the setters may take the value in place of the receiver.
        if (!setters.isEmpty())
            context.holderOfWrittenMember = { };
        return;
    }

    solver.waitOn(access.thisValue, access.site, FlowWakeUp::Revisit);
    FlowObjectSet targets = solver.valueOf(access.thisValue).objects;
    targets.forEach([&](FlowObjectID target) {
        defineKeys(context, target, defined);
    });
}

void storeProperties(const FlowAccessContext& context, FlowPrimitiveReceiver receiver)
{
    forEachPropertySource(context.solver, receiver, context.keys, walkOf(context.access), [&](const FlowPropertySource& source) {
        callSetter(context, FlowObjectID(), source);
    });
}

void defineProperties(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;

    defineKeys(context, receiver, context.keys);

    FlowObject record = solver.objects().at(receiver);
    if (receiver == unknownFlowObject)
        moveElementsOfEscapedLists(context);
    else if (record.numberOfPositions && mayNameInstanceMember(context, record.builtinClass))
        joinPositions(solver, receiver, context.witness);

    for (PropertyKeyID name : context.keys.names) {
        // A walk for a definition waits for nothing, and an accessor that is defined under the name after this run is replaced by this definition all the same.
        solver.waitOnProperty(receiver, name, context.access.site);
        const FlowProperty* entry = solver.graph().findProperty(receiver, name);
        if (!entry)
            continue;
        FlowProperty property = *entry;
        if (!property.getter && !property.setter)
            continue;
        noteLoss(context, property.getter, FlowLocationEvent::Redefined);
        noteLoss(context, property.setter, FlowLocationEvent::Redefined);
        solver.noteMutation(receiver, ObjectMutation::RedefineProperty, context.witness);
    }
}

// The unnamed-key nodes of the unknown object flow into every escaped object, whether it escaped before or after.
static void forEachUnnamedKeyPartOfUnknownObject(const FlowAccessContext& context, NOESCAPE const Invocable<void(FlowNodeID)> auto& functor)
{
    FlowSolver& solver = context.solver;
    const FlowKeySet& keys = context.keys;

    if (keys.mayNameIndex())
        functor(solver.ensureOpenPart(unknownFlowObject, FlowOpenPart::IndexedProperties));
    if (keys.anyString)
        functor(solver.ensureOpenPart(unknownFlowObject, FlowOpenPart::StringProperties));
    if (keys.anySymbol)
        functor(solver.ensureOpenPart(unknownFlowObject, FlowOpenPart::SymbolProperties));
}

static void joinIntoUnnamedKeysOfUnknownObject(const FlowAccessContext& context, const FlowValue& value)
{
    forEachUnnamedKeyPartOfUnknownObject(context, [&](FlowNodeID unnamedKeys) {
        context.solver.join(unnamedKeys, value, context.witness);
    });
}

void defineAccessors(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    const FlowKeySet& keys = context.keys;
    FlowWalkInputs inputs = FlowWalkInputs::of(solver);
    FlowHolder holder = holderOf(solver.objects(), receiver);
    MethodKind kind = access.kind == FlowAccessKind::DefineGetter ? MethodKind::Getter : MethodKind::Setter;

    noteLockedBuiltinWrites(context, receiver, keys);
    if (mutabilityOf(inputs, holder) == FlowHolderMutability::Immutable)
        return;

    for (PropertyKeyID name : keys.names) {
        // An object that escapes after this statement has run finds the record in the location it aliases.
        if (receiver == unknownFlowObject)
            ensureOwnValue(solver, receiver, name);
        FlowOwnProperty own = findOwnProperty(inputs, holder, name);
        // The program puts the name on the built-in object itself, so no lookup takes it for absent there.
        if (holder.builtin && solver.propertyState().namesWrittenOnBuiltins.add({ solver.builtins().canonicalID(holder.builtin), name }).isNewEntry)
            solver.revisitPropertyWaiters(receiver, name);
        FlowNodeID node = ensureOwnAccessor(solver, receiver, name, kind);
        if (!node)
            continue;
        solver.connect(access.value, node);
        noteStore(solver, { access.site, access.origin, access.value, node, access.flags.contains(FlowAccessFlag::IsInCycle) });
        if (memberHolderKindOf(solver.objects(), receiver) != FlowMemberHolder::No) {
            noteMemberStore(solver, access.site, receiver, name);
            context.holderOfWrittenMember = receiver;
        }
        if (own.builtin)
            noteBuiltinUse(solver, own.builtin, BuiltinUse::Written, context.witness);
        if (own.value) {
            noteLoss(context, own.value, FlowLocationEvent::Redefined);
            solver.noteMutation(receiver, ObjectMutation::RedefineProperty, context.witness);
        } else if (access.site.statement.kind == FlowStatementKind::BuiltinCall) {
            // A piece of a literal says what its object is created with. A built-in defines on an object that exists.
            solver.noteMutation(receiver, ObjectMutation::AddProperty, context.witness);
        }
    }

    if (keys.hasUnnamedKeys() || keys.mayNameIndex()) {
        FlowNodeID unknownJavaScript = solver.graph().unknownJavaScript();
        solver.joinObject(unknownJavaScript, receiver, context.witness);
        // No value is inside never. The edge only says, to whoever follows edges, that the receiver's node is used as the sink is.
        solver.connect(access.receiver, unknownJavaScript, TypeTable::never);
        solver.connect(access.value, unknownJavaScript, { }, context.witness);
        solver.noteMutation(receiver, ObjectMutation::RedefineProperty, context.witness);
        noteWritesToMembersOf(solver, receiver, context.witness);
    }

    exposeValuePutIntoBuiltin(context, receiver);
    if (receiver != unknownFlowObject)
        return;

    handToUnknownRecipient(solver, access.value, context.witness);

    // reach() skips the unknown object, so the objects that alias it learn of an accessor nobody can name from its unnamed-key nodes: from their value while flow solves, from their record afterwards.
    joinIntoUnnamedKeysOfUnknownObject(context, solver.values().unknownValue());
    forEachUnnamedKeyPartOfUnknownObject(context, [&](FlowNodeID unnamedKeys) {
        noteLoss(context, unnamedKeys, FlowLocationEvent::Redefined);
    });
}

static void deleteValue(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    const FlowGraph& graph = solver.graph();

    bool mayBeField = false;
    switch (graph.kindOf(source.node)) {
    case FlowNodeKind::DeclaredPart:
        if (!solver.objects().hasLayout(source.holder))
            return;
        mayBeField = true;
        break;
    case FlowNodeKind::Property:
        mayBeField = !!enforcedTypeOf(graph, source.node);
        break;
    case FlowNodeKind::Variable:
        break;
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        return;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        RELEASE_ASSERT_NOT_REACHED();
    }

    noteLoss(context, source.node, FlowLocationEvent::Deleted);
    solver.joinPrimitives(source.node, Primitive::Undefined);
    if (mayBeField)
        solver.noteMutation(receiver, ObjectMutation::DeleteField, context.witness);
}

static void deleteSource(const FlowAccessContext& context, FlowObjectID receiver, const FlowPropertySource& source)
{
    FlowSolver& solver = context.solver;
    switch (source.kind) {
    case FlowPropertySourceKind::Value:
        deleteValue(context, receiver, source);
        return;
    case FlowPropertySourceKind::Getter:
    case FlowPropertySourceKind::Setter:
        noteLoss(context, source.node, FlowLocationEvent::Deleted);
        solver.noteMutation(receiver, ObjectMutation::RedefineProperty, context.witness);
        solver.joinPrimitives(ensureOwnValue(solver, receiver, source.key), Primitive::Undefined);
        return;
    case FlowPropertySourceKind::BuiltinMember: {
        FlowHolder holder = holderOf(solver.objects(), receiver);
        if (mutabilityOf(FlowWalkInputs::of(solver), holder, source.key) == FlowHolderMutability::Immutable) {
            solver.records().addLockedBuiltinWrite(holder.builtin, source.key, context.witness);
            return;
        }
        noteBuiltinUse(solver, source.builtin, BuiltinUse::Written, context.witness);
        FlowNodeID value = ensureOwnValue(solver, receiver, source.key);
        noteLoss(context, value, FlowLocationEvent::Deleted);
        solver.joinPrimitives(value, Primitive::Undefined);
        return;
    }
    case FlowPropertySourceKind::Unknown:
        // The host may have a property under a name the table does not list, and only a record on the holder says that it can go.
        if (receiver == unknownFlowObject)
            return;
        for (PropertyKeyID name : context.keys.names) {
            FlowNodeID value = ensureOwnValue(solver, receiver, name);
            noteLoss(context, value, FlowLocationEvent::Deleted);
            solver.joinPrimitives(value, Primitive::Undefined);
        }
        return;
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::BuiltinGetter:
    case FlowPropertySourceKind::BuiltinSetter:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void deleteProperties(const FlowAccessContext& context, FlowObjectID receiver)
{
    FlowSolver& solver = context.solver;
    const FlowKeySet& keys = context.keys;

    // An object that escapes after this statement has run finds the record in the location it aliases.
    if (receiver == unknownFlowObject) {
        for (PropertyKeyID name : keys.names)
            ensureOwnValue(solver, receiver, name);
        joinIntoUnnamedKeysOfUnknownObject(context, solver.values().valueOfPrimitives(Primitive::Undefined));
    }

    forEachPropertySource(solver, receiver, keys, walkOf(context.access), [&](const FlowPropertySource& source) {
        deleteSource(context, receiver, source);
    });

    if (mutabilityOf(FlowWalkInputs::of(solver), holderOf(solver.objects(), receiver)) == FlowHolderMutability::Immutable)
        return;

    if (keys.mayNameIndex() && mayBeArray(solver.objects(), receiver))
        solver.noteEvent(receiver, FlowObjectEvent::HoleMade, context.witness);
    if (keys.mayNameIndex() && (receiver == unknownFlowObject || solver.objects().at(receiver).numberOfPositions))
        joinPositions(solver, receiver, context.witness);
    if (keys.hasUnnamedKeys()) {
        solver.noteEvent(receiver, FlowObjectEvent::DeletedByUnknownKey, context.witness);
        solver.noteMutation(receiver, ObjectMutation::DeleteField, context.witness);
    }
    forEachKeyCreatedWithObject(solver.program(), solver.objects(), receiver, [&](PropertyKeyID key) {
        if (keys.mayName(key, solver.program()))
            solver.noteMutation(receiver, ObjectMutation::DeleteField, context.witness);
    });
}

static std::optional<FlowOpenPart> unnamedKeyPartOfTarget(const FlowSolver& solver, const FlowPropertySource& target)
{
    if (target.key)
        return unnamedKeyPartOf(classOf(solver.program(), target.key));
    if (solver.graph().kindOf(target.node) == FlowNodeKind::DeclaredPart)
        return FlowOpenPart::IndexedProperties;
    return std::nullopt;
}

static void storeIntoNamedLocation(const FlowAccessContext& context, const FlowPropertySource& target, const FlowLocationStore& store, TypeID enforcedType)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;

    if (!access.flags.contains(FlowAccessFlag::StoresUnobservedValue))
        solver.connect(access.value, target.node, enforcedType);
    noteStore(solver, store);
    if (checkStoredType(solver, store) == FlowStoredType::Inside)
        return;

    noteLoss(context, target.node, FlowLocationEvent::StoredOutsideType);
    solver.noteMutation(target.holder, ObjectMutation::StoreOfOtherType, context.witness);
    if (std::optional<FlowOpenPart> part = unnamedKeyPartOfTarget(solver, target))
        solver.connect(access.value, solver.ensureOpenPart(target.holder, *part));
}

// The value of a store under a number lies with the elements. Under a number that is no index the store adds a property all the same.
static void noteStoreUnderNumericName(const FlowAccessContext& context, FlowObjectID holder)
{
    if (!context.keys.anyNumericName)
        return;
    context.solver.noteEvent(holder, FlowObjectEvent::StoredUnderNumericName, context.witness);
    context.solver.noteMutation(holder, ObjectMutation::AddProperty, context.witness);
}

static void storeIntoUnnamedKeyPart(const FlowAccessContext& context, const FlowPropertySource& target, FlowOpenPart part)
{
    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;

    linkUnnamedKeyStores(solver, target.holder, part);
    solver.connect(access.value, target.node);
    bool isCopy = access.site.statement.kind == FlowStatementKind::PropertyCopy;
    solver.noteEvent(target.holder, isCopy ? FlowObjectEvent::CopiedUnderUnknownKeys : FlowObjectEvent::StoredByComputedKey, context.witness);
    if (part == FlowOpenPart::IndexedProperties) {
        solver.noteMutation(target.holder, ObjectMutation::AddProperty, context.witness);
        noteStoreUnderNumericName(context, target.holder);
        return;
    }

    solver.noteEvent(target.holder, FlowObjectEvent::StoredByUnknownKey, context.witness);
    solver.noteMutation(target.holder, unknownKeyStoreMutations, context.witness);
    noteWritesToMembersOf(solver, target.holder, context.witness);
}

static void storeIntoPart(const FlowAccessContext& context, const FlowPropertySource& target, FlowStoreNaming naming)
{
    const FlowAccess& access = context.access;
    noteStoreUnderNumericName(context, target.holder);
    FlowNodeID retainedValue = convertsStoredElements(context.solver, target.holder) ? FlowNodeID() : access.value;
    accessPart(context.solver, FlowPartAccess { access.site, access.origin, FlowAccessKind::Store, FlowReadUse::Value, FlowLocalID(), retainedValue, naming }, target.node);
}

static void storeIntoDeclaredPart(const FlowAccessContext& context, const FlowPropertySource& target, const FlowLocationStore& store)
{
    FlowSolver& solver = context.solver;
    switch (solver.objects().kindOf(target.holder)) {
    case FlowObjectKind::Allocation:
        storeIntoNamedLocation(context, target, store, enforcedTypeOf(solver.graph(), target.node));
        solver.noteMutation(target.holder, ObjectMutation::StoreAfterCreation, context.witness);
        return;
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        storeIntoPart(context, target, store.naming);
        return;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Nobody knows the length, so an index may lie above it. A key that is converted may be another index than the number that was compared.
static void noteStoreUnderIndex(const FlowAccessContext& context, FlowObjectID holder)
{
    const FlowKeySet& keys = context.keys;
    if (!keys.mayNameIndex() || !mayBeArray(context.solver.objects(), holder))
        return;
    bool isWithinLength = context.access.flags.contains(FlowAccessFlag::IsWithinLength) && keys.names.isEmpty() && !keys.hasUnnamedKeys();
    context.solver.noteEvent(holder, isWithinLength ? FlowObjectEvent::StoredWithinLength : FlowObjectEvent::HoleMade, context.witness);
}

void storeIntoTarget(const FlowAccessContext& context, const FlowPropertySource& target)
{
    RELEASE_ASSERT(target.kind == FlowPropertySourceKind::Value && target.node);

    FlowSolver& solver = context.solver;
    const FlowAccess& access = context.access;
    const FlowGraph& graph = solver.graph();
    noteStoreUnderIndex(context, target.holder);
    // The stamp of an access lists the layouts of the tracked objects that reach its receiver.
    bool namesLocation = access.flags.contains(FlowAccessFlag::NamesReceivers) && target.holder != unknownFlowObject;
    FlowStoreNaming naming = namesLocation ? FlowStoreNaming::NamesLocation : FlowStoreNaming::Unknown;
    FlowLocationStore store { access.site, access.origin, access.value, target.node, access.flags.contains(FlowAccessFlag::IsInCycle), naming };
    // The run time refuses a store that no seen code makes.
    if (holdsLoaderAnswers(solver.objects().at(target.holder).builtin) && isInSeenCode(solver, context.access.site))
        didReplaceLoaderAnswers(solver, context.witness);

    FlowNodeKind kind = graph.kindOf(target.node);
    switch (kind) {
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
        storeIntoUnnamedKeyPart(context, target, *unnamedKeyPartOf(kind));
        break;
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
        storeIntoPart(context, target, naming);
        break;
    case FlowNodeKind::DeclaredPart:
        storeIntoDeclaredPart(context, target, store);
        break;
    case FlowNodeKind::Property: {
        TypeID enforcedType = enforcedTypeOf(graph, target.node);
        storeIntoNamedLocation(context, target, store, enforcedType);
        solver.noteMutation(target.holder, enforcedType ? ObjectMutation::StoreAfterCreation : ObjectMutation::AddProperty, context.witness);
        if (target.key && memberHolderKindOf(solver.objects(), target.holder) != FlowMemberHolder::No) {
            noteMemberStore(solver, access.site, target.holder, target.key);
            context.holderOfWrittenMember = target.holder;
        }
        break;
    }
    case FlowNodeKind::Variable:
        storeIntoNamedLocation(context, target, store, enforcedTypeOf(graph, target.node));
        break;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        RELEASE_ASSERT_NOT_REACHED();
    }

    exposeValuePutIntoBuiltin(context, target.holder);
    if (target.builtin)
        noteBuiltinUse(solver, target.builtin, BuiltinUse::Written, context.witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
