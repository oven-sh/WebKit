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
class PyNativeObject;
class PyRange;
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
// A list, or an instance of a class derived from list: an Array, or what JavaScriptCore calls a derived array. isJSArray() is only the first.
inline bool isListCell(JSCell* cell) { return cell->type() == ArrayType || cell->type() == DerivedArrayType; }
bool isList(JSValue);

PyType* typeOf(JSGlobalObject*, JSValue);
bool isInstance(JSGlobalObject*, JSValue, PyType*); // By its type alone: not what __instancecheck__ or __class__ say.
bool isExactly(JSGlobalObject*, JSValue, PyType*); // And not of a class derived from it.
bool isExactly(JSGlobalObject*, JSValue, BuiltinType);
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
// _PyErr_FormatNote(): adds a note to what has been thrown, which goes on being thrown. It is what exception.add_note() does.
void addNoteToRaised(JSGlobalObject*, const String& note);
// The same, where working out what to say may run something. It is worked out with nothing thrown.
void addNoteToRaised(JSGlobalObject*, const ScopedLambda<String()>& note);
// The Python exception for whatever was thrown, which may have been thrown by JavaScript.
JSValue exceptionValue(JSGlobalObject*, JSValue thrown);

// ---- Attributes

JS_EXPORT_PRIVATE JSValue getAttribute(JSGlobalObject*, JSValue, PropertyName);
// Empty, and nothing raised, if there is no such attribute.
JSValue getAttributeIfPresent(JSGlobalObject*, JSValue, PropertyName);
JSValue getModuleAttribute(JSGlobalObject*, JSValue module, PropertyName); // module.__getattribute__()
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
JSObject* createMemberDescriptor(JSGlobalObject*, PyType* owner, JSString* name, const Identifier* storage, JSValue initialValue = JSValue());

// The property that JavaScript sees on something of Python's, other than what it has of its own: getattr(), and a few names that mean to
// JavaScript what others mean to Python, like toString and Symbol.iterator. Empty if there is none.
JSValue getPropertyForJavaScript(JSGlobalObject*, JSValue receiver, PropertyName, PyType* from);
// Where an object's own attributes are, if it can have any: they are the properties of this. Null if it cannot.
JSObject* attributeStorage(JSGlobalObject*, JSValue, PyType*);
// An attribute that something has of its own is a property of it that is enumerable. What is not enumerable is JavaScript's business: the name and
// length of a function, the stack of an Error. Python does not see it. Empty if there is no such attribute.
//
// A name that JavaScript would take for an index, as "0", cannot be that of a property like the rest: to the engine that is an element of an array. It
// can only come from getattr() and the like or by way of a __dict__, not being something that can be written after a dot. An attribute of such a name is
// kept where a key of a __dict__ that is not a string is: in the dict, which is made if there was none.
inline bool isIndexLike(PropertyName name)
{
    UniquedStringImpl* uid = name.uid();
    return uid && uid->length() && isASCIIDigit((*uid)[0]) && !uid->isSymbol() && parseIndex(name);
}
JSValue getIndexLikeAttribute(VM&, JSObject*, PropertyName);
void putIndexLikeAttribute(VM&, JSObject*, PropertyName, JSValue);

