/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyDict.h"
#include "PyFunction.h"
#include "PyHooks.h"
#include "PyTuple.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

class PyFrame;
class PyGenerator;
class PyTraceback;

// An instance of BaseException or of a class derived from it.
class PyException final : public PyObject {
public:
    static constexpr unsigned StructureFlags = PyObject::StructureFlags | OverridesGetPrototype;
    PY_DECLARE_CELL(PyException, PyObject, PyExceptionType);
    DECLARE_VISIT_CHILDREN;

    // To JS every exception is an Error, and a TypeError is a TypeError: `e instanceof TypeError`. Which of the two prototypes it is goes
    // by the class, and an object can change its class.
    static JSValue getPrototype(JSObject*, JSGlobalObject*);

    // Some of the built-in classes have attributes of their own, which are kept here: OSError.errno and so on.
    static constexpr unsigned numberOfFields = 10;

    static PyException* create(VM&, Structure*, PyType*);

    PyTuple* args() const { return m_args.get(); }
    void setArgs(VM& vm, PyTuple* args) { m_args.set(vm, this, args); }
    // A PyTraceback, or empty.
    JSValue traceback() const
    {
        if (m_hasNotedEntry) [[unlikely]]
            const_cast<PyException*>(this)->willBeSeen();
        assertFrameLeftLastHasCaller();
        return m_traceback.get();
    }
    // For what walks the heap, and gives it to nobody.
    JSValue tracebackAsItIs() const { return m_traceback.get(); }
    // For what adds an entry to it.
    JSValue tracebackSoFar()
    {
        if (m_hasNotedEntry) [[unlikely]]
            willBeSeen();
        return m_traceback.get();
    }
    void setTraceback(VM& vm, JSValue value)
    {
        if (m_hasNotedEntry) [[unlikely]]
            willBeSeen();
        m_traceback.set(vm, this, value);
    }
    JSValue cause() const { return m_cause.get(); } // Empty: None. So for the context.
    void setCause(VM& vm, JSValue value) { m_cause.set(vm, this, value); }
    JSValue context() const { return m_context.get(); }
    void setContext(VM& vm, JSValue value) { m_context.set(vm, this, value); }
    bool suppressContext() const { return m_suppressContext; }
    void setSuppressContext(bool value) { m_suppressContext = value; }
    PyDict* dict() const { return m_dict.get(); }
    void setDict(VM& vm, PyDict* dict) { m_dict.setMayBeNull(vm, this, dict); }

    JSValue field(unsigned index) const { return m_fields[index].get(); } // Empty: not set.
    void setField(VM& vm, unsigned index, JSValue value) { m_fields[index].set(vm, this, value); }

    // ---- See "Entries of tracebacks that are only noted" in PyRuntime.h ----

    // Nothing of the program's refers to it, or can come to without the runtime knowing.
    bool isUnseen() const { return m_isUnseen; }
    // The runtime has just made it, without running code of the program, and is about to raise it.
    void wasMadeToBeRaised() { m_isUnseen = true; }
    // The program is about to get it, or something that leads to it.
    ALWAYS_INLINE void willBeSeen()
    {
        m_isUnseen = false;
        if (m_hasNotedEntry) [[unlikely]]
            makeNotedEntry();
        assertFrameLeftLastHasCaller();
    }
    bool hasNotedEntry() const { return m_hasNotedEntry; }
    // The entry that was made last is of that frame.
    inline bool lastEntryIsOf(PyFrame*) const;
    void setHasNotedEntry(bool value) { m_hasNotedEntry = value; }

private:
    PyException(VM&, Structure*, PyType*);
    void makeNotedEntry();
    // See PyFrame::backIs().
#if ASSERT_ENABLED
    void assertFrameLeftLastHasCaller() const;
#else
    void assertFrameLeftLastHasCaller() const { }
#endif

    WriteBarrier<PyTuple> m_args;
    WriteBarrier<Unknown> m_traceback;
    WriteBarrier<Unknown> m_cause;
    WriteBarrier<Unknown> m_context;
    WriteBarrier<PyDict> m_dict;
    bool m_suppressContext { false };
    bool m_isUnseen { false };
    bool m_hasNotedEntry { false };
    std::array<WriteBarrier<Unknown>, numberOfFields> m_fields;
};

