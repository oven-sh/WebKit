/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineAllocator.h"

#if ENABLE(AOT)

#include "AOTSavedFrame.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static RegisterMask registerKeptAcrossSuspension()
{
    if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
        return maskOf(Reg(SavedFrame::preservedRegister));
    else
        return 0;
}

static RegisterMask registersWrittenBySuspension(const PatchOperation* suspension)
{
    RegisterMask written = suspension->earlyClobberedMask() | suspension->lateClobberedMask();
    for (const ValueRep& constraint : suspension->resultConstraints.span()) {
        if (constraint.isReg())
            written |= maskOf(constraint.reg());
    }
    return written;
}

static bool isFrameAddressAtSuspension(const Operation* value)
{
    if (!value->hasNoPosition())
        return false;
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    for (const Operation* child : value->children()) {
        if (isFrameAddressAtSuspension(child))
            return true;
    }
    return false;
}

static ALWAYS_INLINE bool stepMayNameSpillBesideItsAssignment(const Step& step)
{
    static_assert(sizeof(Location) == sizeof(uint32_t) && sizeof(Step::sources) == 5 * sizeof(Location));
    static_assert(offsetof(Step, sources) == offsetof(Step, destination) + sizeof(Location) && !(offsetof(Step, destination) % alignof(uint64_t)));
    constexpr uint64_t bitOfSpill = std::bit_cast<uint32_t>(Location::spill(0));
    static_assert(hasOneBitSet(bitOfSpill) && bitOfSpill << 1 <= UINT32_MAX);
    constexpr uint64_t bitsOfSpills = bitOfSpill << 32 | bitOfSpill;
    uint64_t words[3];
    memcpy(words, reinterpret_cast<const uint8_t*>(&step) + offsetof(Step, destination), sizeof(words));
    if (!((words[0] | words[1] | words[2]) & bitsOfSpills))
        return false;
    return ((words[0] & ~(words[0] >> 1)) | (words[1] & ~(words[1] >> 1)) | (words[2] & ~(words[2] >> 1))) & bitsOfSpills;
}

static Position findEndOfRareRegionOfWalk(const Allocator& allocator)
{
    const Order& order = allocator.m_order;
    std::span<Block* const> blocks = order.blocksInOrder();
    uint32_t end = order.of(allocator.m_block).orderIndex + 1;
    while (end < blocks.size() && order.of(blocks[end]).rareRegion == allocator.m_rareRegion)
        ++end;
    return order.endOf(blocks[end - 1]);
}

void Allocator::enterRareRegion(uint32_t region)
{
    ASSERT(!isInRareRegion());
    ASSERT(m_valuesSavedInRareRegion.isEmpty());
    ASSERT(m_order.of(m_block).rareRegion == region);
    ASSERT(!m_order.of(m_block).orderIndex || m_order.of(m_order.blocksInOrder()[m_order.of(m_block).orderIndex - 1]).rareRegion != region);
    m_rareRegion = region;
    m_endOfRareRegion = 0;
}

void Allocator::leaveRareRegion()
{
    ASSERT(isInRareRegion());
    for (Operation* value : m_valuesSavedInRareRegion)
        stateOf(value).saveInRareRegion = noSpill;
    m_valuesSavedInRareRegion.clear();
    m_rareRegion = noRareRegion;
}

void Allocator::saveInRareRegion(Operation* value, Reg from)
{
    ValueState& state = stateOf(value);
    if (state.saveInRareRegion != noSpill || spillHoldsValueIn(m_block, value) || m_liveness.of(value).has(ValueFlag::IsRematerialized))
        return;

    if (!isDefinedOutsideRareRegion(value) || !value->type().isNumeric() || m_allocation.stepsOf(m_block).terminator != UINT32_MAX) {
        markSpilled(value);
        return;
    }
    ASSERT(m_order.of(m_block).rareRegion == m_rareRegion);

    if (!from || m_valueIn[from.index()] != value) {
        m_proc.fail(FailureKind::WrongValueInRegister, value);
        return;
    }

    if (!m_endOfRareRegion)
        m_endOfRareRegion = findEndOfRareRegionOfWalk(*this);

    SpillFacts save;
    save.value = value;
    save.begin = m_position;
    save.end = std::max(m_position, m_endOfRareRegion);
    save.width = widthOf(value->type());
    save.isSaveInRareRegion = true;
    state.saveInRareRegion = addSpill(save);
    m_valuesSavedInRareRegion.append(m_arena, value);

    Step& step = appendStep(StepKind::Save, value);
    step.width = save.width;
    step.destination = Location::spill(state.saveInRareRegion);
    step.sources[0] = Location::reg(from);
    noteFrameUse();
}

bool Allocator::isDefinedOutsideRareRegion(const Operation* value) const
{
    if (!isInRareRegion() || value->hasNoPosition() || m_liveness.isSunk(value))
        return false;
    return m_order.of(value->owner()).rareRegion != m_rareRegion;
}

void Allocator::giveUpFramedPartForReadOfSpill() const
{
    m_allocation.m_needsFrameAheadOfFramedPart = true;
    m_allocation.m_needsFrameOutsideRareRegions = true;
}

