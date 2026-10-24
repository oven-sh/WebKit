/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTKnownClosures.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

ValueSetID HeapRule::kinds(uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return m_context.maker().of(members.span());
}

void HeapRule::connect(ValueFlowNodeID from, ValueFlowNodeID to)
{
    if (from && to)
        m_context.connect(from, to);
}

void HeapRule::connect(const ValueFlowOperand& from, ValueFlowNodeID to)
{
    if (to)
        m_context.connect(from, to);
}

// The mask of an operand may put the objects of a family in the place of a placeholder, so it does not make one mask with another.
void HeapRule::connectThrough(ValueSetID mask, const ValueFlowOperand& from, ValueFlowNodeID to)
{
    if (!to)
        return;
    if (from.node)
        m_context.connect(m_context.masked(m_context.masked(from.node, from.mask, from.maskKind), mask), to);
    m_context.add(to, m_context.restrict(m_context.valueOf(ValueFlowOperand { { }, from.constant, from.mask, from.maskKind }), mask));
}

void HeapRule::add(ValueFlowNodeID to, ValueSetID constant)
{
    if (to)
        m_context.add(to, constant);
}

void HeapRule::add(ValueFlowNodeID to, ClimbingSet set)
{
    if (to)
        m_context.add(to, set);
}

void HeapRule::pass(const HeapReceiver& receiver, ValueFlowNodeID to)
{
    if (receiver.operand)
        connect(*receiver.operand, to);
    else
        add(to, receiver.set);
}

// What is handed over is in unknown hands from then on.
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
    // The chain of the untracked object is looked up object by object, and that lets each object escape above which such an accessor may lie. So the untracked object is not handed over for all of them.
    if (mayBeUntrackedObject(m_context.valueOf(*receiver.operand)))
        m_context.lookup(untrackedObjectAlone(), prototypeLinkName);
    connectThrough(m_context.maker().intersect(mask, ValueSets::complement(m_universe.untrackedObjectAlone())), *receiver.operand, escaped);
}

HeapKey HeapRule::keyNamed(PropertyKeyID name) const
{
    if (!name)
        return unknownHeapKey;
    return m_program.isIndexLike(name) ? indexHeapKey : HeapKey { HeapKeyClass::Name, name };
}

// A symbol is no text and no index.
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
    // A declared or an expected type keeps nothing out.
    bool keepsOut = object.mask && (object.maskKind == ValueFlowMaskKind::Plain || object.maskKind == ValueFlowMaskKind::Checked);
    receivers.mask = keepsOut ? maker.intersect(object.mask, meant) : meant;
    if (!keepsOut)
        receivers.maskKind = ValueFlowMaskKind::Plain;
    return receivers;
}

void HeapRule::read(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    if (m_accessorFunctor) {
        forEachStartOfLookup(receivers, [&](ClimbingSet start) {
            readThroughChain(start, key, { &object, { } }, m_statement.result);
        });
        return;
    }

    // See namesObjectOfAccess().
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (namesObjectOfAccess() && isNameThatTableDoesNotListOnGlobalObject(key) && m_context.contains(receivers, globalObject)) {
        readThroughChain(ClimbingSet(only(globalObject)), key, { &object, { } }, m_statement.result);
        receivers = m_context.restrict(receivers, ValueSets::complement(only(globalObject)));
    }

    // The library's code behind a described built-in is entered with operands nobody knows.
    if (mayBeUnknownObject(receivers) && m_statement.function && isCodeThatRecordsDescribe(m_universe, m_statement.function)) {
        add(m_statement.result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        // A value nobody knows is the unknown object or any primitive, and above a primitive lies the prototype object of its wrapper, with every method of strings and numbers.
        receivers = m_context.restrict(receivers, ValueSets::complement(m_context.maker().unite(m_universe.unknownObjectAlone(), m_universe.anyPrimitive())));
    }

    // A script that nobody has seen declares a `let` under the name of a global, and every free name finds that first.
    if (!namesObjectOfAccess() && key.keyClass == HeapKeyClass::Name && m_context.contains(receivers, globalObject) && doesUnseenCodeRun()) {
        forEachMemberUnder(ClimbingSet(only(globalObject)), key, [&](BuiltinID member, ModuleID) {
            if (m_builtins.at(member).mutability == BuiltinMutability::ChangesOnItsOwn)
                add(m_statement.result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        });
    }

    ValueFlowName name = nameOfLookupForValue(key);
    forEachStartOfLookup(receivers, [&](ClimbingSet start) {
        connect(m_context.lookup(start, name), m_statement.result);
    });
    if (key.keyClass == HeapKeyClass::Unknown) {
        ClimbingSet listed = hostObjectsListedBesideUnknownObject(object, receivers);
        if (holdsAnything(listed))
            connect(m_context.lookup(listed, name), m_statement.result);
        readExportsOfListedHolders(object, receivers);
    }
    // The unknown object answers with no member of Bun: see isMemberOfBun(). Where the text names a function of Bun, as in `(await import("bun")).peek`, a call of what is read applies it: see runCall(). Whoever hands it on,
    // as `peek.call()` does, has it listed.
    if (key.keyClass == HeapKeyClass::Name && mayBeUnknownObject(receivers) && m_statement.function && m_statement.site && m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::Bun))) {
        BuiltinID named = entryNamedByOperations(m_program, m_program.at(m_statement.function), m_statement.site);
        bool isNamed = named && isMemberOfBun(named);
        ClimbingSet bun(only(m_universe.objectOf(Builtins::Bun)));
        if (isNamed && m_context.sets().contains(m_universe.builtinFunctions(), m_universe.objectOf(named))) {
            use(named, BuiltinUse::Read);
            add(m_statement.result, only(m_universe.objectOf(named)));
        } else if (isNamed || (object.node && m_context.holdsAnyOf(bun, object.node))) {
            // Beside the unknown object the normal form leaves out Bun once it has escaped. The read binds to it all the same.
            connect(m_context.lookup(bun, name), m_statement.result);
        }
    }
    // And for whoever goes on to a prototype object: `globalThis[name].prototype` answers the prototype object of a constructor that the operand lists, though the constructor has escaped.
    if (key.keyClass == HeapKeyClass::Name && key.name == m_tables.prototypeKey() && object.node && mayBeUnknownObject(receivers)) {
        ClimbingSet listed = m_context.valueWithin(ClimbingSet(m_tables.under(key.name).holders), object.node);
        if (holdsAnything(listed)) {
            ValueFlowNodeID found = m_context.lookup(listed, name);
            connect(found, m_statement.result);
            add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.restrict(m_context.valueOf(found), m_tables.prototypeObjectsThatListingDoesNotName()));
        }
    }
    readEvaluators(object, key);
    handOutPrototypeObjectsAtCreation(receivers, key);
    mayThrow();
}

