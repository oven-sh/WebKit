/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineChecker.h"

#if ENABLE(AOT)

#include "AOTMachineFrameForms.h"
#include "StackAlignment.h"
#include <algorithm>
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

class FrameChecker {
    WTF_MAKE_NONCOPYABLE(FrameChecker);
public:
    explicit FrameChecker(Procedure&);
    bool run();

private:
    static constexpr uint32_t bytesPerWord = sizeof(uint64_t);
    static constexpr uint32_t bytesPerCell = sizeof(uint32_t);
    static constexpr uint32_t bytesOfFrameRecord = 2 * bytesPerWord;

    using Cell = uint32_t;

    struct CellRange {
        Cell begin { 0 };
        Cell end { 0 };
    };
    struct SpillInCell {
        Cell cell { 0 };
        Position begin { 0 };
        Position end { 0 };
        SpillIndex spill { noSpill };
    };
    struct SuspensionInOrder {
        Position late { 0 };
        const PatchOperation* patch { nullptr };
        uint32_t index { 0 };
    };

    bool fail(FailureKind kind, const Operation* operation = nullptr, PlaceOfCheck check = PlaceOfCheck::here())
    {
        m_proc.fail(kind, operation, check);
        return false;
    }

    bool checkShape();
    bool checkCalleeSaves();
    bool savedFrameCanDescribe(RegisterMask saves) const;

    bool runsInFrame(const Block*) const;
    bool isEnteredInFrame(const Block*) const;
    bool isLeftInFrame(const Block*) const;
    bool checkBlock(const Block*);
    bool checkFramedPart();
    bool checkReturnInPlaceOfJump(const Block*);
    bool checkStepsAtHeadOfFramedPart();
    bool checkStep(const Step&, bool frameIsSetUp, bool resultIsStoredBehindIt);
    bool noteLocation(Location, const Operation* blamed, bool& needsFrame);
    bool isStoredBehindDefinition(const Operation*) const;
    static bool namesFrame(const Operation*);
    static RegisterMask registerMaskOf(Location);

    bool findCells(int64_t offsetFromFP, uint32_t byteSize, uint32_t alignment, CellRange&) const;
    bool holdThroughout(int64_t offsetFromFP, uint32_t byteSize, uint32_t alignment);
    bool checkPlaces();

    bool isKeptAtSuspensions(int64_t offsetFromFP) const { return offsetFromFP + m_bytesBelowFrameRecord >= static_cast<int64_t>(m_frame.bytesDeadAtSuspensions()); }
    static uint32_t wordHolding(int64_t offsetOfByteFromFP) { return static_cast<uint32_t>((bytesPerWord - 1 - offsetOfByteFromFP) / bytesPerWord); }
    uint32_t lowestWordKeptAtSuspensions() const { return static_cast<uint32_t>((m_bytesBelowFrameRecord - m_frame.bytesDeadAtSuspensions() / stackAlignmentBytes() * stackAlignmentBytes()) / bytesPerWord); }
    bool checkSuspensions();

    Procedure& m_proc;
    Arena& m_arena;
    const Order& m_order;
    const Liveness& m_liveness;
    const Allocation& m_allocation;
    const Frame& m_frame;
    std::span<const SpillFacts> m_spills;
    const int64_t m_bytesBelowFrameRecord;
    const RegisterMask m_calleeSaved;
    const RegisterMask m_calleeSavesLeftToCaller;
    BitSpan m_isHeldThroughout;
    const Block* m_block { nullptr };
};

FrameChecker::FrameChecker(Procedure& proc)
    : m_proc(proc)
    , m_arena(proc.arena())
    , m_order(proc.order())
    , m_liveness(proc.liveness())
    , m_allocation(proc.allocation())
    , m_frame(proc.frame())
    , m_spills(proc.allocation().spills())
    , m_bytesBelowFrameRecord(proc.frame().bytesBelowFrameRecord())
    , m_calleeSaved(registerFileFor(proc.traits()).calleeSaved)
    , m_calleeSavesLeftToCaller(m_calleeSaved & ~proc.frame().calleeSavesMask())
    , m_isHeldThroughout(BitSpan::create(proc.arena(), (proc.frame().bytesBelowFrameRecord() + bytesPerWord) / bytesPerCell))
{
}

