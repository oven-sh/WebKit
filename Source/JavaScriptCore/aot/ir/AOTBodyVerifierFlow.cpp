/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTTypeNarrowing.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class SubjectsOfProofsInVerifiedBody {
public:
    SubjectsOfProofsInVerifiedBody(const BodyIndex& index, const BodyTerminator& terminator)
        : m_terminator(terminator)
    {
        if (terminator.isConditionalBranch())
            m_test = index.value(terminator.as<TerminatorKind::Branch>().conditionOperand());
        m_provesSomething = (m_test ? provesOf(m_test.opcode()) : provesOf(terminator.kind())) != Proof::None;
    }

    ValueID named(uint32_t proofWord) const
    {
        if (!m_provesSomething)
            return { };
        return m_test ? m_test.operand(proofWord) : m_terminator.operand(proofWord);
    }

private:
    BodyTerminator m_terminator;
    BodyValue m_test;
    bool m_provesSomething { false };
};

} // anonymous namespace

void FlowGraphOfVerifiedBody::build(const BodyIndex& index)
{
    uint32_t numberOfBlocks = index.numberOfBlocks();
    m_successors.shrink(0);
    m_firstSuccessor.shrink(0);
    m_firstSuccessor.reserveCapacity(numberOfBlocks + 1);
    m_hasRetreatingEdge = false;
    for (uint32_t block = 0; block < numberOfBlocks; ++block) {
        m_firstSuccessor.append(static_cast<uint32_t>(m_successors.size()));
        index.block(BlockID(block)).forEachOutgoingEdge([&](BodyTarget target, bool) {
            m_successors.append(target.block());
            m_hasRetreatingEdge |= target.block().index() <= block;
        });
    }
    m_firstSuccessor.append(static_cast<uint32_t>(m_successors.size()));

    m_firstPredecessor.fill(0, numberOfBlocks + 1);
    for (BlockID successor : m_successors)
        ++m_firstPredecessor[successor.index() + 1];
    for (uint32_t block = 0; block < numberOfBlocks; ++block)
        m_firstPredecessor[block + 1] += m_firstPredecessor[block];
    m_predecessors.fill(BlockID(), m_successors.size());
    m_nextPredecessor.shrink(0);
    m_nextPredecessor.append(m_firstPredecessor.span());
    for (uint32_t block = 0; block < numberOfBlocks; ++block) {
        for (BlockID successor : successors(BlockID(block)))
            m_predecessors[m_nextPredecessor[successor.index()]++] = BlockID(block);
    }
}

namespace {

struct DifferenceInBodyIndex {
    ASCIILiteral part;
    BlockID block;
    ValueID value;
    uint32_t indexed { 0 };
    uint32_t plain { 0 };
};

class BodyIndexComparison {
    WTF_MAKE_NONCOPYABLE(BodyIndexComparison);
public:
    BodyIndexComparison(const BodyIndex& index, BodyVerifierScratch& scratch)
        : m_index(index)
        , m_scratch(scratch)
    {
    }

    const std::optional<DifferenceInBodyIndex>& difference() const { return m_difference; }

    void comparePlaces();
    void compareUses();
    void compareIncomingEdges(const FlowGraphOfVerifiedBody&);
    void compareDominators(const FlowGraphOfVerifiedBody&);
    void compareOnlyIncomingEdges(const FlowGraphOfVerifiedBody&);
    void compareLoops(const FlowGraphOfVerifiedBody&);

private:
    static constexpr uint32_t nothing = UINT32_MAX;
    static_assert(LoopID().bits() == nothing && BlockID().bits() == nothing && ValueID().bits() == nothing);

    bool hasDifference() const { return m_difference.has_value(); }
    ALWAYS_INLINE void compareAnswers(ASCIILiteral part, BlockID block, ValueID value, uint64_t indexed, uint64_t plain)
    {
        if (indexed != plain) [[unlikely]]
            noteDifference(part, block, value, indexed, plain);
    }
    NEVER_INLINE void noteDifference(ASCIILiteral part, BlockID block, ValueID value, uint64_t indexed, uint64_t plain)
    {
        if (!m_difference)
            m_difference = DifferenceInBodyIndex { part, block, value, static_cast<uint32_t>(indexed), static_cast<uint32_t>(plain) };
    }