inline JSValue getStoredAttribute(VM& vm, JSObject* object, PropertyName name)
{
    if (isIndexLike(name)) [[unlikely]]
        return getIndexLikeAttribute(vm, object, name);
    unsigned attributes;
    JSValue value = object->getDirect(vm, name, attributes);
    return value && !(attributes & PropertyAttribute::DontEnum) ? value : JSValue();
}
inline void putStoredAttribute(VM& vm, JSObject* object, PropertyName name, JSValue value)
{
    if (isIndexLike(name)) [[unlikely]]
        return putIndexLikeAttribute(vm, object, name, value);
    object->putDirect(vm, name, value, static_cast<unsigned>(PropertyAttribute::None));
}
// Takes away a property that holds an attribute. This is what delattr() comes down to in the end, so it is not to go by way of what JavaScript's
// `delete` does to something of Python's, which is delattr().
bool deleteStoredAttribute(JSGlobalObject*, JSObject*, PropertyName);
// Whether its code is Python.
bool isPythonFunction(JSFunction*);
// isJavaScriptClass(): whether it is a class that JavaScript made, which is to say a constructor: what `new` can be used with.
// classFor(): what there is to know about one. Both are declared in PyType.h.
// The class of what has this for its prototype: that of the constructor that it is the prototype of, or failing that of the next one along that is
// some constructor's.
PyType* classForPrototype(JSGlobalObject*, JSValue prototype);
// The class of a class that JavaScript made. This does not take finding out all about it.
PyType* metatypeOfJavaScriptClass(JSGlobalObject*, JSObject* constructor);
// An attribute of a class. Empty if it has none. A class of JavaScript's is a function besides, and has what a function has.
enum class ClassIsFunctionToo : bool { No, Yes };
// If `from` is given, what the class itself defines is looked for beginning with that one in the order of resolution.
JSValue getTypeAttribute(JSGlobalObject*, PyType*, PropertyName, ClassIsFunctionToo = ClassIsFunctionToo::Yes, PyType* from = nullptr);
// For a class whose instances are the first to have a __dict__, or to be weakly referred to: the descriptors for those.
void addInstanceDescriptors(JSGlobalObject*, PyType*, bool addsDict, bool addsWeakReferences);
// The class that a value is, whichever language made it. Null if it is not one.
PyType* tryClass(JSGlobalObject*, JSValue);

// Whether an object that JavaScript is working on is Python's: a class, or an instance of the class that is its prototype. Something of JavaScript's can
// have a class for a prototype, or further up, if it was made to.
bool isPythonObject(JSGlobalObject*, JSValue);
// Getting a property of something of Python's, or asking whether it has one, as far as that is up to what it has of its own. `ordinary` is what
// finds that.
bool getOwnPropertySlotFromJavaScript(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&, bool (*ordinary)(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&));
// receiver.name = value, delete receiver.name and Object.defineProperty(receiver, name, descriptor) in JavaScript, of something of Python's.
bool setPropertyFromJavaScript(JSGlobalObject*, JSValue receiver, PropertyName, JSValue, PutPropertySlot&);
bool deletePropertyFromJavaScript(JSGlobalObject*, JSValue receiver, PropertyName);
bool definePropertyFromJavaScript(JSGlobalObject*, JSObject* receiver, PropertyName, const PropertyDescriptor&, bool shouldThrow);
// Whether a class has something to say about an attribute of its instances that comes before what the instance itself has: see
// PyType::instanceAccessIsAsFound().
enum class AttributeAccess : uint8_t { Get, Set };
bool classComesBeforeInstance(JSGlobalObject*, PyType*, PropertyName, AttributeAccess);
// Whether it has __set__ or __delete__.
bool isDataDescriptor(JSGlobalObject*, JSValue);

// Whether it is Python that made the call that a frame is for: code in Python, or something of Python's that is written in C++ and calls what it is given.
JS_EXPORT_PRIVATE bool isCalledByPython(VM&, CallFrame*);

// ---- Global variables

// Where a global variable was found, for whoever wants to remember: it is there for as long as the objects have these structures. If it was in
// `globals`, there is no `builtinsStructure`. If it is not something that can be relied on, there is no `globalsStructure` either.
struct GlobalLocation {
    Structure* globalsStructure { nullptr };
    Structure* builtinsStructure { nullptr };
    PropertyOffset offset { 0 };
};
// The value of a global variable: what `globals` has by that name, or failing that `builtins`. Otherwise it raises NameError.
JSValue loadGlobal(JSGlobalObject*, JSObject* globals, JSObject* builtins, PropertyName, GlobalLocation&);

// ---- Calls

JSValue call(JSGlobalObject*, JSValue callable, const ArgList&);
JSValue call(JSGlobalObject*, JSValue callable);
JSValue call(JSGlobalObject*, JSValue callable, JSValue);
JSValue call(JSGlobalObject*, JSValue callable, JSValue, JSValue);
JSValue call(JSGlobalObject*, JSValue callable, JSValue, JSValue, JSValue);
// The values of the keywords are the last of the arguments. `keywordNames` may be null.
// `thisValue` is for a function of JavaScript's: what it was got from, in base.function(...). Python's own make nothing of it.
JSValue callWithKeywords(JSGlobalObject*, JSValue callable, const ArgList&, KeywordNames* keywordNames, JSValue thisValue = jsUndefined());
// callable(*arguments, **keywords). The values of the keywords are added to the arguments. `keywords` may be null.
JSValue callWithKeywordDict(JSGlobalObject*, JSValue callable, MarkedArgumentBuffer& arguments, PyDict* keywords);
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
// The part that `from`, and what comes after it in the order of resolution, have in making an instance of the class.
JSValue instantiateFrom(JSGlobalObject*, PyType*, PyType* from, const ArgList&, KeywordNames* keywordNames);

