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
#include "AOTWalkBlocksThatEveryTurnPasses.h"
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
static constexpr OpcodeSet callsAndClosures { Opcode::Call, Opcode::CallList, Opcode::CallDirect, Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList, Opcode::NewClosure };

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

namespace {

struct CounterOfTurns {
    ValueID name;
    int64_t start { 0 };
    int64_t step { 0 };
};

} // anonymous namespace

static std::optional<bool> answerByText(const IntegersByText& integers, ValueID condition)
{
    const BodyIndex& index = integers.index;
    ValueID asked = condition ? index.stripCasts(condition) : ValueID();
    if (!asked || index.isParameter(asked))
        return std::nullopt;
    BodyValue relation = index.value(asked);
    if (!relationsOfCountersWrittenOut.contains(relation.opcode()) && relation.opcode() != Opcode::StrictEq)
        return std::nullopt;
    std::optional<int64_t> left = integers.at(relation.operand(0));
    std::optional<int64_t> right = left ? integers.at(relation.operand(1)) : std::nullopt;
    if (!right)
        return std::nullopt;
    switch (relation.opcode()) {
    case Opcode::Less:
        return *left < *right;
    case Opcode::LessEq:
        return *left <= *right;
    case Opcode::Greater:
        return *left > *right;
    case Opcode::GreaterEq:
        return *left >= *right;
    default:
        return *left == *right;
    }
}

static uint32_t operationsInUnrolledIterations(const IntegersByText& integers, LoopID loop, const CounterOfTurns& counter, uint32_t numberOfTurns)
{
    const BodyIndex& index = integers.index;
    BlockID head = index.loop(loop).head;
    uint32_t numberOfOperations = 0;
    for (uint32_t turn = 0; turn < numberOfTurns; ++turn) {
        int64_t count = counter.start + turn * counter.step;
        WTF::Function<std::optional<int64_t>(ValueID)> leaf = [&](ValueID value) {
            return value == counter.name ? std::optional<int64_t>(count) : integers.leaf(value);
        };
        IntegersByText inTurn { index, integers.keyOfLength, leaf, integers.lengthOf };
        Vector<BlockID, 16> reached { head };
        for (size_t place = 0; place < reached.size(); ++place) {
            BodyBlock block = index.block(reached[place]);
            numberOfOperations += block.numberOfValues() + 1;
            if (numberOfOperations > mostOperationsWalkedForTurnsWrittenOut)
                return numberOfOperations;
            BodyTerminator end = block.terminator();
            auto reach = [&](BlockID next) {
                if (next != head && index.isInLoop(next, loop) && !reached.contains(next))
                    reached.append(next);
            };
            if (std::optional<bool> answer = end.isConditionalBranch() ? answerByText(inTurn, end.operand(0)) : std::nullopt)
                reach((*answer ? end.taken() : end.notTaken()).block());
            else {
                for (BodyTarget target : end.targets())
                    reach(target.block());
            }
        }
    }
    return numberOfOperations;
}

static std::optional<uint32_t> foreseeTurnsByText(const IntegersByText& integers, LoopID loop, const WTF::Function<std::optional<int64_t>(uint32_t)>& entering, CounterOfTurns* counterOfTurns = nullptr)
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
    if (counterOfTurns)
        *counterOfTurns = { counter, *start, *step };
    return stays;
}

static bool textAdmitsTurnsWrittenOut(const BodyIndex& index, BodyLiveness& liveness, LoopID loop, bool isCopy, NOESCAPE const Invocable<std::span<const uint32_t>()> auto& numbersOfHandlersThatRead, ValueID openSteppedByIndex = { }, bool loopsWithinAreWrittenOut = false, bool yieldsToPattern = false)
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
            if (maySuspendAsWritten(operation) && !(yieldsToPattern && operation.opcode() == Opcode::Yield))
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
            if (list && !body->isParameter(list) && body->opcodeOf(list) == Opcode::PropGet && !body->value(list).isFreeName())
                list = body->stripCasts(body->value(list).objectOperand());
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
static constexpr OpcodeSet innerArrayReads { Opcode::PropGet, Opcode::ElemGet };

