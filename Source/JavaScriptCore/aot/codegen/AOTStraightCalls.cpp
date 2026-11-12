/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTLinkTables.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsValues.h"
#include "AOTTypeRelations.h"
#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DirectCallInStraightForm {
    CodeUnit callee;
    ArgumentLocations locations;
    uint32_t stackWords { 0 };
};

enum class StackWordsOfStraightCall : bool { InScratchArea, OfOwnCaller };

} // anonymous namespace

static constexpr OpcodeSet callsOfValuesInStraightForm { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::ConstructList };

OpcodeSet StraightEmitter::opcodesOfCalls()
{
    return { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::ConstructList, Opcode::CallDirect, Opcode::Results, Opcode::ResultAt };
}

static std::optional<ThrowSite> siteThatNamesCalleeInStraightForm(StraightEmitter& emitter, const BodyValue& value)
{
    StringID name = value.calleeName();
    if (!name)
        return std::nullopt;
    return ThrowSite {
        .kind = ThrowKind::NotAFunction,
        .a = emitter.mentions().add(NumberSpace::ShownString, name),
        .b = std::nullopt,
        .calleeIsReadFromReceiver = !!value.calleeKey(),
        .aIsMention = true,
    };
}

static bool mayForwardCallToReceiverInStraightForm(StraightEmitter& emitter, const BodyValue& call)
{
    if (!call.calleeKey() || !call.calleeName() || !call.thisValueOperand())
        return false;
    BuiltinID member = findOwnMember(emitter.program(), Builtins::FunctionPrototype, call.calleeKey());
    return member == Builtins::FunctionPrototypeCall || member == Builtins::FunctionPrototypeApply;
}

static void requireForwardedCallCanBeMadeInStraightForm(StraightEmitter& emitter, const BodyValue& call, const StraightCallOptions& options)
{
    StraightArgument list = StraightArgument::boxed(jsUndefined());
    if (call.opcode() == Opcode::Call && call.argumentsOperands().size() > 1 && findOwnMember(emitter.program(), Builtins::FunctionPrototype, call.calleeKey()) == Builtins::FunctionPrototypeApply)
        list = call.argumentsOperands()[1];
    emitter.callStub(call, throwUnlessForwardedCallCanBeMade(), { call.calleeOperand(), call.thisValueOperand(), list }, options);
}

static void generateCallByValue(StraightEmitter& emitter, const BodyValue& value)
{
    if (mayForwardCallToReceiverInStraightForm(emitter, value))
        requireForwardedCallCanBeMadeInStraightForm(emitter, value, { .throwSite = siteThatNamesCalleeInStraightForm(emitter, value) });
    std::span<const ValueID> arguments = value.argumentsOperands();
    unsigned count = static_cast<unsigned>(arguments.size());
    ValueID thisValue = value.thisValueOperand();
    bool passesThis = thisValue && !emitter.isUndefinedConstant(thisValue);
    CallPosition position = value.origin().isTailCall ? CallPosition::Tail : CallPosition::Ordinary;
    StraightCallOptions options { .throwSite = siteThatNamesCalleeInStraightForm(emitter, value) };
    if (count > numberOfArgumentGPRs) {
        StraightArgument countOperand = StraightArgument::word(count);
        std::optional<StraightArgument> receiver = thisValue ? StraightArgument(thisValue) : StraightArgument::boxed(jsUndefined());
        emitter.callByConvention(value, callValueWithCount(position), { &countOperand, 1 }, StraightArgument(value.calleeOperand()), receiver, arguments, options);
    } else if (passesThis)
        emitter.callByConvention(value, AOT::callValue(count, position), { }, StraightArgument(value.calleeOperand()), StraightArgument(thisValue), arguments, options);
    else
        emitter.callByConvention(value, callValueWithoutThis(count, position), { }, StraightArgument(value.calleeOperand()), std::nullopt, arguments, options);
    emitter.define(value);
}

static bool walkReplacesCallByValue(StraightEmitter& emitter, const BodyValue& value)
{
    const BodyIndex& index = emitter.index();
    BodyValue callee = index.value(index.stripCasts(value.calleeOperand()));
    if (callee.opcode() != Opcode::Builtin)
        return false;
    if (requiresWalkLowering(emitter.program().builtins(), callee.builtin()))
        return true;
    return value.opcode() == Opcode::Call && (value.isDoneInPlace() || value.callNumber());
}

