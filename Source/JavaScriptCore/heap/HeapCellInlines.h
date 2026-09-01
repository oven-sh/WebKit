/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
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

#include "CellContainer.h"
#include "HeapCell.h"
#include "PreciseAllocation.h"
#include "VM.h"

namespace JSC {

ALWAYS_INLINE bool HeapCell::isPreciseAllocation() const
{
    return PreciseAllocation::isPreciseAllocation(const_cast<HeapCell*>(this));
}

ALWAYS_INLINE CellContainer HeapCell::cellContainer() const
{
    if (isPreciseAllocation())
        return preciseAllocation();
    return markedBlock();
}

ALWAYS_INLINE MarkedBlock& HeapCell::markedBlock() const
{
    return *MarkedBlock::blockFor(this);
}

ALWAYS_INLINE PreciseAllocation& HeapCell::preciseAllocation() const
{
    return *PreciseAllocation::fromCell(const_cast<HeapCell*>(this));
}

ALWAYS_INLINE JSC::Heap* HeapCell::heap() const
{
    return &vm().heap;
}

// SharedGC (T9): conductor-context OK — both branches resolve to the SERVER
// heap's main VM (deviation 3): blocks and precise allocations belong to the
// shared server, never to a client, so cell->vm()/cell->heap() are
// thread-agnostic round-trips (cell -> server -> main VM). What callers do
// with the VM is classified per the Heap::vm() legend (HeapInlines.h).
ALWAYS_INLINE VM& HeapCell::vm() const
{
    if (isPreciseAllocation())
        return preciseAllocation().vm();
    return markedBlock().vm();
}

// TSAN-annotated stale-probe variant (thread-closeout final review): use at
// blessed sites that may probe a cell on a RECYCLED MarkedBlock (e.g. the
// ownerForSlowPath consumers in JITOperations.cpp). Routes through
// MarkedBlock::vmConcurrentProbe(), whose HAPPENS_AFTER pairs with the
// Header-ctor HAPPENS_BEFORE; plain vm() stays unannotated so TSAN's
// cross-thread visibility is preserved engine-wide. No-op outside TSAN.
ALWAYS_INLINE VM& HeapCell::vmConcurrentProbe() const
{
    if (isPreciseAllocation())
        return preciseAllocation().vm();
    return markedBlock().vmConcurrentProbe();
}
    
ALWAYS_INLINE size_t HeapCell::cellSize() const
{
    if (isPreciseAllocation())
        return preciseAllocation().cellSize();
    return markedBlock().cellSize();
}

ALWAYS_INLINE CellAttributes HeapCell::cellAttributes() const
{
    if (isPreciseAllocation())
        return preciseAllocation().attributes();
    return markedBlock().attributes();
}

ALWAYS_INLINE DestructionMode HeapCell::destructionMode() const
{
    return cellAttributes().destruction;
}

ALWAYS_INLINE HeapCell::Kind HeapCell::cellKind() const
{
    return cellAttributes().cellKind;
}

ALWAYS_INLINE Subspace* HeapCell::subspace() const
{
    if (isPreciseAllocation())
        return preciseAllocation().subspace();
    return markedBlock().subspace();
}

ALWAYS_INLINE void HeapCell::notifyNeedsDestruction() const
{
    ASSERT(!isPreciseAllocation());
    ASSERT(destructionMode() == MayNeedDestruction);
    markedBlock().handle().setIsDestructible(true);
}

} // namespace JSC

