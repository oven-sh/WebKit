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
public:
    PY_DECLARE_CELL(PyGenerator, PyObject, PyGeneratorType);
    DECLARE_VISIT_CHILDREN;

    enum class Kind : uint8_t { Generator, Coroutine, AsyncGenerator };

    // A positive state is where to resume: the offset of an instruction of the body.
    enum State : int32_t {
        Completed = -1,
        Executing = -2,
        NotStarted = 0,
    };

    static PyGenerator* create(JSGlobalObject*, PyFunction* function, PyFunction* body, unsigned slotCount);

    Kind kind() const { return m_kind; }
    int32_t state() const { return m_state.get().asInt32(); }
    void setState(int32_t state) { m_state.setWithoutWriteBarrier(jsNumber(state)); }
    bool isRunning() const { return state() == Executing; }
    bool isCompleted() const { return state() == Completed; }
    bool isStarted() const { return state() != NotStarted; }

    PyFunction* function() const { return m_function.get(); }
    PyFunction* body() const { return m_body.get(); }

    // Where the registers of the body are kept while it is suspended: those that hold something that is still needed. Which
    // slot belongs to which register is in the instructions that save and restore them (op_py_generator_registers,
    // op_py_generator_suspend). Until it starts, the first slots hold the values of the parameters. While it is running, and
    // when it is done, they are all empty.
    unsigned slotCount() const { return m_slotCount; }
    JSValue slot(unsigned index) const { ASSERT(index < m_slotCount); return slots()[index].get(); }
    void setSlot(VM& vm, unsigned index, JSValue value) { ASSERT(index < m_slotCount); slots()[index].set(vm, this, value); }
    static constexpr size_t allocationSize(unsigned slotCount) { return offsetOfSlots() + slotCount * sizeof(WriteBarrier<Unknown>); }
    // Those the function had when this was made. If they were still those of its code, the strs are made when one is first asked for.
    JSValue name() { return m_name ? m_name.get() : (takeNamesFromCode(), m_name.get()); }
    void setName(VM& vm, JSValue value) { m_name.set(vm, this, value); }
    JSValue qualname() { return m_qualname ? m_qualname.get() : (takeNamesFromCode(), m_qualname.get()); }
    void setQualname(VM& vm, JSValue value) { m_qualname.set(vm, this, value); }

    // What `yield from` or `await` is waiting on. Empty: nothing.
    JSValue delegate() const { return m_delegate.get(); }
    void setDelegate(VM& vm, JSValue value) { m_delegate.set(vm, this, value); }

    // It has been resumed to raise something, which goes to what it delegates to first, and that has neither raised nor returned
    // yet. (It means nothing once the generator has been resumed to go on.) CPython does not resume the generator for
    // that, so nobody is told that it is resumed, or that it yields again: Monitoring::handle().
    bool throwIsWithDelegate { false };

    // The exception being handled in the generator, which stays with it while it is suspended.
    JSValue handledException() const { return m_handledException.get(); }
    void setHandledException(VM& vm, JSValue value) { m_handledException.set(vm, this, value); }
    // While it is running: the generator that was running when it was resumed, Runtime::runningGenerator() then.
    PyGenerator* resumedBy() const { return m_resumedBy; }
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
    // Not 0: nothing refers to it but the register in which a `for` loop keeps its iterator, if nothing has escaped since: see
    // Runtime::claimIteratorOfLoop().
    unsigned loopOwnership { 0 };
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
    // It is neither running nor completed.
    //
    // To resume a generator is to call its body, with the arguments JSGenerator::Argument lists. The body does the rest:
    // op_py_generator_enter, its first instruction, notes that the generator is running; op_py_generator_suspend and
    // op_py_generator_return, with which it returns, that it no longer is; and the unwinder, if an exception leaves the body
    // (bodyIsUnwinding()). Whoever called the body tells from isCompleted() whether it has yielded or returned. So
    // instructions can do it as well as this function: op_py_for_iter and op_py_send do.
    Outcome resume(JSGlobalObject*, JSValue value, JSGenerator::ResumeMode, JSValue& result);

    // What those instructions do, when the interpreter leaves it to C++. False: an exception.
    bool enterBody(JSGlobalObject*);
    void suspendBody(JSGlobalObject*, unsigned resumeOffset);
    void returnFromBody(JSGlobalObject*);
    // An exception is about to leave the frame of the body. It may be replaced by another.
    void bodyIsUnwinding(JSGlobalObject*, bool isTermination);

    static constexpr ptrdiff_t offsetOfState() { return OBJECT_OFFSETOF(PyGenerator, m_state); }
    static constexpr ptrdiff_t offsetOfBody() { return OBJECT_OFFSETOF(PyGenerator, m_body); }
    static constexpr ptrdiff_t offsetOfSlots();
    static constexpr ptrdiff_t offsetOfDelegate() { return OBJECT_OFFSETOF(PyGenerator, m_delegate); }
    static constexpr ptrdiff_t offsetOfResumedBy() { return OBJECT_OFFSETOF(PyGenerator, m_resumedBy); }
    static constexpr ptrdiff_t offsetOfFrameObject() { return OBJECT_OFFSETOF(PyGenerator, m_frameObject); }
    static constexpr ptrdiff_t offsetOfKind() { return OBJECT_OFFSETOF(PyGenerator, m_kind); }
    static constexpr ptrdiff_t offsetOfLoopOwnership() { return OBJECT_OFFSETOF(PyGenerator, loopOwnership); }
    static constexpr ptrdiff_t offsetOfHandledException() { return OBJECT_OFFSETOF(PyGenerator, m_handledException); }
    static constexpr ptrdiff_t offsetOfFinalization() { return OBJECT_OFFSETOF(PyGenerator, m_finalization); }

    // One that is dropped before it is done may have something to clean up (a `finally`, a `with`), or to complain of.
    enum class Finalization : uint8_t {
        None, // It has not, or it is done.
        Registered, // Runtime::registerFinalizable() has been told, when it was made.
    };
    Finalization finalization() const { return m_finalization; }
    void setFinalization(Finalization finalization) { m_finalization = finalization; }

