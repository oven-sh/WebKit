/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineChecker.h"

#if ENABLE(AOT)

#include "AOTSavedFrame.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// A range in the order of allocation also covers blocks that no path from the definition reaches, and there the allocator owes the value nothing.
// So liveness is computed again along the edges: from each read up through the predecessors to the defining block.
class CollectorRulesChecker {
    WTF_MAKE_NONCOPYABLE(CollectorRulesChecker);
public:
    explicit CollectorRulesChecker(Procedure&);
    bool run();

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr unsigned numberOfRegisters = 64;

    // A value that one of the three rules applies to.
    struct WatchedValue {
        const Operation* value;
        const Block* block;
        Position definition;
        uint32_t firstRead { none };
        SpillIndex pointerBeside { noSpill };
        // At heads of the framed part. Every path to a safepoint below a head passes that head, and no path to any other has to.
        bool pointerIsKeptInFramedPartAlone { false };
    };
    struct Read {
        const Block* block;
        Position position;
        uint32_t next;
    };
    // The part of one block in which a watched value is live.
    struct LiveRange {
        uint32_t watched;
        Position begin;
        Position end;
        uint32_t nextInBlock;
    };
    struct Safepoint {
        const PatchOperation* patch;
        Position late;
    };

    bool fail(FailureKind kind, const Operation* operation)
    {
        m_proc.fail(kind, operation);
        m_failed = true;
        return false;
    }

    static bool isCageBase(const Operation* base)
    {
        if (base->opcode() == Add && base->hasNoPosition())
            base = base->child(0);
        return base->isArgumentReg(Reg(cellCageBaseGPR));
    }

    bool needsPointerBeside(const Operation*) const;
    bool isFrameAddress(const Operation*) const;
    bool isComputedFromFrameAddress(const Operation*) const;
    const Operation* keeperOf(const Operation*) const;

    void findFrameAddresses();
    void findWatchedValues();
    void define(const Operation*, const Block*, Position);
    void noteRead(const Operation*, const Block*, Position);
    void noteReadsOf(const Operation*, const Block*, Position);
    void findLiveRangesOf(uint32_t watched);
    void extendTo(uint32_t watched, const Block*, Position end);

    const Operation* valueIn(Location location) const
    {
        return location.isRegister() && location.reg().index() < numberOfRegisters ? m_valueIn[location.reg().index()] : nullptr;
    }
    void put(Location location, const Operation* value)
    {
        if (location.isRegister() && location.reg().index() < numberOfRegisters)
            m_valueIn[location.reg().index()] = value;
    }

    void enter(const Block*);
    void passSteps(uint32_t begin, uint32_t end);
    void pass(const Step&);
    void execute(const Step&);
    bool followsDefinitionOf(const WatchedValue&) const;
    bool standsAtHeadOfFramedPartBehind(const WatchedValue&) const;

    void checkSafepoint(const Safepoint&);
    bool checkOperandsOfSuspension(const PatchOperation*);
    bool checkAcross(const Operation*, const Safepoint&);
    bool isRootOf(const PatchOperation* suspension, const Operation* value) const { return m_rootOfSuspension[value->index()] == suspension->index(); }
    bool spillHolds(SpillIndex, const Operation*, bool holdsPointer, const Safepoint&) const;
    bool isHeld(const Operation*, const Safepoint&) const;

    Procedure& m_proc;
    Arena& m_arena;
    const Order& m_order;
    const Liveness& m_liveness;
    const Allocation& m_allocation;
    std::span<const SpillFacts> m_spills;
    RegisterMask m_registersKeptAtSuspensions { 0 };

