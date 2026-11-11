/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFrameMapEmitter.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTCodeRecords.h"
#include "AOTHeldOperands.h"
#include "AOTLimits.h"
#include "AOTMachineFrame.h"
#include "AOTMachineTarget.h"
#include "AOTResultRules.h"
#include "AOTSavedFrame.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSavedFrameMap.h"
#include "AOTStubRegisters.h"
#include "AOTSweep.h"
#include "CCallHelpers.h"
#include "GPRInfo.h"
#include "RegisterAtOffsetList.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxFrameMapRoots, hostArchitecture()) == SavedFrame::maxNumberOfWords);
static_assert(defaultLimitValue(LimitID::MaxFrameMapWords, hostArchitecture()) == 1u << SavedFrame::liveSlotMapOffsetBits);

static Machine::RegisterMask registersClobberedBySuspension(const Machine::ProcedureTraits& traits)
{
    Machine::RegisterFile file = Machine::registerFileFor(traits);
    Machine::RegisterMask clobbered = file.allocatableIn(Machine::Bank::GP) | file.allocatableIn(Machine::Bank::FP);
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        clobbered &= ~Machine::maskOf(Reg(SavedFrame::preservedRegister));
    return clobbered;
}

static bool rootPointsIntoFrame(const Machine::Operation* root)
{
    if (root->opcode() == Machine::FramePointer || root->opcode() == Machine::SlotBase)
        return true;
    return std::ranges::any_of(root->children(), rootPointsIntoFrame);
}

FrameMapEmitter::Roots FrameMapEmitter::appendRoots(Machine::PatchOperation* patch, const ValueContext& suspension)
{
    if (!suspension->traits().maySuspend() || !m_proc.traits().savesFrameOnSuspend)
        fail(suspension, InternalErrorKind::InvalidSuspension);

    Roots result { m_proc.resultCount(patch->type()) + patch->numChildren(), { } };
    auto append = [&](Operand operand) {
        std::optional<FrameSlotKind> kind = frameSlotKindOf(operand.representation);
        if (!kind)
            return;
        RELEASE_ASSERT(operand.value && operand.value->type() == machineTypeOf(operand.representation));
        if (operand.value->hasNoPosition()) {
            if (rootPointsIntoFrame(operand.value))
                fail(suspension, InternalErrorKind::FramePointerAcrossSuspension);
            return;
        }
        patch->append(operand.value, Machine::ValueRep::LateColdAny);
        result.kinds.append(*kind);
    };
    auto numberOfResultsDefinedBy = [&](const Operation& producer) -> std::optional<uint32_t> {
        if (producer.opcode != Opcode::CallDirect)
            return numberOfResultsOf(m_program, producer);
        unsigned numberOfResults = conventions().numberOfResults(conventions().unitEnteredBy(producer));
        return numberOfResults > 1 ? std::optional<uint32_t>(numberOfResults) : std::nullopt;
    };
    SavedFrameEmitter& frames = emitter<SavedFrameEmitter>();
    auto addRoot = [&](ValueID id) {
        if (!id || frames.hasWordInMachineFrame(id) || id == suspension.id() || m_code.isConstant(id) || isDeferredClosure(id))
            return;
        if (typeOf(id).isOpaque(OpaqueKind::StringBuilder) && !stringBuilderIsCellOfHeap(m_code, id)) {
            fail(suspension, InternalErrorKind::FramePointerAcrossSuspension);
            return;
        }
        if (std::optional<uint32_t> numberOfResults = numberOfResultsDefinedBy(operationAt(id))) {
            for (uint32_t index = 0; index < *numberOfResults; ++index)
                append(generator().resultAt(id, index));
            return;
        }
        if (frameSlotKindOf(representationOf(id)))
            append(get(id));
    };

    if (frames.keepsWordsInMachineFrame()) {
        result.slotOfKeptWords = frames.slotOfKeptWords();
        result.numberOfTracedWordsInSlot = frames.numberOfTracedWords();
    }
    std::span<const ValueID> live = m_code.liveAt(suspension.id());
    for (ValueID id : live)
        addRoot(id);
    for (ValueID held : heldOperandsOf(suspension.value())) {
        if (!std::ranges::binary_search(live, held))
            addRoot(held);
    }
    if (uint64_t numberOfRoots = result.kinds.size(); numberOfRoots > build().limit(LimitID::MaxFrameMapRoots))
        generator().failAtLimit(suspension, LimitID::MaxFrameMapRoots, numberOfRoots);

    patch->clobberLate(registersClobberedBySuspension(m_proc.traits()));
    patch->setIsSuspension(true);
    m_records.frame.savesFrameOnSuspend = true;
    return result;
}

