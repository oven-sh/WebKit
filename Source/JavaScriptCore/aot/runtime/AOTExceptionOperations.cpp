/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTErrorMessages.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeValidation.h"
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
#include "SuppressedError.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <array>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static const RuntimeType& runtimeTypeAt(Instance* instance, uint32_t type)
{
    const RuntimeType* result = instance->state->objects->layouts.typeWithIndex(type);
    RELEASE_ASSERT(result);
    return *result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollSlow);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    vm.throwTerminationExceptionIfNeeded();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollAtPrologue, void*, (Instance* instance, uint8_t* returnAddress))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollAtPrologue);
    // What a caller knows about a callee that collects nothing holds for the callee's prologue too.
    DeferGCForAWhile deferGC(vm);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    vm.throwTerminationExceptionIfNeeded();
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (vm.traps().needsOutOfMemoryError()) [[unlikely]] {
        OPERATION_RETURN(scope, std::bit_cast<void*>(static_cast<uintptr_t>(pollAtPrologueMustThrow)));
    }
    OPERATION_RETURN(scope, resumeAddressAfterStackCheck(returnAddress));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowToHandlerInFunction, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowToHandlerInFunction);
    DeferGCForAWhile deferGC(vm);
    throwException(globalObject, scope, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrow, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrow);
    if (vm.exceptionForInspection()) [[unlikely]] {
        DeferGCForAWhile deferGC(vm);
        throwException(globalObject, scope, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    instance->runtime.exceptions.thrownValue = value;
    return { std::bit_cast<Exception*>(static_cast<uintptr_t>(1)) };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    DeferGC collectsWhatThrowingPutOff(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception && !vm.isTerminationException(exception));
    JSValue value = exception->value();
    scope.clearException();
    return JSValue::encode(value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatchException, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    DeferGC collectsWhatThrowingPutOff(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception && !vm.isTerminationException(exception));
    scope.clearException();
    return JSValue::encode(exception);
}

JSC_DEFINE_JIT_OPERATION(operationAOTReportMismatch, void, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t typeSite))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReportMismatch);
    DeferGCForAWhile deferGC(vm);
    JSValue decoded = JSValue::decode(value);
    JSValue checked = decoded ? decoded : jsUndefined();
    const RuntimeType& expected = runtimeTypeAt(instance, type);
    if (!instance->state->objects->layouts.valueHasType(expected, checked)) [[unlikely]]
        reportFailedCheck(*instance, typeSite, expected, checked);
    OPERATION_RETURN(scope);
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
        return createInvalidPrivateNameError(globalObject);
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
    case Typed::RuntimeErrorKind::LeftOutOfExecutable:
        if (validatesRuntime(globalObject->vm()))
            crashOnValidationFailure(globalObject->vm(), { ValidationFailureKind::UnreachableValue, { }, { }, JSValue() });
        if (name && !name->isEmpty())
            return createTypeError(globalObject, makeString('\'', name->string(), "' was left out of the executable, because nothing seemed to refer to it. This is a bug in Bun."_s));
        return createTypeError(globalObject, "This function was left out of the executable, because nothing seemed to refer to it. This is a bug in Bun."_s);
    case Typed::RuntimeErrorKind::FunctionNotCompiled:
        return createError(globalObject, name && !name->isEmpty() ? functionNotCompiledMessage(name->string(), ""_s) : functionNotCompiledMessage(""_s));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewRuntimeError, JSCell*, (Instance* instance, uint32_t errorKind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRuntimeError);
    DeferTerminationForAWhile deferTermination(vm);
    RELEASE_AND_RETURN(scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(errorKind), nullptr));
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
    constexpr bool useCurrentFrame = false;
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
    case ThrowSiteSource::WordInRegister:
        localWords[0] = static_cast<uint32_t>(registers[static_cast<unsigned>(throwWordGPR)]);
        words = localWords.data();
        break;
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
    unsigned number = decoded.word.registerNumber;
    uint64_t bits = registers[number];
    uint64_t floatingPointBits = registers[ThrowSiteWord::numberOfRegistersPerBank + number];
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
        return jsNumber(purifyNaN(std::bit_cast<double>(floatingPointBits)));
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
        return jsNumber(purifyNaN(std::bit_cast<double>(floatingPointBits)));
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

// It throws nothing. It is declared to throw so that it stays under the handlers of its place, and has a site that names them.
JSC_DEFINE_JIT_OPERATION(operationAOTIsThrownValueIgnored, size_t, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTIsThrownValueIgnored);
    OPERATION_RETURN(scope, !needsExceptionForEveryThrow(*instance) && nearestHandlerIgnoresValue(vm, *instance, callFrame));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowAtSite, void, (Instance* instance, const uint64_t* registers, const uint32_t* site, uint32_t source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowAtSite);
    // The fault handler has passed an address in a register that compiled code allocates like any other.
    if (static_cast<ThrowSiteSource>(source) == ThrowSiteSource::FaultSite)
        const_cast<uint64_t*>(registers)[static_cast<unsigned>(registerWrittenAtFaults)] = instance->state->registerBeforeFault;
    DecodedThrowSite decoded = decodeThrowSite(registers, site, static_cast<ThrowSiteSource>(source));
    if (!needsExceptionForEveryThrow(*instance) && nearestHandlerIgnoresValue(vm, *instance, callFrame)) {
        count(*instance, RuntimeCounterID::ThrownErrorsNotCreated);
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
        throwException(globalObject, scope, createNotAnObjectError(globalObject, value()));
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
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
