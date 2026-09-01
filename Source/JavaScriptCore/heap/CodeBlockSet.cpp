/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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

#include "config.h"
#include "CodeBlockSet.h"

#include "CodeBlock.h"
#include "HeapInlines.h"
#include <wtf/CommaPrinter.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeBlockSet);

CodeBlockSet::CodeBlockSet() = default;

CodeBlockSet::~CodeBlockSet() = default;

bool CodeBlockSet::contains(const AbstractLocker&, void* candidateCodeBlock)
{
    RELEASE_ASSERT(m_lock.isLocked());
    CodeBlock* codeBlock = static_cast<CodeBlock*>(candidateCodeBlock);
    if (!UncheckedKeyHashSet<CodeBlock*>::isValidValue(codeBlock))
        return false;
    return m_codeBlocks.contains(codeBlock);
}

void CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks(VM& vm)
{
    ASSERT(vm.heap.isInPhase(CollectorPhase::End));
    m_currentlyExecuting.clear();
    m_codeBlocks.removeIf([&](CodeBlock* codeBlock) {
        if (vm.heap.isMarked(codeBlock))
            return false;
        // TSAN-TRIAGE §17.2 row 17 (closeout review): flag-on, unlink the
        // dead block's incoming calls HERE, in the End phase while the world
        // is stopped, instead of waiting for the lazy-sweep destructor.
        // Rationale: ~CodeBlock runs on an allocation-path sweep AFTER the
        // world resumes (the IncrementalSweeper is disabled in shared mode),
        // so between resume and sweep a sibling mutator can still call
        // through a still-linked CallLinkInfo and load this dead cell's
        // pointer — a pointer acquired AFTER the conservative scan, hence
        // not pinned by it. Such a straggler races the sweep and, once the
        // IsoSubspace slot is recycled, binds the baseline prologue's
        // loadPairPtr(offsetOfJITData, offsetOfMetadataTable) to the NEW
        // occupant's fields (the silent wrong-metadata class). Unlinking
        // inside the stop closes that acquisition window: any thread still
        // able to enter this block after resume must have held the pointer
        // at scan time, which conservatively marks the cell — contradiction,
        // so it cannot be dead. The AB18-B / rows 7/8/16 field-keeping leaks
        // remain as defense-in-depth for non-CLI entry vectors. The
        // destructor's own unlinkOrUpgradeIncomingCalls then drains an empty
        // list (no-op). World-stopped repatching has precedent in row 2
        // (resetStubAsJumpInAccess). Flag-off: byte-identical (unlink stays
        // at destructor time). THREADS-INTEGRATE(jit)
        if (Options::useJSThreads()) [[unlikely]]
            codeBlock->unlinkOrUpgradeIncomingCalls(vm, nullptr);
        return true;
    });
}

bool CodeBlockSet::isCurrentlyExecuting(CodeBlock* codeBlock)
{
    return m_currentlyExecuting.contains(codeBlock);
}

void CodeBlockSet::dump(PrintStream& out) const
{
    CommaPrinter comma;
    out.print("{codeBlocks = ["_s);
    for (CodeBlock* codeBlock : m_codeBlocks)
        out.print(comma, pointerDump(codeBlock));
    out.print("], currentlyExecuting = ["_s);
    comma = CommaPrinter();
    for (CodeBlock* codeBlock : m_currentlyExecuting)
        out.print(comma, pointerDump(codeBlock));
    out.print("]}"_s);
}

void CodeBlockSet::add(CodeBlock* codeBlock)
{
    Locker locker { m_lock };
    auto result = m_codeBlocks.add(codeBlock);
    RELEASE_ASSERT(result);
}

void CodeBlockSet::remove(CodeBlock* codeBlock)
{
    Locker locker { m_lock };
    bool result = m_codeBlocks.remove(codeBlock);
    RELEASE_ASSERT(result);
}

} // namespace JSC

