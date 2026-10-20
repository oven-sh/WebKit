/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirPlanFrame.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirCustom.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirPhaseScope.h"
#include "AirShrinkWrapFrame.h"
#include "AirStackColoring.h"
#include "CallFrame.h"
#include <algorithm>
#include <array>
#include <limits>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace B3 { namespace Air {

static_assert(AirLimits::maxBytesAboveFrameRecord + sizeof(CallerFrameAndPC) <= AirLimits::pairPreIndexReach);
static_assert(!(AirLimits::maxBytesAboveFrameRecord % stackAlignmentBytes()) && !(AirLimits::maxBytesBelowFrameRecord % stackAlignmentBytes()));

static constexpr unsigned wordSize = sizeof(CPURegister);
static_assert(wordSize == StackColoring::bytesPerWord);

static constexpr unsigned oneByteDisplacementReach = -std::numeric_limits<int8_t>::min();

namespace {

struct FrameItem {
    StackSlot* lockedSlot { nullptr };
    unsigned firstSpillWord { 0 };
    unsigned numberOfWords { 0 };
    unsigned alignment { 0 };
    unsigned useCount { 0 };
    unsigned bytesBelowAreaTop { 0 };
};

struct PushedSpill {
    unsigned itemIndex;
    Reg source;
    unsigned instIndex;
};

class FramePlanner {
public:
    FramePlanner(Code&, const StackColoring&, const FramePlacement&);
    void run();

private:
    static unsigned roundUpToStackAlignment(unsigned bytes) { return WTF::roundUpToMultipleOf(stackAlignmentBytes(), bytes); }
    static unsigned bytesBelowFramePointer(const StackSlot* slot) { return static_cast<unsigned>(-slot->offsetFromFP()); }
    static unsigned wordsWithinReach(unsigned bytesTaken) { return bytesTaken < oneByteDisplacementReach ? (oneByteDisplacementReach - bytesTaken) / wordSize : 0; }

    void planWithoutFrame();
    void createItems();
    void createSpillItems();
    void checkOffsetsAreRepresentable() const;
    unsigned computeItemBytesBound() const;
    unsigned computeFrameSizeBound() const;
    bool canPlaceSlotsAboveFrameRecord() const;
    bool areAllSavesGPRs() const;
    bool areSavesOnTop() const;
    void chooseCalleeSaveForm();
    bool isPushed(unsigned itemIndex) const;
    std::optional<unsigned> findItemToPush(const Arg& destination) const;
    void findSpillsAtFrameSetup();
    Vector<unsigned> computeItemOrder() const;
    void place(FrameItem&);
    void placeItems();
    void assignOffsets();
    void rewriteEnterFrame();

    Code& m_code;
    const StackColoring& m_coloring;
    const FramePlacement& m_placement;
    FrameLayout m_layout;
    Vector<FrameItem> m_items;
    Vector<unsigned> m_itemOfSpillWord;
    Vector<PushedSpill, AirLimits::maxRegistersPushedAtFrameSetup> m_pushedSpills;
    Vector<StackSlot*, 4> m_slotsWithFixedOffsets;
    unsigned m_bytesOfSaves { 0 };
    unsigned m_cursor { 0 };
};

FramePlanner::FramePlanner(Code& code, const StackColoring& coloring, const FramePlacement& placement)
    : m_code(code)
    , m_coloring(coloring)
    , m_placement(placement)
{
}

void FramePlanner::run()
{
    m_layout.isPlanned = true;
    m_layout.coverage = m_placement.coverage;
    m_layout.hasFrame = !m_placement.setups.isEmpty();
    m_layout.numberOfFrameSetups = m_placement.setups.size();
    m_layout.numberOfFrameTeardowns = m_placement.numberOfTeardowns;
    if (!m_layout.hasFrame) {
        planWithoutFrame();
        return;
    }

    if (StackSlot* saveSlot = m_code.calleeSaveStackSlot())
        m_bytesOfSaves = saveSlot->byteSize();
    for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList())
        m_layout.numberOfSavesClobberedAtSuspensions += m_code.calleeSavesClobberedAtSuspensions().contains(entry.reg(), IgnoreVectors);