static bool dependsOnIteration(const BodyIndex& index, LoopID loop, ValueID written, unsigned operatorsLeft = 4)
{
    ValueID value = index.stripCasts(written);
    if (!index.isInLoop(index.blockOf(value), loop))
        return false;
    if (index.isParameter(value) || !operatorsLeft)
        return true;
    bool depends = false;
    index.value(value).forEachOperand([&](ValueID operand, uint32_t) {
        depends |= dependsOnIteration(index, loop, operand, operatorsLeft - 1);
    });
    return depends;
}

static bool storesIntoInnerList(Walker& walker, const BodyIndex& index, LoopID loop)
{
    LoopID loopAround = index.loop(loop).parent;
    if (!loopAround || index.loop(loop).numberOfLoopsWithin != 1)
        return false;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            ValueID list = operation.opcode() == Opcode::ElemSet ? index.stripCasts(operation.objectOperand()) : ValueID();
            if (list && !index.isParameter(list) && innerArrayReads.contains(index.opcodeOf(list)) && index.isInLoop(index.blockOf(list), loopAround) && !dependsOnIteration(index, loop, list) && !isExpectedToBeView(walker, list))
                return true;
        }
    }
    return false;
}

static bool readsRowsOfUnknownField(Walker& walker, const BodyIndex& index, LoopID loop)
{
    if (!index.loop(loop).parent || index.loop(loop).numberOfLoopsWithin != 1)
        return false;
    return std::ranges::any_of(index.membersOf(loop), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue operation) {
            ValueID row = operation.opcode() == Opcode::ElemGet ? index.stripCasts(operation.objectOperand()) : ValueID();
            if (!row || index.isParameter(row) || index.opcodeOf(row) != Opcode::ElemGet || dependsOnIteration(index, loop, row))
                return false;
            ValueID field = index.stripCasts(index.value(row).objectOperand());
            if (!field || index.isParameter(field) || index.opcodeOf(field) != Opcode::PropGet || !index.isInLoop(index.blockOf(field), loop))
                return false;
            ValueID object = index.stripCasts(index.value(field).objectOperand());
            return object && !index.isInLoop(index.blockOf(object), loop) && isFieldOfUnknownObject(walker, index.value(field), walker.referenceBoundTo(object));
        });
    });
}

static std::optional<uint32_t> numberOfOperationsOfLoopsWrittenOut(Walker& walker, const IntegersByText& integers, LoopID loop, uint32_t stays, bool countsCopies, bool isWrittenOut = true)
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
        std::optional<uint32_t> staysWithin = !isWrittenOut && storesIntoInnerList(walker, index, within) ? std::nullopt : foreseeTurnsByText(integers, within, whatEnters);
        std::optional<uint32_t> ofLoopWithin = staysWithin ? numberOfOperationsOfLoopsWrittenOut(walker, integers, within, *staysWithin, countsCopies) : std::nullopt;
        if (!ofLoopWithin)
            return std::nullopt;
        numberOfOperationsOfTurn += *ofLoopWithin - walkedOnce(within);
    }
    uint32_t numberOfOperations = stays * numberOfOperationsOfTurn + index.block(index.loop(loop).head).numberOfValues() + 1;
    return numberOfOperations > mostOperationsWalkedForTurnsWrittenOut ? std::nullopt : std::optional(numberOfOperations);
}

