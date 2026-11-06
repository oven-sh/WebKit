/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BytecodeIndex.h"
#include "CallData.h"
#include "JSCJSValue.h"
#include "VirtualRegister.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC {

class CallFrame;
class CodeBlock;
class JSCell;
class JSFunction;
class JSGlobalObject;
class VM;
struct OpPyCallSpread;

namespace Python {

// What the rest of JSC calls where code blocks of Python code need something other than what JS does.

// A call of what cannot be called: TypeError, "'int' object is not callable".
JS_EXPORT_PRIVATE void throwNotCallable(JSGlobalObject*, JSValue callee);

// op_check_tdz found nothing in the register: UnboundLocalError.
void throwUnboundLocal(JSGlobalObject*, CodeBlock*, VirtualRegister);

// An exception has been thrown in a frame of Python code, or has come up to it from what it called. Then, it is about to be
// caught there, or to leave the frame. Whoever is told of that (sys.monitoring) may raise another in its place: these return what
// is being thrown now. `frameBefore`: the frame with a code block that the exception left last, or the one it was thrown in.
// `handler`: what will catch it in this frame. Null: nothing will.
JSValue addTracebackEntry(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex, CallFrame* frameBefore, const HandlerInfo* handler);
JSValue exceptionStaysOrLeaves(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex, bool isCaught);
// PyException::willBeSeen(), if it is one.
void exceptionWillBeSeen(JSValue thrown);
// The same for a call of a function of JS, where there is a Python to tell. (The frame is only compared: of a call that has been made
// part of its caller, it is where the frame would be.) `throwsAgain`: what catches it there is no `catch` of the program's. It runs no code.
// `hasNoTraceback` belongs to one unwind, which begins with false: this sets it when it finds a value that is none of Python's and has not
// been through code of Python's. Nothing is noted about where such a value goes, and only a frame of Python's changes what it is.
void addTracebackEntryForJS(JSGlobalObject*, JSValue thrown, CallFrame*, CodeBlock*, BytecodeIndex, bool throwsAgain, bool& hasNoTraceback);
// An op_catch in code of JS has taken what was thrown, where there is a Python to tell. If it is that of a `catch` of the program's,
// this is the exception being handled until the clause is left, as in an `except`. It runs no code.
void didCatchInJS(JSGlobalObject*, CallFrame*, CodeBlock*, BytecodeIndex, JSValue thrown);
// A `throw` is about to throw this, where there is a Python to tell. An exception of Python's that has not been anywhere yet
// remembers what is being handled, as one that is raised does.
void willThrowInJS(JSGlobalObject*, JSValue thrown);

// Code is about to be run for the first time, or for the first time in a while.
void didCreateCodeBlock(CodeBlock*);

// The collector is about to scan the stack, every word of it. A register that held an argument of a call that has returned
// still holds it, and would keep the object alive for as long as the frame lasts, though nothing can read the register
// again: `del x; gc.collect()` would not free what x was. So the temporaries of Python frames that are dead are emptied.
// (Variables are left alone: an object lives as long as a variable names it, whether the code goes on to use it or not.)
void clearDeadTemporaries(VM&);
// An exception is about to leave the frame of the body of a generator. The generator is done, and the exception that is pending
// afterwards may be another.
void generatorBodyIsUnwinding(JSGlobalObject*, CallFrame*, bool isTermination);

// A constant of compiled code that is an object of Python's, as what it is made of, which is how the bytecode cache keeps it
// (runtime/CachedTypes.cpp).
struct ConstantParts {
    enum class Kind : uint8_t { Float, Complex, Bytes, Tuple, KeywordNames, FrozenSet, Slice, Ellipsis, AssertionError, NotImplementedError, BuiltinAll, BuiltinAny, TupleType, ListType, SetType };
    Kind kind { Kind::Ellipsis };
    double numbers[2] { 0, 0 };
    std::span<const uint8_t> bytes;
    Vector<JSValue, 8> items; // While they are here and nowhere else, there must be no collection.
};
// False: the compiler makes no such constant.
bool constantParts(JSCell*, ConstantParts&);
JSValue constantFromParts(JSGlobalObject*, const ConstantParts&);

// VMTraps::NeedPythonCalls, at a place where any code may run. What a signal handler raises is thrown.
JS_EXPORT_PRIVATE void makeCallsOwed(VM&);
// For VMTraps::handleTraps() on another thread: see VMTraps::fireTrapsLeftToEmbedderThread().
void mainThreadHasTrapsToHandle(VM&);

// Python tells 2.0 from 2, which are one number in JS, by keeping floats that are whole numbers in objects. Code that is
// not Python is given the number where a value is handed to it: the result of a call, an argument, an attribute.
JS_EXPORT_PRIVATE JSValue valueForJS(JSValue);
// What a function of Python's returns, for whoever called it. If that is not Python (JS; C++ of the host): valueForJS(), and the
// promise of a coroutine's result, which starts to run, as an async function of JS does. `callFrame` is that of the function.
// Does not throw.
JSValue resultForCaller(JSGlobalObject*, CallFrame*, JSValue result);
// Whether the frame, of something in Python, was called by Python code or by the runtime on its behalf, as opposed to JS code or the host.
bool isCalledByPython(JSGlobalObject*, CallFrame*);

// Whether what JS sees as the properties of the object is what Python has for it (attributes, items): an object, class, module or
// function of Python's, the `prototype` of a class. What a host stores in such an object directly (putDirect()) no script ever sees:
// it is to go through the method table, as for a Proxy.
JS_EXPORT_PRIVATE bool propertiesArePythons(JSObject*);

// The operators of JS on objects of Python (mayOverloadOperators() in JSCJSValueInlines.h says which values those are):
// a + b is what it is in Python, whatever the other operand is: its type is asked as well ("%s" % x, [1] * n), and if neither has an
// answer that is a TypeError. But for `+` with a string of JS: there only the class of the object of Python's is asked, and if
// it has no answer the operator does what it does to any other object in JS, which is to concatenate.
enum class JSOperator : uint8_t {
    Add, Sub, Mul, Div, Mod, Pow, LShift, RShift, BitOr, BitXor, BitAnd,
    Negate, BitNot,
    Less, LessEq, Greater, GreaterEq,
};
// Empty without an exception: not overloaded (that `+`).
JS_EXPORT_PRIVATE JSValue binaryOperatorForJS(JSGlobalObject*, JSOperator, JSValue left, JSValue right);
JS_EXPORT_PRIVATE JSValue unaryOperatorForJS(JSGlobalObject*, JSOperator, JSValue);
// JS has it that a comparison is true or false: that is the truth of whatever the method returns.
JS_EXPORT_PRIVATE std::optional<bool> compareForJS(JSGlobalObject*, JSOperator, JSValue left, JSValue right);
// ==, of two values neither of which is undefined or null.
JS_EXPORT_PRIVATE std::optional<bool> equalForJS(JSGlobalObject*, JSValue left, JSValue right);

// Python code is calling what is not Python code: whether it is, and the frame made ready for it (or an exception).
// True: the callee is a class, and the call is `new callee(...)`.
JS_EXPORT_PRIVATE bool isForeignCallee(JSValue callee);
JS_EXPORT_PRIVATE bool prepareForeignCall(JSGlobalObject*, CallFrame* calleeFrame);
// Python code is calling what JS says cannot be called. An instance of a class derived from list is an array to JS, whatever methods the
// class has: to be a function as well would make it one for typeof, JSON.stringify() and console.log(). And JS does not ask the keys of
// the dict of a class (PyType::AsksKeys::No), under one of which the __call__ may lie.
JS_EXPORT_PRIVATE CallData callDataUnknownToJS(JSValue callee);
// Python code of that realm is calling a method, or an object with a __call__. If a function written in Python is behind it, that is entered in
// its place: the function, and what goes in front of the arguments is left in Runtime::selfOfCall for llint_python_call_with_self_trampoline.
// Null: there is no such function, and C++ makes the call.
JS_EXPORT_PRIVATE JSFunction* beginCallWithSelf(JSGlobalObject*, JSValue callee);

// op_py_call_spread: callee(leading..., *positional, **keywords).
// A longer tuple or list is for the slow path, which knows how many arguments there may be, and copies faster than it costs to get there: in
// instructions from about 100 items of a tuple and 190 of a list on, in time from about 75 of either.
static constexpr unsigned maxLengthSpreadByInterpreter = 96;
enum SpreadCallFlag : unsigned {
    KeywordsAreOfProgram = 1, // `keywords` is what the program wrote after the only **, which may be anything. Otherwise a dict that the compiled code has made.
    PositionalExtendsList = 2, // There are leading arguments. CPython has them in a list, and extends that with what is after the * before it comes to the **.
    ListIsNobodyElses = 4, // If `positional` is a list, the compiled code has made it.
    CodeRunsBeforeCall = 8, // Not of the instruction: whoever prepares the call ahead of it, and then runs code of the program, wants nothing left that this code can change.
};
// What the instruction does before it calls, in two steps, for its slow paths and for whoever has to know the arguments ahead of it.
// The first makes of `positional` a tuple or a list and of `keywords` a dict that is not empty, none of them an instance of a derived class, or leaves or
// makes them empty. If it had to make a dict of what the program wrote, it goes on: `positional` is a list of all the arguments but the leading ones,
// `keywords` their names as `this` has them. What it finds in one of these forms it leaves as it is, so it may be called again. It runs code of the
// program in CPython's order: what is after the ** is read first, but see PositionalExtendsList. What the call passes as `this`, the names or
// undefined, it leaves in `firstFree`. The number of arguments (with an exception, 0). More than maxArguments: the call is to be refused, and not all of this is done.
JS_EXPORT_PRIVATE uint64_t prepareSpreadCall(JSGlobalObject*, CallFrame*, const OpPyCallSpread&, unsigned moreFlags = 0);
// The second, right after that, copies them to where the arguments of the callee go, and returns `this`. It allocates nothing and raises nothing: the stack
// pointer is at the frame of the callee by then, so that C++ runs in what are registers of the caller to the collector (clearDeadTemporaries()).
JS_EXPORT_PRIVATE JSValue fillSpreadCall(CallFrame*, const OpPyCallSpread&, JSValue* arguments);

// The stack has no room for the frame of a call of Python code, which is about to be told so.
void frameHasNoRoom(CallFrame*, CodeBlock*);

// `class constructor extends superclass`, when the methods are in place and before anything sees the class. If the superclass is a
// class of Python's, so is this: the class. Otherwise the constructor. See PyJSClass.h.
JSC_DECLARE_HOST_FUNCTION(deriveClassFromJSFunction); // (constructor, prototype, superclass)
// op_set_function_name, for such a class: { [key]: class extends Base {} }
void setNameOfDerivedClass(JSGlobalObject*, JSCell*, JSValue name);

} // namespace Python
} // namespace JSC
