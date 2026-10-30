/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDerivedFunctions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTInterningPhases.h"
#include "AOTSummarizeForInlining.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/ParkingLot.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr DerivedFunctionRole derivedFunctionRoles[] = {
#define AOT_ROLE_DERIVED_ON_REQUEST(role, builder, target, created) \
    { FunctionRole::role, DerivedFunctionTarget::target, DerivedFunctionCreation::OnRequest, builder, Wire::StatisticID::created },
    FOR_EACH_AOT_FUNCTION_ROLE_DERIVED_ON_REQUEST(AOT_ROLE_DERIVED_ON_REQUEST)
#undef AOT_ROLE_DERIVED_ON_REQUEST
    { FunctionRole::GenericVersion, DerivedFunctionTarget::Function, DerivedFunctionCreation::ByProgramPass, nullptr, Wire::StatisticID::None },
    { FunctionRole::CoroutineBody, DerivedFunctionTarget::Function, DerivedFunctionCreation::ByProgramPass, nullptr, Wire::StatisticID::None },
};

static constexpr unsigned numberOfRowsFor(FunctionRole role)
{
    return std::ranges::count(derivedFunctionRoles, role, &DerivedFunctionRole::role);
}

static_assert(std::ranges::all_of(derivedFunctionRoles, [](const DerivedFunctionRole& row) { return numberOfRowsFor(row.role) == 1; }));
static_assert(!numberOfRowsFor(FunctionRole::Source) && !numberOfRowsFor(FunctionRole::Library) && !numberOfRowsFor(FunctionRole::Model));

const DerivedFunctionRole* findDerivedFunctionRole(FunctionRole role)
{
    for (const DerivedFunctionRole& row : derivedFunctionRoles) {
        if (row.role == role)
            return &row;
    }
    return nullptr;
}

