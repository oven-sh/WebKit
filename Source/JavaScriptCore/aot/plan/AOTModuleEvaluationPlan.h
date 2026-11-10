/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class Body;
class Facts;
class Program;
struct Function;

struct ModuleStep {
    FunctionID callee;
    bool calleeIsStart { false };
    ModuleID moduleThatHasRun;
    ModuleID moduleThatBegins;
};

struct ModuleSitePlan {
    bool asksLoader { true };
    std::span<const ModuleStep> steps;
};

struct ModuleStart {
    ModuleID module;
    FunctionID body;
    std::span<const ModuleStep> steps;
    uint32_t numberOfStepsBeforeItBegins { 0 };
    std::span<const ModuleID> filesRunWhileLoaded;
    bool beginsInBody { false };
    bool mayBeLeftByCaughtError { false };
    bool bodyOnlyReturns { false };
};

struct ModuleStartOperation {
    enum class Kind : uint8_t {
        CallStart,
        CallBody,
        Begin,
        BeginOrReturn,
        Finish,
        ReturnIfLoadingStops,
        AbandonOnThrow,
        NeverComesBack,
    };

    Kind kind { Kind::CallBody };
    FunctionID callee;
    ModuleID module;
};

struct BareModuleSite {
    FunctionID function;
    ValueID value;
    uint32_t firstStep { 0 };
    uint32_t numberOfSteps { 0 };
};

struct StepsOfLateModuleSites {
    uint32_t firstStep { 0 };
    uint32_t numberOfSteps { 0 };
};

struct ModuleEvaluationParts {
    Vector<ModuleStart> starts;
    Vector<uint32_t> startsByBody;
    BitVector bodiesWithStart;
    Vector<ModuleStep> steps;
    Vector<ModuleID> filesRunWhileLoaded;
    Vector<BareModuleSite> bareSites;
    Vector<StepsOfLateModuleSites> stepsOfLateSites;
    BitVector modulesCalledWhereRequired;
    BitVector modulesThatStartThemselves;
    BitVector modulesSkippingRecordRequire;
    BitVector modulesEvaluatedByCodeAlone;
    BitVector modulesWhosePathNoSiteNames;
    BitVector bodiesThatRowsLeaveOut;
    bool noRequireEvaluatesESModule { false };
};

class ModuleEvaluationPlan {
    WTF_MAKE_NONCOPYABLE(ModuleEvaluationPlan);
public:
    ModuleEvaluationPlan() = default;
    explicit ModuleEvaluationPlan(ModuleEvaluationParts&& parts)
        : m_parts(WTF::move(parts))
    {
    }
    ModuleEvaluationPlan(ModuleEvaluationPlan&&) = default;
    ModuleEvaluationPlan& operator=(ModuleEvaluationPlan&&) = default;

    ModuleSitePlan siteOf(FunctionID function, ValueID evaluateModule, ModuleID target) const;
    const ModuleStart* findStart(ModuleID) const LIFETIME_BOUND;
    const ModuleStart* findStartOfBody(FunctionID body) const LIFETIME_BOUND;
    std::span<const ModuleStart> starts() const LIFETIME_BOUND { return m_parts.starts.span(); }
    static uint32_t numberOf(const ModuleStart& start) { return start.module.index(); }
    Vector<ModuleStartOperation, 8> operationsOf(const ModuleStart&) const;
    std::span<const BareModuleSite> bareSites() const LIFETIME_BOUND { return m_parts.bareSites.span(); }
    std::span<const ModuleStep> stepsOf(const BareModuleSite& site) const LIFETIME_BOUND { return m_parts.steps.subspan(site.firstStep, site.numberOfSteps); }
    bool leavesOutCallOf(FunctionID body) const { return m_parts.bodiesThatRowsLeaveOut.get(body.index()); }
    bool isCalledWhereRequired(ModuleID module) const { return m_parts.modulesCalledWhereRequired.get(module.index()); }
    bool startsItself(ModuleID module) const { return m_parts.modulesThatStartThemselves.get(module.index()); }
    bool canSkipRecordRequire(ModuleID module) const { return m_parts.modulesSkippingRecordRequire.get(module.index()); }
    static bool beginAsksRunTime(const Program&, ModuleID);
    bool finishLeavesStatusInRecord(const Program&, ModuleID) const;
    ModuleID moduleThatBeginsIn(FunctionID body) const
    {
        const ModuleStart* start = findStartOfBody(body);
        return start && start->beginsInBody ? start->module : ModuleID();
    }
    bool isEvaluatedByCodeAlone(ModuleID module) const { return m_parts.modulesEvaluatedByCodeAlone.get(module.index()); }
    bool isPathNamedBySites(ModuleID module) const { return !m_parts.modulesWhosePathNoSiteNames.get(module.index()); }
    bool isBodyResumedByJobOfLoader(const Function&) const;

private:
    ModuleEvaluationParts m_parts;
};

bool bodyOfModuleOnlyReturns(const Body&);

ModuleEvaluationPlan planModuleEvaluation(const Program&, const Facts&, const ParallelLoops&, Statistics&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