    const BodyIndex& m_index;
    BodyVerifierScratch& m_scratch;
    std::optional<DifferenceInBodyIndex> m_difference;
};

void BodyIndexComparison::comparePlaces()
{
    const Body& body = m_index.body();
    const UnitIDRemap* unit = &body.unit();
    auto offsetOf = [start = body.words().data()](const uint32_t* words) {
        return static_cast<uint64_t>(words - start);
    };


    OpcodeSet opcodes;
    BodyBlock block = body.entry();
    for (uint32_t blockIndex = 0; blockIndex < body.numberOfBlocks(); ++blockIndex) {
        BlockID id(blockIndex);
        auto compareValue = [&](const BodyValue& value) {

            compareAnswers("the place of the value"_s, id, value.id(), offsetOf(m_index.value(value.id()).words()), offsetOf(value.words()));
            compareAnswers("whether the value is a parameter"_s, id, value.id(), m_index.isParameter(value.id()), value.isParameter());
        };

        BodyBlock indexed = m_index.block(id);
        compareAnswers("the place of the block"_s, id, { }, offsetOf(indexed.words()), offsetOf(block.words()));
        compareAnswers("the first value of the block"_s, id, { }, indexed.firstParameter().bits(), block.firstParameter().bits());
        for (uint32_t parameter = 0; parameter < block.numberOfParameters(); ++parameter)
            compareValue(block.parameterValue(parameter));
        BodyValues::Iterator cursor = block.values().begin();
        for (uint32_t remaining = block.numberOfValues(); remaining--; ++cursor) {
            BodyValue value = *cursor;
            opcodes.add(value.opcode());
            compareValue(value);
        }
        BodyTerminator terminator(cursor.words(), unit, id);
        compareAnswers("the place of the terminator"_s, id, { }, offsetOf(indexed.terminator().words()), offsetOf(terminator.words()));

        if (block.firstParameter() != block.endOfValues()) {
            ValueID last(block.endOfValues().index() - 1);
            compareAnswers("the block of the value"_s, id, block.firstParameter(), m_index.blockOf(block.firstParameter()).bits(), id.bits());
            compareAnswers("the block of the value"_s, id, last, m_index.blockOf(last).bits(), id.bits());
        }
        block = BodyBlock(terminator.end(), nullptr, unit, BlockID(blockIndex + 1), block.endOfValues());
    }

    constexpr OpcodeSet opcodeOfParameters { Opcode::Parameter };
    compareAnswers("the set of opcodes"_s, entryBlock, { }, (m_index.opcodes() | opcodeOfParameters) == (opcodes | opcodeOfParameters), true);
}

void BodyIndexComparison::compareUses()
{
    if (hasDifference())
        return;

    Vector<uint32_t>& counts = m_scratch.numbersOfUses;
    counts.fill(0, m_index.numberOfValues());
    Vector<BodyUser>& firstUsers = m_scratch.firstUsers;
    firstUsers.grow(std::max<size_t>(firstUsers.size(), m_index.numberOfValues()));
    auto mention = [&](ValueID used, BodyUseKind kind, ValueID user, BlockID block) {
        if (!counts[used.index()]++)
            firstUsers[used.index()] = BodyUser { kind, user, block };
    };

    for (uint32_t blockIndex = 0; blockIndex < m_index.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = m_index.block(BlockID(blockIndex));
        if (ValueID subject = block.subject())
            mention(subject, BodyUseKind::BlockSubject, { }, block.id());
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument, BodyUseKind::HandlerArgument, { }, block.id());
        }
        for (BodyValue value : block.values()) {
            value.forEachOperand([&](ValueID operand, uint32_t) {
                mention(operand, BodyUseKind::Operand, value.id(), { });
            });
        }

        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            mention(operand, BodyUseKind::TerminatorOperand, { }, block.id());
        });
        ValueID test = terminator.isConditionalBranch() ? terminator.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
        SubjectsOfProofsInVerifiedBody subjects(m_index, terminator);
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument, BodyUseKind::EdgeArgument, { }, block.id());

            std::span<const uint32_t> proofs = target.proofs();
            uint32_t firstParameterOfProofs = m_index.block(target.block()).firstParameter().index() + static_cast<uint32_t>(target.arguments().size());
            for (uint32_t proofIndex = 0; proofIndex < proofs.size(); ++proofIndex) {
                ValueID subject = subjects.named(proofs[proofIndex]);
                std::optional<BodyEdgeResult> result = m_index.edgeResult(terminator, targetIndex, target, proofIndex);
                compareAnswers("whether the proof names an operand"_s, block.id(), { }, result.has_value(), !!subject);
                if (hasDifference())
                    return;
                if (!subject)
                    continue;
                mention(subject, BodyUseKind::EdgeProof, { }, block.id());
                compareAnswers("the operand that the proof names"_s, block.id(), subject, result->subject.bits(), subject.bits());
                compareAnswers("the index of the operand that the proof names"_s, block.id(), subject, result->operandIndex, proofs[proofIndex]);
                compareAnswers("the test behind the proof"_s, block.id(), subject, result->test.bits(), test.bits());
                compareAnswers("the parameter that takes the proof"_s, block.id(), subject, result->parameter.bits(), firstParameterOfProofs + proofIndex);
            }
            ++targetIndex;
        }
    }

    for (uint32_t blockIndex = 0; blockIndex < m_index.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = m_index.block(BlockID(blockIndex));
        for (uint32_t valueIndex = block.firstParameter().index(); valueIndex < block.endOfValues().index(); ++valueIndex) {
            ValueID value(valueIndex);
            compareAnswers("the number of uses"_s, block.id(), value, m_index.useCount(value), std::min(counts[valueIndex], BodyIndex::maxCountedUses));
            if (hasDifference())
                return;
            if (counts[valueIndex] != 1)
                continue;
            BodyUser indexed = *m_index.onlyUser(value);
            const BodyUser& plain = firstUsers[valueIndex];
            compareAnswers("the kind of the only use"_s, block.id(), value, static_cast<uint32_t>(indexed.kind), static_cast<uint32_t>(plain.kind));
            compareAnswers("the value that is the only user"_s, block.id(), value, indexed.value.bits(), plain.value.bits());
            compareAnswers("the block of the only use"_s, block.id(), value, indexed.block.bits(), plain.block.bits());
        }
    }
}

void BodyIndexComparison::compareIncomingEdges(const FlowGraphOfVerifiedBody& graph)
{
    if (hasDifference())
        return;

    BitVector isEnteredByHandlerEdge(graph.numberOfBlocks());
    for (uint32_t blockIndex = 0; blockIndex < graph.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = m_index.block(BlockID(blockIndex));
        if (block.hasHandler())
            isEnteredByHandlerEdge.quickSet(block.handler().block().index());
    }
    for (uint32_t blockIndex = 0; blockIndex < graph.numberOfBlocks(); ++blockIndex) {
        BlockID block(blockIndex);
        compareAnswers("whether the block is reached"_s, block, { }, m_index.isReachable(block), true);
        compareAnswers("the number of incoming edges"_s, block, { }, m_index.numberOfIncomingEdges(block), std::min<size_t>(graph.predecessors(block).size(), BodyIndex::maxCountedIncomingEdges));
        compareAnswers("whether a handler edge enters the block"_s, block, { }, m_index.isEnteredByHandlerEdge(block), isEnteredByHandlerEdge.quickGet(blockIndex));
    }
}

