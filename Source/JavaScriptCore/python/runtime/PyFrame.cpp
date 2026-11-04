/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyException.h"

#include "BytecodeLivenessAnalysisInlines.h"
#include "BytecodeStructs.h"
#include "BytecodeUseDef.h"
#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "JSCInlines.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "PyAbstract.h"
#include "PyBytecode.h"
#include "PyCommandLine.h"
#include "PyDescriptors.h"
#include "PyErrors.h"
#include "PyExec.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyMonitoring.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"
#include "PyVirtualFiles.h"
#include "TopExceptionScope.h"
#include <wtf/FileSystem.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// Frame objects and tracebacks, and how an exception nothing caught is shown.

PY_DEFINE_CELL(PyFrame);
PY_DEFINE_CELL(PyTraceback);

// ---- The stack ----

static ALWAYS_INLINE bool isPythonCodeBlock(CodeBlock* codeBlock)
{
    return codeBlock && codeBlock->unlinkedCodeBlock()->isPython();
}

bool liveLocalsOfFrame(CallFrame* callFrame, CodeBlock* codeBlock, bool isCalling, FastBitVector& live)
{
    BytecodeIndex index = callFrame->bytecodeIndex();
    const JSInstruction* instruction = codeBlock->instructions().at(index).ptr();
    if (!isCalling) {
        // The frame of the callee is being filled from an array, in registers the instruction does not name.
        switch (instruction->opcodeID()) {
        case op_call_varargs:
        case op_tail_call_varargs:
        case op_construct_varargs:
        case op_super_construct_varargs:
            return false;
        default:
            break;
        }
    }

    live = codeBlock->livenessAnalysis().getLivenessInfoAtInstruction(codeBlock, index);
    for (auto& piece : blockPiecesOf(codeBlock)) {
        if (!piece.contains(index.offset()))
            continue;
        piece.forEachRegister([&](VirtualRegister held) {
            live[held.toLocal()] = true;
        });
    }
    // That is what is live before the instruction, which may be half done: one that unpacks an iterable has set some of its
    // targets. A call sets its result when the callee returns, and until then the register has what some earlier expression left.
    switch (instruction->opcodeID()) {
    case op_call:
    case op_call_ignore_result:
    case op_call_varargs:
    case op_construct:
    case op_construct_varargs:
    // These call the body of a generator. (What else op_py_send sets, the iterator, it sets before that. It reads it too, so it is live.)
    case op_py_for_iter:
    case op_py_send:
        return true;
#define CASE(opcode, registerCount) case opcode:
    FOR_EACH_PYTHON_OPCODE_THAT_CALLS_METHODS(CASE)
#undef CASE
        if (isCalling)
            return true;
        break;
    default:
        break;
    }
    computeDefsForBytecodeIndex(codeBlock, instruction, noCheckpoints, [&](VirtualRegister defined) {
        if (defined.isLocal())
            live[defined.toLocal()] = true;
    });
    return true;
}

Register* endOfFrameMadeByInstruction(CallFrame* callFrame, CodeBlock* codeBlock)
{
    // Where the stack pointer is between two instructions.
    return callFrame->registers() - codeBlock->numCalleeLocals() - maxFrameExtentForSlowPathCallInRegisters;
}

// If an exception has come out of what the instruction called: it would have emptied the frame it made for that before giving it up.
static void clearFrameMadeByInstruction(CallFrame* callFrame, CodeBlock* codeBlock, BytecodeIndex index, CallFrame* frameBefore, JSValue thrown)
{
    unsigned registerCount;
    const JSInstruction* instruction = codeBlock->instructions().at(index).ptr();
    switch (instruction->opcodeID()) {
#define CASE(opcode, count) \
    case opcode: \
        registerCount = count; \
        break;
    FOR_EACH_PYTHON_OPCODE_THAT_CALLS_METHODS(CASE)
#undef CASE
    default:
        return;
    }
    static_assert(CallFrame::headerSizeInRegisters == 5, "sizeOfFrameMadeByInstruction()");
    Register* end = endOfFrameMadeByInstruction(callFrame, codeBlock);
    Register* frame = end - sizeOfFrameMadeByInstruction(registerCount);
    // Otherwise the instruction has called nothing, or C++ has for it: that is where the stack of C++ begins.
    if (frameBefore->registers() != frame)
        return;
    ASSERT(frameBefore->callerFrame() == callFrame);
    // No getAttr() is on the way of what the getter raises.
    if (instruction->opcodeID() == op_py_get_attr)
        completeAttributeError(codeBlock->globalObject(), thrown, frame[static_cast<int>(CallFrameSlot::firstArgument) + RegisterOfGetter::Object].jsValue(), asString(callFrame->r(instruction->as<OpPyGetAttr>().m_name).jsValue()));
    // All but the head: if the exception was thrown in that frame, the machine code that goes on to the handler asks its callee for the VM.
    for (Register* slot = frame + static_cast<int>(CallFrameSlot::firstArgument); slot < end; ++slot)
        *slot = JSValue();
}

// Where the frame ends that the instruction makes for what it calls: it lies in the registers of the caller, at a place that the
// instruction names. Null: it makes none, or where is not known beforehand.
// `hasCalled`: the callee has run. Otherwise the instruction may still be about to call it, or be doing its work in some other way.
static Register* endOfFrameMadeBy(CallFrame* callFrame, const JSInstruction* instruction, bool hasCalled)
{
    auto end = [&](unsigned stackOffset, unsigned argumentCountIncludingThis) {
        return callFrame->registers() - stackOffset + CallFrame::headerSizeInRegisters + argumentCountIncludingThis;
    };
    constexpr unsigned argumentsOfGeneratorBody = static_cast<unsigned>(JSGenerator::Argument::NumberOfArguments) + 1;
    switch (instruction->opcodeID()) {
    case op_call:
        return end(instruction->as<OpCall>().m_argv, instruction->as<OpCall>().m_argc);
    case op_call_ignore_result:
        return end(instruction->as<OpCallIgnoreResult>().m_argv, instruction->as<OpCallIgnoreResult>().m_argc);
    case op_construct:
        return end(instruction->as<OpConstruct>().m_argv, instruction->as<OpConstruct>().m_argc);
    // These fill the frame and call the body of the generator in one go, or leave it all to C++, which makes a frame elsewhere:
    // then what is in those registers is what an earlier call left there.
    case op_py_for_iter:
        return hasCalled ? end(instruction->as<OpPyForIter>().m_stackOffset, argumentsOfGeneratorBody) : nullptr;
    case op_py_send:
        return hasCalled ? end(instruction->as<OpPySend>().m_stackOffset, argumentsOfGeneratorBody) : nullptr;
    default:
        return nullptr;
    }
}

// A frame for which there was no room on the stack still has its code block, but neither its function nor its registers: where those
// would be is what C++ is using to throw the error.
static ALWAYS_INLINE bool isPythonFrame(CallFrame* callFrame)
{
    return !callFrame->isNativeCalleeFrame() && isPythonCodeBlock(callFrame->codeBlock()) && !callFrame->isZombieFrame();
}

// Where the registers of a frame of Python end that are its own. `callee`: the frame of what it is calling, if that is known.
static Register* endOfFrameOfCallee(CallFrame* callFrame, CodeBlock* codeBlock, CallFrame* callee)
{
    // The frame of the callee lies in the registers of the caller. It has room for all the parameters, given or not.
    // A frame that is at a call has one there even if no callee is known: while an exception unwinds the stack, whoever traces
    // the program is told with this frame as the topmost (tellOfException() in PyMonitoring.cpp), and the frames that the
    // exception has left are still there. The machine code that goes on to the handler reads the one it was thrown from.
    bool isToldOfException = !callee && codeBlock->globalObject()->pythonRuntime()->framesToldOfException.contains(callFrame);
    Register* endOfCallee = endOfFrameMadeBy(callFrame, codeBlock->instructions().at(callFrame->bytecodeIndex()).ptr(), isToldOfException);
    if (callee) {
        unsigned argumentCount = callee->argumentCountIncludingThis();
        if (!callee->isNativeCalleeFrame() && callee->codeBlock())
            argumentCount = std::max(argumentCount, callee->codeBlock()->numParameters());
        endOfCallee = std::max(endOfCallee, callee->registers() + CallFrame::headerSizeInRegisters + argumentCount);
    }
    return endOfCallee;
}

static void clearDeadTemporariesOf(CallFrame* callFrame, CodeBlock* codeBlock, CallFrame* callee)
{
    FastBitVector live;
    if (!liveLocalsOfFrame(callFrame, codeBlock, callee, live))
        return;

    Register* endOfCallee = endOfFrameOfCallee(callFrame, codeBlock, callee);
    for (unsigned local = codeBlock->numVars(); local < codeBlock->numCalleeLocals(); ++local) {
        Register* slot = &callFrame->r(virtualRegisterForLocal(local));
        if (slot < endOfCallee)
            break;
        if (!live[local])
            *slot = JSValue();
    }
}

static void clearDeadTemporariesFrom(CallFrame* topCallFrame, EntryFrame* entryFrame)
{
    CallFrame* callee = nullptr;
    for (CallFrame* callFrame = topCallFrame; callFrame;) {
        if (isPythonFrame(callFrame))
            clearDeadTemporariesOf(callFrame, callFrame->codeBlock(), callee);
        EntryFrame* entryFrameOfCallee = entryFrame;
        callee = callFrame;
        callFrame = callFrame->callerFrame(entryFrame);
        // What entered the VM anew is C++, not the frame that comes next.
        if (entryFrame != entryFrameOfCallee)
            callee = nullptr;
    }
}

void clearDeadTemporaries(VM& vm)
{
    for (Runtime* runtime : vm.heap.pythonRuntimes())
        runtime->collectionIsBeginning();
    clearDeadTemporariesFrom(vm.topCallFrame, vm.topEntryFrame);
    // A thread that is parked does not touch its frames before it has the VM again, which it cannot have during a collection.
    if (vm.apiLock().hasParkedThreads()) {
        vm.apiLock().forEachParkedThread([](JSLock::ParkedThread& parked) {
            clearDeadTemporariesFrom(parked.topCallFrame(), parked.topEntryFrame());
        });
    }
}

static ALWAYS_INLINE bool isGeneratorBody(CallFrame* callFrame)
{
    return callFrame->codeBlock()->unlinkedCodeBlock()->parseMode() == SourceParseMode::GeneratorBodyMode;
}

void forEachVariableOfRunningGenerators(VM& vm, const ScopedLambda<void(PyGenerator*, JSValue)>& function)
{
    auto walk = [&](CallFrame* callFrame, EntryFrame* entryFrame) {
        for (; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
            if (!isPythonFrame(callFrame) || !isGeneratorBody(callFrame))
                continue;
            auto* generator = uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell());
            for (auto& variable : codeInfoOf(callFrame).variables) {
                if (JSValue value = callFrame->r(variable.reg).jsValue())
                    function(generator, value);
            }
        }
    };
    walk(vm.topCallFrame, vm.topEntryFrame);
    if (vm.apiLock().hasParkedThreads()) {
        vm.apiLock().forEachParkedThread([&](JSLock::ParkedThread& parked) {
            walk(parked.topCallFrame(), parked.topEntryFrame());
        });
    }
}

static CallFrame* pythonCallFrame(VM& vm, unsigned depth)
{
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* callFrame = vm.topCallFrame; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
        if (isPythonFrame(callFrame) && !depth--)
            return callFrame;
    }
    return nullptr;
}

// The frame of Python code in which this one was called, whatever lies between the two. Null: there is none.
static CallFrame* pythonCallerOf(VM& vm, CallFrame* callee)
{
    bool hasPassedCallee = false;
    auto search = [&](CallFrame* topCallFrame, EntryFrame* entryFrame) -> CallFrame* {
        for (CallFrame* callFrame = topCallFrame; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
            if (hasPassedCallee && isPythonFrame(callFrame))
                return callFrame;
            hasPassedCallee |= callFrame == callee;
        }
        return nullptr;
    };
    CallFrame* caller = search(vm.topCallFrame, vm.topEntryFrame);
    if (hasPassedCallee || !vm.apiLock().hasParkedThreads())
        return caller;
    // It is a frame of another thread.
    vm.apiLock().forEachParkedThread([&](JSLock::ParkedThread& parked) {
        if (!hasPassedCallee)
            caller = search(parked.topCallFrame(), parked.topEntryFrame());
    });
    return caller;
}

// The same for a frame that returns, or that an exception leaves. An unwind goes no further than to where the VM was entered, so the frame
// is among those since the last entry, and there is no need to look for it from the top.
static CallFrame* pythonCallerOfFrameThatEnds(VM& vm, CallFrame* callee)
{
    EntryFrame* entryFrame = vm.topEntryFrame;
    CallFrame* caller = callee->callerFrame(entryFrame);
    while (caller && !isPythonFrame(caller))
        caller = caller->callerFrame(entryFrame);
    ASSERT(caller == pythonCallerOf(vm, callee));
    return caller;
}

static ALWAYS_INLINE PyFunction* functionOf(CallFrame* callFrame)
{
    return uncheckedDowncast<PyFunction>(callFrame->jsCallee());
}

