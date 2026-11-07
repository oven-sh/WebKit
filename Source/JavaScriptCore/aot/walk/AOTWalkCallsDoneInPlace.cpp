/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct OperandsOfForwardedCall {
    BuiltinOperand callee;
    std::optional<BuiltinOperand> thisValue;
    std::optional<BuiltinOperand> newTarget;
    std::optional<BuiltinOperand> list;
    std::optional<unsigned> firstArgument;
    bool constructs { false };
};

struct RowOfForwardedCall {
    BuiltinOverloadID overload;
    OperandsOfForwardedCall operands;
};

} // anonymous namespace

static bool isSingleOperandOfForwardedCall(BuiltinOperand operand)
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

static std::optional<BuiltinOperand> findSingleOperandOfForwardedCall(const BuiltinPlace& place, BuiltinPlaceKind kind)
{
    if (place.kind != kind || !place.operand || !isSingleOperandOfForwardedCall(*place.operand))
        return std::nullopt;
    return place.operand;
}

static const BuiltinCallback* findCallbackOfForwardedCall(const BuiltinOverload& overload)
{
    const BuiltinFlow& flow = overload.flow;
    if (overload.isUndeclared() || overload.mode != BuiltinCallMode::Call)
        return nullptr;
    if (flow.callbacks.size() != 1 || !flow.transfers.empty() || !flow.propertyActions.empty() || !flow.handlerCalls.empty())
        return nullptr;
    if (!overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode) || !overload.effects.writes.isEmpty())
        return nullptr;

    const BuiltinCallback& callback = flow.callbacks.front();
    if (callback.times != BuiltinCallbackTimes::Once || callback.resultUse != BuiltinCallbackResultUse::Returned || !isSingleOperandOfForwardedCall(callback.callee))
        return nullptr;
    return &callback;
}

static std::optional<OperandsOfForwardedCall> findOperandsOfForwardedCall(const BuiltinOverload& overload)
{
    const BuiltinCallback* callback = findCallbackOfForwardedCall(overload);
    if (!callback)
        return std::nullopt;
    OperandsOfForwardedCall operands { callback->callee, { }, { }, { }, { }, callback->constructs };

    if (callback->thisValue.kind != BuiltinPlaceKind::None) {
        operands.thisValue = findSingleOperandOfForwardedCall(callback->thisValue, BuiltinPlaceKind::Operand);
        if (!operands.thisValue)
            return std::nullopt;
    }
    if (callback->newTarget.kind != BuiltinPlaceKind::None) {
        operands.newTarget = findSingleOperandOfForwardedCall(callback->newTarget, BuiltinPlaceKind::Operand);
        if (!operands.newTarget)
            return std::nullopt;
    }

    const BuiltinPlace& spread = callback->spreadArguments;
    if (spread.kind != BuiltinPlaceKind::None) {
        operands.list = findSingleOperandOfForwardedCall(spread, BuiltinPlaceKind::OwnValues);
        if (!operands.list || !callback->arguments.empty())
            return std::nullopt;
    }
    if (!callback->arguments.empty()) {
        const BuiltinPlace& passed = callback->arguments.front();
        if (callback->arguments.size() != 1 || passed.kind != BuiltinPlaceKind::Operand || !passed.operand || isSingleOperandOfForwardedCall(*passed.operand))
            return std::nullopt;
        operands.firstArgument = passed.operand->argumentIndex();
    }

    auto runsOtherProtocols = [&](const BuiltinParameter& parameter) {
        return parameter.operand != operands.list && parameter.operand != operands.newTarget && !parameter.protocols.isEmpty();
    };
    if ((overload.receiver && runsOtherProtocols(*overload.receiver)) || std::ranges::any_of(overload.parameters, runsOtherProtocols))
        return std::nullopt;
    return operands;
}

