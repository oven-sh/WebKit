/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `await load(x)`: the Call of one known async function looks ahead to the Await that alone takes what it gives, and one operation enters the body without a promise and waits for it.
// At a suspension: the frame of the caller is saved once, and the callee's saved frame resumes it. At a throw: the callee hands back a rejected promise, which the stub awaits, so the throw arrives at the await.
// Withheld (DirectCalls, Inlining for the way through a copy, Guesses for the arms): the ramp is called and its promise is awaited.

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkCoroutinePromises.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- The wait, by the input

// It is walked in front of the call and the await that it is written between. So it reads nothing that the callee may write, and nobody can tell whether it has run.
static bool mayStandInFrontOfCallInAwaitedCalls(Opcode opcode)
{
    if (opcodesThatStateTypes.contains(opcode))
        return true;
    const Effects& bound = declaredBoundInTable(opcode);
    return bound.isUnobservable() && bound.reads.containsOnly(bookkeepingHeaps);
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
    if (await.valueOperand() != name || await.takesExtraTurn())
        return std::nullopt;
    wait.await = taker;
    wait.endOfWait = taker.index() - firstValue;
    if (wait.endOfWait >= walker.endOfStretchBeingWalked())
        return std::nullopt;

    for (uint32_t place = wait.placeOfCall + 1; place < wait.endOfWait; ++place) {
        ValueID between(firstValue + place);
        if (!mayStandInFrontOfCallInAwaitedCalls(index.opcodeOf(between)))
            return std::nullopt;
        // For such a read the walk builds more than the row says: the holder gets the member, which the order of its keys shows.
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
        // A cast of what the call gives has no operand yet, and builds nothing.
        if (opcodesThatStateTypes.contains(index.opcodeOf(between)) && index.valueBehindCasts(between) == call)
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

// ---- C2

// `await`: null in a copy, whose values are not those of the unit's body.
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
    return linked.effectsOnReturn.mayThrowOtherThanStackOverflow() || linked.effectsOnThrow.mayThrowOtherThanStackOverflow();
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
    return buildAwaitBySavingFrame(walker, promise, held, false, origin);
}

// The body returns and throws like a plain function: its ramp alone made a promise of either. A value without `then` fulfills a promise as it is, and awaiting the value takes as many turns of the job queue as awaiting that promise.
static Reference buildAwaitOfPlainBodyInAwaitedCalls(Walker& walker, const BodyValue& call, FunctionID callee, std::span<const Reference> operands, std::span<const Reference> held, Origin originOfAwait)
{
    Origin origin = walker.originOf(call);
    BlockID handler;
    if (bodyMayThrowInAwaitedCalls(walker, callee))
        handler = standUnderHandlerOfCoroutineCalls(walker);
    Reference returned = buildStartOfBodyOfCoroutine(walker, call, callee, operands, undefinedOfCoroutineCalls(walker));
    if (walker.hasFailed())
        return { };

    if (handler) {
        BlockID behind;
        if (returned) {
            behind = walker.addBlock();
            walker.jumpTo(behind);
        }
        // Nothing fulfills a promise that is rejected: the function goes on in the handler of the await or not at all.
        Reference caught = catchInHandlerOfCoroutineCalls(walker, handler, origin);
        if (caught && buildAwaitOfNewPromiseInAwaitedCalls(walker, Opcode::RejectPromise, caught, held, originOfAwait))
            walker.endAsUnreachable(originOfAwait);
        if (walker.hasFailed() || !returned)
            return { };
        walker.continueIn(behind);
    }
    if (!returned)
        return { };

    Reference hasNoThen = buildTestForNoThen(walker, returned, origin);
    if (!hasNoThen)
        return { };
    if (std::optional<bool> decided = constantBooleanInAwaitedCalls(walker, hasNoThen))
        return *decided ? buildAwaitBySavingFrame(walker, returned, held, false, originOfAwait) : buildAwaitOfNewPromiseInAwaitedCalls(walker, Opcode::ResolvePromise, returned, held, originOfAwait);

    BlockID awaitsValue = walker.addBlock();
    BlockID awaitsPromise = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();
    Reference result = walker.addParameter(meeting, { }, Representation::JSValue);
    bool someWayArrives = false;
    walker.branch(hasNoThen, awaitsValue, awaitsPromise);

    walker.continueIn(awaitsValue);
    if (Reference given = buildAwaitBySavingFrame(walker, returned, held, false, originOfAwait)) {
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

// ---- C3

// `const get = id => load(id); await get(1)`. Where the callee is copied, the call that its body returns at once is the call that the Await waits for.
static void noteAwaitForCopyInAwaitedCalls(Walker& walker, const BodyIndex& index, BodyBlock block, const BodyValue& call, const WaitOfCallInCoroutine& wait, Reference driver)
{
    if (!walker.has(Mechanism::Inlining) || call.isDoneInPlace())
        return;
    FunctionID callee = findOnlyCallee(walker, call, walker.referenceOf(call.calleeOperand()));
    // Beside the copy of a function with a guard stands a call of its slow version, which awaits nothing.
    if (!callee || walker.program().at(callee).isCoroutine() || walker.plan().conventions().hasParameterGuard(callee))
        return;

    uint32_t firstValue = block.firstValue().index();
    for (uint32_t place = wait.placeOfCall + 1; place < wait.endOfWait; ++place) {
        ValueID between(firstValue + place);
        if (!opcodesThatStateTypes.contains(index.opcodeOf(between)) || index.valueBehindCasts(between) != call.id())
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
    // In an arm of a guess: as in findWaitOfCallInCoroutine(). The copy would return the awaited value on one way and a promise on the other, and the await behind it is taken for both.
    if (walker.walksStretchInsideAnother() || walker.depthOfCopies() != 1 || !kept || !kept->awaitOfCallThatMayBeCopied || kept->awaitOfCallThatMayBeCopied->callee != walker.currentFunction())
        return false;

    // A rejection arrives at the await, which no handler of the copied function stands round.
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
    if (!writtenOperandsMayStartBodyOfCoroutine(walker, walker.draftOf(call), callee))
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

// ---- The link

bool buildAwaitedCall(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || !walker.has(Mechanism::DirectCalls))
        return false;
    if (walker.depthOfCopies())
        return buildCallOfCopyInAwaitedCalls(walker, call);

    std::optional<AwaitingBodyOfCoroutine> body = findAwaitingBodyOfCoroutine(walker);
    if (!body || body->kind != CoroutineKind::AsyncFunction)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    std::optional<WaitOfCallInCoroutine> wait = findWaitOfCallInCoroutine(walker, index, block, call.id());
    if (!wait)
        return false;

    FunctionID callee = findCoroutineCalledBy(walker, call);
    if (!callee) {
        noteAwaitForCopyInAwaitedCalls(walker, index, block, call, *wait, body->driver);
        return false;
    }
    const Function& function = walker.program().at(callee);
    if (!isAsyncFunctionInAwaitedCalls(function) || !writtenOperandsMayStartBodyOfCoroutine(walker, walker.draftOf(call), callee))
        return false;
    // The fused operation is no longer than the call and the await that it stands for. The arms round a body that never suspends are: they stand in loops, and wherever no handler is among them.
    bool neverSuspends = walker.plan().conventions().of(callee).coroutineNeverSuspends;
    if (neverSuspends && !walker.isInLoop() && bodyMayThrowInAwaitedCalls(walker, callee))
        return false;

    std::optional<Vector<Reference, 8>> operands = layOutEntryOperandsOfCall(walker, call, callee, { callee, CodeRole::CoroutineBody });
    if (!operands)
        return false;
    if (operands->size() != function.numberOfEntryParameters())
        return true;

    Vector<Reference, 4> held;
    if (!walkUpToAwaitOfCall(walker, index, block, *wait, held))
        return true;
    Reference given = neverSuspends
        ? buildAwaitOfPlainBodyInAwaitedCalls(walker, call, callee, operands->span(), held.span(), walker.originOf(index.value(wait->await)))
        : buildAwaitCallInAwaitedCalls(walker, call, callee, operands->span(), body->driver, held.span(), wait->await);
    if (given)
        finishWaitOfCall(walker, block, *wait, given);
    return true;
}

// ---- C4

// ---- Where the walk comes to an Await that a call in a copy has awaited for

bool takeWhatCallInCopyHasAwaited(Walker& walker, const BodyValue& await)
{
    KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker);
    if (await.opcode() != Opcode::Await || walker.depthOfCopies() || !kept || !kept->awaitsTakenInCopies.removeFirst(await.id()))
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
    if (!kept || walker.depthOfCopies() || kept->awaitsTakenInCopies.isEmpty())
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
