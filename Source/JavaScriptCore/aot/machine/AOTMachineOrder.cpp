/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineOrder.h"

#if ENABLE(AOT)

#include <algorithm>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static_assert(EdgeClass::Normal < EdgeClass::Unlikely && EdgeClass::Unlikely < EdgeClass::Rare);

// A pair for its head and a pair for its Jump.
static constexpr uint32_t positionsOfBlockSplitLate = 4;

void Order::retarget(Block* from, unsigned successorIndex, Block* newTarget)
{
    Edge& edge = from->m_successors[successorIndex];
    Block* oldTarget = edge.m_block;
    if (oldTarget == newTarget)
        return;

    // An edge that passes its old target by stands for two edges, and is as rare as the rarer of them.
    if (newTarget->m_numberOfIncomingEdges) {
        std::optional<EdgeClass> classOfEdgePassedBy;
        for (const Edge& passedBy : oldTarget->successors()) {
            if (passedBy.block() == newTarget)
                classOfEdgePassedBy = classOfEdgePassedBy ? std::min(*classOfEdgePassedBy, passedBy.edgeClass()) : passedBy.edgeClass();
        }
        if (classOfEdgePassedBy)
            edge.m_class = std::max(edge.m_class, *classOfEdgePassedBy);
    }

    ASSERT(oldTarget->m_numberOfIncomingEdges);
    --oldTarget->m_numberOfIncomingEdges;
    edge.m_block = newTarget;
    newTarget->didGainIncomingEdge();

    // Once no edge of the block leads to the old target, the block no longer lists the Upsilons for its Phis.
    if (oldTarget->m_phis.isEmpty() || from->m_upsilons.isEmpty())
        return;
    for (const Edge& other : from->successors()) {
        if (other.block() == oldTarget)
            return;
    }
    uint32_t numberKept = 0;
    for (Operation* upsilon : from->m_upsilons) {
        Operation* phi = upsilon->phi();
        if (!phi || phi->owner() != oldTarget)
            from->m_upsilons[numberKept++] = upsilon;
    }
    from->m_upsilons.shrink(numberKept);
}

void Order::replaceJumpWithBranch(Block* from, const Operation* phi, bool isInverted)
{
    Block* join = phi->owner();
    Operation* end = from->last();
    RELEASE_ASSERT(end->opcode() == Jump && from->numSuccessors() == 1 && from->successor(0).block() == join);
    RELEASE_ASSERT(join->last()->opcode() == Branch && join->numSuccessors() == 2);

    // The last Upsilon wins, as in Allocator::incomingValueOf().
    Operation* tested = nullptr;
    for (Operation* upsilon : from->upsilons()) {
        if (upsilon->phi() == phi)
            tested = upsilon->child(0);
    }
    RELEASE_ASSERT(tested);

    const Edge whenNotZero = join->successor(isInverted ? 1 : 0);
    const Edge whenZero = join->successor(isInverted ? 0 : 1);
    EdgeClass classOfJump = from->successor(0).edgeClass();
    retarget(from, 0, whenNotZero.block());
    from->m_successors[0].m_class = std::max(classOfJump, whenNotZero.edgeClass());
    from->m_successors.append(from->procedure().arena(), Edge(whenZero.block(), std::max(classOfJump, whenZero.edgeClass())));
    whenZero.block()->didGainIncomingEdge();

    end->m_opcode = Branch;
    end->m_origin = join->last()->origin();
    end->m_auxiliary = 1;
    end->m_children[0] = tested;
}

Block* Order::split(Block* from, unsigned successorIndex)
{
    ASSERT(from->isTerminated());
    Procedure& procedure = from->procedure();
    Arena& arena = procedure.arena();
    const Edge edge = from->successor(successorIndex);
    Block* target = edge.block();
    ASSERT(!target->isLandingPad());
    Block* middle = procedure.addBlock();

    auto append = [&](ArenaVector<Operation*>& list, Opcode opcode, Type type, Origin origin) {
        Operation* operation = arena.create<Operation>(opcode, type, origin);
        operation->m_owner = middle;
        operation->m_indexInBlock = list.size();
        procedure.didCreate(operation);
        list.append(arena, operation);
        return operation;
    };

    if (target->m_phis.isEmpty()) {
        // All that retarget() does where no edge leads to the new target yet and the old one has no Phi.
        ASSERT(target->m_numberOfIncomingEdges);
        --target->m_numberOfIncomingEdges;
        from->m_successors[successorIndex].m_block = middle;
        middle->didGainIncomingEdge();
    } else {
        for (Operation* upsilon : from->upsilons()) {
            Operation* phi = upsilon->phi();
            if (!phi || phi->owner() != target)
                continue;
            Operation* value = upsilon->child(0);
            Operation* moved = append(middle->m_upsilons, Upsilon, upsilon->type(), upsilon->origin());
            moved->m_auxiliary = 1;
            moved->m_children[0] = value;
            moved->m_payload.phi = phi;
            if (value->m_useCount != std::numeric_limits<uint16_t>::max())
                ++value->m_useCount;
        }
        retarget(from, successorIndex, middle);
    }

    append(middle->m_operations, Jump, Void, from->last()->origin());
    middle->appendSuccessorInline(Edge(target, edge.edgeClass()));
    return middle;
}

Block* Order::askBehind(Block* first, unsigned leavingSuccessorOfFirst, Block* second, unsigned leavingSuccessorOfSecond)
{
    Procedure& procedure = first->procedure();
    Arena& arena = procedure.arena();
    Operation* branch = first->last();
    RELEASE_ASSERT(branch->opcode() == Branch && first->numSuccessors() == 2 && second->last()->opcode() == Branch && second->numSuccessors() == 2);
    RELEASE_ASSERT(first->successor(1 - leavingSuccessorOfFirst).block() == second && second->numberOfIncomingEdges() == 1);
    RELEASE_ASSERT(first->upsilons().empty() && second->upsilons().empty());

    const Edge targetOfFirst = first->successor(leavingSuccessorOfFirst);
    const Edge wayOn = first->successor(1 - leavingSuccessorOfFirst);
    const Edge targetOfSecond = second->successor(leavingSuccessorOfSecond);
    const Edge wayOnOfSecond = second->successor(1 - leavingSuccessorOfSecond);
    Block* asked = procedure.addBlock();

    first->m_operations.takeLast();
    branch->m_owner = asked;
    branch->m_indexInBlock = 0;
    asked->m_operations.append(arena, branch);
    Operation* jump = arena.create<Operation>(Jump, Void, branch->origin());
    jump->m_owner = first;
    jump->m_indexInBlock = first->m_operations.size();
    procedure.didCreate(jump);
    first->m_operations.append(arena, jump);

    first->setSuccessors(wayOn);
    Edge toAsked(asked, targetOfSecond.edgeClass());
    if (leavingSuccessorOfSecond)
        second->setSuccessors(wayOnOfSecond, toAsked);
    else
        second->setSuccessors(toAsked, wayOnOfSecond);
    if (leavingSuccessorOfFirst)
        asked->setSuccessors(targetOfSecond, targetOfFirst);
    else
        asked->setSuccessors(targetOfFirst, targetOfSecond);
    return asked;
}

void Order::joinEdgesToOneBlock(Block* from)
{
    ASSERT(from->isTerminated());
    Operation* branch = from->last();
    if (branch->opcode() != Branch || from->numSuccessors() != 2)
        return;
    Edge& kept = from->m_successors[0];
    const Edge& other = from->m_successors[1];
    if (kept.m_block != other.m_block)
        return;

    kept.m_class = std::min(kept.m_class, other.m_class);
    ASSERT(kept.m_block->m_numberOfIncomingEdges >= 2);
    --kept.m_block->m_numberOfIncomingEdges;
    from->m_successors.shrink(1);
    branch->child(0)->loseUse();
}

namespace {

// Few procedures have a loop, a rare region or a rare landing pad, but those that do are the large ones and hold most of the blocks. So none of the three is paid for by a pass over all blocks.
// Until placeBlocks() the order is a list: each block names the one that stands behind it. A cycle or a rare region is put in its place by two links, wherever it belongs.
class BlockOrderBuilder {
    WTF_MAKE_NONCOPYABLE(BlockOrderBuilder);
public:
    explicit BlockOrderBuilder(Procedure& procedure)
        : m_proc(procedure)
        , m_arena(procedure.arena())
    {
    }

    void run();

    enum class Search : uint8_t {
        // Goes through every block and is after those outside rare regions.
        ForBlocksOutsideRareRegions,
        // Stays within rare regions.
        ForRareBlocks,
    };

    ArenaVector<Block*> m_blocksInOrder;
    ArenaVector<Block*> m_blocksInLayout;
    ArenaVector<BlockPlace> m_places;
    ArenaVector<Loop> m_loops;
    ArenaVector<RareRegion> m_rareRegions;
    unsigned m_numberOfBlocksOutsideRareRegions { 0 };
    Position m_end { 0 };
    uint32_t m_numberOfUnsplitEdges { 0 };
    std::span<Block* const> m_predecessors;
    std::span<const uint32_t> m_successorIndexesOfPredecessors;

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr uint32_t numberOfPasses = 3;

    enum Flag : uint32_t {
        IsReached = 1 << 0,
        IsRare = 1 << 1,
        // By the search that goes through every block.
        IsVisited = 1 << 2,
        // Normal edges alone lead to it from an entrypoint.
        IsLikely = 1 << 3,
        // Of a rare block: some edge into it comes from a rare block or gets a block whatever else holds of it. So every edge from outside rare regions gets one.
        IsEnteredThroughBlocksOnEdges = 1 << 4,
        HasUnsplitEdge = 1 << 5,
        // What endsInTestWhoseEdgesMayStayUnsplit() has answered.
        IsAskedWhetherEdgesMayStayUnsplit = 1 << 6,
        EdgesMayStayUnsplit = 1 << 7,
        // By the search that stays within rare regions. It has a mark of its own, so the first search takes none of its marks back.
        IsVisitedAmongRareBlocks = 1 << 8,
        // Reached, and it has several successors, a Patch ends it, or some edge of it is rare. findEdgesThatGetBlocks() looks at the edges of no other block.
        MayGetBlocksOnEdges = 1 << 9,
        // Of a block outside rare regions. Whatever any other edge of such a block leads to is not rare, nor is the block that splitEdges() puts on such an edge.
        HasRareEdge = 1 << 10,
        // Not reached so far, and listed among the targets of unlikely edges.
        WaitsBehindUnlikelyEdge = 1 << 11,
        // Of a block outside rare regions: it is the header of a cycle that holds another block outside rare regions.
        HoldsOtherBlocksInCycle = 1 << 12,
        IsRareLandingPad = 1 << 13,
        // Of a block on the path of a search. With two successors: the one that the search has still to take.
        FirstSuccessorIsLeft = 1 << 14,
        SecondSuccessorIsLeft = 1 << 15,
        // With more: m_progressThroughManySuccessors says how far the search has come with them.
        ManySuccessorsMayBeLeft = 1 << 16,
        SomeSuccessorMayBeLeft = FirstSuccessorIsLeft | SecondSuccessorIsLeft | ManySuccessorsMayBeLeft,
    };

