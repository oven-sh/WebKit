/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3Common.h"
#include "B3CompilationReport.h"
#include "B3InductionRecords.h"
#include "B3IntRange.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3Policy.h"
#include "B3RelocatableValue.h"

struct ValidationTestGraph {
    explicit ValidationTestGraph(const Policy& policy = policyWith([](Policy&) { }))
        : proc(policy)
    {
    }

    Value* appendConstant(Type type)
    {
        Value* constant = proc.addConstant(Origin(), type, 0);
        root->append(constant);
        return constant;
    }

    Value* appendPointer()
    {
        return root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR0);
    }

    Value* appendPhi(Type type)
    {
        UpsilonValue* upsilon = root->appendNew<UpsilonValue>(proc, Origin(), appendConstant(type));
        Value* phi = root->appendNew<Value>(proc, Phi, type, Origin());
        upsilon->setPhi(phi);
        return phi;
    }

    PatchpointValue* appendPatchpoint(Type type, std::initializer_list<Type> childTypes = { })
    {
        Vector<Value*, 4> children;
        for (Type childType : childTypes)
            children.append(appendConstant(childType));
        PatchpointValue* patchpoint = root->appendNew<PatchpointValue>(proc, type, Origin());
        for (Value* child : children)
            patchpoint->append(child, ValueRep::SomeRegister);
        return patchpoint;
    }

    PatchpointValue* appendPureFunction(Type type, PureFunction pureFunction)
    {
        PatchpointValue* patchpoint = appendPatchpoint(type);
        patchpoint->effects = Effects::none();
        patchpoint->pureFunction = pureFunction;
        return patchpoint;
    }

    std::optional<ValidationRule> appendReturnAndValidate()
    {
        root->appendNew<Value>(proc, Return, Origin());
        return findViolatedRule(proc);
    }

    Procedure proc;
    BasicBlock* root { proc.addBlock() };
};

template<typename Functor>
static std::optional<ValidationRule> validateLoopPoll(const Functor& changePoll)
{
    ValidationTestGraph graph;
    PatchpointValue* poll = graph.appendPatchpoint(Void);
    poll->isLoopPoll = true;
    changePoll(*poll);
    return graph.appendReturnAndValidate();
}

static void testValidateLoopPoll()
{
    CHECK(validateLoopPoll([](PatchpointValue&) { }) == std::nullopt);
    CHECK(validateLoopPoll([](PatchpointValue& poll) {
        poll.effects = Effects::none();
        poll.effects.reads = HeapRange::top();
        poll.effects.writes = pollHeap;
    }) == std::nullopt);
    {
        ValidationTestGraph graph;
        graph.appendPatchpoint(Int32, { Int32 })->effects.reads = pollHeap;
        CHECK(graph.appendReturnAndValidate() == std::nullopt);
    }

    {
        ValidationTestGraph graph;
        graph.appendPatchpoint(Int32)->isLoopPoll = true;
        CHECK(graph.appendReturnAndValidate() == ValidationRule::LoopPoll);
    }
    {
        ValidationTestGraph graph;
        graph.appendPatchpoint(Void, { Int32 })->isLoopPoll = true;
        CHECK(graph.appendReturnAndValidate() == ValidationRule::LoopPoll);
    }
    {
        ValidationTestGraph graph;
        PatchpointValue* poll = graph.appendPatchpoint(Void);
        poll->isLoopPoll = true;
        poll->effects.terminal = true;
        CHECK(findViolatedRule(graph.proc) == ValidationRule::LoopPoll);
    }
    CHECK(validateLoopPoll([](PatchpointValue& poll) { poll.pureFunction = PureFunction { .id = 1 }; }) == ValidationRule::LoopPoll);
    CHECK(validateLoopPoll([](PatchpointValue& poll) { poll.effects.reads = pollHeap; }) == ValidationRule::LoopPoll);
    CHECK(validateLoopPoll([](PatchpointValue& poll) { poll.effects.reads = HeapRange(); }) == ValidationRule::LoopPoll);
}

