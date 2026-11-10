/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTSweep.h"
#include "AOTThinSummary.h"
#include "AOTWalkCopies.h"
#include "AOTWalkGeneratorsInLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkLoopsOverGeneratorsBuilder.h"
#include "AOTWalkTextBuilders.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t mostTurnsWrittenOut = 8;
static constexpr uint32_t mostOperationsWalkedForTurnsWrittenOut = 256;
static constexpr uint32_t operationsThatAccessToListOutOfSightBecomes = 14;

static constexpr OpcodeSet relationsOfCountersWrittenOut { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
static constexpr OpcodeSet stepsOfCountersWrittenOut { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
static constexpr OpcodeSet argumentsObjectsOfTurnsWrittenOut { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };

namespace {

struct IntegersByText {
    const BodyIndex& index;
    PropertyKeyID keyOfLength;
    const WTF::Function<std::optional<int64_t>(ValueID)>& leaf;
    const WTF::Function<std::optional<int64_t>(ValueID)>& lengthOf;

    std::optional<int64_t> at(ValueID written, unsigned depth = 0) const
    {
        constexpr unsigned deepestSum = 4;
        ValueID value = written ? index.stripCasts(written) : ValueID();
        if (!value || depth > deepestSum)
            return std::nullopt;
        if (std::optional<int64_t> known = leaf(value))
            return known;
        if (index.isParameter(value))
            return std::nullopt;
        BodyValue operation = index.value(value);
        switch (operation.opcode()) {
        case Opcode::Integer:
            return operation.integer();
        case Opcode::PropGet:
            if (operation.isFreeName() || operation.key() != keyOfLength)
                return std::nullopt;
            return lengthOf(index.stripCasts(operation.operand(0)));
        case Opcode::Increment:
        case Opcode::Decrement: {
            std::optional<int64_t> stepped = at(operation.operand(0), depth + 1);
            if (!stepped)
                return std::nullopt;
            return *stepped + (operation.opcode() == Opcode::Increment ? 1 : -1);
        }
        case Opcode::Add:
        case Opcode::Sub: {
            std::optional<int64_t> left = at(operation.operand(0), depth + 1);
            std::optional<int64_t> right = left ? at(operation.operand(1), depth + 1) : std::nullopt;
            if (!right || !Range::int32().contains(*left) || !Range::int32().contains(*right))
                return std::nullopt;
            return operation.opcode() == Opcode::Add ? *left + *right : *left - *right;
        }
        default:
            return std::nullopt;
        }
    }
};

} // anonymous namespace

static uint32_t numberOfOperationsOfLoopWrittenOut(const BodyIndex& index, LoopID loop)
{
    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        numberOfOperations += index.block(member).numberOfValues() + 1;
        if (numberOfOperations > mostOperationsWalkedForTurnsWrittenOut)
            break;
    }
    return numberOfOperations;
}

static std::optional<uint32_t> foreseeTurnsByText(const IntegersByText& integers, LoopID loop, const WTF::Function<std::optional<int64_t>(uint32_t)>& entering)
{
    const BodyIndex& index = integers.index;
    BodyBlock head = index.block(index.loop(loop).head);
    BodyTerminator end = head.terminator();
    if (!end.isConditionalBranch() || index.isInLoop(end.taken().block(), loop) == index.isInLoop(end.notTaken().block(), loop))
        return std::nullopt;
    bool staysOnTakenEdge = index.isInLoop(end.taken().block(), loop);

    ValueID asked = index.stripCasts(end.operand(0));
    if (!asked || index.isParameter(asked) || !relationsOfCountersWrittenOut.contains(index.opcodeOf(asked)) || !head.holds(asked))
        return std::nullopt;
    BodyValue relation = index.value(asked);
    std::optional<uint32_t> left = head.indexOfParameter(index.stripCasts(relation.operand(0)));
    std::optional<uint32_t> right = head.indexOfParameter(index.stripCasts(relation.operand(1)));
    if (!left == !right)
        return std::nullopt;
    uint32_t position = left ? *left : *right;
    ValueID counter = head.parameter(position);
    std::optional<int64_t> bound = integers.at(relation.operand(left ? 1 : 0));
    std::optional<int64_t> start = entering(position);
    if (!bound || !start || !Range::int32().contains(*bound) || !Range::int32().contains(*start))
        return std::nullopt;

    std::optional<int64_t> step;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != head.id())
                continue;
            ValueID brought = position < target.arguments().size() ? index.stripCasts(target.arguments()[position]) : ValueID();
            if (!brought || index.isParameter(brought) || !stepsOfCountersWrittenOut.contains(index.opcodeOf(brought)))
                return std::nullopt;
            BodyValue stepped = index.value(brought);
            if (index.stripCasts(stepped.operand(0)) != counter)
                return std::nullopt;
            std::optional<int64_t> by;
            switch (stepped.opcode()) {
            case Opcode::Increment:
                by = 1;
                break;
            case Opcode::Decrement:
                by = -1;
                break;
            default:
                by = integers.at(stepped.operand(1));
                if (by && stepped.opcode() == Opcode::Sub)
                    by = -*by;
            }
            if (!by || !*by || !Range::int32().contains(*by) || (step && *step != *by))
                return std::nullopt;
            step = by;
        }
    }
    if (!step)
        return std::nullopt;

    auto holds = [&](int64_t value) {
        int64_t first = left ? value : *bound;
        int64_t second = left ? *bound : value;
        bool answer = false;
        switch (relation.opcode()) {
        case Opcode::Less:
            answer = first < second;
            break;
        case Opcode::LessEq:
            answer = first <= second;
            break;
        case Opcode::Greater:
            answer = first > second;
            break;
        default:
            answer = first >= second;
        }
        return answer == staysOnTakenEdge;
    };
    uint32_t stays = 0;
    for (int64_t value = *start; holds(value); value += *step) {
        if (++stays > mostTurnsWrittenOut)
            return std::nullopt;
    }
    return stays;
}