    using Part = LayoutPart;
    static constexpr Part LikelyPart = LayoutPart::Likely;
    static constexpr Part UnlikelyPart = LayoutPart::Unlikely;
    static constexpr Part RarePart = LayoutPart::Rare;
    static constexpr uint32_t numberOfParts = numberOfLayoutParts;

    // What the steps keep of a block, in one record: a step that comes to a block by an edge asks for several of them.
    // A new record has zeros in its first half and ones in its second, so two stores make it.
    struct BlockState {
        // It has no header and is none.
        bool standsInNoCycle() const
        {
            static_assert(offsetof(BlockState, firstInCycle) == offsetof(BlockState, cycleHeader) + sizeof(uint32_t));
            uint64_t headerAndFirstInCycle;
            memcpy(&headerAndFirstInCycle, reinterpret_cast<const uint8_t*>(this) + offsetof(BlockState, cycleHeader), sizeof(headerAndFirstInCycle));
            return headerAndFirstInCycle == UINT64_MAX;
        }

        uint32_t flags { 0 };
        // Edges from reached blocks.
        uint32_t numberOfPredecessors { 0 };
        union {
            // On the path of a search: which it is of the blocks that the search has entered, from one. So it is greater than that of every block above it. Zero off the path, so zero behind a search.
            uint32_t rankOnPath { 0 };
            // Behind the searches, of the representative of a rare region: how many rare blocks stand no further back than its last landing pad. Zero without one.
            uint32_t rankOfRegion;
        };
        union {
            // On the path of a search: the block above it there, or none.
            uint32_t blockAboveOnPath { 0 };
            // Once a search is done with it: the block that stands behind it.
            uint32_t next;
        };
        // The block through which the search entered the innermost cycle that holds the block. Of such a block itself: the same for the cycle around its own.
        uint32_t cycleHeader { none };
        // Of a header: the first and the last of the blocks that stand behind it for being in its cycle, as far as the search is done with them.
        uint32_t firstInCycle { none };
        uint32_t lastInCycle { none };
        union {
            // Of a block outside rare regions.
            uint32_t indexOutsideRareRegions { none };
            // Of a rare block, from the search among the rare blocks on.
            uint32_t regionLink;
        };
    };
    static_assert(sizeof(BlockState) == 32);

    // Of a block on the path of a search that has more than two successors. They are gone through once for each class of edge, the last one first.
    struct ProgressThroughSuccessors {
        uint32_t successorsLeftInPass;
        uint32_t pass;
    };

    // A rare edge of a block outside rare regions, wherever it leads.
    struct RareEdgeFromOutside {
        Block* from;
        Block* target;
    };

    struct EdgeThatGetsBlock {
        // By Block::index().
        uint32_t from;
        uint32_t successorIndex;
    };

    struct RegionPlan {
        Block* firstEntry { nullptr };
        bool hasSeveralEntries { false };
        // Both are indexes into m_blocksOutsideRareRegions.
        uint32_t standsBehind { 0 };
        uint32_t standsAheadOf { none };
        // By Block::index(). From the one to the other its blocks hang together by BlockState::next.
        uint32_t firstBlock { none };
        uint32_t lastBlock { none };
        // In m_edgesToRareLandingPads.
        uint32_t firstEdgeIntoOtherRegion { none };
    };
    static_assert(sizeof(RegionPlan) == 32);

    // An edge from a block outside rare regions to a rare block.
    struct EntryOfRareRegion {
        Block* entry;
        // The index of the block outside in m_blocksOutsideRareRegions.
        uint32_t source;
    };

    // An edge between rare blocks that joins no regions. Both by Block::index().
    struct EdgeToRareLandingPad {
        uint32_t from;
        uint32_t landingPad;
        // The next edge out of the same region into another one.
        uint32_t nextOutOfRegion;
    };

    struct RareLandingPad {
        uint32_t block;
        // What BlockState::rankOfRegion is where this is the last landing pad of its region.
        uint32_t rank;
    };

    struct LoopHead {
        uint32_t firstBlock;
        uint32_t endBlock;
    };

    bool has(const Block* block, Flag flag) const { return m_states[block->index()].flags & flag; }
    bool isRare(const Block* block) const { return has(block, IsRare); }
    BlockPlace& placeOf(const Block* block) { return m_places[block->index()]; }
    uint32_t& numberOfBlocksIn(Part part) { return m_numberOfBlocksInPart[static_cast<uint32_t>(part)]; }

    void makeRoomForRareEdgeFromOutside();
    void findReachedBlocks();
    bool endsInTestWhoseEdgesMayStayUnsplit(const Block* from, BlockState& stateOfFrom);
    bool hasOneEdgeTo(const Block* from, const Block* target);
    void countEdgesOf(const Block* from);
    bool hasOneOfManyEdgesTo(const Block* from, const Block* target);
    void makeRoomForEdgeThatGetsBlock();
    void findEdgesThatGetBlocks();
    void splitEdges();
    void makePlaces();

    static uint32_t passOf(EdgeClass, Search);
    static bool secondOfTwoEdgesGoesFirst(const Edge& first, const Edge& second, Search);
    Block* takeFirstOfManySuccessors(const Block*, Search);
    Block* takeNextOfManySuccessors(const Block*, Search);
    void noteCycle(uint32_t block, uint32_t header);
    template<Search, typename RootAt> uint32_t orderBlocksReachedFrom(size_t numberOfRoots, const RootAt&);
    void takeRareBlocksOutOfOrder();

    static uint32_t representativeOf(BlockState*, uint32_t block);
    static uint32_t shortenWayToRepresentativeOf(BlockState*, uint32_t block);
    void joinRegionsOf(uint32_t first, uint32_t second);
    void noteEdgeToRareLandingPad(uint32_t from, uint32_t landingPad);
    void findRareRegions();
    void numberRareRegions();
    void placeRareRegions();
    void noteEdgeLeadingBack(BlockPlace& placeOfTarget, uint32_t orderIndexOfSource);
    void placeBlocks();
    void findLoops();
    void rotateLoops(uint32_t& beginOfMovedBlocks, uint32_t& endOfMovedBlocks);
    void placeLoops();
#if ASSERT_ENABLED
    void assertOrderHolds();
#endif

    Procedure& m_proc;
    Arena& m_arena;

    // By Block::index().
    ArenaVector<BlockState> m_states;

    // As findReachedBlocks() comes over them.
    ArenaVector<RareEdgeFromOutside> m_rareEdgesFromOutside;
    // hasOneOfManyEdgesTo(). By Block::index(), of the successors of m_blockWhoseEdgesAreCounted: how many of its edges lead there, without those that are listed to get a block.
    const Block* m_blockWhoseEdgesAreCounted { nullptr };
    ArenaVector<uint32_t> m_numberOfEdgesFromCountedBlock;
    // As their sources are numbered, and those of one source as it lists them.
    ArenaVector<EdgeThatGetsBlock> m_edgesThatGetBlocks;
    bool m_someRareBlockIsLandingPad { false };
    // More than two.
    bool m_someBlockHasManySuccessors { false };
    // Both count reached blocks alone. Every reached block comes to stand in the order, once.
    uint32_t m_numberOfEdges { 0 };
    uint32_t m_numberOfBlocksInPart[numberOfParts] { 0, 0, 0 };

    // One for each block on the path of a search that has more than two successors, that of the block furthest down the path last.
    ArenaVector<ProgressThroughSuccessors> m_progressThroughManySuccessors;
    // It is counted here, and not in a variable of the search, so that it takes none of the registers that the search goes by.
    size_t m_numberOfRootsLeft { 0 };
    // By Block::index(), as all below that name a block by a number.
    uint32_t m_firstBlockInOrder { none };
    uint32_t m_firstRareBlock { none };
    // As they stand in their order.
    const uint32_t* m_blocksOutsideRareRegions { nullptr };
    // As the blocks outside stand and their edges are listed.
    std::span<const EntryOfRareRegion> m_entriesOfRareRegions;
    uint32_t m_numberOfRareRegions { 0 };
    RegionPlan* m_regionPlans { nullptr };
    ArenaVector<EdgeToRareLandingPad> m_edgesToRareLandingPads;
    // As they stand among the rare blocks.
    ArenaVector<RareLandingPad> m_rareLandingPads;
    // placeBlocks(). Few blocks ask for them, so they are kept here and the registers of its loop are left to what every block asks for.
    uint32_t m_nextIndexInUnlikelyPart { 0 };
    uint32_t m_nextIndexInRarePart { 0 };
    ArenaVector<LoopHead> m_loopHeads;
};

// About one block in four has such an edge, so the list seldom has to grow a second time. A procedure without one asks for no list.
NEVER_INLINE void BlockOrderBuilder::makeRoomForRareEdgeFromOutside()
{
    m_rareEdgesFromOutside.reserve(m_arena, std::max(m_rareEdgesFromOutside.capacity() * 2, m_proc.numberOfBlocks() / 4 + 4));
}

// Which blocks are reached, how likely each is and how many edges lead to it hangs on no order, so the blocks are taken as they were reached and the list of them is never shortened: where a wave ends says how many it has reached.
void BlockOrderBuilder::findReachedBlocks()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    // splitEdges() appends the blocks that it makes.
    m_states.reserve(m_arena, numberOfBlocks + numberOfBlocks / 2 + 4);
    m_states.fill(m_arena, numberOfBlocks, BlockState { });
    BlockState* const states = m_states.begin();

    // Each of the two lists takes a block once at most.
    Block** const reachedBlocks = m_arena.allocateArray<Block*>(2 * static_cast<size_t>(numberOfBlocks));
    Block** const targetsOfUnlikelyEdges = reachedBlocks + numberOfBlocks;
    Block** nextBlockToTake = reachedBlocks;
    Block** endOfReachedBlocks = reachedBlocks;
    Block** endOfTargetsOfUnlikelyEdges = targetsOfUnlikelyEdges;
    uint32_t numberOfEdges = 0;
    bool everyBlockIsTerminated = true;

    auto reach = [&](Block* block, uint32_t flags) ALWAYS_INLINE_LAMBDA {
        BlockState& state = states[block->index()];
        if (state.flags & IsReached)
            return;
        state.flags = flags;
        *endOfReachedBlocks++ = block;
    };

