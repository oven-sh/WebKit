/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCoroutines.h"
#include "AOTFuseGeneratorLoops.h"
#include "AOTResolveCallsDirectCall.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct OpenedIterator {
    ValueID iterator;
    ValueID next;
    ValueID isSteppedWithoutCall;
    bool isOpenedBySharedFunction { false };
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

static IRBuilder::ProvenBranch branchOnString(IterationExpansion& expansion, ValueID value)
{
    IRBuilder& builder = expansion.builder();
    return builder.emitBranchWithProof(builder.appendTypeTest(value, TypeTable::string), 0, BlockFrequency::Normal, BlockFrequency::Normal);
}

static ArmState stringStateOf(const ArmState& state, ValueID string)
{
    return ArmState { .arm = IterationArm { .form = IterationForm::String, .testedType = TypeTable::string, .classIsProven = true, .namesAreProven = true }, .subject = string, .position = state.position };
}

static ArmState withSubject(const ArmState& state, ValueID subject)
{
    ArmState narrowed = state;
    narrowed.subject = subject;
    return narrowed;
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
    check.setNullishAccess(expansion.origin() == IterationOrigin::Spread ? NullishAccess::Spread : NullishAccess::Iterate);
    return expansion.builder().append(WTF::move(check));
}

static ValueID appendIteratorMethodCall(IterationExpansion& expansion, ValueID method, ValueID iterable)
{
    return expansion.appendRequireObject(expansion.appendCall(method, iterable, { }), RuntimeErrorKind::IteratorIsNotAnObject);
}

static ValueID appendCallOfSharedFunction(IterationExpansion& expansion, OutlinedIteration operation, std::span<const ValueID> arguments)
{
    DerivedFunctionKey key { .role = FunctionRole::OutlinedBody, .target = FunctionID(), .parameter = 0, .node = static_cast<uint32_t>(operation) };
    return appendDirectCall(expansion.builder(), { .callee = expansion.pass().derivedFunction(key), .arguments = arguments, .isNeverInlined = true });
}

static bool isOpenedBySharedFunction(IterationExpansion& expansion, ValueID iterable)
{
    if (expansion.isMakingSharedFunction() || expansion.origin() != IterationOrigin::Source || isOutlinedIteration(expansion.function()))
        return false;
    return !expansion.program().typeOf(expansion.function(), iterable).layouts;
}

static bool isExpectedToRun(IterationExpansion& expansion)
{
    return expansion.isMakingSharedFunction() || expansion.function().at(expansion.builder().currentBlock()).frequency == BlockFrequency::Normal;
}

static OpenedIterator openSyncIterator(IterationExpansion& expansion, ValueID iterable)
{
    IRBuilder& builder = expansion.builder();
    if (isOpenedBySharedFunction(expansion, iterable)) {
        ValueID opened = appendCallOfSharedFunction(expansion, OutlinedIteration::OpenIterator, singleElementSpan(iterable));
        return { expansion.appendResultAt(opened, 0), expansion.appendResultAt(opened, 1), { }, true };
    }

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
    return { iterator, expansion.appendPropGet(iterator, expansion.keyOf(BuiltinNames::next)), { } };
}

ValueID openWrappedIterator(IterationExpansion& expansion, ValueID iterable)
{
    if (isOpenedBySharedFunction(expansion, iterable))
        return appendCallOfSharedFunction(expansion, OutlinedIteration::OpenWrappedIterator, singleElementSpan(iterable));

    OpenedIterator sync = openSyncIterator(expansion, iterable);
    return expansion.builder().append(Opcode::CreateAsyncFromSyncIterator, { sync.iterator, sync.next });
}

