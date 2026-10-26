/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirPlanFrame.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirCustom.h"
#include "AirFrameEmission.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirPhaseScope.h"
#include "AirShrinkWrapFrame.h"
#include "AirStackColoring.h"
#include "B3StackmapValue.h"
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
    bool isDeadAtSuspensions { false };
};

// A store right behind a frame setup, which a push can stand for, or a reload right in front of a teardown, which a pop can stand for.
struct MoveAtFrameSite {
    unsigned itemIndex;
    Reg reg;
    unsigned instIndex;
};

struct FrameSite {
    BasicBlock* block { nullptr };
    unsigned instIndex { 0 };
    bool isSetup { false };
    // What a teardown may pop a word into that nobody wants.
    Reg deadRegister;
    Vector<MoveAtFrameSite, AirLimits::maxRegistersPushedAtFrameSetup> moves;
};

struct WordsAtFrameSite {
    // In the order in which the site walks the frame: a setup from the top, a teardown from the stack pointer.
    uint64_t words { 0 };
    // Of the setup or the teardown, but for the pushes and pops of the registers that the callee saves, less the bytes of the moves that it stands for.
    int bytes { 0 };
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
    bool needsFramePointer() const;
    bool canOmitFrameRecord() const;
    int maxBytesAddedWithoutFrameRecord() const;
    int bytesOfAddressInFrame(intptr_t offsetFromFramePointer) const;
    void placeFrame(bool hasFrameRecord);
    int countBytesThatDependOnFrameRecord() const;
    unsigned bytesBelowFramePointerOf(unsigned bytesOfArea) const;
    void createItems();
    void createSpillItems();
    void checkOffsetsAreRepresentable() const;
    unsigned computeItemBytesBound() const;
    unsigned computeFrameSizeBound() const;
    bool canPlaceSlotsAboveFrameRecord() const;
    bool areAllSavesGPRs() const;
    bool areSavesOnTop() const;
    void chooseCalleeSaveForm();
    bool isMovedAtFrameSites(unsigned itemIndex) const { return m_numberOfFrameSitesMoving[itemIndex]; }
    std::optional<unsigned> findWordItem(const Arg& stackArg) const;
    bool mayPushAndPop() const;
    void findMovesAtFrameSites();
    void findStoresBehindSetup(FrameSite&);
    void findReloadsInFrontOfTeardown(FrameSite&);
    Vector<unsigned> computeItemOrder() const;
    void place(FrameItem&);
    void placeItems();
    void assignOffsets();
    unsigned numberOfWordsAllocated() const { return m_code.frameSize() / wordSize + !m_layout.hasFrameRecord; }
    unsigned wordOf(const FrameSite&, const MoveAtFrameSite&) const;
    WordsAtFrameSite chooseWords(const FrameSite&) const;
    void rewriteFrameSites();

