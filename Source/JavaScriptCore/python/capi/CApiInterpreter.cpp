/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "CApiInstance.h"
#include "CApiSymbols.h"
#include "CApiType.h"
#include "JSCInlines.h"
#include "PyException.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"
#include <wtf/TZoneMallocInlines.h>

#if PYTHON_HAS_CAPI

namespace JSC {
namespace Python {
namespace CApi {

// ---- The symbols that stand for objects ----

// Each says which it is. (Where the linker puts them is its own business.)
enum SymbolNumber : uint64_t {
#define DEFINE_NUMBER(symbol, id) NumberOf##symbol,
    FOR_EACH_CAPI_STATIC_TYPE(DEFINE_NUMBER)
#undef DEFINE_NUMBER
#define DEFINE_NUMBER(name) NumberOf##name,
    FOR_EACH_CAPI_EXCEPTION(DEFINE_NUMBER)
#undef DEFINE_NUMBER
    numberOfTypesWithSymbols,
    NumberOfNone = numberOfTypesWithSymbols,
    NumberOfFalse,
    NumberOfTrue,
    NumberOfEllipsis,
    NumberOfNotImplemented,
    numberOfSymbols
};

#define DEFINE_STATIC_TYPE(symbol, id) PY_CAPI_STATIC_OBJECT(symbol, 0, NumberOf##symbol)
FOR_EACH_CAPI_STATIC_TYPE(DEFINE_STATIC_TYPE)
#undef DEFINE_STATIC_TYPE

// The headers for abi3t ask Py_GetConstantBorrowed() for these five. Three are no cells, and are what they are at all times.
PY_CAPI_STATIC_OBJECT(_Py_NoneStruct, JSValue::ValueUndefined, NumberOfNone)
PY_CAPI_STATIC_OBJECT(_Py_FalseStruct, JSValue::ValueFalse, NumberOfFalse)
PY_CAPI_STATIC_OBJECT(_Py_TrueStruct, JSValue::ValueTrue, NumberOfTrue)
PY_CAPI_STATIC_OBJECT(_Py_EllipsisObject, 0, NumberOfEllipsis)
PY_CAPI_STATIC_OBJECT(_Py_NotImplementedStruct, 0, NumberOfNotImplemented)

// PyExc_ValueError is a variable. A library may read it as it is loaded, whoever loads it and whenever (JS, by bun:ffi, before there is any Python), and keep what it has read. So what
// it holds is good from before main() to the end of the process, and is never written: the address of a symbol like the others, which nobody names.
#define DEFINE_EXCEPTION(name) \
    extern "C" { \
    __attribute__((visibility("hidden"), PY_CAPI_IN_STATIC_SECTION)) constinit StaticObject pyExceptionSymbol##name { 0, NumberOf##name }; \
    extern __attribute__((visibility("default"))) StaticObject* const PyExc_##name; \
    __attribute__((used)) constinit StaticObject* const PyExc_##name = &pyExceptionSymbol##name; \
    }
FOR_EACH_CAPI_EXCEPTION(DEFINE_EXCEPTION)
#undef DEFINE_EXCEPTION
extern "C" {
extern __attribute__((visibility("default"))) StaticObject* const PyExc_EnvironmentError;
extern __attribute__((visibility("default"))) StaticObject* const PyExc_IOError;
__attribute__((used)) constinit StaticObject* const PyExc_EnvironmentError = &pyExceptionSymbolOSError;
__attribute__((used)) constinit StaticObject* const PyExc_IOError = &pyExceptionSymbolOSError;
}

// This interpreter's.
static PyType* typeWithSymbol(Runtime& rt, uint64_t number)
{
    switch (number) {
#define RETURN_STATIC_TYPE(symbol, id) \
    case NumberOf##symbol: \
        return rt.id##Type();
        FOR_EACH_CAPI_STATIC_TYPE(RETURN_STATIC_TYPE)
#undef RETURN_STATIC_TYPE
#define RETURN_EXCEPTION(name) \
    case NumberOf##name: \
        return rt.name();
        FOR_EACH_CAPI_EXCEPTION(RETURN_EXCEPTION)
#undef RETURN_EXCEPTION
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Function>
static void forEachSymbol(const Function& function)
{
    RELEASE_ASSERT(&pyStaticObjectsEnd - &pyStaticObjectsBegin == numberOfSymbols);
    for (StaticObject* symbol = &pyStaticObjectsBegin; symbol != &pyStaticObjectsEnd; ++symbol)
        function(*symbol);
}

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Interpreter);

const ClassInfo State::s_info = { "PyCApiState"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(State) };

State* State::create(VM& vm, Structure* structure)
{
    auto* state = new (NotNull, allocateCell<State>(vm)) State(vm, structure);
    state->finishCreation(vm);
    return state;
}

template<typename Visitor>
void State::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<State>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->madeByExtensionStructure);
    visitor.append(thisObject->faceStructure);
    visitor.append(thisObject->bodyStructure);
}

DEFINE_VISIT_CHILDREN(State);

// ---- The one interpreter ----

std::atomic<Interpreter*> s_interpreter;
std::atomic<const Runtime*> s_runtimeItIsFor;
std::atomic<const Runtime*> s_runtimeItIsSetUpFor;

static void setSymbols(Runtime& rt)
{
    forEachSymbol([&](StaticObject& symbol) {
        if (symbol.number < numberOfTypesWithSymbols)
            symbol.value = JSValue::encode(typeWithSymbol(rt, symbol.number));
    });
    _Py_EllipsisObject.value = JSValue::encode(rt.ellipsis());
    _Py_NotImplementedStruct.value = JSValue::encode(rt.notImplemented());
}

static void clearSymbols()
{
    forEachSymbol([&](StaticObject& symbol) {
        if (JSValue::decode(symbol.value).isCell())
            symbol.value = 0;
    });
}

static void addMarkingConstraint(VM&);

#define DECLARE_FILE(name) void keep##name();
FOR_EACH_CAPI_FILE(DECLARE_FILE)
#undef DECLARE_FILE

bool ensure(JSGlobalObject* globalObject)
{
    if (tryEnsure(globalObject)) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseImportError(globalObject, scope, "the C API serves the first Python interpreter of a process, and that is not this one"_s);
    return false;
}

void runtimeIsBeginning(Runtime& runtime)
{
    // Once and for all.
    static std::atomic<bool> isSettled;
    if (!isSettled.exchange(true))
        s_runtimeItIsFor.store(&runtime, std::memory_order_relaxed);
}

void didMakeTypes(Runtime& runtime)
{
    forEachSymbol([&](StaticObject& symbol) {
        if (symbol.number < numberOfTypesWithSymbols)
            typeWithSymbol(runtime, symbol.number)->setSymbolForC(&symbol);
    });
}

bool tryEnsure(JSGlobalObject* globalObject)
{
    if (isSetUpFor(runtime(globalObject))) [[likely]]
        return true;
    if (!isFor(runtime(globalObject)))
        return false;

    VM& vm = getVM(globalObject);
    ASSERT(vm.currentThreadIsHoldingAPILock());
#define KEEP_FILE(name) keep##name();
    FOR_EACH_CAPI_FILE(KEEP_FILE)
#undef KEEP_FILE

    State* state = State::create(vm, State::createStructure(vm, globalObject));
    runtime(globalObject).setSlot(Runtime::Slot::CApi, state);
    state->madeByExtensionStructure.set(vm, state, MadeByExtension::createStructure(vm, globalObject));
    state->faceStructure.set(vm, state, Face::createStructure(vm, globalObject));
    state->bodyStructure.set(vm, state, Body::createStructure(vm, globalObject));
    auto* interpreter = new Interpreter(globalObject, state);
    setSymbols(interpreter->runtime);
    // In this order: a reference can be refused, which clears the first, once the second is there.
    s_interpreterThatLetsGo.store(interpreter, std::memory_order_release);
    s_interpreter.store(interpreter, std::memory_order_release);
    s_runtimeItIsSetUpFor.store(&interpreter->runtime, std::memory_order_relaxed);
    // It stays with the heap, which may outlive the runtime.
    addMarkingConstraint(vm);
    return true;
}

// The collector calls this too, with the world stopped, for the interpreter of a realm whose global object it has found dead: no cell is made here, no lock is taken, nothing of the
// program's runs. From then on the global object and all that only it held are dead, though not swept, and C that JS calls on that thread could still come in.
//  - Nobody is in the middle of a call then: a function that has entered has the global object in its Entry, on the stack, and that marks it.
//  - An object that is in the table holds the global object by its Structure, so the realm does not die while C owns one (nor ever, if C has owned one since a reference was refused: nothing is
//    let go of then). What may still be there are cells that belong to no realm, a str or a large int. They die when nothing else holds them, and nothing follows C's pointers to them any more:
//    every function refuses.
//  - The indicator of a thread may still hold a pointer. Nothing follows that either: see errorIndicator().
void runtimeIsEnding(Runtime& runtime)
{
    if (!isFor(runtime))
        return;
    s_runtimeItIsFor.store(nullptr, std::memory_order_relaxed);
    if (!isSetUpFor(runtime))
        return;
    s_runtimeItIsSetUpFor.store(nullptr, std::memory_order_relaxed);
    // Not freed: see Interpreter::lock. In this order: who finds the second null looks at the first, and what a library's thread gives back as the interpreter goes is nobody's mistake.
    Interpreter* interpreter = s_interpreter.exchange(nullptr, std::memory_order_relaxed);
    s_interpreterThatLetsGo.store(nullptr, std::memory_order_relaxed);
    interpreter->counts.clear();
    clearSymbols();
}

// ---- The collector ----

// What C holds is reachable, as what a global variable of the program holds is. The collector learns of it as it learns of an embedder's other
// roots: by a marking constraint, which it evaluates, with the world stopped, until nothing new comes of it.
template<typename Visitor>
static void markWhatCHolds(Visitor& visitor)
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (!current)
        return;
    SetRootMarkReasonScope rootScope(visitor, RootMarkReason::StrongReferences);
    current->counts.forEachCell([&](JSCell* cell) {
        visitor.appendUnbarriered(cell);
    });
}

static void addMarkingConstraint(VM& vm)
{
    vm.heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>("Pyc"_s, "Python C API"_s,
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([](auto& visitor) {
            markWhatCHolds(visitor);
        })),
        ConstraintVolatility::GreyedByExecution, ConstraintConcurrency::Sequential));
}

