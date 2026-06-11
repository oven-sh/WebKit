/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
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

#include "WriteBarrier.h"
#include <type_traits>
#include <wtf/Atomics.h>

namespace JSC {

class JSCell;
class VM;

// An Auxiliary barrier is a barrier that does not try to reason about the value being stored into
// it, other than interpreting a falsy value as not needing a barrier. It's OK to use this for either
// JSCells or any other kind of data, so long as it responds to operator!().
//
// TSAN-TRIAGE §3.15 (butterfly-words; SPEC-objectmodel §2/§9.5/C4): with
// useJSThreads() the butterfly word (AuxiliaryBarrier<Butterfly*>) is read
// concurrently through the raw 64-bit tag accessors (taggedButterflyWord)
// while installs land here via setWithoutBarrier()/the early-init
// constructor. The spec blesses the VALUE race (stale words are legal; C4
// bounds, §3 re-dispatch), but a plain C++ store racing those loads is UB —
// so stores of atomic-capable T go through a RELAXED atomic store. Relaxed
// stores compile to the same plain MOV/STR on x86-64/arm64, so flag-off
// (useJSThreads=false) semantics and codegen are unchanged. Any ordering
// stronger than relaxed is supplied by the publication protocol around the
// store (pre-escape install N3, nuke+DCAS §3.0/M5), never by this class.
template<typename T>
class AuxiliaryBarrier {
public:
    using Type = T;

    AuxiliaryBarrier() = default;

    template<typename U>
    AuxiliaryBarrier(VM&, JSCell*, U&&);

    template<typename U>
    AuxiliaryBarrier(U&& value, WriteBarrierEarlyInitTag)
    {
        setWithoutBarrier(std::forward<U>(value));
    }

    void clear()
    {
        if constexpr (isAtomicCapable)
            WTF::atomicStore(&m_value, T(), std::memory_order_relaxed);
        else
            m_value = T();
    }

    template<typename U>
    void set(VM&, JSCell*, U&&);

    const T& get() const { return m_value; }

    T* slot() LIFETIME_BOUND { return &m_value; }

    explicit operator bool() const { return !!m_value; }

    template<typename U>
    void setWithoutBarrier(U&& value)
    {
        if constexpr (isAtomicCapable)
            WTF::atomicStore(&m_value, T(std::forward<U>(value)), std::memory_order_relaxed);
        else
            m_value = std::forward<U>(value);
    }

    T operator->() const { return get(); }

private:
    // Word-sized trivially-copyable payloads (Butterfly*, StructureID*,
    // WriteBarrier<...>*, CagedPtr) take the relaxed-atomic store; anything
    // else keeps the plain assignment (single-owner by construction).
    static constexpr bool isAtomicCapable = std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(uint64_t);

    T m_value { };
};

} // namespace JSC
