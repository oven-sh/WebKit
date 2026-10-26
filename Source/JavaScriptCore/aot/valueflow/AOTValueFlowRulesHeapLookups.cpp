/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesHeapInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionKinds.h"
#include "AOTThreshold.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"
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

ValueFlowName nameOfLookupForValue(const HeapKey& key)
{
    return key.namesEnumerableProperty ? ValueFlowName { ValueFlowNameKind::EveryProperty, 1 } : foundName(key, HeapPropertyPart::Value);
}

HeapKey keyOfFoundName(ValueFlowName name)
{
    if (name == nameOfLookupForValue(enumeratedHeapKey))
        return enumeratedHeapKey;
    if (name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter || name.kind == ValueFlowNameKind::MemberWritten)
        return { HeapKeyClass::Name, PropertyKeyID(name.payload) };
    if (name.kind == ValueFlowNameKind::Elements || name.kind == ValueFlowNameKind::GetterUnderUnknownKey || name.kind == ValueFlowNameKind::SetterUnderUnknownKey)
        return indexHeapKey;
    return unknownHeapKey;
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

static bool hasOrdinaryPrototypeObject(const Program& program, const Function& function)
{
    return !function.constructedClass && !function.has(FunctionFlags::IsGenerator) && functionHasPrototypeProperty(program, function);
}

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

HeapChain HeapRule::chainFrom(ClimbingSet receivers, ValueFlowName nameOfChain)
{
    ClimbingSet start = startOfLookup(receivers);
    ValueFlowNodeID wholeChain = m_context.lookup(start, nameOfChain);
    ClimbingSet chain = m_context.valueOf(wholeChain);
    ClimbingSet objects = known(chain);
    if (mayBeUnknownObject(chain) && m_context.holdsAnyOf(ClimbingSet(only(ValueFlowUniverse::markOfNamedBun())), wholeChain))
        objects = m_context.unite(objects, m_context.valueWithin(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), wholeChain));
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

ValueSetID HeapRule::holdersOfUndeclaredPropertiesUnder(const HeapKey& key)
{
    if (key.keyClass == HeapKeyClass::Index)
        return m_tables.holdersOfUndeclaredPropertiesUnderIndexes();
    if (key.keyClass != HeapKeyClass::Name)
        return m_tables.holdersOfUndeclaredPropertiesUnderAnyKey();
    if (m_nameOfHoldersOfUndeclaredProperties == key.name && m_holdersOfUndeclaredPropertiesUnderName)
        return m_holdersOfUndeclaredPropertiesUnderName;
    m_nameOfHoldersOfUndeclaredProperties = key.name;
    ValueSetID& holders = m_holdersOfUndeclaredPropertiesUnderName;
    PropertyKeyKind kind = m_program.at(key.name).kind;
    if ((kind == PropertyKeyKind::CreatedSymbol || kind == PropertyKeyKind::RegisteredSymbol) && m_tables.under(key.name).holders == ValueSets::nothing && !doesUnseenCodeRun())
        return holders = ValueSets::nothing;
    if (kind == PropertyKeyKind::ModulePrivateName)
        return holders = ValueSets::nothing;
    holders = m_tables.under(key.name).holdersOfUndeclaredProperties;
    if (key.name == m_program.keyOf(BuiltinNames::then) && !doesUnseenCodeRun())
        holders = m_context.maker().intersect(holders, m_context.maker().unite(ValueSets::complement(m_universe.builtinObjects()), m_tables.prototypeObjects()));
    bool isPrivateName = kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::EnginePrivateName || kind == PropertyKeyKind::ModulePrivateName;
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

bool HeapRule::mayTakeMemberFromCopy(ClimbingSet holders, const HeapKey& key)
{
    ClimbingSet writable = m_context.restrict(known(holders), ValueSets::complement(holdersWithLockedLocationUnder(key)));
    return holdsAnything(writable) && holdsAnything(readWhereWritten(writable, copiedFromName));
}

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
    bool mayGoOn = holdsAnything(readWhereWritten(holders, deleted)) || mayTakeMemberFromCopy(holders, key);
    if (isStoredThroughPlaceholdersInto(holders, deleted) || mayGoOn)
        return std::nullopt;

    AOT_THRESHOLD(unsigned, longestChain, 8);
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

HeapChain HeapRule::chainWithCopiesThatReach(const HeapKey& key, ClimbingSet receivers)
{
    if (!holdsAnything(readWhereWritten(ClimbingSet(m_tables.objectsOfRealm()), copiedFromName)))
        return chainFrom(receivers);
    HeapChain chain = chainFrom(receivers, chainWithoutCopiesOntoRealmName);
    return mayTakeMemberFromCopy(m_context.restrict(chain.objects, m_tables.objectsOfRealm()), key) ? chainFrom(receivers) : chain;
}

HeapChain HeapRule::chainThatGoesUpFrom(ClimbingSet receivers, const HeapKey& key)
{
    HeapChain chain = chainWithCopiesThatReach(key, receivers);
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
    return isStoredThroughPlaceholdersInto(holders, setters) || isFound;
}

HeapChain HeapRule::chainFrom(ClimbingSet receivers, const HeapKey& key)
{
    auto withoutLockedLocations = [&](HeapChain chain) {
        chain.writableObjects = m_context.restrict(chain.objects, ValueSets::complement(holdersWithLockedLocationUnder(key)));
        return chain;
    };

    if (key.keyClass != HeapKeyClass::Name) {
        HeapChain chain = chainWithCopiesThatReach(key, receivers);
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
        if (isStoredThroughPlaceholdersInto(holdersThatMayLoseMember, deleted) || mayBeDeleted)
            answering = ValueSets::nothing;
    }
    if (key.name == m_tables.prototypeKey())
        answering = m_context.maker().unite(answering, m_tables.closuresWithPrototypeProperty());

    ClimbingSet holders = m_context.restrict(receivers, answering);
    if (!holdsAnything(holders) || mayTakeMemberFromCopy(holders, key))
        return withoutLockedLocations(chainThatGoesUpFrom(receivers, key));
    HeapChain chain = chainThatGoesUpFrom(m_context.restrict(receivers, ValueSets::complement(answering)), key);
    chain.holdersThatAnswer = holders;
    chain.objects = m_context.unite(chain.objects, holders);
    return withoutLockedLocations(chain);
}

ValueFlowNodeID HeapRule::storedThroughUnknownObjectInto(ClimbingSet objects, ValueFlowName name)
{
    ValueFlowNodeID stored = m_context.read(unknownObjectAlone(), name);
    if (!m_context.meet(objects, m_universe.builtinObjects()) && !hasEscaped(objects))
        return { };
    if (!holdsAnything(stored))
        return { };
    bool namesProperty = name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter || name.kind == ValueFlowNameKind::Setter;
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (namesProperty && m_context.contains(objects, globalObject) && isNameThatTableDoesNotListOnGlobalObject({ HeapKeyClass::Name, PropertyKeyID(name.payload) }) && hasEscaped(ClimbingSet(only(globalObject))))
        return stored;
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
            if (!site.namesPrototype)
                prototypes.append(m_universe.prototypeOf(site.layout));
            return;
        case LayoutKind::Environment:
        case LayoutKind::Closure:
        case LayoutKind::CoroutineFrame:
        case LayoutKind::ModuleNamespace:
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

bool HeapRule::isNameThatTableDoesNotListOnGlobalObject(const HeapKey& key) const
{
    if (key.keyClass != HeapKeyClass::Name || m_program.at(key.name).kind != PropertyKeyKind::Text || key.name == m_program.keyOf(m_builtins.at(Builtins::GlobalThis).name))
        return false;
    return !m_context.sets().contains(m_tables.under(key.name).holders, m_universe.objectOf(Builtins::GlobalThis));
}

bool HeapRule::namesObjectOfAccess() const
{
    if (m_statement.kind != ValueFlowStatementKind::Operation)
        return false;
    bool mayBeFreeName = m_statement.opcode == Opcode::PropGet || m_statement.opcode == Opcode::PropSet || m_statement.opcode == Opcode::PropDelete;
    return !mayBeFreeName || !m_program.at(m_statement.function).at(m_statement.site).isFreeName();
}

bool HeapRule::mayHoldUndeclaredProperty(ClimbingSet objects, const HeapKey& key)
{
    ValueSetID holders = holdersOfUndeclaredPropertiesUnder(key);
    ValueFlowObjectID globalObject = m_universe.objectOf(Builtins::GlobalThis);
    if (namesObjectOfAccess() && m_context.contains(objects, globalObject) && isNameThatTableDoesNotListOnGlobalObject(key) && holdsGlobalObjectOnlyWhatStatementsHaveStored())
        holders = m_context.maker().intersect(holders, ValueSets::complement(only(globalObject)));
    return m_context.meet(objects, holders);
}

ClimbingSet HeapRule::prototypeObjectsAboveEscapedEntries(std::span<const ValueFlowClassOfEntries> classes, ClimbingSet holders)
{
    Members prototypes;
    for (const ValueFlowClassOfEntries& classOfEntries : classes) {
        if (m_context.contains(holders, classOfEntries.prototype) && hasEscaped(ClimbingSet(classOfEntries.entries)))
            prototypes.append(classOfEntries.prototype);
    }
    return ClimbingSet(m_context.maker().of(prototypes.span()));
}

ClimbingSet HeapRule::holdersFoundThroughUnknownObject(const HeapKey& key)
{
    ValueSetID holders = holdersWithMemberUnder(key);
    if (key.keyClass == HeapKeyClass::Index || (key.keyClass == HeapKeyClass::Name && holders == ValueSets::nothing))
        return ClimbingSet(ValueSets::nothing);
    ClimbingSet found = m_context.unite(escapedAmong(ClimbingSet(holders)), ClimbingSet(key.keyClass == HeapKeyClass::Name ? m_tables.under(key.name).prototypeObjects : m_tables.prototypeObjects()));
    return m_context.unite(found, prototypeObjectsAboveEscapedEntries(m_tables.classesOfEntries(), ClimbingSet(holders)));
}

ClimbingSet HeapRule::holdersOfAccessorsThatRunOnUnknownObject(const HeapKey& key)
{
    ClimbingSet holders = holdersFoundThroughUnknownObject(key);
    ClimbingSet ofHost = m_context.restrict(holders, m_tables.prototypeObjectsOfHost());
    if (!holdsAnything(ofHost) || doesUnseenCodeRun())
        return holders;

    ClimbingSet withInstances = m_context.unite(m_context.valueWithin(ofHost, m_context.nodes().ofHost(ValueFlowHostNode::PrototypeObjectsOfInstances)), escapedAmong(ofHost));
    withInstances = m_context.unite(withInstances, prototypeObjectsAboveEscapedEntries(m_tables.classesOfHostWithEntries(), ofHost));
    return m_context.unite(m_context.restrict(holders, ValueSets::complement(m_tables.prototypeObjectsOfHost())), withInstances);
}

ClimbingSet HeapRule::foundThroughUntrackedObject(ValueSetID objects)
{
    ValueFlowNodeID chain = m_context.lookup(untrackedObjectAlone(), prototypeLinkName);
    if (objects == ValueSets::nothing)
        return { };
    return m_context.valueWithin(ClimbingSet(objects), chain);
}

bool HeapRule::isHandedOutThroughPlaceholder(BuiltinID member, const HeapKey& key)
{
    if (m_tables.letsInUnseenCode(member) || isFoundByNameAlone(m_builtins.canonicalID(member)))
        return false;
    if (BuiltinID holder = m_builtins.at(member).holder; holder && isNativeModuleThatNobodyNames(m_universe.objectOf(holder)))
        return false;
    const Builtin& entry = m_builtins.at(member);
    if (isMemberOfBun(member))
        return false;
    bool isFoundUnderOwnName = entry.kind != BuiltinKind::Constructor || (entry.holder && m_builtins.at(entry.holder).kind == BuiltinKind::Namespace);
    return (key.keyClass == HeapKeyClass::Name && isFoundUnderOwnName) || entry.origin != BuiltinOrigin::Host;
}

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

ClimbingSet HeapRule::prototypesOf(ClimbingSet start, ValueSetID atCreation)
{
    ClimbingSet objects = m_context.restrict(known(start), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink()));
    ValueFlowNodeID links = readWhereWritten(objects, prototypeLinkName);
    ClimbingSet above = m_context.unite(valueOfNodeIfAny(links), ClimbingSet(atCreation ? atCreation : prototypesAtCreationOf(start)));
    if (links && mayBeUnknownObject(above))
        above = m_context.unite(above, m_context.valueWithin(ClimbingSet(m_context.maker().unite(m_universe.holdersOfLibraryExports(), m_context.maker().unite(only(m_universe.objectOf(Builtins::Bun)), only(ValueFlowUniverse::markOfNamedBun())))), links));
    forEachNodeStoredThroughPlaceholdersInto(objects, prototypeLinkName, [&](ValueFlowNodeID stored) {
        above = m_context.unite(above, m_context.valueOf(stored));
    });
    return above;
}

void HeapRule::collectChain()
{
    ClimbingSet start = m_context.setOfKey();
    ValueFlowName nameOfChain = m_context.nameOfKey();
    add(m_statement.result, start);
    if (mayBeUnknownObject(start))
        letPrototypeObjectsOfLanguageEscape();
    if (mayBeUntrackedObject(start)) {
        if (!m_context.meet(start, ValueSets::complement(m_universe.untrackedObjectAlone()))) {
            m_context.forEachMember(known(m_context.newSince(ValueFlowProgramNodes::lost())), [&](ValueFlowObjectID object) {
                connect(m_context.lookup(ClimbingSet(only(object)), nameOfChain), m_statement.result);
            });
            return;
        }
        connect(m_context.lookup(untrackedObjectAlone(), nameOfChain), m_statement.result);
    }

    ValueSetID& atCreation = m_context.rememberedByStatement();
    if (!atCreation)
        atCreation = prototypesAtCreationOf(start);
    ClimbingSet above = prototypesOf(start, atCreation);
    ValueFlowNodeID chainAbove = m_context.lookup(known(above), nameOfChain);
    connect(chainAbove, m_statement.result);
    add(m_statement.result, m_context.restrict(above, m_context.maker().unite(m_universe.placeholdersAlone(), only(ValueFlowUniverse::markOfNamedBun()))));
    if (mayBeUnknownObject(above)) {
        ValueSetID objectsOfProgram = m_context.maker().intersect(m_universe.knownObjects(), ValueSets::complement(m_universe.builtinObjects()));
        if (ValueFlowNodeID links = readWhereWritten(m_context.restrict(known(start), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink())), prototypeLinkName))
            connect(m_context.masked(m_context.lookup(m_context.valueWithinAskedLast(ClimbingSet(objectsOfProgram), links), nameOfChain), m_universe.holdersOfLibraryExports()), m_statement.result);
    }
    ClimbingSet targetsOfCopies = nameOfChain == chainWithoutCopiesOntoRealmName ? m_context.restrict(known(start), ValueSets::complement(m_tables.objectsOfRealm())) : known(start);
    ValueFlowNodeID sourcesOfCopies = readWhereWritten(targetsOfCopies, copiedFromName);
    above = m_context.unite(above, withSourcesOfCopies(m_context, known(valueOfNodeIfAny(sourcesOfCopies))));
    add(m_statement.result, known(above));
    if (sourcesOfCopies && m_context.holdsAnyOf(ClimbingSet(only(ValueFlowUniverse::markOfNamedBun())), sourcesOfCopies))
        add(m_statement.result, only(ValueFlowUniverse::markOfNamedBun()));

    if (m_context.numberOfMembersOfKey() != 1 || !hasBeenLost(known(start)))
        return;
    ClimbingSet chain = m_context.unite(above, m_context.valueOf(chainAbove));
    if (mayHoldAccessorNobodyKnows(chain, unknownHeapKey, HeapPropertyPart::Getter) || mayHoldAccessorNobodyKnows(chain, unknownHeapKey, HeapPropertyPart::Setter))
        add(ValueFlowProgramNodes::escaped(), start);
}

void HeapRule::readMember(BuiltinID member, ModuleID module, ValueFlowNodeID result, Members& getters)
{
    if (m_tables.letsInUnseenCode(member) && !isOperationOfFunction())
        return;
    use(member, BuiltinUse::Read);
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
        if (m_builtins.canonicalID(member) == Builtins::Bun)
            add(result, only(ValueFlowUniverse::markOfNamedBun()));
        return;
    case BuiltinKind::DataProperty: {
        const Builtin& entry = m_builtins.at(member);
        ModuleID compiled = entry.holder && entry.name && !entry.flags.contains(BuiltinFlag::IsDefaultExport) ? m_tables.compiledModuleOf(m_builtins.canonicalID(entry.holder)) : ModuleID();
        if (compiled) {
            connect(m_context.lookup(valueOf(m_context, requiredValueOf(m_context, compiled)), ValueFlowName::property(m_program.keyOf(entry.name))), result);
            return;
        }
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

bool HeapRule::mayHoldAccessorNobodyKnows(ClimbingSet chain, const HeapKey& key, HeapPropertyPart part)
{
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

ClimbingSet HeapRule::receiversBelowAccessorNobodyKnows(ClimbingSet start, const HeapKey& key, HeapPropertyPart part)
{
    auto mayLieAbove = [&](ClimbingSet receivers) {
        ClimbingSet above = objectsAbove(receivers);
        ValueFlowNodeID chainAbove = m_context.lookup(known(above), prototypeLinkName);
        return mayHoldAccessorNobodyKnows(chainUpToPrototypeObjectsOfEntries(above, chainAbove, key).value_or(m_context.unite(above, m_context.valueOf(chainAbove))), key, part);
    };
    start = m_context.membersMissingFrom(ValueFlowProgramNodes::escaped(), start);
    if (!holdsAnything(start) || !mayLieAbove(start))
        return { };

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
    if (!m_context.meet(functions, m_tables.accessors()))
        return;

    if (m_context.contains(functions, m_tables.undescribedNativeCode())) {
        passToAccessorNobodyKnows(receiver, m_tables.whatIsNewToNativeCode());
        if (value)
            connect(*value, ValueFlowProgramNodes::escaped());
        add(result, m_undescribedNativeCodeIsOfEngine ? undescribedValueThatIsNoBuffer(m_context) : unknownValue(ValueFlowUnknownSource::UndescribedNativeResult));
    }

    if (mayBeUnknownObject(functions)) {
        passToAccessorNobodyKnows(receiver, ValueSets::anything);
        if (value)
            connect(*value, ValueFlowProgramNodes::escaped());
        add(result, unknownValue(ValueFlowUnknownSource::ResultOfUnknownCallee));
    }

    ValueFlowObjectID accessorOfPrototypeLink = m_tables.accessorOfPrototypeLink();
    if (kind == ValueFlowCallKind::Set && value && !receiver.operand && accessorOfPrototypeLink && m_context.contains(functions, accessorOfPrototypeLink)
        && !m_context.meet(receiver.set, m_context.maker().unite(ValueSets::complement(m_universe.knownObjects()), m_universe.builtinObjects()))) {
        changePrototype(receiver.set, *value);
        use(m_universe.builtinOf(accessorOfPrototypeLink), BuiltinUse::Written);
        functions = m_context.restrict(functions, ValueSets::complement(only(accessorOfPrototypeLink)));
    }

    // An accessor that a free name finds is entered with undefined.
    bool mayBeFoundByFreeName = m_context.contains(receiver.operand ? m_context.valueOf(*receiver.operand) : receiver.set, m_universe.objectOf(Builtins::GlobalThis));
    forEachPartOfCallees(m_context, m_context.restrict(functions, m_tables.knownAccessors()), [&](ClimbingSet callees) {
        ValueFlowCallGroup group = m_context.group(callees, kind);
        if (receiver.operand)
            connect(*receiver.operand, group.thisValue);
        else
            add(group.thisValue, receiver.set);
        if (mayBeFoundByFreeName)
            add(group.thisValue, m_universe.undefinedValue());
        if (value)
            connect(*value, group.argument(0));
        connect(group.result, result);
        connect(group.thrown, ValueFlowProgramNodes::escaped());
    });
}

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

void HeapRule::readPropertiesAtCreation(ClimbingSet objects, const HeapKey& key, ValueFlowNodeID result)
{
    if (key.mayNameNonEnumerable(m_tables.prototypeKey())) {
        m_context.forEachMember(m_context.restrict(objects, m_universe.closures()), [&](ValueFlowObjectID closure) {
            const Function& function = m_program.at(m_universe.closureFunction(closure));
            if (function.constructedClass)
                add(result, only(m_universe.prototypeOf(function.constructedClass)));
            else if (function.has(FunctionFlags::IsGenerator))
                add(result, only(m_universe.coroutineResultOf(m_universe.closureFunction(closure))));
            else if (functionHasPrototypeProperty(m_program, function))
                add(result, only(m_universe.prototypeObjectOf(m_universe.closureFunction(closure))));
        });
    }
    if (key.mayNameNonEnumerable(m_tables.constructorKey())) {
        m_context.forEachMember(objects, [&](ValueFlowObjectID object) {
            LayoutID layout = m_universe.classLayoutOf(object);
            if (FunctionID constructor = layout ? m_program.at(layout).constructor : FunctionID())
                add(result, only(m_universe.closureOf(constructor)));
            else if (m_universe.kindOf(object) == ValueFlowObjectKind::FunctionPrototype)
                add(result, only(m_universe.closureOf(m_universe.functionOf(object))));
        });
    }
    if (!m_context.meet(objects, m_tables.templateObjects()))
        return;
    if (key.mayNameNonEnumerable(m_tables.rawKey()))
        add(result, unknownObject(ValueFlowUnknownSource::ObjectWithoutLayout));
    if (key.keyClass != HeapKeyClass::Name)
        add(result, m_context.maker().unite(m_universe.strings(), m_universe.undefinedValue()));
}

bool HeapRule::isMemberOfBunUsedByNameAlone(BuiltinID member) const
{
    if (!isMemberOfBun(member))
        return false;
    if (isObjectKind(m_builtins.at(member).kind) || !isMadeWithCodeOfLibrary(m_builtins, member))
        return true;
    return std::ranges::any_of(m_builtins.overloadsOf(m_builtins.canonicalID(member)), [&](const BuiltinOverload& overload) {
        const BuiltinType& answer = m_builtins.at(overload.result.type);
        return overload.mode == BuiltinCallMode::Get && (answer.isAnything() || answer.flags.contains(BuiltinTypeFlag::HasAnyFunction) || !answer.parts.empty());
    });
}

bool HeapRule::hasMemberOfHost(ValueFlowObjectID holder) const
{
    return std::ranges::any_of(m_builtins.membersOf(m_universe.builtinOf(holder)), [&](BuiltinID member) {
        return m_builtins.at(m_builtins.canonicalID(member)).origin == BuiltinOrigin::Host;
    });
}

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
        for (HeapPropertyPart part : { HeapPropertyPart::Value, HeapPropertyPart::Getter, HeapPropertyPart::Setter })
            forEachNodeHolding(objectsOfProgram, foundName(key, part), note);
        return isWritten;
    };

    bool isRedefined = holdsAnything(m_context.markedAmong(objectsOfProgram, ObjectMutation::RedefineProperty));
    for (PropertyKeyID name : { m_tables.prototypeKey(), m_tables.constructorKey(), m_tables.rawKey() }) {
        if (isRedefined || hasWritten(name))
            readPropertiesAtCreation(chain.objects, keyNamed(name), result);
    }
    m_context.forEachMember(m_context.restrict(chain.objects, m_tables.holdersWithMembers()), [&](ValueFlowObjectID holder) {
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
            ValueFlowObjectID function = objectOfMember(member, module);
            if (m_builtins.at(member).origin == BuiltinOrigin::Host && m_context.sets().contains(m_universe.builtinFunctions(), function))
                add(m_context.nodes().ofHost(ValueFlowHostNode::FunctionsOfHostTakenFromKnownHolders), only(function));
        });
        return;
    }

    ClimbingSet holders = m_context.restrict(objects, m_tables.holdersWithMembers());
    if (!holdsAnything(holders))
        return;
    m_context.lookup(holders, ValueFlowName::of(ValueFlowNameKind::EveryMemberRead));
    m_context.forEachMember(holders, [&](ValueFlowObjectID holder) {
        if (key.namesEnumerableProperty && !hasMemberOfHost(holder))
            return;
        if (ModuleID module = m_universe.moduleOf(holder)) {
            bool isReceiver = m_context.contains(start, holder);
            for (BuiltinID member : m_builtins.membersOf(m_universe.builtinOf(holder))) {
                if (isReceiver || !isFoundByNameAlone(m_builtins.canonicalID(member)))
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
}

void HeapRule::readEvaluators(const ValueFlowOperand& object, const HeapKey& key)
{
    ValueSetID holders = ValueSets::nothing;
    switch (key.keyClass) {
    case HeapKeyClass::Name:
        holders = m_tables.under(key.name).holdersOfEvaluators;
        break;
    case HeapKeyClass::Index:
    case HeapKeyClass::Unknown:
        break;
    }
    if (holders == ValueSets::nothing || !isOperationOfFunction())
        return;

    Members getters;
    HeapChain chain = chainFrom(m_context.valueOf(object), key);
    ClimbingSet found = m_context.restrict(chain.objects, holders);
    if (holdsNamedBun(object))
        found = m_context.unite(found, m_context.restrict(ClimbingSet(only(m_universe.objectOf(Builtins::Bun))), holders));
    if (chain.holdsUnknownObject) {
        AOT_THRESHOLD(unsigned, longestChain, 8);
        ClimbingSet above = known(chain.start);
        for (unsigned step = 0; step < longestChain && holdsAnything(above); ++step) {
            found = m_context.unite(found, m_context.restrict(above, holders));
            if (ValueFlowNodeID sourcesOfCopies = readWhereWritten(above, copiedFromName))
                found = m_context.unite(found, m_context.restrict(withSourcesOfCopies(m_context, known(m_context.valueOf(sourcesOfCopies))), holders));
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

bool HeapRule::surelyComesToMember(ClimbingSet start, PropertyKeyID name)
{
    AOT_THRESHOLD(unsigned, longestChain, 8);
    const ValueFlowMembersUnderKey& members = m_tables.under(name);
    if (m_context.maker().intersect(members.holders, members.holdersWithLockedLocation) == ValueSets::nothing)
        return false;

    ClimbingSet current = start;
    for (unsigned step = 0; step < longestChain; ++step) {
        ClimbingSet above;
        ClimbingSet objectsThatMayGetAnotherLink = m_context.restrict(known(current), m_context.maker().intersect(m_universe.objects(), ValueSets::complement(m_tables.holdersWithLockedPrototypeLink())));
        if (holdsAnything(objectsThatMayGetAnotherLink)) {
            above = valueOfNodeIfAny(readWhereWritten(objectsThatMayGetAnotherLink, prototypeLinkName));
            if (isStoredThroughPlaceholdersInto(objectsThatMayGetAnotherLink, prototypeLinkName) || m_context.meet(above, m_universe.placeholdersAlone()) || m_context.contains(above, ValueFlowUniverse::primitive(PrimitiveSet::Null)))
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
    const HeapKey& key = keyOfRead.namesEnumerableProperty && (chain.holdsUnknownObject || chain.holdsUntrackedObject) ? unknownHeapKey : keyOfRead;
    ValueFlowName value = foundName(key, HeapPropertyPart::Value);
    ValueFlowName getter = foundName(key, HeapPropertyPart::Getter);
    ClimbingSet getters;
    bool handsOutEvaluators = key.keyClass != HeapKeyClass::Unknown;
    auto answer = [&](ValueFlowNodeID node) {
        connect(handsOutEvaluators ? node : m_context.masked(node, ValueSets::complement(m_tables.objectsThatLetInUnseenCode())), result);
    };
    auto takeGetters = [&](ValueFlowNodeID node) {
        getters = m_context.unite(getters, valueOfNodeIfAny(node));
    };

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
            ClimbingSet redefinedObjects = m_context.markedAmong(otherWritableObjects, ObjectMutation::RedefineProperty);
            mayFindNothing = (holdsAnything(writableHolders) && holdsAnything(readWhereWritten(writableHolders, setter)))
                || (holdsAnything(otherWritableObjects) && holdsAnything(readWhereWritten(otherWritableObjects, setter)))
                || (holdsAnything(redefinedObjects) && holdsAnything(readWhereWritten(redefinedObjects, getter)));
            mayFindNothing |= isStoredThroughPlaceholdersInto(chain.writableObjects, setter);
        }
    }
    if (mayFindNothing)
        add(result, m_universe.undefinedValue());
    forEachNodeHolding(chain.writableObjects, value, answer);
    takeGetters(readWhereWritten(chain.writableObjects, getter));
    forEachNodeStoredThroughPlaceholdersInto(chain.writableObjects, getter, takeGetters);

    readPropertiesAtCreation(chain.objects, key, result);
    readMembers(chain.objects, chain.start, key, result, builtinGetters);
    if (key.namesEnumerableProperty && holdsAnything(m_context.markedAmong(chain.objects, { ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype })))
        readWhatNamesOfProgramMayHaveHidden(chain, result, builtinGetters);
    if (mayHoldUndeclaredProperty(chain.objects, key))
        builtinGetters.append(m_tables.undescribedNativeCode());
    if (key.keyClass == HeapKeyClass::Unknown || (key.keyClass == HeapKeyClass::Name && key.name == m_tables.prototypeKey())) {
        Members classes;
        m_context.forEachMember(m_context.restrict(chain.objects, holdersOfUndeclaredPropertiesUnder(key)), [&](ValueFlowObjectID holder) {
            BuiltinID builtin = m_universe.builtinOf(holder);
            if (builtin && m_builtins.at(builtin).kind == BuiltinKind::Constructor)
                classes.append(holder);
        });
        add(ValueFlowProgramNodes::escaped(), m_context.maker().of(classes.span()));
    }

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

    if (chain.holdsUntrackedObject) {
        forEachNodeReadByName(value, answer);
        forEachNodeReadByName(getter, takeGetters);
        if (key.keyClass != HeapKeyClass::Name || key.name == m_tables.prototypeKey() || key.name == m_tables.constructorKey() || key.name == m_tables.rawKey())
            readPropertiesAtCreation(foundThroughUntrackedObject(m_universe.knownObjects()), key, result);
        forEachMemberUnder(foundThroughUntrackedObject(holdersWithMemberUnder(key)), key, [&](BuiltinID member, ModuleID module) {
            if (isHandedOutThroughPlaceholder(member, key))
                readMember(member, module, result, builtinGetters);
        });
        if (holdsAnything(foundThroughUntrackedObject(holdersOfUndeclaredPropertiesUnder(key))))
            builtinGetters.append(m_tables.undescribedNativeCode());
        add(result, m_universe.anyPrimitive());
    }

    if (mayBeUnknownObject(start)) {
        add(result, unknownValue(ValueFlowUnknownSource::ContentsOfEscapedObject));
        ClimbingSet holdersOfExports = known(m_context.valueOf(m_context.nodes().ofHost(ValueFlowHostNode::HoldersOfExportsInEscapedObjects)));
        if (key.keyClass == HeapKeyClass::Name && holdsAnything(holdersOfExports)) {
            answer(m_context.read(holdersOfExports, value));
            takeGetters(m_context.read(holdersOfExports, getter));
        }
        if (key.keyClass == HeapKeyClass::Unknown || (key.keyClass == HeapKeyClass::Name && key.name == m_tables.constructorKey())) {
            ValueFlowNodeID constructors = m_context.nodes().ofHost(ValueFlowHostNode::ConstructorsAboveEscapedInstances);
            connect(m_context.masked(constructors, ValueSets::complement(m_universe.builtinObjects())), result);
            connect(m_context.masked(constructors, m_universe.builtinObjects()), ValueFlowProgramNodes::escaped());
        }
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
            if (!isObjectKind(m_builtins.at(member).kind)) {
                use(member, mayBeCalled ? OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped });
                return;
            }
            m_context.useAsAnswerOfUnknownObject(member, mayBeCalled ? OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped, BuiltinUse::Called } : OptionSet<BuiltinUse> { BuiltinUse::Read, BuiltinUse::Escaped });
            handedOutAsUnknownObject.append(objectOfMember(member, module));
        });
        add(ValueFlowProgramNodes::escaped(), m_context.maker().of(handedOutAsUnknownObject.span()));
    } else if (chain.holdsUnknownObject) {
        connect(m_context.lookup(unknownObjectAlone(), value), result);
        if (handsOutEvaluators) {
            // The unknown object stands for no object that lets in unseen code: one that is written on an object listed beside it is read there.
            ClimbingSet listed = m_context.valueWithin(ClimbingSet(m_universe.knownObjects()), m_context.lookup(chain.start, prototypeLinkName));
            ClimbingSet writable = m_context.restrict(listed, ValueSets::complement(holdersWithLockedLocationUnder(key)));
            if (ValueFlowNodeID written = holdsAnything(writable) ? readWhereWritten(writable, value) : ValueFlowNodeID())
                connect(m_context.masked(written, m_tables.objectsThatLetInUnseenCode()), result);
        }
    }
    if (chain.holdsUnknownObject)
        builtinGetters.append(ValueFlowUniverse::unknownObject());
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
        everyEntryComesToMember &= !isStoredThroughPlaceholdersInto(alone, prototypeLinkName);
    });
    if (!everyEntryComesToMember)
        return false;

    ClimbingSet objects(m_context.maker().of(holders.span()));
    ValueFlowName deleted = ValueFlowName::of(ValueFlowNameKind::DeletedMemberUnderUnknownKey);
    bool mayBeDeleted = holdsAnything(m_context.read(objects, deleted));
    return !isStoredThroughPlaceholdersInto(objects, deleted) && !mayBeDeleted;
}

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
        bool everyEntryAnswers = true;
        m_context.forEachMember(m_context.restrict(entries, m_tables.under(name).holders), [&](ValueFlowObjectID holder) {
            everyEntryAnswers &= answersWithMember(holder, name);
        });
        chain = everyEntryAnswers ? chainFrom(entries, key) : chainThatGoesUpFrom(entries, key);
        ClimbingSet objects = objectsOfProgramOn(chain);
        if (!read.isOnlyInspected) {
            forEachNodeHolding(objects, value, handOver);
            readPropertiesAtCreation(objects, key, escaped);
        }
        forEachNodeHolding(objects, getter, handOver);
    } else {
        if (endsAtMemberOfTable(entries, read.name))
            return;
        value = writtenName(unknownHeapKey, HeapPropertyPart::Value);
        getter = writtenName(unknownHeapKey, HeapPropertyPart::Getter);
        chain = chainFrom(entries);
        ClimbingSet objects = objectsOfProgramOn(chain);
        if (!read.isOnlyInspected)
            forEachNodeHolding(objects, value, handOver);
        forEachNodeHolding(objects, getter, handOver);
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

void HeapRule::noteUseOfMembers(const HeapKey& key, BuiltinUse noted)
{
    ClimbingSet start = m_context.setOfKey();
    ClimbingSet foundThroughPlaceholders;
    if (mayBeUnknownObject(start))
        foundThroughPlaceholders = escapedAmong(ClimbingSet(holdersWithMemberUnder(key)));
    if (mayBeUntrackedObject(start))
        foundThroughPlaceholders = m_context.unite(foundThroughPlaceholders, lostAmong(ClimbingSet(holdersWithMemberUnder(key))));
    bool namesMember = key.keyClass == HeapKeyClass::Name;
    forEachMemberUnder(start, key, [&](BuiltinID member, ModuleID module) {
        if (noted == BuiltinUse::Read && !namesMember && m_builtins.at(member).holder && isNativeModuleThatNobodyNames(m_universe.objectOf(m_builtins.at(member).holder)))
            return;
        if (!namesMember && !module && isFoundByNameAlone(m_builtins.canonicalID(member)))
            return;
        if (noted == BuiltinUse::Read && !namesMember && isMemberOfBunUsedByNameAlone(member))
            return;
        if (noted != BuiltinUse::Read || !m_tables.letsInUnseenCode(member))
            use(member, noted);
    });
    if (!holdsAnything(foundThroughPlaceholders))
        return;
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
    m_keepsHoldersOfExportsItStartsFrom = name == everyPropertyOfListedHoldersOfExports;
    switch (name.kind) {
    case ValueFlowNameKind::PrototypeLink:
        collectChain();
        return;
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
        noteUseOfMembers(keyOfFoundName(name), BuiltinUse::Written);
        return;
    case ValueFlowNameKind::EveryMemberRead:
        noteUseOfMembers(unknownHeapKey, BuiltinUse::Read);
        return;
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::EveryProperty:
        if (!splitLookupThroughPlaceholders())
            readThroughChain(m_context.setOfKey(), keyOfFoundName(name), { nullptr, m_context.setOfKey() }, m_statement.result);
        return;
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EverySetter:
        if (!splitLookupThroughPlaceholders())
            collectAccessors(BuiltinCallMode::Set);
        return;
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::EveryGetter:
        if (!splitLookupThroughPlaceholders())
            collectAccessors(BuiltinCallMode::Get);
        return;
    case ValueFlowNameKind::UnknownKey:
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
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        RELEASE_ASSERT(splitLookupThroughPlaceholders());
        return;
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
