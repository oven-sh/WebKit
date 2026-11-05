/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// At a suspension: nothing. A direct call of a coroutine enters its ramp, which returns like any other function.
// At a throw: a direct call throws under the handler of the block being walked, as the call that it stands for. A call that may throw in front of the callee's body stays closed.
// Withheld: every call is copied in its closed form, and the walk's core leaves "anything" for the calls that Link lists.
// Never withheld: replaceCallOfCompiledBuiltin(). A built-in without a function object has no closed form, so a direct call of the function that its rows name is that form.

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
#include "AOTWalkKnown.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkBirths.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

static constexpr OpcodeSet opcodesOfWalkCalls { Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::CallList, Opcode::CallDirect, Opcode::ConstructList, Opcode::SuperConstructList };
// As many arguments as a call can write out.
static constexpr int64_t lengthOfShortListInWalkCalls = defaultLimitValue(LimitID::MaxReservedStackArguments, hostArchitecture());
static constexpr OpcodeSet appendsToListInWalkCalls { Opcode::AppendElement, Opcode::AppendSpread };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendSpread::array);

namespace {

struct CallInWalkCalls {
    FunctionID callee;
    // Null: the call has no function object in hand, so the callee may take nothing of one.
    Reference closure;
    Reference thisValue;
    // In place of `thisValue`, for planOperandsInWalkCalls(): the object is made once the plan stands.
    TypeID typeOfThisToBeMade;
    Reference newTarget;
    std::span<const Reference> arguments;
    // In place of `arguments`: a list that was made for this call alone. The callee has no positional parameter and does not count its arguments.
    Reference restList;
    // The list of a CallList, where only the run shows how long it is. The call as written lays it out on the stack, and throws in front of the callee where it finds no room there.
    Reference listOfUnknownLength;
    BuiltinID calledBuiltin;
    bool isConstruction { false };
    bool isNeverInlined { false };
    // It comes back to its caller whatever it is handed. So in tail position a frame that stays below it costs one frame, and the call need not leave by a jump.
    bool calleeComesBack { false };
};

enum class OperandSourceInWalkCalls : uint8_t { Given, Capture, Undefined, Absent, ArgumentCount, ListOfOtherArguments, SharedEmptyList };

struct OperandInWalkCalls {
    OperandSourceInWalkCalls source { OperandSourceInWalkCalls::Undefined };
    // Of a capture: what the NewClosure in sight lists. Null: it is read off the closure.
    Reference given;
    uint32_t capture { 0 };
    // Of what is given and not made yet.
    TypeID typeToBeGiven;
};

} // anonymous namespace

// ---- Who is called

// Link says who is entered, not that anybody is: what is called may be no function, and a bound function or a proxy in front of the callee holds none of its captures.
// So its word counts only where all that the value can be are named closures.
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
    const LinkTables& link = walker.facts().linked();
    FunctionID listed = link.calleeAt(walker.currentFunction(), call.id());
    // The function whose body this is names the one callee of its call, and whoever has a copy of the body leads to that function or to all that it names.
    if (FunctionID only = singleFunctionOf(types, type))
        return only == listed || link.mayBeMentionedBy(walker.unit(), only) ? only : FunctionID();

    if (!listed || !type.hasOnlyClosures() || !namesFunction(types, type, listed))
        return { };
    return listed;
}

void checkProofOfOnlyCallee(Walker& walker, Reference callee, FunctionID only, Origin origin)
{
    if (!walker.checksProofs() || !callee || !callee.isValue())
        return;
    TypeArena& types = walker.types();
    TypeID proven = walker.typeOf(callee);
    Type type = types.at(proven);
    if (type.isObject() && type.hasOnlyClosures() && !singleFunctionOf(types, type))
        walker.checkProof(callee, types.forFunction(only), proven, ProofSource::Callee, origin);
}

// ---- What may be entered

// A call of the constructor of a class throws in front of the body.
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

// Where they do not, nothing that is entered can return in place of the unit: the entry by value of a callee that returns a raw value calls it and boxes, and a unit that returns a raw value takes no boxed one.
// No ring of calls in tail position has such a pair (LinkedFunction::ringsOfTailCallsAreListed), so the frame that stays is one of a bounded number.
static bool unitsReturnInOneFormInWalkCalls(Walker& walker, CodeUnit entered)
{
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit unit = walker.unit();
    if (conventions.numberOfResults(unit) != 1 || conventions.numberOfResults(entered) != 1)
        return true;
    return haveSameRegisterRepresentation(conventions.resultRepresentation(entered), conventions.resultRepresentation(unit));
}

// CallEmitter::emitTailCall() asks the same of the two units. Where the answer is no the call stays one of the value, which leaves by a stub.
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

// ---- Laying out the operands

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

static TypeID typeHandedOverInWalkCalls(Walker& walker, const OperandInWalkCalls& operand)
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
    case OperandSourceInWalkCalls::SharedEmptyList:
        return walker.types().builtinClass(BuiltinClasses::Array);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasProvenWhatGuardTestsInWalkCalls(Walker& walker, FunctionID callee, unsigned entryParameter, Reference handedOver)
{
    TypeID tested = walker.plan().conventions().typeBehindGuard(callee, entryParameter);
    if (!tested)
        return true;
    TypeID own = handedOver && handedOver.isRecord() ? walker.code().at(handedOver.id()).type : TypeID();
    return own && walker.types().isSubset(own, tested);
}

