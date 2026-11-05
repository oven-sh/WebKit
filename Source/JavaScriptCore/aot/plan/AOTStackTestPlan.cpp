/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackTestPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallingConventions.h"
#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTPlan.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThinSummary.h"
#include <limits>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mostOperationsOfBodyBuiltIntoFrameOfCaller = 40;

static_assert(maxNestedFunctionsWithoutStackCheck <= std::numeric_limits<uint8_t>::max());

static constexpr size_t maxEntryFramesWithoutStackCheck = 3;
static_assert((maxNestedFunctionsWithoutStackCheck + maxEntryFramesWithoutStackCheck) * (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*)) + sizeOfSpilledArgumentRegisters + maxUncheckedFrameSize + 2 * sizeof(void*) <= maxStackUsedWithoutCheck);

namespace {

using CallAtBareSiteForStackTests = std::pair<FunctionID, FunctionID>;

class CallsListedForStackTests {
    WTF_MAKE_NONCOPYABLE(CallsListedForStackTests);
public:
    CallsListedForStackTests(const Program&, const Facts&, const ModuleEvaluationPlan&, const ParallelLoops&);
    CallsListedForStackTests(Vector<uint32_t>&& firstCalleeOf, Vector<FunctionID>&& callees, const ModuleEvaluationPlan&);

    std::span<const FunctionID> calleesOf(FunctionID caller) const { return m_callees.span().subspan(m_firstCalleeOf[caller.index()], m_firstCalleeOf[caller.index() + 1] - m_firstCalleeOf[caller.index()]); }
    std::span<const CallAtBareSiteForStackTests> stepsAtBareSitesOf(FunctionID caller) const
    {
        auto found = std::ranges::equal_range(m_atBareSites.span(), caller, { }, &CallAtBareSiteForStackTests::first);
        return { found.begin(), found.end() };
    }
    const Vector<CallAtBareSiteForStackTests>& atBareSites() const { return m_atBareSites; }

private:
    void listStepsAtBareSites(const ModuleEvaluationPlan&);

    Vector<uint32_t> m_firstCalleeOf;
    Vector<FunctionID> m_callees;
    Vector<CallAtBareSiteForStackTests> m_atBareSites;
};

void listCalleesForStackTests(const Program& program, const Facts& facts, const ParallelLoops& loops, Vector<uint32_t>& firstCalleeOf, Vector<FunctionID>& calleesOfAll)
{
    auto callsListedUnder = [](const LinkTables& tables, FunctionID caller) -> std::span<const LinkedCall> {
        if (caller.index() + 1 >= tables.firstCallOfCaller.size())
            return { };
        uint32_t first = tables.firstCallOfCaller[caller.index()];
        return tables.calls.span().subspan(first, tables.firstCallOfCaller[caller.index() + 1] - first);
    };

    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    firstCalleeOf.fill(0, numberOfFunctions + 1);
    Vector<Vector<FunctionID>> calleesInChunks = collectChunksInParallel<FunctionID>(loops, numberOfFunctions, [&](unsigned index, Vector<FunctionID>& callees) {
        size_t first = callees.size();
        for (const LinkTables* tables : { &facts.linked(), &facts.held() }) {
            for (const LinkedCall& call : callsListedUnder(*tables, FunctionID(index)))
                callees.append(call.callee);
        }
        std::span<FunctionID> added = callees.mutableSpan().subspan(first);
        std::ranges::sort(added);
        callees.shrink(callees.size() - std::ranges::unique(added).size());
        firstCalleeOf[index + 1] = static_cast<uint32_t>(callees.size() - first);
    });
    for (uint32_t index = 0; index < numberOfFunctions; ++index)
        firstCalleeOf[index + 1] += firstCalleeOf[index];
    calleesOfAll = joinChunks<FunctionID>(loops, WTF::move(calleesInChunks));
}

CallsListedForStackTests::CallsListedForStackTests(const Program& program, const Facts& facts, const ModuleEvaluationPlan& evaluation, const ParallelLoops& loops)
{
    listCalleesForStackTests(program, facts, loops, m_firstCalleeOf, m_callees);
    listStepsAtBareSites(evaluation);
}

CallsListedForStackTests::CallsListedForStackTests(Vector<uint32_t>&& firstCalleeOf, Vector<FunctionID>&& callees, const ModuleEvaluationPlan& evaluation)
    : m_firstCalleeOf(WTF::move(firstCalleeOf))
    , m_callees(WTF::move(callees))
{
    listStepsAtBareSites(evaluation);
}

void CallsListedForStackTests::listStepsAtBareSites(const ModuleEvaluationPlan& evaluation)
{
    for (const BareModuleSite& site : evaluation.bareSites()) {
        for (const ModuleStep& step : evaluation.stepsOf(site)) {
            if (step.callee && !step.calleeIsStart)
                m_atBareSites.append({ site.function, step.callee });
        }
    }
    std::ranges::sort(m_atBareSites);
    m_atBareSites.shrink(m_atBareSites.size() - std::ranges::unique(m_atBareSites).size());
}

class CounterOfCoveredFrames {
    WTF_MAKE_NONCOPYABLE(CounterOfCoveredFrames);
public:
    CounterOfCoveredFrames(const Program&, const Facts&, const CallingConventions&, const ModuleEvaluationPlan&, const ParallelLoops&, CallsCountedForStackTests&&);

