/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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
#include "FTLSaveRestore.h"

#if ENABLE(FTL_JIT)

#include "AssemblyHelpersSpoolers.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "Reg.h"
#include "RegisterAtOffsetList.h"
#include "RegisterSet.h"
#include "VM.h"
#include "VMLite.h"

namespace JSC {

// UNGIL U-T3 emitter (defined in jit/AssemblyHelpers.cpp). Self-declaration,
// mirroring that TU's own pattern — no header owns this form yet (the
// AssemblyHelpers.h member surface is macro-gated on
// JSC_ASSEMBLYHELPERS_HAS_LOAD_VMLITE, which the header-owning task defines).
void loadVMLite(AssemblyHelpers&, GPRReg);

namespace FTL {

static size_t NODELETE bytesForGPRs()
{
    return MacroAssembler::numberOfRegisters() * sizeof(int64_t);
}

static size_t NODELETE bytesForFPRs()
{
    // FIXME: It might be worthwhile saving the full state of the FP registers, at some point.
    // Right now we don't need this since we only do the save/restore just prior to OSR exit, and
    // OSR exit will be guaranteed to only need the double portion of the FP registers.
    return MacroAssembler::numberOfFPRegisters() * sizeof(double);
}

size_t requiredScratchMemorySizeInBytes()
{
    return bytesForGPRs() + bytesForFPRs();
}

size_t offsetOfGPR(GPRReg reg)
{
    return MacroAssembler::registerIndex(reg) * sizeof(int64_t);
}

size_t offsetOfFPR(FPRReg reg)
{
    return bytesForGPRs() + MacroAssembler::fpRegisterIndex(reg) * sizeof(double);
}

size_t offsetOfReg(Reg reg)
{
    if (reg.isGPR())
        return offsetOfGPR(reg.gpr());
    return offsetOfFPR(reg.fpr());
}

namespace {

struct Regs {
    Regs()
    {
        special = RegisterSet::stackRegisters();
        special.merge(RegisterSet::reservedHardwareRegisters());

        first = MacroAssembler::firstRegister();
        while (special.contains(first, IgnoreVectors))
            first = MacroAssembler::nextRegister(first);
    }

    GPRReg NODELETE nextRegister(GPRReg current)
    {
        auto next = MacroAssembler::nextRegister(current);
        for (; next <= MacroAssembler::lastRegister(); next = MacroAssembler::nextRegister(next)) {
            if (!special.contains(next, IgnoreVectors))
                break;
        }
        return next;
    }

