/*
 * Copyright (C) 2008-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ConstructorKind.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "Exception.h"
#include "JSObject.h"
#include "ThrowScope.h"
#include <wtf/text/MakeString.h>

namespace JSC {

typedef JSObject* (*ErrorFactory)(JSGlobalObject*, const String&, ErrorInstance::SourceAppender);

inline constexpr ASCIILiteral SuperNotCalledError { "'super()' must be called in derived constructor before accessing |this| or returning non-object."_s };
inline constexpr ASCIILiteral SuperCalledTwiceError { "'super()' can't be called more than once in a constructor."_s };
inline constexpr ASCIILiteral DeleteSuperPropertyError { "Cannot delete a super property"_s };
inline constexpr ASCIILiteral DestructureNullishError { "Cannot destructure null or undefined value"_s };
inline constexpr ASCIILiteral DerivedConstructorResultError { "Cannot return a non-object type in the constructor of a derived class."_s };
inline constexpr ASCIILiteral StaticFieldNamedPrototypeError { "Cannot declare a static field named 'prototype'"_s };
inline constexpr ASCIILiteral IteratorResultNotObjectError { "Iterator result interface is not an object."_s };
inline constexpr ASCIILiteral YieldStarIteratorWithoutThrowError { "The iterator, to which yield* delegated iteration, does not have a 'throw' method."_s };
inline constexpr ASCIILiteral StaticPrivateBrandError { "Cannot access static private method or accessor"_s };
inline constexpr ASCIILiteral StaticPrivateBrandOfNonObjectError { "Cannot access static private method or accessor of a non-Object"_s };
inline constexpr ASCIILiteral UndefinedPrivateGetterError { "Trying to access an undefined private getter"_s };
inline constexpr ASCIILiteral UndefinedPrivateSetterError { "Trying to access an undefined private setter"_s };
inline constexpr ASCIILiteral InstanceofRightHandSideNotObjectError { "Right hand side of instanceof is not an object"_s };
inline constexpr ASCIILiteral SuperclassNotConstructorError { "The superclass is not a constructor."_s };
inline constexpr ASCIILiteral ConstructorCalledWithoutNewError { "Cannot call a constructor without |new|"_s };
inline constexpr ASCIILiteral ClassConstructorCalledWithoutNewError { "Cannot call a class constructor without |new|"_s };
inline constexpr ASCIILiteral GetterNotFunctionError { "Getter must be a function."_s };
inline constexpr ASCIILiteral SetterNotFunctionError { "Setter must be a function."_s };
inline constexpr ASCIILiteral DefinePropertyOnNonObjectError { "Properties can only be defined on Objects."_s };
inline constexpr ASCIILiteral ReplaceAllNonGlobalRegExpError { "String.prototype.replaceAll argument must not be a non-global regular expression"_s };
inline constexpr ASCIILiteral ModuleGraphTooDeepError { "Maximum call stack size exceeded"_s };
inline constexpr ASCIILiteral DeferredModuleNotReadyError { "Unable to synchronously evaluate deferred module"_s };
inline constexpr ASCIILiteral NamespaceBindingToAccessorError { "Cannot change module namespace object's binding to accessor"_s };
inline constexpr ASCIILiteral NamespaceBindingToNonWritableError { "Cannot change module namespace object's binding to non-writable attribute"_s };
inline constexpr ASCIILiteral NamespaceBindingToNonEnumerableError { "Cannot replace module namespace object's binding with non-enumerable attribute"_s };
inline constexpr ASCIILiteral NamespaceBindingToConfigurableError { "Cannot replace module namespace object's binding with configurable attribute"_s };
inline constexpr ASCIILiteral NamespaceBindingValueError { "Cannot replace module namespace object's binding's value"_s };

String defaultSourceAppender(const String&, StringView, RuntimeType, ErrorInstance::SourceTextWhereErrorOccurred);
String notAFunctionSourceAppender(const String&, StringView, RuntimeType, ErrorInstance::SourceTextWhereErrorOccurred);

String constructErrorMessage(JSGlobalObject*, JSValue, const String&);
JS_EXPORT_PRIVATE JSObject* createError(JSGlobalObject*, JSValue, const String&, ErrorInstance::SourceAppender);
JS_EXPORT_PRIVATE JSObject* createStackOverflowError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createUndefinedVariableError(JSGlobalObject*, const Identifier&);
JS_EXPORT_PRIVATE JSObject* createTDZError(JSGlobalObject*, StringView);
JS_EXPORT_PRIVATE JSObject* createTDZError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createTDZError(JSGlobalObject*, const Identifier&);
JS_EXPORT_PRIVATE JSObject* createNotAnObjectError(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createInvalidFunctionApplyParameterError(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createInvalidInParameterError(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createInvalidInstanceofParameterErrorNotFunction(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createInvalidInstanceofParameterErrorHasInstanceValueNotFunction(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createNotAConstructorError(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createNotAFunctionError(JSGlobalObject*, JSValue);
JS_EXPORT_PRIVATE JSObject* createInvalidPrototypeError(JSGlobalObject*, JSValue);
JSObject* createErrorForDuplicateGlobalVariableDeclaration(JSGlobalObject*, UniquedStringImpl*);
JSObject* createErrorForInvalidGlobalFunctionDeclaration(JSGlobalObject*, const Identifier&);
JSObject* createErrorForInvalidGlobalVarDeclaration(JSGlobalObject*, const Identifier&);
JS_EXPORT_PRIVATE JSObject* createInvalidPrivateNameError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createRedefinedPrivateNameError(JSGlobalObject*);
String destructureNullishErrorMessage(const Identifier& propertyName);
String derivedConstructorResultErrorMessage(const String& className);
String constructorCalledWithoutNewErrorMessage(ConstructorKind, const String& constructorName);
JS_EXPORT_PRIVATE JSObject* createSuperNotCalledError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createSuperCalledTwiceError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createDeleteSuperPropertyError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createDestructureNullishError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createDestructureNullishError(JSGlobalObject*, const Identifier& propertyName);
JS_EXPORT_PRIVATE JSObject* createDerivedConstructorResultError(JSGlobalObject*, const String& className);
JS_EXPORT_PRIVATE JSObject* createStaticFieldNamedPrototypeError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createIteratorResultNotObjectError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createYieldStarIteratorWithoutThrowError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createStaticPrivateBrandError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createStaticPrivateBrandOfNonObjectError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createUndefinedPrivateGetterError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createUndefinedPrivateSetterError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createInstanceofRightHandSideNotObjectError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createSuperclassNotConstructorError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createConstructorCalledWithoutNewError(JSGlobalObject*, ConstructorKind, const String& constructorName);
JS_EXPORT_PRIVATE JSObject* createGetterNotFunctionError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createSetterNotFunctionError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createDefinePropertyOnNonObjectError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createReplaceAllNonGlobalRegExpError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createGeneratorIsExecutingError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createNotAGeneratorError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createSpreadOfNullishError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createSpreadIteratorNotFunctionError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createSpreadResultNotObjectError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createModuleGraphTooDeepError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createDeferredModuleNotReadyError(JSGlobalObject*);
JS_EXPORT_PRIVATE JSObject* createExportNotFoundError(JSGlobalObject*, const String& exportName, const String& moduleKey);
JS_EXPORT_PRIVATE JSObject* createAmbiguousExportError(JSGlobalObject*, const String& exportName, const String& moduleKey);
JS_EXPORT_PRIVATE JSObject* createMissingDefaultExportError(JSGlobalObject*, const String& moduleKey);
String errorDescriptionForValue(JSGlobalObject*, JSValue);

JS_EXPORT_PRIVATE Exception* throwOutOfMemoryError(JSGlobalObject*, ThrowScope&);
JS_EXPORT_PRIVATE Exception* throwOutOfMemoryError(JSGlobalObject*, ThrowScope&, const String&);
JS_EXPORT_PRIVATE Exception* throwStackOverflowError(JSGlobalObject*, ThrowScope&);
#if USE(BUN_JSC_ADDITIONS)
Exception* throwNonStaticCodeError(JSGlobalObject*, ThrowScope&);
#endif

#if ASSERT_ENABLED

#define DEFER_TERMINATION_AND_ASSERT(vm, assertion, ...) do { \
        JSC::DeferTerminationForAWhile deferScope(vm); \
        ASSERT(assertion, __VA_ARGS__); \
    } while (false)

#define DEFER_TERMINATION_AND_ASSERT_WITH_MESSAGE(vm, assertion, ...) do { \
        JSC::DeferTerminationForAWhile deferScope(vm); \
        ASSERT_WITH_MESSAGE(assertion, __VA_ARGS__); \
    } while (false)

#else

#define DEFER_TERMINATION_AND_ASSERT(vm, assertion, ...) UNUSED_PARAM(vm)
#define DEFER_TERMINATION_AND_ASSERT_WITH_MESSAGE(vm, assertion, ...) UNUSED_PARAM(vm)

#endif // ASSERT_ENABLED

#define DEFER_TERMINATION_AND_RELEASE_ASSERT(vm, assertion, ...) do { \
        JSC::DeferTerminationForAWhile deferScope(vm); \
        RELEASE_ASSERT(assertion, __VA_ARGS__); \
    } while (false)

#define DEFER_TERMINATION_AND_RELEASE_ASSERT_WITH_MESSAGE(vm, assertion, ...) do { \
        JSC::DeferTerminationForAWhile deferScope(vm); \
        RELEASE_ASSERT_WITH_MESSAGE(assertion, __VA_ARGS__); \
    } while (false)

// Defined here rather than in JSCJSValue.h because they need throwException/createRangeError/createNotAnObjectError.
// https://tc39.es/ecma262/#sec-toindex
inline uint64_t JSValue::toIndex(JSGlobalObject* globalObject, ASCIILiteral errorName) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isInt32()) {
        int32_t integer = asInt32();
        if (integer < 0) [[unlikely]] {
            throwException(globalObject, scope, createRangeError(globalObject, makeString(errorName, " cannot be negative"_s)));
            return 0;
        }
        return integer;
    }

    double d = toIntegerOrInfinity(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    if (d < 0) [[unlikely]] {
        throwException(globalObject, scope, createRangeError(globalObject, makeString(errorName, " cannot be negative"_s)));
        return 0;
    }

    if (d > maxSafeInteger()) [[unlikely]] {
        throwException(globalObject, scope, createRangeError(globalObject, makeString(errorName, " larger than (2 ** 53) - 1"_s)));
        return 0;
    }

    RELEASE_AND_RETURN(scope, d);
}

ALWAYS_INLINE bool JSValue::requireObjectCoercible(JSGlobalObject* globalObject) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!isUndefinedOrNull())
        return true;
    throwException(globalObject, scope, createNotAnObjectError(globalObject, *this));
    return false;
}

} // namespace JSC
