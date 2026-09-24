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
#include <wtf/Assertions.h>

namespace JSC {

// The reference count of an object that mutators of different threads reference only when the JS threads flag is on. The
// flag off it is what the object had on `main`: a count that is loaded and stored, without a locked instruction, because
// every reference is taken and dropped by the one mutator (or the collector at a stop). The flag on it is a relaxed atomic
// increment, a release decrement and an acquire fence before the object is destroyed, as WTF::ThreadSafeRefCounted.
//
// The mode is a process-wide byte that is fixed before the first VM exists: an object is never counted in both ways.
class ThreadsModeRefCountedBase {
public:
    void ref() const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            m_refCount.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_refCount.store(m_refCount.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    bool hasOneRef() const { return refCount() == 1; }
    uint32_t refCount() const { return m_refCount.load(std::memory_order_relaxed); }

    // WTF::ThreadSafeRefCounted's debug hook; nothing is checked here.
    void disableThreadingChecks() { }

protected:
    ThreadsModeRefCountedBase() = default;
    ~ThreadsModeRefCountedBase() = default;

    // True if the object is to be destroyed.
    bool derefBase() const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            if (m_refCount.fetch_sub(1, std::memory_order_release) != 1)
                return false;
            std::atomic_thread_fence(std::memory_order_acquire);
        } else {
            uint32_t count = m_refCount.load(std::memory_order_relaxed) - 1;
            m_refCount.store(count, std::memory_order_relaxed);
            if (count)
                return false;
        }
        m_refCount.store(1, std::memory_order_relaxed);
        return true;
    }

private:
    mutable std::atomic<uint32_t> m_refCount { 1 };
};

template<typename T>
class ThreadsModeRefCounted : public ThreadsModeRefCountedBase {
public:
    void deref() const
    {
        if (derefBase())
            delete static_cast<const T*>(this);
    }

protected:
    ThreadsModeRefCounted() = default;
    ~ThreadsModeRefCounted() = default;
};

} // namespace JSC
