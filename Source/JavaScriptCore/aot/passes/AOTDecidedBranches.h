/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTKnownValue.h"
#include <optional>
#include <span>
#include <tuple>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class IRBuilder;
class UseLists;

bool hasEdgeResults(const Function&, Edge);
bool isConditionOfBranchWithProofs(const Function&, const UseLists&, ValueID);

bool canFollowWithoutTest(const Function&, Edge);
Arguments argumentsWithoutTest(const Function&, Edge);
bool argumentsFitParameters(const Program&, const Function&, const Target&);
bool argumentsFitParametersAfterCasts(const Program&, const Function&, TypeArena&, const Target&);
void castArgumentsToParameters(IRBuilder&, BlockID jumpingBlock, bool validates);

// The branches on a test of one value for one type, whichever cast of the value each of them names.
class TypeTestBranches {
public:
    explicit TypeTestBranches(FunctionAnalyses&);

    KnownBoolean outcomeFromDominatingTest(BlockID branchingBlock) const;

private:
    struct TestedBranch {
        ValueID subject;
        TypeID testedType;
        BlockID block;

        auto test() const { return std::tuple { subject.index(), testedType.index() }; }
    };

    FunctionAnalyses& m_analyses;
    Vector<TestedBranch> m_branches;
};

// The tests of values that were read from slots. Each layout says what its slot holds, so an edge of such a test rules out the layouts whose slot
// holds nothing that passes along it. An object keeps its layout, whatever is stored into the slot afterwards.
class SlotValueTests {
public:
    SlotValueTests(FunctionAnalyses&, TypeArena&);

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
    bool isRuledOut(const TestedRead&, uint32_t targetIndex, LayoutID) const;
    const Vector<LayoutID>& layoutsRuledOutBy(TestedRead&) LIFETIME_BOUND;

    FunctionAnalyses& m_analyses;
    TypeArena& m_types;
    Vector<TestedRead> m_reads;
};

KnownBoolean evaluateCondition(FunctionAnalyses&, const TypeTestBranches&, SlotValueTests&, BlockID branchingBlock);
std::optional<uint32_t> decidedTargetOf(const Terminator&, KnownValue tested);

void decideTerminator(IRBuilder&, BlockID, uint32_t targetIndex);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
