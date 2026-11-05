/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCountersOfLoops.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTWalkListReadsInLoops.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet stepsByOneOfCountersOfLoops { Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet stepsByNumberOfCountersOfLoops { Opcode::Add, Opcode::Sub };
static constexpr OpcodeSet operatorsOnBitsOfCountersOfLoops { Opcode::BitOr, Opcode::BitAnd, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift };
static constexpr OpcodeSet accessesUnderKeysInCountersOfLoops { Opcode::ElemGet, Opcode::ElemSet };
static constexpr size_t mostNamesOfCounterOfLoop = 16;

using NamesOfCounterOfLoop = Vector<ValueID, mostNamesOfCounterOfLoop>;

static bool mayBeInt32InCountersOfLoops(const Walker& walker, Reference value)
{
    if (!value || !value.isValue())
        return false;
    Type type = walker.types().at(walker.typeOf(value));
    return type.isJavaScriptValue() && type.primitives & Primitive::Integer && !type.isInteger();
}

static bool isBoxOrDoubleThatMayBeInt32InCountersOfLoops(const Walker& walker, Reference value)
{
    if (!mayBeInt32InCountersOfLoops(walker, value))
        return false;
    Representation held = walker.representationOf(value);
    return held == Representation::JSValue || (held == Representation::Double && walker.types().at(walker.typeOf(value)).isNumber());
}

static bool isWholeNumberAsWrittenInCountersOfLoops(const BodyIndex& index, ValueID value)
{
    ValueID written = index.valueBehindCasts(value);
    return written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer;
}

static bool hasNoFractionByLookInCountersOfLoops(Walker& walker, const BodyIndex& index, const LoopPreview& preview, ValueID value)
{
    ValueID written = index.valueBehindCasts(value);
    if (!written)
        return false;
    TypeID type;
    if (written.index() >= preview.firstValue) {
        size_t place = written.index() - preview.firstValue;
        type = place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
    } else if (Reference built = walker.referenceOf(written))
        type = walker.typeOf(built);
    return type && !isNeverType(type) && walker.types().at(type).primitives & Primitive::Integer && walker.types().at(type).hasOnlyPrimitives(Primitive::NumberWithoutFraction);
}

static bool isSteppedAllRoundInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position, NamesOfCounterOfLoop& names, const LoopPreview* typesOfSteps = nullptr, Vector<ValueID, 2>* stepsAskedInFront = nullptr)
{
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    bool isStepped = false;
    auto add = [&](ValueID name) {
        if (names.size() < mostNamesOfCounterOfLoop)
            names.append(name);
    };
    names.append(index.block(inputHead).parameter(position));
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        if (member != inputHead && !index.isEnteredByHandlerEdge(member)) {
            std::span<const BodyIncomingEdge> incoming = walker.incomingEdgesOf(index, member);
            for (uint32_t place = 0; place < block.numberOfParameters(); ++place) {
                ValueID parameter = block.parameter(place);
                if (ValueID narrowed = index.valueNarrowedInto(parameter)) {
                    if (names.contains(narrowed))
                        add(parameter);
                    continue;
                }
                bool joinsNamesAlone = !incoming.empty() && std::ranges::all_of(incoming, [&](const BodyIncomingEdge& edge) {
                    std::span<const ValueID> arguments = edge.target.arguments();
                    return place < arguments.size() && names.contains(arguments[place]);
                });
                if (joinsNamesAlone)
                    add(parameter);
            }
        }
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            if (opcodesThatStateTypes.contains(opcode)) {
                if (names.contains(operation.operand(0)))
                    add(operation.id());
                continue;
            }
            bool steps = false;
            if (stepsByOneOfCountersOfLoops.contains(opcode))
                steps = names.contains(operation.operand(0));
            else if (stepsByNumberOfCountersOfLoops.contains(opcode)) {
                auto isStep = [&](ValueID other) {
                    if (isWholeNumberAsWrittenInCountersOfLoops(index, other) || (typesOfSteps && hasNoFractionByLookInCountersOfLoops(walker, index, *typesOfSteps, other)))
                        return true;
                    ValueID written = stepsAskedInFront ? index.valueBehindCasts(other) : ValueID();
                    if (!written || written.index() >= firstValueOfLoop || !isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, walker.referenceOf(written)))
                        return false;
                    if (!stepsAskedInFront->contains(written))
                        stepsAskedInFront->append(written);
                    return true;
                };
                steps = (names.contains(operation.operand(0)) && isStep(operation.operand(1))) || (opcode == Opcode::Add && names.contains(operation.operand(1)) && isStep(operation.operand(0)));
            }
            if (steps) {
                add(operation.id());
                isStepped = true;
            }
        }
    }
    if (!isStepped)
        return false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        if (position >= arguments.size() || !names.contains(arguments[position]))
            return false;
    }
    return true;
}