int lineNumberForBytecodeIndex(CodeBlock* codeBlock, BytecodeIndex index)
{
    auto marks = codeBlock->unlinkedCodeBlock()->pythonMarks();
    auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable());
    CodeInfo* info = executable ? executable->unlinkedExecutable()->pythonCodeInfo() : nullptr;
    int lineDelta = info ? info->lineDelta : 0;
    int line = lineOfMarks(marks, index.offset());
    if (line >= 0)
        return line + lineDelta;
    // What sets up the frame is of the line the code begins on.
    if (info && !marks.empty() && !isPastStart(marks, index.offset()))
        return info->firstLineNumber;
    // What else the compiler added comes from no line. It is near one.
    return static_cast<int>(codeBlock->lineColumnForBytecodeIndex(index).line) + lineDelta;
}

// The value of a variable of a call in progress. Empty: it has none.
static JSValue valueOfVariable(CallFrame* callFrame, const CodeInfo::Variable& variable)
{
    JSValue value = callFrame->r(variable.reg).jsValue();
    if (variable.kind == CodeInfo::VariableKind::Local || !value)
        return value;
    return PyCell::from(value)->value();
}

static const CodeInfo::Variable* findVariable(CodeInfo& info, const StringImpl* name)
{
    for (auto& variable : info.variables) {
        if (variable.name.impl() == name && !variable.isHidden)
            return &variable;
    }
    return nullptr;
}

bool findSuperArguments(JSGlobalObject* globalObject, JSValue& type, JSValue& instance)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto fail = [&](ASCIILiteral message) {
        raiseRuntimeError(globalObject, scope, message);
        return false;
    };
    CallFrame* callFrame = pythonCallFrame(vm, 0);
    if (!callFrame)
        return fail("super(): no current frame"_s);
    CodeInfo& info = codeInfoOf(callFrame);
    if (!info.argCount)
        return fail("super(): no arguments"_s);
    const auto* first = findVariable(info, info.parameterNames[0].impl());
    instance = first ? valueOfVariable(callFrame, *first) : JSValue();
    if (!instance)
        return fail("super(): arg[0] deleted"_s);

    const auto* classVariable = findVariable(info, rt.names.dClass()->tryGetValueImpl());
    if (!classVariable || classVariable->kind != CodeInfo::VariableKind::Free)
        return fail("super(): __class__ cell not found"_s);
    type = valueOfVariable(callFrame, *classVariable);
    if (!type)
        return fail("super(): empty __class__ cell"_s);
    if (!PyType::is(type)) {
        raiseRuntimeError(globalObject, scope, "super(): __class__ is not a type ("_s, typeName(globalObject, type), ')');
        return false;
    }
    return true;
}

PyDict* currentGlobals(JSGlobalObject* globalObject)
{
    CallFrame* callFrame = pythonCallFrame(getVM(globalObject), 0);
    return callFrame ? functionOf(callFrame)->globals() : nullptr;
}

JSValue coroutineOrigin(JSGlobalObject* globalObject, unsigned depth)
{
    VM& vm = getVM(globalObject);
    MarkedArgumentBuffer entries;
    EntryFrame* entryFrame = vm.topEntryFrame;
    bool isCoroutineFunction = true;
    for (CallFrame* callFrame = vm.topCallFrame; callFrame && entries.size() < depth; callFrame = callFrame->callerFrame(entryFrame)) {
        if (!isPythonFrame(callFrame))
            continue;
        if (std::exchange(isCoroutineFunction, false))
            continue;
        CodeInfo& info = codeInfoOf(callFrame);
        int line = lineNumberForBytecodeIndex(callFrame->codeBlock(), callFrame->bytecodeIndex());
        entries.append(PyTuple::create(globalObject, { makeStr(vm, info.filename), jsNumber(line), makeStr(vm, info.name.string()) }));
    }
    return PyTuple::create(globalObject, spanOf(entries));
}

PyFunction* currentFunction(JSGlobalObject* globalObject)
{
    CallFrame* callFrame = pythonCallFrame(getVM(globalObject), 0);
    return callFrame ? functionOf(callFrame) : nullptr;
}

// ---- The variables of a frame ----

// The call of a generator function only makes the generator. CodeInfo::variables says where the variables of the body are.
static bool hasVariables(CallFrame* callFrame)
{
    return !codeInfoOf(callFrame).isGeneratorLike() || isGeneratorBody(callFrame);
}

void forEachValueInFramesOfParkedThreads(VM& vm, const ScopedLambda<void(JSValue)>& function)
{
    if (!vm.apiLock().hasParkedThreads())
        return;
    vm.apiLock().forEachParkedThread([&](JSLock::ParkedThread& parked) {
        EntryFrame* entryFrame = parked.topEntryFrame();
        CallFrame* callee = nullptr;
        for (CallFrame* callFrame = parked.topCallFrame(); callFrame;) {
            if (!callFrame->isNativeCalleeFrame() && !callFrame->isZombieFrame()) {
                function(callFrame->jsCallee());
                for (unsigned i = 0; i < callFrame->argumentCountIncludingThis(); ++i)
                    function(callFrame->r(virtualRegisterForArgumentIncludingThis(i)).jsValue());
            }
            if (isPythonFrame(callFrame)) {
                CodeBlock* codeBlock = callFrame->codeBlock();
                // A variable is held until the frame goes, read again or not.
                if (hasVariables(callFrame)) {
                    for (auto& variable : codeInfoOf(callFrame).variables)
                        function(callFrame->r(variable.reg).jsValue());
                }
                // In a register that the code does not read again may be what is left of the frame of an earlier call.
                FastBitVector live;
                if (liveLocalsOfFrame(callFrame, codeBlock, callee, live)) {
                    Register* endOfCallee = endOfFrameOfCallee(callFrame, codeBlock, callee);
                    for (unsigned local = CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters(); local < codeBlock->numCalleeLocals(); ++local) {
                        Register* slot = &callFrame->r(virtualRegisterForLocal(local));
                        if (slot < endOfCallee)
                            break;
                        if (live[local])
                            function(slot->jsValue());
                    }
                }
            }
            EntryFrame* entryFrameOfCallee = entryFrame;
            callee = callFrame;
            callFrame = callFrame->callerFrame(entryFrame);
            if (entryFrame != entryFrameOfCallee)
                callee = nullptr;
        }
    });
}

namespace {

// Where the variables of a call are, which depends on what has become of it.
class FrameVariables {
public:
    // Of a call in progress.
    FrameVariables(VM& vm, CallFrame* callFrame)
        : m_vm(vm)
        , m_info(codeInfoOf(callFrame))
        , m_callFrame(hasVariables(callFrame) ? callFrame : nullptr)
    {
    }

    FrameVariables(VM& vm, PyFrame* frame)
        : m_vm(vm)
        , m_info(frame->codeInfo())
        , m_callFrame(frame->callFrame())
    {
        if (m_callFrame) {
            if (!hasVariables(m_callFrame))
                m_callFrame = nullptr;
            return;
        }
        m_names = frame->names();
        if ((m_values = frame->values()))
            return;
        PyGenerator* generator = frame->generator();
        if (!generator)
            return;
        m_generator = generator;
        m_holdsArguments = !generator->isStarted();
        if (m_holdsArguments) {
            m_closure = frame->function()->closure();
            return;
        }
        // Which slot belongs to which register is in the instructions that save and restore them.
        auto note = [&](const auto& bytecode) {
            for (unsigned i = 0; i < bytecode.m_count; ++i)
                m_slots.append({ VirtualRegister(bytecode.m_first.offset() - static_cast<int>(i)), bytecode.m_offset + i });
        };
        for (const auto& instruction : generator->body()->functionExecutable()->codeBlockForCall()->instructions()) {
            if (instruction->opcodeID() == op_py_generator_registers)
                note(instruction->as<OpPyGeneratorRegisters>());
            else if (instruction->opcodeID() == op_py_generator_suspend)
                note(instruction->as<OpPyGeneratorSuspend>());
        }
    }

    const CodeInfo& info() const { return m_info; }
    unsigned size() const { return m_info.variables.size(); }
    const CodeInfo::Variable& at(unsigned index) const { return m_info.variables[index]; }

    // Empty: it has none.
    JSValue value(unsigned index) const
    {
        if (PyCell* cell = cellInClosure(index))
            return cell->value();
        JSValue held = registerOf(index);
        // The cells of a generator are made when it starts.
        if (!held || at(index).kind == CodeInfo::VariableKind::Local || m_holdsArguments)
            return held;
        // So are those of a function, and something may look at the frame before that.
        auto* cell = PyCell::tryFrom(held);
        return cell ? cell->value() : JSValue();
    }

    // False: there is nowhere to keep it.
    bool setValue(unsigned index, JSValue value)
    {
        auto& variable = at(index);
        if (PyCell* cell = cellInClosure(index)) {
            cell->setValue(m_vm, value);
            return true;
        }
        if (variable.kind != CodeInfo::VariableKind::Local && !m_holdsArguments) {
            auto* cell = PyCell::tryFrom(registerOf(index));
            if (!cell)
                return false;
            cell->setValue(m_vm, value);
            return true;
        }
        if (m_callFrame) {
            m_callFrame->r(variable.reg) = value;
            return true;
        }
        if (m_values) {
            m_values->initializeAt(m_vm, index, value);
            return true;
        }
        if (m_generator) {
            if (auto slot = slotOf(variable)) {
                m_generator->setSlot(m_vm, *slot, value);
                return true;
            }
        }
        return false;
    }

    // The body of a module or a class keeps its variables in a namespace. Within a comprehension those of the comprehension,
    // which are not in the namespace, are all that there is to see: then, and for a function, this is empty.
    JSValue visibleNamespace() const
    {
        if (!m_info.takesNamespace)
            return { };
        for (unsigned i = 0; i < size(); ++i) {
            if (at(i).isHidden && value(i))
                return { };
        }
        if (m_callFrame)
            return m_callFrame->r(m_info.namespaceRegister).jsValue();
        if (m_names)
            return m_names;
        if (!m_generator)
            return { };
        if (m_holdsArguments)
            return m_generator->slot(0);
        auto slot = slotOf(m_info.namespaceRegister);
        return slot ? m_generator->slot(*slot) : JSValue();
    }

    // Those that have a value, in a new dict. Null: an exception.
    PyDict* toDict(JSGlobalObject* globalObject, PyDict* extra) const
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyDict* result = PyDict::create(globalObject);
        // Those of a comprehension have a value while it is in progress, and then hide others of the same name.
        for (bool hidden : { false, true }) {
            for (unsigned i = 0; i < size(); ++i) {
                if (at(i).isHidden != hidden)
                    continue;
                if (JSValue value = this->value(i))
                    result->set(m_vm, jsString(m_vm, at(i).name.string().string()), value);
            }
        }
        if (extra) {
            unsigned position = 0;
            JSValue key, value;
            while (extra->next(position, key, value)) {
                result->set(globalObject, key, value);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
        }
        return result;
    }

private:
    // What the register of the variable holds: its value, or the cell that has it. Empty: nothing.
    // A generator that has not started has not taken the variables of the enclosing functions from its closure yet.
    PyCell* cellInClosure(unsigned index) const
    {
        if (!m_closure || at(index).kind != CodeInfo::VariableKind::Free)
            return nullptr;
        size_t position = m_info.freeVariables.find(at(index).name);
        return position < m_closure->length() ? PyCell::tryFrom(m_closure->at(position)) : nullptr;
    }

    JSValue registerOf(unsigned index) const
    {
        auto& variable = at(index);
        if (m_callFrame)
            return m_callFrame->r(variable.reg).jsValue();
        if (m_values)
            return m_values->at(index);
        if (m_generator) {
            if (auto slot = slotOf(variable))
                return m_generator->slot(*slot);
        }
        return { };
    }

    std::optional<unsigned> slotOf(VirtualRegister reg) const
    {
        for (auto& entry : m_slots) {
            if (entry.first == reg)
                return entry.second;
        }
        return std::nullopt;
    }

    std::optional<unsigned> slotOf(const CodeInfo::Variable& variable) const
    {
        if (!m_holdsArguments)
            return slotOf(variable.reg);
        if (variable.isHidden)
            return std::nullopt;
        for (unsigned i = 0; i < m_info.parameterNames.size() && i < m_generator->slotCount(); ++i) {
            if (m_info.parameterNames[i] == variable.name)
                return i;
        }
        return std::nullopt;
    }

    VM& m_vm;
    const CodeInfo& m_info;
    CallFrame* m_callFrame; // In progress: in its registers.
    PyTuple* m_values { nullptr }; // Ended: the frame object has what the registers held.
    PyGenerator* m_generator { nullptr }; // A suspended generator: in its slots.
    Vector<std::pair<VirtualRegister, unsigned>, 16> m_slots;
    bool m_holdsArguments { false }; // It has not started: all there is are the values of the parameters, in the first slots.
    PyTuple* m_closure { nullptr }; // And the closure of the function.
    JSValue m_names;
};

} // namespace

PyFrame* frameObjectIfAny(CallFrame* callFrame)
{
    JSValue held = callFrame->r(frameObjectRegister()).jsValue();
    return held.isCell() ? uncheckedDowncast<PyFrame>(held.asCell()) : nullptr;
}

