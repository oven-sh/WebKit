/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3CompilationReport.h"
#include "B3EliminateCommonSubexpressions.h"
#include "B3EliminateDeadCode.h"
#include "B3InsertionSetInlines.h"
#include "B3LowerRelocatableValues.h"
#include "B3PatchpointValue.h"
#include "B3Policy.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"

class TestRelocationLowering final : public RelocationLowering {
public:
    static constexpr uint32_t declinedSpace = 7;

    static int64_t constantFor(RelocationSymbol symbol, int64_t addend)
    {
        return static_cast<int64_t>(symbol.space) * 1000000 + static_cast<int64_t>(symbol.index) * 1000 + addend;
    }

    Value* lower(Procedure&, InsertionSet& insertionSet, size_t index, const RelocatableValue& value) final
    {
        if (value.symbol().space == declinedSpace)
            return nullptr;
        return insertionSet.insert<Const64Value>(index, value.origin(), constantFor(value.symbol(), value.addend()));
    }
};

struct ValueTestGraph {
    explicit ValueTestGraph(const Policy& policy = validatingPolicy())
        : proc(policy)
    {
    }

    static Policy validatingPolicy()
    {
        return policyWith([](Policy&) { });
    }

    static Policy policyWithLowering(RelocationLowering& lowering)
    {
        Policy policy = validatingPolicy();
        policy.addressing.relocationLowering = &lowering;
        return policy;
    }

    static Policy positionIndependentPolicy(RelocationLowering& lowering)
    {
        Policy policy = policyWithLowering(lowering);
        policy.addressing.isPositionIndependent = true;
        policy.addressing.dataSectionSpace = testDataSectionSpace;
        policy.addressing.jumpTableSpace = testJumpTableSpace;
        return policy;
    }

    RelocatableValue* appendRelocatable(uint32_t space, uint32_t index, int64_t addend = 0)
    {
        return root->appendNew<RelocatableValue>(proc, Origin(), RelocationSymbol { space, index }, addend);
    }

    Value* appendConstant(int64_t value) { return root->appendNew<Const64Value>(proc, Origin(), value); }
    Value* appendAdd(Value* left, Value* right) { return root->appendNew<Value>(proc, Add, Origin(), left, right); }

    PatchpointValue* appendPatchpoint(Type type, std::optional<PureFunction> pureFunction, std::initializer_list<Value*> children)
    {
        PatchpointValue* patchpoint = root->appendNew<PatchpointValue>(proc, type, Origin());
        patchpoint->effects = Effects::none();
        patchpoint->pureFunction = pureFunction;
        for (Value* child : children)
            patchpoint->appendSomeRegister(child);
        return patchpoint;
    }

    void appendReturn(Value* value)
    {
        root->appendNew<Value>(proc, Return, Origin(), value);
        proc.resetReachability();
    }

    void appendReturnOfCall(Value* callee, uintptr_t originNumber)
    {
        appendReturn(root->appendNew<CCallValue>(proc, Int64, numberedOrigin(originNumber), callee));
    }

    Procedure proc;
    BasicBlock* root { proc.addBlock() };
};

static void checkRelocatable(Value* value, uint32_t space, uint32_t index, int64_t addend)
{
    RelocatableValue* relocatable = value->as<RelocatableValue>();
    CHECK(relocatable);
    CHECK(relocatable->type() == pointerType());
    CHECK_EQ(relocatable->symbol().space, space);
    CHECK_EQ(relocatable->symbol().index, index);
    CHECK_EQ(relocatable->addend(), addend);
}

static void checkLoweredToConstant(Value* value, int64_t constant)
{
    CHECK(value->opcode() == Identity);
    CHECK(value->child(0)->isInt64(constant));
}

static void checkFailure(const std::optional<Failure>& failure, FailureKind kind, uintptr_t originNumber)
{
    CHECK(failure);
    CHECK_EQ(failure->kind, kind);
    CHECK(hasNumber(failure->origin, originNumber));
}

