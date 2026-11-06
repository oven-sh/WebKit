/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "CApiSymbols.h"
#include "JSCInlines.h"
#include "PyCtypes.h"
#include "PyException.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"
#include <wtf/TZoneMallocInlines.h>

#if PYTHON_HAS_CAPI

namespace JSC {
namespace Python {
namespace CApi {

// ---- The symbols that stand for objects ----

#define DEFINE_STATIC_TYPE(symbol, id) PY_CAPI_STATIC_OBJECT(symbol)
FOR_EACH_CAPI_STATIC_TYPE(DEFINE_STATIC_TYPE)
#undef DEFINE_STATIC_TYPE

// The headers for abi3t ask Py_GetConstantBorrowed() for these five.
PY_CAPI_STATIC_OBJECT(_Py_NoneStruct)
PY_CAPI_STATIC_OBJECT(_Py_FalseStruct)
PY_CAPI_STATIC_OBJECT(_Py_TrueStruct)
PY_CAPI_STATIC_OBJECT(_Py_EllipsisObject)
PY_CAPI_STATIC_OBJECT(_Py_NotImplementedStruct)

#define DEFINE_EXCEPTION(name) PY_CAPI_DATA(Handle, PyExc_##name)
FOR_EACH_CAPI_EXCEPTION(DEFINE_EXCEPTION)
#undef DEFINE_EXCEPTION
PY_CAPI_DATA(Handle, PyExc_EnvironmentError)
PY_CAPI_DATA(Handle, PyExc_IOError)

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Interpreter);

const ClassInfo TypeExtension::s_info = { "PyCApiTypeExtension"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(TypeExtension) };
const ClassInfo State::s_info = { "PyCApiState"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(State) };

TypeExtension* TypeExtension::create(VM& vm, Structure* structure)
{
    auto* extension = new (NotNull, allocateCell<TypeExtension>(vm)) TypeExtension(vm, structure);
    extension->finishCreation(vm);
    return extension;
}

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
    visitor.append(thisObject->typeExtensionStructure);
}

DEFINE_VISIT_CHILDREN(State);

// ---- The one interpreter ----

std::atomic<Interpreter*> s_interpreter;
std::atomic<const Runtime*> s_runtimeItIsFor;
std::atomic<const Runtime*> s_runtimeItIsSetUpFor;

static void setSymbols(Interpreter* interpreter)
{
    auto typeIs = [&](StaticObject& symbol, PyType* type) {
        symbol.value = JSValue::encode(type);
        auto* extension = TypeExtension::create(interpreter->vm, interpreter->state->typeExtensionStructure.get());
        extension->symbol = &symbol;
        type->setExtension(interpreter->vm, extension);
    };
    Runtime& rt = interpreter->runtime;
#define SET_STATIC_TYPE(symbol, id) typeIs(symbol, rt.id##Type());
    FOR_EACH_CAPI_STATIC_TYPE(SET_STATIC_TYPE)
#undef SET_STATIC_TYPE

    _Py_NoneStruct.value = JSValue::encode(jsUndefined());
    _Py_FalseStruct.value = JSValue::encode(jsBoolean(false));
    _Py_TrueStruct.value = JSValue::encode(jsBoolean(true));
    _Py_EllipsisObject.value = JSValue::encode(rt.ellipsis());
    _Py_NotImplementedStruct.value = JSValue::encode(rt.notImplemented());

#define SET_EXCEPTION(name) PyExc_##name = toC(rt.name());
    FOR_EACH_CAPI_EXCEPTION(SET_EXCEPTION)
#undef SET_EXCEPTION
    PyExc_EnvironmentError = PyExc_OSError;
    PyExc_IOError = PyExc_OSError;
}

static void clearSymbols()
{
    for (StaticObject* symbol = &pyStaticObjectsBegin; symbol != &pyStaticObjectsEnd; ++symbol)
        symbol->value = 0;
#define CLEAR_EXCEPTION(name) PyExc_##name = nullptr;
    FOR_EACH_CAPI_EXCEPTION(CLEAR_EXCEPTION)
#undef CLEAR_EXCEPTION
    PyExc_EnvironmentError = nullptr;
    PyExc_IOError = nullptr;
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
    state->typeExtensionStructure.set(vm, state, TypeExtension::createStructure(vm, globalObject));
    auto* interpreter = new Interpreter(globalObject, state);
    setSymbols(interpreter);
    s_interpreter.store(interpreter, std::memory_order_release);
    s_interpreterWhoseCountsFall.store(interpreter, std::memory_order_release);
    s_runtimeItIsSetUpFor.store(&interpreter->runtime, std::memory_order_relaxed);
    // It stays with the heap, which may outlive the runtime.
    addMarkingConstraint(vm);
    return true;
}

void runtimeIsEnding(Runtime& runtime)
{
    if (!isFor(runtime))
        return;
    s_runtimeItIsFor.store(nullptr, std::memory_order_relaxed);
    if (!isSetUpFor(runtime))
        return;
    s_runtimeItIsSetUpFor.store(nullptr, std::memory_order_relaxed);
    s_interpreterWhoseCountsFall.store(nullptr, std::memory_order_relaxed);
    // Not freed: see Interpreter::lock.
    Interpreter* interpreter = s_interpreter.exchange(nullptr, std::memory_order_relaxed);
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

// The first time in a thread, and whenever the answer is no: nothing is refused on the word of what a thread has kept.
bool hasGILSlow(Interpreter& current)
{
    // What else is destroyed with the thread comes here too, and may come later. Nothing is kept for it.
    if (t_thisThreadIsEnding)
        return current.lock->currentThreadIsHoldingLock();
    if (!t_thisThread) {
        Thread& thread = Thread::currentSingleton();
        // (No room for the value: then nothing is kept.)
        if (pthread_setspecific(current.keyOfReferenceToThread, &thread))
            return current.lock->isHeldBy(thread);
        thread.ref();
        t_thisThread = &thread;
    }
    return current.lock->isHeldBy(*t_thisThread);
}

static constinit thread_local JSGlobalObject* t_loadsLibraryFor = nullptr;

LibraryBeingLoaded::LibraryBeingLoaded(JSGlobalObject* globalObject)
    : m_previous(std::exchange(t_loadsLibraryFor, globalObject))
{
    tryEnsure(globalObject);
}

LibraryBeingLoaded::~LibraryBeingLoaded()
{
    t_loadsLibraryFor = m_previous;
}

// Whether what the thread is doing is C that another interpreter has called or is loading.
static bool runsCForAnotherInterpreter(Interpreter& current)
{
    if (t_loadsLibraryFor)
        return t_loadsLibraryFor != current.globalObject;
    VM* vm = Ctypes::vmOfForeignCallInProgress();
    return vm && vm != &current.vm;
}

static void tellOnce(std::atomic<bool>& hasTold, const char* what)
{
    if (!hasTold.exchange(true))
        fputs(what, stderr);
}

bool mayEnterSlow(Interpreter* current)
{
    // What a library does when the interpreter has ended, as the process goes, is nobody's mistake. Nor is it one to ask before there is anything to ask about.
    if (!current)
        return false;
    static std::atomic<bool> hasToldOfOther;
    static std::atomic<bool> hasTold;
    if (runsCForAnotherInterpreter(*current))
        tellOnce(hasToldOfOther, "Python: a function of the C API was called for an interpreter that the C API does not serve (it serves the first of the process); it fails, and nothing changes for the interpreter that it serves\n");
    else
        tellOnce(hasTold, "Python: a function of the C API was called by a thread that does not hold the GIL; it fails\n");
    return false;
}

std::atomic<Interpreter*> s_interpreterWhoseCountsFall;

void refuseToChangeCount(bool isToFall)
{
    Interpreter* current = s_interpreter.load(std::memory_order_relaxed);
    if (!current)
        return;
    static std::atomic<bool> hasToldOfOther;
    static std::atomic<bool> hasToldOfTaken;
    static std::atomic<bool> hasToldOfGivenBack;
    if (runsCForAnotherInterpreter(*current)) {
        tellOnce(hasToldOfOther, "Python: Py_INCREF or Py_DECREF was called for an interpreter that the C API does not serve (it serves the first of the process); the count stays as it was, and nothing changes for the interpreter that it serves\n");
        return;
    }
    if (!isToFall) {
        s_interpreterWhoseCountsFall.store(nullptr, std::memory_order_relaxed);
        tellOnce(hasToldOfTaken, "Python: Py_INCREF was called by a thread that does not hold the GIL; the count stays as it was, and from now on no count falls: whatever C owns is held for ever\n");
        return;
    }
    // With the GIL: this is what that line has told of.
    if (s_interpreterWhoseCountsFall.load(std::memory_order_relaxed))
        tellOnce(hasToldOfGivenBack, "Python: Py_DECREF was called by a thread that does not hold the GIL; the count stays as it was: the object is held for ever\n");
}

// ---- The error indicator ----

static thread_local Handle t_errorIndicator;

Handle& errorIndicator()
{
    return t_errorIndicator;
}

void setErrorIndicator(Handle exception)
{
    if (Handle previous = std::exchange(t_errorIndicator, exception))
        release(previous);
}

void Entry::moveExceptionToIndicator()
{
    // Empty: it is a termination, and goes on.
    if (JSValue exception = takePendingException(globalObject))
        setErrorIndicator(CApi::newReference(exception));
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

bool throwWhatCRaised(JSGlobalObject* globalObject)
{
    Handle exception = std::exchange(t_errorIndicator, nullptr);
    if (!exception) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwPythonException(globalObject, scope, toJS(exception));
    release(exception);
    return false;
}

bool throwWhatCRaisedBesideResult(JSGlobalObject* globalObject, const String& who)
{
    Handle exception = std::exchange(t_errorIndicator, nullptr);
    if (!exception) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue cause = toJS(exception);
    release(exception);
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
    setErrorIndicator(nullptr);
}

void takeOverReference(uint64_t bits)
{
    if (bits)
        release(std::bit_cast<Handle>(static_cast<uintptr_t>(bits)));
}

void giveReference(uint64_t bits)
{
    if (bits)
        retain(std::bit_cast<Handle>(static_cast<uintptr_t>(bits)));
}

// ---- The symbols, for every interpreter ----

// Not by TypeExtension::symbol, which only the types of one interpreter have, and only once the API is set up. Nothing is kept for this: it is asked for a type that is
// written into memory as an object, and by id().
static StaticObject* symbolOf(Runtime& rt, PyType* type)
{
    if (type->isHeapType())
        return nullptr;
#define RETURN_IF_STATIC_TYPE(symbol, id) \
    if (type == rt.id##Type()) \
        return &symbol;
    FOR_EACH_CAPI_STATIC_TYPE(RETURN_IF_STATIC_TYPE)
#undef RETURN_IF_STATIC_TYPE
    return nullptr;
}

uint64_t bitsForC(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isNull())
        value = jsUndefined();
    else if (value && value.isCell() && value.asCell()->type() == InternalFunctionType) {
        if (PyType* type = PyType::tryFrom(value.asCell())) {
            if (StaticObject* symbol = symbolOf(runtime(globalObject), type))
                return std::bit_cast<uintptr_t>(symbol);
        }
    }
    return static_cast<uint64_t>(JSValue::encode(value));
}

JSValue objectOfSymbol(JSGlobalObject* globalObject, uint64_t bits)
{
    Handle handle = std::bit_cast<Handle>(static_cast<uintptr_t>(bits));
    if (!isStatic(handle)) [[likely]]
        return { };
    auto* given = std::bit_cast<StaticObject*>(handle);
    Runtime& rt = runtime(globalObject);
#define RETURN_IF_STATIC_TYPE(symbol, id) \
    if (given == &symbol) \
        return rt.id##Type();
    FOR_EACH_CAPI_STATIC_TYPE(RETURN_IF_STATIC_TYPE)
#undef RETURN_IF_STATIC_TYPE
    if (given == &_Py_NoneStruct)
        return jsUndefined();
    if (given == &_Py_FalseStruct)
        return jsBoolean(false);
    if (given == &_Py_TrueStruct)
        return jsBoolean(true);
    if (given == &_Py_EllipsisObject)
        return rt.ellipsis();
    if (given == &_Py_NotImplementedStruct)
        return rt.notImplemented();
    // An address in the middle of one.
    return { };
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

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