static bool isRootFoldedToConstant(const Machine::Frame& frame, const Machine::ValueRep& rep)
{
    if (rep.isConstant())
        return true;
    if (!rep.isGPR() || !frame.isPinned(rep.reg()))
        return false;
    return rep.gpr() == GPRInfo::numberTagRegister || rep.gpr() == GPRInfo::notCellMaskRegister;
}

static std::optional<uint32_t> locationInSavedFrame(const Machine::ValueRep& rep, FrameSlotKind kind)
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

static std::optional<uint32_t> savedRegistersWordOf(const Machine::Frame& frame)
{
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    size_t count = saves.registerCount();
    for (size_t index = 0; index < count; ++index) {
        if (saves.at(index).offset() != -static_cast<ptrdiff_t>((count - index) * sizeof(CPURegister)))
            return std::nullopt;
    }
#if CPU(ARM64)
    constexpr size_t maxCount = suspensionClobberedGPRs.size();
    static_assert(!(maxCount % 2) && !((maxCount / 2) >> SavedFrame::savedRegisterBits));
    if (count > maxCount || count % 2)
        return std::nullopt;
    for (size_t index = 0; index < count; ++index) {
        if (saves.at(index).reg() != Reg(suspensionClobberedGPRs[maxCount - count + index]))
            return std::nullopt;
    }
    return static_cast<uint32_t>(count / 2);
#elif CPU(X86_64)
    static_assert(suspensionClobberedGPRs.size() == 1 && suspensionClobberedGPRs[0] == X86Registers::r15);
    uint32_t word = 0;
    for (size_t index = 0; index < count; ++index) {
        Reg reg = saves.at(index).reg();
        if (reg == Reg(SavedFrame::preservedRegister) && !index)
            word |= SavedFrame::functionSavesPreservedRegister;
        else if (reg == Reg(suspensionClobberedGPRs[0]) && index + 1 == count)
            word |= SavedFrame::functionSavesR15;
        else
            return std::nullopt;
    }
    return word >> SavedFrame::liveSlotMapOffsetBits;
#endif
}

void FrameMapEmitter::releaseSavedRegisters(CCallHelpers& jit, const Machine::PatchParameters& params)
{
    const Machine::Frame& frame = params.frame();
    const RegisterAtOffsetList& saves = frame.calleeSaveRegisterAtOffsetList();
    RegisterSet clobberedAtSuspensions = frame.calleeSavesClobberedAtSuspensions();
    RegisterSet inUse = params.usedRegisters();
    for (const Machine::ValueRep& rep : params) {
        if (rep.isReg())
            inUse.add(rep.reg(), IgnoreVectors);
    }
    auto isReloaded = [&](size_t index) {
        if (index >= saves.registerCount())
            return false;
        Reg reg = saves.at(index).reg();
        return clobberedAtSuspensions.contains(reg, IgnoreVectors) && !inUse.contains(reg, IgnoreVectors);
    };
    for (size_t index = 0; index < saves.registerCount(); ++index) {
        if (!isReloaded(index))
            continue;
        CCallHelpers::Address address = frame.addressInFrame(saves.at(index).offset());
#if CPU(ARM64)
        if (isReloaded(index + 1) && saves.at(index + 1).offset() == saves.at(index).offset() + static_cast<ptrdiff_t>(sizeof(CPURegister))) {
            jit.loadPair64(address.base, CCallHelpers::TrustedImm32(address.offset), saves.at(index).reg().gpr(), saves.at(index + 1).reg().gpr());
            ++index;
            continue;
        }
#endif
        jit.loadPtr(address, saves.at(index).reg().gpr());
    }
}

