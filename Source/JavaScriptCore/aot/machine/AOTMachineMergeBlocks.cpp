/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineOrder.h"
#include "AOTInliningAcrossFiles.h"

#if ENABLE(AOT)

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

namespace {

class MachineBlockMerger {
    WTF_MAKE_NONCOPYABLE(MachineBlockMerger);
public:
    explicit MachineBlockMerger(Procedure& procedure)
        : m_proc(procedure)
        , m_arena(procedure.arena())
    {
    }

    void run();
    void letGoOfLists();

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr unsigned saturatedUseCount = std::numeric_limits<uint16_t>::max();
    static constexpr uint32_t placesOfSuccessor = 4;

    struct BlocksByPlace {
        uint64_t* storage;
        const uint32_t* previousInPlace;
        std::span<const uint32_t> lastInPlaces;
    };

    static ALWAYS_INLINE uint64_t mix(uint64_t hash, uint64_t word);
    static ALWAYS_INLINE uint64_t signatureOf(const Operation*);
    static ALWAYS_INLINE uint64_t payloadOf(const Operation*);
    static ALWAYS_INLINE uint64_t identityOfChild(const Operation* user, const Operation* child);
    static ALWAYS_INLINE bool originIsObservable(const Operation*);

    BlocksByPlace listBlocksByPlace();
    ALWAYS_INLINE std::optional<uint32_t> hashOfContentIfMergeable(const Block*);
    NEVER_INLINE bool isMergeable(const Block*);

    static bool areEqual(const HeapSet&, const HeapSet&);
    static bool areEqual(const Effects&, const Effects&);
    static bool areEqual(const PatchOperation*, const PatchOperation*);
    static bool areEqualButForOrigin(const Operation*, const Operation*);
    static ALWAYS_INLINE bool mayBeIdentical(const Block*, const Block*);
    static bool areIdentical(const Block*, const Block*);
    NEVER_INLINE void moveEdgesToBlocksThatStay(Block* from, const uint32_t* replacements);
    NEVER_INLINE void findBlocksEnteredFromRareBlocks();
    bool isEnteredFromRareBlock(uint32_t block)
    {
        if (!m_isEnteredFromRareBlock)
            findBlocksEnteredFromRareBlocks();
        return m_isEnteredFromRareBlock[block];
    }