// What lies in the object itself, where no accessor can stand and nothing is inherited.
void HeapRule::readOwn(ClimbingSet holders, ValueFlowName name, ValueFlowNodeID result)
{
    ClimbingSet objects = known(holders);
    if (name == prototypeLinkName)
        objects = m_context.restrict(objects, ValueSets::complement(m_tables.holdersWithLockedPrototypeLink()));
    auto answer = [&](ValueFlowNodeID node) {
        connect(node, result);
    };
    answer(m_context.read(objects, name));
    forEachNodeStoredThroughPlaceholdersInto(objects, name, answer);
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

// The host makes many of its members when they are first looked for, whoever looks and whatever for: on an object that the operation names, a test needs the code that makes the member as a read does.
void HeapRule::noteTestOfMembers(const ValueFlowOperand& object, const HeapKey& key)
{
    ClimbingSet receivers = m_context.valueOf(object);
    HeapChain chain = chainFrom(receivers);
    auto noteTest = [&](BuiltinID member, ModuleID) {
        use(member, BuiltinUse::Tested);
    };
    auto noteTestThatNamesHolder = [&](BuiltinID member, ModuleID) {
        // `key in process` names no way to the module objects: in an image whose code does not name process.mainModule the host makes undefined of it, and runs nothing for that.
        bool isNamed = key.keyClass == HeapKeyClass::Name || (!isFoundByNameAlone(m_builtins.canonicalID(member)) && !isMemberOfBun(member));
        bool isMadeByTest = isNamed && isMadeWithCodeOfLibrary(m_builtins, member) && !m_tables.letsInUnseenCode(member);
        use(member, isMadeByTest ? OptionSet<BuiltinUse> { BuiltinUse::Tested, BuiltinUse::Read } : OptionSet<BuiltinUse> { BuiltinUse::Tested });
    };
    // Under a key nobody knows every member of every holder is noted, and what is noted follows from the holder alone: once a thread is enough.
    auto notNotedYet = [&](ClimbingSet holders, unsigned number) {
        return key.keyClass == HeapKeyClass::Unknown ? m_context.membersNewToThread(m_context.restrict(holders, m_tables.holdersWithMembers()), number) : holders;
    };
    forEachMemberUnder(notNotedYet(chain.objects, 0), key, noteTestThatNamesHolder);
    forEachMemberUnder(notNotedYet(hostObjectsListedBesideUnknownObject(object, receivers), 0), key, noteTestThatNamesHolder);
    if (chain.holdsUntrackedObject)
        forEachMemberUnder(notNotedYet(foundThroughUntrackedObject(holdersWithMemberUnder(key)), 1), key, noteTest);
    if (chain.holdsUnknownObject) {
        forEachMemberUnder(notNotedYet(holdersFoundThroughUnknownObject(key), 1), key, noteTest);
        noteTrapOfUnknownObject(unknownObjectAlone(), ValueFlowCallKind::Get);
    }
}

// What is stored through a placeholder lies in one node for each name.
void HeapRule::write(ClimbingSet objects, ValueFlowName name, const ValueFlowOperand& value)
{
    connect(value, m_context.written(known(objects), name));
    bool mayBeUntracked = mayBeUntrackedObject(objects);
    if (mayBeUntracked)
        connect(value, m_context.written(untrackedObjectAlone(), name));
    // Native code reaches the built-in objects of the realm without being handed them, and it reads and calls what the program puts there.
    bool writesIntoRealm = mayBeUntracked && hasBeenLost(ClimbingSet(m_tables.objectsOfRealm()));
    bool namesProperty = name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter;
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
        switch (name.kind) {
        case ValueFlowNameKind::Property: {
            const ValueFlowNameReadByNativeCode* read = listed->find(PropertyKeyID(name.payload));
            writesIntoRealm |= read && !read->isOnlyInspected;
            return;
        }
        case ValueFlowNameKind::Getter:
        case ValueFlowNameKind::Setter:
            writesIntoRealm |= !!listed->find(PropertyKeyID(name.payload));
            return;
        case ValueFlowNameKind::PrototypeLink:
            for (const ValueFlowNameReadByNativeCode& read : listed->names)
                letNativeCodeRead(ClimbingSet(listed->entries), read);
            return;
        case ValueFlowNameKind::Elements:
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::GetterUnderUnknownKey:
        case ValueFlowNameKind::SetterUnderUnknownKey:
        case ValueFlowNameKind::EveryProperty:
        case ValueFlowNameKind::EveryGetter:
        case ValueFlowNameKind::EverySetter:
        case ValueFlowNameKind::CopiedFrom:
        case ValueFlowNameKind::DeletedMember:
        case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
        case ValueFlowNameKind::RedefinedMember:
        case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
        case ValueFlowNameKind::PrivateField:
        case ValueFlowNameKind::Slot:
        case ValueFlowNameKind::HiddenSlot:
        case ValueFlowNameKind::InternalFields:
        case ValueFlowNameKind::SteppedValues:
        case ValueFlowNameKind::ReturnedValue:
        case ValueFlowNameKind::SentValues:
        case ValueFlowNameKind::OpenedBy:
        case ValueFlowNameKind::ResolvedPromises:
        case ValueFlowNameKind::RejectedPromises:
        case ValueFlowNameKind::AllocatedAs:
        case ValueFlowNameKind::OpenedIterators:
        case ValueFlowNameKind::IteratedValues:
        case ValueFlowNameKind::IteratorResults:
        case ValueFlowNameKind::AwaitedValue:
        case ValueFlowNameKind::MemberWritten:
        case ValueFlowNameKind::EveryMemberWritten:
        case ValueFlowNameKind::EveryMemberRead:
        case ValueFlowNameKind::RestArgument:
        case ValueFlowNameKind::AddedNames:
        case ValueFlowNameKind::FoundThroughEscapedInstances:
            writesIntoRealm = true;
            return;
        }
    });
    if (writesIntoRealm)
        connect(value, ValueFlowProgramNodes::escaped());
    if (!mayBeUnknownObject(objects))
        return;
    connect(value, m_context.written(unknownObjectAlone(), name));
    connect(value, ValueFlowProgramNodes::escaped());
    add(m_context.nodes().ofHost(ValueFlowHostNode::ObjectNobodyKnowsIsWritten), m_universe.unknownObjectAlone());
}