static bool isOutermostUnrollingTooLarge(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t operationsOfIteration, uint32_t numberOfWalks)
{
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            FunctionID callee = operation.opcode() == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), operation.id()) : operation.opcode() == Opcode::CallDirect && !operation.isConstruction() ? operation.function() : FunctionID();
            if (callee && calleeIsCopiedInLoopsAlone(walker, callee, operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().argumentsOperands().size() : operation.numberOfVariadicOperands()))
                return true;
            if (operation.opcode() != Opcode::ElemGet && operation.opcode() != Opcode::ElemSet)
                continue;
            ValueID object = index.stripCasts(operation.objectOperand());
            bool isDefinedInLoop = object && object.index() >= firstValue && index.isInLoop(index.blockOf(object), loop);
            Reference list = object && !isDefinedInLoop ? walker.referenceBoundTo(object) : Reference();
            ValueID cell = list && list.isValue() ? underlyingCellOf(walker.code(), list.id()) : ValueID();
            bool isInSight = (list && list.isConstant()) || (cell && (walker.code().isConstant(cell) || walker.state().isFresh(Reference::value(cell)))) || (list && findHoistedArrayGuard(walker, list));
            if (!isInSight)
                operationsOfIteration += operationsThatAccessToListOutOfSightBecomes;
        }
    }
    return operationsOfIteration * numberOfWalks > mostOperationsWalkedForTurnsWrittenOut;
}

bool Walker::hasUnrolledInnerLoops(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValue = index.block(inputHead).firstParameter().index();
    auto isDefinedInLoop = [&](ValueID value) {
        return value.index() >= firstValue && index.isInLoop(index.blockOf(value), loop);
    };
    WTF::Function<std::optional<int64_t>(ValueID)> leaf = [&](ValueID value) -> std::optional<int64_t> {
        return isDefinedInLoop(value) ? std::nullopt : integerBuiltForTurnsWrittenOut(*this, referenceBoundTo(value));
    };
    WTF::Function<std::optional<int64_t>(ValueID)> lengthOf = [](ValueID) -> std::optional<int64_t> {
        return std::nullopt;
    };
    IntegersByText integers { index, program().keyOf(BuiltinNames::length), leaf, lengthOf };
    return has(Mechanism::Folding) && has(Mechanism::LoopInvariants) && numberOfOperationsOfLoopsWrittenOut(*this, integers, loop, 1, !frameIndex, false);
}

static bool isLeftAtHeadAlone(const BodyIndex& index, LoopID loop)
{
    BlockID head = index.loop(loop).head;
    return std::ranges::all_of(index.membersOf(loop), [&](BlockID member) {
        auto targets = index.terminator(member).targets();
        return member == head || (std::ranges::begin(targets) != std::ranges::end(targets) && std::ranges::all_of(targets, [&](const BodyTarget& target) { return index.isInLoop(target.block(), loop); }));
    });
}

