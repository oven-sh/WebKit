/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"
#include "AOTValueFlowRulesModules.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

// The target finds what the source holds as it finds what it inherits.
void HeapRule::copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& source, CopiedProperties copiedProperties)
{
    bool storesThroughSetters = copiedProperties == CopiedProperties::AreAssigned;
    bool keepsAccessors = copiedProperties == CopiedProperties::AreDefinedAsTheyStand;
    ClimbingSet targets = m_context.valueOf(target);
    ClimbingSet sources = m_context.valueOf(source);
    ClimbingSet untrackedObject(m_universe.untrackedObjectAlone());
    ValueFlowName everyValue = foundName(unknownHeapKey, HeapPropertyPart::Value);
    ValueFlowName everyGetter = foundName(unknownHeapKey, HeapPropertyPart::Getter);
    ValueFlowName everySetter = foundName(unknownHeapKey, HeapPropertyPart::Setter);
    ValueFlowName valueUnderUnknownKey = writtenName(unknownHeapKey, HeapPropertyPart::Value);

    add(m_context.written(known(targets), copiedFromName), known(sources));
    notePrototypeObjectsListedAtWrite(target, targets, unknownHeapKey);
    // Whoever walks an array reads its elements out of the array, without a lookup. So what a source holds under an index is written into the target, with what was copied onto the source before. What the source inherits is not copied.
    connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), elementsName), m_context.written(known(targets), elementsName));

    ValueFlowNodeID copied = m_context.written(known(targets), valueUnderUnknownKey);
    Vector<ValueFlowNodeID, 3> nodesTakingCopies { copied };
    // No lookup goes on from a placeholder to what was copied onto it: what the sources hold is stored through it like any value.
    auto storeThrough = [&](ClimbingSet placeholder) {
        ValueFlowNodeID stored = m_context.written(placeholder, valueUnderUnknownKey);
        connect(m_context.read(known(sources), everyValue), stored);
        connect(storedThroughUnknownObjectInto(known(sources), everyValue), stored);
        connect(storedThroughUntrackedObjectInto(known(sources), everyValue), stored);
        if (keepsAccessors) {
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyGetter), m_context.written(placeholder, writtenName(unknownHeapKey, HeapPropertyPart::Getter)));
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everySetter), m_context.written(placeholder, writtenName(unknownHeapKey, HeapPropertyPart::Setter)));
        }
        nodesTakingCopies.append(stored);
        return stored;
    };
    // What is stored through the untracked object stays tracked.
    if (mayBeUntrackedObject(targets))
        storeThrough(untrackedObject);
    // What is stored through the unknown object is in unknown hands. The source is handed to nobody but its own getters. What was copied onto it before is found through the objects that it was copied from.
    if (mayBeUnknownObject(targets)) {
        noteStoreThroughUnknownObject(m_context, valueUnderUnknownKey);
        if (keepsAccessors)
            noteStoreThroughUnknownObject(m_context, writtenName(unknownHeapKey, HeapPropertyPart::Getter));
        connect(storeThrough(unknownObjectAlone()), ValueFlowProgramNodes::escaped());
        connect(m_context.read(known(sources), copiedFromName), ValueFlowProgramNodes::escaped());
        if (keepsAccessors) {
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyGetter), ValueFlowProgramNodes::escaped());
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everySetter), ValueFlowProgramNodes::escaped());
        }
        // Only an object that the program has allocated with a layout holds nothing but what was written into it. A built-in object has its members and a namespace object its exports: such a source is handed over whole.
        Members handedOver;
        m_context.forEachMember(known(sources), [&](ValueFlowObjectID object) {
            ValueFlowObjectKind objectKind = m_universe.kindOf(object);
            LayoutID layout = objectKind == ValueFlowObjectKind::Site || objectKind == ValueFlowObjectKind::Instance ? m_universe.layoutOf(object) : LayoutID();
            LayoutKind layoutKind = layout ? m_program.at(layout).kind : LayoutKind::ModuleNamespace;
            if (layoutKind != LayoutKind::Record && layoutKind != LayoutKind::Instance && layoutKind != LayoutKind::Container)
                handedOver.append(object);
        });
        add(ValueFlowProgramNodes::escaped(), m_context.maker().of(handedOver.span()));
    }

    // The table knows what a built-in object holds, and the lookup reads the table: the copy reads every member, and one that the host makes at its first read is made here.
    ClimbingSet hostObjects = m_context.unite(m_context.restrict(sources, m_tables.holdersOfMembersThatHostMakes()), hostObjectsListedBesideUnknownObject(source, sources));
    if (holdsAnything(hostObjects)) {
        ValueFlowNodeID members = m_context.lookup(hostObjects, everyValue);
        for (ValueFlowNodeID node : nodesTakingCopies)
            connect(members, node);
    }

    // What was defined on a source by the descriptors of another object has the accessors of that object.
    ClimbingSet getters = m_context.valueOf(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyGetter));
    // The untracked object may be any object of the program, so whatever lies on one of them under any name is copied.
    if (mayBeUntrackedObject(sources)) {
        for (ClimbingSet holders : { ClimbingSet(m_universe.knownObjects()), untrackedObject, unknownObjectAlone() }) {
            for (ValueFlowNodeID node : nodesTakingCopies)
                connect(m_context.read(holders, everyValue), node);
            getters = m_context.unite(getters, m_context.valueOf(m_context.read(holders, everyGetter)));
            if (keepsAccessors) {
                connect(m_context.read(holders, everyGetter), m_context.written(known(targets), writtenName(unknownHeapKey, HeapPropertyPart::Getter)));
                connect(m_context.read(holders, everySetter), m_context.written(known(targets), writtenName(unknownHeapKey, HeapPropertyPart::Setter)));
            }
        }
    }
    for (ValueFlowNodeID node : nodesTakingCopies) {
        if (mayBeUnknownObject(sources))
            add(node, Typed::unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        if (m_context.meet(sources, m_universe.strings()))
            add(node, m_universe.anyPrimitive());
        if (!keepsAccessors)
            callAccessors(getters, ValueFlowCallKind::Get, { &source, { } }, nullptr, node);
    }
    // An accessor nobody knows is handed the object it lies on sooner or later. The function behind an accessor of the table is one.
    if (keepsAccessors && (mayBeUnknownObject(sources) || holdsAnything(hostObjects) || m_context.meet(sources, m_universe.builtinObjects())))
        connect(target, ValueFlowProgramNodes::escaped());

    // A setter takes what the source itself holds, or what a getter of the source has answered.
    if (storesThroughSetters) {
        convertStoredValue(targets, unknownHeapKey, operandOf(m_context.read(known(sources), everyValue)));
        convertStoredValue(targets, unknownHeapKey, operandOf(copied));
        // As for a store: what is found through a placeholder among the targets says nothing about the targets beside it.
        forEachStartOfLookup(targets, [&](ClimbingSet start) {
            callSettersOfReceivers(start, unknownHeapKey, operandOf(m_context.read(known(sources), everyValue)));
            callSettersOfReceivers(start, unknownHeapKey, operandOf(m_context.read(known(targets), valueUnderUnknownKey)));
        });
    }

    noteWrittenMembers(targets, unknownHeapKey);
    if (!storesThroughSetters)
        noteMembersLookedForBeforeChange(targets, unknownHeapKey);
    noteTrapOfUnknownObject(sources, ValueFlowCallKind::Get);
    noteTrapOfUnknownObject(targets, ValueFlowCallKind::Set);
    if (storesThroughSetters)
        markStore(targets, unknownHeapKey, { });
    else
        markDefinition(targets, unknownHeapKey, HeapPropertyPart::Value);
    // A property comes with the attributes that it has on the source.
    if (keepsAccessors)
        markRedefinition(targets, unknownHeapKey);
    mayThrow();
}

