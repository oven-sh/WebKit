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

class PyGenerator;
class PyTraceback;

// An instance of BaseException or of a class derived from it.
class PyException final : public PyObject {
public:
    PY_DECLARE_CELL(PyException, PyObject, PyExceptionType);
    DECLARE_VISIT_CHILDREN;

    // Some of the built-in classes have attributes of their own, which are kept here: OSError.errno and so on.
    static constexpr unsigned numberOfFields = 8;

    static PyException* create(VM&, Structure*, PyType*);

    PyTuple* args() const { return m_args.get(); }
    void setArgs(VM& vm, PyTuple* args) { m_args.set(vm, this, args); }
    JSValue traceback() const { return m_traceback.get(); } // A PyTraceback, or empty.
    void setTraceback(VM& vm, JSValue value) { m_traceback.set(vm, this, value); }
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

private:
    PyException(VM&, Structure*, PyType*);

    WriteBarrier<PyTuple> m_args;
    WriteBarrier<Unknown> m_traceback;
    WriteBarrier<Unknown> m_cause;
    WriteBarrier<Unknown> m_context;
    WriteBarrier<PyDict> m_dict;
    bool m_suppressContext { false };
    std::array<WriteBarrier<Unknown>, numberOfFields> m_fields;
};

// The fields, by class.
namespace ExceptionField {
static constexpr unsigned stopIterationValue = 0;
static constexpr unsigned systemExitCode = 0;
static constexpr unsigned nameErrorName = 0;
static constexpr unsigned attributeErrorName = 0, attributeErrorObj = 1;
static constexpr unsigned importErrorMsg = 0, importErrorName = 1, importErrorPath = 2, importErrorNameFrom = 3;
static constexpr unsigned osErrorErrno = 0, osErrorStrerror = 1, osErrorFilename = 2, osErrorFilename2 = 3, osErrorCharactersWritten = 4;
static constexpr unsigned syntaxErrorMsg = 0, syntaxErrorFilename = 1, syntaxErrorLineno = 2, syntaxErrorOffset = 3, syntaxErrorText = 4, syntaxErrorEndLineno = 5, syntaxErrorEndOffset = 6, syntaxErrorPrintFileAndLine = 7;
static constexpr unsigned unicodeErrorEncoding = 0, unicodeErrorObject = 1, unicodeErrorStart = 2, unicodeErrorEnd = 3, unicodeErrorReason = 4;
static constexpr unsigned exceptionGroupMessage = 0, exceptionGroupExceptions = 1;
static constexpr unsigned jsErrorValue = 0;
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
    // Of no call at all: as if the function had been called and had got nowhere.
    static PyFrame* createSuspended(JSGlobalObject*, PyFunction*, int32_t state);

    PyFunction* function() const { return m_function.get(); }
    // Where the call has got to, or had when it ended.
    int lineNumber();
    unsigned bytecodeOffset();
    void setLineNumber(int line) { m_lineNumber = line; }

    // The registers of the call. Null: it is not in progress.
    CallFrame* callFrame();
    // Null: it is not the frame of a generator, or that has ended.
    PyGenerator* generator() const;

    // The call ends, with its frame still there.
    void callIsEnding(JSGlobalObject*, CallFrame*);
    // The generator has yielded, or has ended: its frame is gone.
    void generatorHasStopped(bool isCompleted);

    // Once the call has ended: the frame of the caller. Empty: there was none.
    JSValue back() const { return m_back.get(); }
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
    // The line the call was last found to be on, and where that begins (as f_lasti has it). -1: none yet.
    int lastLine { -1 };
    int lastLineOffset { -1 };
    // The line of the program that last raised an exception in the call, and the instruction.
    int raisedOnLine { -1 };
    int raisedAtOffset { -1 };
    // Where a trace function has said to go on (f_lineno = line): the offset of an instruction. Then, where it has just gone.
    std::optional<unsigned> jumpTarget;
    std::optional<unsigned> jumpedTo;
    bool jumpsToStart { false }; // To the line of `def`, which is before the first line of the body.

private:
    PyFrame(VM&, Structure*, PyType*);
    static PyFrame* create(JSGlobalObject*, PyFunction*);

    WriteBarrier<PyFunction> m_function;
    WriteBarrier<PyObject> m_generator;
    WriteBarrier<Unknown> m_back;
    WriteBarrier<PyTuple> m_values;
    WriteBarrier<Unknown> m_names;
    WriteBarrier<PyDict> m_extraLocals;
    WriteBarrier<Unknown> m_trace;
    bool m_isOnNoLine { false };
    // Not null while the call is in progress, except that the frame object of a generator may not have been told that it runs again.
    CallFrame* m_callFrame { nullptr };
    int m_lineNumber { 0 };
    unsigned m_bytecodeOffset { 0 };
};

class PyTraceback final : public PyObject {
public:
    PY_DECLARE_CELL(PyTraceback, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyTraceback* create(JSGlobalObject*, JSValue next, PyFrame*, int lineNumber, unsigned bytecodeOffset);

    JSValue next() const { return m_next.get(); } // Empty: the last.
    void setNext(VM& vm, JSValue value) { m_next.set(vm, this, value); }
    PyFrame* frame() const { return m_frame.get(); }
    int lineNumber() const { return m_lineNumber; }
    unsigned bytecodeOffset() const { return m_bytecodeOffset; }

private:
    PyTraceback(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_next;
    WriteBarrier<PyFrame> m_frame;
    int m_lineNumber { 0 };
    unsigned m_bytecodeOffset { 0 };
};

// type(*args), for a built-in exception type, without running any Python code.
PyException* newException(JSGlobalObject*, PyType*, std::span<const JSValue> args);
PyException* newException(JSGlobalObject*, PyType*, const String& message);

// What a `raise` statement does. Either may be empty.
void raiseStatement(JSGlobalObject*, JSValue exception, JSValue cause);

// What JS code, or the VM itself, threw, as an exception `except` clauses can catch.
JSValue exceptionFromThrownValue(JSGlobalObject*, JSValue thrown);
// The same, if that takes making nothing. Null: it would.
PyException* exceptionOfThrownValueIfKnown(JSGlobalObject*, JSValue thrown);

// The line of the code an instruction comes from.
int lineNumberForBytecodeIndex(CodeBlock*, BytecodeIndex);

// Prints the exception and its traceback on sys.stderr, as the interpreter does for one nothing caught.
void printException(JSGlobalObject*, JSValue exception);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
