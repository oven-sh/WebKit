/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

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
    Wire::AccessCost cost { Wire::AccessCost::Free };
    uint32_t copies { 0 };
    LayoutID layout;
    PropertyKeyID key;
    // Of an operation that the passes left as it was: the receivers or the callees that flow lists for it, where the list is complete.
    uint32_t candidatesOfLeftOperation { 0 };
};

class AccessSiteObservations {
public:
    struct OfFunctions;

    AccessSiteObservations();
    explicit AccessSiteObservations(std::unique_ptr<OfFunctions>&&);
    AccessSiteObservations(AccessSiteObservations&&);
    AccessSiteObservations& operator=(AccessSiteObservations&&);
    ~AccessSiteObservations();

    bool isEmpty() const { return !m_ofFunctions; }
    std::unique_ptr<OfFunctions> take();

private:
    std::unique_ptr<OfFunctions> m_ofFunctions;
};

// Outlining merges equal operations of many sites into one body, so the sites are observed before it runs.
AccessSiteObservations observeAccessSites(const Program&, const FlowResults&, BuildContext&);

// The same, one function at a time, so that whoever has a function in the cache can have it observed there.
class AccessSiteObserver {
    WTF_MAKE_NONCOPYABLE(AccessSiteObserver);
public:
    AccessSiteObserver(const Program&, const FlowResults&, BuildContext&);
    ~AccessSiteObserver();

    // Once for a function with a body, from any thread.
    void observe(FunctionID);
    // Behind the last function. The bodies are as they were observed.
    AccessSiteObservations finish();

private:
    struct State;
    std::unique_ptr<State> m_state;
};
// Indexed by AccessSiteID.
Vector<FlowSiteLevel> gradeAccessSites(const FlowResults&, AccessSiteObservations&&, BuildContext&);

// The checks that are in the program, and the tests of arguments that some caller may fail.
Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program&, BuildContext&);

// Under --aot-check=functions-get-past-entry.
Vector<InternalError> findFunctionsThatEndInFirstBlock(const Program&, const BitVector& isReachable, BuildContext&);
// Behind the optimizations and the removal of what nothing names any more: each whose body still gets code, or lies inlined in one, is an error. The others were dead code.
void reportFunctionsThatEndInFirstBlock(const Program&, Vector<InternalError>&&, BuildContext&);

// The part of the report that reads function bodies, which code generation releases.
struct FlowReportBodyRecords {
    Vector<Wire::TypeTestRecord> typeTests;
    Vector<Wire::RemainingCheckRecord> remainingChecks;
};

FlowReportBodyRecords collectFlowReportBodyRecords(const Program&, BuildContext&);
FlowReport buildFlowReport(const Program&, const FlowResults&, std::span<const FlowSiteLevel>, std::span<const Wire::RemainingCheckRecord> checksLeftByFlow, FlowReportBodyRecords&&, BuildContext&);

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
