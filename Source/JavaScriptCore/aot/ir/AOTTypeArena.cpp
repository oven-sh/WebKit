/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeArena.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringArena.h"
#include <algorithm>
#include <concepts>
#include <wtf/Locker.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(TypeTable::maxStorages == Limits::maxWorkers);
static_assert(TypeTable::provisionalBit >> TypeTable::storageShift == TypeTable::maxStorages + 1);

template<typename T>
static void rewriteValues(SortedSet<T>& set, NOESCAPE const Invocable<T(T)> auto& rewrite)
{
    Vector<T, 16> rewritten(set.size(), [&](size_t index) {
        return rewrite(set[index]);
    });
    if (!std::ranges::equal(rewritten.span(), set.span()))
        set = SortedSet<T>(rewritten.span());
}

struct TypeInterningPhase::Merge {
    Merge(TypeInterningPhase& phase, const FunctionRemap* functions, const StringRemap& strings)
        : phase(phase)
        , table(*phase.m_committedTable)
        , functions(functions)
        , strings(strings)
    {
        allocateFinalIDs<PropertyKey>(remap.m_propertyKeys);
        allocateFinalIDs<LayoutSet>(remap.m_layoutSets);
        allocateFinalIDs<CalleeSet>(remap.m_calleeSets);
        allocateFinalIDs<Type>(remap.m_types);
        remap.m_tasksWithNewRecords.ensureSize(phase.m_firstUses.size());
    }

    template<typename Record>
    static auto& internedRecordsOf(auto& table)
    {
        if constexpr (std::same_as<Record, Type>)
            return table.m_types;
        else if constexpr (std::same_as<Record, LayoutSet>)
            return table.m_layoutSets;
        else if constexpr (std::same_as<Record, CalleeSet>)
            return table.m_calleeSets;
        else {
            static_assert(std::same_as<Record, PropertyKey>);
            return table.m_propertyKeys;
        }
    }

    void commitTask(unsigned task)
    {
        const FirstUses* firstUses = phase.m_firstUses[task].get();
        if (!firstUses)
            return;
        remap.m_tasksWithNewRecords.quickSet(task);
        for (PropertyKeyID key : firstUses->propertyKeys)
            commit(key);
        for (LayoutSetID set : firstUses->layoutSets)
            commit(set);
        for (CalleeSetID set : firstUses->calleeSets)
            commit(set);
        for (TypeID type : firstUses->types)
            commit(type);
    }

    PropertyKeyID commit(PropertyKeyID id) { return commitRecord<PropertyKey>(remap.m_propertyKeys, id); }
    LayoutSetID commit(LayoutSetID id) { return commitRecord<LayoutSet>(remap.m_layoutSets, id); }
    CalleeSetID commit(CalleeSetID id) { return commitRecord<CalleeSet>(remap.m_calleeSets, id); }
    TypeID commit(TypeID id) { return commitRecord<Type>(remap.m_types, id); }

    template<typename Record, typename IDType>
    void allocateFinalIDs(TypeRemap::FinalIDs<IDType>& finalIDs)
    {
        finalIDs.byStorage = Vector<Vector<IDType>>(TypeTable::maxStorages, [&](size_t number) {
            const Storage* storage = phase.m_storages.at(number);
            return Vector<IDType>(FillWith { }, storage ? storage->recordsOf<Record>().records.size() : 0, IDType());
        });
    }

    template<typename Record, typename IDType>
    IDType commitRecord(TypeRemap::FinalIDs<IDType>& finalIDs, IDType id)
    {
        if (!TypeTable::isProvisional(id))
            return id;
        uint32_t number = TypeTable::storageNumberOf(id);
        uint32_t position = TypeTable::positionOf(id);
        IDType& finalID = finalIDs.byStorage[number][position];
        if (finalID)
            return finalID;
        Record record = WTF::move(phase.m_storages.at(number)->template recordsOf<Record>().records[position]);
        rewriteIDs(record);
        uint32_t hash = computeHash(record);
        finalID = internedRecordsOf<Record>(table).findOrAdd(WTF::move(record), hash);
        RELEASE_ASSERT(!TypeTable::isProvisional(finalID));
        return finalID;
    }

    FunctionID finalFunctionOf(FunctionID function) const
    {
        return functions ? (*functions)(function) : function;
    }

    template<typename Record>
    void rewriteReferences(Record& record)
    {
        forEachReference(record, [&]<typename Reference>(Reference& reference) {
            if constexpr (std::same_as<Reference, StringID>)
                strings.apply(reference);
            else if constexpr (std::same_as<Reference, FunctionID>)
                reference = finalFunctionOf(reference);
        });
    }

