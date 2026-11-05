/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `g.next(v)` and `await it.next(v)` written by hand, on what is proven a generator with the engine's `next`: the body is resumed where the call stands, and the result object is made here, in sight of the reads behind it.
// At a suspension (the async step): the frame waits for the step itself, with no promise for the call. At a throw: the resumed body throws out of the step, as out of the call.
// A generator that runs or has ended is answered by the call as written. Withheld (DirectCalls, BuiltinCalls): the call as written.

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkCoroutinePromises.h"

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
// AOTWalkNumbers.cpp
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

namespace {

struct LayoutOfResultInStepsByHand {
    LayoutID layout;
    uint32_t valueField { 0 };
    uint32_t doneField { 0 };
};

} // anonymous namespace

// ---- What the two steps are made of. An arm may end early: from there on nothing is built, and null stands for every value.

static Reference buildInStepsByHand(Walker& walker, Draft&& operation)
{
    return walker.isUnreachable() ? Reference() : walker.build(WTF::move(operation));
}

static Reference buildResultAtInStepsByHand(Walker& walker, Reference step, StepGeneratorResult result, Origin origin)
{
    Draft read = draftOfCoroutineCalls(walker, Opcode::ResultAt, origin);
    read.setResultsOperand(step);
    read.setIndex(static_cast<uint32_t>(result));
    return buildInStepsByHand(walker, WTF::move(read));
}

template<typename Status>
static Reference buildTestOfStatusInStepsByHand(Walker& walker, Reference status, Status asked, Origin origin)
{
    if (walker.isUnreachable())
        return { };
    Draft written(Opcode::Integer, origin);
    written.setInteger(static_cast<int64_t>(asked));
    Draft test = draftOfCoroutineCalls(walker, Opcode::StrictEq, origin);
    test.setOperand(0, status);
    test.setOperand(1, walker.constant(WTF::move(written)));
    test.setForm(formAllowedForTypedOperator(walker, test));
    return boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(test)), origin);
}

static Reference buildReadInStepsByHand(Walker& walker, Reference object, BuiltinName name, Origin origin)
{
    Draft access = draftOfCoroutineCalls(walker, Opcode::PropGet, origin);
    access.setObjectOperand(object);
    access.setKey(walker.program().keyOf(name));
    return buildInStepsByHand(walker, WTF::move(access));
}

static Reference buildReadOfDoneInStepsByHand(Walker& walker, Reference object, Origin origin)
{
    Draft conversion = draftOfCoroutineCalls(walker, Opcode::ToBoolean, origin);
    conversion.setValueOperand(buildReadInStepsByHand(walker, object, BuiltinNames::done, origin));
    return buildInStepsByHand(walker, WTF::move(conversion));
}

static void jumpToInStepsByHand(Walker& walker, BlockID meeting, std::span<const Reference> arguments, bool& someWayArrives)
{
    if (walker.isUnreachable())
        return;
    walker.jumpTo(meeting, arguments);
    someWayArrives = true;
}

static std::optional<LayoutOfResultInStepsByHand> findLayoutOfResultInStepsByHand(const Walker& walker)
{
    const Program& program = walker.program();
    LayoutID id = program.layoutOf(BuiltinLayouts::IteratorResult);
    if (!id || !walker.plan().isInUse(id))
        return std::nullopt;
    const Layout& layout = program.at(id);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    if (!valueField || !doneField)
        return std::nullopt;
    return LayoutOfResultInStepsByHand { id, *valueField, *doneField };
}

// buildIteratorResultAsReplacement(). It is noted as what nothing else names, so the reads of `value` and `done` behind it are answered and the sweep drops it.
static Reference buildResultObjectInStepsByHand(Walker& walker, const LayoutOfResultInStepsByHand& layout, Reference value, Reference done, ValueID given, Origin origin)
{
    if (walker.isUnreachable())
        return { };
    done = proveOrTrapInCoroutineCalls(walker, done, TypeTable::boolean, origin);
    if (!done)
        return { };
    origin.isTailCall = false;
    Draft object(Opcode::NewObject, origin);
    object.setLayout(layout.layout);
    object.setFields(std::array { layout.valueField, layout.doneField });
    object.setValuesOperands(std::array { value, done });
    object.inputValueGiven = given;
    walker.chooseConservativeFields(object);
    Reference result = walker.build(WTF::move(object));
    noteFreshObject(walker, result, 2);
    return result;
}

// ---- Whether the callee is the engine's `next`