bool FrameChecker::run()
{
    if (!checkShape() || !checkCalleeSaves() || !checkFramedPart() || !checkStepsAtHeadOfFramedPart())
        return false;
    for (const Block* block : m_order.blocksInOrder()) {
        if (!checkBlock(block))
            return false;
    }
    if (!m_frame.hasFrame())
        return true;
    return checkPlaces() && checkSuspensions();
}

bool FrameChecker::checkShape()
{
    if (m_proc.traits().setsUpFrameAtEntry() && m_frame.coverage() != FrameCoverage::WholeFunction)
        return fail(FailureKind::MissingFrameSetup);
    if (!m_frame.hasFrame())
        return true;
    if (!m_frame.hasFrameRecord() && (!m_proc.traits().mayOmitFrameRecord || m_proc.traits().setsUpFrameAtEntry() || !m_proc.stackSlots().empty()))
        return fail(FailureKind::MissingFrameSetup);
    if ((m_bytesBelowFrameRecord + bytesOfFrameRecord) % stackAlignmentBytes())
        return fail(FailureKind::WrongValueInSpillSlot);
    if (m_frame.bytesDeadAtSuspensions() > m_frame.bytesBelowFrameRecord() || m_frame.bytesDeadAtSuspensions() % bytesPerWord)
        return fail(FailureKind::WrongValueInSpillSlot);
    return true;
}

bool FrameChecker::savedFrameCanDescribe(RegisterMask saves) const
{
    if (saves & ~m_calleeSaved)
        return false;
#if CPU(ARM64)
    if (!saves)
        return true;
    RegisterMask lowestSave = RegisterMask { 1 } << std::countr_zero(saves);
    return !(std::popcount(saves) % 2) && saves + lowestSave == maskOf(Reg(ARM64Registers::x24)) << 1;
#else
    return true;
#endif
}

bool FrameChecker::checkCalleeSaves()
{
    RegisterMask saves = m_frame.calleeSavesMask();
    if (m_allocation.usedCalleeSaves() & ~saves)
        return fail(FailureKind::WrongValueInRegister);
    if (saves && m_frame.coverage() != FrameCoverage::WholeFunction && m_frame.coverage() != FrameCoverage::FramedPart)
        return fail(FailureKind::MissingFrameSetup);
    if (m_proc.traits().savesFrameOnSuspend && !savedFrameCanDescribe(saves))
        return fail(FailureKind::WrongValueInRegister);

    const RegisterAtOffsetList& list = m_frame.calleeSaveRegisterAtOffsetList();
    size_t count = list.registerCount();
    RegisterMask listed = 0;
    for (size_t index = 0; index < count; ++index) {
        const RegisterAtOffset& save = list.at(index);
        int64_t offsetInRun = offsetOfFirstSaveOf(m_frame) + static_cast<int64_t>(bytesPerWord * index);
        if (save.reg().index() > Reg::maxIndex() || maskOf(save.reg()) <= listed || save.offset() != offsetInRun)
            return fail(FailureKind::WrongValueInSpillSlot);
        if (!holdThroughout(save.offset(), bytesPerWord, bytesPerWord))
            return false;
        listed |= maskOf(save.reg());
    }
    return listed == saves || fail(FailureKind::WrongValueInSpillSlot);
}

bool FrameChecker::runsInFrame(const Block* block) const
{
    switch (m_frame.coverage()) {
    case FrameCoverage::None:
        return false;
    case FrameCoverage::WholeFunction:
        return true;
    case FrameCoverage::RareRegions: {
        const BlockPlace& place = m_order.of(block);
        return place.isRare() && m_allocation.needsFrameIn(place.rareRegion);
    }
    case FrameCoverage::FramedPart:
        return m_frame.holdsStepsOf(block);
    }
    return false;
}

bool FrameChecker::isEnteredInFrame(const Block* block) const
{
    if (!runsInFrame(block) || block->isEntry())
        return false;
    if (m_frame.coverage() == FrameCoverage::FramedPart)
        return !m_frame.isSetUpAtHeadOf(block);
    return m_frame.coverage() != FrameCoverage::RareRegions || !m_order.of(block).entersRareRegion;
}