template<typename Functor>
static std::optional<ValidationRule> validatePureFunctionWithEffects(const Functor& changeEffects)
{
    ValidationTestGraph graph;
    changeEffects(graph.appendPureFunction(Int32, PureFunction { .id = 1 })->effects);
    return graph.appendReturnAndValidate();
}

static std::optional<ValidationRule> validatePureFunctionWithChildren(PureFunction pureFunction, unsigned numberOfChildren)
{
    ValidationTestGraph graph;
    Value* child = graph.appendConstant(Int32);
    PatchpointValue* patchpoint = graph.appendPureFunction(Int32, pureFunction);
    for (unsigned i = 0; i < numberOfChildren; ++i)
        patchpoint->append(child, ValueRep::WarmAny);
    return graph.appendReturnAndValidate();
}

static void testValidatePureFunction()
{
    CHECK(validatePureFunctionWithEffects([](Effects&) { }) == std::nullopt);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.writes = HeapRange(1); }) == std::nullopt);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.controlDependent = true; }) == std::nullopt);

    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.reads = HeapRange::top(); }) == ValidationRule::PureFunction);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.exitsSideways = true; }) == ValidationRule::PureFunction);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.fence = true; }) == ValidationRule::PureFunction);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.readsPinned = true; }) == ValidationRule::PureFunction);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.writesPinned = true; }) == ValidationRule::PureFunction);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.readsLocalState = true; }) == ValidationRule::PureFunction);
    CHECK(validatePureFunctionWithEffects([](Effects& effects) { effects.writesLocalState = true; }) == ValidationRule::PureFunction);
    {
        ValidationTestGraph graph;
        graph.appendPureFunction(Int32, PureFunction { .id = 1 })->effects.terminal = true;
        CHECK(findViolatedRule(graph.proc) == ValidationRule::PureFunction);
    }

    for (unsigned numberOfChildren : { 0u, 3u, 4u, 100u }) {
        CHECK(validatePureFunctionWithChildren(PureFunction { .id = 1, .dependsOnChildren = true }, numberOfChildren) == std::nullopt);
        CHECK(validatePureFunctionWithChildren(PureFunction { .id = 1 }, numberOfChildren) == std::nullopt);
    }
}

static void testValidateRelocatable()
{
    {
        ValidationTestGraph graph;
        graph.root->appendNew<RelocatableValue>(graph.proc, Origin(), RelocationSymbol { 1, 2 }, 8);
        CHECK(graph.appendReturnAndValidate() == std::nullopt);
    }
    {
        ValidationTestGraph graph;
        graph.root->appendNew<RelocatableValue>(graph.proc, Origin(), RelocationSymbol { 1, 2 }, 8)->setType(Int32);
        CHECK(graph.appendReturnAndValidate() == ValidationRule::Relocatable);
    }
}

template<typename Functor>
static std::optional<ValidationRule> validateKnownRange(IntRange range, const Functor& appendValue)
{
    ValidationTestGraph graph;
    Value* value = appendValue(graph);
    graph.proc.setKnownRange(value, range);
    CHECK(graph.proc.knownRange(value) == range);
    return graph.appendReturnAndValidate();
}

