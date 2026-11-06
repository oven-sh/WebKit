/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyBytecode.h"
#include "PyRuntime.h"
#include "WeakInlines.h"
#include "UnlinkedCodeBlock.h"

namespace JSC {
namespace Python {

class CallArgs;
class CodeInfo;
class PyCode;
class PyFrame;

// sys.monitoring (PEP 669), and sys.settrace() and sys.setprofile(), which are two of its tools.
//
// Code that nobody wants to hear about pays nothing: no instruction is there for the sake of tracing, and none tests a flag.
//
// Most events happen at places in the code: a line begins, a call is made, the function returns. When a tool asks for such
// events, the code is changed where it stands. The opcode of the instruction at each such place is replaced by
// op_py_instrumented (for an opcode of page 2, the prefix op_page2 is); the operands stay. That instruction calls the runtime (slow_path_py_instrumented), which tells the tools
// and returns the opcode that was there, and the interpreter goes on to the code for that with the same PC. Python code is only
// ever interpreted, so there is no other copy of the instructions to keep up to date. Whatever else reads instructions
// (liveness analysis for the collector, the slow paths, co_lines()) sees the opcode that was there: Instruction::opcodeID().
//
// The instructions belong to the UnlinkedCodeBlock, and so does what is known about the places in them: a CodeMonitor, which
// hangs off the CodeInfo. It is made when the code is first to be changed. Which places there are is found by looking at
// the instructions, and at the marks the compiler left (PythonMark), which say where lines begin.
//
// There is nothing to check when a function is entered either. When the events that are wanted everywhere change, the code of the
// calls in progress is changed at once. All other code that has a CodeBlock is changed only where it can be entered
// (CodeMonitor::Entrance), which is one byte for a function; the places in it are looked for, and changed, if and when it is
// entered (CodeMonitor::wake()). Code that gets a CodeBlock later is treated the same way then (didCreateCodeBlock()).
//
// The other events have to do with exceptions, and are told of by the unwinder: addTracebackEntry(), exceptionStaysOrLeaves().

namespace Event {
enum : uint8_t {
    // At places in the code.
    PyStart, PyResume, PyReturn, PyYield, Call, Line, Instruction, Jump, BranchLeft, BranchRight, StopIteration,
    // Others.
    Raise, ExceptionHandled, PyUnwind, PyThrow, Reraise,
    // Asked for with Call.
    CReturn, CRaise,
    // The two of old in one.
    Branch,
};
static constexpr unsigned instrumentedCount = StopIteration + 1;
static constexpr unsigned ungroupedCount = Reraise + 1;
static constexpr unsigned count = Branch + 1;
}

static constexpr unsigned numberOfMonitoringTools = 8;
static constexpr unsigned numberOfFreeMonitoringTools = 6; // Those a program can use.
static constexpr unsigned profileTool = 6; // sys.setprofile()
static constexpr unsigned traceTool = 7; // sys.settrace()

// The line the instruction at the offset comes from. -1: none.
int lineOfMarks(std::span<const PythonMark>, unsigned offset);
// Each of the lines that begin at an instruction has one of its bytes, in order, and the last of them the rest. That is how co_lines() has
// it, and this is where it has the instruction itself: what the program is given for its offset.
unsigned visibleOffset(std::span<const PythonMark>, unsigned offset);
// What the program is given for that (f_lasti, tb_lasti, co_lines(), what tools are told): two for each byte. In CPython instructions
// are made of units of two bytes, and traceback.py and dis count on it. So each byte here has an item of co_positions() and a unit of
// co_code to itself.
constexpr unsigned shownOffset(unsigned visibleOffset) { return visibleOffset * 2; }
// The instruction is one of the program's own, not one of those that set up the frame.
bool isPastStart(std::span<const PythonMark>, unsigned offset);

class Monitoring;

class CodeMonitor {
    WTF_MAKE_NONCOPYABLE(CodeMonitor);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CodeMonitor);
public:
    CodeMonitor(Monitoring&, UnlinkedCodeBlock*, const CodeInfo&);
    ~CodeMonitor();

    // A line that begins at a place. There may be several, one after the other: of the earlier ones no instruction came.
    struct LineStart {
        int line;
        unsigned offset; // As co_lines() has it.
        uint8_t disabledTools { 0 };
        bool isShared { false }; // By several places: see Point::lineStartOfTarget.
        bool isOnLineOfDefinition { false }; // See findPoints().
    };
    static constexpr unsigned noLineStart = std::numeric_limits<unsigned>::max();

