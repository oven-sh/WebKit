/*
 * Copyright (C) 2015-2019 Apple Inc. All rights reserved.
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
#include "AirHandleCalleeSaves.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "RegisterSet.h"

namespace JSC { namespace B3 { namespace Air {

static void addSuffixSavedForSuspensions(RegisterSet& usedCalleeSaves, const RegisterSet& clobberedAtSuspensions)
{
    Vector<Reg, 8> registersInSaveOrder;
    clobberedAtSuspensions.forEach(
        [&](Reg reg) {
            registersInSaveOrder.append(reg);
        });

    size_t first = registersInSaveOrder.findIf(
        [&](Reg reg) {
            return usedCalleeSaves.contains(reg, IgnoreVectors);
        });
    if (first == notFound)
        return;

    constexpr size_t registersPerSave = isARM64() ? 2 : 1;
    for (size_t position = first - first % registersPerSave; position < registersInSaveOrder.size(); ++position)
        usedCalleeSaves.add(registersInSaveOrder[position], IgnoreVectors);
}

// Nothing saves a register that the policy pins: it keeps its value only while no instruction writes it and no patch clobbers it.
static void checkPinnedRegistersAreOnlyRead(Code& code)
{
    RegisterSet writtenRegisters;
    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            inst.forEachTmp(
                [&] (Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (Arg::isAnyDef(role))
                        writtenRegisters.add(tmp.reg(), IgnoreVectors);
                });

            if (inst.kind.opcode == Patch) {
                writtenRegisters.merge(inst.extraClobberedRegs());
                writtenRegisters.merge(inst.extraEarlyClobberedRegs());
            }
        }
    }

    writtenRegisters.filter(code.policy().registers.pinned);
    if (writtenRegisters.isEmpty())
        return;
    dataLogLn("Air: the code writes or clobbers ", writtenRegisters, ", which the register policy pins");
    RELEASE_ASSERT_NOT_REACHED();
}

void handleCalleeSaves(Code& code)
{
    static_assert(Reg::maxIndex() < 64);
    uint64_t namedRegisters = 0;
    RegisterSet clobberedRegisters;

    for (BasicBlock* block : code) {
        for (Inst& inst : *block) {
            inst.forEachTmpFast(
                [&] (Tmp& tmp) {
                    namedRegisters |= static_cast<uint64_t>(1) << tmp.reg().index();
                });

            if (inst.kind.opcode == Patch) {
                clobberedRegisters.merge(inst.extraClobberedRegs());
                clobberedRegisters.merge(inst.extraEarlyClobberedRegs());
            }
        }
    }

    clobberedRegisters.exclude(code.calleeSavesClobberedAtSuspensions());
    for (uint64_t registers = namedRegisters; registers; registers &= registers - 1)
        clobberedRegisters.add(Reg::fromIndex(std::countr_zero(registers)), IgnoreVectors);

    RegisterSet pinnedRegistersInUse = clobberedRegisters;
    pinnedRegistersInUse.filter(code.policy().registers.pinned);
    if (!pinnedRegistersInUse.isEmpty()) [[unlikely]]
        checkPinnedRegistersAreOnlyRead(code);

    handleCalleeSaves(code, WTF::move(clobberedRegisters));
}

void handleCalleeSaves(Code& code, RegisterSet usedCalleeSaves)
{
    // We filter to really get the callee saves.
    RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
    calleeSaves.exclude(code.unsavedCalleeSaves());
    calleeSaves.merge(code.policy().registers.savedByCallee);
    usedCalleeSaves.filter(calleeSaves);
    usedCalleeSaves.filter(code.mutableRegs());
    usedCalleeSaves.exclude(RegisterSet::stackRegisters()); // We don't need to save FP here.
    addSuffixSavedForSuspensions(usedCalleeSaves, code.calleeSavesClobberedAtSuspensions());

    auto calleSavesToSave = usedCalleeSaves;

    if (!calleSavesToSave.numberOfSetRegisters())
        return;

    RegisterAtOffsetList calleeSaveRegisters = RegisterAtOffsetList(calleSavesToSave);

    size_t byteSize = 0;
    for (const RegisterAtOffset& entry : calleeSaveRegisters)
        byteSize = std::max(static_cast<size_t>(-entry.offset()), byteSize);
    ASSERT(calleeSaveRegisters.sizeOfAreaInBytes() == byteSize);

    code.setCalleeSaveRegisterAtOffsetList(
        WTF::move(calleeSaveRegisters),
        code.addStackSlot(byteSize, StackSlotKind::Locked));
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
