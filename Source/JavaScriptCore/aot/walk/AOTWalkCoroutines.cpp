/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// At a suspension: the operation becomes the one of the family SavedFrame that names the driver, and all that rests on the heap ends. Values keep their types.
// What the saved frame holds is found behind the walk: Code::liveAt(), and the values that the input lists as held.
// At a throw: nothing. The unwinder rejects the promise of an async function, and a generator throws on to whoever resumed it.
// Never withheld.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTPrimitives.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkCoroutineCalls.h"
#include "AOTWalkCoroutinePromises.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalk.h declares none of these. The first three are for AOTWalk.cpp, the last for AOTWalkIteration.cpp.
bool walkRampOfCoroutine(Walker&);
Reference addDriverParameterOfCoroutine(Walker&, BlockID entry);
Reference convertReturnedValueOfCoroutine(Walker&, Reference returned, Origin);
Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin);
bool canWaitForStepOfAsyncGenerator(Walker&);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, Origin);
bool engineAnswersAboveGenerators(const Walker&); // AOTWalkIteration.cpp

static constexpr OpcodeSet operationsOfFamilyCoroutine { Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::ReadScope };

namespace {

struct SuspendingBodyOfCoroutine {
    CoroutineKind kind;
    Reference driver;
};

} // anonymous namespace

// ---- What everything below is made of

static Draft draftForCoroutine(Walker& walker, Opcode opcode, Origin origin)
{
    origin.isTailCall = false;
    Draft draft(opcode, origin);
    walker.chooseConservativeFields(draft);
    return draft;
}

static Reference boxedUndefinedForCoroutine(Walker& walker)
{
    Draft undefined(Opcode::Undefined);
    undefined.representation = Representation::JSValue;
    return walker.constant(WTF::move(undefined));
}

static Reference boxedResumeModeForCoroutine(Walker& walker, ResumeMode mode)
{
    Draft integer(Opcode::Integer);
    integer.setInteger(static_cast<int64_t>(mode));
    integer.representation = Representation::JSValue;
    return walker.constant(WTF::move(integer));
}

// Return or Throw. The walk stands nowhere behind it: whoever goes on calls Walker::continueIn().
static bool endBlockOfCoroutine(Walker& walker, TerminatorKind kind, Reference value, Origin origin)
{
    if (!value)
        return false;
    origin.isTailCall = false;
    if (kind == TerminatorKind::Throw)
        walker.endWithThrow(value, origin);
    else
        walker.endWithReturn(singleElementSpan(value), origin);
    return !walker.hasFailed();
}

// Nothing: no frame stands here that a suspension could save. A suspension is bound to the frame of the function that it is written in, and a coroutine is never walked at a call.
static std::optional<SuspendingBodyOfCoroutine> findSuspendingBodyOfCoroutine(Walker& walker)
{
    CodeUnit unit = walker.unit();
    if (unit.role != CodeRole::CoroutineBody || walker.currentFunction() != unit.function)
        return std::nullopt;
    const Function& function = walker.program().at(unit.function);
    if (!function.isCoroutine() || walker.plan().conventions().of(unit.function).coroutineNeverSuspends)
        return std::nullopt;

    // The driver arrives behind the parameters of the function.
    const Code& code = walker.code();
    if (code.entries().empty())
        return std::nullopt;
    std::span<const ValueID> parameters = code.at(code.entries()[0].block).parameters();
    if (parameters.size() != function.numberOfEntryParameters() + 1)
        return std::nullopt;

    CoroutineKind kind = CoroutineKind::AsyncFunction;
    if (function.has(FunctionFlags::IsGenerator))
        kind = function.has(FunctionFlags::IsAsync) ? CoroutineKind::AsyncGenerator : CoroutineKind::Generator;
    return SuspendingBodyOfCoroutine { kind, Reference::value(parameters.back()) };
}