    RegisterSet special;
    GPRReg first;
};

} // anonymous namespace

static void saveAllRegistersImpl(AssemblyHelpers& jit, const ScopedLambda<void(AssemblyHelpers&, GPRReg)>& materializeBase)
{
    Regs regs;

    // Get the first register out of the way, so that we can use it as a pointer.
    GPRReg baseGPR = regs.first;
#if CPU(ARM64)
    GPRReg nextGPR = regs.nextRegister(baseGPR);
    GPRReg firstToSaveGPR = regs.nextRegister(nextGPR);
    ASSERT(baseGPR == ARM64Registers::x0);
    ASSERT(nextGPR == ARM64Registers::x1);
#else
    GPRReg firstToSaveGPR = regs.nextRegister(baseGPR);
#endif
    jit.poke64(baseGPR, 0);
    materializeBase(jit, baseGPR);

    AssemblyHelpers::StoreRegSpooler spooler(jit, baseGPR);

    // Get all of the other GPRs out of the way.
    for (MacroAssembler::RegisterID reg = firstToSaveGPR; reg <= MacroAssembler::lastRegister(); reg = MacroAssembler::nextRegister(reg)) {
        if (regs.special.contains(reg, IgnoreVectors))
            continue;
        spooler.storeGPR({ reg, static_cast<ptrdiff_t>(offsetOfGPR(reg)), conservativeWidthWithoutVectors(reg) });
    }
    spooler.finalizeGPR();
    
    // Restore the first register into the second one and save it.
    jit.peek64(firstToSaveGPR, 0);
#if CPU(ARM64)
    jit.storePair64(firstToSaveGPR, nextGPR, baseGPR, AssemblyHelpers::TrustedImm32(offsetOfGPR(baseGPR)));
#else
    jit.store64(firstToSaveGPR, MacroAssembler::Address(baseGPR, offsetOfGPR(baseGPR)));
#endif
    
    // Finally save all FPR's.
    for (MacroAssembler::FPRegisterID reg = MacroAssembler::firstFPRegister(); reg <= MacroAssembler::lastFPRegister(); reg = MacroAssembler::nextFPRegister(reg)) {
        if (regs.special.contains(reg, IgnoreVectors))
            continue;
        spooler.storeFPR({ reg, static_cast<ptrdiff_t>(offsetOfFPR(reg)), conservativeWidthWithoutVectors(reg) });
    }
    spooler.finalizeFPR();
}

void saveAllRegisters(AssemblyHelpers& jit, char* scratchMemory)
{
    saveAllRegistersImpl(jit, scopedLambda<void(AssemblyHelpers&, GPRReg)>([&] (AssemblyHelpers& jit, GPRReg baseGPR) {
        jit.move(MacroAssembler::TrustedImmPtr(scratchMemory), baseGPR);
    }));
}

void saveAllRegisters(AssemblyHelpers& jit, const ScopedLambda<void(AssemblyHelpers&, GPRReg)>& materializeBase)
{
    saveAllRegistersImpl(jit, materializeBase);
}

static void restoreAllRegistersImpl(AssemblyHelpers& jit, const ScopedLambda<void(AssemblyHelpers&, GPRReg)>& materializeBase)
{
    Regs regs;

    // Give ourselves a pointer to the scratch memory.
    GPRReg baseGPR = regs.first;
    materializeBase(jit, baseGPR);

    AssemblyHelpers::LoadRegSpooler spooler(jit, baseGPR);

    // Restore all FPR's.
    for (MacroAssembler::FPRegisterID reg = MacroAssembler::firstFPRegister(); reg <= MacroAssembler::lastFPRegister(); reg = MacroAssembler::nextFPRegister(reg)) {
        if (regs.special.contains(reg, IgnoreVectors))
            continue;
        spooler.loadFPR({ reg, static_cast<ptrdiff_t>(offsetOfFPR(reg)), conservativeWidthWithoutVectors(reg) });
    }
    spooler.finalizeFPR();
    
#if CPU(ARM64)
    GPRReg nextGPR = regs.nextRegister(baseGPR);
    GPRReg firstToRestoreGPR = regs.nextRegister(nextGPR);
    ASSERT(baseGPR == ARM64Registers::x0);
    ASSERT(nextGPR == ARM64Registers::x1);
#else
    GPRReg firstToRestoreGPR = regs.nextRegister(baseGPR);
#endif
    for (MacroAssembler::RegisterID reg = firstToRestoreGPR; reg <= MacroAssembler::lastRegister(); reg = MacroAssembler::nextRegister(reg)) {
        if (regs.special.contains(reg, IgnoreVectors))
            continue;
        spooler.loadGPR({ reg, static_cast<ptrdiff_t>(offsetOfGPR(reg)), conservativeWidthWithoutVectors(reg) });
    }
    spooler.finalizeGPR();

#if CPU(ARM64)
    jit.loadPair64(baseGPR, AssemblyHelpers::TrustedImm32(offsetOfGPR(baseGPR)), baseGPR, nextGPR);
#else
    jit.load64(MacroAssembler::Address(baseGPR, offsetOfGPR(baseGPR)), baseGPR);
#endif
}

void restoreAllRegisters(AssemblyHelpers& jit, char* scratchMemory)
{
    restoreAllRegistersImpl(jit, scopedLambda<void(AssemblyHelpers&, GPRReg)>([&] (AssemblyHelpers& jit, GPRReg baseGPR) {
        jit.move(MacroAssembler::TrustedImmPtr(scratchMemory), baseGPR);
    }));
}

void restoreAllRegisters(AssemblyHelpers& jit, const ScopedLambda<void(AssemblyHelpers&, GPRReg)>& materializeBase)
{
    restoreAllRegistersImpl(jit, materializeBase);
}

// ---- UNGIL §A.1.6 (ANNEX A16) — U-T4b. See FTLSaveRestore.h for the
// contract (frozen addressing sequence; clobbers only `dest`; loads are
// address-dependent against the release-publishing install).

void materializeBakedScratchBufferPointer(AssemblyHelpers& jit, unsigned bakedIndex, GPRReg dest)
{
    ASSERT(bakedIndex < VMLite::maxScratchSegments * VMLite::scratchSegmentSize);
    loadVMLite(jit, dest);
    jit.loadPtr(
        MacroAssembler::Address(
            dest,
            static_cast<int32_t>(VMLite::offsetOfScratchSegments() + static_cast<ptrdiff_t>(bakedIndex >> VMLite::scratchSegmentShift) * sizeof(void*))),
        dest);
    jit.loadPtr(
        MacroAssembler::Address(
            dest,
            static_cast<int32_t>(static_cast<ptrdiff_t>(bakedIndex & (VMLite::scratchSegmentSize - 1)) * sizeof(void*))),
        dest);
}

void materializeBakedScratchBufferDataPointer(AssemblyHelpers& jit, unsigned bakedIndex, GPRReg dest)
{
    materializeBakedScratchBufferPointer(jit, bakedIndex, dest);
    jit.addPtr(MacroAssembler::TrustedImm32(static_cast<int32_t>(OBJECT_OFFSETOF(ScratchBuffer, m_buffer))), dest);
}

// gilOff twin of AssemblyHelpers::restoreCalleeSavesFromEntryFrameCalleeSavesBuffer
// (jit/AssemblyHelpers.cpp): the only delta is the topEntryFrame source —
// the CURRENT lite's per-thread slot instead of the baked &vm.topEntryFrame
// (UNGIL §A.1.3: topEntryFrame is Group-3 state, per-lite when gilOff).
void restoreCalleeSavesFromCurrentVMLiteEntryFrameCalleeSavesBuffer(AssemblyHelpers& jit)
{
#if NUMBER_OF_CALLEE_SAVES_REGISTERS > 0
    RegisterAtOffsetList* allCalleeSaves = RegisterSet::vmCalleeSaveRegisterOffsets();
    auto dontRestoreRegisters = RegisterSet::stackRegisters();
    unsigned registerCount = allCalleeSaves->registerCount();

    GPRReg scratch = InvalidGPRReg;
    unsigned scratchGPREntryIndex = 0;
#if CPU(ARM64)
    // We don't need a second scratch GPR, but we'll also defer restoring this
    // GPR (in the next slot after the scratch) so that we can restore them
    // together later using a loadPair64.
    GPRReg unusedNextSlotGPR = InvalidGPRReg;
#endif

    // Use the first GPR entry's register as our baseGPR.
    for (unsigned i = 0; i < registerCount; i++) {
        RegisterAtOffset entry = allCalleeSaves->at(i);
        if (dontRestoreRegisters.contains(entry.reg(), IgnoreVectors))
            continue;
        if (entry.reg().isGPR()) {
#if CPU(ARM64)
            if (i + 1 < registerCount) {
                RegisterAtOffset entry2 = allCalleeSaves->at(i + 1);
                if (!dontRestoreRegisters.contains(entry2.reg(), IgnoreVectors)
                    && entry2.reg().isGPR()
                    && entry2.offset() == entry.offset() + static_cast<ptrdiff_t>(sizeof(CPURegister))) {
                    scratchGPREntryIndex = i;
                    scratch = entry.reg().gpr();
                    unusedNextSlotGPR = entry2.reg().gpr();
                    break;
                }
            }
#else
            scratchGPREntryIndex = i;
            scratch = entry.reg().gpr();
            break;
#endif
        }
    }
    ASSERT(scratch != InvalidGPRReg);

    RegisterSet skipList;
    skipList.merge(dontRestoreRegisters);

    // Skip the scratch register(s). We'll restore them later.
    skipList.add(scratch, IgnoreVectors);
#if CPU(ARM64)
    RELEASE_ASSERT(unusedNextSlotGPR != InvalidGPRReg);
    skipList.add(unusedNextSlotGPR, IgnoreVectors);
#endif

    // The delta vs the VM-baked original: topEntryFrame from the CURRENT lite.
    loadVMLite(jit, scratch);
    jit.loadPtr(
        MacroAssembler::Address(
            scratch,
            static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_topEntryFrame())),
        scratch);
    jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBufferImpl(scratch, skipList);

    // Restore the callee save value of the scratch.
    RegisterAtOffset entry = allCalleeSaves->at(scratchGPREntryIndex);
    ASSERT(!dontRestoreRegisters.contains(entry.reg(), IgnoreVectors));
    ASSERT(entry.reg().isGPR());
    ASSERT(scratch == entry.reg().gpr());
#if CPU(ARM64)
    RegisterAtOffset entry2 = allCalleeSaves->at(scratchGPREntryIndex + 1);
    ASSERT_UNUSED(entry2, !dontRestoreRegisters.contains(entry2.reg(), IgnoreVectors));
    ASSERT(entry2.reg().isGPR());
    ASSERT(unusedNextSlotGPR == entry2.reg().gpr());
    jit.loadPair64(scratch, AssemblyHelpers::TrustedImm32(entry.offset()), scratch, unusedNextSlotGPR);
#else
    jit.load64(MacroAssembler::Address(scratch, entry.offset()), scratch);
#endif
#else
    UNUSED_PARAM(jit);
#endif // NUMBER_OF_CALLEE_SAVES_REGISTERS > 0
}

} } // namespace JSC::FTL

#endif // ENABLE(FTL_JIT)