    ArenaVector<WatchedValue> m_watched;
    struct PointerKeptAtHead {
        uint32_t watched;
        const Block* head;
    };
    ArenaVector<PointerKeptAtHead> m_pointersKeptAtHeads;
    ArenaVector<Read> m_reads;
    ArenaVector<LiveRange> m_liveRanges;
    ArenaVector<const Block*> m_blocksToEnter;
    // By Operation::index().
    ArenaVector<uint32_t> m_watchedOf;
    ArenaVector<SpillIndex> m_saveOf;
    // Operation::index() of the latest suspension passed that has the value as a root.
    ArenaVector<uint32_t> m_rootOfSuspension;
    BitSpan m_isDisplaced;
    BitSpan m_isFrameAddress;
    // By Block::index(). The first is valid only for the value whose live ranges are being computed.
    ArenaVector<uint32_t> m_liveRangeIn;
    ArenaVector<uint32_t> m_firstLiveRangeIn;
    // By SpillIndex.
    BitSpan m_isCleared;

    const Block* m_block { nullptr };
    // By Reg::index().
    const Operation* m_valueIn[numberOfRegisters] { };
    bool m_failed { false };
};

CollectorRulesChecker::CollectorRulesChecker(Procedure& proc)
    : m_proc(proc)
    , m_arena(proc.arena())
    , m_order(proc.order())
    , m_liveness(proc.liveness())
    , m_allocation(proc.allocation())
    , m_spills(proc.allocation().spills())
{
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        m_registersKeptAtSuspensions = maskOf(Reg(SavedFrame::preservedRegister));

    uint32_t numberOfOperations = proc.numberOfOperations();
    m_watchedOf.fill(m_arena, numberOfOperations, none);
    m_saveOf.fill(m_arena, numberOfOperations, noSpill);
    m_rootOfSuspension.fill(m_arena, numberOfOperations, none);
    m_isDisplaced = BitSpan::create(m_arena, numberOfOperations);
    m_isFrameAddress = BitSpan::create(m_arena, numberOfOperations);
    m_liveRangeIn.fill(m_arena, proc.numberOfBlocks(), none);
    m_firstLiveRangeIn.fill(m_arena, proc.numberOfBlocks(), none);
    m_isCleared = BitSpan::create(m_arena, m_spills.size());
}

bool CollectorRulesChecker::needsPointerBeside(const Operation* value) const
{
    return m_proc.traits().usesNarrowReferences && value->isNarrowReference() && !value->has(OperationFlag::NamesPermanentCell) && !value->hasNoPosition();
}

bool CollectorRulesChecker::isFrameAddress(const Operation* value) const
{
    if (!m_proc.traits().savesFrameOnSuspend)
        return false;
    if (!value->hasNoPosition())
        return m_isFrameAddress.get(value->index());
    if (value->opcode() == Add)
        value = value->child(0);
    return value->opcode() == FramePointer || value->opcode() == SlotBase;
}

bool CollectorRulesChecker::isComputedFromFrameAddress(const Operation* value) const
{
    Opcode opcode = value->opcode();
    if (opcode == Add || opcode == Sub)
        return isFrameAddress(value->child(0)) || isFrameAddress(value->child(1));
    if (opcode == Opaque)
        return isFrameAddress(value->child(0));
    if (opcode == Select)
        return isFrameAddress(value->child(1)) || isFrameAddress(value->child(2));
    return false;
}

// What has to be kept for as long as the value is read: the reference that a displaced one is computed from, the owner of an inner pointer.
const Operation* CollectorRulesChecker::keeperOf(const Operation* value) const
{
    if (m_isDisplaced.get(value->index())) {
        for (const Operation* child : value->children()) {
            if (needsPointerBeside(child) || m_isDisplaced.get(child->index()))
                return child;
        }
        return nullptr;
    }
    if (!value->has(OperationFlag::IsInnerPointer) || !value->numChildren())
        return nullptr;

    const Operation* owner = isLoad(value->opcode()) ? value->lastChild() : value->child(0);
    bool isOffsetInCage = false;
    while (m_liveness.isFused(owner) && owner->numChildren()) {
        if (isLoad(owner->opcode()))
            owner = owner->lastChild();
        else if (owner->opcode() == Add && isCageBase(owner->child(0))) {
            isOffsetInCage = true;
            owner = owner->child(1);
        } else
            owner = owner->child(0);
    }

    // The collector does not treat an offset into the cage as a pointer, so a place that holds one keeps no cell alive.
    if (isOffsetInCage && m_proc.traits().usesNarrowReferences) {
        for (const Operation* word = owner; !word->hasNoPosition(); word = word->child(0)) {
            if (word->isNarrowReference())
                return word;
            if (word->opcode() != Shl && word->opcode() != ZExt32 && word->opcode() != SExt32)
                break;
        }
    }
    return owner->hasNoPosition() ? nullptr : owner;
}