static Reference buildSuspensionOfCoroutine(Walker& walker, Draft&& suspension)
{
    noteSuspensionOfCoroutine(walker, suspension);
    Reference resumption = walker.build(WTF::move(suspension));
    walker.state().invalidateHeap();
    return resumption;
}

std::optional<CoroutineKind> kindOfSuspendingBody(Walker& walker, Reference& driver)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    driver = body ? body->driver : Reference();
    return body ? std::optional(body->kind) : std::nullopt;
}

Reference buildSuspensionInCoroutine(Walker& walker, Draft&& suspension)
{
    return buildSuspensionOfCoroutine(walker, WTF::move(suspension));
}

// ---- Await

Reference buildAwaitBySavingFrame(Walker& walker, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin origin)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body || body->kind == CoroutineKind::Generator) {
        walker.fail("an await stands where no frame of an async function can be saved"_s);
        return { };
    }

    if (takesExtraTurn && body->kind == CoroutineKind::AsyncGenerator) {
        walker.fail("no job that resumes an async generator takes an extra turn"_s);
        return { };
    }

    if (Reference given = buildAwaitOfWhatHasNoThen(walker, awaited, held, takesExtraTurn, origin))
        return given;

    Draft await = draftForCoroutine(walker, Opcode::AwaitBySavingFrame, origin);
    await.setAwaitedOperand(awaited);
    await.setPromiseOperand(body->driver);
    await.setHeldOperands(held);
    await.setTakesExtraTurn(takesExtraTurn);
    return buildSuspensionOfCoroutine(walker, WTF::move(await));
}

// ---- A step of an async generator that the frame waits for

// Which queue resumes the body of a module depends on whether it awaits a promise. Whoever finishes a step queues a job that is none of the loader's.
bool canWaitForStepOfAsyncGenerator(Walker& walker)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    return body && body->kind != CoroutineKind::Generator && !walker.program().at(walker.unit().function).isModuleBody();
}

// `await generator.next(sent)` on an async generator, with the engine's `next`: no promise is made for the call and no object for its answer. It gives StepGeneratorResult.
Reference buildStepOfAsyncGenerator(Walker& walker, Reference generator, Reference sent, std::span<const Reference> held, Origin origin)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body || body->kind == CoroutineKind::Generator) {
        walker.fail("a step of an async generator stands where no frame of an async function can be saved"_s);
        return { };
    }

    Draft step = draftForCoroutine(walker, Opcode::StepAsyncGenerator, origin);
    step.setGeneratorOperand(generator);
    step.setValueOperand(sent);
    step.setDriverOperand(body->driver);
    step.setHeldOperands(held);
    return buildSuspensionOfCoroutine(walker, WTF::move(step));
}

// ---- Yield

static Reference buildYieldBySavingFrameOfCoroutine(Walker& walker, const SuspendingBodyOfCoroutine& body, Reference yielded, std::span<const Reference> held, Origin origin)
{
    Draft yield = draftForCoroutine(walker, Opcode::YieldBySavingFrame, origin);
    yield.setGeneratorOperand(body.driver);
    yield.setValueOperand(yielded);
    yield.setHeldOperands(held);
    return buildSuspensionOfCoroutine(walker, WTF::move(yield));
}

static std::optional<SuspendingBodyOfCoroutine> findYieldingBodyOfCoroutine(Walker& walker)
{
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (body && body->kind != CoroutineKind::AsyncFunction)
        return body;
    walker.fail("a yield stands where no frame of a generator can be saved"_s);
    return std::nullopt;
}

static Reference buildResultOfYieldOfCoroutine(Walker& walker, Reference resumption, YieldResult result, Origin origin)
{
    Draft read = draftForCoroutine(walker, Opcode::ResultAt, origin);
    read.setResultsOperand(resumption);
    read.setIndex(static_cast<uint32_t>(result));
    return walker.build(WTF::move(read));
}

// ---- The initial yield

