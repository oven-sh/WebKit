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
// It must not throw. If the host has to give up (the VM is terminating), it returns -1 with errno EINTR: the caller then
// runs what is pending, which raises, or waits again for the time that is left. The same goes for a signal: as soon as one
// that Python handles has arrived, the function should return -1 with EINTR, as the system call does, or the handler waits.
// A request with timeout 0 comes here too (asyncio asks that way whenever it has something to run): waitWithoutBlocking() is
// the whole answer, and whether to turn the loop once first, so that a busy Python loop does not starve it, is up to the host.
// What does not come here: time.sleep(), os.read() and the like, os.waitpid(), locks and semaphores.

struct WaitRequest {
    enum class Kind : uint8_t { Poll, Select, Epoll, Kqueue };

    Kind kind;
    // In nanoseconds. Negative: for ever. 0: only look.
    int64_t timeout { -1 };

    // Poll: as for poll(). `revents` is filled in.
    struct pollfd* descriptors { nullptr };
    nfds_t descriptorCount { 0 };

    // Select: as for select(). Any of the sets may be null. They are replaced by what is ready.
    int selectLimit { 0 }; // The highest descriptor in any of the sets, plus 1.
    fd_set* readable { nullptr };
    fd_set* writable { nullptr };
    fd_set* exceptional { nullptr };

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
// The result is as above, or waitRaised if there is an exception: the handler's.
int waitForEvents(JSGlobalObject*, WaitRequest&);

// Before a system call that reads at least one byte from a descriptor and may wait for long. If the descriptor is of a kind of which
// it is known that read() waits exactly as long as poll() says that there is nothing to read (a pipe, a connected socket, a
// terminal that delivers lines or waits for a byte; open for reading, blocking), this waits instead, until there is something:
// in waitForEvents(), where a thread that is idle collects and a host's event loop turns. False: an exception.
bool waitUntilReadable(JSGlobalObject*, int fd);
// The same without asking what the descriptor is: for one that the caller has made.
bool waitUntilReady(JSGlobalObject*, int fd);

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