void didFinishMarking(JSC::Heap& heap)
{
#if ASSERT_ENABLED
    interpreter().counts.forEachCell([&](JSCell* cell) {
        ASSERT(heap.isMarked(cell));
    });
#else
    UNUSED_PARAM(heap);
#endif
}

// ---- References ----

constinit thread_local Thread* t_thisThread = nullptr;
static constinit thread_local bool t_thisThreadIsEnding = false;

// The reference that goes with t_thisThread is the value of a key of pthreads, whose destructor gives it back. Not a thread_local with a destructor: Bun is compiled with
// -fno-c++-static-destructors, which leaves those out; and to register one takes the loader's lock in glibc, which a thread that is in dlopen() holds.
static void thisThreadIsEnding(void* thread)
{
    t_thisThread = nullptr;
    t_thisThreadIsEnding = true;
    static_cast<Thread*>(thread)->deref();
}

pthread_key_t Interpreter::makeKeyOfReferenceToThread()
{
    pthread_key_t key;
    RELEASE_ASSERT(!pthread_key_create(&key, thisThreadIsEnding));
    return key;
}

// Compared, never followed.
static constinit thread_local const Runtime* t_cRunsFor = nullptr;

static ALWAYS_INLINE void setWhomCRunsFor(const Runtime* runtime)
{
    t_cRunsFor = runtime;
    // See t_thisThread. (If the API is set up later, that is by code further in, which comes by here on its way out.)
    if (runtime && !isSetUpFor(*runtime))
        t_thisThread = nullptr;
}