// By binding: the type of the callee names the function. Or the callee is read under the name off the very receiver, of which Link has proven that it is as its class makes it (L2), so the read has found what the table lists above it.
// What an object has lost it never gets back, so what holds at the call held at the read.
static bool calleeIsNextOfEngineInStepsByHand(Walker& walker, const BodyValue& call, Reference generator, bool isAsync)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    TypeArena& types = walker.types();
    BuiltinID next = table.canonicalID(isAsync ? Builtins::AsyncGeneratorPrototypeNext : Builtins::GeneratorPrototypeNext);

    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return false;
    Type typeOfCallee = types.at(walker.typeOf(callee));
    if (BuiltinID named = typeOfCallee.isObject() && typeOfCallee.isJavaScriptValue() ? singleBuiltinOf(types, typeOfCallee) : BuiltinID())
        return table.canonicalID(named) == next;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.valueBehindCasts(call.calleeOperand());
    if (index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet)
        return false;
    BodyValue access = index.value(read);
    PropertyKeyID key = program.keyOf(BuiltinNames::next);
    if (access.isFreeName() || access.key() != key || index.valueBehindCasts(access.objectOperand()) != index.valueBehindCasts(call.thisValueOperand()))
        return false;

    BuiltinClassID builtinClass = isAsync ? BuiltinClasses::AsyncGenerator : BuiltinClasses::Generator;
    bool isAsItsClassMakesIt = isAsync
        ? pristineAspectsProvenBy(BuiltinTypeContext { program, types, &walker.facts() }, walker.typeOf(generator), builtinClass).contains(PristineAspects::Names)
        : checkLeftToStepOf(walker, generator) == GeneratorStepCheck::None;
    BuiltinID prototype = table.at(builtinClass).prototype;
    if (!isAsItsClassMakesIt || !prototype)
        return false;
    BuiltinID found = findConstantMemberFrom(program, walker.facts(), prototype, key);
    return found && table.canonicalID(found) == next;
}

// ---- g.next(v)

static bool buildStepInStepsByHand(Walker& walker, const BodyValue& call, Reference generator, Reference sent, const LayoutOfResultInStepsByHand& layout)
{
    // What a look at a loop has reckoned with.
    if (walker.isInLoop()) {
        TypeID widest = widestTypeOfOperation(walker, call);
        if (widest && !walker.types().isSubset(walker.types().forLayout(layout.layout), widest))
            return false;
    }

    Draft written = walker.draftOf(call);
    Origin origin = written.origin();
    origin.isTailCall = false;
    Reference stepped = buildStepOfGenerator(walker, generator, ResumeMode::Next, sent, GeneratorStepCheck::None, origin);
    if (!stepped)
        return true;
    Reference value = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Value, origin);
    Reference status = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Status, origin);
    Reference isNotStepped = buildTestOfStatusInStepsByHand(walker, status, GeneratorStepStatus::NotStepped, origin);
    if (!isNotStepped)
        return true;

    BlockID byCall = walker.addBlock(BlockFrequency::Rare);
    BlockID isStepped = walker.addBlock();
    BlockID meeting = walker.addBlock();
    Reference valueOfResult = walker.addParameter(meeting, { }, Representation::JSValue);
    Reference doneOfResult = walker.addParameter(meeting, { }, Representation::JSValue);
    bool someWayArrives = false;
    walker.branch(isNotStepped, byCall, isStepped);

    // The generator is running or has ended, which the method answers, with an object of its own that this code alone gets.
    walker.continueIn(byCall);
    written.setOrigin(origin);
    written.resultIsReturnedAtOnce = false;
    written.inputValueGiven = { };
    walker.chooseConservativeFields(written);
    Reference answer = walker.build(WTF::move(written));
    Reference valueOfAnswer = buildReadInStepsByHand(walker, answer, BuiltinNames::value, origin);
    Reference doneOfAnswer = buildReadOfDoneInStepsByHand(walker, answer, origin);
    jumpToInStepsByHand(walker, meeting, std::array { valueOfAnswer, doneOfAnswer }, someWayArrives);

    walker.continueIn(isStepped);
    Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, GeneratorStepStatus::Completed, origin);
    jumpToInStepsByHand(walker, meeting, std::array { value, isCompleted }, someWayArrives);
    if (!someWayArrives || walker.hasFailed())
        return true;

    walker.continueIn(meeting);
    if (Reference result = buildResultObjectInStepsByHand(walker, layout, valueOfResult, doneOfResult, call.id(), origin))
        walker.bind(call.id(), result);
    return true;
}

// ---- await it.next(v)

