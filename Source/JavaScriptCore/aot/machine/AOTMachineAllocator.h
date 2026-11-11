/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

enum class KindOfHeadBeingFixed : uint8_t;

class Allocator {
    WTF_MAKE_NONCOPYABLE(Allocator);
public:
    Allocator(Procedure&, Allocation&);
    void run();
    void letGoOfWorkingLists();

    struct ValueState {
        RegisterMask registers { 0 };
        uint32_t nextUse { 0 };
        SpillIndex saveInRareRegion { noSpill };
    };

    struct Snapshot {
        bool isSet() const { return first; }
        std::span<const RegisterBinding> bindings() const { return { first, static_cast<size_t>(std::popcount(registers)) }; }
        void set(const RegisterBinding* bindingsInOrderOfRegisters, RegisterMask registersBound)
        {
            ASSERT(bindingsInOrderOfRegisters);
            first = bindingsInOrderOfRegisters;
            registers = registersBound;
        }

        const RegisterBinding* first { nullptr };
        RegisterMask registers { 0 };
    };
    static_assert(sizeof(Snapshot) == 16);

    void allocateBlock(Block*);
    void allocateOperation(Operation*);
    template<bool mayBePatch> void allocateByKindOfOpcode(Operation*);
    void allocateOperationTheLongWay(Operation*);
    void allocateOperationWithAnyLeaves(Operation*);
    template<unsigned numberOfChildren, unsigned traitsOfKind> void allocateOperationOfKind(Operation*);
    template<unsigned numberOfLeaves, unsigned traitsOfKind> bool allocateWithChildrenForLeaves(Operation*, const Operation* user, bool userIsFusedLoad);
    bool defineResultBehindLeaves(Operation*, unsigned traits, unsigned fusedChildren, RegisterMask operandRegisters, Location& firstSource, Location secondSource, Position, Location& destination);
    void allocateArgumentRegister(Operation*);
    void allocateBranch(Operation*);
    void allocateBranchWithAnyLeaves(Operation*);
    void allocateTerminator(Block*, Operation*);
    Location placeLeaf(const Operation* user, unsigned childIndex, Operation* leaf, RegisterMask& locked, bool& hasMemoryOperand);
    void passUsesOf(const Operation*, Position);
    void passUse(Operation* value, Position);
    ALWAYS_INLINE void passUseReachedByWalk(Operation* value, Position position) { passUseReachedByWalk(value, stateOf(value), m_valueFacts[value->index()], position); }
    ALWAYS_INLINE void passUseReachedByWalk(Operation* value, ValueState& state, const ValueFacts& facts, Position position)
    {
        ASSERT(position <= m_position + 1);
        ASSERT(&state == &stateOf(value) && &facts == &m_valueFacts[value->index()]);
        const Use* uses = m_uses;
        uint32_t nextUse = state.nextUse;
        const Use* next = uses + nextUse;
        if (next->position <= position) {
            do {
                nextUse = next->next;
                next = uses + nextUse;
            } while (next->position <= position);
            state.nextUse = nextUse;
        }
        const Position positionOfNext = next->position;
        if (positionOfNext < m_endOfBlock)
            return;
        const RegisterMask registers = state.registers;
        if (!registers && !value->has(OperationFlag::IsInnerPointer))
            return;

        bool rangeGoesOn;
        bool noWayLeadsToNextRead;
        if (nextUse) {
            noWayLeadsToNextRead = positionOfNext >= m_endOfWhatWaysLeadToFromBlock;
            if (!noWayLeadsToNextRead && m_everyReadBehindBlockCounts) {
                const Use* read = uses + findReadFromAmongLaterReads(value, nextUse, m_beginOfWaysOnFromBlock);
                if (read->position < m_endOfWhatWaysLeadToFromBlock && read->greatestHeadOfBlockPassedOnEveryPath >= m_headOfBlock)
                    return;
            }
            rangeGoesOn = true;
        } else {
            rangeGoesOn = static_cast<Position>(facts.lastUse + 1) > position + 1;
            noWayLeadsToNextRead = m_endOfWhatWaysLeadToFromBlock != noPosition;
        }
        if (rangeGoesOn) {
            if (isInRareRegion())
                return;
            if (noWayLeadsToNextRead) {
                if (facts.definition > position + 1 || facts.has(ValueFlag::IsKeptForCollector))
                    return;
            } else {
                if (facts.definition == noPosition)
                    return;
                if (facts.definition < m_beginOfLoopAroundBlock && position >= m_headOfBlock)
                    return;
                if (!isDeadOnEveryPathFrom(value, m_block, position + 1))
                    return;
            }
        }
        if (edgeMovesOfBlockMayRead(value, position))
            return;
        if (isKeptThroughLoopAround(value, position))
            return;
        if (registers)
            unbindAll(value, state);

        if (value->has(OperationFlag::IsInnerPointer)) [[unlikely]]
            passReadOfOwnerOfInnerPointer(value, position);
    }
    ALWAYS_INLINE bool edgeMovesOfBlockMayRead(const Operation* value, Position position) const
    {
        if (position < m_endOfBlock - 2)
            return false;
        for (const Operation* upsilon : m_block->upsilons()) {
            if (upsilon->valueHandedOn() == value && !m_valueFacts[upsilon->index()].has(ValueFlag::IsDropped))
                return true;
        }
        return false;
    }
    void passReadOfOwnerOfInnerPointer(const Operation* innerPointer, Position);
    Location defineResult(Operation*, RegisterMask allowed, RegisterMask avoided, Reg preferred);
    Location defineResultWithFacts(Operation*, const ValueFacts&, RegisterMask allowed, RegisterMask avoided, Reg preferred);
    ALWAYS_INLINE Step& appendStep(StepKind kind, Operation* operation)
    {
        Allocation& allocation = m_allocation;
        allocation.m_numberOfOperationsAllocated += kind == StepKind::Execute;
        Step* step = new (NotNull, allocation.m_steps.appendUninitialized(m_arena)) Step;
        step->kind = kind;
        step->operation = operation;
        if (kind != StepKind::Execute && operation && operation->type().isNumeric())
            step->width = widthOf(operation->type());
        return *step;
    }
    ALWAYS_INLINE void appendStep(const Step& step)
    {
        Allocation& allocation = m_allocation;
        allocation.m_numberOfOperationsAllocated += step.kind == StepKind::Execute;
        new (NotNull, allocation.m_steps.appendUninitialized(m_arena)) Step(step);
    }
    ALWAYS_INLINE void appendExecute(Operation* operation, Location destination, const Location* sources)
    {
        Allocation& allocation = m_allocation;
        ++allocation.m_numberOfOperationsAllocated;
        Step* step = allocation.m_steps.appendUninitialized(m_arena);
        step->kind = StepKind::Execute;
        step->width = Width64;
        step->patchAssignment = UINT32_MAX;
        step->destination = destination;
        std::copy_n(sources, maxLeaves, step->sources);
        step->operation = operation;
        step->other = nullptr;
    }
    ALWAYS_INLINE void appendExecute(Operation* operation, Location destination, Location firstSource, Location secondSource)
    {
        Allocation& allocation = m_allocation;
        ++allocation.m_numberOfOperationsAllocated;
        Step* step = allocation.m_steps.appendUninitialized(m_arena);
        step->kind = StepKind::Execute;
        step->width = Width64;
        step->patchAssignment = UINT32_MAX;
        step->destination = destination;
        step->sources[0] = firstSource;
        step->sources[1] = secondSource;
        std::fill_n(step->sources + 2, maxLeaves - 2, Location());
        step->operation = operation;
        step->other = nullptr;
    }
    ALWAYS_INLINE void notePatchWithOperandInFrame(const PatchOperation* patch)
    {
        m_allocation.m_holdsPatchWithOperandInFrame = true;
        if (!patch->findsOperandsInFrameWithoutRecord())
            m_allocation.m_holdsPatchThatCountsOnFrameRecord = true;
    }
    ALWAYS_INLINE void noteFrameUse()
    {
        if (isInRareRegion()) {
            m_allocation.m_regionNeedsFrame.set(m_rareRegion);
            return;
        }
        m_allocation.m_needsFrameOutsideRareRegions = true;
        m_allocation.m_needsFrameAheadOfFramedPart |= m_isOutsideFramedPart;
    }
    void enterFramedPart();
    void keepPointersThatWait(Position endOfWhatWaysLeadTo);
    Position endOfWhatWaysLeadToFromSafepointsOfRareRegion() const;
    Position position() const { return m_position; }
    bool isInRareRegion() const { return m_rareRegion != noRareRegion; }