    void rewriteIDs(PropertyKey& key) { rewriteReferences(key); }
    void rewriteIDs(LayoutSet& set) { rewriteReferences(set); }

    void rewriteIDs(CalleeSet& set)
    {
        rewriteReferences(set);
        canonicalize(set);
    }

    void rewriteIDs(Type& type)
    {
        rewriteValues(type.stringLiterals, [&](StringID literal) {
            return strings(literal);
        });
        rewriteValues(type.regExpLiteralSet, [&](RegExpLiteralInType literal) {
            return RegExpLiteralInType { strings(literal.pattern), literal.rawFlags };
        });
        rewriteValues(type.symbolLiterals, [&](PropertyKeyID key) {
            return commit(key);
        });
        rewriteValues(type.functions, [&](FunctionID function) {
            return finalFunctionOf(function);
        });
        type.layouts = commit(type.layouts);
        for (ClassPart& part : type.classes) {
            for (TypeID& contents : part.contents)
                contents = commit(contents);
            for (TypeID& contents : part.predictedContents)
                contents = commit(contents);
        }
        type.canonicalize();
    }

    TypeInterningPhase& phase;
    TypeTable& table;
    const FunctionRemap* functions;
    const StringRemap& strings;
    TypeRemap remap;
};

template<typename Record, typename IDType>
const Record& TypeInterningPhase::recordAt(const TypeInterningPhase* phase, IDType id)
{
    uint32_t number = TypeTable::storageNumberOf(id);
    RELEASE_ASSERT(phase && number < TypeTable::maxStorages);
    const Storage* storage = phase->m_storages.at(number);
    uint32_t position = TypeTable::positionOf(id);
    RELEASE_ASSERT(storage && position < storage->recordsOf<Record>().records.size());
    return storage->recordsOf<Record>().records[position];
}

const Type& TypeTable::provisionalAt(TypeID id) const
{
    return TypeInterningPhase::recordAt<Type>(phaseOfProvisionalIDs(), id);
}

const LayoutSet& TypeTable::provisionalAt(LayoutSetID id) const
{
    return TypeInterningPhase::recordAt<LayoutSet>(phaseOfProvisionalIDs(), id);
}

const CalleeSet& TypeTable::provisionalAt(CalleeSetID id) const
{
    return TypeInterningPhase::recordAt<CalleeSet>(phaseOfProvisionalIDs(), id);
}

const PropertyKey& TypeTable::provisionalAt(PropertyKeyID id) const
{
    return TypeInterningPhase::recordAt<PropertyKey>(phaseOfProvisionalIDs(), id);
}

TypeInterningPhase::TypeInterningPhase(TypeTable& table, unsigned numberOfTasks)
    : TypeInterningPhase(table, numberOfTasks, Scratch)
{
    m_committedTable = &table;
}

TypeInterningPhase::TypeInterningPhase(const TypeTable& table, unsigned numberOfTasks, ScratchTag)
    : m_table(table)
{
    if (numberOfTasks)
        m_firstUses.grow(numberOfTasks);
    RELEASE_ASSERT(!table.m_openPhase);
    table.destroyPhaseOfScratchArenas();
    table.m_openPhase = this;
}

TypeInterningPhase::TypeInterningPhase(const TypeTable& table, OwnedByTableTag)
    : m_table(table)
    , m_isOpenPhaseOfTable(false)
{
}

TypeInterningPhase::~TypeInterningPhase()
{
    RELEASE_ASSERT(!m_isOpen);
}

unsigned TypeInterningPhase::addTasks(unsigned count)
{
    Locker locker { m_firstUsesGrowthLock };
    RELEASE_ASSERT(m_isOpen);
    unsigned first = m_firstUses.size();
    if (count)
        m_firstUses.grow(first + count);
    return first;
}

auto TypeInterningPhase::leaseStorage() -> Storage&
{
    RELEASE_ASSERT(m_isOpen);
    Storage& storage = m_storages.lease();
    ++storage.lease;
    RELEASE_ASSERT(storage.lease);
    return storage;
}

auto TypeInterningPhase::createFirstUses(unsigned task) -> FirstUses&
{
    RELEASE_ASSERT(m_isOpen && task < m_firstUses.size());
    std::unique_ptr<FirstUses>& firstUses = m_firstUses[task];
    RELEASE_ASSERT(!firstUses);
    firstUses = makeUnique<FirstUses>();
    return *firstUses;
}

void TypeInterningPhase::returnStorages(std::span<Storage* const> storages)
{
    for (Storage* storage : storages)
        m_storages.giveBack(*storage);
}

void TypeInterningPhase::close()
{
    RELEASE_ASSERT(m_isOpen && !m_storages.hasLeases());
    m_isOpen = false;
    if (m_isOpenPhaseOfTable)
        m_table.m_openPhase = nullptr;
}

