/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include <wtf/Platform.h>

#if !OS(WINDOWS)

#include <poll.h>
#include <sys/select.h>

#if OS(LINUX)
#include <sys/epoll.h>
#endif
#if OS(DARWIN) || OS(FREEBSD) || OS(NETBSD) || OS(OPENBSD)
#define PY_HAVE_KQUEUE 1
#include <sys/event.h>
#else
#define PY_HAVE_KQUEUE 0
#endif

namespace JSC {

class JSGlobalObject;

namespace Python {

// Waiting for file descriptors.
//
// Whenever Python code waits until a file descriptor is ready, or until time is up, it gets here: select.select(),
// poll.poll(), epoll.poll(), kqueue.control(), and every operation of a socket that may have to wait (a socket with a timeout
// polls its descriptor first). There is no other place where the runtime sleeps on a descriptor.
//
// That is so that a host with an event loop of its own can keep it turning meanwhile. asyncio, unchanged, sits in
// selectors.DefaultSelector().select(timeout), which is epoll.poll() or kqueue.control(): with the host's function in place of
// the system call, timers, promises and I/O of JS go on while Python's loop is idle, and the two loops are one.
//
// What the host's function has to do: return as the system call would (see WaitRequest), having run whatever else it likes
// while the request is not satisfied. A way to do that:
//   - Epoll, Kqueue: `queue` is itself a descriptor that polls as readable when it has events. Add it to the host's loop, with
//     a timer for `timeout`; turn the loop until either fires; then call waitWithoutBlocking(), which fetches the events.
//     For Kqueue, apply the changes first: call waitWithoutBlocking() once before waiting (it clears them), and if that
//     already returns events or an error, that is the answer.
//   - Poll, Select: call waitWithoutBlocking() first; while it returns 0, watch the descriptors named in the request (or
//     put them in a temporary epoll or kqueue and watch that) and the timer, turn the loop, and call it again.
// Code that the host runs meanwhile may call back into Python, and that may wait in turn: the function must be reentrant.
// It throws nothing but a termination: if the host has to give up (the VM is terminating), it sees to it that one is thrown, and
// returns -1 with errno EINTR. It returns the same, with nothing thrown, as soon as calls are owed (VMTraps::NeedPythonCalls; the
// function of setCallsOwedFunction() is told), and makes none of them itself: the caller does, which may raise, and waits again
// for the time that is left. That is how a signal that Python handles gets its handler called, as after the system call, and
// how a wait is ended when the host ends what runs (mainThreadIsToHandleTrap()).
// A request with timeout 0 comes here too (asyncio asks that way whenever it has something to run): waitWithoutBlocking() is
// the whole answer. The host must not sleep for such a request: nobody would know to wake it. Nor does it turn its loop: what the
// loop calls is to find nothing current of whoever waits (AsyncLocalStorage, contextvars), and that is seen to for a wait, not
// for a look, of which a busy loop makes one for each of its iterations.
// What does not come here: time.sleep(), locks and semaphores. What does, as a Poll of one descriptor: os.read() and the like
// (waitUntilReadable()), and on Linux os.waitpid() for one child and os.spawnv(os.P_WAIT) and its kin (waitUntilReady()).

struct WaitRequest {
    enum class Kind : uint8_t { Poll, Select, Epoll, Kqueue };

    Kind kind;
    // In nanoseconds. Negative: for ever. 0: only look.
    int64_t timeout { -1 };
    // See IdleWait. For the host this is nothing.
    bool isWaitOfCall { false };
    // Once a signal of the program's has arrived on the thread (signalsArrivedOnThread()), the wait is over, as if time had run out. (Else it goes on: PEP 475.)
    // For the host this is nothing: it returns EINTR as soon as calls of Python's are owed, and waitForEvents() decides.
    bool endsAtSignal { false };
    // With that: what signalsArrivedOnThread() said when the caller last looked for signals. One that has arrived since ends the wait before it begins: its handler would be called in
    // the wait, which would go on.
    unsigned signalsSeen { 0 };
    // The caller has looked a moment ago, and there was nothing: what a wait does itself before it has the call begun again (IdleWait::willBlock()). For the host this is nothing.
    bool hasJustLooked { false };
    // For a caller that makes one wait of several requests: what this has at the end of one it gives to the next. See IdleWait::goOnWithInterval(). For the host this is nothing.
    int64_t idleInterval { 0 };
    // If the call has been begun again in the middle of this wait: when the wait is to end, on the monotonic clock, which is not `timeout`
    // from now. 0: it has not, or there is no end. For a caller that may wait again for what is left of the time.
    int64_t deadlineTakenUp { 0 };

    // Poll: as for poll(). `revents` is filled in.
    struct pollfd* descriptors { nullptr };
    nfds_t descriptorCount { 0 };

