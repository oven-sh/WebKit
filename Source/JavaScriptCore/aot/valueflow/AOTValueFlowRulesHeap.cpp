/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTKnownClosures.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

ValueSetID HeapRule::kinds(uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return m_context.maker().of(members.span());
}

void HeapRule::connectThrough(ValueSetID mask, const ValueFlowOperand& from, ValueFlowNodeID to)
{
    if (!to)
        return;
    if (from.node)
        m_context.connect(m_context.masked(m_context.masked(from.node, from.mask, from.maskKind), mask), to);
    m_context.add(to, m_context.restrict(m_context.valueOf(ValueFlowOperand { { }, from.constant, from.mask, from.maskKind }), mask));
}

void HeapRule::passToAccessorNobodyKnows(const HeapReceiver& receiver, ValueSetID mask)
{
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    if (!receiver.operand) {
        add(escaped, m_context.restrict(receiver.belowAccessorNobodyKnows, mask));
        return;
    }
    if (!receiver.isStartOfLookup) {
        connectThrough(mask, *receiver.operand, escaped);
        return;
    }
    if (mayBeUntrackedObject(m_context.valueOf(*receiver.operand)))
        m_context.lookup(untrackedObjectAlone(), prototypeLinkName);
    connectThrough(m_context.maker().intersect(mask, ValueSets::complement(m_universe.untrackedObjectAlone())), *receiver.operand, escaped);
}

HeapKeyReceivers HeapRule::receiversOfSymbolNobodyKnows(ClimbingSet keys, const HeapKeyUse& use)
{
    if (!use.object)
        return HeapKeyReceivers::AllButBuiltinObjects;
    ValueSetMaker& maker = m_context.maker();
    if (m_context.meet(keys, maker.intersect(m_tables.keysNobodyKnows(), ValueSets::complement(m_universe.symbols()))))
        return HeapKeyReceivers::AllButBuiltinObjects;
    if (maker.intersect(m_universe.symbols(), m_universe.keys()) == ValueSets::nothing)
        return HeapKeyReceivers::AllButBuiltinObjects;
    ClimbingSet receivers = m_context.valueOf(*use.object);
    ValueSetID builtinObjects = maker.intersect(m_universe.builtinObjects(), ValueSets::complement(m_universe.placeholdersAlone()));
    if (!m_context.meet(receivers, builtinObjects))
        return HeapKeyReceivers::AllButBuiltinObjects;
    if (!m_context.meet(receivers, ValueSets::complement(builtinObjects)))
        return HeapKeyReceivers::All;
    return use.tellsReceiversApart ? HeapKeyReceivers::BuiltinObjects : HeapKeyReceivers::AllButBuiltinObjects;
}

ValueFlowOperand HeapRule::receiversUnder(const ValueFlowOperand& object, const HeapKey& key)
{
    if (key.receivers == HeapKeyReceivers::All)
        return object;
    ValueSetMaker& maker = m_context.maker();
    ValueSetID builtinObjects = maker.intersect(m_universe.builtinObjects(), ValueSets::complement(m_universe.placeholdersAlone()));
    ValueSetID meant = key.receivers == HeapKeyReceivers::BuiltinObjects ? builtinObjects : ValueSets::complement(builtinObjects);
    ValueFlowOperand receivers = object;
    bool keepsOut = object.mask && (object.maskKind == ValueFlowMaskKind::Plain || object.maskKind == ValueFlowMaskKind::Checked);
    receivers.mask = keepsOut ? maker.intersect(object.mask, meant) : meant;
    if (!keepsOut)
        receivers.maskKind = ValueFlowMaskKind::Plain;
    return receivers;
}

void HeapRule::read(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    // Beside a placeholder the receivers that it covers are listed for a while only: each list is looked up as it comes.
    bool areReceiversListedForGood = !m_context.meet(receivers, m_universe.placeholdersAlone());
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (namesObjectOfAccess() && isNameThatTableDoesNotListOnGlobalObject(key) && m_context.contains(receivers, globalObject)) {
        readThroughChain(ClimbingSet(only(globalObject)), key, { &object, { } }, m_statement.result);
        receivers = m_context.restrict(receivers, ValueSets::complement(only(globalObject)));
    }

    if (mayBeUnknownObject(receivers) && m_statement.function && isCodeThatRecordsDescribe(m_universe, m_statement.function)) {
        add(m_statement.result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        receivers = m_context.restrict(receivers, ValueSets::complement(m_context.maker().unite(m_universe.unknownObjectAlone(), m_universe.anyPrimitive())));
    }

    if (!namesObjectOfAccess() && key.keyClass == HeapKeyClass::Name && m_context.contains(receivers, globalObject) && doesUnseenCodeRun()) {
        forEachMemberUnder(ClimbingSet(only(globalObject)), key, [&](BuiltinID member, ModuleID) {
            if (m_builtins.at(member).mutability == BuiltinMutability::ChangesOnItsOwn)
                add(m_statement.result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        });
    }

    ValueFlowName name = nameOfLookupForValue(key);
    forEachStartOfLookup(receivers, [&](ClimbingSet start) {
        if (areReceiversListedForGood && tellsApartStartsThatFindNothing(start, key))
            readFromStartsThatMayFindSomething(start, key);
        else
            connect(m_context.lookup(start, name), m_statement.result);
    });
    if (key.keyClass == HeapKeyClass::Unknown) {
        // A key nobody knows may be a symbol that the program has found by listing the keys.
        if (m_context.holdsAnyOf(known(receivers), m_context.nodes().ofHost(ValueFlowHostNode::ObjectsHoldingSymbolsUsedOnlyAsKeys)))
            add(m_statement.result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        if (ClimbingSet listed = hostObjectsListedBesideUnknownObject(object, receivers); holdsAnything(listed))
            connect(m_context.lookup(listed, name), m_statement.result);
        readExportsOfListedHolders(object, receivers);
        if (holdsNamedOrLoadedBun(object))
            add(m_statement.result, only(ValueFlowUniverse::markOfLoadedBun()));
    }
    if (key.keyClass == HeapKeyClass::Name && mayBeUnknownObject(receivers) && m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::Bun))) {
        // A read that a built-in makes for its caller, such as Reflect.get(), has no site.
        BuiltinID named = m_statement.function && m_statement.site ? entryNamedByOperations(m_program, m_program.at(m_statement.function), m_statement.site) : BuiltinID();
        bool isNamed = named && isMemberOfBun(named);
        ClimbingSet bun(only(m_universe.objectOf(Builtins::Bun)));
        if (isNamed && m_context.sets().contains(m_universe.builtinFunctions(), m_universe.objectOf(named))) {
            use(named, BuiltinUse::Read);
            add(m_statement.result, only(m_universe.objectOf(named)));
        } else if (isNamed)
            connect(m_context.lookup(bun, name), m_statement.result);
        else if ((object.node && m_context.holdsAnyOf(bun, object.node)) || holdsMarkOfLoadedBun(object.node) || hasEscaped(ClimbingSet(only(ValueFlowUniverse::markOfNamedBun())))) {
            // The read already answers a value nobody knows, which stands for whatever has escaped.
            connect(m_context.lookup(bun, name), ValueFlowProgramNodes::escaped());
        }
    }
    if (key.keyClass == HeapKeyClass::Name && !mayBeUnknownObject(receivers) && m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::Bun))
        && hasEscaped(ClimbingSet(only(ValueFlowUniverse::markOfNamedBun()))) && m_context.holdsAnyOf(known(receivers), m_context.nodes().ofHost(ValueFlowHostNode::CopiesOfObjectsNobodyKnows))) {
        // What was copied stands for whatever has escaped, and the read already answers a value nobody knows.
        connect(m_context.lookup(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), name), ValueFlowProgramNodes::escaped());
    }
    if (key.keyClass == HeapKeyClass::Name && m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::Bun)) && holdsMarkOfLoadedBun(m_statement.result)) {
        // What the read answers came out of Bun under a key nobody knows, so it may be the member of this name. Such a value stands for whatever has escaped.
        ValueFlowNodeID member = m_context.lookup(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), name);
        connect(member, m_statement.result);
        connect(member, ValueFlowProgramNodes::escaped());
        if (ValueFlowNodeID described = valueOfDescriptorsInAnswer())
            connect(member, described);
    } else if (key.keyClass == HeapKeyClass::Name && m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::Bun)) && mayAnswerWhatEscapedOutOfBun()) {
        // The read already answers a value nobody knows, which stands for whatever has escaped.
        connect(m_context.lookup(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), name), ValueFlowProgramNodes::escaped());
    }
    if (key.keyClass == HeapKeyClass::Name && object.node && mayBeUnknownObject(receivers)) {
        // Bun read off a value that may be the global object goes on being listed, so that a name read on it is found.
        BuiltinID member = memberUnder(Builtins::GlobalThis, key.name);
        if (member && m_builtins.canonicalID(member) == Builtins::Bun && m_context.holdsAnyOf(ClimbingSet(only(globalObject)), object.node))
            connect(m_context.lookup(ClimbingSet(only(globalObject)), name), m_statement.result);
    }
    if (key.keyClass == HeapKeyClass::Name && key.name == m_tables.prototypeKey() && object.node && mayBeUnknownObject(receivers)) {
        if (ClimbingSet listed = m_context.valueWithin(ClimbingSet(m_tables.under(key.name).holders), object.node); holdsAnything(listed)) {
            ValueFlowNodeID found = m_context.lookup(listed, name);
            connect(found, m_statement.result);
            add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.restrict(m_context.valueOf(found), m_tables.prototypeObjectsThatListingDoesNotName()));
        }
    }
    readEvaluators(object, key);
    handOutPrototypeObjectsAtCreation(receivers, key);
    mayThrow();
}

