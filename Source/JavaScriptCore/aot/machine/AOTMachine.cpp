/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachine.h"

#if ENABLE(AOT)

#include <algorithm>
#include <atomic>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Machine {

// No value is wider than 64 bits, so a register whose upper half alone is in the set is not in the mask.
RegisterMask maskOf(const RegisterSet& set)
{
    static_assert(sizeof(RegisterBitSet) == sizeof(RegisterBitSet::WordType));
    return set.toScalarRegisterSet().bitsForDebugging();
}

RegisterSet registerSetOf(RegisterMask mask)
{
    [[maybe_unused]] constexpr RegisterMask everyRegister = ~RegisterMask { 0 } >> (63 - Reg::maxIndex());
    ASSERT(!(mask & ~everyRegister));

    RegisterSet result;
    forEachRegister(mask, [&](Reg reg) {
        result.add(reg, IgnoreVectors);
    });
    return result;
}

// An Add without position counted its children when it was made.
static bool machineCoreCountsChildrenAtThisRead(const Operation* operation)
{
    return !operation->useCount() && operation->owner() && countsChildrenAtFirstUse(operation->opcode());
}

// A chain of operations that nothing has read yet can be long, so this does not recurse.
void Operation::countChildrenAtFirstUse()
{
    Operation* operation = this;
    do {
        ASSERT(operation->m_owner && countsChildrenAtFirstUse(operation->m_opcode));
        uint16_t flagsOfChildren = operation->m_opcode == Trunc ? 0 : static_cast<uint16_t>(OperationFlag::UpperHalfIsRead);
        Operation* childReadFirst = nullptr;
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
            if (childReadFirst) [[unlikely]]
                childReadFirst->countChildrenAtFirstUseWithWorklist();
            childReadFirst = child;
        }
        operation = childReadFirst;
    } while (operation);
}

