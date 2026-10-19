/*
 * Copyright (C) 2016-2017 Apple Inc. All rights reserved.
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
#include "FreeList.h"

namespace JSC {

FreeList::FreeList(unsigned cellSize)
    : m_cellSize(cellSize)
{
}

FreeList::~FreeList() = default;

void FreeList::clear()
{
    m_intervalStart = nullptr;
    m_intervalEnd = nullptr;
    m_nextInterval = std::bit_cast<FreeCell*>(static_cast<uintptr_t>(1));
    m_secret = 0;
    m_originalSize = 0;
}

void FreeList::initialize(FreeCell* start, uint64_t secret, unsigned bytes)
{
    if (!start) [[unlikely]] {
        clear();
        return;
    }
    m_secret = secret;
    m_nextInterval = start;
    FreeCell::advance(m_secret, m_nextInterval, m_intervalStart, m_intervalEnd);
    m_originalSize = bytes;
}

#if USE(BUN_JSC_ADDITIONS)
unsigned FreeList::hideCellsForSampling(unsigned cells, char*& savedIntervalEnd, FreeCell*& savedNextInterval)
{
    if (m_intervalStart >= m_intervalEnd) {
        if (isSentinel(m_nextInterval))
            return 0;
        FreeCell::advance(m_secret, m_nextInterval, m_intervalStart, m_intervalEnd);
    }
    unsigned visible = std::min<uint64_t>(cells, (m_intervalEnd - m_intervalStart) / m_cellSize);
    savedIntervalEnd = m_intervalEnd;
    savedNextInterval = m_nextInterval;
    m_intervalEnd = m_intervalStart + static_cast<size_t>(visible) * m_cellSize;
    m_nextInterval = std::bit_cast<FreeCell*>(static_cast<uintptr_t>(1));
    return visible;
}

unsigned FreeList::restoreHiddenCells(char* savedIntervalEnd, FreeCell* savedNextInterval)
{
    ASSERT(m_intervalStart <= m_intervalEnd);
    unsigned notTaken = (m_intervalEnd - m_intervalStart) / m_cellSize;
    m_intervalEnd = savedIntervalEnd;
    m_nextInterval = savedNextInterval;
    return notTaken;
}
#endif

void FreeList::dump(PrintStream& out) const
{
    out.print("{nextInterval = ", RawPointer(nextInterval()), ", secret = ", m_secret, ", intervalStart = ", RawPointer(m_intervalStart), ", intervalEnd = ", RawPointer(m_intervalEnd), ", originalSize = ", m_originalSize, "}");
}

} // namespace JSC