// A descriptor that was read under a key nobody knows, and is found under a name, describes the member of that name.
ValueFlowNodeID HeapRule::valueOfDescriptorsInAnswer()
{
    std::optional<ClimbingSet> descriptors = m_context.holdersUnder(describedObjectName, known(m_context.valueOf(m_statement.result)));
    if (!descriptors || !holdsAnything(*descriptors))
        return { };
    return m_context.written(*descriptors, ValueFlowName::property(m_program.keyOf(BuiltinNames::value)));
}

void HeapRule::readOwn(ClimbingSet holders, ValueFlowName name, ValueFlowNodeID result)
{
    ClimbingSet objects = known(holders);
    if (name == prototypeLinkName)
        objects = m_context.restrict(objects, ValueSets::complement(m_tables.holdersWithLockedPrototypeLink()));
    auto answer = [&](ValueFlowNodeID node) {
        connect(node, result);
    };
    forEachNodeHolding(objects, name, answer);
    if (mayBeUntrackedObject(holders))
        forEachNodeReadByName(name, answer);
    if (!mayBeUnknownObject(holders))
        return;
    add(result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
    if (name == prototypeLinkName)
        letPrototypeObjectsOfLanguageEscape();
}

void HeapRule::test(const ValueFlowOperand& object, const HeapKey& key)
{
    noteTestOfMembers(object, key);
    answerBoolean();
    mayThrow();
}

void HeapRule::noteTestOfMembers(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    HeapChain chain = chainFrom(receivers);
    auto noteTest = [&](BuiltinID member, ModuleID) {
        use(member, BuiltinUse::Tested);
    };
    auto noteTestThatNamesHolder = [&](BuiltinID member, ModuleID) {
        bool isNamed = key.keyClass == HeapKeyClass::Name || (!isFoundByNameAlone(m_builtins.canonicalID(member)) && !isMemberOfBun(member));
        bool isMadeByTest = isNamed && isMadeWithCodeOfLibrary(m_builtins, member) && !m_tables.letsInUnseenCode(member);
        use(member, isMadeByTest ? OptionSet<BuiltinUse> { BuiltinUse::Tested, BuiltinUse::Read } : OptionSet<BuiltinUse> { BuiltinUse::Tested });
    };
    auto notNotedYet = [&](ClimbingSet holders, unsigned number) {
        return key.keyClass == HeapKeyClass::Unknown ? m_context.membersNewToThread(m_context.restrict(holders, m_tables.holdersWithMembers()), number) : holders;
    };
    forEachMemberUnder(notNotedYet(chain.objects, 0), key, noteTestThatNamesHolder);
    forEachMemberUnder(notNotedYet(hostObjectsListedBesideUnknownObject(object, receivers), 0), key, noteTestThatNamesHolder);
    if (chain.holdsUntrackedObject)
        forEachMemberUnder(notNotedYet(foundThroughUntrackedObject(holdersWithMemberUnder(key)), 1), key, noteTest);
    if (chain.holdsUnknownObject)
        forEachMemberUnder(notNotedYet(holdersFoundThroughUnknownObject(key), 1), key, noteTest);
}

void HeapRule::write(ClimbingSet objects, ValueFlowName name, const ValueFlowOperand& value)
{
    connect(value, m_context.written(known(objects), name));
    bool namesProperty = name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter;
    if (namesProperty && m_tables.isSymbolUsedOnlyAsKey(PropertyKeyID(name.payload)))
        add(m_context.nodes().ofHost(ValueFlowHostNode::ObjectsHoldingSymbolsUsedOnlyAsKeys), known(objects));
    bool mayBeUntracked = mayBeUntrackedObject(objects);
    if (mayBeUntracked)
        connect(value, m_context.written(untrackedObjectAlone(), name));
    bool writesIntoRealm = mayBeUntracked && hasBeenLost(ClimbingSet(m_tables.objectsOfRealm()));
    m_context.forEachMember(m_context.restrict(objects, m_universe.builtinObjects()), [&](ValueFlowObjectID object) {
        if (m_universe.kindOf(object) != ValueFlowObjectKind::Builtin)
            return;
        const ValueFlowNamesReadByNativeCode* listed = m_tables.namesReadByNativeCodeOn(object);
        if (!listed) {
            bool isSeenByStatementsAlone = object == m_universe.objectOf(Builtins::GlobalThis) && namesProperty && namesObjectOfAccess()
                && isNameThatTableDoesNotListOnGlobalObject({ HeapKeyClass::Name, PropertyKeyID(name.payload) }) && holdsGlobalObjectOnlyWhatStatementsHaveStored();
            writesIntoRealm |= !isSeenByStatementsAlone;
            return;
        }
        if (name.kind == ValueFlowNameKind::PrototypeLink) {
            for (const ValueFlowNameReadByNativeCode& read : listed->names)
                letNativeCodeRead(ClimbingSet(listed->entries), read);
            return;
        }
        const ValueFlowNameReadByNativeCode* read = namesProperty ? listed->find(PropertyKeyID(name.payload)) : nullptr;
        writesIntoRealm |= !namesProperty || (read && (name.kind != ValueFlowNameKind::Property || !read->isOnlyInspected));
    });
    if (writesIntoRealm)
        connect(value, ValueFlowProgramNodes::escaped());
    if (!mayBeUnknownObject(objects))
        return;
    connect(value, m_context.written(unknownObjectAlone(), name));
    connect(value, ValueFlowProgramNodes::escaped());
    add(m_context.nodes().ofHost(ValueFlowHostNode::ObjectNobodyKnowsIsWritten), m_universe.unknownObjectAlone());
}

void HeapRule::noteMembersLookedForBeforeChange(ClimbingSet receivers, const HeapKey& key, bool makesAllOnceFound)
{
    ClimbingSet holders = m_context.restrict(receivers, m_tables.holdersOfMembersThatHostMakes());
    if (!holdsAnything(holders))
        return;
    bool hasFoundMember = false;
    auto noteMaking = [&](const HeapKey& lookedFor) {
        forEachMemberUnder(holders, lookedFor, [&](BuiltinID member, ModuleID) {
            hasFoundMember = true;
            bool isNamed = lookedFor.keyClass == HeapKeyClass::Name || (!isFoundByNameAlone(m_builtins.canonicalID(member)) && !isMemberOfBun(member));
            if (isNamed && isMadeWithCodeOfLibrary(m_builtins, member) && !m_tables.letsInUnseenCode(member))
                use(member, { BuiltinUse::Tested, BuiltinUse::Read });
        });
    };
    noteMaking(key);
    if (makesAllOnceFound && hasFoundMember && key.keyClass == HeapKeyClass::Name)
        noteMaking(unknownHeapKey);
}

// Retired nodes list the holders of replaceable members all over a program. An object that is written escapes through the unknown object, so such a holder takes the unknown value for it, and the writes that list it are not joined with the reads that do.
// A primitive holds nothing and runs nothing: it is written as it is.
void HeapRule::writeToListedObjects(ClimbingSet listed, ValueFlowName name, const ValueFlowOperand& value)
{
    ValueSetID holders = m_tables.holdersOfReplaceableMembers();
    write(m_context.restrict(listed, ValueSets::complement(holders)), name, value);
    ClimbingSet listedHolders = m_context.restrict(listed, holders);
    ValueFlowOperand primitives = value;
    primitives.mask = m_context.maker().intersect(value.mask, m_universe.primitives());
    write(listedHolders, name, primitives);
    if (m_context.meet(m_context.valueOf(value), m_universe.objects()))
        write(listedHolders, name, ValueFlowOperand { { }, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject) });
}