// The members of a descriptor are read like any property, and each may be there.
void HeapRule::defineAsDescribed(const ValueFlowOperand& object, const HeapKey& key, ClimbingSet descriptors)
{
    auto define = [&](HeapPropertyPart part, BuiltinName member) {
        ValueFlowNodeID described = m_context.lookup(descriptors, ValueFlowName::property(m_program.keyOf(member)));
        // A descriptor without the member, or with undefined there, defines no function.
        if (part != HeapPropertyPart::Value && !m_context.meet(m_context.valueOf(described), ValueSets::complement(m_universe.undefinedValue())))
            return;
        this->define(object, key, part, operandOf(described));
    };
    define(HeapPropertyPart::Value, BuiltinNames::value);
    define(HeapPropertyPart::Getter, BuiltinNames::get);
    define(HeapPropertyPart::Setter, BuiltinNames::set);
    // An attribute that the descriptor leaves out is false.
    markRedefinition(m_context.valueOf(object), key);
}

void HeapRule::changePrototype(ClimbingSet objects, const ValueFlowOperand& prototype)
{
    write(objects, prototypeLinkName, prototype);
    m_context.mark(m_context.restrict(objects, m_universe.objects()), ObjectMutation::ChangePrototype);
    m_context.mark(m_context.restrict(m_context.valueOf(prototype), m_universe.objects()), ObjectMutation::BecomePrototype);
}