// ---- Classes

// What a class statement does. `body` is a function that fills in the namespace it is given.
JSValue buildClass(JSGlobalObject*, JSValue body, JSString* name, PyTuple* bases, PyDict* keywords);
// type.__new__(metatype, name, bases, namespace, **keywords)
JSValue newType(JSGlobalObject*, PyType* metatype, JSString* name, PyTuple* bases, PyDict* namespaceDict, PyDict* keywords);
// isinstance() and issubclass(), which a class can have its own idea of.
bool isInstanceOf(JSGlobalObject*, JSValue, JSValue classInfo);

// function(self) or function(self, argument), for a function that is called on behalf of an instance. One of JavaScript's gets the instance as `this`.
JSValue callForInstance(JSGlobalObject*, JSValue function, JSValue self, JSValue argument = JSValue());

// property
JSValue getProperty(JSGlobalObject*, PyNativeObject*, JSValue instance);
void setProperty(JSGlobalObject*, PyNativeObject*, JSValue instance, JSValue); // An empty value deletes.
bool isAbstract(JSGlobalObject*, JSValue); // Whether it says that it is: __isabstractmethod__.

// list[int] and int | str
bool isGenericAlias(JSGlobalObject*, JSValue);
bool isUnion(JSGlobalObject*, JSValue);
PyTuple* argumentsOfUnion(JSValue);
JSValue newGenericAlias(JSGlobalObject*, JSValue origin, JSValue arguments, bool starred = false);
JSValue unionOf(JSGlobalObject*, JSValue, JSValue); // NotImplemented if either is not something that there can be a union of.
JSValue unionFrom(JSGlobalObject*, JSValue arguments);
PyTuple* makeParameters(JSGlobalObject*, JSValue arguments);
PyTuple* substituteParameters(JSGlobalObject*, JSValue self, JSValue arguments, PyTuple* parameters, JSValue item);
void appendTypeRepr(JSGlobalObject*, StringBuilder&, JSValue);

// What `def f[T]`, `class C[T]` and `type A = ...` make
bool isTypeAlias(JSGlobalObject*, JSValue);
JSValue newTypeVar(JSGlobalObject*, JSString* name, JSValue evaluator, bool isConstraints);
JSValue newParamSpec(JSGlobalObject*, JSString* name);
JSValue newTypeVarTuple(JSGlobalObject*, JSString* name);
void setTypeParameterDefault(JSGlobalObject*, JSValue parameter, JSValue evaluator);
JSValue newTypeAlias(JSGlobalObject*, JSString* name, JSValue typeParameters, JSValue computeValue);
JSValue subscriptGeneric(JSGlobalObject*, PyTuple* typeParameters);
JSObject* createTypingModule(JSGlobalObject*); // _typing

// t"..."
JSValue newInterpolation(JSGlobalObject*, JSValue value, JSValue expression, JSValue conversion, JSValue formatSpecification);
JSValue newTemplate(JSGlobalObject*, JSValue strings, JSValue interpolations);
bool isSubclassOf(JSGlobalObject*, JSValue, JSValue classInfo);
PyTuple* defaultOrder(JSGlobalObject*, PyType*); // What type.mro() gives.
PyType* superCheck(JSGlobalObject*, PyType*, JSValue object); // The class whose order super(type, object) searches. Null, having raised, if it makes no sense.
void setBases(JSGlobalObject*, PyType*, JSValue); // C.__bases__ = ...
bool areLaidOutAlike(JSGlobalObject*, PyType* oldType, PyType* newType); // Whether an instance of the one could be made an instance of the other.
// An attribute got through super().
JSValue getSuperAttribute(JSGlobalObject*, JSValue superObject, PropertyName);

// ---- Modules

