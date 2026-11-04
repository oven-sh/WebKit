/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, promises and awaits: a value without a `then` settles a promise that nothing has taken by a store, and whoever awaits it goes on with the value itself.
// At a suspension: CoroutineStateOfUnit::hasBuiltSuspension turns true for good, and what is noted of a fulfilled promise holds in the block of Code in which it was noted and nowhere else.
// At a throw: nothing. A promise is always built, and each store stands where the program settles: the sweep drops what nobody has come to need.
// Withheld: every operation of a promise is copied as written, every await saves the frame and gives what the job hands back.

#include "config.h"
#include "AOTWalkCoroutinePromises.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTPrimitives.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkCoroutineCalls.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkCoroutineLibraryQuestions.cpp.
bool answerLibraryQuestionOfCoroutines(Walker&, const BodyValue&);

static constexpr OpcodeSet operationsOfCoroutinePromises {
    Opcode::CreatePromise, Opcode::ResolvePromise, Opcode::Call, Opcode::HasJSType, Opcode::ReadsIteratorOfListUnseen
};

namespace {

struct FulfillmentInCoroutinePromises {
    // The NewPromise.
    ValueID promise;
    Reference value;
    BlockID block;
};

struct KeptByCoroutinePromises final : KeptForUnitOfCoroutines {
    // Of promises that stand for what a call of the program gives: no operation of the library's own settles one of them again. All of one block of Code: the one in which the last of them was built.
    Vector<FulfillmentInCoroutinePromises, 4> fulfillments;
};

} // anonymous namespace

// ---- What everything below is made of

static KeptByCoroutinePromises& keptByCoroutinePromises(Walker& walker)
{
    std::unique_ptr<KeptForUnitOfCoroutines>& kept = walker.coroutineState().keptByPromises;
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByCoroutinePromises>();
    return static_cast<KeptByCoroutinePromises&>(*kept);
}

static const KeptByCoroutinePromises* findKeptByCoroutinePromises(const Walker& walker)
{
    return static_cast<const KeptByCoroutinePromises*>(walker.coroutineState().keptByPromises.get());
}

static Draft draftInCoroutinePromises(Walker& walker, Opcode opcode, Origin origin)
{
    origin.isTailCall = false;
    Draft draft(opcode, origin);
    walker.chooseConservativeFields(draft);
    return draft;
}

static TypeID typeOfPromisesInCoroutinePromises(Walker& walker)
{
    return walker.types().builtinClass(BuiltinClasses::Promise);
}

// The emitters read the fields of a JSPromise, and the operand kind Object does not promise one.
static bool isProvenPromiseInCoroutinePromises(Walker& walker, Reference promise)
{
    TypeID type = promise ? walker.typeOf(promise) : TypeID();
    return type && walker.types().at(type).isObject() && walker.types().isSubset(type, typeOfPromisesInCoroutinePromises(walker));
}

static bool isProvenPromiseConstructorInCoroutinePromises(Walker& walker, Reference reference)
{
    TypeID id = reference ? walker.typeOf(reference) : TypeID();
    if (!id)
        return false;
    Type type = walker.types().at(id);
    if (!type.isObject())
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = singleBuiltinOf(walker.types(), type);
    return builtin && table.canonicalID(builtin) == table.canonicalID(table.at(BuiltinClasses::Promise).constructor);
}

// The NewPromise behind the names that the sweep looks through too. Null: something else has made the value.
static ValueID allocationOfPromiseInCoroutinePromises(const Walker& walker, Reference promise)
{
    if (!promise || !promise.isValue())
        return { };
    const Code& code = walker.code();
    ValueID made = rootOfCellInSweptCode(code, promise.id());
    return !code.isConstant(made) && code.at(made).opcode == Opcode::NewPromise ? made : ValueID();
}

// ---- Promises that nothing has taken

