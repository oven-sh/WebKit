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
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

using namespace Python;

// ---- The opcodes that have been replaced ----

// By the address of the instruction, for all VMs.
struct ReplacedOpcode {
    uint8_t opcode;
    const CodeMonitor* owner;
};
static Lock replacedOpcodesLock;
static UncheckedKeyHashMap<const void*, ReplacedOpcode>& replacedOpcodes() WTF_REQUIRES_LOCK(replacedOpcodesLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<const void*, ReplacedOpcode>> map;
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
    Locker locker { replacedOpcodesLock };
    auto it = replacedOpcodes().find(instruction);
    if (it != replacedOpcodes().end())
        return static_cast<OpcodeID>(it->value.opcode);
    // It has been put back since the caller looked.
    auto opcode = static_cast<OpcodeID>(*addressOfOpcode(instruction));
    RELEASE_ASSERT(opcode != op_py_instrumented);
    return opcode;
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_instrumented)
{
    CodeBlock* codeBlock = callFrame->codeBlock();
    JSGlobalObject* globalObject = codeBlock->globalObject();
    VM& vm = codeBlock->vm();
    SlowPathFrameTracer tracer(vm, callFrame);
    callFrame->setCurrentVPC(pc);
    auto outcome = runtime(globalObject).monitoring().handle(globalObject, callFrame, pc);
    if (!outcome.pc) [[unlikely]]
        return encodeResult(LLInt::returnToThrow(vm), LLInt::exceptionSignal());
    return encodeResult(outcome.pc, std::bit_cast<void*>(static_cast<uintptr_t>(outcome.opcode)));
}