    createItems();
    chooseCalleeSaveForm();
    findSpillsAtFrameSetup();
    placeItems();
    assignOffsets();
    rewriteEnterFrame();
    m_code.setFrameLayout(WTF::move(m_layout));
    m_code.setStackIsAllocated(true);
}

void FramePlanner::planWithoutFrame()
{
    RELEASE_ASSERT(!m_code.calleeSaveStackSlot());
    m_code.setFrameSize(0);
    m_code.setFrameLayout(WTF::move(m_layout));
    m_code.setStackIsAllocated(true);
}

void FramePlanner::createItems()
{
    RELEASE_ASSERT(m_coloring.unitsOfSlot.size() == m_code.stackSlots().size());
    const bool hasUseCounts = !m_coloring.useCountOfSlot.isEmpty();

    for (StackSlot* slot : m_code.stackSlots()) {
        if (!slot->isLocked() || slot == m_code.calleeSaveStackSlot())
            continue;
        if (slot->offsetFromFP()) {
            RELEASE_ASSERT(slot->offsetFromFP() + static_cast<intptr_t>(slot->byteSize()) <= -static_cast<intptr_t>(m_bytesOfSaves));
            m_slotsWithFixedOffsets.append(slot);
            continue;
        }
        FrameItem item;
        item.lockedSlot = slot;
        item.numberOfWords = slot->byteSize() / wordSize + !!(slot->byteSize() % wordSize);
        item.alignment = std::max(wordSize, slot->alignment());
        if (hasUseCounts)
            item.useCount = m_coloring.useCountOfSlot[slot->index()];
        m_items.append(item);
    }

    createSpillItems();

    if (hasUseCounts) {
        for (unsigned slotIndex = 0; slotIndex < m_coloring.unitsOfSlot.size(); ++slotIndex) {
            if (const auto& units = m_coloring.unitsOfSlot[slotIndex])
                m_items[m_itemOfSpillWord[units->first / StackColoring::unitsPerWord]].useCount += m_coloring.useCountOfSlot[slotIndex];
        }
    }

    std::ranges::sort(m_slotsWithFixedOffsets, std::ranges::greater { }, &StackSlot::offsetFromFP);
    checkOffsetsAreRepresentable();
}

void FramePlanner::createSpillItems()
{
    const unsigned numberOfSpillWords = m_coloring.numberOfWords();
    BitVector joinsNextWord(numberOfSpillWords);
    for (const std::optional<StackColoring::UnitRange>& units : m_coloring.unitsOfSlot) {
        if (!units)
            continue;
        RELEASE_ASSERT(units->first <= units->last && units->last < m_coloring.numberOfUnits);
        for (unsigned word = units->first / StackColoring::unitsPerWord; word < units->last / StackColoring::unitsPerWord; ++word)
            joinsNextWord.quickSet(word);
    }

    m_itemOfSpillWord.fill(0, numberOfSpillWords);
    for (unsigned word = 0; word < numberOfSpillWords;) {
        unsigned lastWord = word;
        while (joinsNextWord.quickGet(lastWord))
            ++lastWord;

        FrameItem item;
        item.firstSpillWord = word;
        item.numberOfWords = lastWord - word + 1;
        item.alignment = item.numberOfWords > 1 ? stackAlignmentBytes() : wordSize;
        RELEASE_ASSERT(item.numberOfWords == 1 || !(word % 2));
        for (; word <= lastWord; ++word)
            m_itemOfSpillWord[word] = m_items.size();
        m_items.append(item);
    }
}

