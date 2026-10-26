/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpectedRowsEmitter.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTPlacement.h"

namespace JSC { namespace AOT { namespace Typed {

ExpectedRowsEmitter::ExpectedRowsEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ExpectedRowsEmitter::generateExpectRows(const ValueContext& value)
{
    LayoutID layout = value->layout();
    const FlowResults& flow = inputs().flow;
    AllocationSiteID site = flow.typedResultSite(layout);
    uint32_t shape = m_placement.shapes().shapeOf(layout, site ? flow.at(site).allocationClass : AllocationClassID());
    if (!shape) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    callVoidOperation(value, RuntimeOperation::ExpectRows, { boxed(value->targetOperand()), word32(static_cast<int32_t>(shape)) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
