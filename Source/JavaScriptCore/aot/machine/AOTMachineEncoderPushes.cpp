/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AOTMachineFrameForms.h"
#include "AOTMachineTarget.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

namespace {

struct StoreBehindSetup {
    SpillIndex spill;
    GPRReg source;
};

} // anonymous namespace

static void noteStoreBehindSetup(Encoder& encoder, ArenaVector<StoreBehindSetup>& stores, SpillIndex spill, Reg source)
{
    const SpillFacts& facts = encoder.m_allocation.spills()[spill];
    if (facts.width == Width64 && !facts.isPointerBesideNarrowReference && source.isGPR())
        stores.append(encoder.m_arena, StoreBehindSetup { spill, source.gpr() });
}

static void findSavesBehindSetupOfHead(Encoder& encoder, const Block* block, ArenaVector<StoreBehindSetup>& stores)
{
    const Allocation::HeadOfFramedPart& head = encoder.m_allocation.headOfFramedPartAbove(block);
    for (uint32_t index = head.beginOfSteps; index < head.endOfSteps; ++index) {
        const Step& step = encoder.m_allocation.step(index);
        RELEASE_ASSERT(step.kind == StepKind::Save && step.destination.isSpill() && step.sources[0].isRegister());
        RELEASE_ASSERT(std::ranges::find(stores.span(), step.destination.spillIndex(), &StoreBehindSetup::spill) == stores.span().end());
        noteStoreBehindSetup(encoder, stores, step.destination.spillIndex(), step.sources[0].reg());
    }
}

static void findStoresOfArgumentsBehindSetupOfEntry(Encoder& encoder, const Block* block, ArenaVector<StoreBehindSetup>& stores)
{
    const Allocation& allocation = encoder.m_allocation;
    const BlockSteps& steps = allocation.stepsOf(block);
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = allocation.step(index);
        const Operation* argument = step.operation;
        if (!argument || argument->opcode() != ArgumentReg)
            return;
        if (step.kind == StepKind::Move || step.kind == StepKind::KeepPointer)
            continue;
        if (step.kind != StepKind::Execute)
            return;
        if (allocation.isStoredBehindDefinition(argument))
            noteStoreBehindSetup(encoder, stores, allocation.spillOf(argument), argument->argumentReg());
    }
}

static void findSavesBehindSetupOfRareRegion(Encoder& encoder, const Block* block, ArenaVector<StoreBehindSetup>& stores)
{
    const Allocation& allocation = encoder.m_allocation;
    const BlockSteps& steps = allocation.stepsOf(block);
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = allocation.step(index);
        if (step.kind != StepKind::Save || !step.destination.isSpill() || std::ranges::find(stores.span(), step.destination.spillIndex(), &StoreBehindSetup::spill) != stores.span().end())
            return;
        noteStoreBehindSetup(encoder, stores, step.destination.spillIndex(), step.sources[0].reg());
    }
}