JSValue currentLocals(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    CallFrame* callFrame = pythonCallFrame(vm, 0);
    if (!callFrame)
        return { };
    FrameVariables variables(vm, callFrame);
    if (JSValue names = variables.visibleNamespace())
        return names;
    PyFrame* frame = frameObjectIfAny(callFrame);
    return variables.toDict(globalObject, frame ? frame->extraLocals() : nullptr);
}

JSValue frameLocals(JSGlobalObject* globalObject, PyFrame* frame)
{
    FrameVariables variables(getVM(globalObject), frame);
    if (JSValue names = variables.visibleNamespace())
        return names;
    return variables.toDict(globalObject, frame->extraLocals());
}

// ---- Frame objects ----

VirtualRegister frameObjectRegister()
{
    // After the scope register, which follows the registers that the interpreter saves.
    static const VirtualRegister result = virtualRegisterForLocal(CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters() + 1);
    return result;
}

PyFrame::PyFrame(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyFrame* PyFrame::create(JSGlobalObject* globalObject, PyFunction* function, FunctionExecutable* executable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* frame = new (NotNull, allocateCell<PyFrame>(vm)) PyFrame(vm, rt.structure<PyFrame>(), rt.frameType());
    frame->finishCreation(vm);
    frame->m_function.set(vm, frame, function);
    frame->m_executable.set(vm, frame, executable);
    frame->m_lineNumber = frame->codeInfo().firstLineNumber;
    return frame;
}

CodeInfo& PyFrame::codeInfo() const { return *m_executable->unlinkedExecutable()->pythonCodeInfo(); }
PyCode* PyFrame::code(JSGlobalObject* globalObject) const { return codeOf(globalObject, m_function.get(), m_executable.get()); }

PyFrame* PyFrame::of(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    if (PyFrame* frame = frameObjectIfAny(callFrame))
        return frame;
    // The body of a generator is called with the generator.
    PyFrame* frame = isGeneratorBody(callFrame) ? of(globalObject, uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell())) : create(globalObject, functionOf(callFrame), executableOf(callFrame));
    frame->m_callFrame = callFrame;
    callFrame->r(frameObjectRegister()) = JSValue(frame);
    if (Runtime& rt = runtime(globalObject); rt.tracedCallFrame == callFrame) {
        frame->shownLine = rt.tracedLine;
        frame->shownOffset = rt.tracedOffset;
    }
    return frame;
}

PyFrame* PyFrame::of(JSGlobalObject* globalObject, PyGenerator* generator)
{
    if (PyFrame* frame = generator->frameObject())
        return frame;
    VM& vm = getVM(globalObject);
    PyFrame* frame = create(globalObject, generator->body(), generator->body()->functionExecutable());
    frame->m_generator.set(vm, frame, generator);
    generator->setFrameObject(vm, frame);
    return frame;
}

// Where a generator in that state goes on. False: at the start.
static bool bytecodeOffsetOfState(PyFunction* body, int32_t state, unsigned& result)
{
    if (state <= 0 || !body->functionExecutable()->codeBlockForCall())
        return false;
    result = state;
    return true;
}

PyFrame* PyFrame::createSuspended(JSGlobalObject* globalObject, PyFunction* function, int32_t state)
{
    PyFrame* frame = create(globalObject, function, function->functionExecutable());
    if (bytecodeOffsetOfState(function, state, frame->m_bytecodeOffset))
        frame->m_lineNumber = lineNumberForBytecodeIndex(function->functionExecutable()->codeBlockForCall(), BytecodeIndex(frame->m_bytecodeOffset));
    return frame;
}

template<typename Visitor>
void PyFrame::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyFrame>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_function);
    visitor.append(thisObject->m_executable);
    visitor.append(thisObject->m_codeBlockAtEnd);
    visitor.append(thisObject->m_generator);
    visitor.append(thisObject->m_back);
    visitor.append(thisObject->m_values);
    visitor.append(thisObject->m_names);
    visitor.append(thisObject->m_extraLocals);
    visitor.append(thisObject->m_trace);
}

DEFINE_VISIT_CHILDREN(PyFrame);

PyGenerator* PyFrame::generator() const
{
    if (!m_generator)
        return nullptr;
    auto* generator = uncheckedDowncast<PyGenerator>(m_generator.get());
    return generator->isCompleted() ? nullptr : generator;
}

CallFrame* PyFrame::callFrame()
{
    if (m_callFrame)
        return m_callFrame;
    PyGenerator* generator = this->generator();
    if (!generator || !generator->isRunning())
        return nullptr;
    // It has been resumed since the frame object was last looked at.
    VM& vm = this->vm();
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* callFrame = vm.topCallFrame; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
        if (!isPythonFrame(callFrame) || !isGeneratorBody(callFrame) || callFrame->uncheckedArgument(0) != JSValue(generator))
            continue;
        m_callFrame = callFrame;
        callFrame->r(frameObjectRegister()) = JSValue(this);
        return callFrame;
    }
    return nullptr;
}

unsigned PyFrame::bytecodeOffset()
{
    if (CallFrame* callFrame = this->callFrame()) {
        Runtime& rt = runtime(globalObject());
        if (rt.tracedCallFrame == callFrame) {
            shownLine = rt.tracedLine;
            shownOffset = rt.tracedOffset;
        }
        if (shownLine >= 0) {
            m_bytecodeOffset = shownOffset;
            m_lineNumber = shownLine;
            m_isOnNoLine = false;
            return m_bytecodeOffset;
        }
        findPlace(callFrame->codeBlock(), callFrame->bytecodeIndex());
    } else if (m_codeBlockAtEnd) {
        findPlace(m_codeBlockAtEnd.get(), BytecodeIndex(m_bytecodeOffset));
        m_codeBlockAtEnd.clear();
    } else if (PyGenerator* generator = this->generator()) {
        if (bytecodeOffsetOfState(generator->body(), generator->state(), m_bytecodeOffset))
            m_lineNumber = lineNumberForBytecodeIndex(generator->body()->functionExecutable()->codeBlockForCall(), BytecodeIndex(m_bytecodeOffset));
    }
    return m_bytecodeOffset;
}

bool raisesAgainAsIfFromWhereItWasRaised(CodeBlock* codeBlock, BytecodeIndex index)
{
    auto instruction = codeBlock->instructionAt(index);
    if (instruction->opcodeID() != op_py_runtime)
        return false;
    auto function = static_cast<RuntimeFunction>(instruction->as<OpPyRuntime>().m_function);
    return function == RuntimeFunction::Reraise || function == RuntimeFunction::ReraiseOutOfHandler || function == RuntimeFunction::ReraiseUnseen;
}

void PyFrame::findPlace(CodeBlock* codeBlock, BytecodeIndex index)
{
    auto marks = codeBlock->unlinkedCodeBlock()->pythonMarks();
    m_bytecodeOffset = visibleOffset(marks, index.offset());
    if (int line = lineOfMarks(marks, index.offset()); line >= 0) [[likely]] {
        m_lineNumber = line + codeInfo().lineDelta;
        m_isOnNoLine = false;
        return;
    }
    m_lineNumber = lineNumberForBytecodeIndex(codeBlock, index);
    m_isOnNoLine = isPastStart(marks, index.offset());
    // That is where a frame is found to be that was left that way.
    if (!m_isOnNoLine || raisedAt < 0 || !raisesAgainAsIfFromWhereItWasRaised(codeBlock, index))
        return;
    if (int line = lineOfMarks(marks, raisedAt); line >= 0) {
        m_bytecodeOffset = visibleOffset(marks, raisedAt);
        m_lineNumber = line + codeInfo().lineDelta;
        m_isOnNoLine = false;
    }
}

int PyFrame::lineNumber()
{
    bytecodeOffset();
    return m_lineNumber;
}

void PyFrame::callIsEnding(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = getVM(globalObject);
    DeferGC deferGC(vm);
    m_callFrame = callFrame;
    if (shownLine >= 0 || runtime(globalObject).tracedCallFrame == callFrame) [[unlikely]]
        bytecodeOffset();
    else {
        m_codeBlockAtEnd.set(vm, this, callFrame->codeBlock());
        m_bytecodeOffset = callFrame->bytecodeIndex().offset();
    }

    if (hasVariables(callFrame)) {
        const CodeInfo& info = codeInfo();
        PyTuple* values = PyTuple::create(globalObject, info.variables.size());
        for (unsigned i = 0; i < info.variables.size(); ++i)
            values->initializeAt(vm, i, callFrame->r(info.variables[i].reg).jsValue());
        m_values.set(vm, this, values);
        if (info.takesNamespace)
            m_names.set(vm, this, callFrame->r(info.namespaceRegister).jsValue());
    }

    // Whoever resumed a generator is not its caller once it has stopped.
    if (m_generator)
        m_generator.clear();
    else if (CallFrame* caller = pythonCallerOfFrameThatEnds(vm, callFrame))
        m_back.set(vm, this, of(globalObject, caller));
    m_callFrame = nullptr;
}

void PyFrame::generatorHasStopped(bool isCompleted)
{
    m_callFrame = nullptr;
    if (isCompleted)
        m_generator.clear();
}

void PyFrame::clear()
{
    m_values.clear();
    m_extraLocals.clear();
}

void Runtime::frameIsEnding(CallFrame* callFrame)
{
    if (!isPythonFrame(callFrame)) [[unlikely]]
        return;
#if ASSERT_ENABLED
    assertNoNotedEntryOf(callFrame);
#endif
    PyFrame* frame = frameObjectIfAny(callFrame);
    // That of a generator may be from before it was last resumed.
    if (!frame && isGeneratorBody(callFrame))
        frame = uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell())->frameObject();
    if (frame)
        frame->callIsEnding(m_globalObject, callFrame);
}

void forEachPythonFrameWithoutAllocating(CallFrame* topCallFrame, EntryFrame* entryFrame, const ScopedLambda<bool(const CodeInfo&, int line)>& function)
{
    for (CallFrame* callFrame = topCallFrame; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
        if (!isPythonFrame(callFrame))
            continue;
        CodeBlock* codeBlock = callFrame->codeBlock();
        CodeInfo* info = uncheckedDowncast<FunctionExecutable>(codeBlock->ownerExecutable())->unlinkedExecutable()->pythonCodeInfo();
        if (!info || !function(*info, lineNumberForBytecodeIndex(codeBlock, callFrame->bytecodeIndex())))
            return;
    }
}

PyFrame* currentFrame(JSGlobalObject* globalObject, unsigned depth)
{
    CallFrame* callFrame = pythonCallFrame(getVM(globalObject), depth);
    return callFrame ? PyFrame::of(globalObject, callFrame) : nullptr;
}

// ---- Calls of functions of JS ----

static bool isShownCodeOfJS(CodeBlock* codeBlock)
{
    return !codeBlock->unlinkedCodeBlock()->isBuiltinFunction() && codeBlock->ownerExecutable()->implementationVisibility() == ImplementationVisibility::Public;
}

void forEachCallOfEitherLanguage(VM& vm, const ScopedLambda<IterationStatus(const CallOfEitherLanguage&)>& function)
{
    StackVisitor::visit(vm.topCallFrame, vm, [&](StackVisitor& visitor) -> IterationStatus {
        CodeBlock* codeBlock = visitor->isNativeCalleeFrame() ? nullptr : visitor->codeBlock();
        if (!codeBlock)
            return IterationStatus::Continue;
        if (isPythonCodeBlock(codeBlock))
            return isPythonFrame(visitor->callFrame()) ? function(CallOfEitherLanguage { visitor->callFrame(), codeBlock, visitor->bytecodeIndex(), visitor->callFrame() }) : IterationStatus::Continue;
        return isShownCodeOfJS(codeBlock) ? function(CallOfEitherLanguage { nullptr, codeBlock, visitor->bytecodeIndex(), visitor->callFrame() }) : IterationStatus::Continue;
    });
}

PositionInJS positionInJS(VM& vm, CodeBlock* codeBlock, BytecodeIndex index)
{
    PositionInJS position;
    LineColumn lineColumn = codeBlock->lineColumnForBytecodeIndex(index);
    if (std::optional<int> line = codeBlock->ownerExecutable()->overrideLineNumber(vm))
        lineColumn.line = *line;
    SourceProvider* provider = codeBlock->source().provider();
    position.filename = provider->sourceURL();
#if USE(BUN_JSC_ADDITIONS)
    // What runs may have been made from what the programmer wrote.
    if (auto& map = vm.computeLineColumnWithSourcemap()) {
        String original;
        map(vm, provider, lineColumn, original);
        if (!original.isEmpty())
            position.filename = WTF::move(original);
    }
#endif
    position.line = lineColumn.line;
    switch (codeBlock->codeType()) {
    case FunctionCode:
        position.function = uncheckedDowncast<FunctionExecutable>(codeBlock->ownerExecutable())->ecmaNameWithoutGC();
        if (position.function.isEmpty())
            position.function = "<anonymous>"_s;
        break;
    case EvalCode:
        position.function = "<eval>"_s;
        break;
    case GlobalCode:
    case ModuleCode:
        position.function = "<module>"_s;
        break;
    }
    return position;
}

static bool (*s_isMainFile)(JSGlobalObject*, const String&);

void setIsMainFileFunction(bool (*function)(JSGlobalObject*, const String&))
{
    s_isMainFile = function;
}

