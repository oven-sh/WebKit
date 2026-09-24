/*
 * Copyright (C) 2014-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2014 Saam Barati. <saambarati1@gmail.com>
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
#include "BasicBlockLocation.h"

#include "CCallHelpers.h"
#include <algorithm>
#include <climits>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BasicBlockLocation);

BasicBlockLocation::BasicBlockLocation(int startOffset, int endOffset)
    : m_startOffset(startOffset)
    , m_endOffset(endOffset)
    , m_executionCount(0)
{
}

void BasicBlockLocation::insertGap(int startOffset, int endOffset)
{
    std::pair<int, int> gap(startOffset, endOffset);
    if (!m_gaps.contains(gap))
        m_gaps.append(gap);
}

Vector<std::pair<int, int>> BasicBlockLocation::getExecutedRanges() const
{
    Vector<Gap> result;
    Vector<Gap> gaps = m_gaps;
    // Because we know that the Gaps inside m_gaps aren't enclosed within one another, it suffices to just check the first element to test ordering.
    std::sort(gaps.begin(), gaps.end(), [](const Gap& a, const Gap& b) {
        return a.first < b.first;
    });
    result.reserveInitialCapacity(gaps.size() + 1);
    int nextRangeStart = m_startOffset;
    for (const Gap& gap : gaps) {
        result.append(Gap(nextRangeStart, gap.first - 1));
        nextRangeStart = gap.second + 1;
    }

    result.append(Gap(nextRangeStart, m_endOffset));
    return result;
}

void BasicBlockLocation::dumpData() const
{
    Vector<Gap> executedRanges = getExecutedRanges();
    for (Gap gap : executedRanges) {
        dataLogF("\tBasicBlock: [%d, %d] hasExecuted: %s, executionCount:", gap.first, gap.second, hasExecuted() ? "true" : "false");
        dataLogLn(m_executionCount);
    }
}

#if ENABLE(JIT)
void BasicBlockLocation::emitExecuteCode(CCallHelpers& jit) const
{
    static_assert(sizeof(UCPURegister) == 8, "Assuming size_t is 64 bits on 64 bit platforms.");
    jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(&m_executionCount));
}
#endif // ENABLE(JIT)

} // namespace JSC
