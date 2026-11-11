/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
#include <algorithm>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t weightOfCallByOneRowForStrings = 2;

static bool convertsToIntegerForStringOperands(BuiltinConversion conversion)
{
    return conversion == BuiltinConversion::ToIntegerOrInfinity || conversion == BuiltinConversion::ToInt32 || conversion == BuiltinConversion::ToUint32;
}

static TypeID typeThatEveryRowTakesForStringOperands(Walker& walker, BuiltinID builtin, BuiltinOperand position)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinTypeContext context { walker.program(), walker.types() };
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

static TypeID presentTypeThatRowsTakeForStringOperands(Walker& walker, BuiltinID builtin, BuiltinOperand position, TypeID proven, TypeID expected)
{
    TypeArena& types = walker.types();
    if (!types.at(proven).mayBeUndefinedOrNull())
        return { };
    TypeID present = types.withoutNullish(expected ? types.intersectionOf(proven, expected) : proven);
    if (isNeverType(present))
        return { };

    const BuiltinTable& table = walker.program().builtins();
    BuiltinTypeContext context { walker.program(), types };
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
    bool typeAloneDecides = wouldCallBuiltin(walker, call, TypesOfInputs(typeOfInput)) == BuiltinCallAskedAbout::ByOneRow;
    if (!typeAloneDecides && !goesByOneRowOnOperandAsMade(walker, call, builtin, value, behindTest))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(call.id());
    uint32_t place = call.id().index() - index.block(inputBlock).firstValue().index();

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

bool openGuessAtOperandOfBuiltinCall(Walker& walker, const BodyValue& call, OperandsTestedAtBuiltinCall tested)
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

    if (ValueID receiver = call.thisValueOperand(); receiver && walker.isInLoop() && tested == OperandsTestedAtBuiltinCall::All) {
        Reference value = walker.referenceOf(receiver);
        TypeID proven = value && value.isValue() ? walker.typeOf(value) : TypeID();
        if (proven && !isNeverType(proven) && types.at(proven).isObject() && singleBuiltinClassOf(types, types.at(proven)) && !isKnownPristine(walker, value) && goesByOneRowOnOperandAsMade(walker, call, builtin, value, proven)) {
            NotesOfStrings& changed = notesOfStrings(walker);
            ValueID callBefore = std::exchange(changed.callWhoseOperandIsTested, call.id());
            size_t depthBefore = std::exchange(changed.depthOfCopiesAroundThatCall, walker.depthOfCopies());
            openGuessAtOperandTakenAsMade(walker, call, value, proven);
            changed.callWhoseOperandIsTested = callBefore;
            changed.depthOfCopiesAroundThatCall = depthBefore;
            return true;
        }
    }
    if (ValueID receiver = call.thisValueOperand(); receiver && tested == OperandsTestedAtBuiltinCall::All && openGuessAtOperandForStringOperands(walker, call, builtin, receiver, BuiltinOperand::receiver()))
        return true;
    unsigned position = 0;
    for (ValueID argument : call.argumentsOperands()) {
        if (openGuessAtOperandForStringOperands(walker, call, builtin, argument, BuiltinOperand::argument(position++)))
            return true;
    }
    return false;
}

