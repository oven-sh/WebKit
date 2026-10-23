/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTIRTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr FunctionID unreachedFunction { 0u };
static constexpr FunctionID functionRunOnce { 1u };
static constexpr FunctionID derivedFunction { 2u };
static constexpr FunctionID functionPastTheProgram { 9u };

static void checkFunctionIsUnknown(const FlowResults& flow, FunctionID id)
{
    const FunctionFlow& function = flow.function(id);
    AOT_TEST_CHECK(function == FunctionFlow { } && function.witness == noWitness);
    AOT_TEST_CHECK(function.reachability == FunctionReachability::Unknown && function.callers == CallerState::Unknown);
    AOT_TEST_CHECK(function.executions == ExecutionCount::Unknown && function.closureUse == ClosureUse::Unknown);
    AOT_TEST_CHECK(flow.mayBeReached(id) && !flow.runsAtMostOnce(id));
    AOT_TEST_CHECK(flow.closureUseOfBoundFunction(id, 0) == ClosureUse::Unknown && flow.closureUseOfBoundFunction(id, 1) == ClosureUse::Unknown);
    for (uint32_t parameter : { 0u, 1u, 100u })
        AOT_TEST_CHECK(flow.parameterSummary(id, parameter) == OperandSummary());
}

static void checkSitesAreUnknown(const FlowResults& flow, uint32_t index)
{
    AOT_TEST_CHECK(flow.at(AccessSiteID(index)) == AccessSite { } && flow.at(AccessSiteID(index)).location == noSourceLocation && !flow.at(AccessSiteID(index)).function);
    AOT_TEST_CHECK(flow.at(AllocationSiteID(index)) == AllocationSite { } && flow.at(AllocationSiteID(index)).location == noSourceLocation && !flow.at(AllocationSiteID(index)).allocationClass);
    for (const AllocationClass* allocationClass : { &flow.at(AllocationClassID(index)), &flow.classOf(AllocationSiteID(index)) }) {
        AOT_TEST_CHECK(*allocationClass == AllocationClass { } && allocationClass->witness == noWitness);
        AOT_TEST_CHECK(allocationClass->impossibleMutations.isEmpty() && allocationClass->keys == KeyEnumeration::Unknown && allocationClass->locality == FrameLocality::Unknown && !allocationClass->addedKeysAreComplete);
    }
    AOT_TEST_CHECK(flow.lossOf(AccessSiteID(index)) == FlowSiteLoss { });
    AOT_TEST_CHECK(flow.lossOf(AccessSiteID(index)).cause == UnprovenCause::Unknown && flow.lossOf(AccessSiteID(index)).limit == LimitID::None && flow.lossOf(AccessSiteID(index)).witness == noWitness);
    AOT_TEST_CHECK(!flow.seesOnlyFinishedObjects(AccessSiteID(index)) && !flow.mayCallBoundFunction(AccessSiteID(index)));

    const SinkCandidates& candidates = flow.sinkCandidates(AccessSiteID(index), 0);
    AOT_TEST_CHECK(candidates.strings.isEmpty() && candidates.mayBeUndefined && !candidates.isComplete);
    AOT_TEST_CHECK(!flow.typedResultType(AccessSiteID(index)) && flow.typedResultLayouts(AccessSiteID(index)).empty() && !flow.typedResultSite(LayoutID(index)));
    AOT_TEST_CHECK(flow.dataNodeStamp(AllocationSiteID(index), 0) == FlowDataNodeStamp { });
}

static void checkNullIDsAreUnknown(const FlowResults& flow)
{
    checkFunctionIsUnknown(flow, FunctionID());
    checkSitesAreUnknown(flow, AccessSiteID().index());
    AOT_TEST_CHECK(flow.storedValueUses({ LayoutID(), 0 }) == OperandUses::all());
    AOT_TEST_CHECK(flow.module(ModuleID()) == ModuleFlow { } && flow.module(ModuleID()).namespaceUse == NamespaceUse::Unknown);
    AOT_TEST_CHECK(flow.builtinLocation(BuiltinID()) == BuiltinLocationState::Unknown && flow.builtinHolder(BuiltinID()) == BuiltinHolderState::Unknown);
    AOT_TEST_CHECK(!flow.findBuiltinLocation(BuiltinID(), PropertyKeyID()));
}