CRunsFor::CRunsFor(const Runtime& runtime)
    : m_previous(t_cRunsFor)
{
    setWhomCRunsFor(&runtime);
}

CRunsFor::~CRunsFor()
{
    setWhomCRunsFor(m_previous);
}

static bool runsCForAnotherInterpreter(Interpreter& current)
{
    return t_cRunsFor && t_cRunsFor != &current.runtime;
}

// The first time in a thread, and whenever the answer is no: nothing is refused on the word of what a thread has kept.
bool hasGILSlow(Interpreter& current)
{
    if (runsCForAnotherInterpreter(current))
        return false;
    // What else is destroyed with the thread comes here too, and may come later. Nothing is kept for it.
    if (t_thisThreadIsEnding)
        return current.lock->currentThreadIsHoldingLock();
    if (!t_thisThread) {
        t_thisThread = static_cast<Thread*>(pthread_getspecific(current.keyOfReferenceToThread));
        if (!t_thisThread) {
            Thread& thread = Thread::currentSingleton();
            // (No room for the value: then nothing is kept.)
            if (pthread_setspecific(current.keyOfReferenceToThread, &thread))
                return current.lock->isHeldBy(thread);
            thread.ref();
            t_thisThread = &thread;
        }
    }
    return current.lock->isHeldBy(*t_thisThread);
}