// The walk goes on where the generator was resumed in another mode.
static void branchOnResumeModeOfCoroutine(Walker& walker, Reference mode, ResumeMode modeThatLeaves, TerminatorKind kind, Reference sent, Origin origin)
{
    Draft test = draftForCoroutine(walker, Opcode::StrictEq, origin);
    test.setLeftOperand(mode);
    test.setRightOperand(boxedResumeModeForCoroutine(walker, modeThatLeaves));
    Reference isResumedToLeave = walker.build(WTF::move(test));

    BlockID leaves = walker.addBlock(BlockFrequency::Rare);
    BlockID goesOn = walker.addBlock();
    walker.branch(isResumedToLeave, leaves, goesOn);
    walker.continueIn(leaves);
    endBlockOfCoroutine(walker, kind, sent, origin);
    walker.continueIn(goesOn);
}

// Nothing of the language names what the call gives but one iteration, which hands it to nobody.
static bool generatorGoesIntoIterationAloneOfCoroutine(const BodyIndex& body, ValueID site)
{
    if (body.isParameter(site) || body.opcodeOf(site) != Opcode::Call)
        return false;
    if (body.isUnused(site))
        return true;
    ValueID taker = body.onlyUsingValue(site);
    if (!taker)
        return false;
    BodyValue taking = body.value(taker);
    if (taking.opcode() == Opcode::AppendSpread)
        return taking.as<Opcode::AppendSpread>().iterableOperand() == site && taking.as<Opcode::AppendSpread>().arrayOperand() != site;
    return taking.opcode() == Opcode::IteratorOpen && taking.as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync;
}

// Between a generator and %GeneratorPrototype% stands the object under `prototype` of its function. It is come by through a closure of the function or through a generator.
// LinkedFunction::everyCallHasOneCallee: no closure has reached unseen code, is lost, or was asked for that name, and every call that enters is listed, each in a referrer. What each of those calls gives is looked at here.
// The ramp and the body are walked apart, so the mechanisms asked for are those of the build.
static bool nobodyLooksAboveGeneratorsOfCoroutine(const Walker& walker, FunctionID generatorFunction)
{
    const Function& function = walker.program().at(generatorFunction);
    if (!function.has(FunctionFlags::IsGenerator) || function.has(FunctionFlags::IsAsync) || !walker.facts().mechanisms().contains(Mechanism::TypedOperations))
        return false;
    const LinkTables& linked = walker.facts().linked();
    if (!linked.function(generatorFunction).everyCallHasOneCallee || !engineAnswersAboveGenerators(walker))
        return false;
    // `arguments.callee`, and a text that is evaluated in the function, name its closure.
    const BodyIndex* own = walker.facts().bodies().find(generatorFunction);
    if (!own || own->holdsAnyOf(opcodesOfMappedArguments) || walker.facts().summaries().at(generatorFunction).has(SummaryTrait::HasDirectEval))
        return false;
    for (FunctionID referrer : linked.referrersOf(generatorFunction)) {
        if (linked.isGroup(referrer))
            return false;
        uint32_t first = linked.firstCallOfCaller[referrer.index()];
        const BodyIndex* body = nullptr;
        for (const LinkedCall& call : linked.calls.span().subspan(first, linked.firstCallOfCaller[referrer.index() + 1] - first)) {
            if (call.callee != generatorFunction)
                continue;
            if (!body)
                body = walker.facts().bodies().find(referrer);
            if (!body || !generatorGoesIntoIterationAloneOfCoroutine(*body, call.site))
                return false;
        }
    }
    return true;
}

