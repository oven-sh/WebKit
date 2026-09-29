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

#include "SourceOrigin.h"
#include <optional>
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

// All that is done before a program is run and can go wrong, if it has not been done: init_interp_main() of CPython's Python/pylifecycle.c. After it, what is in files can be imported, codecs are found by name,
// sys.stdin and the like are there, and `site` has been imported. Whatever runs Python from outside calls it first. It may throw.
JS_EXPORT_PRIVATE void startPython(JSGlobalObject*);

// Runs a program as `python file.py` does, as the module __main__, and reports what it does not catch. Returns the status that the process is to end with, if this is all that it does.
JS_EXPORT_PRIVATE int runMain(JSGlobalObject*, std::span<const uint8_t>, const SourceOrigin&, const String& filename);

// The same, for what `python` was started with, which readCommandLine() has read: whichever of a command, a module, a file, or what comes in on the standard input it says to run. pymain_run_python() of CPython.
JS_EXPORT_PRIVATE int runMain(JSGlobalObject*);
// Whether what runMain() ran ended by raising something that nothing caught, SystemExit among them, and not by being run to its end. It is over then, as a program that is JavaScript's is after process.exit() or an
// exception that nothing catches, whatever it has left for later. One that was run to its end is over when the host has nothing left to do for it.
JS_EXPORT_PRIVATE bool mainHasRaised(JSGlobalObject*);
// For a host, of what has been thrown and nothing has caught, wherever that was: in something that its event loop called, say. If it is SystemExit the program has said that it is over, and this is the status to end with. If
// what it was given to exit with is a message, that has been written, as CPython writes it. Nothing if it is anything else, which is for the host to report.
JS_EXPORT_PRIVATE std::optional<int> exitStatusOfSystemExit(JSGlobalObject*, JSValue thrown);
// _PyRun_InteractiveLoop(): statements are read from the standard input, with a prompt for each, and run, until there are no more. What one of them raises is shown, and the next is read. It throws only if it is SystemExit, or
// if there is no going on.
void runInteractiveLoop(JSGlobalObject*);
// _PyRun_AnyFile(), of the standard input: that, if it is a terminal or `python -i` says to take it for one. Otherwise all of it is read and run as a file is. It may throw.
void runStandardInput(JSGlobalObject*);
// PyErr_Print(), of what has been raised, which is raised no longer. In CPython that ends the process there and then if it is SystemExit, and it is not -i. Here it says what that says and no more.
void printRaisedException(JSGlobalObject*);
// flush_io(). What has been raised goes on being raised.
void flushStandardStreams(JSGlobalObject*);
// What is done when there is no more to run, of what Py_FinalizeEx() does: what has been written and not sent on is sent on. It is for the host to say when that is, since a program in two languages is not over when
// the Python that began it is. False if it could not all be, for which CPython ends the process with the status 120. It does nothing if Python was never started.
JS_EXPORT_PRIVATE bool finalizePython(JSGlobalObject*);
// The end of Py_RunMain(), for a program that runMain() ran: that, and then the status that the process is to end with, given the one that runMain() returned. If what ended the program was a KeyboardInterrupt that nothing
// caught, it ends the process itself, by SIGINT, as CPython does, so that a shell that started it knows to stop.
JS_EXPORT_PRIVATE int finalizeMain(JSGlobalObject*, int status);
// _PyConfig_InitPathConfig(): works out all that Configuration says is worked out if `computesPaths`. It may throw.
struct Configuration;
void computePathConfiguration(JSGlobalObject*, Configuration&);
// Modules/getpath.py of CPython, which does the working out
std::span<const uint8_t> getPathSource();
// _PyAtExit_Call(): what a program has registered with atexit, the last first. It raises nothing.
void callAtExitFunctions(JSGlobalObject*);

} } // namespace JSC::Python
