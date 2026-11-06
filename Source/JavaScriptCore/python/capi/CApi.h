/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CApiCounts.h"
#include "CApiHooks.h"
#include "PyErrors.h"
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
// Two kinds of value are spelled otherwise for C than in the heap:
//  - A built-in type that C names by a symbol (&PyLong_Type). The address is fixed when bun is linked, and the cell is made when the
//    program runs. All these symbols lie in one section (StaticObject). What comes in is tested for that range; a type that goes
//    out is asked for its symbol (TypeExtension::symbol). The program sees the same number: it is the type's id() (bitsForC()).
//  - null, which only JS makes and which is None as undefined is. C compares with Py_None: it gets undefined.
struct Opaque;
using Handle = Opaque*;

// What a symbol such as PyLong_Type or _Py_NoneStruct is. C takes its address and nothing else.
struct alignas(16) StaticObject {
    EncodedJSValue value; // Of what the runtime itself keeps alive for as long as it lives.
    uint64_t unused;
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

// What C links with. `used`: nothing in bun refers to most of it. (The names are C's whatever namespace the definitions are in.)
#define PY_CAPI extern "C" __attribute__((visibility("default"), used))
#define PY_CAPI_DATA(type, name) extern "C" { __attribute__((visibility("default"), used)) type name; }
#define PY_CAPI_STATIC_OBJECT(name) extern "C" { __attribute__((visibility("default"), used, section(PY_CAPI_STATIC_SECTION))) JSC::Python::CApi::StaticObject name; }

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
    return JSValue::decode(static_cast<EncodedJSValue>(bitsOf(handle)));
}

// The cell whose references are counted. Null: the object is not a cell, or one that the runtime keeps alive anyway.
ALWAYS_INLINE JSCell* countedCell(Handle handle)
{
    if ((bitsOf(handle) & JSValue::NotCellMask) || isStatic(handle))
        return nullptr;
    return std::bit_cast<JSCell*>(handle);
}

// What the C API knows about a type, for the types about which it knows something: PyType::extension().
class TypeExtension final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
    }
    static TypeExtension* create(VM&, Structure*);

    static TypeExtension* of(PyType* type) { return static_cast<TypeExtension*>(type->extension()); }

    // The symbol that C knows the type by. Null: none, and the address of the cell is what C gets.
    StaticObject* symbol { nullptr };

private:
    TypeExtension(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

ALWAYS_INLINE Handle toC(JSValue value)
{
    EncodedJSValue bits = JSValue::encode(value);
    if (!(bits & JSValue::NotCellMask)) {
        if (bits && value.asCell()->type() == InternalFunctionType) [[unlikely]] {
            if (PyType* type = PyType::tryFrom(value.asCell())) {
                if (auto* extension = TypeExtension::of(type); extension && extension->symbol)
                    return std::bit_cast<Handle>(extension->symbol);
            }
        }
    } else if (bits == JSValue::ValueNull) [[unlikely]]
        bits = JSValue::ValueUndefined;
    return std::bit_cast<Handle>(static_cast<uintptr_t>(bits));
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

    WriteBarrier<Structure> typeExtensionStructure;

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

    // See CAPI_QUIET().
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
// has to see what the constructor wrote: it is stored with release, here and in s_interpreterWhoseCountsFall, and what is read is read through the pointer (an address dependency, which
// orders the two loads on every processor that JSC runs on).
extern std::atomic<Interpreter*> s_interpreter;

// For who has asked mayEnter(), or is the collector of the interpreter's VM.
ALWAYS_INLINE Interpreter& interpreter()
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    ASSERT(current);
    return *current;
}

// ---- Who may come in ----

// Who holds the GIL is for JSLock to say. What costs in asking is to find out which thread is asking (on Linux, a call of pthread_getspecific()), so each thread keeps
// that: null until it has asked once, and again when it is about to end. While this points to a Thread, a reference to it is held: it is there, and no other thread's
// lies at its address. (The reference is the value of a key of pthreads. Its destructor clears this first.)
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
bool mayEnterSlow(Interpreter*);
ALWAYS_INLINE bool mayEnter()
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (hasGIL(current)) [[likely]]
        return true;
    return mayEnterSlow(current);
}

// ---- References ----

// With the GIL, as everything here. A thread without it (one that C made and that has not attached itself, one between Py_BEGIN_ALLOW_THREADS and
// Py_END_ALLOW_THREADS) counts as stopped: it would change the table under the thread that has the GIL, or under the collector. In CPython that
// spoils a count. Here it would spoil the table, so it is refused: the count stays as it is, and the program is told, once, on stderr.
//  - A reference that was to be given back is held for ever.
//  - A reference that was to be taken is one that C believes in and has not got, and nobody can tell which Py_DECREF will be the one that gives it back.
//    So FROM THEN ON NO COUNT FALLS: all that C owns, or comes to own, is held for ever, and nothing that C rightly owns dies under it.
// A thread that runs C for ANOTHER interpreter (a Worker's, that calls a function with ctypes or loads a library) is refused too, and that is all: it costs the interpreter that has the
// API nothing. No function gives such a thread an object of this interpreter. What it can name are the exceptions (PyExc_ValueError), which the runtime keeps alive whatever
// the count says, and what a library that both have loaded keeps in a variable of its own: to share that between two interpreters is an error in CPython as well.
// (When the interpreter has ended there is nothing left to count: what an extension gives back as the process goes is ignored.)
void refuseToChangeCount(bool isToFall);
// s_interpreter, until a reference that was to be taken has been refused.
extern std::atomic<Interpreter*> s_interpreterWhoseCountsFall;

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
    Interpreter* current = s_interpreterWhoseCountsFall.load(std::memory_order_relaxed);
    if (hasGIL(current)) [[likely]]
        current->counts.remove(cell);
    else
        refuseToChangeCount(true);
}

