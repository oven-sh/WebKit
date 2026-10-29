/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

// The `callee` accessor of strict arguments is not enumerable. Any other native accessor on an arguments object was copied there, and a copy is written under the unknown key.
ClimbingSet HeapRule::gettersOfEnumerableProperties(ClimbingSet holders)
{
    ValueFlowName everyGetter = foundName(unknownHeapKey, HeapPropertyPart::Getter);
    ClimbingSet argumentsObjects = m_context.restrict(holders, m_tables.argumentsObjects());
    if (!holdsAnything(argumentsObjects) || m_context.contains(m_context.valueOf(m_context.readWhatWasWritten(argumentsObjects, writtenName(unknownHeapKey, HeapPropertyPart::Getter))), m_tables.undescribedNativeCode()))
        return m_context.valueOf(m_context.readWhatWasWritten(holders, everyGetter));
    ClimbingSet gettersOfArguments = m_context.restrict(m_context.valueOf(m_context.readWhatWasWritten(argumentsObjects, everyGetter)), ValueSets::complement(only(m_tables.undescribedNativeCode())));
    ClimbingSet others = m_context.restrict(holders, ValueSets::complement(m_tables.argumentsObjects()));
    return m_context.unite(gettersOfArguments, m_context.valueOf(m_context.readWhatWasWritten(others, everyGetter)));
}