void HeapRule::noteWrittenMembers(ClimbingSet receivers, const HeapKey& key, bool areOnlyListed)
{
    if (!m_context.meet(receivers, m_tables.builtinObjectsAndPlaceholders()))
        return;
    ValueSetID counted = areOnlyListed ? m_tables.holdersOfMembersThatHostMakes() : m_universe.builtinObjects();
    if (ClimbingSet written = m_context.restrict(known(receivers), counted); holdsAnything(written))
        add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsThatStatementsWrite), written);

    ValueSetID holders = key.keyClass == HeapKeyClass::Index ? ValueSets::nothing : holdersWithMemberUnder(key);
    if (holders != ValueSets::nothing) {
        ClimbingSet written = m_context.restrict(receivers, m_context.maker().unite(holders, m_universe.placeholdersAlone()));
        if (holdsAnything(written))
            m_context.lookup(written, key.keyClass == HeapKeyClass::Name ? ValueFlowName { ValueFlowNameKind::MemberWritten, key.name.index() } : ValueFlowName::of(ValueFlowNameKind::EveryMemberWritten));
    }
    applyEventsOfStore(m_statement, m_context, receivers, key.keyClass == HeapKeyClass::Name ? key.name : PropertyKeyID());
}

ValueSetID HeapRule::standingForClassAndThoseBelow(BuiltinClassID builtinClass)
{
    const ValueFlowBuiltinTables& tables = m_universe.builtinTables();
    Members standing;
    for (BuiltinClassID below : tables.classesAtOrBelow(builtinClass)) {
        if (ValueFlowObjectID object = tables.objectStandingFor(below))
            standing.append(object);
    }
    return m_context.maker().of(standing.span());
}

ValueSetID HeapRule::allocatedByBuiltinsAs(ClimbingSet objects, ValueSetID standingForClasses)
{
    ClimbingSet candidates = m_context.restrict(objects, m_context.maker().intersect(m_universe.objectsThatMayBeAnything(), m_universe.knownObjects()));
    if (standingForClasses == ValueSets::nothing || !holdsAnything(candidates))
        return ValueSets::nothing;
    ValueSetID otherClasses = ValueSets::complement(standingForClasses);
    Members allocated;
    m_context.forEachMember(candidates, [&](ValueFlowObjectID object) {
        ClimbingSet named = m_context.valueOf(m_context.written(ClimbingSet(only(object)), allocatedAsName));
        if (holdsAnything(named) && !m_context.meet(named, otherClasses))
            allocated.append(object);
    });
    return m_context.maker().of(allocated.span());
}

ValueSetID HeapRule::instancesThatKeepProperty(ClimbingSet objects, PropertyKeyID name)
{
    ValueSetID instances = m_tables.under(name).instancesThatKeepProperty;
    for (const ValueFlowPropertyKeptForLife& kept : m_tables.propertiesKeptForLife()) {
        if (kept.key == name)
            instances = m_context.maker().unite(instances, allocatedByBuiltinsAs(objects, standingForClassAndThoseBelow(kept.instanceClass)));
    }
    return instances;
}

void HeapRule::markStore(ClimbingSet receivers, const HeapKey& key)
{
    ClimbingSet objects = m_context.restrict(receivers, m_universe.objects());
    m_context.mark(objects, ObjectMutation::StoreAfterCreation);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        objects = m_context.restrict(objects, ValueSets::complement(m_context.maker().unite(m_tables.objectsDeclaringField(key.name), instancesThatKeepProperty(objects, key.name))));
        break;
    case HeapKeyClass::Index: {
        ValueSetID standingForArrays = m_context.maker().unite(standingForClassAndThoseBelow(BuiltinClasses::Array), m_tables.prototypesOfTypedArrays());
        objects = m_context.restrict(objects, ValueSets::complement(m_context.maker().unite(m_universe.sureInstancesOf(BuiltinClasses::Array), allocatedByBuiltinsAs(objects, standingForArrays))));
        break;
    }
    case HeapKeyClass::Unknown:
        break;
    }
    if (!holdsAnything(objects))
        return;
    m_context.mark(objects, ObjectMutation::AddProperty);
    ClimbingSet objectsOfProgram = m_context.restrict(objects, m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects())));
    if (holdsAnything(objectsOfProgram))
        add(m_context.written(objectsOfProgram, addedNamesName), key.keyClass == HeapKeyClass::Name ? only(m_universe.keyConstant(key.name)) : m_tables.stringAndSymbolKinds());
}

void HeapRule::markDefinition(ClimbingSet receivers, const HeapKey& key, HeapPropertyPart part)
{
    ClimbingSet objects = known(receivers);
    auto mayHave = [&](HeapPropertyPart accessor) {
        ValueFlowName name = foundName(key, accessor);
        return holdsAnything(readWhereWritten(objects, name)) || storedThroughUnknownObjectInto(objects, name) || storedThroughUntrackedObjectInto(objects, name);
    };

    bool redefines = part != HeapPropertyPart::Value || m_context.meet(receivers, m_universe.placeholdersAlone()) || mayHave(HeapPropertyPart::Getter) || mayHave(HeapPropertyPart::Setter);
    markStore(receivers, key);
    // A namespace has its exports from the start: what is defined on one is there already.
    if (ClimbingSet redefined = redefines ? receivers : m_context.restrict(receivers, m_universe.namespaces()); redefines || holdsAnything(redefined))
        markRedefinition(redefined, key);
}

void HeapRule::markRedefinition(ClimbingSet receivers, const HeapKey& key)
{
    m_context.mark(m_context.restrict(receivers, m_universe.objects()), ObjectMutation::RedefineProperty);
    if (key.keyClass == HeapKeyClass::Index)
        return;
    ValueFlowName name = key.keyClass == HeapKeyClass::Name ? ValueFlowName { ValueFlowNameKind::RedefinedMember, key.name.index() } : ValueFlowName::of(ValueFlowNameKind::RedefinedMemberUnderUnknownKey);
    ClimbingSet redefined = m_context.restrict(receivers, m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects())));
    if (holdsAnything(redefined))
        add(m_context.written(redefined, name), m_universe.undefinedValue());
    if (mayBeUntrackedObject(receivers))
        add(m_context.written(untrackedObjectAlone(), name), m_universe.undefinedValue());
    if (mayBeUnknownObject(receivers))
        add(m_context.written(unknownObjectAlone(), name), m_universe.undefinedValue());
}

void HeapRule::define(const ValueFlowOperand& object, const HeapKey& key, HeapPropertyPart part, const ValueFlowOperand& value)
{
    ClimbingSet receivers = m_context.valueOf(object);
    write(receivers, writtenName(key, part), value);
    if (part == HeapPropertyPart::Value)
        convertStoredValue(receivers, key, value);
    else if (mayBeAccessorNobodyKnows(m_context.valueOf(value))) {
        connect(object, ValueFlowProgramNodes::escaped());
    }
    noteWrittenMembers(receivers, key);
    noteMembersLookedForBeforeChange(receivers, key);
    if (ClimbingSet listed = unlockedObjectsListedBesideUnknownObject(object, receivers, key); holdsAnything(listed)) {
        writeToListedObjects(listed, writtenName(key, part), value);
        noteWrittenMembers(listed, key);
        noteMembersLookedForBeforeChange(listed, key);
    }
    if (ClimbingSet holders = holdersOfReplaceableMemberThatUnknownObjectMayBe(object, receivers, key); holdsAnything(holders)) {
        write(holders, writtenName(key, part), value);
        noteWrittenMembers(holders, key, true);
    }
    notePrototypeObjectsThatWriteMayReach(object, receivers, key);
    noteClosuresThatMayBeRenamed(object, key, receivers);
    markDefinition(receivers, key, part);
    mayThrow();
}