private:
    PyGenerator(VM&, Structure*, PyType*, Kind, unsigned slotCount, PyFunction*, PyFunction* body);
    void leaveBody(Runtime&);
    void takeNamesFromCode();
    WriteBarrier<Unknown>* slots() const { return storageBehind<WriteBarrier<Unknown>>(const_cast<PyGenerator*>(this)); }

    WriteBarrier<Unknown> m_state; // An int32.
    WriteBarrier<PyFunction> m_body;
    WriteBarrier<PyFunction> m_function;
    WriteBarrier<Unknown> m_name;
    WriteBarrier<Unknown> m_qualname;
    WriteBarrier<Unknown> m_delegate;
    WriteBarrier<Unknown> m_handledException;
    PyGenerator* m_resumedBy { nullptr }; // It is running, so the frame of its body refers to it.
    WriteBarrier<PyFrame> m_frameObject;
    WriteBarrier<Unknown> m_finalizer;
    WriteBarrier<Unknown> m_origin;
    unsigned m_slotCount;
    Kind m_kind;
    Finalization m_finalization { Finalization::None };
};

constexpr ptrdiff_t PyGenerator::offsetOfSlots() { return sizeof(PyGenerator); }

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
// What is done to one that the program has dropped.
void finalizeGeneratorNow(JSGlobalObject*, PyGenerator*);

// (x for x in g()): what the generator expression goes through is a variable of its frame, `.0`, and nothing else refers to it. It goes when
// the frame goes. That is known to be at once where the generator expression is closed, or dropped: nobody gets to see the GeneratorExit, whose
// traceback has the frame. An exception that comes out of the generator expression has the frame for as long as it lasts, which only the collector knows.
// So it is if the program has asked for the frame object (PyFrame::wasAskedFor()): what the generator expression called may have kept sys._getframe(1).
// For what closes a generator that is suspended: made before, and told when it is over.
class GeneratorsOwnedByFrame {
public:
    explicit GeneratorsOwnedByFrame(Runtime&, PyGenerator*);
    // Those that loops in there own have been closed on the way out.
    void frameIsGone();

private:
    Runtime& m_runtime;
    std::array<PyGenerator*, 4> m_generators { };
    unsigned m_count { 0 };
};
// Nothing refers to it any more, and the code that held it knows: what CPython does at that moment. Does not throw.
void generatorIsDropped(JSGlobalObject*, PyGenerator*);
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
