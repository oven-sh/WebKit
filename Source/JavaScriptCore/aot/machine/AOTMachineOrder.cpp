/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineOrder.h"

#if ENABLE(AOT)

#include "AOTInliningAcrossFiles.h"
#include "AOTMachineTarget.h"
#include "AOTSort.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static_assert(EdgeClass::Normal < EdgeClass::Unlikely && EdgeClass::Unlikely < EdgeClass::Rare);

static constexpr uint32_t positionsOfBlockSplitLate = 4;

void Order::dropUpsilonsHandedTo(const Block* formerTarget, Block* from)
{
    for (const Edge& other : from->successors()) {
        if (other.block() == formerTarget)
            return;
    }
    uint32_t numberKept = 0;
    for (Operation* upsilon : from->m_upsilons) {
        Operation* phi = upsilon->phi();
        if (phi && phi->owner() == formerTarget)
            continue;
        upsilon->m_indexInBlock = numberKept;
        from->m_upsilons[numberKept++] = upsilon;
    }
    from->m_upsilons.shrink(numberKept);
}

void Order::emptyBlock(Block* block)
{
    auto takeBackReadsOf = [](const Operation* reader) {
        if (countsChildrenAtFirstUse(reader->opcode()))
            return;
        for (Operation* child : reader->children())
            child->loseUse();
    };
    for (const Operation* upsilon : block->m_upsilons)
        takeBackReadsOf(upsilon);
    for (const Operation* operation : block->m_operations)
        takeBackReadsOf(operation);
    block->clearSuccessors();
    block->m_operations.clear();
    block->m_upsilons.clear();
    block->m_phis.clear();
}

void Order::letGoOfBlockThatNothingLeadsTo(Block* first)
{
    while (first->m_successors.size() <= 1) {
        ASSERT(!first->m_numberOfIncomingEdges && !first->m_isEntry);
        Block* next = first->m_successors.isEmpty() ? nullptr : first->m_successors[0].block();
        emptyBlock(first);
        if (!next || next->m_numberOfIncomingEdges || next->m_isEntry || next->isEmpty())
            return;
        first = next;
    }

    Vector<Block*, 8> pending;
    pending.append(first);
    while (!pending.isEmpty()) {
        Block* block = pending.takeLast();
        ASSERT(!block->m_numberOfIncomingEdges && !block->m_isEntry);
        Vector<Block*, 4> targets;
        for (const Edge& edge : block->m_successors)
            targets.append(edge.block());
        emptyBlock(block);
        for (Block* target : targets) {
            if (!target->m_numberOfIncomingEdges && !target->m_isEntry && !target->isEmpty())
                pending.append(target);
        }
    }
}

void Order::replaceJumpWithBranch(Block* from, const Operation* phi, bool isInverted)
{
    Block* join = phi->owner();
    Operation* end = from->last();
    RELEASE_ASSERT(end->opcode() == Jump && from->numSuccessors() == 1 && from->successor(0).block() == join);
    RELEASE_ASSERT(join->last()->opcode() == Branch && join->numSuccessors() == 2);

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
    setClassOf(from->m_successors[0], std::max(classOfJump, whenNotZero.edgeClass()));
    from->m_successors.append(from->procedure().arena(), Edge(whenZero.block(), std::max(classOfJump, whenZero.edgeClass())));
    whenZero.block()->didGainIncomingEdge(from, from->m_successors[1].edgeClass());

    end->m_opcode = Branch;
    end->m_origin = join->last()->origin();
    end->m_auxiliary = 1;
    end->m_children[0] = tested;
}

void Order::makePhiOfTruncationBehindJoin(Block* join, Operation* truncation)
{
    Block* passing = truncation->owner();
    Procedure& procedure = join->procedure();
    Arena& arena = procedure.arena();
    Operation* wide = truncation->m_children[0];
    RELEASE_ASSERT(truncation->opcode() == Trunc && truncation->type() == Int32 && truncation->useCount());
    RELEASE_ASSERT(wide->opcode() == Phi && wide->owner() == join && passing != join && passing->m_phis.isEmpty() && passing->numberOfIncomingEdges() == 1);

    Operation* end = join->m_operations.takeLast();
    Operation* own = arena.create<Operation>(Trunc, Int32, truncation->origin());
    own->m_auxiliary = 1;
    own->m_children[0] = wide;
    own->m_useCount = 1;
    own->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfIsRead) | static_cast<uint16_t>(OperationFlag::HasOpaqueReader);
    own->m_owner = join;
    own->m_indexInBlock = join->m_operations.size();
    procedure.didCreate(own);
    join->m_operations.append(arena, own);
    end->m_indexInBlock = join->m_operations.size();
    join->m_operations.append(arena, end);

    uint32_t size = passing->m_operations.size();
    for (uint32_t index = truncation->m_indexInBlock + 1; index < size; ++index) {
        Operation* moved = passing->m_operations[index];
        moved->m_indexInBlock = index - 1;
        passing->m_operations[index - 1] = moved;
    }
    passing->m_operations.shrink(size - 1);

    truncation->m_opcode = Phi;
    truncation->m_auxiliary = 0;
    truncation->m_children[0] = nullptr;
    truncation->m_flags |= static_cast<uint16_t>(OperationFlag::UpperHalfMayBeSet);
    truncation->m_indexInBlock = 0;
    passing->m_phis.append(arena, truncation);

    Operation* upsilon = arena.create<Operation>(Upsilon, Void, truncation->origin());
    upsilon->m_auxiliary = 1;
    upsilon->m_children[0] = own;
    upsilon->m_payload.phi = truncation;
    upsilon->m_owner = join;
    upsilon->m_indexInBlock = join->m_upsilons.size();
    procedure.didCreate(upsilon);
    join->m_upsilons.append(arena, upsilon);
}

void Order::passTestToJoin(Block* from, Block* join, const Operation* phi, bool isInverted)
{
    Procedure& procedure = from->procedure();
    Arena& arena = procedure.arena();
    std::array<Block*, 2> ways { from->successor(0).block(), from->successor(1).block() };
    RELEASE_ASSERT(from->last()->opcode() == Branch && from->numSuccessors() == 2 && phi->owner() == join && phi->type() == Int32);

    const Block* way = ways[0] == join ? ways[1] : ways[0];
    Operation* branch = from->m_operations.takeLast();
    Operation* test = branch->m_children[0];
    auto append = [&](ArenaVector<Operation*>& list, Opcode opcode, Type type, Origin origin) {
        Operation* made = arena.create<Operation>(opcode, type, origin);
        made->m_owner = from;
        made->m_indexInBlock = list.size();
        procedure.didCreate(made);
        list.append(arena, made);
        return made;
    };
    Operation* number = test;
    if (isInverted) {
        number = append(from->m_operations, invertedComparison(test->opcode()), Int32, test->origin());
        number->m_auxiliary = 2;
        number->m_children[0] = test->m_children[0];
        number->m_children[1] = test->m_children[1];
    }
    append(from->m_operations, Jump, Void, branch->origin());

    Vector<std::pair<Operation*, Operation*>, 4> passed;
    for (Operation* upsilon : way->upsilons()) {
        if (Operation* other = upsilon->phi(); other && other->owner() == join)
            passed.append({ other, other == phi ? number : upsilon->child(0) });
    }
    for (auto [other, value] : passed)
        value->gainUse();
    test->loseUse();
    for (Operation* upsilon : from->m_upsilons) {
        if (Operation* other = upsilon->phi(); other && other->owner() == join)
            upsilon->child(0)->loseUse();
    }
    from->clearSuccessors();
    dropUpsilonsHandedTo(join, from);
    for (auto [other, value] : passed) {
        Operation* upsilon = append(from->m_upsilons, Upsilon, Void, branch->origin());
        upsilon->m_auxiliary = 1;
        upsilon->m_children[0] = value;
        upsilon->m_payload.phi = other;
        procedure.noteUpperHalfHandedOn(upsilon);
    }
    from->setSuccessors(Edge(join));
    for (Block* left : ways) {
        if (left != join)
            letGoOfBlockThatNothingLeadsTo(left);
    }
}

