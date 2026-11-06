/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyMonitoring.h"

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "LLIntExceptions.h"
#include "PreciseJumpTargetsInlines.h"
#include "PyAbstract.h"
#include "PyBytecode.h"
#include "PyCodeInfo.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNamespace.h"
#include "PyNumbers.h"
#include "PySlowPaths.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include <map>
#include <wtf/BitVector.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

using namespace Python;

// ---- The opcodes that have been replaced ----

// The code that has a monitor, of all VMs, by the address at which its instructions end. The monitor knows what it has replaced.
static Lock monitoredCodeLock;
static std::map<const uint8_t*, CodeMonitor*>& monitoredCode() WTF_REQUIRES_LOCK(monitoredCodeLock)
{
    static NeverDestroyed<std::map<const uint8_t*, CodeMonitor*>> map;
    return map;
}

static ALWAYS_INLINE const uint8_t* bytesOf(const UnlinkedCodeBlock* code) { return static_cast<const uint8_t*>(code->instructions().rawPointer()); }

static ALWAYS_INLINE const uint8_t* addressOfOpcode(const void* instruction)
{
    auto* bytes = static_cast<const uint8_t*>(instruction);
    return *bytes == op_wide16 || *bytes == op_wide32 ? bytes + 1 : bytes;
}

// The collector asks too (liveness of registers), from its own threads.
OpcodeID originalOpcodeOfInstrumentedInstruction(const void* instruction)
{
    auto* address = static_cast<const uint8_t*>(instruction);
    Locker locker { monitoredCodeLock };
    auto it = monitoredCode().upper_bound(address);
    if (it != monitoredCode().end() && it->second->m_instructions <= address) {
        if (auto opcode = it->second->opcodeThatMayBeReplacedAt(address - it->second->m_instructions))
            return static_cast<OpcodeID>(*opcode);
    }
    // It has been put back since the caller looked.
    auto opcode = static_cast<OpcodeID>(*addressOfOpcode(instruction));
    RELEASE_ASSERT(opcode != op_py_instrumented);
    if (opcode == op_page2)
        return static_cast<OpcodeID>(firstPage2OpcodeID + addressOfOpcode(instruction)[1]);
    return opcode;
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_instrumented)
{
    CodeBlock* codeBlock = callFrame->codeBlock();
    JSGlobalObject* globalObject = codeBlock->globalObject();
    VM& vm = codeBlock->vm();
    SlowPathFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto outcome = runtime(globalObject).monitoring().handle(globalObject, callFrame, pc);
    if (failed(scope, outcome.pc)) [[unlikely]]
        return encodeResult(LLInt::returnToThrow(vm), LLInt::exceptionSignal());
    return encodeResult(outcome.pc, std::bit_cast<void*>(static_cast<uintptr_t>(outcome.opcode)));
}