// What stands in front of the InitialYield evaluates the parameters. True: some of it may run other code or write a named property, or more than one way leads from the entry to the yield.
// The declared bound leaves out what a protocol on an operand may run: operandsMayRunCode().
static bool headOfGeneratorMayReplacePrototypeOfCoroutine(const BodyIndex& input)
{
    if (input.numberOfIncomingEdges(entryBlock))
        return true;
    BlockID block = entryBlock;
    for (uint32_t numberOfBlocksPassed = 0; numberOfBlocksPassed < input.numberOfBlocks(); ++numberOfBlocksPassed) {
        BodyBlock head = input.block(block);
        if (head.hasHandler())
            return true;
        for (BodyValue operation : head.values()) {
            Opcode opcode = operation.opcode();
            if (opcode == Opcode::InitialYield)
                return false;
            const Effects& bound = declaredBoundInTable(opcode);
            if (bound.mayRunOtherCode() || operandsMayRunCode(opcode) || bound.writes.contains(AbstractHeap::NamedProperties))
                return true;
        }
        BodyTerminator terminator = input.terminator(block);
        if (terminator.kind() != TerminatorKind::Jump)
            return true;
        block = terminator.next().block();
        if (input.numberOfIncomingEdges(block) != 1)
            return true;
    }
    return true;
}

// The ramp starts the body where it makes the generator, so the body always suspends here. The walk goes on where the first next() resumes it.
static void replaceInitialYieldOfCoroutine(Walker& walker, const BodyValue& value)
{
    std::optional<SuspendingBodyOfCoroutine> body = findYieldingBodyOfCoroutine(walker);
    if (!body)
        return;
    Draft initialYield = walker.draftOf(value);
    Origin origin = initialYield.origin();

    // The ramp allocates the generator, which reads the `prototype` of the function, in front of the code that evaluates the parameters. ECMAScript reads the property behind them.
    std::optional<unsigned> calleeParameter = walker.program().at(walker.unit().function).calleeParameterIndex();
    if (!calleeParameter) {
        walker.fail("a generator function does not take its function object, which its generators read their prototype from"_s);
        return;
    }
    if (!nobodyLooksAboveGeneratorsOfCoroutine(walker, walker.unit().function) && (!walker.has(Mechanism::TypedOperations) || headOfGeneratorMayReplacePrototypeOfCoroutine(walker.currentBodyIndex()))) {
        Draft takePrototype = draftForCoroutine(walker, Opcode::TakeGeneratorPrototype, origin);
        takePrototype.setGeneratorOperand(body->driver);
        takePrototype.setCalleeOperand(walker.referenceOf(walker.currentBodyIndex().block(entryBlock).parameter(*calleeParameter)));
        walker.build(WTF::move(takePrototype));
    }

    Reference resumption = buildYieldBySavingFrameOfCoroutine(walker, *body, boxedUndefinedForCoroutine(walker), initialYield.heldOperands(), origin);
    if (!resumption)
        return;
    Reference sent = buildResultOfYieldOfCoroutine(walker, resumption, YieldResult::Sent, origin);
    Reference mode = buildResultOfYieldOfCoroutine(walker, resumption, YieldResult::Mode, origin);
    if (!sent || !mode)
        return;
    branchOnResumeModeOfCoroutine(walker, mode, ResumeMode::Throw, TerminatorKind::Throw, sent, origin);
    branchOnResumeModeOfCoroutine(walker, mode, ResumeMode::Return, TerminatorKind::Return, sent, origin);
}

// ---- `yield v` in an async generator. The input has an Await, casts, a Yield and a switch on the resume mode.

static bool isResultOfYieldOfCoroutine(const BodyIndex& index, ValueID value, ValueID yield, YieldResult result)
{
    if (!value || index.opcodeOf(value) != Opcode::ResultAt)
        return false;
    auto read = index.value(value).as<Opcode::ResultAt>();
    return read.resultsOperand() == yield && read.index() == static_cast<uint32_t>(result);
}

static bool blocksShareHandlerOfCoroutine(BodyBlock first, BodyBlock second)
{
    if (!first.hasHandler() || !second.hasHandler())
        return first.hasHandler() == second.hasHandler();
    BodyTarget handlerOfFirst = first.handler();
    BodyTarget handlerOfSecond = second.handler();
    return handlerOfFirst.block() == handlerOfSecond.block() && std::ranges::equal(handlerOfFirst.arguments(), handlerOfSecond.arguments());
}

