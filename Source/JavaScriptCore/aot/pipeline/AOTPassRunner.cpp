/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPassRunner.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowAnalysis.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRPrinter.h"
#include "AOTTypePropagation.h"
#include <wtf/CPUTime.h>
#include <wtf/DataLog.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static size_t findFixpointEnd(std::span<const ScheduleStep> steps, size_t begin)
{
    unsigned depth = 0;
    for (size_t index = begin; index < steps.size(); ++index) {
        switch (steps[index].kind) {
        case ScheduleStep::Kind::BeginFixpoint:
            ++depth;
            break;
        case ScheduleStep::Kind::EndFixpoint:
            if (!--depth)
                return index;
            break;
        case ScheduleStep::Kind::RunPass:
        case ScheduleStep::Kind::RunSchedule:
            break;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool acceptsForm(const PassDescriptor& descriptor, IRForm form)
{
    return descriptor.firstForm <= form && form <= descriptor.lastForm;
}

static void invalidateAnalyses(FunctionAnalyses& analyses, InvalidatedAnalyses invalidated)
{
    switch (invalidated) {
    case InvalidatedAnalyses::None:
        return;
    case InvalidatedAnalyses::Values:
        analyses.didChangeValues();
        return;
    case InvalidatedAnalyses::All:
        analyses.didChangeControlFlow();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasCodeBehindNeverValue(const Function& function, const Block& block)
{
    size_t index = block.values.findIf([&](ValueID id) {
        const Value& value = function.at(id);
        return value.type == TypeTable::never && resultRuleOf(value.opcode) != ResultRule::None;
    });
    if (index == notFound)
        return false;
    return index + 1 != block.values.size() || block.terminator.kind != TerminatorKind::Unreachable;
}

// A pass, or whoever built the function, may leave a conversion that always fails, or a value of whose type nothing is left, in the middle of a block.
// What stands behind it is never reached, and nothing may stand there. No pass has to look for that itself.
static bool cutCodeBehindNeverValues(FunctionPassContext& context)
{
    const Function& function = context.function;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        if (!hasCodeBehindNeverValue(function, function.blocks[index]) || !context.analyses.cfg().isReachable(BlockID(index)))
            continue;
        finishTypedValues(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build });
        return true;
    }
    return false;
}

PassRunner::PassRunner(BuildContext& build, std::optional<PassID> stopAfter)
    : m_build(build)
    , m_stopAfter(stopAfter)
    , m_isStopped(build.diagnostics().hasErrors())
{
}

PassOutcome PassRunner::runFunctionPass(PassID id, FunctionPassContext& context) const
{
    RELEASE_ASSERT(context.analyses.context().id == context.id);
    ScheduleRun run;
    run.functionAsRead = context.subject().function;
    PassOutcome outcome = runFunctionPass(id, context, run);
    addTotals(run);
    return outcome;
}

PassOutcome PassRunner::runSchedule(FunctionSchedule schedule, const Program& program, const OptimizationInputs& inputs, FunctionID id, Function& function, TaskArenas& arenas) const
{
    FunctionAnalyses analyses(AnalysisContext { m_build, program, &inputs.flow, &inputs.effects, id }, function);
    FunctionPassContext context { program, inputs, id, function, analyses, arenas.types, arenas.strings, m_build };
    ScheduleRun run;
    run.functionAsRead = context.subject().function;
    if (program.form() == IRForm::Prepared && cutCodeBehindNeverValues(context))
        ++run.numberOfChanges;
    runSteps(stepsOf(schedule), context, run);
    addTotals(run);
    return run.numberOfChanges ? PassOutcome::Changed : PassOutcome::Unchanged;
}

void PassRunner::didFinishFunctionLoop()
{
    flushFunctionPassTotals();
    if (m_build.diagnostics().hasErrors() || m_didRunLastPass.load(std::memory_order_relaxed))
        m_isStopped = true;
}

void PassRunner::didReachForm(const Program& program)
{
    if (m_build.options().logging.dumps(program.form()))
        dumpFunctions(program, nameOf(program.form()));
}

PassOutcome PassRunner::updateAnalyses(PassID id, const ProgramEditor& editor, ProgramAnalyses& analyses)
{
    if (editor.changedFunctions().isEmpty()) {
        analyses.invalidate({ }, editor);
        return PassOutcome::Unchanged;
    }
    analyses.invalidate(descriptorOf(id).invalidatedResults, editor);
    return PassOutcome::Changed;
}

void PassRunner::checkFormAndInputs(const PassDescriptor& descriptor, const Program& program) const
{
    RELEASE_ASSERT(acceptsForm(descriptor, program.form()));
    RELEASE_ASSERT(m_availableResults.containsAll(descriptor.inputs));
}

auto PassRunner::beginProgramPass(PassID id, const Program& program) -> std::optional<PassTimer>
{
    const PassDescriptor& descriptor = descriptorOf(id);
    RELEASE_ASSERT(descriptor.scope == PassScope::Program);
    m_lastProgramPassOutcome = PassOutcome::Unchanged;
    if (m_isStopped)
        return std::nullopt;
    if (descriptor.necessity == PassNecessity::Optional && !m_build.isEnabled(id, FunctionID())) {
        didCompute(descriptor.outputs);
        return std::nullopt;
    }
    checkFormAndInputs(descriptor, program);
    return PassTimer { MonotonicTime::now(), CPUTime::forCurrentThread() };
}

void PassRunner::endProgramPass(PassID id, const Program& program, PassTimer timer, PassOutcome outcome)
{
    const PassDescriptor& descriptor = descriptorOf(id);

    m_lastProgramPassOutcome = outcome;
    m_build.statistics().addPassTime(id, { .wall = MonotonicTime::now() - timer.wallStart, .cpu = CPUTime::forCurrentThread() - timer.cpuStart, .runs = 1 });
    if (outcome == PassOutcome::Changed && !descriptor.countsOwnChanges)
        m_build.statistics().countChanges(id, 1);
    flushFunctionPassTotals();

    didCompute(descriptor.outputs);

    if (outcome == PassOutcome::Changed) {
        m_build.resolveFunctionToDump(program);
        verifyAfterProgramPass(descriptor, program);
    }

    if (m_build.options().logging.dumpedPass == id)
        dumpFunctions(program, nameOf(id));

    updateIsStopped(id);
}

bool PassRunner::canVerify(IRForm form) const
{
    return form != IRForm::Placed || m_verifierInputs.placed;
}

void PassRunner::verifyAfterProgramPass(const PassDescriptor& descriptor, const Program& program) const
{
    if (!m_build.options().verifiesAfterEveryPass || descriptor.isVerifiedAfterNextPass)
        return;
    IRForm form = descriptor.produces.value_or(program.form());
    if (!canVerify(form))
        return;

    // Not ProgramAnalyses::effects(): summaries cached here would reach later passes, and the output would depend on this option.
    EffectSummaries effects;
    const PassDescriptor& summarizer = descriptorOf(PassID::ComputeEffectSummaries);
    if (m_verifierInputs.flow && acceptsForm(summarizer, program.form()) && m_build.isEnabled(summarizer.id, FunctionID()))
        effects = computeEffectSummaries(program, *m_verifierInputs.flow, m_build);

    VerifierInputs inputs = m_verifierInputs;
    inputs.isOptimizing = form == IRForm::Prepared;
    inputs.effects = &effects;
    if (!verifyProgram(program, form, m_build, inputs)) {
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAfterPass, .pass = descriptor.id });
        return;
    }
    if (descriptor.id == PassID::ApplyFlowResults) {
        RELEASE_ASSERT(m_verifierInputs.flow);
        validateFlowResults(program, *m_verifierInputs.flow, m_build);
    }
}

bool PassRunner::mayRunAnalysesTogether() const
{
    return !m_stopAfter && !m_build.options().logging.dumpedPass;
}

void PassRunner::runTogether(NOESCAPE const ScopedLambda<void()>& first, NOESCAPE const ScopedLambda<void()>& second) const
{
    forEachIndexInParallel(m_build.parallelLoops(), 2, [&](unsigned index) {
        index ? second() : first();
    });
}

void PassRunner::updateIsStopped(PassID id)
{
    if (m_build.diagnostics().hasErrors() || m_stopAfter == id || m_didRunLastPass.load(std::memory_order_relaxed))
        m_isStopped = true;
}

PassOutcome PassRunner::runFunctionPass(PassID id, FunctionPassContext& context, ScheduleRun& run) const
{
    const PassDescriptor& descriptor = descriptorOf(id);
    const unsigned index = static_cast<unsigned>(id);

    RELEASE_ASSERT(descriptor.scope == PassScope::Function);
    if (run.unchangedAt[index] == run.numberOfChanges + 1)
        return PassOutcome::Unchanged;
    if (descriptor.necessity == PassNecessity::Optional && !m_build.isEnabled(id, run.functionAsRead))
        return PassOutcome::Unchanged;
    checkFormAndInputs(descriptor, context.program);
    RELEASE_ASSERT(!descriptor.derivesFunctions || context.inputs.derivedFunctions);

    const bool isTimed = m_build.options().logging.logsPhaseTimes;
    MonotonicTime start = isTimed ? MonotonicTime::now() : MonotonicTime();
    ++run.runs[index];
    PassOutcome outcome = descriptor.run(context);
    if (isTimed)
        run.nanoseconds[index] += (MonotonicTime::now() - start).nanosecondsAs<uint64_t>();

    if (outcome == PassOutcome::Changed) {
        invalidateAnalyses(context.analyses, descriptor.invalidatedAnalyses);
        if (descriptor.produces.value_or(context.program.form()) == IRForm::Prepared && !descriptor.isVerifiedAfterNextPass)
            cutCodeBehindNeverValues(context);
        ++run.numberOfChanges;
        ++run.changes[index];
    } else
        run.unchangedAt[index] = run.numberOfChanges + 1;

    verifyAfterFunctionPass(descriptor, context, run);
    dumpAfterFunctionPass(id, context);

    if (m_stopAfter == id) [[unlikely]] {
        run.isStopped = true;
        m_didRunLastPass.store(true, std::memory_order_relaxed);
    }
    return outcome;
}

// The messages are worded at the end of the build, from the body that the program has then. This is the body that failed.
static String describeError(const InternalError& error, const Function& verifiedBody)
{
    StringBuilder description;
    description.append(nameOf(error.kind));
    if (error.block)
        description.append(", block b"_s, error.block.index());
    if (error.value) {
        description.append(", value v"_s, error.value.index());
        if (error.value.index() < verifiedBody.values.size())
            description.append(" ("_s, nameOf(verifiedBody.at(error.value).opcode), ')');
    }
    return description.toString();
}

void PassRunner::verifyAfterFunctionPass(const PassDescriptor& descriptor, FunctionPassContext& context, ScheduleRun& run) const
{
    if (!m_build.options().verifiesAfterEveryPass) [[likely]]
        return;
    IRForm form = descriptor.produces.value_or(context.program.form());
    VerifierInputs inputs = m_verifierInputs;
    inputs.isOptimizing = form == IRForm::Prepared;
    inputs.effects = &context.inputs.effects;
    Vector<InternalError> errors;
    if (canVerify(form) && !descriptor.isVerifiedAfterNextPass)
        errors = findErrorsInFunction(context.program, context.id, context.function, form, m_build, inputs);
    if (errors.isEmpty()) {
        context.analyses.verifyCached();
        return;
    }

    // A listing of many errors is cut short, so the line that names the pass also says what the verifier found.
    InternalError failure { .kind = InternalErrorKind::VerificationFailedAfterPass, .function = run.functionAsRead, .pass = descriptor.id, .detail = describeError(errors.first(), context.function) };
    for (InternalError& error : errors)
        error.pass = descriptor.id;
    m_build.diagnostics().append(WTF::move(errors));
    m_build.diagnostics().add(failure);
    run.isStopped = true;
}

void PassRunner::dumpAfterFunctionPass(PassID id, const FunctionPassContext& context) const
{
    if (m_build.options().logging.dumpedPass != id) [[likely]]
        return;
    if (!m_build.shouldDump(context.subject().function))
        return;
    Locker locker { m_dumpLock };
    PrintStream& out = WTF::dataFile();
    out.println("AOT IR after ", nameOf(id), ":");
    printFunction(out, context.program, context.id, context.function);
}

void PassRunner::runSteps(std::span<const ScheduleStep> steps, FunctionPassContext& context, ScheduleRun& run) const
{
    for (size_t index = 0; index < steps.size() && !run.isStopped; ++index) {
        const ScheduleStep& step = steps[index];
        switch (step.kind) {
        case ScheduleStep::Kind::RunPass:
            runFunctionPass(step.pass, context, run);
            break;
        case ScheduleStep::Kind::RunSchedule:
            runSteps(stepsOf(step.schedule), context, run);
            break;
        case ScheduleStep::Kind::BeginFixpoint: {
            size_t end = findFixpointEnd(steps, index);
            std::span<const ScheduleStep> body = steps.subspan(index + 1, end - index - 1);
            const uint64_t maxRounds = m_build.limit(step.limit);
            for (uint64_t round = 0; ; ++round) {
                if (round >= maxRounds) {
                    m_build.gaveUp(step.limit, context.subject());
                    break;
                }
                uint32_t numberOfChangesBefore = run.numberOfChanges;
                runSteps(body, context, run);
                if (run.numberOfChanges == numberOfChangesBefore || run.isStopped)
                    break;
            }
            index = end;
            break;
        }
        case ScheduleStep::Kind::EndFixpoint:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
}

void PassRunner::addTotals(const ScheduleRun& run) const
{
    for (unsigned index = 0; index < numberOfPasses; ++index) {
        if (!run.runs[index])
            continue;
        FunctionPassTotals& totals = m_functionPassTotals[index];
        totals.runs.fetch_add(run.runs[index], std::memory_order_relaxed);
        if (uint32_t changes = run.changes[index])
            totals.changes.fetch_add(changes, std::memory_order_relaxed);
        if (uint64_t nanoseconds = run.nanoseconds[index])
            totals.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    }
}

void PassRunner::flushFunctionPassTotals()
{
    for (PassID id : allMembersOf<PassID>) {
        FunctionPassTotals& totals = m_functionPassTotals[static_cast<unsigned>(id)];
        uint64_t runs = totals.runs.exchange(0, std::memory_order_relaxed);
        if (!runs)
            continue;
        uint64_t changes = totals.changes.exchange(0, std::memory_order_relaxed);
        uint64_t nanoseconds = totals.nanoseconds.exchange(0, std::memory_order_relaxed);
        m_build.statistics().addPassTime(id, { .wall = Seconds(0), .cpu = Seconds::fromNanoseconds(static_cast<double>(nanoseconds)), .runs = runs });
        m_build.statistics().countChanges(id, changes);
    }
}

void PassRunner::dumpFunctions(const Program& program, ASCIILiteral point) const
{
    PrintStream& out = WTF::dataFile();
    out.println("AOT IR at ", point, ":");
    // Without a choice of functions the text is the whole program, which the parser of IR text reads back.
    if (m_build.options().logging.functionToDump.span().empty()) {
        printProgram(out, program);
        out.println("AOT IR ends");
        return;
    }
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        if (function.hasBody() && m_build.shouldDump(id))
            printFunction(out, program, id, function);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
