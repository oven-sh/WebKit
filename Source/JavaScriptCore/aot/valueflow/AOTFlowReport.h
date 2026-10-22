/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTWire.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>
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
// From selection on, a body that gets no code says nothing.
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
// Indexed by AccessSiteID. A site has the level of the operations that get code. Reads no body.
// The level says what flow knows about the receivers. The cost says what the code of the access itself does: a test is charged to the access it was made for while it is right in front of it, and every other access that relies on it is free.
Vector<FlowSiteLevel> gradeAccessSites(const FlowResults&, AccessSiteObservations&&, BuildContext&);

// The checks that are in the program, and the tests of arguments that some caller may fail. Empty unless the flow report is collected.
// AddGenericVersion makes a test and a branch of every check, so the ones that flow leaves are collected before it runs.
Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program&, BuildContext&);

// Under --aot-check=functions-get-past-entry. Where flow holds that nothing arrives at a parameter, a read or a call's result, what comes behind it cannot be reached, and the code there is a trap.
// That is right of dead code. It is also the form in which a source that no rule names shows first. A function whose first block ends so, and by no throw that the program spells out, is entered for nothing.
// They are looked for once the bodies are prepared and simplified and nothing is inlined: an inlined body has no first block. `isReachable` is findReachableFunctions().
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
