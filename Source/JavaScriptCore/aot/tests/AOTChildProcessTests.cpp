/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChildProcess.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTNativeTest.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTTestRunner.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <stdlib.h>
#include <wtf/ASCIICType.h>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#endif
#if OS(LINUX)
#include <array>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace JSC { namespace AOT {

using namespace Typed;

static constexpr const char* crashRequestVariable = "TESTAOT_CRASH_ON_REQUEST";

static constexpr ASCIILiteral passingFile =
    "# RUN: print\n"
    "# CHECK: Negate\n"
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"negate\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Negate v0\n"
    "    Return v1\n"
    "}\n"_s;
static constexpr ASCIILiteral fileWithoutMemory = "# RUN: crash without-memory\n"_s;
static constexpr ASCIILiteral fileWithoutMemoryAtNoWork = "# RUN: crash without-memory-at-no-work\n"_s;

static ChildProcessLauncher createLauncher(Vector<String>&& sharedArguments = { })
{
    return ChildProcessLauncher(String(programPath()), WTF::move(sharedArguments));
}

static ChildProcessResult runWith(const ChildProcessLauncher& launcher, std::initializer_list<String> arguments)
{
    return launcher.run(std::span(arguments.begin(), arguments.size()));
}

static void setCrashRequest(bool isRequested)
{
#if OS(WINDOWS)
    SetEnvironmentVariableA(crashRequestVariable, isRequested ? "1" : nullptr);
#else
    if (isRequested)
        setenv(crashRequestVariable, "1", 1);
    else
        unsetenv(crashRequestVariable);
#endif
}

// A build has a heap of the engine where it runs code of the program. Too little memory for it is an error of the build, as too little for the compiler's own tables is.
static void testHeapWithoutMemoryEndsBuildWithError()
{
    TemporaryFile file(".ir"_s, byteCast<uint8_t>(fileWithoutMemory.span8()));
    ChildProcessResult result = runWith(createLauncher(), { String(runSubcommandName), "--single-process"_s, file.path() });
    AOT_TEST_CHECK(result.exitedWith(1));
    AOT_TEST_CHECK(result.output.contains("error: Out of memory while compiling\nStopped while compiling stopsTheCompiler"_s));
    AOT_TEST_CHECK(!result.output.contains("ASSERTION FAILED"_s));

    TemporaryFile atNoWork(".ir"_s, byteCast<uint8_t>(fileWithoutMemoryAtNoWork.span8()));
    ChildProcessResult crashed = runWith(createLauncher(), { String(runSubcommandName), "--single-process"_s, atNoWork.path() });
    AOT_TEST_CHECK(crashed.outcome == ChildProcessOutcome::Crashed);
    AOT_TEST_CHECK(!crashed.output.contains("Out of memory while compiling"_s));
}

static void testShellCrash()
{
    ChildProcessResult result = runWith(ChildProcessLauncher(shellPath(), { }), { "--useDollarVM=1"_s, "-e"_s, "$vm.crash()"_s });
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);
}

// Out of memory the process exits with the code of a failed run, as under a sanitizer, and has come to no last line.
static void testFileWhoseProcessEndsEarlyIsNamed()
{
    TemporaryFile ending(".ir"_s, byteCast<uint8_t>(fileWithoutMemory.span8()));
    TemporaryFile passing(".ir"_s, byteCast<uint8_t>(passingFile.span8()));
    ChildProcessLauncher launcher = createLauncher();
    TestRunRequest request;
    request.paths = { ending.path(), passing.path() };
    request.launcher = &launcher;

    StringPrintStream out;
    TestRunSummary summary = runTestFiles(request, out);

    AOT_TEST_CHECK(summary.passed == 1 && summary.failed == 1);
    AOT_TEST_CHECK(out.toString().contains(makeString("FAIL "_s, ending.path(), ":1: exited with 1\n"_s)));
}

static constexpr ASCIILiteral fileLeftPrefix = "leaves "_s;

static void testCrashesOnRequest()
{
    if (!getenv(crashRequestVariable))
        return;
    TemporaryFile file(".image"_s, { });
    dataLogLn(fileLeftPrefix, file.path());
    CRASH();
}

#if OS(LINUX) && (CPU(X86_64) || CPU(ARM64))

#endif

#if OS(LINUX)

#endif // OS(LINUX)

static void testCrashLeavesNoFiles()
{
    setCrashRequest(true);
    ChildProcessResult result = runWith(createLauncher(), { String(nativeSubcommandName), String(singleProcessFlag), "crash-on-request/first"_s });
    setCrashRequest(false);
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);

    size_t start = result.output.find(fileLeftPrefix);
    AOT_TEST_CHECK(start != notFound);
    start += fileLeftPrefix.length();
    String path = result.output.substring(start, result.output.find('\n', start) - start).trim(isASCIIWhitespace<char16_t>);
    AOT_TEST_CHECK(path.startsWith(temporaryFolder()) && path.endsWith(".image"_s));
    AOT_TEST_CHECK(!FileSystem::fileExists(path));
    AOT_TEST_CHECK(!FileSystem::fileExists(FileSystem::parentPath(path)));
}

std::span<const NativeTest> childProcessTests()
{
    static constexpr NativeTest tests[] = {
        { "child-process/heap-without-memory-ends-build-with-error"_s, testHeapWithoutMemoryEndsBuildWithError },
        { "child-process/shell-crash"_s, testShellCrash },
        { "child-process/crash-leaves-no-files"_s, testCrashLeavesNoFiles },
#if OS(LINUX) && (CPU(X86_64) || CPU(ARM64))
#endif
#if OS(LINUX)
#endif
        { "runner/file-whose-process-ends-early"_s, testFileWhoseProcessEndsEarlyIsNamed },
        { "crash-on-request/first"_s, testCrashesOnRequest },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