static bool textAdmitsTurnsWrittenOut(const BodyIndex& index, BodyLiveness& liveness, LoopID loop, bool isCopy, NOESCAPE const Invocable<std::span<const uint32_t>()> auto& numbersOfHandlersThatRead, ValueID openSteppedByIndex = { }, bool loopsWithinAreWrittenOut = false)
{
    BlockID head = index.loop(loop).head;
    if (index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(head) || (index.loop(loop).numberOfLoopsWithin != 1 && !loopsWithinAreWrittenOut))
        return false;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        TerminatorKind end = block.terminator().kind();
        if (isCopy && (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall))
            return false;
        if (block.isHandlerBlock() || (block.hasHandler() && (!openSteppedByIndex || iteratorClosedLeavingFunction(index, block) != openSteppedByIndex)))
            return false;
        for (BodyValue operation : block.values()) {
            if (maySuspendAsWritten(operation))
                return false;
        }
    }
    if (!liveness.isComputedFor(index))
        liveness.compute(index);
    std::span<const uint32_t> numbers = numbersOfHandlersThatRead();
    if (numbers.empty())
        return true;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value) {
            if (numbers[value])
                return false;
        }
    }
    return true;
}

static std::optional<int64_t> integerBuiltForTurnsWrittenOut(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isConstant() || walker.code().at(reference.id()).opcode != Opcode::Integer)
        return std::nullopt;
    return walker.code().at(reference.id()).integer();
}

static bool bodyOnlyReadsListOfCall(const Walker& walker, const BodyIndex& index, PropertyKeyID keyOfLength, ValueID list)
{
    uint32_t mentionsLeft = index.useCount(list);
    if (mentionsLeft == BodyIndex::maxCountedUses)
        return false;
    for (uint32_t next = list.index() + 1; mentionsLeft && next < index.numberOfValues(); ++next) {
        if (index.isParameter(ValueID(next)))
            continue;
        BodyValue user = index.value(ValueID(next));
        uint32_t mentions = 0;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += operand == list;
        });
        if (!mentions)
            continue;
        mentionsLeft -= mentions;
        Opcode opcode = user.opcode();
        if (opcode == Opcode::IteratorOpen && openOverListAsMadeGoesByIndex(const_cast<Walker&>(walker), index, user.id()))
            continue;
        if ((opcode == Opcode::PropGet && !user.isFreeName() && user.key() == keyOfLength) || (opcode == Opcode::ElemGet && user.propertyOperand() != list))
            continue;
        return false;
    }
    return !mentionsLeft;
}