    StackTestPlan run();

private:
    enum class BodyTrait : uint8_t {
        MayBeCovered = 1 << 0,
        MayBeInlined = 1 << 1,
        IsBodyOfModule = 1 << 2,
        HasBodyThatNeverSuspends = 1 << 3,
    };

    enum class Count : uint8_t { NotBegun, InProgress, Done };

    struct CountInProgress {
        FunctionID function;
        std::span<const CallAtBareSiteForStackTests> stepsAtBareSites;
        uint32_t nextCallee { 0 };
        uint32_t nextStep { 0 };
    };

    bool bodyMaySuspend(FunctionID id) const { return m_bodiesThatMaySuspend.quickGet(id.index()); }
    OptionSet<BodyTrait> traitsOf(FunctionID, const BitVector& methodsOfTable) const;
    bool needsNoCount(FunctionID id) const { return m_traits[id.index()].isEmpty() || m_counts[id.index()] == Count::Done; }
    bool addsNothingBelowCaller(FunctionID id) const { return needsNoCount(id) || !m_traits[id.index()].containsAny({ BodyTrait::MayBeCovered, BodyTrait::MayBeInlined }); }
    std::span<const FunctionID> calleesCountedBelow(FunctionID) const;
    void beginCount(FunctionID);
    FunctionID takeNextCalleeToCount();
    void finishCount(const CountInProgress&);
    void countCoveredFrames();

    const Program& m_program;
    const Facts& m_facts;
    const CallingConventions& m_conventions;
    const ParallelLoops& m_loops;
    CallsListedForStackTests m_calls;
    StackTestPlan m_plan;

