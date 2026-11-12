/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTWalkCoroutineCalls.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkKnown.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkScalarReplacement.h"
#include "AOTWalkBirths.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);

static constexpr OpcodeSet opcodesOfWalkCalls { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallList, Opcode::CallDirect, Opcode::ConstructList, Opcode::SuperConstructList };
static constexpr int64_t lengthOfShortListInWalkCalls = defaultLimitValue(LimitID::MaxReservedStackArguments, hostArchitecture());
static constexpr OpcodeSet appendsToListInWalkCalls { Opcode::AppendElement, Opcode::AppendSpread };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendSpread::array);

namespace {

struct CallInWalkCalls {
    FunctionID callee;
    Reference closure;
    Reference thisValue;
    TypeID typeOfThisToBeMade;
    Reference newTarget;
    std::span<const Reference> arguments;
    Reference restList;
    bool restListStandsBehindArguments { false };
    bool restListIsCopied { false };
    Reference listOfUnknownLength;
    Reference borrowedHiddenRest;
    Reference borrowedHiddenRestLength;
    bool listLengthIsBounded { false };
    std::span<const Reference> writtenInFrontOfList;
    BuiltinID calledBuiltin;
    bool isConstruction { false };
    bool isNeverInlined { false };
    bool isMadeByProtocol { false };
    bool calleeComesBack { false };
    bool provesNumbers { false };
    std::optional<Walker::JoinArm> armWhereGuardOfPrototypeHasEnded;
    FunctionID calledWhereNoCopyIsMade;
};

enum class OperandSourceInWalkCalls : uint8_t { Given, Capture, Undefined, Absent, ArgumentCount, ListOfOtherArguments, CopyOfRestList, SharedEmptyList };

struct OperandInWalkCalls {
    OperandSourceInWalkCalls source { OperandSourceInWalkCalls::Undefined };
    Reference given;
    uint32_t capture { 0 };
    TypeID typeToBeGiven;
};

} // anonymous namespace

FunctionID findOnlyCallee(Walker& walker, const BodyValue& call, Reference callee)
{
    return findOnlyCallee(walker, call, callee ? walker.typeOf(callee) : TypeID());
}

FunctionID findOnlyCallee(Walker& walker, const BodyValue& call, TypeID proven)
{
    if (!proven)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    if (!type.isObject() || !type.isJavaScriptValue())
        return { };
    FunctionID only = singleFunctionOf(types, type);
    if (!only)
        return { };
    const LinkTables& link = walker.facts().linked();
    if (link.function(only).isInQueueFromStart)
        return only;
    return only == link.calleeAt(walker.currentFunction(), call.id()) || link.mayBeMentionedBy(walker.unit(), only) ? only : FunctionID();
}

