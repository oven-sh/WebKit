/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowHeapTables.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTKnownClosures.h"
#include "AOTLayout.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTValueIdentity.h"
#include "AOTValueSets.h"
#include <array>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

bool isMadeWithCodeOfLibrary(const BuiltinTable& builtins, BuiltinID member)
{
    member = builtins.canonicalID(member);
    const Builtin& entry = builtins.at(member);
    if (entry.origin != BuiltinOrigin::Host)
        return false;
    if (!builtins.calledFunctionsOf(member).empty() || !builtins.moduleLoadsOf(member).empty())
        return true;
    return !isObjectKind(entry.kind) && std::ranges::any_of(builtins.overloadsOf(member), [](const BuiltinOverload& overload) {
        return !!overload.code.implementation.function;
    });
}

namespace {

using Objects = Vector<ValueFlowObjectID, 32>;

struct ObjectCreatedWithKey {
    PropertyKeyID key;
    ValueFlowObjectID object;
};

struct ObjectsCreatedWithFields {
    Vector<ObjectCreatedWithKey> underKeys;
    Vector<ValueFlowObjectID> objects;
    Vector<ObjectCreatedWithKey> storedToUnderKeys;
};

} // anonymous namespace

static uint32_t layOutByKey(Vector<uint32_t>& numberOfEntriesOfKey)
{
    uint32_t numberOfEntries = 0;
    for (uint32_t& entries : numberOfEntriesOfKey)
        entries = std::exchange(numberOfEntries, numberOfEntries + entries);
    return numberOfEntries;
}

template<typename T>
static std::span<const T> entriesOfKey(const Vector<T>& entries, const Vector<uint32_t>& endOfKey, size_t key)
{
    uint32_t first = key ? endOfKey[key - 1] : 0;
    return entries.subspan(first, endOfKey[key] - first);
}

static ValueSetID kindsOf(ValueSetMaker& maker, uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return maker.of(members.span());
}

static bool isKeptByInstancesForLife(BuiltinID member)
{
    return member == Builtins::ArrayPrototypeLength || member == Builtins::StringPrototypeLength || member == Builtins::RegExpPrototypeLastIndex;
}

struct EntryBelow {
    BuiltinID parent;
    BuiltinID entry;

    auto operator<=>(const EntryBelow&) const = default;
};

static Vector<EntryBelow> listEntriesBelowEntries(const BuiltinTable& builtins)
{
    Vector<EntryBelow> list;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const Builtin& entry = builtins.at(BuiltinID(index));
        if (isObjectKind(entry.kind) && entry.prototypeLink == BuiltinPrototypeLink::Builtin)
            list.append({ builtins.canonicalID(entry.inheritsFrom), BuiltinID(index) });
    }
    AOT::sort(list);
    return list;
}

static ValueSetID entriesRightBelow(const BuiltinTable& builtins, const ValueFlowUniverse& universe, ValueSetMaker& maker, std::span<const EntryBelow> entriesBelowEntries, BuiltinID prototype)
{
    Objects entries;
    BuiltinID parent = builtins.canonicalID(prototype);
    auto first = std::ranges::lower_bound(entriesBelowEntries, parent, { }, &EntryBelow::parent);
    for (; first != entriesBelowEntries.end() && first->parent == parent; ++first)
        universe.appendObjectsOf(first->entry, entries);
    return maker.of(entries.span());
}

void ValueFlowHeapTables::collectNamesReadByNativeCode(const Program& program, const ValueFlowUniverse& universe, ValueSetMaker& maker)
{
    const BuiltinTable& builtins = program.builtins();
    auto appendNamesOf = [&](const BuiltinNamesReadByNativeCode& record, ValueFlowNamesReadByNativeCode& listed) {
        for (BuiltinName name : record.names) {
            bool isOnlyInspected = std::ranges::binary_search(record.namesOnlyInspected, name);
            size_t position = listed.names.findIf([&](const ValueFlowNameReadByNativeCode& other) {
                return other.name == name;
            });
            if (position == notFound)
                listed.names.append({ name, program.keyOf(name), isOnlyInspected });
            else
                listed.names[position].isOnlyInspected &= isOnlyInspected;
        }
    };

    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        const BuiltinNamesReadByNativeCode* record = builtins.namesReadByNativeCode(BuiltinID(index));
        if (!record)
            continue;
        ValueFlowObjectID object = universe.objectOf(BuiltinID(index));
        ValueFlowNamesReadByNativeCode listed { object, maker.of(object), { } };
        appendNamesOf(*record, listed);
        m_namesReadByNativeCode.append(WTF::move(listed));
    }

    for (const ValueFlowClassOfEntries& classOfEntries : m_classesOfEntries) {
        bool everyEntryHasList = true;
        ValueFlowNamesReadByNativeCode listed { classOfEntries.prototype, classOfEntries.entries, { } };
        maker.sets().forEachMember(classOfEntries.entries, [&](ValueFlowObjectID entry) {
            if (const BuiltinNamesReadByNativeCode* record = builtins.namesReadByNativeCode(builtins.canonicalID(universe.builtinOf(entry))))
                appendNamesOf(*record, listed);
            else
                everyEntryHasList = false;
        });
        if (everyEntryHasList)
            m_namesReadByNativeCode.append(WTF::move(listed));
    }
}

const ValueFlowNameReadByNativeCode* ValueFlowNamesReadByNativeCode::find(PropertyKeyID key) const
{
    for (const ValueFlowNameReadByNativeCode& read : names) {
        if (read.key == key)
            return &read;
    }
    return nullptr;
}

const ValueFlowNamesReadByNativeCode* ValueFlowHeapTables::namesReadByNativeCodeOn(ValueFlowObjectID object) const
{
    for (const ValueFlowNamesReadByNativeCode& listed : m_namesReadByNativeCode) {
        if (listed.object == object)
            return &listed;
    }
    return nullptr;
}

static constexpr uint32_t recordOfOtherKey = 0;
static constexpr uint32_t recordOfEnginePrivateName = 1;
static constexpr uint32_t recordOfText = 2;
static constexpr size_t numberOfRecordsOfUnnamedKeys = 3;

std::span<const ValueFlowObjectID> ValueFlowHeapTables::objectsOf(BuiltinID canonicalHolder) const
{
    return entriesOfKey(m_objectsOfHolders, m_endOfObjectsOfHolder, canonicalHolder.index());
}

ValueFlowObjectID ValueFlowHeapTables::objectOf(BuiltinID builtin) const
{
    return ValueFlowObjectID(m_firstObjectOfBuiltins + m_program.builtins().canonicalID(builtin).index());
}

void ValueFlowHeapTables::describeKey(PropertyKeyID key) const
{
    Locker locker { m_lock };
    HoldersUnderKey& holders = m_holdersUnderKey[key.index()];
    if (holders.isDescribed.load(std::memory_order_relaxed))
        return;
    holders.record = findOrAddRecordOf(key);
    const ValueFlowMembersUnderKey& members = m_membersUnderKey[holders.record];
    ValueSetID created = m_maker.of(entriesOfKey(m_objectsCreatedWithKeys, m_endOfObjectsCreatedWithKey, key.index()));
    holders.storedToAtCreation = m_maker.of(entriesOfKey(m_objectsStoredToUnderKeys, m_endOfObjectsStoredToUnderKey, key.index()));
    holders.thatAnswer = m_maker.unite(members.holders, created);
    ValueSetID holdersOfOpenLocations = m_maker.intersect(members.holders, ValueSets::complement(members.holdersWithLockedLocation));
    holders.thatMayLoseMember = m_maker.unite(m_maker.unite(holdersOfOpenLocations, created), holders.storedToAtCreation);
    holders.isDescribed.store(true, std::memory_order_release);
}

