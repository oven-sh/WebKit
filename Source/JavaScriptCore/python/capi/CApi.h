/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CApiCounts.h"
#include "CApiHooks.h"
#include "CApiInstance.h"
#include "PyErrors.h"
#include "PyTuple.h"
#include "PyType.h"

#if PYTHON_HAS_CAPI

#include <pthread.h>

// CPython's C API, for extension modules built for abi3t (PEP 803): the ABI in which C sees the layout of no object. See "The C API"
// in ../README.md. This header is for the files of this directory. What ../runtime calls is in CApiHooks.h.

namespace JSC {
namespace Python {
namespace CApi {

// ---- What a PyObject* is ----
//
// The 64 bits of the JSValue. NULL is the empty value. So the address of a cell stands for the cell, None, the bools, and the ints and
// floats that are not cells are "addresses" that no memory has, and C, which has no struct to look into an object with, cannot tell.
// Two objects are the same if the pointers are equal, a container's own array of values is an array of PyObject*, and nothing
// is allocated for a value to cross.
//
// Four kinds of value are spelled otherwise for C than in the heap (identityBits() in ../runtime/PyType.h, which id() goes by too):
//  - An instance of the type of an extension. It is two cells, and C has the address of the one that Python and JS never see: CApiInstance.h.
//  - A built-in type that C names by a symbol (&PyLong_Type), or by a variable that is as good as one (PyExc_ValueError, which a library may read as it is loaded, before
//    there is an interpreter, and keep). The address is fixed when bun is linked, and the cell is made when the program runs. All these symbols lie in one section
//    (StaticObject). What comes in is tested for that range; a type that goes out is asked for its symbol (PyType::symbolForC()). The program sees the same number: it is the
//    type's id(), and its hash() goes by it.
//  - null, which only JS makes and which is None as undefined is. C compares with Py_None: it gets undefined.
//  - An int that is tagged as a double, as arithmetic may leave it. C compares pointers: it gets the one spelling that PyLong_FromLong() gives.
struct Opaque;
using Handle = Opaque*;

// What a symbol such as PyLong_Type or _Py_NoneStruct is. C takes its address and nothing else.
struct alignas(16) StaticObject {
    // Of what the runtime itself keeps alive for as long as it lives. Empty while the API is not set up, but in those that stand for what is no cell.
    EncodedJSValue value;
    // Which it is: see CApiInterpreter.cpp.
    uint64_t number;
};

#if OS(DARWIN)
#define PY_CAPI_STATIC_SECTION "__DATA,__py_statics"
extern "C" StaticObject pyStaticObjectsBegin __asm("section$start$__DATA$__py_statics");
extern "C" StaticObject pyStaticObjectsEnd __asm("section$end$__DATA$__py_statics");
#else
#define PY_CAPI_STATIC_SECTION "py_statics"
extern "C" StaticObject pyStaticObjectsBegin __asm("__start_py_statics");
extern "C" StaticObject pyStaticObjectsEnd __asm("__stop_py_statics");
#endif

// The section is gone through as an array (forEachSymbol()), so nothing lies between one symbol and the next. AddressSanitizer would put a red zone behind each: on Mach-O it does so in a
// section with a name of its own too (not on ELF).
#define PY_CAPI_IN_STATIC_SECTION used, section(PY_CAPI_STATIC_SECTION), no_sanitize("address")

// What C links with. `used`: nothing in bun refers to most of it. (The names are C's whatever namespace the definitions are in.)
#define PY_CAPI extern "C" __attribute__((visibility("default"), used))
#define PY_CAPI_STATIC_OBJECT(name, value, number) extern "C" { __attribute__((visibility("default"), PY_CAPI_IN_STATIC_SECTION)) constinit JSC::Python::CApi::StaticObject name { value, number }; }

ALWAYS_INLINE uintptr_t bitsOf(Handle handle) { return std::bit_cast<uintptr_t>(handle); }

ALWAYS_INLINE bool isStatic(Handle handle)
{
    return bitsOf(handle) - std::bit_cast<uintptr_t>(&pyStaticObjectsBegin) < std::bit_cast<uintptr_t>(&pyStaticObjectsEnd) - std::bit_cast<uintptr_t>(&pyStaticObjectsBegin);
}

// NULL gives the empty value.
ALWAYS_INLINE JSValue toJS(Handle handle)
{
    if (isStatic(handle)) [[unlikely]]
        return JSValue::decode(std::bit_cast<StaticObject*>(handle)->value);
    uintptr_t bits = bitsOf(handle);
    if (bits && !(bits & JSValue::NotCellMask)) {
        if (Body* body = Body::tryFromCellOfHandle(std::bit_cast<JSCell*>(handle))) [[unlikely]]
            return faceOf(body);
    }
    return JSValue::decode(static_cast<EncodedJSValue>(bits));
}

// For who only asks what it is. A body stays a body. NOT TO BE STORED OR PASSED ON.
ALWAYS_INLINE JSValue toLookAt(Handle handle)
{
    if (isStatic(handle)) [[unlikely]]
        return JSValue::decode(std::bit_cast<StaticObject*>(handle)->value);
    return JSValue::decode(static_cast<EncodedJSValue>(bitsOf(handle)));
}

// The cell whose references are counted: of an instance, the body. Null: the object is not a cell, or one that the runtime keeps alive anyway.
ALWAYS_INLINE JSCell* countedCell(Handle handle)
{
    if ((bitsOf(handle) & JSValue::NotCellMask) || isStatic(handle))
        return nullptr;
    return std::bit_cast<JSCell*>(handle);
}

ALWAYS_INLINE Handle toC(JSValue value) { return std::bit_cast<Handle>(static_cast<uintptr_t>(identityBits(value))); }

ALWAYS_INLINE Handle toC(Body* body) { return std::bit_cast<Handle>(body); }

// Null: it is no instance of the type of an extension.
ALWAYS_INLINE Body* bodyOrNull(Handle handle)
{
    JSCell* cell = countedCell(handle);
    return cell ? Body::tryFromCellOfHandle(cell) : nullptr;
}

// ---- The one interpreter ----
//
// Whose the API is, and when it is set up: see CApiHooks.h.

// What has to be a cell: the runtime refers to it (Runtime::Slot::CApi).
class State final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
    }
    static State* create(VM&, Structure*);