static bool bodyRunsWhenCalledInWalkCalls(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool admitsDirectEntryInWalkCalls(Walker& walker, const Function& callee, CodeUnit entered)
{
    if (!callee.hasBody() || !canBeCalledByValueOf(callee.role) || callee.has(FunctionCapabilities::RecordsCallee) || callee.results.size() != 1)
        return false;
    return walker.plan().conventions().exists(entered);
}

static bool isReturnedByTailCallInWalkCalls(const BodyIndex& index, ValueID call)
{
    std::optional<BodyUser> user = index.onlyUser(call);
    return user && user->kind == BodyUseKind::TerminatorOperand && index.terminator(user->block).kind() == TerminatorKind::ReturnCall;
}

static bool unitsReturnInOneFormInWalkCalls(Walker& walker, CodeUnit entered)
{
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit unit = walker.unit();
    if (conventions.numberOfResults(unit) != 1 || conventions.numberOfResults(entered) != 1)
        return true;
    return haveSameRegisterRepresentation(conventions.resultRepresentation(entered), conventions.resultRepresentation(unit));
}

static bool unitMayLeaveByCallInWalkCalls(Walker& walker, CodeUnit entered)
{
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit unit = walker.unit();
    if (walker.code().holdsFrameCells() || conventions.entryConventionOf(unit) != EntryConvention::Direct)
        return false;
    if (conventions.savesRegistersOfCallers(unit))
        return false;
    return conventions.numberOfResults(unit) == 1 && conventions.numberOfResults(entered) == 1;
}

bool unitMayLeaveByDirectCallOf(Walker& walker, FunctionID callee)
{
    return unitMayLeaveByCallInWalkCalls(walker, walker.plan().conventions().directEntryOf(callee));
}

bool canTailCallMethodTable(Walker& walker, uint32_t place, std::span<const FunctionID> methods)
{
    const Plan& plan = walker.plan();
    const CallingConventions& conventions = plan.conventions();
    CodeUnit unit = walker.unit();
    bool mayNeedReturn = plan.resultsPassNoReturn(unit.function) || plan.endsItsTailCalls(unit.function);
    if (!returnsResultOfFunction(unit.role) || (mayNeedReturn && plan.typeHeldOfResult(walker.program(), walker.facts().linked(), unit.function)))
        return false;
    const MethodSelector& selector = conventions.methodSelector(place);
    if (!unitMayLeaveByCallInWalkCalls(walker, conventions.tableLookupOf(selector)) || conventions.resultRepresentation(unit) != selector.result)
        return false;
    return std::ranges::all_of(methods, [&](FunctionID method) {
        return plan.endsItsTailCalls(method) || conventions.resultRepresentation(conventions.directEntryOf(method)) == selector.result;
    });
}

static bool findCapturesInSightInWalkCalls(const Code& code, FunctionID function, Reference closure, size_t numberOfCaptures, Vector<Reference, 8>& captures)
{
    if (!closure || !closure.isRecord())
        return false;
    ValueID id = closure.id();
    while (conversionsOfForm.contains(code.at(id).opcode))
        id = code.at(id).operand(0);
    const Operation& made = code.at(id);
    if (made.opcode != Opcode::NewClosure || made.function() != function || made.numberOfVariadicOperands() != numberOfCaptures)
        return false;
    for (ValueID capture : made.variadicOperands())
        captures.append(code.isConstant(capture) ? Reference::constant(capture) : Reference::value(capture));
    return true;
}

static TypeID passedTypeInWalkCalls(Walker& walker, const OperandInWalkCalls& operand)
{
    switch (operand.source) {
    case OperandSourceInWalkCalls::Given:
        return operand.given ? walker.typeOf(operand.given) : operand.typeToBeGiven;
    case OperandSourceInWalkCalls::Capture:
        return { };
    case OperandSourceInWalkCalls::Undefined:
        return TypeTable::undefined;
    case OperandSourceInWalkCalls::Absent:
        return TypeTable::absent;
    case OperandSourceInWalkCalls::ArgumentCount:
        return TypeTable::uint32;
    case OperandSourceInWalkCalls::ListOfOtherArguments:
    case OperandSourceInWalkCalls::CopyOfRestList:
    case OperandSourceInWalkCalls::SharedEmptyList:
        return walker.types().builtinClass(BuiltinClasses::Array);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasProvenWhatGuardTestsInWalkCalls(Walker& walker, FunctionID callee, unsigned entryParameter, Reference passedArgument)
{
    TypeID tested = walker.plan().conventions().typeBehindGuard(callee, entryParameter);
    if (!tested)
        return true;
    return passedArgument && passedArgument.isRecord() && walker.types().isSubset(walker.typeOf(passedArgument), tested) && walker.isShown(passedArgument, tested);
}

static bool bringOperandsIntoFormsOfCalleeInWalkCalls(Walker&, Origin, CodeUnit entered, Vector<Reference, 8>& operands);

static bool canProveNumber(Walker& walker, CodeUnit entered, unsigned entryParameter, const OperandInWalkCalls& operand)
{
    if (entered.role != CodeRole::Body || operand.source != OperandSourceInWalkCalls::Given || !operand.given || !operand.given.isValue())
        return false;
    TypeArena& types = walker.types();
    TypeID promised = walker.facts().typeOfEntryParameter(entered.function, entryParameter).proven;
    TypeID given = walker.typeOf(operand.given);
    if (!promised || !types.at(promised).isJavaScriptValue() || !types.at(given).hasOnlyPrimitives(Primitive::Number) || !types.overlap(given, promised))
        return false;
    return isLegalRepresentation(types.at(types.testablePartOf(promised)), walker.plan().conventions().parameterRepresentation(walker.program(), entered, entryParameter));
}

static bool bodyTakesOperandInWalkCalls(Walker& walker, CodeUnit entered, unsigned entryParameter, const OperandInWalkCalls& operand, bool provesNumbers = false)
{
    if (operand.source == OperandSourceInWalkCalls::Capture)
        return true;
    TypeID argumentType = passedTypeInWalkCalls(walker, operand);
    if (!argumentType)
        return false;
    TypeArena& types = walker.types();
    Type type = types.at(argumentType);
    if (type.mayBeUnspecified())
        return false;
    TypingContext typing { walker.program(), types, entered.role == CodeRole::Body ? walker.positions() : PositionTypes::knowingNothing(), TypeLevel::Proven };
    if (!walker.plan().promotedEnvironments().isRemoved(entered.function, entryParameter)) {
        TypeID required = declaredEntryTypeOf(typing, entered.function, entryParameter);
        if (required && !types.isSubset(argumentType, required))
            return false;
    }
    if (entered.role == CodeRole::Body && operand.source == OperandSourceInWalkCalls::Given && !hasProvenWhatGuardTestsInWalkCalls(walker, entered.function, entryParameter, operand.given))
        return false;
    Representation taken = walker.plan().conventions().parameterRepresentation(walker.program(), entered, entryParameter);
    return !hasRegister(taken) || isLegalRepresentation(type, taken) || (provesNumbers && canProveNumber(walker, entered, entryParameter, operand));
}

static bool planOperandsInWalkCalls(Walker& walker, const CallInWalkCalls& request, const Function& callee, CodeUnit entered, Vector<OperandInWalkCalls, 8>& operands)
{
    using Source = OperandSourceInWalkCalls;
    auto givenOrUndefined = [](Reference given) {
        return given ? OperandInWalkCalls { Source::Given, given, 0 } : OperandInWalkCalls { };
    };

    Vector<Reference, 8> capturesInSight;
    bool capturesAreInSight = !callee.captures.isEmpty() && findCapturesInSightInWalkCalls(walker.code(), request.callee, request.closure, callee.captures.size(), capturesInSight);

    unsigned numberOfPositionalParameters = callee.numberOfPositionalParameters();
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    uint32_t nextCapture = 0;
    unsigned nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        OperandInWalkCalls operand;
        switch (callee.entryParameterKind(index)) {
        case EntryParameter::Capture:
            if (walker.plan().promotedEnvironments().isRemoved(request.callee, nextCapture)) {
                operand = { Source::Capture, wordInPlaceOfRemovedCapture(walker, request.callee, nextCapture), nextCapture };
                ++nextCapture;
                break;
            }
            if (!request.closure)
                return false;
            operand = { Source::Capture, capturesAreInSight ? capturesInSight[nextCapture] : Reference(), nextCapture };
            if (!operand.given)
                operand.given = findCaptureAtHand(walker, request.callee, nextCapture, request.closure);
            ++nextCapture;
            break;
        case EntryParameter::This:
            operand = request.typeOfThisToBeMade ? OperandInWalkCalls { Source::Given, { }, 0, request.typeOfThisToBeMade } : givenOrUndefined(request.thisValue);
            break;
        case EntryParameter::Callee:
            if (!request.closure)
                return false;
            operand = { Source::Given, request.closure, 0 };
            break;
        case EntryParameter::NewTarget:
            operand = givenOrUndefined(request.newTarget);
            break;
        case EntryParameter::ArgumentCount:
            if (request.borrowedHiddenRest) {
                operand = { Source::Given, request.borrowedHiddenRestLength, 0 };
                break;
            }
            if (request.restList)
                return false;
            operand.source = Source::ArgumentCount;
            break;
        case EntryParameter::Parameter:
            if (nextParameter >= numberOfPositionalParameters)
                operand = request.borrowedHiddenRest ? OperandInWalkCalls { Source::Given, request.borrowedHiddenRest, 0 } : request.restList ? OperandInWalkCalls { request.restListIsCopied ? Source::CopyOfRestList : Source::Given, request.restList, 0 } : OperandInWalkCalls { Source::ListOfOtherArguments, { }, 0 };
            else if (request.restList && !request.restListStandsBehindArguments)
                return false;
            else if (nextParameter < request.arguments.size())
                operand = { Source::Given, request.arguments[nextParameter], 0 };
            else if (TypeID writtenType = callee.parameters[nextParameter].written(); writtenType && (walker.types().at(writtenType).primitives & Primitive::Absent))
                operand.source = Source::Absent;
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            if (request.borrowedHiddenRest) {
                operand = { Source::Given, request.borrowedHiddenRest, 0 };
                break;
            }
            if (request.restList)
                return false;
            operand.source = request.arguments.size() > numberOfPositionalParameters ? Source::ListOfOtherArguments : Source::SharedEmptyList;
            break;
        }
        if (operand.source == Source::ListOfOtherArguments && request.arguments.size() > numberOfPositionalParameters + maxComputedElementsOfArrayLiteral)
            return false;
        if (!bodyTakesOperandInWalkCalls(walker, entered, index, operand, request.provesNumbers))
            return false;
        operands.append(operand);
    }
    return true;
}

static Reference buildListOfArgumentsAndListInWalkCalls(Walker& walker, Origin origin, std::span<const Reference> arguments, Reference listBehindThem)
{
    Draft literal(Opcode::NewArray, origin);
    walker.chooseConservativeFields(literal);
    literal.standsForNoValueOfInput = true;
    literal.setContents(TypeTable::anything);
    literal.setVariadicOperands(arguments);
    Reference list = walker.build(WTF::move(literal));
    if (!list)
        return { };
    noteFreshList(walker, list);
    Draft append(Opcode::AppendElements, origin);
    walker.chooseConservativeFields(append);
    append.setArrayOperand(list);
    append.setSourceOperand(listBehindThem);
    walker.build(WTF::move(append));
    return walker.isUnreachable() ? Reference() : list;
}

static Reference buildOperandInWalkCalls(Walker& walker, const CallInWalkCalls& request, const Function& callee, Origin origin, const OperandInWalkCalls& operand)
{
    switch (operand.source) {
    case OperandSourceInWalkCalls::Given:
        return operand.given;
    case OperandSourceInWalkCalls::Capture: {
        if (operand.given)
            return operand.given;
        if (Reference scope = findScopeOfClosureReadOffObjectInSight(walker, request.callee, request.closure))
            return scope;
        Draft capture(Opcode::GetCapture, origin);
        walker.chooseConservativeFields(capture);
        capture.setFunction(request.callee);
        capture.setIndex(operand.capture);
        capture.setClosureOperand(request.closure);
        return walker.build(WTF::move(capture));
    }
    case OperandSourceInWalkCalls::Undefined:
        return walker.constant(Draft(Opcode::Undefined, origin));
    case OperandSourceInWalkCalls::Absent:
        return walker.constant(Draft(Opcode::Absent, origin));
    case OperandSourceInWalkCalls::ArgumentCount: {
        Draft count(Opcode::Integer, origin);
        count.setInteger(static_cast<int64_t>(request.arguments.size()));
        return walker.constant(WTF::move(count));
    }
    case OperandSourceInWalkCalls::ListOfOtherArguments: {
        Draft list(Opcode::NewArray, origin);
        walker.chooseConservativeFields(list);
        list.standsForNoValueOfInput = true;
        list.setContents(TypeTable::anything);
        list.setVariadicOperands(request.arguments.subspan(std::min<size_t>(callee.numberOfPositionalParameters(), request.arguments.size())));
        Reference built = walker.build(WTF::move(list));
        noteFreshList(walker, built);
        return built;
    }
    case OperandSourceInWalkCalls::CopyOfRestList:
        return buildListOfArgumentsAndListInWalkCalls(walker, origin, { }, operand.given);
    case OperandSourceInWalkCalls::SharedEmptyList: {
        Draft list(Opcode::EmptyArgumentList, origin);
        walker.chooseConservativeFields(list);
        list.setValueType(walker.types().builtinClass(BuiltinClasses::Array));
        return walker.build(WTF::move(list));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class UseOfRestParameter : bool { PassedToCall, PassedToCallOrSpread };

static bool isReadOnlyRestArray(Walker& walker, FunctionID id, UseOfRestParameter accepted)
{
    static constexpr OpcodeSet arrayCallOpcodes { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
    const Function& function = walker.program().at(id);
    if (function.has(FunctionFlags::HasHiddenRest))
        return true;
    const BodyIndex* body = function.has(FunctionFlags::HasRestParameter) ? walker.facts().bodies().find(id) : nullptr;
    if (!body)
        return false;
    ValueID rest = body->block(entryBlock).parameter(function.firstParameterIndex() + function.numberOfPositionalParameters());
    ValueID user = body->hasOneUse(rest) ? body->onlyUsingValue(rest) : ValueID();
    if (!user)
        return false;
    if (body->opcodeOf(user) == Opcode::AppendSpread)
        return accepted == UseOfRestParameter::PassedToCallOrSpread && body->value(user).as<Opcode::AppendSpread>().iterableOperand() == rest;
    return arrayCallOpcodes.contains(body->opcodeOf(user)) && body->value(user).listOperand() == rest;
}

static std::optional<uint32_t> restArrayFirstArgument(Walker& walker, Reference array)
{
    if (!array || !array.isValue())
        return std::nullopt;
    const Code& code = walker.code();
    const Operation& allocation = code.at(underlyingCellOf(code, array.id()));
    if (allocation.opcode != Opcode::CreateRest || !isReadOnlyRestArray(walker, walker.unit().function, UseOfRestParameter::PassedToCallOrSpread))
        return std::nullopt;
    return allocation.firstArgument();
}

bool holdsArgumentsOfUnit(Walker& walker, Reference array)
{
    if (restArrayFirstArgument(walker, array))
        return true;
    std::optional<uint32_t> index = walker.entryParameterOfUnitBoundTo(array);
    if (!index && array.isValue() && !walker.code().isConstant(array.id()))
        index = walker.entryParameterOfUnitBoundTo(Reference::value(underlyingCellOf(walker.code(), array.id())));
    if (!index)
        return false;
    const Function& function = walker.program().at(walker.unit().function);
    EntryParameter kind = function.entryParameterKind(*index);
    return kind == EntryParameter::HiddenRest
        || (kind == EntryParameter::Parameter && function.has(FunctionFlags::HasRestParameter) && *index - function.firstParameterIndex() == function.numberOfPositionalParameters());
}

static Reference sinkRestArrayAllocation(Walker& walker, Reference array)
{
    if (!restArrayFirstArgument(walker, array))
        return array;
    Reference again = walker.rematerializeHiddenRestArray();
    if (!again)
        return array;
    walker.count(StatisticID::RestArrayAllocationsSunk);
    return again;
}

static Reference buildSpreadArrayLength(Walker& walker, Reference array, Origin origin)
{
    if (std::optional<uint32_t> firstArgument = restArrayFirstArgument(walker, array)) {
        Draft count(Opcode::GetRestLength, origin);
        count.setFirstArgument(*firstArgument);
        walker.chooseConservativeFields(count);
        return walker.build(WTF::move(count));
    }
    Draft length(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setArrayOperand(array);
    return walker.build(WTF::move(length));
}

static bool buildCallAsWrittenForLongListInWalkCalls(Walker& walker, const Draft& written, const CallInWalkCalls& request, Origin originInFrontOfCall, BlockID& continuationBlock, Reference& mergedResult)
{
    Draft lengthOfShortList(Opcode::Integer, originInFrontOfCall);
    lengthOfShortList.setInteger(lengthOfShortListInWalkCalls);

    Draft test(Opcode::LessEq, originInFrontOfCall);
    walker.chooseConservativeFields(test);
    test.setLeftOperand(buildSpreadArrayLength(walker, request.listOfUnknownLength, originInFrontOfCall));
    test.setRightOperand(walker.constant(WTF::move(lengthOfShortList)));
    if (!test.leftOperand() || !test.rightOperand())
        return false;
    if (walker.has(Mechanism::TypedOperations))
        test.setForm(operatorFormFor(walker, test));
    Reference isShort = walker.build(WTF::move(test));
    if (!isShort)
        return false;

    BlockID shortList = walker.addBlock();
    BlockID longList = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isShort, shortList, longList);

    walker.continueIn(longList);
    Draft asWritten = written;
    if (request.restListStandsBehindArguments)
        asWritten.setListOperand(buildListOfArgumentsAndListInWalkCalls(walker, originInFrontOfCall, request.arguments, request.restList));
    else if (!request.writtenInFrontOfList.empty())
        asWritten.setListOperand(buildListOfArgumentsAndListInWalkCalls(walker, originInFrontOfCall, request.writtenInFrontOfList, request.listOfUnknownLength));
    else
        asWritten.setListOperand(sinkRestArrayAllocation(walker, asWritten.listOperand()));
    walker.chooseConservativeFields(asWritten);
    Reference result = walker.build(WTF::move(asWritten));
    walker.leaveAnythingAt(request.callee);
    if (walker.unitReturnsAtOnce(written))
        walker.returnAtOnce(result);
    else {
        continuationBlock = walker.addBlock();
        mergedResult = walker.addParameter(continuationBlock, { }, Representation::JSValue);
        if (result)
            walker.jumpTo(continuationBlock, singleElementSpan(result));
    }
    walker.continueIn(shortList);
    return !walker.isUnreachable();
}


static bool argumentsPassGuards(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    if (!walker.mayEnterAfterGuardOf(callee))
        return false;
    const CallingConventions& conventions = walker.plan().conventions();
    const Function& function = walker.program().at(callee);
    CodeUnit entered = conventions.entryBehindGuardOf(callee);
    if (conventions.takesScalars(entered) || function.kind == FunctionKind::Initializer || !admitsDirectEntryInWalkCalls(walker, function, entered) || entryArguments.size() != function.numberOfEntryParameters())
        return false;
    for (unsigned index = 0; index < entryArguments.size(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::Capture)
            continue;
        if (!entryArguments[index] || !bodyTakesOperandInWalkCalls(walker, entered, index, OperandInWalkCalls { OperandSourceInWalkCalls::Given, entryArguments[index], 0, { } }))
            return false;
    }
    return true;
}

Reference callInPlaceOfCopy(Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> entryArguments, bool isConstruction, bool entersAsCallWould)
{
    Draft direct(Opcode::CallDirect, site.origin());
    direct.expected = site.expected;
    direct.resultIsReturnedAtOnce = site.resultIsReturnedAtOnce;
    direct.inputValueGiven = site.inputValueGiven;
    direct.setFunction(callee);
    bool hasSlowVersion = walker.plan().conventions().hasParameterGuard(callee);
    bool entersBody = !entersAsCallWould && (!hasSlowVersion || (!isConstruction && argumentsPassGuards(walker, callee, entryArguments)));
    direct.setRole(entersAsCallWould ? walker.plan().conventions().directEntryOf(callee).role : entersBody ? CodeRole::Body : CodeRole::SlowBody);
    if (site.opcode == Opcode::Call)
        direct.setCalledBuiltin(site.calledBuiltin());
    direct.setIsConstruction(isConstruction);
    direct.setIsNeverInlined(true);
    direct.setIsRemovableWhenUnused(callOnlyMakesGenerator(walker, callee));
    direct.setVariadicOperands(entryArguments);
    Effects effects = effectsOfDirectCall(walker, callee, entryArguments);
    if (entersAsCallWould || entersBody)
        walker.leaveArrivals(callee, entryArguments);
    return walker.buildCallThatMayBorrowScopes(WTF::move(direct), &effects);
}

bool directEntryTakesArguments(Walker&, FunctionID callee, std::span<const Reference> entryArguments);

bool unitTakesWhatCopyIsHanded(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    return walker.plan().conventions().hasParameterGuard(callee) || directEntryTakesArguments(walker, callee, entryArguments);
}

bool directEntryTakesArguments(Walker& walker, FunctionID callee, std::span<const Reference> entryArguments)
{
    const CallingConventions& conventions = walker.plan().conventions();
    const Function& function = walker.program().at(callee);
    CodeUnit entered = conventions.directEntryOf(callee);
    if (function.kind == FunctionKind::Initializer)
        return false;
    if (!admitsDirectEntryInWalkCalls(walker, function, entered) || entryArguments.size() != function.numberOfEntryParameters())
        return false;
    for (unsigned index = 0; index < entryArguments.size(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::Capture)
            continue;
        if (!entryArguments[index] || !bodyTakesOperandInWalkCalls(walker, entered, index, OperandInWalkCalls { OperandSourceInWalkCalls::Given, entryArguments[index], 0, { } }))
            return false;
    }
    return true;
}

static Reference proveResultThatPassesNoReturnInWalkCalls(Walker& walker, const BodyValue& call, FunctionID callee, Reference result, Origin origin)
{
    TypeArena& types = walker.types();
    if (!walker.plan().resultsPassNoReturn(callee) || walker.program().at(callee).isCoroutine() || !result.isValue() || !walker.currentBodyIndex().useCount(call.id()))
        return result;
    TypeID proven = walker.facts().linked().function(callee).result;
    TypeID own = walker.typeOf(result);
    if (!proven || !types.at(proven).isJavaScriptValue() || !types.at(own).isJavaScriptValue() || types.isSubset(own, proven))
        return result;
    TypeID tested = walker.typeThatUnitMayTestFor(types.testablePartOf(proven));
    if (types.isSubset(own, tested))
        return result;
    return walker.proveOrTrap(result, tested, ProofSource::ResultOfCall, origin);
}

static void countDirectEntry(Walker& walker, FunctionID callee, CodeRole entered)
{
    if (!walker.plan().conventions().hasParameterGuard(callee))
        return;
    if (entered == CodeRole::Body)
        walker.count(StatisticID::ParameterGuardsSkipped);
    else if (entered == CodeRole::ParameterGuard)
        walker.count(StatisticID::ParameterGuardsEntered);
}

static bool calleeNeverReturns(Walker& walker, FunctionID callee)
{
    const LinkTables& link = walker.facts().linked();
    return callee.index() < link.functions.size() && link.function(callee).isReached && link.function(callee).neverReturns && bodyRunsWhenCalledInWalkCalls(walker.program().at(callee).kind);
}

static bool narrowIntegersPassedToItself(Walker& walker, const Draft& written, FunctionID callee, Vector<Reference, 8>& operands, Origin origin, BlockID& continuationBlock, Reference& mergedResult)
{
    TypeArena& types = walker.types();
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit body = conventions.entryBehindGuardOf(callee);
    if (callee != walker.unit().function || walker.unit().role != CodeRole::Body || !walker.mayEnterAfterGuardOf(callee))
        return false;
    TypeID int32 = types.integer(Range::int32());
    Vector<uint32_t, 4> integers;
    for (uint32_t index = 0; index < operands.size(); ++index) {
        if (hasProvenWhatGuardTestsInWalkCalls(walker, callee, index, operands[index]))
            continue;
        TypeID type = operands[index] && operands[index].isValue() ? walker.typeOf(operands[index]) : TypeID();
        if (conventions.typeBehindGuard(callee, index) != int32 || !type || !types.at(type).isInteger() || !types.overlap(type, int32))
            return false;
        integers.append(index);
    }
    if (integers.isEmpty())
        return false;

    Vector<Reference, 8> narrowed = operands;
    BlockID failing = walker.addBlock(BlockFrequency::Rare);
    for (uint32_t index : integers) {
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(operands[index]);
        test.setTestedType(int32);
        Reference passes = walker.build(WTF::move(test));
        if (!passes)
            break;
        BlockID passing = walker.addBlock();
        walker.branch(passes, passing, failing);
        walker.continueIn(passing);
        Draft draft(Opcode::Narrowed, origin);
        walker.chooseConservativeFields(draft);
        draft.setValueOperand(operands[index]);
        draft.setTestOperand(passes);
        draft.setOperandIndex(OperandIndex::TypeTest::value);
        draft.setIsOnTakenEdge(true);
        narrowed[index] = walker.build(WTF::move(draft));
        if (!narrowed[index])
            break;
    }
    BlockID enters = walker.addBlock();
    walker.jumpTo(enters);

    walker.continueIn(failing);
    if (!walker.isUnreachable()) {
        Draft direct(Opcode::CallDirect, written.origin());
        walker.chooseConservativeFields(direct);
        direct.expected = written.expected;
        direct.setFunction(callee);
        direct.setRole(CodeRole::ParameterGuard);
        direct.setIsNeverInlined(true);
        direct.setVariadicOperands(operands.span());
        Effects effects = effectsOfDirectCall(walker, callee, operands.span());
        Reference result = walker.build(WTF::move(direct), &effects);
        walker.leaveArrivals(callee, operands.span());
        if (walker.unitReturnsAtOnce(written))
            walker.returnAtOnce(result);
        else {
            continuationBlock = walker.addBlock();
            mergedResult = walker.addParameter(continuationBlock, { }, conventions.resultRepresentation(body));
            if (result)
                walker.jumpTo(continuationBlock, singleElementSpan(result));
        }
    }
    walker.continueIn(enters);
    operands = WTF::move(narrowed);
    return true;
}

static bool buildDirectCallInWalkCalls(Walker& walker, const BodyValue& call, const Draft& written, const CallInWalkCalls& request)
{
    const Function& callee = walker.program().at(request.callee);
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit entered = walker.mayEnterAfterGuardOf(request.callee) ? conventions.entryBehindGuardOf(request.callee) : conventions.directEntryOf(request.callee);
    if (!admitsDirectEntryInWalkCalls(walker, callee, entered))
        return false;
    if (!request.calleeComesBack && !calleeOfTailCallComesBack(walker, request.callee, request.arguments) && isReturnedByTailCallInWalkCalls(walker.currentBodyIndex(), call.id()) && unitsReturnInOneFormInWalkCalls(walker, entered) && !unitMayLeaveByCallInWalkCalls(walker, entered))
        return false;
    Vector<OperandInWalkCalls, 8> planned;
    CallInWalkCalls planning = request;
    planning.provesNumbers = request.callee == walker.unit().function && !conventions.hasParameterGuard(request.callee);
    if (!planOperandsInWalkCalls(walker, planning, callee, entered, planned)) {
        if (!conventions.hasParameterGuard(request.callee))
            return false;
        entered = conventions.directEntryOf(request.callee);
        planned.shrink(0);
        if (!planOperandsInWalkCalls(walker, request, callee, entered, planned))
            return false;
    }

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    BlockID continuationBlock;
    Reference mergedResult;
    bool asksLength = request.listOfUnknownLength && !request.listLengthIsBounded;
    if (asksLength && !buildCallAsWrittenForLongListInWalkCalls(walker, written, request, originInFrontOfCall, continuationBlock, mergedResult))
        return true;

    Vector<Reference, 8> operands;
    operands.reserveInitialCapacity(planned.size());
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, originInFrontOfCall, operand);
        if (!built)
            return true;
        operands.append(built);
    }
    if (planning.provesNumbers && !bringOperandsIntoFormsOfCalleeInWalkCalls(walker, originInFrontOfCall, entered, operands))
        return true;
    if (entered.role == CodeRole::ParameterGuard && !asksLength && !request.isConstruction && narrowIntegersPassedToItself(walker, written, request.callee, operands, originInFrontOfCall, continuationBlock, mergedResult)) {
        if (walker.isUnreachable()) {
            if (continuationBlock) {
                walker.continueIn(continuationBlock);
                walker.bind(call.id(), mergedResult);
            }
            return true;
        }
        entered = conventions.entryBehindGuardOf(request.callee);
    }

    Draft direct(Opcode::CallDirect, written.origin());
    walker.chooseConservativeFields(direct);
    direct.expected = written.expected;
    direct.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    direct.inputValueGiven = written.inputValueGiven;
    direct.setFunction(request.callee);
    direct.setRole(entered.role);
    direct.setCalledBuiltin(request.calledBuiltin);
    direct.setIsConstruction(request.isConstruction);
    direct.setIsNeverInlined(request.isNeverInlined);
    direct.setIsRemovableWhenUnused(callOnlyMakesGenerator(walker, request.callee));
    direct.setVariadicOperands(operands.span());
    Effects effects = effectsOfDirectCall(walker, request.callee, operands.span());
    bool neverReturns = calleeNeverReturns(walker, request.callee) && !request.isConstruction;
    walker.buildsCallWithoutReturn(neverReturns && !continuationBlock && !walker.standsUnderHandler());
    Reference result = walker.buildCallThatMayBorrowScopes(WTF::move(direct), &effects);
    walker.buildsCallWithoutReturn(false);
    if (result) {
        countDirectEntry(walker, request.callee, entered.role);
        walker.leaveArrivals(request.callee, operands.span());
    }
    if (result && !walker.unitReturnsAtOnce(written))
        result = proveResultThatPassesNoReturnInWalkCalls(walker, call, request.callee, result, originInFrontOfCall);
    if (result && callee.has(FunctionFlags::IsAsync) && !callee.has(FunctionFlags::IsGenerator) && !request.isConstruction)
        markObjectPristine(walker, result);
    if (result && neverReturns) {
        walker.endAsUnreachable(originInFrontOfCall);
        result = { };
    }
    if (continuationBlock) {
        if (result)
            walker.jumpTo(continuationBlock, singleElementSpan(result));
        else
            walker.continueIn(continuationBlock);
        result = walker.isUnreachable() ? Reference() : mergedResult;
    } else if (walker.runsInIterations() && result && result.isValue() && !request.isConstruction && !walker.isUnreachable() && walker.has(Mechanism::EscapeAnalysis) && hasKind(walker.types(), walker.typeOf(result), OperandKind::Array) && returnsUnsharedList(walker, request.callee)) {
        Reference list = Reference::value(underlyingCellOf(walker.code(), result.id()));
        if (walker.code().at(list.id()).opcode == Opcode::CallDirect) {
            walker.state().noteFresh(list);
            markObjectPristine(walker, list);
            walker.noteReleasableArrayMade();
        }
    }
    if (result)
        walker.bind(call.id(), result);
    return true;
}

bool canCallCompilerHelper(Walker& walker, FunctionID helper)
{
    const LinkTables& link = walker.facts().linked();
    if (!helper || helper.index() >= link.functions.size() || !walker.has(Mechanism::DirectCalls) || !link.function(helper).isReached || link.function(helper).everyCallerIsKnown)
        return false;
    const Function& callee = walker.program().at(helper);
    if (!bodyRunsWhenCalledInWalkCalls(callee.kind) || !callee.captures.isEmpty() || callee.has(FunctionFlags::TakesCallee))
        return false;
    return admitsDirectEntryInWalkCalls(walker, callee, walker.plan().conventions().directEntryOf(helper));
}

Reference callCompilerHelper(Walker& walker, FunctionID helper, std::span<const Reference> arguments, Origin origin)
{
    const Function& callee = walker.program().at(helper);
    CodeUnit entered = walker.plan().conventions().directEntryOf(helper);
    CallInWalkCalls request;
    request.callee = helper;
    request.arguments = arguments;
    request.calleeComesBack = true;
    Vector<OperandInWalkCalls, 8> planned;
    if (!planOperandsInWalkCalls(walker, request, callee, entered, planned)) {
        walker.fail("a helper of the compiler does not take what the walk hands it"_s);
        return { };
    }

    origin.isTailCall = false;
    Vector<Reference, 8> operands;
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, origin, operand);
        if (!built)
            return { };
        operands.append(built);
    }
    Draft direct(Opcode::CallDirect, origin);
    walker.chooseConservativeFields(direct);
    direct.setFunction(helper);
    direct.setRole(entered.role);
    direct.setIsNeverInlined(true);
    direct.setVariadicOperands(operands.span());
    Effects effects = effectsOfDirectCall(walker, helper, operands.span());
    Reference result = walker.build(WTF::move(direct), &effects);
    if (result)
        walker.leaveArrivals(helper, operands.span());
    return result;
}

static bool isConstructedWithoutStubInWalkCalls(const Walker& walker, FunctionID id)
{
    const Function& constructor = walker.program().at(id);
    if (isClassConstructorOf(constructor.kind))
        return !constructor.has(FunctionFlags::HasThis);
    return makesThisOfConstruction(walker, id) || walker.plan().conventions().exists(CodeUnit { id, CodeRole::AllocatingConstructEntry });
}

struct BindingOfCalleeInWalkCalls {
    ~BindingOfCalleeInWalkCalls()
    {
        if (input)
            walker.bind(input, asBoundBefore);
    }
    Walker& walker;
    ValueID input;
    Reference asBoundBefore;
};

static constexpr Effects effectsOfCallOfWhatIsNoObjectInWalkCalls()
{
    return { .reads = LocationMasks::top(), .writes = LocationMasks::top().restrictedTo(unobservableWriteHeaps), .mayThrow = true, .mayCollect = true, .mayCallUnknownCode = false, .maySuspend = false, .mayBeTerminated = false };
}

static bool turnOfLoopMakesCalleeAnewInWalkCalls(const BodyIndex& index, const BodyValue& call)
{
    static constexpr OpcodeSet readsOfVariables { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture };
    LoopID loop = index.loopOf(index.blockOf(call.id()));
    if (!loop)
        return false;
    for (ValueID value = index.stripCasts(call.calleeOperand()); value && index.isInLoop(index.blockOf(value), loop);) {
        if (index.isParameter(value))
            return true;
        BodyValue made = index.value(value);
        if (inputPassThroughOpcodes.contains(made.opcode())) {
            value = made.operand(0);
            continue;
        }
        if (readsOfVariables.contains(made.opcode()))
            return false;
        if (made.opcode() != Opcode::PropGet || made.isFreeName())
            return true;
        value = index.stripCasts(made.objectOperand());
    }
    return false;
}

bool computesWithNumbersAlone(const Program&, BuiltinID);

static FunctionID onlyFunctionBesideRecords(Walker& walker, const Type& type)
{
    if (!type.objects || type.anyObject || type.anyFunction || type.unnamedClasses || type.hasOnlyClosures())
        return { };
    const ObjectTable& objects = walker.types().objects();
    FunctionID only;
    bool holdsNothingElse = TypeReader(walker.types()).at(type.objects).allOf([&](ObjectID object) {
        if (FunctionID function = objects.closureFunction(object)) {
            bool isFirst = !only;
            only = function;
            return isFirst;
        }
        if (objects.kindOf(object) != ObjectKind::Instance)
            return false;
        const Layout& layout = walker.program().at(objects.layoutOf(object));
        return layout.kind == LayoutKind::Record && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
    });
    return holdsNothingElse ? only : FunctionID();
}

TypeID closuresOfOptionalCallee(Walker& walker, ValueID written, const BodyValue& call)
{
    bool isConstruction = call.opcode() == Opcode::Construct;
    Reference callee = walker.referenceOf(written);
    TypeID proven = callee && callee.isValue() ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return { };
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    if (!type.isJavaScriptValue() || !type.hasObjects() || type.hasUnboundedUnknown())
        return { };
    FunctionID onlyBesideRecords = onlyFunctionBesideRecords(walker, type);
    if (type.isObject() && !onlyBesideRecords)
        return { };
    TypeID closures = onlyBesideRecords ? types.forFunction(onlyBesideRecords) : types.withOnlyObjects(proven);
    FunctionID only = singleFunctionOf(types, types.at(closures));
    auto isCalledByRowsOfBuiltin = [&] {
        BuiltinID builtin = walker.has(Mechanism::BuiltinCalls) ? singleBuiltinOf(types, types.at(closures)) : BuiltinID();
        if (!builtin)
            return false;
        if (call.opcode() == Opcode::Call && computesWithNumbersAlone(walker.program(), builtin))
            return true;
        auto typeBehindTest = [&](ValueID input) {
            if (input == written)
                return closures;
            Reference operand = walker.referenceOf(input);
            return operand ? walker.typeOf(operand) : TypeID();
        };
        TypesOfInputs inputsBehindTest(typeBehindTest);
        return wouldCallBuiltin(walker, call, inputsBehindTest) != BuiltinCallAskedAbout::None;
    };
    if (isConstruction) {
        if (only ? !isConstructedWithoutStubInWalkCalls(walker, only) : !isCalledByRowsOfBuiltin())
            return { };
    } else if (only ? !bodyRunsWhenCalledInWalkCalls(walker.program().at(only).kind) : !isCalledByRowsOfBuiltin())
        return { };
    return walker.withoutFunctionsThatMayNotBeMentioned(closures) == closures ? closures : TypeID();
}

static bool testCalleeThatMayBeNoObjectInWalkCalls(Walker& walker, const BodyValue& call)
{
    ValueID written = call.calleeOperand();
    TypeID closures = closuresOfOptionalCallee(walker, written, call);
    if (!closures)
        return false;
    Reference callee = walker.referenceOf(written);

    Effects throwsAlone = effectsOfCallOfWhatIsNoObjectInWalkCalls();
    const Effects* ofThrow = (walker.isInSpecializedIteration() && !walker.canRestartIteration()) || turnOfLoopMakesCalleeAnewInWalkCalls(walker.currentBodyIndex(), call) ? &throwsAlone : nullptr;
    if (ofThrow)
        walker.count(StatisticID::PrimitiveCalleesTreatedAsThrows);
    Draft asWritten = walker.draftOf(call);
    if (!walker.unitReturnsAtOnce(asWritten)) {
        Draft throws = walker.draftOf(call);
        walker.chooseConservativeFields(throws);
        if (walker.splitNullishOffWhatContainerHasJustGiven(callee, closures, WTF::move(throws), ofThrow))
            return true;
    }
    Origin origin = asWritten.origin();
    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(callee);
    test.setTestedType(closures);
    Reference isClosure = walker.build(WTF::move(test));
    if (!isClosure)
        return true;
    bool isCheckedBeforeLoop = walker.isInSpecializedIteration() && walker.tryBranchInPreheader(isClosure, true, Walker::IterationPurity::KnownEffectsOnly, Walker::GuardedIterations::SomeWithUnguardedVersion);
    if (isCheckedBeforeLoop)
        walker.recordSpeculation();
    else {
        BlockID calls = walker.addBlock();
        BlockID throws = walker.addBlock(BlockFrequency::Rare);
        walker.branch(isClosure, calls, throws);

        walker.continueIn(throws);
        bool returnsAtOnce = walker.unitReturnsAtOnce(asWritten);
        walker.chooseConservativeFields(asWritten);
        if (asWritten.opcode == Opcode::Call)
            asWritten.setArgumentsOperands(std::span<const Reference>());
        Reference neverGiven = walker.build(WTF::move(asWritten), ofThrow);
        if (returnsAtOnce)
            walker.returnAtOnce(neverGiven);
        else
            walker.endAsUnreachable(origin);
        walker.continueIn(calls);
    }

    Walker::SpecializedPreheaderScope preheaderScope(walker, isCheckedBeforeLoop, Walker::IterationPurity::KnownEffectsOnly);
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(callee);
    narrowed.setTestOperand(isClosure);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    if (Reference closure = walker.build(WTF::move(narrowed)))
        walker.bind(written, closure);
    return true;
}

static bool walkCallInWalkCalls(Walker& walker, const BodyValue& call)
{
    CallInWalkCalls request;
    request.closure = walker.referenceOf(call.as<Opcode::Call>().calleeOperand());
    if (walker.hasUninformativeType(request.closure))
        return false;
    request.callee = findOnlyCallee(walker, call, request.closure);

    BindingOfCalleeInWalkCalls bindingOfCallee { walker, { }, walker.referenceBoundTo(call.as<Opcode::Call>().calleeOperand()) };
    if (!request.callee && testCalleeThatMayBeNoObjectInWalkCalls(walker, call)) {
        bindingOfCallee.input = call.as<Opcode::Call>().calleeOperand();
        if (walker.isUnreachable() || (walker.has(Mechanism::BuiltinCalls) && callBuiltin(walker, call)) || (walker.has(Mechanism::Inlining) && inlineCallee(walker, call)))
            return true;
        if (walker.unit().role == CodeRole::CoroutineBody && walker.walksOwnBodyOfUnit() && buildAwaitedCall(walker, call))
            return true;
        request.closure = walker.referenceOf(call.as<Opcode::Call>().calleeOperand());
        request.callee = findOnlyCallee(walker, call, request.closure);
    }
    if (!request.callee || !bodyRunsWhenCalledInWalkCalls(walker.program().at(request.callee).kind))
        return false;

    Draft written = walker.draftOf(call);
    request.thisValue = written.thisValueOperand();
    request.arguments = written.argumentsOperands();
    request.calledBuiltin = written.calledBuiltin();
    request.isNeverInlined = written.isNeverInlined();
    return buildDirectCallInWalkCalls(walker, call, written, request);
}

bool makesThisOfConstruction(const Walker& walker, FunctionID id)
{
    const Function& constructor = walker.program().at(id);
    if (!constructor.takesThisFromConstruction() || constructor.isCoroutine() || constructor.results.size() != 1 || !constructor.constructedClass)
        return false;
    const Program& program = walker.program();
    const SlotPlan& slots = walker.plan().slots();
    LayoutID declared = declaredLayoutOf(program, constructor.constructedClass);
    return declared && program.at(declared).has(LayoutFlags::IsConstructorFunction) && !isConstructorFunctionBelowValue(program.at(declared)) && slots.holdsCompiledObjects(constructor.constructedClass) && !slots.isNeverAllocated(constructor.constructedClass);
}

static LayoutID classOfClassObjectAtInWalkCalls(Walker& walker, Reference value)
{
    TypeID proven = value ? walker.typeOf(value) : TypeID();
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    return constructor ? classOfClassObject(walker.program(), constructor) : LayoutID();
}

static bool isWrittenAtOrBelowInWalkCalls(const Program& program, LayoutID layout, LayoutID above)
{
    for (size_t stepsLeft = program.layouts.size(); layout && stepsLeft; layout = program.at(layout).base, --stepsLeft) {
        if (layout == above)
            return true;
    }
    return false;
}

static bool standsAtOrBelowByParentsOfClassRecordsInWalkCalls(const Walker& walker, LayoutID layout, LayoutID above)
{
    for (size_t stepsLeft = walker.program().layouts.size(); layout && stepsLeft; --stepsLeft) {
        if (layout == above)
            return true;
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, layout, parent))
            return false;
        layout = parent;
    }
    return false;
}

