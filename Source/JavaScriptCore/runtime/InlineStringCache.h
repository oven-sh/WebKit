/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include <array>
#include <cstdint>
#include <wtf/RefPtr.h>
#include <wtf/text/AtomStringImpl.h>

namespace JSC {

class JSString;

// Direct-mapped JSString* cache keyed by the inline-encoding fiber word.
// The fiber word is content-unique for 16-byte inline strings (lengths 2..7
// Latin-1 / 1..3 UTF-16), so a single word compare is the full equality test.
// Weak: cleared at every GC finalize alongside KeyAtomStringCache.
class InlineStringCache {
public:
    static constexpr unsigned capacity = 512;

    ALWAYS_INLINE JSString* lookup(uintptr_t fiber) const
    {
        unsigned index = indexFor(fiber);
        if (m_keys[index] == fiber)
            return m_values[index];
        return nullptr;
    }

    ALWAYS_INLINE void insert(uintptr_t fiber, JSString* string)
    {
        unsigned index = indexFor(fiber);
        m_keys[index] = fiber;
        m_values[index] = string;
    }

    ALWAYS_INLINE void clear()
    {
        m_keys.fill(0);
        m_values.fill(nullptr);
    }

private:
    ALWAYS_INLINE static unsigned indexFor(uintptr_t fiber)
    {
        // Low byte is flags+length; payload starts at byte 1. Fold the word
        // once so chars 5..7 contribute to the index for ≤7-char Latin-1 keys.
        uintptr_t mixed = fiber ^ (fiber >> 32);
        return static_cast<unsigned>((mixed >> 8) % capacity);
    }

    std::array<uintptr_t, capacity> m_keys { };
    std::array<JSString*, capacity> m_values { };
};

// Direct-mapped AtomStringImpl* cache keyed by the inline fiber word, so
// Identifier::add/fromString for repeated short names skips the atom-table
// hash+probe. Holds a ref per slot; cleared on full GC.
class InlineAtomCache {
public:
    static constexpr unsigned capacity = 512;

    ALWAYS_INLINE AtomStringImpl* lookup(uintptr_t fiber) const
    {
        unsigned index = indexFor(fiber);
        if (m_keys[index] == fiber)
            return m_values[index].get();
        return nullptr;
    }

    ALWAYS_INLINE void insert(uintptr_t fiber, AtomStringImpl* atom)
    {
        unsigned index = indexFor(fiber);
        m_keys[index] = fiber;
        m_values[index] = atom;
    }

    ALWAYS_INLINE void clear()
    {
        m_keys.fill(0);
        for (auto& slot : m_values)
            slot = nullptr;
    }

private:
    ALWAYS_INLINE static unsigned indexFor(uintptr_t fiber)
    {
        uintptr_t mixed = fiber ^ (fiber >> 32);
        return static_cast<unsigned>((mixed >> 8) % capacity);
    }

    std::array<uintptr_t, capacity> m_keys { };
    std::array<RefPtr<AtomStringImpl>, capacity> m_values { };
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
