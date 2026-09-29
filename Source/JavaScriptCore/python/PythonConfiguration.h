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

#include "ImplementationVisibility.h"
#include "JSCJSValue.h"
#include "PythonFileOperations.h"
#include <optional>
#include <span>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;
class JSObject;

namespace Python {

// A module that is written in C++: `struct _inittab` of CPython.
struct BuiltinModule {
    ASCIILiteral name;
    JSObject* (*create)(JSGlobalObject*); // Null if it raised.
    // What is left to do once the module is in sys.modules, if anything is: Py_mod_exec. It is for one that imports what imports it. It may raise.
    void (*execute)(JSGlobalObject*, JSObject* module) { nullptr };
};

// A module that is written in Python and is not in a file: `struct _frozen` of CPython, but that it is the source that there is here, and not what comes of compiling it.
struct FrozenModule {
    ASCIILiteral name;
    std::span<const uint8_t> source; // If there is no source at all, there is no importing anything of this name, from here or from anywhere after.
    bool isPackage { false };
    ASCIILiteral originalName { }; // What it is called in the library, if that is something else.
    ImplementationVisibility visibility { ImplementationVisibility::Public };
};

// What `import a.b` means, if it is something of JavaScript's.
struct JavaScriptModule {
    String key; // What the module loader knows it by: what GlobalObjectMethodTable::moduleLoaderResolve gives. Null if there is nothing to load, and it is only somewhere for others to be, as `node` is for `node:fs`.
    String file; // __file__. Null if it is in none.
    // What its __path__ has in it, if there may be others below it, so that `import a.b.c` is worth asking about. It is what the host is given to find them by, and can be anything that is not the name of a
    // directory, which Python would look in. Null if there are none.
    String package;
};

// What whoever embeds the engine tells Python about the program: what PyConfig is to whoever embeds CPython. It is asked for once for each
// global object, through GlobalObjectMethodTable::configurePython, when Python is first used there.
struct Configuration {
    Vector<String> arguments; // sys.argv
    Vector<String> moduleSearchPaths; // sys.path, after what follows
    String firstSearchPath; // What sys.path begins with: where the program is, or "" for wherever the process is at the time. Null for neither. PyConfig.sys_path_0
    String executable; // sys.executable
    String implementationName { "javascriptcore"_s }; // sys.implementation.name
    const FileOperations* files { systemFileOperations() }; // Null if there are to be no files.
    Vector<BuiltinModule> builtinModules; // Besides the engine's own: PyImport_AppendInittab()
    Vector<FrozenModule> frozenModules; // Besides the engine's own, and before all of them but importlib: PyImport_FrozenModules
    bool usesFrozenModules { true }; // -X frozen_modules
    bool importsSite { true }; // Not -S
    String prefix; // sys.prefix and sys.base_prefix: what the library is installed under, if it is installed anywhere.
    String executablePrefix; // sys.exec_prefix and sys.base_exec_prefix
    String libraryDirectory; // sys._stdlib_dir: where the files of the library are. Null if they are not in a directory.
    String standardStreamEncoding { "utf-8"_s }; // PYTHONIOENCODING
    String standardStreamErrors { "surrogateescape"_s };
    // Whether sys.stdout and sys.stderr keep what is written to them until there is a good deal of it: not -u. They do not, unless it is asked for, so that what Python writes and what JavaScript writes come out in
    // the order in which they were written. What is to be kept back, so as not to ask the system so often, is for `files` to keep back, which can keep both.
    bool buffersStandardStreams { false };
    // Whether, from when Python starts, SIGINT raises KeyboardInterrupt and SIGPIPE and SIGXFSZ are ignored: PyConfig.install_signal_handlers. That is for a program that is Python's. It is not done to one that is
    // JavaScript's and imports something, which would find that it could no longer be interrupted. A program that imports `signal` has the first of them done to it whatever this says, as in CPython.
    bool installsSignalHandlers { false };
    // What io.open_code() is: given the name of a file whose contents are to be run, a str, it returns a file that is open for reading bytes, or nothing, having thrown. It is for what wants a say in what is
    // run, or has it somewhere other than in a file. Null is open(path, "rb"). PyFile_SetOpenCodeHook() of CPython.
    JSValue (*openCode)(JSGlobalObject*, JSValue path) { nullptr };
    // Waits until a descriptor can be read from, or for so long, or for less: whoever asks looks for itself afterwards, and asks again. Nothing for the time is for as long as it takes. It is where an event loop of
    // Python's waits, `asyncio`'s among them, for what it is watching or until it has something to do. What it watches with is a descriptor that can itself be watched. So a host that has an event loop of its own
    // watches that, and goes on with its own meanwhile, and the two are one. It may throw. Null is to wait in the system, with nothing else going on.
    void (*waitForDescriptor)(JSGlobalObject*, int descriptor, std::optional<Seconds> timeout) { nullptr };
    // A coroutine that JavaScript is waiting for may use asyncio with nothing in Python running an event loop. The host then turns one: it is to call Python::turnEventLoop() when `descriptor` can be read from, or
    // after so long if a time is given, whichever is first, and not from inside this. Each call replaces the last. `hasSomethingToWatch` is whether the loop is watching for anything but its own being woken, and so
    // whether there is something to stay for though no time is given. A descriptor of -1 is to say that there is nothing to watch any longer, and what was being watched is about to be closed. Without this there is no such loop.
    void (*watchEventLoop)(JSGlobalObject*, int descriptor, std::optional<Seconds> wakeAfter, bool hasSomethingToWatch) { nullptr };
    // Finds the module of JavaScript's that `import name` means, the name being the whole of it, dots and all. It is asked when nothing of Python's has been found by that name. `directories` is where Python looked:
    // sys.path, or the __path__ of what the module is below, which is what the host gave for it if that is JavaScript's too. Nothing if there is none, or if it threw. Null if none can be imported.
    std::optional<JavaScriptModule> (*findJavaScriptModule)(JSGlobalObject*, const String& name, std::span<const String> directories) { nullptr };

