/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTSweep.h"
#include <array>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet opcodesOfWalkForIn { Opcode::ForInOpen, Opcode::ForInStep };
static constexpr uint32_t closedOperationsOfStepOverListInWalkForIn = 2;

namespace {

struct LoopOfForInInWalkForIn {
    ValueID step;
    BlockID head;
    bool isEnteredBehindOpen { false };
};

struct StepInWalkForIn {
    Reference done;
    Reference key;
    Reference foundAt;
    Reference next;
    Reference count;
};

}

static Reference integerInWalkForIn(Walker& walker, int64_t value, Origin origin)
{
    Draft written(Opcode::Integer, origin);
    written.setInteger(value);
    return walker.constant(WTF::move(written));
}

static Reference booleanInWalkForIn(Walker& walker, bool value, Origin origin)
{
    Draft written(Opcode::Boolean, origin);
    written.setBoolean(value);
    return walker.constant(WTF::move(written));
}

static std::optional<int64_t> integerHeldInWalkForIn(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isConstant() || walker.code().at(reference.id()).opcode != Opcode::Integer)
        return std::nullopt;
    return walker.code().at(reference.id()).integer();
}

static Reference buildInWalkForIn(Walker& walker, Draft&& operation)
{
    if (walker.isUnreachable())
        return { };
    walker.chooseConservativeFields(operation);
    return walker.build(WTF::move(operation));
}

static Reference buildOperatorInWalkForIn(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    if (walker.isUnreachable() || !left)
        return { };
    Draft operation(opcode, origin);
    walker.chooseConservativeFields(operation);
    operation.setOperand(0, left);
    if (right)
        operation.setOperand(1, right);
    operation.setForm(operatorFormFor(walker, operation));
    return boxNumberIfNeeded(walker, walker.build(WTF::move(operation)), origin);
}

static Reference resultAtInWalkForIn(Walker& walker, Reference results, uint32_t index, Origin origin)
{
    if (!results)
        return { };
    Draft read(Opcode::ResultAt, origin);
    read.setResultsOperand(results);
    read.setIndex(index);
    return buildInWalkForIn(walker, WTF::move(read));
}

static Reference resultsInWalkForIn(Walker& walker, Reference done, Reference key, Origin origin)
{
    if (!done || !key)
        return { };
    Draft results(Opcode::Results, origin);
    results.setValuesOperands(std::array { done, key });
    return buildInWalkForIn(walker, WTF::move(results));
}

static Reference narrowedInWalkForIn(Walker& walker, Reference value, Reference test, uint32_t operandIndex, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(true);
    return walker.isUnreachable() ? Reference() : walker.build(WTF::move(proof));
}

void markEnumeratorCurrent(Walker& walker, Reference enumerator, Reference object)
{
    if (object.isValue() && walker.state().isFresh(Reference::value(underlyingCellOf(walker.code(), object.id())))) {
        walker.fail("an enumerator is marked current for an object that is still fresh"_s, InternalErrorKind::EnumeratorCurrentForFreshObject);
        return;
    }
    walker.state().recordCellFacts(enumerator, CellFact::EnumeratorIsCurrent);
}

static bool isEnumeratorKnownCurrent(Walker& walker, Reference enumerator)
{
    return walker.state().cellFacts(enumerator).contains(CellFact::EnumeratorIsCurrent);
}

bool countOfNamesComesRound(Walker& walker, const Walker::IndexedIteration& iteration)
{
    if (iteration.otherForm != Walker::IndexedIteration::OtherForm::Enumeration || !iteration.count || !iteration.list)
        return false;
    if (iteration.head)
        return isEnumeratorKnownCurrent(walker, iteration.list);
    markEnumeratorCurrent(walker, iteration.list, iteration.subject);
    return true;
}

static std::optional<LoopOfForInInWalkForIn> findLoopOfForInInWalkForIn(const BodyIndex& index, const BodyValue& open)
{
    if (index.hasIrreducibleFlow())
        return std::nullopt;
    ValueID step = index.onlyUsingValue(open.id());
    if (!step || index.opcodeOf(step) != Opcode::ForInStep || index.value(step).enumeratorOperand() != open.id())
        return std::nullopt;
    BlockID head = index.blockOf(step);
    BodyBlock blockOfOpen = index.block(index.blockOf(open.id()));
    if (!index.isLoopHead(head) || index.isEnteredByHandlerEdge(head))
        return std::nullopt;
    const BodyLoop& loop = index.loop(index.loopOf(head));
    if (loop.head != head || loop.parent != index.loopOf(blockOfOpen.id()))
        return std::nullopt;

    LoopOfForInInWalkForIn found { step, head, false };
    BodyTerminator end = blockOfOpen.terminator();
    found.isEnteredBehindOpen = open.id().index() + 1 == blockOfOpen.endOfValues().index() && end.kind() == TerminatorKind::Jump && end.next().block() == head
        && index.stripCasts(index.value(step).objectOperand()) == index.stripCasts(open.objectOperand());
    return found;
}