    BitVector m_bodiesThatMaySuspend;
    BitVector m_functionsInCycles;
    Vector<OptionSet<BodyTrait>> m_traits;
    Vector<Count> m_counts;
    Vector<CountInProgress> m_countsInProgress;
};

CounterOfCoveredFrames::CounterOfCoveredFrames(const Program& program, const Facts& facts, const CallingConventions& conventions, const ModuleEvaluationPlan& evaluation, const ParallelLoops& loops, CallsCountedForStackTests&& callsCounted)
    : m_program(program)
    , m_facts(facts)
    , m_conventions(conventions)
    , m_loops(loops)
    , m_calls(WTF::move(callsCounted.firstCalleeOf), WTF::move(callsCounted.callees), evaluation)
    , m_bodiesThatMaySuspend(WTF::move(callsCounted.bodiesThatMaySuspend))
    , m_functionsInCycles(WTF::move(callsCounted.functionsInCycles))
{
    size_t numberOfFunctions = program.functions.size();
    m_plan.bodiesCoveredByCallers = BitVector(numberOfFunctions);
    m_plan.slowBodiesCoveredByCallers = BitVector(numberOfFunctions);
    m_plan.coroutineBodiesCoveredByCallers = BitVector(numberOfFunctions);
    m_plan.bodiesWhoseCallsAreCountedBelowCallers = BitVector(numberOfFunctions);
    m_plan.coveredFramesOfCall.fill(0, numberOfFunctions);
    m_plan.callsCountedAtBareSites = m_calls.atBareSites();
    m_traits.grow(numberOfFunctions);
    m_counts.fill(Count::NotBegun, numberOfFunctions);
}

auto CounterOfCoveredFrames::traitsOf(FunctionID id, const BitVector& methodsOfTable) const -> OptionSet<BodyTrait>
{
    const Function& function = m_program.at(id);
    FunctionConvention convention = m_conventions.of(id);
    bool isCoroutine = function.isCoroutine();
    if (!convention.has(CodeRole::Body) || (isCoroutine ? function.isModuleBody() : m_facts.summaries().at(id).has(SummaryTrait::Suspends)))
        return { };
    if (function.isModuleBody())
        return { BodyTrait::MayBeCovered, BodyTrait::IsBodyOfModule };

    OptionSet<BodyTrait> traits;
    bool isSmallEnough = m_facts.summaries().at(id).numberOfOperations <= mostOperationsOfBodyBuiltIntoFrameOfCaller || isLibraryCode(m_program, function);
    if (!m_functionsInCycles.get(id.index()) && isSmallEnough && !bodyMaySuspend(id))
        traits.add(BodyTrait::MayBeInlined);
    if (!methodsOfTable.get(id.index()))
        traits.add(BodyTrait::MayBeCovered);
    if (convention.coroutineNeverSuspends)
        traits.add(BodyTrait::HasBodyThatNeverSuspends);
    return traits;
}

std::span<const FunctionID> CounterOfCoveredFrames::calleesCountedBelow(FunctionID id) const
{
    if (bodyMaySuspend(id))
        return { };
    return m_calls.calleesOf(id);
}

void CounterOfCoveredFrames::beginCount(FunctionID id)
{
    m_counts[id.index()] = Count::InProgress;
    m_countsInProgress.append(CountInProgress { id, m_calls.stepsAtBareSitesOf(id) });
}

FunctionID CounterOfCoveredFrames::takeNextCalleeToCount()
{
    CountInProgress& top = m_countsInProgress.last();
    std::span<const FunctionID> callees = calleesCountedBelow(top.function);
    while (top.nextCallee < callees.size()) {
        FunctionID callee = callees[top.nextCallee++];
        if (!addsNothingBelowCaller(callee) && !m_traits[callee.index()].contains(BodyTrait::IsBodyOfModule))
            return callee;
    }
    while (top.nextStep < top.stepsAtBareSites.size()) {
        FunctionID callee = top.stepsAtBareSites[top.nextStep++].second;
        if (!needsNoCount(callee))
            return callee;
    }
    return { };
}

void CounterOfCoveredFrames::finishCount(const CountInProgress& finished)
{
    FunctionID id = finished.function;
    OptionSet<BodyTrait> traits = m_traits[id.index()];

    unsigned framesBelow = 0;
    for (FunctionID callee : calleesCountedBelow(id)) {
        if (!m_traits[callee.index()].contains(BodyTrait::IsBodyOfModule))
            framesBelow = std::max<unsigned>(framesBelow, m_plan.coveredFramesOfCall[callee.index()]);
    }
    for (const CallAtBareSiteForStackTests& step : finished.stepsAtBareSites)
        framesBelow = std::max<unsigned>(framesBelow, m_plan.coveredFramesOfCall[step.second.index()]);

    unsigned framesOfCall = framesBelow + 1;
    bool isCoroutine = m_conventions.of(id).has(CodeRole::CoroutineBody);
    bool coversBodyOfCoroutine = false;
    if (isCoroutine) {
        coversBodyOfCoroutine = traits.containsAll({ BodyTrait::HasBodyThatNeverSuspends, BodyTrait::MayBeCovered }) && framesOfCall < maxNestedFunctionsWithoutStackCheck;
        framesOfCall = coversBodyOfCoroutine ? framesOfCall + 1 : 1;
    }
    uint8_t counted = 0;
    bool isCovered = traits.contains(BodyTrait::MayBeCovered) && framesOfCall <= maxNestedFunctionsWithoutStackCheck;
    if (isCovered) {
        m_plan.bodiesCoveredByCallers.quickSet(id.index());
        counted = static_cast<uint8_t>(framesOfCall);
    }
    if (coversBodyOfCoroutine)
        m_plan.coroutineBodiesCoveredByCallers.quickSet(id.index());
    if (traits.contains(BodyTrait::MayBeInlined))
        counted = std::max(counted, static_cast<uint8_t>(framesBelow));
    bool frameOfBodyIsCounted = isCoroutine ? coversBodyOfCoroutine : isCovered;
    if (frameOfBodyIsCounted)
        m_plan.slowBodiesCoveredByCallers.quickSet(id.index());
    if (!traits.contains(BodyTrait::IsBodyOfModule) && (traits.contains(BodyTrait::MayBeInlined) || frameOfBodyIsCounted))
        m_plan.bodiesWhoseCallsAreCountedBelowCallers.quickSet(id.index());
    m_counts[id.index()] = Count::Done;
    m_plan.coveredFramesOfCall[id.index()] = counted;
}

void CounterOfCoveredFrames::countCoveredFrames()
{
    for (uint32_t index = 0; index < m_traits.size(); ++index) {
        FunctionID root(index);
        if (needsNoCount(root))
            continue;
        beginCount(root);
        while (!m_countsInProgress.isEmpty()) {
            FunctionID callee = takeNextCalleeToCount();
            if (!callee)
                finishCount(m_countsInProgress.takeLast());
            else if (m_counts[callee.index()] == Count::NotBegun)
                beginCount(callee);
            else {
                m_traits[callee.index()].remove(BodyTrait::MayBeCovered);
            }
        }
    }
}

StackTestPlan CounterOfCoveredFrames::run()
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(m_traits.size());