// By the input alone: from where the promise is made to the resolution nothing mentions it. A rejection and a store into an internal field leave a fresh promise fresh, and pending no longer.
// Whatever runs between the two in one turn stands between them in the stream, and the block that makes the promise dominates the resolution: in one innermost loop each run of the one follows a run of the other.
static bool isFirstToMentionPromiseInCoroutinePromises(const BodyIndex& index, const BodyValue& resolution)
{
    ValueID promise = index.valueBehindCasts(resolution.as<Opcode::ResolvePromise>().promiseOperand());
    if (!promise || index.isParameter(promise) || index.hasIrreducibleFlow() || index.loopOf(index.blockOf(promise)) != index.loopOf(index.blockOf(resolution.id())))
        return false;
    // Every other name of the promise is a cast of the last of these, and stands behind the first mention of that.
    while (ValueID onlyUser = index.onlyUsingValue(promise)) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(onlyUser)))
            break;
        promise = onlyUser;
    }
    std::optional<BodyUser> first = index.firstUser(promise);
    return first && first->kind == BodyUseKind::Operand && first->value == resolution.id();
}

// This code has made the promise, nothing else has it, and it is pending.
static bool nothingHasTakenPromiseInCoroutinePromises(Walker& walker, const BodyValue& resolution, Reference promise)
{
    ValueID made = allocationOfPromiseInCoroutinePromises(walker, promise);
    if (!made)
        return false;
    if (!walker.state().isFresh(Reference::value(made)))
        return false;
    return isFirstToMentionPromiseInCoroutinePromises(walker.currentBodyIndex(), resolution);
}

// It has the names and the prototype that a promise is made with, and nothing but this code has it.
static Reference buildPromiseOfRealmInCoroutinePromises(Walker& walker, const BodyValue& standsFor, Origin origin)
{
    Draft allocation = draftInCoroutinePromises(walker, Opcode::NewPromise, origin);
    allocation.inputValueGiven = standsFor.id();
    Reference promise = walker.build(WTF::move(allocation));
    if (promise && promise.isValue() && walker.has(Mechanism::EscapeAnalysis)) {
        walker.state().noteShape(promise, walker.code().at(promise.id()).type);
        walker.state().noteFresh(promise);
    }
    return promise;
}

// Resolving with an object reads its `then` at once, which may run a getter. A value without one settles the promise and runs no code, which leaves the effects that the row declares apart from the protocol.
static void buildResolutionInCoroutinePromises(Walker& walker, Reference promise, Reference value, Origin origin)
{
    Draft resolution = draftInCoroutinePromises(walker, Opcode::ResolvePromise, origin);
    resolution.setPromiseOperand(promise);
    resolution.setValueOperand(value);
    if (!walker.has(Mechanism::TypedOperations) || !isProvenToHaveNoThen(walker, value)) {
        walker.build(WTF::move(resolution));
        return;
    }
    Effects declared = declaredEffectsOfOperation(walker.program(), Opcode::ResolvePromise, resolution);
    walker.build(WTF::move(resolution), &declared);
}

static void buildFulfillmentInCoroutinePromises(Walker& walker, Reference promise, Reference value, Origin origin)
{
    Draft fulfillment = draftInCoroutinePromises(walker, Opcode::FulfillFreshPromise, origin);
    fulfillment.setPromiseOperand(promise);
    fulfillment.setValueOperand(value);
    walker.build(WTF::move(fulfillment));
}

