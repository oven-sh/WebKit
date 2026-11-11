/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTPlan.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkLoopsOverGenerators.h"

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
bool canWaitForStepOfAsyncGenerator(Walker&);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, BuiltinID awaitedCallee, Reference goesByDriver, Origin);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);


namespace {

enum ParameterOfMeetingInStepsByHand : uint32_t { Whole, Answer, IsAnswered, Value, Done, numberOfParametersOfMeetingInStepsByHand };

struct StepAtHandInStepsByHand {
    Reference answer;
    Reference isAnswered;
    Reference value;
    Reference done;
};

} // anonymous namespace

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
    test.setForm(operatorFormFor(walker, test));
    return boxNumberIfNeeded(walker, walker.build(WTF::move(test)), origin);
}

static Reference buildResultObjectInStepsByHand(Walker& walker, Reference value, Reference done, Origin origin)
{
    if (walker.isUnreachable())
        return { };
    const Program& program = walker.program();
    LayoutID layoutID = program.layoutOf(BuiltinLayouts::IteratorResult);
    const Layout& layout = program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);
    Draft object(Opcode::NewObject, origin);
    object.setLayout(layoutID);
    object.setFields(std::array { *valueField, *doneField });
    object.setValuesOperands(std::array { value, done });
    walker.chooseConservativeFields(object);
    return walker.build(WTF::move(object));
}

static void jumpToInStepsByHand(Walker& walker, BlockID meeting, std::span<const Reference> arguments, bool& someWayArrives)
{
    if (walker.isUnreachable())
        return;
    walker.jumpTo(meeting, arguments);
    someWayArrives = true;
}

static Reference booleanInStepsByHand(Walker& walker, bool written, Origin origin)
{
    Draft constant(Opcode::Boolean, origin);
    constant.setBoolean(written);
    return walker.constant(WTF::move(constant));
}

static bool nothingSeenHappensBetweenInStepsByHand(const BodyIndex& index, ValueID read, ValueID call)
{
    if (index.blockOf(read) != index.blockOf(call) || read.index() >= call.index())
        return false;
    for (uint32_t between = read.index() + 1; between < call.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (!declaredBoundInTable(opcode).mayBeRunAgainUnseen() || operandsMayRunCode(opcode))
            return false;
    }
    return true;
}

static bool readFindsNextOfEngine(Walker& walker, const BodyValue& call)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID next = table.canonicalID(Builtins::GeneratorPrototypeNext);
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(call.calleeOperand());
    if (index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet)
        return false;
    BodyValue access = index.value(read);
    PropertyKeyID key = program.keyOf(BuiltinNames::next);
    if (access.isFreeName() || access.key() != key || index.stripCasts(access.objectOperand()) != index.stripCasts(call.thisValueOperand()))
        return false;
    if (!nothingSeenHappensBetweenInStepsByHand(index, read, call.id()))
        return false;

    BuiltinID prototype = table.at(BuiltinClasses::Generator).prototype;
    if (!engineAnswersForGenerators(walker) || !prototype)
        return false;
    BuiltinID found = findConstantMemberFrom(program, walker.facts(), prototype, key);
    return found && table.canonicalID(found) == next;
}

static bool calleeIsNextOfEngineInStepsByHand(Walker& walker, const BodyValue& call)
{
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    Type typeOfCallee = callee ? types.at(walker.typeOf(callee)) : Type();
    if (BuiltinID named = callee && typeOfCallee.isObject() && typeOfCallee.isJavaScriptValue() ? singleBuiltinOf(types, typeOfCallee) : BuiltinID()) {
        const BuiltinTable& table = walker.program().builtins();
        return table.canonicalID(named) == table.canonicalID(Builtins::GeneratorPrototypeNext);
    }
    return readFindsNextOfEngine(walker, call);
}

enum class ResultOfNextInStepsByHand : uint8_t { IsWhatCallGives, IsWhatAwaitOfCallGives };