static bool turnsTakeCounterAsIntegerInCountersOfLoops(const BodyIndex& index, LoopID loop, const NamesOfCounterOfLoop& names)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
    static constexpr OpcodeSet calls { Opcode::Call, Opcode::CallDirect, Opcode::Construct };
    auto isName = [&](ValueID operand) {
        return names.contains(operand);
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            Opcode opcode = operation.opcode();
            if (accessesByKey.contains(opcode) && isName(operation.propertyOperand()))
                return true;
            if (calls.contains(opcode) && std::ranges::any_of(operation.argumentsOperands(), isName))
                return true;
            if (operatorsOnBitsOfCountersOfLoops.contains(opcode) && (isName(operation.operand(0)) || (opcode != Opcode::BitNot && isName(operation.operand(1)))))
                return true;
        }
    }
    return false;
}

enum class WhatComesRoundInCountersOfLoops : uint8_t { NotAsked, IsBeingAsked, Int32, Other };

static bool comesRoundAsInt32InCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const LoopPreview& preview, uint32_t position, Vector<WhatComesRoundInCountersOfLoops, 8>& answers)
{
    if (answers[position] != WhatComesRoundInCountersOfLoops::NotAsked)
        return answers[position] == WhatComesRoundInCountersOfLoops::Int32;
    answers[position] = WhatComesRoundInCountersOfLoops::IsBeingAsked;
    TypeArena& types = walker.types();
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstParameter = index.block(inputHead).firstParameter().index();
    auto everyWayRoundBringsInt32 = [&] {
        bool comesRound = false;
        for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
            if (!index.isInLoop(edge.from, loop))
                continue;
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID brought = position < arguments.size() ? index.valueBehindCasts(arguments[position]) : ValueID();
            if (!brought)
                return false;
            if (index.isParameter(brought) && index.blockOf(brought) == inputHead) {
                if (!comesRoundAsInt32InCountersOfLoops(walker, index, inputHead, preview, brought.index() - firstParameter, answers))
                    return false;
            } else {
                size_t place = brought.index() - preview.firstValue;
                TypeID found = brought.index() >= preview.firstValue && place < preview.typesOfValues.size() ? preview.typesOfValues[place] : TypeID();
                TypeID number = found ? types.withoutPrimitives(found, Primitive::BigInt) : TypeID();
                if (!number || isNeverType(number) || !types.at(number).isInt32())
                    return false;
            }
            comesRound = true;
        }
        return comesRound;
    };
    bool comesRound = everyWayRoundBringsInt32();
    answers[position] = comesRound ? WhatComesRoundInCountersOfLoops::Int32 : WhatComesRoundInCountersOfLoops::Other;
    return comesRound;
}

static void guessBoundsInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const NamesOfCounterOfLoop& names, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses)
{
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!allRelations.contains(operation.opcode()))
                continue;
            for (uint32_t place : { 0u, 1u }) {
                if (!names.contains(operation.operand(place)))
                    continue;
                ValueID bound = index.valueBehindCasts(operation.operand(1 - place));
                if (!bound || bound.index() >= firstValueOfLoop)
                    continue;
                Reference value = walker.referenceOf(bound);
                if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end())
                    continue;
                size_t listed = guesses.findIf([&](const GuessInFrontOfLoop& guess) {
                    return guess.value == value;
                });
                if (listed == notFound)
                    guesses.append(GuessInFrontOfLoop { value, TypeTable::int32, { }, namesInFrontThatLoopMentions(walker, index, loop, firstValueOfLoop, value) });
                else if (walker.types().at(guesses[listed].tested).isNumber())
                    guesses[listed].tested = TypeTable::int32;
            }
        }
    }
}