Location Allocator::memoryLocationOf(const Operation* value) const
{
    SpillIndex spill = m_allocation.spillOf(value);
    ASSERT(m_isOutsideFramedPart == isOutsideFramedPart(m_block));
    if (spill != noSpill && (!m_isOutsideFramedPart || spillHoldsValueIn(m_block, value)))
        return placeToReadIn(spill);
    SpillIndex save = m_values[value->index()].saveInRareRegion;
    if (save != noSpill && (spill == noSpill || wayHasSavedInRareRegion(m_block, value)))
        return Location::spill(save);
    if (spill != noSpill) {
        giveUpFramedPartForReadOfSpill();
        return placeToReadIn(spill);
    }

    m_proc.fail(FailureKind::WrongValueInRegister, value);
    return Location();
}

void Allocator::prepareSuspension(PatchOperation* patch)
{
    ASSERT(patch->opcode() == Patch && patch->isSuspension());
    m_allocation.m_suspensions.append(m_arena, patch);

    const RegisterMask kept = registerKeptAcrossSuspension();
    const RegisterMask dying = (m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP)) & ~kept;
    const RegisterMask written = registersWrittenBySuspension(patch);
    if (!m_proc.traits().savesFrameOnSuspend || (dying & ~written)) {
        m_proc.fail(FailureKind::MalformedProcedure, patch);
        return;
    }
    ASSERT((m_file.calleeSaved & dying) == m_file.calleeSavedClobberedAtSuspensions);

    RegisterMask registersOfOperands = 0;
    for (const Operation* operand : patch->operands())
        registersOfOperands |= stateOf(operand).registers;
    forEachRegister(dying & m_bound & ~registersOfOperands, [&](Reg reg) {
        evict(reg);
    });
    forEachRegister(kept & m_bound, [&](Reg reg) {
        if (isFrameAddressAtSuspension(m_valueIn[reg.index()]))
            unbind(reg);
    });

    const Position late = m_order.latePositionOf(patch);
    const RegisterMask registersThatCanHoldRoot = kept & ~written;
    const unsigned numberOfOperands = patch->m_operands.size();
    Operation* const* const operands = patch->m_operands.begin();
    const ValueRep* const constraints = patch->m_constraints.begin();
    for (unsigned child = 0; child < numberOfOperands; ++child) {
        if (constraints[child].kind() != ValueRep::LateColdAny)
            continue;
        Operation* root = operands[child];
        const ValueState& state = stateOf(root);
        if (root->hasNoPosition() || (state.registers & registersThatCanHoldRoot))
            continue;

        if (state.saveInRareRegion == noSpill && !m_allocation.isSpilled(root)) {
            if (isDefinedOutsideRareRegion(root))
                saveInRareRegion(root, anyRegisterOf(root, ~RegisterMask { 0 }));
            else
                markSpilled(root);
        }
        Location home = memoryLocationOf(root);
        if (!home.isSpill())
            return;

        SpillFacts& place = m_allocation.m_spills[home.spillIndex()];
        place.end = std::max(place.end, late);
        place.livesAcrossSuspension = true;
    }
    noteFrameUse();
}

template<typename T>
static NEVER_INLINE T* copyIntoLargerListOfSpillLiveness(Arena& arena, const T* list, uint32_t size, uint32_t newCapacity)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T* larger = arena.allocateArray<T>(newCapacity);
    memcpy(larger, list, static_cast<size_t>(size) * sizeof(T));
    return larger;
}

namespace {

class SpillLivenessAtSuspensions {
    WTF_MAKE_NONCOPYABLE(SpillLivenessAtSuspensions);
public:
    struct ListsOfAllocation {
        uint64_t* liveSpills;
        const Reg* registerAtDefinition;
    };

    SpillLivenessAtSuspensions(const Allocator&, const ListsOfAllocation&);
    void run();
    BitSpan spillsHeldAtSomeSuspension() const { return BitSpan(std::span<uint64_t>(m_isHeldAtSomeSuspension, m_numberOfWords)); }

private:
    static constexpr uint64_t reachBesideRange = 2;

    struct Access {
        uint32_t block;
        uint32_t previous;
        uint8_t bit;
        bool isWrite;
    };
    struct PlaceOfSuspension {
        uint32_t block;
        uint32_t firstAccessBehind;
    };
    struct StoreAtHead {
        uint32_t block;
        SpillIndex spill;
    };

    bool isNearSuspension(SpillIndex spill) const { return m_isNearSuspension[spill / 64] >> (spill % 64) & 1; }

    void findSpillsNearSuspensions();
    void listAccesses();
    void noteAccessInLine(SpillIndex, bool isWrite);
    void noteStoreBehindDefinition(const Operation*, SpillIndex);
    void noteStoreAtHeadBehindPatch(const Operation*, SpillIndex);
    void noteStoresAtHeadOfBlock();
    void noteStoresOfPhis(const Block*);
    void passExecuteThatNamesSpillOrSuspends(const Step&);
    void passStepThatAccessesMemory(const Step&);
    void listPredecessors();
    void followBackFromReads(uint32_t wordOfSpills);
    void hold(uint32_t suspension, SpillIndex);
    void holdPointersBesideOperands();
    void holdValuesKeptForCollector();

    Procedure& m_proc;
    Arena& m_arena;
    const Order& m_order;
    const Liveness& m_liveness;
    const Allocation& m_allocation;
    std::span<PatchOperation* const> m_suspensions;
    const ListsOfAllocation m_lists;
    const size_t m_numberOfWords;
    uint64_t* m_isHeldAtSomeSuspension { nullptr };
    uint64_t* m_isNearSuspension { nullptr };
    ArenaVector<Position> m_latePositions;
    PlaceOfSuspension* m_places { nullptr };
    uint64_t* m_spillsNotAccessedBehind { nullptr };