    // A place where something may happen that tools can ask to be told of.
    struct Point {
        static constexpr uint16_t whileCallsAreToldOf = 1 << 15; // It goes another way while any call in the code is told of: op_py_prepare_construct, super().m(). See Monitoring::handle().
        static constexpr uint16_t forLoop = 1 << 14; // op_py_for_iter
        // A branch after which, if it does not jump, CPython has something more to do on the same line, which here the instruction has
        // done: a `for` loop stores the item, `except E:` drops the exception. That is where it is said to go on.
        static constexpr uint16_t goesOnInItself = 1 << 13;
        // Nothing leads here but one way of a branch: it is here that the way is told of. See `leftIsToldOfAt`.
        static constexpr uint16_t isWhereBranchFallsThrough = 1 << 12;
        static constexpr uint16_t isWhereBranchJumps = 1 << 11;
        static_assert(Event::instrumentedCount <= 11);

        unsigned offset { 0 };
        uint16_t kinds { 0 }; // 1 << event, for the events that are told of here.
        uint16_t opcode { 0 }; // The opcode of the instruction.
        unsigned next { 0 }; // The offset of the instruction that follows.
        unsigned target { 0 }; // A jump, or a branch: where it goes.
        unsigned shownTarget { 0 }; // A branch, a jump: where it is said to go. A call: what comes after it.
        unsigned source { 0 }; // asyncForEndsMark: where the branch is said to be. isWhereBranchFallsThrough, isWhereBranchJumps: where it is.
        // A branch. Which way it goes is known once it has gone, so to tell of it here is to take the branch in C++, every time, for as long as one of
        // the ways is still wanted: and the way out of a loop is wanted until the loop ends. Where nothing else leads to where a way
        // goes, it is told of there, and the branch is left as it is. (CPython has an instruction behind every branch for that.) 0: it is told of here.
        unsigned leftIsToldOfAt { 0 };
        unsigned rightIsToldOfAt { 0 };
        bool isPatched { false };
        // isWhereBranchJumps: jumps lead here as well, as a `break` does to where the loop ends. They say so as they go (leadsWhereBranchJumps), for as
        // long as the way of the branch is wanted: they are taken once where the branch is taken at every turn.
        bool isAlsoJumpedTo { false };
        bool leadsWhereBranchJumps { false };
        // If nothing else is wanted here:
        enum class ShortWay : uint8_t {
            None,
            // A branch that is told of here when it jumps, because more than jumps lead to where it jumps (`if` without `else` and with something behind it,
            // `a and b`). It may never jump: the runtime takes the branch and that is all.
            TakeBranch,
            SayItIsAJump, // leadsWhereBranchJumps
            ArriveByJump, // isAlsoJumpedTo, and that is how the frame got here.
        };
        ShortWay shortWay { ShortWay::None };
        // Whether the first of the lines is news depends on where the frame comes from: see findPoints().
        bool firstLineIsInDoubt { false };
        bool firstLineFollowsCatch { false }; // It is news unless the exception was raised on that line.
        // A call after which another statement begins. In CPython something is still to be done with the result, on the line of the call: it
        // is stored or dropped. Here the call has done that. Whoever is told of instructions is told of one more when the call is
        // over: that is where breakpoint() stops.
        bool isLastOfStatement { false };
        unsigned firstLineStart { 0 };
        unsigned lineStartCount { 0 };
        // A jump, or a branch: the line that begins where it goes, if it has something to say about that: see findPoints().
        unsigned lineStartOfTarget { noLineStart };
        bool targetLineIsNotNews { false }; // That is what it says. Otherwise it tells of the line.
        std::array<uint8_t, Event::instrumentedCount> disabledTools { }; // By event; not for Line.
    };

    // Where the code can be entered: its first instruction and, in the body of a generator, wherever it goes on after it has yielded.
    struct Entrance {
        unsigned offset;
        uint16_t opcode;
    };

    UnlinkedCodeBlock* code() const { return m_code.get(); }
    const uint8_t* instructions() const { return m_instructions; }
    int lineBeforeStart() const { return m_lineBeforeStart; } // What f_lineno is before anything has happened.
    ALWAYS_INLINE Point* pointAt(unsigned offset)
    {
        if (offset >= m_indexOfPointAt.size()) [[unlikely]]
            return searchForPointAt(offset);
        unsigned index = m_indexOfPointAt[offset];
        return index ? &m_points[index - 1] : nullptr;
    }
    // Changes whenever pointers to the places may have become invalid.
    unsigned generation() const { return m_generation; }
    uint8_t toolsToTellOf(const LineStart&) const;
    std::span<LineStart> lineStartsOf(Point& point) { return m_lineStarts.mutableSpan().subspan(point.firstLineStart, point.lineStartCount); }
    // The line the instruction comes from. -1: none.
    int lineAt(unsigned offset) const;
    // Several lines may begin at an instruction: `while True:` and the first of its body.
    int firstLineAt(unsigned offset) const;
    unsigned whereHandlersBegin();

