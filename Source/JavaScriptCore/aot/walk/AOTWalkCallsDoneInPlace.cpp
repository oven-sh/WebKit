/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTResultRulesInternal.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTTypeRelations.h"
#include "AOTWalkStrings.h"
#include "JSBoundFunction.h"
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
    ValueID callee = index.stripCasts(call.as<Opcode::Call>().calleeOperand());
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

static bool rowTakesArgumentsOfForwardedCall(Walker& walker, const Draft& written, const BuiltinOverload& overload, std::optional<BuiltinOperand> operandAskedAtRunTime = std::nullopt)
{
    BuiltinTypeContext context { walker.program(), walker.types() };
    std::span<const Reference> arguments = written.argumentsOperands();
    auto takes = [&](const BuiltinParameter& parameter, Reference argumentOrNotPassed) {
        OperandFit fit = fitOf(context, argumentOrNotPassed ? walker.typeOf(argumentOrNotPassed) : TypeID(), parameter.accepted);
        return fit.fit == TypeFit::Inside && !fit.unprovenPristine;
    };
    return std::ranges::all_of(overload.parameters, [&](const BuiltinParameter& parameter) {
        if (parameter.operand.isReceiver() || parameter.operand == operandAskedAtRunTime)
            return true;
        size_t index = parameter.operand.argumentIndex();
        if (isSingleOperandOfForwardedCall(parameter.operand))
            return takes(parameter, index < arguments.size() ? arguments[index] : Reference());
        return std::ranges::all_of(arguments.subspan(std::min(index, arguments.size())), [&](Reference argument) {
            return takes(parameter, argument);
        });
    });
}

static bool callExpectedFunctionDirectly(Walker& walker, const BodyValue& call, const Draft& forwarded, ValueID input)
{
    if (!walker.isInLoop() || !walker.opensGuesses() || forwarded.opcode != Opcode::Call || !input)
        return false;
    Reference callee = forwarded.calleeOperand();
    if (!callee.isValue() || findOnlyCallee(walker, call, callee))
        return false;
    FunctionID expected = functionExpectedOf(walker, input);
    if (!expected || isClassConstructorOf(walker.program().at(expected).kind) || walker.program().at(expected).isCoroutine())
        return false;
    Origin origin = forwarded.origin();
    origin.isTailCall = false;
    Draft test(Opcode::IsClosureOf, origin);
    walker.chooseConservativeFields(test);
    test.setFunction(expected);
    test.setValueOperand(callee);
    Reference isExpected = walker.build(WTF::move(test));
    if (!isExpected || !isExpected.isValue() || walker.isUnreachable())
        return true;
    walker.recordSpeculation();
    BlockID passed = walker.addBlock();
    BlockID failed = walker.addBlock();
    BlockID meeting = walker.addBlock();
    walker.branch(isExpected, passed, failed);
    Vector<Walker::JoinArm, 2> arms;
    auto endArm = [&](Reference result) {
        if (walker.hasFailed() || walker.isUnreachable() || !result)
            return;
        Walker::JoinArm arm { walker.currentBlock(), { } };
        arm.values.append(result);
        arms.append(WTF::move(arm));
    };
    walker.continueIn(passed);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(callee);
    proof.setTestOperand(isExpected);
    proof.setOperandIndex(OperandIndex::IsClosureOf::value);
    proof.setIsOnTakenEdge(true);
    if (Reference closure = walker.build(WTF::move(proof))) {
        Draft direct = forwarded;
        direct.setCalleeOperand(closure);
        if (callForwardedDirectly(walker, call, direct))
            endArm(walker.referenceOf(call.id()));
        else if (!walker.hasFailed() && !walker.isUnreachable())
            endArm(walker.build(WTF::move(direct)));
    }
    if (walker.hasFailed())
        return true;
    walker.continueIn(failed);
    Draft closed = forwarded;
    endArm(walker.build(WTF::move(closed)));
    if (walker.hasFailed())
        return true;
    walker.bind(call.id(), Reference());
    Vector<Reference, 1> met(1);
    walker.joinArms(arms.span(), meeting, met.mutableSpan());
    if (!arms.isEmpty() && !walker.hasFailed())
        walker.bind(call.id(), met[0]);
    return true;
}

