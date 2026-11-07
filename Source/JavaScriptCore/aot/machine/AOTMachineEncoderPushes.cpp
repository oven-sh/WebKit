/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AOTMachineFrameForms.h"

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