static std::optional<ThrowSite> siteThatNamesConstructorInStraightForm(StraightEmitter& emitter, const BodyValue& value)
{
    StringID name = value.calleeName();
    if (!name)
        return std::nullopt;
    return emitter.throwSiteWithName(ThrowKind::NotAFunction, name);
}

static StraightArgument thisOrUndefinedInStraightForm(ValueID thisValue)
{
    return thisValue ? StraightArgument(thisValue) : StraightArgument::boxed(jsUndefined());
}

static void generateConstructByValue(StraightEmitter& emitter, const BodyValue& value)
{
    ValueID callee = value.calleeOperand();
    ValueID newTarget = value.newTargetOperand();
    Vector<ValueID, 8> arguments { newTarget ? newTarget : callee };
    arguments.append(value.argumentsOperands());
    StraightArgument count = StraightArgument::word(arguments.size());
    emitter.callByConvention(value, AOT::constructValue(ConstructSyntax::NewExpression), { &count, 1 }, StraightArgument(callee), StraightArgument::boxed(jsUndefined()), arguments.span(), { .throwSite = siteThatNamesConstructorInStraightForm(emitter, value), .constructs = true });
    emitter.define(value);
}

static void generateConstructWithList(StraightEmitter& emitter, const BodyValue& value)
{
    ValueID callee = value.calleeOperand();
    ValueID newTarget = value.newTargetOperand();
    emitter.callOperation(value, RuntimeOperation::ConstructWithArray, { callee, newTarget ? newTarget : callee, value.listOperand() }, { .constructs = true });
    emitter.define(value);
}

static void generateCallWithList(StraightEmitter& emitter, const BodyValue& value)
{
    if (mayForwardCallToReceiverInStraightForm(emitter, value))
        requireForwardedCallCanBeMadeInStraightForm(emitter, value, { .throwSite = siteThatNamesCalleeInStraightForm(emitter, value) });
    CallPosition position = value.origin().isTailCall ? CallPosition::Tail : CallPosition::Ordinary;
    StraightArgument list(value.listOperand());
    emitter.callByConvention(value, callWithArray(position), { &list, 1 }, StraightArgument(value.calleeOperand()), thisOrUndefinedInStraightForm(value.thisValueOperand()), { }, { .throwSite = siteThatNamesCalleeInStraightForm(emitter, value) });
    emitter.define(value);
}

static bool crossesStraightCall(const ArgumentLocation& location)
{
    if (location.kind == ArgumentLocation::Kind::None)
        return true;
    switch (location.representation) {
    case Representation::JSValue:
    case Representation::Cell:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Boolean:
    case Representation::Double:
        return true;
    default:
        return false;
    }
}

static TypeID typeAskedOfOperandOfStraightCall(StraightEmitter& emitter, FunctionID callee, size_t entryParameter, const ArgumentLocation& location)
{
    if (location.kind == ArgumentLocation::Kind::None)
        return { };
    bool isUnboxed = location.representation == Representation::Int32 || location.representation == Representation::Double || location.representation == Representation::Boolean;
    if (!isUnboxed && location.representation != Representation::JSValue && location.representation != Representation::Cell)
        return { };
    uint32_t index = static_cast<uint32_t>(entryParameter);
    TypeID promised = emitter.facts().typeBelievedOfEntryParameter(emitter.types(), callee, index, emitter.facts().typeOfEntryParameter(callee, index).proven);
    if (promised && !emitter.types().at(promised).isJavaScriptValue())
        promised = { };
    if (!promised)
        return isUnboxed ? widestTypeOf(location.representation) : TypeID();
    return emitter.types().testablePartOf(promised);
}

static bool constantIsOfTypeAskedOfStraightCall(StraightEmitter& emitter, JSValue constant, TypeID asked)
{
    uint16_t primitives = holdsOnlyPrimitives(asked) ? primitivesInID(asked) : emitter.types().recordOf(asked).primitives;
    if (!constant)
        return primitives & Primitive::Absent;
    if (constant.isUndefined())
        return primitives & Primitive::Undefined;
    return constant.isInt32() && (primitives & Primitive::Integer);
}