// Walker::build() stops the compile at an operand that does not fit, so whatever it would turn away is turned away here, where the closed form can still take the call.
static bool bodyTakesOperandInWalkCalls(Walker& walker, CodeUnit entered, unsigned entryParameter, const OperandInWalkCalls& operand)
{
    // A closure holds a capture in the form in which the body takes it.
    if (operand.source == OperandSourceInWalkCalls::Capture)
        return true;
    TypeID handedOver = typeHandedOverInWalkCalls(walker, operand);
    if (!handedOver)
        return false;
    TypeArena& types = walker.types();
    Type type = types.at(handedOver);
    if (type.mayBeUnspecified())
        return false;
    // requiredTypeOfOperand() of a CallDirect. Only the entry behind the guard asks for what the guard tests.
    TypingContext typing { walker.program(), types, entered.role == CodeRole::Body ? walker.positions() : PositionTypes::knowingNothing(), TypeLevel::Proven };
    if (!walker.positions().entryParameterIsLeftOutOfCalls(entered.function, entryParameter)) {
        TypeID required = declaredEntryTypeOf(typing, entered.function, entryParameter);
        if (required && !types.isSubset(handedOver, required))
            return false;
    }
    // What the callee takes for proven to its end is the record's own type, which never ends. What the state knows beyond that has no witness in Code.
    if (entered.role == CodeRole::Body && operand.source == OperandSourceInWalkCalls::Given && !hasProvenWhatGuardTestsInWalkCalls(walker, entered.function, entryParameter, operand.given))
        return false;
    Representation taken = walker.plan().conventions().parameterRepresentation(walker.program(), entered, entryParameter);
    return !hasRegister(taken) || isLegalRepresentation(type, taken);
}

// It builds nothing.
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
            if (request.restList)
                return false;
            operand.source = Source::ArgumentCount;
            break;
        case EntryParameter::Parameter:
            if (nextParameter >= numberOfPositionalParameters)
                operand = request.restList ? OperandInWalkCalls { Source::Given, request.restList, 0 } : OperandInWalkCalls { Source::ListOfOtherArguments, { }, 0 };
            else if (request.restList)
                return false;
            else if (nextParameter < request.arguments.size())
                operand = { Source::Given, request.arguments[nextParameter], 0 };
            else if (TypeID writtenType = callee.parameters[nextParameter].written(); writtenType && (walker.types().at(writtenType).primitives & Primitive::Absent))
                operand.source = Source::Absent;
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            if (request.restList)
                return false;
            // The language has no name for the list, and a body only reads it out into `arguments`. So nothing tells one empty list from another, and no cell is made for the call.
            operand.source = request.arguments.size() > numberOfPositionalParameters ? Source::ListOfOtherArguments : Source::SharedEmptyList;
            break;
        }
        if (operand.source == Source::ListOfOtherArguments && request.arguments.size() > numberOfPositionalParameters + maxComputedElementsOfArrayLiteral)
            return false;
        if (!bodyTakesOperandInWalkCalls(walker, entered, index, operand))
            return false;
        operands.append(operand);
    }
    return true;
}

// Null: the walk has failed.
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
        // No operation of the input stands behind it, so buildFreshObject() never comes to it.
        noteFreshList(walker, built);
        return built;
    }
    case OperandSourceInWalkCalls::SharedEmptyList: {
        Draft list(Opcode::EmptyArgumentList, origin);
        walker.chooseConservativeFields(list);
        list.setValueType(walker.types().builtinClass(BuiltinClasses::Array));
        return walker.build(WTF::move(list));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A callee that is handed the list itself, or nothing of it, would take a list of any length. So a long list goes by the call as written, out of the way, and the walk goes on where the list is short.
// `behindCall`: where the two ways meet, with what the call gives as its parameter. Null: the unit returns that at once, and the long way has done so.
// False: the walk cannot go on.
static bool buildCallAsWrittenForLongListInWalkCalls(Walker& walker, const Draft& written, const CallInWalkCalls& request, Origin originInFrontOfCall, BlockID& behindCall, Reference& resultBehindCall)
{
    Draft length(Opcode::GetArrayLength, originInFrontOfCall);
    walker.chooseConservativeFields(length);
    length.setArrayOperand(request.listOfUnknownLength);
    Draft lengthOfShortList(Opcode::Integer, originInFrontOfCall);
    lengthOfShortList.setInteger(lengthOfShortListInWalkCalls);

    Draft test(Opcode::LessEq, originInFrontOfCall);
    walker.chooseConservativeFields(test);
    test.setLeftOperand(walker.build(WTF::move(length)));
    test.setRightOperand(walker.constant(WTF::move(lengthOfShortList)));
    if (!test.leftOperand() || !test.rightOperand())
        return false;
    if (walker.has(Mechanism::TypedOperations))
        test.setForm(formAllowedForTypedOperator(walker, test));
    Reference isShort = walker.build(WTF::move(test));
    if (!isShort)
        return false;

    BlockID shortList = walker.addBlock();
    BlockID longList = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isShort, shortList, longList);

    walker.continueIn(longList);
    Draft asWritten = written;
    walker.chooseConservativeFields(asWritten);
    Reference result = walker.build(WTF::move(asWritten));
    walker.arrivals().leaveAnything(request.callee);
    if (walker.unitReturnsAtOnce(written))
        walker.returnAtOnce(result);
    else {
        behindCall = walker.addBlock();
        resultBehindCall = walker.addParameter(behindCall, { }, Representation::JSValue);
        if (result)
            walker.jumpTo(behindCall, singleElementSpan(result));
    }
    walker.continueIn(shortList);
    return !walker.isUnreachable();
}