void CollectorRulesChecker::findFrameAddresses()
{
    if (!m_proc.traits().savesFrameOnSuspend)
        return;

    // A back edge can bring an address to a Phi after everything that reads the Phi has been passed. Each further round marks at least one such Phi.
    bool markedPhiAlreadyPassed;
    do {
        markedPhiAlreadyPassed = false;
        for (const Block* block : m_order.blocksInOrder()) {
            for (const Operation* operation : block->operations()) {
                if (isComputedFromFrameAddress(operation))
                    m_isFrameAddress.set(operation->index());
            }
            for (const Operation* upsilon : block->upsilons()) {
                const Operation* phi = upsilon->phi();
                if (!phi || m_liveness.isDropped(upsilon) || m_isFrameAddress.get(phi->index()) || !isFrameAddress(upsilon->child(0)))
                    continue;
                m_isFrameAddress.set(phi->index());
                markedPhiAlreadyPassed |= m_order.headOf(phi->owner()) <= m_order.headOf(block);
            }
        }
    } while (markedPhiAlreadyPassed);
}

void CollectorRulesChecker::findWatchedValues()
{
    for (const Block* block : m_order.blocksInOrder()) {
        if (block->isEmpty())
            continue;
        Position head = m_order.headOf(block);
        for (const Operation* phi : block->phis()) {
            if (!m_liveness.isDropped(phi))
                define(phi, block, head);
        }
        for (const Operation* operation : m_liveness.sunkInto(block)) {
            noteReadsOf(operation, block, head);
            define(operation, block, head);
        }
        for (unsigned index = 0; index < block->size(); ++index) {
            const Operation* operation = block->at(index);
            if (!m_liveness.standsInItsBlock(operation))
                continue;
            Position early = m_order.earlyPositionOf(block, index);
            noteReadsOf(operation, block, early);
            define(operation, block, early + 1);
        }
        Position positionOfEdges = m_order.earlyPositionOf(block, block->size() - 1);
        for (const Operation* upsilon : block->upsilons()) {
            const Operation* phi = upsilon->phi();
            if (phi && !m_liveness.isDropped(upsilon) && !m_liveness.isDropped(phi))
                noteRead(upsilon->child(0), block, positionOfEdges);
        }
    }
}

// A definition precedes its reads in the order, so the children have already been defined.
void CollectorRulesChecker::define(const Operation* value, const Block* block, Position definition)
{
    uint32_t index = value->index();
    if ((value->opcode() == Add || value->opcode() == Sub) && !value->isNarrowReference()) {
        for (const Operation* child : value->children()) {
            if (needsPointerBeside(child) || m_isDisplaced.get(child->index()))
                m_isDisplaced.set(index);
        }
    }
    if (!needsPointerBeside(value) && !m_isFrameAddress.get(index) && !keeperOf(value))
        return;
    m_watchedOf[index] = m_watched.size();
    m_watched.append(m_arena, WatchedValue { value, block, definition });
}

void CollectorRulesChecker::noteRead(const Operation* value, const Block* block, Position position)
{
    uint32_t watched = m_watchedOf[value->index()];
    if (watched == none)
        return;
    uint32_t& firstRead = m_watched[watched].firstRead;
    m_reads.append(m_arena, Read { block, position, firstRead });
    firstRead = m_reads.size() - 1;
}

// Whether the word equals a constant does not depend on the cell that it names: no constant names a cell that the collector frees. Nothing gets from such a read to the cell,
// so the cell need not outlive it. `closed === "outage"` behind an await compares the word of the slot with the number of the text, and the widened word is the root.
static bool comparesNarrowReferenceWithConstant(const Operation* user, unsigned childIndex, const Operation* leaf)
{
    if (!leaf->isNarrowReference())
        return false;
    if (user->opcode() == Switch)
        return true;
    return isComparison(user->opcode()) && user->child(!childIndex)->hasNoPosition() && user->child(!childIndex)->hasInt32();
}