std::optional<TextOfCopyRead> turnsOfLoopsOfCopyWrittenOut(const Walker& walker, FunctionID callee, std::span<const std::optional<int64_t>> integersHandedOver, std::span<const std::optional<uint32_t>> lengthsOfListsHandedOver)
{
    const BodyIndex* found = walker.facts().bodies().find(callee);
    if (!found || !found->hasLoops() || found->hasIrreducibleFlow() || walker.facts().summaries().at(callee).numberOfOperations > mostOperationsWalkedForTurnsWrittenOut)
        return std::nullopt;
    BodyLiveness liveness;
    Vector<uint32_t, 2> turns;
    const BodyIndex& index = *found;
    PropertyKeyID keyOfLength = walker.program().keyOf(BuiltinNames::length);
    BodyBlock entry = index.block(entryBlock);

    Vector<std::optional<int64_t>> known;
    Vector<std::optional<bool>> answers;
    known.fill(std::nullopt, index.numberOfValues());
    answers.fill(std::nullopt, index.numberOfValues());
    BitVector isEntered(index.numberOfBlocks());
    BitVector leadsToTakenAlone(index.numberOfBlocks());
    BitVector leadsToNotTakenAlone(index.numberOfBlocks());
    isEntered.set(entryBlock.index());
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters() && parameter < integersHandedOver.size(); ++parameter)
        known[entry.parameter(parameter).index()] = integersHandedOver[parameter];

    WTF::Function<std::optional<int64_t>(ValueID)> leaf = [&](ValueID value) -> std::optional<int64_t> {
        return known[value.index()];
    };
    WTF::Function<std::optional<int64_t>(ValueID)> lengthOf = [&](ValueID list) -> std::optional<int64_t> {
        if (!list || !bodyOnlyReadsListOfCall(walker, index, keyOfLength, list))
            return std::nullopt;
        if (std::optional<uint32_t> parameter = entry.indexOfParameter(list)) {
            if (*parameter >= lengthsOfListsHandedOver.size() || !lengthsOfListsHandedOver[*parameter])
                return std::nullopt;
            return *lengthsOfListsHandedOver[*parameter];
        }
        if (index.isParameter(list) || !argumentsObjectsOfTurnsWrittenOut.contains(index.opcodeOf(list)))
            return std::nullopt;
        ValueID count = index.stripCasts(index.value(list).countOperand());
        return count ? known[count.index()] : std::nullopt;
    };
    IntegersByText integers { index, keyOfLength, leaf, lengthOf };

    auto answerAt = [&](ValueID written) -> std::optional<bool> {
        bool isNegated = false;
        for (ValueID value = index.stripCasts(written); value && !index.isParameter(value); value = index.stripCasts(index.value(value).operand(0))) {
            Opcode opcode = index.opcodeOf(value);
            if (opcode == Opcode::LogicalNot)
                isNegated = !isNegated;
            else if (opcode != Opcode::ToBoolean)
                return answers[value.index()] ? std::optional(*answers[value.index()] != isNegated) : std::nullopt;
        }
        return std::nullopt;
    };

    Vector<BlockID, 4> handlerBlocks;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (index.block(BlockID(number)).isHandlerBlock())
            handlerBlocks.append(BlockID(number));
    }
    Vector<uint32_t> numbersByValue;
    auto numbersOfHandlers = [&]() -> std::span<const uint32_t> {
        if (numbersByValue.isEmpty() && !handlerBlocks.isEmpty()) {
            numbersByValue.fill(0, index.numberOfValues());
            liveness.countHeadsWhereLive(handlerBlocks.span(), numbersByValue.mutableSpan());
        }
        return numbersByValue.span();
    };
    Vector<ValueID, 2> opensSteppedByIndex;
    uint32_t numberOfOperations = walker.facts().summaries().at(callee).numberOfOperations;
    turns.fill(0, index.numberOfLoops());
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!isEntered.get(number))
            continue;
        BodyBlock block = index.block(BlockID(number));
        LoopID loop = index.loopOf(block.id());
        if (loop && index.loop(loop).head == block.id()) {
            std::optional<ForOfByText> forOf = findForOfSteppedAtHead(index, loop);
            if (index.loop(loop).parent || !textAdmitsTurnsWrittenOut(index, liveness, loop, true, numbersOfHandlers, forOf ? forOf->open : ValueID()))
                return std::nullopt;
            Vector<std::optional<int64_t>, 8> entering;
            for (uint32_t parameter = 0; parameter < block.numberOfParameters(); ++parameter)
                entering.append(std::exchange(known[block.parameter(parameter).index()], std::nullopt));
            WTF::Function<std::optional<int64_t>(uint32_t)> whatEnters = [&](uint32_t position) -> std::optional<int64_t> {
                return position < entering.size() ? entering[position] : std::nullopt;
            };
            std::optional<uint32_t> stays = forOf ? std::nullopt : foreseeTurnsByText(integers, loop, whatEnters);
            ValueID iterated = forOf ? index.stripCasts(index.value(forOf->open).operand(0)) : ValueID();
            if (std::optional<int64_t> length = iterated && entry.indexOfParameter(iterated) ? lengthOf(iterated) : std::nullopt; length && *length <= mostTurnsWrittenOut) {
                stays = static_cast<uint32_t>(*length);
                opensSteppedByIndex.append(forOf->open);
            }
            if (!stays)
                return std::nullopt;
            numberOfOperations += numberOfOperationsOfLoopWrittenOut(index, loop) * *stays;
            turns[loop.index()] = *stays;
        }

        for (BodyValue operation : block.values()) {
            if (index.stripCasts(operation.id()) != operation.id())
                continue;
            known[operation.id().index()] = integers.at(operation.id());
            if (!relationsOfCountersWrittenOut.contains(operation.opcode()) && operation.opcode() != Opcode::StrictEq && operation.opcode() != Opcode::Eq)
                continue;
            std::optional<int64_t> left = integers.at(operation.operand(0));
            std::optional<int64_t> right = left ? integers.at(operation.operand(1)) : std::nullopt;
            if (!right)
                continue;
            switch (operation.opcode()) {
            case Opcode::Less:
                answers[operation.id().index()] = *left < *right;
                break;
            case Opcode::LessEq:
                answers[operation.id().index()] = *left <= *right;
                break;
            case Opcode::Greater:
                answers[operation.id().index()] = *left > *right;
                break;
            case Opcode::GreaterEq:
                answers[operation.id().index()] = *left >= *right;
                break;
            default:
                answers[operation.id().index()] = *left == *right;
            }
        }

        auto tell = [&](BodyTarget target) {
            if (target.block().index() <= number)
                return;
            BodyBlock to = index.block(target.block());
            bool isFirst = !isEntered.get(to.id().index());
            isEntered.set(to.id().index());
            for (uint32_t position = 0; position < to.numberOfParameters(); ++position) {
                std::optional<int64_t> brought = position < target.arguments().size() ? integers.at(target.arguments()[position]) : std::nullopt;
                std::optional<int64_t>& held = known[to.parameter(position).index()];
                if (isFirst)
                    held = brought;
                else if (held != brought)
                    held = std::nullopt;
            }
        };
        if (block.hasHandler() && !opensSteppedByIndex.contains(iteratorClosedLeavingFunction(index, block)))
            tell(block.handler());
        BodyTerminator end = block.terminator();
        std::optional<bool> answer = !loop && end.isConditionalBranch() ? answerAt(end.operand(0)) : std::nullopt;
        if (answer) {
            (*answer ? leadsToTakenAlone : leadsToNotTakenAlone).set(number);
            tell(*answer ? end.taken() : end.notTaken());
        } else {
            for (BodyTarget target : end.targets())
                tell(target);
        }
    }
    if (numberOfOperations > mostOperationsWalkedForTurnsWrittenOut)
        return std::nullopt;
    return TextOfCopyRead { WTF::move(turns), WTF::move(isEntered), WTF::move(leadsToTakenAlone), WTF::move(leadsToNotTakenAlone), numberOfOperations };
}

static constexpr OpcodeSet accessesOfListsInTurnsWrittenOut { Opcode::ElemGet, Opcode::ElemSet };

uint32_t sizeOfCopyThatWritesOutLoopsOverListsHandedIn(const Walker& walker, FunctionID callee)
{
    const BodyIndex* body = walker.facts().bodies().find(callee);
    static constexpr uint32_t mostOperationsOfCallee = 40;
    if (!body || !body->hasLoops() || !body->holdsAnyOf(accessesOfListsInTurnsWrittenOut) || walker.facts().summaries().at(callee).numberOfOperations > mostOperationsOfCallee)
        return 0;
    bool readsListHandedIn = false;
    for (uint32_t number = 0; number < body->numberOfBlocks() && !readsListHandedIn; ++number) {
        for (BodyValue operation : body->block(BlockID(number)).values()) {
            ValueID list = accessesOfListsInTurnsWrittenOut.contains(operation.opcode()) ? body->stripCasts(operation.objectOperand()) : ValueID();
            readsListHandedIn |= list && body->isParameter(list) && body->blockOf(list) == entryBlock;
        }
    }
    if (!readsListHandedIn)
        return 0;
    Vector<std::optional<int64_t>, 8> noIntegers;
    Vector<std::optional<uint32_t>, 8> noLengths;
    noIntegers.fill(std::nullopt, walker.program().at(callee).numberOfEntryParameters());
    noLengths.fill(std::nullopt, walker.program().at(callee).numberOfEntryParameters());
    std::optional<TextOfCopyRead> text = turnsOfLoopsOfCopyWrittenOut(walker, callee, noIntegers.span(), noLengths.span());
    return text ? text->numberOfOperations : 0;
}

