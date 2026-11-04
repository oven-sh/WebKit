/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTThinSummary.h"
#include "AOTWalkCopies.h"
#include "AOTWalkLoopsOverGeneratorsBuilder.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// `function sum(...values) { let s = 0; for (let i = 0; i < values.length; i++) s += values[i]; return s; }`, called as `sum(a, 1, 2)`; `for (let i = 0; i < 4; i++) out += row[i]`.
// A counter that enters with a known integer, that every way round steps by a known integer, and that the head holds against a known integer stays a known number of times.
// The blocks of such a loop are walked once for each turn and once more for the test that fails. No loop is open, so an edge that comes round is an edge forward, into the head of the next turn:
// the counter is a constant in each turn, its test folds, a read under it out of a list that nothing else has is the element, and the list is never made.
// IT IS RIGHT WHATEVER WAS FORESEEN: a turn written out is a turn of the loop. Whatever still comes round behind the last of them enters the loop as it is written.
static constexpr uint32_t mostTurnsWrittenOut = 8;
static constexpr uint32_t mostOperationsWalkedForTurnsWrittenOut = 256;

static constexpr OpcodeSet relationsOfCountersWrittenOut { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
static constexpr OpcodeSet stepsOfCountersWrittenOut { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
static constexpr OpcodeSet argumentsObjectsOfTurnsWrittenOut { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };

namespace {

// What the text of a body makes of known integers. It goes by the operations alone: `leaf` answers for what stands in front (a value from in front of the loop, an entry parameter), `lengthOf` for the list that a length is read off.
struct IntegersByText {
    const BodyIndex& index;
    PropertyKeyID keyOfLength;
    const WTF::Function<std::optional<int64_t>(ValueID)>& leaf;
    const WTF::Function<std::optional<int64_t>(ValueID)>& lengthOf;

    std::optional<int64_t> at(ValueID written, unsigned depth = 0) const
    {
        constexpr unsigned deepestSum = 4;
        ValueID value = written ? index.valueBehindCasts(written) : ValueID();
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
            return lengthOf(index.valueBehindCasts(operation.operand(0)));
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
    for (BlockID member : index.membersOf(loop))
        numberOfOperations += index.block(member).numberOfValues() + 1;
    return numberOfOperations;
}

// How often the test at the head holds. Nothing: the text does not say, or more often than is written out.
// `entering`: what enters the head for the parameter at a position.
static std::optional<uint32_t> foreseeTurnsByText(const IntegersByText& integers, LoopID loop, const WTF::Function<std::optional<int64_t>(uint32_t)>& entering)
{
    const BodyIndex& index = integers.index;
    BodyBlock head = index.block(index.loop(loop).head);
    BodyTerminator end = head.terminator();
    if (!end.isConditionalBranch() || index.isInLoop(end.taken().block(), loop) == index.isInLoop(end.notTaken().block(), loop))
        return std::nullopt;
    bool staysOnTakenEdge = index.isInLoop(end.taken().block(), loop);

    ValueID asked = index.valueBehindCasts(end.operand(0));
    if (!asked || index.isParameter(asked) || !relationsOfCountersWrittenOut.contains(index.opcodeOf(asked)) || !head.holds(asked))
        return std::nullopt;
    BodyValue relation = index.value(asked);
    std::optional<uint32_t> left = head.indexOfParameter(index.valueBehindCasts(relation.operand(0)));
    std::optional<uint32_t> right = head.indexOfParameter(index.valueBehindCasts(relation.operand(1)));
    if (!left == !right)
        return std::nullopt;
    uint32_t position = left ? *left : *right;
    ValueID counter = head.parameter(position);
    std::optional<int64_t> bound = integers.at(relation.operand(left ? 1 : 0));
    std::optional<int64_t> start = entering(position);
    if (!bound || !start || !Range::int32().contains(*bound) || !Range::int32().contains(*start))
        return std::nullopt;

    // Every way round brings the counter of the turn, stepped by one known integer.
    std::optional<int64_t> step;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != head.id())
                continue;
            ValueID brought = position < target.arguments().size() ? index.valueBehindCasts(target.arguments()[position]) : ValueID();
            if (!brought || index.isParameter(brought) || !stepsOfCountersWrittenOut.contains(index.opcodeOf(brought)))
                return std::nullopt;
            BodyValue stepped = index.value(brought);
            if (index.valueBehindCasts(stepped.operand(0)) != counter)
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

// What the text of the loop says, whoever walks it. `isCopy`: the body is that of a callee that does not return in the place of the unit.
static bool textAdmitsTurnsWrittenOut(const BodyIndex& index, BodyLiveness& liveness, LoopID loop, bool isCopy, std::span<const BlockID> handlerBlocks)
{
    BlockID head = index.loop(loop).head;
    if (index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(head) || index.holdsParametersReadByReleaseHintsAlone() || index.loop(loop).numberOfLoopsWithin != 1)
        return false;
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        // As of a loop that is built twice: the block of a return stays open for walkInline().
        TerminatorKind end = block.terminator().kind();
        if (isCopy && (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall))
            return false;
        if (block.hasHandler() || block.isHandlerBlock())
            return false;
        for (BodyValue operation : block.values()) {
            if (maySuspendAsWritten(operation))
                return false;
        }
    }
    // A handler block takes no names from an edge.
    if (!liveness.isComputedFor(index))
        liveness.compute(index);
    uint32_t firstValue = index.block(head).firstParameter().index();
    ValueID endOfValues = index.block(index.loop(loop).last).endOfValues();
    for (BlockID handlerBlock : handlerBlocks) {
        bool readsWhatLoopDefines = false;
        liveness.forEachLiveAtHead(handlerBlock, ValueID(firstValue), endOfValues, [&](ValueID live) {
            readsWhatLoopDefines |= index.isInLoop(index.blockOf(live), loop);
        });
        if (readsWhatLoopDefines)
            return false;
    }
    return true;
}

static std::optional<int64_t> integerBuiltForTurnsWrittenOut(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isConstant() || walker.code().at(reference.id()).opcode != Opcode::Integer)
        return std::nullopt;
    return walker.code().at(reference.id()).integer();
}

// ---- The callee whose loops run through the list of a call

// Every mention of the list in the body reads its length or an element, which changes nothing of it and hands it to nobody. So in a copy it stays what the call has made, and nothing but the copy has it.
static bool bodyOnlyReadsListOfCall(const BodyIndex& index, PropertyKeyID keyOfLength, ValueID list)
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
        if (opcode == Opcode::ReleaseHint || (opcode == Opcode::PropGet && !user.isFreeName() && user.key() == keyOfLength) || (opcode == Opcode::ElemGet && user.propertyOperand() != list))
            continue;
        return false;
    }
    // What is left stands at the end of a block or on an edge.
    return !mentionsLeft;
}