void HeapRule::convertStoredValue(ClimbingSet receivers, const HeapKey& key, const ValueFlowOperand& value)
{
    bool mayBeUntracked = mayBeUntrackedObject(receivers);
    bool isConverted = false;
    if (key.keyClass != HeapKeyClass::Name) {
        ValueSetID prototypes = m_tables.prototypesOfTypedArrays();
        isConverted = m_context.meet(chainFrom(receivers).objects, prototypes) || (mayBeUntracked && holdsAnything(foundThroughUntrackedObject(prototypes)));
    }
    bool mayNameLength = key.keyClass == HeapKeyClass::Unknown || (key.keyClass == HeapKeyClass::Name && key.name == m_tables.lengthKey());
    if (!isConverted && mayNameLength) {
        ValueSetID arrays = m_context.maker().intersect(m_universe.instancesOf(BuiltinClasses::Array), m_universe.knownObjects());
        isConverted = m_context.meet(receivers, arrays) || (mayBeUntracked && hasBeenLost(ClimbingSet(arrays)));
    }
    ValueSetID keepingStrings = m_tables.hostObjectsKeepingStrings();
    if (m_context.meet(receivers, keepingStrings) || (mayBeUntracked && hasBeenLost(ClimbingSet(keepingStrings)))) {
        isConverted = true;
        if (m_context.meet(m_context.valueOf(value), ValueSets::complement(m_tables.stringsNumbersAndBooleans())))
            add(m_context.nodes().ofHost(ValueFlowHostNode::ObjectNobodyKnowsIsWritten), m_universe.unknownObjectAlone());
    }
    if (isConverted)
        connect(value, m_context.hub(m_universe.builtinTables().conversionToPrimitive()));
}

ClimbingSet HeapRule::accessorsFoundFrom(ClimbingSet start, const HeapKey& key, HeapPropertyPart part)
{
    ValueFlowName name = foundName(key, part);
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (!namesObjectOfAccess() || !isNameThatTableDoesNotListOnGlobalObject(key) || !m_context.contains(start, globalObject))
        return m_context.valueOf(m_context.lookup(start, name));

    ClimbingSet alone(only(globalObject));
    ClimbingSet accessors = m_context.valueOf(m_context.lookup(alone, name));
    if (!mayHoldUndeclaredProperty(chainFrom(alone, key).objects, key))
        accessors = m_context.restrict(accessors, ValueSets::complement(only(m_tables.undescribedNativeCode())));
    ClimbingSet others = m_context.restrict(start, ValueSets::complement(only(globalObject)));
    return holdsAnything(others) ? m_context.unite(accessors, m_context.valueOf(m_context.lookup(others, name))) : accessors;
}

bool HeapRule::replaceMembersThroughSettersFoundFrom(ClimbingSet receivers, const HeapKey& key, PropertySearch search, const ValueFlowOperand* value, bool isGivenAnyValue)
{
    bool isFound = false;
    for (const ValueFlowMemberReplacedBySetter& member : m_tables.membersReplacedBySetters()) {
        if (key.keyClass == HeapKeyClass::Index || (key.keyClass == HeapKeyClass::Name && key.name != member.key))
            continue;
        ClimbingSet holder(only(member.holder));
        bool isListed = m_context.contains(search == PropertySearch::OwnProperty ? receivers : chainFrom(receivers).objects, member.holder);
        bool isNamedOnObjectNobodyFollows = key.keyClass == HeapKeyClass::Name && ((mayBeUnknownObject(receivers) && hasEscaped(holder)) || (mayBeUntrackedObject(receivers) && hasBeenLost(holder)));
        if (!isListed && !isNamedOnObjectNobodyFollows)
            continue;
        isFound = true;
        HeapKey named { HeapKeyClass::Name, member.key };
        add(m_context.written(holder, writtenName(named, HeapPropertyPart::Setter)), only(m_tables.undescribedNativeCode()));
        if (!value && !isGivenAnyValue)
            continue;
        write(holder, writtenName(named, HeapPropertyPart::Value), value ? *value : ValueFlowOperand { { }, unknownValue(ValueFlowUnknownSource::UndescribedNativeResult) });
        noteWrittenMembers(holder, named);
    }
    return isFound;
}

void HeapRule::callSettersOfReceivers(ClimbingSet start, const HeapKey& key, const ValueFlowOperand& value)
{
    ClimbingSet setters = accessorsFoundFrom(start, key, HeapPropertyPart::Setter);
    HeapReceiver receiver { nullptr, start };
    receiver.keyLookedUp = &key;
    if (mayBeAccessorNobodyKnows(setters))
        receiver.belowAccessorNobodyKnows = receiversBelowAccessorNobodyKnows(chainFrom(start, key).start, key, HeapPropertyPart::Setter);
    callAccessors(setters, ValueFlowCallKind::Set, receiver, &value, { });
}

ClimbingSet HeapRule::builtinObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    if (!object.node || !mayBeUnknownObject(receivers) || key.receivers == HeapKeyReceivers::AllButBuiltinObjects)
        return { };
    return m_context.valueWithin(ClimbingSet(m_context.maker().intersect(m_context.maker().intersect(m_universe.builtinObjects(), objectsThatListingNamesUnder(key)), allButBunUnlessNamed(object))), object.node);
}

// A stack frame shows the own property `name` or `displayName` of the function object. A rule reads no closure that has escaped beside the object nobody knows, so the node of the operand hands on its closures.
void HeapRule::noteClosuresThatMayBeRenamed(const ValueFlowOperand& object, const HeapKey& key, ClimbingSet receivers)
{
    if (key.keyClass == HeapKeyClass::Index || (key.keyClass == HeapKeyClass::Name && key.name != m_tables.nameKey() && key.name != m_tables.displayNameKey()))
        return;
    if (m_namesFunctionWhereItIsMade)
        return;
    // The texts that are written as the `name` of a closure that the rule reads say whether they rename it.
    bool writesTextOfName = key.keyClass == HeapKeyClass::Name && key.name == m_tables.nameKey();
    ClimbingSet closures = m_context.restrict(known(receivers), m_universe.closures());
    if (writesTextOfName && holdsAnything(closures))
        add(m_context.nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsName), std::exchange(closures, { }));
    // The modules of the run time name a function of the program only under the key as it is written.
    if (object.node && (key.keyClass == HeapKeyClass::Name || isInProgramCode()) && m_context.meet(receivers, m_universe.placeholdersAlone()))
        connect(object.node, m_context.nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsMayRename));
    if (holdsAnything(closures)) {
        add(m_context.nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsMayRename), closures);
        add(m_context.nodes().ofHost(ValueFlowHostNode::ClosuresThatStatementsGiveDisplayName), closures);
    }
}

void HeapRule::notePrototypeObjectsThatWriteMayReach(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    if (!object.node || key.keyClass != HeapKeyClass::Unknown || !mayBeUnknownObject(receivers) || key.receivers == HeapKeyReceivers::AllButBuiltinObjects)
        return;
    add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.valueWithin(ClimbingSet(m_tables.prototypeObjectsThatListingDoesNotName()), object.node));
    // A lookup above the unknown object lets every prototype object of the language escape. Only one that was carried out comes back as a value.
    if (isInProgramCode())
        add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.valueWithin(ClimbingSet(m_tables.prototypeObjectsThatListingDoesNotName()), m_context.nodes().ofHost(ValueFlowHostNode::ValuesCarriedOut)));
}

ClimbingSet HeapRule::unlockedObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    ClimbingSet listed = builtinObjectsListedBesideUnknownObject(object, receivers, key);
    return holdsAnything(listed) ? m_context.valueWithin(listed, m_context.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsThatStatementsWrite)) : ClimbingSet();
}

ClimbingSet HeapRule::holdersOfReplaceableMemberThatUnknownObjectMayBe(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    if (key.keyClass != HeapKeyClass::Name || !isInProgramCode())
        return { };
    ClimbingSet candidates = builtinObjectsListedBesideUnknownObject(object, receivers, key);
    // No listing names a prototype object of the language that came back from code nobody sees.
    if (mayBeUnknownObject(receivers) && key.receivers != HeapKeyReceivers::AllButBuiltinObjects)
        candidates = m_context.unite(candidates, escapedAmong(ClimbingSet(m_tables.prototypeObjectsThatListingDoesNotName())));
    Members holders;
    forEachMemberUnder(candidates, key, [&](BuiltinID member, ModuleID module) {
        if (!module && m_builtins.at(member).mutability == BuiltinMutability::ConstantUnlessWritten && !m_builtins.isLocationOfLockedObject(member))
            holders.append(m_universe.objectOf(m_builtins.at(member).holder));
    });
    return ClimbingSet(m_context.maker().of(holders.span()));
}

