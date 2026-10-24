/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionKinds.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

ValueFlowName writtenName(const HeapKey& key, HeapPropertyPart part)
{
    switch (part) {
    case HeapPropertyPart::Value:
        switch (key.keyClass) {
        case HeapKeyClass::Name:
            return ValueFlowName::property(key.name);
        case HeapKeyClass::Index:
            return elementsName;
        case HeapKeyClass::Unknown:
            return ValueFlowName::of(ValueFlowNameKind::UnknownKey);
        }
        break;
    case HeapPropertyPart::Getter:
        return key.keyClass == HeapKeyClass::Name ? ValueFlowName::getter(key.name) : ValueFlowName::of(ValueFlowNameKind::GetterUnderUnknownKey);
    case HeapPropertyPart::Setter:
        return key.keyClass == HeapKeyClass::Name ? ValueFlowName::setter(key.name) : ValueFlowName::of(ValueFlowNameKind::SetterUnderUnknownKey);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueFlowName foundName(const HeapKey& key, HeapPropertyPart part)
{
    if (key.keyClass != HeapKeyClass::Unknown)
        return writtenName(key, part);
    switch (part) {
    case HeapPropertyPart::Value:
        return ValueFlowName::of(ValueFlowNameKind::EveryProperty);
    case HeapPropertyPart::Getter:
        return ValueFlowName::of(ValueFlowNameKind::EveryGetter);
    case HeapPropertyPart::Setter:
        return ValueFlowName::of(ValueFlowNameKind::EverySetter);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Only the lookup tells the two apart: what was written lies under the same names for both.
ValueFlowName nameOfLookupForValue(const HeapKey& key)
{
    if (key.namesEnumerableProperty)
        return { ValueFlowNameKind::EveryProperty, 1 };
    return foundName(key, HeapPropertyPart::Value);
}

HeapKey keyOfFoundName(ValueFlowName name)
{
    if (name == nameOfLookupForValue(enumeratedHeapKey))
        return enumeratedHeapKey;
    switch (name.kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
        return { HeapKeyClass::Name, PropertyKeyID(name.payload) };
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
        return indexHeapKey;
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
        return unknownHeapKey;
    default:
        return unknownHeapKey;
    case ValueFlowNameKind::MemberWritten:
        return { HeapKeyClass::Name, PropertyKeyID(name.payload) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinClassID wrapperOf(PrimitiveSet kind)
{
    uint16_t bit = static_cast<uint16_t>(kind);
    if (bit & Primitive::AnyString)
        return BuiltinClasses::String;
    if (bit & Primitive::Number)
        return BuiltinClasses::Number;
    if (bit & Primitive::Boolean)
        return BuiltinClasses::Boolean;
    if (bit & Primitive::AnySymbol)
        return BuiltinClasses::Symbol;
    if (bit & Primitive::BigInt)
        return BuiltinClasses::BigInt;
    return { };
}

// The run time answers the same question with FunctionTables::hasPrototypePropertyAtCreation.
static bool functionHasPrototypeProperty(const Program& program, const Function& function)
{
    if (function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor)
        return true;
    if (isInRuntimeModule(program, function))
        return false;
    if (function.has(FunctionFlags::IsGenerator))
        return true;
    return function.kind == FunctionKind::Plain && !function.has(FunctionFlags::IsAsync);
}

// A generator function has a prototype object too, for which its generators stand.
static bool hasOrdinaryPrototypeObject(const Program& program, const Function& function)
{
    return !function.constructedClass && !function.has(FunctionFlags::IsGenerator) && functionHasPrototypeProperty(program, function);
}

bool HeapRule::isOperationOfFunction()
{
    return m_statement.function || m_context.functionOfStatement();
}

// A placeholder is looked at by itself, so that a set with it and the same set without it share one lookup.
ClimbingSet HeapRule::startOfLookup(ClimbingSet receivers)
{
    ClimbingSet start = m_context.restrict(receivers, m_tables.startsOfLookups());
    ClimbingSet keys = m_context.restrict(receivers, m_universe.keys());
    if (m_context.meet(keys, ValueSets::complement(m_universe.strings())))
        start = m_context.unite(start, ClimbingSet(m_tables.stringAndSymbolKinds()));
    else if (holdsAnything(keys))
        start = m_context.unite(start, ClimbingSet(only(ValueFlowUniverse::primitive(PrimitiveSet::String))));
    return start;
}

HeapChain HeapRule::chainFrom(ClimbingSet receivers)
{
    ClimbingSet start = startOfLookup(receivers);
    ValueFlowNodeID wholeChain = m_context.lookup(start, prototypeLinkName);
    ClimbingSet chain = m_context.valueOf(wholeChain);
    ClimbingSet objects = known(chain);
    // The names of a namespace object are copied from the object that the body of its module answers, which is such a holder too: beside the unknown object the chain shows neither once it has escaped.
    if (m_keepsHoldersOfExportsItStartsFrom) {
        ClimbingSet holdersOfExports(m_universe.holdersOfLibraryExports());
        objects = m_context.unite(objects, m_context.unite(m_context.restrict(start, m_universe.holdersOfLibraryExports()), m_context.valueWithin(holdersOfExports, wholeChain)));
    }
    return { start, ClimbingSet(ValueSets::nothing), objects, objects, mayBeUnknownObject(chain) || mayBeUnknownObject(receivers), mayBeUntrackedObject(chain) || mayBeUntrackedObject(receivers) };
}

ValueSetID HeapRule::holdersWithLockedLocationUnder(const HeapKey& key) const
{
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        return m_tables.under(key.name).holdersWithLockedLocation;
    case HeapKeyClass::Index:
        return m_tables.holdersWithLockedIndexes();
    case HeapKeyClass::Unknown:
        return m_tables.lockedObjects();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Native code puts no property under a symbol that the program makes, nor under one that the program registers and no entry of the table names. Unseen code registers the same text, and lists the symbols of what it holds.
ValueSetID HeapRule::holdersOfUndeclaredPropertiesUnder(const HeapKey& key)
{
    if (key.keyClass == HeapKeyClass::Index)
        return m_tables.holdersOfUndeclaredPropertiesUnderIndexes();
    if (key.keyClass != HeapKeyClass::Name)
        return m_tables.holdersOfUndeclaredPropertiesUnderAnyKey();
    if (m_nameOfHoldersOfUndeclaredProperties == key.name && m_holdersOfUndeclaredPropertiesUnderName)
        return m_holdersOfUndeclaredPropertiesUnderName;
    m_nameOfHoldersOfUndeclaredProperties = key.name;
    m_holdersOfUndeclaredPropertiesUnderName = holdersOfUndeclaredPropertiesUnderName(key.name);
    return m_holdersOfUndeclaredPropertiesUnderName;
}

ValueSetID HeapRule::holdersOfUndeclaredPropertiesUnderName(PropertyKeyID name)
{
    HeapKey key { HeapKeyClass::Name, name };
    PropertyKeyKind kind = m_program.at(key.name).kind;
    if ((kind == PropertyKeyKind::CreatedSymbol || kind == PropertyKeyKind::RegisteredSymbol) && m_tables.under(key.name).holders == ValueSets::nothing && !doesUnseenCodeRun())
        return ValueSets::nothing;
    ValueSetID holders = m_tables.under(key.name).holdersOfUndeclaredProperties;
    // No namespace, constructor or function of the table has a `then`: BuiltinTable::mismatches() looks.
    if (key.name == m_program.keyOf(BuiltinNames::then) && !doesUnseenCodeRun())
        holders = m_context.maker().intersect(holders, m_context.maker().unite(ValueSets::complement(m_universe.builtinObjects()), m_tables.prototypeObjects()));
    // The run time refuses a new member on a prototype object of the table only where the table closes the name.
    bool isPrivateName = kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::EnginePrivateName;
    if (!isPrivateName && doesUnseenCodeRun())
        holders = m_context.maker().unite(holders, m_context.maker().intersect(m_tables.prototypeObjects(), ValueSets::complement(m_tables.under(key.name).holdersWithLockedLocation)));
    return holders;
}

ValueSetID HeapRule::holdersWithMemberUnder(const HeapKey& key) const
{
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        return m_tables.under(key.name).holders;
    case HeapKeyClass::Index:
        return m_tables.holdersWithMembersUnderNumericNames();
    case HeapKeyClass::Unknown:
        return m_tables.holdersWithMembers();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The prototype object of a class whose instances are entries lies above those entries, and a program may put an object of its own above it, as node:events does above the one of `process`.
std::optional<ClimbingSet> HeapRule::chainUpToPrototypeObjectsOfEntries(ClimbingSet start, ValueFlowNodeID wholeChain, const HeapKey& key)
{
    if (key.keyClass != HeapKeyClass::Name)
        return std::nullopt;
    ValueSetID answering = m_tables.under(key.name).prototypeObjectsOfEntries;
    if (answering == ValueSets::nothing)
        return std::nullopt;
    ClimbingSet holders = m_context.unite(m_context.restrict(start, answering), m_context.valueWithin(ClimbingSet(answering), wholeChain));
    if (!holdsAnything(holders))
        return std::nullopt;

    ValueFlowName deleted { ValueFlowNameKind::DeletedMember, key.name.index() };
    bool mayGoOn = holdsAnything(readWhereWritten(holders, deleted)) || holdsAnything(readWhereWritten(holders, copiedFromName));
    forEachNodeStoredThroughPlaceholdersInto(holders, deleted, [&](ValueFlowNodeID) {
        mayGoOn = true;
    });
    if (mayGoOn)
        return std::nullopt;

    constexpr unsigned longestChain = 8;
    ClimbingSet walked = start;
    ClimbingSet current = m_context.restrict(known(start), ValueSets::complement(answering));
    for (unsigned step = 0; step < longestChain; ++step) {
        if (!holdsAnything(current))
            return walked;
        ClimbingSet above = objectsAbove(current);
        walked = m_context.unite(walked, above);
        current = m_context.restrict(known(above), ValueSets::complement(answering));
    }
    return std::nullopt;
}

HeapChain HeapRule::chainThatGoesUpFrom(ClimbingSet receivers, const HeapKey& key)
{
    HeapChain chain = chainFrom(receivers);
    std::optional<ClimbingSet> walked = chainUpToPrototypeObjectsOfEntries(chain.start, m_context.lookup(chain.start, prototypeLinkName), key);
    if (!walked)
        return chain;
    chain.objects = known(*walked);
    chain.writableObjects = chain.objects;
    chain.holdsUnknownObject = mayBeUnknownObject(*walked) || mayBeUnknownObject(receivers);
    chain.holdsUntrackedObject = mayBeUntrackedObject(*walked) || mayBeUntrackedObject(receivers);
    return chain;
}

bool HeapRule::mayInheritSetter(ClimbingSet objects)
{
    ClimbingSet chain = m_context.valueOf(m_context.lookup(objects, prototypeLinkName));
    if (m_context.meet(chain, m_universe.placeholdersAlone()) || doesUnseenCodeRun())
        return true;
    ClimbingSet holders = known(chain);
    if (mayHoldUndeclaredProperty(holders, unknownHeapKey))
        return true;
    ValueFlowName setters = foundName(unknownHeapKey, HeapPropertyPart::Setter);
    bool isFound = holdsAnything(readWhereWritten(holders, setters));
    forEachNodeStoredThroughPlaceholdersInto(holders, setters, [&](ValueFlowNodeID) {
        isFound = true;
    });
    return isFound;
}

// A built-in holder that has the name among its own members answers by itself.
HeapChain HeapRule::chainFrom(ClimbingSet receivers, const HeapKey& key)
{
    auto withoutLockedLocations = [&](HeapChain chain) {
        chain.writableObjects = m_context.restrict(chain.objects, ValueSets::complement(holdersWithLockedLocationUnder(key)));
        return chain;
    };

    if (key.keyClass != HeapKeyClass::Name) {
        HeapChain chain = chainFrom(receivers);
        chain.objects = m_context.unite(chain.objects, m_context.restrict(chain.start, m_tables.holdersOfMembersThatHostMakes()));
        return withoutLockedLocations(chain);
    }
    ValueSetID answering = m_tables.holdersThatAnswerUnder(key.name);
    if (ValueSetID storedTo = m_tables.objectsStoredToAtCreationUnder(key.name); storedTo != ValueSets::nothing) {
        ClimbingSet storedToAmongReceivers = m_context.restrict(receivers, storedTo);
        if (holdsAnything(storedToAmongReceivers) && !mayInheritSetter(storedToAmongReceivers))
            answering = m_context.maker().unite(answering, storedTo);
    }
    ClimbingSet holdersThatMayLoseMember = m_context.restrict(receivers, m_tables.holdersThatMayLoseMemberUnder(key.name));
    ValueFlowName deleted { ValueFlowNameKind::DeletedMember, key.name.index() };
    if (holdsAnything(holdersThatMayLoseMember)) {
        bool mayBeDeleted = holdsAnything(readWhereWritten(holdersThatMayLoseMember, deleted));
        forEachNodeStoredThroughPlaceholdersInto(holdersThatMayLoseMember, deleted, [&](ValueFlowNodeID) {
            mayBeDeleted = true;
        });
        if (mayBeDeleted)
            answering = ValueSets::nothing;
    }
    // A function keeps the prototype property that it is created with. Above a generator function lies an object with a property of that name, which no read from the function finds.
    if (key.name == m_tables.prototypeKey())
        answering = m_context.maker().unite(answering, m_tables.closuresWithPrototypeProperty());

    ClimbingSet holders = m_context.restrict(receivers, answering);
    // What was copied onto an object takes the place of what it was created with, and a lookup finds it through the objects that it was copied from, which lie above the object like what it inherits from.
    if (!holdsAnything(holders) || holdsAnything(readWhereWritten(known(holders), copiedFromName)))
        return withoutLockedLocations(chainThatGoesUpFrom(receivers, key));
    HeapChain chain = chainThatGoesUpFrom(m_context.restrict(receivers, ValueSets::complement(answering)), key);
    chain.holdersThatAnswer = holders;
    chain.objects = m_context.unite(chain.objects, holders);
    return withoutLockedLocations(chain);
}

// Most names are never stored through the unknown object, and then nobody has to look at what has escaped.
ValueFlowNodeID HeapRule::storedThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName name)
{
    ValueFlowNodeID stored = m_context.read(unknownObjectAlone(), name);
    // In a large program every name is stored through the unknown object sooner or later, and many objects never escape.
    if (!m_context.meet(objects, m_universe.builtinObjects()) && !hasEscaped(objects))
        return { };
    if (!holdsAnything(stored))
        return { };
    // Under a name that the table does not list the global object takes a property from whoever holds it: nothing is locked there.
    bool namesProperty = name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter;
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (namesProperty && m_context.contains(objects, globalObject) && isNameThatTableDoesNotListOnGlobalObject({ HeapKeyClass::Name, PropertyKeyID(name.payload) }) && hasEscaped(ClimbingSet(only(globalObject))))
        return stored;
    // Nothing below says anything of a set that holds no object of the realm, and most receivers are objects of the program alone.
    if (m_context.meet(objects, m_tables.objectsOfRealm()) && !doesUnseenCodeChangeRealm()) {
        if (m_context.holdsAnyOf(m_context.restrict(objects, m_tables.objectsOfRealm()), m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed)))
            return stored;
        if (namesProperty) {
            HeapKey key { HeapKeyClass::Name, PropertyKeyID(name.payload) };
            ValueSetID takingNoNewNames = m_context.maker().unite(holdersWithLockedLocationUnder(key), holdersWithMemberUnder(key));
            ClimbingSet takingTheName = m_context.restrict(objects, m_context.maker().intersect(m_tables.objectsOfRealm(), ValueSets::complement(takingNoNewNames)));
            if (holdsAnything(takingTheName) && hasEscaped(takingTheName))
                return stored;
        }
        bool findsEveryName = name.kind == ValueFlowNameKind::EveryProperty || name.kind == ValueFlowNameKind::EveryGetter || name.kind == ValueFlowNameKind::EverySetter;
        if (findsEveryName) {
            ClimbingSet takingNewNames = m_context.restrict(objects, m_context.maker().intersect(m_tables.objectsOfRealm(), ValueSets::complement(m_tables.lockedObjects())));
            if (holdsAnything(takingNewNames) && hasEscaped(takingNewNames))
                return stored;
        }
        objects = m_context.restrict(objects, ValueSets::complement(m_tables.objectsOfRealm()));
    }
    return hasEscaped(objects) ? stored : ValueFlowNodeID();
}

ValueFlowNodeID HeapRule::storedThroughUntrackedObjectInto(ClimbingSet objects, ValueFlowName name)
{
    if (!hasBeenLost(objects))
        return { };
    ValueFlowNodeID stored = m_context.read(untrackedObjectAlone(), name);
    return holdsAnything(stored) ? stored : ValueFlowNodeID();
}

// What lies above the unknown object is handed out as the unknown object, and the prototype objects of the language are among it.
void HeapRule::letPrototypeObjectsOfLanguageEscape()
{
    add(ValueFlowProgramNodes::escaped(), m_tables.prototypeObjectsOfLanguage());
}

void HeapRule::appendPrototypeOf(BuiltinClassID builtinClass, Members& prototypes) const
{
    if (!builtinClass)
        return;
    if (BuiltinID prototype = m_builtins.at(builtinClass).prototype)
        prototypes.append(m_universe.objectOf(prototype));
    else
        prototypes.append(m_tables.undescribedNativeCode());
}

void HeapRule::appendPrototypeAtCreation(ValueFlowObjectID object, Members& prototypes) const
{
    auto hasHeritage = [&](LayoutID layout) {
        return layout && m_program.at(layout).has(LayoutFlags::HasHeritage);
    };

    switch (m_universe.kindOf(object)) {
    case ValueFlowObjectKind::Primitive:
        appendPrototypeOf(wrapperOf(static_cast<PrimitiveSet>(1u << object.index())), prototypes);
        return;
    case ValueFlowObjectKind::Key:
        appendPrototypeOf(m_program.at(m_universe.keyOf(object)).kind == PropertyKeyKind::Text ? BuiltinClasses::String : BuiltinClasses::Symbol, prototypes);
        return;
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::ResultOfBuiltin:
        return;
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin: {
        const Builtin& entry = m_builtins.at(m_universe.builtinOf(object));
        switch (entry.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            if (object != m_tables.undescribedNativeCode())
                prototypes.append(m_tables.undescribedNativeCode());
            return;
        case BuiltinPrototypeLink::Null:
            return;
        case BuiltinPrototypeLink::Builtin:
            prototypes.append(m_universe.objectOf(entry.inheritsFrom));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    case ValueFlowObjectKind::Closure: {
        const Function& function = m_program.at(m_universe.closureFunction(object));
        if (!hasHeritage(function.constructedClass))
            appendPrototypeOf(classOfFunctionObject(function), prototypes);
        return;
    }
    case ValueFlowObjectKind::Instance:
        prototypes.append(m_universe.prototypeOf(m_universe.layoutOf(object)));
        return;
    case ValueFlowObjectKind::CoroutineResult:
    case ValueFlowObjectKind::RestArray:
    case ValueFlowObjectKind::FunctionPrototype:
        appendPrototypeOf(m_universe.classOf(object), prototypes);
        return;
    // The construction writes what lies above the object.
    case ValueFlowObjectKind::ConstructedObject:
        return;
    case ValueFlowObjectKind::Prototype: {
        LayoutID layout = m_universe.classLayoutOf(object);
        if (hasHeritage(layout))
            return;
        FunctionID constructor = m_program.at(layout).constructor;
        appendPrototypeOf(constructor ? classAbovePrototypeProperty(m_program.at(constructor)) : BuiltinClasses::Object, prototypes);
        return;
    }
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *m_universe.siteOf(object);
        if (site.builtinClass)
            appendPrototypeOf(site.builtinClass == BuiltinClasses::Arguments ? BuiltinClasses::Object : site.builtinClass, prototypes);
        if (!site.layout)
            return;
        switch (m_program.at(site.layout).kind) {
        case LayoutKind::Record:
        case LayoutKind::Container:
        case LayoutKind::BuiltinResult:
            if (!site.namesPrototype)
                appendPrototypeOf(BuiltinClasses::Object, prototypes);
            return;
        case LayoutKind::Instance:
            prototypes.append(m_universe.prototypeOf(site.layout));
            return;
        default:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueSetID HeapRule::prototypesAtCreationOf(ClimbingSet objects)
{
    constexpr uint64_t isKnown = 1ull << 63;
    constexpr uint32_t mask = (1u << 31) - 1;
    Members prototypes;
    Vector<uint32_t, 64> appendedLately;
    appendedLately.fill(UINT32_MAX, 64);
    unsigned shift = 26;
    auto keep = [&](uint32_t prototype) {
        uint32_t& slot = appendedLately[(prototype * 0x9e3779b1u) >> shift];
        if (slot == prototype)
            return;
        slot = prototype;
        prototypes.append(ValueFlowObjectID(prototype));
        if (prototypes.size() * 4 <= appendedLately.size() || shift <= 14)
            return;
        shift -= 4;
        appendedLately.fill(UINT32_MAX, 1u << (32 - shift));
        for (ValueFlowObjectID earlier : prototypes)
            appendedLately[(earlier.index() * 0x9e3779b1u) >> shift] = earlier.index();
    };
    auto keepPrototypesOf = [&](ValueFlowObjectID object, NOESCAPE const Invocable<void(uint32_t)> auto& keep) {
        std::atomic<uint64_t>& known = m_tables.prototypesAtCreationOf(object);
        uint64_t packed = known.load(std::memory_order_relaxed);
        if (!packed) [[unlikely]] {
            Members own;
            appendPrototypeAtCreation(object, own);
            RELEASE_ASSERT(own.size() <= 2);
            packed = isKnown;
            for (size_t index = 0; index < own.size(); ++index)
                packed |= static_cast<uint64_t>(own[index].index() + 1) << (31 * index);
            known.store(packed, std::memory_order_relaxed);
        }
        for (unsigned index = 0; index < 2; ++index) {
            if (uint32_t prototype = (packed >> (31 * index)) & mask)
                keep(prototype - 1);
        }
    };
    constexpr unsigned mostKept = 6;
    bool goesByBlock = m_context.forEachBlock(objects, [&](uint64_t nameOfBlock) {
        std::atomic<uint64_t>* entry = m_tables.prototypesOfBlock(nameOfBlock);
        uint64_t count = entry[0].load(std::memory_order_acquire);
        if (!(count & 1) && entry[1].load(std::memory_order_relaxed) == nameOfBlock) {
            std::array<uint64_t, 3> words { entry[2].load(std::memory_order_relaxed), entry[3].load(std::memory_order_relaxed), entry[4].load(std::memory_order_relaxed) };
            std::atomic_thread_fence(std::memory_order_acquire);
            if (entry[0].load(std::memory_order_relaxed) == count) {
                // One more than the prototype; zero ends the list.
                for (unsigned index = 0; index < mostKept; ++index) {
                    uint32_t prototypePlusOne = static_cast<uint32_t>(words[index / 2] >> (index % 2 * 32));
                    if (!prototypePlusOne)
                        break;
                    keep(prototypePlusOne - 1);
                }
                return;
            }
        }
        std::array<uint32_t, mostKept> found { };
        unsigned numberFound = 0;
        m_context.sets().forEachMemberOfBlock(nameOfBlock, [&](ValueFlowObjectID object) {
            keepPrototypesOf(object, [&](uint32_t prototype) {
                keep(prototype);
                if (numberFound > mostKept)
                    return;
                for (unsigned index = 0; index < std::min(numberFound, mostKept); ++index) {
                    if (found[index] == prototype + 1)
                        return;
                }
                if (numberFound < mostKept)
                    found[numberFound] = prototype + 1;
                ++numberFound;
            });
        });
        if (numberFound > mostKept)
            return;
        count = entry[0].load(std::memory_order_relaxed);
        if ((count & 1) || !entry[0].compare_exchange_strong(count, count + 1, std::memory_order_acquire, std::memory_order_relaxed))
            return;
        std::atomic_thread_fence(std::memory_order_release);
        entry[1].store(nameOfBlock, std::memory_order_relaxed);
        for (unsigned word = 0; word < 3; ++word)
            entry[2 + word].store(static_cast<uint64_t>(found[word * 2 + 1]) << 32 | found[word * 2], std::memory_order_relaxed);
        entry[0].store(count + 2, std::memory_order_release);
    });
    if (!goesByBlock) {
        m_context.forEachMember(objects, [&](ValueFlowObjectID object) {
            keepPrototypesOf(object, keep);
        });
    }
    return m_context.maker().of(prototypes.span());
}

BuiltinID HeapRule::memberUnder(BuiltinID holder, PropertyKeyID key) const
{
    if (BuiltinID member = findOwnMember(m_program, holder, key))
        return member;
    // The host's own modules reach some of its functions under private names of the global object.
    if (m_builtins.canonicalID(holder) != Builtins::GlobalThis || m_program.at(key).kind != PropertyKeyKind::EnginePrivateName)
        return { };
    return m_builtins.findOwnMember(Builtins::PrivateGlobals, builtinMemberNameOf(m_program, key).name);
}

ValueFlowObjectID HeapRule::objectOfMember(BuiltinID member, ModuleID module) const
{
    if (ValueFlowObjectID object = module ? m_universe.objectOf(member, module) : ValueFlowObjectID())
        return object;
    return m_universe.objectOf(member);
}

// A text under which the table has no member of the global object. The one own name that it does not list as a member is the name of the object itself.
bool HeapRule::isNameThatTableDoesNotListOnGlobalObject(const HeapKey& key) const
{
    if (key.keyClass != HeapKeyClass::Name || m_program.at(key.name).kind != PropertyKeyKind::Text || key.name == m_program.keyOf(m_builtins.at(Builtins::GlobalThis).name))
        return false;
    return !m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::GlobalThis));
}

// `globalThis.name`, and no free name: a free name that the table does not list is still taken for a binding that somebody else has made, a value nobody knows.
bool HeapRule::namesObjectOfAccess() const
{
    if (m_statement.kind != ValueFlowStatementKind::Operation)
        return false;
    bool mayBeFreeName = m_statement.opcode == Opcode::PropGet || m_statement.opcode == Opcode::PropSet || m_statement.opcode == Opcode::PropDelete;
    return !mayBeFreeName || !m_program.at(m_statement.function).at(m_statement.site).isFreeName();
}

// The table lists every own name that the engine and the host give the global object (test/aot/builtins/realm-agreement.test.ts).
bool HeapRule::holdsGlobalObjectOnlyWhatStatementsHaveStored()
{
    return !doesUnseenCodeRun() && !m_context.holdsAnyOf(ClimbingSet(only(m_universe.objectOf(Builtins::ProcessDlopen))), m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins));
}

// What lies on the instances of a class is found at its prototype object: an instance has that object above it.
bool HeapRule::mayHoldUndeclaredProperty(ClimbingSet objects, const HeapKey& key)
{
    ValueSetID holders = holdersOfUndeclaredPropertiesUnder(key);
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (namesObjectOfAccess() && m_context.contains(objects, globalObject) && isNameThatTableDoesNotListOnGlobalObject(key) && holdsGlobalObjectOnlyWhatStatementsHaveStored())
        holders = m_context.maker().intersect(holders, ValueSets::complement(only(globalObject)));
    return m_context.meet(objects, holders);
}

// Where the instances of a class are entries, the unknown object stands for one of them once that entry has escaped.
ClimbingSet HeapRule::prototypeObjectsAboveEscapedEntries(ValueSetID holders)
{
    Members prototypes;
    for (const ValueFlowClassOfEntries& classOfEntries : m_tables.classesOfEntries()) {
        if (m_context.contains(ClimbingSet(holders), classOfEntries.prototype) && hasEscaped(ClimbingSet(classOfEntries.entries)))
            prototypes.append(classOfEntries.prototype);
    }
    return ClimbingSet(m_context.maker().of(prototypes.span()));
}

// The unknown object stands for an instance of any class of the table, which inherits the members of the prototype object of the class, and for every built-in holder that has escaped.
ClimbingSet HeapRule::holdersFoundThroughUnknownObject(const HeapKey& key)
{
    switch (key.keyClass) {
    case HeapKeyClass::Name: {
        const ValueFlowMembersUnderKey& members = m_tables.under(key.name);
        if (members.holders == ValueSets::nothing)
            return ClimbingSet(ValueSets::nothing);
        ClimbingSet found = m_context.unite(escapedAmong(ClimbingSet(members.holders)), ClimbingSet(members.prototypeObjects));
        return m_context.unite(found, prototypeObjectsAboveEscapedEntries(members.holders));
    }
    case HeapKeyClass::Index:
        return ClimbingSet(ValueSets::nothing);
    case HeapKeyClass::Unknown: {
        ClimbingSet found = m_context.unite(escapedAmong(ClimbingSet(m_tables.holdersWithMembers())), ClimbingSet(m_tables.prototypeObjects()));
        return m_context.unite(found, prototypeObjectsAboveEscapedEntries(m_tables.holdersWithMembers()));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An accessor of the host looks at its receiver first and throws for whatever is no instance of its class.
ClimbingSet HeapRule::holdersOfAccessorsThatRunOnUnknownObject(const HeapKey& key)
{
    ClimbingSet holders = holdersFoundThroughUnknownObject(key);
    ClimbingSet ofHost = m_context.restrict(holders, m_tables.prototypeObjectsOfHost());
    if (!holdsAnything(ofHost) || doesUnseenCodeRun())
        return holders;

    ClimbingSet withInstances = m_context.unite(m_context.valueWithin(ofHost, m_context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances)), escapedAmong(ofHost));
    Members aboveEscapedEntries;
    for (const ValueFlowClassOfEntries& classOfEntries : m_tables.classesOfHostWithEntries()) {
        if (m_context.contains(ofHost, classOfEntries.prototype) && hasEscaped(ClimbingSet(classOfEntries.entries)))
            aboveEscapedEntries.append(classOfEntries.prototype);
    }
    withInstances = m_context.unite(withInstances, ClimbingSet(m_context.maker().of(aboveEscapedEntries.span())));
    return m_context.unite(m_context.restrict(holders, ValueSets::complement(m_tables.prototypeObjectsOfHost())), withInstances);
}

// Whoever asks looks through the untracked object, so the lookup of its chain is there from then on. That lookup also sees to the accessors nobody knows: see collectChain().
ClimbingSet HeapRule::foundThroughUntrackedObject(ValueSetID objects)
{
    ValueFlowNodeID chain = m_context.lookup(untrackedObjectAlone(), prototypeLinkName);
    if (objects == ValueSets::nothing)
        return { };
    return m_context.valueWithin(ClimbingSet(objects), chain);
}

// The run time refuses a function that starts unseen code to whoever has not named it. Likewise a member of the host that no name in the program leads to: the image holds the code behind it only for a program that names it.
bool HeapRule::isHandedOutThroughPlaceholder(BuiltinID member, const HeapKey& key)
{
    if (m_tables.letsInUnseenCode(member) || isFoundByNameAlone(m_builtins.canonicalID(member)))
        return false;
    // The object of bun:test is in unknown hands because a load under a name nobody knows may answer it. `request.expect` is no use of bun:test for that.
    if (BuiltinID holder = m_builtins.at(member).holder; holder && isNativeModuleThatNobodyNames(m_universe.objectOf(holder)))
        return false;
    const Builtin& entry = m_builtins.at(member);
    // That Bun is in unknown hands is no use of what hangs on it: `value.Glob` spells a name and binds to nothing. See isMemberOfBun().
    if (isMemberOfBun(member))
        return false;
    // A class hangs under its own name on a namespace, and whoever reads that name from a value nobody knows may hold the namespace.
    bool isFoundUnderOwnName = entry.kind != BuiltinKind::Constructor || (entry.holder && m_builtins.at(entry.holder).kind == BuiltinKind::Namespace);
    return (key.keyClass == HeapKeyClass::Name && isFoundUnderOwnName) || entry.origin != BuiltinOrigin::Host;
}

// The prototype object of the class of an instance is not among these. Whoever holds the instance finds through it what lies on that object, and that leaves the object as it is.
ClimbingSet HeapRule::objectsHeldFromCreation(ClimbingSet objects)
{
    Members held;
    m_context.forEachMember(known(objects), [&](ValueFlowObjectID object) {
        if (FunctionID function = m_universe.closureFunction(object)) {
            if (LayoutID layout = m_program.at(function).constructedClass)
                held.append(m_universe.prototypeOf(layout));
            else if (hasOrdinaryPrototypeObject(m_program, m_program.at(function)))
                held.append(m_universe.prototypeObjectOf(function));
        }
        if (m_universe.kindOf(object) == ValueFlowObjectKind::FunctionPrototype)
            held.append(m_universe.closureOf(m_universe.functionOf(object)));
        LayoutID layout = m_universe.classLayoutOf(object);
        if (FunctionID constructor = layout ? m_program.at(layout).constructor : FunctionID())
            held.append(m_universe.closureOf(constructor));
    });
    return ClimbingSet(m_context.maker().of(held.span()));
}

// What was put above the objects, and what they are born with.
ClimbingSet HeapRule::prototypesOf(ClimbingSet start, ValueSetID atCreation)
{
    // A link that is locked is the one that the table names.
    ClimbingSet objects = m_context.restrict(known(start), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink()));
    ValueFlowNodeID links = readWhereWritten(objects, prototypeLinkName);
    ClimbingSet above = m_context.unite(valueOfNodeIfAny(links), ClimbingSet(atCreation ? atCreation : prototypesAtCreationOf(start)));
    if (links && mayBeUnknownObject(above))
        above = m_context.unite(above, m_context.valueWithin(ClimbingSet(m_universe.holdersOfLibraryExports()), links));
    forEachNodeStoredThroughPlaceholdersInto(objects, prototypeLinkName, [&](ValueFlowNodeID stored) {
        above = m_context.unite(above, m_context.valueOf(stored));
    });
    return above;
}

// Where a lookup goes on from the objects: to their prototypes, and to the objects whose properties were copied onto them.
ClimbingSet HeapRule::objectsAbove(ClimbingSet start)
{
    return m_context.unite(prototypesOf(start), valueOfNodeIfAny(readWhereWritten(known(start), copiedFromName)));
}

void HeapRule::collectChain()
{
    ClimbingSet start = m_context.setOfKey();
    add(m_statement.result, start);
    if (mayBeUnknownObject(start))
        letPrototypeObjectsOfLanguageEscape();
    if (mayBeUntrackedObject(start)) {
        if (!m_context.meet(start, ValueSets::complement(m_universe.untrackedObjectAlone()))) {
            collectChainOfUntrackedObject();
            return;
        }
        connect(m_context.lookup(untrackedObjectAlone(), prototypeLinkName), m_statement.result);
    }

    // A placeholder above the objects is listed, and nothing that lies above it: whoever finds it on a chain looks through it by itself.
    ValueSetID& atCreation = m_context.rememberedByStatement();
    if (!atCreation)
        atCreation = prototypesAtCreationOf(start);
    ClimbingSet above = prototypesOf(start, atCreation);
    ValueFlowNodeID chainAbove = m_context.lookup(known(above), prototypeLinkName);
    connect(chainAbove, m_statement.result);
    add(m_statement.result, m_context.restrict(above, m_universe.placeholdersAlone()));
    if (mayBeUnknownObject(above)) {
        ValueSetID objectsOfProgram = m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
        if (ValueFlowNodeID links = readWhereWritten(m_context.restrict(known(start), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink())), prototypeLinkName))
            connect(m_context.masked(m_context.lookup(m_context.valueWithin(ClimbingSet(objectsOfProgram), links), prototypeLinkName), m_universe.holdersOfLibraryExports()), m_statement.result);
    }
    // A copy takes the own properties of its source: what lies above the source stays behind.
    above = m_context.unite(above, withSourcesOfCopies(m_context, known(valueOfNodeIfAny(readWhereWritten(known(start), copiedFromName)))));
    add(m_statement.result, known(above));

    // Nobody sees which object a read or a store through the untracked object goes to.
    if (m_context.numberOfMembersOfKey() != 1 || !hasBeenLost(known(start)))
        return;
    ClimbingSet chain = m_context.unite(above, m_context.valueOf(chainAbove));
    if (mayHoldAccessorNobodyKnows(chain, unknownHeapKey, HeapPropertyPart::Getter) || mayHoldAccessorNobodyKnows(chain, unknownHeapKey, HeapPropertyPart::Setter))
        add(ValueFlowProgramNodes::escaped(), start);
}

// The untracked object stands for whatever the analysis has stopped following, so what lies above one of those objects lies above it. Each of them is looked at once, when it arrives.
void HeapRule::collectChainOfUntrackedObject()
{
    m_context.forEachMember(known(m_context.newSince(ValueFlowProgramNodes::lost())), [&](ValueFlowObjectID object) {
        connect(m_context.lookup(ClimbingSet(only(object)), prototypeLinkName), m_statement.result);
    });
}

// The analysis follows a built-in that lets in unseen code from the read that names it. The statements that stand for what native code may do name nothing, and neither does a lookup, which they share with everybody.
void HeapRule::readMember(BuiltinID member, ModuleID module, ValueFlowNodeID result, Members& getters)
{
    if (m_tables.letsInUnseenCode(member) && !isOperationOfFunction())
        return;
    use(member, BuiltinUse::Read);
    // The `exports` of the module object of a module of the image shows a variable of that module, and what the variable holds lies on the object like what is stored there.
    if (m_builtins.canonicalID(member) == Builtins::ModuleObjectExports) {
        if (!module)
            add(result, unknownValue(ValueFlowUnknownSource::UndescribedNativeResult));
        return;
    }
    switch (m_builtins.at(member).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
        add(result, only(objectOfMember(member, module)));
        return;
    case BuiltinKind::DataProperty: {
        // The entry stands for a property of the object that the body of a compiled module makes: what lies there under the name is all there is to read.
        const Builtin& entry = m_builtins.at(member);
        ModuleID compiled = entry.holder && entry.name && !entry.flags.contains(BuiltinFlag::IsDefaultExport) ? m_tables.compiledModuleOf(m_builtins.canonicalID(entry.holder)) : ModuleID();
        if (compiled) {
            connect(m_context.lookup(valueOf(m_context, requiredValueOf(m_context, compiled)), ValueFlowName::property(m_program.keyOf(entry.name))), result);
            return;
        }
        // An own property of the instances whose type the class says lies on the object that stands for an instance. What the prototype object itself has under the name is of that type too.
        BuiltinClassID declaringClass = entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) && entry.holder ? m_builtins.at(entry.holder).instanceClass : BuiltinClassID();
        if (BuiltinTypeID type = declaringClass ? m_builtins.typeOfNameDefinedOnNewInstances(declaringClass, entry.name) : BuiltinTypeID()) {
            add(result, m_universe.builtinTables().primitivesOf(type));
            return;
        }
        getters.append(objectOfMember(member, module));
        return;
    }
    case BuiltinKind::Accessor:
        getters.append(objectOfMember(member, module));
        return;
    case BuiltinKind::Constant: {
        BuiltinName symbol = m_builtins.nameOfSymbol(m_builtins.canonicalID(member));
        if (PropertyKeyID key = symbol ? m_program.keyOf(symbol) : PropertyKeyID())
            add(result, only(m_universe.keyConstant(key)));
        else
            add(result, m_universe.anyPrimitive());
        return;
    }
    case BuiltinKind::NativeBinding:
        add(result, unknownValue(ValueFlowUnknownSource::UndescribedNativeResult));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether a read (Getter) or a store (Setter) under the key that comes by these may find an accessor nobody knows there.
bool HeapRule::mayHoldAccessorNobodyKnows(ClimbingSet chain, const HeapKey& key, HeapPropertyPart part)
{
    // Nobody lists what lies on a placeholder.
    if (m_context.meet(chain, m_universe.placeholdersAlone()))
        return true;
    ClimbingSet objects = known(chain);
    if (mayHoldUndeclaredProperty(objects, key))
        return true;

    ValueFlowName name = foundName(key, part);
    ClimbingSet writableObjects = m_context.restrict(objects, ValueSets::complement(holdersWithLockedLocationUnder(key)));
    bool isFound = mayBeAccessorNobodyKnows(valueOfNodeIfAny(readWhereWritten(writableObjects, name)));
    forEachNodeStoredThroughPlaceholdersInto(writableObjects, name, [&](ValueFlowNodeID stored) {
        isFound |= mayBeAccessorNobodyKnows(m_context.valueOf(stored));
    });
    return isFound;
}

// A closure is created with a length and, where its function is named, with a name.
bool HeapRule::endsAtPropertyOfCreation(ValueFlowObjectID receiver, const HeapKey& key)
{
    if (key.keyClass != HeapKeyClass::Name || m_universe.kindOf(receiver) != ValueFlowObjectKind::Closure)
        return false;
    const Function& function = m_program.at(m_universe.closureFunction(receiver));
    if (function.constructedClass || (key.name != m_program.keyOf(BuiltinNames::length) && (key.name != m_program.keyOf(BuiltinNames::name) || !function.name)))
        return false;
    ClimbingSet closure(only(receiver));
    return !hasEscaped(closure) && !hasBeenLost(closure) && !holdsAnything(m_context.markedAmong(closure, { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty }));
}

// Only a receiver that inherits an accessor nobody knows has to be handed to it.
ClimbingSet HeapRule::receiversBelowAccessorNobodyKnows(ClimbingSet start, const HeapKey& key, HeapPropertyPart part)
{
    auto mayLieAbove = [&](ClimbingSet receivers) {
        ClimbingSet above = objectsAbove(receivers);
        ValueFlowNodeID chainAbove = m_context.lookup(known(above), prototypeLinkName);
        return mayHoldAccessorNobodyKnows(chainUpToPrototypeObjectsOfEntries(above, chainAbove, key).value_or(m_context.unite(above, m_context.valueOf(chainAbove))), key, part);
    };
    // Whoever is found here escapes, and that is all that happens to it. A receiver that has escaped is not asked about again: the largest sets of receivers hold little else.
    start = m_context.membersMissingFrom(ValueFlowProgramNodes::escaped(), start);
    // Where it lies on receivers alone, as what the table does not declare lies on a built-in object, nobody is looked at by itself.
    if (!holdsAnything(start) || !mayLieAbove(start))
        return { };

    // One receiver has just been looked at.
    bool isOneReceiver = m_context.holdsOneMember(start);
    Members receivers;
    m_context.forEachMember(start, [&](ValueFlowObjectID receiver) {
        if (!endsAtPropertyOfCreation(receiver, key) && (isOneReceiver || mayLieAbove(ClimbingSet(only(receiver)))))
            receivers.append(receiver);
    });
    return ClimbingSet(m_context.maker().of(receivers.span()));
}

void HeapRule::callAccessors(ClimbingSet functions, ValueFlowCallKind kind, const HeapReceiver& receiver, const ValueFlowOperand* value, ValueFlowNodeID result)
{
    // An accessor or a data property of the table stands for the function behind it, which no read hands out.
    if (!m_context.meet(functions, m_tables.accessors()))
        return;

    if (m_accessorFunctor) {
        m_context.forEachMember(m_context.restrict(functions, m_tables.accessors()), [&](ValueFlowObjectID function) {
            (*m_accessorFunctor)(function == m_tables.undescribedNativeCode() ? ValueFlowUniverse::unknownObject() : function, kind);
        });
    }

    // Native code that nothing describes looks at what it is given. A built-in object is nothing new to it.
    if (m_context.contains(functions, m_tables.undescribedNativeCode())) {
        passToAccessorNobodyKnows(receiver, m_tables.whatIsNewToNativeCode());
        if (value)
            connect(*value, ValueFlowProgramNodes::escaped());
        add(result, m_undescribedNativeCodeIsOfEngine ? undescribedValueThatIsNoBuffer(m_context) : unknownValue(ValueFlowUnknownSource::UndescribedNativeResult));
    }

    // An accessor nobody knows, on an object nobody knows, may be a function that has escaped, which is entered with the unknown object as every operand.
    if (mayBeUnknownObject(functions)) {
        passToAccessorNobodyKnows(receiver, ValueSets::anything);
        if (value)
            connect(*value, ValueFlowProgramNodes::escaped());
        add(result, unknownValue(ValueFlowUnknownSource::ResultOfUnknownCallee));
    }

    // A function that the analysis has stopped following takes the operands like any other: the group of the untracked object passes them on.
    forEachPartOfCallees(m_context, m_context.restrict(functions, m_tables.knownAccessors()), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = m_context.group(callees, kind);
        pass(receiver, group.thisValue);
        if (value)
            connect(*value, group.argument(0));
        connect(group.result, result);
        connect(group.thrown, ValueFlowProgramNodes::escaped());
    });
}

// A lookup says what stands for the prototype object that a function has from the start and hands it to nobody: `instanceof` compares it, a construction puts it above what it makes.
void HeapRule::handOutPrototypeObjectsAtCreation(ClimbingSet receivers, const HeapKey& key)
{
    if (key.keyClass == HeapKeyClass::Index || (key.keyClass == HeapKeyClass::Name && key.name != m_tables.prototypeKey()))
        return;
    m_context.forEachMember(m_context.restrict(key.keyClass == HeapKeyClass::Name ? chainFrom(receivers).objects : receivers, m_universe.closures()), [&](ValueFlowObjectID closure) {
        const Function& function = m_program.at(m_universe.closureFunction(closure));
        if (!function.constructedClass && function.has(FunctionFlags::IsGenerator))
            add(m_statement.result, unknownObject(ValueFlowUnknownSource::ObjectWithoutLayout));
    });
}

// The unknown object may be a proxy, whose handler is in unknown hands like the proxy. Only the results have to hear of it.
void HeapRule::noteTrapOfUnknownObject(ClimbingSet objects, ValueFlowCallKind kind)
{
    if (m_accessorFunctor && mayBeUnknownObject(objects))
        (*m_accessorFunctor)(ValueFlowUniverse::unknownObject(), kind);
}

bool HeapRule::mayNamePropertyAtCreation(const HeapKey& key) const
{
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        return key.name == m_tables.prototypeKey() || key.name == m_tables.constructorKey() || key.name == m_tables.rawKey();
    case HeapKeyClass::Index:
    case HeapKeyClass::Unknown:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A function that can construct has a prototype object from the start, and that object names the function as its constructor.
void HeapRule::readPropertiesAtCreation(ClimbingSet objects, const HeapKey& key, ValueFlowNodeID result)
{
    // None of the three is enumerable.
    auto mayName = [&](PropertyKeyID name) {
        return (key.keyClass == HeapKeyClass::Unknown && !key.namesEnumerableProperty) || (key.keyClass == HeapKeyClass::Name && key.name == name);
    };

    if (mayName(m_tables.prototypeKey())) {
        m_context.forEachMember(m_context.restrict(objects, m_universe.closures()), [&](ValueFlowObjectID closure) {
            const Function& function = m_program.at(m_universe.closureFunction(closure));
            if (function.constructedClass)
                add(result, only(m_universe.prototypeOf(function.constructedClass)));
            else if (function.has(FunctionFlags::IsGenerator))
                // The prototype object of a generator function lies above its generators and nowhere else, so the object that stands for them stands for it too. Whoever reads it out says that it is no generator.
                add(result, only(m_universe.coroutineResultOf(m_universe.closureFunction(closure))));
            else if (functionHasPrototypeProperty(m_program, function))
                add(result, only(m_universe.prototypeObjectOf(m_universe.closureFunction(closure))));
        });
    }
    if (mayName(m_tables.constructorKey())) {
        m_context.forEachMember(objects, [&](ValueFlowObjectID object) {
            LayoutID layout = m_universe.classLayoutOf(object);
            if (FunctionID constructor = layout ? m_program.at(layout).constructor : FunctionID())
                add(result, only(m_universe.closureOf(constructor)));
            else if (m_universe.kindOf(object) == ValueFlowObjectKind::FunctionPrototype)
                add(result, only(m_universe.closureOf(m_universe.functionOf(object))));
        });
    }
    // The array of the raw strings of a template object is followed by nobody. No statement fills the elements: each is a string, or undefined where the text has an escape that means nothing.
    if (!m_context.meet(objects, m_tables.templateObjects()))
        return;
    if (mayName(m_tables.rawKey()))
        add(result, unknownObject(ValueFlowUnknownSource::ObjectWithoutLayout));
    if (key.keyClass != HeapKeyClass::Name)
        add(result, m_context.maker().unite(m_universe.strings(), m_universe.undefinedValue()));
}

// Under a key nobody knows every member is read.
bool HeapRule::isNativeModuleThatNobodyNames(ValueFlowObjectID holder)
{
    return m_context.sets().contains(m_tables.objectsOfNativeModules(), holder) && !m_context.holdsAnyOf(ClimbingSet(only(holder)), m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins));
}

// A member of Bun is used where the program names it on Bun: `Bun.file`, `import { file } from "bun"`.
bool HeapRule::isMemberOfBun(BuiltinID member) const
{
    BuiltinID holder = m_builtins.at(member).holder;
    return holder && m_builtins.canonicalID(holder) == Builtins::Bun;
}

bool HeapRule::hasMemberOfHost(ValueFlowObjectID holder) const
{
    return std::ranges::any_of(m_builtins.membersOf(m_universe.builtinOf(holder)), [&](BuiltinID member) {
        return m_builtins.at(m_builtins.canonicalID(member)).origin == BuiltinOrigin::Host;
    });
}

bool HeapRule::isAsEnumerationHasSeenIt(const HeapChain& chain)
{
    constexpr OptionSet<ObjectMutation> changes { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
    return !holdsAnything(m_context.markedAmong(chain.objects, changes));
}

// The enumeration has listed the name, so an object of the program on the chain had an enumerable property under it.
void HeapRule::readWhatNamesOfProgramMayHaveHidden(const HeapChain& chain, ValueFlowNodeID result, Members& getters)
{
    ClimbingSet objectsOfProgram = m_context.restrict(chain.objects, ValueSets::complement(m_universe.builtinObjects()));
    auto hasWritten = [&](PropertyKeyID name) {
        if (!name)
            return false;
        HeapKey key = keyNamed(name);
        bool isWritten = false;
        auto note = [&](ValueFlowNodeID node) {
            isWritten |= holdsAnything(node);
        };
        forEachNodeHoldingValueUnder(key, objectsOfProgram, note);
        for (HeapPropertyPart part : { HeapPropertyPart::Getter, HeapPropertyPart::Setter }) {
            note(m_context.read(objectsOfProgram, foundName(key, part)));
            forEachNodeStoredThroughPlaceholdersInto(objectsOfProgram, foundName(key, part), note);
        }
        return isWritten;
    };

    bool isRedefined = holdsAnything(m_context.markedAmong(objectsOfProgram, ObjectMutation::RedefineProperty));
    for (PropertyKeyID name : { m_tables.prototypeKey(), m_tables.constructorKey(), m_tables.rawKey() }) {
        if (isRedefined || hasWritten(name))
            readPropertiesAtCreation(chain.objects, keyNamed(name), result);
    }
    m_context.forEachMember(m_context.restrict(chain.objects, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
        // readMembers() has read all of these.
        if (hasMemberOfHost(holder))
            return;
        bool holderIsRedefined = holdsAnything(m_context.markedAmong(ClimbingSet(only(holder)), ObjectMutation::RedefineProperty));
        for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder))) {
            if (isFoundByNameAlone(m_builtins.canonicalID(member)))
                continue;
            if (holderIsRedefined || hasWritten(m_program.keyOf(m_builtins.at(member).name)))
                readMember(member, m_universe.moduleOf(holder), result, getters);
        }
    });
}

void HeapRule::readMembers(ClimbingSet objects, ClimbingSet start, const HeapKey& key, ValueFlowNodeID result, Members& getters)
{
    if (key.keyClass != HeapKeyClass::Unknown) {
        forEachMemberUnder(objects, key, [&](BuiltinID member, ModuleID module) {
            readMember(member, module, result, getters);
        });
        return;
    }

    ClimbingSet holders = m_context.restrict(objects, m_tables.holdersWithMembers());
    if (!holdsAnything(holders))
        return;
    m_context.lookup(holders, ValueFlowName::of(ValueFlowNameKind::EveryMemberRead));
    m_context.forEachMember(holders, [&](ValueFlowObjectID holder) {
        // No member that the engine makes is enumerable.
        if (key.namesEnumerableProperty && !hasMemberOfHost(holder))
            return;
        if (ModuleID module = m_universe.moduleOf(holder)) {
            for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder))) {
                if (!isFoundByNameAlone(m_builtins.canonicalID(member)))
                    readMember(member, module, result, getters);
            }
            return;
        }
        if (isNativeModuleThatNobodyNames(holder) || m_builtins.canonicalID(m_universe.builtinOf(holder)) == Builtins::Bun) {
            add(result, unknownValue(ValueFlowUnknownSource::UndescribedNativeResult));
            return;
        }
        const ValueFlowMembersOfHolder& members = m_tables.membersOf(m_universe.builtinOf(holder));
        add(result, members.values);
        if (hasMemberOfHost(holder))
            add(m_context.nodes().ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders), m_context.maker().intersect(members.values, m_universe.builtinFunctions()));
        // Only of a holder that the lookup starts at. Above every function lies Function.prototype, whose `constructor` is Function: should `f[key]` answer that, the run time refuses what the program then adds to it.
        if (m_context.contains(start, holder))
            add(m_context.nodes().ofHost(ValueFlowHostNode::BuiltinsHeldWithoutBeingListed), members.leftOut);
        if (members.hasNativeBinding)
            add(result, unknownValue(ValueFlowUnknownSource::UndescribedNativeResult));
        m_context.sets().forEachMember(members.accessors, [&](ValueFlowObjectID accessor) {
            BuiltinID member = m_builtins.canonicalID(m_universe.builtinOf(accessor));
            if (!isFoundByNameAlone(member) || (member == Builtins::ProcessMainModule && namesWaysToModuleObjects(m_program)))
                getters.append(accessor);
        });
    });
    // What is known for a holder leaves out the members that let in unseen code, and a key nobody knows names none of them: see readEvaluators(). Nor a table of the loader: see noteUseOfMembers().
}

// A lookup leaves out the members that let in unseen code, so a read in a function finds them by itself, on the objects that the analysis follows. Whether seen code runs the function counts only where the member is applied.
void HeapRule::readEvaluators(const ValueFlowOperand& object, const HeapKey& key)
{
    ValueSetID holders = ValueSets::nothing;
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        holders = m_tables.under(key.name).holdersOfEvaluators;
        break;
    case HeapKeyClass::Index:
    // `globalThis[key]` names none of them. What may be an evaluator is none: the run time refuses it in an image whose build saw no application of one.
    case HeapKeyClass::Unknown:
        break;
    }
    if (holders == ValueSets::nothing || !isOperationOfFunction())
        return;

    Members getters;
    HeapChain chain = chainFrom(m_context.valueOf(object), key);
    ClimbingSet found = m_context.restrict(chain.objects, holders);
    // Beside the unknown object nobody is shown a holder that has escaped, and the unknown object answers with no evaluator. So the links are walked by what they list.
    if (chain.holdsUnknownObject) {
        constexpr unsigned longestChain = 8;
        ClimbingSet above = known(chain.start);
        for (unsigned step = 0; step < longestChain && holdsAnything(above); ++step) {
            found = m_context.unite(found, m_context.restrict(above, holders));
            ClimbingSet next(prototypesAtCreationOf(above));
            if (ValueFlowNodeID links = readWhereWritten(above, prototypeLinkName))
                next = m_context.unite(next, m_context.valueWithin(ClimbingSet(m_universe.knownObjects()), links));
            above = known(next);
        }
    }
    forEachMemberUnder(found, key, [&](BuiltinID member, ModuleID module) {
        if (m_tables.letsInUnseenCode(member))
            readMember(member, module, m_statement.result, getters);
    });
    callAccessors(ClimbingSet(m_context.maker().of(getters.span())), ValueFlowCallKind::Get, { &object, { } }, nullptr, m_statement.result);
}

// While nobody deletes the member, a read that comes to the holder finds it and answers what the table says.
bool HeapRule::answersWithMember(ValueFlowObjectID holder, PropertyKeyID name) const
{
    return answers(memberUnder(m_universe.builtinOf(holder), name));
}

bool HeapRule::answers(BuiltinID member) const
{
    if (!member || m_tables.letsInUnseenCode(member))
        return false;
    const Builtin& entry = m_builtins.at(member);
    if (entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) || entry.mutability == BuiltinMutability::ChangesOnItsOwn || !m_builtins.existsOn(member, targetOfThisBuild().platform))
        return false;
    if (m_builtins.has(BuiltinMark::MayBeAbsent, member))
        return false;
    return entry.kind != BuiltinKind::Accessor || m_tables.hasGetEntry(member);
}

// Whichever of the objects a lookup under the name starts at, and whatever the program has done, it comes to a built-in holder whose location under the name is locked.
bool HeapRule::surelyComesToMember(ClimbingSet start, PropertyKeyID name)
{
    constexpr unsigned longestChain = 8;
    const ValueFlowMembersUnderKey& members = m_tables.under(name);
    if (m_context.maker().intersect(members.holders, members.holdersWithLockedLocation) == ValueSets::nothing)
        return false;

    ClimbingSet current = start;
    for (unsigned step = 0; step < longestChain; ++step) {
        ClimbingSet above;
        ClimbingSet objectsThatMayGetAnotherLink = m_context.restrict(known(current), m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink())));
        if (holdsAnything(objectsThatMayGetAnotherLink)) {
            above = valueOfNodeIfAny(readWhereWritten(objectsThatMayGetAnotherLink, prototypeLinkName));
            bool isStoredThroughPlaceholder = false;
            forEachNodeStoredThroughPlaceholdersInto(objectsThatMayGetAnotherLink, prototypeLinkName, [&](ValueFlowNodeID) {
                isStoredThroughPlaceholder = true;
            });
            if (isStoredThroughPlaceholder || m_context.meet(above, m_universe.placeholdersAlone()) || m_context.contains(above, ValueFlowUniverse::primitive(PrimitiveSet::Null)))
                return false;
            above = m_context.restrict(above, m_universe.objects());
        }

        Members atCreation;
        bool mayEndHere = false;
        m_context.forEachMember(current, [&](ValueFlowObjectID object) {
            size_t numberOfOthers = atCreation.size();
            appendPrototypeAtCreation(object, atCreation);
            const ValueFlowSite* site = m_universe.siteOf(object);
            mayEndHere |= atCreation.size() == numberOfOthers && !(site && site->origin == ValueFlowSiteOrigin::BuiltinResult);
        });
        above = m_context.unite(above, ClimbingSet(m_context.maker().of(atCreation.span())));
        if (mayEndHere || m_context.contains(above, m_tables.undescribedNativeCode()))
            return false;

        bool everyHolderAnswers = true;
        m_context.forEachMember(m_context.restrict(above, members.holders), [&](ValueFlowObjectID holder) {
            everyHolderAnswers &= m_context.sets().contains(members.holdersWithLockedLocation, holder) && answersWithMember(holder, name);
        });
        if (!everyHolderAnswers)
            return false;
        current = m_context.restrict(above, ValueSets::complement(members.holders));
        if (!holdsAnything(current))
            return true;
    }
    return false;
}