    WriteBarrier<Structure> madeByExtensionStructure;
    WriteBarrier<Structure> faceStructure;
    WriteBarrier<Structure> bodyStructure;

private:
    State(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

struct Interpreter {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Interpreter);

    Interpreter(JSGlobalObject* globalObject, State* state)
        : globalObject(globalObject)
        , vm(getVM(globalObject))
        , runtime(Python::runtime(globalObject))
        , state(state)
        , lock(vm.apiLock())
        , keyOfReferenceToThread(makeKeyOfReferenceToThread())
    {
    }

    // A thread that has nothing to do with the interpreter may call the API while the interpreter ends (a thread of C's that gives back a reference). It reads
    // s_interpreter, and then asks who holds the lock, before it looks at anything else. So this object is never freed, and the lock outlives the VM.
    JSGlobalObject* const globalObject;
    VM& vm;
    Runtime& runtime;
    State* const state;
    const Ref<JSLock> lock;
    // See t_thisThread.
    const pthread_key_t keyOfReferenceToThread;
    static pthread_key_t makeKeyOfReferenceToThread();
    CountTable counts;

    // See CAPI_QUIET.
    ALWAYS_INLINE void noteNoisyCall()
    {
        if (finalizersOfCBeingServed) [[unlikely]]
            ++noise;
    }
    // Nobody sets the first yet, and nobody reads the second: they are for the order in which instances of extension types are destroyed.
    unsigned finalizersOfCBeingServed { 0 };
    uint32_t noise { 0 };
};

// Null until the API is set up, and again when its interpreter has ended. Any thread may read it, at any time: see Interpreter::lock. Whoever follows the pointer without the GIL
// has to see what the constructor wrote: it is stored with release, here and in s_interpreterThatLetsGo, and what is read is read through the pointer (an address dependency, which
// orders the two loads on every processor that JSC runs on).
extern std::atomic<Interpreter*> s_interpreter;

// For the runtime of the interpreter that the API is set up for (isSetUpFor()), with the GIL, and for the collector of its VM. Whom C calls has `current`.
ALWAYS_INLINE Interpreter& interpreter()
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    ASSERT(current);
    return *current;
}

ALWAYS_INLINE PyType* typeOf(Handle object)
{
    if (Body* body = bodyOrNull(object)) [[unlikely]]
        return body->type();
    return interpreter().runtime.typeOf(toLookAt(object));
}

// ---- Who may come in ----

// Who holds the GIL is for JSLock to say. What costs in asking is to find out which thread is asking (on Linux, a call of pthread_getspecific()), so each thread keeps
// that: null until it has asked once, and again when it is about to end. While this points to a Thread, a reference to it is held: it is there, and no other thread's
// lies at its address. (The reference is the value of a key of pthreads. Its destructor clears this first.)
// It is null as well while the thread runs C for an interpreter that the API is not for (CRunsFor): that may be one of the same VM, which has the same lock. So who is to be
// refused on that ground takes the long way, and who is let in pays nothing for the question.
extern constinit thread_local Thread* t_thisThread;
bool hasGILSlow(Interpreter&);
ALWAYS_INLINE bool hasGIL(Interpreter* current)
{
    if (!current) [[unlikely]]
        return false;
    Thread* thread = t_thisThread;
    if (thread && current->lock->isHeldBy(*thread)) [[likely]]
        return true;
    return hasGILSlow(*current);
}

// EVERY FUNCTION THAT LOOKS AT AN OBJECT, AT THE TABLE OF COUNTS OR AT THE INTERPRETER ASKS THIS FIRST. A thread without the GIL would read what another thread is changing, and when
// the interpreter has ended, or before there is one, there is nothing to look at. In CPython all of that is undefined. Here the function fails: it returns what it returns
// when it fails, and sets no exception. A thread without the GIL is told of, once, on stderr, and so is one that runs C for an interpreter that the API is not for.
// Null: the caller is refused.
void didRefuseToEnter(Interpreter*);
ALWAYS_INLINE Interpreter* interpreterToEnter()
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (hasGIL(current)) [[likely]]
        return current;
    didRefuseToEnter(current);
    return nullptr;
}