static const BitVector& findCallsOfNextWhoseResultIsOnlyReadInStepsByHand(Walker& walker, ResultOfNextInStepsByHand result = ResultOfNextInStepsByHand::IsWhatCallGives)
{
    bool isAwaited = result == ResultOfNextInStepsByHand::IsWhatAwaitOfCallGives;
    auto& found = isAwaited ? walker.loopsOverGenerators().awaitedCallsOfNextWhoseResultIsOnlyRead : walker.loopsOverGenerators().callsOfNextWhoseResultIsOnlyRead;
    auto added = found.add(walker.currentFunction().index(), BitVector());
    BitVector& onlyRead = added.iterator->value;
    if (!added.isNewEntry)
        return onlyRead;

    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    PropertyKeyID next = program.keyOf(BuiltinNames::next);
    PropertyKeyID done = program.keyOf(BuiltinNames::done);
    PropertyKeyID value = program.keyOf(BuiltinNames::value);
    onlyRead.ensureSize(index.numberOfValues());
    Vector<uint32_t> callsBehindNames;
    callsBehindNames.fill(UINT32_MAX, index.numberOfValues());
    auto callBehind = [&](ValueID name) {
        return name ? callsBehindNames[name.index()] : UINT32_MAX;
    };
    auto refuse = [&](ValueID mentioned) {
        if (uint32_t call = callBehind(mentioned); call != UINT32_MAX)
            onlyRead.quickClear(call);
    };

    for (uint32_t blockIndex = 0; blockIndex < index.numberOfBlocks(); ++blockIndex) {
        BodyBlock block = index.block(BlockID(blockIndex));
        refuse(block.subject());
        for (BodyValue user : block.values()) {
            Opcode opcode = user.opcode();
            if (opcodesThatStateTypes.contains(opcode))
                callsBehindNames[user.id().index()] = callBehind(user.operand(0));
            else if (ValueID awaited = isAwaited && opcode == Opcode::Await ? index.stripCasts(user.valueOperand()) : ValueID(); awaited && callBehind(awaited) == awaited.index()) {
                callsBehindNames[user.id().index()] = awaited.index();
                for (ValueID held : user.heldOperands())
                    refuse(held);
            } else if (opcode == Opcode::TypeTest && walker.types().isSubset(user.as<Opcode::TypeTest>().testedType(), TypeTable::nullish)) {
            } else if (opcode == Opcode::PropGet && !user.isFreeName() && (user.key() == done || user.key() == value)) {
            } else {
                user.forEachOperand([&](ValueID operand, uint32_t) {
                    refuse(operand);
                });
            }
            if (opcode == Opcode::Call && user.calleeKey() == next) {
                callsBehindNames[user.id().index()] = user.id().index();
                onlyRead.quickSet(user.id().index());
            }
        }
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            refuse(operand);
        });
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& handed) {
            if (callBehind(handed.subject) == UINT32_MAX)
                return;
            if (index.valueNarrowedInto(handed.parameter) == handed.subject && index.blockOf(handed.parameter).index() > blockIndex)
                callsBehindNames[handed.parameter.index()] = callBehind(handed.subject);
            else
                refuse(handed.subject);
        });
        block.forEachOutgoingEdge([&](BodyTarget target, bool isHandler) {
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t position = 0; position < arguments.size(); ++position) {
                if (isHandler || !index.isUnused(index.block(target.block()).parameter(position)))
                    refuse(arguments[position]);
            }
        });
    }
    return onlyRead;
}

