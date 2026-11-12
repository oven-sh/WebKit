/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCallSiteTable.h"
#include "AOTClassPrototype.h"
#include "AOTBuiltinModules.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTEffectsNet.h"
#include "AOTEnvironment.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTModuleHost.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPoisonedCell.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSlotRepresentation.h"
#include "AOTState.h"
#include "AOTStubRegisters.h"
#include "AOTStubsPolls.h"
#include "AOTStubsValues.h"
#include "AOTUnwinding.h"
#include "DateInstance.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "FunctionExecutable.h"
#include "IteratorOperations.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "JSInternalFieldObjectImpl.h"
#include "JSMap.h"
#include "JSPromise.h"
#include "JSSet.h"
#include "MarkedBlockCage.h"
#include "OwedOutOfMemoryError.h"
#include "RegExpObject.h"
#include "SuppressedError.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <array>
#include <wtf/CommaPrinter.h>
#include <wtf/HashSet.h>
#include <wtf/StringPrintStream.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

SUPPRESS_ASAN static const void* returnAddressOfPolledCall(void* callerStackPointer)
{
    return removeCodePtrTag(static_cast<void* const*>(callerStackPointer)[isX86() ? -1 : -2]);
}

static bool hasCollectedInPolledCall(VM& vm, void* callerStackPointer, CallFrame* frameOfPollStub)
{
    CallFrame* framePointerOfCaller = isX86() ? static_cast<CallFrame*>(frameOfPollStub->callerFrameOrEntryFrame()) : frameOfPollStub;
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreate(vm, framePointerOfCaller, returnAddressOfPolledCall(callerStackPointer), vm.topEntryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, callerStackPointer);
    return frames && !frames->isAtEnd() && frames->callSite().address == vm.aotState().returnAddressOfCallThatCollected;
}

static NEVER_INLINE void throwOwedOutOfMemoryError(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope)
{
    DeferGCForAWhile deferGC(vm);
    throwOutOfMemoryErrorIfOwed(vm, globalObject, scope);
}

static void pollInCallStub(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope, void* callerStackPointer, CallFrame* frameOfPollStub)
{
    {
        DeferGCForAWhile deferGC(vm);
        vm.traps().handleTraps(VMTraps::AsyncEvents);
        RETURN_IF_EXCEPTION(scope, void());
        vm.throwTerminationExceptionIfNeeded();
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!vm.traps().needsOutOfMemoryError()) [[likely]]
        return;
    if (!hasCollectedInPolledCall(vm, callerStackPointer, frameOfPollStub) || vm.heap.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm))
        return;
    throwOwedOutOfMemoryError(vm, globalObject, scope);
}

void collectInPlaceOfPollBehindOperation(VM& vm, JSCell* madeByOperation)
{
    if (vm.aotState().returnAddressOfCallThatCollected != returnAddressOfInnermostCompiledCall(vm))
        return;
    vm.heap.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm);
    vm.writeBarrier(madeByOperation);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollBeforeEntering, void, (Instance* instance, void* callerStackPointer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollBeforeEntering);
    pollInCallStub(vm, globalObject, scope, callerStackPointer, callFrame);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollSlow);
    bool leavesOwedError = std::exchange(instance->runtime.exceptions.pollLeavesOwedError, 0);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    vm.throwTerminationExceptionIfNeeded();
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (vm.traps().needsOutOfMemoryError() && !vm.heap.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm) && !leavesOwedError) [[unlikely]]
        throwOwedOutOfMemoryError(vm, globalObject, scope);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollAtPrologue, void*, (Instance* instance, uint8_t* returnAddress))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollAtPrologue);
    {
        DeferGCForAWhile deferGC(vm);
        vm.traps().handleTraps(VMTraps::AsyncEvents);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        vm.throwTerminationExceptionIfNeeded();
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (vm.traps().needsOutOfMemoryError() && !vm.heap.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm) && vm.traps().needsOutOfMemoryError()) [[unlikely]] {
        OPERATION_RETURN(scope, std::bit_cast<void*>(static_cast<uintptr_t>(pollAtPrologueMustThrow)));
    }
    OPERATION_RETURN(scope, resumeAddressAfterStackCheck(returnAddress));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollAtPrologueWithoutCollecting, void*, (Instance* instance, uint8_t* returnAddress))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTPollAtPrologueWithoutCollecting);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    vm.throwTerminationExceptionIfNeeded();
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (vm.traps().needsOutOfMemoryError() && !vm.heap.cage().errorWaitsForPoll()) [[unlikely]] {
        OPERATION_RETURN(scope, std::bit_cast<void*>(static_cast<uintptr_t>(pollAtPrologueMustThrow)));
    }
    OPERATION_RETURN(scope, resumeAddressAfterStackCheck(returnAddress));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowToHandlerInFunction, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowToHandlerInFunction);
    bool stackIsUnobserved = !needsExceptionForEveryThrow(*instance) && vm.interpreter.isExceptionCaughtByUserCatchClause();
    throwException(globalObject, scope, Exception::create(vm, JSValue::decode(value), stackIsUnobserved ? Exception::StackCaptureAction::DoNotCaptureStack : Exception::StackCaptureAction::CaptureStackOfThrow));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowAfterCleanup, void, (Instance* instance, EncodedJSValue value, uint64_t kept))
{
    AOT_OPERATION_BEGIN_THROW_OF_VALUE(instance, Entry::operationAOTThrowAfterCleanup);
    if (vm.exceptionForInspection()) [[unlikely]] {
        DeferGCForAWhile deferGC(vm);
        throwException(globalObject, scope, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    instance->runtime.exceptions.thrownValue = value;
    instance->state->returnAddressOfThrowAfterCleanup = removeCodePtrTag(callFrame->rawReturnPC());
    instance->state->keptByCleanup = kept;
    return { std::bit_cast<Exception*>(static_cast<uintptr_t>(1)) };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAddressOfThrow, uint64_t, (Instance* instance))
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, AOT_OPERATION_FRAME_RECORD());
    const void* returnAddress = CompiledFrameIterator::topFrameReturnAddress(vm);
    RELEASE_ASSERT(returnAddress);
    return std::bit_cast<uint64_t>(returnAddress);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrow, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_THROW_OF_VALUE(instance, Entry::operationAOTThrow);
    if (vm.exceptionForInspection()) [[unlikely]] {
        DeferGCForAWhile deferGC(vm);
        throwException(globalObject, scope, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    instance->runtime.exceptions.thrownValue = value;
    return { std::bit_cast<Exception*>(static_cast<uintptr_t>(1)) };
}

static Exception* takePendingException(VM& vm)
{
    DeferGC collectsWhatThrowingPutOff(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception && !vm.isTerminationException(exception));
    scope.clearException();
    return exception;
}

struct ThrownValue {
    JSValue value;
    Exception* exception;
};

static ThrownValue takeThrownValue(VM& vm, Instance* instance)
{
    JSValue thrown = JSValue::decode(std::exchange(instance->runtime.exceptions.thrownValue, 0));
    if (!thrown) {
        Exception* exception = takePendingException(vm);
        return { exception->value(), exception };
    }
    if (thrown.isCell() && thrown.asCell()->type() == CellType) {
        Exception* exception = uncheckedDowncast<Exception>(thrown.asCell());
        return { exception->value(), exception };
    }
    return { thrown, nullptr };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, AOT_OPERATION_FRAME_RECORD());
    return JSValue::encode(takeThrownValue(vm, instance).value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatchException, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, AOT_OPERATION_FRAME_RECORD());
    ThrownValue thrown = takeThrownValue(vm, instance);
    if (thrown.exception)
        return JSValue::encode(thrown.exception);
    return JSValue::encode(Exception::create(vm, thrown.value, Exception::StackCaptureAction::DoNotCaptureStack));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatchValueAndException, UGPRPair, (Instance* instance))
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, AOT_OPERATION_FRAME_RECORD());
    ThrownValue thrown = takeThrownValue(vm, instance);
    return makeUGPRPair(JSValue::encode(thrown.value), std::bit_cast<UCPURegister>(thrown.exception));
}