    // What normal edges alone lead to, then what unlikely ones lead to as well, then the rest.
    enum class Wave : uint8_t {
        Likely,
        Unlikely,
        Rare,
    };
    auto takeReachedBlocks = [&](auto constantOfWave) ALWAYS_INLINE_LAMBDA {
        constexpr Wave wave = decltype(constantOfWave)::value;
        while (nextBlockToTake != endOfReachedBlocks) {
            Block* block = *nextBlockToTake++;
            if (!block->isTerminated()) [[unlikely]] {
                everyBlockIsTerminated = false;
                continue;
            }
            uint32_t flagsOfBlock = block->last()->opcode() == Patch ? MayGetBlocksOnEdges : 0;

            auto follow = [&](const Edge& edge) ALWAYS_INLINE_LAMBDA {
                Block* target = edge.block();
                BlockState& stateOfTarget = states[target->index()];
                ++stateOfTarget.numberOfPredecessors;
                if constexpr (wave == Wave::Rare) {
                    uint32_t flagsOfTarget = stateOfTarget.flags;
                    if (!(flagsOfTarget & IsReached)) {
                        flagsOfTarget = IsReached | IsRare;
                        *endOfReachedBlocks++ = target;
                    }
                    stateOfTarget.flags = flagsOfTarget | IsEnteredThroughBlocksOnEdges;
                    if (edge.isRare())
                        flagsOfBlock |= MayGetBlocksOnEdges;
                } else {
                    if (edge.edgeClass() != EdgeClass::Normal) [[unlikely]] {
                        if (edge.isRare()) {
                            flagsOfBlock |= MayGetBlocksOnEdges | HasRareEdge;
                            if (m_rareEdgesFromOutside.size() == m_rareEdgesFromOutside.capacity()) [[unlikely]]
                                makeRoomForRareEdgeFromOutside();
                            m_rareEdgesFromOutside.appendWithinCapacity(RareEdgeFromOutside { block, target });
                            return;
                        }
                        if constexpr (wave == Wave::Likely) {
                            if (!(stateOfTarget.flags & (IsReached | WaitsBehindUnlikelyEdge))) {
                                stateOfTarget.flags = WaitsBehindUnlikelyEdge;
                                *endOfTargetsOfUnlikelyEdges++ = target;
                            }
                            return;
                        }
                    }
                    if (stateOfTarget.flags & IsReached)
                        return;
                    stateOfTarget.flags = wave == Wave::Likely ? IsReached | IsLikely : IsReached;
                    *endOfReachedBlocks++ = target;
                }
            };

            const Edge* edges = block->successors().data();
            unsigned numberOfSuccessors = block->numSuccessors();
            // The edge that stays of two to one block is as likely as the likelier, so its target is as likely and as rare as with both.
            if (numberOfSuccessors == 2 && edges[0].block() == edges[1].block()) [[unlikely]] {
                Order::joinEdgesToOneBlock(block);
                numberOfSuccessors = block->numSuccessors();
            }
            numberOfEdges += numberOfSuccessors;
            if (numberOfSuccessors == 1)
                follow(edges[0]);
            else if (numberOfSuccessors == 2) {
                flagsOfBlock = MayGetBlocksOnEdges;
                follow(edges[0]);
                follow(edges[1]);
            } else if (numberOfSuccessors) [[unlikely]] {
                flagsOfBlock = MayGetBlocksOnEdges;
                for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex)
                    follow(edges[successorIndex]);
            }

            if (flagsOfBlock)
                states[block->index()].flags |= flagsOfBlock;
        }
    };

    for (const Entrypoint& entrypoint : m_proc.entrypoints())
        reach(entrypoint.block, IsReached | IsLikely);
    takeReachedBlocks(std::integral_constant<Wave, Wave::Likely> { });
    uint32_t numberOfLikelyBlocks = static_cast<uint32_t>(endOfReachedBlocks - reachedBlocks);

    for (Block** target = targetsOfUnlikelyEdges; target != endOfTargetsOfUnlikelyEdges; ++target)
        reach(*target, IsReached);
    takeReachedBlocks(std::integral_constant<Wave, Wave::Unlikely> { });
    uint32_t numberOfBlocksOutsideRareRegions = static_cast<uint32_t>(endOfReachedBlocks - reachedBlocks);

    for (const RareEdgeFromOutside& edge : m_rareEdgesFromOutside)
        reach(edge.target, IsReached | IsRare);
    takeReachedBlocks(std::integral_constant<Wave, Wave::Rare> { });

    numberOfBlocksIn(LikelyPart) = numberOfLikelyBlocks;
    numberOfBlocksIn(UnlikelyPart) = numberOfBlocksOutsideRareRegions - numberOfLikelyBlocks;
    numberOfBlocksIn(RarePart) = static_cast<uint32_t>(endOfReachedBlocks - reachedBlocks) - numberOfBlocksOutsideRareRegions;
    m_numberOfEdges = numberOfEdges;

    if (everyBlockIsTerminated) [[likely]]
        return;
    for (Block* block : m_proc.blocks()) {
        if (has(block, IsReached) && !block->isTerminated()) {
            m_proc.fail(FailureKind::MalformedProcedure, block->isEmpty() ? nullptr : block->last());
            return;
        }
    }
}

// Outside rare regions, with several successors.
ALWAYS_INLINE bool BlockOrderBuilder::endsInTestWhoseEdgesMayStayUnsplit(const Block* from, BlockState& stateOfFrom)
{
    uint32_t flags = stateOfFrom.flags;
    if (flags & IsAskedWhetherEdgesMayStayUnsplit)
        return flags & EdgesMayStayUnsplit;
    const Operation* end = from->last();
    bool answer = !(flags & IsRare) && from->numSuccessors() >= 2 && (end->opcode() == Switch || (end->opcode() == Branch && !mayTestSumByOverflow(end)));
    stateOfFrom.flags = flags | (answer ? IsAskedWhetherEdgesMayStayUnsplit | EdgesMayStayUnsplit : IsAskedWhetherEdgesMayStayUnsplit);
    return answer;
}

ALWAYS_INLINE bool BlockOrderBuilder::hasOneEdgeTo(const Block* from, const Block* target)
{
    if (from->numSuccessors() <= 2) [[likely]] {
        unsigned numberOfEdges = 0;
        for (const Edge& edge : from->successors())
            numberOfEdges += edge.block() == target;
        return numberOfEdges == 1;
    }
    return hasOneOfManyEdgesTo(from, target);
}

// A Switch may have as many edges as the procedure has blocks, and the question comes once for each of them. So its edges are counted once, by target.
// No edge has a block yet, so each leads where it led when it was appended.
NEVER_INLINE void BlockOrderBuilder::countEdgesOf(const Block* from)
{
    if (m_blockWhoseEdgesAreCounted == from)
        return;
    m_blockWhoseEdgesAreCounted = from;
    ArenaVector<uint32_t>& numberOfEdges = m_numberOfEdgesFromCountedBlock;
    if (numberOfEdges.isEmpty())
        numberOfEdges.fill(m_arena, m_proc.numberOfBlocks(), 0);
    for (const Edge& edge : from->successors())
        numberOfEdges[edge.block()->index()] = 0;
    for (const Edge& edge : from->successors())
        ++numberOfEdges[edge.block()->index()];
}

NEVER_INLINE bool BlockOrderBuilder::hasOneOfManyEdgesTo(const Block* from, const Block* target)
{
    countEdgesOf(from);
    return m_numberOfEdgesFromCountedBlock[target->index()] == 1;
}

// About one block in seven gets a block on one of its edges.
NEVER_INLINE void BlockOrderBuilder::makeRoomForEdgeThatGetsBlock()
{
    m_edgesThatGetBlocks.reserve(m_arena, std::max(m_edgesThatGetBlocks.capacity() * 2, m_proc.numberOfBlocks() / 4 + 4));
}

// Whether an edge gets a block hangs on its two ends as findReachedBlocks() has left them, and on which earlier edges of its source get one.
// So the edges are listed here, by a loop that seldom calls anything and so keeps in registers what it goes by, and splitEdges() makes the blocks.
void BlockOrderBuilder::findEdgesThatGetBlocks()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    BlockState* const states = m_states.begin();
    Block* const* const blocks = m_proc.blocks().data();

    // A frame that covers a rare region is set up where the region is entered. So the edges from outside into one of its blocks get blocks all together or not at all, and none stays as it is beside an edge from within.
    // findReachedBlocks() has marked what a rare block leads to.
    for (const RareEdgeFromOutside& edge : m_rareEdgesFromOutside) {
        BlockState& stateOfTarget = states[edge.target->index()];
        if (!(stateOfTarget.flags & IsRare) || stateOfTarget.numberOfPredecessors < 2 || (stateOfTarget.flags & IsEnteredThroughBlocksOnEdges))
            continue;
        if (!endsInTestWhoseEdgesMayStayUnsplit(edge.from, states[edge.from->index()]) || !hasOneEdgeTo(edge.from, edge.target))
            stateOfTarget.flags |= IsEnteredThroughBlocksOnEdges;
    }

    for (uint32_t index = 0; index < numberOfBlocks; ++index) {
        // A block outside rare regions that a rare block leads to has a predecessor outside too. So the edges out of a rare region from a block with several successors are among the critical ones,
        // and the one edge of a Patch out of its rare region is among those into a block with several predecessors.
        if (!(states[index].flags & MayGetBlocksOnEdges))
            continue;
        const Block* from = blocks[index];
        const Edge* const edges = from->successors().data();
        unsigned numberOfSuccessors = from->numSuccessors();
        // They are counted before the first of them is listed, and each that is listed is taken off the count.
        if (numberOfSuccessors > 2) [[unlikely]]
            countEdgesOf(from);

        for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex) {
            const Edge& edge = edges[successorIndex];
            const Block* target = edge.block();
            uint32_t indexOfTarget = target->index();
            const BlockState& stateOfTarget = states[indexOfTarget];
            bool isJoin = stateOfTarget.numberOfPredecessors >= 2;
            bool targetHasPhis = !target->phis().empty();
            // Moves into registers stand ahead of a Jump and nowhere else, and the Phis of a block ask for them however many edges lead to it.
            if (!isJoin && (numberOfSuccessors < 2 || !targetHasPhis))
                continue;
            if (target->isLandingPad())
                continue;

            if (!isJoin) {
                // The allocator looks for the Extracts of a Patch that ends its block at the head of the first successor.
                if (!successorIndex && from->last()->opcode() == Patch && from->last()->type().isTuple())
                    continue;
            } else if (!targetHasPhis) {
                // An edge that is listed to get a block leads to that block, and no longer to its target.
                auto isOnlyEdgeToTarget = [&]() ALWAYS_INLINE_LAMBDA -> bool {
                    if (numberOfSuccessors > 2) [[unlikely]]
                        return hasOneOfManyEdgesTo(from, target);
                    if (numberOfSuccessors < 2 || edges[1 - successorIndex].block() != target) [[likely]]
                        return true;
                    return successorIndex && !m_edgesThatGetBlocks.isEmpty() && m_edgesThatGetBlocks.last().from == index;
                };
                uint32_t flagsOfTarget = stateOfTarget.flags;
                bool targetIsRare = flagsOfTarget & IsRare;
                if (edge.isRare() == targetIsRare && !(targetIsRare && (flagsOfTarget & IsEnteredThroughBlocksOnEdges))
                    && endsInTestWhoseEdgesMayStayUnsplit(from, states[index]) && isOnlyEdgeToTarget()) {
                    ++m_numberOfUnsplitEdges;
                    states[index].flags |= HasUnsplitEdge;
                    continue;
                }
            }

            if (numberOfSuccessors > 2) [[unlikely]]
                --m_numberOfEdgesFromCountedBlock[indexOfTarget];
            if (m_edgesThatGetBlocks.size() == m_edgesThatGetBlocks.capacity()) [[unlikely]]
                makeRoomForEdgeThatGetsBlock();
            m_edgesThatGetBlocks.appendWithinCapacity(EdgeThatGetsBlock { index, successorIndex });
        }
    }
}