    Code& m_code;
    const StackColoring& m_coloring;
    const FramePlacement& m_placement;
    FrameLayout m_layout;
    Vector<FrameItem> m_items;
    Vector<unsigned> m_itemOfSpillWord;
    Vector<FrameSite> m_frameSites;
    Vector<unsigned> m_numberOfFrameSitesMoving;
    bool m_hasMovesAtFrameSites { false };
    Vector<StackSlot*, 4> m_slotsWithFixedOffsets;
    unsigned m_bytesOfSaves { 0 };
    // Counts down from the top of the slot area, which lies m_bytesOfAreaAboveFramePointer above the place of the frame record.
    unsigned m_cursor { 0 };
    unsigned m_bytesOfAreaAboveFramePointer { 0 };
    // From the top of the slot area down to the last item that some suspension keeps.
    unsigned m_bytesKeptAtSuspensions { 0 };
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
    findMovesAtFrameSites();
    // A frame without a record saves instructions each time it is set up. It is chosen where it costs no bytes, or no more than the policy allows.
    placeFrame(true);
    if (canOmitFrameRecord()) {
        int bytesWithFrameRecord = countBytesThatDependOnFrameRecord();
        placeFrame(false);
        if (countBytesThatDependOnFrameRecord() - bytesWithFrameRecord > maxBytesAddedWithoutFrameRecord())
            placeFrame(true);
    }
    rewriteFrameSites();
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

// lowerStackArgs() turns an address off the frame pointer, and a move of it, into one off the stack pointer. A Patch only describes such an address to its generator.
bool FramePlanner::needsFramePointer() const
{
    const Tmp framePointer(MacroAssembler::framePointerRegister);
    for (BasicBlock* block : m_code) {
        for (Inst& inst : *block) {
            bool isMoveOfFramePointer = inst.kind.opcode == Move && inst.args().size() == 2 && inst.args()[0] == Arg(framePointer) && inst.args()[1].isGPR() && inst.args()[1] != Arg(framePointer);
            bool isSumWithFramePointer = inst.kind.opcode == Add64 && inst.args().size() == 3 && inst.args()[0].isImm() && inst.args()[1] == Arg(framePointer) && inst.args()[2].isGPR() && inst.args()[2] != Arg(framePointer);
            if (isMoveOfFramePointer || isSumWithFramePointer)
                continue;
            if (inst.kind.opcode == Patch && inst.origin) {
                if (auto* stackmap = inst.origin->as<StackmapValue>(); stackmap && stackmap->needsFrameRecord())
                    return true;
            }
            for (Arg& arg : inst.args()) {
                bool isBaseOfAddress = arg.isAddr() || (arg.isIndex() && arg.index() != framePointer);
                if (arg.usesTmp(framePointer) && !isBaseOfAddress)
                    return true;
            }
        }
    }
    return false;
}

bool FramePlanner::canOmitFrameRecord() const
{
    return isX86_64()
        && m_code.policy().frame.mayOmitFrameRecord
        && m_slotsWithFixedOffsets.isEmpty()
        && !needsFramePointer();
}

int FramePlanner::maxBytesAddedWithoutFrameRecord() const
{
    double frequencyOfSetups = 0;
    for (const FrameSetupSite& setup : m_placement.setups)
        frequencyOfSetups += setup.block->frequency();
    bool isSetUpAtMostEntries = 2 * frequencyOfSetups >= m_code[0]->frequency();
    return isSetUpAtMostEntries ? static_cast<int>(m_code.policy().frame.maxBytesAddedWithoutFrameRecord) : 0;
}

void FramePlanner::placeFrame(bool hasFrameRecord)
{
    m_layout.hasFrameRecord = hasFrameRecord;
    m_bytesOfAreaAboveFramePointer = hasFrameRecord ? 0 : wordSize;
    placeItems();
    assignOffsets();
}

// As lowerStackArgs() writes it: an address off the stack pointer takes a byte more than one off the frame pointer.
int FramePlanner::bytesOfAddressInFrame(intptr_t offsetFromFramePointer) const
{
    auto isShort = [](intptr_t offset) {
        return offset >= std::numeric_limits<int8_t>::min() && offset <= std::numeric_limits<int8_t>::max();
    };
    intptr_t offsetFromStackPointer = offsetFromFramePointer + static_cast<intptr_t>(m_code.frameSize());
    int bytesOffStackPointer = 1 + (!offsetFromStackPointer ? 0 : isShort(offsetFromStackPointer) ? 1 : 4);
    if (!m_layout.hasFrameRecord)
        return bytesOffStackPointer;
    if (m_code.policy().frame.prefersStackPointerAddresses && !isShort(offsetFromFramePointer) && offsetFromFramePointer < 0 && offsetFromStackPointer >= 0 && isShort(offsetFromStackPointer))
        return bytesOffStackPointer;
    return isShort(offsetFromFramePointer) ? 1 : 4;
}

// By the lengths of the instructions: of the frame as it is placed now.
int FramePlanner::countBytesThatDependOnFrameRecord() const
{
    int bytes = 0;
    for (const FrameSite& site : m_frameSites)
        bytes += chooseWords(site).bytes;

    const Tmp framePointer(MacroAssembler::framePointerRegister);
    for (BasicBlock* block : m_code) {
        for (Inst& inst : *block) {
            for (Arg& arg : inst.args()) {
                if (arg.isStack())
                    bytes += bytesOfAddressInFrame(arg.stackSlot()->offsetFromFP() + arg.offset());
                else if (arg.isMemory() && arg.usesTmp(framePointer))
                    bytes += bytesOfAddressInFrame(arg.offset());
            }
        }
    }
    if (m_layout.calleeSaveForm == CalleeSaveForm::Stored) {
        for (const RegisterAtOffset& entry : m_code.calleeSaveRegisterAtOffsetList())
            bytes += static_cast<int>(m_frameSites.size()) * bytesOfAddressInFrame(entry.offset());
    }
    return bytes;
}

unsigned FramePlanner::bytesBelowFramePointerOf(unsigned bytesOfArea) const
{
    return bytesOfArea <= m_bytesOfAreaAboveFramePointer ? 0 : roundUpToStackAlignment(bytesOfArea - m_bytesOfAreaAboveFramePointer);
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
        item.isDeadAtSuspensions = m_coloring.hasSuspensions && slot->isDeadAtSuspensions();
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

    for (size_t slotIndex : m_coloring.spillSlotsLiveAcrossSuspensions) {
        if (const auto& units = m_coloring.unitsOfSlot[slotIndex])
            m_items[m_itemOfSpillWord[units->first / StackColoring::unitsPerWord]].isDeadAtSuspensions = false;
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
        item.isDeadAtSuspensions = m_coloring.hasSuspensions;
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

std::optional<unsigned> FramePlanner::findWordItem(const Arg& stackArg) const
{
    StackSlot* slot = stackArg.stackSlot();
    if (!slot->isSpill() || slot->byteSize() != wordSize || stackArg.offset())
        return std::nullopt;
    const std::optional<StackColoring::UnitRange>& units = m_coloring.unitsOfSlot[slot->index()];
    if (!units)
        return std::nullopt;
    unsigned itemIndex = m_itemOfSpillWord[units->first / StackColoring::unitsPerWord];
    if (m_items[itemIndex].numberOfWords != 1)
        return std::nullopt;
    return itemIndex;
}

bool FramePlanner::mayPushAndPop() const
{
    return isX86_64()
        && m_code.policy().frame.mayPushSpillsAtFrameSetup
        && !m_code.callArgAreaSizeInBytes()
        && m_slotsWithFixedOffsets.isEmpty()
        && computeFrameSizeBound() <= AirLimits::maxFrameSizeWithPushedRegisters;
}

void FramePlanner::findMovesAtFrameSites()
{
    m_numberOfFrameSitesMoving.fill(0, m_items.size());
    if (!isX86_64())
        return;

    const bool mayPushAndPop = this->mayPushAndPop();
    for (const FrameSetupSite& setup : m_placement.setups) {
        FrameSite site { setup.block, setup.instIndex, true, { }, { } };
        if (mayPushAndPop)
            findStoresBehindSetup(site);
        m_frameSites.append(WTF::move(site));
    }
    for (BasicBlock* block : m_code) {
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            if (block->at(instIndex).kind.opcode != LeaveFrame)
                continue;
            FrameSite site { block, instIndex, false, { }, { } };
            if (std::optional<GPRReg> deadRegister = registerDeadBehindFrameTeardown(m_code, block, instIndex))
                site.deadRegister = *deadRegister;
            if (mayPushAndPop)
                findReloadsInFrontOfTeardown(site);
            m_frameSites.append(WTF::move(site));
        }
    }
    for (const FrameSite& site : m_frameSites) {
        m_hasMovesAtFrameSites |= !site.moves.isEmpty();
        for (const MoveAtFrameSite& move : site.moves)
            ++m_numberOfFrameSitesMoving[move.itemIndex];
    }
}

void FramePlanner::findStoresBehindSetup(FrameSite& site)
{
    BasicBlock* block = site.block;
    const Inst& enterFrame = block->at(site.instIndex);
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

    const unsigned endIndex = std::min(block->size(), site.instIndex + 1 + AirLimits::maxInstsScannedForSpillsAtFrameSetup);
    for (unsigned index = site.instIndex + 1; index < endIndex && site.moves.size() < AirLimits::maxRegistersPushedAtFrameSetup; ++index) {
        Inst& inst = block->at(index);
        bool isMove = inst.kind.opcode == Move && inst.args().size() == 2 && inst.args()[0].isGPR();
        if (isMove && inst.args()[1].isGPR()) {
            valueAtSetupIn[inst.args()[1].reg().index()] = valueAtSetupIn[inst.args()[0].reg().index()];
            continue;
        }
        if (isMove && inst.args()[1].isStack()) {
            Reg source = valueAtSetupIn[inst.args()[0].reg().index()];
            std::optional<unsigned> itemIndex = findWordItem(inst.args()[1]);
            bool isStoredTwice = itemIndex && site.moves.containsIf([&](const MoveAtFrameSite& move) {
                return move.itemIndex == *itemIndex;
            });
            if (!source || !itemIndex || isStoredTwice)
                return;
            site.moves.append(MoveAtFrameSite { *itemIndex, source, index });
            continue;
        }
        if (inst.kind.opcode == Patch || inst.kind.opcode == LeaveFrame || inst.isTerminal())
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

// A reload may wait for the teardown where nothing in between names its register or writes to the frame.
void FramePlanner::findReloadsInFrontOfTeardown(FrameSite& site)
{
    BasicBlock* block = site.block;
    uint64_t namedRegisters = 0;
    block->at(site.instIndex).forEachTmpFast([&](Tmp& tmp) {
        namedRegisters |= static_cast<uint64_t>(1) << tmp.reg().index();
    });
    const unsigned firstIndex = site.instIndex > AirLimits::maxInstsScannedForSpillsAtFrameSetup ? site.instIndex - AirLimits::maxInstsScannedForSpillsAtFrameSetup : 0;
    for (unsigned index = site.instIndex; index-- > firstIndex && site.moves.size() < AirLimits::maxRegistersPushedAtFrameSetup;) {
        Inst& inst = block->at(index);
        if (inst.kind.opcode == Patch || inst.kind.opcode == EnterFrame || inst.kind.opcode == LeaveFrame)
            return;
        bool isReload = inst.kind.opcode == Move && inst.args().size() == 2 && inst.args()[0].isStack() && inst.args()[1].isGPR();
        if (isReload) {
            Reg destination = inst.args()[1].reg();
            std::optional<unsigned> itemIndex = findWordItem(inst.args()[0]);
            bool isReloadedTwice = itemIndex && site.moves.containsIf([&](const MoveAtFrameSite& move) {
                return move.itemIndex == *itemIndex;
            });
            if (itemIndex && !isReloadedTwice && !(namedRegisters & static_cast<uint64_t>(1) << destination.index())) {
                site.moves.append(MoveAtFrameSite { *itemIndex, destination, index });
                namedRegisters |= static_cast<uint64_t>(1) << destination.index();
                continue;
            }
        }
        bool writesFrame = false;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            writesFrame |= (arg.isStack() || arg.isCallArg()) && (Arg::isAnyDef(role) || role == Arg::UseAddr);
        });
        if (writesFrame)
            return;
        inst.forEachTmpFast([&](Tmp& tmp) {
            namedRegisters |= static_cast<uint64_t>(1) << tmp.reg().index();
        });
    }
}

Vector<unsigned> FramePlanner::computeItemOrder() const
{
    Vector<unsigned> order;
    order.reserveInitialCapacity(m_items.size());
    unsigned numberOfWords = 0;
    for (unsigned itemIndex = 0; itemIndex < m_items.size(); ++itemIndex) {
        if (isMovedAtFrameSites(itemIndex))
            continue;
        order.append(itemIndex);
        numberOfWords += m_items[itemIndex].numberOfWords;
    }

    const FramePolicy& policy = m_code.policy().frame;
    const unsigned wordsNearFramePointer = m_layout.hasFrameRecord ? wordsWithinReach(m_cursor) : 0;
    if (!isX86() || !policy.ordersSlotsByUse || m_coloring.useCountOfSlot.isEmpty() || numberOfWords <= wordsNearFramePointer)
        return order;

    std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return static_cast<uint64_t>(m_items[a].useCount) * m_items[b].numberOfWords > static_cast<uint64_t>(m_items[b].useCount) * m_items[a].numberOfWords;
    });