namespace Python {

CodeInfo::CodeInfo() = default;
CodeInfo::~CodeInfo() = default;

static constexpr unsigned goOn = numOpcodeIDs; // See Monitoring::Outcome.

// What stands in the code where op_py_instrumented is put: of an instruction of page 2, the prefix.
static ALWAYS_INLINE uint8_t byteOfOpcode(uint16_t opcode) { return static_cast<uint8_t>(opcode >= firstPage2OpcodeID ? op_page2 : opcode); }

static constexpr uint16_t bit(unsigned event) { return static_cast<uint16_t>(1 << event); }

// frameObjectIfAny(), and PyFrame::ofForTracing(), for where every call counts.
static ALWAYS_INLINE PyFrame* frameObjectIfThereIsOne(CallFrame* callFrame)
{
    static const VirtualRegister reg = frameObjectRegister();
    JSValue held = callFrame->uncheckedR(reg).jsValue();
    return held.isCell() ? uncheckedDowncast<PyFrame>(held.asCell()) : nullptr;
}

static ALWAYS_INLINE PyFrame* frameObjectOf(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    if (PyFrame* frame = frameObjectIfThereIsOne(callFrame)) [[likely]]
        return frame;
    return PyFrame::ofForTracing(globalObject, callFrame);
}

// ---- Lines ----

int lineOfMarks(std::span<const PythonMark> marks, unsigned offset)
{
    auto it = std::ranges::upper_bound(marks, offset, { }, &PythonMark::offset);
    while (it != marks.begin()) {
        --it;
        if (it->kind() == PythonMark::Line)
            return lineOfMarkValue(it->value());
        if ((it->kind() == PythonMark::NoLine && !it->value()) || it->kind() == PythonMark::Start)
            return -1;
    }
    return -1;
}

unsigned visibleOffset(std::span<const PythonMark> marks, unsigned offset)
{
    auto [first, last] = std::ranges::equal_range(marks, offset, { }, &PythonMark::offset);
    unsigned linesBefore = 0;
    unsigned result = offset;
    for (auto it = first; it != last; ++it) {
        if (it->kind() == PythonMark::Line)
            result = offset + linesBefore++;
        else if (it->kind() == PythonMark::NoLine && !it->value())
            result = offset + linesBefore;
    }
    return result;
}

bool isPastStart(std::span<const PythonMark> marks, unsigned offset)
{
    for (auto& mark : marks) {
        if (mark.kind() == PythonMark::Start)
            return mark.offset <= offset;
    }
    return false;
}

// ---- CodeMonitor ----

CodeMonitor::CodeMonitor(Monitoring& monitoring, UnlinkedCodeBlock* code, const CodeInfo& info)
    : m_monitoring(&monitoring)
    , m_code(code)
    , m_instructions(bytesOf(code))
    , m_instructionsSize(code->instructions().sizeInBytes())
    , m_index(monitoring.m_monitors.size())
    , m_lineBeforeStart(info.name.string() == "<module>"_s ? 0 : info.firstLineNumber)
{
    monitoring.m_monitors.append(this);
    findEntrances();
    Locker locker { monitoredCodeLock };
    // When a VM ends, things go in no particular order: the instructions of some code of its may be gone, the monitor still be there,
    // and these be where those were.
    auto& map = monitoredCode();
    for (auto it = map.upper_bound(m_instructions); it != map.end() && it->second->m_instructions < m_instructions + m_instructionsSize;)
        it = map.erase(it);
    map.emplace(m_instructions + m_instructionsSize, this);
}

CodeMonitor::~CodeMonitor()
{
    if (m_monitoring) {
        auto& monitors = m_monitoring->m_monitors;
        monitors.last()->m_index = m_index;
        monitors[m_index] = monitors.last();
        monitors.removeLast();
        m_monitoring->m_monitorsWhoseLocalToolsWantExceptions -= m_localToolsWantExceptions;
        m_monitoring->m_monitorsWithLocalTools -= m_hasLocalTools;
    }
    // The instructions go away with the code, as they are.
    Locker locker { monitoredCodeLock };
    auto it = monitoredCode().find(m_instructions + m_instructionsSize);
    if (it != monitoredCode().end() && it->second == this)
        monitoredCode().erase(it);
}

CodeMonitor::Point* CodeMonitor::searchForPointAt(unsigned offset)
{
    auto it = std::ranges::lower_bound(m_points, offset, { }, &Point::offset);
    return it != m_points.end() && it->offset == offset ? &*it : nullptr;
}

std::optional<uint16_t> CodeMonitor::opcodeThatMayBeReplacedAt(unsigned offset) const
{
    for (auto& entrance : m_entrances) {
        if (entrance.offset == offset)
            return entrance.opcode;
    }
    auto it = std::ranges::lower_bound(m_points, offset, { }, &Point::offset);
    if (it != m_points.end() && it->offset == offset)
        return it->opcode;
    return std::nullopt;
}

int CodeMonitor::lineAt(unsigned offset) const
{
    return lineOfMarks(m_code->pythonMarks(), offset);
}

int CodeMonitor::firstLineAt(unsigned offset) const
{
    auto marks = m_code->pythonMarks();
    for (auto it = std::ranges::lower_bound(marks, offset, { }, &PythonMark::offset); it != marks.end() && it->offset == offset; ++it) {
        if (it->kind() == PythonMark::Line)
            return lineOfMarkValue(it->value());
    }
    return lineOfMarks(marks, offset);
}

uint32_t CodeMonitor::localEvents(unsigned tool) const
{
    uint32_t events = 0;
    for (unsigned event = 0; event < Event::ungroupedCount; ++event) {
        if (m_localTools[event] >> tool & 1)
            events |= 1 << event;
    }
    return events;
}

// Whether any tool wants to be told of something for which instructions have to be replaced,
template<typename Tools>
static bool toolsWantPlaces(const Tools& tools)
{
    uint8_t wanted = tools[Event::PyThrow] | tools[Event::Raise];
    for (unsigned event = 0; event < Event::instrumentedCount; ++event)
        wanted |= tools[event];
    return wanted;
}

// or of something the unwinder tells of.
template<typename Tools>
static bool toolsWantExceptions(const Tools& tools)
{
    return tools[Event::Raise] | tools[Event::Reraise] | tools[Event::ExceptionHandled] | tools[Event::PyUnwind];
}

void CodeMonitor::noteLocalTools()
{
    m_localToolsWantPlaces = toolsWantPlaces(m_localTools);
    bool hasLocalTools = std::ranges::any_of(m_localTools, [](uint8_t tools) { return tools; });
    m_monitoring->m_monitorsWithLocalTools += hasLocalTools - m_hasLocalTools;
    m_hasLocalTools = hasLocalTools;
    bool wantExceptions = toolsWantExceptions(m_localTools);
    m_monitoring->m_monitorsWhoseLocalToolsWantExceptions += wantExceptions - m_localToolsWantExceptions;
    m_localToolsWantExceptions = wantExceptions;
}

void CodeMonitor::setLocalEvents(unsigned tool, uint32_t events)
{
    for (unsigned event = 0; event < Event::ungroupedCount; ++event) {
        if (events >> event & 1)
            m_localTools[event] |= 1 << tool;
        else
            m_localTools[event] &= ~(1 << tool);
    }
    noteLocalTools();
}

void CodeMonitor::removeLocalTool(unsigned event, unsigned tool)
{
    m_localTools[event] &= ~(1 << tool);
    noteLocalTools();
}

uint8_t CodeMonitor::tools(unsigned event) const
{
    return m_monitoring->m_globalTools[event] | m_localTools[event];
}

bool CodeMonitor::isWanted() const
{
    return m_monitoring->m_globalToolsWantPlaces || m_localToolsWantPlaces;
}

uint8_t* CodeMonitor::opcodeAddress(unsigned offset) const
{
    return const_cast<uint8_t*>(addressOfOpcode(m_instructions + offset));
}

void CodeMonitor::findEntrances()
{
    const JSInstructionStream& instructions = m_code->instructions();
    m_entrances.append({ 0, static_cast<uint16_t>(instructions.at(0)->opcodeID()) });
    if (m_code->parseMode() != SourceParseMode::GeneratorBodyMode)
        return;
    bool hasYielded = false;
    for (const auto& instruction : instructions) {
        OpcodeID opcode = instruction->opcodeID();
        if (std::exchange(hasYielded, opcode == op_py_generator_suspend))
            m_entrances.append({ instruction.offset(), static_cast<uint16_t>(opcode) });
    }
}

void CodeMonitor::setEntrancesAreReplaced(bool replace)
{
    if (m_entrancesAreReplaced == replace)
        return;
    m_entrancesAreReplaced = replace;
    for (auto& entrance : m_entrances)
        *opcodeAddress(entrance.offset) = replace ? static_cast<uint8_t>(op_py_instrumented) : byteOfOpcode(entrance.opcode);
}

static bool isJump(OpcodeID opcode)
{
    return opcode == op_jmp || opcode == op_py_jmp_back;
}

static unsigned targetOfJump(UnlinkedCodeBlock* code, unsigned offset)
{
    auto instruction = code->instructions().at(offset);
    return offset + (instruction->opcodeID() == op_jmp ? jumpTargetForInstruction<OpJmp>(code, instruction) : jumpTargetForInstruction<OpPyJmpBack>(code, instruction));
}

static bool fallsThrough(OpcodeID opcode)
{
    switch (opcode) {
    case op_jmp:
    case op_py_jmp_back:
    case op_ret:
    case op_py_ret:
    case op_py_generator_suspend:
    case op_py_generator_return:
    case op_throw:
    case op_unreachable:
    case op_switch_imm:
    case op_switch_char:
    case op_switch_string:
        return false;
    default:
        return true;
    }
}

static bool isLoadOfSuperAttribute(RuntimeFunction function)
{
    return function == RuntimeFunction::LoadSuperAttr || function == RuntimeFunction::LoadZeroSuperAttr || function == RuntimeFunction::LoadSuperMethod || function == RuntimeFunction::LoadZeroSuperMethod;
}

static bool isPythonBranch(OpcodeID opcode)
{
    switch (opcode) {
    case op_py_jtrue:
    case op_py_jfalse:
    case op_py_jeq:
    case op_py_jne:
    case op_py_jlt:
    case op_py_jle:
    case op_py_jgt:
    case op_py_jge:
    case op_py_jneq:
    case op_py_jnne:
    case op_py_jnlt:
    case op_py_jnle:
    case op_py_jngt:
    case op_py_jnge:
    case op_py_for_iter:
    case op_jundefined_or_null: // `is None`, unless the compiler says that it is a test of its own (hiddenJumpMark).
    case op_jnundefined_or_null:
        return true;
    default:
        return false;
    }
}

// A line begins at an instruction that has a mark for it, and at one that is jumped to. Being there is an event if the instruction
// run before it in the frame was of another line. Whether it was is nearly always plain from the code.
void CodeMonitor::putBackInstructions()
{
    setEntrancesAreReplaced(false);
    for (auto& point : m_points) {
        if (!point.isPatched)
            continue;
        *opcodeAddress(point.offset) = byteOfOpcode(point.opcode);
        point.isPatched = false;
    }
}

void CodeMonitor::findPoints(bool everyInstruction)
{
    findPointsWithoutTellingTheCollector(everyInstruction);
    // These tables are several times the size of the code. The collector does not see them, and is to know what a program uses up that makes
    // code, runs it under a tool and drops it. They only grow, are told of once however often they are filled, and go with the code.
    // (Not with the lock held, which its threads take. It is not to collect here: code is woken where it is entered, and the frame does
    // not say yet where it is.)
    size_t size = m_points.capacity() * sizeof(Point) + m_lineStarts.capacity() * sizeof(LineStart) + m_indexOfPointAt.capacity() * sizeof(uint16_t);
    if (size > m_sizeToldToCollector && m_monitoring) {
        VM& vm = m_monitoring->runtime().vm();
        DeferGCForAWhile deferGC(vm);
        vm.heap.deprecatedReportExtraMemory(size - m_sizeToldToCollector);
        m_sizeToldToCollector = size;
    }
}

void CodeMonitor::findPointsWithoutTellingTheCollector(bool everyInstruction)
{
    putBackInstructions();
    Locker locker { monitoredCodeLock };
    m_points.clear();
    m_lineStarts.clear();
    m_indexOfPointAt.fill(0, m_instructionsSize);
    ++m_generation;
    m_needsLastLine = false;
    m_hasPoints = true;
    m_hasEveryInstruction = everyInstruction;

    UnlinkedCodeBlock* code = m_code.get();
    const JSInstructionStream& instructions = code->instructions();
    auto marks = code->pythonMarks();
    bool isGeneratorBody = code->parseMode() == SourceParseMode::GeneratorBodyMode;

    unsigned start = std::numeric_limits<unsigned>::max();
    for (auto& mark : marks) {
        if (mark.kind() == PythonMark::Start) {
            start = mark.offset;
            break;
        }
    }
    if (start == std::numeric_limits<unsigned>::max())
        return;

    struct Item {
        unsigned offset;
        OpcodeID opcode;
        // The compiler said what line it is of, or that it is of none. What it said nothing of goes with whatever comes before.
        bool isMarked { false };
        int exitLine { -1 }; // If so: the line of the instruction.
        unsigned firstLine { 0 }; // In `lines`: those that begin here.
        unsigned lineCount { 0 };
        // Not -1: the first of them, if whether it is news depends on where the frame comes from. The others always are.
        int entryLine { -1 };
        Vector<int, 2> linesBefore { }; // Those the frame may be on when it gets here.
        // By op_jmp, or a branch that is taken: what can tell where it goes.
        bool isReachedFromOtherLineOnlyByJumps { true };
        bool isReachedFromSameLineOnlyByJumps { true };
        bool isNewsUnlessToldOtherwise { false };
        bool isJumpedTo { false }; // By anything but a `for` loop that ends.
        unsigned jumpsHere { 0 }; // How many jumps and branches lead here,
        unsigned branchesHere { 0 }; // how many of them are branches,
        unsigned hiddenJumpsHere { 0 }; // how many are jumps that tools are not told of,
        unsigned firstJumpHereFrom { std::numeric_limits<unsigned>::max() }; // where the jumps are,
        unsigned lastJumpHereFrom { 0 };
        bool isFallenInto { false }; // and whether what comes before it leads here too.
        // Not 0: only a way of the branch there does.
        unsigned branchThatLeadsHere { 0 };
        bool branchJumpsHere { false };
        bool isResumedAt { false };
        bool isHiddenJump { false };
        bool isJumpAsLaidOut { false };
        bool isJumpForwardsAsLaidOut { false };
        bool hasLineToItself { false }; // A jump: it is a statement, or jumpHasLineToItselfMark.
        unsigned silentLinesFirst { 0 }; // How many lines that are not told of come before the first that begins here.
        uint8_t asyncForMark { 0 };
        bool beginsStatement { false };
        unsigned lastJumpForwardFrom { 0 }; // The offset of the last of the jumps and branches before it that go to it.
        bool firstLineFollowsCatch { false };
        bool comesAfterNoLine { false };
        bool isDeadHandler { false };
        bool firstLineFollowsSilentLine { false };
        bool fallsThrough { true };
        unsigned sharedLineStart { noLineStart };

        // -2: it depends.
        int lineAfter() const { return isMarked ? exitLine : linesBefore.isEmpty() ? -1 : linesBefore.size() == 1 ? linesBefore[0] : -2; }
        bool mayBeOn(int line) const { return lineAfter() == -2 ? linesBefore.contains(line) : lineAfter() == line; }
    };
    struct Line {
        int line;
        unsigned offset; // As co_lines() has it.
    };
    Vector<Item> items;
    Vector<Line> lines;
    UncheckedKeyHashMap<unsigned, unsigned, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> indexOfOffset;
    {
        unsigned nextMark = 0;
        for (const auto& instruction : instructions) {
            Item item { instruction.offset(), instruction->opcodeID() };
            item.firstLine = lines.size();
            item.fallsThrough = fallsThrough(item.opcode);
            if (item.opcode == op_py_runtime) {
                auto function = static_cast<RuntimeFunction>(instruction->as<OpPyRuntime>().m_function);
                item.fallsThrough = function != RuntimeFunction::Raise && function != RuntimeFunction::Reraise && function != RuntimeFunction::ReraiseOutOfHandler && function != RuntimeFunction::ReraiseUnseen && function != RuntimeFunction::ReraiseGroup && function != RuntimeFunction::ThrowIntoGenerator;
            }
            unsigned lineMarkCount = 0;
            bool isOfSilentLine = false;
            for (; nextMark < marks.size() && marks[nextMark].offset <= item.offset; ++nextMark) {
                auto& mark = marks[nextMark];
                if (mark.kind() == PythonMark::NoLine && mark.value()) {
                    if (mark.value() == hiddenJumpMark)
                        item.isHiddenJump = true;
                    else if (mark.value() == jumpAsLaidOutMark)
                        item.isJumpAsLaidOut = true;
                    else if (mark.value() == jumpForwardsAsLaidOutMark)
                        item.isJumpForwardsAsLaidOut = true;
                    else if (mark.value() == jumpHasLineToItselfMark)
                        item.hasLineToItself = true;
                    else if (mark.value() == afterNoLineMark)
                        item.comesAfterNoLine = true;
                    else if (mark.value() == deadHandlerMark)
                        item.isDeadHandler = true;
                    else if (mark.value() != handlerOfLineMark)
                        item.asyncForMark = mark.value();
                    continue;
                }
                if (mark.kind() != PythonMark::Line && mark.kind() != PythonMark::NoLine && mark.kind() != PythonMark::Start)
                    continue;
                int before = item.exitLine;
                bool followsSilentLine = isOfSilentLine;
                item.isMarked = true;
                item.exitLine = mark.kind() == PythonMark::Line ? lineOfMarkValue(mark.value()) : -1;
                isOfSilentLine = mark.kind() == PythonMark::Line && isSilentLine(mark.value());
                if (mark.kind() != PythonMark::Line)
                    continue;
                item.beginsStatement |= isStatementStart(mark.value());
                bool isFirst = !lineMarkCount;
                unsigned offset = item.offset + lineMarkCount++;
                if (isOfSilentLine && lines.size() == item.firstLine)
                    ++item.silentLinesFirst;
                if (isOfSilentLine || (!isFirst && item.exitLine == before))
                    continue;
                if (isFirst)
                    item.entryLine = item.exitLine;
                else if (lines.size() == item.firstLine)
                    item.firstLineFollowsSilentLine = followsSilentLine;
                lines.append({ item.exitLine, offset });
            }
            item.lineCount = lines.size() - item.firstLine;
            if (!item.lineCount)
                item.silentLinesFirst = 0;
            item.hasLineToItself |= item.lineCount && item.beginsStatement;
            // BytecodeGenerator::emitOutOfLinePyCatchHandler()
            if (item.opcode == op_jmp && !items.isEmpty() && items.last().opcode == op_py_catch)
                item.isHiddenJump = true;
            indexOfOffset.add(item.offset, items.size());
            items.append(WTF::move(item));
        }
    }

    auto reach = [&](unsigned index, const Item& from, bool byJump) {
        if (index >= items.size())
            return;
        Item& item = items[index];
        auto add = [&](int line) {
            if (!item.linesBefore.contains(line))
                item.linesBefore.append(line);
        };
        int line = from.lineAfter();
        if (line != -2)
            add(line);
        else {
            for (int possible : from.linesBefore)
                add(possible);
        }
        bool maybeFromSameLine = from.mayBeOn(item.entryLine);
        bool maybeFromOtherLine = line != item.entryLine && (line != -2 || from.linesBefore.size() > 1 || !maybeFromSameLine);
        if (maybeFromOtherLine && (!byJump || maybeFromSameLine))
            item.isReachedFromOtherLineOnlyByJumps = false;
        if (maybeFromSameLine && (!byJump || maybeFromOtherLine))
            item.isReachedFromSameLineOnlyByJumps = false;
    };
    // Nothing comes from what cannot be reached (`if 0:`).
    BitVector isReachable(items.size());
    // Without an exception. CPython's compiler puts the rest behind all of this.
    BitVector isReachableWithoutException;
    {
        Vector<unsigned, 16> toVisit;
        auto visit = [&](unsigned index) {
            if (index < items.size() && !isReachable.set(index))
                toVisit.append(index);
        };
        // A generator goes on behind where it was suspended. That it gets there from the top says nothing about the place.
        BitVector isResumedAt(items.size());
        auto visitAllFromThere = [&] {
            while (!toVisit.isEmpty()) {
                unsigned i = toVisit.takeLast();
                if (items[i].fallsThrough || (i + 1 < items.size() && isResumedAt.get(i + 1)))
                    visit(i + 1);
                bool isResumption = isGeneratorBody && items[i].opcode == op_switch_imm && items[i].offset < start;
                extractStoredJumpTargetsForInstruction(code, instructions.at(items[i].offset), [&](int32_t relative) {
                    auto it = indexOfOffset.find(items[i].offset + relative);
                    if (it == indexOfOffset.end())
                        return;
                    if (isResumption && items[it->value].offset > start)
                        isResumedAt.set(it->value);
                    else
                        visit(it->value);
                });
            }
        };
        visit(0);
        visitAllFromThere();
        isReachableWithoutException = isReachable;
        for (unsigned i = 0; i + 1 < items.size(); ++i) {
            if (items[i].opcode != op_py_catch)
                continue;
            // BytecodeGenerator::emitOutOfLinePyCatchHandler(): a jump to where the handler goes on comes next.
            if (auto it = indexOfOffset.find(targetOfJump(code, items[i + 1].offset)); it == indexOfOffset.end() || !items[it->value].isDeadHandler)
                visit(i);
        }
        visitAllFromThere();
    }
    // A jump over handlers alone leads to what comes next there, and is dropped. One out of a handler to what is none goes backwards.
    // Of one that has a line to itself, something that does nothing stays for the line.
    {
        BitVector nothingIsLeft(items.size());
        for (unsigned i = items.size(); i--;) {
            if (!items[i].isJumpAsLaidOut && !items[i].isJumpForwardsAsLaidOut)
                continue;
            auto it = indexOfOffset.find(targetOfJump(code, items[i].offset));
            if (it == indexOfOffset.end())
                continue;
            bool isInHandler = !isReachableWithoutException.get(i);
            if (isInHandler == isReachableWithoutException.get(it->value)) {
                items[i].isHiddenJump = items[i].isJumpForwardsAsLaidOut;
                continue;
            }
            items[i].isHiddenJump = true;
            for (unsigned between = i + 1; between < it->value && items[i].isHiddenJump; ++between)
                items[i].isHiddenJump = !isReachable.get(between) || isReachableWithoutException.get(between) == isInHandler || nothingIsLeft.get(between);
            if (items[i].isHiddenJump && !items[i].hasLineToItself)
                nothingIsLeft.set(i);
        }
    }
    for (unsigned i = 0; i < items.size(); ++i) {
        if (!isReachable.get(i))
            continue;
        if (items[i].offset == start) {
            items[i].linesBefore.append(-1);
            items[i].isReachedFromOtherLineOnlyByJumps = false;
        }
        // It goes on with the line it was on.
        if (items[i].isResumedAt) {
            items[i].linesBefore.append(lineAt(items[i].offset));
            items[i].isReachedFromSameLineOnlyByJumps = false;
        }
        const Item& item = items[i];
        if (item.fallsThrough) {
            reach(i + 1, item, false);
            if (i + 1 < items.size())
                items[i + 1].isFallenInto = true;
        }
        bool isResumption = isGeneratorBody && item.opcode == op_switch_imm && item.offset < start;
        extractStoredJumpTargetsForInstruction(code, instructions.at(item.offset), [&](int32_t relative) {
            auto it = indexOfOffset.find(item.offset + relative);
            if (it == indexOfOffset.end())
                return;
            if (isResumption) {
                // (Before the start: where throw() has one begin that has not started.)
                if (items[it->value].offset != start)
                    items[it->value].isResumedAt = true;
                return;
            }
            ++items[it->value].jumpsHere;
            if (isJump(item.opcode)) {
                items[it->value].hiddenJumpsHere += item.isHiddenJump;
                items[it->value].firstJumpHereFrom = std::min(items[it->value].firstJumpHereFrom, item.offset);
                items[it->value].lastJumpHereFrom = std::max(items[it->value].lastJumpHereFrom, item.offset);
            } else
                ++items[it->value].branchesHere;
            items[it->value].isJumpedTo |= item.opcode != op_py_for_iter;
            if (relative > 0 && !item.isHiddenJump)
                items[it->value].lastJumpForwardFrom = std::max(items[it->value].lastJumpForwardFrom, item.offset);
            reach(it->value, item, item.offset >= start && (isJump(item.opcode) || (isPythonBranch(item.opcode) && !item.isHiddenJump)));
        });
    }
    // In CPython a line begins where the instruction before, as they are laid out, is of another line, or where a jump goes. Where
    // a `for` loop goes when it ends does not count as that. So `x = [i` is not told of again after the lines of `for i in y]`.
    // What drops the iterator there does not count as the instruction before either: if a `return` in the loop, laid out before its end,
    // has left a `with` statement, the line of the `with` is not told of when the loop ends and the statement is left.
    for (unsigned i = 1; i < items.size(); ++i) {
        Item& item = items[i];
        if (item.isJumpedTo || item.offset <= start || (item.entryLine < 0 && !item.firstLineFollowsSilentLine))
            continue;
        // (Not what cannot be reached: CPython's compiler drops that. Nor a handler in front of what is none, nor the other way round.)
        bool isInHandler = !isReachableWithoutException.get(i);
        unsigned before = i - 1;
        while (before && (!isReachable.get(before) || isReachableWithoutException.get(before) == isInHandler))
            --before;
        int lineBefore = lineAt(items[before].offset);
        if (item.entryLine >= 0 && lineBefore == item.entryLine) {
            item.linesBefore.clear();
            item.linesBefore.append(item.entryLine);
        } else if (item.firstLineFollowsSilentLine && lineBefore == lines[item.firstLine].line) {
            ++item.firstLine;
            --item.lineCount;
        }
    }
    for (unsigned i = 0; i + 1 < items.size(); ++i) {
        if (items[i].opcode != op_py_catch)
            continue;
        unsigned next = i + 1;
        if (items[next].isHiddenJump) {
            auto it = indexOfOffset.find(targetOfJump(code, items[next].offset));
            if (it == indexOfOffset.end())
                continue;
            next = it->value;
        }
        while (next < items.size() && !items[next].lineCount && items[next].fallsThrough)
            ++next;
        if (next < items.size() && items[next].entryLine >= 0 && !items[next].comesAfterNoLine)
            items[next].firstLineFollowsCatch = true;
    }
    // What drops the iterator when a `for` loop ends is of the line of the loop, and is not where the loop, or a `break`, is said to go on.
    auto behindSilentLines = [&](unsigned offset) {
        auto it = indexOfOffset.find(offset);
        return it == indexOfOffset.end() ? offset : offset + items[it->value].silentLinesFirst;
    };
    // CPython's compiler has a branch to a jump go where the jump goes. (One that has a line to itself, a `break`, leaves something there for the line.)
    auto whereItEndsUp = [&](unsigned offset) {
        for (unsigned count = 0; count < 8; ++count) {
            auto it = indexOfOffset.find(offset);
            if (it == indexOfOffset.end() || !isJump(items[it->value].opcode) || !items[it->value].isHiddenJump || items[it->value].hasLineToItself)
                break;
            offset = targetOfJump(code, offset);
        }
        return behindSilentLines(offset);
    };
    // So it is with a jump to a jump, of whatever kind: they are told of as one jump.
    auto whereJumpEndsUp = [&](unsigned offset) {
        for (unsigned count = 0; count < 8; ++count) {
            auto it = indexOfOffset.find(offset);
            if (it == indexOfOffset.end() || !isJump(items[it->value].opcode) || items[it->value].hasLineToItself)
                break;
            offset = targetOfJump(code, offset);
        }
        return behindSilentLines(offset);
    };
    // Code written on the line of its `def`, `lambda` or `class` is on that line from the start. As in CPython, a trace function is told
    // of the line all the same, and so are the tools if one of them is told that the code starts.
    unsigned indexOfLineOfDefinition = std::numeric_limits<unsigned>::max();
    if (auto it = indexOfOffset.find(start); it != indexOfOffset.end()) {
        unsigned index = it->value;
        while (index < items.size() && !items[index].lineCount && items[index].fallsThrough && (index == it->value || !items[index].isMarked))
            ++index;
        if (index < items.size() && items[index].entryLine == m_lineBeforeStart)
            indexOfLineOfDefinition = index;
    }
    // Being at an instruction is news of the first of its lines if the frame was on another. For some, it may or may not have been.
    // Such is the head of a `for` loop: it is on the line of what comes before it, and is jumped to from the end of the body. If
    // all that comes from other lines is jumps, the jumps tell of the line. If all that comes from the same line is jumps, they tell
    // that it is not news (`return (a and` over `b)`). What is left is in doubt: the frame has to remember the line it was last on.
    auto isReachedFromSameLine = [](const Item& item) { return item.linesBefore.contains(item.entryLine); };
    auto isReachedFromOtherLine = [](const Item& item) { return item.linesBefore.isEmpty() || item.linesBefore.size() > 1 || item.linesBefore[0] != item.entryLine; };
    for (auto& item : items) {
        if (item.offset < start || item.entryLine < 0 || !isReachedFromSameLine(item) || !isReachedFromOtherLine(item))
            continue;
        if (!item.isReachedFromOtherLineOnlyByJumps) {
            item.isNewsUnlessToldOtherwise = item.isReachedFromSameLineOnlyByJumps;
            continue;
        }
        item.sharedLineStart = m_lineStarts.size();
        m_lineStarts.append({ item.entryLine, item.offset, 0, true });
    }
    // What a jump has to say about the line that begins where it goes.
    Vector<std::pair<unsigned, unsigned>> jumpsFromSameLine; // The offset of the jump, that of its target.
    auto noteTarget = [&](Point& point, const Item& item, unsigned targetOffset) {
        auto it = indexOfOffset.find(targetOffset);
        if (it == indexOfOffset.end())
            return;
        const Item& target = items[it->value];
        if (!item.mayBeOn(target.entryLine))
            point.lineStartOfTarget = target.sharedLineStart;
        else if (target.isNewsUnlessToldOtherwise)
            jumpsFromSameLine.append({ item.offset, targetOffset });
    };

    bool isBeingResumed = false;
    bool isExceptionMatch = false;
    Vector<unsigned, 4> asyncForLoops;
    unsigned startOfStatement = 0;
    int lineOfStatement = -1;
    for (auto& item : items) {
        // (Not where a `yield` that cannot be reached would go on.)
        isBeingResumed = isReachable.get(&item - items.begin()) && (isBeingResumed || item.isResumedAt);
        if (item.beginsStatement) {
            startOfStatement = item.offset;
            lineOfStatement = item.exitLine;
        }
        if ((item.offset < start && !isBeingResumed) || !isReachable.get(&item - items.begin()))
            continue;

        Point point;
        point.offset = item.offset;
        point.opcode = static_cast<uint16_t>(item.opcode);
        point.next = item.offset + instructions.at(item.offset)->size();
        if (item.offset == start)
            point.kinds |= bit(Event::PyStart);
        if (item.branchThatLeadsHere) {
            point.kinds |= item.branchJumpsHere ? Point::isWhereBranchJumps : Point::isWhereBranchFallsThrough;
            point.source = item.branchThatLeadsHere;
            point.isAlsoJumpedTo = item.jumpsHere > item.branchJumpsHere;
        }
        if (everyInstruction && !item.isHiddenJump && item.offset >= start)
            point.kinds |= bit(Event::Instruction);

        // The registers are put back first.
        bool isResumption = isBeingResumed && item.opcode != op_py_generator_registers;
        if (isResumption) {
            point.kinds |= bit(Event::PyResume);
            isBeingResumed = false;
        }

        bool previousIsExceptionMatch = std::exchange(isExceptionMatch, item.opcode == op_py_compare && static_cast<CompareOperator>(instructions.at(item.offset)->as<OpPyCompare>().m_operation) == CompareOperator::ExceptionMatch);
        switch (item.opcode) {
        case op_py_generator_suspend:
            point.kinds |= bit(Event::PyYield);
            break;
        case op_py_generator_return:
        case op_py_ret:
            point.kinds |= bit(Event::PyReturn);
            break;
        case op_call:
        case op_py_call_spread:
            point.kinds |= bit(Event::Call);
            point.shownTarget = point.next;
            while (true) {
                auto it = indexOfOffset.find(point.shownTarget);
                if (it == indexOfOffset.end())
                    break;
                const Item& next = items[it->value];
                if (next.isHiddenJump && next.offset > item.offset) {
                    point.shownTarget = targetOfJump(code, next.offset);
                    continue;
                }
                // What ends a `with` or a handler is on the line where that begins.
                bool endsBlock = next.lineCount && lines[next.firstLine].line < lineOfStatement;
                // `a and f()`: what drops the value is where the two ways meet, and is of no line in CPython.
                point.isLastOfStatement = (next.beginsStatement || endsBlock) && next.lastJumpForwardFrom < startOfStatement;
                break;
            }
            break;
        case op_py_prepare_construct:
            point.kinds |= Point::construct;
            break;
        case op_py_runtime:
            if (isLoadOfSuperAttribute(static_cast<RuntimeFunction>(instructions.at(item.offset)->as<OpPyRuntime>().m_function)))
                point.kinds |= bit(Event::Call);
            break;
        case op_py_send:
            point.kinds |= bit(Event::StopIteration);
            break;
        case op_jmp:
        case op_py_jmp_back:
            // (Not one that only jumps lead to which are told of: it is part of them.)
            if (!item.isHiddenJump && (item.hasLineToItself || item.isFallenInto || item.branchesHere || item.hiddenJumpsHere || !item.jumpsHere || item.isResumedAt))
                point.kinds |= bit(Event::Jump);
            point.target = targetOfJump(code, item.offset);
            point.shownTarget = whereJumpEndsUp(point.target);
            noteTarget(point, item, point.target);
            if (auto it = indexOfOffset.find(point.target); it != indexOfOffset.end())
                point.leadsWhereBranchJumps = items[it->value].branchJumpsHere;
            break;
        default:
            // Whether it was resumed to go on, or to raise something, is not a branch of the program's.
            if (isPythonBranch(item.opcode) && !item.isHiddenJump && !isResumption) {
                point.kinds |= bit(Event::BranchLeft) | bit(Event::BranchRight);
                if (item.opcode == op_py_for_iter)
                    point.kinds |= Point::forLoop | Point::goesOnInItself;
                else if (previousIsExceptionMatch)
                    point.kinds |= Point::goesOnInItself;
                extractStoredJumpTargetsForInstruction(code, instructions.at(item.offset), [&](int32_t relative) {
                    point.target = item.offset + relative;
                    point.shownTarget = whereItEndsUp(point.target);
                    noteTarget(point, item, point.target);
                });
                // See Point::leftIsToldOfAt. (What is behind this in the code has not been looked at yet.)
                auto canTellOfWay = [&](const Item& other) {
                    return !other.isResumedAt && other.opcode != op_py_catch && !other.asyncForMark && !other.branchThatLeadsHere;
                };
                unsigned index = &item - items.begin();
                if (Item* next = index + 1 < items.size() ? &items[index + 1] : nullptr; next && !(point.kinds & Point::goesOnInItself) && !next->jumpsHere && canTellOfWay(*next)) {
                    next->branchThatLeadsHere = item.offset;
                    point.leftIsToldOfAt = next->offset;
                }
                if (auto it = indexOfOffset.find(point.target); it != indexOfOffset.end() && it->value > index) {
                    Item& target = items[it->value];
                    // `break`: jumps may lead there as well, if they are between here and there. See Point::leadsWhereBranchJumps.
                    bool jumpsAreBetween = target.jumpsHere == 1 || (target.firstJumpHereFrom > item.offset && target.lastJumpHereFrom < target.offset);
                    if (target.branchesHere == 1 && jumpsAreBetween && !target.isFallenInto && canTellOfWay(target)) {
                        target.branchThatLeadsHere = item.offset;
                        target.branchJumpsHere = true;
                        point.rightIsToldOfAt = target.offset;
                    }
                }
            }
            break;
        }
        if (item.asyncForMark == asyncForGoesOnMark) {
            point.kinds |= bit(Event::BranchLeft);
            point.shownTarget = point.next;
            asyncForLoops.append(item.offset);
        } else if (item.asyncForMark == asyncForEndsMark && !asyncForLoops.isEmpty()) {
            point.kinds |= bit(Event::BranchRight);
            point.source = asyncForLoops.takeLast();
            // After RuntimeFunction::EndAsyncFor.
            unsigned index = &item - items.begin();
            while (index + 1 < items.size() && items[index].opcode != op_py_runtime)
                ++index;
            point.shownTarget = whereItEndsUp(items[index].offset + instructions.at(items[index].offset)->size());
        }

        // While the registers are being put back nothing is told of.
        if (!isBeingResumed && !item.isResumedAt && item.opcode != op_py_catch) {
            point.firstLineStart = m_lineStarts.size();
            if (item.entryLine >= 0 && item.sharedLineStart == noLineStart && isReachedFromOtherLine(item)) {
                m_lineStarts.append({ item.entryLine, lines[item.firstLine].offset, 0, item.isNewsUnlessToldOtherwise });
                if (static_cast<unsigned>(&item - items.begin()) == indexOfLineOfDefinition)
                    m_lineStarts.last().isOnLineOfDefinition = true;
                point.firstLineFollowsCatch = item.firstLineFollowsCatch;
                if (isReachedFromSameLine(item) && !item.isNewsUnlessToldOtherwise) {
                    point.firstLineIsInDoubt = true;
                    m_needsLastLine = true;
                }
            }
            for (unsigned i = item.entryLine >= 0; i < item.lineCount; ++i)
                m_lineStarts.append({ lines[item.firstLine + i].line, lines[item.firstLine + i].offset });
            point.lineStartCount = m_lineStarts.size() - point.firstLineStart;
            if (point.lineStartCount)
                point.kinds |= bit(Event::Line);
        }

        if (point.kinds || point.leadsWhereBranchJumps || point.lineStartOfTarget != noLineStart || (!jumpsFromSameLine.isEmpty() && jumpsFromSameLine.last().first == item.offset)) {
            m_points.append(point);
            if (m_points.size() > std::numeric_limits<uint16_t>::max())
                m_indexOfPointAt.clear();
            else if (!m_indexOfPointAt.isEmpty())
                m_indexOfPointAt[point.offset] = m_points.size();
        }
    }
    for (auto [jump, target] : jumpsFromSameLine) {
        Point* targetPoint = pointAt(target);
        if (!targetPoint || !targetPoint->lineStartCount)
            continue;
        Point* point = pointAt(jump);
        point->lineStartOfTarget = targetPoint->firstLineStart;
        point->targetLineIsNotNews = true;
    }
}

// Of code that is written on the line of its `def` or `lambda`, only a trace function is told that the line begins, unless somebody is told
// that the code starts (fireLineStart()). So nobody else ever gets to say DISABLE there.
uint8_t CodeMonitor::toolsToTellOf(const LineStart& start) const
{
    uint8_t wanted = tools(Event::Line) & ~start.disabledTools;
    if (start.isOnLineOfDefinition && !tools(Event::PyStart))
        wanted &= 1 << traceTool;
    return wanted;
}

static constexpr uint16_t bothWays = bit(Event::BranchLeft) | bit(Event::BranchRight);

static ALWAYS_INLINE bool isToldOfElsewhere(const CodeMonitor::Point& point, unsigned event)
{
    return (event == Event::BranchLeft && point.leftIsToldOfAt) || (event == Event::BranchRight && point.rightIsToldOfAt);
}

uint8_t CodeMonitor::toolsWantedAt(Point& point)
{
    auto toldWhereItJumps = [&](const Point& arrival) -> uint8_t {
        return tools(Event::BranchRight) & ~pointAt(arrival.source)->disabledTools[Event::BranchRight];
    };
    uint8_t wanted = 0;
    // What there is a short way for, if nothing else is wanted here.
    uint8_t wantedOnShortWay = 0;
    auto shortWay = Point::ShortWay::None;
    bool isBranch = (point.kinds & bothWays) == bothWays;
    for (unsigned event = 0; event < Event::instrumentedCount; ++event) {
        if (event == Event::Line || !(point.kinds & bit(event)) || isToldOfElsewhere(point, event))
            continue;
        uint8_t toldOfEvent = tools(event) & ~point.disabledTools[event];
        if (isBranch && event == Event::BranchRight) {
            wantedOnShortWay = toldOfEvent;
            shortWay = Point::ShortWay::TakeBranch;
        } else
            wanted |= toldOfEvent;
    }
    if (point.kinds & Point::isWhereBranchFallsThrough)
        wanted |= tools(Event::BranchLeft) & ~pointAt(point.source)->disabledTools[Event::BranchLeft];
    if (point.kinds & Point::isWhereBranchJumps) {
        if (point.isAlsoJumpedTo && shortWay == Point::ShortWay::None) {
            wantedOnShortWay = toldWhereItJumps(point);
            shortWay = Point::ShortWay::ArriveByJump;
        } else
            wanted |= toldWhereItJumps(point);
    }
    if (point.leadsWhereBranchJumps) {
        // (A `break` that a branch leads to, and a jump: it may be all of this at once. Then there is no short way.)
        uint8_t toldThere = toldWhereItJumps(*pointAt(point.target));
        if (wantedOnShortWay)
            wanted |= toldThere;
        else {
            wantedOnShortWay = toldThere;
            shortWay = Point::ShortWay::SayItIsAJump;
        }
    }
    if (point.kinds & bit(Event::PyResume))
        wanted |= tools(Event::PyThrow);
    if (point.kinds & Point::construct)
        wanted |= tools(Event::Call);
    if (point.kinds & Point::forLoop)
        wanted |= tools(Event::Raise);
    if (point.kinds & bit(Event::Line)) {
        if (m_needsLastLine)
            wanted |= m_lineToolsStillOn;
        else {
            for (auto& start : lineStartsOf(point))
                wanted |= toolsToTellOf(start);
        }
    }
    if (point.lineStartOfTarget != noLineStart)
        wanted |= m_needsLastLine ? m_lineToolsStillOn : toolsToTellOf(m_lineStarts[point.lineStartOfTarget]);
    point.shortWay = wantedOnShortWay && !wanted ? shortWay : Point::ShortWay::None;
    return wanted | wantedOnShortWay;
}

void CodeMonitor::update(Point& point)
{
    bool isWanted = m_isAwake && toolsWantedAt(point);
    if (isWanted == point.isPatched)
        return;
    *opcodeAddress(point.offset) = isWanted ? static_cast<uint8_t>(op_py_instrumented) : byteOfOpcode(point.opcode);
    point.isPatched = isWanted;
}

void CodeMonitor::updateBranch(Point& point)
{
    update(point);
    if (point.leftIsToldOfAt)
        update(*pointAt(point.leftIsToldOfAt));
    if (!point.rightIsToldOfAt)
        return;
    Point* arrival = pointAt(point.rightIsToldOfAt);
    update(*arrival);
    if (!arrival->isAlsoJumpedTo)
        return;
    for (Point* other = &point + 1; other < arrival; ++other) {
        if (other->leadsWhereBranchJumps && other->target == arrival->offset)
            update(*other);
    }
}

void CodeMonitor::update()
{
    if (!m_isAwake) {
        setEntrancesAreReplaced(isWanted() || m_hasToForget);
        return;
    }
    bool everyInstruction = tools(Event::Instruction);
    if (!m_hasPoints || (everyInstruction && !m_hasEveryInstruction))
        findPoints(everyInstruction);
    noteLineTools();
    for (auto& point : m_points)
        update(point);
}

void CodeMonitor::sleep()
{
    if (m_isAwake) {
        putBackInstructions();
        m_isAwake = false;
    }
    m_hasToForget = m_hasSomethingDisabled;
    update();
}

void CodeMonitor::wake()
{
    if (m_hasToForget)
        forget();
    if (m_isAwake)
        return;
    m_isAwake = isWanted();
    setEntrancesAreReplaced(false);
    update();
}

// What DISABLE switched off stays off until restart_events(), or until the tool no longer wants the event at a time when the code is
// running or is entered: that is when CPython changes code, and it takes what it knows of the tool out of it then.
void CodeMonitor::forget()
{
    m_hasToForget = false;
    uint8_t stillDisabled = 0;
    std::array<uint8_t, Event::instrumentedCount> wanted;
    for (unsigned event = 0; event < Event::instrumentedCount; ++event)
        wanted[event] = tools(event);
    for (auto& point : m_points) {
        for (unsigned event = 0; event < Event::instrumentedCount; ++event)
            stillDisabled |= point.disabledTools[event] &= wanted[event];
    }
    for (auto& start : m_lineStarts)
        stillDisabled |= start.disabledTools &= wanted[Event::Line];
    m_hasSomethingDisabled = stillDisabled;
}

void CodeMonitor::noteLineTools()
{
    m_lineToolsStillOn = 0;
    if (!m_needsLastLine)
        return;
    for (auto& start : m_lineStarts)
        m_lineToolsStillOn |= toolsToTellOf(start);
}

void CodeMonitor::didDisableLine(unsigned offset, const LineStart& start)
{
    if (!m_needsLastLine && !start.isShared) {
        if (auto* point = pointAt(offset))
            update(*point);
        return;
    }
    uint8_t before = m_lineToolsStillOn;
    noteLineTools();
    if (before != m_lineToolsStillOn || start.isShared)
        update();
}

void CodeMonitor::restart()
{
    if (!std::exchange(m_hasSomethingDisabled, false))
        return;
    for (auto& point : m_points)
        point.disabledTools.fill(0);
    for (auto& start : m_lineStarts)
        start.disabledTools = 0;
    m_hasToForget = false;
    update();
}

// ---- What is wanted ----

Monitoring::Monitoring(Runtime& runtime)
    : m_runtime(runtime)
{
}

Monitoring::~Monitoring()
{
    for (CodeMonitor* monitor : m_monitors)
        monitor->m_monitoring = nullptr;
}

// A list: the names of the tools; their callbacks, by tool and event; DISABLE; MISSING; what trace and profile functions are told (What).
enum StateField : unsigned { StateNames, StateCallbacks, StateDisable, StateMissing, StateWhat, StateFieldCount };

void Monitoring::createState()
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    JSGlobalObject* globalObject = m_runtime.globalObject();
    JSArray* state = newList(globalObject);
    JSArray* names = newList(globalObject);
    for (unsigned i = 0; i < numberOfMonitoringTools; ++i) {
        listAppend(globalObject, names, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
    }
    JSArray* callbacks = newList(globalObject);
    for (unsigned i = 0; i < numberOfMonitoringTools * Event::count; ++i) {
        listAppend(globalObject, callbacks, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
    }
    listAppend(globalObject, state, names);
    RETURN_IF_EXCEPTION(scope, void());
    listAppend(globalObject, state, callbacks);
    RETURN_IF_EXCEPTION(scope, void());
    listAppend(globalObject, state, PyObject::create(m_runtime.vm(), m_runtime.structure<PyObject>(), m_runtime.objectType()));
    RETURN_IF_EXCEPTION(scope, void());
    listAppend(globalObject, state, PyObject::create(m_runtime.vm(), m_runtime.structure<PyObject>(), m_runtime.objectType()));
    RETURN_IF_EXCEPTION(scope, void());
    JSArray* what = newList(globalObject);
    for (ASCIILiteral name : { "call"_s, "line"_s, "return"_s, "exception"_s, "opcode"_s, "c_call"_s, "c_return"_s, "c_exception"_s }) {
        m_namesOfWhat[listLength(what)] = internString(m_runtime.vm(), name);
        listAppend(globalObject, what, m_namesOfWhat[listLength(what)]);
        RETURN_IF_EXCEPTION(scope, void());
    }
    listAppend(globalObject, state, what);
    RETURN_IF_EXCEPTION(scope, void());
    m_runtime.setSlot(Runtime::Slot::Monitoring, state);
}

JSValue Monitoring::disable() { return listGet(state(), StateDisable); }
JSValue Monitoring::missing() { return listGet(state(), StateMissing); }

JSValue Monitoring::callback(unsigned tool, unsigned event)
{
    JSValue callback = listGet(asArray(listGet(state(), StateCallbacks)), tool * Event::count + event);
    return callback.isUndefined() ? JSValue() : callback;
}

JSValue Monitoring::setCallback(unsigned tool, unsigned event, JSValue callback)
{
    JSValue previous = this->callback(tool, event);
    listSet(m_runtime.globalObject(), asArray(listGet(state(), StateCallbacks)), tool * Event::count + event, callback ? callback : jsUndefined());
    return previous;
}

JSValue Monitoring::toolName(unsigned tool)
{
    JSValue name = listGet(asArray(listGet(state(), StateNames)), tool);
    return name.isUndefined() ? JSValue() : name;
}

void Monitoring::setToolName(unsigned tool, JSValue name)
{
    listSet(m_runtime.globalObject(), asArray(listGet(state(), StateNames)), tool, name ? name : jsUndefined());
}

void Monitoring::noteWhatIsWanted()
{
    m_globalTools.fill(0);
    for (unsigned tool = 0; tool < numberOfMonitoringTools; ++tool) {
        for (unsigned event = 0; event < Event::ungroupedCount; ++event) {
            if (m_events[tool] >> event & 1)
                m_globalTools[event] |= 1 << tool;
        }
    }
    m_globalToolsWantPlaces = toolsWantPlaces(m_globalTools);
    m_wantsExceptions = toolsWantExceptions(m_globalTools) || m_monitorsWhoseLocalToolsWantExceptions;
}

// A monitor goes away when its code is swept, so nothing may be allocated meanwhile.
template<typename Functor>
void Monitoring::forEachMonitor(const Functor& functor)
{
    AssertNoGC assertNoGC;
    for (CodeMonitor* monitor : m_monitors) {
        if (monitor->code())
            functor(*monitor);
    }
}

CodeMonitor* Monitoring::monitorFor(CodeBlock* codeBlock)
{
    UnlinkedCodeBlock* code = codeBlock->unlinkedCodeBlock();
    if (!code->isPython())
        return nullptr;
    auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable());
    CodeInfo* info = executable ? executable->unlinkedExecutable()->pythonCodeInfo() : nullptr;
    if (!info)
        return nullptr;
    // A generator function does nothing but make the generator. The code is the body's.
    if (info->isGeneratorLike() && code->parseMode() != SourceParseMode::GeneratorBodyMode)
        return nullptr;
    if (!info->monitor)
        info->monitor = makeUnique<CodeMonitor>(*this, code, *info);
    ASSERT(info->monitor->code() == code);
    return info->monitor.get();
}

CodeMonitor* Monitoring::monitorFor(JSGlobalObject* globalObject, PyCode* code)
{
    if (CodeMonitor* monitor = code->codeInfo().monitor.get())
        return monitor;
    CodeBlock* codeBlock = codeBlockOf(globalObject, code);
    return codeBlock ? monitorFor(codeBlock) : nullptr;
}

void Monitoring::didCreateCodeBlock(CodeBlock* codeBlock)
{
    if (!m_knowsAllCode)
        return;
    if (CodeMonitor* monitor = monitorFor(codeBlock))
        monitor->update();
}

void Monitoring::findAllCode()
{
    if (std::exchange(m_knowsAllCode, true))
        return;
    Vector<CodeBlock*> codeBlocks;
    m_runtime.vm().heap.forEachCodeBlock([&](CodeBlock* codeBlock) {
        if (codeBlock->unlinkedCodeBlock()->isPython() && codeBlock->globalObject() == m_runtime.globalObject())
            codeBlocks.append(codeBlock);
    });
    for (CodeBlock* codeBlock : codeBlocks)
        monitorFor(codeBlock);
}

void Monitoring::updateAll()
{
    noteWhatIsWanted();
    if (m_globalToolsWantPlaces)
        findAllCode();
    forEachMonitor([](CodeMonitor& monitor) {
        monitor.sleep();
    });
    wakeCodeOfCallsInProgress();
}

void Monitoring::wakeCodeOfCallsInProgress()
{
    VM& vm = m_runtime.vm();
    JSGlobalObject* globalObject = m_runtime.globalObject();
    auto walk = [&](CallFrame* callFrame, EntryFrame* entryFrame) {
        for (; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
            if (callFrame->isNativeCalleeFrame() || !callFrame->codeBlock() || !callFrame->codeBlock()->unlinkedCodeBlock()->isPython() || callFrame->isZombieFrame())
                continue;
            if (callFrame->codeBlock()->globalObject() != globalObject)
                continue;
            CodeMonitor* monitor = m_knowsAllCode ? monitorFor(callFrame->codeBlock()) : codeInfoOf(callFrame).monitor.get();
            if (!monitor)
                continue;
            monitor->wake();
            if (!monitor->isAwake())
                continue;
            // A frame that is under way when lines begin to be told of has been on its line for a while.
            if (!monitor->m_needsLastLine && !(m_globalTools[Event::Line] >> traceTool & 1))
                continue;
            PyFrame* frame = PyFrame::ofForTracing(globalObject, callFrame);
            if (frame->lastLine < 0)
                frame->lastLine = lineOfMarks(callFrame->codeBlock()->unlinkedCodeBlock()->pythonMarks(), callFrame->bytecodeIndex().offset());
        }
    };
    walk(vm.topCallFrame, vm.topEntryFrame);
    if (vm.apiLock().hasParkedThreads()) {
        Vector<std::pair<CallFrame*, EntryFrame*>, 8> others;
        vm.apiLock().forEachParkedThread([&](JSLock::ParkedThread& parked) {
            others.append({ parked.topCallFrame(), parked.topEntryFrame() });
        });
        for (auto [callFrame, entryFrame] : others)
            walk(callFrame, entryFrame);
    }
}

Monitoring::OriginalInstructions::OriginalInstructions(Monitoring& monitoring)
    : m_monitoring(monitoring)
    , m_isForAll(true)
{
    monitoring.forEachMonitor([](CodeMonitor& monitor) {
        monitor.putBackInstructions();
    });
}

Monitoring::OriginalInstructions::OriginalInstructions(Monitoring& monitoring, PyCode* code)
    : m_monitoring(monitoring)
    , m_only(code->codeInfo().monitor.get())
    , m_isForAll(false)
{
    if (m_only)
        m_only->putBackInstructions();
}

Monitoring::OriginalInstructions::~OriginalInstructions()
{
    if (m_only)
        m_only->update();
    if (!m_isForAll)
        return;
    m_monitoring.forEachMonitor([](CodeMonitor& monitor) {
        monitor.update();
    });
}

void Monitoring::setEvents(unsigned tool, uint32_t events)
{
    if (m_events[tool] == events)
        return;
    m_events[tool] = events;
    updateAll();
}

void Monitoring::clearTool(unsigned tool)
{
    for (unsigned event = 0; event < Event::count; ++event)
        setCallback(tool, event, { });
    bool wantedSomethingEverywhere = std::exchange(m_events[tool], 0);
    if (m_monitorsWithLocalTools) {
        forEachMonitor([&](CodeMonitor& monitor) {
            if (!monitor.m_hasLocalTools || !monitor.localEvents(tool))
                return;
            monitor.setLocalEvents(tool, 0);
            if (!wantedSomethingEverywhere)
                monitor.sleepAndWake();
        });
    }
    if (wantedSomethingEverywhere)
        updateAll();
    else
        noteWhatIsWanted();
}

void Monitoring::restartEvents()
{
    forEachMonitor([](CodeMonitor& monitor) {
        monitor.restart();
    });
}

// ---- sys.settrace(), sys.setprofile() ----

static constexpr uint32_t traceEvents = 1 << Event::PyStart | 1 << Event::PyResume | 1 << Event::PyReturn | 1 << Event::PyYield | 1 << Event::Raise | 1 << Event::Line
    | 1 << Event::Jump | 1 << Event::PyUnwind | 1 << Event::PyThrow | 1 << Event::StopIteration;
static constexpr uint32_t profileEvents = 1 << Event::PyStart | 1 << Event::PyResume | 1 << Event::PyReturn | 1 << Event::PyYield | 1 << Event::Call | 1 << Event::PyUnwind
    | 1 << Event::PyThrow;

void Monitoring::tracingThreadCountChanged(int traceDelta, int profileDelta)
{
    m_tracingThreads += traceDelta;
    m_profilingThreads += profileDelta;
    setEvents(traceTool, m_tracingThreads ? traceEvents : 0);
    setEvents(profileTool, m_profilingThreads ? profileEvents : 0);
}

void Monitoring::setTraceFunction(JSValue function)
{
    int delta = !!function - !!m_runtime.slot(Runtime::Slot::TraceFunction);
    m_runtime.setSlot(Runtime::Slot::TraceFunction, function);
    tracingThreadCountChanged(delta, 0);
    if (!function)
        return;
    // As for a frame that a function is about to be called for.
    if (PyFrame* frame = currentFrame(m_runtime.globalObject()); frame && frame->tracesOpcodes)
        setTracesOpcodes(m_runtime.globalObject(), frame, true);
}

void Monitoring::setProfileFunction(JSValue function)
{
    int delta = !!function - !!m_runtime.slot(Runtime::Slot::ProfileFunction);
    m_runtime.setSlot(Runtime::Slot::ProfileFunction, function);
    tracingThreadCountChanged(0, delta);
}

void Monitoring::setTracesOpcodes(JSGlobalObject* globalObject, PyFrame* frame, bool enable)
{
    // A generator that has not begun: whatever happens first in the frame leads here again (legacyEvent()).
    CodeBlock* codeBlock = codeBlockIfAny(frame->code(globalObject));
    CodeMonitor* monitor = codeBlock ? monitorFor(codeBlock) : nullptr;
    if (!monitor)
        return;
    uint32_t events = monitor->localEvents(traceTool);
    uint32_t wanted = enable ? events | 1 << Event::Instruction : events & ~(1 << Event::Instruction);
    if (events == wanted)
        return;
    monitor->setLocalEvents(traceTool, wanted);
    monitor->sleepAndWake();
    // The frame may be that of a caller.
    if (!enable || !frame->callFrame() || !monitor->isAwake())
        return;
    if (auto* point = monitor->pointAt(frame->bytecodeOffset()); point && point->isLastOfStatement)
        frame->callWithResultToTake = point->offset;
}

// Something else is about to run than what a tool called: another thread, or what sys.call_tracing() was given.
void Monitoring::toolIsInterrupted()
{
    CodeMonitor* monitor = std::exchange(m_codeLeftAlone, nullptr);
    if (monitor && !monitor->isAwake())
        monitor->update();
}

// Nothing that happens meanwhile is told of.
JSValue Monitoring::callWhileTracing(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned event, Shown shown, JSValue function, const CallArgs& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    SetForScope tracedEvent(m_runtime.tracedEvent, static_cast<int>(event));
    SetForScope tracedCallFrame(m_runtime.tracedCallFrame, callFrame);
    SetForScope tracedLine(m_runtime.tracedLine, shown.line);
    SetForScope tracedOffset(m_runtime.tracedOffset, shown.offset);
    // The function would say that it is entered, only to be left asleep (see handle()). So, for as long as it runs, it does not.
    RefPtr<CodeInfo> info;
    if (auto* callee = PyFunction::tryFrom(function))
        info = &callee->codeInfo();
    else if (auto* method = PyMethod::tryFrom(function)) {
        if (auto* callee = PyFunction::tryFrom(method->function()))
            info = &callee->codeInfo();
    }
    CodeMonitor* monitor = info ? info->monitor.get() : nullptr;
    if (monitor && monitor->m_entrancesAreReplaced && !monitor->m_hasToForget) {
        monitor->setEntrancesAreReplaced(false);
        m_codeLeftAlone = monitor;
    } else
        monitor = nullptr;
    if (shown.line >= 0) {
        if (PyFrame* frame = frameObjectIfThereIsOne(callFrame)) {
            frame->shownLine = shown.line;
            frame->shownOffset = shown.offset;
        }
    }
    ++m_runtime.tracingDepth;
    JSValue result = call(globalObject, function, arguments);
    EXCEPTION_ASSERT_UNUSED(scope, !!scope.exception() == !result);
    --m_runtime.tracingDepth;
    if (PyFrame* frame = frameObjectIfThereIsOne(callFrame))
        frame->shownLine = frame->shownOffset = -1;
    if (monitor) {
        m_codeLeftAlone = nullptr;
        if (!monitor->isAwake())
            monitor->update();
    }
    return result;
}

bool Monitoring::callTraceFunction(JSGlobalObject* globalObject, PyFrame* frame, unsigned event, Shown shown, What what, JSValue argument)
{
    VM& vm = m_runtime.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue traceFunction = m_runtime.slot(Runtime::Slot::TraceFunction);
    if (!traceFunction)
        return true;
    // A function is called with "call", and what it returns with whatever else happens in the frame.
    JSValue function = event == Event::PyStart || event == Event::PyResume || event == Event::PyThrow ? traceFunction : frame->trace();
    if (!function)
        return true;
    JSValue arguments[3] = { frame, nameOf(what), argument };
    JSValue result = callWhileTracing(globalObject, frame->callFrame(), event, shown, function, CallArgs(arguments));
    if (failed(scope, result)) {
        ExceptionSetAside suspended(globalObject);
        setTraceFunction({ });
        frame->setTrace(vm, { });
        return false;
    }
    if (!isNone(result) && result != frame->trace())
        frame->setTrace(vm, result);
    return true;
}

bool Monitoring::callProfileFunction(JSGlobalObject* globalObject, PyFrame* frame, unsigned event, Shown shown, What what, JSValue argument)
{
    VM& vm = m_runtime.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = m_runtime.slot(Runtime::Slot::ProfileFunction);
    if (!function)
        return true;
    JSValue arguments[3] = { frame, nameOf(what), argument };
    if (!failed(scope, callWhileTracing(globalObject, frame->callFrame(), event, shown, function, CallArgs(arguments))))
        return true;
    ExceptionSetAside suspended(globalObject);
    setProfileFunction({ });
    return false;
}

bool Monitoring::traceLine(JSGlobalObject* globalObject, PyFrame* frame, Shown shown)
{
    if (!frame->tracesLines || shown.line < 0)
        return true;
    return callTraceFunction(globalObject, frame, Event::Line, shown, What::Line, jsUndefined());
}

// A profile function is told of calls of built-in functions only. Empty, without an exception: this is not one.
static JSValue builtinFunctionToProfile(JSGlobalObject* globalObject, JSValue callable, JSValue first)
{
    Runtime& rt = runtime(globalObject);
    auto* native = PyNativeFunction::tryFrom(callable);
    if (!native)
        return { };
    switch (native->kind()) {
    case NativeKind::Function:
    case NativeKind::StaticMethod:
    case NativeKind::New:
        return callable;
    case NativeKind::Method:
        // A method of a built-in type, called with the object: it is told of as the method of the object, as it used to be.
        if (first == rt.monitoring().missing())
            return { };
        return bindDescriptor(globalObject, callable, first, rt.typeOf(first));
    case NativeKind::SlotWrapper:
    case NativeKind::ClassMethod:
        return { };
    }
    return { };
}

static Monitoring::Shown whatToShow(CallFrame* callFrame, CodeMonitor& monitor, unsigned event, unsigned offset)
{
    switch (event) {
    case Event::PyStart:
        // Nothing has happened yet.
        return { monitor.lineBeforeStart(), 0 };
    case Event::PyUnwind:
    case Event::Reraise:
        if (monitor.lineAt(offset) < 0 && raisesAgainAsIfFromWhereItWasRaised(callFrame->codeBlock(), BytecodeIndex(offset))) {
            if (PyFrame* frame = frameObjectIfAny(callFrame); frame && frame->raisedAt >= 0 && monitor.lineAt(frame->raisedAt) >= 0)
                return { monitor.lineAt(frame->raisedAt), static_cast<int>(visibleOffset(monitor.code()->pythonMarks(), frame->raisedAt)) };
        }
        return { };
    case Event::Instruction:
        // Point::isLastOfStatement: the frame has gone on.
        if (callFrame->bytecodeIndex().offset() != offset)
            return { monitor.lineAt(offset), static_cast<int>(offset) };
        return { };
    default:
        return { };
    }
}

// What legacy_tracing.c does in CPython.
bool Monitoring::legacyEvent(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned tool, unsigned event, unsigned offset, JSValue third, JSValue fourth, bool& disable)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    if (!m_runtime.slot(tool == traceTool ? Runtime::Slot::TraceFunction : Runtime::Slot::ProfileFunction))
        return true;
    PyFrame* frame = frameObjectOf(globalObject, callFrame);
    Shown shown = whatToShow(callFrame, monitor, event, offset);