uint32_t ValueFlowHeapTables::findOrAddRecordOf(PropertyKeyID key) const
{
    BuiltinMemberName name = builtinMemberNameOf(m_program, key);
    bool isPrivateName = m_program.at(key).kind == PropertyKeyKind::EnginePrivateName;
    if (!name) {
        if (isPrivateName)
            return recordOfEnginePrivateName;
        return m_program.at(key).kind == PropertyKeyKind::Text ? recordOfText : recordOfOtherKey;
    }
    return (isPrivateName ? m_recordOfPrivateName : m_recordOfName).ensure(name.name.index(), [&] {
        RELEASE_ASSERT(m_membersUnderKey.size() < m_membersUnderKey.capacity());
        m_membersUnderKey.append(describeMembersUnder(key, name.name, isPrivateName));
        return static_cast<uint32_t>(m_membersUnderKey.size() - 1);
    }).iterator->value;
}

ValueFlowMembersUnderKey ValueFlowHeapTables::describeMembersUnder(PropertyKeyID key, BuiltinName name, bool isPrivateName) const
{
    const BuiltinTable& builtins = m_program.builtins();
    MembersUnderKey membersUnderKey(m_program, key);
    std::span<const ValueFlowObjectID> objectsOfGlobal = objectsOf(Builtins::GlobalThis);
    Objects objectsOfHolders;
    Objects holdersOfEvaluators;
    Objects membersOfEngineAndLibrary;
    auto addHolder = [&](BuiltinID holder) {
        objectsOfHolders.append(objectsOf(holder));
        BuiltinID member = membersUnderKey.findOwnMember(holder);
        if (member && builtins.at(member).origin != BuiltinOrigin::Host)
            membersOfEngineAndLibrary.append(objectOf(member));
    };
    for (auto [holder, member] : entriesOfKey(m_membersUnderNames, m_endOfMembersUnderName, name.index())) {
        if (!isPrivateName || builtins.at(member).flags.contains(BuiltinFlag::HasPrivateAlias))
            addHolder(holder);
        if (isPrivateName && holder == Builtins::PrivateGlobals)
            addHolder(Builtins::GlobalThis);
        if (!letsInUnseenCode(member))
            continue;
        holdersOfEvaluators.append(objectsOf(holder));
        if (holder == Builtins::PrivateGlobals)
            holdersOfEvaluators.append(objectsOfGlobal);
    }
    if (!isPrivateName && name == builtins.at(Builtins::GlobalThis).name)
        addHolder(Builtins::GlobalThis);

    ValueSetID holders = m_maker.of(objectsOfHolders.span());
    ValueSetID undeclared = m_maker.intersect(isPrivateName ? m_builtinObjects : m_entriesOpenToTableOrNativeCode, ValueSets::complement(holders));
    // BuiltinTable::mismatches() compares: an object of the table that has such a method lists it.
    if (!isPrivateName && std::ranges::contains(builtins.namesOfHostMethodsCallingStartupLoads(), name))
        undeclared = m_maker.intersect(undeclared, openTo(Actor::NativeCode));
    ValueSetID instancesThatKeepProperty = ValueSets::nothing;
    if (!isPrivateName) {
        Objects prototypes;
        for (auto found = std::ranges::lower_bound(m_prototypesOfInstancesWithNames, name, { }, &HolderListingName::name); found != m_prototypesOfInstancesWithNames.end() && found->name == name; ++found)
            prototypes.append(objectsOf(found->holder));
        undeclared = m_maker.unite(undeclared, m_maker.of(prototypes.span()));
        for (auto [keptName, instances] : m_instancesThatKeepName) {
            if (keptName == name)
                instancesThatKeepProperty = instances;
        }
    }
    return { holders, m_maker.intersect(holders, m_prototypeObjects), undeclared, m_maker.unite(m_lockedObjects, findLockedHolders(membersUnderKey, name, isPrivateName)), m_maker.intersect(holders, m_maker.of(holdersOfEvaluators.span())),
        instancesThatKeepProperty, m_maker.intersect(holders, m_prototypeObjectsOfEntries), m_maker.of(membersOfEngineAndLibrary.span()) };
}

ValueSetID ValueFlowHeapTables::findLockedHolders(const MembersUnderKey& membersUnderKey, BuiltinName name, bool isPrivateName) const
{
    Objects locked;
    auto appendIfLocked = [&](BuiltinID holder, Objects& list) {
        if (membersUnderKey.isLockedLocation(holder))
            list.append(objectsOf(holder));
    };
    Vector<BuiltinID, 16> closedHoldersListingName;
    auto noteHolderListingName = [&](BuiltinID holder) {
        if (m_hasClosedKeys.quickGet(holder.index()) && !m_listsNameOfKey.quickSet(holder.index()))
            closedHoldersListingName.append(holder);
    };
    for (auto [holder, member] : entriesOfKey(m_membersUnderNames, m_endOfMembersUnderName, name.index())) {
        appendIfLocked(holder, locked);
        noteHolderListingName(holder);
    }
    for (auto found = std::ranges::lower_bound(m_closedHoldersListingNames, name, { }, &HolderListingName::name); found != m_closedHoldersListingNames.end() && found->name == name; ++found)
        noteHolderListingName(found->holder);

    // A closed holder gives one answer to every key of a class that it does not list.
    ClosedHoldersNotListingKey& others = m_closedHoldersNotListingKeyOfClass[membersUnderKey.classOfKey()];
    if (!std::exchange(others.isStarted, true))
        others.notAsked = m_holdersWithClosedKeys;
    Objects lockedAmongOthers;
    others.notAsked.removeAllMatching([&](BuiltinID holder) {
        if (m_listsNameOfKey.quickGet(holder.index()))
            return false;
        appendIfLocked(holder, lockedAmongOthers);
        return true;
    });
    others.locked = m_maker.unite(others.locked, m_maker.of(lockedAmongOthers.span()));

    Objects objectsListingName;
    for (BuiltinID holder : closedHoldersListingName) {
        appendIfLocked(holder, locked);
        objectsListingName.append(objectsOf(holder));
        m_listsNameOfKey.quickClear(holder.index());
    }
    ValueSetID result = m_maker.unite(m_maker.of(locked.span()), m_maker.intersect(others.locked, ValueSets::complement(m_maker.of(objectsListingName.span()))));
    if (!isPrivateName && m_program.builtins().isClosedOnHostFunctions(name))
        result = m_maker.unite(result, m_functionsOfEngine);
    return result;
}

