/*
 * Copyright (C) 2011 Apple Inc. All rights reserved.
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

#include <wtf/Atomics.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

// Concurrency (TSAN triage 3.16, SPEC-objectmodel §5): add() (e.g. Structure
// transition-table insert under the owner's lock) races with lock-free
// ruleOut() from concurrent transition lookup / getConcurrently(). Bloom
// false positives/negatives only steer callers to a re-validating slow path,
// so stale values are fine — but a torn plain load/store of m_bits would be
// UB. m_bits is therefore a relaxed WTF::Atomic word: same layout, relaxed
// loads/stores compile to plain loads/stores, and add() uses a relaxed
// fetch_or per the triage ruling (insert is the rare path). No ordering is
// implied. JIT code reads the word directly via offsetOfBits(), which is
// unchanged because Atomic<Bits> wraps a single std::atomic<Bits>.
template <typename Bits = uintptr_t>
class TinyBloomFilter {
public:
    TinyBloomFilter() = default;
    TinyBloomFilter(Bits);
    TinyBloomFilter(const TinyBloomFilter& other) { m_bits.storeRelaxed(other.m_bits.loadRelaxed()); }
    TinyBloomFilter& operator=(const TinyBloomFilter& other)
    {
        m_bits.storeRelaxed(other.m_bits.loadRelaxed());
        return *this;
    }

    void add(Bits);
    void add(TinyBloomFilter&);
    bool ruleOut(Bits) const; // True for 0.
    void reset();
    Bits bits() const { return m_bits.loadRelaxed(); }

    static constexpr ptrdiff_t offsetOfBits()
    {
        static_assert(sizeof(WTF::Atomic<Bits>) == sizeof(Bits));
        return OBJECT_OFFSETOF(TinyBloomFilter, m_bits);
    }

private:
    WTF::Atomic<Bits> m_bits { 0 };
};

template <typename Bits>
inline TinyBloomFilter<Bits>::TinyBloomFilter(Bits bits)
    : m_bits { bits }
{
}

template <typename Bits>
inline void TinyBloomFilter<Bits>::add(Bits bits)
{
    m_bits.exchangeOr(bits, std::memory_order_relaxed);
}

template <typename Bits>
inline void TinyBloomFilter<Bits>::add(TinyBloomFilter& other)
{
    m_bits.exchangeOr(other.m_bits.loadRelaxed(), std::memory_order_relaxed);
}

template <typename Bits>
inline bool TinyBloomFilter<Bits>::ruleOut(Bits bits) const
{
    if (!bits)
        return true;

    if ((bits & m_bits.loadRelaxed()) != bits)
        return true;

    return false;
}

template <typename Bits>
inline void TinyBloomFilter<Bits>::reset()
{
    m_bits.storeRelaxed(0);
}

} // namespace JSC