// A module is an instance of the class `module`, or of one derived from it, like any other instance. Its attributes, which are its properties, are
// the global variables of the code in it.
JS_EXPORT_PRIVATE JSObject* newModule(JSGlobalObject*, const String& name, PyType* = nullptr);
// One that is written in C++. It has the __doc__ that CPython's has, if CPython has it.
JS_EXPORT_PRIVATE JSObject* newBuiltinModule(JSGlobalObject*, ASCIILiteral name);
// The value, if it is a module. Otherwise null.
JSObject* tryModule(JSGlobalObject*, JSValue);

// import name, as the statement does it. `fromList` is None or a tuple of names.
JS_EXPORT_PRIVATE JSValue importModule(JSGlobalObject*, JSObject* globals, const String& name, JSValue fromList, unsigned level, bool wantsLeaf);
void registerModule(JSGlobalObject*, const String& name, JSValue module);
// The source in a file. Null, with nothing raised, if it cannot be read.
SourceCode readSourceIfPresent(JSGlobalObject*, const String& path);
// The module that a file is, which is run if it has not been. For a module that is asked for by where it is: JavaScript's `import`.
JSValue importModuleFromSource(JSGlobalObject*, const SourceCode&);
// The same, as what a module of JavaScript's can import: each of its global variables by name, and itself as the default.
void exportModule(JSGlobalObject*, const SourceCode&, Vector<Identifier, 4>& exportNames, MarkedArgumentBuffer& exportValues);
// exception.args
PyTuple* exceptionArguments(JSGlobalObject*, JSValue exception);
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
// What is in a cell, which is empty if nothing is.
JSValue contentsOfCell(JSValue cell);
// The cell for __class__, from what the body of a class returned. None if there is none.
JSValue cellForClass(JSGlobalObject*, JSValue returnedByBody);
// Whether it is a generator whose code has CO_ITERABLE_COROUTINE, and so can be awaited.
bool isIterableCoroutine(JSGlobalObject*, JSValue);
void setContentsOfCell(VM&, JSValue cell, JSValue);
WriteBarrierBase<Unknown>* variableOfCell(JSValue cell, JSCell*& owner); // Where that is, and the cell that a write to it is a write to.