void HeapRule::readThroughChain(ClimbingSet start, const HeapKey& keyOfRead, const HeapReceiver& receiver, ValueFlowNodeID result)
{
    Members builtinGetters;
    HeapChain chain = chainFrom(start, keyOfRead);
    // A proxy lists what it likes, and a proxy is the unknown object.
    const HeapKey& key = keyOfRead.namesEnumerableProperty && (chain.holdsUnknownObject || chain.holdsUntrackedObject) ? unknownHeapKey : keyOfRead;
    ValueFlowName value = foundName(key, HeapPropertyPart::Value);
    ValueFlowName getter = foundName(key, HeapPropertyPart::Getter);
    ClimbingSet getters;
    // A table of the program that holds `eval` among its members and is read under a key nobody knows may answer it, and what may be an evaluator is none.
    bool handsOutEvaluators = key.keyClass != HeapKeyClass::Unknown;
    auto answer = [&](ValueFlowNodeID node) {
        connect(handsOutEvaluators ? node : m_context.masked(node, ValueSets::complement(m_tables.objectsThatLetInUnseenCode())), result);
    };
    auto takeGetters = [&](ValueFlowNodeID node) {
        getters = m_context.unite(getters, valueOfNodeIfAny(node));
    };

    // Only a lookup that may come to the end of a chain answers undefined for a property that is not there, and one that starts nowhere answers nothing.
    bool mayFindNothing = chain.holdsUnknownObject || chain.holdsUntrackedObject || (key.keyClass != HeapKeyClass::Name && holdsAnything(chain.start));
    if (!mayFindNothing && key.keyClass == HeapKeyClass::Name) {
        ClimbingSet startThatGoesUp = m_context.restrict(chain.start, ValueSets::complement(instancesThatKeepProperty(chain.start, key.name)));
        mayFindNothing = holdsAnything(startThatGoesUp) && !surelyComesToMember(startThatGoesUp, key.name);
        m_context.forEachMember(m_context.restrict(chain.holdersThatAnswer, m_tables.under(key.name).holders), [&](ValueFlowObjectID holder) {
            mayFindNothing |= !answersWithMember(holder, key.name);
        });
        if (!mayFindNothing && (holdsAnything(startThatGoesUp) || holdsAnything(chain.holdersThatAnswer))) {
            ValueFlowName setter = foundName(key, HeapPropertyPart::Setter);
            ValueSetID holders = m_tables.under(key.name).holders;
            ClimbingSet writableHolders = m_context.restrict(chain.writableObjects, holders);
            ClimbingSet otherWritableObjects = m_context.restrict(chain.writableObjects, m_context.maker().intersect(m_universe.objects(), ValueSets::complement(holders)));
            // Whoever makes an accessor of a property writes what the descriptor holds as `get` and `set` under the name, undefined too. An object that is marked for another name has none under this one.
            ClimbingSet redefinedObjects = m_context.markedAmong(otherWritableObjects, ObjectMutation::RedefineProperty);
            mayFindNothing = (holdsAnything(writableHolders) && holdsAnything(readWhereWritten(writableHolders, setter)))
                || (holdsAnything(otherWritableObjects) && holdsAnything(readWhereWritten(otherWritableObjects, setter)))
                || (holdsAnything(redefinedObjects) && holdsAnything(readWhereWritten(redefinedObjects, getter)));
            forEachNodeStoredThroughPlaceholdersInto(chain.writableObjects, setter, [&](ValueFlowNodeID) {
                mayFindNothing = true;
            });
        }
    }
    if (mayFindNothing)
        add(result, m_universe.undefinedValue());
    forEachNodeHoldingValueUnder(key, chain.writableObjects, answer);
    takeGetters(readWhereWritten(chain.writableObjects, getter));
    forEachNodeStoredThroughPlaceholdersInto(chain.writableObjects, getter, takeGetters);

    readPropertiesAtCreation(chain.objects, key, result);
    readMembers(chain.objects, chain.start, key, result, builtinGetters);
    if (key.namesEnumerableProperty && !isAsEnumerationHasSeenIt(chain))
        readWhatNamesOfProgramMayHaveHidden(chain, result, builtinGetters);
    if (mayHoldUndeclaredProperty(chain.objects, key))
        builtinGetters.append(m_tables.undescribedNativeCode());

    // The method by which a class of the host prints itself calls a function of a start-up module, whoever calls the method: the formatter, or the program, which has it from here.
    for (BuiltinName name : m_builtins.namesOfHostMethodsCallingStartupLoads()) {
        if (key.keyClass != HeapKeyClass::Name || m_program.keyOf(name) != key.name)
            continue;
        bool findsMethodOfHost = chain.holdsUnknownObject && mayBeFoundOnObjectNobodyKnows(m_context, name);
        m_context.forEachMember(m_context.restrict(chain.objects, m_context.maker().unite(m_tables.under(key.name).holders, holdersOfUndeclaredPropertiesUnder(key))), [&](ValueFlowObjectID holder) {
            BuiltinID builtin = m_universe.builtinOf(holder);
            findsMethodOfHost |= builtin && m_builtins.at(builtin).origin == BuiltinOrigin::Host;
        });
        if (findsMethodOfHost)
            letHostChooseOperands(m_context);
    }

    // The untracked object answers by name. What it answers holds a placeholder only where a value that was written does.
    if (chain.holdsUntrackedObject) {
        forEachNodeReadByName(value, answer);
        forEachNodeReadByName(getter, takeGetters);
        if (mayNamePropertyAtCreation(key))
            readPropertiesAtCreation(foundThroughUntrackedObject(m_universe.knownObjects()), key, result);
        forEachMemberUnder(foundThroughUntrackedObject(holdersWithMemberUnder(key)), key, [&](BuiltinID member, ModuleID module) {
            if (isHandedOutThroughPlaceholder(member, key))
                readMember(member, module, result, builtinGetters);
        });
        if (holdsAnything(foundThroughUntrackedObject(holdersOfUndeclaredPropertiesUnder(key))))
            builtinGetters.append(m_tables.undescribedNativeCode());
        // The length of an array, the name of a function, a character of a string.
        add(result, m_universe.anyPrimitive());
    }

    if (mayBeUnknownObject(start)) {
        add(result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        ClimbingSet holdersOfExports = known(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::HoldersOfExportsInEscapedObjects)));
        if (key.keyClass == HeapKeyClass::Name && holdsAnything(holdersOfExports)) {
            answer(m_context.read(holdersOfExports, value));
            takeGetters(m_context.read(holdersOfExports, getter));
        }
        // The unknown object stands for an instance that has escaped, and the class above an instance has not escaped with it.
        if (key.keyClass == HeapKeyClass::Unknown || (key.keyClass == HeapKeyClass::Name && key.name == m_tables.constructorKey()))
            connect(m_context.nodes().ofHost(ValueFlowHostNode::ConstructorsAboveEscapedInstances), result);
        // A constructor, a prototype object, a namespace, and under a key nobody knows every member, is handed out as the unknown object.
        bool mayBeCalled = mayCallWhateverHasEscaped(m_context);
        Members handedOutAsUnknownObject;
        forEachMemberUnder(holdersFoundThroughUnknownObject(key), key, [&](BuiltinID member, ModuleID module) {
            if (!isHandedOutThroughPlaceholder(member, key))
                return;
            BuiltinKind kind = m_builtins.at(member).kind;
            bool holdsMembers = kind == BuiltinKind::Constructor || kind == BuiltinKind::Prototype || kind == BuiltinKind::Namespace;
            if (key.keyClass == HeapKeyClass::Name && !holdsMembers) {
                readMember(member, module, result, builtinGetters);
                return;
            }
            // The read of what is no object runs the code behind it. An object has only come into the hands of whoever has the value: no statement has it as itself.
            if (!isObjectKind(m_builtins.at(member).kind)) {
                use(member, mayBeCalled ? OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped });
                return;
            }
            m_context.useAsAnswerOfUnknownObject(member, mayBeCalled ? OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped });
            handedOutAsUnknownObject.append(objectOfMember(member, module));
        });
        add(ValueFlowProgramNodes::escaped(), m_context.maker().of(handedOutAsUnknownObject.span()));
    } else if (chain.holdsUnknownObject) {
        // One lookup for each name records what is found through the unknown object.
        connect(m_context.lookup(unknownObjectAlone(), value), result);
    }
    if (chain.holdsUnknownObject)
        builtinGetters.append(ValueFlowUniverse::unknownObject());
    // A string has its characters under the indexes, whether it is a primitive or wrapped, and a typed array its numbers.
    if (key.keyClass != HeapKeyClass::Name && m_context.meet(chain.objects, m_tables.prototypesAboveNativeElements()))
        add(result, m_universe.anyPrimitive());

    bool undescribedNativeCodeIsOfEngine = !chain.holdsUntrackedObject && !m_context.contains(getters, m_tables.undescribedNativeCode());
    m_context.forEachMember(m_context.restrict(chain.objects, holdersOfUndeclaredPropertiesUnder(key)), [&](ValueFlowObjectID holder) {
        BuiltinID builtin = m_universe.builtinOf(holder);
        undescribedNativeCodeIsOfEngine &= builtin && m_builtins.at(builtin).origin == BuiltinOrigin::Engine;
    });
    getters = m_context.unite(getters, ClimbingSet(m_context.maker().of(builtinGetters.span())));

    HeapReceiver thisValue = receiver;
    if (!receiver.operand && mayBeAccessorNobodyKnows(getters))
        thisValue.belowAccessorNobodyKnows = receiversBelowAccessorNobodyKnows(chain.start, key, HeapPropertyPart::Getter);
    SetForScope ofEngine(m_undescribedNativeCodeIsOfEngine, undescribedNativeCodeIsOfEngine);
    callAccessors(getters, ValueFlowCallKind::Get, thisValue, nullptr, result);
}