static bool isKeyOfElementInCountersOfLoops(const BodyIndex& index, ValueID sum)
{
    ValueID name = sum;
    for (ValueID reader = index.onlyUsingValue(name); reader; reader = index.onlyUsingValue(name)) {
        Opcode opcode = index.opcodeOf(reader);
        if (accessesUnderKeysInCountersOfLoops.contains(opcode))
            return index.value(reader).propertyOperand() == name;
        if (!opcodesThatStateTypes.contains(opcode))
            return false;
        name = reader;
    }
    return false;
}

static void guessAddendsOfKeysInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, const NamesOfCounterOfLoop& names, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses)
{
    LoopID loop = index.loopOf(inputHead);
    uint32_t firstValueOfLoop = index.block(inputHead).firstParameter().index();
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!stepsByNumberOfCountersOfLoops.contains(operation.opcode()) || !isKeyOfElementInCountersOfLoops(index, operation.id()))
                continue;
            for (uint32_t place : { 0u, 1u }) {
                if (!names.contains(operation.operand(place)))
                    continue;
                ValueID addend = index.valueBehindCasts(operation.operand(1 - place));
                if (!addend || addend.index() >= firstValueOfLoop)
                    continue;
                Reference value = walker.referenceOf(addend);
                if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end())
                    continue;
                if (guesses.containsIf([&](const GuessInFrontOfLoop& guess) { return guess.value == value; }))
                    continue;
                guesses.append(GuessInFrontOfLoop { value, TypeTable::int32, { }, namesInFrontThatLoopMentions(walker, index, loop, firstValueOfLoop, value) });
                walker.count(StatisticID::NumbersBesideCountersAskedInFrontOfLoops);
            }
        }
    }
}

static ValueID parameterOfHeadNamedInCountersOfLoops(const BodyIndex& index, ValueID value, BlockID inputHead)
{
    for (size_t steps = 0; value && steps < mostNamesOfCounterOfLoop; ++steps) {
        value = index.valueBehindCasts(value);
        if (!index.isParameter(value))
            return { };
        if (index.blockOf(value) == inputHead)
            return value;
        value = index.valueNarrowedInto(value);
    }
    return { };
}