static bool isExpectedToBeListInWalkForIn(Walker& walker, ValueID input, Reference object, bool& isDeclared)
{
    TypeArena& types = walker.types();
    TypeID lists = types.builtinClass(BuiltinClasses::Array);
    TypeID proven = walker.typeOf(object);
    if (!proven || !types.overlap(proven, lists))
        return false;
    if (types.isSubset(types.withoutNullish(proven), lists)) {
        isDeclared = true;
        return true;
    }
    ExpectationOfValue expectation = findExpectation(walker, input);
    if (!expectation.type)
        return false;
    TypeID expected = types.withoutNullish(expectation.type);
    isDeclared = expectation.isDeclared;
    return !isNeverType(expected) && types.isSubset(expected, lists);
}

static bool openInWalkForIn(Walker& walker, const BodyValue& open)
{
    std::optional<LoopOfForInInWalkForIn> loop = findLoopOfForInInWalkForIn(walker.currentBodyIndex(), open);
    Reference object = loop ? walker.referenceOf(open.objectOperand()) : Reference();
    if (!object || !object.isRecord())
        return false;
    Origin origin = walker.originOf(open);

    Walker::IndexedIteration iteration;
    iteration.otherForm = Walker::IndexedIteration::OtherForm::Enumeration;
    iteration.subject = object;
    iteration.isDeadAfterLoop = true;
    if (walker.types().at(walker.typeOf(object)).isUndefinedOrNull()) {
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
        return true;
    }

    bool isDeclared = false;
    bool isExpectedToBeList = walker.has(Mechanism::Guesses) && isExpectedToBeListInWalkForIn(walker, open.objectOperand(), object, isDeclared);
    if (loop->isEnteredBehindOpen && !isExpectedToBeList) {
        Draft counting(Opcode::ForInOpenCountingNames, origin);
        counting.setObjectOperand(object);
        Reference results = walker.build(WTF::move(counting));
        iteration.list = resultAtInWalkForIn(walker, results, static_cast<uint32_t>(ForInOpenResult::Enumerator), origin);
        iteration.count = resultAtInWalkForIn(walker, results, static_cast<uint32_t>(ForInOpenResult::CurrentNames), origin);
    } else {
        Draft asWritten(Opcode::ForInOpen, origin);
        asWritten.setObjectOperand(object);
        iteration.list = walker.build(WTF::move(asWritten));
    }
    if (!iteration.list)
        return true;
    markEnumeratorCurrent(walker, iteration.list, iteration.subject);

    iteration.position = integerInWalkForIn(walker, 0, origin);
    iteration.inputHead = loop->head;
    if (isExpectedToBeList)
        iteration.builtinClass = BuiltinClasses::Array;
    walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

static StepInWalkForIn buildStepFromInWalkForIn(Walker& walker, const Walker::IndexedIteration& iteration, Reference object, Reference position, Origin origin)
{
    StepInWalkForIn built;
    if (walker.isUnreachable())
        return built;
    Draft step(Opcode::ForInStep, origin);
    step.setEnumeratorOperand(iteration.list);
    step.setObjectOperand(object);
    step.setPositionOperand(position);
    if (iteration.count)
        step.setCurrentNamesOperand(isEnumeratorKnownCurrent(walker, iteration.list) ? iteration.count : integerInWalkForIn(walker, 0, origin));
    Reference results = walker.build(WTF::move(step));
    if (!results)
        return built;
    built.done = resultAtInWalkForIn(walker, results, static_cast<uint32_t>(ForInStepResult::Done), origin);
    built.key = resultAtInWalkForIn(walker, results, static_cast<uint32_t>(ForInStepResult::Key), origin);
    built.foundAt = resultAtInWalkForIn(walker, results, static_cast<uint32_t>(ForInStepResult::Position), origin);
    if (iteration.count)
        built.count = resultAtInWalkForIn(walker, results, static_cast<uint32_t>(ForInStepResult::CurrentNames), origin);
    built.next = buildOperatorInWalkForIn(walker, Opcode::Increment, built.foundAt, { }, origin);
    markEnumeratorCurrent(walker, iteration.list, iteration.subject);
    return built;
}

static StepInWalkForIn buildStepOverListInWalkForIn(Walker& walker, Walker::IndexedIteration& iteration, Reference object, Origin origin)
{
    TypeArena& types = walker.types();
    Draft length(Opcode::GetEnumeratorIndexedLength, origin);
    length.setEnumeratorOperand(iteration.list);
    Reference indexedLength = buildInWalkForIn(walker, WTF::move(length));
    Reference isInIndexedPart = buildOperatorInWalkForIn(walker, Opcode::Less, iteration.position, indexedLength, origin);
    if (!isInIndexedPart)
        return { };

    BlockID mayBeIndexed = walker.addBlock();
    BlockID indexed = walker.addBlock();
    BlockID byStep = walker.addBlock(BlockFrequency::Unlikely);
    BlockID after = walker.addBlock();
    TypeID positions = types.integer(Range { 0, Walker::doneIterationPosition });
    StepInWalkForIn met;
    met.done = walker.addParameter(after, { }, walker.formOfParameter(TypeTable::boolean, { }));
    met.key = walker.addParameter(after, { }, walker.formOfParameter(TypeTable::string, { }));
    met.foundAt = walker.addParameter(after, { }, walker.formOfParameter(positions, { }));
    met.next = walker.addParameter(after, { }, walker.formOfParameter(positions, { }));
    Reference keyForAccesses = walker.addParameter(after, { }, Representation::JSValue);
    walker.branch(isInIndexedPart, mayBeIndexed, byStep);
    walker.recordSpeculation();

    walker.continueIn(mayBeIndexed);
    Reference inside = narrowedInWalkForIn(walker, iteration.position, isInIndexedPart, OperandIndex::Less::left, origin);
    Draft look(Opcode::IsElementInStorage, origin);
    look.setObjectOperand(object);
    look.setPositionOperand(inside);
    Reference isInStorage = inside ? buildInWalkForIn(walker, WTF::move(look)) : Reference();
    if (isInStorage)
        walker.branch(isInStorage, indexed, byStep);

    walker.continueIn(indexed);
    Reference name = buildOperatorInWalkForIn(walker, Opcode::ToString, inside, { }, origin);
    Reference behind = buildOperatorInWalkForIn(walker, Opcode::Increment, inside, { }, origin);
    if (name && behind)
        walker.jumpTo(after, std::array { booleanInWalkForIn(walker, false, origin), name, inside, behind, inside });

    walker.continueIn(byStep);
    StepInWalkForIn asWritten = buildStepFromInWalkForIn(walker, iteration, object, iteration.position, origin);
    if (asWritten.done && asWritten.key && asWritten.foundAt && asWritten.next)
        walker.jumpTo(after, std::array { asWritten.done, asWritten.key, asWritten.foundAt, asWritten.next, asWritten.key });

    walker.continueIn(after);
    if (walker.isUnreachable())
        return { };
    iteration.keyForAccesses = keyForAccesses;
    return met;
}

static bool stepInWalkForIn(Walker& walker, const BodyValue& step)
{
    Reference state = walker.referenceOf(step.enumeratorOperand());
    if (!state || !state.isVirtual() || walker.indexedIteration(state).otherForm != Walker::IndexedIteration::OtherForm::Enumeration)
        return false;
    Walker::IndexedIteration& iteration = walker.indexedIteration(state);
    Origin origin = walker.originOf(step);
    iteration.newestStep = step.id();
    iteration.keyForAccesses = { };

    if (!iteration.list) {
        Draft nothing(Opcode::String, origin);
        nothing.setString(walker.program().strings.emptyString());
        iteration.foundAt = { };
        if (Reference results = resultsInWalkForIn(walker, booleanInWalkForIn(walker, true, origin), walker.constant(WTF::move(nothing)), origin))
            walker.bind(step.id(), results);
        return true;
    }

    if (std::optional<int64_t> turn = integerHeldInWalkForIn(walker, iteration.position); turn && static_cast<uint64_t>(*turn) < iteration.precomputedSteps.size()) {
        iteration.foundAt = iteration.position;
        iteration.position = integerInWalkForIn(walker, *turn + 1, origin);
        if (Reference results = resultsInWalkForIn(walker, booleanInWalkForIn(walker, false, origin), iteration.precomputedSteps[static_cast<size_t>(*turn)], origin))
            walker.bind(step.id(), results);
        return true;
    }

    Reference object = walker.referenceOf(step.objectOperand());
    if (!object || !object.isRecord()) {
        walker.fail("the step of a for-in asks what is no record of Code"_s);
        return true;
    }
    if (iteration.count && (!iteration.head || walker.code().blockOf(iteration.count.id()) != iteration.head))
        iteration.count = { };
    bool isDeclared = false;
    bool looksAtStorage = iteration.builtinClass == BuiltinClasses::Array && object.isValue() && walker.has(Mechanism::Guesses) && walker.opensGuesses()
        && isExpectedToBeListInWalkForIn(walker, step.objectOperand(), object, isDeclared)
        && shouldOpenGuess(walker, object, walker.types().builtinClass(BuiltinClasses::Array), isDeclared, closedOperationsOfStepOverListInWalkForIn);
    if (looksAtStorage)
        walker.count(StatisticID::OwnKeyWalksExpanded);
    StepInWalkForIn built = looksAtStorage ? buildStepOverListInWalkForIn(walker, iteration, object, origin) : buildStepFromInWalkForIn(walker, iteration, object, iteration.position, origin);
    if (!built.done || !built.key || !built.foundAt || !built.next)
        return true;
    iteration.position = built.next;
    iteration.foundAt = built.foundAt;
    if (iteration.count)
        iteration.count = built.count;
    if (Reference results = resultsInWalkForIn(walker, built.done, built.key, origin))
        walker.bind(step.id(), results);
    return true;
}

bool buildTypedForInOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!opcodesOfWalkForIn.contains(value.opcode()))
        return false;
    return value.opcode() == Opcode::ForInOpen ? openInWalkForIn(walker, value) : stepInWalkForIn(walker, value);
}