    if (tool == profileTool) {
        switch (event) {
        case Event::PyStart:
        case Event::PyResume:
        case Event::PyThrow:
            RELEASE_AND_RETURN(scope, callProfileFunction(globalObject, frame, event, shown, What::Call, jsUndefined()));
        case Event::PyReturn:
        case Event::PyYield:
            RELEASE_AND_RETURN(scope, callProfileFunction(globalObject, frame, event, shown, What::Return, third));
        case Event::PyUnwind:
            RELEASE_AND_RETURN(scope, callProfileFunction(globalObject, frame, event, shown, What::Return, jsUndefined()));
        case Event::Call:
        case Event::CReturn:
        case Event::CRaise: {
            What what = event == Event::Call ? What::CCall : event == Event::CReturn ? What::CReturn : What::CException;
            JSValue function = builtinFunctionToProfile(globalObject, third, fourth);
            RETURN_IF_EXCEPTION(scope, false);
            if (!function)
                return true;
            RELEASE_AND_RETURN(scope, callProfileFunction(globalObject, frame, event, shown, what, function));
        }
        default:
            return true;
        }
    }

    if (event != Event::Instruction && event != Event::Jump && frame->tracesOpcodes)
        setTracesOpcodes(globalObject, frame, true);
    switch (event) {
    case Event::PyStart:
    case Event::PyResume:
    case Event::PyThrow:
        RELEASE_AND_RETURN(scope, callTraceFunction(globalObject, frame, event, shown, What::Call, jsUndefined()));
    case Event::PyReturn:
    case Event::PyYield:
        RELEASE_AND_RETURN(scope, callTraceFunction(globalObject, frame, event, shown, What::Return, third));
    case Event::PyUnwind:
        RELEASE_AND_RETURN(scope, callTraceFunction(globalObject, frame, event, shown, What::Return, jsUndefined()));
    case Event::Raise:
    case Event::StopIteration: {
        JSValue traceback = jsUndefined();
        if (auto* exception = PyException::tryFrom(third); exception && exception->traceback())
            traceback = exception->traceback();
        RELEASE_AND_RETURN(scope, callTraceFunction(globalObject, frame, event, shown, What::Exception, PyTuple::create(globalObject, { m_runtime.typeOf(third), third, traceback })));
    }
    case Event::Jump: {
        // A trace function is told of a line each time a loop comes round to it, even if the whole loop is on that line.
        unsigned target = third.asInt32() / shownOffset(1);
        int line = monitor.firstLineAt(target);
        if (target > offset || line != monitor.lineAt(offset)) {
            disable = true;
            return true;
        }
        frame->lastLine = line;
        RELEASE_AND_RETURN(scope, traceLine(globalObject, frame, { line, static_cast<int>(target) }));
    }
    case Event::Instruction:
        if (!frame->tracesOpcodes) {
            setTracesOpcodes(globalObject, frame, false);
            return true;
        }
        RELEASE_AND_RETURN(scope, callTraceFunction(globalObject, frame, event, shown, What::Opcode, jsUndefined()));
    default:
        return true;
    }
}