static void testRelocatableKey()
{
    ValueTestGraph graph;
    Value* value = graph.appendRelocatable(3, 5, -16);
    CHECK(!value->numChildren());
    CHECK(!value->isConstant());
    CHECK(value->effects() == Effects::none());

    CHECK(value->key());
    CHECK(value->key() == graph.appendRelocatable(3, 5, -16)->key());
    CHECK(value->key() != graph.appendRelocatable(4, 5, -16)->key());
    CHECK(value->key() != graph.appendRelocatable(3, 6, -16)->key());
    CHECK(value->key() != graph.appendRelocatable(3, 5, 16)->key());
    CHECK(value->key() != graph.appendRelocatable(3, 5, -16 + (1ll << 32))->key());
    CHECK(value->key() != graph.appendRelocatable(5, 3, -16)->key());

    checkRelocatable(value->key().materialize(graph.proc, Origin()), 3, 5, -16);
    checkRelocatable(graph.proc.clone(value), 3, 5, -16);

    constexpr uint32_t largeSpace = std::numeric_limits<uint32_t>::max() - 1;
    constexpr uint32_t largeIndex = std::numeric_limits<uint32_t>::max();
    constexpr int64_t minAddend = std::numeric_limits<int64_t>::min();
    Value* extreme = graph.appendRelocatable(largeSpace, largeIndex, minAddend);
    checkRelocatable(extreme->key().materialize(graph.proc, Origin()), largeSpace, largeIndex, minAddend);
    checkRelocatable(graph.proc.clone(extreme), largeSpace, largeIndex, minAddend);
}

static unsigned numberOfRelocatablesAfterNumbering(int64_t firstAddend, int64_t secondAddend)
{
    ValueTestGraph graph;
    graph.appendReturn(graph.appendAdd(graph.appendRelocatable(1, 2, firstAddend), graph.appendRelocatable(1, 2, secondAddend)));
    eliminateCommonSubexpressions(graph.proc);
    eliminateDeadCode(graph.proc);
    return numberOfValues(graph.proc, Relocatable);
}

static void testRelocatableIsNumbered()
{
    CHECK_EQ(numberOfRelocatablesAfterNumbering(8, 8), 1u);
    CHECK_EQ(numberOfRelocatablesAfterNumbering(8, 16), 2u);
}

static void testLowerRelocatableValues()
{
    TestRelocationLowering lowering;
    Policy policy = ValueTestGraph::policyWithLowering(lowering);

    ValueTestGraph graph(policy);
    Procedure& proc = graph.proc;
    BasicBlock* tail = proc.addBlock();
    Value* first = graph.appendRelocatable(1, 2, 3);
    graph.root->appendNew<Value>(proc, Jump, Origin());
    graph.root->setSuccessors(FrequentedBlock(tail));
    Value* second = tail->appendNew<RelocatableValue>(proc, Origin(), RelocationSymbol { 4, 5 }, static_cast<int64_t>(-6));
    tail->appendNew<Value>(proc, Return, Origin(), tail->appendNew<Value>(proc, Add, Origin(), first, second));
    proc.resetReachability();

    int64_t firstConstant = TestRelocationLowering::constantFor(RelocationSymbol { 1, 2 }, 3);
    int64_t secondConstant = TestRelocationLowering::constantFor(RelocationSymbol { 4, 5 }, -6);

    CHECK(lowerRelocatableValues(proc));
    CHECK(!proc.report().failure);
    CHECK_EQ(numberOfValues(proc, Relocatable), 0u);
    checkLoweredToConstant(first, firstConstant);
    checkLoweredToConstant(second, secondConstant);
    for (Value* value : proc.values()) {
        for (Value* child : value->children()) {
            CHECK(child->opcode() != Relocatable);
            CHECK(child->opcode() != Identity);
        }
    }
    CHECK(!lowerRelocatableValues(proc));
    CHECK_EQ(compileAndRun<int64_t>(proc), firstConstant + secondConstant);

    ValueTestGraph graphWithoutRelocatable(policy);
    graphWithoutRelocatable.appendReturn(graphWithoutRelocatable.appendConstant(42));
    CHECK(!lowerRelocatableValues(graphWithoutRelocatable.proc));
    CHECK(!graphWithoutRelocatable.proc.report().failure);
}

static void testLowerRelocatableValuesDeclined()
{
    TestRelocationLowering lowering;
    ValueTestGraph graph(ValueTestGraph::policyWithLowering(lowering));
    Value* before = graph.appendRelocatable(1, 2, 3);
    Value* declined = graph.appendRelocatable(TestRelocationLowering::declinedSpace, 2, 3);
    declined->setOrigin(numberedOrigin(9));
    Value* after = graph.appendRelocatable(1, 4);
    graph.appendReturn(graph.appendAdd(graph.appendAdd(before, declined), after));

    CHECK(lowerRelocatableValues(graph.proc));
    checkFailure(graph.proc.report().failure, FailureKind::RelocationNotLowered, 9);
    checkRelocatable(declined, TestRelocationLowering::declinedSpace, 2, 3);
    CHECK(graph.root->values().contains(declined));
    CHECK_EQ(numberOfValues(graph.proc, Relocatable), 1u);
    CHECK(!findViolatedRule(graph.proc));
    checkLoweredToConstant(before, TestRelocationLowering::constantFor(RelocationSymbol { 1, 2 }, 3));
    checkLoweredToConstant(after, TestRelocationLowering::constantFor(RelocationSymbol { 1, 4 }, 0));
}