// The function behind an accessor of the table is an object that nobody follows.
bool HeapRule::holdsOnlyWhatWasWritten(ClimbingSet receivers)
{
    return !m_context.meet(receivers, m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_tables.objectsWithoutPropertiesOfNativeCode())));
}

void HeapRule::readAccessorFunctions(const ValueFlowOperand& object, const HeapKey& key, HeapPropertyPart part, PropertySearch search)
{
    ClimbingSet receivers = m_context.valueOf(object);
    bool mayNameCallee = (key.keyClass == HeapKeyClass::Unknown && !key.namesEnumerableProperty) || (key.keyClass == HeapKeyClass::Name && key.name == m_tables.calleeKey());
    ClimbingSet searched = search == PropertySearch::OwnProperty ? receivers : ClimbingSet(m_universe.knownObjects());
    bool mayFindAccessorOfArguments = mayNameCallee && m_context.meet(searched, m_tables.argumentsObjects());
    auto addFunctionNobodyFollows = [&] {
        add(m_statement.result, m_context.restrict(ClimbingSet(Typed::unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult)), m_universe.objects()));
    };
    if (search == PropertySearch::OwnProperty && holdsOnlyWhatWasWritten(receivers) && holdsOnlyWhatWasWritten(withSourcesOfCopies(m_context, known(receivers)))) {
        readOwn(withSourcesOfCopies(m_context, known(receivers)), foundName(key, part), m_statement.result);
        if (mayFindAccessorOfArguments)
            addFunctionNobodyFollows();
        add(m_statement.result, m_universe.undefinedValue());
        return;
    }
    ValueFlowNodeID found = m_context.lookup(receivers, foundName(key, part));
    connect(m_context.masked(found, m_universe.callables()), m_statement.result);
    bool mayFindAccessorOfTable = m_context.meet(m_context.valueOf(found), m_universe.builtinObjects());
    if (mayFindAccessorOfArguments || mayFindAccessorOfTable)
        addFunctionNobodyFollows();
    // The getter of `__proto__` climbs.
    bool mayNamePrototypeLink = (key.keyClass == HeapKeyClass::Unknown && !key.namesEnumerableProperty) || (key.keyClass == HeapKeyClass::Name && key.name == m_context.program().keyOf(BuiltinNames::__proto__));
    if (mayFindAccessorOfTable && part == HeapPropertyPart::Getter && mayNamePrototypeLink)
        connect(m_context.nodes().ofHost(ValueFlowHostNode::ObjectsAboveEscapedInstances), ValueFlowProgramNodes::escaped());
    add(m_statement.result, m_universe.undefinedValue());
}

