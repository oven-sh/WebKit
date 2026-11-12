/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

bool answerLibraryQuestionOfCoroutines(Walker&, const BodyValue&);

static constexpr OpcodeSet operationsOfCoroutinePromises {
    Opcode::CreatePromise, Opcode::ResolvePromise, Opcode::Call, Opcode::HasJSType, Opcode::ReadsIteratorOfListUnseen
};

namespace {

struct FulfillmentInCoroutinePromises {
    ValueID promise;
    Reference value;
    BlockID block;
};

struct KeptByCoroutinePromises final : KeptForUnitOfCoroutines {
    Vector<FulfillmentInCoroutinePromises, 4> fulfillments;
};

} // anonymous namespace

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

static ValueID allocationOfPromiseInCoroutinePromises(const Walker& walker, Reference promise)
{
    if (!promise || !promise.isValue())
        return { };
    const Code& code = walker.code();
    ValueID made = underlyingCellOf(code, promise.id());
    return !code.isConstant(made) && code.at(made).opcode == Opcode::NewPromise ? made : ValueID();
}

static bool isFirstToMentionPromiseInCoroutinePromises(const BodyIndex& index, const BodyValue& resolution)
{
    ValueID promise = index.stripCasts(resolution.as<Opcode::ResolvePromise>().promiseOperand());
    if (!promise || index.isParameter(promise) || index.hasIrreducibleFlow() || index.loopOf(index.blockOf(promise)) != index.loopOf(index.blockOf(resolution.id())))
        return false;
    while (ValueID onlyUser = index.onlyUsingValue(promise)) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(onlyUser)))
            break;
        promise = onlyUser;
    }
    std::optional<BodyUser> first = index.firstUser(promise);
    return first && first->kind == BodyUseKind::Operand && first->value == resolution.id();
}

static bool nothingHasTakenPromiseInCoroutinePromises(Walker& walker, const BodyValue& resolution, Reference promise)
{
    ValueID made = allocationOfPromiseInCoroutinePromises(walker, promise);
    if (!made)
        return false;
    if (!walker.state().isFresh(Reference::value(made)))
        return false;
    return isFirstToMentionPromiseInCoroutinePromises(walker.currentBodyIndex(), resolution);
}

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