    // ---- What `python` is told on its command line and in its environment: the rest of PyConfig. readCommandLine() fills it in, and much of what is above. As it comes it is what a program has that is not started that way.

    Vector<String> originalArguments; // sys.orig_argv. `arguments`, if there are none.
    Vector<String> warningOptions; // sys.warnoptions: -W and PYTHONWARNINGS
    Vector<String> extraOptions; // sys._xoptions: -X
    int parserDebug { 0 }; // -d, which does nothing
    int inspect { 0 }; // -i
    int interactive { 0 }; // -i
    int optimizationLevel { 0 }; // -O
    int verbose { 0 }; // -v
    int bytesWarning { 0 }; // -b
    int quiet { 0 }; // -q
    // Not -B. It is not asked for unless it is asked for here, whatever the command line says: nothing is written yet.
    bool writesBytecode { false };
    bool usesUserSiteDirectory { true }; // Not -s
    bool usesEnvironment { true }; // Not -E
    bool isIsolated { false }; // -I
    bool isDevelopmentMode { false }; // -X dev
    // -X utf8. Everything is UTF-8 whatever this says. It is what sys.flags.utf8_mode is, which is what the library goes by.
    bool usesUTF8Mode { true };
    bool warnsOfDefaultEncoding { false }; // -X warn_default_encoding
    bool hasSafePath { false }; // -P
    int maximumDigitsOfIntAsString { 4300 }; // -X int_max_str_digits
    bool threadsInheritContext { false }; // -X thread_inherit_context
    bool hasContextAwareWarnings { false }; // -X context_aware_warnings
    int importTime { 0 }; // -X importtime
    int cpuCount { -1 }; // -X cpu_count. Negative is however many there are.
    String bytecodeCachePrefix; // sys.pycache_prefix: -X pycache_prefix. Null is None.
    bool hasDebugRanges { true }; // Not -X no_debug_ranges

    // What is to be run, of which there is at most one. If there is none it is what comes in on the standard input.
    String runCommand; // -c
    String runModule; // -m
    String runFilename;
    bool skipsFirstLineOfSource { false }; // -x

    // ---- Where things are. If `computesPaths`, all that is not given here is worked out from where the program is, by CPython's own Modules/getpath.py, when Python starts: `executable`, `prefix`, `executablePrefix`,
    // `libraryDirectory` and `moduleSearchPaths` above, and what follows.

    bool computesPaths { false };
    bool hasModuleSearchPaths { false }; // Whether `moduleSearchPaths` is all of them already: PyConfig.module_search_paths_set
    String programName; // What it was started as
    String home; // PYTHONHOME
    String searchPathFromEnvironment; // PYTHONPATH
    String platformLibraryDirectory { "lib"_s }; // sys.platlibdir
    String baseExecutable; // sys._base_executable. `executable`, if it is null.
    String basePrefix; // sys.base_prefix. `prefix`, if it is null.
    String baseExecutablePrefix; // sys.base_exec_prefix. `executablePrefix`, if it is null.
    bool warnsOfPathConfiguration { true };
};

// What Py_BytesMain() does before there is anything to run anything with: _PyConfig_Read(), for what `python` was started with, the first of which is what it was started as. If there is nothing to be run after all, because
// what was asked for was to be told something, which it has been, or because what was asked for makes no sense, which has been said, it is the status that the process is to end with.
JS_EXPORT_PRIVATE std::optional<int> readCommandLine(Configuration&, std::span<const char* const> arguments);

} } // namespace JSC::Python