static bool canAskOperandOfStraightCall(StraightEmitter& emitter, TypeID asked)
{
    if (!emitter.canTestType(asked))
        return false;
    return !emitter.facts().testMentionsWhatIsOutOfReach(emitter.types(), asked, scopedLambda<bool(FunctionID)>([&](FunctionID named) {
        return emitter.facts().linked().mayBeMentionedBy(emitter.unit(), named);
    }));
}

static void testOperandsOfStraightCall(StraightEmitter& emitter, const BodyValue& call, FunctionID callee, std::span<const StraightArgument> operands, const ArgumentLocations& locations)
{
    for (size_t index = 0; index < operands.size(); ++index) {
        if (operands[index].kind != StraightArgument::Kind::Value)
            continue;
        if (TypeID asked = typeAskedOfOperandOfStraightCall(emitter, callee, index, locations.parameters[index]))
            emitter.trapUnlessOfType(operands[index].value, asked, ProofSource::EntryParameter, call.origin());
    }
}

static std::optional<DirectCallInStraightForm> planStraightDirectCall(StraightEmitter& emitter, const BodyValue& value)
{
    const Program& program = emitter.program();
    const CallingConventions& conventions = emitter.conventions();
    CodeUnit callee = conventions.directEntryOf(value.function());
    if (!program.at(callee.function).hasBody() || !conventions.exists(callee))
        return std::nullopt;
    const LinkTables& linked = emitter.facts().linked();
    if (!linked.functions.isEmpty() && !linked.mayBeMentionedBy(emitter.unit(), callee.function))
        return std::nullopt;

    DirectCallInStraightForm call { callee, conventions.locationsOf(program, callee) };
    std::span<const ValueID> arguments = value.argumentsOperands();
    if (arguments.size() != call.locations.parameters.size())
        return std::nullopt;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const ArgumentLocation& location = call.locations.parameters[index];
        if (!crossesStraightCall(location) || (location.kind != ArgumentLocation::Kind::None && !arguments[index]))
            return std::nullopt;
        if (TypeID asked = typeAskedOfOperandOfStraightCall(emitter, callee.function, index, location); asked && !canAskOperandOfStraightCall(emitter, asked))
            return std::nullopt;
    }
    for (const ArgumentLocation& result : call.locations.results) {
        if (!crossesStraightCall(result) || result.kind == ArgumentLocation::Kind::Stack)
            return std::nullopt;
        if (call.locations.results.size() > 1 && result.kind == ArgumentLocation::Kind::FPR)
            return std::nullopt;
    }
    call.stackWords = std::max(conventions.stackWordsReservedByCallersOf(program, callee), call.locations.numberOfStackWords());
    if (call.stackWords > emitter.wordsOfScratchArea())
        return std::nullopt;
    return call;
}

static void checkArgumentsOfStraightDirectCall(StraightEmitter& emitter, std::span<const StraightArgument> arguments, const ArgumentLocations& locations)
{
    constexpr FPRReg numberAgain = StraightEmitter::conversionFPR;
    CCallHelpers& jit = emitter.jit();
    AllowMacroScratchRegisterUsage allowScratch(jit);
    CCallHelpers::JumpList differs;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const ArgumentLocation& location = locations.parameters[index];
        if (location.kind != ArgumentLocation::Kind::GPR && location.kind != ArgumentLocation::Kind::FPR)
            continue;
        GPRReg again = jit.scratchRegister();
        emitter.bring(arguments[index], again);
        if (location.kind == ArgumentLocation::Kind::GPR) {
            emitter.unboxIntoForm(location.representation, again);
            differs.append(jit.branch64(CCallHelpers::NotEqual, again, location.reg.gpr()));
            continue;
        }
        emitter.unboxNumber(again, numberAgain);
        CCallHelpers::Jump isSame = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, numberAgain, location.reg.fpr());
        differs.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, numberAgain, numberAgain));
        differs.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, location.reg.fpr(), location.reg.fpr()));
        isSame.link(&jit);
    }
    if (differs.empty())
        return;
    CCallHelpers::Jump isRight = jit.jump();
    differs.link(&jit);
    jit.breakpoint();
    isRight.link(&jit);
}