static String nameOfFunctionOnTop(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames)
        return { };
    while (!frames->isAtEnd() && frames->frame().visibility == TraceVisibility::Hidden)
        frames->advance();
    if (frames->isAtEnd())
        return { };
    return writtenFunctionName(FunctionDescription(frames->frame().function).sourceFunction());
}

static String importableModuleOnTop(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames || frames->isAtEnd())
        return { };
    String url = frames->frame().sourceURL();
    return url.startsWith("node:"_s) || url.startsWith("bun:"_s) ? url : String();
}

static void reportUnreachableCall(VM& vm, const Identifier* name, ASCIILiteral missingUse)
{
    String called = name ? name->string() : nameOfFunctionOnTop(vm);
    RuntimeCheckFailure failure("reachability check failed"_s);
    failure.atTopCall(vm);
    failure.detail(called.isEmpty() ? String("a function"_s) : makeString('\'', called, '\''), " was called, but it was not compiled into this executable because nothing seemed to ", missingUse, " it");
    failure.report();
}

JSObject* createRuntimeError(JSGlobalObject* globalObject, Typed::RuntimeErrorKind kind, const Identifier* name)
{
    switch (kind) {
    case Typed::RuntimeErrorKind::UninitializedBinding:
        return name ? createTDZError(globalObject, *name) : createTDZError(globalObject);
    case Typed::RuntimeErrorKind::UnresolvableReference:
        RELEASE_ASSERT(name);
        return createUndefinedVariableError(globalObject, *name);
    case Typed::RuntimeErrorKind::ConstantAssignment:
    case Typed::RuntimeErrorKind::ImportAssignment:
        return createTypeError(globalObject, ReadonlyPropertyWriteError);
    case Typed::RuntimeErrorKind::UninitializedThis:
        return createSuperNotCalledError(globalObject);
    case Typed::RuntimeErrorKind::ThisAlreadyBound:
        return createSuperCalledTwiceError(globalObject);
    case Typed::RuntimeErrorKind::DeleteSuperProperty:
        return createDeleteSuperPropertyError(globalObject);
    case Typed::RuntimeErrorKind::NotDestructurable:
        return name ? createDestructureNullishError(globalObject, *name) : createDestructureNullishError(globalObject);
    case Typed::RuntimeErrorKind::DerivedConstructorResult:
        return createDerivedConstructorResultError(globalObject, name ? name->string() : String());
    case Typed::RuntimeErrorKind::StaticPrototypeKey:
        return createStaticFieldNamedPrototypeError(globalObject);
    case Typed::RuntimeErrorKind::IteratorResultNotObject:
    case Typed::RuntimeErrorKind::IteratorNotObject:
        return createIteratorResultNotObjectError(globalObject);
    case Typed::RuntimeErrorKind::IteratorWithoutThrowMethod:
        return createYieldStarIteratorWithoutThrowError(globalObject);
    case Typed::RuntimeErrorKind::InvalidPrivateRead:
    case Typed::RuntimeErrorKind::InvalidPrivateWrite:
        return createAbsentPrivateNameError(globalObject, createInvalidPrivateNameError);
    case Typed::RuntimeErrorKind::InvalidPrivateBrand:
        return createStaticPrivateBrandError(globalObject);
    case Typed::RuntimeErrorKind::PrivateFieldExists:
        return createRedefinedPrivateNameError(globalObject);
    case Typed::RuntimeErrorKind::PrivateAccessorWithoutGetter:
        return createUndefinedPrivateGetterError(globalObject);
    case Typed::RuntimeErrorKind::PrivateMethodWrite:
    case Typed::RuntimeErrorKind::PrivateAccessorWithoutSetter:
        return createUndefinedPrivateSetterError(globalObject);
    case Typed::RuntimeErrorKind::StaticPrivateInNotObject:
        return createStaticPrivateBrandOfNonObjectError(globalObject);
    case Typed::RuntimeErrorKind::SpreadResultNotObject:
        return createSpreadResultNotObjectError(globalObject);
    case Typed::RuntimeErrorKind::AsyncFromSyncIteratorMissing:
        return createTypeError(globalObject, "iterable should have an iterator symbol"_s);
    case Typed::RuntimeErrorKind::AsyncFromSyncIteratorNotObject:
        return createTypeError(globalObject, "iterator method should return an object"_s);
    case Typed::RuntimeErrorKind::ResourceNotObject:
        return createTypeError(globalObject, "Disposable value must be an object, null, or undefined"_s);
    case Typed::RuntimeErrorKind::DisposeMethodAbsent:
        return createTypeError(globalObject, "@@dispose must not be undefined or null"_s);
    case Typed::RuntimeErrorKind::DisposeMethodNotCallable:
        return createTypeError(globalObject, "@@dispose must be callable"_s);
    case Typed::RuntimeErrorKind::OmittedFromExecutable:
        reportUnreachableCall(globalObject->vm(), name, "refer to"_s);
        CRASH();
    case Typed::RuntimeErrorKind::FunctionNotCompiled:
#if ASSERT_ENABLED || ASAN_ENABLED
        if (isClosedWorld(*Image::current())) {
            reportUnreachableCall(globalObject->vm(), name, "call"_s);
            CRASH();
        }
#endif
        if (!name || name->isEmpty())
            return createError(globalObject, functionNotCompiledMessage());
        if (String module = importableModuleOnTop(globalObject->vm()); !module.isNull())
            return createError(globalObject, exportNotCompiledMessage(name->string(), module));
        return createError(globalObject, functionNotCompiledMessage(name->string()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewRuntimeError, JSCell*, (Instance* instance, uint32_t errorKind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRuntimeError);
    DeferTerminationForAWhile deferTermination(vm);
    RELEASE_AND_RETURN(scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(errorKind), nullptr));
}

static void printKindsOfNativeResult(PrintStream& out, uint32_t kinds)
{
    static constexpr std::array<ASCIILiteral, WrittenNativeResult::numberOfKinds> names { "undefined"_s, "null"_s, "boolean"_s, "number"_s, "string"_s, "symbol"_s, "bigint"_s, "object"_s };
    if (!kinds) {
        out.print("nothing"_s);
        return;
    }
    CommaPrinter separator(" | "_s);
    for (unsigned index = 0; index < names.size(); ++index) {
        if (kinds & 1u << index)
            out.print(separator, names[index]);
    }
}

[[noreturn]] static void stopAtNativeResult(Instance& instance, CallerFrameAndPC* operationFrameRecord, JSValue found, WrittenNativeResult written, const String& nameOfFunction = { })
{
    StringPrintStream declared;
    printKindsOfNativeResult(declared, written.kinds);
    RuntimeCheckFailure failure("type check failed"_s);
    failure.atTopCall(*instance.vm, operationFrameRecord);
    failure.detail("expected ", declared.toString(), ", found ");
    if (found.isObject() && written.kinds & WrittenNativeResult::objectKind)
        failure.detail("an object of another class (", asObject(found)->classInfo()->className, ")");
    else if (found.isNumber() && written.kinds & WrittenNativeResult::numberKind)
        failure.detail("a number outside the declared range");
    else
        failure.detail(reportedTypeName(found));
    failure.detail(" in the result of ", written.constructs ? "new " : "", nameOfFunction.isNull() ? BuiltinTable::singleton().textOf(BuiltinID(written.builtin)) : nameOfFunction);
    failure.report();
    CRASH();
}

void stopAtHostResult(Instance& instance, CallerFrameAndPC* operationFrameRecord, JSValue found, JSCell* function, uint32_t kinds)
{
    if (std::optional<BuiltinCellKey> key = instance.state->builtinCells->keyOfCellHolding(function))
        stopAtNativeResult(instance, operationFrameRecord, found, WrittenNativeResult::decode(WrittenNativeResult::encode(kinds, false, key->builtin().index())));
    stopAtNativeResult(instance, operationFrameRecord, found, WrittenNativeResult::decode(WrittenNativeResult::encode(kinds, false, 0)), getCalculatedDisplayName(*instance.vm, asObject(function)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtHostResult, void, (Instance* instance, EncodedJSValue found, JSCell* function, uint32_t kinds))
{
    stopAtHostResult(*instance, AOT_OPERATION_FRAME_RECORD(), JSValue::decode(found), function, kinds);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtNativeResult, void, (Instance* instance, EncodedJSValue found, uint32_t encodedWritten))
{
    stopAtNativeResult(*instance, AOT_OPERATION_FRAME_RECORD(), JSValue::decode(found), WrittenNativeResult::decode(encodedWritten));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopUnlessNativeResultIsIntegerAsWritten, void, (Instance* instance, EncodedJSValue found, uint32_t encodedWritten))
{
    Typed::Range written = WrittenNativeResult::decode(encodedWritten).writesLengths ? Typed::Range::int32OrLength() : Typed::Range::int32();
    if (JSValue value = JSValue::decode(found); value.isDouble()) {
        double number = value.asDouble();
        if (number >= static_cast<double>(written.minimum) && number <= static_cast<double>(written.maximum) && std::bit_cast<uint64_t>(static_cast<double>(static_cast<int64_t>(number))) == std::bit_cast<uint64_t>(number))
            return;
    }
    stopAtNativeResult(*instance, AOT_OPERATION_FRAME_RECORD(), JSValue::decode(found), WrittenNativeResult::decode(encodedWritten));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCheckFreshResult, void, (Instance* instance, EncodedJSValue encodedResult, uint64_t collectionsBefore, uint32_t builtin, EncodedJSValue receiver, EncodedJSValue firstArgument))
{
    JSValue result = JSValue::decode(encodedResult);
    if (!result || !result.isObject())
        return;
    JSCell* cell = result.asCell();
    VM& vm = *instance->vm;
    Heap& heap = vm.heap;
    uint64_t collections = heap.objectSpace().newlyAllocatedVersion();
    struct FreshResults {
        UncheckedKeyHashSet<JSCell*> cells;
        std::optional<VMIdentifier> vm;
        uint64_t collections { 0 };
    };
    WTF_THREAD_LOCAL_ALWAYS_DESTROY static thread_local FreshResults returned;
    if (returned.vm != vm.identifier() || returned.collections != collections) {
        returned.cells.clear();
        returned.vm = vm.identifier();
        returned.collections = collections;
    }
    ASCIILiteral finding;
    if (encodedResult == receiver)
        finding = "the result is the receiver"_s;
    else if (encodedResult == firstArgument)
        finding = "the result is the first argument"_s;
    else if (!returned.cells.add(cell).isNewEntry)
        finding = "an earlier call that is also declared fresh returned the same object"_s;
    else if (collections == collectionsBefore && !heap.collectionScope() && cell->cellState() != CellState::DefinitelyWhite)
        finding = "the result survived a collection that ended before the call"_s;
    if (finding.isNull())
        return;
    RuntimeCheckFailure failure("fresh result check failed"_s);
    failure.atTopCall(vm, AOT_OPERATION_FRAME_RECORD());
    failure.detail(BuiltinTable::singleton().textOf(BuiltinID(static_cast<uint16_t>(builtin))), " is declared to return a fresh object, but ", finding);
    failure.report();
    CRASH();
}

static void stopIfSaidNeverToReachUnseenCode(VM& vm, const State& state, JSValue escapingValue)
{
    if (!escapingValue || !escapingValue.isCell())
        return;
    countEvent(vm, RuntimeCounterID::CellsPassedToUnknownCode);
    JSCell* cell = escapingValue.asCell();
    String escaped;
    if (isCompiledObject(cell)) {
        const LayoutData& layout = uncheckedDowncast<Object>(cell)->layout();
        if (!layout.isSaidNeverToReachUnseenCode)
            return;
        escaped = reportedObjectName(layout);
    } else if (std::optional<FunctionID> function = functionOf(cell)) {
        if (!Image::current()->programTables().traceOfFunction(function->index()).closuresAreSaidNeverToReachUnseenCode)
            return;
        escaped = makeString("a closure of "_s, reportedFunctionName(sourceFunctionOf(*function)));
    } else if (cell->isObject()) {
        const ClassTable& classes = state.objects->classes;
        ClassSide side = ClassSide::Prototype;
        const LayoutData* layout = classes.classHeldBy(asObject(cell), side);
        if (!layout && cell->classInfo() == ClassPrototype::info())
            layout = classes.classOf(*uncheckedDowncast<ClassPrototype>(cell));
        if (!layout || !layout->isSaidNeverToReachUnseenCode)
            return;
        escaped = makeString(side == ClassSide::Prototype ? "the prototype of "_s : "the constructor of "_s, reportedClassName(*layout));
    } else
        return;
    RuntimeCheckFailure failure("escape check failed"_s);
    failure.atTopCall(vm);
    failure.detail(escaped, " was passed to code that the compiler did not see, but the compiler proved that it never escapes");
    failure.report();
    CRASH();
}

static void askWhatIsHandedToCalleeOfEngine(VM& vm, const State& state, const CallFrame* laid)
{
    JSValue called = laid->guaranteedJSValueCallee();
    auto* callee = called.isCell() ? dynamicDowncast<JSFunction>(called.asCell()) : nullptr;
    auto* executable = callee ? dynamicDowncast<FunctionExecutable>(callee->executable()) : nullptr;
    if (!executable || executable->isBuiltinFunction())
        return;
    stopIfSaidNeverToReachUnseenCode(vm, state, laid->thisValue());
    for (size_t argument = 0; argument < laid->argumentCount(); ++argument)
        stopIfSaidNeverToReachUnseenCode(vm, state, laid->uncheckedArgument(argument));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPoisonCell, void, (Instance* instance, JSCell* cell, uint32_t function, uint32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTPoisonCell);
    if (function & PoisonedCell::isFrameLaidForCalleeOfEngine) {
        askWhatIsHandedToCalleeOfEngine(vm, *instance->state, reinterpret_cast<const CallFrame*>(cell));
        return;
    }
    if (function & PoisonedCell::isLentForCallAhead) {
        PoisonedCell::noteThatCellIsLent(cell, AOT_FRAME_RECORD_OF_THIS_FUNCTION()->callerFrame, function & ~PoisonedCell::isLentForCallAhead, position);
        return;
    }
    countEvent(vm, PoisonedCell::tryPoison(vm, cell, function, position) ? RuntimeCounterID::CellsPoisoned : RuntimeCounterID::UnpoisonableCellsSkipped);
}

static ASCIILiteral failedCheckOf(uint32_t source)
{
    using Typed::ProofSource;
    if (source == static_cast<uint32_t>(ProofSource::RememberedFact))
        return "redundancy elimination check failed"_s;
    if (source == static_cast<uint32_t>(ProofSource::HoistedCall))
        return "loop-invariant code motion check failed"_s;
    if (source >= static_cast<uint32_t>(ProofSource::Lifetime))
        return "lifetime check failed"_s;
    return "type check failed"_s;
}

static void printExpectedType(PrintStream& out, uint32_t summary)
{
    namespace Primitive = Typed::Primitive;
    static constexpr std::pair<uint16_t, ASCIILiteral> names[] = {
        { Primitive::Undefined, "undefined"_s }, { Primitive::Null, "null"_s }, { Primitive::Boolean, "boolean"_s }, { Primitive::True, "true"_s }, { Primitive::False, "false"_s },
        { Primitive::Number, "number"_s }, { Primitive::NumberWithoutFraction, "integer"_s }, { Primitive::Double, "double"_s }, { Primitive::Integer, "int32"_s }, { Primitive::WholeNumber, "integer"_s }, { Primitive::FractionalNumber, "double"_s },
        { Primitive::String, "string"_s }, { Primitive::Symbol, "symbol"_s }, { Primitive::BigInt, "bigint"_s },
    };
    if (summary == (FalseProofWord::primitivesMask | FalseProofWord::objectBit)) {
        out.print("a JavaScript value"_s);
        return;
    }
    CommaPrinter separator(" | "_s);
    uint32_t rest = summary;
    for (auto [bits, name] : names) {
        if ((rest & bits) != bits)
            continue;
        rest &= ~bits;
        out.print(separator, name);
    }
    if (summary & FalseProofWord::stringLiteralBit)
        out.print(separator, "string literal"_s);
    if (summary & FalseProofWord::symbolLiteralBit)
        out.print(separator, "unique symbol"_s);
    if (summary & FalseProofWord::objectBit)
        out.print(separator, "object"_s);
}

[[noreturn]] static void stopAtFalseProof(Instance& instance, RuntimeCheckFailure& failure, EncodedJSValue encodedFound, uint32_t source, uint32_t expectedType)
{
    using Typed::ProofSource;
    static constexpr std::array descriptions {
        "an argument"_s, "a call result"_s, "a property"_s, "a local variable"_s, "a captured variable"_s, "a closure variable"_s, "an element"_s, "the result of a built-in or indirect call"_s, "the result of an operation"_s, "a value narrowed by a branch"_s,
        "a load or check that the compiler removed as redundant gives a different result here"_s, "a call that the compiler hoisted out of the loop returns a different result in this iteration"_s, "an object was used after the compiler ended its lifetime"_s,
    };
    static_assert(descriptions.size() == numberOfEnumerators<ProofSource> && !static_cast<uint32_t>(ProofSource::EntryParameter) && static_cast<uint32_t>(ProofSource::Lifetime) + 1 == descriptions.size());
    JSValue found = JSValue::decode(encodedFound);
    if (found && found.isCell() && found.asCell()->classInfo() == PoisonedCell::info())
        PoisonedCell::stop(found.asCell());
    ASCIILiteral description = descriptions[std::min<size_t>(source, descriptions.size() - 1)];
    if (source >= static_cast<uint32_t>(ProofSource::RememberedFact))
        failure.detail(description);
    else {
        if (expectedType) {
            StringPrintStream type;
            printExpectedType(type, expectedType);
            failure.detail("expected ", type.toString(), ", ");
        }
        failure.detail("found ", found ? reportedTypeName(found) : source == static_cast<uint32_t>(ProofSource::Field) ? "an empty slot, undefined or null"_s : "an unexpected value"_s, " in ", description);
    }
    failure.report();
    failure.crashInHost(instance.globalObject);
    CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtFalseProof, void, (Instance* instance, EncodedJSValue encodedFound, uint32_t function, uint32_t, uint32_t falseProofWord))
{
    uint32_t source = FalseProofWord::sourceOf(falseProofWord);
    RuntimeCheckFailure failure(failedCheckOf(source));
    failure.atTopCall(*instance->vm, AOT_OPERATION_FRAME_RECORD()).inFunction(FunctionID(function));
    stopAtFalseProof(*instance, failure, encodedFound, source, FalseProofWord::expectedTypeOf(falseProofWord));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtStaleLoad, void, (Instance* instance, uint32_t function, uint32_t))
{
    RuntimeCheckFailure failure("load elimination check failed"_s);
    failure.atTopCall(*instance->vm, AOT_OPERATION_FRAME_RECORD()).inFunction(FunctionID(function));
    failure.detail("memory changed between a load and a later load that the compiler replaced with its result");
    failure.report();
    failure.crashInHost(instance->globalObject);
    CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnterBodyThatRunsOnce, void, (Instance* instance, uint32_t function))
{
    if (instance->state->functionsEnteredThatRunOnce.add(function).isNewEntry) [[likely]]
        return;
    RuntimeCheckFailure failure("run-once check failed"_s);
    failure.inFunction(FunctionID(function));
    failure.detail("the function was entered a second time, but the compiler proved that it runs at most once");
    failure.report();
    CRASH();
}

static bool isKnownToSayLessOfSecondDigest(uint32_t operationAndCalleeRows, uint64_t differs)
{
    unsigned calleeRow = operationAndCalleeRows >> 16;
    unsigned row = calleeRow == Typed::noRowInEffectsNet ? operationAndCalleeRows & UINT16_MAX : calleeRow;
    using Lane = Typed::SecondLaneInEffectsNet;
    struct Row {
        ASCIILiteral name;
        std::initializer_list<Lane> lanes;
    };
    static const Row rows[] = {
        { "runtime:operationAOTObjectCreate"_s, { Lane::NamedProperties } },
        { "built-in:Object.create"_s, { Lane::NamedProperties } },
        { "stub:BuiltinCell"_s, { Lane::Structure, Lane::NamedProperties } },
        { "stub:DidCreateFieldOfSlot"_s, { Lane::Structure } },
        { "stub:PutSlotOrAdd"_s, { Lane::Structure } },
        { "runtime:operationAOTDidCreateField"_s, { Lane::Structure } },
        { "operation:GetConstructPrototype"_s, { Lane::Structure, Lane::NamedProperties } },
        { "operation:PropGet"_s, { Lane::Structure, Lane::NamedProperties } },
        { "operation:SuperGet"_s, { Lane::Structure, Lane::NamedProperties } },
        { "runtime:operationAOTReadRunningNoCode"_s, { Lane::Structure } },
        { "runtime:operationAOTReadBeforeLoop"_s, { Lane::Structure } },
        { "operation:CheckIterationRestart"_s, { Lane::Structure, Lane::NamedProperties } },
        { "operation:CheckInlineeFallback"_s, { Lane::Structure, Lane::NamedProperties } },
    };
    StringPrintStream name;
    printRowOfEffectsNet(name, row);
    String named = name.toString();
    for (const Row& row : rows) {
        if (named != row.name)
            continue;
        for (Lane lane : row.lanes)
            differs &= ~(0xffull << static_cast<unsigned>(lane) * 8);
        return !differs;
    }
    return false;
}

static void printWhoseWordsInEffectsNet(PrintStream& out, uint32_t operand)
{
    if (operand & Typed::variableInEffectsNet)
        out.print("the value of module variable g", operand & (Typed::variableInEffectsNet - 1), " (not an operand of the call)");
    else
        out.print("operand ", operand);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtFalseEffects, void, (Instance* instance, uint32_t function, uint32_t position, uint32_t part, uint32_t rows, uint64_t differs))
{
    static constexpr std::array<ASCIILiteral, 5> names { "does nothing"_s, "runs no other code, but compiled code was entered"_s, "throws no language exception, but it threw one"_s, "does nothing"_s, "does not collect, but the heap was collected"_s };
    static const bool goesOn = !!getenv("BUN_AOT_EFFECTS_NET_GOES_ON");
    StringPrintStream said;
    Typed::Opcode opcode = static_cast<Typed::Opcode>(rows & UINT16_MAX);
    bool isOfTurn = opcode == Typed::Opcode::CheckIterationRestart || opcode == Typed::Opcode::CheckInlineeFallback || opcode == Typed::Opcode::CheckReloadBypass;
    said.print(!isOfTurn ? "the declared effects of " : opcode == Typed::Opcode::CheckIterationRestart ? "a loop iteration restarts in the generic version, so the compiler's state" : opcode == Typed::Opcode::CheckInlineeFallback ? "an inlined call falls back to the real call, so the compiler's state" : "a loop iteration skips the loads that were hoisted for iterations without side effects, so the compiler's state");
    if (!isOfTurn) {
        printRowOfEffectsNet(said, rows & UINT16_MAX);
        said.print(" and of ");
    }
    if (isOfTurn) {
    } else if (rows >> 16 == Typed::noRowInEffectsNet && Typed::effectRuleOf(opcode) != Typed::EffectRule::Call && opcode != Typed::Opcode::CallTable)
        said.print("the proven types of its operands");
    else
        printRowOfEffectsNet(said, rows >> 16);
    if (isOfTurn && (part & 0xff) == static_cast<uint32_t>(Typed::PartOfEffects::RunsOtherCode))
        said.print(opcode == Typed::Opcode::CheckReloadBypass ? " says that no other code ran in it, but an operation that may run other code has run since it began" : " says that nothing observable happened in it, but an operation that is observable if repeated has run since it began");
    else if (part & Typed::speaksOfValueOfVariableInEffectsNet)
        said.print(" says that every store of the iteration was undone, but module variable g", part >> 16 & (Typed::variableInEffectsNet - 1), " differs from its value at the loop header");
    else if ((part & 0xff) == static_cast<uint32_t>(Typed::PartOfEffects::Writes)) {
        said.print(isOfTurn ? " says that nothing in it wrote " : " say that the call does not write ");
        CommaPrinter comma;
        if (part >> 16 == Typed::noOperandInEffectsNet) {
            static constexpr std::array<ASCIILiteral, Typed::heapsOfCountsInEffectsNet.size()> deeds {
                "guards were invalidated"_s, "guards were armed or started"_s, "an unmodified built-in, or the prototype chain of its instances, was written"_s,
                "a module variable, a static field or a statically allocated scope variable changed"_s, "static fields moved to their class object"_s,
                "compiled code stored to a module variable"_s, "compiled code stored to a static field"_s, "compiled code stored to a statically allocated scope variable"_s
            };
            CommaPrinter commaOfDeeds;
            StringPrintStream done;
            Typed::AbstractHeapSet heaps;
            for (unsigned count = 0; count < deeds.size(); ++count) {
                if (!Typed::countInWordOfEffectsNet(differs, count))
                    continue;
                heaps.add(Typed::heapsOfCountsInEffectsNet[count]);
                done.print(commaOfDeeds, deeds[count]);
            }
            for (Typed::AbstractHeap heap : heaps)
                said.print(comma, Typed::nameOf(heap));
            said.print(", but ", done.toString());
            differs = 0;
        }
        if (part & Typed::speaksOfSecondDigestInEffectsNet) {
            static constexpr std::array<ASCIILiteral, Typed::heapsOfSecondDigestInEffectsNet.size()> words {
                "the structure"_s, "the prototype"_s, "the named properties"_s, "the bytes"_s, "the environment's variables"_s, "the entries"_s, "the internal fields"_s, "the RegExp state"_s
            };
            Typed::AbstractHeapSet heaps;
            CommaPrinter commaOfWords;
            StringPrintStream others;
            for (unsigned lane = 0; lane < words.size(); ++lane) {
                if (!(differs >> lane * 8 & 0xff))
                    continue;
                heaps.add(Typed::heapsOfSecondDigestInEffectsNet[lane]);
                others.print(commaOfWords, words[lane]);
            }
            for (Typed::AbstractHeap heap : heaps)
                said.print(comma, Typed::nameOf(heap));
            said.print(", but these parts of ");
            printWhoseWordsInEffectsNet(said, part >> 16);
            said.print(" changed: ", others.toString());
            if (isKnownToSayLessOfSecondDigest(rows, differs)) {
                if (!goesOn)
                    return;
                said.print(" (a table entry that is known to declare less)");
            }
            differs = 0;
        }
        for (unsigned lane = 0; lane < Typed::heapsOfDigestInEffectsNet.size(); ++lane) {
            if (differs >> lane * 8 & 0xff)
                said.print(comma, Typed::nameOf(Typed::heapsOfDigestInEffectsNet[lane]));
        }
        if (part >> 16 != Typed::noOperandInEffectsNet && !(part & Typed::speaksOfSecondDigestInEffectsNet)) {
            said.print(", but ");
            printWhoseWordsInEffectsNet(said, part >> 16);
            said.print(" changed there");
        }
    } else {
        if (differs)
            said.print(" (", BuiltinTable::singleton().textOf(BuiltinID(static_cast<uint16_t>(differs))), ")");
        said.print(" say that the call ", names[std::min<size_t>(part, names.size() - 1)]);
    }
    if (goesOn && !noteFalseRowOfEffects(makeString(function, ':', position, ':', said.toString())))
        return;
    RuntimeCheckFailure failure("effects check failed"_s);
    failure.atTopCall(*instance->vm, AOT_OPERATION_FRAME_RECORD()).inFunction(FunctionID(function));
    failure.detail(said.toString());
    failure.report();
    if (!goesOn)
        CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCollectionsForEffectsNet, uint64_t, (uint64_t vm))
{
    return reinterpret_cast<VM*>(vm)->heap.objectSpace().newlyAllocatedVersion();
}

static thread_local uint64_t t_restOfLastDigestForEffectsNet;

static uint64_t digestOfLanesForEffectsNet(const std::array<uint64_t, 8>& hashes, uint64_t lanes)
{
    uint64_t digest = 0;
    uint64_t rest = 0;
    for (unsigned lane = 0; lane < hashes.size(); ++lane) {
        if (lanes & 1ull << lane) {
            digest |= (hashes[lane] >> 56) << lane * 8;
            rest |= (hashes[lane] >> 48 & 0xff) << lane * 8;
        }
    }
    t_restOfLastDigestForEffectsNet = rest;
    return digest;
}

static uint64_t secondDigestForEffectsNet(JSCell* cell, uint64_t lanes)
{
    using Lane = Typed::SecondLaneInEffectsNet;
    std::array<uint64_t, Typed::heapsOfSecondDigestInEffectsNet.size()> hashes;
    hashes.fill(1);
    auto mix = [&](Lane lane, uint64_t word) {
        uint64_t& hash = hashes[static_cast<size_t>(lane)];
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    };
    auto mixBytes = [&](Lane lane, const void* start, size_t numberOfBytes) {
        const uint8_t* bytes = static_cast<const uint8_t*>(start);
        size_t offset = 0;
        for (; offset + sizeof(uint64_t) <= numberOfBytes; offset += sizeof(uint64_t))
            mix(lane, WTF::unalignedLoad<uint64_t>(bytes + offset));
        for (; offset < numberOfBytes; ++offset)
            mix(lane, bytes[offset]);
    };
    auto mixRestOfCell = [&](Lane lane, const JSCell* whole, size_t from) {
        mixBytes(lane, reinterpret_cast<const uint8_t*>(whole) + from, whole->cellSize() - from);
    };
    auto mixEndsOfBytes = [&](Lane lane, const void* start, size_t numberOfBytes) {
        constexpr size_t most = 4 * KB;
        if (numberOfBytes <= 2 * most) {
            mixBytes(lane, start, numberOfBytes);
            return;
        }
        mixBytes(lane, start, most);
        mixBytes(lane, static_cast<const uint8_t*>(start) + numberOfBytes - most, most);
    };
    auto mixNamedProperties = [&](JSObject* object) {
        Structure* structure = object->structure();
        constexpr PropertyOffset most = 128;
        PropertyOffset last = structure->maxOffset();
        for (PropertyOffset offset = 0; offset <= last; ++offset) {
            if (offset == static_cast<PropertyOffset>(structure->inlineCapacity()) && offset < firstOutOfLineOffset)
                offset = firstOutOfLineOffset;
            if (offset == firstOutOfLineOffset + most && last - most > offset)
                offset = last - most;
            if (offset <= last)
                mix(Lane::NamedProperties, JSValue::encode(object->getDirect(offset)));
        }
    };
    auto mixEnvironment = [&](const JSCell* environment) {
        mixBytes(Lane::Environment, reinterpret_cast<const uint8_t*>(environment) + offsetOfFirstEnvironmentVariable, uncheckedDowncast<Environment>(environment)->layout().size - offsetOfFirstEnvironmentVariable);
    };

    JSType type = cell->type();
    if (type == CompiledEnvironmentType)
        mixEnvironment(cell);
    else if (cell->isObject()) {
        JSObject* object = asObject(cell);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cell);
        Object* withExtraProperties = compiledObjectOrNull(cell);
        if (withExtraProperties && !withExtraProperties->extraProperties())
            withExtraProperties = nullptr;
        if (withExtraProperties) {
            const CompiledStructure& structure = withExtraProperties->compiledStructure();
            mix(Lane::Structure, reinterpret_cast<uintptr_t>(&structure.layout()));
            mix(Lane::Structure, structure.flags().toRaw());
        } else
            mix(Lane::Structure, cell->structureID().bits());
        mix(Lane::Prototype, JSValue::encode(object->getPrototypeDirect()));
        if (Object* compiled = compiledObjectOrNull(cell)) {
            if (JSObject* extraProperties = compiled->extraProperties()) {
                mix(Lane::NamedProperties, extraProperties->structureID().bits());
                mixNamedProperties(extraProperties);
            }
        } else
            mixNamedProperties(object);
        if (isCompiledClosure(cell)) {
            const Closure* closure = uncheckedDowncast<Closure>(cell);
            if (closure->entry()) {
                for (uint64_t reference : closure->captures().first(closure->numberOfReferences())) {
                    JSValue captured = JSValue::decode(reference);
                    if (captured && captured.isCell() && captured.asCell()->type() == CompiledEnvironmentType)
                        mixEnvironment(captured.asCell());
                }
            }
        } else if (isTypedArrayTypeIncludingDataView(type)) {
            JSArrayBufferView* view = uncheckedDowncast<JSArrayBufferView>(cell);
            if (const void* vector = view->vectorWithoutPACValidation(); vector && !view->isShared())
                mixEndsOfBytes(Lane::BytesOfView, vector, view->byteLength());
        } else if (type == ArrayBufferType) {
            ArrayBuffer* buffer = uncheckedDowncast<JSArrayBuffer>(cell)->impl();
            if (const void* data = buffer->data(); data && !buffer->isShared())
                mixEndsOfBytes(Lane::BytesOfView, data, buffer->byteLength());
        } else if (type == JSMapType || type == JSSetType) {
            const JSCell* storage = WTF::unalignedLoad<const JSCell*>(bytes + (type == JSMapType ? JSMap::offsetOfStorage() : JSSet::offsetOfStorage()));
            mix(Lane::Collection, reinterpret_cast<uintptr_t>(storage));
            if (storage)
                mixEndsOfBytes(Lane::Collection, reinterpret_cast<const uint8_t*>(storage) + sizeof(JSCell), storage->cellSize() - sizeof(JSCell));
        } else if (type == JSPromiseType) {
            mix(Lane::InternalFields, WTF::unalignedLoad<uint64_t>(bytes + JSPromise::offsetOfPacked()));
            mix(Lane::InternalFields, WTF::unalignedLoad<uint64_t>(bytes + JSPromise::offsetOfSlot()));
        } else if (type == JSDateType)
            mix(Lane::InternalFields, WTF::unalignedLoad<uint64_t>(bytes + DateInstance::offsetOfInternalNumber()));
        else if (type == RegExpObjectType) {
            mix(Lane::RegExp, WTF::unalignedLoad<uint64_t>(bytes + RegExpObject::offsetOfRegExpAndFlags()));
            mix(Lane::RegExp, WTF::unalignedLoad<uint64_t>(bytes + RegExpObject::offsetOfLastIndex()));
        } else if (type == JSArrayIteratorType || type == JSMapIteratorType || type == JSSetIteratorType || type == JSStringIteratorType || type == JSIteratorHelperType
            || type == JSWrapForValidIteratorType || type == JSRegExpStringIteratorType || type == JSAsyncFromSyncIteratorType)
            mixRestOfCell(Lane::InternalFields, cell, JSInternalFieldObjectImpl<>::offsetOfInternalFields());
    }
    return digestOfLanesForEffectsNet(hashes, lanes);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDigestForEffectsNet, uint64_t, (EncodedJSValue encodedValue, uint64_t lanes))
{
    if (lanes & Typed::countsStoreInEffectsNet) {
        reinterpret_cast<State*>(encodedValue)->countWriteForEffectsNet(static_cast<Typed::CountInEffectsNet>(lanes & 0xff));
        return 0;
    }
    if (lanes & Typed::asksForCountsInEffectsNet) {
        const State& state = *reinterpret_cast<const State*>(encodedValue);
        uint64_t counts = 0;
        for (unsigned count = 0; count < Typed::heapsOfCountsInEffectsNet.size(); ++count) {
            if (lanes & 1ull << count)
                counts |= Typed::wordOfCountInEffectsNet(state.writesCountedForEffectsNet[count], count);
        }
        return counts;
    }
    if (lanes & Typed::asksForRestOfLastDigestInEffectsNet)
        return t_restOfLastDigestForEffectsNet;
    t_restOfLastDigestForEffectsNet = 0;
    JSValue value = JSValue::decode(encodedValue);
    if (!value || !value.isCell())
        return 0;
    if (lanes & Typed::asksForSecondDigestInEffectsNet)
        return secondDigestForEffectsNet(value.asCell(), lanes);
    std::array<uint64_t, Typed::heapsOfDigestInEffectsNet.size()> hashes;
    hashes.fill(1);
    auto mix = [&](unsigned lane, uint64_t word) {
        hashes[lane] = (hashes[lane] ^ word) * 0x9e3779b97f4a7c15ull;
        hashes[lane] ^= hashes[lane] >> 29;
    };
    auto mixWords = [&](unsigned lane, const void* start, size_t numberOfBytes) {
        const uint8_t* bytes = static_cast<const uint8_t*>(start);
        for (size_t offset = 0; offset + sizeof(uint64_t) <= numberOfBytes; offset += sizeof(uint64_t))
            mix(lane, WTF::unalignedLoad<uint64_t>(bytes + offset));
    };
    constexpr unsigned shapeWords = 0, orderWords = 1, objectFields = 2, arrayStorage = 4, elements = 5, stringStorage = 6, typedArrayStorage = 7;
    static_assert(Typed::heapsOfDigestInEffectsNet[shapeWords] == Typed::AbstractHeap::ShapeWords && Typed::heapsOfDigestInEffectsNet[typedArrayStorage] == Typed::AbstractHeap::TypedArrayStorage);

    JSCell* cell = value.asCell();
    if (cell->isString()) {
        if (const StringImpl* found = asString(cell)->tryGetValueImpl()) {
            const uint8_t* impl = reinterpret_cast<const uint8_t*>(found);
            mix(stringStorage, reinterpret_cast<uintptr_t>(impl));
            mix(stringStorage, WTF::unalignedLoad<uint32_t>(impl + StringImpl::flagsOffset()));
            mix(stringStorage, WTF::unalignedLoad<uint32_t>(impl + StringImpl::lengthMemoryOffset()));
            mix(stringStorage, WTF::unalignedLoad<uint64_t>(impl + StringImpl::dataOffset()));
        } else
            mixWords(stringStorage, reinterpret_cast<const uint8_t*>(cell) + sizeof(JSCell), sizeof(JSRopeString) - sizeof(JSCell));
    } else if (Object* compiled = compiledObjectOrNull(cell)) {
        mix(shapeWords, compiled->shapeWord());
        mix(orderWords, compiled->orderWord());
        mixWords(objectFields, reinterpret_cast<const uint8_t*>(cell) + offsetOfFirstField, compiled->layout().size - offsetOfFirstField);
    } else if (cell->isObject()) {
        JSObject* object = asObject(cell);
        Butterfly* butterfly = object->butterfly();
        bool isArray = cell->type() == ArrayType || cell->type() == DerivedArrayType;
        if (isArray) {
            mix(arrayStorage, object->indexingTypeAndMisc() & AllArrayTypesAndHistory);
            mix(arrayStorage, reinterpret_cast<uintptr_t>(butterfly));
        }
        if (butterfly && hasAnyArrayStorage(object->indexingType())) {
            mix(arrayStorage, butterfly->arrayStorage()->length());
            mix(arrayStorage, butterfly->arrayStorage()->vectorLength());
        } else if (butterfly && hasIndexedProperties(object->indexingType())) {
            mix(arrayStorage, butterfly->publicLength());
            mix(arrayStorage, butterfly->vectorLength());
            mixWords(elements, butterfly->contiguous().data(), std::min<size_t>(butterfly->publicLength(), butterfly->vectorLength()) * sizeof(EncodedJSValue));
        }
        if (isTypedArrayTypeIncludingDataView(cell->type())) {
            JSArrayBufferView* view = uncheckedDowncast<JSArrayBufferView>(cell);
            mix(typedArrayStorage, reinterpret_cast<uintptr_t>(view->vectorWithoutPACValidation()));
            mix(typedArrayStorage, view->lengthRaw());
            mix(typedArrayStorage, view->byteOffsetRaw());
            mix(typedArrayStorage, static_cast<uint64_t>(view->mode()));
            if (!view->isShared())
                mix(typedArrayStorage, view->byteLength());
        } else if (cell->type() == ArrayBufferType) {
            if (ArrayBuffer* buffer = uncheckedDowncast<JSArrayBuffer>(cell)->impl(); !buffer->isShared()) {
                mix(typedArrayStorage, reinterpret_cast<uintptr_t>(buffer->data()));
                mix(typedArrayStorage, buffer->byteLength());
            }
        }
    }
    return digestOfLanesForEffectsNet(hashes, lanes);
}

static Identifier identifierOf(VM& vm, StringID text)
{
    return Identifier::fromString(vm, ProgramStrings::atomOf(*Image::current(), text));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewRuntimeErrorWithName, JSCell*, (Instance* instance, uint32_t errorKind, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRuntimeErrorWithName);
    DeferTerminationForAWhile deferTermination(vm);
    Identifier identifier = identifierOf(vm, StringID(name));
    RELEASE_AND_RETURN(scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(errorKind), &identifier));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewSuppressedError, JSCell*, (Instance* instance, EncodedJSValue error, EncodedJSValue suppressed))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSuppressedError);
    DeferTerminationForAWhile deferTermination(vm);
    constexpr bool useCurrentFrame = true;
    RELEASE_AND_RETURN(scope, createSuppressedError(globalObject, vm, globalObject->errorStructure(ErrorType::SuppressedError), JSValue::decode(error), JSValue::decode(suppressed), jsUndefined(), nullptr, TypeNothing, useCurrentFrame));
}