    BitVector methodsOfTable(numberOfFunctions);
    for (const MethodFamily& family : m_conventions.methodFamilies()) {
        for (FunctionID method : family.methods)
            methodsOfTable.set(method.index());
    }
    forEachRangeOfIndicesInParallel(m_loops, numberOfFunctions, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            m_traits[index] = traitsOf(FunctionID(static_cast<uint32_t>(index)), methodsOfTable);
    });
    countCoveredFrames();
    return WTF::move(m_plan);
}

} // anonymous namespace

CallsCountedForStackTests findCallsCountedForStackTests(const Program& program, const Facts& facts, const CallingConventions& conventions, const ParallelLoops& loops)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    CallsCountedForStackTests result;
    listCalleesForStackTests(program, facts, loops, result.firstCalleeOf, result.callees);
    result.bodiesThatMaySuspend = BitVector(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        const FunctionConvention& convention = conventions.of(FunctionID(index));
        if (convention.has(CodeRole::CoroutineBody) && !convention.coroutineNeverSuspends)
            result.bodiesThatMaySuspend.quickSet(index);
    }
    StronglyConnectedComponents cyclesOfCountedCalls = computeStronglyConnectedComponents(numberOfFunctions, [&](unsigned node) -> std::span<const FunctionID> {
        if (result.bodiesThatMaySuspend.quickGet(node))
            return { };
        return result.callees.span().subspan(result.firstCalleeOf[node], result.firstCalleeOf[node + 1] - result.firstCalleeOf[node]);
    });
    result.functionsInCycles = WTF::move(cyclesOfCountedCalls.isInCycle);
    return result;
}

StackTestPlan planStackTests(const Program& program, const Facts& facts, const CallingConventions& conventions, const ModuleEvaluationPlan& evaluation, const ParallelLoops& loops, CallsCountedForStackTests&& callsCounted)
{
    return CounterOfCoveredFrames(program, facts, conventions, evaluation, loops, WTF::move(callsCounted)).run();
}