bool Walker::canVersionLoopWithIterations(BlockID inputHead) const
{
    uint32_t numberInFront = m_numberOfIterationsInFrontOfTheirLoops;
    for (size_t place = m_indexedIterations.size(); place-- && numberInFront;) {
        const IndexedIteration& iteration = m_indexedIterations[place];
        if (iteration.head || !iteration.inputHead)
            continue;
        --numberInFront;
        if (iteration.inputHead == inputHead && iteration.inlineFrame == m_frames.last().inlineFrame && !iteration.isDeadAfterLoop)
            return false;
    }
    return true;
}

bool Walker::iterationsSteppedAtHandOverPositionsAlone(BlockID inputHead) const
{
    uint32_t numberInFront = m_numberOfIterationsInFrontOfTheirLoops;
    for (size_t place = m_indexedIterations.size(); place-- && numberInFront;) {
        const IndexedIteration& iteration = m_indexedIterations[place];
        if (iteration.head || !iteration.inputHead)
            continue;
        --numberInFront;
        if (iteration.inputHead != inputHead || iteration.inlineFrame != m_frames.last().inlineFrame)
            continue;
        if ((!iteration.isIndexedArrayIteration() && !iteration.isIndexedTypedArrayIteration() && !iteration.isIndexedStringIteration()) || !iteration.isDeadAfterLoop)
            return false;
    }
    return true;
}

Walker::IterationsSteppedAtHead Walker::takeIterationsSteppedAt(BlockID inputHead) const
{
    IterationsSteppedAtHead taken;
    uint32_t numberInFront = m_numberOfIterationsInFrontOfTheirLoops;
    for (size_t place = m_indexedIterations.size(); place-- && numberInFront;) {
        const IndexedIteration& iteration = m_indexedIterations[place];
        if (iteration.head || !iteration.inputHead)
            continue;
        --numberInFront;
        if (iteration.inputHead == inputHead && iteration.inlineFrame == m_frames.last().inlineFrame)
            taken.asTheyEntered.append(std::pair { static_cast<uint32_t>(place), iteration });
    }
    return taken;
}

void Walker::putBackIterationsSteppedAt(const IterationsSteppedAtHead& taken)
{
    for (const auto& [place, iteration] : taken.asTheyEntered) {
        if (m_indexedIterations[place].head)
            ++m_numberOfIterationsInFrontOfTheirLoops;
        m_indexedIterations[place] = iteration;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