PyDict* namespaceOfFileOfJS(JSGlobalObject* globalObject, const String& filename)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyDict* namespaces = PyDict::tryFrom(rt.slot(Runtime::Slot::NamespacesOfFilesOfJS));
    if (!namespaces) {
        namespaces = PyDict::create(globalObject);
        rt.setSlot(Runtime::Slot::NamespacesOfFilesOfJS, namespaces);
    }
    JSString* name = makeStr(vm, filename);
    if (JSValue existing = namespaces->get(name))
        return PyDict::from(existing);
    PyDict* result = PyDict::create(globalObject);
    // As warnings.warn_explicit() names a module of which it knows the file only. The file that a program in JS begins with is what
    // __main__ is in a program in Python: a DeprecationWarning about its code is shown.
    result->set(vm, rt.names.dName(), s_isMainFile && s_isMainFile(globalObject, filename) ? rt.names.dMain() : name);
    result->set(vm, internString(vm, "__file__"_s), name);
    namespaces->set(vm, name, result);
    return result;
}

PyFrame* topFrameOf(JSGlobalObject* globalObject, JSLock::ParkedThread& parked)
{
    EntryFrame* entryFrame = parked.topEntryFrame();
    for (CallFrame* callFrame = parked.topCallFrame(); callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
        if (isPythonFrame(callFrame))
            return PyFrame::of(globalObject, callFrame);
    }
    return nullptr;
}

static JSValue frameGetBack(JSGlobalObject* globalObject, JSValue self)
{
    auto* frame = PyFrame::from(self);
    if (JSValue back = frame->back())
        return back;
    CallFrame* callFrame = frame->callFrame();
    CallFrame* caller = callFrame ? pythonCallerOf(getVM(globalObject), callFrame) : nullptr;
    return caller ? JSValue(PyFrame::of(globalObject, caller)) : jsUndefined();
}

static JSValue frameGetLocals(JSGlobalObject* globalObject, JSValue self)
{
    if (JSValue names = FrameVariables(getVM(globalObject), PyFrame::from(self)).visibleNamespace())
        return names;
    return PyWrapperObject::create(globalObject, runtime(globalObject).frameLocalsProxyType(), self);
}

static JSValue frameGetCode(JSGlobalObject* globalObject, JSValue self) { return PyFrame::from(self)->code(globalObject); }
static JSValue frameGetGlobals(JSGlobalObject*, JSValue self) { return PyFrame::from(self)->function()->globals(); }
static JSValue frameGetBuiltins(JSGlobalObject*, JSValue self) { return PyFrame::from(self)->function()->builtinsObject(); }
static JSValue frameGetLastI(JSGlobalObject*, JSValue self) { return jsNumber(shownOffset(PyFrame::from(self)->bytecodeOffset())); }
static JSValue frameGetLineNo(JSGlobalObject*, JSValue self)
{
    auto* frame = PyFrame::from(self);
    int line = frame->lineNumber();
    return frame->isOnNoLine() ? jsUndefined() : jsNumber(line);
}
static JSValue frameGetTrace(JSGlobalObject*, JSValue self)
{
    JSValue function = PyFrame::from(self)->trace();
    return function ? function : jsUndefined();
}

static void frameSetTrace(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    auto* frame = PyFrame::from(self);
    frame->setTrace(getVM(globalObject), value && !isNone(value) ? value : JSValue());
    if (frame->trace() && frame->tracesOpcodes)
        runtime(globalObject).monitoring().setTracesOpcodes(globalObject, frame, true);
}

static JSValue frameGetTraceLines(JSGlobalObject*, JSValue self) { return jsBoolean(PyFrame::from(self)->tracesLines); }
static JSValue frameGetTraceOpcodes(JSGlobalObject*, JSValue self) { return jsBoolean(PyFrame::from(self)->tracesOpcodes); }

static void frameSetTraceLines(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete numeric/char attribute"_s);
        return;
    }
    if (!value.isBoolean()) {
        raiseTypeError(globalObject, scope, "attribute value type must be bool"_s);
        return;
    }
    PyFrame::from(self)->tracesLines = value.asBoolean();
}

static void frameSetTraceOpcodes(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), "attribute 'f_trace_opcodes' of 'frame' objects cannot be deleted"_s);
        return;
    }
    if (!value.isBoolean()) {
        raiseTypeError(globalObject, scope, "attribute value type must be bool"_s);
        return;
    }
    auto* frame = PyFrame::from(self);
    frame->tracesOpcodes = value.asBoolean();
    scope.release();
    if (frame->tracesOpcodes)
        runtime(globalObject).monitoring().setTracesOpcodes(globalObject, frame, true);
}

static JSValue frameGetGenerator(JSGlobalObject* globalObject, JSValue self)
{
    runtime(globalObject).temporariesMayHaveEscaped();
    PyGenerator* generator = PyFrame::from(self)->generator();
    return generator ? JSValue(generator) : jsUndefined();
}

static void frameSetLineNo(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), "attribute 'f_lineno' of 'frame' objects cannot be deleted"_s);
        return;
    }
    if (!isExactInt(value)) {
        raiseValueError(globalObject, scope, "lineno must be an integer"_s);
        return;
    }
    int line = toCInt(globalObject, value);
    if (scope.exception()) {
        takePendingException(globalObject);
        raiseValueError(globalObject, scope, "lineno out of range"_s);
        return;
    }
    RELEASE_AND_RETURN(scope, runtime(globalObject).monitoring().jump(globalObject, PyFrame::from(self), line));
}

static JSValue frameRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto* frame = PyFrame::from(self);
    CodeInfo& info = frame->codeInfo();
    return makeStr(globalObject, makeString("<frame at "_s, addressForRepr(globalObject, self), ", file "_s, strRepr(info.filename), ", line "_s, frame->lineNumber(), ", code "_s, info.name.string(), '>'));
}

static JSValue frameClear(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* frame = PyFrame::from(self);
    PyGenerator* generator = frame->generator();
    if (frame->callFrame() || (generator && generator->isRunning()))
        return raiseRuntimeError(globalObject, scope, "cannot clear an executing frame"_s);
    if (generator) {
        if (generator->isStarted())
            return raiseRuntimeError(globalObject, scope, "cannot clear an suspended frame"_s);
        if (generator->kind() == PyGenerator::Kind::Coroutine)
            warnOfUnawaitedCoroutine(globalObject, generator);
        generator->complete();
    }
    frame->clear();
    return jsUndefined();
}

static constexpr NativeDef frameMethods[] = {
    PY_SLOT("__repr__", frameRepr, "($self, /)"),
    PY_METHOD("clear", frameClear, "($self, /)"),
};

static constexpr NativeGetSet frameGetSets[] = {
    { "f_back"_s, frameGetBack },
    { "f_code"_s, frameGetCode },
    { "f_globals"_s, frameGetGlobals },
    { "f_builtins"_s, frameGetBuiltins },
    { "f_locals"_s, frameGetLocals },
    { "f_lineno"_s, frameGetLineNo, frameSetLineNo },
    { "f_lasti"_s, frameGetLastI },
    { "f_trace"_s, frameGetTrace, frameSetTrace },
    PY_WRITABLE_MEMBER("f_trace_lines", frameGetTraceLines, frameSetTraceLines),
    { "f_trace_opcodes"_s, frameGetTraceOpcodes, frameSetTraceOpcodes },
    { "f_generator"_s, frameGetGenerator },
};

// ---- FrameLocalsProxy: the variables of a function's frame as a mapping, in which to store is to assign (PEP 667) ----

static PyFrame* frameOfProxy(JSValue self) { return PyFrame::from(uncheckedDowncast<PyWrapperObject>(self.asCell())->wrapped()); }

static bool isFrameLocalsProxy(JSGlobalObject* globalObject, JSValue value)
{
    return typeOf(globalObject, value) == runtime(globalObject).frameLocalsProxyType();
}

enum class ProxyAccess : uint8_t { Read, Write };
static constexpr int noSuchVariable = -1;
static constexpr int lookupThrew = -2;

// Which variable the key names. To read: one that has a value. To write: one of the function's own, not of a comprehension in it.
static int indexOfVariable(JSGlobalObject* globalObject, const FrameVariables& variables, JSValue key, ProxyAccess access)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t keyHash = hash(globalObject, key);
    RETURN_IF_EXCEPTION(scope, lookupThrew);

    String text;
    bool isExactStr = key.isString();
    if (isExactStr) {
        text = asString(key)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, lookupThrew);
    }
    int result = noSuchVariable;
    for (unsigned i = 0; i < variables.size(); ++i) {
        auto& variable = variables.at(i);
        if (access == ProxyAccess::Read ? !variables.value(i) : variable.isHidden)
            continue;
        if (isExactStr) {
            if (!equal(variable.name.impl(), text.impl()))
                continue;
        } else {
            if (hashOfString(variable.name.impl()) != keyHash)
                continue;
            bool isSame = isEqual(globalObject, jsString(vm, variable.name.string().string()), key);
            RETURN_IF_EXCEPTION(scope, lookupThrew);
            if (!isSame)
                continue;
        }
        // That of a comprehension in progress hides another of the same name.
        if (variable.isHidden)
            return i;
        result = i;
    }
    return result;
}

// Empty without an exception: there is none.
static JSValue proxyLookup(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyFrame* frame = frameOfProxy(self);
    FrameVariables variables(vm, frame);
    int index = indexOfVariable(globalObject, variables, key, ProxyAccess::Read);
    RETURN_IF_EXCEPTION(scope, { });
    if (index >= 0)
        return variables.value(index);
    if (PyDict* extra = frame->extraLocals())
        RELEASE_AND_RETURN(scope, extra->get(globalObject, key));
    return { };
}

static JSValue proxyGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = proxyLookup(globalObject, self, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    JSString* shown = repr(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    String text = shown->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, raiseKeyError(globalObject, scope, makeStr(globalObject, makeString("local variable '"_s, text, "' is not defined"_s))));
}

// An empty value deletes.
static void proxyStore(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyFrame* frame = frameOfProxy(self);
    FrameVariables variables(vm, frame);
    int index = indexOfVariable(globalObject, variables, key, ProxyAccess::Write);
    RETURN_IF_EXCEPTION(scope, void());
    if (index >= 0) {
        if (!value) {
            raiseValueError(globalObject, scope, "cannot remove local variables from FrameLocalsProxy"_s);
            return;
        }
        // A value may be on its way into an item of what the variable has held until now: TemporaryUse::StoredValue.
        runtime(globalObject).forgetOwnedTemporaries();
        if (variables.setValue(index, value))
            return;
    }
    PyDict* extra = frame->extraLocals();
    if (!value) {
        bool wasThere = extra && extra->remove(globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
        if (!wasThere)
            raiseKeyError(globalObject, scope, key);
        return;
    }
    if (!extra) {
        extra = PyDict::create(globalObject);
        frame->setExtraLocals(vm, extra);
    }
    RELEASE_AND_RETURN(scope, extra->set(globalObject, key, value));
}

static JSValue proxySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    proxyStore(globalObject, self, key, value);
    return jsUndefined();
}

static JSValue proxyDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    proxyStore(globalObject, self, key, { });
    return jsUndefined();
}

static JSValue proxyContains(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    return jsBoolean(!!proxyLookup(globalObject, self, key));
}

static PyDict* proxyToDict(JSGlobalObject* globalObject, JSValue self)
{
    PyFrame* frame = frameOfProxy(self);
    return FrameVariables(getVM(globalObject), frame).toDict(globalObject, frame->extraLocals());
}

static JSValue proxyCopy(JSGlobalObject* globalObject, JSValue self) { return proxyToDict(globalObject, self); }

static JSValue proxyLen(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    PyDict* dict = proxyToDict(globalObject, self);
    if (failed(scope, dict))
        return { };
    return jsNumber(dict->size());
}

enum class ProxyList : uint8_t { Keys, Values, Items };
template<ProxyList what>
static JSValue proxyList(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    PyDict* dict = proxyToDict(globalObject, self);
    if (failed(scope, dict))
        return { };
    Vector<JSValue, 16> items;
    MarkedArgumentBuffer keepAlive;
    unsigned position = 0;
    JSValue key, value;
    while (dict->next(position, key, value)) {
        JSValue item = what == ProxyList::Keys ? key : what == ProxyList::Values ? value : JSValue(PyTuple::create(globalObject, { key, value }));
        keepAlive.append(item);
        items.append(item);
    }
    RELEASE_AND_RETURN(scope, newList(globalObject, items.span()));
}

static JSValue proxyIter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue keys = proxyList<ProxyList::Keys>(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getIter(globalObject, keys));
}

static JSValue proxyReversed(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    PyDict* dict = proxyToDict(globalObject, self);
    if (failed(scope, dict))
        return { };
    Vector<JSValue, 16> keys;
    unsigned position = dict->endPosition();
    JSValue key, value;
    while (dict->previous(position, key, value))
        keys.append(key);
    ensureStillAliveHere(dict);
    RELEASE_AND_RETURN(scope, newList(globalObject, keys.span()));
}

static JSValue proxyRepr(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(getVM(globalObject), "{...}"_s);
    PyDict* dict = proxyToDict(globalObject, self);
    if (failed(scope, dict))
        return { };
    RELEASE_AND_RETURN(scope, repr(globalObject, dict));
}