void CollectorRulesChecker::noteReadsOf(const Operation* operation, const Block* block, Position position)
{
    forEachLeaf(m_liveness, operation, [&](const Operation* user, unsigned childIndex, const Operation* leaf) {
        if (!comparesNarrowReferenceWithConstant(user, childIndex, leaf))
            noteRead(leaf, block, position);
    });
}

void CollectorRulesChecker::findLiveRangesOf(uint32_t watched)
{
    for (uint32_t read = m_watched[watched].firstRead; read != none; read = m_reads[read].next)
        extendTo(watched, m_reads[read].block, m_reads[read].position);
    while (!m_blocksToEnter.isEmpty()) {
        for (const Block* predecessor : m_order.of(m_blocksToEnter.takeLast()).predecessors)
            extendTo(watched, predecessor, m_order.endOf(predecessor));
    }
}

void CollectorRulesChecker::extendTo(uint32_t watched, const Block* block, Position end)
{
    uint32_t& range = m_liveRangeIn[block->index()];
    if (range != none && m_liveRanges[range].watched == watched) {
        m_liveRanges[range].end = std::max(m_liveRanges[range].end, end);
        return;
    }

    bool isDefinedHere = m_watched[watched].block == block;
    uint32_t& first = m_firstLiveRangeIn[block->index()];
    range = m_liveRanges.size();
    m_liveRanges.append(m_arena, LiveRange { watched, isDefinedHere ? m_watched[watched].definition : m_order.headOf(block), end, first });
    first = range;
    if (!isDefinedHere)
        m_blocksToEnter.append(m_arena, block);
}

void CollectorRulesChecker::enter(const Block* block)
{
    m_block = block;
    for (const Operation*& held : m_valueIn)
        held = nullptr;
    for (const RegisterBinding& binding : m_allocation.registersAtHeadOf(block))
        put(Location::reg(binding.reg), binding.value);
    if (!block->isEntry())
        return;
    for (const Operation* operation : block->operations()) {
        if (operation->opcode() == ArgumentReg && m_liveness.standsInItsBlock(operation))
            put(Location::reg(operation->argumentReg()), operation);
    }
}

void CollectorRulesChecker::passSteps(uint32_t begin, uint32_t end)
{
    std::span<const Step> steps = m_allocation.steps();
    end = std::min(end, static_cast<uint32_t>(steps.size()));
    for (uint32_t index = begin; index < end && !m_failed; ++index)
        pass(steps[index]);
}

// Only a step there runs on every path to every point where the reference is live.
bool CollectorRulesChecker::followsDefinitionOf(const WatchedValue& watched) const
{
    if (watched.block == m_block)
        return true;
    return watched.value->endsBlock() && watched.block->numSuccessors() && watched.block->successor(0).block() == m_block;
}

bool CollectorRulesChecker::standsAtHeadOfFramedPartBehind(const WatchedValue& watched) const
{
    return m_allocation.isHeadOfFramedPart(m_block) && !m_allocation.isInFramedPart(watched.block);
}