static void testValidateKnownRange()
{
    IntRange range(0, 10);

    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) { return graph.appendPhi(Int32); }) == std::nullopt);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) { return graph.appendPhi(Int64); }) == std::nullopt);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) { return graph.appendPointer(); }) == std::nullopt);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) { return graph.appendPatchpoint(Int32); }) == std::nullopt);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) {
        return graph.root->appendNew<MemoryValue>(graph.proc, Load, Int32, Origin(), graph.appendPointer());
    }) == std::nullopt);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) {
        return graph.root->appendNew<CCallValue>(graph.proc, Int32, Origin(), graph.appendPointer());
    }) == std::nullopt);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Trunc, Origin(), graph.appendPointer());
    }) == std::nullopt);
    for (B3::Opcode opcode : { Load8Z, Load8S, Load16Z, Load16S }) {
        CHECK(validateKnownRange(range, [&](ValidationTestGraph& graph) {
            return graph.root->appendNew<MemoryValue>(graph.proc, opcode, Origin(), graph.appendPointer());
        }) == std::nullopt);
    }

    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendConstant(Int32), graph.appendConstant(Int32));
    }) == ValidationRule::KnownRange);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) { return graph.appendConstant(Int32); }) == ValidationRule::KnownRange);

    auto appendUnsignedByteLoad = [](ValidationTestGraph& graph) {
        return graph.root->appendNew<MemoryValue>(graph.proc, Load8Z, Origin(), graph.appendPointer());
    };
    CHECK(validateKnownRange(IntRange(255, 300), appendUnsignedByteLoad) == std::nullopt);
    CHECK(validateKnownRange(IntRange(256, 300), appendUnsignedByteLoad) == ValidationRule::KnownRange);
    CHECK(validateKnownRange(IntRange(-10, -1), appendUnsignedByteLoad) == ValidationRule::KnownRange);

#if !ASSERT_ENABLED
    CHECK(validateKnownRange(IntRange(0, 1LL << 40), [](ValidationTestGraph& graph) { return graph.appendPhi(Int32); }) == ValidationRule::KnownRange);
    CHECK(validateKnownRange(IntRange(10, 0), [](ValidationTestGraph& graph) { return graph.appendPhi(Int32); }) == ValidationRule::KnownRange);
    CHECK(validateKnownRange(range, [](ValidationTestGraph& graph) {
        return graph.root->appendNew<MemoryValue>(graph.proc, Load, Double, Origin(), graph.appendPointer());
    }) == ValidationRule::KnownRange);
#endif
}

template<typename Functor>
static std::optional<ValidationRule> validateKnownDereferenceableBytes(const Functor& appendValue)
{
    ValidationTestGraph graph;
    Value* value = appendValue(graph);
    graph.proc.setKnownDereferenceableBytes(value, 16);
    CHECK(graph.proc.knownDereferenceableBytes(value) == 16);
    return graph.appendReturnAndValidate();
}

static void testValidateKnownDereferenceableBytes()
{
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) { return graph.appendPointer(); }) == std::nullopt);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) { return graph.appendPhi(pointerType()); }) == std::nullopt);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendPointer(), graph.appendConstant(pointerType()));
    }) == std::nullopt);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<MemoryValue>(graph.proc, Load, pointerType(), Origin(), graph.appendPointer());
    }) == std::nullopt);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        Value* loaded = graph.root->appendNew<MemoryValue>(graph.proc, Load, pointerType(), Origin(), graph.appendPointer());
        Value* other = graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendPointer(), graph.appendConstant(pointerType()));
        return graph.root->appendNew<Value>(graph.proc, Select, Origin(), loaded, loaded, other);
    }) == std::nullopt);

    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        Value* loaded = graph.root->appendNew<MemoryValue>(graph.proc, Load, pointerType(), Origin(), graph.appendPointer());
        return graph.root->appendNew<Value>(graph.proc, Add, Origin(), loaded, graph.appendConstant(pointerType()));
    }) == ValidationRule::KnownDereferenceableBytes);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendPointer(), graph.appendPointer());
    }) == ValidationRule::KnownDereferenceableBytes);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendConstant(pointerType()), graph.appendConstant(pointerType()));
    }) == ValidationRule::KnownDereferenceableBytes);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Sub, Origin(), graph.appendPointer(), graph.appendConstant(pointerType()));
    }) == ValidationRule::KnownDereferenceableBytes);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Select, Origin(), graph.appendConstant(pointerType()), graph.appendPointer(), graph.appendPointer());
    }) == ValidationRule::KnownDereferenceableBytes);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Select, Origin(), graph.appendPointer(), graph.appendConstant(pointerType()), graph.appendPointer());
    }) == ValidationRule::KnownDereferenceableBytes);
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) {
        Value* sum = graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendPointer(), graph.appendPointer());
        return graph.root->appendNew<Value>(graph.proc, Select, Origin(), graph.appendPointer(), graph.appendPointer(), sum);
    }) == ValidationRule::KnownDereferenceableBytes);

