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

// As large a body as Inlining copies in a loop. It decides what pays alone: a copy of a larger body brings calls that nobody has counted, and its unit tests.
static constexpr uint32_t mostOperationsOfBodyBuiltIntoFrameOfCaller = 40;

static_assert(maxNestedFunctionsWithoutStackCheck <= std::numeric_limits<uint8_t>::max());

// Besides the frames counted here, three entry frames may stand below a stack check: that of a generic entry, which is entered right after the check made by whoever calls through the word of a closure,
// that of an entry of the method table above a method that tests, and that of a parameter guard in front of the version that makes the next check.
// The body that a generic entry leads to may be covered too. Its frames are among those counted here.
// The room that is left has to hold the argument registers that a generic entry spills, and the frame of a stub or the frame record that a function pushes before its own check.
static constexpr size_t maxEntryFramesWithoutStackCheck = 3;
static_assert((maxNestedFunctionsWithoutStackCheck + maxEntryFramesWithoutStackCheck) * (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*)) + sizeOfSpilledArgumentRegisters + maxUncheckedFrameSize + 2 * sizeof(void*) <= maxStackUsedWithoutCheck);

namespace {

using CallAtBareSiteForStackTests = std::pair<FunctionID, FunctionID>;

// By function: whom the calls enter that linked() or held() lists under it, each once, and the bodies that the steps at its bare sites call.
class CallsListedForStackTests {
    WTF_MAKE_NONCOPYABLE(CallsListedForStackTests);
public:
    CallsListedForStackTests(const Program&, const Facts&, const Plan&);

    std::span<const FunctionID> calleesOf(FunctionID caller) const { return m_callees.span().subspan(m_firstCalleeOf[caller.index()], m_firstCalleeOf[caller.index() + 1] - m_firstCalleeOf[caller.index()]); }
    std::span<const CallAtBareSiteForStackTests> stepsAtBareSitesOf(FunctionID caller) const
    {
        auto found = std::ranges::equal_range(m_atBareSites.span(), caller, { }, &CallAtBareSiteForStackTests::first);
        return { found.begin(), found.end() };
    }
    const Vector<CallAtBareSiteForStackTests>& atBareSites() const { return m_atBareSites; }

private:
    Vector<uint32_t> m_firstCalleeOf;
    Vector<FunctionID> m_callees;
    Vector<CallAtBareSiteForStackTests> m_atBareSites;
};

CallsListedForStackTests::CallsListedForStackTests(const Program& program, const Facts& facts, const Plan& plan)
{
    auto callsListedUnder = [](const LinkTables& tables, FunctionID caller) -> std::span<const LinkedCall> {
        if (caller.index() + 1 >= tables.firstCallOfCaller.size())
            return { };
        uint32_t first = tables.firstCallOfCaller[caller.index()];
        return tables.calls.span().subspan(first, tables.firstCallOfCaller[caller.index() + 1] - first);
    };

    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    m_firstCalleeOf.reserveInitialCapacity(numberOfFunctions + 1);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        size_t first = m_callees.size();
        m_firstCalleeOf.append(static_cast<uint32_t>(first));
        for (const LinkTables* tables : { &facts.linked(), &facts.held() }) {
            for (const LinkedCall& call : callsListedUnder(*tables, FunctionID(index)))
                m_callees.append(call.callee);
        }
        std::span<FunctionID> added = m_callees.mutableSpan().subspan(first);
        std::ranges::sort(added);
        m_callees.shrink(m_callees.size() - std::ranges::unique(added).size());
    }
    m_firstCalleeOf.append(static_cast<uint32_t>(m_callees.size()));

    const ModuleEvaluationPlan& evaluation = plan.moduleEvaluation();
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
    CounterOfCoveredFrames(const Program&, const Facts&, const Plan&);

    StackTestPlan run();

private:
    enum class BodyTrait : uint8_t {
        // Everything but the number of frames below the body allows it.
        MayBeCovered = 1 << 0,
        MayBeInlined = 1 << 1,
        // Code calls it from the start of a module and at the bare sites of other bodies, and nowhere else: a listed call that names it leads to a start or to the loader.
        IsBodyOfModule = 1 << 2,
        // Of an async function in which nothing suspends. Its ramp enters its body, and so does every caller that has the ramp written out at the call, past whatever test the ramp makes.
        HasBodyThatNeverSuspends = 1 << 3,
    };

    enum class Count : uint8_t { NotBegun, InProgress, Done };

    struct CountInProgress {
        FunctionID function;
        std::span<const CallAtBareSiteForStackTests> stepsAtBareSites;
        uint32_t nextCallee { 0 };
        uint32_t nextStep { 0 };
    };

