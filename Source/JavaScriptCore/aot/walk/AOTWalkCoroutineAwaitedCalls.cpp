/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkCoroutinePromises.h"

namespace JSC { namespace AOT { namespace Typed {

static bool mayStandInFrontOfCallInAwaitedCalls(Opcode opcode)
{
    if (opcodesThatStateTypes.contains(opcode))
        return true;
    const Effects& bound = declaredBoundInTable(opcode);
    return bound.isUnobservable() && bound.reads.containsOnly(bookkeepingHeaps) && !operandsMayRunCode(opcode);
}

std::optional<WaitOfCallInCoroutine> findWaitOfCallInCoroutine(Walker& walker, const BodyIndex& index, BodyBlock block, ValueID call)
{
    uint32_t firstValue = block.firstValue().index();
    WaitOfCallInCoroutine wait;
    wait.placeOfCall = call.index() - firstValue;

    ValueID name = call;
    ValueID taker = followCastsToOnlyTaker(index, name);
    if (!taker || index.opcodeOf(taker) != Opcode::Await || !block.holds(taker))
        return std::nullopt;
    BodyValue await = index.value(taker);
    if (await.valueOperand() != name)
        return std::nullopt;
    wait.await = taker;
    wait.endOfWait = taker.index() - firstValue;
    if (wait.endOfWait >= walker.endOfStretchBeingWalked())
        return std::nullopt;

    for (uint32_t place = wait.placeOfCall + 1; place < wait.endOfWait; ++place) {
        ValueID between(firstValue + place);
        if (!mayStandInFrontOfCallInAwaitedCalls(index.opcodeOf(between)))
            return std::nullopt;
        if (index.opcodeOf(between) == Opcode::Builtin && firstReadOfNamedBuiltinIsSeen(walker, index.value(between)))
            return std::nullopt;
    }
    return wait;
}

bool walkUpToAwaitOfCall(Walker& walker, const BodyIndex& index, BodyBlock block, const WaitOfCallInCoroutine& wait, Vector<Reference, 4>& held)
{
    uint32_t firstValue = block.firstValue().index();
    ValueID call(firstValue + wait.placeOfCall);
    for (uint32_t place = wait.placeOfCall + 1; place < wait.endOfWait; ++place) {
        ValueID between(firstValue + place);
        if (opcodesThatStateTypes.contains(index.opcodeOf(between)) && index.stripCasts(between) == call)
            continue;
        walker.walkStretch(block.id(), place, place + 1);
        if (walker.isUnreachable() || walker.hasFailed())
            return false;
    }

    for (ValueID operand : index.value(wait.await).heldOperands()) {
        Reference reference = walker.referenceOf(operand);
        if (!reference) {
            walker.fail("an await holds a value of the body that nothing was built for"_s);
            return false;
        }
        held.append(reference);
    }
    return true;
}

void finishWaitOfCall(Walker& walker, BodyBlock block, const WaitOfCallInCoroutine& wait, Reference given)
{
    walker.bind(wait.await, given);
    leaveAnythingForSwallowedOperation(walker, wait.await);
    walker.walkStretch(block.id(), wait.endOfWait + 1, wait.endOfWait + 1);
}

static Reference buildAwaitCallInAwaitedCalls(Walker& walker, const BodyValue& call, FunctionID callee, std::span<const Reference> operands, Reference driver, std::span<const Reference> held, ValueID await)
{
    Draft fused = draftOfCoroutineCalls(walker, Opcode::AwaitCallBySavingFrame, walker.originOf(call));
    fused.setFunction(callee);
    fused.setRole(CodeRole::CoroutineBody);
    fused.setNumberOfBoundArguments(0);
    fused.setPromiseOperand(driver);
    Vector<Reference, 12> arguments(operands);
    arguments.append(held);
    fused.setArgumentsOperands(arguments.span());
    fused.setNumberOfHeld(held.empty() ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(held.size())));
    fused.inputValueGiven = await;
    Reference given = buildSuspensionInCoroutine(walker, WTF::move(fused));
    if (!walker.hasFailed())
        walker.leaveArrivals(callee, operands);
    return given;
}

static bool bodyMayThrowInAwaitedCalls(const Walker& walker, FunctionID callee)
{
    const LinkedFunction& linked = walker.facts().linked().function(callee);
    return linked.effectsOnReturn().mayThrowOtherThanStackOverflow() || linked.effectsOnThrow().mayThrowOtherThanStackOverflow();
}

static bool resultHasNoThenInAwaitedCalls(const Walker& walker, FunctionID callee)
{
    return hasNoThen(walker, walker.plan().typeHeldOfResult(walker.program(), walker.facts().linked(), CodeUnit { callee, CodeRole::CoroutineBody }));
}

static std::optional<bool> constantBooleanInAwaitedCalls(const Walker& walker, Reference reference)
{
    if (!reference.isRecord())
        return std::nullopt;
    const Operation& record = walker.code().at(reference.id());
    return record.opcode == Opcode::Boolean ? std::optional<bool>(record.boolean()) : std::nullopt;
}

static Reference buildAwaitOfNewPromiseInAwaitedCalls(Walker& walker, Opcode settlement, Reference value, std::span<const Reference> held, Origin origin)
{
    Reference promise = buildNewPromiseOfCoroutineCalls(walker, origin);
    if (!promise || !buildSettlementOfCoroutineCalls(walker, settlement, promise, value, origin))
        return { };
    return buildAwaitBySavingFrame(walker, promise, held, origin);
}

static Reference buildAwaitOfPlainBodyInAwaitedCalls(Walker& walker, const BodyValue& call, FunctionID callee, CoroutineKind kindOfAwaitingBody, std::span<const Reference> operands, std::span<const Reference> held, Origin originOfAwait)
{
    Origin origin = walker.originOf(call);
    BlockID handler;
    if (bodyMayThrowInAwaitedCalls(walker, callee))
        handler = standUnderHandlerOfCoroutineCalls(walker);
    Draft start = draftOfCoroutineCalls(walker, Opcode::CallDirect, origin);
    start.setFunction(callee);
    start.setRole(CodeRole::CoroutineBody);
    start.setNumberOfBoundArguments(0);
    start.setVariadicOperands(operands);
    bool isCopied = !walker.hasHandlerMadeOverCopies() && shouldCopyCallee(walker, start, callee, operands) && walker.walkInlineUnderHandlerMade(call, callee, operands, start, handler);
    Reference returned = !isCopied ? buildStartOfBodyOfCoroutine(walker, call, callee, operands, undefinedOfCoroutineCalls(walker)) : walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    if (walker.hasFailed())
        return { };

    if (handler) {
        BlockID behind;
        if (returned) {
            behind = walker.addBlock();
            walker.jumpTo(behind);
        }
        Reference caught = catchInHandlerOfCoroutineCalls(walker, handler, origin);
        if (caught && buildAwaitOfNewPromiseInAwaitedCalls(walker, Opcode::RejectPromise, caught, held, originOfAwait))
            walker.endAsUnreachable(originOfAwait);
        if (walker.hasFailed() || !returned)
            return { };
        walker.continueIn(behind);
    }
    if (!returned)
        return { };

    if (kindOfAwaitingBody == CoroutineKind::AsyncGenerator || walker.program().at(walker.unit().function).isModuleBody()) {
        Reference promise = buildNewPromiseOfCoroutineCalls(walker, origin);
        if (!promise)
            return { };
        resolvePromiseThatNothingHasTaken(walker, promise, returned, true, origin);
        return walker.isUnreachable() ? Reference() : buildAwaitBySavingFrame(walker, promise, held, originOfAwait);
    }

    Reference hasNoThen = buildTestForNoThen(walker, returned, origin);
    if (!hasNoThen)
        return { };
    if (std::optional<bool> decided = constantBooleanInAwaitedCalls(walker, hasNoThen))
        return *decided ? buildAwaitBySavingFrame(walker, returned, held, originOfAwait) : buildAwaitOfNewPromiseInAwaitedCalls(walker, Opcode::ResolvePromise, returned, held, originOfAwait);

    BlockID awaitsValue = walker.addBlock();
    BlockID awaitsPromise = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    Reference result = walker.addParameter(meeting, { }, Representation::JSValue);
    bool someWayArrives = false;
    walker.branch(hasNoThen, awaitsValue, awaitsPromise);

    walker.continueIn(awaitsValue);
    if (Reference given = buildAwaitBySavingFrame(walker, returned, held, originOfAwait)) {
        walker.jumpTo(meeting, singleElementSpan(given));
        someWayArrives = true;
    }
    walker.continueIn(awaitsPromise);
    if (Reference given = buildAwaitOfNewPromiseInAwaitedCalls(walker, Opcode::ResolvePromise, returned, held, originOfAwait)) {
        walker.jumpTo(meeting, singleElementSpan(given));
        someWayArrives = true;
    }
    if (!someWayArrives || walker.hasFailed())
        return { };
    walker.continueIn(meeting);
    return result;
}

static bool isAsyncFunctionInAwaitedCalls(const Function& function)
{
    return function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator);
}

static void noteAwaitForCopyInAwaitedCalls(Walker& walker, const BodyIndex& index, BodyBlock block, const BodyValue& call, const WaitOfCallInCoroutine& wait, Reference driver)
{
    if (!walker.has(Mechanism::Inlining) || call.isDoneInPlace())
        return;
    FunctionID callee = findOnlyCallee(walker, call, walker.referenceOf(call.calleeOperand()));
    if (!callee || walker.program().at(callee).isCoroutine() || walker.plan().conventions().hasParameterGuard(callee))
        return;

    uint32_t firstValue = block.firstValue().index();
    for (uint32_t place = wait.placeOfCall + 1; place < wait.endOfWait; ++place) {
        ValueID between(firstValue + place);
        if (!opcodesThatStateTypes.contains(index.opcodeOf(between)) || index.stripCasts(between) != call.id())
            return;
    }

    KeptByCallsOfCoroutines::AwaitOfCallThatMayBeCopied note { callee, wait.await, driver, { } };
    for (ValueID operand : index.value(wait.await).heldOperands()) {
        Reference reference = walker.referenceOf(operand);
        if (!reference)
            return;
        note.held.append(reference);
    }
    keptByCallsOfCoroutines(walker).awaitOfCallThatMayBeCopied = WTF::move(note);
}

static bool hasOneReturnInAwaitedCalls(const BodyIndex& index)
{
    unsigned numberOfReturns = 0;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        TerminatorKind kind = index.terminator(BlockID(number)).kind();
        numberOfReturns += kind == TerminatorKind::Return || kind == TerminatorKind::ReturnCall;
    }
    return numberOfReturns == 1;
}