static void bringArgumentsOfStraightDirectCall(StraightEmitter& emitter, const BodyValue& value, const ArgumentLocations& locations, StackWordsOfStraightCall stackWords)
{
    std::span<const ValueID> arguments = value.argumentsOperands();
    Vector<StraightArgument, 8> operands;
    for (ValueID argument : arguments)
        operands.append(argument ? StraightArgument(argument) : StraightArgument::boxed(jsUndefined()));
    testOperandsOfStraightCall(emitter, value, value.function(), operands.span(), locations);
    if (emitter.hasFailed())
        return;
    CCallHelpers& jit = emitter.jit();
    for (ArgumentLocation::Kind kind : { ArgumentLocation::Kind::Stack, ArgumentLocation::Kind::FPR, ArgumentLocation::Kind::GPR }) {
        for (size_t index = 0; index < arguments.size(); ++index) {
            const ArgumentLocation& location = locations.parameters[index];
            if (location.kind != kind)
                continue;
            if (kind == ArgumentLocation::Kind::GPR) {
                emitter.load(arguments[index], location.reg.gpr());
                emitter.unboxIntoForm(location.representation, location.reg.gpr());
                continue;
            }
            GPRReg word = emitter.loadIntoResultRegister(arguments[index]);
            if (kind == ArgumentLocation::Kind::FPR) {
                emitter.unboxNumber(word, location.reg.fpr());
                continue;
            }
            CCallHelpers::Address destination = stackWords == StackWordsOfStraightCall::OfOwnCaller
                ? CCallHelpers::Address(GPRInfo::callFrameRegister, offsetOfStackWordFromFrame(location.stackIndex))
                : emitter.wordOfScratchArea(location.stackIndex);
            if (location.representation == Representation::Double) {
                FPRReg number = emitter.takeResultFPR();
                emitter.unboxNumber(word, number);
                jit.storeDouble(number, destination);
            } else {
                emitter.unboxIntoForm(location.representation, word);
                jit.store64(word, destination);
            }
        }
    }
    if (emitter.build().options().recorded.validates) [[unlikely]]
        checkArgumentsOfStraightDirectCall(emitter, operands.span(), locations);
}

static bool bodyRunsWhenCalledInStraightForm(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    default:
        return false;
    }
}