// ---- What happens ----

int Monitoring::callTool(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned tool, unsigned event, Shown shown, std::span<const JSValue> arguments)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    JSValue function = callback(tool, event);
    if (!function)
        return 0;
    JSValue result = callWhileTracing(globalObject, callFrame, event, shown, function, CallArgs(arguments));
    if (failed(scope, result))
        return -1;
    return result == disable();
}

static constexpr ASCIILiteral eventNames[Event::count] = {
    "PY_START"_s, "PY_RESUME"_s, "PY_RETURN"_s, "PY_YIELD"_s, "CALL"_s, "LINE"_s, "INSTRUCTION"_s, "JUMP"_s, "BRANCH_LEFT"_s, "BRANCH_RIGHT"_s, "STOP_ITERATION"_s,
    "RAISE"_s, "EXCEPTION_HANDLED"_s, "PY_UNWIND"_s, "PY_THROW"_s, "RERAISE"_s, "C_RETURN"_s, "C_RAISE"_s, "BRANCH"_s,
};

bool Monitoring::fire(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned offset, unsigned event, uint8_t tools, JSValue third, JSValue fourth, std::optional<unsigned> shownOffsetOfPlace)
{
    if (!tools)
        return true;
    VM& vm = m_runtime.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue arguments[4] = { { }, { }, third, fourth };
    unsigned count = fourth ? 4 : third ? 3 : 2;
    if (tools & ((1 << numberOfFreeMonitoringTools) - 1)) {
        arguments[0] = codeOf(globalObject, callFrame);
        // What raises an exception again on its way out of a `with` has two marks: the line of the `with`, which is where a tool is told
        // it is, and no line, which is what the frame goes by.
        unsigned place = shownOffsetOfPlace.value_or(offset);
        arguments[1] = jsNumber(shownOffset(event == Event::Reraise ? place : visibleOffset(monitor.code()->pythonMarks(), place)));
    }
    while (tools) {
        unsigned tool = 31 - std::countl_zero(static_cast<uint32_t>(tools));
        tools &= ~(1 << tool);
        int result;
        if (tool >= profileTool) {
            bool disable = false;
            result = legacyEvent(globalObject, callFrame, monitor, tool, event, offset, third, fourth, disable) ? disable : -1;
        } else
            result = callTool(globalObject, callFrame, tool, event, whatToShow(callFrame, monitor, event, offset), std::span<const JSValue>(arguments, count));
        if (failed(scope, result >= 0))
            return false;
        if (!result)
            continue;
        if (event < Event::instrumentedCount) {
            if (auto* point = monitor.pointAt(offset)) {
                point->disabledTools[event] |= 1 << tool;
                monitor.m_hasSomethingDisabled = true;
                // BRANCH, the event of old, is either way a branch goes.
                if ((event == Event::BranchLeft || event == Event::BranchRight) && callback(tool, Event::Branch))
                    point->disabledTools[Event::BranchLeft] = point->disabledTools[Event::BranchRight] |= 1 << tool;
                monitor.updateBranch(*point);
            }
        } else if (event < Event::ungroupedCount) {
            monitor.removeLocalTool(event, tool);
            noteWhatIsWanted();
        } else {
            setCallback(tool, event, { });
            raiseValueError(globalObject, scope, "Cannot disable "_s, eventNames[event], " events. Callback removed."_s);
            return false;
        }
    }
    return true;
}