static bool buildStepInStepsByHand(Walker& walker, const BodyValue& call, Reference generator, Reference sent, bool resultIsOnlyRead)
{
    bool readsNextBehindStep = !walker.referenceOf(call.calleeOperand());
    if (readsNextBehindStep)
        walker.bind(call.calleeOperand(), generator);
    Draft written = walker.draftOf(call);
    if (readsNextBehindStep)
        walker.bind(call.calleeOperand(), Reference());
    Origin origin = written.origin();
    origin.isTailCall = false;
    Reference stepped = buildStepOfGenerator(walker, generator, ResumeMode::Next, sent, checkLeftToStepOf(walker, generator), origin);
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
    Reference whole;
    uint32_t numberOfParameters = resultIsOnlyRead ? static_cast<uint32_t>(numberOfParametersOfMeetingInStepsByHand) : 1;
    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
        Reference added = walker.addParameter(meeting, { }, Representation::JSValue);
        if (parameter == Whole)
            whole = added;
    }
    if (resultIsOnlyRead)
        walker.code().markMeetingOfStepWrittenByHand(meeting);
    Reference nothing = undefinedOfCoroutineCalls(walker);
    bool someWayArrives = false;
    walker.branch(isNotStepped, byCall, isStepped);

    walker.continueIn(byCall);
    written.setOrigin(origin);
    written.resultIsReturnedAtOnce = false;
    written.inputValueGiven = { };
    if (readsNextBehindStep) {
        Draft read = walker.draftOf(walker.currentBodyIndex().value(call.calleeOperand()));
        read.inputValueGiven = { };
        walker.chooseConservativeFields(read);
        written.setCalleeOperand(buildInStepsByHand(walker, WTF::move(read)));
    }
    Reference answer;
    if (!walker.isUnreachable()) {
        walker.chooseConservativeFields(written);
        answer = walker.build(WTF::move(written));
    }
    std::array answered { answer, answer, booleanInStepsByHand(walker, true, origin), nothing, nothing };
    jumpToInStepsByHand(walker, meeting, std::span<const Reference>(answered).first(numberOfParameters), someWayArrives);

    walker.continueIn(isStepped);
    Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, GeneratorStepStatus::Completed, origin);
    Reference made = buildResultObjectInStepsByHand(walker, value, isCompleted, origin);
    std::array atHand { made, nothing, booleanInStepsByHand(walker, false, origin), value, isCompleted };
    jumpToInStepsByHand(walker, meeting, std::span<const Reference>(atHand).first(numberOfParameters), someWayArrives);
    if (!someWayArrives || walker.hasFailed())
        return true;

    walker.continueIn(meeting);
    walker.bind(call.id(), whole);
    if (resultIsOnlyRead)
        walker.loopsOverGenerators().hasStepsWrittenByHand = true;
    else
        walker.rememberedExpectationOf(call.id()) = { walker.types().forLayout(walker.program().layoutOf(BuiltinLayouts::IteratorResult)), false, false, true };
    return true;
}

static std::optional<StepAtHandInStepsByHand> findStepBehindInStepsByHand(const Walker& walker, Reference name)
{
    const Code& code = walker.code();
    while (name && name.isValue() && cellPreservingOpcodes.contains(code.at(name.id()).opcode)) {
        ValueID record = code.at(name.id()).operand(0);
        name = code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
    }
    if (!name || !name.isValue())
        return std::nullopt;
    const Block& meeting = code.at(code.blockOf(name.id()));
    std::span<const ValueID> parameters = meeting.parameters();
    if (!meeting.meetsStepWrittenByHand || parameters.size() != numberOfParametersOfMeetingInStepsByHand || parameters[Whole] != name.id())
        return std::nullopt;
    return StepAtHandInStepsByHand { Reference::value(parameters[Answer]), Reference::value(parameters[IsAnswered]), Reference::value(parameters[Value]), Reference::value(parameters[Done]) };
}

bool answerFromStepWrittenByHandOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    bool isTest = value.opcode() == Opcode::TypeTest;
    if (!isTest && value.isFreeName())
        return false;
    ValueID subject = isTest ? value.operand(0) : value.objectOperand();
    std::optional<StepAtHandInStepsByHand> step = subject ? findStepBehindInStepsByHand(walker, walker.referenceBoundTo(subject)) : std::nullopt;
    if (!step)
        return false;

    const Program& program = walker.program();
    Draft written = walker.draftOf(value);
    Reference atHand;
    if (isTest) {
        if (!walker.types().isSubset(value.as<Opcode::TypeTest>().testedType(), TypeTable::nullish))
            return false;
        atHand = booleanInStepsByHand(walker, false, written.origin());
    } else if (value.key() == program.keyOf(BuiltinNames::done))
        atHand = step->done;
    else if (value.key() == program.keyOf(BuiltinNames::value))
        atHand = step->value;
    else
        return false;

    BlockID asWritten = walker.addBlock(BlockFrequency::Rare);
    BlockID ofStep = walker.addBlock();
    BlockID meeting = walker.addBlock();
    Reference read = walker.addParameter(meeting, isTest ? TypeID(TypeTable::boolean) : TypeID(), Representation::JSValue);
    bool someWayArrives = false;
    walker.branch(step->isAnswered, asWritten, ofStep);

    walker.continueIn(asWritten);
    written.setOperand(0, step->answer);
    walker.chooseConservativeFields(written);
    Reference offAnswer = walker.build(WTF::move(written));
    jumpToInStepsByHand(walker, meeting, singleElementSpan(offAnswer), someWayArrives);

    walker.continueIn(ofStep);
    jumpToInStepsByHand(walker, meeting, singleElementSpan(atHand), someWayArrives);
    if (!someWayArrives || walker.hasFailed())
        return true;
    walker.continueIn(meeting);
    walker.bind(value.id(), read);
    return true;
}