// Whether a lookup under a name that the program does not spell ends at a member of the table, on the entry or on what the entry is born below.
bool HeapRule::endsAtMemberOfTable(ClimbingSet entries, BuiltinName name)
{
    bool everyEntryComesToMember = true;
    Members holders;
    m_context.forEachMember(entries, [&](ValueFlowObjectID object) {
        holders.append(object);
        BuiltinID holder = m_universe.builtinOf(object);
        if (answers(m_builtins.findOwnMember(holder, name)))
            return;
        const Builtin& entry = m_builtins.at(holder);
        BuiltinID above = entry.prototypeLink == BuiltinPrototypeLink::Builtin ? m_builtins.canonicalID(entry.inheritsFrom) : BuiltinID();
        if (!above || !answers(m_builtins.findOwnMember(above, name))) {
            everyEntryComesToMember = false;
            return;
        }
        holders.append(m_universe.objectOf(above));
        ClimbingSet alone(only(object));
        everyEntryComesToMember &= !holdsAnything(m_context.read(alone, prototypeLinkName));
        forEachNodeStoredThroughPlaceholdersInto(alone, prototypeLinkName, [&](ValueFlowNodeID) {
            everyEntryComesToMember = false;
        });
    });
    if (!everyEntryComesToMember)
        return false;

    ClimbingSet objects(m_context.maker().of(holders.span()));
    ValueFlowName deleted = ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey);
    bool mayBeDeleted = holdsAnything(m_context.read(objects, deleted));
    forEachNodeStoredThroughPlaceholdersInto(objects, deleted, [&](ValueFlowNodeID) {
        mayBeDeleted = true;
    });
    return !mayBeDeleted;
}