static std::optional<DirectCallInStraightForm> planStraightCallOfOnlyCallee(StraightEmitter& emitter, const BodyValue& value, Vector<StraightArgument, 8>& operands, ValueID* checkThatPasses = nullptr)
{
    const Program& program = emitter.program();
    const CallingConventions& conventions = emitter.conventions();
    const LinkTables& linked = emitter.facts().linked();
    const BodyIndex& index = emitter.index();
    if (value.opcode() != Opcode::Call || linked.functions.isEmpty())
        return std::nullopt;

    FunctionID only = emitter.facts().functionThatCallEntersByBinding(index, value, checkThatPasses);
    if (!only || only != linked.calleeAt(emitter.function(), value.id()) || !linked.mayBeMentionedBy(emitter.unit(), only))
        return std::nullopt;

    const Function& function = program.at(only);
    if (!function.hasBody() || !canBeCalledByValueOf(function.role) || function.has(FunctionCapabilities::RecordsCallee) || function.results.size() != 1)
        return std::nullopt;
    if (!bodyRunsWhenCalledInStraightForm(function.kind) || function.isCoroutine() || !function.captures.isEmpty())
        return std::nullopt;
    CodeUnit callee = conventions.directEntryOf(only);
    if (!conventions.exists(callee))
        return std::nullopt;

    std::span<const ValueID> arguments = value.argumentsOperands();
    unsigned numberOfPositionalParameters = function.numberOfPositionalParameters();
    unsigned nextParameter = 0;
    operands.shrink(0);
    for (unsigned entryParameter = 0; entryParameter < function.numberOfEntryParameters(); ++entryParameter) {
        switch (function.entryParameterKind(entryParameter)) {
        case EntryParameter::This:
            operands.append(thisOrUndefinedInStraightForm(value.thisValueOperand()));
            break;
        case EntryParameter::NewTarget:
            operands.append(StraightArgument::boxed(jsUndefined()));
            break;
        case EntryParameter::ArgumentCount:
            operands.append(StraightArgument::boxed(jsNumber(static_cast<int32_t>(arguments.size()))));
            break;
        case EntryParameter::Parameter:
            if (nextParameter >= numberOfPositionalParameters)
                return std::nullopt;
            if (nextParameter < arguments.size())
                operands.append(StraightArgument(arguments[nextParameter]));
            else if (TypeID written = function.parameters[nextParameter].written(); written && (program.at(written).primitives & Primitive::Absent))
                operands.append(StraightArgument::boxed(JSValue()));
            else
                operands.append(StraightArgument::boxed(jsUndefined()));
            ++nextParameter;
            break;
        case EntryParameter::Capture:
        case EntryParameter::Callee:
        case EntryParameter::HiddenRest:
            return std::nullopt;
        }
    }

    DirectCallInStraightForm call { callee, conventions.locationsOf(program, callee) };
    if (operands.size() != call.locations.parameters.size() || call.locations.results.size() != 1)
        return std::nullopt;
    for (size_t place = 0; place < operands.size(); ++place) {
        const ArgumentLocation& location = call.locations.parameters[place];
        if (!crossesStraightCall(location))
            return std::nullopt;
        if (location.kind == ArgumentLocation::Kind::None)
            continue;
        if (operands[place].kind == StraightArgument::Kind::Value) {
            if (!operands[place].value)
                return std::nullopt;
            if (TypeID asked = typeAskedOfOperandOfStraightCall(emitter, only, place, location); asked && !canAskOperandOfStraightCall(emitter, asked))
                return std::nullopt;
            continue;
        }
        JSValue constant = JSValue::decode(static_cast<EncodedJSValue>(operands[place].bits));
        if (TypeID asked = typeAskedOfOperandOfStraightCall(emitter, only, place, location); asked && !constantIsOfTypeAskedOfStraightCall(emitter, constant, asked))
            return std::nullopt;
        bool isInt32 = constant.isInt32();
        if (location.kind != ArgumentLocation::Kind::GPR || (location.representation != Representation::JSValue && !(location.representation == Representation::Int32 && isInt32)))
            return std::nullopt;
    }
    if (!crossesStraightCall(call.locations.results[0]) || call.locations.results[0].kind == ArgumentLocation::Kind::Stack)
        return std::nullopt;
    call.stackWords = std::max(conventions.stackWordsReservedByCallersOf(program, callee), call.locations.numberOfStackWords());
    if (call.stackWords)
        return std::nullopt;
    return call;
}

bool StraightEmitter::mayCallOnlyCalleeDirectly(const BodyValue& value, ValueID& checkThatPasses)
{
    Vector<StraightArgument, 8> operands;
    return !!planStraightCallOfOnlyCallee(*this, value, operands, &checkThatPasses);
}

static void bringOperandsOfStraightCallOfOnlyCallee(StraightEmitter& emitter, const BodyValue& value, FunctionID callee, std::span<const StraightArgument> operands, const ArgumentLocations& locations)
{
    if (emitter.build().options().recorded.validates) [[unlikely]]
        emitter.jit();
    testOperandsOfStraightCall(emitter, value, callee, operands, locations);
    if (emitter.hasFailed())
        return;
    for (ArgumentLocation::Kind kind : { ArgumentLocation::Kind::FPR, ArgumentLocation::Kind::GPR }) {
        for (size_t index = 0; index < operands.size(); ++index) {
            const ArgumentLocation& location = locations.parameters[index];
            if (location.kind != kind)
                continue;
            if (kind == ArgumentLocation::Kind::FPR) {
                emitter.unboxNumber(emitter.loadIntoResultRegister(operands[index].value), location.reg.fpr());
                continue;
            }
            emitter.bring(operands[index], location.reg.gpr());
            emitter.unboxIntoForm(location.representation, location.reg.gpr());
        }
    }
    if (emitter.build().options().recorded.validates) [[unlikely]]
        checkArgumentsOfStraightDirectCall(emitter, operands, locations);
}

static bool isReadByPartsRightBehindAlone(StraightEmitter& emitter, const BodyValue& producer, size_t numberOfParts)
{
    uint32_t numberOfPartsRead = 0;
    for (uint32_t index = 0; index < numberOfParts; ++index)
        numberOfPartsRead += emitter.resultIsRead(producer, index);
    return numberOfPartsRead == emitter.index().useCount(producer.id());
}