void BlockOrderBuilder::splitEdges()
{
    findEdgesThatGetBlocks();

    for (const EdgeThatGetsBlock& listed : m_edgesThatGetBlocks) {
        Block* from = m_proc.blockAt(listed.from);
        const Edge edge = from->successor(listed.successorIndex);
        Block* target = edge.block();
        bool targetIsRare = m_states[target->index()].flags & IsRare;
        uint32_t flagsOfFrom = m_states[listed.from].flags;
        Block* middle = Order::split(from, listed.successorIndex);
        ASSERT(middle->index() == m_states.size());
        BlockState stateOfMiddle;
        stateOfMiddle.numberOfPredecessors = 1;
        Part partOfMiddle = UnlikelyPart;
        if (edge.isRare() || (flagsOfFrom & IsRare)) {
            stateOfMiddle.flags = IsReached | IsRare;
            partOfMiddle = RarePart;
        } else if ((flagsOfFrom & IsLikely) && edge.edgeClass() == EdgeClass::Normal) {
            stateOfMiddle.flags = IsReached | IsLikely;
            partOfMiddle = LikelyPart;
        } else
            stateOfMiddle.flags = IsReached;
        m_states.append(m_arena, stateOfMiddle);
        ++numberOfBlocksIn(partOfMiddle);
        ++m_numberOfEdges;

        // The registers at the head of a block are fixed by the first edge into it that is not rare, which has to come from a block ahead of it.
        // A rare block may have no such edge but this one: the others may all lead back to it from within its region.
        if (edge.isRare() && targetIsRare) {
            middle->clearSuccessors();
            middle->appendSuccessor(Edge(target, EdgeClass::Normal));
        }
    }
}

// Each block gets its piece of the list of predecessors, empty so far. placeBlocks() makes a piece longer by each predecessor that it lists.
// The places that are kept for Order::splitLate() stay as the arena hands them out: it makes the place of each block that it makes.
void BlockOrderBuilder::makePlaces()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    uint32_t numberOfPlaces = numberOfBlocks + m_numberOfUnsplitEdges;
    if (m_proc.failure()) [[unlikely]] {
        m_places.grow(m_arena, numberOfPlaces);
        return;
    }

    Block** const storage = m_arena.allocateArray<Block*>(m_numberOfEdges);
    m_predecessors = std::span<Block* const>(storage, m_numberOfEdges);

    m_places.growWithoutInitializing(m_arena, numberOfPlaces);
    BlockPlace* place = m_places.begin();
    Block** piece = storage;
    const BlockState* state = m_states.begin();
    for (const BlockState* const endOfStates = state + numberOfBlocks; state != endOfStates; ++state, ++place) {
        new (NotNull, place) BlockPlace();
        place->predecessors = std::span<Block* const>(piece, static_cast<size_t>(0));
        piece += state->numberOfPredecessors;
    }
    RELEASE_ASSERT(piece == storage + m_numberOfEdges);
}