static uint32_t numberOfOperationsOfCopiesInTurnWrittenOut(const Walker& walker, const BodyIndex& index, LoopID loop)
{
    uint32_t numberOfOperations = 0;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            FunctionID callee = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : FunctionID();
            uint32_t size = callee ? sizeOfCopyThatWritesOutLoopsOverListsHandedIn(walker, callee) : 0;
            if (size && operationsFitWindowOfCallWrittenInLoop(walker, size))
                numberOfOperations += size;
        }
    }
    return numberOfOperations;
}
static std::optional<uint32_t> numberOfOperationsOfLoopsWrittenOut(const Walker& walker, const IntegersByText& integers, LoopID loop, uint32_t stays, bool countsCopies)
{
    const BodyIndex& index = integers.index;
    auto walkedOnce = [&](LoopID walked) {
        return numberOfOperationsOfLoopWrittenOut(index, walked) + (countsCopies ? numberOfOperationsOfCopiesInTurnWrittenOut(walker, index, walked) : 0);
    };
    uint32_t numberOfOperationsOfTurn = walkedOnce(loop);
    uint32_t numberOfLoopsLeft = index.loop(loop).numberOfLoopsWithin - 1;
    for (uint32_t number = loop.index() + 1; numberOfLoopsLeft && number < index.numberOfLoops(); ++number) {
        LoopID within(number);
        if (index.loop(within).parent != loop)
            continue;
        numberOfLoopsLeft -= index.loop(within).numberOfLoopsWithin;
        WTF::Function<std::optional<int64_t>(uint32_t)> whatEnters = [&](uint32_t position) -> std::optional<int64_t> {
            std::optional<int64_t> entering;
            for (BlockID member : index.membersOf(loop)) {
                if (index.isInLoop(member, within))
                    continue;
                for (BodyTarget target : index.terminator(member).targets()) {
                    if (target.block() != index.loop(within).head)
                        continue;
                    std::optional<int64_t> brought = position < target.arguments().size() ? integers.at(target.arguments()[position]) : std::nullopt;
                    if (!brought || (entering && *entering != *brought))
                        return std::nullopt;
                    entering = brought;
                }
            }
            return entering;
        };
        std::optional<uint32_t> staysWithin = foreseeTurnsByText(integers, within, whatEnters);
        std::optional<uint32_t> ofLoopWithin = staysWithin ? numberOfOperationsOfLoopsWrittenOut(walker, integers, within, *staysWithin, countsCopies) : std::nullopt;
        if (!ofLoopWithin)
            return std::nullopt;
        numberOfOperationsOfTurn += *ofLoopWithin - walkedOnce(within);
    }
    uint32_t numberOfOperations = stays * numberOfOperationsOfTurn + index.block(index.loop(loop).head).numberOfValues() + 1;
    return numberOfOperations > mostOperationsWalkedForTurnsWrittenOut ? std::nullopt : std::optional(numberOfOperations);
}

bool Walker::writeOutTurnsOfLoop(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Folding) || !has(Mechanism::LoopInvariants) || isUnreachable() || !index.isLoopHead(inputHead))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingVersioned.contains(loop))
        return false;
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) || hasStringsInBuilders())
        return false;

    if (numberOfOperationsOfLoopWrittenOut(index, loop) > mostOperationsWalkedForTurnsWrittenOut)
        return false;

    PropertyKeyID keyOfLength = program().keyOf(BuiltinNames::length);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    auto isDefinedInLoop = [&](ValueID value) {
        return value.index() >= firstValue && index.isInLoop(index.blockOf(value), loop);
    };
    WTF::Function<std::optional<int64_t>(ValueID)> leaf = [&](ValueID value) -> std::optional<int64_t> {
        return isDefinedInLoop(value) ? std::nullopt : integerBuiltForTurnsWrittenOut(*this, referenceBoundTo(value));
    };
    WTF::Function<std::optional<int64_t>(ValueID)> lengthOf = [&](ValueID list) -> std::optional<int64_t> {
        Reference made = list && !isDefinedInLoop(list) ? referenceBoundTo(list) : Reference();
        std::optional<uint32_t> length = made ? lengthOfFreshList(*this, made) : std::nullopt;
        return length ? std::optional<int64_t>(*length) : std::nullopt;
    };
    WTF::Function<std::optional<int64_t>(uint32_t)> whatEnters = [&](uint32_t position) -> std::optional<int64_t> {
        return position < entering.size() ? integerBuiltForTurnsWrittenOut(*this, entering[position]) : std::nullopt;
    };
    IntegersByText integers { index, keyOfLength, leaf, lengthOf };
    std::optional<uint32_t> stays = foreseeTurnsByText(integers, loop, whatEnters);
    if (!stays)
        return false;
    bool holdsLoops = index.loop(loop).numberOfLoopsWithin != 1;
    bool standsInLoop = m_frames[frameIndex].loopDepthAtCall || index.loopDepthOf(inputHead) >= 2;
    if (holdsLoops && (!standsInLoop || !numberOfOperationsOfLoopsWrittenOut(*this, integers, loop, *stays, !frameIndex)))
        return false;
    uint32_t numberOfOperationsOfTurn = numberOfOperationsOfLoopWrittenOut(index, loop) + (frameIndex ? 0 : numberOfOperationsOfCopiesInTurnWrittenOut(*this, index, loop));
    BlockID headAround = m_frames[frameIndex].fullyUnrolledHeader;
    bool isReckonedWithNest = !holdsLoops && headAround && index.loop(loop).parent == index.loopOf(headAround) && numberOfOperationsOfTurn * (*stays + 1) > mostOperationsWalkedForTurnsWrittenOut;
    if (!holdsLoops && !isReckonedWithNest && numberOfOperationsOfTurn * (*stays + 1) > mostOperationsWalkedForTurnsWrittenOut)
        return false;
    if (!m_frames[frameIndex].loopDepthAtCall && index.loopDepthOf(inputHead) < 2) {
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                FunctionID callee = operation.opcode() == Opcode::Call ? facts().linked().calleeAt(currentFunction(), operation.id()) : operation.opcode() == Opcode::CallDirect && !operation.isConstruction() ? operation.function() : FunctionID();
                if (callee && calleeIsCopiedInLoopsAlone(*this, callee, operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().argumentsOperands().size() : operation.numberOfVariadicOperands())) {
                    count(StatisticID::FullUnrollingsRejectedForSize);
                    return false;
                }
                if (operation.opcode() != Opcode::ElemGet && operation.opcode() != Opcode::ElemSet)
                    continue;
                ValueID object = index.stripCasts(operation.objectOperand());
                Reference list = object && !isDefinedInLoop(object) ? referenceBoundTo(object) : Reference();
                ValueID cell = list && list.isValue() ? underlyingCellOf(m_code, list.id()) : ValueID();
                bool isInSight = (list && list.isConstant()) || (cell && (m_code.isConstant(cell) || m_state.isFresh(Reference::value(cell)))) || (list && findHoistedArrayGuard(*this, list));
                if (!isInSight)
                    numberOfOperationsOfTurn += operationsThatAccessToListOutOfSightBecomes;
            }
        }
        if (numberOfOperationsOfTurn * (*stays + 1) > mostOperationsWalkedForTurnsWrittenOut) {
            count(StatisticID::FullUnrollingsRejectedForSize);
            return false;
        }
    }

    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, [&] { return numbersOfHandlersThatRead(m_frames.last()); }, { }, holdsLoops))
        return false;

    beginTurnsWrittenOut(inputHead);
    if (frameIndex)
        count(StatisticID::InlineeLoopsFullyUnrolled);
    if (holdsLoops)
        count(StatisticID::LoopNestsFullyUnrolled);
    if (isReckonedWithNest)
        count(StatisticID::InnerLoopsUnrolledWithNest);
    walkTurnsWrittenOut(inputHead, entering, *stays + 1, nullptr);
    return true;
}

