/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSGenerator.h"
#include "PyBytecode.h"
#include "PyException.h"
#include "PyFunction.h"

namespace JSC {
namespace Python {

// A generator, a coroutine or an asynchronous generator.
class PyGenerator final : public PyObject {
    // The compiled code reads and writes these four as "internal fields", so they come first. See PyBytecode.h.
    WriteBarrier<Unknown> m_state;
    WriteBarrier<Unknown> m_body;
    WriteBarrier<Unknown> m_frame;
    WriteBarrier<Unknown> m_arguments;

public:
    PY_DECLARE_CELL(PyGenerator, PyObject, PyGeneratorType);
    DECLARE_VISIT_CHILDREN;

    enum class Kind : uint8_t { Generator, Coroutine, AsyncGenerator };

    // As JSGenerator::State: a positive state is where to resume.
    enum State : int32_t {
        Completed = -1,
        Executing = -2,
        NotStarted = 0,
    };

    static PyGenerator* create(JSGlobalObject*, PyFunction* function, PyFunction* body, PyTuple* arguments);

    Kind kind() const { return m_kind; }
    int32_t state() const { return m_state.get().asInt32(); }
    void setState(int32_t state) { m_state.setWithoutWriteBarrier(jsNumber(state)); }
    bool isRunning() const { return state() == Executing; }
    bool isCompleted() const { return state() == Completed; }
    bool isStarted() const { return state() != NotStarted; }

    PyFunction* function() const { return m_function.get(); }
    PyFunction* body() const { return uncheckedDowncast<PyFunction>(m_body.get().asCell()); }
    JSValue frame() const { return m_frame.get(); } // Once it has started: a JSLexicalEnvironment, where its registers are while it is suspended.
    JSValue arguments() const { return m_arguments.get(); } // Until it starts: a tuple, the values of the parameters.
    JSValue name() const { return m_name.get(); }
    void setName(VM& vm, JSValue value) { m_name.set(vm, this, value); }
    JSValue qualname() const { return m_qualname.get(); }
    void setQualname(VM& vm, JSValue value) { m_qualname.set(vm, this, value); }

    // What `yield from` or `await` is waiting on. Empty: nothing.
    JSValue delegate() const { return m_delegate.get(); }
    void setDelegate(VM& vm, JSValue value) { m_delegate.set(vm, this, value); }

    // The exception being handled in the generator, which stays with it while it is suspended.
    JSValue handledException() const { return m_handledException.get(); }
    void setHandledException(VM& vm, JSValue value) { m_handledException.set(vm, this, value); }
    PyGenerator* resumedBy() const { return m_resumedBy.get(); }
    void setResumedBy(VM& vm, PyGenerator* value) { m_resumedBy.setMayBeNull(vm, this, value); }
    // gi_frame, once somebody has asked for it. See PyFrame::of().
    PyFrame* frameObject() const { return m_frameObject.get(); }
    void setFrameObject(VM& vm, PyFrame* frame) { m_frameObject.set(vm, this, frame); }

    // Nothing more will run in it.
    void complete();

    // Made by a generator function that types.coroutine() marked: `await` takes it.
    bool isIterableCoroutine { false };

    // An asynchronous generator.
    bool hooksInitialized { false };
    bool isClosed { false };
    bool isRunningAsync { false };
    JSValue finalizer() const { return m_finalizer.get(); }
    void setFinalizer(VM& vm, JSValue value) { m_finalizer.set(vm, this, value); }

    // A coroutine: where it was created, if sys.set_coroutine_origin_tracking_depth() asked.
    JSValue origin() const { return m_origin.get(); }

    enum class Outcome : uint8_t {
        Yielded,
        Returned,
        Threw,
    };
    // Runs it until it next yields. With ThrowMode, `value` is an exception to raise where it is suspended.
    Outcome resume(JSGlobalObject*, JSValue value, JSGenerator::ResumeMode, JSValue& result);

private:
    PyGenerator(VM&, Structure*, PyType*, Kind);

    WriteBarrier<PyFunction> m_function;
    WriteBarrier<Unknown> m_name;
    WriteBarrier<Unknown> m_qualname;
    WriteBarrier<Unknown> m_delegate;
    WriteBarrier<Unknown> m_handledException;
    WriteBarrier<PyGenerator> m_resumedBy;
    WriteBarrier<PyFrame> m_frameObject;
    WriteBarrier<Unknown> m_finalizer;
    WriteBarrier<Unknown> m_origin;
    Kind m_kind;
    bool m_isRegisteredForFinalization { false };
};

// What asend(), athrow() and aclose() return.
class PyAsyncGenAwaitable final : public PyObject {
public:
    PY_DECLARE_CELL(PyAsyncGenAwaitable, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    enum class Kind : uint8_t {
        Send, // value: what to send
        Throw, // value: a tuple, the arguments of athrow()
        Close,
    };
    enum class State : uint8_t { Initial, Iterating, Closed };

    static PyAsyncGenAwaitable* create(JSGlobalObject*, Kind, PyGenerator*, JSValue value);

    Kind kind() const { return m_kind; }
    PyGenerator* generator() const { return m_generator.get(); }
    JSValue value() const { return m_value.get(); }

    State state() const { return m_state; }
    void setState(State);

private:
    PyAsyncGenAwaitable(VM&, Structure*, PyType*, Kind);

    WriteBarrier<PyGenerator> m_generator;
    WriteBarrier<Unknown> m_value;
    Kind m_kind;
    State m_state { State::Initial };
};

// generator.send(value), as an instruction needs it: what it yields, or an empty value when it has returned, with what it
// returned in `result`; or an exception.
JSValue generatorSend(JSGlobalObject*, PyGenerator*, JSValue value, JSValue& result);
// generator.throw(exception), likewise.
JSValue generatorThrow(JSGlobalObject*, PyGenerator*, JSValue exception, JSValue& result);
// What anext(iterator, default) returns, given iterator.__anext__().
JSValue makeANextAwaitable(JSGlobalObject*, JSValue awaitable, JSValue defaultValue);
// generator.close()
void generatorClose(JSGlobalObject*, PyGenerator*);
// RuntimeWarning: coroutine '...' was never awaited
void warnOfUnawaitedCoroutine(JSGlobalObject*, PyGenerator*);

// The `yield from` and `await` loops.
JSValue getYieldFromIter(JSGlobalObject*, JSValue, bool inCoroutine);
JSValue getAwaitable(JSGlobalObject*, JSValue, int where);
// `async for`
JSValue getAIter(JSGlobalObject*, JSValue);
JSValue getANext(JSGlobalObject*, JSValue iterator);
// What tells the value of a `yield` in an asynchronous generator from what an `await` in it passes up.
JSValue wrapAsyncGeneratorValue(JSGlobalObject*, JSValue);
// What the iterator yields, or empty when it is done, leaving its result with the runtime.
JSValue sendToDelegate(JSGlobalObject*, JSValue iterator, JSValue value, JSGenerator::ResumeMode);

} // namespace Python
} // namespace JSC
