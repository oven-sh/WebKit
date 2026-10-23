/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInterningPhases.h"
#include "AOTOpcode.h"
#include "AOTPass.h"
#include "AOTPassTriggers.h"
#include "AOTProgramAnalyses.h"
#include "AOTResultRuleMonotonicity.h"
#include "AOTVerifier.h"
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/BitSet.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

// The passes whose changes may give this pass something to do.
const PassSet& passesEnabling(PassID);
const PassSet& passesThatCanWaitBehind(PassID changer);

void shrinkToFit(Function&);

enum class BodiesAreChanged : uint8_t { Anyhow, ThroughEditors };

class PassRunner {
    WTF_MAKE_NONCOPYABLE(PassRunner);
public:
    explicit PassRunner(BuildContext&, std::optional<PassID> stopAfter = std::nullopt);

    BuildContext& build() const { return m_build; }

    PassOutcome runFunctionPass(PassID, FunctionPassContext&) const;
    PassOutcome runSchedule(FunctionSchedule, const Program&, const OptimizationInputs&, FunctionID, Function&, TaskArenas&) const;

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
    void didReachForm(const Program&);

    void setVerifierInputs(const VerifierInputs& inputs) { m_verifierInputs = inputs; }
    const VerifierInputs& verifierInputs() const { return m_verifierInputs; }

    void didCompute(OptionSet<ProgramResult> results) { m_availableResults.add(results); }
    void didInvalidate(OptionSet<ProgramResult> results) { m_availableResults.remove(results); }

private:
    using PassBits = WTF::BitSet<numberOfPasses>;

    static const PassBits& passesEnabledBy(PassID);
    static const PassBits& passesGivenWorkBy(PassID);

    struct PassTimer {
        MonotonicTime wallStart;
        Seconds cpuStart;
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
        std::array<uint64_t, numberOfPasses> codeBytesAdded { };
        std::array<uint64_t, numberOfPasses> codeBytesRemoved { };
    };

