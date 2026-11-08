/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"
#include <wtf/ScopedLambda.h>

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
class SlotVisitor;
class VM;

namespace Python {

class PyType;
class Runtime;
struct FinalizerState;

namespace CApi {

// WHOSE THE API IS. The functions that C calls are not told for which interpreter, and a symbol such as PyLong_Type stands for one object. So the API serves one interpreter of
// the process: THE FIRST THAT THERE IS, from its beginning to its end, and nobody after that: what was loaded has pointers into its heap. Nothing that a program does
// changes whose it is.
//
// WHEN IT IS SET UP. When that interpreter first has a use for it:
//  - before it loads a library: what a library does as it is loaded may call;
//  - before code of C's is called that may call it (a function of a PyDLL);
//  - when C is handed a reference.
// With the GIL.

// Sets it up. False, with an ImportError: the API is not for this interpreter.
bool ensure(JSGlobalObject*);

// identityBits(), for an instance of a type of an extension (TypeFlag::InstancesHaveDataOfC). Declared in PyType.h as well, which others than the engine include.
uint64_t bitsOfInstance(JSCell*);

#if PYTHON_HAS_CAPI

// False, and nothing is raised: for what can do without.
bool tryEnsure(JSGlobalObject*);

void runtimeIsBeginning(Runtime&);
// Its built-in types are there: those that C names by symbols are told so.
void didMakeTypes(Runtime&);
// By its thread, or by the collector with the world stopped. More than once does no harm.
void runtimeIsEnding(Runtime&);
// The collector of another VM asks too, on a thread of its own, while this one may be ending: the pointers are compared and never followed.
extern std::atomic<const Runtime*> s_runtimeItIsFor;
extern std::atomic<const Runtime*> s_runtimeItIsSetUpFor;
ALWAYS_INLINE bool isFor(const Runtime& runtime) { return s_runtimeItIsFor.load(std::memory_order_relaxed) == &runtime; }
ALWAYS_INLINE bool isSetUpFor(const Runtime& runtime) { return s_runtimeItIsSetUpFor.load(std::memory_order_relaxed) == &runtime; }
ALWAYS_INLINE bool isSomebodys() { return s_runtimeItIsSetUpFor.load(std::memory_order_relaxed); }

// Around code of C's that an interpreter runs on this thread: a foreign call; dlopen() and dlclose(), for what a library does as it is loaded and as it goes (a constructor or a
// destructor, those of a static variable in C++). Whoever calls the API meanwhile calls it for that interpreter, until code of Python's that C calls back does the same again.
// (C that JS runs, by bun:ffi or N-API, runs for nobody in particular: it is served if its thread holds the GIL of the interpreter that has the API.)
// IT IS ABOUT C, AND ONLY C IS ASKED. What the runtime does for itself, the functions below, does not depend on it: code of the interpreter that is served may run deep inside C that
// another one runs (that C calls back, the callback calls JS, JS calls Python of the first), and its calls come with references and exceptions like any.
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

// ctypes: code has handed over a reference with its result. It is given up. `programGetsObject`: code of the program gets the object, as the result of the call or as what a _check_retval_ or an errcheck is handed; if not, nobody has got it. The code may be that of a
// callback of the interpreter that the API is set up for, which another of the VM has called by its address. Nor need the API be set up. It asks for itself.
void takeOverReference(uint64_t bits, bool programGetsObject);

// ---- For the interpreter that the API has been set up for (isSetUpFor()) ----

// For the collector, with the world stopped, when marking is over and nothing has been freed yet. True: it has queued something (Runtime::queueToFinalize()), of which the threads are to be told.
bool didFinishMarking(JSC::Heap&);

// ---- Instances of the types of extensions (TypeFlag::InstancesHaveDataOfC) ----

// Empty, with an exception: there is no room.
JSValue allocateInstance(JSGlobalObject*, PyType*);
// Whether an instance of one type can become an instance of the other. One of them at least has the flag.
bool instancesAreAlike(PyType*, PyType*);

// What Runtime::registerFinalizable() keeps in the object. Null: the cell is no instance.
FinalizerState* finalizerStateOfSlow(JSCell*);
ALWAYS_INLINE FinalizerState* finalizerStateOf(JSCell* cell) { return isSomebodys() ? finalizerStateOfSlow(cell) : nullptr; }

// For the collector, with the world stopped, when all that is reachable and all that is kept for finalizers has been marked. True: it has marked more.
bool markWhatIsLeft(SlotVisitor&);

// The object whose PyObject* is the address of this cell, if the cell is not an object itself. Empty: it is nothing of the kind. With the GIL.
JSValue objectOfBody(Runtime&, JSCell*);
// Whether this is the address of a cell that C owns a reference to. Then there is a cell, and it lives: every key of the table is marked by every collection. The address is not followed.
// With the GIL, for the interpreter that the API is set up for.
bool isAddressOfCellThatCOwns(const void*);

// __class__ has been assigned to.
void didChangeClass(JSCell*, PyType* oldType, PyType* newType);

// Of those that have something to do when they die: how many there are, dead or alive. With the GIL.
size_t numberOfInstancesSlow();
ALWAYS_INLINE size_t numberOfInstances(Runtime& runtime) { return isSetUpFor(runtime) ? numberOfInstancesSlow() : 0; }
// Whether one of them is still to be destroyed, if it dies. With the GIL.
bool hasInstancesToDestroySlow();
ALWAYS_INLINE bool hasInstancesToDestroy(Runtime& runtime) { return isSetUpFor(runtime) && hasInstancesToDestroySlow(); }

// gc.is_tracked(), for an instance.
bool isTracked(JSCell*);
// For a cell in the queue of finalizers: what Python sees of it, if it is the body of an instance that has a face. It makes nothing, and nobody is seen to take anything.
JSCell* faceOfBodyIfAnySlow(JSCell*);
ALWAYS_INLINE JSCell* faceOfBodyIfAny(JSCell* cell) { return isSomebodys() ? faceOfBodyIfAnySlow(cell) : nullptr; }
// The address that id() and repr() say, for a message about an object that nobody has in hand. The same.
const void* addressInMessagesSlow(JSCell*);
ALWAYS_INLINE const void* addressInMessages(JSCell* cell) { return isSomebodys() ? addressInMessagesSlow(cell) : cell; }
// What a type with TypeFlag::InstancesHaveDataOfC tells the program: how much of C's it adds to an instance, and Py_TPFLAGS_HAVE_GC.
size_t sizeOfDataThatTypeAdds(PyType*); // A type that an extension has made.
bool saysThatCollectorIsTold(PyType*);
// gc.get_referrers(): what refers to one of the values by a reference that C holds in it. With the GIL, right after a full collection.
void forEachInstanceThatRefersTo(std::span<const JSValue>, const ScopedLambda<void(JSCell*)>&);

// The turn of a cell, whatever it is, in the queue of finalizers: around all that is done for it.
class TurnInQueue {
    WTF_MAKE_NONCOPYABLE(TurnInQueue);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    TurnInQueue(JSGlobalObject* globalObject, JSCell* cell)
    {
        if (isSomebodys()) [[unlikely]]
            begin(globalObject, cell);
    }
    // There is nothing left for the runtime to do.
    bool isOver() const { return m_isOver; }

private:
    void begin(JSGlobalObject*, JSCell*);

