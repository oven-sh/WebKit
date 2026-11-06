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

// WHEN THE API COMES ABOUT. It is one interpreter's for good, so it is taken only for what cannot be done without it:
//  - before code of C's is called that may call it (a function of a PyDLL);
//  - when C is about to be handed what only the API can account for: a reference, or a type that C knows by a symbol;
//  - when a library calls it while it is being loaded.
// With the GIL.

// False, with an ImportError: the API is another interpreter's.
bool ensure(JSGlobalObject*);
// False, and nothing is raised: for what can do without.
bool tryEnsure(JSGlobalObject*);
// Around dlopen(). What a library does as it is loaded (a constructor, the initializer of a static variable in C++) may call the API, on this thread.
class LibraryBeingLoaded {
    WTF_MAKE_NONCOPYABLE(LibraryBeingLoaded);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit LibraryBeingLoaded(JSGlobalObject*);
    ~LibraryBeingLoaded();

private:
    JSGlobalObject* m_previous;
};
// The collector of another VM asks too, on a thread of its own, while this one may be ending: the pointer is compared and never followed.
extern std::atomic<const Runtime*> s_runtime;
ALWAYS_INLINE bool belongsTo(const Runtime& runtime) { return s_runtime.load(std::memory_order_relaxed) == &runtime; }
void runtimeIsEnding(Runtime&);
// The bits of the PyObject* for the object, for memory that C may read, now or later.
uint64_t bitsForC(JSGlobalObject*, JSValue);

// The rest is for the runtime that the API belongs to.

// For the collector, with the world stopped, when marking is over and nothing has been freed yet.
void didFinishMarking(JSC::Heap&);

// After a call of a function of C's that has the GIL: what it raised, if it did, is thrown. False: it is.
bool throwWhatCRaised(JSGlobalObject*);
// C has handed over a reference with a result: these are the bits of the PyObject*.
void takeOverReference(uint64_t bits);
// And the other way round.
void giveReference(uint64_t bits);
// The object, if the bits are the address of one of the symbols that stand for objects (&PyLong_Type). Empty: they are not.
JSValue objectOfSymbol(uint64_t bits);

} // namespace CApi
} // namespace Python
} // namespace JSC