// Notes that an exception has come to a frame of Python code, by being raised in it or by coming out of what it called. The unwinder calls it.
void addTracebackEntry(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex);
// A frame of Python code is about to be no more. If it has a frame object, that outlives it. The unwinder calls it, and op_py_ret.
void leaveFrame(VM&, CallFrame*, BytecodeIndex);
// The slow path of op_py_enter: either it is too deep, and RecursionError is raised, or something is to be told that a frame has begun or been resumed.
void raiseRecursionError(JSGlobalObject*);
void enterFrame(JSGlobalObject*, CallFrame*, BytecodeIndex, bool isResume);
// Those of op_py_line, op_py_ret and op_py_leave, which are only come to if something is to be told.
enum class LineKind : uint8_t {
    Line,
    AfterBackwardJump, // Going round a loop again, which sys.settrace() is told of though it be all on one line.
    OfHandledException, // Nothing is told. The frame is back on the line that what is being handled was raised on, as it is when `with` lets an exception go on its way.
};
void frameIsAtLine(JSGlobalObject*, CallFrame*, BytecodeIndex, LineKind);
void setTracesOpcodes(JSGlobalObject*, PyFrame*, bool); // Whether sys.settrace() is told of each instruction of the frame's code.
int lineOfTracebackFor(JSGlobalObject*, JSValue exception, PyFrame*); // The line that an exception came to a frame on, or -1.
enum class ToldArgument : uint8_t { None, First, ListOfPositional };
void frameIsCalling(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue callable, JSValue argument, ToldArgument);
void frameIsBranching(JSGlobalObject*, CallFrame*, BytecodeIndex, bool isTaken);
void frameIsBranchingInLoop(JSGlobalObject*, CallFrame*, BytecodeIndex, bool isExhausted); // At op_py_iter_next.
void frameIsJumping(JSGlobalObject*, CallFrame*, BytecodeIndex);
void frameIsReturning(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue);
void frameIsYielding(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue);
// A generator that the frame was going through, or waiting on, has returned. To be called if VM::isPythonWatched().
void generatorHasReturnedTo(JSGlobalObject*, CallFrame*, BytecodeIndex, JSValue returned);
// What is no generator says that there is no more by raising StopIteration, which is caught at once by what asked. It was raised all the same, and the frame that asked is told
// of as one that it came to. The first is for what catches it, the second for what began the asking, before and after. All are for if VM::isPythonWatched().
void noteCaughtStopIteration(JSGlobalObject*, JSValue exception);
void forgetCaughtStopIteration(JSGlobalObject*);
void tellOfCaughtStopIteration(JSGlobalObject*, CallFrame*, BytecodeIndex);
// The unwinder has done with a frame of Python code, which is counted no more.
void unwindFrame(VM&, CallFrame*, BytecodeIndex);
// What becomes of an exception in a frame of Python code, which the unwinder tells of if VM::isPythonWatched(). What is told may raise something itself, which is then what is
// being thrown, and is returned.
enum class ExceptionProgress : uint8_t {
    CameToFrame, // By being raised in it, or by coming out of what it called.
    WasRaisedAgain, // By what had caught it, in the same frame.
    IsHandled,
    LeavesFrame,
};
Exception* tellOfException(VM&, CallFrame*, BytecodeIndex, JSValue thrown, ExceptionProgress);
// What Python prints when an exception gets away: the traceback, and those of what led to it.
String formatException(JSGlobalObject*, JSValue exception);
bool isSequence(JSGlobalObject*, JSValue); // PySequence_Check()
bool isSequenceSlot(JSGlobalObject*, JSValue method); // Whether it is what a built-in sequence has for + or *, which has its turn after what numbers have. See binaryOperation().
// Warnings from what is written in C++: PyErr_WarnEx() and PyErr_WarnExplicitObject(). False if something has been raised, as it is if the program has asked for such warnings to be errors.
bool warn(JSGlobalObject*, BuiltinType category, const String& message, int64_t stackLevel = 1, JSValue source = { });
bool warnExplicit(JSGlobalObject*, BuiltinType category, const String& message, const String& filename, int64_t line);
// For what __int__() and the like return, which is to be of the class itself. What is of a class derived from it is let by, with a warning: `before`, the name of its class, and the rest.
bool warnIfOfStrictSubclass(JSGlobalObject*, JSValue result, BuiltinType, const String& before, ASCIILiteral className);
// For `except*`.
JSValue matchExceptionGroup(JSGlobalObject*, CallFrame*, JSValue exception, JSValue pattern);
JSValue prepareReraiseStar(JSGlobalObject*, JSValue original, JSArray* exceptions);
void forEachTracebackEntry(JSGlobalObject*, JSValue traceback, const ScopedLambda<void(PyFrame*, unsigned bytecodeOffset, unsigned line)>&); // From the outermost frame in.
String appendSyntaxErrorLocation(JSGlobalObject*, StringBuilder&, JSValue exception);
bool isSpace(char32_t); // str.isspace()
String formatTraceback(JSGlobalObject*, JSValue traceback); // "Traceback (most recent call last):" and what follows, or nothing if it is not a traceback.
// What has been raised has nowhere to go: sys.unraisablehook is told of it, and nothing is raised any longer. PyErr_FormatUnraisable() and PyErr_WriteUnraisable().
void reportUnraisable(JSGlobalObject*, const String& message, JSValue object = JSValue());
// sys.audit(event, *arguments). False if a hook raised. It costs nothing to speak of while there are no hooks.
bool auditSlow(JSGlobalObject*, ASCIILiteral event, const ArgList& arguments);
// What has been raised and not caught by a program that is being run: sys.excepthook is given it. PyErr_Print(), but for SystemExit.
void reportUncaughtException(JSGlobalObject*, JSValue exception);

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

// Whether what is in it can be taken straight out of it: it is a dict, of a class that may be derived from dict but goes through it as dict does. As in dict_merge(), a
// __getitem__() or a keys() of its own is not asked.
bool isGoneThroughAsDict(JSGlobalObject*, JSValue);
void updateDictFrom(JSGlobalObject*, PyDict*, JSValue mappingOrPairs);

// ---- Iteration

JSValue getIterator(JSGlobalObject*, JSValue);
// Empty when there is no more, with nothing raised. If that is because a generator has returned, what it returned is given too.
JSValue iteratorNext(JSGlobalObject*, JSValue iterator, JSValue* returnedByGenerator = nullptr);
// Calls the function with each. It returns false to stop. Returns false if something was raised.
bool forEach(JSGlobalObject*, JSValue iterable, const ScopedLambda<bool(JSValue)>&);
bool collect(JSGlobalObject*, JSValue iterable, MarkedArgumentBuffer&);
void unpackSequence(JSGlobalObject*, JSValue iterable, unsigned count, int starIndex, Register* first);
JSValue newTuple(JSGlobalObject*, Register* first, unsigned count);