static bool buildAwaitedStepInStepsByHand(Walker& walker, const BodyValue& call, Reference generator, Reference sent, const LayoutOfResultInStepsByHand& layout)
{
    if (walker.depthOfCopies())
        return false;
    std::optional<AwaitingBodyOfCoroutine> body = findAwaitingBodyOfCoroutine(walker);
    if (!body)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    std::optional<WaitOfCallInCoroutine> wait = findWaitOfCallInCoroutine(walker, index, block, call.id());
    if (!wait || isCellOfFrameInCoroutineCalls(walker, sent))
        return false;

    Vector<Reference, 4> held;
    if (!walkUpToAwaitOfCall(walker, index, block, *wait, held))
        return true;
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Draft step = draftOfCoroutineCalls(walker, Opcode::StepAsyncGenerator, origin);
    step.setGeneratorOperand(generator);
    step.setValueOperand(sent);
    step.setDriverOperand(body->driver);
    step.setHeldOperands(held.span());
    step.setCalledBuiltin(walker.program().builtins().canonicalID(Builtins::AsyncGeneratorPrototypeNext));
    step.setAwaitsCall(true);
    Reference stepped = buildSuspensionInCoroutine(walker, WTF::move(step));
    if (!stepped)
        return true;
    Reference value = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Value, origin);
    Reference status = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Status, origin);
    Reference isResult = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Result, origin);
    if (!isResult)
        return true;

    // The run time has gone the whole way of the call and the await, and the value is what the await gave.
    BlockID isAnsweredByAwait = walker.addBlock(BlockFrequency::Rare);
    BlockID isAnswered = walker.addBlock();
    BlockID meeting = walker.addBlock();
    bool someWayArrives = false;
    Reference result;

    if (isProvenToHaveNoThen(walker.program(), walker.facts(), walker.types(), walker.types().forLayout(layout.layout))) {
        // No result object is a thenable, so the await gave the object that the generator made, which this code alone gets. One object is made for both ways.
        Reference valueOfResult = walker.addParameter(meeting, { }, Representation::JSValue);
        Reference doneOfResult = walker.addParameter(meeting, { }, Representation::JSValue);
        walker.branch(isResult, isAnsweredByAwait, isAnswered);

        walker.continueIn(isAnsweredByAwait);
        Draft check = draftOfCoroutineCalls(walker, Opcode::RequireObject, origin);
        check.setValueOperand(value);
        check.setRuntimeError(RuntimeErrorKind::IteratorResultNotObject);
        Reference answer = buildInStepsByHand(walker, WTF::move(check));
        Reference valueOfAnswer = buildReadInStepsByHand(walker, answer, BuiltinNames::value, origin);
        Reference doneOfAnswer = buildReadOfDoneInStepsByHand(walker, answer, origin);
        jumpToInStepsByHand(walker, meeting, std::array { valueOfAnswer, doneOfAnswer }, someWayArrives);

        walker.continueIn(isAnswered);
        Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Completed, origin);
        jumpToInStepsByHand(walker, meeting, std::array { value, isCompleted }, someWayArrives);
        if (!someWayArrives || walker.hasFailed())
            return true;
        walker.continueIn(meeting);
        result = buildResultObjectInStepsByHand(walker, layout, valueOfResult, doneOfResult, wait->await, origin);
    } else {
        result = walker.addParameter(meeting, { }, Representation::JSValue);
        walker.branch(isResult, isAnsweredByAwait, isAnswered);

        walker.continueIn(isAnsweredByAwait);
        jumpToInStepsByHand(walker, meeting, singleElementSpan(value), someWayArrives);

        walker.continueIn(isAnswered);
        Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Completed, origin);
        Reference made = buildResultObjectInStepsByHand(walker, layout, value, isCompleted, { }, origin);
        jumpToInStepsByHand(walker, meeting, singleElementSpan(made), someWayArrives);
        if (!someWayArrives || walker.hasFailed())
            return true;
        walker.continueIn(meeting);
    }
    if (result)
        finishWaitOfCall(walker, block, *wait, result);
    return true;
}

bool buildStepWrittenByHand(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::BuiltinCalls))
        return false;
    if (!call.thisValueOperand() || call.argumentsOperands().size() > 1)
        return false;
    // The key under which the callee was read, where the call still says it. It decides nothing: it spares the questions below.
    const Program& program = walker.program();
    if (PropertyKeyID key = call.calleeKey(); key && key != program.keyOf(BuiltinNames::next))
        return false;

    Reference generator = walker.referenceOf(call.thisValueOperand());
    if (!generator || !generator.isValue())
        return false;
    TypeArena& types = walker.types();
    TypeID typeOfGenerator = walker.typeOf(generator);
    if (!types.at(typeOfGenerator).isObject())
        return false;
    bool isAsync = false;
    if (!types.isSubset(typeOfGenerator, types.builtinClass(BuiltinClasses::Generator))) {
        if (!types.isSubset(typeOfGenerator, types.builtinClass(BuiltinClasses::AsyncGenerator)))
            return false;
        isAsync = true;
    }
    if (!calleeIsNextOfEngineInStepsByHand(walker, call, generator, isAsync))
        return false;
    std::optional<LayoutOfResultInStepsByHand> layout = findLayoutOfResultInStepsByHand(walker);
    if (!layout)
        return false;

    Reference sent = call.argumentsOperands().empty() ? undefinedOfCoroutineCalls(walker) : walker.referenceOf(call.argumentsOperands()[0]);
    if (!sent)
        return false;
    return isAsync ? buildAwaitedStepInStepsByHand(walker, call, generator, sent, *layout) : buildStepInStepsByHand(walker, call, generator, sent, *layout);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
