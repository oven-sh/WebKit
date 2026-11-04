/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Guesses at a call of a known built-in: ONE operand keeps the call from going by one row, so that operand is tested, and the call as written stands where the test fails.
// What is tested for comes from the rows: it is no proof of anything, and behind the rejoin nothing is left of it. The arms are openGuess()'s, which knows what a suspension and a throw mean to a stretch.
// Where the row takes the operand only as its class makes it they are those of AOTWalkStringOperandsAsMade.cpp, which looks at the cell too.
// And what the rows, the library's own text and the kind of the callee let one expect of the result of a call: an expectation, which a test of a guess asks at each use.
// Withheld (Guesses): the call enters the native code on any operands, and nothing is expected of a result of which nothing is written or held.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkStringParsers.h"
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// Closed, the call is 25 to 40 instructions in CallBoxedBuiltin against 8 to 12 by its row.
static constexpr uint32_t weightOfCallByOneRowForStrings = 2;

// ---- What an operand is tested for

static bool convertsToIntegerForStringOperands(BuiltinConversion conversion)
{
    return conversion == BuiltinConversion::ToIntegerOrInfinity || conversion == BuiltinConversion::ToInt32 || conversion == BuiltinConversion::ToUint32;
}

// An int32 where the rows convert to an integer, else the type that every row takes unconverted. Null: the rows do not agree, or none has code in line.
static TypeID typeThatEveryRowTakesForStringOperands(Walker& walker, BuiltinID builtin, BuiltinOperand position)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinTypeContext context { walker.program(), walker.types(), &walker.facts() };
    TypeID taken;
    bool someRowHasCodeInLine = false;
    for (const BuiltinOverload& overload : table.overloadsOf(builtin)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const BuiltinParameter* parameter = overload.isUndeclared() ? nullptr : table.parameterFor(overload, position);
        if (!parameter || !parameter->accepted)
            return { };
        TypeID takenByRow = convertsToIntegerForStringOperands(parameter->conversion) ? TypeID(TypeTable::int32) : typeOfBuiltinType(context, parameter->accepted);
        if (taken && taken != takenByRow)
            return { };
        taken = takenByRow;
        someRowHasCodeInLine |= overload.code.implementation.inlineBuiltin.has_value();
    }
    return someRowHasCodeInLine ? taken : TypeID();
}

// `text.replace(pattern, "")` with `pattern: RegExp | null`: the rows refuse undefined and null there, and one takes what is left. Null: absence is an answer that some row takes.
// What is left of what is proven, cut by what is expected: of a parameter that the guard passes by nothing is proven, and what is written of it says what it is where it is there. `expected` may be null.
static TypeID presentTypeThatRowsTakeForStringOperands(Walker& walker, BuiltinID builtin, BuiltinOperand position, TypeID proven, TypeID expected)
{
    TypeArena& types = walker.types();
    if (!types.at(proven).mayBeUndefinedOrNull())
        return { };
    TypeID present = types.withoutNullish(expected ? types.intersectionOf(proven, expected) : proven);
    if (isNeverType(present))
        return { };

    const BuiltinTable& table = walker.program().builtins();
    BuiltinTypeContext context { walker.program(), types, &walker.facts() };
    bool everyRowRefusesAbsence = true;
    bool someRowTakesPresence = false;
    for (const BuiltinOverload& overload : table.overloadsOf(builtin)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const BuiltinParameter* parameter = table.parameterFor(overload, position);
        bool refusesAbsence = parameter && parameter->accepted && parameter->mode == BuiltinParameterMode::Required && parameter->conversion == BuiltinConversion::None
            && fitOf(context, TypeTable::undefined, parameter->accepted).fit == TypeFit::Outside && fitOf(context, TypeTable::null, parameter->accepted).fit == TypeFit::Outside;
        everyRowRefusesAbsence &= refusesAbsence;
        someRowTakesPresence |= refusesAbsence && fitOf(context, present, parameter->accepted).fit == TypeFit::Inside;
    }
    return someRowTakesPresence && (everyRowRefusesAbsence || types.at(present).isObject()) ? present : TypeID();
}