    Access* m_accesses { nullptr };
    uint32_t m_numberOfAccesses { 1 };
    uint32_t m_capacityOfAccesses { 0 };
    uint32_t* m_lastAccessOfWord { nullptr };
    uint32_t m_block { 0 };
    uint32_t m_numberOfSuspensionsPassed { 0 };
    ArenaVector<StoreAtHead> m_storesAtHeads;
    uint32_t m_nextBlockWithStoreAtHead { UINT32_MAX };
    bool* m_accessesSpillOrSuspends { nullptr };

    const uint32_t* m_predecessors { nullptr };
    uint32_t* m_firstPredecessor { nullptr };
    uint32_t* m_endOfSuspensions { nullptr };
    uint64_t* m_areAccessed { nullptr };
    uint64_t* m_areLiveAtEnd { nullptr };
    uint64_t* m_pendingAtHead { nullptr };
    uint64_t* m_blocksPending { nullptr };
    uint32_t* m_blocksTouched { nullptr };
};

SpillLivenessAtSuspensions::SpillLivenessAtSuspensions(const Allocator& allocator, const ListsOfAllocation& lists)
    : m_proc(allocator.m_proc)
    , m_arena(allocator.m_arena)
    , m_order(allocator.m_order)
    , m_liveness(allocator.m_liveness)
    , m_allocation(allocator.m_allocation)
    , m_suspensions(allocator.m_allocation.suspensions())
    , m_lists(lists)
    , m_numberOfWords((allocator.m_allocation.spills().size() + 63) / 64)
{
    const size_t numberOfSuspensions = m_suspensions.size();
    const size_t numberOfBlocks = m_order.blocksInOrder().size();
    m_isHeldAtSomeSuspension = m_arena.createSpan<uint64_t>(m_numberOfWords).data();
    m_isNearSuspension = m_arena.createSpan<uint64_t>(m_numberOfWords).data();
    m_lastAccessOfWord = m_arena.createSpan<uint32_t>(m_numberOfWords).data();
    m_latePositions.reserve(m_arena, static_cast<uint32_t>(numberOfSuspensions));
    for (const PatchOperation* suspension : m_suspensions) {
        Position late = m_order.latePositionOf(suspension);
        RELEASE_ASSERT(m_latePositions.isEmpty() || m_latePositions.last() < late);
        m_latePositions.appendWithinCapacity(late);
    }
    uint64_t* const setsAtBlocks = m_arena.createSpan<uint64_t>(3 * numberOfBlocks).data();
    m_areAccessed = setsAtBlocks;
    m_areLiveAtEnd = setsAtBlocks + numberOfBlocks;
    m_pendingAtHead = setsAtBlocks + 2 * numberOfBlocks;
    m_blocksPending = m_arena.createSpan<uint64_t>(numberOfBlocks / 64 + 1).data();
    m_endOfSuspensions = m_arena.createSpan<uint32_t>(numberOfBlocks).data();
    m_accessesSpillOrSuspends = m_arena.createSpan<bool>(numberOfBlocks).data();
    m_places = m_arena.allocateArray<PlaceOfSuspension>(numberOfSuspensions);
    m_firstPredecessor = m_arena.allocateArray<uint32_t>(numberOfBlocks + 1);
    m_spillsNotAccessedBehind = m_arena.allocateArray<uint64_t>(numberOfSuspensions);
    if (m_numberOfWords > 1)
        m_blocksTouched = m_arena.allocateArray<uint32_t>(numberOfBlocks);
    m_capacityOfAccesses = static_cast<uint32_t>(m_allocation.spills().size()) * 4 + 16;
    m_accesses = m_arena.allocateArray<Access>(m_capacityOfAccesses);
    m_accesses[0] = { };
}

void SpillLivenessAtSuspensions::run()
{
    findSpillsNearSuspensions();
    listAccesses();
    if (m_numberOfAccesses > 1) {
        listPredecessors();
        for (uint32_t wordOfSpills = 0; wordOfSpills < m_numberOfWords; ++wordOfSpills)
            followBackFromReads(wordOfSpills);
    }
    holdPointersBesideOperands();
    holdValuesKeptForCollector();
}

void SpillLivenessAtSuspensions::findSpillsNearSuspensions()
{
    const SpillFacts* const spills = m_allocation.spills().data();
    const SpillIndex numberOfSpills = static_cast<SpillIndex>(m_allocation.spills().size());
    const Position* const begin = m_latePositions.begin();
    const Position* const end = m_latePositions.end();
    uint64_t* const isNearSuspension = m_isNearSuspension;
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        const uint64_t beginOfRange = spills[spill].begin;
        const uint64_t endOfReach = spills[spill].end + reachBesideRange;
        const Position* suspension = std::partition_point(begin, end, [&](Position late) {
            return late + reachBesideRange < beginOfRange;
        });
        if (suspension != end && *suspension <= endOfReach)
            isNearSuspension[spill / 64] |= uint64_t { 1 } << (spill % 64);
    }
}

