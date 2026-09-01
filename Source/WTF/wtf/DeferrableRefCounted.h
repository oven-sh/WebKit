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
#include <wtf/WTFConfig.h>

namespace WTF {

// A variant of RefCounted that allows reference counting to be deferred,
// and can tell you if that has happened. You can think of a deferral as
// just being an additional "ref", except that you can detect if it has
// specifically happened - this can be useful either for debugging, or
// sometimes even for some additional functionality.
//
// The only user is JSC::ArrayBuffer (via GCIncomingRefCounted). Its count is
// ref'd/deref'd without a lock from concurrently running mutators only when
// JSC runs its JS threads without the GIL; JSC latches
// g_wtfConfig.useAtomicDeferrableRefCount for the process in that shape,
// before the first VM is published, and the count then uses atomic RMWs
// (ThreadSafeRefCounted-style ordering). Otherwise every ref/deref is
// serialized by the JSLock and the count stays a plain load/store: taking a
// RefPtr<ArrayBuffer> is on the hot path of every typed-array view created
// over an existing buffer (subarray, new TypedArray(buffer), DataView).

class DeferrableRefCountedBase {
    static constexpr uint32_t deferredFlag = 1;
    static constexpr uint32_t normalIncrement = 2;

public:
    void ref() const
    {
        if (g_wtfConfig.useAtomicDeferrableRefCount) [[unlikely]] {
            m_refCount.fetch_add(normalIncrement, std::memory_order_relaxed);
            return;
        }
        m_refCount.store(m_refCount.load(std::memory_order_relaxed) + normalIncrement, std::memory_order_relaxed);
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
        if (g_wtfConfig.useAtomicDeferrableRefCount) [[unlikely]] {
            // acq_rel: the release publishes this thread's writes to the object
            // for whichever thread performs the final deref; the acquire on the
            // final deref orders the delete after every other thread's release.
            return m_refCount.fetch_sub(normalIncrement, std::memory_order_acq_rel) == normalIncrement;
        }
        uint32_t newValue = m_refCount.load(std::memory_order_relaxed) - normalIncrement;
        m_refCount.store(newValue, std::memory_order_relaxed);
        return !newValue;
    }

    bool setIsDeferredBase(bool value)
    {
        if (g_wtfConfig.useAtomicDeferrableRefCount) [[unlikely]] {
            if (value) {
                m_refCount.fetch_or(deferredFlag, std::memory_order_acq_rel);
                return false;
            }
            return (m_refCount.fetch_and(~deferredFlag, std::memory_order_acq_rel) & ~deferredFlag) == 0;
        }
        uint32_t newValue = m_refCount.load(std::memory_order_relaxed);
        if (value) {
            m_refCount.store(newValue | deferredFlag, std::memory_order_relaxed);
            return false;
        }
        newValue &= ~deferredFlag;
        m_refCount.store(newValue, std::memory_order_relaxed);
        return !newValue;
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