Reference callSlowVersion(Walker& walker, const Draft& site, FunctionID callee, std::span<const Reference> entryArguments, bool isConstruction)
{
    Draft direct(Opcode::CallDirect, site.origin());
    direct.expected = site.expected;
    direct.resultIsReturnedAtOnce = site.resultIsReturnedAtOnce;
    direct.inputValueGiven = site.inputValueGiven;
    direct.setFunction(callee);
    direct.setRole(CodeRole::SlowBody);
    if (site.opcode == Opcode::Call)
        direct.setCalledBuiltin(site.calledBuiltin());
    direct.setIsConstruction(isConstruction);
    direct.setIsNeverInlined(true);
    direct.setVariadicOperands(entryArguments);
    Effects effects = effectsOfDirectCall(walker, callee, entryArguments);
    return walker.build(WTF::move(direct), &effects);
}

// `written`: the draft of the call as it stands in the input. False: nothing was built.
static bool buildDirectCallInWalkCalls(Walker& walker, const BodyValue& call, const Draft& written, const CallInWalkCalls& request)
{
    const Function& callee = walker.program().at(request.callee);
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit entered = conventions.entryBehindGuardOf(request.callee);
    if (!admitsDirectEntryInWalkCalls(walker, callee, entered))
        return false;
    if (!request.calleeComesBack && !walker.plan().endsItsTailCalls(request.callee) && isReturnedByTailCallInWalkCalls(walker.currentBodyIndex(), call.id()) && unitsReturnInOneFormInWalkCalls(walker, entered) && !unitMayLeaveByCallInWalkCalls(walker, entered))
        return false;
    // A caller that has proven its arguments enters behind the guard. Every other enters at the guard.
    Vector<OperandInWalkCalls, 8> planned;
    if (!planOperandsInWalkCalls(walker, request, callee, entered, planned)) {
        if (!conventions.hasParameterGuard(request.callee))
            return false;
        entered = conventions.directEntryOf(request.callee);
        planned.shrink(0);
        if (!planOperandsInWalkCalls(walker, request, callee, entered, planned))
            return false;
    }

    Origin originInFrontOfCall = written.origin();
    originInFrontOfCall.isTailCall = false;
    checkProofOfOnlyCallee(walker, request.closure, request.callee, originInFrontOfCall);
    BlockID behindCall;
    Reference resultBehindCall;
    if (request.listOfUnknownLength && !buildCallAsWrittenForLongListInWalkCalls(walker, written, request, originInFrontOfCall, behindCall, resultBehindCall))
        return true;

    Vector<Reference, 8> operands;
    operands.reserveInitialCapacity(planned.size());
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, originInFrontOfCall, operand);
        if (!built)
            return true;
        operands.append(built);
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
    direct.setVariadicOperands(operands.span());
    Effects effects = effectsOfDirectCall(walker, request.callee, operands.span());
    Reference result = walker.build(WTF::move(direct), &effects);
    if (result)
        walker.leaveArrivals(request.callee, operands.span());
    if (behindCall) {
        if (result)
            walker.jumpTo(behindCall, singleElementSpan(result));
        else
            walker.continueIn(behindCall);
        result = walker.isUnreachable() ? Reference() : resultBehindCall;
    }
    if (result)
        walker.bind(call.id(), result);
    return true;
}

// ---- Helpers of the compiler

// A function of the library that the walk enters where no operation of the body calls it. Link has entered it for anybody: ScanOfBody::readIteration().
bool canCallCompilerHelper(Walker& walker, FunctionID helper)
{
    if (!helper || !walker.has(Mechanism::DirectCalls) || !walker.facts().linked().function(helper).isReached)
        return false;
    const Function& callee = walker.program().at(helper);
    if (!bodyRunsWhenCalledInWalkCalls(callee.kind) || !callee.captures.isEmpty() || callee.has(FunctionFlags::TakesCallee))
        return false;
    return admitsDirectEntryInWalkCalls(walker, callee, walker.plan().conventions().directEntryOf(helper));
}

// Null: no way leads on.
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

// ---- The operations

// walkConstructInWalkCalls() has a form for it, or the emitter goes straight to the unit that makes `this`: CallEmitter::generateConstruct().
static bool isConstructedWithoutStubInWalkCalls(const Walker& walker, FunctionID id)
{
    const Function& constructor = walker.program().at(id);
    if (isClassConstructorOf(constructor.kind))
        return !constructor.has(FunctionFlags::HasThis);
    return makesThisOfConstruction(walker, id) || walker.plan().conventions().exists(CodeUnit { id, CodeRole::AllocatingConstructEntry });
}

// Whoever asks for the test below binds the callee back when the call is built.
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