static void testLowerRelocatableValuesWithoutLowering()
{
    ValueTestGraph graph { Policy() };
    Value* value = graph.appendRelocatable(1, 2, 3);
    value->setOrigin(numberedOrigin(9));
    graph.appendReturn(value);

    CHECK(!lowerRelocatableValues(graph.proc));
    checkFailure(graph.proc.report().failure, FailureKind::RelocationNotLowered, 9);
    checkRelocatable(value, 1, 2, 3);
    CHECK(!findViolatedRule(graph.proc));
}

template<typename Functor>
static std::optional<Failure> failureAfterLoweringCall(const Policy& policy, const Functor& appendCallee)
{
    ValueTestGraph graph(policy);
    graph.appendReturnOfCall(appendCallee(graph), 5);
    CHECK(!lowerRelocatableValues(graph.proc));
    CHECK(!findViolatedRule(graph.proc));
    return graph.proc.report().failure;
}

template<typename Functor>
static std::optional<Failure> failureAfterPreparingCall(const Policy& policy, const Functor& appendCallee)
{
    ValueTestGraph graph(policy);
    graph.appendReturnOfCall(appendCallee(graph), 5);
    CHECK(!graph.proc.hasEverAdded(Relocatable));
    prepareForGeneration(graph.proc);
    return graph.proc.report().failure;
}

static void testConstantCalleeWhenPositionIndependent()
{
    auto appendConstantCallee = [](ValueTestGraph& graph) {
        return graph.appendConstant(4096);
    };
    auto appendCopiedConstantCallee = [](ValueTestGraph& graph) {
        return graph.root->appendNew<Value>(graph.proc, Identity, Origin(), graph.appendConstant(4096));
    };
    auto appendArgumentCallee = [](ValueTestGraph& graph) {
        return graph.root->appendNew<ArgumentRegValue>(graph.proc, Origin(), GPRInfo::argumentGPR0);
    };
    TestRelocationLowering lowering;
    Policy positionIndependent = ValueTestGraph::positionIndependentPolicy(lowering);

    checkFailure(failureAfterLoweringCall(positionIndependent, appendConstantCallee), FailureKind::AbsoluteAddress, 5);
    checkFailure(failureAfterLoweringCall(positionIndependent, appendCopiedConstantCallee), FailureKind::AbsoluteAddress, 5);
    CHECK(!failureAfterLoweringCall(Policy(), appendConstantCallee));
    CHECK(!failureAfterLoweringCall(positionIndependent, appendArgumentCallee));

    checkFailure(failureAfterPreparingCall(positionIndependent, appendConstantCallee), FailureKind::AbsoluteAddress, 5);
    CHECK(!failureAfterPreparingCall(Policy(), appendConstantCallee));
}

static void testPatchpointHasNoKey()
{
    ValueTestGraph graph;
    Value* child = graph.appendConstant(1);
    constexpr PureFunction childIndependent { .id = 1 };
    constexpr PureFunction childDependent { .id = 1, .dependsOnChildren = true };

    CHECK(!graph.root->appendNew<PatchpointValue>(graph.proc, Int64, Origin())->key());
    CHECK(!graph.appendPatchpoint(Int64, std::nullopt, { })->key());
    CHECK(!graph.appendPatchpoint(Int64, std::nullopt, { child })->key());
    CHECK(!graph.appendPatchpoint(Int64, childIndependent, { })->key());
    CHECK(!graph.appendPatchpoint(Int64, childIndependent, { child })->key());
    CHECK(!graph.appendPatchpoint(Int64, childDependent, { })->key());
    CHECK(!graph.appendPatchpoint(Int64, childDependent, { child })->key());
}