static void checkHasNoResults(const FlowResults& flow)
{
    AOT_TEST_CHECK(!flow.hasResults() && !flow.functionCountAsRead() && !flow.functionCount());
    checkNullIDsAreUnknown(flow);
    for (FunctionID id : { unreachedFunction, functionRunOnce, derivedFunction, functionPastTheProgram }) {
        checkFunctionIsUnknown(flow, id);
        AOT_TEST_CHECK(flow.indirectCallsFrom(id).empty());
    }
    checkSitesAreUnknown(flow, 0);
    checkSitesAreUnknown(flow, 5);

    AOT_TEST_CHECK(flow.storedValueUses({ LayoutID(0u), 0 }) == OperandUses::all());
    AOT_TEST_CHECK(flow.module(ModuleID(0u)) == ModuleFlow { });
    AOT_TEST_CHECK(flow.builtinLocation(Builtins::MathMax) == BuiltinLocationState::Unknown && flow.builtinLocation(Builtins::ArrayPrototypePush) == BuiltinLocationState::Unknown);
    AOT_TEST_CHECK(flow.builtinHolder(Builtins::Math) == BuiltinHolderState::Unknown);
    AOT_TEST_CHECK(!flow.findBuiltinLocation(Builtins::Math, PropertyKeyID(0u)));
    AOT_TEST_CHECK(flow.usesOf(Builtins::MathMax) == OptionSet<BuiltinUse>::all() && flow.usesOf(BuiltinID()) == OptionSet<BuiltinUse>::all());

    AOT_TEST_CHECK(flow.indirectCalls().empty() && flow.builtinLocations().empty() && flow.accessSites().empty() && flow.allocationSites().empty() && flow.allocationClasses().empty());
    AOT_TEST_CHECK(!flow.numberOfAccessSites() && !flow.numberOfAllocationSites() && !flow.numberOfAllocationClasses());
    AOT_TEST_CHECK(flow.unseenCode().empty() && flow.builtinUses().empty() && flow.featureUses().empty() && flow.localeUses().empty());
    AOT_TEST_CHECK(flow.mayRunUnseenCode() && flow.unseenCodeCanRun() && flow.mayReadLegacyRegExpStatics() && flow.programInstances() == ProgramInstances::Unknown);
}

static constexpr OperandSummary inspectedOnly { OperandUse::Inspects, RetainedIn::Result, 0 };
static constexpr OperandSummary retainedInFirstOperand { { OperandUse::Inspects, OperandUse::Retains }, RetainedIn::Operand, 0 };

static void addFunctions(TestProgram& test)
{
    test.addFunction(createFunction({ }));
    test.addFunction(createFunction({ }));
    Function derived = createFunction({ });
    derived.derivation.source = functionRunOnce;
    derived.derivation.parameterMap = { ParameterOrigin { 1 }, ParameterOrigin { }, ParameterOrigin { 0 } };
    test.addFunction(WTF::move(derived));
}

static FlowResultTables createTables(const TestProgram& test)
{
    FlowResultTables tables;
    tables.program = &test.program;

    tables.functions.functionCountAsRead = 2;
    tables.functions.functions.grow(2);
    tables.functions.functions[unreachedFunction.index()].reachability = FunctionReachability::Unreached;
    tables.functions.functions[functionRunOnce.index()].executions = ExecutionCount::AtMostOnce;
    tables.functions.functions[functionRunOnce.index()].callers = CallerState::AllKnown;
    tables.functions.parameterSummaries = { { }, { inspectedOnly, retainedInFirstOperand } };
    tables.functions.storedValueUses = { { OperandUses { OperandUse::Inspects }, OperandUses { } }, { } };
    tables.functions.indirectCalls = {
        { unreachedFunction, AccessSiteID(0u), functionRunOnce, FlowIndirectCallKind::Accessor, BuiltinCallbackTimes::Once, false },
        { functionRunOnce, AccessSiteID(0u), unreachedFunction, FlowIndirectCallKind::BuiltinCallback, BuiltinCallbackTimes::PerElement, false },
        { functionRunOnce, AccessSiteID(0u), functionRunOnce, FlowIndirectCallKind::Protocol, BuiltinCallbackTimes::Repeatedly, true },
    };
    tables.functions.boundFunctionUses = { { functionRunOnce, 1, ClosureUse::CalledOnly } };
    tables.functions.functionsSeeingOnlyFinishedObjects.set(functionRunOnce.index());

    tables.sites.accessSites.grow(1);
    tables.sites.accessSites[0].function = functionRunOnce;
    tables.sites.accessSiteLosses = { FlowSiteLoss { UnprovenCause::LimitReached, LimitID::MaxLayoutsInType, noWitness } };
    tables.sites.callsOfBoundFunctions = { true };
    tables.sites.allocationSites.grow(1);
    tables.sites.allocationSites[0].allocationClass = AllocationClassID(0u);
    tables.sites.allocationClasses.grow(1);
    tables.sites.allocationClasses[0].impossibleMutations = ObjectMutation::DeleteField;
    tables.sites.sinkOperands = { FlowSinkOperand { AccessSiteID(0u), 1, SinkCandidates { { StringID(0u) }, false, true } } };

    tables.builtins.constantMembers.set(Builtins::MathMax.index());
    tables.builtins.intactHolders.set(Builtins::Math.index());
    tables.builtins.locations = { BuiltinLocationFlow { Builtins::Math, PropertyKeyID(3u), BuiltinLocationState::Constant, noWitness } };
    tables.builtins.uses = { BuiltinUseRecord { Builtins::MathMax.index(), BuiltinUse::Called, noWitness } };
    tables.builtins.legacyRegExpStatics = LegacyRegExpStatics::NeverRead;

    tables.wholeProgram.modules.grow(1);
    tables.wholeProgram.modules[0].namespaceUse = NamespaceUse::NeverCreated;
    tables.wholeProgram.instances = ProgramInstances::AtMostOne;
    return tables;
}

