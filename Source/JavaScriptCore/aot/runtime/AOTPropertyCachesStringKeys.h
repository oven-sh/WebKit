/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include "RememberedSlot.h"
#include <array>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct State;

class StringKeyCache {
    WTF_MAKE_NONCOPYABLE(StringKeyCache);
public:
    static constexpr size_t maxCaseConvertedStringLength = 64;

    explicit StringKeyCache(State&);

    size_t maxCachedKeyLength() const { return m_maxCachedKeyLength; }
    JSString* findOrAdd(VM&, std::span<const Latin1Character>);
    JSString* tryFindCachedKey(std::span<const Latin1Character>, unsigned hash) const;
    JSString* makeKey(VM&, std::span<const Latin1Character>, unsigned hash);
    JSString* caseConvertedString(VM&, std::span<const Latin1Character>);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    static size_t firstIndex(unsigned hash, size_t tableSize) { return hash & (tableSize - 1) & ~static_cast<size_t>(1); }
    JSString* matchingKey(uint64_t entry, std::span<const Latin1Character>) const;
    size_t removeDeadEntries(VM&, std::span<uint64_t>);

    uintptr_t m_cageBase;
    size_t m_maxCachedKeyLength;
    Vector<uint64_t> m_cachedKeys;
    size_t m_cachedKeyEvictionCount { 0 };
    std::array<uint64_t, 256> m_caseConvertedStrings { };
    RememberedSlotSet m_writtenSinceCollection;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
