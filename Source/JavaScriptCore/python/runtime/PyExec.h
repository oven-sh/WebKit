/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCJSValue.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;
class VM;

namespace Python {

// What a host (the jsc shell, Bun) runs Python programs with.

// Runs the source of a file as the module __main__, as `python path arguments...` does, to the end of the interpreter:
// exit handlers have run and the standard streams are flushed. Returns the exit status of the process.
JS_EXPORT_PRIVATE int runMainFile(JSGlobalObject*, const String& path, std::span<const uint8_t> source, const Vector<String>& arguments);

// The same, for a host that has more to do when the module has run (an event loop to turn): the interpreter goes on, and the
// host calls finalizeInterpreter() when it exits. If an exception ended the module, it has been reported, and this returns the
// exit status it calls for: the program is over. A negative status is a signal: see exitAfterException().
// A module that awaits at the top level runs on the host's event loop, if it has one (setEventLoopModule), and need not be over
// when this returns: `unfinished` is then a JS promise, and the host calls mainModuleEnded() when that has settled.
JS_EXPORT_PRIVATE std::optional<int> runMainModule(JSGlobalObject*, const String& path, std::span<const uint8_t> source, const Vector<String>& arguments, JSValue* unfinished = nullptr);
// `exception`: what the promise was rejected with, or empty. Returns what runMainModule() would have.
JS_EXPORT_PRIVATE std::optional<int> mainModuleEnded(JSGlobalObject*, JSValue exception);

// The name of a module, which the host supplies, that is imported when importlib has been set up, before `site`: it adds what
// the host can import to sys.meta_path and sys.path_hooks.
JS_EXPORT_PRIVATE void setImportSystemModule(ASCIILiteral name);

// ---- The event loop, which is the host's ----

// The name of a module, which the host supplies, that stands between coroutines and the host's event loop:
//   run(awaitable)  A JS promise of what `await awaitable` gives. This is what JS gets when it awaits a coroutine, and how a
//                   module that awaits at the top level runs.
//   wait(promise)   The iterator that `await promise` runs, for a JS promise.
// Without one, only what never really waits can be awaited across the languages.
JS_EXPORT_PRIVATE void setEventLoopModule(ASCIILiteral name);

// A host that is about to sleep on a thread that runs Python (in its event loop, say) does so through this, so that the other
// threads of Python run meanwhile: the function is called without the global interpreter lock, and must not touch the VM.
// False: there are no other threads, and the function has not been called.
JS_EXPORT_PRIVATE bool callWithoutGIL(VM&, void (*function)(void*), void* argument);

// A host that destroys a VM while the process goes on (a worker ends) calls this first, on the thread of the VM, when no code of the
// program is to run any more: with or without finalizeInterpreter() before it. It ends the threads that Python started in the VM and
// returns when they are gone. They run no more code in Python. One that is in the middle of something in C++ finishes that first.
JS_EXPORT_PRIVATE void stopThreads(VM&);

// Finalizers, callbacks of weak references and signal handlers are queued by the collector and by signal handlers, and called
// at the next safe point in Python code (makeCallsOwed() in PyHooks.h). A host that may be asleep in its event loop, or busy
// with other things than Python code, is told here that there are some, and calls makeCallsOwed() when it can run any code.
// The function is called on any thread, and from signal handlers: it may only do what is safe there.
JS_EXPORT_PRIVATE void setCallsOwedFunction(void (*)(VM&));

// os.chdir() has changed the current directory of the process. For a host that keeps its name, or things that depend on it.
// The function is called on the main thread: at once, or, if it was another thread, as soon as the main thread has the VM again,
// wherever that is. So it throws nothing and runs no code.
JS_EXPORT_PRIVATE void setDidChangeDirectoryFunction(void (*)(JSGlobalObject*));

// For a host that keeps the variables of the environment itself, as JS has them in process.env: the host and os.environ tell each
// other what changes. To the host names and values are text: UTF-8, with U+FFFD for what is not.
using EnvironmentVariables = Vector<std::pair<String, String>>;
struct HostEnvironment {
    // os.environ is being made. The host adds all it has, and os.environ begins as that. True: the environment of the process,
    // which children inherit, is made the same. It may throw.
    bool (*read)(JSGlobalObject*, EnvironmentVariables&);
    // os.putenv() or os.unsetenv(), which os.environ calls, has changed the environment of the process. A null value: the variable is
    // gone. When and where it is called, and what it may do, is as for the function of setDidChangeDirectoryFunction().
    void (*didChange)(JSGlobalObject*, const String& name, const String& value);
};
JS_EXPORT_PRIVATE void setHostEnvironment(const HostEnvironment&);
// The host's has changed: os.environ follows, if there is one yet, and with `ofProcess` the environment of the process. A null value: the
// variable is gone. A name that no environment can hold is passed over. It may throw.
JS_EXPORT_PRIVATE void hostDidChangeEnvironment(JSGlobalObject*, const String& name, const String& value, bool ofProcess);
// `import` of a file, for code that is not Python: the module, or an exception. `source`, if it is not null, is taken for what
// is in the file. If the host's module for the import system (setImportSystemModule) has a function import_file(path, source),
// that decides what module the file is.
JS_EXPORT_PRIVATE JSValue importFileFromJS(JSGlobalObject*, const String& path, const String& source = { });

// `import name`, for code that is not Python: the module, or an exception. The first use starts the interpreter, with the
// current directory first on sys.path. "__python__" is no module but an object of JS with what JS needs to talk to Python:
// kwargs({ name: value }), float(number), import(name). See PyJSView.h.
JS_EXPORT_PRIVATE JSValue importModuleFromJS(JSGlobalObject*, const String& name);
// The same for a host whose module loader takes the name from a specifier (`import json from "python:json"`). If there is no such
// module, the ModuleNotFoundError gets an attribute `code` with this text: that is what code in JS tells such errors by.
JS_EXPORT_PRIVATE JSValue importModuleFromJS(JSGlobalObject*, const String& name, ASCIILiteral codeIfNotFound);
// Whether the name, which may be dotted, is that of a module of the standard library, going by its first part and
// sys.stdlib_module_names. Does not start the interpreter.
JS_EXPORT_PRIVATE bool isStandardLibraryModuleName(StringView);

// Reports the exception that is propagating as the interpreter does one nothing caught, through sys.excepthook.
// Returns the exit status it calls for: that of a SystemExit, which is not reported, or 1; or, negative, the number of the
// signal the process is to die of: a KeyboardInterrupt nothing caught ends it as the SIGINT would have that it stands for.
JS_EXPORT_PRIVATE int handleUncaughtException(JSGlobalObject*);
// The same for an exception of Python's that is not propagating any more: one that a host has found nobody to catch, in a callback
// that its event loop called or in a promise. False: the interpreter has ended, and there is no hook to tell.
JS_EXPORT_PRIVATE bool reportUncaughtException(JSGlobalObject*, JSValue exception);
// For a host that is about to write to standard output or standard error for code that is not Python (console.log): what Python
// code has printed so far, and is still in buffers, is written first. If vm.heap.pythonRuntimes() is empty there is no need.
JS_EXPORT_PRIVATE void flushStandardStreams(VM&);

// Whether the value is an exception that is raised to end the program, which a host should let it do wherever it surfaces:
// SystemExit, KeyboardInterrupt.
JS_EXPORT_PRIVATE bool endsProgram(JSGlobalObject*, JSValue exception);

// For a status that is negative, once all that is to happen at exit has happened: dies of the signal. Otherwise returns.
JS_EXPORT_PRIVATE void dieIfStatusIsSignal(int status);

// The first part of finalizeInterpreter(): threading._shutdown(), which waits for the threads that are no daemons, and the atexit
// handlers. For a host that has handlers of its own for the exit of the program, which may be functions of Python's: it calls
// this, then those, then finalizeInterpreter(), which does this again for what those have left.
JS_EXPORT_PRIVATE void runExitHandlers(JSGlobalObject*);

// What happens when the interpreter exits: threading._shutdown(), the atexit handlers, the finalizers of what is left, flushing.
// Does nothing if the interpreter never started, or the second time. False: sys.stdout or sys.stderr could not be flushed (a
// full disk, a pipe nobody reads), and the exit status of the process is to be statusIfNotFlushed whatever it would have been.
// Throws nothing but the host's termination, after which nothing of the above runs any more.
JS_EXPORT_PRIVATE bool finalizeInterpreter(JSGlobalObject*);
static constexpr int statusIfNotFlushed = 120;

} // namespace Python
} // namespace JSC