static void defineResultsOfStraightDirectCall(StraightEmitter& emitter, const BodyValue& value, std::span<const ArgumentLocation> results)
{
    CCallHelpers& jit = emitter.jit();
    if (results.size() == 1) {
        if (results[0].kind == ArgumentLocation::Kind::None)
            jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), emitter.takeResultRegister());
        else if (results[0].kind == ArgumentLocation::Kind::FPR)
            emitter.boxDouble(emitter.numberOfCall(), emitter.takeResultRegister());
        else
            emitter.boxInForm(results[0].representation, emitter.resultOfCall());
        emitter.define(value);
        return;
    }

    AllowMacroScratchRegisterUsage allowScratch(jit);
    for (uint32_t index = 0; index < results.size(); ++index) {
        const ArgumentLocation& result = results[index];
        if (!emitter.resultIsRead(value, index))
            continue;
        GPRReg boxed = jit.scratchRegister();
        if (result.kind == ArgumentLocation::Kind::None)
            jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), boxed);
        else if (result.kind == ArgumentLocation::Kind::FPR)
            emitter.boxDouble(result.reg.fpr(), boxed);
        else {
            boxed = result.reg.gpr();
            emitter.boxInForm(result.representation, boxed);
        }
        emitter.defineResult(value, index, boxed);
    }
}

static void generateStraightDirectCall(StraightEmitter& emitter, const BodyValue& value)
{
    std::optional<DirectCallInStraightForm> call = planStraightDirectCall(emitter, value);
    if (!call || (call->locations.results.size() > 1 && !isReadByPartsRightBehindAlone(emitter, value, call->locations.results.size()))) {
        emitter.fail(value);
        return;
    }
    bringArgumentsOfStraightDirectCall(emitter, value, call->locations, StackWordsOfStraightCall::InScratchArea);
    uint32_t site = emitter.addSite(value.origin(), { }, value.opcode());
    emitter.noteSiteMayThrow(site);
    emitter.records().stubCalls.callFunction(emitter.jit(), call->callee, primaryEntry, OptionalIndex { site });
    defineResultsOfStraightDirectCall(emitter, value, call->locations.results.span());
}

static void generateStraightCallOfOnlyCallee(StraightEmitter& emitter, const BodyValue& value)
{
    Vector<StraightArgument, 8> operands;
    std::optional<DirectCallInStraightForm> call = planStraightCallOfOnlyCallee(emitter, value, operands);
    RELEASE_ASSERT(call);
    bringOperandsOfStraightCallOfOnlyCallee(emitter, value, call->callee.function, operands.span(), call->locations);
    uint32_t site = emitter.addSite(value.origin(), { }, value.opcode());
    emitter.noteSiteMayThrow(site);
    emitter.records().stubCalls.callFunction(emitter.jit(), call->callee, primaryEntry, OptionalIndex { site });
    defineResultsOfStraightDirectCall(emitter, value, call->locations.results.span());
}

static void generateStraightResults(StraightEmitter& emitter, const BodyValue& value)
{
    std::span<const ValueID> values = value.valuesOperands();
    if (!isReadByPartsRightBehindAlone(emitter, value, values.size())) {
        emitter.fail(value);
        return;
    }
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (!emitter.resultIsRead(value, index))
            continue;
        emitter.defineResult(value, index, emitter.loadIntoResultRegister(values[index]));
    }
}

static bool isMadeAsFunctionInStraightForm(StraightEmitter& emitter, ValueID callee)
{
    const BodyIndex& index = emitter.index();
    BodyValue made = index.value(index.stripCasts(callee));
    if (made.opcode() != Opcode::Builtin)
        return made.opcode() == Opcode::NewClosure;
    const BuiltinTable& builtins = emitter.program().builtins();
    return isCallableBuiltin(builtins, builtins.canonicalID(made.builtin()));
}

static bool namesBuiltinInStraightForm(StraightEmitter& emitter, ValueID callee)
{
    const BodyIndex& index = emitter.index();
    return index.value(index.stripCasts(callee)).opcode() == Opcode::Builtin;
}