static bool isStepOfGenerator(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::BuiltinCalls) || !walker.opensGuesses())
        return false;
    const Program& program = walker.program();
    if (!call.thisValueOperand() || call.argumentsOperands().size() > 1 || call.calleeKey() != program.keyOf(BuiltinNames::next))
        return false;

    Reference generator = walker.referenceOf(call.thisValueOperand());
    if (!generator || !generator.isValue())
        return false;
    TypeArena& types = walker.types();
    TypeID typeOfGenerator = walker.typeOf(generator);
    if (!types.at(typeOfGenerator).isObject() || !types.isSubset(typeOfGenerator, types.builtinClass(BuiltinClasses::Generator))) {
        TypeID expected = walker.has(Mechanism::Guesses) ? findExpectation(walker, call.thisValueOperand()).type : TypeID();
        if (!expected || !types.isSubset(types.withoutNullish(expected), types.builtinClass(BuiltinClasses::Generator)))
            return false;
    }
    return walker.isInLoop() || findCallsOfNextWhoseResultIsOnlyReadInStepsByHand(walker).quickGet(call.id().index());
}

bool callStepsGeneratorOfItsOpcodes(Walker& walker, const BodyValue& call)
{
    return isStepOfGenerator(walker, call) && readFindsNextOfEngine(walker, call);
}

bool leaveNextToStepOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || read.key() != walker.program().keyOf(BuiltinNames::next))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID taker = index.onlyUsingValue(read.id());
    if (!taker || index.opcodeOf(taker) != Opcode::Call || index.blockOf(taker) != index.blockOf(read.id()))
        return false;
    BodyValue call = index.value(taker);
    if (call.calleeOperand() != read.id() || taker.index() - index.block(index.blockOf(taker)).firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    if (!callStepsGenerator(walker, call))
        return false;
    walker.bind(read.id(), Reference());
    return true;
}

bool buildStepWrittenByHand(Walker& walker, const BodyValue& call)
{
    if (!isStepOfGenerator(walker, call) || !calleeIsNextOfEngineInStepsByHand(walker, call))
        return false;
    bool resultIsOnlyRead = findCallsOfNextWhoseResultIsOnlyReadInStepsByHand(walker).quickGet(call.id().index());
    Reference sent = call.argumentsOperands().empty() ? undefinedOfCoroutineCalls(walker) : walker.referenceOf(call.argumentsOperands()[0]);
    return sent && buildStepInStepsByHand(walker, call, walker.referenceOf(call.thisValueOperand()), sent, resultIsOnlyRead);
}