// A definition and a deletion look for the property before they change it, and the host makes many of its members when they are first looked for: see noteTestOfMembers().
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

// Nearly every store goes into objects of the program alone, and then there is nothing to note.
void HeapRule::noteWrittenMembers(ClimbingSet receivers, const HeapKey& key, bool areOnlyListed)
{
    if (!m_context.meet(receivers, m_tables.builtinObjectsAndPlaceholders()))
        return;
    // Packages store under computed keys through operands that list hundreds of objects of the language, nearly all of whose members the table locks. Of what is only listed, the objects of the host count.
    ValueSetID counted = areOnlyListed ? m_tables.holdersOfMembersThatHostMakes() : m_universe.builtinObjects();
    if (ClimbingSet written = m_context.restrict(known(receivers), counted); holdsAnything(written))
        add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsThatStatementsWrite), written);

    ValueSetID holders = ValueSets::nothing;
    ValueFlowName name = ValueFlowName::of(ValueFlowNameKind::EveryMemberWritten);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        holders = m_tables.under(key.name).holders;
        name = { ValueFlowNameKind::MemberWritten, key.name.index() };
        break;
    case HeapKeyClass::Index:
        break;
    case HeapKeyClass::Unknown:
        holders = m_tables.holdersWithMembers();
        break;
    }
    if (holders != ValueSets::nothing) {
        ClimbingSet written = m_context.restrict(receivers, m_context.maker().unite(holders, m_universe.placeholdersAlone()));
        if (holdsAnything(written))
            m_context.lookup(written, name);
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

// The universe is made before anybody knows the callee of a call, so what a built-in allocates is a sure instance of nothing there.
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

// A store under an index adds no name to an array or to a typed array, and neither does a store under a name that the layout of the object declares or that the object has for life.
void HeapRule::markStore(ClimbingSet receivers, const HeapKey& key, OptionSet<ObjectMutation> otherMarks)
{
    ClimbingSet objects = m_context.restrict(receivers, m_universe.objects());
    m_context.mark(objects, otherMarks | ObjectMutation::StoreAfterCreation);
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
    // Whoever lists the names of an object of the program asks for them.
    ClimbingSet objectsOfProgram = m_context.restrict(objects, m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects())));
    if (holdsAnything(objectsOfProgram))
        add(m_context.written(objectsOfProgram, addedNamesName), key.keyClass == HeapKeyClass::Name ? only(m_universe.keyConstant(key.name)) : m_tables.stringAndSymbolKinds());
}

