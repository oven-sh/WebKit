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

namespace JSC {

class JSGlobalObject;
class JSObject;

namespace Python {

// Signals: what Modules/signalmodule.c of CPython has besides the module _signal.

// The module _signal. Like `posix`, it is there if whoever embeds the engine says so, in Configuration::builtinModules: what is done about a signal is the whole process's, and a host may have things of its own to do
// about one.
JS_EXPORT_PRIVATE JSObject* createSignalModule(JSGlobalObject*);

// _PySignal_Init(): the first realm that this is called for is the one whose program can say what is done about a signal, as the main interpreter is in CPython. With `installsHandlers`, SIGPIPE and SIGXFSZ are ignored
// from then on and SIGINT raises KeyboardInterrupt.
void initializeSignals(JSGlobalObject*, bool installsHandlers);
// _PyErr_CheckSignalsTstate(): calls what the program has for each signal that has come. It stops at the first that raises, and the rest are for the next time.
void handleSignals(JSGlobalObject*);
// What CPython puts off with _PyEval_AddPendingCall() when a signal has come and there was no writing to the descriptor of signal.set_wakeup_fd(). It raises nothing.
void reportSignalWakeupErrors(JSGlobalObject*);
// PyErr_SetInterruptEx(): as if the signal had come. False if there is no such signal.
bool simulateSignal(JSGlobalObject*, int signal);
// exit_sigint() of CPython's Modules/main.c: ends the process as SIGINT ends one that does nothing about it, so that whatever started it can tell. If it comes back, this is the status to end with.
int exitByInterrupt();

} } // namespace JSC::Python