static void checkFunctionAnswers(const FlowResults& flow, const Program& program)
{
    AOT_TEST_CHECK(flow.hasResults() && flow.functionCountAsRead() == 2 && flow.functionCount() == 2);
    AOT_TEST_CHECK(!flow.mayBeReached(unreachedFunction) && !flow.runsAtMostOnce(unreachedFunction));
    AOT_TEST_CHECK(flow.mayBeReached(functionRunOnce) && flow.runsAtMostOnce(functionRunOnce) && flow.runsAtMostOnce(program, functionRunOnce));
    AOT_TEST_CHECK(flow.function(functionRunOnce).callers == CallerState::AllKnown);
    AOT_TEST_CHECK(&flow.function(derivedFunction) == &flow.function(functionRunOnce));
    checkFunctionIsUnknown(flow, functionPastTheProgram);

    AOT_TEST_CHECK(flow.closureUseOfBoundFunction(functionRunOnce, 1) == ClosureUse::CalledOnly && flow.closureUseOfBoundFunction(derivedFunction, 1) == ClosureUse::CalledOnly);
    AOT_TEST_CHECK(flow.closureUseOfBoundFunction(functionRunOnce, 0) == ClosureUse::Unknown && flow.closureUseOfBoundFunction(unreachedFunction, 1) == ClosureUse::Unknown);

    AOT_TEST_CHECK(flow.parameterSummary(functionRunOnce, 0) == inspectedOnly && flow.parameterSummary(functionRunOnce, 1) == retainedInFirstOperand);
    AOT_TEST_CHECK(flow.parameterSummary(functionRunOnce, 2) == OperandSummary() && flow.parameterSummary(unreachedFunction, 0) == OperandSummary());
    AOT_TEST_CHECK(flow.parameterSummary(derivedFunction, 2) == inspectedOnly);
    AOT_TEST_CHECK(flow.parameterSummary(derivedFunction, 0) == (OperandSummary { retainedInFirstOperand.uses, RetainedIn::Operand, 2 }));
    AOT_TEST_CHECK(flow.parameterSummary(derivedFunction, 1) == OperandSummary() && flow.parameterSummary(derivedFunction, 3) == OperandSummary());

    AOT_TEST_CHECK(flow.storedValueUses({ LayoutID(0u), 0 }) == OperandUses { OperandUse::Inspects } && flow.storedValueUses({ LayoutID(0u), 1 }).isEmpty());
    AOT_TEST_CHECK(flow.storedValueUses({ LayoutID(0u), 2 }) == OperandUses::all() && flow.storedValueUses({ LayoutID(1u), 0 }) == OperandUses::all() && flow.storedValueUses({ LayoutID(2u), 0 }) == OperandUses::all());

    AOT_TEST_CHECK(flow.indirectCalls().size() == 3 && flow.indirectCallsFrom(unreachedFunction).size() == 1 && flow.indirectCallsFrom(functionRunOnce).size() == 2);
    AOT_TEST_CHECK(flow.indirectCallsFrom(functionRunOnce)[1].kind == FlowIndirectCallKind::Protocol && flow.indirectCallsFrom(functionRunOnce)[1].isInCycle);
    AOT_TEST_CHECK(flow.indirectCallsFrom(derivedFunction).empty() && flow.indirectCallsFrom(FunctionID()).empty());
}