    struct ScheduleRun {
        void didChange(PassID changer, const PassSet& passesLeftIdle = { }, const PassSet& passesThatCanWait = { })
        {
            PassBits enabled = passesEnabledBy(changer);
            passesBehindChange.setAll();
            passesEnabledByTable.merge(enabled);
            mayDecideTestsAcrossBlocks |= changer != PassID::SimplifyControlFlow && changer != PassID::EliminateDeadCode;
            passesLeftIdle.forEach([&](PassID pass) {
                enabled.clear(static_cast<unsigned>(pass));
            });
            PassBits givenWork = enabled;
            givenWork.filter(passesGivenWorkBy(changer));
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

        const BodyContents& contentsOf(const Function& function)
        {
            if (!contents || numberOfValuesInContents != function.values.size()) {
                contents = Typed::contentsOf(function);
                passesThatMayFire = passesThatMayFireIn(*contents);
                numberOfValuesInContents = function.values.size();
            }
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
        PassesByTrigger passesThatMayFire;
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
        std::optional<uint64_t> codeBytes;
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
        std::atomic<uint64_t> codeBytesAdded { 0 };
        std::atomic<uint64_t> codeBytesRemoved { 0 };
        std::atomic<uint64_t> needlessDrops { 0 };
        std::atomic<uint64_t> nanosecondsWithoutChange { 0 };
        std::atomic<uint64_t> runsWithoutTrigger { 0 };
        std::atomic<uint64_t> runsThatKeptAnalyses { 0 };
        std::atomic<uint64_t> logEntries { 0 };
        std::atomic<uint64_t> runsThatMadeLogForget { 0 };
        std::atomic<uint64_t> idleRunsWithLogEntries { 0 };
    };

    // What the passes of the schedule Simplify still find to do where the schedule begins.
    struct ChangesBehindSimplify {
        std::atomic<uint64_t> runs { 0 };
        std::atomic<uint64_t> functions { 0 };
        std::atomic<uint64_t> functionsWithLoop { 0 };
        std::atomic<uint32_t> firstFunction { std::numeric_limits<uint32_t>::max() };
    };

    static PassOutcome updateAnalyses(PassID, const ProgramEditor&, ProgramAnalyses&);
    void shrinkChangedFunctionsToFit(Program&, const ProgramEditor&) const;

    void checkFormAndInputs(const PassDescriptor&, const Program&) const;

    std::optional<PassTimer> beginProgramPass(PassID, const Program&);
    void endProgramPass(PassID, const Program&, PassTimer, PassOutcome);
    void runTogether(NOESCAPE const ScopedLambda<void()>&, NOESCAPE const ScopedLambda<void()>&) const;
    bool canVerify(IRForm) const;
    void verifyAfterProgramPass(const PassDescriptor&, const Program&) const;
    void updateIsStopped(PassID);

    PassOutcome runFunctionPass(PassID, FunctionPassContext&, ScheduleRun&) const;
    PassOutcome runFunctionPassThatMayFire(PassID, FunctionPassContext&, ScheduleRun&) const;
    enum class WhatFollows : uint8_t { MoreSteps, Nothing };
    void runSteps(std::span<const ScheduleStep>, FunctionPassContext&, ScheduleRun&, WhatFollows = WhatFollows::MoreSteps) const;
    void verifyAfterFunctionPass(const PassDescriptor&, FunctionPassContext&, ScheduleRun&) const;
    void verifySkippedFunctionPass(const PassDescriptor&, const FunctionPassContext&, ScheduleRun&) const;
    void countChangesBehindSimplify(std::span<const ScheduleStep>, const FunctionPassContext&, const ScheduleRun&) const;
    void reportChangesBehindSimplify();
    void dumpAfterFunctionPass(PassID, const FunctionPassContext&) const;
    void stopBehind(PassID, ScheduleRun&) const;

    void prepare(ScheduleRun&) const;
    void addTotals(const ScheduleRun&, size_t numberOfValuesAtStart) const;
    void flushFunctionPassTotals();
    void dumpFunctions(const Program&, ASCIILiteral point) const;

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
    mutable std::array<ChangesBehindSimplify, numberOfPasses> m_changesBehindSimplify { };
    // For the names in the report. Null until a change is counted.
    mutable std::atomic<const Program*> m_programWithChangesBehindSimplify { nullptr };
    mutable Lock m_dumpLock;
};

template<typename Functor>
auto PassRunner::runProgramPass(PassID id, Program& program, ProgramAnalyses& analyses, const Functor& functor) -> std::invoke_result_t<Functor, ProgramEditor&>
{
    using Result = std::invoke_result_t<Functor, ProgramEditor&>;
    auto timer = beginProgramPass(id, program);
    if (!timer)
        return Result();
    ProgramEditor editor(program);
    if constexpr (std::is_void_v<Result>) {
        functor(editor);
        shrinkChangedFunctionsToFit(program, editor);
        endProgramPass(id, program, *timer, updateAnalyses(id, editor, analyses));
    } else {
        Result result = functor(editor);
        shrinkChangedFunctionsToFit(program, editor);
        endProgramPass(id, program, *timer, updateAnalyses(id, editor, analyses));
        return result;
    }
}

template<typename Functor>
auto PassRunner::runProgramPassWithoutEditor(PassID id, Program& program, ProgramAnalyses& analyses, const Functor& functor, BodiesAreChanged bodiesAreChanged) -> std::invoke_result_t<Functor>
{
    using Result = std::invoke_result_t<Functor>;
    auto timer = beginProgramPass(id, program);
    if (!timer)
        return Result();
    if (bodiesAreChanged == BodiesAreChanged::ThroughEditors)
        analyses.willChangeBodiesThroughOwnEditors();
    else
        analyses.willChangeBodiesWithoutEditor();
    if constexpr (std::is_void_v<Result>) {
        functor();
        analyses.invalidate(descriptorOf(id).invalidatedResults);
        endProgramPass(id, program, *timer, PassOutcome::Changed);
    } else {
        Result result = functor();
        analyses.invalidate(descriptorOf(id).invalidatedResults);
        endProgramPass(id, program, *timer, PassOutcome::Changed);
        return result;
    }
}

template<typename Functor>
auto PassRunner::runAnalysis(PassID id, const Program& program, const Functor& functor) -> std::invoke_result_t<Functor>
{
    using Result = std::invoke_result_t<Functor>;
    auto timer = beginProgramPass(id, program);
    if (!timer)
        return Result();
    if constexpr (std::is_void_v<Result>) {
        functor();
        endProgramPass(id, program, *timer, PassOutcome::Unchanged);
    } else {
        Result result = functor();
        endProgramPass(id, program, *timer, PassOutcome::Unchanged);
        return result;
    }
}

template<typename Functor, typename Beside>
auto PassRunner::runAnalysisBeside(PassID id, const Program& program, const Functor& functor, const Beside& beside) -> std::invoke_result_t<Functor>
{
    using Result = std::invoke_result_t<Functor>;
    RELEASE_ASSERT(mayRunAnalysesTogether());
    auto timer = beginProgramPass(id, program);
    if (!timer) {
        beside();
        return Result();
    }
    std::optional<Result> result;
    runTogether([&] {
        result = functor();
    }, [&] {
        beside();
    });
    endProgramPass(id, program, *timer, PassOutcome::Unchanged);
    return WTF::move(*result);
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