void HeapRule::runPropertyAction(const BuiltinPropertyAction& action, const ValueFlowPropertyActionOperands& operands)
{
    const ValueFlowOperand& object = operands.object;
    const ValueFlowOperand& value = operands.value;
    auto forEachKey = [&](NOESCAPE const Invocable<void(const HeapKey&)> auto& functor, const ValueFlowOperand* stored = nullptr) {
        forEachKeyOfAction(action.key, operands.key, functor, { &object, stored });
    };

    // A field of the result is a part of a descriptor, which describes an own property and calls no getter. __lookupGetter__ answers the function itself, from wherever on the chain it lies.
    PropertySearch search = action.value.kind == BuiltinPlaceKind::ResultField ? PropertySearch::OwnProperty : PropertySearch::WholeChain;

    switch (action.kind) {
    case BuiltinPropertyActionKind::ReadsByKey:
        forEachKey([&](const HeapKey& key) {
            if (search == PropertySearch::WholeChain || !holdsOnlyWhatWasWritten(m_context.valueOf(object))) {
                read(object, key);
                return;
            }
            // What a copy has taken from its sources lies with the sources.
            ClimbingSet holders = withSourcesOfCopies(m_context, known(m_context.valueOf(object)));
            if (!holdsOnlyWhatWasWritten(holders)) {
                read(object, key);
                return;
            }
            readOwn(holders, foundName(key, HeapPropertyPart::Value), m_statement.result);
            // Nobody wrote `prototype` into a function or `constructor` into its prototype object, and both are their own.
            readPropertiesAtCreation(holders, key, m_statement.result);
            handOutPrototypeObjectsAtCreation(holders, key);
            auto mayName = [&](PropertyKeyID name) {
                return (key.keyClass == HeapKeyClass::Unknown && !key.namesEnumerableProperty) || (key.keyClass == HeapKeyClass::Name && key.name == name);
            };
            if (mayName(m_tables.lengthKey()) && m_context.meet(holders, m_context.maker().unite(m_tables.arraysThatOperationsAllocate(), m_universe.closures())))
                add(m_statement.result, m_tables.numberKinds());
            if (mayName(m_tables.nameKey()) && m_context.meet(holders, m_universe.closures()))
                add(m_statement.result, m_universe.strings());
            if (m_context.meet(m_context.valueOf(object), m_universe.strings())) {
                if (key.keyClass != HeapKeyClass::Name)
                    add(m_statement.result, m_universe.strings());
                if (mayName(m_tables.lengthKey()))
                    add(m_statement.result, m_tables.numberKinds());
            }
            add(m_statement.result, m_universe.undefinedValue());
        });
        return;
    case BuiltinPropertyActionKind::StoresByKey:
        forEachKey([&](const HeapKey& key) {
            store(object, key, value);
        }, &value);
        return;
    case BuiltinPropertyActionKind::DefinesName:
        // The table does not say which attributes native code gives the property.
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Value, value);
            markRedefinition(m_context.valueOf(object), key);
        }, &value);
        return;
    case BuiltinPropertyActionKind::DefinesGetter:
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Getter, value);
        }, &value);
        return;
    case BuiltinPropertyActionKind::DefinesSetter:
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Setter, value);
        }, &value);
        return;
    case BuiltinPropertyActionKind::DeletesByKey:
        forEachKey([&](const HeapKey& key) {
            remove(object, key);
        });
        return;
    case BuiltinPropertyActionKind::TestsByKey:
        forEachKey([&](const HeapKey& key) {
            test(object, key);
        });
        return;
    case BuiltinPropertyActionKind::ReadsGetter:
        forEachKey([&](const HeapKey& key) {
            readAccessorFunctions(object, key, HeapPropertyPart::Getter, search);
        });
        return;
    case BuiltinPropertyActionKind::ReadsSetter:
        forEachKey([&](const HeapKey& key) {
            readAccessorFunctions(object, key, HeapPropertyPart::Setter, search);
        });
        return;
    case BuiltinPropertyActionKind::AssignsFrom:
        if (!operands.namesInMap) {
            copyProperties(object, value, CopiedProperties::AreAssigned);
            return;
        }
        for (PropertyKeyID name : *operands.namesInMap) {
            HeapKey key = keyNamed(name);
            store(object, key, operandOf(m_context.lookup(m_context.valueOf(value), foundName(key, HeapPropertyPart::Value))));
        }
        return;
    case BuiltinPropertyActionKind::DefinesFrom:
        copyProperties(object, value, CopiedProperties::AreDefinedAsTheyStand);
        return;
    case BuiltinPropertyActionKind::TakesDescriptor:
        forEachKey([&](const HeapKey& key) {
            defineAsDescribed(object, key, m_context.valueOf(value));
        }, &value);
        return;
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    // Whoever applies it hands over the result as the object and the map as the value.
    case BuiltinPropertyActionKind::DescribesResult:
        if (!operands.namesInMap) {
            defineAsDescribed(object, unknownHeapKey, m_context.valueOf(m_context.lookup(m_context.valueOf(value), foundName(unknownHeapKey, HeapPropertyPart::Value))));
            return;
        }
        for (PropertyKeyID name : *operands.namesInMap) {
            HeapKey key = keyNamed(name);
            defineAsDescribed(object, key, m_context.valueOf(m_context.lookup(m_context.valueOf(value), foundName(key, HeapPropertyPart::Value))));
        }
        if (operands.mapHasComputedNames)
            defineAsDescribed(object, unknownHeapKey, m_context.valueOf(m_context.lookup(m_context.valueOf(value), foundName(unknownHeapKey, HeapPropertyPart::Value))));
        return;
    case BuiltinPropertyActionKind::ChangesIntegrity:
        m_context.mark(m_context.restrict(m_context.valueOf(object), m_universe.objects()), ObjectMutation::ChangeIntegrity);
        // Freezing and sealing define every own property anew, and a definition looks for the property first.
        noteMembersLookedForBeforeChange(m_context.valueOf(object), unknownHeapKey);
        return;
    case BuiltinPropertyActionKind::ChangesPrototype:
        changePrototype(m_context.valueOf(object), value);
        return;
    case BuiltinPropertyActionKind::BecomesPrototype:
        if (isPassed(value))
            changePrototype(m_context.valueOf(value), object);
        else {
            connect(object, ValueFlowProgramNodes::escaped());
            m_context.mark(m_context.restrict(m_context.valueOf(object), m_universe.objects()), ObjectMutation::BecomePrototype);
        }
        return;
    case BuiltinPropertyActionKind::DescribesOwnProperties:
        // Nobody follows the descriptors: what they hold is in unknown hands, and so is what is read from them.
        connect(object, ValueFlowProgramNodes::escaped());
        if (operands.target)
            add(m_context.written(known(m_context.valueOf(operands.target)), writtenName(unknownHeapKey, HeapPropertyPart::Value)), m_context.restrict(ClimbingSet(Typed::unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject)), m_universe.objects()));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void applyPropertyAction(ValueFlowRuleContext& context, const BuiltinPropertyAction& action, const ValueFlowPropertyActionOperands& operands)
{
    ValueFlowStatement statement;
    statement.result = action.kind == BuiltinPropertyActionKind::DescribesOwnProperties ? ValueFlowNodeID() : operands.target;
    statement.thrown = operands.thrown;
    HeapRule(statement, context).runPropertyAction(action, operands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
