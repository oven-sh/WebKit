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

#include "ArgList.h"
#include "CallFrame.h"
#include "JSCJSValue.h"
#include "JSCellButterfly.h"
#include "PyRealm.h"
#include "PythonCommonNames.h"
#include "PythonFunctionInfo.h"
#include "PythonOperators.h"
#include <wtf/HexNumber.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/MakeString.h>

namespace JSC {

class BytecodeIndex;
class CodeBlock;
class PyDict;
class PyFrame;
class SourceCode;

namespace Python {

// What the language does. Each of these is what an opcode or a built-in function comes down to, for any values at all. They
// raise Python exceptions, in the way that JavaScriptCore throws: the caller checks its scope.

// The names of the keywords in a call, which are strings. It belongs to no global object, so it can be a constant of compiled code.
using KeywordNames = JSCellButterfly;
inline bool isKeywordNames(JSValue value) { return value.isCell() && value.asCell()->type() == JSCellButterflyType; }

// ---- Kinds of value

inline bool isNone(JSValue value) { return value.isUndefinedOrNull(); }
// int, and not bool.
bool isInt(JSValue);
bool isFloat(JSValue);
inline bool isStr(JSValue value) { return value.isString(); }
bool isList(JSValue);

PyType* typeOf(JSGlobalObject*, JSValue);
bool isInstance(JSGlobalObject*, JSValue, PyType*); // By its type alone: not what __instancecheck__ or __class__ say.
String typeName(JSGlobalObject*, JSValue);

// ---- Exceptions

JSObject* createException(JSGlobalObject*, PyType*, const String& message);
JSObject* createNotCallableError(JSGlobalObject*, JSValue callee);
// NameError: name 'x' is not defined
JSValue raiseNameError(JSGlobalObject*, ThrowScope&, const String& name);
// Makes what is being handled now the __context__ of an exception that is about to be raised.
void setContext(JSGlobalObject*, JSObject* exception);
String nameOfFunction(JSGlobalObject*, JSFunction*, bool qualified);
JSObject* createException(JSGlobalObject*, PyType*, JSValue argument);
// These throw, and return an empty value for the caller to return.
JSValue raise(JSGlobalObject*, ThrowScope&, BuiltinType, const String& message);
JSValue raise(JSGlobalObject*, ThrowScope&, BuiltinType, JSValue argument);
inline JSValue raiseTypeError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, BuiltinType::TypeError, message); }
inline JSValue raiseValueError(JSGlobalObject* globalObject, ThrowScope& scope, const String& message) { return raise(globalObject, scope, BuiltinType::ValueError, message); }
void throwUnboundVariable(JSGlobalObject*, CodeBlock*, JSString* name);
// Whether what has been thrown is an instance of the type. If so it is caught, and no longer thrown.
bool catchException(JSGlobalObject*, BuiltinType);
// The Python exception for whatever was thrown, which may have been thrown by JavaScript.
JSValue exceptionValue(JSGlobalObject*, JSValue thrown);

// ---- Attributes

JS_EXPORT_PRIVATE JSValue getAttribute(JSGlobalObject*, JSValue, PropertyName);
// Empty, and nothing raised, if there is no such attribute.
JSValue getAttributeIfPresent(JSGlobalObject*, JSValue, PropertyName);
void setAttribute(JSGlobalObject*, JSValue, PropertyName, JSValue);
void deleteAttribute(JSGlobalObject*, JSValue, PropertyName);
// What object and type do, whatever the class says: object.__getattribute__, which gives an empty value if there is none, and
// object.__setattr__, which deletes if the value is empty.
JSValue genericGetAttribute(JSGlobalObject*, JSValue, PropertyName);
void genericSetAttribute(JSGlobalObject*, JSValue, PropertyName, JSValue);
JSValue loadMethod(JSGlobalObject*, JSValue base, PropertyName, JSValue& self);
// A special method, which is looked for in the type and not in the instance. Empty if the type has none. `self` as for loadMethod.
JSValue lookupSpecial(JSGlobalObject*, JSValue, PropertyName, JSValue& self);
// What `descriptor`, found in a class, gives when got from `instance` (empty for the class itself) of `type`.
JSValue bindDescriptor(JSGlobalObject*, JSValue descriptor, JSValue instance, PyType*);
// descriptor.__set__(instance, newValue), or __delete__ if `newValue` is empty. `attribute` is what to call it if it cannot be done.
void setDescriptor(JSGlobalObject*, JSValue descriptor, JSValue instance, StringView attribute, JSValue newValue);

// What is in a slot: one of __slots__, or what in CPython is a field of the C struct of a built-in type. It is a property of the instance under
// a name that no attribute can have, and that __dict__ does not show. Empty if nothing is in it.
JSObject* createMemberDescriptor(JSGlobalObject*, PyType* owner, JSString* name, const Identifier* storage = nullptr, JSValue initialValue = JSValue());

