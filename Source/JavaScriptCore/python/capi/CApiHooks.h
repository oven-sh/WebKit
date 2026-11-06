/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"

// What the runtime calls of the C API (CApi.h).

// Not on Windows: the symbols that stand for objects are found by the section they lie in, for which every linker has its own way, and nothing there has been compiled, let alone run.
// Without it no interpreter ever has the API, and what needs it fails as it does in an interpreter that the API is not for.
#if !defined(PYTHON_HAS_CAPI)
#if OS(WINDOWS)
#define PYTHON_HAS_CAPI 0
#else
#define PYTHON_HAS_CAPI 1
#endif
#endif

namespace JSC {

class Heap;

namespace Python {

class Runtime;

namespace CApi {

// WHOSE THE API IS. The functions that C calls are not told for which interpreter, and a variable such as PyExc_ValueError holds one object. So the API serves one interpreter of
// the process: THE FIRST THAT THERE IS, from its beginning to its end, and nobody after that: what was loaded has pointers into its heap. Nothing that a program does
// changes whose it is.
//
// WHEN IT IS SET UP. When that interpreter first has a use for it:
//  - before it loads a library: what a library does as it is loaded may read those variables, and may call;
//  - before code of C's is called that may call it (a function of a PyDLL);
//  - when C is handed a reference.
// With the GIL.

// Sets it up. False, with an ImportError: the API is not for this interpreter.
bool ensure(JSGlobalObject*);

#if PYTHON_HAS_CAPI

// False, and nothing is raised: for what can do without.
bool tryEnsure(JSGlobalObject*);

void runtimeIsBeginning(Runtime&);
// Its built-in types are there, bare: those that C names by symbols are told so.
void didMakeTypes(Runtime&);
void runtimeIsEnding(Runtime&);
// The collector of another VM asks too, on a thread of its own, while this one may be ending: the pointers are compared and never followed.
extern std::atomic<const Runtime*> s_runtimeItIsFor;
extern std::atomic<const Runtime*> s_runtimeItIsSetUpFor;
ALWAYS_INLINE bool isFor(const Runtime& runtime) { return s_runtimeItIsFor.load(std::memory_order_relaxed) == &runtime; }
ALWAYS_INLINE bool isSetUpFor(const Runtime& runtime) { return s_runtimeItIsSetUpFor.load(std::memory_order_relaxed) == &runtime; }

// Around code of C's that an interpreter runs on this thread: a foreign call; dlopen() and dlclose(), for what a library does as it is loaded and as it goes (a constructor or a
// destructor, those of a static variable in C++). Whoever calls the API meanwhile calls it for that interpreter, until code of Python's that C calls back does the same again.
// (C that JS runs, by bun:ffi or N-API, runs for nobody in particular: it is served if its thread holds the GIL of the interpreter that has the API.)
class CRunsFor {
    WTF_MAKE_NONCOPYABLE(CRunsFor);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit CRunsFor(const Runtime&);
    ~CRunsFor();

private:
    const Runtime* m_previous;
};

// ---- For every interpreter ----

// From identityBits() back: this interpreter's object, if the bits are the address of one of the symbols that stand for objects. Empty: they are not.
JSValue objectOfSymbol(JSGlobalObject*, uint64_t bits);

// ---- For the interpreter that the API has been set up for ----

// For the collector, with the world stopped, when marking is over and nothing has been freed yet.
void didFinishMarking(JSC::Heap&);

// After code of C's has run with the GIL.
// What it raised, if it did, is thrown. False: it is.
bool throwWhatCRaised(JSGlobalObject*);
// The same where C has said that all went well: a SystemError, of which what C raised is the cause. `who` is the repr() of what was called.
bool throwWhatCRaisedBesideResult(JSGlobalObject*, const String& who);
// Nobody will look: the call ends otherwise.
void forgetWhatCRaised();

// C has handed over a reference with a result: these are the bits of the PyObject*.
void takeOverReference(uint64_t bits);
// And the other way round.
void giveReference(uint64_t bits);

#else // !PYTHON_HAS_CAPI

inline bool tryEnsure(JSGlobalObject*) { return false; }
inline void runtimeIsBeginning(Runtime&) { }
inline void didMakeTypes(Runtime&) { }
inline void runtimeIsEnding(Runtime&) { }
inline bool isFor(const Runtime&) { return false; }
inline bool isSetUpFor(const Runtime&) { return false; }
class CRunsFor {
public:
    explicit CRunsFor(const Runtime&) { }
    ~CRunsFor() { }
};
inline JSValue objectOfSymbol(JSGlobalObject*, uint64_t) { return { }; }
inline void didFinishMarking(JSC::Heap&) { }
inline bool throwWhatCRaised(JSGlobalObject*) { return true; }
inline bool throwWhatCRaisedBesideResult(JSGlobalObject*, const String&) { return true; }
inline void forgetWhatCRaised() { }
inline void takeOverReference(uint64_t) { }
inline void giveReference(uint64_t) { }

#endif // PYTHON_HAS_CAPI

} // namespace CApi
} // namespace Python
} // namespace JSC