BlockID Walker::blockAroundConstantTrips(size_t frameIndex, BlockID block, LoopID around) const
{
    if (m_constantTripLoops.isEmpty())
        return block;
    const BodyIndex& index = *m_frames[frameIndex].index;
    for (LoopID inner = index.loopOf(block); inner && inner != around; inner = index.loop(inner).parent) {
        size_t place = m_constantTripLoops.findIf([&](const ConstantTripLoop& loop) { return loop.frameIndex == frameIndex && loop.loop == inner; });
        if (place == notFound || !index.dominates(block, m_constantTripLoops[place].lastDominator))
            break;
        block = index.loop(inner).head;
    }
    return block;
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
    std::optional<uint32_t> takenByPattern = iterationsTakenByPattern(loop);
    CounterOfTurns counter;
    std::optional<uint32_t> stays = takenByPattern ? takenByPattern : foreseeTurnsByText(integers, loop, whatEnters, &counter);
    if (!stays)
        return false;
    BlockID headAround = m_frames[frameIndex].fullyUnrolledHeader;
    bool isInUnrolledNest = headAround && index.loop(loop).parent == index.loopOf(headAround);
    if (!isInUnrolledNest && storesIntoInnerList(*this, index, loop)) {
        count(StatisticID::StoringLoopsNotUnrolled);
        return false;
    }
    if (!isInUnrolledNest && readsRowsOfUnknownField(*this, index, loop))
        return false;
    bool holdsLoops = index.loop(loop).numberOfLoopsWithin != 1;
    bool standsInLoop = m_frames[frameIndex].loopDepthAtCall || index.loopDepthOf(inputHead) >= 2;
    if (holdsLoops && (!standsInLoop || !numberOfOperationsOfLoopsWrittenOut(*this, integers, loop, *stays, !frameIndex)))
        return false;
    uint32_t numberOfOperationsOfTurn = numberOfOperationsOfLoopWrittenOut(index, loop) + (frameIndex ? 0 : numberOfOperationsOfCopiesInTurnWrittenOut(*this, index, loop));
    bool isTooLarge = !holdsLoops && numberOfOperationsOfTurn * (*stays + 1) > mostOperationsWalkedForTurnsWrittenOut;
    if (isTooLarge && counter.name && !isInUnrolledNest && standsInLoop) {
        uint32_t operationsOfCopies = numberOfOperationsOfTurn - numberOfOperationsOfLoopWrittenOut(index, loop);
        isTooLarge = operationsInUnrolledIterations(integers, loop, counter, *stays) + numberOfOperationsOfTurn + operationsOfCopies * *stays > mostOperationsWalkedForTurnsWrittenOut;
    }
    bool isReckonedWithNest = isTooLarge && isInUnrolledNest;
    if (isTooLarge && !isReckonedWithNest)
        return false;
    if (!m_frames[frameIndex].loopDepthAtCall && index.loopDepthOf(inputHead) < 2 && isOutermostUnrollingTooLarge(*this, index, inputHead, numberOfOperationsOfTurn, *stays + 1)) {
        count(StatisticID::FullUnrollingsRejectedForSize);
        return false;
    }

    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, [&] { return numbersOfHandlersThatRead(m_frames.last()); }, { }, holdsLoops, takenByPattern.has_value()))
        return false;

    beginTurnsWrittenOut(inputHead);
    if (frameIndex)
        count(StatisticID::InlineeLoopsFullyUnrolled);
    if (holdsLoops)
        count(StatisticID::LoopNestsFullyUnrolled);
    if (isReckonedWithNest)
        count(StatisticID::InnerLoopsUnrolledWithNest);
    bool isConstantTrip = !takenByPattern && isLeftAtHeadAlone(index, loop);
    if (isConstantTrip)
        m_constantTripLoops.append(ConstantTripLoop { frameIndex, loop, BackEdgeDominators(index, loop).lastOf(loop) });
    walkTurnsWrittenOut(inputHead, entering, *stays + 1, nullptr);
    if (isConstantTrip)
        m_constantTripLoops.removeLast();
    return true;
}

static std::optional<int64_t> integerWhereListIsOfLength(const Walker&, Reference value, Reference list, uint32_t length, unsigned operationsLeft = 6);

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

void Walker::leadIntoLoopAsWrittenBehindTurns(UnrolledForInIterations& turns, BlockID inputHead, std::span<const Reference> arriving, Reference position)
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
        if (turns.step)
            open.names.setNameIn(m_frames[frameIndex].ensureDuplicatedValues().ensureSlotOf(turns.step), position);
        turns.edgesIntoLoopAsWritten.append(place);
    }
    m_block = { };
    m_state.clear();
}

