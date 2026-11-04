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

// The function may go on in another frame, so an address in this one is wrong behind the suspension, in a register that is kept too.
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

// Step::destination and Step::sources lie side by side: six Locations in three words. Every Location that is a spill has the one bit of Location::spill(0), and so have a few of other kinds.
// An immediate is of such a kind. It has the bit above too, which belongs to the kind as well and which a spill lacks.
// False: none of the six is a spill.
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

// Order::endOf() the last block of the region that the block of the walk stands in. A region stands in one piece in the order.
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
    // The walk enters a region at its first block.
    ASSERT(!m_order.of(m_block).orderIndex || m_order.of(m_order.blocksInOrder()[m_order.of(m_block).orderIndex - 1]).rareRegion != region);
    m_rareRegion = region;
    // Few regions save anything. Zero, which is the end of no block: the first Save of the region looks for the end.
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

// ValueState::saveInRareRegion stays set to the end of the region, also in blocks that a path reaches without the Save. Only a Save in the block of the walk is known to have run.
// So evict() saves the value again before its register goes, a Patch that takes an operand in memory saves it again while it still sits in its register,
// and a head that an edge fixes binds or spills what lies in a save alone.
void Allocator::saveInRareRegion(Operation* value, Reg from)
{
    ValueState& state = stateOf(value);
    if (state.saveInRareRegion != noSpill || spillHoldsValueIn(m_block, value) || m_liveness.of(value).has(ValueFlag::IsRematerialized))
        return;

    // A step behind the operation that ends the block belongs to no block.
    if (!isDefinedOutsideRareRegion(value) || !value->type().isNumeric() || m_allocation.stepsOf(m_block).terminator != UINT32_MAX) {
        markSpilled(value);
        return;
    }
    ASSERT(m_order.of(m_block).rareRegion == m_rareRegion);

    if (!from || m_valueIn[from.index()] != value) {
        m_proc.fail(FailureKind::RegisterLacksItsValue, value);
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
    // What sank stands at the head of the one region that reads it, whatever block it was appended to.
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
    // Where the framed part is given up in the middle of a rare region, a value may have both. The spill holds on every way.
    SpillIndex spill = m_allocation.spillOf(value);
    if (spillHoldsValueIn(m_block, value))
        return Location::spill(spill);
    // ValueState::saveInRareRegion names a place for the whole region. A value that the heads store may come into the region without a register, be loaded and saved in one block, and be read in a sibling of that block.
    SpillIndex save = m_values[value->index()].saveInRareRegion;
    if (save != noSpill && (spill == noSpill || wayHasSavedInRareRegion(m_block, value)))
        return Location::spill(save);
    if (spill != noSpill) {
        giveUpFramedPartForReadOfSpill();
        return Location::spill(spill);
    }

    // Not every caller looks at the answer.
    m_proc.fail(FailureKind::RegisterLacksItsValue, value);
    return Location();
}

void Allocator::prepareSuspension(PatchOperation* patch)
{
    ASSERT(patch->opcode() == Patch && patch->isSuspension());
    m_allocation.m_suspensions.append(m_arena, patch);

    const RegisterMask kept = registerKeptAcrossSuspension();
    const RegisterMask dying = (m_file.allocatableIn(Bank::GP) | m_file.allocatableIn(Bank::FP)) & ~kept;
    const RegisterMask written = registersWrittenBySuspension(patch);
    // An operand stays in its register until the operation has read it, and behind the operation allocatePatch() clears the registers that the Patch says it writes, and no other.
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
    for (unsigned child = 0; child < patch->numberOfOperands(); ++child) {
        if (patch->constraintOf(child).kind() != ValueRep::LateColdAny)
            continue;
        Operation* root = patch->child(child);
        if (root->hasNoPosition() || (stateOf(root).registers & registersThatCanHoldRoot))
            continue;

        if (stateOf(root).saveInRareRegion == noSpill && !m_allocation.isSpilled(root)) {
            if (isDefinedOutsideRareRegion(root))
                saveInRareRegion(root, anyRegisterOf(root, ~RegisterMask { 0 }));
            else
                markSpilled(root);
        }
        // placeOperandOfPatchUnder() answers with the place, and saves a root again that still sits in its register while its Save stands in another block.
        Location home = memoryLocationOf(root);
        if (!home.isSpill())
            return;

        // The collector reads the word while the function is suspended, also where the suspension is the last to read the value.
        SpillFacts& place = m_allocation.m_spills[home.spillIndex()];
        place.end = std::max(place.end, late);
        place.livesAcrossSuspension = true;
    }
    noteFrameUse();
}

// The old list is left to the arena, as ArenaVector leaves it.
template<typename T>
static NEVER_INLINE T* copyIntoLargerListOfSpillLiveness(Arena& arena, const T* list, uint32_t size, uint32_t newCapacity)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T* larger = arena.allocateArray<T>(newCapacity);
    memcpy(larger, list, static_cast<size_t>(size) * sizeof(T));
    return larger;
}