#if !ASSERT_ENABLED
    CHECK(validateKnownDereferenceableBytes([](ValidationTestGraph& graph) { return graph.appendPhi(Int32); }) == ValidationRule::KnownDereferenceableBytes);
#endif
}

struct InductionTestLoop {
    template<typename Functor>
    std::optional<ValidationRule> endBodyAndValidate(const Functor& changeRecord)
    {
        loop.endBody(loop.body);
        std::optional<InductionRecord> record = proc.knownInduction(loop.counter());
        CHECK(record);
        changeRecord(*record);
        proc.knownInductions().add(*record);
        return findViolatedRule(proc);
    }

    Procedure proc { policyWith([](Policy&) { }) };
    Graph graph { proc };
    TopTestedLoop loop { graph };
};

template<typename Functor>
static std::optional<ValidationRule> validateKnownInduction(const Functor& changeRecord)
{
    InductionTestLoop test;
    return test.endBodyAndValidate(changeRecord);
}

static void testValidateKnownInduction()
{
    CHECK(validateKnownInduction([](InductionRecord&) { }) == std::nullopt);
    CHECK(validateKnownInduction([](InductionRecord& record) { record.test = nullptr; }) == std::nullopt);

    CHECK(validateKnownInduction([](InductionRecord& record) { record.step = 2; }) == ValidationRule::KnownInduction);
    CHECK(validateKnownInduction([](InductionRecord& record) { record.step = -1; }) == ValidationRule::KnownInduction);
    CHECK(validateKnownInduction([](InductionRecord& record) { record.step = 0; }) == ValidationRule::KnownInduction);

    {
        InductionTestLoop test;
        CHECK(test.endBodyAndValidate([](InductionRecord& record) { record.next = record.test; }) == std::nullopt);
        CHECK(!test.proc.knownInduction(test.loop.counter()));
    }
    {
        InductionTestLoop test;
        BasicBlock* body = test.loop.body;
        Variable* index = test.loop.index;
        test.graph.set(body, index, test.graph.make(body, Mul, test.graph.get(body, index), test.graph.int64(body, 3)));
        CHECK(test.endBodyAndValidate([](InductionRecord& record) { record.step = 2; }) == std::nullopt);
    }
    {
        ValidationTestGraph graph;
        Value* phi = graph.appendPhi(Int64);
        Value* next = graph.root->appendNew<Value>(graph.proc, Add, Origin(), phi, graph.appendConstant(Int64));
        graph.root->appendNew<UpsilonValue>(graph.proc, Origin(), next, phi);
        graph.proc.setKnownInduction(InductionRecord { phi, next, nullptr, 1, false, false });
        CHECK(graph.proc.knownInduction(phi));
        CHECK(graph.appendReturnAndValidate() == std::nullopt);
    }
}

static std::optional<ValidationRule> validateSuccessorClobbers(Type type, std::initializer_list<unsigned> clobberedSuccessors)
{
    ValidationTestGraph graph;
    PatchpointValue* patchpoint = graph.appendPatchpoint(type);
    patchpoint->effects.terminal = true;
    for (unsigned successorIndex : clobberedSuccessors)
        patchpoint->clobberLateOnSuccessor(successorIndex, RegisterSet::calleeSaveRegisters());

    BasicBlock* first = graph.proc.addBlock();
    BasicBlock* second = graph.proc.addBlock();
    graph.root->setSuccessors(FrequentedBlock(first), FrequentedBlock(second));
    first->appendNew<Value>(graph.proc, Return, Origin());
    second->appendNew<Value>(graph.proc, Return, Origin());
    graph.proc.resetReachability();
    return findViolatedRule(graph.proc);
}

