/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkCoroutinePromises.h"

namespace JSC { namespace AOT { namespace Typed {

KeptByCallsOfCoroutines* findKeptByCallsOfCoroutines(Walker& walker)
{
    return static_cast<KeptByCallsOfCoroutines*>(walker.coroutineState().keptByCalls.get());
}

KeptByCallsOfCoroutines& keptByCallsOfCoroutines(Walker& walker)
{
    std::unique_ptr<KeptForUnitOfCoroutines>& kept = walker.coroutineState().keptByCalls;
    if (!kept)
        kept = makeUnique<KeptByCallsOfCoroutines>();
    return static_cast<KeptByCallsOfCoroutines&>(*kept);
}

std::optional<AwaitingBodyOfCoroutine> findAwaitingBodyOfCoroutine(Walker& walker)
{
    Reference driver;
    std::optional<CoroutineKind> kind = kindOfSuspendingBody(walker, driver);
    if (!kind || *kind == CoroutineKind::Generator)
        return std::nullopt;
    return AwaitingBodyOfCoroutine { *kind, driver };
}

Draft draftOfCoroutineCalls(Walker& walker, Opcode opcode, Origin origin)
{
    origin.isTailCall &= origin.inlineFrame && opcode == Opcode::AwaitCallBySavingFrame;
    Draft draft(opcode, origin);
    walker.chooseConservativeFields(draft);
    return draft;
}

Reference undefinedOfCoroutineCalls(Walker& walker)
{
    Draft undefined(Opcode::Undefined);
    undefined.representation = Representation::JSValue;
    return walker.constant(WTF::move(undefined));
}

Reference buildNewPromiseOfCoroutineCalls(Walker& walker, Origin origin)
{
    return walker.build(draftOfCoroutineCalls(walker, Opcode::NewPromise, origin));
}

bool buildSettlementOfCoroutineCalls(Walker& walker, Opcode opcode, Reference promise, Reference value, Origin origin)
{
    Draft settlement = draftOfCoroutineCalls(walker, opcode, origin);
    settlement.setPromiseOperand(promise);
    settlement.setValueOperand(value);
    if (opcode == Opcode::ResolvePromise)
        settlement.setValueHasFailedTestForNoThen(true);
    walker.build(WTF::move(settlement));
    return !walker.isUnreachable();
}

Reference proveOrTrapInCoroutineCalls(Walker& walker, Reference value, TypeID promised, Origin origin)
{
    TypeArena& types = walker.types();
    if (types.isSubset(walker.typeOf(value), promised))
        return value;

    Draft test = draftOfCoroutineCalls(walker, Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(types.testablePartOf(promised));
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return { };

    BlockID passing = walker.addBlock();
    BlockID failing = walker.addBlock(BlockFrequency::Rare);
    walker.branch(passes, passing, failing);
    walker.continueIn(failing);
    walker.code().end(failing, TerminatorDraft(TerminatorKind::Unreachable, origin), 0);
    walker.continueIn(passing);

    Draft narrowed = draftOfCoroutineCalls(walker, Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return walker.build(WTF::move(narrowed));
}

BlockID standUnderHandlerOfCoroutineCalls(Walker& walker)
{
    Code& code = walker.code();
    BlockID handler = code.addBlock(BlockRole::CatchHandler, BlockFrequency::Rare);
    code.setLoopMarks(handler, walker.isInLoop(), false);
    walker.jumpTo(walker.addBlock());
    code.closeEdge(code.addHandlerEdge(walker.currentBlock()), handler, { });
    return handler;
}

Reference catchInHandlerOfCoroutineCalls(Walker& walker, BlockID handler, Origin origin)
{
    walker.continueIn(handler);
    walker.state().clear();
    return walker.build(draftOfCoroutineCalls(walker, Opcode::CatchValue, origin));
}

bool isCellOfFrameInCoroutineCalls(const Walker& walker, Reference operand)
{
    if (!operand || !operand.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& made = code.at(underlyingCellOf(code, operand.id()));
    return PayloadWord::storage(made.opcode) && made.storage() != AllocationStorage::Heap;
}

bool writtenOperandsMayStartBodyOfCoroutine(Walker& walker, const Draft& written)
{
    bool holdsCellOfFrame = false;
    written.forEachOperand([&](Reference operand, uint32_t) {
        holdsCellOfFrame |= isCellOfFrameInCoroutineCalls(walker, operand);
    });
    return !holdsCellOfFrame;
}

void leaveAnythingForSwallowedOperation(Walker& walker, ValueID swallowed)
{
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), swallowed))
        walker.leaveAnythingAt(listed);
}

ValueID followCastsToOnlyTaker(const BodyIndex& index, ValueID& name)
{
    while (true) {
        ValueID taker = index.onlyUsingValue(name);
        if (!taker || !opcodesThatStateTypes.contains(index.opcodeOf(taker)))
            return taker;
        if (index.value(taker).operand(0) != name)
            return { };
        name = taker;
    }
}

ValueID findOnlyTakerBehindCasts(const BodyIndex& index, ValueID value)
{
    return followCastsToOnlyTaker(index, value);
}

FunctionID findCoroutineCalledBy(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace() || !walker.has(Mechanism::DirectCalls))
        return { };
    FunctionID callee = findOnlyCallee(walker, call, walker.referenceOf(call.calleeOperand()));
    if (!callee)
        return { };
    const Function& function = walker.program().at(callee);
    if (!function.isCoroutine() || function.isModuleBody() || !function.hasBody() || function.results.size() != 1)
        return { };
    return walker.plan().conventions().exists({ callee, CodeRole::CoroutineBody }) ? callee : FunctionID();
}

static Effects effectsOfEnteringInCoroutineCalls(const Walker& walker, FunctionID callee)
{
    const LinkedFunction& linked = walker.facts().linked().function(callee);
    Effects effects = linked.effectsOnReturn();
    effects.merge(linked.effectsOnThrow());
    effects.mayThrow |= ThrowEffect::onlyStackOverflow();
    return effects;
}

Reference buildStartOfBodyOfCoroutine(Walker& walker, const BodyValue& call, FunctionID callee, std::span<const Reference> entryOperands, Reference driver)
{
    Draft start = draftOfCoroutineCalls(walker, Opcode::CallDirect, walker.originOf(call));
    start.setFunction(callee);
    start.setRole(CodeRole::CoroutineBody);
    start.setNumberOfBoundArguments(0);
    start.setIsNeverInlined(true);
    Vector<Reference, 12> operands(entryOperands);
    operands.append(driver);
    start.setVariadicOperands(operands.span());

    Effects effects = effectsOfEnteringInCoroutineCalls(walker, callee);
    Reference started = walker.build(WTF::move(start), &effects);
    if (!walker.hasFailed())
        walker.leaveArrivals(callee, entryOperands);
    return started;
}

static Reference buildPromiseOfStartedBodyInCoroutineCalls(Walker& walker, const BodyValue& call, FunctionID callee, std::span<const Reference> operands, Origin origin)
{
    BlockID handler = standUnderHandlerOfCoroutineCalls(walker);
    Reference returned = buildStartOfBodyOfCoroutine(walker, call, callee, operands, undefinedOfCoroutineCalls(walker));
    if (walker.hasFailed())
        return { };

    BlockID meeting = walker.addBlock();
    Reference result = walker.addParameter(meeting, { }, Representation::JSValue);
    bool someWayArrives = false;

    if (returned) {
        walker.jumpTo(walker.addBlock());
        if (Reference promise = buildNewPromiseOfCoroutineCalls(walker, origin)) {
            resolvePromiseThatNothingHasTaken(walker, promise, returned, true, origin);
            if (!walker.isUnreachable()) {
                walker.jumpTo(meeting, singleElementSpan(promise));
                someWayArrives = true;
            }
        }
    }

    Reference caught = catchInHandlerOfCoroutineCalls(walker, handler, origin);
    Reference rejected = caught ? buildNewPromiseOfCoroutineCalls(walker, origin) : Reference();
    if (rejected && buildSettlementOfCoroutineCalls(walker, Opcode::RejectPromise, rejected, caught, origin)) {
        walker.jumpTo(meeting, singleElementSpan(rejected));
        someWayArrives = true;
    }
    if (!someWayArrives || walker.hasFailed())
        return { };
    walker.continueIn(meeting);
    return result;
}

static Reference buildRampInCoroutineCalls(Walker& walker, const BodyValue& call, FunctionID callee, std::span<const Reference> operands, Origin origin)
{
    Reference promise = buildNewPromiseOfCoroutineCalls(walker, origin);
    Reference started = promise ? buildStartOfBodyOfCoroutine(walker, call, callee, operands, promise) : Reference();
    return started ? proveOrTrapInCoroutineCalls(walker, started, walker.types().builtinClass(BuiltinClasses::Promise), origin) : Reference();
}

bool buildRampAtCall(Walker& walker, const BodyValue& call)
{
    FunctionID callee = findCoroutineCalledBy(walker, call);
    if (!callee)
        return false;
    const Function& function = walker.program().at(callee);
    bool neverSuspends = walker.plan().conventions().of(callee).coroutineNeverSuspends;
    if (!walker.isInLoop() || !walker.walksOwnBodyOfUnit())
        return false;
    if (function.has(FunctionFlags::IsGenerator))
        return false;

    Draft written = walker.draftOf(call);
    if (written.resultIsReturnedAtOnce || !writtenOperandsMayStartBodyOfCoroutine(walker, written))
        return false;

    std::optional<Vector<Reference, 8>> operands = layOutEntryOperandsOfCall(walker, call, callee, { callee, CodeRole::CoroutineBody });
    if (!operands)
        return false;
    if (operands->size() != function.numberOfEntryParameters())
        return true;
    Origin origin = written.origin();

    Reference result = neverSuspends ? buildPromiseOfStartedBodyInCoroutineCalls(walker, call, callee, operands->span(), origin) : buildRampInCoroutineCalls(walker, call, callee, operands->span(), origin);
    if (result)
        walker.bind(call.id(), result);
    return true;
}

void forgetAwaitOfCallThatMayBeCopied(Walker& walker)
{
    if (!walker.walksOwnBodyOfUnit())
        return;
    if (KeptByCallsOfCoroutines* kept = findKeptByCallsOfCoroutines(walker))
        kept->awaitOfCallThatMayBeCopied = std::nullopt;
}

bool buildCoroutineOperationOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcodesThatStateTypes.contains(opcode))
        return nameCastInFrontOfAwaitTakenInCopy(walker, value);
    if (opcode == Opcode::Await)
        return takeWhatCallInCopyHasAwaited(walker, value);
    if (opcode != Opcode::Call)
        return buildPromiseOperation(walker, value);
    auto written = value.as<Opcode::Call>();
    if (written.argumentsOperands().size() <= 1 && written.thisValueOperand() && buildPromiseOperation(walker, value))
        return true;
    bool walksOwnBody = walker.walksOwnBodyOfUnit();
    return (walker.currentBodyIndex().opcodes().contains(Opcode::NewArray) && buildPromiseAllOfElements(walker, value))
        || buildStepWrittenByHand(walker, value)
        || (walksOwnBody && walker.unit().role == CodeRole::CoroutineBody && buildAwaitedStepWrittenByHand(walker, value))
        || ((walksOwnBody ? walker.unit().role == CodeRole::CoroutineBody : !!walker.coroutineState().keptByCalls) && buildAwaitedCall(walker, value))
        || (walksOwnBody && walker.isInLoop() && buildRampAtCall(walker, value));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