std::span<const GPRReg> Encoder::findRegistersThatTeardownPops(uint32_t beginOfSteps, uint32_t endOfSteps, uint32_t& endOfStepsAheadOfPops)
{
    constexpr int32_t wordSize = sizeof(CPURegister);
    endOfStepsAheadOfPops = endOfSteps;
    m_registersPoppedAtTeardown.clear();
    if (!isX86_64() || m_frame.hasFrameRecord() || m_frame.savesLieAtBottom())
        return { };
    const uint32_t numberOfWords = numberOfWordsBesideSavesOf(m_frame);
    uint32_t first = endOfSteps;
    m_registersPoppedAtTeardown.fill(m_arena, numberOfWords, InvalidGPRReg);
    for (; first > beginOfSteps; --first) {
        const Step& step = m_allocation.step(first - 1);
        if (step.kind != StepKind::Reload || !step.sources[0].isSpill() || step.width != Width64 || !step.destination.reg().isGPR())
            break;
        if (step.destination.reg().gpr() == scratchGPR || std::ranges::find(m_registersPoppedAtTeardown.span(), step.destination.reg().gpr()) != m_registersPoppedAtTeardown.span().end())
            break;
        const SpillFacts& facts = m_allocation.spills()[step.sources[0].spillIndex()];
        if (facts.width != Width64 || facts.isPointerBesideNarrowReference)
            break;
        MacroAssembler::Address address = addressOf(step.sources[0]);
        RELEASE_ASSERT(address.base == MacroAssembler::stackPointerRegister && address.offset >= 0 && !(address.offset % wordSize));
        uint32_t word = static_cast<uint32_t>(address.offset / wordSize);
        if (word >= numberOfWords || m_registersPoppedAtTeardown[word] != InvalidGPRReg)
            break;
        m_registersPoppedAtTeardown[word] = step.destination.reg().gpr();
    }
    unsigned numberOfStretchesThatNobodyWants = 0;
    for (uint32_t word = 0; word < numberOfWords; ++word)
        numberOfStretchesThatNobodyWants += m_registersPoppedAtTeardown[word] == InvalidGPRReg && (!word || m_registersPoppedAtTeardown[word - 1] != InvalidGPRReg);
    if (first == endOfSteps || numberOfStretchesThatNobodyWants > 1) {
        m_registersPoppedAtTeardown.clear();
        return { };
    }
    endOfStepsAheadOfPops = first;
    return m_registersPoppedAtTeardown.span();
}

std::span<const GPRReg> Encoder::findRegistersThatSetupPushes(const Block* block)
{
    constexpr uint32_t wordSize = sizeof(CPURegister);
    m_registersPushedAtSetup.clear();
    m_spillsPushedAtSetup.clear();
    if (!isX86_64() || m_proc.traits().savesFrameOnSuspend || m_frame.bytesBelowFrameRecord() > maxBytesAllocatedBeforeStackCheck)
        return { };
    for (const RegisterAtOffset& save : m_frame.calleeSaveRegisterAtOffsetList()) {
        if (!save.reg().isGPR())
            return { };
    }

    ArenaVector<StoreBehindSetup> stores;
    if (m_allocation.isHeadOfFramedPart(block))
        findSavesBehindSetupOfHead(*this, block, stores);
    else if (block->isEntry())
        findStoresOfArgumentsBehindSetupOfEntry(*this, block, stores);
    else if (placeOf(block).isRare())
        findSavesBehindSetupOfRareRegion(*this, block, stores);
    if (stores.isEmpty())
        return { };

    const uint32_t numberOfStores = stores.size();
    m_registersPushedAtSetup.fill(m_arena, numberOfStores, InvalidGPRReg);
    m_spillsPushedAtSetup.fill(m_arena, numberOfStores, noSpill);
    for (const StoreBehindSetup& store : stores) {
        uint32_t word = m_proc.slots().distanceOf(store.spill) / wordSize - 1;
        if (word >= numberOfStores || m_spillsPushedAtSetup[word] != noSpill)
            continue;
        m_registersPushedAtSetup[word] = store.source;
        m_spillsPushedAtSetup[word] = store.spill;
    }
    uint32_t numberOfWordsPushed = 0;
    while (numberOfWordsPushed < numberOfStores && m_spillsPushedAtSetup[numberOfWordsPushed] != noSpill)
        ++numberOfWordsPushed;

    const unsigned wordsAboveSlots = numberOfWordsAboveSlotsOf(m_frame);
    const bool wordsAreLeftBelow = numberOfWordsBesideSavesOf(m_frame) > wordsAboveSlots + numberOfWordsPushed;
    if (wordsAboveSlots && wordsAreLeftBelow)
        numberOfWordsPushed = 0;

    m_registersPushedAtSetup.shrink(numberOfWordsPushed);
    m_spillsPushedAtSetup.shrink(numberOfWordsPushed);
    return m_registersPushedAtSetup.span();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