static std::optional<RowOfForwardedCall> findRowOfCallOfBuiltin(Walker& walker, const BodyValue& call, BuiltinID builtin)
{
    auto written = call.as<Opcode::Call>();
    const BuiltinTable& table = walker.program().builtins();

    Vector<BuiltinOverloadID, 4> rows;
    for (BuiltinOverloadID overload : table.overloadIDsOf(builtin)) {
        if (table.at(overload).mode == BuiltinCallMode::Call)
            rows.append(overload);
    }
    if (std::ranges::none_of(rows, [&](BuiltinOverloadID row) { return !!findCallbackOfForwardedCall(table.at(row)); }))
        return std::nullopt;
    if (rows.size() > 1) {
        auto typeOfOperand = [&](ValueID operand) {
            Reference built = walker.referenceOf(operand);
            return built ? walker.typeOf(built) : TypeID();
        };
        BuiltinOperandTypes passed;
        if (ValueID thisValue = written.thisValueOperand())
            passed.receiver = typeOfOperand(thisValue);
        for (ValueID argument : written.argumentsOperands())
            passed.arguments.append(typeOfOperand(argument));
        rows = overloadsReachedBy({ walker.program(), walker.types() }, builtin, BuiltinCallMode::Call, passed);
    }
    if (rows.size() != 1)
        return std::nullopt;
    std::optional<OperandsOfForwardedCall> operands = findOperandsOfForwardedCall(table.at(rows[0]));
    if (!operands)
        return std::nullopt;
    return RowOfForwardedCall { rows[0], *operands };
}

static std::optional<RowOfForwardedCall> findRowOfForwardedCall(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || !call.isDoneInPlace() || walker.isUnreachable())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID callee = index.valueBehindCasts(call.as<Opcode::Call>().calleeOperand());
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin)
        return std::nullopt;
    return findRowOfCallOfBuiltin(walker, call, walker.program().builtins().canonicalID(index.value(callee).as<Opcode::Builtin>().builtin()));
}

static Reference operandOfForwardedCall(const Draft& written, BuiltinOperand operand)
{
    if (operand.isReceiver())
        return written.thisValueOperand();
    std::span<const Reference> arguments = written.argumentsOperands();
    return operand.argumentIndex() < arguments.size() ? arguments[operand.argumentIndex()] : Reference();
}

static bool rowTakesArgumentsOfForwardedCall(Walker& walker, const Draft& written, const BuiltinOverload& overload)
{
    BuiltinTypeContext context { walker.program(), walker.types() };
    std::span<const Reference> arguments = written.argumentsOperands();
    auto takes = [&](const BuiltinParameter& parameter, Reference argumentOrNotPassed) {
        OperandFit fit = fitOf(context, argumentOrNotPassed ? walker.typeOf(argumentOrNotPassed) : TypeID(), parameter.accepted);
        return fit.fit == TypeFit::Inside && !fit.unprovenPristine;
    };
    return std::ranges::all_of(overload.parameters, [&](const BuiltinParameter& parameter) {
        if (parameter.operand.isReceiver())
            return true;
        size_t index = parameter.operand.argumentIndex();
        if (isSingleOperandOfForwardedCall(parameter.operand))
            return takes(parameter, index < arguments.size() ? arguments[index] : Reference());
        return std::ranges::all_of(arguments.subspan(std::min(index, arguments.size())), [&](Reference argument) {
            return takes(parameter, argument);
        });
    });
}