ALWAYS_INLINE void SpillLivenessAtSuspensions::noteAccessInLine(SpillIndex spill, bool isWrite)
{
    const uint32_t word = spill / 64;
    const uint32_t bit = spill % 64;
    if (!(m_isNearSuspension[word] >> bit & 1))
        return;
    if (m_numberOfAccesses == m_capacityOfAccesses) [[unlikely]] {
        m_capacityOfAccesses *= 2;
        m_accesses = copyIntoLargerListOfSpillLiveness(m_arena, m_accesses, m_numberOfAccesses, m_capacityOfAccesses);
    }
    m_accesses[m_numberOfAccesses] = { m_block, m_lastAccessOfWord[word], static_cast<uint8_t>(bit), isWrite };
    m_lastAccessOfWord[word] = m_numberOfAccesses++;
    m_accessesSpillOrSuspends[m_block] = true;
}

NEVER_INLINE void SpillLivenessAtSuspensions::noteStoreBehindDefinition(const Operation* value, SpillIndex spill)
{
    if (m_lists.registerAtDefinition[value->index()].isSet())
        noteAccessInLine(spill, true);
}

NEVER_INLINE void SpillLivenessAtSuspensions::noteStoreAtHeadBehindPatch(const Operation* last, SpillIndex spill)
{
    const Block* from = last->owner();
    if (last->opcode() != Patch || !from->numSuccessors() || !m_lists.registerAtDefinition[last->index()].isSet())
        return;
    const Block* target = from->successor(0).block();
    const BlockPlace& place = m_order.of(target);
    if (target->isLandingPad() || place.predecessors.size() != 1)
        return;
    RELEASE_ASSERT(place.orderIndex > m_block);
    m_storesAtHeads.append(m_arena, StoreAtHead { place.orderIndex, spill });
    m_nextBlockWithStoreAtHead = std::min(m_nextBlockWithStoreAtHead, place.orderIndex);
}

NEVER_INLINE void SpillLivenessAtSuspensions::noteStoresAtHeadOfBlock()
{
    uint32_t numberOfStoresLeft = 0;
    uint32_t nextBlock = UINT32_MAX;
    for (const StoreAtHead store : m_storesAtHeads) {
        if (store.block == m_block) {
            noteAccessInLine(store.spill, true);
            continue;
        }
        nextBlock = std::min(nextBlock, store.block);
        m_storesAtHeads[numberOfStoresLeft++] = store;
    }
    m_storesAtHeads.shrink(numberOfStoresLeft);
    m_nextBlockWithStoreAtHead = nextBlock;
}

NEVER_INLINE void SpillLivenessAtSuspensions::noteStoresOfPhis(const Block* block)
{
    for (const Operation* phi : block->phis()) {
        if (m_liveness.isDropped(phi))
            continue;
        SpillIndex spill = m_allocation.spillOf(phi);
        if (spill != noSpill && m_lists.registerAtDefinition[phi->index()].isSet())
            noteAccessInLine(spill, true);
    }
}

ALWAYS_INLINE void SpillLivenessAtSuspensions::hold(uint32_t suspension, SpillIndex spill)
{
    const uint64_t bit = uint64_t { 1 } << (spill % 64);
    m_lists.liveSpills[suspension * m_numberOfWords + spill / 64] |= bit;
    m_isHeldAtSomeSuspension[spill / 64] |= bit;
}

NEVER_INLINE void SpillLivenessAtSuspensions::passExecuteThatNamesSpillOrSuspends(const Step& step)
{
    const uint32_t assignment = step.patchAssignment;
    bool isSuspension = false;
    if (assignment != UINT32_MAX) {
        const PatchOperation* patch = step.operation->as<PatchOperation>();
        isSuspension = patch && patch->isSuspension();
        RELEASE_ASSERT(!isSuspension || (m_numberOfSuspensionsPassed < m_suspensions.size() && m_suspensions.data()[m_numberOfSuspensionsPassed] == patch));
    }
    auto noteRead = [&](Location location) ALWAYS_INLINE_LAMBDA {
        if (!location.isSpill())
            return;
        noteAccessInLine(location.spillIndex(), false);
        if (isSuspension)
            hold(m_numberOfSuspensionsPassed, location.spillIndex());
    };
    noteRead(step.destination);
    for (Location source : step.sources)
        noteRead(source);
    if (assignment != UINT32_MAX) {
        for (Location location : m_allocation.patchAssignment(assignment).locations)
            noteRead(location);
    }
    if (isSuspension) {
        m_places[m_numberOfSuspensionsPassed] = { m_block, m_numberOfAccesses };
        m_endOfSuspensions[m_block] = ++m_numberOfSuspensionsPassed;
        m_accessesSpillOrSuspends[m_block] = true;
    }
}