// A value that is defined where a value lies leaves the property what it was. It makes another kind of property of an accessor, and so does an accessor of whatever was there.
void HeapRule::markDefinition(ClimbingSet receivers, const HeapKey& key, HeapPropertyPart part)
{
    ClimbingSet objects = known(receivers);
    auto mayHave = [&](HeapPropertyPart accessor) {
        ValueFlowName name = foundName(key, accessor);
        return holdsAnything(readWhereWritten(objects, name)) || storedThroughUnknownObjectInto(objects, name) || storedThroughUntrackedObjectInto(objects, name);
    };

    bool redefines = part != HeapPropertyPart::Value || m_context.meet(receivers, m_universe.placeholdersAlone()) || mayHave(HeapPropertyPart::Getter) || mayHave(HeapPropertyPart::Setter);
    markStore(receivers, key, { });
    if (redefines)
        markRedefinition(receivers, key);
}

// The results about the fields of an object of the program ask under which keys: a redefinition that names its key leaves every other field what it was.
void HeapRule::markRedefinition(ClimbingSet receivers, const HeapKey& key)
{
    m_context.mark(m_context.restrict(receivers, m_universe.objects()), ObjectMutation::RedefineProperty);
    ValueFlowName name = ValueFlowName::of(ValueFlowNameKind::RedefinedMemberUnderUnknownKey);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        name = { ValueFlowNameKind::RedefinedMember, key.name.index() };
        break;
    case HeapKeyClass::Index:
        return;
    case HeapKeyClass::Unknown:
        break;
    }
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
        // Such an accessor is handed the object it lies on sooner or later. So whatever an accessor nobody knows lies on has escaped, and a lookup that finds one only asks who inherits it.
        connect(object, ValueFlowProgramNodes::escaped());
    }
    noteWrittenMembers(receivers, key);
    noteMembersLookedForBeforeChange(receivers, key);
    ClimbingSet listed = unlockedObjectsListedBesideUnknownObject(object, receivers, key);
    if (holdsAnything(listed)) {
        write(listed, writtenName(key, part), value);
        noteWrittenMembers(listed, key);
        noteMembersLookedForBeforeChange(listed, key);
    }
    notePrototypeObjectsListedAtWrite(object, receivers, key);
    noteTrapOfUnknownObject(receivers, ValueFlowCallKind::Set);
    markDefinition(receivers, key, part);
    mayThrow();
}

// A typed array keeps a number under an index and an array keeps a number as its length.
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

// See namesObjectOfAccess(): the lookup answers as for a free name, with a setter of native code that nothing describes.
ClimbingSet HeapRule::settersFoundFrom(ClimbingSet start, const HeapKey& key)
{
    ValueFlowName name = foundName(key, HeapPropertyPart::Setter);
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (!namesObjectOfAccess() || !isNameThatTableDoesNotListOnGlobalObject(key) || !m_context.contains(start, globalObject))
        return m_context.valueOf(m_context.lookup(start, name));

    ClimbingSet alone(only(globalObject));
    ClimbingSet setters = m_context.valueOf(m_context.lookup(alone, name));
    if (!mayHoldUndeclaredProperty(chainFrom(alone, key).objects, key))
        setters = m_context.restrict(setters, ValueSets::complement(only(m_tables.undescribedNativeCode())));
    ClimbingSet others = m_context.restrict(start, ValueSets::complement(only(globalObject)));
    return holdsAnything(others) ? m_context.unite(setters, m_context.valueOf(m_context.lookup(others, name))) : setters;
}

void HeapRule::callSetters(ClimbingSet receivers, const HeapKey& key, const ValueFlowOperand& receiver, const ValueFlowOperand& value, bool receiverIsStartOfLookup)
{
    ClimbingSet setters;
    forEachStartOfLookup(receivers, [&](ClimbingSet start) {
        setters = m_context.unite(setters, settersFoundFrom(start, key));
    });
    callAccessors(setters, ValueFlowCallKind::Set, { &receiver, { }, { }, receiverIsStartOfLookup }, &value, { });
}