template<CompareOperator op>
static JSValue proxyCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    if (isFrameLocalsProxy(globalObject, other)) {
        bool isSame = frameOfProxy(self) == frameOfProxy(other);
        if (op == CompareOperator::Eq)
            return jsBoolean(isSame);
        if (op == CompareOperator::NotEq)
            return jsBoolean(!isSame);
    } else if (PyDict::is(other)) {
        PyDict* dict = proxyToDict(globalObject, self);
        if (failed(scope, dict))
            return { };
        RELEASE_AND_RETURN(scope, richCompare(globalObject, dict, other, op));
    }
    return runtime(globalObject).notImplemented();
}

static bool isDictOrProxy(JSGlobalObject* globalObject, JSValue value)
{
    return PyDict::is(value) || isFrameLocalsProxy(globalObject, value);
}

static JSValue proxyUnion(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* result = PyDict::create(globalObject);
    dictUpdate(globalObject, result, left);
    RETURN_IF_EXCEPTION(scope, { });
    dictUpdate(globalObject, result, right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue proxyOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return isDictOrProxy(globalObject, other) ? proxyUnion(globalObject, self, other) : runtime(globalObject).notImplemented();
}

static JSValue proxyROr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    return isDictOrProxy(globalObject, other) ? proxyUnion(globalObject, other, self) : runtime(globalObject).notImplemented();
}

// False: an exception, or, without one, `other` is neither a dict nor a proxy.
static bool proxyMerge(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isDictOrProxy(globalObject, other))
        return false;
    JSValue keys = callMethod(globalObject, other, internString(vm, "keys"_s));
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, forEach(globalObject, keys, [&](JSValue key) {
        JSValue value = getItem(globalObject, other, key);
        RETURN_IF_EXCEPTION(scope, false);
        proxyStore(globalObject, self, key, value);
        return !scope.exception();
    }));
}

// As in CPython, whatever goes wrong looks as if the operand were of the wrong type.
static JSValue proxyIOr(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    bool isMerged = proxyMerge(globalObject, self, other);
    EXCEPTION_ASSERT(!scope.exception() || !isMerged);
    if (isMerged)
        return self;
    takePendingException(globalObject);
    return runtime(globalObject).notImplemented();
}

static JSValue proxyUpdate(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isMerged = proxyMerge(globalObject, self, other);
    EXCEPTION_ASSERT(!scope.exception() || !isMerged);
    if (isMerged)
        return jsUndefined();
    takePendingException(globalObject);
    return raiseTypeError(globalObject, scope, "update() argument must be dict or another FrameLocalsProxy"_s);
}

static JSValue proxyGet(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() < 1 || args.size() > 2)
        return raiseTypeError(globalObject, scope, "get expected 1 or 2 arguments"_s);
    JSValue value = proxyLookup(globalObject, self, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return value ? value : args.size() == 2 ? args[1] : jsUndefined();
}

static JSValue proxySetDefault(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() < 1 || args.size() > 2)
        return raiseTypeError(globalObject, scope, "setdefault expected 1 or 2 arguments"_s);
    JSValue value = proxyLookup(globalObject, self, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    value = args.size() == 2 ? args[1] : jsUndefined();
    proxyStore(globalObject, self, args[0], value);
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

static JSValue proxyPop(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, checkPositional(globalObject, "pop"_s, args, 1, 2)))
        return { };
    PyFrame* frame = frameOfProxy(self);
    int index = indexOfVariable(globalObject, FrameVariables(vm, frame), args[0], ProxyAccess::Write);
    RETURN_IF_EXCEPTION(scope, { });
    if (index >= 0)
        return raiseValueError(globalObject, scope, "cannot remove local variables from FrameLocalsProxy"_s);
    JSValue value = frame->extraLocals() ? frame->extraLocals()->take(globalObject, args[0]) : JSValue();
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;
    if (args.size() == 2)
        return args[1];
    RELEASE_AND_RETURN(scope, raiseKeyError(globalObject, scope, args[0]));
}

static JSValue proxyNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "FrameLocalsProxy expected 1 argument, got "_s, args.size());
    if (!PyFrame::is(args[0]))
        return raiseTypeError(globalObject, scope, "expect frame, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, args[0])));
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, "FrameLocalsProxy takes no keyword arguments"_s);
    RELEASE_AND_RETURN(scope, PyWrapperObject::create(globalObject, runtime(globalObject).frameLocalsProxyType(), args[0]));
}

static constexpr NativeDef frameLocalsProxyMethods[] = {
    PY_NEW(proxyNew, "($type, *args, **kwargs)"),
    PY_SLOT("__len__", proxyLen, "($self, /)"),
    PY_METHOD("__getitem__", proxyGetItem, "($self, object, /)"),
    PY_SLOT("__setitem__", proxySetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", proxyDelItem, "($self, key, /)"),
    PY_METHOD("__contains__", proxyContains, "($self, object, /)"),
    PY_SLOT("__iter__", proxyIter, "($self, /)"),
    PY_SLOT("__repr__", proxyRepr, "($self, /)"),
    PY_SLOT("__eq__", proxyCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", proxyCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", proxyCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", proxyCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", proxyCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", proxyCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__or__", proxyOr, "($self, value, /)"),
    PY_SLOT("__ror__", proxyROr, "($self, value, /)"),
    PY_SLOT("__ior__", proxyIOr, "($self, value, /)"),
    PY_METHOD("__reversed__", proxyReversed, "($self, /)"),
    PY_METHOD("copy", proxyCopy, "($self, /)"),
    PY_METHOD("keys", proxyList<ProxyList::Keys>, "($self, /)"),
    PY_METHOD("values", proxyList<ProxyList::Values>, "($self, /)"),
    PY_METHOD("items", proxyList<ProxyList::Items>, "($self, /)"),
    PY_METHOD("get", proxyGet, "($self, *args)"),
    PY_METHOD("pop", proxyPop, "($self, *args)"),
    PY_METHOD("setdefault", proxySetDefault, "($self, *args)"),
    PY_METHOD("update", proxyUpdate, "($self, object, /)"),
};

// ---- Tracebacks ----

PyTraceback::PyTraceback(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyTraceback* PyTraceback::createForJS(JSGlobalObject* globalObject, JSValue next, CodeBlock* codeBlock, BytecodeIndex index)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* traceback = new (NotNull, allocateCell<PyTraceback>(vm)) PyTraceback(vm, rt.structure<PyTraceback>(), rt.tracebackType());
    traceback->finishCreation(vm);
    traceback->m_next.set(vm, traceback, next);
    traceback->m_codeBlock.set(vm, traceback, codeBlock);
    traceback->m_index = index;
    traceback->m_lastI = -1;
    traceback->m_isOfJS = true;
    return traceback;
}

// The frame is one of code of Python's that does nothing, and says that it is from that file and has that name.
void PyTraceback::makeFrameOfJS()
{
    CodeBlock* codeBlock = m_codeBlock.get();
    // (Not that of the code, which may be of a realm that has no Python.)
    JSGlobalObject* globalObject = realm();
    VM& vm = getVM(globalObject);
    // An exception may be on its way.
    SuspendExceptionScope suspended(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PositionInJS position = positionInJS(vm, codeBlock, m_index);
    PyCode* code = PyCode::tryFrom(compileSource(globalObject, { }, position.filename, "exec"_s, 0, -1));
    scope.assertNoException();
    RELEASE_ASSERT(code);
    CodeInfo& info = code->codeInfo();
    info.name = Identifier::fromString(vm, position.function);
    info.qualname = position.function;
    PyDict* names = namespaceOfFileOfJS(globalObject, position.filename);
    scope.assertNoException();
    PyFrame* frame = PyFrame::createSuspended(globalObject, PyFunction::create(globalObject, code->executable(), names), 0);
    scope.assertNoException();
    frame->setLineNumber(position.line);
    m_lineNumber = position.line;
    m_frame.set(vm, this, frame);
    m_codeBlock.clear();
}

void PyTraceback::findPlace()
{
    if (m_isOfJS)
        return makeFrameOfJS();
    m_lineNumber = lineNumberForBytecodeIndex(m_codeBlock.get(), m_index);
    m_lastI = shownOffset(visibleOffset(m_codeBlock->unlinkedCodeBlock()->pythonMarks(), m_index.offset()));
    m_codeBlock.clear();
}

PyTraceback* PyTraceback::create(JSGlobalObject* globalObject, JSValue next, PyFrame* frame, CodeBlock* codeBlock, BytecodeIndex index)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* traceback = new (NotNull, allocateCell<PyTraceback>(vm)) PyTraceback(vm, rt.structure<PyTraceback>(), rt.tracebackType());
    traceback->finishCreation(vm);
    traceback->m_next.set(vm, traceback, next);
    traceback->m_frame.set(vm, traceback, frame);
    traceback->m_codeBlock.set(vm, traceback, codeBlock);
    traceback->m_index = index;
    return traceback;
}

PyTraceback* PyTraceback::create(JSGlobalObject* globalObject, JSValue next, PyFrame* frame, int lineNumber, int lastI)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* traceback = new (NotNull, allocateCell<PyTraceback>(vm)) PyTraceback(vm, rt.structure<PyTraceback>(), rt.tracebackType());
    traceback->finishCreation(vm);
    traceback->m_next.set(vm, traceback, next);
    traceback->m_frame.set(vm, traceback, frame);
    traceback->m_lineNumber = lineNumber;
    traceback->m_lastI = lastI;
    return traceback;
}

template<typename Visitor>
void PyTraceback::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyTraceback>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_next);
    visitor.append(thisObject->m_frame);
    visitor.append(thisObject->m_codeBlock);
}

DEFINE_VISIT_CHILDREN(PyTraceback);

bool addTracebackEntryToThrownObject(JSGlobalObject*, JSValue thrown, const ScopedLambda<JSValue(JSValue next)>& makeEntry);
bool addTracebackEntryForJSToThrownValue(JSGlobalObject*, JSValue thrown, const ScopedLambda<JSValue(JSValue next)>& makeEntry);

// An error of JS comes up to code of Python's for the first time. Nobody has kept track of where it has been, but it knows the calls
// that were in progress when it was made. Those within the call that Python made are the traceback so far, if that call is among them:
// else the error was made at some other time than now.
static JSValue tracebackOfErrorOfJS(JSGlobalObject* globalObject, JSValue thrown, CodeBlock* reached)
{
    auto* error = dynamicDowncast<ErrorInstance>(thrown);
    Vector<StackFrame>* stackTrace = error ? error->stackTrace() : nullptr;
    if (!stackTrace)
        return { };
    size_t count = 0;
    while (count < stackTrace->size() && !(stackTrace->at(count).codeBlock() && isPythonCodeBlock(stackTrace->at(count).codeBlock())))
        ++count;
    if (count == stackTrace->size() || stackTrace->at(count).codeBlock() != reached)
        return { };
    JSValue traceback;
    for (size_t i = 0; i < count; ++i) {
        CodeBlock* codeBlock = stackTrace->at(i).codeBlock();
        if (codeBlock && isShownCodeOfJS(codeBlock))
            traceback = PyTraceback::createForJS(globalObject, traceback, codeBlock, stackTrace->at(i).bytecodeIndex());
    }
    return traceback;
}

void addTracebackEntryForJS(JSGlobalObject* globalObject, JSValue thrown, CallFrame* callFrame, CodeBlock* codeBlock, BytecodeIndex index, bool throwsAgain, bool& hasNoTraceback)
{
    exceptionWillBeSeen(thrown);
    auto& caught = runtime(globalObject).caughtToBeThrownAgainInJS;
    if (hasNoTraceback) {
        // There are no entries, so none can come twice.
        caught.callFrame = nullptr;
        return;
    }
    if (!isShownCodeOfJS(codeBlock))
        return;
    // Optimized code that catches goes on as baseline code, so what throws again there is another code block of the same function.
    codeBlock = codeBlock->baselineAlternative();
    bool isThrownAgain = caught.callFrame == callFrame && caught.codeBlock == codeBlock && caught.thrown == JSValue::encode(thrown);
    caught = { };
    if (isThrownAgain) {
        if (throwsAgain)
            caught = { callFrame, codeBlock, JSValue::encode(thrown) };
        return;
    }
    auto makeEntry = [&](JSValue next) -> JSValue {
        return PyTraceback::createForJS(globalObject, next, codeBlock, index);
    };
    if (!addTracebackEntryForJSToThrownValue(globalObject, thrown, makeEntry))
        hasNoTraceback = true;
    else if (throwsAgain)
        caught = { callFrame, codeBlock, JSValue::encode(thrown) };
}

// ---- Entries that are only noted: see PyRuntime.h ----

void Runtime::noteEntry(PyException* exception, CallFrame* callFrame, CodeBlock* codeBlock, unsigned offset)
{
    ASSERT(exception->isUnseen() && !exception->hasNotedEntry());
    ASSERT(m_notedEntries.isEmpty() || m_notedEntries.last().callFrame >= callFrame);
    exception->setHasNotedEntry(true);
    Locker locker { m_notedEntriesLock };
    m_notedEntries.append({ exception, callFrame, codeBlock, offset });
}

