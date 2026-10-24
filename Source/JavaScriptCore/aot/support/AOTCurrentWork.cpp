/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCurrentWork.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTIRProgram.h"
#include <algorithm>
#include <array>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>

#if OS(UNIX)
#include <signal.h>
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

namespace CurrentWorkInternal {

static constexpr size_t numberOfRecords = 512;

static constinit std::array<CurrentWork, numberOfRecords> records { };
constinit CurrentWork sharedRecord { };
constinit thread_local CurrentWork* recordOfThread = &sharedRecord;

namespace {

struct RecordHolder {
    ~RecordHolder()
    {
        CurrentWork* record = std::exchange(recordOfThread, &sharedRecord);
        if (record != &sharedRecord)
            record->isTaken.store(false, std::memory_order_release);
#if OS(UNIX)
        if (!signalStack)
            return;
        stack_t none { };
        none.ss_flags = SS_DISABLE;
        sigaltstack(&none, nullptr);
        munmap(signalStack, signalStackSize);
#endif
    }

#if OS(UNIX)
    static constexpr size_t signalStackSize = 512 * KB;

    // A thread that has run out of stack has none left for whoever reports that.
    void ensureSignalStack()
    {
        stack_t current { };
        if (signalStack || sigaltstack(nullptr, &current) || !(current.ss_flags & SS_DISABLE))
            return;
        void* memory = mmap(nullptr, signalStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (memory == MAP_FAILED)
            return;
        stack_t stack { };
        stack.ss_sp = memory;
        stack.ss_size = signalStackSize;
        if (sigaltstack(&stack, nullptr)) {
            munmap(memory, signalStackSize);
            return;
        }
        signalStack = memory;
    }

    void* signalStack { nullptr };
#endif
};

} // anonymous namespace

CurrentWork& takeRecord()
{
    static thread_local RecordHolder holder;
#if OS(UNIX)
    holder.ensureSignalStack();
#else
    UNUSED_VARIABLE(holder);
#endif
    for (CurrentWork& record : records) {
        if (record.isTaken.load(std::memory_order_relaxed) || record.isTaken.exchange(true, std::memory_order_acquire))
            continue;
        recordOfThread = &record;
        return record;
    }
    return sharedRecord;
}

} // namespace CurrentWorkInternal

namespace {

// Text in memory that the caller has. What does not fit is left out.
class BoundedText {
public:
    explicit BoundedText(std::span<char> buffer)
        : m_buffer(buffer)
    {
    }

    size_t size() const { return m_size; }

    void append(char character)
    {
        if (m_size < m_buffer.size())
            m_buffer[m_size++] = character;
    }

    void append(ASCIILiteral text)
    {
        for (char character : text.span())
            append(character);
    }

    void append(const char* text)
    {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        for (; *text; ++text)
            append(*text);
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }

    void append(uint64_t number)
    {
        std::array<char, 20> digits;
        size_t count = 0;
        do
            digits[count++] = static_cast<char>('0' + number % 10);
        while (number /= 10);
        while (count)
            append(digits[--count]);
    }

    // A name or a path of the program, which a terminal is going to show.
    void appendProgramText(std::span<const uint8_t> text)
    {
        constexpr size_t longestShown = 200;
        for (uint8_t byte : text.first(std::min(text.size(), longestShown)))
            append(byte < 0x20 || byte == 0x7f ? '?' : static_cast<char>(byte));
        if (text.size() > longestShown)
            append("..."_s);
    }

private:
    std::span<char> m_buffer;
    size_t m_size { 0 };
};

struct WorkAsRead {
    explicit WorkAsRead(const CurrentWork& work)
        : program(work.program.load(std::memory_order_relaxed))
        , functionPlusOne(work.functionPlusOne.load(std::memory_order_relaxed))
        , pass(work.pass.load(std::memory_order_relaxed))
        , stage(work.stage.load(std::memory_order_relaxed))
        , backEndPhase(work.backEndPhase.load(std::memory_order_relaxed))
        , finishedPart(work.finishedPart.load(std::memory_order_relaxed))
        , startInSeconds(work.startInSeconds.load(std::memory_order_relaxed))
    {
        if (static_cast<uint32_t>(pass) >= numberOfPasses)
            pass = PassID::None;
        if (static_cast<uint32_t>(stage) >= numberOfEnumerators<Stage>)
            stage = Stage::Any;
    }

    bool isNothing() const { return !functionPlusOne && pass == PassID::None && stage == Stage::Any; }

