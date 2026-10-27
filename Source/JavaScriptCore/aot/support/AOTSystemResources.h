/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/PageBlock.h>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

// Threads and pages that a build needs. When the system has none left, the process prints an error and exits with code 1.

// The thread has a stack of ParallelLoops::workerStackSize.
JS_EXPORT_PRIVATE Ref<Thread> startBuildThread(ASCIILiteral name, Function<void()>&&);

// The pages are zero-filled. `bytes` is a multiple of the page size.
void* allocatePages(size_t bytes);
void* reservePages(size_t bytes);
// The most that one reservation takes of the address space: `wanted`, or less where the process has a limit on its address space.
size_t bytesToReserve(size_t wanted);
[[noreturn]] void exitOutOfMemory();
void commitPages(void* pages, size_t bytes);
// Takes everything that one call of allocatePages or reservePages returned.
void releasePages(void* pages, size_t bytes);

// An array of zeros. A page of it takes memory when it is first written to.
template<typename T>
class ZeroFilledPages {
    WTF_MAKE_NONCOPYABLE(ZeroFilledPages);
    static_assert(std::is_trivially_destructible_v<T>);
public:
    ZeroFilledPages() = default;
    explicit ZeroFilledPages(size_t size)
        : m_bytes(roundUpToMultipleOf(pageSize(), size * sizeof(T)))
        , m_elements(m_bytes ? static_cast<T*>(allocatePages(m_bytes)) : nullptr)
    {
    }
    ZeroFilledPages(ZeroFilledPages&& other)
        : m_bytes(std::exchange(other.m_bytes, 0))
        , m_elements(std::exchange(other.m_elements, nullptr))
    {
    }
    ~ZeroFilledPages()
    {
        if (m_elements)
            releasePages(m_elements, m_bytes);
    }

    ZeroFilledPages& operator=(ZeroFilledPages&& other)
    {
        std::swap(m_bytes, other.m_bytes);
        std::swap(m_elements, other.m_elements);
        return *this;
    }

    T& operator[](size_t index) const LIFETIME_BOUND { return m_elements[index]; }

private:
    size_t m_bytes { 0 };
    T* m_elements { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
