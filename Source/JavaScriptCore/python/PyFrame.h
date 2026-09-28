/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "BytecodeIndex.h"
#include "JSGenerator.h"
#include "JSObject.h"
#include "PyInstance.h"
#include "PythonFunctionInfo.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSScope;
class PyDict;

// A frame, as Python sees one: sys._getframe(), traceback.tb_frame, generator.gi_frame.
//
// There is at most one for each time that a piece of code is run, and none until it is asked for. What is running knows which is its own:
// it is in a register of the frame, or for a generator, which has a new frame each time it is resumed, in a property of the generator.
//
// Where the variables are depends on what has become of the frame.
// - While it is on the stack, they are in its registers, and this reads and writes them there.
// - While a generator is suspended, they are where the generator saved them.
// - Once it is over they are here. What is leaving a frame says so (op_py_ret, and the unwinder), and they are copied then.
// Variables that inner functions use are in environments, which last as long as anything needs them.
class PyFrame final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        static_assert(CellType::needsDestruction == DoesNotNeedDestruction);
        return &vm.cellSpace();
    }

    DECLARE_EXPORT_INFO;
    PYTHON_OVERLOADS_OPERATORS
    DECLARE_VISIT_CHILDREN;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // Of a frame of Python code that has got as far as running what was written.
    static PyFrame* forCallFrame(VM&, CallFrame*);
    static PyFrame* forCallFrameIfExists(VM&, CallFrame*);
    // Null if it has finished and nothing had asked for its frame.
    static PyFrame* forGenerator(JSGlobalObject*, JSGenerator*);

    // The frame is about to be no more.
    void leave(VM&, CallFrame*, BytecodeIndex);

    enum class State : uint8_t {
        Running, // On the stack.
        NotStarted, // A generator that has yet to be resumed for the first time.
        Suspended, // A generator, at a yield.
        Over,
    };
    State state() const;

    JSFunction* function() const { return m_function.get(); }
    FunctionExecutable* executable() const;
    const Python::FunctionInfo& functionInfo() const;
    JSGenerator* generator() const { return m_generator.get(); }

    // Its local variables, in the order of co_varnames, co_cellvars and co_freevars. The value of one that is unbound is empty.
    unsigned variableCount() const { return m_variableCount; }
    const Identifier& variableName(unsigned index) const;
    JSValue variable(VM&, unsigned index);
    void setVariable(VM&, unsigned index, JSValue);
    // For the body of a class, and what exec() runs: the mapping that its names are looked up in. Empty for anything else.
    JSValue namespaceMapping(VM&);
    JSObject* globals(VM&);

    // What has been added to f_locals that is not a variable.
    PyDict* extraLocals() const { return m_extraLocals.get(); }
    void setExtraLocals(VM&, PyDict*);

    PyFrame* back(VM&);
    unsigned line(VM&);
    unsigned lineAt(VM&, BytecodeIndex); // The line that some instruction of its code is from.
    std::optional<std::pair<unsigned, unsigned>> sourceRangeAt(VM&, BytecodeIndex); // And what part of the source, if it says.
    JSValue namespaceArgument(VM&, CallFrame*);
    // Where it has got to in the bytecode. Nothing if it has not started.
    std::optional<BytecodeIndex> bytecodeIndex(VM&);

    // Lets go of everything that it holds. Only for one that is over.
    void clear();

    JSValue trace() const { return m_trace.get(); }
    void setTrace(VM& vm, JSValue trace) { m_trace.set(vm, this, trace); }
    // The line that it was last found to be on, by op_py_line, or -1.
    int lastLine() const { return m_lastLine; }
    void setLastLine(int line) { m_lastLine = line; }
    // What f_lineno is to be for the time being, or -1 for it to be worked out.
    void setLineOverride(int line) { m_lineOverride = line; }
    // A call that has been told of, of what is not written in Python, and where it was made. When it is over that is told of too, which is done at the next thing that is told of the
    // frame, nothing being told of it in between.
    JSValue pendingCallable() const { return m_pendingCallable.get(); }
    JSValue pendingArgument() const { return m_pendingArgument.get(); }
    unsigned pendingCallOffset() const { return m_pendingCallOffset; }
    uint8_t pendingCallTools() const { return m_pendingCallTools; } // Who was told of it.
    void setPendingCall(VM& vm, JSValue callable, JSValue argument, unsigned offset, uint8_t tools)
    {
        m_pendingCallTools = tools;
        m_pendingCallable.set(vm, this, callable);
        m_pendingArgument.set(vm, this, argument);
        m_pendingCallOffset = offset;
    }
    void clearPendingCall()
    {
        m_pendingCallable.clear();
        m_pendingArgument.clear();
    }
    bool tracesLines() const { return m_tracesLines; }
    void setTracesLines(bool value) { m_tracesLines = value; }
    bool tracesOpcodes() const { return m_tracesOpcodes; }
    void setTracesOpcodes(bool value) { m_tracesOpcodes = value; }

private:
    PyFrame(VM&, Structure*, JSFunction*, unsigned variableCount);

    static PyFrame* create(VM&, JSGlobalObject*, JSFunction*);

    static size_t offsetOfVariables() { return WTF::roundUpToMultipleOf<sizeof(WriteBarrier<Unknown>)>(sizeof(PyFrame)); }
    static size_t allocationSize(Checked<size_t> variableCount) { return offsetOfVariables() + variableCount * sizeof(WriteBarrier<Unknown>); }
    WriteBarrier<Unknown>* variables() { return std::bit_cast<WriteBarrier<Unknown>*>(std::bit_cast<char*>(this) + offsetOfVariables()); }

    const Python::CodeDetails& details() const;
    // The frame on the stack, if it is running.
    CallFrame* callFrame(VM&) const;
    // Where a suspended generator saved a register. Null if it did not.
    WriteBarrierBase<Unknown>* savedRegister(VM&, VirtualRegister) const;
    // The innermost environment.
    JSScope* scope(VM&);
    // Where a variable is now. Null if that is nowhere, as for a local variable of a generator that has not started.
    WriteBarrierBase<Unknown>* heapSlot(VM&, unsigned index, JSCell*& owner);

    WriteBarrier<JSFunction> m_function; // For a generator, the one that is called to resume it.
    WriteBarrier<JSGenerator> m_generator; // Until it is over.
    WriteBarrier<PyFrame> m_back; // Once it is over.
    WriteBarrier<JSScope> m_scope; // Once it is over.
    WriteBarrier<Unknown> m_namespace; // Once it is over.
    WriteBarrier<PyDict> m_extraLocals;
    WriteBarrier<Unknown> m_trace;
    WriteBarrier<Unknown> m_pendingCallable;
    WriteBarrier<Unknown> m_pendingArgument;
    CallFrame* m_callFrame { nullptr }; // While it is on the stack, unless it is a generator's, which is looked for when it is wanted.
    BytecodeIndex m_bytecodeIndex; // Once it is over.
    unsigned m_variableCount;
    int m_lastLine { -1 };
    int m_lineOverride { -1 };
    unsigned m_pendingCallOffset { 0 };
    uint8_t m_pendingCallTools { 0 };
    bool m_isOver { false };
    bool m_tracesLines { true };
    bool m_tracesOpcodes { false };
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