static void requireCallableBeforeStraightTailCall(StraightEmitter& emitter, const BodyValue& call)
{
    Origin origin = call.origin();
    origin.isTailCall = true;
    StraightCallOptions options { .origin = origin, .throwSite = siteThatNamesCalleeInStraightForm(emitter, call), .isTailCall = true };
    if (mayForwardCallToReceiverInStraightForm(emitter, call))
        requireForwardedCallCanBeMadeInStraightForm(emitter, call, options);
    else if (!isMadeAsFunctionInStraightForm(emitter, call.calleeOperand()))
        emitter.callStub(call, throwUnlessCallable(), { call.calleeOperand() }, options);
}

static void leaveByCallOfValue(StraightEmitter& emitter, const BodyValue& call)
{
    StubCalls& stubCalls = emitter.records().stubCalls;
    std::span<const ValueID> arguments = call.argumentsOperands();
    unsigned count = static_cast<unsigned>(arguments.size());
    ValueID thisValue = call.thisValueOperand();
    if (count > numberOfArgumentGPRs) {
        for (unsigned index = 0; index < count; ++index)
            emitter.storeInScratchArea(arguments[index], index);
        const StubSignature& signature = emitter.catalog().summaryOf(tailCallWithList()).signature;
        StraightArgument operands[] = { call.calleeOperand(), thisOrUndefinedInStraightForm(thisValue), StraightArgument::word(count), StraightArgument::addressInScratchArea(0), StraightArgument::word(0) };
        RELEASE_ASSERT(!signature.usesCallConvention && signature.numberOfOperands == std::size(operands));
        for (size_t index = 0; index < std::size(operands); ++index)
            emitter.bring(operands[index], signature.operands[index].gpr());
        stubCalls.tailCall(emitter.jit(), tailCallWithList());
        return;
    }

    bool passesThis = thisValue && !emitter.isUndefinedConstant(thisValue);
    for (unsigned index = 0; index < count; ++index)
        emitter.load(arguments[index], argumentGPR(index));
    emitter.load(call.calleeOperand(), calleeGPR);
    if (passesThis)
        emitter.load(thisValue, thisGPR);
    emitter.tearDownFrame();
    stubCalls.tailCall(emitter.jit(), passesThis ? AOT::callValue(count, CallPosition::Tail) : callValueWithoutThis(count, CallPosition::Tail));
}

static void leaveByCallWithList(StraightEmitter& emitter, const BodyValue& call)
{
    StubKey stub = callWithArray(CallPosition::Tail);
    const StubSignature& signature = emitter.catalog().summaryOf(stub).signature;
    RELEASE_ASSERT(signature.usesCallConvention && signature.numberOfOperands == 1);
    emitter.load(call.listOperand(), signature.operands[0].gpr());
    emitter.load(call.calleeOperand(), calleeGPR);
    emitter.bring(thisOrUndefinedInStraightForm(call.thisValueOperand()), thisGPR);
    emitter.tearDownFrame();
    emitter.records().stubCalls.tailCall(emitter.jit(), stub);
}

static bool tryLeaveByDirectCall(StraightEmitter& emitter, const BodyValue& call)
{
    const CallingConventions& conventions = emitter.conventions();
    std::optional<DirectCallInStraightForm> planned = planStraightDirectCall(emitter, call);
    if (!planned || conventions.numberOfResults(planned->callee) != 1 || !haveSameRegisterRepresentation(planned->locations.results[0].representation, conventions.resultRepresentation(emitter.unit())))
        return false;

    CCallHelpers& jit = emitter.jit();
    StubCalls& stubCalls = emitter.records().stubCalls;
    if (!planned->stackWords || planned->stackWords <= conventions.stackWordsReservedByCallersOf(emitter.program(), emitter.unit())) {
        bringArgumentsOfStraightDirectCall(emitter, call, planned->locations, StackWordsOfStraightCall::OfOwnCaller);
        emitter.tearDownFrame();
        stubCalls.jumpToFunction(jit, planned->callee, primaryEntry);
        return true;
    }
    bringArgumentsOfStraightDirectCall(emitter, call, planned->locations, StackWordsOfStraightCall::InScratchArea);
    stubCalls.moveAddressOfFunction(planned->callee, primaryEntry, valueEntryGPR);
    if (planned->stackWords <= StubKey::maxCount) {
        stubCalls.tailCall(jit, AOT::callFromArgumentListFrameWithWords(planned->stackWords, 0));
        return true;
    }
    emitMoveStackWordsToArgumentListFrame(jit, planned->stackWords, 0);
    stubCalls.tailCall(jit, AOT::callFromArgumentListFrame());
    return true;
}

