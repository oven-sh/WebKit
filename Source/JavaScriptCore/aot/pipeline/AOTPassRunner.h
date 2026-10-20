/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInterningPhases.h"
#include "AOTPass.h"
#include "AOTProgramAnalyses.h"
#include "AOTVerifier.h"
#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

class PassRunner {
    WTF_MAKE_NONCOPYABLE(PassRunner);
public:
    explicit PassRunner(BuildContext&, std::optional<PassID> stopAfter = std::nullopt);

    BuildContext& build() const { return m_build; }

    PassOutcome runFunctionPass(PassID, FunctionPassContext&) const;
    PassOutcome runSchedule(FunctionSchedule, const Program&, const OptimizationInputs&, FunctionID, Function&, TaskArenas&) const;

    template<typename Functor> auto runProgramPass(PassID, Program&, ProgramAnalyses&, const Functor&) -> std::invoke_result_t<Functor, ProgramEditor&>;
    template<typename Functor> auto runProgramPassWithoutEditor(PassID, Program&, ProgramAnalyses&, const Functor&) -> std::invoke_result_t<Functor>;
    template<typename Functor> auto runAnalysis(PassID, const Program&, const Functor&) -> std::invoke_result_t<Functor>;
    // The analysis may not use the runner. What runs beside it may: it runs other analyses.
    template<typename Functor, typename Beside> auto runAnalysisBeside(PassID, const Program&, const Functor&, const Beside&) -> std::invoke_result_t<Functor>;
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
    struct PassTimer {
        MonotonicTime wallStart;
        Seconds cpuStart;
    };

    struct ScheduleRun {
        FunctionID functionAsRead;
        uint32_t numberOfChanges { 0 };
        bool isStopped { false };
        std::array<uint32_t, numberOfPasses> unchangedAt { };
        std::array<uint32_t, numberOfPasses> runs { };
        std::array<uint32_t, numberOfPasses> changes { };
        std::array<uint64_t, numberOfPasses> nanoseconds { };
    };

    struct FunctionPassTotals {
        std::atomic<uint64_t> runs { 0 };
        std::atomic<uint64_t> changes { 0 };
        std::atomic<uint64_t> nanoseconds { 0 };
    };

    static PassOutcome updateAnalyses(PassID, const ProgramEditor&, ProgramAnalyses&);

    void checkFormAndInputs(const PassDescriptor&, const Program&) const;

    std::optional<PassTimer> beginProgramPass(PassID, const Program&);
    void endProgramPass(PassID, const Program&, PassTimer, PassOutcome);
    void runTogether(NOESCAPE const ScopedLambda<void()>&, NOESCAPE const ScopedLambda<void()>&) const;
    bool canVerify(IRForm) const;
    void verifyAfterProgramPass(const PassDescriptor&, const Program&) const;
    void updateIsStopped(PassID);

    PassOutcome runFunctionPass(PassID, FunctionPassContext&, ScheduleRun&) const;
    void runSteps(std::span<const ScheduleStep>, FunctionPassContext&, ScheduleRun&) const;
    void verifyAfterFunctionPass(const PassDescriptor&, FunctionPassContext&, ScheduleRun&) const;
    void dumpAfterFunctionPass(PassID, const FunctionPassContext&) const;

    void addTotals(const ScheduleRun&) const;
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
        endProgramPass(id, program, *timer, updateAnalyses(id, editor, analyses));
    } else {
        Result result = functor(editor);
        endProgramPass(id, program, *timer, updateAnalyses(id, editor, analyses));
        return result;
    }
}

template<typename Functor>
auto PassRunner::runProgramPassWithoutEditor(PassID id, Program& program, ProgramAnalyses& analyses, const Functor& functor) -> std::invoke_result_t<Functor>
{
    using Result = std::invoke_result_t<Functor>;
    auto timer = beginProgramPass(id, program);
    if (!timer)
        return Result();
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
