/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeRecords.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AirInst.h"
#include "B3Value.h"

namespace JSC { namespace AOT { namespace Typed {

InstructionObserver::InstructionObserver(CodeUnitRecorder& units, TrapRecords& traps)
    : m_units(units)
    , m_traps(traps)
{
}

void InstructionObserver::willGenerate(const B3::Air::BasicBlock& block)
{
    m_units.willGenerate(block);
}

static const B3::Air::Arg& addressOf(const B3::Air::Inst& inst)
{
    for (const B3::Air::Arg& arg : inst.args()) {
        if (!arg.isMemory())
            continue;
        RELEASE_ASSERT(!arg.isStack() && !arg.isCallArg());
        return arg;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void InstructionObserver::didGenerate(const B3::Air::Inst& inst, MacroAssembler::Label begin, MacroAssembler::Label end)
{
    m_units.didGenerate(inst, begin, end);

    // A fenced access on x86-64 has kind.effects too, and the address computation of a trapping access has the access as its origin.
    const B3::Value* access = inst.origin;
    if (!inst.kind.effects || !access || !access->traps())
        return;
    // `[cage + 8 * word + offset]`: the reference was never made a pointer.
    const B3::Air::Arg& address = addressOf(inst);
    bool isNarrow = address.isIndex();
    RELEASE_ASSERT(!isNarrow || (address.base().gpr() == cellCageBaseGPR && address.scale() == 1u << cell32Shift));
    m_traps.trappingAccesses.append(TrappingAccess { begin, valueOfOrigin(access->origin()), (isNarrow ? address.index() : address.base()).gpr(), isNarrow });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