static void testIsSamePureCall()
{
    ValueTestGraph graph;
    Value* a = graph.appendConstant(1);
    Value* b = graph.appendConstant(2);
    Value* c = graph.appendConstant(3);
    auto keepChild = [](Value* child) {
        return child;
    };
    auto replaceAWithC = [&](Value* child) {
        return child == a ? c : child;
    };
    auto append = [&](PureFunction pureFunction, std::initializer_list<Value*> children, Type type = Int64) {
        return graph.appendPatchpoint(type, pureFunction, children);
    };
    auto isSame = [](const PatchpointValue* first, const PatchpointValue* second, const auto& canonicalChild) {
        bool result = isSamePureCall(first, second, canonicalChild);
        CHECK_EQ(isSamePureCall(second, first, canonicalChild), result);
        return result;
    };

    constexpr uint64_t id = 7ull << 32 | 9;
    constexpr PureFunction childIndependent { .id = id };
    constexpr PureFunction childDependent { .id = id, .dependsOnChildren = true };

    PatchpointValue* call = append(childDependent, { a, b });
    CHECK(isSame(call, call, keepChild));
    CHECK(isSame(call, append(childDependent, { a, b }), keepChild));
    CHECK(!isSame(call, append(PureFunction { .id = id + 1, .dependsOnChildren = true }, { a, b }), keepChild));
    CHECK(!isSame(call, append(PureFunction { .id = id + (1ull << 32), .dependsOnChildren = true }, { a, b }), keepChild));
    CHECK(!isSame(call, append(childDependent, { a, b }, Int32), keepChild));
    CHECK(!isSame(call, append(childDependent, { a, c }), keepChild));
    CHECK(!isSame(call, append(childDependent, { b, a }), keepChild));
    CHECK(!isSame(call, append(childDependent, { a }), keepChild));
    CHECK(!isSame(call, append(childDependent, { a, b, c }), keepChild));
    CHECK(!isSame(call, append(childIndependent, { a, b }), keepChild));

    CHECK(isSame(append(childDependent, { a, b, c, a, b }), append(childDependent, { a, b, c, a, b }), keepChild));
    CHECK(!isSame(append(childDependent, { a, b, c, a, b }), append(childDependent, { a, b, c, a, c }), keepChild));

    CHECK(isSame(append(childIndependent, { a, b }), append(childIndependent, { c }), keepChild));
    CHECK(isSame(append(childIndependent, { a, b }), append(childIndependent, { }), keepChild));
    CHECK(!isSame(append(childIndependent, { }), append(PureFunction { .id = id + 1 }, { }), keepChild));
    CHECK(!isSame(append(childIndependent, { }), append(childIndependent, { }, Int32), keepChild));

    CHECK(!isSame(call, append(childDependent, { c, b }), keepChild));
    CHECK(isSame(call, append(childDependent, { c, b }), replaceAWithC));
    CHECK(isSame(call, append(childDependent, { a, b }), replaceAWithC));
    CHECK(!isSame(call, append(childDependent, { b, b }), replaceAWithC));
    CHECK(!isSame(call, append(childDependent, { c, a }), replaceAWithC));

    PatchpointValue* patchpointWithoutPureFunction = graph.appendPatchpoint(Int64, std::nullopt, { a, b });
    CHECK(!isSame(patchpointWithoutPureFunction, patchpointWithoutPureFunction, keepChild));
    CHECK(!isSame(patchpointWithoutPureFunction, graph.appendPatchpoint(Int64, std::nullopt, { a, b }), keepChild));
    CHECK(!isSame(patchpointWithoutPureFunction, call, keepChild));
}

static void testFrozenConstraints()
{
    ValueTestGraph graph;
    PatchpointValue* patchpoint = graph.appendPatchpoint(Int64, std::nullopt, { graph.appendConstant(1) });
    CHECK(!patchpoint->areConstraintsFrozen());
    CHECK(!graph.proc.clone(patchpoint)->as<PatchpointValue>()->areConstraintsFrozen());
    patchpoint->setConstraint(0, ValueRep::WarmAny);
    CHECK(patchpoint->constrainedChild(0).rep() == ValueRep::WarmAny);

    patchpoint->freezeConstraints();
    CHECK(patchpoint->areConstraintsFrozen());
    CHECK(graph.proc.clone(patchpoint)->as<PatchpointValue>()->areConstraintsFrozen());

    CHECK(!patchpoint->lateClobbered().contains(GPRInfo::argumentGPR1, IgnoreVectors));
    patchpoint->clobberLate(RegisterSet(GPRInfo::argumentGPR1));
    CHECK(patchpoint->lateClobbered().contains(GPRInfo::argumentGPR1, IgnoreVectors));

    CheckValue* check = graph.root->appendNew<CheckValue>(graph.proc, Check, Origin(), graph.appendConstant(0));
    CHECK(!check->areConstraintsFrozen());
}

inline void addBunValueTests(const TestConfig* config, Deque<RefPtr<SharedTask<void()>>>& tasks)
{
    RUN(testRelocatableKey());
    RUN(testRelocatableIsNumbered());
    RUN(testLowerRelocatableValues());
    RUN(testLowerRelocatableValuesDeclined());
    RUN(testLowerRelocatableValuesWithoutLowering());
    RUN(testConstantCalleeWhenPositionIndependent());
    RUN(testPatchpointHasNoKey());
    RUN(testIsSamePureCall());
    RUN(testFrozenConstraints());
}

#endif // ENABLE(B3_JIT)