void CollectorRulesChecker::pass(const Step& step)
{
    switch (step.kind) {
    case StepKind::Execute:
        execute(step);
        return;
    case StepKind::Move:
        put(step.destination, valueIn(step.sources[0]));
        return;
    case StepKind::Reload:
    case StepKind::Materialize:
        put(step.destination, step.operation);
        return;
    case StepKind::Swap: {
        const Operation* endsInSource = valueIn(step.destination);
        put(step.destination, valueIn(step.sources[0]));
        put(step.sources[0], endsInSource);
        return;
    }
    case StepKind::Save: {
        SpillIndex spill = step.destination.isSpill() ? step.destination.spillIndex() : noSpill;
        if (spill < m_spills.size() && m_spills[spill].isSaveInRareRegion && step.operation && valueIn(step.sources[0]) == step.operation)
            m_saveOf[step.operation->index()] = spill;
        return;
    }
    case StepKind::ClearSpill:
        if (step.destination.isSpill() && step.destination.spillIndex() < m_spills.size())
            m_isCleared.set(step.destination.spillIndex());
        return;
    case StepKind::KeepPointer: {
        uint32_t watched = step.operation ? m_watchedOf[step.operation->index()] : none;
        if (watched == none || !step.destination.isSpill())
            return;
        bool isKeptInFramedPartAlone = standsAtHeadOfFramedPartBehind(m_watched[watched]);
        if (followsDefinitionOf(m_watched[watched]) || isKeptInFramedPartAlone) {
            m_watched[watched].pointerBeside = step.destination.spillIndex();
            m_watched[watched].pointerIsKeptInFramedPartAlone = isKeptInFramedPartAlone;
            if (isKeptInFramedPartAlone)
                m_pointersKeptAtHeads.append(m_arena, PointerKeptAtHead { watched, m_block });
        }
        return;
    }
    }
}

void CollectorRulesChecker::execute(const Step& step)
{
    const Operation* operation = step.operation;
    if (!operation)
        return;

    const PatchOperation* patch = operation->as<PatchOperation>();
    // A Check writes them on its late path alone.
    RegisterMask overwritten = patch && patch->opcode() == Patch ? patch->earlyClobberedMask() | patch->lateClobberedMask() : 0;
    std::span<const Location> results;
    if (step.patchAssignment != UINT32_MAX) {
        const PatchAssignment& assignment = m_allocation.patchAssignment(step.patchAssignment);
        overwritten |= assignment.gpScratch | assignment.fpScratch;
        if (patch) {
            results = assignment.locations.first(std::min<size_t>(m_proc.resultCount(patch->type()), assignment.locations.size()));
            // Its generator may write every other register.
            if (patch->opcode() == Patch && !patch->onlyKeepsChildrenAlive)
                overwritten |= ~assignment.usedRegisters;
        }
    }
    if (patch && patch->isSuspension())
        overwritten |= ~m_registersKeptAtSuspensions;
    for (unsigned index = 0; index < numberOfRegisters; ++index) {
        if (overwritten >> index & 1)
            m_valueIn[index] = nullptr;
    }

    if (!patch) {
        put(step.destination, operation);
        return;
    }
    // The member of a tuple is the value of its Extract, which has a step of its own.
    for (Location result : results)
        put(result, patch->type().isTuple() ? nullptr : patch);

    if (patch->opcode() != Patch || (!patch->effects.mayCollect && !patch->isSuspension()))
        return;
    bool isAtHead = patch->owner() != m_block || m_liveness.isSunk(patch);
    checkSafepoint({ patch, (isAtHead ? m_order.headOf(m_block) : m_order.earlyPositionOf(patch)) + 1 });
}

void CollectorRulesChecker::checkSafepoint(const Safepoint& safepoint)
{
    if (safepoint.patch->isSuspension() && !checkOperandsOfSuspension(safepoint.patch))
        return;

    // The list rises by begin, and the safepoints of a block are passed in rising order. So a range that has ended is never needed again.
    uint32_t* link = &m_firstLiveRangeIn[m_block->index()];
    while (*link != none) {
        LiveRange& range = m_liveRanges[*link];
        if (range.begin >= safepoint.late)
            return;
        if (range.end <= safepoint.late) {
            *link = range.nextInBlock;
            continue;
        }
        if (!checkAcross(m_watched[range.watched].value, safepoint))
            return;
        link = &range.nextInBlock;
    }
}

// The generator may read a late operand after the function has resumed in another frame.
bool CollectorRulesChecker::checkOperandsOfSuspension(const PatchOperation* suspension)
{
    for (unsigned child = 0; child < suspension->numberOfOperands(); ++child) {
        const Operation* operand = suspension->operands()[child];
        const ValueRep& constraint = suspension->constraintOf(child);
        if (!constraint.isLate() || operand->hasNoPosition())
            continue;
        if (isFrameAddress(operand))
            return fail(FailureKind::FrameAddressAcrossSuspension, operand);
        if (constraint.kind() == ValueRep::LateColdAny)
            m_rootOfSuspension[operand->index()] = suspension->index();
    }
    return true;
}