void checkStackTests(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    const CallingConventions& conventions = plan.conventions();
    const StackTestPlan& tests = conventions.stackTests();
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    if (tests.coveredFramesOfCall.size() != numberOfFunctions)
        return;

    auto expect = [&](bool holds, FunctionID function, uint32_t count, ASCIILiteral words) {
        if (holds)
            return;
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .function = function,
            .count = count,
            .detail = String(words),
        });
    };
    auto isCovered = [&](FunctionID function) {
        return tests.bodiesCoveredByCallers.get(function.index());
    };
    auto bodyOfCoroutineIsCovered = [&](FunctionID function) {
        return tests.coroutineBodiesCoveredByCallers.get(function.index());
    };

    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        FunctionConvention convention = conventions.of(id);
        expect(!bodyOfCoroutineIsCovered(id) || (convention.has(CodeRole::CoroutineBody) && convention.coroutineNeverSuspends && isCovered(id)), id, 0, "the body of a coroutine makes no stack test, and it may suspend or its ramp tests"_s);
        expect(!isCovered(id) || (convention.has(CodeRole::Body) && !(function.isCoroutine() && function.isModuleBody())), id, 0, "a body makes no stack test, and somebody enters it whom nobody counts"_s);
        expect(!tests.slowBodiesCoveredByCallers.get(index) || (isCovered(id) && (!convention.has(CodeRole::CoroutineBody) || bodyOfCoroutineIsCovered(id))), id, 0, "a slow version makes no stack test, and the body whose place it takes tests"_s);
    }
    for (const MethodFamily& family : conventions.methodFamilies()) {
        for (FunctionID method : family.methods)
            expect(!isCovered(method), method, 0, "a method of the method table makes no stack test"_s);
    }

    enum class Height : uint8_t { NotBegun, InProgress, Known };
    struct CallerOnPath {
        FunctionID function;
        std::span<const CallAtBareSiteForStackTests> stepsAtBareSites;
        uint32_t nextCallee { 0 };
        uint32_t nextStep { 0 };
    };

    CallsListedForStackTests calls(program, facts, plan.moduleEvaluation(), build.parallelLoops());
    Vector<Height> heights(FillWith { }, numberOfFunctions, Height::NotBegun);
    Vector<uint32_t> framesOfCall(FillWith { }, numberOfFunctions, 0u);
    Vector<CallerOnPath> path;

    auto bodyOfCoroutineTestsForItself = [&](FunctionID function) {
        return conventions.of(function).has(CodeRole::CoroutineBody) && !bodyOfCoroutineIsCovered(function);
    };
    auto calleesCountedBelow = [&](FunctionID function) -> std::span<const FunctionID> {
        if (bodyOfCoroutineTestsForItself(function) && !tests.countsCallsOfBodyBelowCallers(function))
            return { };
        return calls.calleesOf(function);
    };
    auto addsNothingBelowCaller = [&](FunctionID function) {
        return heights[function.index()] == Height::Known || (!isCovered(function) && !tests.countsCallsOfBodyBelowCallers(function));
    };
    auto addToPath = [&](FunctionID function) {
        heights[function.index()] = Height::InProgress;
        path.append(CallerOnPath { function, calls.stepsAtBareSitesOf(function) });
    };
    auto takeNextCallee = [&]() -> FunctionID {
        CallerOnPath& top = path.last();
        std::span<const FunctionID> callees = calleesCountedBelow(top.function);
        while (top.nextCallee < callees.size()) {
            FunctionID callee = callees[top.nextCallee++];
            if (!addsNothingBelowCaller(callee) && !program.at(callee).isModuleBody())
                return callee;
        }
        while (top.nextStep < top.stepsAtBareSites.size()) {
            FunctionID callee = top.stepsAtBareSites[top.nextStep++].second;
            if (!addsNothingBelowCaller(callee))
                return callee;
        }
        return { };
    };
    auto checkHeightOf = [&](const CallerOnPath& caller) {
        FunctionID function = caller.function;
        uint32_t framesBelow = 0;
        for (FunctionID callee : calleesCountedBelow(function)) {
            if (!program.at(callee).isModuleBody())
                framesBelow = std::max(framesBelow, framesOfCall[callee.index()]);
        }
        for (const CallAtBareSiteForStackTests& step : caller.stepsAtBareSites)
            framesBelow = std::max(framesBelow, framesOfCall[step.second.index()]);

        uint32_t height = (bodyOfCoroutineTestsForItself(function) ? 0 : framesBelow) + isCovered(function) + bodyOfCoroutineIsCovered(function);
        heights[function.index()] = Height::Known;
        expect(!isCovered(function) || height <= maxNestedFunctionsWithoutStackCheck, function, height, "more frames without a stack test stand below a test than there is room for"_s);
        height = std::max(height, framesBelow);
        expect(height <= tests.coveredFramesOfCall[function.index()], function, height, "a call puts more frames without a stack test below its caller than the plan has counted"_s);
        framesOfCall[function.index()] = height;
    };

    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (addsNothingBelowCaller(FunctionID(index)))
            continue;
        addToPath(FunctionID(index));
        while (!path.isEmpty()) {
            FunctionID callee = takeNextCallee();
            if (!callee)
                checkHeightOf(path.takeLast());
            else if (heights[callee.index()] == Height::NotBegun)
                addToPath(callee);
            else
                expect(false, callee, 0, "a ring of calls has no member that tests the stack"_s);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