    bool m_isOver { false };
};

// ---- ctypes ----

// After code of C's has run with the GIL.
// What it raised, if it did, is thrown. False: it is.
bool throwWhatCRaised(JSGlobalObject*);
// The same where C has said that all went well: a SystemError, of which what C raised is the cause. `who` is the repr() of what was called.
bool throwWhatCRaisedBesideResult(JSGlobalObject*, const String& who);
// Nobody will look: the call ends otherwise.
void forgetWhatCRaised();

// C is handed a reference: these are the bits of the PyObject*.
void giveReference(uint64_t bits);
// It is given back, as by C: _ctypes.Py_DECREF().
void giveBackReference(uint64_t bits);
// (takeOverReference(): see "For every interpreter".)

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
inline bool didFinishMarking(JSC::Heap&) { return false; }
inline JSValue allocateInstance(JSGlobalObject*, PyType*) { return { }; }
inline bool instancesAreAlike(PyType*, PyType*) { return false; }
inline bool markWhatIsLeft(SlotVisitor&) { return false; }
inline JSValue objectOfBody(Runtime&, JSCell*) { return { }; }
inline bool isAddressOfCellThatCOwns(const void*) { return false; }
inline void didChangeClass(JSCell*, PyType*, PyType*) { }
inline size_t numberOfInstances(Runtime&) { return 0; }
inline bool hasInstancesToDestroy(Runtime&) { return false; }
inline FinalizerState* finalizerStateOf(JSCell*) { return nullptr; }
class TurnInQueue {
public:
    TurnInQueue(JSGlobalObject*, JSCell*) { }
    ~TurnInQueue() { }
    bool isOver() const { return false; }
};
inline bool isTracked(JSCell*) { return false; }
inline JSCell* faceOfBodyIfAny(JSCell*) { return nullptr; }
inline const void* addressInMessages(JSCell* cell) { return cell; }
inline size_t sizeOfDataThatTypeAdds(PyType*) { return 0; }
inline bool saysThatCollectorIsTold(PyType*) { return true; }
inline void forEachInstanceThatRefersTo(std::span<const JSValue>, const ScopedLambda<void(JSCell*)>&) { }
inline bool throwWhatCRaised(JSGlobalObject*) { return true; }
inline bool throwWhatCRaisedBesideResult(JSGlobalObject*, const String&) { return true; }
inline void forgetWhatCRaised() { }
inline void giveBackReference(uint64_t) { }
inline void takeOverReference(uint64_t, bool) { }
inline void giveReference(uint64_t) { }

#endif // PYTHON_HAS_CAPI

} // namespace CApi
} // namespace Python
} // namespace JSC
