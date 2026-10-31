/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTKnownValue.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <tuple>
#include "AOTBitVector.h"

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;
class UseLists;

bool hasEdgeResults(const Function&, Edge);
bool isConditionOfBranchWithProofs(const Function&, const UseLists&, ValueID);
// The same answer for every value at once, from the terminators of the blocks that are reached: for whoever asks nothing else of the use lists.
BitVector conditionsOfBranchesWithProofs(const Function&, const CFG&);

bool canFollowWithoutTest(const Function&, Edge);
Arguments argumentsWithoutTest(const Function&, Edge);
bool argumentsFitParameters(const Program&, const Function&, const Target&);
bool argumentsFitParametersAfterCasts(const Program&, const Function&, TypeArena&, const Target&);
void castArgumentsToParameters(IRBuilder&, BlockID jumpingBlock, bool validates);

const Value* typeTestBranchedOn(const Function&, const Block&);
bool holdsOnlyKnownFunctions(const Type&);

// Nothing is left out of the type of a value that may be any function where a test for one function refuses it. Such a test may refuse a closure that is no longer as it was created.
bool mayRefuseChangedClosures(const Type& typeOfTestedValue);

struct BranchOnTypeTest {
    ValueID subject;
    TypeID testedType;
    BlockID block;
    bool mayRefuseChangedClosures { false };

    auto test() const { return std::tuple { subject.index(), testedType.index() }; }
};

struct BranchOnValue {
    ValueID operand;
    BlockID block;
};

// Leaves the tests that several of the branches are on, sorted by test, whichever cast of the value each of them names.
void keepRepeatedTests(FunctionAnalyses&, Vector<BranchOnTypeTest>&);

class TypeTestBranches {
public:
    explicit TypeTestBranches(FunctionAnalyses&);

    KnownBoolean outcomeFromDominatingTest(BlockID branchingBlock) const;
    KnownBoolean outcomeOnEveryWay(BlockID branchingBlock) const;
    KnownBoolean outcomeFromDominatingBranches(ValueID condition, BlockID branchingBlock) const;
    bool gaveUpOnLongWays() const { return m_gaveUpOnLongWays; }
    // In reverse post order.
    std::span<const BlockID> blocksThatBranchOnTypeTest() const LIFETIME_BOUND { return m_blocksThatBranchOnTypeTest.span(); }

private:
    FunctionAnalyses& m_analyses;
    Vector<BranchOnTypeTest> m_branches;
    BitVector m_repeatsTest;
    // By block, for the blocks of m_repeatsTest: the index of its branch in m_branches.
    Vector<uint32_t> m_indexOfBranch;
    Vector<BlockID, 16> m_blocksThatBranchOnTypeTest;
    // The conditional branches by the values among their operands that several of them name, those on one value in reverse post order.
    Vector<BranchOnValue, 32> m_branchesByOperand;
    mutable Vector<KnownBoolean> m_outcomesOfFrequentTests;
    mutable Vector<KnownBoolean> m_outcomesOfFrequentConditions;
    mutable bool m_gaveUpOnLongWays { false };
};

// The tests of values that were read from slots.
class SlotValueTests {
public:
    SlotValueTests(FunctionAnalyses&, TypeArena&, std::span<const BlockID> blocksThatBranchOnTypeTest);

    KnownBoolean outcomeFromDominatingTests(BlockID branchingBlock);

private:
    // A block that is entered from the test only.
    struct TestedRead {
        ValueID object;
        ValueID read;
        BlockID block;
        // None where the branch is on the value itself.
        std::optional<uint32_t> operandIndex;
        BlockID target;
        std::optional<Vector<LayoutID>> layoutsRuledOut;
    };

    void collectTestedReads(std::span<const ValueID> testedObjects, BlockID);
    void addTestedRead(std::span<const ValueID> testedObjects, BlockID, ValueID tested, std::optional<uint32_t> operandIndex);
    const Vector<Type>& slotTypesByReceiverOf(ValueID read) LIFETIME_BOUND;
    const Vector<LayoutID>& layoutsRuledOutBy(TestedRead&) LIFETIME_BOUND;

    FunctionAnalyses& m_analyses;
    TypeArena& m_types;
    Vector<TestedRead> m_reads;
    Vector<std::pair<ValueID, std::unique_ptr<Vector<Type>>>, 4> m_slotTypesByReceiver;
};

// What the tests above a block have left of the values that its own test looks at, whichever type or value each of them asks for.
class TestsOfSameValues {
public:
    TestsOfSameValues(FunctionAnalyses&, TypeArena&);

    // What is expected of a value where the tests above have left nothing else of it.
    struct ProvenExpectation {
        BlockID block;
        ValueID expectation;
        TypeID type;
    };

    KnownBoolean outcomeBehindDominatingTests(BlockID branchingBlock);
    Vector<ProvenExpectation, 4> takeProvenExpectations();
    bool isEmpty() const { return m_operands.isEmpty(); }
    // Branches have become jumps. The values are the same. False if no test is left that could say something of another.
    bool didChangeControlFlow();

private:
    struct TestedOperand {
        ValueID subject;
        BlockID block;
        // The test that the block branches on, or what the block expects of the value.
        ValueID user;
        uint32_t operandIndex;
        bool isExpectation { false };
    };

    bool isLive(const TestedOperand&) const;
    void computeOutcomes();

    FunctionAnalyses& m_analyses;
    TypeArena& m_types;
    // The operands that stand for a value which several blocks test or expect something of, those for one value side by side. At least one of them is tested.
    Vector<TestedOperand, 16> m_operands;
    BitVector m_blocksThatTest;
    bool m_hasExpectations { false };
    bool m_hasOutcomes { false };
    Vector<KnownBoolean> m_outcomes;
    Vector<ProvenExpectation, 4> m_provenExpectations;
};

void proveExpectation(IRBuilder&, const TestsOfSameValues::ProvenExpectation&);

KnownBoolean evaluateConditionByDefinitions(FunctionAnalyses&, BlockID branchingBlock);
KnownBoolean evaluateCondition(FunctionAnalyses&, const TypeTestBranches&, SlotValueTests&, TestsOfSameValues&, BlockID branchingBlock);
std::optional<uint32_t> decidedTargetOf(const Terminator&, KnownValue tested);

void decideTerminator(IRBuilder&, BlockID, uint32_t targetIndex);
// A test is decided against what was expected of it: the arm that was to run seldom runs whenever the test does, and so does what only that arm leads to.
void raiseFrequenciesBehind(FunctionAnalyses&, Function&, Edge decided);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
