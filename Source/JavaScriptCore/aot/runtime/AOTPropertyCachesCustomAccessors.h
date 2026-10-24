/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeRecords.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class VM;

namespace AOT {

class CacheRegistry;

class CustomAccessorTable {
    WTF_MAKE_NONCOPYABLE(CustomAccessorTable);
public:
    CustomAccessorTable(VM&, CacheRegistry&);
    ~CustomAccessorTable();

    const CustomAccessorRecord* records() const { return m_records; }
    const CustomAccessorRecord& at(uint32_t index) const
    {
        ASSERT(isInUse(index));
        return m_records[index];
    }
    bool isInUse(uint32_t index) const { return index < m_numberOfRecordsEverUsed && m_records[index].function; }

    std::optional<uint32_t> find(const CustomAccessorRecord&) const;
    std::optional<uint32_t> add(const CustomAccessorRecord&);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using Key = std::tuple<void*, JSObject*, JSGlobalObject*, uint32_t>;
    static Key keyOf(const CustomAccessorRecord& record) { return { record.function, record.slotBase, record.globalObject, record.offsetOfCachedValue }; }

    VM& m_vm;
    CustomAccessorRecord* m_records { nullptr };
    uint32_t m_capacity { 0 };
    uint32_t m_numberOfRecordsEverUsed { 0 };
    Vector<uint32_t> m_freeIndexes;
    UncheckedKeyHashMap<Key, uint32_t> m_indexByKey;
    RememberedSlotSet m_writtenSinceCollection;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
