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

static bool isSteppedAllRoundInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position, NamesOfCounterOfLoop& names, const LoopPreview* typesOfSteps = nullptr)
{
    LoopID loop = index.loopOf(inputHead);
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
                    return isWholeNumberAsWrittenInCountersOfLoops(index, other) || (typesOfSteps && hasNoFractionByLookInCountersOfLoops(walker, index, *typesOfSteps, other));
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
        if (isHeldWhereItComesRound) {
            names.shrink(0);
            if (!isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names, &preview))
                continue;
        }
        if ((isHeldWhereItComesRound || walker.types().at(walker.typeOf(enters)).isNumber()) && !turnsTakeCounterAsIntegerInCountersOfLoops(index, index.loopOf(inputHead), names))
            continue;
        counters.append({ position, TypeTable::int32, isHeldWhereItComesRound });
        guessBoundsInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
        if (holdsAccessesUnderKeys)
            guessAddendsOfKeysInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
    }
    return counters;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