static bool buildForwardedCall(Walker& walker, const BodyValue& call, const Draft& written, const RowOfForwardedCall& row)
{
    TypeArena& types = walker.types();
    Reference callee = operandOfForwardedCall(written, row.operands.callee);
    if (!callee)
        return false;

    Reference list = row.operands.list ? operandOfForwardedCall(written, *row.operands.list) : Reference();
    if (list && isNullishType(walker.typeOf(list)))
        list = { };
    std::optional<Vector<Reference, 8>> elements;
    if (list && walker.walksBodyBehindEntry() && !hasKind(types, walker.typeOf(list), OperandKind::Array)
        && passOnOwnArgumentsByCount(walker, call, written, callee, row.operands.thisValue ? operandOfForwardedCall(written, *row.operands.thisValue) : Reference(), list))
        return true;
    if (list && !hasKind(types, walker.typeOf(list), OperandKind::Array)) {
        if (Reference standing = listStandingForFreshArguments(walker, list))
            list = standing;
        else if ((elements = elementsOfFreshArguments(walker, list)))
            list = { };
    }
    if (list && !hasKind(types, walker.typeOf(list), OperandKind::Array)) {
        if (passOnOwnArgumentsByCount(walker, call, written, callee, row.operands.thisValue ? operandOfForwardedCall(written, *row.operands.thisValue) : Reference(), list))
            return true;
        Origin originBeforeCall = written.origin();
        originBeforeCall.isTailCall = false;
        TypeID arrays = types.builtinClass(BuiltinClasses::Array);
        BlockID behindList;
        Reference listOfCall;
        if (walker.has(Mechanism::Guesses) && walker.opensGuesses() && list.isValue() && types.overlap(walker.typeOf(list), arrays)) {
            Draft typeTest(Opcode::TypeTest, originBeforeCall);
            walker.chooseConservativeFields(typeTest);
            typeTest.setValueOperand(list);
            typeTest.setTestedType(arrays);
            Reference isArray = walker.build(WTF::move(typeTest));
            if (!isArray)
                return true;
            BlockID goesOnAsItIs = walker.addBlock();
            BlockID isReadFirst = walker.addBlock();
            behindList = walker.addBlock();
            listOfCall = walker.addParameter(behindList, { }, Representation::Cell);
            walker.branch(isArray, goesOnAsItIs, isReadFirst);

            walker.continueIn(goesOnAsItIs);
            Draft proof(Opcode::Narrowed, originBeforeCall);
            walker.chooseConservativeFields(proof);
            proof.setValueOperand(list);
            proof.setTestOperand(isArray);
            proof.setOperandIndex(OperandIndex::TypeTest::value);
            proof.setIsOnTakenEdge(true);
            if (Reference array = walker.build(WTF::move(proof)))
                walker.jumpTo(behindList, singleElementSpan(array));
            walker.continueIn(isReadFirst);
        }
        Draft newList(Opcode::NewArgumentList, originBeforeCall);
        walker.chooseConservativeFields(newList);
        newList.setArrayLikeOperand(list);
        list = walker.build(WTF::move(newList));
        if (behindList) {
            if (list)
                walker.jumpTo(behindList, singleElementSpan(list));
            walker.continueIn(behindList);
            list = walker.isUnreachable() ? Reference() : listOfCall;
        }
        if (!list)
            return true;
    }

    Draft forwarded(list ? Opcode::CallList : Opcode::Call, written.origin());
    walker.chooseConservativeFields(forwarded);
    forwarded.setCalleeOperand(callee);
    if (row.operands.thisValue)
        forwarded.setThisValueOperand(operandOfForwardedCall(written, *row.operands.thisValue));
    if (list)
        forwarded.setListOperand(list);
    else if (elements)
        forwarded.setArgumentsOperands(elements->span());
    else if (row.operands.firstArgument) {
        std::span<const Reference> arguments = written.argumentsOperands();
        forwarded.setArgumentsOperands(arguments.subspan(std::min<size_t>(*row.operands.firstArgument, arguments.size())));
    }
    forwarded.setCalleeName(written.calleeName());
    forwarded.setExpectedResult(written.expectedResult());
    forwarded.expected = written.expected;
    forwarded.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    forwarded.inputValueGiven = written.inputValueGiven;
    if (callForwardedDirectly(walker, call, forwarded) || callBuiltinForwarded(walker, call, forwarded))
        return true;
    if (Reference result = walker.build(WTF::move(forwarded)))
        walker.bind(call.id(), result);
    return true;
}