void FramePlanner::checkOffsetsAreRepresentable() const
{
    uint64_t bytes = static_cast<uint64_t>(m_bytesOfSaves) + m_code.callArgAreaSizeInBytes() + stackAlignmentBytes();
    if (!m_slotsWithFixedOffsets.isEmpty())
        bytes += static_cast<uint64_t>(-m_slotsWithFixedOffsets.last()->offsetFromFP());
    for (const FrameItem& item : m_items)
        bytes += static_cast<uint64_t>(wordSize) * item.numberOfWords + wordSize;
    RELEASE_ASSERT(bytes <= static_cast<uint64_t>(std::numeric_limits<Value::OffsetType>::max()));
}

unsigned FramePlanner::computeItemBytesBound() const
{
    unsigned bytes = 0;
    for (const FrameItem& item : m_items)
        bytes += wordSize * item.numberOfWords + (item.alignment > wordSize ? wordSize : 0);
    return bytes;
}

unsigned FramePlanner::computeFrameSizeBound() const
{
    unsigned bytes = roundUpToStackAlignment(m_bytesOfSaves) + computeItemBytesBound() + m_code.callArgAreaSizeInBytes();
    if (!m_slotsWithFixedOffsets.isEmpty())
        bytes += roundUpToStackAlignment(bytesBelowFramePointer(m_slotsWithFixedOffsets.last()));
    return roundUpToStackAlignment(bytes);
}

bool FramePlanner::canPlaceSlotsAboveFrameRecord() const
{
    return isARM64()
        && m_code.policy().frame.mayPlaceSlotsAboveFrameRecord
        && !m_code.callArgAreaSizeInBytes()
        && !m_placement.addressesIncomingArgumentsInFrame
        && m_slotsWithFixedOffsets.isEmpty()
        && roundUpToStackAlignment(computeItemBytesBound()) <= AirLimits::maxBytesAboveFrameRecord
        && roundUpToStackAlignment(m_bytesOfSaves) <= AirLimits::maxBytesBelowFrameRecord;
}

bool FramePlanner::areAllSavesGPRs() const
{
    return !m_code.calleeSaveRegisters().numberOfSetFPRs();
}

bool FramePlanner::areSavesOnTop() const
{
    return m_layout.slotArea == SlotArea::BelowFrameRecord && m_layout.calleeSaveForm != CalleeSaveForm::PushedAtFrameBottom;
}

void FramePlanner::chooseCalleeSaveForm()
{
    if (canPlaceSlotsAboveFrameRecord()) {
        m_layout.slotArea = SlotArea::AboveFrameRecord;
        if (m_bytesOfSaves)
            m_layout.calleeSaveForm = CalleeSaveForm::PushedBelowFrameRecord;
        return;
    }

    if (!isX86_64() || !m_bytesOfSaves || !areAllSavesGPRs() || m_bytesOfSaves != m_code.calleeSaveRegisterAtOffsetList().registerCount() * wordSize)
        return;

    bool canPushAtFrameBottom = m_code.policy().frame.mayPushCalleeSaves
        && !m_code.callArgAreaSizeInBytes()
        && m_slotsWithFixedOffsets.isEmpty()
        && computeFrameSizeBound() <= AirLimits::maxFrameSizeWithPushedRegisters;
    m_layout.calleeSaveForm = canPushAtFrameBottom ? CalleeSaveForm::PushedAtFrameBottom : CalleeSaveForm::PushedBelowFrameRecord;
}

bool FramePlanner::isPushed(unsigned itemIndex) const
{
    return m_pushedSpills.containsIf([&](const PushedSpill& spill) {
        return spill.itemIndex == itemIndex;
    });
}

std::optional<unsigned> FramePlanner::findItemToPush(const Arg& destination) const
{
    StackSlot* slot = destination.stackSlot();
    if (!slot->isSpill() || slot->byteSize() != wordSize || destination.offset())
        return std::nullopt;
    const std::optional<StackColoring::UnitRange>& units = m_coloring.unitsOfSlot[slot->index()];
    if (!units)
        return std::nullopt;
    unsigned itemIndex = m_itemOfSpillWord[units->first / StackColoring::unitsPerWord];
    if (m_items[itemIndex].numberOfWords != 1 || isPushed(itemIndex))
        return std::nullopt;
    return itemIndex;
}