bool HeapRule::isInProgramCode()
{
    FunctionID function = m_statement.function ? m_statement.function : m_context.functionOfStatement();
    if (!function)
        return false;
    FunctionRole role = m_program.at(function).role;
    return role != FunctionRole::Library && role != FunctionRole::Model;
}

void HeapRule::readExportsOfListedHolders(const ValueFlowOperand& object, ClimbingSet receivers)
{
    ClimbingSet holdersOfExports = holdersOfExportsListedBesideUnknownObjectInProgram(object, receivers);
    if (ClimbingSet start = startOfLookup(receivers); holdsAnything(start) && isInProgramCode()) {
        ValueFlowNodeID chain = m_context.lookup(start, prototypeLinkName);
        if (mayBeUnknownObject(m_context.valueOf(chain)))
            holdersOfExports = m_context.unite(holdersOfExports, m_context.valueWithin(ClimbingSet(m_universe.holdersOfLibraryExports()), chain));
    }
    if (holdsAnything(holdersOfExports))
        connect(m_context.lookup(holdersOfExports, everyPropertyOfListedHoldersOfExports), m_statement.result);
}

ValueFlowNodeID HeapRule::exportsOfHoldersListedBesideUnknownObject(const ValueFlowOperand& object)
{
    ClimbingSet holdersOfExports = holdersOfExportsListedBesideUnknownObjectInProgram(object, m_context.valueOf(object));
    return holdsAnything(holdersOfExports) ? m_context.lookup(holdersOfExports, everyPropertyOfListedHoldersOfExports) : ValueFlowNodeID();
}

ValueFlowNodeID HeapRule::exportsOfHoldersIn(const ValueFlowOperand& object)
{
    if (!isInProgramCode())
        return { };
    ClimbingSet receivers = m_context.valueOf(object);
    ClimbingSet holdersOfExports = m_context.unite(m_context.restrict(receivers, m_universe.holdersOfLibraryExports()), holdersOfExportsListedBesideUnknownObjectInProgram(object, receivers));
    return holdsAnything(holdersOfExports) ? m_context.lookup(holdersOfExports, everyPropertyOfListedHoldersOfExports) : ValueFlowNodeID();
}

void HeapRule::store(const ValueFlowOperand& object, const HeapKey& key, const ValueFlowOperand& value)
{
    ClimbingSet receivers = m_context.valueOf(object);
    write(receivers, writtenName(key, HeapPropertyPart::Value), value);
    convertStoredValue(receivers, key, value);
    noteWrittenMembers(receivers, key);
    if (ClimbingSet listed = builtinObjectsListedBesideUnknownObject(object, receivers, key); holdsAnything(listed)) {
        writeToListedObjects(listed, writtenName(key, HeapPropertyPart::Value), value);
        noteWrittenMembers(listed, key, true);
    }
    if (ClimbingSet holders = holdersOfReplaceableMemberThatUnknownObjectMayBe(object, receivers, key); holdsAnything(holders)) {
        write(holders, writtenName(key, HeapPropertyPart::Value), value);
        noteWrittenMembers(holders, key, true);
    }
    notePrototypeObjectsThatWriteMayReach(object, receivers, key);
    noteClosuresThatMayBeRenamed(object, key, receivers);
    markStore(receivers, key);
    forEachStartOfLookup(receivers, [&](ClimbingSet start) {
        callSettersOfReceivers(start, key, value);
    });
    replaceMembersThroughSettersFoundFrom(receivers, key, PropertySearch::WholeChain, &value);
    mayThrow();
}

void HeapRule::noteDeletedMembers(ClimbingSet receivers, const HeapKey& key)
{
    ValueSetID objectsOfProgram = m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
    if (key.keyClass == HeapKeyClass::Index)
        return;
    bool isNamed = key.keyClass == HeapKeyClass::Name;
    ValueSetID holders = isNamed ? m_tables.holdersThatAnswerUnder(key.name) : m_tables.holdersThatAnswerUnderSomeKey();
    ValueFlowName name = isNamed ? ValueFlowName { ValueFlowNameKind::DeletedMember, key.name.index() } : ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey);
    ClimbingSet deletedFrom = m_context.restrict(receivers, m_context.maker().unite(holders, objectsOfProgram));
    if (holdsAnything(deletedFrom))
        add(m_context.written(deletedFrom, name), m_universe.undefinedValue());
    if (mayBeUntrackedObject(receivers))
        add(m_context.written(untrackedObjectAlone(), name), m_universe.undefinedValue());
    if (mayBeUnknownObject(receivers))
        add(m_context.written(unknownObjectAlone(), name), m_universe.undefinedValue());
}

void HeapRule::remove(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    noteWrittenMembers(receivers, key);
    noteMembersLookedForBeforeChange(receivers, key, true);
    noteDeletedMembers(receivers, key);
    if (ClimbingSet listed = unlockedObjectsListedBesideUnknownObject(object, receivers, key); holdsAnything(listed)) {
        noteWrittenMembers(listed, key);
        noteMembersLookedForBeforeChange(listed, key, true);
        noteDeletedMembers(listed, key);
    }
    if (ClimbingSet holders = holdersOfReplaceableMemberThatUnknownObjectMayBe(object, receivers, key); holdsAnything(holders)) {
        noteWrittenMembers(holders, key, true);
        noteDeletedMembers(holders, key);
    }
    notePrototypeObjectsThatWriteMayReach(object, receivers, key);
    m_context.mark(m_context.restrict(receivers, m_universe.objects()), ObjectMutation::DeleteField);
    answerBoolean();
    mayThrow();
}

ClimbingSet HeapRule::allocateUnder(const ValueFlowOperand& prototype)
{
    ValueFlowObjectID site = m_universe.siteObject(m_statement);
    Members allocated;
    bool isGivenOtherPrototype = false;
    m_context.forEachMember(m_context.valueOf(prototype), [&](ValueFlowObjectID object) {
        ValueFlowObjectID instance = m_universe.objectAllocatedUnder(m_program, site, object);
        if (instance == site) {
            isGivenOtherPrototype = true;
            return;
        }
        allocated.append(instance);
        add(m_context.written(ClimbingSet(only(instance)), prototypeLinkName), only(object));
    });
    if (isGivenOtherPrototype) {
        allocated.append(site);
        connect(prototype, m_context.written(ClimbingSet(only(site)), prototypeLinkName));
    }
    ClimbingSet objects(m_context.maker().of(allocated.span()));
    add(m_statement.result, objects);
    return objects;
}

void HeapRule::runCreateArguments(const ValueFlowOperand& count, const ValueFlowOperand& rest, std::span<const ValueFlowOperand> parameters, const ValueFlowOperand* callee)
{
    ClimbingSet arguments(only(m_universe.siteObject(m_statement)));
    ValueFlowNodeID contents = m_context.written(arguments, elementsName);
    readOwn(rest, elementsName, contents);
    for (const ValueFlowOperand& parameter : parameters)
        connect(parameter, contents);

    auto propertyNamed = [&](BuiltinName name) {
        return m_context.written(arguments, ValueFlowName::property(m_program.keyOf(name)));
    };
    connect(count, propertyNamed(BuiltinNames::length));
    if (callee)
        connect(*callee, propertyNamed(BuiltinNames::callee));
    BuiltinID iteratorMethod = m_builtins.findOwnMember(m_builtins.at(BuiltinClasses::Array).prototype, BuiltinNames::iteratorSymbol);
    add(propertyNamed(BuiltinNames::iteratorSymbol), iteratorMethod ? only(m_universe.objectOf(iteratorMethod)) : unknownObject(ValueFlowUnknownSource::UndescribedNativeResult));
}

