/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AirReportUsedRegisters.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirRegLiveness.h"
#include "AirPhaseScope.h"

namespace JSC { namespace B3 { namespace Air {

bool reportUsedRegisters(Code& code)
{
    PhaseScope phaseScope(code, "reportUsedRegisters"_s);

    RegLiveness liveness(code);

    bool changed = false;
    for (BasicBlock* block : code) {
        RegLiveness::LocalCalc localCalc(liveness, block);
        unsigned removed = 0;

        for (unsigned instIndex = block->size(); instIndex--;) {
            Inst& inst = block->at(instIndex);

            // Kill dead assignments to registers. For simplicity we say that a store is killable if
            // it has only late defs and those late defs are to registers that are dead right now.
            if (localCalc.definesOnlyDeadRegisters(instIndex) && !inst.hasNonArgEffects()) {
                inst = Inst();
                ++removed;
            } else if (inst.kind.opcode == Patch)
                inst.reportUsedRegisters(localCalc.live());
            localCalc.execute(instIndex);
        }

        if (!removed)
            continue;
        unsigned sizeBefore = block->size();
        block->insts().removeAllMatching(
            [&](const Inst& inst) -> bool {
                return !inst;
            });
        if (sizeBefore - removed <= 1)
            changed = true;
    }

    return changed;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