// Native code looks the name up on entries of the table.
void HeapRule::letNativeCodeRead(ClimbingSet entries, const ValueFlowNameReadByNativeCode& read)
{
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    ClimbingSet found;
    auto handOver = [&](ValueFlowNodeID node) {
        connect(node, escaped);
        found = m_context.unite(found, m_context.valueOf(node));
    };
    auto objectsOfProgramOn = [&](const HeapChain& chain) {
        return m_context.restrict(chain.writableObjects, ValueSets::complement(m_universe.builtinObjects()));
    };

    HeapChain chain;
    ValueFlowName value;
    ValueFlowName getter;
    if (PropertyKeyID name = read.key) {
        HeapKey key { HeapKeyClass::Name, name };
        value = foundName(key, HeapPropertyPart::Value);
        getter = foundName(key, HeapPropertyPart::Getter);
        // A member that is not there in every run sends the lookup on.
        bool everyEntryAnswers = true;
        m_context.forEachMember(m_context.restrict(entries, m_tables.under(name).holders), [&](ValueFlowObjectID holder) {
            everyEntryAnswers &= answersWithMember(holder, name);
        });
        chain = everyEntryAnswers ? chainFrom(entries, key) : chainThatGoesUpFrom(entries, key);
        ClimbingSet objects = objectsOfProgramOn(chain);
        if (!read.isOnlyInspected) {
            forEachNodeHoldingValueUnder(key, objects, handOver);
            readPropertiesAtCreation(objects, key, escaped);
        }
        handOver(m_context.read(objects, getter));
        forEachNodeStoredThroughPlaceholdersInto(objects, getter, handOver);
    } else {
        if (endsAtMemberOfTable(entries, read.name))
            return;
        // A name that the program does not spell lies only where something was stored under a key nobody knows.
        value = writtenName(unknownHeapKey, HeapPropertyPart::Value);
        getter = writtenName(unknownHeapKey, HeapPropertyPart::Getter);
        chain = chainFrom(entries);
        ClimbingSet objects = objectsOfProgramOn(chain);
        for (ValueFlowName written : { value, getter }) {
            if (written == value && read.isOnlyInspected)
                continue;
            handOver(m_context.read(objects, written));
            forEachNodeStoredThroughPlaceholdersInto(objects, written, handOver);
        }
    }
    if (chain.holdsUntrackedObject) {
        if (!read.isOnlyInspected)
            forEachNodeReadByName(value, handOver);
        forEachNodeReadByName(getter, handOver);
    }
    if (m_context.meet(found, m_context.maker().intersect(m_universe.callables(), m_universe.knownObjects())))
        add(escaped, entries);
}