static bool walkConstructOfFunctionInWalkCalls(Walker&, const BodyValue& call, CallInWalkCalls&, Reference classObjectBelow);

static bool continueIfPrototypeUnchanged(Walker& walker, const BodyValue& call, CallInWalkCalls& request, const GuardOfPrototypeAtConstruction& guard, Origin origin)
{
    if (guard.isKnownToHold)
        return !walker.isUnreachable();
    origin.isTailCall = false;
    if (!walker.state().guardsHaveBegun(guard.condition.layout)) {
        Draft begin(Opcode::BeginMemberGuards, origin);
        begin.setLayout(guard.condition.layout);
        walker.build(WTF::move(begin));
    }
    Draft look(Opcode::AreMembersUnchanged, origin);
    look.setConditions(singleElementSpan(guard.condition));
    Reference holds = walker.isUnreachable() ? Reference() : walker.build(WTF::move(look));
    if (!holds)
        return false;
    BlockID whereItHolds = walker.addBlock();
    BlockID whereItHasEnded = walker.addBlock(BlockFrequency::Rare);
    walker.branch(holds, whereItHolds, whereItHasEnded);
    walker.recordSpeculation();

    walker.continueIn(whereItHasEnded);
    {
        Walker::ClosedArm closedArm(walker, true);
        CallInWalkCalls withoutGuard;
        withoutGuard.callee = request.callee;
        withoutGuard.closure = request.closure;
        withoutGuard.isNeverInlined = true;
        if (!walker.isUnreachable() && !walkConstructOfFunctionInWalkCalls(walker, call, withoutGuard, { }))
            walker.fail("a construction that is built behind the guard of its prototype is not built without it"_s);
        if (!walker.isUnreachable() && !walker.hasFailed())
            request.armWhereGuardOfPrototypeHasEnded = Walker::JoinArm { walker.currentBlock(), { walker.referenceBoundTo(call.id()) } };
    }
    if (walker.hasFailed())
        return false;
    walker.continueIn(whereItHolds);
    if (walker.isUnreachable())
        return false;
    noteGuardsShownByTest(walker, singleElementSpan(guard.condition), holds);
    return true;
}

