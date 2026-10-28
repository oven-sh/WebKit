/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringArena.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(ProvisionalStringID::maxStorages == Limits::maxWorkers);
static_assert(ProvisionalStringID::storageOf(StringID()) == ProvisionalStringID::maxStorages, "The null id must not decode to a storage.");
static_assert(ProvisionalStringID::storageOf(StringID(StringID::hashTableDeletedIndex)) == ProvisionalStringID::maxStorages, "The deleted id must not decode to a storage.");

StringID StringRemap::finalIDOf(StringID provisional) const
{
    uint32_t number = ProvisionalStringID::storageOf(provisional);
    uint32_t position = ProvisionalStringID::positionOf(provisional);
    RELEASE_ASSERT(number < m_finalIDs.size() && position < m_finalIDs[number].size());
    StringID finalID = m_finalIDs[number][position];
    RELEASE_ASSERT(finalID);
    return finalID;
}

StringInterningPhase::StringInterningPhase(StringTable& table, unsigned numberOfTasks)
    : m_table(table)
{
    m_firstUses.grow(numberOfTasks);
    RELEASE_ASSERT(!table.m_openPhase && !ParallelLoops::isInsideLoop());
    table.m_openPhase = this;
}

StringInterningPhase::~StringInterningPhase()
{
    RELEASE_ASSERT(!m_isOpen);
}

unsigned StringInterningPhase::addTasks(unsigned count)
{
    Locker locker { m_firstUsesGrowthLock };
    RELEASE_ASSERT(m_isOpen);
    unsigned first = m_firstUses.size();
    m_firstUses.grow(first + count);
    return first;
}

std::span<const uint8_t> StringInterningPhase::textOf(StringID provisional) const
{
    uint32_t number = ProvisionalStringID::storageOf(provisional);
    RELEASE_ASSERT(number < ProvisionalStringID::maxStorages);
    const Storage* storage = m_storages.at(number);
    uint32_t position = ProvisionalStringID::positionOf(provisional);
    RELEASE_ASSERT(storage && position < storage->strings.size());
    return storage->strings[position].span();
}

auto StringInterningPhase::leaseStorage() -> Storage&
{
    RELEASE_ASSERT(m_isOpen);
    Storage& storage = m_storages.lease();
    ++storage.lease;
    RELEASE_ASSERT(storage.lease);
    return storage;
}

void StringInterningPhase::discard()
{
    RELEASE_ASSERT(m_isOpen && !m_storages.hasLeases());
    m_isOpen = false;
    m_table.m_openPhase = nullptr;
}

StringRemap StringInterningPhase::finish()
{
    Vector<unsigned> taskOrder(m_firstUses.size(), [](size_t task) {
        return static_cast<unsigned>(task);
    });
    return finish(taskOrder.span());
}

StringRemap StringInterningPhase::finish(std::span<const unsigned> taskOrder)
{
    discard();

    StringRemap remap;
    if (!m_storages.isEmpty()) {
        remap.m_finalIDs = Vector<Vector<StringID>>(ProvisionalStringID::maxStorages, [&](size_t number) {
            const Storage* storage = m_storages.at(number);
            return Vector<StringID>(FillWith { }, storage ? storage->strings.size() : 0, StringID());
        });
    }
    remap.m_tasksWithNewStrings.ensureSize(m_firstUses.size());

    for (unsigned task : taskOrder) {
        RELEASE_ASSERT(task < m_firstUses.size());
        if (m_firstUses[task].isEmpty())
            continue;
        remap.m_tasksWithNewStrings.quickSet(task);
        remap.m_isIdentity = false;
        for (StringID provisional : m_firstUses[task]) {
            StringID& finalID = remap.m_finalIDs[ProvisionalStringID::storageOf(provisional)][ProvisionalStringID::positionOf(provisional)];
            if (!finalID)
                finalID = m_table.intern(textOf(provisional));
        }
    }
    return remap;
}

StringArena::StringArena(StringTable& table)
    : m_table(table)
{
}

StringArena::StringArena(StringInterningPhase& phase, unsigned task)
    : m_table(phase.m_table)
    , m_phase(&phase)
{
    RELEASE_ASSERT(task < phase.m_firstUses.size());
    m_firstUses = &phase.m_firstUses[task];
}

StringArena::~StringArena()
{
    for (StringInterningPhase::Storage* storage : m_storages)
        m_phase->m_storages.giveBack(*storage);
}

StringID StringArena::intern(StringView view)
{
    return intern(encodeProgramString(view).span());
}

StringID StringArena::intern(std::span<const uint8_t> text)
{
    ASSERT(isCanonicalProgramString(text));
    if (!m_phase)
        return m_table.intern(text);

    if (StringID finalID = m_table.find(text))
        return finalID;

    uint32_t hash = StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(text));
    for (StringInterningPhase::Storage* storage : m_storages) {
        if (auto provisionalID = useExistingIn(*storage, text, hash))
            return *provisionalID;
    }

    while (m_storages.isEmpty() || m_storages.last()->strings.size() == ProvisionalStringID::maxStringsPerStorage) {
        m_storages.append(&m_phase->leaseStorage());
        if (auto provisionalID = useExistingIn(*m_storages.last(), text, hash))
            return *provisionalID;
    }

    StringInterningPhase::Storage& storage = *m_storages.last();
    uint32_t position = storage.strings.size();
    storage.strings.appendConcurrently(Vector<uint8_t>(text));
    storage.positionsByHash.add(hash, Vector<uint32_t, 1>()).iterator->value.append(position);
    storage.lastLease.append(0);
    return didUse(storage, position);
}

std::optional<StringID> StringArena::useExistingIn(StringInterningPhase::Storage& storage, std::span<const uint8_t> text, uint32_t hash)
{
    auto positions = storage.positionsByHash.find(hash);
    if (positions == storage.positionsByHash.end())
        return std::nullopt;
    for (uint32_t position : positions->value) {
        if (equalSpans(storage.strings[position].span(), text))
            return didUse(storage, position);
    }
    return std::nullopt;
}

StringID StringArena::didUse(StringInterningPhase::Storage& storage, uint32_t position)
{
    StringID id = ProvisionalStringID::encode(storage.number, position);
    if (storage.lastLease[position] != storage.lease) {
        storage.lastLease[position] = storage.lease;
        m_firstUses->append(id);
    }
    return id;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