TypeTable::~TypeTable()
{
    destroyPhaseOfScratchArenas();
}

const TypeInterningPhase* TypeTable::phaseOfProvisionalIDs() const
{
    return m_openPhase ? m_openPhase : m_phaseOfScratchArenas.load(std::memory_order_acquire);
}

// Many loops ask questions about functions on all threads while no phase is open, and far below some question needs a type that the table may not have.
TypeInterningPhase& TypeTable::attachScratchArena() const
{
    if (m_openPhase)
        return *m_openPhase;
    m_numberOfArenasInPhaseOfScratchArenas.fetch_add(1, std::memory_order_relaxed);
    if (TypeInterningPhase* phase = m_phaseOfScratchArenas.load(std::memory_order_acquire))
        return *phase;
    Locker locker { m_phaseOfScratchArenasLock };
    TypeInterningPhase* phase = m_phaseOfScratchArenas.load(std::memory_order_relaxed);
    if (!phase) {
        phase = new TypeInterningPhase(*this, TypeInterningPhase::OwnedByTable);
        m_phaseOfScratchArenas.store(phase, std::memory_order_release);
    }
    return *phase;
}

void TypeTable::detachScratchArena(const TypeInterningPhase& phase) const
{
    if (!phase.m_isOpenPhaseOfTable)
        m_numberOfArenasInPhaseOfScratchArenas.fetch_sub(1, std::memory_order_relaxed);
}

void TypeTable::destroyPhaseOfScratchArenas() const
{
    RELEASE_ASSERT(!m_numberOfArenasInPhaseOfScratchArenas.load(std::memory_order_relaxed));
    std::unique_ptr<TypeInterningPhase> phase(m_phaseOfScratchArenas.exchange(nullptr, std::memory_order_acquire));
    if (phase)
        phase->discard();
}

TypeRemap TypeInterningPhase::finish(const StringRemap& strings)
{
    Vector<unsigned> taskOrder(m_firstUses.size(), [](size_t task) {
        return static_cast<unsigned>(task);
    });
    return merge(taskOrder.span(), nullptr, strings);
}

TypeRemap TypeInterningPhase::finish(std::span<const unsigned> taskOrder, const FunctionRemap& functions, const StringRemap& strings)
{
    BitVector listedTasks;
    listedTasks.ensureSize(m_firstUses.size());
    for (unsigned task : taskOrder) {
        RELEASE_ASSERT(task < m_firstUses.size());
        listedTasks.quickSet(task);
    }
    for (unsigned task = 0; task < m_firstUses.size(); ++task)
        RELEASE_ASSERT(!m_firstUses[task] || listedTasks.quickGet(task));
    return merge(taskOrder, &functions, strings);
}

void TypeInterningPhase::discard()
{
    close();
}

TypeRemap TypeInterningPhase::merge(std::span<const unsigned> taskOrder, const FunctionRemap* functions, const StringRemap& strings)
{
    RELEASE_ASSERT(m_committedTable);
    close();
    unsigned initialTableSize = m_committedTable->size();

    Merge merge(*this, functions, strings);
    for (unsigned task : taskOrder)
        merge.commitTask(task);

    m_committedTable->m_numberOfProvisionalTypes += m_committedTable->size() - initialTableSize;
    return WTF::move(merge.remap);
}

TypeArena::TypeArena(TypeTable& table)
    : m_table(table)
    , m_serialTable(&table)
{
}

TypeArena::TypeArena(TypeInterningPhase& phase, unsigned task)
    : TypeArena(phase, TypeInterningPhase::Scratch)
{
    m_task = task;
}

TypeArena::TypeArena(TypeInterningPhase& phase, TypeInterningPhase::ScratchTag)
    : m_table(phase.m_table)
    , m_phase(&phase)
{
}

TypeArena::~TypeArena()
{
    if (m_phase)
        m_phase->returnStorages(m_storages.span());
}

template<typename Record, typename RecordID>
RecordID TypeArena::didUse(TypeInterningPhase::Storage& storage, uint32_t position)
{
    RecordID id = TypeTable::provisionalID<RecordID>(storage.number, position);
    uint32_t& lastLease = storage.recordsOf<Record>().lastLease[position];
    if (lastLease != storage.lease) {
        lastLease = storage.lease;
        if (m_task) {
            if (!m_firstUses)
                m_firstUses = &m_phase->createFirstUses(*m_task);
            m_firstUses->listOf<RecordID>().append(id);
        }
    }
    return id;
}