    // Select: as for select(). Any of the sets may be null. They are replaced by what is ready.
    int selectLimit { 0 }; // The highest descriptor in any of the sets, plus 1.
    fd_set* readable { nullptr };
    fd_set* writable { nullptr };
    fd_set* exceptional { nullptr };
    // The three as they are asked, the caller's copies: a wait that is made piece by piece puts the sets back from them. A Select request must have them.
    const fd_set* asked { nullptr };

    // Epoll, Kqueue: the descriptor of the queue, and room for the events.
    int queue { -1 };
    int maxEvents { 0 };
#if OS(LINUX)
    struct epoll_event* epollEvents { nullptr };
#endif
#if PY_HAVE_KQUEUE
    struct kevent* kqueueEvents { nullptr };
    // Applied before waiting, once.
    const struct kevent* kqueueChanges { nullptr };
    int kqueueChangeCount { 0 };
#endif
};

// The result is that of the system call: how many descriptors are ready (for Select, how many bits are set; for Epoll and
// Kqueue, how many events were stored), 0 if time ran out first, -1 with errno set.
using WaitFunction = int (*)(JSGlobalObject*, WaitRequest&);

// For the host. Null puts the default back: blockingWait().
JS_EXPORT_PRIVATE void setWaitFunction(WaitFunction);
// Whether waitForEvents() on this thread calls a function of the host's: it has set one, and this is the main thread, whose event
// loop it turns. If so, an operation that would block inside a system call (recv() on a socket without a timeout) waits here
// first, for ever, so that it blocks here instead.
bool waitsInHost(JSGlobalObject*);

// The system call, waiting for as long as the request says.
JS_EXPORT_PRIVATE int blockingWait(JSGlobalObject*, WaitRequest&);
// The system call, not waiting at all, whatever the request says. If nothing is ready, the request is as it was (the sets of
// Select too, which select() itself would have emptied), and this can be called again.
JS_EXPORT_PRIVATE int waitWithoutBlocking(WaitRequest&);

// ---- For the modules ----

static constexpr int waitRaised = -2;

// Waits. If a signal interrupts the wait, its handler runs, and the wait goes on for the time that is left (PEP 475).
// The result is as above, or waitRaised if there is an exception: the handler's, or the host ends what runs.
int waitForEvents(JSGlobalObject*, WaitRequest&);

// Before a system call that reads at least one byte from a descriptor and may wait for long. If the descriptor is of a kind of which
// it is known that read() waits exactly as long as poll() says that there is nothing to read (a pipe, a connected socket, a
// terminal that delivers lines or waits for a byte; open for reading, blocking), this waits instead, until there is something:
// in waitForEvents(), where a thread that is idle collects and a host's event loop turns. False: an exception.
bool waitUntilReadable(JSGlobalObject*, int fd);
// The same without asking what the descriptor is: for one that the caller has made. `isWaitOfCall`: see IdleWait. `hasJustLooked`: see WaitRequest.
bool waitUntilReady(JSGlobalObject*, int fd, bool isWaitOfCall, bool hasJustLooked = false);

// The clock that timeouts are measured by, in nanoseconds.
int64_t monotonicNanoseconds();

// A timeout given as a number of seconds (`unit` 1000000000) or milliseconds (1000000), in nanoseconds, rounded away from
// zero. False: an exception. Of what is not a number it says "timeout must be a real number or None, not x", or, if
// `namesTimeout` is false, what float() says of it.
bool timeoutFromObject(JSGlobalObject*, JSValue, int64_t unit, int64_t& nanoseconds, bool namesTimeout = true);
// t / k, rounded up.
constexpr int64_t divideRoundingUp(int64_t t, int64_t k)
{
    int64_t quotient = t / k;
    return t % k > 0 ? quotient + 1 : quotient;
}
// Away from zero.
constexpr int64_t divideRoundingOut(int64_t t, int64_t k)
{
    int64_t quotient = t / k;
    if (!(t % k))
        return quotient;
    return t >= 0 ? quotient + 1 : quotient - 1;
}

// Runs the handlers of the signals that have arrived. False: one of them raised. (PyErr_CheckSignals) It is in PySignal.cpp.
bool checkSignals(JSGlobalObject*);

// An object nobody sees, which closes the descriptor when the collector finds it unreachable. For an object that owns a
// descriptor and whose type has no __del__: it keeps the guard in a field.
JSValue makeDescriptorGuard(JSGlobalObject*, int fd);
// The owner has closed the descriptor, or given it away.
void disarmDescriptorGuard(JSGlobalObject*, JSValue guard);

} // namespace Python
} // namespace JSC

#endif // !OS(WINDOWS)
