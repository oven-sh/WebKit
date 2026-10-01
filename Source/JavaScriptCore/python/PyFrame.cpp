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

#include "config.h"
#include "PyFrame.h"

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

using namespace Python;

const ClassInfo PyFrame::s_info = { "frame"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyFrame) };

Structure* PyFrame::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

PyFrame::PyFrame(VM& vm, Structure* structure, JSFunction* function, unsigned variableCount)
    : Base(vm, structure)
    , m_function(function, WriteBarrierEarlyInit)
    , m_variableCount(variableCount)
{
    for (unsigned i = 0; i < variableCount; ++i)
        variables()[i].clear();
}

PyFrame* PyFrame::create(VM& vm, JSGlobalObject* globalObject, JSFunction* function)
{
    unsigned variableCount = function->jsExecutable()->unlinkedExecutable()->pythonInfo()->details->frameVariables.size();
    Structure* structure = globalObject->pyRealm()->typeFrame()->instanceStructure();
    auto* frame = new (NotNull, allocateCell<PyFrame>(vm, allocationSize(variableCount))) PyFrame(vm, structure, function, variableCount);
    frame->finishCreation(vm);
    return frame;
}

PyFrame* PyFrame::forWhatIsNotRun(VM& vm, JSGlobalObject* globalObject, JSFunction* function)
{
    Python::ensureCodeDetails(vm, function->jsExecutable());
    PyFrame* frame = create(vm, globalObject, function);
    frame->m_isOver = true;
    frame->m_scope.set(vm, frame, function->scope());
    frame->m_bytecodeIndex = BytecodeIndex(unlinkedCodeBlockOf(vm, function->jsExecutable())->instructions().at(frame->details().enterOffset).next().offset());
    return frame;
}

template<typename Visitor>
void PyFrame::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyFrame>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_function);
    visitor.append(thisObject->m_generator);
    visitor.append(thisObject->m_back);
    visitor.append(thisObject->m_scope);
    visitor.append(thisObject->m_namespace);
    visitor.append(thisObject->m_extraLocals);
    visitor.append(thisObject->m_trace);
    visitor.append(thisObject->m_pendingCallable);
    visitor.append(thisObject->m_pendingArgument);
    visitor.appendValues(thisObject->variables(), thisObject->m_variableCount);
}

DEFINE_VISIT_CHILDREN(PyFrame);

// One of CodeBlock::registersSeenFromOutside(), of a frame that is on the stack. Code that has been compiled with more care keeps them where it sees fit. They are there whenever anything that it has called is running,
// which is the only time that anything can be looking.
static Register& registerOf(CallFrame* callFrame, VirtualRegister virtualRegister)
{
    return callFrame->uncheckedR(callFrame->codeBlock()->machineRegisterSeenFromOutside(virtualRegister));
}

static JSGenerator* generatorOf(CallFrame* callFrame)
{
    return uncheckedDowncast<JSGenerator>(callFrame->uncheckedArgument(static_cast<int>(JSGenerator::Argument::Generator) - 1).asCell());
}

static PyRealm::WaitingGenerator* waitingEntryOf(JSGenerator* generator)
{
    for (auto* waiting = generator->globalObject()->pyRealm()->innermostWaitingGenerator(); waiting; waiting = waiting->outer) {
        if (waiting->generator == generator)
            return waiting;
    }
    return nullptr;
}

// Where it is, which is where it will go on from if it is at a yield. One that is waiting while something is thrown into what it waits on is at a yield too, though it says that it is running.
static int32_t generatorStateOf(JSGenerator* generator)
{
    int32_t state = generator->internalField(static_cast<unsigned>(JSGenerator::Field::State)).get().asInt32();
    if (state == static_cast<int32_t>(JSGenerator::State::Executing)) {
        if (auto* waiting = waitingEntryOf(generator)) [[unlikely]]
            return waiting->state;
    }
    return state;
}

PyFrame* PyFrame::forCallFrameIfExists(VM& vm, CallFrame* callFrame)
{
    const FunctionInfo& info = *pythonInfoOfFrame(callFrame);
    JSValue existing = info.isGeneratorBody ? generatorOf(callFrame)->getDirect(vm, vm.pythonNames().private_frame) : registerOf(callFrame, info.details->frameObjectRegister).jsValue();
    return existing && existing.isCell() ? uncheckedDowncast<PyFrame>(existing.asCell()) : nullptr;
}

PyFrame* PyFrame::forCallFrame(VM& vm, CallFrame* callFrame)
{
    if (PyFrame* existing = forCallFrameIfExists(vm, callFrame))
        return existing;
    JSGlobalObject* globalObject = callFrame->codeBlock()->globalObject();
    const FunctionInfo& info = *pythonInfoOfFrame(callFrame);
    if (info.isGeneratorBody)
        return forGenerator(globalObject, generatorOf(callFrame));
    PyFrame* frame = create(vm, globalObject, uncheckedDowncast<JSFunction>(callFrame->jsCallee()));
    frame->m_callFrame = callFrame;
    registerOf(callFrame, info.details->frameObjectRegister) = JSValue(frame);
    return frame;
}

