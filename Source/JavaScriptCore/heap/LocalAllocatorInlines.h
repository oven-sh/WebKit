/*
 * Copyright (C) 2018-2021 Apple Inc. All rights reserved.
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

#include "HeapInlines.h"
#include "LocalAllocator.h"

namespace JSC {

// Workaround crash in Clang Analyzer when ALWAYS_INLINE_LAMBDA is in use here.
#if defined(__clang_analyzer__) && defined(ALWAYS_INLINE_LAMBDA)
#undef ALWAYS_INLINE_LAMBDA
#define ALWAYS_INLINE_LAMBDA
#endif

ALWAYS_INLINE void* LocalAllocator::allocate(JSC::Heap& heap, size_t cellSize, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    // SharedGC (T9): any-client OK — heap.vm() is the main VM (deviation 3)
    // from every client incl. standalone (§12.1 allocateForClient routes
    // here with the SERVER heap). verifyCanGC() reads validation flags only;
    // sanitizeStackForVM() in the slow-path lambda self-guards (no-op unless
    // the CALLING thread holds the main VM's API lock), so secondary /
    // standalone client threads skip it.
    VM& vm = heap.vm();
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();
    return m_freeList.allocateWithCellSize(
        [&]() ALWAYS_INLINE_LAMBDA {
            sanitizeStackForVM(vm);
            return static_cast<HeapCell*>(allocateSlowCase(heap, cellSize, deferralContext, failureMode));
        }, cellSize);
}

#if defined(__clang_analyzer__) && defined(ALWAYS_INLINE_LAMBDA)
#undef ALWAYS_INLINE_LAMBDA
#define ALWAYS_INLINE_LAMBDA __attribute__((__always_inline__))
#endif

} // namespace JSC