    uint32_t localEvents(unsigned tool) const;
    void setLocalEvents(unsigned tool, uint32_t events);
    uint8_t localTools(unsigned event) const { return m_localTools[event]; }
    void removeLocalTool(unsigned event, unsigned tool);
    // The tools that want to be told of the event in this code.
    uint8_t tools(unsigned event) const;
    // Whether an event at a place in the code has ever had more than one while the code was what the tools wanted it to be. From then
    // on CPython keeps the tools of each instruction of the code object, and until then only those of the code object. A program can
    // see which: a tool that answers DISABLE to a CALL is told how that call ends only until then, and no more when the other tool has gone.
    bool hadEventWithSeveralTools() const { return m_hadEventWithSeveralTools; }

    // What the tools want has changed. If they want anything here, the code says so when it is next entered.
    void sleep();
    // The code is running, or about to: from now on each place in it is what the tools want it to be.
    void wake();
    // For a change that is about this code alone: a call of it may be in progress.
    void sleepAndWake()
    {
        sleep();
        wake();
    }
    bool isAwake() const { return m_isAwake; }
    // Makes the instructions what they are to be: the entrances of code that is asleep, the places in code that is awake.
    void update();
    void update(Point&);
    // The same for a branch, with the places where its ways are told of.
    void updateBranch(Point&);
    // After a tool has been added to LineStart::disabledTools.
    void didDisableLine(unsigned offset, const LineStart&);
    // What DISABLE switched off is on again.
    void restart();
    // Makes the instructions what the compiler left, until the next update().
    void putBackInstructions();

private:
    friend class Monitoring;

    friend OpcodeID JSC::originalOpcodeOfInstrumentedInstruction(const void*);

    Point* searchForPointAt(unsigned offset);
    void findEntrances();
    void findPoints(bool everyInstruction);
    void findPointsWithoutTellingTheCollector(bool everyInstruction);
    void noteLineTools();
    void noteLocalTools();
    void forget();
    bool isWanted() const;
    void setEntrancesAreReplaced(bool);
    uint8_t toolsWantedAt(Point&);
    uint8_t* opcodeAddress(unsigned offset) const;
    std::optional<uint16_t> opcodeThatMayBeReplacedAt(unsigned offset) const;

    Monitoring* m_monitoring; // Null once it is gone: when the VM ends, things go in no particular order.
    // The executable that owns this (by way of the CodeInfo) keeps its code for as long as it lives: see UnlinkedFunctionExecutable::returnCodeToCache().
    // They die together, and are swept in no particular order: null once the code is dead, and then the instructions may be gone.
    Weak<UnlinkedCodeBlock> m_code;
    const uint8_t* m_instructions; // Of the code.
    unsigned m_instructionsSize;
    unsigned m_index; // In Monitoring::m_monitors.
    int m_lineBeforeStart;
    // These three change with monitoredCodeLock held: the threads of the collector look things up in them.
    Vector<Entrance, 1> m_entrances;
    Vector<Point> m_points; // By offset.
    // For each byte of the instructions: the index of the place there, plus one, or 0. Empty: there are too many.
    Vector<uint16_t> m_indexOfPointAt;
    unsigned m_generation { 0 };
    unsigned m_whereHandlersBegin { 0 }; // 0: not asked for yet.
    Vector<LineStart> m_lineStarts;
    size_t m_sizeToldToCollector { 0 };
    std::array<uint8_t, Event::ungroupedCount> m_localTools { };
    bool m_isAwake { false };
    bool m_entrancesAreReplaced { false };
    bool m_hasLocalTools { false };
    bool m_localToolsWantPlaces { false };
    bool m_localToolsWantExceptions { false };
    bool m_hadEventWithSeveralTools { false };
    bool m_hasSomethingDisabled { false };
    bool m_hasToForget { false }; // See forget().
    bool m_hasPoints { false };
    bool m_hasEveryInstruction { false };
    // There is a place where a line is in doubt. Then the frame has to know which line it was last on (PyFrame::lastLine), so every
    // place where a line begins says so for as long as any tool wants to be told of any of them.
    bool m_needsLastLine { false };
    uint8_t m_lineToolsStillOn { 0 };
};

// There is one for each Runtime.
class Monitoring {
    WTF_MAKE_NONCOPYABLE(Monitoring);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Monitoring);
public:
    explicit Monitoring(Runtime&);
    ~Monitoring();