// The stack grows downwards.
static bool isFrameOfRunningThread(VM& vm, CallFrame* wanted)
{
    EntryFrame* entryFrame = vm.topEntryFrame;
    CallFrame* callFrame = vm.topCallFrame;
    while (callFrame && callFrame < wanted)
        callFrame = callFrame->callerFrame(entryFrame);
    return callFrame == wanted;
}

#if ASSERT_ENABLED
void Runtime::assertNoNotedEntryOf(CallFrame* callFrame) const
{
    for (auto& entry : m_notedEntries)
        ASSERT(entry.callFrame != callFrame);
}
#endif

static void makeEntry(JSGlobalObject* globalObject, const Runtime::NotedEntry& entry)
{
    VM& vm = getVM(globalObject);
    ASSERT(!entry.exception->hasNotedEntry());
    // Nothing is followed that is read out of the frame: the note keeps its code block alive, whatever has become of the frame.
    CallFrame* callFrame = entry.callFrame;
    bool isStillThere = !callFrame->isNativeCalleeFrame() && callFrame->codeBlock() == entry.codeBlock && callFrame->jsCallee() != entry.codeBlock->globalObject()->zombieFrameCallee();
    ASSERT(isStillThere);
    if (!isStillThere)
        return;
    DeferGC deferGC(vm);
    PyFrame* frame = PyFrame::of(globalObject, callFrame);
    // As exceptionIsRaised() would have had it, had there been a frame object.
    if (frame->raisedAt < 0)
        frame->raisedAt = entry.offset;
    entry.exception->setTraceback(vm, PyTraceback::create(globalObject, entry.exception->traceback(), frame, entry.codeBlock, BytecodeIndex(entry.offset)));
}

static bool takeOut(Runtime::NotedEntries& entries, Lock& lock, PyException* exception, Runtime::NotedEntry& result)
{
    for (size_t i = entries.size(); i--;) {
        if (entries[i].exception != exception)
            continue;
        result = entries[i];
        exception->setHasNotedEntry(false);
        Locker locker { lock };
        entries.removeAt(i);
        return true;
    }
    return false;
}

void Runtime::makeNotedEntry(PyException* exception)
{
    NotedEntry entry;
    if (takeOut(m_notedEntries, m_notedEntriesLock, exception, entry)) {
        bool isStillThere = isFrameOfRunningThread(m_vm, entry.callFrame);
        ASSERT(isStillThere);
        if (!isStillThere)
            return;
    } else {
        // It is being handled by a thread that waits for the GIL, with its frames where they were.
        bool isFound = false;
        threads().forEachThread([&](ThreadState& thread) {
            if (isFound || !thread.saved)
                return;
            for (auto& saved : thread.saved->entries) {
                if (saved.runtime == this && !isFound)
                    isFound = takeOut(saved.state.notedEntries, m_notedEntriesLock, exception, entry);
            }
        });
        ASSERT(isFound);
        if (!isFound) {
            exception->setHasNotedEntry(false);
            return;
        }
    }
    makeEntry(m_globalObject, entry);
}

void exceptionWillBeSeen(JSValue thrown)
{
    if (auto* exception = PyException::tryFrom(thrown))
        exception->willBeSeen();
}

void PyException::makeNotedEntry()
{
    runtime(realm()).makeNotedEntry(this);
}

void Runtime::makeNotedEntries()
{
    while (!m_notedEntries.isEmpty())
        m_notedEntries.last().exception->willBeSeen();
    Vector<PyException*, 8> exceptions;
    threads().forEachThread([&](ThreadState& thread) {
        if (!thread.saved)
            return;
        for (auto& saved : thread.saved->entries) {
            if (saved.runtime != this)
                continue;
            for (auto& entry : saved.state.notedEntries)
                exceptions.append(entry.exception);
        }
    });
    for (PyException* exception : exceptions)
        exception->willBeSeen();
}

void Runtime::takeOutNotedEntriesOf(CallFrame* callFrame, JSValue thrown)
{
    // (The stack grows downwards. There is none of a frame further down.)
    while (!m_notedEntries.isEmpty() && m_notedEntries.last().callFrame <= callFrame) {
        ASSERT(m_notedEntries.last().callFrame == callFrame);
        PyException* exception = m_notedEntries.last().exception;
        // It goes on, and the frame it leaves is part of where it has been. Any other was being handled here, and that is over.
        if (thrown == JSValue(exception) && m_notedEntries.last().callFrame == callFrame) {
            makeNotedEntry(exception);
            continue;
        }
        exception->setHasNotedEntry(false);
        Locker locker { m_notedEntriesLock };
        m_notedEntries.removeLast();
    }
}

void Runtime::handlerIsLeft(CallFrame* callFrame, JSValue previous)
{
    JSValue handled = setHandledExceptionInTopSlot(previous);
    if (m_notedEntries.isEmpty()) [[likely]]
        return;
    NotedEntry& entry = m_notedEntries.last();
    // (An exception that is caught again in its own handler is still being handled when the inner handler is left.)
    if (entry.callFrame != callFrame || JSValue(entry.exception) != handled || handled == previous)
        return;
    entry.exception->setHasNotedEntry(false);
    Locker locker { m_notedEntriesLock };
    m_notedEntries.removeLast();
}

// Whether the handler keeps to itself what it catches: see OpPyCatch.
static bool handlerLeavesItUnseen(CodeBlock* codeBlock, const HandlerInfo& handler)
{
    return codeBlock->instructions().at(handler.target)->as<OpPyCatch>().m_mode == static_cast<unsigned>(CatchMode::HandledUnseen);
}

JSValue addTracebackEntry(JSGlobalObject* globalObject, JSValue exceptionValue, CallFrame* callFrame, BytecodeIndex index, CallFrame* frameBefore, const HandlerInfo* handler)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    // An exception raised again where it was caught has been through this frame already.
    bool isRaisedAgain = std::exchange(rt.reraisingFrame, nullptr) == callFrame;
    // A call whose arguments do not fit never began. Nor did that of a generator expression whose first iterable is none: it is the
    // code around it that asks for the iterator, as far as a program can tell.
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (!isPastStart(codeBlock->unlinkedCodeBlock()->pythonMarks(), index.offset()))
        return exceptionValue;
    clearFrameMadeByInstruction(callFrame, codeBlock, index, frameBefore, exceptionValue);
    if (auto* exception = PyException::tryFrom(exceptionValue); !isRaisedAgain && exception && exception->isUnseen() && handler && handlerLeavesItUnseen(codeBlock, *handler) && !rt.monitoring().wantsExceptions()) {
        rt.noteEntry(exception, callFrame, codeBlock, index.offset());
        return exceptionIsRaised(globalObject, exceptionValue, callFrame, index, false);
    }
    if (!isRaisedAgain) {
        DeferGC deferGC(vm);
        auto makeEntry = [&](JSValue next) -> JSValue {
            return PyTraceback::create(globalObject, next, PyFrame::of(globalObject, callFrame), codeBlock, index);
        };
        // What JS threw has a JSError once Python has caught it: making one runs code. Until then the object keeps the traceback.
        if (auto* exception = exceptionOfThrownValueIfKnown(globalObject, exceptionValue))
            exception->setTraceback(vm, makeEntry(exception->traceback()));
        else {
            auto makeEntries = [&](JSValue next) -> JSValue {
                return makeEntry(next ? next : tracebackOfErrorOfJS(globalObject, exceptionValue, codeBlock));
            };
            addTracebackEntryToThrownObject(globalObject, exceptionValue, makeEntries);
        }
    }
    return exceptionIsRaised(globalObject, exceptionValue, callFrame, index, isRaisedAgain);
}

static JSValue tracebackNew(JSGlobalObject* globalObject, JSValue, JSValue next, JSValue frame, JSValue lastI, JSValue lineNumber)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isNone(next) && !PyTraceback::is(next))
        return raiseTypeError(globalObject, scope, "expected traceback object or None, got '"_s, typeName(globalObject, next), '\'');
    if (!PyFrame::is(frame))
        return raiseTypeError(globalObject, scope, "traceback() argument 'tb_frame' must be frame, not "_s, typeName(globalObject, frame));
    int offset = toCInt(globalObject, lastI);
    RETURN_IF_EXCEPTION(scope, { });
    int line = toCInt(globalObject, lineNumber);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTraceback::create(globalObject, isNone(next) ? JSValue() : next, PyFrame::from(frame), line, offset));
}

static JSValue tracebackGetNext(JSGlobalObject*, JSValue self)
{
    JSValue next = PyTraceback::from(self)->next();
    return next ? next : jsUndefined();
}

static void tracebackSetNext(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete tb_next attribute"_s);
        return;
    }
    if (isNone(value))
        return PyTraceback::from(self)->setNext(vm, { });
    if (!PyTraceback::is(value)) {
        raiseTypeError(globalObject, scope, "expected traceback object, got '"_s, typeName(globalObject, value), '\'');
        return;
    }
    for (JSValue cursor = value; cursor; cursor = PyTraceback::from(cursor)->next()) {
        if (cursor == self) {
            raiseValueError(globalObject, scope, "traceback loop detected"_s);
            return;
        }
    }
    PyTraceback::from(self)->setNext(vm, value);
}

static JSValue tracebackGetFrame(JSGlobalObject*, JSValue self) { return PyTraceback::from(self)->frame(); }
static JSValue tracebackGetLineNo(JSGlobalObject* globalObject, JSValue self)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    auto* traceback = PyTraceback::from(self);
    int line = traceback->lineNumber();
    // As in CPython, that stands for the line of tb_lasti.
    if (line == -1) {
        line = lineOfShownOffset(globalObject, traceback->frame()->code(globalObject), traceback->lastI());
        RETURN_IF_EXCEPTION(scope, { });
        if (line < 0)
            return jsUndefined();
    }
    return jsNumber(line);
}
static JSValue tracebackGetLastI(JSGlobalObject*, JSValue self) { return jsNumber(PyTraceback::from(self)->lastI()); }

static JSValue tracebackDir(JSGlobalObject* globalObject, JSValue)
{
    VM& vm = getVM(globalObject);
    JSValue names[] = { makeStr(vm, "tb_frame"_s), makeStr(vm, "tb_next"_s), makeStr(vm, "tb_lasti"_s), makeStr(vm, "tb_lineno"_s) };
    return newList(globalObject, names);
}

static constexpr NativeDef tracebackMethods[] = {
    PY_NEW(tracebackNew, "($type, /, tb_next, tb_frame, tb_lasti, tb_lineno)"),
    PY_METHOD("__dir__", tracebackDir, "($self, /)"),
};

static constexpr NativeGetSet tracebackGetSets[] = {
    { "tb_next"_s, tracebackGetNext, tracebackSetNext },
    PY_MEMBER("tb_frame", tracebackGetFrame),
    { "tb_lineno"_s, tracebackGetLineNo },
    PY_MEMBER("tb_lasti", tracebackGetLastI),
};

void initializeFrameTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.frameType()->initializeBuiltin(globalObject, { "frame"_s, { }, frameMethods, frameGetSets, { } }, rt.typeType(), rt.objectType());
    rt.frameLocalsProxyType()->initializeBuiltin(globalObject, { "FrameLocalsProxy"_s, { TypeFlag::Mapping }, frameLocalsProxyMethods, { }, { } }, rt.typeType(), rt.objectType());
    rt.frameLocalsProxyType()->dict()->set(rt.vm(), rt.name(Special::Hash), jsUndefined());
    rt.frameLocalsProxyType()->didChange(rt.vm());
    rt.tracebackType()->initializeBuiltin(globalObject, { "traceback"_s, { }, tracebackMethods, tracebackGetSets, { } }, rt.typeType(), rt.objectType());
}

// ---- Showing an exception ----

namespace {

class ExceptionPrinter {
public:
    explicit ExceptionPrinter(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_vm(getVM(globalObject))
    {
    }

    String print(JSValue exception)
    {
        printChain(exception);
        return m_result.toString();
    }

private:
    // Whatever goes wrong while printing is not reported.
    String textOf(JSValue value, bool useRepr = false)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        JSString* string = useRepr ? repr(m_globalObject, value) : str(m_globalObject, value);
        String result;
        if (!scope.exception())
            result = string->value(m_globalObject);
        if (scope.exception()) {
            scope.clearException();
            return { };
        }
        return result.isNull() ? emptyString() : result;
    }

    // What has been written to m_out goes to the result. Within an exception group every line is indented, behind a margin.
    void flush(char margin = '|')
    {
        String text = m_out.toString();
        m_out.clear();
        if (!m_groupDepth) {
            m_result.append(text);
            return;
        }
        unsigned start = 0;
        while (start < text.length()) {
            size_t end = text.find('\n', start);
            end = end == notFound ? text.length() : end + 1;
            appendIndent();
            m_result.append(margin, ' ', StringView(text).substring(start, end - start));
            start = end;
        }
    }

    void appendIndent()
    {
        for (unsigned i = 0; i < 2 * m_groupDepth; ++i)
            m_result.append(' ');
    }