static bool buildForwardedCall(Walker& walker, const BodyValue& call, const Draft& written, const RowOfForwardedCall& row)
{
    TypeArena& types = walker.types();
    Reference callee = operandOfForwardedCall(written, row.operands.callee);
    if (!callee)
        return false;

    bool constructs = row.operands.constructs;
    Reference list = row.operands.list ? operandOfForwardedCall(written, *row.operands.list) : Reference();
    if (list && isNullishType(walker.typeOf(list)))
        list = { };
    std::optional<Vector<Reference, 8>> elements;
    if (list && !constructs && walker.walksBodyBehindEntry() && !hasKind(types, walker.typeOf(list), OperandKind::Array)
        && passOnOwnArgumentsByCount(walker, call, written, callee, row.operands.thisValue ? operandOfForwardedCall(written, *row.operands.thisValue) : Reference(), list))
        return true;
    if (list && !hasKind(types, walker.typeOf(list), OperandKind::Array)) {
        if (Reference standing = listStandingForFreshArguments(walker, list))
            list = standing;
        else if ((elements = elementsOfFreshArguments(walker, list)))
            list = { };
    }
    if (list && !hasKind(types, walker.typeOf(list), OperandKind::Array)) {
        if (!constructs && passOnOwnArgumentsByCount(walker, call, written, callee, row.operands.thisValue ? operandOfForwardedCall(written, *row.operands.thisValue) : Reference(), list))
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

    Origin originOfForwarded = written.origin();
    if (constructs)
        originOfForwarded.isTailCall = false;
    std::optional<uint32_t> lengthInSight = list ? lengthOfFreshList(walker, list) : std::nullopt;
    bool fitsCallOfBuiltin = lengthInSight && *lengthInSight <= defaultLimitValue(LimitID::MaxBuiltinCallArguments, hostArchitecture());
    if (std::optional<Vector<Reference, 8>> known = fitsCallOfBuiltin ? elementsOfFreshList(walker, list) : std::nullopt) {
        walker.checkWhatIsInSightOfList(list, originOfForwarded);
        elements = WTF::move(known);
        list = { };
    }
    Opcode opcodeOfList = constructs ? Opcode::ConstructList : Opcode::CallList;
    Draft forwarded(list ? opcodeOfList : constructs ? Opcode::Construct : Opcode::Call, originOfForwarded);
    walker.chooseConservativeFields(forwarded);
    forwarded.setCalleeOperand(callee);
    if (row.operands.thisValue)
        forwarded.setThisValueOperand(operandOfForwardedCall(written, *row.operands.thisValue));
    if (row.operands.newTarget)
        forwarded.setNewTargetOperand(operandOfForwardedCall(written, *row.operands.newTarget));
    if (list)
        forwarded.setListOperand(list);
    else if (elements)
        forwarded.setArgumentsOperands(elements->span());
    else if (row.operands.firstArgument) {
        std::span<const Reference> arguments = written.argumentsOperands();
        forwarded.setArgumentsOperands(arguments.subspan(std::min<size_t>(*row.operands.firstArgument, arguments.size())));
    }
    forwarded.setCalleeName(written.calleeName());
    if (!constructs)
        forwarded.setExpectedResult(written.expectedResult());
    forwarded.expected = written.expected;
    forwarded.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    forwarded.inputValueGiven = written.inputValueGiven;
    if (constructs ? constructForwardedDirectly(walker, call, forwarded) : callForwardedDirectly(walker, call, forwarded) || callBuiltinForwarded(walker, call, forwarded))
        return true;
    if (goOverListInPlaceOfCallOfBuiltin(walker, call, forwarded))
        return true;
    ValueID input = row.operands.callee.isReceiver() && call.opcode() == Opcode::Call ? call.as<Opcode::Call>().thisValueOperand() : ValueID();
    if (!constructs && callExpectedFunctionDirectly(walker, call, forwarded, input))
        return true;
    if (Reference result = walker.build(WTF::move(forwarded)))
        walker.bind(call.id(), result);
    return true;
}

static bool constructionIsToldFromNewByNothing(Walker& walker, const BodyValue& call, const Draft& written, const RowOfForwardedCall& row)
{
    auto isConstructorOfClass = [&](Reference value) {
        FunctionID only = value ? findOnlyCallee(walker, call, value) : FunctionID();
        return only && isClassConstructorOf(walker.program().at(only).kind);
    };
    Reference list = row.operands.list ? operandOfForwardedCall(written, *row.operands.list) : Reference();
    if (!list || !walker.types().isSubset(walker.typeOf(list), TypeTable::object) || !isConstructorOfClass(operandOfForwardedCall(written, row.operands.callee)))
        return false;
    return !row.operands.newTarget || isConstructorOfClass(operandOfForwardedCall(written, *row.operands.newTarget));
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

static bool forwardCallBehindTestOfTarget(Walker&, const BodyValue& call, const Draft& written, const RowOfForwardedCall&);

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
    if (!row)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    if (!rowTakesArgumentsOfForwardedCall(walker, written, table.at(row->overload)))
        return forwardCallBehindTestOfTarget(walker, call, written, *row);
    if (row->operands.constructs && !constructionIsToldFromNewByNothing(walker, call, written, *row))
        return false;
    TypeID receiver = written.thisValueOperand() ? walker.typeOf(written.thisValueOperand()) : TypeID();
    if (row->operands.callee.isReceiver() && (!receiver || !walker.types().isSubset(receiver, TypeTable::function))) {
        BuiltinID spelled = written.calleeKey() ? findOwnMember(walker.program(), Builtins::FunctionPrototype, written.calleeKey()) : BuiltinID();
        if (spelled != table.canonicalID(builtin))
            return false;
    }
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
    if (position >= arguments.size())
        return;
    ValueID object = arguments[position];
    const Function& function = walker.program().at(walker.currentFunction());
    bool isListOfCopyInLine = walker.depthOfCopies() && function.has(FunctionFlags::HasRestParameter) && object == index.block(entryBlock).parameter(function.firstParameterIndex() + function.numberOfPositionalParameters());
    if (!isListOfCopyInLine && (index.isParameter(object) || !listsMadeAgainForForwardedCall.contains(index.opcodeOf(object))))
        return;
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
    bool isArgumentsObject = !isListOfCopyInLine && argumentsObjectsOfForwardedCall.contains(index.opcodeOf(object));
    std::optional<Vector<Reference, 8>> elements = isArgumentsObject ? std::nullopt : elementsOfFreshList(walker, built);
    if (isArgumentsObject ? !lengthOfFreshArguments(walker, built) || !listBehindParametersOfFreshArguments(walker, built, true) : !elements)
        return;

    Origin originInFrontOfCall = asWritten.origin();
    originInFrontOfCall.isTailCall = false;
    Draft again = isArgumentsObject ? walker.draftOf(index.value(object)) : Draft(Opcode::NewArray, originInFrontOfCall);
    if (!isArgumentsObject)
        again.setContents(TypeTable::anything);
    again.standsForNoValueOfInput = true;
    again.inputValueGiven = { };
    if (elements)
        again.setElementsOperands(elements->span());
    if (again.opcode == Opcode::CreateUnmappedArguments && walker.walksBodyBehindEntry()) {
        if (Reference list = walker.rematerializeHiddenRestArray())
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

static bool forwardCallBehindTestOfTarget(Walker& walker, const BodyValue& call, const Draft& written, const RowOfForwardedCall& row)
{
    if (row.operands.constructs || row.operands.callee.isReceiver() || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    TypeArena& types = walker.types();
    Reference target = operandOfForwardedCall(written, row.operands.callee);
    if (!target || !target.isValue() || types.isSubset(walker.typeOf(target), TypeTable::function) || !types.overlap(walker.typeOf(target), TypeTable::function))
        return false;
    if (!rowTakesArgumentsOfForwardedCall(walker, written, walker.program().builtins().at(row.overload), row.operands.callee))
        return false;
    if (!shouldOpenGuess(walker, target, TypeTable::function, true, closedOperationsOfForwardedCall))
        return false;

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    Draft test(Opcode::TypeTest, originInFrontOfCall);
    test.setValueOperand(target);
    test.setTestedType(TypeTable::function);
    Reference isFunction = walker.build(WTF::move(test));
    if (!isFunction)
        return true;
    walker.recordSpeculation();
    walker.count(StatisticID::CallsForwardedAfterTargetChecks);
    BlockID forwarding = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isFunction, forwarding, asWritten);

    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(listed);

    walker.continueIn(asWritten);
    Draft closed = written;
    makeListForCallAsWritten(walker, call, row, closed);
    walker.chooseConservativeFields(closed);
    Reference result = walker.build(WTF::move(closed));
    BlockID continuationBlock;
    Reference mergedResult;
    if (walker.unitReturnsAtOnce(written))
        walker.returnAtOnce(result);
    else {
        continuationBlock = walker.addBlock();
        mergedResult = walker.addParameter(continuationBlock, { }, Representation::JSValue);
        if (result)
            walker.jumpTo(continuationBlock, singleElementSpan(result));
    }

    walker.continueIn(forwarding);
    walker.bind(call.id(), { });
    Draft proof(Opcode::Narrowed, originInFrontOfCall);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(target);
    proof.setTestOperand(isFunction);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    if (Reference function = walker.build(WTF::move(proof))) {
        Draft withFunction = written;
        Vector<Reference, 8> passed(written.argumentsOperands());
        passed[row.operands.callee.argumentIndex()] = function;
        withFunction.setArgumentsOperands(passed.span());
        buildForwardedCall(walker, call, withFunction, row);
    }
    if (!continuationBlock)
        return true;
    Reference forwardedResult = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (forwardedResult)
        walker.jumpTo(continuationBlock, singleElementSpan(forwardedResult));
    else
        walker.continueIn(continuationBlock);
    if (!walker.isUnreachable())
        walker.bind(call.id(), mergedResult);
    return true;
}

bool forwardCallBehindTestOfCallee(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls))
        return false;
    auto operation = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return false;
    if (index.stripCasts(index.value(read).objectOperand()) != index.stripCasts(operation.thisValueOperand()))
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
    walker.recordSpeculation();
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
    BlockID continuationBlock;
    Reference mergedResult;
    if (walker.unitReturnsAtOnce(written))
        walker.returnAtOnce(result);
    else {
        continuationBlock = walker.addBlock();
        mergedResult = walker.addParameter(continuationBlock, { }, Representation::JSValue);
        if (result)
            walker.jumpTo(continuationBlock, singleElementSpan(result));
    }

    walker.continueIn(forwarding);
    walker.bind(call.id(), { });
    buildForwardedCall(walker, call, written, *row);
    if (!continuationBlock)
        return true;
    Reference forwardedResult = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (forwardedResult)
        walker.jumpTo(continuationBlock, singleElementSpan(forwardedResult));
    else
        walker.continueIn(continuationBlock);
    if (!walker.isUnreachable())
        walker.bind(call.id(), mergedResult);
    return true;
}

bool forwardConstructionBehindTestOfTarget(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.isInLoop())
        return false;
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.as<Opcode::Call>().calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    BuiltinID builtin = proven && types.at(proven).isObject() ? singleBuiltinOf(types, types.at(proven)) : BuiltinID();
    if (!builtin)
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    std::optional<RowOfForwardedCall> row = findRowOfCallOfBuiltin(walker, call, table.canonicalID(builtin));
    if (!row || !row->operands.constructs || row->operands.newTarget || row->operands.callee.isReceiver() || !row->operands.list)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed() || walker.unitReturnsAtOnce(written) || !rowTakesArgumentsOfForwardedCall(walker, written, table.at(row->overload)) || constructionIsToldFromNewByNothing(walker, call, written, *row))
        return false;
    Reference target = operandOfForwardedCall(written, row->operands.callee);
    Reference list = operandOfForwardedCall(written, *row->operands.list);
    if (!target || !target.isValue() || !list || !elementsOfFreshList(walker, list))
        return false;

    Origin origin = written.origin();
    origin.isTailCall = false;
    BlockID continuationBlock = walker.addBlock();
    Reference mergedResult = walker.addParameter(continuationBlock, { }, Representation::JSValue);
    auto goOnBehindCall = [&] {
        Reference result = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
        if (result)
            walker.jumpTo(continuationBlock, singleElementSpan(result));
    };

    ValueID inputOfTarget = call.as<Opcode::Call>().argumentsOperands()[row->operands.callee.argumentIndex()];
    TypeID expected = findExpectation(walker, inputOfTarget).type;
    TypeID closures = expected ? types.withOnlyObjects(expected) : TypeID();
    FunctionID only = closures && !isNeverType(closures) ? singleFunctionOf(types, types.at(closures)) : FunctionID();
    if (only && isClassConstructorOf(program.at(only).kind) && types.overlap(walker.typeOf(target), closures) && walker.withoutFunctionsThatMayNotBeMentioned(closures) == closures
        && shouldOpenGuess(walker, target, closures, false, closedOperationsOfForwardedCall)) {
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(target);
        test.setTestedType(closures);
        Reference isClass = walker.build(WTF::move(test));
        if (!isClass)
            return true;
        walker.recordSpeculation();
        BlockID makes = walker.addBlock();
        BlockID isOther = walker.addBlock();
        walker.branch(isClass, makes, isOther);

        walker.continueIn(makes);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(target);
        narrowed.setTestOperand(isClass);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        Reference boundBefore = walker.referenceBoundTo(inputOfTarget);
        if (Reference closure = walker.build(WTF::move(narrowed))) {
            walker.bind(inputOfTarget, closure);
            Draft ofClass = walker.draftOf(call);
            walker.bind(call.id(), { });
            buildForwardedCall(walker, call, ofClass, *row);
            goOnBehindCall();
        }
        walker.bind(inputOfTarget, boundBefore);
        walker.continueIn(isOther);
    }

    Draft question(Opcode::IsConstructor, origin);
    walker.chooseConservativeFields(question);
    question.setValueOperand(target);
    Reference isConstructor = walker.build(WTF::move(question));
    if (!isConstructor)
        return true;
    BlockID constructs = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isConstructor, constructs, asWritten);

    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(listed);

    walker.continueIn(asWritten);
    Draft closed = written;
    makeListForCallAsWritten(walker, call, *row, closed);
    walker.chooseConservativeFields(closed);
    if (Reference result = walker.build(WTF::move(closed)))
        walker.jumpTo(continuationBlock, singleElementSpan(result));

    walker.continueIn(constructs);
    walker.bind(call.id(), { });
    buildForwardedCall(walker, call, written, *row);
    goOnBehindCall();

    walker.continueIn(continuationBlock);
    if (!walker.isUnreachable())
        walker.bind(call.id(), mergedResult);
    return true;
}