// The property that JavaScript sees on something of Python's, other than what it has of its own: getattr(), and a few names that mean to
// JavaScript what others mean to Python, like toString and Symbol.iterator. Empty if there is none.
JSValue getPropertyForJavaScript(JSGlobalObject*, JSValue receiver, PropertyName);

// ---- Calls

JSValue call(JSGlobalObject*, JSValue callable, const ArgList&);
JSValue call(JSGlobalObject*, JSValue callable);
JSValue call(JSGlobalObject*, JSValue callable, JSValue);
JSValue call(JSGlobalObject*, JSValue callable, JSValue, JSValue);
JSValue call(JSGlobalObject*, JSValue callable, JSValue, JSValue, JSValue);
// The values of the keywords are the last of the arguments. `keywordNames` may be null.
// `thisValue` is for a function of JavaScript's: what it was got from, in base.function(...). Python's own make nothing of it.
JSValue callWithKeywords(JSGlobalObject*, JSValue callable, const ArgList&, KeywordNames* keywordNames, JSValue thisValue = jsUndefined());
// The value of each parameter of a function written in Python. False if it raised.
bool bindArguments(JSGlobalObject*, JSFunction*, const FunctionInfo&, const ArgList&, KeywordNames*, MarkedArgumentBuffer& bound);
// What loadMethod or lookupSpecial gave.
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self, const ArgList&);
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self);
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self, JSValue);
JSValue callMethod(JSGlobalObject*, JSValue function, JSValue self, JSValue, JSValue);
bool isCallable(JSGlobalObject*, JSValue);
// type(...): __new__, and then __init__. The first argument is not the type.
JSValue instantiate(JSGlobalObject*, PyType*, const ArgList&, KeywordNames* keywordNames);

// ---- Classes

// What a class statement does. `body` is a function that fills in the namespace it is given.
JSValue buildClass(JSGlobalObject*, JSValue body, JSString* name, PyTuple* bases, PyDict* keywords);
// type.__new__(metatype, name, bases, namespace, **keywords)
JSValue newType(JSGlobalObject*, PyType* metatype, JSString* name, PyTuple* bases, PyDict* namespaceDict, PyDict* keywords);
// isinstance() and issubclass(), which a class can have its own idea of.
bool isInstanceOf(JSGlobalObject*, JSValue, JSValue classInfo);
bool isSubclassOf(JSGlobalObject*, JSValue, JSValue classInfo);
// An attribute got through super().
JSValue getSuperAttribute(JSGlobalObject*, JSValue superObject, PropertyName);

// ---- Modules

// import name, as the statement does it. `fromList` is None or a tuple of names.
JS_EXPORT_PRIVATE JSValue importModule(JSGlobalObject*, JSObject* globals, const String& name, JSValue fromList, unsigned level, bool wantsLeaf);
void registerModule(JSGlobalObject*, const String& name, JSValue module);
// The module that a file is, which is run if it has not been. For a module that is asked for by where it is: JavaScript's `import`.
JSValue importModuleFromSource(JSGlobalObject*, const SourceCode&);
// The same, as what a module of JavaScript's can import: each of its global variables by name, and itself as the default.
void exportModule(JSGlobalObject*, const SourceCode&, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues);
// OSError(errno, strerror(errno)[, filename]), or the class derived from it that is for that error.
JS_EXPORT_PRIVATE JSValue raiseOSError(JSGlobalObject*, ThrowScope&, int errorNumber, JSValue filename = JSValue());
// What is given for the name of a file, as the system wants it: a str, bytes, or what has __fspath__. Nothing if it raised.
JS_EXPORT_PRIVATE std::optional<CString> toFileSystemPath(JSGlobalObject*, JSValue);
// An attribute of the module sys, as it is now. Empty if it has been deleted.
JSValue sysAttribute(JSGlobalObject*, ASCIILiteral name);

// ---- Looking into code that is running

// The namespace of the module that the code of a frame is in. Null if it is not Python's.
JSObject* globalsOfFrame(JSGlobalObject*, CallFrame*);
// What locals() gives there: the namespace itself for a module or the body of a class, and for a function a new dict of its variables.
JSValue localsOfFrame(JSGlobalObject*, CallFrame*);
JSValue localsOfFrame(JSGlobalObject*, PyFrame*);
// The frame of the Python code that called a function written in C++.
CallFrame* callerOf(CallFrame*);

// Notes that an exception has come to a frame of Python code, by being raised in it or by coming out of what it called. The unwinder calls it.
void addTracebackEntry(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex);
// A frame of Python code is about to be no more. If it has a frame object, that outlives it. The unwinder calls it, and op_py_ret.
void leaveFrame(VM&, CallFrame*, BytecodeIndex);
// What Python prints when an exception gets away: the traceback, and those of what led to it.
String formatException(JSGlobalObject*, JSValue exception);