static bool tryLeaveByCallOfOnlyCallee(StraightEmitter& emitter, const BodyValue& call)
{
    Vector<StraightArgument, 8> operands;
    std::optional<DirectCallInStraightForm> planned = planStraightCallOfOnlyCallee(emitter, call, operands);
    RELEASE_ASSERT(planned);
    if (!haveSameRegisterRepresentation(planned->locations.results[0].representation, emitter.conventions().resultRepresentation(emitter.unit())))
        return false;
    bringOperandsOfStraightCallOfOnlyCallee(emitter, call, planned->callee.function, operands.span(), planned->locations);
    emitter.tearDownFrame();
    emitter.records().stubCalls.jumpToFunction(emitter.jit(), planned->callee, primaryEntry);
    return true;
}

bool StraightEmitter::coversReturnCalls()
{
    return true;
}

void StraightEmitter::generateReturnCall(const BodyTerminator& terminator)
{
    BodyValue call = m_index.value(terminator.callOperand());
    if (plan().moduleEvaluation().moduleThatBeginsIn(function())) {
        fail(call);
        return;
    }

    for (TailCast cast : terminator.as<TerminatorKind::ReturnCall>().casts()) {
        if (cast.declared.has(PositionFlags::IsInt32Promise)) {
            fail(call);
            return;
        }
    }

    bool mayLeave = returnsResultOfFunction(m_unit.role) && !m_block.hasHandler();
    switch (call.opcode()) {
    case Opcode::Call:
    case Opcode::CallList:
        if (callsOnlyCalleeDirectly(call.id())) {
            if (mayLeave && tryLeaveByCallOfOnlyCallee(*this, call))
                return;
            break;
        }
        if (mayLeave && haveSameRegisterRepresentation(Representation::JSValue, conventions().resultRepresentation(m_unit)) && !namesBuiltinInStraightForm(*this, call.calleeOperand())) {
            requireCallableBeforeStraightTailCall(*this, call);
            if (call.opcode() == Opcode::Call)
                leaveByCallOfValue(*this, call);
            else
                leaveByCallWithList(*this, call);
            return;
        }
        break;
    case Opcode::CallDirect:
        if (mayLeave && tryLeaveByDirectCall(*this, call))
            return;
        if (conventions().numberOfResults(conventions().directEntryOf(call.function())) > 1) {
            fail(call);
            return;
        }
        break;
    default:
        fail(call);
        return;
    }

    generateCall(call);
    if (m_hasFailed)
        return;
    returnToCaller(call.id(), terminator.origin());
}

void StraightEmitter::generateCall(const BodyValue& value)
{
    if (callsOfValuesInStraightForm.contains(value.opcode()) && walkReplacesCallByValue(*this, value)) {
        fail(value);
        return;
    }

    switch (value.opcode()) {
    case Opcode::Call:
        if (callsOnlyCalleeDirectly(value.id()))
            generateStraightCallOfOnlyCallee(*this, value);
        else
            generateCallByValue(*this, value);
        return;
    case Opcode::Construct:
        generateConstructByValue(*this, value);
        return;
    case Opcode::CallList:
        generateCallWithList(*this, value);
        return;
    case Opcode::ConstructList:
        generateConstructWithList(*this, value);
        return;
    case Opcode::CallDirect:
        generateStraightDirectCall(*this, value);
        return;
    case Opcode::Results:
        generateStraightResults(*this, value);
        return;
    case Opcode::ResultAt: {
        ValueID producer = value.resultsOperand();
        bool standsRightBehind = currentBlock().holds(producer);
        for (uint32_t between = producer.index() + 1; standsRightBehind && between < value.id().index(); ++between) {
            BodyValue other = index().value(ValueID(between));
            standsRightBehind = other.opcode() == Opcode::ResultAt && other.resultsOperand() == producer;
        }
        if (!standsRightBehind)
            fail(value);
        return;
    }
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