static void tellOnce(std::atomic<bool>& hasTold, const char* what)
{
    if (!hasTold.exchange(true))
        fputs(what, stderr);
}

void didRefuseToEnter(Interpreter* current)
{
    // What a library does when the interpreter has ended, as the process goes, is nobody's mistake. Nor is it one to ask before there is anything to ask about.
    if (!current)
        return;
    static std::atomic<bool> hasToldOfOther;
    static std::atomic<bool> hasTold;
    if (runsCForAnotherInterpreter(*current))
        tellOnce(hasToldOfOther, "Python: a function of the C API was called for an interpreter that the C API does not serve (it serves the first of the process); it fails\n");
    else
        tellOnce(hasTold, "Python: a function of the C API was called by a thread that does not hold the GIL; it fails\n");
}

std::atomic<Interpreter*> s_interpreterThatLetsGo;

void refuseToChangeCount(bool isToFall)
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (!current)
        return;
    static std::atomic<bool> hasToldOfOther;
    static std::atomic<bool> hasToldOfTaken;
    static std::atomic<bool> hasToldOfGivenBack;
    if (runsCForAnotherInterpreter(*current)) {
        tellOnce(hasToldOfOther, "Python: Py_INCREF or Py_DECREF was called for an interpreter that the C API does not serve (it serves the first of the process); the count stays as it was\n");
        return;
    }
    if (!isToFall) {
        s_interpreterThatLetsGo.store(nullptr, std::memory_order_relaxed);
        tellOnce(hasToldOfTaken, "Python: Py_INCREF was called by a thread that does not hold the GIL; the count stays as it was, and from now on whatever C owns is held for ever\n");
        return;
    }
    tellOnce(hasToldOfGivenBack, "Python: Py_DECREF was called by a thread that does not hold the GIL; the count stays as it was: the object is held for ever\n");
}

// Not the way of a thread that has the GIL while all is well. `thatLetsGo`: what the caller has read. If there is one, it has found that the thread has not got the GIL.
void releaseSlow(JSCell* cell, Interpreter* thatLetsGo)
{
    Interpreter* current = thatLetsGo ? nullptr : s_interpreter.load(std::memory_order_relaxed);
    if (hasGIL(current))
        current->counts.remove(cell, false);
    else
        refuseToChangeCount(true);
}

// ---- The error indicator ----

static thread_local Handle t_errorIndicator;

Handle& errorIndicator()
{
    return t_errorIndicator;
}

void setErrorIndicator(Interpreter& current, Handle exception)
{
    release(current, std::exchange(t_errorIndicator, exception));
}

void Entry::moveExceptionToIndicator()
{
    // Empty: it is a termination, and goes on.
    if (JSValue exception = takePendingException(globalObject))
        setErrorIndicator(m_interpreter, CApi::newReference(m_interpreter, exception));
}

void Entry::raise(PyType* type, const String& message)
{
    {
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        Python::raise(globalObject, throwScope, type, message);
    }
    bool hasFailed = failed();
    ASSERT_UNUSED(hasFailed, hasFailed);
}

void Entry::raiseBadInternalCall()
{
    {
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        Python::raiseBadInternalCall(globalObject, throwScope);
    }
    bool hasFailed = failed();
    ASSERT_UNUSED(hasFailed, hasFailed);
}

// C can put anything there (PyErr_SetRaisedException()), and the program gets it.
static JSValue toThrow(Handle exception)
{
    JSValue value = toJS(exception);
    didGetHolder(value, Holder::Program);
    return value;
}