// ---- References ----

// With the GIL, as everything here. A thread without it (one that C made and that has not attached itself, one between Py_BEGIN_ALLOW_THREADS and
// Py_END_ALLOW_THREADS) counts as stopped: it would change the table under the thread that has the GIL, or under the collector. In CPython that
// spoils a count. Here it would spoil the table, so it is refused: the count stays as it is, and the program is told, once, on stderr.
//  - A reference that was to be given back is held for ever.
//  - A reference that was to be taken is one that C believes in and has not got, and nobody can tell which Py_DECREF will be the one that gives it back.
//    So FROM THEN ON NOTHING IS LET GO OF: all that C owns, or comes to own, is held for ever, and nothing that C rightly owns dies under it. The counts go on falling, for they
//    are asked about (Py_REFCNT(), PyTuple_SetItem()); it is the entry that stays when the count has come to 0.
// A thread that runs C for ANOTHER interpreter (CRunsFor: it calls a function with ctypes, loads a library or lets go of one) is refused too, and that is all: it costs the interpreter
// that has the API nothing. What ctypes hands to C there are that interpreter's own objects, and that C takes a reference to one says nothing about this one's. No function gives
// such a thread an object of this interpreter. What it can name is what a library that both have loaded keeps in a variable of its own: to share that between two interpreters
// is an error in CPython as well. (One more way: it calls the code of a callback of this interpreter's, whose address it has got as a number. That gives a reference with its result, as
// to any C, and this C cannot give it back: the object is held for ever.)
// (When the interpreter has ended there is nothing left to count: what an extension gives back as the process goes is ignored.)
void refuseToChangeCount(bool isToFall);
void releaseSlow(JSCell*, Interpreter* thatLetsGo);
// s_interpreter, until a reference that was to be taken has been refused.
extern std::atomic<Interpreter*> s_interpreterThatLetsGo;