template<typename Record, typename RecordID>
std::optional<RecordID> TypeArena::useExistingIn(TypeInterningPhase::Storage& storage, const Record& record, uint32_t hash)
{
    auto& provisionalRecords = storage.recordsOf<Record>();
    auto position = provisionalRecords.index.find(hash, [&](uint32_t candidate) {
        return provisionalRecords.records[candidate] == record;
    });
    if (!position)
        return std::nullopt;
    return didUse<Record, RecordID>(storage, *position);
}

template<typename Record, typename RecordID>
RecordID TypeArena::internCanonical(Record&& record)
{
    uint32_t hash = computeHash(record);
    if (auto finalID = TypeInterningPhase::Merge::internedRecordsOf<Record>(m_table).find(record, hash))
        return *finalID;
    for (TypeInterningPhase::Storage* storage : m_storages) {
        if (auto provisionalID = useExistingIn<Record, RecordID>(*storage, record, hash))
            return *provisionalID;
    }

    while (m_storages.isEmpty() || m_storages.last()->recordsOf<Record>().records.size() == TypeTable::maxRecordsPerStorage) {
        m_storages.append(&m_phase->leaseStorage());
        if (auto provisionalID = useExistingIn<Record, RecordID>(*m_storages.last(), record, hash))
            return *provisionalID;
    }

    TypeInterningPhase::Storage& storage = *m_storages.last();
    auto& provisionalRecords = storage.recordsOf<Record>();
    uint32_t position = provisionalRecords.records.size();
    provisionalRecords.records.appendConcurrently(WTF::move(record));
    provisionalRecords.index.add(hash, position);
    provisionalRecords.lastLease.append(0);
    return didUse<Record, RecordID>(storage, position);
}

TypeID TypeArena::intern(const Type& type)
{
    if (m_serialTable)
        return m_serialTable->intern(type);

    Type canonical = type;
    canonical.canonicalize();
    ASSERT(!canonical.layouts || at(canonical.layouts).isComplete);
    return internCanonical<Type, TypeID>(WTF::move(canonical));
}

LayoutSetID TypeArena::intern(LayoutSet&& set)
{
    if (m_serialTable)
        return m_serialTable->intern(WTF::move(set));

    canonicalize(set);
    return internCanonical<LayoutSet, LayoutSetID>(WTF::move(set));
}

CalleeSetID TypeArena::intern(CalleeSet&& set)
{
    if (m_serialTable)
        return m_serialTable->intern(WTF::move(set));

    canonicalize(set);
    return internCanonical<CalleeSet, CalleeSetID>(WTF::move(set));
}

PropertyKeyID TypeArena::intern(const PropertyKey& key)
{
    if (m_serialTable)
        return m_serialTable->intern(key);

    return internCanonical<PropertyKey, PropertyKeyID>(PropertyKey(key));
}

LayoutSetID TypeArena::completeLayoutSet(std::span<const LayoutID> unsortedMembers)
{
    return intern(LayoutSet { Vector<LayoutID>(unsortedMembers), true });
}

PropertyKeyID TypeArena::textKey(StringID text)
{
    return intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
}

PropertyKeyID TypeArena::wellKnownSymbolKey(WellKnownSymbol symbol)
{
    return intern(PropertyKey { .kind = PropertyKeyKind::WellKnownSymbol, .symbol = symbol });
}

PropertyKeyID TypeArena::registeredSymbolKey(StringID description)
{
    return intern(PropertyKey { .kind = PropertyKeyKind::RegisteredSymbol, .text = description });
}

TypeID TypeArena::unionOf(TypeID a, TypeID b)
{
    if (a == b || b == TypeTable::never)
        return a;
    if (a == TypeTable::never)
        return b;
    return intern(at(a).unionWith(at(b), *this));
}

TypeID TypeArena::intersectionOf(TypeID a, TypeID b)
{
    if (a == b)
        return a;
    return intern(at(a).intersectWith(at(b), *this));
}

TypeID TypeArena::differenceOf(TypeID a, TypeID b)
{
    if (b == TypeTable::never)
        return a;
    return intern(at(a).without(at(b), *this));
}

TypeID TypeArena::withPrimitives(TypeID id, uint16_t added)
{
    return intern(at(id).withPrimitives(added));
}

TypeID TypeArena::withoutPrimitives(TypeID id, uint16_t removed)
{
    return intern(at(id).withoutPrimitives(removed));
}

ScratchTypeArena::Attachment::Attachment(const TypeTable& table)
    : table(table)
    , phase(table.attachScratchArena())
{
}

ScratchTypeArena::Attachment::~Attachment()
{
    table.detachScratchArena(phase);
}

ScratchTypeArena::ScratchTypeArena(const TypeTable& table)
    : m_attachment(table)
    , m_arena(m_attachment.phase, TypeInterningPhase::Scratch)
{
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
