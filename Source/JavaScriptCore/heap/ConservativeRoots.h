/*
 * Copyright (C) 2009-2024 Apple Inc. All rights reserved.
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

#include "Heap.h"

#if ASAN_ENABLED && __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#define JSC_CONSERVATIVE_SCAN_SKIPS_POISONED_WORDS 1
#else
#define JSC_CONSERVATIVE_SCAN_SKIPS_POISONED_WORDS 0
#endif

namespace JSC {

class CodeBlockSet;
class HeapCell;
class JITStubRoutineSet;

// A word ASan has poisoned is one the program may not touch: the redzone between two locals of an instrumented frame,
// a local whose scope has ended, the unused capacity of an annotated container. Nothing can have put a live value
// there, but whatever an earlier frame left in that stack memory is still in it, and a redzone is never written, so
// under a frame that stays on the stack (MicrotaskQueue::drain, for the whole of a module's top-level-await body) a
// stale cell pointer in one was a root at every collection. A pointer needs all of its bytes addressable.
// Asked of the stack itself: a copy of it (MachineThreads::tryCopyOtherThreadStack) has no poison of its own.
ALWAYS_INLINE bool isPoisonedForConservativeScan(const void* word)
{
#if JSC_CONSERVATIVE_SCAN_SKIPS_POISONED_WORDS
    return __asan_region_is_poisoned(const_cast<void*>(word), sizeof(void*));
#else
    UNUSED_PARAM(word);
    return false;
#endif
}

class ConservativeRoots {
public:
    ConservativeRoots(Heap&);
    ~ConservativeRoots();

    void add(void* begin, void* end);
    void add(void* begin, void* end, JITStubRoutineSet&, CodeBlockSet&);
    
    size_t size() const;
    HeapCell** roots() const;

private:
    static constexpr size_t inlineCapacity = 2048;
    
    template<bool lookForWasmCallees, typename MarkHook>
    void genericAddPointer(char*, HeapVersion markingVersion, HeapVersion newlyAllocatedVersion, TinyBloomFilter<uintptr_t> jsGCFilter, TinyBloomFilter<uintptr_t> boxedWasmCalleeFilter, MarkHook&);

    template<typename MarkHook>
    void genericAddSpan(void* begin, void* end, MarkHook&);
    
    void grow();

    HeapCell** m_roots;
    size_t m_size;
    size_t m_capacity;
    JSC::Heap& m_heap;
    HeapCell* m_inlineRoots[inlineCapacity];
};

inline size_t ConservativeRoots::size() const
{
    return m_size;
}

inline HeapCell** ConservativeRoots::roots() const
{
    return m_roots;
}

} // namespace JSC