void BodyIndexComparison::compareDominators(const FlowGraphOfVerifiedBody& graph)
{
    if (hasDifference())
        return;

    uint32_t numberOfBlocks = graph.numberOfBlocks();
    Vector<BlockID>& dominatorOf = m_scratch.dominators;
    dominatorOf.fill(BlockID(), numberOfBlocks);
    Vector<uint32_t>& lastRoundThatClimbedPast = m_scratch.lastRoundsThatClimbedPast;
    lastRoundThatClimbedPast.fill(0, numberOfBlocks);
    uint32_t currentRound = 0;
    bool didChange;
    do {
        didChange = false;
        for (uint32_t block = 1; block < numberOfBlocks; ++block) {
            ++currentRound;
            BlockID common;
            for (BlockID predecessor : graph.predecessors(BlockID(block))) {
                if (predecessor != entryBlock && !dominatorOf[predecessor.index()])
                    continue;
                if (!common) {
                    common = predecessor;
                    continue;
                }
                BlockID other = predecessor;
                while (other != common) {
                    BlockID& later = other.index() > common.index() ? other : common;
                    if (std::exchange(lastRoundThatClimbedPast[later.index()], currentRound) == currentRound)
                        break;
                    later = dominatorOf[later.index()];
                }
            }
            if (common == dominatorOf[block])
                continue;
            dominatorOf[block] = common;
            didChange = true;
        }
    } while (didChange && graph.hasRetreatingEdge());

    for (uint32_t block = 0; block < numberOfBlocks; ++block)
        compareAnswers("the immediate dominator"_s, BlockID(block), { }, m_index.immediateDominator(BlockID(block)).bits(), dominatorOf[block].bits());
    if (hasDifference())
        return;

    Vector<uint32_t>& firstChild = m_scratch.firstChildren;
    firstChild.fill(0, numberOfBlocks + 1);
    for (uint32_t block = 1; block < numberOfBlocks; ++block)
        ++firstChild[dominatorOf[block].index() + 1];
    for (uint32_t block = 0; block < numberOfBlocks; ++block)
        firstChild[block + 1] += firstChild[block];
    Vector<BlockID>& children = m_scratch.children;
    children.fill(BlockID(), numberOfBlocks - 1);
    Vector<uint32_t>& nextChild = m_scratch.nextChildren;
    auto startAtFirstChildren = [&] {
        nextChild.shrink(0);
        nextChild.append(firstChild.span());
    };
    startAtFirstChildren();
    for (uint32_t block = 1; block < numberOfBlocks; ++block)
        children[nextChild[dominatorOf[block].index()]++] = BlockID(block);

    Vector<BlockID>& order = m_scratch.blocksInDominatorOrder;
    order.shrink(0);
    Vector<uint32_t>& lastNumberBelow = m_scratch.lastNumbersBelow;
    lastNumberBelow.fill(0, numberOfBlocks);
    Vector<BlockID>& path = m_scratch.path;
    path.shrink(0);
    startAtFirstChildren();
    order.append(entryBlock);
    path.append(entryBlock);
    while (!path.isEmpty()) {
        BlockID block = path.last();
        if (nextChild[block.index()] == firstChild[block.index() + 1]) {
            lastNumberBelow[block.index()] = static_cast<uint32_t>(order.size()) - 1;
            path.removeLast();
            continue;
        }
        BlockID child = children[nextChild[block.index()]++];
        order.append(child);
        path.append(child);
    }

    std::span<const BlockID> indexedOrder = m_index.blocksInDominatorOrder();
    compareAnswers("the number of blocks in dominator order"_s, entryBlock, { }, indexedOrder.size(), order.size());
    if (hasDifference())
        return;
    for (uint32_t number = 0; number < order.size(); ++number) {
        BlockID block = order[number];
        compareAnswers("the block at this place in dominator order"_s, block, { }, indexedOrder[number].bits(), block.bits());
        compareAnswers("the number of the block in dominator order"_s, block, { }, m_index.dominatorNumber(block), number);
        compareAnswers("the last number in dominator order below the block"_s, block, { }, m_index.lastDominatorNumberBelow(block), lastNumberBelow[block.index()]);
    }
}

void BodyIndexComparison::compareOnlyIncomingEdges(const FlowGraphOfVerifiedBody& graph)
{
    if (hasDifference())
        return;

    auto isEnteredByOneEdgeOfTerminator = [&](BlockID block) {
        return graph.predecessors(block).size() == 1 && !m_index.isEnteredByHandlerEdge(block);
    };
    for (uint32_t blockIndex = 0; blockIndex < graph.numberOfBlocks(); ++blockIndex) {
        BlockID block(blockIndex);
        compareAnswers("whether the block has an only incoming edge"_s, block, { }, m_index.onlyIncomingEdge(block).has_value(), isEnteredByOneEdgeOfTerminator(block));
    }
    if (hasDifference())
        return;

    auto placeOf = [start = m_index.body().words().data()](BodyTarget target) {
        return static_cast<uint64_t>(reinterpret_cast<const uint32_t*>(target.arguments().data()) - start);
    };
    for (uint32_t blockIndex = 0; blockIndex < graph.numberOfBlocks(); ++blockIndex) {
        BlockID from(blockIndex);
        uint32_t targetIndex = 0;
        for (BodyTarget target : m_index.terminator(from).targets()) {
            BlockID to = target.block();
            if (isEnteredByOneEdgeOfTerminator(to)) {
                BodyIncomingEdge indexed = *m_index.onlyIncomingEdge(to);
                compareAnswers("the block that the only incoming edge leaves"_s, to, { }, indexed.from.bits(), from.bits());
                compareAnswers("the index of the only incoming edge among the targets"_s, to, { }, indexed.targetIndex, targetIndex);
                compareAnswers("the place of the only incoming edge"_s, to, { }, placeOf(indexed.target), placeOf(target));
            }
            ++targetIndex;
        }
    }
}