static void letConstructionMeetArmWhereGuardHasEndedInWalkCalls(Walker& walker, const BodyValue& call, const CallInWalkCalls& request)
{
    if (!request.armWhereGuardOfPrototypeHasEnded || walker.hasFailed())
        return;
    Vector<Walker::JoinArm, 2> arms { *request.armWhereGuardOfPrototypeHasEnded };
    if (!walker.isUnreachable())
        arms.append(Walker::JoinArm { walker.currentBlock(), { walker.referenceBoundTo(call.id()) } });
    Reference met;
    if (walker.joinArms(arms.span(), walker.addBlock(), singleElementSpan(met)))
        walker.bind(call.id(), met);
}

static bool walkConstructOfFunctionInWalkCalls(Walker& walker, const BodyValue& call, CallInWalkCalls& request, Reference classObjectBelow = { })
{
    const Program& program = walker.program();
    const Function& constructor = program.at(request.callee);
    const CallingConventions& conventions = walker.plan().conventions();
    if (!makesThisOfConstruction(walker, request.callee))
        return false;
    LayoutID constructed = constructor.constructedClass;
    if (classObjectBelow) {
        const Plan& plan = walker.plan();
        LayoutID below = classOfClassObjectAtInWalkCalls(walker, classObjectBelow);
        if (!below || !isWrittenAtOrBelowInWalkCalls(program, below, declaredLayoutOf(program, constructed)) || !isFoundByShapeTest(program.at(below)) || !plan.isInUse(below) || !plan.slots().holdsCompiledObjects(below))
            return false;
        constructed = below;
    } else if (!chooseLayoutByTagAtConstruction(walker, call, request.callee, constructed))
        return false;
    TypeArena& types = walker.types();

    Draft written = walker.draftOf(call);
    written.expected = { };
    written.inputValueGiven = { };
    request.newTarget = classObjectBelow ? classObjectBelow : request.closure;
    request.arguments = written.argumentsOperands();
    request.isConstruction = true;
    request.typeOfThisToBeMade = types.forLayout(constructed);
    CodeUnit entered = conventions.directEntryOf(request.callee);
    Vector<OperandInWalkCalls, 8> planned;
    if (!admitsDirectEntryInWalkCalls(walker, constructor, entered) || !planOperandsInWalkCalls(walker, request, constructor, entered, planned))
        return false;

    Origin origin = written.origin();
    Draft read(classObjectBelow ? Opcode::GetClassPrototype : Opcode::GetConstructPrototype, origin);
    if (classObjectBelow) {
        read.setClassObjectOperand(classObjectBelow);
        read.setLayout(constructed);
    } else {
        read.setNewTargetOperand(request.closure);
        read.setLayout(declaredLayoutOf(program, constructed));
    }
    walker.chooseConservativeFields(read);
    Effects ofRead = declaredBoundOf(Opcode::NewObject);
    ofRead.reads.add(AbstractHeap::NamedProperties);
    bool isPrototypeProven = !classObjectBelow && call.opcode() == Opcode::Construct && isPrototypeAtConstructionProven(walker, request.callee);
    std::optional<GuardOfPrototypeAtConstruction> guardOfPrototype = !classObjectBelow && !isPrototypeProven && call.opcode() == Opcode::Construct ? findGuardOfPrototypeAtConstruction(walker, call.id(), request.callee, walker.opensGuesses() && looksAtCellsAskForLayoutPrototype(walker, constructed), false, origin) : std::nullopt;
    if (guardOfPrototype && !continueIfPrototypeUnchanged(walker, call, request, *guardOfPrototype, origin))
        return true;
    bool allocationNamesPrototype = guardOfPrototype || isPrototypeProven || (classObjectBelow && walker.facts().isEvaluatedAtMostOnce(constructed) && walker.plan().hasClassRecord(constructed));
    Reference prototype = allocationNamesPrototype ? Reference() : walker.build(WTF::move(read), &ofRead);
    if (!prototype && !allocationNamesPrototype)
        return true;
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(constructed);
    if (prototype)
        allocation.setPrototypeOperand(prototype);
    walker.chooseConservativeFields(allocation);
    allocation.setEndsNoGuards(walker.isInLoop() && writerEndsNoGuard(walker, allocation));
    Reference object = walker.build(WTF::move(allocation));
    if (!object)
        return true;
    request.thisValue = object;
    request.typeOfThisToBeMade = { };
    noteFreshObject(walker, object, 0);
    notePrototypeOfConstruction(walker, object, constructed);
    if (guardOfPrototype || isPrototypeProven) {
        checkPrototypeOfCellMadeBehindGuard(walker, object, request.closure, constructed, origin);
        walker.state().recordCellFacts(object, CellFact::HasLayoutPrototype);
    }

    bool isCopied = false;
    if (walker.has(Mechanism::Inlining) && !request.isNeverInlined && !constructor.isHiddenInStackTraces()) {
        planned.shrink(0);
        if (!planOperandsInWalkCalls(walker, request, constructor, entered, planned)) {
            walker.fail("the body of a constructor function does not take the object that was made for it"_s);
            return true;
        }
        Vector<Reference, 8> operands;
        for (const OperandInWalkCalls& operand : planned) {
            Reference built = buildOperandInWalkCalls(walker, request, constructor, origin, operand);
            if (!built)
                return true;
            operands.append(built);
        }
        Draft site(Opcode::CallDirect, origin);
        site.setFunction(request.callee);
        site.setIsConstruction(true);
        site.setVariadicOperands(operands.span());
        isCopied = shouldCopyCallee(walker, site, request.callee, operands.span()) && walker.walkInline(call, request.callee, operands.span());
    }
    if (!isCopied && !buildDirectCallInWalkCalls(walker, call, written, request)) {
        walker.fail("the body of a constructor function does not take the object that was made for it"_s);
        return true;
    }
    Reference returned = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (!returned)
        return true;

    TypeID provenOfBody = walker.plan().typeHeldOfResult(walker.program(), walker.facts().linked(), request.callee);
    TypeID ofWhatCameBack = walker.typeOf(returned);
    bool givesThis = (provenOfBody && !types.overlap(provenOfBody, TypeTable::object)) || (ofWhatCameBack && !types.overlap(ofWhatCameBack, TypeTable::object));
    if (givesThis && isCopied)
        object = allocateFilledObject(walker, object, origin);
    Draft finish(Opcode::TryFinishBirth, origin);
    finish.setObjectOperand(object);
    walker.chooseConservativeFields(finish);
    if (lookMayFinishBirthOf(walker, object))
        walker.build(WTF::move(finish));
    showWhatIsHeldFilledWhereConstructionEnds(walker, request.callee, object, origin);
    if (walker.isUnreachable())
        return true;

    if (givesThis) {
        walker.bind(call.id(), object);
        return true;
    }
    if (ofWhatCameBack && types.isSubset(ofWhatCameBack, TypeTable::object))
        return true;

    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(returned);
    test.setTestedType(TypeTable::object);
    walker.chooseConservativeFields(test);
    Reference returnsObject = walker.build(WTF::move(test));
    if (!returnsObject)
        return true;
    BlockID takesReturned = walker.addBlock(BlockFrequency::Unlikely);
    BlockID takesThis = walker.addBlock();
    BlockID behind = walker.addBlock();
    Reference result = walker.addParameter(behind, { }, Representation::JSValue);
    walker.branch(returnsObject, takesReturned, takesThis);
    walker.continueIn(takesReturned);
    Reference objectReturned = returned;
    if (returnsObject.isValue() && returned.isValue()) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(returned);
        narrowed.setTestOperand(returnsObject);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        if (Reference built = walker.build(WTF::move(narrowed)))
            objectReturned = built;
    }
    walker.jumpTo(behind, singleElementSpan(objectReturned));
    walker.continueIn(takesThis);
    walker.jumpTo(behind, singleElementSpan(object));
    walker.continueIn(behind);
    walker.bind(call.id(), result);
    return true;
}

static bool constructAsWrittenInWalkCalls(Walker& walker, const BodyValue& call, FunctionID constructor, bool isBehindTest)
{
    TypeID made = constructor ? typeOfObjectsMadeThroughEntryOf(walker, constructor) : TypeID();
    if (!made && !isBehindTest)
        return false;
    Draft written = walker.draftOf(call);
    if (walker.isUnreachable())
        return true;
    walker.chooseConservativeFields(written);
    if (Reference built = buildCallAsWrittenThatGives(walker, WTF::move(written), made))
        walker.bind(call.id(), built);
    return true;
}

static bool walkConstructInWalkCalls(Walker& walker, const BodyValue& call)
{
    auto operation = call.as<Opcode::Construct>();
    if (operation.newTargetOperand() && operation.newTargetOperand() != operation.calleeOperand())
        return false;

    CallInWalkCalls request;
    request.closure = walker.referenceOf(operation.calleeOperand());
    request.callee = findOnlyCallee(walker, call, request.closure);
    BindingOfCalleeInWalkCalls bindingOfCallee { walker, { }, walker.referenceBoundTo(operation.calleeOperand()) };
    if (!request.callee && !walker.hasUninformativeType(request.closure) && testCalleeThatMayBeNoObjectInWalkCalls(walker, call)) {
        bindingOfCallee.input = operation.calleeOperand();
        if (walker.isUnreachable() || (walker.has(Mechanism::BuiltinCalls) && callBuiltin(walker, call)) || (walker.has(Mechanism::EscapeAnalysis) && walker.opensGuesses() && listStoresOfCopiedConstructor(walker, call)))
            return true;
        request.closure = walker.referenceOf(operation.calleeOperand());
        request.callee = findOnlyCallee(walker, call, request.closure);
    }
    bool isBehindTest = !!bindingOfCallee.input;
    if (!request.callee) {
        FunctionID listed = walker.has(Mechanism::TypedOperations) && !operation.newTargetOperand() ? walker.calleeListedAt(walker.currentFunction(), call.id()) : FunctionID();
        return constructAsWrittenInWalkCalls(walker, call, listed, isBehindTest);
    }
    const Function& constructor = walker.program().at(request.callee);
    if (!isClassConstructorOf(constructor.kind)) {
        if (!walkConstructOfFunctionInWalkCalls(walker, call, request))
            return constructAsWrittenInWalkCalls(walker, call, operation.newTargetOperand() ? FunctionID() : request.callee, isBehindTest);
        letConstructionMeetArmWhereGuardHasEndedInWalkCalls(walker, call, request);
        return true;
    }
    if (constructor.has(FunctionFlags::HasThis))
        return constructAsWrittenInWalkCalls(walker, call, { }, isBehindTest);

    Draft written = walker.draftOf(call);
    request.newTarget = request.closure;
    request.arguments = written.argumentsOperands();
    request.isConstruction = true;
    return buildDirectCallInWalkCalls(walker, call, written, request);
}

static bool walkSuperConstructInWalkCalls(Walker& walker, const BodyValue& call)
{
    auto operation = call.as<Opcode::SuperConstruct>();
    CallInWalkCalls request;
    request.closure = walker.referenceOf(operation.parentOperand());
    request.callee = findOnlyCallee(walker, call, request.closure);
    if (!request.callee)
        return false;
    const Function& constructor = walker.program().at(request.callee);
    if (!isClassConstructorOf(constructor.kind))
        return walkConstructOfFunctionInWalkCalls(walker, call, request, walker.referenceOf(operation.newTargetOperand()));
    if (constructor.has(FunctionFlags::HasThis))
        return false;

    Draft written = walker.draftOf(call);
    request.newTarget = written.newTargetOperand();
    request.arguments = written.argumentsOperands();
    request.isConstruction = true;
    return buildDirectCallInWalkCalls(walker, call, written, request);
}

static bool parametersAreReadOutOfListInWalkCalls(Walker& walker, const Function& callee, Reference list)
{
    if (!list || callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter }))
        return false;
    return !callee.numberOfPositionalParameters() || (walker.has(Mechanism::TypedOperations) && isFreshPristineArray(walker, list));
}

static bool parametersAreReadOutOfListMadeElsewhereInWalkCalls(Walker& walker, const BodyValue& call, const Function& callee, Reference list, Origin originInFrontOfCall)
{
    if (!list || callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter }) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis))
        return false;
    if (!hasKind(walker.types(), walker.typeOf(list), OperandKind::Array) || !isArrayPrototypeChainSane(walker))
        return false;
    if (isKnownPristine(walker, list))
        return true;
    ValueID written;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (walker.referenceOf(operand) == list)
            written = operand;
    });
    if (!written || !linksWordCanBeHeldAgainst(walker, list) || classOfIterableThatStaysAsMadeByLink(walker, call, written) != BuiltinClasses::Array)
        return false;
    holdLinksWordAgainstObjectAsMade(walker, list, originInFrontOfCall);
    return isKnownPristine(walker, list);
}

static bool readParametersOutOfListInWalkCalls(Walker& walker, const Function& callee, Reference list, Origin originInFrontOfCall, Vector<Reference, 8>& elements)
{
    unsigned numberOfParameters = callee.numberOfPositionalParameters();
    unsigned numberWrittenInFront = elements.size();
    std::optional<uint32_t> firstArgument = restArrayFirstArgument(walker, list);
    for (unsigned position = numberWrittenInFront; position < numberOfParameters; ++position) {
        if (firstArgument) {
            Draft argument(Opcode::GetArgument, originInFrontOfCall);
            argument.setArgumentIndex(*firstArgument + position - numberWrittenInFront);
            walker.chooseConservativeFields(argument);
            Reference element = walker.build(WTF::move(argument));
            if (!element)
                return false;
            elements.append(element);
            continue;
        }
        Draft at(Opcode::Integer, originInFrontOfCall);
        at.setInteger(position - numberWrittenInFront);
        Draft read(Opcode::GetArrayElement, originInFrontOfCall);
        walker.chooseConservativeFields(read);
        read.setArrayOperand(list);
        read.setPositionOperand(walker.constant(WTF::move(at)));
        read.setArrayStorage(ArrayStorage::Other);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        Reference element = walker.build(WTF::move(read));
        if (!element)
            return false;
        elements.append(element);
    }
    return true;
}

static Reference findBorrowableSpreadSource(Walker& walker, const BodyValue& call, FunctionID calleeID, const Function& callee, Reference copy)
{
    if (callee.numberOfPositionalParameters() || !walker.has(Mechanism::TypedOperations) || !isReadOnlyRestArray(walker, calleeID, UseOfRestParameter::PassedToCall))
        return { };
    Reference source = findElidableSpreadSource(walker, call, copy);
    if (!source)
        source = copy;
    return isFreshPristineArray(walker, source) ? source : Reference();
}

bool listOfSuperCallCanBeHandedOver(Walker& walker, const BodyValue& call, FunctionID callee)
{
    Reference list = walker.referenceOf(call.as<Opcode::SuperConstructList>().listOperand());
    if (!list)
        return false;
    const Function& constructor = walker.program().at(callee);
    if (!isClassConstructorOf(constructor.kind) || constructor.has(FunctionFlags::HasThis))
        return false;
    return elementsOfFreshList(walker, list) || parametersAreReadOutOfListInWalkCalls(walker, constructor, list) || findBorrowableSpreadSource(walker, call, callee, constructor, list);
}