static constexpr uint32_t closedOperationsOfCallOfBoundFunction = 2;

static std::optional<uint32_t> numberOfBoundArgumentsThatLinkWrites(Walker& walker, const ClassPart& expected)
{
    const BuiltinTable& builtins = walker.program().builtins();
    Type written = walker.types().at(expected.writtenContentsAt(builtins, ContentsSlot::Element));
    if (!isNeverType(expected.writtenContentsAt(builtins, ContentsSlot::Key)) || !written.isInteger() || !written.integers.isSingleValue() || written.integers.minimum < 0)
        return std::nullopt;
    return static_cast<uint32_t>(std::min<int64_t>(written.integers.minimum, UINT32_MAX));
}

static bool readsBindOffItsOwnReceiver(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto operation = call.as<Opcode::Call>();
    ValueID read = index.stripCasts(operation.calleeOperand());
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || !operation.thisValueOperand())
        return false;
    if (index.value(read).key() != walker.program().keyOf(BuiltinNames::bind) || index.stripCasts(index.value(read).objectOperand()) != index.stripCasts(operation.thisValueOperand()))
        return false;
    Reference receiver = walker.referenceOf(operation.thisValueOperand());
    TypeID proven = receiver ? walker.typeOf(receiver) : TypeID();
    if (!proven)
        return false;
    const Type& known = walker.types().at(proven);
    return known.anyObject || known.anyFunction || known.hasOnlyClosures();
}