    bool bodyMaySuspend(FunctionID id) const { return m_conventions.of(id).has(CodeRole::CoroutineBody) && !m_conventions.of(id).coroutineNeverSuspends; }
    OptionSet<BodyTrait> traitsOf(FunctionID, const StronglyConnectedComponents& cyclesOfCountedCalls, const BitVector& methodsOfTable) const;
    bool needsNoCount(FunctionID id) const { return m_traits[id.index()].isEmpty() || m_counts[id.index()] == Count::Done; }
    // The unit that a call of it enters tests the stack, and its body is built into no caller. It is counted as a root, for what stands below that unit.
    bool addsNothingBelowCaller(FunctionID id) const { return needsNoCount(id) || !m_traits[id.index()].containsAny({ BodyTrait::MayBeCovered, BodyTrait::MayBeInlined }); }
    std::span<const FunctionID> calleesCountedBelow(FunctionID) const;
    void beginCount(FunctionID);
    FunctionID takeNextCalleeToCount();
    void finishCount(const CountInProgress&);
    void countCoveredFrames();

    const Program& m_program;
    const Facts& m_facts;
    const CallingConventions& m_conventions;
    CallsListedForStackTests m_calls;
    StackTestPlan m_plan;

    // By function. None: the function tests the stack in a frame of its own in front of whatever it calls.
    Vector<OptionSet<BodyTrait>> m_traits;
    Vector<Count> m_counts;
    Vector<CountInProgress> m_countsInProgress;
};

CounterOfCoveredFrames::CounterOfCoveredFrames(const Program& program, const Facts& facts, const Plan& plan)
    : m_program(program)
    , m_facts(facts)
    , m_conventions(plan.conventions())
    , m_calls(program, facts, plan)
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

auto CounterOfCoveredFrames::traitsOf(FunctionID id, const StronglyConnectedComponents& cyclesOfCountedCalls, const BitVector& methodsOfTable) const -> OptionSet<BodyTrait>
{
    const Function& function = m_program.at(id);
    FunctionConvention convention = m_conventions.of(id);
    bool isCoroutine = function.isCoroutine();
    // The Body of a coroutine is its ramp, which suspends nowhere. The loader enters that of a module's body.
    if (!convention.has(CodeRole::Body) || (isCoroutine ? function.isModuleBody() : m_facts.summaries().at(id).has(SummaryTrait::Suspends)))
        return { };
    // A start tests the stack in its own frame, the calls at bare sites are counted, and the run time enters by value, right behind the check of whoever calls through the word of a closure.
    if (function.isModuleBody())
        return { BodyTrait::MayBeCovered, BodyTrait::IsBodyOfModule };

    OptionSet<BodyTrait> traits;
    // On a cycle of calls a caller may be counted in front of a callee, which then tests for itself. Nothing is counted for the body of such a member where it is built into a frame: the walk takes none of its calls for counted.
    // A member that ends up covered was counted in front of every caller, with a frame of its own above what it calls, so the walk takes its calls for counted wherever its body is built.
    bool isSmallEnough = m_facts.summaries().at(id).numberOfOperations <= mostOperationsOfBodyBuiltIntoFrameOfCaller || isLibraryCode(m_program, function);
    if (!cyclesOfCountedCalls.isInCycle.get(id.index()) && isSmallEnough && !bodyMaySuspend(id))
        traits.add(BodyTrait::MayBeInlined);
    // Every way into the body leads past a test. A call that is listed is counted by whoever makes it. Whoever enters through the word of a closure, the host among them, tests right in front of the generic entry.
    // A unit that makes a call which nobody has counted for it tests in its own frame: Code::holdsCallThatPlanHasNotCounted().
    // An emitter calls through the method table with no test in front and no note of the walk.
    if (!methodsOfTable.get(id.index()))
        traits.add(BodyTrait::MayBeCovered);
    if (convention.coroutineNeverSuspends)
        traits.add(BodyTrait::HasBodyThatNeverSuspends);
    return traits;
}

// What is listed under a coroutine is called by its body. One that may suspend tests the stack for itself, so nothing is counted below its ramp, and no cycle of calls leads on through it untested.
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

// What the function on top may call and is not counted yet.
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

// Whatever the function may call is counted, or is being counted and tests for itself.
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

    // The slow version calls what the function calls as it is written, and is entered by a jump: the one frame is that of either version.
    unsigned framesOfCall = framesBelow + 1;
    bool isCoroutine = m_conventions.of(id).has(CodeRole::CoroutineBody);
    bool coversBodyOfCoroutine = false;
    if (isCoroutine) {
        // So far the frame is that of the body. The ramp waits above it, and below a body that tests for itself nothing is counted.
        // A body never counts on the test of its ramp, which a caller may pass by: it is covered only where the ramp is, by whoever counts both.
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
    // An inlined body has neither a frame nor a stack check: what it calls stands right below the frame that it was built into.
    // The frames of a call leave that out where the body of a coroutine tests for itself.
    if (traits.contains(BodyTrait::MayBeInlined))
        counted = std::max(counted, static_cast<uint8_t>(framesBelow));
    bool frameOfBodyIsCounted = isCoroutine ? coversBodyOfCoroutine : isCovered;
    // The slow version of a coroutine is started by the ramp and resumed like the body. Below a ramp whose body may suspend nothing is counted, so there it tests as that body does.
    if (frameOfBodyIsCounted)
        m_plan.slowBodiesCoveredByCallers.quickSet(id.index());
    if (!traits.contains(BodyTrait::IsBodyOfModule) && (traits.contains(BodyTrait::MayBeInlined) || frameOfBodyIsCounted))
        m_plan.bodiesWhoseCallsAreCountedBelowCallers.quickSet(id.index());
    m_counts[id.index()] = Count::Done;
    m_plan.coveredFramesOfCall[id.index()] = counted;
}