static bool walkConstructionOfListInWalkCalls(Walker& walker, const BodyValue& call)
{
    bool isSuperCall = call.opcode() == Opcode::SuperConstructList;
    CallInWalkCalls request;
    request.closure = walker.referenceOf(isSuperCall ? call.parentOperand() : call.calleeOperand());
    request.callee = findOnlyCallee(walker, call, request.closure);
    if (!request.callee)
        return false;
    const Function& constructor = walker.program().at(request.callee);
    if (!isClassConstructorOf(constructor.kind) || constructor.has(FunctionFlags::HasThis))
        return false;
    std::optional<Draft> written = draftOfCallWithListWrittenOut(walker, call);
    Vector<Reference, 8> elements;
    if (!written) {
        written = walker.draftOf(call);
        Reference borrowed = isSuperCall ? findBorrowableSpreadSource(walker, call, request.callee, constructor, written->listOperand()) : Reference();
        if (Reference source = borrowed ? borrowed : findElidableSpreadSource(walker, call, written->listOperand()); source && (borrowed || parametersAreReadOutOfListInWalkCalls(walker, constructor, source))) {
            written->setListOperand(source);
            walker.count(StatisticID::SpreadCallArrayCopiesElided);
        }
        if (!isSuperCall || (!borrowed && !parametersAreReadOutOfListInWalkCalls(walker, constructor, written->listOperand())))
            return false;
        request.listOfUnknownLength = written->listOperand();
        if (borrowed) {
            request.borrowedHiddenRest = borrowed;
            request.listLengthIsBounded = holdsArgumentsOfUnit(walker, borrowed);
            request.borrowedHiddenRestLength = buildSpreadArrayLength(walker, borrowed, written->origin());
            if (!request.borrowedHiddenRestLength)
                return true;
        } else if (!readParametersOutOfListInWalkCalls(walker, constructor, request.listOfUnknownLength, written->origin(), elements))
            return true;
    }
    if (!isSuperCall && written->newTargetOperand() && written->newTargetOperand() != written->calleeOperand())
        return false;
    request.newTarget = isSuperCall ? written->newTargetOperand() : request.closure;
    request.arguments = request.listOfUnknownLength ? elements.span() : written->argumentsOperands();
    request.isConstruction = true;
    return buildDirectCallInWalkCalls(walker, call, *written, request);
}

static bool isLiteralWithoutHolesInWalkCalls(const BodyIndex& index, ValueID list)
{
    return !index.isParameter(list) && index.opcodeOf(list) == Opcode::NewArray && index.value(list).as<Opcode::NewArray>().holes().empty();
}

static bool listIsMadeForCallAloneInWalkCalls(const BodyIndex& index, ValueID list, ValueID call)
{
    if (index.hasIrreducibleFlow() || !isLiteralWithoutHolesInWalkCalls(index, list) || index.loopOf(index.blockOf(list)) != index.loopOf(index.blockOf(call)))
        return false;
    uint32_t mentions = index.useCount(list);
    if (mentions == BodyIndex::maxCountedUses)
        return false;
    uint32_t appends = 0;
    for (uint32_t id = list.index() + 1; id < call.index(); ++id) {
        if (index.isParameter(ValueID(id)))
            continue;
        BodyValue value = index.value(ValueID(id));
        appends += appendsToListInWalkCalls.contains(value.opcode()) && value.operand(0) == list;
    }
    return appends + 1 == mentions;
}

static ValueID findListSpreadBehindArgumentsInWalkCalls(const BodyIndex& index, ValueID list, ValueID call)
{
    if (!listIsMadeForCallAloneInWalkCalls(index, list, call) || index.useCount(list) != 2)
        return { };
    ValueID spread(call.index() - 1);
    if (index.isParameter(spread) || index.blockOf(spread) != index.blockOf(call) || index.opcodeOf(spread) != Opcode::AppendSpread || index.value(spread).operand(0) != list)
        return { };
    return index.value(spread).as<Opcode::AppendSpread>().iterableOperand();
}

static ValueID findLastOperationThatIsObservedInWalkCalls(const Code& code, BlockID block)
{
    for (unsigned blocksLeft = 2; blocksLeft; --blocksLeft) {
        for (ValueID id = code.lastOperationOf(block); id; id = code.at(id).previous) {
            const Operation& operation = code.at(id);
            if (operation.opcode == Opcode::AppendElements || !operation.traits().isUnobservable() || operation.traits().mayCollect())
                return id;
        }
        BlockID from;
        unsigned numberOfEdges = 0;
        code.forEachIncomingEdgeUnsealed(block, [&](const Edge& edge) {
            from = edge.from;
            ++numberOfEdges;
        });
        if (numberOfEdges != 1)
            return { };
        block = from;
    }
    return { };
}

enum class WrittenInFrontOfListInWalkCalls : bool { Nothing, Values };

static Reference findSpreadCopySource(Walker& walker, const BodyValue& call, Reference copy, WrittenInFrontOfListInWalkCalls written = WrittenInFrontOfListInWalkCalls::Nothing)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID list = call.listOperand();
    if (!findListSpreadBehindArgumentsInWalkCalls(index, list, call.id()) || index.value(list).as<Opcode::NewArray>().elementsOperands().empty() != (written == WrittenInFrontOfListInWalkCalls::Nothing))
        return { };
    const Code& code = walker.code();
    ValueID last = findLastOperationThatIsObservedInWalkCalls(code, walker.currentBlock());
    if (!copy || !copy.isValue() || !last || code.at(copy.id()).opcode != Opcode::NewArray)
        return { };
    const Operation& append = code.at(last);
    if (append.opcode != Opcode::AppendElements || append.arrayOperand() != copy.id())
        return { };
    return Reference::value(append.sourceOperand());
}

Reference findElidableSpreadSource(Walker& walker, const BodyValue& call, Reference copy)
{
    return findSpreadCopySource(walker, call, copy);
}

static bool isOnlySpreadInWalkCalls(const BodyIndex& index, ValueID list)
{
    uint32_t uses = index.useCount(list);
    if (uses == BodyIndex::maxCountedUses)
        return false;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue operation : index.block(BlockID(number)).values())
            uses -= operation.opcode() == Opcode::AppendSpread && operation.as<Opcode::AppendSpread>().iterableOperand() == list;
    }
    return !uses;
}

static std::optional<Vector<Reference, 8>> elementsOfRestList(Walker& walker, const BodyValue& call, Reference list)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Code& code = walker.code();
    ValueID spreadList = findListSpreadBehindArgumentsInWalkCalls(index, call.listOperand(), call.id());
    const Function& function = walker.program().at(walker.currentFunction());
    std::optional<uint32_t> entryParameter = spreadList && index.isParameter(spreadList) ? index.block(entryBlock).indexOfParameter(spreadList) : std::nullopt;
    bool isRestParameter = entryParameter && function.entryParameterKind(*entryParameter) == EntryParameter::Parameter && *entryParameter - function.firstParameterIndex() >= function.numberOfPositionalParameters();
    if (!isRestParameter || !list.isValue() || code.isConstant(list.id()) || !isOnlySpreadInWalkCalls(index, spreadList))
        return std::nullopt;
    constexpr uint32_t mostOperationsLookedThrough = 4096;
    ValueID root = code.valueBehindNames<valuePreservingOpcodes>(list.id());
    const Operation& made = code.at(root);
    if (made.opcode != Opcode::NewArray || !made.holes().empty() || code.numberOfValues() - root.index() > mostOperationsLookedThrough)
        return std::nullopt;
    static constexpr OpcodeSet appends { Opcode::AppendElement, Opcode::AppendElements, Opcode::AppendHole, Opcode::AppendIterable, Opcode::AppendMembers, Opcode::AppendSpread, Opcode::AppendValuesOfMap };
    for (uint32_t number = root.index() + 1; number < code.numberOfValues(); ++number) {
        const Operation& operation = code.at(ValueID(number));
        if (appends.contains(operation.opcode) && !code.isConstant(operation.operand(0)) && code.valueBehindNames<valuePreservingOpcodes>(operation.operand(0)) == root)
            return std::nullopt;
    }
    Vector<Reference, 8> elements;
    for (ValueID element : made.variadicOperands()) {
        if (!element)
            return std::nullopt;
        elements.append(code.isConstant(element) ? Reference::constant(element) : Reference::value(element));
    }
    return elements;
}

static bool passElementsOfList(Walker& walker, const BodyValue& call, Draft& written, Reference list)
{
    constexpr size_t mostElements = 8;
    std::optional<Vector<Reference, 8>> elements;
    if (written.opcode == Opcode::CallList) {
        elements = elementsOfFreshList(walker, list);
        if (elements)
            walker.checkWhatIsInSightOfList(list, written.origin());
        else
            elements = elementsOfRestList(walker, call, list);
    }
    if (!elements || elements->size() > mostElements)
        return false;
    Draft plain = written;
    plain.become(Opcode::Call);
    plain.setCalleeOperand(written.calleeOperand());
    plain.setThisValueOperand(written.thisValueOperand());
    plain.setCalleeKey(written.calleeKey());
    plain.setCalleeName(written.calleeName());
    plain.setExpectedResult(written.expectedResult());
    plain.setCalledBuiltin(written.calledBuiltin());
    plain.setArgumentsOperands(elements->span());
    plain.expected = written.expected;
    plain.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    plain.inputValueGiven = written.inputValueGiven;
    walker.chooseConservativeFields(plain);
    written = WTF::move(plain);
    return true;
}

void elideSpreadCopy(Walker& walker, const BodyValue& call, Draft& written)
{
    Reference list = findSpreadCopySource(walker, call, written.listOperand());
    if (list && passElementsOfList(walker, call, written, list)) {
        walker.count(StatisticID::SpreadCallArrayCopiesElided);
        return;
    }
    if (!list) {
        written.setListOperand(sinkRestArrayAllocation(walker, written.listOperand()));
        return;
    }
    written.setListOperand(sinkRestArrayAllocation(walker, list));
    walker.count(StatisticID::SpreadCallArrayCopiesElided);
}

static bool listSpreadIsMadeForCallAloneInWalkCalls(Walker& walker, const BodyIndex& index, ValueID spreadList, ValueID call)
{
    const Function& function = walker.program().at(walker.currentFunction());
    std::optional<uint32_t> entryParameter = index.isParameter(spreadList) ? index.block(entryBlock).indexOfParameter(spreadList) : std::nullopt;
    bool isRestParameter = entryParameter && function.entryParameterKind(*entryParameter) == EntryParameter::Parameter && *entryParameter - function.firstParameterIndex() >= function.numberOfPositionalParameters();
    return isRestParameter && !index.hasIrreducibleFlow() && index.onlyUsingValue(spreadList) == ValueID(call.index() - 1) && !index.loopOf(index.blockOf(call))
        && isFreshPristineArray(walker, walker.referenceOf(spreadList));
}

bool hasKnownSpreadElements(Walker& walker, const BodyValue& call, ValueID before)
{
    if (call.opcode() != Opcode::CallList)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID spreadList = findListSpreadBehindArgumentsInWalkCalls(index, call.as<Opcode::CallList>().listOperand(), call.id());
    if (!spreadList || (!index.isParameter(spreadList) && spreadList.index() >= before.index()))
        return false;
    Reference spread = walker.referenceOf(spreadList);
    return spread && elementsOfFreshList(walker, spread);
}

bool listSpreadBehindArgumentsIsRestListOf(Walker& walker, const BodyValue& call, FunctionID function)
{
    const Function& callee = walker.program().at(function);
    if (call.opcode() != Opcode::CallList || !bodyRunsWhenCalledInWalkCalls(callee.kind) || !callee.has(FunctionFlags::HasRestParameter) || !callee.numberOfPositionalParameters() || callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID list = call.as<Opcode::CallList>().listOperand();
    ValueID spreadList = findListSpreadBehindArgumentsInWalkCalls(index, list, call.id());
    if (!spreadList || index.value(list).as<Opcode::NewArray>().elementsOperands().size() != callee.numberOfPositionalParameters())
        return false;
    return spreadAppendsElementsOfListAsMade(walker, spreadList);
}

static bool walkCallListInWalkCalls(Walker& walker, const BodyValue& call)
{
    auto operation = call.as<Opcode::CallList>();
    CallInWalkCalls request;
    request.closure = walker.referenceOf(operation.calleeOperand());
    request.callee = findOnlyCallee(walker, call, request.closure);
    if (!request.callee)
        return false;
    const Function& callee = walker.program().at(request.callee);
    if (!bodyRunsWhenCalledInWalkCalls(callee.kind))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID list = operation.listOperand();
    Vector<Reference, 8> elements;
    if (isLiteralWithoutHolesInWalkCalls(index, list) && index.onlyUsingValue(list) == call.id()) {
        for (ValueID element : index.value(list).as<Opcode::NewArray>().elementsOperands()) {
            Reference reference = walker.referenceOf(element);
            if (!reference)
                return false;
            elements.append(reference);
        }
        request.arguments = elements.span();
    } else if (std::optional<Vector<Reference, 8>> known = elementsOfFreshList(walker, walker.referenceOf(list))) {
        walker.checkWhatIsInSightOfList(walker.referenceOf(list), walker.originOf(call));
        elements = WTF::move(*known);
        request.arguments = elements.span();
    } else {
        if (listSpreadBehindArgumentsIsRestListOf(walker, call, request.callee)) {
            ValueID spreadList = findListSpreadBehindArgumentsInWalkCalls(index, list, call.id());
            for (ValueID element : index.value(list).as<Opcode::NewArray>().elementsOperands()) {
                Reference reference = walker.referenceOf(element);
                if (!reference)
                    return false;
                elements.append(reference);
            }
            request.arguments = elements.span();
            request.restList = walker.referenceOf(spreadList);
            request.restListStandsBehindArguments = true;
            request.restListIsCopied = !listSpreadIsMadeForCallAloneInWalkCalls(walker, index, spreadList, call.id());
            request.listOfUnknownLength = request.restList;
            Draft written = walker.draftOf(call);
            request.thisValue = written.thisValueOperand();
            request.calledBuiltin = written.calledBuiltin();
            return buildDirectCallInWalkCalls(walker, call, written, request);
        }
        if (callee.numberOfPositionalParameters() || callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest })) {
            if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter }) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis))
                return false;
            request.listOfUnknownLength = findSpreadCopySource(walker, call, walker.referenceOf(list));
            Vector<Reference, 8> writtenInFront;
            if (!request.listOfUnknownLength && (request.listOfUnknownLength = findSpreadCopySource(walker, call, walker.referenceOf(list), WrittenInFrontOfListInWalkCalls::Values))) {
                for (ValueID element : index.value(list).as<Opcode::NewArray>().elementsOperands()) {
                    Reference reference = walker.referenceOf(element);
                    if (!reference)
                        return false;
                    writtenInFront.append(reference);
                    if (elements.size() < callee.numberOfPositionalParameters())
                        elements.append(reference);
                }
                request.writtenInFrontOfList = writtenInFront.span();
            }
            if (!request.listOfUnknownLength || !isKnownPristine(walker, request.listOfUnknownLength) || !hasKind(walker.types(), walker.typeOf(request.listOfUnknownLength), OperandKind::Array) || !isArrayPrototypeChainSane(walker))
                return false;
            Draft written = walker.draftOf(call);
            Origin originInFrontOfCall = written.origin();
            originInFrontOfCall.isTailCall = false;
            if (!readParametersOutOfListInWalkCalls(walker, callee, request.listOfUnknownLength, originInFrontOfCall, elements))
                return true;
            walker.count(StatisticID::SpreadCallArrayCopiesElided);
            walker.count(StatisticID::SpreadCallsUnpackedToParameters);
            if (writtenInFront.isEmpty())
                written.setListOperand(request.listOfUnknownLength);
            request.arguments = elements.span();
            request.thisValue = written.thisValueOperand();
            request.calledBuiltin = written.calledBuiltin();
            return buildDirectCallInWalkCalls(walker, call, written, request);
        }
        request.listOfUnknownLength = walker.referenceOf(list);
        if (!request.listOfUnknownLength)
            return false;
        if (callee.has(FunctionFlags::HasRestParameter)) {
            if (!listIsMadeForCallAloneInWalkCalls(index, list, call.id()))
                return false;
            request.restList = request.listOfUnknownLength;
        }
    }

    Draft written = walker.draftOf(call);
    request.thisValue = written.thisValueOperand();
    request.calledBuiltin = written.calledBuiltin();
    return buildDirectCallInWalkCalls(walker, call, written, request);
}