    static constexpr uint16_t bitOf(ValueFlag flag) { return static_cast<uint16_t>(flag); }
    static ALWAYS_INLINE Reg lowestRegisterIn(RegisterMask mask)
    {
        ASSERT(mask);
        return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(mask)));
    }
    ValueState& stateOf(const Operation* value) { return m_values[value->index()]; }
    ALWAYS_INLINE Position nextUseOf(const Operation* value) const
    {
        for (uint32_t use = m_values[value->index()].nextUse; use; use = m_uses[use].next) {
            Position position = m_uses[use].position;
            if (position >= m_position)
                return position;
        }

        Position lastUse = m_valueFacts[value->index()].lastUse;
        if (lastUse != noPosition && lastUse >= m_position)
            return lastUse;
        return isKeptThroughLoopAround(value, m_position) ? m_rematerializedValueIsKeptUntil[m_hints[value->index()].index()] : noPosition;
    }
    ALWAYS_INLINE uint32_t findReadFrom(const Operation* value, Position position) const
    {
        uint32_t use = m_values[value->index()].nextUse;
        for (unsigned stepsLeft = 64; use && m_liveness.use(use).position < position; use = m_liveness.use(use).next) {
            if (!--stepsLeft) [[unlikely]]
                return findReadFromInList(value, position);
        }
        return use;
    }
    uint32_t findReadFromInList(const Operation* value, Position) const;
    uint32_t findReadOutsideUnlikelyRegions(const Operation* value, Position) const;
    ALWAYS_INLINE uint32_t findReadFromAmongLaterReads(const Operation* value, uint32_t use, Position position) const
    {
        const Use* const uses = m_uses;
        for (unsigned stepsLeft = 64; uses[use].position < position; use = uses[use].next) {
            if (!--stepsLeft) [[unlikely]]
                return findReadFromInList(value, position);
        }
        return use;
    }
    ALWAYS_INLINE bool isLiveBehind(const Operation* value, Position position) const
    {
        Position lastUse = m_valueFacts[value->index()].lastUse;
        return lastUse != noPosition && lastUse > position;
    }
    ALWAYS_INLINE Reg anyRegisterOf(const Operation* value, RegisterMask allowed) const
    {
        RegisterMask holding = m_values[value->index()].registers & allowed;
        return holding ? lowestRegisterIn(holding) : Reg();
    }
    ALWAYS_INLINE void bind(Reg reg, Operation* value)
    {
        ASSERT(reg);
        ASSERT(value);
        RegisterMask bit = maskOf(reg);
        ASSERT(bit & m_file.allocatableIn(bankOf(reg)));
        ASSERT(!value->type().isNumeric() || bankOf(value->type()) == bankOf(reg));

        if (m_valueIn[reg.index()]) [[unlikely]] {
            bindRegisterThatIsBound(reg, value);
            return;
        }

        m_valueIn[reg.index()] = value;
        m_bound |= bit;
        stateOf(value).registers |= bit;
        noteCalleeSaveUse(reg);
    }
    void bindRegisterThatIsBound(Reg, Operation*);
    ALWAYS_INLINE void unbind(Reg reg)
    {
        Operation* value = m_valueIn[reg.index()];
        if (!value)
            return;
        RegisterMask bit = maskOf(reg);
        ASSERT(m_bound & bit);
        ASSERT(stateOf(value).registers & bit);
        stateOf(value).registers &= ~bit;
        m_valueIn[reg.index()] = nullptr;
        m_bound &= ~bit;
    }
    ALWAYS_INLINE void unbindAll(Operation* value) { unbindAll(value, stateOf(value)); }
    ALWAYS_INLINE void unbindAll([[maybe_unused]] Operation* value, ValueState& state)
    {
        RegisterMask held = state.registers;
        for (RegisterMask registers = held; registers; registers &= registers - 1) {
            unsigned index = static_cast<unsigned>(std::countr_zero(registers));
            ASSERT(m_valueIn[index] == value);
            m_valueIn[index] = nullptr;
        }
        ASSERT((m_bound & held) == held);
        m_bound &= ~held;
        state.registers = 0;
    }
    ALWAYS_INLINE RegisterMask registersThatMayBeTaken(Bank bank, RegisterMask allowed) const
    {
        RegisterMask registers = allowed & m_file.allocatableIn(bank);
        if (m_isOutsideFramedPart || isInRareRegion())
            registers &= ~m_file.calleeSaved | calleeSavesThatMayBeTaken();
        return registers;
    }
    ALWAYS_INLINE Reg chooseFreeRegisterWithoutNewCalleeSave(RegisterMask freeRegisters, Reg preferred) const
    {
        if (preferred && (freeRegisters & maskOf(preferred)))
            return preferred;
        if (RegisterMask savedByNoCallee = freeRegisters & ~m_file.calleeSaved)
            return lowestRegisterIn(savedByNoCallee);
        if (RegisterMask savedAlready = freeRegisters & m_allocation.usedCalleeSaves())
            return lowestRegisterIn(savedAlready);
        return Reg();
    }