static std::optional<uint32_t> numberOfArgumentsBoundInSight(Walker& walker, ValueID callee, const ClassPart& expected)
{
    TypeID bound = expected.writtenContentsAt(walker.program().builtins(), ContentsSlot::Element);
    if (!bound || isNeverType(bound))
        return 0;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = index.stripCasts(callee);
    if (!made || index.isParameter(made) || index.opcodeOf(made) != Opcode::Call || !readsBindOffItsOwnReceiver(walker, index.value(made)))
        return numberOfBoundArgumentsThatLinkWrites(walker, expected);
    size_t passed = index.value(made).as<Opcode::Call>().argumentsOperands().size();
    return passed ? static_cast<uint32_t>(passed - 1) : 0;
}

static TypeID typeHeldInFieldOfReceiverThatIsRead(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(input);
    while (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::CheckInitialized)
        read = index.stripCasts(index.value(read).operand(0));
    if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::GetModuleVariable)
        return walker.facts().held().variable(index.value(read).as<Opcode::GetModuleVariable>().variable()).proven;
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    TypeArena& types = walker.types();
    TypingContext holding { walker.program(), types, walker.heldPositions(), TypeLevel::Proven };
    ValueID object = index.value(read).objectOperand();
    Reference receiver = walker.referenceOf(object);
    auto heldInAppendedField = [&](TypeID holder) -> TypeID {
        LayoutID layout = holder ? singleLayoutOf(types, types.at(types.withoutNullish(holder))) : LayoutID();
        if (!layout)
            return { };
        std::span<const PropertyKeyID> appended = walker.facts().linked().appendedKeysOf(layout);
        size_t place = std::ranges::find(appended, index.value(read).key()) - appended.begin();
        const LinkedCell* cell = place < appended.size() ? walker.facts().held().findCellOfField({ layout, static_cast<uint32_t>(walker.program().at(layout).fields.size() + place) }) : nullptr;
        return cell ? cell->proven : TypeID();
    };
    if (TypeID held = receiver ? heldInAppendedField(walker.typeOf(receiver)) : TypeID())
        return held;
    TypeID expected = findExpectation(walker, object).type;
    if (TypeID held = heldInAppendedField(expected))
        return held;
    if (TypeID held = receiver ? tryTypeOfFieldRead(holding, walker.typeOf(receiver), index.value(read).key()) : TypeID())
        return held;
    return expected ? tryTypeOfFieldRead(holding, types.withoutNullish(expected), index.value(read).key()) : TypeID();
}