// A rejection of what the run time awaits resumes the yield in ResumeMode::Throw. That matches the await throwing only if the Throw arm just rethrows the sent value, under the handler of the await's block.
static bool throwArmRethrowsSentValueOfCoroutine(const BodyIndex& index, BodyBlock block, ValueID yield)
{
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Switch)
        return false;
    auto modeSwitch = terminator.as<TerminatorKind::Switch>();
    if (!isResultOfYieldOfCoroutine(index, modeSwitch.subjectOperand(), yield, YieldResult::Mode))
        return false;

    BodyList<int64_t> cases = modeSwitch.cases();
    for (uint32_t place = 0; place < cases.size(); ++place) {
        if (cases[place] != static_cast<int64_t>(ResumeMode::Throw))
            continue;
        BodyBlock arm = index.block(terminator.targets()[place].block());
        if (arm.numberOfParameters() || arm.numberOfValues() || !blocksShareHandlerOfCoroutine(block, arm))
            return false;
        BodyTerminator endOfArm = arm.terminator();
        return endOfArm.kind() == TerminatorKind::Throw && isResultOfYieldOfCoroutine(index, endOfArm.as<TerminatorKind::Throw>().valueOperand(), yield, YieldResult::Sent);
    }
    return false;
}

// It is bound to its operand and nothing is built for it. A cast that tests its operand would test what has not been awaited.
static bool isCastBetweenAwaitAndYieldOfCoroutine(BodyValue operation)
{
    if (operation.opcode() == Opcode::Upcast)
        return true;
    return operation.opcode() == Opcode::Convert && !operation.as<Opcode::Convert>().declared().isEstablished();
}

// The Yield that follows the Await with only casts in between, where the only user of each value is the next operation. Null: the two stay separate suspensions.
static ValueID findYieldThatAloneTakesAwaitOfCoroutine(const BodyIndex& index, BodyBlock block, ValueID await)
{
    if (index.value(await).as<Opcode::Await>().takesExtraTurn())
        return { };

    ValueID value = await;
    for (uint32_t next = await.index() + 1; next < block.endOfValues().index(); ++next) {
        ValueID user(next);
        if (index.onlyUsingValue(value) != user)
            return { };
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::Yield) {
            if (operation.as<Opcode::Yield>().valueOperand() != value || !throwArmRethrowsSentValueOfCoroutine(index, block, user))
                return { };
            return user;
        }
        if (!isCastBetweenAwaitAndYieldOfCoroutine(operation))
            return { };
        value = user;
    }
    return { };
}

static ValueID findAwaitThatYieldAloneTakesOfCoroutine(const BodyIndex& index, BodyBlock block, ValueID yield)
{
    for (uint32_t place = yield.index(); place-- > block.firstValue().index();) {
        BodyValue operation = index.value(ValueID(place));
        if (operation.opcode() == Opcode::Await)
            return findYieldThatAloneTakesAwaitOfCoroutine(index, block, operation.id()) == yield ? operation.id() : ValueID();
        if (!isCastBetweenAwaitAndYieldOfCoroutine(operation))
            return { };
    }
    return { };
}

static bool runTimeAwaitsWhatCoroutineYields(Walker& walker)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    return body && body->kind == CoroutineKind::AsyncGenerator;
}

// The run time awaits the value and then yields what that gives, which is stock JSC's own form of `yield v`: the frame is saved and restored once.
static Reference buildAsyncGeneratorYieldOfCoroutine(Walker& walker, const SuspendingBodyOfCoroutine& body, Reference yielded, std::span<const Reference> held, Origin origin)
{
    Draft yield = draftForCoroutine(walker, Opcode::AsyncGeneratorYield, origin);
    yield.setGeneratorOperand(body.driver);
    yield.setValueOperand(yielded);
    yield.setHeldOperands(held);
    return buildSuspensionOfCoroutine(walker, WTF::move(yield));
}

