/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonStrings.h"
#include <wtf/HexNumber.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace Python {

// For the files that define the built-in types and functions.

struct MethodDefinition {
    ASCIILiteral name;
    NativeFunction function;
    PyNativeFunction::Kind kind { PyNativeFunction::Kind::Method };
    unsigned data { 0 };
};

// Up to three small values, for PyNativeFunction::data().
template<typename A>
constexpr unsigned pack(A a) { return static_cast<unsigned>(a) & 0xFF; }
template<typename A, typename B>
constexpr unsigned pack(A a, B b) { return pack(a) | pack(b) << 8; }
template<typename A, typename B, typename C>
constexpr unsigned pack(A a, B b, C c) { return pack(a, b) | pack(c) << 16; }

template<typename T>
inline T unpack(CallFrame* callFrame, unsigned position)
{
    return static_cast<T>((uncheckedDowncast<PyNativeFunction>(callFrame->jsCallee())->data() >> (position * 8)) & 0xFF);
}

void addMethods(JSGlobalObject*, PyType*, std::initializer_list<MethodDefinition>);
void addGetSet(JSGlobalObject*, PyType*, ASCIILiteral name, PyGetSetDescriptor::Getter, PyGetSetDescriptor::Setter = nullptr);
void addMember(JSGlobalObject*, PyType*, ASCIILiteral name, PyGetSetDescriptor::Getter, PyGetSetDescriptor::Setter = nullptr);
PyNativeFunction* addFunction(JSGlobalObject*, JSObject* namespaceObject, ASCIILiteral name, NativeFunction, unsigned data = 0);

// Whether a function written in C++ has been given a first argument that it can work on: for a method, an instance of the class that it is a
// method of, and for __new__, a class derived from that one. If not, TypeError has been raised. What follows can then take it for what it is.
bool checkFirstArgument(JSGlobalObject*, CallFrame*);

#define PYTHON_NATIVE_WITH_LINKAGE(linkage, name) \
    static EncodedJSValue name##Checked(JSGlobalObject*, CallFrame*); \
    linkage JSC_DECLARE_HOST_FUNCTION(name); \
    JSC_DEFINE_HOST_FUNCTION(name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        if (!checkFirstArgument(globalObject, callFrame)) [[unlikely]] \
            return { }; \
        return name##Checked(globalObject, callFrame); \
    } \
    static EncodedJSValue name##Checked(JSGlobalObject* globalObject, CallFrame* callFrame)

#define PYTHON_NATIVE(name) PYTHON_NATIVE_WITH_LINKAGE(static, name)
// One that other files use.
#define PYTHON_SHARED_NATIVE(name) PYTHON_NATIVE_WITH_LINKAGE(, name)

#define NATIVE_PROLOGUE() \
    VM& vm = globalObject->vm(); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    [[maybe_unused]] PyRealm* realm = globalObject->pyRealm(); \
    [[maybe_unused]] auto& names = vm.pythonNames(); \
    NativeArguments args(callFrame);

#define RETURN_NONE() return JSValue::encode(jsUndefined())
#define RETURN_NOT_IMPLEMENTED() return JSValue::encode(realm->notImplemented())

// Each sets up some of the built-in types.
void initializeObjectAndType(JSGlobalObject*);
void initializeNumberTypes(JSGlobalObject*);
void initializeComplexType(JSGlobalObject*);
void initializeFunctionTypes(JSGlobalObject*);
void initializeCodeTypes(JSGlobalObject*, JSObject* builtinsNamespace);
void initializeAsyncTypes(JSGlobalObject*, JSObject* builtinsNamespace);
void initializeTracebackTypes(JSGlobalObject*);
void addFrameFunctions(JSGlobalObject*, JSObject* sysNamespace);
void initializeStrType(JSGlobalObject*);
void initializeContainerTypes(JSGlobalObject*);
void initializeIteratorTypes(JSGlobalObject*);
void initializeExceptionTypes(JSGlobalObject*);
void initializeBuiltinFunctions(JSGlobalObject*, JSObject* namespaceObject);
// Last of all, since it runs Python: what is written in it.
void initializeLibrary(JSGlobalObject*);
JSObject* createJavaScriptFunctions(VM&, JSGlobalObject*);
void initializeJavaScriptTypes(JSGlobalObject*);

// What is in other files, and shared among these.
String builtinRepr(JSGlobalObject*, JSValue);
JSValue builtinFormat(JSGlobalObject*, JSValue, const String& specification);
String strOfException(JSGlobalObject*, JSValue);
String qualifiedNameOfType(JSGlobalObject*, PyType*);
String nameOfFunction(JSGlobalObject*, JSFunction*, bool qualified);
std::optional<bool> builtinContains(JSGlobalObject*, JSValue container, JSValue);
int64_t builtinLength(JSGlobalObject*, JSValue); // -1 if it has none.

// Special methods that do the same for every built-in type that has them: they look at what kind of cell they are given.
JSC_DECLARE_HOST_FUNCTION(nativeRepr);
JSC_DECLARE_HOST_FUNCTION(nativeHash);
JSC_DECLARE_HOST_FUNCTION(nativeLen);
JSC_DECLARE_HOST_FUNCTION(nativeGetItem);
JSC_DECLARE_HOST_FUNCTION(nativeSetItem);
JSC_DECLARE_HOST_FUNCTION(nativeDelItem);
JSC_DECLARE_HOST_FUNCTION(nativeContains);
JSC_DECLARE_HOST_FUNCTION(nativeIter);
JSC_DECLARE_HOST_FUNCTION(nativeNext);
JSC_DECLARE_HOST_FUNCTION(nativeSelf);
void addComparisons(JSGlobalObject*, PyType*, bool ordering);
// __add__ and __radd__ and so on for these operators, by builtinBinaryOperation().
void addBinaryOperators(JSGlobalObject*, PyType*, std::initializer_list<BinaryOperator>, bool reflected, bool inPlace);

// An instance of `type`, which is `builtin` or derived from it, holding a value that is not a cell.
JSValue boxIfDerived(JSGlobalObject*, PyType* type, PyType* builtin, JSValue);

} } // namespace JSC::Python
