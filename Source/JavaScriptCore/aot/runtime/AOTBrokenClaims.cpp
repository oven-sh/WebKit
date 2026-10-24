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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportBrokenClaim, void, (Instance*, uint32_t claim, uint32_t function, uint32_t valueIndex))
{
    static Lock lock;
    lock.lock();
    dataLogLn("AOT claim check: ", nameOf(static_cast<BrokenClaim>(claim)), " f", function, " v", static_cast<int32_t>(valueIndex));
    CRASH_WITH_INFO(claim, function, valueIndex);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
