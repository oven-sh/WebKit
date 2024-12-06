#include "config.h"
#include <wtf/RunLoop.h>


namespace WTF {

// Functions exported by Timer.zig
extern "C" __attribute__((weak)) RunLoop::TimerBase::Bun__WTFTimer* WTFTimer__create(RunLoop::TimerBase*);
extern "C" __attribute__((weak)) void WTFTimer__update(RunLoop::TimerBase::Bun__WTFTimer*, double seconds, bool repeat);
extern "C" __attribute__((weak)) void WTFTimer__deinit(RunLoop::TimerBase::Bun__WTFTimer*);
extern "C" __attribute__((weak)) bool WTFTimer__isActive(const RunLoop::TimerBase::Bun__WTFTimer*);
extern "C" __attribute__((weak)) double WTFTimer__secondsUntilTimer(const RunLoop::TimerBase::Bun__WTFTimer*);
extern "C" __attribute__((weak)) void WTFTimer__cancel(RunLoop::TimerBase::Bun__WTFTimer*);

RunLoop::TimerBase::TimerBase(Ref<RunLoop>&& loop)
    : m_runLoop(WTFMove(loop))
    // check if the zig function is actually available (it won't be in JSC shell, since that doesn't
    // link Bun's zig code)
    , m_zigTimer(&WTFTimer__create ? WTFTimer__create(this) : nullptr)
{
}

RunLoop::TimerBase::~TimerBase()
{
    if (&WTFTimer__deinit) {
        ASSERT(m_zigTimer);
        WTFTimer__deinit(m_zigTimer);
    }
}

void RunLoop::TimerBase::stop() {
    if (&WTFTimer__cancel) {
        ASSERT(m_zigTimer);
        WTFTimer__cancel(m_zigTimer);
    }
}

bool RunLoop::TimerBase::isActive() const {
    if (&WTFTimer__isActive) {
        ASSERT(m_zigTimer);
        return WTFTimer__isActive(m_zigTimer);
    }
    return false;
}

Seconds RunLoop::TimerBase::secondsUntilFire() const {
    if (&WTFTimer__secondsUntilTimer) {
        ASSERT(m_zigTimer);
        return Seconds(WTFTimer__secondsUntilTimer(m_zigTimer));
    }
    return -1.0_s;
}

void RunLoop::TimerBase::start(Seconds interval, bool repeat) {
    if (&WTFTimer__update) {
        ASSERT(m_zigTimer);
        WTFTimer__update(m_zigTimer, interval.value(), repeat);
    }
}

extern "C" void WTFTimer__fire(RunLoop::TimerBase* timer) {
    timer->fired();
}

// probably more Bun-specific TimerBase methods

RunLoop::RunLoop()
{
}

RunLoop::~RunLoop()
{
}

void RunLoop::run() {
    ASSERT_NOT_REACHED();
}

void RunLoop::stop() {
    ASSERT_NOT_REACHED();
}

void RunLoop::wakeUp() {
    ASSERT_NOT_REACHED();
}

RunLoop::CycleResult RunLoop::cycle(RunLoopMode mode) {
    (void) mode;
    ASSERT_NOT_REACHED();
    return RunLoop::CycleResult::Stop;
}

} // namespace WTF
