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

namespace Python {

// How the interpreter is set up, and what the command line asks it to run: what CPython calls PyConfig, as far as it means
// something here. There is one for the process. A host fills it in before anything starts the interpreter
// (Runtime::ensure), which reads it then; later changes have no effect, except where it says so.
struct Configuration {
    // ---- sys.flags ----
    int parserDebug { 0 }; // -d
    int inspect { 0 }; // -i. Read again when the program has run.
    int interactive { 0 }; // -i
    int optimizationLevel { 0 }; // -O
    bool writeBytecode { true }; // -B
    bool userSiteDirectory { true }; // -s
    bool siteImport { true }; // -S
    bool useEnvironment { true }; // -E
    int verbose { 0 }; // -v
    int bytesWarning { 0 }; // -b
    int quiet { 0 }; // -q
    bool isolated { false }; // -I
    bool devMode { false }; // -X dev
    bool utf8Mode { true }; // -X utf8
    bool warnDefaultEncoding { false }; // -X warn_default_encoding
    bool safePath { false }; // -P
    int intMaxStrDigits { 4300 }; // -X int_max_str_digits
    bool hashRandomization { true }; // PYTHONHASHSEED. Hashes do not depend on it.
    int threadInheritContext { 0 };
    int contextAwareWarnings { 0 };
    int lazyImports { -1 }; // -X lazy_imports: 1 is "all".

    // ---- What else there is an option for ----
    bool bufferedStdio { true }; // -u
    bool skipSourceFirstLine { false }; // -x
    bool faulthandler { false }; // -X faulthandler
    int tracemalloc { 0 }; // -X tracemalloc
    int importTime { 0 }; // -X importtime
    int cpuCount { -1 }; // -X cpu_count. Negative: what the system says.
    String pycachePrefix; // -X pycache_prefix. Null: none.
    String checkHashPycsMode { "default"_s };
    String stdioEncoding; // PYTHONIOENCODING. Null: UTF-8,
    String stdioErrors; // and what goes with it.
    String pythonPath; // PYTHONPATH. Null: none.
    String home; // PYTHONHOME. Null: none.
    String platlibdir { "lib"_s };
    Vector<String> warnOptions; // sys.warnoptions
    Vector<String> xOptions; // As given: "name" or "name=value".

    // ---- Who runs ----
    String programName; // What messages start with.
    String executable; // sys.executable. Empty: there is no way to start the interpreter as a program.
    String baseExecutable; // sys._base_executable
    Vector<String> originalArguments; // sys.orig_argv
    String interactiveModule { "_pyrepl.main"_s }; // Its interactive_console() is the interactive interpreter on a terminal.

    // ---- What to run: at most one of these is not null. None: standard input. ----
    String runCommand; // -c
    String runModule; // -m
    String runFilename;
    Vector<String> arguments; // sys.argv
};

JS_EXPORT_PRIVATE Configuration& configuration();

// The PYTHON* variables of the environment, for a host that has a command line of its own (`bun main.py`).
// What is returned is as for readCommandLine().
JS_EXPORT_PRIVATE std::optional<int> readEnvironment(Configuration&);

// The command line of `python3`, and the environment. `arguments[0]` is the name of the program.
// If there is nothing to run (--help, --version, a mistake), that has been dealt with, and this returns the exit status.
JS_EXPORT_PRIVATE std::optional<int> readCommandLine(Configuration&, std::span<const char* const> arguments);

// Runs what the configuration says, as `python3` does, up to where the interpreter would be finalized.
// The result, and `unfinished`, are as for runMainModule() (PyExec.h).
JS_EXPORT_PRIVATE std::optional<int> runCommandLine(JSGlobalObject*, JSValue* unfinished = nullptr);
// When the promise has settled. `exception`: what it was rejected with, or empty. (-i has its say now.)
JS_EXPORT_PRIVATE std::optional<int> commandLineProgramEnded(JSGlobalObject*, JSValue exception);

// ---- For the runtime ----

// What happens between the start of the import system and the program: the options that are carried out by modules, and `site`.
void startMainInterpreter(JSGlobalObject*);

// -X importtime: lives while a module is found and loaded, and says on stderr how long that took.
class ImportTimeReport {
public:
    explicit ImportTimeReport(const String& moduleName)
    {
        if (configuration().importTime) [[unlikely]]
            start(moduleName);
    }
    ~ImportTimeReport()
    {
        if (!m_name.isNull()) [[unlikely]]
            finish();
    }
    // -X importtime=2: the module had been imported already.
    static void wasCached(const String& moduleName)
    {
        if (configuration().importTime == 2) [[unlikely]]
            reportCached(moduleName);
    }

private:
    void start(const String&);
    void finish();
    static void reportCached(const String&);

    String m_name;
    double m_start { 0 };
    double m_accumulatedBefore { 0 };
};

// The module __main__, made the first time.
class PyModule;
PyModule* ensureMainModule(JSGlobalObject*);

} // namespace Python
} // namespace JSC
