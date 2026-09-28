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
#include "PythonMonitoring.h"

#include "CodeBlock.h"
#include "FrameTracers.h"
#include "FunctionExecutable.h"
#include "JSGenerator.h"
#include "PreciseJumpTargetsInlines.h"
#include "PyFrame.h"
#include "PythonBuiltins.h"
#include <wtf/SetForScope.h>

// This follows Python/instrumentation.c and Python/legacy_tracing.c of CPython.

namespace JSC { namespace Python {

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(CodeMonitor);

static MonitoringState& stateOf(JSGlobalObject* globalObject) { return globalObject->pyRealm()->monitoring(); }

static void setOrClear(VM& vm, JSCell* owner, WriteBarrier<Unknown>& slot, JSValue value)
{
    if (value)
        slot.set(vm, owner, value);
    else
        slot.clear();
}

static constexpr uint32_t bit(MonitoringEvent event) { return 1u << static_cast<unsigned>(event); }

static constexpr ASCIILiteral eventNames[numberOfMonitoringEvents] = {
    "PY_START"_s, "PY_RESUME"_s, "PY_RETURN"_s, "PY_YIELD"_s, "CALL"_s, "LINE"_s, "INSTRUCTION"_s, "JUMP"_s, "BRANCH_LEFT"_s, "BRANCH_RIGHT"_s, "STOP_ITERATION"_s,
    "RAISE"_s, "EXCEPTION_HANDLED"_s, "PY_UNWIND"_s, "PY_THROW"_s, "RERAISE"_s, "C_RETURN"_s, "C_RAISE"_s, "BRANCH"_s,
};

// ---- Who is told of what

// The code looks at one word of the VM's, which says whether any realm has anything to tell.
static void updateWatching(JSGlobalObject* globalObject)
{
    MonitoringState& state = stateOf(globalObject);
    bool wants = state.codesWithLocalEvents;
    for (uint8_t tools : state.tools)
        wants |= !!tools;
    if (wants == state.isWatching)
        return;
    state.isWatching = wants;
    if (wants)
        globalObject->vm().addPythonWatcher();
    else
        globalObject->vm().removePythonWatcher();
}

static uint32_t eventsOf(const MonitoringState& state, unsigned tool)
{
    uint32_t events = 0;
    for (unsigned event = 0; event < numberOfUngroupedMonitoringEvents; ++event) {
        if (state.tools[event] >> tool & 1)
            events |= 1u << event;
    }
    return events;
}

static void setEvents(JSGlobalObject* globalObject, unsigned tool, uint32_t events)
{
    MonitoringState& state = stateOf(globalObject);
    for (unsigned event = 0; event < numberOfUngroupedMonitoringEvents; ++event) {
        bool wasAskedFor = state.tools[event] >> tool & 1;
        bool isAskedFor = events >> event & 1;
        if (isAskedFor && !wasAskedFor && event < numberOfLocalMonitoringEvents)
            ++state.timesAskedFor[tool][event];
        state.tools[event] &= ~(1u << tool);
        state.tools[event] |= isAskedFor << tool;
    }
    updateWatching(globalObject);
}

// What a tool asked for of a code object before it was cleared is asked for no more.
static uint8_t localToolsFor(const MonitoringState& state, const CodeMonitor& monitor, unsigned event)
{
    uint8_t tools = monitor.tools[event];
    for (unsigned tool = 0; tools >> tool; ++tool) {
        if (monitor.toolVersions[tool] != state.toolVersions[tool])
            tools &= ~(1u << tool);
    }
    return tools;
}

static uint32_t localEventsOf(const MonitoringState& state, FunctionExecutable* executable, unsigned tool)
{
    CodeMonitor* monitor = executable->pythonMonitor();
    if (!monitor)
        return 0;
    uint32_t events = 0;
    for (unsigned event = 0; event < numberOfLocalMonitoringEvents; ++event) {
        if (localToolsFor(state, *monitor, event) >> tool & 1)
            events |= 1u << event;
    }
    return events;
}

static void setLocalEvents(JSGlobalObject* globalObject, FunctionExecutable* executable, unsigned tool, uint32_t events)
{
    MonitoringState& state = stateOf(globalObject);
    CodeMonitor& monitor = executable->ensurePythonMonitor();
    auto hasAny = [&] {
        for (unsigned event = 0; event < numberOfLocalMonitoringEvents; ++event) {
            if (localToolsFor(state, monitor, event))
                return true;
        }
        return false;
    };
    bool hadAny = hasAny();
    for (unsigned event = 0; event < numberOfLocalMonitoringEvents; ++event) {
        bool wasAskedFor = localToolsFor(state, monitor, event) >> tool & 1;
        bool isAskedFor = events >> event & 1;
        if (isAskedFor && !wasAskedFor)
            monitor.forgetDisabled(tool, event);
        monitor.tools[event] &= ~(1u << tool);
        monitor.tools[event] |= isAskedFor << tool;
    }
    monitor.toolVersions[tool] = state.toolVersions[tool];
    state.codesWithLocalEvents += hasAny() - hadAny;
    updateWatching(globalObject);
}

// ---- Where something happens

struct Site {
    JSGlobalObject* globalObject;
    CallFrame* callFrame;
    JSObject* code;
    FunctionExecutable* executable; // What the code object is of, which has what has been asked for of it. What is running may be another for the same source.
    unsigned offset;
};

// Nothing if it is nowhere that a program is told of.
static std::optional<Site> siteOf(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index)
{
    if (stateOf(globalObject).callbackDepth)
        return std::nullopt;
    const FunctionInfo* info = pythonInfoOfFrame(callFrame);
    if (!info || info->visibility != ImplementationVisibility::Public)
        return std::nullopt;
    // The function that does nothing but make a generator is no frame to Python.
    if ((info->isGenerator || info->isCoroutine) && !info->isGeneratorBody)
        return std::nullopt;
    JSObject* code = codeObjectFor(globalObject, uncheckedDowncast<JSFunction>(callFrame->jsCallee())->jsExecutable());
    return Site { globalObject, callFrame, code, executableOfCode(code), index.offset() };
}

static uint8_t toolsFor(const Site& site, MonitoringEvent event)
{
    MonitoringState& state = stateOf(site.globalObject);
    if (event == MonitoringEvent::CReturn || event == MonitoringEvent::CRaise)
        event = MonitoringEvent::Call;
    unsigned index = static_cast<unsigned>(event);
    uint8_t tools = state.tools[index];
    if (index >= numberOfLocalMonitoringEvents)
        return tools;
    CodeMonitor* monitor = site.executable->pythonMonitor();
    if (!monitor)
        return tools;
    tools |= localToolsFor(state, *monitor, index);
    if (monitor->restartVersion != state.restartVersion || monitor->disabledTools[index].isEmpty())
        return tools;
    for (unsigned tool = 0; tool < numberOfMonitoringTools; ++tool) {
        if (monitor->timesAskedFor[tool][index] == state.timesAskedFor[tool][index])
            continue;
        monitor->timesAskedFor[tool][index] = state.timesAskedFor[tool][index];
        monitor->forgetDisabled(tool, index);
    }
    return tools & ~monitor->disabledTools[index].get(site.offset + 1);
}

static void disableAt(const Site& site, MonitoringEvent event, unsigned tool)
{
    MonitoringState& state = stateOf(site.globalObject);
    if (event == MonitoringEvent::CReturn || event == MonitoringEvent::CRaise)
        event = MonitoringEvent::Call;
    CodeMonitor& monitor = site.executable->ensurePythonMonitor();
    if (monitor.restartVersion != state.restartVersion) {
        for (auto& disabled : monitor.disabledTools)
            disabled.clear();
        monitor.restartVersion = state.restartVersion;
    }
    unsigned index = static_cast<unsigned>(event);
    monitor.timesAskedFor[tool][index] = state.timesAskedFor[tool][index];
    monitor.disabledTools[index].add(site.offset + 1, 0).iterator->value |= 1u << tool;
}

// ---- sys.settrace() and sys.setprofile()

enum class TraceKind : uint8_t { Call, Exception, Line, Return, CCall, CException, CReturn, Opcode };

static JSValue nameOf(VM& vm, TraceKind kind)
{
    static constexpr ASCIILiteral names[] = { "call"_s, "exception"_s, "line"_s, "return"_s, "c_call"_s, "c_exception"_s, "c_return"_s, "opcode"_s };
    return jsNontrivialString(vm, names[static_cast<unsigned>(kind)]);
}

static bool setTraceFunction(JSGlobalObject*, JSValue function);
static bool setProfileFunction(JSGlobalObject*, JSValue function);

// trace_trampoline(). False if it raised.
static bool callTraceFunction(JSGlobalObject* globalObject, PyFrame* frame, TraceKind kind, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // What was given to sys.settrace() is told that a frame has begun, and what it returns is told of what happens in the frame.
    JSValue callback = kind == TraceKind::Call ? stateOf(globalObject).traceFunction.get() : frame->trace();
    if (!callback || isNone(callback))
        return true;
    JSValue result = call(globalObject, callback, frame, nameOf(vm, kind), argument ? argument : jsUndefined());
    if (scope.exception()) [[unlikely]] {
        // That is the end of it.
        SuspendExceptionScope suspend(vm);
        setTraceFunction(globalObject, JSValue());
        frame->setTrace(vm, JSValue());
        return false;
    }
    if (!isNone(result))
        frame->setTrace(vm, result);
    return true;
}

// profile_trampoline()
static bool callProfileFunction(JSGlobalObject* globalObject, PyFrame* frame, TraceKind kind, JSValue argument)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    call(globalObject, stateOf(globalObject).profileFunction.get(), frame, nameOf(vm, kind), argument ? argument : jsUndefined());
    if (scope.exception()) [[unlikely]] {
        SuspendExceptionScope suspend(vm);
        setProfileFunction(globalObject, JSValue());
        return false;
    }
    return true;
}

enum class Outcome : uint8_t { Nothing, Raised, Disable };

static Outcome outcomeOf(bool succeeded) { return succeeded ? Outcome::Nothing : Outcome::Raised; }

static void setTracesOpcodes(JSGlobalObject* globalObject, FunctionExecutable* executable, bool enable)
{
    uint32_t events = localEventsOf(stateOf(globalObject), executable, traceTool);
    uint32_t wanted = enable ? events | bit(MonitoringEvent::Instruction) : events & ~bit(MonitoringEvent::Instruction);
    if (wanted != events)
        setLocalEvents(globalObject, executable, traceTool, wanted);
}

// _PyEval_SetOpcodeTrace()
void setTracesOpcodes(JSGlobalObject* globalObject, PyFrame* frame, bool enable)
{
    setTracesOpcodes(globalObject, executableOfCode(codeObjectFor(globalObject, frame->executable())), enable);
}

static Outcome tellTraceFunction(const Site& site, MonitoringEvent event, JSValue argument)
{
    JSGlobalObject* globalObject = site.globalObject;
    VM& vm = globalObject->vm();
    if (!stateOf(globalObject).traceFunction)
        return Outcome::Nothing;
    PyFrame* frame = PyFrame::forCallFrame(vm, site.callFrame);
    if (event == MonitoringEvent::Instruction) {
        if (!frame->tracesOpcodes()) {
            setTracesOpcodes(globalObject, site.executable, false);
            return Outcome::Nothing;
        }
        return outcomeOf(callTraceFunction(globalObject, frame, TraceKind::Opcode, JSValue()));
    }
    if (event == MonitoringEvent::Line)
        return frame->tracesLines() ? outcomeOf(callTraceFunction(globalObject, frame, TraceKind::Line, JSValue())) : Outcome::Nothing;
    if (frame->tracesOpcodes())
        setTracesOpcodes(globalObject, site.executable, true);
    switch (event) {
    case MonitoringEvent::PyStart:
    case MonitoringEvent::PyResume:
    case MonitoringEvent::PyThrow:
        return outcomeOf(callTraceFunction(globalObject, frame, TraceKind::Call, JSValue()));
    case MonitoringEvent::PyReturn:
    case MonitoringEvent::PyYield:
        return outcomeOf(callTraceFunction(globalObject, frame, TraceKind::Return, argument));
    case MonitoringEvent::PyUnwind:
        return outcomeOf(callTraceFunction(globalObject, frame, TraceKind::Return, JSValue()));
    case MonitoringEvent::Raise:
    case MonitoringEvent::StopIteration: {
        JSValue traceback = getAttributeIfPresent(globalObject, argument, vm.pythonNames().dunder_traceback);
        if (!traceback)
            traceback = jsUndefined();
        return outcomeOf(callTraceFunction(globalObject, frame, TraceKind::Exception, PyTuple::create(globalObject, { typeOf(globalObject, argument)->object(), argument, traceback })));
    }
    default:
        return Outcome::Nothing;
    }
}

static Outcome tellProfileFunction(const Site& site, MonitoringEvent event, JSValue argument, JSValue firstArgument)
{
    JSGlobalObject* globalObject = site.globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    if (!stateOf(globalObject).profileFunction)
        return Outcome::Nothing;
    PyFrame* frame = PyFrame::forCallFrame(vm, site.callFrame);
    switch (event) {
    case MonitoringEvent::PyStart:
    case MonitoringEvent::PyResume:
    case MonitoringEvent::PyThrow:
        RELEASE_AND_RETURN(scope, outcomeOf(callProfileFunction(globalObject, frame, TraceKind::Call, JSValue())));
    case MonitoringEvent::PyReturn:
    case MonitoringEvent::PyYield:
        RELEASE_AND_RETURN(scope, outcomeOf(callProfileFunction(globalObject, frame, TraceKind::Return, argument)));
    case MonitoringEvent::PyUnwind:
        RELEASE_AND_RETURN(scope, outcomeOf(callProfileFunction(globalObject, frame, TraceKind::Return, JSValue())));
    case MonitoringEvent::Call:
    case MonitoringEvent::CReturn:
    case MonitoringEvent::CRaise: {
        // Only of what is written in C++.
        TraceKind kind = event == MonitoringEvent::Call ? TraceKind::CCall : event == MonitoringEvent::CReturn ? TraceKind::CReturn : TraceKind::CException;
        PyType* type = typeOf(globalObject, argument);
        if (type == realm->typeBuiltinFunction())
            RELEASE_AND_RETURN(scope, outcomeOf(callProfileFunction(globalObject, frame, kind, argument)));
        if (type != realm->typeMethodDescriptor() || firstArgument == stateOf(globalObject).missing.get())
            return Outcome::Nothing;
        // As it used to be, it is the method of the object that is told of.
        JSValue method = bindDescriptor(globalObject, argument, firstArgument, typeOf(globalObject, firstArgument));
        RETURN_IF_EXCEPTION(scope, Outcome::Raised);
        RELEASE_AND_RETURN(scope, outcomeOf(callProfileFunction(globalObject, frame, kind, method)));
    }
    default:
        return Outcome::Nothing;
    }
}

// ---- Telling

// call_instrumentation_vector(). `second` is what comes after the code object: where in it, or for a line, which. False if what was told raised.
static bool fire(const Site& site, MonitoringEvent event, JSValue second, JSValue third = JSValue(), JSValue fourth = JSValue(), std::optional<uint8_t> givenTools = std::nullopt)
{
    JSGlobalObject* globalObject = site.globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MonitoringState& state = stateOf(globalObject);
    uint8_t tools = givenTools ? *givenTools : toolsFor(site, event);
    if (!tools)
        return true;
    MarkedArgumentBuffer arguments;
    arguments.append(site.code);
    arguments.append(second);
    if (third)
        arguments.append(third);
    if (fourth)
        arguments.append(fourth);

    // From the last to the first.
    for (unsigned tool = numberOfMonitoringTools; tool--;) {
        if (!(tools >> tool & 1))
            continue;
        Outcome outcome = Outcome::Nothing;
        ++state.callbackDepth;
        if (tool == traceTool)
            outcome = tellTraceFunction(site, event, third);
        else if (tool == profileTool)
            outcome = tellProfileFunction(site, event, third, fourth);
        else if (JSValue callback = state.callbacks[tool][static_cast<unsigned>(event)].get()) {
            JSValue result = call(globalObject, callback, arguments);
            outcome = scope.exception() ? Outcome::Raised : result == state.disable.get() ? Outcome::Disable : Outcome::Nothing;
        }
        --state.callbackDepth;
        switch (outcome) {
        case Outcome::Nothing:
            break;
        case Outcome::Raised:
            return false;
        case Outcome::Disable:
            if (static_cast<unsigned>(event) >= numberOfLocalMonitoringEvents) {
                // So that it does not go on for ever.
                state.callbacks[tool][static_cast<unsigned>(event)].clear();
                raiseValueError(globalObject, scope, concatenate("Cannot disable "_s, eventNames[static_cast<unsigned>(event)], " events. Callback removed."_s));
                return false;
            }
            disableAt(site, event, tool);
            break;
        }
    }
    return true;
}

static bool fireAtOffset(const Site& site, MonitoringEvent event, JSValue third = JSValue(), JSValue fourth = JSValue())
{
    return fire(site, event, jsNumber(site.offset), third, fourth);
}

// Every place that anything is told of counts as an instruction. What the engine runs is its own business.
static bool fireInstruction(const Site& site)
{
    return fireAtOffset(site, MonitoringEvent::Instruction);
}

// ---- Calls of what is not written in Python

// Whether calling it is only to begin a frame of Python code, which is told of for itself.
static bool beginsFrame(JSValue callable)
{
    if (auto* method = dynamicDowncast<PyBoundMethod>(callable))
        callable = method->function();
    auto* function = dynamicDowncast<JSFunction>(callable);
    return function && !function->isHostOrBuiltinFunction() && function->jsExecutable()->isPython();
}

// The call that the frame was last told to be making is over. If something is being thrown, `thrownFrom` is where in the frame that is from, which is the call if it came out of what was
// called. The call is what comes after op_py_call. False if what was told of it raised.
static bool finishPendingCall(JSGlobalObject* globalObject, CallFrame* callFrame, std::optional<BytecodeIndex> thrownFrom = std::nullopt)
{
    VM& vm = globalObject->vm();
    PyFrame* frame = PyFrame::forCallFrameIfExists(vm, callFrame);
    if (!frame || !frame->pendingCallable())
        return true;
    bool hasRaised = thrownFrom && thrownFrom->offset() == callFrame->codeBlock()->instructions().at(frame->pendingCallOffset()).next().offset();
    JSValue callable = frame->pendingCallable();
    JSValue argument = frame->pendingArgument();
    BytecodeIndex index(frame->pendingCallOffset());
    frame->clearPendingCall();
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return true;
    // The frame has gone on since, and is told of as if it had not.
    frame->setLineOverride(frame->lineAt(vm, index));
    bool succeeded = fire(*site, hasRaised ? MonitoringEvent::CRaise : MonitoringEvent::CReturn, jsNumber(site->offset), callable, argument, frame->pendingCallTools());
    frame->setLineOverride(-1);
    return succeeded;
}

// Where the jump that comes `skipped` instructions after this one goes, and where what follows it is.
struct JumpTargets {
    unsigned taken;
    unsigned notTaken;
};

template<typename Block>
static JumpTargets jumpTargetsAfter(Block* codeBlock, unsigned offset, unsigned skipped)
{
    auto instruction = codeBlock->instructions().at(offset);
    for (unsigned i = 0; i <= skipped; ++i)
        instruction = instruction.next();
    JumpTargets result { instruction.offset(), instruction.next().offset() };
    extractStoredJumpTargetsForInstruction(codeBlock, instruction, [&] (int32_t relative) {
        result.taken = instruction.offset() + relative;
    });
    return result;
}

// ---- What the code calls, when it finds that something is to be told

void raiseRecursionError(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded"_s);
}

void enterFrame(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, bool isResume)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!stateOf(globalObject).isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return;
    // Whatever line comes first is a new one.
    if (PyFrame* frame = isResume ? nullptr : PyFrame::forCallFrameIfExists(vm, callFrame))
        frame->setLastLine(-1);
    // JSGenerator::Argument counts `this`.
    auto argument = [&] (JSGenerator::Argument which) { return callFrame->uncheckedArgument(static_cast<unsigned>(which) - 1); };
    if (pythonInfoOfFrame(callFrame)->isGeneratorBody && argument(JSGenerator::Argument::ResumeMode).asInt32() == static_cast<int32_t>(JSGenerator::ResumeMode::ThrowMode))
        RELEASE_AND_RETURN(scope, void(fireAtOffset(*site, MonitoringEvent::PyThrow, argument(JSGenerator::Argument::Value))));
    RELEASE_AND_RETURN(scope, void(fireAtOffset(*site, isResume ? MonitoringEvent::PyResume : MonitoringEvent::PyStart)));
}