// ---- The guess

static bool openGuessAtOperandForStringOperands(Walker& walker, const BodyValue& call, BuiltinID builtin, ValueID operand, BuiltinOperand position)
{
    TypeArena& types = walker.types();
    Reference value = walker.referenceOf(operand);
    if (!value.isValue())
        return false;
    TypeID proven = walker.typeOf(value);
    ExpectationOfValue expectation = findExpectation(walker, operand);
    TypeID tested = typeThatEveryRowTakesForStringOperands(walker, builtin, position);
    if (!tested || types.isSubset(proven, tested) || !types.overlap(proven, tested))
        tested = presentTypeThatRowsTakeForStringOperands(walker, builtin, position, proven, expectation.type);
    if (!tested || types.isSubset(proven, tested) || !types.overlap(proven, tested) || types.testablePartOf(tested) != tested)
        return false;

    if (expectation.type && !types.overlap(expectation.type, tested))
        return false;
    if (!shouldOpenGuess(walker, value, tested, expectation.isDeclared, weightOfCallByOneRowForStrings))
        return false;

    TypeID behindTest = types.intersectionOf(proven, tested);
    auto typeOfInput = [&](ValueID input) {
        Reference reference = walker.referenceOf(input);
        if (reference == value)
            return behindTest;
        return reference ? walker.typeOf(reference) : TypeID();
    };
    // Asked ahead, nothing is seen of an object: a row that takes it only as its class makes it is not reached by its type.
    bool typeAloneDecides = wouldCallBuiltin(walker, call, TypesOfInputs(typeOfInput)) == BuiltinCallAskedAbout::ByOneRow;
    if (!typeAloneDecides && !goesByOneRowOnOperandAsMade(walker, call, builtin, value, behindTest))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(call.id());
    uint32_t place = call.id().index() - index.block(inputBlock).firstValue().index();

    // Both arms come by here again. Where no test can be chosen the call is walked once more as it stands.
    NotesOfStrings& notes = notesOfStrings(walker);
    ValueID callBefore = std::exchange(notes.callWhoseOperandIsTested, call.id());
    size_t depthBefore = std::exchange(notes.depthOfCopiesAroundThatCall, walker.depthOfCopies());
    if (typeAloneDecides)
        openGuess(walker, value, tested, inputBlock, place, place + 1);
    else
        openGuessAtOperandTakenAsMade(walker, call, value, tested);
    notes.callWhoseOperandIsTested = callBefore;
    notes.depthOfCopiesAroundThatCall = depthBefore;
    return true;
}

bool openGuessAtOperandOfBuiltinCall(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || !walker.opensGuesses() || !walker.has(Mechanism::Guesses) || walker.isUnreachable())
        return false;
    const NotesOfStrings& notes = notesOfStrings(walker);
    if (notes.callWhoseOperandIsTested == call.id() && notes.depthOfCopiesAroundThatCall == walker.depthOfCopies())
        return false;

    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return false;
    Type typeOfCallee = types.at(walker.typeOf(callee));
    BuiltinID builtin = typeOfCallee.isObject() ? singleBuiltinOf(types, typeOfCallee) : BuiltinID();
    if (!builtin)
        return false;
    builtin = walker.program().builtins().canonicalID(builtin);

    if (ValueID receiver = call.thisValueOperand(); receiver && openGuessAtOperandForStringOperands(walker, call, builtin, receiver, BuiltinOperand::receiver()))
        return true;
    unsigned position = 0;
    for (ValueID argument : call.argumentsOperands()) {
        if (openGuessAtOperandForStringOperands(walker, call, builtin, argument, BuiltinOperand::argument(position++)))
            return true;
    }
    return false;
}

// ---- What is expected of a result