// What is called is proven to be a closure of one function or no object at all: `p.m()`, where `F.prototype.m = function () { .. }` may not have run yet. A call of what is no object throws.
// The same of one built-in function: `var is = Object.is`, read by a function that may run in front of the statement.
// So one test stands in front of the call, nothing comes back from the side that fails it, and behind it the callee is known as the closure. False: nothing was built.
// The callee is bound to a value that stands on one side of a branch, and a binding lasts for the rest of the walk: whoever asks for the test binds the callee back when the call is built.
// `var NodeConstructor; function start() { NodeConstructor = Node; }   new NodeConstructor(kind)`: a construction of what is no object throws as well.
static bool testCalleeThatMayBeNoObjectInWalkCalls(Walker& walker, const BodyValue& call)
{
    bool isConstruction = call.opcode() == Opcode::Construct;
    ValueID written = call.calleeOperand();
    Reference callee = walker.referenceOf(written);
    TypeID proven = callee && callee.isValue() ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return false;
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    if (!type.isJavaScriptValue() || !type.hasObjects() || type.isObject() || type.hasUnboundedUnknown())
        return false;
    TypeID closures = types.withOnlyObjects(proven);
    FunctionID only = singleFunctionOf(types, types.at(closures));
    auto isCalledByRowsOfBuiltin = [&] {
        if (!walker.has(Mechanism::BuiltinCalls) || !singleBuiltinOf(types, types.at(closures)))
            return false;
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
        if (!only || !isConstructedWithoutStubInWalkCalls(walker, only))
            return false;
    } else if (only ? !bodyRunsWhenCalledInWalkCalls(walker.program().at(only).kind) : !isCalledByRowsOfBuiltin())
        return false;

    Draft asWritten = walker.draftOf(call);
    Origin origin = asWritten.origin();
    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(callee);
    test.setTestedType(closures);
    Reference isClosure = walker.build(WTF::move(test));
    if (!isClosure)
        return true;
    BlockID calls = walker.addBlock();
    BlockID throws = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isClosure, calls, throws);

    walker.continueIn(throws);
    bool returnsAtOnce = walker.unitReturnsAtOnce(asWritten);
    walker.chooseConservativeFields(asWritten);
    Reference neverGiven = walker.build(WTF::move(asWritten));
    if (returnsAtOnce)
        walker.returnAtOnce(neverGiven);
    else
        walker.endAsUnreachable(origin);

    walker.continueIn(calls);
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
    // What may be any object at all names no function, with the primitives beside it or without them.
    if (walker.nothingIsProvenOf(request.closure))
        return false;
    request.callee = findOnlyCallee(walker, call, request.closure);

    BindingOfCalleeInWalkCalls bindingOfCallee { walker, { }, walker.referenceBoundTo(call.as<Opcode::Call>().calleeOperand()) };
    if (!request.callee && testCalleeThatMayBeNoObjectInWalkCalls(walker, call)) {
        bindingOfCallee.input = call.as<Opcode::Call>().calleeOperand();
        if (walker.isUnreachable() || (walker.has(Mechanism::BuiltinCalls) && callBuiltin(walker, call)) || (walker.has(Mechanism::Inlining) && inlineCallee(walker, call)))
            return true;
        request.closure = walker.referenceOf(call.as<Opcode::Call>().calleeOperand());
        request.callee = findOnlyCallee(walker, call, request.closure);
    }
    if (!request.callee || !bodyRunsWhenCalledInWalkCalls(walker.program().at(request.callee).kind))
        return false;

    Draft written = walker.draftOf(call);
    request.thisValue = written.thisValueOperand();
    Vector<Reference, 8> arguments(written.argumentsOperands());
    if (walker.depthOfCopies()) {
        for (Reference& argument : arguments)
            argument = walker.closureMadeWhereCopyHandsItOn(call, argument);
    }
    request.arguments = arguments.span();
    request.calledBuiltin = written.calledBuiltin();
    request.isNeverInlined = written.isNeverInlined();
    return buildDirectCallInWalkCalls(walker, call, written, request);
}

// The objects of the class are compiled objects, and whoever constructs the function makes one and hands it over. As constructEntryAllocatesThisOf() of plan/AOTCallingConventions.cpp asks, but for two things that are about entries by value alone:
// a function of the library, which the run time constructs by name, and a function that reads new.target, for which the run time makes `this` in front of the entry, have no unit that allocates. Here the body is entered directly, with both.
bool makesThisOfConstruction(const Walker& walker, FunctionID id)
{
    const Function& constructor = walker.program().at(id);
    if (!constructor.takesThisFromConstruction() || constructor.isCoroutine() || !constructor.has(FunctionFlags::HasThis) || constructor.results.size() != 1 || !constructor.constructedClass)
        return false;
    const Program& program = walker.program();
    const SlotPlan& slots = walker.plan().slots();
    LayoutID declared = declaredLayoutOf(program, constructor.constructedClass);
    return declared && program.at(declared).has(LayoutFlags::IsConstructorFunction) && slots.holdsCompiledObjects(constructor.constructedClass) && !slots.isNeverAllocated(constructor.constructedClass);
}

// The class whose class object is all that the value can be.
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

