/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachine.h"
#include "AOTInliningAcrossFiles.h"

#if ENABLE(AOT)

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Machine {

static constexpr std::array<uint16_t, sizeOfTableByOpcode> machineCoreChildFlags = [] {
    std::array<uint16_t, sizeOfTableByOpcode> flags { };
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        if (opcode != Trunc)
            flags[index] |= static_cast<uint16_t>(OperationFlag::UpperHalfIsRead);
        if (!readsZeroExtendedWord(opcode, 1))
            flags[index] |= static_cast<uint16_t>(OperationFlag::HasOpaqueReader);
    }
    return flags;
}();

static bool machineCoreCountsChildrenAtThisRead(const Operation* operation)
{
    return !operation->useCount() && operation->owner() && countsChildrenAtFirstUse(operation->opcode());
}

void Operation::countChildrenAtFirstUse()
{
    Operation* operation = this;
    do {
        ASSERT(operation->m_owner && countsChildrenAtFirstUse(operation->m_opcode));
        uint16_t flagsOfChildren = machineCoreChildFlags[operation->m_opcode];
        Operation* childReadFirst = nullptr;
        Operation* secondChildReadFirst = nullptr;
        Operation* thirdChildReadFirst = nullptr;
        for (Operation* child : std::span<Operation* const>(operation->m_children, operation->m_auxiliary)) {
            child->m_flags |= flagsOfChildren;
            if (child->m_useCount) {
                if (child->m_useCount != std::numeric_limits<uint16_t>::max())
                    ++child->m_useCount;
                continue;
            }
            child->m_useCount = 1;
            if (!child->m_owner || !countsChildrenAtFirstUse(child->m_opcode))
                continue;
            if (childReadFirst) [[unlikely]] {
                if (secondChildReadFirst)
                    thirdChildReadFirst = child;
                else
                    secondChildReadFirst = child;
                continue;
            }
            childReadFirst = child;
        }
        if (secondChildReadFirst) [[unlikely]]
            return countChildrenOfSeveralAtFirstUse(childReadFirst, secondChildReadFirst, thirdChildReadFirst);
        operation = childReadFirst;
    } while (operation);
}

NEVER_INLINE void Operation::countChildrenOfSeveralAtFirstUse(Operation* first, Operation* second, Operation* thirdOrNull)
{
    first->countChildrenAtFirstUseWithWorklist();
    second->countChildrenAtFirstUseWithWorklist();
    if (thirdOrNull)
        thirdOrNull->countChildrenAtFirstUseWithWorklist();
}

NEVER_INLINE void Operation::countChildrenAtFirstUseWithWorklist()
{
    Vector<Operation*, 16> operationsReadFirst;
    Operation* operation = this;
    for (;;) {
        ASSERT(operation->m_owner && countsChildrenAtFirstUse(operation->m_opcode));
        uint16_t flagsOfChildren = machineCoreChildFlags[operation->m_opcode];
        for (Operation* child : std::span<Operation* const>(operation->m_children, operation->m_auxiliary)) {
            child->m_flags |= flagsOfChildren;
            if (child->m_useCount) {
                if (child->m_useCount != std::numeric_limits<uint16_t>::max())
                    ++child->m_useCount;
                continue;
            }
            child->m_useCount = 1;
            if (child->m_owner && countsChildrenAtFirstUse(child->m_opcode))
                operationsReadFirst.append(child);
        }
        if (operationsReadFirst.isEmpty())
            return;
        operation = operationsReadFirst.takeLast();
    }
}

void Operation::loseUse()
{
    Vector<Operation*, 16> pendingReads;
    pendingReads.append(this);
    while (!pendingReads.isEmpty()) {
        Operation* operation = pendingReads.takeLast();
        if (operation->m_useCount == std::numeric_limits<uint16_t>::max())
            continue;
        ASSERT(operation->m_useCount);
        --operation->m_useCount;
        if (machineCoreCountsChildrenAtThisRead(operation)) {
            for (Operation* child : operation->children())
                pendingReads.append(child);
        }
    }
}

void ResultConstraints::appendNoneOrSeveral(std::span<const ValueRep> reps)
{
    m_reps.reserve(*m_arena, static_cast<uint32_t>(reps.size()));
    for (const ValueRep& rep : reps)
        m_reps.appendWithinCapacity(rep);
}

NEVER_INLINE void PatchOperation::growStorageOfOperands()
{
    Arena& arena = m_procedure.arena();
    uint32_t capacity = std::max(firstCapacityOfOperands, m_operands.capacity() * 2);
    m_operands.reserve(arena, capacity);
    m_constraints.reserve(arena, capacity);
}

