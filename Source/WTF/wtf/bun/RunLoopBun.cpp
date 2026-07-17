#include "config.h"
#include <wtf/RunLoop.h>

// We need the definition of RunLoop::TimerBase::ScheduledTask, as well as the definitions of
// RunLoopGeneric member functions (which we redefine to be on RunLoopGenericState instead)
#include "../generic/RunLoopGeneric.cpp"

// Ensure we cleaned up the mess in RunLoopGeneric.cpp
#if defined(RunLoop)
#error RunLoop was not undef'd
#elif defined(m_runLoop)
#error m_runLoop was not undef'd
#elif defined(m_scheduledTask)
#error m_scheduledTask was not undef'd
#endif

namespace WTF {

// Functions exported by Timer.zig
extern "C" __attribute__((weak)) RunLoop::TimerBase::Bun__WTFTimer* WTFTimer__create(RunLoop::TimerBase*);
extern "C" __attribute__((weak)) void WTFTimer__update(RunLoop::TimerBase::Bun__WTFTimer*, double seconds, bool repeat);
extern "C" __attribute__((weak)) void WTFTimer__deinit(RunLoop::TimerBase::Bun__WTFTimer*);
extern "C" __attribute__((weak)) bool WTFTimer__isActive(const RunLoop::TimerBase::Bun__WTFTimer*);
extern "C" __attribute__((weak)) double WTFTimer__secondsUntilTimer(const RunLoop::TimerBase::Bun__WTFTimer*);
// Returns whether the timer was still armed in Bun's wtf_timers heap (i.e.
// fired() has not been and will not be dispatched for it).
extern "C" __attribute__((weak)) bool WTFTimer__cancel(RunLoop::TimerBase::Bun__WTFTimer*);

// Weak, so that Bun can override it
extern "C" __attribute__((weak)) bool Bun__thisThreadHasVM();

// Default definition for the JSC shell. Returning false will make us use a RunLoopGeneric which
// works when Bun's event loop is not active.
bool Bun__thisThreadHasVM()
{
    // Bun should override this function, so we should only reach here if we are *not* running in
    // Bun in which case all the WTFTimer functions should not be defined
    ASSERT(!WTFTimer__create);
    ASSERT(!WTFTimer__update);
    ASSERT(!WTFTimer__deinit);
    ASSERT(!WTFTimer__isActive);
    ASSERT(!WTFTimer__secondsUntilTimer);
    ASSERT(!WTFTimer__cancel);
    return false;
}

RunLoop::TimerBase::TimerBase(Ref<RunLoop>&& loop, ASCIILiteral description)
    : m_runLoop(WTF::move(loop))
    , m_description(description)
    // We need to init this here since there's no default constructor. We init with a nullptr
    // WTFTimer. The body of the constructor always initializes it with a proper value.
    , m_impl(NullWTFTimer)
{
    switch (m_runLoop->kind()) {
    case Kind::Generic:
        m_impl.emplace<Ref<ScheduledTask>>(ScheduledTask::create(*this));
        break;
    case Kind::Bun:
        auto* maybe_timer = WTFTimer__create(this);
        if (maybe_timer) {
            m_impl.emplace<std::reference_wrapper<Bun__WTFTimer>>(std::ref(*maybe_timer));
        } else {
            m_impl.emplace<NullWTFTimerTag>(NullWTFTimer);
        }
        break;
    }
}

// Bun might start a JSC::VM without intending to create a Timer.
// An example case is bytecode caching.
// In such cases, we should avoid calling the function.

RunLoop::TimerBase::~TimerBase()
{
    switch (kind()) {
    case Kind::Generic:
        destructGeneric();
        break;
    case Kind::Bun:
        if (auto* ref = std::get_if<std::reference_wrapper<Bun__WTFTimer>>(&m_impl)) {
            WTFTimer__deinit(&ref->get());
        }
        break;
    }
}

void RunLoop::TimerBase::stop()
{
    switch (kind()) {
    case Kind::Generic:
        return stopGeneric();
    case Kind::Bun:
        if (auto* ref = std::get_if<std::reference_wrapper<Bun__WTFTimer>>(&m_impl)) {
            if (WTFTimer__cancel(&ref->get()))
                didStopWhileActive();
        }
        return;
    }
}

bool RunLoop::TimerBase::isActive() const
{
    switch (kind()) {
    case Kind::Generic:
        return isActiveGeneric();
    case Kind::Bun:
        if (auto* ref = std::get_if<std::reference_wrapper<Bun__WTFTimer>>(&m_impl)) {
            return WTFTimer__isActive(&ref->get());
        }
        return false;
    }
}

Seconds RunLoop::TimerBase::secondsUntilFire() const
{
    switch (kind()) {
    case Kind::Generic:
        return secondsUntilFireGeneric();
    case Kind::Bun:
        if (auto* ref = std::get_if<std::reference_wrapper<Bun__WTFTimer>>(&m_impl)) {
            return Seconds(WTFTimer__secondsUntilTimer(&ref->get()));
        }
        return -1.0_s;
    }
}

void RunLoop::TimerBase::start(Seconds interval, bool repeat)
{
    switch (kind()) {
    case Kind::Generic:
        return startGeneric(interval, repeat);
    case Kind::Bun:
        if (auto* ref = std::get_if<std::reference_wrapper<Bun__WTFTimer>>(&m_impl)) {
            WTFTimer__update(&ref->get(), interval.value(), repeat);
        }
        return;
    }
}

extern "C" void WTFTimer__fire(RunLoop::TimerBase* timer)
{
    timer->fired();
}

// probably more Bun-specific TimerBase methods

RunLoop::RunLoop()
{
    bool useGeneric = WTFTimer__create
        // Bun function is defined, so we're in Bun, and the main Bun thread should always use the
        // Bun RunLoop even though it's created when the VM doesn't exist yet
        ? !(isMainThread() || Bun__thisThreadHasVM())
        // We're not Bun
        : true;
    if (useGeneric) {
        m_genericState.emplace(*this);
    } else {
        // these functions should all be defined if we're in Bun
        ASSERT(WTFTimer__create);
        ASSERT(WTFTimer__update);
        ASSERT(WTFTimer__deinit);
        ASSERT(WTFTimer__isActive);
        ASSERT(WTFTimer__secondsUntilTimer);
        ASSERT(WTFTimer__cancel);
    }
}

RunLoop::~RunLoop()
{
    // if m_genericState has a value, ~RunLoopGenericState() will be called, so we don't need
    // to do anything else here
}

void RunLoop::run()
{
    switch (RunLoop::currentSingleton().kind()) {
    case Kind::Generic:
        // matches RunLoopGeneric implementation
        RunLoop::currentSingleton().m_genericState->runImpl(RunLoopGenericState::RunMode::Drain);
        return;
    case Kind::Bun:
        // Our event loop should not call this function
        ASSERT_NOT_REACHED();
    }
}

void RunLoop::stop()
{
    switch (kind()) {
    case Kind::Generic:
        m_genericState->stop();
        return;
    case Kind::Bun:
        // Our event loop should not call this function
        ASSERT_NOT_REACHED();
    }
}

void RunLoop::wakeUp()
{
    switch (kind()) {
    case Kind::Generic:
        m_genericState->wakeUp();
        return;
    case Kind::Bun:
        // Do nothing. This means that JSRunLoopTimer::Manager::PerVMData's RunLoop::Timer leaks instead
        // of being freed.
    }
}

RunLoop::CycleResult RunLoop::cycle(RunLoopMode)
{
    switch (RunLoop::currentSingleton().kind()) {
    case Kind::Generic:
        // matches RunLoopGeneric implementation
        RunLoop::currentSingleton().m_genericState->runImpl(RunLoopGenericState::RunMode::Iterate);
        return CycleResult::Continue;
    case Kind::Bun:
        // Our event loop should not call this function
        ASSERT_NOT_REACHED();
        return RunLoop::CycleResult::Stop;
    }
}

} // namespace WTF