// `new F(..)` of a function that is constructed: what the unit AllocatingConstructEntry does, in the place of the call: BuilderOfEntries::allocateThis() and returnWhatWasConstructed(). new.target is the closure itself.
// So the walk knows the object by its layout, the body is entered directly or copied, and nothing is asked behind it where the body is proven to return no object.
// `class Below extends F { constructor() { super(..) } }`: `classObjectBelow`: new.target of the `super(..)`, the one class object of a class that is written below F: the object is one of that class.
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

    // The call gives what the body returns, which is not what the construction gives.
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
    // What is constructed is a closure of one plain function. Its `prototype` is a data property of its own that nothing can turn into an accessor, so the read runs nobody's code; the run time may make the object at the first read.
    Effects ofRead = declaredBoundOf(Opcode::NewObject);
    ofRead.reads.add(AbstractHeap::NamedProperties);
    // A class that is evaluated once has one prototype object, which an allocation without the operand names: buildTypedConstructionOfItsOpcodes().
    bool allocationNamesPrototype = classObjectBelow && walker.facts().isEvaluatedAtMostOnce(constructed) && walker.plan().hasClassRecord(constructed);
    Reference prototype = allocationNamesPrototype ? Reference() : walker.build(WTF::move(read), &ofRead);
    if (!prototype && !allocationNamesPrototype)
        return true;
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(constructed);
    if (prototype)
        allocation.setPrototypeOperand(prototype);
    walker.chooseConservativeFields(allocation);
    Reference object = walker.build(WTF::move(allocation));
    if (!object)
        return true;
    request.thisValue = object;
    request.typeOfThisToBeMade = { };
    // Nothing else names the cell yet: it is not frozen, no field lies outside its slot, and its order word lists nothing. Whether it stands below the prototype object of its layout is for whoever stores to ask: `F.prototype` can be stored into.
    noteFreshObject(walker, object, 0);

    // Copied, the body stores into an object that the walk has in sight.
    bool isCopied = false;
    if (walker.has(Mechanism::Inlining) && !constructor.isHiddenInStackTraces() && !constructor.has(FunctionFlags::HasRestParameter)) {
        planned.shrink(0);
        if (!planOperandsInWalkCalls(walker, request, constructor, entered, planned)) {
            walker.fail("the body of a constructor function does not take the object that was made for it"_s);
            return true;
        }
        // Beside the object and what the call hands over they are constants, which the direct call finds again.
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

    // The construction is over: whoever sees the object from here on may ask whether it is finished.
    Draft finish(Opcode::TryFinishBirth, origin);
    finish.setObjectOperand(object);
    walker.chooseConservativeFields(finish);
    if (lookMayFinishBirthOf(walker, object))
        walker.build(WTF::move(finish));

    TypeID provenOfBody = walker.facts().linked().function(request.callee).result;
    TypeID ofWhatCameBack = walker.typeOf(returned);
    if ((provenOfBody && !types.overlap(provenOfBody, TypeTable::object)) || (ofWhatCameBack && !types.overlap(ofWhatCameBack, TypeTable::object))) {
        walker.bind(call.id(), object);
        return true;
    }

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

// The construction stays as written. Where the type names the constructor it goes straight to the unit that makes `this` (CallEmitter::generateConstruct()), and what that unit makes is known: `new Node(kind)`, where the tag picks one of several layouts.
// `isBehindTest`: the callee is bound to what the test has left of it, which lasts no longer than the caller. False: nothing is known, and nothing was built.
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

// The constructor of a class makes its own object, and what it returns is what was constructed.
static bool walkConstructInWalkCalls(Walker& walker, const BodyValue& call)
{
    auto operation = call.as<Opcode::Construct>();
    // Whether another new.target can construct is for the closed form to ask.
    if (operation.newTargetOperand() && operation.newTargetOperand() != operation.calleeOperand())
        return false;

    CallInWalkCalls request;
    request.closure = walker.referenceOf(operation.calleeOperand());
    request.callee = findOnlyCallee(walker, call, request.closure);
    BindingOfCalleeInWalkCalls bindingOfCallee { walker, { }, walker.referenceBoundTo(operation.calleeOperand()) };
    if (!request.callee && !walker.nothingIsProvenOf(request.closure) && testCalleeThatMayBeNoObjectInWalkCalls(walker, call)) {
        bindingOfCallee.input = operation.calleeOperand();
        // The link of the chain that stands in front of this one for a construction, and that has passed what was no closure for sure.
        if (walker.isUnreachable() || (walker.has(Mechanism::EscapeAnalysis) && walker.opensGuesses() && listStoresOfCopiedConstructor(walker, call)))
            return true;
        request.closure = walker.referenceOf(operation.calleeOperand());
        request.callee = findOnlyCallee(walker, call, request.closure);
    }
    bool isBehindTest = !!bindingOfCallee.input;
    if (!request.callee)
        return constructAsWrittenInWalkCalls(walker, call, { }, isBehindTest);
    const Function& constructor = walker.program().at(request.callee);
    if (!isClassConstructorOf(constructor.kind))
        return walkConstructOfFunctionInWalkCalls(walker, call, request) || constructAsWrittenInWalkCalls(walker, call, operation.newTargetOperand() ? FunctionID() : request.callee, isBehindTest);
    if (constructor.has(FunctionFlags::HasThis))
        return constructAsWrittenInWalkCalls(walker, call, { }, isBehindTest);

    Draft written = walker.draftOf(call);
    request.newTarget = request.closure;
    request.arguments = written.argumentsOperands();
    request.isConstruction = true;
    return buildDirectCallInWalkCalls(walker, call, written, request);
}

// `super(..)`: what stands above the class object is the one class object of the class above. new.target has constructed to get here, so it can.
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

// A parameter is the element at its position, or undefined behind the end. Out of a list that is as it was made, and that nothing else has, the read runs no code.
static bool parametersAreReadOutOfListInWalkCalls(Walker& walker, const Function& callee, Reference list)
{
    if (!list || callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasRestParameter }))
        return false;
    return !callee.numberOfPositionalParameters() || (walker.has(Mechanism::TypedOperations) && isFreshListAsMade(walker, list));
}