void Walker::walkTurnsWrittenOut(BlockID inputHead, std::span<const Reference> entering, uint32_t numberOfWalks, UnrolledForInIterations* forIn)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    LoopID loop = index.loopOf(inputHead);
    uint32_t narrowerNamesBeforeLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;
    if (!m_frames[frameIndex].loopDepthAtCall && index.loopDepthOf(inputHead) < 2)
        count(StatisticID::OutermostLoopsFullyUnrolled);
    Vector<ValueID, 4> listedForTheseTurns = std::exchange(m_frames[frameIndex].valuesListedForTurnsWrittenOut, { });

    auto enteringArm = [&](size_t position) {
        if (!forIn || !forIn->list || !forIn->armBeingWalked || position != forIn->counter || entering[position].isConstant())
            return entering[position];
        Draft start(Opcode::Integer, m_origin);
        start.setInteger(*integerWhereListIsOfLength(*this, entering[position], referenceOf(forIn->list), forIn->numberOfNames));
        return constant(WTF::move(start));
    };
    auto leadIntoFirstTurn = [&] {
        BlockID from = m_block;
        inputBlock(frameIndex, inputHead) = InputBlock { };
        if (from)
            m_code.end(from, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
        uint32_t place = from ? addOpenEdge(frameIndex, inputHead) : noOpenEdge;
        if (place != noOpenEdge) {
            OpenEdge& open = m_openEdges[place];
            open.edge = m_code.at(from).terminator.next();
            for (size_t position = 0; position < entering.size(); ++position) {
                Reference argument = enteringArm(position);
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
    UnrolledForInIterations* forInBefore = std::exchange(m_frames[frameIndex].turnsOfForIn, forIn);
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
        uint32_t cameRound = std::exchange(inputBlock(frameIndex, inputHead).firstOpenEdge, noOpenEdge);
        while (cameRound != noOpenEdge) {
            if (forIn->step)
                m_openEdges[cameRound].names.setNameIn(m_frames[frameIndex].ensureDuplicatedValues().ensureSlotOf(forIn->step), m_indexedIterations[forIn->place].position);
            forIn->edgesIntoLoopAsWritten.append(cameRound);
            cameRound = std::exchange(m_openEdges[cameRound].next, noOpenEdge);
        }
    };
    if (!forIn || forIn->arms.isEmpty())
        walkTurns(numberOfWalks);
    else {
        OpenEdge inFrontOfArms;
        takeNamesOntoEdge(inFrontOfArms, frameIndex);
        for (UnrolledForInIterations::Arm& arm : forIn->arms) {
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
            forIn->armBeingWalked = &arm;
            if (forIn->step) {
                m_indexedIterations[forIn->place].position = forIn->firstPosition;
                m_indexedIterations[forIn->place].precomputedSteps = arm.names;
            }
            walkTurns(forIn->enumerator ? forIn->numberOfNames : forIn->numberOfNames + 1);
            if (!m_hasFailed)
                letWhatCameRoundWaitForLoopAsWritten();
        }
    }
    m_frames[frameIndex].fullyUnrolledHeader = headBefore;
    m_frames[frameIndex].turnsOfForIn = forInBefore;

    if (forIn && !forIn->step && !m_hasFailed) {
        for (uint32_t waiting : forIn->edgesIntoLoopAsWritten)
            m_openEdges[waiting].next = std::exchange(inputBlock(frameIndex, inputHead).firstOpenEdge, waiting);
    }
    if (forIn && forIn->step && !m_hasFailed) {
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
    UnrolledForInIterations& turns = *m_frames.last().turnsOfForIn;
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

static bool appendNamesOfOrder(Walker& walker, const OwnKeyOrder& order, Origin origin, Vector<Reference, 4>& names)
{
    const Program& program = walker.program();
    for (FieldIndex field : order.fields) {
        PropertyKeyID key = walker.plan().slots().keyOf(program, FieldReference { order.layout, field });
        if (!key || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text || program.isIndexLike(key))
            return false;
        Draft name(Opcode::String, origin);
        name.setString(program.at(key).text);
        names.append(walker.constant(WTF::move(name)));
    }
    return true;
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

    UnrolledForInIterations turns;
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
        UnrolledForInIterations::Arm arm;
        if (!appendNamesOfOrder(*this, order, origin, arm.names))
            return;
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
        UnrolledForInIterations::Arm& arm = turns.arms[number];
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

static ValueID listOnlyReadInBody(const Walker& walker, const BodyIndex& index, ValueID written)
{
    ValueID list = index.stripCasts(written);
    if (!list || index.isParameter(list) || index.opcodeOf(list) != Opcode::Call || index.value(list).argumentsOperands().size() != 1)
        return { };
    bool isStatedOnce = list != written && index.useCount(list) == 1 && index.value(written).operand(0) == list;
    if (list != written && !isStatedOnce)
        return { };
    return bodyOnlyReadsListOfCall(walker, index, walker.program().keyOf(BuiltinNames::length), written) ? list : ValueID();
}

static bool isOutermostSmallForOf(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t length)
{
    LoopID loop = index.loopOf(inputHead);
    if (index.loop(loop).numberOfLoopsWithin != 1)
        return false;
    uint32_t numberOfOperations = numberOfOperationsOfLoopWrittenOut(index, loop);
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (callsAndClosures.contains(operation.opcode()))
                return false;
            if (operation.opcode() != Opcode::PropGet && operation.opcode() != Opcode::PropSet)
                continue;
            Reference object = operation.isFreeName() ? Reference() : walker.referenceBoundTo(index.stripCasts(operation.objectOperand()));
            if (!object || !singleLayoutOf(walker.types(), walker.types().at(walker.typeOf(object))))
                numberOfOperations += operationsThatAccessToListOutOfSightBecomes;
        }
    }
    return !isOutermostUnrollingTooLarge(walker, index, inputHead, numberOfOperations, length + 1);
}

bool Walker::unrollUnderOrders(BlockID inputHead, std::span<const Reference> entering, UnrolledForInIterations& turns, ValueID open, ValueID objectInBody, Reference object, Origin origin, const Invocable<bool(uint32_t)> auto& mayWriteOut)
{
    const BodyIndex& index = *m_frames.last().index;
    LoopID loop = index.loopOf(inputHead);
    uint32_t mostNames = mostOperationsWalkedForTurnsWrittenOut / numberOfOperationsOfLoopWrittenOut(index, loop);
    if (mostNames < 2)
        return false;
    --mostNames;
    OwnKeyPlan planned = planOwnKeyWalk(*this, objectInBody, object, mostNames);
    size_t frameIndex = m_frames.size() - 1;
    if (!planned || !textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, [&] { return numbersOfHandlersThatRead(m_frames.last()); }, open))
        return false;

    Vector<const OwnKeyOrder*, 2> orders;
    for (const OwnKeyOrder& order : planned.orders) {
        uint32_t numberOfNames = static_cast<uint32_t>(order.fields.size());
        UnrolledForInIterations::Arm arm;
        if (!order.layout || !numberOfNames || numberOfNames > mostNames || !mayWriteOut(numberOfNames) || !shouldOpenGuess(*this, object, m_types.forLayout(order.layout), planned.isDeclared, numberOfNames) || !appendNamesOfOrder(*this, order, origin, arm.names))
            continue;
        mostNames -= numberOfNames;
        orders.append(&order);
        turns.arms.append(WTF::move(arm));
    }
    if (orders.isEmpty())
        return false;

    beginTurnsWrittenOut(inputHead);
    if (turns.step)
        turns.firstPosition = std::as_const(m_indexedIterations)[turns.place].position;
    for (size_t number = 0; number < orders.size() && !m_hasFailed; ++number) {
        UnrolledForInIterations::Arm& arm = turns.arms[number];
        arm.object = object;
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
            leadIntoLoopAsWrittenBehindTurns(turns, inputHead, entering, turns.firstPosition);
    }
    if (m_hasFailed)
        return true;
    for (size_t number = 0; number < orders.size(); ++number)
        count(StatisticID::OwnKeyWalksExpanded);
    walkTurnsWrittenOut(inputHead, entering, 0, &turns);
    return true;
}

bool Walker::unrollOwnKeysLoop(BlockID inputHead, std::span<const Reference> entering, UnrolledForInIterations& turns, const ForOfByText& forOf)
{
    const BodyIndex& index = *m_frames.last().index;
    LoopID loop = index.loopOf(inputHead);
    ValueID list = has(Mechanism::Guesses) && m_opensGuesses ? listOnlyReadInBody(*this, index, index.value(forOf.open).as<Opcode::IteratorOpen>().iterableOperand()) : ValueID();
    Reference made = list ? referenceOf(list) : Reference();
    ValueID objectInBody = list ? index.value(list).argumentsOperands()[0] : ValueID();
    Reference object = objectInBody ? referenceOf(objectInBody) : Reference();
    if (!made || !made.isValue() || !object || !object.isValue() || !findKnown(*this, KnownNote::ListedOwnKeys, object, made) || !isStepValueUsedAsKey(index, loop, forOf.step))
        return false;
    auto mayWriteOut = [](uint32_t) {
        return true;
    };
    if (!unrollUnderOrders(inputHead, entering, turns, forOf.open, objectInBody, object, originOf(index.value(forOf.step)), mayWriteOut))
        return false;
    if (IndexedIteration& iteration = m_indexedIterations[turns.place]; !m_hasFailed && !iteration.head && iteration.inputHead) {
        iteration.inputHead = { };
        --m_numberOfIterationsInFrontOfTheirLoops;
    }
    return true;
}

namespace {

struct KeyReadOutOfList {
    ValueID key;
    uint32_t counter { 0 };
};

} // anonymous namespace

static std::optional<uint32_t> counterOfPosition(const BodyIndex& index, LoopID loop, ValueID written)
{
    BodyBlock head = index.block(index.loop(loop).head);
    ValueID position = index.stripCasts(written);
    if (position && !index.isParameter(position) && stepsOfCountersWrittenOut.contains(index.opcodeOf(position)) && index.isInLoop(index.blockOf(position), loop))
        position = index.stripCasts(index.value(position).operand(0));
    std::optional<uint32_t> counter = position ? head.indexOfParameter(position) : std::nullopt;
    if (!counter)
        return std::nullopt;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != head.id())
                continue;
            ValueID brought = *counter < target.arguments().size() ? index.stripCasts(target.arguments()[*counter]) : ValueID();
            if (!brought || index.isParameter(brought) || !stepsOfCountersWrittenOut.contains(index.opcodeOf(brought)) || index.stripCasts(index.value(brought).operand(0)) != head.parameter(*counter))
                return std::nullopt;
        }
    }
    return counter;
}