// ---- Operators

JSValue binaryOperation(JSGlobalObject*, BinaryOperator, bool inPlace, JSValue, JSValue);
JSValue unaryOperation(JSGlobalObject*, UnaryOperator, JSValue);
JSValue compare(JSGlobalObject*, ComparisonOperator, JSValue, JSValue);
bool isTrue(JSGlobalObject*, JSValue);
bool isEqual(JSGlobalObject*, JSValue, JSValue); // x is y or x == y, which is what containers ask.
bool isIdentical(JSValue, JSValue);
bool contains(JSGlobalObject*, JSValue container, JSValue);
JSValue power(JSGlobalObject*, JSValue, JSValue, JSValue modulus);
JSValue divmod(JSGlobalObject*, JSValue, JSValue);

// ---- Subscripts

JSValue getItem(JSGlobalObject*, JSValue, JSValue key);
void setItem(JSGlobalObject*, JSValue, JSValue key, JSValue);
void deleteItem(JSGlobalObject*, JSValue, JSValue key);
// len(). Negative if it raised.
int64_t length(JSGlobalObject*, JSValue);

// ---- Iteration

JSValue getIterator(JSGlobalObject*, JSValue);
JSValue iteratorNext(JSGlobalObject*, JSValue iterator); // Empty when there is no more, with nothing raised.
// Calls the function with each. It returns false to stop. Returns false if something was raised.
bool forEach(JSGlobalObject*, JSValue iterable, const ScopedLambda<bool(JSValue)>&);
bool collect(JSGlobalObject*, JSValue iterable, MarkedArgumentBuffer&);
void unpackSequence(JSGlobalObject*, JSValue iterable, unsigned count, int starIndex, Register* first);
JSValue newTuple(JSGlobalObject*, Register* first, unsigned count);

// ---- What every object can be asked

String repr(JSGlobalObject*, JSValue);
String str(JSGlobalObject*, JSValue);
JSValue format(JSGlobalObject*, JSValue, const String& specification);
// Never -1, unless it raised.
int64_t hash(JSGlobalObject*, JSValue);
int64_t hashOfString(const String&);

// ---- Numbers

// An int, from what may not fit an int32.
JSValue intFromInt64(JSGlobalObject*, int64_t);
JSValue intFromDouble(JSGlobalObject*, double); // Truncated.
JSValue floatFromDouble(double);
// What __index__ gives, as something to index with: clamped to the range of an int64 if `clamp`, or else it raises IndexError.
std::optional<int64_t> toIndex(JSGlobalObject*, JSValue, bool clamp = false);
// The value of an int, a bool or a float, or of what has __float__ or __index__.
std::optional<double> toDouble(JSGlobalObject*, JSValue);
String reprOfDouble(double);

// ---- The arguments of a function written in C++

class NativeArguments {
public:
    explicit NativeArguments(CallFrame* callFrame)
        : m_callFrame(callFrame)
        , m_keywordNames(isKeywordNames(callFrame->thisValue()) ? uncheckedDowncast<KeywordNames>(callFrame->thisValue().asCell()) : nullptr)
        , m_positionalCount(callFrame->argumentCount() - (m_keywordNames ? m_keywordNames->length() : 0))
    {
    }

    unsigned size() const { return m_positionalCount; }
    JSValue at(unsigned index) const { return index < m_positionalCount ? m_callFrame->uncheckedArgument(index) : JSValue(); }
    JSValue operator[](unsigned index) const { return m_callFrame->uncheckedArgument(index); }

    unsigned keywordCount() const { return m_keywordNames ? m_keywordNames->length() : 0; }
    JSString* keywordName(unsigned index) const { return asString(m_keywordNames->get(index)); }
    JSValue keywordValue(unsigned index) const { return m_callFrame->uncheckedArgument(m_positionalCount + index); }
    // Empty if it was not given.
    JSValue keyword(JSGlobalObject*, ASCIILiteral name) const;
    KeywordNames* keywordNames() const { return m_keywordNames; }

    // Raises TypeError, in the words CPython uses, and returns false, if there are not between these many or there are keywords.
    bool check(JSGlobalObject*, ThrowScope&, ASCIILiteral functionName, unsigned minimum, unsigned maximum) const;
    bool checkNoKeywords(JSGlobalObject*, ThrowScope&, ASCIILiteral functionName) const;

    CallFrame* callFrame() const { return m_callFrame; }

private:
    CallFrame* m_callFrame;
    KeywordNames* m_keywordNames;
    unsigned m_positionalCount;
};

} } // namespace JSC::Python
