/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCallSiteTable.h"
#include "AOTCompiledFrame.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeFunctions.h"
#include "AOTSlotRepresentation.h"
#include "AOTState.h"
#include "AOTStubRegisters.h"
#include "AOTStubsPolls.h"
#include "AOTUnwinding.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "IteratorOperations.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "OwedOutOfMemoryError.h"
#include "SuppressedError.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <array>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

// generatePollBeforeEntering() has the call's return address on the stack: where the call pushed it, or below the stub's.
SUPPRESS_ASAN static const void* returnAddressOfPolledCall(void* stackPointerOfCaller)
{
    return removeCodePtrTag(static_cast<void* const*>(stackPointerOfCaller)[isX86() ? -1 : -2]);
}

static bool hasCollectedInPolledCall(VM& vm, void* stackPointerOfCaller, CallFrame* frameOfPollStub)
{
    // Another stub may have made the polled call. generatePollBeforeEntering() sets up a frame record on x86-64 only.
    CallFrame* framePointerOfCaller = isX86() ? static_cast<CallFrame*>(frameOfPollStub->callerFrameOrEntryFrame()) : frameOfPollStub;
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreate(vm, framePointerOfCaller, returnAddressOfPolledCall(stackPointerOfCaller), vm.topEntryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, stackPointerOfCaller);
    return frames && !frames->isAtEnd() && frames->callSite().address == vm.aotState().returnAddressOfCallThatCollected;
}

// Out of line so the frame that a poll collects from has no slot that only the throw writes.
static NEVER_INLINE void throwOwedOutOfMemoryError(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope)
{
    DeferGCForAWhile deferGC(vm);
    throwOutOfMemoryErrorIfOwed(vm, globalObject, scope);
}

// The compiler does not see a poll inside a call stub: the call's IR operation may say that it neither collects nor throws.
// A call inside which a collection has run says both. At any other call the error waits for the prologue of a function that may collect, or for a loop poll.
static void pollInCallStub(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope, void* stackPointerOfCaller, CallFrame* frameOfPollStub)
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
    if (!hasCollectedInPolledCall(vm, stackPointerOfCaller, frameOfPollStub) || vm.heap.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm))
        return;
    throwOwedOutOfMemoryError(vm, globalObject, scope);
}

// Asks what hasCollectedInPolledCall() is about to ask. Nothing allocates between here and that poll, so it collects nothing more.
void collectInPlaceOfPollBehindOperation(VM& vm, JSCell* madeByOperation)
{
    if (vm.aotState().returnAddressOfCallThatCollected != returnAddressOfInnermostCompiledCall(vm))
        return;
    vm.heap.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm);
    vm.writeBarrier(madeByOperation);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollBeforeEntering, void, (Instance* instance, void* stackPointerOfCaller))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollBeforeEntering);
    pollInCallStub(vm, globalObject, scope, stackPointerOfCaller, callFrame);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollBeforeEnteringFunctionObject, void, (Instance* instance, JSCell*, void* stackPointerOfCaller, void*))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollBeforeEnteringFunctionObject);
    pollInCallStub(vm, globalObject, scope, stackPointerOfCaller, callFrame);
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
    // Thrown at such a poll, the error would pass the catch clause of the try block around the loop. It stays owed until a poll under a handler, a function entry or the return of an operation.
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
    // What a caller knows about a callee that collects nothing holds for the callee's prologue too.
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

// The program threw the value somewhere else, and a cleanup that took the value throws it on.
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

// A `throw` that jumps to a cleanup of its function calls nothing else, and a stack trace finds the place of a frame by a return address.
// The address lies in the function. A stub that calls the operation has a frame of its own in between, and one that jumps to it has none.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAddressOfThrow, uint64_t, (Instance* instance))
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, AOT_OPERATION_FRAME_RECORD());
    const void* returnAddress = CompiledFrameIterator::findReturnAddressInFunctionAtTop(vm);
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
    // Null where compiled code threw the value and nothing has made an Exception of it.
    Exception* exception;
};

// The word holds what compiled code threw: the value, or the Exception that a cleanup throws on. While it is empty the exception is pending in the VM.
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
    // A collection that starts here goes over the frames from this one.
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
    // Nobody lists the frames of a throw that compiled code catches further up.
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

