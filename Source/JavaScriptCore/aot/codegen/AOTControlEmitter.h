/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ControlEmitter final : public OperationEmitter {
public:
    explicit ControlEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    void generateJump(const TerminatorContext& terminator) { generator().jumpTo(terminator, 0); }
    void generateBranch(const TerminatorContext& terminator)
    {
        if (generator().holdsByQuestionInFrontOfTurnsLaidOut(terminator->conditionOperand())) {
            generator().jumpTo(terminator, 0);
            return;
        }
        if (terminator.generatesCondition()) {
            generateTestAndBranch(terminator);
            return;
        }
        BranchTargets targets = branchTargetsOf(terminator);
        branch(in<Condition>(terminator->conditionOperand()), targets);
    }
    void generateSwitch(const TerminatorContext&);
    void generateSwitchString(const TerminatorContext&);
    void generateReturn(const TerminatorContext&);
    void generateReturnCall(const TerminatorContext&);
    void generateThrow(const TerminatorContext&);
    void generateRethrow(const TerminatorContext&);
    void generateUnreachable(const TerminatorContext&);
    void generateSwitchOnShape(const TerminatorContext&);
    void generateTestAndBranch(const TerminatorContext& terminator) { generator().generateBranchCondition(terminator, terminator->conditionOperand(), branchTargetsOf(terminator)); }

    Condition isLess(ValueID left, ValueID right) { return lessThan(integerOf(left), integerOf(right)); }

    BranchTargets branchTargetsOf(const TerminatorContext& terminator) { return { generator().edgeTo(terminator, 0), generator().edgeTo(terminator, 1) }; }

private:
    struct SwitchesBehindSwitch {
        Vector<int64_t, 16> casesThatLeadToPlace;
        BitVector casesOfFirstThatLeadOn;
        uint32_t firstCaseOfPlace { 0 };
        std::optional<BlockEdge> otherwise;
    };
    bool testByteOfTableInPlaceOfSwitch(const TerminatorContext&, LValue subject, LType, BlockEdge otherwise, SwitchesBehindSwitch&);
    BlockEdge switchOnInteger(const TerminatorContext&, LValue subject, LType, BlockEdge otherwise);
    void switchOnBoxedInteger(const TerminatorContext&, Boxed subject, BlockEdge otherwise);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