// False: the walk cannot go on.
static bool readParametersOutOfListInWalkCalls(Walker& walker, const Function& callee, Reference list, Origin originInFrontOfCall, Vector<Reference, 8>& elements)
{
    unsigned numberOfParameters = callee.numberOfPositionalParameters();
    for (unsigned position = 0; position < numberOfParameters; ++position) {
        Draft at(Opcode::Integer, originInFrontOfCall);
        at.setInteger(position);
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

bool listOfSuperCallCanBeHandedOver(Walker& walker, const BodyValue& call, FunctionID callee)
{
    Reference list = walker.referenceOf(call.as<Opcode::SuperConstructList>().listOperand());
    if (!list)
        return false;
    const Function& constructor = walker.program().at(callee);
    if (!isClassConstructorOf(constructor.kind) || constructor.has(FunctionFlags::HasThis))
        return false;
    return elementsOfFreshList(walker, list) || parametersAreReadOutOfListInWalkCalls(walker, constructor, list);
}

// `super(...args)`, `new C(...[a, b])`: the state knows what the list holds, so the constructor of a class is entered as by the construction that writes the elements out.
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
        // `constructor(...args) { super(...args); }` entered by whoever has the class object: how many arguments the list stands for is known only when the call runs.
        written = walker.draftOf(call);
        if (!isSuperCall || !parametersAreReadOutOfListInWalkCalls(walker, constructor, written->listOperand()))
            return false;
        request.listOfUnknownLength = written->listOperand();
        if (!readParametersOutOfListInWalkCalls(walker, constructor, request.listOfUnknownLength, written->origin(), elements))
            return true;
    }
    // Whether another new.target can construct is for the closed form to ask.
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

// The callee keeps the list as its own array. By the counts of the input every mention of the literal but the call is an append to it that stands in front of the call:
// an append hands the list to nobody and leaves no hole, and nothing reads the list behind the call. Each run of the call has a literal of its own.
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
        walker.checkElementsRememberedOfList(walker.referenceOf(list), known->span(), walker.originOf(call));
        elements = WTF::move(*known);
        request.arguments = elements.span();
    } else {
        // How many arguments the list stands for is known only when the call runs.
        if (callee.numberOfPositionalParameters() || callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
            return false;
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

// The front end has found the callee and laid out the operands. It is copied, and says what it hands over.
// Link has proven what every caller hands over, this one among them, and the plan chose the forms of the parameters by that. Where the walk has seen less of an operand a test gives it the proof:
// it is no guess and has no closed arm, only a fault of the compiler takes the failing side. BuilderOfEntries::inFormThatUnitTakes() does the same for whoever comes by value. Null: no way leads on.
static Reference proveOperandOrTrapInWalkCalls(Walker& walker, Origin origin, Reference operand, TypeID promised)
{
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(operand);
    test.setTestedType(walker.types().testablePartOf(promised));
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return { };

    BlockID passing = walker.addBlock();
    BlockID failing = walker.addBlock(BlockFrequency::Rare);
    walker.branch(passes, passing, failing);
    walker.continueIn(failing);
    walker.endAsUnreachable(origin);
    walker.continueIn(passing);

    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(operand);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return walker.build(WTF::move(narrowed));
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
        // A test asks a value of JavaScript for a type of such values. Of anything else Walker::build() names the misfit.
        TypeID promised = walker.facts().typeOfEntryParameter(entered.function, index).proven;
        if (!promised || !types.at(promised).isJavaScriptValue() || !seen.isJavaScriptValue() || seen.mayBeUnspecified())
            continue;
        operands[index] = proveOperandOrTrapInWalkCalls(walker, origin, operands[index], promised);
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
    if (walker.depthOfCopies()) {
        for (Reference& operand : operands)
            operand = walker.closureMadeWhereCopyHandsItOn(call, operand);
        direct.setVariadicOperands(operands.span());
    }
    if (returnsResultOfFunction(direct.role())) {
        if (!bringOperandsIntoFormsOfCalleeInWalkCalls(walker, walker.originOf(call), CodeUnit { callee, direct.role() }, operands))
            return true;
        direct.setVariadicOperands(operands.span());
    }
    if (direct.role() == CodeRole::ParameterGuard && hasProvenWhatGuardTestsInWalkCalls(walker, callee, operands.span()))
        direct.setRole(CodeRole::Body);
    bool entersBody = returnsResultOfFunction(direct.role());
    std::optional<Effects> effects;
    if (entersBody)
        effects = effectsOfDirectCall(walker, callee, operands.span());
    Reference result = walker.build(WTF::move(direct), effects ? &*effects : nullptr);
    if (!result)
        return true;
    walker.bind(call.id(), result);
    if (entersBody)
        walker.leaveArrivals(callee, operands.span());
    return true;
}

// ---- What the constructor of a class allocates

static constexpr OpcodeSet opcodesOfConstructionInWalkCalls { Opcode::GetConstructPrototype, Opcode::NewObject };

// Nobody can store to `prototype` of a class or define it anew, so the read runs no code. For the prototype object of a class that is written at or below the class of the allocation the run time makes an object of that class:
// ClassTable::instanceWithPrototype(). So the allocation names that layout. A class that is evaluated once has one prototype object, which an allocation without the operand names: nothing is looked up when it runs.
constinit const OpcodeSet opcodesThatBuildTypedConstructionTakes = opcodesOfConstructionInWalkCalls;

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

    if (buildObjectOfTaggedClassConstructor(walker, value))
        return true;
    auto allocation = value.as<Opcode::NewObject>();
    Reference prototype = allocation.prototypeOperand() ? walker.referenceOf(allocation.prototypeOperand()) : Reference();
    if (!prototype || !prototype.isValue() || !allocation.fields().empty())
        return false;
    const Code& code = walker.code();
    ValueID root = prototype.id();
    while (opcodesThatNameTheirOperand.contains(code.at(root).opcode))
        root = code.at(root).operand(0);
    const Operation& read = code.at(root);
    if (read.opcode != Opcode::GetClassPrototype)
        return false;
    ValueID classObject = read.classObjectOperand();
    LayoutID selected = classOfClassObjectAtInWalkCalls(walker, code.isConstant(classObject) ? Reference::constant(classObject) : Reference::value(classObject));
    const Plan& plan = walker.plan();
    if (!selected || selected != read.layout() || !isWrittenAtOrBelowInWalkCalls(walker.program(), selected, allocation.layout()) || !isFoundByShapeTest(walker.program().at(selected)))
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

// ---- What is written as something else than a call of the callee

// `value`: the operation in whose place the callee is entered. `written`: as for buildDirectCallInWalkCalls(). False: nothing was built.
static bool copyOrCallDirectlyInWalkCalls(Walker& walker, const BodyValue& value, const Draft& written, const CallInWalkCalls& request)
{
    const Function& callee = walker.program().at(request.callee);
    CodeUnit entered = walker.plan().conventions().directEntryOf(request.callee);
    Vector<OperandInWalkCalls, 8> planned;
    bool mayBeCopied = walker.has(Mechanism::Inlining) && !request.isNeverInlined && !request.listOfUnknownLength && admitsDirectEntryInWalkCalls(walker, callee, entered) && planOperandsInWalkCalls(walker, request, callee, entered, planned);
    if (mayBeCopied) {
        // What is not made yet is no closure of the program.
        Vector<Reference, 8> operands;
        for (const OperandInWalkCalls& operand : planned)
            operands.append(operand.source == OperandSourceInWalkCalls::Given && operand.given ? operand.given : walker.constant(Draft(Opcode::Undefined, written.origin())));
        Draft site(Opcode::CallDirect, written.origin());
        site.setFunction(request.callee);
        site.setVariadicOperands(operands.span());
        mayBeCopied = shouldCopyCallee(walker, site, request.callee, operands.span());
        // `sum.apply(null, [a, 1, 2])`: the call that is forwarded, as a program would write it.
        if (!mayBeCopied && !walker.hasTextsInStringBuilders() && walker.currentFunction() == walker.unit().function) {
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
        // What was made for a copy that is not walked after all nobody needs, and the sweep drops it.
        if (walker.walkInline(value, request.callee, operands.span()))
            return true;
    }
    return buildDirectCallInWalkCalls(walker, value, written, request);
}

bool accessorAdmitsDirectCall(Walker& walker, FunctionID accessor)
{
    const Function& callee = walker.program().at(accessor);
    return bodyRunsWhenCalledInWalkCalls(callee.kind) && admitsDirectEntryInWalkCalls(walker, callee, walker.plan().conventions().entryBehindGuardOf(accessor));
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
    // A frame of the library names the built-in that was called, and an access names none.
    request.isNeverInlined = callee.isHiddenInStackTraces();
    return copyOrCallDirectlyInWalkCalls(walker, access, walker.draftOf(access), request);
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
    checkProofOfOnlyCallee(walker, request.closure, function, origin);
    Vector<Reference, 8> operands;
    for (const OperandInWalkCalls& operand : planned) {
        Reference built = buildOperandInWalkCalls(walker, request, callee, origin, operand);
        if (!built)
            break;
        operands.append(built);
    }
    return operands;
}

bool callFunctionDirectly(Walker& walker, const BodyValue& operation, FunctionID function, Reference thisValue, std::span<const Reference> arguments, MayCopyFunctionCalledDirectly mayCopy)
{
    // Link has not seen the call. What it says of a function whose callers it has counted does not hold of one more.
    const LinkTables& link = walker.facts().linked();
    if (!function || function.index() >= link.functions.size() || link.function(function).everyCallerIsKnown || !canCallCompilerHelper(walker, function))
        return false;
    Draft written = walker.draftOf(operation);
    if (walker.hasFailed())
        return false;
    CallInWalkCalls request;
    request.callee = function;
    request.thisValue = thisValue;
    request.arguments = arguments;
    // A frame of the library names the built-in that was called, and only a call names one.
    if (operation.opcode() == Opcode::Call)
        request.calledBuiltin = written.calledBuiltin();
    else
        request.isNeverInlined = walker.program().at(function).isHiddenInStackTraces();
    if (mayCopy == MayCopyFunctionCalledDirectly::No)
        request.isNeverInlined = true;
    return copyOrCallDirectlyInWalkCalls(walker, operation, written, request);
}

// Closed, such a read is a lookup and a call out of a stub, which saves every register round it.
static constexpr uint32_t closedOperationsOfReadThatCallsGetter = 2;

bool callGetterExpectedAtRead(Walker& walker, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet)
        return false;
    // The reads of the function are found once.
    Walker::GettersExpectedInFunction& expected = walker.gettersExpectedInFunction;
    if (FunctionID function = walker.currentFunction(); expected.function != function) {
        auto reads = std::ranges::equal_range(walker.facts().held().gettersExpectedAtReads, function, { }, &LinkedGetterAtRead::function);
        expected = { function, std::span<const LinkedGetterAtRead>(reads.begin(), reads.end()) };
    }
    if (expected.reads.empty() || read.isFreeName() || !walker.has(Mechanism::DirectCalls))
        return false;
    Reference object = walker.referenceOf(read.objectOperand());
    if (!object || !object.isValue() || !walker.types().at(walker.typeOf(object)).isObject())
        return false;
    auto listed = std::ranges::lower_bound(expected.reads, read.id(), { }, &LinkedGetterAtRead::site);
    FunctionID getter = listed != expected.reads.end() && listed->site == read.id() ? listed->getter : FunctionID();
    if (!getter || !bodyRunsWhenCalledInWalkCalls(walker.program().at(getter).kind))
        return false;
    TypeID closures = walker.types().forFunction(getter);
    if (!shouldOpenGuess(walker, object, closures, true, closedOperationsOfReadThatCallsGetter))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    uint32_t place = read.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;

    Draft lookup(Opcode::GetCachedGetter, origin);
    walker.chooseConservativeFields(lookup);
    lookup.setObjectOperand(object);
    lookup.setKey(read.key());
    Reference found = walker.build(WTF::move(lookup));
    if (!found)
        return true;
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(found);
    test.setTestedType(closures);
    Reference isGetter = walker.build(WTF::move(test));
    if (!isGetter)
        return true;

    BlockID calls = walker.addBlock();
    BlockID reads = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    walker.branch(isGetter, calls, reads);
    walker.noteGuessOpened();

    struct Arm {
        BlockID last;
        Reference value;
    };
    Vector<Arm, 2> arms;
    auto endArm = [&] {
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        arms.append(Arm { walker.currentBlock(), walker.referenceOf(read.id()) });
    };

    walker.continueIn(reads);
    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    walker.continueIn(calls);
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(found);
    proof.setTestOperand(isGetter);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    Reference closure = walker.build(WTF::move(proof));
    if (closure && !callAccessor(walker, read, getter, object, { }, closure)) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm();
    } else
        endArm();
    if (walker.hasFailed())
        return true;

    Reference met;
    if (arms.size() == 2 && arms[0].value && arms[1].value) {
        const Code& code = walker.code();
        TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(arms[0].value.id()).type, code.at(arms[1].value.id()).type);
        Representation forms[] { walker.representationOf(arms[0].value), walker.representationOf(arms[1].value) };
        met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms));
    }
    for (const Arm& arm : arms) {
        walker.continueIn(arm.last);
        if (met)
            walker.jumpTo(meeting, singleElementSpan(arm.value));
        else
            walker.jumpTo(meeting);
        if (walker.hasFailed())
            return true;
    }
    walker.continueIn(meeting);
    if (met)
        walker.bind(read.id(), met);
    else if (arms.size() == 1 && arms[0].value)
        walker.bind(read.id(), arms[0].value);
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
        walker.checkElementsRememberedOfList(forwarded.listOperand(), known->span(), forwarded.origin());
        elements = WTF::move(*known);
        request.arguments = elements.span();
    } else {
        // As in walkCallListInWalkCalls(). The list has a name in the program, so no callee keeps it as its own.
        // `apply` reads every element, whatever the callee takes: what no parameter takes stays unread only where the read runs no code.
        request.listOfUnknownLength = forwarded.listOperand();
        if (!isFreshListAsMade(walker, request.listOfUnknownLength) || !parametersAreReadOutOfListInWalkCalls(walker, callee, request.listOfUnknownLength))
            return false;
        Origin originInFrontOfCall = forwarded.origin();
        originInFrontOfCall.isTailCall = false;
        if (!readParametersOutOfListInWalkCalls(walker, callee, request.listOfUnknownLength, originInFrontOfCall, elements))
            return true;
        request.arguments = elements.span();
    }
    request.thisValue = forwarded.thisValueOperand();
    request.calledBuiltin = forwarded.calledBuiltin();
    return copyOrCallDirectlyInWalkCalls(walker, call, forwarded, request);
}

// ---- A built-in whose body is compiled

// Null: the rows name no function, several, or one that the program does not hold.
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

// `builtin`: canonical.
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

// `list.map(f)`: the library has a text of its own for a receiver that is known for a list, which a row names, and one that takes whatever comes: that is what the function object of the built-in runs, so a call by value enters it.
// `list.sort(f)`: the function object is native code, and one row names a function of the library.
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
    return builtin && isCallThatTheWalkMustLower(walker.program().builtins(), builtin) ? builtin : BuiltinID();
}

FunctionID findFunctionBehindCompiledBuiltin(const Walker& walker, TypeID typeOfCallee)
{
    BuiltinID builtin = findBuiltinWithoutFunctionObjectInWalkCalls(walker, typeOfCallee);
    return builtin ? findFunctionThatRowsNameInWalkCalls(walker.program(), walker.program().builtins().canonicalID(builtin)) : FunctionID();
}

// The run time has nothing to enter for such a built-in: isCallThatTheWalkMustLower(). The function takes whatever the call hands over, as its generic entry would.
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
    if (!walker.hasFailed() && !buildDirectCallInWalkCalls(walker, call, written, request))
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
