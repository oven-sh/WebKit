/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"
#include "RegisterAtOffsetList.h"

namespace JSC { namespace AOT { namespace Machine {

enum class FrameCoverage : uint8_t {
    None,
    WholeFunction,
    RareRegions,
    FramedPart,
};

class Frame {
    WTF_MAKE_NONCOPYABLE(Frame);
public:
    FrameCoverage coverage() const { return m_coverage; }
    bool hasFrame() const { return m_coverage != FrameCoverage::None; }
    bool hasFrameRecord() const { return m_hasFrameRecord; }
    bool savesLieAtBottom() const { return m_savesLieAtBottom; }
    int32_t topOfSlots() const { return m_topOfSlots; }
    bool nothingCountsOnStackCheck() const { return m_nothingCountsOnStackCheck; }
    void setHasFrameRecordForTesting(bool hasFrameRecord) { m_hasFrameRecord = hasFrameRecord; }

    unsigned bytesBelowFrameRecord() const { return m_bytesBelowFrameRecord; }
    unsigned frameSize() const { return m_bytesBelowFrameRecord; }
    unsigned bytesDeadAtSuspensions() const { return m_bytesDeadAtSuspensions; }
    int32_t beginOfWordsClearedAtSetup() const { return m_beginOfWordsClearedAtSetup; }
    int32_t endOfWordsClearedAtSetup() const { return m_endOfWordsClearedAtSetup; }

    RegisterMask calleeSavesMask() const { return m_calleeSaves; }
    RegisterSet calleeSaveRegisters() const { return registerSetOf(m_calleeSaves); }
    const RegisterAtOffsetList& calleeSaveRegisterAtOffsetList() const LIFETIME_BOUND { return m_calleeSaveOffsets; }
    unsigned numberOfCalleeSaves() const { return static_cast<unsigned>(m_calleeSaveOffsets.registerCount()); }
    RegisterSet calleeSavesClobberedAtSuspensions() const { return registerSetOf(m_calleeSavesClobberedAtSuspensions); }

    JS_EXPORT_PRIVATE bool isPinned(Reg) const;

    int32_t offsetFromFPOf(SpillIndex spill) const { return m_offsetOfSpill[spill]; }
    void setOffsetFromFPForTesting(SpillIndex spill, int32_t offset) { m_offsetOfSpill[spill] = offset; }

    bool holdsStepsOf(const Block* block) const { return m_blocksInFrame.get(block->index()); }
    bool isSetUpAtHeadOf(const Block* block) const { return m_blocksThatSetUpFrame.get(block->index()); }
    bool isTornDownAtEndOf(const Block* block) const { return m_blocksThatTearDownFrame.get(block->index()); }
    bool returnsInPlaceOfJumpAtEndOf(const Block* block) const { return m_blocksThatReturnInPlaceOfJump.get(block->index()); }
    void setHoldsStepsForTesting(const Block* block, bool holds) { holds ? m_blocksInFrame.set(block->index()) : m_blocksInFrame.clear(block->index()); }

    MacroAssembler::Address addressInFrame(intptr_t offsetFromFP) const
    {
        ASSERT(hasFrame());
        intptr_t offsetFromSP = offsetFromFP + static_cast<intptr_t>(m_bytesBelowFrameRecord);
#if CPU(X86_64)
        if (m_hasFrameRecord && (isRepresentableAs<int8_t>(static_cast<int64_t>(offsetFromFP)) || !isRepresentableAs<int8_t>(static_cast<int64_t>(offsetFromSP))))
            return MacroAssembler::Address(MacroAssembler::framePointerRegister, safeCast<int32_t>(offsetFromFP));
#endif
        return MacroAssembler::Address(MacroAssembler::stackPointerRegister, safeCast<int32_t>(offsetFromSP));
    }

    JS_EXPORT_PRIVATE void dump(PrintStream&) const;

private:
    friend class Arena;
    friend Frame* planFrame(Procedure&);

    Frame() = default;

    FrameCoverage m_coverage { FrameCoverage::None };
    bool m_nothingCountsOnStackCheck { false };
    bool m_hasFrameRecord { true };
    bool m_savesLieAtBottom { false };
    int32_t m_topOfSlots { 0 };
    unsigned m_bytesBelowFrameRecord { 0 };
    unsigned m_bytesDeadAtSuspensions { 0 };
    RegisterMask m_calleeSaves { 0 };
    RegisterMask m_calleeSavesClobberedAtSuspensions { 0 };
    RegisterMask m_mutableRegisters { 0 };
    RegisterAtOffsetList m_calleeSaveOffsets;
    std::span<int32_t> m_offsetOfSpill;
    int32_t m_beginOfWordsClearedAtSetup { 0 };
    int32_t m_endOfWordsClearedAtSetup { 0 };
    BitSpan m_blocksInFrame;
    BitSpan m_blocksThatSetUpFrame;
    BitSpan m_blocksThatTearDownFrame;
    BitSpan m_blocksThatReturnInPlaceOfJump;

    friend void emitFrameSetup(CCallHelpers&, const Procedure&);
};

Frame* planFrame(Procedure&);

bool plannedBlockOnlyReturns(const Procedure&, const Block*);

JS_EXPORT_PRIVATE void emitFrameSetup(CCallHelpers&, const Procedure&);
JS_EXPORT_PRIVATE void emitFrameTeardown(CCallHelpers&, const Frame&);
JS_EXPORT_PRIVATE void emitCalleeSaveRestore(CCallHelpers&, const Frame&);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
