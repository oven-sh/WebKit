/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
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

#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/PtrTag.h>
#include <wtf/RawPtrTraits.h>

#include <climits>
#include <wtf/Atomics.h>

namespace WTF {

template<Gigacage::Kind passedKind, typename T, typename PtrTraits = RawPtrTraits<T>>
class CagedPtr {
public:
    static constexpr Gigacage::Kind kind = passedKind;

    // THREADS/TSAN: constructor stores are relaxed atomics too — a CagedPtr
    // embedded in a GC cell can be constructed at a recycled address that
    // stale readers on other threads still probe with atomic loads.
    CagedPtr() : CagedPtr(nullptr) { }
    CagedPtr(std::nullptr_t)
    {
        typename PtrTraits::StorageType storage { nullptr };
        WTF::atomicStore(&m_ptr, storage, std::memory_order_relaxed);
    }

    CagedPtr(T* ptr)
    {
        typename PtrTraits::StorageType storage { ptr };
        WTF::atomicStore(&m_ptr, storage, std::memory_order_relaxed);
    }

    // THREADS/TSAN: the storage word is read and written with relaxed atomics —
    // a detaching/resizing thread may legally race readers on other threads
    // (JSC ArrayBuffer detach/transfer under shared-heap Threads); staleness is
    // handled by the callers' protocols, the atomics only make the accesses
    // defined. Codegen is a plain mov on x86-64/arm64.
    typename PtrTraits::StorageType loadStorageRelaxed() const
    {
        return WTF::atomicLoad(const_cast<typename PtrTraits::StorageType*>(&m_ptr), std::memory_order_relaxed);
    }

    T* get() const LIFETIME_BOUND
    {
        typename PtrTraits::StorageType storage = loadStorageRelaxed();
        ASSERT(storage);
        T* ptr = PtrTraits::unwrap(storage);
        return Gigacage::caged(kind, ptr);
    }

    T* getMayBeNull() const LIFETIME_BOUND
    {
        T* ptr = PtrTraits::unwrap(loadStorageRelaxed());
        if (!ptr)
            return nullptr;
        return Gigacage::caged(kind, ptr);
    }

    T* getUnsafe() const LIFETIME_BOUND
    {
        T* ptr = PtrTraits::unwrap(loadStorageRelaxed());
        return Gigacage::cagedMayBeNull(kind, ptr);
    }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    // We need the template here so that the type of U is deduced at usage time rather than class time. U should always be T.
    template<typename U = T>
        requires (!std::same_as<void, U>)
    WTF_UNSAFE_BUFFER_USAGE U& at(size_t index) const { return get()[index]; }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    CagedPtr(CagedPtr& other)
    {
        WTF::atomicStore(&m_ptr, other.loadStorageRelaxed(), std::memory_order_relaxed);
    }

    CagedPtr& operator=(const CagedPtr& ptr)
    {
        WTF::atomicStore(&m_ptr, ptr.loadStorageRelaxed(), std::memory_order_relaxed);
        return *this;
    }

    CagedPtr(CagedPtr&& other)
    {
        WTF::atomicStore(&m_ptr, PtrTraits::exchange(other.m_ptr, nullptr), std::memory_order_relaxed);
    }

    CagedPtr& operator=(CagedPtr&& ptr)
    {
        WTF::atomicStore(&m_ptr, PtrTraits::exchange(ptr.m_ptr, nullptr), std::memory_order_relaxed);
        return *this;
    }

    bool operator==(const CagedPtr& other) const
    {
        bool result = loadStorageRelaxed() == other.loadStorageRelaxed();
        ASSERT(result == (getUnsafe() == other.getUnsafe()));
        return result;
    }
    
    explicit operator bool() const
    {
        return getUnsafe() != nullptr;
    }

    T* rawBits() const
    {
        return std::bit_cast<T*>(loadStorageRelaxed());
    }
    
protected:
    typename PtrTraits::StorageType m_ptr;
};

} // namespace WTF

using WTF::CagedPtr;

