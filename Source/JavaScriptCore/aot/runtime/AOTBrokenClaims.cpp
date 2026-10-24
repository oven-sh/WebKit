/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBrokenClaims.h"

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/HexNumber.h>

namespace JSC { namespace AOT {

static ASCIILiteral nameOf(BrokenClaim claim)
{
    switch (claim) {
    case BrokenClaim::StackSlotIsWrittenBeforeRead:
        return "StackSlotIsWrittenBeforeRead"_s;
    case BrokenClaim::WordNeverChanges:
        return "WordNeverChanges"_s;
    case BrokenClaim::LoadMayMove:
        return "LoadMayMove"_s;
    case BrokenClaim::ValueIsInRange:
        return "ValueIsInRange"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportBrokenClaim, void, (uint32_t claimAndOffset, uint32_t function, uint32_t valueIndex, const uint64_t* foundAndGiven))
{
    uint32_t claim = claimAndOffset & 0xff;
    static Lock lock;
    lock.lock();
    dataLog("AOT claim check: ", nameOf(static_cast<BrokenClaim>(claim)), " f", function, " v", static_cast<int32_t>(valueIndex));
    if (static_cast<BrokenClaim>(claim) != BrokenClaim::ValueIsInRange)
        dataLog(" at +", static_cast<int32_t>(claimAndOffset) >> 8);
    if (static_cast<BrokenClaim>(claim) == BrokenClaim::WordNeverChanges || static_cast<BrokenClaim>(claim) == BrokenClaim::LoadMayMove)
        dataLog(" holds 0x", hex(foundAndGiven[0], Lowercase), ", not 0x", hex(foundAndGiven[1], Lowercase));
    dataLogLn();
    CRASH_WITH_INFO(claim, function, valueIndex);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