bool CollectorRulesChecker::checkAcross(const Operation* value, const Safepoint& safepoint)
{
    bool isSuspension = safepoint.patch->isSuspension();
    if (isSuspension && m_isFrameAddress.get(value->index()))
        return fail(FailureKind::FrameAddressAcrossSuspension, value);

    // A pointer between an inner pointer and its cell keeps nothing alive by itself.
    const Operation* keeper = value;
    while (const Operation* next = keeperOf(keeper))
        keeper = next;

    bool isKept;
    if (needsPointerBeside(keeper)) {
        // The pointer keeps the cell alive while the frame is on the stack. A saved frame is visited by its map, which lists the roots and not the pointer.
        uint32_t watched = m_watchedOf[keeper->index()];
        bool hasPointer = watched != none && spillHolds(m_watched[watched].pointerBeside, keeper, true, safepoint)
            && (!m_watched[watched].pointerIsKeptInFramedPartAlone || (m_allocation.isInFramedPart(m_block) && std::ranges::any_of(m_pointersKeptAtHeads, [&](const PointerKeptAtHead& kept) {
                return kept.watched == watched && kept.head == m_allocation.headOfFramedPartAbove(m_block).block;
            })));
        isKept = (hasPointer || !safepoint.patch->effects.mayCollect) && (!isSuspension || isRootOf(safepoint.patch, keeper));
    } else
        isKept = keeper == value || keeper->has(OperationFlag::NamesPermanentCell) || isHeld(keeper, safepoint);
    if (isKept)
        return true;
    return fail(value->has(OperationFlag::IsInnerPointer) ? FailureKind::InnerPointerWithoutOwner : FailureKind::NarrowReferenceWithoutPointer, value);
}

bool CollectorRulesChecker::spillHolds(SpillIndex spill, const Operation* value, bool holdsPointer, const Safepoint& safepoint) const
{
    if (spill >= m_spills.size() || m_isCleared.get(spill))
        return false;
    const SpillFacts& facts = m_spills[spill];
    if (facts.value != value || facts.isPointerBesideNarrowReference != holdsPointer || facts.begin > safepoint.late || safepoint.late > facts.end)
        return false;
    if (!safepoint.patch->isSuspension())
        return true;
    std::span<PatchOperation* const> suspensions = m_allocation.suspensions();
    auto listed = std::partition_point(suspensions.begin(), suspensions.end(), [&](const PatchOperation* candidate) {
        return m_order.latePositionOf(candidate) < safepoint.late;
    });
    return listed != suspensions.end() && *listed == safepoint.patch && m_allocation.spillsHeldAt(static_cast<uint32_t>(listed - suspensions.begin())).get(spill);
}

bool CollectorRulesChecker::isHeld(const Operation* value, const Safepoint& safepoint) const
{
    for (const Operation* held : m_valueIn) {
        if (held == value)
            return true;
    }
    return spillHolds(m_allocation.spillOf(value), value, false, safepoint) || spillHolds(m_saveOf[value->index()], value, false, safepoint);
}

bool CollectorRulesChecker::run()
{
    findFrameAddresses();
    findWatchedValues();
    // Values are watched in rising order of definition, and extendTo() adds at the front. Taken from the last value to the first, each block's list rises by begin.
    for (uint32_t watched = m_watched.size(); watched--;)
        findLiveRangesOf(watched);
    if (m_liveRanges.isEmpty())
        return true;

    for (const Block* block : m_order.blocksInOrder()) {
        const BlockSteps& range = m_allocation.stepsOf(block);
        enter(block);
        passSteps(range.begin, range.end);
        passSteps(range.beginOfEdgeMoves, range.endOfEdgeMoves);
        passSteps(range.terminator, range.terminator + 1);
        if (m_failed)
            return false;
    }
    return true;
}

bool checkReferences(Procedure& proc)
{
    CollectorRulesChecker checker(proc);
    return checker.run();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