void frameIsAtLine(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, LineKind kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MonitoringState& state = stateOf(globalObject);
    if (!state.isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return;
    // A line is told of if what was run last was on some other.
    finishPendingCall(globalObject, callFrame);
    RETURN_IF_EXCEPTION(scope, void());
    PyFrame* frame = PyFrame::forCallFrame(vm, callFrame);
    if (kind == LineKind::OfHandledException) {
        if (JSValue exception = globalObject->pyRealm()->handledException())
            frame->setLastLine(lineOfTracebackFor(globalObject, exception, frame));
        return;
    }
    bool isAfterBackwardJump = kind == LineKind::AfterBackwardJump;
    int line = frame->line(vm);
    int previous = frame->lastLine();
    frame->setLastLine(line);
    if (line != previous) {
        fire(*site, MonitoringEvent::Line, jsNumber(line));
        RETURN_IF_EXCEPTION(scope, void());
    } else if (isAfterBackwardJump && (toolsFor(*site, MonitoringEvent::Line) >> traceTool & 1)) {
        // sys.settrace() is told each time round a loop, though it be all on one line.
        ++state.callbackDepth;
        tellTraceFunction(*site, MonitoringEvent::Line, JSValue());
        --state.callbackDepth;
        RETURN_IF_EXCEPTION(scope, void());
    }
    fireInstruction(*site);
    RETURN_IF_EXCEPTION(scope, void());
    if (isAfterBackwardJump)
        RELEASE_AND_RETURN(scope, void(fireAtOffset(*site, MonitoringEvent::Jump, jsNumber(jumpTargetsAfter(callFrame->codeBlock(), site->offset, 0).taken))));
}

void frameIsCalling(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, JSValue callable, JSValue argument, ToldArgument kind)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MonitoringState& state = stateOf(globalObject);
    if (!state.isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return;
    finishPendingCall(globalObject, callFrame);
    RETURN_IF_EXCEPTION(scope, void());
    fireInstruction(*site);
    RETURN_IF_EXCEPTION(scope, void());
    uint8_t tools = toolsFor(*site, MonitoringEvent::Call);
    if (!tools)
        return;
    switch (kind) {
    case ToldArgument::None:
        argument = state.missing.get();
        break;
    case ToldArgument::First:
        break;
    case ToldArgument::ListOfPositional: {
        JSArray* list = asList(argument);
        argument = list->length() ? listGet(globalObject, list, 0) : state.missing.get();
        RETURN_IF_EXCEPTION(scope, void());
        break;
    }
    }
    // A method of an object is told of as the function, called with the object.
    if (auto* method = dynamicDowncast<PyBoundMethod>(callable); method && typeOf(globalObject, method) == globalObject->pyRealm()->typeMethod()) {
        callable = method->function();
        argument = method->self();
    }
    fireAtOffset(*site, MonitoringEvent::Call, callable, argument);
    RETURN_IF_EXCEPTION(scope, void());
    if (!beginsFrame(callable))
        PyFrame::forCallFrame(vm, callFrame)->setPendingCall(vm, callable, argument, site->offset, tools);
}

static void tellOfBranch(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, bool isTaken, unsigned skipped)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!stateOf(globalObject).isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return;
    finishPendingCall(globalObject, callFrame);
    RETURN_IF_EXCEPTION(scope, void());
    fireInstruction(*site);
    RETURN_IF_EXCEPTION(scope, void());
    MonitoringEvent event = isTaken ? MonitoringEvent::BranchRight : MonitoringEvent::BranchLeft;
    if (!toolsFor(*site, event))
        return;
    JumpTargets targets = jumpTargetsAfter(callFrame->codeBlock(), site->offset, skipped);
    RELEASE_AND_RETURN(scope, void(fireAtOffset(*site, event, jsNumber(isTaken ? targets.taken : targets.notTaken))));
}