void Order::moveTestToLatch(Block* header, Block* latch, Operation* test, unsigned successorInLoop)
{
    Operation* phi = header->m_phis[0];
    RELEASE_ASSERT(header->m_phis.size() == 1 && header->m_upsilons.isEmpty() && header->last()->opcode() == Branch && header->numSuccessors() == 2 && (test->child(0) == phi || test->child(1) == phi));
    RELEASE_ASSERT(latch->last()->opcode() == Jump && latch->numSuccessors() == 1 && latch->successor(0).block() == header);
    Operation* next = nullptr;
    for (Operation* upsilon : latch->m_upsilons) {
        if (upsilon->phi() == phi)
            next = upsilon->child(0);
    }
    RELEASE_ASSERT(next);

    Arena& arena = header->procedure().arena();
    const Edge inLoop = header->m_successors[successorInLoop];
    const Edge leaving = header->m_successors[1 - successorInLoop];
    Operation* jump = latch->m_operations.takeLast();
    test->m_children[test->child(0) == phi ? 0 : 1] = next;
    next->gainUse();
    phi->loseUse();
    for (Operation* operation : header->m_operations) {
        operation->m_owner = latch;
        operation->m_indexInBlock = latch->m_operations.size();
        latch->m_operations.append(arena, operation);
    }
    header->m_operations.clear();
    jump->m_owner = header;
    jump->m_indexInBlock = 0;
    header->m_operations.append(arena, jump);
    latch->m_writesMemory |= header->m_writesMemory;

    header->clearSuccessors();
    latch->clearSuccessors();
    const Edge back(header, inLoop.edgeClass());
    if (successorInLoop)
        latch->setSuccessors(leaving, back);
    else
        latch->setSuccessors(back, leaving);
    header->setSuccessors(Edge(inLoop.block()));
}

void Order::passJoinBy(Block* from, Block* target, Operation* phi, Operation* value)
{
    RELEASE_ASSERT(from->last()->opcode() == Jump && from->numSuccessors() == 1 && !phi == !value);
    Block* join = from->successor(0).block();
    Procedure& procedure = from->procedure();
    Arena& arena = procedure.arena();

    Vector<Operation*, 4> handedToJoin;
    for (Operation* upsilon : from->upsilons()) {
        if (upsilon->phi() && upsilon->phi()->owner() == join)
            handedToJoin.append(upsilon->child(0));
    }
    retarget(from, 0, target);

    if (phi) {
        RELEASE_ASSERT(phi->opcode() == Phi && phi->owner() == target && phi->type() == value->type());
        Operation* upsilon = arena.create<Operation>(Upsilon, Void, from->last()->origin());
        upsilon->m_auxiliary = 1;
        upsilon->m_children[0] = value;
        upsilon->m_payload.phi = phi;
        upsilon->m_owner = from;
        upsilon->m_indexInBlock = from->m_upsilons.size();
        procedure.didCreate(upsilon);
        from->m_upsilons.append(arena, upsilon);
        value->gainUse();
        procedure.noteUpperHalfHandedOn(upsilon);
    }
    for (Operation* handed : handedToJoin)
        handed->loseUse();
}