ErrorType errorTypeFor(Typed::ErrorKind kind)
{
    switch (kind) {
    case Typed::ErrorKind::Error:
        return ErrorType::Error;
    case Typed::ErrorKind::EvalError:
        return ErrorType::EvalError;
    case Typed::ErrorKind::RangeError:
        return ErrorType::RangeError;
    case Typed::ErrorKind::ReferenceError:
        return ErrorType::ReferenceError;
    case Typed::ErrorKind::SyntaxError:
        return ErrorType::SyntaxError;
    case Typed::ErrorKind::TypeError:
        return ErrorType::TypeError;
    case Typed::ErrorKind::URIError:
        return ErrorType::URIError;
    case Typed::ErrorKind::AggregateError:
        return ErrorType::AggregateError;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ErrorInstance* createErrorWithMessage(Instance* instance, JSGlobalObject* globalObject, Typed::ErrorKind kind, StringID message)
{
    ErrorType errorType = errorTypeFor(kind);
    constexpr bool useCurrentFrame = true;
    return ErrorInstance::create(globalObject, globalObject->errorStructure(errorType), instance->state->strings->string(globalObject->vm(), message), jsUndefined(), nullptr, TypeNothing, errorType, useCurrentFrame);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowRuntimeError, void, (Instance* instance, uint32_t errorKind))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowRuntimeError);
    throwException(globalObject, scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(errorKind), nullptr));
    OPERATION_RETURN(scope);
}