void frameIsBranching(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, bool isTaken)
{
    tellOfBranch(globalObject, callFrame, index, isTaken, 0);
}

// What comes after op_py_iter_next finds out whether there was anything, and then jumps if there was not.
void frameIsBranchingInLoop(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, bool isExhausted)
{
    tellOfBranch(globalObject, callFrame, index, isExhausted, 1);
}

void frameIsJumping(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!stateOf(globalObject).isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return;
    finishPendingCall(globalObject, callFrame);
    RETURN_IF_EXCEPTION(scope, void());
    fireInstruction(*site);
    RETURN_IF_EXCEPTION(scope, void());
    if (toolsFor(*site, MonitoringEvent::Jump))
        RELEASE_AND_RETURN(scope, void(fireAtOffset(*site, MonitoringEvent::Jump, jsNumber(jumpTargetsAfter(callFrame->codeBlock(), site->offset, 0).taken))));
}

// code.co_branches(): where each jump that depends on something is, where it goes on to if it is not made, and where it goes if it is
void forEachBranch(UnlinkedCodeBlock* codeBlock, const ScopedLambda<void(unsigned, unsigned, unsigned)>& function)
{
    for (const auto& instruction : codeBlock->instructions()) {
        OpcodeID opcode = instruction->opcodeID();
        if (opcode != op_py_branch && opcode != op_py_iter_next)
            continue;
        JumpTargets targets = jumpTargetsAfter(codeBlock, instruction.offset(), opcode == op_py_iter_next);
        function(instruction.offset(), targets.notTaken, targets.taken);
    }
}