namespace {

// It goes by the steps in the order in which the encoder writes them, stores behind definitions among them. So it finds from behind what checkAllocation() finds from the front.
class SpillLivenessAtSuspensions {
    WTF_MAKE_NONCOPYABLE(SpillLivenessAtSuspensions);
public:
    // What Allocation shows to Allocator alone.
    struct ListsOfAllocation {
        // The words of Allocation::spillsHeldAt() of the first suspension, behind them those of the second, and so on.
        uint64_t* spillsHeld;
        // By Operation::index(): Allocation::registerAtDefinitionOf().
        const Reg* registerAtDefinition;
    };

    SpillLivenessAtSuspensions(const Allocator&, const ListsOfAllocation&);
    void run();
    BitSpan spillsHeldAtSomeSuspension() const { return BitSpan(std::span<uint64_t>(m_isHeldAtSomeSuspension, m_numberOfWords)); }

private:
    // A step may write a spill one operation ahead of its range: SpillFacts::begin.
    static constexpr uint64_t reachBesideRange = 2;

    // A block is named by its place in Order::blocksInOrder(). The accesses are listed as the steps follow each other: of two in one block the one that is listed first runs first.
    struct Access {
        uint32_t block;
        // The one in front of it among the accesses to the same sixty-four spills. Zero at the start.
        uint32_t previous;
        // Which of the sixty-four.
        uint8_t bit;
        bool isWrite;
    };
    struct PlaceOfSuspension {
        uint32_t block;
        // In m_accesses: what the suspension itself reads is listed in front of this entry, and whatever runs behind the suspension from this entry on.
        uint32_t firstAccessBehind;
    };
    // Of the result of a Patch that ends its block.
    struct StoreAtHead {
        uint32_t block;
        SpillIndex spill;
    };

    bool isNearSuspension(SpillIndex spill) const { return m_isNearSuspension[spill / 64] >> (spill % 64) & 1; }

    void findSpillsNearSuspensions();
    void listAccesses();
    // Few steps name a spill. So listAccesses() keeps in registers what it needs to see that a step names none, and whatever else a step asks for stands out of line and goes by the members.
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
    // Of a set of spills.
    const size_t m_numberOfWords;
    uint64_t* m_isHeldAtSomeSuspension { nullptr };
    uint64_t* m_isNearSuspension { nullptr };
    // By suspension, rising.
    ArenaVector<Position> m_latePositions;
    PlaceOfSuspension* m_places { nullptr };
    // By suspension, of the sixty-four spills that are followed: nothing behind the suspension in its block accesses them. Only where the block accesses one of the sixty-four at all.
    uint64_t* m_spillsNotAccessedBehind { nullptr };

    // Entry zero is not used.
    Access* m_accesses { nullptr };
    uint32_t m_numberOfAccesses { 1 };
    uint32_t m_capacityOfAccesses { 0 };
    // By word of the sets of spills.
    uint32_t* m_lastAccessOfWord { nullptr };
    // Where listAccesses() stands.
    uint32_t m_block { 0 };
    uint32_t m_numberOfSuspensionsPassed { 0 };
    // At the heads of blocks that listAccesses() has yet to come to, and the first of those blocks.
    ArenaVector<StoreAtHead> m_storesAtHeads;
    uint32_t m_nextBlockWithStoreAtHead { UINT32_MAX };
    // By block: m_accesses lists an access in it, or it holds a suspension.
    bool* m_accessesSpillOrSuspends { nullptr };