NEVER_INLINE void Operation::countChildrenAtFirstUseWithWorklist()
{
    Vector<Operation*, 16> operationsReadFirst;
    Operation* operation = this;
    for (;;) {
        ASSERT(operation->m_owner && countsChildrenAtFirstUse(operation->m_opcode));
        uint16_t flagsOfChildren = operation->m_opcode == Trunc ? 0 : static_cast<uint16_t>(OperationFlag::UpperHalfIsRead);
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

// A count that has reached its limit says nothing, and stays.
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

void ResultConstraints::assign(std::span<const ValueRep> reps)
{
    m_reps.clear();
    if (reps.size() == 1) [[likely]] {
        m_reps.appendWithinCapacity(reps.front());
        return;
    }
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

void PatchOperation::append(Operation* value, const ValueRep& rep)
{
    ASSERT(value->type().isNumeric());
    ASSERT(rep.isAny() || rep.kind() == ValueRep::SomeRegister || rep.kind() == ValueRep::SomeLateRegister || rep.kind() == ValueRep::Register || rep.kind() == ValueRep::StackArgument);
    // Operand 0 of a Check is its predicate.
    ASSERT(m_opcode != Check || !m_operands.isEmpty() || rep.kind() == ValueRep::ColdAny);
    // Says nothing about a value in another block: validate() checks dominance.
    RELEASE_ASSERT(value->hasNoPosition() || value->owner() != m_owner || value->opcode() == Phi || value->indexInBlock() < m_indexInBlock);

    if (m_operands.size() == m_operands.capacity()) [[unlikely]]
        growStorageOfOperands();
    m_operands.appendWithinCapacity(value);
    m_constraints.appendWithinCapacity(rep);

    // A Check that was folded away stands in no block, and nothing reads its operands.
    if (m_owner)
        value->gainUse();
}

void PatchOperation::markChildAsLetGoByCallee(unsigned child)
{
    RELEASE_ASSERT(m_opcode == Patch && child < m_operands.size());
    m_childrenLetGoByCallee.append(m_procedure.arena(), child);
}

void PatchOperation::leaveOperandsWhereTheyLie()
{
    RELEASE_ASSERT(m_opcode == Patch && bringsOperandsToTheirRegisters() && m_registersToBringOperandsTo.isEmpty());
    // The generator writes the registers before anything is read that the allocator has placed. So nothing may be placed in one of them by the allocator's choice.
    if (numGPScratchRegisters)
        return;
    for (const ValueRep& constraint : m_constraints) {
        if (constraint.kind() == ValueRep::SomeRegister || constraint.kind() == ValueRep::SomeLateRegister || constraint.kind() == ValueRep::WarmAny)
            return;
    }
    for (unsigned child = 0; child < m_operands.size(); ++child) {
        ValueRep& constraint = m_constraints[child];
        // What else has no position lies nowhere: it is made in the register. The generator is told a constant.
        if (!constraint.isGPR() || (m_operands[child]->hasNoPosition() && !m_operands[child]->isConstant()))
            continue;
        if (m_registersToBringOperandsTo.isEmpty())
            m_registersToBringOperandsTo.fill(m_procedure.arena(), m_operands.size(), Reg());
        m_registersToBringOperandsTo[child] = constraint.reg();
        constraint = ValueRep::ColdAny;
    }
}

void PatchOperation::setReads(std::span<const AbstractHeap* const> heaps)
{
    effects.reads = m_procedure.heapSet(heaps);
}

void PatchOperation::setWrites(std::span<const AbstractHeap* const> heaps)
{
    effects.writes = m_procedure.heapSet(heaps);
}

// The emitters set effects.terminal only after this call, so it is not asserted here.
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

void Block::appendSuccessor(Edge edge)
{
    appendSuccessorInline(edge);
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
    for (const Edge& edge : m_successors) {
        ASSERT(edge.block()->m_numberOfIncomingEdges);
        --edge.block()->m_numberOfIncomingEdges;
    }
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

void Procedure::fail(FailureKind kind, const Operation* operation)
{
    if (m_failure)
        return;
    if (operation)
        m_failure = Failure { kind, operation->origin(), operation->index() };
    else
        m_failure = Failure { kind, Origin(), UINT32_MAX };
}

Block* Procedure::addBlock()
{
    static_assert(std::is_trivially_destructible_v<Block>);
    static_assert(alignof(Block) >= alignof(Edge) && alignof(Edge) >= alignof(Operation*) && !(sizeof(Block) % alignof(Edge)));
    constexpr size_t bytesOfSuccessors = Block::firstCapacityOfSuccessors * sizeof(Edge);
    constexpr size_t bytesOfOperations = Block::firstCapacityOfOperations * sizeof(Operation*);
    constexpr size_t bytesOfUpsilons = Block::firstCapacityOfUpsilons * sizeof(Operation*);
    uintptr_t memory = reinterpret_cast<uintptr_t>(m_arena.allocate(sizeof(Block) + bytesOfSuccessors + bytesOfOperations + bytesOfUpsilons, alignof(Block)));
    Block* block = new (NotNull, reinterpret_cast<void*>(memory)) Block(*this, m_blocks.size());
    block->m_successors.adoptStorage(reinterpret_cast<Edge*>(memory + sizeof(Block)), Block::firstCapacityOfSuccessors);
    block->m_operations.adoptStorage(reinterpret_cast<Operation**>(memory + sizeof(Block) + bytesOfSuccessors), Block::firstCapacityOfOperations);
    block->m_upsilons.adoptStorage(reinterpret_cast<Operation**>(memory + sizeof(Block) + bytesOfSuccessors + bytesOfOperations), Block::firstCapacityOfUpsilons);
    m_blocks.append(m_arena, block);
    return block;
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

// FNV-1a.
static uint32_t machineCoreHashOfConstantData(std::span<const uint8_t> bytes, uint32_t alignment)
{
    uint32_t hash = 2166136261u ^ alignment;
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * 16777619u;
    return hash;
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

// A call lists dozens of heaps, so no heap is compared with every other: each walks up the tree once.
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

// Whoever reads at any index has already added the offset of the elements to the address, so that heap has no offset of its own.
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
