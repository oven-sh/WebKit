/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuildContext.h"

#if ENABLE(AOT)

#include "AOTHeaps.h"
#include "AOTIR.h"
#include "AOTIdentifierNumbering.h"
#include "AOTRegExpSummary.h"
#include "AOTScratchRealm.h"
#include "AOTVector.h"
#include "B3CompilationScratch.h"
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BuildContext);

class BuildContext::DeferredDestroyer {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DeferredDestroyer);
    WTF_MAKE_NONCOPYABLE(DeferredDestroyer);
public:
    DeferredDestroyer() = default;

    // Without a thread the value dies here.
    void enqueue(std::unique_ptr<DeferredValue> value)
    {
        Locker locker { m_lock };
        if (!m_thread) {
            m_thread = Thread::tryCreate("AOT destroyer"_s, [this] {
                destroyQueuedValues();
            });
        }
        if (!m_thread) [[unlikely]] {
            locker.unlockEarly();
            value = nullptr;
            return;
        }
        m_queue.append(WTF::move(value));
        m_condition.notifyOne();
    }

    // A value that dies on the thread may enqueue another, which starts a thread again.
    void finish()
    {
        while (true) {
            RefPtr<Thread> thread;
            {
                Locker locker { m_lock };
                thread = std::exchange(m_thread, nullptr);
                m_isStopping = !!thread;
            }
            if (!thread)
                return;
            m_condition.notifyAll();
            thread->waitForCompletion();
        }
    }

private:
    void destroyQueuedValues()
    {
        while (true) {
            std::unique_ptr<DeferredValue> value;
            {
                Locker locker { m_lock };
                m_condition.wait(m_lock, [this] {
                    assertIsHeld(m_lock);
                    return !m_queue.isEmpty() || m_isStopping;
                });
                if (m_queue.isEmpty())
                    return;
                value = m_queue.takeFirst();
            }
        }
    }

    Lock m_lock;
    WTF::Condition m_condition;
    Deque<std::unique_ptr<DeferredValue>> m_queue WTF_GUARDED_BY_LOCK(m_lock);
    bool m_isStopping WTF_GUARDED_BY_LOCK(m_lock) { false };
    RefPtr<Thread> m_thread WTF_GUARDED_BY_LOCK(m_lock);
};

BuildContext::BuildContext(const Wire::CompileOptions& wire, const HostThreadPool* hostPool)
    : m_options(CompileOptions::fromWire(wire, m_diagnostics))
    , m_assemblerPolicy(AssemblerPolicy::forImage(CPUFeatureSet::fromBits(static_cast<uint32_t>(m_options.recorded.targetFeatures))))
    , m_losses(Losses::Collected { .declines = m_options.collectsDeclines(), .allocationRemarks = m_options.collectsAllocationRemarks() })
    , m_parallelLoops(m_options.threadCount, hostPool)
    , m_backEndScratches(m_options.threadCount)
    , m_regExpSummaries(makeUnique<RegExpSummaries>(static_cast<unsigned>(m_options.limits[LimitID::RegExpNestingDepthWithCode])))
    , m_scratchRealms(makeUniqueWithoutFastMallocCheck<Typed::ScratchRealms>())
    , m_deferredDestroyer(makeUnique<DeferredDestroyer>())
{
    if (!m_diagnostics.hasErrors() && wire.threadCount > m_options.threadCount)
        gaveUp(LimitID::MaxWorkers);
}

BuildContext::~BuildContext()
{
    // Not reset(), which clears the pointer first: a value that dies on the destroyer thread may call destroyLater().
    m_deferredDestroyer->finish();
}

void BuildContext::finishDeferredDestruction()
{
    m_deferredDestroyer->finish();
}

namespace {

struct FunctionFilter {
    std::optional<uint32_t> id;
    std::optional<uint32_t> module;
    std::optional<uint32_t> position;
};

} // anonymous namespace

static std::optional<uint32_t> parseDecimal(StringView text)
{
    if (text.isEmpty() || !isASCIIDigit(text[0]))
        return std::nullopt;
    return parseInteger<uint32_t>(text, 10, WTF::ParseIntegerWhitespacePolicy::Disallow);
}