const ValueFlowMembersOfHolder& ValueFlowHeapTables::describeMembersOf(BuiltinID holder) const
{
    Locker locker { m_lock };
    std::atomic<const ValueFlowMembersOfHolder*>& described = m_membersOfHolder[holder.index()];
    if (const ValueFlowMembersOfHolder* members = described.load(std::memory_order_relaxed))
        return *members;

    const BuiltinTable& builtins = m_program.builtins();
    const Builtin& entry = builtins.at(holder);
    Objects values;
    Objects accessors;
    Objects accessorsWithGetEntry;
    Objects accessorsWithSetEntry;
    ValueSetID primitives = ValueSets::nothing;
    bool hasNativeBinding = false;
    Objects leftOut;
    bool isHolder = isObjectKind(entry.kind) && builtins.canonicalID(holder) == holder && entry.memberCount;
    for (BuiltinID member : isHolder ? builtins.membersOf(holder) : BuiltinTableIDRange<BuiltinID>()) {
        const Builtin& memberEntry = builtins.at(member);
        ValueFlowObjectID object = objectOf(member);
        if (hasGetEntry(member))
            accessorsWithGetEntry.append(object);
        if (hasSetEntry(member))
            accessorsWithSetEntry.append(object);
        if (letsInUnseenCode(member)) {
            if (!isObjectKind(memberEntry.kind))
                continue;
            leftOut.append(object);
            for (BuiltinID held : builtins.membersOf(builtins.canonicalID(member))) {
                if (isObjectKind(builtins.at(held).kind))
                    leftOut.append(objectOf(held));
            }
            continue;
        }
        switch (memberEntry.kind) {
        case BuiltinKind::Namespace:
        case BuiltinKind::Constructor:
        case BuiltinKind::Function:
        case BuiltinKind::Prototype:
        case BuiltinKind::Method:
            values.append(object);
            break;
        case BuiltinKind::Accessor:
        case BuiltinKind::DataProperty:
            accessors.append(object);
            break;
        case BuiltinKind::Constant:
            primitives = m_anyPrimitive;
            break;
        case BuiltinKind::NativeBinding:
            hasNativeBinding = true;
            break;
        }
    }
    m_describedMembersOfHolders.append(std::make_unique<ValueFlowMembersOfHolder>(ValueFlowMembersOfHolder { m_maker.unite(m_maker.of(values.span()), primitives), m_maker.of(accessors.span()), m_maker.of(accessorsWithGetEntry.span()), m_maker.of(accessorsWithSetEntry.span()), hasNativeBinding, m_maker.of(leftOut.span()) }));
    described.store(m_describedMembersOfHolders.last().get(), std::memory_order_release);
    return *m_describedMembersOfHolders.last();
}

bool isAnswerOfModuleOperation(const Function& function, ValueID value)
{
    const Value* definition = &function.at(definitionThroughSameValues(function, value));
    if (definition->opcode == Opcode::Await)
        definition = &function.at(definitionThroughSameValues(function, definition->operand(OperandIndex::Await::value)));
    return flowRuleOf(definition->opcode) == FlowRule::Module;
}

ConstantsHoldingModules::ConstantsHoldingModules(const Program& program)
    : m_variables(program.variables.size())
{
    for (const Function& function : program.functions) {
        for (const Value& store : function.values) {
            if (store.opcode == Opcode::PutModuleVariable) {
                if (program.at(store.variable()).flags.contains(VariableFlags::IsImmutable) && isAnswerOfModuleOperation(function, store.valueOperand()))
                    m_variables.set(store.variable().index());
            } else if (store.opcode == Opcode::PutEnvironmentField) {
                if (program.at(store.layout()).fields[store.field()].flags.contains(FieldFlags::IsImmutable) && isAnswerOfModuleOperation(function, store.valueOperand()))
                    m_fields.append(keyOf(store.layout(), store.field()));
            }
        }
    }
    AOT::sort(m_fields);
}

bool ConstantsHoldingModules::isReadBy(const Value& read) const
{
    if (read.opcode == Opcode::GetModuleVariable)
        return m_variables.get(read.variable().index());
    return read.opcode == Opcode::GetEnvironmentField && std::ranges::binary_search(m_fields, keyOf(read.layout(), read.field()));
}

static bool spellsGlobalObject(const Program& program, const Function& function, ValueID id)
{
    const BuiltinTable& builtins = program.builtins();
    const Value& object = function.at(sameObjectThroughCasts(function, id));
    if (object.opcode == Opcode::Builtin)
        return builtins.canonicalID(object.builtin()) == Builtins::GlobalThis;
    if (object.opcode != Opcode::PropGet || !object.isFreeName())
        return false;
    if (object.key() == program.keyOf(builtins.at(Builtins::GlobalThis).name))
        return true;
    BuiltinID member = findOwnMember(program, Builtins::GlobalThis, object.key());
    return member && builtins.canonicalID(member) == Builtins::GlobalThis;
}

static BitVector findKeysStoredUnderOnGlobalObject(const Program& program, const ParallelLoops& loops)
{
    Vector<PropertyKeyID> stored = collectInParallel<PropertyKeyID>(loops, program.functions.size(), [&](unsigned index, Vector<PropertyKeyID>& found) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !mayHoldValueWith(program, FunctionID(index), { Opcode::PropSet }))
            return;
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& operation = function.at(id);
                if (operation.opcode == Opcode::PropSet && spellsGlobalObject(program, function, operation.operand(OperandIndex::PropSet::object)))
                    found.append(operation.key());
            }
        }
    });
    BitVector keys(program.types.numberOfPropertyKeys());
    for (PropertyKeyID key : stored)
        keys.set(key.index());
    return keys;
}

void ValueFlowHeapTables::listWherePackagesAreNamedInPaths()
{
    static constexpr std::array<uint8_t, 12> folder { 'n', 'o', 'd', 'e', '_', 'm', 'o', 'd', 'u', 'l', 'e', 's' };
    static constexpr std::array<uint8_t, 5> prefixOfLibrary { 'n', 'o', 'd', 'e', ':' };
    m_mayAnswerToName.ensureSize(m_program.modules.size());
    for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
        if (!Typed::mayAnswerToName(m_program, ModuleID(index)))
            continue;
        m_mayAnswerToName.quickSet(index);
        StringID path = m_program.modules[index].path;
        std::span<const uint8_t> text = m_program.string(path);
        m_whereNamesOfPackagesBegin.append(RestOfPath { path, 0 });
        if (const Module& module = m_program.modules[index]; module.runtimeModule && (!module.body || isAskedOfHostByName(module))) {
            m_namesOfLibraryModules.append(RestOfPath { path, 0 });
            if (spanHasPrefix(text, std::span(prefixOfLibrary)))
                m_namesOfLibraryModules.append(RestOfPath { path, static_cast<uint32_t>(prefixOfLibrary.size()) });
        }
        for (size_t start = 0; start + folder.size() < text.size(); ++start) {
            uint8_t separator = text[start + folder.size()];
            if ((separator == '/' || separator == '\\') && spanHasPrefix(text.subspan(start), std::span(folder)))
                m_whereNamesOfPackagesBegin.append(RestOfPath { path, static_cast<uint32_t>(start + folder.size() + 1) });
        }
    }
    for (Vector<RestOfPath>* list : { &m_whereNamesOfPackagesBegin, &m_namesOfLibraryModules }) {
        AOT::sort(*list, [&](RestOfPath first, RestOfPath second) {
            return std::ranges::lexicographical_compare(textOf(first), textOf(second));
        });
    }
}

auto ValueFlowHeapTables::findFirstNotBefore(const Vector<RestOfPath>& sorted, std::span<const uint8_t> text) const -> const RestOfPath*
{
    auto found = std::ranges::lower_bound(sorted, text, [](std::span<const uint8_t> first, std::span<const uint8_t> second) {
        return std::ranges::lexicographical_compare(first, second);
    }, [&](RestOfPath rest) {
        return textOf(rest);
    });
    return found == sorted.end() ? nullptr : &*found;
}

bool ValueFlowHeapTables::holdsModuleOfPackage(std::span<const uint8_t> package) const
{
    const RestOfPath* found = findFirstNotBefore(m_whereNamesOfPackagesBegin, package);
    return found && spanHasPrefix(textOf(*found), package);
}

bool ValueFlowHeapTables::isNameOfLibraryModule(std::span<const uint8_t> name) const
{
    const RestOfPath* found = findFirstNotBefore(m_namesOfLibraryModules, name);
    return found && equalSpans(textOf(*found), name);
}

