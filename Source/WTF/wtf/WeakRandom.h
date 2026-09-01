/*
 * Copyright (C) 2009, 2015 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 *
 * Vigna, Sebastiano (2014). "Further scramblings of Marsaglia's xorshift
 * generators". arXiv:1404.0390 (http://arxiv.org/abs/1404.0390)
 *
 * See also https://en.wikipedia.org/wiki/Xorshift.
 */

#pragma once

#include <limits>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace WTF {

// The code used to generate random numbers are inlined manually in JIT code.
// So it needs to stay in sync with the JIT one.
class WeakRandom final {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(WeakRandom);
public:
    WeakRandom(unsigned seed = cryptographicallyRandomNumber<unsigned>())
    {
        setSeed(seed);
    }

    void setSeed(unsigned seed)
    {
        m_seed = seed;

        // A zero seed would cause an infinite series of zeroes.
        if (!seed)
            seed = 1;

        __atomic_store_n(&m_low, static_cast<uint64_t>(seed), __ATOMIC_RELAXED);
        __atomic_store_n(&m_high, static_cast<uint64_t>(seed), __ATOMIC_RELAXED);
        advance();
    }

    unsigned seed() const { return m_seed; }

    double get()
    {
        uint64_t value = advance() & ((1ULL << 53) - 1);
        return value * (1.0 / (1ULL << 53));
    }

    unsigned getUint32()
    {
        return static_cast<unsigned>(advance());
    }

    unsigned getUint32(unsigned limit)
    {
        if (limit <= 1)
            return 0;
        uint64_t cutoff = (static_cast<uint64_t>(std::numeric_limits<unsigned>::max()) + 1) / limit * limit;
        for (;;) {
            uint64_t value = getUint32();
            if (value >= cutoff)
                continue;
            return value % limit;
        }
    }

    uint64_t getUint64()
    {
        return advance();
    }

    bool returnTrueWithProbability(double probability)
    {
        ASSERT(0.0 <= probability && probability <= 1.0);

        if (!probability)
            return false;

        double value = getUint32();
        if (value <= static_cast<double>(std::numeric_limits<unsigned>::max()) * probability)
            return true;
        return false;
    }

    static constexpr unsigned lowOffset() { return OBJECT_OFFSETOF(WeakRandom, m_low); }
    static constexpr unsigned highOffset() { return OBJECT_OFFSETOF(WeakRandom, m_high); }

    static constexpr uint64_t nextState(uint64_t x, uint64_t y)
    {
        x ^= x << 23;
        x ^= x >> 17;
        x ^= y ^ (y >> 26);
        return x;
    }

    static constexpr uint64_t generate(unsigned seed)
    {
        if (!seed)
            seed = 1;
        uint64_t low = seed;
        uint64_t high = seed;
        high = nextState(low, high);
        return low + high;
    }

private:
    // SCAN-TSAN-REVERIFY / TSAN-TRIAGE §15 relaxed-atomic class: in JSC's
    // shared-memory Thread mode the VM-level WeakRandom is read-modify-written
    // from multiple JS Threads at safepoint-jitter sites (tsan-deep r0 family
    // ('WTF::WeakRandom::advance', 'WTF::WeakRandom::advance'), 268+263
    // reports). The value is non-semantic — a torn or lost-update state simply
    // degrades to a different random seed — so per the §15 ruling for "racy
    // words whose value is non-semantic" the accesses are converted to
    // RELAXED atomics over the existing plain storage. A single-word relaxed
    // load/store compiles to the identical mov/ldr/str the plain access did
    // (flag-off byte-identical preserved; same precedent as
    // PropertyInlineCache.h icConcurrentRelaxed{Load,Store}). m_low/m_high
    // stay plain uint64_t so lowOffset()/highOffset() and the manually-inlined
    // JIT codegen (see the file-top comment) are unchanged; the JIT path is
    // not compiled in the ENABLE_JIT=OFF TSAN config that observes this race.
    uint64_t advance()
    {
        uint64_t x = __atomic_load_n(&m_low, __ATOMIC_RELAXED);
        uint64_t y = __atomic_load_n(&m_high, __ATOMIC_RELAXED);
        __atomic_store_n(&m_low, y, __ATOMIC_RELAXED);
        uint64_t high = nextState(x, y);
        __atomic_store_n(&m_high, high, __ATOMIC_RELAXED);
        return high + y;
    }

    unsigned m_seed;
    uint64_t m_low;
    uint64_t m_high;
};

} // namespace WTF

using WTF::WeakRandom;
