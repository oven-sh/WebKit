/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTInstanceLayout.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "LinkBuffer.h"

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfHints()
{
    if (!hasFaultingLoopPolls)
        return { };
    return { Opcode::LoopHint };
}

static void generateLoopHint(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    AllowMacroScratchRegisterUsage allowScratch(jit);
    uint32_t site = emitter.addSite(value.origin(), { }, value.opcode());
    emitter.noteSiteMayThrow(site);
    CCallHelpers::Label label = jit.labelIgnoringWatchpoints();
    ptrdiff_t offset = InstanceLayout::offsetOfLoopPollPage();
#if CPU(ARM64)
    jit.m_assembler.ldr<64>(ARM64Registers::zr, instanceGPR, static_cast<unsigned>(offset));
#else
    jit.load32(CCallHelpers::Address(instanceGPR, offset), jit.scratchRegister());
#endif
    jit.addLinkTask([records = &emitter.records(), label, site](LinkBuffer& linkBuffer) {
        records->traps.pollSites.append(PollSite { linkBuffer.offsetOf(label), false, OptionalIndex { site } });
    });
}

void StraightEmitter::generateHint(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::LoopHint:
        generateLoopHint(*this, value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
