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

    BranchTargets branchTargetsOf(const TerminatorContext& terminator) { return { generator().edgeTo(terminator, 0), generator().edgeTo(terminator, 1) }; }

private:
    void switchOnInteger(const TerminatorContext&, LValue subject, LType, BlockEdge otherwise);
    void switchOnBoxedInteger(const TerminatorContext&, Boxed subject, BlockEdge otherwise);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