ValueFlowHeapTables::ValueFlowHeapTables(const Program& program, const ValueFlowUniverse& universe, ValueSets& allSets, ValueSetMaker& maker, const ParallelLoops& loops, NOESCAPE const ScopedLambda<void()>& runBesideTables)
    : m_program(program)
    , m_maker(allSets)
{
    const BuiltinTable& builtins = program.builtins();
    auto without = [&](ValueSetID set, ValueSetID leftOut) {
        return maker.intersect(set, ValueSets::complement(leftOut));
    };

    ValueSetID unknownObject = universe.unknownObjectAlone();
    ValueSetID placeholders = universe.placeholdersAlone();
    ValueSetID builtinObjects = without(universe.builtinObjects(), placeholders);
    m_builtinObjects = builtinObjects;
    m_anyPrimitive = universe.anyPrimitive();
    m_firstObjectOfBuiltins = universe.objectOf(Builtins::GlobalThis).index() - builtins.canonicalID(Builtins::GlobalThis).index();
    m_builtinObjectsAndPlaceholders = maker.unite(builtinObjects, placeholders);
    m_stringAndSymbolKinds = kindsOf(maker, Primitive::String | Primitive::Symbol);
    m_numberKinds = kindsOf(maker, Primitive::Number);
    m_booleanKinds = kindsOf(maker, Primitive::Boolean);
    m_startsOfLookups = ValueSets::complement(maker.unite(maker.unite(placeholders, universe.keys()), kindsOf(maker, Primitive::UndefinedOrNull | Primitive::Absent)));
    m_keysNobodyKnows = ValueSets::complement(maker.unite(universe.keys(), m_numberKinds));
    m_accessors = maker.unite(maker.unite(universe.callables(), builtinObjects), placeholders);
    m_undescribedNativeCode = universe.objectOf(Builtins::UndeclaredNativeCode);
    if (BuiltinMemberLookup found = builtins.findMember(Builtins::ObjectPrototype, BuiltinNames::__proto__); found.outcome == BuiltinMemberLookup::Outcome::Found)
        m_accessorOfPrototypeLink = universe.objectOf(found.member);
    m_knownAccessors = without(m_accessors, maker.unite(unknownObject, maker.of(m_undescribedNativeCode)));
    m_whatIsNewToNativeCode = ValueSets::complement(builtinObjects);
    m_objectsOfRealm = universe.objectsOfRealm();
    m_prototypeKey = program.keyOf(BuiltinNames::prototype);
    m_constructorKey = program.keyOf(BuiltinNames::constructor);
    m_rawKey = program.keyOf(BuiltinNames::raw);
    m_lengthKey = program.keyOf(BuiltinNames::length);
    m_calleeKey = program.keyOf(BuiltinNames::callee);
    m_nameKey = program.keyOf(BuiltinNames::name);
    if (StringID text = program.strings.find(byteCast<uint8_t>("displayName"_span)))
        m_displayNameKey = program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = text }).value_or(PropertyKeyID());
    listWherePackagesAreNamedInPaths();

    m_prototypesAtCreation = ZeroFilledPages<std::atomic<uint64_t>>(universe.numberOfObjects());
    unsigned bitsOfBlocks = std::clamp<unsigned>(std::bit_width(static_cast<uint64_t>(universe.numberOfObjects())) + 2, 12, 20);
    m_shiftOfBlocks = 64 - bitsOfBlocks;
    m_prototypesOfBlocks = ZeroFilledPages<std::atomic<uint64_t>>(static_cast<size_t>(5) << bitsOfBlocks);

    uint32_t numberOfKeys = universe.numberOfObjects() - universe.keyConstant(PropertyKeyID(0u)).index();
    ValueSetID knownObjectsOfProgram = without(universe.knownObjects(), builtinObjects);
    Vector<ValueFlowObjectID> closuresWithPrototypeProperty;
    Vector<ValueFlowObjectID> templateObjects;
    Vector<ValueFlowObjectID> argumentsObjects;
    Vector<ValueFlowObjectID> arraysThatOperationsAllocate;
    Vector<ValueFlowObjectID> ordinaryObjects;
    Vector<ValueFlowObjectID> objectsHoldingOnlyWhatWasWritten;
    Vector<ValueFlowObjectID> objectsCreatedWithFields;

    auto describeMembersOfTable = [&] {
        m_hasGetEntry.ensureSize(builtins.builtinCount());
        m_hasSetEntry.ensureSize(builtins.builtinCount());
        m_letsInUnseenCode.ensureSize(builtins.builtinCount());
        Vector<ValueFlowObjectID, 32> objectsThatLetInUnseenCode;
        Vector<ValueFlowObjectID, 32> objectsThatLetInUnseenCodeWhenCalled;
        // f.call(..), f.bind(..), Reflect.apply(f, ..), list.map(f): the row says that the function calls its receiver or an argument, or answers a bound function.
        auto callsOrBindsOperand = [&](BuiltinID function) {
            auto isCalled = [](const BuiltinParameter& parameter) {
                return parameter.uses != OperandUses::all() && parameter.uses.containsAny(callingUses);
            };
            return std::ranges::any_of(builtins.overloadsOf(function), [&](const BuiltinOverload& overload) {
                if ((overload.receiver && isCalled(*overload.receiver)) || std::ranges::any_of(overload.parameters, isCalled))
                    return true;
                return overload.result.type && std::ranges::any_of(builtins.at(overload.result.type).parts, [](const BuiltinTypePart& part) {
                    return part.kind == BuiltinTypePartKind::Instance && part.builtinClass == BuiltinClasses::BoundFunction;
                });
            });
        };
        Vector<ValueFlowObjectID, 32> functionsThatCallOrBindOperand;
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.dispatchOf(BuiltinID(index), BuiltinCallMode::Get))
                m_hasGetEntry.quickSet(index);
            if (builtins.dispatchOf(BuiltinID(index), BuiltinCallMode::Set))
                m_hasSetEntry.quickSet(index);
            if (builtins.letsInUnseenCode(builtins.canonicalID(BuiltinID(index))))
                m_letsInUnseenCode.quickSet(index);
            if (builtins.canonicalID(BuiltinID(index)) == BuiltinID(index) && builtins.letsInUnseenCode(BuiltinID(index)))
                universe.appendObjectsOf(BuiltinID(index), objectsThatLetInUnseenCode);
            if (builtins.canonicalID(BuiltinID(index)) == BuiltinID(index) && builtins.letsInUnseenCodeWhenCalled(BuiltinID(index)))
                universe.appendObjectsOf(BuiltinID(index), objectsThatLetInUnseenCodeWhenCalled);
            if (builtins.canonicalID(BuiltinID(index)) == BuiltinID(index) && callsOrBindsOperand(BuiltinID(index)))
                universe.appendObjectsOf(BuiltinID(index), functionsThatCallOrBindOperand);
        }
        m_functionsThatCallOrBindOperand = maker.intersect(maker.of(functionsThatCallOrBindOperand.span()), universe.builtinFunctions());
        m_objectsThatLetInUnseenCode = maker.of(objectsThatLetInUnseenCode.span());
        m_objectsThatLetInUnseenCodeWhenCalled = maker.of(objectsThatLetInUnseenCodeWhenCalled.span());
        Vector<ValueFlowObjectID, 32> objectsOfNativeModules;
        Vector<ValueFlowObjectID, 32> objectsOfPublicModules;
        for (uint32_t index = 0; index < builtins.moduleCount(); ++index) {
            const RuntimeModuleData& module = builtins.at(RuntimeModuleID(index));
            if (module.namespaceObject && module.flags.contains(RuntimeModuleFlag::IsPublic))
                objectsOfPublicModules.append(universe.objectOf(module.namespaceObject));
            if (module.namespaceObject && module.flags.contains(RuntimeModuleFlag::IsPublic) && !module.flags.contains(RuntimeModuleFlag::HasSource) && !builtins.at(builtins.canonicalID(module.namespaceObject)).holder)
                objectsOfNativeModules.append(universe.objectOf(module.namespaceObject));
        }
        m_objectsOfNativeModules = maker.of(objectsOfNativeModules.span());
        m_objectsOfPublicModules = maker.of(objectsOfPublicModules.span());

        Vector<EntryBelow> entriesBelowEntries = listEntriesBelowEntries(builtins);
        Objects prototypeObjects;
        Objects prototypeObjectsOfLanguage;
        Objects prototypeObjectsOfEntries;
        Objects prototypeObjectsOfHost;
        Objects prototypesOfTypedArrays;
        Objects prototypeObjectsThatListingDoesNotName;
        Objects prototypesAboveNativeElements;
        Objects prototypeObjectsOfClassesThatListNoOwnNames;
        for (uint32_t index = 0; index < builtins.classCount(); ++index) {
            const BuiltinClass& builtinClass = builtins.at(BuiltinClassID(index));
            if (!builtinClass.prototype)
                continue;
            ValueFlowObjectID prototype = universe.objectOf(builtinClass.prototype);
            if (!builtinClass.listsNamesDefinedOnNewInstances)
                prototypeObjectsOfClassesThatListNoOwnNames.append(prototype);
            bool reachesPrograms = builtinClass.id != BuiltinClasses::AsyncFromSyncIterator;
            if (builtinClass.instancesAreEntries) {
                m_classesOfEntries.append({ prototype, entriesRightBelow(builtins, universe, maker, entriesBelowEntries.span(), builtinClass.prototype) });
                prototypeObjectsOfEntries.append(prototype);
            } else if (reachesPrograms) {
                prototypeObjects.append(prototype);
                if (builtinClass.origin == BuiltinOrigin::Host) {
                    prototypeObjectsOfHost.append(prototype);
                    if (ValueSetID entries = entriesRightBelow(builtins, universe, maker, entriesBelowEntries.span(), builtinClass.prototype); entries != ValueSets::nothing)
                        m_classesOfHostWithEntries.append({ prototype, entries });
                }
            }
            if (builtinClass.origin == BuiltinOrigin::Engine && reachesPrograms)
                prototypeObjectsOfLanguage.append(prototype);
            if (!builtinClass.isPrimitiveWrapper)
                prototypeObjectsThatListingDoesNotName.append(prototype);
            if (builtins.typedArrayTypeOf(builtinClass.id) != NotTypedArray)
                prototypesOfTypedArrays.append(prototype);
            if (builtins.typedArrayTypeOf(builtinClass.id) != NotTypedArray || builtins.definesCharactersOnNewInstances(builtinClass.id))
                prototypesAboveNativeElements.append(prototype);
        }
        collectNamesReadByNativeCode(program, universe, maker);
        m_prototypeObjects = maker.of(prototypeObjects.span());
        m_prototypeObjectsOfLanguage = maker.of(prototypeObjectsOfLanguage.span());
        m_prototypeObjectsOfHost = maker.of(prototypeObjectsOfHost.span());
        m_prototypesOfTypedArrays = maker.of(prototypesOfTypedArrays.span());
        m_prototypeObjectsThatListingDoesNotName = maker.of(prototypeObjectsThatListingDoesNotName.span());
        m_prototypesAboveNativeElements = maker.of(prototypesAboveNativeElements.span());
        m_prototypeObjectsOfClassesThatListNoOwnNames = maker.of(prototypeObjectsOfClassesThatListNoOwnNames.span());

        Objects hostObjectsKeepingStrings;
        std::span<const BuiltinHostObject> hostObjects = builtins.hostObjects();
        for (uint32_t index = 0; index < hostObjects.size(); ++index) {
            if (hostObjects[index].keepsStringsOfStoredValues)
                hostObjectsKeepingStrings.append(universe.hostObject(index));
        }
        m_hostObjectsKeepingStrings = maker.of(hostObjectsKeepingStrings.span());
        m_stringsNumbersAndBooleans = maker.unite(kindsOf(maker, Primitive::String | Primitive::LiteralString | Primitive::Number | Primitive::Boolean), without(universe.keys(), universe.symbols()));

        Objects holdersWithMembers;
        Objects holdersWithMembersUnderNumericNames;
        Objects holdersOfMembersThatHostMakes;
        Objects holdersOfReplaceableMembers;
        Objects namespacesWhoseHoldersListThem;
        Objects holdersThatListAllMembers;
        Objects lockedObjects;
        Objects holdersWithLockedIndexes;
        Objects holdersWithLockedPrototypeLink;
        Objects holdersClosedToOtherNames;
        Objects functionsOfEngine;
        Objects prototypesOfInstancesWithNames;
        Objects prototypeObjectsListingOwnPropertiesOfInstances;
        struct NamedMember {
            BuiltinName name;
            MemberOfHolder member;
        };
        Vector<NamedMember> namedMembers;
        m_endOfMembersUnderName.fill(0u, builtins.nameCount());
        m_endOfObjectsOfHolder.fill(0u, builtins.builtinCount());
        m_hasClosedKeys.ensureSize(builtins.builtinCount());
        m_listsNameOfKey.ensureSize(builtins.builtinCount());
        m_hasNumericName.ensureSize(builtins.builtinCount());
        m_compiledModuleOfHolder.fill(ModuleID(), builtins.builtinCount());
        for (uint32_t index = 0; index < program.modules.size(); ++index) {
            const Module& module = program.modules[index];
            BuiltinID object = module.kind == ModuleKind::RuntimeSource && module.runtimeModule ? builtins.namespaceOf(module.runtimeModule) : BuiltinID();
            if (object && compiledModuleWithObject(program, builtins.canonicalID(object)) == ModuleID(index))
                m_compiledModuleOfHolder[builtins.canonicalID(object).index()] = ModuleID(index);
        }
        auto hasNumericText = [&](BuiltinName name) {
            StringID text = program.strings.stringOf(name);
            return mayBeElementKey(text ? program.strings.nameClass(text) : classifyName(builtins.textOf(name)));
        };
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            m_endOfObjectsOfHolder[index] = m_objectsOfHolders.size();
            BuiltinID holder(index);
            const Builtin& entry = builtins.at(holder);
            if (!isObjectKind(entry.kind) || builtins.canonicalID(holder) != holder)
                continue;

            Objects objects;
            universe.appendObjectsOf(holder, objects);
            m_objectsOfHolders.appendVector(objects);
            m_endOfObjectsOfHolder[index] = m_objectsOfHolders.size();
            if (entry.flags.contains(BuiltinFlag::ListsAllMembers) || m_compiledModuleOfHolder[index])
                holdersThatListAllMembers.appendVector(objects);
            if (entry.flags.contains(BuiltinFlag::IsLockedObject))
                lockedObjects.appendVector(objects);
            if (builtins.closesIndexKeys(holder))
                holdersWithLockedIndexes.appendVector(objects);
            if (builtins.hasLockedPrototypeLink(holder))
                holdersWithLockedPrototypeLink.appendVector(objects);
            if (entry.flags.contains(BuiltinFlag::IsClosedToOtherNames))
                holdersClosedToOtherNames.appendVector(objects);
            bool hasClosedKeys = entry.flags.contains(BuiltinFlag::IsClosedToOtherNames) || !builtins.closedKeysOf(holder).empty();
            auto listName = [&](BuiltinName name) {
                if (name)
                    m_closedHoldersListingNames.append({ name, holder });
            };
            if (hasClosedKeys) {
                m_holdersWithClosedKeys.append(holder);
                m_hasClosedKeys.quickSet(index);
                listName(entry.name);
                for (BuiltinName name : builtins.closedKeysOf(holder))
                    listName(name);
                for (BuiltinName name : builtins.openNamesOf(holder))
                    listName(name);
            }
            if (builtins.isFunctionOfEngine(holder))
                functionsOfEngine.appendVector(objects);
            if (entry.kind == BuiltinKind::Prototype && entry.instanceClass) {
                bool listsNameWithoutType = false;
                for (BuiltinName name : builtins.at(entry.instanceClass).namesDefinedOnNewInstances) {
                    if (builtins.typeOfNameDefinedOnNewInstances(entry.instanceClass, name))
                        continue;
                    m_prototypesOfInstancesWithNames.append({ name, holder });
                    listsNameWithoutType = true;
                }
                if (listsNameWithoutType)
                    prototypesOfInstancesWithNames.appendVector(objects);
            }
            if (!entry.memberCount)
                continue;
            holdersWithMembers.appendVector(objects);

            bool hasMemberThatHostMakes = false;
            bool hasReplaceableMember = false;
            bool hasMemberUnderNumericName = false;
            bool listsOwnPropertyOfInstances = false;
            for (BuiltinID member : builtins.membersOf(holder)) {
                const Builtin& memberEntry = builtins.at(member);
                if (!memberEntry.name)
                    continue;
                listsOwnPropertyOfInstances |= memberEntry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
                if (memberEntry.flags.contains(BuiltinFlag::IsDefaultExport)) {
                    if (hasClosedKeys)
                        listName(memberEntry.name);
                    continue;
                }
                hasMemberThatHostMakes = hasMemberThatHostMakes || isMadeWithCodeOfLibrary(builtins, member);
                hasReplaceableMember = hasReplaceableMember || canLockAtStartUp(builtins, member);
                if (hasNumericText(memberEntry.name)) {
                    m_hasNumericName.quickSet(member.index());
                    hasMemberUnderNumericName = true;
                }
                namedMembers.append({ memberEntry.name, { holder, member } });
                ++m_endOfMembersUnderName[memberEntry.name.index()];
                if (isKeptByInstancesForLife(builtins.canonicalID(member)) && entry.instanceClass) {
                    size_t position = m_instancesThatKeepName.findIf([&](const auto& kept) {
                        return kept.first == memberEntry.name;
                    });
                    if (position == notFound) {
                        position = m_instancesThatKeepName.size();
                        m_instancesThatKeepName.append({ memberEntry.name, ValueSets::nothing });
                    }
                    ValueSetID& instances = m_instancesThatKeepName[position].second;
                    instances = maker.unite(instances, universe.sureInstancesOf(entry.instanceClass));
                    if (PropertyKeyID key = program.keyOf(memberEntry.name))
                        m_propertiesKeptForLife.append({ key, entry.instanceClass });
                    if (entry.instanceClass == BuiltinClasses::String)
                        instances = maker.unite(instances, kindsOf(maker, Primitive::AnyString));
                }
            }
            if (hasMemberThatHostMakes) {
                holdersOfMembersThatHostMakes.appendVector(objects);
                bool isNamespaceOrFunction = entry.kind == BuiltinKind::Namespace || entry.kind == BuiltinKind::Function;
                if (isNamespaceOrFunction && holder != Builtins::GlobalThis && holder != Builtins::Bun)
                    namespacesWhoseHoldersListThem.appendVector(objects);
            }
            if (hasReplaceableMember)
                holdersOfReplaceableMembers.appendVector(objects);
            if (hasMemberUnderNumericName)
                holdersWithMembersUnderNumericNames.appendVector(objects);
            if (listsOwnPropertyOfInstances)
                prototypeObjectsListingOwnPropertiesOfInstances.appendVector(objects);
        }
        m_membersUnderNames.grow(layOutByKey(m_endOfMembersUnderName));
        for (const NamedMember& named : namedMembers)
            m_membersUnderNames[m_endOfMembersUnderName[named.name.index()]++] = named.member;
        Objects keysOfMembersOfBun;
        for (const NamedMember& named : namedMembers) {
            PropertyKeyID key = program.keyOf(named.name);
            if (key && builtins.canonicalID(named.member.holder) == Builtins::Bun && !letsInUnseenCode(builtins.canonicalID(named.member.member)))
                keysOfMembersOfBun.append(universe.keyConstant(key));
        }
        m_keysOfMembersOfBun = maker.of(keysOfMembersOfBun.span());
        AOT::sort(m_closedHoldersListingNames);
        AOT::sort(m_prototypesOfInstancesWithNames);
        for (auto [mark, member] : builtins.marked(BuiltinMark::IsReplacedBySetter)) {
            if (PropertyKeyID key = program.keyOf(builtins.at(member).name))
                m_membersReplacedBySetters.append({ key, universe.objectOf(builtins.at(member).holder) });
        }

        m_holdersWithMembers = maker.of(holdersWithMembers.span());
        m_holdersWithMembersUnderNumericNames = maker.of(holdersWithMembersUnderNumericNames.span());
        m_holdersOfMembersThatHostMakes = maker.of(holdersOfMembersThatHostMakes.span());
        // HeapRule::notePrototypeObjectsThatWriteMayReach() says which of the prototype objects that no listing names a write reaches.
        m_holdersOfReplaceableMembers = without(maker.of(holdersOfReplaceableMembers.span()), m_prototypeObjectsThatListingDoesNotName);
        m_namespacesWhoseHoldersListThem = maker.of(namespacesWhoseHoldersListThem.span());
        m_namespacesNotedWhenCarriedOut = maker.unite(m_namespacesWhoseHoldersListThem, universe.holdersOfLibraryExports());
        for (Actor actor : actorsInOrder) {
            m_openTo[indexOf(actor)] = [&] {
                switch (actor) {
                case Actor::Creation:
                    return placeholders;
                case Actor::Table:
                    return maker.unite(placeholders, without(builtinObjects, maker.of(holdersThatListAllMembers.span())));
                case Actor::NativeCode:
                    return maker.unite(placeholders, maker.of(m_undescribedNativeCode));
                case Actor::Statements:
                case Actor::Loader:
                case Actor::KeysNobodyKnows:
                case Actor::ReceiversNobodyKnows:
                case Actor::UnseenCode:
                    // Which objects these reach only the solve finds.
                    return ValueSets::nothing;
                }
                RELEASE_ASSERT_NOT_REACHED();
            }();
        }
        m_entriesOpenToTableOrNativeCode = without(maker.unite(openTo(Actor::Table), openTo(Actor::NativeCode)), placeholders);
        m_holdersOfUndeclaredPropertiesUnderIndexes = m_entriesOpenToTableOrNativeCode;
        m_prototypeObjectsOfClassesThatListNamesWithoutTypes = maker.of(prototypesOfInstancesWithNames.span());
        m_prototypeObjectsListingOwnPropertiesOfInstances = maker.of(prototypeObjectsListingOwnPropertiesOfInstances.span());
        m_prototypeObjectsOfClassesThatLayPropertiesOnInstances = maker.unite(maker.unite(m_prototypeObjectsListingOwnPropertiesOfInstances, m_prototypeObjectsOfClassesThatListNamesWithoutTypes), maker.unite(m_prototypeObjectsOfClassesThatListNoOwnNames, m_prototypesAboveNativeElements));
        m_holdersOfUndeclaredPropertiesUnderAnyKey = maker.unite(m_entriesOpenToTableOrNativeCode, m_prototypeObjectsOfClassesThatListNamesWithoutTypes);

        m_lockedObjects = maker.of(lockedObjects.span());
        m_holdersWithLockedIndexes = maker.of(holdersWithLockedIndexes.span());
        m_holdersWithLockedPrototypeLink = maker.of(holdersWithLockedPrototypeLink.span());
        m_functionsOfEngine = maker.of(functionsOfEngine.span());
        m_prototypeObjectsOfEntries = maker.of(prototypeObjectsOfEntries.span());

        m_membersUnderKey.reserveInitialCapacity(numberOfRecordsOfUnnamedKeys + 2 * static_cast<size_t>(builtins.nameCount()));
        m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, m_entriesOpenToTableOrNativeCode, m_lockedObjects, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing });
        m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, builtinObjects, m_lockedObjects, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing });
        m_membersUnderKey.append({ ValueSets::nothing, ValueSets::nothing, m_entriesOpenToTableOrNativeCode, maker.unite(m_lockedObjects, maker.of(holdersClosedToOtherNames.span())), ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing });
        m_closedHoldersNotListingKeyOfClass.grow(MembersUnderKey::numberOfClasses);
        m_holdersUnderKey = std::make_unique<HoldersUnderKey[]>(numberOfKeys);
        m_membersOfHolder = std::make_unique<std::atomic<const ValueFlowMembersOfHolder*>[]>(builtins.builtinCount());
    };
    auto listObjectsCreatedWithKeys = [&] {
        const ValueSets& sets = maker.sets();
        Vector<Vector<ValueFlowObjectID>> instancesOfClassesBelow(program.layouts.size());
        sets.forEachMember(universe.instancesOfClasses(), [&](ValueFlowObjectID instance) {
            LayoutID classOfInstance = universe.layoutOf(instance);
            LayoutID current = classOfInstance;
            for (size_t steps = 0; steps < program.layouts.size() && current && program.at(current).has(LayoutFlags::HasHeritage); ++steps) {
                current = predictedHeritageOf(program.at(current));
                if (!current || current == classOfInstance)
                    continue;
                Vector<ValueFlowObjectID>& instancesBelow = instancesOfClassesBelow[current.index()];
                if (instancesBelow.isEmpty() || instancesBelow.last() != instance)
                    instancesBelow.append(instance);
            }
        });
        auto forEachInstanceAllocatedAt = [&](ValueFlowObjectID site, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) {
            if (!universe.siteOf(site) || !sets.contains(universe.objectsThatPickTheirLayout(), site))
                return;
            LayoutID allocated = universe.siteOf(site)->layout;
            LayoutID ownClass = declaredLayoutOf(program, allocated);
            if (allocated == ownClass && sets.contains(universe.instancesOfClasses(), universe.instanceOf(ownClass)))
                functor(universe.instanceOf(ownClass));
            for (ValueFlowObjectID instance : instancesOfClassesBelow[ownClass.index()])
                functor(instance);
        };
        HashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfAllocationsOfInstance;
        sets.forEachMember(universe.objectsThatPickTheirLayout(), [&](ValueFlowObjectID object) {
            forEachInstanceAllocatedAt(object, [&](ValueFlowObjectID instance) {
                ++numberOfAllocationsOfInstance.add(instance.index(), 0).iterator->value;
            });
        });

        constexpr uint32_t objectsInStretch = 4096;
        Vector<ObjectsCreatedWithFields> createdInStretches((universe.numberOfObjects() + objectsInStretch - 1) / objectsInStretch);
        forEachIndexInParallel(loops, createdInStretches.size(), [&](unsigned stretch) {
            ObjectsCreatedWithFields createdInStretch;
            FunctionID functionOfLiterals;
            KeysOfLiterals keysOfLiterals;
            FunctionID functionOfStores;
            KeysOfLiterals keysOfStores;
            auto addStores = [&](FunctionID function, ValueID allocation, std::span<const ValueFlowObjectID> created) {
                if (function != functionOfStores) {
                    functionOfStores = function;
                    keysOfStores = keysStoredRightAfterAllocations(program, program.at(function));
                }
                auto stores = keysOfStores.find(allocation.index());
                if (stores == keysOfStores.end())
                    return;
                for (PropertyKeyID key : stores->value) {
                    for (ValueFlowObjectID createdObject : created)
                        createdInStretch.storedToUnderKeys.append({ key, createdObject });
                }
            };
            FunctionID dataFunction;
            ValueID dataOperation;
            Vector<std::pair<uint32_t, const DataNode::Object*>, 8> dataObjects;
            uint32_t end = std::min(universe.numberOfObjects(), (stretch + 1) * objectsInStretch);
            for (uint32_t index = stretch * objectsInStretch; index < end; ++index) {
                ValueFlowObjectID object(index);
                const ValueFlowSite* site = universe.siteOf(object);
                if (!site)
                    continue;
                if (site->origin == ValueFlowSiteOrigin::DataNode) {
                    FunctionID function = universe.functionOf(object);
                    if (function != dataFunction || site->value != dataOperation) {
                        dataFunction = function;
                        dataOperation = site->value;
                        dataObjects.shrink(0);
                        forEachDataNodeInPreorder(program.at(program.at(function).at(site->value).data()).root, [&](uint32_t ordinal, const DataNode& node) {
                            if (const auto* dataObject = node.tryAs<DataNode::Object>())
                                dataObjects.append({ ordinal, dataObject });
                        });
                    }
                    if (!site->ordinal)
                        addStores(function, site->value, singleElementSpan(object));
                    auto found = std::ranges::lower_bound(dataObjects, site->ordinal, { }, &std::pair<uint32_t, const DataNode::Object*>::first);
                    if (found == dataObjects.end() || found->first != site->ordinal || found->second->members.isEmpty())
                        continue;
                    for (const DataMember& member : found->second->members)
                        createdInStretch.underKeys.append({ member.key, object });
                    createdInStretch.objects.append(object);
                    continue;
                }
                if (site->origin != ValueFlowSiteOrigin::Operation || site->ordinal)
                    continue;
                FunctionID function = universe.functionOf(object);
                const Value& allocation = program.at(function).at(site->value);
                if (allocation.opcode != Opcode::NewObject || program.at(allocation.layout()).isEnvironment())
                    continue;

                Vector<ValueFlowObjectID, 4> created { object };
                forEachInstanceAllocatedAt(object, [&](ValueFlowObjectID instance) {
                    if (numberOfAllocationsOfInstance.get(instance.index()) == 1)
                        created.append(instance);
                });
                bool isListed = false;
                auto add = [&](PropertyKeyID key) {
                    if (!key)
                        return;
                    for (ValueFlowObjectID createdObject : created)
                        createdInStretch.underKeys.append({ key, createdObject });
                    if (!std::exchange(isListed, true))
                        createdInStretch.objects.appendVector(created);
                };
                for (uint32_t field : allocation.fields())
                    add(program.at(allocation.layout()).fields[field].key);
                for (PropertyKeyID key : keysDefinedRightAfterAllocation(program, function, site->value))
                    add(key);
                if (function != functionOfLiterals) {
                    functionOfLiterals = function;
                    keysOfLiterals = keysDefinedBehindAllocationsOfLiterals(program.at(function));
                }
                if (auto literal = keysOfLiterals.find(site->value.index()); literal != keysOfLiterals.end()) {
                    for (PropertyKeyID key : literal->value)
                        add(key);
                }
                addStores(function, site->value, created.span());
            }
            createdInStretches[stretch] = WTF::move(createdInStretch);
        });
        auto layOut = [&](Vector<ObjectCreatedWithKey> ObjectsCreatedWithFields::* list, Vector<uint32_t>& endOfKey, Vector<ValueFlowObjectID>& objects) {
            endOfKey.fill(0u, numberOfKeys);
            for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches) {
                for (const ObjectCreatedWithKey& created : createdInStretch.*list)
                    ++endOfKey[created.key.index()];
            }
            objects.grow(layOutByKey(endOfKey));
            for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches) {
                for (const ObjectCreatedWithKey& created : createdInStretch.*list)
                    objects[endOfKey[created.key.index()]++] = created.object;
            }
        };
        layOut(&ObjectsCreatedWithFields::underKeys, m_endOfObjectsCreatedWithKey, m_objectsCreatedWithKeys);
        layOut(&ObjectsCreatedWithFields::storedToUnderKeys, m_endOfObjectsStoredToUnderKey, m_objectsStoredToUnderKeys);
        for (const ObjectsCreatedWithFields& createdInStretch : createdInStretches)
            objectsCreatedWithFields.appendVector(createdInStretch.objects);
    };
    auto uniteObjectsDeclaringFields = [&] {
        ValueSetMaker maker(allSets);
        Vector<std::pair<PropertyKeyID, ValueSetID>> declaredFields;
        Vector<uint32_t> endOfObjectsDeclaringField;
        endOfObjectsDeclaringField.fill(0u, numberOfKeys);
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            ValueSetID objects = maker.intersect(universe.objectsWith(LayoutID(index)), ValueSets::complement(universe.objectsThatMayBeAnything()));
            if (objects == ValueSets::nothing)
                continue;
            for (const Field& field : program.layouts[index].fields) {
                if (!field.key)
                    continue;
                declaredFields.append({ field.key, objects });
                ++endOfObjectsDeclaringField[field.key.index()];
            }
        }
        Vector<ValueSetID> objectsDeclaringFields(layOutByKey(endOfObjectsDeclaringField));
        for (auto [key, objects] : declaredFields)
            objectsDeclaringFields[endOfObjectsDeclaringField[key.index()]++] = objects;
        m_objectsDeclaringField = Vector<ValueSetID>(numberOfKeys, [&](size_t key) {
            return maker.uniteAll(entriesOfKey(objectsDeclaringFields, endOfObjectsDeclaringField, key));
        });
    };
    runTasksInParallel(loops, {
        [&] {
            runTasksInParallel(loops, { describeMembersOfTable, listObjectsCreatedWithKeys });
            m_holdersThatAnswerUnderSomeKey = maker.unite(m_holdersWithMembers, maker.of(objectsCreatedWithFields.span()));
        },
        uniteObjectsDeclaringFields,
        [&] {
            auto hasHeritage = [&](LayoutID layout) {
                return layout && program.at(layout).has(LayoutFlags::HasHeritage);
            };
            auto noteLayout = [&](ValueFlowObjectID object) {
                LayoutID layout = universe.layoutOf(object);
                LayoutKind kind = layout ? program.at(layout).kind : LayoutKind::ModuleNamespace;
                if (kind == LayoutKind::Record || kind == LayoutKind::Instance || kind == LayoutKind::Container)
                    objectsHoldingOnlyWhatWasWritten.append(object);
            };
            // An object that no case lists among those that hold what the solve shows is left to its class.
            maker.sets().forEachMember(knownObjectsOfProgram, [&](ValueFlowObjectID object) {
                switch (universe.kindOf(object)) {
                case ValueFlowObjectKind::RestArray:
                    arraysThatOperationsAllocate.append(object);
                    return;
                case ValueFlowObjectKind::FunctionPrototype:
                case ValueFlowObjectKind::ConstructedObject:
                case ValueFlowObjectKind::Prototype:
                    ordinaryObjects.append(object);
                    return;
                case ValueFlowObjectKind::Instance:
                    noteLayout(object);
                    // Below another class the constructor at the top makes the object, and that may be a built-in.
                    if (!hasHeritage(universe.layoutOf(object)))
                        ordinaryObjects.append(object);
                    return;
                case ValueFlowObjectKind::Site: {
                    noteLayout(object);
                    const ValueFlowSite& site = *universe.siteOf(object);
                    switch (site.origin) {
                    case ValueFlowSiteOrigin::BuiltinResult:
                        return;
                    case ValueFlowSiteOrigin::Operation:
                    case ValueFlowSiteOrigin::DataNode:
                    case ValueFlowSiteOrigin::TypedResult:
                        break;
                    }
                    if (hasHeritage(site.layout))
                        return;
                    if (!site.builtinClass || site.builtinClass == BuiltinClasses::Object)
                        ordinaryObjects.append(object);
                    else if (site.builtinClass == BuiltinClasses::Array)
                        arraysThatOperationsAllocate.append(object);
                    else if (site.builtinClass == BuiltinClasses::Arguments)
                        argumentsObjects.append(object);
                    return;
                }
                case ValueFlowObjectKind::CoroutineResult:
                case ValueFlowObjectKind::ResultOfBuiltin:
                    return;
                case ValueFlowObjectKind::Closure:
                case ValueFlowObjectKind::Namespace:
                    // universe.closures() and universe.namespaces() hold them. The loader makes a namespace object and lists its names, and it takes no other.
                    return;
                case ValueFlowObjectKind::Primitive:
                case ValueFlowObjectKind::UnknownObject:
                case ValueFlowObjectKind::Builtin:
                case ValueFlowObjectKind::ModuleBuiltin:
                case ValueFlowObjectKind::Key:
                    // None is an object of the program.
                    return;
                }
            });
        },
        [&] {
            m_symbolsUsedOnlyAsKeys = findLibrarySymbolsUsedOnlyAsKeys(program);
        },
        [&] {
            m_functionsNamedAtOneOperation = findFunctionsNamedAtOneOperation(program, loops);
        },
        [&] {
            m_keysStoredUnderOnGlobalObject = findKeysStoredUnderOnGlobalObject(program, loops);
        },
        [&] {
            m_constantsHoldingModules = ConstantsHoldingModules(program);
        },
        [&] {
            maker.sets().forEachMember(universe.arrays(), [&](ValueFlowObjectID object) {
                const ValueFlowSite* site = universe.siteOf(object);
                if (site && site->origin == ValueFlowSiteOrigin::Operation && program.at(universe.functionOf(object)).at(site->value).opcode == Opcode::NewTemplateObject)
                    templateObjects.append(object);
            });
        },
        [&] {
            for (uint32_t index = 0; index < program.functions.size(); ++index) {
                const Function& function = program.functions[index];
                if (hasPrototypeProperty(program, function) || (function.constructedClass && isInRuntimeModule(program, function)))
                    closuresWithPrototypeProperty.append(universe.closureOf(FunctionID(index)));
            }
        },
        [&] {
            runBesideTables();
        },
    });
    m_closuresWithPrototypeProperty = maker.of(closuresWithPrototypeProperty.span());
    m_templateObjects = maker.of(templateObjects.span());
    m_argumentsObjects = maker.of(argumentsObjects.span());
    m_arraysThatOperationsAllocate = maker.of(arraysThatOperationsAllocate.span());
    m_objectsHoldingOnlyWhatWasWritten = maker.of(objectsHoldingOnlyWhatWasWritten.span());
    m_objectsWithoutPropertiesOfNativeCode = maker.unite(maker.unite(maker.of(ordinaryObjects.span()), universe.closures()), maker.unite(m_argumentsObjects, m_arraysThatOperationsAllocate));
    m_objectsThatTheirClassAnswersFor = without(knownObjectsOfProgram, maker.unite(m_objectsWithoutPropertiesOfNativeCode, universe.namespaces()));
    m_objectsMadeWithPropertiesListedAboveThem = without(universe.knownObjects(), maker.unite(maker.of(ordinaryObjects.span()), m_prototypeObjects));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