static Reference continueIfHasType(Walker& walker, Reference value, TypeID tested, BlockID otherwise, bool& someWayTurnsAway, Origin origin)
{
    if (walker.isUnreachable() || walker.types().isSubset(walker.typeOf(value), tested))
        return value;
    Draft test = draftOfCoroutineCalls(walker, Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(tested);
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return { };
    BlockID passed = walker.addBlock();
    walker.branch(passes, passed, otherwise);
    someWayTurnsAway = true;
    walker.continueIn(passed);
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(value);
    proof.setTestOperand(passes);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    return buildInStepsByHand(walker, WTF::move(proof));
}

static Reference buildAwaitedStepInStepsByHand(Walker& walker, const BodyValue& call, Reference callee, Reference receiver, Reference sent, std::span<const Reference> held, bool resultIsOnlyRead, Origin originOfAwait)
{
    TypeArena& types = walker.types();
    BuiltinID next = walker.program().builtins().canonicalID(Builtins::AsyncGeneratorPrototypeNext);
    Draft written = walker.draftOf(call);
    Origin origin = written.origin();
    origin.isTailCall = false;

    BlockID byCall = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    Reference whole;
    for (uint32_t parameter = 0; parameter < numberOfParametersOfMeetingInStepsByHand; ++parameter) {
        Reference added = walker.addParameter(meeting, { }, Representation::JSValue);
        if (parameter == Whole)
            whole = added;
    }
    if (resultIsOnlyRead)
        walker.code().markMeetingOfStepWrittenByHand(meeting);
    Reference nothing = undefinedOfCoroutineCalls(walker);
    bool someWayArrives = false;
    bool someWayTurnsAway = false;

    continueIfHasType(walker, callee, types.forBuiltin(next), byCall, someWayTurnsAway, origin);
    Reference generator = continueIfHasType(walker, receiver, types.builtinClass(BuiltinClasses::AsyncGenerator), byCall, someWayTurnsAway, origin);
    if (walker.shouldFailSpeculations() && !walker.isUnreachable()) [[unlikely]] {
        walker.letGuessFailUnasked(byCall, origin);
        someWayTurnsAway = true;
    }
    walker.recordSpeculation();
    Reference stepped = walker.isUnreachable() ? Reference() : buildStepOfAsyncGenerator(walker, generator, sent, held, next, { }, origin);
    Reference value = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Value, origin);
    Reference status = buildResultAtInStepsByHand(walker, stepped, StepGeneratorResult::Status, origin);
    if (Reference isAnsweredByObject = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Result, origin)) {
        BlockID ofEngine = walker.addBlock(BlockFrequency::Rare);
        BlockID ofStep = walker.addBlock();
        walker.branch(isAnsweredByObject, ofEngine, ofStep);
        walker.continueIn(ofEngine);
        jumpToInStepsByHand(walker, meeting, std::array { value, value, booleanInStepsByHand(walker, true, origin), nothing, nothing }, someWayArrives);
        walker.continueIn(ofStep);
        Reference isCompleted = buildTestOfStatusInStepsByHand(walker, status, AsyncGeneratorStepStatus::Completed, origin);
        Reference made = buildResultObjectInStepsByHand(walker, value, isCompleted, origin);
        jumpToInStepsByHand(walker, meeting, std::array { made, nothing, booleanInStepsByHand(walker, false, origin), value, isCompleted }, someWayArrives);
    }

    if (someWayTurnsAway && !walker.hasFailed()) {
        walker.continueIn(byCall);
        written.setOrigin(origin);
        written.resultIsReturnedAtOnce = false;
        written.inputValueGiven = { };
        walker.chooseConservativeFields(written);
        Reference promised = walker.build(WTF::move(written));
        Reference answer = promised ? buildAwaitBySavingFrame(walker, promised, held, originOfAwait) : Reference();
        if (answer)
            jumpToInStepsByHand(walker, meeting, std::array { answer, answer, booleanInStepsByHand(walker, true, origin), nothing, nothing }, someWayArrives);
    }
    if (!someWayArrives || walker.hasFailed())
        return { };
    walker.continueIn(meeting);
    walker.loopsOverGenerators().hasStepsWrittenByHand = true;
    return whole;
}

bool buildAwaitedStepWrittenByHand(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    const Program& program = walker.program();
    if (!call.thisValueOperand() || call.argumentsOperands().size() > 1 || call.calleeKey() != program.keyOf(BuiltinNames::next) || !canWaitForStepOfAsyncGenerator(walker) || program.at(walker.unit().function).isModuleBody())
        return false;

    Reference receiver = walker.referenceOf(call.thisValueOperand());
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!receiver || !receiver.isValue() || !callee)
        return false;
    TypeArena& types = walker.types();
    TypeID generators = types.builtinClass(BuiltinClasses::AsyncGenerator);
    TypeID typeOfReceiver = walker.typeOf(receiver);
    if (!types.at(typeOfReceiver).isObject() || !types.isSubset(typeOfReceiver, generators)) {
        TypeID expected = findExpectation(walker, call.thisValueOperand()).type;
        if (!expected || !types.isSubset(types.withoutNullish(expected), generators))
            return false;
    }

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    std::optional<WaitOfCallInCoroutine> wait = findWaitOfCallInCoroutine(walker, index, block, call.id());
    if (!wait)
        return false;
    Reference sent = call.argumentsOperands().empty() ? undefinedOfCoroutineCalls(walker) : walker.referenceOf(call.argumentsOperands()[0]);
    if (!sent)
        return false;

    Vector<Reference, 4> held;
    if (!walkUpToAwaitOfCall(walker, index, block, *wait, held))
        return true;
    bool resultIsOnlyRead = findCallsOfNextWhoseResultIsOnlyReadInStepsByHand(walker, ResultOfNextInStepsByHand::IsWhatAwaitOfCallGives).quickGet(call.id().index());
    if (Reference whole = buildAwaitedStepInStepsByHand(walker, call, callee, receiver, sent, held.span(), resultIsOnlyRead, walker.originOf(index.value(wait->await))))
        finishWaitOfCall(walker, block, *wait, whole);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