PyFrame* PyFrame::forGenerator(JSGlobalObject* globalObject, JSGenerator* generator)
{
    VM& vm = globalObject->vm();
    auto& name = vm.pythonNames().private_frame;
    if (JSValue existing = generator->getDirect(vm, name))
        return uncheckedDowncast<PyFrame>(existing.asCell());
    if (generatorStateOf(generator) == static_cast<int32_t>(JSGenerator::State::Completed))
        return nullptr;
    auto* body = uncheckedDowncast<JSFunction>(generator->internalField(static_cast<unsigned>(JSGenerator::Field::Next)).get().asCell());
    ensureCodeDetails(vm, body->jsExecutable());
    PyFrame* frame = create(vm, globalObject, body);
    frame->m_generator.set(vm, frame, generator);
    generator->putDirect(vm, name, frame);
    return frame;
}

FunctionExecutable* PyFrame::executable() const
{
    return m_function->jsExecutable();
}

const FunctionInfo& PyFrame::functionInfo() const
{
    return *executable()->unlinkedExecutable()->pythonInfo();
}

const CodeDetails& PyFrame::details() const
{
    return *functionInfo().details;
}

PyFrame::State PyFrame::state() const
{
    if (m_isOver)
        return State::Over;
    if (!m_generator)
        return State::Running;
    int32_t state = generatorStateOf(m_generator.get());
    if (state == static_cast<int32_t>(JSGenerator::State::Executing))
        return State::Running;
    if (state == static_cast<int32_t>(JSGenerator::State::Init))
        return State::NotStarted;
    // One that finished without leaving a frame was closed before it had started. It still has what it was called with.
    if (state == static_cast<int32_t>(JSGenerator::State::Completed))
        return State::NotStarted;
    return State::Suspended;
}

bool PyFrame::isWaitingWhileSaidToRun() const { return m_generator && waitingEntryOf(m_generator.get()); }