    unsigned wordsNearStackPointer = 0;
    if (policy.prefersStackPointerAddresses || !m_layout.hasFrameRecord) {
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
    m_cursor = WTF::roundUpToMultipleOf(item.alignment, m_cursor + size - m_bytesOfAreaAboveFramePointer) + m_bytesOfAreaAboveFramePointer;
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
    // On top, side by side, what most setups push and most teardowns pop: then no stretch of other words lies between them.
    Vector<unsigned, 8> itemsMovedAtFrameSites;
    for (const FrameSite& site : m_frameSites) {
        for (const MoveAtFrameSite& move : site.moves) {
            if (!itemsMovedAtFrameSites.contains(move.itemIndex))
                itemsMovedAtFrameSites.append(move.itemIndex);
        }
    }
    std::stable_sort(itemsMovedAtFrameSites.begin(), itemsMovedAtFrameSites.end(), [&](unsigned a, unsigned b) {
        return m_numberOfFrameSitesMoving[a] > m_numberOfFrameSitesMoving[b];
    });
    for (unsigned itemIndex : itemsMovedAtFrameSites)
        place(m_items[itemIndex]);
    // At the bottom, next to the outgoing arguments, what no suspension has to keep.
    Vector<unsigned> order = computeItemOrder();
    auto firstDeadItem = std::stable_partition(order.begin(), order.end(), [&](unsigned itemIndex) {
        return !m_items[itemIndex].isDeadAtSuspensions;
    });
    for (auto it = order.begin(); it != firstDeadItem; ++it)
        place(m_items[*it]);
    // A suspension keeps whole pairs of words.
    if (firstDeadItem != order.end())
        m_cursor = roundUpToStackAlignment(m_cursor);
    m_bytesKeptAtSuspensions = m_cursor;
    for (auto it = firstDeadItem; it != order.end(); ++it)
        place(m_items[*it]);
}

void FramePlanner::assignOffsets()
{
    const unsigned callArgAreaSize = m_code.callArgAreaSizeInBytes();
    const bool placesSlotsAbove = m_layout.slotArea == SlotArea::AboveFrameRecord;
    const bool pushesBeforeStackCheck = m_layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom || m_hasMovesAtFrameSites;

    unsigned frameSize = 0;
    intptr_t areaTop = m_bytesOfAreaAboveFramePointer;
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
        frameSize = bytesBelowFramePointerOf(m_cursor + m_bytesOfSaves);
        saveSlotOffset = -static_cast<intptr_t>(frameSize);
    } else {
        unsigned bytesOfSlots = m_cursor;
        if (!m_slotsWithFixedOffsets.isEmpty())
            bytesOfSlots = std::max(bytesOfSlots, bytesBelowFramePointer(m_slotsWithFixedOffsets.last()));
        frameSize = bytesBelowFramePointerOf(bytesOfSlots + callArgAreaSize);
        saveSlotOffset = areaTop - static_cast<intptr_t>(m_bytesOfSaves);
        if (m_layout.hasFrameRecord)
            m_layout.bytesDeadAtSuspensions = frameSize - (m_slotsWithFixedOffsets.isEmpty() ? m_bytesKeptAtSuspensions : bytesOfSlots);
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
        RELEASE_ASSERT(!areSavesOnTop() || saveSlotOffset == areaTop - static_cast<intptr_t>(saveSlot->byteSize()));
        saveSlot->setOffsetFromFP(saveSlotOffset);
    }
    m_code.setFrameSize(frameSize);

