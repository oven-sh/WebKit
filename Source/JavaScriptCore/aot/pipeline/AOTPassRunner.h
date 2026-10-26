/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTInterningPhases.h"
#include "AOTObservableOperations.h"
#include "AOTOpcode.h"
#include "AOTPass.h"
#include "AOTProgramAnalyses.h"
#include "AOTResultRuleMonotonicity.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/BitSet.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Scope.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

// The passes whose changes may give this pass something to do.
const PassSet& passesEnabling(PassID);
const PassSet& passesThatCanWaitBehind(PassID changer);
// False for a pass that may have something to do in any body.
bool hasTriggers(PassID);
// The passes that may have something to do in such a body.
PassSet passesThatMayFireIn(const BodyContents&);

void shrinkToFit(Function&);

class PassRunner {
    WTF_MAKE_NONCOPYABLE(PassRunner);
public:
    explicit PassRunner(BuildContext&, std::optional<PassID> stopAfter = std::nullopt);

    BuildContext& build() const { return m_build; }

    PassOutcome runFunctionPass(PassID, FunctionPassContext&) const;
    // The contents are those of the function as it is handed in, for a caller that has them without a walk.
    PassOutcome runSchedule(FunctionSchedule, const Program&, const OptimizationInputs&, FunctionID, Function&, TaskArenas&, const std::optional<BodyContents>& = std::nullopt) const;

    template<typename Functor> auto runProgramPass(PassID, Program&, ProgramAnalyses&, const Functor&) -> std::invoke_result_t<Functor, ProgramEditor&>;
    template<typename Functor> auto runProgramPassWithoutEditor(PassID, Program&, ProgramAnalyses&, const Functor&, BodiesAreChanged = BodiesAreChanged::Anyhow) -> std::invoke_result_t<Functor>;
    template<typename Functor> auto runAnalysis(PassID, const Program&, const Functor&) -> std::invoke_result_t<Functor>;
    // The analysis may not use the runner. What runs beside it may: it runs other analyses.
    template<typename Functor, typename Beside> auto runAnalysisBeside(PassID, const Program&, const Functor&, const Beside&) -> std::invoke_result_t<Functor>;
    // The first step of the analysis runs alone, and what runs beside the second step finds what it has made.
    template<typename First, typename Second, typename Beside> auto runAnalysisInTwoSteps(PassID, const Program&, const First&, const Second&, const Beside&) -> std::invoke_result_t<Second>;
    // Not in a run that stops or prints after a pass: there the order of the analyses shows.
    bool mayRunAnalysesTogether() const;

    bool isStopped() const { return m_isStopped; }
    PassOutcome lastProgramPassOutcome() const { return m_lastProgramPassOutcome; }
    void didFinishFunctionLoop();

    void setVerifierInputs(const VerifierInputs& inputs) { m_verifierInputs = inputs; }
    const VerifierInputs& verifierInputs() const { return m_verifierInputs; }

    void didCompute(OptionSet<ProgramResult> results) { m_availableResults.add(results); }
    void didInvalidate(OptionSet<ProgramResult> results) { m_availableResults.remove(results); }

private:
    using PassBits = WTF::BitSet<numberOfPasses>;

    // The passes that a change by one pass may give something to do, and those of all passes that do not wait behind such a change.
    struct AffectedPasses {
        PassBits enabled;
        PassBits givenWork;
    };
    static const AffectedPasses& passesAffectedBy(PassID);

    struct BodiesBeforeProgramPass {
        Vector<Function> functions;
        Vector<ReasonsForOperationsThatWent> reasons;
    };

    struct PassTimer {
        MonotonicTime wallStart;
        Seconds cpuStart;
        PassID enclosingPass { PassID::None };
        double startOfEnclosingPassInSeconds { 0 };
        std::shared_ptr<BodiesBeforeProgramPass> bodiesBefore;
    };

