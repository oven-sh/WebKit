/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStoragePool.h"
#include "AOTStringTable.h"
#include "AOTVector.h"
#include <atomic>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/ConcurrentVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

struct ProvisionalStringID {
    static constexpr uint32_t provisionalBit = StringTable::provisionalBit;
    static constexpr unsigned storageShift = 20;
    static constexpr uint32_t maxStorages = (provisionalBit >> storageShift) - 1;
    static constexpr uint32_t maxStringsPerStorage = 1u << storageShift;

    static constexpr bool isProvisional(StringID id) { return StringTable::isProvisional(id); }
    static constexpr StringID encode(uint32_t storage, uint32_t position) { return StringID(provisionalBit | storage << storageShift | position); }
    static constexpr uint32_t storageOf(StringID id) { return (id.index() & ~provisionalBit) >> storageShift; }
    static constexpr uint32_t positionOf(StringID id) { return id.index() & (maxStringsPerStorage - 1); }
};

class StringRemap {
public:
    StringID operator()(StringID id) const { return ProvisionalStringID::isProvisional(id) ? finalIDOf(id) : id; }
    void apply(StringID& id) const { id = (*this)(id); }

    bool isIdentity() const { return m_isIdentity; }
    bool isIdentityFor(unsigned task) const { return !m_tasksWithNewStrings.get(task); }

private:
    friend class StringInterningPhase;

    StringID finalIDOf(StringID provisional) const;

    Vector<Vector<StringID>> m_finalIDs;
    BitVector m_tasksWithNewStrings;
    bool m_isIdentity { true };
};

class StringInterningPhase {
    WTF_MAKE_NONCOPYABLE(StringInterningPhase);
public:
    StringInterningPhase(StringTable&, unsigned numberOfTasks);
    ~StringInterningPhase();

    unsigned addTasks(unsigned count);
    StringRemap finish();
    StringRemap finish(std::span<const unsigned> taskOrder);
    void discard();

    std::span<const uint8_t> textOf(StringID provisional) const;

private:
    friend class StringArena;

    struct Storage {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Storage);

        explicit Storage(uint32_t number)
            : number(number)
        {
        }

        uint32_t number;
        uint32_t lease { 0 };
        std::atomic<bool> isLeased { true };
        ConcurrentVector<Vector<uint8_t>, 64> strings;
        // The key is wider than a hash, so that no hash is the empty or the deleted key.
        UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> positionsByHash;
        Vector<uint32_t> lastLease;
    };

    Storage& leaseStorage();

    StringTable& m_table;
    StoragePool<Storage, ProvisionalStringID::maxStorages> m_storages;
    Lock m_firstUsesGrowthLock;
    ConcurrentVector<Vector<StringID>, 64> m_firstUses;
    bool m_isOpen { true };
};

class StringArena {
    WTF_MAKE_NONCOPYABLE(StringArena);
public:
    explicit StringArena(StringTable&);
    StringArena(StringInterningPhase&, unsigned task);
    ~StringArena();

    StringID intern(StringView);
    StringID intern(std::span<const uint8_t> canonicalProgramString);
    std::span<const uint8_t> at(StringID id) const { return m_table[id]; }
    const StringTable& table() const { return m_table; }
    bool hasUsedProvisionalStrings() const { return m_firstUses && !m_firstUses->isEmpty(); }

private:
    std::optional<StringID> useExistingIn(StringInterningPhase::Storage&, std::span<const uint8_t>, uint32_t hash);
    StringID didUse(StringInterningPhase::Storage&, uint32_t position);

    StringTable& m_table;
    StringInterningPhase* m_phase { nullptr };
    Vector<StringID>* m_firstUses { nullptr };
    Vector<StringInterningPhase::Storage*, 1> m_storages;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
