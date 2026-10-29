/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeArena.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTOrderedInterning.h"
#include "AOTProvisionalFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <concepts>
#include <utility>
#include <wtf/Locker.h>
#include <wtf/MallocSpan.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(TypeTable::maxStorages == Limits::maxWorkers);
static_assert(TypeTable::provisionalBit >> TypeTable::storageShift == TypeTable::maxStorages + 1);

template<typename T>
static bool rewriteValues(SortedSet<T>& set, NOESCAPE const Invocable<T(T)> auto& rewrite)
{
    if (set.isEmpty())
        return false;
    Vector<T, 16> rewritten(set.size(), [&](size_t index) {
        return rewrite(set[index]);
    });
    if (std::ranges::equal(rewritten.span(), set.span()))
        return false;
    set = SortedSet<T>(rewritten.span());
    return true;
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
        remap.m_isIdentity = false;
        for (PropertyKeyID key : firstUses->propertyKeys)
            commit(key);
        for (LayoutSetID set : firstUses->layoutSets)
            commit(set);
        for (CalleeSetID set : firstUses->calleeSets)
            commit(set);
        constexpr size_t recordsAhead = 6;
        std::span<const TypeID> types = firstUses->types.span();
        for (size_t index = 0; index < types.size(); ++index) {
            if (index + recordsAhead < types.size())
                askFor(types[index + recordsAhead]);
            commit(types[index]);
        }
    }

    void askFor(TypeID id)
    {
        uint32_t number = TypeTable::storageNumberOf(id);
        uint32_t position = TypeTable::positionOf(id);
        if (remap.m_types.byStorage[number][position])
            return;
        const auto& provisionalTypes = phase.m_storages.at(number)->types;
        const auto* record = reinterpret_cast<const uint8_t*>(&provisionalTypes.records[position]);
        for (size_t offset = 0; offset < sizeof(Type); offset += 64)
            __builtin_prefetch(record + offset);
        table.m_types.prefetch(provisionalTypes.hashes[position]);
    }

    PropertyKeyID commit(PropertyKeyID id) { return commitRecord<PropertyKey>(remap.m_propertyKeys, id); }
    LayoutSetID commit(LayoutSetID id) { return commitRecord<LayoutSet>(remap.m_layoutSets, id); }
    CalleeSetID commit(CalleeSetID id) { return commitRecord<CalleeSet>(remap.m_calleeSets, id); }
    TypeID commit(TypeID id) { return commitRecord<Type>(remap.m_types, id); }

    template<typename Record, typename IDType>
    void allocateFinalIDs(TypeRemap::FinalIDs<IDType>& finalIDs)
    {
        if (phase.m_storages.isEmpty())
            return;
        finalIDs.byStorage = Vector<Vector<IDType>>(TypeTable::maxStorages, [&](size_t number) {
            const Storage* storage = phase.m_storages.at(number);
            size_t size = storage ? storage->recordsOf<Record>().records.size() : 0;
            numberOfProvisionalRecords += size;
            return Vector<IDType>(FillWith { }, size, IDType());
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
        auto& provisionalRecords = phase.m_storages.at(number)->template recordsOf<Record>();
        Record record = WTF::move(provisionalRecords.records[position]);
        uint32_t hash = rewriteIDs(record) ? computeHash(record) : provisionalRecords.hashes[position];
        finalID = internedRecordsOf<Record>(table).findOrAdd(WTF::move(record), hash);
        RELEASE_ASSERT(!TypeTable::isProvisional(finalID));
        return finalID;
    }

    FunctionID finalFunctionOf(FunctionID function) const
    {
        return functions ? (*functions)(function) : function;
    }

    template<typename Record>
    bool rewriteReferences(Record& record)
    {
        bool hasChanged = false;
        forEachReference(record, [&]<typename Reference>(Reference& reference) {
            if constexpr (std::same_as<Reference, StringID> || std::same_as<Reference, FunctionID>) {
                Reference previous = reference;
                if constexpr (std::same_as<Reference, StringID>)
                    strings.apply(reference);
                else
                    reference = finalFunctionOf(reference);
                hasChanged |= reference != previous;
            }
        });
        return hasChanged;
    }

    bool rewriteIDs(PropertyKey& key) { return rewriteReferences(key); }
    bool rewriteIDs(LayoutSet& set) { return rewriteReferences(set); }

    bool rewriteIDs(CalleeSet& set)
    {
        if (!rewriteReferences(set))
            return false;
        canonicalize(set);
        return true;
    }

    bool rewriteIDs(Type& type)
    {
        bool hasChanged = rewriteValues(type.stringLiterals, [&](StringID literal) {
            return strings(literal);
        });
        hasChanged |= rewriteValues(type.regExpLiteralSet, [&](RegExpLiteralInType literal) {
            return RegExpLiteralInType { strings(literal.pattern), literal.rawFlags };
        });
        hasChanged |= rewriteValues(type.symbolLiterals, [&](PropertyKeyID key) {
            return commit(key);
        });
        hasChanged |= rewriteValues(type.functions, [&](FunctionID function) {
            return finalFunctionOf(function);
        });
        auto commitInPlace = [&](auto& id) {
            auto previous = id;
            id = commit(id);
            hasChanged |= id != previous;
        };
        commitInPlace(type.layouts);
        for (ClassPart& part : type.classes) {
            for (TypeID& contents : part.writtenContents)
                commitInPlace(contents);
        }
        if (hasChanged)
            type.canonicalize();
        return hasChanged;
    }

    TypeInterningPhase& phase;
    TypeTable& table;
    const FunctionRemap* functions;
    const StringRemap& strings;
    TypeRemap remap;
    size_t numberOfProvisionalRecords { 0 };
};

struct TypeInterningPhase::ParallelMerge {
    static constexpr uint32_t notCommitted = UINT32_MAX;
    static constexpr size_t recordsPerTask = 256;

    template<typename IDType>
    struct CommitOrder {
        uint32_t& positionOf(IDType id) { return positions[firstInStorage[TypeTable::storageNumberOf(id)] + TypeTable::positionOf(id)]; }

        Vector<uint32_t> firstInStorage;
        Vector<uint32_t> positions;
        Vector<IDType> ids;
        Vector<IDType> finalIDs;
    };

    ParallelMerge(Merge& merge, const ParallelLoops& loops)
        : merge(merge)
        , phase(merge.phase)
        , loops(loops)
    {
        initialize<PropertyKey>(propertyKeys);
        initialize<LayoutSet>(layoutSets);
        initialize<CalleeSet>(calleeSets);
        initialize<Type>(types);
    }

    template<typename Record, typename IDType>
    void initialize(CommitOrder<IDType>& order)
    {
        uint32_t size = 0;
        order.firstInStorage = Vector<uint32_t>(TypeTable::maxStorages, [&](size_t number) {
            const Storage* storage = phase.m_storages.at(number);
            return std::exchange(size, size + (storage ? static_cast<uint32_t>(storage->recordsOf<Record>().records.size()) : 0));
        });
        order.positions.grow(size);
        forEachInParallel(size, [&](size_t index) {
            order.positions[index] = notCommitted;
        });
    }

    template<typename Record, typename IDType>
    Record& recordOf(IDType id)
    {
        return phase.m_storages.at(TypeTable::storageNumberOf(id))->template recordsOf<Record>().records[TypeTable::positionOf(id)];
    }

    template<typename Record, typename IDType>
    uint32_t hashOf(IDType id)
    {
        return phase.m_storages.at(TypeTable::storageNumberOf(id))->template recordsOf<Record>().hashes[TypeTable::positionOf(id)];
    }

    void forEachInParallel(size_t count, NOESCAPE const Invocable<void(size_t)> auto& functor)
    {
        forEachIndexInParallel(loops, static_cast<unsigned>((count + recordsPerTask - 1) / recordsPerTask), [&](unsigned task) {
            for (size_t index = task * recordsPerTask, end = std::min(count, index + recordsPerTask); index < end; ++index)
                functor(index);
        });
    }

    static bool refersToProvisionalRecords(const Type& type)
    {
        if (TypeTable::isProvisional(type.layouts))
            return true;
        for (PropertyKeyID key : type.symbolLiterals) {
            if (TypeTable::isProvisional(key))
                return true;
        }
        for (const ClassPart& part : type.classes) {
            for (TypeID contents : part.writtenContents) {
                if (TypeTable::isProvisional(contents))
                    return true;
            }
        }
        return false;
    }

    void findTypesThatReferToProvisionalRecords()
    {
        refersToOthers.grow(types.positions.size());
        Vector<uint32_t> storages;
        for (uint32_t number = 0; number < TypeTable::maxStorages; ++number) {
            if (phase.m_storages.at(number))
                storages.append(number);
        }
        forEachIndexInParallel(loops, static_cast<unsigned>(storages.size()), [&](unsigned index) {
            uint32_t number = storages[index];
            const auto& records = phase.m_storages.at(number)->types.records;
            for (uint32_t position = 0; position < records.size(); ++position)
                refersToOthers[types.firstInStorage[number] + position] = refersToProvisionalRecords(records[position]);
        });
    }

    template<typename IDType>
    void visit(CommitOrder<IDType>& order, IDType id)
    {
        if (!TypeTable::isProvisional(id))
            return;
        uint32_t& position = order.positionOf(id);
        if (position != notCommitted)
            return;
        position = static_cast<uint32_t>(order.ids.size());
        order.ids.append(id);
    }

    uint8_t visitType(TypeID id)
    {
        if (!TypeTable::isProvisional(id))
            return 0;
        uint32_t index = types.firstInStorage[TypeTable::storageNumberOf(id)] + TypeTable::positionOf(id);
        if (types.positions[index] != notCommitted)
            return nestingDepths[types.positions[index]] + 1;
        unsigned depth = 0;
        if (refersToOthers[index]) {
            const Type& type = recordOf<Type>(id);
            for (PropertyKeyID key : type.symbolLiterals)
                visit(propertyKeys, key);
            visit(layoutSets, type.layouts);
            for (const ClassPart& part : type.classes) {
                for (TypeID contents : part.writtenContents)
                    depth = std::max<unsigned>(depth, visitType(contents));
            }
        }
        if (depth >= maxNestingDepth) {
            isNestedTooDeeply = true;
            depth = maxNestingDepth;
        }
        types.positions[index] = static_cast<uint32_t>(types.ids.size());
        types.ids.append(id);
        nestingDepths.append(static_cast<uint8_t>(depth));
        return static_cast<uint8_t>(depth + 1);
    }

    enum class Kind : uint8_t { PropertyKey, LayoutSet, CalleeSet, Type };

    bool findCommitOrder(std::span<const unsigned> taskOrder)
    {
        constexpr size_t tasksPerChunk = 256;
        constexpr uint64_t neverListed = UINT64_MAX;
        size_t numberOfChunks = (taskOrder.size() + tasksPerChunk - 1) / tasksPerChunk;
        auto forEachTaskInChunk = [&](unsigned chunk, const auto& functor) {
            for (size_t index = chunk * tasksPerChunk, end = std::min(taskOrder.size(), index + tasksPerChunk); index < end; ++index) {
                if (const FirstUses* firstUses = phase.m_firstUses[taskOrder[index]].get())
                    functor(*firstUses);
            }
        };
        Vector<uint64_t> firstPlaceOfChunk(numberOfChunks);
        forEachIndexInParallel(loops, static_cast<unsigned>(numberOfChunks), [&](unsigned chunk) {
            uint64_t numberOfPlaces = 0;
            forEachTaskInChunk(chunk, [&](const FirstUses& firstUses) {
                numberOfPlaces += firstUses.propertyKeys.size() + firstUses.layoutSets.size() + firstUses.calleeSets.size() + firstUses.types.size();
            });
            firstPlaceOfChunk[chunk] = numberOfPlaces;
        });
        uint64_t numberOfPlaces = 0;
        for (uint64_t& first : firstPlaceOfChunk)
            first = std::exchange(numberOfPlaces, numberOfPlaces + first);
        for (unsigned task : taskOrder) {
            if (!phase.m_firstUses[task])
                continue;
            merge.remap.m_tasksWithNewRecords.quickSet(task);
            merge.remap.m_isIdentity = false;
        }

        std::array<size_t, 4> firstRecordOfKind;
        size_t numberOfRecords = 0;
        for (auto [kind, size] : { std::pair { Kind::PropertyKey, propertyKeys.positions.size() }, std::pair { Kind::LayoutSet, layoutSets.positions.size() }, std::pair { Kind::CalleeSet, calleeSets.positions.size() }, std::pair { Kind::Type, types.positions.size() } })
            firstRecordOfKind[static_cast<size_t>(kind)] = std::exchange(numberOfRecords, numberOfRecords + size);
        MallocSpan<std::atomic<uint64_t>> firstPlaces = MallocSpan<std::atomic<uint64_t>>::malloc(numberOfRecords * sizeof(std::atomic<uint64_t>));
        forEachInParallel(numberOfRecords, [&](size_t record) {
            firstPlaces[record].store(neverListed, std::memory_order_relaxed);
        });
        auto note = [&](Kind kind, auto& order, auto id, uint64_t place) {
            std::atomic<uint64_t>& first = firstPlaces[firstRecordOfKind[static_cast<size_t>(kind)] + order.firstInStorage[TypeTable::storageNumberOf(id)] + TypeTable::positionOf(id)];
            uint64_t value = place << 34 | static_cast<uint64_t>(kind) << 32 | id.index();
            uint64_t previous = first.load(std::memory_order_relaxed);
            while (value < previous && !first.compare_exchange_weak(previous, value, std::memory_order_relaxed)) { }
        };
        RELEASE_ASSERT(numberOfPlaces < (uint64_t { 1 } << 30));
        forEachIndexInParallel(loops, static_cast<unsigned>(numberOfChunks), [&](unsigned chunk) {
            uint64_t place = firstPlaceOfChunk[chunk];
            forEachTaskInChunk(chunk, [&](const FirstUses& firstUses) {
                for (PropertyKeyID key : firstUses.propertyKeys)
                    note(Kind::PropertyKey, propertyKeys, key, place++);
                for (LayoutSetID set : firstUses.layoutSets)
                    note(Kind::LayoutSet, layoutSets, set, place++);
                for (CalleeSetID set : firstUses.calleeSets)
                    note(Kind::CalleeSet, calleeSets, set, place++);
                for (TypeID type : firstUses.types)
                    note(Kind::Type, types, type, place++);
            });
        });
        Vector<uint64_t> listed = collectInParallel<uint64_t>(loops, static_cast<unsigned>(numberOfRecords), [&](unsigned record, Vector<uint64_t>& found) {
            uint64_t place = firstPlaces[record].load(std::memory_order_relaxed);
            if (place != neverListed)
                found.append(place);
        });
        parallelSort(loops, listed.mutableSpan(), std::less<uint64_t> { });

        types.ids.reserveInitialCapacity(types.positions.size());
        nestingDepths.reserveInitialCapacity(types.positions.size());
        for (uint64_t place : listed) {
            uint32_t id = static_cast<uint32_t>(place);
            switch (static_cast<Kind>(place >> 32 & 3)) {
            case Kind::PropertyKey:
                visit(propertyKeys, PropertyKeyID(id));
                break;
            case Kind::LayoutSet:
                visit(layoutSets, LayoutSetID(id));
                break;
            case Kind::CalleeSet:
                visit(calleeSets, CalleeSetID(id));
                break;
            case Kind::Type:
                visitType(TypeID(id));
                break;
            }
        }
        return !isNestedTooDeeply;
    }

    template<typename IDType>
    IDType finalIDOf(CommitOrder<IDType>& order, IDType id)
    {
        return TypeTable::isProvisional(id) ? order.finalIDs[order.positionOf(id)] : id;
    }

    template<typename IDType>
    void publish(CommitOrder<IDType>& order, TypeRemap::FinalIDs<IDType>& finalIDs)
    {
        forEachInParallel(order.ids.size(), [&](size_t position) {
            IDType id = order.ids[position];
            finalIDs.byStorage[TypeTable::storageNumberOf(id)][TypeTable::positionOf(id)] = order.finalIDs[position];
        });
    }

    template<typename Record, typename IDType>
    void commitRecordsWithoutReferences(CommitOrder<IDType>& order, TypeRemap::FinalIDs<IDType>& finalIDs)
    {
        if (order.ids.isEmpty())
            return;
        ParallelArray<Record> records(loops, static_cast<unsigned>(order.ids.size()), [&](unsigned position) {
            return WTF::move(recordOf<Record>(order.ids[position]));
        });
        auto& table = Merge::internedRecordsOf<Record>(merge.table);
        OrderedInterning<Record, IDType, std::remove_reference_t<decltype(table)>> interning(table, loops, records.mutableSpan());
        forEachInParallel(records.size(), [&](size_t position) {
            interning.setHash(position, merge.rewriteIDs(records[position]) ? computeHash(records[position]) : hashOf<Record>(order.ids[position]));
        });
        interning.classifyAll();
        interning.assignIDs();
        interning.commit();
        order.finalIDs = interning.takeIDs();
        publish(order, finalIDs);
    }

    bool rewriteAllButContents(Type& type)
    {
        bool hasChanged = rewriteValues(type.stringLiterals, [&](StringID literal) {
            return merge.strings(literal);
        });
        hasChanged |= rewriteValues(type.regExpLiteralSet, [&](RegExpLiteralInType literal) {
            return RegExpLiteralInType { merge.strings(literal.pattern), literal.rawFlags };
        });
        hasChanged |= rewriteValues(type.symbolLiterals, [&](PropertyKeyID key) {
            return finalIDOf(propertyKeys, key);
        });
        hasChanged |= rewriteValues(type.functions, [&](FunctionID function) {
            return merge.finalFunctionOf(function);
        });
        LayoutSetID layouts = finalIDOf(layoutSets, type.layouts);
        hasChanged |= layouts != type.layouts;
        type.layouts = layouts;
        return hasChanged;
    }

    void commitTypes()
    {
        if (types.ids.isEmpty())
            return;
        size_t count = types.ids.size();
        ParallelArray<Type> records(loops, static_cast<unsigned>(count), [&](unsigned position) {
            return WTF::move(recordOf<Type>(types.ids[position]));
        });
        Vector<Vector<uint32_t>> positionsByDepth(std::ranges::max(nestingDepths) + 1, [&](size_t depth) {
            return findIndicesInParallel(loops, static_cast<unsigned>(count), [&](unsigned position) {
                return nestingDepths[position] == depth;
            });
        });

        auto& table = Merge::internedRecordsOf<Type>(merge.table);
        OrderedInterning<Type, TypeID, std::remove_reference_t<decltype(table)>> interning(table, loops, records.mutableSpan());
        for (const Vector<uint32_t>& positions : positionsByDepth) {
            forEachInParallel(positions.size(), [&](size_t index) {
                uint32_t position = positions[index];
                Type& type = records[position];
                bool hasChanged = rewriteAllButContents(type);
                if (nestingDepths[position]) {
                    for (ClassPart& part : type.classes) {
                        for (TypeID& contents : part.writtenContents) {
                            if (!TypeTable::isProvisional(contents))
                                continue;
                            uint32_t earlier = types.positionOf(contents);
                            TypeID existing = interning.existingID(earlier);
                            contents = existing ? existing : TypeID(TypeTable::provisionalBit | interning.equivalenceClass(earlier));
                            hasChanged = true;
                        }
                    }
                }
                if (hasChanged)
                    type.canonicalize();
                interning.setHash(position, hasChanged ? computeHash(type) : hashOf<Type>(types.ids[position]));
            });
            interning.classify(positions.span());
        }
        interning.assignIDs();
        forEachInParallel(count, [&](size_t position) {
            if (!nestingDepths[position] || !interning.isNew(position))
                return;
            bool hasChanged = false;
            for (ClassPart& part : records[position].classes) {
                for (TypeID& contents : part.writtenContents) {
                    if (!TypeTable::isProvisional(contents))
                        continue;
                    contents = interning.idAt(contents.index() & ~TypeTable::provisionalBit);
                    hasChanged = true;
                }
            }
            if (hasChanged)
                interning.setHash(position, computeHash(records[position]));
        });
        interning.commit();
        types.finalIDs = interning.takeIDs();
        publish(types, merge.remap.m_types);
    }

    bool run(std::span<const unsigned> taskOrder)
    {
        findTypesThatReferToProvisionalRecords();
        if (!findCommitOrder(taskOrder))
            return false;
        commitRecordsWithoutReferences<PropertyKey>(propertyKeys, merge.remap.m_propertyKeys);
        commitRecordsWithoutReferences<LayoutSet>(layoutSets, merge.remap.m_layoutSets);
        commitRecordsWithoutReferences<CalleeSet>(calleeSets, merge.remap.m_calleeSets);
        commitTypes();
        return true;
    }

    AOT_CLASS_THRESHOLD(unsigned, maxNestingDepth, 254);

    Merge& merge;
    TypeInterningPhase& phase;
    const ParallelLoops& loops;
    CommitOrder<PropertyKeyID> propertyKeys;
    CommitOrder<LayoutSetID> layoutSets;
    CommitOrder<CalleeSetID> calleeSets;
    CommitOrder<TypeID> types;
    Vector<uint8_t> refersToOthers;
    Vector<uint8_t> nestingDepths;
    bool isNestedTooDeeply { false };
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

NEVER_INLINE const Type& TypeTable::provisionalAt(TypeID id) const
{
    return TypeInterningPhase::recordAt<Type>(phaseOfProvisionalIDs(), id);
}

NEVER_INLINE const LayoutSet& TypeTable::provisionalAt(LayoutSetID id) const
{
    return TypeInterningPhase::recordAt<LayoutSet>(phaseOfProvisionalIDs(), id);
}

NEVER_INLINE const CalleeSet& TypeTable::provisionalAt(CalleeSetID id) const
{
    return TypeInterningPhase::recordAt<CalleeSet>(phaseOfProvisionalIDs(), id);
}

NEVER_INLINE const PropertyKey& TypeTable::provisionalAt(PropertyKeyID id) const
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
    RELEASE_ASSERT(!table.m_openPhase && !ParallelLoops::isInsideLoop());
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

uint64_t TypeInterningPhase::nextSerialNumber()
{
    static std::atomic<uint64_t> numberOfPhases { 0 };
    return numberOfPhases.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t TypeInterningPhase::currentPhaseSerialNumber(const TypeTable& table)
{
    if (const TypeInterningPhase* phase = table.phaseOfProvisionalIDs())
        return phase->m_serialNumber;
    TypeInterningPhase& phase = table.attachScratchArena();
    table.detachScratchArena(phase);
    return phase.m_serialNumber;
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

TypeRemap TypeInterningPhase::finish(const StringRemap& strings, const ParallelLoops* loops)
{
    Vector<unsigned> taskOrder(m_firstUses.size(), [](size_t task) {
        return static_cast<unsigned>(task);
    });
    return merge(taskOrder.span(), nullptr, strings, loops);
}

TypeRemap TypeInterningPhase::finish(std::span<const unsigned> taskOrder, const FunctionRemap& functions, const StringRemap& strings, const ParallelLoops* loops)
{
    for (unsigned task : taskOrder)
        RELEASE_ASSERT(task < m_firstUses.size());
    return merge(taskOrder, &functions, strings, loops);
}

void TypeInterningPhase::discard()
{
    close();
}

TypeRemap TypeInterningPhase::merge(std::span<const unsigned> taskOrder, const FunctionRemap* functions, const StringRemap& strings, const ParallelLoops* loops)
{
    AOT_THRESHOLD(size_t, minRecordsForThreads, 4096);
    RELEASE_ASSERT(m_committedTable);
    close();
    unsigned initialTableSize = m_committedTable->size();

    Merge merge(*this, functions, strings);
    bool isMerged = loops && loops->width() > 1 && merge.numberOfProvisionalRecords >= minRecordsForThreads && ParallelMerge(merge, *loops).run(taskOrder);
    if (!isMerged) {
        for (unsigned task : taskOrder)
            merge.commitTask(task);
    }

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
    ++m_unshareableEventCount;
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
    provisionalRecords.hashes.append(hash);
    provisionalRecords.lastLease.append(0);
    return didUse<Record, RecordID>(storage, position);
}

void TypeMemo::add(uint64_t key, TypeID type)
{
    constexpr size_t initialNumberOfSlots = 16;
    RELEASE_ASSERT(type);
    if ((static_cast<size_t>(m_size) + 1) * 2 > m_slots.size()) {
        size_t numberOfSlots = std::max(initialNumberOfSlots, m_slots.size() * 2);
        Vector<Slot> oldSlots = std::exchange(m_slots, Vector<Slot>(FillWith { }, numberOfSlots, Slot { }));
        m_shift = 64 - std::countr_zero(numberOfSlots);
        for (const Slot& slot : oldSlots) {
            if (slot.type)
                insert(slot);
        }
    }
    insert({ key, type });
    ++m_size;
}

void TypeMemo::insert(Slot slot)
{
    size_t mask = m_slots.size() - 1;
    size_t index = firstIndexOf(slot.key);
    while (m_slots[index].type)
        index = (index + 1) & mask;
    m_slots[index] = slot;
}

TypeID TypeArena::intern(const Type& type)
{
    if (m_serialTable) {
        ++m_unshareableEventCount;
        return m_serialTable->intern(type);
    }
    return intern(Type(type));
}

TypeID TypeArena::intern(Type&& type)
{
    if (m_serialTable) {
        ++m_unshareableEventCount;
        return m_serialTable->intern(WTF::move(type));
    }

    type.canonicalize();
    ASSERT(!type.layouts || at(type.layouts).listsEveryLayout());
    return internCanonical<Type, TypeID>(WTF::move(type));
}

LayoutSetID TypeArena::intern(LayoutSet&& set)
{
    if (m_serialTable) {
        ++m_unshareableEventCount;
        return m_serialTable->intern(WTF::move(set));
    }

    canonicalize(set);
    return internCanonical<LayoutSet, LayoutSetID>(WTF::move(set));
}

CalleeSetID TypeArena::intern(CalleeSet&& set)
{
    if (m_serialTable) {
        ++m_unshareableEventCount;
        return m_serialTable->intern(WTF::move(set));
    }

    canonicalize(set);
    return internCanonical<CalleeSet, CalleeSetID>(WTF::move(set));
}

PropertyKeyID TypeArena::intern(const PropertyKey& key)
{
    if (m_serialTable) {
        ++m_unshareableEventCount;
        return m_serialTable->intern(key);
    }

    return internCanonical<PropertyKey, PropertyKeyID>(PropertyKey(key));
}

Type TypeArena::adopt(const Type& fromOtherArena)
{
    Type adopted = fromOtherArena;
    adopted.layouts = adopt(adopted.layouts);
    rewriteValues(adopted.symbolLiterals, [&](PropertyKeyID key) {
        return adopt(key);
    });
    for (ClassPart& part : adopted.classes) {
        for (TypeID& contents : part.writtenContents)
            contents = adopt(contents);
    }
    return adopted;
}

TypeID TypeArena::adopt(TypeID fromOtherArena)
{
    if (!TypeTable::isProvisional(fromOtherArena))
        return fromOtherArena;
    return intern(adopt(at(fromOtherArena)));
}

LayoutSetID TypeArena::adopt(LayoutSetID fromOtherArena)
{
    if (!TypeTable::isProvisional(fromOtherArena))
        return fromOtherArena;
    if (at(fromOtherArena).written().size() < minCachedSetSize)
        return intern(LayoutSet(at(fromOtherArena)));
    auto known = m_adoptedLayoutSets.find(fromOtherArena);
    if (known != m_adoptedLayoutSets.end()) {
        ++m_unshareableEventCount;
        return known->value;
    }
    LayoutSetID adopted = intern(LayoutSet(at(fromOtherArena)));
    m_adoptedLayoutSets.add(fromOtherArena, adopted);
    return adopted;
}

bool TypeArena::hasLeasedStorage(uint32_t number) const
{
    return std::ranges::any_of(m_storages, [&](const TypeInterningPhase::Storage* storage) {
        return storage->number == number;
    });
}

CalleeSetID TypeArena::adopt(CalleeSetID fromOtherArena)
{
    if (!TypeTable::isProvisional(fromOtherArena))
        return fromOtherArena;
    return intern(CalleeSet(at(fromOtherArena)));
}

PropertyKeyID TypeArena::adopt(PropertyKeyID fromOtherArena)
{
    if (!TypeTable::isProvisional(fromOtherArena))
        return fromOtherArena;
    return intern(at(fromOtherArena));
}

LayoutSetID TypeArena::completeLayoutSet(std::span<const LayoutID> unsortedMembers)
{
    return intern(LayoutSet::complete(Vector<LayoutID>(unsortedMembers)));
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
    uint64_t key = TypeMemo::keyOf(a, b);
    if (TypeID known = m_unions.find(key)) {
        ++m_unshareableEventCount;
        return known;
    }
    TypeID result = sharedAnswer(SharedAnswers::AboutTypes, sharedKeyOf(SharedTypeMemo::Relation::Union, a, b), [&] {
        return intern(at(a).unionWith(at(b), *this));
    });
    m_unions.add(key, result);
    return result;
}

TypeID TypeArena::intersectionOf(TypeID a, TypeID b)
{
    if (a == b)
        return a;
    uint64_t key = TypeMemo::keyOf(a, b);
    if (TypeID known = m_intersections.find(key)) {
        ++m_unshareableEventCount;
        return known;
    }
    TypeID result = sharedAnswer(SharedAnswers::AboutTypes, sharedKeyOf(SharedTypeMemo::Relation::Intersection, a, b), [&] {
        return intern(at(a).intersectWith(at(b), *this));
    });
    m_intersections.add(key, result);
    return result;
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

static uint64_t pairKey(LayoutSetID a, LayoutSetID b)
{
    return static_cast<uint64_t>(a.index()) << 32 | b.index();
}

bool TypeArena::shouldCacheResult(LayoutSetID a, LayoutSetID b) const
{
    return at(a).written().size() + at(b).written().size() >= minCachedSetSize;
}

LayoutSetID TypeArena::internCompleteSet(Vector<LayoutID>&& sortedMembers)
{
    if (sortedMembers.isEmpty())
        return { };
    return intern(LayoutSet::complete(WTF::move(sortedMembers)));
}

LayoutSetID TypeArena::computeResult(LayoutSetOperation operation, LayoutSetID a, LayoutSetID b)
{
    const LayoutSet& first = at(a);
    const LayoutSet& second = at(b);
    std::span<const LayoutID> inFirst = first.completeMembers();
    std::span<const LayoutID> inSecond = second.completeMembers();

    Vector<LayoutID> members;
    switch (operation) {
    case LayoutSetOperation::Union:
        members.grow(inFirst.size() + inSecond.size());
        members.shrink(std::ranges::set_union(inFirst, inSecond, members.begin()).out - members.begin());
        break;
    case LayoutSetOperation::Intersection:
        members.grow(std::min(inFirst.size(), inSecond.size()));
        members.shrink(std::ranges::set_intersection(inFirst, inSecond, members.begin()).out - members.begin());
        break;
    case LayoutSetOperation::Difference:
        members.grow(inFirst.size());
        members.shrink(std::ranges::set_difference(inFirst, inSecond, members.begin()).out - members.begin());
        break;
    }

    auto isResult = [&](const LayoutSet& set) {
        return members.size() == set.written().size();
    };
    if (isResult(first))
        return a;
    if (operation != LayoutSetOperation::Difference && isResult(second))
        return b;
    return internCompleteSet(WTF::move(members));
}

LayoutSetID TypeArena::cachedResult(LayoutSetOperation operation, LayoutSetID a, LayoutSetID b)
{
    if (!shouldCacheResult(a, b))
        return computeResult(operation, a, b);

    LayoutSetPairMap<LayoutSetID>& results = m_layoutSetResultCache[static_cast<size_t>(operation)];
    auto known = results.find(pairKey(a, b));
    if (known != results.end()) {
        ++m_unshareableEventCount;
        return known->value;
    }
    LayoutSetID result = computeResult(operation, a, b);
    results.add(pairKey(a, b), result);
    return result;
}

LayoutSetID TypeArena::unionOf(LayoutSetID a, LayoutSetID b)
{
    if (!b || a == b)
        return a;
    if (!a)
        return b;
    return cachedResult(LayoutSetOperation::Union, std::min(a, b), std::max(a, b));
}

LayoutSetID TypeArena::intersectionOf(LayoutSetID a, LayoutSetID b)
{
    if (!a || !b)
        return { };
    if (a == b)
        return a;
    return cachedResult(LayoutSetOperation::Intersection, std::min(a, b), std::max(a, b));
}

LayoutSetID TypeArena::differenceOf(LayoutSetID a, LayoutSetID b)
{
    if (!a || !b)
        return a;
    return cachedResult(LayoutSetOperation::Difference, a, b);
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