bool FrameChecker::isLeftInFrame(const Block* block) const
{
    if (!runsInFrame(block))
        return false;
    if (m_frame.coverage() == FrameCoverage::FramedPart)
        return !m_frame.isTornDownAtEndOf(block);
    return m_frame.coverage() != FrameCoverage::RareRegions || !m_order.of(block).leavesRareRegion;
}

bool FrameChecker::isStoredBehindDefinition(const Operation* value) const
{
    return m_allocation.isStoredBehindDefinition(value);
}

bool FrameChecker::checkFramedPart()
{
    if (m_frame.coverage() != FrameCoverage::FramedPart)
        return true;
    if (!m_allocation.hasFramedPart() || m_allocation.needsFrameAheadOfFramedPart() || m_proc.entrypoints().size() != 1)
        return fail(FailureKind::MissingFrameSetup);
    for (const Allocation::HeadOfFramedPart& headOfFramedPart : m_allocation.headsOfFramedPart()) {
        const Block* head = headOfFramedPart.block;
        const BlockPlace& placeOfHead = m_order.of(head);
        if (head->isEntry() || head->isLandingPad() || !placeOfHead.isReached() || (placeOfHead.isRare() && m_order.rareRegions()[placeOfHead.rareRegion].onlyEntry != head) || m_liveness.standsInLoopThatMayTurn(m_order, placeOfHead) || !m_frame.isSetUpAtHeadOf(head) || !m_allocation.isHeadOfFramedPart(head))
            return fail(FailureKind::MissingFrameSetup, head->last());
        for (const Operation* phi : head->phis()) {
            if (!m_liveness.isDropped(phi) && m_allocation.isSpilled(phi) && !m_allocation.registerAtDefinitionOf(phi).isSet())
                return fail(FailureKind::WrongValueInSpillSlot, phi);
        }
    }

    for (const Block* block : m_order.blocksInOrder()) {
        bool isInFramedPart = m_allocation.isInFramedPart(block);
        bool isHead = m_allocation.isHeadOfFramedPart(block);
        if (isInFramedPart && !m_frame.holdsStepsOf(block))
            return fail(FailureKind::MissingFrameSetup, block->last());
        for (const Block* predecessor : m_order.of(block).predecessors) {
            if (!isInFramedPart || isHead)
                break;
            bool bringsOnlyHeadsKnownAboveBlock = false;
            if (m_allocation.isInFramedPart(predecessor)) {
                const Allocation::HeadOfFramedPart* first = &m_allocation.headOfFramedPartAbove(predecessor);
                const uint32_t further = m_allocation.furtherHeadsOfFramedPartAbove(predecessor);
                bool firstIsKnown = first == &m_allocation.headOfFramedPartAbove(block);
                bool furtherAreKnown = further == UINT32_MAX;
                uint32_t known = m_allocation.furtherHeadsOfFramedPartAbove(block);
                for (size_t steps = m_order.of(block).predecessors.size(); ; --steps) {
                    furtherAreKnown |= known == further;
                    if ((firstIsKnown && furtherAreKnown) || known == UINT32_MAX || !steps)
                        break;
                    firstIsKnown |= &m_allocation.headsOfFramedPart()[m_allocation.furtherHeadsOfFramedPart()[known].head] == first;
                    known = m_allocation.furtherHeadsOfFramedPart()[known].rest;
                }
                bringsOnlyHeadsKnownAboveBlock = firstIsKnown && furtherAreKnown;
            }
            if (!bringsOnlyHeadsKnownAboveBlock)
                return fail(FailureKind::MissingFrameSetup, block->last());
        }
        if (!m_frame.isSetUpAtHeadOf(block) || isHead)
            continue;
        if (isInFramedPart || !m_frame.holdsStepsOf(block) || !m_order.of(block).entersRareRegion)
            return fail(FailureKind::MissingFrameSetup, block->last());
    }
    return true;
}