// The fields, by class.
// Those of a type in the order in which CPython's type clears them: see PyException::visitChildrenImpl().
namespace ExceptionField {
static constexpr unsigned stopIterationValue = 0;
static constexpr unsigned systemExitCode = 0;
static constexpr unsigned nameErrorName = 0;
static constexpr unsigned attributeErrorObj = 0, attributeErrorName = 1;
static constexpr unsigned importErrorMsg = 0, importErrorName = 1, importErrorPath = 2, importErrorNameFrom = 3;
static constexpr unsigned osErrorErrno = 0, osErrorStrerror = 1, osErrorFilename = 2, osErrorFilename2 = 3, osErrorCharactersWritten = 4;
static constexpr unsigned syntaxErrorMsg = 0, syntaxErrorFilename = 1, syntaxErrorLineno = 2, syntaxErrorOffset = 3, syntaxErrorText = 4, syntaxErrorEndLineno = 5, syntaxErrorEndOffset = 6, syntaxErrorPrintFileAndLine = 7, syntaxErrorMetadata = 8;
static constexpr unsigned unicodeErrorEncoding = 0, unicodeErrorObject = 1, unicodeErrorStart = 2, unicodeErrorEnd = 3, unicodeErrorReason = 4;
static constexpr unsigned exceptionGroupMessage = 0, exceptionGroupExceptions = 1;
// It has a place to itself: a JSError may be a TypeError or an OSError as well.
static constexpr unsigned jsErrorValue = 9;
}