std::optional<uint64_t> FrameMapEmitter::siteWordOfFrameOnHeap(const Machine::PatchParameters& params)
{
    std::optional<uint32_t> savedRegisters = savedRegistersWordOf(params.frame());
    if (!savedRegisters)
        return std::nullopt;
    return SavedFrame::siteWord(0, 0, *savedRegisters);
}

std::optional<uint64_t> FrameMapEmitter::siteWordFor(FrameMapRecords& records, const Machine::PatchParameters& params, const Roots& roots, unsigned promiseChild)
{
    const Machine::Frame& frame = params.frame();
    std::optional<uint32_t> savedRegisters = savedRegistersWordOf(frame);
    if (!savedRegisters)
        return std::nullopt;

    uint32_t numberOfSaves = frame.numberOfCalleeSaves();
    uint32_t deadWordsAtBottom = static_cast<uint32_t>(frame.bytesDeadAtSuspensions() / sizeof(uint64_t));
    size_t numberOfWordsInFrame = frame.bytesBelowFrameRecord() / sizeof(uint64_t);
    RELEASE_ASSERT(numberOfSaves >= SavedFrame::skippedAtTopFor(*savedRegisters) && deadWordsAtBottom <= numberOfWordsInFrame);
    uint32_t lastLocationKept = static_cast<uint32_t>(numberOfWordsInFrame) - SavedFrame::numberOfWordsSkippedFor(deadWordsAtBottom);
    auto isKeptForFunction = [&](uint32_t location) {
        return location == SavedFrame::locationInRegister || (location > numberOfSaves && location <= lastLocationKept);
    };

    SavedFrameMapBuilder builder;
    for (unsigned i = 0; i < roots.kinds.size(); ++i) {
        const Machine::ValueRep& rep = params[roots.firstChild + i];
        if (isRootFoldedToConstant(frame, rep))
            continue;
        std::optional<uint32_t> location = locationInSavedFrame(rep, roots.kinds[i]);
        if (!location)
            return std::nullopt;
        switch (roots.kinds[i]) {
        case FrameSlotKind::Value:
        case FrameSlotKind::Cell:
            if (!isKeptForFunction(*location))
                return std::nullopt;
            builder.addValue(*location);
            break;
        case FrameSlotKind::NarrowReference:
            if (!isKeptForFunction(SavedFrame::locationHolding(*location)))
                return std::nullopt;
            builder.addNarrowReference(*location);
            break;
        }
    }
    for (uint32_t word = 0; roots.slotOfKeptWords && word < roots.numberOfTracedWordsInSlot; ++word) {
        uint32_t location = SavedFrame::locationInFrame(roots.slotOfKeptWords->offsetFromFP() + static_cast<intptr_t>(word * sizeof(uint64_t)));
        if (!isKeptForFunction(location))
            return std::nullopt;
        builder.addValue(location);
    }
    std::optional<uint32_t> promiseLocation = locationInSavedFrame(params[promiseChild], FrameSlotKind::Cell);
    if (!promiseLocation || !isKeptForFunction(*promiseLocation))
        return std::nullopt;

    if (const std::optional<Machine::BitSpan>& deadWords = params.value()->frameWordsDeadAcrossSuspension()) {
        for (uint32_t location = numberOfSaves + 1; location <= lastLocationKept; ++location)
            RELEASE_ASSERT(!builder.listsReferenceIn(location) || !deadWords->get(location));
    }
    return SavedFrame::siteWord(records.addMap(builder.finish().span()), *promiseLocation, *savedRegisters, deadWordsAtBottom, numberOfWordsInFrame);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