FactsOfSuspension& PatchOperation::ensureFactsOfSuspension()
{
    if (!m_factsOfSuspension)
        m_factsOfSuspension = m_procedure.arena().create<FactsOfSuspension>();
    return *m_factsOfSuspension;
}

ALWAYS_INLINE_INTO_OTHER_FILES void PatchOperation::leaveOperandsWhereTheyLie()
{
    RELEASE_ASSERT(m_opcode == Patch && bringsOperandsToTheirRegisters() && m_registersToBringOperandsTo.isEmpty());
    if (numGPScratchRegisters)
        return;
    for (const ValueRep& constraint : m_constraints) {
        if (constraint.kind() == ValueRep::SomeRegister || constraint.kind() == ValueRep::SomeLateRegister || constraint.kind() == ValueRep::WarmAny)
            return;
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const uint32_t numberOfOperands = m_operands.size();
    Operation* const* const operands = m_operands.begin();
    ValueRep* const constraints = m_constraints.begin();
    Reg* registers = nullptr;
    for (uint32_t child = 0; child < numberOfOperands; ++child) {
        ValueRep& constraint = constraints[child];
        bool isLeftWhereItLies = constraint.isGPR() && (!operands[child]->hasNoPosition() || operands[child]->isConstant());
        if (!registers) {
            if (!isLeftWhereItLies)
                continue;
            m_registersToBringOperandsTo.growWithoutInitializing(m_procedure.arena(), numberOfOperands);
            registers = m_registersToBringOperandsTo.begin();
            std::fill_n(registers, child, Reg());
        }
        if (!isLeftWhereItLies) {
            registers[child] = Reg();
            continue;
        }
        registers[child] = constraint.reg();
        constraint = ValueRep::ColdAny;
    }
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
}

void PatchOperation::setReads(std::span<const AbstractHeap* const> heaps)
{
    effects.reads = m_procedure.heapSet(heaps);
}

void PatchOperation::setWrites(std::span<const AbstractHeap* const> heaps)
{
    effects.writes = m_procedure.heapSet(heaps);
}

void PatchOperation::clobberLateOnSuccessor(unsigned successorIndex, const RegisterSet& set)
{
    RELEASE_ASSERT(m_opcode == Patch && m_owner && m_owner->last() == this);
    RELEASE_ASSERT(successorIndex < m_owner->numSuccessors());
    m_owner->successor(successorIndex).block()->m_isLandingPad = true;

    RegisterMask mask = maskOf(set);
    for (SuccessorClobber& clobber : m_successorClobbers) {
        if (clobber.successorIndex == successorIndex) {
            clobber.mask |= mask;
            return;
        }
    }
    m_successorClobbers.append(m_procedure.arena(), SuccessorClobber { successorIndex, mask });
}

RegisterMask PatchOperation::lateClobberedOnSuccessor(unsigned successorIndex) const
{
    for (const SuccessorClobber& clobber : m_successorClobbers) {
        if (clobber.successorIndex == successorIndex)
            return clobber.mask;
    }
    return 0;
}

void Block::setSuccessors(Edge target)
{
    setSuccessorsInline(target);
}

void Block::setSuccessors(Edge taken, Edge notTaken)
{
    setSuccessorsInline(taken, notTaken);
}

void Block::clearSuccessors()
{
    for (const Edge& edge : m_successors)
        edge.block()->didLoseIncomingEdge(this, edge.edgeClass());
    m_successors.clear();
}

Procedure::Procedure(Arena& arena, const HeapTree& heapTree, const ProcedureTraits& traits)
    : m_arena(arena)
    , m_heapTree(heapTree)
    , m_traits(traits)
{
    m_blocks.reserve(arena, 16);
    m_operations.reserve(arena, 64);
}

void Procedure::fail(FailureKind kind, const Operation* operation, PlaceOfCheck check)
{
    if (m_failure)
        return;
    if (operation)
        m_failure = Failure { kind, operation->origin(), operation->index(), check };
    else
        m_failure = Failure { kind, Origin(), UINT32_MAX, check };
}

void Procedure::noteRoomThatBuildingLeaves(ListThatBuildingLeaves list, void* room, size_t bytes)
{
    RELEASE_ASSERT(!m_order);
    if (!m_roomsThatBuildingLeaves)
        m_roomsThatBuildingLeaves = m_arena.create<RoomsThatBuildingLeaves>();
    (*m_roomsThatBuildingLeaves)[static_cast<unsigned>(list)] = { room, bytes };
}

void Procedure::letGoOfWhatBuildingLeaves()
{
    for (const RoomThatBuildingLeaves& left : *std::exchange(m_roomsThatBuildingLeaves, nullptr))
        m_arena.letGo(left.room, left.bytes);
}

NEVER_INLINE Block* Procedure::addBlock()
{
    return addBlockInline();
}

NEVER_INLINE Block* Procedure::addBlockThatOnlyJumps()
{
    return addBlockWithRoomForOperations<1>();
}

static bool wayLeadsFromEntrypointInto(const Procedure& procedure, const Block* asked)
{
    BitVector isReached(procedure.numberOfBlocks());
    Vector<const Block*, 16> toLookAt;
    auto reach = [&](const Block* block) {
        if (!isReached.set(block->index()))
            toLookAt.append(block);
    };
    for (const Entrypoint& entrypoint : procedure.entrypoints())
        reach(entrypoint.block);
    while (!toLookAt.isEmpty()) {
        const Block* block = toLookAt.takeLast();
        if (block == asked)
            return true;
        for (const Edge& edge : block->successors())
            reach(edge.block());
    }
    return false;
}

void Procedure::stopAtStoreOverLoadThatStands(const Operation* store)
{
    if (wayLeadsFromEntrypointInto(*this, store->owner()))
        fail(FailureKind::MalformedProcedure, store);
    else
        m_storesOverLoadsWhereNoWayLed.append(m_arena, store);
}

void Procedure::stopAtStoresThatWayLeadsToAfterAll()
{
    for (const Operation* store : m_storesOverLoadsWhereNoWayLed) {
        if (wayLeadsFromEntrypointInto(*this, store->owner())) {
            fail(FailureKind::MalformedProcedure, store);
            return;
        }
    }
}

void Procedure::separateShiftsOfSums()
{
    for (auto [sum, own] : m_ownShiftsOfSums.span()) {
        Operation* shared = sum->m_children[1];
        ASSERT(shared->opcode() == Shl && own->opcode() == Shl);
        if (!sum->useCount() || shared->useCount() < 2 || shared->useCount() == std::numeric_limits<uint16_t>::max())
            continue;
        sum->m_children[1] = own;
        shared->loseUse();
        if (own->gainUseLeavingChildrenUncounted(shared->m_flags))
            own->countChildrenAtFirstUse();
    }
}

void Procedure::settleUpperHalvesOfPhis()
{
    for (bool someChanged = !m_upsilonsOfWordsBetweenPhis.isEmpty(); someChanged;) {
        someChanged = false;
        for (const Operation* upsilon : m_upsilonsOfWordsBetweenPhis) {
            Operation* phi = upsilon->phi();
            if (!upsilon->child(0)->has(OperationFlag::UpperHalfMayBeSet) || phi->has(OperationFlag::UpperHalfMayBeSet))
                continue;
            phi->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfMayBeSet);
            someChanged = true;
        }
    }
}