void HeapRule::collectAccessors(BuiltinCallMode mode)
{
    ValueFlowName name = m_context.nameOfKey();
    HeapKey key = keyOfFoundName(name);
    Members accessors;
    auto appendIfAccessor = [&](BuiltinID member, ModuleID module) {
        if (mode == BuiltinCallMode::Get ? m_tables.hasGetEntry(member) : m_tables.hasSetEntry(member))
            accessors.append(objectOfMember(member, module));
    };
    HeapChain chain = chainFrom(m_context.setOfKey(), key);

    auto answer = [&](ValueFlowNodeID node) {
        connect(node, m_statement.result);
    };
    answer(readWhereWritten(chain.writableObjects, name));
    forEachNodeStoredThroughPlaceholdersInto(chain.writableObjects, name, answer);

    auto appendAccessorsOf = [&](ClimbingSet objects) {
        if (key.keyClass != HeapKeyClass::Unknown) {
            forEachMemberUnder(objects, key, appendIfAccessor);
            return;
        }
        m_context.forEachMember(m_context.restrict(objects, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
            if (ModuleID module = m_universe.moduleOf(holder)) {
                for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder)))
                    appendIfAccessor(member, module);
                return;
            }
            const ValueFlowMembersOfHolder& members = m_tables.membersOf(m_universe.builtinOf(holder));
            add(m_statement.result, mode == BuiltinCallMode::Get ? members.accessorsWithGetEntry : members.accessorsWithSetEntry);
        });
    };
    appendAccessorsOf(chain.objects);
    if (mayHoldUndeclaredProperty(chain.objects, key))
        accessors.append(m_tables.undescribedNativeCode());
    if (chain.holdsUntrackedObject) {
        forEachNodeReadByName(name, answer);
        appendAccessorsOf(foundThroughUntrackedObject(holdersWithMemberUnder(key)));
        if (holdsAnything(foundThroughUntrackedObject(holdersOfUndeclaredPropertiesUnder(key))))
            accessors.append(m_tables.undescribedNativeCode());
    }
    if (chain.holdsUnknownObject) {
        accessors.append(ValueFlowUniverse::unknownObject());
        appendAccessorsOf(holdersOfAccessorsThatRunOnUnknownObject(key));
    }
    add(m_statement.result, m_context.maker().of(accessors.span()));
}

