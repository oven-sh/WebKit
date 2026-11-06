/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"

// What the runtime calls of the C API (CApi.h).

namespace JSC {

class Heap;

namespace Python {

class Runtime;

namespace CApi {

struct Interpreter;

// Null until a program needs the API, and again when its interpreter has ended.
extern Interpreter* s_interpreter;

// Before anything of C's that may call the API is called. False, with an ImportError: the API is another interpreter's.
bool ensure(JSGlobalObject*);
bool belongsToSlow(const Runtime&);
ALWAYS_INLINE bool belongsTo(const Runtime& runtime) { return s_interpreter && belongsToSlow(runtime); }
void runtimeIsEnding(Runtime&);

// The rest is for the runtime that the API belongs to.

// For the collector, with the world stopped, when marking is over and nothing has been freed yet.
void didFinishMarking(JSC::Heap&);

// After a call of a function of C's that has the GIL: what it raised, if it did, is thrown. False: it is.
bool throwWhatCRaised(JSGlobalObject*);
// C has handed over a reference with a result: these are the bits of the PyObject*.
void takeOverReference(uint64_t bits);
// The bits of the PyObject* for the object.
uint64_t bitsForC(JSValue);
// The object, if the bits are the address of one of the symbols that stand for objects (&PyLong_Type). Empty: they are not.
JSValue objectOfSymbol(uint64_t bits);

} // namespace CApi
} // namespace Python
} // namespace JSC
