/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameMapEmitter.h"

#if ENABLE(AOT)

#include "AOTCodeRecords.h"
#include "AOTSavedFrame.h"
#include "AOTSavedFrameMap.h"
#include "AOTStubRegisters.h"
#include "AirCode.h"
#include "AirLimits.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include "GPRInfo.h"
#include "RegisterAtOffsetList.h"

namespace JSC { namespace AOT { namespace Typed {

// A root lies in a word of the frame, or two narrow ones in one. Nothing else about a map depends on how many there are.
static_assert(defaultLimitValue(LimitID::MaxFrameMapRoots, hostArchitecture()) == SavedFrame::maxNumberOfWords);
static_assert(defaultLimitValue(LimitID::MaxFrameMapWords, hostArchitecture()) == 1u << SavedFrame::liveSlotMapOffsetBits);
static_assert(B3::AirLimits::maxWordsClearedAtFirstSuspension == SavedFrame::maxClearedWords);

static RegisterSet registersClobberedBySuspension(const B3::Air::Code& code)
{
    RegisterSet clobbered = RegisterSet::allScalarRegisters();
    clobbered.filter(code.mutableRegs());
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        clobbered.remove(SavedFrame::preservedRegister);
    return clobbered;
}

FrameMapEmitter::Roots FrameMapEmitter::appendRoots(B3::PatchpointValue* patchpoint, const ValueContext& suspension)
{
    const SuspensionRoots& roots = m_generator.inputs().frameMaps.rootsAt(suspension.id());
    Roots result { m_proc.resultCount(patchpoint->type()) + patchpoint->numChildren(), { } };
    auto append = [&](Operand operand, FrameSlotKind kind) {
        RELEASE_ASSERT(frameSlotKindOf(operand.representation) == kind && operand.value->type() == b3TypeOf(operand.representation));
        patchpoint->append(B3::ConstrainedValue(operand.value, B3::ValueRep::LateColdAny));
        result.kinds.append(kind);
    };
    for (const FrameRoot& root : roots.roots)
        append(m_generator.getAsDefined(root.value), root.kind);
    for (const FrameResultRoot& root : roots.resultRoots)
        append(m_generator.resultAt(root.producer, root.index), root.kind);
    patchpoint->clobberLate(registersClobberedBySuspension(m_proc.code()));
    patchpoint->setIsSuspension(true);
    m_records.frame.savesFrameOnSuspend = true;
    return result;
}

static bool isFoldedToConstant(const B3::Air::Code& code, const B3::ValueRep& rep)
{
    if (rep.isConstant())
        return true;
    // Compiled code on x86-64 allocates notCellMaskRegister, so only a pinned register holds a constant.
    if (!rep.isGPR() || !code.isPinned(rep.gpr()))
        return false;
    return rep.gpr() == GPRInfo::numberTagRegister || rep.gpr() == GPRInfo::notCellMaskRegister;
}

static std::optional<uint32_t> locationOf(const B3::ValueRep& rep, FrameSlotKind kind)
{
    if (rep.isGPR() && rep.gpr() == SavedFrame::preservedRegister)
        return SavedFrame::locationInRegister;
    if (!rep.isStack())
        return std::nullopt;
    intptr_t offset = rep.offsetFromFP();
    if (offset >= 0)
        return std::nullopt;
    switch (kind) {
    case FrameSlotKind::Value:
    case FrameSlotKind::Cell:
        if (offset % static_cast<intptr_t>(sizeof(uint64_t)))
            return std::nullopt;
        return SavedFrame::locationInFrame(offset);
    case FrameSlotKind::NarrowReference:
        if (offset % static_cast<intptr_t>(sizeof(uint32_t)))
            return std::nullopt;
        return SavedFrame::narrowLocationInFrame(offset);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool storesCalleeSavesBelowFrameRecord(const B3::Air::FrameLayout& layout)
{
    if (!layout.isPlanned || layout.slotArea != B3::Air::SlotArea::BelowFrameRecord)
        return false;
    switch (layout.calleeSaveForm) {
    case B3::Air::CalleeSaveForm::Stored:
    case B3::Air::CalleeSaveForm::PushedBelowFrameRecord:
        return true;
    case B3::Air::CalleeSaveForm::PushedAtFrameBottom:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<uint32_t> savedRegistersWord(const B3::Air::Code& code)
{
    const B3::Air::FrameLayout& layout = code.frameLayout();
    if (!storesCalleeSavesBelowFrameRecord(layout))
        return std::nullopt;
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    size_t count = saves.registerCount();
#if CPU(ARM64)
    constexpr size_t maxCount = suspensionClobberedGPRs.size();
    static_assert(!(maxCount % 2) && !((maxCount / 2) >> SavedFrame::savedRegisterBits));
    if (count != layout.numberOfSavesClobberedAtSuspensions || count % 2)
        return std::nullopt;
    return layout.numberOfSavesClobberedAtSuspensions / 2;
#elif CPU(X86_64)
    static_assert(suspensionClobberedGPRs.size() == 1 && suspensionClobberedGPRs[0] == X86Registers::r15);
    uint32_t word = 0;
    for (size_t i = 0; i < count; ++i) {
        Reg reg = saves.at(i).reg();
        if (reg == Reg(SavedFrame::preservedRegister) && !i)
            word |= SavedFrame::functionSavesPreservedRegister;
        else if (reg == Reg(suspensionClobberedGPRs[0]) && i + 1 == count)
            word |= SavedFrame::functionSavesR15;
        else
            return std::nullopt;
    }
    return word >> SavedFrame::liveSlotMapOffsetBits;
#else
    if (count)
        return std::nullopt;
    return 0u;
#endif
}

// The function owns the register, and nothing reads what it holds here. Until the function's first store to it that is what the caller left in it.
// The suspension would keep that value, and the resumption would put it back into the register.
bool FrameMapEmitter::savesDeadPreservedRegister(const B3::StackmapGenerationParams& params)
{
    if constexpr (SavedFrame::preservedRegister == InvalidGPRReg)
        return false;
    else {
        constexpr Reg reg { SavedFrame::preservedRegister };
        if (!params.code().calleeSaveRegisters().contains(reg, IgnoreVectors) || params.usedRegisters().contains(reg, IgnoreVectors))
            return false;
        for (unsigned i = 0; i < params.size(); ++i) {
            if (params[i].isGPR() && params[i].gpr() == SavedFrame::preservedRegister)
                return false;
        }
        return true;
    }
}

void FrameMapEmitter::releaseSavedRegisters(CCallHelpers& jit, const B3::StackmapGenerationParams& params)
{
    const B3::Air::Code& code = params.code();
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    RegisterSet inUse = params.usedRegisters();
    for (unsigned i = 0; i < params.size(); ++i) {
        if (params[i].isReg())
            inUse.add(params[i].reg(), IgnoreVectors);
    }
    auto isReloaded = [&](size_t index) {
        if (index >= saves.registerCount())
            return false;
        Reg reg = saves.at(index).reg();
        return code.calleeSavesClobberedAtSuspensions().contains(reg, IgnoreVectors) && !inUse.contains(reg, IgnoreVectors);
    };
    for (size_t index = 0; index < saves.registerCount(); ++index) {
        if (!isReloaded(index))
            continue;
        CCallHelpers::Address address = code.addressInFrame(saves.at(index).offset());
#if CPU(ARM64)
        if (isReloaded(index + 1) && saves.at(index + 1).offset() == saves.at(index).offset() + static_cast<ptrdiff_t>(sizeof(CPURegister))) {
            jit.loadPair64(address.base, CCallHelpers::TrustedImm32(address.offset), saves.at(index).reg().gpr(), saves.at(index + 1).reg().gpr());
            ++index;
            continue;
        }
#endif
        jit.loadPtr(address, saves.at(index).reg().gpr());
    }
    if (savesDeadPreservedRegister(params))
        jit.move(CCallHelpers::TrustedImm32(0), SavedFrame::preservedRegister);
}

std::optional<uint64_t> FrameMapEmitter::siteWordFor(FrameMapRecords& records, const B3::StackmapGenerationParams& params, const Roots& roots, unsigned promiseChild)
{
    SavedFrameMapBuilder builder;
    for (unsigned i = 0; i < roots.kinds.size(); ++i) {
        const B3::ValueRep& rep = params[roots.firstChild + i];
        if (isFoldedToConstant(params.code(), rep))
            continue;
        std::optional<uint32_t> location = locationOf(rep, roots.kinds[i]);
        if (!location)
            return std::nullopt;
        switch (roots.kinds[i]) {
        case FrameSlotKind::Value:
        case FrameSlotKind::Cell:
            builder.addValue(*location);
            break;
        case FrameSlotKind::NarrowReference:
            builder.addNarrowReference(*location);
            break;
        }
    }
    const B3::Air::Code& code = params.code();
    std::optional<uint32_t> promiseLocation = locationOf(params[promiseChild], FrameSlotKind::Cell);
    std::optional<uint32_t> savedRegisters = savedRegistersWord(code);
    if (!promiseLocation || !savedRegisters)
        return std::nullopt;
    uint32_t deadWordsAtBottom = static_cast<uint32_t>(code.frameLayout().bytesDeadAtSuspensions / sizeof(uint64_t));
    size_t numberOfWordsInFrame = code.bytesBelowFrameRecord() / sizeof(uint64_t);
    if (const std::optional<BitVector>& deadWords = params.value()->frameWordsDeadAcrossSuspension()) {
        builder.setListsEveryLiveWord();
        uint32_t lastLocationKept = static_cast<uint32_t>(numberOfWordsInFrame) - SavedFrame::numberOfWordsSkippedFor(deadWordsAtBottom);
        // Every restore writes the save area anew.
        uint32_t numberOfSaves = static_cast<uint32_t>(code.calleeSaveRegisterAtOffsetList().registerCount());
        RELEASE_ASSERT(numberOfSaves >= SavedFrame::skippedAtTopFor(*savedRegisters));
        for (uint32_t location = numberOfSaves + 1; location <= lastLocationKept; ++location) {
            bool holdsReference = builder.listsReferenceIn(location);
            RELEASE_ASSERT(!holdsReference || !deadWords->get(location));
            if (!holdsReference && !deadWords->get(location))
                builder.addOtherLiveWord(location);
        }
        if constexpr (SavedFrame::preservedRegister != InvalidGPRReg) {
            bool isLive = *promiseLocation == SavedFrame::locationInRegister || params.usedRegisters().contains(SavedFrame::preservedRegister, IgnoreVectors);
            if (isLive && !builder.listsReferenceIn(SavedFrame::locationInRegister))
                builder.addOtherLiveWord(SavedFrame::locationInRegister);
        }
    }
    return SavedFrame::siteWord(records.addMap(builder.finish().span()), *promiseLocation, *savedRegisters, deadWordsAtBottom, numberOfWordsInFrame, params.value()->numberOfFrameWordsToClearAtFirstSuspension());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