static std::optional<FunctionFilter> parseFunctionFilter(StringView text)
{
    if (text.startsWith('f')) {
        if (auto id = parseDecimal(text.substring(1)))
            return FunctionFilter { .id = id };
    }
    if (auto position = parseDecimal(text))
        return FunctionFilter { .position = position };

    size_t separator = text.find(':');
    if (separator == notFound)
        return std::nullopt;
    auto module = parseDecimal(text.left(separator));
    auto position = parseDecimal(text.substring(separator + 1));
    if (!module || !position)
        return std::nullopt;
    return FunctionFilter { .module = module, .position = position };
}

static bool matches(const FunctionFilter& filter, uint32_t index, const Typed::Function& function)
{
    return (!filter.id || *filter.id == index)
        && (!filter.module || *filter.module == function.module.index())
        && (!filter.position || *filter.position == function.position);
}

void BuildContext::resolveFunctionToDump(const Typed::Program& program)
{
    auto text = m_options.logging.functionToDump.span();
    if (text.empty())
        return;

    auto filter = parseFunctionFilter(StringView(byteCast<Latin1Character>(text)));
    // Several names are separated by '|'.
    auto isListed = [&](auto name) {
        for (auto rest = text;;) {
            size_t end = find(rest, '|');
            if (equalSpans(name, rest.first(std::min(end, rest.size()))))
                return true;
            if (end == notFound)
                return false;
            rest = rest.subspan(end + 1);
        }
    };
    auto isSelected = [&](uint32_t index) {
        const Typed::Function& function = program.functions[index];
        if (filter)
            return matches(*filter, index, function);
        return function.name && isListed(program.string(function.name));
    };

    uint32_t functionCount = program.functions.size();
    BitVector selected(functionCount);
    for (uint32_t index = 0; index < functionCount; ++index) {
        if (isSelected(index))
            selected.quickSet(index);
    }

    BitVector functionsToDump = selected;
    for (uint32_t index = 0; index < functionCount; ++index) {
        Typed::FunctionID source = findFunctionAsRead(&program, Typed::FunctionID(index));
        if (source && selected.get(source.index()))
            functionsToDump.quickSet(index);
    }
    m_functionsToDump = WTF::move(functionsToDump);
}

bool BuildContext::exceeds(LimitID id, uint64_t amount, const Subject& subject)
{
    if (amount <= limit(id))
        return false;
    gaveUp(id, subject);
    return true;
}

GaveUp BuildContext::gaveUp(LimitID id, const Subject& subject)
{
    RELEASE_ASSERT(id != LimitID::None);
    if (!m_recordsLimitHits)
        return { id };
    m_statistics.countHit(id);
    m_losses.addLimitHit(id, subject);
    return { id };
}

void BuildContext::reportLimitExceeded(LimitID id, uint64_t needed, const Subject& subject)
{
    gaveUp(id, subject);
    m_diagnostics.add(UserDiagnostic {
        .kind = UserDiagnosticKind::LimitExceeded,
        .function = subject.function,
        .block = subject.block,
        .value = subject.value,
        .layout = subject.layout,
        .limit = id,
        .count = clampTo<uint32_t>(needed),
        .location = subject.location,
    });
}

const IdentifierNumbering& BuildContext::identifierNumbering() const
{
    RELEASE_ASSERT(m_identifierNumbering);
    return *m_identifierNumbering;
}

B3::CompilationScratch& BuildContext::backEndScratch(unsigned worker)
{
    RELEASE_ASSERT(worker < m_backEndScratches.size());
    std::unique_ptr<B3::CompilationScratch>& scratch = m_backEndScratches[worker];
    if (!scratch)
        scratch = makeUnique<B3::CompilationScratch>(&createAOTHeaps);
    return *scratch;
}

void BuildContext::destroyBackEndScratches()
{
    forEachIndexInParallel(m_parallelLoops, m_backEndScratches.size(), [&](unsigned worker) {
        m_backEndScratches[worker] = nullptr;
    });
}

void BuildContext::setIdentifierNumbering(IdentifierNumbering&& numbering)
{
    RELEASE_ASSERT(!m_identifierNumbering);
    m_identifierNumbering = makeUniqueWithoutFastMallocCheck<IdentifierNumbering>(WTF::move(numbering));
}

void BuildContext::enqueueDeferredValue(std::unique_ptr<DeferredValue> value)
{
    // The value dies here, on the caller: a build with one thread starts no other.
    if (threadCount() == 1)
        return;
    m_deferredDestroyer->enqueue(WTF::move(value));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