    Runtime& runtime() { return m_runtime; }

    // ---- What is wanted ----

    uint32_t events(unsigned tool) const { return m_events[tool]; }
    void setEvents(unsigned tool, uint32_t events);
    void setLocalEvents(CodeMonitor&, unsigned tool, uint32_t events);
    uint8_t globalTools(unsigned event) const { return m_globalTools[event]; }
    void clearTool(unsigned tool);
    void restartEvents();
    // Null: the code cannot be traced.
    CodeMonitor* monitorFor(JSGlobalObject*, PyCode*);
    CodeMonitor* monitorFor(CodeBlock*);
    void didCreateCodeBlock(CodeBlock*);

    JSValue callback(unsigned tool, unsigned event); // Empty: none.
    JSValue setCallback(unsigned tool, unsigned event, JSValue); // What it was.
    JSValue toolName(unsigned tool); // Empty: it is not in use.
    void setToolName(unsigned tool, JSValue);
    JSValue disable();
    JSValue missing();

    // While there is one, all instructions, or those of one code object, are as the compiler left them: for whatever reads the bytes.
    class OriginalInstructions {
        WTF_MAKE_NONCOPYABLE(OriginalInstructions);
    public:
        explicit OriginalInstructions(Monitoring&);
        OriginalInstructions(Monitoring&, PyCode*);
        ~OriginalInstructions();
    private:
        Monitoring& m_monitoring;
        CodeMonitor* m_only { nullptr };
        bool m_isForAll;
    };

    // ---- sys.settrace(), sys.setprofile(): for the current thread ----

    void setTraceFunction(JSValue); // Empty: none.
    void setProfileFunction(JSValue);
    // A thread has been given, or has lost, a function while it was not running, or has ended.
    void tracingThreadCountChanged(int traceDelta, int profileDelta);

    void toolIsInterrupted();

    // ---- What happens ----

    // What f_lineno and f_lasti of a frame are while something is told of an event in it. -1: those of the instruction it is at.
    struct Shown {
        int line { -1 };
        int offset { -1 };
    };

    struct Outcome {
        const JSInstruction* pc; // Null: an exception.
        unsigned opcode; // Run this at pc. numOpcodeIDs: whatever is there.
    };
    Outcome handle(JSGlobalObject*, CallFrame*, const JSInstruction*);
    // True: it is worth calling these two.
    bool wantsExceptions() const { return m_wantsExceptions; }
    void noteWhereItIsRaised(CallFrame* callFrame, unsigned offset)
    {
        m_raisedIn = callFrame;
        m_raisedAt = offset;
    }
    bool isRaisedIn(CallFrame* callFrame) const { return m_raisedIn == callFrame; }
    // With no exception pending. False: one is now.
    bool exceptionEvent(JSGlobalObject*, CallFrame*, unsigned offset, unsigned event, JSValue exception);
    // RuntimeFunction::ReraiseOutOfHandler. What is to be raised: the exception, or what a tool raised instead. (Nothing: the thread is to end.)
    JSValue exceptionLeavesHandler(JSGlobalObject*, CallFrame*, JSValue exception);
    // throw() or close() of a generator that delegates. What was thrown has gone to the delegate, by the op_py_send at the offset. In
    // CPython the generator is resumed only when the delegate has raised or returned, with the exception or the
    // StopIteration, in a handler on the line of the `yield from`, which raises all but a StopIteration again.
    bool lineOfSendIsToldOfAgain(JSGlobalObject*, CallFrame*, unsigned offset);
    void delegateReturnedWhenThrownInto(JSGlobalObject*, CallFrame*, unsigned offset, JSValue result);

    // frame.f_lineno = line. With an exception if it cannot be.
    void jump(JSGlobalObject*, PyFrame*, int line);
    // frame.f_trace_opcodes = true, or false
    void setTracesOpcodes(JSGlobalObject*, PyFrame*, bool);
    // sys.monitoring._places(code)
    JSValue places(JSGlobalObject*, PyCode*);
    // code.co_branches(), as a list
    JSArray* branches(JSGlobalObject*, PyCode*);
    // When the runtime is set up.
    void createState();