static bool isValidKey(const DerivedFunctionKey& key, DerivedFunctionTarget target)
{
    switch (target) {
    case DerivedFunctionTarget::Function:
        return key.target && !key.node;
    case DerivedFunctionTarget::FunctionAndCallee: {
        FunctionID callee(key.node);
        return key.target && callee && !ProvisionalFunctions::contains(callee);
    }
    case DerivedFunctionTarget::Layout:
        return !key.target && !key.node;
    case DerivedFunctionTarget::Type:
        return !key.target && !key.node && !TypeTable::isProvisional(TypeID(key.parameter));
    case DerivedFunctionTarget::Selector:
        return !key.target;
    case DerivedFunctionTarget::FunctionAndSelector:
        return !!key.target;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DerivedFunctionKey keyOfFunction(const Function& function)
{
    return { .role = function.role, .target = function.derivation.source, .parameter = function.derivation.parameter, .node = function.derivation.node };
}

static std::unique_ptr<Function> copyWithoutBody(const Function& function)
{
    auto copy = makeUniqueWithoutFastMallocCheck<Function>();
    static_cast<FunctionRecord&>(*copy) = function;
    copy->inlineFrames.clear();
    return copy;
}

DerivedFunctions::Entry::~Entry() = default;

thread_local DerivedFunctions::RequestRecorder* DerivedFunctions::RequestRecorder::s_current { nullptr };

DerivedFunctions::IDTable::Slots::Slots(size_t capacity)
    : slots(makeUniqueArray<Slot>(capacity))
    , mask(capacity - 1)
{
    RELEASE_ASSERT(hasOneBitSet(capacity));
}

auto DerivedFunctions::IDTable::Slots::findWhileNobodyAdds(const DerivedFunctionKey& key, uint32_t hash) const -> Slot&
{
    for (size_t index = hash & mask;; index = (index + 1) & mask) {
        Slot& slot = slots[index];
        if (slot.id.load(std::memory_order_relaxed) == FunctionID().index() || slot.key == key)
            return slot;
    }
}

FunctionID DerivedFunctions::IDTable::get(const DerivedFunctionKey& key, uint32_t hash) const
{
    const Slots* current = m_current.load(std::memory_order_acquire);
    if (!current)
        return FunctionID();
    for (size_t index = hash & current->mask;; index = (index + 1) & current->mask) {
        const Slot& slot = current->slots[index];
        // The key of a slot is written before its id, and an empty slot may get the id of another key at any time: the id is read once.
        uint32_t id = slot.id.load(std::memory_order_acquire);
        if (id == FunctionID().index())
            return FunctionID();
        if (slot.key == key)
            return FunctionID(id);
    }
}

void DerivedFunctions::IDTable::replaceSlots(size_t capacity, NOESCAPE const ScopedLambda<bool(FunctionID)>& shouldRemove)
{
    auto replacement = makeUnique<Slots>(capacity);
    m_size = 0;
    if (const Slots* current = m_current.load(std::memory_order_relaxed)) {
        for (size_t index = 0; index <= current->mask; ++index) {
            const Slot& old = current->slots[index];
            uint32_t id = old.id.load(std::memory_order_relaxed);
            if (id == FunctionID().index() || shouldRemove(FunctionID(id)))
                continue;
            Slot& slot = replacement->findWhileNobodyAdds(old.key, hashOf(old.key));
            slot.key = old.key;
            slot.id.store(id, std::memory_order_relaxed);
            ++m_size;
        }
    }
    m_current.store(replacement.get(), std::memory_order_release);
    m_allSlots.append(WTF::move(replacement));
}

auto DerivedFunctions::IDTable::findForAdding(const DerivedFunctionKey& key, uint32_t hash) -> Slot&
{
    constexpr size_t smallestCapacity = 16;
    const Slots* current = m_current.load(std::memory_order_relaxed);
    if (!current || 2 * (static_cast<size_t>(m_size) + 1) > current->mask + 1) {
        replaceSlots(current ? 2 * (current->mask + 1) : smallestCapacity, [](FunctionID) {
            return false;
        });
        current = m_current.load(std::memory_order_relaxed);
    }
    return current->findWhileNobodyAdds(key, hash);
}

void DerivedFunctions::IDTable::add(const DerivedFunctionKey& key, uint32_t hash, FunctionID id)
{
    Slot& slot = findForAdding(key, hash);
    if (slot.id.load(std::memory_order_relaxed) != FunctionID().index())
        return;
    slot.key = key;
    slot.id.store(id.index(), std::memory_order_release);
    ++m_size;
}

void DerivedFunctions::IDTable::set(const DerivedFunctionKey& key, uint32_t hash, FunctionID id)
{
    Slot& slot = findForAdding(key, hash);
    if (slot.id.load(std::memory_order_relaxed) == FunctionID().index()) {
        slot.key = key;
        ++m_size;
    }
    slot.id.store(id.index(), std::memory_order_release);
}

void DerivedFunctions::IDTable::removeIfWhileNobodyLooks(NOESCAPE const ScopedLambda<bool(FunctionID)>& shouldRemove)
{
    const Slots* current = m_current.load(std::memory_order_relaxed);
    if (!current)
        return;
    Vector<std::unique_ptr<Slots>> replaced = std::exchange(m_allSlots, { });
    replaceSlots(current->mask + 1, shouldRemove);
}

void DerivedFunctions::removeIDsIf(NOESCAPE const ScopedLambda<bool(FunctionID)>& shouldRemove)
{
    forEachIndexInParallel(m_build.parallelLoops(), m_shards.size(), [&](unsigned index) {
        Shard& shard = m_shards[index];
        Locker locker { shard.lock };
        shard.ids.removeIfWhileNobodyLooks(shouldRemove);
    });
}

FunctionID DerivedFunctions::find(const DerivedFunctionKey& key) const
{
    uint32_t hash = hashOf(key);
    return shardOf(hash).ids.get(key, hash);
}

FunctionID DerivedFunctions::ensure(const DerivedFunctionKey& key)
{
    const DerivedFunctionRole* role = findDerivedFunctionRole(key.role);
    RELEASE_ASSERT(role && role->creation == DerivedFunctionCreation::OnRequest);
    RELEASE_ASSERT(isValidKey(key, role->target));

    FunctionID id = findOrAdd(key);
    if (!contains(id) || m_phase->entries[ordinalOf(id)].isConfirmed())
        return id;
    RequestRecorder* recorder = RequestRecorder::s_current;
    if (recorder && recorder->handling == RequestHandling::Record)
        recorder->record(ordinalOf(id));
    else
        m_phase->confirm(ordinalOf(id));
    return id;
}

FunctionID DerivedFunctions::findOrAdd(const DerivedFunctionKey& key)
{
    uint32_t hash = hashOf(key);
    Shard& shard = shardOf(hash);
    if (FunctionID existing = shard.ids.get(key, hash))
        return existing;

    uint32_t ordinal;
    {
        Locker locker { shard.lock };
        if (FunctionID existing = shard.ids.get(key, hash))
            return existing;

        RELEASE_ASSERT(m_phase);
        ordinal = m_phase->add(key);
        shard.ids.add(key, hash, idOf(ordinal));
    }
    m_phase->createEntriesAheadOf(ordinal);
    return idOf(ordinal);
}

static constexpr uint32_t entriesCreatedAfter(uint32_t numberCreated)
{
    return std::max<uint32_t>(512, numberCreated / 4);
}

// Every thread asks for new functions all the time, so a number is handed out without a lock. Entries and their tasks are created many at a time.
uint32_t DerivedFunctions::PhaseState::add(const DerivedFunctionKey& key)
{
    uint32_t ordinal = numberOfEntries.fetch_add(1, std::memory_order_acq_rel);
    if (ordinal >= numberOfCreatedEntries.load(std::memory_order_acquire)) {
        Locker locker { creationLock };
        while (ordinal >= numberOfCreatedEntries.load(std::memory_order_relaxed))
            createEntries();
    }
    entries[ordinal].key = key;
    return ordinal;
}

// The thread that takes a certain one of the last entries creates the next ones, so that nobody waits for them, not even when that thread loses its processor meanwhile.
void DerivedFunctions::PhaseState::createEntriesAheadOf(uint32_t ordinal)
{
    uint32_t numberCreated = numberOfCreatedEntries.load(std::memory_order_relaxed);
    if (ordinal + entriesCreatedAfter(numberCreated) / 2 != numberCreated)
        return;
    Locker locker { creationLock };
    if (numberOfCreatedEntries.load(std::memory_order_relaxed) == numberCreated)
        createEntries();
}

void DerivedFunctions::PhaseState::createEntries()
{
    uint32_t numberCreated = numberOfCreatedEntries.load(std::memory_order_relaxed);
    uint32_t numberAdded = entriesCreatedAfter(numberCreated);
    unsigned firstTask = interning.addTasks(2 * numberAdded);
    entries.grow(numberCreated + numberAdded);
    for (uint32_t index = 0; index < numberAdded; ++index)
        entries[numberCreated + index].buildTask = firstTask + 2 * index;
    numberOfCreatedEntries.store(numberCreated + numberAdded, std::memory_order_release);
}

// The derivation of a function names the target of its key, so the target needs an id too.
void DerivedFunctions::PhaseState::confirm(uint32_t requestedOrdinal)
{
    Vector<uint32_t, 16> worklist { requestedOrdinal };
    while (!worklist.isEmpty()) {
        Entry& entry = entries[worklist.takeLast()];
        uint8_t previous = entry.flags.fetch_or(Entry::Confirmed, std::memory_order_acq_rel);
        if (previous & Entry::Confirmed)
            continue;
        if (ProvisionalFunctions::contains(entry.key.target))
            worklist.append(ProvisionalFunctions::ordinalOf(entry.key.target));
        if (previous & Entry::Built)
            worklist.appendVector(entry.nestedRequests);
    }
}

NEVER_INLINE DerivedFunctions::Entry& DerivedFunctions::entryOf(FunctionID id) const
{
    RELEASE_ASSERT(m_phase && contains(id) && ordinalOf(id) < m_phase->size());
    return m_phase->entries[ordinalOf(id)];
}

NEVER_INLINE const Function& DerivedFunctions::at(FunctionID id) const
{
    Entry& entry = entryOf(id);
    if (const Function* function = entry.published.load(std::memory_order_acquire))
        return *function;
    return buildOrWait(entry);
}

const InliningSummary& DerivedFunctions::inliningSummaryOf(FunctionID id, NOESCAPE const ScopedLambda<InliningSummary()>& summarize) const
{
    Entry& entry = entryOf(id);
    if (const InliningSummary* summary = entry.publishedInliningSummary.load(std::memory_order_acquire))
        return *summary;

    Locker locker { entry.inliningSummaryLock };
    if (!entry.inliningSummary) {
        entry.inliningSummary = makeUniqueWithoutFastMallocCheck<const InliningSummary>(summarize());
        entry.publishedInliningSummary.store(entry.inliningSummary.get(), std::memory_order_release);
    }
    return *entry.inliningSummary;
}

const Function& DerivedFunctions::buildOrWait(Entry& entry) const
{
    if (entry.state.compareExchangeStrong(State::Requested, State::Building) != State::Requested) {
        RELEASE_ASSERT(entry.builder.load(std::memory_order_relaxed) != &Thread::currentSingleton());
        while (entry.state.load() != State::Published)
            ParkingLot::compareAndPark(&entry.state, State::Building);
        return *entry.asBuilt;
    }
    entry.builder.store(&Thread::currentSingleton(), std::memory_order_relaxed);

    {
        RequestRecorder recorder(RequestHandling::Record);
        {
            const DerivedFunctionKey& key = entry.key;
            TaskArenas arenas(m_phase->interning, entry.buildTask);
            DerivedFunctionInputs inputs { m_phase->program, m_phase->flow, arenas.types, arenas.strings, const_cast<DerivedFunctions&>(*this), m_build };
            auto function = makeUniqueWithoutFastMallocCheck<Function>(findDerivedFunctionRole(key.role)->build(key, inputs));
            RELEASE_ASSERT(function->role == key.role && function->hasBody());
            function->derivation.source = key.target;
            function->derivation.parameter = key.parameter;
            function->derivation.node = key.node;
            entry.asBuilt = WTF::move(function);
        }
        entry.nestedRequests = recorder.takeRequests();
    }
    entry.published.store(entry.asBuilt.get(), std::memory_order_release);
    entry.state.store(State::Published);
    ParkingLot::unparkAll(&entry.state);

    // Whichever thread sets the second of Confirmed and Built confirms the nested requests.
    if (entry.flags.fetch_or(Entry::Built, std::memory_order_acq_rel) & Entry::Confirmed) {
        for (uint32_t ordinal : entry.nestedRequests)
            m_phase->confirm(ordinal);
    }
    return *entry.asBuilt;
}

std::weak_ordering DerivedFunctions::compare(FunctionID a, FunctionID b) const
{
    if (a == b)
        return std::weak_ordering::equivalent;
    if (!a || !b)
        return a ? std::weak_ordering::greater : std::weak_ordering::less;
    bool isProvisional = contains(a);
    if (isProvisional != contains(b))
        return isProvisional ? std::weak_ordering::greater : std::weak_ordering::less;
    if (!isProvisional)
        return a.index() <=> b.index();
    return compareKeys(keyOf(a), keyOf(b));
}

std::weak_ordering DerivedFunctions::compareKeys(const DerivedFunctionKey& a, const DerivedFunctionKey& b) const
{
    // The target comes first so that a function sorts after its target: final ids follow this order, and a derivation's source must have the lower id.
    if (std::weak_ordering order = compare(a.target, b.target); order != 0)
        return order;
    if (std::weak_ordering order = a.role <=> b.role; order != 0)
        return order;
    if (std::weak_ordering order = a.parameter <=> b.parameter; order != 0)
        return order;
    return a.node <=> b.node;
}

void DerivedFunctions::record(const DerivedFunctionKey& key, FunctionID id)
{
    RELEASE_ASSERT(!m_phase && id && !contains(id) && findDerivedFunctionRole(key.role));
    uint32_t hash = hashOf(key);
    Shard& shard = shardOf(hash);
    Locker locker { shard.lock };
    shard.ids.set(key, hash, id);
}

void DerivedFunctions::recordFunctionsOf(const Program& program)
{
    RELEASE_ASSERT(!m_phase);
    const ParallelLoops& loops = m_build.parallelLoops();
    unsigned first = program.numberOfFunctionsAsRead();
    // The hash above the number: sorted, the functions of a shard stand together, and of two with one key the lower number comes first and stays.
    Vector<uint64_t> derived = collectInParallel<uint64_t>(loops, program.functions.size() - first, [&](unsigned offset, Vector<uint64_t>& found) {
        const Function& function = program.functions[first + offset];
        if (function.hasBody() && findDerivedFunctionRole(function.role))
            found.append(static_cast<uint64_t>(hashOf(keyOfFunction(function))) << 32 | (first + offset));
    });
    parallelSort(loops, derived.mutableSpan(), std::less<uint64_t> { });

    Vector<uint32_t> firstInShard(m_shards.size() + 1);
    uint32_t position = 0;
    for (size_t shard = 0; shard < m_shards.size(); ++shard) {
        firstInShard[shard] = position;
        while (position < derived.size() && derived[position] >> (64 - numberOfShardsLog2) == shard)
            ++position;
    }
    firstInShard[m_shards.size()] = position;
    RELEASE_ASSERT(position == derived.size());

    forEachIndexInParallel(loops, m_shards.size(), [&](unsigned index) {
        Shard& shard = m_shards[index];
        Locker locker { shard.lock };
        for (uint64_t hashAndFunction : derived.subspan(firstInShard[index], firstInShard[index + 1] - firstInShard[index])) {
            FunctionID function(static_cast<uint32_t>(hashAndFunction));
            shard.ids.add(keyOfFunction(program.at(function)), static_cast<uint32_t>(hashAndFunction >> 32), function);
        }
    });
}

void DerivedFunctions::forgetFunctionsWithoutBody(const Program& program)
{
    removeIDsIf([&](FunctionID id) {
        RELEASE_ASSERT(!contains(id));
        return !program.at(id).hasBody();
    });
}

DerivedFunctionPhase::DerivedFunctionPhase(DerivedFunctions& table, Program& program, const FlowResults& flow, InterningPhases& interning)
    : m_table(table)
{
    RELEASE_ASSERT(!table.m_phase);
    table.m_phase = makeUnique<DerivedFunctions::PhaseState>(program, flow, interning);
    program.setProvisionalFunctions(&table);
}

DerivedFunctionPhase::~DerivedFunctionPhase()
{
    m_table.m_phase->program.setProvisionalFunctions(nullptr);
    if (!m_didCommit) {
        m_table.removeIDsIf([](FunctionID id) {
            return ProvisionalFunctions::contains(id);
        });
    }
    m_table.m_build.destroyLater(WTF::move(m_table.m_phase));
}

void DerivedFunctionPhase::confirmRequestsOf(FunctionID requester)
{
    DerivedFunctions::PhaseState& phase = *m_table.m_phase;
    Vector<uint32_t> requests;
    {
        Locker locker { phase.recordedRequestsLock };
        requests = phase.recordedRequests.take(requester);
    }
    for (uint32_t ordinal : requests)
        phase.confirm(ordinal);
}

DerivedFunctionCommit DerivedFunctionPhase::commit(const BitVector& reachedOrdinals)
{
    RELEASE_ASSERT(!m_didCommit);
    DerivedFunctions::PhaseState& phase = *m_table.m_phase;
    auto& entries = phase.entries;

    // Building a function can confirm one with a lower ordinal.
    bool didBuild;
    do {
        didBuild = false;
        for (uint32_t ordinal = 0; ordinal < phase.size(); ++ordinal) {
            if (!entries[ordinal].isConfirmed() || entries[ordinal].published.load(std::memory_order_relaxed))
                continue;
            m_table.at(ProvisionalFunctions::idOf(ordinal));
            didBuild = true;
        }
    } while (didBuild);

    Vector<uint32_t> order;
    order.reserveInitialCapacity(phase.size());
    for (uint32_t ordinal = 0; ordinal < phase.size(); ++ordinal) {
        if (entries[ordinal].isConfirmed())
            order.append(ordinal);
    }
    parallelSort(m_table.m_build.parallelLoops(), order.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return std::is_lt(m_table.compareKeys(entries[a].key, entries[b].key));
    });
    RELEASE_ASSERT(phase.program.functions.size() + order.size() <= ProvisionalFunctions::firstIndex);

    DerivedFunctionCommit result;
    result.firstID = FunctionID(phase.program.functions.size());
    Vector<FunctionID> finalIDs(phase.size());
    for (uint32_t ordinal : order) {
        DerivedFunctions::Entry& entry = entries[ordinal];
        finalIDs[ordinal] = FunctionID(result.firstID.index() + result.functions.size());
        result.tasks.append(entry.buildTask);
        // Inline frames, derivations and interned types name functions that no call reaches, so an unreached function loses its body and keeps its id.
        if (!reachedOrdinals.get(ordinal)) {
            result.functions.append(copyWithoutBody(*entry.asBuilt));
            continue;
        }
        RELEASE_ASSERT(entry.optimized);
        result.tasks.append(entry.optimizeTask());
        result.functions.append(WTF::move(entry.optimized));
        m_table.m_build.statistics().count(findDerivedFunctionRole(entry.key.role)->created);
    }
    result.remap = FunctionRemap(WTF::move(finalIDs));

    m_table.removeIDsIf([](FunctionID id) {
        return ProvisionalFunctions::contains(id);
    });
    for (uint32_t ordinal = 0; ordinal < phase.size(); ++ordinal) {
        if (!reachedOrdinals.get(ordinal))
            continue;
        DerivedFunctionKey key = entries[ordinal].key;
        key.target = result.remap(key.target);
        uint32_t hash = DerivedFunctions::hashOf(key);
        DerivedFunctions::Shard& shard = m_table.shardOf(hash);
        Locker locker { shard.lock };
        shard.ids.add(key, hash, result.remap(ProvisionalFunctions::idOf(ordinal)));
    }
    m_didCommit = true;
    return result;
}

DerivedFunctionRequester::DerivedFunctionRequester(DerivedFunctionPhase& phase, FunctionID requester, bool isConfirmed)
    : m_phase(phase)
    , m_requester(requester)
    , m_recorder(isConfirmed ? DerivedFunctions::RequestHandling::Confirm : DerivedFunctions::RequestHandling::Record)
{
}

DerivedFunctionRequester::~DerivedFunctionRequester()
{
    Vector<uint32_t> requests = m_recorder.takeRequests();
    if (requests.isEmpty())
        return;
    DerivedFunctions::PhaseState& phase = *m_phase.m_table.m_phase;
    Locker locker { phase.recordedRequestsLock };
    auto result = phase.recordedRequests.add(m_requester, WTF::move(requests));
    RELEASE_ASSERT(result.isNewEntry);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
