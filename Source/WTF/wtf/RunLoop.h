/*
 * Copyright (C) 2010-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
 * Portions Copyright (c) 2010 Motorola Mobility, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <functional>
#include <wtf/CheckedPtr.h>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Forward.h>
#include <wtf/FunctionDispatcher.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Observer.h>
#include <wtf/RetainPtr.h>
#include <wtf/Seconds.h>
#include <wtf/ThreadSafetyAnalysis.h>
#include <wtf/ThreadingEnums.h>
#include <wtf/ThreadingPrimitives.h>
#include <wtf/TypeTraits.h>
#include <wtf/Variant.h>
#include <wtf/WeakHashSet.h>
#include <wtf/text/WTFString.h>

#if USE(CF)
#include <CoreFoundation/CFRunLoop.h>
#endif

#if USE(GLIB_EVENT_LOOP)
#include <wtf/OptionCountedSet.h>
#include <wtf/glib/GRefPtr.h>
#endif

// BUN_EVENT_LOOP ends up compiling much of the generic code too, so we need this include
#if USE(GENERIC_EVENT_LOOP) || USE(BUN_EVENT_LOOP)
#include <wtf/RedBlackTree.h>
#endif

namespace WTF {

template<typename, CanBeGCThread> class ThreadSpecific;

#if USE(GLIB_EVENT_LOOP)
class ActivityObserver;
#endif

#if USE(COCOA_EVENT_LOOP)
class SchedulePair;
struct SchedulePairHash;
using SchedulePairHashSet = HashSet<Ref<SchedulePair>, SchedulePairHash>;
#endif

#if USE(CF)
using RunLoopMode = CFStringRef;
#define DefaultRunLoopMode kCFRunLoopDefaultMode
#else
using RunLoopMode = unsigned;
#define DefaultRunLoopMode 0
#endif

#if !PLATFORM(COCOA)
// Classes that offer Timers should be ref-counted of CanMakeCheckedPtr. Please do not add new exceptions.
template<typename T> struct IsDeprecatedTimerSmartPointerException : std::false_type { };
#endif

// WTF::RunLoop is designed to have only one implementation compiled in, depending on build
// configuration. Bun would like to use either the implementation for GENERIC_EVENT_LOOP, on threads
// that do not have a virtual machine (because these threads don't have a Bun event loop), or a Bun-
// specific implementation that stubs almost all functionality except that needed for JSRunLoopTimer
// (we integrate those timers into our event loop so that we do GC work at the times JSC wants to).
// RunLoop has some fields that always exist, and some fields that are enabled depending on which
// kind of RunLoop is being used. Our solution is to, when USE(BUN_EVENT_LOOP) is true, move all the
// fields specific to GENERIC_EVENT_LOOP into a separate class (RunLoop::RunLoopGenericState), and
// then give RunLoop a std::optional of that class. When a RunLoop is created, if there is a Bun VM
// on its thread, we use the stub implementation; otherwise, we initialize m_genericState and use
// those functions.
//
// Since RunLoopGeneric.cpp is not compiled in when building for Bun, we #include it in RunLoop.cpp
// and use some #defines to make it define member functions of RunLoop::RunLoopGenericState instead
// of RunLoop.

class WTF_CAPABILITY("is current") RunLoop final : public GuaranteedSerialFunctionDispatcher {
    WTF_MAKE_NONCOPYABLE(RunLoop);
public:
    // Must be called from the main thread.
    WTF_EXPORT_PRIVATE static void initializeMain();
#if USE(WEB_THREAD)
    WTF_EXPORT_PRIVATE static void initializeWeb();
#endif

    WTF_EXPORT_PRIVATE static RunLoop& currentSingleton();
    WTF_EXPORT_PRIVATE static RunLoop& NODELETE mainSingleton();
#if USE(WEB_THREAD)
    WTF_EXPORT_PRIVATE static RunLoop& webSingleton();
    WTF_EXPORT_PRIVATE static RunLoop* webIfExists();
#endif
    WTF_EXPORT_PRIVATE static Ref<RunLoop> create(ASCIILiteral threadName, ThreadType = ThreadType::Unknown, ThreadQOS = ThreadQOS::UserInitiated);

    static bool isMain() { return mainSingleton().isCurrent(); }
    WTF_EXPORT_PRIVATE bool isCurrent() const final;
    WTF_EXPORT_PRIVATE ~RunLoop() final;

    WTF_EXPORT_PRIVATE void dispatch(Function<void()>&&) final;
#if USE(COCOA_EVENT_LOOP)
    WTF_EXPORT_PRIVATE static void dispatch(const SchedulePairHashSet&, Function<void()>&&);
#endif

    WTF_EXPORT_PRIVATE static void run();
    WTF_EXPORT_PRIVATE void stop();
    WTF_EXPORT_PRIVATE void wakeUp();

    WTF_EXPORT_PRIVATE void suspendFunctionDispatchForCurrentCycle();

    enum class CycleResult { Continue, Stop };
    WTF_EXPORT_PRIVATE CycleResult static cycle(RunLoopMode = DefaultRunLoopMode);

    WTF_EXPORT_PRIVATE void threadWillExit();

    enum class Activity : uint8_t {
        BeforeWaiting   = 1 << 0,
        Entry           = 1 << 1,
        Exit            = 1 << 2,
        AfterWaiting    = 1 << 3
    };

#if USE(GLIB_EVENT_LOOP)
    WTF_EXPORT_PRIVATE GMainContext* mainContext() const { return m_mainContext.get(); }

    // Event observers (only used for PageTimelineAgent)
    enum class Event : bool {
        WillDispatch,
        DidDispatch
    };

    using EventObserver = Observer<void(Event, const String&)>;
    WTF_EXPORT_PRIVATE void observeEvent(const EventObserver&);
#endif

#if USE(GENERIC_EVENT_LOOP) || USE(WINDOWS_EVENT_LOOP)
    WTF_EXPORT_PRIVATE static void setWakeUpCallback(WTF::Function<void()>&&);
#endif

#if USE(WINDOWS_EVENT_LOOP)
    using WindowsMessageHandler = Function<bool(MSG&)>;
    WTF_EXPORT_PRIVATE static void setWindowsMessageHandler(WindowsMessageHandler&&);
    static void registerRunLoopMessageWindowClass();
#endif

#if USE(BUN_EVENT_LOOP)
    enum class Kind { Generic, Bun };
    WTF_EXPORT_PRIVATE Kind kind() const { return m_genericState ? Kind::Generic : Kind::Bun; }
#endif

    // A RunLoop::Timer is owned by the thread whose run loop it is constructed with: it fires on that
    // run loop's thread, and stop() and the destructor must run on that thread. Stopping or destroying
    // a timer from another thread races with an in-flight callback and is a use-after-free; both assert
    // RunLoop::isCurrent() in debug builds (see assertIsCurrent()). Starting/re-arming a timer from
    // another thread is allowed -- it only schedules onto the run loop and never frees the timer -- which
    // is how RunLoop::dispatch()/dispatchAfter() and cross-thread timer schedulers (e.g. JSRunLoopTimer)
    // work.
    class TimerBase {
        friend class RunLoop;
    public:
        WTF_EXPORT_PRIVATE explicit TimerBase(Ref<RunLoop>&&, ASCIILiteral description);
        // Must run on the run loop's thread if the timer is active (asserted in debug); see class comment.
        WTF_EXPORT_PRIVATE virtual ~TimerBase();

        // May be called from any thread; (re)schedules the timer onto its run loop's thread.
        void startRepeating(Seconds interval) { start(std::max(interval, 0_s), true); }
        void startOneShot(Seconds interval) { start(std::max(interval, 0_s), false); }

        // Must be called on the run loop's thread when the timer is active (asserted in debug).
        WTF_EXPORT_PRIVATE void stop();

        WTF_EXPORT_PRIVATE bool isActive() const;
        WTF_EXPORT_PRIVATE Seconds secondsUntilFire() const;

        virtual void fired() = 0;

#if USE(GLIB_EVENT_LOOP)
        WTF_EXPORT_PRIVATE void setPriority(int);
#elif USE(BUN_EVENT_LOOP)
        // WTFTimer in Timer.zig
        struct Bun__WTFTimer;
#endif

        ASCIILiteral description() const { return m_description; }

    private:
        WTF_EXPORT_PRIVATE void start(Seconds interval, bool repeat);

        const Ref<RunLoop> m_runLoop;
        ASCIILiteral m_description;

#if USE(WINDOWS_EVENT_LOOP)
        bool isActiveWithLock() const WTF_REQUIRES_LOCK(m_runLoop->m_loopLock);
        void timerFired();
        MonotonicTime m_nextFireDate;
        Seconds m_interval;
        bool m_isRepeating { false };
        bool m_isActive { false };
#elif USE(COCOA_EVENT_LOOP)
        RetainPtr<CFRunLoopTimerRef> m_timer;
#elif USE(GLIB_EVENT_LOOP)
        void updateReadyTime();
        GRefPtr<GSource> m_source;
        bool m_isRepeating { false };
        Seconds m_interval { 0 };
#elif USE(GENERIC_EVENT_LOOP) || USE(BUN_EVENT_LOOP)
#if USE(BUN_EVENT_LOOP)
        bool isActiveWithLock() const WTF_REQUIRES_LOCK(m_runLoop->m_genericState->m_loopLock);
        void stopWithLock() WTF_REQUIRES_LOCK(m_runLoop->m_genericState->m_loopLock);
#else
        bool isActiveWithLock() const WTF_REQUIRES_LOCK(m_runLoop->m_loopLock);
        void stopWithLock() WTF_REQUIRES_LOCK(m_runLoop->m_loopLock);
#endif

        class ScheduledTask;
#if USE(GENERIC_EVENT_LOOP)
        const Ref<ScheduledTask> m_scheduledTask;
#endif
#endif
#if USE(BUN_EVENT_LOOP)
        // These functions will be defined in RunLoopGeneric.cpp.
        // RunLoopBun.cpp defines non-`Generic` versions which choose which implementation to use
        // based on m_isGenericTimer.
        // I could have also done something like RunLoopGenericState, and moved the Generic-specific
        // fields into another class, but since there are so few TimerBase member functions and
        // fields I decided it would be less intrusive to rename the functions and include both the
        // Generic field and the Bun field.
        void destructGeneric();
        void startGeneric(Seconds interval, bool repeat);
        void stopGeneric();
        bool isActiveGeneric() const;
        Seconds secondsUntilFireGeneric() const;

        inline Kind kind() const { return std::holds_alternative<Ref<ScheduledTask>>(m_impl) ? Kind::Generic : Kind::Bun; }

        // Bun__WTFTimer reference for Bun implementation, Ref<ScheduledTask> for generic implementation.
        // The Bun__WTFTimer pointer could be null if (in Bun)
        // VirtualMachine.is_bundler_thread_for_bytecode_cache is true. In that case we use a
        // special tag.
        enum NullWTFTimerTag { NullWTFTimer };
        Variant<Ref<ScheduledTask>, std::reference_wrapper<Bun__WTFTimer>, NullWTFTimerTag> m_impl;
#endif
    };

    class Timer : public TimerBase {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Timer);
    public:
        template <typename TimerFiredClass>
        requires (WTF::HasThreadSafeWeakPtrFunctions<TimerFiredClass>::value)
        Timer(Ref<RunLoop>&& runLoop, ASCIILiteral description, TimerFiredClass* object, void (TimerFiredClass::*function)())
            : Timer(WTF::move(runLoop), description, [weakObject = ThreadSafeWeakPtr { *object }, function] {
                if (RefPtr object = weakObject.get())
                    (object.get()->*function)();
            })
        {
        }

        template <typename TimerFiredClass>
        requires (!WTF::HasThreadSafeWeakPtrFunctions<TimerFiredClass>::value && WTF::HasWeakPtrFunctions<TimerFiredClass>::value && WTF::HasRefPtrMemberFunctions<TimerFiredClass>::value)
        Timer(Ref<RunLoop>&& runLoop, ASCIILiteral description, TimerFiredClass* object, void (TimerFiredClass::*function)())
            : Timer(WTF::move(runLoop), description, [weakObject = WeakPtr { *object }, function] {
                if (RefPtr object = weakObject.get())
                    (object.get()->*function)();
            })
        {
        }

        template <typename TimerFiredClass>
        requires (!WTF::HasThreadSafeWeakPtrFunctions<TimerFiredClass>::value && WTF::HasWeakPtrFunctions<TimerFiredClass>::value && !WTF::HasRefPtrMemberFunctions<TimerFiredClass>::value && WTF::HasCheckedPtrMemberFunctions<TimerFiredClass>::value)
        Timer(Ref<RunLoop>&& runLoop, ASCIILiteral description, TimerFiredClass* object, void (TimerFiredClass::*function)())
            : Timer(WTF::move(runLoop), description, [weakObject = WeakPtr { *object }, function] {
                if (CheckedPtr object = weakObject)
                    (object.get()->*function)();
            })
        {
        }

        template <typename TimerFiredClass>
        requires (!WTF::HasThreadSafeWeakPtrFunctions<TimerFiredClass>::value && !WTF::HasWeakPtrFunctions<TimerFiredClass>::value && WTF::HasCheckedPtrMemberFunctions<TimerFiredClass>::value)
        Timer(Ref<RunLoop>&& runLoop, ASCIILiteral description, TimerFiredClass* object, void (TimerFiredClass::*function)())
            : Timer(WTF::move(runLoop), description, [object = CheckedRef { *object }, function] {
                (object.ptr()->*function)();
            })
        {
        }

        Timer(Ref<RunLoop>&& runLoop, ASCIILiteral description, Function<void ()>&& function)
            : TimerBase(WTF::move(runLoop), description)
            , m_function(WTF::move(function))
        {
        }

#if !PLATFORM(COCOA)
        // FIXME: This constructor isn't as safe as the other ones and should be removed.
        template <typename TimerFiredClass>
        requires (!WTF::HasRefPtrMemberFunctions<TimerFiredClass>::value && !WTF::HasCheckedPtrMemberFunctions<TimerFiredClass>::value)
        Timer(Ref<RunLoop>&& runLoop, ASCIILiteral description, TimerFiredClass* object, void (TimerFiredClass::*function)())
            : Timer(WTF::move(runLoop), description, std::bind(function, object))
        {
            static_assert(IsDeprecatedTimerSmartPointerException<TimerFiredClass>::value, "Classes using RunLoop::Timer should either be RefCounted or CanMakeCheckedPtr");
        }
#endif

    private:
        void fired() override { m_function(); }

        Function<void()> m_function;
    };

    class DispatchTimer final : public TimerBase, public ThreadSafeRefCounted<DispatchTimer> {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(DispatchTimer);
    public:
        DispatchTimer(RunLoop& runLoop)
            : TimerBase(runLoop, "DispatchTimer"_s)
        {
        }

        void setFunction(Function<void()>&& function)
        {
            m_function = WTF::move(function);
        }
    private:
        void fired() final { m_function(); }

        Function<void()> m_function;
    };

    WTF_EXPORT_PRIVATE Ref<RunLoop::DispatchTimer> dispatchAfter(Seconds, Function<void()>&&);

    WTF_EXPORT_PRIVATE String listActiveTimersForLogging() const;

private:
    class Holder;
    static ThreadSpecific<Holder, CanBeGCThread::False>& runLoopHolder();

    RunLoop();

    void performWork();

    void registerTimer(TimerBase&);
    void unregisterTimer(TimerBase&);

#if ENABLE(UNFAIR_LOCK)
    mutable UnfairLock m_registeredTimerLock;
#else
    mutable Lock m_registeredTimerLock;
#endif
    HashSet<TimerBase *> m_registeredTimers;

    Deque<Function<void()>> m_currentIteration;

#if ENABLE(UNFAIR_LOCK)
    UnfairLock m_nextIterationLock;
#else
    Lock m_nextIterationLock;
#endif
    Deque<Function<void()>> m_nextIteration WTF_GUARDED_BY_LOCK(m_nextIterationLock);

    bool m_isFunctionDispatchSuspended { false };
    bool m_hasSuspendedFunctions { false };

#if USE(WINDOWS_EVENT_LOOP)
    static LRESULT CALLBACK RunLoopWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void dispatchMessage(MSG&);
    DWORD msTillNextTimer();
    void fireTimers();
    HWND m_runLoopMessageWindow;
    Deque<TimerBase*> m_timers;

    Lock m_loopLock;
    WindowsMessageHandler m_windowsMessageHandler;
#elif USE(COCOA_EVENT_LOOP)
    static void performWork(void*);
    const RetainPtr<CFRunLoopRef> m_runLoop;
    const RetainPtr<CFRunLoopSourceRef> m_runLoopSource;
#elif USE(GLIB_EVENT_LOOP)
    void notifyEvent(Event, const char*);
    void notifyActivity(Activity);

    void runGLibMainLoop();

    enum class MayBlock { Yes, No };
    void runGLibMainLoopIteration(MayBlock);

    friend class ActivityObserver;
    WTF_EXPORT_PRIVATE void observeActivity(const Ref<ActivityObserver>&);
    WTF_EXPORT_PRIVATE void unobserveActivity(const Ref<ActivityObserver>&);

    static GSourceFuncs s_runLoopSourceFunctions;
    GRefPtr<GMainContext> m_mainContext;
    GRefPtr<GSource> m_source;

    Lock m_eventObserversLock;
    WeakHashSet<EventObserver> m_eventObservers WTF_GUARDED_BY_LOCK(m_eventObserversLock);

    using ActivityObservers = Vector<Ref<ActivityObserver>, 4>;
    Lock m_activityObserversLock;
    ActivityObservers m_activityObservers WTF_GUARDED_BY_LOCK(m_activityObserversLock);
    OptionCountedSet<Activity> m_activities WTF_GUARDED_BY_LOCK(m_activityObserversLock);

    static constexpr size_t s_pollFDsCapacity = 64;
    Vector<GPollFD, s_pollFDsCapacity> m_pollFDs;

    int m_nestedLoopLevel { 0 };
    std::atomic<bool> m_shouldStop { false };
#elif USE(GENERIC_EVENT_LOOP) || USE(BUN_EVENT_LOOP)
#if USE(BUN_EVENT_LOOP)
    // Here we make these fields be in their own class rather than existing on RunLoop.
    // This is so RunLoop can wrap it all in an std::optional.
class RunLoopGenericState {
public:
    RunLoopGenericState(RunLoop& parent);
    ~RunLoopGenericState();
    WTF_MAKE_NONCOPYABLE(RunLoopGenericState);
    using CycleResult = ::WTF::RunLoop::CycleResult;
    friend class ::WTF::RunLoop;
private:
    // The member function definitions in RunLoopGeneric.cpp will go on this class instead of on
    // RunLoop, so we need declarations here.
    void scheduleWithLock(TimerBase::ScheduledTask&) WTF_REQUIRES_LOCK(m_loopLock);
    void unscheduleWithLock(TimerBase::ScheduledTask&) WTF_REQUIRES_LOCK(m_loopLock);
    void wakeUpWithLock() WTF_REQUIRES_LOCK(m_loopLock);
    void wakeUp();
    void stop();
    CycleResult static cycle(RunLoopMode = DefaultRunLoopMode);

    // Some of the above member functions need to call RunLoop member functions, and they can't call
    // them directly anymore since they are not on the same class. So the RunLoopGenericState needs
    // access to the RunLoop containing it.
    RunLoop& m_parent;
#else
    // USE(GENERIC_EVENT_LOOP) without USE(BUN_EVENT_LOOP)
    // wakeUp(), stop(), cycle() are already declared as public methods above
    void scheduleWithLock(TimerBase::ScheduledTask&) WTF_REQUIRES_LOCK(m_loopLock);
    void unscheduleWithLock(TimerBase::ScheduledTask&) WTF_REQUIRES_LOCK(m_loopLock);
    void wakeUpWithLock() WTF_REQUIRES_LOCK(m_loopLock);
#endif

    enum class RunMode {
        Iterate,
        Drain
    };

    enum class Status {
        Clear,
        Stopping,
    };
    void runImpl(RunMode);
    bool populateTasks(RunMode, Status&, Deque<Ref<TimerBase::ScheduledTask>>&);

    friend class TimerBase;

    Lock m_loopLock;
    Condition m_readyToRun;
    Condition m_stopCondition;
    RedBlackTree<TimerBase::ScheduledTask, MonotonicTime> m_schedules;
    Vector<Status*> m_mainLoops;
    bool m_shutdown { false };
    bool m_pendingTasks { false };
#if USE(BUN_EVENT_LOOP)
    // This field is normally defined separately form the main `#if USE(GENERIC_EVENT_LOOP)`,
    // because it also exists on Windows. So we have to explicitly move it up into
    // RunLoopGenericState.
    Function<void()> m_wakeUpCallback;
    // Close the RunLoopGenericState class
};
#endif
#endif

// Bun moves this field to RunLoopGenericState
#if !USE(BUN_EVENT_LOOP) && (USE(GENERIC_EVENT_LOOP) || USE(WINDOWS_EVENT_LOOP))
    Function<void()> m_wakeUpCallback;
#endif

#if USE(BUN_EVENT_LOOP)
    // nullopt if this run loop uses the Bun implementation
    std::optional<RunLoopGenericState> m_genericState;
#endif
};

inline void assertIsCurrent(const RunLoop& runLoop) WTF_ASSERTS_ACQUIRED_CAPABILITY(runLoop)
{
    UNUSED_PARAM(runLoop);
    ASSERT_WITH_SECURITY_IMPLICATION(runLoop.isCurrent());
}

} // namespace WTF

using WTF::RunLoop;
using WTF::RunLoopMode;
using WTF::assertIsCurrent;
