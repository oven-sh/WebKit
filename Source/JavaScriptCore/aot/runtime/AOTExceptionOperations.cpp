/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCallSiteTable.h"
#include "AOTClassPrototype.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTEffectsNet.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMembersLeftOut.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPoisonedCell.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
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
#include "FunctionExecutable.h"
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

SUPPRESS_ASAN static const void* returnAddressOfPolledCall(void* stackPointerOfCaller)
{
    return removeCodePtrTag(static_cast<void* const*>(stackPointerOfCaller)[isX86() ? -1 : -2]);
}

static bool hasCollectedInPolledCall(VM& vm, void* stackPointerOfCaller, CallFrame* frameOfPollStub)
{
    CallFrame* framePointerOfCaller = isX86() ? static_cast<CallFrame*>(frameOfPollStub->callerFrameOrEntryFrame()) : frameOfPollStub;
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreate(vm, framePointerOfCaller, returnAddressOfPolledCall(stackPointerOfCaller), vm.topEntryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, stackPointerOfCaller);
    return frames && !frames->isAtEnd() && frames->callSite().address == vm.aotState().returnAddressOfCallThatCollected;
}

static NEVER_INLINE void throwOwedOutOfMemoryError(VM& vm, JSGlobalObject* globalObject, ThrowScope& scope)
{
    DeferGCForAWhile deferGC(vm);
    throwOutOfMemoryErrorIfOwed(vm, globalObject, scope);
}

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
    if (JSValue value = JSValue::decode(found); value.isObject() && written.kinds & WrittenNativeResult::objectKind)
        answered.print("an object of a class that the row does not name: "_s, asObject(value)->classInfo()->className);
    else
        printKindsOfNativeResult(answered, kindOfNativeResult(value));
    dataLogLn("AOT: ", BuiltinTable::singleton().textOf(BuiltinID(written.builtin)), written.constructs ? " (construct)" : " (call)", " is declared as ", declared.toString(), ", and native code answered with something else: ", answered.toString(), ". This is a bug in Bun.");
    CRASH();
}

static void stopIfSaidNeverToReachUnseenCode(VM& vm, const State& state, JSValue handedOver)
{
    if (!handedOver || !handedOver.isCell())
        return;
    countEvent(vm, RuntimeCounterID::CellsHandedToUnseenCode);
    JSCell* cell = handedOver.asCell();
    StringPrintStream what;
    if (isCompiledObject(cell)) {
        const LayoutData& layout = uncheckedDowncast<Object>(cell)->layout();
        if (!layout.isSaidNeverToReachUnseenCode)
            return;
        what.print("an object of layout L"_s, layout.index);
    } else if (std::optional<FunctionID> function = functionOf(cell)) {
        if (!Image::current()->programTables().traceOfFunction(function->index()).closuresAreSaidNeverToReachUnseenCode)
            return;
        what.print("a closure of f"_s, sourceFunctionOf(*function).index());
    } else if (cell->isObject()) {
        const ClassTable& classes = state.objects->classes;
        ClassSide side = ClassSide::Prototype;
        const LayoutData* layout = classes.classHeldBy(asObject(cell), side);
        if (!layout && cell->classInfo() == ClassPrototype::info())
            layout = classes.classOf(*uncheckedDowncast<ClassPrototype>(cell));
        if (!layout || !layout->isSaidNeverToReachUnseenCode)
            return;
        what.print(side == ClassSide::Prototype ? "the prototype object of class L"_s : "the class object of class L"_s, layout->index);
    } else
        return;
    dataLogLn("AOT: ", what.toString(), " is handed to code that the compiler has not read, and the compiler took it never to leave. This is a bug in Bun.");
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
    countEvent(vm, PoisonedCell::tryPoison(vm, cell, function, position) ? RuntimeCounterID::CellsPoisoned : RuntimeCounterID::CellsOfClassesThatAreNotPoisoned);
}