// A frame object: a call of a Python function, as programs see it.
class PyFrame final : public PyObject {
public:
    PY_DECLARE_CELL(PyFrame, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    // The frame object of a call in progress, of which there is one: the frame keeps it (frameObjectRegister()).
    static PyFrame* of(JSGlobalObject*, CallFrame*);
    // That of a generator, which is the same one each time the generator runs.
    static PyFrame* of(JSGlobalObject*, PyGenerator*);
    // For what tells tools of events, and keeps notes of its own here.
    static PyFrame* ofForTracing(JSGlobalObject*, CallFrame*);
    // The program has asked for it: sys._getframe(), gi_frame, f_back, tb_frame. False: at most a trace function has been handed it.
    bool wasAskedFor() const { return m_wasAskedFor; }
    void isAskedFor() { m_wasAskedFor = true; }
    // Of no call at all: as if the function had been called and had got nowhere.
    static PyFrame* createSuspended(JSGlobalObject*, PyFunction*, int32_t state);

    PyFunction* function() const { return m_function.get(); }
    // The code of the call, which the function may not have any more.
    FunctionExecutable* executable() const { return m_executable.get(); }
    CodeInfo& codeInfo() const;
    PyCode* code(JSGlobalObject*) const; // f_code
    // Where the call has got to, or had when it ended.
    int lineNumber();
    unsigned bytecodeOffset();
    void setLineNumber(int line) { m_lineNumber = line; }

    // The registers of the call. Null: it is not in progress.
    CallFrame* callFrame();
    // Null: it is not the frame of a generator, or that has ended.
    PyGenerator* generator() const;

    // The same for the entry of a traceback. `isOutOfReach`: of an exception that nobody has seen and that is about to leave the call. If the frame object is
    // made for that, only the exception leads to it.
    static PyFrame* ofForTraceback(JSGlobalObject*, CallFrame*, bool isOutOfReach);

    // The call ends, with its frame still there. `callerIsFoundLater`: see backIs().
    void callIsEnding(JSGlobalObject*, CallFrame*, bool callerIsFoundLater = false);
    bool isKnownToTracebackOnly() const { return m_isKnownToTracebackOnly; }
    // An exception that nobody has seen has left the call and come up to its caller, a call of Python's. If that one catches it and nobody
    // looks, it needs no frame object, nor do its callers. If it gets one after all, for its own entry in the traceback, this is told. That is before the
    // exception is seen (PyException::willBeSeen()), and only the exception leads here: so nothing of the program's meets a frame object that lacks its caller.
    void backIs(VM& vm, PyFrame* caller)
    {
        if (std::exchange(m_lacksBack, false))
            m_back.set(vm, this, caller);
    }
    bool lacksBack() const { return m_lacksBack; }
    // For what walks the heap.
    JSValue backAsItIs() const { return m_back.get(); }
    // The generator has yielded, or has ended: its frame is gone.
    void generatorHasStopped(bool isCompleted);

    // Once the call has ended: the frame of the caller. Empty: there was none.
    JSValue back() const
    {
        ASSERT(!m_lacksBack);
        return m_back.get();
    }
    // Once the call has ended: what the registers of CodeInfo::variables held. Null: nothing is kept.
    PyTuple* values() const { return m_values.get(); }
    // Once the call has ended: the namespace, for the body of a module or a class.
    JSValue names() const { return m_names.get(); }
    // What has been stored through f_locals under names that are not variables. Null: nothing yet.
    PyDict* extraLocals() const { return m_extraLocals.get(); }
    void setExtraLocals(VM& vm, PyDict* dict) { m_extraLocals.set(vm, this, dict); }
    // frame.clear(), when the call has ended
    void clear();

    // After lineNumber(): the call is at something the compiler added, which is of no line. What lineNumber() gave is a line near it.
    bool isOnNoLine() const { return m_isOnNoLine; }

    // ---- For tracing: see PyMonitoring.h ----

    JSValue trace() const { return m_trace.get(); } // f_trace. Empty: None.
    void setTrace(VM& vm, JSValue function) { m_trace.set(vm, this, function); }
    bool tracesLines { true }; // f_trace_lines
    bool tracesOpcodes { false }; // f_trace_opcodes
    // The line the call was last found to be on. -1: none yet.
    int lastLine { -1 };
    // The instruction of the program that last raised an exception in the call: its offset in the bytecode. -1: none has.
    int raisedAt { -1 };
    // Where a trace function has said to go on (f_lineno = line): the offset of an instruction. Then, where it has just gone.
    std::optional<unsigned> jumpTarget;
    std::optional<unsigned> jumpedTo;
    bool jumpsToStart { false }; // To the line of `def`, which is before the first line of the body.
    // Monitoring::Shown, while something is told of an event in the call. The runtime has it too, for the thread that is told. It is
    // another thread that asks a frame which a debugger has stopped.
    int shownLine { -1 };
    int shownOffset { -1 };
    // The offset of a call in progress after which an instruction is to be told of: see CodeMonitor::Point::isLastOfStatement.
    std::optional<unsigned> callWithResultToTake;

private:
    PyFrame(VM&, Structure*, PyType*);
    static PyFrame* create(JSGlobalObject*, PyFunction*, FunctionExecutable*);
    static PyFrame* makeFor(JSGlobalObject*, CallFrame*);
    static PyFrame* findOrMakeFor(JSGlobalObject*, PyGenerator*);
    void findPlace(CodeBlock*, BytecodeIndex);

    WriteBarrier<PyFunction> m_function;
    WriteBarrier<FunctionExecutable> m_executable;
    // Once the call has ended, until something asks where it was: m_bytecodeOffset is as the bytecode has it.
    WriteBarrier<CodeBlock> m_codeBlockAtEnd;
    WriteBarrier<PyObject> m_generator;
    WriteBarrier<Unknown> m_back;
    WriteBarrier<PyTuple> m_values;
    WriteBarrier<Unknown> m_names;
    WriteBarrier<PyDict> m_extraLocals;
    WriteBarrier<Unknown> m_trace;
    bool m_isOnNoLine { false };
    bool m_isKnownToTracebackOnly { false };
    bool m_lacksBack { false };
    bool m_wasAskedFor { false };
    // Not null while the call is in progress, except that the frame object of a generator may not have been told that it runs again.
    CallFrame* m_callFrame { nullptr };
    int m_lineNumber { 0 };
    unsigned m_bytecodeOffset { 0 };
};

class PyTraceback final : public PyObject {
public:
    PY_DECLARE_CELL(PyTraceback, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyTraceback* create(JSGlobalObject*, JSValue next, PyFrame*, int lineNumber, int lastI);
    // For a call in progress. The line is found when something asks for it, as for a StackFrame of JS.
    static PyTraceback* create(JSGlobalObject*, JSValue next, PyFrame*, CodeBlock*, BytecodeIndex);
    // For a call of a function of JS. What Python sees of that, a frame and a line, is made when something asks for it: finding the
    // line takes the source map of the file.
    static PyTraceback* createForJS(JSGlobalObject*, JSValue next, CodeBlock*, BytecodeIndex);

    JSValue next() const { return m_next.get(); } // Empty: the last.
    void setNext(VM& vm, JSValue value) { m_next.set(vm, this, value); }
    bool isOfJS() const { return m_isOfJS; }
    PyFrame* frame()
    {
        if (!m_frame) [[unlikely]]
            makeFrameOfJS();
        ASSERT(!m_frame->lacksBack());
        return m_frame.get();
    }
    // For what adds an entry in front of this one.
    PyFrame* frameIfMade() const { return m_frame.get(); }
    int lineNumber()
    {
        if (m_codeBlock)
            findPlace();
        return m_lineNumber;
    }
    // tb_lasti: see shownOffset(). -1: no instruction of Python's.
    int lastI()
    {
        if (m_codeBlock)
            findPlace();
        return m_lastI;
    }

private:
    PyTraceback(VM&, Structure*, PyType*);
    void findPlace();
    void makeFrameOfJS();

    WriteBarrier<Unknown> m_next;
    WriteBarrier<PyFrame> m_frame;
    WriteBarrier<CodeBlock> m_codeBlock; // Until the line is known, and for JS the frame.
    BytecodeIndex m_index;
    int m_lineNumber { 0 };
    int m_lastI { 0 };
    bool m_isOfJS { false };
};

inline bool PyException::lastEntryIsOf(PyFrame* frame) const
{
    JSValue traceback = m_traceback.get();
    return traceback && uncheckedDowncast<PyTraceback>(traceback.asCell())->frameIfMade() == frame;
}

// type(*args), for a built-in exception type, without running any Python code.
PyException* newException(JSGlobalObject*, PyType*, std::span<const JSValue> args);
PyException* newException(JSGlobalObject*, PyType*, const String& message);

// What a `raise` statement does. Either may be empty.
// `callee`: not empty if the exception is what calling that has just given.
void raiseStatement(JSGlobalObject*, JSValue exception, JSValue cause, JSValue callee);
// Whether calling the type makes an instance and does nothing else that a program could tell, then or later: it runs no __new__, __init__ or
// __del__ of the program's.
bool isMadeWithoutCodeOfTheProgram(JSGlobalObject*, PyType*);

// What JS code, or the VM itself, threw, as an exception `except` clauses can catch.
JSValue exceptionFromThrownValue(JSGlobalObject*, JSValue thrown);
// The same, if that takes making nothing. Null: it would.
PyException* exceptionOfThrownValueIfKnown(JSGlobalObject*, JSValue thrown);

// The line of the code an instruction comes from.
int lineNumberForBytecodeIndex(CodeBlock*, BytecodeIndex);
// The instruction is on no line and raises an exception again on behalf of the program, as if from where the program raised it. (In CPython
// RERAISE puts f_lasti back.)
bool raisesAgainAsIfFromWhereItWasRaised(CodeBlock*, BytecodeIndex);
// It is where throw() raises what it was given.
bool throwsIntoGenerator(CodeBlock*, BytecodeIndex);

// Prints the exception and its traceback on sys.stderr, as the interpreter does for one nothing caught: nothing if that is None.
// As in CPython it is Lib/traceback.py that does it, which underlines what raised, colours what goes to a terminal and has hints
// to give. What is written in C++ (formatException()) is for when that cannot be imported or fails.
void printException(JSGlobalObject*, JSValue exception);
// The same for a file. CPython's callers differ in whether traceback.py hears of it, or writes to sys.stderr whatever it is.
void printException(JSGlobalObject*, JSValue exception, JSValue file, bool tracebackModuleIsToldTheFile);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