static void resolveInCoroutinePromises(Walker& walker, Reference promise, Reference value, bool mayTest, bool standsForWhatCallGives, Origin origin)
{
    if (!walker.has(Mechanism::TypedOperations) || !isProvenPromiseInCoroutinePromises(walker, promise)) {
        buildResolutionInCoroutinePromises(walker, promise, value, origin);
        return;
    }

    if (isProvenToHaveNoThen(walker, value)) {
        buildFulfillmentInCoroutinePromises(walker, promise, value, origin);
        ValueID made = standsForWhatCallGives ? allocationOfPromiseInCoroutinePromises(walker, promise) : ValueID();
        if (!made || walker.isUnreachable() || !walker.has(Mechanism::EscapeAnalysis))
            return;
        KeptByCoroutinePromises& kept = keptByCoroutinePromises(walker);
        if (!kept.fulfillments.isEmpty() && kept.fulfillments.last().block != walker.currentBlock())
            kept.fulfillments.shrink(0);
        kept.fulfillments.append({ made, value, walker.currentBlock() });
        return;
    }

    Reference hasNoThen = mayTest && walker.has(Mechanism::Guesses) && walker.opensGuesses() ? buildTestForNoThen(walker, value, origin) : Reference();
    if (!hasNoThen || !hasNoThen.isValue()) {
        buildResolutionInCoroutinePromises(walker, promise, value, origin);
        return;
    }

    // The value is asked here, so that the way on which it has no `then` stores it and does nothing else.
    BlockID stores = walker.addBlock();
    BlockID looksUp = walker.addBlock(BlockFrequency::Rare);
    BlockID meet = walker.addBlock();
    walker.branch(hasNoThen, stores, looksUp);
    walker.noteGuessOpened();

    walker.continueIn(stores);
    buildFulfillmentInCoroutinePromises(walker, promise, value, origin);
    walker.jumpTo(meet);

    walker.continueIn(looksUp);
    buildResolutionInCoroutinePromises(walker, promise, value, origin);
    walker.jumpTo(meet);
}

void resolvePromiseThatNothingHasTaken(Walker& walker, Reference promise, Reference value, bool mayTest, Origin origin)
{
    resolveInCoroutinePromises(walker, promise, value, mayTest, true, origin);
}

Reference valueOfPromiseFulfilledHere(Walker& walker, Reference promise)
{
    const KeptByCoroutinePromises* kept = findKeptByCoroutinePromises(walker);
    ValueID made = kept ? allocationOfPromiseInCoroutinePromises(walker, promise) : ValueID();
    if (!made || !walker.state().isFresh(Reference::value(made)))
        return { };
    for (const FulfillmentInCoroutinePromises& fulfillment : kept->fulfillments) {
        if (fulfillment.promise == made && fulfillment.block == walker.currentBlock())
            return fulfillment.value;
    }
    return { };
}

// ---- The frame

// The stream may hold the body of a loop behind all that follows the loop (BodyIndex::membersOf()), so behind the head of a loop a suspension that lies on a way to here may be built yet.
bool frameWasNeverSuspended(const Walker& walker)
{
    return !walker.coroutineState().hasBuiltSuspension && !walker.depthOfCopies() && !walker.isInLoop() && !walker.code().hasLoops();
}

void noteSuspensionOfCoroutine(Walker& walker, Draft&)
{
    walker.coroutineState().hasBuiltSuspension = true;
}

bool bodyOfCoroutineMayBeCopied(const Walker& walker, FunctionID callee)
{
    return walker.has(Mechanism::Inlining) && walker.plan().conventions().of(callee).coroutineNeverSuspends;
}

// ---- Await

// `constructor` finds something else on an instance of a subclass, on a promise with a `constructor` of its own, and on one whose prototype was replaced.
static bool findsConstructorOfPromisesInCoroutinePromises(Walker& walker, Reference promise)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID found = findConstantMemberReadOff(walker, promise, program.keyOf(BuiltinNames::constructor));
    return found && table.canonicalID(found) == table.canonicalID(table.at(BuiltinClasses::Promise).constructor);
}

// The await of a promise of the realm that is fulfilled queues one job, and so does the await of what has no `then`. Nobody has the promise, so nobody can tell the two apart.
// Whoever RETURNS such a promise, or resolves another with it, calls its `then` in a job of its own: two turns more than the value takes. So they take the promise.
static Reference valueAwaitedInPlaceOfPromiseInCoroutinePromises(Walker& walker, Reference promise, bool takesExtraTurn)
{
    if (takesExtraTurn || !walker.has(Mechanism::EscapeAnalysis))
        return { };
    Reference value = valueOfPromiseFulfilledHere(walker, promise);
    if (!value || !isListAsMade(walker, promise) || !findsConstructorOfPromisesInCoroutinePromises(walker, promise))
        return { };
    return value;
}