static void checkOtherAnswers(const FlowResults& flow)
{
    AOT_TEST_CHECK(flow.numberOfAccessSites() == 1 && flow.numberOfAllocationSites() == 1 && flow.numberOfAllocationClasses() == 1);
    AOT_TEST_CHECK(flow.at(AccessSiteID(0u)).function == functionRunOnce && flow.lossOf(AccessSiteID(0u)).limit == LimitID::MaxLayoutsInType);
    AOT_TEST_CHECK(flow.seesOnlyFinishedObjects(AccessSiteID(0u)) && flow.mayCallBoundFunction(AccessSiteID(0u)));
    AOT_TEST_CHECK(&flow.classOf(AllocationSiteID(0u)) == &flow.at(AllocationClassID(0u)) && flow.at(AllocationClassID(0u)).impossibleMutations.contains(ObjectMutation::DeleteField));
    AOT_TEST_CHECK(flow.sinkCandidates(AccessSiteID(0u), 1).isComplete && flow.sinkCandidates(AccessSiteID(0u), 1).strings.size() == 1);
    AOT_TEST_CHECK(!flow.sinkCandidates(AccessSiteID(0u), 0).isComplete && !flow.sinkCandidates(AccessSiteID(0u), 2).isComplete);
    checkSitesAreUnknown(flow, 1);

    AOT_TEST_CHECK(flow.builtinLocation(Builtins::MathMax) == BuiltinLocationState::Constant && flow.builtinLocation(Builtins::MathAbs) == BuiltinLocationState::Unknown);
    AOT_TEST_CHECK(flow.builtinHolder(Builtins::Math) == BuiltinHolderState::Intact && flow.builtinHolder(Builtins::JSON) == BuiltinHolderState::Unknown);
    AOT_TEST_CHECK(flow.findBuiltinLocation(Builtins::Math, PropertyKeyID(3u)) == &flow.builtinLocations()[0]);
    AOT_TEST_CHECK(!flow.findBuiltinLocation(Builtins::Math, PropertyKeyID(4u)) && !flow.findBuiltinLocation(Builtins::JSON, PropertyKeyID(3u)));
    AOT_TEST_CHECK(flow.usesOf(Builtins::MathMax) == OptionSet<BuiltinUse> { BuiltinUse::Called });
    AOT_TEST_CHECK(flow.usesOf(Builtins::MathAbs).isEmpty() && flow.usesOf(BuiltinID()).isEmpty());

    AOT_TEST_CHECK(flow.module(ModuleID(0u)).namespaceUse == NamespaceUse::NeverCreated && flow.module(ModuleID(1u)) == ModuleFlow { });
    AOT_TEST_CHECK(!flow.mayRunUnseenCode() && !flow.unseenCodeCanRun() && !flow.mayReadLegacyRegExpStatics() && flow.programInstances() == ProgramInstances::AtMostOne);
    checkNullIDsAreUnknown(flow);
}

static void flowResultsWithoutResultsAnswerUnknown()
{
    FlowResults flow;
    checkHasNoResults(flow);

    TestProgram test;
    addFunctions(test);
    AOT_TEST_CHECK(!flow.runsAtMostOnce(test.program, functionRunOnce));
}

static void flowResultsAnswerFromTheirTables()
{
    TestProgram test;
    addFunctions(test);
    FlowResults flow(createTables(test));
    checkFunctionAnswers(flow, test.program);
    checkOtherAnswers(flow);
}

static void flowResultsKeepAnswersWhenMoved()
{
    FlowResults empty;
    FlowResults movedEmpty(WTF::move(empty));
    checkHasNoResults(movedEmpty);

    TestProgram test;
    addFunctions(test);
    FlowResults flow(createTables(test));
    FlowResults moved(WTF::move(flow));
    checkFunctionAnswers(moved, test.program);
    checkOtherAnswers(moved);

    FlowResults assigned;
    assigned = WTF::move(moved);
    checkFunctionAnswers(assigned, test.program);
    checkOtherAnswers(assigned);

    assigned = FlowResults();
    checkHasNoResults(assigned);
}