CounterHeldBelowRoomOfViews findCounterHeldBelowRoomOfViews(Walker& walker, BlockID inputHead, NOESCAPE const ScopedLambda<TypeID(uint32_t position)>& typeThatEnters, NOESCAPE const ScopedLambda<TypeID(ValueID)>& typeInFront)
{
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    CounterHeldBelowRoomOfViews found;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    uint32_t firstValueOfLoop = head.firstParameter().index();
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || !head.numberOfParameters() || !index.holdsAnyOf(accessesUnderKeysInCountersOfLoops))
        return found;
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || asked.index() + 1 != head.endOfValues().index() || !relations.contains(index.opcodeOf(asked)))
        return found;
    BodyTargets::Iterator target = end.targets().begin();
    if (!index.isInLoop((*target).block(), loop) || index.isInLoop((*++target).block(), loop))
        return found;
    auto rangeOfIntegerFromInFront = [&](ValueID written) -> std::optional<Range> {
        ValueID value = index.valueBehindCasts(written);
        if (value.index() >= firstValueOfLoop) {
            if (index.isParameter(value) || index.opcodeOf(value) != Opcode::Integer)
                return std::nullopt;
            int64_t integer = index.value(value).as<Opcode::Integer>().integer();
            return Range { integer, integer };
        }
        TypeID type = typeInFront(written.index() < firstValueOfLoop ? written : value);
        if (!type || !walker.types().at(type).isInteger())
            return std::nullopt;
        return walker.types().at(type).integers;
    };
    BodyValue relation = index.value(asked);
    ValueID counter = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(0), inputHead);
    if (ValueID read = index.valueBehindCasts(relation.operand(1)); read.index() >= firstValueOfLoop && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName() && index.value(read).key() == walker.program().keyOf(BuiltinNames::length)) {
        ValueID view = index.value(read).objectOperand();
        if (view.index() >= firstValueOfLoop)
            view = index.valueBehindCasts(view);
        if (view.index() < firstValueOfLoop && isViewOfNumbersReadInLine(walker, typeInFront(view)))
            found.viewWhoseLengthIsBound = view;
    }
    if (!counter || (!found.viewWhoseLengthIsBound && !rangeOfIntegerFromInFront(relation.operand(1))))
        return found;
    found.position = counter.index() - firstValueOfLoop;
    found.relation = asked;
    found.bound = relation.operand(1);
    TypeID enters = typeThatEnters(found.position);
    if (!enters || !walker.types().at(enters).isInteger())
        return found;
    auto namesCounter = [&](ValueID value) {
        return parameterOfHeadNamedInCountersOfLoops(index, value, inputHead) == counter;
    };
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID next = found.position < arguments.size() ? index.valueBehindCasts(arguments[found.position]) : ValueID();
        if (!next || index.isParameter(next))
            return found;
        BodyValue step = index.value(next);
        if (step.opcode() == Opcode::Increment && namesCounter(step.operand(0)))
            continue;
        if (step.opcode() != Opcode::Add)
            return found;
        ValueID by = namesCounter(step.operand(0)) ? step.operand(1) : namesCounter(step.operand(1)) ? step.operand(0) : ValueID();
        std::optional<Range> range = by ? rangeOfIntegerFromInFront(by) : std::nullopt;
        if (!range)
            return found;
        if (range->minimum >= 0)
            continue;
        ValueID written = index.valueBehindCasts(by);
        if (found.stepThatMayBeNegative && index.valueBehindCasts(found.stepThatMayBeNegative) != written)
            return found;
        found.stepThatMayBeNegative = by;
    }
    for (BlockID member : index.membersOf(loop)) {
        if (member == inputHead)
            continue;
        for (BodyValue access : index.block(member).values()) {
            if (!accessesUnderKeysInCountersOfLoops.contains(access.opcode()) || index.valueBehindCasts(access.objectOperand()).index() >= firstValueOfLoop)
                continue;
            ValueID key = index.valueBehindCasts(access.propertyOperand());
            ValueID addend;
            if (!namesCounter(key)) {
                if (index.isParameter(key) || index.opcodeOf(key) != Opcode::Add)
                    continue;
                BodyValue sum = index.value(key);
                addend = namesCounter(sum.operand(0)) ? sum.operand(1) : namesCounter(sum.operand(1)) ? sum.operand(0) : ValueID();
                if (!addend || !rangeOfIntegerFromInFront(addend))
                    continue;
            }
            ValueID view = access.objectOperand().index() < firstValueOfLoop ? access.objectOperand() : index.valueBehindCasts(access.objectOperand());
            if (!isViewOfNumbersReadInLine(walker, typeInFront(view)) || testOfBodyHoldsPositionBelowLengthOfText(walker, index, member, access.objectOperand(), access.propertyOperand()))
                continue;
            found.accesses.append({ access.id(), view, addend });
        }
    }
    return found;
}