// One statement for each set of holders, so that no store and no read goes over the members by itself.
void HeapRule::noteUseOfMembers(const HeapKey& key, BuiltinUse noted)
{
    ClimbingSet start = m_context.setOfKey();
    ClimbingSet foundThroughPlaceholders;
    if (mayBeUnknownObject(start))
        foundThroughPlaceholders = escapedAmong(ClimbingSet(holdersWithMemberUnder(key)));
    if (mayBeUntrackedObject(start))
        foundThroughPlaceholders = m_context.unite(foundThroughPlaceholders, lostAmong(ClimbingSet(holdersWithMemberUnder(key))));
    // A key nobody knows names no table of the loader either: `require("node:module")[key]` is no use of the module cache.
    bool namesMember = key.keyClass == HeapKeyClass::Name;
    forEachMemberUnder(start, key, [&](BuiltinID member, ModuleID) {
        if (noted == BuiltinUse::Read && !namesMember && m_builtins.at(member).holder && isNativeModuleThatNobodyNames(m_universe.objectOf(m_builtins.at(member).holder)))
            return;
        if (!namesMember && isFoundByNameAlone(m_builtins.canonicalID(member)))
            return;
        if (noted == BuiltinUse::Read && !namesMember && isMemberOfBun(member))
            return;
        if (noted != BuiltinUse::Read || !m_tables.letsInUnseenCode(member))
            use(member, noted);
    });
    if (!holdsAnything(foundThroughPlaceholders))
        return;
    // `unknown[key]` names no member of the host, so it is no use of one: see isHandedOutThroughPlaceholder(). A store that may hit a member that the program does name is a store to it.
    Members mayBeNamed;
    Vector<BuiltinID, 16> membersThatMayBeNamed;
    forEachMemberUnder(foundThroughPlaceholders, key, [&](BuiltinID member, ModuleID) {
        if (isHandedOutThroughPlaceholder(member, key)) {
            use(member, noted);
            return;
        }
        if (noted == BuiltinUse::Read)
            return;
        mayBeNamed.append(m_universe.objectOf(member));
        membersThatMayBeNamed.append(member);
    });
    if (mayBeNamed.isEmpty())
        return;
    ClimbingSet named = m_context.valueWithin(ClimbingSet(m_context.maker().of(mayBeNamed.span())), m_context.nodes().ofHost(ValueFlowHostNode::UsedBuiltins));
    for (BuiltinID member : membersThatMayBeNamed) {
        if (m_context.contains(named, m_universe.objectOf(member)))
            use(member, noted);
    }
}

