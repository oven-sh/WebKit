/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `return Reflect.apply(f, t, list)`, `return f.apply(t, list)`, `return f.apply(this, arguments)`: the call of the operand takes the place of the built-in, so that it leaves by a tail call, and the depth of a program may rest on that.
// At a suspension: nothing. What is built are calls like any other.
// At a throw: each arm throws where the call as written threw. Where a test fails the call is made as it is written.
// Withheld: the tests of the operands with Guesses or BuiltinCalls, the arms by the count of arguments with EscapeAnalysis. The call is then forwarded as AOTWalkCallsDoneInPlace.cpp forwards it, or stays one of the built-in.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTSweep.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

// The call of the built-in and the call that its text makes become one call.
static constexpr uint32_t closedOperationsOfForwardedTailCall = 2;
// As many arms as a wrapper has named parameters, and one.
static constexpr uint32_t mostArgumentsPassedOnByCount = 4;

namespace {

struct OperandsOfForwardedTailCall {
    BuiltinOperand callee;
    // Its own values are the arguments.
    std::optional<BuiltinOperand> list;
    bool constructs { false };
};

struct TestOfForwardedTailCall {
    ValueID input;
    Reference value;
    TypeID tested;
};

} // anonymous namespace

// ---- What the row says. As in AOTWalkCallsDoneInPlace.cpp.

static bool isSingleOperandOfForwardedTailCall(BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return true;
    case BuiltinOperand::Kind::ArgumentsFrom:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<BuiltinOperand> findSingleOperandOfForwardedTailCall(const BuiltinPlace& place, BuiltinPlaceKind kind)
{
    if (place.kind != kind || !place.operand || !isSingleOperandOfForwardedTailCall(*place.operand))
        return std::nullopt;
    return place.operand;
}

// The built-in does nothing but call one operand once, or construct it, and return what that returns.
static const BuiltinCallback* findCallbackOfForwardedTailCall(const BuiltinOverload& overload)
{
    const BuiltinFlow& flow = overload.flow;
    if (overload.isUndeclared() || overload.mode != BuiltinCallMode::Call)
        return nullptr;
    if (flow.callbacks.size() != 1 || !flow.transfers.empty() || !flow.propertyActions.empty() || !flow.handlerCalls.empty())
        return nullptr;
    if (!overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode) || !overload.effects.writes.isEmpty())
        return nullptr;

    const BuiltinCallback& callback = flow.callbacks.front();
    if (callback.times != BuiltinCallbackTimes::Once || callback.resultUse != BuiltinCallbackResultUse::Returned || !isSingleOperandOfForwardedTailCall(callback.callee))
        return nullptr;
    return &callback;
}

static std::optional<OperandsOfForwardedTailCall> findOperandsOfForwardedTailCall(const BuiltinOverload& overload)
{
    const BuiltinCallback* callback = findCallbackOfForwardedTailCall(overload);
    if (!callback)
        return std::nullopt;
    OperandsOfForwardedTailCall operands { callback->callee, { }, callback->constructs };

    if (callback->thisValue.kind != BuiltinPlaceKind::None && !findSingleOperandOfForwardedTailCall(callback->thisValue, BuiltinPlaceKind::Operand))
        return std::nullopt;
    std::optional<BuiltinOperand> newTarget;
    if (callback->newTarget.kind != BuiltinPlaceKind::None) {
        newTarget = findSingleOperandOfForwardedTailCall(callback->newTarget, BuiltinPlaceKind::Operand);
        if (!newTarget)
            return std::nullopt;
    }

    const BuiltinPlace& spread = callback->spreadArguments;
    if (spread.kind != BuiltinPlaceKind::None) {
        operands.list = findSingleOperandOfForwardedTailCall(spread, BuiltinPlaceKind::OwnValues);
        if (!operands.list || !callback->arguments.empty())
            return std::nullopt;
    }
    if (!callback->arguments.empty()) {
        const BuiltinPlace& passed = callback->arguments.front();
        if (callback->arguments.size() != 1 || passed.kind != BuiltinPlaceKind::Operand || !passed.operand || isSingleOperandOfForwardedTailCall(*passed.operand))
            return std::nullopt;
    }

    auto runsOtherProtocols = [&](const BuiltinParameter& parameter) {
        return parameter.operand != operands.list && parameter.operand != newTarget && !parameter.protocols.isEmpty();
    };
    if ((overload.receiver && runsOtherProtocols(*overload.receiver)) || std::ranges::any_of(overload.parameters, runsOtherProtocols))
        return std::nullopt;
    return operands;
}