    void printChain(JSValue value)
    {
        auto* exception = PyException::tryFrom(value);
        if (exception && !m_seen.contains(exception) && m_vm.isSafeToRecurseSoft()) {
            m_seen.append(exception);
            JSValue cause = exception->cause();
            JSValue context = exception->context();
            if (cause && !m_seen.contains(cause.asCell())) {
                printChain(cause);
                m_out.append("\nThe above exception was the direct cause of the following exception:\n\n"_s);
            } else if (!cause && context && !exception->suppressContext() && !m_seen.contains(context.asCell())) {
                printChain(context);
                m_out.append("\nDuring handling of the above exception, another exception occurred:\n\n"_s);
            }
            flush();
        }
        PyTuple* members = nullptr;
        if (exception && exception->type()->isSubtypeOf(runtime(m_globalObject).BaseExceptionGroup()))
            members = PyTuple::tryFrom(exception->field(ExceptionField::exceptionGroupExceptions));
        if (!members) {
            printOne(value);
            flush();
            return;
        }
        printGroup(exception, members);
    }

    // As traceback.TracebackException.format() draws it:
    //   + Exception Group Traceback (most recent call last):
    //   |   File ...
    //   | ExceptionGroup: message (2 sub-exceptions)
    //   +-+---------------- 1 ----------------
    //     | ValueError: 1
    //     +---------------- 2 ----------------
    //     | TypeError: 2
    //     +------------------------------------
    void printGroup(PyException* exception, PyTuple* members)
    {
        static constexpr unsigned maxGroupWidth = 15;
        static constexpr unsigned maxGroupDepth = 10;
        if (m_groupDepth > maxGroupDepth) {
            m_out.append("... (max_group_depth is "_s, maxGroupDepth, ")\n"_s);
            flush();
            return;
        }
        bool isTopLevel = !m_groupDepth;
        if (isTopLevel)
            ++m_groupDepth;
        if (exception->traceback()) {
            m_out.append("Exception Group Traceback (most recent call last):\n"_s);
            flush(isTopLevel ? '+' : '|');
        }
        printOne(exception, false);
        flush();

        unsigned count = members->length();
        unsigned shown = count <= maxGroupWidth ? count : maxGroupWidth + 1;
        m_needsClose = false;
        for (unsigned i = 0; i < shown; ++i) {
            bool isLast = i == shown - 1;
            // One of the calls below may draw the closing line.
            if (isLast)
                m_needsClose = true;
            bool isTruncated = i >= maxGroupWidth;
            appendIndent();
            m_result.append(i ? "  "_s : "+-"_s, "+---------------- "_s);
            if (isTruncated)
                m_result.append("..."_s);
            else
                m_result.append(i + 1);
            m_result.append(" ----------------\n"_s);
            ++m_groupDepth;
            if (!isTruncated)
                printChain(members->at(i));
            else {
                unsigned remaining = count - maxGroupWidth;
                m_out.append("and "_s, remaining, " more exception"_s, remaining > 1 ? "s"_s : ""_s, '\n');
                flush();
            }
            if (isLast && m_needsClose) {
                appendIndent();
                m_result.append("+------------------------------------\n"_s);
                m_needsClose = false;
            }
            --m_groupDepth;
        }
        if (isTopLevel)
            m_groupDepth = 0;
    }

    static String sourceLine(PyFrame* frame, int lineNumber)
    {
        if (lineNumber < 1)
            return { };
        SourceProvider* provider = frame->executable()->source().provider();
        if (frame->codeInfo().filename.startsWith('<') && !frame->codeInfo().sourceIsShown)
            return { };
        StringView source = provider->source();
        unsigned start = 0;
        for (int line = 1; line < lineNumber; ++line) {
            size_t end = source.find('\n', start);
            if (end == notFound)
                return { };
            start = end + 1;
        }
        size_t end = source.find('\n', start);
        StringView text = source.substring(start, end == notFound ? source.length() - start : end - start);
        // The source is kept as bytes of UTF-8.
        Vector<uint8_t> bytes;
        for (unsigned i = 0; i < text.length(); ++i)
            bytes.append(static_cast<uint8_t>(text[i]));
        return decodeUTF8Lenient(bytes.span()).trim([](char16_t c) { return c == ' ' || c == '\t' || c == '\f' || c == '\r'; });
    }

    // What runs is not what the programmer wrote, which is in the file.
    String sourceLineOfJS(const String& filename, int lineNumber)
    {
        // Code that was given to the engine as text.
        if (filename.isEmpty())
            return { };
        auto& contents = m_filesOfJS.ensure(filename, [&] {
            return readFile(filename).value_or(Vector<uint8_t> { });
        }).iterator->value;
        std::span<const uint8_t> rest = contents.span();
        for (int line = 1; line < lineNumber; ++line) {
            size_t end = WTF::find(rest, '\n');
            if (end == notFound)
                return { };
            rest = rest.subspan(end + 1);
        }
        size_t end = WTF::find(rest, '\n');
        return decodeUTF8Lenient(end == notFound ? rest : rest.first(end)).trim([](char16_t c) { return c == ' ' || c == '\t' || c == '\f' || c == '\r'; });
    }

    void printTraceback(JSValue traceback, bool withHeader)
    {
        if (withHeader)
            m_out.append("Traceback (most recent call last):\n"_s);
        // A long run of the same line, as endless recursion leaves, is cut short.
        CodeInfo* lastInfo = nullptr;
        int lastLine = -1;
        unsigned repeats = 0;
        auto flushRepeats = [&] {
            if (repeats > 3)
                m_out.append("  [Previous line repeated "_s, repeats - 3, " more time"_s, repeats - 3 == 1 ? ""_s : "s"_s, "]\n"_s);
        };
        for (; traceback; traceback = PyTraceback::from(traceback)->next()) {
            auto* entry = PyTraceback::from(traceback);
            CodeInfo& info = entry->frame()->codeInfo();
            if (&info == lastInfo && entry->lineNumber() == lastLine)
                ++repeats;
            else {
                flushRepeats();
                repeats = 1;
                lastInfo = &info;
                lastLine = entry->lineNumber();
            }
            if (repeats > 3)
                continue;
            m_out.append("  File \""_s, info.filename, "\", line "_s, entry->lineNumber(), ", in "_s, info.name.string(), '\n');
            String line = entry->isOfJS() ? sourceLineOfJS(info.filename, entry->lineNumber()) : sourceLine(entry->frame(), entry->lineNumber());
            if (!line.isEmpty())
                m_out.append("    "_s, line, '\n');
        }
        flushRepeats();
    }

    void printSyntaxErrorLocation(PyException* exception)
    {
        JSValue filename = exception->field(ExceptionField::syntaxErrorFilename);
        JSValue lineNumber = exception->field(ExceptionField::syntaxErrorLineno);
        if (!lineNumber || !isInt(lineNumber))
            return;
        m_out.append("  File \""_s, filename && !isNone(filename) ? textOf(filename) : "<string>"_str, "\", line "_s, textOf(lineNumber), '\n');
        JSValue text = exception->field(ExceptionField::syntaxErrorText);
        if (!text || !isStr(text))
            return;
        String line = textOf(text);
        size_t newline = line.find('\n');
        if (newline != notFound)
            line = line.left(newline);
        unsigned leading = 0;
        while (leading < line.length() && (line[leading] == ' ' || line[leading] == '\t' || line[leading] == '\f'))
            ++leading;
        m_out.append("    "_s, StringView(line).substring(leading), '\n');

        auto asInt = [&](JSValue value) -> int64_t {
            if (!value || !isInt(value))
                return -1;
            return tryInt64(intValue(value)).value_or(-1);
        };
        int64_t offset = asInt(exception->field(ExceptionField::syntaxErrorOffset));
        if (offset < 1)
            return;
        int64_t endOffset = asInt(exception->field(ExceptionField::syntaxErrorEndOffset));
        int64_t endLine = asInt(exception->field(ExceptionField::syntaxErrorEndLineno));
        if (endLine != asInt(lineNumber) || endOffset <= offset)
            endOffset = offset + 1;
        int64_t start = std::max<int64_t>(offset - 1 - leading, 0);
        int64_t end = std::max<int64_t>(endOffset - 1 - leading, start + 1);
        m_out.append("    "_s);
        for (int64_t i = 0; i < start; ++i)
            m_out.append(' ');
        for (int64_t i = start; i < end; ++i)
            m_out.append('^');
        m_out.append('\n');
    }

    void printOne(JSValue value, bool withHeader = true)
    {
        Runtime& rt = runtime(m_globalObject);
        auto* exception = PyException::tryFrom(value);
        if (!exception) {
            m_out.append("TypeError: print_exception(): Exception expected for value, "_s, typeName(m_globalObject, value), " found\n"_s);
            return;
        }
        if (JSValue traceback = exception->traceback())
            printTraceback(traceback, withHeader);
        PyType* type = exception->type();
        bool isSyntaxError = type->isSubtypeOf(rt.SyntaxError());
        if (isSyntaxError)
            printSyntaxErrorLocation(exception);

        m_out.append(typeNameForMessage(m_globalObject, type));
        String message;
        if (isSyntaxError && exception->field(ExceptionField::syntaxErrorMsg) && isInt(exception->field(ExceptionField::syntaxErrorLineno) ? exception->field(ExceptionField::syntaxErrorLineno) : jsUndefined()))
            message = textOf(exception->field(ExceptionField::syntaxErrorMsg));
        else
            message = textOf(value);
        if (message.isNull())
            message = "<exception str() failed>"_s;
        else if (!isSyntaxError)
            addHint(exception, message);
        if (!message.isEmpty())
            m_out.append(": "_s, message);
        m_out.append('\n');
        printNotes(exception);
    }

    void printNotes(PyException* exception)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        auto orFailure = [](const String& text, ASCIILiteral failure) { return text.isNull() ? String(failure) : text; };
        JSValue notes = getAttrIfPresent(m_globalObject, exception, runtime(m_globalObject).names.dNotes());
        if (scope.exception()) {
            JSValue error = takePendingException(m_globalObject);
            m_out.append("Ignored error getting __notes__: "_s, orFailure(textOf(error, true), "<__notes__ repr() failed>"_s), '\n');
            return;
        }
        if (!notes || isNone(notes))
            return;
        auto printNote = [&](JSValue note) { m_out.append(orFailure(textOf(note), "<note str() failed>"_s), '\n'); };
        if (isList(notes)) {
            for (unsigned i = 0; i < listLength(asList(notes)); ++i)
                printNote(listGet(asList(notes), i));
        } else if (auto* tuple = PyTuple::tryFrom(notes)) {
            for (unsigned i = 0; i < tuple->length(); ++i)
                printNote(tuple->at(i));
        } else
            m_out.append(orFailure(textOf(notes, true), "<__notes__ repr() failed>"_s), '\n');
    }

    // ---- "Did you mean ...?" (_compute_suggestion_error() in Lib/traceback.py is the reference.) ----

    // The strs among the items of a list or the keys of a dict.
    void appendNames(Vector<String>& names, JSValue container)
    {
        auto append = [&](JSValue item) {
            if (item.isString())
                names.append(stringOf(asString(item), m_globalObject));
        };
        if (auto* dict = PyDict::tryFrom(container)) {
            JSValue key, value;
            for (unsigned position = 0; dict->next(position, key, value);)
                append(key);
        } else if (isList(container)) {
            for (unsigned i = 0; i < listLength(asList(container)); ++i)
                append(listGet(asList(container), i));
        }
    }

    static String stringOf(JSString* string, JSGlobalObject* globalObject) { return string->value(globalObject); }