static void throwUninitializedBinding(JSGlobalObject* globalObject, ThrowScope& scope, uint32_t name)
{
    if (name == ThrowSiteWord::none) {
        throwException(globalObject, scope, createTDZError(globalObject));
        return;
    }
    throwException(globalObject, scope, createTDZError(globalObject, Image::current()->text().view(StringID(name))));
}

struct DecodedThrowSite {
    ThrowSiteWord word;
    uint32_t a;
    bool isFaultSite;
};

static uint32_t wordBehindCall(const uint32_t* site, size_t index)
{
    return WTF::unalignedLoad<uint32_t>(reinterpret_cast<const uint8_t*>(site) + (index + 1) * ThrowSiteWord::sizeOfWordBehindCall - sizeof(uint32_t));
}

static DecodedThrowSite decodeThrowSite(const uint64_t* registers, const uint32_t* site, ThrowSiteSource source)
{
    std::array<uint32_t, FaultSite::maxThrowSiteWords> localWords { };
    const uint32_t* words = nullptr;
    switch (source) {
    case ThrowSiteSource::WordsAfterCall:
        localWords[0] = wordBehindCall(site, 0);
        if (std::bit_cast<ThrowSiteWord>(localWords[0]).hasWideA)
            localWords[1] = wordBehindCall(site, 1);
        words = localWords.data();
        break;
    case ThrowSiteSource::WordInRegister: {
        uint64_t wordsInRegister = registers[static_cast<unsigned>(throwWordGPR)];
        localWords[0] = static_cast<uint32_t>(wordsInRegister);
        localWords[1] = static_cast<uint32_t>(wordsInRegister >> 32);
        words = localWords.data();
        break;
    }
    case ThrowSiteSource::FaultSite: {
        auto faultSite = Image::current()->faultSiteAt(reinterpret_cast<const uint8_t*>(site) - sizeOfNearCall);
        RELEASE_ASSERT(faultSite);
        localWords = faultSite->throwSite;
        words = localWords.data();
        break;
    }
    }
    ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(*words++);
    uint32_t a = word.hasWideA ? *words : word.narrowA();
    return { word, a, source == ThrowSiteSource::FaultSite };
}