NEVER_INLINE void SpillLivenessAtSuspensions::passStepThatAccessesMemory(const Step& step)
{
    switch (step.kind) {
    case StepKind::Reload:
        if (step.sources[0].isSpill())
            noteAccessInLine(step.sources[0].spillIndex(), false);
        return;
    case StepKind::Save:
    case StepKind::KeepPointer:
        if (step.destination.isSpill())
            noteAccessInLine(step.destination.spillIndex(), true);
        return;
    case StepKind::Execute:
    case StepKind::Move:
    case StepKind::Materialize:
    case StepKind::Swap:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SpillLivenessAtSuspensions::listAccesses()
{
    Block* const* const blocks = m_order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(m_order.blocksInOrder().size());
    const Step* const steps = m_allocation.steps().data();
    const BlockSteps* const stepsOfBlocks = m_allocation.stepsOfBlocks();
    const SpillIndex* const spillOfValue = m_allocation.spillsOfValues();
    const PatchAssignment* const patchAssignments = &m_allocation.patchAssignment(0);

    auto passStep = [&](const Step& step, bool endsBlock) ALWAYS_INLINE_LAMBDA {
        const StepKind kind = step.kind;
        if (kind == StepKind::Execute) [[likely]] {
            const uint32_t assignment = step.patchAssignment;
            ASSERT(assignment != UINT32_MAX || !step.operation->as<PatchOperation>());
            bool namesSpillOrSuspends = stepMayNameSpillBesideItsAssignment(step);
            if (assignment != UINT32_MAX && !namesSpillOrSuspends) {
                const PatchOperation* patch = step.operation->as<PatchOperation>();
                namesSpillOrSuspends = patch && patch->isSuspension();
                for (Location location : patchAssignments[assignment].locations) {
                    if (location.isSpill()) {
                        namesSpillOrSuspends = true;
                        break;
                    }
                }
            }
            if (namesSpillOrSuspends) [[unlikely]]
                passExecuteThatNamesSpillOrSuspends(step);
            if (endsBlock && assignment == UINT32_MAX)
                return;
            const Operation* operation = step.operation;
            const SpillIndex spill = spillOfValue[operation->index()];
            if (spill != noSpill) [[unlikely]] {
                if (endsBlock)
                    noteStoreAtHeadBehindPatch(operation, spill);
                else
                    noteStoreBehindDefinition(operation, spill);
            }
            return;
        }
        if (kind != StepKind::Move && kind != StepKind::Materialize && kind != StepKind::Swap)
            passStepThatAccessesMemory(step);
    };

    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        const Block* block = blocks[index];
        m_block = index;
        if (!block->phis().empty()) [[unlikely]]
            noteStoresOfPhis(block);
        if (index == m_nextBlockWithStoreAtHead) [[unlikely]]
            noteStoresAtHeadOfBlock();

        const BlockSteps& stepsOfBlock = stepsOfBlocks[block->index()];
        for (const Step* step = steps + stepsOfBlock.begin, *end = steps + stepsOfBlock.end; step < end; ++step)
            passStep(*step, false);
        if (stepsOfBlock.beginOfEdgeMoves < stepsOfBlock.endOfEdgeMoves) {
            for (const Step* step = steps + stepsOfBlock.beginOfEdgeMoves, *end = steps + stepsOfBlock.endOfEdgeMoves; step < end; ++step)
                passStep(*step, false);
        }
        passStep(steps[stepsOfBlock.terminator], true);
    }
    RELEASE_ASSERT(m_numberOfSuspensionsPassed == m_suspensions.size());
    ASSERT(m_storesAtHeads.isEmpty());
}

void SpillLivenessAtSuspensions::listPredecessors()
{
    Block* const* const blocks = m_order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(m_order.blocksInOrder().size());
    const BlockPlace* const places = m_order.places();
    const bool* const accessesSpillOrSuspends = m_accessesSpillOrSuspends;
    uint32_t* const firstPredecessor = m_firstPredecessor;
    uint32_t* const blockListedFor = m_arena.allocateArray<uint32_t>(numberOfBlocks);

    uint32_t capacityOfPredecessors = numberOfBlocks * 2;
    uint32_t* predecessors = m_arena.allocateArray<uint32_t>(capacityOfPredecessors);
    uint32_t numberOfPredecessors = 0;

    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        const Block* block = blocks[index];
        const BlockPlace& place = places[block->index()];
        Block* const* const predecessorsOfBlock = place.predecessors.data();
        const size_t numberOfPredecessorsOfBlock = place.predecessors.size();
        firstPredecessor[index] = numberOfPredecessors;
        uint32_t listedForBlock = index;
        if (numberOfPredecessorsOfBlock == 1) [[likely]] {
            uint32_t predecessor = places[predecessorsOfBlock[0]->index()].orderIndex;
            if (predecessor < index) [[likely]] {
                predecessor = blockListedFor[predecessor];
                if (!accessesSpillOrSuspends[index])
                    listedForBlock = predecessor;
            }
            predecessors[numberOfPredecessors++] = predecessor;
        } else {
            ASSERT(block->isLandingPad() || std::ranges::none_of(place.predecessors, [&](const Block* predecessor) {
                return predecessor->last()->opcode() == Patch && predecessor->successor(0).block() == block;
            }));
            const uint32_t numberOfBlocksToCome = numberOfBlocks - index - 1;
            if (numberOfPredecessorsOfBlock > capacityOfPredecessors - numberOfPredecessors - numberOfBlocksToCome) [[unlikely]] {
                capacityOfPredecessors = (capacityOfPredecessors + static_cast<uint32_t>(numberOfPredecessorsOfBlock)) * 2;
                predecessors = copyIntoLargerListOfSpillLiveness(m_arena, predecessors, numberOfPredecessors, capacityOfPredecessors);
            }
            for (size_t number = 0; number < numberOfPredecessorsOfBlock; ++number) {
                const uint32_t predecessor = places[predecessorsOfBlock[number]->index()].orderIndex;
                predecessors[numberOfPredecessors++] = predecessor < index ? blockListedFor[predecessor] : predecessor;
            }
        }
        blockListedFor[index] = listedForBlock;
    }
    firstPredecessor[numberOfBlocks] = numberOfPredecessors;
    m_predecessors = predecessors;
}

