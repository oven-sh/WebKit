/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class Heat;
class Program;
struct FlowAddedName;

struct AllocationSiteReport {
    FunctionID function;
    Wire::AllocationKind kind { Wire::AllocationKind::Other };
};

struct FlowReport {
    Vector<Wire::UnseenCodeRecord> unseenCode;
    Vector<Wire::BuiltinUseRecord> builtinsUsed;
    Vector<Wire::LackedFunctionRecord> libraryFunctionsLacked;
    Vector<Wire::ReplacedExportRecord> exportsThatSpiesReplace;
    Vector<Wire::ReplacedBuiltinRecord> builtinsThatTestsReplace;
    Vector<Wire::AddedNameRecord> namesThatTestsAdd;
    Vector<uint8_t> textOfNamesThatTestsAdd;
    Vector<Wire::FeatureUseRecord> featuresUsed;
    Vector<Wire::LocaleUseRecord> localesUsed;
    Vector<uint8_t> localeTags;
    Vector<Wire::WorkerEntryRecord> workerEntries;
    Vector<uint8_t> workerEntryPaths;
    Vector<Wire::WorkerStartRecord> workerStartsWithoutName;
    Vector<Wire::AccessCountRecord> accessCounts;
    Vector<Wire::AccessRecord> accesses;
    Vector<Wire::OtherOriginsRecord> otherOrigins;
    Vector<Wire::RemainingCheckRecord> remainingChecks;
    Vector<Wire::TypeTestRecord> typeTests;
    Vector<AllocationSiteReport> allocationSites;
};

struct FlowSiteLevel {
    Wire::KnowledgeLevel level { Wire::KnowledgeLevel::Proven };
    Wire::AccessCost cost { Wire::AccessCost::Free };
    uint32_t copies { 0 };
    uint8_t distance { UINT8_MAX };
    uint8_t distanceForCost { UINT8_MAX };
    LayoutID layout;
    PropertyKeyID key;
    uint32_t candidatesOfLeftOperation { 0 };
    double heat { 0 };
};

struct AccessSiteObservations {
    struct OfFunctions;

    AccessSiteObservations();
    AccessSiteObservations(AccessSiteObservations&&);
    AccessSiteObservations& operator=(AccessSiteObservations&&);
    ~AccessSiteObservations();

    bool isEmpty() const { return !ofFunctions; }

    std::unique_ptr<OfFunctions> ofFunctions;
};

AccessSiteObservations observeAccessSites(const Program&, const FlowResults&, BuildContext&, const Heat* = nullptr);

class AccessSiteObserver {
    WTF_MAKE_NONCOPYABLE(AccessSiteObserver);
public:
    AccessSiteObserver(const Program&, const FlowResults&, BuildContext&, const Heat* = nullptr);
    ~AccessSiteObserver();

    void observe(FunctionID);
    AccessSiteObservations finish();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

struct GradedSites {
    Vector<FlowSiteLevel> levels;
    Vector<Wire::AccessRecord> generalOperations;
};
GradedSites gradeAccessSites(const Program&, const FlowResults&, AccessSiteObservations&&, BuildContext&);

Vector<Wire::RemainingCheckRecord> collectRemainingChecks(const Program&, BuildContext&);

struct FlowReportBodyRecords {
    Vector<Wire::TypeTestRecord> typeTests;
    Vector<Wire::RemainingCheckRecord> remainingChecks;
    Vector<BuiltinID> builtinsCalledDirectly;
};
FlowReportBodyRecords collectFlowReportBodyRecords(const Program&, BuildContext&);
// Of the report: unseenCode, builtinsUsed, libraryFunctionsLacked, exportsThatSpiesReplace, builtinsThatTestsReplace and namesThatTestsAdd.
FlowReport reportUsesOfLibraryCode(const FlowResults&);
using JudgeOfUsesOfLibraryCode = ScopedLambda<bool(const FlowReport&)>;
FlowReport reportUsesOfLibraryCode(std::span<const Wire::UnseenCodeRecord> unseenCode, std::span<const Wire::UnseenCodeRecord> loadsOfMachineCode, std::span<const Wire::BuiltinUseRecord>, std::span<const RuntimeFunctionID> libraryFunctionsLacked, std::span<const VariableID> exportsThatSpiesReplace, std::span<const BuiltinID> builtinsThatTestsReplace, std::span<const FlowAddedName> namesThatTestsAdd);
FlowReport buildFlowReport(const Program&, const FlowResults&, const GradedSites&, std::span<const Wire::RemainingCheckRecord> checksLeftByFlow, FlowReportBodyRecords&&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