// What is found through a placeholder says nothing about the receivers beside it: a getter that is found by name, or one nobody knows, would get every other receiver as `this`.
bool HeapRule::splitLookupThroughPlaceholders()
{
    ClimbingSet start = m_context.setOfKey();
    ValueSetID others = ValueSets::complement(m_universe.placeholdersAlone());
    bool holdsUnknownObject = mayBeUnknownObject(start);
    bool holdsUntrackedObject = mayBeUntrackedObject(start);
    if (holdsUnknownObject + holdsUntrackedObject + m_context.meet(start, others) < 2)
        return false;
    connect(m_context.lookup(m_context.restrict(start, others), m_context.nameOfKey()), m_statement.result);
    ValueFlowName nameThroughPlaceholder = m_context.nameOfKey().kind == ValueFlowNameKind::EveryProperty ? ValueFlowName::of(ValueFlowNameKind::EveryProperty) : m_context.nameOfKey();
    if (holdsUntrackedObject)
        connect(m_context.lookup(untrackedObjectAlone(), nameThroughPlaceholder), m_statement.result);
    if (holdsUnknownObject)
        connect(m_context.lookup(unknownObjectAlone(), nameThroughPlaceholder), m_statement.result);
    return true;
}

void HeapRule::runLookup()
{
    ValueFlowName name = m_context.nameOfKey();
    if (name.kind == ValueFlowNameKind::PrototypeLink) {
        collectChain();
        return;
    }
    switch (name.kind) {
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
        noteUseOfMembers(keyOfFoundName(name), BuiltinUse::Written);
        return;
    case ValueFlowNameKind::EveryMemberRead:
        noteUseOfMembers(unknownHeapKey, BuiltinUse::Read);
        return;
    default:
        break;
    }
    if (splitLookupThroughPlaceholders())
        return;

    m_keepsHoldersOfExportsItStartsFrom = name == everyPropertyOfListedHoldersOfExports;
    switch (name.kind) {
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::EveryProperty:
        readThroughChain(m_context.setOfKey(), keyOfFoundName(name), { nullptr, m_context.setOfKey() }, m_statement.result);
        return;
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EverySetter:
        collectAccessors(BuiltinCallMode::Set);
        return;
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::EveryGetter:
        collectAccessors(BuiltinCallMode::Get);
        return;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runLookup(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    HeapRule rule(statement, context);
    rule.askWithoutNodesWhereNothingIsWritten();
    rule.runLookup();
}

ClimbingSet objectsHeldFromCreation(ValueFlowRuleContext& context, ClimbingSet objects)
{
    ValueFlowStatement statement;
    return HeapRule(statement, context).objectsHeldFromCreation(objects);
}

ClimbingSet objectsRightAbove(ValueFlowRuleContext& context, ClimbingSet knownObjects)
{
    ValueFlowStatement statement;
    return HeapRule(statement, context).prototypesOf(knownObjects);
}

ValueFlowNodeID exportsOfHoldersListedBesideUnknownObject(ValueFlowRuleContext& context, const ValueFlowOperand& object)
{
    ValueFlowStatement statement;
    return HeapRule(statement, context).exportsOfHoldersListedBesideUnknownObject(object);
}

ValueFlowNodeID exportsOfHoldersIn(ValueFlowRuleContext& context, const ValueFlowOperand& object)
{
    ValueFlowStatement statement;
    return HeapRule(statement, context).exportsOfHoldersIn(object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
