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
static constexpr ASCIILiteral fileThatOwesCheck =
    "# TODO: nobody: some day\n"
    "# RUN: print\n"
    "# CHECK: Multiply\n"
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"negate\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Negate v0\n"
    "    Return v1\n"
    "}\n"_s;
static constexpr ASCIILiteral fileThatStopsVerifierUnderTodo =
    "# TODO: nobody: some day\n"
    "# RUN: verify\n"
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"_s;
static constexpr ASCIILiteral fileThatDiesUnderTodo =
    "# TODO: nobody: some day\n"
    "# RUN: crash without-memory-at-no-work\n"_s;
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

static void testTodoOfFileStandsForNoDeath()
{
    TemporaryFile owing(".ir"_s, byteCast<uint8_t>(fileThatOwesCheck.span8()));
    TemporaryFile stopping(".ir"_s, byteCast<uint8_t>(fileThatStopsVerifierUnderTodo.span8()));
    TemporaryFile dying(".ir"_s, byteCast<uint8_t>(fileThatDiesUnderTodo.span8()));
    ChildProcessLauncher launcher = createLauncher();
    TestRunRequest request;
    request.paths = { owing.path(), stopping.path(), dying.path() };
    request.launcher = &launcher;

    StringPrintStream out;
    TestRunSummary summary = runTestFiles(request, out);

    AOT_TEST_CHECK(!summary.passed && summary.failed == 2 && summary.todo == 1);
    AOT_TEST_CHECK(out.toString().contains(makeString("TODO "_s, owing.path(), ":1: nobody: some day\n"_s)));
    AOT_TEST_CHECK(out.toString().contains(makeString("FAIL "_s, stopping.path(), ":2: verify: unexpected diagnostic NoEntryPoint: "_s)));
    AOT_TEST_CHECK(out.toString().contains(makeString("FAIL "_s, dying.path(), ":1: crashed with "_s)));
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

static void testFailsCheckOnRequest()
{
    AOT_TEST_CHECK(!getenv(crashRequestVariable));
}

static void testTodoOfNativeTestStandsForNoDeath()
{
    static constexpr ASCIILiteral bothOnRequest[] { "crash-on-request/first"_s, "crash-on-request/fails-a-check"_s };
    ChildProcessLauncher launcher = createLauncher();
    NativeTestRequest request;
    request.filter = "crash-on-request/"_s;
    request.launcher = &launcher;
    request.testsTakenAsTodo = bothOnRequest;

    setCrashRequest(true);
    StringPrintStream out;
    NativeTestSummary summary = runNativeTests(request, out);
    setCrashRequest(false);

    AOT_TEST_CHECK(!summary.passed && summary.failed == 1 && summary.todo == 1);
    AOT_TEST_CHECK(out.toString().contains("FAIL crash-on-request/first: crashed with "_s));
    AOT_TEST_CHECK(out.toString().contains("TODO crash-on-request/fails-a-check: "_s));
}

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
        { "runner/file-whose-process-ends-early"_s, testFileWhoseProcessEndsEarlyIsNamed },
        { "runner/todo-of-a-file-stands-for-no-death"_s, testTodoOfFileStandsForNoDeath },
        { "runner/todo-of-a-native-test-stands-for-no-death"_s, testTodoOfNativeTestStandsForNoDeath },
        { "crash-on-request/first"_s, testCrashesOnRequest },
        { "crash-on-request/fails-a-check"_s, testFailsCheckOnRequest },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
