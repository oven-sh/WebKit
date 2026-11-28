/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCurrentWork.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <utility>
#include <wtf/StdLibExtras.h>

#if OS(UNIX)
#include <signal.h>
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

namespace CurrentWorkInternal {

static constexpr size_t numberOfCurrentWorkRecords = 512;

using CurrentWorkRecords = std::array<CurrentWork, numberOfCurrentWorkRecords>;

static constinit std::atomic<CurrentWorkRecords*> currentWorkRecords { nullptr };
constinit CurrentWork sharedRecord { };
constinit thread_local CurrentWork* recordOfThread = &sharedRecord;

static std::span<CurrentWork> allCurrentWorkRecords()
{
    CurrentWorkRecords* records = currentWorkRecords.load(std::memory_order_acquire);
    return records ? std::span<CurrentWork>(*records) : std::span<CurrentWork>();
}

namespace {

struct CurrentWorkRecordHolder {
    ~CurrentWorkRecordHolder()
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
    WTF_THREAD_LOCAL_ALWAYS_DESTROY static thread_local CurrentWorkRecordHolder holder;
#if OS(UNIX)
    holder.ensureSignalStack();
#else
    UNUSED_VARIABLE(holder);
#endif
    static std::once_flag makeRecords;
    std::call_once(makeRecords, [] {
        currentWorkRecords.store(makeUniqueWithoutFastMallocCheck<CurrentWorkRecords>().release(), std::memory_order_release);
    });
    for (CurrentWork& record : allCurrentWorkRecords()) {
        if (record.isTaken.load(std::memory_order_relaxed) || record.isTaken.exchange(true, std::memory_order_acquire))
            continue;
        recordOfThread = &record;
        return record;
    }
    return sharedRecord;
}

} // namespace CurrentWorkInternal

namespace {

struct CurrentWorkAsRead {
    explicit CurrentWorkAsRead(const CurrentWork& work)
        : program(work.program.load(std::memory_order_relaxed))
        , functionPlusOne(work.functionPlusOne.load(std::memory_order_relaxed))
        , step(work.step.load(std::memory_order_relaxed))
    {
    }

    bool isIdle() const { return !functionPlusOne && !step; }

    const Typed::Program* program;
    uint32_t functionPlusOne;
    const char* step;
};

class CurrentWorkText {
public:
    explicit CurrentWorkText(std::span<char> buffer)
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

    void appendWork(const CurrentWorkAsRead& work)
    {
        append(" compiling"_s);
        size_t sizeBeforeFunction = m_size;
        append(' ');
        if (!appendFunction(work))
            m_size = sizeBeforeFunction;
        if (!work.step)
            return;
        append(" in "_s);
        append(work.step);
    }

private:
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

    void appendProgramText(std::span<const uint8_t> text)
    {
        constexpr size_t longestShown = 200;
        for (uint8_t byte : text.first(std::min(text.size(), longestShown)))
            append(byte < 0x20 || byte == 0x7f ? '?' : static_cast<char>(byte));
        if (text.size() > longestShown)
            append("..."_s);
    }

    static std::span<const uint8_t> stringOrEmpty(const Typed::Program& program, StringID id)
    {
        if (!id || id.index() >= program.strings.size())
            return { };
        return program.strings[id];
    }

    static std::span<const uint8_t> pathOrEmpty(const Typed::Program& program, ModuleID id)
    {
        if (!id || id.index() >= program.modules.size())
            return { };
        const Typed::Module& module = program.modules[id.index()];
        return stringOrEmpty(program, module.flags.contains(Typed::ModuleFlags::HasInstalledPositions) ? module.path : module.displayPath);
    }

    bool appendFunction(const CurrentWorkAsRead& work)
    {
        if (!work.program || !work.functionPlusOne)
            return false;
        const Typed::Program& program = *work.program;
        uint32_t index = work.functionPlusOne - 1;
        if (index >= program.functions.size())
            return false;
        const Typed::Function& function = program.functions[index];
        std::span<const uint8_t> path = pathOrEmpty(program, function.module);
        if (function.isModuleBody()) {
            if (path.empty())
                return false;
            appendProgramText(path);
            return true;
        }
        std::span<const uint8_t> name = stringOrEmpty(program, function.name);
        if (name.empty())
            append("a function"_s);
        else
            appendProgramText(name);
        if (path.empty())
            return true;
        append(" ("_s);
        appendProgramText(path);
        append(':');
        append(static_cast<uint64_t>(program.lineColumnFor(function.module, function.position).line));
        append(')');
        return true;
    }

    std::span<char> m_buffer;
    size_t m_size { 0 };
};

} // anonymous namespace

size_t describeCurrentThreadWork(std::span<char> buffer)
{
    const CurrentWork& record = *CurrentWorkInternal::recordOfThread;
    if (&record == &CurrentWorkInternal::sharedRecord)
        return 0;
    CurrentWorkAsRead work(record);
    if (work.isIdle())
        return 0;
    CurrentWorkText text(buffer);
    text.append("Stopped while"_s);
    text.appendWork(work);
    text.append(".\n"_s);
    return text.size();
}

size_t describeAllThreadsWork(std::span<char> buffer)
{
    CurrentWorkText text(buffer);
    for (const CurrentWork& record : CurrentWorkInternal::allCurrentWorkRecords()) {
        if (!record.isTaken.load(std::memory_order_acquire))
            continue;
        CurrentWorkAsRead work(record);
        if (!work.functionPlusOne)
            continue;
        text.append("Still"_s);
        text.appendWork(work);
        text.append(".\n"_s);
    }
    return text.size();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