    // To read either takes a microsecond, and a build runs millions of function passes, most of them on functions of a few values, where the pass takes no longer than that.
    struct PassClock {
        static PassClock now();

        Seconds time;
        // None where the system has no counter for the thread.
        std::optional<uint64_t> instructions;
    };

    // What a build that logs its times, verifies every pass or reports bytes of code notes about the runs of one schedule. No other build makes room for it.
    struct NotesOfScheduleRun {
        NotesOfScheduleRun() { }

        std::array<uint32_t, numberOfPasses> runsWithoutTrigger { };
        std::array<uint32_t, numberOfPasses> runsThatKeptAnalyses { };
        std::array<PassSet, numberOfPasses> changersSinceLastRun { };
        std::array<uint64_t, numberOfPasses> nanoseconds { };
        std::array<uint64_t, numberOfPasses> instructions { };
    };

    struct ScheduleRun {
        void didChange(PassID changer, const PassSet& passesLeftIdle = { }, const PassSet& passesThatCanWait = { })
        {
            const AffectedPasses& passes = passesAffectedBy(changer);
            PassBits enabled = passes.enabled;
            passesBehindChange.setAll();
            passesEnabledByTable.merge(enabled);
            mayDecideTestsAcrossBlocks |= changer != PassID::SimplifyControlFlow && changer != PassID::EliminateDeadCode;
            passesLeftIdle.forEach([&](PassID pass) {
                enabled.clear(static_cast<unsigned>(pass));
            });
            PassBits givenWork = enabled;
            givenWork.filter(passes.givenWork);
            passesThatCanWait.forEach([&](PassID pass) {
                givenWork.clear(static_cast<unsigned>(pass));
            });
            passesWithWork.merge(givenWork);
            enabled.exclude(givenWork);
            passesThatWait.merge(enabled);
            if (recordsChangers) [[unlikely]] {
                for (PassSet& changers : notes->changersSinceLastRun)
                    changers.add(changer);
            }
        }

        void didLookAtFunction(PassID pass)
        {
            passesBehindChange.clear(static_cast<unsigned>(pass));
            passesEnabledByTable.clear(static_cast<unsigned>(pass));
            passesWithWork.clear(static_cast<unsigned>(pass));
            passesThatWait.clear(static_cast<unsigned>(pass));
            if (recordsChangers) [[unlikely]]
                notes->changersSinceLastRun[static_cast<unsigned>(pass)] = { };
        }

        void giveWorkTo(PassID pass)
        {
            passesBehindChange.set(static_cast<unsigned>(pass));
            passesEnabledByTable.set(static_cast<unsigned>(pass));
            passesWithWork.set(static_cast<unsigned>(pass));
            if (recordsChangers) [[unlikely]]
                notes->changersSinceLastRun[static_cast<unsigned>(pass)] |= passesEnabling(pass);
        }

        void setContents(const BodyContents& newContents, const Function& function)
        {
            contents = newContents;
            passesThatMayFire = passesThatMayFireIn(newContents);
            numberOfValuesInContents = function.values.size();
        }

        const BodyContents& contentsOf(const Function& function)
        {
            if (!contents || numberOfValuesInContents != function.values.size())
                setContents(Typed::contentsOf(function), function);
            return *contents;
        }

        bool mayFire(PassID pass, const Function& function)
        {
            contentsOf(function);
            return passesThatMayFire.contains(pass);
        }

        // The pass has looked at the function as it is now: its step does nothing and leaves nothing behind.
        bool isAtRest(PassID pass) const
        {
            unsigned index = static_cast<unsigned>(pass);
            return passesThatRan.contains(pass) && !passesWithWork.get(index) && !passesEnabledByTable.get(index) && !passesBehindChange.get(index) && !reachedFixpoint[index];
        }