void HeapRule::copyProperties(const ValueFlowOperand& target, const ValueFlowOperand& everySource, CopiedProperties copiedProperties)
{
    bool storesThroughSetters = copiedProperties == CopiedProperties::AreAssigned;
    bool keepsAccessors = copiedProperties == CopiedProperties::AreDefinedAsTheyStand;
    // A proxy is listed by its traps, and what it answers comes under keys that nobody lists.
    ValueFlowOperand source = keepsAccessors ? everySource : withoutProxiesAskedByTheirTraps(everySource);
    ClimbingSet proxies = std::exchange(m_proxiesAskedByTheirTraps, { });
    ClimbingSet targets = m_context.valueOf(target);
    ClimbingSet sources = m_context.unite(m_context.valueOf(source), targetsListedInPlaceOf(proxies));
    ValueFlowName everyValue = foundName(unknownHeapKey, HeapPropertyPart::Value);
    ValueFlowName everyGetter = foundName(unknownHeapKey, HeapPropertyPart::Getter);
    ValueFlowName everySetter = foundName(unknownHeapKey, HeapPropertyPart::Setter);
    ValueFlowName valueUnderUnknownKey = writtenName(unknownHeapKey, HeapPropertyPart::Value);

    // The copy stays a copy. Of the setters above the target it runs those under the keys of the sources.
    ClimbingSet sourcesWithKeys = objectsWithOwnKeysAmong(m_context, sources);
    std::optional<ValueSetID> keysOfNewSources = storesThroughSetters && !holdsAnything(proxies) ? keysThatObjectsWereMadeWith(m_context, sourcesWithKeys, OwnKeys::All) : std::nullopt;
    auto forEachKeyOfSource = [&](NOESCAPE const Invocable<void(const HeapKey&)> auto& functor) {
        if (!keysOfNewSources)
            return functor(unknownHeapKey);
        forEachKey(m_context.operandHolding(m_context.unite(ClimbingSet(*keysOfNewSources), m_context.valueOf(m_context.read(sourcesWithKeys, addedNamesName)))), functor);
    };
    if (storesThroughSetters) {
        forEachKeyOfSource([&](const HeapKey& key) {
            replaceMembersThroughSettersFoundFrom(targets, key, PropertySearch::WholeChain, nullptr, true);
        });
    }
    add(m_context.written(known(targets), copiedFromName), m_context.unite(known(sources), findListedBun(source, sources)));
    if (holdsNamedBun(source))
        add(m_context.written(known(targets), copiedFromName), only(ValueFlowUniverse::markOfNamedBun()));
    if (mayBeUnknownObject(sources)) {
        add(m_context.nodes().ofHost(ValueFlowHostNode::CopiesOfObjectsNobodyKnows), known(targets));
        runGettersOnObjectsNobodyKnows(m_context);
    }
    notePrototypeObjectsThatWriteMayReach(target, targets, unknownHeapKey);
    noteClosuresThatMayBeRenamed(target, unknownHeapKey, targets);
    // No value changes while a rule runs.
    ClimbingSet allSources = withSourcesOfCopies(m_context, known(sources));
    connect(m_context.readWhatWasWritten(allSources, elementsName), m_context.written(known(targets), elementsName));
    if (keepsAccessors) {
        for (ValueFlowName name : { everyValue, everyGetter, everySetter })
            compareWithPropertyThatIsThere(operandOf(m_context.readWhatWasWritten(allSources, name)));
    }

    ValueFlowNodeID copied = m_context.written(known(targets), valueUnderUnknownKey);
    Vector<ValueFlowNodeID, 3> nodesTakingCopies { copied };
    auto storeThrough = [&](ClimbingSet placeholder, ValueFlowHostNode sourcesOfCopies) {
        ValueFlowNodeID stored = m_context.written(placeholder, valueUnderUnknownKey);
        ClimbingSet holders = allSources;
        add(m_context.nodes().ofHost(sourcesOfCopies), holders);
        connect(m_context.readWhatWasWritten(holders, everyValue), stored);
        connect(enumerablePropertiesNobodyWrote(m_context, holders, MetBy::Program), stored);
        connect(storedThroughUnknownObjectInto(holders, everyValue), stored);
        connect(storedThroughUntrackedObjectInto(holders, everyValue), stored);
        if (keepsAccessors) {
            connect(m_context.readWhatWasWritten(allSources, everyGetter), m_context.written(placeholder, writtenName(unknownHeapKey, HeapPropertyPart::Getter)));
            connect(m_context.readWhatWasWritten(allSources, everySetter), m_context.written(placeholder, writtenName(unknownHeapKey, HeapPropertyPart::Setter)));
        }
        nodesTakingCopies.append(stored);
        return stored;
    };
    if (mayBeUntrackedObject(targets))
        storeThrough(untrackedObjectAlone(), ValueFlowHostNode::SourcesOfCopiesIntoUntrackedObject);
    if (mayBeUnknownObject(targets)) {
        connect(storeThrough(unknownObjectAlone(), ValueFlowHostNode::SourcesOfCopiesIntoUnknownObject), ValueFlowProgramNodes::escaped());
        connect(m_context.readWhatWasWritten(known(sources), copiedFromName), ValueFlowProgramNodes::escaped());
        if (keepsAccessors) {
            connect(m_context.readWhatWasWritten(allSources, everyGetter), ValueFlowProgramNodes::escaped());
            connect(m_context.readWhatWasWritten(allSources, everySetter), ValueFlowProgramNodes::escaped());
        }
        add(ValueFlowProgramNodes::escaped(), m_context.restrict(known(sources), ValueSets::complement(m_tables.objectsHoldingOnlyWhatWasWritten())));
    }
    if (hasEscaped(known(targets))) {
        ClimbingSet all = allSources;
        ClimbingSet read = m_context.restrict(all, m_tables.objectsHoldingOnlyWhatWasWritten());
        add(ValueFlowProgramNodes::escaped(), m_context.restrict(all, ValueSets::complement(m_tables.objectsHoldingOnlyWhatWasWritten())));
        connect(m_context.readWhatWasWritten(read, everyValue), ValueFlowProgramNodes::escaped());
        connect(storedThroughUnknownObjectInto(read, everyValue), ValueFlowProgramNodes::escaped());
        connect(storedThroughUntrackedObjectInto(read, everyValue), ValueFlowProgramNodes::escaped());
        if (keepsAccessors) {
            connect(m_context.readWhatWasWritten(read, everyGetter), ValueFlowProgramNodes::escaped());
            connect(m_context.readWhatWasWritten(read, everySetter), ValueFlowProgramNodes::escaped());
        }
    }

    if (holdsAnything(proxies)) {
        listProxiesByTheirTraps(proxies);
        for (ValueFlowNodeID node : nodesTakingCopies)
            askProxiesForListedValues(proxies, node);
        noteWhatMayLieUnderSymbolThatHostLooksFor(unknownHeapKey, operandOf(copied));
    }
    if (holdsNamedOrLoadedBun(source)) {
        for (ValueFlowNodeID node : nodesTakingCopies)
            add(node, only(ValueFlowUniverse::markOfLoadedBun()));
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
    // A lookup under every key leaves out the loaders of a holder. A copy that the program makes takes along what makes a loader, and the program then holds it like one that it names.
    if (FunctionID copier = m_context.functionOfStatement(); copier && isWrittenInProgram(m_program.at(copier))) {
        Vector<ValueFlowObjectID, 32> loaders;
        m_context.forEachMember(m_context.restrict(sources, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
            BuiltinID builtin = m_universe.builtinOf(holder);
            if (!builtin)
                return;
            for (BuiltinID member : m_builtins.membersOf(builtin)) {
                BuiltinID loader = m_builtins.canonicalID(member);
                if (m_builtins.at(member).origin == BuiltinOrigin::Host && m_builtins.letsInUnseenCode(loader) && !m_builtins.letsInUnseenCodeWhenCalled(loader) && !m_builtins.has(BuiltinMark::IsNotEnumerable, loader))
                    m_universe.appendObjectsOf(loader, loaders);
            }
        });
        ClimbingSet copiedLoaders(m_context.maker().of(loaders.span()));
        for (ValueFlowNodeID node : nodesTakingCopies)
            add(node, copiedLoaders);
        add(m_context.nodes().ofHost(ValueFlowHostNode::LoadersAndEvaluatorsThatProgramNames), copiedLoaders);
    }

    ClimbingSet getters = keepsAccessors ? m_context.valueOf(m_context.readWhatWasWritten(allSources, everyGetter)) : gettersOfEnumerableProperties(allSources);
    if (mayBeUntrackedObject(sources)) {
        for (ClimbingSet holders : { ClimbingSet(m_universe.knownObjects()), untrackedObjectAlone(), unknownObjectAlone() }) {
            for (ValueFlowNodeID node : nodesTakingCopies)
                connect(m_context.readWhatWasWritten(holders, everyValue), node);
            getters = m_context.unite(getters, m_context.valueOf(m_context.readWhatWasWritten(holders, everyGetter)));
            if (keepsAccessors) {
                connect(m_context.readWhatWasWritten(holders, everyGetter), m_context.written(known(targets), writtenName(unknownHeapKey, HeapPropertyPart::Getter)));
                connect(m_context.readWhatWasWritten(holders, everySetter), m_context.written(known(targets), writtenName(unknownHeapKey, HeapPropertyPart::Setter)));
            }
        }
    }
    // A getter nobody knows is entered with the source that holds it, whichever others are copied beside it.
    HeapReceiver readSources { nullptr, holdsAnything(proxies) ? m_context.unite(sources, m_context.valueOf(m_context.read(proxies, proxyItselfName))) : sources, { } };
    if (!keepsAccessors && mayBeAccessorNobodyKnows(getters)) {
        ValueFlowObjectsToldApart toldApart = tellObjectsApart(m_context, m_context.membersMissingFrom(ValueFlowProgramNodes::escaped(), known(sources)));
        ClimbingSet holdingSuchGetter(toldApart.rest);
        for (ValueSetID group : toldApart.groups) {
            if (mayBeAccessorNobodyKnows(gettersOfEnumerableProperties(withSourcesOfCopies(m_context, ClimbingSet(group)))))
                holdingSuchGetter = m_context.unite(holdingSuchGetter, ClimbingSet(group));
        }
        readSources.belowAccessorNobodyKnows = holdingSuchGetter;
    }
    for (ValueFlowNodeID node : nodesTakingCopies) {
        if (mayBeUnknownObject(sources))
            add(node, Typed::unknownValue(m_context, ValueFlowUnknownSource::ContentsOfEscapedObject));
        if (m_context.meet(sources, m_universe.strings()))
            add(node, m_universe.anyPrimitive());
        if (!keepsAccessors)
            callAccessors(getters, ValueFlowCallKind::Get, readSources, nullptr, node);
    }
    // The arguments of a strict function hold an accessor of native code under `callee`.
    bool copiesArguments = m_context.meet(allSources, m_tables.argumentsObjects());
    if (keepsAccessors && (mayBeUnknownObject(sources) || holdsAnything(hostObjects) || copiesArguments || m_context.meet(sources, m_universe.builtinObjects())))
        connect(target, ValueFlowProgramNodes::escaped());
    // An unknown object may implement a property with a native accessor, and the copy takes its getter and setter.
    if (keepsAccessors && mayBeUnknownObject(sources)) {
        for (HeapPropertyPart part : { HeapPropertyPart::Getter, HeapPropertyPart::Setter })
            add(m_context.written(known(targets), writtenName(unknownHeapKey, part)), only(m_tables.undescribedNativeCode()));
    }

    if (storesThroughSetters) {
        ValueFlowOperand held = operandOf(m_context.readWhatWasWritten(allSources, everyValue));
        forEachKeyOfSource([&](const HeapKey& key) {
            convertStoredValue(targets, key, held);
            convertStoredValue(targets, key, operandOf(copied));
            forEachStartOfLookup(targets, [&](ClimbingSet start) {
                callSettersOfReceivers(start, key, held);
                callSettersOfReceivers(start, key, operandOf(m_context.readWhatWasWritten(known(targets), valueUnderUnknownKey)));
            });
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

// A property that cannot be configured takes a definition only of the value, the getter and the setter that it has.
void HeapRule::compareWithPropertyThatIsThere(const ValueFlowOperand& defined)
{
    connect(defined, m_context.nodes().ofHost(ValueFlowHostNode::ClosuresThatDefinitionsCompare));
}

// The modules of the run time name only function objects that they make themselves.
void HeapRule::noteNameDefinedThroughObjectsNobodyFollows(const ValueFlowOperand& object, const HeapKey& key)
{
    if (key.keyClass != HeapKeyClass::Name || key.name != m_tables.nameKey() || !isInProgramCode())
        return;
    if (ClimbingSet placeholders = m_context.restrict(m_context.valueOf(object), m_universe.placeholdersAlone()); holdsAnything(placeholders))
        add(m_context.nodes().ofHost(ValueFlowHostNode::ObjectsNobodyFollowsThatDefinitionsName), placeholders);
}

void HeapRule::defineAsDescribed(const ValueFlowOperand& object, const HeapKey& key, ClimbingSet descriptors, DefinedObject definedObject)
{
    if (definedObject == DefinedObject::MayHoldProperty)
        noteNameDefinedThroughObjectsNobodyFollows(object, key);
    auto define = [&](HeapPropertyPart part, BuiltinName member) {
        ValueFlowNodeID described = m_context.lookup(descriptors, ValueFlowName::property(m_program, m_program.keyOf(member)));
        if (part != HeapPropertyPart::Value && !m_context.meet(m_context.valueOf(described), ValueSets::complement(m_universe.undefinedValue())))
            return;
        this->define(object, key, part, operandOf(described));
        if (definedObject == DefinedObject::MayHoldProperty)
            compareWithPropertyThatIsThere(operandOf(described));
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
    m_context.mark(m_context.withoutProxiesThatWalksPass(m_context.restrict(m_context.valueOf(prototype), m_universe.objects())), ObjectMutation::BecomePrototype);
}

void HeapRule::readAccessorFunctions(const ValueFlowOperand& object, const HeapKey& key, HeapPropertyPart part, PropertySearch search)
{
    ClimbingSet receivers = m_context.valueOf(object);
    ClimbingSet searched = search == PropertySearch::OwnProperty ? receivers : ClimbingSet(m_universe.knownObjects());
    bool mayFindAccessorOfArguments = key.mayNameNonEnumerable(m_tables.calleeKey()) && m_context.meet(searched, m_tables.argumentsObjects());
    auto addFunctionNobodyFollows = [&] {
        add(m_statement.result, m_context.restrict(ClimbingSet(undescribedValueThatIsNoBuffer(m_context)), m_universe.objects()));
    };
    // Whoever has the setter in hand calls it with any value.
    if (replaceMembersThroughSettersFoundFrom(receivers, key, search, nullptr, part == HeapPropertyPart::Setter))
        addFunctionNobodyFollows();
    // The "length" of a function of native code is a value, whoever made the function. Only a program lays an accessor there.
    ClimbingSet mayHoldAccessorOfTable = receivers;
    if (search == PropertySearch::OwnProperty && key.keyClass == HeapKeyClass::Name && key.name == m_program.keyOf(BuiltinNames::length))
        mayHoldAccessorOfTable = m_context.restrict(receivers, ValueSets::complement(m_universe.builtinFunctions()));
    if (search == PropertySearch::OwnProperty && holdsOnlyWhatWasWritten(mayHoldAccessorOfTable) && holdsOnlyWhatWasWritten(withSourcesOfCopies(m_context, known(mayHoldAccessorOfTable)))) {
        readOwn(withSourcesOfCopies(m_context, known(receivers)), foundName(key, part), m_statement.result);
        if (mayFindAccessorOfArguments)
            addFunctionNobodyFollows();
        add(m_statement.result, m_universe.undefinedValue());
        return;
    }
    ValueFlowNodeID found = m_context.lookup(receivers, foundName(key, part));
    connect(m_context.masked(found, m_universe.callables()), m_statement.result);
    bool mayFindAccessorOfTable = m_context.meet(accessorsFoundFrom(receivers, key, part), m_universe.builtinObjects());
    // The table describes the result of reading a property. Whether native code implements the property as a value or as its own accessor is only recorded for the realm's objects, whose own properties it lists,
    // not for the exports of a native module, nor for an object that a built-in creates.
    ValueSetMaker& maker = m_context.maker();
    ValueSetID objectsOfRealmInTable = maker.intersect(m_universe.builtinObjects(), ValueSets::complement(maker.unite(m_tables.objectsOfNativeModules(), m_tables.objectsOfPublicModules())));
    ValueSetID createdByNativeCode = maker.intersect(m_universe.knownObjects(), ValueSets::complement(maker.unite(objectsOfRealmInTable, m_tables.objectsWithoutPropertiesOfNativeCode())));
    // A search of the whole chain starts at the receiver's own properties.
    bool mayFindAccessorOfNativeCode = m_context.meet(m_context.unite(receivers, withSourcesOfCopies(m_context, known(receivers))), createdByNativeCode);
    if (mayFindAccessorOfArguments || mayFindAccessorOfTable || mayFindAccessorOfNativeCode)
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
            readOwnPropertiesNobodyWrote(holders, key, m_statement.result);
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
            noteNameDefinedThroughObjectsNobodyFollows(object, key);
        }, &value);
        return;
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        forEachKey([&](const HeapKey& key) {
            define(object, key, action.kind == BuiltinPropertyActionKind::DefinesGetter ? HeapPropertyPart::Getter : HeapPropertyPart::Setter, value);
            noteNameDefinedThroughObjectsNobodyFollows(object, key);
        }, &value);
        compareWithPropertyThatIsThere(value);
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
            defineAsDescribed(object, key, m_context.valueOf(value), DefinedObject::MayHoldProperty);
        }, &value);
        return;
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::DescribesResult: {
        auto defineUnder = [&](const HeapKey& key) {
            defineAsDescribed(object, key, m_context.valueOf(m_context.lookup(m_context.valueOf(value), foundName(key, HeapPropertyPart::Value))), action.kind == BuiltinPropertyActionKind::DescribesResult ? DefinedObject::IsNew : DefinedObject::MayHoldProperty);
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
