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

#include <wtf/DataLog.h>

namespace JSC {

// Per-flush-path provenance tag for isStructurallySoundWithin failure reports
// (Options::validateFreeListStructure). Set around the conductor step-5 /
// per-client flush / teardown traversals; "other" = owner-thread refill etc.
static thread_local const char* s_structureValidationContext = "other";

void FreeList::setStructureValidationContext(const char* context)
{
    s_structureValidationContext = context ? context : "other";
}

const char* FreeList::structureValidationContext()
{
    return s_structureValidationContext;
}

bool FreeList::isStructurallySoundWithin(const char* site, char* payloadBegin, char* payloadEnd, size_t cellSize, size_t maxIntervals) const
{
    // Structural validation of the FreeList against its owning MarkedBlock's
    // payload. Every pointer is bounds/alignment-checked BEFORE decode so
    // corruption is reported, never dereferenced; the walk is capped at
    // maxIntervals as the cycle breaker.
    auto fail = [&] (const char* what, const void* value) -> bool {
        dataLogF("FreeList structural violation: site=%s ctx=%s freeList=%p what=%s val=%p block=[%p,%p) cellSize=%zu is=%p ie=%p ni=%p originalSize=%u\n",
            site, s_structureValidationContext, static_cast<const void*>(this), what, value,
            static_cast<void*>(payloadBegin), static_cast<void*>(payloadEnd), cellSize,
            static_cast<void*>(m_intervalStart), static_cast<void*>(m_intervalEnd), static_cast<void*>(m_nextInterval), m_originalSize);
        return false;
    };

    if (!cellSize || payloadBegin >= payloadEnd || cellSize > static_cast<size_t>(payloadEnd - payloadBegin))
        return fail("cellSize", std::bit_cast<void*>(cellSize));
    auto inPayloadAligned = [&] (char* p) {
        return p >= payloadBegin && p < payloadEnd && !((p - payloadBegin) % cellSize);
    };
    // Current bump interval.
    if (m_intervalStart < m_intervalEnd) {
        if (!inPayloadAligned(m_intervalStart))
            return fail("intervalStart", m_intervalStart);
        if (m_intervalEnd > payloadEnd || ((m_intervalEnd - m_intervalStart) % cellSize))
            return fail("intervalEnd", m_intervalEnd);
    }
    // Linked intervals: bounds-check BEFORE each decode so garbage is
    // reported, not dereferenced.
    FreeCell* cell = m_nextInterval;
    size_t steps = 0;
    while (!isSentinel(cell)) {
        if (!inPayloadAligned(std::bit_cast<char*>(cell)))
            return fail("nextInterval", cell);
        if (++steps > maxIntervals)
            return fail("cycle", cell);
        auto [offsetToNext, lengthInBytes] = cell->decode(m_secret);
        char* intervalStart = std::bit_cast<char*>(cell);
        char* intervalEnd = intervalStart + lengthInBytes;
        if (!lengthInBytes || (lengthInBytes % cellSize) || intervalEnd > payloadEnd)
            return fail("intervalLength", std::bit_cast<void*>(static_cast<uintptr_t>(lengthInBytes)));
        cell = std::bit_cast<FreeCell*>(intervalStart + offsetToNext);
    }
    return true;
}

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

void FreeList::dump(PrintStream& out) const
{
    out.print("{nextInterval = ", RawPointer(nextInterval()), ", secret = ", m_secret, ", intervalStart = ", RawPointer(m_intervalStart), ", intervalEnd = ", RawPointer(m_intervalEnd), ", originalSize = ", m_originalSize, "}");
}

} // namespace JSC

