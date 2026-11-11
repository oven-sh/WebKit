/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFrame.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"

namespace JSC { namespace AOT { namespace Machine {

static_assert(2 * sizeof(CPURegister) == stackAlignmentBytes(), "pushing the frame record keeps the stack pointer aligned");

static bool calleeSavesFormPair(const RegisterAtOffset& lower, const RegisterAtOffset& upper)
{
    return lower.reg().isGPR() == upper.reg().isGPR()
        && upper.offset() == lower.offset() + static_cast<ptrdiff_t>(sizeof(CPURegister));
}

template<typename PairFunctor, typename SingleFunctor>
static void forEachCalleeSaveAccess(const Frame& frame, const PairFunctor& accessPair, const SingleFunctor& accessOne)
{
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    size_t count = saves.registerCount();
    for (size_t index = 0; index < count; ++index) {
        const RegisterAtOffset& save = saves.at(index);
        ASSERT(save.width() == Width64);
        CCallHelpers::Address address = frame.addressInFrame(save.offset());
        if (index + 1 < count && calleeSavesFormPair(save, saves.at(index + 1))) {
            accessPair(save.reg(), saves.at(index + 1).reg(), address);
            ++index;
            continue;
        }
        accessOne(save.reg(), address);
    }
}

static bool frameHoldsNothingButSavedGPRs(const Frame& frame)
{
    constexpr size_t wordSize = sizeof(CPURegister);
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    size_t count = saves.registerCount();
    if (!count || frame.bytesBelowFrameRecord() != (count + count % 2) * wordSize)
        return false;
    for (size_t index = 0; index < count; ++index) {
        const RegisterAtOffset& save = saves.at(index);
        if (!save.reg().isGPR() || save.offset() != -static_cast<ptrdiff_t>((count - index) * wordSize))
            return false;
    }
    return true;
}

template<typename PairFunctor>
static void forEachPairOfSavedGPRs(const Frame& frame, bool fromRecordDown, const PairFunctor& accessPair)
{
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    size_t count = saves.registerCount();
    size_t numberOfPairs = (count + 1) / 2;
    for (size_t step = 0; step < numberOfPairs; ++step) {
        size_t pair = fromRecordDown ? numberOfPairs - 1 - step : step;
        size_t upper = 2 * pair + 1 - count % 2;
        accessPair(upper ? saves.at(upper - 1).reg().gpr() : ARM64Registers::zr, saves.at(upper).reg().gpr());
    }
}

static void clearFrameWordsAtSetup(CCallHelpers& jit, const Frame& frame, int32_t begin, int32_t end)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    ASSERT(begin <= end && end <= 0);
    ASSERT(!(begin % wordSize) && !(end % wordSize));
    ASSERT(begin == end || -static_cast<int64_t>(begin) <= static_cast<int64_t>(frame.bytesBelowFrameRecord()));

    int32_t offset = begin;
    for (; offset + 2 * wordSize <= end; offset += 2 * wordSize)
        jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, frame.addressInFrame(offset));
    if (offset < end)
        jit.store64(ARM64Registers::zr, frame.addressInFrame(offset));
}

void emitFrameSetup(CCallHelpers& jit, const Procedure& procedure)
{
    const Frame& frame = procedure.frame();
    ASSERT(frame.hasFrame());
    unsigned bytesBelowFrameRecord = frame.bytesBelowFrameRecord();
    ASSERT(!(bytesBelowFrameRecord % stackAlignmentBytes()));

    AllowMacroScratchRegisterUsage allowScratch(jit);

    jit.tagReturnAddress();
    jit.pushPair(CCallHelpers::framePointerRegister, CCallHelpers::linkRegister);
    jit.move(CCallHelpers::stackPointerRegister, CCallHelpers::framePointerRegister);

    bool allocatesBeforeCheck = bytesBelowFrameRecord <= maxBytesAllocatedBeforeStackCheck;
    bool pushesSaves = allocatesBeforeCheck && frameHoldsNothingButSavedGPRs(frame);
    auto allocate = [&] {
        if (pushesSaves) {
            forEachPairOfSavedGPRs(frame, true, [&](GPRReg lower, GPRReg upper) {
                jit.pushPair(lower, upper);
            });
            return;
        }
        if (bytesBelowFrameRecord)
            jit.sub64(CCallHelpers::TrustedImm32(safeCast<int32_t>(bytesBelowFrameRecord)), CCallHelpers::stackPointerRegister);
    };

    if (allocatesBeforeCheck)
        allocate();
    if (StackCheckGenerator generator = procedure.stackCheckGenerator())
        generator(procedure.stackCheckContext(), jit, frame, StackCheckSite { bytesBelowFrameRecord, allocatesBeforeCheck, frame.m_nothingCountsOnStackCheck });
    if (!allocatesBeforeCheck)
        allocate();
    if (pushesSaves) {
        ASSERT(frame.m_beginOfWordsClearedAtSetup == frame.m_endOfWordsClearedAtSetup);
        return;
    }

    forEachCalleeSaveAccess(frame, [&](Reg lower, Reg upper, CCallHelpers::Address address) {
        if (lower.isGPR())
            jit.storePair64(lower.gpr(), upper.gpr(), address);
        else
            jit.storePair64(lower.fpr(), upper.fpr(), address.base, CCallHelpers::TrustedImm32(address.offset));
    }, [&](Reg reg, CCallHelpers::Address address) {
        if (reg.isGPR())
            jit.store64(reg.gpr(), address);
        else
            jit.storeDouble(reg.fpr(), address);
    });

    clearFrameWordsAtSetup(jit, frame, frame.m_beginOfWordsClearedAtSetup, frame.m_endOfWordsClearedAtSetup);
}

void emitCalleeSaveRestore(CCallHelpers& jit, const Frame& frame)
{
    ASSERT(frame.hasFrame());

    AllowMacroScratchRegisterUsage allowScratch(jit);

    forEachCalleeSaveAccess(frame, [&](Reg lower, Reg upper, CCallHelpers::Address address) {
        if (lower.isGPR())
            jit.loadPair64(address, lower.gpr(), upper.gpr());
        else
            jit.loadPair64(address.base, CCallHelpers::TrustedImm32(address.offset), lower.fpr(), upper.fpr());
    }, [&](Reg reg, CCallHelpers::Address address) {
        if (reg.isGPR())
            jit.load64(address, reg.gpr());
        else
            jit.loadDouble(address, reg.fpr());
    });
}

void emitFrameTeardown(CCallHelpers& jit, const Frame& frame)
{
    if (frameHoldsNothingButSavedGPRs(frame)) {
        forEachPairOfSavedGPRs(frame, false, [&](GPRReg lower, GPRReg upper) {
            jit.popPair(lower, upper);
        });
    } else {
        emitCalleeSaveRestore(jit, frame);
        if (frame.bytesBelowFrameRecord())
            jit.move(CCallHelpers::framePointerRegister, CCallHelpers::stackPointerRegister);
    }
    jit.popPair(CCallHelpers::framePointerRegister, CCallHelpers::linkRegister);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