void SpillLivenessAtSuspensions::followBackFromReads(uint32_t wordOfSpills)
{
    uint32_t access = m_lastAccessOfWord[wordOfSpills];
    if (!access)
        return;

    uint64_t* const areAccessed = m_areAccessed;
    uint64_t* const areLiveAtEnd = m_areLiveAtEnd;
    uint64_t* const pendingAtHead = m_pendingAtHead;
    uint64_t* const blocksPending = m_blocksPending;
    uint32_t* const blocksTouched = m_blocksTouched;
    const size_t numberOfWords = m_numberOfWords;
    const bool notesBlocksTouched = wordOfSpills + 1 < numberOfWords;
    uint32_t numberOfBlocksTouched = 0;
    uint32_t numberOfBlocksPending = 0;
    uint32_t lastWordOfBlocksPending = 0;
    uint64_t areHeldSomewhere = 0;

    {
        const Access* const accesses = m_accesses;
        const PlaceOfSuspension* const places = m_places;
        const uint32_t* const endOfSuspensions = m_endOfSuspensions;
        uint64_t* const spillsNotAccessedBehind = m_spillsNotAccessedBehind;
        uint64_t* const liveSpills = m_lists.liveSpills + wordOfSpills;
        while (access) {
            const uint32_t block = accesses[access].block;
            uint64_t areAccessedBehind = 0;
            uint64_t areReadNext = 0;
            uint32_t suspension = endOfSuspensions[block];
            auto passSuspension = [&]() ALWAYS_INLINE_LAMBDA {
                --suspension;
                liveSpills[suspension * numberOfWords] |= areReadNext;
                areHeldSomewhere |= areReadNext;
                spillsNotAccessedBehind[suspension] = ~areAccessedBehind;
            };
            do {
                const Access& current = accesses[access];
                while (suspension && places[suspension - 1].block == block && places[suspension - 1].firstAccessBehind > access)
                    passSuspension();
                const uint64_t bit = uint64_t { 1 } << current.bit;
                areAccessedBehind |= bit;
                areReadNext = current.isWrite ? areReadNext & ~bit : areReadNext | bit;
                access = current.previous;
            } while (access && accesses[access].block == block);
            while (suspension && places[suspension - 1].block == block)
                passSuspension();

            areAccessed[block] = areAccessedBehind;
            if (notesBlocksTouched)
                blocksTouched[numberOfBlocksTouched++] = block;
            if (areReadNext) {
                pendingAtHead[block] = areReadNext;
                blocksPending[block / 64] |= uint64_t { 1 } << (block % 64);
                if (!numberOfBlocksPending++)
                    lastWordOfBlocksPending = block / 64;
            }
        }
    }

    {
        const uint32_t* const predecessors = m_predecessors;
        const uint32_t* const firstPredecessor = m_firstPredecessor;
        while (numberOfBlocksPending) {
            uint64_t pendingInWord = blocksPending[lastWordOfBlocksPending];
            while (!pendingInWord)
                pendingInWord = blocksPending[--lastWordOfBlocksPending];
            const uint32_t bitOfBlock = 63 - static_cast<uint32_t>(std::countl_zero(pendingInWord));
            blocksPending[lastWordOfBlocksPending] = pendingInWord ^ uint64_t { 1 } << bitOfBlock;
            const uint32_t left = lastWordOfBlocksPending * 64 + bitOfBlock;
            --numberOfBlocksPending;
            const uint64_t liveAtHead = std::exchange(pendingAtHead[left], 0);
            const uint32_t* predecessor = predecessors + firstPredecessor[left];
            const uint32_t* const endOfPredecessors = predecessors + firstPredecessor[left + 1];
            for (; predecessor != endOfPredecessors; ++predecessor) {
                const uint32_t block = *predecessor;
                const uint64_t wereLiveAtEnd = areLiveAtEnd[block];
                const uint64_t found = liveAtHead & ~wereLiveAtEnd;
                if (!found)
                    continue;
                const uint64_t accessed = areAccessed[block];
                const uint64_t pending = pendingAtHead[block];
                if (notesBlocksTouched && !(wereLiveAtEnd | accessed | pending))
                    blocksTouched[numberOfBlocksTouched++] = block;
                areLiveAtEnd[block] = wereLiveAtEnd | found;
                const uint64_t liveThroughBlock = found & ~accessed;
                if (!liveThroughBlock)
                    continue;
                if (!pending) {
                    ++numberOfBlocksPending;
                    blocksPending[block / 64] |= uint64_t { 1 } << (block % 64);
                    lastWordOfBlocksPending = std::max(lastWordOfBlocksPending, block / 64);
                }
                pendingAtHead[block] = pending | liveThroughBlock;
            }
        }
    }

    {
        const PlaceOfSuspension* const places = m_places;
        const uint64_t* const spillsNotAccessedBehind = m_spillsNotAccessedBehind;
        uint64_t* liveSpillsHere = m_lists.liveSpills + wordOfSpills;
        const uint32_t numberOfSuspensions = static_cast<uint32_t>(m_suspensions.size());
        for (uint32_t suspension = 0; suspension < numberOfSuspensions; ++suspension, liveSpillsHere += numberOfWords) {
            const uint32_t block = places[suspension].block;
            uint64_t areHeld = areLiveAtEnd[block];
            if (areAccessed[block])
                areHeld &= spillsNotAccessedBehind[suspension];
            *liveSpillsHere |= areHeld;
            areHeldSomewhere |= areHeld;
        }
    }
    m_isHeldAtSomeSuspension[wordOfSpills] |= areHeldSomewhere;

    for (uint32_t index = 0; index < numberOfBlocksTouched; ++index) {
        const uint32_t block = blocksTouched[index];
        areAccessed[block] = 0;
        areLiveAtEnd[block] = 0;
        pendingAtHead[block] = 0;
    }
}

