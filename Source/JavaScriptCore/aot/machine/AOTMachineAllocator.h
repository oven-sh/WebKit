/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// One forward walk over the blocks in their order. Each method is defined in the file named above it, or here where every operation or every binding calls it.
//
// Everything is public: a helper that this header does not declare is a static function of the file that needs it, takes Allocator&, and has a name that no other file of aot/ uses.
//
// What holds between any two operations:
//  - A value that will be read again is in a register, or is spilled, or is saved in the rare region the walk is in, or is made again at each use.
//  - m_valueIn and ValueState::registers say the same thing from both sides.
//  - A pinned or scratch register is never bound.
//  - No register is bound to a value whose type is a tuple. A register that holds a member is bound to the Extract.
//  - A register may stay bound to a value that nothing reads any more. So whoever clobbers or drops registers unbinds each of them, not only those whose value is read again.
class Allocator {
    WTF_MAKE_NONCOPYABLE(Allocator);
public:
    Allocator(Procedure&, Allocation&);
    // The walk. Then, so that no other file has to: what each suspension holds, and the frame that each step, each store of a spilled value and each callee-saved register needs.
    void run();

    struct ValueState {
        // Every register that holds it now.
        RegisterMask registers { 0 };
        // In Liveness::use(): the first read that the walk has not passed. Zero if there is none.
        // A read that the scan records for the collector and no operation makes may stay here after the walk has left its position: nextUseOf() steps over it.
        uint32_t nextUse { 0 };
        // Set from the first Save to the end of the region. In a block that a path reaches without that Save it names the place and does not say that the place holds the value:
        // whoever takes the register of such a value saves it again, as evict() does.
        SpillIndex saveInRareRegion { noSpill };
    };

    // The registers at the end of a block, or those that a block expects at its head.
    struct Snapshot {
        bool isSet { false };
        std::span<const RegisterBinding> bindings;
        // The registers that the bindings name. AOTMachineAllocatorJoins.cpp, which makes every snapshot, sets it and alone reads it.
        RegisterMask registers { 0 };
    };

    // ---- AOTMachineAllocator.cpp: the walk, and every operation that is no Patch, Check or Phi.