bool FrameChecker::checkStepsAtHeadOfFramedPart()
{
    for (const Allocation::HeadOfFramedPart& head : m_allocation.headsOfFramedPart()) {
        uint32_t begin = head.beginOfSteps;
        uint32_t end = head.endOfSteps;
        if (begin == end)
            continue;
        if (begin > end || end > m_allocation.steps().size() || !runsInFrame(head.block))
            return fail(FailureKind::MissingFrameSetup);
        m_block = head.block;
        for (uint32_t index = begin; index < end; ++index) {
            const Step& step = m_allocation.step(index);
            if (step.kind != StepKind::Save || !step.destination.isSpill() || !step.sources[0].isRegister() || step.destination.spillIndex() >= m_spills.size())
                return fail(FailureKind::WrongValueInSpillSlot, step.operation);
            const SpillFacts& facts = m_spills[step.destination.spillIndex()];
            if (!facts.isStoredAtHeadOfFramedPart || facts.value != step.operation)
                return fail(FailureKind::WrongValueInSpillSlot, step.operation);
            if (!checkStep(step, true, false))
                return false;
        }
    }
    return true;
}

bool FrameChecker::checkReturnInPlaceOfJump(const Block* block)
{
    const Operation* last = block->last();
    if (isLeftInFrame(block) || block->numSuccessors() != 1 || !last->isUnconditionalJump())
        return fail(FailureKind::MissingFrameSetup, last);
    const Block* returning = block->successor(0).block();
    const BlockSteps& steps = m_allocation.stepsOf(returning);
    if (returning->last()->opcode() != Return || !isEnteredInFrame(returning) || steps.beginOfEdgeMoves != steps.endOfEdgeMoves)
        return fail(FailureKind::MissingFrameSetup, last);
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        const Step& step = m_allocation.step(index);
        Opcode opcode = step.operation ? step.operation->opcode() : Oops;
        if (step.kind != StepKind::Execute || (opcode != Nop && opcode != Phi && opcode != Upsilon))
            return fail(FailureKind::MissingFrameSetup, last);
    }
    for (const Operation* phi : returning->phis()) {
        if (!m_liveness.isDropped(phi) && m_allocation.isSpilled(phi))
            return fail(FailureKind::MissingFrameSetup, phi);
    }
    return true;
}

bool FrameChecker::namesFrame(const Operation* value)
{
    if (!value->hasNoPosition())
        return false;
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    for (const Operation* child : value->children()) {
        if (namesFrame(child))
            return true;
    }
    return false;
}

RegisterMask FrameChecker::registerMaskOf(Location location)
{
    if (!location.isRegister() || location.reg().index() > Reg::maxIndex())
        return 0;
    return maskOf(location.reg());
}

bool FrameChecker::noteLocation(Location location, const Operation* blamed, bool& needsFrame)
{
    switch (location.kind()) {
    case Location::Spill:
        needsFrame = true;
        if (location.spillIndex() >= m_spills.size())
            return fail(FailureKind::WrongValueInSpillSlot, blamed);
        return !m_spills[location.spillIndex()].isStoredAtHeadOfFramedPart || m_allocation.isInFramedPart(m_block) || fail(FailureKind::WrongValueInSpillSlot, blamed);
    case Location::SlotBytes:
        needsFrame = true;
        return location.indexOfStackSlot() < m_proc.stackSlots().size() || fail(FailureKind::WrongValueInSpillSlot, blamed);
    case Location::Outgoing:
        needsFrame = true;
        return static_cast<uint64_t>(location.offsetFromSP()) + bytesPerWord <= m_proc.callArgAreaSizeInBytes() || fail(FailureKind::WrongValueInSpillSlot, blamed);
    case Location::None:
    case Location::Register:
    case Location::InstanceWord:
    case Location::Immediate:
        return true;
    }
    return fail(FailureKind::MalformedProcedure, blamed);
}

