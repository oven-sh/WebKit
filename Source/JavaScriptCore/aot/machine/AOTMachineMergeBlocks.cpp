/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineOrder.h"

#if ENABLE(AOT)

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

namespace {

// A block takes part if it is entered through edges alone, has no Phi, and nothing outside it reads what it computes.
// Two such blocks with equal content do the same with the same values and lead to the same blocks, so one stands for both.
// What both read from outside is computed on every way to either. So a block that went on in the scope of its one predecessor takes part too: numbering is over, and Order::retarget() takes the mark from the one that stays.
// The places and the hashes only bring blocks together that may be equal: areIdentical() decides, and of equal blocks the one with the lowest index stays. So they may be made of whatever equal blocks agree in.
// Whatever blocks are placed or ordered by is made of nothing that differs from run to run, no address: the work done here is the same in every run.
class MachineBlockMerger {
    WTF_MAKE_NONCOPYABLE(MachineBlockMerger);
public:
    explicit MachineBlockMerger(Procedure& procedure)
        : m_proc(procedure)
        , m_arena(procedure.arena())
    {
    }

    void run();

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr unsigned saturatedUseCount = std::numeric_limits<uint16_t>::max();
    // listBlocksByPlace() has so many places for the blocks that lead to one block first.
    static constexpr uint32_t placesOfSuccessor = 4;

    struct BlocksByPlace {
        // What held the places, 16 bytes for each block, which nobody needs any more.
        uint64_t* storage;
        // By Block::index(), of a block that has come to a place: the block that came there just before it, its index and one more. 0: none.
        const uint32_t* previousInPlace;
        // Of each place that several blocks have come to: the last of them, its index and one more.
        std::span<const uint32_t> lastInPlaces;
    };

    static ALWAYS_INLINE uint64_t mix(uint64_t hash, uint64_t word);
    static ALWAYS_INLINE uint64_t signatureOf(const Operation*);
    static ALWAYS_INLINE uint64_t payloadOf(const Operation*);
    static ALWAYS_INLINE uint64_t identityOfChild(const Operation* user, const Operation* child);
    static ALWAYS_INLINE bool originIsObservable(const Operation*);

    BlocksByPlace listBlocksByPlace();
    ALWAYS_INLINE std::optional<uint32_t> hashOfContentIfMergeable(const Block*, uint32_t& hashOfGenerators);
    NEVER_INLINE bool isMergeable(const Block*);

    static bool areEqual(const HeapSet&, const HeapSet&);
    static bool areEqual(const Effects&, const Effects&);
    static bool areEqual(const PatchOperation*, const PatchOperation*);
    static bool areEqualButForOrigin(const Operation*, const Operation*);
    static ALWAYS_INLINE bool mayBeIdentical(const Block*, const Block*);
    static bool areIdentical(const Block*, const Block*);
    static void takeBackReadsOf(const Block*);
    NEVER_INLINE void moveEdgesToBlocksThatStay(Block* from, const uint32_t* replacements);