    // sorted(object.__dir__()). False: that failed.
    bool namesIn(JSValue object, bool hideUnderscored, Vector<String>& names)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        Runtime& rt = runtime(m_globalObject);
        JSValue method = rt.typeOf(object)->lookup(internString(m_vm, "__dir__"_s));
        if (!method)
            return false;
        JSValue result = callSpecial(m_globalObject, method, object);
        if (failed(scope, result))
            return false;
        JSArray* list = listFromIterable(m_globalObject, result);
        RETURN_IF_EXCEPTION(scope, false);
        Vector<String> all;
        appendNames(all, list);
        std::ranges::sort(all, [](const String& a, const String& b) { return codePointCompareLessThan(a, b); });
        for (auto& name : all) {
            if (!hideUnderscored || !name.startsWith('_'))
                names.append(name);
        }
        return true;
    }

    // The frame the exception was raised in.
    PyFrame* innermostFrame(PyException* exception)
    {
        JSValue traceback = exception->traceback();
        if (!traceback)
            return nullptr;
        while (JSValue next = PyTraceback::from(traceback)->next())
            traceback = next;
        return PyTraceback::from(traceback)->frame();
    }

    JSValue selfIn(PyFrame* frame)
    {
        auto* locals = PyDict::tryFrom(frameLocals(m_globalObject, frame));
        return locals ? locals->get(internString(m_vm, "self"_s)) : JSValue();
    }

    // object.outer.name, if there is an `outer` for which that exists. No descriptor is run to find out.
    String nestedAttribute(JSValue object, JSString* name, const Vector<String>& names)
    {
        Runtime& rt = runtime(m_globalObject);
        unsigned checked = 0;
        for (auto& outer : names) {
            if (outer.startsWith('_'))
                continue;
            if (++checked > 20)
                break;
            JSString* outerName = internString(m_vm, outer);
            if (JSValue inClass = rt.typeOf(object)->lookup(outerName); inClass && rt.typeOf(inClass)->lookup(Special::Get))
                continue;
            JSValue value = getAttrIfPresent(m_globalObject, object, outerName);
            if (!value) {
                takePendingException(m_globalObject);
                continue;
            }
            bool found = instanceAttribute(m_globalObject, value, name) || rt.typeOf(value)->lookup(name) || (PyType::is(value) && PyType::from(value)->lookup(name));
            if (found)
                return makeString(outer, '.', stringOf(name, m_globalObject));
        }
        return { };
    }

    static String withASCII(const String& text)
    {
        return text.containsOnlyASCII() ? makeString('\'', text, '\'') : makeString('\'', text, "' ("_s, asciiEscape(strRepr(text)), ')');
    }

    // What a method of a built-in type is called in another language, or on the type that was probably meant: names too
    // far from the right one for their spelling to lead there. (_CROSS_LANGUAGE_HINTS in Lib/traceback.py has the list.)
    String hintForForeignName(JSValue object, const String& name)
    {
        Runtime& rt = runtime(m_globalObject);
        static constexpr ASCIILiteral useSet = "Did you mean to use a 'set' object?"_s;
        static constexpr ASCIILiteral useList = "Did you mean to use a 'list' object?"_s;
        static constexpr ASCIILiteral useDict = "Did you mean to use a 'dict' object?"_s;
        static constexpr ASCIILiteral useInt = "Did you mean to use an 'int' object? Bitwise operators are not supported by 'float'."_s;
        struct Entry {
            ASCIILiteral name;
            PyType* type;
            ASCIILiteral hint; // The name of a method, or a sentence.
        };
        PyType* list = rt.listType();
        PyType* str = rt.strType();
        PyType* dict = rt.dictType();
        PyType* tuple = rt.tupleType();
        PyType* frozenset = rt.frozensetType();
        PyType* floatType = rt.floatType();
        const Entry entries[] = {
            { "push"_s, list, "append"_s }, { "concat"_s, list, "extend"_s }, { "addAll"_s, list, "extend"_s },
            { "contains"_s, list, "Use 'x in list'."_s }, { "add"_s, list, useSet }, { "add"_s, frozenset, useSet },
            { "toUpperCase"_s, str, "upper"_s }, { "toLowerCase"_s, str, "lower"_s },
            { "trimStart"_s, str, "lstrip"_s }, { "trimEnd"_s, str, "rstrip"_s },
            { "keySet"_s, dict, "keys"_s }, { "entrySet"_s, dict, "items"_s }, { "entries"_s, dict, "items"_s },
            { "putAll"_s, dict, "update"_s }, { "put"_s, dict, "Use d[k] = v."_s },
            { "append"_s, tuple, useList }, { "extend"_s, tuple, useList }, { "insert"_s, tuple, useList },
            { "remove"_s, tuple, useList }, { "remove"_s, frozenset, useSet }, { "discard"_s, frozenset, useSet },
            { "update"_s, frozenset, useSet }, { "update"_s, rt.frozendictType(), useDict },
            { "__or__"_s, floatType, useInt }, { "__and__"_s, floatType, useInt }, { "__xor__"_s, floatType, useInt },
            { "__lshift__"_s, floatType, useInt }, { "__rshift__"_s, floatType, useInt },
        };
        PyType* type = rt.typeOf(object);
        for (auto& entry : entries) {
            if (name != entry.name || !type->isSubtypeOf(entry.type))
                continue;
            if (StringView(entry.hint).contains(' '))
                return makeString(". "_s, entry.hint);
            return makeString(". Did you mean '."_s, entry.hint, "'?"_s);
        }
        return { };
    }

    // For a module that was not found. The modules that may have been meant are those in the directories that were searched.
    String hintForMissingModule(JSString* moduleName, String& replacement)
    {
        Runtime& rt = runtime(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        String wrong = stringOf(moduleName, m_globalObject);
        JSValue standardModules = rt.sysDict()->get(internString(m_vm, "stdlib_module_names"_s));
        bool isStandardModule = standardModules && contains(m_globalObject, standardModules, moduleName);
        RETURN_IF_EXCEPTION(scope, { });
        if (isStandardModule) {
            replacement = makeString("Standard library module "_s, strRepr(wrong), " was not found"_s);
            return { };
        }
        if (!configuration().siteImport)
            return ". Site initialization is disabled, did you forget to add the site-packages directory to sys.path or to enable your virtual environment?"_s;

        JSValue searchPath = rt.sysDict()->get(rt.names.path());
        String prefix = emptyString();
        if (size_t dot = wrong.reverseFind('.'); dot != notFound) {
            prefix = wrong.left(dot + 1);
            JSValue parent = rt.modules()->get(internString(m_vm, wrong.left(dot)));
            searchPath = parent ? getAttrIfPresent(m_globalObject, parent, rt.names.dPath()) : JSValue();
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!searchPath)
            return { };
        JSArray* directories = listFromIterable(m_globalObject, searchPath);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue posix = importModule(m_globalObject, "posix"_s);
        RETURN_IF_EXCEPTION(scope, { });
        Vector<String> names;
        for (unsigned i = 0; i < listLength(directories); ++i) {
            JSValue entry = listGet(directories, i);
            if (!entry.isString())
                continue;
            String directory = stringOf(asString(entry), m_globalObject);
            if (directory.isEmpty())
                directory = "."_s;
            JSValue listing = callMethod(m_globalObject, posix, internString(m_vm, "listdir"_s), makeStr(m_vm, directory));
            if (scope.exception()) {
                takePendingException(m_globalObject);
                continue;
            }
            Vector<String> files;
            appendNames(files, listing);
            for (auto& file : files) {
                if (file == "__pycache__"_s)
                    continue;
                if (file.endsWith(".py"_s)) {
                    if (kindOfFile(FileSystem::pathByAppendingComponent(directory, file)) == VirtualFileKind::File)
                        names.append(makeString(prefix, StringView(file).left(file.length() - 3)));
                } else if (!file.contains('.') && kindOfFile(FileSystem::pathByAppendingComponent(directory, file)) == VirtualFileKind::Directory)
                    names.append(makeString(prefix, file));
            }
        }
        String suggestion = calculateSuggestion(names, wrong);
        return suggestion.isNull() ? String() : makeString(". Did you mean: '"_s, suggestion, "'?"_s);
    }

    // Adds to the message what may help, or replaces it.
    void addHint(PyException* exception, String& message)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        String replacement;
        String hint = computeHint(exception, replacement);
        if (scope.exception()) {
            scope.clearException();
            return;
        }
        if (!replacement.isNull())
            message = replacement;
        else if (!hint.isNull())
            message = makeString(message, hint);
    }

    String computeHint(PyException* exception, String& replacement)
    {
        Runtime& rt = runtime(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyType* type = exception->type();
        auto asName = [&](JSValue value) -> JSString* { return value && value.isString() ? internString(m_globalObject, asString(value)) : nullptr; };

        if (type->isSubtypeOf(rt.ImportError())) {
            JSString* wrongName = asName(exception->field(ExceptionField::importErrorNameFrom));
            JSString* moduleName = asName(exception->field(ExceptionField::importErrorName));
            if (!moduleName)
                return { };
            if (!wrongName) {
                if (!type->isSubtypeOf(rt.ModuleNotFoundError()))
                    return { };
                RELEASE_AND_RETURN(scope, hintForMissingModule(moduleName, replacement));
            }
            JSValue module = importModule(m_globalObject, moduleName);
            RETURN_IF_EXCEPTION(scope, { });
            String wrong = stringOf(wrongName, m_globalObject);
            Vector<String> names;
            bool listed = namesIn(module, !wrong.startsWith('_'), names);
            RETURN_IF_EXCEPTION(scope, { });
            String suggestion = listed ? calculateSuggestion(names, wrong) : String();
            return suggestion.isNull() ? String() : makeString(". Did you mean: "_s, withASCII(suggestion), '?');
        }

        if (type->isSubtypeOf(rt.AttributeError())) {
            JSString* wrongName = asName(exception->field(ExceptionField::attributeErrorName));
            JSValue object = exception->field(ExceptionField::attributeErrorObj);
            if (!wrongName || !object)
                return { };
            String wrong = stringOf(wrongName, m_globalObject);
            if (String hint = hintForForeignName(object, wrong); !hint.isNull())
                return hint;
            bool hideUnderscored = !wrong.startsWith('_');
            if (hideUnderscored) {
                // A method may well mean a private attribute of its own object.
                if (PyFrame* frame = innermostFrame(exception)) {
                    if (JSValue self = selfIn(frame); self && isIdentical(self, object))
                        hideUnderscored = false;
                }
            }
            Vector<String> names;
            bool listed = namesIn(object, hideUnderscored, names);
            RETURN_IF_EXCEPTION(scope, { });
            if (!listed)
                return { };
            String suggestion = calculateSuggestion(names, wrong);
            if (suggestion.isNull()) {
                suggestion = nestedAttribute(object, wrongName, names);
                RETURN_IF_EXCEPTION(scope, { });
            }
            if (suggestion.isNull())
                return { };
            if (suggestion.containsOnlyASCII())
                return makeString(". Did you mean '."_s, suggestion, "' instead of '."_s, wrong, "'?"_s);
            return makeString(". Did you mean '."_s, suggestion, "' ("_s, asciiEscape(strRepr(suggestion)), ") instead of '."_s, wrong, "' ("_s, asciiEscape(strRepr(wrong)), ")?"_s);
        }

        if (type->isSubtypeOf(rt.NameError())) {
            JSString* wrongName = asName(exception->field(ExceptionField::nameErrorName));
            if (!wrongName)
                return { };
            String wrong = stringOf(wrongName, m_globalObject);
            String suggestion;
            if (PyFrame* frame = innermostFrame(exception)) {
                Vector<String> names;
                appendNames(names, frameLocals(m_globalObject, frame));
                appendNames(names, frame->function()->globals());
                appendNames(names, frame->function()->builtins());
                // In a method, it may be an attribute of the instance.
                bool isAttributeOfSelf = false;
                if (JSValue self = selfIn(frame)) {
                    isAttributeOfSelf = hasAttr(m_globalObject, self, wrongName);
                    if (scope.exception()) {
                        takePendingException(m_globalObject);
                        isAttributeOfSelf = false;
                    }
                }
                suggestion = isAttributeOfSelf ? makeString("self."_s, wrong) : calculateSuggestion(names, wrong);
            }
            StrBuilder hint;
            if (!suggestion.isNull())
                hint.append(". Did you mean: "_s, withASCII(suggestion), '?');
            JSValue standardModules = rt.sysDict()->get(internString(m_vm, "stdlib_module_names"_s));
            bool isStandardModule = standardModules && contains(m_globalObject, standardModules, wrongName);
            RETURN_IF_EXCEPTION(scope, { });
            if (isStandardModule)
                hint.append(suggestion.isNull() ? ". Did"_s : " Or did"_s, " you forget to import '"_s, wrong, "'?"_s);
            return hint.toString();
        }
        return { };
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    StrBuilder m_result;
    StrBuilder m_out; // What has not been indented yet.
    Vector<JSCell*, 8> m_seen;
    HashMap<String, Vector<uint8_t>> m_filesOfJS;
    unsigned m_groupDepth { 0 };
    bool m_needsClose { false };
};

}

String formatException(JSGlobalObject* globalObject, JSValue exception)
{
    return ExceptionPrinter(globalObject).print(exception);
}

// traceback._print_exception_bltin(exception[, file]). False: there is no such thing, or it failed. Nothing is left pending.
static bool printWithTracebackModule(JSGlobalObject* globalObject, JSValue exception, JSValue file)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto hasFailed = [&] {
        if (!scope.exception()) [[likely]]
            return false;
        if (!vm.hasPendingTerminationException())
            scope.clearException();
        return true;
    };
    JSValue module = importModule(globalObject, "traceback"_s);
    if (hasFailed())
        return false;
    JSValue function = getAttrIfPresent(globalObject, module, internString(vm, "_print_exception_bltin"_s));
    if (hasFailed() || !function || !isCallable(globalObject, function))
        return false;
    if (file)
        call(globalObject, function, exception, file);
    else
        call(globalObject, function, exception);
    return !hasFailed();
}

void printException(JSGlobalObject* globalObject, JSValue exception, JSValue file, bool tracebackModuleIsToldTheFile)
{
    VM& vm = getVM(globalObject);
    if (printWithTracebackModule(globalObject, exception, tracebackModuleIsToldTheFile ? file : JSValue()) || vm.hasPendingTerminationException())
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    writeToFile(globalObject, file, makeStr(globalObject, formatException(globalObject, exception)));
    if (!scope.exception())
        callMethod(globalObject, file, runtime(globalObject).names.flush());
    if (scope.exception() && !vm.hasPendingTerminationException())
        scope.clearException();
}

void printException(JSGlobalObject* globalObject, JSValue exception)
{
    Runtime& rt = runtime(globalObject);
    JSValue file = rt.sysDict()->get(rt.names.stderr_());
    // There is none before the interpreter has started, and what goes wrong until then has to be said all the same.
    if (!file)
        return writeToStderr(globalObject, formatException(globalObject, exception));
    if (!isNone(file))
        printException(globalObject, exception, file, false);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