bool FrameChecker::checkStep(const Step& step, bool frameIsSetUp, bool resultIsStoredBehindIt)
{
    const Operation* operation = step.operation;
    if (!operation)
        return fail(FailureKind::MalformedProcedure);

    bool needsFrame = false;
    if (!noteLocation(step.destination, operation, needsFrame))
        return false;
    for (Location source : step.sources) {
        if (!noteLocation(source, operation, needsFrame))
            return false;
    }
    RegisterMask written = registerMaskOf(step.destination);
    RegisterMask writtenByCodeOfPatch = 0;

    switch (step.kind) {
    case StepKind::Execute: {
        needsFrame |= resultIsStoredBehindIt && isStoredBehindDefinition(operation);

        const PatchOperation* patch = operation->as<PatchOperation>();
        bool emitsCode = !patch || !patch->onlyKeepsChildrenAlive;
        if (emitsCode) {
            forEachLeaf(m_liveness, operation, [&](const Operation*, unsigned, const Operation* leaf) {
                needsFrame |= namesFrame(leaf);
            });
        }
        if (patch && emitsCode) {
            needsFrame |= patch->needsFrameRecord() || !patch->runsWithoutFrame();
            bool everyPathPassesSetup = m_frame.coverage() == FrameCoverage::WholeFunction || (m_frame.coverage() == FrameCoverage::FramedPart && m_allocation.isInFramedPart(m_block));
            if (patch->followsFrameSetup() && !everyPathPassesSetup && !(patch->testsStackAheadOfFramedPart() && m_frame.coverage() == FrameCoverage::FramedPart))
                return fail(FailureKind::MissingFrameSetup, patch);
            if (patch->opcode() == Patch)
                writtenByCodeOfPatch = patch->earlyClobberedMask() | patch->lateClobberedMask();
        }

        if (step.patchAssignment == UINT32_MAX)
            break;
        const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
        written |= assignment.gpScratch | assignment.fpScratch;
        size_t numberOfResults = m_proc.resultCount(operation->type());
        for (size_t index = 0; index < assignment.locations.size(); ++index) {
            if (!noteLocation(assignment.locations[index], operation, needsFrame))
                return false;
            if (index < numberOfResults)
                written |= registerMaskOf(assignment.locations[index]);
        }
        break;
    }
    case StepKind::Materialize:
        needsFrame |= namesFrame(operation);
        break;
    case StepKind::Swap:
        written |= registerMaskOf(step.sources[0]);
        break;
    case StepKind::Move:
    case StepKind::Reload:
    case StepKind::Save:
    case StepKind::KeepPointer:
        break;
    }

    if (!m_frame.hasFrameRecord() && stepCountsOnFrameRecord(m_proc, step))
        return fail(FailureKind::MissingFrameSetup, operation);
    if (needsFrame && !frameIsSetUp)
        return fail(FailureKind::MissingFrameSetup, operation);
    if (written & m_calleeSavesLeftToCaller)
        return fail(FailureKind::WrongValueInRegister, operation);
    if (m_frame.coverage() == FrameCoverage::FramedPart && !frameIsSetUp && ((written | writtenByCodeOfPatch) & m_calleeSaved))
        return fail(FailureKind::WrongValueInRegister, operation);
    return true;
}

bool FrameChecker::checkBlock(const Block* block)
{
    if (block->isEmpty())
        return fail(FailureKind::MalformedProcedure);
    const Operation* last = block->last();
    const BlockSteps& steps = m_allocation.stepsOf(block);
    size_t numberOfSteps = m_allocation.steps().size();
    if (steps.begin > steps.end || steps.end > numberOfSteps
        || steps.beginOfEdgeMoves > steps.endOfEdgeMoves || steps.endOfEdgeMoves > numberOfSteps
        || steps.terminator >= numberOfSteps)
        return fail(FailureKind::MalformedProcedure, last);

    m_block = block;
    bool isInFrame = runsInFrame(block);
    bool isLeft = isLeftInFrame(block);
    if (m_frame.coverage() == FrameCoverage::FramedPart && m_frame.returnsInPlaceOfJumpAtEndOf(block)) {
        if (!checkReturnInPlaceOfJump(block))
            return false;
    } else {
        for (const Edge& edge : block->successors()) {
            if (isEnteredInFrame(edge.block()) != isLeft)
                return fail(FailureKind::MissingFrameSetup, last);
        }
    }
    if (m_frame.coverage() == FrameCoverage::FramedPart && !isEnteredInFrame(block)) {
        for (const RegisterBinding& binding : m_allocation.registersAtHeadOf(block)) {
            if (maskOf(binding.reg) & m_calleeSaved)
                return fail(FailureKind::WrongValueInRegister, binding.value);
        }
    }
    if (isInFrame && !isLeft && !last->isUnconditionalJump())
        return fail(FailureKind::MissingFrameSetup, last);

    for (const Operation* phi : block->phis()) {
        if (!isInFrame && isStoredBehindDefinition(phi))
            return fail(FailureKind::MissingFrameSetup, phi);
    }
    for (uint32_t index = steps.begin; index < steps.end; ++index) {
        if (!checkStep(m_allocation.step(index), isInFrame, true))
            return false;
    }
    for (uint32_t index = steps.beginOfEdgeMoves; index < steps.endOfEdgeMoves; ++index) {
        if (!checkStep(m_allocation.step(index), isInFrame, true))
            return false;
    }
    if (!checkStep(m_allocation.step(steps.terminator), isLeft, false))
        return false;

    if (last->opcode() == Patch && block->numSuccessors() && isStoredBehindDefinition(last)) {
        const Block* blockOfStore = block->successor(0).block();
        if (!blockOfStore->isLandingPad() && !runsInFrame(blockOfStore))
            return fail(FailureKind::MissingFrameSetup, last);
    }
    return true;
}