    // What a jump has left for the instruction it leads to, which is the next that the frame runs. But a jump back is a safe point: code of the
    // program may run in between, a finalizer or another thread, and whatever is told of that takes it that these are of the instruction before.
    struct PlaceJumpedTo {
        CallFrame* callFrame { nullptr };
        unsigned offset { 0 };
    };
    struct NotesOfJump {
        PlaceJumpedTo jumpGoesOn;
        PlaceJumpedTo comesFromSameLine;
        PlaceJumpedTo arrivesByJump;
    };
    NotesOfJump takeNotesOfJump() { return { std::exchange(m_jumpGoesOn, { }), std::exchange(m_comesFromSameLine, { }), std::exchange(m_arrivesByJump, { }) }; }
    void putBack(const NotesOfJump& notes)
    {
        m_jumpGoesOn = notes.jumpGoesOn;
        m_comesFromSameLine = notes.comesFromSameLine;
        m_arrivesByJump = notes.arrivesByJump;
    }

private:
    friend class CodeMonitor;

    enum class What : uint8_t { Call, Line, Return, Exception, Opcode, CCall, CReturn, CException };

    Outcome handle(JSGlobalObject*, CallFrame*, const JSInstruction*, CodeMonitor&, CodeMonitor::Point*);
    Outcome takeBranch(JSGlobalObject*, CallFrame*, const JSInstruction*, CodeMonitor&, const CodeMonitor::Point&);
    JSArray* state() const { return asArray(m_runtime.slot(Runtime::Slot::Monitoring)); }
    JSValue nameOf(What what) { return m_namesOfWhat[static_cast<unsigned>(what)]; }
    void updateAll();
    void noteWhatIsWanted();
    void findAllCode();

    void wakeCodeOfCallsInProgress();
    template<typename Functor> void forEachMonitor(const Functor&);

    // 0: go on. 1: DISABLE. -1: an exception.
    int callTool(JSGlobalObject*, CallFrame*, unsigned tool, unsigned event, Shown, std::span<const JSValue> arguments);
    // Tells the tools (bits) of an event at a place. False: an exception.
    bool fire(JSGlobalObject*, CallFrame*, CodeMonitor&, unsigned offset, unsigned event, uint8_t tools, JSValue third = { }, JSValue fourth = { }, std::optional<unsigned> shownOffsetOfPlace = { });
    bool fireLine(JSGlobalObject*, CallFrame*, CodeMonitor&, const CodeMonitor::Point&, bool comesFromSameLine);
    bool fireLineStart(JSGlobalObject*, CallFrame*, CodeMonitor&, unsigned offset, unsigned lineStart);
    bool legacyEvent(JSGlobalObject*, CallFrame*, CodeMonitor&, unsigned tool, unsigned event, unsigned offset, JSValue third, JSValue fourth, bool& disable);
    bool callTraceFunction(JSGlobalObject*, PyFrame*, unsigned event, Shown, What, JSValue argument);
    bool callProfileFunction(JSGlobalObject*, PyFrame*, unsigned event, Shown, What, JSValue argument);
    JSValue callWhileTracing(JSGlobalObject*, CallFrame*, unsigned event, Shown, JSValue function, const CallArgs&);
    bool traceLine(JSGlobalObject*, PyFrame*, Shown);

    Runtime& m_runtime;
    std::array<uint32_t, numberOfMonitoringTools> m_events { };
    std::array<uint8_t, Event::ungroupedCount> m_globalTools { };
    bool m_globalToolsWantPlaces { false };
    bool m_wantsExceptions { false };
    CallFrame* m_startIsToldOfIn { nullptr };
    CallFrame* m_raisedIn { nullptr };
    unsigned m_raisedAt { 0 };
    // All code that has a CodeBlock has a monitor: so it is from the first time that something is wanted everywhere.
    bool m_knowsAllCode { false };
    unsigned m_monitorsWithLocalTools { 0 };
    unsigned m_monitorsWhoseLocalToolsWantExceptions { 0 };
    // The frame is about to run the op_jmp at the offset, having jumped to it: see handle().
    PlaceJumpedTo m_jumpGoesOn;
    // The frame is about to run the instruction at the offset, having jumped to it from the line that begins there.
    PlaceJumpedTo m_comesFromSameLine;
    // It got there by a jump: see Point::leadsWhereBranchJumps.
    PlaceJumpedTo m_arrivesByJump;
    std::array<JSValue, 8> m_namesOfWhat { }; // What trace and profile functions are told has happened. state() keeps them.
    unsigned m_tracingThreads { 0 };
    unsigned m_profilingThreads { 0 };

    Vector<CodeMonitor*> m_monitors;
    CodeMonitor* m_codeLeftAlone { nullptr }; // See callWhileTracing().
};

} // namespace Python
} // namespace JSC