void BodyIndexComparison::compareLoops(const FlowGraphOfVerifiedBody& graph)
{
    if (hasDifference())
        return;

    struct Loop {
        BlockID head;
        BlockID last;
        uint32_t parent { nothing };
        uint32_t depth { 0 };
    };
    uint32_t numberOfBlocks = graph.numberOfBlocks();
    if (!graph.hasRetreatingEdge()) {
        compareAnswers("whether the flow is irreducible"_s, entryBlock, { }, m_index.hasIrreducibleFlow(), false);
        compareAnswers("the number of loops"_s, entryBlock, { }, m_index.numberOfLoops(), 0);
        for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            BlockID block(blockIndex);
            compareAnswers("the innermost loop of the block"_s, block, { }, m_index.loopOf(block).bits(), nothing);
            compareAnswers("whether the block is the head of a loop"_s, block, { }, m_index.isLoopHead(block), false);
        }
        return;
    }

    Vector<Loop> loops;
    Vector<uint32_t> loopOf;
    loopOf.fill(nothing, numberOfBlocks);
    Vector<BlockID> keptSuccessors;
    Vector<uint32_t> firstKeptSuccessor;
    bool hasIrreducibleFlow = false;
    for (uint32_t depth = 1; !hasIrreducibleFlow; ++depth) {
        keptSuccessors.shrink(0);
        firstKeptSuccessor.shrink(0);
        for (uint32_t block = 0; block < numberOfBlocks; ++block) {
            firstKeptSuccessor.append(static_cast<uint32_t>(keptSuccessors.size()));
            uint32_t loop = loopOf[block];
            if (depth > 1 && (loop == nothing || loops[loop].depth != depth - 1))
                continue;
            for (BlockID successor : graph.successors(BlockID(block))) {
                if (depth == 1 || (loopOf[successor.index()] == loop && successor != loops[loop].head))
                    keptSuccessors.append(successor);
            }
        }
        firstKeptSuccessor.append(static_cast<uint32_t>(keptSuccessors.size()));

        StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfBlocks, [&](unsigned block) -> std::span<const BlockID> {
            return keptSuccessors.span().subspan(firstKeptSuccessor[block], firstKeptSuccessor[block + 1] - firstKeptSuccessor[block]);
        });
        if (components.isInCycle.isEmpty())
            break;

        for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
            std::span<const uint32_t> members = components.membersOf(component);
            if (!components.isInCycle.quickGet(members.front()))
                continue;
            auto [first, last] = std::ranges::minmax(members);
            uint32_t loop = static_cast<uint32_t>(loops.size());
            loops.append(Loop { BlockID(first), BlockID(last), loopOf[first], depth });
            for (uint32_t member : members)
                loopOf[member] = loop;
        }

        for (uint32_t block = 0; block < numberOfBlocks; ++block) {
            uint32_t loop = loopOf[block];
            if (loop == nothing || loops[loop].depth != depth || loops[loop].head.index() == block)
                continue;
            for (BlockID predecessor : graph.predecessors(BlockID(block)))
                hasIrreducibleFlow |= loopOf[predecessor.index()] != loop;
        }
    }

    if (hasIrreducibleFlow) {
        loops.shrink(0);
        loops.append(Loop { entryBlock, BlockID(numberOfBlocks - 1), nothing, 1 });
        loopOf.fill(0, numberOfBlocks);
    }

    Vector<uint32_t> loopWithHead;
    loopWithHead.fill(nothing, numberOfBlocks);
    for (uint32_t loop = 0; loop < loops.size(); ++loop)
        loopWithHead[loops[loop].head.index()] = loop;
    Vector<uint32_t> numbersInOrderOfHeads;
    numbersInOrderOfHeads.fill(nothing, loops.size());
    uint32_t numberOfHeads = 0;
    for (uint32_t block = 0; block < numberOfBlocks; ++block) {
        if (loopWithHead[block] != nothing)
            numbersInOrderOfHeads[loopWithHead[block]] = numberOfHeads++;
    }
    auto numberOf = [&](uint32_t loop) {
        return loop == nothing ? nothing : numbersInOrderOfHeads[loop];
    };

    compareAnswers("whether the flow is irreducible"_s, entryBlock, { }, m_index.hasIrreducibleFlow(), hasIrreducibleFlow);
    compareAnswers("the number of loops"_s, entryBlock, { }, m_index.numberOfLoops(), loops.size());
    if (hasDifference())
        return;
    for (uint32_t loop = 0; loop < loops.size(); ++loop) {
        const Loop& plain = loops[loop];
        const BodyLoop& indexed = m_index.loop(LoopID(numberOf(loop)));
        compareAnswers("the head of the loop"_s, plain.head, { }, indexed.head.bits(), plain.head.bits());
        compareAnswers("the last block of the loop"_s, plain.head, { }, indexed.last.bits(), plain.last.bits());
        compareAnswers("the loop around the loop"_s, plain.head, { }, indexed.parent.bits(), numberOf(plain.parent));
        compareAnswers("the depth of the loop"_s, plain.head, { }, indexed.depth, plain.depth);
    }
    Vector<uint32_t> numbersOfMembersFound;
    numbersOfMembersFound.fill(0, loops.size());
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks && !hasDifference(); ++blockIndex) {
        for (uint32_t loop = loopOf[blockIndex]; loop != nothing; loop = loops[loop].parent) {
            std::span<const BlockID> members = m_index.membersOf(LoopID(numberOf(loop)));
            uint32_t place = numbersOfMembersFound[loop]++;
            compareAnswers("the next member of the loop"_s, loops[loop].head, { }, place < members.size() ? members[place].bits() : BlockID().bits(), BlockID(blockIndex).bits());
        }
    }
    for (uint32_t loop = 0; loop < loops.size() && !hasDifference(); ++loop)
        compareAnswers("the number of members of the loop"_s, loops[loop].head, { }, m_index.membersOf(LoopID(numberOf(loop))).size(), numbersOfMembersFound[loop]);
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        compareAnswers("the innermost loop of the block"_s, block, { }, m_index.loopOf(block).bits(), numberOf(loopOf[blockIndex]));
        compareAnswers("whether the block is the head of a loop"_s, block, { }, m_index.isLoopHead(block), loopWithHead[blockIndex] != nothing);
    }
}

} // anonymous namespace

static String describeDifferenceInBodyIndex(const DifferenceInBodyIndex& difference)
{
    auto textOf = [](uint32_t answer) {
        return answer == UINT32_MAX ? String("nothing"_s) : String::number(answer);
    };
    return makeString(difference.part, ": the index says "_s, textOf(difference.indexed), ", a plain computation gives "_s, textOf(difference.plain));
}

static void setPlaceOfErrorWithoutBodyIndex(InternalError& error, const Function& function, const Body& body)
{
    body.forEachBlock([&](const BodyBlock& block) {
        if (!error.value) {
            if (block.id() != error.block)
                return;
            BodyTerminator terminator = block.terminator();
            error.location = function.locationOf(terminator);
            error.operation = nameOf(terminator.kind());
            return;
        }

        if (!block.holds(error.value))
            return;
        BodyValue value;
        if (std::optional<uint32_t> parameterIndex = block.indexOfParameter(error.value))
            value = block.parameterValue(*parameterIndex);
        else {
            BodyValues::Iterator cursor = block.values().begin();
            for (uint32_t remaining = error.value.index() - block.firstValue().index(); remaining--;)
                ++cursor;
            value = *cursor;
        }
        error.location = function.locationOf(value);
        error.operation = nameOf(value.opcode());
    });
}

static uint32_t numberOfCasesListedInBody(const BodyTerminator& terminator)
{
    if (terminator.kind() == TerminatorKind::Switch)
        return terminator.as<TerminatorKind::Switch>().cases().size();
    RELEASE_ASSERT(terminator.kind() == TerminatorKind::SwitchString);
    return terminator.as<TerminatorKind::SwitchString>().strings().size();
}