static bool buildCallOfCopyInAwaitedCalls(Walker& walker, const BodyValue& call)
{
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (walker.walksStretchInsideAnother() || walker.depthOfCopies() != 1 || !kept || !kept->awaitOfCallThatMayBeCopied || kept->awaitOfCallThatMayBeCopied->callee != walker.currentFunction())
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    BodyTerminator end = block.terminator();
    if (end.kind() != TerminatorKind::ReturnCall || end.operand(0) != call.id() || call.id().index() + 1 != block.endOfValues().index() || block.hasHandler())
        return false;
    if (!hasOneReturnInAwaitedCalls(index))
        return false;

    FunctionID callee = findCoroutineCalledBy(walker, call);
    if (!callee)
        return false;
    const Function& function = walker.program().at(callee);
    if (!isAsyncFunctionInAwaitedCalls(function) || walker.plan().conventions().of(callee).coroutineNeverSuspends)
        return false;
    if (!writtenOperandsMayStartBodyOfCoroutine(walker, walker.draftOf(call)))
        return false;

    std::optional<Vector<Reference, 8>> operands = layOutEntryOperandsOfCall(walker, call, callee, { callee, CodeRole::CoroutineBody });
    if (!operands)
        return false;
    if (operands->size() != function.numberOfEntryParameters())
        return true;

    KeptByCallsOfCoroutines::AwaitOfCallThatMayBeCopied note = WTF::move(*kept->awaitOfCallThatMayBeCopied);
    kept->awaitOfCallThatMayBeCopied = std::nullopt;
    Reference given = buildAwaitCallInAwaitedCalls(walker, call, callee, operands->span(), note.driver, note.held.span(), { });
    if (!given)
        return true;
    walker.bind(call.id(), given);
    kept->awaitsTakenInCopies.append(note.await);
    return true;
}