        FunctionID functionAsRead;
        uint32_t numberOfChanges { 0 };
        uint32_t numberOfSkippedRuns { 0 };
        uint64_t numberOfValuesWalked { 0 };
        bool hasSpentItsWork { false };
        // Null where every optional pass runs, in as many rounds as it takes.
        const PassSet* optionalPassesOfShortSchedule { nullptr };
        uint64_t maxRounds { std::numeric_limits<uint64_t>::max() };
        bool isStopped { false };
        PassSet passesThatRan;
        // None behind a change, until somebody asks.
        std::optional<BodyContents> contents;
        PassSet passesThatMayFire;
        // A run that says that it has changed nothing may still have added a value that stands in no list.
        size_t numberOfValuesInContents { 0 };
        std::array<bool, numberOfPasses> reachedFixpoint { };
        PassBits passesBehindChange;
        PassBits passesEnabledByTable;
        PassBits passesWithWork;
        PassBits passesThatWait;
        bool mayDecideTestsAcrossBlocks { true };
        bool recordsChangers { false };
        // There wherever recordsChangers holds.
        std::optional<NotesOfScheduleRun> notes;
        std::array<uint32_t, numberOfPasses> runs { };
        std::array<uint32_t, numberOfPasses> changes { };
        // None unless the last pass of the run was timed. The end of one pass is the start of the next, which so counts the little that lies between the two as its own.
        std::optional<PassClock> clockAtEndOfLastPass;
        Vector<PassID> changingRuns;
        ShapesWithMonotoneResult shapesWithMonotoneResult;
    };

    static constexpr unsigned numberOfSizeClasses = 16;

    // Every schedule run of every function adds to these. Threads share them out among themselves, so that no two wait for one line of memory.
    struct CountsOfPass {
        std::atomic<uint64_t> runs { 0 };
        std::atomic<uint64_t> changes { 0 };
    };
    static constexpr unsigned numberOfSharesOfCounts = 32;
    using CountsOfPasses = std::array<CountsOfPass, numberOfPasses>;

    struct FunctionPassTotals {
        std::atomic<uint64_t> runs { 0 };
        std::atomic<uint64_t> changes { 0 };
        std::atomic<uint64_t> nanoseconds { 0 };
        std::atomic<uint64_t> instructions { 0 };
        std::atomic<int64_t> valuesAdded { 0 };
        std::atomic<uint64_t> needlessDrops { 0 };
        std::atomic<uint64_t> nanosecondsWithoutChange { 0 };
        std::atomic<uint64_t> runsWithoutTrigger { 0 };
        std::atomic<uint64_t> runsThatKeptAnalyses { 0 };
        std::atomic<uint64_t> logEntries { 0 };
        std::atomic<uint64_t> runsThatMadeLogForget { 0 };
        std::atomic<uint64_t> idleRunsWithLogEntries { 0 };
    };

    static PassOutcome updateAnalyses(PassID, const ProgramEditor&, ProgramAnalyses&);
    void shrinkChangedFunctionsToFit(Program&, const ProgramEditor&) const;

    void checkFormAndInputs(const PassDescriptor&, const Program&) const;

    enum class MayChangeBodies : bool { No, Yes };
    std::optional<PassTimer> beginProgramPass(PassID, const Program&, MayChangeBodies = MayChangeBodies::No);
    void endProgramPass(PassID, const Program&, PassTimer, PassOutcome);
    void runTogether(NOESCAPE const ScopedLambda<void()>&, NOESCAPE const ScopedLambda<void()>&) const;
    bool canVerify(IRForm form) const { return form != IRForm::Placed || m_verifierInputs.placed; }
    void verifyAfterProgramPass(const PassDescriptor&, const Program&) const;

    PassOutcome runFunctionPass(PassID, FunctionPassContext&, ScheduleRun&) const;
    PassOutcome runFunctionPassThatMayFire(PassID, FunctionPassContext&, ScheduleRun&) const;
    enum class WhatFollows : uint8_t { MoreSteps, Nothing };
    void runSteps(std::span<const ScheduleStep>, FunctionPassContext&, ScheduleRun&, WhatFollows = WhatFollows::MoreSteps) const;
    void verifyAfterFunctionPass(const PassDescriptor&, FunctionPassContext&, ScheduleRun&) const;
    void verifySkippedFunctionPass(const PassDescriptor&, const FunctionPassContext&, ScheduleRun&) const;
    void stopBehind(PassID, ScheduleRun&) const;