void frameIsReturning(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, JSValue value)
{
    if (!stateOf(globalObject).isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return;
    if (!finishPendingCall(globalObject, callFrame) || !fireInstruction(*site))
        return;
    // Several `return` statements can share what does the returning, if there is a `finally` on the way out. What comes after one is on no line of its own, so the line is the last
    // that there was.
    PyFrame* frame = PyFrame::forCallFrameIfExists(globalObject->vm(), callFrame);
    if (frame)
        frame->setLineOverride(frame->lastLine());
    // The body of a class hands back the cell that __class__ is in, if anything in it wants that. Here it hands back the environment that has the variable.
    if (pythonInfoOfFrame(callFrame)->kind == CodeKind::Class)
        value = cellForClass(globalObject, value);
    fireAtOffset(*site, MonitoringEvent::PyReturn, value);
    if (frame)
        frame->setLineOverride(-1);
}

void frameIsYielding(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, JSValue value)
{
    if (!stateOf(globalObject).isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (site && finishPendingCall(globalObject, callFrame) && fireInstruction(*site))
        fireAtOffset(*site, MonitoringEvent::PyYield, value);
}

// As if StopIteration had been raised, which is how a generator used to say that it had done.
void generatorHasReturnedTo(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index, JSValue returned)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!stateOf(globalObject).isWatching)
        return;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site || !toolsFor(*site, MonitoringEvent::StopIteration))
        return;
    PyType* type = globalObject->pyRealm()->typeStopIteration();
    JSValue exception = isNone(returned) ? call(globalObject, type) : call(globalObject, type, returned);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, void(fireAtOffset(*site, MonitoringEvent::StopIteration, exception)));
}

