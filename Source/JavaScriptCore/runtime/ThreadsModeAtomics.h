/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 */

#pragma once

#include "Options.h"
#include <atomic>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/Locker.h>

namespace JSC {

// Read-modify-writes and locks that only mutators of different threads need, and that `main` does without: with the JS threads
// flag off they are `main`'s plain load and store (or no lock), with it on they are the atomic RMW (or the lock). One predicted
// byte test of the process-wide, frozen option; no locked instruction and no lock acquisition in a process that never sets the flag.

// Profile words (arithmetic, array and value profiles, the inline caches' advisory cells) are written by mutators, and read by the
// compiler threads, without synchronization: a lost update self-heals (I12). Several threads running one CodeBlock's lower tiers
// make an unconditional store keep the cache line moving between cores even when it records nothing new, so with the flag on a
// store is skipped when the word already holds the value ("write avoidance", SPEC-ungil section 5.7; the same condition as the
// Baseline emitter's). Without the flag one thread writes the profile and the store is `main`'s: an unconditional plain `or` or `mov`,
// with no load, compare and branch in front of it.
ALWAYS_INLINE bool sharedProfileWriteAvoidance()
{
    return processUsesJSThreads() && Options::useSharedProfileWriteAvoidance();
}

template<typename T, typename U>
ALWAYS_INLINE void racyOrProfileWord(T& location, U bits)
{
    if (sharedProfileWriteAvoidance()) [[unlikely]] {
        T current = WTF::racyLoad(location);
        if (static_cast<T>(current | bits) != current)
            WTF::racyStore(location, static_cast<T>(current | bits));
        return;
    }
    WTF::racyStore(location, static_cast<T>(WTF::racyLoad(location) | bits));
}

template<typename T, typename U>
ALWAYS_INLINE void racyStoreProfileWord(T& location, U value)
{
    if (sharedProfileWriteAvoidance()) [[unlikely]] {
        if (WTF::racyLoad(location) == static_cast<T>(value))
            return;
    }
    WTF::racyStore(location, static_cast<T>(value));
}

template<typename T>
ALWAYS_INLINE T threadsModeFetchAddRelaxed(std::atomic<T>& location, T operand)
{
    if (processUsesJSThreads()) [[unlikely]]
        return location.fetch_add(operand, std::memory_order_relaxed);
    T old = location.load(std::memory_order_relaxed);
    location.store(static_cast<T>(old + operand), std::memory_order_relaxed);
    return old;
}

template<typename T>
ALWAYS_INLINE T threadsModeFetchSubRelaxed(std::atomic<T>& location, T operand)
{
    if (processUsesJSThreads()) [[unlikely]]
        return location.fetch_sub(operand, std::memory_order_relaxed);
    T old = location.load(std::memory_order_relaxed);
    location.store(static_cast<T>(old - operand), std::memory_order_relaxed);
    return old;
}

template<typename T>
ALWAYS_INLINE T threadsModeExchangeRelaxed(std::atomic<T>& location, T desired)
{
    if (processUsesJSThreads()) [[unlikely]]
        return location.exchange(desired, std::memory_order_relaxed);
    T old = location.load(std::memory_order_relaxed);
    location.store(desired, std::memory_order_relaxed);
    return old;
}

// A scoped lock taken only with the flag on.
template<typename LockType>
class ThreadsModeLocker {
    WTF_MAKE_NONCOPYABLE(ThreadsModeLocker);
public:
    explicit ThreadsModeLocker(LockType& lock)
    {
        if (processUsesJSThreads()) [[unlikely]]
            m_locker.emplace(lock);
    }

private:
    std::optional<Locker<LockType>> m_locker;
};

// The same for a lock that the clients of a shared collector need without the JS threads flag: the shared-heap test harness runs
// several clients of one heap in a process that has useSharedGCHeap and not useJSThreads.
template<typename LockType>
class SharedHeapModeLocker {
    WTF_MAKE_NONCOPYABLE(SharedHeapModeLocker);
public:
    explicit SharedHeapModeLocker(LockType& lock)
    {
        if (processUsesSharedGCHeap() || processUsesJSThreads()) [[unlikely]]
            m_locker.emplace(lock);
    }

private:
    std::optional<Locker<LockType>> m_locker;
};

} // namespace JSC
