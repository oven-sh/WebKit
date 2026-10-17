/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3StackmapValue.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Procedure;

class ProcedureFingerprint {
public:
    JS_EXPORT_PRIVATE bool compute(Procedure&, std::span<StackmapGenerator* const> distinguishedGenerators, uint32_t clientBits);

    bool operator==(const ProcedureFingerprint& other) const { return m_hash == other.m_hash && m_words == other.m_words; }

    const Vector<RefPtr<StackmapGenerator>, 8>& generators() const LIFETIME_BOUND { return m_generators; }
    size_t findGenerator(const StackmapGenerator* generator) const
    {
        return m_generators.findIf([&](auto& other) {
            return other.get() == generator;
        });
    }
    void clearGenerators() { m_generators.clear(); }

private:
    static constexpr size_t maxWordsInRep = 3;
    static constexpr size_t maxWordsInValueWithoutLists = 96;

    void reserve(size_t numberOfWords)
    {
        if (m_size + numberOfWords > m_words.size()) [[unlikely]]
            m_words.grow(std::max(m_words.size() * 2, m_size + numberOfWords));
    }
    ALWAYS_INLINE void append(uint32_t word) { m_words.mutableSpan().data()[m_size++] = word; }
    ALWAYS_INLINE void append64(uint64_t word)
    {
        append(static_cast<uint32_t>(word));
        append(static_cast<uint32_t>(word >> 32));
    }
    void append(const HeapRange&);
    void append(const Effects&);
    void append(const RegisterSet&);
    void append(const ValueRep&);
    bool appendValue(Value*, std::span<StackmapGenerator* const> distinguishedGenerators);
    void appendStackmap(StackmapValue*, std::span<StackmapGenerator* const> distinguishedGenerators);

    Vector<uint32_t, 512> m_words;
    size_t m_size { 0 };
    Vector<RefPtr<StackmapGenerator>, 8> m_generators;
    uint64_t m_hash { 0 };
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