std::optional<TextOfCopyRead> turnsOfLoopsOfCopyWrittenOut(const Walker& walker, FunctionID callee, std::span<const std::optional<int64_t>> integersHandedOver, std::span<const std::optional<uint32_t>> lengthsOfListsHandedOver)
{
    const BodyIndex* found = walker.facts().bodies().find(callee);
    // The turns only add to what the body has, so a larger body is not read at all: what one question costs does not grow with the callee.
    if (!found || !found->hasLoops() || found->hasIrreducibleFlow() || walker.facts().summaries().at(callee).numberOfOperations > mostOperationsWalkedForTurnsWrittenOut)
        return std::nullopt;
    BodyLiveness liveness;
    Vector<uint32_t, 2> turns;
    const BodyIndex& index = *found;
    PropertyKeyID keyOfLength = walker.program().keyOf(BuiltinNames::length);
    BodyBlock entry = index.block(entryBlock);

    // THE CALLEE IS READ WITH WHAT THE CALL HANDS OVER, block by block in the order of the stream, in which every edge but one that comes round leads forward.
    // A Branch on a known answer leads one way, and a parameter that every way in brings the same integer is that integer. By value of the body, behind casts.
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
        if (!list || !bodyOnlyReadsListOfCall(index, keyOfLength, list))
            return std::nullopt;
        if (std::optional<uint32_t> parameter = entry.indexOfParameter(list)) {
            if (*parameter >= lengthsOfListsHandedOver.size() || !lengthsOfListsHandedOver[*parameter])
                return std::nullopt;
            return *lengthsOfListsHandedOver[*parameter];
        }
        // `arguments.length`.
        if (index.isParameter(list) || !argumentsObjectsOfTurnsWrittenOut.contains(index.opcodeOf(list)))
            return std::nullopt;
        ValueID count = index.valueBehindCasts(index.value(list).countOperand());
        return count ? known[count.index()] : std::nullopt;
    };
    IntegersByText integers { index, keyOfLength, leaf, lengthOf };

    auto answerAt = [&](ValueID written) -> std::optional<bool> {
        bool isNegated = false;
        for (ValueID value = index.valueBehindCasts(written); value && !index.isParameter(value); value = index.valueBehindCasts(index.value(value).operand(0))) {
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
    uint32_t numberOfOperations = walker.facts().summaries().at(callee).numberOfOperations;
    turns.fill(0, index.numberOfLoops());
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!isEntered.get(number))
            continue;
        BodyBlock block = index.block(BlockID(number));
        LoopID loop = index.loopOf(block.id());
        if (loop && index.loop(loop).head == block.id()) {
            if (index.loop(loop).parent || !textAdmitsTurnsWrittenOut(index, liveness, loop, true, handlerBlocks.span()))
                return std::nullopt;
            // What the ways in have told is what enters. In the loop the parameters are other values in every turn.
            Vector<std::optional<int64_t>, 8> entering;
            for (uint32_t parameter = 0; parameter < block.numberOfParameters(); ++parameter)
                entering.append(std::exchange(known[block.parameter(parameter).index()], std::nullopt));
            WTF::Function<std::optional<int64_t>(uint32_t)> whatEnters = [&](uint32_t position) -> std::optional<int64_t> {
                return position < entering.size() ? entering[position] : std::nullopt;
            };
            std::optional<uint32_t> stays = foreseeTurnsByText(integers, loop, whatEnters);
            if (!stays)
                return std::nullopt;
            numberOfOperations += numberOfOperationsOfLoopWrittenOut(index, loop) * *stays;
            turns[loop.index()] = *stays;
        }

        for (BodyValue operation : block.values()) {
            if (index.valueBehindCasts(operation.id()) != operation.id())
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
            // What comes round.
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
        if (block.hasHandler())
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
    return TextOfCopyRead { WTF::move(turns), WTF::move(isEntered), WTF::move(leadsToTakenAlone), WTF::move(leadsToNotTakenAlone) };
}

// ---- The turns

bool Walker::writeOutTurnsOfLoop(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Folding) || !has(Mechanism::LoopInvariants) || isUnreachable() || !index.isLoopHead(inputHead))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;
    if (m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)) || hasTextsInStringBuilders())
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
    std::optional<uint32_t> stays = foreseeTurnsByText(IntegersByText { index, keyOfLength, leaf, lengthOf }, loop, whatEnters);
    if (!stays || numberOfOperationsOfLoopWrittenOut(index, loop) * (*stays + 1) > mostOperationsWalkedForTurnsWrittenOut)
        return false;

    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, handlerBlocksOfFrameOnTop()))
        return false;

    beginTurnsWrittenOut(inputHead);
    walkTurnsWrittenOut(inputHead, entering, *stays + 1, nullptr);
    return true;
}