// `node:fs`, where the function on top is of a module that a program can import.
static String importableModuleOnTop(VM& vm)
{
    auto frames = CompiledFrameIterator::tryCreateAtTop(vm);
    if (!frames || frames->isAtEnd())
        return { };
    String url = frames->frame().sourceURL();
    return url.startsWith("node:"_s) || url.startsWith("bun:"_s) ? url : String();
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
    case Typed::RuntimeErrorKind::IteratorIsNotAnObject:
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
    case Typed::RuntimeErrorKind::PrivateStaticTestOfNonObject:
        return createStaticPrivateBrandOfNonObjectError(globalObject);
    case Typed::RuntimeErrorKind::SpreadResultNotObject:
        return createSpreadResultNotObjectError(globalObject);
    case Typed::RuntimeErrorKind::NoSyncIteratorForAsyncIteration:
        return createTypeError(globalObject, "iterable should have an iterator symbol"_s);
    case Typed::RuntimeErrorKind::SyncIteratorForAsyncIterationIsNotAnObject:
        return createTypeError(globalObject, "iterator method should return an object"_s);
    case Typed::RuntimeErrorKind::LeftOutOfExecutable:
        // An error that the program could catch would let it go on with a wrong answer.
        if (String nameOfFunction = name ? name->string() : nameOfFunctionOnTop(globalObject->vm()); !nameOfFunction.isEmpty())
            dataLogLn('\'', nameOfFunction, "' was left out of the executable, because nothing seemed to refer to it. This is a bug in Bun.");
        else
            dataLogLn("This function was left out of the executable, because nothing seemed to refer to it. This is a bug in Bun.");
        CRASH();
    case Typed::RuntimeErrorKind::FunctionNotCompiled:
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

static uint32_t kindOfNativeResult(JSValue found)
{
    using Written = WrittenNativeResult;
    if (found.isUndefined())
        return Written::undefinedKind;
    if (found.isNull())
        return Written::nullKind;
    if (found.isBoolean())
        return Written::booleanKind;
    if (found.isNumber())
        return Written::numberKind;
    if (found.isString())
        return Written::stringKind;
    if (found.isSymbol())
        return Written::symbolKind;
    if (found.isBigInt())
        return Written::bigIntKind;
    return found.isObject() ? Written::objectKind : 0;
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtNativeResult, void, (Instance*, EncodedJSValue found, uint32_t encodedWritten))
{
    WrittenNativeResult written = WrittenNativeResult::decode(encodedWritten);
    StringPrintStream declared;
    printKindsOfNativeResult(declared, written.kinds);
    StringPrintStream answered;
    printKindsOfNativeResult(answered, kindOfNativeResult(JSValue::decode(found)));
    dataLogLn("AOT: ", BuiltinTable::singleton().textOf(BuiltinID(written.builtin)), written.constructs ? " (construct)" : " (call)", " is declared as ", declared.toString(), ", and native code answered with something else: ", answered.toString(), ". This is a bug in Bun.");
    CRASH();
}

// By numbers alone: an executable may hold no names. `testaot walk <program> f<function>` shows the CheckProof at the position, with the type.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtFalseProof, void, (Instance*, EncodedJSValue encodedFound, uint32_t function, uint32_t position, uint32_t source))
{
    // By Typed::ProofSource: CastEmitter::generateCheckProof().
    static constexpr std::array<ASCIILiteral, 10> names {
        "an entry parameter"_s, "the result of a call"_s, "what a field holds"_s, "what a variable holds"_s, "what was captured"_s, "what a container holds"_s, "the function that is called"_s, "what an operation gives"_s, "what a test has let pass"_s, "what memory holds where the compiler remembers another value"_s,
    };
    StringPrintStream found;
    // The answer of a test comes without a value. Of a field: whether its slot is filled, and whether a slot for raw numbers holds one of its marks.
    constexpr uint32_t field = 2;
    if (!encodedFound)
        found.print(source + 1 == names.size() ? "another value or a test that once passed fails"_s : source == field ? "nothing, undefined or null"_s : "nothing"_s);
    else if (JSValue value = JSValue::decode(encodedFound); value.isCell() && !kindOfNativeResult(value))
        found.print("a cell that is no value of the language"_s);
    else
        printKindsOfNativeResult(found, kindOfNativeResult(JSValue::decode(encodedFound)));
    dataLogLn("AOT: f", function, " at ", position, ": ", names[std::min<size_t>(source, names.size() - 1)], " is not of the type that the compiler has proven of it. It is: ", found.toString(), ". This is a bug in Bun.");
    CRASH();
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
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowRuntimeError);
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
    // Such a site names no register: ThrowSiteWord::isCallSite().
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
    case Typed::Representation::LiteralString:
        if (isLiteralStringWord(bits))
            return instance->state->strings->stringForLiteralStringWord(bits);
        [[fallthrough]];
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
    ErrorInstance* error = createErrorWithMessage(instance, globalObject, static_cast<Typed::ErrorKind>(decoded.word.registerNumber), StringID(decoded.a));
    RETURN_IF_EXCEPTION(scope, void());
    instance->runtime.exceptions.thrownValue = JSValue::encode(error);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowAtSite, void, (Instance* instance, const uint64_t* registers, const uint32_t* site, uint32_t source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowAtSite);
    // The fault handler has passed an address in a register that compiled code allocates like any other.
    if (static_cast<ThrowSiteSource>(source) == ThrowSiteSource::FaultSite)
        const_cast<uint64_t*>(registers)[static_cast<unsigned>(registerWrittenAtFaults)] = instance->state->registerBeforeFault;
    DecodedThrowSite decoded = decodeThrowSite(registers, site, static_cast<ThrowSiteSource>(source));
    if (!needsExceptionForEveryThrow(*instance) && nearestHandlerIgnoresValue(vm, *instance, callFrame)) {
        instance->runtime.exceptions.thrownValue = JSValue::encode(jsUndefined());
        OPERATION_RETURN(scope);
    }
    auto value = [&] {
        return throwSiteValue(instance, registers, decoded);
    };
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
