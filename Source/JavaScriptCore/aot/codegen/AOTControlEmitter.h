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

    void generateJump(const TerminatorContext&);
    void generateBranch(const TerminatorContext&);
    void generateSwitch(const TerminatorContext&);
    void generateSwitchString(const TerminatorContext&);
    void generateReturn(const TerminatorContext&);
    void generateReturnCall(const TerminatorContext&);
    void generateThrow(const TerminatorContext&);
    void generateRethrow(const TerminatorContext&);
    void generateUnreachable(const TerminatorContext&);
    void generateSwitchOnShape(const TerminatorContext&);
    void generateTestAndBranch(const TerminatorContext&);

    BranchTargets branchTargetsOf(const TerminatorContext&);

private:
    void switchOnInteger(const TerminatorContext&, LValue subject, LType, BlockEdge otherwise);
    void switchOnBoxedInteger(const TerminatorContext&, Boxed subject, BlockEdge otherwise);
    // AOTUnionSwitchEmitter.cpp. switchOnInteger(), in front of m_out.switchOn(). The cases lead to few blocks and lie close together: a range test and one test of a bit of a constant for each block. False: nothing was emitted.
    bool trySwitchByBitsOfConstants(LValue subject, LType, std::span<const SwitchTarget>, BlockEdge otherwise);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
