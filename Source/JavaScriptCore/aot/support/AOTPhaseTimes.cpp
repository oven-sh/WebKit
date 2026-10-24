/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPhaseTimes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTVector.h"
#include <mutex>
#include <stdio.h>
#include <wtf/CPUTime.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PageBlock.h>
#include <wtf/StringPrintStream.h>

#if OS(LINUX) && CPU(X86_64) && __has_include(<linux/perf_event.h>)
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

std::optional<uint64_t> readWithSystemCall(int file)
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
        void* page = mmap(nullptr, pageSize(), PROT_READ, MAP_SHARED, m_file, 0);
        if (page != MAP_FAILED)
            m_page = static_cast<const volatile perf_event_mmap_page*>(page);
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
            counters.instructionsOfEndedThreads += readWithSystemCall(m_file).value_or(0);
            counters.files.removeFirst(m_file);
        }
        if (m_page)
            munmap(const_cast<perf_event_mmap_page*>(m_page), pageSize());
        close(m_file);
    }

    // As perf_event_open(2) says it: the kernel changes `lock` around every change of the page.
    std::optional<uint64_t> read() const
    {
        if (m_file < 0)
            return std::nullopt;
        while (m_page && m_page->cap_user_rdpmc) {
            uint32_t sequence = m_page->lock;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            uint32_t index = m_page->index;
            int64_t count = m_page->offset;
            // The counter is not on the processor at this moment.
            if (!index)
                break;
            unsigned unusedBits = 64 - m_page->pmc_width;
            count += static_cast<int64_t>(__builtin_ia32_rdpmc(index - 1) << unusedBits) >> unusedBits;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            if (m_page->lock == sequence)
                return static_cast<uint64_t>(count);
        }
        return readWithSystemCall(m_file);
    }

private:
    int m_file { -1 };
    const volatile perf_event_mmap_page* m_page { nullptr };
};

const CounterOfThread& counterOfCurrentThread()
{
    static thread_local CounterOfThread counter;
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
        instructions += readWithSystemCall(file).value_or(0);
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

static Seconds serialCPUTimeOfCurrentThread()
{
    return CPUTime::forCurrentThread() - ParallelRegionScope::cpuTimeOfCurrentThread();
}

static size_t residentMegabytes()
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

PhaseTimes::PhaseTimes(BuildContext& build, ASCIILiteral namePrefix, ASCIILiteral name, std::optional<Stage> timedStage, PassID pass)
    : m_build(build)
    , m_namePrefix(namePrefix)
    , m_name(name)
    , m_timedStage(timedStage)
    , m_start(MonotonicTime::now())
    , m_currentWork(timedStage, pass, m_start)
    , m_phaseStart(m_start)
{
    if (m_build.recordedOptions().sabotage) [[unlikely]]
        stopWhereSabotaged(m_build, timedStage, pass);
    if (!m_build.options().logging.logsPhaseTimes) [[likely]]
        return;
    m_processCPUTimeAtPhaseStart = processCPUTime();
    m_serialCPUTimeAtPhaseStart = serialCPUTimeOfCurrentThread();
    BusyThreads::countCurrentThread();
    m_busyThreadsTimesAtPhaseStart = BusyThreads::times();
    InstructionCounts::begin();
    InstructionCounts::countOnCurrentThread();
    m_instructionsAtPhaseStart = InstructionCounts::ofAllThreads();
}

PhaseTimes::PhaseTimes(BuildContext& build, Stage stage)
    : PhaseTimes(build, ""_s, nameOf(stage), stage, PassID::None)
{
}

PhaseTimes::PhaseTimes(BuildContext& build, PassID pass)
    : PhaseTimes(build, "pass "_s, nameOf(pass), std::nullopt, pass)
{
    ASSERT(pass != PassID::None);
}

PhaseTimes::~PhaseTimes()
{
    if (m_timedStage)
        m_build.statistics().addTime(*m_timedStage, MonotonicTime::now() - m_start);
}

void PhaseTimes::didFinish(ASCIILiteral phase)
{
    currentWork().setFinishedPart(phase);
    if (!m_build.options().logging.logsPhaseTimes) [[likely]]
        return;
    Seconds wallTime = MonotonicTime::now() - m_phaseStart;
    Seconds cpuTime = processCPUTime() - m_processCPUTimeAtPhaseStart;
    Seconds serialCPUTime = serialCPUTimeOfCurrentThread() - m_serialCPUTimeAtPhaseStart;
    StringPrintStream instructions;
    std::optional<uint64_t> instructionsNow = InstructionCounts::ofAllThreads();
    if (instructionsNow && m_instructionsAtPhaseStart)
        instructions.print(" | instructions ", static_cast<double>(*instructionsNow - *m_instructionsAtPhaseStart) / 1e6, " M");
    m_instructionsAtPhaseStart = instructionsNow;
    BusyThreads::Times busyThreadsTimes = BusyThreads::times();
    StringPrintStream busyThreads;
    unsigned minThreads = 0;
    for (unsigned bucket = 0; bucket < BusyThreads::numberOfBuckets; ++bucket) {
        busyThreads.print(bucket ? ", " : "", minThreads);
        if (bucket + 1 < BusyThreads::numberOfBuckets)
            busyThreads.print("-", BusyThreads::maxThreadsOfBucket[bucket]);
        else
            busyThreads.print("+");
        busyThreads.print(": ", (busyThreadsTimes[bucket] - m_busyThreadsTimesAtPhaseStart[bucket]).milliseconds());
        minThreads = BusyThreads::maxThreadsOfBucket[bucket] + 1;
    }
    m_busyThreadsTimesAtPhaseStart = busyThreadsTimes;
    dataLogLn("AOT phase times: ", m_namePrefix, m_name, " | ", phase, " | wall ", wallTime.milliseconds(), " ms | cpu ", cpuTime.milliseconds(), " ms | resident ", residentMegabytes(), " MB | in the kernel so far ", processSystemTime().milliseconds(), " ms", instructions.toString(), " | outside parallel loops ", serialCPUTime.milliseconds(), " ms | ms by busy threads ", busyThreads.toString());
    m_serialCPUTimeAtPhaseStart = serialCPUTimeOfCurrentThread();
    m_phaseStart = MonotonicTime::now();
    m_processCPUTimeAtPhaseStart = processCPUTime();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