[[noreturn]] static void stopAtFalseProof(EncodedJSValue encodedFound, uint32_t function, const String& position, uint32_t source)
{
    static constexpr std::array<ASCIILiteral, 11> names {
        "an entry parameter"_s, "the result of a call"_s, "what a field holds"_s, "what a variable holds"_s, "what was captured"_s, "what a slot of a scope holds"_s, "what a container holds"_s, "what a built-in or a call by value gives"_s, "what an operation gives"_s, "what a test has let pass"_s, "what memory holds where the compiler remembers another value"_s,
    };
    if (JSValue value = JSValue::decode(encodedFound); value && value.isCell() && value.asCell()->classInfo() == PoisonedCell::info())
        PoisonedCell::stop(value.asCell());
    StringPrintStream found;
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtFalseProof, void, (Instance*, EncodedJSValue encodedFound, uint32_t function, uint32_t position, uint32_t source))
{
    StringPrintStream written;
    written.print(position);
    stopAtFalseProof(encodedFound, function, written.toString(), source);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnterBodyThatRunsOnce, void, (Instance* instance, uint32_t function))
{
    if (instance->state->functionsEnteredThatRunOnce.add(function).isNewEntry) [[likely]]
        return;
    dataLogLn("AOT: f", function, " is entered a second time, and the compiler has proven that it runs at most once. This is a bug in Bun.");
    CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStopAtFalseEffects, void, (Instance*, uint32_t function, uint32_t position, uint32_t part, uint32_t rows, uint64_t differs))
{
    static constexpr std::array<ASCIILiteral, 5> names { "nothing"_s, "run no other code, and compiled code was entered"_s, "throw nothing by a rule of the language, and it threw"_s, "nothing"_s, "not collect, and the heap was collected"_s };
    static const bool goesOn = !!getenv("BUN_AOT_EFFECTS_NET_GOES_ON");
    StringPrintStream said;
    said.print("f", function, " at ", position, ": the rows of effects of ");
    printRowOfEffectsNet(said, rows & UINT16_MAX);
    said.print(" and of ");
    printRowOfEffectsNet(said, rows >> 16);
    if ((part & 0xff) == static_cast<uint32_t>(Typed::PartOfEffects::Writes)) {
        said.print(" say that the call will not write ");
        CommaPrinter comma;
        for (unsigned lane = 0; lane < Typed::heapsOfDigestInEffectsNet.size(); ++lane) {
            if (differs >> lane * 8 & 0xff)
                said.print(comma, Typed::nameOf(Typed::heapsOfDigestInEffectsNet[lane]));
        }
        said.print(", and those words of operand ", part >> 16, " are others behind it");
    } else {
        if (differs)
            said.print(" (", BuiltinTable::singleton().textOf(BuiltinID(static_cast<uint16_t>(differs))), ")");
        said.print(" say that the call will ", names[std::min<size_t>(part, names.size() - 1)]);
    }
    if (goesOn && !noteFalseRowOfEffects(said.toString()))
        return;
    dataLogLn("AOT: ", said.toString(), ". This is a bug in Bun.");
    if (!goesOn)
        CRASH();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCollectionsForEffectsNet, uint64_t, (uint64_t vm))
{
    return reinterpret_cast<VM*>(vm)->heap.objectSpace().newlyAllocatedVersion();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDigestForEffectsNet, uint64_t, (EncodedJSValue encodedValue, uint64_t lanes))
{
    JSValue value = JSValue::decode(encodedValue);
    if (!value || !value.isCell())
        return 0;
    std::array<uint64_t, Typed::heapsOfDigestInEffectsNet.size()> hashes { };
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
        }
    }
    uint64_t digest = 0;
    for (unsigned lane = 0; lane < hashes.size(); ++lane) {
        if (lanes & 1ull << lane)
            digest |= (hashes[lane] >> 56) << lane * 8;
    }
    return digest;
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
    ErrorInstance* error = createErrorWithMessage(instance, globalObject, static_cast<Typed::ErrorKind>(decoded.word.registerNumber), StringID(decoded.a));
    RETURN_IF_EXCEPTION(scope, void());
    instance->runtime.exceptions.thrownValue = JSValue::encode(error);
}

[[noreturn]] static void stopAtFalseProofNamedBy(const DecodedThrowSite& decoded, const uint64_t* registers, const uint32_t* returnAddress)
{
    bool namesValue = static_cast<Typed::Representation>(decoded.word.representation) != Typed::Representation::Void;
    const Image& image = *Image::current();
    CodeLocation location = image.locate(returnAddress);
    RELEASE_ASSERT(location.kind == CodeKind::Function);
    FunctionID writtenIn = image.rootFunctionOf(location.function);
    StringPrintStream position;
    std::optional<CallSiteRecord> record = image.callSiteAt(location.function, location.offset);
    if (record && record->inlineFrame)
        writtenIn = InlineFrameReader().at(image, location.function, record->inlineFrame).function;
    if (std::optional<SourcePosition> found = record ? image.positionOf(location.function, record->callSite) : std::nullopt)
        position.print(found->lineColumn.line, ":", found->lineColumn.column);
    else
        position.print("?");
    stopAtFalseProof(namesValue ? registers[decoded.word.registerNumber] : 0, writtenIn.index(), position.toString(), decoded.a);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowAtSite, void, (Instance* instance, const uint64_t* registers, const uint32_t* site, uint32_t source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowAtSite);
    if (static_cast<ThrowSiteSource>(source) == ThrowSiteSource::FaultSite)
        const_cast<uint64_t*>(registers)[static_cast<unsigned>(registerWrittenAtFaults)] = instance->state->registerBeforeFault;
    DecodedThrowSite decoded = decodeThrowSite(registers, site, static_cast<ThrowSiteSource>(source));
    if (decoded.word.stopsAtFalseProof())
        stopAtFalseProofNamedBy(decoded, registers, site);
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