    RELEASE_ASSERT(!callArgAreaSize || (!pushesBeforeStackCheck && !placesSlotsAbove));
    RELEASE_ASSERT(!pushesBeforeStackCheck || frameSize <= AirLimits::maxFrameSizeWithPushedRegisters);
    RELEASE_ASSERT(frameSize <= m_code.policy().frame.maxBytesAllocatedBeforeStackCheck
        || (m_layout.coverage != FrameCoverage::SomePaths && m_layout.coverage != FrameCoverage::EveryPathNeedsFrame));
}

unsigned FramePlanner::wordOf(const FrameSite& site, const MoveAtFrameSite& move) const
{
    const unsigned numberOfWords = numberOfWordsAllocated();
    unsigned wordFromTop = m_items[move.itemIndex].bytesBelowAreaTop / wordSize - 1;
    RELEASE_ASSERT(wordFromTop < numberOfWords);
    return site.isSetup ? wordFromTop : numberOfWords - 1 - wordFromTop;
}

// Every stretch of words that the site neither pushes nor pops costs a move of the stack pointer. The site takes no more instructions than with moves alone.
WordsAtFrameSite FramePlanner::chooseWords(const FrameSite& site) const
{
    const bool hasFrameRecord = m_layout.hasFrameRecord;
    const unsigned numberOfWords = numberOfWordsAllocated();
    const unsigned numberOfSaves = m_bytesOfSaves / wordSize;
    const unsigned wordsOfSavesOnTop = m_layout.calleeSaveForm == CalleeSaveForm::PushedBelowFrameRecord ? numberOfSaves : 0;
    const unsigned wordsOfSavesAtBottom = m_layout.calleeSaveForm == CalleeSaveForm::PushedAtFrameBottom ? numberOfSaves : 0;
    const unsigned firstWord = site.isSetup ? wordsOfSavesOnTop : wordsOfSavesAtBottom;
    const unsigned endWord = numberOfWords - (site.isSetup ? wordsOfSavesAtBottom : wordsOfSavesOnTop);

    auto bytesOfPushOrPop = [](Reg reg) {
#if CPU(X86_64)
        return reg.gpr() >= X86Registers::r8 ? 2 : 1;
#else
        UNUSED_PARAM(reg);
        return 0;
#endif
    };
    auto bytesOfStretch = [&](unsigned words) {
        if (!words)
            return 0;
        if (words == 1 && site.isSetup)
            return 1;
        if (words == 1 && site.deadRegister)
            return bytesOfPushOrPop(site.deadRegister);
        return words * wordSize <= static_cast<unsigned>(std::numeric_limits<int8_t>::max()) ? 4 : 7;
    };
    auto bytesOfMove = [&](unsigned word) {
        constexpr int bytesWithoutAddress = 3;
        unsigned wordFromTop = site.isSetup ? word : numberOfWords - 1 - word;
        return bytesWithoutAddress + bytesOfAddressInFrame(-static_cast<intptr_t>(wordSize * (wordFromTop + hasFrameRecord)));
    };
    constexpr int bytesOfRecordSetup = 4;
    constexpr int bytesOfRecordTeardown = 1;
    const int bytesForRecord = !hasFrameRecord ? 0 : site.isSetup ? bytesOfRecordSetup : bytesOfRecordTeardown;
    // `leave` gives the whole frame back.
    const bool releasesForNothing = hasFrameRecord && !site.isSetup;

    WordsAtFrameSite best;
    best.bytes = bytesForRecord + (releasesForNothing ? 0 : bytesOfStretch(endWord - firstWord));
    if (site.moves.isEmpty())
        return best;

    RELEASE_ASSERT(numberOfWords < 64);
    uint64_t candidates = 0;
    std::array<Reg, 64> registerOfWord;
    for (const MoveAtFrameSite& move : site.moves) {
        unsigned word = wordOf(site, move);
        RELEASE_ASSERT(word >= firstWord && word < endWord);
        candidates |= static_cast<uint64_t>(1) << word;
        registerOfWord[word] = move.reg;
    }
    uint64_t wordsAtTheEnds = 0;
    for (unsigned word = firstWord; word < endWord && (candidates >> word & 1); ++word)
        wordsAtTheEnds |= static_cast<uint64_t>(1) << word;
    for (unsigned word = endWord; word-- > firstWord && (candidates >> word & 1);)
        wordsAtTheEnds |= static_cast<uint64_t>(1) << word;

    const unsigned stretchesWithMovesAlone = !releasesForNothing && endWord > firstWord;
    for (uint64_t words : { candidates, wordsAtTheEnds }) {
        if (!words)
            continue;
        int bytes = bytesForRecord;
        unsigned numberOfStretches = 0;
        unsigned wordsInStretch = 0;
        for (unsigned word = firstWord; word < endWord; ++word) {
            if (!(words >> word & 1)) {
                numberOfStretches += !wordsInStretch++;
                continue;
            }
            bytes += bytesOfStretch(wordsInStretch) + bytesOfPushOrPop(registerOfWord[word]) - bytesOfMove(word);
            wordsInStretch = 0;
        }
        bytes += bytesOfStretch(wordsInStretch);
        if (bytes < best.bytes && numberOfStretches <= stretchesWithMovesAlone)
            best = { words, bytes };
    }
    return best;
}

void FramePlanner::rewriteFrameSites()
{
    if (!m_hasMovesAtFrameSites)
        return;

    Vector<BasicBlock*, 8> changedBlocks;
    for (const FrameSite& site : m_frameSites) {
        uint64_t words = chooseWords(site).words;
        if (!words)
            continue;

        Inst& inst = site.block->at(site.instIndex);
        Vector<Arg, 16> operands;
        operands.append(inst.args());
        for (const MoveAtFrameSite& move : site.moves) {
            unsigned word = wordOf(site, move);
            if (!(words >> word & 1))
                continue;
            operands.append(Tmp(move.reg));
            operands.append(Arg::imm(word));
            site.block->at(move.instIndex) = Inst();
            ++(site.isSetup ? m_layout.numberOfSpillsPushedAtFrameSetup : m_layout.numberOfReloadsPoppedAtFrameTeardown);
        }
        inst = Inst(inst.kind.opcode, inst.origin, operands.span());
        if (!changedBlocks.contains(site.block))
            changedBlocks.append(site.block);
    }
    for (BasicBlock* block : changedBlocks) {
        block->insts().removeAllMatching([](const Inst& inst) {
            return !inst;
        });
    }
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