bool buildAwaitedCall(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || !walker.has(Mechanism::DirectCalls))
        return false;
    if (!walker.walksOwnBodyOfUnit())
        return buildCallOfCopyInAwaitedCalls(walker, call);

    std::optional<AwaitingBodyOfCoroutine> body = findAwaitingBodyOfCoroutine(walker);
    if (!body)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    std::optional<WaitOfCallInCoroutine> wait = findWaitOfCallInCoroutine(walker, index, block, call.id());
    if (!wait)
        return false;

    FunctionID callee = findCoroutineCalledBy(walker, call);
    if (!callee) {
        if (body->kind == CoroutineKind::AsyncFunction)
            noteAwaitForCopyInAwaitedCalls(walker, index, block, call, *wait, body->driver);
        return false;
    }
    const Function& function = walker.program().at(callee);
    if (!isAsyncFunctionInAwaitedCalls(function) || !writtenOperandsMayStartBodyOfCoroutine(walker, walker.draftOf(call)))
        return false;
    bool neverSuspends = walker.plan().conventions().of(callee).coroutineNeverSuspends;
    if (!neverSuspends && body->kind != CoroutineKind::AsyncFunction)
        return false;
    bool rejectsInStub = neverSuspends && !walker.isInLoop() && bodyMayThrowInAwaitedCalls(walker, callee);
    if (rejectsInStub && (body->kind != CoroutineKind::AsyncFunction || walker.program().at(walker.unit().function).isModuleBody() || !resultHasNoThenInAwaitedCalls(walker, callee)))
        return false;

    std::optional<Vector<Reference, 8>> operands = layOutEntryOperandsOfCall(walker, call, callee, { callee, CodeRole::CoroutineBody });
    if (!operands)
        return false;
    if (operands->size() != function.numberOfEntryParameters())
        return true;

    Vector<Reference, 4> held;
    if (!walkUpToAwaitOfCall(walker, index, block, *wait, held))
        return true;
    Reference given = neverSuspends && !rejectsInStub
        ? buildAwaitOfPlainBodyInAwaitedCalls(walker, call, callee, body->kind, operands->span(), held.span(), walker.originOf(index.value(wait->await)))
        : buildAwaitCallInAwaitedCalls(walker, call, callee, operands->span(), body->driver, held.span(), wait->await);
    if (given)
        finishWaitOfCall(walker, block, *wait, given);
    return true;
}

bool takeWhatCallInCopyHasAwaited(Walker& walker, const BodyValue& await)
{
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (await.opcode() != Opcode::Await || !walker.walksOwnBodyOfUnit() || !kept || !kept->awaitsTakenInCopies.removeFirst(await.id()))
        return false;
    Reference given = walker.referenceOf(await.valueOperand());
    if (!given) {
        walker.fail("nothing was built for what an await takes that was built ahead of its place"_s);
        return true;
    }
    walker.bind(await.id(), given);
    return true;
}

bool nameCastInFrontOfAwaitTakenInCopy(Walker& walker, const BodyValue& cast)
{
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (!kept || !walker.walksOwnBodyOfUnit() || kept->awaitsTakenInCopies.isEmpty())
        return false;
    if (!kept->awaitsTakenInCopies.contains(findOnlyTakerBehindCasts(walker.currentBodyIndex(), cast.id())))
        return false;
    Reference given = walker.referenceOf(cast.operand(0));
    if (!given)
        return false;
    walker.bind(cast.id(), given);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