// Each edge is gone over once, in an order that the program alone decides: it says which member of a cycle tests.
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
                // It closes a cycle of calls. Every cycle has such a member, so every turn of it tests the stack, and the other members may count on that.
                m_traits[callee.index()].remove(BodyTrait::MayBeCovered);
            }
        }
    }
}

StackTestPlan CounterOfCoveredFrames::run()
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(m_traits.size());
    StronglyConnectedComponents cyclesOfCountedCalls = computeStronglyConnectedComponents(numberOfFunctions, [&](unsigned node) {
        return calleesCountedBelow(FunctionID(node));
    });
    BitVector methodsOfTable(numberOfFunctions);
    for (const MethodFamily& family : m_conventions.methodFamilies()) {
        for (FunctionID method : family.methods)
            methodsOfTable.set(method.index());
    }
    for (uint32_t index = 0; index < numberOfFunctions; ++index)
        m_traits[index] = traitsOf(FunctionID(index), cyclesOfCountedCalls, methodsOfTable);
    countCoveredFrames();
    return WTF::move(m_plan);
}

} // anonymous namespace

StackTestPlan planStackTests(const Program& program, const Facts& facts, const Plan& plan)
{
    return CounterOfCoveredFrames(program, facts, plan).run();
}

// ---- The check

// Behind a stack check there is room for maxNestedFunctionsWithoutStackCheck frames of covered bodies. An inlined body brings no check along, so what it calls stands right below the frame that it was inlined into.
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
        // A caller that has the ramp written out at the call enters a body that never suspends past whatever test the ramp makes. So the body counts on whoever the ramp counts on.
        expect(!bodyOfCoroutineIsCovered(id) || (convention.has(CodeRole::CoroutineBody) && convention.coroutineNeverSuspends && isCovered(id)), id, 0, "the body of a coroutine makes no stack test, and it may suspend or its ramp tests"_s);
        expect(!isCovered(id) || (convention.has(CodeRole::Body) && !(function.isCoroutine() && function.isModuleBody())), id, 0, "a body makes no stack test, and somebody enters it whom nobody counts"_s);
        expect(!tests.slowBodiesCoveredByCallers.get(index) || (isCovered(id) && (!convention.has(CodeRole::CoroutineBody) || bodyOfCoroutineIsCovered(id))), id, 0, "a slow version makes no stack test, and the body whose place it takes tests"_s);
    }
    for (const MethodFamily& family : conventions.methodFamilies()) {
        for (FunctionID method : family.methods)
            expect(!isCovered(method), method, 0, "a method of the method table makes no stack test"_s);
    }

    // The heights speak of the calls that are listed. A unit into which the walk builds any other call of a covered body tests the stack for itself.
    enum class Height : uint8_t { NotBegun, InProgress, Known };
    struct CallerOnPath {
        FunctionID function;
        std::span<const CallAtBareSiteForStackTests> stepsAtBareSites;
        uint32_t nextCallee { 0 };
        uint32_t nextStep { 0 };
    };

    CallsListedForStackTests calls(program, facts, plan);
    Vector<Height> heights(FillWith { }, numberOfFunctions, Height::NotBegun);
    Vector<uint32_t> framesOfCall(FillWith { }, numberOfFunctions, 0u);
    Vector<CallerOnPath> path;

    // What is listed under a coroutine is called by its body. One that tests the stack for itself leaves nothing below the ramp.
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

        // A ramp waits above the body of its coroutine.
        uint32_t height = (bodyOfCoroutineTestsForItself(function) ? 0 : framesBelow) + isCovered(function) + bodyOfCoroutineIsCovered(function);
        heights[function.index()] = Height::Known;
        expect(!isCovered(function) || height <= maxNestedFunctionsWithoutStackCheck, function, height, "more frames without a stack test stand below a test than there is room for"_s);
        // Built into a caller, such a body brings no test along.
        height = std::max(height, framesBelow);
        expect(height <= tests.coveredFramesOfCall[function.index()], function, height, "a call puts more frames without a stack test below its caller than the plan has counted"_s);
        framesOfCall[function.index()] = height;
    };

    // The path holds nothing that is sure to test the stack.
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