void SpillLivenessAtSuspensions::holdPointersBesideOperands()
{
    std::span<const SpillFacts> spills = m_allocation.spills();
    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        if (!spills[spill].isPointerBesideNarrowReference || !isNearSuspension(spill))
            continue;
        for (uint32_t use = m_liveness.of(spills[spill].value).firstUse; use; use = m_liveness.use(use).next) {
            Position lateOfReader = m_liveness.use(use).position + 1;
            auto suspension = std::lower_bound(m_latePositions.begin(), m_latePositions.end(), lateOfReader);
            if (suspension != m_latePositions.end() && *suspension == lateOfReader)
                hold(static_cast<uint32_t>(suspension - m_latePositions.begin()), spill);
        }
    }
}

void SpillLivenessAtSuspensions::holdValuesKeptForCollector()
{
    std::span<const ValueKeptForCollector> valuesKept = m_liveness.valuesKeptForCollector();
    if (valuesKept.empty())
        return;
    ArenaVector<Position> keptUntil;
    keptUntil.fill(m_arena, m_proc.numberOfOperations(), 0);
    for (const ValueKeptForCollector& kept : valuesKept)
        keptUntil[kept.value->index()] = std::max(keptUntil[kept.value->index()], kept.until);

    std::span<const SpillFacts> spills = m_allocation.spills();
    for (SpillIndex spill = 0; spill < spills.size(); ++spill) {
        const SpillFacts& facts = spills[spill];
        Position until = keptUntil[facts.value->index()];
        if (!until || !isNearSuspension(spill))
            continue;
        uint64_t lastLatePosition = static_cast<uint64_t>(std::min(until, facts.end)) + 1;
        auto suspension = std::upper_bound(m_latePositions.begin(), m_latePositions.end(), facts.begin);
        for (; suspension != m_latePositions.end() && *suspension <= lastLatePosition; ++suspension)
            hold(static_cast<uint32_t>(suspension - m_latePositions.begin()), spill);
    }
}

} // anonymous namespace

void Allocator::findSpillsHeldAtSuspensions()
{
    uint32_t numberOfSuspensions = m_allocation.m_suspensions.size();
    if (!numberOfSuspensions)
        return;
    const size_t numberOfWords = (static_cast<size_t>(m_allocation.m_spills.size()) + 63) / 64;
    std::span<uint64_t> liveSpills = m_arena.createSpan<uint64_t>(numberOfWords * numberOfSuspensions);
    m_allocation.m_liveSpillsAtSuspension.reserve(m_arena, numberOfSuspensions);
    for (uint32_t suspension = 0; suspension < numberOfSuspensions; ++suspension)
        m_allocation.m_liveSpillsAtSuspension.appendWithinCapacity(BitSpan(liveSpills.subspan(suspension * numberOfWords, numberOfWords)));

    SpillLivenessAtSuspensions liveness(*this, { liveSpills.data(), m_allocation.m_registerAtDefinition.begin() });
    liveness.run();
    liveness.spillsHeldAtSomeSuspension().forEachSetBit([&](size_t spill) {
        m_allocation.m_spills[static_cast<SpillIndex>(spill)].livesAcrossSuspension = true;
    });
}

template<typename Functor>
static ALWAYS_INLINE void forEachSpillNamedIn(const Allocation& allocation, const Block* block, const Functor& functor)
{
    auto pass = [&](const Step& step) ALWAYS_INLINE_LAMBDA {
        if (stepMayNameSpillBesideItsAssignment(step)) {
            if (step.destination.isSpill())
                functor(step.destination.spillIndex());
            for (Location source : step.sources) {
                if (source.isSpill())
                    functor(source.spillIndex());
            }
        }
        if (step.patchAssignment == UINT32_MAX)
            return;
        for (Location location : allocation.patchAssignment(step.patchAssignment).locations) {
            if (location.isSpill())
                functor(location.spillIndex());
        }
    };
    const Step* const steps = allocation.steps().data();
    const BlockSteps& stepsOfBlock = allocation.stepsOf(block);
    for (const Step* step = steps + stepsOfBlock.begin, *end = steps + stepsOfBlock.end; step < end; ++step)
        pass(*step);
    for (const Step* step = steps + stepsOfBlock.beginOfEdgeMoves, *end = steps + stepsOfBlock.endOfEdgeMoves; step < end; ++step)
        pass(*step);
    pass(steps[stepsOfBlock.terminator]);
}