// ---- The four operations

bool replaceCoroutineOperation(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!operationsOfFamilyCoroutine.contains(opcode))
        return false;

    if (replaceSuspensionOfCoroutine(walker, value))
        return true;

    // It only makes values live longer in the input.
    if (opcode == Opcode::ReadScope)
        return true;

    if (opcode == Opcode::InitialYield) {
        replaceInitialYieldOfCoroutine(walker, value);
        return true;
    }

    Draft written = walker.draftOf(value);
    Reference built;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(value.id()));
    if (opcode == Opcode::Await) {
        if (runTimeAwaitsWhatCoroutineYields(walker) && findYieldThatAloneTakesAwaitOfCoroutine(index, block, value.id())) {
            walker.bind(value.id(), written.valueOperand());
            return true;
        }
        built = buildAwaitBySavingFrame(walker, written.valueOperand(), written.heldOperands(), written.takesExtraTurn(), written.origin());
    } else if (std::optional<SuspendingBodyOfCoroutine> body = findYieldingBodyOfCoroutine(walker)) {
        // Whoever else has taken the Await has awaited: only one that still stands for its own operand is left to the run time.
        ValueID await = runTimeAwaitsWhatCoroutineYields(walker) ? findAwaitThatYieldAloneTakesOfCoroutine(index, block, value.id()) : ValueID();
        if (await && walker.referenceOf(await) == walker.referenceOf(index.value(await).as<Opcode::Await>().valueOperand()))
            built = buildAsyncGeneratorYieldOfCoroutine(walker, *body, written.valueOperand(), written.heldOperands(), written.origin());
        else
            built = buildYieldBySavingFrameOfCoroutine(walker, *body, written.valueOperand(), written.heldOperands(), written.origin());
    }
    if (built)
        walker.bind(value.id(), built);
    return true;
}

// ---- CodeRole::CoroutineBody

// A generator, or the promise of an async function, where undefined stands for no promise.
static TypeID typeOfDriverOfCoroutine(TypeArena& types, const Function& coroutine, FunctionID id)
{
    const ObjectTable& objects = types.objects();
    BuiltinClassID classOfResult = objects.classOf(objects.coroutineResultOf(id));
    TypeID driver = classOfResult ? types.builtinClass(classOfResult) : TypeTable::object;
    return coroutine.has(FunctionFlags::IsGenerator) ? driver : types.withPrimitives(driver, Primitive::Undefined);
}

// `entry` holds the entry parameters of the function and nothing more, and is not yet an entry of Code.
Reference addDriverParameterOfCoroutine(Walker& walker, BlockID entry)
{
    CodeUnit unit = walker.unit();
    const Function& function = walker.program().at(unit.function);
    return walker.addParameter(entry, typeOfDriverOfCoroutine(walker.types(), function, unit.function), walker.plan().conventions().parameterRepresentation(walker.program(), unit, function.numberOfEntryParameters()));
}

// What the unit returns in the place of what `return` hands over in the text. Of every other unit, and of a callee that is walked in line: the value as it came.
Reference convertReturnedValueOfCoroutine(Walker& walker, Reference returned, Origin origin)
{
    CodeUnit unit = walker.unit();
    if (unit.role != CodeRole::CoroutineBody || walker.currentFunction() != unit.function)
        return returned;
    if (!returned)
        returned = boxedUndefinedForCoroutine(walker);

    // A generator hands the value to whoever stepped it. Where nothing suspends the ramp resolves the promise.
    if (walker.program().at(unit.function).has(FunctionFlags::IsGenerator) || walker.plan().conventions().of(unit.function).coroutineNeverSuspends)
        return returned;

    std::optional<SuspendingBodyOfCoroutine> body = findSuspendingBodyOfCoroutine(walker);
    if (!body) {
        walker.fail("the body of an async function has no parameter for its driver"_s);
        return { };
    }
    Draft result = draftForCoroutine(walker, Opcode::ReturnAsyncResult, origin);
    result.setPromiseOperand(body->driver);
    result.setValueOperand(returned);
    return walker.build(WTF::move(result));
}