Block* Order::split(Block* from, unsigned successorIndex, EdgeClass classOfEdgeToTarget)
{
    ASSERT(from->isTerminated());
    Procedure& procedure = from->procedure();
    Arena& arena = procedure.arena();
    Block* target = from->successor(successorIndex).block();
    ASSERT(!target->isLandingPad());
    Block* middle = procedure.addBlockThatOnlyJumps();

    auto append = [&](ArenaVector<Operation*>& list, Opcode opcode, Type type, Origin origin) ALWAYS_INLINE_LAMBDA {
        Operation* operation = arena.create<Operation>(opcode, type, origin);
        operation->m_owner = middle;
        operation->m_indexInBlock = list.size();
        procedure.didCreate(operation);
        list.append(arena, operation);
        return operation;
    };

    if (target->m_phis.isEmpty()) {
        target->didLoseIncomingEdge(from, from->m_successors[successorIndex].m_class);
        from->m_successors[successorIndex].m_block = middle;
        middle->didGainIncomingEdge(from, from->m_successors[successorIndex].m_class);
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
    middle->appendSuccessorInline(Edge(target, classOfEdgeToTarget));
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

void Order::readWordWhereRequested(Block* asked, Operation* word)
{
    Procedure& procedure = asked->procedure();
    Arena& arena = procedure.arena();
    Operation* branch = asked->m_operations.takeLast();
    RELEASE_ASSERT(asked->isEmpty() && branch->opcode() == Branch && branch->m_children[0] == word && isLoad(word->opcode()) && word->useCount() == 1);
    Operation* read = arena.create<Operation>(word->opcode(), word->type(), word->origin());
    read->m_flags = word->m_flags;
    read->m_auxiliary = word->m_auxiliary;
    read->m_payload = word->m_payload;
    read->m_owner = asked;
    read->m_indexInBlock = 0;
    read->m_children[0] = word->m_children[0];
    read->m_children[0]->gainUse();
    procedure.didCreate(read);
    asked->m_operations.append(arena, read);
    branch->m_children[0] = read;
    branch->m_indexInBlock = 1;
    asked->m_operations.append(arena, branch);
    read->gainUse();
    word->loseUse();
}

void Order::compareEarlierReadWord(Operation* comparison, Operation* word)
{
    Operation* readAgain = comparison->m_children[0];
    comparison->m_children[0] = word;
    word->gainUse();
    readAgain->loseUse();
}

void Order::leaveTestToNext(Block* first, unsigned leavingSuccessor)
{
    Operation* branch = first->last();
    RELEASE_ASSERT(branch->opcode() == Branch && first->numSuccessors() == 2 && first->upsilons().empty());
    const Edge wayOn = first->successor(1 - leavingSuccessor);
    branch->m_children[0]->loseUse();
    branch->m_children[0] = nullptr;
    branch->m_opcode = Jump;
    branch->m_auxiliary = 0;
    first->setSuccessors(wayOn);
}

void Order::askBitsOfTwoTestsAtOnce(Block* first, unsigned wayOfFirstWhereBitIsSet, Operation* bitsOfFirst, Block* second, unsigned wayOfSecondWhereBitIsSet, Operation* bitsOfSecond)
{
    Procedure& procedure = first->procedure();
    RELEASE_ASSERT(first->last()->opcode() == Branch && first->numSuccessors() == 2 && second->last()->opcode() == Branch && second->numSuccessors() == 2);
    RELEASE_ASSERT(first->successor(1 - wayOfFirstWhereBitIsSet).block() == second && second->numberOfIncomingEdges() == 1);
    RELEASE_ASSERT(first->upsilons().empty() && second->upsilons().empty());

    const Edge targetOfFirst = first->successor(wayOfFirstWhereBitIsSet);
    const Edge toSecond = first->successor(1 - wayOfFirstWhereBitIsSet);
    const Edge targetOfSecond = second->successor(wayOfSecondWhereBitIsSet);
    const Edge wayOn = second->successor(1 - wayOfSecondWhereBitIsSet);

    Operation* maskOfFirst = bitsOfFirst->m_children[1];
    Operation* maskOfSecond = bitsOfSecond->m_children[1];
    Operation* maskOfBoth = procedure.arena().create<Operation>(maskOfFirst->opcode(), maskOfFirst->type(), first->last()->origin());
    maskOfBoth->m_payload.bits = maskOfFirst->asInt() | maskOfSecond->asInt();
    procedure.didCreate(maskOfBoth);
    bitsOfFirst->m_children[1] = maskOfBoth;
    maskOfBoth->gainUse();
    bitsOfSecond->m_children[1] = maskOfFirst;
    maskOfSecond->loseUse();
    Operation* wordOfFirst = bitsOfFirst->m_children[0];
    Operation* wordOfSecond = bitsOfSecond->m_children[0];
    if (wordOfSecond != wordOfFirst) {
        RELEASE_ASSERT(wordOfFirst->owner() == first && wordOfSecond->owner() == second && wordOfSecond->useCount() == 1);
        bitsOfSecond->m_children[0] = wordOfFirst;
        wordOfFirst->gainUse();
        wordOfSecond->loseUse();
    }

    Edge whereBitOfEitherIsSet(second, std::min(targetOfFirst.edgeClass(), targetOfSecond.edgeClass()));
    Edge whereNoBitIsSet(wayOn.block(), std::max(toSecond.edgeClass(), wayOn.edgeClass()));
    second->clearSuccessors();
    if (wayOfFirstWhereBitIsSet)
        first->setSuccessors(whereNoBitIsSet, whereBitOfEitherIsSet);
    else
        first->setSuccessors(whereBitOfEitherIsSet, whereNoBitIsSet);
    if (wayOfSecondWhereBitIsSet)
        second->setSuccessors(targetOfSecond, targetOfFirst);
    else
        second->setSuccessors(targetOfFirst, targetOfSecond);
}

void Order::askBothBoundsAtOnce(Block* first, unsigned leavingSuccessorOfFirst, Block* second, unsigned leavingSuccessorOfSecond, Operation* subject, int32_t low, int32_t high)
{
    Procedure& procedure = first->procedure();
    Arena& arena = procedure.arena();
    RELEASE_ASSERT(first->last()->opcode() == Branch && first->numSuccessors() == 2 && second->last()->opcode() == Branch && second->numSuccessors() == 2);
    RELEASE_ASSERT(first->successor(1 - leavingSuccessorOfFirst).block() == second && second->numberOfIncomingEdges() == 1);
    RELEASE_ASSERT(first->upsilons().empty() && second->upsilons().empty() && subject->type() == Int32);

    const Edge leavingFirst = first->successor(leavingSuccessorOfFirst);
    const Edge toSecond = first->successor(1 - leavingSuccessorOfFirst);
    const Edge leavingSecond = second->successor(leavingSuccessorOfSecond);
    const Edge inside = second->successor(1 - leavingSuccessorOfSecond);

    Operation* branch = first->m_operations.takeLast();
    Origin origin = branch->origin();
    auto constant = [&](int32_t value) {
        Operation* made = arena.create<Operation>(Const32, Int32, origin);
        made->m_payload.bits = value;
        procedure.didCreate(made);
        return made;
    };
    auto append = [&](Opcode opcode, Operation* left, Operation* right) {
        Operation* made = arena.create<Operation>(opcode, Int32, origin);
        made->m_auxiliary = 2;
        made->m_children[0] = left;
        made->m_children[1] = right;
        made->m_owner = first;
        made->m_indexInBlock = first->m_operations.size();
        procedure.didCreate(made);
        first->m_operations.append(arena, made);
        return made;
    };
    Operation* distance = low ? append(Add, subject, constant(static_cast<int32_t>(0u - static_cast<uint32_t>(low)))) : subject;
    Operation* test = append(BelowEqual, distance, constant(static_cast<int32_t>(static_cast<uint32_t>(high) - static_cast<uint32_t>(low))));
    branch->m_children[0]->loseUse();
    branch->m_children[0] = test;
    test->gainUse();
    branch->m_indexInBlock = first->m_operations.size();
    first->m_operations.append(arena, branch);

    second->clearSuccessors();
    first->setSuccessors(Edge(inside.block(), std::max(toSecond.edgeClass(), inside.edgeClass())), Edge(leavingFirst.block(), std::min(leavingFirst.edgeClass(), leavingSecond.edgeClass())));
    letGoOfBlockThatNothingLeadsTo(second);
    if (!leavingSecond.block()->numberOfIncomingEdges())
        letGoOfBlockThatNothingLeadsTo(leavingSecond.block());
}

void Order::askBytesOfTwoTestsAtOnce(Block* first, unsigned wayOnOfFirst, Block* second, unsigned wayOnOfSecond, Operation* base, int64_t offsetOfWord, uint32_t mask, uint32_t value)
{
    Procedure& procedure = first->procedure();
    Arena& arena = procedure.arena();
    RELEASE_ASSERT(first->last()->opcode() == Branch && first->numSuccessors() == 2 && second->last()->opcode() == Branch && second->numSuccessors() == 2);
    RELEASE_ASSERT(first->successor(wayOnOfFirst).block() == second && second->numberOfIncomingEdges() == 1);
    RELEASE_ASSERT(first->upsilons().empty() && second->upsilons().empty());

    const Edge leavingFirst = first->successor(1 - wayOnOfFirst);
    const Edge leavingSecond = second->successor(1 - wayOnOfSecond);
    const Edge wayOn = second->successor(wayOnOfSecond);

    Operation* branch = first->m_operations.takeLast();
    Origin origin = branch->origin();
    auto constant = [&](uint32_t bits) {
        Operation* made = arena.create<Operation>(Const32, Int32, origin);
        made->m_payload.bits = static_cast<int32_t>(bits);
        procedure.didCreate(made);
        return made;
    };
    auto append = [&](Opcode opcode, Operation* left, Operation* right) {
        Operation* made = arena.create<Operation>(opcode, Int32, origin);
        made->m_auxiliary = right ? 2 : 1;
        made->m_children[0] = left;
        if (right)
            made->m_children[1] = right;
        made->m_owner = first;
        made->m_indexInBlock = first->m_operations.size();
        procedure.didCreate(made);
        first->m_operations.append(arena, made);
        return made;
    };
    Operation* word = append(Load, base, nullptr);
    base->gainUse();
    word->m_payload.memory.offset = offsetOfWord;
    word->m_payload.memory.heap = rootHeapIndex;
    Operation* test = append(Equal, append(BitAnd, word, constant(mask)), constant(value));
    branch->m_children[0]->loseUse();
    branch->m_children[0] = test;
    test->gainUse();
    branch->m_indexInBlock = first->m_operations.size();
    first->m_operations.append(arena, branch);

    second->clearSuccessors();
    first->setSuccessors(Edge(wayOn.block(), wayOn.edgeClass()), Edge(leavingFirst.block(), leavingFirst.edgeClass()));
    letGoOfBlockThatNothingLeadsTo(second);
    if (!leavingSecond.block()->numberOfIncomingEdges())
        letGoOfBlockThatNothingLeadsTo(leavingSecond.block());
}

void Order::askEqualitiesBySwitch(Block* first, Operation* subject, std::span<Block* const> laterLinks, std::span<const CaseOfEquality> sortedCases, Edge otherwise)
{
    Procedure& procedure = first->procedure();
    Arena& arena = procedure.arena();
    RELEASE_ASSERT(first->last()->opcode() == Branch && first->upsilons().empty() && subject->type() == Int32);

    for (Block* link : laterLinks)
        link->clearSuccessors();
    first->clearSuccessors();

    std::span<int64_t> values = arena.createSpan<int64_t>(sortedCases.size());
    for (size_t index = 0; index < sortedCases.size(); ++index) {
        RELEASE_ASSERT(!index || sortedCases[index - 1].value < sortedCases[index].value);
        values[index] = sortedCases[index].value;
    }
    Operation* end = first->last();
    end->m_children[0]->loseUse();
    end->m_opcode = Switch;
    end->m_auxiliary = 1;
    end->m_children[0] = subject;
    subject->gainUse();
    end->m_payload.switchData = arena.create<SwitchData>(SwitchData { values });
    for (const CaseOfEquality& one : sortedCases)
        first->appendSuccessor(one.target);
    first->appendSuccessor(otherwise);

    for (Block* link : laterLinks)
        letGoOfBlockThatNothingLeadsTo(link);
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

    ASSERT(kept.m_block->m_numberOfIncomingEdges >= 2);
    kept.m_block->didLoseIncomingEdge(from, other.m_class);
    setClassOf(kept, std::min(kept.m_class, other.m_class));
    from->m_successors.shrink(1);
    branch->child(0)->loseUse();
}

namespace {

class BlockOrderBuilder {
    WTF_MAKE_NONCOPYABLE(BlockOrderBuilder);
public:
    enum class WaysDownFromRareBlocks : uint8_t {
        LeadAnywhere,
        StayAmongRareBlocks,
    };

    BlockOrderBuilder(Procedure& procedure, WaysDownFromRareBlocks waysDownFromRareBlocks)
        : m_proc(procedure)
        , m_arena(procedure.arena())
        , m_waysDownFromRareBlocksStayAmongThem(waysDownFromRareBlocks == WaysDownFromRareBlocks::StayAmongRareBlocks)
    {
    }

    void run();
    void letGoOfWorkingLists();
    void letGoOfOrder();

    enum class Search : uint8_t {
        ForBlocksOutsideRareRegions,
        ForRareBlocks,
    };

    ArenaVector<Block*> m_blocksInOrder;
    ArenaVector<Block*> m_blocksInLayout;
    ArenaVector<BlockPlace> m_places;
    ArenaVector<Loop> m_loops;
    uint32_t m_numberOfLoopsRotated { 0 };
    ArenaVector<RareRegion> m_rareRegions;
    unsigned m_numberOfBlocksOutsideRareRegions { 0 };
    Position m_end { 0 };
    uint32_t m_numberOfUnsplitEdges { 0 };
    bool m_someBlockStandsAheadOfItsPredecessors { false };
    std::span<Block* const> m_predecessors;
    std::span<const uint32_t> m_successorIndexesOfPredecessors;

private:
    static constexpr uint32_t none = UINT32_MAX;
    static constexpr uint32_t numberOfPasses = 3;

    enum Flag : uint32_t {
        IsReached = 1 << 0,
        IsRare = 1 << 1,
        IsVisited = 1 << 2,
        IsLikely = 1 << 3,
        IsEnteredThroughBlocksOnEdges = 1 << 4,
        HasUnsplitEdge = 1 << 5,
        IsAskedWhetherEdgesMayStayUnsplit = 1 << 6,
        EdgesMayStayUnsplit = 1 << 7,
        IsVisitedAmongRareBlocks = 1 << 8,
        MayGetBlocksOnEdges = 1 << 9,
        HasRareEdge = 1 << 10,
        WaitsBehindUnlikelyEdge = 1 << 11,
        HoldsOtherBlocksInCycle = 1 << 12,
        IsRareLandingPad = 1 << 13,
        FirstSuccessorIsLeft = 1 << 14,
        SecondSuccessorIsLeft = 1 << 15,
        ManySuccessorsMayBeLeft = 1 << 16,
        IsMovedAhead = 1 << 17,
        TakesRunBehind = 1 << 18,
        SomeSuccessorMayBeLeft = FirstSuccessorIsLeft | SecondSuccessorIsLeft | ManySuccessorsMayBeLeft,
    };

    using Part = LayoutPart;
    static constexpr Part LikelyPart = LayoutPart::Likely;
    static constexpr Part UnlikelyPart = LayoutPart::Unlikely;
    static constexpr Part RarePart = LayoutPart::Rare;
    static constexpr uint32_t numberOfParts = numberOfLayoutParts;

    struct BlockState {
        bool standsInNoCycle() const
        {
            static_assert(offsetof(BlockState, firstInCycle) == offsetof(BlockState, cycleHeader) + sizeof(uint32_t));
            uint64_t headerAndFirstInCycle;
            memcpy(&headerAndFirstInCycle, reinterpret_cast<const uint8_t*>(this) + offsetof(BlockState, cycleHeader), sizeof(headerAndFirstInCycle));
            return headerAndFirstInCycle == UINT64_MAX;
        }

        uint32_t flags { 0 };
        uint32_t numberOfPredecessors { 0 };
        union {
            uint32_t rankOnPath { 0 };
            uint32_t rankOfRegion;
        };
        union {
            uint32_t blockAboveOnPath { 0 };
            uint32_t next;
        };
        uint32_t cycleHeader { none };
        uint32_t firstInCycle { none };
        uint32_t lastInCycle { none };
        union {
            uint32_t indexOutsideRareRegions { none };
            uint32_t regionLink;
        };
    };
    static_assert(sizeof(BlockState) == 32);

    struct ProgressThroughSuccessors {
        uint32_t successorsLeftInPass;
        uint32_t pass;
    };

    struct RareEdgeFromOutside {
        Block* from;
        Block* target;
    };

    struct EdgeThatGetsBlock {
        uint32_t from;
        uint32_t successorIndex;
    };

    struct RegionPlan {
        Block* firstEntry { nullptr };
        bool hasSeveralEntries { false };
        uint32_t standsBehind { 0 };
        uint32_t standsAheadOf { none };
        uint32_t firstBlock { none };
        uint32_t lastBlock { none };
        uint32_t firstEdgeIntoOtherRegion { none };
    };
    static_assert(sizeof(RegionPlan) == 32);

    struct EntryOfRareRegion {
        Block* entry;
        uint32_t source;
    };

    struct EdgeToRareLandingPad {
        uint32_t from;
        uint32_t landingPad;
        uint32_t nextOutOfRegion;
    };

    struct RareLandingPad {
        uint32_t block;
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
    void noteEdgeLeadingBack(const Block* target, BlockPlace& placeOfTarget, uint32_t orderIndexOfSource);
    void placeBlocks();
    void findLoops();
    void findLoopsWithSideEntrances();
    void rotateLoops(uint32_t& beginOfMovedBlocks, uint32_t& endOfMovedBlocks);
    void placeLoops();
    uint32_t cycleOf(const Block*) const;
    bool fallsIntoNext(uint32_t layoutIndex) const;
    static bool needsNoFollowingRun(const Block*, const Block* next);
    bool mayMoveRunAhead(uint32_t beginOfRun, uint32_t cycle) const;
    uint32_t findRunToPlaceAfter(uint32_t layoutIndex) const;
    void moveRunsAhead();
#if ASSERT_ENABLED
    void assertOrderHolds();
#endif

    Procedure& m_proc;
    Arena& m_arena;
    const bool m_waysDownFromRareBlocksStayAmongThem;

    ArenaVector<BlockState> m_states;

    ArenaVector<RareEdgeFromOutside> m_rareEdgesFromOutside;
    const Block* m_blockWhoseEdgesAreCounted { nullptr };
    ArenaVector<uint32_t> m_numberOfEdgesFromCountedBlock;
    ArenaVector<EdgeThatGetsBlock> m_edgesThatGetBlocks;
    bool m_someRareBlockIsLandingPad { false };
    bool m_someBlockHasManySuccessors { false };
    uint32_t m_numberOfEdges { 0 };
    uint32_t m_numberOfBlocksInPart[numberOfParts] { 0, 0, 0 };

    ArenaVector<ProgressThroughSuccessors> m_progressThroughManySuccessors;
    uint32_t m_firstBlockInOrder { none };
    uint32_t m_firstRareBlock { none };
    const uint32_t* m_blocksOutsideRareRegions { nullptr };
    std::span<const EntryOfRareRegion> m_entriesOfRareRegions;
    uint32_t m_numberOfRareRegions { 0 };
    RegionPlan* m_regionPlans { nullptr };
    ArenaVector<EdgeToRareLandingPad> m_edgesToRareLandingPads;
    ArenaVector<RareLandingPad> m_rareLandingPads;
    uint32_t m_nextIndexInUnlikelyPart { 0 };
    uint32_t m_nextIndexInRarePart { 0 };
    ArenaVector<LoopHead> m_loopHeads;
};

NEVER_INLINE void BlockOrderBuilder::makeRoomForRareEdgeFromOutside()
{
    m_rareEdgesFromOutside.reserve(m_arena, std::max(m_rareEdgesFromOutside.capacity() * 2, m_proc.numberOfBlocks() / 4 + 4));
}

void BlockOrderBuilder::findReachedBlocks()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    m_states.reserve(m_arena, numberOfBlocks + numberOfBlocks / 2 + 4);
    m_states.growWithoutInitializing(m_arena, numberOfBlocks);
    BlockState* const states = m_states.begin();
    std::memset(static_cast<void*>(states), 0, static_cast<size_t>(numberOfBlocks) * sizeof(BlockState));

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

    auto mayGetBlockOnEdgeTo = [](const Block* target) ALWAYS_INLINE_LAMBDA {
        return target->numberOfIncomingEdges() >= 2 || !target->phis().empty();
    };

    enum class Wave : uint8_t {
        Likely,
        Unlikely,
        Rare,
    };
    auto takeReachedBlocks = [&](auto constantOfWave) ALWAYS_INLINE_LAMBDA {
        constexpr Wave wave = decltype(constantOfWave)::value;
        while (nextBlockToTake != endOfReachedBlocks) {
            Block* block = *nextBlockToTake++;
            uint32_t size = block->size();
            const Operation* last = size ? block->operations().data()[size - 1] : nullptr;
            uint32_t flagsOfBlock = 0;
            if (!last || !isTerminal(last->opcode())) [[unlikely]] {
                if (!last || !last->endsBlock()) {
                    everyBlockIsTerminated = false;
                    continue;
                }
                flagsOfBlock = MayGetBlocksOnEdges;
            }

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
            if (numberOfSuccessors == 2 && edges[0].block() == edges[1].block()) [[unlikely]] {
                Order::joinEdgesToOneBlock(block);
                numberOfSuccessors = block->numSuccessors();
            }
            numberOfEdges += numberOfSuccessors;
            if (numberOfSuccessors == 1)
                follow(edges[0]);
            else if (numberOfSuccessors == 2) {
                follow(edges[0]);
                follow(edges[1]);
                flagsOfBlock &= HasRareEdge;
                if (mayGetBlockOnEdgeTo(edges[0].block()) || mayGetBlockOnEdgeTo(edges[1].block()))
                    flagsOfBlock |= MayGetBlocksOnEdges;
            } else if (numberOfSuccessors) [[unlikely]] {
                flagsOfBlock = MayGetBlocksOnEdges;
                for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex)
                    follow(edges[successorIndex]);
            } else
                flagsOfBlock = 0;

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
    m_arena.letGoOfArray(reachedBlocks, 2 * static_cast<size_t>(numberOfBlocks));

    if (everyBlockIsTerminated) [[likely]]
        return;
    for (Block* block : m_proc.blocks()) {
        if (has(block, IsReached) && !block->isTerminated()) {
            m_proc.fail(FailureKind::MalformedProcedure, block->isEmpty() ? nullptr : block->last());
            return;
        }
    }
}

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

NEVER_INLINE void BlockOrderBuilder::makeRoomForEdgeThatGetsBlock()
{
    m_edgesThatGetBlocks.reserve(m_arena, std::max(m_edgesThatGetBlocks.capacity() * 2, m_proc.numberOfBlocks() / 4 + 4));
}

void BlockOrderBuilder::findEdgesThatGetBlocks()
{
    uint32_t numberOfBlocks = m_proc.numberOfBlocks();
    BlockState* const states = m_states.begin();
    Block* const* const blocks = m_proc.blocks().data();

    for (const RareEdgeFromOutside& edge : m_rareEdgesFromOutside) {
        BlockState& stateOfTarget = states[edge.target->index()];
        if (!(stateOfTarget.flags & IsRare) || stateOfTarget.numberOfPredecessors < 2 || (stateOfTarget.flags & IsEnteredThroughBlocksOnEdges))
            continue;
        if (!endsInTestWhoseEdgesMayStayUnsplit(edge.from, states[edge.from->index()]) || !hasOneEdgeTo(edge.from, edge.target))
            stateOfTarget.flags |= IsEnteredThroughBlocksOnEdges;
    }

    enum class Successors : uint8_t {
        One,
        Two,
        Many,
    };
    auto lookAtEdge = [&](auto constantOfSuccessors, const Block* from, BlockState& stateOfFrom, uint32_t index, const Edge* edges, unsigned successorIndex) ALWAYS_INLINE_LAMBDA {
        constexpr Successors successors = decltype(constantOfSuccessors)::value;
        const Edge& edge = edges[successorIndex];
        const Block* target = edge.block();
        uint32_t indexOfTarget = target->index();
        const BlockState& stateOfTarget = states[indexOfTarget];
        bool isJoin = stateOfTarget.numberOfPredecessors >= 2;
        bool targetHasPhis = !target->phis().empty();
        if (!isJoin && (successors == Successors::One || !targetHasPhis))
            return;
        if (target->isLandingPad())
            return;

        if (!isJoin) {
            if (!successorIndex && from->last()->opcode() == Patch && from->last()->type().isTuple())
                return;
        } else if (!targetHasPhis) {
            auto isOnlyEdgeToTarget = [&]() ALWAYS_INLINE_LAMBDA -> bool {
                if constexpr (successors == Successors::Many)
                    return hasOneOfManyEdgesTo(from, target);
                else if constexpr (successors == Successors::One)
                    return true;
                else {
                    if (edges[1 - successorIndex].block() != target) [[likely]]
                        return true;
                    return successorIndex && !m_edgesThatGetBlocks.isEmpty() && m_edgesThatGetBlocks.last().from == index;
                }
            };
            uint32_t flagsOfTarget = stateOfTarget.flags;
            bool targetIsRare = flagsOfTarget & IsRare;
            if (edge.isRare() == targetIsRare && !(targetIsRare && (flagsOfTarget & IsEnteredThroughBlocksOnEdges))
                && endsInTestWhoseEdgesMayStayUnsplit(from, stateOfFrom) && isOnlyEdgeToTarget()) {
                ++m_numberOfUnsplitEdges;
                stateOfFrom.flags |= HasUnsplitEdge;
                return;
            }
        }

        if constexpr (successors == Successors::Many)
            --m_numberOfEdgesFromCountedBlock[indexOfTarget];
        if (m_edgesThatGetBlocks.size() == m_edgesThatGetBlocks.capacity()) [[unlikely]]
            makeRoomForEdgeThatGetsBlock();
        m_edgesThatGetBlocks.appendWithinCapacity(EdgeThatGetsBlock { index, successorIndex });
    };

    BlockState* const endOfStates = states + numberOfBlocks;
    for (BlockState* stateOfFrom = states; ; ++stateOfFrom) {
        while (stateOfFrom != endOfStates && !(stateOfFrom->flags & MayGetBlocksOnEdges))
            ++stateOfFrom;
        if (stateOfFrom == endOfStates)
            break;
        uint32_t index = static_cast<uint32_t>(stateOfFrom - states);
        const Block* from = blocks[index];
        const Edge* const edges = from->successors().data();
        unsigned numberOfSuccessors = from->numSuccessors();
        if (numberOfSuccessors == 2) [[likely]] {
            lookAtEdge(std::integral_constant<Successors, Successors::Two> { }, from, *stateOfFrom, index, edges, 0);
            lookAtEdge(std::integral_constant<Successors, Successors::Two> { }, from, *stateOfFrom, index, edges, 1);
        } else if (numberOfSuccessors == 1)
            lookAtEdge(std::integral_constant<Successors, Successors::One> { }, from, *stateOfFrom, index, edges, 0);
        else if (numberOfSuccessors) {
            countEdgesOf(from);
            for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex)
                lookAtEdge(std::integral_constant<Successors, Successors::Many> { }, from, *stateOfFrom, index, edges, successorIndex);
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
        [[maybe_unused]] Block* middle = Order::split(from, listed.successorIndex, edge.isRare() && targetIsRare ? EdgeClass::Normal : edge.edgeClass());
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
    }
}

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

NEVER_INLINE Block* BlockOrderBuilder::takeNextOfManySuccessors(const Block* block, Search search)
{
    ProgressThroughSuccessors& progress = m_progressThroughManySuccessors.last();
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

template<BlockOrderBuilder::Search search, typename RootAt>
ALWAYS_INLINE uint32_t BlockOrderBuilder::orderBlocksReachedFrom(size_t numberOfRoots, const RootAt& rootAt)
{
    constexpr bool staysWithinRareRegions = search == Search::ForRareBlocks;
    constexpr uint32_t isVisited = staysWithinRareRegions ? IsVisitedAmongRareBlocks : IsVisited;

    [[maybe_unused]] const bool landingPadsBeginRegions = m_proc.traits().setsUpFrameAtEntry();

    BlockState* const states = m_states.begin();
    Block* const* const blocks = m_proc.blocks().data();
    uint32_t numberOfBlocksEntered = 0;
    uint32_t firstInOrder = none;
    uint32_t current = none;
    BlockState* stateOfCurrent = nullptr;
    Block* successor = nullptr;

    auto enter = [&](Block* block, uint32_t index, BlockState& state, uint32_t flags) ALWAYS_INLINE_LAMBDA -> bool {
        flags |= isVisited;
        state.rankOnPath = ++numberOfBlocksEntered;
        state.blockAboveOnPath = current;
        current = index;
        stateOfCurrent = &state;
        state.cycleHeader = none;
        state.firstInCycle = none;
        if constexpr (staysWithinRareRegions) {
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

    auto searchBelowRoot = [&]() ALWAYS_INLINE_LAMBDA {
        while (true) {
            uint32_t index = successor->index();
            BlockState& state = states[index];
            uint32_t flags = state.flags;
            if (!staysWithinRareRegions || (flags & IsRare)) {
                if (!(flags & isVisited)) {
                    bool goesDown = true;
                    if constexpr (staysWithinRareRegions) {
                        if (landingPadsBeginRegions && successor->isLandingPad()) [[unlikely]] {
                            state.regionLink = index;
                            ++m_numberOfRareRegions;
                            noteEdgeToRareLandingPad(current, index);
                        } else
                            state.regionLink = stateOfCurrent->regionLink;
                    } else if (state.numberOfPredecessors > 1 && !(flags & IsRare)) {
                        const Block* from = blocks[current];
                        if (m_waysDownFromRareBlocksStayAmongThem && (stateOfCurrent->flags & IsRare)) [[unlikely]]
                            goesDown = false;
                        else if (from->numSuccessors() > 1 && from->last()->opcode() != Patch && !successor->isLandingPad())
                            m_places[index].sourceOfUnsplitEdgeThatCountsLast = from;
                    }
                    if (goesDown && enter(successor, index, state, flags))
                        continue;
                } else {
                    if constexpr (staysWithinRareRegions) {
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
                        if constexpr (!staysWithinRareRegions) {
                            if (!(stateOfFinished.flags & IsRare) || (stateOfFinished.flags & HoldsOtherBlocksInCycle))
                                stateOfHeader.flags |= HoldsOtherBlocksInCycle;
                        }

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

    for (size_t numberOfRootsLeft = numberOfRoots; numberOfRootsLeft;) {
        Block* root = rootAt(--numberOfRootsLeft);
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
        stateOfRoot.rankOnPath = 0;
        stateOfRoot.next = firstInOrder;
        firstInOrder = indexOfRoot;
        current = none;
    }
    ASSERT(m_progressThroughManySuccessors.isEmpty());

    return firstInOrder;
}

void BlockOrderBuilder::takeRareBlocksOutOfOrder()
{
    BlockState* const states = m_states.begin();
    Block* const* const blocks = m_proc.blocks().data();
    uint32_t numberOfBlocksOutside = numberOfBlocksIn(LikelyPart) + numberOfBlocksIn(UnlikelyPart);
    uint32_t* const blocksOutside = m_arena.allocateArray<uint32_t>(numberOfBlocksOutside);
    EntryOfRareRegion* const entries = m_arena.allocateArray<EntryOfRareRegion>(m_rareEdgesFromOutside.size());
    EntryOfRareRegion* endOfEntries = entries;

    uint32_t numberOutside = 0;
    uint32_t* link = &m_firstBlockInOrder;
    for (uint32_t index = m_firstBlockInOrder; index != none;) {
        BlockState& state = states[index];
        uint32_t flags = state.flags;
        if (flags & IsRare) {
            do
                index = states[index].next;
            while (index != none && (states[index].flags & IsRare));
            *link = index;
            continue;
        }
        link = &state.next;
        blocksOutside[numberOutside] = index;
        state.indexOutsideRareRegions = numberOutside;

        uint32_t header = state.cycleHeader;
        if (header != none) {
            while (header != none && (states[header].flags & IsRare))
                header = states[header].cycleHeader;
            state.cycleHeader = header;
        }

        if (flags & HasRareEdge) {
            for (const Edge& edge : blocks[index]->successors()) {
                if (edge.isRare() && (states[edge.block()->index()].flags & IsRare))
                    *endOfEntries++ = EntryOfRareRegion { edge.block(), numberOutside };
            }
        }
        ++numberOutside;
        index = state.next;
    }
    RELEASE_ASSERT(numberOutside == numberOfBlocksOutside);

    m_blocksOutsideRareRegions = blocksOutside;
    m_entriesOfRareRegions = std::span<const EntryOfRareRegion>(entries, static_cast<size_t>(endOfEntries - entries));
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t BlockOrderBuilder::representativeOf(BlockState* states, uint32_t block)
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

    bool ranksRiseAlongEdges = true;
    for (const EdgeToRareLandingPad& edge : m_edgesToRareLandingPads) {
        uint32_t from = representativeOf(states, edge.from);
        uint32_t target = representativeOf(states, edge.landingPad);
        if (from != target)
            ranksRiseAlongEdges &= states[from].rankOfRegion < states[target].rankOfRegion;
    }
    if (ranksRiseAlongEdges)
        return;
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
    uint32_t representativeOfBlockInFront = none;
    uint32_t region = noRareRegion;
    for (uint32_t index = m_firstRareBlock; index != none;) {
        uint32_t next = states[index].next;
        uint32_t representative = representativeOf(states, index);
        if (representative != representativeOfBlockInFront) {
            representativeOfBlockInFront = representative;
            region = places[representative].rareRegion;
            if (region == noRareRegion) {
                region = numberOfRegionsWithoutRank++;
                places[representative].rareRegion = region;
            }
            RegionPlan& plan = plans[region];
            if (plan.firstBlock == none)
                plan.firstBlock = index;
            else
                states[plan.lastBlock].next = index;
        }
        places[index].rareRegion = region;
        plans[region].lastBlock = index;
        ++numberOfRareBlocks;
        index = next;
    }
    RELEASE_ASSERT(numberOfRegionsWithoutRank == firstRegionWithRank);
    RELEASE_ASSERT(numberOfRareBlocks == numberOfBlocksIn(RarePart));
}

void BlockOrderBuilder::placeRareRegions()
{
    BlockState* const states = m_states.begin();
    BlockPlace* const places = m_places.begin();
    Block* const* const blocks = m_proc.blocks().data();
    RegionPlan* const plans = m_regionPlans;
    uint32_t numberOfRegions = m_numberOfRareRegions;

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

NEVER_INLINE void BlockOrderBuilder::noteEdgeLeadingBack(const Block* target, BlockPlace& placeOfTarget, uint32_t orderIndexOfSource)
{
    if (placeOfTarget.isLoopHead) {
        m_loopHeads[placeOfTarget.loop].endBlock = orderIndexOfSource + 1;
        return;
    }
    if (placeOfTarget.predecessors.size() == 1 && !target->isEntry() && !target->isLandingPad())
        m_someBlockStandsAheadOfItsPredecessors = true;
    placeOfTarget.isLoopHead = true;
    placeOfTarget.loop = m_loopHeads.size();
    m_loopHeads.append(m_arena, LoopHead { placeOfTarget.orderIndex, orderIndexOfSource + 1 });
}

void BlockOrderBuilder::placeBlocks()
{
    uint32_t numberOfBlocks = numberOfBlocksIn(LikelyPart) + numberOfBlocksIn(UnlikelyPart) + numberOfBlocksIn(RarePart);

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
    Block* const* const storage = m_predecessors.data();

    auto listAsPredecessor = [&](Block* block, uint32_t orderIndex, const Edge& edge) ALWAYS_INLINE_LAMBDA -> Block* const* {
        BlockPlace& placeOfTarget = places[edge.block()->index()];
        Block* const* piece = placeOfTarget.predecessors.data();
        size_t numberListed = placeOfTarget.predecessors.size();
        placeOfTarget.predecessors = std::span<Block* const>(piece, numberListed + 1);
        const_cast<Block**>(piece)[numberListed] = block;
        if (placeOfTarget.orderIndex <= orderIndex) [[unlikely]]
            noteEdgeLeadingBack(edge.block(), placeOfTarget, orderIndex);
        return piece + numberListed;
    };

    static_assert(Block::notOrdered == UINT32_MAX);
    uint32_t numberOfBlocksPlaced = 0;
    uint64_t position = 0;
    for (uint32_t index = m_firstBlockInOrder; index != none;) {
        const BlockState& state = states[index];
        uint32_t flags = state.flags;
        Block* block = blocks[index];
        BlockPlace& place = places[index];
        index = state.next;

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
        uint32_t orderIndex = numberOfBlocksPlaced++;
        blocksInOrder[orderIndex] = block;
        blocksInLayout[layoutIndex] = block;
        place.orderIndex = orderIndex;
        place.layoutIndex = layoutIndex;
        place.head = static_cast<Position>(position);

        const Edge* edges = block->successors().data();
        unsigned numberOfSuccessors = block->numSuccessors();
        position += 2 * (static_cast<uint64_t>(block->size()) + 1);
        if (flags & HasUnsplitEdge) [[unlikely]]
            position += positionsOfBlockSplitLate * static_cast<uint64_t>(numberOfSuccessors);

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
    RELEASE_ASSERT(numberOfBlocksPlaced == numberOfBlocks);

    RELEASE_ASSERT(position < noPosition);
    m_end = static_cast<Position>(position);
}

static bool followsLink(const Block* latch, const Block* header)
{
    for (const Operation* upsilon : latch->upsilons()) {
        const Operation* phi = upsilon->phi();
        const Operation* next = upsilon->child(0);
        if (phi && phi->owner() == header && isLoad(next->opcode()) && next->lastChild() == phi)
            return true;
    }
    return false;
}

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

    auto flagsOfPartOf = [&](const Block* block) ALWAYS_INLINE_LAMBDA -> uint32_t {
        return states[block->index()].flags & (IsRare | IsLikely);
    };

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
        bool isSearch = false;
        for (const Block* predecessor : places[header].predecessors) {
            if (isDirectMemberOfLoop(predecessor) && flagsOfPartOf(predecessor) == flagsOfPart && predecessor->last()->isUnconditionalJump()) {
                end = std::max(end, places[predecessor->index()].layoutIndex + 1);
                isSearch |= followsLink(predecessor, head);
            }
        }
        if (end == begin)
            continue;
        if (isSearch) {
            uint32_t numberOfExits = 0;
            for (uint32_t index = begin; index < end && numberOfExits < 2; ++index) {
                const Block* block = blocksInLayout[index];
                if (block != head && !isDirectMemberOfLoop(block))
                    continue;
                for (const Edge& edge : block->successors())
                    numberOfExits += !edge.isRare() && !staysInLoop(edge.block());
            }
            if (numberOfExits >= 2)
                continue;
        }
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
        ++m_numberOfLoopsRotated;
        beginOfMovedBlocks = std::min(beginOfMovedBlocks, begin);
        endOfMovedBlocks = std::max(endOfMovedBlocks, end);
    }
}

void BlockOrderBuilder::findLoops()
{
    std::sort(m_loopHeads.begin(), m_loopHeads.end(), [](const LoopHead& first, const LoopHead& second) {
        return first.firstBlock < second.firstBlock;
    });

    BlockPlace* const places = m_places.begin();
    Block* const* const blocksInOrder = m_blocksInOrder.begin();
    const LoopHead* nextHead = m_loopHeads.begin();
    const LoopHead* const endOfHeads = m_loopHeads.end();
    m_loops.reserve(m_arena, m_loopHeads.size());
    Loop* const loops = m_loops.begin();

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

void BlockOrderBuilder::findLoopsWithSideEntrances()
{
    const BlockPlace* const places = m_places.begin();
    Block* const* const blocksInOrder = m_blocksInOrder.begin();
    Loop* const loops = m_loops.begin();
    uint32_t* const loopAroundThatMayLackMark = m_arena.allocateArray<uint32_t>(m_loops.size());
    for (const Loop& outermost : m_loops) {
        if (outermost.parent != noLoop)
            continue;
        for (uint32_t index = outermost.firstBlock + 1; index < outermost.endBlock; ++index) {
            const BlockPlace& place = places[blocksInOrder[index]->index()];
            uint32_t first = 0;
            uint32_t last = 0;
            if (!place.predecessors.empty()) {
                first = places[place.predecessors.front()->index()].orderIndex;
                last = places[place.predecessors.back()->index()].orderIndex;
            }
            uint32_t innermost = place.isLoopHead ? loops[place.loop].parent : place.loop;
            if (innermost == noLoop || (first >= loops[innermost].firstBlock && last < loops[innermost].endBlock))
                continue;
            for (uint32_t loop = innermost;; loop = loops[loop].parent) {
                while (loop != noLoop && loops[loop].hasSideEntrance) {
                    uint32_t around = loopAroundThatMayLackMark[loop];
                    if (around != noLoop && loops[around].hasSideEntrance)
                        around = loopAroundThatMayLackMark[loop] = loopAroundThatMayLackMark[around];
                    loop = around;
                }
                if (loop == noLoop || (first >= loops[loop].firstBlock && last < loops[loop].endBlock))
                    break;
                loops[loop].hasSideEntrance = true;
                loopAroundThatMayLackMark[loop] = loops[loop].parent;
            }
        }
    }
}

NEVER_INLINE void BlockOrderBuilder::placeLoops()
{
    findLoops();
    findLoopsWithSideEntrances();

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
        if (loop.parent == noLoop)
            continue;
        const Loop& parent = m_loops[loop.parent];
        bool isInRunOfParent = parent.endBlock == loop.endBlock && placeOf(m_blocksInOrder[parent.firstBlock]).rareRegion == placeOf(m_blocksInOrder[loop.firstBlock]).rareRegion;
        loop.loopAroundOutsideRun = isInRunOfParent ? parent.loopAroundOutsideRun : loop.parent;
    }
}

uint32_t BlockOrderBuilder::cycleOf(const Block* block) const
{
    const BlockState& state = m_states[block->index()];
    return state.firstInCycle != none ? block->index() : state.cycleHeader;
}

ALWAYS_INLINE bool BlockOrderBuilder::fallsIntoNext(uint32_t layoutIndex) const
{
    if (layoutIndex + 1 == m_blocksInLayout.size())
        return false;
    const Block* next = m_blocksInLayout[layoutIndex + 1];
    for (const Edge& edge : m_blocksInLayout[layoutIndex]->successors()) {
        if (edge.block() == next)
            return true;
    }
    return false;
}

ALWAYS_INLINE bool BlockOrderBuilder::needsNoFollowingRun(const Block* block, const Block* next)
{
    const Edge* edges = block->successors().data();
    unsigned numberOfSuccessors = block->numSuccessors();
    if (!numberOfSuccessors)
        return true;
    if (edges[0].block() == next && edges[0].edgeClass() == EdgeClass::Normal)
        return true;
    return numberOfSuccessors == 2 && edges[1].block() == next && edges[1].edgeClass() == EdgeClass::Normal;
}

bool BlockOrderBuilder::mayMoveRunAhead(uint32_t beginOfRun, uint32_t cycle) const
{
    static constexpr unsigned mostOperations = 30;
    if (has(m_blocksInLayout[beginOfRun], IsMovedAhead) || fallsIntoNext(beginOfRun - 1))
        return false;
    unsigned numberOfOperations = 0;
    for (uint32_t index = beginOfRun;; ++index) {
        const Block* block = m_blocksInLayout[index];
        numberOfOperations += block->size() + 1;
        if (numberOfOperations > mostOperations)
            return false;
        if (!fallsIntoNext(index))
            return cycleOf(block) == cycle;
        if (m_places[m_blocksInLayout[index + 1]->index()].layoutPart != LikelyPart)
            return false;
    }
}

uint32_t BlockOrderBuilder::findRunToPlaceAfter(uint32_t layoutIndex) const
{
    const Block* block = m_blocksInLayout[layoutIndex];
    uint32_t found = none;
    for (const Edge& edge : block->successors()) {
        const Block* successor = edge.block();
        const BlockPlace& place = m_places[successor->index()];
        if (edge.edgeClass() != EdgeClass::Normal || place.layoutPart != LikelyPart || successor->isLandingPad())
            continue;
        if (place.layoutIndex == layoutIndex + 1)
            return none;
        if (found == none && place.layoutIndex > layoutIndex && cycleOf(successor) == cycleOf(block) && mayMoveRunAhead(place.layoutIndex, cycleOf(block)))
            found = place.layoutIndex;
    }
    return found;
}

void BlockOrderBuilder::moveRunsAhead()
{
    if (m_proc.traits().fallsIntoWhatLiesBehind)
        return;
    const uint32_t numberOfLikelyBlocks = numberOfBlocksIn(LikelyPart);
    Block** const layout = m_blocksInLayout.begin();
    uint32_t firstMoved = none;
    for (uint32_t index = 0; index + 1 < numberOfLikelyBlocks; ++index) {
        if (needsNoFollowingRun(layout[index], layout[index + 1]))
            continue;
        uint32_t beginOfRun = findRunToPlaceAfter(index);
        if (beginOfRun == none)
            continue;
        BlockState& state = m_states[layout[index]->index()];
        state.flags |= TakesRunBehind;
        state.next = beginOfRun;
        m_states[layout[beginOfRun]->index()].flags |= IsMovedAhead;
        firstMoved = std::min(firstMoved, index);
    }
    if (firstMoved == none)
        return;

    Block** const moved = m_arena.allocateArray<Block*>(numberOfLikelyBlocks - firstMoved);
    uint32_t numberPlaced = 0;
    for (uint32_t index = firstMoved; index < numberOfLikelyBlocks; ++index) {
        if (has(layout[index], IsMovedAhead)) {
            while (fallsIntoNext(index) && !has(layout[index], TakesRunBehind))
                ++index;
            continue;
        }
        moved[numberPlaced++] = layout[index];
        for (uint32_t place = index; has(layout[place], TakesRunBehind);) {
            place = m_states[layout[place]->index()].next;
            moved[numberPlaced++] = layout[place];
            while (fallsIntoNext(place) && !has(layout[place], TakesRunBehind))
                moved[numberPlaced++] = layout[++place];
        }
    }
    RELEASE_ASSERT(numberPlaced == numberOfLikelyBlocks - firstMoved);
    for (uint32_t index = 0; index < numberPlaced; ++index) {
        layout[firstMoved + index] = moved[index];
        placeOf(moved[index]).layoutIndex = firstMoved + index;
    }
    m_arena.letGoOfArray(moved, numberOfLikelyBlocks - firstMoved);
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
    moveRunsAhead();
#if ASSERT_ENABLED
    if (!m_someBlockStandsAheadOfItsPredecessors)
        assertOrderHolds();
#endif
}

void BlockOrderBuilder::letGoOfWorkingLists()
{
    if (m_states.capacity() * sizeof(BlockState) < Arena::smallestRoomLetGo)
        return;
    m_loopHeads.letGo(m_arena);
    m_rareLandingPads.letGo(m_arena);
    m_edgesToRareLandingPads.letGo(m_arena);
    if (m_regionPlans)
        m_arena.letGoOfArray(m_regionPlans, m_numberOfRareRegions);
    if (m_blocksOutsideRareRegions) {
        m_arena.letGoOfArray(m_entriesOfRareRegions.data(), m_rareEdgesFromOutside.size());
        m_arena.letGoOfArray(m_blocksOutsideRareRegions, numberOfBlocksIn(LikelyPart) + numberOfBlocksIn(UnlikelyPart));
    }
    m_progressThroughManySuccessors.letGo(m_arena);
    m_edgesThatGetBlocks.letGo(m_arena);
    m_numberOfEdgesFromCountedBlock.letGo(m_arena);
    m_rareEdgesFromOutside.letGo(m_arena);
    m_states.letGo(m_arena);
}

void BlockOrderBuilder::letGoOfOrder()
{
    m_blocksInOrder.letGo(m_arena);
    m_blocksInLayout.letGo(m_arena);
    m_places.letGo(m_arena);
    m_loops.letGo(m_arena);
    m_rareRegions.letGo(m_arena);
    m_arena.letGoOfArray(m_predecessors.data(), m_predecessors.size());
    m_arena.letGoOfArray(m_successorIndexesOfPredecessors.data(), m_successorIndexesOfPredecessors.size());
}

} // anonymous namespace

Order* orderBlocks(Procedure& procedure)
{
    auto takeOrderOf = [&](const BlockOrderBuilder& builder) {
        Order* order = procedure.arena().create<Order>();
        order->m_blocksInOrder = builder.m_blocksInOrder;
        order->m_blocksInLayout = builder.m_blocksInLayout;
        order->m_places = builder.m_places;
        order->m_loops = builder.m_loops;
        order->m_numberOfLoopsRotated = builder.m_numberOfLoopsRotated;
        order->m_rareRegions = builder.m_rareRegions;
        order->m_numberOfBlocksOutsideRareRegions = builder.m_numberOfBlocksOutsideRareRegions;
        order->m_end = builder.m_end;
        order->m_predecessors = builder.m_predecessors;
        order->m_successorIndexesOfPredecessors = builder.m_successorIndexesOfPredecessors;
        order->m_mostBlocks = procedure.numberOfBlocks() + builder.m_numberOfUnsplitEdges;
        order->m_mostOperations = procedure.numberOfOperations() + builder.m_numberOfUnsplitEdges;
        return order;
    };

    if (!procedure.traits().shouldUseFallbackBlockOrder) [[likely]] {
        BlockOrderBuilder builder(procedure, BlockOrderBuilder::WaysDownFromRareBlocks::LeadAnywhere);
        builder.run();
        builder.letGoOfWorkingLists();
        if (!builder.m_someBlockStandsAheadOfItsPredecessors) [[likely]]
            return takeOrderOf(builder);
        builder.letGoOfOrder();
    }
    procedure.noteThatOrderWasMadeAgain();

    BlockOrderBuilder builder(procedure, BlockOrderBuilder::WaysDownFromRareBlocks::StayAmongRareBlocks);
    builder.run();
    builder.letGoOfWorkingLists();
    if (builder.m_someBlockStandsAheadOfItsPredecessors)
        procedure.fail(FailureKind::MalformedProcedure, nullptr);
    return takeOrderOf(builder);
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

    Block* middle = split(from, successorIndex, edge.isRare() && order.of(target).isRare() ? EdgeClass::Normal : edge.edgeClass());
    RELEASE_ASSERT(middle->index() < order.m_mostBlocks && middle->last()->index() < order.m_mostOperations);

    const BlockPlace& placeOfFrom = order.of(from);
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
    auto sortNewBlocksBy = [&](const auto& keyOf) {
        if (!std::ranges::is_sorted(newBlocks, { }, keyOf))
            AOT::stableSort(newBlocks, { }, keyOf);
    };

    sortNewBlocksBy(orderIndexOf);

    uint32_t numberOfBlocksSoFar = order.m_blocksInOrder.size();
    uint32_t numberOfBlocks = numberOfBlocksSoFar + static_cast<uint32_t>(newBlocks.size());

    auto newIndexOf = [&](uint32_t index) {
        return index + static_cast<uint32_t>(std::ranges::lower_bound(newBlocks, index, { }, orderIndexOf) - newBlocks.begin());
    };
    for (Loop& loop : order.m_loops) {
        loop.firstBlock = newIndexOf(loop.firstBlock);
        loop.endBlock = newIndexOf(loop.endBlock);
    }

    ArenaVector<Block*> listOfBlocksInOrder;
    listOfBlocksInOrder.growWithoutInitializing(arena, numberOfBlocks);
    Block** const blocksInOrder = listOfBlocksInOrder.begin();
    Block* const* const blocksInOrderSoFar = order.m_blocksInOrder.begin();
    Block* const* nextNewBlock = firstNewBlock;
    uint32_t orderIndexOfNextSource = orderIndexOf(*nextNewBlock);
    RELEASE_ASSERT(orderIndexOfNextSource < numberOfBlocksSoFar);
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
    uint32_t* const layoutIndexesOfSources = arena.allocateArray<uint32_t>(newBlocks.size());
    const uint32_t* const endOfLayoutIndexesOfSources = layoutIndexesOfSources + newBlocks.size();
    for (size_t index = 0; index < newBlocks.size(); ++index)
        layoutIndexesOfSources[index] = layoutIndexOfSourceOf(firstNewBlock[index]);

    ArenaVector<Block*> listOfBlocksInLayout;
    listOfBlocksInLayout.growWithoutInitializing(arena, numberOfBlocks);
    Block** const blocksInLayout = listOfBlocksInLayout.begin();
    Block* const* const blocksInLayoutSoFar = order.m_blocksInLayout.begin();
    uint32_t newLayoutIndex = 0;
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
