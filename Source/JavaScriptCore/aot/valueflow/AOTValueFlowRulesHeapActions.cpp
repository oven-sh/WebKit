/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

void HeapRule::copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& source, CopiedProperties copiedProperties)
{
    bool storesThroughSetters = copiedProperties == CopiedProperties::AreAssigned;
    bool keepsAccessors = copiedProperties == CopiedProperties::AreDefinedAsTheyStand;
    ClimbingSet targets = m_context.valueOf(target);
    ClimbingSet sources = m_context.valueOf(source);
    ValueFlowName everyValue = foundName(unknownHeapKey, HeapPropertyPart::Value);
    ValueFlowName everyGetter = foundName(unknownHeapKey, HeapPropertyPart::Getter);
    ValueFlowName everySetter = foundName(unknownHeapKey, HeapPropertyPart::Setter);
    ValueFlowName valueUnderUnknownKey = writtenName(unknownHeapKey, HeapPropertyPart::Value);

    if (storesThroughSetters)
        replaceMembersThroughSettersFoundFrom(targets, unknownHeapKey, PropertySearch::WholeChain, nullptr, true);
    add(m_context.written(known(targets), copiedFromName), m_context.unite(known(sources), findListedBun(source, sources)));
    if (holdsNamedBun(source))
        add(m_context.written(known(targets), copiedFromName), only(ValueFlowUniverse::markOfNamedBun()));
    if (mayBeUnknownObject(sources))
        add(m_context.nodes().ofHost(ValueFlowHostNode::CopiesOfObjectsNobodyKnows), known(targets));
    notePrototypeObjectsListedAtWrite(target, targets, unknownHeapKey);
    noteClosuresWrittenUnder(unknownHeapKey, targets);
    connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), elementsName), m_context.written(known(targets), elementsName));

    ValueFlowNodeID copied = m_context.written(known(targets), valueUnderUnknownKey);
    Vector<ValueFlowNodeID, 3> nodesTakingCopies { copied };
    auto storeThrough = [&](ClimbingSet placeholder) {
        ValueFlowNodeID stored = m_context.written(placeholder, valueUnderUnknownKey);
        ClimbingSet holders = withSourcesOfCopies(m_context, known(sources));
        connect(m_context.read(holders, everyValue), stored);
        connect(storedThroughUnknownObjectInto(holders, everyValue), stored);
        connect(storedThroughUntrackedObjectInto(holders, everyValue), stored);
        if (keepsAccessors) {
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyGetter), m_context.written(placeholder, writtenName(unknownHeapKey, HeapPropertyPart::Getter)));
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everySetter), m_context.written(placeholder, writtenName(unknownHeapKey, HeapPropertyPart::Setter)));
        }
        nodesTakingCopies.append(stored);
        return stored;
    };
    if (mayBeUntrackedObject(targets))
        storeThrough(untrackedObjectAlone());
    if (mayBeUnknownObject(targets)) {
        connect(storeThrough(unknownObjectAlone()), ValueFlowProgramNodes::escaped());
        connect(m_context.read(known(sources), copiedFromName), ValueFlowProgramNodes::escaped());
        if (keepsAccessors) {
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyGetter), ValueFlowProgramNodes::escaped());
            connect(m_context.read(withSourcesOfCopies(m_context, known(sources)), everySetter), ValueFlowProgramNodes::escaped());
        }
        add(ValueFlowProgramNodes::escaped(), m_context.restrict(known(sources), ValueSets::complement(m_tables.objectsHoldingOnlyWhatWasWritten())));
    }
    if (hasEscaped(known(targets))) {
        ClimbingSet all = withSourcesOfCopies(m_context, known(sources));
        ClimbingSet read = m_context.restrict(all, m_tables.objectsHoldingOnlyWhatWasWritten());
        add(ValueFlowProgramNodes::escaped(), m_context.restrict(all, ValueSets::complement(m_tables.objectsHoldingOnlyWhatWasWritten())));
        connect(m_context.read(read, everyValue), ValueFlowProgramNodes::escaped());
        connect(storedThroughUnknownObjectInto(read, everyValue), ValueFlowProgramNodes::escaped());
        connect(storedThroughUntrackedObjectInto(read, everyValue), ValueFlowProgramNodes::escaped());
        if (keepsAccessors) {
            connect(m_context.read(read, everyGetter), ValueFlowProgramNodes::escaped());
            connect(m_context.read(read, everySetter), ValueFlowProgramNodes::escaped());
        }
    }

    ClimbingSet hostObjects = m_context.unite(m_context.restrict(sources, m_tables.holdersOfMembersThatHostMakes()), hostObjectsListedBesideUnknownObject(source, sources));
    if (holdsAnything(hostObjects)) {
        ValueFlowNodeID members = m_context.lookup(hostObjects, everyValue);
        for (ValueFlowNodeID node : nodesTakingCopies)
            connect(members, node);
    }
    ClimbingSet otherHolders = m_context.restrict(sources, m_context.maker().intersect(m_tables.holdersWithMembers(), ValueSets::complement(m_tables.holdersOfMembersThatHostMakes())));
    if (holdsAnything(otherHolders)) {
        ValueFlowNodeID members = m_context.lookup(otherHolders, nameOfLookupForValue(enumeratedHeapKey));
        for (ValueFlowNodeID node : nodesTakingCopies)
            connect(members, node);
    }
    if (ValueFlowNodeID exports = exportsOfHoldersListedBesideUnknownObject(source)) {
        for (ValueFlowNodeID node : nodesTakingCopies)
            connect(exports, node);
    }

    ClimbingSet getters = m_context.valueOf(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyGetter));
    if (mayBeUntrackedObject(sources)) {
        for (ClimbingSet holders : { ClimbingSet(m_universe.knownObjects()), untrackedObjectAlone(), unknownObjectAlone() }) {
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
    if (keepsAccessors && (mayBeUnknownObject(sources) || holdsAnything(hostObjects) || m_context.meet(sources, m_universe.builtinObjects())))
        connect(target, ValueFlowProgramNodes::escaped());

    if (storesThroughSetters) {
        ValueFlowOperand held = operandOf(m_context.read(withSourcesOfCopies(m_context, known(sources)), everyValue));
        convertStoredValue(targets, unknownHeapKey, held);
        convertStoredValue(targets, unknownHeapKey, operandOf(copied));
        forEachStartOfLookup(targets, [&](ClimbingSet start) {
            callSettersOfReceivers(start, unknownHeapKey, held);
            callSettersOfReceivers(start, unknownHeapKey, operandOf(m_context.read(known(targets), valueUnderUnknownKey)));
        });
    }

    noteWrittenMembers(targets, unknownHeapKey);
    if (!storesThroughSetters)
        noteMembersLookedForBeforeChange(targets, unknownHeapKey);
    if (storesThroughSetters)
        markStore(targets, unknownHeapKey);
    else
        markDefinition(targets, unknownHeapKey, HeapPropertyPart::Value);
    if (keepsAccessors)
        markRedefinition(targets, unknownHeapKey);
    mayThrow();
}

void HeapRule::defineAsDescribed(const ValueFlowOperand& object, const HeapKey& key, ClimbingSet descriptors)
{
    auto define = [&](HeapPropertyPart part, BuiltinName member) {
        ValueFlowNodeID described = m_context.lookup(descriptors, ValueFlowName::property(m_program.keyOf(member)));
        if (part != HeapPropertyPart::Value && !m_context.meet(m_context.valueOf(described), ValueSets::complement(m_universe.undefinedValue())))
            return;
        this->define(object, key, part, operandOf(described));
    };
    define(HeapPropertyPart::Value, BuiltinNames::value);
    define(HeapPropertyPart::Getter, BuiltinNames::get);
    define(HeapPropertyPart::Setter, BuiltinNames::set);
    markRedefinition(m_context.valueOf(object), key);
}

void HeapRule::changePrototype(ClimbingSet objects, const ValueFlowOperand& prototype)
{
    write(objects, prototypeLinkName, prototype);
    m_context.mark(m_context.restrict(objects, m_universe.objects()), ObjectMutation::ChangePrototype);
    m_context.mark(m_context.restrict(m_context.valueOf(prototype), m_universe.objects()), ObjectMutation::BecomePrototype);
}

void HeapRule::readAccessorFunctions(const ValueFlowOperand& object, const HeapKey& key, HeapPropertyPart part, PropertySearch search)
{
    ClimbingSet receivers = m_context.valueOf(object);
    ClimbingSet searched = search == PropertySearch::OwnProperty ? receivers : ClimbingSet(m_universe.knownObjects());
    bool mayFindAccessorOfArguments = key.mayNameNonEnumerable(m_tables.calleeKey()) && m_context.meet(searched, m_tables.argumentsObjects());
    auto addFunctionNobodyFollows = [&] {
        add(m_statement.result, m_context.restrict(ClimbingSet(Typed::unknownValue(m_context, ValueFlowUnknownSource::UndescribedNativeResult)), m_universe.objects()));
    };
    // Whoever has the setter in hand calls it with any value.
    if (replaceMembersThroughSettersFoundFrom(receivers, key, search, nullptr, part == HeapPropertyPart::Setter))
        addFunctionNobodyFollows();
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
    if (mayFindAccessorOfTable && part == HeapPropertyPart::Getter && key.mayNameNonEnumerable(m_program.keyOf(BuiltinNames::__proto__)))
        connect(m_context.nodes().ofHost(ValueFlowHostNode::ObjectsAboveEscapedInstances), ValueFlowProgramNodes::escaped());
    add(m_statement.result, m_universe.undefinedValue());
}

void HeapRule::runPropertyAction(const BuiltinPropertyAction& action, const ValueFlowPropertyActionOperands& operands)
{
    const ValueFlowOperand& object = operands.object;
    const ValueFlowOperand& value = operands.value;
    auto forEachKey = [&](NOESCAPE const Invocable<void(const HeapKey&)> auto& functor, const ValueFlowOperand* stored = nullptr) {
        forEachKeyOfAction(action.key, operands.key, functor, { &object, stored, false, action.kind == BuiltinPropertyActionKind::ReadsByKey });
    };

    PropertySearch search = action.value.kind == BuiltinPlaceKind::ResultField ? PropertySearch::OwnProperty : PropertySearch::WholeChain;

    switch (action.kind) {
    case BuiltinPropertyActionKind::ReadsByKey:
        forEachKey([&](const HeapKey& key) {
            if (search == PropertySearch::WholeChain || !holdsOnlyWhatWasWritten(m_context.valueOf(object))) {
                read(object, key);
                return;
            }
            ClimbingSet holders = withSourcesOfCopies(m_context, known(m_context.valueOf(object)));
            if (!holdsOnlyWhatWasWritten(holders)) {
                read(object, key);
                return;
            }
            readOwn(holders, foundName(key, HeapPropertyPart::Value), m_statement.result);
            readPropertiesAtCreation(holders, key, m_statement.result);
            handOutPrototypeObjectsAtCreation(holders, key);
            if (key.mayNameNonEnumerable(m_tables.lengthKey()) && m_context.meet(holders, m_context.maker().unite(m_tables.arraysThatOperationsAllocate(), m_universe.closures())))
                add(m_statement.result, m_tables.numberKinds());
            if (key.mayNameNonEnumerable(m_tables.nameKey()) && m_context.meet(holders, m_universe.closures()))
                add(m_statement.result, m_universe.strings());
            if (m_context.meet(m_context.valueOf(object), m_universe.strings())) {
                if (key.keyClass != HeapKeyClass::Name)
                    add(m_statement.result, m_universe.strings());
                if (key.mayNameNonEnumerable(m_tables.lengthKey()))
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
        forEachKey([&](const HeapKey& key) {
            define(object, key, HeapPropertyPart::Value, value);
            markRedefinition(m_context.valueOf(object), key);
        }, &value);
        return;
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        forEachKey([&](const HeapKey& key) {
            define(object, key, action.kind == BuiltinPropertyActionKind::DefinesGetter ? HeapPropertyPart::Getter : HeapPropertyPart::Setter, value);
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
    case BuiltinPropertyActionKind::ReadsSetter:
        forEachKey([&](const HeapKey& key) {
            readAccessorFunctions(object, key, action.kind == BuiltinPropertyActionKind::ReadsGetter ? HeapPropertyPart::Getter : HeapPropertyPart::Setter, search);
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
    case BuiltinPropertyActionKind::DescribesResult: {
        auto defineUnder = [&](const HeapKey& key) {
            defineAsDescribed(object, key, m_context.valueOf(m_context.lookup(m_context.valueOf(value), foundName(key, HeapPropertyPart::Value))));
        };
        for (PropertyKeyID name : operands.namesInMap.value_or(std::span<const PropertyKeyID>()))
            defineUnder(keyNamed(name));
        if (!operands.namesInMap || operands.mapHasComputedNames)
            defineUnder(unknownHeapKey);
        return;
    }
    case BuiltinPropertyActionKind::ChangesIntegrity:
        m_context.mark(m_context.restrict(m_context.valueOf(object), m_universe.objects()), ObjectMutation::ChangeIntegrity);
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