// As for a read: only a receiver above which a setter nobody knows may lie is its `this`.
void HeapRule::callSettersOfReceivers(ClimbingSet start, const HeapKey& key, const ValueFlowOperand& value)
{
    ClimbingSet setters = settersFoundFrom(start, key);
    HeapReceiver receiver { nullptr, start };
    if (mayBeAccessorNobodyKnows(setters))
        receiver.belowAccessorNobodyKnows = receiversBelowAccessorNobodyKnows(chainFrom(start, key).start, key, HeapPropertyPart::Setter);
    callAccessors(setters, ValueFlowCallKind::Set, receiver, &value, { });
}

// An operand nobody knows lists the prototype objects of the language, whoever wrote the statement and whatever for: they lie above whatever it may hold.
ValueSetID HeapRule::objectsThatListingNamesUnder(const HeapKey& key)
{
    return key.keyClass == HeapKeyClass::Name ? ValueSets::anything : ValueSets::complement(m_tables.prototypeObjectsThatListingDoesNotName());
}

// Beside the unknown object the normal form leaves out whatever has escaped.
ClimbingSet HeapRule::builtinObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    if (!object.node || !mayBeUnknownObject(receivers) || key.receivers == HeapKeyReceivers::AllButBuiltinObjects)
        return { };
    return m_context.valueWithin(ClimbingSet(m_context.maker().intersect(m_universe.builtinObjects(), objectsThatListingNamesUnder(key))), object.node);
}

// One of the prototype objects that a listing does not name may still be the very object that the statement writes: `globalThis[name].prototype[key] = f`.
void HeapRule::notePrototypeObjectsListedAtWrite(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    if (!object.node || key.keyClass != HeapKeyClass::Unknown || !mayBeUnknownObject(receivers) || key.receivers == HeapKeyReceivers::AllButBuiltinObjects)
        return;
    add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), m_context.valueWithin(ClimbingSet(m_tables.prototypeObjectsThatListingDoesNotName()), object.node));
}

// A definition and a deletion through the unknown object are refused there too.
ClimbingSet HeapRule::unlockedObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers, const HeapKey& key)
{
    if (!object.node || !mayBeUnknownObject(receivers) || key.receivers == HeapKeyReceivers::AllButBuiltinObjects)
        return { };
    // Whoever asks is due again when one of the objects that the operand lists arrives there, not when any object does.
    ClimbingSet listed = m_context.valueWithin(ClimbingSet(m_context.maker().intersect(m_universe.builtinObjects(), objectsThatListingNamesUnder(key))), object.node);
    if (!holdsAnything(listed))
        return { };
    return m_context.valueWithin(listed, m_context.nodes().ofHost(ValueFlowHostNode::BuiltinObjectsThatStatementsWrite));
}

// The same for whoever walks: `unknown[key]` names no member of the host, but a host object that the operand lists is walked by the program, in code that is compiled.
ClimbingSet HeapRule::hostObjectsListedBesideUnknownObject(const ValueFlowOperand& object, ClimbingSet receivers)
{
    if (!object.node || !mayBeUnknownObject(receivers))
        return { };
    return m_context.valueWithin(ClimbingSet(m_tables.holdersOfMembersThatHostMakes()), object.node);
}

bool HeapRule::isInProgramCode()
{
    // What a built-in does to its operand has no statement of its own: it is done where the built-in is called.
    FunctionID function = m_statement.function ? m_statement.function : m_context.functionOfStatement();
    if (!function)
        return false;
    FunctionRole role = m_program.at(function).role;
    return role != FunctionRole::Library && role != FunctionRole::Model;
}

ClimbingSet HeapRule::holdersOfExportsListedBesideUnknownObjectInProgram(const ValueFlowOperand& object, ClimbingSet receivers)
{
    if (!object.node || !mayBeUnknownObject(receivers) || !isInProgramCode())
        return { };
    return m_context.valueWithin(ClimbingSet(m_universe.holdersOfLibraryExports()), object.node);
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
    ClimbingSet listed = builtinObjectsListedBesideUnknownObject(object, receivers, key);
    if (holdsAnything(listed)) {
        write(listed, writtenName(key, HeapPropertyPart::Value), value);
        noteWrittenMembers(listed, key, true);
    }
    notePrototypeObjectsListedAtWrite(object, receivers, key);
    markStore(receivers, key, { });
    forEachStartOfLookup(receivers, [&](ClimbingSet start) {
        callSettersOfReceivers(start, key, value);
    });
    noteTrapOfUnknownObject(receivers, ValueFlowCallKind::Set);
    mayThrow();
}

