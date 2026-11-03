/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BytecodeIndex.h"
#include "JSCJSValue.h"
#include "VirtualRegister.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC {

class CallFrame;
class CodeBlock;
class JSCell;
class JSGlobalObject;
class VM;

namespace Python {

// What the rest of JSC calls where code blocks of Python code need something other than what JS does.

// A call of what cannot be called: TypeError, "'int' object is not callable".
JS_EXPORT_PRIVATE void throwNotCallable(JSGlobalObject*, JSValue callee);

// op_check_tdz found nothing in the register: UnboundLocalError.
void throwUnboundLocal(JSGlobalObject*, CodeBlock*, VirtualRegister);

// An exception has been thrown in a frame of Python code, or has come up to it from what it called. Then, it is about to be
// caught there, or to leave the frame. Whoever is told of that (sys.monitoring) may raise another in its place: these return what
// is being thrown now.
JSValue addTracebackEntry(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex);
JSValue exceptionStaysOrLeaves(JSGlobalObject*, JSValue exception, CallFrame*, BytecodeIndex, bool isCaught);

// Code is about to be run for the first time, or for the first time in a while.
void didCreateCodeBlock(CodeBlock*);

// The collector is about to scan the stack, every word of it. A register that held an argument of a call that has returned
// still holds it, and would keep the object alive for as long as the frame lasts, though nothing can read the register
// again: `del x; gc.collect()` would not free what x was. So the temporaries of Python frames that are dead are emptied.
// (Variables are left alone: an object lives as long as a variable names it, whether the code goes on to use it or not.)
void clearDeadTemporaries(VM&);

// A constant of compiled code that is an object of Python's, as what it is made of, which is how the bytecode cache keeps it
// (runtime/CachedTypes.cpp).
struct ConstantParts {
    enum class Kind : uint8_t { Float, Complex, Bytes, Tuple, KeywordNames, FrozenSet, Slice, Ellipsis, AssertionError, NotImplementedError };
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

// Python tells 2.0 from 2, which are one number in JS, by keeping floats that are whole numbers in objects. Code that is
// not Python is given the number where a value is handed to it: the result of a call, an argument, an attribute.
JS_EXPORT_PRIVATE JSValue valueForJS(JSValue);
// What a function of Python's returns, for whoever called it. If that is not Python (JS; C++ of the host): valueForJS(), and the
// promise of a coroutine's result, which starts to run, as an async function of JS does. `callFrame` is that of the function.
// Does not throw.
JSValue resultForCaller(JSGlobalObject*, CallFrame*, JSValue result);

// The operators of JS on objects of Python (mayOverloadOperators() in JSCJSValueInlines.h says which values those are):
// a + b is what it is in Python if the class of one of them says what it is (__add__, __radd__). If neither does, the
// operator does what it does to any other object in JS.
enum class JSOperator : uint8_t {
    Add, Sub, Mul, Div, Mod, Pow, LShift, RShift, BitOr, BitXor, BitAnd,
    Negate, BitNot,
    Less, LessEq, Greater, GreaterEq,
};
// Empty without an exception: not overloaded.
JS_EXPORT_PRIVATE JSValue binaryOperatorForJS(JSGlobalObject*, JSOperator, JSValue left, JSValue right);
JS_EXPORT_PRIVATE JSValue unaryOperatorForJS(JSGlobalObject*, JSOperator, JSValue);
// JS has it that a comparison is true or false: that is the truth of whatever the method returns.
JS_EXPORT_PRIVATE std::optional<bool> compareForJS(JSGlobalObject*, JSOperator, JSValue left, JSValue right);
// ==, of two values that are not the same and neither of which is undefined or null.
JS_EXPORT_PRIVATE std::optional<bool> equalForJS(JSGlobalObject*, JSValue left, JSValue right);

// Python code is calling what is not Python code: whether it is, and the frame made ready for it (or an exception).
JS_EXPORT_PRIVATE bool isForeignCallee(JSValue callee);
JS_EXPORT_PRIVATE void prepareForeignCall(JSGlobalObject*, CallFrame* calleeFrame);

} // namespace Python
} // namespace JSC