// ---- CodeRole::Body of a coroutine

// Code is reset for the unit and holds no block.
bool walkRampOfCoroutine(Walker& walker)
{
    CodeUnit unit = walker.unit();
    const Program& program = walker.program();
    const Function& function = program.at(unit.function);
    const CallingConventions& conventions = walker.plan().conventions();
    CodeUnit body { unit.function, CodeRole::CoroutineBody };
    if (unit.role != CodeRole::Body || !function.isCoroutine() || function.results.size() != 1 || !conventions.exists(body)) {
        walker.fail("a ramp was asked for of a function that is no coroutine with one result and a body"_s);
        return false;
    }

    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    bool neverSuspends = conventions.of(unit.function).coroutineNeverSuspends;
    if (isGenerator && neverSuspends) {
        walker.fail("the plan says that a generator never suspends"_s);
        return false;
    }

    // The ramp takes no more for granted than holds of every caller, and hands on what it got.
    Origin origin { .position = function.position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false };
    TypingContext withoutFacts { program, walker.types(), PositionTypes::knowingNothing(), TypeLevel::Proven };
    BlockID entry = walker.addBlock();
    Vector<Reference, 12> operands;
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        TypeID type = declaredEntryTypeOf(withoutFacts, unit.function, index);
        operands.append(walker.addParameter(entry, type ? type : TypeTable::anything, conventions.parameterRepresentation(program, unit, index)));
    }
    walker.code().addEntry(entry);
    walker.continueIn(entry);

    Reference driver;
    if (isGenerator) {
        std::optional<unsigned> calleeParameter = function.calleeParameterIndex();
        if (!calleeParameter) {
            walker.fail("a generator function does not take its function object, which its generators read their prototype from"_s);
            return false;
        }
        Draft allocation = draftForCoroutine(walker, function.has(FunctionFlags::IsAsync) ? Opcode::NewAsyncGenerator : Opcode::NewGenerator, origin);
        allocation.setFunction(unit.function);
        if (!nobodyLooksAboveGeneratorsOfCoroutine(walker, unit.function))
            allocation.setCalleeOperand(operands[*calleeParameter]);
        driver = walker.build(WTF::move(allocation));
    } else if (neverSuspends)
        driver = boxedUndefinedForCoroutine(walker);
    else
        driver = walker.build(draftForCoroutine(walker, Opcode::NewPromise, origin));
    if (!driver)
        return false;
    operands.append(driver);

    // It leaves no arrivals: what the callers of the ramp have left speaks of the parameters of the body too.
    Draft start = draftForCoroutine(walker, Opcode::CallDirect, origin);
    start.setFunction(body.function);
    start.setRole(body.role);
    start.setNumberOfBoundArguments(body.numberOfBoundArguments);
    start.setIsNeverInlined(true);
    start.setVariadicOperands(operands.span());
    Reference started = walker.build(WTF::move(start));
    if (!started)
        return false;

    // A body that throws in front of its first suspension hands back a new, rejected promise, and the one that it was given stays pending.
    // What a body that never suspends throws, the unwinder turns into a rejected promise in the frame of the ramp.
    Reference result = started;
    if (isGenerator)
        result = driver;
    else if (neverSuspends) {
        result = walker.build(draftForCoroutine(walker, Opcode::NewPromise, origin));
        if (!result)
            return false;
        resolvePromiseThatNothingHasTaken(walker, result, started, true, origin);
    }
    return endBlockOfCoroutine(walker, TerminatorKind::Return, result, origin);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