static bool useInBodyMustNameDefinition(const BodyIndex& index, BodyUseKind kind, BlockID blockOfUse, ValueID user, uint32_t operandIndex)
{
    switch (kind) {
    case BodyUseKind::Operand:
        return mustBeDefinition(index.value(user).operandKind(operandIndex));
    case BodyUseKind::TerminatorOperand:
        return mustBeDefinition(index.terminator(blockOfUse).operandKind(operandIndex));
    case BodyUseKind::BlockSubject:
        return index.block(blockOfUse).role() == BlockRole::IterationCleanup;
    case BodyUseKind::EdgeArgument:
    case BodyUseKind::EdgeProof:
    case BodyUseKind::HandlerArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BodyVerifier::verifyFlow()
{
    if (!m_body)
        return;

    BodyIndexComparison places(index(), m_scratch);
    if (m_comparesIndexWithPlainComputation)
        places.comparePlaces();

    if (places.difference()) {
        fail(InternalErrorKind::BodyIndexMismatch, places.difference()->block);
        InternalError& error = m_errors.last();
        error.value = places.difference()->value;
        error.detail = describeDifferenceInBodyIndex(*places.difference());
        setPlaceOfErrorWithoutBodyIndex(error, function(), m_body);
        return;
    }

    using Step = void (BodyVerifier::*)();
    static constexpr Step steps[] = {
        &BodyVerifier::verifyEdges,
        &BodyVerifier::verifyBlockOrder,
        &BodyVerifier::verifyIndexAgainstPlainComputation,
        &BodyVerifier::verifyUsesAndOperations,
        &BodyVerifier::verifyLoopMarks,
        &BodyVerifier::verifyEntryParameters,
    };
    for (Step step : steps) {
        (this->*step)();
        if (hasFailed())
            return;
    }
}

const FlowGraphOfVerifiedBody& BodyVerifier::flowGraph()
{
    if (!m_hasFlowGraph) {
        m_scratch.graph.build(index());
        m_hasFlowGraph = true;
    }
    return m_scratch.graph;
}

void BodyVerifier::verifyBlockOrder()
{
    if (!index().hasIrreducibleFlow())
        return;

    const FlowGraphOfVerifiedBody& graph = flowGraph();
    uint32_t numberOfBlocks = graph.numberOfBlocks();
    for (uint32_t block = 1; block < numberOfBlocks; ++block) {
        std::span<const BlockID> predecessors = graph.predecessors(BlockID(block));
        if (predecessors.empty() || predecessors.front().index() >= block) {
            fail(InternalErrorKind::BlockOrderInvalid, BlockID(block));
            return;
        }
    }
    if (!graph.hasRetreatingEdge())
        return;

    Vector<uint32_t>& marks = m_scratch.lastLoopHeadsThatReach;
    marks.fill(UINT32_MAX, numberOfBlocks);
    Vector<BlockID>& worklist = m_scratch.worklist;
    worklist.shrink(0);
    Vector<uint32_t> numberBelowInTree;
    numberBelowInTree.fill(1, numberOfBlocks);
    for (uint32_t block = numberOfBlocks; --block;)
        numberBelowInTree[graph.predecessors(BlockID(block)).front().index()] += numberBelowInTree[block];
    Vector<uint32_t> placeInTree;
    placeInTree.fill(0, numberOfBlocks);
    Vector<uint32_t> nextPlaceBelow;
    nextPlaceBelow.fill(1, numberOfBlocks);
    for (uint32_t block = 1; block < numberOfBlocks; ++block) {
        uint32_t& next = nextPlaceBelow[graph.predecessors(BlockID(block)).front().index()];
        placeInTree[block] = next;
        next += numberBelowInTree[block];
        nextPlaceBelow[block] = placeInTree[block] + 1;
    }
    for (uint32_t head = 1; head < numberOfBlocks; ++head) {
        std::span<const BlockID> predecessors = graph.predecessors(BlockID(head));
        if (predecessors.back().index() <= head)
            continue;
        bool standsAboveEachSource = std::ranges::all_of(predecessors, [&](BlockID source) {
            return source.index() <= head || placeInTree[source.index()] - placeInTree[head] < numberBelowInTree[head];
        });
        if (standsAboveEachSource)
            continue;

        uint32_t leadsToSource = 2 * head;
        uint32_t isReachedFromHead = 2 * head + 1;
        auto noteThatLeadsToSource = [&](BlockID block) {
            if (block.index() > head && std::exchange(marks[block.index()], leadsToSource) != leadsToSource)
                worklist.append(block);
        };
        for (BlockID source : predecessors)
            noteThatLeadsToSource(source);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (BlockID predecessor : graph.predecessors(block)) {
                if (predecessor.index() < block.index())
                    noteThatLeadsToSource(predecessor);
            }
        }

        worklist.append(BlockID(head));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            for (BlockID successor : graph.successors(block)) {
                if (successor.index() <= block.index() || marks[successor.index()] != leadsToSource)
                    continue;
                marks[successor.index()] = isReachedFromHead;
                worklist.append(successor);
            }
        }
        for (BlockID source : predecessors) {
            if (source.index() > head && marks[source.index()] != isReachedFromHead) {
                fail(InternalErrorKind::BlockOrderInvalid, source);
                return;
            }
        }
    }
}

void BodyVerifier::verifyDominance()
{
    for (uint32_t blockIndex = 0; blockIndex < index().numberOfBlocks() && !hasFailed(); ++blockIndex) {
        BodyBlock block = index().block(BlockID(blockIndex));
        BlockID id = block.id();
        if (ValueID subject = block.subject())
            verifyUseStepByStep(subject, BodyUseKind::BlockSubject, id, { }, 0);
        if (block.hasHandler()) {
            uint32_t argumentIndex = 0;
            for (ValueID argument : block.handler().arguments())
                verifyUseStepByStep(argument, BodyUseKind::HandlerArgument, id, { }, argumentIndex++);
        }
        for (BodyValue value : block.values()) {
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                verifyUseStepByStep(operand, BodyUseKind::Operand, id, value.id(), operandIndex);
            });
        }
        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            verifyUseStepByStep(operand, BodyUseKind::TerminatorOperand, id, { }, operandIndex);
        });
        for (BodyTarget target : terminator.targets()) {
            uint32_t argumentIndex = 0;
            for (ValueID argument : target.arguments())
                verifyUseStepByStep(argument, BodyUseKind::EdgeArgument, id, { }, argumentIndex++);
        }
        index().forEachEdgeResult(id, [&](const BodyEdgeResult& result) {
            verifyUseStepByStep(result.subject, BodyUseKind::EdgeProof, id, { }, result.operandIndex);
        });
    }
}

static ALWAYS_INLINE bool hasOnePositionInVerifiedBody(const BodyValue& value)
{
    return !(value.words()[0] & Wire::WrittenPositionFollows) && value.hasPosition();
}

void BodyVerifier::verifyOperandsStepByStep(const BodyBlock& block, const BodyValue& value)
{
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        verifyUseStepByStep(operand, BodyUseKind::Operand, block.id(), value.id(), operandIndex);
    });
    if (!hasFailed())
        verifyOperandDefinitionsOneByOne(block, value);
}

bool BodyVerifier::givesOneValue(ValueID used) const
{
    BodyValue definition = index().value(used);
    return !opcodesWithoutResultInVerifiedBodies.contains(definition.opcode()) && !isTakenByFewOperands(definition);
}

bool BodyVerifier::isAvailableInOtherBlock(NoteOnVerifiedValue definition, BlockID blockOfUse)
{
    BlockID blockOfDefinition = definition.block();
    if (blockOfDefinition == blockOfUse || !index().dominates(blockOfDefinition, blockOfUse))
        return false;
    return definition.isParameter() || !index().block(blockOfDefinition).hasHandler() || terminatorDominates(blockOfDefinition, blockOfUse);
}