static KeyReadOutOfList findKeyReadOutOfList(const BodyIndex& index, LoopID loop)
{
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!accessesUnderComputedKeys.contains(operation.opcode()))
                continue;
            ValueID key = index.stripCasts(operation.propertyOperand());
            if (!key || index.isParameter(key) || index.opcodeOf(key) != Opcode::ElemGet || !index.isInLoop(index.blockOf(key), loop))
                continue;
            if (std::optional<uint32_t> counter = counterOfPosition(index, loop, index.value(key).propertyOperand()))
                return { key, *counter };
        }
    }
    return { };
}

static std::optional<int64_t> integerWhereListIsOfLength(const Walker& walker, Reference value, Reference list, uint32_t length, unsigned operationsLeft)
{
    static constexpr OpcodeSet conversions { Opcode::Box, Opcode::Unbox, Opcode::IntegerToNumber, Opcode::NarrowInteger, Opcode::Narrowed };
    if (std::optional<int64_t> known = integerBuiltForTurnsWrittenOut(walker, value))
        return known;
    if (!value || !value.isValue() || !operationsLeft--)
        return std::nullopt;
    const Operation& made = walker.code().at(value.id());
    if (made.opcode == Opcode::GetArrayLength)
        return underlyingCellOf(walker.code(), made.operand(0)) == underlyingCellOf(walker.code(), list.id()) ? std::optional<int64_t>(length) : std::nullopt;
    if (!conversions.contains(made.opcode) && !stepsOfCountersWrittenOut.contains(made.opcode))
        return std::nullopt;
    std::optional<int64_t> first = integerWhereListIsOfLength(walker, Reference::value(made.operand(0)), list, length, operationsLeft);
    if (!first || conversions.contains(made.opcode))
        return first;
    if (made.opcode == Opcode::Increment || made.opcode == Opcode::Decrement)
        return *first + (made.opcode == Opcode::Increment ? 1 : -1);
    std::optional<int64_t> second = integerWhereListIsOfLength(walker, Reference::value(made.operand(1)), list, length, operationsLeft);
    if (!second)
        return std::nullopt;
    return made.opcode == Opcode::Add ? *first + *second : *first - *second;
}