std::span<const BlockID> Walker::handlerBlocksOf(Frame& frame)
{
    if (!frame.handlerBlocks) {
        frame.handlerBlocks.emplace();
        for (uint32_t number = 0; number < frame.index->numberOfBlocks(); ++number) {
            if (frame.index->block(BlockID(number)).isHandlerBlock())
                frame.handlerBlocks->append(BlockID(number));
        }
    }
    return frame.handlerBlocks->span();
}

void Walker::beginTurnsWrittenOut(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    ValueID endOfValues = index.block(index.loop(loop).last).endOfValues();
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    Vector<ValueID, 8> readBehindLoop;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (index.isInLoop(target.block(), loop))
                continue;
            m_liveness.forEachLiveAtHead(target.block(), ValueID(firstValue), endOfValues, [&](ValueID live) {
                if (index.isInLoop(index.blockOf(live), loop) && !readBehindLoop.contains(live))
                    readBehindLoop.append(live);
            });
        }
    }
    m_frames[frameIndex].valuesListedForTurnsWrittenOut.shrink(0);
    for (ValueID value : readBehindLoop) {
        if (m_frames[frameIndex].addDuplicatedValue(value))
            m_frames[frameIndex].valuesListedForTurnsWrittenOut.append(value);
    }
    m_frames[frameIndex].loopsBeingVersioned.append(loop);
    m_frames[frameIndex].isInPreheader = false;
}

void Walker::leadIntoLoopAsWrittenBehindTurns(TurnsOfForInWrittenOut& turns, BlockID inputHead, std::span<const Reference> arriving, Reference position)
{
    size_t frameIndex = m_frames.size() - 1;
    BlockID from = m_block;
    m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
    InputBlock& record = inputBlock(frameIndex, inputHead);
    bool wasEntered = std::exchange(record.isEntered, false);
    uint32_t place = addOpenEdge(frameIndex, inputHead);
    inputBlock(frameIndex, inputHead).isEntered = wasEntered;
    if (place != noOpenEdge) {
        OpenEdge& open = m_openEdges[place];
        inputBlock(frameIndex, inputHead).firstOpenEdge = std::exchange(open.next, noOpenEdge);
        open.edge = m_code.at(from).terminator.next();
        for (Reference argument : arriving) {
            open.arguments.append(argument);
            open.types.append(ownTypeOf(argument));
        }
        open.state = WTF::move(m_state);
        takeNamesOntoEdge(open, frameIndex);
        open.names.setNameIn(m_frames[frameIndex].ensureDuplicatedValues().ensureSlotOf(turns.step), position);
        turns.edgesIntoLoopAsWritten.append(place);
    }
    m_block = { };
    m_state.clear();
}

