/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCountersOfLoops.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet stepsByOneOfCountersOfLoops { Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet stepsByNumberOfCountersOfLoops { Opcode::Add, Opcode::Sub };
// The names that a counter has in the turns: the parameter, what states a type of it, what a test narrows it into, what a step makes of it, what joins those.
static constexpr size_t mostNamesOfCounterOfLoop = 16;

using NamesOfCounterOfLoop = Vector<ValueID, mostNamesOfCounterOfLoop>;

// A box of which a test for an int32 says something: it may hold one, and it may hold what is no number.
static bool isBoxThatMayHoldInt32InCountersOfLoops(const Walker& walker, Reference value)
{
    if (!value || !value.isValue() || walker.representationOf(value) != Representation::JSValue)
        return false;
    Type type = walker.types().at(walker.typeOf(value));
    return type.isJavaScriptValue() && type.primitives & Primitive::Integer && !type.isNumber();
}

static bool isWholeNumberAsWrittenInCountersOfLoops(const BodyIndex& index, ValueID value)
{
    ValueID written = index.valueBehindCasts(value);
    return written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer;
}

// One pass over the blocks of the loop in their order: a definition stands in front of its uses. What a loop inside brings round to its own head is no name.
// False: some way round brings the head something else, or no turn steps the parameter.
static bool isSteppedAllRoundInCountersOfLoops(Walker& walker, const BodyIndex& index, BlockID inputHead, uint32_t position, NamesOfCounterOfLoop& names)
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
                steps = (names.contains(operation.operand(0)) && isWholeNumberAsWrittenInCountersOfLoops(index, operation.operand(1)))
                    || (opcode == Opcode::Add && names.contains(operation.operand(1)) && isWholeNumberAsWrittenInCountersOfLoops(index, operation.operand(0)));
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

// GuessInFrontOfLoop::names.
static Vector<ValueID, 2> namesInFrontThatLoopMentionsInCountersOfLoops(const Walker& walker, const BodyIndex& index, LoopID loop, uint32_t firstValueOfLoop, Reference value)
{
    Vector<ValueID, 2> names;
    auto mention = [&](ValueID operand) {
        if (operand && operand.index() < firstValueOfLoop && !names.contains(operand) && walker.referenceOf(operand) == value)
            names.append(operand);
    };
    for (BlockID member : index.membersOf(loop)) {
        BodyBlock block = index.block(member);
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                mention(operand);
            });
        }
        BodyTerminator end = block.terminator();
        end.forEachOperand([&](ValueID operand, uint32_t) {
            mention(operand);
        });
        for (BodyTarget target : end.targets()) {
            for (ValueID argument : target.arguments())
                mention(argument);
        }
        if (block.hasHandler()) {
            for (ValueID argument : block.handler().arguments())
                mention(argument);
        }
    }
    return names;
}

// `index < end`: below an int32 a counter that begins as one stays one, and the step needs no test of what it makes.
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
                // What enters the head as well has its test there, or none.
                if (!isBoxThatMayHoldInt32InCountersOfLoops(walker, value) || std::ranges::find(entering, value) != entering.end())
                    continue;
                bool isListed = guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
                    return guess.value == value;
                });
                if (!isListed)
                    guesses.append(GuessInFrontOfLoop { value, TypeTable::int32, { }, namesInFrontThatLoopMentionsInCountersOfLoops(walker, index, loop, firstValueOfLoop, value) });
            }
        }
    }
}

Vector<CounterTestedInFrontOfLoop, 2> findCountersTestedInFrontOfLoop(Walker& walker, BlockID inputHead, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses)
{
    Vector<CounterTestedInFrontOfLoop, 2> counters;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return counters;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.holdsAnyOf(stepsByOneOfCountersOfLoops | stepsByNumberOfCountersOfLoops))
        return counters;
    uint32_t numberOfParameters = index.block(inputHead).numberOfParameters();
    for (uint32_t position = 0; position < numberOfParameters && position < entering.size(); ++position) {
        Reference enters = entering[position];
        if (!isBoxThatMayHoldInt32InCountersOfLoops(walker, enters))
            continue;
        // One test a value.
        bool isTestedAlready = std::ranges::find(entering.first(position), enters) != entering.first(position).end() || guesses.containsIf([&](const GuessInFrontOfLoop& guess) {
            return guess.value == enters;
        });
        if (isTestedAlready)
            continue;
        NamesOfCounterOfLoop names;
        if (!isSteppedAllRoundInCountersOfLoops(walker, index, inputHead, position, names))
            continue;
        counters.append({ position, TypeTable::int32 });
        guessBoundsInCountersOfLoops(walker, index, inputHead, names, entering, guesses);
    }
    return counters;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