    const uint32_t* m_predecessors { nullptr };
    // By block, and one more. In m_predecessors: those of a block end where those of the next block begin.
    uint32_t* m_firstPredecessor { nullptr };
    // By block: one more than the last suspension in it. The others of the block stand right in front of that one. Zero if the block has none.
    uint32_t* m_endOfSuspensions { nullptr };
    // By block, a bit for each of the sixty-four spills that are followed together. All are zero between two words of spills.
    uint64_t* m_areAccessed { nullptr };
    uint64_t* m_areLiveAtEnd { nullptr };
    // Live at the head, and the predecessors have not been told.
    uint64_t* m_pendingAtHead { nullptr };
    // A bit for each block: m_pendingAtHead holds something for it.
    uint64_t* m_blocksPending { nullptr };
    // Null where the sets have one word.
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
    // listAccesses() writes every entry of the first, listPredecessors() every entry of the second, and followBackFromReads() each entry of the others ahead of reading it.
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
    // Entry zero is not used.
    if (m_numberOfAccesses > 1) {
        listPredecessors();
        for (uint32_t wordOfSpills = 0; wordOfSpills < m_numberOfWords; ++wordOfSpills)
            followBackFromReads(wordOfSpills);
    }
    holdPointersBesideOperands();
    holdValuesKeptForCollector();
}

// No step reads a spill outside its range, and what lies in one is lost on an edge to a head outside its range. So no other spill is live at a suspension.
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

// `spill` is that of the value.
NEVER_INLINE void SpillLivenessAtSuspensions::noteStoreBehindDefinition(const Operation* value, SpillIndex spill)
{
    if (m_lists.registerAtDefinition[value->index()].isSet())
        noteAccessInLine(spill, true);
}

// `last` ends the block that listAccesses() stands in and has a PatchAssignment, and `spill` is that of its value. The result of a Patch is stored at the head of the first successor, which has no other predecessor.
// Allocator::enterBlock() stops unless the walk has left that predecessor, and Order::placeBlocksSplitLate() puts a block behind the one that leads to it. So listAccesses() has yet to come to the successor.
NEVER_INLINE void SpillLivenessAtSuspensions::noteStoreAtHeadBehindPatch(const Operation* last, SpillIndex spill)
{
    const Block* from = last->owner();
    if (last->opcode() != Patch || !from->numSuccessors() || !m_lists.registerAtDefinition[last->index()].isSet())
        return;
    const Block* target = from->successor(0).block();
    const BlockPlace& place = m_order.of(target);
    // On the edge to a landing pad the results of the Patch do not exist.
    if (target->isLandingPad() || place.predecessors.size() != 1)
        return;
    RELEASE_ASSERT(place.orderIndex > m_block);
    m_storesAtHeads.append(m_arena, StoreAtHead { place.orderIndex, spill });
    m_nextBlockWithStoreAtHead = std::min(m_nextBlockWithStoreAtHead, place.orderIndex);
}

// Of the block that listAccesses() stands in, which is m_nextBlockWithStoreAtHead.
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
    m_lists.spillsHeld[suspension * m_numberOfWords + spill / 64] |= bit;
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
        // The run time finds what a suspension names by its place in the saved frame, so a value that the suspension is the last to read is kept too.
        if (isSuspension)
            hold(m_numberOfSuspensionsPassed, location.spillIndex());
    };
    // An Extract without a register names the spill that its member lies in.
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