void Walker::walkTurnsWrittenOut(BlockID inputHead, std::span<const Reference> entering, uint32_t numberOfWalks, TurnsOfForInWrittenOut* forIn)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    uint32_t narrowerNamesBeforeLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;
    if (!m_frames[frameIndex].loopDepthAtCall && index.loopDepthOf(inputHead) < 2)
        count(StatisticID::OutermostLoopsFullyUnrolled);
    Vector<ValueID, 4> listedForTheseTurns = std::exchange(m_frames[frameIndex].valuesListedForTurnsWrittenOut, { });

    auto leadIntoFirstTurn = [&] {
        BlockID from = m_block;
        inputBlock(frameIndex, inputHead) = InputBlock { };
        if (from)
            m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
        uint32_t place = from ? addOpenEdge(frameIndex, inputHead) : noOpenEdge;
        if (place != noOpenEdge) {
            OpenEdge& open = m_openEdges[place];
            open.edge = m_code.at(from).terminator.next();
            for (Reference argument : entering) {
                open.arguments.append(argument);
                open.types.append(ownTypeOf(argument));
            }
            open.state = WTF::move(m_state);
            takeNamesOntoEdge(open, frameIndex);
        }
        m_block = { };
        m_state.clear();
    };

    const bool standsInTry = !m_trialVisits.isEmpty() && m_trialVisits.last().frameIndex == frameIndex;
    const size_t placeOfTryAround = m_trialVisits.size() - standsInTry;
    const size_t firstTypeLeft = standsInTry ? m_trialVisits.last().typesLeftByTurnsWrittenOut.size() : 0;
    auto emptyName = [&](uint32_t value) {
        Reference bound = std::exchange(m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::TakesNamesOfTurnWrittenOut>(value), Reference());
        TypeID type = standsInTry && bound ? typeOfRecordForLooks(*this, ownTypeOf(bound)) : TypeID();
        if (!type)
            return;
        auto& typesLeft = m_trialVisits[placeOfTryAround].typesLeftByTurnsWrittenOut;
        auto ofTheseTurns = typesLeft.mutableSpan().subspan(firstTypeLeft);
        auto found = std::ranges::find(ofTheseTurns, value, &TrialVisit::TypeLeftByTurnsWrittenOut::value);
        if (found != ofTheseTurns.end())
            found->type = m_types.unionOf(found->type, type);
        else
            typesLeft.append(TrialVisit::TypeLeftByTurnsWrittenOut { value, type });
    };
    auto beginWalkOfLoop = [&] {
        uint32_t cameRound = inputBlock(frameIndex, inputHead).firstOpenEdge;
        for (BlockID member : index.membersOf(loop)) {
            inputBlock(frameIndex, member) = InputBlock { };
            BodyBlock block = index.block(member);
            for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
                emptyName(value);
        }
        inputBlock(frameIndex, inputHead).firstOpenEdge = cameRound;
        inputBlock(frameIndex, inputHead).firstNarrowerName = narrowerNamesBeforeLoop;
    };

    size_t numberOfLoopsAround = m_openLoops.size();
    BlockID headBefore = std::exchange(m_frames[frameIndex].fullyUnrolledHeader, inputHead);
    TurnsOfForInWrittenOut* forInBefore = std::exchange(m_frames[frameIndex].turnsOfForIn, forIn);
    auto walkTurns = [&](uint32_t numberOfTurns) {
        leadIntoFirstTurn();
        for (uint32_t turn = 0; turn < numberOfTurns && !m_hasFailed && inputBlock(frameIndex, inputHead).firstOpenEdge != noOpenEdge; ++turn) {
            beginWalkOfLoop();
            for (BlockID member : index.membersOf(loop)) {
                if (m_hasFailed)
                    break;
                walkBlock(member);
            }
            endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
        }
    };
    auto letWhatCameRoundWaitForLoopAsWritten = [&] {
        uint32_t slotOfStep = m_frames[frameIndex].ensureDuplicatedValues().ensureSlotOf(forIn->step);
        uint32_t cameRound = std::exchange(inputBlock(frameIndex, inputHead).firstOpenEdge, noOpenEdge);
        while (cameRound != noOpenEdge) {
            m_openEdges[cameRound].names.setNameIn(slotOfStep, m_indexedIterations[forIn->place].position);
            forIn->edgesIntoLoopAsWritten.append(cameRound);
            cameRound = std::exchange(m_openEdges[cameRound].next, noOpenEdge);
        }
    };
    if (!forIn || forIn->arms.isEmpty())
        walkTurns(numberOfWalks);
    else {
        OpenEdge inFrontOfArms;
        takeNamesOntoEdge(inFrontOfArms, frameIndex);
        for (TurnsOfForInWrittenOut::Arm& arm : forIn->arms) {
            if (m_hasFailed)
                break;
            if (!arm.whereTestsPassed)
                continue;
            if (Frame& frame = m_frames[frameIndex]; frame.duplicatedValues) {
                forgetNamesOfWay(frame);
                frame.duplicatedValues->namesOfWay = inFrontOfArms.names;
                frame.duplicatedValues->blockThatWayEnters = inputHead;
            }
            m_block = arm.whereTestsPassed;
            m_state = WTF::move(arm.stateWhereTestsPassed);
            forIn->object = arm.object;
            forIn->enumeratorIsAsked = false;
            forIn->numberOfNames = static_cast<uint32_t>(arm.names.size());
            m_indexedIterations[forIn->place].position = forIn->firstPosition;
            m_indexedIterations[forIn->place].precomputedSteps = arm.names;
            walkTurns(forIn->numberOfNames);
            if (!m_hasFailed)
                letWhatCameRoundWaitForLoopAsWritten();
        }
    }
    m_frames[frameIndex].fullyUnrolledHeader = headBefore;
    m_frames[frameIndex].turnsOfForIn = forInBefore;

    if (forIn && !m_hasFailed) {
        IndexedIteration& iteration = m_indexedIterations[forIn->place];
        uint32_t slotOfStep = m_frames[frameIndex].ensureDuplicatedValues().ensureSlotOf(forIn->step);
        for (uint32_t cameRound = inputBlock(frameIndex, inputHead).firstOpenEdge; cameRound != noOpenEdge; cameRound = m_openEdges[cameRound].next)
            m_openEdges[cameRound].names.setNameIn(slotOfStep, iteration.position);
        for (uint32_t waiting : forIn->edgesIntoLoopAsWritten)
            m_openEdges[waiting].next = std::exchange(inputBlock(frameIndex, inputHead).firstOpenEdge, waiting);
        iteration.precomputedSteps.shrink(0);
        m_frames[frameIndex].stepWhosePositionEdgesBring = forIn->step;
        m_frames[frameIndex].placeOfForInWhosePositionEdgesBring = forIn->place;
    }

    if (!m_hasFailed && inputBlock(frameIndex, inputHead).firstOpenEdge != noOpenEdge) {
        beginWalkOfLoop();
        for (BlockID member : index.membersOf(loop)) {
            if (m_hasFailed)
                break;
            walkBlock(member);
        }
        endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    }

    for (BlockID member : index.membersOf(loop)) {
        inputBlock(frameIndex, member).firstNarrowerName = narrowerNamesBeforeLoop;
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            emptyName(value);
    }
    m_frames[frameIndex].loopsBeingVersioned.removeLast();
    m_frames[frameIndex].stepWhosePositionEdgesBring = { };
    BlockID onlyWayOut;
    bool leavesByOneBlock = true;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (index.isInLoop(target.block(), loop))
                continue;
            leavesByOneBlock &= !onlyWayOut || onlyWayOut == target.block();
            onlyWayOut = target.block();
        }
    }
    if (leavesByOneBlock && onlyWayOut && !listedForTheseTurns.isEmpty())
        m_frames[frameIndex].duplicatedValues->namesGivenForGood.append({ onlyWayOut, WTF::move(listedForTheseTurns) });
    m_block = { };
    m_state.clear();
}

