/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowReport.h"
#include "AOTFlowResults.h"

namespace JSC { namespace AOT { namespace Typed {

constexpr unsigned rankOf(KnowledgeLevel level)
{
    switch (level) {
    case KnowledgeLevel::Unknown:
        return 0;
    case KnowledgeLevel::Expected:
        return 1;
    case KnowledgeLevel::Proven:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr unsigned rankOf(AccessCost cost)
{
    switch (cost) {
    case AccessCost::ByName:
        return 0;
    case AccessCost::TestsReceiver:
        return 1;
    case AccessCost::TestsSlot:
        return 2;
    case AccessCost::Free:
        return 3;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

Vector<Wire::AccessRecord> buildAccessRecords(const Program&, const FlowResults&, std::span<const FlowSiteLevel>, std::span<const double> heat, BuildContext&);
Vector<Wire::AccessCountRecord> countAccesses(const Program&, std::span<const Wire::AccessRecord>, std::span<const Wire::TypeTestRecord>);
// What countAccesses() says of the records that buildAccessRecords() makes.
Vector<Wire::AccessCountRecord> countAccessesWithoutRecords(const Program&, const FlowResults&, std::span<const FlowSiteLevel>, std::span<const Wire::TypeTestRecord>, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