// ---- What every object can be asked

String repr(JSGlobalObject*, JSValue);
String str(JSGlobalObject*, JSValue);
String addressOf(const void*); // 0x..., as in <object object at 0x...>
String fullyQualifiedTypeName(JSGlobalObject*, JSValue); // What CPython's %T writes: the class of something, with where it is from unless that is builtins or __main__.
JSValue format(JSGlobalObject*, JSValue, const String& specification);
// Never -1, unless it raised.
int64_t hash(JSGlobalObject*, JSValue);
int64_t hashOfString(const String&);
// object.__hash__
int64_t hashOfPointer(const void*);

// ---- Numbers

// An int, from what may not fit an int32.
JSValue intFromInt64(JSGlobalObject*, int64_t);
JSValue parseInt(JSGlobalObject*, StringView, unsigned base); // The int that a string spells, in the base. Empty if it spells none.
JSValue intFromDouble(JSGlobalObject*, double); // Truncated.
JSValue floatFromDouble(double);
// ---- range

// len(range). It raises OverflowError, and this is -1, if that is more than fits.
int64_t rangeLength(JSGlobalObject*, PyRange*);
// range[key]
JSValue rangeGetItem(JSGlobalObject*, PyRange*, JSValue key);
// value in range
bool rangeContains(JSGlobalObject*, PyRange*, JSValue);
bool rangesAreEqual(PyRange*, PyRange*);
JSValue rangeIterator(JSGlobalObject*, PyRange*);
// The item of a range that is at an index, which is an int that is not negative. Empty if that is past the end.
JSValue nextOfLongRange(JSGlobalObject*, PyRange*, JSValue index);

// ---- Numbers, from anything

// operator.index(value): an int and nothing else, from an int, a bool, an instance of a class derived from int, or what has __index__.
JSValue toInt(JSGlobalObject*, JSValue);
// The value of an int, if it fits.
std::optional<int64_t> tryInt64(JSValue);
int compareInts(JSValue, JSValue); // Negative, zero or positive. Both are ints, of any size.
// What __index__ gives, as something to index with: clamped to the range of an int64 if `clamp`, or else it raises IndexError.
std::optional<int64_t> toIndex(JSGlobalObject*, JSValue, bool clamp = false);
// The same, for what in CPython is an int of C's, and raises OverflowError if it does not fit in one.
std::optional<int> toCInt(JSGlobalObject*, JSValue);
std::optional<int64_t> toIndexOrOverflow(JSGlobalObject*, JSValue); // PyNumber_AsSsize_t(value, PyExc_OverflowError)
// The value of an int, a bool or a float, or of what has __float__ or __index__.
std::optional<double> toDouble(JSGlobalObject*, JSValue);
String reprOfDouble(double);
String reprOfComplex(double real, double imaginary);

// ---- The arguments of a function written in C++

class NativeArguments {
public:
    explicit NativeArguments(CallFrame* callFrame)
        : m_callFrame(callFrame)
        , m_keywordNames(isKeywordNames(callFrame->thisValue()) ? uncheckedDowncast<KeywordNames>(callFrame->thisValue().asCell()) : nullptr)
        , m_positionalCount(callFrame->argumentCount() - (m_keywordNames ? m_keywordNames->length() : 0))
    {
    }

    // How many were given by position.
    unsigned size() const { return m_positionalCount; }
    // The argument that is at a position in the signature, whether it was given by position or by name. Empty if it was not given.
    JSValue at(unsigned index) const
    {
        if (index < m_positionalCount) [[likely]]
            return m_callFrame->uncheckedArgument(index);
        return m_keywordNames ? givenByName(index) : JSValue();
    }
    // One that is known to have been given by position.
    JSValue operator[](unsigned index) const
    {
        ASSERT(index < m_positionalCount);
        return m_callFrame->uncheckedArgument(index);
    }

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
    JSValue givenByName(unsigned index) const;

    CallFrame* m_callFrame;
    KeywordNames* m_keywordNames;
    unsigned m_positionalCount;
};

} } // namespace JSC::Python