static void buildResolutionInCoroutinePromises(Walker& walker, Reference promise, Reference value, Origin origin, bool valueHasFailedTestForNoThen = false)
{
    Draft resolution = draftInCoroutinePromises(walker, Opcode::ResolvePromise, origin);
    resolution.setPromiseOperand(promise);
    resolution.setValueOperand(value);
    resolution.setValueHasFailedTestForNoThen(valueHasFailedTestForNoThen);
    if (!walker.has(Mechanism::TypedOperations) || !leansOnProofOfNoThen(walker, value, origin)) {
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

    if (leansOnProofOfNoThen(walker, value, origin)) {
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

    BlockID stores = walker.addBlock();
    BlockID looksUp = walker.addBlock(BlockFrequency::Rare);
    BlockID meet = walker.addBlock();
    walker.branch(hasNoThen, stores, looksUp);
    walker.recordSpeculation();

    walker.continueIn(stores);
    buildFulfillmentInCoroutinePromises(walker, promise, value, origin);
    walker.jumpTo(meet);

    walker.continueIn(looksUp);
    buildResolutionInCoroutinePromises(walker, promise, value, origin, true);
    walker.jumpTo(meet);
}

void resolvePromiseThatNothingHasTaken(Walker& walker, Reference promise, Reference value, bool mayTest, Origin origin)
{
    resolveInCoroutinePromises(walker, promise, value, mayTest, true, origin);
}

bool buildReturnsOfPromise(Walker& walker, Reference value, Origin origin, const ScopedLambda<bool(Reference promise)>& buildReturn)
{
    auto returnPromise = [&](const auto& resolve) {
        Reference promise = walker.build(draftInCoroutinePromises(walker, Opcode::NewPromise, origin));
        if (!promise)
            return false;
        resolve(promise);
        return buildReturn(promise);
    };
    auto fulfill = [&](Reference promise) {
        buildFulfillmentInCoroutinePromises(walker, promise, value, origin);
    };
    bool isTyped = walker.has(Mechanism::TypedOperations);
    if (isTyped && leansOnProofOfNoThen(walker, value, origin))
        return returnPromise(fulfill);
    Reference hasNoThen = isTyped && walker.has(Mechanism::Guesses) && walker.opensGuesses() ? buildTestForNoThen(walker, value, origin) : Reference();
    if (!hasNoThen || !hasNoThen.isValue()) {
        return returnPromise([&](Reference promise) {
            buildResolutionInCoroutinePromises(walker, promise, value, origin);
        });
    }

    BlockID fulfills = walker.addBlock();
    BlockID looksUp = walker.addBlock(BlockFrequency::Rare);
    walker.branch(hasNoThen, fulfills, looksUp);
    walker.recordSpeculation();

    walker.continueIn(fulfills);
    if (!returnPromise(fulfill))
        return false;
    walker.continueIn(looksUp);
    return returnPromise([&](Reference promise) {
        buildResolutionInCoroutinePromises(walker, promise, value, origin, true);
    });
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

static bool findsConstructorOfPromisesInCoroutinePromises(Walker& walker, Reference promise)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID found = findConstantMemberReadOff(walker, promise, program.keyOf(BuiltinNames::constructor));
    return found && table.canonicalID(found) == table.canonicalID(table.at(BuiltinClasses::Promise).constructor);
}

static Reference valueAwaitedInPlaceOfPromiseInCoroutinePromises(Walker& walker, Reference promise)
{
    if (!walker.has(Mechanism::EscapeAnalysis))
        return { };
    Reference value = valueOfPromiseFulfilledHere(walker, promise);
    if (!value || !isKnownPristine(walker, promise) || !findsConstructorOfPromisesInCoroutinePromises(walker, promise))
        return { };
    walker.verifyPristineObject(promise);
    return value;
}

Reference buildAwaitOfWhatHasNoThen(Walker& walker, Reference awaited, std::span<const Reference> held, Origin origin)
{
    if (!walker.has(Mechanism::TypedOperations))
        return { };
    Reference driver;
    std::optional<CoroutineKind> kind = kindOfSuspendingBody(walker, driver);
    if (!kind || *kind == CoroutineKind::Generator)
        return { };

    const Function& function = walker.program().at(walker.unit().function);
    if (Reference fulfilledWith = function.isModuleBody() ? Reference() : valueAwaitedInPlaceOfPromiseInCoroutinePromises(walker, awaited))
        awaited = fulfilledWith;
    if (!leansOnProofOfNoThen(walker, awaited, origin))
        return { };

    Draft await = draftInCoroutinePromises(walker, Opcode::AwaitBySavingFrame, origin);
    await.setAwaitedOperand(awaited);
    await.setPromiseOperand(driver);
    await.setHeldOperands(held);

    bool asksWhetherFrameMayGoOn = *kind == CoroutineKind::AsyncFunction && walker.isInLoop() && walker.has(Mechanism::Guesses) && walker.opensGuesses()
        && !walker.plan().moduleEvaluation().isBodyResumedByJobOfLoader(function);
    if (!asksWhetherFrameMayGoOn)
        return buildSuspensionInCoroutine(walker, WTF::move(await)) ? awaited : Reference();

    Reference mayGoOn = walker.build(draftInCoroutinePromises(walker, Opcode::AwaitMayContinue, origin));
    if (!mayGoOn)
        return { };

    BlockID suspends = walker.addBlock();
    BlockID goesOn = walker.addBlock();
    walker.branch(mayGoOn, goesOn, suspends);
    walker.recordSpeculation();
    walker.continueIn(suspends);
    walker.build(WTF::move(await));
    walker.state().invalidateHeap();
    walker.jumpTo(goesOn);
    return walker.isUnreachable() ? Reference() : awaited;
}

bool replaceSuspensionOfCoroutine(Walker& walker, const BodyValue& value)
{
    return value.opcode() == Opcode::Await && takeWhatCallInCopyHasAwaited(walker, value);
}

static bool buildCreatePromiseInCoroutinePromises(Walker& walker, const BodyValue& creation)
{
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
    if (!nothingHasTakenPromise && !leansOnProofOfNoThen(walker, value, walker.originOf(resolution)))
        return false;

    Origin origin = walker.originOf(resolution);
    if (nothingHasTakenPromise)
        resolveInCoroutinePromises(walker, promise, value, walker.isInLoop(), false, origin);
    else
        buildResolutionInCoroutinePromises(walker, promise, value, origin);
    return true;
}

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

    if (types.overlap(typeOfValue, typeOfPromisesInCoroutinePromises(walker))) {
        if (!promiseHasConstructorOfThisRealmInCoroutinePromises(walker, value))
            return false;
        walker.bind(call.id(), value);
        return true;
    }

    if (!leansOnProofOfNoThen(walker, value, origin))
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