bool openGuessAtOperandThatSpecialCaseTakesAsMade(Walker& walker, const BodyValue& call, FunctionID enteredByValue)
{
    if (call.opcode() != Opcode::Call || !walker.opensGuesses() || !walker.has(Mechanism::Guesses) || walker.isUnreachable())
        return false;
    NotesOfStrings& notes = notesOfStrings(walker);
    if (notes.callWhoseOperandIsTested == call.id() && notes.depthOfCopiesAroundThatCall == walker.depthOfCopies())
        return false;

    TypeArena& types = walker.types();
    const Program& program = walker.program();
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return false;
    Type typeOfCallee = types.at(walker.typeOf(callee));
    BuiltinID builtin = typeOfCallee.isObject() ? singleBuiltinOf(types, typeOfCallee) : BuiltinID();
    if (!builtin)
        return false;
    builtin = program.builtins().canonicalID(builtin);

    bool passesKnownFunction = std::ranges::any_of(call.argumentsOperands(), [&](ValueID argument) {
        Reference passedValue = walker.referenceOf(argument);
        if (!passedValue)
            return false;
        Type type = types.at(walker.typeOf(passedValue));
        return type.isObject() && type.isJavaScriptValue() && singleFunctionOf(types, type);
    });
    if (!passesKnownFunction)
        return false;

    for (ValueID argument : call.argumentsOperands()) {
        Reference value = walker.referenceOf(argument);
        if (!value || !value.isValue() || isKnownPristine(walker, value))
            continue;
        TypeID proven = walker.typeOf(value);
        if (isNeverType(proven))
            continue;
        TypeID tested = proven;
        bool isDeclared = true;
        if (!types.at(proven).isObject() || !singleBuiltinClassOf(types, types.at(proven))) {
            ExpectationOfValue expectation = findExpectation(walker, argument);
            TypeID present = expectation.type ? types.withoutNullish(expectation.type) : TypeID();
            tested = present && !isNeverType(present) ? types.testablePartOf(present) : TypeID();
            isDeclared = expectation.isDeclared;
            if (!tested || isNeverType(tested) || !types.overlap(proven, tested))
                continue;
        }
        BuiltinOverloadID onlyRow;
        if (!goesByOneRowOnOperandAsMade(walker, call, builtin, value, types.intersectionOf(proven, tested), &onlyRow))
            continue;
        const BuiltinOverload& row = program.builtins().at(onlyRow);
        const RuntimeFunctionEntry* held = row.flags.contains(BuiltinOverloadFlag::IsSpecialCase) ? findRuntimeFunction(program, row.libraryFunction()) : nullptr;
        if (!held || !held->function || held->function == enteredByValue || !shouldOpenGuess(walker, value, tested, isDeclared, weightOfCallByOneRowForStrings))
            continue;
        ValueID callBefore = std::exchange(notes.callWhoseOperandIsTested, call.id());
        size_t depthBefore = std::exchange(notes.depthOfCopiesAroundThatCall, walker.depthOfCopies());
        openGuessAtOperandTakenAsMade(walker, call, value, tested);
        notes.callWhoseOperandIsTested = callBefore;
        notes.depthOfCopiesAroundThatCall = depthBefore;
        return true;
    }
    return false;
}

static TypeID typeProvenOrExpectedForStringOperands(Walker& walker, ValueID input, ValueID call)
{
    Reference reference = walker.referenceOf(input);
    TypeID proven = reference ? walker.typeOf(reference) : TypeID();
    TypeID expected = input < call ? findExpectation(walker, input).type : TypeID();
    if (!expected)
        return proven;
    return !proven || walker.types().isSubset(expected, proven) ? expected : proven;
}

static BuiltinID builtinExpectedToBeCalledForStringOperands(Walker& walker, const BodyValue& call)
{
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    if (Reference callee = walker.referenceOf(call.calleeOperand())) {
        Type type = types.at(walker.typeOf(callee));
        if (BuiltinID builtin = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID())
            return builtin;
    }
    ValueID read = index.stripCasts(call.calleeOperand());
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    TypeID object = typeProvenOrExpectedForStringOperands(walker, index.value(read).objectOperand(), call.id());
    if (!object)
        return { };
    if (BuiltinID member = findConstantMemberReadOff(static_cast<const Walker&>(walker), object, index.value(read).key()))
        return member;
    Type type = types.at(object);
    return type.isObject() && singleFunctionOf(types, type) ? findConstantMemberOnceForBuild(walker, Builtins::FunctionPrototype, index.value(read).key()) : BuiltinID();
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

    BuiltinTypeContext context { program, types, nullptr, TypeLevel::Expected };
    TypeID byRows = resultTypeOfReachableOverloads(context, builtin, mode, operands);
    if (byRows && byRows != TypeID(TypeTable::anything) && !isNeverType(byRows))
        return byRows;

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