// The built-in refuses an argument that its row does not take, in its own words. A receiver that cannot be called is refused by the call itself.
static bool rowTakesArgumentsOfForwardedTailCall(Walker& walker, std::span<const ValueID> arguments, const BuiltinOverload& overload, const TypesOfInputs& typeBehindTests)
{
    BuiltinTypeContext context { walker.program(), walker.types(), &walker.facts() };
    auto takes = [&](const BuiltinParameter& parameter, ValueID argumentOrNotPassed) {
        OperandFit fit = fitOf(context, argumentOrNotPassed ? typeBehindTests(argumentOrNotPassed) : TypeID(), parameter.accepted);
        return fit.fit == TypeFit::Inside && !fit.unprovenPristine;
    };
    return std::ranges::all_of(overload.parameters, [&](const BuiltinParameter& parameter) {
        if (parameter.operand.isReceiver())
            return true;
        size_t index = parameter.operand.argumentIndex();
        if (isSingleOperandOfForwardedTailCall(parameter.operand))
            return takes(parameter, index < arguments.size() ? arguments[index] : ValueID());
        return std::ranges::all_of(arguments.subspan(std::min(index, arguments.size())), [&](ValueID argument) {
            return takes(parameter, argument);
        });
    });
}

// What is called is one built-in, or is read under a name off the receiver, above every function lies one member under that name for good, and nothing rules it out: forwardCallBehindTestOfCallee() tests for it.
static BuiltinID findBuiltinOfForwardedTailCall(Walker& walker, const BodyValue& call, bool& calleeIsProven)
{
    auto operation = call.as<Opcode::Call>();
    Reference callee = walker.referenceOf(operation.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return { };
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (BuiltinID builtin = types.at(proven).isObject() ? singleBuiltinOf(types, types.at(proven)) : BuiltinID()) {
        calleeIsProven = true;
        return table.canonicalID(builtin);
    }

    calleeIsProven = false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    if (index.valueBehindCasts(index.value(read).objectOperand()) != index.valueBehindCasts(operation.thisValueOperand()))
        return { };
    BuiltinID member = findConstantMemberFrom(program, walker.facts(), table.at(BuiltinClasses::Function).prototype, index.value(read).key());
    if (!member)
        return { };
    BuiltinID builtin = table.canonicalID(member);
    if (!isTestableBuiltin(table, builtin) || !callee.isValue() || !types.overlap(proven, types.forBuiltin(builtin)))
        return { };
    return builtin;
}

// ---- Behind tests of the operands

bool forwardTailCallBehindTestsOfOperandsOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || walker.isUnreachable())
        return false;
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    // The block being walked, without a search for it.
    BodyBlock block = index.block(walker.inputBlockBeingWalked());
    if (!block.holds(call.id()))
        block = index.block(index.blockOf(call.id()));
    BodyTerminator end = block.terminator();
    if (end.kind() != TerminatorKind::ReturnCall || end.operand(0) != call.id())
        return false;

    bool calleeIsProven = false;
    BuiltinID builtin = findBuiltinOfForwardedTailCall(walker, call, calleeIsProven);
    if (!builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinOverloadID row;
    unsigned numberOfRows = 0;
    for (BuiltinOverloadID overload : table.overloadIDsOf(builtin)) {
        if (table.at(overload).mode != BuiltinCallMode::Call)
            continue;
        row = overload;
        ++numberOfRows;
    }
    std::optional<OperandsOfForwardedTailCall> operands = numberOfRows == 1 ? findOperandsOfForwardedTailCall(table.at(row)) : std::nullopt;
    if (!operands || operands->constructs || (!calleeIsProven && !operands->callee.isReceiver()))
        return false;

    TypeArena& types = walker.types();
    auto operation = call.as<Opcode::Call>();
    std::span<const ValueID> arguments = operation.argumentsOperands();
    auto inputAt = [&](BuiltinOperand operand) {
        if (operand.isReceiver())
            return operation.thisValueOperand();
        return operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : ValueID();
    };
    Vector<TestOfForwardedTailCall, 2> tests;
    auto testUnlessDecided = [&](ValueID input, TypeID tested) {
        Reference value = walker.referenceOf(input);
        TypeID proven = value && value.isValue() ? walker.typeOf(value) : TypeID();
        if (proven && !types.isSubset(proven, tested) && types.overlap(proven, tested))
            tests.append(TestOfForwardedTailCall { input, value, tested });
    };

    // A built-in that calls its receiver needs no test: the call of the receiver refuses what the built-in refuses.
    if (ValueID called = operands->callee.isReceiver() ? ValueID() : inputAt(operands->callee))
        testUnlessDecided(called, TypeTable::function);
    // An array goes on as it is. Of its own arguments object the call takes what the caller handed over: passOnOwnArgumentsByCount().
    if (ValueID list = operands->list ? inputAt(*operands->list) : ValueID()) {
        Reference value = walker.referenceOf(list);
        TypeID proven = value ? walker.typeOf(value) : TypeID();
        TypeID arrays = types.builtinClass(BuiltinClasses::Array);
        TypeID expected = findExpectation(walker, list).type;
        if (proven && !isNullishType(proven) && !lengthOfFreshArguments(walker, value) && types.overlap(expected ? expected : proven, arrays))
            testUnlessDecided(list, arrays);
    }
    if (tests.isEmpty() || (tests.size() == 2 && tests[0].input == tests[1].input))
        return false;

    auto typeBehindTests = [&](ValueID input) {
        for (const TestOfForwardedTailCall& test : tests) {
            if (test.input == input)
                return types.intersectionOf(walker.typeOf(test.value), test.tested);
        }
        Reference value = walker.referenceOf(input);
        return value ? walker.typeOf(value) : TypeID();
    };
    if (!rowTakesArgumentsOfForwardedTailCall(walker, arguments, table.at(row), TypesOfInputs(typeBehindTests)))
        return false;
    // The source writes the name, and the table declares what the row takes.
    if (!shouldOpenGuess(walker, tests[0].value, tests[0].tested, true, closedOperationsOfForwardedTailCall))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || !walker.unitReturnsAtOnce(written))
        return false;

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    uint32_t place = call.id().index() - block.firstValue().index();
    BlockID asWritten;
    Vector<Reference, 2> proofs;
    for (const TestOfForwardedTailCall& test : tests) {
        if (walker.isUnreachable())
            break;
        Draft typeTest(Opcode::TypeTest, originInFrontOfCall);
        walker.chooseConservativeFields(typeTest);
        typeTest.setValueOperand(test.value);
        typeTest.setTestedType(test.tested);
        Reference passes = walker.build(WTF::move(typeTest));
        if (!passes)
            return true;
        BlockID passing = walker.addBlock();
        if (!asWritten) {
            asWritten = walker.addBlock(BlockFrequency::Rare);
            walker.noteGuessOpened();
        }
        walker.branch(passes, passing, asWritten);
        walker.continueIn(passing);

        Draft proof(Opcode::Narrowed, originInFrontOfCall);
        walker.chooseConservativeFields(proof);
        proof.setValueOperand(test.value);
        proof.setTestOperand(passes);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        proofs.append(walker.build(WTF::move(proof)));
    }
    bool everyTestMayPass = !walker.isUnreachable();
    BlockID forwarding = walker.currentBlock();

    walker.continueIn(asWritten);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        walker.endArmWithReturnOf(block.id());
    }
    if (!everyTestMayPass || walker.hasFailed())
        return true;

    walker.continueIn(forwarding);
    Vector<Reference, 2> boundBefore;
    for (size_t position = 0; position < tests.size(); ++position) {
        boundBefore.append(walker.referenceBoundTo(tests[position].input));
        walker.bind(tests[position].input, proofs[position]);
    }
    walker.walkStretch(block.id(), place, place + 1);
    walker.endArmWithReturnOf(block.id());
    for (size_t position = 0; position < tests.size(); ++position)
        walker.bind(tests[position].input, boundBefore[position]);
    return true;
}