Reference buildAwaitOfWhatHasNoThen(Walker& walker, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin origin)
{
    if (!walker.has(Mechanism::TypedOperations))
        return { };
    Reference driver;
    std::optional<CoroutineKind> kind = kindOfSuspendingBody(walker, driver);
    if (!kind || *kind == CoroutineKind::Generator)
        return { };

    // Which queue resumes the body of a module hangs on whether it awaits a promise.
    const Function& function = walker.program().at(walker.unit().function);
    if (Reference fulfilledWith = function.isModuleBody() ? Reference() : valueAwaitedInPlaceOfPromiseInCoroutinePromises(walker, awaited, takesExtraTurn))
        awaited = fulfilledWith;
    if (!isProvenToHaveNoThen(walker, awaited))
        return { };

    Draft await = draftInCoroutinePromises(walker, Opcode::AwaitBySavingFrame, origin);
    await.setAwaitedOperand(awaited);
    await.setPromiseOperand(driver);
    await.setHeldOperands(held);
    await.setTakesExtraTurn(takesExtraTurn);

    // The engine resumes an async generator. A require() of an ES module runs the jobs of the loader itself, so the engine decides which queue takes this one. Neither can skip the suspension on a test in line.
    bool asksWhetherFrameMayGoOn = *kind == CoroutineKind::AsyncFunction && !takesExtraTurn && walker.isInLoop() && walker.has(Mechanism::Guesses) && walker.opensGuesses()
        && !walker.plan().moduleEvaluation().isBodyResumedByJobOfLoader(function);
    if (!asksWhetherFrameMayGoOn)
        return buildSuspensionInCoroutine(walker, WTF::move(await)) ? awaited : Reference();

    // In front of the branch, so that the way that suspends and the way that does not come to their join with the same stores built. So the arm builds the suspension itself, past the door.
    noteSuspensionOfCoroutine(walker, await);
    // The await still takes its turn of the job queue, unless nobody can tell: a job has entered this frame as the last thing it does, and no other job waits.
    Reference mayGoOn = walker.build(draftInCoroutinePromises(walker, Opcode::AwaitMayContinue, origin));
    if (!mayGoOn)
        return { };

    BlockID suspends = walker.addBlock();
    BlockID goesOn = walker.addBlock();
    walker.branch(mayGoOn, goesOn, suspends);
    walker.noteGuessOpened();
    walker.continueIn(suspends);
    walker.build(WTF::move(await));
    walker.state().invalidateHeap();
    walker.jumpTo(goesOn);
    return walker.isUnreachable() ? Reference() : awaited;
}

// ---- Results that are returned in front of the first suspension

// The ramp has allocated the promise and still stands in the call that started the body, so nobody else has it. A caller that awaits the call itself hands in undefined, and takes the value out of the call.
Reference buildResultInFrontOfFirstSuspension(Walker& walker, Reference driver, Reference returned, Origin origin)
{
    if (!walker.has(Mechanism::TypedOperations) || !frameWasNeverSuspended(walker) || !isProvenToHaveNoThen(walker, returned))
        return { };
    TypeArena& types = walker.types();
    TypeID typeOfDriver = driver ? walker.typeOf(driver) : TypeID();
    if (!typeOfDriver || isNeverType(typeOfDriver))
        return { };
    Type type = types.at(typeOfDriver);
    if (!type.isJavaScriptValue() || (type.primitives & ~Primitive::Undefined))
        return { };
    if (!type.hasObjects())
        return returned;
    if (!types.isSubset(types.withOnlyObjects(typeOfDriver), typeOfPromisesInCoroutinePromises(walker)))
        return { };
    if (!type.primitives) {
        buildFulfillmentInCoroutinePromises(walker, driver, returned, origin);
        return walker.isUnreachable() ? Reference() : driver;
    }

    Draft test = draftInCoroutinePromises(walker, Opcode::TypeTest, origin);
    test.setValueOperand(driver);
    test.setTestedType(TypeTable::undefined);
    Reference hasNoPromise = walker.build(WTF::move(test));
    if (!hasNoPromise)
        return { };

    BlockID givesValue = walker.addBlock();
    BlockID fulfills = walker.addBlock();
    BlockID meet = walker.addBlock();
    Reference result = walker.addParameter(meet, { }, Representation::JSValue);
    walker.branch(hasNoPromise, givesValue, fulfills);

    walker.continueIn(givesValue);
    walker.jumpTo(meet, std::array { returned });

    walker.continueIn(fulfills);
    Draft narrowed = draftInCoroutinePromises(walker, Opcode::Narrowed, origin);
    narrowed.setValueOperand(driver);
    narrowed.setTestOperand(hasNoPromise);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(false);
    Reference promise = walker.build(WTF::move(narrowed));
    if (promise)
        buildFulfillmentInCoroutinePromises(walker, promise, returned, origin);
    walker.jumpTo(meet, std::array { promise });
    return walker.isUnreachable() ? Reference() : result;
}