// THE QUESTION IS FOR C. Who has asked and been let in does not ask again, and the runtime does not ask at all where it keeps the books of a call that it has made itself (the hooks of
// ctypes in CApiHooks.h): for whom C runs further out on the stack has nothing to do with whose reference the result of this call came with.
ALWAYS_INLINE void retain(Interpreter& current, Handle handle)
{
    if (JSCell* cell = countedCell(handle))
        current.counts.add(cell);
}

ALWAYS_INLINE void release(Interpreter& current, Handle handle)
{
    if (JSCell* cell = countedCell(handle))
        current.counts.remove(cell, s_interpreterThatLetsGo.load(std::memory_order_relaxed));
}

// Py_INCREF(), Py_DECREF().
ALWAYS_INLINE void retain(Handle handle)
{
    JSCell* cell = countedCell(handle);
    if (!cell)
        return;
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (hasGIL(current)) [[likely]]
        current->counts.add(cell);
    else
        refuseToChangeCount(false);
}

ALWAYS_INLINE void release(Handle handle)
{
    JSCell* cell = countedCell(handle);
    if (!cell)
        return;
    Interpreter* current = s_interpreterThatLetsGo.load(std::memory_order_relaxed);
    if (hasGIL(current)) [[likely]]
        current->counts.remove(cell, true);
    else
        releaseSlow(cell, current);
}

// Somebody else has a reference to the object now, and the API has seen it: C has handed one on (to a container that takes it over, to the program with a result or with what it
// raised), or a container has taken one of its own. See PyTuple_SetItem().
enum class Holder : bool { Container, Program };
ALWAYS_INLINE void didGetHolder(JSValue value, Holder holder)
{
    if (auto* tuple = PyTuple::tryFrom(value)) [[unlikely]]
        tuple->addHolderThatCKnowsOf(holder == Holder::Program);
}

// A reference that a function takes over from C ("steals"), whether it succeeds or not: given up when the function returns, by which time
// whatever was to hold the object does.
class TakenOver {
    WTF_MAKE_NONCOPYABLE(TakenOver);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    TakenOver(Interpreter& current, Handle handle)
        : m_interpreter(current)
        , m_handle(handle)
    {
    }
    ~TakenOver() { release(m_interpreter, m_handle); }

private:
    Interpreter& m_interpreter;
    Handle m_handle;
};

// A reference that C will own. The empty value gives NULL.
ALWAYS_INLINE Handle newReference(Interpreter& current, JSValue value)
{
    Handle handle = toC(value);
    retain(current, handle);
    return handle;
}

// ---- The error indicator ----
//
// C goes on calling the API with an exception set, and the functions of the runtime expect none to be pending when they are entered. So what C
// knows as the error indicator is not the VM's pending exception: it is a reference of its own, one for each thread, which is counted as
// C's references are and is kept alive by that. An exception moves from one to the other where control does: see Entry, throwWhatCRaised().
// What ends the program (a termination) is never in the indicator. It stays pending, and while it is, every function fails.
// WHEN THE INTERPRETER ENDS nobody clears the indicators: they are the threads' own. What is left in one is a pointer to a cell that nothing holds any more. It is never followed: whoever
// reads it to look at the object has asked who calls, and is refused from then on (PyErr_Occurred(), PyErr_ExceptionMatches(), PyErr_GetRaisedException()); whoever has called C looks only
// if its interpreter is the one that the API is set up for (throwWhatCRaised() and the like), which none is any more; and to put another in its place gives the old one to
// release(), which looks at its bits and at s_interpreterThatLetsGo and s_interpreter, which are null. The API never comes back, so no later interpreter can take the pointer for one of its own.
Handle& errorIndicator();
// Takes over the reference. NULL: none is set any more.
void setErrorIndicator(Interpreter&, Handle);

