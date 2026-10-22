/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTArrivalSiteNumbers.h"

namespace JSC { namespace AOT { namespace Typed {

// The call has the effects of the operation and not those of the access it stands before. An access that writes nothing would leave a call that writes nothing, which B3 removes.
void AccessEmitter::recordArrival(const OperationContext& context, Boxed receiver, uint32_t part)
{
    const ArrivalSiteNumbers& sites = inputs().arrivalSites;
    if (!sites.isCounting())
        return;
    std::optional<uint32_t> number = sites.numberOf(m_id, context.valueID(), part);
    if (!number)
        return;
    Operand arguments[] { receiver, word32(static_cast<int32_t>(*number)) };
    callWithoutSite(RuntimeOperation::RecordArrival, arguments, Representation::Void, { .isOnRarePath = true });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
