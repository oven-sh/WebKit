/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPhaseTimes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTStatistics.h"
#include "AOTVector.h"
#include <algorithm>
#include <mutex>
#include <stdio.h>
#include <wtf/CPUTime.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PageBlock.h>
#include <wtf/StringPrintStream.h>

#if OS(UNIX)
#include <sys/resource.h>
#endif

#if OS(LINUX) && __has_include(<linux/perf_event.h>)
#define AOT_HAS_INSTRUCTION_COUNTERS 1
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#define AOT_HAS_INSTRUCTION_COUNTERS 0
#endif

namespace JSC { namespace AOT {

std::atomic<bool> InstructionCounts::s_hasBegun { false };

static thread_local uint64_t t_instructionsSpentInsideLoops = 0;

void InstructionCounts::noteSpentInsideLoop(uint64_t instructions)
{
    t_instructionsSpentInsideLoops += instructions;
}

uint64_t InstructionCounts::spentInsideLoopsOnCurrentThread()
{
    return t_instructionsSpentInsideLoops;
}

#if AOT_HAS_INSTRUCTION_COUNTERS

namespace {

struct CountersOfThreads {
    Lock lock;
    Vector<int> files WTF_GUARDED_BY_LOCK(lock);
    uint64_t instructionsOfEndedThreads WTF_GUARDED_BY_LOCK(lock) { 0 };
    std::atomic<bool> isRefused { false };
};

CountersOfThreads& countersOfThreads()
{
    static LazyNeverDestroyed<CountersOfThreads> counters;
    static std::once_flag once;
    std::call_once(once, [] {
        counters.construct();
    });
    return counters;
}

std::optional<uint64_t> readCounterOfThread(int file)
{
    uint64_t count = 0;
    if (read(file, &count, sizeof(count)) != sizeof(count))
        return std::nullopt;
    return count;
}

class CounterOfThread {
    WTF_MAKE_NONCOPYABLE(CounterOfThread);
public:
    CounterOfThread()
    {
        CountersOfThreads& counters = countersOfThreads();
        perf_event_attr attributes { };
        attributes.size = sizeof(attributes);
        attributes.type = PERF_TYPE_HARDWARE;
        attributes.config = PERF_COUNT_HW_INSTRUCTIONS;
        attributes.exclude_kernel = 1;
        attributes.exclude_hv = 1;
        m_file = static_cast<int>(syscall(SYS_perf_event_open, &attributes, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
        if (m_file < 0) {
            counters.isRefused.store(true, std::memory_order_relaxed);
            return;
        }
#if CPU(X86_64)
        void* page = mmap(nullptr, pageSize(), PROT_READ, MAP_SHARED, m_file, 0);
        if (page != MAP_FAILED)
            m_page = static_cast<const perf_event_mmap_page*>(page);
#endif
        Locker locker { counters.lock };
        counters.files.append(m_file);
    }

    ~CounterOfThread()
    {
        if (m_file < 0)
            return;
        CountersOfThreads& counters = countersOfThreads();
        {
            Locker locker { counters.lock };
            counters.instructionsOfEndedThreads += readCounterOfThread(m_file).value_or(0);
            counters.files.removeFirst(m_file);
        }
        if (m_page)
            munmap(const_cast<perf_event_mmap_page*>(m_page), pageSize());
        close(m_file);
    }

    std::optional<uint64_t> read() const
    {
        if (m_file < 0)
            return std::nullopt;
        if (std::optional<uint64_t> count = readWithoutSystemCall())
            return count;
        return readCounterOfThread(m_file);
    }

private:
    std::optional<uint64_t> readWithoutSystemCall() const
    {
#if CPU(X86_64)
        if (!m_page)
            return std::nullopt;
        const volatile perf_event_mmap_page& page = *m_page;
        for (;;) {
            uint32_t sequence = page.lock;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            uint32_t index = page.index;
            int64_t count = page.offset;
            if (!page.cap_user_rdpmc || !index)
                return std::nullopt;
            unsigned unusedBits = 64 - page.pmc_width;
            uint32_t low;
            uint32_t high;
            asm volatile("rdpmc" : "=a"(low), "=d"(high) : "c"(index - 1));
            std::atomic_signal_fence(std::memory_order_seq_cst);
            if (page.lock != sequence)
                continue;
            uint64_t sinceOffset = static_cast<uint64_t>(high) << 32 | low;
            return static_cast<uint64_t>(count + (static_cast<int64_t>(sinceOffset << unusedBits) >> unusedBits));
        }
#else
        return std::nullopt;
#endif
    }

    int m_file { -1 };
    const perf_event_mmap_page* m_page { nullptr };
};

CounterOfThread& counterOfCurrentThread()
{
    WTF_THREAD_LOCAL_ALWAYS_DESTROY static thread_local CounterOfThread counter;
    return counter;
}

} // anonymous namespace

void InstructionCounts::countOnCurrentThread()
{
    counterOfCurrentThread();
}

std::optional<uint64_t> InstructionCounts::ofCurrentThread()
{
    return counterOfCurrentThread().read();
}

std::optional<uint64_t> InstructionCounts::ofAllThreads()
{
    CountersOfThreads& counters = countersOfThreads();
    if (counters.isRefused.load(std::memory_order_relaxed))
        return std::nullopt;
    Locker locker { counters.lock };
    uint64_t instructions = counters.instructionsOfEndedThreads;
    for (int file : counters.files)
        instructions += readCounterOfThread(file).value_or(0);
    return instructions;
}

#else

void InstructionCounts::countOnCurrentThread() { }
std::optional<uint64_t> InstructionCounts::ofCurrentThread() { return std::nullopt; }
std::optional<uint64_t> InstructionCounts::ofAllThreads() { return std::nullopt; }

#endif // AOT_HAS_INSTRUCTION_COUNTERS

static Seconds processCPUTime()
{
    auto time = CPUTime::get();
    return time ? time->userTime + time->systemTime : 0_s;
}

static Seconds processSystemTime()
{
    auto time = CPUTime::get();
    return time ? time->systemTime : 0_s;
}

size_t PhaseTimes::residentMegabytes()
{
#if OS(LINUX)
    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    FILE* file = fopen("/proc/self/statm", "r");
    if (!file)
        return 0;
    int fieldsRead = fscanf(file, "%lu %lu", &totalPages, &residentPages);
    fclose(file);
    return fieldsRead == 2 ? residentPages * pageSize() / MB : 0;
#else
    return 0;
#endif
}

static constinit Lock peakBeforeResetsLock;
static size_t peakBeforeResets WTF_GUARDED_BY_LOCK(peakBeforeResetsLock) { 0 };

PhaseTimes::PeakResidentMegabytes PhaseTimes::takePeakResidentMegabytes()
{
    Locker locker { peakBeforeResetsLock };
    size_t sinceLastTaken = takePeakResidentMegabytesSinceLastTaken();
#if OS(LINUX)
    size_t sinceLastReset = sinceLastTaken;
#else
    size_t sinceLastReset = peakResidentMegabytes();
#endif
    peakBeforeResets = std::max(sinceLastReset, peakBeforeResets);
    return { peakBeforeResets, sinceLastTaken };
}

size_t PhaseTimes::takePeakResidentMegabytesSinceLastTaken()
{
#if OS(LINUX)
    size_t peak = peakResidentMegabytes();
    if (FILE* file = fopen("/proc/self/clear_refs", "w")) {
        fputs("5", file);
        fclose(file);
    }
    return peak;
#else
    return 0;
#endif
}

size_t PhaseTimes::peakResidentMegabytes()
{
#if OS(LINUX)
    size_t peakKilobytes = 0;
    if (FILE* file = fopen("/proc/self/status", "r")) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            unsigned long value = 0;
            if (sscanf(line, "VmHWM: %lu kB", &value) == 1) {
                peakKilobytes = value;
                break;
            }
        }
        fclose(file);
    }
    return peakKilobytes / KB;
#elif OS(UNIX)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage))
        return 0;
#if OS(DARWIN)
    return usage.ru_maxrss / MB;
#else
    return usage.ru_maxrss / KB;
#endif
#else
    return 0;
#endif
}

PhaseTimes::PhaseTimes(BuildContext& build, ASCIILiteral name, std::optional<Stage> timedStage)
    : m_build(build)
    , m_name(name)
    , m_timedStage(timedStage)
    , m_start(MonotonicTime::now())
    , m_currentWork(name)
    , m_phaseStart(m_start)
{
    if (!m_build.options().logging.logsPhaseTimes) [[likely]]
        return;
    m_processCPUTimeAtPhaseStart = processCPUTime();
    InstructionCounts::begin();
    InstructionCounts::countOnCurrentThread();
    m_instructionsAtPhaseStart = InstructionCounts::ofAllThreads();
}

PhaseTimes::PhaseTimes(BuildContext& build, Stage stage)
    : PhaseTimes(build, nameOf(stage), stage)
{
}

PhaseTimes::PhaseTimes(BuildContext& build, ASCIILiteral name)
    : PhaseTimes(build, name, std::nullopt)
{
}

PhaseTimes::~PhaseTimes()
{
    if (m_timedStage)
        m_build.statistics().addTime(*m_timedStage, MonotonicTime::now() - m_start);
}

void PhaseTimes::didFinish(ASCIILiteral phase)
{
    if (!m_build.options().logging.logsPhaseTimes) [[likely]]
        return;
    Seconds wallTime = MonotonicTime::now() - m_phaseStart;
    Seconds cpuTime = processCPUTime() - m_processCPUTimeAtPhaseStart;
    std::optional<uint64_t> instructionsNow = InstructionCounts::ofAllThreads();
    PeakResidentMegabytes peak = takePeakResidentMegabytes();
    StringPrintStream line;
    line.print("AOT phase times: ", m_name, " | ", phase, " | wall ", wallTime.milliseconds(), " ms | cpu ", cpuTime.milliseconds(), " ms | resident ", residentMegabytes(), " MB | peak ", peak.ofProcess, " MB | peak since the last line ", peak.sinceLastTaken, " MB | in the kernel so far ", processSystemTime().milliseconds(), " ms");
    if (instructionsNow && m_instructionsAtPhaseStart)
        line.print(" | instructions ", static_cast<double>(*instructionsNow - *m_instructionsAtPhaseStart) / 1e6, " M");
    line.println();
    dataLog(line.toUTF8CString());
    m_instructionsAtPhaseStart = instructionsNow;
    m_phaseStart = MonotonicTime::now();
    m_processCPUTimeAtPhaseStart = processCPUTime();
}

StepTimes::StepTimes(const BuildContext& build, ASCIILiteral name)
    : m_logs(build.options().logging.logsPhaseTimes)
    , m_name(name)
    , m_stepStart(MonotonicTime::now())
    , m_threadCPUTimeAtStepStart(m_logs ? CPUTime::forCurrentThread() : Seconds())
    , m_instructionsOfThreadAtStepStart(m_logs ? InstructionCounts::ofCurrentThread() : std::nullopt)
    , m_instructionsInsideLoopsAtStepStart(InstructionCounts::spentInsideLoopsOnCurrentThread())
{
}

void StepTimes::didFinish(ASCIILiteral step)
{
    if (!m_logs) [[likely]]
        return;
    MonotonicTime now = MonotonicTime::now();
    Seconds threadCPUTime = CPUTime::forCurrentThread();
    std::optional<uint64_t> instructionsOfThread = InstructionCounts::ofCurrentThread();
    uint64_t instructionsInsideLoops = InstructionCounts::spentInsideLoopsOnCurrentThread();
    double instructionsOfStep = instructionsOfThread && m_instructionsOfThreadAtStepStart ? static_cast<double>(*instructionsOfThread - *m_instructionsOfThreadAtStepStart) : static_cast<double>(instructionsInsideLoops - m_instructionsInsideLoopsAtStepStart);
    dataLog(toUTF8CString("AOT phase times: ", m_name, " | ", step, " | wall ", (now - m_stepStart).milliseconds(), " ms | this thread ", (threadCPUTime - m_threadCPUTimeAtStepStart).milliseconds(), " ms | instructions of this thread ", instructionsOfStep / 1e6, " M | outside loops ", (instructionsOfStep - static_cast<double>(instructionsInsideLoops - m_instructionsInsideLoopsAtStepStart)) / 1e6, " M\n"));
    m_instructionsOfThreadAtStepStart = instructionsOfThread;
    m_instructionsInsideLoopsAtStepStart = instructionsInsideLoops;
    m_stepStart = now;
    m_threadCPUTimeAtStepStart = threadCPUTime;
}

void StepTimes::didFinish(ASCIILiteral step, const UTF8CString& note)
{
    if (!m_logs) [[likely]]
        return;
    MonotonicTime now = MonotonicTime::now();
    Seconds threadCPUTime = CPUTime::forCurrentThread();
    std::optional<uint64_t> instructionsOfThread = InstructionCounts::ofCurrentThread();
    uint64_t instructionsInsideLoops = InstructionCounts::spentInsideLoopsOnCurrentThread();
    double instructionsOfStep = instructionsOfThread && m_instructionsOfThreadAtStepStart ? static_cast<double>(*instructionsOfThread - *m_instructionsOfThreadAtStepStart) : static_cast<double>(instructionsInsideLoops - m_instructionsInsideLoopsAtStepStart);
    dataLog(toUTF8CString("AOT phase times: ", m_name, " | ", step, ": ", note, " | wall ", (now - m_stepStart).milliseconds(), " ms | this thread ", (threadCPUTime - m_threadCPUTimeAtStepStart).milliseconds(), " ms | instructions of this thread ", instructionsOfStep / 1e6, " M | outside loops ", (instructionsOfStep - static_cast<double>(instructionsInsideLoops - m_instructionsInsideLoopsAtStepStart)) / 1e6, " M\n"));
    m_instructionsOfThreadAtStepStart = instructionsOfThread;
    m_instructionsInsideLoopsAtStepStart = instructionsInsideLoops;
    m_stepStart = now;
    m_threadCPUTimeAtStepStart = threadCPUTime;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
