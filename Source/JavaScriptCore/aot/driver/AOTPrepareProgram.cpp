/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStages.h"

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTBodyVerifier.h"
#include "AOTBuildContext.h"
#include "AOTCurrentWork.h"
#include "AOTFactFaults.h"
#include "AOTFacts.h"
#include "AOTHeaps.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTProgramVerifier.h"
#include "AOTRegExpSummary.h"
#include "AOTStackTestPlan.h"
#include "AOTStubCatalog.h"
#include "AOTTailCallPlan.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static Typed::FactsRequest factsRequestForPreparedProgram(const CompileOptions& options, Typed::Mechanisms mechanisms)
{
    Typed::FactsRequest request;
    request.mechanisms = Typed::withoutMechanismsMissingDependencies(mechanisms);
    if (hasFlag(options.testing, Wire::TestingSwitch::DelayFlowRules))
        request.orderOfWorklist = Typed::OrderOfWorklist::Scattered;
    else if (hasFlag(options.testing, Wire::TestingSwitch::DelayOtherFlowRules))
        request.orderOfWorklist = Typed::OrderOfWorklist::OldestFirst;
    return request;
}

static bool verifyProgramToPrepare(const Typed::Program& program, BuildContext& build, std::unique_ptr<Typed::BodyIndexTable>& bodyIndexes)
{
    PhaseTimes times(build, Stage::ReadAndVerify);
    CurrentWorkScope work("verify"_s);

    bool recordsAreValid = Typed::verifyRecords(program, build);
    times.didFinish("verify records"_s);
    if (!recordsAreValid)
        return false;

    bodyIndexes = makeUniqueWithoutFastMallocCheck<Typed::BodyIndexTable>(static_cast<uint32_t>(program.functions.size()), build.parallelLoops().width());
    bool bodiesAreValid = Typed::verifyBodies(program, build, bodyIndexes.get());
    times.didFinish("verify bodies"_s);
    return bodiesAreValid;
}

static bool computeFactsOfPreparedProgram(PreparedProgram& prepared, BuildContext& build, std::unique_ptr<Typed::BodyIndexTable>&& bodyIndexes)
{
    PhaseTimes times(build, Stage::AnalyzeFlow);
    CurrentWorkScope work("facts"_s);

    prepared.facts = Typed::computeFacts(*prepared.program, build, factsRequestForPreparedProgram(build.options(), prepared.mechanisms), WTF::move(bodyIndexes));
    times.didFinish("compute facts"_s);
    return !build.diagnostics().hasErrors();
}

static Typed::BitVector findDictionariesOfPreparedProgram(const Typed::Program& program, const Typed::Facts& facts)
{
    unsigned numberOfLayouts = program.layouts.size();
    Typed::BitVector layoutsOfDictionaries(numberOfLayouts);
    for (unsigned index = 0; index < numberOfLayouts; ++index)
        layoutsOfDictionaries.quickSet(index, facts.has(Typed::LayoutID(index), Typed::LayoutFact::ObjectsAreDictionaries));
    return layoutsOfDictionaries;
}

static void leaveOutCallsOfBuiltinsThatProgramCannotHold(StubOptions& options, const Typed::Program& program, const Typed::Facts& facts)
{
    if (facts.unseenCodeCanRun())
        return;
    const auto& builtins = program.builtins();
    BitVector held(builtins.builtinCount());
    for (const auto& use : facts.builtinUses())
        held.set(builtins.canonicalID(BuiltinID(use.builtin)).index());
    auto isHeld = [&](BuiltinID builtin) {
        return held.get(builtins.canonicalID(builtin).index());
    };
    options.callsNumberOrStringByValue = isHeld(Builtins::Number) || isHeld(Builtins::String);
    options.callsArrayByValue = isHeld(Builtins::Array);
    options.boxedCallsOutOfReach.setAll();
    for (const auto& call : builtins.boxedCalls()) {
        if (isHeld(call.builtin))
            options.boxedCallsOutOfReach.clear(static_cast<size_t>(call.stub));
    }
}