ALWAYS_INLINE uint32_t BlockOrderBuilder::passOf(EdgeClass edgeClass, Search search)
{
    // What is visited last stands first.
    // Outside rare regions the rare edges come last all the same: whatever another way leads to is visited by then, so a rare way places only what it alone leads on to.
    bool takesRareEdgesLast = search == Search::ForBlocksOutsideRareRegions;
    switch (edgeClass) {
    case EdgeClass::Rare:
        return takesRareEdgesLast ? 2 : 0;
    case EdgeClass::Unlikely:
        return takesRareEdgesLast ? 0 : 1;
    case EdgeClass::Normal:
        return takesRareEdgesLast ? 1 : 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Of two in one pass the second goes first.
ALWAYS_INLINE bool BlockOrderBuilder::secondOfTwoEdgesGoesFirst(const Edge& first, const Edge& second, Search search)
{
    return first.edgeClass() == second.edgeClass() || passOf(second.edgeClass(), search) <= passOf(first.edgeClass(), search);
}

NEVER_INLINE Block* BlockOrderBuilder::takeFirstOfManySuccessors(const Block* block, Search search)
{
    m_someBlockHasManySuccessors = true;
    m_progressThroughManySuccessors.append(m_arena, ProgressThroughSuccessors { block->numSuccessors(), 0 });
    return takeNextOfManySuccessors(block, search);
}

// Of the block at the end of the path. Whatever block with many successors was further down the path has had its last one taken.
NEVER_INLINE Block* BlockOrderBuilder::takeNextOfManySuccessors(const Block* block, Search search)
{
    ProgressThroughSuccessors& progress = m_progressThroughManySuccessors.last();
    // What is visited last stands first. Every case of a Switch is jumped to wherever it stands, and a search through its values ends in front of what no case takes: that falls through where it stands next.
    unsigned numberOfSuccessors = block->numSuccessors();
    bool lastStandsFirst = block->last()->opcode() == Switch;
    for (; progress.pass < numberOfPasses; ++progress.pass, progress.successorsLeftInPass = numberOfSuccessors) {
        while (progress.successorsLeftInPass) {
            unsigned left = --progress.successorsLeftInPass;
            const Edge& edge = block->successor(lastStandsFirst ? (left ? left - 1 : numberOfSuccessors - 1) : left);
            if (passOf(edge.edgeClass(), search) == progress.pass)
                return edge.block();
        }
    }
    m_progressThroughManySuccessors.shrink(m_progressThroughManySuccessors.size() - 1);
    return nullptr;
}

// `header` is on the path, above `block`, and `block` leads back to it. Weaves it into the headers that are known of `block`, which are on the path as well, the deeper one within.
// Only a block on the path is given another header. So the header of a block is settled when the search is done with the block.
// Wei, Mao, Zou, Chen: A New Algorithm for Identifying Loops in Decompilation.
NEVER_INLINE void BlockOrderBuilder::noteCycle(uint32_t block, uint32_t header)
{
    if (header == none || header == block)
        return;
    BlockState* const states = m_states.begin();
    uint32_t inner = block;
    uint32_t outer = header;
    while (states[inner].cycleHeader != none) {
        uint32_t current = states[inner].cycleHeader;
        if (current == outer)
            return;
        if (states[current].rankOnPath < states[outer].rankOnPath) {
            states[inner].cycleHeader = outer;
            inner = outer;
            outer = current;
        } else
            inner = current;
    }
    states[inner].cycleHeader = outer;
}

// Reverse postorder, the first root first, with the blocks of each cycle moved together behind its header.
// A block that is moved back over the rest of a cycle leads to no block of the cycle, so every edge that led forward still does.
// The blocks outside rare regions are ordered by every way there is. One that goes on through rare blocks alone stands ahead of what they rejoin, and in the cycle that they close:
// were those ways left out, it would stand behind all that, and the edge that rejoins would lead back over it.
// A block that the search is done with goes in front of all that it was done with before. Returns the first. The search through every block leaves the rare blocks in the order: takeRareBlocksOutOfOrder().
// The search among the rare blocks comes over each edge between two of them once, and joins the regions of the two as it does.
// The path lies in the records of its blocks: each names the block above it, and its flags say which of its successors are left.
template<BlockOrderBuilder::Search search, typename RootAt>
ALWAYS_INLINE uint32_t BlockOrderBuilder::orderBlocksReachedFrom(size_t numberOfRoots, const RootAt& rootAt)
{
    constexpr bool staysWithinRareRegions = search == Search::ForRareBlocks;
    constexpr uint32_t isVisited = staysWithinRareRegions ? IsVisitedAmongRareBlocks : IsVisited;

    // A landing pad is entered with every value in its slot, so an edge to one may come from another region. Nothing on such an edge sets a frame up or tears one down.
    [[maybe_unused]] const bool landingPadsBeginRegions = m_proc.traits().setsUpFrameAtEntry();

    BlockState* const states = m_states.begin();
    Block* const* const blocks = m_proc.blocks().data();
    uint32_t numberOfBlocksEntered = 0;
    uint32_t firstInOrder = none;
    // The block at the end of the path.
    uint32_t current = none;
    BlockState* stateOfCurrent = nullptr;
    // The successor of it that the search looks at next.
    Block* successor = nullptr;

    // Puts the block at the end of the path. Returns whether it has a successor, and has the search look at the first to take.
    auto enter = [&](Block* block, uint32_t index, BlockState& state, uint32_t flags) ALWAYS_INLINE_LAMBDA -> bool {
        flags |= isVisited;
        state.rankOnPath = ++numberOfBlocksEntered;
        state.blockAboveOnPath = current;
        current = index;
        stateOfCurrent = &state;
        if constexpr (staysWithinRareRegions) {
            // The rare blocks are searched again, among themselves. Nothing is kept of what the search through every block has found of them.
            state.cycleHeader = none;
            state.firstInCycle = none;
            if (block->isLandingPad()) [[unlikely]] {
                flags |= IsRareLandingPad;
                m_someRareBlockIsLandingPad = true;
            }
        }

        const Edge* edges = block->successors().data();
        unsigned numberOfSuccessors = block->numSuccessors();
        if (numberOfSuccessors == 1) [[likely]] {
            state.flags = flags;
            successor = edges[0].block();
            return true;
        }
        if (numberOfSuccessors == 2) {
            if (secondOfTwoEdgesGoesFirst(edges[0], edges[1], search)) {
                state.flags = flags | FirstSuccessorIsLeft;
                successor = edges[1].block();
            } else {
                state.flags = flags | SecondSuccessorIsLeft;
                successor = edges[0].block();
            }
            return true;
        }
        if (!numberOfSuccessors) {
            state.flags = flags;
            return false;
        }
        state.flags = flags | ManySuccessorsMayBeLeft;
        successor = takeFirstOfManySuccessors(block, search);
        ASSERT(successor);
        return true;
    };

    // Of the block at the end of the path. Returns whether one was left, and has the search look at it.
    auto takeSuccessorLeft = [&]() ALWAYS_INLINE_LAMBDA -> bool {
        uint32_t flags = stateOfCurrent->flags;
        if (!(flags & SomeSuccessorMayBeLeft))
            return false;
        const Block* block = blocks[current];
        if (flags & ManySuccessorsMayBeLeft) [[unlikely]] {
            successor = takeNextOfManySuccessors(block, search);
            if (successor)
                return true;
            stateOfCurrent->flags = flags & ~SomeSuccessorMayBeLeft;
            return false;
        }
        stateOfCurrent->flags = flags & ~SomeSuccessorMayBeLeft;
        successor = block->successors().data()[flags & SecondSuccessorIsLeft ? 1 : 0].block();
        return true;
    };

    // From a root that has a successor, until the path is empty.
    // Each turn of the outer loop looks at a successor. What enter() and takeSuccessorLeft() answer is asked where they answer it, and never carried round a loop: there the compiler would keep it as a value and test it again.
    auto searchBelowRoot = [&]() ALWAYS_INLINE_LAMBDA {
        while (true) {
            uint32_t index = successor->index();
            BlockState& state = states[index];
            uint32_t flags = state.flags;
            if (!staysWithinRareRegions || (flags & IsRare)) {
                if (!(flags & isVisited)) {
                    if constexpr (staysWithinRareRegions) {
                        // Nothing links to a block that is not visited yet, so it joins a region by its own link.
                        if (landingPadsBeginRegions && successor->isLandingPad()) [[unlikely]] {
                            state.regionLink = index;
                            ++m_numberOfRareRegions;
                            noteEdgeToRareLandingPad(current, index);
                        } else
                            state.regionLink = stateOfCurrent->regionLink;
                    } else if (state.numberOfPredecessors > 1 && !(flags & IsRare)) {
                        const Block* from = blocks[current];
                        if (from->numSuccessors() > 1 && from->last()->opcode() != Patch && !successor->isLandingPad())
                            m_places[index].sourceOfUnsplitEdgeThatCountsLast = from;
                    }
                    if (enter(successor, index, state, flags))
                        continue;
                } else {
                    if constexpr (staysWithinRareRegions) {
                        // The search has been in the block, so the flag says whether it is a landing pad.
                        if (landingPadsBeginRegions && (flags & IsRareLandingPad)) [[unlikely]]
                            noteEdgeToRareLandingPad(current, index);
                        else if (stateOfCurrent->regionLink != state.regionLink)
                            joinRegionsOf(current, index);
                    }
                    if (state.rankOnPath || state.cycleHeader != none) [[unlikely]] {
                        uint32_t header = index;
                        while (header != none && !states[header].rankOnPath)
                            header = states[header].cycleHeader;
                        if (header != none && header != current) {
                            // noteCycle() gives a block without a header the one that it is told.
                            uint32_t headerOfCurrent = stateOfCurrent->cycleHeader;
                            if (headerOfCurrent == none)
                                stateOfCurrent->cycleHeader = header;
                            else if (headerOfCurrent != header)
                                noteCycle(current, header);
                        }
                    }
                }
            }

            while (!takeSuccessorLeft()) {
                BlockState& stateOfFinished = *stateOfCurrent;
                ASSERT(!(stateOfFinished.flags & SomeSuccessorMayBeLeft));
                uint32_t above = stateOfFinished.blockAboveOnPath;
                stateOfFinished.rankOnPath = 0;
                if (stateOfFinished.standsInNoCycle()) [[likely]] {
                    stateOfFinished.next = firstInOrder;
                    firstInOrder = current;
                } else {
                    // The search is done with the block, which is a header or has one. So it is done with all that its cycle holds, and not with its header.
                    // The block and behind it the blocks of its cycle go in front of what stands behind the header so far, or of the whole order where it has no header.
                    uint32_t last = current;
                    if (stateOfFinished.firstInCycle != none) {
                        stateOfFinished.next = stateOfFinished.firstInCycle;
                        last = stateOfFinished.lastInCycle;
                    }
                    uint32_t header = stateOfFinished.cycleHeader;
                    if (header == none) {
                        states[last].next = firstInOrder;
                        firstInOrder = current;
                    } else {
                        BlockState& stateOfHeader = states[header];
                        uint32_t firstBehindHeader = stateOfHeader.firstInCycle;
                        if (firstBehindHeader == none)
                            stateOfHeader.lastInCycle = last;
                        states[last].next = firstBehindHeader;
                        stateOfHeader.firstInCycle = current;
                        // It is asked of blocks outside rare regions alone, which only the search through every block comes to.
                        if constexpr (!staysWithinRareRegions) {
                            if (!(stateOfFinished.flags & IsRare) || (stateOfFinished.flags & HoldsOtherBlocksInCycle))
                                stateOfHeader.flags |= HoldsOtherBlocksInCycle;
                        }

                        // The header is on the path, so some block is above. It leads back to the header through this one.
                        ASSERT(above != none);
                        if (above != header) {
                            BlockState& stateOfBlockAbove = states[above];
                            uint32_t headerOfBlockAbove = stateOfBlockAbove.cycleHeader;
                            if (headerOfBlockAbove == none)
                                stateOfBlockAbove.cycleHeader = header;
                            else if (headerOfBlockAbove != header)
                                noteCycle(above, header);
                        }
                    }
                }
                current = above;
                if (above == none)
                    return;
                stateOfCurrent = states + above;
            }
        }
    };

    for (m_numberOfRootsLeft = numberOfRoots; m_numberOfRootsLeft;) {
        Block* root = rootAt(--m_numberOfRootsLeft);
        uint32_t indexOfRoot = root->index();
        BlockState& stateOfRoot = states[indexOfRoot];
        uint32_t flagsOfRoot = stateOfRoot.flags;
        if (flagsOfRoot & isVisited)
            continue;
        if constexpr (staysWithinRareRegions) {
            stateOfRoot.regionLink = indexOfRoot;
            ++m_numberOfRareRegions;
        }

        ASSERT(current == none);
        if (enter(root, indexOfRoot, stateOfRoot, flagsOfRoot)) {
            searchBelowRoot();
            continue;
        }
        // It leads nowhere, so it stands in no cycle.
        stateOfRoot.rankOnPath = 0;
        stateOfRoot.next = firstInOrder;
        firstInOrder = indexOfRoot;
        current = none;
    }
    ASSERT(m_progressThroughManySuccessors.isEmpty());

    return firstInOrder;
}

// A rare block has a rare edge from outside on every way to it.
void BlockOrderBuilder::takeRareBlocksOutOfOrder()
{
    BlockState* const states = m_states.begin();
    Block* const* const blocks = m_proc.blocks().data();
    uint32_t numberOfBlocksOutside = numberOfBlocksIn(LikelyPart) + numberOfBlocksIn(UnlikelyPart);
    uint32_t* const blocksOutside = m_arena.allocateArray<uint32_t>(numberOfBlocksOutside);
    // Each is a rare edge of a block outside.
    EntryOfRareRegion* const entries = m_arena.allocateArray<EntryOfRareRegion>(m_rareEdgesFromOutside.size());
    EntryOfRareRegion* endOfEntries = entries;

    uint32_t numberOutside = 0;
    // Where the next block outside is to be named.
    uint32_t* link = &m_firstBlockInOrder;
    for (uint32_t index = m_firstBlockInOrder; index != none;) {
        BlockState& state = states[index];
        uint32_t flags = state.flags;
        uint32_t next = state.next;
        if (!(flags & IsRare)) {
            *link = index;
            link = &state.next;
            blocksOutside[numberOutside] = index;
            state.indexOutsideRareRegions = numberOutside;

            // What is kept of the search names no rare block.
            uint32_t header = state.cycleHeader;
            if (header != none) {
                while (header != none && (states[header].flags & IsRare))
                    header = states[header].cycleHeader;
                state.cycleHeader = header;
            }

            if (flags & HasRareEdge) {
                for (const Edge& edge : blocks[index]->successors()) {
                    if (states[edge.block()->index()].flags & IsRare)
                        *endOfEntries++ = EntryOfRareRegion { edge.block(), numberOutside };
                }
            }
            ++numberOutside;
        }
        index = next;
    }
    *link = none;
    RELEASE_ASSERT(numberOutside == numberOfBlocksOutside);

    m_blocksOutsideRareRegions = blocksOutside;
    m_entriesOfRareRegions = std::span<const EntryOfRareRegion>(entries, static_cast<size_t>(endOfEntries - entries));
}

ALWAYS_INLINE uint32_t BlockOrderBuilder::representativeOf(BlockState* states, uint32_t block)
{
    uint32_t link = states[block].regionLink;
    if (states[link].regionLink == link) [[likely]]
        return link;
    return shortenWayToRepresentativeOf(states, block);
}

NEVER_INLINE uint32_t BlockOrderBuilder::shortenWayToRepresentativeOf(BlockState* states, uint32_t block)
{
    while (states[block].regionLink != block) {
        states[block].regionLink = states[states[block].regionLink].regionLink;
        block = states[block].regionLink;
    }
    return block;
}

// While the search among the rare blocks is under way. No region has a rank yet.
NEVER_INLINE void BlockOrderBuilder::joinRegionsOf(uint32_t first, uint32_t second)
{
    BlockState* const states = m_states.begin();
    uint32_t kept = representativeOf(states, first);
    uint32_t joined = representativeOf(states, second);
    if (kept == joined)
        return;
    if (kept > joined)
        std::swap(kept, joined);
    states[joined].regionLink = kept;
    --m_numberOfRareRegions;
}

NEVER_INLINE void BlockOrderBuilder::noteEdgeToRareLandingPad(uint32_t from, uint32_t landingPad)
{
    m_edgesToRareLandingPads.append(m_arena, EdgeToRareLandingPad { from, landingPad, none });
}

// The search among the rare blocks has joined what an edge joins. Only an edge that it has listed may lead from one region into another, and only a landing pad gives its region a rank.
void BlockOrderBuilder::findRareRegions()
{
    if (!m_someRareBlockIsLandingPad)
        return;

    BlockState* const states = m_states.begin();
    uint32_t rank = 0;
    for (uint32_t index = m_firstRareBlock; index != none; index = states[index].next) {
        ++rank;
        if (states[index].flags & IsRareLandingPad) [[unlikely]] {
            states[representativeOf(states, index)].rankOfRegion = rank;
            m_rareLandingPads.append(m_arena, RareLandingPad { index, rank });
        }
    }

    // Regions are placed by rising rank, each behind those that lead into it. So ranks have to rise along the edges between regions, as they do where one handler lies within another.
    // Where they do not, every region with a landing pad is joined with those that it leads into.
    bool ranksRiseAlongEdges = true;
    for (const EdgeToRareLandingPad& edge : m_edgesToRareLandingPads) {
        uint32_t from = representativeOf(states, edge.from);
        uint32_t target = representativeOf(states, edge.landingPad);
        if (from != target)
            ranksRiseAlongEdges &= states[from].rankOfRegion < states[target].rankOfRegion;
    }
    if (ranksRiseAlongEdges)
        return;
    // A region without a rank holds no landing pad, so nothing is joined with it and it never comes to have one: in whatever order the edges are gone through, the same ones join.
    for (const EdgeToRareLandingPad& edge : m_edgesToRareLandingPads) {
        uint32_t kept = representativeOf(states, edge.from);
        uint32_t joined = representativeOf(states, edge.landingPad);
        if (kept == joined || !states[kept].rankOfRegion)
            continue;
        if (kept > joined)
            std::swap(kept, joined);
        states[joined].regionLink = kept;
        states[kept].rankOfRegion = std::max(states[kept].rankOfRegion, states[joined].rankOfRegion);
        --m_numberOfRareRegions;
    }
}

// The regions without a rank come first, as their first blocks stand among the rare blocks, then the others by rising rank.
void BlockOrderBuilder::numberRareRegions()
{
    BlockState* const states = m_states.begin();
    BlockPlace* const places = m_places.begin();
    uint32_t numberOfRegions = m_numberOfRareRegions;
    RegionPlan* const plans = m_arena.allocateArray<RegionPlan>(numberOfRegions);
    for (uint32_t region = 0; region < numberOfRegions; ++region)
        new (NotNull, plans + region) RegionPlan();
    m_regionPlans = plans;

    uint32_t firstRegionWithRank = numberOfRegions;
    for (uint32_t number = m_rareLandingPads.size(); number--;) {
        const RareLandingPad& landingPad = m_rareLandingPads[number];
        uint32_t representative = representativeOf(states, landingPad.block);
        if (states[representative].rankOfRegion == landingPad.rank)
            places[representative].rareRegion = --firstRegionWithRank;
    }

    uint32_t numberOfRegionsWithoutRank = 0;
    uint32_t numberOfRareBlocks = 0;
    for (uint32_t index = m_firstRareBlock; index != none;) {
        uint32_t next = states[index].next;
        uint32_t representative = representativeOf(states, index);
        uint32_t region = places[representative].rareRegion;
        if (region == noRareRegion) {
            region = numberOfRegionsWithoutRank++;
            places[representative].rareRegion = region;
        }
        places[index].rareRegion = region;

        RegionPlan& plan = plans[region];
        if (plan.firstBlock == none)
            plan.firstBlock = index;
        else
            states[plan.lastBlock].next = index;
        plan.lastBlock = index;
        ++numberOfRareBlocks;
        index = next;
    }
    RELEASE_ASSERT(numberOfRegionsWithoutRank == firstRegionWithRank);
    RELEASE_ASSERT(numberOfRareBlocks == numberOfBlocksIn(RarePart));
}

// Each rare region stands in one piece, behind every block that leads into it. Of those that stand at one place, the one with the lower number comes first.
void BlockOrderBuilder::placeRareRegions()
{
    BlockState* const states = m_states.begin();
    BlockPlace* const places = m_places.begin();
    Block* const* const blocks = m_proc.blocks().data();
    RegionPlan* const plans = m_regionPlans;
    uint32_t numberOfRegions = m_numberOfRareRegions;

    // Whether a region has several entries, and behind what it stands, hangs on no order among its entries.
    auto noteEntry = [&](Block* entry, uint32_t standsBehind) ALWAYS_INLINE_LAMBDA {
        BlockPlace& place = places[entry->index()];
        RegionPlan& plan = plans[place.rareRegion];
        plan.standsBehind = std::max(plan.standsBehind, standsBehind);
        if (!plan.firstEntry)
            plan.firstEntry = entry;
        else if (plan.firstEntry != entry || !entry->isLandingPad())
            plan.hasSeveralEntries = true;
        place.entersRareRegion = true;
    };

    for (const EntryOfRareRegion& entry : m_entriesOfRareRegions)
        noteEntry(entry.entry, entry.source);

    EdgeToRareLandingPad* const edgesToLandingPads = m_edgesToRareLandingPads.begin();
    for (uint32_t number = 0; number < m_edgesToRareLandingPads.size(); ++number) {
        EdgeToRareLandingPad& edge = edgesToLandingPads[number];
        uint32_t region = places[edge.from].rareRegion;
        if (region == places[edge.landingPad].rareRegion)
            continue;
        edge.nextOutOfRegion = plans[region].firstEdgeIntoOtherRegion;
        plans[region].firstEdgeIntoOtherRegion = number;
    }

    for (uint32_t region = 0; region < numberOfRegions; ++region) {
        RegionPlan& plan = plans[region];
        uint32_t standsBehind = plan.standsBehind;
        uint32_t standsAheadOf = none;
        uint32_t lastBlock = plan.lastBlock;
        for (uint32_t index = plan.firstBlock; ; index = states[index].next) {
            const Block* block = blocks[index];
            const Edge* edge = block->successors().data();
            unsigned numberOfSuccessors = block->numSuccessors();
            bool leadsOutOfRareRegions = false;
            for (const Edge* const endOfEdges = edge + numberOfSuccessors; edge != endOfEdges; ++edge) {
                const BlockState& stateOfTarget = states[edge->block()->index()];
                if (stateOfTarget.flags & IsRare)
                    continue;
                leadsOutOfRareRegions = true;
                uint32_t rejoined = stateOfTarget.indexOutsideRareRegions;
                if (rejoined > standsBehind)
                    standsAheadOf = std::min(standsAheadOf, rejoined);
            }
            if (leadsOutOfRareRegions && numberOfSuccessors == 1)
                places[index].leavesRareRegion = true;
            if (index == lastBlock)
                break;
        }
        if (standsAheadOf == none)
            standsAheadOf = standsBehind + 1;
        plan.standsAheadOf = standsAheadOf;

        for (uint32_t number = plan.firstEdgeIntoOtherRegion; number != none; number = edgesToLandingPads[number].nextOutOfRegion) {
            ASSERT(places[edgesToLandingPads[number].landingPad].rareRegion > region);
            noteEntry(blocks[edgesToLandingPads[number].landingPad], standsAheadOf - 1);
        }
    }

    // Each goes right behind the block outside that it stands behind, the one with the highest number first: so in front of those that went there before it.
    m_rareRegions.growWithoutInitializing(m_arena, numberOfRegions);
    RareRegion* const rareRegions = m_rareRegions.begin();
    const uint32_t* const blocksOutside = m_blocksOutsideRareRegions;
    for (uint32_t region = numberOfRegions; region--;) {
        const RegionPlan& plan = plans[region];
        new (NotNull, rareRegions + region) RareRegion { plan.hasSeveralEntries ? nullptr : plan.firstEntry };
        uint32_t& link = plan.standsAheadOf ? states[blocksOutside[plan.standsAheadOf - 1]].next : m_firstBlockInOrder;
        states[plan.lastBlock].next = link;
        link = plan.firstBlock;
    }
}

// The sources come as they stand in the order, so the last one that is noted of a target is the last that leads back to it.
// Until findLoops() BlockPlace::loop of a loop head is its number in m_loopHeads.
NEVER_INLINE void BlockOrderBuilder::noteEdgeLeadingBack(BlockPlace& placeOfTarget, uint32_t orderIndexOfSource)
{
    if (placeOfTarget.isLoopHead) {
        m_loopHeads[placeOfTarget.loop].endBlock = orderIndexOfSource + 1;
        return;
    }
    placeOfTarget.isLoopHead = true;
    placeOfTarget.loop = m_loopHeads.size();
    m_loopHeads.append(m_arena, LoopHead { placeOfTarget.orderIndex, orderIndexOfSource + 1 });
}

// Goes over the blocks as they stand in the order and gives each all that its place in the order decides, then over their edges.
// Either loop keeps in registers what it goes by. One loop for both does not.
void BlockOrderBuilder::placeBlocks()
{
    uint32_t numberOfBlocks = numberOfBlocksIn(LikelyPart) + numberOfBlocksIn(UnlikelyPart) + numberOfBlocksIn(RarePart);

    // An arm that is unlikely stands behind everything likely, so the likely way falls through to the join and the arm jumps back to it.
    bool likelyPartComesLast = m_proc.traits().fallsIntoWhatLiesBehind;
    uint32_t firstInPart[numberOfParts];
    uint32_t numberOfBlocksInFront = 0;
    for (uint32_t step = 0; step < numberOfParts; ++step) {
        uint32_t part = likelyPartComesLast ? numberOfParts - 1 - step : step;
        firstInPart[part] = numberOfBlocksInFront;
        numberOfBlocksInFront += m_numberOfBlocksInPart[part];
    }
    uint32_t nextIndexInLikelyPart = firstInPart[static_cast<uint32_t>(LikelyPart)];
    m_nextIndexInUnlikelyPart = firstInPart[static_cast<uint32_t>(UnlikelyPart)];
    m_nextIndexInRarePart = firstInPart[static_cast<uint32_t>(RarePart)];
    m_numberOfBlocksOutsideRareRegions = numberOfBlocks - numberOfBlocksIn(RarePart);

    m_blocksInOrder.growWithoutInitializing(m_arena, numberOfBlocks);
    m_blocksInLayout.growWithoutInitializing(m_arena, numberOfBlocks);
    uint32_t* successorIndexes = nullptr;
    if (m_someBlockHasManySuccessors) [[unlikely]] {
        std::span<uint32_t> indexes = m_arena.createSpan<uint32_t>(m_numberOfEdges);
        m_successorIndexesOfPredecessors = indexes;
        successorIndexes = indexes.data();
    }

    const BlockState* const states = m_states.begin();
    BlockPlace* const places = m_places.begin();
    Block* const* const blocks = m_proc.blocks().data();
    Block** const blocksInOrder = m_blocksInOrder.begin();
    Block** const blocksInLayout = m_blocksInLayout.begin();

    uint32_t numberOfBlocksPlaced = 0;
    uint64_t position = 0;
    for (uint32_t index = m_firstBlockInOrder; index != none;) {
        const BlockState& state = states[index];
        uint32_t flags = state.flags;
        Block* block = blocks[index];
        BlockPlace& place = places[index];
        index = state.next;

        // A place is made with LayoutPart::Likely.
        uint32_t layoutIndex;
        if (flags & IsLikely) [[likely]]
            layoutIndex = nextIndexInLikelyPart++;
        else if (flags & IsRare) {
            layoutIndex = m_nextIndexInRarePart++;
            place.layoutPart = RarePart;
        } else {
            layoutIndex = m_nextIndexInUnlikelyPart++;
            place.layoutPart = UnlikelyPart;
        }
        blocksInOrder[numberOfBlocksPlaced] = block;
        blocksInLayout[layoutIndex] = block;
        place.orderIndex = numberOfBlocksPlaced++;
        place.layoutIndex = layoutIndex;
        place.head = static_cast<Position>(position);

        position += 2 * (static_cast<uint64_t>(block->size()) + 1);
        if (flags & HasUnsplitEdge) [[unlikely]]
            position += positionsOfBlockSplitLate * static_cast<uint64_t>(block->numSuccessors());
    }
    RELEASE_ASSERT(numberOfBlocksPlaced == numberOfBlocks);

    RELEASE_ASSERT(position < noPosition);
    m_end = static_cast<Position>(position);

    // Every block has its index by now, so an edge leads back where the index of its target is no greater.
    auto listAsPredecessor = [&](Block* block, uint32_t orderIndex, const Edge& edge) ALWAYS_INLINE_LAMBDA -> Block* const* {
        BlockPlace& placeOfTarget = places[edge.block()->index()];
        Block* const* piece = placeOfTarget.predecessors.data();
        size_t numberListed = placeOfTarget.predecessors.size();
        placeOfTarget.predecessors = std::span<Block* const>(piece, numberListed + 1);
        const_cast<Block**>(piece)[numberListed] = block;
        if (placeOfTarget.orderIndex <= orderIndex) [[unlikely]]
            noteEdgeLeadingBack(placeOfTarget, orderIndex);
        return piece + numberListed;
    };

    Block* const* const storage = m_predecessors.data();
    for (uint32_t orderIndex = 0; orderIndex < numberOfBlocks; ++orderIndex) {
        Block* block = blocksInOrder[orderIndex];
        const Edge* edges = block->successors().data();
        unsigned numberOfSuccessors = block->numSuccessors();
        if (numberOfSuccessors <= 2) [[likely]] {
            if (numberOfSuccessors) {
                listAsPredecessor(block, orderIndex, edges[0]);
                if (numberOfSuccessors == 2)
                    listAsPredecessor(block, orderIndex, edges[1]);
            }
        } else {
            for (uint32_t successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex)
                successorIndexes[listAsPredecessor(block, orderIndex, edges[successorIndex]) - storage] = successorIndex;
        }
    }
}

// From its head to the last block that jumps back to the head, a loop is rotated until it ends with a Branch that leaves it. The jump back then falls through, and a turn takes one branch.
// It goes by the cycles of the search and not by the Loop intervals, which hold rare blocks too.
// No block is moved but between the two indexes that it hands back.
void BlockOrderBuilder::rotateLoops(uint32_t& beginOfMovedBlocks, uint32_t& endOfMovedBlocks)
{
    enum class ExitPlacement : uint8_t {
        FallsThrough,
        SamePart,
        OtherPart,
        None,
    };

    const BlockState* const states = m_states.begin();
    const BlockPlace* const places = m_places.begin();
    Block* const* const blocksInOrder = m_blocksInOrder.begin();
    Block** const blocksInLayout = m_blocksInLayout.begin();
    const Loop* const loops = m_loops.begin();
    uint32_t numberOfBlocks = m_blocksInLayout.size();

    // A block has one of the two flags at most, so two blocks stand in one part of the layout where they agree in both.
    auto flagsOfPartOf = [&](const Block* block) ALWAYS_INLINE_LAMBDA -> uint32_t {
        return states[block->index()].flags & (IsRare | IsLikely);
    };

    // Inner loops first. That moves no direct member of the loop around them, so BlockPlace::layoutIndex still holds of those.
    for (uint32_t loopIndex = m_loops.size(); loopIndex--;) {
        Block* head = blocksInOrder[loops[loopIndex].firstBlock];
        uint32_t flagsOfPart = flagsOfPartOf(head);
        if (flagsOfPart & IsRare)
            continue;

        uint32_t header = head->index();
        auto isDirectMemberOfLoop = [&](const Block* block) ALWAYS_INLINE_LAMBDA {
            const BlockState& state = states[block->index()];
            return state.cycleHeader == header && !(state.flags & HoldsOtherBlocksInCycle);
        };
        auto staysInLoop = [&](const Block* block) ALWAYS_INLINE_LAMBDA {
            if (flagsOfPartOf(block) != flagsOfPart)
                return false;
            uint32_t current = block->index();
            while (current != none && current != header)
                current = states[current].cycleHeader;
            return current == header;
        };

        uint32_t begin = places[header].layoutIndex;
        uint32_t end = begin;
        for (const Block* predecessor : places[header].predecessors) {
            if (isDirectMemberOfLoop(predecessor) && flagsOfPartOf(predecessor) == flagsOfPart && predecessor->last()->isUnconditionalJump())
                end = std::max(end, places[predecessor->index()].layoutIndex + 1);
        }
        if (end == begin)
            continue;
        ASSERT(begin + 1 < end && blocksInLayout[begin] == head);

        const Block* blockAfterLoop = end < numberOfBlocks ? blocksInLayout[end] : nullptr;
        ExitPlacement best = ExitPlacement::None;
        uint32_t indexOfBest = end;
        for (uint32_t index = begin; index + 1 < end && best != ExitPlacement::FallsThrough; ++index) {
            const Block* block = blocksInLayout[index];
            if ((block != head && !isDirectMemberOfLoop(block)) || block->last()->opcode() != Branch || block->numSuccessors() != 2)
                continue;
            const Block* first = block->successor(0).block();
            const Block* second = block->successor(1).block();
            bool firstStaysInLoop = staysInLoop(first);
            if (firstStaysInLoop == staysInLoop(second))
                continue;
            const Block* exit = firstStaysInLoop ? second : first;
            ExitPlacement placement = exit == blockAfterLoop ? ExitPlacement::FallsThrough : flagsOfPartOf(exit) == flagsOfPart ? ExitPlacement::SamePart : ExitPlacement::OtherPart;
            if (placement < best) {
                best = placement;
                indexOfBest = index;
            }
        }
        if (best == ExitPlacement::None)
            continue;
        Block** loopBlocks = blocksInLayout + begin;
        std::rotate(loopBlocks, loopBlocks + (indexOfBest + 1 - begin), loopBlocks + (end - begin));
        beginOfMovedBlocks = std::min(beginOfMovedBlocks, begin);
        endOfMovedBlocks = std::max(endOfMovedBlocks, end);
    }
}

// Every block that an edge leads back to begins a loop of its own: a value that is defined ahead of that block and read behind it is live as far as the edge, whatever other loop both stand in.
// So where two intervals overlap and neither holds the other, the one that begins first is made to end with the other, and both stay.
// A place is made with no loop, so it goes over the blocks that stand in a loop and no others.
void BlockOrderBuilder::findLoops()
{
    std::sort(m_loopHeads.begin(), m_loopHeads.end(), [](const LoopHead& first, const LoopHead& second) {
        return first.firstBlock < second.firstBlock;
    });

    BlockPlace* const places = m_places.begin();
    Block* const* const blocksInOrder = m_blocksInOrder.begin();
    const LoopHead* nextHead = m_loopHeads.begin();
    const LoopHead* const endOfHeads = m_loopHeads.end();
    // One for each head, so the list stays where it lies.
    m_loops.reserve(m_arena, m_loopHeads.size());
    Loop* const loops = m_loops.begin();

    // A loop stays open as long as a loop within it does, and takes over the end of that loop when it closes.
    ArenaVector<uint32_t> openLoops;
    auto closeLoopsEndingAt = [&](uint32_t index) {
        while (!openLoops.isEmpty() && loops[openLoops.last()].endBlock <= index) {
            uint32_t endBlock = loops[openLoops.takeLast()].endBlock;
            if (!openLoops.isEmpty()) {
                Loop& parent = loops[openLoops.last()];
                parent.endBlock = std::max(parent.endBlock, endBlock);
            }
        }
    };

    while (nextHead != endOfHeads) {
        uint32_t index = nextHead->firstBlock;
        do {
            if (nextHead != endOfHeads && nextHead->firstBlock == index) {
                Loop loop;
                loop.parent = openLoops.isEmpty() ? noLoop : openLoops.last();
                loop.firstBlock = index;
                loop.endBlock = nextHead->endBlock;
                openLoops.append(m_arena, m_loops.size());
                m_loops.appendWithinCapacity(loop);
                ++nextHead;
            }

            // No loop opens ahead of the next head, and none closes as long as the innermost goes on. Both lie behind this block.
            uint32_t innermost = openLoops.last();
            uint32_t endOfRun = loops[innermost].endBlock;
            if (nextHead != endOfHeads)
                endOfRun = std::min(endOfRun, nextHead->firstBlock);
            do
                places[blocksInOrder[index]->index()].loop = innermost;
            while (++index < endOfRun);
            closeLoopsEndingAt(index);
        } while (!openLoops.isEmpty());
    }
}

// Where some edge leads back.
NEVER_INLINE void BlockOrderBuilder::placeLoops()
{
    findLoops();

    uint32_t beginOfMovedBlocks = m_blocksInLayout.size();
    uint32_t endOfMovedBlocks = 0;
    rotateLoops(beginOfMovedBlocks, endOfMovedBlocks);
    BlockPlace* const places = m_places.begin();
    Block* const* const blocksInLayout = m_blocksInLayout.begin();
    for (uint32_t index = beginOfMovedBlocks; index < endOfMovedBlocks; ++index)
        places[blocksInLayout[index]->index()].layoutIndex = index;

    for (Loop& loop : m_loops) {
        loop.begin = placeOf(m_blocksInOrder[loop.firstBlock]).head;
        loop.end = loop.endBlock < m_blocksInOrder.size() ? placeOf(m_blocksInOrder[loop.endBlock]).head : m_end;
    }
}

#if ASSERT_ENABLED
void BlockOrderBuilder::assertOrderHolds()
{
    for (Block* block : m_blocksInOrder) {
        const BlockPlace& place = placeOf(block);
        ASSERT(place.predecessors.size() == m_states[block->index()].numberOfPredecessors);
        ASSERT(m_blocksInLayout[place.layoutIndex] == block);
        ASSERT(place.predecessors.empty() == block->isEntry());
        ASSERT(place.predecessors.empty() || placeOf(place.predecessors.front()).orderIndex < place.orderIndex);
        ASSERT(!place.entersRareRegion || block->isLandingPad() || place.predecessors.size() == 1 || std::ranges::none_of(place.predecessors, [&](const Block* predecessor) { return isRare(predecessor); }));
        for (const Edge& edge : block->successors()) {
            Block* target = edge.block();
            if (isRare(target) && placeOf(target).rareRegion != place.rareRegion) {
                ASSERT(placeOf(target).entersRareRegion && place.orderIndex < placeOf(target).orderIndex);
                ASSERT(!place.isRare() || target->isLandingPad());
            }
            if (target->isLandingPad())
                continue;
            bool endsInPatch = block->last()->opcode() == Patch;
            bool holdsExtractsOfBlock = endsInPatch && block->last()->type().isTuple() && &edge == &block->successor(0);
            ASSERT(block->numSuccessors() == 1 || placeOf(target).predecessors.size() == 1 || (!endsInPatch && !place.isRare() && target->phis().empty()));
            ASSERT(block->numSuccessors() == 1 || target->phis().empty() || holdsExtractsOfBlock);
            ASSERT(!endsInPatch || placeOf(target).predecessors.size() == 1);
            ASSERT(!place.leavesRareRegion || block->last()->isUnconditionalJump());
            ASSERT(!edge.isRare() || placeOf(target).predecessors.size() == 1 || !isRare(target) || Order::isUnsplit(block, placeOf(target)));
            ASSERT(isRare(target) || !place.isRare() || place.leavesRareRegion);
        }
    }
}
#endif

void BlockOrderBuilder::run()
{
    findReachedBlocks();
    if (!m_proc.failure())
        splitEdges();
    makePlaces();
    if (m_proc.failure())
        return;

    // Where the roots are listed is read anew for each root, so that it takes none of the registers that the search goes by.
    m_firstBlockInOrder = orderBlocksReachedFrom<Search::ForBlocksOutsideRareRegions>(m_proc.entrypoints().size(), [&](size_t index) {
        return m_proc.entrypoints().data()[index].block;
    });

    if (numberOfBlocksIn(RarePart)) {
        takeRareBlocksOutOfOrder();
        m_firstRareBlock = orderBlocksReachedFrom<Search::ForRareBlocks>(m_entriesOfRareRegions.size(), [&](size_t index) {
            return m_entriesOfRareRegions.data()[index].entry;
        });
        findRareRegions();
        numberRareRegions();
        placeRareRegions();
    }
    placeBlocks();
    if (!m_loopHeads.isEmpty())
        placeLoops();
#if ASSERT_ENABLED
    assertOrderHolds();
#endif
}

} // anonymous namespace

Order* orderBlocks(Procedure& procedure)
{
    BlockOrderBuilder builder(procedure);
    builder.run();

    Order* order = procedure.arena().create<Order>();
    order->m_blocksInOrder = builder.m_blocksInOrder;
    order->m_blocksInLayout = builder.m_blocksInLayout;
    order->m_places = builder.m_places;
    order->m_loops = builder.m_loops;
    order->m_rareRegions = builder.m_rareRegions;
    order->m_numberOfBlocksOutsideRareRegions = builder.m_numberOfBlocksOutsideRareRegions;
    order->m_end = builder.m_end;
    order->m_predecessors = builder.m_predecessors;
    order->m_successorIndexesOfPredecessors = builder.m_successorIndexesOfPredecessors;
    order->m_mostBlocks = procedure.numberOfBlocks() + builder.m_numberOfUnsplitEdges;
    order->m_mostOperations = procedure.numberOfOperations() + builder.m_numberOfUnsplitEdges;
    return order;
}

unsigned Order::successorIndexOfPredecessorWithManySuccessors(const Block* target, Block* const* entry) const
{
    if (!of(target).wasSplitLate)
        return m_successorIndexesOfPredecessors[entry - m_predecessors.data()];
    unsigned successorIndex = 0;
    while ((*entry)->successor(successorIndex).block() != target)
        ++successorIndex;
    return successorIndex;
}

Block* Order::splitLate(Procedure& procedure, Block* from, unsigned successorIndex)
{
    Order& order = *procedure.m_order;
    const Edge edge = from->successor(successorIndex);
    Block* target = edge.block();
    ASSERT(isUnsplit(from, order.of(target)) && !order.of(from).isRare() && target->phis().empty());

    Block* middle = split(from, successorIndex);
    RELEASE_ASSERT(middle->index() < order.m_mostBlocks && middle->last()->index() < order.m_mostOperations);
    // As where orderBlocks() splits such an edge.
    if (edge.isRare() && order.of(target).isRare()) {
        middle->clearSuccessors();
        middle->appendSuccessor(Edge(target, EdgeClass::Normal));
    }

    const BlockPlace& placeOfFrom = order.of(from);
    // orderBlocks() has kept the room and made no place in it.
    BlockPlace& place = *new (NotNull, &order.m_places[middle->index()]) BlockPlace();
    place.orderIndex = placeOfFrom.orderIndex;
    place.head = order.endOf(from) + positionsOfBlockSplitLate * successorIndex;
    place.loop = placeOfFrom.loop;
    place.wasSplitLate = true;
    if (edge.isRare())
        place.layoutPart = LayoutPart::Rare;
    else
        place.layoutPart = placeOfFrom.layoutPart == LayoutPart::Likely && edge.edgeClass() == EdgeClass::Normal ? LayoutPart::Likely : LayoutPart::Unlikely;
    std::span<Block*> predecessors = procedure.arena().createSpan<Block*>(1);
    predecessors[0] = from;
    place.predecessors = predecessors;

    // They stand as their blocks stand in the order, and what was split off late stands where its source stood.
    std::span<Block* const> predecessorsOfTarget = order.of(target).predecessors;
    auto entryOfFrom = std::ranges::lower_bound(predecessorsOfTarget, placeOfFrom.orderIndex, { }, [&](const Block* predecessor) {
        return order.of(predecessor).orderIndex;
    });
    RELEASE_ASSERT(entryOfFrom != predecessorsOfTarget.end() && *entryOfFrom == from);
    const_cast<Block*&>(*entryOfFrom) = middle;

    order.m_blocksSplitLate.append(procedure.arena(), middle);
    return middle;
}

void Order::placeBlocksSplitLate(Procedure& procedure)
{
    Order& order = *procedure.m_order;
    if (order.m_blocksSplitLate.isEmpty())
        return;
    Arena& arena = procedure.arena();
    BlockPlace* const places = order.m_places.begin();
    auto placeOf = [&](const Block* block) ALWAYS_INLINE_LAMBDA -> BlockPlace& {
        return places[block->index()];
    };
    auto placeOfSourceOf = [&](const Block* block) ALWAYS_INLINE_LAMBDA -> BlockPlace& {
        return placeOf(*placeOf(block).predecessors.data());
    };
    auto orderIndexOf = [&](const Block* block) {
        return placeOf(block).orderIndex;
    };
    auto layoutIndexOfSourceOf = [&](const Block* block) {
        return placeOfSourceOf(block).layoutIndex;
    };

    std::span<Block*> newBlocks = order.m_blocksSplitLate.span();
    Block* const* const firstNewBlock = newBlocks.data();
    Block* const* const endOfNewBlocks = firstNewBlock + newBlocks.size();
    // A stable sort leaves blocks whose keys never fall as they stand.
    auto sortNewBlocksBy = [&](const auto& keyOf) {
        if (!std::ranges::is_sorted(newBlocks, { }, keyOf))
            std::ranges::stable_sort(newBlocks, { }, keyOf);
    };

    // So far each has counted as standing where the block that leads to it does.
    sortNewBlocksBy(orderIndexOf);

    uint32_t numberOfBlocksSoFar = order.m_blocksInOrder.size();
    uint32_t numberOfBlocks = numberOfBlocksSoFar + static_cast<uint32_t>(newBlocks.size());

    // A block moves back by the new blocks whose sources stand ahead of it. A loop takes in what stands behind its last block.
    auto newIndexOf = [&](uint32_t index) {
        return index + static_cast<uint32_t>(std::ranges::lower_bound(newBlocks, index, { }, orderIndexOf) - newBlocks.begin());
    };
    for (Loop& loop : order.m_loops) {
        loop.firstBlock = newIndexOf(loop.firstBlock);
        loop.endBlock = newIndexOf(loop.endBlock);
    }

    // Each block is told its index as it is put in its place. The index that a new block has counted by is read before that.
    ArenaVector<Block*> listOfBlocksInOrder;
    listOfBlocksInOrder.growWithoutInitializing(arena, numberOfBlocks);
    Block** const blocksInOrder = listOfBlocksInOrder.begin();
    Block* const* const blocksInOrderSoFar = order.m_blocksInOrder.begin();
    Block* const* nextNewBlock = firstNewBlock;
    uint32_t orderIndexOfNextSource = orderIndexOf(*nextNewBlock);
    RELEASE_ASSERT(orderIndexOfNextSource < numberOfBlocksSoFar);
    // As far as the first source every block stays where it stands.
    std::copy_n(blocksInOrderSoFar, orderIndexOfNextSource + 1, blocksInOrder);
    uint32_t newOrderIndex = orderIndexOfNextSource + 1;
    for (uint32_t index = orderIndexOfNextSource; ;) {
        while (orderIndexOfNextSource == index) {
            Block* newBlock = *nextNewBlock++;
            placeOf(newBlock).orderIndex = newOrderIndex;
            blocksInOrder[newOrderIndex++] = newBlock;
            orderIndexOfNextSource = nextNewBlock != endOfNewBlocks ? orderIndexOf(*nextNewBlock) : Block::notOrdered;
        }
        if (++index == numberOfBlocksSoFar)
            break;
        Block* block = blocksInOrderSoFar[index];
        placeOf(block).orderIndex = newOrderIndex;
        blocksInOrder[newOrderIndex++] = block;
    }
    RELEASE_ASSERT(nextNewBlock == endOfNewBlocks);

    sortNewBlocksBy(layoutIndexOfSourceOf);
    // The sources are told their new indexes below, so where they have stood is kept here.
    uint32_t* const layoutIndexesOfSources = arena.allocateArray<uint32_t>(newBlocks.size());
    const uint32_t* const endOfLayoutIndexesOfSources = layoutIndexesOfSources + newBlocks.size();
    for (size_t index = 0; index < newBlocks.size(); ++index)
        layoutIndexesOfSources[index] = layoutIndexOfSourceOf(firstNewBlock[index]);

    ArenaVector<Block*> listOfBlocksInLayout;
    listOfBlocksInLayout.growWithoutInitializing(arena, numberOfBlocks);
    Block** const blocksInLayout = listOfBlocksInLayout.begin();
    Block* const* const blocksInLayoutSoFar = order.m_blocksInLayout.begin();
    uint32_t newLayoutIndex = 0;
    // Each block comes here once at most: one that was there as the list so far is gone through, a new one behind its source or at the end of its part.
    auto appendToLayout = [&](Block* block, BlockPlace& place) ALWAYS_INLINE_LAMBDA {
        place.layoutIndex = newLayoutIndex;
        blocksInLayout[newLayoutIndex++] = block;
    };
    bool likelyPartComesLast = procedure.traits().fallsIntoWhatLiesBehind;
    uint32_t numberOfBlocksInRarePart = 0;
    uint32_t indexInLayout = 0;
    nextNewBlock = firstNewBlock;
    const uint32_t* nextLayoutIndexOfSource = layoutIndexesOfSources;
    uint32_t layoutIndexOfNextSource = *nextLayoutIndexOfSource;
    auto standsBehindItsSource = [&](const Block* block) ALWAYS_INLINE_LAMBDA {
        return placeOf(block).layoutPart == placeOfSourceOf(block).layoutPart;
    };
    for (uint32_t step = 0; step < numberOfLayoutParts; ++step) {
        LayoutPart part = static_cast<LayoutPart>(likelyPartComesLast ? numberOfLayoutParts - 1 - step : step);
        uint32_t numberOfBlocksInFront = newLayoutIndex;
        for (; indexInLayout < numberOfBlocksSoFar; ++indexInLayout) {
            Block* block = blocksInLayoutSoFar[indexInLayout];
            BlockPlace& place = placeOf(block);
            if (place.layoutPart != part)
                break;
            appendToLayout(block, place);
            while (layoutIndexOfNextSource == indexInLayout) {
                Block* newBlock = *nextNewBlock++;
                if (standsBehindItsSource(newBlock))
                    appendToLayout(newBlock, placeOf(newBlock));
                layoutIndexOfNextSource = ++nextLayoutIndexOfSource != endOfLayoutIndexesOfSources ? *nextLayoutIndexOfSource : Block::notOrdered;
            }
        }
        for (Block* block : newBlocks) {
            if (placeOf(block).layoutPart == part && !standsBehindItsSource(block))
                appendToLayout(block, placeOf(block));
        }
        if (part == LayoutPart::Rare)
            numberOfBlocksInRarePart = newLayoutIndex - numberOfBlocksInFront;
    }
    RELEASE_ASSERT(newLayoutIndex == numberOfBlocks);

    order.m_blocksInOrder = listOfBlocksInOrder;
    order.m_blocksInLayout = listOfBlocksInLayout;
    order.m_numberOfBlocksOutsideRareRegions = numberOfBlocks - numberOfBlocksInRarePart;
    order.m_blocksSplitLate.clear();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
