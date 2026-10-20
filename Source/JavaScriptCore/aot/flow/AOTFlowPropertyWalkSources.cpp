/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyWalkInternal.h"

#if ENABLE(AOT)

#include "AOTFlowBuiltinLocations.h"
#include "AOTBuiltinMembers.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowModules.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyState.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr bool readsProperties(FlowWalkOperation operation)
{
    switch (operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
        return true;
    case FlowWalkOperation::Set:
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::Delete:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr bool readsPrototypeChain(FlowWalkOperation operation)
{
    return readsProperties(operation) && followsPrototypes(operation);
}

static bool isOrdinary(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    return mutabilityOf(inputs, holder) == FlowHolderMutability::Ordinary;
}

static bool canBeSource(const FlowWalkInputs& inputs, FlowNodeID node)
{
    return node && (inputs.isSolving() || !inputs.graph.valueOf(node).isEmpty());
}

// Escaping gives the unknown object an entry per name of the escaped object, so only its mutations say that something was defined through it.
static bool mayHaveUnlistedAliases(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowPropertyWalk& walk)
{
    OptionSet<ObjectMutation> definitions;
    switch (walk.operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
        definitions = { ObjectMutation::AddProperty, ObjectMutation::RedefineProperty };
        break;
    case FlowWalkOperation::Set:
        definitions = ObjectMutation::RedefineProperty;
        break;
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::Delete:
        return false;
    }
    if (!aliasesUnknownObject(inputs, holder))
        return false;

    if (inputs.isSolving())
        inputs.solver->waitOnMutations(unknownFlowObject, walk.waiter, definitions);
    return inputs.records.possibleMutations(unknownFlowObject).containsAny(definitions);
}

static Vector<FlowNodeID, 2> computedStaticFieldsOf(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    Vector<FlowNodeID, 2> fields;
    if (!holder.object || inputs.objects.kindOf(holder.object) != FlowObjectKind::Closure)
        return fields;
    LayoutID constructedClass = inputs.program.at(inputs.objects.at(holder.object).function).constructedClass;
    if (!constructedClass || inputs.program.at(constructedClass).staticFields.isEmpty())
        return fields;

    forEachOwnProperty(inputs, holder, [&](const FlowOwnProperty& property) {
        if (!property.key && property.value)
            fields.append(property.value);
    });
    return fields;
}

static bool standsForOneIndex(const FlowWalkInputs& inputs, FlowObjectID holder, FlowNodeID node)
{
    switch (inputs.graph.kindOf(node)) {
    case FlowNodeKind::Position:
        return true;
    case FlowNodeKind::DeclaredPart:
        return inputs.objects.kindOf(holder) == FlowObjectKind::Allocation;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Escaping gives the unknown object an entry per name of the escaped object, so the entry is judged by what it holds. A value stored through it leaves the location filled.
static bool mayBeEmptiedThroughUnknownObject(const FlowWalkInputs& inputs, PropertyKeyID key)
{
    if (inputs.records.possibleMutations(unknownFlowObject).contains(ObjectMutation::DeleteField))
        return true;

    const FlowProperty* alias = inputs.graph.findProperty(unknownFlowObject, key);
    if (!alias)
        return false;
    if (alias->getter || alias->setter)
        return true;
    return alias->value && inputs.records.recordOf(alias->value).events.containsAny(emptyingLocationEvents);
}

// One key stands for each symbol that its call creates, so an object that has one of them may lack the one that is looked up.
static bool standsForOneKey(const FlowWalkInputs& inputs, PropertyKeyID key)
{
    switch (inputs.program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return true;
    case PropertyKeyKind::CreatedSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isFilled(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowOwnProperty& own)
{
    if (own.presence != FlowOwnPresence::ShownByValue || !own.value || own.getter || own.setter || !standsForOneKey(inputs, own.key))
        return false;
    if (inputs.records.recordOf(own.value).events.containsAny(emptyingLocationEvents))
        return false;
    return !aliasesUnknownObject(inputs, holder) || !mayBeEmptiedThroughUnknownObject(inputs, own.key);
}

static bool isProvenFilled(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowOwnProperty& own, FlowCertainty certainty)
{
    return !inputs.isSolving() && certainty == FlowCertainty::Proven && isFilled(inputs, holder, own);
}

// Whatever takes the answer back runs the waiter of the walk again. An event that empties the location, or the location of the unknown object that it aliases, and an accessor created beside either of them wake whoever waits on the name.
// An escape and the arrival of unseen JavaScript are mutations of the holder. A deletion through the unknown object under a key nobody can name is a mutation of that object.
// A store looks for setters above a location that it may be the first to fill.
static bool isAssumedFilled(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowOwnProperty& own, const FlowPropertyWalk& walk)
{
    if (!inputs.isSolving() || !walk.waiter.instance || !readsProperties(walk.operation) || !own.value)
        return false;
    if (inputs.hasReachedUnknownJavaScript(holder.object))
        return false;

    switch (inputs.graph.kindOf(own.value)) {
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Property:
        break;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return false;
    }
    if (aliasesUnknownObject(inputs, holder))
        inputs.solver->waitOnMutations(unknownFlowObject, walk.waiter, ObjectMutation::DeleteField);
    return isFilled(inputs, holder, own);
}

static constexpr OptionSet<ObjectMutation> mutationsEmptyingLocations { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty };

// Whatever takes a member of the table away from its holder first creates an entry under the name or records one of these mutations, on the holder or on the unknown object it aliases. Each of them runs the waiter of the walk again.
static bool isUntouchedBuiltinMember(const FlowWalkInputs& inputs, const FlowHolder& holder, const FlowOwnProperty& own, const FlowPropertyWalk& walk)
{
    if (!own.builtin || own.value || own.getter || own.setter)
        return false;
    if (inputs.builtins().at(own.builtin).mutability == BuiltinMutability::ChangesOnItsOwn)
        return false;
    if (inputs.isSolving() && (!holder.object || !walk.waiter.instance))
        return false;
    if (!holder.object)
        return true;
    if (inputs.records.possibleMutations(holder.object).containsAny(mutationsEmptyingLocations))
        return false;
    if (!aliasesUnknownObject(inputs, holder))
        return true;

    if (inputs.isSolving())
        inputs.solver->waitOnMutations(unknownFlowObject, walk.waiter, mutationsEmptyingLocations);
    return !inputs.graph.findProperty(unknownFlowObject, own.key) && !inputs.records.possibleMutations(unknownFlowObject).containsAny(mutationsEmptyingLocations);
}

// Start-up makes a member immutable that a lookup took for a constant, so a store, a definition or a deletion changes it only where the holder itself is among the receivers.
// Such an operation creates an entry under the name or marks the member as written, and wakes whoever waits on the name or on the mutations of the holder.
bool FlowPropertyWalker::isConstantMember(const FlowHolder& holder, const FlowOwnProperty& own) const
{
    if (!own.builtin || own.value || own.getter || own.setter || !readsProperties(m_walk.operation))
        return false;
    if (!m_inputs.state.membersLockedWhileConstant.get(own.builtin.index()) || m_inputs.records.builtinUse(own.builtin).uses.contains(BuiltinUse::Written))
        return false;
    if (!m_inputs.isSolving())
        return m_inputs.state.membersTakenForConstants.get(own.builtin.index());
    if (!holder.object || !m_walk.waiter.instance)
        return false;
    m_inputs.solver->propertyState().takeForConstant(own.builtin);
    return true;
}

// The program resolves the name itself and only passes the holder on the way: a store makes or hits a property of the receiver, a read has found the name below, or a member of the table has it above.
// A read that finds the name nowhere says that somebody else supplies it: where code that the build has not seen can run, that code may add the name, and the read answers unknown.
bool FlowPropertyWalker::resolvesNameElsewhere(const FlowHolder& holder, PropertyKeyID key) const
{
    if (m_walk.operation == FlowWalkOperation::Set || m_hasFoundOnlyName)
        return true;
    const BuiltinTable& builtins = m_inputs.builtins();
    for (BuiltinID above = defaultPrototypeOf(builtins, holder.builtin).builtin; above; above = defaultPrototypeOf(builtins, above).builtin) {
        if (findOwnMember(m_inputs.program, above, key))
            return true;
    }
    return false;
}

// The run time refuses whoever defines a name that a lookup has passed as absent, so something comes under the name only where the holder itself is among the receivers.
// Such an operation is on record, and wakes whoever waits on the name or on the mutations of the holder. What the holder's locations hold without one has come through the unknown object, which is refused.
bool FlowPropertyWalker::isClosedAbsentName(const FlowHolder& holder, const FlowOwnProperty& own) const
{
    if (own.builtin || own.lookup == FlowOwnLookup::Unknown || reportsTargetsOn(holder) || !followsPrototypes(m_walk.operation))
        return false;
    if (!canCloseAbsentNames(m_inputs.builtins(), holder.builtin) || classOf(m_inputs.program, own.key) != FlowKeyClass::String)
        return false;

    BuiltinID canonicalHolder = m_inputs.builtins().canonicalID(holder.builtin);
    FlowAbsentName name { canonicalHolder, own.key };
    if (m_inputs.state.namesWrittenOnBuiltins.contains(name) || m_inputs.state.builtinsWrittenUnderUnnamedKeys.get(canonicalHolder.index()))
        return false;
    // Nothing fixes a link that the program changes itself, so unseen code changes it too.
    if (m_inputs.state.builtinsWithPrototypeChangedByProgram.get(canonicalHolder.index()))
        return false;
    if (!m_inputs.isSolving())
        return m_inputs.state.namesTakenForAbsent.contains(name);
    // Below an unknown source nothing is gained.
    if (m_hasReportedUnknown || !holder.object || !m_walk.waiter.instance || !isReachedByUnseenCode(holder))
        return false;
    if (m_inputs.records.unseenCodeCanRun() && !resolvesNameElsewhere(holder, own.key))
        return false;
    m_inputs.solver->propertyState().takeForAbsent(name);
    return true;
}

void FlowPropertyWalker::noteWriteUnderName(const FlowHolder& holder, PropertyKeyID key) const
{
    if (!holder.builtin || !m_inputs.isSolving())
        return;
    if (m_inputs.solver->propertyState().namesWrittenOnBuiltins.add({ m_inputs.builtins().canonicalID(holder.builtin), key }).isNewEntry)
        m_inputs.solver->revisitPropertyWaiters(holder.object, key);
}

bool FlowPropertyWalker::findsOnlyConstantMembers(const FlowHolder& holder) const
{
    if (!holder.builtin || !m_keys.hasOnlyNames())
        return false;
    return std::ranges::all_of(m_keys.names, [&](PropertyKeyID name) {
        if (mutabilityOf(m_inputs, holder, name) == FlowHolderMutability::Immutable)
            return true;
        FlowOwnProperty own = findOwnProperty(m_inputs, holder, name);
        own.key = name;
        return isConstantMember(holder, own) || isClosedAbsentName(holder, own);
    });
}

// The edge from the unknown object's location into such a location drops what the type excludes.
static bool filtersStoredValues(const FlowWalkInputs& inputs, FlowNodeID location)
{
    TypeID enforcedType = enforcedTypeOf(inputs.graph, location);
    return enforcedType && !inputs.program.at(enforcedType).isAnything();
}

void FlowPropertyWalker::reportOwnSources(const FlowHolder& holder, FlowCertainty certainty)
{
    for (PropertyKeyID name : m_keys.names)
        reportNamedKey(holder, name, certainty);
    if (m_keys.hasUnlistedNames())
        reportEveryNamedProperty(holder, certainty);
    if (m_keys.mayNameIndex())
        reportIndexes(holder, certainty);
    if (m_keys.anyString && !m_unknownSourceAnswersForHolder)
        reportUnnamedKeyPart(holder, FlowOpenPart::StringProperties, certainty);
    if (m_keys.anySymbol && !m_unknownSourceAnswersForHolder)
        reportUnnamedKeyPart(holder, FlowOpenPart::SymbolProperties, certainty);
    reportUndeclaredPart(holder);
}

void FlowPropertyWalker::reportNamedKey(const FlowHolder& holder, PropertyKeyID key, FlowCertainty certainty)
{
    FlowOwnProperty own = findOwnProperty(m_inputs, holder, key);
    own.key = key;
    bool isOrdinaryHolder = mutabilityOf(m_inputs, holder, key) == FlowHolderMutability::Ordinary;
    bool isShownByValue = own.lookup == FlowOwnLookup::Found && own.presence == FlowOwnPresence::ShownByValue;
    if (isReceiver(holder) && !(isShownByValue && (own.value || own.builtin)))
        m_receiverShowsEveryKey = false;

    if (reportsTargetsOn(holder) && isOrdinaryHolder) {
        noteWriteUnderName(holder, key);
        FlowNodeID target = own.value;
        if (!target && m_inputs.isSolving())
            target = ensureOwnValue(*m_inputs.solver, holder.object, key);
        if (target)
            m_functor(FlowPropertySource { FlowPropertySourceKind::Value, certainty, true, holder.object, key, target, own.builtin });
        for (FlowNodeID field : computedStaticFieldsOf(m_inputs, holder))
            reportNode(FlowPropertySourceKind::Value, holder, key, field, FlowCertainty::Predicted);
    }

    bool isClosedAbsent = m_holderShowsConstantMembers && isClosedAbsentName(holder, own);
    m_holderHasClosedAbsentName |= isClosedAbsent;
    if (own.lookup != FlowOwnLookup::Absent && m_keys.onlyName())
        m_hasFoundOnlyName = true;
    if (own.lookup == FlowOwnLookup::Unknown)
        reportUnknown(holder);
    if (own.lookup != FlowOwnLookup::Absent && !isClosedAbsent)
        reportProperty(holder, own, certainty);

    bool isConstant = isClosedAbsent || isConstantMember(holder, own);
    if (readsPrototypeChain(m_walk.operation) && holder.object && isOrdinaryHolder && !isConstant && !m_unknownSourceAnswersForHolder) {
        if (!own.value || m_inputs.records.possibleMutations(holder.object).contains(ObjectMutation::StoreOfOtherType)) {
            if (std::optional<FlowOpenPart> part = unnamedKeyPartOf(classOf(m_inputs.program, key)))
                reportUnnamedKeyPart(holder, *part, certainty);
        }
        // A static field under a computed key replaces a member of the same name, a method included.
        for (FlowNodeID field : computedStaticFieldsOf(m_inputs, holder))
            reportNode(FlowPropertySourceKind::Value, holder, PropertyKeyID(), field, FlowCertainty::Predicted);
    }

    if (isOrdinaryHolder && !isConstant && !m_unknownSourceAnswersForHolder)
        reportAliasOfNamedKey(holder, key, own, certainty);

    // Whatever comes under the name here later ends the lookup here.
    if (m_keys.onlyName() && readsPrototypeChain(m_walk.operation) && !m_walk.isOwnOnly && own.lookup == FlowOwnLookup::Absent && isOrdinaryHolder && !isClosedAbsent && !m_unknownSourceAnswersForHolder)
        m_passesHolderWithoutName = true;

    bool answersLookup = own.lookup == FlowOwnLookup::Found && !isClosedAbsent && (own.value || own.getter || own.builtin);
    if (m_keys.onlyName() && followsPrototypes(m_walk.operation) && answersLookup)
        m_lookupEndsAtHolder = isPrototypePropertyOfFunction(m_inputs, holder, key) || (isOrdinaryHolder ? isConstant || isProvenFilled(m_inputs, holder, own, certainty) || isAssumedFilled(m_inputs, holder, own, m_walk) || isUntouchedBuiltinMember(m_inputs, holder, own, m_walk) : isShownByValue);
}

void FlowPropertyWalker::reportEveryNamedProperty(const FlowHolder& holder, FlowCertainty certainty)
{
    // A definition under a key nobody can name goes to the node of those keys, which leads on to each location.
    if (m_walk.operation == FlowWalkOperation::DefineOwnProperty) {
        if (ownListingOf(m_inputs, holder) == FlowOwnListing::MayHaveOthers || mayHaveUnlistedAliases(m_inputs, holder, m_walk))
            reportUnknown(holder);
        return;
    }

    // Whoever defines a setter through the unknown object hands it to native code, which the unknown source of this holder stands for.
    // What a delete does to the escaped objects is on record at the unknown object and in its unnamed parts, for the names it has and for those it gets after this walk.
    bool isSetOrDelete = m_walk.operation == FlowWalkOperation::Set || m_walk.operation == FlowWalkOperation::Delete;
    if (m_inputs.isSolving() && holder.object == unknownFlowObject && isSetOrDelete)
        return;

    FlowOwnListing listing = forEachOwnProperty(m_inputs, holder, [&](const FlowOwnProperty& property) {
        if (!property.key) {
            reportProperty(holder, property, FlowCertainty::Predicted);
            return;
        }
        if (m_keys.names.contains(property.key) || !m_keys.mayName(property.key, m_inputs.program))
            return;
        reportProperty(holder, property, certainty);
    });

    bool namesOnlyNumbers = !m_keys.hasUnnamedKeys();
    if (namesOnlyNumbers && holder.builtin && m_inputs.builtins().closesIndexKeys(holder.builtin))
        return;
    if (listing == FlowOwnListing::MayHaveOthers || mayHaveUnlistedAliases(m_inputs, holder, m_walk))
        reportUnknown(holder);
}

void FlowPropertyWalker::reportIndexes(const FlowHolder& holder, FlowCertainty certainty)
{
    if (!holder.object)
        return;
    if (m_walk.operation == FlowWalkOperation::Set && !isReceiver(holder))
        return;
    if (!readsProperties(m_walk.operation) && mutabilityOfIndexes(m_inputs, holder) == FlowHolderMutability::Immutable)
        return;
    if (m_inputs.objects.kindOf(holder.object) == FlowObjectKind::ModuleNamespace) {
        reportIndexExports(holder, certainty);
        return;
    }

    uint64_t shown = 0;
    FlowIndexedStorage storage = forEachIndexedLocation(m_inputs, holder.object, m_keys.indexes, [&](const FlowIndexedLocation& location) {
        reportNode(FlowPropertySourceKind::Value, holder, location.key, location.node, certainty);
        if (standsForOneIndex(m_inputs, holder.object, location.node))
            ++shown;
    });

    bool showsEveryIndex = m_keys.indexes.minimum >= 0 && shown == static_cast<uint64_t>(m_keys.indexes.maximum - m_keys.indexes.minimum) + 1;
    if (isReceiver(holder) && !showsEveryIndex)
        m_receiverShowsEveryKey = false;

    if (storage == FlowIndexedStorage::OpenPart)
        reportUnnamedKeyPart(holder, FlowOpenPart::IndexedProperties, certainty);
}

void FlowPropertyWalker::reportIndexExports(const FlowHolder& holder, FlowCertainty certainty)
{
    FlowOwnListing listing = forEachOwnProperty(m_inputs, holder, [&](const FlowOwnProperty& property) {
        if (property.key && classOf(m_inputs.program, property.key) == FlowKeyClass::Index && m_keys.mayName(property.key, m_inputs.program))
            reportProperty(holder, property, certainty);
    });

    if (listing == FlowOwnListing::MayHaveOthers)
        reportUnknown(holder);
    if (isReceiver(holder))
        m_receiverShowsEveryKey = false;
}

void FlowPropertyWalker::reportUnnamedKeyPart(const FlowHolder& holder, FlowOpenPart part, FlowCertainty certainty)
{
    UNUSED_PARAM(certainty);
    if (!holder.object || !isOrdinary(m_inputs, holder))
        return;
    if (part == FlowOpenPart::IndexedProperties && mutabilityOfIndexes(m_inputs, holder) == FlowHolderMutability::Immutable)
        return;
    if (m_walk.operation == FlowWalkOperation::Set && !isReceiver(holder))
        return;

    FlowNodeID node = m_inputs.graph.find(holder.object, part);
    bool isFilledAtCreation = !node && part == FlowOpenPart::IndexedProperties && isCreatedWithCharacters(m_inputs, holder.object);
    if (!node && m_inputs.isSolving() && (isFilledAtCreation || readsPrototypeChain(m_walk.operation) || reportsTargetsOn(holder)))
        node = m_inputs.solver->ensureOpenPart(holder.object, part);
    // No statement has looked under the indexes of the holder while the solver ran.
    if (!node && isFilledAtCreation) {
        reportUnknown(holder);
        return;
    }
    if (canBeSource(m_inputs, node))
        reportNode(FlowPropertySourceKind::Value, holder, PropertyKeyID(), node, FlowCertainty::Predicted);
}

void FlowPropertyWalker::reportUndeclaredPart(const FlowHolder& holder)
{
    if (!holder.object)
        return;

    bool isReading = readsProperties(m_walk.operation);
    FlowNodeID node = m_inputs.graph.undeclaredPart(holder.object);
    if (node && isReading)
        reportNode(FlowPropertySourceKind::Value, holder, PropertyKeyID(), node, FlowCertainty::Predicted);
    if (m_inputs.objects.kindOf(holder.object) == FlowObjectKind::Opaque && (isReading || m_walk.operation == FlowWalkOperation::Set))
        reportUnknown(holder);
}

void FlowPropertyWalker::reportAliasOfNamedKey(const FlowHolder& holder, PropertyKeyID key, const FlowOwnProperty& own, FlowCertainty certainty)
{
    UNUSED_PARAM(certainty);
    if (!aliasesUnknownObject(m_inputs, holder) || classOf(m_inputs.program, key) == FlowKeyClass::PrivateName)
        return;
    const FlowProperty* entry = m_inputs.graph.findProperty(unknownFlowObject, key);
    if (!entry)
        return;

    FlowProperty alias = *entry;
    switch (m_walk.operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
        if ((!own.value || filtersStoredValues(m_inputs, own.value)) && canBeSource(m_inputs, alias.value))
            reportNode(FlowPropertySourceKind::Value, holder, key, alias.value, FlowCertainty::Predicted);
        if (canBeSource(m_inputs, alias.getter))
            reportNode(FlowPropertySourceKind::Getter, holder, key, alias.getter, FlowCertainty::Predicted);
        return;
    case FlowWalkOperation::Set:
        if (canBeSource(m_inputs, alias.setter))
            reportNode(FlowPropertySourceKind::Setter, holder, key, alias.setter, FlowCertainty::Predicted);
        return;
    case FlowWalkOperation::DefineOwnProperty:
    case FlowWalkOperation::Delete:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowPropertyWalker::reportProperty(const FlowHolder& holder, const FlowOwnProperty& own, FlowCertainty certainty)
{
    if (m_walk.isEnumerableOnly && own.enumerability == FlowOwnEnumerability::NeverEnumerable)
        return;

    switch (m_walk.operation) {
    case FlowWalkOperation::Get:
    case FlowWalkOperation::HasProperty:
    case FlowWalkOperation::CopyDataProperties:
        if (own.value)
            reportNode(FlowPropertySourceKind::Value, holder, own.key, own.value, certainty);
        if (own.getter)
            reportNode(FlowPropertySourceKind::Getter, holder, own.key, own.getter, certainty);
        if (own.builtin)
            reportBuiltinMember(holder, own, certainty);
        if (own.setter && !own.getter && !own.builtin) {
            m_chainMayEnd = true;
            m_receiverShowsEveryKey = false;
            m_certaintyOfAbsence = weakerOf(m_certaintyOfAbsence, certainty);
            notePassedMember(FlowPropertySourceKind::Setter, holder, own.key, own.setter, certainty);
        }
        return;
    case FlowWalkOperation::Set:
        if (own.setter)
            reportNode(FlowPropertySourceKind::Setter, holder, own.key, own.setter, certainty);
        else if (own.getter)
            notePassedMember(FlowPropertySourceKind::Getter, holder, own.key, own.getter, certainty);
        if (own.value && !isReceiver(holder))
            notePassedMember(FlowPropertySourceKind::Value, holder, own.key, own.value, certainty);
        if (own.builtin)
            reportBuiltinMember(holder, own, certainty);
        return;
    case FlowWalkOperation::DefineOwnProperty:
        return;
    case FlowWalkOperation::Delete:
        if (isOrdinary(m_inputs, holder)) {
            if (own.value)
                reportNode(FlowPropertySourceKind::Value, holder, own.key, own.value, certainty);
            if (own.getter)
                reportNode(FlowPropertySourceKind::Getter, holder, own.key, own.getter, certainty);
            if (own.setter)
                reportNode(FlowPropertySourceKind::Setter, holder, own.key, own.setter, certainty);
        }
        if (own.builtin)
            reportBuiltinMember(holder, own, certainty);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowPropertyWalker::reportBuiltinMember(const FlowHolder& holder, const FlowOwnProperty& own, FlowCertainty certainty, LookupPath path)
{
    RELEASE_ASSERT(own.builtin && m_walk.operation != FlowWalkOperation::DefineOwnProperty);

    const BuiltinTable& table = m_inputs.builtins();
    switch (m_walk.builtinMembers) {
    case FlowBuiltinMemberLookup::Every:
        break;
    case FlowBuiltinMemberLookup::ExceptConstructorLinks:
        if (holder.builtin && isConstructorLink(table.at(holder.builtin), table.at(own.builtin)))
            return;
        break;
    case FlowBuiltinMemberLookup::None:
        return;
    }

    bool isSet = m_walk.operation == FlowWalkOperation::Set;
    FlowPropertySource source { FlowPropertySourceKind::BuiltinMember, certainty, isReceiver(holder), holder.object, own.key, FlowNodeID(), own.builtin, path == LookupPath::UnknownObject };
    switch (table.at(own.builtin).kind) {
    case BuiltinKind::Method:
    case BuiltinKind::Function:
    case BuiltinKind::Constructor:
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::Constant:
        if (!isSet)
            m_functor(source);
        return;
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        if (isSet) {
            if (!table.dispatchOf(own.builtin, BuiltinCallMode::Set))
                return;
            source.kind = FlowPropertySourceKind::BuiltinSetter;
        } else if (m_walk.operation != FlowWalkOperation::Delete)
            source.kind = FlowPropertySourceKind::BuiltinGetter;
        m_functor(source);
        return;
    case BuiltinKind::NativeBinding:
        reportUnknown(holder);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasReachedNativeCode(const FlowWalkInputs& inputs, BuiltinID builtin)
{
    FlowObjectID object = inputs.objects.findBuiltin(builtin);
    return object && inputs.hasReachedNativeCode(object);
}

// The unknown object stands for instances of built-in classes, whoever created them. It inherits from their prototypes, and what it holds under the name constructor is their constructors.
// An instance of a class that trimming can remove exists only where flow sees the class in use, so that prototype counts only once it has reached native code, like any other built-in object.
static bool isFoundThroughUnknownObject(const FlowWalkInputs& inputs, BuiltinID holder)
{
    const BuiltinTable& table = inputs.builtins();
    if (hasReachedNativeCode(inputs, holder))
        return true;

    BuiltinID prototype = holder;
    if (table.at(holder).kind == BuiltinKind::Constructor)
        prototype = table.findOwnMember(holder, BuiltinNames::prototype);
    if (!prototype || table.at(prototype).kind != BuiltinKind::Prototype)
        return false;
    prototype = table.canonicalID(prototype);
    return table.featureUsesOf(prototype).empty() || hasReachedNativeCode(inputs, prototype);
}

// Native code calls a function that has escaped with unknown operands, unless the call would be an event.
bool isCalledByNativeCode(const FlowWalkInputs& inputs, BuiltinID member)
{
    BuiltinID object = inputs.builtins().canonicalID(member);
    return inputs.records.builtinUse(object).uses.contains(BuiltinUse::Escaped) && !letsInUnseenCode(inputs.builtins(), object);
}

static FlowMembersOfBuiltins collectMembersNamed(const FlowWalkInputs& inputs, PropertyKeyID key)
{
    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    FlowMembersOfBuiltins members;
    // A read through a receiver nobody knows names no function that starts unseen code and no object that holds what the loader answers: the run time refuses the call and the store.
    auto isNamedOnlyBySeenCode = [&](BuiltinID member) {
        return standsForWhatItHolds(table, member) || holdsLoaderAnswers(table.canonicalID(member));
    };
    for (BuiltinClassID builtinClass : table.classesWithInstanceMember(builtinMemberNameOf(program, key).name)) {
        BuiltinID prototype = table.canonicalID(table.at(builtinClass).prototype);
        BuiltinID member = findOwnMember(program, prototype, key);
        if (member && table.at(member).kind != BuiltinKind::Constructor && !isNamedOnlyBySeenCode(member))
            members.append(FlowMemberOfBuiltin { prototype, member });
    }

    BuiltinMemberName name = builtinMemberNameOf(program, key);
    auto nameIndexOf = [&](const FlowMemberOfBuiltin& found) {
        return table.at(found.member).name.index();
    };
    for (const FlowMemberOfBuiltin& found : std::ranges::equal_range(inputs.state.membersOutsidePrototypes, name.name.index(), { }, nameIndexOf)) {
        if ((!name.isPrivateAlias || table.at(found.member).flags.contains(BuiltinFlag::HasPrivateAlias)) && !isNamedOnlyBySeenCode(found.member))
            members.append(found);
    }
    return members;
}

// Unknown stands for what has reached native code, and the members of a built-in object do not follow it there.
// The constructor of a prototype is found only as a value of its own: Function is the constructor of every function, and it evaluates text.
void FlowPropertyWalker::reportMembersOfBuiltinObjects()
{
    if (std::exchange(m_hasReportedMembersOfBuiltinObjects, true) || m_walk.builtinMembers == FlowBuiltinMemberLookup::None)
        return;
    // Unseen JavaScript names the global object, and from there the members follow every prototype into native code.
    if (m_inputs.records.mayRunUnknownJavaScript())
        return;

    const BuiltinTable& table = m_inputs.builtins();
    for (PropertyKeyID key : m_keys.names) {
        if (!builtinMemberNameOf(m_inputs.program, key))
            continue;
        if (!m_inputs.isSolving()) {
            reportMembersFoundByName(key, collectMembersNamed(m_inputs, key).span());
            continue;
        }

        FlowMembersOfBuiltins& members = m_inputs.solver->propertyState().membersFoundByName.ensure(key, [&] {
            return collectMembersNamed(m_inputs, key);
        }).iterator->value;
        members.removeAllMatching([&](const FlowMemberOfBuiltin& found) {
            return isCalledByNativeCode(m_inputs, found.member) && !table.dispatchOf(found.member, BuiltinCallMode::Set);
        });
        reportMembersFoundByName(key, members.span());
    }
    if (m_keys.hasUnnamedKeys() && m_walk.operation == FlowWalkOperation::Set)
        reportSettersUnderUnnamedKeys();
    if (m_keys.hasUnnamedKeys() && m_walk.operation == FlowWalkOperation::Get)
        m_readsUnnamedKeysOfUnknownObject = true;
}

void FlowPropertyWalker::reportMembersFoundByName(PropertyKeyID key, std::span<const FlowMemberOfBuiltin> members)
{
    // No read of a property hands out the function behind an accessor, so only a store runs its setter.
    bool skipsMembersOfNativeCode = m_walk.operation != FlowWalkOperation::Set;
    bool waitsForHolder = false;
    for (const FlowMemberOfBuiltin& found : members) {
        if (skipsMembersOfNativeCode && isCalledByNativeCode(m_inputs, found.member))
            continue;
        if (!isFoundThroughUnknownObject(m_inputs, found.holder)) {
            waitsForHolder = true;
            continue;
        }
        FlowOwnProperty own;
        own.key = key;
        own.builtin = found.member;
        reportBuiltinMember(holderOf(m_inputs, found.holder), own, FlowCertainty::Predicted, LookupPath::UnknownObject);
    }
    if (waitsForHolder && m_inputs.isSolving())
        m_inputs.solver->waitOnProperty(unknownFlowObject, key, m_walk.waiter);
}

void FlowPropertyWalker::reportSettersUnderUnnamedKeys()
{
    for (const FlowMemberOfBuiltin& setter : m_inputs.state.settersChangingReceivers) {
        PropertyKeyID key = m_inputs.program.keyOf(m_inputs.builtins().at(setter.member).name);
        if (!m_keys.names.contains(key) && m_keys.mayName(key, m_inputs.program))
            reportMembersFoundByName(key, std::span(&setter, 1));
    }
}

void FlowPropertyWalker::reportNode(FlowPropertySourceKind kind, const FlowHolder& holder, PropertyKeyID key, FlowNodeID node, FlowCertainty certainty)
{
    m_functor(FlowPropertySource { kind, certainty, isReceiver(holder), holder.object, key, node, BuiltinID() });
}

// A read ends at an accessor without a getter, a store at one without a setter or at a data property above its receiver. The operation takes nothing from there, and goes another way while the member is missing.
void FlowPropertyWalker::notePassedMember(FlowPropertySourceKind kind, const FlowHolder& holder, PropertyKeyID key, FlowNodeID node, FlowCertainty certainty)
{
    m_passedMembers.append(FlowPropertySource { kind, certainty, isReceiver(holder), holder.object, key, node, BuiltinID() });
}

void FlowPropertyWalker::reportUnknown(const FlowHolder& holder)
{
    m_hasReportedUnknown = true;
    bool isRead = m_walk.operation == FlowWalkOperation::Get;
    if (isRead && m_inputs.isSolving() && holder.builtin && standsForWhatItHolds(m_inputs.builtins(), holder.builtin))
        m_functor(FlowPropertySource { FlowPropertySourceKind::BuiltinMember, FlowCertainty::Predicted, isReceiver(holder), holder.object, PropertyKeyID(), FlowNodeID(), holder.builtin });
    m_functor(FlowPropertySource { FlowPropertySourceKind::Unknown, FlowCertainty::Predicted, isReceiver(holder), holder.object, PropertyKeyID(), FlowNodeID(), BuiltinID() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