bool FrameChecker::findCells(int64_t offsetFromFP, uint32_t byteSize, uint32_t alignment, CellRange& cells) const
{
    int64_t offsetFromSP = offsetFromFP + m_bytesBelowFrameRecord;
    if (offsetFromSP < 0 || offsetFromFP + byteSize > (m_frame.hasFrameRecord() ? 0 : static_cast<int64_t>(bytesPerWord)) || offsetFromFP % alignment)
        return false;
    cells.begin = static_cast<Cell>(offsetFromSP / bytesPerCell);
    cells.end = static_cast<Cell>((offsetFromSP + byteSize + bytesPerCell - 1) / bytesPerCell);
    return true;
}

bool FrameChecker::holdThroughout(int64_t offsetFromFP, uint32_t byteSize, uint32_t alignment)
{
    CellRange cells;
    if (!findCells(offsetFromFP, byteSize, alignment, cells))
        return fail(FailureKind::WrongValueInSpillSlot);
    for (Cell cell = cells.begin; cell < cells.end; ++cell) {
        if (m_isHeldThroughout.get(cell))
            return fail(FailureKind::WrongValueInSpillSlot);
        m_isHeldThroughout.set(cell);
    }
    return true;
}

bool FrameChecker::checkPlaces()
{
    if (!holdThroughout(-m_bytesBelowFrameRecord, m_proc.callArgAreaSizeInBytes(), bytesPerWord))
        return false;
    for (const StackSlot* slot : m_proc.stackSlots()) {
        uint32_t alignment = slot->byteSize() >= stackAlignmentBytes() ? stackAlignmentBytes() : bytesPerWord;
        if (!holdThroughout(slot->offsetFromFP(), slot->byteSize(), alignment))
            return false;
    }

    const SpillIndex numberOfSpills = static_cast<SpillIndex>(m_spills.size());
    ArenaVector<SpillInCell> spillsInCells;
    spillsInCells.reserve(m_arena, 4 * numberOfSpills);
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        const SpillFacts& facts = m_spills[spill];
        if (facts.width != Width32 && facts.width != Width64)
            return fail(FailureKind::WrongValueInSpillSlot, facts.value);
        uint32_t byteSize = facts.isPointerBesideNarrowReference ? bytesPerWord : static_cast<uint32_t>(bytesForWidth(facts.width));
        CellRange cells;
        if (!findCells(m_frame.offsetFromFPOf(spill), byteSize, byteSize, cells))
            return fail(FailureKind::WrongValueInSpillSlot, facts.value);
        if (facts.hasGap() && (facts.beginOfGap <= facts.begin || facts.endOfGap < facts.beginOfGap || facts.end <= facts.endOfGap))
            return fail(FailureKind::WrongValueInSpillSlot, facts.value);
        for (Cell cell = cells.begin; cell < cells.end; ++cell) {
            if (m_isHeldThroughout.get(cell))
                return fail(FailureKind::WrongValueInSpillSlot, facts.value);
            if (facts.hasGap()) {
                spillsInCells.append(m_arena, SpillInCell { cell, facts.begin, facts.beginOfGap - 1, spill });
                spillsInCells.append(m_arena, SpillInCell { cell, facts.endOfGap + 1, facts.end, spill });
            } else
                spillsInCells.append(m_arena, SpillInCell { cell, facts.begin, std::max(facts.begin, facts.end), spill });
        }
    }

    std::sort(spillsInCells.begin(), spillsInCells.end(), [](const SpillInCell& a, const SpillInCell& b) {
        if (a.cell != b.cell)
            return a.cell < b.cell;
        if (a.begin != b.begin)
            return a.begin < b.begin;
        return a.spill < b.spill;
    });
    for (uint32_t index = 1; index < spillsInCells.size(); ++index) {
        const SpillInCell& earlier = spillsInCells[index - 1];
        const SpillInCell& later = spillsInCells[index];
        if (earlier.cell == later.cell && later.begin <= earlier.end)
            return fail(FailureKind::WrongValueInSpillSlot, m_spills[later.spill].value);
    }
    return true;
}

