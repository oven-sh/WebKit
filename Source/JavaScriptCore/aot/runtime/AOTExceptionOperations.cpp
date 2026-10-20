/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTCompiledFrame.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeReporter.h"
#include "AOTSlotRepresentation.h"
#include "AOTState.h"
#include "AOTStubRegisters.h"
#include "AOTTypeErrorMessages.h"
#include "AOTUnwinding.h"
#include "DeferGCInlines.h"
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
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static const RuntimeType& runtimeTypeAt(Instance* instance, uint32_t type)
{
    const RuntimeType* result = instance->state->objects->layouts.typeWithIndex(type);
    RELEASE_ASSERT(result);
    return *result;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollSlow);
    scope.release();
    vm.traps().handleTraps(VMTraps::AsyncEvents);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPollAtPrologue, void*, (Instance* instance, uint8_t* returnAddress))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPollAtPrologue);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
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
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception && !vm.isTerminationException(exception));
    scope.clearException();
    return JSValue::encode(exception);
}

void throwTypeMismatch(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t type, Typed::TypeSiteID site)
{
    const RuntimeType& expected = runtimeTypeAt(instance, type);
    reportThrownClaim(*instance, expected, value, site);
    throwTypeError(globalObject, scope, withAOTTypeCheckNote(mismatchMessage(*instance->state, globalObject->vm(), value, expected, site)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnforceType, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t typeSite))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEnforceType);
    DeferGCForAWhile deferGC(vm);
    JSValue decoded = JSValue::decode(value);
    JSValue checked = decoded ? decoded : jsUndefined();
    if (!instance->state->objects->layouts.valueHasType(runtimeTypeAt(instance, type), checked)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, checked, type, Typed::TypeSiteID(typeSite));
    OPERATION_RETURN(scope, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTReportMismatch, void, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t typeSite))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTReportMismatch);
    DeferGCForAWhile deferGC(vm);
    JSValue decoded = JSValue::decode(value);
    JSValue checked = decoded ? decoded : jsUndefined();
    const RuntimeType& expected = runtimeTypeAt(instance, type);
    if (!instance->state->objects->layouts.valueHasType(expected, checked)) [[unlikely]]
        reportFailedClaim(*instance, typeSite, expected, checked);
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
        return createReferenceError(globalObject, "'super()' must be called in derived constructor before accessing |this| or returning non-object."_s);
    case Typed::RuntimeErrorKind::ThisAlreadyBound:
        return createReferenceError(globalObject, "'super()' can't be called more than once in a constructor."_s);
    case Typed::RuntimeErrorKind::DeleteSuperProperty:
        return createReferenceError(globalObject, "Cannot delete a super property"_s);
    case Typed::RuntimeErrorKind::NotDestructurable:
        if (name)
            return createTypeError(globalObject, makeString("Cannot destructure property '"_s, name->string(), "' from null or undefined value"_s));
        return createTypeError(globalObject, "Cannot destructure null or undefined value"_s);
    case Typed::RuntimeErrorKind::DerivedConstructorResult:
        if (name)
            return createTypeError(globalObject, makeString("Cannot return a non-object type in the constructor of a derived class "_s, name->string(), "."_s));
        return createTypeError(globalObject, "Cannot return a non-object type in the constructor of a derived class."_s);
    case Typed::RuntimeErrorKind::StaticPrototypeKey:
        return createTypeError(globalObject, "Cannot declare a static field named 'prototype'"_s);
    case Typed::RuntimeErrorKind::IteratorResultNotObject:
    case Typed::RuntimeErrorKind::IteratorIsNotAnObject:
        return createTypeError(globalObject, "Iterator result interface is not an object."_s);
    case Typed::RuntimeErrorKind::IteratorWithoutThrowMethod:
        return createTypeError(globalObject, "The iterator, to which yield* delegated iteration, does not have a 'throw' method."_s);
    case Typed::RuntimeErrorKind::InvalidPrivateRead:
    case Typed::RuntimeErrorKind::InvalidPrivateWrite:
        return createInvalidPrivateNameError(globalObject);
    case Typed::RuntimeErrorKind::InvalidPrivateBrand:
        return createTypeError(globalObject, "Cannot access static private method or accessor"_s);
    case Typed::RuntimeErrorKind::PrivateFieldExists:
        return createRedefinedPrivateNameError(globalObject);
    case Typed::RuntimeErrorKind::PrivateAccessorWithoutGetter:
        return createTypeError(globalObject, "Trying to access an undefined private getter"_s);
    case Typed::RuntimeErrorKind::PrivateMethodWrite:
    case Typed::RuntimeErrorKind::PrivateAccessorWithoutSetter:
        return createTypeError(globalObject, "Trying to access an undefined private setter"_s);
    case Typed::RuntimeErrorKind::PrivateStaticTestOfNonObject:
        return createTypeError(globalObject, "Cannot access static private method or accessor of a non-Object"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewRuntimeError, JSCell*, (Instance* instance, uint32_t errorKind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRuntimeError);
    RELEASE_AND_RETURN(scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(errorKind), nullptr));
}