void noteCaughtStopIteration(JSGlobalObject* globalObject, JSValue exception)
{
    stateOf(globalObject).caughtStopIteration.set(globalObject->vm(), globalObject->pyRealm(), exception);
}

void forgetCaughtStopIteration(JSGlobalObject* globalObject)
{
    stateOf(globalObject).caughtStopIteration.clear();
}

void tellOfCaughtStopIteration(JSGlobalObject* globalObject, CallFrame* callFrame, BytecodeIndex index)
{
    MonitoringState& state = stateOf(globalObject);
    JSValue exception = state.caughtStopIteration.get();
    state.caughtStopIteration.clear();
    if (!exception || !state.isWatching)
        return;
    if (auto site = siteOf(globalObject, callFrame, index))
        fireAtOffset(*site, MonitoringEvent::Raise, exception);
}

// ---- What the unwinder calls

Exception* tellOfException(VM& vm, CallFrame* callFrame, BytecodeIndex index, JSValue thrown, ExceptionProgress progress)
{
    JSGlobalObject* globalObject = callFrame->codeBlock()->globalObject();
    if (!stateOf(globalObject).isWatching || !isFrameToPython(callFrame, index))
        return nullptr;
    auto site = siteOf(globalObject, callFrame, index);
    if (!site)
        return nullptr;
    MonitoringEvent event = MonitoringEvent::Raise;
    switch (progress) {
    case ExceptionProgress::CameToFrame:
        event = MonitoringEvent::Raise;
        break;
    case ExceptionProgress::WasRaisedAgain:
        event = MonitoringEvent::Reraise;
        break;
    case ExceptionProgress::IsHandled:
        event = MonitoringEvent::ExceptionHandled;
        break;
    case ExceptionProgress::LeavesFrame:
        event = MonitoringEvent::PyUnwind;
        break;
    }
    PyFrame* existingFrame = PyFrame::forCallFrameIfExists(vm, callFrame);
    bool hasPendingCall = existingFrame && existingFrame->pendingCallable();
    if (!toolsFor(*site, event) && !hasPendingCall)
        return nullptr;

    // What is told is called from this frame, those that were beyond it being done with. What it raises is raised in place of what was being raised.
    SetForScope topCallFrame(vm.topCallFrame, callFrame);
    Exception* replacement = nullptr;
    {
        SuspendExceptionScope suspend(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        // What throws something again on its way out was not written, and is on no line of its own.
        PyFrame* frame = progress == ExceptionProgress::LeavesFrame ? PyFrame::forCallFrameIfExists(vm, callFrame) : nullptr;
        if (frame)
            frame->setLineOverride(frame->lastLine());
        // It came out of what the frame was calling.
        if (finishPendingCall(globalObject, callFrame, index))
            fireAtOffset(*site, event, thrown);
        if (frame)
            frame->setLineOverride(-1);
        if (Exception* raised = scope.exception()) {
            replacement = raised;
            scope.clearException();
        }
    }
    if (replacement) {
        {
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            scope.clearException();
        }
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwException(globalObject, scope, replacement);
    }
    return replacement;
}

// ---- sys.settrace(), sys.setprofile() and their like

static bool setTraceFunction(JSGlobalObject* globalObject, JSValue function)
{
    VM& vm = globalObject->vm();
    if (!audit(globalObject, "sys.settrace"_s))
        return false;
    MonitoringState& state = stateOf(globalObject);
    setOrClear(vm, globalObject->pyRealm(), state.traceFunction, function);
    using enum MonitoringEvent;
    setEvents(globalObject, traceTool, function ? bit(PyStart) | bit(PyResume) | bit(PyReturn) | bit(PyYield) | bit(Raise) | bit(Line) | bit(Jump) | bit(PyUnwind) | bit(PyThrow) | bit(StopIteration) : 0);
    return true;
}

static bool setProfileFunction(JSGlobalObject* globalObject, JSValue function)
{
    VM& vm = globalObject->vm();
    if (!audit(globalObject, "sys.setprofile"_s))
        return false;
    MonitoringState& state = stateOf(globalObject);
    setOrClear(vm, globalObject->pyRealm(), state.profileFunction, function);
    using enum MonitoringEvent;
    setEvents(globalObject, profileTool, function ? bit(PyStart) | bit(PyResume) | bit(PyReturn) | bit(PyYield) | bit(Call) | bit(PyUnwind) | bit(PyThrow) : 0);
    return true;
}

PYTHON_NATIVE(sysSetTrace)
{
    NATIVE_PROLOGUE();
    setTraceFunction(globalObject, isNone(args.at(0)) ? JSValue() : args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    // The frame that this was called from, if it wants to be told of each instruction.
    if (CallFrame* caller = stateOf(globalObject).traceFunction ? callerOf(callFrame) : nullptr) {
        PyFrame* frame = PyFrame::forCallFrameIfExists(vm, caller);
        if (frame && frame->tracesOpcodes())
            setTracesOpcodes(globalObject, executableOfCode(codeObjectFor(globalObject, frame->executable())), true);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(sysGetTrace)
{
    JSValue function = stateOf(globalObject).traceFunction.get();
    UNUSED_PARAM(callFrame);
    return JSValue::encode(function ? function : jsUndefined());
}

PYTHON_NATIVE(sysSetProfile)
{
    NATIVE_PROLOGUE();
    setProfileFunction(globalObject, isNone(args.at(0)) ? JSValue() : args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(sysGetProfile)
{
    JSValue function = stateOf(globalObject).profileFunction.get();
    UNUSED_PARAM(callFrame);
    return JSValue::encode(function ? function : jsUndefined());
}

// ---- sys.monitoring

// The number of a tool that a program can use. Nothing if it raised.
static std::optional<unsigned> toolArgument(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto tool = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*tool < 0 || *tool >= static_cast<int>(profileTool)) {
        raiseValueError(globalObject, scope, concatenate("invalid tool "_s, *tool, " (must be between 0 and 5)"_s));
        return std::nullopt;
    }
    return static_cast<unsigned>(*tool);
}

// check_tool()
static bool checkToolIsInUse(JSGlobalObject* globalObject, unsigned tool)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (stateOf(globalObject).toolNames[tool])
        return true;
    raiseValueError(globalObject, scope, concatenate("tool "_s, tool, " is not in use"_s));
    return false;
}

// _PyMonitoring_ClearToolId()
static void clearTool(JSGlobalObject* globalObject, unsigned tool)
{
    MonitoringState& state = stateOf(globalObject);
    for (auto& callback : state.callbacks[tool])
        callback.clear();
    setEvents(globalObject, tool, 0);
    // What it asked for of code objects is forgotten when they are next looked at.
    ++state.toolVersions[tool];
}

PYTHON_NATIVE(monitoringUseToolID)
{
    NATIVE_PROLOGUE();
    auto tool = toolArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(args.at(1)))
        return JSValue::encode(raiseValueError(globalObject, scope, "tool name must be a str"_s));
    MonitoringState& state = stateOf(globalObject);
    if (state.toolNames[*tool])
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("tool "_s, *tool, " is already in use"_s)));
    state.toolNames[*tool].set(vm, realm, args.at(1));
    RETURN_NONE();
}

// clear_tool_id(), and free_tool_id(), which lets go of the number too
PYTHON_NATIVE(monitoringClearToolID)
{
    bool frees = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    auto tool = toolArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    MonitoringState& state = stateOf(globalObject);
    if (state.toolNames[*tool])
        clearTool(globalObject, *tool);
    if (frees)
        state.toolNames[*tool].clear();
    RETURN_NONE();
}

PYTHON_NATIVE(monitoringGetTool)
{
    NATIVE_PROLOGUE();
    auto tool = toolArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue name = stateOf(globalObject).toolNames[*tool].get();
    return JSValue::encode(name ? name : jsUndefined());
}

PYTHON_NATIVE(monitoringRegisterCallback)
{
    NATIVE_PROLOGUE();
    auto tool = toolArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto events = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (std::popcount(static_cast<uint32_t>(*events)) != 1)
        return JSValue::encode(raiseValueError(globalObject, scope, "The callback can only be set for one event at a time"_s));
    unsigned event = std::countr_zero(static_cast<uint32_t>(*events));
    if (event >= numberOfMonitoringEvents)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid event "_s, *events)));
    JSValue function = args.at(2);
    if (!audit(globalObject, "sys.monitoring.register_callback"_s, function))
        return { };
    if (isNone(function))
        function = JSValue();

    MonitoringState& state = stateOf(globalObject);
    auto& callbacks = state.callbacks[*tool];
    unsigned left = static_cast<unsigned>(MonitoringEvent::BranchLeft);
    unsigned right = static_cast<unsigned>(MonitoringEvent::BranchRight);
    JSValue previous;
    if (event == static_cast<unsigned>(MonitoringEvent::Branch)) {
        previous = callbacks[left].get();
        setOrClear(vm, realm, callbacks[left], function);
        setOrClear(vm, realm, callbacks[right], function);
    } else {
        previous = callbacks[event].get();
        setOrClear(vm, realm, callbacks[event], function);
    }
    return JSValue::encode(previous ? previous : jsUndefined());
}

PYTHON_NATIVE(monitoringGetEvents)
{
    NATIVE_PROLOGUE();
    auto tool = toolArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(eventsOf(stateOf(globalObject), *tool)));
}