bool FrameChecker::checkSuspensions()
{
    std::span<PatchOperation* const> listed = m_allocation.suspensions();
    std::span<SuspensionInOrder> suspensions = m_arena.createSpan<SuspensionInOrder>(listed.size());
    for (size_t index = 0; index < listed.size(); ++index) {
        if (!listed[index]->frameWordsDeadAcrossSuspension())
            return fail(FailureKind::WrongValueInSpillSlot, listed[index]);
        suspensions[index] = { m_order.latePositionOf(listed[index]), listed[index], static_cast<uint32_t>(index) };
    }
    std::sort(suspensions.begin(), suspensions.end(), [](const SuspensionInOrder& a, const SuspensionInOrder& b) {
        return a.late < b.late;
    });

    for (SpillIndex spill = 0; spill < m_spills.size(); ++spill) {
        const SpillFacts& facts = m_spills[spill];
        int64_t offset = m_frame.offsetFromFPOf(spill);
        if (facts.livesAcrossSuspension && !isKeptAtSuspensions(offset))
            return fail(FailureKind::WrongValueInSpillSlot, facts.value);

        auto suspension = std::partition_point(suspensions.begin(), suspensions.end(), [&](const SuspensionInOrder& candidate) {
            return candidate.late < facts.begin;
        });
        for (; suspension != suspensions.end() && suspension->late - 1 <= facts.end; ++suspension) {
            if (!m_allocation.liveSpillsAt(suspension->index).get(spill))
                continue;
            if (!facts.livesAcrossSuspension || facts.isInGap(suspension->late - 1) || facts.isInGap(suspension->late) || suspension->patch->frameWordsDeadAcrossSuspension()->get(wordHolding(offset)))
                return fail(FailureKind::WrongValueInSpillSlot, facts.value);
        }
    }

    if (suspensions.empty())
        return true;
    for (const StackSlot* slot : m_proc.stackSlots()) {
        if (slot->isDeadAtSuspensions() || !slot->byteSize())
            continue;
        int64_t offset = slot->offsetFromFP();
        if (!isKeptAtSuspensions(offset))
            return fail(FailureKind::WrongValueInSpillSlot);
        uint32_t highestWord = wordHolding(offset + slot->byteSize() - 1);
        uint32_t lowestWord = wordHolding(offset);
        for (const SuspensionInOrder& suspension : suspensions) {
            const BitSpan& deadWords = *suspension.patch->frameWordsDeadAcrossSuspension();
            for (uint32_t word = highestWord; word <= lowestWord; ++word) {
                if (deadWords.get(word))
                    return fail(FailureKind::WrongValueInSpillSlot, suspension.patch);
            }
        }
    }

    auto isKeptWithLastPairAlone = [&](int64_t offsetOfHighestByteFromFP) {
        return !isKeptAtSuspensions(offsetOfHighestByteFromFP) && wordHolding(offsetOfHighestByteFromFP) <= lowestWordKeptAtSuspensions();
    };
    for (SpillIndex spill = 0; spill < m_spills.size(); ++spill) {
        if (isKeptWithLastPairAlone(m_frame.offsetFromFPOf(spill)))
            return fail(FailureKind::WrongValueInSpillSlot, m_spills[spill].value);
    }
    for (const StackSlot* slot : m_proc.stackSlots()) {
        if (slot->byteSize() && isKeptWithLastPairAlone(slot->offsetFromFP() + slot->byteSize() - 1))
            return fail(FailureKind::WrongValueInSpillSlot);
    }
    if (m_proc.callArgAreaSizeInBytes() && isKeptWithLastPairAlone(static_cast<int64_t>(m_proc.callArgAreaSizeInBytes()) - m_bytesBelowFrameRecord - 1))
        return fail(FailureKind::WrongValueInSpillSlot);
    return true;
}

bool checkFrame(Procedure& proc)
{
    return FrameChecker(proc).run();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