static void flowResultsFollowRemovedEntryParameters()
{
    TestProgram test;
    addFunctions(test);
    FlowResultTables tables = createTables(test);
    OperandSummary retainedInLastOperand { retainedInFirstOperand.uses, RetainedIn::Operand, 3 };
    tables.functions.parameterSummaries[functionRunOnce.index()] = { inspectedOnly, retainedInFirstOperand, retainedInLastOperand, inspectedOnly };
    FlowResults flow(WTF::move(tables));

    BitVector firstParameter;
    firstParameter.set(0);
    flow.removeEntryParameters(functionRunOnce, firstParameter);
    AOT_TEST_CHECK(flow.parameterSummary(functionRunOnce, 0) == (OperandSummary { retainedInFirstOperand.uses, RetainedIn::UnknownMemory, 0 }));
    AOT_TEST_CHECK(flow.parameterSummary(functionRunOnce, 1) == (OperandSummary { retainedInFirstOperand.uses, RetainedIn::Operand, 2 }));
    AOT_TEST_CHECK(flow.parameterSummary(functionRunOnce, 2) == inspectedOnly && flow.parameterSummary(functionRunOnce, 3) == OperandSummary());

    flow.removeEntryParameters(functionRunOnce, BitVector());
    flow.removeEntryParameters(unreachedFunction, firstParameter);
    flow.removeEntryParameters(functionPastTheProgram, firstParameter);
    AOT_TEST_CHECK(flow.parameterSummary(functionRunOnce, 2) == inspectedOnly && flow.parameterSummary(unreachedFunction, 0) == OperandSummary());

    FlowResults withoutResults;
    withoutResults.removeEntryParameters(functionRunOnce, firstParameter);
    checkHasNoResults(withoutResults);
}

static AllocationClass createClosureClass(FunctionID function, OptionSet<ObjectMutation> impossibleMutations, Vector<PropertyKeyID>&& addedKeys, bool addedKeysAreComplete)
{
    AllocationClass closures;
    closures.closureFunction = function;
    closures.impossibleMutations = impossibleMutations;
    closures.addedKeys = WTF::move(addedKeys);
    closures.addedKeysAreComplete = addedKeysAreComplete;
    return closures;
}

static bool areClosuresAsCreated(const TestProgram& test, Vector<AllocationClass>&& classes, FunctionID function)
{
    static constexpr PropertyKeyID readKeys[] { PropertyKeyID(1u), PropertyKeyID(2u) };
    FlowResultTables tables = createTables(test);
    tables.sites.allocationClasses.appendVector(WTF::move(classes));
    return FlowResults(WTF::move(tables)).areClosuresAsCreated(function, readKeys);
}

static void flowResultsFindChangedClosures()
{
    constexpr OptionSet<ObjectMutation> unchanged { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
    constexpr PropertyKeyID readKey { 2u };
    constexpr PropertyKeyID otherKey { 3u };

    TestProgram test;
    addFunctions(test);
    AOT_TEST_CHECK(!FlowResults().areClosuresAsCreated(functionRunOnce, { }));
    AOT_TEST_CHECK(!areClosuresAsCreated(test, { }, functionRunOnce));
    AOT_TEST_CHECK(areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { }, true) }, functionRunOnce));
    AOT_TEST_CHECK(areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { otherKey }, true), createClosureClass(unreachedFunction, { }, { }, false), createClosureClass(functionRunOnce, unchanged, { }, true) }, functionRunOnce));
    AOT_TEST_CHECK(!areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { }, true) }, unreachedFunction));
    AOT_TEST_CHECK(!areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { }, true) }, derivedFunction));
    AOT_TEST_CHECK(!areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { otherKey, readKey }, true) }, functionRunOnce));
    AOT_TEST_CHECK(!areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { }, false) }, functionRunOnce));
    for (ObjectMutation mutation : unchanged) {
        OptionSet<ObjectMutation> impossibleMutations = unchanged - mutation;
        AOT_TEST_CHECK(!areClosuresAsCreated(test, { createClosureClass(functionRunOnce, unchanged, { }, true), createClosureClass(functionRunOnce, impossibleMutations, { }, true) }, functionRunOnce));
    }
}

#define FOR_EACH_AOT_FLOW_RESULTS_TEST(v) \
    v(flowResultsWithoutResultsAnswerUnknown) v(flowResultsAnswerFromTheirTables) v(flowResultsKeepAnswersWhenMoved) \
    v(flowResultsFollowRemovedEntryParameters) v(flowResultsFindChangedClosures)

std::span<const NativeTest> flowResultsTests()
{
#define AOT_FLOW_RESULTS_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_FLOW_RESULTS_TEST(AOT_FLOW_RESULTS_TEST_ENTRY) };
#undef AOT_FLOW_RESULTS_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