void BodyVerifier::lookAtUsesAndOperations()
{
    using Questions = QuestionsOfVerifiedOperation;
    static_assert(!hasFieldRulesInEveryFunction(TerminatorKind::Return) && !isIntrinsicOf(TerminatorKind::Return));

    const BodyIndex& bodyIndex = index();
    const UnitIDRemap* unit = &m_body.unit();
    const NoteOnVerifiedValue* notes = m_scratch.notesOnValues.span().data();
    const ValueID* subjectOfNextProof = m_scratch.subjectsOfProofs.span().data();
    uint8_t questionsInFunction = UINT8_MAX;
    if (function().role != FunctionRole::Source)
        questionsInFunction &= ~Questions::libraryCode;
    bool returnsOneValue = function().isCoroutine() || function().results.size() == 1;

    for (uint32_t blockIndex = 0; blockIndex < bodyIndex.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = bodyIndex.block(BlockID(blockIndex));
        BlockID id = block.id();
        uint32_t firstOfBlock = block.firstParameter().index();
        uint32_t firstValue = block.firstValue().index();
        uint32_t endOfValues = block.endOfValues().index();

        auto isPlainUse = [&](ValueID used, uint32_t end) ALWAYS_INLINE_LAMBDA {
            NoteOnVerifiedValue definition = notes[used.index()];
            if (definition.mayBeTakenByFewUses() && !givesOneValue(used)) [[unlikely]]
                return false;
            return used.index() - firstOfBlock < end - firstOfBlock || isAvailableInOtherBlock(definition, id);
        };

        if (ValueID subject = block.subject()) {
            verifyUseStepByStep(subject, BodyUseKind::BlockSubject, id, { }, 0);
            if (hasFailed())
                return;
        }
        bool usesOfEdgesArePlain = true;
        if (block.hasHandler()) {
            uint32_t argumentIndex = 0;
            for (ValueID argument : block.handler().arguments()) {
                if (!isPlainUse(argument, firstValue)) {
                    usesOfEdgesArePlain = false;
                    verifyUseStepByStep(argument, BodyUseKind::HandlerArgument, id, { }, argumentIndex);
                }
                ++argumentIndex;
            }
            if (!usesOfEdgesArePlain && hasFailed())
                return;
        }

        for (uint32_t valueIndex = firstValue; valueIndex < endOfValues; ++valueIndex) {
            ValueID valueID(valueIndex);
            BodyValue value = BodyValue::operation(bodyIndex.value(valueID).words(), unit, valueID);
            uint8_t questions = CaseTable<Opcode, questionsOfVerifiedOperation>::at(value.opcode()) & questionsInFunction;
            bool operandsArePlain = !(questions & Questions::kindsOfOperands);
            value.forEachOperand([&](ValueID operand, uint32_t) ALWAYS_INLINE_LAMBDA {
                operandsArePlain &= isPlainUse(operand, valueIndex);
            });
            if (!operandsArePlain) [[unlikely]] {
                verifyOperandsStepByStep(block, value);
                if (hasFailed())
                    return;
            }
            if (!hasOnePositionInVerifiedBody(value)) [[unlikely]] {
                verifyPositions(block, value);
                if (hasFailed())
                    return;
            }
            if (questions & (Questions::fieldRules | Questions::libraryCode)) {
                verifyFieldRules(*this, block, value);
                if (hasFailed())
                    return;
            }
            if (questions & Questions::convertedAllocation) {
                if (bodyIndex.opcodeOf(value.as<Opcode::Convert>().valueOperand()) != Opcode::NewObject) [[likely]]
                    continue;
                verifyConvertedAllocation(block, value);
                if (hasFailed())
                    return;
            }
            if (questions & Questions::ruleOfOpcode) {
                verifyRuleOfOpcode(block, valueIndex - firstValue, value);
                if (hasFailed())
                    return;
            }
        }

        BodyTerminator terminator = block.terminator();
        TerminatorKind kind = terminator.kind();
        bool takesAnyValue = terminatorsThatTakeAnyValueInVerifiedBodies.contains(kind);
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (isPlainUse(operand, endOfValues) && (takesAnyValue || !notes[operand.index()].isParameter())) [[likely]]
                return;
            usesOfEdgesArePlain = false;
            verifyUseStepByStep(operand, BodyUseKind::TerminatorOperand, id, { }, operandIndex);
        });
        for (BodyTarget target : terminator.targets()) {
            uint32_t argumentIndex = 0;
            for (ValueID argument : target.arguments()) {
                if (!isPlainUse(argument, endOfValues)) {
                    usesOfEdgesArePlain = false;
                    verifyUseStepByStep(argument, BodyUseKind::EdgeArgument, id, { }, argumentIndex);
                }
                ++argumentIndex;
            }
            for (uint32_t operandIndex : target.proofs()) {
                ValueID subject = *subjectOfNextProof++;
                if (!isPlainUse(subject, endOfValues)) {
                    usesOfEdgesArePlain = false;
                    verifyUseStepByStep(subject, BodyUseKind::EdgeProof, id, { }, operandIndex);
                }
            }
        }

        bool isAskedOnlyForUses = terminatorsAskedOnlyForTheirUsesInVerifiedBodies.contains(kind)
            || (kind == TerminatorKind::Return && returnsOneValue && terminator.as<TerminatorKind::Return>().valueOperand());
        if (usesOfEdgesArePlain && isAskedOnlyForUses) [[likely]]
            continue;
        if (!hasFailed())
            verifyTerminatorOperation(block, terminator);
        if (hasFailed())
            return;
    }
}

void BodyVerifier::verifyUsesAndOperations()
{
    lookAtUsesAndOperations();
    m_operationsAreVerified = !hasFailed();
    if (m_operationsAreVerified)
        return;

    m_errors.clear();
    m_initialYield = { };
    m_yields.clear();
    m_methodDefinitions.clear();
    m_moduleEvaluations.clear();
    verifyDominance();
}

