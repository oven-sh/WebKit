/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RegisterPressure.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "RegisterSet.h"

namespace JSC { namespace B3 {

template<typename Functor>
static unsigned countAllocatableRegisters(const Procedure& proc, Bank bank, const Functor& isPreserved)
{
    unsigned result = 0;
    for (Reg reg : proc.code().regsInPriorityOrder(bank))
        result += isPreserved(reg);
    return result;
}

template<typename Functor>
static RegisterBudget budgetOf(const Procedure& proc, const Functor& isPreserved)
{
    return { countAllocatableRegisters(proc, GP, isPreserved), countAllocatableRegisters(proc, FP, isPreserved) };
}

RegisterBudget allocatableRegisters(const Procedure& proc)
{
    const Air::Code& code = proc.code();
    return { static_cast<unsigned>(code.regsInPriorityOrder(GP).size()), static_cast<unsigned>(code.regsInPriorityOrder(FP).size()) };
}

RegisterBudget registersPreservedBy(const Procedure& proc, const Value* value)
{
    if (value->opcode() == CCall) {
        RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
        return budgetOf(proc, [&](Reg reg) {
            return calleeSaves.contains(reg, IgnoreVectors);
        });
    }
    auto* patchpoint = value->as<PatchpointValue>();
    if (!patchpoint)
        return allocatableRegisters(proc);

    RegisterBudget result = budgetOf(proc, [&](Reg reg) {
        return !patchpoint->earlyClobbered().contains(reg, IgnoreVectors) && !patchpoint->lateClobbered().contains(reg, IgnoreVectors);
    });
    result.gprs -= std::min<unsigned>(result.gprs, patchpoint->numGPScratchRegisters);
    result.fprs -= std::min<unsigned>(result.fprs, patchpoint->numFPScratchRegisters);
    return result;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
