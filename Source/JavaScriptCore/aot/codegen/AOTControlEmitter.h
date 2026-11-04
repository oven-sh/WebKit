/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBranchCounterNumbers.h"
#include "AOTOperationEmitter.h"
#include "AOTSwitchWindow.h"
#include "AOTVector.h"
#include <optional>
#include <span>

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
    B3::FrequentedBlock countedEdgeTo(const TerminatorContext&, uint32_t targetIndex, std::span<const Operand> declaredResults = { });

private:
    bool leavesAnotherWayUnlessPromised(const TerminatorContext&, ValueID);
    void testBitsOfWindow(const TerminatorContext&, const SwitchWindow&, LValue subject, LType, B3::FrequentedBlock otherwise);
    void switchOnInteger(const TerminatorContext&, LValue subject, LType, B3::FrequentedBlock otherwise);
    void switchOnBoxedInteger(const TerminatorContext&, Boxed subject, B3::FrequentedBlock otherwise);

    std::optional<Vector<CountedBranch>> m_countedBranches;
    LBasicBlock m_testedReturn { nullptr };
    LValue m_resultOfTestedReturn { nullptr };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