static BitVector findBlocksReachedFromHandlerOfDominator(const BodyIndex& index)
{
    struct EdgeBetweenSiblings {
        BlockID from;
        BlockID to;
    };
    uint32_t numberOfBlocks = index.numberOfBlocks();
    BitVector isReached(numberOfBlocks);
    Vector<BlockID, 16> worklist;
    Vector<EdgeBetweenSiblings> edges;
    Vector<uint32_t> depthOf;
    depthOf.fill(0, numberOfBlocks);
    Vector<BlockID> path;
    for (BlockID block : index.blocksInDominatorOrder()) {
        uint32_t depth = block == entryBlock ? 0 : depthOf[index.immediateDominator(block).index()] + 1;
        depthOf[block.index()] = depth;
        path.shrink(depth);
        path.append(block);
        index.block(block).forEachOutgoingEdge([&](BodyTarget target, bool isHandlerEdge) {
            BlockID successor = target.block();
            BlockID dominator = index.immediateDominator(successor);
            if (dominator != block)
                edges.append(EdgeBetweenSiblings { path[depthOf[dominator.index()] + 1], successor });
            else if (isHandlerEdge && !isReached.quickSet(successor.index()))
                worklist.append(successor);
        });
    }

    Vector<uint32_t> firstEdgeFrom;
    firstEdgeFrom.fill(0, numberOfBlocks + 1);
    for (const EdgeBetweenSiblings& edge : edges)
        ++firstEdgeFrom[edge.from.index() + 1];
    for (uint32_t block = 0; block < numberOfBlocks; ++block)
        firstEdgeFrom[block + 1] += firstEdgeFrom[block];
    Vector<BlockID> siblingsEnteredFrom;
    siblingsEnteredFrom.fill(BlockID(), edges.size());
    Vector<uint32_t> nextEdgeFrom = firstEdgeFrom;
    for (const EdgeBetweenSiblings& edge : edges)
        siblingsEnteredFrom[nextEdgeFrom[edge.from.index()]++] = edge.to;

    while (!worklist.isEmpty()) {
        uint32_t child = worklist.takeLast().index();
        for (BlockID sibling : siblingsEnteredFrom.span().subspan(firstEdgeFrom[child], firstEdgeFrom[child + 1] - firstEdgeFrom[child])) {
            if (!isReached.quickSet(sibling.index()))
                worklist.append(sibling);
        }
    }
    return isReached;
}

bool BodyVerifier::terminatorDominates(BlockID from, BlockID to)
{
    if (!index().strictlyDominates(from, to))
        return false;
    BodyBlock block = index().block(from);
    if (!block.hasHandler() || index().immediateDominator(block.handler().block()) != from)
        return true;

    if (!m_blocksReachedFromHandlerOfDominator)
        m_blocksReachedFromHandlerOfDominator.emplace(findBlocksReachedFromHandlerOfDominator(index()));

    std::span<const BlockID> order = index().blocksInDominatorOrder();
    uint32_t number = index().dominatorNumber(to);
    BlockID child = order[index().dominatorNumber(from) + 1];
    while (index().lastDominatorNumberBelow(child) < number)
        child = order[index().lastDominatorNumberBelow(child) + 1];
    return !m_blocksReachedFromHandlerOfDominator->quickGet(child.index());
}

void BodyVerifier::verifyUseStepByStep(ValueID used, BodyUseKind kind, BlockID blockOfUse, ValueID user, uint32_t operandIndex)
{
    if (hasFailed())
        return;

    BlockID blockOfDefinition = m_scratch.notesOnValues[used.index()].block();
    bool isParameter = index().isParameter(used);
    bool isAvailable = false;
    if (blockOfDefinition != blockOfUse)
        isAvailable = isParameter ? index().dominates(blockOfDefinition, blockOfUse) : terminatorDominates(blockOfDefinition, blockOfUse);
    else {
        switch (kind) {
        case BodyUseKind::Operand:
            isAvailable = used.index() < user.index();
            break;
        case BodyUseKind::TerminatorOperand:
        case BodyUseKind::EdgeArgument:
        case BodyUseKind::EdgeProof:
            isAvailable = true;
            break;
        case BodyUseKind::HandlerArgument:
        case BodyUseKind::BlockSubject:
            isAvailable = isParameter;
            break;
        }
    }

    ValueID reportedValue = kind == BodyUseKind::Operand ? user : used;
    if (!isAvailable) {
        if (kind == BodyUseKind::HandlerArgument)
            fail(InternalErrorKind::HandlerArgumentNotAvailable, blockOfUse, used, operandIndex);
        else
            fail(InternalErrorKind::UseNotDominated, blockOfUse, reportedValue, used.index());
        return;
    }
    if (resultRuleOf(index().opcodeOf(used)) == ResultRule::None) {
        fail(InternalErrorKind::OperandTypeMismatch, blockOfUse, reportedValue, used.index());
        return;
    }
    if (isParameter && useInBodyMustNameDefinition(index(), kind, blockOfUse, user, operandIndex))
        fail(InternalErrorKind::OperandMustBeDefinition, blockOfUse, reportedValue, used.index());
}

void BodyVerifier::verifyEdges()
{
    m_scratch.subjectsOfProofs.shrink(0);
    BlockID sourceOfEdgeToHandlerBlock;
    BlockID handlerBlockThatEdgeEnters;
    for (uint32_t blockIndex = 0; blockIndex < index().numberOfBlocks(); ++blockIndex) {
        BodyBlock block = index().block(BlockID(blockIndex));
        BodyTerminator terminator = block.terminator();
        if (!verifyTargetCount(block, terminator))
            return;
        TakenByTargets takenByTargets = takenByTargetsOf(terminator.kind());
        uint32_t targetIndex = 0;
        for (BodyTarget target : terminator.targets()) {
            BodyBlock to = index().block(target.block());
            if (!verifyEdge(block, terminator, takenByTargets, targetIndex++, target, to))
                return;
            if (to.isHandlerBlock() && !sourceOfEdgeToHandlerBlock) [[unlikely]] {
                sourceOfEdgeToHandlerBlock = block.id();
                handlerBlockThatEdgeEnters = target.block();
            }
        }
        if (block.hasHandler()) {
            verifyHandler(block);
            if (hasFailed())
                return;
        }
        if (block.role() != BlockRole::Ordinary || block.subject()) {
            verifyBlockRole(block);
            if (hasFailed())
                return;
        }
    }
    if (sourceOfEdgeToHandlerBlock)
        fail(InternalErrorKind::InvalidHandler, sourceOfEdgeToHandlerBlock, { }, handlerBlockThatEdgeEnters.index());
}