bool HeapRule::readsRestArgumentOfInstance()
{
    namespace At = OperandIndex;
    const Function& function = m_program.at(m_statement.function);
    if (!function.has(FunctionFlags::HasRestParameter) || !m_context.restArgumentOfRun(m_statement.function, 0))
        return false;
    unsigned positional = function.numberOfPositionalParameters();
    if (definitionThroughSameValues(function, operation().operand(At::ElemGet::object)) != function.at(entryBlock).parameters[function.firstParameterIndex() + positional])
        return false;
    ClimbingSet array(only(m_universe.restArrayOf(m_statement.function)));
    constexpr OptionSet<ObjectMutation> everyMutation { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty,
        ObjectMutation::ChangeIntegrity, ObjectMutation::ChangePrototype, ObjectMutation::BecomePrototype, ObjectMutation::StoreAfterCreation, ObjectMutation::StoreOfOtherType };
    if (holdsAnything(m_context.markedAmong(array, everyMutation)) || hasEscaped(array) || hasBeenLost(array))
        return false;

    const Value& key = function.at(definitionThroughSameValues(function, operation().operand(At::ElemGet::property)));
    unsigned numberOfKnownPositions = numberOfGroupedArguments - std::min(numberOfGroupedArguments, positional);
    if (key.opcode != Opcode::Integer || key.integer() < 0 || static_cast<uint64_t>(key.integer()) >= numberOfKnownPositions)
        return false;
    connect(m_context.restArgumentOfRun(m_statement.function, key.integer()), m_statement.result);
    connect(m_context.restArgumentOfRun(m_statement.function, numberOfGroupedArguments), m_statement.result);
    add(m_statement.result, m_universe.undefinedValue());
    return true;
}

bool HeapRule::readsUnderNameOfEnumerationOfSameObject()
{
    namespace At = OperandIndex;
    const Function& function = m_program.at(m_statement.function);
    const Value& name = function.at(definitionThroughTests(function, operation().operand(At::ElemGet::property)));
    if (name.opcode != Opcode::ResultAt || name.index() != 1)
        return false;
    const Value& step = function.at(name.operand(At::ResultAt::results));
    if (step.opcode != Opcode::ForInStep)
        return false;
    return definitionThroughTests(function, step.operand(At::ForInStep::object)) == definitionThroughTests(function, operation().operand(At::ElemGet::object));
}