bool Walker::unrollOverListedKeys(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Folding) || !has(Mechanism::TypedOperations) || !has(Mechanism::LoopInvariants) || !has(Mechanism::Guesses) || !m_opensGuesses || isUnreachable() || hasStringsInBuilders() || !index.isLoopHead(inputHead))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingVersioned.contains(loop) || m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))
        return false;
    if (index.loop(loop).numberOfLoopsWithin != 1 || numberOfOperationsOfLoopWrittenOut(index, loop) * 2 > mostOperationsWalkedForTurnsWrittenOut)
        return false;
    KeyReadOutOfList read = findKeyReadOutOfList(index, loop);
    ValueID list = read.key ? listOnlyReadInBody(*this, index, index.value(read.key).objectOperand()) : ValueID();
    Reference made = list ? referenceOf(list) : Reference();
    ValueID objectInBody = list ? index.value(list).argumentsOperands()[0] : ValueID();
    Reference object = objectInBody ? referenceOf(objectInBody) : Reference();
    if (!made || !made.isValue() || !object || !object.isValue() || read.counter >= entering.size() || !findKnown(*this, KnownNote::ListedOwnKeys, object, made))
        return false;

    UnrolledForInIterations turns;
    turns.list = list;
    turns.counter = read.counter;
    auto startsWithin = [&](uint32_t numberOfNames) {
        std::optional<int64_t> start = integerWhereListIsOfLength(*this, entering[read.counter], made, numberOfNames);
        return start && *start >= 0 && *start <= numberOfNames;
    };
    return unrollUnderOrders(inputHead, entering, turns, { }, objectInBody, object, originOf(index.value(read.key)), startsWithin);
}

