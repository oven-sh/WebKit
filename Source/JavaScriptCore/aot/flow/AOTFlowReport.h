/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTWire.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class Program;

struct AllocationSiteReport {
    FunctionID function;
    Wire::AllocationKind kind { Wire::AllocationKind::Other };
};

struct FlowReport {
    Vector<Wire::UnseenCodeRecord> unseenCode;
    Vector<Wire::BuiltinUseRecord> builtinsUsed;
    Vector<Wire::FeatureUseRecord> featuresUsed;
    Vector<Wire::LocaleUseRecord> localesUsed;
    Vector<uint8_t> localeTags;
    Vector<Wire::AccessCountRecord> accessCounts;
    Vector<Wire::AccessRecord> accesses;
    Vector<Wire::RemainingCheckRecord> remainingChecks;
    Vector<Wire::TypeTestRecord> typeTests;
    // Indexed by AllocationSiteID. Empty unless allocation remarks are collected.
    Vector<AllocationSiteReport> allocationSites;
};

struct FlowSiteLevel {
    Wire::KnowledgeLevel level { Wire::KnowledgeLevel::Proven };
    uint32_t copies { 0 };
    LayoutID layout;
    PropertyKeyID key;
    // Of an operation that the passes left as it was: the receivers or the callees that flow lists for it, where the list is complete.
    uint32_t candidatesOfLeftOperation { 0 };
};

// Indexed by AccessSiteID. Outlining merges equal operations of many sites into one body, so the sites are graded before it runs.
Vector<FlowSiteLevel> gradeAccessSites(const Program&, const FlowResults&, BuildContext&);

FlowReport buildFlowReport(const Program&, const FlowResults&, std::span<const FlowSiteLevel>, BuildContext&);

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
