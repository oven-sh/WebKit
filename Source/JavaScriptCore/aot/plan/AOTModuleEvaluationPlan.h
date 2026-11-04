/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
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

// No emitter stands behind EvaluateModule. Where to evaluate a module is only to call functions, compiled code calls them. Elsewhere the loader is asked (EvaluateRequiredModule).
// Decided once for the program, before any code exists, from the records of the modules, from what Facts says of the program, and from the operations of reached code that name a module.

// One of the three is set.
struct ModuleStep {
    // The body of a module without a state, or the start of a module with one.
    FunctionID callee;
    bool calleeIsStart { false };
    // It keeps no status itself and has run by then: BeginModuleEvaluation and FinishModuleEvaluation, for the record that the loader may hold.
    ModuleID moduleThatHasRun;
    // BeginModuleEvaluation alone.
    ModuleID moduleThatBegins;
};

// What stands in place of one EvaluateModule.
struct ModuleSitePlan {
    // The loader links what the module imports, waits or refuses to, or has the module itself: EvaluateRequiredModule, and no steps.
    bool asksLoader { true };
    std::span<const ModuleStep> steps;
};

// CodeRole::ModuleStart of the module's body: to evaluate a module with a state is to call it.
struct ModuleStart {
    ModuleID module;
    FunctionID body;
    // What evaluates all that the module imports, in the order of the requests.
    std::span<const ModuleStep> steps;
    uint32_t numberOfStepsBeforeItBegins { 0 };
    // By step before it begins: the file that the step runs, or none. Behind such a step stands StopLoadingBehindFile, and the start returns where that says so.
    std::span<const ModuleID> filesRunWhileLoaded;
    // The body opens with BeginModuleEvaluation, returns undefined where that says no, and has FinishModuleEvaluation in front of each Return.
    bool beginsInBody { false };
    // Whoever has required the module may catch what its evaluation throws and go on, and the module keeps the error for whoever asks next.
    bool mayBeLeftByCaughtError { false };
    // The body does nothing but return, and is not called.
    bool bodyOnlyReturns { false };
};

// What a start does, one thing behind the other. Image writes each as a word of the row of the start (ModuleStartStepData), and the stub StartModule goes through the row.
struct ModuleStartOperation {
    enum class Kind : uint8_t {
        // `callee` is the body of the module that is started.
        CallStart,
        CallBody,
        // BeginModuleEvaluation of `module`, whatever it answers.
        Begin,
        // The same. The start returns where it answers no.
        BeginOrReturn,
        Finish,
        // StopLoadingBehindFile of `module`. The start returns where it answers yes.
        ReturnIfLoadingStops,
        // From here on whatever is thrown through the start abandons the evaluation of `module` on its way.
        AbandonOnThrow,
        // The last one, behind the call of a body that never returns.
        NeverComesBack,
    };

    Kind kind { Kind::CallBody };
    FunctionID callee;
    ModuleID module;
};

// An EvaluateModule in the body of its referrer, where every run of the body passes once and no handler stands around it.
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

// A set that is empty claims nothing.
struct ModuleEvaluationParts {
    // Ascending by module.
    Vector<ModuleStart> starts;
    // Places in `starts`, ascending by body.
    Vector<uint32_t> startsByBody;
    // What ModuleStart::steps, the bare sites and the late sites point into.
    Vector<ModuleStep> steps;
    // What ModuleStart::filesRunWhileLoaded point into.
    Vector<ModuleID> filesRunWhileLoaded;
    // Ascending by function and value.
    Vector<BareModuleSite> bareSites;
    // By module: what stands at a site that is not bare.
    Vector<StepsOfLateModuleSites> stepsOfLateSites;
    BitVector modulesCalledWhereRequired;
    BitVector modulesThatStartThemselves;
    BitVector modulesWhoseRequiresRecordNothingThatIsSeen;
    BitVector modulesEvaluatedByCodeAlone;
    BitVector modulesWhosePathNoSiteNames;
    // By function.
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

    // A bare site is known by the function and the value of the input. Any other pair finds the plan of a late site, which asks the status of the target and is right wherever it stands.
    ModuleSitePlan siteOf(FunctionID function, ValueID evaluateModule, ModuleID target) const;
    // Null: nothing calls a start of this module, and the run time enters its body.
    const ModuleStart* findStart(ModuleID) const LIFETIME_BOUND;
    const ModuleStart* findStartOfBody(FunctionID body) const LIFETIME_BOUND;
    std::span<const ModuleStart> starts() const LIFETIME_BOUND { return m_parts.starts.span(); }
    // What the stub StartModule takes: the number of the module, so that whoever holds the number of a start can ask whether the module has begun.
    static uint32_t numberOf(const ModuleStart& start) { return start.module.index(); }
    Vector<ModuleStartOperation, 8> operationsOf(const ModuleStart&) const;
    std::span<const BareModuleSite> bareSites() const LIFETIME_BOUND { return m_parts.bareSites.span(); }
    std::span<const ModuleStep> stepsOf(const BareModuleSite& site) const LIFETIME_BOUND { return m_parts.steps.subspan(site.firstStep, site.numberOfSteps); }
    // Nobody can tell whether the body has run, by what Facts says of it, so no row and no site calls it.
    bool leavesOutCallOf(FunctionID body) const { return m_parts.bodiesThatRowsLeaveOut.get(body.index()); }
    // For the loader there would be nothing to do but to evaluate it.
    bool isCalledWhereRequired(ModuleID module) const { return m_parts.modulesCalledWhereRequired.get(module.index()); }
    bool startsItself(ModuleID module) const { return m_parts.modulesThatStartThemselves.get(module.index()); }
    // Nobody can see who required whom first: RecordRequire of this module builds nothing.
    bool recordsNothingThatIsSeen(ModuleID module) const { return m_parts.modulesWhoseRequiresRecordNothingThatIsSeen.get(module.index()); }
    // BeginModuleEvaluation asks the run time about a module that has not begun, and does not store the status itself: a test run holds a file against what lies on disk.
    static bool beginAsksRunTime(const Program&, ModuleID);
    // FinishModuleEvaluation leaves StoredModuleStatus::InRecord as it is.
    bool finishLeavesStatusInRecord(const Program&, ModuleID) const;
    ModuleID moduleThatBeginsIn(FunctionID body) const
    {
        const ModuleStart* start = findStartOfBody(body);
        return start && start->beginsInBody ? start->module : ModuleID();
    }
    // Compiled code alone comes to the module, so for the loader it has nothing to run: the table of modules names no body for it.
    bool isEvaluatedByCodeAlone(ModuleID module) const { return m_parts.modulesEvaluatedByCodeAlone.get(module.index()); }
    // What stands in place of some EvaluateModule or RecordRequire hands the path of the module to the run time.
    bool isPathNamedBySites(ModuleID module) const { return !m_parts.modulesWhosePathNoSiteNames.get(module.index()); }
    // The job that resumes the body of a module has to be one of the loader's, unless no require() evaluates an ES module.
    bool isBodyResumedByJobOfLoader(const Function&) const;

private:
    ModuleEvaluationParts m_parts;
};

// The rule by which a start leaves out the call of its body. The plan passes such a body over by the same rule.
bool bodyOfModuleOnlyReturns(const Body&);

// It reads the bodies that are reached and whose body index lists EvaluateModule, RequireModule, ImportModule, ModuleNamespace or RecordRequire, in parallel by function.
ModuleEvaluationPlan planModuleEvaluation(const Program&, const Facts&, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