static Identifier identifierOf(VM& vm, StringID text)
{
    return Identifier::fromString(vm, ProgramStrings::atomOf(*Image::current(), text));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewRuntimeErrorWithName, JSCell*, (Instance* instance, uint32_t errorKind, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewRuntimeErrorWithName);
    Identifier identifier = identifierOf(vm, StringID(name));
    RELEASE_AND_RETURN(scope, createRuntimeError(globalObject, static_cast<Typed::RuntimeErrorKind>(errorKind), &identifier));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewSuppressedError, JSCell*, (Instance* instance, EncodedJSValue error, EncodedJSValue suppressed))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewSuppressedError);
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

static String textOrNull(uint32_t name)
{
    return name == ThrowSiteWord::none ? String() : ProgramStrings::text(*Image::current(), StringID(name));
}

static void throwKindMismatch(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t kinds, uint32_t name)
{
    throwTypeError(globalObject, scope, withAOTTypeCheckNote(makeString("expected "_s, describeKinds(*instance->state, kinds, name), ", got "_s, describe(value))));
}

static void throwNullish(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t name, Typed::NullishAccess access)
{
    if (access == Typed::NullishAccess::Iterate)
        throwTypeError(globalObject, scope, nullishIterationMessage(value));
    else
        throwTypeError(globalObject, scope, nullishMessage(value, textOrNull(name), access == Typed::NullishAccess::Write));
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
    uint32_t b;
    bool isFaultSite;
};

static DecodedThrowSite decodeThrowSite(const uint64_t* registers, const uint32_t* site, ThrowSiteSource source)
{
    std::array<uint32_t, FaultSite::maxThrowSiteWords> localWords { };
    const uint32_t* words = nullptr;
    switch (source) {
    case ThrowSiteSource::WordsAfterCall:
        words = site;
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
    uint32_t a = word.hasWideA ? *words++ : word.a;
    uint32_t b = word.hasB ? *words++ : ThrowSiteWord::none;
    return { word, a, b, source == ThrowSiteSource::FaultSite };
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
        value = JSValue::decode(bits);
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

static bool isReportedWhenThrown(ThrowKind kind)
{
    switch (kind) {
    case ThrowKind::TypeMismatch:
        return true;
    case ThrowKind::KindMismatch:
    case ThrowKind::NullishRead:
    case ThrowKind::NullishWrite:
    case ThrowKind::NullishIterate:
    case ThrowKind::UninitializedBinding:
    case ThrowKind::NotAFunction:
    case ThrowKind::ErrorWithMessage:
        return false;
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
    DecodedThrowSite decoded = decodeThrowSite(registers, site, static_cast<ThrowSiteSource>(source));
    ThrowKind kind = static_cast<ThrowKind>(decoded.word.kind);
    if (!isReportedWhenThrown(kind) && !needsExceptionForEveryThrow(*instance) && nearestHandlerIgnoresValue(vm, *instance, callFrame)) {
        count(*instance, RuntimeCounterID::ThrownErrorsNotCreated);
        instance->runtime.exceptions.thrownValue = JSValue::encode(jsUndefined());
        OPERATION_RETURN(scope);
    }
    auto value = [&] {
        return throwSiteValue(instance, registers, decoded);
    };
    switch (kind) {
    case ThrowKind::TypeMismatch:
        if (decoded.isFaultSite && static_cast<Typed::Representation>(decoded.word.representation) == Typed::Representation::JSValue) {
            CallSiteOverride castSiteOverride(*instance, callFrame->callerFrame());
            throwTypeMismatch(instance, globalObject, scope, value(), decoded.a, Typed::TypeSiteID(decoded.b));
            break;
        }
        throwTypeMismatch(instance, globalObject, scope, value(), decoded.a, Typed::TypeSiteID(decoded.b));
        break;
    case ThrowKind::KindMismatch:
        throwKindMismatch(instance, globalObject, scope, value(), decoded.a, decoded.b);
        break;
    case ThrowKind::NullishRead:
        throwNullish(globalObject, scope, value(), decoded.a, Typed::NullishAccess::Read);
        break;
    case ThrowKind::NullishWrite:
        throwNullish(globalObject, scope, value(), decoded.a, Typed::NullishAccess::Write);
        break;
    case ThrowKind::NullishIterate:
        throwNullish(globalObject, scope, value(), decoded.a, Typed::NullishAccess::Iterate);
        break;
    case ThrowKind::UninitializedBinding:
        throwUninitializedBinding(globalObject, scope, decoded.a);
        break;
    case ThrowKind::NotAFunction:
        if (decoded.a == ThrowSiteWord::none)
            throwNotAFunctionError(globalObject, scope, value());
        else
            throwNotAFunctionError(globalObject, scope, value(), identifierOf(vm, StringID(decoded.a)));
        break;
    case ThrowKind::ErrorWithMessage:
        throwErrorWithMessage(instance, globalObject, scope, decoded);
        break;
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