Block* Procedure::addEntrypoint(uint16_t id)
{
    ASSERT(std::ranges::none_of(m_entrypoints.span(), [&](const Entrypoint& entrypoint) { return entrypoint.id == id; }));
    Block* block = addBlock();
    block->m_isEntry = true;
    m_entrypoints.append(m_arena, Entrypoint { id, block, { } });
    return block;
}

Type Procedure::addTuple(std::span<const Type> types)
{
    ASSERT(std::ranges::all_of(types, [](Type type) { return type.isNumeric(); }));
    for (uint32_t index = 0; index < m_tuples.size(); ++index) {
        if (std::ranges::equal(m_tuples[index], types))
            return Type::tuple(static_cast<uint16_t>(index));
    }

    RELEASE_ASSERT(m_tuples.size() <= std::numeric_limits<uint16_t>::max());
    Type result = Type::tuple(static_cast<uint16_t>(m_tuples.size()));
    m_tuples.append(m_arena, m_arena.copySpan<Type>(types));
    return result;
}

StackSlot* Procedure::addStackSlot(unsigned byteSize)
{
    ASSERT(byteSize);
    StackSlot* slot = m_arena.create<StackSlot>(m_stackSlots.size(), byteSize);
    m_stackSlots.append(m_arena, slot);
    return slot;
}

static uint32_t machineCoreHashOfConstantData(std::span<const uint8_t> bytes, uint32_t alignment)
{
    uint64_t hash = 2166136261u ^ alignment;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const uint8_t* cursor = bytes.data();
    size_t bytesLeft = bytes.size();
    for (; bytesLeft >= sizeof(uint64_t); bytesLeft -= sizeof(uint64_t), cursor += sizeof(uint64_t)) {
        uint64_t word;
        std::memcpy(&word, cursor, sizeof(uint64_t));
        hash = (std::rotr(hash, 32) ^ word) * 0x9E3779B97F4A7C15ull;
    }
    for (; bytesLeft; --bytesLeft, ++cursor)
        hash = (hash ^ *cursor) * 16777619u;
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    hash = (hash ^ hash >> 32) * 0x9E3779B97F4A7C15ull;
    return static_cast<uint32_t>(hash >> 32);
}