static ValueID stepInFrontOfEffectsInTurnsWrittenOut(const BodyIndex& index, BodyBlock head)
{
    for (uint32_t number = head.firstValue().index(); number < head.endOfValues().index(); ++number) {
        Opcode opcode = index.opcodeOf(ValueID(number));
        if (opcode == Opcode::ForInStep)
            return ValueID(number);
        if (opcode != Opcode::LoopHint)
            return { };
    }
    return { };
}

void Walker::standAtHeadOfTurnOfForIn(BlockID inputHead)
{
    TurnsOfForInWrittenOut& turns = *m_frames.last().turnsOfForIn;
    if (!turns.enumerator)
        return;
    bool isKnownCurrent = m_state.cellFacts(turns.enumerator).contains(CellFact::EnumeratorIsCurrent);
    bool isFirstTurn = !std::exchange(turns.enumeratorIsAsked, true);
    if (!isFirstTurn && isKnownCurrent)
        return;
    Reference position = m_indexedIterations[turns.place].position;
    Reference isCurrent;
    if (isFirstTurn && isKnownCurrent && turns.namesCountedByOpen) {
        Draft names(Opcode::Integer, m_origin);
        names.setInteger(turns.numberOfNames);
        Draft test(Opcode::GreaterEq, m_origin);
        chooseConservativeFields(test);
        test.setLeftOperand(turns.namesCountedByOpen);
        test.setRightOperand(constant(WTF::move(names)));
        test.setForm(operatorFormFor(*this, test));
        isCurrent = build(WTF::move(test));
    } else {
        Draft test(Opcode::IsEnumeratorCurrent, m_origin);
        test.setEnumeratorOperand(turns.enumerator);
        test.setObjectOperand(turns.object);
        test.setPositionOperand(position);
        isCurrent = build(WTF::move(test));
    }
    if (!isCurrent)
        return;
    BlockID current = addBlock();
    BlockID changed = addBlock(BlockFrequency::Unlikely);
    branch(isCurrent, current, changed);
    continueIn(changed);
    if (!isUnreachable()) {
        BodyBlock head = currentBodyIndex().block(inputHead);
        Vector<Reference, 8> arriving;
        for (uint32_t parameter = 0; parameter < head.numberOfParameters(); ++parameter)
            arriving.append(referenceOf(head.parameter(parameter)));
        leadIntoLoopAsWrittenBehindTurns(turns, inputHead, arriving.span(), position);
    }
    continueIn(current);
    if (!isUnreachable())
        markEnumeratorCurrent(*this, turns.enumerator, std::as_const(m_indexedIterations)[turns.place].subject);
}

bool Walker::writeOutTurnsOfForIn(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Folding) || !has(Mechanism::TypedOperations) || !has(Mechanism::LoopInvariants) || !has(Mechanism::Guesses) || !m_opensGuesses || isUnreachable() || hasStringsInBuilders())
        return false;
    if (!index.isLoopHead(inputHead) || !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingVersioned.contains(loop))
        return false;

    BodyBlock head = index.block(inputHead);
    ValueID step = stepInFrontOfEffectsInTurnsWrittenOut(index, head);
    ValueID open = step ? index.value(step).enumeratorOperand() : ValueID();
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::ForInOpen)
        return false;
    ValueID objectInBody = index.value(step).objectOperand();
    if (index.stripCasts(objectInBody) != index.stripCasts(index.value(open).objectOperand()))
        return false;
    Reference state = referenceOf(open);
    if (!state || !state.isVirtual())
        return false;

    TurnsOfForInWrittenOut turns;
    turns.place = state.object().index();
    turns.step = step;
    const IndexedIteration& asItEntered = m_indexedIterations[turns.place];
    bool isOpenedInFrontOfThisLoop = asItEntered.otherForm == IndexedIteration::OtherForm::Enumeration && !asItEntered.head && asItEntered.inputHead == inputHead && asItEntered.inlineFrame == m_frames[frameIndex].inlineFrame;
    if (!isOpenedInFrontOfThisLoop || !asItEntered.list || asItEntered.builtinClass || !asItEntered.precomputedSteps.isEmpty())
        return false;
    turns.enumerator = asItEntered.list;
    Reference firstPosition = asItEntered.position;
    if (integerBuiltForTurnsWrittenOut(*this, firstPosition) != std::optional<int64_t>(0))
        return false;
    if (!m_state.cellFacts(turns.enumerator).contains(CellFact::EnumeratorIsCurrent))
        return false;
    Reference object = referenceOf(objectInBody);
    if (!object || !object.isValue())
        return false;

    uint32_t numberOfOperationsOfLoop = numberOfOperationsOfLoopWrittenOut(index, loop);
    uint32_t mostNames = mostOperationsWalkedForTurnsWrittenOut / numberOfOperationsOfLoop;
    if (mostNames < 2)
        return false;
    --mostNames;
    uint32_t namesThatStillFit = mostNames;
    std::optional<OwnKeyOrder> inSight = ownKeysInSight(*this, object);
    OwnKeyPlan planned = inSight ? OwnKeyPlan() : planOwnKeyWalk(*this, objectInBody, object, mostNames);
    if (!inSight && !planned)
        return false;
    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, [&] { return numbersOfHandlersThatRead(m_frames.last()); }))
        return false;

    Origin origin = originOf(index.value(step));
    Vector<const OwnKeyOrder*, 2> orders;
    auto takeOrder = [&](const OwnKeyOrder& order) {
        uint32_t numberOfNames = static_cast<uint32_t>(order.fields.size());
        if (!order.layout || !numberOfNames || numberOfNames > namesThatStillFit)
            return;
        if (!inSight && !shouldOpenGuess(*this, object, m_types.forLayout(order.layout), planned.isDeclared, numberOfNames))
            return;
        TurnsOfForInWrittenOut::Arm arm;
        for (FieldIndex field : order.fields) {
            PropertyKeyID key = plan().slots().keyOf(program(), FieldReference { order.layout, field });
            if (!key || liesInThread(key) || program().at(key).kind != PropertyKeyKind::Text || program().isIndexLike(key))
                return;
            Draft name(Opcode::String, origin);
            name.setString(program().at(key).text);
            arm.names.append(constant(WTF::move(name)));
        }
        namesThatStillFit -= numberOfNames;
        orders.append(&order);
        turns.arms.append(WTF::move(arm));
    };
    if (inSight)
        takeOrder(*inSight);
    else {
        for (const OwnKeyOrder& order : planned.orders)
            takeOrder(order);
    }
    if (orders.isEmpty())
        return false;

    beginTurnsWrittenOut(inputHead);
    turns.firstPosition = firstPosition;
    for (size_t number = 0; number < orders.size() && !m_hasFailed; ++number) {
        TurnsOfForInWrittenOut::Arm& arm = turns.arms[number];
        arm.object = object;
        if (inSight) {
            arm.whereTestsPassed = m_block;
            arm.stateWhereTestsPassed = WTF::move(m_state);
            break;
        }
        BlockID otherwise = addBlock(number + 1 == orders.size() ? BlockFrequency::Unlikely : BlockFrequency::Normal);
        bool hasKeys = continueIfOwnKeys(*this, arm.object, *orders[number], planned.layoutsAreProven, origin, otherwise);
        recordSpeculation();
        if (hasKeys && arm.object != object && !m_state.checkedNameOf(object))
            m_state.noteCheckedName(object, arm.object);
        arm.whereTestsPassed = hasKeys ? m_block : BlockID();
        arm.stateWhereTestsPassed = WTF::move(m_state);
        continueIn(otherwise);
        if (isUnreachable())
            break;
        if (number + 1 == orders.size())
            leadIntoLoopAsWrittenBehindTurns(turns, inputHead, entering, firstPosition);
    }
    if (m_hasFailed)
        return true;
    turns.namesCountedByOpen = std::exchange(m_indexedIterations[turns.place].count, Reference());
    for (size_t number = 0; number < orders.size(); ++number)
        count(StatisticID::OwnKeyWalksExpanded);
    walkTurnsWrittenOut(inputHead, entering, 0, &turns);
    return true;
}