    Procedure& m_proc;
    Arena& m_arena;
    const bool* m_isSuccessorOfPatch { nullptr };
    const bool* m_isEnteredFromRareBlock { nullptr };
    ArenaVector<uint32_t> m_usesWithinBlock;
    std::span<uint8_t> m_memoryOfPlaces;
};

ALWAYS_INLINE uint64_t MachineBlockMerger::mix(uint64_t hash, uint64_t word)
{
    hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
    return hash ^ hash >> 32;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint64_t MachineBlockMerger::signatureOf(const Operation* operation)
{
    uint64_t flags = 0;
    for (OperationFlag flag : { OperationFlag::Traps, OperationFlag::IsNarrowReference, OperationFlag::IsInnerPointer, OperationFlag::IsChill, OperationFlag::ReadsImmutableHeap, OperationFlag::NamesPermanentCell, OperationFlag::IsIntegerAddedTo, OperationFlag::ExtendsWordThatIsNotNegative }) {
        if (operation->has(flag))
            flags |= static_cast<uint16_t>(flag);
    }
    Type type = operation->type();
    return static_cast<uint64_t>(operation->opcode()) | flags << 8 | static_cast<uint64_t>(type.kind()) << 24 | static_cast<uint64_t>(type.tupleIndex()) << 32;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint64_t MachineBlockMerger::payloadOf(const Operation* operation)
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
    case RotR:
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

ALWAYS_INLINE uint64_t MachineBlockMerger::identityOfChild(const Operation* user, const Operation* child)
{
    const Block* block = user->owner();
    if (child->owner() != block)
        return static_cast<uint64_t>(child->index()) << 1;
    uint32_t placeOfUser = user->opcode() == Upsilon ? block->size() : user->indexInBlock();
    return static_cast<uint64_t>(placeOfUser - child->indexInBlock()) << 1 | 1;
}

ALWAYS_INLINE bool MachineBlockMerger::originIsObservable(const Operation* operation)
{
    return operation->traps() || PatchOperation::accepts(operation->opcode());
}

MachineBlockMerger::BlocksByPlace MachineBlockMerger::listBlocksByPlace()
{
    const uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    Block* const* blocks = m_proc.blocks().data();

    static_assert(placesOfSuccessor * sizeof(uint32_t) == 2 * sizeof(uint64_t));
    RELEASE_ASSERT(numberOfBlocks <= UINT32_MAX / placesOfSuccessor);
    const uint32_t numberOfPlaces = numberOfBlocks * placesOfSuccessor;
    const size_t bytesOfPlaces = static_cast<size_t>(numberOfPlaces) * sizeof(uint32_t);
    const size_t offsetOfPreviousInPlace = (bytesOfPlaces + numberOfBlocks + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
    const size_t offsetOfPlacesOfSeveral = offsetOfPreviousInPlace + static_cast<size_t>(numberOfBlocks) * sizeof(uint32_t);
    const size_t bytesOfMemory = offsetOfPlacesOfSeveral + static_cast<size_t>(numberOfBlocks / 2) * sizeof(uint32_t);
    uint8_t* memory = static_cast<uint8_t*>(m_arena.allocate(bytesOfMemory, alignof(uint64_t)));
    m_memoryOfPlaces = std::span<uint8_t>(memory, bytesOfMemory);
    std::memset(memory, 0, bytesOfPlaces + numberOfBlocks);
    uint32_t* places = reinterpret_cast<uint32_t*>(memory);
    bool* isSuccessorOfPatch = reinterpret_cast<bool*>(memory + bytesOfPlaces);
    m_isSuccessorOfPatch = isSuccessorOfPatch;
    uint32_t* previousInPlace = reinterpret_cast<uint32_t*>(memory + offsetOfPreviousInPlace);
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

        const Edge* edge = successors;
        for (; edge != end; ++edge) {
            const Block* successor = edge->block();
            if (successor->numberOfIncomingEdges() < 2)
                break;
            if (successor == block)
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
        if (previous && !previousInPlace[previous - 1])
            placesOfSeveral[numberOfPlacesOfSeveral++] = place;
    }

    for (uint32_t number = 0; number < numberOfPlacesOfSeveral; ++number)
        placesOfSeveral[number] = places[placesOfSeveral[number]];
    return { reinterpret_cast<uint64_t*>(memory), previousInPlace, { placesOfSeveral, numberOfPlacesOfSeveral } };
}

ALWAYS_INLINE std::optional<uint32_t> MachineBlockMerger::hashOfContentIfMergeable(const Block* block)
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

    auto add = [&](const Operation* operation, uint32_t place) ALWAYS_INLINE_LAMBDA {
        Opcode opcode = operation->opcode();
        if (opcode != Jump) {
            hash = mix(hash, payloadOf(operation) << 8 | static_cast<uint64_t>(opcode));
            if (const PatchOperation* patch = operation->as<PatchOperation>())
                generators = mix(generators, reinterpret_cast<uintptr_t>(patch->generator()));
            bool hasCountedChildren = operation->useCount() || !countsChildrenAtFirstUse(opcode);
            for (const Operation* child : operation->children()) {
                if (child->owner() != block) {
                    hash = mix(hash, static_cast<uint64_t>(child->index()) << 1);
                    continue;
                }
                uint32_t placeOfChild = child->indexInBlock();
                hash = mix(hash, static_cast<uint64_t>(place - placeOfChild) << 1 | 1);
                if (hasCountedChildren && placeOfChild < place)
                    ++usesWithinBlock[placeOfChild];
            }
        }
        hashOfOrigins = hashOfOrigins * 31 + operation->origin().bits();
        originsMatter |= originIsObservable(operation);
    };

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
    return static_cast<uint32_t>(mix(hash, generators));
}

bool MachineBlockMerger::isMergeable(const Block* block)
{
    return hashOfContentIfMergeable(block).has_value();
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
        || a->showsRegistersToCollector() != b->showsRegistersToCollector()
        || a->followsFrameSetup() != b->followsFrameSetup()
        || a->testsStackAheadOfFramedPart() != b->testsStackAheadOfFramedPart()
        || a->leavesByReturning() != b->leavesByReturning()
        || a->needsFrameRecord() != b->needsFrameRecord()
        || a->findsOperandsInFrameWithoutRecord() != b->findsOperandsInFrameWithoutRecord()
        || a->isSuspension() != b->isSuspension()
        || a->sharesLatePath() != b->sharesLatePath()
        || a->failsWhenPredicateIsZero() != b->failsWhenPredicateIsZero()
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

void MachineBlockMerger::moveEdgesToBlocksThatStay(Block* from, const uint32_t* replacements)
{
    if (replacements[from->index()] != none) {
        Order::emptyBlock(from);
        return;
    }
    for (unsigned successorIndex = 0; successorIndex < from->numSuccessors(); ++successorIndex) {
        uint32_t replacement = replacements[from->successor(successorIndex).block()->index()];
        if (replacement != none)
            Order::retargetToBlockThatStandsForTarget(from, successorIndex, m_proc.blockAt(replacement));
    }
}

void MachineBlockMerger::findBlocksEnteredFromRareBlocks()
{
    const uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    Block* const* blocks = m_proc.blocks().data();
    bool* isReached = m_arena.allocateArray<bool>(numberOfBlocks);
    std::fill_n(isReached, numberOfBlocks, false);
    uint32_t* reached = m_arena.allocateArray<uint32_t>(numberOfBlocks);
    uint32_t numberReached = 0;
    for (const Entrypoint& entrypoint : m_proc.entrypoints()) {
        uint32_t index = entrypoint.block->index();
        if (isReached[index])
            continue;
        isReached[index] = true;
        reached[numberReached++] = index;
    }
    for (uint32_t next = 0; next < numberReached; ++next) {
        for (const Edge& edge : blocks[reached[next]]->successors()) {
            uint32_t target = edge.block()->index();
            if (edge.isRare() || isReached[target])
                continue;
            isReached[target] = true;
            reached[numberReached++] = target;
        }
    }
    bool* isEnteredFromRareBlock = m_arena.allocateArray<bool>(numberOfBlocks);
    std::fill_n(isEnteredFromRareBlock, numberOfBlocks, false);
    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        if (isReached[index])
            continue;
        for (const Edge& edge : blocks[index]->successors())
            isEnteredFromRareBlock[edge.block()->index()] = true;
    }
    m_isEnteredFromRareBlock = isEnteredFromRareBlock;
    m_arena.letGoOfArray(reached, numberOfBlocks);
    m_arena.letGoOfArray(isReached, numberOfBlocks);
}

void MachineBlockMerger::letGoOfLists()
{
    if (m_memoryOfPlaces.size() < Arena::smallestRoomLetGo)
        return;
    m_arena.letGoOfArray(m_isEnteredFromRareBlock, m_isEnteredFromRareBlock ? m_proc.numberOfBlocks() : 0);
    m_usesWithinBlock.letGo(m_arena);
    m_arena.letGoOfArray(m_memoryOfPlaces.data(), m_memoryOfPlaces.size());
}

void MachineBlockMerger::run()
{
    const uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    if (numberOfBlocks < 3)
        return;
    Block* const* blocks = m_proc.blocks().data();

    BlocksByPlace byPlace = listBlocksByPlace();
    if (byPlace.lastInPlaces.empty())
        return;
    const uint32_t* previousInPlace = byPlace.previousInPlace;
    const bool* isSuccessorOfPatch = m_isSuccessorOfPatch;
    auto mayMergeUnlessRareBlockEnters = [&](uint32_t index) ALWAYS_INLINE_LAMBDA {
        return blocks[index]->isLandingPad() || !isSuccessorOfPatch[index];
    };
    auto staysApartForRareBlockThatEnters = [&](uint32_t index) ALWAYS_INLINE_LAMBDA {
        return !blocks[index]->isLandingPad() && !blocks[index]->numSuccessors() && isEnteredFromRareBlock(index);
    };

    uint64_t* candidates = byPlace.storage;
    uint32_t numberOfCandidates = 0;
    uint32_t* replacements = reinterpret_cast<uint32_t*>(byPlace.storage + numberOfBlocks);
    bool hasReplacements = false;
    auto replace = [&](uint32_t other, uint32_t kept) {
        if (!hasReplacements) {
            std::fill_n(replacements, numberOfBlocks, none);
            hasReplacements = true;
        }
        replacements[other] = kept;
    };

    for (uint32_t last : byPlace.lastInPlaces) {
        uint32_t firstTwo[2];
        uint32_t numberThatMayMerge = 0;
        for (uint32_t member = last; member; member = previousInPlace[member - 1]) {
            if (!mayMergeUnlessRareBlockEnters(member - 1))
                continue;
            if (numberThatMayMerge < 2)
                firstTwo[numberThatMayMerge] = member - 1;
            ++numberThatMayMerge;
        }
        if (numberThatMayMerge < 2)
            continue;

        if (numberThatMayMerge == 2) {
            const Block* kept = blocks[firstTwo[1]];
            const Block* other = blocks[firstTwo[0]];
            if (mayBeIdentical(kept, other) && areIdentical(kept, other) && !staysApartForRareBlockThatEnters(firstTwo[1]) && !staysApartForRareBlockThatEnters(firstTwo[0]) && isMergeable(kept) && isMergeable(other))
                replace(firstTwo[0], firstTwo[1]);
            continue;
        }

        for (uint32_t member = last; member; member = previousInPlace[member - 1]) {
            uint32_t index = member - 1;
            if (!mayMergeUnlessRareBlockEnters(index))
                continue;
            if (std::optional<uint32_t> hash = hashOfContentIfMergeable(blocks[index]))
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
            if (staysApartForRareBlockThatEnters(kept))
                continue;
            for (uint32_t next = first + 1; next < numberOfCandidates && candidates[next] >> 32 == hash; ++next) {
                uint32_t other = static_cast<uint32_t>(candidates[next]);
                if (hasReplacements && replacements[other] != none)
                    continue;
                if (!areIdentical(blocks[kept], blocks[other]))
                    continue;
                if (staysApartForRareBlockThatEnters(other))
                    continue;
                replace(other, kept);
            }
        }
    }
    if (!hasReplacements)
        return;

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
    MachineBlockMerger merger(procedure);
    merger.run();
    merger.letGoOfLists();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