std::span<const BlockID> Walker::handlerBlocksOfFrameOnTop()
{
    Frame& frame = m_frames.last();
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
    // The values of the loop have the numbers up to the end of its last block: what one loop costs does not grow with the body.
    ValueID endOfValues = index.block(index.loop(loop).last).endOfValues();
    // What the loop defines and code behind it reads: each turn has its own, so each edge that leaves brings them. Walker::buildLoopTwice().
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
        if (m_frames[frameIndex].valuesBuiltTwice.contains(value))
            continue;
        m_frames[frameIndex].valuesBuiltTwice.append(value);
        m_frames[frameIndex].valuesListedForTurnsWrittenOut.append(value);
    }
    m_frames[frameIndex].loopsBeingBuiltTwice.append(loop);
    m_frames[frameIndex].standsInFrontOfHead = false;
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
        takeNamesOntoEdge(open, frameIndex, inputHead);
        open.names.append({ turns.step, position });
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
    uint32_t namesInFrontOfLoop = inputBlock(frameIndex, inputHead).firstNarrowerName;
    // No loop stands inside this one, but a copy of a callee has a frame of its own: nobody writes over the list meanwhile.
    Vector<ValueID, 4> listedForTheseTurns = std::exchange(m_frames[frameIndex].valuesListedForTurnsWrittenOut, { });

    // The block in front enters the first turn alone.
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
        takeNamesOntoEdge(open, frameIndex, inputHead);
    }
    m_block = { };
    m_state.clear();

    // Each walk of the loop begins as the first: nothing stands for a value of the loop yet. A link that looks ahead in the text would find what the turn before has bound.
    auto beginWalkOfLoop = [&] {
        uint32_t cameRound = inputBlock(frameIndex, inputHead).firstOpenEdge;
        for (BlockID member : index.membersOf(loop)) {
            inputBlock(frameIndex, member) = InputBlock { };
            BodyBlock block = index.block(member);
            for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
                m_frames[frameIndex].references[value] = Reference();
        }
        inputBlock(frameIndex, inputHead).firstOpenEdge = cameRound;
        inputBlock(frameIndex, inputHead).firstNarrowerName = namesInFrontOfLoop;
    };

    size_t numberOfLoopsAround = m_openLoops.size();
    BlockID headBefore = std::exchange(m_frames[frameIndex].headOfTurnsWrittenOut, inputHead);
    TurnsOfForInWrittenOut* forInBefore = std::exchange(m_frames[frameIndex].turnsOfForIn, forIn);
    for (uint32_t turn = 0; turn < numberOfWalks && !m_hasFailed && inputBlock(frameIndex, inputHead).firstOpenEdge != noOpenEdge; ++turn) {
        beginWalkOfLoop();
        for (BlockID member : index.membersOf(loop)) {
            if (m_hasFailed)
                break;
            walkBlock(member);
        }
        endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    }
    m_frames[frameIndex].headOfTurnsWrittenOut = headBefore;
    m_frames[frameIndex].turnsOfForIn = forInBefore;

    // The loop as written steps the enumeration from wherever an edge has left the turns.
    if (forIn && !m_hasFailed) {
        IterationByIndex& iteration = m_iterationsByIndex[forIn->place];
        for (uint32_t cameRound = inputBlock(frameIndex, inputHead).firstOpenEdge; cameRound != noOpenEdge; cameRound = m_openEdges[cameRound].next)
            m_openEdges[cameRound].names.append({ forIn->step, iteration.position });
        for (uint32_t waiting : forIn->edgesIntoLoopAsWritten)
            m_openEdges[waiting].next = std::exchange(inputBlock(frameIndex, inputHead).firstOpenEdge, waiting);
        iteration.takenAtOpen.shrink(0);
        m_frames[frameIndex].stepWhosePositionEdgesBring = forIn->step;
        m_frames[frameIndex].placeOfForInWhosePositionEdgesBring = forIn->place;
    }

    // Whatever still comes round enters the loop as it is written.
    if (!m_hasFailed && inputBlock(frameIndex, inputHead).firstOpenEdge != noOpenEdge) {
        beginWalkOfLoop();
        for (BlockID member : index.membersOf(loop)) {
            if (m_hasFailed)
                break;
            walkBlock(member);
        }
        endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    }

    // Behind the loop nothing of a turn is at hand but what the edges bring.
    for (BlockID member : index.membersOf(loop)) {
        inputBlock(frameIndex, member).firstNarrowerName = namesInFrontOfLoop;
        BodyBlock block = index.block(member);
        for (uint32_t value = block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            m_frames[frameIndex].references[value] = Reference();
    }
    m_frames[frameIndex].loopsBeingBuiltTwice.removeLast();
    m_frames[frameIndex].stepWhosePositionEdgesBring = { };
    // Every edge asks each listed value whether it is read behind it. Where ONE block takes all that leaves the loop, every edge into it is led by now, and it dominates whatever reads a value of the loop:
    // the join there gives each its name for good, and no later edge need bring it. So a body of many such loops lists a few values and not all of them.
    // Whoever walks that block for some of its ways alone lists them again: KnownThreading::letTerminatorBeWalkedForEachWay().
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
    if (leavesByOneBlock) {
        m_frames[frameIndex].valuesBuiltTwice.removeAllMatching([&](ValueID value) {
            return listedForTheseTurns.contains(value);
        });
    }
    m_block = { };
    m_state.clear();
}