static constexpr uint32_t returnFromCEvents = bit(MonitoringEvent::CReturn) | bit(MonitoringEvent::CRaise);

// What is asked for, as it is kept. Nothing if it raised.
static std::optional<uint32_t> normalizeEvents(JSGlobalObject* globalObject, int events)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t result = events;
    // All three or none of the two.
    if ((result & returnFromCEvents) && (result & (returnFromCEvents | bit(MonitoringEvent::Call))) != (returnFromCEvents | bit(MonitoringEvent::Call))) {
        raiseValueError(globalObject, scope, "cannot set C_RETURN or C_RAISE events independently"_s);
        return std::nullopt;
    }
    result &= ~returnFromCEvents;
    if (result & bit(MonitoringEvent::Branch)) {
        result &= ~bit(MonitoringEvent::Branch);
        result |= bit(MonitoringEvent::BranchLeft) | bit(MonitoringEvent::BranchRight);
    }
    return result;
}

PYTHON_NATIVE(monitoringSetEvents)
{
    NATIVE_PROLOGUE();
    auto tool = toolArgument(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto given = toCInt(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    if (*given < 0 || *given >= 1 << numberOfMonitoringEvents)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid event set 0x"_s, hex(static_cast<uint32_t>(*given), Lowercase))));
    auto events = normalizeEvents(globalObject, *given);
    RETURN_IF_EXCEPTION(scope, { });
    if (!checkToolIsInUse(globalObject, *tool))
        return { };
    setEvents(globalObject, *tool, *events);
    RETURN_NONE();
}

PYTHON_NATIVE(monitoringGetLocalEvents)
{
    NATIVE_PROLOGUE();
    // The numbers are looked at before what they are numbers of.
    auto number = toCInt(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCode(globalObject, args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "code must be a code object"_s));
    auto tool = toolArgument(globalObject, jsNumber(*number));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(localEventsOf(stateOf(globalObject), executableOfCode(args.at(1)), *tool)));
}

PYTHON_NATIVE(monitoringSetLocalEvents)
{
    NATIVE_PROLOGUE();
    auto number = toCInt(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto given = toCInt(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isCode(globalObject, args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "code must be a code object"_s));
    auto tool = toolArgument(globalObject, jsNumber(*number));
    RETURN_IF_EXCEPTION(scope, { });
    auto events = normalizeEvents(globalObject, *given);
    RETURN_IF_EXCEPTION(scope, { });
    if (static_cast<int32_t>(*events) < 0 || *events >= 1u << numberOfLocalMonitoringEvents)
        return JSValue::encode(raiseValueError(globalObject, scope, concatenate("invalid local event set 0x"_s, hex(*events, Lowercase))));
    if (!checkToolIsInUse(globalObject, *tool))
        return { };
    setLocalEvents(globalObject, executableOfCode(args.at(1)), *tool, *events);
    RETURN_NONE();
}

PYTHON_NATIVE(monitoringRestartEvents)
{
    UNUSED_PARAM(callFrame);
    // What has been disabled goes by this, and is forgotten when it is next looked at.
    ++stateOf(globalObject).restartVersion;
    return JSValue::encode(jsUndefined());
}

PYTHON_NATIVE(monitoringAllEvents)
{
    NATIVE_PROLOGUE();
    PyDict* result = PyDict::create(globalObject);
    MonitoringState& state = stateOf(globalObject);
    for (unsigned event = 0; event < numberOfUngroupedMonitoringEvents; ++event) {
        if (!state.tools[event])
            continue;
        result->setString(globalObject, eventNames[event], jsNumber(state.tools[event]));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

void addMonitoring(JSGlobalObject* globalObject, JSObject* sys)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    MonitoringState& state = stateOf(globalObject);

    addFunction(globalObject, sys, "settrace"_s, sysSetTrace);
    addFunction(globalObject, sys, "gettrace"_s, sysGetTrace);
    addFunction(globalObject, sys, "setprofile"_s, sysSetProfile);
    addFunction(globalObject, sys, "getprofile"_s, sysGetProfile);
    // There is the one thread.
    addFunction(globalObject, sys, "_settraceallthreads"_s, sysSetTrace);
    addFunction(globalObject, sys, "_setprofileallthreads"_s, sysSetProfile);

    JSObject* module = newBuiltinModule(globalObject, "sys.monitoring"_s);
    auto set = [&] (JSObject* object, ASCIILiteral name, JSValue value) { putStoredAttribute(vm, object, Identifier::fromString(vm, name), value); };
    addFunction(globalObject, module, "use_tool_id"_s, monitoringUseToolID);
    addFunction(globalObject, module, "clear_tool_id"_s, monitoringClearToolID, pack(false));
    addFunction(globalObject, module, "free_tool_id"_s, monitoringClearToolID, pack(true));
    addFunction(globalObject, module, "get_tool"_s, monitoringGetTool);
    addFunction(globalObject, module, "register_callback"_s, monitoringRegisterCallback);
    addFunction(globalObject, module, "get_events"_s, monitoringGetEvents);
    addFunction(globalObject, module, "set_events"_s, monitoringSetEvents);
    addFunction(globalObject, module, "get_local_events"_s, monitoringGetLocalEvents);
    addFunction(globalObject, module, "set_local_events"_s, monitoringSetLocalEvents);
    addFunction(globalObject, module, "restart_events"_s, monitoringRestartEvents);
    addFunction(globalObject, module, "_all_events"_s, monitoringAllEvents);

    // Two objects that are like no others.
    state.disable.set(vm, realm, call(globalObject, realm->typeObject()));
    state.missing.set(vm, realm, call(globalObject, realm->typeObject()));
    set(module, "DISABLE"_s, state.disable.get());
    set(module, "MISSING"_s, state.missing.get());
    JSObject* events = newSimpleNamespace(globalObject);
    for (unsigned event = 0; event < numberOfMonitoringEvents; ++event)
        set(events, eventNames[event], jsNumber(1 << event));
    set(events, "NO_EVENTS"_s, jsNumber(0));
    set(module, "events"_s, events);
    set(module, "DEBUGGER_ID"_s, jsNumber(0));
    set(module, "COVERAGE_ID"_s, jsNumber(1));
    set(module, "PROFILER_ID"_s, jsNumber(2));
    set(module, "OPTIMIZER_ID"_s, jsNumber(5));
    set(sys, "monitoring"_s, module);
}

} } // namespace JSC::Python