#if ASSERT_ENABLED
    bool registerFileIsConsistent() const;
#endif
    ALWAYS_INLINE Reg takeRegister(Bank bank, RegisterMask allowed, Reg preferred = Reg())
    {
        ASSERT(registerFileIsConsistent());

        Reg chosen = chooseFreeRegisterWithoutNewCalleeSave(registersThatMayBeTaken(bank, allowed) & ~m_bound, preferred);
        if (!chosen) [[unlikely]]
            return takeRegisterAfterSearch(bank, allowed, preferred);

        ASSERT(!m_valueIn[chosen.index()]);
        noteCalleeSaveUse(chosen);
        return chosen;
    }
    Reg takeRegisterAfterSearch(Bank, RegisterMask allowed, Reg preferred);
    ALWAYS_INLINE bool hasFreeRegister(Bank bank, RegisterMask allowed) const
    {
        RegisterMask candidates = registersThatMayBeTaken(bank, allowed);
        return (candidates & ~m_bound) || canDropSomeRegisterForFree(candidates);
    }
    bool canDropSomeRegisterForFree(RegisterMask candidates) const;
    void evict(Reg);
    ALWAYS_INLINE Reg ensureInRegister(Operation* value, RegisterMask allowed, Reg preferred = Reg())
    {
        allowed &= m_file.allocatableIn(bankOf(value->type()));
        if (RegisterMask holding = stateOf(value).registers & allowed)
            return preferred && (holding & maskOf(preferred)) ? preferred : lowestRegisterIn(holding);
        return bringIntoRegister(value, allowed, preferred);
    }
    Reg bringIntoRegister(Operation* value, RegisterMask allowed, Reg preferred);
    SpillIndex markSpilled(Operation* value);
    SpillIndex addSpill(const SpillFacts&);
    ALWAYS_INLINE void noteCalleeSaveUse(Reg reg)
    {
        RegisterMask bit = maskOf(reg) & m_file.calleeSaved;
        if (!bit)
            return;
        m_allocation.m_usedCalleeSaves |= bit;
        if (!isInRareRegion())
            m_calleeSavesTakenOutsideRareRegions |= bit;
        m_allocation.m_needsFrameOutsideRareRegions = true;
        if (m_isOutsideFramedPart)
            noteFrameUse();
    }
    ALWAYS_INLINE Reg hintOf(const Operation* value) const
    {
        Reg hint = m_hints[value->index()];
        return hint ? hint : m_valueFacts[value->index()].hint;
    }

    RegisterMask calleeSavesWorthTakingFor(const Operation* value, Bank, const Block*) const;

    bool passThroughBlockThatOnlyJumps(Block*);
    void enterBlock(Block*);
    void leaveBlock(Block*);
    void fixHeadOf(Block* target, Block* from);
    void fixHeadOf(Block* target, Block* from, const BlockPlace& placeOfTarget, KindOfHeadBeingFixed);
    void appendEdgeMoves(Block* from, unsigned successorIndex, const Snapshot& endOfFrom);
    bool edgeMayChangeFrame(const Block* from, const Block* target);
    Block* splitEdge(Block* from, unsigned successorIndex, const Snapshot& endOfFrom);
    Position findFirstReadFromHeadOf(const Block*, const Operation* value);
    bool firstCallBehindHeadReadsValueElsewhere(const Block* target, Reg, const Operation* value);
    bool listsRegistersAtHeadOf(const Block* block) const { return m_allocation.recordsWhatCheckersRead() || m_order.of(block).isRare() || m_allocation.isHeadOfFramedPart(block); }
    ArenaVector<std::span<const RegisterBinding>>& registersAtHeads() { return m_allocation.m_registersAtHead; }
    Snapshot takeSnapshot();
    void restore(const Snapshot&);
    void noteHintsOfHead(const Block* target);
    bool hasBackEdgeOutsideRareRegions(const Loop&) const;
    const Block* immediateDominatorOf(const Block*);
    bool dominates(const Block* above, const Block* below);
    struct OperandTakenBack {
        Reg reg;
        Operation* value { nullptr };
    };
    OperandTakenBack findOperandThatSumMayOverwrite(const Operation* sum, std::span<const Location> sources);
    void noteOperandTakenBack(const Operation* sum, OperandTakenBack, Location otherOperand);

    void chooseRegistersKeptThroughLoop(const Loop&, bool movesMayWriteRegisters);
    const Block* headOfLoopThatBlockOnlyLeadsBackTo(const Block*) const;
    void dropWhatBlockThatLeadsBackNeitherReadsNorHandsOn(const Block*, const Block* headOfLoop);
    void holdWhatHeadOfLoopHolds(const Block*, const Block* headOfLoop, Position limitOfDefinitions);
    Reg takeRegisterOfValueThatLoopReadsBehindPhi(const Loop&, const Operation* phi);
    bool isReadInside(const Operation* value, const Loop&) const;
    void bringInWhatLoopReads(const Loop&, Position limitOfDefinitions);
    bool isKeptThroughLoopAround(Reg, const Operation* value, Position) const;
    bool isKeptThroughLoopAround(const Operation* value, Position position) const { return position < m_endOfLastLoopThatKeepsValueMadeAgain && isKeptThroughLoopAround(m_hints[value->index()], value, position); }
    bool isDeadOnEveryPathFrom(const Operation* value, const Block*, Position from);
    ALWAYS_INLINE Position beginOfWaysOnFrom(const Block* block, Position endOfBlock) const
    {
        if (block->numSuccessors() != 1)
            return endOfBlock;
        return std::max(endOfBlock, m_order.headOf(block->successor(0).block()));
    }

    void allocatePatch(PatchOperation*);
    void placeFixedOperands(PatchOperation*, std::span<Location> locationsOfOperands, RegisterMask& locked);
    void vacate(RegisterMask clobbered, Position behind);
    RegisterMask takeScratchRegisters(Bank, unsigned count, RegisterMask& locked);
    void findBlocksThatLeadToReadsOf(const Operation* value, uint32_t firstBlock);
    uint32_t firstBlockOnWaysFrom(const Block*, const Operation* value);
    bool isReadOnSomeWayFromHeadOf(const Operation* value, const Block*);
    bool dominatesSomeReadBelowDefinitionOf(const Block*, const Operation* value);

    void enterRareRegion(uint32_t region);
    void leaveRareRegion();
    void saveInRareRegion(Operation* value, Reg from);
    bool wayHasSavedInRareRegion(const Block*, const Operation* value) const;
    void giveUpFramedPartForReadOfSpill() const;
    bool isDefinedOutsideRareRegion(const Operation* value) const;
    Location memoryLocationOf(const Operation* value) const;
    void prepareSuspension(PatchOperation*);
    void findSpillsHeldAtSuspensions();
    void findGapsAheadOfLandingPads();

    RegisterMask registersWhereCallInBlockTakesWhatLiesThere(const Block*) const;
    unsigned numberOfReadsThatWordWouldReload(const Operation*, Position end) const;
    void findFramedPart();
    static bool hasImplicitReader(const ValueFacts& facts) { return facts.has(ValueFlag::LivesIntoLandingPad) || facts.has(ValueFlag::LivesAcrossSuspension) || facts.has(ValueFlag::IsKeptForCollector); }
    bool hasImplicitReader(const SpillFacts&) const;
    void findDeadSlots();
    ALWAYS_INLINE Location placeToReadIn(SpillIndex spill) const
    {
        m_allocation.m_spills[spill].isRead = true;
        return Location::spill(spill);
    }
    void settleFramedPart();
    void standIn(const Block* block)
    {
        m_rareRegion = m_order.of(block).rareRegion;
        m_isOutsideFramedPart = isOutsideFramedPart(block);
    }
    bool isOutsideFramedPart(const Block* block) const { return m_allocation.hasFramedPart() && !m_allocation.isInFramedPart(block); }
    bool startsOutsideFramedPart(const Block* block) const { return m_allocation.hasFramedPart() && (!m_allocation.isInFramedPart(block) || m_allocation.isHeadOfFramedPart(block)); }
    bool isStoredAtHeadsOfFramedPart(const Operation* value) const
    {
        if (!m_allocation.hasFramedPart() || m_allocation.needsFrameAheadOfFramedPart() || value->hasNoPosition() || m_liveness.isSunk(value))
            return false;
        const Block* blockOfStore = value->endsBlock() ? value->owner()->successor(0).block() : value->owner();
        return !m_allocation.isInFramedPart(blockOfStore) && !m_order.of(blockOfStore).isRare();
    }
    ALWAYS_INLINE bool spillHoldsValueIn(const Block* block, const Operation* value) const { return m_allocation.isSpilled(value) && !(isOutsideFramedPart(block) && isStoredAtHeadsOfFramedPart(value)); }
    ALWAYS_INLINE bool spillHoldsValueAtHeadOf(const Block* block, const Operation* value) const { return m_allocation.isSpilled(value) && !(startsOutsideFramedPart(block) && isStoredAtHeadsOfFramedPart(value)); }
    RegisterMask calleeSavesThatMayBeTaken() const
    {
        if (m_isOutsideFramedPart)
            return 0;
        return isInRareRegion() ? m_calleeSavesTakenOutsideRareRegions : m_file.calleeSaved;
    }
    bool leavesValuesAcrossCallsToFramedPart() const { return m_isOutsideFramedPart && !isInRareRegion(); }
    bool pointerWaitsForFramedPart(Operation* reference);

    Procedure& m_proc;
    Arena& m_arena;
    const Order& m_order;
    const Liveness& m_liveness;
    Allocation& m_allocation;
    const RegisterFile m_file;
    const ValueFacts* m_valueFacts { nullptr };
    const Use* m_uses { nullptr };
    uint64_t m_smallestPinnedConstant { UINT64_MAX };

    Operation* m_valueIn[64];
    RegisterMask m_bound { 0 };
    ArenaVector<ValueState> m_values;
    ArenaVector<Snapshot> m_heads;
    ArenaVector<Snapshot> m_ends;
    struct WaitingEdge {
        Block* from;
        uint32_t successorIndex;
        uint32_t next;
    };
    ArenaVector<WaitingEdge> m_waitingEdges;
    struct SaveReadByWaitingEdge {
        const Block* from;
        const Operation* value;
        SpillIndex save;
    };
    ArenaVector<SaveReadByWaitingEdge> m_savesReadByWaitingEdges;
    struct WaitingEdgesOfHead {
        uint32_t first { 0 };
        uint32_t last { 0 };
    };
    ArenaVector<WaitingEdgesOfHead> m_waitingEdgesOfHeads;
    BitSpan m_isAskedWhetherFramedPartLeadsTo;
    BitSpan m_framedPartLeadsTo;
    ArenaVector<const Block*> m_immediateDominators;
    const uint32_t* m_numbersOfDominators { nullptr };
    ArenaVector<uint32_t> m_nearestDominatorsWithCall[numberOfBanks];
    struct NumbersOfDominatedBlocks {
        uint32_t first { 0 };
        uint32_t last { 0 };
    };
    ArenaVector<NumbersOfDominatedBlocks> m_numbersOfDominatedBlocks;
    struct NumbersOfReadingBlocks {
        uint32_t least { 0 };
        uint32_t greatest { 0 };
    };
    ArenaVector<NumbersOfReadingBlocks> m_numbersOfBlocksThatReadFrom;
    ArenaVector<Operation*> m_resultsDefinedAtEndOfPredecessor;

    Block* m_block { nullptr };
    const Block* m_blockWhoseEndRegistersHold { nullptr };
    Position m_position { 0 };
    Position m_headOfBlock { 0 };
    Position m_endOfBlock { 0 };
    bool m_everyReadBehindBlockCounts { false };
    Position m_beginOfLoopAroundBlock { 0 };
    Position m_endOfWhatWaysLeadToFromBlock { noPosition };
    Position m_beginOfWaysOnFromBlock { 0 };
    bool m_somePatchWithResultHasEndedBlock { false };
    Position m_lastDefinitionOfArgumentRegister { 0 };
    RegisterMask m_argumentRegistersThatGiveWay { 0 };
    uint32_t m_rareRegion { noRareRegion };
    RegisterMask m_calleeSavesTakenOutsideRareRegions { 0 };
    bool m_isOutsideFramedPart { false };
    struct SafepointsAheadOfFramedPart {
        Position first;
        Position last;
        Position until;
    };
    ArenaVector<SafepointsAheadOfFramedPart> m_safepointsAheadOfFramedPart;
    ArenaVector<Position> m_untilByRareRegion;
    struct ReferenceWhosePointerWaits {
        Operation* reference;
        SpillIndex pointer { noSpill };
    };
    ArenaVector<ReferenceWhosePointerWaits> m_referencesWhosePointersWaitForFramedPart;
    ArenaVector<Operation*> m_valuesSavedInRareRegion;
    Position m_endOfRareRegion { 0 };
    ArenaVector<PendingMove> m_pendingMoves;
    using PhisInSpill = ArenaVector<Operation*>;
    ArenaVector<PhisInSpill*> m_phisInSpills;
    PhisInSpill& phisInSpill(SpillIndex);
    SpillIndex findSpillOfDeadSource(const Operation* phi);
    void letEdgesWritePhi(Operation* phi, SpillIndex, const BlockPlace& placeOfHead);
    ArenaVector<Reg> m_hints;
    ArenaVector<Position> m_knownReadsOutsideRareRegions;
    struct SpillWrittenInBlock {
        SpillIndex spill;
        const Block* block;
    };
    ArenaVector<SpillWrittenInBlock> m_spillsWrittenInBlocks;
    mutable ArenaVector<uint8_t> m_whetherValuesThatRareRegionsReadAreWorthCalleeSaveNotYetSaved;
    struct WhetherHeadAroundHolds {
        uint32_t loopWhoseHeadHolds { noLoop };
        uint32_t loopFromWhichNoHeadHolds { noLoop };
        RegisterMask registersHeld { 0 };
        RegisterMask registersNotHeld { 0 };
    };
    ArenaVector<WhetherHeadAroundHolds> m_whetherHeadsAroundHold;
    BitSpan m_hasPassedOnHintsOfPhis;
    ArenaVector<uint32_t> m_firstReadsNotKnownOutOfReach;
    uint64_t m_laterUsesScanned { 0 };
    ArenaVector<uint32_t> m_lastReadsThatCounted;
    struct FirstReadFromHead {
        Position head { noPosition };
        Position read { noPosition };
    };
    ArenaVector<FirstReadFromHead> m_firstReadsFromHeads;
    mutable ArenaVector<uint32_t> m_numbersOfListsOfReads;
    mutable ArenaVector<std::span<const uint32_t>> m_listsOfReads;
    mutable ArenaVector<uint32_t> m_numbersOfListsOutsideRareRegions;
    mutable ArenaVector<std::span<const uint32_t>> m_listsOutsideRareRegions;
    ArenaVector<uint32_t> m_lastSearchThatReachedBlock;
    ArenaVector<const Block*> m_blocksReachedBySearchForWays;
    uint32_t m_numberOfSearchesForWays { 0 };
    size_t m_livenessSearchBlocksVisited { 0 };
    const Operation* m_valueOfLastSearchForWays { nullptr };
    uint32_t m_firstBlockOfLastSearchForWays { 0 };
    std::optional<bool> m_someLoopHasSideEntrance;
    ArenaVector<uint32_t> m_firstLoopsWhoseHeadsAreNotJudged;
    Position m_positionOfLastQuestionForLoopNotJudged { noPosition };
    Position m_beginOfFirstLoopNotJudged { noPosition };
    const Operation* m_rematerializedValueKeptIn[64];
    Position m_rematerializedValueIsKeptUntil[64];
    Position m_endOfLastLoopThatKeepsValueMadeAgain { 0 };
    ArenaVector<OperandTakenBack> m_operandsTakenBackAtHeads;
    Operation* m_operandTakenBackByBranch { nullptr };
    RegisterMask m_registersReadOnWayToFailingSuccessor { 0 };
};

void finishResultsOfPatchThatEndedPredecessor(Allocator&, Block*);

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