// ---- The turns of a for-in

// What the head of a turn has seen of the object holds at the step: nothing that anybody sees stands between the two.
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
    // No key of any object can have changed since the tests, or since the step of the turn before.
    bool noKeyHasChangedSince = m_state.factsOfCell(turns.enumerator).contains(FactOfCell::IsLikelyAsAllocated);
    bool isFirstTurn = !std::exchange(turns.enumeratorIsAsked, true);
    if (!isFirstTurn && noKeyHasChangedSince)
        return;
    Reference position = m_iterationsByIndex[turns.place].position;
    Reference isCurrent;
    if (isFirstTurn && noKeyHasChangedSince && turns.namesCountedByOpen) {
        // So many names were current where the open answered, none where it has entered the run time, and no key has changed since.
        Draft names(Opcode::Integer, m_origin);
        names.setInteger(turns.numberOfNames);
        Draft test(Opcode::GreaterEq, m_origin);
        chooseConservativeFields(test);
        test.setLeftOperand(turns.namesCountedByOpen);
        test.setRightOperand(constant(WTF::move(names)));
        test.setForm(formAllowedForTypedOperator(*this, test));
        isCurrent = build(WTF::move(test));
    } else {
        // What a step asks before it hands out a name without entering the run time.
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
        m_state.noteFactsOfCell(turns.enumerator, FactOfCell::IsLikelyAsAllocated);
}

