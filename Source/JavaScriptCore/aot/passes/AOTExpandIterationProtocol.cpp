/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFuseGeneratorLoops.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct OpenedIterator {
    ValueID iterator;
    ValueID next;
};

} // anonymous namespace

static BlockID addMergeBlock(IterationExpansion& expansion)
{
    IRBuilder& builder = expansion.builder();
    return builder.addBlock(expansion.function().at(builder.currentBlock()).frequency);
}

static IRBuilder::ProvenBranch branchOnNullish(IterationExpansion& expansion, ValueID value, BlockFrequency nullish, BlockFrequency other)
{
    IRBuilder& builder = expansion.builder();
    return builder.emitBranchWithProof(builder.appendTypeTest(value, TypeTable::nullish), 0, nullish, other);
}

static ValueID appendAwaitIfAsync(IterationExpansion& expansion, IteratorKind kind, ValueID value)
{
    switch (kind) {
    case IteratorKind::Sync:
        return value;
    case IteratorKind::Async:
        return appendAwait(expansion.builder(), value);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID appendRequireObjectCoercible(IterationExpansion& expansion, ValueID iterable)
{
    if (!expansion.program().typeOf(expansion.function(), iterable).mayBeUndefinedOrNull())
        return iterable;
    Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { iterable });
    check.setNullishAccess(NullishAccess::Iterate);
    return expansion.builder().append(WTF::move(check));
}

static ValueID appendIteratorMethodCall(IterationExpansion& expansion, ValueID method, ValueID iterable)
{
    return expansion.appendRequireObject(expansion.appendCall(method, iterable, { }), RuntimeErrorKind::IteratorIsNotAnObject);
}

static OpenedIterator openSyncIterator(IterationExpansion& expansion, ValueID iterable)
{
    IRBuilder& builder = expansion.builder();
    iterable = appendRequireObjectCoercible(expansion, iterable);
    ValueID method = expansion.appendPropGet(iterable, expansion.keyOf(WellKnownSymbol::Iterator));
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(method, TypeTable::function), 0, BlockFrequency::Normal, BlockFrequency::Rare);

    builder.moveToEndOf(branch.notTaken);
    Value error = IRBuilder::make(Opcode::ThrowNotIterable, { iterable });
    error.setIsSpread(expansion.origin() == IterationOrigin::Spread);
    builder.append(WTF::move(error));
    builder.emitUnreachable();

    builder.moveToEndOf(branch.taken);
    ValueID iterator = appendIteratorMethodCall(expansion, branch.provenOnTaken, iterable);
    return { iterator, expansion.appendPropGet(iterator, expansion.keyOf(BuiltinNames::next)) };
}

static OpenedIterator openAsyncIterator(IterationExpansion& expansion, ValueID iterable)
{
    IRBuilder& builder = expansion.builder();
    iterable = appendRequireObjectCoercible(expansion, iterable);
    ValueID method = expansion.appendPropGet(iterable, expansion.keyOf(WellKnownSymbol::AsyncIterator));
    BlockID merge = addMergeBlock(expansion);
    ValueID iterator = builder.addParameter(merge, TypeTable::object);
    IRBuilder::ProvenBranch branch = branchOnNullish(expansion, method, BlockFrequency::Normal, BlockFrequency::Normal);

    OpenedIterator sync = openSyncIterator(expansion, iterable);
    builder.emitJump(merge, { builder.append(Opcode::CreateAsyncFromSyncIterator, { sync.iterator, sync.next }) });

    builder.moveToEndOf(branch.notTaken);
    builder.emitJump(merge, { appendIteratorMethodCall(expansion, branch.provenOnNotTaken, iterable) });

    builder.moveToEndOf(merge);
    return { iterator, expansion.appendPropGet(iterator, expansion.keyOf(BuiltinNames::next)) };
}