static OpenedIterator openAsyncIterator(IterationExpansion& expansion, ValueID iterable)
{
    IRBuilder& builder = expansion.builder();
    iterable = appendRequireObjectCoercible(expansion, iterable);
    ValueID method = expansion.appendPropGet(iterable, expansion.keyOf(WellKnownSymbol::AsyncIterator));
    BlockID merge = addMergeBlock(expansion);
    ValueID iterator = builder.addParameter(merge, TypeTable::object);
    ValueID isFromSyncIterator = builder.addParameter(merge, TypeTable::boolean);
    IRBuilder::ProvenBranch branch = branchOnNullish(expansion, method, BlockFrequency::Normal, BlockFrequency::Normal);

    ValueID wrapper = openWrappedIterator(expansion, iterable);
    builder.emitJump(merge, { wrapper, builder.appendBoolean(true) });

    builder.moveToEndOf(branch.notTaken);
    ValueID made = appendIteratorMethodCall(expansion, branch.provenOnNotTaken, iterable);
    builder.emitJump(merge, { made, builder.appendBoolean(false) });

    builder.moveToEndOf(merge);
    ValueID next = expansion.appendPropGet(iterator, expansion.keyOf(BuiltinNames::next));
    BlockID opened = addMergeBlock(expansion);
    ValueID isSteppedWithoutCall = builder.addParameter(opened, TypeTable::boolean);
    IRBuilder::BranchBlocks fromSyncIterator = builder.emitBranch(isFromSyncIterator, BlockFrequency::Normal, BlockFrequency::Normal);
    builder.emitJump(opened, { builder.appendBoolean(true) });

    builder.moveToEndOf(fromSyncIterator.notTaken);
    IRBuilder::BranchBlocks asyncGenerator = builder.emitBranch(builder.appendTypeTest(iterator, Type::builtinClass(BuiltinClasses::AsyncGenerator)), BlockFrequency::Normal, BlockFrequency::Normal);
    builder.emitJump(opened, { builder.appendTypeTest(next, Type::forBuiltin(Builtins::AsyncGeneratorPrototypeNext)) });

    builder.moveToEndOf(asyncGenerator.notTaken);
    builder.emitJump(opened, { builder.appendBoolean(false) });

    builder.moveToEndOf(opened);
    return { iterator, next, isSteppedWithoutCall };
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

// From here on the subject is the string itself or the iterator of anything else, and every operation asks which.
static void emitOpenOfStringOrIterable(IterationExpansion& expansion, ArmState& state)
{
    IRBuilder& builder = expansion.builder();
    ArmState walked = stringStateOf(state, state.subject);
    emitIndexedOpen(expansion, walked);
    state.position = walked.position;

    if (isOpenedBySharedFunction(expansion, state.subject) && !isExpectedToRun(expansion)) {
        ValueID opened = appendCallOfSharedFunction(expansion, OutlinedIteration::OpenStringOrIterator, singleElementSpan(state.subject));
        state.subject = expansion.appendResultAt(opened, 0);
        state.nextMethod = expansion.appendResultAt(opened, 1);
        state.callsSharedFunctions = true;
        return;
    }

    BlockID merge = addMergeBlock(expansion);
    ValueID subject = builder.addParameter(merge, expansion.types().unionOf(TypeTable::object, TypeTable::string));
    IRBuilder::ProvenBranch branch = branchOnString(expansion, state.subject);
    ValueID nextOfString = builder.appendUndefined();
    builder.emitJump(merge, { branch.provenOnTaken });

    builder.moveToEndOf(branch.notTaken);
    OpenedIterator opened = openSyncIterator(expansion, branch.provenOnNotTaken);
    builder.emitJump(merge, { opened.iterator });

    // For a second parameter the builder looks for the edges into the block, which computes the control flow graph of the whole function at every open. SSARepair adds this one with all the others.
    // A call is typed from the definition it names, before SSARepair picks the one that reaches it: it names the method that was read.
    expansion.define(expansion.addVariable(opened.next), nextOfString);

    builder.moveToEndOf(merge);
    state.subject = subject;
    state.nextMethod = opened.next;
    state.callsSharedFunctions = opened.isOpenedBySharedFunction;
}

void emitProtocolOpen(IterationExpansion& expansion, ArmState& state, IteratorKind kind)
{
    if (state.arm.alsoServesStrings) {
        RELEASE_ASSERT(kind == IteratorKind::Sync);
        emitOpenOfStringOrIterable(expansion, state);
        return;
    }

    OpenedIterator opened = openIterator(expansion, state.subject, kind);
    state.subject = opened.iterator;
    state.nextMethod = opened.next;
    state.isSteppedWithoutCall = opened.isSteppedWithoutCall;
    state.callsSharedFunctions = opened.isOpenedBySharedFunction;
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

static void emitCloseOfIterator(IterationExpansion& expansion, const ArmState& state, IteratorKind kind, IteratorCompletion completion, BlockID after)
{
    if (state.callsSharedFunctions && kind == IteratorKind::Sync) {
        appendCallOfSharedFunction(expansion, completion == IteratorCompletion::Normal ? OutlinedIteration::CloseIterator : OutlinedIteration::CloseIteratorAfterThrow, singleElementSpan(state.subject));
        expansion.builder().emitJump(after);
        return;
    }

    switch (completion) {
    case IteratorCompletion::Normal:
        emitCloseCalls(expansion, state, kind, true, after);
        return;
    case IteratorCompletion::Throw:
        emitCloseCallsDroppingExceptions(expansion, state, kind, after);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitProtocolClose(IterationExpansion& expansion, ArmState& state, IteratorKind kind, IteratorCompletion completion)
{
    IRBuilder& builder = expansion.builder();
    if (state.arm.alsoServesStrings && state.callsSharedFunctions && kind == IteratorKind::Sync && !isExpectedToRun(expansion)) {
        appendCallOfSharedFunction(expansion, completion == IteratorCompletion::Normal ? OutlinedIteration::CloseStringOrIterator : OutlinedIteration::CloseStringOrIteratorAfterThrow, singleElementSpan(state.subject));
        expansion.finishClose();
        return;
    }

    BlockID after = addMergeBlock(expansion);
    if (state.arm.alsoServesStrings) {
        IRBuilder::ProvenBranch branch = branchOnString(expansion, state.subject);
        builder.emitJump(after);
        builder.moveToEndOf(branch.notTaken);
        emitCloseOfIterator(expansion, withSubject(state, branch.provenOnNotTaken), kind, completion, after);
    } else
        emitCloseOfIterator(expansion, state, kind, completion, after);

    builder.moveToEndOf(after);
    expansion.finishClose();
}

static ValueID sentOrUndefined(IterationExpansion& expansion, const StepRequest& request)
{
    return request.sent ? request.sent : expansion.builder().appendUndefined();
}

// %AsyncFromSyncIteratorPrototype%.next passes on what it was called with: where nothing is sent, the iterator's `next` is called without an argument.
static ValueID sentOrAbsent(IterationExpansion& expansion, const StepRequest& request)
{
    return request.sent ? request.sent : expansion.builder().append(Opcode::Absent, { });
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

static void emitStepByCall(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    ValueID result = appendAwaitIfAsync(expansion, request.kind, appendResumeCall(expansion, state, request));
    emitIteratorResultReads(expansion, result, request.readsValueWhenDone);
}

static void emitStepOfStringOrIterator(IterationExpansion& expansion, const ArmState& state, const StepRequest& request)
{
    RELEASE_ASSERT(request.mode == ResumeMode::Next && !request.sent);
    bool walksStringBySharedFunction = state.callsSharedFunctions && !isExpectedToRun(expansion);
    IRBuilder::ProvenBranch branch = branchOnString(expansion, state.subject);
    if (walksStringBySharedFunction) {
        ValueID step = appendCallOfSharedFunction(expansion, OutlinedIteration::StepString, std::array { branch.provenOnTaken, state.position->anyDefinition });
        ValueID done = expansion.appendResultAt(step, static_cast<uint32_t>(IteratorResumeResult::Done));
        ValueID value = expansion.appendResultAt(step, static_cast<uint32_t>(IteratorResumeResult::Value));
        expansion.define(*state.position, expansion.appendResultAt(step, 2));
        expansion.finishStep(done, value);
    } else {
        ArmState walked = stringStateOf(state, branch.provenOnTaken);
        emitStringStep(expansion, walked);
    }

    expansion.builder().moveToEndOf(branch.notTaken);
    emitStepByCall(expansion, withSubject(state, branch.provenOnNotTaken), request);
}

void emitProtocolStep(IterationExpansion& expansion, ArmState& state, const StepRequest& request)
{
    if (state.arm.alsoServesStrings) {
        emitStepOfStringOrIterator(expansion, state, request);
        return;
    }

    // The program never sees the wrapper around a sync iterator, so nobody can tell that its `next` is not called. Where the method that was read from an async generator is the built-in `next`, the call does what the step does.
    if (request.kind == IteratorKind::Async && request.mode == ResumeMode::Next && state.isSteppedWithoutCall) {
        IRBuilder& builder = expansion.builder();
        IRBuilder::BranchBlocks blocks = builder.emitBranch(state.isSteppedWithoutCall, BlockFrequency::Normal, BlockFrequency::Normal);
        finishAsyncIteratorStep(expansion, appendAsyncIteratorStep(builder, state.subject, sentOrAbsent(expansion, request)), request);
        builder.moveToEndOf(blocks.notTaken);
    }
    emitStepByCall(expansion, state, request);
}

static Function buildOpenIterator(DerivedFunctionInputs& inputs)
{
    Function function = createOutlinedIteration(inputs, "openIterator"_s, std::array { TypeTable::anything }, std::array { TypeTable::object, TypeTable::anything });
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID iterable = builder.addParameter(entry, TypeTable::anything);
    builder.moveToEndOf(entry);

    IterationExpansion expansion(inputs.program, function, inputs.types, builder);
    OpenedIterator opened = openSyncIterator(expansion, iterable);
    builder.emitReturn(builder.append(Opcode::Results, { opened.iterator, opened.next }));
    return function;
}

static Function buildOpenWrappedIterator(DerivedFunctionInputs& inputs)
{
    Function function = createOutlinedIteration(inputs, "openWrappedIterator"_s, std::array { TypeTable::anything }, std::array { TypeTable::object });
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID iterable = builder.addParameter(entry, TypeTable::anything);
    builder.moveToEndOf(entry);

    IterationExpansion expansion(inputs.program, function, inputs.types, builder);
    builder.emitReturn(openWrappedIterator(expansion, iterable));
    return function;
}

static Function buildCloseIterator(DerivedFunctionInputs& inputs, ASCIILiteral name, IteratorCompletion completion)
{
    Function function = createOutlinedIteration(inputs, name, std::array { TypeTable::object }, std::array { TypeTable::undefined });
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID iterator = builder.addParameter(entry, TypeTable::object);
    builder.moveToEndOf(entry);

    IterationExpansion expansion(inputs.program, function, inputs.types, builder);
    BlockID after = addMergeBlock(expansion);
    emitCloseOfIterator(expansion, ArmState { .subject = iterator }, IteratorKind::Sync, completion, after);
    builder.moveToEndOf(after);
    builder.emitReturn(builder.appendUndefined());
    return function;
}

static Function buildOpenStringOrIterator(DerivedFunctionInputs& inputs)
{
    TypeID subjectType = inputs.types.unionOf(TypeTable::object, TypeTable::string);
    Function function = createOutlinedIteration(inputs, "openStringOrIterator"_s, std::array { TypeTable::anything }, std::array { subjectType, TypeTable::anything });
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID iterable = builder.addParameter(entry, TypeTable::anything);
    builder.moveToEndOf(entry);

    IterationExpansion expansion(inputs.program, function, inputs.types, builder);
    IRBuilder::ProvenBranch branch = branchOnString(expansion, iterable);
    builder.emitReturn(builder.append(Opcode::Results, { branch.provenOnTaken, builder.appendUndefined() }));

    builder.moveToEndOf(branch.notTaken);
    OpenedIterator opened = openSyncIterator(expansion, branch.provenOnNotTaken);
    builder.emitReturn(builder.append(Opcode::Results, { opened.iterator, opened.next }));
    return function;
}

static Function buildCloseStringOrIterator(DerivedFunctionInputs& inputs, ASCIILiteral name, IteratorCompletion completion)
{
    TypeID subjectType = inputs.types.unionOf(TypeTable::object, TypeTable::string);
    Function function = createOutlinedIteration(inputs, name, std::array { subjectType }, std::array { TypeTable::undefined });
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID subject = builder.addParameter(entry, subjectType);
    builder.moveToEndOf(entry);

    IterationExpansion expansion(inputs.program, function, inputs.types, builder);
    BlockID after = addMergeBlock(expansion);
    IRBuilder::ProvenBranch branch = branchOnString(expansion, subject);
    builder.emitJump(after);
    builder.moveToEndOf(branch.notTaken);
    emitCloseOfIterator(expansion, ArmState { .subject = branch.provenOnNotTaken }, IteratorKind::Sync, completion, after);
    builder.moveToEndOf(after);
    builder.emitReturn(builder.appendUndefined());
    return function;
}

static Function buildStepString(DerivedFunctionInputs& inputs)
{
    TypeID positionType = positionTypeOfStringWalk(inputs.types);
    Function function = createOutlinedIteration(inputs, "stepString"_s, std::array { TypeTable::string, positionType }, std::array { TypeTable::boolean, TypeTable::anything, positionType });
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, function, inputs.types);
    BlockID entry = builder.addBlock();
    ValueID string = builder.addParameter(entry, TypeTable::string);
    ValueID position = builder.addParameter(entry, positionType);
    builder.moveToEndOf(entry);

    IterationExpansion expansion(inputs.program, function, inputs.types, builder);
    expansion.returnResultsOfStep(position);
    ArmState state = stringStateOf(ArmState { .position = StateVariable { 0, position } }, string);
    emitStringStep(expansion, state);
    return function;
}

Function buildSharedProtocolFunction(OutlinedIteration operation, DerivedFunctionInputs& inputs)
{
    switch (operation) {
    case OutlinedIteration::OpenIterator:
        return buildOpenIterator(inputs);
    case OutlinedIteration::CloseIterator:
        return buildCloseIterator(inputs, "closeIterator"_s, IteratorCompletion::Normal);
    case OutlinedIteration::CloseIteratorAfterThrow:
        return buildCloseIterator(inputs, "closeIteratorAfterThrow"_s, IteratorCompletion::Throw);
    case OutlinedIteration::OpenWrappedIterator:
        return buildOpenWrappedIterator(inputs);
    case OutlinedIteration::OpenStringOrIterator:
        return buildOpenStringOrIterator(inputs);
    case OutlinedIteration::StepString:
        return buildStepString(inputs);
    case OutlinedIteration::CloseStringOrIterator:
        return buildCloseStringOrIterator(inputs, "closeStringOrIterator"_s, IteratorCompletion::Normal);
    case OutlinedIteration::CloseStringOrIteratorAfterThrow:
        return buildCloseStringOrIterator(inputs, "closeStringOrIteratorAfterThrow"_s, IteratorCompletion::Throw);
    case OutlinedIteration::AppendIterable:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void emitIteratorResultReads(IterationExpansion& expansion, ValueID result, bool readsValueWhenDone)
{
    IRBuilder& builder = expansion.builder();
    ValueID object = expansion.appendRequireObject(result, expansion.origin() == IterationOrigin::Spread ? RuntimeErrorKind::SpreadResultNotObject : RuntimeErrorKind::IteratorResultNotObject);
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