void FramePlanner::findSpillsAtFrameSetup()
{
    if (!isX86_64()
        || !m_code.policy().frame.mayPushSpillsAtFrameSetup
        || m_code.callArgAreaSizeInBytes()
        || m_placement.setups.size() != 1
        || !m_slotsWithFixedOffsets.isEmpty()
        || computeFrameSizeBound() > AirLimits::maxFrameSizeWithPushedRegisters
        || (m_bytesOfSaves && m_layout.calleeSaveForm != CalleeSaveForm::PushedAtFrameBottom))
        return;

    BasicBlock* block = m_placement.setups[0].block;
    const unsigned setupIndex = m_placement.setups[0].instIndex;
    const Inst& enterFrame = block->at(setupIndex);
    RELEASE_ASSERT(enterFrame.kind.opcode == EnterFrame);

    std::array<Reg, MacroAssembler::numberOfRegisters()> valueAtSetupIn;
    for (unsigned i = 0; i < valueAtSetupIn.size(); ++i)
        valueAtSetupIn[i] = Reg::fromIndex(i);
    valueAtSetupIn[Reg(MacroAssembler::stackPointerRegister).index()] = Reg();
    valueAtSetupIn[Reg(MacroAssembler::framePointerRegister).index()] = Reg();
    for (unsigned i = 1; i <= EnterFrameCustom::numberOfScratchRegisters(enterFrame); ++i) {
        if (enterFrame.args()[i].isGPR())
            valueAtSetupIn[enterFrame.args()[i].reg().index()] = Reg();
    }

    const unsigned endIndex = std::min(block->size(), setupIndex + 1 + AirLimits::maxInstsScannedForSpillsAtFrameSetup);
    for (unsigned index = setupIndex + 1; index < endIndex && m_pushedSpills.size() < AirLimits::maxRegistersPushedAtFrameSetup; ++index) {
        Inst& inst = block->at(index);
        bool isMove = inst.kind.opcode == Move && inst.args().size() == 2 && inst.args()[0].isGPR();
        if (isMove && inst.args()[1].isGPR()) {
            valueAtSetupIn[inst.args()[1].reg().index()] = valueAtSetupIn[inst.args()[0].reg().index()];
            continue;
        }
        if (isMove && inst.args()[1].isStack()) {
            Reg source = valueAtSetupIn[inst.args()[0].reg().index()];
            std::optional<unsigned> itemIndex = findItemToPush(inst.args()[1]);
            if (!source || !itemIndex)
                return;
            m_pushedSpills.append(PushedSpill { *itemIndex, source, index });
            continue;
        }
        if (inst.kind.opcode == Patch || inst.isTerminal())
            return;
        for (Arg& arg : inst.args()) {
            if (arg.isStack() || arg.isCallArg())
                return;
        }
        inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
            if (Arg::isAnyDef(role) && tmp.isGPR())
                valueAtSetupIn[tmp.reg().index()] = Reg();
        });
    }
}