// `yield v` in an async generator is one suspension since async-yields-1 (walk/AOTWalkCoroutines.cpp): its Yield asks whether the Await still stands for its own operand, so an Await that is taken here is seen there.

bool replaceSuspensionOfCoroutine(Walker& walker, const BodyValue& value)
{
    return value.opcode() == Opcode::Await && takeWhatEdgesHaveAwaited(walker, value);
}

BlockFrequency frequencyOfBlockBehindResumption(const Walker& walker, const BodyTerminator& terminator, uint32_t targetIndex)
{
    if (terminator.kind() != TerminatorKind::Switch)
        return BlockFrequency::Normal;
    auto modeSwitch = terminator.as<TerminatorKind::Switch>();
    BodyList<int64_t> cases = modeSwitch.cases();
    if (targetIndex >= cases.size() || cases[targetIndex] == static_cast<int64_t>(ResumeMode::Next))
        return BlockFrequency::Normal;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID subject = modeSwitch.subjectOperand();
    if (!subject || index.opcodeOf(subject) != Opcode::ResultAt)
        return BlockFrequency::Normal;
    auto read = index.value(subject).as<Opcode::ResultAt>();
    bool readsModeOfYield = read.index() == static_cast<uint32_t>(YieldResult::Mode) && index.opcodeOf(read.resultsOperand()) == Opcode::Yield;
    return readsModeOfYield ? BlockFrequency::Rare : BlockFrequency::Normal;
}

// ---- The operations of promises

static bool buildCreatePromiseInCoroutinePromises(Walker& walker, const BodyValue& creation)
{
    // The `prototype` property of Promise is neither writable nor configurable, so the read finds the prototype that NewPromise uses. Any other constructor has to be asked: a subclass, a bound function, a Proxy.
    if (!walker.has(Mechanism::TypedOperations) || !isProvenPromiseConstructorInCoroutinePromises(walker, walker.referenceOf(creation.as<Opcode::CreatePromise>().constructorOperand())))
        return false;
    if (Reference promise = buildPromiseOfRealmInCoroutinePromises(walker, creation, walker.originOf(creation)))
        walker.bind(creation.id(), promise);
    return true;
}

static bool buildResolvePromiseInCoroutinePromises(Walker& walker, const BodyValue& resolution)
{
    auto written = resolution.as<Opcode::ResolvePromise>();
    Reference promise = walker.referenceOf(written.promiseOperand());
    Reference value = walker.referenceOf(written.valueOperand());
    if (!promise || !value)
        return false;
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    bool nothingHasTakenPromise = nothingHasTakenPromiseInCoroutinePromises(walker, resolution, promise);
    if (!nothingHasTakenPromise && !isProvenToHaveNoThen(walker, value))
        return false;

    Origin origin = walker.originOf(resolution);
    if (nothingHasTakenPromise)
        resolveInCoroutinePromises(walker, promise, value, walker.isInLoop(), false, origin);
    else
        buildResolutionInCoroutinePromises(walker, promise, value, origin);
    return true;
}