ALWAYS_INLINE_INTO_OTHER_FILES bool BodyVerifier::verifyEdge(const BodyBlock& from, const BodyTerminator& terminator, TakenByTargets takenByTargets, uint32_t targetIndex, BodyTarget target, const BodyBlock& to)
{
    if (target.block() == entryBlock) [[unlikely]] {
        fail(InternalErrorKind::EdgeToEntryBlock, from.id());
        return false;
    }

    TargetResults results = takenByTargets == TakenByTargets::Nothing ? TargetResults::None
        : takenByTargets == TakenByTargets::Proofs ? TargetResults::Proofs
        : targetResultsOf(terminator.kind(), targetIndex, terminator.numberOfTargets());
    ASSERT(results == targetResultsOf(terminator.kind(), targetIndex, terminator.numberOfTargets()));
    std::span<const uint32_t> proofs = target.proofs();
    if (!proofs.empty() && results != TargetResults::Proofs) [[unlikely]] {
        fail(InternalErrorKind::InvalidProof, from.id());
        return false;
    }

    uint64_t numberOfIncomingValues = target.numberOfIncomingValues();
    if (results == TargetResults::DeclaredResults)
        numberOfIncomingValues += infoOfTarget(terminator.kind(), targetIndex, terminator.numberOfTargets())->numberOfResults;
    if (numberOfIncomingValues != to.numberOfParameters()) [[unlikely]] {
        fail(InternalErrorKind::EdgeArgumentCountMismatch, from.id(), { }, target.block().index());
        return false;
    }

    if (proofs.empty())
        return true;
    SubjectsOfProofsInVerifiedBody subjects(index(), terminator);
    for (uint32_t proofIndex = 0; proofIndex < proofs.size(); ++proofIndex) {
        ValueID subject = subjects.named(proofs[proofIndex]);
        if (!subject) [[unlikely]] {
            fail(InternalErrorKind::InvalidProof, from.id(), { }, proofIndex);
            return false;
        }
        m_scratch.subjectsOfProofs.append(subject);
    }
    return true;
}

void BodyVerifier::verifyHandler(const BodyBlock& block)
{
    BodyTarget handler = block.handler();
    if (handler.block() == entryBlock) {
        fail(InternalErrorKind::EdgeToEntryBlock, block.id());
        return;
    }
    if (!handler.proofs().empty()) {
        fail(InternalErrorKind::InvalidProof, block.id());
        return;
    }

    BodyBlock handlerBlock = index().block(handler.block());
    if (handler.arguments().size() != handlerBlock.numberOfParameters()) {
        fail(InternalErrorKind::EdgeArgumentCountMismatch, block.id(), { }, handler.block().index());
        return;
    }
    if (!handlerBlock.isHandlerBlock())
        fail(InternalErrorKind::InvalidHandler, block.id(), { }, handler.block().index());
}

void BodyVerifier::verifyBlockRole(const BodyBlock& block)
{
    ValueID subject = block.subject();
    bool hasFittingSubject = false;
    switch (block.role()) {
    case BlockRole::Ordinary:
    case BlockRole::CatchHandler:
    case BlockRole::CleanupHandler:
        hasFittingSubject = !subject;
        break;
    case BlockRole::IterationCleanup:
        hasFittingSubject = subject && index().opcodeOf(subject) == Opcode::IteratorOpen;
        break;
    case BlockRole::DisposalCleanup:
        hasFittingSubject = !!subject;
        break;
    }
    if (!hasFittingSubject || (block.id() == entryBlock && block.isHandlerBlock()))
        fail(InternalErrorKind::InvalidBlockRole, block.id(), subject);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool BodyVerifier::verifyTargetCount(const BodyBlock& block, const BodyTerminator& terminator)
{
    std::optional<uint32_t> numberOfFixedTargets = numberOfFixedTargetsOf(terminator.kind());
    uint64_t expectedNumber = numberOfFixedTargets
        ? *numberOfFixedTargets
        : numberOfCasesListedInBody(terminator) + static_cast<uint64_t>(std::ranges::count(targetInfosOf(terminator.kind()), false, &TargetInfo::isVariadic));
    if (terminator.numberOfTargets() == expectedNumber) [[likely]]
        return true;
    fail(InternalErrorKind::TargetCountMismatch, block.id(), { }, terminator.numberOfTargets());
    return false;
}

static bool holdsLoopMarkInVerifiedBody(const BodyBlock& block)
{
    for (BodyValue value : block.values()) {
        if (value.opcode() == Opcode::LoopHint)
            return true;
    }
    return false;
}

void BodyVerifier::verifyLoopMarks()
{
    if (!index().hasLoops())
        return;

    bool everyHeadHoldsLoopMark = !index().hasIrreducibleFlow();
    for (uint32_t loop = 0; loop < index().numberOfLoops() && everyHeadHoldsLoopMark; ++loop)
        everyHeadHoldsLoopMark = holdsLoopMarkInVerifiedBody(index().block(index().loop(LoopID(loop)).head));
    if (everyHeadHoldsLoopMark)
        return;

    const FlowGraphOfVerifiedBody& graph = flowGraph();
    BitVector holdsLoopMark(graph.numberOfBlocks());
    for (uint32_t blockIndex = 0; blockIndex < graph.numberOfBlocks(); ++blockIndex) {
        if (index().loopOf(BlockID(blockIndex)) && holdsLoopMarkInVerifiedBody(index().block(BlockID(blockIndex))))
            holdsLoopMark.quickSet(blockIndex);
    }

    StronglyConnectedComponents components = computeStronglyConnectedComponents(graph.numberOfBlocks(), [&](unsigned blockIndex) -> std::span<const BlockID> {
        if (holdsLoopMark.quickGet(blockIndex))
            return { };
        return graph.successors(BlockID(blockIndex));
    });
    if (!components.isInCycle.isEmpty())
        fail(InternalErrorKind::CycleWithoutLoopMark, BlockID(components.isInCycle.findBit(0, true)));
}

void BodyVerifier::verifyEntryParameters()
{
    BodyBlock entry = index().block(entryBlock);
    unsigned numberOfEntryParameters = function().numberOfEntryParameters();
    if (entry.numberOfParameters() != numberOfEntryParameters) {
        fail(InternalErrorKind::EntryParametersMismatch, entryBlock);
        return;
    }

    for (unsigned parameterIndex = 0; parameterIndex < numberOfEntryParameters; ++parameterIndex) {
        const DeclaredType* declared = function().declaredEntryParameter(parameterIndex);
        if (!declared || entry.parameterType(parameterIndex) == declared->written())
            continue;
        fail(InternalErrorKind::EntryParametersMismatch, entryBlock, entry.parameter(parameterIndex), parameterIndex);
        return;
    }
}

void BodyVerifier::verifyIndexAgainstPlainComputation()
{
    if (!m_comparesIndexWithPlainComputation)
        return;
    const FlowGraphOfVerifiedBody& graph = flowGraph();
    BodyIndexComparison comparison(index(), m_scratch);
    comparison.compareUses();
    comparison.compareIncomingEdges(graph);
    comparison.compareDominators(graph);
    comparison.compareOnlyIncomingEdges(graph);
    comparison.compareLoops(graph);
    if (!comparison.difference())
        return;
    fail(InternalErrorKind::BodyIndexMismatch, comparison.difference()->block, comparison.difference()->value);
    m_errors.last().detail = describeDifferenceInBodyIndex(*comparison.difference());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