bool Monitoring::fireLineStart(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned offset, unsigned index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int line = monitor.m_lineStarts[index].line;
    Shown shown { line, static_cast<int>(monitor.m_lineStarts[index].offset) };
    uint8_t tools = monitor.tools(Event::Line) & ~monitor.m_lineStarts[index].disabledTools;
    if (monitor.m_lineStarts[index].isOnLineOfDefinition && std::exchange(m_startIsToldOfIn, nullptr) != callFrame)
        tools &= 1 << traceTool;
    if (!tools)
        return true;
    if (tools >> traceTool & 1) {
        PyFrame* frame = frameObjectOf(globalObject, callFrame);
        if (frame->trace()) {
            if (failed(scope, traceLine(globalObject, frame, shown)))
                return false;
            if (frame->jumpTarget)
                return true;
        }
    }
    tools &= (1 << numberOfFreeMonitoringTools) - 1;
    if (!tools)
        return true;
    JSValue arguments[2] = { codeOf(globalObject, callFrame), jsNumber(line) };
    for (unsigned tool = numberOfFreeMonitoringTools; tool--;) {
        if (!(tools >> tool & 1))
            continue;
        int result = callTool(globalObject, callFrame, tool, Event::Line, shown, arguments);
        if (failed(scope, result >= 0))
            return false;
        if (!result)
            continue;
        auto& start = monitor.m_lineStarts[index];
        start.disabledTools |= 1 << tool;
        monitor.m_hasSomethingDisabled = true;
        monitor.didDisableLine(offset, start);
    }
    return true;
}

// What throw() raises comes from outside: a handler on the line of the `yield` is told of as a line that begins.
static int lineItIsRaisedOn(CodeBlock* codeBlock, unsigned offset)
{
    return throwsIntoGenerator(codeBlock, BytecodeIndex(offset)) ? -1 : lineOfMarks(codeBlock->unlinkedCodeBlock()->pythonMarks(), offset);
}

bool Monitoring::fireLine(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, const CodeMonitor::Point& point, bool comesFromSameLine)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    unsigned offset = point.offset;
    unsigned first = point.firstLineStart;
    unsigned count = point.lineStartCount;
    bool firstLineIsInDoubt = point.firstLineIsInDoubt;
    // A handler on the line of what raised the exception: `with a: b()`.
    if (point.firstLineFollowsCatch && m_raisedIn == callFrame && lineItIsRaisedOn(callFrame->codeBlock(), m_raisedAt) == monitor.m_lineStarts[first].line)
        comesFromSameLine = true;
    PyFrame* frame = monitor.m_needsLastLine || monitor.tools(Event::Line) >> traceTool ? frameObjectOf(globalObject, callFrame) : frameObjectIfThereIsOne(callFrame);
    // It got here because a trace function said so.
    if (frame && frame->jumpedTo) {
        if (*std::exchange(frame->jumpedTo, std::nullopt) == offset) {
            frame->lastLine = monitor.m_lineStarts[first + count - 1].line;
            return true;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        int line = monitor.m_lineStarts[first + i].line;
        bool isNews = i || (!comesFromSameLine && (!firstLineIsInDoubt || line != frame->lastLine));
        if (frame)
            frame->lastLine = line;
        if (!isNews)
            continue;
        if (failed(scope, fireLineStart(globalObject, callFrame, monitor, offset, first + i)))
            return false;
        if (!frame)
            frame = frameObjectIfThereIsOne(callFrame);
        if (frame && frame->jumpTarget)
            return true;
    }
    return true;
}

// Point::goesOnInItself: in its last byte.
static constexpr unsigned whereItGoesOnInItself(unsigned offsetOfNextInstruction) { return offsetOfNextInstruction - 1; }

static SlowPathFunction slowPathOfBranch(OpcodeID opcode)
{
    switch (opcode) {
    case op_py_jtrue: return slow_path_py_jtrue;
    case op_py_jfalse: return slow_path_py_jfalse;
    case op_py_jeq: return slow_path_py_jeq;
    case op_py_jne: return slow_path_py_jne;
    case op_py_jlt: return slow_path_py_jlt;
    case op_py_jle: return slow_path_py_jle;
    case op_py_jgt: return slow_path_py_jgt;
    case op_py_jge: return slow_path_py_jge;
    case op_py_jneq: return slow_path_py_jneq;
    case op_py_jnne: return slow_path_py_jnne;
    case op_py_jnlt: return slow_path_py_jnlt;
    case op_py_jnle: return slow_path_py_jnle;
    case op_py_jngt: return slow_path_py_jngt;
    case op_py_jnge: return slow_path_py_jnge;
    case op_py_for_iter: return slow_path_py_for_iter;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static ALWAYS_INLINE PyGenerator* generatorOf(CallFrame* callFrame)
{
    return uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell());
}

static ALWAYS_INLINE bool isResumedToThrow(CallFrame* callFrame)
{
    return callFrame->uncheckedArgument(static_cast<int>(JSGenerator::Argument::ResumeMode) - 1).asInt32() != static_cast<int32_t>(JSGenerator::ResumeMode::NormalMode);
}

static bool isSendThatThrows(CallFrame* callFrame, const JSInstruction* instruction)
{
    if (instruction->opcodeID() != op_py_send)
        return false;
    int mode = callFrame->r(instruction->as<OpPySend>().m_mode).jsValue().asInt32();
    return mode != static_cast<int>(JSGenerator::ResumeMode::NormalMode) && mode < firstSendOfAwait;
}

// iterNext(), for a `for` loop. If the iterator ends by raising StopIteration, that is told of as raised in the loop. A generator that
// returns something raises it as far as anybody can tell.
static JSValue nextOfLoop(JSGlobalObject* globalObject, JSValue iterator, JSValue& stopIteration)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    if (auto* generator = PyGenerator::tryFrom(iterator); generator && generator->kind() == PyGenerator::Kind::Generator) {
        JSValue returned;
        JSValue item = generatorSend(globalObject, generator, jsUndefined(), returned);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item && returned && !isNone(returned)) {
            scope.release();
            stopIteration = newException(globalObject, rt.StopIteration(), std::span<const JSValue> { &returned, 1 });
        }
        return item;
    }
    PyType* type = rt.typeOf(iterator);
    JSValue method = type->isHeapType() ? type->lookup(Special::Next) : JSValue();
    if (!method || isNone(method))
        RELEASE_AND_RETURN(scope, iterNext(globalObject, iterator));
    JSValue item = callSpecial(globalObject, method, iterator);
    if (failed(scope, item) && pendingExceptionMatches(globalObject, rt.StopIteration()))
        stopIteration = takePendingException(globalObject);
    return item;
}

Monitoring::Outcome Monitoring::handle(JSGlobalObject* globalObject, CallFrame* callFrame, const JSInstruction* pc)
{
    CodeMonitor* monitor = codeInfoOf(callFrame).monitor.get();
    ASSERT(monitor);
    unsigned offset = reinterpret_cast<const uint8_t*>(pc) - monitor->instructions();
    if (!monitor->isAwake()) {
        // What a tool does is not told of, so there is nothing to look for in what it calls.
        if (m_runtime.tracingDepth && !monitor->m_hasToForget)
            return { pc, *monitor->opcodeThatMayBeReplacedAt(offset) };
        monitor->wake();
        return { pc, goOn };
    }
    auto* point = monitor->pointAt(offset);
    ASSERT(point);
    // What a tool does is not told of.
    if (m_runtime.tracingDepth)
        return { pc, point->opcode };
    callFrame->setCallSiteIndex(CallSiteIndex(BytecodeIndex(offset)));
    switch (point->shortWay) {
    case CodeMonitor::Point::ShortWay::None:
        break;
    case CodeMonitor::Point::ShortWay::TakeBranch:
        return takeBranch(globalObject, callFrame, pc, *monitor, *point);
    case CodeMonitor::Point::ShortWay::SayItIsAJump:
        m_comesFromSameLine = { };
        m_jumpGoesOn = { };
        m_arrivesByJump = { callFrame, point->target };
        return { pc, point->opcode };
    case CodeMonitor::Point::ShortWay::ArriveByJump:
        if (m_arrivesByJump.callFrame != callFrame || m_arrivesByJump.offset != offset)
            break;
        m_comesFromSameLine = { };
        m_jumpGoesOn = { };
        m_arrivesByJump = { };
        return { pc, point->opcode };
    }
    return handle(globalObject, callFrame, pc, *monitor, point);
}

// See Point::ShortWay.
Monitoring::Outcome Monitoring::takeBranch(JSGlobalObject* globalObject, CallFrame* callFrame, const JSInstruction* pc, CodeMonitor& monitor, const CodeMonitor::Point& point)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    m_comesFromSameLine = { };
    m_jumpGoesOn = { };
    m_arrivesByJump = { };
    // What the branch calls may change what there is to know about the places in the code.
    unsigned offset = point.offset;
    unsigned next = point.next;
    unsigned target = point.target;
    unsigned shownTarget = point.shownTarget;
    auto opcode = static_cast<OpcodeID>(point.opcode);
    auto* whereItJumps = reinterpret_cast<const JSInstruction*>(monitor.instructions() + target);
    bool jumps;
    if (opcode == op_jundefined_or_null)
        jumps = callFrame->r(pc->as<OpJundefinedOrNull>().m_value).jsValue().isUndefinedOrNull();
    else if (opcode == op_jnundefined_or_null)
        jumps = !callFrame->r(pc->as<OpJnundefinedOrNull>().m_value).jsValue().isUndefinedOrNull();
    else {
        const void* destination;
        const void* signal;
        decodeResult(slowPathOfBranch(opcode)(callFrame, pc), destination, signal);
        if (failed(scope, signal != LLInt::exceptionSignal()))
            return { nullptr, 0 };
        jumps = destination == whereItJumps;
    }
    if (!jumps) [[likely]]
        return { reinterpret_cast<const JSInstruction*>(monitor.instructions() + next), goOn };
    callFrame->setCallSiteIndex(CallSiteIndex(BytecodeIndex(offset)));
    m_runtime.forgetOwnedTemporaries();
    auto* current = monitor.pointAt(offset);
    uint8_t tools = current ? monitor.tools(Event::BranchRight) & ~current->disabledTools[Event::BranchRight] : 0;
    if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::BranchRight, tools, jsNumber(shownOffset(shownTarget)))))
        return { nullptr, 0 };
    // As in handle().
    if (auto* other = shownTarget != target ? monitor.pointAt(target) : nullptr; other && other->isPatched && isJump(static_cast<OpcodeID>(other->opcode)))
        m_jumpGoesOn = { callFrame, target };
    return { whereItJumps, goOn };
}