static bool hasProvenWhatGuardTestsInWalkCalls(Walker& walker, FunctionID callee, std::span<const Reference> operands)
{
    for (unsigned entryParameter = 0; entryParameter < operands.size(); ++entryParameter) {
        if (!hasProvenWhatGuardTestsInWalkCalls(walker, callee, entryParameter, operands[entryParameter]))
            return false;
    }
    return true;
}

static bool bringOperandsIntoFormsOfCalleeInWalkCalls(Walker& walker, Origin origin, CodeUnit entered, Vector<Reference, 8>& operands)
{
    TypeArena& types = walker.types();
    origin.isTailCall = false;
    for (unsigned index = 0; index < operands.size(); ++index) {
        Representation wanted = walker.plan().conventions().parameterRepresentation(walker.program(), entered, index);
        Type seen = types.at(walker.typeOf(operands[index]));
        if (!hasRegister(wanted) || isLegalRepresentation(seen, wanted))
            continue;
        TypeID promised = walker.facts().typeOfEntryParameter(entered.function, index).proven;
        if (!promised || !types.at(promised).isJavaScriptValue() || !seen.isJavaScriptValue() || seen.mayBeUnspecified())
            continue;
        operands[index] = walker.proveOrTrap(operands[index], types.testablePartOf(promised), ProofSource::EntryParameter, origin);
        if (!operands[index])
            return false;
    }
    return true;
}

static bool walkDirectCallOfInputInWalkCalls(Walker& walker, const BodyValue& call)
{
    Draft direct = walker.draftOf(call);
    walker.chooseConservativeFields(direct);
    FunctionID callee = direct.function();
    Vector<Reference, 8> operands(direct.variadicOperands());
    if (returnsResultOfFunction(direct.role())) {
        if (!bringOperandsIntoFormsOfCalleeInWalkCalls(walker, walker.originOf(call), CodeUnit { callee, direct.role() }, operands))
            return true;
        direct.setVariadicOperands(operands.span());
    }
    if (direct.role() == CodeRole::ParameterGuard && walker.mayEnterAfterGuardOf(callee) && hasProvenWhatGuardTestsInWalkCalls(walker, callee, operands.span()))
        direct.setRole(CodeRole::Body);
    CodeRole entered = direct.role();
    bool entersBody = returnsResultOfFunction(entered);
    std::optional<Effects> effects;
    if (entersBody)
        effects = effectsOfDirectCall(walker, callee, operands.span());
    bool neverReturns = entersBody && !direct.isConstruction() && calleeNeverReturns(walker, callee);
    walker.buildsCallWithoutReturn(neverReturns && !walker.standsUnderHandler());
    Reference result = walker.buildCallThatMayBorrowScopes(WTF::move(direct), effects ? &*effects : nullptr);
    walker.buildsCallWithoutReturn(false);
    if (!result)
        return true;
    countDirectEntry(walker, callee, entered);
    if (entersBody)
        walker.leaveArrivals(callee, operands.span());
    if (neverReturns) {
        Origin origin = walker.originOf(call);
        origin.isTailCall = false;
        walker.count(StatisticID::DirectCallsWithoutReturn);
        walker.endAsUnreachable(origin);
        return true;
    }
    walker.bind(call.id(), result);
    return true;
}

static constexpr OpcodeSet opcodesOfConstructionInWalkCalls { Opcode::GetConstructPrototype, Opcode::NewObject };

constinit const OpcodeSet opcodesThatBuildTypedConstructionTakes = opcodesOfConstructionInWalkCalls;
static_assert(opcodesThatBuildTypedConstructionTakesInChains == opcodesOfConstructionInWalkCalls);

static LayoutID classWhoseConstructorRunsInWalkCalls(Walker& walker, const Operation& fetch, LayoutID written)
{
    const Program& program = walker.program();
    const Function& own = program.at(walker.unit().function);
    LayoutID ownClass = own.constructedClass;
    if (walker.unit().role != CodeRole::Body || !ownClass || ownClass == written || !own.has(FunctionFlags::HasNewTarget) || !isWrittenAtOrBelowInWalkCalls(program, ownClass, written))
        return { };
    if (!isFoundByShapeTest(program.at(ownClass)) || !walker.plan().isInUse(ownClass) || !walker.plan().slots().holdsCompiledObjects(ownClass))
        return { };
    const LinkTables& link = walker.facts().linked();
    if (ownClass.index() < link.layouts.size() && !link.layout(ownClass).possibleChanges.contains(LayoutChange::ChangesPrototype))
        return { };
    const Code& code = walker.code();
    ValueID named = fetch.newTargetOperand();
    while (opcodesThatNameTheirOperand.contains(code.at(named).opcode))
        named = code.at(named).operand(0);
    std::span<const ValueID> entered = code.at(entryBlock).parameters();
    unsigned place = own.numberOfEntryParametersBefore(EntryParameter::NewTarget);
    return code.at(entryBlock).isEntry && place < entered.size() && entered[place] == named ? ownClass : LayoutID();
}

bool buildTypedConstructionOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!opcodesOfConstructionInWalkCalls.contains(value.opcode()))
        return false;

    if (value.opcode() == Opcode::GetConstructPrototype) {
        Reference newTarget = walker.referenceOf(value.as<Opcode::GetConstructPrototype>().newTargetOperand());
        LayoutID selected = classOfClassObjectAtInWalkCalls(walker, newTarget);
        if (!selected)
            return false;
        Draft read(Opcode::GetClassPrototype, walker.originOf(value));
        walker.chooseConservativeFields(read);
        read.inputValueGiven = value.id();
        read.setClassObjectOperand(newTarget);
        read.setLayout(selected);
        walker.bind(value.id(), walker.build(WTF::move(read)));
        return true;
    }

    auto allocation = value.as<Opcode::NewObject>();
    Reference prototype = allocation.prototypeOperand() ? walker.referenceOf(allocation.prototypeOperand()) : Reference();
    if (!prototype || !prototype.isValue() || !allocation.fields().empty())
        return false;
    const Code& code = walker.code();
    ValueID root = prototype.id();
    while (opcodesThatNameTheirOperand.contains(code.at(root).opcode))
        root = code.at(root).operand(0);
    const Operation& read = code.at(root);
    if (read.opcode == Opcode::GetConstructPrototype) {
        LayoutID ownClass = classWhoseConstructorRunsInWalkCalls(walker, read, allocation.layout());
        if (!ownClass)
            return false;
        Draft draft = walker.draftOf(value);
        walker.chooseConservativeFields(draft);
        draft.setLayout(ownClass);
        walker.count(StatisticID::AllocationsTypedByRunningConstructor);
        walker.bind(value.id(), walker.build(WTF::move(draft)));
        return true;
    }
    if (read.opcode != Opcode::GetClassPrototype)
        return false;
    ValueID classObject = read.classObjectOperand();
    LayoutID selected = classOfClassObjectAtInWalkCalls(walker, code.isConstant(classObject) ? Reference::constant(classObject) : Reference::value(classObject));
    const Plan& plan = walker.plan();
    bool standsBelow = selected && (isWrittenAtOrBelowInWalkCalls(walker.program(), selected, allocation.layout()) || standsAtOrBelowByParentsOfClassRecordsInWalkCalls(walker, selected, allocation.layout()));
    if (!standsBelow || selected != read.layout() || !isFoundByShapeTest(walker.program().at(selected)))
        return false;
    if (!plan.isInUse(selected) || !plan.slots().holdsCompiledObjects(selected))
        return false;

    Draft draft = walker.draftOf(value);
    walker.chooseConservativeFields(draft);
    draft.setLayout(selected);
    if (walker.facts().isEvaluatedAtMostOnce(selected) && plan.hasClassRecord(selected))
        draft.setPrototypeOperand({ });
    walker.bind(value.id(), walker.build(WTF::move(draft)));
    return true;
}

static bool copyOrCallDirectlyInWalkCalls(Walker& walker, const BodyValue& value, const Draft& written, const CallInWalkCalls& request)
{
    const Function& callee = walker.program().at(request.callee);
    CodeUnit entered = walker.plan().conventions().directEntryOf(request.callee);
    Vector<OperandInWalkCalls, 8> planned;
    bool mayBeCopied = walker.has(Mechanism::Inlining) && !request.isNeverInlined && !request.listOfUnknownLength && admitsDirectEntryInWalkCalls(walker, callee, entered) && planOperandsInWalkCalls(walker, request, callee, entered, planned);
    if (mayBeCopied) {
        Vector<Reference, 8> operands;
        for (const OperandInWalkCalls& operand : planned)
            operands.append(operand.source == OperandSourceInWalkCalls::Given && operand.given ? operand.given : walker.constant(Draft(Opcode::Undefined, written.origin())));
        Draft site(Opcode::CallDirect, written.origin());
        site.setFunction(request.callee);
        site.setVariadicOperands(operands.span());
        mayBeCopied = shouldCopyCallee(walker, site, request.callee, operands.span());
        if (!mayBeCopied && walker.currentFunction() == walker.unit().function) {
            Draft asWrittenOut(Opcode::Call, written.origin());
            asWrittenOut.setArgumentsOperands(request.arguments);
            mayBeCopied = turnsOfCopyWrittenOutAt(walker, asWrittenOut, request.callee, PlaceOfCall { }).has_value();
        }
    }
    if (mayBeCopied) {
        Origin originInFrontOfCall = written.origin();
        originInFrontOfCall.isTailCall = false;
        Vector<Reference, 8> operands;
        for (const OperandInWalkCalls& operand : planned) {
            Reference built = buildOperandInWalkCalls(walker, request, callee, originInFrontOfCall, operand);
            if (!built)
                return true;
            operands.append(built);
        }
        if (request.isMadeByProtocol ? walker.walkInlineMethodThatProtocolCalls(value, request.callee, operands.span()) : walker.walkInline(value, request.callee, operands.span()))
            return true;
    }
    if (!request.calledWhereNoCopyIsMade)
        return buildDirectCallInWalkCalls(walker, value, written, request);
    CallInWalkCalls ofOtherText = request;
    ofOtherText.callee = request.calledWhereNoCopyIsMade;
    return buildDirectCallInWalkCalls(walker, value, written, ofOtherText);
}

bool accessorAdmitsDirectCall(Walker& walker, FunctionID accessor)
{
    const Function& callee = walker.program().at(accessor);
    return bodyRunsWhenCalledInWalkCalls(callee.kind) && admitsDirectEntryInWalkCalls(walker, callee, walker.plan().conventions().entryBehindGuardOf(accessor));
}

static BuiltinID calledBuiltinOfAccess(const BodyValue& access)
{
    return access.opcode() == Opcode::PropSet ? access.as<Opcode::PropSet>().calledBuiltin() : BuiltinID();
}

bool callAccessor(Walker& walker, const BodyValue& access, FunctionID accessor, Reference thisValue, Reference stored, Reference closure)
{
    CallInWalkCalls request;
    request.callee = accessor;
    request.closure = closure;
    request.thisValue = thisValue;
    if (stored)
        request.arguments = singleElementSpan(stored);
    const Function& callee = walker.program().at(accessor);
    if (!thisValue || !bodyRunsWhenCalledInWalkCalls(callee.kind))
        return false;
    request.calledBuiltin = calledBuiltinOfAccess(access);
    request.isNeverInlined = callee.isHiddenInStackTraces() || request.calledBuiltin;
    if (!stored)
        walker.askObjectAgainForGetterThatNameFinds(access, thisValue, accessor);
    return copyOrCallDirectlyInWalkCalls(walker, access, walker.draftOf(access), request);
}

bool callTrapOfProxy(Walker& walker, const BodyValue& access, FunctionID trap, Reference handler, std::span<const Reference> arguments, Reference closure)
{
    const Function& callee = walker.program().at(trap);
    if (!handler || !closure || !bodyRunsWhenCalledInWalkCalls(callee.kind) || callee.isCoroutine())
        return false;
    CallInWalkCalls request;
    request.callee = trap;
    request.closure = closure;
    request.thisValue = handler;
    request.arguments = arguments;
    request.calledBuiltin = calledBuiltinOfAccess(access);
    request.isNeverInlined = callee.isHiddenInStackTraces() || request.calledBuiltin;
    request.isMadeByProtocol = true;
    Draft written(Opcode::Call, walker.originOf(access));
    walker.chooseConservativeFields(written);
    written.setCalleeOperand(closure);
    written.setThisValueOperand(handler);
    written.setArgumentsOperands(arguments);
    return copyOrCallDirectlyInWalkCalls(walker, access, written, request);
}

bool callMethodOfProtocol(Walker& walker, const BodyValue& step, FunctionID method, Reference thisValue, Reference closure)
{
    const Function& callee = walker.program().at(method);
    if (!thisValue || !closure || !bodyRunsWhenCalledInWalkCalls(callee.kind) || callee.isCoroutine())
        return false;
    CallInWalkCalls request;
    request.callee = method;
    request.closure = closure;
    request.thisValue = thisValue;
    request.isNeverInlined = callee.isHiddenInStackTraces();
    request.isMadeByProtocol = true;
    Draft written(Opcode::Call, walker.originOf(step));
    walker.chooseConservativeFields(written);
    written.setCalleeOperand(closure);
    written.setThisValueOperand(thisValue);
    return copyOrCallDirectlyInWalkCalls(walker, step, written, request);
}

std::optional<Vector<Reference, 8>> layOutEntryOperandsOfCall(Walker& walker, const BodyValue& call, FunctionID function, CodeUnit entered)
{
    const Function& callee = walker.program().at(function);
    if (call.opcode() != Opcode::Call || !walker.has(Mechanism::DirectCalls) || !bodyRunsWhenCalledInWalkCalls(callee.kind) || !admitsDirectEntryInWalkCalls(walker, callee, entered))
        return std::nullopt;
    Draft written = walker.draftOf(call);
    CallInWalkCalls request;
    request.callee = function;
    request.closure = written.calleeOperand();
    request.thisValue = written.thisValueOperand();
    request.arguments = written.argumentsOperands();
    Vector<OperandInWalkCalls, 8> planned;
    if (!planOperandsInWalkCalls(walker, request, callee, entered, planned))
        return std::nullopt;
    Origin origin = written.origin();
    origin.isTailCall = false;
    Vector<Reference, 8> operands;
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, origin, operand);
        if (!built)
            break;
        operands.append(built);
    }
    return operands;
}

std::optional<Vector<Reference, 8>> buildProtocolCallOperands(Walker& walker, FunctionID function, CodeUnit entered, Reference thisValue, Reference closure, Origin origin)
{
    const Function& callee = walker.program().at(function);
    if (!thisValue || !closure || !walker.has(Mechanism::DirectCalls) || !bodyRunsWhenCalledInWalkCalls(callee.kind) || !admitsDirectEntryInWalkCalls(walker, callee, entered))
        return std::nullopt;
    CallInWalkCalls request;
    request.callee = function;
    request.closure = closure;
    request.thisValue = thisValue;
    Vector<OperandInWalkCalls, 8> planned;
    if (!planOperandsInWalkCalls(walker, request, callee, entered, planned))
        return std::nullopt;
    origin.isTailCall = false;
    Vector<Reference, 8> operands;
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, origin, operand);
        if (!built)
            break;
        operands.append(built);
    }
    return operands;
}