// A lookup that starts at a built-in holder asks for this. Of an object of the program the results about its fields ask: a deletion that names its key takes no other field out of the object.
void HeapRule::noteDeletedMembers(ClimbingSet receivers, const HeapKey& key)
{
    ValueSetID objectsOfProgram = m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
    ValueSetID holders = ValueSets::nothing;
    ValueFlowName name = ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey);
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        holders = m_tables.holdersThatAnswerUnder(key.name);
        name = { ValueFlowNameKind::DeletedMember, key.name.index() };
        break;
    case HeapKeyClass::Index:
        return;
    case HeapKeyClass::Unknown:
        holders = m_tables.holdersThatAnswerUnderSomeKey();
        break;
    }
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
    ClimbingSet listed = unlockedObjectsListedBesideUnknownObject(object, receivers, key);
    if (holdsAnything(listed)) {
        noteWrittenMembers(listed, key);
        noteMembersLookedForBeforeChange(listed, key, true);
        noteDeletedMembers(listed, key);
    }
    notePrototypeObjectsListedAtWrite(object, receivers, key);
    noteTrapOfUnknownObject(receivers, ValueFlowCallKind::Set);
    m_context.mark(m_context.restrict(receivers, m_universe.objects()), ObjectMutation::DeleteField);
    answerBoolean();
    mayThrow();
}

// The constructor of a base class allocates the instances of the classes below it too, and the prototype that it is given picks the class.
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

void HeapRule::runNewObject()
{
    const Value& allocation = operation();
    LayoutID layoutID = allocation.layout();
    const Layout& layout = m_program.at(layoutID);
    ClimbingSet object(only(m_universe.siteObject(m_statement)));

    const ValueFlowOperand& prototype = operand(OperandIndex::NewObject::prototype);
    if (mayAllocateOtherLayout(m_program, allocation))
        object = allocateUnder(prototype);
    else if (isPassed(prototype))
        connect(prototype, m_context.written(object, prototypeLinkName));

    std::span<const uint32_t> fields = allocation.fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        const ValueFlowOperand& value = operand(OperandIndex::NewObject::values + index);
        const Field& field = layout.fields[fields[index]];
        if (field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden }))
            connect(value, field.originLayout ? m_context.nodes().field(field.originLayout, *field.originField) : m_context.nodes().field(layoutID, fields[index]));
        else
            connect(value, m_context.written(object, writtenName(keyNamed(field.key), HeapPropertyPart::Value)));
    }
}

// The copy of a record is a record with the same layout: operationAOTCopyObject().
void HeapRule::runCopyObject()
{
    const ValueFlowOperand& source = operand(OperandIndex::CopyObject::source);
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
    if (mayBeUnknownObject(sources))
        add(m_statement.result, unknownObjectAlone());
    if (mayBeUntrackedObject(sources))
        add(m_statement.result, m_universe.untrackedObjectAlone());
}

void HeapRule::runNewArray()
{
    ClimbingSet array(only(m_universe.siteObject(m_statement)));
    ValueFlowNodeID contents = m_context.written(array, elementsName);
    for (const ValueFlowOperand& element : m_operands.subspan(OperandIndex::NewArray::elements))
        connect(element, contents);
}