// The encoder calls this wherever it materializes a double, so the cost of a call must not grow with the number of constants.
uint32_t Procedure::addConstantData(std::span<const uint8_t> bytes, uint32_t alignment, ConstantKind kind)
{
    ASSERT(!bytes.empty());
    ASSERT(hasOneBitSet(alignment));

    if (m_constantData.size() * 2 >= m_constantDataBuckets.size()) {
        m_constantDataBuckets.fill(m_arena, std::max<uint32_t>(16, m_constantDataBuckets.size() * 2), 0);
        uint32_t mask = m_constantDataBuckets.size() - 1;
        for (uint32_t index = 0; index < m_constantData.size(); ++index) {
            const ConstantDataRecord& record = m_constantData[index];
            uint32_t bucket = machineCoreHashOfConstantData(record.bytes, record.alignment) & mask;
            while (m_constantDataBuckets[bucket])
                bucket = (bucket + 1) & mask;
            m_constantDataBuckets[bucket] = index + 1;
        }
    }

    uint32_t mask = m_constantDataBuckets.size() - 1;
    uint32_t bucket = machineCoreHashOfConstantData(bytes, alignment) & mask;
    for (; m_constantDataBuckets[bucket]; bucket = (bucket + 1) & mask) {
        uint32_t index = m_constantDataBuckets[bucket] - 1;
        const ConstantDataRecord& record = m_constantData[index];
        if (record.alignment == alignment && std::ranges::equal(record.bytes, bytes))
            return index;
    }

    uint32_t index = m_constantData.size();
    m_constantData.append(m_arena, ConstantDataRecord { m_arena.copySpan<uint8_t>(bytes), alignment, kind });
    m_constantDataBuckets[bucket] = index + 1;
    return index;
}

static bool machineCoreLiesBelowListedHeap(const HeapTree& tree, const BitSpan& listed, HeapIndex heap)
{
    while (heap != rootHeapIndex) {
        heap = tree.parentOf(heap);
        if (listed.get(heap))
            return true;
    }
    return false;
}

static std::span<const HeapIndex> machineCoreKeepHeapsBelowNoOther(Arena& arena, const HeapTree& tree, std::span<const AbstractHeap* const> heaps)
{
    std::span<HeapIndex> kept = arena.createSpan<HeapIndex>(heaps.size());
    BitSpan listed = BitSpan::create(arena, tree.size());
    size_t numberOfDistinctHeaps = 0;
    for (const AbstractHeap* heap : heaps) {
        if (listed.get(heap->index()))
            continue;
        listed.set(heap->index());
        kept[numberOfDistinctHeaps++] = heap->index();
    }

    size_t numberKept = 0;
    for (HeapIndex heap : kept.first(numberOfDistinctHeaps)) {
        if (!machineCoreLiesBelowListedHeap(tree, listed, heap))
            kept[numberKept++] = heap;
    }
    return kept.first(numberKept);
}

static std::atomic<uint64_t> machineCoreNumberOfHeapTreesMade { 0 };

struct MachineCoreListsOfHeapsKept {
    static constexpr size_t numberOfSlots = 64;
    static constexpr size_t longestList = 64;

    struct Slot {
        const HeapTree* tree;
        uint64_t numberOfHeapTreesMade;
        uint32_t sizeOfList;
        uint32_t numberKept;
        std::array<HeapIndex, longestList> list;
        std::array<HeapIndex, longestList> kept;
    };

    std::array<Slot, numberOfSlots> slots;
};

