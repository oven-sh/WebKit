/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

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

static void generateCallByValue(StraightEmitter& emitter, const BodyValue& value)
{
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

// replaceOperation() of the walk, which nothing withholds: a call done in place, a call that makes the symbol of a key, a built-in without a function object.
static bool walkReplacesCallByValue(StraightEmitter& emitter, const BodyValue& value)
{
    const BodyIndex& index = emitter.index();
    BodyValue callee = index.value(index.valueBehindCasts(value.calleeOperand()));
    if (callee.opcode() != Opcode::Builtin)
        return false;
    if (isCallThatTheWalkMustLower(emitter.program().builtins(), callee.builtin()))
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
    case Representation::Boolean:
    case Representation::Double:
        return true;
    default:
        return false;
    }
}

static std::optional<DirectCallInStraightForm> planStraightDirectCall(StraightEmitter& emitter, const BodyValue& value)
{
    const Program& program = emitter.program();
    const CallingConventions& conventions = emitter.conventions();
    CodeUnit callee = conventions.directEntryOf(value.function());
    if (!program.at(callee.function).hasBody() || !conventions.exists(callee) || conventions.of(callee.function).hasSplitResult)
        return std::nullopt;
    // A block that the scan has passed over names what Link has not heard of.
    const LinkTables& linked = emitter.facts().linked();
    if (!linked.functions.isEmpty() && !linked.mayBeMentionedBy(emitter.function(), callee.function))
        return std::nullopt;

    DirectCallInStraightForm call { callee, conventions.locationsOf(program, callee) };
    std::span<const ValueID> arguments = value.argumentsOperands();
    if (arguments.size() != call.locations.parameters.size())
        return std::nullopt;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const ArgumentLocation& location = call.locations.parameters[index];
        if (!crossesStraightCall(location) || (location.kind != ArgumentLocation::Kind::None && !arguments[index]))
            return std::nullopt;
    }
    for (const ArgumentLocation& result : call.locations.results) {
        if (!crossesStraightCall(result) || result.kind == ArgumentLocation::Kind::Stack)
            return std::nullopt;
    }
    call.stackWords = std::max(conventions.stackWordsReservedByCallersOf(program, callee), call.locations.numberOfStackWords());
    if (call.stackWords > emitter.wordsOfScratchArea())
        return std::nullopt;
    return call;
}

// The stack words and the numbers go through the result registers, which are argument registers too: so the GPRs come last.
static void bringArgumentsOfStraightDirectCall(StraightEmitter& emitter, const BodyValue& value, const ArgumentLocations& locations, StackWordsOfStraightCall stackWords)
{
    constexpr GPRReg word = StraightEmitter::resultGPR;
    constexpr FPRReg number = StraightEmitter::resultFPR;
    CCallHelpers& jit = emitter.jit();
    std::span<const ValueID> arguments = value.argumentsOperands();
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
            emitter.load(arguments[index], word);
            if (kind == ArgumentLocation::Kind::FPR) {
                emitter.unboxNumber(word, location.reg.fpr());
                continue;
            }
            CCallHelpers::Address destination = stackWords == StackWordsOfStraightCall::OfOwnCaller
                ? CCallHelpers::Address(GPRInfo::callFrameRegister, offsetOfStackWordFromFrame(location.stackIndex))
                : emitter.wordOfScratchArea(location.stackIndex);
            if (location.representation == Representation::Double) {
                emitter.unboxNumber(word, number);
                jit.storeDouble(number, destination);
            } else {
                emitter.unboxIntoForm(location.representation, word);
                jit.store64(word, destination);
            }
        }
    }
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
            jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), StraightEmitter::resultGPR);
        else if (results[0].kind == ArgumentLocation::Kind::FPR)
            emitter.boxDouble(StraightEmitter::resultFPR, StraightEmitter::resultGPR);
        else
            emitter.boxInForm(results[0].representation, StraightEmitter::resultGPR);
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
        emitter.load(values[index], StraightEmitter::resultGPR);
        emitter.defineResult(value, index, StraightEmitter::resultGPR);
    }
}

static bool mayForwardCallToReceiverInStraightForm(StraightEmitter& emitter, const BodyValue& call)
{
    if (!call.calleeKey() || !call.calleeName() || !call.thisValueOperand())
        return false;
    BuiltinID member = findOwnMember(emitter.program(), Builtins::FunctionPrototype, call.calleeKey());
    return member == Builtins::FunctionPrototypeCall || member == Builtins::FunctionPrototypeApply;
}

static bool isMadeAsFunctionInStraightForm(StraightEmitter& emitter, ValueID callee)
{
    const BodyIndex& index = emitter.index();
    BodyValue made = index.value(index.valueBehindCasts(callee));
    if (made.opcode() != Opcode::Builtin)
        return made.opcode() == Opcode::NewClosure;
    const BuiltinTable& builtins = emitter.program().builtins();
    return isCallableBuiltin(builtins, builtins.canonicalID(made.builtin()));
}

// The stub that calls finds out too late that there is nothing to call: the frame that the error names is gone by then, and with it the words that say what the program calls the callee.
static void requireCallableBeforeStraightTailCall(StraightEmitter& emitter, const BodyValue& call)
{
    Origin origin = call.origin();
    origin.isTailCall = true;
    StraightCallOptions options { .origin = origin, .throwSite = siteThatNamesCalleeInStraightForm(emitter, call), .isTailCall = true };
    if (mayForwardCallToReceiverInStraightForm(emitter, call))
        emitter.callStub(call, throwUnlessForwardedCalleeIsCallable(), { call.calleeOperand(), call.thisValueOperand() }, options);
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
        // The stub lays the list where this frame stands, whatever room the caller of the unit has left.
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

// False: nothing was emitted. The callee returns in another form than the unit, or the call has no straight form at all.
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
    emitMoveStackWordsToArgumentListFrame(jit, planned->stackWords, 0);
    stubCalls.moveAddressOfFunction(planned->callee, primaryEntry, valueEntryGPR);
    stubCalls.tailCall(jit, AOT::callFromArgumentListFrame());
    return true;
}

bool StraightEmitter::coversReturnCalls()
{
    return true;
}

void StraightEmitter::generateReturnCall(const BodyTerminator& terminator)
{
    BodyValue call = m_index.value(terminator.callOperand());
    // FinishModuleEvaluation stands in front of each return of such a body.
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
        // Whatever is entered by value returns one boxed value.
        if (mayLeave && haveSameRegisterRepresentation(Representation::JSValue, conventions().resultRepresentation(m_unit)) && !walkReplacesCallByValue(*this, call)) {
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
    load(call.id(), resultGPR);
    returnToCaller();
}

void StraightEmitter::generateCall(const BodyValue& value)
{
    if (callsOfValuesInStraightForm.contains(value.opcode()) && walkReplacesCallByValue(*this, value)) {
        fail(value);
        return;
    }

    switch (value.opcode()) {
    case Opcode::Call:
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
