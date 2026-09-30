/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#pragma once

#include "JSCJSValue.h"
#include <optional>
#include <wtf/Seconds.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSPromise;

namespace Python {

// What the rest of the engine has to do with asyncio: PythonAsyncioModule.cpp. See "What JavaScript waits for, and asyncio" in README.md.

// For as long as JavaScript is running something of Python's that it is waiting for: resumeAwaitable().
class JavaScriptStep {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    JavaScriptStep(JSGlobalObject*, JSObject* iterator);
    ~JavaScriptStep();

    // It is a Task of asyncio's, having asked which task it is.
    bool isTask() const;
    // It has yielded what only a Task knows how to wait for, and so it is one from now on. False if that is not what it yielded, or if it raised, which it does if there is no loop for it to be a task of.
    bool becomeTask(JSValue yielded);
    // What a Task does when its coroutine has yielded, or returned, or raised, which is then what has been raised here. It goes on with it from now on, and settles the promise.
    void conclude(JSValue yielded, JSValue returned);

private:
    JSGlobalObject* m_globalObject;
    JSObject* m_iterator;
    JSObject* m_outerIterator;
    JSValue m_outerTask;
    bool m_hadRunningLoop { false };
};

// Whether an event loop is in the middle of going round, further up the stack: one that the host is turning, or one that the program is running for itself.
bool isEventLoopRunningHere(JSGlobalObject*);

// For as long as something of Python's waits for the host to go on with its own event loop. Whatever the host then runs is as it might be in another thread: it is in no task, and in no loop, of what is waiting.
//
// What gets out of the loop that the host turns is as a rule the host's to report. It is KeyboardInterrupt or the like, since the loop sees to the rest. While this lasts it is for whoever is waiting.
class WaitingForHost {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit WaitingForHost(JSGlobalObject*);
    ~WaitingForHost();

    // What has got out of the loop meanwhile, which is then nobody else's. Empty if nothing has.
    JSValue takeWhatGotOutOfTheLoop();

private:
    JSGlobalObject* m_globalObject;
    JSObject* m_awaitable;
    JSValue m_loop;
    JSValue m_task;
    bool m_loopIsOfStep { false };
    bool m_isCounted { false };
};

// For the host, which is to leave off waiting: Configuration::waitForPromise.
JS_EXPORT_PRIVATE bool hasSomethingGotOutOfTheLoop(JSGlobalObject*);
// asyncio.Runner._on_sigint(), the first time: what is being run to its end is cancelled, if it is a task, so that it can tidy up. False if it is not one, or if it raised.
bool cancelForInterrupt(JSGlobalObject*, JSObject* iterator);
bool isCancelledError(JSGlobalObject*, JSValue);

// The promise that JavaScript is given for a Future, which needs nothing to run it. Null if it is not one.
JSPromise* promiseOfFuture(JSGlobalObject*, JSValue);

// An event loop that nothing in Python is running is turned by the host, once round at a time. Where it would wait, it says instead what it would have waited for.
bool isEventLoopBeingTurned(JSGlobalObject*);
// May raise.
void noteWaitOfEventLoop(JSGlobalObject*, int descriptor, int watched, std::optional<Seconds> timeout, bool hasEvents);
// It is about to be closed. If it is what the host is watching, the host is to leave off first: the next to be opened may well have the same number.
void noteClosingOfDescriptor(JSGlobalObject*, int descriptor);
// For the host, when what it was asked to watch for has come: Configuration::watchEventLoop. May throw, and it is for the host to report.
JS_EXPORT_PRIVATE void turnEventLoop(JSGlobalObject*);

} // namespace Python

} // namespace JSC
