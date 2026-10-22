/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTFlowReport.h"
#include "AOTFlowResults.h"
#include <span>
#include <wtf/Vector.h>

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

Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program&, const AOT::CompileOptions&, FunctionID);

Vector<Wire::AccessRecord> buildAccessRecords(const Program&, const FlowResults&, std::span<const FlowSiteLevel>, BuildContext&);
Vector<Wire::AccessCountRecord> countAccesses(const Program&, std::span<const Wire::AccessRecord>, std::span<const Wire::TypeTestRecord>);

uint64_t remainingCheckIDOf(const Program&, const AOT::CompileOptions&, Wire::SourceLocation origin, TypeSiteKind);

void appendLocaleUses(const Program&, const FlowResults&, FlowReport&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