// ---- The function's own arguments

static ValueID rootOfOperandOfForwardedTailCall(const Code& code, Reference operand)
{
    return operand && operand.isValue() ? rootOfCellInSweptCode(code, operand.id()) : ValueID();
}

// `function wrap(a, b) { return f.apply(this, arguments) }`. Whoever has read the object or stored into it so far has left it so that the state says what it holds, and no arm changes it.
// An arm for each number of arguments that the parameters can hold passes those parameters on: neither the object nor the copy is made there. Where more were passed, the call takes a new list of the parameters and of what lies behind them.
bool passOnOwnArgumentsByCount(Walker& walker, const BodyValue& call, const Draft& written, Reference callee, Reference thisValueOrNull, Reference arguments)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !walker.opensGuesses() || walker.isUnreachable())
        return false;
    Reference count = lengthOfFreshArguments(walker, arguments);
    if (!count || !count.isValue())
        return false;
    // Of an object without parameters the list of what was passed stands for the object.
    std::optional<ListBehindParametersOfArguments> behind = listBehindParametersOfFreshArguments(walker, arguments, true);
    if (!behind || !behind->firstPosition || behind->firstPosition > mostArgumentsPassedOnByCount)
        return false;
    const Code& code = walker.code();
    ValueID object = rootOfOperandOfForwardedTailCall(code, arguments);
    if (!object || code.at(object).opcode != Opcode::CreateUnmappedArguments || !isListAsMade(walker, arguments))
        return false;
    // A call that takes the object itself needs it.
    if (rootOfOperandOfForwardedTailCall(code, callee) == object || rootOfOperandOfForwardedTailCall(code, thisValueOrNull) == object)
        return false;
    Vector<Reference, mostArgumentsPassedOnByCount> parameters;
    for (uint32_t position = 0; position < behind->firstPosition; ++position) {
        parameters.append(elementOfFreshArguments(walker, arguments, position));
        if (!parameters.last())
            return false;
    }
    if (walker.facts().runsAtMostOnce(walker.currentFunction()))
        return false;

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    // Whoever Link lists for the call may be entered from any arm with whatever the call as written hands over.
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.arrivals().leaveAnything(listed);

    // Arms that met in front of the ReturnCall would keep the frame.
    bool returnsAtOnce = walker.unitReturnsAtOnce(written);
    BlockID behindCall;
    Reference resultBehindCall;
    if (!returnsAtOnce) {
        behindCall = walker.addBlock();
        resultBehindCall = walker.addParameter(behindCall, { }, Representation::JSValue);
    }
    auto buildCallAndEndArm = [&](Draft&& forwarded) {
        forwarded.setCalleeOperand(callee);
        if (thisValueOrNull)
            forwarded.setThisValueOperand(thisValueOrNull);
        forwarded.setCalleeName(written.calleeName());
        forwarded.setExpectedResult(written.expectedResult());
        forwarded.expected = written.expected;
        forwarded.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
        forwarded.inputValueGiven = written.inputValueGiven;
        walker.bind(call.id(), { });
        if (!callForwardedDirectly(walker, call, forwarded)) {
            if (Reference result = walker.build(WTF::move(forwarded)))
                walker.bind(call.id(), result);
        }
        Reference result = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
        walker.bind(call.id(), { });
        if (!result)
            return;
        if (returnsAtOnce)
            walker.returnAtOnce(result);
        else
            walker.jumpTo(behindCall, singleElementSpan(result));
    };

    Type typeOfCount = walker.types().at(walker.typeOf(count));
    for (uint32_t numberPassed = behind->firstPosition + 1; numberPassed-- && !walker.isUnreachable();) {
        if (typeOfCount.isInteger() && !typeOfCount.integers.contains(numberPassed))
            continue;
        Draft number(Opcode::Integer, originInFrontOfCall);
        number.setInteger(numberPassed);
        Draft test(Opcode::StrictEq, originInFrontOfCall);
        walker.chooseConservativeFields(test);
        test.setLeftOperand(count);
        test.setRightOperand(walker.constant(WTF::move(number)));
        if (walker.has(Mechanism::TypedOperations))
            test.setForm(formAllowedForTypedOperator(walker, test));
        Reference isNumberPassed = walker.build(WTF::move(test));
        if (!isNumberPassed)
            return true;
        BlockID passesParameters = walker.addBlock();
        BlockID otherwise = walker.addBlock(numberPassed ? BlockFrequency::Normal : BlockFrequency::Unlikely);
        walker.branch(isNumberPassed, passesParameters, otherwise);

        walker.continueIn(passesParameters);
        Draft plain(Opcode::Call, written.origin());
        walker.chooseConservativeFields(plain);
        plain.setArgumentsOperands(parameters.span().first(numberPassed));
        buildCallAndEndArm(WTF::move(plain));
        walker.continueIn(otherwise);
    }

    // Every parameter was passed. The stub of a tail call reads the list once the frame has gone, so the list lies in the heap.
    if (!walker.isUnreachable()) {
        Draft all(Opcode::NewArray, originInFrontOfCall);
        walker.chooseConservativeFields(all);
        all.standsForNoValueOfInput = true;
        all.setContents(TypeTable::anything);
        all.setVariadicOperands(parameters.span());
        Reference list = walker.build(WTF::move(all));
        noteFreshList(walker, list);
        Draft append(Opcode::AppendElements, originInFrontOfCall);
        walker.chooseConservativeFields(append);
        append.setArrayOperand(list);
        append.setSourceOperand(behind->list);
        if (list)
            walker.build(WTF::move(append));
        if (!walker.isUnreachable()) {
            Draft withList(Opcode::CallList, written.origin());
            walker.chooseConservativeFields(withList);
            withList.setListOperand(list);
            buildCallAndEndArm(WTF::move(withList));
        }
    }

    if (returnsAtOnce)
        return true;
    walker.continueIn(behindCall);
    if (!walker.isUnreachable())
        walker.bind(call.id(), resultBehindCall);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