// Whoever asks what is expected goes from a value to older ones alone, so the questions end.
static TypeID typeProvenOrExpectedForStringOperands(Walker& walker, ValueID input, ValueID call)
{
    Reference reference = walker.referenceOf(input);
    TypeID proven = reference ? walker.typeOf(reference) : TypeID();
    TypeID expected = input < call ? findExpectation(walker, input).type : TypeID();
    if (!expected)
        return proven;
    return !proven || walker.types().isSubset(expected, proven) ? expected : proven;
}

// The built-in that the call is proven or expected to enter, and what is expected of its receiver.
static BuiltinID builtinExpectedToBeCalledForStringOperands(Walker& walker, const BodyValue& call)
{
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    if (Reference callee = walker.referenceOf(call.calleeOperand())) {
        Type type = types.at(walker.typeOf(callee));
        if (BuiltinID builtin = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID())
            return builtin;
    }
    ValueID read = index.valueBehindCasts(call.calleeOperand());
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    TypeID object = typeProvenOrExpectedForStringOperands(walker, index.value(read).objectOperand(), call.id());
    return object ? findConstantMemberReadOff(static_cast<const Walker&>(walker), object, index.value(read).key()) : BuiltinID();
}

static BuiltinClassID classHandedOutByRampForStringOperands(const Function& function)
{
    if (!function.has(FunctionFlags::IsGenerator))
        return BuiltinClasses::Promise;
    return function.has(FunctionFlags::IsAsync) ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
}

TypeID typeExpectedOfResultOfCall(Walker& walker, const BodyValue& call)
{
    Opcode opcode = call.opcode();
    if (opcode != Opcode::Call && opcode != Opcode::Construct)
        return { };
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const LinkTables& held = walker.facts().held();

    // What `return` hands over in a coroutine is not what its caller gets: he gets what the ramp makes.
    if (FunctionID callee = held.calleeAt(walker.currentFunction(), call.id())) {
        if (opcode == Opcode::Call && program.at(callee).isCoroutine())
            return types.builtinClass(classHandedOutByRampForStringOperands(program.at(callee)));
        return { };
    }

    BuiltinID builtin = builtinExpectedToBeCalledForStringOperands(walker, call);
    if (!builtin)
        return { };
    const BuiltinTable& table = program.builtins();
    builtin = table.canonicalID(builtin);
    BuiltinCallMode mode = opcode == Opcode::Construct ? BuiltinCallMode::Construct : BuiltinCallMode::Call;

    BuiltinOperandTypes operands;
    if (opcode == Opcode::Call && call.thisValueOperand())
        operands.receiver = typeProvenOrExpectedForStringOperands(walker, call.thisValueOperand(), call.id());
    for (ValueID argument : call.argumentsOperands())
        operands.arguments.append(typeProvenOrExpectedForStringOperands(walker, argument, call.id()));
    for (TypeID argument : operands.arguments) {
        if (!argument)
            return { };
    }

    // `x.slice(1).length`, x only expected to be a string: the rows, applied to what is expected of the operands.
    BuiltinTypeContext context { program, types, &walker.facts(), nullptr, TypeLevel::Expected };
    TypeID byRows = resultTypeOfReachableOverloads(context, builtin, mode, operands);
    if (byRows && byRows != TypeID(TypeTable::anything) && !isNeverType(byRows))
        return byRows;

    // `Buffer.from(s).toJSON()`: what Link holds of the result of the library's own text.
    TypeID byLibrary;
    for (BuiltinOverloadID id : overloadsReachedBy(context, builtin, mode, operands)) {
        const RuntimeFunctionEntry* entry = table.at(id).libraryFunction() ? findRuntimeFunction(program, table.at(id).libraryFunction()) : nullptr;
        FunctionID function = entry ? entry->function : FunctionID();
        if (!function || function.index() >= held.functions.size() || program.at(function).isCoroutine() || !held.function(function).result)
            return { };
        byLibrary = byLibrary ? types.unionOf(byLibrary, held.function(function).result) : held.function(function).result;
    }
    return byLibrary;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