    const Typed::Program* program;
    uint32_t functionPlusOne;
    PassID pass;
    Stage stage;
    const char* backEndPhase;
    const char* finishedPart;
    double startInSeconds;
};

} // anonymous namespace

static std::span<const uint8_t> textOrNothing(const Typed::Program& program, Typed::StringID id)
{
    if (!id || Typed::StringTable::isProvisional(id) || id.index() >= program.strings.size())
        return { };
    return program.strings[id];
}

// "parse (src/parser.ts:12)", "a function (src/parser.ts:12)", "src/parser.ts".
static bool appendFunction(BoundedText& text, const WorkAsRead& work)
{
    if (!work.program || !work.functionPlusOne)
        return false;
    const Typed::Program& program = *work.program;
    uint32_t index = work.functionPlusOne - 1;
    if (index >= program.functions.size())
        return false;
    const Typed::Function& function = program.functions[index];
    std::span<const uint8_t> path;
    if (function.module && function.module.index() < program.modules.size())
        path = textOrNothing(program, program.pathForLineColumns(function.module));
    if (function.isModuleBody()) {
        if (path.empty())
            return false;
        text.appendProgramText(path);
        return true;
    }
    std::span<const uint8_t> name = textOrNothing(program, function.name);
    if (name.empty())
        text.append("a function"_s);
    else
        text.appendProgramText(name);
    if (path.empty())
        return true;
    text.append(" ("_s);
    text.appendProgramText(path);
    text.append(':');
    text.append(static_cast<uint64_t>(program.lineColumnFor(function.module, function.position).line));
    text.append(')');
    return true;
}

// " in PlaceTypeTests", " in GenerateCode (allocateRegistersByGraphColoring)", " in AnalyzeFlow (after: link bodies)".
static void appendStep(BoundedText& text, const WorkAsRead& work)
{
    if (work.pass == PassID::None && work.stage == Stage::Any)
        return;
    text.append(" in "_s);
    text.append(work.pass != PassID::None ? nameOf(work.pass) : nameOf(work.stage));
    if (work.backEndPhase) {
        text.append(" ("_s);
        text.append(work.backEndPhase);
        text.append(')');
    } else if (work.finishedPart) {
        text.append(" (after: "_s);
        text.append(work.finishedPart);
        text.append(')');
    }
}

static void appendWork(BoundedText& text, const WorkAsRead& work)
{
    text.append(" compiling"_s);
    BoundedText rest = text;
    rest.append(' ');
    if (appendFunction(rest, work))
        text = rest;
    appendStep(text, work);
}

static void appendWayAround(BoundedText& text, PassID pass)
{
    if (!isOptionalPass(pass))
        return;
    text.append("To build without that step, add --aot-disable=pass:"_s);
    text.append(nameOf(pass));
    text.append(".\n"_s);
}

size_t describeWhereThisThreadStopped(std::span<char> buffer)
{
    WorkAsRead work(currentWork());
    if (&currentWork() == &CurrentWorkInternal::sharedRecord || work.isNothing())
        return 0;
    BoundedText text(buffer);
    text.append("Stopped while"_s);
    appendWork(text, work);
    text.append(".\n"_s);
    appendWayAround(text, work.pass);
    return text.size();
}

size_t describeWhatThreadsAreCompiling(std::span<char> buffer)
{
    BoundedText text(buffer);
    double now = MonotonicTime::now().secondsSinceEpoch().value();
    // The threads of a loop that names no function are all at the same.
    std::array<std::pair<PassID, Stage>, 16> stepsSaid;
    size_t numberOfStepsSaid = 0;
    // No function takes a minute.
    double longestTimeInFunction = 60;
    PassID passOfLongestTime = PassID::None;
    bool isStuck = false;
    for (const CurrentWork& record : CurrentWorkInternal::records) {
        if (!record.isTaken.load(std::memory_order_acquire))
            continue;
        WorkAsRead work(record);
        if (work.isNothing())
            continue;
        if (!work.functionPlusOne) {
            std::pair step { work.pass, work.stage };
            auto said = std::span(stepsSaid).first(numberOfStepsSaid);
            if (std::ranges::find(said, step) != said.end())
                continue;
            if (numberOfStepsSaid < stepsSaid.size())
                stepsSaid[numberOfStepsSaid++] = step;
        }
        double time = std::max(now - work.startInSeconds, 0.0);
        if (work.functionPlusOne && time >= longestTimeInFunction) {
            longestTimeInFunction = time;
            passOfLongestTime = work.pass;
            isStuck = true;
        }
        text.append("Still"_s);
        appendWork(text, work);
        text.append(" for "_s);
        text.append(static_cast<uint64_t>(time));
        text.append(" s.\n"_s);
    }
    if (isStuck) {
        text.append("A build that stays this long in one function is stuck, which is a bug in Bun. Please report it with these lines: https://github.com/oven-sh/bun/issues\n"_s);
        appendWayAround(text, passOfLongestTime);
    }
    return text.size();
}

void stopWhereSabotaged(const BuildContext& build, std::optional<Stage> stage, PassID pass)
{
    RELEASE_ASSERT(!build.isSabotaged(Sabotage::ProgramPassFailsAssertion) || pass != PassID::BuildCallGraph);
    RELEASE_ASSERT(!build.isSabotaged(Sabotage::FlowAnalysisFailsAssertion) || pass != PassID::AnalyzeFlow);
    RELEASE_ASSERT(!build.isSabotaged(Sabotage::ImageBuildingFailsAssertion) || stage != Stage::BuildImage);
}

static NEVER_INLINE uint64_t useStackWithoutEnd(uint64_t depth)
{
    volatile uint64_t words[64];
    words[depth % 64] = depth;
    if (!words[depth % 64])
        return 0;
    return useStackWithoutEnd(depth + 1) + words[depth % 64];
}

void stopWhereSabotaged(const BuildContext& build, const Typed::Program& program, const Typed::Function& function, PassID pass)
{
    if (!equalSpans(textOrNothing(program, function.name), "stopsTheCompiler"_span8))
        return;
    if (pass == PassID::GenerateCode) {
        RELEASE_ASSERT(!build.isSabotaged(Sabotage::CodeGenerationFailsAssertion));
        return;
    }
    RELEASE_ASSERT(!build.isSabotaged(Sabotage::FunctionPassFailsAssertion));
    if (build.isSabotaged(Sabotage::FunctionPassReadsBadAddress)) {
        volatile uintptr_t badAddress = 16;
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        *reinterpret_cast<volatile uint32_t*>(badAddress);
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }
    if (build.isSabotaged(Sabotage::FunctionPassRunsOutOfStack))
        useStackWithoutEnd(1);
    if (!build.isSabotaged(Sabotage::FunctionPassNeverEnds))
        return;
    dataLogLn("This build never ends.");
    for (;;)
        WTF::sleep(10_ms);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