    Procedure& m_proc;
    Arena& m_arena;
    // By Block::index(), from listBlocksByPlace() on.
    const bool* m_isSuccessorOfPatch { nullptr };
    // By Operation::indexInBlock(). Every entry is zero between two calls of hashOfContentIfMergeable().
    ArenaVector<uint32_t> m_usesWithinBlock;
};

ALWAYS_INLINE uint64_t MachineBlockMerger::mix(uint64_t hash, uint64_t word)
{
    hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
    return hash ^ hash >> 32;
}

ALWAYS_INLINE uint64_t MachineBlockMerger::signatureOf(const Operation* operation)
{
    uint64_t flags = 0;
    for (OperationFlag flag : { OperationFlag::Traps, OperationFlag::IsNarrowReference, OperationFlag::IsInnerPointer, OperationFlag::IsChill, OperationFlag::ReadsImmutableHeap, OperationFlag::NamesPermanentCell, OperationFlag::IsIntegerAddedTo, OperationFlag::ExtendsWordThatIsNotNegative, OperationFlag::ReadsZeroOffNoCell }) {
        if (operation->has(flag))
            flags |= static_cast<uint16_t>(flag);
    }
    Type type = operation->type();
    return static_cast<uint64_t>(operation->opcode()) | flags << 8 | static_cast<uint64_t>(type.kind()) << 24 | static_cast<uint64_t>(type.tupleIndex()) << 32;
}

// Leaves out the case values of a Switch and the fields of a Patch. areEqualButForOrigin() compares those.
ALWAYS_INLINE uint64_t MachineBlockMerger::payloadOf(const Operation* operation)
{
    switch (operation->opcode()) {
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store:
        return static_cast<uint32_t>(operation->offset()) | static_cast<uint64_t>(operation->heap()) << 32;
    case ArgumentReg:
        return operation->argumentReg().index();
    case Extract:
        return operation->extractedIndex();
    case Upsilon:
        return operation->phi() ? operation->phi()->index() : none;
    case Switch:
        return operation->switchData().caseValues.size();
    case Nop:
    case Opaque:
    case Add:
    case Sub:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
    case Select:
    case Fence:
    case Patch:
    case Check:
    case Jump:
    case Branch:
    case Return:
    case Oops:
        return 0;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A child in the block of its user goes by its distance from the user, any other by its index. An Upsilon reads at the end of its block.
ALWAYS_INLINE uint64_t MachineBlockMerger::identityOfChild(const Operation* user, const Operation* child)
{
    const Block* block = user->owner();
    if (child->owner() != block)
        return static_cast<uint64_t>(child->index()) << 1;
    uint32_t placeOfUser = user->opcode() == Upsilon ? block->size() : user->indexInBlock();
    return static_cast<uint64_t>(placeOfUser - child->indexInBlock()) << 1 | 1;
}

// The origin of such an operation becomes a position in a stack trace.
ALWAYS_INLINE bool MachineBlockMerger::originIsObservable(const Operation* operation)
{
    return operation->traps() || PatchOperation::accepts(operation->opcode());
}

// Gives each block a place that may merge by what is known of it without reading its operations. Blocks that merge have one place, and most blocks have a place to themselves, so their operations are never read.
// The place goes by the first successor and the size. Of a block without successors it goes by the size and by what ends the block: the origin of a Patch is observable, so blocks that merge agree in it.
// Two kinds of blocks in one place cost a look at their operations, and nothing else.
//
// The allocator cannot move a value into a register ahead of a Patch that ends a block, so Order puts a block on the edge of such a Patch to a block with several predecessors.
// That block would part a Patch with a tuple result from its Extracts, which the allocator looks for at the head of the first successor.
// So a successor of such a Patch neither gains a predecessor nor is replaced. A landing pad is exempt: it is entered with every value in its slot, and no edge to it is split.
// Which blocks are such successors is known when the last block has been looked at. One that is known in time gets no place, and run() drops the others.
MachineBlockMerger::BlocksByPlace MachineBlockMerger::listBlocksByPlace()
{
    const uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    Block* const* blocks = m_proc.blocks().data();

    // Most procedures are small, so the four lists are one piece of memory, and the two that start out as zeros are cleared in one go.
    static_assert(placesOfSuccessor * sizeof(uint32_t) == 2 * sizeof(uint64_t));
    RELEASE_ASSERT(numberOfBlocks <= UINT32_MAX / placesOfSuccessor);
    const uint32_t numberOfPlaces = numberOfBlocks * placesOfSuccessor;
    const size_t bytesOfPlaces = static_cast<size_t>(numberOfPlaces) * sizeof(uint32_t);
    const size_t offsetOfPreviousInPlace = (bytesOfPlaces + numberOfBlocks + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
    const size_t offsetOfPlacesOfSeveral = offsetOfPreviousInPlace + static_cast<size_t>(numberOfBlocks) * sizeof(uint32_t);
    uint8_t* memory = static_cast<uint8_t*>(m_arena.allocate(offsetOfPlacesOfSeveral + static_cast<size_t>(numberOfBlocks / 2) * sizeof(uint32_t), alignof(uint64_t)));
    std::memset(memory, 0, bytesOfPlaces + numberOfBlocks);
    // The block that has come to the place last, its index and one more. 0: none has.
    uint32_t* places = reinterpret_cast<uint32_t*>(memory);
    bool* isSuccessorOfPatch = reinterpret_cast<bool*>(memory + bytesOfPlaces);
    m_isSuccessorOfPatch = isSuccessorOfPatch;
    uint32_t* previousInPlace = reinterpret_cast<uint32_t*>(memory + offsetOfPreviousInPlace);
    // Two blocks at least have come to each, and no block comes to two places.
    uint32_t* placesOfSeveral = reinterpret_cast<uint32_t*>(memory + offsetOfPlacesOfSeveral);
    uint32_t numberOfPlacesOfSeveral = 0;

    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        const Block* block = blocks[index];
        ASSERT(block->index() == index);
        const Edge* successors = block->successors().data();
        const Edge* end = successors + block->numSuccessors();
        const uint32_t size = block->size();
        const Operation* last = size ? block->operations().data()[size - 1] : nullptr;
        Opcode opcode = size ? last->opcode() : Nop;
        if (opcode != Jump && opcode != Branch && opcode != Switch) {
            for (const Edge* edge = successors; edge != end; ++edge)
                isSuccessorOfPatch[edge->block()->index()] = true;
            if (!size || !last->endsBlock())
                continue;
        }
        if (!block->numberOfIncomingEdges())
            continue;

        // Most blocks lead where one edge alone leads, so that is asked ahead of all that is asked of few.
        const Edge* edge = successors;
        for (; edge != end; ++edge) {
            const Block* successor = edge->block();
            // Order::retarget() treats an edge from the old target to the new one as an edge that is being bypassed, and raises the class of the moved edge.
            // Blocks that merge have the same successors, so one leads to the other only if the other leads to itself.
            if (successor == block)
                break;
            // No other block leads where one edge alone leads, so none has the same successors.
            if (successor->numberOfIncomingEdges() < 2)
                break;
        }
        if (edge != end || block->isEntry() || !block->phis().empty())
            continue;
        if (isSuccessorOfPatch[index] && !block->isLandingPad())
            continue;

        uint32_t place;
        if (successors != end)
            place = successors->block()->index() * placesOfSuccessor + size % placesOfSuccessor;
        else {
            uint64_t sizeAndLast = static_cast<uint64_t>(size) << 8 | static_cast<uint64_t>(opcode);
            if (opcode == Patch)
                sizeAndLast ^= static_cast<uint64_t>(last->origin().bits()) << 32 ^ static_cast<uint64_t>(last->numChildren()) << 24;
            place = static_cast<uint32_t>(static_cast<uint64_t>(static_cast<uint32_t>(mix(0, sizeAndLast))) * numberOfPlaces >> 32);
        }

        uint32_t previous = places[place];
        previousInPlace[index] = previous;
        places[place] = index + 1;
        // It is the second to come.
        if (previous && !previousInPlace[previous - 1])
            placesOfSeveral[numberOfPlacesOfSeveral++] = place;
    }

    for (uint32_t number = 0; number < numberOfPlacesOfSeveral; ++number)
        placesOfSeveral[number] = places[placesOfSeveral[number]];
    return { reinterpret_cast<uint64_t*>(memory), previousInPlace, { placesOfSeveral, numberOfPlacesOfSeveral } };
}

// `hashOfGenerators` is made of addresses. It is for asking whether two blocks have the same generators, which comes out the same in every run, and for nothing else.
ALWAYS_INLINE std::optional<uint32_t> MachineBlockMerger::hashOfContentIfMergeable(const Block* block, uint32_t& hashOfGenerators)
{
    const uint32_t size = block->size();
    uint64_t hash = static_cast<uint64_t>(size) << 32 | static_cast<uint64_t>(block->upsilons().size()) << 1 | static_cast<uint64_t>(block->isLandingPad());
    for (const Edge& edge : block->successors())
        hash = mix(hash, static_cast<uint64_t>(edge.block()->index()) << 2 | static_cast<uint64_t>(edge.edgeClass()));
    uint64_t hashOfOrigins = 0;
    uint64_t generators = 0;
    bool originsMatter = false;
    if (m_usesWithinBlock.size() < size) [[unlikely]]
        m_usesWithinBlock.grow(m_arena, std::max(size, m_usesWithinBlock.size() * 2));
    uint32_t* usesWithinBlock = m_usesWithinBlock.begin();
    Operation* const* operations = block->operations().data();

    // `place`: where the operation reads, as in identityOfChild().
    auto add = [&](const Operation* operation, uint32_t place) ALWAYS_INLINE_LAMBDA {
        Opcode opcode = operation->opcode();
        // A Jump has no child, and nothing of its own but its origin.
        if (opcode != Jump) {
            hash = mix(hash, payloadOf(operation) << 8 | static_cast<uint64_t>(opcode));
            if (const PatchOperation* patch = operation->as<PatchOperation>())
                generators = mix(generators, reinterpret_cast<uintptr_t>(patch->generator()));
            // A pure operation that nothing reads has counted no use of its children.
            bool hasCountedChildren = operation->useCount() || !countsChildrenAtFirstUse(opcode);
            for (const Operation* child : operation->children()) {
                if (child->owner() != block) {
                    hash = mix(hash, static_cast<uint64_t>(child->index()) << 1);
                    continue;
                }
                uint32_t placeOfChild = child->indexInBlock();
                hash = mix(hash, static_cast<uint64_t>(place - placeOfChild) << 1 | 1);
                // The count of an operation that stands behind its reader has been looked at, and is zero again.
                if (hasCountedChildren && placeOfChild < place)
                    ++usesWithinBlock[placeOfChild];
            }
        }
        hashOfOrigins = hashOfOrigins * 31 + operation->origin().bits();
        originsMatter |= originIsObservable(operation);
    };

    // As add() would: an Upsilon has counted its one child, and is no Patch.
    for (const Operation* upsilon : block->upsilons()) {
        const Operation* phi = upsilon->phi();
        const Operation* child = upsilon->valueHandedOn();
        uint64_t identity = static_cast<uint64_t>(child->index()) << 1;
        if (child->owner() == block) {
            uint32_t placeOfChild = child->indexInBlock();
            identity = static_cast<uint64_t>(size - placeOfChild) << 1 | 1;
            if (placeOfChild < size)
                ++usesWithinBlock[placeOfChild];
        }
        hash = mix(hash, static_cast<uint64_t>(phi ? phi->index() : none) << 32 ^ identity);
        hashOfOrigins = hashOfOrigins * 31 + upsilon->origin().bits();
        originsMatter |= upsilon->traps();
    }
    // Whatever reads an operation within its block stands behind it. A use count leaves out only the reads of pure operations that nothing reads, which the scan drops, and one that has reached its limit says nothing.
    for (uint32_t index = size; index--;) {
        const Operation* operation = operations[index];
        unsigned useCount = operation->useCount();
        if (useCount == saturatedUseCount || useCount != usesWithinBlock[index]) {
            std::fill_n(usesWithinBlock, index + 1, 0);
            return std::nullopt;
        }
        usesWithinBlock[index] = 0;
        add(operation, index);
    }

    if (originsMatter)
        hash = mix(hash, hashOfOrigins);
    hashOfGenerators = static_cast<uint32_t>(generators);
    return static_cast<uint32_t>(hash);
}

// Nothing outside the block reads what it computes.
bool MachineBlockMerger::isMergeable(const Block* block)
{
    uint32_t hashOfGenerators;
    return hashOfContentIfMergeable(block, hashOfGenerators).has_value();
}

bool MachineBlockMerger::areEqual(const HeapSet& a, const HeapSet& b)
{
    return a.isTop() == b.isTop() && std::ranges::equal(a.heaps(), b.heaps());
}

bool MachineBlockMerger::areEqual(const Effects& a, const Effects& b)
{
    return a.terminal == b.terminal
        && a.exitsSideways == b.exitsSideways
        && a.controlDependent == b.controlDependent
        && a.writesLocalState == b.writesLocalState
        && a.mayCollect == b.mayCollect
        && areEqual(a.reads, b.reads)
        && areEqual(a.writes, b.writes);
}

// A generator made for one site keeps that site apart from every other.
bool MachineBlockMerger::areEqual(const PatchOperation* a, const PatchOperation* b)
{
    ASSERT(a->numberOfOperands() == b->numberOfOperands());
    ASSERT(a->owner()->numSuccessors() == b->owner()->numSuccessors());

    if (a->generator() != b->generator()
        || a->numGPScratchRegisters != b->numGPScratchRegisters
        || a->numFPScratchRegisters != b->numFPScratchRegisters
        || a->onlyKeepsChildrenAlive != b->onlyKeepsChildrenAlive
        || a->pureFunction != b->pureFunction
        || a->earlyClobberedMask() != b->earlyClobberedMask()
        || a->lateClobberedMask() != b->lateClobberedMask()
        || a->runsWithoutFrame() != b->runsWithoutFrame()
        || a->followsFrameSetup() != b->followsFrameSetup()
        || a->leavesByReturning() != b->leavesByReturning()
        || a->needsFrameRecord() != b->needsFrameRecord()
        || a->isSuspension() != b->isSuspension()
        || a->releasesDeadValues() != b->releasesDeadValues()
        || a->sharesLatePath() != b->sharesLatePath()
        || !areEqual(a->effects, b->effects)
        || !std::ranges::equal(a->resultConstraints.span(), b->resultConstraints.span()))
        return false;

    for (unsigned child = 0; child < a->numberOfOperands(); ++child) {
        if (a->constraintOf(child) != b->constraintOf(child))
            return false;
    }
    for (unsigned successorIndex = 0; successorIndex < a->owner()->numSuccessors(); ++successorIndex) {
        if (a->lateClobberedOnSuccessor(successorIndex) != b->lateClobberedOnSuccessor(successorIndex))
            return false;
    }
    return true;
}

bool MachineBlockMerger::areEqualButForOrigin(const Operation* a, const Operation* b)
{
    if (signatureOf(a) != signatureOf(b) || payloadOf(a) != payloadOf(b))
        return false;

    std::span<Operation* const> childrenOfA = a->children();
    std::span<Operation* const> childrenOfB = b->children();
    if (childrenOfA.size() != childrenOfB.size())
        return false;
    for (size_t index = 0; index < childrenOfA.size(); ++index) {
        if (identityOfChild(a, childrenOfA.data()[index]) != identityOfChild(b, childrenOfB.data()[index]))
            return false;
    }

    if (a->opcode() == Switch)
        return std::ranges::equal(a->switchData().caseValues, b->switchData().caseValues);
    if (const PatchOperation* patch = a->as<PatchOperation>())
        return areEqual(patch, b->as<PatchOperation>());
    return true;
}

// False: areIdentical() would say no. Neither block is empty. The two arms of a Branch that meet again mostly differ in one of these.
ALWAYS_INLINE bool MachineBlockMerger::mayBeIdentical(const Block* a, const Block* b)
{
    if (a->size() != b->size() || a->upsilons().size() != b->upsilons().size() || a->at(0)->opcode() != b->at(0)->opcode())
        return false;
    if (a->upsilons().empty())
        return true;
    const Operation* upsilonOfA = a->upsilons().data()[0];
    const Operation* upsilonOfB = b->upsilons().data()[0];
    return upsilonOfA->phi() == upsilonOfB->phi() && identityOfChild(upsilonOfA, upsilonOfA->valueHandedOn()) == identityOfChild(upsilonOfB, upsilonOfB->valueHandedOn());
}

bool MachineBlockMerger::areIdentical(const Block* a, const Block* b)
{
    if (a->isLandingPad() != b->isLandingPad()
        || a->size() != b->size()
        || a->upsilons().size() != b->upsilons().size()
        || !std::ranges::equal(a->successors(), b->successors()))
        return false;

    bool originsMatter = false;
    bool originsDiffer = false;
    // The lists are as long as each other.
    auto areEqualLists = [&](std::span<Operation* const> listOfA, std::span<Operation* const> listOfB) {
        for (size_t index = 0; index < listOfA.size(); ++index) {
            const Operation* operationOfA = listOfA.data()[index];
            const Operation* operationOfB = listOfB.data()[index];
            if (!areEqualButForOrigin(operationOfA, operationOfB))
                return false;
            originsMatter |= originIsObservable(operationOfA);
            originsDiffer |= operationOfA->origin() != operationOfB->origin();
        }
        return true;
    };
    if (!areEqualLists(a->operations(), b->operations()) || !areEqualLists(a->upsilons(), b->upsilons()))
        return false;
    return !originsMatter || !originsDiffer;
}

// Nothing leads to the block any more. What it and the block that stays both read has one reader fewer, and the scan fuses a value into its only reader.
void MachineBlockMerger::takeBackReadsOf(const Block* block)
{
    auto takeBack = [](const Operation* reader) {
        // Such an operation takes its reads back when it loses its last reader.
        if (countsChildrenAtFirstUse(reader->opcode()))
            return;
        for (Operation* child : reader->children())
            child->loseUse();
    };
    for (const Operation* upsilon : block->upsilons())
        takeBack(upsilon);
    for (const Operation* operation : block->operations())
        takeBack(operation);
}

// `replacements`: by Block::index().
void MachineBlockMerger::moveEdgesToBlocksThatStay(Block* from, const uint32_t* replacements)
{
    if (replacements[from->index()] != none)
        takeBackReadsOf(from);
    for (unsigned successorIndex = 0; successorIndex < from->numSuccessors(); ++successorIndex) {
        uint32_t replacement = replacements[from->successor(successorIndex).block()->index()];
        if (replacement != none)
            Order::retarget(from, successorIndex, m_proc.blockAt(replacement));
    }
}

void MachineBlockMerger::run()
{
    // Two blocks that merge, and the entrypoint, which never does.
    const uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    if (numberOfBlocks < 3)
        return;
    Block* const* blocks = m_proc.blocks().data();

    BlocksByPlace byPlace = listBlocksByPlace();
    if (byPlace.lastInPlaces.empty())
        return;
    const uint32_t* previousInPlace = byPlace.previousInPlace;
    const bool* isSuccessorOfPatch = m_isSuccessorOfPatch;
    auto mayMerge = [&](uint32_t index) ALWAYS_INLINE_LAMBDA {
        return !isSuccessorOfPatch[index] || blocks[index]->isLandingPad();
    };

    // The storage holds three lists, one behind the other.
    // The blocks of the places that more than two blocks share: the hash in the high 32 bits, Block::index() in the low 32. There are no more of them than blocks.
    uint64_t* candidates = byPlace.storage;
    uint32_t numberOfCandidates = 0;
    // By Block::index(), filled before the first is written. No edge moves until every pair has been compared, so what merges does not depend on the order in which pairs are compared.
    uint32_t* replacements = reinterpret_cast<uint32_t*>(byPlace.storage + numberOfBlocks);
    // By Block::index(), of a candidate.
    uint32_t* hashesOfGenerators = replacements + numberOfBlocks;
    bool hasReplacements = false;
    auto replace = [&](uint32_t other, uint32_t kept) {
        if (!hasReplacements) {
            std::fill_n(replacements, numberOfBlocks, none);
            hasReplacements = true;
        }
        replacements[other] = kept;
    };

    // Blocks that are equal have one place, so each place is settled by itself. The blocks of a place are gone through from the highest index down.
    for (uint32_t last : byPlace.lastInPlaces) {
        uint32_t firstTwo[2];
        uint32_t numberThatMayMerge = 0;
        for (uint32_t member = last; member; member = previousInPlace[member - 1]) {
            if (!mayMerge(member - 1))
                continue;
            if (numberThatMayMerge < 2)
                firstTwo[numberThatMayMerge] = member - 1;
            ++numberThatMayMerge;
        }
        if (numberThatMayMerge < 2)
            continue;

        // Most places that are shared are shared by two, which mostly differ: they are told apart without a hash.
        if (numberThatMayMerge == 2) {
            const Block* kept = blocks[firstTwo[1]];
            const Block* other = blocks[firstTwo[0]];
            if (mayBeIdentical(kept, other) && areIdentical(kept, other) && isMergeable(kept) && isMergeable(other))
                replace(firstTwo[0], firstTwo[1]);
            continue;
        }

        for (uint32_t member = last; member; member = previousInPlace[member - 1]) {
            uint32_t index = member - 1;
            if (!mayMerge(index))
                continue;
            if (std::optional<uint32_t> hash = hashOfContentIfMergeable(blocks[index], hashesOfGenerators[index]))
                candidates[numberOfCandidates++] = static_cast<uint64_t>(*hash) << 32 | index;
        }
    }

    if (numberOfCandidates >= 2) {
        std::ranges::sort(std::span<uint64_t>(candidates, numberOfCandidates));
        for (uint32_t first = 0; first + 1 < numberOfCandidates; ++first) {
            uint64_t hash = candidates[first] >> 32;
            if (candidates[first + 1] >> 32 != hash)
                continue;
            uint32_t kept = static_cast<uint32_t>(candidates[first]);
            if (hasReplacements && replacements[kept] != none)
                continue;
            for (uint32_t next = first + 1; next < numberOfCandidates && candidates[next] >> 32 == hash; ++next) {
                uint32_t other = static_cast<uint32_t>(candidates[next]);
                if (hasReplacements && replacements[other] != none)
                    continue;
                // A generator made for one site keeps that site apart from every other.
                if (hashesOfGenerators[kept] != hashesOfGenerators[other] || !areIdentical(blocks[kept], blocks[other]))
                    continue;
                replace(other, kept);
            }
        }
    }
    if (!hasReplacements)
        return;

    // Few blocks are replaced or lead to one that is.
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        Block* from = blocks[index];
        if (replacements[index] == none) {
            const Edge* edge = from->successors().data();
            const Edge* end = edge + from->numSuccessors();
            while (edge != end && replacements[edge->block()->index()] == none)
                ++edge;
            if (edge == end)
                continue;
        }
        moveEdgesToBlocksThatStay(from, replacements);
    }
}

} // anonymous namespace

void mergeIdenticalBlocks(Procedure& procedure)
{
    if (procedure.failure())
        return;
    MachineBlockMerger(procedure).run();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