NEVER_INLINE Monitoring::Outcome Monitoring::handle(JSGlobalObject* globalObject, CallFrame* callFrame, const JSInstruction* pc, CodeMonitor& monitor, CodeMonitor::Point* point)
{
    VM& vm = m_runtime.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A tool is shown the first argument of a call, and can assign to the variables of the frame.
    m_runtime.forgetOwnedTemporaries();
    CodeBlock* codeBlock = callFrame->codeBlock();
    unsigned offset = point->offset;
    auto at = [&](unsigned where) { return reinterpret_cast<const JSInstruction*>(monitor.instructions() + where); };
    auto setCurrentOffset = [&](unsigned where) { callFrame->setCallSiteIndex(CallSiteIndex(BytecodeIndex(where))); };
    // A tool may change what there is to know about the places in the code. These stay.
    auto opcode = static_cast<OpcodeID>(point->opcode);
    uint16_t kinds = point->kinds;
    unsigned offsetOfNext = point->next;
    unsigned target = point->target;
    unsigned shownTarget = point->shownTarget;
    unsigned source = point->source;
    bool isLastOfStatement = point->isLastOfStatement;
    Outcome runIt { pc, opcode };
    static constexpr Outcome threw { nullptr, 0 };

    auto next = [&] { return at(offsetOfNext); };
    unsigned generation = monitor.generation();
    auto currentPoint = [&] {
        if (generation != monitor.generation()) [[unlikely]] {
            generation = monitor.generation();
            point = monitor.pointAt(offset);
        }
        return point;
    };
    auto wanted = [&](unsigned event) -> uint8_t {
        auto* point = currentPoint();
        return point ? monitor.tools(event) & ~point->disabledTools[event] : 0;
    };
    // frame.f_lineno = line, in a trace function
    auto jumped = [&](Outcome& outcome) {
        PyFrame* frame = frameObjectIfThereIsOne(callFrame);
        if (!frame || !frame->jumpTarget)
            return false;
        frame->jumpedTo = std::exchange(frame->jumpTarget, std::nullopt);
        outcome = { at(*frame->jumpedTo), goOn };
        return true;
    };
    Outcome elsewhere;
    bool comesFromSameLine = m_comesFromSameLine.callFrame == callFrame && m_comesFromSameLine.offset == offset;
    m_comesFromSameLine = { };
    // A branch has led here: see Point::leftIsToldOfAt. (Unless a trace function has sent the frame here.)
    auto wasSentHere = [&] {
        PyFrame* frame = frameObjectIfThereIsOne(callFrame);
        return frame && frame->jumpedTo;
    };
    bool arrivesByJump = m_arrivesByJump.callFrame == callFrame && m_arrivesByJump.offset == offset;
    m_arrivesByJump = { };
    bool jumpIsToldOfAlready = m_jumpGoesOn.callFrame == callFrame && m_jumpGoesOn.offset == offset;
    m_jumpGoesOn = { };
    if (kinds & (CodeMonitor::Point::isWhereBranchFallsThrough | CodeMonitor::Point::isWhereBranchJumps) && !arrivesByJump && !wasSentHere()) {
        auto* branch = monitor.pointAt(source);
        unsigned event = kinds & CodeMonitor::Point::isWhereBranchJumps ? Event::BranchRight : Event::BranchLeft;
        unsigned shownDestination = event == Event::BranchLeft ? offset : branch->shownTarget;
        // As far as anybody can tell, the frame is still at the branch.
        setCurrentOffset(source);
        if (failed(scope, fire(globalObject, callFrame, monitor, source, event, monitor.tools(event) & ~branch->disabledTools[event], jsNumber(shownOffset(shownDestination)))))
            return threw;
        setCurrentOffset(offset);
        // This is a jump, then, which is not told of: CPython's compiler has the branch go where it goes.
        if (shownDestination != offset)
            jumpIsToldOfAlready = true;
    }
    if (kinds & bit(Event::Instruction)) {
        PyFrame* frame = frameObjectIfThereIsOne(callFrame);
        if (auto call = frame ? std::exchange(frame->callWithResultToTake, std::nullopt) : std::nullopt) {
            auto* pointOfCall = monitor.pointAt(*call);
            if (pointOfCall && pointOfCall->shownTarget == offset) {
                if (failed(scope, fire(globalObject, callFrame, monitor, *call, Event::Instruction, monitor.tools(Event::Instruction) & ~pointOfCall->disabledTools[Event::Instruction])))
                    return threw;
            }
        }
    }
    // What a jump, or a branch that is taken, has to say about the line that begins at its target. False: an exception, or the frame
    // has been sent elsewhere.
    auto tellOfLineAt = [&](unsigned target, Outcome& outcome) {
        auto* point = currentPoint();
        if (!point || point->lineStartOfTarget == CodeMonitor::noLineStart)
            return true;
        if (point->targetLineIsNotNews) {
            if (auto* other = monitor.pointAt(target); other && other->isPatched)
                m_comesFromSameLine = { callFrame, target };
            return true;
        }
        unsigned lineStart = point->lineStartOfTarget;
        if (monitor.m_needsLastLine || monitor.tools(Event::Line) >> traceTool)
            frameObjectOf(globalObject, callFrame)->lastLine = monitor.m_lineStarts[lineStart].line;
        if (failed(scope, fireLineStart(globalObject, callFrame, monitor, offset, lineStart))) {
            outcome = threw;
            return false;
        }
        return !jumped(outcome);
    };

    if (kinds & bit(Event::PyStart)) {
        m_startIsToldOfIn = wanted(Event::PyStart) ? callFrame : nullptr;
        PyFrame* frame = frameObjectIfAny(callFrame);
        if (frame && frame->jumpedTo && std::exchange(frame->jumpsToStart, false))
            frame->jumpedTo = std::nullopt;
        else if (frame && frame->jumpedTo == offset) {
            // To the first line of the body: the call does not start again.
        } else if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::PyStart, wanted(Event::PyStart)))) {
            // The call has not begun: no `try` of its own catches this. The unwinder takes a frame at offset 0 for one that
            // op_py_bind_args has not counted in yet.
            setCurrentOffset(0);
            if (codeBlock->unlinkedCodeBlock()->parseMode() != SourceParseMode::GeneratorBodyMode)
                m_runtime.leaveFrame();
            return threw;
        }
    }
    if (kinds & bit(Event::PyResume)) {
        if (PyFrame* frame = frameObjectIfAny(callFrame))
            frame->lastLine = monitor.lineAt(offset);
        bool isThrow = isResumedToThrow(callFrame);
        if (isThrow) {
            JSValue exception = callFrame->uncheckedArgument(static_cast<int>(JSGenerator::Argument::Value) - 1);
            if (!generatorOf(callFrame)->throwIsWithDelegate && failed(scope, fire(globalObject, callFrame, monitor, offset, Event::PyThrow, monitor.tools(Event::PyThrow), exception)))
                return threw;
        } else if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::PyResume, wanted(Event::PyResume))))
            return threw;
        if (jumped(elsewhere))
            return elsewhere;
    }
    if (kinds & bit(Event::Line)) {
        if (failed(scope, fireLine(globalObject, callFrame, monitor, *currentPoint(), comesFromSameLine)))
            return threw;
        if (jumped(elsewhere))
            return elsewhere;
        // The trace function may have asked for opcodes: pdb's `stepi`.
        if (auto* point = currentPoint())
            kinds |= point->kinds & bit(Event::Instruction);
        if (kinds == bit(Event::Line) && !isJump(opcode))
            return runIt;
    }
    if (kinds & bit(Event::Instruction)) {
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::Instruction, wanted(Event::Instruction))))
            return threw;
    }

    switch (opcode) {
    case op_py_ret:
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::PyReturn, wanted(Event::PyReturn), callFrame->r(pc->as<OpPyRet>().m_value).jsValue())))
            return threw;
        return runIt;

    case op_py_generator_suspend:
    case op_py_generator_return: {
        unsigned event = opcode == op_py_generator_suspend ? Event::PyYield : Event::PyReturn;
        VirtualRegister value = opcode == op_py_generator_suspend ? pc->as<OpPyGeneratorSuspend>().m_value : pc->as<OpPyGeneratorReturn>().m_value;
        if (isResumedToThrow(callFrame) && generatorOf(callFrame)->throwIsWithDelegate)
            return runIt;
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, event, wanted(event), callFrame->r(value).jsValue())))
            return threw;
        if (jumped(elsewhere))
            return elsewhere;
        return runIt;
    }

    // While calls are told of, `super` is called like anything else, as in CPython.
    case op_py_runtime: {
        uint8_t tools = kinds & bit(Event::Call) ? wanted(Event::Call) : 0;
        if (!tools)
            return runIt;
        auto bytecode = pc->as<OpPyRuntime>();
        auto function = static_cast<RuntimeFunction>(bytecode.m_function);
        auto operand = [&](int index) -> Register& { return callFrame->uncheckedR(VirtualRegister(bytecode.m_argv.offset() - index)); };
        bool isWrittenWithoutArguments = function == RuntimeFunction::LoadZeroSuperAttr || function == RuntimeFunction::LoadZeroSuperMethod;
        JSValue callable = operand(0).jsValue();
        JSValue arguments[] = { operand(1).jsValue(), operand(2).jsValue() };
        JSValue first = isWrittenWithoutArguments ? missing() : arguments[0];
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::Call, tools, callable, first)))
            return threw;
        JSValue made = call(globalObject, callable, CallArgs(arguments, isWrittenWithoutArguments ? 0 : 2));
        tools = wanted(Event::Call);
        if (failed(scope, made)) {
            Exception* exception = scope.exception();
            TRY_CLEAR_EXCEPTION(scope, threw);
            if (!failed(scope, fire(globalObject, callFrame, monitor, offset, Event::CRaise, tools, callable, first)))
                throwException(globalObject, scope, exception);
            return threw;
        }
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::CReturn, tools, callable, first)))
            return threw;
        JSValue attribute = getAttr(globalObject, made, asString(operand(3).jsValue()));
        RETURN_IF_EXCEPTION(scope, threw);
        if (function == RuntimeFunction::LoadSuperMethod || function == RuntimeFunction::LoadZeroSuperMethod)
            operand(2) = JSValue();
        callFrame->uncheckedR(bytecode.m_dst) = attribute;
        return { next(), goOn };
    }

    // While calls are told of, a class is called like anything else.
    case op_py_prepare_construct: {
        if (!monitor.tools(Event::Call))
            return runIt;
        auto bytecode = pc->as<OpPyPrepareConstruct>();
        callFrame->uncheckedR(bytecode.m_self) = JSValue();
        callFrame->uncheckedR(bytecode.m_dst) = JSValue();
        return { next(), goOn };
    }

    case op_call:
    case op_py_call_spread: {
        if (kinds & bit(Event::Instruction) && isLastOfStatement && wanted(Event::Instruction))
            frameObjectOf(globalObject, callFrame)->callWithResultToTake = offset;
        uint8_t tools = wanted(Event::Call);
        if (!tools)
            return runIt;
        JSValue callee;
        JSValue thisValue;
        VirtualRegister dst;
        MarkedArgumentBuffer spread;
        const JSValue* arguments;
        unsigned count;
        if (opcode == op_call) {
            auto bytecode = pc->as<OpCall>();
            CallFrame* calleeFrame = callFrame - bytecode.m_argv;
            callee = callFrame->r(bytecode.m_callee).jsValue();
            thisValue = calleeFrame->thisValue();
            arguments = reinterpret_cast<const JSValue*>(calleeFrame->addressOfArgumentsStart());
            count = bytecode.m_argc - 1;
            dst = bytecode.m_dst;
        } else {
            // What the instruction does first is done here, and left in its registers: it finds it done.
            auto bytecode = pc->as<OpPyCallSpread>();
            callee = callFrame->r(bytecode.m_callee).jsValue();
            uint64_t total = prepareSpreadCall(globalObject, callFrame, bytecode, CodeRunsBeforeCall);
            RETURN_IF_EXCEPTION(scope, threw);
            // The instruction refuses it.
            if (total > maxArguments) [[unlikely]]
                return runIt;
            count = total;
            spread.fill(vm, count, [&](JSValue* values) {
                thisValue = fillSpreadCall(callFrame, bytecode, values);
            });
            if (spread.hasOverflowed()) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return threw;
            }
            arguments = reinterpret_cast<const JSValue*>(spread.data());
            dst = bytecode.m_dst;
        }
        PyTuple* keywordNames = thisValue.isCell() && thisValue.asCell()->type() == PyKwNamesType ? uncheckedDowncast<PyTuple>(thisValue.asCell()) : nullptr;
        JSValue callable = callee;
        JSValue first = count > (opcode == op_py_call_spread && keywordNames ? keywordNames->length() : 0) ? arguments[0] : missing();
        // f(*args) is told of as it stands, as in CPython.
        if (auto* method = opcode == op_call ? PyMethod::tryFrom(callee) : nullptr) {
            callable = method->function();
            first = method->self();
        }
        // It may keep what it is shown. (A profile function is only shown the function.)
        if (tools & ~(1 << profileTool))
            m_runtime.forgetNewGenerators();
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::Call, tools, callable, first)))
            return threw;
        // What is not a function written in Python is also told of when it is over. So it is called from here.
        if (PyFunction::is(callable) || PyMethod::is(callable))
            return runIt;
        tools = wanted(Event::Call);
        uint8_t listening = tools & 1 << profileTool;
        for (unsigned tool = 0; tool < numberOfFreeMonitoringTools; ++tool) {
            if (tools >> tool & 1 && (callback(tool, Event::CReturn) || callback(tool, Event::CRaise)))
                listening |= 1 << tool;
        }
        if (!listening)
            return runIt;
        // next(g()): the arguments are dropped, and the generator is closed, after the tools have been told that the function is done.
        JSCell* generatorArgument = nullptr;
        m_runtime.leaveGeneratorArgumentIn(generatorArgument, callFrame, offset);
        auto dropArguments = makeScopeExit([&] {
            if (!generatorArgument)
                return;
            ExceptionSetAside suspended(globalObject);
            m_runtime.releaseIteratorOfLoop(generatorArgument);
        });
        JSValue result = call(globalObject, callee, CallArgs(arguments, count - (keywordNames ? keywordNames->length() : 0), keywordNames));
        m_runtime.didCallWithGeneratorArgument();
        // It may have been sys.monitoring.set_events().
        listening &= wanted(Event::Call);
        if (failed(scope, result)) {
            Exception* exception = scope.exception();
            TRY_CLEAR_EXCEPTION(scope, threw);
            if (!failed(scope, fire(globalObject, callFrame, monitor, offset, Event::CRaise, listening, callable, first)))
                throwException(globalObject, scope, exception);
            return threw;
        }
        // The result goes to its register last. To the collector the frame is still at the call, and what a call sets is dead until
        // it is over (liveLocalsOfFrame()): a collection while a tool runs would empty the register.
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::CReturn, listening, callable, first)))
            return threw;
        dropArguments.release();
        if (generatorArgument)
            m_runtime.releaseIteratorOfLoop(generatorArgument);
        callFrame->uncheckedR(dst) = result;
        return { next(), goOn };
    }

    // asyncForGoesOnMark, asyncForEndsMark
    case op_nop: {
        if (!(kinds & (bit(Event::BranchLeft) | bit(Event::BranchRight))))
            return runIt;
        unsigned event = kinds & bit(Event::BranchLeft) ? Event::BranchLeft : Event::BranchRight;
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, event, wanted(event), jsNumber(shownOffset(shownTarget)), { }, event == Event::BranchRight ? source : offset)))
            return threw;
        return runIt;
    }

    case op_jmp:
    case op_py_jmp_back: {
        // A jump to a jump is told of as one jump: CPython's compiler makes the first go where the second goes. Not if the second has a line to
        // itself, a `break` or a `continue`.
        auto jumpItGoesOnWith = [&](unsigned where) {
            auto* other = monitor.pointAt(where);
            return other && isJump(static_cast<OpcodeID>(other->opcode)) && other->kinds & bit(Event::Jump) && shownTarget != where ? other : nullptr;
        };
        if (uint8_t tools = !jumpIsToldOfAlready && kinds & bit(Event::Jump) ? wanted(Event::Jump) : 0) {
            if (failed(scope, fire(globalObject, callFrame, monitor, offset, Event::Jump, tools, jsNumber(shownOffset(shownTarget)))))
                return threw;
            if (jumped(elsewhere))
                return elsewhere;
        }
        if (!tellOfLineAt(target, elsewhere))
            return elsewhere;
        if (auto* other = kinds & bit(Event::Jump) ? jumpItGoesOnWith(target) : nullptr; other && other->isPatched)
            m_jumpGoesOn = { callFrame, target };
        if (auto* point = currentPoint(); point && point->leadsWhereBranchJumps && monitor.pointAt(target)->isPatched)
            m_arrivesByJump = { callFrame, target };
        return runIt;
    }

    default:
        break;
    }

    // `yield from` or `await` is over. If it was a generator, that has returned without raising StopIteration, and is told of as if it had.
    if (kinds & bit(Event::StopIteration)) {
        uint8_t tools = wanted(Event::StopIteration);
        if (!tools)
            return runIt;
        auto bytecode = pc->as<OpPySend>();
        bool throws = isSendThatThrows(callFrame, pc);
        const void* target;
        const void* signal;
        decodeResult(slow_path_py_send(callFrame, pc), target, signal);
        if (failed(scope, signal != LLInt::exceptionSignal()))
            return threw;
        setCurrentOffset(offset);
        auto* destination = static_cast<const JSInstruction*>(target);
        // (delegateReturnedWhenThrownInto() has told of that.)
        if (destination == next() || throws || !PyGenerator::is(callFrame->uncheckedR(bytecode.m_iterator).jsValue()))
            return { destination, goOn };
        JSValue value = callFrame->uncheckedR(bytecode.m_dst).jsValue();
        PyException* exception;
        if (auto* tuple = PyTuple::tryFrom(value))
            exception = newException(globalObject, m_runtime.StopIteration(), tuple->span());
        else if (isNone(value))
            exception = newException(globalObject, m_runtime.StopIteration(), std::span<const JSValue> { });
        else
            exception = newException(globalObject, m_runtime.StopIteration(), std::span<const JSValue> { &value, 1 });
        if (failed(scope, exception) || failed(scope, fire(globalObject, callFrame, monitor, offset, Event::StopIteration, tools, exception)))
            return threw;
        callFrame->uncheckedR(bytecode.m_dst) = value; // Again: see op_call above.
        return { destination, goOn };
    }

    // Which way a branch goes is known once it has gone.
    currentPoint();
    bool saysSomethingOfTarget = point && point->lineStartOfTarget != CodeMonitor::noLineStart;
    auto wantedHere = [&](unsigned event) -> uint8_t {
        auto* point = currentPoint();
        return point && !isToldOfElsewhere(*point, event) ? wanted(event) : 0;
    };
    bool isWanted = saysSomethingOfTarget || wantedHere(Event::BranchLeft) | wantedHere(Event::BranchRight);
    // The iterators written in C++ end without raising anything.
    if (!isWanted && kinds & CodeMonitor::Point::forLoop && monitor.tools(Event::Raise)) {
        JSValue iterator = callFrame->uncheckedR(pc->as<OpPyForIter>().m_iterator).jsValue();
        isWanted = PyGenerator::is(iterator) || m_runtime.typeOf(iterator)->isHeapType();
    }
    if (kinds & bit(Event::BranchLeft) && isWanted) {
        const JSInstruction* destination;
        JSValue item;
        if (kinds & CodeMonitor::Point::forLoop) {
            auto bytecode = pc->as<OpPyForIter>();
            JSValue stopIteration;
            item = nextOfLoop(globalObject, callFrame->uncheckedR(bytecode.m_iterator).jsValue(), stopIteration);
            RETURN_IF_EXCEPTION(scope, threw);
            destination = item ? next() : at(target);
            if (stopIteration && failed(scope, fire(globalObject, callFrame, monitor, offset, Event::Raise, monitor.tools(Event::Raise), stopIteration)))
                return threw;
        } else if (opcode == op_jundefined_or_null)
            destination = callFrame->r(pc->as<OpJundefinedOrNull>().m_value).jsValue().isUndefinedOrNull() ? at(target) : next();
        else if (opcode == op_jnundefined_or_null)
            destination = callFrame->r(pc->as<OpJnundefinedOrNull>().m_value).jsValue().isUndefinedOrNull() ? next() : at(target);
        else {
            const void* result;
            const void* signal;
            decodeResult(slowPathOfBranch(opcode)(callFrame, pc), result, signal);
            if (failed(scope, signal != LLInt::exceptionSignal()))
                return threw;
            setCurrentOffset(offset);
            destination = static_cast<const JSInstruction*>(result);
        }
        unsigned event = destination == next() ? Event::BranchLeft : Event::BranchRight;
        unsigned shownDestination = event == Event::BranchLeft ? offsetOfNext : shownTarget;
        if (event == Event::BranchLeft && kinds & CodeMonitor::Point::goesOnInItself)
            shownDestination = whereItGoesOnInItself(shownDestination);
        if (failed(scope, fire(globalObject, callFrame, monitor, offset, event, wantedHere(event), jsNumber(shownOffset(shownDestination)))))
            return threw;
        if (event == Event::BranchRight) {
            if (!tellOfLineAt(target, elsewhere))
                return elsewhere;
            // So that jump is not told of.
            if (auto* other = shownTarget != target ? monitor.pointAt(target) : nullptr; other && other->isPatched && isJump(static_cast<OpcodeID>(other->opcode)))
                m_jumpGoesOn = { callFrame, target };
        }
        if (item)
            callFrame->uncheckedR(pc->as<OpPyForIter>().m_dst) = item; // Last: see op_call above.
        return { destination, goOn };
    }
    return runIt;
}

