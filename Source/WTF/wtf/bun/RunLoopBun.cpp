#include "config.h"
#include <wtf/RunLoop.h>

namespace WTF {

RunLoop::TimerBase::TimerBase(Ref<RunLoop>&& loop)
    : m_runLoop(WTFMove(loop))
{
}

RunLoop::TimerBase::~TimerBase()
{
}

void RunLoop::TimerBase::stop() {}

bool RunLoop::TimerBase::isActive() const {}

Seconds RunLoop::TimerBase::secondsUntilFire() const {}

void RunLoop::TimerBase::start(Seconds interval, bool repeat) {}

// probably more Bun-specific TimerBase methods

RunLoop::RunLoop()
{
}

RunLoop::~RunLoop() {}

void RunLoop::run() {}

void RunLoop::stop() {}

void RunLoop::wakeUp() {}

RunLoop::CycleResult RunLoop::cycle(RunLoopMode mode) {}

} // namespace WTF