    // Sets BlockSteps::begin ahead of enterBlock(). Calls finishResultsOfPatchThatEndedPredecessor() right behind enterBlock(), and leaveBlock() behind the operation that ends the block, a Patch too.
    // What sank into the block reads at the head, all of it at one position. It is allocated with position() two ahead of the head, and its uses are passed together afterward.
    void allocateBlock(Block*);
    // Leaves into places, dead operands freed, scratch registers, the result, one Execute.
    // An Extract is bound already, by placeResults(): its Execute has the register as destination and as sources[0].
    // An ArgumentReg is executed where it stands. Its register is bound at the head of the entrypoint, to the last operation that reads it, and is taken for nothing else until that operation is reached.
    // The result prefers the register of its hint, then that of the first child. Where isFreeInRegisterOfChild() holds, the child comes first.
    void allocateOperation(Operation*);
    // allocateOperation(). With `mayBePatch` it hands a Patch or a Check to allocatePatch(). It is defined in line in AOTMachineAllocator.cpp.
    template<bool mayBePatch> void allocateByKindOfOpcode(Operation*);
    // All of allocateOperation(), for any operation. The three below it do the same with less for what most operations are, and hand over the others before they have done anything:
    // an ArgumentReg, an Extract, a Nop, a Branch, what may take a scratch register, what works in memory or goes by overflow, and whatever stands where some register is reserved.
    void allocateOperationTheLongWay(Operation*);
    // Whatever is fused into the operation.
    void allocateOperationWithAnyLeaves(Operation*);
    // Of an opcode with that many children. `traitsOfKind`: what the result of such an opcode may ask for, as AOTMachineAllocator.cpp names it. It hands over an operation with a fused child.
    template<unsigned numberOfChildren, unsigned traitsOfKind> void allocateOperationOfKind(Operation*);
    // The leaves of the operation are the children of `user`, which is the operation itself or what is fused into a Branch. False, and nothing done: one of them is fused.
    template<unsigned numberOfLeaves, unsigned traitsOfKind> bool allocateWithChildrenForLeaves(Operation*, const Operation* user, bool userIsFusedLoad);
    // What all of them do for the result. It is defined in line in AOTMachineAllocator.cpp.
    bool defineResultBehindLeaves(Operation*, unsigned traits, unsigned fusedChildren, RegisterMask operandRegisters, Location& firstSource, Location secondSource, Position, Location& destination);
    // allocateOperation() of an ArgumentReg.
    void allocateArgumentRegister(Operation*);
    // allocateOperation() of the Branch that ends the block of the walk. allocateBlock() is defined in line in AOTMachineAllocator.cpp, and no other file calls it or any of these.
    void allocateBranch(Operation*);
    void allocateBranchWithAnyLeaves(Operation*);
    // Jump, Branch, Switch, Return, Oops. Return moves its child to the return value register of its bank. Sets BlockSteps::end and BlockSteps::terminator.
    // A Branch that tests nothing has no leaf, so its Execute has no source, and the moves of its edge may write registers as ahead of a Jump.
    // allocateBlock() does the same by itself for a Jump, and through allocateBranch() for a Branch.
    void allocateTerminator(Block*, Operation*);
    // The place a leaf is read from: an immediate, a pinned register, a word of the instance, the bytes of a slot under an address, its spill where the instruction reads memory, else a register.
    // `locked` grows by the register chosen: no later leaf, scratch register or reload of the same operation takes it.
    // It looks for a fused load among the children of `user` by itself before it answers with memory.
    // A leaf under an address is never answered with memory, and with an immediate only as the base of the access itself or as the amount of a Shl.
    // A SlotBase that the access takes is Location::slotBytes. A FramePointer, an Add without position and an InstanceAddressOf that it takes are Location::immediate.
    Location placeLeaf(const Operation* user, unsigned childIndex, Operation* leaf, RegisterMask& locked, bool& hasMemoryOperand);
    // Moves the cursor of each leaf past `position` and frees what is not read again. Nothing is passed behind position() + 1, whatever `position` says.
    // The child of an Upsilon of the block that stays is not freed once `position` has reached the operation that ends the block: the moves of the edge read it.
    void passUsesOf(const Operation*, Position);
    void passUse(Operation* value, Position);
    // passUse(), for a position that does not lie ahead of the walk.
    ALWAYS_INLINE void passUseReachedByWalk(Operation* value, Position position) { passUseReachedByWalk(value, stateOf(value), m_valueFacts[value->index()], position); }
    // For whoever has stateOf() the value and Liveness::of() it at hand.
    ALWAYS_INLINE void passUseReachedByWalk(Operation* value, ValueState& state, const ValueFacts& facts, Position position)
    {
        ASSERT(position <= m_position + 1);
        ASSERT(&state == &stateOf(value) && &facts == &m_valueFacts[value->index()]);
        const Use* uses = m_uses;
        uint32_t nextUse = state.nextUse;
        const Use* next = uses + nextUse;
        // Entry zero, which stands where no read is left, has noPosition for its position: no position passes it, and it lies in no block.
        if (next->position <= position) {
            do {
                nextUse = next->next;
                next = uses + nextUse;
            } while (next->position <= position);
            state.nextUse = nextUse;
        }
        // The block reads the value again.
        const Position positionOfNext = next->position;
        if (positionOfNext < m_endOfBlock)
            return;
        // All that is left is to free the registers of a dead value and to pass the read of the owner of an inner pointer. What is asked on the way changes nothing.
        const RegisterMask registers = state.registers;
        if (!registers && !value->has(OperationFlag::IsInnerPointer))
            return;

        // What isDeadOnEveryPathFrom() finds out first, in its order.
        bool rangeGoesOn;
        bool noWayLeadsToNextRead;
        if (nextUse) {
            noWayLeadsToNextRead = positionOfNext >= m_endOfWhatWaysLeadToFromBlock;
            // The read is not known to be out of reach.
            if (!noWayLeadsToNextRead && m_everyReadBehindBlockCounts && next->greatestHeadOfBlockPassedOnEveryPath >= m_headOfBlock)
                return;
            rangeGoesOn = true;
        } else {
            // isLiveBehind(). One more than noPosition is zero, which is greater than nothing, and the position itself is never noPosition.
            rangeGoesOn = static_cast<Position>(facts.lastUse + 1) > position + 1;
            noWayLeadsToNextRead = m_endOfWhatWaysLeadToFromBlock != noPosition;
        }
        if (rangeGoesOn) {
            // Of a block in a rare region it says no, whatever the value. The walk stands in the region of its block from enterBlock() on.
            if (isInRareRegion())
                return;
            if (noWayLeadsToNextRead) {
                // It says yes, but of a value without a definition ahead of the position, a constant among them, and of what the collector needs.
                if (facts.definition > position + 1 || facts.has(ValueFlag::IsKeptForCollector))
                    return;
            } else {
                if (facts.definition == noPosition)
                    return;
                // It says no of what is defined ahead of the innermost loop around the block. At the head of the block alone it may go by the loop around that one.
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
    // The moves of the edge read the children of the Upsilons at the position of the operation that ends the block, behind whatever that operation reads there.
    ALWAYS_INLINE bool edgeMovesOfBlockMayRead(const Operation* value, Position position) const
    {
        // The block ends one pair of positions behind the early position of its last operation.
        if (position < m_endOfBlock - 2)
            return false;
        for (const Operation* upsilon : m_block->upsilons()) {
            if (upsilon->valueHandedOn() == value && !m_valueFacts[upsilon->index()].has(ValueFlag::IsDropped))
                return true;
        }
        return false;
    }
    // The scan has its owner read once more where it is last read itself, by no operation.
    void passReadOfOwnerOfInnerPointer(const Operation* innerPointer, Position);
    // Binds the result and applies what Liveness says of it: a callee-saved register or a spill if it lives across a call, a spill if a landing pad or a suspension needs one.
    // It appends no step of its own (takeRegister() may, for what it evicts).
    // A register in `avoided` is never taken, whatever else is free. Without `preferred` the hint of the value is. A result that nothing reads gets a register and is not bound.
    // A result that lives across a call and is held to registers that calls clobber is marked spilled at once only if no callee-saved register of its bank is free. Otherwise vacate() moves it at the call.
    // The KeepPointer of a narrow reference cannot stand ahead of the Execute, so whoever appends the Execute appends it behind.
    Location defineResult(Operation*, RegisterMask allowed, RegisterMask avoided, Reg preferred);
    // defineResult(), for whoever has Liveness::of() the operation at hand. It is defined in line in AOTMachineAllocator.cpp, and no other file calls it.
    Location defineResultWithFacts(Operation*, const ValueFacts&, RegisterMask allowed, RegisterMask avoided, Reg preferred);
    // Sets Step::width for every kind but Execute, from the type of the operation. The caller sets Width64 for a KeepPointer.
    // Counts each Execute for Allocation::numberOfOperationsAllocated().
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
    // For a step that its maker has filled in, width and all. It is counted in the same way.
    ALWAYS_INLINE void appendStep(const Step& step)
    {
        Allocation& allocation = m_allocation;
        allocation.m_numberOfOperationsAllocated += step.kind == StepKind::Execute;
        new (NotNull, allocation.m_steps.appendUninitialized(m_arena)) Step(step);
    }
    // The Execute of an operation that is no Patch or Check, takes no scratch register and does not go by overflow. `sources` has maxLeaves entries, those behind the leaves without a location.
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
    // The same of an operation with two leaves at most. A leaf that it lacks has no location.
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
    ALWAYS_INLINE void noteFrameUse()
    {
        if (isInRareRegion()) {
            m_allocation.m_regionNeedsFrame.set(m_rareRegion);
            return;
        }
        m_allocation.m_needsFrameOutsideRareRegions = true;
        m_allocation.m_needsFrameAheadOfFramedPart |= m_isAheadOfFramedPart;
    }
    // Where the walk enters a head of the framed part, behind enterBlock(). Keeps the pointers that have waited for it, and moves what lives across a call to callee-saved registers while there are free ones.
    void enterFramedPart();
    Position position() const { return m_position; }
    bool isInRareRegion() const { return m_rareRegion != noRareRegion; }

    // ---- AOTMachineAllocatorRegisters.cpp: the register file, for both banks.

    static constexpr uint16_t bitOf(ValueFlag flag) { return static_cast<uint16_t>(flag); }
    static ALWAYS_INLINE Reg lowestRegisterIn(RegisterMask mask)
    {
        ASSERT(mask);
        return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(mask)));
    }
    ValueState& stateOf(const Operation* value) { return m_values[value->index()]; }
    // The position of the first read at or behind the position of the walk. noPosition if the value is needed no more.
    // A value whose reads have all been passed and whose range reaches the position of the walk or goes beyond it counts as read at ValueFacts::lastUse:
    // a loop reads it on its next turn, or an Upsilon of the block hands it on. So `nextUseOf(value) == noPosition` means dead.
    ALWAYS_INLINE Position nextUseOf(const Operation* value) const
    {
        // The scan records reads for the collector that no operation makes, and passUse() does not pass all of them. The walk never comes back to a position that it has left.
        for (uint32_t use = m_values[value->index()].nextUse; use; use = m_uses[use].next) {
            Position position = m_uses[use].position;
            if (position >= m_position)
                return position;
        }

        // The walk has passed every read. Two kinds of value are needed all the same, and count as read at the end of their range:
        // what a loop reads on its next turn, and what an Upsilon hands on, which the moves of the edge read behind the operation that ends the block.
        Position lastUse = m_valueFacts[value->index()].lastUse;
        if (lastUse != noPosition && lastUse >= m_position)
            return lastUse;
        return isKeptThroughLoopAround(value, m_position) ? m_rematerializedValueIsKeptUntil[m_hints[value->index()].index()] : noPosition;
    }
    // In Liveness::use(): the first read of the value at the position or behind it, zero if there is none. The walk has not gone past the position.
    // A Switch may lead to as many heads as the procedure has blocks, and each of them asks about what all of them read. So whoever has stepped over a few reads goes on in a list of them.
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
    // By ValueFacts::lastUse, which is extended over loops. Never by the definition: an ArgumentReg and an Extract are bound ahead of theirs.
    ALWAYS_INLINE bool isLiveBehind(const Operation* value, Position position) const
    {
        Position lastUse = m_valueFacts[value->index()].lastUse;
        return lastUse != noPosition && lastUse > position;
    }
    // Reg() if it is in none of them.
    ALWAYS_INLINE Reg anyRegisterOf(const Operation* value, RegisterMask allowed) const
    {
        RegisterMask holding = m_values[value->index()].registers & allowed;
        return holding ? lowestRegisterIn(holding) : Reg();
    }
    // Binds a free register. Over a value that is read behind the position of the walk and is nowhere else: fail(RegisterLacksItsValue). Calls noteCalleeSaveUse().
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
        // Whatever way a register came to hold a value, the function writes it.
        noteCalleeSaveUse(reg);
    }
    // The rest of bind(): nothing where the register holds the value already, else the test for a value that would be lost, unbind() and bind().
    void bindRegisterThatIsBound(Reg, Operation*);
    // Nothing, if the register is free.
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
    // For whoever has stateOf() the value at hand.
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
    // Of `allowed`: the registers of the bank that the block of the walk may take.
    ALWAYS_INLINE RegisterMask registersThatMayBeTaken(Bank bank, RegisterMask allowed) const
    {
        RegisterMask registers = allowed & m_file.allocatableIn(bank);
        // Anywhere else calleeSavesThatMayBeTaken() names all of them.
        if (m_isAheadOfFramedPart || isInRareRegion())
            registers &= ~m_file.calleeSaved | calleeSavesThatMayBeTaken();
        return registers;
    }
    // `preferred` if it is free, else a free register whose use adds nothing to the frame. None, if the only free ones are callee-saved registers that the function does not save yet.
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
    // A register among `allowed`, not bound. In this order: `preferred` if it is free, a free one that no callee saves, a free callee-saved one that the function saves already,
    // one whose value is dead, held in another register too or made again, a free callee-saved one that the function does not save yet, the victim.
    // In a rare region no callee-saved register is taken that the code outside has not taken, unless `allowed` holds nothing else.
    // If `allowed` holds no register of the bank: fail(OutOfRegisters), and some register of the bank, so that the walk reaches the place where the failure is seen. Never Reg().
    ALWAYS_INLINE Reg takeRegister(Bank bank, RegisterMask allowed, Reg preferred = Reg())
    {
        ASSERT(registerFileIsConsistent());

        Reg chosen = chooseFreeRegisterWithoutNewCalleeSave(registersThatMayBeTaken(bank, allowed) & ~m_bound, preferred);
        if (!chosen) [[unlikely]]
            return takeRegisterAfterSearch(bank, allowed, preferred);

        ASSERT(!m_valueIn[chosen.index()]);
        // A scratch register is written and never bound.
        noteCalleeSaveUse(chosen);
        return chosen;
    }
    // All of takeRegister(), from its start: for where no register is to be had that is free and adds nothing to the frame.
    Reg takeRegisterAfterSearch(Bank, RegisterMask allowed, Reg preferred);
    // takeRegister() would neither store anything now nor cause a load later: a register is free, or its value is dead, held in another register too or made again.
    ALWAYS_INLINE bool hasFreeRegister(Bank bank, RegisterMask allowed) const
    {
        RegisterMask candidates = registersThatMayBeTaken(bank, allowed);
        return (candidates & ~m_bound) || canDropSomeRegisterForFree(candidates);
    }
    // Every candidate is bound.
    bool canDropSomeRegisterForFree(RegisterMask candidates) const;
    // The victim among `allowed`: a value that is made again or is held in another register too or is spilled already, else the one whose next read is farthest.
    Reg chooseVictim(Bank, RegisterMask allowed) const;
    // Frees the register. Its value stays reachable: it is held elsewhere, or made again, or gets spilled, or in a rare region is saved (AOTMachineAllocatorRareRegions.cpp).
    // A value whose save was made in another block of the region is saved again: the path to this block may not have passed that Save.
    // Once the operation that ends the block has its step, no Save can stand in the block: the value is marked spilled instead.
    void evict(Reg);
    // Puts the value into a register among `allowed`, by Move, Reload or Materialize, unless it is in one.
    ALWAYS_INLINE Reg ensureInRegister(Operation* value, RegisterMask allowed, Reg preferred = Reg())
    {
        allowed &= m_file.allocatableIn(bankOf(value->type()));
        if (RegisterMask holding = stateOf(value).registers & allowed)
            return preferred && (holding & maskOf(preferred)) ? preferred : lowestRegisterIn(holding);
        return bringIntoRegister(value, allowed, preferred);
    }
    // The rest of ensureInRegister(). `allowed` holds registers of the bank of the value alone, and none of them holds it.
    Reg bringIntoRegister(Operation* value, RegisterMask allowed, Reg preferred);
    // From now on the value has a spill, which holds it from its definition on. Never for a value that is made again.
    // Sets ValueState::saveInRareRegion back: the spill holds on the paths around the region too.
    // The range of a Phi without a register at its definition takes in the moves of its incoming edges, so fixHeadOf() records that register first.
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
        // Registers are saved by the frame that is set up at the entries. The frame of a rare region saves none.
        m_allocation.m_needsFrameOutsideRareRegions = true;
        // But where there is a framed part every setup saves them, that of a rare region ahead of it too.
        if (m_isAheadOfFramedPart)
            noteFrameUse();
    }
    // The register that the value is best defined in or brought to: what noteHintsOfHead() has found for it, else ValueFacts::hint. Whoever asks takes another register where this one is not free.
    ALWAYS_INLINE Reg hintOf(const Operation* value) const
    {
        Reg hint = m_hints[value->index()];
        return hint ? hint : m_valueFacts[value->index()].hint;
    }

    // ---- AOTMachineAllocatorCalleeSaves.cpp

    // The callee-saved registers of the bank that a value which lives across a call may take where the walk stands in the block, or fixes its head. All of them without ProcedureTraits::chargesForCalleeSaves.
    // One that the function does not save yet costs a store at each setup and a load at each teardown. In memory the value costs the store behind its definition and a load for each read outside rare regions.
    // So such a register is among them only for a value that is read twice or more, or in a loop. On ARM64 the second of a pair costs nothing.
    RegisterMask calleeSavesWorthTakingFor(const Operation* value, Bank, const Block*) const;

    // ---- AOTMachineAllocatorJoins.cpp: what happens between blocks.

    // Of a block that holds a Jump alone, continues its one predecessor and leads to a head that is fixed: the block takes no register and gives none up, so the edge goes by the end of the predecessor.
    // The registers stay as they are, whichever block has left them. False, and nothing done, for any other block.
    bool passThroughBlockThatOnlyJumps(Block*);
    // Sets the registers for the head of the block. An entrypoint and a landing pad start empty. A block with one predecessor and no Phi that stays takes that block's end. Any other takes what was fixed for it.
    // Where the rare region of the block differs from that of the block ahead of it in the order, and nowhere else, it calls leaveRareRegion() ahead of setting the registers and enterRareRegion() behind it.
    // Order keeps a region in one piece, so each is called once for a region. ValueState::saveInRareRegion is left alone: it lasts from block to block within the region.
    // It appends no step for another block.
    void enterBlock(Block*);
    // For each successor: fixes its head if this edge is the one to do so, keeps the end of this block if a successor will take it, appends the moves of the edge if the head is fixed.
    // Where the edge has to wait, what lies in a save of the rare region alone is marked spilled.
    void leaveBlock(Block*);
    // The first edge that is not rare fixes the head of a block with several predecessors. If every edge is rare, the first one does.
    // The one edge to a block with one predecessor fixes its head where the block has a Phi that stays.
    bool edgeFixesHead(const Block* from, const Edge&) const;
    // From the registers as they are: drops what is not live at the head or is not defined on every path to it, gives each Phi a place, and for a loop applies chooseRegistersKeptThroughLoop().
    // A Phi takes the register of its incoming value if that value dies on the edge, else a free register, else its spill alone.
    // An Extract of the Patch that ends `from` goes by the definition of the Patch: its own lies in the successor, behind the head.
    // What lies in a save of the rare region alone gets a free register or is marked spilled. The pointer beside a Phi is kept by allocateBlock().
    // Outside rare regions a head that starts no loop also holds what is made again, names no frame and is read ahead of the next call, in the register it lies in, if nothing else has taken that.
    // Where the head differs from a plain one by its Phis alone and `from` has one successor, it appends the moves of that edge too, and leaveBlock() does not call appendEdgeMoves() for it.
    void fixHeadOf(Block* target, Block* from);
    // The moves that turn the registers at the end of `from` into those the head expects, the Upsilons of `from` among them. Also called, late, for an edge that was passed before the head was fixed.
    // A head outside rare regions that starts no loop and that the walk has not entered lets go of two kinds of value where the edge has them elsewhere, and the edge brings neither:
    //  - what is made again and no loop around keeps, unless the edge is rare and the only other one into the head: that edge makes it;
    //  - what is spilled and is read next by the first call behind the head, in another register or wherever it lies.
    // leaveBlock() may hand in the entry of m_ends for its block while that is not set: then the registers are those at the end of the block.
    void appendEdgeMoves(Block* from, unsigned successorIndex, const Snapshot& endOfFrom);
    // Of an unsplit edge. A block has to stand on it whether or not it gets a move: planFrame() may tear the frame of the framed part down there, or return there in the place of a jump.
    bool frameMayChangeOnEdge(const Block* from, const Block* target);
    // Order::splitLate(), and the new block has its steps, but for the moves of its edge. It takes no register and gives none up, so it is entered and left with the registers at the end of `from`.
    Block* splitEdge(Block* from, unsigned successorIndex, const Snapshot& endOfFrom);
    // The block stands further on than the walk. The position of the first read of the value at or behind its head, noPosition if there is none.
    // Every edge into a block asks the same of a value, so the last answer is kept with the head it was given for.
    Position findFirstReadFromHeadOf(const Block*, const Operation* value);
    // Allocation::registersAtHeadOf().
    bool listsRegistersAtHeadOf(const Block* block) const { return m_proc.traits().checksAllocation || m_order.of(block).isRare() || m_allocation.isHeadOfFramedPart(block); }
    Snapshot takeSnapshot();
    void restore(const Snapshot&);
    // One pass over the Upsilons of `from`. fixHeadOf() and appendEdgeMoves() list the incoming values of an edge in one pass for all Phis instead.
    Operation* incomingValueOf(const Operation* phi, const Block* from) const;
    // At the end of fixHeadOf(), for the edges into the head that the walk has not reached, the edges back to the head of a loop among them.
    // The hint of what such an edge hands to a Phi is the register of the Phi. So is that of the child whose register its result takes over where the child dies there, and so on down.
    // Where it is a Phi of a head that no edge has fixed, so is that of what the edges hand to that Phi: a block passes the hints of its Phis on once. A rare edge gives hints in rare regions alone.
    // The hint of what the head holds beside its Phis is the register that the head holds it in.
    void noteHintsOfHead(const Block* target);
    // Order makes a loop of every edge that leads back in the order, that of a rare block which stands behind the block it rejoins too. Such an interval is run through once.
    bool hasBackEdgeOutsideRareRegions(const Loop&) const;
    // Both fill m_immediateDominators at the first question. Null for an entrypoint, and where the two have no block in common.
    const Block* immediateDominatorOf(const Block*);
    const Block* lastBlockOnEveryPathToBoth(const Block*, const Block*);
    struct OperandTakenBack {
        Reg reg;
        Operation* value { nullptr };
    };
    // Of a sum with ValueFlag::GoesByOverflow in the block of the walk, whose leaves lie in `sources` and have been passed.
    // The operand whose register the sum may take though the failing side reads the operand, by the conditions of AOTMachineLiveness.h. No value where there is none. It changes nothing.
    OperandTakenBack findOperandThatSumMayOverwrite(const Operation* sum, std::span<const Location> sources);
    // The sum has taken the register. enterBlock() binds it to the operand again at the head of the failing successor, and nothing takes the register of the other operand ahead of the Branch.
    void noteOperandTakenBack(const Operation* sum, OperandTakenBack, Location otherOperand);

    // ---- AOTMachineAllocatorLoops.cpp

    // At the edge that fixes the head of a loop. A value that the loop reads outside rare regions keeps its register, unless a Patch of the loop takes that register: LoopFacts::registersTakenByPatches.
    // Then the edge moves it to a free register that none of them takes. Where there is none, a spilled value is unbound, so no edge back to the head loads it, and any other stays.
    // A value that only lives through, or that rare regions alone read, keeps its register while the pressure of the loop leaves one free in its bank and no Patch of the loop takes it,
    // those with the nearest read first. The others are spilled and unbound. So are those that hold a register which no Patch takes while what the loop reads or a Phi of the head waits for one.
    // Nothing moves or gives way on account of a Patch without hasBackEdgeOutsideRareRegions().
    // A value that only lives through and lives across a call keeps a callee-saved register alone, and one that lives across a suspension only a register that suspensions keep.
    // Never spilled here, and counted against the free registers: a result of a Patch that ends the block in front of the loop, and in a loop of a rare region a value from outside the region that has no spill yet.
    // It finds no value bound that is dead at the head, none that is made again but what a loop around this one keeps, no value in two registers, and those Phis of the head bound that have taken the register of a dying value over.
    // The other Phis get theirs behind it, from what it leaves free. A Phi that it unbinds may get another register or lives in its spill alone.
    void chooseRegistersKeptThroughLoop(const Loop&, bool movesMayWriteRegisters);
    // The first read at or behind the start of the loop stands ahead of its end.
    bool isReadInside(const Operation* value, const Loop&) const;
    // Behind chooseRegistersKeptThroughLoop(), once the Phis of the head have their registers, and only where the moves of the edge may write registers.
    // What the loop reads outside rare regions and no register holds gets one at the head, so the edge into the loop loads it or makes it once and not every turn: LoopFacts::valuesReadFromOutside.
    // Neither a register that a Patch of the loop takes, nor a callee-saved one that the function does not save yet. A spilled value gets one while the registers cover the pressure of the loop, which counts it.
    // A value that is made again is the first to be evicted, so it gets one only while more such registers are free than the loop still asks for.
    // Nothing is brought in without hasBackEdgeOutsideRareRegions(). `limitOfDefinitions`: as in fixHeadOf().
    void bringInWhatLoopReads(const Loop&, Position limitOfDefinitions);
    // The head of a loop around the position holds the value, which is made again, in this register. The range of such a value ends with its last read, wherever the loop ends.
    // So up to the end of the loop nothing takes it for dead, and a head within the loop holds it too: the edge back finds it where the head of the loop expects it.
    bool isKeptThroughLoopAround(Reg, const Operation* value, Position) const;
    // In the register that m_hints names for it.
    bool isKeptThroughLoopAround(const Operation* value, Position position) const { return position < m_endOfLastLoopThatKeepsValueMadeAgain && isKeptThroughLoopAround(m_hints[value->index()], value, position); }
    // `from` is a position of the block. The value is defined ahead of it, in the innermost loop around the block if there is one: behind the start of that loop it is defined again before anything reads it.
    // No way on from the position leads to one of its reads but over that start, however far its range goes in the order. Two kinds of read are known to be out of reach:
    //  - one behind the loop, where no way leads from the block out of the loop but over its start: in the body of a loop that is left at its head, a Phi of the head that is read behind the loop;
    //  - one in a block further on whose one predecessor stands ahead of this block: in `if (a) { if (b) n++; }`, both arms that hand n on as it is.
    //    Of a rare block it is Liveness::blockThatRareBlockBranchesOff() that has to stand ahead: a chain of rare blocks that leaves a loop, wherever the order has put it.
    // Of a block in a rare region it says nothing. There whatever is live by its range and has no register was saved on the way to the block, and is loaded from that save.
    // Nor of a value with ValueFlag::IsKeptForCollector.
    bool isDeadOnEveryPathFrom(const Operation* value, const Block*, Position from);

    // ---- AOTMachineAllocatorPatches.cpp: Patch and Check.

    // It does not call leaveBlock(). For a Patch that ends its block the Execute is the last step it appends, and it sets BlockSteps::end and BlockSteps::terminator to that step.
    // Where no successor takes registers (none at all, or landing pads alone) it unbinds every register and vacates nothing.
    void allocatePatch(PatchOperation*);
    // Clears each register that an operand is constrained to of any other value that is still needed, then brings all such operands in at once.
    void placeFixedOperands(PatchOperation*, std::span<Location> locationsOfOperands, RegisterMask& locked);
    // Every value that is read behind the operation leaves the registers in `clobbered`: to a free register outside, else by evict().
    void vacate(RegisterMask clobbered, Position behind);
    // Does for each result what defineResult() does. The results of a Patch with a tuple type are its Extracts, which it binds. The Patch itself is never bound.
    void placeResults(PatchOperation*, std::span<Location> locationsOfResults, RegisterMask locked);
    RegisterMask takeScratchRegisters(Bank, unsigned count, RegisterMask& locked);
    // Of a Patch that releases its child, where the child is read no more.
    void releaseDeadValue(PatchOperation*);
    // Of a call, once its operands are placed and ahead of the pass over its uses. `locked` names the registers that the call takes operands in.
    // Each child that is let go and read no more: the other registers that hold it go into m_registersToRelease, and its place in memory is cleared in front of the call unless the call reads it there.
    // Every child that holds such a value is marked, so a value that is handed over twice is seen both times.
    void releaseChildrenLetGoByCallee(PatchOperation*, std::span<const Location> locationsOfOperands, Position early, RegisterMask locked);
    // Of a Phi that is released at the position. What the edges handed to it, themselves or through other Phis, still lies in the spills in which it waited.
    // Each such spill whose value is read no more keeps its place up to the position and is cleared there. So is one that only other arms read: isReadOnSomeWayOn().
    // Such a value loses its registers here too, which are returned for the generator to clear: with many registers that callees save it waits in one and has no spill.
    RegisterMask clearPlacesOfValuesHandedTo(Operation* released, Position);
    // Behind isReadOnSomeWayOn() has said false of the value. An edge back owes a head that the walk has entered whatever the head holds: appendEdgeMoves().
    // No way leads from here to a head from which the value is read. Any other head of a loop around this block that holds it, by its range alone, may be reached.
    bool isHeldAtHeadThatWayLeadsBackTo(const Operation* value) const;
    // The position lies in the block of the walk, and the value is spilled and defined ahead of it. The arms of a Branch stand one behind the other in the order, so ValueFacts::lastUse
    // counts the reads of arms that no way leads to from here: `table[i](make())` asks for each callee in turn and hands the same word to each.
    // False: each way from the position to a read of the value leads past its definition, which fills the spill anew. It goes by the edges alone, back from every read to the definition,
    // once for a value: a rare region that rejoins a block ahead of it makes a loop of the order where the program has none. True in a rare region and of ValueFlag::IsKeptForCollector.
    bool isReadOnSomeWayOn(const Operation* value, Position);
    // A value waits across calls in a register that callees save, and an edge moves it on under another name. Nobody remembers which register that was, and a native callee
    // stores it where the collector reads. Each of these that is bound to nothing is cleared with the value. They are the function's own from its entries on: noteCalleeSaveUse().
    RegisterMask registersWhereReleasedValueMayHaveWaited() const;

    // ---- AOTMachineAllocatorRareRegions.cpp: rare regions, landing pads, suspensions.

    void enterRareRegion(uint32_t region);
    void leaveRareRegion();
    // For a value defined outside the region that is not spilled: a Save to a spill of the region. It is then not marked spilled, so the code outside does not store it.
    // Any other value is marked spilled. Either way memoryLocationOf() answers afterward.
    // Once the operation that ends the block has its step, no Save can stand in the block: the value is marked spilled.
    void saveInRareRegion(Operation* value, Reg from);
    // Of a value without a register in `block`, where the walk stands or has stood last: it was in a register where the way last had nothing in a save, so the way has saved it.
    bool wayHasSavedInRareRegion(const Block*, const Operation* value) const;
    // A read ahead of the framed part finds the value in no register and in no save of its way. The value is stored behind its definition after all, and every decision from here on goes by that.
    void giveUpFramedPartForReadOfSpill() const;
    bool isDefinedOutsideRareRegion(const Operation* value) const;
    // The location of a value that is in no register: its save in the rare region, else its spill. Where it has neither: fail(RegisterLacksItsValue), and no location.
    Location memoryLocationOf(const Operation* value) const;
    // Every register but SavedFrame::preservedRegister dies. Each root is answered with its spill, or with that register.
    // allocatePatch() calls it first of all, ahead of the operands. Every LateColdAny operand with a position counts as a root.
    // fail(MalformedProcedure) unless ProcedureTraits::savesFrameOnSuspend is set and the Patch clobbers, or defines a result in, every allocatable register but that one.
    void prepareSuspension(PatchOperation*);
    // Behind the walk: Allocation::spillsHeldAt() of every suspension, and SpillFacts::livesAcrossSuspension of every spill.
    // It reads the steps once, as the encoder writes them. Then each spill whose range comes near a suspension is followed back from its reads, through each block at most once.
    void findSpillsHeldAtSuspensions();
    // Behind everything else: SpillFacts::beginOfGap and endOfGap. Of a spill that no suspension holds, that the store behind the definition of its value alone writes, and whose range ends in a landing pad.
    // In a function with such spills it reads the steps once, and those of the landing pads once more.
    void findGapsAheadOfLandingPads();

    // ---- AOTMachineFrame.cpp: the framed part, Allocation::headsOfFramedPart().

    // Ahead of the walk. A block outside rare regions needs a frame whatever the walk does where it holds a Patch that does not run without one or follows a setup, a use of a SlotBase or of the FramePointer,
    // or more values than there are registers that no caller counts on. A head stands at the first block from which every way on comes to such a need, and above each loop that holds one: no way pays for a frame that it does not need.
    // A setup may stop the function for lack of stack, and whoever catches that finds nothing done. So whatever writes memory on a way to a first need counts as one, but for a rare block that calls: its own setup has tested the same frame,
    // and so has the setup of a head that a way has passed.
    // None in a function that sets its frame up at entry, has several entrypoints or saves the registers of its callers,
    // where a head would be the entrypoint, and where no way outside rare regions leaves the function without passing a head.
    void findFramedPart();
    // Behind the walk and what run() finds out of the frame. Where a block outside the framed part writes a callee-saved register, follows a setup or names the spill of a value that the framed part alone would store: needsFrameAheadOfFramedPart().
    // Otherwise each value that is defined ahead of the framed part, outside rare regions, and is spilled gets its Save at each head that it reaches in a register. So too where a step below a head reads what that head has not saved.
    void settleFramedPart();
    void standIn(const Block* block)
    {
        m_rareRegion = m_order.of(block).rareRegion;
        m_isAheadOfFramedPart = isAheadOfFramedPart(block);
    }
    bool isAheadOfFramedPart(const Block* block) const { return m_allocation.hasFramedPart() && !m_allocation.isInFramedPart(block); }
    // The moves of the edges into the block run without the frame of the framed part.
    bool headIsAheadOfFramedPart(const Block* block) const { return m_allocation.hasFramedPart() && (!m_allocation.isInFramedPart(block) || m_allocation.isHeadOfFramedPart(block)); }
    // Nothing stores such a value behind its definition: the heads of the framed part store it. settleFramedPart() goes by the same rule.
    bool isStoredAtHeadsOfFramedPart(const Operation* value) const
    {
        if (!m_allocation.hasFramedPart() || m_allocation.needsFrameAheadOfFramedPart() || value->hasNoPosition() || m_liveness.isSunk(value))
            return false;
        const Block* blockOfStore = value->endsBlock() ? value->owner()->successor(0).block() : value->owner();
        return !m_allocation.isInFramedPart(blockOfStore) && !m_order.of(blockOfStore).isRare();
    }
    // The walk comes to the rare regions last, and to what stands between two heads behind the first. Whatever the framed part has spilled by then lies in no memory there.
    bool spillHoldsValueIn(const Block* block, const Operation* value) const { return m_allocation.isSpilled(value) && !(isAheadOfFramedPart(block) && isStoredAtHeadsOfFramedPart(value)); }
    bool spillHoldsValueAtHeadOf(const Block* block, const Operation* value) const { return m_allocation.isSpilled(value) && !(headIsAheadOfFramedPart(block) && isStoredAtHeadsOfFramedPart(value)); }
    // By the block of the walk. Ahead of the framed part none: they hold what the caller left in them.
    RegisterMask calleeSavesThatMayBeTaken() const
    {
        if (m_isAheadOfFramedPart)
            return 0;
        return isInRareRegion() ? m_calleeSavesTakenOutsideRareRegions : m_file.calleeSaved;
    }
    // The walk stands ahead of the framed part, outside rare regions. What is defined here and lives across a call waits in any register: the calls stand in the framed part,
    // where enterFramedPart() or vacate() moves it to a callee-saved register, or it is spilled and stored at the head.
    bool leavesValuesAcrossCallsToFramedPart() const { return m_isAheadOfFramedPart && !isInRareRegion(); }
    // True: the pointer beside the reference, which is defined here, is kept by enterFramedPart(). Every safepoint that the reference lives across stands in the framed part.
    bool pointerWaitsForFramedPart(Operation* reference);

    Procedure& m_proc;
    Arena& m_arena;
    const Order& m_order;
    const Liveness& m_liveness;
    Allocation& m_allocation;
    const RegisterFile m_file;
    // What Liveness::of() and Liveness::use() index. The scan is over, so neither list moves or changes while the allocator lives.
    const ValueFacts* m_valueFacts { nullptr };
    const Use* m_uses { nullptr };
    // The least of bitsOfPinnedConstants(): findPinnedConstant() knows no constant below it.
    uint64_t m_smallestPinnedConstant { UINT64_MAX };

    // By Reg::index(). Of this list and of the two for values that are made again the constructor clears the entries up to Reg::maxIndex(). Nobody looks at the others.
    Operation* m_valueIn[64];
    RegisterMask m_bound { 0 };
    // By Operation::index().
    ArenaVector<ValueState> m_values;
    // By Block::index().
    ArenaVector<Snapshot> m_heads;
    ArenaVector<Snapshot> m_ends;
    // Edges that were passed before the head of their target was fixed.
    struct WaitingEdge {
        Block* from;
        uint32_t successorIndex;
        // The next edge that waits for the same head: one more than its index in m_waitingEdges. Zero at the end.
        uint32_t next;
    };
    ArenaVector<WaitingEdge> m_waitingEdges;
    // Ahead of the framed part no spill holds what the heads store. An edge that waits there reads the save of the rare region that it leaves, which ValueState::saveInRareRegion has forgotten by then.
    struct SaveReadByWaitingEdge {
        const Block* from;
        const Operation* value;
        SpillIndex save;
    };
    ArenaVector<SaveReadByWaitingEdge> m_savesReadByWaitingEdges;
    // By Block::index() of the target, numbered in the same way. Empty until the first edge waits.
    struct WaitingEdgesOfHead {
        uint32_t first { 0 };
        uint32_t last { 0 };
    };
    ArenaVector<WaitingEdgesOfHead> m_waitingEdgesOfHeads;
    // frameMayChangeOnEdge(). By Block::index() of a block that ends in a Return: whether it has been looked at, and whether a predecessor of it stands in the framed part. Empty until the first question.
    BitSpan m_isAskedWhetherFramedPartLeadsTo;
    BitSpan m_framedPartLeadsTo;
    // By Block::index(): the last block that every path to the block passes. Null for an entrypoint, and where paths from two entrypoints meet.
    // AOTMachineAllocatorJoins.cpp fills it ahead of the first head that an edge fixes. No other file reads it.
    ArenaVector<const Block*> m_immediateDominators;
    // AOTMachineAllocatorJoins.cpp fills these three at the first question. No other file reads them.
    // By bank and BlockPlace::orderIndex: BlockPlace::orderIndex of the nearest block, among the block itself and those that every path to it passes, that holds a call outside rare regions. UINT32_MAX: none does. UINT32_MAX - 1: nobody has asked.
    ArenaVector<uint32_t> m_nearestDominatorsWithCall[numberOfBanks];
    // By BlockPlace::orderIndex. The blocks are numbered from one so that a block and all those that every path to which passes it have the numbers from `first`, its own, to `last`.
    struct NumbersOfDominatedBlocks {
        uint32_t first { 0 };
        uint32_t last { 0 };
    };
    ArenaVector<NumbersOfDominatedBlocks> m_numbersOfDominatedBlocks;
    // By the index of a read in Liveness::use(): the least and the greatest of those numbers among the blocks of this read and of the later reads of the same value, reads in rare regions aside.
    // Both zero: nobody has asked. The least above the greatest: only rare regions read from here on.
    struct NumbersOfReadingBlocks {
        uint32_t least { 0 };
        uint32_t greatest { 0 };
    };
    ArenaVector<NumbersOfReadingBlocks> m_numbersOfBlocksThatReadFrom;
    // Results of a Patch that ended the block before: what has to follow their definition stands at the head of the first successor.
    // Nothing appends to it, reads it or clears it: finishResultsOfPatchThatEndedPredecessor() goes by the Patch.
    // Between such a Patch and its first successor the order may hold other blocks, so a reader goes by the block and not by the next block entered.
    ArenaVector<Operation*> m_resultsDefinedAtEndOfPredecessor;

    Block* m_block { nullptr };
    // The block that the walk has just left, where nothing has changed the registers since its end. enterBlock() alone reads it.
    const Block* m_blockWhoseEndRegistersHold { nullptr };
    Position m_position { 0 };
    // Of the block of the walk.
    Position m_headOfBlock { 0 };
    Position m_endOfBlock { 0 };
    // It stands outside rare regions, and a way leads on from it out of the innermost loop around it, if there is one: isDeadOnEveryPathFrom() passes over no read because it stands behind that loop.
    bool m_everyReadBehindBlockCounts { false };
    // Loop::begin of the innermost loop around it. Zero if there is none.
    Position m_beginOfLoopAroundBlock { 0 };
    // Liveness::endOfWhatWaysLeadToFrom() the block of the walk: a read at or behind it is out of reach, whatever the line above says of it.
    Position m_endOfWhatWaysLeadToFromBlock { noPosition };
    // The walk has been through a block that a Patch with a result ends. Until then finishResultsOfPatchThatEndedPredecessor() has nothing to finish: it goes by the step of such a Patch.
    bool m_somePatchWithResultHasEndedBlock { false };
    // Of the entrypoint of the walk: ValueFacts::definition of the last ArgumentReg that stands in it. Zero if there is none.
    Position m_lastDefinitionOfArgumentRegister { 0 };
    uint32_t m_rareRegion { noRareRegion };
    // Taken by code outside rare regions so far.
    RegisterMask m_calleeSavesTakenOutsideRareRegions { 0 };
    // isAheadOfFramedPart() of the block of the walk.
    bool m_isAheadOfFramedPart { false };
    // The safepoints of a block outside the framed part, or of the blocks of a rare region there: the late positions of the first and of the last.
    // A value that is defined ahead of the last one may live across one of them if it is read behind the first, ahead of `until`.
    struct SafepointsAheadOfFramedPart {
        Position first;
        Position last;
        Position until;
    };
    ArenaVector<SafepointsAheadOfFramedPart> m_safepointsAheadOfFramedPart;
    // Each head that the reference reaches keeps the pointer, all of them in one spill.
    struct ReferenceWhosePointerWaits {
        Operation* reference;
        SpillIndex pointer { noSpill };
    };
    ArenaVector<ReferenceWhosePointerWaits> m_referencesWhosePointersWaitForFramedPart;
    // Of the rare region that the walk is in. AOTMachineAllocatorRareRegions.cpp writes and reads m_valuesSavedInRareRegion and m_endOfRareRegion, no other file does.
    // Nothing writes or reads the other three: a Save is made without asking which paths pass it.
    // The values whose ValueState::saveInRareRegion it has set.
    ArenaVector<Operation*> m_valuesSavedInRareRegion;
    // In Order::blocksInOrder().
    uint32_t m_firstBlockOfRareRegion { 0 };
    // Order::endOf() its last block.
    Position m_endOfRareRegion { 0 };
    // By the place of a block in the order, counted from m_firstBlockOfRareRegion: the head of the first block behind it that is reached on a path that does not pass it. noPosition if the region has none.
    ArenaVector<Position> m_savesHoldUntil;
    // An edge leads back to one of its blocks.
    bool m_rareRegionHasLoop { false };
    ArenaVector<PendingMove> m_pendingMoves;
    // By Operation::index(): what noteHintsOfHead() has found.
    ArenaVector<Reg> m_hints;
    // By Operation::index(): the position of a read outside rare regions that fixHeadOf() has found, zero if none. Empty until the first one is looked for.
    ArenaVector<Position> m_knownReadsOutsideRareRegions;
    // By Block::index().
    BitSpan m_hasPassedOnHintsOfPhis;
    // By Operation::index(), in Liveness::use(): the reads ahead of this one are out of reach of the block that the walk stands in, and so of every block behind it. UINT32_MAX: all of them are.
    // Zero: nothing is known. Empty until isDeadOnEveryPathFrom() has something to note.
    ArenaVector<uint32_t> m_firstReadsNotKnownOutOfReach;
    // By Operation::index(): the last answer of findFirstReadFromHeadOf(). Empty until the first question.
    struct FirstReadFromHead {
        Position head { noPosition };
        Position read { noPosition };
    };
    ArenaVector<FirstReadFromHead> m_firstReadsFromHeads;
    // findReadFromInList(). By Operation::index(): every read of the value, as they follow each other. Empty until the first question, and for a value until the first question about it.
    mutable ArenaVector<std::span<const uint32_t>> m_listsOfReads;
    // Of isReadOnSomeWayOn(). By Block::index(): the last search, counted from one, that found its value read on a way from the head of the block. Empty until the first.
    ArenaVector<uint32_t> m_lastSearchThatReachedBlock;
    uint32_t m_numberOfSearchesForWays { 0 };
    const Operation* m_valueOfLastSearchForWays { nullptr };
    // AOTMachineAllocatorLoops.cpp fills it at the first question. No other file reads it.
    ArenaVector<uint32_t> m_firstLoopsWhoseHeadsAreNotJudged;
    // Every value that a head holds asks isDeadOnEveryPathFrom() with the same position: the start of the first such loop behind that position, noPosition if there is none.
    Position m_positionOfLastQuestionForLoopNotJudged { noPosition };
    Position m_beginOfFirstLoopNotJudged { noPosition };
    // By Reg::index(): the value that is made again which bringInWhatLoopReads() has last put there, and the end of that loop. m_hints names the register for the value.
    const Operation* m_rematerializedValueKeptIn[64];
    Position m_rematerializedValueIsKeptUntil[64];
    // The greatest of them.
    Position m_endOfLastLoopThatKeepsValueMadeAgain { 0 };
    // By Block::index() of the successor that a Branch with ValueFlag::GoesByOverflow fails to. Empty until the first operand is taken back.
    ArenaVector<OperandTakenBack> m_operandsTakenBackAtHeads;
    // Of the block of the walk, from the sum to the Branch: what Step::other of the Branch names, and the register that the way to the failing successor reads the other operand from.
    Operation* m_operandTakenBackByBranch { nullptr };
    RegisterMask m_registersReadOnWayToFailingSuccessor { 0 };
};

// AOTMachineAllocatorPatches.cpp. Where the one predecessor of the block ends in a Patch and the block is its first successor: what has to follow the definition of its results.
// A result in an outgoing word is reloaded and saved, a narrow reference gets its KeepPointer. It reads the Patch and its PatchAssignment, not m_resultsDefinedAtEndOfPredecessor.
void finishResultsOfPatchThatEndedPredecessor(Allocator&, Block*);

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