static bool buildForwardedConstruction(Walker& walker, const BodyValue& call, const Draft& written, const RowOfForwardedCall& row)
{
    const BuiltinImplementation& implementation = walker.program().builtins().at(row.overload).code.implementation;
    if (implementation.kind != BuiltinImplementationKind::Inline || !implementation.inlineBuiltin || !implementation.otherwise)
        return false;
    if (row.operands.callee.isReceiver() || !row.operands.list || row.operands.list->isReceiver() || (row.operands.newTarget && row.operands.newTarget->isReceiver()))
        return false;

    unsigned numberOfArguments = std::max(row.operands.callee.argumentIndex(), row.operands.list->argumentIndex()) + 1;
    if (row.operands.newTarget)
        numberOfArguments = std::max(numberOfArguments, row.operands.newTarget->argumentIndex() + 1);
    std::span<const Reference> arguments = written.argumentsOperands();
    if (arguments.size() < numberOfArguments)
        return false;

    Vector<uint32_t, 4> parameters;
    for (uint8_t parameter : std::span(implementation.inlineParameters).first(numberOfParametersOf(*implementation.inlineBuiltin)))
        parameters.append(parameter);
    TerminatorDraft construction(TerminatorKind::TryCallInline, written.origin());
    construction.setInlineBuiltin(*implementation.inlineBuiltin);
    construction.setInlineParameters(parameters.span());
    construction.setOverload(row.overload);
    construction.setArgumentsOperands(arguments.first(numberOfArguments));

    BlockID done = walker.addBlock();
    BlockID miss = walker.addBlock(BlockFrequency::Rare);
    BlockID join = walker.addBlock();
    Reference result = walker.addParameter(join, { }, Representation::JSValue);
    Reference constructed = walker.tryCallInline(WTF::move(construction), done, miss);
    if (!constructed)
        return true;
    walker.continueIn(done);
    walker.jumpTo(join, singleElementSpan(constructed));

    walker.continueIn(miss);
    Draft asWritten = written;
    walker.chooseConservativeFields(asWritten);
    if (Reference called = walker.build(WTF::move(asWritten)))
        walker.jumpTo(join, singleElementSpan(called));
    else
        walker.continueIn(join);
    if (!walker.isUnreachable())
        walker.bind(call.id(), result);
    return true;
}

bool isCallForwardedInPlace(Walker& walker, const BodyValue& call)
{
    return findRowOfForwardedCall(walker, call).has_value();
}

bool forwardCallOfBuiltin(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || walker.isUnreachable())
        return false;
    Reference callee = walker.referenceOf(call.as<Opcode::Call>().calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return false;
    Type type = walker.types().at(proven);
    BuiltinID builtin = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    if (!builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    std::optional<RowOfForwardedCall> row = findRowOfCallOfBuiltin(walker, call, table.canonicalID(builtin));
    if (!row || row->operands.constructs)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || !rowTakesArgumentsOfForwardedCall(walker, written, table.at(row->overload)))
        return false;
    return buildForwardedCall(walker, call, written, *row);
}

static constexpr uint32_t closedOperationsOfForwardedCall = 2;

static constexpr OpcodeSet argumentsObjectsOfForwardedCall { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
static constexpr OpcodeSet listsMadeAgainForForwardedCall = argumentsObjectsOfForwardedCall | OpcodeSet { Opcode::NewArray };

static void makeListForCallAsWritten(Walker& walker, const BodyValue& call, const RowOfForwardedCall& row, Draft& asWritten)
{
    if (!row.operands.list || row.operands.list->isReceiver())
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    size_t position = row.operands.list->argumentIndex();
    if (position >= arguments.size() || index.isParameter(arguments[position]) || !listsMadeAgainForForwardedCall.contains(index.opcodeOf(arguments[position])))
        return;
    ValueID object = arguments[position];
    std::optional<BodyUser> user = index.onlyUser(object);
    unsigned timesNamedByCall = 0;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        timesNamedByCall += operand == object;
    });
    if (!user || user->kind != BodyUseKind::Operand || user->value != call.id() || timesNamedByCall != 1)
        return;
    if (index.loopOf(index.blockOf(object)) != index.loopOf(index.blockOf(call.id())))
        return;
    Reference built = walker.referenceOf(object);
    bool isArgumentsObject = argumentsObjectsOfForwardedCall.contains(index.opcodeOf(object));
    std::optional<Vector<Reference, 8>> elements = isArgumentsObject ? std::nullopt : elementsOfFreshList(walker, built);
    if (isArgumentsObject ? !lengthOfFreshArguments(walker, built) || !listBehindParametersOfFreshArguments(walker, built, true) : !elements)
        return;

    Draft again = walker.draftOf(index.value(object));
    again.standsForNoValueOfInput = true;
    again.inputValueGiven = { };
    if (elements)
        again.setElementsOperands(elements->span());
    if (again.opcode == Opcode::CreateUnmappedArguments && walker.walksBodyBehindEntry()) {
        if (Reference list = walker.makeHiddenListOfEntryAgain())
            again.setRestOperand(list);
    }
    walker.chooseConservativeFields(again);
    Reference made = walker.build(WTF::move(again));
    if (!made)
        return;
    Vector<Reference, 8> passed(asWritten.argumentsOperands());
    passed[position] = made;
    asWritten.setArgumentsOperands(passed.span());
}