std::span<const HeapIndex> Procedure::heapsKeptOf(std::span<const AbstractHeap* const> heaps)
{
    if (heaps.size() > MachineCoreListsOfHeapsKept::longestList)
        return machineCoreKeepHeapsBelowNoOther(m_arena, m_heapTree, heaps);

    std::array<HeapIndex, MachineCoreListsOfHeapsKept::longestList> indexes;
    uint32_t hash = 0;
    size_t size = 0;
    for (const AbstractHeap* heap : heaps) {
        indexes[size++] = heap->index();
        hash = hash * 31 + heap->index();
    }
    std::span<const HeapIndex> list = std::span<const HeapIndex>(indexes).first(size);

    static_assert(MachineCoreListsOfHeapsKept::numberOfSlots == 64);
    if (!m_listsOfHeapsKept)
        m_listsOfHeapsKept = &m_arena.objectKeptAcrossResets<MachineCoreListsOfHeapsKept>();
    MachineCoreListsOfHeapsKept::Slot& slot = m_listsOfHeapsKept->slots[(hash * 0x9E3779B1u) >> 26];
    uint64_t numberOfHeapTreesMade = machineCoreNumberOfHeapTreesMade.load(std::memory_order_relaxed);
    if (slot.tree == &m_heapTree && slot.numberOfHeapTreesMade == numberOfHeapTreesMade && equalSpans(std::span<const HeapIndex>(slot.list).first(slot.sizeOfList), list))
        return m_arena.copySpan<HeapIndex>(std::span<const HeapIndex>(slot.kept).first(slot.numberKept));

    std::span<const HeapIndex> kept = machineCoreKeepHeapsBelowNoOther(m_arena, m_heapTree, heaps);
    slot.tree = &m_heapTree;
    slot.numberOfHeapTreesMade = numberOfHeapTreesMade;
    slot.sizeOfList = static_cast<uint32_t>(size);
    slot.numberKept = static_cast<uint32_t>(kept.size());
    std::ranges::copy(list, slot.list.begin());
    std::ranges::copy(kept, slot.kept.begin());
    return kept;
}

HeapSet Procedure::heapSet(std::span<const AbstractHeap* const> heaps)
{
    if (heaps.empty())
        return { };

#if ASSERT_ENABLED
    for (const AbstractHeap* heap : heaps)
        ASSERT(heap->isInitialized() && heap->index() < m_heapTree.size());
#endif

    if (heaps.size() == 1) {
        std::span<HeapIndex> kept = m_arena.createSpan<HeapIndex>(1);
        kept[0] = heaps[0]->index();
        return HeapSet::of(kept);
    }

    for (const ListOfHeapsWithItsSet& known : m_lastListsOfHeaps) {
        if (equalSpans(known.list, heaps))
            return known.set;
    }

    ListOfHeapsWithItsSet& replaced = m_lastListsOfHeaps[m_numberOfListsOfHeapsKept++ % m_lastListsOfHeaps.size()];
    replaced = { m_arena.copySpan<const AbstractHeap*>(heaps), HeapSet::of(heapsKeptOf(heaps)) };
    return replaced.set;
}

HeapTree::HeapTree()
{
    machineCoreNumberOfHeapTreesMade.fetch_add(1, std::memory_order_relaxed);
    m_parents.append(rootHeapIndex);
    m_names.append("root"_s);
}

HeapIndex HeapTree::add(HeapIndex parent, ASCIILiteral name)
{
    size_t index = m_parents.size();
    RELEASE_ASSERT(parent < index && index <= std::numeric_limits<HeapIndex>::max());
    m_parents.append(parent);
    m_names.append(name);
    return static_cast<HeapIndex>(index);
}

IndexedAbstractHeap::IndexedAbstractHeap(HeapTree& tree, HeapIndex parent, ASCIILiteral name, ptrdiff_t offset, size_t elementSize, OptionSet<HeapTrait> traits)
    : m_anyIndex(tree.add(parent, name), 0, traits)
    , m_offset(offset)
    , m_elementSize(elementSize)
{
    for (unsigned child = 0; child <= numberOfIndexesWithOwnHeap; ++child) {
        HeapIndex heap = tree.add(m_anyIndex.index(), name);
        if (!child)
            m_firstChild = heap;
        ASSERT(heap == m_firstChild + child);
    }
}

Opcode invertedComparison(Opcode opcode)
{
    switch (opcode) {
    case Equal:
        return NotEqual;
    case NotEqual:
        return Equal;
    case LessThan:
        return GreaterEqual;
    case GreaterThan:
        return LessEqual;
    case LessEqual:
        return GreaterThan;
    case GreaterEqual:
        return LessThan;
    case Above:
        return BelowEqual;
    case Below:
        return AboveEqual;
    case AboveEqual:
        return Below;
    case BelowEqual:
        return Above;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Opcode flippedComparison(Opcode opcode)
{
    switch (opcode) {
    case Equal:
    case NotEqual:
    case EqualOrUnordered:
        return opcode;
    case LessThan:
        return GreaterThan;
    case GreaterThan:
        return LessThan;
    case LessEqual:
        return GreaterEqual;
    case GreaterEqual:
        return LessEqual;
    case Above:
        return Below;
    case Below:
        return Above;
    case AboveEqual:
        return BelowEqual;
    case BelowEqual:
        return AboveEqual;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