bool Monitoring::exceptionEvent(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned offset, unsigned event, JSValue exception)
{
    if (m_runtime.tracingDepth)
        return true;
    CodeMonitor* monitor = codeInfoOf(callFrame).monitor.get();
    uint8_t tools = m_globalTools[event] | (monitor ? monitor->localTools(event) : 0);
    if (!tools)
        return true;
    if (!monitor)
        monitor = monitorFor(callFrame->codeBlock());
    if (!monitor)
        return true;
    exceptionWillBeSeen(exception);
    return fire(globalObject, callFrame, *monitor, offset, event, tools, exception);
}

static bool leavesHandler(CodeBlock* codeBlock, BytecodeIndex index)
{
    auto instruction = codeBlock->instructionAt(index);
    return instruction->opcodeID() == op_py_runtime && static_cast<RuntimeFunction>(instruction->as<OpPyRuntime>().m_function) == RuntimeFunction::ReraiseOutOfHandler;
}

// Where the handlers begin: a place of no line, as the handler around a handler is.
static unsigned whereHandlersBegin(CodeBlock* codeBlock)
{
    unsigned result = std::numeric_limits<unsigned>::max();
    for (unsigned i = 0; i < codeBlock->numberOfExceptionHandlers(); ++i)
        result = std::min<unsigned>(result, codeBlock->exceptionHandler(i).target);
    return result;
}

// In CPython these are three steps: the exception is raised again; a handler around the handler catches it, and puts back what was being
// handled; that raises it once more. Tools hear of them all, and of the last from the unwinder. That handler is of no line.
JSValue Monitoring::exceptionLeavesHandler(JSGlobalObject* globalObject, CallFrame* callFrame, JSValue exception)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_runtime.vm());
    for (unsigned event : { Event::Reraise, Event::ExceptionHandled }) {
        unsigned offset = event == Event::Reraise ? callFrame->bytecodeIndex().offset() : whereHandlersBegin(callFrame->codeBlock());
        if (failed(scope, exceptionEvent(globalObject, callFrame, offset, event, exception))) {
            if (m_runtime.vm().isTerminationException(scope.exception()))
                return { };
            exception = scope.exception()->value();
            scope.clearException();
        }
    }
    return exception;
}

bool Monitoring::lineOfSendIsToldOfAgain(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned offset)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    CodeMonitor* monitor = codeInfoOf(callFrame).monitor.get();
    if (m_runtime.tracingDepth || !monitor || !monitor->isAwake())
        return true;
    auto* point = monitor->pointAt(offset);
    int line = monitor->lineAt(offset);
    if (!point || line < 0)
        return true;
    // The handler is a place of its own, which a tool can disable.
    uint8_t tools = monitor->tools(Event::Line) & ~point->disabledTools[Event::Line];
    if (!tools)
        return true;
    Shown shown { line, static_cast<int>(offset) };
    if (tools >> traceTool & 1) {
        PyFrame* frame = frameObjectOf(globalObject, callFrame);
        frame->lastLine = line;
        if (frame->trace() && failed(scope, traceLine(globalObject, frame, shown)))
            return false;
    }
    JSValue arguments[2] = { codeOf(globalObject, callFrame), jsNumber(line) };
    for (unsigned tool = numberOfFreeMonitoringTools; tool--;) {
        if (!(tools >> tool & 1))
            continue;
        int result = callTool(globalObject, callFrame, tool, Event::Line, shown, arguments);
        if (failed(scope, result >= 0))
            return false;
        if (!result)
            continue;
        if ((point = monitor->pointAt(offset))) {
            point->disabledTools[Event::Line] |= 1 << tool;
            monitor->m_hasSomethingDisabled = true;
        }
    }
    return true;
}

void Monitoring::delegateReturnedWhenThrownInto(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned offset, JSValue result)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    CodeMonitor* monitor = codeInfoOf(callFrame).monitor.get();
    if (m_runtime.tracingDepth || !monitor || !monitor->isAwake())
        return;
    PyException* exception = isNone(result) ? newException(globalObject, m_runtime.StopIteration(), std::span<const JSValue> { }) : newException(globalObject, m_runtime.StopIteration(), std::span<const JSValue> { &result, 1 });
    if (failed(scope, exception))
        return;
    for (unsigned event : { Event::PyThrow, Event::Raise, Event::ExceptionHandled }) {
        if (failed(scope, exceptionEvent(globalObject, callFrame, offset, event, exception)))
            return;
    }
    scope.release();
    lineOfSendIsToldOfAgain(globalObject, callFrame, offset);
}

// `thrown` is being thrown, and the unwinder is at the frame. `tell` is given the exception. False: it has raised another.
template<typename Function>
static JSValue tellWhileUnwinding(JSGlobalObject* globalObject, JSValue thrown, CallFrame* callFrame, const Function& tell)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* pending = scope.exception();
    if (!pending)
        return thrown;
    scope.clearException();
    // The frames it has left are still there.
    SetForScope topCallFrame(vm.topCallFrame, callFrame);
    rt.framesToldOfException.append(callFrame);
    auto isTold = makeScopeExit([&] {
        rt.framesToldOfException.removeLast(callFrame);
    });
    {
        // The unwinder defers terminations. What is told is code of the program, as is what says what JS has thrown, and it has to
        // be possible to end it. Then the exception goes on as if that had returned, and the termination is thrown at the next
        // safe point.
        TerminationNotDeferred canBeEnded(globalObject);
        JSValue exception = exceptionFromThrownValue(globalObject, thrown);
        if (!failed(scope, exception))
            tell(exception);
    }
    if (scope.exception())
        return scope.exception()->value();
    {
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        throwException(globalObject, throwScope, pending);
    }
    EXCEPTION_ASSERT(scope.exception());
    return thrown;
}

static JSValue tellOfException(JSGlobalObject* globalObject, JSValue thrown, CallFrame* callFrame, unsigned offset, unsigned event)
{
    return tellWhileUnwinding(globalObject, thrown, callFrame, [&](JSValue exception) {
        return runtime(globalObject).monitoring().exceptionEvent(globalObject, callFrame, offset, event, exception);
    });
}

JSValue exceptionIsRaised(JSGlobalObject* globalObject, JSValue exception, CallFrame* callFrame, BytecodeIndex index, bool isRaisedAgain)
{
    Monitoring& monitoring = runtime(globalObject).monitoring();
    bool isWanted = monitoring.wantsExceptions();
    CodeMonitor* monitor = codeInfoOf(callFrame).monitor.get();
    bool isAwake = monitor && monitor->isAwake();
    CodeBlock* codeBlock = callFrame->codeBlock();
    // What raises it again on behalf of the program, from no line, does so as if from here: see PyFrame::findPlace().
    PyFrame* frame = isWanted ? PyFrame::ofForTracing(globalObject, callFrame) : frameObjectIfAny(callFrame);
    // That of a generator may be from before it was last resumed.
    if (!frame && codeBlock->unlinkedCodeBlock()->parseMode() == SourceParseMode::GeneratorBodyMode)
        frame = generatorOf(callFrame)->frameObject();
    if (frame) {
        // (What is left over of an `except*` is raised from no line, and that is where it comes from.)
        if (!isRaisedAgain || lineOfMarks(codeBlock->unlinkedCodeBlock()->pythonMarks(), index.offset()) >= 0 || !raisesAgainAsIfFromWhereItWasRaised(codeBlock, index))
            frame->raisedAt = index.offset();
    }
    // Only whoever is told of lines asks for the line.
    if (!isAwake) [[likely]] {
        monitoring.noteWhereItIsRaised(nullptr, 0);
        if (!isWanted) [[likely]]
            return exception;
    }
    auto* instruction = codeBlock->instructionAt(index);
    if (isWanted && isRaisedAgain && instruction->opcodeID() == op_py_runtime && static_cast<RuntimeFunction>(instruction->as<OpPyRuntime>().m_function) == RuntimeFunction::ReraiseUnseen)
        isWanted = false;
    auto tell = [&] {
        if (!isRaisedAgain)
            return tellOfException(globalObject, exception, callFrame, index.offset(), Event::Raise);
        // (By the handler around the handler: see Monitoring::exceptionLeavesHandler().)
        return tellOfException(globalObject, exception, callFrame, leavesHandler(codeBlock, index) ? whereHandlersBegin(codeBlock) : index.offset(), Event::Reraise);
    };
    if (!isAwake) {
        if (!isWanted)
            return exception;
        return tell();
    }
    // As above: in `with a, b, c():` the handler of `a` is on the line that the exception comes from, though it comes by way of the handler of `b`.
    if (!isRaisedAgain || !monitoring.isRaisedIn(callFrame) || lineItIsRaisedOn(codeBlock, index.offset()) >= 0)
        monitoring.noteWhereItIsRaised(callFrame, index.offset());
    if (isRaisedAgain || !isSendThatThrows(callFrame, instruction)) [[likely]] {
        if (!isWanted) [[likely]]
            return exception;
        return tell();
    }
    // See Monitoring::lineOfSendIsToldOfAgain().
    for (unsigned event : { Event::PyThrow, Event::Raise, Event::ExceptionHandled })
        exception = tellOfException(globalObject, exception, callFrame, index.offset(), event);
    exception = tellWhileUnwinding(globalObject, exception, callFrame, [&](JSValue) {
        return runtime(globalObject).monitoring().lineOfSendIsToldOfAgain(globalObject, callFrame, index.offset());
    });
    return tellOfException(globalObject, exception, callFrame, index.offset(), Event::Reraise);
}

JSValue exceptionStaysOrLeaves(JSGlobalObject* globalObject, JSValue exception, CallFrame* callFrame, BytecodeIndex index, bool isCaught)
{
    // First what CPython does by popping the stack of the frame.
    runtime(globalObject).releaseIteratorsOfLoopsLeft(callFrame, index.offset(), isCaught);
    if (!runtime(globalObject).monitoring().wantsExceptions()) [[likely]]
        return exception;
    CodeBlock* codeBlock = callFrame->codeBlock();
    // The call never began: its arguments do not fit, or there was no room on the stack for its frame.
    if (codeBlock->instructionAt(index)->opcodeID() == op_py_bind_args || callFrame->isZombieFrame())
        return exception;
    unsigned offset = index.offset();
    if (isCaught) {
        if (auto* handler = codeBlock->handlerForBytecodeIndex(index)) {
            if (handler->type() == HandlerType::SynthesizedCatch)
                return exception;
            offset = handler->target;
            auto [first, last] = std::ranges::equal_range(codeBlock->unlinkedCodeBlock()->pythonMarks(), offset, { }, &PythonMark::offset);
            if (std::ranges::any_of(first, last, [](auto& mark) { return mark.kind() == PythonMark::NoLine && mark.value() == handlerOfLineMark; }))
                offset = whereHandlerBegins(codeBlock, offset);
        }
    } else if (leavesHandler(codeBlock, index))
        offset = whereHandlersBegin(codeBlock);
    // In CPython all of a generator is in a handler, which makes a RuntimeError of a StopIteration and raises anything else again.
    if (!isCaught && codeInfoOf(callFrame).isGeneratorLike() && isPastStart(codeBlock->unlinkedCodeBlock()->pythonMarks(), index.offset())) {
        // It is of no line: the compiler has left a place for it right before the code of the program.
        auto marks = codeBlock->unlinkedCodeBlock()->pythonMarks();
        auto start = std::ranges::find(marks, PythonMark::Start, &PythonMark::kind);
        ASSERT(start != marks.begin() && start != marks.end() && (start - 1)->kind() == PythonMark::NoLine);
        offset = (start - 1)->offset;
        exception = tellOfException(globalObject, exception, callFrame, offset, Event::ExceptionHandled);
        exception = tellOfException(globalObject, exception, callFrame, offset, Event::Reraise);
    }
    return tellOfException(globalObject, exception, callFrame, offset, isCaught ? Event::ExceptionHandled : Event::PyUnwind);
}

void didCreateCodeBlock(CodeBlock* codeBlock)
{
    // The collector and the unwinder ask for the CodeInfo of code they find in frames, and cannot take it out of the bytecode cache.
    if (auto* executable = dynamicDowncast<FunctionExecutable>(codeBlock->ownerExecutable()))
        executable->unlinkedExecutable()->pythonCodeInfo();
    if (Runtime* rt = codeBlock->globalObject()->pythonRuntime())
        rt->monitoring().didCreateCodeBlock(codeBlock);
}

// ---- frame.f_lineno = line ----

namespace {

struct OpenBlock {
    unsigned number;
    TracedBlock::Kind kind;
    VirtualRegister reg;
};
using OpenBlocks = Vector<OpenBlock, 8>;

// A frame in `from` can go on in `to`: it leaves blocks, and what the others hold will do for those it finds itself in.
bool canGoOnIn(const OpenBlocks& from, const OpenBlocks& to)
{
    if (to.size() > from.size())
        return false;
    for (unsigned i = 0; i < to.size(); ++i) {
        if (from[i].number == to[i].number)
            continue;
        if (to[i].kind == TracedBlock::ExceptionHandlerWithMore || to[i].kind == TracedBlock::Other)
            return false;
        if (from[i].kind != to[i].kind && (from[i].kind != TracedBlock::ExceptionHandlerWithMore || to[i].kind != TracedBlock::ExceptionHandler))
            return false;
    }
    return true;
}

} // namespace

void Monitoring::jump(JSGlobalObject* globalObject, PyFrame* frame, int requestedLine)
{
    VM& vm = m_runtime.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (m_runtime.tracedEvent) {
    case Event::PyResume:
    case Event::Jump:
    case Event::Branch:
    case Event::BranchLeft:
    case Event::BranchRight:
    case Event::Line:
    case Event::PyYield:
        break;
    case Event::PyStart:
        raiseValueError(globalObject, scope, "can't jump from the 'call' trace event of a new frame"_s);
        return;
    case Event::Call:
    case Event::CReturn:
        raiseValueError(globalObject, scope, "can't jump during a call"_s);
        return;
    case Event::PyReturn:
    case Event::PyUnwind:
    case Event::PyThrow:
    case Event::Raise:
    case Event::CRaise:
    case Event::Instruction:
    case Event::ExceptionHandled:
        raiseValueError(globalObject, scope, "can only jump from a 'line' trace event"_s);
        return;
    default:
        if (m_runtime.tracedEvent < 0)
            raiseValueError(globalObject, scope, "f_lineno can only be set in a trace function"_s);
        else
            raise(globalObject, scope, m_runtime.SystemError(), "unexpected event type"_s);
        return;
    }
    // Any other frame is in the middle of a call.
    CallFrame* callFrame = frame->callFrame();
    if (!callFrame || callFrame != m_runtime.tracedCallFrame) {
        raiseValueError(globalObject, scope, "can only jump in the frame that is being traced"_s);
        return;
    }

    const CodeInfo& info = frame->codeInfo();
    if (requestedLine < info.firstLineNumber) {
        raiseValueError(globalObject, scope, "line "_s, requestedLine, " comes before the current code block"_s);
        return;
    }
    auto marks = callFrame->codeBlock()->unlinkedCodeBlock()->pythonMarks();
    int line = requestedLine == info.firstLineNumber ? requestedLine : std::numeric_limits<int>::max();
    for (auto& mark : marks) {
        if (mark.kind() == PythonMark::Line && lineOfMarkValue(mark.value()) >= requestedLine)
            line = std::min(line, lineOfMarkValue(mark.value()));
    }
    if (line == std::numeric_limits<int>::max()) {
        raiseValueError(globalObject, scope, "line "_s, requestedLine, " comes after the current code block"_s);
        return;
    }

    // Where the frame is: at the mark for the line it is being told of, or else past all those of the instruction.
    unsigned offset = callFrame->bytecodeIndex().offset();
    OpenBlocks open;
    std::optional<OpenBlocks> from;
    struct Target {
        unsigned offset;
        unsigned visibleOffset;
        OpenBlocks blocks;
        bool isStart { false };
    };
    unsigned linesAtOffset = 0;
    unsigned lastOffset = std::numeric_limits<unsigned>::max();
    Vector<Target, 2> targets;
    bool isInExpression = false;
    for (auto& mark : marks) {
        if (!from && mark.offset > offset)
            from = open;
        if (mark.offset != std::exchange(lastOffset, mark.offset))
            linesAtOffset = 0;
        switch (mark.kind()) {
        case PythonMark::Start:
            if (line == info.firstLineNumber)
                targets.append({ mark.offset, 0, { }, true });
            break;
        case PythonMark::BlockBegin:
            open.append({ mark.value(), TracedBlock::kindOf(mark.bits), VirtualRegister(TracedBlock::registerOffsetOf(mark.bits)) });
            break;
        case PythonMark::BlockEnd:
            open.removeLast();
            break;
        case PythonMark::Line:
            if (!from && mark.offset == offset && lineOfMarkValue(mark.value()) == m_runtime.tracedLine)
                from = open;
            ++linesAtOffset;
            if (lineOfMarkValue(mark.value()) != line)
                break;
            if (isStatementStart(mark.value()))
                targets.append({ mark.offset, mark.offset + linesAtOffset - 1, open });
            else
                isInExpression = true;
            break;
        default:
            break;
        }
    }
    if (!from)
        from = open;

    const Target* best = nullptr;
    for (auto& target : targets) {
        if (canGoOnIn(*from, target.blocks) && (!best || target.blocks.size() > best->blocks.size()))
            best = &target;
    }
    if (!best) {
        ASCIILiteral message = "incompatible stacks"_s;
        if (!targets.isEmpty() && !targets[0].blocks.isEmpty()) {
            switch (targets[0].blocks.last().kind) {
            case TracedBlock::ForLoop:
                message = "can't jump into the body of a for loop"_s;
                break;
            case TracedBlock::ExceptionHandler:
            case TracedBlock::ExceptionHandlerWithMore:
                message = "can't jump into an 'except' block as there's no exception"_s;
                break;
            case TracedBlock::With:
            case TracedBlock::Other:
                break;
            }
        }
        UNUSED_VARIABLE(isInExpression);
        raiseValueError(globalObject, scope, message);
        return;
    }

    // The code there may take it that a variable has a value which has none.
    unsigned unbound = 0;
    for (auto& variable : info.variables)
        unbound += variable.kind == CodeInfo::VariableKind::Local && !callFrame->r(variable.reg).jsValue();
    if (unbound) {
        bool ok = warn(globalObject, m_runtime.RuntimeWarning(), makeString("assigning None to "_s, unbound, " unbound local"_s, unbound == 1 ? ""_s : "s"_s));
        RETURN_IF_EXCEPTION(scope, void());
        if (!ok)
            return;
        for (auto& variable : info.variables) {
            if (variable.kind == CodeInfo::VariableKind::Local && !callFrame->r(variable.reg).jsValue())
                callFrame->r(variable.reg) = jsUndefined();
        }
    }

    for (unsigned i = from->size(); i-- > best->blocks.size();) {
        auto& block = from->at(i);
        if (block.kind == TracedBlock::ExceptionHandler || block.kind == TracedBlock::ExceptionHandlerWithMore)
            m_runtime.handlerIsLeft(callFrame, callFrame->r(block.reg).jsValue());
    }
    // (A `with` holds two registers, other blocks one.)
    auto next = [](VirtualRegister reg) { return VirtualRegister(reg.offset() - 1); };
    Vector<std::pair<JSValue, JSValue>, 8> held;
    for (unsigned i = 0; i < best->blocks.size(); ++i) {
        auto& block = from->at(i);
        held.append({ block.reg.isValid() ? callFrame->r(block.reg).jsValue() : JSValue(), block.kind == TracedBlock::With ? callFrame->r(next(block.reg)).jsValue() : JSValue() });
    }
    for (unsigned i = 0; i < best->blocks.size(); ++i) {
        auto& block = best->blocks[i];
        if (!block.reg.isValid() || !from->at(i).reg.isValid())
            continue;
        callFrame->r(block.reg) = held[i].first;
        if (block.kind == TracedBlock::With)
            callFrame->r(next(block.reg)) = held[i].second;
    }

    frame->jumpTarget = best->offset;
    frame->jumpsToStart = best->isStart;
    frame->lastLine = line;
    m_runtime.tracedLine = frame->shownLine = line;
    m_runtime.tracedOffset = frame->shownOffset = best->visibleOffset;
}