bool forwardCallBehindTestOfCallee(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    auto operation = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return false;
    if (index.valueBehindCasts(index.value(read).objectOperand()) != index.valueBehindCasts(operation.thisValueOperand()))
        return false;

    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID member = findConstantMemberOnceForBuild(walker, table.at(BuiltinClasses::Function).prototype, index.value(read).key());
    if (!member)
        return false;
    BuiltinID builtin = table.canonicalID(member);
    std::optional<RowOfForwardedCall> row = isTestableBuiltin(table, builtin) ? findRowOfCallOfBuiltin(walker, call, builtin) : std::nullopt;
    if (!row || row->operands.constructs || !row->operands.callee.isReceiver())
        return false;

    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(operation.calleeOperand());
    Reference receiver = walker.referenceOf(operation.thisValueOperand());
    TypeID expected = types.forBuiltin(builtin);
    if (!callee || !callee.isValue() || !receiver || !types.overlap(walker.typeOf(callee), expected))
        return false;
    if (!shouldOpenGuess(walker, callee, expected, true, closedOperationsOfForwardedCall))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || !rowTakesArgumentsOfForwardedCall(walker, written, table.at(row->overload)))
        return false;

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    Draft test(Opcode::TypeTest, originInFrontOfCall);
    test.setValueOperand(callee);
    test.setTestedType(expected);
    Reference isExpected = walker.build(WTF::move(test));
    if (!isExpected)
        return true;
    walker.noteGuessOpened();
    BlockID forwarding = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isExpected, forwarding, asWritten);

    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(listed);

    walker.continueIn(asWritten);
    Draft closed = written;
    makeListForCallAsWritten(walker, call, *row, closed);
    walker.chooseConservativeFields(closed);
    Reference result = walker.build(WTF::move(closed));
    BlockID behindCall;
    Reference resultBehindCall;
    if (walker.unitReturnsAtOnce(written))
        walker.returnAtOnce(result);
    else {
        behindCall = walker.addBlock();
        resultBehindCall = walker.addParameter(behindCall, { }, Representation::JSValue);
        if (result)
            walker.jumpTo(behindCall, singleElementSpan(result));
    }

    walker.continueIn(forwarding);
    walker.bind(call.id(), { });
    buildForwardedCall(walker, call, written, *row);
    if (!behindCall)
        return true;
    Reference forwardedResult = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (forwardedResult)
        walker.jumpTo(behindCall, singleElementSpan(forwardedResult));
    else
        walker.continueIn(behindCall);
    if (!walker.isUnreachable())
        walker.bind(call.id(), resultBehindCall);
    return true;
}

bool replaceCallDoneInPlace(Walker& walker, const BodyValue& call)
{
    std::optional<RowOfForwardedCall> row = findRowOfForwardedCall(walker, call);
    if (!row)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return true;
    if (!rowTakesArgumentsOfForwardedCall(walker, written, walker.program().builtins().at(row->overload)))
        return false;
    return row->operands.constructs ? buildForwardedConstruction(walker, call, written, *row) : buildForwardedCall(walker, call, written, *row);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
