/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBranchCounterNumbers.h"
#include "AOTOperationEmitter.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class ControlEmitter final : public OperationEmitter {
public:
    explicit ControlEmitter(CodeGenerator&);

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
    void countBranch(const TerminatorContext&, uint32_t targetIndex, B3::FrequentedBlock& edge);
    void switchOnInteger(const TerminatorContext&, LValue subject, LType, B3::FrequentedBlock otherwise);
    void switchOnBoxedInteger(const TerminatorContext&, Boxed subject, B3::FrequentedBlock otherwise);

    std::optional<Vector<CountedBranch>> m_countedBranches;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
