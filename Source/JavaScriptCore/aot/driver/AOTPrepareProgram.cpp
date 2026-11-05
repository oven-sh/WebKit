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
#include "AOTFacts.h"
#include "AOTHeaps.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTProgramVerifier.h"
#include "AOTStackTestPlan.h"
#include "AOTStubCatalog.h"
#include "AOTTypeRelations.h"
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static Typed::FactsRequest factsRequestForPreparedProgram(const CompileOptions& options, Typed::Mechanisms mechanisms)
{
    Typed::FactsRequest request;
    request.mechanisms = Typed::withoutMechanismsMissingDependencies(mechanisms);
    // Link answers the same in every order of its worklist and on any number of lanes. One thread tells a small program, so what only lanes get wrong shows in large ones alone, unless a test asks for lanes.
    if (hasFlag(options.testing, Wire::TestingSwitch::DelayFlowRules))
        request.orderOfWorklist = Typed::OrderOfWorklist::Scattered;
    else if (hasFlag(options.testing, Wire::TestingSwitch::DelayOtherFlowRules))
        request.orderOfWorklist = Typed::OrderOfWorklist::OldestFirst;
    return request;
}

// The verifier of bodies goes over an index of each body, and the scan of Facts over the same index: it is built once, and Facts keeps it.
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

static BitVector findDictionariesOfPreparedProgram(const Typed::Program& program, const Typed::Facts& facts)
{
    unsigned numberOfLayouts = program.layouts.size();
    BitVector layoutsOfDictionaries(numberOfLayouts);
    for (unsigned index = 0; index < numberOfLayouts; ++index)
        layoutsOfDictionaries.quickSet(index, facts.has(Typed::LayoutID(index), Typed::LayoutFact::ObjectsAreDictionaries));
    return layoutsOfDictionaries;
}

static bool planPreparedProgram(PreparedProgram& prepared, BuildContext& build)
{
    PhaseTimes times(build, Stage::PlanSlots);
    CurrentWorkScope work("plan"_s);

    Typed::Program& program = *prepared.program;
    const Typed::Facts& facts = *prepared.facts;

    // A TypeArena sees the program and not Facts. None is alive here: what one remembers of these layouts would be stale.
    BitVector layoutsOfDictionaries = findDictionariesOfPreparedProgram(program, facts);
    Typed::noteLayoutsOfDictionaries(program, layoutsOfDictionaries);

    prepared.plan = Typed::planProgram(program, facts, build);
    program.types.freeze();
    times.didFinish("plan"_s);
    if (!prepared.plan || build.diagnostics().hasErrors())
        return false;
    if (prepared.changePlan)
        prepared.changePlan(*prepared.plan);

    // An ordinary object has no shape word. A type that still named its layout would be tested by that word, and a read of a field without storage folds to undefined.
    for (size_t index : layoutsOfDictionaries) {
        Typed::LayoutID layout(index);
        RELEASE_ASSERT(prepared.plan->layout(layout).storage != Typed::LayoutStorage::OrdinaryObject || Typed::builtinClassWrittenAbove(program, layout));
    }

    if (prepared.checkOfPlan == CheckOfPlan::InFrontOfCompile) {
        Typed::checkPlan(program, facts, *prepared.plan, build);
        Typed::checkStackTests(program, facts, *prepared.plan, build);
        times.didFinish("check the plan"_s);
        if (build.diagnostics().hasErrors())
            return false;
    }

    prepared.stubCatalog = makeUniqueWithoutFastMallocCheck<const StubCatalog>(StubOptions::from(build.recordedOptions(), program, facts.hasOneInstance(), facts.matchesRegExps(), AOT::hasFlag(build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime)), stubPolicyOf(prepared.plan->slots()));
    times.didFinish("make the stub catalog"_s);
    return true;
}

static void makeCompileInputsOfPreparedProgram(PreparedProgram& prepared, BuildContext& build)
{
    PhaseTimes times(build, Stage::GenerateCode);
    CurrentWorkScope work("compile: inputs"_s);

    prepared.heaps = makeUniqueWithoutFastMallocCheck<const AOTHeaps>();
    prepared.arrivals = makeUniqueWithoutFastMallocCheck<Typed::Arrivals>(*prepared.program, *prepared.facts, prepared.program->types);
    prepared.compileInputs = createCompileInputs(prepared, *prepared.facts, *prepared.arrivals, build);
    times.didFinish("make the inputs of Compile"_s);
}

bool prepareProgram(PreparedProgram& prepared, BuildContext& build)
{
    RELEASE_ASSERT(prepared.program && !prepared.plan);
    // A type that Facts or Plan makes behind freeze() dies with the arena that made it, and the tables that name it live on.
    RELEASE_ASSERT(!prepared.program->types.isFrozen());

    if (!prepared.facts) {
        // The verifiers hold the tables to what the front end sends, and computeFacts() changes them. Its scan is also the first reader of the bodies.
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
    // After freeze() no arena adds to the table of types or leaves a memo in it, whatever it keeps: this lane and Compile only read the table.
    RELEASE_ASSERT(prepared.program->types.isFrozen());

    CurrentWorkScope work("check the plan"_s);
    // It only logs: the caller's PhaseTimes already counts this time for the stage that runs beside the lane.
    StepTimes times(build, nameOf(Stage::PlanSlots));
    Typed::checkPlan(*prepared.program, *prepared.facts, *prepared.plan, build);
    Typed::checkStackTests(*prepared.program, *prepared.facts, *prepared.plan, build);
    times.didFinish("check the plan: in its lane"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