bool Walker::writeOutTurnsOfForIn(BlockID inputHead, std::span<const Reference> entering)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (!has(Mechanism::Folding) || !has(Mechanism::TypedOperations) || !has(Mechanism::LoopInvariants) || !has(Mechanism::Guesses) || !m_opensGuesses || isUnreachable() || hasTextsInStringBuilders())
        return false;
    if (!index.isLoopHead(inputHead) || !m_inputHeadsOfIterations.contains(inputHeadInFrame(m_frames[frameIndex].inlineFrame, inputHead)))
        return false;
    LoopID loop = index.loopOf(inputHead);
    if (m_frames[frameIndex].loopsBeingBuiltTwice.contains(loop))
        return false;

    BodyBlock head = index.block(inputHead);
    ValueID step = stepInFrontOfEffectsInTurnsWrittenOut(index, head);
    ValueID open = step ? index.value(step).enumeratorOperand() : ValueID();
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::ForInOpen)
        return false;
    ValueID objectInBody = index.value(step).objectOperand();
    if (index.valueBehindCasts(objectInBody) != index.valueBehindCasts(index.value(open).objectOperand()))
        return false;
    Reference state = referenceOf(open);
    if (!state || !state.isVirtual())
        return false;

    TurnsOfForInWrittenOut turns;
    turns.place = state.object().index();
    turns.step = step;
    const IterationByIndex& asItEntered = m_iterationsByIndex[turns.place];
    bool isOpenedInFrontOfThisLoop = asItEntered.otherForm == IterationByIndex::OtherForm::Enumeration && !asItEntered.head && asItEntered.inputHead == inputHead && asItEntered.inlineFrame == m_frames[frameIndex].inlineFrame;
    if (!isOpenedInFrontOfThisLoop || !asItEntered.list || asItEntered.builtinClass || !asItEntered.takenAtOpen.isEmpty())
        return false;
    turns.enumerator = asItEntered.list;
    Reference firstPosition = asItEntered.position;
    if (integerBuiltForTurnsWrittenOut(*this, firstPosition) != std::optional<int64_t>(0))
        return false;
    // The tests say which keys the object has here, and the enumerator lists those that it had at the open.
    if (!m_state.factsOfCell(turns.enumerator).contains(FactOfCell::IsLikelyAsAllocated))
        return false;
    Reference object = referenceOf(objectInBody);
    if (!object || !object.isValue())
        return false;

    std::optional<OwnKeyOrder> inSight = ownKeysInSight(*this, object);
    OwnKeyPlan planned = inSight ? OwnKeyPlan() : planOwnKeyWalk(*this, objectInBody, object);
    if (!inSight && !planned)
        return false;
    // The likeliest order. The objects of the others take the loop as written.
    const OwnKeyOrder& order = inSight ? *inSight : planned.orders[0];
    uint32_t numberOfNames = static_cast<uint32_t>(order.fields.size());
    if (!order.layout || !numberOfNames || numberOfNames > mostTurnsWrittenOut || numberOfOperationsOfLoopWrittenOut(index, loop) * (numberOfNames + 1) > mostOperationsWalkedForTurnsWrittenOut)
        return false;
    if (!textAdmitsTurnsWrittenOut(index, m_liveness, loop, frameIndex && !m_frames[frameIndex].returnsInPlaceOfUnit, handlerBlocksOfFrameOnTop()))
        return false;
    if (!inSight && !shouldOpenGuess(*this, object, m_types.forLayout(order.layout), planned.isDeclared, numberOfNames))
        return false;

    Origin origin = originOf(index.value(step));
    Vector<Reference, 4> names;
    for (FieldIndex field : order.fields) {
        PropertyKeyID key = plan().slots().keyOf(program(), FieldReference { order.layout, field });
        // An index is listed in front of every name, whenever the object got it.
        if (!key || liesInThread(key) || program().at(key).kind != PropertyKeyKind::Text || program().isIndexLike(key))
            return false;
        Draft name(Opcode::String, origin);
        name.setString(program().at(key).text);
        names.append(constant(WTF::move(name)));
    }

    beginTurnsWrittenOut(inputHead);
    turns.object = object;
    if (!inSight) {
        BlockID otherwise = addBlock(BlockFrequency::Unlikely);
        bool hasKeys = goOnWhereObjectHasOwnKeys(*this, turns.object, order, planned.layoutsAreProven, origin, otherwise);
        noteGuessOpened();
        BlockID passed = hasKeys ? m_block : BlockID();
        WalkState stateWhereTestsPassed = WTF::move(m_state);
        continueIn(otherwise);
        if (!isUnreachable())
            leadIntoLoopAsWrittenBehindTurns(turns, inputHead, entering, firstPosition);
        m_block = passed;
        m_state = WTF::move(stateWhereTestsPassed);
        // Wherever this state comes to, the object goes by the name behind the test of its shape. The edges into the loop as written do not all bring it.
        if (passed && turns.object != object && !m_state.checkedNameOf(object))
            m_state.noteCheckedName(object, turns.object);
    }
    if (m_hasFailed)
        return true;
    m_iterationsByIndex[turns.place].takenAtOpen = names;
    // What comes round behind the turns is the position alone.
    turns.namesCountedByOpen = std::exchange(m_iterationsByIndex[turns.place].count, Reference());
    turns.numberOfNames = numberOfNames;
    count(StatisticID::OwnKeyWalksExpanded);
    walkTurnsWrittenOut(inputHead, entering, numberOfNames, &turns);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