Vector<CounterTestedInFrontOfLoop, 2> findCountersTestedInFrontOfLoop(Walker& walker, BlockID inputHead, const LoopPreview& preview, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses)
{
    Vector<CounterTestedInFrontOfLoop, 2> counters;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return counters;
    const BodyIndex& index = walker.currentBodyIndex();
    bool holdsOperatorsOnBits = index.holdsAnyOf(operatorsOnBitsOfCountersOfLoops);
    if (!holdsOperatorsOnBits && !index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops))
        return counters;
    bool holdsAccessesUnderKeys = index.holdsAnyOf(accessesUnderKeysInCountersOfLoops) && index.holdsAnyOf(stepsByNumberOfCountersOfLoops);
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    Vector<WhatComesRoundInCountersOfLoops, 8> whatComesRound;
    whatComesRound.fill(WhatComesRoundInCountersOfLoops::NotAsked, numberOfParameters);
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        Reference enters = entering[position];
        if (enters && walker.types().at(walker.typeOf(enters)).isInt32() && (!guesses.isEmpty() || holdsAccessesUnderKeys)) {
            NamesOfCounterOfLoop names;
            if (isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names)) {
                if (holdsAccessesUnderKeys)
                    guessAddendsOfKeysInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
                if (!guesses.isEmpty())
                    guessBoundsInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
            }
            continue;
        }
        if (!mayBeInt32InCountersOfLoops(walker, enters))
            continue;
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end() || guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
            return guess.value == enters;
        });
        if (isTestedAlready)
            continue;
        if (holdsOperatorsOnBits && comesRoundAsInt32InCountersOfLoops(walker, index, inputHead, preview, position, whatComesRound)) {
            counters.append({ position, TypeTable::int32 });
            continue;
        }
        NamesOfCounterOfLoop names;
        if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, enters))
            continue;
        bool isHeldWhereItComesRound = !isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names);
        Vector<ValueID, 2> stepsAskedInFront;
        if (isHeldWhereItComesRound) {
            names.shrink(0);
            if (!isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names, &preview)) {
                names.shrink(0);
                if (!holdsAccessesUnderKeys || !isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names, &preview, &stepsAskedInFront))
                    continue;
            }
        }
        if ((isHeldWhereItComesRound || walker.types().at(walker.typeOf(enters)).isNumber()) && !turnsTakeCounterAsIntegerInCountersOfLoops(index, index.loopOf(inputHead), names))
            continue;
        bool stepEnters = std::ranges::any_of(stepsAskedInFront, [&](ValueID step) {
            return std::ranges::find(entering, walker.referenceOf(step)) != entering.end();
        });
        if (stepEnters)
            continue;
        counters.append({ position, TypeTable::int32, isHeldWhereItComesRound });
        for (ValueID step : stepsAskedInFront) {
            Reference value = walker.referenceOf(step);
            if (guesses.containsIf([&](const GuessInFrontOfLoop& guess) { return guess.value == value; }))
                continue;
            guesses.append(GuessInFrontOfLoop { value, TypeTable::int32, { }, namesInFrontThatLoopMentions(walker, index, index.loopOf(inputHead), index.block(inputHead).firstParameter().index(), value) });
            walker.count(StatisticID::NumbersBesideCountersAskedInFrontOfLoops);
        }
        guessBoundsInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
        if (holdsAccessesUnderKeys)
            guessAddendsOfKeysInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
    }
    return counters;
}

RelationOfHeadOnNumber findRelationOfHeadBetweenCounterThatEntersAsInt32AndNumber(Walker& walker, BlockID inputHead, std::span<const Reference> entering, std::span<const CounterTestedInFrontOfLoop> askedInFront)
{
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq };
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || !walker.has(Mechanism::LoopInvariants))
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops))
        return { };
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || !head.numberOfParameters())
        return { };
    uint32_t firstValueOfLoop = head.firstParameter().index();
    ValueID asked = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!asked || index.isParameter(asked) || !head.holds(asked) || asked.index() + 1 != head.endOfValues().index() || !relations.contains(index.opcodeOf(asked)))
        return { };
    BodyTargets::Iterator target = end.targets().begin();
    if (!index.isInLoop((*target).block(), loop) || index.isInLoop((*++target).block(), loop))
        return { };
    BodyValue relation = index.value(asked);
    ValueID counter = parameterOfHeadNamedInCountersOfLoops(index, relation.operand(0), inputHead);
    ValueID number = index.valueBehindCasts(relation.operand(1));
    if (!counter || !number || number.index() >= firstValueOfLoop)
        return { };
    uint32_t position = counter.index() - firstValueOfLoop;
    Reference enters = position < entering.size() ? entering[position] : Reference();
    bool isAskedForInt32 = std::ranges::any_of(askedInFront, [&](const CounterTestedInFrontOfLoop& counter) {
        return counter.position == position && counter.tested == TypeTable::int32 && !counter.isHeldWhereItComesRound;
    });
    if (!enters || !enters.isRecord() || (!isAskedForInt32 && !walker.types().at(walker.typeOf(enters)).isInt32()))
        return { };
    Reference value = walker.referenceOf(number);
    if (!isBoxOrDoubleThatMayBeInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end())
        return { };
    NamesOfCounterOfLoop names;
    if (!isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names) || !turnsTakeCounterAsIntegerInCountersOfLoops(index, loop, names))
        return { };
    return { asked, number };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