static bool isStepValueUsedAsKey(const BodyIndex& index, LoopID loop, ValueID step)
{
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!accessesUnderComputedKeys.contains(operation.opcode()))
                continue;
            ValueID key = index.stripCasts(operation.propertyOperand());
            if (key && !index.isParameter(key) && index.opcodeOf(key) == Opcode::ResultAt && index.value(key).operand(0) == step)
                return true;
        }
    }
    return false;
}

bool Walker::writeOutTurnsOfForOf(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    bool standsInLoop = frameIndex ? !!m_frames[frameIndex].loopDepthAtCall : index.loopDepthOf(inputHead) >= 2;
    if (!standsInLoop && !index.holdsAnyOf(accessesUnderComputedKeys))
        return false;
    if (!has(Mechanism::Folding) || !has(Mechanism::TypedOperations) || !has(Mechanism::LoopInvariants) || !has(Mechanism::EscapeAnalysis) || isUnreachable() || hasStringsInBuilders())
        return false;
    if (!index.isLoopHead(inputHead) || !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingVersioned.contains(loop))
        return false;
    std::optional<ForOfByText> forOf = findForOfSteppedAtHead(index, loop);
    Reference state = forOf ? referenceOf(forOf->open) : Reference();
    if (!state || !state.isVirtual())
        return false;

    TurnsOfForInWrittenOut turns;
    turns.place = state.object().index();
    turns.step = forOf->step;
    const IndexedIteration& asItEntered = m_indexedIterations[turns.place];
    bool isOpenedInFrontOfThisLoop = asItEntered.otherForm == IndexedIteration::OtherForm::None && !asItEntered.head && asItEntered.inputHead == inputHead && asItEntered.inlineFrame == m_frames[frameIndex].inlineFrame;
    bool isIndexedArrayIteration = asItEntered.list && asItEntered.contents == IndexedIteration::Contents::List && asItEntered.kind == JSC::IterationKind::Values
        && !asItEntered.hasProtocolFallback && !asItEntered.hasPrecomputedSteps && !asItEntered.isOverFlatEntries && !asItEntered.table;
    if (!isOpenedInFrontOfThisLoop || !isIndexedArrayIteration || integerBuiltForTurnsWrittenOut(*this, asItEntered.position) != std::optional<int64_t>(0))
        return false;
    std::optional<uint32_t> length = lengthOfListInSight(*this, asItEntered.list);
    bool lengthIsNoted = !length;
    if (lengthIsNoted)
        length = lengthOfListMadeInSight(*this, asItEntered.list);
    if (!length || *length > mostTurnsWrittenOut || numberOfOperationsOfLoopWrittenOut(index, loop) * (*length + 1) > mostOperationsWalkedForTurnsWrittenOut)
        return false;
    if (!standsInLoop) {
        if (!isStepValueUsedAsKey(index, loop, forOf->step))
            return false;
        for (uint32_t position = 0; position < *length; ++position) {
            Reference element = elementOfListInSight(*this, asItEntered.list, position);
            if (!element)
                element = elementWrittenInListMadeInSight(*this, asItEntered.list, position);
            if (!element || !element.isConstant() || m_code.at(element.id()).opcode != Opcode::String)
                return false;
        }
    }
    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, [&] { return numbersOfHandlersThatRead(m_frames.last()); }, forOf->open))
        return false;
    if (!standsInLoop)
        count(StatisticID::ConstantKeyLoopsFullyUnrolled);

    if (lengthIsNoted) {
        checkLengthOfListMadeInSight(*this, asItEntered.list, *length, m_origin);
        count(StatisticID::RecordedLengthLoopsFullyUnrolled);
    }
    beginTurnsWrittenOut(inputHead);
    walkTurnsWrittenOut(inputHead, entering, *length + 1, &turns);
    IndexedIteration& iteration = m_indexedIterations[turns.place];
    if (!m_hasFailed && !iteration.head && iteration.inputHead) {
        iteration.inputHead = { };
        --m_numberOfIterationsInFrontOfTheirLoops;
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