JSValue valueOfDecodedReference(const Instance* instance, uint64_t bits)
{
    uint64_t base = instance->cellCageBase;
    if (base && bits >= base && bits - base <= (static_cast<uint64_t>(undefinedCell32) << MarkedBlockCage::shift))
        return JSValue::decode((bits - base) >> MarkedBlockCage::shift);
    return JSValue::decode(bits);
}

static JSValue throwSiteValue(const Instance* instance, const uint64_t* registers, const DecodedThrowSite& decoded)
{
    if (static_cast<Typed::Representation>(decoded.word.representation) == Typed::Representation::Void)
        return jsUndefined();
    unsigned number = decoded.word.registerNumber;
    RELEASE_ASSERT(number < ThrowSiteWord::numberOfStoredFPRs);
    uint64_t bits = registers[number];
    uint64_t floatingPointBits = registers[ThrowSiteWord::numberOfStoredGPRs + number];
    JSValue value;
    switch (static_cast<Typed::Representation>(decoded.word.representation)) {
    case Typed::Representation::Void:
        return jsUndefined();
    case Typed::Representation::Boolean:
        return jsBoolean(bits & 1);
    case Typed::Representation::Int32:
        return jsNumber(static_cast<int32_t>(bits));
    case Typed::Representation::Int64:
        return jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
    case Typed::Representation::Double:
        return jsNumber(withoutPayload(std::bit_cast<double>(floatingPointBits)));
    case Typed::Representation::Cell:
        value = decoded.isFaultSite ? valueOfDecodedReference(instance, bits) : JSValue::decode(bits);
        return value ? value : jsUndefined();
    case Typed::Representation::Cell32: {
        uint32_t reference = static_cast<uint32_t>(bits);
        if (reference > undefinedCell32)
            return MarkedBlockCage::decode(instance->cellCageBase, reference);
        value = JSValue::decode(reference);
        return value ? value : jsUndefined();
    }
    case Typed::Representation::JSValue:
        value = decoded.isFaultSite ? valueOfDecodedReference(instance, bits) : JSValue::decode(bits);
        return value ? value : jsUndefined();
    case Typed::Representation::OptionalInt64:
        if (static_cast<int64_t>(bits) == nullInteger)
            return jsNull();
        if (static_cast<int64_t>(bits) == undefinedInteger || static_cast<int64_t>(bits) == absentInteger)
            return jsUndefined();
        return jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
    case Typed::Representation::OptionalDouble:
        if (floatingPointBits == nullDouble)
            return jsNull();
        if (floatingPointBits == undefinedDouble || floatingPointBits == absentDouble)
            return jsUndefined();
        return jsNumber(withoutPayload(std::bit_cast<double>(floatingPointBits)));
    case Typed::Representation::Unchosen:
    case Typed::Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void throwErrorWithMessage(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, const DecodedThrowSite& decoded)
{
    if (decoded.word.throwsRuntimeError()) {
        std::optional<Identifier> name;
        if (decoded.a != ThrowSiteWord::none)
            name = identifierOf(globalObject->vm(), StringID(decoded.a));
        throwException(globalObject, scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(decoded.word.registerNumber), name ? &*name : nullptr));
        return;
    }
    ErrorInstance* error = createErrorWithMessage(instance, globalObject, static_cast<Typed::ErrorKind>(decoded.word.registerNumber), StringID(decoded.a));
    RETURN_IF_EXCEPTION(scope, void());
    instance->runtime.exceptions.thrownValue = JSValue::encode(error);
}

[[noreturn]] static void stopAtFalseProofBehind(Instance& instance, const uint32_t* returnAddress, EncodedJSValue found, uint32_t source, uint32_t expectedType)
{
    RELEASE_ASSERT(Image::current()->locate(returnAddress).kind == CodeKind::Function);
    RuntimeCheckFailure failure(failedCheckOf(source));
    failure.atReturnAddress(returnAddress);
    stopAtFalseProof(instance, failure, found, source, expectedType);
}

static uint32_t expectedTypeOfKinds(uint32_t kinds, JSValue found)
{
    if (!(kinds & KindsOfProof::everyObject) && found && found.isObject())
        return 0;
    uint16_t primitives = kinds & KindsOfProof::primitives;
    if (primitives & KindsOfProof::everyNumber)
        primitives |= Typed::Primitive::Number;
    return FalseProofWord::summarize(primitives, kinds & KindsOfProof::everyObject);
}

static bool isEntryThatInspectorBroughtCallTo(VM& vm, const Instance& instance, const uint32_t* returnAddress, uint32_t source)
{
    if (source != static_cast<uint32_t>(Typed::ProofSource::EntryParameter) || !isAskedByCodeThatInspectorBrought(vm, instance, FrameThatAsks::IsAboveTheTopFrame))
        return false;
    const Image& image = *Image::current();
    CodeLocation location = image.locate(returnAddress);
    if (location.kind != CodeKind::Function)
        return false;
    std::optional<CallSiteRecord> record = image.callSiteAt(location.function, location.offset);
    return !record || !record->inlineFrame;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopUnlessValueHasType, void, (Instance* instance, uint32_t kindsAndSource, uint32_t type, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTStopUnlessValueHasType);
    const RuntimeLayoutTable& layouts = instance->state->objects->layouts;
    const RuntimeType& expected = layouts.typeAt(type);
    if (!layouts.valueHasType(expected, JSValue::decode(value))) [[unlikely]]
        stopAtFalseProofBehind(*instance, static_cast<const uint32_t*>(std::bit_cast<const CallerFrameAndPC*>(callFrame)->returnPC), value, kindsAndSource >> KindsOfProof::shiftOfSource, FalseProofWord::summarize(expected.primitiveSet().toRaw(), expected.numberOfCellTests));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowAtSite, void, (Instance* instance, const uint64_t* registers, const uint32_t* site, uint32_t source))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTThrowAtSite);
    if (static_cast<ThrowSiteSource>(source) == ThrowSiteSource::FaultSite)
        const_cast<uint64_t*>(registers)[static_cast<unsigned>(registerWrittenAtFaults)] = instance->state->registerBeforeFault;
    DecodedThrowSite decoded = decodeThrowSite(registers, site, static_cast<ThrowSiteSource>(source));
    if (decoded.word.stopsAtFalseProof()) {
        EncodedJSValue found = static_cast<Typed::Representation>(decoded.word.representation) != Typed::Representation::Void ? registers[decoded.word.registerNumber] : 0;
        uint32_t proofSource = FalseProofWord::sourceOf(decoded.a);
        uint32_t expectedType = FalseProofWord::expectedTypeOf(decoded.a);
        if (decoded.word.hasWideA) {
            proofSource = decoded.a >> KindsOfProof::shiftOfSource;
            expectedType = expectedTypeOfKinds(decoded.a & KindsOfProof::all, JSValue::decode(found));
        }
        if (isEntryThatInspectorBroughtCallTo(vm, *instance, site, proofSource)) {
            throwTypeError(globalObject, scope, changeThatCompiledCodeRulesOutMessage("call this function with these values"_s));
            OPERATION_RETURN(scope);
        }
        stopAtFalseProofBehind(*instance, site, found, proofSource, expectedType);
    }
    auto value = [&] {
        return throwSiteValue(instance, registers, decoded);
    };
    if (decoded.isFaultSite && static_cast<Typed::Representation>(decoded.word.representation) == Typed::Representation::JSValue) {
        if (JSValue found = value(); found.isNumber() || found.isBoolean())
            stopAtFalseProofBehind(*instance, site, JSValue::encode(found), static_cast<uint32_t>(Typed::ProofSource::Contents), 0);
    }
    if (!needsExceptionForEveryThrow(*instance) && nearestHandlerIgnoresValue(vm, *instance, callFrame)) {
        instance->runtime.exceptions.thrownValue = JSValue::encode(jsUndefined());
        OPERATION_RETURN(scope);
    }
    switch (static_cast<ThrowKind>(decoded.word.kind)) {
    case ThrowKind::NullishRead:
    case ThrowKind::NullishWrite:
        throwException(globalObject, scope, createNotAnObjectError(globalObject, value(), decoded.a == ThrowSiteWord::none ? StringView() : Image::current()->text().view(StringID(decoded.a))));
        break;
    case ThrowKind::NullishIterate:
        throwException(globalObject, scope, createIterationOfNullishError(globalObject, value()));
        break;
    case ThrowKind::NullishSpread:
        throwException(globalObject, scope, createSpreadOfNullishError(globalObject));
        break;
    case ThrowKind::UninitializedBinding:
        throwUninitializedBinding(globalObject, scope, decoded.a);
        break;
    case ThrowKind::NotAFunction:
        throwException(globalObject, scope, createNotAFunctionError(globalObject, value(), decoded.a == ThrowSiteWord::none ? StringView() : Image::current()->text().view(StringID(decoded.a))));
        break;
    case ThrowKind::ErrorWithMessage:
        throwErrorWithMessage(instance, globalObject, scope, decoded);
        break;
    case ThrowKind::NullishUpdate:
        throwException(globalObject, scope, createUpdateOfNullishError(globalObject));
        break;
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