void HeapRule::runOperation()
{
    namespace At = OperandIndex;
    switch (m_statement.opcode) {
    case Opcode::PropGet:
        read(operand(At::PropGet::object), keyNamed(operation().key()));
        return;
    case Opcode::ElemGet: {
        if (readsRestArgumentOfInstance())
            return;
        ValueSetID& answerOfFirstRun = m_context.rememberedByStatement();
        if (!answerOfFirstRun)
            answerOfFirstRun = readsUnderNameOfEnumerationOfSameObject() ? ValueSets::anything : ValueSets::nothing;
        bool namesEnumerableProperty = answerOfFirstRun == ValueSets::anything;
        forEachKey(operand(At::ElemGet::property), [&](HeapKey key) {
            key.namesEnumerableProperty = namesEnumerableProperty && key.keyClass == HeapKeyClass::Unknown;
            read(receiversUnder(operand(At::ElemGet::object), key), key);
        }, { &operand(At::ElemGet::object), nullptr, true, true });
        mayThrow();
        return;
    }
    case Opcode::PropSet:
        store(operand(At::PropSet::object), keyNamed(operation().key()), operand(At::PropSet::value));
        return;
    case Opcode::ElemSet:
        forEachKey(operand(At::ElemSet::property), [&](const HeapKey& key) {
            store(receiversUnder(operand(At::ElemSet::object), key), key, operand(At::ElemSet::value));
        }, { &operand(At::ElemSet::object), &operand(At::ElemSet::value), true });
        mayThrow();
        return;
    case Opcode::PropDefine:
        define(operand(At::PropDefine::object), keyNamed(operation().key()), HeapPropertyPart::Value, operand(At::PropDefine::value));
        return;
    case Opcode::ElemDefine:
        forEachKey(operand(At::ElemDefine::property), [&](const HeapKey& key) {
            define(receiversUnder(operand(At::ElemDefine::object), key), key, HeapPropertyPart::Value, operand(At::ElemDefine::value));
        }, { &operand(At::ElemDefine::object), &operand(At::ElemDefine::value), true });
        mayThrow();
        return;
    case Opcode::DefineGetter:
        forEachKeyOfOperation(At::DefineGetter::computedKey, [&](const HeapKey& key) {
            define(operand(At::DefineGetter::object), key, HeapPropertyPart::Getter, operand(At::DefineGetter::function));
        });
        return;
    case Opcode::DefineSetter:
        forEachKeyOfOperation(At::DefineSetter::computedKey, [&](const HeapKey& key) {
            define(operand(At::DefineSetter::object), key, HeapPropertyPart::Setter, operand(At::DefineSetter::function));
        });
        return;
    case Opcode::TryDefineLiveBinding:
        for (uint32_t keyIndex : { At::TryDefineLiveBinding::sourceKey, At::TryDefineLiveBinding::targetKey }) {
            forEachKey(operand(keyIndex), [&](const HeapKey& key) {
                define(operand(At::TryDefineLiveBinding::target), key, HeapPropertyPart::Getter, operand(At::TryDefineLiveBinding::getter));
            });
        }
        answerBoolean();
        return;
    case Opcode::TryReExportFields:
        answerBoolean();
        return;
    case Opcode::PropHas:
        test(operand(At::PropHas::object), keyNamed(operation().key()));
        return;
    case Opcode::ElemHas:
        forEachKey(operand(At::ElemHas::property), [&](const HeapKey& key) {
            test(receiversUnder(operand(At::ElemHas::object), key), key);
        }, { &operand(At::ElemHas::object), nullptr, true });
        answerBoolean();
        mayThrow();
        return;
    case Opcode::PropDelete:
        remove(operand(At::PropDelete::object), keyNamed(operation().key()));
        return;
    case Opcode::ElemDelete:
        forEachKey(operand(At::ElemDelete::property), [&](const HeapKey& key) {
            remove(receiversUnder(operand(At::ElemDelete::object), key), key);
        }, { &operand(At::ElemDelete::object), nullptr, true });
        answerBoolean();
        mayThrow();
        return;
    case Opcode::SetFunctionName: {
        ValueFlowOperand text { { }, m_universe.strings(), { } };
        // One key names a function whose objects take their name here alone from the start, as a name that is written does.
        ClimbingSet keys = m_context.valueOf(operand(At::SetFunctionName::computedKey));
        ClimbingSet named = m_context.valueOf(operand(At::SetFunctionName::function));
        bool isNamedByOneKey = (!holdsAnything(keys) || m_context.holdsOneMember(keys)) && (!holdsAnything(named) || m_context.holdsOneMember(named));
        m_context.forEachMember(isNamedByOneKey ? keys : ClimbingSet(), [&](ValueFlowObjectID member) {
            PropertyKeyID key = m_universe.keyOf(member);
            isNamedByOneKey = key && tellsNameOfFunction(m_program.at(key));
        });
        m_context.forEachMember(isNamedByOneKey ? named : ClimbingSet(), [&](ValueFlowObjectID member) {
            FunctionID function = m_universe.closureFunction(member);
            isNamedByOneKey = function && m_tables.isNamedAtOneOperation(function);
        });
        if (isNamedByOneKey) {
            write(named, writtenName(keyNamed(m_program.keyOf(BuiltinNames::name)), HeapPropertyPart::Value), text);
            return;
        }
        m_namesFunctionWhereItIsMade = true;
        define(operand(At::SetFunctionName::function), keyNamed(m_program.keyOf(BuiltinNames::name)), HeapPropertyPart::Value, text);
        m_namesFunctionWhereItIsMade = false;
        return;
    }
    case Opcode::AppendHole:
    case Opcode::GrowNewArray:
        return;
    case Opcode::SetLiteralPrototype: {
        ClimbingSet literals = m_context.valueOf(operand(At::SetLiteralPrototype::object));
        changePrototype(literals, operand(At::SetLiteralPrototype::prototype));
        ValueSetID valuesLeftOut = m_context.maker().intersect(ValueSets::complement(m_universe.objects()), ValueSets::complement(kinds(Primitive::Null)));
        if (m_context.meet(m_context.valueOf(operand(At::SetLiteralPrototype::prototype)), valuesLeftOut)) {
            if (operation().refusesOtherValues())
                mayThrow();
            else
                add(m_context.written(literals, prototypeLinkName), only(m_universe.objectOf(Builtins::ObjectPrototype)));
        }
        return;
    }
    case Opcode::CopyDataProperties:
        copyProperties(operand(At::CopyDataProperties::target), operand(At::CopyDataProperties::source), CopiedProperties::AreDefinedAsValues);
        return;
    case Opcode::CopyObject: {
        const ValueFlowOperand& source = operand(At::CopyObject::source);
        ValueSetID site = m_context.maker().of(m_universe.siteObject(m_statement));
        add(m_context.written(ClimbingSet(site), prototypeLinkName), only(m_universe.objectOf(Builtins::ObjectPrototype)));
        copyProperties({ { }, site, { } }, source, CopiedProperties::AreDefinedAsValues);
        add(m_statement.result, site);
        if (!operation().excludedKeys().empty() || !operation().excludedOperands().empty())
            return;

        ClimbingSet sources = m_context.valueOf(source);
        Members records;
        m_context.forEachMember(known(sources), [&](ValueFlowObjectID object) {
            ValueFlowObjectKind objectKind = m_universe.kindOf(object);
            LayoutID layout = objectKind == ValueFlowObjectKind::Site || objectKind == ValueFlowObjectKind::Instance ? m_universe.layoutOf(object) : LayoutID();
            if (layout && m_program.at(layout).kind == LayoutKind::Record)
                records.append(object);
        });
        add(m_statement.result, m_context.maker().of(records.span()));
        add(m_statement.result, m_context.restrict(known(sources), m_universe.objectsThatMayBeAnything()));
        if (mayBeUnknownObject(sources))
            add(m_statement.result, unknownObjectAlone());
        if (mayBeUntrackedObject(sources))
            add(m_statement.result, m_universe.untrackedObjectAlone());
        return;
    }
    case Opcode::ScopeHasBinding: {
        const ValueFlowOperand& scope = operand(At::ScopeHasBinding::scope);
        HeapKey key = keyNamed(operation().key());
        test(scope, key);
        ValueFlowNodeID unscopables = m_context.lookup(m_context.valueOf(scope), ValueFlowName::property(m_program.keyOf(BuiltinNames::unscopablesSymbol)));
        m_context.lookup(m_context.valueOf(unscopables), foundName(key, HeapPropertyPart::Value));
        return;
    }
    case Opcode::ForInOpen:
        noteTestOfMembers(operand(At::ForInOpen::object), unknownHeapKey);
        [[fallthrough]];
    case Opcode::ForInStep:
        add(m_statement.result, m_universe.maskOf(m_context.maker(), m_program, m_program.at(operation().type)));
        mayThrow();
        return;
    case Opcode::GetSuperBase: {
        ClimbingSet homes = m_context.valueOf(operand(At::GetSuperBase::home));
        if (operation().homeKind() == HomeObjectKind::ClassPrototype)
            homes = m_context.valueOf(m_context.lookup(homes, ValueFlowName::property(m_program.keyOf(BuiltinNames::prototype))));
        readOwn(homes, prototypeLinkName, m_statement.result);
        add(m_statement.result, prototypesAtCreationOf(homes));
        if (mayBeUntrackedObject(homes))
            connect(m_context.lookup(untrackedObjectAlone(), prototypeLinkName), m_statement.result);

        bool everyLinkIsWritten = true;
        m_context.forEachMember(homes, [&](ValueFlowObjectID home) {
            LayoutID layout;
            if (m_universe.kindOf(home) == ValueFlowObjectKind::Closure)
                layout = m_program.at(m_universe.closureFunction(home)).constructedClass;
            else if (m_universe.kindOf(home) == ValueFlowObjectKind::Prototype)
                layout = m_universe.classLayoutOf(home);
            everyLinkIsWritten &= layout && m_program.at(layout).has(LayoutFlags::HasHeritage);
        });
        if (!everyLinkIsWritten)
            add(m_statement.result, kinds(Primitive::Null));
        return;
    }
    case Opcode::SuperGet: {
        ClimbingSet bases = m_context.valueOf(operand(At::SuperGet::base));
        HeapReceiver receiver { &operand(At::SuperGet::receiver), { }, { }, false };
        forEachKeyOfOperation(At::SuperGet::computedKey, [&](const HeapKey& key) {
            forEachStartOfLookup(bases, [&](ClimbingSet start) {
                readThroughChain(start, key, receiver, m_statement.result);
            });
            if (key.keyClass == HeapKeyClass::Unknown)
                readExportsOfListedHolders(operand(At::SuperGet::base), bases);
        });
        mayThrow();
        return;
    }
    case Opcode::SuperSet: {
        ClimbingSet bases = m_context.valueOf(operand(At::SuperSet::base));
        const ValueFlowOperand& receiver = operand(At::SuperSet::receiver);
        const ValueFlowOperand& value = operand(At::SuperSet::value);
        forEachKeyOfOperation(At::SuperSet::computedKey, [&](const HeapKey& key) {
            define(receiver, key, HeapPropertyPart::Value, value);
            ClimbingSet setters;
            forEachStartOfLookup(bases, [&](ClimbingSet start) {
                setters = m_context.unite(setters, accessorsFoundFrom(start, key, HeapPropertyPart::Setter));
            });
            callAccessors(setters, ValueFlowCallKind::Set, { &receiver, { }, { }, false }, &value, { });
        });
        return;
    }
    case Opcode::PrivateGet:
        connect(m_context.nodes().privateField(operation().layout(), operation().field()), m_statement.result);
        mayThrow();
        return;
    case Opcode::PrivateSet:
        connect(operand(At::PrivateSet::value), m_context.nodes().privateField(operation().layout(), operation().field()));
        mayThrow();
        return;
    case Opcode::PrivateDefine: {
        connect(operand(At::PrivateDefine::value), m_context.nodes().privateField(operation().layout(), operation().field()));
        HeapKey key { HeapKeyClass::Name, m_program.at(operation().layout()).fields[operation().field()].key };
        ClimbingSet objects = m_context.valueOf(operand(At::PrivateDefine::object));
        ValueSetID declaring = m_tables.objectsDeclaringField(key.name);
        Members mayLackField;
        m_context.forEachMember(m_context.restrict(objects, m_context.maker().intersect(declaring, m_universe.objectsThatPickTheirLayout())), [&](ValueFlowObjectID object) {
            if (!m_universe.siteOf(object))
                return;
            bool mayLack = false;
            m_context.forEachMember(m_context.valueOf(m_context.read(ClimbingSet(only(object)), prototypeLinkName)), [&](ValueFlowObjectID prototype) {
                LayoutID classOfPrototype = m_universe.classLayoutOf(prototype);
                mayLack |= classOfPrototype && !std::ranges::any_of(m_program.at(classOfPrototype).fields, [&](const Field& field) {
                    return field.key == key.name;
                });
            });
            if (mayLack)
                mayLackField.append(object);
        });
        if (!mayLackField.isEmpty())
            declaring = m_context.maker().intersect(declaring, ValueSets::complement(m_context.maker().of(mayLackField.span())));
        ClimbingSet stamped = m_context.restrict(objects, ValueSets::complement(declaring));
        if (m_context.meet(stamped, m_universe.objects())) {
            ValueFlowName name = writtenName(key, HeapPropertyPart::Value);
            auto writeInto = [&](ClimbingSet objects) {
                ValueFlowNodeID written = m_context.written(objects, name);
                connect(operand(At::PrivateDefine::value), written);
                add(written, m_universe.undefinedValue());
            };
            writeInto(known(stamped));
            if (mayBeUntrackedObject(stamped))
                writeInto(untrackedObjectAlone());
            if (mayBeUnknownObject(stamped))
                writeInto(unknownObjectAlone());
            markStore(stamped, key);
        }
        mayThrow();
        return;
    }
    case Opcode::PrivateHas:
        answerBoolean();
        mayThrow();
        return;
    case Opcode::NewObject: {
        const Value& allocation = operation();
        LayoutID layoutID = allocation.layout();
        const Layout& layout = m_program.at(layoutID);
        ClimbingSet object(only(m_universe.siteObject(m_statement)));

        const ValueFlowOperand& prototype = operand(At::NewObject::prototype);
        if (mayAllocateOtherLayout(m_program, allocation))
            object = allocateUnder(prototype);
        else if (isPassed(prototype))
            connect(prototype, m_context.written(object, prototypeLinkName));

        std::span<const uint32_t> fields = allocation.fields();
        for (size_t index = 0; index < fields.size(); ++index) {
            const ValueFlowOperand& value = operand(At::NewObject::values + index);
            const Field& field = layout.fields[fields[index]];
            if (field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden }))
                connect(value, field.originLayout ? m_context.nodes().field(field.originLayout, *field.originField) : m_context.nodes().field(layoutID, fields[index]));
            else
                connect(value, m_context.written(object, writtenName(keyNamed(field.key), HeapPropertyPart::Value)));
        }
        return;
    }
    case Opcode::NewArray: {
        ClimbingSet array(only(m_universe.siteObject(m_statement)));
        ValueFlowNodeID contents = m_context.written(array, elementsName);
        for (const ValueFlowOperand& element : m_operands.subspan(At::NewArray::elements))
            connect(element, contents);
        return;
    }
    case Opcode::NewTemplateObject: {
        ClimbingSet strings(only(m_universe.siteObject(m_statement)));
        add(m_context.written(strings, elementsName), m_context.maker().unite(m_universe.strings(), m_universe.undefinedValue()));
        add(m_context.written(strings, ValueFlowName::property(m_tables.rawKey())), unknownObject(ValueFlowUnknownSource::ObjectWithoutLayout));
        return;
    }
    case Opcode::AppendElement:
        write(m_context.valueOf(operand(At::AppendElement::array)), elementsName, operand(At::AppendElement::value));
        return;
    case Opcode::CreateUnmappedArguments:
        runCreateArguments(operand(At::CreateUnmappedArguments::count), operand(At::CreateUnmappedArguments::rest), m_operands.subspan(At::CreateUnmappedArguments::parameters), nullptr);
        return;
    case Opcode::CreateMappedArguments:
        runCreateArguments(operand(At::CreateMappedArguments::count), operand(At::CreateMappedArguments::rest), m_operands.subspan(At::CreateMappedArguments::parameters), &operand(At::CreateMappedArguments::callee));
        return;
    case Opcode::GetMappedArgument:
        readOwn(operand(At::GetMappedArgument::arguments), elementsName, m_statement.result);
        add(m_statement.result, m_universe.undefinedValue());
        return;
    case Opcode::SetMappedArgument:
        write(m_context.valueOf(operand(At::SetMappedArgument::arguments)), elementsName, operand(At::SetMappedArgument::value));
        return;
    case Opcode::Data: {
        struct Pending {
            const DataNode* node;
            ValueFlowNodeID holder;
        };
        auto textOf = [&](const DataNode& node) {
            std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = node.as<DataNode::String>().value });
            return key ? m_universe.keyConstant(*key) : ValueFlowUniverse::primitive(PrimitiveSet::String);
        };
        Vector<Pending, 32> pending { Pending { &m_program.at(operation().data()).root, { } } };
        for (uint32_t ordinal = 0; !pending.isEmpty(); ++ordinal) {
            auto [node, holder] = pending.takeLast();
            switch (node->kind()) {
            case DataNode::Kind::Array: {
                ClimbingSet array(only(m_universe.siteObject(m_statement, ordinal)));
                add(holder, array);
                ValueFlowNodeID contents = m_context.written(array, elementsName);
                const Vector<DataNode>& items = node->as<DataNode::Array>().elements;
                Members texts;
                for (size_t index = items.size(); index--;) {
                    bool isText = items[index].kind() == DataNode::Kind::String;
                    if (isText)
                        texts.append(textOf(items[index]));
                    pending.append({ &items[index], isText ? ValueFlowNodeID() : contents });
                }
                add(contents, m_context.maker().of(texts.span()));
                break;
            }
            case DataNode::Kind::Object: {
                ClimbingSet object(only(m_universe.siteObject(m_statement, ordinal)));
                add(holder, object);
                const Vector<DataMember>& members = node->as<DataNode::Object>().members;
                for (size_t index = members.size(); index--;)
                    pending.append({ &members[index].value, m_context.written(object, writtenName(keyNamed(members[index].key), HeapPropertyPart::Value)) });
                break;
            }
            case DataNode::Kind::Undefined:
            case DataNode::Kind::Hole:
                add(holder, m_universe.undefinedValue());
                break;
            case DataNode::Kind::Null:
                add(holder, kinds(Primitive::Null));
                break;
            case DataNode::Kind::Boolean:
                add(holder, kinds(node->as<DataNode::Boolean>().value ? Primitive::True : Primitive::False));
                break;
            case DataNode::Kind::Number:
                add(holder, kinds(static_cast<uint16_t>(primitiveOfNumber(node->as<DataNode::Number>().value))));
                break;
            case DataNode::Kind::String:
                if (holder)
                    add(holder, only(textOf(*node)));
                break;
            case DataNode::Kind::BigInt:
                add(holder, kinds(Primitive::BigInt));
                break;
            }
        }
        return;
    }
    case Opcode::CreateIteratorResult: {
        ClimbingSet result(only(m_universe.siteObject(m_statement)));
        connect(operand(At::CreateIteratorResult::value), m_context.written(result, ValueFlowName::property(m_program.keyOf(BuiltinNames::value))));
        connect(operand(At::CreateIteratorResult::done), m_context.written(result, ValueFlowName::property(m_program.keyOf(BuiltinNames::done))));
        return;
    }
    case Opcode::GetOwnProperty:
        read(operand(At::GetOwnProperty::object), keyNamed(operation().key()));
        return;
    case Opcode::GetByValWithThis: {
        ClimbingSet holders = m_context.valueOf(operand(At::GetByValWithThis::object));
        HeapReceiver receiver { &operand(At::GetByValWithThis::thisValue), { }, { }, false };
        forEachKey(operand(At::GetByValWithThis::property), [&](const HeapKey& key) {
            forEachStartOfLookup(holders, [&](ClimbingSet start) {
                readThroughChain(start, key, receiver, m_statement.result);
            });
        });
        mayThrow();
        return;
    }
    case Opcode::GetContiguousIndex:
        readOwn(operand(At::GetContiguousIndex::array), elementsName, m_statement.result);
        add(m_statement.result, m_universe.undefinedValue());
        return;
    case Opcode::GetDoubleIndex:
        add(m_statement.result, kinds(Primitive::Number));
        return;
    case Opcode::PutContiguousIndex:
        write(m_context.valueOf(operand(At::PutContiguousIndex::array)), elementsName, operand(At::PutContiguousIndex::value));
        return;
    case Opcode::TryPutByValDirect:
        write(m_context.valueOf(operand(At::TryPutByValDirect::array)), elementsName, operand(At::TryPutByValDirect::value));
        answerBoolean();
        return;
    case Opcode::TryPutIndex:
        write(m_context.valueOf(operand(At::TryPutIndex::array)), elementsName, operand(At::TryPutIndex::value));
        answerBoolean();
        return;
    case Opcode::TryPutDoubleIndex:
        write(m_context.valueOf(operand(At::TryPutDoubleIndex::array)), elementsName, operand(At::TryPutDoubleIndex::value));
        answerBoolean();
        return;
    case Opcode::PutDoubleIndex:
        write(m_context.valueOf(operand(At::PutDoubleIndex::array)), elementsName, operand(At::PutDoubleIndex::value));
        return;
    case Opcode::TruncateNewArray: {
        const ValueFlowOperand& array = operand(At::TruncateNewArray::array);
        connect(array, m_statement.result);
        if (ValueFlowObjectID site = m_universe.siteObject(m_statement))
            readOwn(array, elementsName, m_context.written(ClimbingSet(only(site)), elementsName));
        return;
    }
    case Opcode::GetInternalField:
        readOwn(operand(At::GetInternalField::object), internalFieldsName, m_statement.result);
        add(m_statement.result, m_universe.anyPrimitive());
        return;
    case Opcode::PutInternalField:
        write(m_context.valueOf(operand(At::PutInternalField::object)), internalFieldsName, operand(At::PutInternalField::value));
        connect(operand(At::PutInternalField::value), m_statement.result);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void runHeapOperation(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    if (!context.isReached(statement.function))
        return;
    HeapRule rule(statement, context);
    rule.askWithoutNodesWhereNothingIsWritten();
    rule.runOperation();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