// No statement of the program fills a template object.
void HeapRule::runNewTemplateObject()
{
    ClimbingSet strings(only(m_universe.siteObject(m_statement)));
    add(m_context.written(strings, elementsName), m_context.maker().unite(m_universe.strings(), m_universe.undefinedValue()));
    add(m_context.written(strings, ValueFlowName::property(m_tables.rawKey())), unknownObject(ValueFlowUnknownSource::ObjectWithoutLayout));
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

void HeapRule::runData()
{
    struct Pending {
        const DataNode* node;
        ValueFlowNodeID holder;
    };
    // The sites are numbered in preorder, and a primitive takes a number too.
    Vector<Pending, 32> pending { Pending { &m_program.at(operation().data()).root, { } } };
    for (uint32_t ordinal = 0; !pending.isEmpty(); ++ordinal) {
        auto [node, holder] = pending.takeLast();
        switch (node->kind()) {
        case DataNode::Kind::Array: {
            ClimbingSet array(only(m_universe.siteObject(m_statement, ordinal)));
            add(holder, array);
            ValueFlowNodeID contents = m_context.written(array, elementsName);
            const Vector<DataNode>& items = node->as<DataNode::Array>().elements;
            for (size_t index = items.size(); index--;)
                pending.append({ &items[index], contents });
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
            add(holder, kinds(Primitive::String));
            break;
        case DataNode::Kind::BigInt:
            add(holder, kinds(Primitive::BigInt));
            break;
        }
    }
}

// The result is the array, whether or not the bodies take it for an allocation of its own.
void HeapRule::runTruncateNewArray()
{
    const ValueFlowOperand& array = operand(OperandIndex::TruncateNewArray::array);
    connect(array, m_statement.result);
    if (ValueFlowObjectID site = m_universe.siteObject(m_statement))
        readOwn(array, elementsName, m_context.written(ClimbingSet(only(site)), elementsName));
}

void HeapRule::runCreateIteratorResult()
{
    ClimbingSet result(only(m_universe.siteObject(m_statement)));
    connect(operand(OperandIndex::CreateIteratorResult::value), m_context.written(result, ValueFlowName::property(m_program.keyOf(BuiltinNames::value))));
    connect(operand(OperandIndex::CreateIteratorResult::done), m_context.written(result, ValueFlowName::property(m_program.keyOf(BuiltinNames::done))));
}

void HeapRule::runGetByValWithThis()
{
    ClimbingSet holders = m_context.valueOf(operand(OperandIndex::GetByValWithThis::object));
    HeapReceiver receiver { &operand(OperandIndex::GetByValWithThis::thisValue), { }, { }, false };
    forEachKey(operand(OperandIndex::GetByValWithThis::property), [&](const HeapKey& key) {
        forEachStartOfLookup(holders, [&](ClimbingSet start) {
            readThroughChain(start, key, receiver, m_statement.result);
        });
    });
    mayThrow();
}

// `with (scope)`: whether the scope has the name, and whether the object under Symbol.unscopables hides it.
void HeapRule::runScopeHasBinding()
{
    const ValueFlowOperand& scope = operand(OperandIndex::ScopeHasBinding::scope);
    HeapKey key = keyNamed(operation().key());
    test(scope, key);
    ValueFlowNodeID unscopables = m_context.lookup(m_context.valueOf(scope), ValueFlowName::property(m_program.keyOf(BuiltinNames::unscopablesSymbol)));
    m_context.lookup(m_context.valueOf(unscopables), foundName(key, HeapPropertyPart::Value));
}

void HeapRule::runGetSuperBase()
{
    ClimbingSet homes = m_context.valueOf(operand(OperandIndex::GetSuperBase::home));
    // The operand is the class, and the home object of a method is what the class holds under `prototype`.
    if (operation().homeKind() == HomeObjectKind::ClassPrototype)
        homes = m_context.valueOf(m_context.lookup(homes, ValueFlowName::property(m_program.keyOf(BuiltinNames::prototype))));
    readOwn(homes, prototypeLinkName, m_statement.result);
    add(m_statement.result, prototypesAtCreationOf(homes));
    if (mayBeUntrackedObject(homes))
        connect(m_context.lookup(untrackedObjectAlone(), prototypeLinkName), m_statement.result);

    // A class with a heritage and its prototype object have their links from NewClass, null among them, and whoever changes a link writes it. Of any other object nobody says here whether it was born without one.
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
}

void HeapRule::runSuperGet()
{
    ClimbingSet bases = m_context.valueOf(operand(OperandIndex::SuperGet::base));
    HeapReceiver receiver { &operand(OperandIndex::SuperGet::receiver), { }, { }, false };
    forEachKeyOfOperation(OperandIndex::SuperGet::computedKey, [&](const HeapKey& key) {
        forEachStartOfLookup(bases, [&](ClimbingSet start) {
            readThroughChain(start, key, receiver, m_statement.result);
        });
        if (key.keyClass == HeapKeyClass::Unknown)
            readExportsOfListedHolders(operand(OperandIndex::SuperGet::base), bases);
    });
    mayThrow();
}

void HeapRule::runSuperSet()
{
    ClimbingSet bases = m_context.valueOf(operand(OperandIndex::SuperSet::base));
    const ValueFlowOperand& receiver = operand(OperandIndex::SuperSet::receiver);
    const ValueFlowOperand& value = operand(OperandIndex::SuperSet::value);
    forEachKeyOfOperation(OperandIndex::SuperSet::computedKey, [&](const HeapKey& key) {
        define(receiver, key, HeapPropertyPart::Value, value);
        callSetters(bases, key, receiver, value, false);
    });
}

// `args[1]` in an instance of `function (type, ...args)`.
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

// `for (const name in object) .. object[name]`, the object being one value of the function. A test between the step and the read hands on the same value: `if (name === "emitNode") continue;`.
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
        // It follows from the operations of the function alone.
        ValueSetID& answerOfFirstRun = m_context.rememberedByStatement();
        if (!answerOfFirstRun)
            answerOfFirstRun = readsUnderNameOfEnumerationOfSameObject() ? ValueSets::anything : ValueSets::nothing;
        bool namesEnumerableProperty = answerOfFirstRun == ValueSets::anything;
        forEachKey(operand(At::ElemGet::property), [&](HeapKey key) {
            key.namesEnumerableProperty = namesEnumerableProperty && key.keyClass == HeapKeyClass::Unknown;
            read(receiversUnder(operand(At::ElemGet::object), key), key);
        }, { &operand(At::ElemGet::object), nullptr, true });
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
        // It does part of what the loop of the helper does, which stands behind its answer `false` and is followed like any code: nothing happens here that does not happen there.
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
        // A function under a computed key gets its name at run time: a definition of `name` with a text that nobody knows.
        ValueFlowOperand text { { }, m_universe.strings(), { } };
        define(operand(At::SetFunctionName::function), keyNamed(m_program.keyOf(BuiltinNames::name)), HeapPropertyPart::Value, text);
        return;
    }
    case Opcode::AppendHole:
        return;
    case Opcode::SetLiteralPrototype: {
        ClimbingSet literals = m_context.valueOf(operand(At::SetLiteralPrototype::object));
        changePrototype(literals, operand(At::SetLiteralPrototype::prototype));
        // A value that is neither an object nor null leaves the literal under Object.prototype.
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
    case Opcode::CopyObject:
        runCopyObject();
        return;
    case Opcode::ScopeHasBinding:
        runScopeHasBinding();
        return;
    case Opcode::ForInOpen:
        noteTestOfMembers(operand(At::ForInOpen::object), unknownHeapKey);
        [[fallthrough]];
    case Opcode::ForInStep:
        // A name is a string nobody knows. A proxy on the chain runs its traps. A proxy is the unknown object and its handler has escaped, so the traps are called from there; what they throw is the unknown value below.
        add(m_statement.result, m_universe.maskOf(m_context.maker(), m_program, m_program.at(operation().type)));
        mayThrow();
        return;
    case Opcode::GetSuperBase:
        runGetSuperBase();
        return;
    case Opcode::SuperGet:
        runSuperGet();
        return;
    case Opcode::SuperSet:
        runSuperSet();
        return;
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
        // A constructor whose parent returns another object puts its private names on that object, whose layout has no field for them: there the name is one more property.
        HeapKey key { HeapKeyClass::Name, m_program.at(operation().layout()).fields[operation().field()].key };
        // The allocation in the constructor of a base class also stands for what it makes under the prototype of a class of which nothing says what it extends, and it is listed under every such class.
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
            // The brand of a class with private methods holds a token, which is no value of the language. A write of nothing would be no write.
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
            markStore(stamped, key, { });
        }
        mayThrow();
        return;
    }
    case Opcode::PrivateHas:
        answerBoolean();
        mayThrow();
        return;
    case Opcode::NewObject:
        runNewObject();
        return;
    case Opcode::NewArray:
        runNewArray();
        return;
    case Opcode::NewTemplateObject:
        runNewTemplateObject();
        return;
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
    case Opcode::Data:
        runData();
        return;
    case Opcode::CreateIteratorResult:
        runCreateIteratorResult();
        return;
    case Opcode::GetOwnProperty:
        read(operand(At::GetOwnProperty::object), keyNamed(operation().key()));
        return;
    case Opcode::GetByValWithThis:
        runGetByValWithThis();
        return;
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
    case Opcode::PutDoubleIndex:
        write(m_context.valueOf(operand(At::PutDoubleIndex::array)), elementsName, operand(At::PutDoubleIndex::value));
        return;
    case Opcode::GrowNewArray:
        return;
    case Opcode::TruncateNewArray:
        runTruncateNewArray();
        return;
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