Vector<unsigned> FramePlanner::computeItemOrder() const
{
    Vector<unsigned> order;
    order.reserveInitialCapacity(m_items.size());
    unsigned numberOfWords = 0;
    for (unsigned itemIndex = 0; itemIndex < m_items.size(); ++itemIndex) {
        if (isPushed(itemIndex))
            continue;
        order.append(itemIndex);
        numberOfWords += m_items[itemIndex].numberOfWords;
    }

    const FramePolicy& policy = m_code.policy().frame;
    const unsigned wordsNearFramePointer = wordsWithinReach(m_cursor);
    if (!isX86() || !policy.ordersSlotsByUse || m_coloring.useCountOfSlot.isEmpty() || numberOfWords <= wordsNearFramePointer)
        return order;

    std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return static_cast<uint64_t>(m_items[a].useCount) * m_items[b].numberOfWords > static_cast<uint64_t>(m_items[b].useCount) * m_items[a].numberOfWords;
    });

    unsigned wordsNearStackPointer = 0;
    if (policy.prefersStackPointerAddresses) {
        unsigned bytesBelowItems = m_code.callArgAreaSizeInBytes();
        if (m_layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom)
            bytesBelowItems += m_bytesOfSaves;
        wordsNearStackPointer = wordsWithinReach(bytesBelowItems);
    }

    auto findEndOfGroup = [&](size_t begin, unsigned capacityInWords) {
        size_t end = begin;
        for (; end < order.size() && m_items[order[end]].numberOfWords <= capacityInWords; ++end)
            capacityInWords -= m_items[order[end]].numberOfWords;
        return end;
    };
    size_t endOfTopGroup = findEndOfGroup(0, wordsNearFramePointer);
    size_t endOfBottomGroup = findEndOfGroup(endOfTopGroup, wordsNearStackPointer);

    std::span<const unsigned> itemsByDensity = order.span();
    Vector<unsigned> orderInFrame;
    orderInFrame.reserveInitialCapacity(order.size());
    orderInFrame.append(itemsByDensity.first(endOfTopGroup));
    orderInFrame.append(itemsByDensity.subspan(endOfBottomGroup));
    orderInFrame.append(itemsByDensity.subspan(endOfTopGroup, endOfBottomGroup - endOfTopGroup));
    return orderInFrame;
}

void FramePlanner::place(FrameItem& item)
{
    const unsigned size = wordSize * item.numberOfWords;
    m_cursor = WTF::roundUpToMultipleOf(item.alignment, m_cursor + size);
    for (StackSlot* slot : m_slotsWithFixedOffsets) {
        unsigned slotEnd = bytesBelowFramePointer(slot);
        if (WTF::rangesOverlap(slotEnd - slot->byteSize(), slotEnd, m_cursor - size, m_cursor))
            m_cursor = WTF::roundUpToMultipleOf(item.alignment, slotEnd + size);
    }
    item.bytesBelowAreaTop = m_cursor;
}

void FramePlanner::placeItems()
{
    m_cursor = areSavesOnTop() ? m_bytesOfSaves : 0;
    for (unsigned position = 0; position < m_pushedSpills.size(); ++position) {
        FrameItem& item = m_items[m_pushedSpills[position].itemIndex];
        place(item);
        RELEASE_ASSERT(item.bytesBelowAreaTop == wordSize * (position + 1));
    }
    for (unsigned itemIndex : computeItemOrder())
        place(m_items[itemIndex]);
}