void Allocator::findGapsAheadOfRarePhis()
{
    for (SpillFacts& spill : m_allocation.m_spills) {
        const Operation* phi = spill.value;
        if (!phi || phi->opcode() != Phi || spill.hasGap() || spill.isSharedByPhis || spill.isSaveInRareRegion || spill.isPointerBesideNarrowReference || spill.isStoredAtHeadOfFramedPart || spill.livesAcrossSuspension)
            continue;
        const BlockPlace& place = m_order.of(phi->owner());
        const ValueFacts& facts = m_valueFacts[phi->index()];
        if (!place.isRare() || facts.definition == noPosition || spill.begin >= facts.definition || facts.has(ValueFlag::IsKeptForCollector) || phi->owner()->isLandingPad())
            continue;
        Position first = facts.definition;
        Position second = 0;
        Position latest = 0;
        for (const Block* predecessor : place.predecessors) {
            Position behindMoves = m_order.endOf(predecessor);
            first = std::min(first, behindMoves - 2);
            if (behindMoves > facts.definition || behindMoves == latest)
                continue;
            if (behindMoves > latest)
                second = std::exchange(latest, behindMoves);
            else
                second = std::max(second, behindMoves);
        }
        if (first != spill.begin || !latest)
            continue;
        bool isAheadOfLatest = second && latest - second > facts.definition - latest + 2;
        Position begin = isAheadOfLatest ? second : latest;
        Position end = isAheadOfLatest ? latest - 2 : facts.definition;
        if (begin >= end || end > spill.end)
            continue;
        spill.beginOfGap = begin;
        spill.endOfGap = end - 1;
    }
}

void Allocator::findGapsAheadOfLandingPads()
{
    constexpr uint32_t none = UINT32_MAX;
    std::span<Block* const> blocksInOrder = m_order.blocksInOrder();
    const SpillIndex numberOfSpills = m_allocation.m_spills.size();
    if (!numberOfSpills || !m_liveness.hasLandingPads())
        return;
    Block* const* const blocks = blocksInOrder.data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(blocksInOrder.size());
    uint32_t firstLandingPad = 0;
    while (firstLandingPad < numberOfBlocks && !blocks[firstLandingPad]->isLandingPad())
        ++firstLandingPad;
    if (firstLandingPad == numberOfBlocks)
        return;
    const BlockPlace* const places = m_order.places();
    SpillFacts* const spills = m_allocation.m_spills.begin();
    const Position headOfFirstLandingPad = places[blocks[firstLandingPad]->index()].head;

    auto indexOfBlockAround = [&](Position position) {
        auto next = std::ranges::upper_bound(blocksInOrder, position, { }, [&](const Block* block) {
            return places[block->index()].head;
        });
        return static_cast<uint32_t>(next - blocksInOrder.begin()) - 1;
    };

    ArenaVector<uint32_t> listOfLastBlocksAheadOfGaps;
    listOfLastBlocksAheadOfGaps.fill(m_arena, numberOfSpills, none);
    uint32_t* const lastBlockAheadOfGap = listOfLastBlocksAheadOfGaps.begin();
    bool someRangeEndsInLandingPad = false;
    uint32_t firstBlockWhereRangeBegins = numberOfBlocks;
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        const SpillFacts& facts = spills[spill];
        if (facts.end < headOfFirstLandingPad || facts.isPointerBesideNarrowReference || facts.isSaveInRareRegion || facts.isStoredAtHeadOfFramedPart || facts.end <= facts.begin)
            continue;
        if (facts.value->opcode() == Phi && (!m_allocation.registerAtDefinitionOf(facts.value).isSet() || m_allocation.isWrittenByEdges(facts.value)))
            continue;
        if (m_liveness.of(facts.value).has(ValueFlag::IsKeptForCollector))
            continue;
        if (!blocks[indexOfBlockAround(facts.end)]->isLandingPad())
            continue;
        lastBlockAheadOfGap[spill] = indexOfBlockAround(facts.begin);
        firstBlockWhereRangeBegins = std::min(firstBlockWhereRangeBegins, lastBlockAheadOfGap[spill]);
        someRangeEndsInLandingPad = true;
    }
    if (!someRangeEndsInLandingPad)
        return;

    for (uint32_t index = std::min(firstBlockWhereRangeBegins, firstLandingPad); index < numberOfBlocks; ++index) {
        const Block* block = blocks[index];
        uint32_t counted = index;
        if (block->isLandingPad()) {
            counted = 0;
            for (const Block* predecessor : places[block->index()].predecessors)
                counted = std::max(counted, places[predecessor->index()].orderIndex);
            if (counted > index)
                counted = numberOfBlocks - 1;
        }
        forEachSpillNamedIn(m_allocation, block, [&](SpillIndex spill) {
            if (lastBlockAheadOfGap[spill] != none)
                lastBlockAheadOfGap[spill] = std::max(lastBlockAheadOfGap[spill], counted);
        });
    }

    const Loop* const loops = m_order.loops().data();
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        uint32_t& last = lastBlockAheadOfGap[spill];
        if (last == none)
            continue;
        for (uint32_t loop = places[blocks[last]->index()].loop; loop != noLoop && loops[loop].begin > spills[spill].begin; loop = loops[loop].parent)
            last = std::max(last, loops[loop].endBlock - 1);
    }

    for (uint32_t index = firstLandingPad; index < numberOfBlocks; ++index) {
        if (!blocks[index]->isLandingPad())
            continue;
        forEachSpillNamedIn(m_allocation, blocks[index], [&](SpillIndex spill) {
            SpillFacts& facts = spills[spill];
            uint32_t& last = lastBlockAheadOfGap[spill];
            if (last == none || facts.hasGap() || index <= last)
                return;
            if (index == last + 1) {
                last = index;
                return;
            }
            facts.beginOfGap = places[blocks[last + 1]->index()].head;
            facts.endOfGap = places[blocks[index]->index()].head - 1;
        });
    }
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