static bool planPreparedProgram(PreparedProgram& prepared, BuildContext& build)
{
    PhaseTimes times(build, Stage::PlanSlots);
    CurrentWorkScope work("plan"_s);

    Typed::Program& program = *prepared.program;
    const Typed::Facts& facts = *prepared.facts;

    Typed::BitVector layoutsOfDictionaries = findDictionariesOfPreparedProgram(program, facts);
    Typed::noteLayoutsOfDictionaries(program, layoutsOfDictionaries);

    program.types.keepAddressesOfTypes();
    prepared.plan = Typed::planProgram(program, facts, build);
    if (prepared.plan && build.options().misstatedFact) [[unlikely]]
        Typed::misstatePlan(program, facts, *prepared.plan, build);
    program.types.freeze();
    times.didFinish("plan"_s);
    if (!prepared.plan || build.diagnostics().hasErrors())
        return false;
    if (prepared.changePlan)
        prepared.changePlan(*prepared.plan);

    for (size_t index : layoutsOfDictionaries) {
        Typed::LayoutID layout(index);
        RELEASE_ASSERT(prepared.plan->layout(layout).storage != Typed::LayoutStorage::OrdinaryObject || Typed::builtinClassWrittenAbove(program, layout));
    }

    if (build.validates()) {
        Typed::checkFormsRoundRingsOfTailCalls(program, facts, *prepared.plan, build);
        times.didFinish("check the rings of tail calls"_s);
    }
    if (prepared.checkOfPlan == CheckOfPlan::InFrontOfCompile) {
        Typed::checkPlan(program, facts, *prepared.plan, build);
        Typed::checkStackTests(program, facts, *prepared.plan, build);
        times.didFinish("check the plan"_s);
    }
    if (build.diagnostics().hasErrors())
        return false;

    build.regExpSummaries().setRecordsMatches(facts.mayReadLegacyRegExpStatics());
    runLanes(build.parallelLoops(), {
        [&] {
            StubOptions options = StubOptions::from(build.recordedOptions(), program, facts.hasOneInstance(), facts.matchesRegExps(), facts.mayReadLegacyRegExpStatics(), AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime), AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckWalkAtRunTime));
            leaveOutCallsOfBuiltinsThatProgramCannotHold(options, program, facts);
            prepared.stubCatalog = makeUniqueWithoutFastMallocCheck<const StubCatalog>(options);
        },
        [&] {
            if (!willVerifyCode(prepared, build))
                prepared.facts->trimSummariesForWalk(build.parallelLoops());
        },
    });
    times.didFinish("make the stub catalog"_s);
    return true;
}

static void makeCompileInputsOfPreparedProgram(PreparedProgram& prepared, BuildContext& build)
{
    PhaseTimes times(build, Stage::PlanSlots);
    CurrentWorkScope work("compile: inputs"_s);

    prepared.heaps = makeUniqueWithoutFastMallocCheck<const AOTHeaps>();
    prepared.arrivals = makeUniqueWithoutFastMallocCheck<Typed::Arrivals>(*prepared.program, *prepared.facts, prepared.program->types);
    prepared.compileInputs = createCompileInputs(prepared, *prepared.facts, *prepared.arrivals, build);
    times.didFinish("make the inputs of Compile"_s);
}

bool prepareProgram(PreparedProgram& prepared, BuildContext& build)
{
    RELEASE_ASSERT(prepared.program && !prepared.plan);
    RELEASE_ASSERT(!prepared.program->types.isFrozen());

    if (!prepared.facts) {
        std::unique_ptr<Typed::BodyIndexTable> bodyIndexes;
        if (!verifyProgramToPrepare(*prepared.program, build, bodyIndexes))
            return false;
        if (!computeFactsOfPreparedProgram(prepared, build, WTF::move(bodyIndexes)))
            return false;
    }
    RELEASE_ASSERT(prepared.facts && &prepared.facts->program() == prepared.program.get());

    if (!planPreparedProgram(prepared, build))
        return false;

    makeCompileInputsOfPreparedProgram(prepared, build);
    return true;
}

void checkPlanOfPreparedProgram(const PreparedProgram& prepared, BuildContext& build)
{
    RELEASE_ASSERT(prepared.checkOfPlan == CheckOfPlan::LeftToCaller && prepared.compileInputs);
    RELEASE_ASSERT(prepared.program->types.isFrozen());

    CurrentWorkScope work("check the plan"_s);
    StepTimes times(build, nameOf(Stage::PlanSlots));
    Typed::checkPlan(*prepared.program, *prepared.facts, *prepared.plan, build);
    Typed::checkStackTests(*prepared.program, *prepared.facts, *prepared.plan, build);
    times.didFinish("check the plan: in its lane"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