void FramePlanner::assignOffsets()
{
    const unsigned callArgAreaSize = m_code.callArgAreaSizeInBytes();
    const bool placesSlotsAbove = m_layout.slotArea == SlotArea::AboveFrameRecord;
    const bool pushesBeforeStackCheck = m_layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom || !m_pushedSpills.isEmpty();

    unsigned frameSize = 0;
    intptr_t areaTop = 0;
    intptr_t saveSlotOffset = 0;
    if (placesSlotsAbove) {
        unsigned above = roundUpToStackAlignment(m_cursor);
        unsigned below = roundUpToStackAlignment(m_bytesOfSaves);
        RELEASE_ASSERT(above <= AirLimits::maxBytesAboveFrameRecord && below <= AirLimits::maxBytesBelowFrameRecord);
        m_layout.bytesAboveFrameRecord = above;
        frameSize = above + below;
        areaTop = sizeof(CallerFrameAndPC) + above;
        saveSlotOffset = -static_cast<intptr_t>(below);
    } else if (m_layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom) {
        frameSize = roundUpToStackAlignment(m_cursor + m_bytesOfSaves);
        saveSlotOffset = -static_cast<intptr_t>(frameSize);
    } else {
        unsigned bytesOfSlots = m_cursor;
        if (!m_slotsWithFixedOffsets.isEmpty())
            bytesOfSlots = std::max(bytesOfSlots, bytesBelowFramePointer(m_slotsWithFixedOffsets.last()));
        frameSize = roundUpToStackAlignment(bytesOfSlots + callArgAreaSize);
        saveSlotOffset = -static_cast<intptr_t>(m_bytesOfSaves);
    }

    auto setOffset = [&](StackSlot* slot, unsigned bytesBelowAreaTop) {
        intptr_t offset = areaTop - static_cast<intptr_t>(bytesBelowAreaTop);
        RELEASE_ASSERT(!(offset % static_cast<intptr_t>(slot->alignment())));
        slot->setOffsetFromFP(offset);
    };

    for (const FrameItem& item : m_items) {
        if (item.lockedSlot)
            setOffset(item.lockedSlot, item.bytesBelowAreaTop);
    }
    for (StackSlot* slot : m_code.stackSlots()) {
        const std::optional<StackColoring::UnitRange>& units = m_coloring.unitsOfSlot[slot->index()];
        if (!units)
            continue;
        const FrameItem& item = m_items[m_itemOfSpillWord[units->first / StackColoring::unitsPerWord]];
        unsigned itemEnd = wordSize * (item.firstSpillWord + item.numberOfWords);
        unsigned slotEnd = StackColoring::bytesPerUnit * (units->last + 1);
        setOffset(slot, item.bytesBelowAreaTop - (itemEnd - slotEnd));
    }
    if (StackSlot* saveSlot = m_code.calleeSaveStackSlot()) {
        RELEASE_ASSERT(!(saveSlotOffset % static_cast<intptr_t>(wordSize)));
        RELEASE_ASSERT(!areSavesOnTop() || saveSlotOffset == -static_cast<intptr_t>(saveSlot->byteSize()));
        saveSlot->setOffsetFromFP(saveSlotOffset);
    }
    m_code.setFrameSize(frameSize);

    RELEASE_ASSERT(!callArgAreaSize || (!pushesBeforeStackCheck && !placesSlotsAbove));
    RELEASE_ASSERT(!pushesBeforeStackCheck || frameSize <= AirLimits::maxFrameSizeWithPushedRegisters);
    RELEASE_ASSERT(m_pushedSpills.isEmpty() || m_placement.setups.size() == 1);
    RELEASE_ASSERT(frameSize <= m_code.policy().frame.maxBytesAllocatedBeforeStackCheck
        || (m_layout.coverage != FrameCoverage::SomePaths && m_layout.coverage != FrameCoverage::EveryPathNeedsFrame));
}

void FramePlanner::rewriteEnterFrame()
{
    if (m_pushedSpills.isEmpty())
        return;

    BasicBlock* block = m_placement.setups[0].block;
    Inst& enterFrame = block->at(m_placement.setups[0].instIndex);
    Vector<Arg, 16> operands;
    operands.append(enterFrame.args());
    for (const PushedSpill& spill : m_pushedSpills) {
        operands.append(Tmp(spill.source));
        block->at(spill.instIndex) = Inst();
    }
    enterFrame = Inst(EnterFrame, enterFrame.origin, operands.span());
    block->insts().removeAllMatching([](const Inst& inst) {
        return !inst;
    });
    m_layout.numberOfSpillsPushedAtFrameSetup = m_pushedSpills.size();
}

} // anonymous namespace

void planFrame(Code& code, const StackColoring& coloring, const FramePlacement& placement)
{
    PhaseScope phaseScope(code, "planFrame"_s);
    ASSERT(code.policy().frame.emitsFrame && !code.stackIsAllocated());
    FramePlanner(code, coloring, placement).run();
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