namespace Python {

CodeInfo::CodeInfo() = default;
CodeInfo::~CodeInfo() = default;

static constexpr unsigned goOn = 256; // See Monitoring::Outcome.

static constexpr uint16_t bit(unsigned event) { return static_cast<uint16_t>(1 << event); }

static ALWAYS_INLINE PyFunction* functionOf(CallFrame* callFrame)
{
    return uncheckedDowncast<PyFunction>(callFrame->jsCallee());
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

CodeMonitor::CodeMonitor(Monitoring& monitoring, UnlinkedCodeBlock* code)
    : m_monitoring(&monitoring)
    , m_code(monitoring.runtime().vm(), code)
    , m_instructions(bytesOf(code))
{
    Locker locker { monitoring.m_monitorsLock };
    monitoring.m_monitors.add(this);
}

CodeMonitor::~CodeMonitor()
{
    if (m_monitoring) {
        Locker locker { m_monitoring->m_monitorsLock };
        m_monitoring->m_monitors.remove(this);
    }
    // The instructions go away with the code, as they are. When the VM ends they may be gone already, and another VM may have
    // instructions of its own there.
    Locker locker { replacedOpcodesLock };
    for (auto& point : m_points) {
        if (!point.isPatched)
            continue;
        auto it = replacedOpcodes().find(m_instructions + point.offset);
        if (it != replacedOpcodes().end() && it->value.owner == this)
            replacedOpcodes().remove(it);
    }
}

CodeMonitor::Point* CodeMonitor::pointAt(unsigned offset)
{
    auto it = std::ranges::lower_bound(m_points, offset, { }, &Point::offset);
    return it != m_points.end() && it->offset == offset ? &*it : nullptr;
}

int CodeMonitor::lineAt(unsigned offset) const
{
    return lineOfMarks(m_code->pythonMarks(), offset);
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

void CodeMonitor::setLocalEvents(unsigned tool, uint32_t events)
{
    for (unsigned event = 0; event < Event::ungroupedCount; ++event) {
        if (events >> event & 1)
            m_localTools[event] |= 1 << tool;
        else
            m_localTools[event] &= ~(1 << tool);
    }
}

uint8_t CodeMonitor::tools(unsigned event) const
{
    return m_monitoring->m_globalTools[event] | m_localTools[event];
}

uint8_t* CodeMonitor::opcodeAddress(const Point& point) const
{
    return const_cast<uint8_t*>(addressOfOpcode(m_instructions + point.offset));
}

static bool fallsThrough(OpcodeID opcode)
{
    switch (opcode) {
    case op_jmp:
    case op_ret:
    case op_py_ret:
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
        return true;
    default:
        return false;
    }
}

// A line begins at an instruction that has a mark for it, and at one that is jumped to. Being there is an event if the instruction
// run before it in the frame was of another line. Whether it was is nearly always plain from the code.
void CodeMonitor::putBackInstructions()
{
    for (auto& point : m_points) {
        if (!point.isPatched)
            continue;
        *opcodeAddress(point) = point.opcode;
        WTF::storeStoreFence();
        Locker locker { replacedOpcodesLock };
        replacedOpcodes().remove(m_instructions + point.offset);
        point.isPatched = false;
    }
}

void CodeMonitor::findPoints(bool everyInstruction)
{
    putBackInstructions();
    m_points.clear();
    m_lineStarts.clear();
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
        bool isReachedFromOtherLineOnlyByJumps { true }; // By op_jmp.
        bool isJumpedTo { false }; // By anything but a `for` loop that ends.
        bool isResumedAt { false };
        bool isHiddenJump { false };
        bool fallsThrough { true };
        unsigned sharedLineStart { noLineStart };

        // -2: it depends.
        int lineAfter() const { return isMarked ? exitLine : linesBefore.isEmpty() ? -1 : linesBefore.size() == 1 ? linesBefore[0] : -2; }
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
                item.fallsThrough = function != RuntimeFunction::Raise && function != RuntimeFunction::Reraise && function != RuntimeFunction::ThrowIntoGenerator;
            }
            unsigned lineMarkCount = 0;
            for (; nextMark < marks.size() && marks[nextMark].offset <= item.offset; ++nextMark) {
                auto& mark = marks[nextMark];
                if (mark.kind() == PythonMark::NoLine && mark.value() == hiddenJumpMark) {
                    item.isHiddenJump = true;
                    continue;
                }
                if (mark.kind() != PythonMark::Line && mark.kind() != PythonMark::NoLine && mark.kind() != PythonMark::Start)
                    continue;
                int before = item.exitLine;
                bool isFirst = !std::exchange(item.isMarked, true);
                item.exitLine = mark.kind() == PythonMark::Line ? lineOfMarkValue(mark.value()) : -1;
                if (mark.kind() != PythonMark::Line)
                    continue;
                unsigned offset = item.offset + lineMarkCount++;
                if (isSilentLine(mark.value()) || (!isFirst && item.exitLine == before))
                    continue;
                if (isFirst)
                    item.entryLine = item.exitLine;
                lines.append({ item.exitLine, offset });
            }
            item.lineCount = lines.size() - item.firstLine;
            // BytecodeGenerator::emitOutOfLineCatchHandler()
            if (item.opcode == op_jmp && !items.isEmpty() && items.last().opcode == op_catch)
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
        if (line != item.entryLine && (!byJump || line == -2))
            item.isReachedFromOtherLineOnlyByJumps = false;
    };
    for (unsigned i = 0; i < items.size(); ++i) {
        if (items[i].offset == start) {
            items[i].linesBefore.append(-1);
            items[i].isReachedFromOtherLineOnlyByJumps = false;
        }
        // It goes on with the line it was on.
        if (items[i].isResumedAt)
            items[i].linesBefore.append(lineAt(items[i].offset));
        const Item& item = items[i];
        if (item.fallsThrough)
            reach(i + 1, item, false);
        bool isResumption = isGeneratorBody && item.opcode == op_switch_imm && item.offset < start;
        extractStoredJumpTargetsForInstruction(code, instructions.at(item.offset), [&](int32_t relative) {
            auto it = indexOfOffset.find(item.offset + relative);
            if (it == indexOfOffset.end())
                return;
            if (isResumption) {
                if (items[it->value].offset > start)
                    items[it->value].isResumedAt = true;
                return;
            }
            items[it->value].isJumpedTo |= item.opcode != op_py_for_iter;
            reach(it->value, item, item.opcode == op_jmp && !item.isHiddenJump);
        });
    }
    // In CPython a line begins where the instruction before, as they are laid out, is of another line, or where a jump goes. Where
    // a `for` loop goes when it ends does not count as that. So `x = [i` is not told of again after the lines of `for i in y]`.
    for (unsigned i = 1; i < items.size(); ++i) {
        Item& item = items[i];
        if (item.entryLine >= 0 && !item.isJumpedTo && item.offset > start && lineAt(items[i - 1].offset) == item.entryLine) {
            item.linesBefore.clear();
            item.linesBefore.append(item.entryLine);
        }
    }
    // Being at an instruction is news of the first of its lines if the frame was on another. For some, it may or may not have been.
    // Such is the head of a `for` loop: it is on the line of what comes before it, and is jumped to from the end of the body. If
    // all that comes from other lines is jumps, the jumps tell of the line.
    auto isReachedFromSameLine = [](const Item& item) { return item.linesBefore.contains(item.entryLine); };
    auto isReachedFromOtherLine = [](const Item& item) { return item.linesBefore.isEmpty() || item.linesBefore.size() > 1 || item.linesBefore[0] != item.entryLine; };
    for (auto& item : items) {
        if (item.offset < start || item.entryLine < 0 || !isReachedFromSameLine(item) || !isReachedFromOtherLine(item) || !item.isReachedFromOtherLineOnlyByJumps)
            continue;
        item.sharedLineStart = m_lineStarts.size();
        m_lineStarts.append({ item.entryLine, item.offset, 0, true });
    }

    VirtualRegister generatorRegister = virtualRegisterForArgumentIncludingThis(static_cast<int>(JSGenerator::Argument::Generator));
    bool isAboutToYield = false;
    bool isBeingResumed = false;
    bool isExceptionMatch = false;
    for (auto& item : items) {
        isBeingResumed |= item.isResumedAt;
        if (item.offset < start)
            continue;

        Point point;
        point.offset = item.offset;
        point.opcode = static_cast<uint8_t>(item.opcode);
        if (item.offset == start)
            point.kinds |= bit(Event::PyStart);
        if (everyInstruction)
            point.kinds |= bit(Event::Instruction);

        // The registers are put back first.
        bool isResumption = isBeingResumed && item.opcode != op_get_from_scope;
        if (isResumption) {
            point.kinds |= bit(Event::PyResume);
            isBeingResumed = false;
        }

        bool wasAboutToYield = std::exchange(isAboutToYield, false);
        bool previousIsExceptionMatch = std::exchange(isExceptionMatch, item.opcode == op_py_compare && static_cast<CompareOperator>(instructions.at(item.offset)->as<OpPyCompare>().m_operation) == CompareOperator::ExceptionMatch);
        switch (item.opcode) {
        case op_put_internal_field: {
            auto bytecode = instructions.at(item.offset)->as<OpPutInternalField>();
            isAboutToYield = isGeneratorBody && bytecode.m_base == generatorRegister && bytecode.m_index == generatorStateField;
            break;
        }
        case op_put_to_scope:
            isAboutToYield = wasAboutToYield;
            break;
        case op_ret:
            point.kinds |= bit(wasAboutToYield ? Event::PyYield : Event::PyReturn);
            break;
        case op_py_ret:
            point.kinds |= bit(Event::PyReturn);
            break;
        case op_call:
        case op_call_varargs:
            point.kinds |= bit(Event::Call);
            break;
        case op_py_prepare_construct:
            point.kinds |= Point::construct;
            break;
        case op_py_runtime:
            if (static_cast<RuntimeFunction>(instructions.at(item.offset)->as<OpPyRuntime>().m_function) == RuntimeFunction::TakeSendResult)
                point.kinds |= bit(Event::StopIteration);
            break;
        case op_jmp: {
            if (item.isHiddenJump)
                break;
            point.kinds |= bit(Event::Jump);
            auto it = indexOfOffset.find(item.offset + jumpTargetForInstruction<OpJmp>(code, instructions.at(item.offset)));
            if (it != indexOfOffset.end() && items[it->value].entryLine != item.lineAfter())
                point.lineStartOfTarget = items[it->value].sharedLineStart;
            break;
        }
        default:
            // Whether it was resumed to go on, or to raise something, is not a branch of the program's.
            if (isPythonBranch(item.opcode) && !isResumption) {
                point.kinds |= bit(Event::BranchLeft) | bit(Event::BranchRight);
                if (item.opcode == op_py_for_iter)
                    point.kinds |= Point::forLoop | Point::goesOnInItself;
                else if (previousIsExceptionMatch)
                    point.kinds |= Point::goesOnInItself;
            }
            break;
        }

        // While the registers are being put back nothing is told of.
        if (!isBeingResumed && !item.isResumedAt && item.opcode != op_catch) {
            point.firstLineStart = m_lineStarts.size();
            if (item.entryLine >= 0 && item.sharedLineStart == noLineStart && isReachedFromOtherLine(item)) {
                m_lineStarts.append({ item.entryLine, lines[item.firstLine].offset });
                if (isReachedFromSameLine(item)) {
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

        if (point.kinds)
            m_points.append(point);
    }
}

uint8_t CodeMonitor::toolsWantedAt(Point& point)
{
    uint8_t wanted = 0;
    for (unsigned event = 0; event < Event::instrumentedCount; ++event) {
        if (event != Event::Line && point.kinds & bit(event))
            wanted |= tools(event) & ~point.disabledTools[event];
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
                wanted |= tools(Event::Line) & ~start.disabledTools;
        }
    }
    if (point.lineStartOfTarget != noLineStart)
        wanted |= m_needsLastLine ? m_lineToolsStillOn : tools(Event::Line) & ~m_lineStarts[point.lineStartOfTarget].disabledTools;
    return wanted;
}

void CodeMonitor::update(Point& point)
{
    bool isWanted = toolsWantedAt(point);
    if (isWanted == point.isPatched)
        return;
    const void* instruction = m_instructions + point.offset;
    uint8_t* opcode = opcodeAddress(point);
    if (isWanted) {
        {
            Locker locker { replacedOpcodesLock };
            replacedOpcodes().set(instruction, ReplacedOpcode { point.opcode, this });
        }
        WTF::storeStoreFence();
        *opcode = op_py_instrumented;
    } else {
        *opcode = point.opcode;
        WTF::storeStoreFence();
        Locker locker { replacedOpcodesLock };
        replacedOpcodes().remove(instruction);
    }
    point.isPatched = isWanted;
}

void CodeMonitor::update()
{
    uint8_t anything = 0;
    for (unsigned event = 0; event < Event::instrumentedCount; ++event)
        anything |= tools(event);
    anything |= tools(Event::PyThrow) | tools(Event::Raise);
    if (!anything && !m_hasPoints)
        return;
    bool everyInstruction = tools(Event::Instruction);
    if (!m_hasPoints || (everyInstruction && !m_hasEveryInstruction))
        findPoints(everyInstruction);
    noteLineTools();
    for (auto& point : m_points)
        update(point);
}

void CodeMonitor::noteLineTools()
{
    m_lineToolsStillOn = 0;
    if (!m_needsLastLine)
        return;
    uint8_t lineTools = tools(Event::Line);
    for (auto& start : m_lineStarts)
        m_lineToolsStillOn |= lineTools & ~start.disabledTools;
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
    for (auto& point : m_points)
        point.disabledTools.fill(0);
    for (auto& start : m_lineStarts)
        start.disabledTools = 0;
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

JSArray* Monitoring::state()
{
    if (JSValue state = m_runtime.slot(Runtime::Slot::Monitoring))
        return asArray(state);
    JSGlobalObject* globalObject = m_runtime.globalObject();
    JSArray* state = newList(globalObject);
    JSArray* names = newList(globalObject);
    for (unsigned i = 0; i < numberOfMonitoringTools; ++i)
        listAppend(globalObject, names, jsUndefined());
    JSArray* callbacks = newList(globalObject);
    for (unsigned i = 0; i < numberOfMonitoringTools * Event::count; ++i)
        listAppend(globalObject, callbacks, jsUndefined());
    listAppend(globalObject, state, names);
    listAppend(globalObject, state, callbacks);
    listAppend(globalObject, state, PyObject::create(m_runtime.vm(), m_runtime.structure<PyObject>(), m_runtime.objectType()));
    listAppend(globalObject, state, PyObject::create(m_runtime.vm(), m_runtime.structure<PyObject>(), m_runtime.objectType()));
    JSArray* what = newList(globalObject);
    for (ASCIILiteral name : { "call"_s, "line"_s, "return"_s, "exception"_s, "opcode"_s, "c_call"_s, "c_return"_s, "c_exception"_s })
        listAppend(globalObject, what, internString(m_runtime.vm(), name));
    listAppend(globalObject, state, what);
    m_runtime.setSlot(Runtime::Slot::Monitoring, state);
    return state;
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
    uint8_t wanted = 0;
    auto consider = [&](auto& tools) {
        wanted |= tools[Event::Raise] | tools[Event::Reraise] | tools[Event::ExceptionHandled] | tools[Event::PyUnwind];
    };
    consider(m_globalTools);
    Locker locker { m_monitorsLock };
    for (CodeMonitor* monitor : m_monitors)
        consider(monitor->m_localTools);
    m_wantsExceptions = wanted;
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
        info->monitor = makeUnique<CodeMonitor>(*this, code);
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
    uint8_t anything = m_globalTools[Event::PyThrow];
    for (unsigned event = 0; event < Event::instrumentedCount; ++event)
        anything |= m_globalTools[event];
    if (!anything)
        return;
    if (CodeMonitor* monitor = monitorFor(codeBlock))
        monitor->update();
}

void Monitoring::updateAll()
{
    noteWhatIsWanted();
    VM& vm = m_runtime.vm();
    uint8_t anything = m_globalTools[Event::PyThrow];
    for (unsigned event = 0; event < Event::instrumentedCount; ++event)
        anything |= m_globalTools[event];
    if (anything) {
        Vector<CodeBlock*> codeBlocks;
        vm.heap.forEachCodeBlock([&](CodeBlock* codeBlock) {
            if (codeBlock->unlinkedCodeBlock()->isPython() && codeBlock->globalObject() == m_runtime.globalObject())
                codeBlocks.append(codeBlock);
        });
        for (CodeBlock* codeBlock : codeBlocks)
            monitorFor(codeBlock);
    }
    Vector<CodeMonitor*> monitors;
    {
        Locker locker { m_monitorsLock };
        monitors = copyToVector(m_monitors);
    }
    for (CodeMonitor* monitor : monitors)
        monitor->update();
    noteWhereFramesAre();
}

// A frame that is under way when lines begin to be told of has been on its line for a while.
void Monitoring::noteWhereFramesAre()
{
    if (!m_globalTools[Event::Line])
        return;
    VM& vm = m_runtime.vm();
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* callFrame = vm.topCallFrame; callFrame; callFrame = callFrame->callerFrame(entryFrame)) {
        if (callFrame->isNativeCalleeFrame() || !callFrame->codeBlock() || !callFrame->codeBlock()->unlinkedCodeBlock()->isPython())
            continue;
        if (!monitorFor(callFrame->codeBlock()))
            continue;
        PyFrame* frame = PyFrame::of(m_runtime.globalObject(), callFrame);
        if (frame->lastLine < 0) {
            auto marks = callFrame->codeBlock()->unlinkedCodeBlock()->pythonMarks();
            frame->lastLine = lineOfMarks(marks, callFrame->bytecodeIndex().offset());
            frame->lastLineOffset = visibleOffset(marks, callFrame->bytecodeIndex().offset());
        }
    }
}

Monitoring::OriginalInstructions::OriginalInstructions(Monitoring& monitoring)
    : m_monitoring(monitoring)
{
    Locker locker { monitoring.m_monitorsLock };
    for (CodeMonitor* monitor : monitoring.m_monitors)
        monitor->putBackInstructions();
}

Monitoring::OriginalInstructions::~OriginalInstructions()
{
    Locker locker { m_monitoring.m_monitorsLock };
    for (CodeMonitor* monitor : m_monitoring.m_monitors)
        monitor->update();
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
    m_events[tool] = 0;
    {
        Locker locker { m_monitorsLock };
        for (CodeMonitor* monitor : m_monitors)
            monitor->setLocalEvents(tool, 0);
    }
    updateAll();
}

void Monitoring::restartEvents()
{
    {
        Locker locker { m_monitorsLock };
        for (CodeMonitor* monitor : m_monitors)
            monitor->restart();
    }
    updateAll();
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
    CodeMonitor* monitor = monitorFor(globalObject, codeOf(globalObject, frame->function()));
    if (!monitor)
        return;
    uint32_t events = monitor->localEvents(traceTool);
    uint32_t wanted = enable ? events | 1 << Event::Instruction : events & ~(1 << Event::Instruction);
    if (events == wanted)
        return;
    monitor->setLocalEvents(traceTool, wanted);
    monitor->update();
}

// Nothing that happens meanwhile is told of.
JSValue Monitoring::callWhileTracing(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned event, Shown shown, JSValue function, const CallArgs& arguments)
{
    SetForScope tracedEvent(m_runtime.tracedEvent, static_cast<int>(event));
    SetForScope tracedCallFrame(m_runtime.tracedCallFrame, callFrame);
    SetForScope tracedLine(m_runtime.tracedLine, shown.line);
    SetForScope tracedOffset(m_runtime.tracedOffset, shown.offset);
    ++m_runtime.tracingDepth;
    JSValue result = call(globalObject, function, arguments);
    --m_runtime.tracingDepth;
    return result;
}

bool Monitoring::callTraceFunction(JSGlobalObject* globalObject, PyFrame* frame, unsigned event, Shown shown, What what, JSValue argument)
{
    VM& vm = m_runtime.vm();
    JSValue traceFunction = m_runtime.slot(Runtime::Slot::TraceFunction);
    if (!traceFunction)
        return true;
    // A function is called with "call", and what it returns with whatever else happens in the frame.
    JSValue function = event == Event::PyStart || event == Event::PyResume || event == Event::PyThrow ? traceFunction : frame->trace();
    if (!function)
        return true;
    JSValue arguments[3] = { frame, listGet(asArray(listGet(state(), StateWhat)), static_cast<unsigned>(what)), argument };
    JSValue result = callWhileTracing(globalObject, frame->callFrame(), event, shown, function, CallArgs(arguments));
    if (!result) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        Exception* exception = scope.exception();
        scope.clearException();
        setTraceFunction({ });
        frame->setTrace(vm, { });
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        throwException(globalObject, throwScope, exception);
        return false;
    }
    if (!isNone(result))
        frame->setTrace(vm, result);
    return true;
}

bool Monitoring::callProfileFunction(JSGlobalObject* globalObject, PyFrame* frame, unsigned event, Shown shown, What what, JSValue argument)
{
    VM& vm = m_runtime.vm();
    JSValue function = m_runtime.slot(Runtime::Slot::ProfileFunction);
    if (!function)
        return true;
    JSValue arguments[3] = { frame, listGet(asArray(listGet(state(), StateWhat)), static_cast<unsigned>(what)), argument };
    if (callWhileTracing(globalObject, frame->callFrame(), event, shown, function, CallArgs(arguments)))
        return true;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    scope.clearException();
    setProfileFunction({ });
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    throwException(globalObject, throwScope, exception);
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
    case Event::PyStart: {
        // Nothing has happened yet.
        const CodeInfo& info = functionOf(callFrame)->codeInfo();
        return { info.name.string() == "<module>"_s ? 0 : info.firstLineNumber, 0 };
    }
    case Event::PyUnwind:
    case Event::Reraise:
        // What raises an exception again on behalf of the program does so as if from where it was raised.
        if (monitor.lineAt(offset) < 0) {
            if (PyFrame* frame = frameObjectIfAny(callFrame))
                return { frame->raisedOnLine, frame->raisedAtOffset };
        }
        return { };
    default:
        return { };
    }
}

// What legacy_tracing.c does in CPython.
bool Monitoring::legacyEvent(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned tool, unsigned event, unsigned offset, JSValue third, JSValue fourth, bool& disable)
{
    if (!m_runtime.slot(tool == traceTool ? Runtime::Slot::TraceFunction : Runtime::Slot::ProfileFunction))
        return true;
    PyFrame* frame = PyFrame::of(globalObject, callFrame);
    Shown shown = whatToShow(callFrame, monitor, event, offset);

    if (tool == profileTool) {
        switch (event) {
        case Event::PyStart:
        case Event::PyResume:
        case Event::PyThrow:
            return callProfileFunction(globalObject, frame, event, shown, What::Call, jsUndefined());
        case Event::PyReturn:
        case Event::PyYield:
            return callProfileFunction(globalObject, frame, event, shown, What::Return, third);
        case Event::PyUnwind:
            return callProfileFunction(globalObject, frame, event, shown, What::Return, jsUndefined());
        case Event::Call:
        case Event::CReturn:
        case Event::CRaise: {
            What what = event == Event::Call ? What::CCall : event == Event::CReturn ? What::CReturn : What::CException;
            JSValue function = builtinFunctionToProfile(globalObject, third, fourth);
            if (!function)
                return !m_runtime.vm().exceptionForInspection();
            return callProfileFunction(globalObject, frame, event, shown, what, function);
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
        return callTraceFunction(globalObject, frame, event, shown, What::Call, jsUndefined());
    case Event::PyReturn:
    case Event::PyYield:
        // What the compiler adds at the end goes with the line before it, whichever that was.
        return callTraceFunction(globalObject, frame, event, { frame->lastLine, frame->lastLineOffset }, What::Return, third);
    case Event::PyUnwind:
        return callTraceFunction(globalObject, frame, event, shown, What::Return, jsUndefined());
    case Event::Raise:
    case Event::StopIteration: {
        JSValue traceback = jsUndefined();
        if (auto* exception = PyException::tryFrom(third); exception && exception->traceback())
            traceback = exception->traceback();
        return callTraceFunction(globalObject, frame, event, shown, What::Exception, PyTuple::create(globalObject, { m_runtime.typeOf(third), third, traceback }));
    }
    case Event::Jump: {
        // A trace function is told of a line each time a loop comes round to it, even if the whole loop is on that line.
        unsigned target = third.asInt32() / shownOffset(1);
        int line = monitor.lineAt(target);
        if (target > offset || line != monitor.lineAt(offset)) {
            disable = true;
            return true;
        }
        frame->lastLine = line;
        frame->lastLineOffset = target;
        return traceLine(globalObject, frame, { line, static_cast<int>(target) });
    }
    case Event::Instruction:
        if (!frame->tracesOpcodes) {
            setTracesOpcodes(globalObject, frame, false);
            return true;
        }
        return callTraceFunction(globalObject, frame, event, shown, What::Opcode, jsUndefined());
    default:
        return true;
    }
}

// ---- What happens ----

int Monitoring::callTool(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned tool, unsigned event, Shown shown, std::span<const JSValue> arguments)
{
    JSValue function = callback(tool, event);
    if (!function)
        return 0;
    JSValue result = callWhileTracing(globalObject, callFrame, event, shown, function, CallArgs(arguments));
    if (!result)
        return -1;
    return result == disable();
}

static constexpr ASCIILiteral eventNames[Event::count] = {
    "PY_START"_s, "PY_RESUME"_s, "PY_RETURN"_s, "PY_YIELD"_s, "CALL"_s, "LINE"_s, "INSTRUCTION"_s, "JUMP"_s, "BRANCH_LEFT"_s, "BRANCH_RIGHT"_s, "STOP_ITERATION"_s,
    "RAISE"_s, "EXCEPTION_HANDLED"_s, "PY_UNWIND"_s, "PY_THROW"_s, "RERAISE"_s, "C_RETURN"_s, "C_RAISE"_s, "BRANCH"_s,
};

bool Monitoring::fire(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned offset, unsigned event, uint8_t tools, JSValue third, JSValue fourth)
{
    if (!tools)
        return true;
    VM& vm = m_runtime.vm();
    JSValue arguments[4] = { { }, { }, third, fourth };
    unsigned count = fourth ? 4 : third ? 3 : 2;
    if (tools & ((1 << numberOfFreeMonitoringTools) - 1)) {
        arguments[0] = codeOf(globalObject, functionOf(callFrame));
        arguments[1] = jsNumber(shownOffset(visibleOffset(monitor.code()->pythonMarks(), offset)));
    }
    for (unsigned tool = numberOfMonitoringTools; tool--;) {
        if (!(tools >> tool & 1))
            continue;
        int result;
        if (tool >= profileTool) {
            bool disable = false;
            result = legacyEvent(globalObject, callFrame, monitor, tool, event, offset, third, fourth, disable) ? disable : -1;
        } else
            result = callTool(globalObject, callFrame, tool, event, whatToShow(callFrame, monitor, event, offset), std::span<const JSValue>(arguments, count));
        if (result < 0)
            return false;
        if (!result)
            continue;
        if (event < Event::instrumentedCount) {
            if (auto* point = monitor.pointAt(offset)) {
                point->disabledTools[event] |= 1 << tool;
                // BRANCH, the event of old, is either way a branch goes.
                if ((event == Event::BranchLeft || event == Event::BranchRight) && callback(tool, Event::Branch))
                    point->disabledTools[Event::BranchLeft] = point->disabledTools[Event::BranchRight] |= 1 << tool;
                monitor.update(*point);
            }
        } else if (event < Event::ungroupedCount) {
            monitor.removeLocalTool(event, tool);
            noteWhatIsWanted();
        } else {
            setCallback(tool, event, { });
            auto scope = DECLARE_THROW_SCOPE(vm);
            raiseValueError(globalObject, scope, "Cannot disable "_s, eventNames[event], " events. Callback removed."_s);
            return false;
        }
    }
    return true;
}

bool Monitoring::fireLineStart(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned offset, unsigned index)
{
    int line = monitor.m_lineStarts[index].line;
    Shown shown { line, static_cast<int>(monitor.m_lineStarts[index].offset) };
    uint8_t tools = monitor.tools(Event::Line) & ~monitor.m_lineStarts[index].disabledTools;
    if (!tools)
        return true;
    if (tools >> traceTool & 1) {
        PyFrame* frame = PyFrame::of(globalObject, callFrame);
        if (!traceLine(globalObject, frame, shown))
            return false;
        if (frame->jumpTarget)
            return true;
    }
    tools &= (1 << numberOfFreeMonitoringTools) - 1;
    if (!tools)
        return true;
    JSValue arguments[2] = { codeOf(globalObject, functionOf(callFrame)), jsNumber(line) };
    for (unsigned tool = numberOfFreeMonitoringTools; tool--;) {
        if (!(tools >> tool & 1))
            continue;
        int result = callTool(globalObject, callFrame, tool, Event::Line, shown, arguments);
        if (result < 0)
            return false;
        if (!result)
            continue;
        auto& start = monitor.m_lineStarts[index];
        start.disabledTools |= 1 << tool;
        monitor.didDisableLine(offset, start);
    }
    return true;
}

bool Monitoring::fireLine(JSGlobalObject* globalObject, CallFrame* callFrame, CodeMonitor& monitor, unsigned offset)
{
    auto* point = monitor.pointAt(offset);
    unsigned first = point->firstLineStart;
    unsigned count = point->lineStartCount;
    bool firstLineIsInDoubt = point->firstLineIsInDoubt;
    PyFrame* frame = monitor.m_needsLastLine || monitor.tools(Event::Line) >> traceTool ? PyFrame::of(globalObject, callFrame) : frameObjectIfAny(callFrame);
    // It got here because a trace function said so.
    if (frame && frame->jumpedTo) {
        if (*std::exchange(frame->jumpedTo, std::nullopt) == offset) {
            frame->lastLine = monitor.m_lineStarts[first + count - 1].line;
            frame->lastLineOffset = monitor.m_lineStarts[first + count - 1].offset;
            return true;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        int line = monitor.m_lineStarts[first + i].line;
        bool isNews = i || !firstLineIsInDoubt || line != frame->lastLine;
        if (frame) {
            frame->lastLine = line;
            frame->lastLineOffset = monitor.m_lineStarts[first + i].offset;
        }
        if (!isNews)
            continue;
        if (!fireLineStart(globalObject, callFrame, monitor, offset, first + i))
            return false;
        if (!frame)
            frame = frameObjectIfAny(callFrame);
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

// iterNext(), for a `for` loop. If the iterator ends by raising StopIteration, that is told of as raised in the loop. A generator that
// returns something raises it as far as anybody can tell.
static JSValue nextOfLoop(JSGlobalObject* globalObject, JSValue iterator, JSValue& stopIteration)
{
    Runtime& rt = runtime(globalObject);
    if (auto* generator = PyGenerator::tryFrom(iterator); generator && generator->kind() == PyGenerator::Kind::Generator) {
        JSValue returned;
        JSValue item = generatorSend(globalObject, generator, jsUndefined(), returned);
        if (!item && returned && !isNone(returned))
            stopIteration = newException(globalObject, rt.StopIteration(), std::span<const JSValue> { &returned, 1 });
        return item;
    }
    PyType* type = rt.typeOf(iterator);
    JSValue method = type->isHeapType() ? type->lookup(Special::Next) : JSValue();
    if (!method || isNone(method))
        return iterNext(globalObject, iterator);
    JSValue item = callSpecial(globalObject, method, iterator);
    if (!item && pendingExceptionMatches(globalObject, rt.StopIteration()))
        stopIteration = takePendingException(globalObject);
    return item;
}

Monitoring::Outcome Monitoring::handle(JSGlobalObject* globalObject, CallFrame* callFrame, const JSInstruction* pc)
{
    VM& vm = m_runtime.vm();
    CodeBlock* codeBlock = callFrame->codeBlock();
    CodeMonitor* monitorPointer = functionOf(callFrame)->codeInfo().monitor.get();
    RELEASE_ASSERT(monitorPointer);
    CodeMonitor& monitor = *monitorPointer;
    unsigned offset = codeBlock->bytecodeOffset(pc);
    auto* point = monitor.pointAt(offset);
    RELEASE_ASSERT(point);
    // A tool may change what there is to know about the places in the code. These stay.
    auto opcode = static_cast<OpcodeID>(point->opcode);
    uint16_t kinds = point->kinds;
    Outcome runIt { pc, opcode };
    static constexpr Outcome threw { nullptr, 0 };
    // What a tool does is not told of.
    if (m_runtime.tracingDepth)
        return runIt;

    auto next = [&] { return reinterpret_cast<const JSInstruction*>(reinterpret_cast<const uint8_t*>(pc) + pc->size()); };
    auto wanted = [&](unsigned event) -> uint8_t {
        auto* point = monitor.pointAt(offset);
        return point ? monitor.tools(event) & ~point->disabledTools[event] : 0;
    };
    // frame.f_lineno = line, in a trace function
    auto jumped = [&](Outcome& outcome) {
        PyFrame* frame = frameObjectIfAny(callFrame);
        if (!frame || !frame->jumpTarget)
            return false;
        frame->jumpedTo = std::exchange(frame->jumpTarget, std::nullopt);
        outcome = { codeBlock->instructions().at(*frame->jumpedTo).ptr(), goOn };
        return true;
    };
    Outcome elsewhere;

    if (kinds & bit(Event::PyStart)) {
        PyFrame* frame = frameObjectIfAny(callFrame);
        if (frame && frame->jumpedTo && std::exchange(frame->jumpsToStart, false))
            frame->jumpedTo = std::nullopt;
        else if (!fire(globalObject, callFrame, monitor, offset, Event::PyStart, wanted(Event::PyStart))) {
            // The call has not begun: no `try` of its own catches this. The unwinder takes a frame at offset 0 for one that
            // op_py_bind_args has not counted in yet.
            callFrame->setCurrentVPC(codeBlock->instructions().at(0).ptr());
            if (codeBlock->unlinkedCodeBlock()->parseMode() != SourceParseMode::GeneratorBodyMode)
                m_runtime.leaveFrame();
            return threw;
        }
    }
    if (kinds & bit(Event::PyResume)) {
        if (PyFrame* frame = frameObjectIfAny(callFrame)) {
            frame->lastLine = monitor.lineAt(offset);
            frame->lastLineOffset = offset;
        }
        bool isThrow = callFrame->uncheckedArgument(static_cast<int>(JSGenerator::Argument::ResumeMode) - 1).asInt32() != static_cast<int32_t>(JSGenerator::ResumeMode::NormalMode);
        if (isThrow) {
            JSValue exception = callFrame->uncheckedArgument(static_cast<int>(JSGenerator::Argument::Value) - 1);
            if (!fire(globalObject, callFrame, monitor, offset, Event::PyThrow, monitor.tools(Event::PyThrow), exception))
                return threw;
        } else if (!fire(globalObject, callFrame, monitor, offset, Event::PyResume, wanted(Event::PyResume)))
            return threw;
        if (jumped(elsewhere))
            return elsewhere;
    }
    if (kinds & bit(Event::Line)) {
        if (!fireLine(globalObject, callFrame, monitor, offset))
            return threw;
        if (jumped(elsewhere))
            return elsewhere;
    }
    if (kinds & bit(Event::Instruction)) {
        if (!fire(globalObject, callFrame, monitor, offset, Event::Instruction, wanted(Event::Instruction)))
            return threw;
    }

    switch (opcode) {
    case op_py_ret:
        if (!fire(globalObject, callFrame, monitor, offset, Event::PyReturn, wanted(Event::PyReturn), callFrame->r(pc->as<OpPyRet>().m_value).jsValue()))
            return threw;
        return runIt;

    case op_ret: {
        unsigned event = kinds & bit(Event::PyYield) ? Event::PyYield : Event::PyReturn;
        if (!fire(globalObject, callFrame, monitor, offset, event, wanted(event), callFrame->r(pc->as<OpRet>().m_value).jsValue()))
            return threw;
        if (jumped(elsewhere))
            return elsewhere;
        return runIt;
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
    case op_call_varargs: {
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
            auto bytecode = pc->as<OpCallVarargs>();
            callee = callFrame->r(bytecode.m_callee).jsValue();
            thisValue = callFrame->r(bytecode.m_thisValue).jsValue();
            JSArray* list = asArray(callFrame->r(bytecode.m_arguments).jsValue());
            for (unsigned i = 0; i < listLength(list); ++i)
                spread.append(listGet(list, i));
            arguments = reinterpret_cast<const JSValue*>(spread.data());
            count = spread.size();
            dst = bytecode.m_dst;
        }
        PyTuple* keywordNames = thisValue.isCell() && thisValue.asCell()->type() == PyKwNamesType ? uncheckedDowncast<PyTuple>(thisValue.asCell()) : nullptr;
        JSValue callable = callee;
        JSValue first = count > (opcode == op_call_varargs && keywordNames ? keywordNames->length() : 0) ? arguments[0] : missing();
        // f(*args) is told of as it stands, as in CPython.
        if (auto* method = opcode == op_call ? PyMethod::tryFrom(callee) : nullptr) {
            callable = method->function();
            first = method->self();
        }
        if (!fire(globalObject, callFrame, monitor, offset, Event::Call, tools, callable, first))
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
        JSValue result = call(globalObject, callee, CallArgs(arguments, count - (keywordNames ? keywordNames->length() : 0), keywordNames));
        // It may have been sys.monitoring.set_events().
        listening &= wanted(Event::Call);
        if (!result) {
            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            Exception* exception = scope.exception();
            scope.clearException();
            if (fire(globalObject, callFrame, monitor, offset, Event::CRaise, listening, callable, first)) {
                auto throwScope = DECLARE_THROW_SCOPE(vm);
                throwException(globalObject, throwScope, exception);
            }
            return threw;
        }
        callFrame->uncheckedR(dst) = result;
        if (!fire(globalObject, callFrame, monitor, offset, Event::CReturn, listening, callable, first))
            return threw;
        return { next(), goOn };
    }

    case op_jmp: {
        // A jump to a jump is told of as one jump: CPython's compiler makes the first go where the second goes.
        auto targetOf = [&](unsigned jump) { return jump + jumpTargetForInstruction<OpJmp>(codeBlock, codeBlock->instructions().at(jump)); };
        auto jumpAt = [&](unsigned where) {
            auto* other = monitor.pointAt(where);
            return other && other->opcode == op_jmp && other->kinds & bit(Event::Jump) ? other : nullptr;
        };
        bool isToldOfAlready = m_jumpGoesOn.callFrame == callFrame && m_jumpGoesOn.offset == offset;
        m_jumpGoesOn = { };
        unsigned target = targetOf(offset);
        if (!isToldOfAlready) {
            unsigned last = target;
            for (unsigned count = 0; count < 8 && jumpAt(last); ++count)
                last = targetOf(last);
            if (!fire(globalObject, callFrame, monitor, offset, Event::Jump, wanted(Event::Jump), jsNumber(shownOffset(last))))
                return threw;
            if (jumped(elsewhere))
                return elsewhere;
        }
        point = monitor.pointAt(offset);
        if (point && point->lineStartOfTarget != CodeMonitor::noLineStart) {
            if (monitor.m_needsLastLine || monitor.tools(Event::Line) >> traceTool) {
                PyFrame* frame = PyFrame::of(globalObject, callFrame);
                frame->lastLine = monitor.m_lineStarts[point->lineStartOfTarget].line;
                frame->lastLineOffset = monitor.m_lineStarts[point->lineStartOfTarget].offset;
            }
            if (!fireLineStart(globalObject, callFrame, monitor, offset, point->lineStartOfTarget))
                return threw;
            if (jumped(elsewhere))
                return elsewhere;
        }
        if (auto* other = jumpAt(target); other && other->isPatched)
            m_jumpGoesOn = { callFrame, target };
        return runIt;
    }

    default:
        break;
    }

    // `yield from` or `await` is over. If it was a generator, that has returned without raising StopIteration, and is told of as if it had.
    if (kinds & bit(Event::StopIteration)) {
        uint8_t tools = wanted(Event::StopIteration);
        auto bytecode = pc->as<OpPyRuntime>();
        if (!tools || !PyGenerator::is(callFrame->uncheckedR(bytecode.m_argv).jsValue()))
            return runIt;
        const void* target;
        const void* signal;
        decodeResult(slow_path_py_runtime(callFrame, pc), target, signal);
        if (signal == LLInt::exceptionSignal())
            return threw;
        callFrame->setCurrentVPC(pc);
        JSValue value = callFrame->uncheckedR(bytecode.m_dst).jsValue();
        PyException* exception;
        if (auto* tuple = PyTuple::tryFrom(value))
            exception = newException(globalObject, m_runtime.StopIteration(), tuple->span());
        else if (isNone(value))
            exception = newException(globalObject, m_runtime.StopIteration(), std::span<const JSValue> { });
        else
            exception = newException(globalObject, m_runtime.StopIteration(), std::span<const JSValue> { &value, 1 });
        if (!exception || !fire(globalObject, callFrame, monitor, offset, Event::StopIteration, tools, exception))
            return threw;
        return { next(), goOn };
    }

    // Which way a branch goes is known once it has gone.
    if (kinds & bit(Event::BranchLeft) && (wanted(Event::BranchLeft) | wanted(Event::BranchRight) | (kinds & CodeMonitor::Point::forLoop ? monitor.tools(Event::Raise) : 0))) {
        const JSInstruction* destination;
        if (kinds & CodeMonitor::Point::forLoop) {
            auto bytecode = pc->as<OpPyForIter>();
            JSValue stopIteration;
            JSValue item = nextOfLoop(globalObject, callFrame->uncheckedR(bytecode.m_iterator).jsValue(), stopIteration);
            if (vm.exceptionForInspection())
                return threw;
            if (item) {
                callFrame->uncheckedR(bytecode.m_dst) = item;
                destination = next();
            } else
                destination = codeBlock->instructions().at(offset + jumpTargetForInstruction<OpPyForIter>(codeBlock, codeBlock->instructions().at(offset))).ptr();
            if (stopIteration && !fire(globalObject, callFrame, monitor, offset, Event::Raise, monitor.tools(Event::Raise), stopIteration))
                return threw;
        } else {
            const void* target;
            const void* signal;
            decodeResult(slowPathOfBranch(opcode)(callFrame, pc), target, signal);
            if (signal == LLInt::exceptionSignal())
                return threw;
            callFrame->setCurrentVPC(pc);
            destination = static_cast<const JSInstruction*>(target);
        }
        unsigned event = destination == next() ? Event::BranchLeft : Event::BranchRight;
        unsigned shownDestination = codeBlock->bytecodeOffset(destination);
        if (event == Event::BranchLeft && kinds & CodeMonitor::Point::goesOnInItself)
            shownDestination = whereItGoesOnInItself(shownDestination);
        if (!fire(globalObject, callFrame, monitor, offset, event, wanted(event), jsNumber(shownOffset(shownDestination))))
            return threw;
        return { destination, goOn };
    }
    return runIt;
}

bool Monitoring::exceptionEvent(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned offset, unsigned event, JSValue exception)
{
    if (m_runtime.tracingDepth)
        return true;
    CodeMonitor* monitor = functionOf(callFrame)->codeInfo().monitor.get();
    uint8_t tools = m_globalTools[event] | (monitor ? monitor->localTools(event) : 0);
    if (!tools)
        return true;
    if (!monitor)
        monitor = monitorFor(callFrame->codeBlock());
    if (!monitor)
        return true;
    return fire(globalObject, callFrame, *monitor, offset, event, tools, exception);
}

// `thrown` is being thrown, and the unwinder is at the frame.
static JSValue tellOfException(JSGlobalObject* globalObject, JSValue thrown, CallFrame* callFrame, unsigned offset, unsigned event)
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
    JSValue exception = exceptionFromThrownValue(globalObject, thrown);
    if (exception && rt.monitoring().exceptionEvent(globalObject, callFrame, offset, event, exception)) {
        auto throwScope = DECLARE_THROW_SCOPE(vm);
        throwException(globalObject, throwScope, pending);
        return thrown;
    }
    return scope.exception()->value();
}

JSValue exceptionIsRaised(JSGlobalObject* globalObject, JSValue exception, CallFrame* callFrame, BytecodeIndex index, bool isRaisedAgain)
{
    if (!runtime(globalObject).monitoring().wantsExceptions()) [[likely]]
        return exception;
    auto marks = callFrame->codeBlock()->unlinkedCodeBlock()->pythonMarks();
    if (int line = lineOfMarks(marks, index.offset()); line >= 0) {
        PyFrame* frame = PyFrame::of(globalObject, callFrame);
        frame->raisedOnLine = line;
        frame->raisedAtOffset = visibleOffset(marks, index.offset());
    }
    return tellOfException(globalObject, exception, callFrame, index.offset(), isRaisedAgain ? Event::Reraise : Event::Raise);
}

JSValue exceptionStaysOrLeaves(JSGlobalObject* globalObject, JSValue exception, CallFrame* callFrame, BytecodeIndex index, bool isCaught)
{
    if (!runtime(globalObject).monitoring().wantsExceptions()) [[likely]]
        return exception;
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (codeBlock->instructionAt(index)->opcodeID() == op_py_bind_args)
        return exception;
    unsigned offset = index.offset();
    if (isCaught) {
        if (auto* handler = codeBlock->handlerForBytecodeIndex(index))
            offset = handler->target;
    }
    // In CPython all of a generator is in a handler, which makes a RuntimeError of a StopIteration and raises anything else again.
    if (!isCaught && functionOf(callFrame)->codeInfo().isGeneratorLike()) {
        exception = tellOfException(globalObject, exception, callFrame, offset, Event::ExceptionHandled);
        exception = tellOfException(globalObject, exception, callFrame, offset, Event::Reraise);
    }
    return tellOfException(globalObject, exception, callFrame, offset, isCaught ? Event::ExceptionHandled : Event::PyUnwind);
}

void didCreateCodeBlock(CodeBlock* codeBlock)
{
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

    const CodeInfo& info = frame->function()->codeInfo();
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
            m_runtime.setHandledExceptionInTopSlot(callFrame->r(block.reg).jsValue());
    }
    Vector<JSValue, 8> held;
    for (unsigned i = 0; i < best->blocks.size(); ++i)
        held.append(from->at(i).reg.isValid() ? callFrame->r(from->at(i).reg).jsValue() : JSValue());
    for (unsigned i = 0; i < best->blocks.size(); ++i) {
        if (best->blocks[i].reg.isValid() && from->at(i).reg.isValid())
            callFrame->r(best->blocks[i].reg) = held[i];
    }

    frame->jumpTarget = best->offset;
    frame->jumpsToStart = best->isStart;
    frame->lastLine = line;
    frame->lastLineOffset = best->visibleOffset;
    m_runtime.tracedLine = line;
    m_runtime.tracedOffset = best->visibleOffset;
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
    if (!monitoringTool(globalObject, toolValue, tool))
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
    Monitoring& monitoring = runtime(globalObject).monitoring();
    unsigned tool;
    if (!monitoringTool(globalObject, toolValue, tool))
        return { };
    if (monitoring.toolName(tool))
        monitoring.clearTool(tool);
    return jsUndefined();
}

static JSValue monitoringFreeToolId(JSGlobalObject* globalObject, JSValue toolValue)
{
    Monitoring& monitoring = runtime(globalObject).monitoring();
    unsigned tool;
    if (!monitoringTool(globalObject, toolValue, tool))
        return { };
    if (monitoring.toolName(tool))
        monitoring.clearTool(tool);
    monitoring.setToolName(tool, { });
    return jsUndefined();
}

static JSValue monitoringGetTool(JSGlobalObject* globalObject, JSValue toolValue)
{
    unsigned tool;
    if (!monitoringTool(globalObject, toolValue, tool))
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
    if (!monitoringTool(globalObject, toolValue, tool))
        return { };
    int event = toCInt(globalObject, eventValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (std::popcount(static_cast<unsigned>(event)) != 1)
        return raiseValueError(globalObject, scope, "The callback can only be set for one event at a time"_s);
    unsigned index = std::countr_zero(static_cast<unsigned>(event));
    if (index >= Event::count)
        return raiseValueError(globalObject, scope, "invalid event "_s, event);
    if (!audit(globalObject, "sys.monitoring.register_callback"_s, function))
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
    unsigned tool;
    if (!monitoringTool(globalObject, toolValue, tool))
        return { };
    return jsNumber(runtime(globalObject).monitoring().events(tool));
}

static JSValue monitoringSetEvents(JSGlobalObject* globalObject, JSValue toolValue, JSValue eventSet)
{
    unsigned tool;
    uint32_t events;
    if (!monitoringTool(globalObject, toolValue, tool) || !monitoringEventSet(globalObject, eventSet, false, events) || !monitoringToolInUse(globalObject, tool))
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
    if (!monitoringTool(globalObject, toolValue, tool))
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
    if (!monitoringTool(globalObject, toolValue, tool) || !monitoringEventSet(globalObject, eventSet, true, events) || !monitoringToolInUse(globalObject, tool))
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
    monitor.update();
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
    JSArray* result = newList(globalObject);
    CodeMonitor* monitor = code->codeInfo().monitor.get();
    if (!monitor)
        return result;
    for (auto& point : monitor->m_points) {
        JSArray* lines = newList(globalObject);
        for (auto& start : monitor->lineStartsOf(point))
            listAppend(globalObject, lines, jsNumber(start.line));
        listAppend(globalObject, result, PyTuple::create(globalObject, { jsNumber(point.offset), makeStr(globalObject, String::fromLatin1(opcodeNames[point.opcode])), jsNumber(point.kinds), lines,
            jsBoolean(point.firstLineIsInDoubt), point.lineStartOfTarget == CodeMonitor::noLineStart ? jsUndefined() : jsNumber(monitor->m_lineStarts[point.lineStartOfTarget].line), jsBoolean(point.isPatched) }));
    }
    return result;
}

JSArray* Monitoring::branches(JSGlobalObject* globalObject, PyCode* code)
{
    JSArray* result = newList(globalObject);
    CodeMonitor* monitor = monitorFor(globalObject, code);
    if (!monitor)
        return result;
    if (!monitor->m_hasPoints)
        monitor->findPoints(false);
    const JSInstructionStream& instructions = monitor->code()->instructions();
    for (auto& point : monitor->m_points) {
        if (!(point.kinds & bit(Event::BranchLeft)))
            continue;
        auto instruction = instructions.at(point.offset);
        extractStoredJumpTargetsForInstruction(monitor->code(), instruction, [&](int32_t relative) {
            unsigned left = point.offset + instruction->size();
            if (point.kinds & CodeMonitor::Point::goesOnInItself)
                left = whereItGoesOnInItself(left);
            listAppend(globalObject, result, PyTuple::create(globalObject, { jsNumber(shownOffset(visibleOffset(monitor->code()->pythonMarks(), point.offset))), jsNumber(shownOffset(left)), jsNumber(shownOffset(point.offset + relative)) }));
        });
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
    if (!audit(globalObject, "sys.settrace"_s))
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
    if (!audit(globalObject, "sys.setprofile"_s))
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