bool isHeapOperation(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropGet:
    case Opcode::ElemGet:
    case Opcode::PropSet:
    case Opcode::ElemSet:
    case Opcode::PropDefine:
    case Opcode::ElemDefine:
    case Opcode::DefineGetter:
    case Opcode::DefineSetter:
    case Opcode::TryDefineLiveBinding:
    case Opcode::TryReExportFields:
    case Opcode::PropHas:
    case Opcode::ElemHas:
    case Opcode::PropDelete:
    case Opcode::ElemDelete:
    case Opcode::SetFunctionName:
    case Opcode::AppendHole:
    case Opcode::SetLiteralPrototype:
    case Opcode::CopyDataProperties:
    case Opcode::CopyObject:
    case Opcode::ScopeHasBinding:
    case Opcode::ForInOpen:
    case Opcode::ForInStep:
    case Opcode::GetSuperBase:
    case Opcode::SuperGet:
    case Opcode::SuperSet:
    case Opcode::PrivateGet:
    case Opcode::PrivateSet:
    case Opcode::PrivateDefine:
    case Opcode::PrivateHas:
    case Opcode::NewObject:
    case Opcode::NewArray:
    case Opcode::NewTemplateObject:
    case Opcode::AppendElement:
    case Opcode::CreateUnmappedArguments:
    case Opcode::CreateMappedArguments:
    case Opcode::GetMappedArgument:
    case Opcode::SetMappedArgument:
    case Opcode::Data:
    case Opcode::CreateIteratorResult:
    case Opcode::GetOwnProperty:
    case Opcode::GetByValWithThis:
    case Opcode::GetContiguousIndex:
    case Opcode::GetDoubleIndex:
    case Opcode::PutContiguousIndex:
    case Opcode::TryPutByValDirect:
    case Opcode::TryPutIndex:
    case Opcode::PutDoubleIndex:
    case Opcode::GrowNewArray:
    case Opcode::TruncateNewArray:
    case Opcode::GetInternalField:
    case Opcode::PutInternalField:
        return true;
    default:
        return false;
    }
}

// A lookup enters getters and records uses of built-ins, and many operations share it. So even a read does nothing before its function is reached.
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