// ---- The module sys.monitoring ----

static bool monitoringTool(JSGlobalObject* globalObject, JSValue value, unsigned& tool)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int number = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (number < 0 || number >= static_cast<int>(numberOfFreeMonitoringTools)) {
        raiseValueError(globalObject, scope, "invalid tool "_s, number, " (must be between 0 and 5)"_s);
        return false;
    }
    tool = number;
    return true;
}

static bool monitoringToolInUse(JSGlobalObject* globalObject, unsigned tool)
{
    if (runtime(globalObject).monitoring().toolName(tool))
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "tool "_s, tool, " is not in use"_s);
    return false;
}

static JSValue monitoringUseToolId(JSGlobalObject* globalObject, JSValue toolValue, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Monitoring& monitoring = runtime(globalObject).monitoring();
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    if (!isStr(name))
        return raiseValueError(globalObject, scope, "tool name must be a str"_s);
    if (monitoring.toolName(tool))
        return raiseValueError(globalObject, scope, "tool "_s, tool, " is already in use"_s);
    monitoring.setToolName(tool, name);
    return jsUndefined();
}

static JSValue monitoringClearToolId(JSGlobalObject* globalObject, JSValue toolValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Monitoring& monitoring = runtime(globalObject).monitoring();
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    if (monitoring.toolName(tool))
        monitoring.clearTool(tool);
    return jsUndefined();
}

static JSValue monitoringFreeToolId(JSGlobalObject* globalObject, JSValue toolValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Monitoring& monitoring = runtime(globalObject).monitoring();
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    if (monitoring.toolName(tool))
        monitoring.clearTool(tool);
    monitoring.setToolName(tool, { });
    return jsUndefined();
}

static JSValue monitoringGetTool(JSGlobalObject* globalObject, JSValue toolValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    JSValue name = runtime(globalObject).monitoring().toolName(tool);
    return name ? name : jsUndefined();
}

static JSValue monitoringRegisterCallback(JSGlobalObject* globalObject, JSValue toolValue, JSValue eventValue, JSValue function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Monitoring& monitoring = runtime(globalObject).monitoring();
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    int event = toCInt(globalObject, eventValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (std::popcount(static_cast<unsigned>(event)) != 1)
        return raiseValueError(globalObject, scope, "The callback can only be set for one event at a time"_s);
    unsigned index = std::countr_zero(static_cast<unsigned>(event));
    if (index >= Event::count)
        return raiseValueError(globalObject, scope, "invalid event "_s, event);
    if (failed(scope, audit(globalObject, "sys.monitoring.register_callback"_s, function)))
        return { };
    JSValue callback = isNone(function) ? JSValue() : function;
    JSValue previous;
    if (index == Event::Branch) {
        // The event of old: either way a branch goes.
        previous = monitoring.setCallback(tool, Event::Branch, callback);
        monitoring.setCallback(tool, Event::BranchLeft, callback);
        monitoring.setCallback(tool, Event::BranchRight, callback);
    } else
        previous = monitoring.setCallback(tool, index, callback);
    return previous ? previous : jsUndefined();
}

static constexpr int cReturnEvents = 1 << Event::CReturn | 1 << Event::CRaise;

// False: an exception.
static bool monitoringEventSet(JSGlobalObject* globalObject, JSValue value, bool isLocal, uint32_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int events = toCInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isLocal && (events < 0 || events >= 1 << Event::count)) {
        raiseValueError(globalObject, scope, "invalid event set 0x"_s, hex(static_cast<unsigned>(events), Lowercase));
        return false;
    }
    if (events & cReturnEvents && (events & (cReturnEvents | 1 << Event::Call)) != (cReturnEvents | 1 << Event::Call)) {
        raiseValueError(globalObject, scope, "cannot set C_RETURN or C_RAISE events independently"_s);
        return false;
    }
    events &= ~cReturnEvents;
    if (events & 1 << Event::Branch)
        events = (events & ~(1 << Event::Branch)) | 1 << Event::BranchLeft | 1 << Event::BranchRight;
    if (isLocal && (events < 0 || events >= 1 << Event::ungroupedCount)) {
        raiseValueError(globalObject, scope, "invalid local event set 0x"_s, hex(static_cast<unsigned>(events), Lowercase));
        return false;
    }
    result = events;
    return true;
}

static JSValue monitoringGetEvents(JSGlobalObject* globalObject, JSValue toolValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    RELEASE_AND_RETURN(scope, jsNumber(runtime(globalObject).monitoring().events(tool)));
}

static JSValue monitoringSetEvents(JSGlobalObject* globalObject, JSValue toolValue, JSValue eventSet)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned tool;
    uint32_t events;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)) || failed(scope, monitoringEventSet(globalObject, eventSet, false, events)) || failed(scope, monitoringToolInUse(globalObject, tool)))
        return { };
    runtime(globalObject).monitoring().setEvents(tool, events);
    return jsUndefined();
}

static JSValue monitoringGetLocalEvents(JSGlobalObject* globalObject, JSValue toolValue, JSValue code)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyCode::is(code))
        return raiseTypeError(globalObject, scope, "code must be a code object"_s);
    unsigned tool;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)))
        return { };
    CodeMonitor* monitor = PyCode::from(code)->codeInfo().monitor.get();
    return jsNumber(monitor ? monitor->localEvents(tool) : 0);
}

static JSValue monitoringSetLocalEvents(JSGlobalObject* globalObject, JSValue toolValue, JSValue code, JSValue eventSet)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Monitoring& monitoring = runtime(globalObject).monitoring();
    if (!PyCode::is(code))
        return raiseTypeError(globalObject, scope, "code must be a code object"_s);
    unsigned tool;
    uint32_t events;
    if (failed(scope, monitoringTool(globalObject, toolValue, tool)) || failed(scope, monitoringEventSet(globalObject, eventSet, true, events)) || failed(scope, monitoringToolInUse(globalObject, tool)))
        return { };
    CodeMonitor* monitor = monitoring.monitorFor(globalObject, PyCode::from(code));
    RETURN_IF_EXCEPTION(scope, { });
    if (!monitor)
        return raise(globalObject, scope, runtime(globalObject).SystemError(), makeString("cannot instrument shim code object '"_s, PyCode::from(code)->codeInfo().name.string(), '\''));
    monitoring.setLocalEvents(*monitor, tool, events);
    return jsUndefined();
}

void Monitoring::setLocalEvents(CodeMonitor& monitor, unsigned tool, uint32_t events)
{
    if (monitor.localEvents(tool) == events)
        return;
    monitor.setLocalEvents(tool, events);
    noteWhatIsWanted();
    monitor.sleepAndWake();
}

static JSValue monitoringRestartEvents(JSGlobalObject* globalObject)
{
    runtime(globalObject).monitoring().restartEvents();
    return jsUndefined();
}

// { name of the event: the tools that want it, as bits }
static JSValue monitoringAllEvents(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    Monitoring& monitoring = runtime(globalObject).monitoring();
    PyDict* result = PyDict::create(globalObject);
    for (unsigned event = 0; event < Event::ungroupedCount; ++event) {
        if (uint8_t tools = monitoring.globalTools(event))
            result->set(vm, internString(vm, eventNames[event]), jsNumber(tools));
    }
    return result;
}

// For tests, and to see what is going on: the places in the code where something is told of. For each, the offset and the name of the
// instruction, the events as bits, the lines that begin there, whether the first of them is in doubt, the line that begins
// where it jumps to, and whether the instruction is replaced right now.
static JSValue monitoringPlaces(JSGlobalObject* globalObject, JSValue code)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyCode::is(code))
        return raiseTypeError(globalObject, scope, "code must be a code object"_s);
    RELEASE_AND_RETURN(scope, runtime(globalObject).monitoring().places(globalObject, PyCode::from(code)));
}

JSValue Monitoring::places(JSGlobalObject* globalObject, PyCode* code)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    JSArray* result = newList(globalObject);
    CodeMonitor* monitor = code->codeInfo().monitor.get();
    if (!monitor)
        return result;
    monitor->wake();
    for (auto& point : monitor->m_points) {
        JSArray* lines = newList(globalObject);
        for (auto& start : monitor->lineStartsOf(point)) {
            listAppend(globalObject, lines, jsNumber(start.line));
            RETURN_IF_EXCEPTION(scope, { });
        }
        listAppend(globalObject, result, PyTuple::create(globalObject, { jsNumber(point.offset), makeStr(globalObject, String::fromLatin1(opcodeNames[point.opcode])), jsNumber(point.kinds), lines,
            jsBoolean(point.firstLineIsInDoubt), point.lineStartOfTarget == CodeMonitor::noLineStart ? jsUndefined() : jsNumber(monitor->m_lineStarts[point.lineStartOfTarget].line), jsBoolean(point.isPatched) }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

JSArray* Monitoring::branches(JSGlobalObject* globalObject, PyCode* code)
{
    auto scope = DECLARE_THROW_SCOPE(m_runtime.vm());
    JSArray* result = newList(globalObject);
    CodeMonitor* monitor = monitorFor(globalObject, code);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!monitor)
        return result;
    if (!monitor->m_hasPoints) {
        monitor->findPoints(false);
        monitor->update();
    }
    const JSInstructionStream& instructions = monitor->code()->instructions();
    auto marks = monitor->code()->pythonMarks();
    for (auto& point : monitor->m_points) {
        if (!(point.kinds & bit(Event::BranchRight)))
            continue;
        // asyncForEndsMark
        unsigned source = point.kinds & bit(Event::BranchLeft) ? point.offset : point.source;
        unsigned left = source + instructions.at(source)->size();
        if (point.kinds & CodeMonitor::Point::goesOnInItself)
            left = whereItGoesOnInItself(left);
        listAppend(globalObject, result, PyTuple::create(globalObject, { jsNumber(shownOffset(visibleOffset(marks, source))), jsNumber(shownOffset(left)), jsNumber(shownOffset(point.shownTarget)) }));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return result;
}

static constexpr NativeDef monitoringFunctions[] = {
    PY_FUNCTION("_all_events", monitoringAllEvents, "()"),
    PY_FUNCTION("_places", monitoringPlaces, "(code, /)"),
    PY_FUNCTION("clear_tool_id", monitoringClearToolId, "(tool_id, /)"),
    PY_FUNCTION("free_tool_id", monitoringFreeToolId, "(tool_id, /)"),
    PY_FUNCTION("get_events", monitoringGetEvents, "(tool_id, /)"),
    PY_FUNCTION("get_local_events", monitoringGetLocalEvents, "(tool_id, code, /)"),
    PY_FUNCTION("get_tool", monitoringGetTool, "(tool_id, /)"),
    PY_FUNCTION("register_callback", monitoringRegisterCallback, "(tool_id, event, func, /)"),
    PY_FUNCTION("restart_events", monitoringRestartEvents, "()"),
    PY_FUNCTION("set_events", monitoringSetEvents, "(tool_id, event_set, /)"),
    PY_FUNCTION("set_local_events", monitoringSetLocalEvents, "(tool_id, code, event_set, /)"),
    PY_FUNCTION("use_tool_id", monitoringUseToolId, "(tool_id, name, /)"),
};

PyModule* makeMonitoringModule(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    rt.monitoring().createState();
    RETURN_IF_EXCEPTION(scope, nullptr);
    PyModule* module = PyModule::create(globalObject, "sys.monitoring"_s);
    module->addFunctions(globalObject, monitoringFunctions);
    PyDict* events = PyDict::create(globalObject);
    for (unsigned event = 0; event < Event::count; ++event)
        events->set(vm, internString(vm, eventNames[event]), jsNumber(1 << event));
    events->set(vm, internString(vm, "NO_EVENTS"_s), jsNumber(0));
    module->add(globalObject, "events"_s, makeSimpleNamespace(globalObject, events));
    module->add(globalObject, "DISABLE"_s, rt.monitoring().disable());
    module->add(globalObject, "MISSING"_s, rt.monitoring().missing());
    module->add(globalObject, "DEBUGGER_ID"_s, jsNumber(0));
    module->add(globalObject, "COVERAGE_ID"_s, jsNumber(1));
    module->add(globalObject, "PROFILER_ID"_s, jsNumber(2));
    module->add(globalObject, "OPTIMIZER_ID"_s, jsNumber(5));
    return module;
}

// ---- sys.settrace() and the like ----

JSValue sysSetTrace(JSGlobalObject* globalObject, JSValue function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, audit(globalObject, "sys.settrace"_s)))
        return { };
    runtime(globalObject).monitoring().setTraceFunction(isNone(function) ? JSValue() : function);
    return jsUndefined();
}

JSValue sysGetTrace(JSGlobalObject* globalObject)
{
    JSValue function = runtime(globalObject).slot(Runtime::Slot::TraceFunction);
    return function ? function : jsUndefined();
}

JSValue sysSetProfile(JSGlobalObject* globalObject, JSValue function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, audit(globalObject, "sys.setprofile"_s)))
        return { };
    runtime(globalObject).monitoring().setProfileFunction(isNone(function) ? JSValue() : function);
    return jsUndefined();
}

JSValue sysGetProfile(JSGlobalObject* globalObject)
{
    JSValue function = runtime(globalObject).slot(Runtime::Slot::ProfileFunction);
    return function ? function : jsUndefined();
}

static void setFunctionOfOtherThreads(JSGlobalObject* globalObject, JSValue function, bool isProfile)
{
    Runtime& rt = runtime(globalObject);
    int delta = 0;
    rt.threads().forEachThread([&](ThreadState& thread) {
        if (!thread.saved)
            return;
        for (auto& entry : thread.saved->entries) {
            if (entry.runtime != &rt)
                continue;
            JSValue& slot = isProfile ? entry.state.profileFunction : entry.state.traceFunction;
            delta += !!function - !!slot;
            slot = function;
        }
    });
    rt.monitoring().tracingThreadCountChanged(isProfile ? 0 : delta, isProfile ? delta : 0);
}

JSValue sysSetTraceAllThreads(JSGlobalObject* globalObject, JSValue function)
{
    if (!sysSetTrace(globalObject, function))
        return { };
    setFunctionOfOtherThreads(globalObject, isNone(function) ? JSValue() : function, false);
    return jsUndefined();
}

JSValue sysSetProfileAllThreads(JSGlobalObject* globalObject, JSValue function)
{
    if (!sysSetProfile(globalObject, function))
        return { };
    setFunctionOfOtherThreads(globalObject, isNone(function) ? JSValue() : function, true);
    return jsUndefined();
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