bool throwWhatCRaised(JSGlobalObject* globalObject)
{
    Handle exception = std::exchange(t_errorIndicator, nullptr);
    if (!exception) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwPythonException(globalObject, scope, toThrow(exception));
    release(interpreter(), exception);
    return false;
}

bool throwWhatCRaisedBesideResult(JSGlobalObject* globalObject, const String& who)
{
    Handle exception = std::exchange(t_errorIndicator, nullptr);
    if (!exception) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue cause = toThrow(exception);
    release(interpreter(), exception);
    raiseSystemError(globalObject, scope, who, " returned a result with an exception set"_s);
    if (auto* error = PyException::tryFrom(pendingException(globalObject))) {
        error->setCause(vm, cause);
        error->setContext(vm, cause);
        error->setSuppressContext(true);
    }
    return false;
}

void forgetWhatCRaised()
{
    setErrorIndicator(interpreter(), nullptr);
}

void giveReference(uint64_t bits)
{
    retain(interpreter(), std::bit_cast<Handle>(static_cast<uintptr_t>(bits)));
}

void giveBackReference(uint64_t bits)
{
    release(interpreter(), std::bit_cast<Handle>(static_cast<uintptr_t>(bits)));
}

// The caller may be any interpreter, so it has asked nothing, and this asks for itself.
//  - Whether the API is set up. While it is not, nothing is counted, and no reference has come with anything.
//  - Whether the thread has the lock of the VM whose table this is. A callback that is called on a thread of another VM's is not run, and returns NULL. But the caller reads whether the code gives
//    a reference before it calls, and the code may be another callback's by the time it runs: its owner has let it die meanwhile, and a thread of the caller's VM has been given the same code for a callback
//    of its own, which returns an object of that VM. (To call a callback that has died is an error everywhere.) Nothing has been given then either.
void takeOverReference(uint64_t bits, bool programGetsObject)
{
    Handle handle = std::bit_cast<Handle>(static_cast<uintptr_t>(bits));
    JSCell* cell = countedCell(handle);
    if (!cell)
        return;
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (!current || !current->lock->currentThreadIsHoldingLock()) [[unlikely]]
        return;
    // Where the call has ended otherwise than with a result, nobody has looked at the bits. What is in the table is a cell, and lives. (By the key: when nothing is let go of any more, the
    // count may be 0, and the program gets the object all the same.)
    if (current->counts.contains(cell)) [[likely]] {
        if (programGetsObject) [[likely]]
            didGetHolder(cell, Holder::Program);
        release(*current, handle);
    }
}

// ---- The symbols, for every interpreter ----

// identityBits(), for the face of an instance. Whoever asks has it in hand. With the GIL.
uint64_t bitsOfInstance(JSCell* cell)
{
    return std::bit_cast<uintptr_t>(uncheckedDowncast<Face>(cell)->body());
}

// ---- The symbols, for every interpreter ----

JSValue objectOfSymbol(JSGlobalObject* globalObject, uint64_t bits)
{
    Handle handle = std::bit_cast<Handle>(static_cast<uintptr_t>(bits));
    if (!isStatic(handle)) [[likely]]
        return { };
    // An address in the middle of one.
    if ((bitsOf(handle) - std::bit_cast<uintptr_t>(&pyStaticObjectsBegin)) % sizeof(StaticObject))
        return { };
    Runtime& rt = runtime(globalObject);
    switch (uint64_t number = std::bit_cast<StaticObject*>(handle)->number) {
    case NumberOfNone:
        return jsUndefined();
    case NumberOfFalse:
        return jsBoolean(false);
    case NumberOfTrue:
        return jsBoolean(true);
    case NumberOfEllipsis:
        return rt.ellipsis();
    case NumberOfNotImplemented:
        return rt.notImplemented();
    default:
        return typeWithSymbol(rt, number);
    }
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#else // !PYTHON_HAS_CAPI

namespace JSC {
namespace Python {
namespace CApi {

bool ensure(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseImportError(globalObject, scope, "the C API is not there on this system"_s);
    return false;
}

uint64_t bitsOfInstance(JSCell*)
{
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