bool Walker::bindListedKey(const BodyValue& value)
{
    const UnrolledForInIterations& turns = *m_frames.last().turnsOfForIn;
    if (!turns.list || !turns.armBeingWalked || (value.opcode() != Opcode::ElemGet && value.opcode() != Opcode::PropGet))
        return false;
    const BodyIndex& index = currentBodyIndex();
    if ((value.opcode() == Opcode::PropGet && value.isFreeName()) || index.stripCasts(value.objectOperand()) != turns.list)
        return false;
    if (value.opcode() == Opcode::PropGet) {
        if (value.key() != program().keyOf(BuiltinNames::length))
            return false;
        Draft length(Opcode::Integer, originOf(value));
        length.setInteger(turns.numberOfNames);
        bind(value.id(), constant(WTF::move(length)));
        return true;
    }
    std::optional<int64_t> position = integerBuiltForTurnsWrittenOut(*this, referenceOf(value.propertyOperand()));
    if (!position || *position < 0 || *position >= turns.numberOfNames)
        return false;
    bind(value.id(), turns.armBeingWalked->names[static_cast<size_t>(*position)]);
    return true;
}

bool Walker::writeOutTurnsOfForOf(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    bool standsInLoop = frameIndex ? !!m_frames[frameIndex].loopDepthAtCall : index.loopDepthOf(inputHead) >= 2;
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

    UnrolledForInIterations turns;
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
    if (!length)
        return unrollOwnKeysLoop(inputHead, entering, turns, *forOf);
    if (*length > mostTurnsWrittenOut || numberOfOperationsOfLoopWrittenOut(index, loop) * (*length + 1) > mostOperationsWalkedForTurnsWrittenOut)
        return false;
    bool isConstantKeyLoop = !standsInLoop && isStepValueUsedAsKey(index, loop, forOf->step);
    if (!standsInLoop) {
        for (uint32_t position = 0; position < *length; ++position) {
            Reference element = elementOfListInSight(*this, asItEntered.list, position);
            if (!element)
                element = elementWrittenInListMadeInSight(*this, asItEntered.list, position);
            if (!element)
                return false;
            isConstantKeyLoop &= element.isConstant() && m_code.at(element.id()).opcode == Opcode::String;
        }
        if (!isConstantKeyLoop && !isOutermostSmallForOf(*this, index, inputHead, *length))
            return false;
    }
    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, [&] { return numbersOfHandlersThatRead(m_frames.last()); }, forOf->open))
        return false;
    if (isConstantKeyLoop)
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