    void prepare(ScheduleRun&) const;
    void addTotals(const ScheduleRun&, size_t numberOfValuesAtStart) const;
    void flushFunctionPassTotals();

    BuildContext& m_build;
    std::optional<PassID> m_stopAfter;
    bool m_isStopped { false };
    PassOutcome m_lastProgramPassOutcome { PassOutcome::Unchanged };
    OptionSet<ProgramResult> m_availableResults;
    VerifierInputs m_verifierInputs;
    mutable std::atomic<bool> m_didRunLastPass { false };
    mutable std::array<FunctionPassTotals, numberOfPasses> m_functionPassTotals { };
    mutable std::array<CountsOfPasses, numberOfSharesOfCounts> m_countsOfPasses { };
    mutable std::array<FunctionPassTotals, numberOfSizeClasses> m_totalsBySize { };
};

template<typename Functor>
auto PassRunner::runProgramPass(PassID id, Program& program, ProgramAnalyses& analyses, const Functor& functor) -> std::invoke_result_t<Functor, ProgramEditor&>
{
    auto timer = beginProgramPass(id, program, MayChangeBodies::Yes);
    if (!timer)
        return std::invoke_result_t<Functor, ProgramEditor&>();
    ProgramEditor editor(program);
    auto endPass = makeScopeExit([&] {
        shrinkChangedFunctionsToFit(program, editor);
        endProgramPass(id, program, *timer, updateAnalyses(id, editor, analyses));
    });
    return functor(editor);
}

template<typename Functor>
auto PassRunner::runProgramPassWithoutEditor(PassID id, Program& program, ProgramAnalyses& analyses, const Functor& functor, BodiesAreChanged bodiesAreChanged) -> std::invoke_result_t<Functor>
{
    auto timer = beginProgramPass(id, program, MayChangeBodies::Yes);
    if (!timer)
        return std::invoke_result_t<Functor>();
    analyses.willChangeBodies(bodiesAreChanged);
    auto endPass = makeScopeExit([&] {
        analyses.invalidate(descriptorOf(id).invalidatedResults);
        endProgramPass(id, program, *timer, PassOutcome::Changed);
    });
    return functor();
}

template<typename Functor>
auto PassRunner::runAnalysis(PassID id, const Program& program, const Functor& functor) -> std::invoke_result_t<Functor>
{
    auto timer = beginProgramPass(id, program);
    if (!timer)
        return std::invoke_result_t<Functor>();
    auto endPass = makeScopeExit([&] {
        endProgramPass(id, program, *timer, PassOutcome::Unchanged);
    });
    return functor();
}

template<typename Functor, typename Beside>
auto PassRunner::runAnalysisBeside(PassID id, const Program& program, const Functor& functor, const Beside& beside) -> std::invoke_result_t<Functor>
{
    return runAnalysisInTwoSteps(id, program, [] { }, functor, beside);
}

template<typename First, typename Second, typename Beside>
auto PassRunner::runAnalysisInTwoSteps(PassID id, const Program& program, const First& first, const Second& second, const Beside& beside) -> std::invoke_result_t<Second>
{
    using Result = std::invoke_result_t<Second>;
    RELEASE_ASSERT(mayRunAnalysesTogether());
    auto timer = beginProgramPass(id, program);
    if (!timer) {
        beside();
        return Result();
    }
    first();
    std::optional<Result> result;
    runTogether([&] {
        result = second();
    }, [&] {
        beside();
    });
    endProgramPass(id, program, *timer, PassOutcome::Unchanged);
    return WTF::move(*result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