static OpenedIterator openIterator(IterationExpansion& expansion, ValueID iterable, IteratorKind kind)
{
    switch (kind) {
    case IteratorKind::Sync:
        return openSyncIterator(expansion, iterable);
    case IteratorKind::Async:
        return openAsyncIterator(expansion, iterable);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitProtocolOpen(IterationExpansion& expansion, ArmState& state, IteratorKind kind)
{
    OpenedIterator opened = openIterator(expansion, state.subject, kind);
    state.subject = opened.iterator;
    state.nextMethod = opened.next;
}

static void emitCloseCalls(IterationExpansion& expansion, const ArmState& state, IteratorKind kind, bool checksResult, BlockID after)
{
    IRBuilder& builder = expansion.builder();
    ValueID method = expansion.appendPropGet(state.subject, expansion.keyOf(BuiltinNames::returnKeyword));
    IRBuilder::ProvenBranch branch = branchOnNullish(expansion, method, BlockFrequency::Normal, BlockFrequency::Normal);
    builder.emitJump(after);

    builder.moveToEndOf(branch.notTaken);
    ValueID result = appendAwaitIfAsync(expansion, kind, expansion.appendCall(branch.provenOnNotTaken, state.subject, { }));
    if (checksResult)
        expansion.appendRequireObject(result, RuntimeErrorKind::IteratorResultNotObject);
    builder.emitJump(after);
}

static void emitCloseCallsDroppingExceptions(IterationExpansion& expansion, const ArmState& state, IteratorKind kind, BlockID after)
{
    IRBuilder& builder = expansion.builder();
    std::optional<Target> outer = builder.handler();
    BlockID dropped = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    builder.setHandler(Target { dropped, { }, { } });
    BlockID attempt = builder.addBlock(BlockFrequency::Rare);
    builder.emitJump(attempt);

    builder.moveToEndOf(attempt);
    emitCloseCalls(expansion, state, kind, false, after);
    builder.setHandler(WTF::move(outer));

    builder.moveToEndOf(dropped);
    builder.append(Opcode::CatchValue, { });
    builder.emitJump(after);
}

void emitProtocolClose(IterationExpansion& expansion, ArmState& state, IteratorKind kind, IteratorCompletion completion)
{
    BlockID after = addMergeBlock(expansion);
    switch (completion) {
    case IteratorCompletion::Normal:
        emitCloseCalls(expansion, state, kind, true, after);
        break;
    case IteratorCompletion::Throw:
        emitCloseCallsDroppingExceptions(expansion, state, kind, after);
        break;
    }
    expansion.builder().moveToEndOf(after);
    expansion.finishClose();
}

static ValueID sentOrUndefined(IterationExpansion& expansion, const StepRequest& request)
{
    return request.sent ? request.sent : expansion.builder().appendUndefined();
}

static ValueID appendNextCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    if (!request.sent)
        return expansion.appendCall(state.nextMethod, state.subject, { });
    return expansion.appendCall(state.nextMethod, state.subject, singleElementSpan(request.sent));
}

static ValueID appendThrowCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder();
    ValueID sent = sentOrUndefined(expansion, request);
    ValueID method = expansion.appendPropGet(state.subject, expansion.keyOf(BuiltinNames::throwKeyword));
    IRBuilder::ProvenBranch branch = branchOnNullish(expansion, method, BlockFrequency::Rare, BlockFrequency::Normal);

    BlockID closed = addMergeBlock(expansion);
    emitCloseCalls(expansion, state, request.kind, true, closed);
    builder.moveToEndOf(closed);
    Value error = IRBuilder::make(Opcode::NewRuntimeError);
    error.setRuntimeError(RuntimeErrorKind::IteratorWithoutThrowMethod);
    builder.emitThrow(builder.append(WTF::move(error)));

    builder.moveToEndOf(branch.notTaken);
    return expansion.appendCall(branch.provenOnNotTaken, state.subject, singleElementSpan(sent));
}

static ValueID appendReturnCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    IRBuilder& builder = expansion.builder();
    ValueID sent = sentOrUndefined(expansion, request);
    ValueID method = expansion.appendPropGet(state.subject, expansion.keyOf(BuiltinNames::returnKeyword));
    IRBuilder::ProvenBranch branch = branchOnNullish(expansion, method, BlockFrequency::Normal, BlockFrequency::Normal);

    ValueID returned = appendAwaitIfAsync(expansion, request.kind, sent);
    expansion.finishStep(builder.appendBoolean(true), returned);

    builder.moveToEndOf(branch.notTaken);
    return expansion.appendCall(branch.provenOnNotTaken, state.subject, singleElementSpan(sent));
}

static ValueID appendResumeCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    switch (request.mode) {
    case ResumeMode::Next:
        return appendNextCall(expansion, state, request);
    case ResumeMode::Throw:
        return appendThrowCall(expansion, state, request);
    case ResumeMode::Return:
        return appendReturnCall(expansion, state, request);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitProtocolStep(IterationExpansion& expansion, ArmState& state, const StepRequest& request)
{
    ValueID result = appendAwaitIfAsync(expansion, request.kind, appendResumeCall(expansion, state, request));
    emitIteratorResultReads(expansion, result, request.readsValueWhenDone);
}

void emitIteratorResultReads(IterationExpansion& expansion, ValueID result, bool readsValueWhenDone)
{
    IRBuilder& builder = expansion.builder();
    ValueID object = expansion.appendRequireObject(result, RuntimeErrorKind::IteratorResultNotObject);
    ValueID done = builder.append(Opcode::ToBoolean, { expansion.appendPropGet(object, expansion.keyOf(BuiltinNames::done)) });
    if (readsValueWhenDone) {
        expansion.finishStep(done, expansion.appendPropGet(object, expansion.keyOf(BuiltinNames::value)));
        return;
    }

    IRBuilder::BranchBlocks blocks = builder.emitBranch(done, BlockFrequency::Normal, BlockFrequency::Normal);
    expansion.finishStepAsDone();

    builder.moveToEndOf(blocks.notTaken);
    ValueID value = expansion.appendPropGet(object, expansion.keyOf(BuiltinNames::value));
    expansion.finishStep(builder.appendBoolean(false), value);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