// A reference that a function takes over from C ("steals"), whether it succeeds or not: given up when the function returns, by which time
// whatever was to hold the object does.
class TakenOver {
    WTF_MAKE_NONCOPYABLE(TakenOver);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit TakenOver(Handle handle)
        : m_handle(handle)
    {
    }
    ~TakenOver()
    {
        if (m_handle)
            release(m_handle);
    }

private:
    Handle m_handle;
};

// A reference that C will own. The empty value gives NULL.
ALWAYS_INLINE Handle newReference(JSValue value)
{
    Handle handle = toC(value);
    if (handle)
        retain(handle);
    return handle;
}

// ---- The error indicator ----
//
// C goes on calling the API with an exception set, and the functions of the runtime expect none to be pending when they are entered. So what C
// knows as the error indicator is not the VM's pending exception: it is a reference of its own, one for each thread, which is counted as
// C's references are and is kept alive by that. An exception moves from one to the other where control does: see Entry, throwWhatCRaised().
// What ends the program (a termination) is never in the indicator. It stays pending, and while it is, every function fails.
Handle& errorIndicator();
// Takes over the reference. NULL: none is set any more.
void setErrorIndicator(Handle);

// ---- A function that C calls ----
//
//     PY_CAPI Handle PyObject_Repr(Handle object)
//     {
//         CAPI_ENTER(nullptr);
//         return entry.newReference(reprObject(entry.globalObject, toJS(object)));
//     }
//
// The argument is what the function returns when it fails. The thread has the GIL: the API demands it, and these ask (mayEnter()).
//
// Every function that C calls says in its first line which of two kinds it is:
//  - CAPI_QUIET(): it can neither run code of the program nor put a reference where the program finds it. There are few of these, and they are what
//    a tp_dealloc consists of: counts, the type and the data of an object, memory, the error indicator put aside and back.
//  - CAPI_ENTER(failure), or CAPI_NOISY(failure) where there is nothing to enter the runtime for: all others. They are counted, as they begin, while a finalizer
//    of C's is being served.
// What is done with the count: see "The C API" in ../README.md. A function is noisy unless it is known to be quiet; to take a quiet one for
// noisy costs time, the other way round is an error. ../tools/check_capi.py looks for the first lines.
class Entry {
    WTF_MAKE_NONCOPYABLE(Entry);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit Entry(TopExceptionScope&& scope)
        : globalObject(interpreter().globalObject)
        , vm(interpreter().vm)
        , scope(WTF::move(scope))
    {
        ASSERT(vm.currentThreadIsHoldingAPILock());
        interpreter().noteNoisyCall();
    }

    Runtime& runtime() const { return interpreter().runtime; }

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
        return CApi::newReference(value);
    }
    // 0, or -1 if the runtime has raised.
    ALWAYS_INLINE int status() { return failed() ? -1 : 0; }

    // raise type(message)
    void raise(PyType*, const String& message);
    // What CPython raises where a function is handed what it cannot be handed.
    void raiseBadInternalCall();

    JSGlobalObject* const globalObject;
    VM& vm;
    TopExceptionScope scope;

private:
    void moveExceptionToIndicator();
};

#define CAPI_QUIET() do { } while (false)
#define CAPI_NOISY(failure) \
    if (!JSC::Python::CApi::mayEnter()) [[unlikely]] \
        return failure; \
    JSC::Python::CApi::interpreter().noteNoisyCall()
// Nothing but a termination can be pending here.
#define CAPI_ENTER(failure) \
    if (!JSC::Python::CApi::mayEnter()) [[unlikely]] \
        return failure; \
    JSC::Python::CApi::Entry entry(DECLARE_TOP_EXCEPTION_SCOPE(JSC::Python::CApi::interpreter().vm)); \
    if (entry.scope.exception()) [[unlikely]] \
        return failure

// ---- The files of this directory ----
//
// A linker takes a member of an archive if something refers to it, and nothing in bun refers to a function that only C calls. So each file has
// CAPI_FILE(Name) in it, and ensure() refers to them all.
#define FOR_EACH_CAPI_FILE(v) \
    v(Errors) \
    v(Numbers) \
    v(Object) \
    v(References) \
    v(Sequences)

#define CAPI_FILE(name) \
    void keep##name() { }

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