static void testValidateSuccessorClobbers()
{
    CHECK(validateSuccessorClobbers(Void, { }) == std::nullopt);
    CHECK(validateSuccessorClobbers(Void, { 0, 1 }) == std::nullopt);
    CHECK(validateSuccessorClobbers(Int32, { 1 }) == std::nullopt);

    CHECK(validateSuccessorClobbers(Void, { 2 }) == ValidationRule::SuccessorClobbers);
    CHECK(validateSuccessorClobbers(Int32, { 0 }) == ValidationRule::SuccessorClobbers);
    CHECK(validateSuccessorClobbers(Int32, { 0, 1 }) == ValidationRule::SuccessorClobbers);
    {
        ValidationTestGraph graph;
        graph.appendPatchpoint(Void)->clobberLateOnSuccessor(0, RegisterSet::calleeSaveRegisters());
        CHECK(graph.appendReturnAndValidate() == ValidationRule::SuccessorClobbers);
    }
}

static void testFindViolatedRuleReportsUpstreamRules()
{
    ValidationTestGraph graph;
    graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendConstant(Int32), graph.appendConstant(Int64));
    graph.root->appendNew<Value>(graph.proc, Return, Origin());

    UTF8CString dumpBefore = toUTF8CString(graph.proc);
    CHECK(findViolatedRule(graph.proc) == ValidationRule::Upstream);
    CHECK(findViolatedRule(graph.proc) == ValidationRule::Upstream);
    CHECK(toUTF8CString(graph.proc) == dumpBefore);
}

static void testValidationLevels()
{
    Policy policy;

    policy.validation.level = ValidationLevel::FromOptions;
    Procedure fromOptions(policy);
    CHECK(shouldValidateIR(fromOptions) == shouldValidateIR());
    CHECK(shouldValidateIRAtEachPhase(fromOptions) == shouldValidateIRAtEachPhase());

    policy.validation.level = ValidationLevel::AtBoundaries;
    Procedure atBoundaries(policy);
    CHECK(shouldValidateIR(atBoundaries));
    CHECK(shouldValidateIRAtEachPhase(atBoundaries) == shouldValidateIRAtEachPhase());

    policy.validation.level = ValidationLevel::AtEachPhase;
    Procedure atEachPhase(policy);
    CHECK(shouldValidateIR(atEachPhase));
    CHECK(shouldValidateIRAtEachPhase(atEachPhase));
}

static void testPhaseScopeSkipsFailedProcedure()
{
    Policy policy;
    policy.validation.level = ValidationLevel::AtEachPhase;
    ValidationTestGraph graph(policy);
    graph.root->appendNew<Value>(graph.proc, Add, Origin(), graph.appendConstant(Int32), graph.appendConstant(Int64));
    CHECK(graph.appendReturnAndValidate() == ValidationRule::Upstream);

    graph.proc.fail(FailureKind::RelocationNotLowered);
    {
        PhaseScope phaseScope(graph.proc, "testPhaseScopeSkipsFailedProcedure"_s);
    }
    CHECK(graph.proc.report().failure->kind == FailureKind::RelocationNotLowered);
}

inline void addBunValidateTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testValidateLoopPoll());
    RUN(testValidatePureFunction());
    RUN(testValidateRelocatable());
    RUN(testValidateKnownRange());
    RUN(testValidateKnownDereferenceableBytes());
    RUN(testValidateKnownInduction());
    RUN(testValidateSuccessorClobbers());
    RUN(testFindViolatedRuleReportsUpstreamRules());
    RUN(testValidationLevels());
    RUN(testPhaseScopeSkipsFailedProcedure());
}

#endif // ENABLE(B3_JIT)