bool callFunctionDirectly(Walker& walker, const BodyValue& operation, FunctionID function, Reference thisValue, std::span<const Reference> arguments, MayCopyFunctionCalledDirectly mayCopy, FunctionID calledWhereNoCopyIsMade)
{
    if (!canCallCompilerHelper(walker, function))
        return false;
    Draft written = walker.draftOf(operation);
    if (walker.hasFailed())
        return false;
    CallInWalkCalls request;
    request.callee = function;
    request.thisValue = thisValue;
    request.arguments = arguments;
    if (operation.opcode() == Opcode::Call)
        request.calledBuiltin = written.calledBuiltin();
    else
        request.isNeverInlined = walker.program().at(function).isHiddenInStackTraces();
    if (mayCopy == MayCopyFunctionCalledDirectly::No)
        request.isNeverInlined = true;
    request.calledWhereNoCopyIsMade = calledWhereNoCopyIsMade;
    return copyOrCallDirectlyInWalkCalls(walker, operation, written, request);
}

bool mayCopyFusedChain(Walker& walker, FunctionID text)
{
    return text && canCallCompilerHelper(walker, text) && admitsDirectEntryInWalkCalls(walker, walker.program().at(text), walker.plan().conventions().directEntryOf(text));
}

bool copyFusedChain(Walker& walker, const BodyValue& lastCall, FunctionID text, std::span<const Reference> arguments, BlockID& asWritten)
{
    const Function& callee = walker.program().at(text);
    CallInWalkCalls request;
    request.callee = text;
    request.arguments = arguments;
    Vector<OperandInWalkCalls, 8> planned;
    if (!planOperandsInWalkCalls(walker, request, callee, walker.plan().conventions().directEntryOf(text), planned))
        return false;
    Origin origin = walker.originOf(lastCall);
    origin.isTailCall = false;
    Vector<Reference, 8> operands;
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, origin, operand);
        if (!built)
            return false;
        operands.append(built);
    }
    Draft site(Opcode::CallDirect, origin);
    site.setFunction(text);
    site.setVariadicOperands(operands.span());
    return walker.walkInlineFusedChain(lastCall, text, operands.span(), site, asWritten);
}

static constexpr uint32_t closedOperationsOfReadThatCallsGetter = 2;

static bool callGetterExpectedUnderKeyInWalkCalls(Walker& walker, const BodyValue& read, Reference object, FunctionID getter, TypeID closures, BlockID inputBlock, uint32_t place, Origin origin)
{
    Reference property = walker.referenceOf(read.propertyOperand());
    if (!property)
        return false;
    Draft lookup(Opcode::ElemGetOrOwnAccessor, origin);
    walker.chooseConservativeFields(lookup);
    lookup.setObjectOperand(object);
    lookup.setPropertyOperand(property);
    Reference found = walker.build(WTF::move(lookup));
    if (!found)
        return true;
    Draft half(Opcode::GetGetterOfAccessorOrUndefined, origin);
    walker.chooseConservativeFields(half);
    half.setValueOperand(found);
    Reference function = walker.build(WTF::move(half));
    if (!function)
        return true;
    auto testFor = [&](TypeID tested) {
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(function);
        test.setTestedType(tested);
        return walker.build(WTF::move(test));
    };
    Reference isAnswer = testFor(TypeTable::undefined);
    if (!isAnswer)
        return true;

    BlockID answers = walker.addBlock();
    BlockID holdsAccessor = walker.addBlock();
    BlockID calls = walker.addBlock();
    BlockID callsByValue = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.branch(isAnswer, answers, holdsAccessor);
    walker.recordSpeculation();

    Walker::StretchInArms stretch = walker.stretchInArms(inputBlock, place, place + 1);
    Walker::ArmsThatMeet arms;
    walker.continueIn(answers);
    walker.bind(read.id(), found);
    walker.listArm(stretch, arms);

    auto callByValue = [&] {
        Draft call(Opcode::Call, origin);
        call.inputValueGiven = read.id();
        call.setCalleeOperand(function);
        call.setThisValueOperand(object);
        walker.bind(read.id(), walker.build(WTF::move(call)));
        walker.listArm(stretch, arms);
    };
    walker.continueIn(holdsAccessor);
    walker.letGuessFailUnasked(callsByValue, origin);
    Reference isGetter = testFor(closures);
    if (!isGetter)
        return true;
    walker.branch(isGetter, calls, callsByValue);
    walker.continueIn(callsByValue);
    callByValue();
    if (walker.hasFailed())
        return true;

    walker.continueIn(calls);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(function);
    proof.setTestOperand(isGetter);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    Reference closure = walker.build(WTF::move(proof));
    if (closure && !callAccessor(walker, read, getter, object, { }, closure))
        callByValue();
    else
        walker.listArm(stretch, arms);
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

static uint32_t findEndOfArmsOfGetterInWalkCalls(Walker& walker, const BodyIndex& index, const BodyBlock& block, const BodyValue& read, uint32_t place)
{
    static constexpr uint32_t maxOperationsBetween = 8;
    uint32_t end = place + 1;
    ValueID called = walker.isInLoop() ? index.onlyUsingValue(read.id()) : ValueID();
    if (!called || index.opcodeOf(called) != Opcode::Call || index.value(called).calleeOperand() != read.id() || !block.holds(called))
        return end;
    uint32_t endBehindCall = called.index() - block.firstValue().index() + 1;
    if (endBehindCall > end + maxOperationsBetween || endBehindCall > limitOfStretchOfArms(walker, block))
        return end;
    for (uint32_t later = end; later < endBehindCall; ++later) {
        if (!mayBeInStretchOfArms(index, index.value(ValueID(block.firstValue().index() + later))))
            return end;
    }
    return endBehindCall;
}

static Reference narrowToObjectInIterationInWalkCalls(Walker& walker, Reference object, Origin origin)
{
    static constexpr auto purity = Walker::IterationPurity::KnownEffectsOnly;
    bool isInvariant = walker.isInvariantInSpecializedVersion(object, purity);
    Reference isObject;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isInvariant, purity);
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(object);
        test.setTestedType(TypeTable::object);
        isObject = walker.build(WTF::move(test));
        if (!isObject || walker.hasFailed())
            return { };
    }
    walker.recordSpeculation();
    bool isAskedBeforeLoop = isInvariant && walker.tryBranchInPreheader(isObject, true, purity);
    if (!isAskedBeforeLoop) {
        BlockID leaves = walker.addBlock(BlockFrequency::Rare);
        BlockID goesOn = walker.addBlock();
        walker.branch(isObject, goesOn, leaves);
        walker.continueIn(leaves);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(goesOn);
        if (walker.isUnreachable() || walker.hasFailed())
            return { };
    }
    Walker::SpecializedPreheaderScope preheaderScope(walker, isAskedBeforeLoop, purity);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(object);
    proof.setTestOperand(isObject);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    return walker.build(WTF::move(proof));
}

static bool callAccessorInIterationInWalkCalls(Walker& walker, const BodyValue& read, Reference object, Reference stored, FunctionID getter, LayoutID receiver, TypeID closures, Origin origin)
{
    if (!walker.canBuildIterationGuardAfterStores() || !walker.canRestartIteration() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::TypedOperations))
        return false;
    static constexpr auto purity = Walker::IterationPurity::KnownEffectsOnly;
    if (TypeID expected = receiver ? walker.types().forLayout(receiver) : TypeID(); expected && !walker.types().isSubset(walker.typeOf(object), expected) && walker.types().overlap(walker.typeOf(object), expected)) {
        guardExpectedType(walker, read.objectOperand(), expected, origin);
        if (walker.hasFailed() || walker.isUnreachable())
            return true;
        object = walker.referenceOf(read.objectOperand());
        if (!object || !object.isValue())
            return true;
    }
    if (!walker.types().at(walker.typeOf(object)).hasObjects())
        return false;
    if (!walker.types().at(walker.typeOf(object)).isObject()) {
        object = narrowToObjectInIterationInWalkCalls(walker, object, origin);
        if (!object || walker.hasFailed() || walker.isUnreachable())
            return true;
    }
    bool isInvariant = walker.isInvariantObjectInSpecializedVersion(object, purity);
    bool isTestedBeforeLoop = isInvariant && walker.canHoistAccessorLookup(read.key());
    bool isFoundBeforeLoop = isTestedBeforeLoop || (isInvariant && walker.opensGuesses() && walker.canFindAccessorInPreheader(read.key()));
    Reference found;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isFoundBeforeLoop, purity);
        Draft lookup(stored ? Opcode::GetCachedSetter : Opcode::GetCachedGetter, origin);
        walker.chooseConservativeFields(lookup);
        lookup.setObjectOperand(object);
        lookup.setKey(read.key());
        lookup.setIsBeforeLoop(isFoundBeforeLoop);
        lookup.setAsksRuntimeOnMiss(!isFoundBeforeLoop);
        found = walker.build(WTF::move(lookup));
        if (!found || walker.hasFailed())
            return true;
    }
    Reference isGetter;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isTestedBeforeLoop, purity);
        Draft test(Opcode::TypeTest, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(found);
        test.setTestedType(closures);
        isGetter = walker.build(WTF::move(test));
        if (!isGetter || walker.hasFailed())
            return true;
    }
    walker.recordSpeculation();
    walker.count(stored ? StatisticID::SettersExpectedInLoops : StatisticID::GettersExpectedInLoops);
    if (isFoundBeforeLoop)
        walker.count(StatisticID::SiteCachedReadsHoisted);
    bool isAskedBeforeLoop = isTestedBeforeLoop && walker.tryBranchInPreheader(isGetter, true, purity);
    if (!isAskedBeforeLoop) {
        BlockID leaves = walker.addBlock(BlockFrequency::Rare);
        BlockID goesOn = walker.addBlock();
        walker.branch(isGetter, goesOn, leaves);
        walker.continueIn(leaves);
        if (!walker.isUnreachable())
            walker.restartIteration();
        walker.continueIn(goesOn);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }
    Reference closure;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isAskedBeforeLoop, purity);
        Draft proof(Opcode::Narrowed, origin);
        walker.chooseConservativeFields(proof);
        proof.setValueOperand(found);
        proof.setTestOperand(isGetter);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        closure = walker.build(WTF::move(proof));
        if (!closure || walker.hasFailed())
            return true;
    }
    if (callAccessor(walker, read, getter, object, stored, closure))
        return true;
    Draft call(Opcode::Call, origin);
    call.setCalleeOperand(closure);
    call.setThisValueOperand(object);
    if (stored) {
        call.setArgumentsOperands(singleElementSpan(stored));
        walker.build(WTF::move(call));
        return true;
    }
    call.inputValueGiven = read.id();
    walker.bind(read.id(), walker.build(WTF::move(call)));
    return true;
}

static ALWAYS_INLINE std::span<const LinkedGetterAtRead> findGettersExpectedInCurrentFunctionInWalkCalls(Walker& walker)
{
    Walker::GettersExpectedInFunction& expected = walker.gettersExpectedInFunction;
    if (FunctionID function = walker.currentFunction(); expected.function != function) {
        auto reads = std::ranges::equal_range(walker.facts().held().gettersExpectedAtReads, function, { }, &LinkedGetterAtRead::function);
        expected = { function, std::span<const LinkedGetterAtRead>(reads.begin(), reads.end()) };
    }
    return expected.reads;
}

std::span<const LinkedGetterAtRead> gettersExpectedInCurrentFunction(Walker& walker)
{
    return findGettersExpectedInCurrentFunctionInWalkCalls(walker);
}

struct AccessorInWalkCalls {
    explicit operator bool() const { return !!accessor; }

    FunctionID accessor;
    LayoutID receiver;
};

static AccessorInWalkCalls findAccessorUnderCapturedKeyInWalkCalls(Walker& walker, const BodyValue& access)
{
    const Vector<LinkedAccessorOfLayout>& laid = walker.facts().held().accessorsOfLayouts;
    if (laid.isEmpty() || !walker.isInCopyOfAccessor() || !access.key() || liesInThread(access.key()))
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    Opcode written = index.opcodeOf(access.id());
    if (written != Opcode::ElemGet && written != Opcode::ElemSet)
        return { };
    Reference object = walker.referenceOf(access.objectOperand());
    auto layoutOf = [&](TypeID type) {
        TypeID objects = type ? walker.types().withoutNullish(type) : TypeID();
        return objects && !isNeverType(objects) ? singleLayoutOf(walker.types(), walker.types().at(objects)) : LayoutID();
    };
    LayoutID layout = object ? layoutOf(walker.typeOf(object)) : LayoutID();
    if (!layout)
        layout = layoutOf(findExpectation(walker, access.objectOperand()).type);
    if (!layout)
        return { };
    bool isSetter = access.opcode() == Opcode::PropSet;
    for (const LinkedAccessorOfLayout& accessor : std::ranges::equal_range(laid, std::pair { layout, isSetter }, { }, [](const LinkedAccessorOfLayout& accessor) { return std::pair { accessor.layout, accessor.isSetter }; })) {
        if (accessor.key ? accessor.key == access.key() : accessor.keysAreListed || (!findField(walker.program(), layout, access.key()) && !std::ranges::contains(walker.facts().held().appendedKeysOf(layout), access.key()) && !std::ranges::contains(walker.facts().linked().appendedKeysOf(layout), access.key())))
            return { accessor.accessor, layout };
    }
    return { };
}

static bool fieldHoldsValuesInWalkCalls(Walker& walker, Reference object, PropertyKeyID key)
{
    if (!object || !object.isValue())
        return false;
    TypeID type = walker.types().withoutNullish(walker.typeOf(object));
    LayoutID layout = isNeverType(type) ? LayoutID() : singleLayoutOf(walker.types(), walker.types().at(type));
    std::optional<FieldReference> field = layout && key && !liesInThread(key) ? findField(walker.program(), layout, key) : std::nullopt;
    const LinkedCell* cell = field ? walker.facts().linked().findCellOfField(*field) : nullptr;
    if (!cell || cell->isFilledByAccessorsAlone || cell->holdsAccessorFromBirth)
        return false;
    return std::ranges::none_of(std::ranges::equal_range(walker.facts().held().accessorsOfLayouts, layout, { }, &LinkedAccessorOfLayout::layout), [](const LinkedAccessorOfLayout& accessor) {
        return !accessor.key;
    });
}

static AccessorInWalkCalls findAccessorSoughtBeforeLoopsInWalkCalls(Walker& walker, const BodyValue& access)
{
    if (access.isFreeName() || fieldHoldsValuesInWalkCalls(walker, walker.referenceOf(access.objectOperand()), access.key()))
        return { };
    std::span<const LinkedGetterAtRead> expected = gettersExpectedInCurrentFunction(walker);
    auto listed = std::ranges::lower_bound(expected, access.id(), { }, &LinkedGetterAtRead::site);
    if (listed != expected.end() && listed->site == access.id() && listed->isSoughtBeforeLoops)
        return { listed->getter, listed->receiver };
    return findAccessorUnderCapturedKeyInWalkCalls(walker, access);
}

bool readsExpectedGetter(Walker& walker, const BodyValue& read)
{
    return read.opcode() == Opcode::PropGet && !!findAccessorSoughtBeforeLoopsInWalkCalls(walker, read);
}

bool storesThroughExpectedSetter(Walker& walker, const BodyValue& store)
{
    return store.opcode() == Opcode::PropSet && !!findAccessorSoughtBeforeLoopsInWalkCalls(walker, store);
}

bool callsResultOfExpectedGetter(Walker& walker, const BodyValue& call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = call.opcode() == Opcode::Call ? call.calleeOperand() : ValueID();
    return read && !index.isParameter(read) && index.onlyUsingValue(read) == call.id() && readsExpectedGetter(walker, index.value(read));
}