// Neither an Execute nor a step from register to register.
NEVER_INLINE void SpillLivenessAtSuspensions::passStepThatAccessesMemory(const Step& step)
{
    switch (step.kind) {
    case StepKind::Reload:
        if (step.sources[0].isSpill())
            noteAccessInLine(step.sources[0].spillIndex(), false);
        return;
    case StepKind::Save:
    case StepKind::ClearSpill:
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

// One pass over the steps: the accesses, and the places of the suspensions among them.
void SpillLivenessAtSuspensions::listAccesses()
{
    Block* const* const blocks = m_order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(m_order.blocksInOrder().size());
    const Step* const steps = m_allocation.steps().data();
    const BlockSteps* const stepsOfBlocks = m_allocation.stepsOfBlocks();
    const SpillIndex* const spillOfValue = m_allocation.spillsOfValues();
    // Each suspension has one, so there is a first.
    const PatchAssignment* const patchAssignments = &m_allocation.patchAssignment(0);

    auto passStep = [&](const Step& step, bool endsBlock) ALWAYS_INLINE_LAMBDA {
        const StepKind kind = step.kind;
        if (kind == StepKind::Execute) [[likely]] {
            const uint32_t assignment = step.patchAssignment;
            // Allocator::allocatePatch() alone appends the Execute of a Patch or a Check, and gives each its PatchAssignment.
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
            // Of the operations that end a block only a Patch defines something.
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

        // The steps of the block, then the moves of its edge, then the operation that ends it: what that one defines is stored at the head of a successor.
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

// The predecessors of each block by their places in the order.
// A block that neither accesses a spill nor suspends, and that one block ahead of it leads to and no other, hands on to that block whatever is live at its end, and nobody asks what is live in it.
// So whatever it leads to lists, in its place, the nearest block above it that is not of that kind.
void SpillLivenessAtSuspensions::listPredecessors()
{
    Block* const* const blocks = m_order.blocksInOrder().data();
    const uint32_t numberOfBlocks = static_cast<uint32_t>(m_order.blocksInOrder().size());
    const BlockPlace* const places = m_order.places();
    const bool* const accessesSpillOrSuspends = m_accessesSpillOrSuspends;
    uint32_t* const firstPredecessor = m_firstPredecessor;
    // By block: the block itself, or the one that is listed in its place. A block asks for the blocks ahead of it alone.
    uint32_t* const blockListedFor = m_arena.allocateArray<uint32_t>(numberOfBlocks);

    // A block has one or two as a rule. There is always room for one for this block and for each that is still to come, so only a block with several asks for room.
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
            // orderBlocks() splits the edge of a block that a Patch ends to a block with several predecessors, unless that is a landing pad. So no result of a Patch is stored at this head: the encoder stops where one would be.
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

// A spill is live ahead of a read, back to the nearest write in the block or to its head. Where it is live at a head it is live at the end of each predecessor,
// and from there back to the last access in that block, or through all of it.
// The spills of one word of the sets are followed together. Values that are defined side by side have spills side by side and are live in much the same blocks,
// so a body with many of both goes through a block once for sixty-four spills and not once for each.
// The last block of the order goes first: but for an edge that leads back, whatever a block leads to has told it everything by the time it is taken.
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
    // Behind the last word nobody looks at the blocks again.
    const bool notesBlocksTouched = wordOfSpills + 1 < numberOfWords;
    uint32_t numberOfBlocksTouched = 0;
    // Of m_blocksPending: how many bits are set, and a word that none of them stands behind.
    uint32_t numberOfBlocksPending = 0;
    uint32_t lastWordOfBlocksPending = 0;
    uint64_t areHeldSomewhere = 0;

    // The accesses from the last to the first, those of one block together. A suspension holds what the next access behind it in its block reads.
    {
        const Access* const accesses = m_accesses;
        const PlaceOfSuspension* const places = m_places;
        const uint32_t* const endOfSuspensions = m_endOfSuspensions;
        uint64_t* const spillsNotAccessedBehind = m_spillsNotAccessedBehind;
        // This word of the first suspension. That of the next one stands numberOfWords further on.
        uint64_t* const spillsHeld = m_lists.spillsHeld + wordOfSpills;
        while (access) {
            const uint32_t block = accesses[access].block;
            // By the accesses of the block that the loop has gone over: those behind the place that it has come to.
            uint64_t areAccessedBehind = 0;
            uint64_t areReadNext = 0;
            uint32_t suspension = endOfSuspensions[block];
            auto passSuspension = [&]() ALWAYS_INLINE_LAMBDA {
                --suspension;
                spillsHeld[suspension * numberOfWords] |= areReadNext;
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
                // Every later one stands ahead of this one.
                if (!numberOfBlocksPending++)
                    lastWordOfBlocksPending = block / 64;
            }
        }
    }

    // What a block tells mostly stands ahead of it, so the search for the last one goes down through the order once, and again from the end of a loop for an edge that leads back.
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

    // And what is live at the end of its block, unless the block accesses it behind the suspension.
    {
        const PlaceOfSuspension* const places = m_places;
        const uint64_t* const spillsNotAccessedBehind = m_spillsNotAccessedBehind;
        uint64_t* spillsHeldHere = m_lists.spillsHeld + wordOfSpills;
        const uint32_t numberOfSuspensions = static_cast<uint32_t>(m_suspensions.size());
        for (uint32_t suspension = 0; suspension < numberOfSuspensions; ++suspension, spillsHeldHere += numberOfWords) {
            const uint32_t block = places[suspension].block;
            uint64_t areHeld = areLiveAtEnd[block];
            if (areAccessed[block])
                areHeld &= spillsNotAccessedBehind[suspension];
            *spillsHeldHere |= areHeld;
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

// No step reads the pointer: it is there for the collector, as long as the reference is read. A reference that is read behind a suspension is a root of it, or checkReferences() refuses the function.
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

// These go by the order, as the scan does that lists them.
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
        // Also where the suspension is the last to read what the value keeps alive. What the suspension itself defines is written behind it.
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
    // The sets of all suspensions lie in one piece, each as BitSpan::create() would make it.
    const size_t numberOfWords = (static_cast<size_t>(m_allocation.m_spills.size()) + 63) / 64;
    std::span<uint64_t> spillsHeld = m_arena.createSpan<uint64_t>(numberOfWords * numberOfSuspensions);
    m_allocation.m_spillsHeldAtSuspension.reserve(m_arena, numberOfSuspensions);
    for (uint32_t suspension = 0; suspension < numberOfSuspensions; ++suspension)
        m_allocation.m_spillsHeldAtSuspension.appendWithinCapacity(BitSpan(spillsHeld.subspan(suspension * numberOfWords, numberOfWords)));

    SpillLivenessAtSuspensions liveness(*this, { spillsHeld.data(), m_allocation.m_registerAtDefinition.begin() });
    liveness.run();
    liveness.spillsHeldAtSomeSuspension().forEachSetBit([&](size_t spill) {
        m_allocation.m_spills[static_cast<SpillIndex>(spill)].livesAcrossSuspension = true;
    });
}

// functor(SpillIndex), for each time a step of the block names a spill.
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

// The value is defined on every path to a block that leads to a pad which reads it. So whatever leaves the blocks up to the last of those comes back to them through the definition, which stores the value again,
// unless it comes round a loop that the value is defined ahead of: such a loop is no part of the gap. checkAllocation() follows every path and finds each read served.
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
    // Positions rise with the order, so a range that ends ahead of it ends in no landing pad.
    const Position headOfFirstLandingPad = places[blocks[firstLandingPad]->index()].head;

    auto indexOfBlockAround = [&](Position position) {
        auto next = std::ranges::upper_bound(blocksInOrder, position, { }, [&](const Block* block) {
            return places[block->index()].head;
        });
        return static_cast<uint32_t>(next - blocksInOrder.begin()) - 1;
    };

    // By spill: the last block that names it or leads to a landing pad that names it. `none`: the spill gets no gap.
    ArenaVector<uint32_t> listOfLastBlocksAheadOfGaps;
    listOfLastBlocksAheadOfGaps.fill(m_arena, numberOfSpills, none);
    uint32_t* const lastBlockAheadOfGap = listOfLastBlocksAheadOfGaps.begin();
    bool someRangeEndsInLandingPad = false;
    // The least of the entries that the loop sets.
    uint32_t firstBlockWhereRangeBegins = numberOfBlocks;
    for (SpillIndex spill = 0; spill < numberOfSpills; ++spill) {
        const SpillFacts& facts = spills[spill];
        if (facts.end < headOfFirstLandingPad || facts.livesAcrossSuspension || facts.isPointerBesideNarrowReference || facts.isSaveInRareRegion || facts.isStoredAtHeadOfFramedPart || facts.end <= facts.begin)
            continue;
        // The moves of the incoming edges write a Phi without a register.
        if (facts.value->opcode() == Phi && !m_allocation.registerAtDefinitionOf(facts.value).isSet())
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

    // An entry only rises, and a block that is no landing pad counts as itself. So none that stands ahead of all the entries changes one.
    for (uint32_t index = std::min(firstBlockWhereRangeBegins, firstLandingPad); index < numberOfBlocks; ++index) {
        const Block* block = blocks[index];
        uint32_t counted = index;
        if (block->isLandingPad()) {
            counted = 0;
            for (const Block* predecessor : places[block->index()].predecessors)
                counted = std::max(counted, places[predecessor->index()].orderIndex);
            // Nothing is known of a pad that something behind it leads to.
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