static TypeID primitivesSeenToArriveAtParameter(Walker& walker, FunctionID target, uint32_t position, Reference argument)
{
    const Function& function = walker.program().at(target);
    std::span<const LinkedParameter> parameters = walker.facts().held().parametersOf(target);
    uint32_t parameter = function.firstParameterIndex() + position;
    if (position >= function.numberOfPositionalParameters() || parameter >= parameters.size())
        return { };
    TypeID seen = parameters[parameter].proven;
    return seen && holdsOnlyPrimitives(seen) && !isNeverType(seen) ? typeThatTypeTestAsksForWhereExpected(walker, argument, seen) : TypeID();
}

struct TargetOfBoundFunctionExpected {
    FunctionID target;
    uint32_t numberOfBoundArguments { 0 };
    bool isDeclared { false };
};

static std::optional<TargetOfBoundFunctionExpected> findTargetOfBoundFunctionExpected(Walker& walker, ValueID callee, TypeID proven)
{
    TypeArena& types = walker.types();
    auto partOfBoundFunctions = [&](TypeID type) -> const ClassPart* {
        if (!type || holdsOnlyPrimitives(type))
            return nullptr;
        Type objects = types.at(types.withOnlyObjects(type));
        std::span<const ClassPart> parts = classPartsOf(types, objects);
        return !objects.anyObject && !objects.anyFunction && parts.size() == 1 && parts[0].builtinClass == BuiltinClasses::BoundFunction && parts[0].hasWrittenContents() ? parts.data() : nullptr;
    };
    ExpectationOfValue expectation = proven ? ExpectationOfValue { proven, true } : findExpectation(walker, callee);
    if (!partOfBoundFunctions(expectation.type))
        expectation = findExpectation(walker, walker.currentBodyIndex().stripCasts(callee));
    if (TypeID held = partOfBoundFunctions(expectation.type) ? TypeID() : typeHeldInFieldOfReceiverThatIsRead(walker, callee))
        expectation = { types.withoutNullish(held), false };
    const ClassPart* expected = partOfBoundFunctions(expectation.type);
    if (!expected)
        return std::nullopt;
    TypeID targets = expected->writtenContentsAt(walker.program().builtins(), ContentsSlot::Value);
    FunctionID target = targets && types.at(targets).isObject() ? singleFunctionOf(types, types.at(targets)) : FunctionID();
    std::optional<uint32_t> numberOfBoundArguments = target ? numberOfArgumentsBoundInSight(walker, callee, *expected) : std::nullopt;
    if (!numberOfBoundArguments || *numberOfBoundArguments > JSBoundFunction::maxEmbeddedArgs || !walker.facts().linked().mayBeMentionedBy(walker.unit(), target))
        return std::nullopt;
    return TargetOfBoundFunctionExpected { target, *numberOfBoundArguments, expectation.isDeclared };
}