static bool callAccessorBehindTestInWalkCalls(Walker& walker, const BodyValue& access, Reference object, Reference stored, FunctionID accessor, TypeID closures, const BodyBlock& block, uint32_t place, uint32_t end, bool isSoughtBeforeLoops, Origin origin)
{
    Draft lookup(stored ? Opcode::GetCachedSetter : Opcode::GetCachedGetter, origin);
    walker.chooseConservativeFields(lookup);
    lookup.setObjectOperand(object);
    lookup.setKey(access.key());
    Reference found = walker.build(WTF::move(lookup));
    if (!found)
        return true;
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(found);
    test.setTestedType(closures);
    Reference isAccessor = walker.build(WTF::move(test));
    if (!isAccessor)
        return true;

    BlockID calls = walker.addBlock();
    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.branch(isAccessor, calls, asWritten);
    walker.recordSpeculation();
    if (isSoughtBeforeLoops)
        walker.count(stored ? StatisticID::SettersExpectedInLoops : StatisticID::GettersExpectedInLoops);

    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, end);
    Walker::ArmsThatMeet arms;
    walker.walkClosedArm(asWritten, stretch, arms);
    if (walker.hasFailed())
        return true;

    for (uint32_t later = place; later < end; ++later)
        walker.bind(ValueID(block.firstValue().index() + later), Reference());
    walker.continueIn(calls);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(found);
    proof.setTestOperand(isAccessor);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    Reference closure = walker.build(WTF::move(proof));
    if (closure && !callAccessor(walker, access, accessor, object, stored, closure))
        walker.walkClosedArm(walker.currentBlock(), stretch, arms);
    else {
        if (end > place + 1 && !walker.isUnreachable() && !walker.hasFailed())
            walker.walkStretch(block.id(), place + 1, end);
        walker.listArm(stretch, arms);
    }
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

bool callSetterExpectedAtStore(Walker& walker, const BodyValue& store)
{
    if (store.opcode() != Opcode::PropSet || !walker.isInLoop() || !walker.has(Mechanism::DirectCalls))
        return false;
    auto [setter, receiver] = findAccessorSoughtBeforeLoopsInWalkCalls(walker, store);
    if (!setter || !bodyRunsWhenCalledInWalkCalls(walker.program().at(setter).kind))
        return false;
    Reference object = walker.referenceOf(store.objectOperand());
    Reference stored = walker.referenceOf(store.valueOperand());
    if (!object || !object.isValue() || !stored)
        return false;
    TypeID closures = walker.types().forFunction(setter);
    Origin origin = walker.originOf(store);
    origin.isTailCall = false;
    if (callAccessorInIterationInWalkCalls(walker, store, object, stored, setter, receiver, closures, origin))
        return true;
    if (!walker.types().at(walker.typeOf(object)).isObject() || !shouldOpenGuess(walker, object, closures, true, closedOperationsOfReadThatCallsGetter))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(store.id()));
    uint32_t place = store.id().index() - block.firstValue().index();
    return callAccessorBehindTestInWalkCalls(walker, store, object, stored, setter, closures, block, place, place + 1, true, origin);
}

bool callGetterExpectedAtRead(Walker& walker, const BodyValue& read)
{
    bool isByKey = read.opcode() == Opcode::ElemGet;
    if (read.opcode() != Opcode::PropGet && !isByKey)
        return false;
    std::span<const LinkedGetterAtRead> expected = findGettersExpectedInCurrentFunctionInWalkCalls(walker);
    bool mayBeUnderCapturedKey = !isByKey && !walker.facts().held().accessorsOfLayouts.isEmpty() && walker.isInLoop();
    if ((expected.empty() && !mayBeUnderCapturedKey) || (!isByKey && read.isFreeName()) || !walker.has(Mechanism::DirectCalls))
        return false;
    Reference object = walker.referenceOf(read.objectOperand());
    if (!object || !object.isValue() || (!isByKey && fieldHoldsValuesInWalkCalls(walker, object, read.key())))
        return false;
    auto listed = std::ranges::lower_bound(expected, read.id(), { }, &LinkedGetterAtRead::site);
    bool isListed = listed != expected.end() && listed->site == read.id() && (!listed->isHeardInLoopsAlone || walker.isInLoop());
    AccessorInWalkCalls found = isListed ? AccessorInWalkCalls { listed->getter, listed->receiver } : mayBeUnderCapturedKey ? findAccessorUnderCapturedKeyInWalkCalls(walker, read) : AccessorInWalkCalls();
    FunctionID getter = found.accessor;
    bool isSoughtBeforeLoops = !isListed || listed->isSoughtBeforeLoops;
    if (!getter || !bodyRunsWhenCalledInWalkCalls(walker.program().at(getter).kind))
        return false;
    TypeID closures = walker.types().forFunction(getter);
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    if (!isByKey && isSoughtBeforeLoops && callAccessorInIterationInWalkCalls(walker, read, object, { }, getter, found.receiver, closures, origin))
        return true;
    if (!walker.types().at(walker.typeOf(object)).isObject() || !shouldOpenGuess(walker, object, closures, true, closedOperationsOfReadThatCallsGetter))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t place = read.id().index() - block.firstValue().index();

    if (isByKey)
        return callGetterExpectedUnderKeyInWalkCalls(walker, read, object, getter, closures, block.id(), place, origin);
    return callAccessorBehindTestInWalkCalls(walker, read, object, { }, getter, closures, block, place, findEndOfArmsOfGetterInWalkCalls(walker, index, block, read, place), isSoughtBeforeLoops, origin);
}

bool callSingleInstanceGetter(Walker& walker, const BodyValue& read)
{
    FunctionID getter = findModuleScopeGetter(walker, read);
    if (!getter || !bodyRunsWhenCalledInWalkCalls(walker.program().at(getter).kind))
        return false;
    Reference object = walker.referenceOf(read.objectOperand());
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    ModuleID module = walker.program().at(getter).module;
    std::optional<SingleInstance> only = findSingleInstance(walker, module, origin);
    if (!only)
        return walker.hasFailed() || walker.isUnreachable();
    if (!only->isSingleInstance) {
        Reference closure = buildClosureOverInstance(walker, getter, only->instance, origin);
        return closure && callAccessor(walker, read, getter, object, { }, closure);
    }

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t place = read.id().index() - block.firstValue().index();
    BlockID calls = walker.addBlock();
    BlockID reads = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.branch(only->isSingleInstance, calls, reads);
    walker.recordSpeculation();
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Walker::ArmsThatMeet arms;
    walker.walkClosedArm(reads, stretch, arms);
    if (walker.hasFailed())
        return true;

    walker.continueIn(calls);
    Reference closure = buildClosureOverInstance(walker, getter, instanceAfterTest(walker, module, *only, origin), origin);
    if (closure && !callAccessor(walker, read, getter, object, { }, closure))
        walker.walkClosedArm(walker.currentBlock(), stretch, arms);
    else
        walker.listArm(stretch, arms);
    if (!walker.hasFailed())
        walker.joinArms(stretch, arms, meeting);
    return true;
}

bool callForwardedDirectly(Walker& walker, const BodyValue& call, const Draft& forwarded)
{
    if (!walker.has(Mechanism::DirectCalls))
        return false;
    CallInWalkCalls request;
    request.closure = forwarded.calleeOperand();
    request.callee = findOnlyCallee(walker, call, request.closure);
    if (!request.callee)
        return false;
    const Function& callee = walker.program().at(request.callee);
    if (!bodyRunsWhenCalledInWalkCalls(callee.kind))
        return false;

    Vector<Reference, 8> elements;
    if (forwarded.opcode == Opcode::Call)
        request.arguments = forwarded.argumentsOperands();
    else if (std::optional<Vector<Reference, 8>> known = elementsOfFreshList(walker, forwarded.listOperand())) {
        walker.checkWhatIsInSightOfList(forwarded.listOperand(), forwarded.origin());
        elements = WTF::move(*known);
        request.arguments = elements.span();
    } else {
        request.listOfUnknownLength = forwarded.listOperand();
        Origin originInFrontOfCall = forwarded.origin();
        originInFrontOfCall.isTailCall = false;
        bool isMadeHere = isFreshPristineArray(walker, request.listOfUnknownLength) && parametersAreReadOutOfListInWalkCalls(walker, callee, request.listOfUnknownLength);
        if (!isMadeHere && !parametersAreReadOutOfListMadeElsewhereInWalkCalls(walker, call, callee, request.listOfUnknownLength, originInFrontOfCall))
            return false;
        if (!readParametersOutOfListInWalkCalls(walker, callee, request.listOfUnknownLength, originInFrontOfCall, elements))
            return true;
        if (!isMadeHere)
            walker.count(StatisticID::SpreadCallsUnpackedToParameters);
        request.arguments = elements.span();
    }
    request.thisValue = forwarded.thisValueOperand();
    request.calledBuiltin = forwarded.calledBuiltin();
    return copyOrCallDirectlyInWalkCalls(walker, call, forwarded, request);
}

bool constructForwardedDirectly(Walker& walker, const BodyValue& call, const Draft& forwarded)
{
    if (!walker.has(Mechanism::DirectCalls) || forwarded.opcode != Opcode::Construct)
        return false;
    if (forwarded.newTargetOperand() && forwarded.newTargetOperand() != forwarded.calleeOperand())
        return false;
    CallInWalkCalls request;
    request.closure = forwarded.calleeOperand();
    request.callee = findOnlyCallee(walker, call, request.closure);
    if (!request.callee)
        return false;
    const Function& constructor = walker.program().at(request.callee);
    if (!isClassConstructorOf(constructor.kind) || constructor.has(FunctionFlags::HasThis))
        return false;
    if (inlineConstructionHandedOn(walker, call, forwarded, request.callee) || walker.hasFailed())
        return true;
    request.newTarget = request.closure;
    request.arguments = forwarded.argumentsOperands();
    request.isConstruction = true;
    return buildDirectCallInWalkCalls(walker, call, forwarded, request);
}

static FunctionID findFunctionThatRowsNameInWalkCalls(const Program& program, BuiltinID builtin)
{
    FunctionID named;
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(builtin)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, overload.libraryFunction());
        if (!held || !held->function || (named && named != held->function))
            return { };
        named = held->function;
    }
    return named;
}

static FunctionID findLibraryFunctionOfBuiltinInWalkCalls(const Program& program, BuiltinID builtin)
{
    FunctionID named = findFunctionThatRowsNameInWalkCalls(program, builtin);
    if (!named)
        return { };
    const Function& function = program.at(named);
    return bodyRunsWhenCalledInWalkCalls(function.kind) && function.captures.isEmpty() && !function.has(FunctionFlags::TakesCallee) ? named : FunctionID();
}

bool takesNothingOfFunctionObject(const Program& program, FunctionID named)
{
    const Function& function = program.at(named);
    return bodyRunsWhenCalledInWalkCalls(function.kind) && function.captures.isEmpty() && !function.has(FunctionFlags::TakesCallee);
}

LibraryFunctionsBehindBuiltin findLibraryFunctionsBehindBuiltin(const Walker& walker, TypeID typeOfCallee)
{
    LibraryFunctionsBehindBuiltin found;
    if (!typeOfCallee)
        return found;
    Type type = walker.types().at(typeOfCallee);
    BuiltinID named = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    if (!named)
        return found;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    bool everyRowNamesFunction = true;
    FunctionID runByFunctionObject;
    bool severalAreRunByFunctionObject = false;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(named))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, overload.libraryFunction());
        if (!held || !held->function) {
            everyRowNamesFunction = false;
            continue;
        }
        if (!found.named.contains(held->function))
            found.named.append(held->function);
        found.someSpecialCaseNamesFunction |= overload.flags.contains(BuiltinOverloadFlag::IsSpecialCase);
        RuntimeFunctionRole role = table.at(held->id).role;
        if (role != RuntimeFunctionRole::EngineBuiltin && role != RuntimeFunctionRole::HostBuiltin)
            continue;
        severalAreRunByFunctionObject |= runByFunctionObject && runByFunctionObject != held->function;
        runByFunctionObject = held->function;
    }
    if (everyRowNamesFunction && found.named.size() == 1)
        found.enteredByValue = found.named[0];
    else if (everyRowNamesFunction && !severalAreRunByFunctionObject)
        found.enteredByValue = runByFunctionObject;
    if (found.enteredByValue && !takesNothingOfFunctionObject(program, found.enteredByValue))
        found.enteredByValue = { };
    found.named.removeAllMatching([&](FunctionID function) {
        return !takesNothingOfFunctionObject(program, function);
    });
    return found;
}

FunctionID findLibraryFunctionBehindBuiltin(const Walker& walker, TypeID typeOfCallee)
{
    if (!typeOfCallee)
        return { };
    Type type = walker.types().at(typeOfCallee);
    BuiltinID builtin = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    return builtin ? findLibraryFunctionOfBuiltinInWalkCalls(walker.program(), walker.program().builtins().canonicalID(builtin)) : FunctionID();
}

static BuiltinID findBuiltinWithoutFunctionObjectInWalkCalls(const Walker& walker, TypeID typeOfCallee)
{
    if (!typeOfCallee)
        return { };
    Type type = walker.types().at(typeOfCallee);
    BuiltinID builtin = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    return builtin && requiresWalkLowering(walker.program().builtins(), builtin) ? builtin : BuiltinID();
}

FunctionID findFunctionBehindCompiledBuiltin(const Walker& walker, TypeID typeOfCallee)
{
    BuiltinID builtin = findBuiltinWithoutFunctionObjectInWalkCalls(walker, typeOfCallee);
    return builtin ? findFunctionThatRowsNameInWalkCalls(walker.program(), walker.program().builtins().canonicalID(builtin)) : FunctionID();
}

static bool trapAtCallThatLinkNeverMakesInWalkCalls(Walker& walker, const Draft& written)
{
    Origin origin = written.origin();
    origin.isTailCall = false;
    for (Reference argument : written.argumentsOperands()) {
        if (!argument || !argument.isValue())
            continue;
        walker.buildCheckOfProof(argument, TypeTable::never, TypeTable::anything, ProofSource::ResultOfCall, origin);
        walker.endAsUnreachable(origin);
        return true;
    }
    return false;
}

bool replaceCallOfCompiledBuiltin(Walker& walker, const BodyValue& call)
{
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!findBuiltinWithoutFunctionObjectInWalkCalls(walker, proven))
        return false;

    const Program& program = walker.program();
    CallInWalkCalls request;
    if (call.opcode() == Opcode::Call)
        request.callee = findFunctionBehindCompiledBuiltin(walker, proven);
    if (!request.callee || !bodyRunsWhenCalledInWalkCalls(program.at(request.callee).kind)) {
        walker.fail("a built-in without a function object is called, and its rows name no one function to enter"_s);
        return true;
    }

    Draft written = walker.draftOf(call);
    request.thisValue = written.thisValueOperand();
    request.arguments = written.argumentsOperands();
    request.calledBuiltin = written.calledBuiltin();
    request.isNeverInlined = written.isNeverInlined();
    request.calleeComesBack = true;
    if (walker.hasFailed() || buildDirectCallInWalkCalls(walker, call, written, request))
        return true;
    if (walker.facts().linked().function(request.callee).isReached || !trapAtCallThatLinkNeverMakesInWalkCalls(walker, written))
        walker.fail("the function behind a built-in without a function object does not take the call"_s);
    return true;
}

static_assert(opcodesThatCallDirectlyTakes == (opcodesOfWalkCalls));

bool callDirectlyOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    Opcode opcode = call.opcode();
    if (!opcodesOfWalkCalls.contains(opcode))
        return false;
    if (opcode == Opcode::Call)
        return walkCallInWalkCalls(walker, call);
    if (opcode == Opcode::Construct)
        return walkConstructInWalkCalls(walker, call);
    if (opcode == Opcode::SuperConstruct)
        return walkSuperConstructInWalkCalls(walker, call);
    if (opcode == Opcode::ConstructList || opcode == Opcode::SuperConstructList)
        return walkConstructionOfListInWalkCalls(walker, call);
    if (opcode == Opcode::CallList)
        return walkCallListInWalkCalls(walker, call);
    return walkDirectCallOfInputInWalkCalls(walker, call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
