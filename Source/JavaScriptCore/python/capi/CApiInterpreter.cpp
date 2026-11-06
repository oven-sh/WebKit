/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "CApiSymbols.h"
#include "JSCInlines.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"
#include <wtf/TZoneMallocInlines.h>

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

Interpreter* s_interpreter;
// Once and for all.
static std::atomic<bool> s_isTaken;

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
    if (s_interpreter && s_interpreter->globalObject == globalObject) [[likely]]
        return true;

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (s_isTaken.exchange(true)) {
        raiseImportError(globalObject, scope, "the C API serves one interpreter of a process, and it is not this one"_s);
        return false;
    }

#define KEEP_FILE(name) keep##name();
    FOR_EACH_CAPI_FILE(KEEP_FILE)
#undef KEEP_FILE

    State* state = State::create(vm, State::createStructure(vm, globalObject));
    runtime(globalObject).setSlot(Runtime::Slot::CApi, state);
    state->typeExtensionStructure.set(vm, state, TypeExtension::createStructure(vm, globalObject));
    auto* interpreter = new Interpreter(globalObject, state);
    setSymbols(interpreter);
    // The collector of another VM asks whether this is its own.
    WTF::storeStoreFence();
    s_interpreter = interpreter;
    // It stays with the heap, which may outlive the runtime.
    addMarkingConstraint(vm);
    return true;
}

bool belongsToSlow(const Runtime& runtime)
{
    return &s_interpreter->runtime == &runtime;
}

void runtimeIsEnding(Runtime& runtime)
{
    if (!belongsTo(runtime))
        return;
    delete std::exchange(s_interpreter, nullptr);
    clearSymbols();
}

// ---- The collector ----

// What C holds is reachable, as what a global variable of the program holds is. The collector learns of it as it learns of an embedder's other
// roots: by a marking constraint, which it evaluates, with the world stopped, until nothing new comes of it.
template<typename Visitor>
static void markWhatCHolds(Visitor& visitor)
{
    if (!s_interpreter)
        return;
    SetRootMarkReasonScope rootScope(visitor, RootMarkReason::StrongReferences);
    s_interpreter->counts.forEachCell([&](JSCell* cell) {
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

bool refuseToChangeCounts()
{
    static std::atomic<bool> hasTold;
    if (s_interpreter && !hasTold.exchange(true))
        fputs("Python: Py_INCREF or Py_DECREF was called by a thread that does not hold the GIL; the count stays as it was\n", stderr);
    return false;
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

} // namespace CApi
} // namespace Python
} // namespace JSC