bool callIsExpectedToEnterTargetOfBoundFunction(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Guesses))
        return false;
    auto operation = call.as<Opcode::Call>();
    std::optional<TargetOfBoundFunctionExpected> expected = findTargetOfBoundFunctionExpected(walker, operation.calleeOperand(), { });
    return expected && copyOfCalleeCalledInLoopPays(walker, expected->target, expected->numberOfBoundArguments + operation.argumentsOperands().size());
}

bool callTargetOfBoundFunctionBehindTests(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || walker.isUnreachable() || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Guesses) || !walker.isInLoop())
        return false;
    auto operation = call.as<Opcode::Call>();
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(operation.calleeOperand());
    if (!callee || !callee.isValue())
        return false;
    TypeID boundFunctions = types.builtinClass(BuiltinClasses::BoundFunction);
    TypeID known = walker.typeOf(callee);
    bool isProven = types.isSubset(known, boundFunctions);
    if (!types.overlap(known, boundFunctions))
        return false;
    std::optional<TargetOfBoundFunctionExpected> expected = findTargetOfBoundFunctionExpected(walker, operation.calleeOperand(), isProven ? known : TypeID());
    if (!expected)
        return false;
    FunctionID target = expected->target;
    if (!shouldOpenGuess(walker, callee, boundFunctions, expected->isDeclared, closedOperationsOfCallOfBoundFunction))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;

    Origin origin = written.origin();
    origin.isTailCall = false;
    bool canGuardInIteration = walker.canRestartIteration() && walker.canBuildIterationGuard();
    BlockID asWritten;
    auto isLoopInvariant = [&](Reference operand) {
        return canGuardInIteration && walker.isInvariantInSpecializedVersion(operand);
    };
    auto continueIf = [&](Reference test) {
        if (canGuardInIteration && walker.tryBranchInPreheader(test, true))
            return true;
        if (!asWritten)
            asWritten = walker.addBlock(BlockFrequency::Rare);
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, asWritten);
        walker.continueIn(passed);
        return false;
    };
    auto askType = [&](Reference value, TypeID tested) {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isLoopInvariant(value));
        return buildTypeTestForStrings(walker, value, tested, origin);
    };
    auto proveBehind = [&](Reference value, Reference test) {
        Walker::SpecializedPreheaderScope preheaderScope(walker, continueIf(test));
        return buildProofBehindTestForStrings(walker, value, test, OperandIndex::TypeTest::value, origin);
    };
    static constexpr uint32_t positionOfFirstArgument = 2;
    bool countIsHeldInFront = false;
    auto wordOf = [&](Reference function, uint32_t position) {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isLoopInvariant(function) && (countIsHeldInFront || position < positionOfFirstArgument));
        Draft word(Opcode::GetWordOfBoundFunction, origin);
        walker.chooseConservativeFields(word);
        word.setFunctionOperand(function);
        word.setIndex(position);
        return walker.build(WTF::move(word));
    };

    Reference bound = callee;
    if (!isProven) {
        Reference isBound = askType(callee, boundFunctions);
        if (!isBound)
            return true;
        bound = proveBehind(callee, isBound);
    }
    Reference hasCount;
    if (bound) {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isLoopInvariant(bound));
        Draft count(Opcode::HasNumberOfBoundArguments, origin);
        walker.chooseConservativeFields(count);
        count.setFunctionOperand(bound);
        count.setCount(expected->numberOfBoundArguments);
        hasCount = walker.build(WTF::move(count));
    }
    if (!hasCount)
        return true;
    countIsHeldInFront = continueIf(hasCount);
    Reference word = wordOf(bound, 0);
    Reference isTarget = word ? askType(word, types.forFunction(target)) : Reference();
    if (!isTarget)
        return true;
    Reference closure = proveBehind(word, isTarget);
    Vector<Reference, 8> passed;
    Reference thisValue = wordOf(bound, 1);
    for (uint32_t position = 0; position < expected->numberOfBoundArguments + (walker.shouldOverstateBoundArgumentCount() && expected->numberOfBoundArguments < JSBoundFunction::maxEmbeddedArgs); ++position) {
        Reference argument = wordOf(bound, 2 + position);
        if (TypeID seen = argument ? primitivesSeenToArriveAtParameter(walker, target, position, argument) : TypeID()) {
            Reference isAsSeen = askType(argument, seen);
            if (!isAsSeen)
                return true;
            argument = proveBehind(argument, isAsSeen);
        }
        passed.append(argument);
    }
    if (!closure || !thisValue || passed.contains(Reference()))
        return true;
    passed.append(written.argumentsOperands());
    walker.recordSpeculation();
    BlockID entering = walker.currentBlock();

    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(listed);

    BlockID continuationBlock;
    Reference mergedResult;
    if (asWritten)
        walker.continueIn(asWritten);
    if (canGuardInIteration) {
        if (asWritten && !walker.isUnreachable())
            walker.restartIteration();
    } else {
        Draft closed = written;
        walker.chooseConservativeFields(closed);
        Reference result = walker.build(WTF::move(closed));
        if (walker.unitReturnsAtOnce(written))
            walker.returnAtOnce(result);
        else {
            continuationBlock = walker.addBlock();
            mergedResult = walker.addParameter(continuationBlock, { }, Representation::JSValue);
            if (result)
                walker.jumpTo(continuationBlock, singleElementSpan(result));
        }
    }

    walker.continueIn(entering);
    walker.bind(call.id(), { });
    Draft forwarded(Opcode::Call, written.origin());
    walker.chooseConservativeFields(forwarded);
    forwarded.setCalleeOperand(closure);
    forwarded.setThisValueOperand(thisValue);
    forwarded.setArgumentsOperands(passed.span());
    forwarded.setCalleeName(written.calleeName());
    forwarded.setExpectedResult(written.expectedResult());
    forwarded.expected = written.expected;
    forwarded.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    forwarded.inputValueGiven = written.inputValueGiven;
    if (!callForwardedDirectly(walker, call, forwarded)) {
        if (Reference called = walker.build(WTF::move(forwarded)))
            walker.bind(call.id(), called);
    }
    if (!continuationBlock)
        return true;
    Reference forwardedResult = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (forwardedResult)
        walker.jumpTo(continuationBlock, singleElementSpan(forwardedResult));
    else
        walker.continueIn(continuationBlock);
    if (!walker.isUnreachable())
        walker.bind(call.id(), mergedResult);
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