// A promise from another realm can be as it was made and still have that realm's Promise as its constructor. What this program has allocated, or has got from a built-in that it called, belongs to the realm of the running code.
static bool promiseBelongsToThisRealmInCoroutinePromises(Walker& walker, Reference promise, const Type& type)
{
    return (type.hasOnlyNamedObjects() && !type.namesMergedObjects()) || allocationOfPromiseInCoroutinePromises(walker, promise);
}

static bool promiseHasConstructorOfThisRealmInCoroutinePromises(Walker& walker, Reference promise)
{
    Type type = walker.types().at(walker.typeOf(promise));
    if (!type.isObject() || !type.hasOnlyClassObjects())
        return false;
    const ClassPart* part = singleBuiltinClassOf(walker.types(), type);
    if (!part || part->builtinClass != BuiltinClasses::Promise)
        return false;
    return promiseBelongsToThisRealmInCoroutinePromises(walker, promise, type) && findsConstructorOfPromisesInCoroutinePromises(walker, promise);
}

static bool buildCallOfPromiseResolveInCoroutinePromises(Walker& walker, const BodyValue& call)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::BuiltinCalls))
        return false;
    auto written = call.as<Opcode::Call>();
    std::span<const ValueID> arguments = written.argumentsOperands();
    if (arguments.size() > 1 || !written.thisValueOperand() || written.isDoneInPlace())
        return false;

    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    Reference callee = walker.referenceOf(written.calleeOperand());
    TypeID typeOfCallee = callee ? walker.typeOf(callee) : TypeID();
    if (!typeOfCallee || !types.at(typeOfCallee).isObject())
        return false;
    BuiltinID called = singleBuiltinOf(types, types.at(typeOfCallee));
    if (!called || table.canonicalID(called) != table.canonicalID(Builtins::PromiseResolve))
        return false;
    if (!isProvenPromiseConstructorInCoroutinePromises(walker, walker.referenceOf(written.thisValueOperand())))
        return false;

    Origin origin = walker.originOf(call);
    Reference value = arguments.empty() ? walker.constant(Draft(Opcode::Undefined, origin)) : walker.referenceOf(arguments[0]);
    TypeID typeOfValue = value ? walker.typeOf(value) : TypeID();
    if (!typeOfValue)
        return false;

    // A look at the loop has typed the call by its rows.
    auto liesWithinWhatLooksHaveSeen = [&](TypeID given) {
        TypeID widest = walker.isInLoop() ? widestTypeOfOperation(walker, call) : given;
        return widest && types.isSubset(given, widest);
    };

    // Promise.resolve returns a promise as it is if its `constructor` is the receiver. Where that is not proven, the call keeps its own form, which asks at run time.
    if (types.overlap(typeOfValue, typeOfPromisesInCoroutinePromises(walker))) {
        if (!promiseHasConstructorOfThisRealmInCoroutinePromises(walker, value))
            return false;
        if (!liesWithinWhatLooksHaveSeen(typeOfValue))
            return false;
        walker.bind(call.id(), value);
        return true;
    }

    if (!isProvenToHaveNoThen(walker, value))
        return false;
    if (!liesWithinWhatLooksHaveSeen(typeOfPromisesInCoroutinePromises(walker)))
        return false;
    Reference promise = buildPromiseOfRealmInCoroutinePromises(walker, call, origin);
    if (!promise)
        return true;
    resolvePromiseThatNothingHasTaken(walker, promise, value, false, origin);
    if (!walker.isUnreachable())
        walker.bind(call.id(), promise);
    return true;
}

bool buildPromiseOperation(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!operationsOfCoroutinePromises.contains(opcode))
        return false;
    switch (opcode) {
    case Opcode::CreatePromise:
        return buildCreatePromiseInCoroutinePromises(walker, value);
    case Opcode::ResolvePromise:
        return buildResolvePromiseInCoroutinePromises(walker, value);
    case Opcode::Call:
        return buildCallOfPromiseResolveInCoroutinePromises(walker, value);
    default:
        return answerLibraryQuestionOfCoroutines(walker, value);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