CallFrame* PyFrame::callFrame(VM& vm) const
{
    if (m_callFrame)
        return m_callFrame;
    if (state() != State::Running)
        return nullptr;
    // A generator has a new frame each time it is resumed, and it may be JavaScript that resumed it. It is somewhere on the stack.
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* frame = vm.topCallFrame; frame; frame = frame->callerFrame(entryFrame)) {
        const FunctionInfo* info = pythonInfoOfFrame(frame);
        if (info && info->isGeneratorBody && generatorOf(frame) == m_generator.get())
            return frame;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

WriteBarrierBase<Unknown>* PyFrame::savedRegister(VM& vm, VirtualRegister location) const
{
    // BytecodeGeneratorification gives each register that is saved a variable of its own, named for the number of the register.
    auto* saved = dynamicDowncast<JSLexicalEnvironment>(m_generator->internalField(static_cast<unsigned>(JSGenerator::Field::Frame)).get());
    if (!saved)
        return nullptr;
    SymbolTableEntry::Fast entry = saved->symbolTable()->get(Identifier::from(vm, static_cast<unsigned>(location.toLocal())).impl());
    return entry.isNull() ? nullptr : &saved->variableAt(entry.scopeOffset());
}

JSScope* PyFrame::scope(VM& vm)
{
    switch (state()) {
    case State::Running:
        return registerOf(callFrame(vm), details().scopeRegister).Register::scope();
    case State::Suspended:
        if (auto* slot = savedRegister(vm, details().scopeRegister))
            return uncheckedDowncast<JSScope>(slot->get().asCell());
        [[fallthrough]];
    case State::NotStarted:
        return m_function->scope();
    case State::Over:
        return m_scope.get();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Identifier& PyFrame::variableName(unsigned index) const
{
    return details().frameVariables[index].name;
}

bool PyFrame::isHiddenVariable(unsigned index) const
{
    return details().frameVariables[index].isOnlyOfComprehensions;
}

bool PyFrame::hasHiddenVariable(VM& vm)
{
    for (unsigned i = 0; i < m_variableCount; ++i) {
        if (isHiddenVariable(i) && variable(vm, i))
            return true;
    }
    return false;
}

// Where the variable is: that of the innermost comprehension that has one by the name and is being run, if any is. False if there is no such variable for the time being.
bool PyFrame::locate(VM& vm, unsigned index, VirtualRegister& location)
{
    const CodeDetails& details = this->details();
    const auto& variable = details.frameVariables[index];
    location = variable.location;
    if (details.comprehensionVariables.isEmpty()) [[likely]]
        return true;
    // What an exception has left has left its comprehensions, and so has what has returned.
    State state = this->state();
    if (state == State::Running || state == State::Suspended) {
        unsigned offset = bytecodeIndex(vm)->offset();
        for (auto& candidate : details.comprehensionVariables | std::views::reverse) {
            if (candidate.frameVariable == index && candidate.begin <= offset && offset < candidate.end) {
                location = candidate.location;
                return true;
            }
        }
    }
    return !variable.isOnlyOfComprehensions;
}

WriteBarrierBase<Unknown>* PyFrame::heapSlot(VM& vm, unsigned index, VirtualRegister location, JSCell*& owner)
{
    const auto& variable = details().frameVariables[index];
    if (!location.isValid()) {
        ScopeOffset offset;
        JSLexicalEnvironment* environment = findVariable(scope(vm), variable.name.impl(), offset);
        if (!environment)
            return nullptr;
        // What is there may be a cell that has the variable.
        if (functionInfo().variablesGivenAsCells.contains(variable.name)) [[unlikely]]
            return Python::variableOfCell(environment->variableAt(offset).get(), owner);
        owner = environment;
        return &environment->variableAt(offset);
    }
    switch (state()) {
    case State::Running:
        RELEASE_ASSERT_NOT_REACHED();
    case State::NotStarted:
        return nullptr;
    case State::Suspended:
        owner = m_generator->internalField(static_cast<unsigned>(JSGenerator::Field::Frame)).get().asCell();
        return savedRegister(vm, location);
    case State::Over:
        owner = this;
        return &variables()[index];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue PyFrame::variable(VM& vm, unsigned index)
{
    VirtualRegister location;
    if (!locate(vm, index, location))
        return { };
    if (location.isValid() && state() == State::Running)
        return registerOf(callFrame(vm), location).jsValue();
    JSCell* owner = nullptr;
    auto* slot = heapSlot(vm, index, location, owner);
    return slot ? slot->get() : JSValue();
}

void PyFrame::setVariable(VM& vm, unsigned index, JSValue value)
{
    VirtualRegister location;
    if (!locate(vm, index, location))
        return;
    if (location.isValid() && state() == State::Running) {
        CallFrame* frame = callFrame(vm);
        registerOf(frame, location) = value;
        // Code that has been compiled with more care has what it had, and does not look again. It gives way to code that does when it is come back to, which takes what it finds here.
        if (CodeBlock* codeBlock = frame->codeBlock(); JITCode::isOptimizingJIT(codeBlock->jitType()))
            codeBlock->jettison(Profiler::JettisonDueToUnprofiledWatchpoint, CountReoptimization);
        return;
    }
    JSCell* owner = nullptr;
    if (auto* slot = heapSlot(vm, index, location, owner))
        slot->set(vm, owner, value);
}

// What the code was called with. If it is a coroutine, that is a variable of the function that made it.
JSValue PyFrame::namespaceArgument(VM& vm, CallFrame* callFrame)
{
    const auto& info = functionInfo();
    if (!info.isGeneratorBody)
        return callFrame->argument(0);
    ScopeOffset offset;
    JSLexicalEnvironment* environment = findVariable(m_function->scope(), info.parameterNames[0].impl(), offset);
    return environment ? environment->variableAt(offset).get() : JSValue();
}

JSValue PyFrame::namespaceMapping(VM& vm)
{
    if (!functionInfo().usesNamespace)
        return { };
    if (m_isOver)
        return m_namespace.get();
    return namespaceArgument(vm, callFrame(vm));
}

JSObject* PyFrame::globals(VM& vm)
{
    return globalsOfScope(vm, m_function->scope());
}

void PyFrame::setExtraLocals(VM& vm, PyDict* extra)
{
    m_extraLocals.set(vm, this, extra);
}

// The frame that Python has outside a frame of the engine's, or at it if `includesIt`. It is that of the next of Python's frames out, unless a generator has been linked in before that. Only those outside `within` are looked at.
static PyFrame* nextFrameOut(VM& vm, CallFrame* from, bool includesIt, PyRealm::WaitingGenerator* candidates)
{
    EntryFrame* entryFrame = vm.topEntryFrame;
    bool hasBegun = false;
    for (CallFrame* frame = vm.topCallFrame; frame; frame = frame->callerFrame(entryFrame)) {
        if (!hasBegun) {
            if (frame != from)
                continue;
            hasBegun = true;
            if (!includesIt)
                continue;
        }
        for (auto* waiting = candidates; waiting; waiting = waiting->outer) {
            if (waiting->topCallFrame == frame && waiting->isLinked)
                return PyFrame::forGenerator(waiting->generator->globalObject(), waiting->generator);
        }
        if (isFrameToPython(frame, frame->bytecodeIndex()))
            return PyFrame::forCallFrame(vm, frame);
    }
    return nullptr;
}

PyFrame* PyFrame::back(VM& vm)
{
    if (m_isOver)
        return m_back.get();
    if (m_generator) {
        if (auto* waiting = waitingEntryOf(m_generator.get())) [[unlikely]]
            return waiting->isLinked ? nextFrameOut(vm, waiting->topCallFrame, true, waiting->outer) : nullptr;
    }
    CallFrame* frame = callFrame(vm);
    if (!frame)
        return nullptr;
    if (auto* waiting = globalObject()->pyRealm()->innermostWaitingGenerator()) [[unlikely]]
        return nextFrameOut(vm, frame, false, waiting);
    CallFrame* caller = callerOf(frame);
    return caller ? forCallFrame(vm, caller) : nullptr;
}

std::optional<BytecodeIndex> PyFrame::bytecodeIndex(VM& vm)
{
    switch (state()) {
    case State::Running:
        return callFrame(vm)->bytecodeIndex();
    case State::NotStarted:
        return std::nullopt;
    case State::Suspended: {
        // Where it will pick up again. BytecodeGeneratorification puts a switch on the state after op_enter, and its table is the last.
        UnlinkedCodeBlock* codeBlock = unlinkedCodeBlockOf(vm, executable());
        auto switchInstruction = codeBlock->instructions().at(0).next();
        ASSERT(switchInstruction->is<OpSwitchImm>());
        const auto& table = codeBlock->unlinkedSwitchJumpTable(codeBlock->numberOfUnlinkedSwitchJumpTables() - 1);
        return BytecodeIndex(switchInstruction.offset() + table.offsetForValue(generatorStateOf(m_generator.get())));
    }
    case State::Over:
        return m_bytecodeIndex ? std::optional(m_bytecodeIndex) : std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned PyFrame::line(VM& vm)
{
    if (m_goesOnFrom)
        return m_goesOnFrom->line;
    if (m_lineOverride >= 0)
        return m_lineOverride;
    auto index = bytecodeIndex(vm);
    // Until what was written has begun, it is where it says that it begins. So is what is from nowhere in the source, as co_lines() has it.
    if (!index || index->offset() <= details().enterOffset || index->offset() < offsetWhereSourceBegins(details(), unlinkedCodeBlockOf(vm, executable()))) {
        // A module is on the line before its first, as co_lines() has it.
        CodeKind kind = functionInfo().kind;
        bool isWhole = kind == CodeKind::Module || kind == CodeKind::Interactive || kind == CodeKind::Expression;
        return functionInfo().firstLine + functionInfo().lineDelta - (index && isWhole);
    }
    return lineAt(vm, *index);
}

unsigned PyFrame::lineAt(VM& vm, BytecodeIndex givenIndex)
{
    std::optional<BytecodeIndex> index = givenIndex;
    const SourceCode& source = executable()->source();
    return source.provider()->documentLineColumnForOffset(unlinkedCodeBlockOf(vm, executable())->expressionInfoForBytecodeIndex(*index).divot + source.startOffset()).line + functionInfo().lineDelta;
}

std::optional<std::pair<unsigned, unsigned>> PyFrame::sourceRangeAt(VM& vm, BytecodeIndex index)
{
    auto entry = unlinkedCodeBlockOf(vm, executable())->expressionInfoForBytecodeIndex(index);
    if (!entry.startOffset && !entry.endOffset)
        return std::nullopt;
    unsigned divot = entry.divot + executable()->source().startOffset();
    return std::pair { divot - entry.startOffset, divot + entry.endOffset };
}

void PyFrame::leave(VM& vm, CallFrame* callFrame, BytecodeIndex bytecodeIndex)
{
    ASSERT(!m_isOver);
    const CodeDetails& details = this->details();
    for (unsigned i = 0; i < m_variableCount; ++i) {
        if (VirtualRegister location = details.frameVariables[i].location; location.isValid())
            variables()[i].set(vm, this, registerOf(callFrame, location).jsValue());
    }
    m_scope.set(vm, this, registerOf(callFrame, details.scopeRegister).Register::scope());
    if (functionInfo().usesNamespace)
        m_namespace.set(vm, this, namespaceArgument(vm, callFrame));
    // What resumed a generator is not what called it, and is forgotten each time that it stops.
    if (CallFrame* caller = m_generator ? nullptr : callerOf(callFrame))
        m_back.set(vm, this, forCallFrame(vm, caller));
    m_bytecodeIndex = bytecodeIndex;
    m_callFrame = nullptr;
    m_generator.clear();
    m_isOver = true;
}

void PyFrame::clear()
{
    ASSERT(m_isOver);
    for (unsigned i = 0; i < m_variableCount; ++i)
        variables()[i].clear();
    m_scope.clear();
    m_namespace.clear();
    m_extraLocals.clear();
    m_trace.clear();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