// ---- A function that C calls ----
//
//     PY_CAPI Handle PyObject_Repr(Handle object)
//     {
//         CAPI_ENTER(nullptr);
//         return entry.newReference(reprObject(entry.globalObject, toJS(object)));
//     }
//
// The argument is what the function returns when it fails. The thread has the GIL: the API demands it, and these ask (interpreterToEnter()). What they are told is `current`.
//
// Every function that C calls says in its first line which of two kinds it is:
//  - CAPI_QUIET(failure): it can neither run code of the program nor put a reference where the program finds it. There are few of these, and they are what
//    a tp_dealloc consists of: counts, the type and the data of an object, memory, the error indicator put aside and back.
//    CAPI_QUIET_AND_ASKS_NOBODY(), for those of them that look at nothing but the bits of their arguments and what is the thread's own: any thread may call them, at any time.
//    (retain() and release() ask for themselves.)
//  - CAPI_ENTER(failure), or CAPI_NOISY(failure) where there is nothing to enter the runtime for: all others. They are counted, as they begin, while a finalizer
//    of C's is being served. (CAPI_ENTER_AFTER_ALL(failure): further down in a function that has asked, on the way that does enter the runtime. Whoever enters is counted: Entry does it.)
// What is done with the count: see "The C API" in ../README.md. A function is noisy unless it is known to be quiet; to take a quiet one for
// noisy costs time, the other way round is an error. ../tools/check_capi.py looks for the first lines, and that what asks nobody does not name the interpreter.
class Entry {
    WTF_MAKE_NONCOPYABLE(Entry);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // The scope is a variable of the function's own (CAPI_ENTER_AFTER_ALL()), made where it stands. In a build that verifies them a scope puts its own address into the VM, so it is not to be moved.
    Entry(Interpreter& current, TopExceptionScope& scope)
        : globalObject(current.globalObject)
        , vm(current.vm)
        , scope(scope)
        , m_interpreter(current)
    {
        ASSERT(vm.currentThreadIsHoldingAPILock());
        current.noteNoisyCall();
    }

    Runtime& runtime() const { return m_interpreter.runtime; }

    // Whether the runtime has raised. Then the exception is in the indicator, and the function returns what says so.
    ALWAYS_INLINE bool failed()
    {
        if (!scope.exception()) [[likely]]
            return false;
        moveExceptionToIndicator();
        return true;
    }

    // For the result of a function of the runtime that returns the empty value exactly when it has raised.
    ALWAYS_INLINE Handle newReference(JSValue value)
    {
        if (failed()) [[unlikely]]
            return nullptr;
        ASSERT(value);
        return CApi::newReference(m_interpreter, value);
    }
    // 0, or -1 if the runtime has raised.
    ALWAYS_INLINE int status() { return failed() ? -1 : 0; }

    // raise type(message)
    void raise(PyType*, const String& message);
    // What CPython raises where a function is handed what it cannot be handed.
    void raiseBadInternalCall();

    JSGlobalObject* const globalObject;
    VM& vm;
    TopExceptionScope& scope;

private:
    void moveExceptionToIndicator();

    Interpreter& m_interpreter;
};

#define CAPI_QUIET_AND_ASKS_NOBODY() do { } while (false)
#define CAPI_QUIET(failure) \
    [[maybe_unused]] JSC::Python::CApi::Interpreter* const current = JSC::Python::CApi::interpreterToEnter(); \
    if (!current) [[unlikely]] \
        return failure
#define CAPI_NOISY(failure) \
    CAPI_QUIET(failure); \
    current->noteNoisyCall()
// Nothing but a termination can be pending here.
#define CAPI_ENTER_AFTER_ALL(failure) \
    auto scopeOfEntry = DECLARE_TOP_EXCEPTION_SCOPE(current->vm); \
    JSC::Python::CApi::Entry entry(*current, scopeOfEntry); \
    if (entry.scope.exception()) [[unlikely]] \
        return failure
#define CAPI_ENTER(failure) \
    CAPI_QUIET(failure); \
    CAPI_ENTER_AFTER_ALL(failure)

// ---- The files of this directory ----
//
// A linker takes a member of an archive if something refers to it, and nothing in bun refers to a function that only C calls. So each file has
// CAPI_FILE(Name) in it, and ensure() refers to them all.
#define FOR_EACH_CAPI_FILE(v) \
    v(Errors) \
    v(Life) \
    v(Numbers) \
    v(Object) \
    v(References) \
    v(Sequences) \
    v(Type)

#define CAPI_FILE(name) \
    void keep##name() { }

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
