/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RaceAmplifier.h"

#include "Options.h"
#include <atomic>
#include <mutex>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/DataLog.h>
#include <wtf/Seconds.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>

namespace JSC {

unsigned RaceAmplifier::s_period = 0;
uint64_t RaceAmplifier::s_seed = 0;
unsigned RaceAmplifier::s_maxSleepMicroseconds = 100;

namespace {

// Per-thread perturbation state. Each thread draws from its own WeakRandom,
// seeded from the process-wide seed mixed with a per-thread ordinal (NOT the
// OS thread id, so a given seed produces the same per-thread decision streams
// across runs as long as thread creation order is stable).
struct ThreadState {
    WeakRandom random;
    unsigned countdown { 0 };
    bool initialized { false };
};

static thread_local ThreadState s_threadState;

static std::atomic<uint64_t> s_threadOrdinal { 0 };

// SplitMix64 finalizer; cheap, full-avalanche mix of seed and ordinal.
static uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

} // anonymous namespace

void RaceAmplifier::initialize()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        unsigned period = Options::randomYieldPeriod();
        if (!period)
            return; // Disabled; s_period stays 0 and perturb() stays free.

        // The seed is kept to 32 bits so the logged value round-trips through
        // the Unsigned --randomYieldSeed option exactly.
        uint64_t seed = Options::randomYieldSeed();
        if (!seed) {
            seed = cryptographicallyRandomNumber<uint32_t>();
            if (!seed)
                seed = 1;
        }

        s_seed = seed;
        s_maxSleepMicroseconds = std::max(1u, Options::randomYieldMaxMicroseconds());

        // Log the effective seed so any run can be reproduced with
        // --randomYieldSeed=<seed>.
        dataLogLn("[RaceAmplifier] enabled: period=", period, " seed=", seed, " maxSleepUs=", s_maxSleepMicroseconds);

        // Publish the period last: threads observing nonzero s_period must
        // see the seed. perturbSlow() loads s_seed only after the s_period
        // check, and this store provides the release.
        std::atomic_thread_fence(std::memory_order_release);
        s_period = period;
    });
}

void RaceAmplifier::perturbSlow()
{
    ThreadState& state = s_threadState;

    if (!state.initialized) [[unlikely]] {
        uint64_t ordinal = s_threadOrdinal.fetch_add(1, std::memory_order_relaxed);
        uint64_t mixed = mix(s_seed ^ mix(ordinal));
        // WeakRandom takes a 32-bit seed; fold the full 64-bit mix into it.
        state.random.setSeed(static_cast<unsigned>(mixed ^ (mixed >> 32)));
        state.initialized = true;
        // Stagger first firings across threads: countdown in [1, period].
        state.countdown = 1 + state.random.getUint32(s_period);
    }

    if (--state.countdown)
        return;

    // Re-arm with a fresh countdown in [1, 2 * period) so the average firing
    // interval is ~period but the phase keeps drifting between threads —
    // fixed periods can resonate with loop trip counts and hide races.
    state.countdown = 1 + state.random.getUint32(2 * s_period - 1);

    // 3/4 of firings are a bare yield (fine-grained interleaving churn);
    // 1/4 are a short sleep (parks the thread long enough for another thread
    // to run a whole slow path through the window we are sitting in).
    if (state.random.getUint32(4)) {
        Thread::yield();
        return;
    }

    unsigned microseconds = 1 + state.random.getUint32(s_maxSleepMicroseconds);
    sleep(Seconds::fromMicroseconds(microseconds));
}

} // namespace JSC
