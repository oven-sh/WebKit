/*
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
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

#include <atomic>
#include <wtf/Assertions.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace WTF {

// A variant of RefCounted that allows reference counting to be deferred,
// and can tell you if that has happened. You can think of a deferral as
// just being an additional "ref", except that you can detect if it has
// specifically happened - this can be useful either for debugging, or
// sometimes even for some additional functionality.
//
// The count is atomic (ThreadSafeRefCounted-style ordering): the only user
// of this class is JSC::ArrayBuffer (via GCIncomingRefCounted), whose
// wrappers are reachable from every thread through the shared JS heap once
// shared-memory Threads are on — RefPtr<ArrayBuffer> is taken/dropped on the
// C++ side from concurrent mutators (DataView/slice/structuredClone/wasm
// memory) and from per-thread sweeps, so a plain count would be corruptible
// (premature free / leak). The single-threaded cost is one uncontended
// lock-prefixed RMW per ref/deref on a cold path; not worth a mode split.

class DeferrableRefCountedBase {
    static constexpr uint32_t deferredFlag = 1;
    static constexpr uint32_t normalIncrement = 2;

public:
    void ref() const
    {
        m_refCount.fetch_add(normalIncrement, std::memory_order_relaxed);
    }

    bool hasOneRef() const
    {
        return refCount() == 1;
    }

    uint32_t refCount() const
    {
        return m_refCount.load(std::memory_order_relaxed) / normalIncrement;
    }

    bool isDeferred() const
    {
        return !!(m_refCount.load(std::memory_order_relaxed) & deferredFlag);
    }

protected:
    DeferrableRefCountedBase()
        : m_refCount(normalIncrement)
    {
    }

    ~DeferrableRefCountedBase()
    {
    }

    bool derefBase() const
    {
        // acq_rel: the release publishes this thread's writes to the object
        // for whichever thread performs the final deref; the acquire on the
        // final deref orders the delete after every other thread's release.
        return m_refCount.fetch_sub(normalIncrement, std::memory_order_acq_rel) == normalIncrement;
    }

    bool setIsDeferredBase(bool value)
    {
        if (value) {
            m_refCount.fetch_or(deferredFlag, std::memory_order_acq_rel);
            return false;
        }
        return (m_refCount.fetch_and(~deferredFlag, std::memory_order_acq_rel) & ~deferredFlag) == 0;
    }

private:
    mutable std::atomic<uint32_t> m_refCount;
};

template<typename T>
class DeferrableRefCounted : public DeferrableRefCountedBase {
    WTF_MAKE_NONCOPYABLE(DeferrableRefCounted); WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DeferrableRefCounted);
public:
    void deref() const
    {
        if (derefBase())
            delete static_cast<const T*>(this);
    }

    bool setIsDeferred(bool value)
    {
        if (!setIsDeferredBase(value))
            return false;
        delete static_cast<T*>(this);
        return true;
    }

protected:
    DeferrableRefCounted() { }
    ~DeferrableRefCounted() { }
};

} // namespace WTF

using WTF::DeferrableRefCounted;
