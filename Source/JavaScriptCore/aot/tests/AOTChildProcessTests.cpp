/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChildProcess.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTTemporaryFile.h"
#include "AOTTestRunner.h"
#include "AOTTestSubcommands.h"
#include "AOTVector.h"
#include <stdlib.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace JSC { namespace AOT {

using namespace Typed;

static constexpr const char* crashRequestVariable = "TESTAOT_CRASH_ON_REQUEST";
static constexpr ASCIILiteral crashOnRequestFilter = "crash-on-request"_s;

static constexpr ASCIILiteral passingFile =
    "# RUN: print\n"
    "# CHECK: Negate\n"
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"negate\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Negate v0\n"
    "    Return v1\n"
    "}\n"_s;
static constexpr ASCIILiteral crashingFile = "# RUN: crash\n"_s;
static constexpr ASCIILiteral failingFile = "# RUN: no-such-command\n"_s;
static constexpr ASCIILiteral owedFile = "# TODO: somebody: the thing that is lost\n# RUN: no-such-command\n"_s;
static constexpr ASCIILiteral paidFile =
    "# TODO: somebody: the thing that is lost\n"
    "# RUN: print\n"
    "# CHECK: Negate\n"
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"negate\" parameters=[{type=number}] results=[{type=number}] {\n"
    "  b0(v0: number):\n"
    "    v1 = Negate v0\n"
    "    Return v1\n"
    "}\n"_s;
static constexpr ASCIILiteral crashingFileWithTodo = "# TODO: somebody: the thing that is lost\n# RUN: crash\n"_s;
static constexpr ASCIILiteral fileWhoseTodoNamesNothing = "# TODO: somebody\n# RUN: no-such-command\n"_s;

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

static void testExitCodes()
{
    ChildProcessLauncher launcher = createLauncher();
    ChildProcessResult listed = runWith(launcher, { "list"_s, "commands"_s });
    AOT_TEST_CHECK(listed.exitedWith(exitCodeForSuccess));
    AOT_TEST_CHECK(listed.output.contains("verify"_s));

    ChildProcessResult rejected = runWith(launcher, { "no-such-subcommand"_s });
    AOT_TEST_CHECK(rejected.exitedWith(exitCodeForInvalidCommandLine));
    AOT_TEST_CHECK(rejected.output.contains("usage: testaot"_s));
}

static void testSharedArguments()
{
    AOT_TEST_CHECK(runWith(createLauncher({ "--useDollarVM=1"_s }), { "list"_s, "commands"_s }).exitedWith(exitCodeForSuccess));
    AOT_TEST_CHECK(runWith(createLauncher({ "--noSuchOption=1"_s }), { "list"_s, "commands"_s }).exitedWith(exitCodeForInvalidCommandLine));
}

static void testCrash()
{
    TemporaryFile file(".ir"_s, byteCast<uint8_t>(crashingFile.span8()));
    ChildProcessResult result = runWith(createLauncher(), { String(runSubcommandName), file.path() });
    AOT_TEST_CHECK(result.outcome == ChildProcessOutcome::Crashed);
    AOT_TEST_CHECK(result.description().startsWith("crashed with "_s));
}

static void testNotStarted()
{
    ChildProcessLauncher launcher("/nonexistent-directory/testaot"_s, { });
    AOT_TEST_CHECK(runWith(launcher, { "list"_s, "commands"_s }).outcome == ChildProcessOutcome::NotStarted);
}

static void testFilesAfterCrashStillRun()
{
    TemporaryFile crashing(".ir"_s, byteCast<uint8_t>(crashingFile.span8()));
    TemporaryFile failing(".ir"_s, byteCast<uint8_t>(failingFile.span8()));
    TemporaryFile passing(".ir"_s, byteCast<uint8_t>(passingFile.span8()));
    ChildProcessLauncher launcher = createLauncher();
    TestRunRequest request;
    request.paths = { crashing.path(), failing.path(), passing.path() };
    request.launcher = &launcher;

    StringPrintStream out;
    TestRunSummary summary = runTestFiles(request, out);

    AOT_TEST_CHECK(summary.passed == 1 && summary.failed == 2);
    String report = out.toString();
    size_t crashLine = report.find(makeString("FAIL "_s, crashing.path(), ":1: crashed with "_s));
    size_t failureLine = report.find(makeString("FAIL "_s, failing.path(), ":1: unknown command no-such-command\n"_s));
    AOT_TEST_CHECK(crashLine != notFound && failureLine != notFound && crashLine < failureLine);
    AOT_TEST_CHECK(!report.contains(passing.path()));
    AOT_TEST_CHECK(!report.contains(" passed, "_s));
}

static void checkTodoLines(const ChildProcessLauncher* launcher)
{
    TemporaryFile owed(".ir"_s, byteCast<uint8_t>(owedFile.span8()));
    TemporaryFile paid(".ir"_s, byteCast<uint8_t>(paidFile.span8()));
    TemporaryFile unnamed(".ir"_s, byteCast<uint8_t>(fileWhoseTodoNamesNothing.span8()));
    TemporaryFile passing(".ir"_s, byteCast<uint8_t>(passingFile.span8()));
    TemporaryFile crashing(".ir"_s, byteCast<uint8_t>(crashingFileWithTodo.span8()));
    TestRunRequest request;
    request.paths = { owed.path(), paid.path(), unnamed.path(), passing.path() };
    if (launcher)
        request.paths.append(crashing.path());
    request.launcher = launcher;

    StringPrintStream out;
    TestRunSummary summary = runTestFiles(request, out);

    AOT_TEST_CHECK(summary.passed == 1 && summary.todo == 1 && summary.failed == (launcher ? 3u : 2u));
    String report = out.toString();
    AOT_TEST_CHECK(report.contains(makeString("TODO "_s, owed.path(), ":1: somebody: the thing that is lost\n"_s)));
    AOT_TEST_CHECK(report.contains(makeString("  still fails: "_s, owed.path(), ":2: unknown command no-such-command\n"_s)));
    AOT_TEST_CHECK(!report.contains(makeString("FAIL "_s, owed.path())));
    AOT_TEST_CHECK(report.contains(makeString("FAIL "_s, paid.path(), ":1: passes: remove the TODO line\n"_s)));
    AOT_TEST_CHECK(report.contains(makeString("FAIL "_s, unnamed.path(), ":1: TODO is written "_s)));
    AOT_TEST_CHECK(!launcher || report.contains(makeString("FAIL "_s, crashing.path(), ":1: crashed with "_s)));
    AOT_TEST_CHECK(!report.contains(passing.path()));
}

static void testTodoLines()
{
    ChildProcessLauncher launcher = createLauncher();
    checkTodoLines(&launcher);
    checkTodoLines(nullptr);
}

static void testOutputOfPassingFileIsShown()
{
    TemporaryFile first(".ir"_s, byteCast<uint8_t>(passingFile.span8()));
    TemporaryFile second(".ir"_s, byteCast<uint8_t>(passingFile.span8()));
    ChildProcessLauncher launcher = createLauncher();
    TestRunRequest request;
    request.paths = { first.path(), second.path() };
    request.showsOutput = true;
    request.launcher = &launcher;

    StringPrintStream out;
    TestRunSummary summary = runTestFiles(request, out);

    AOT_TEST_CHECK(summary.passed == 2 && !summary.failed);
    String report = out.toString();
    AOT_TEST_CHECK(report.contains(makeString("OUTPUT "_s, second.path(), ":1:\n"_s)));
    AOT_TEST_CHECK(report.contains("Negate v0"_s));
}

static void testDoesNothing()
{
}

static void testCrashesOnRequest()
{
    if (getenv(crashRequestVariable))
        CRASH();
}

static void testNativeTestsAfterCrashStillRun()
{
    ChildProcessLauncher launcher = createLauncher();
    NativeTestRequest request { crashOnRequestFilter, { }, &launcher };

    StringPrintStream quietOut;
    NativeTestSummary summary = runNativeTests(request, quietOut);
    AOT_TEST_CHECK(summary.passed == 4 && !summary.failed);
    AOT_TEST_CHECK(quietOut.toString().isEmpty());

    setCrashRequest(true);
    StringPrintStream out;
    summary = runNativeTests(request, out);
    setCrashRequest(false);

    AOT_TEST_CHECK(summary.passed == 2 && summary.failed == 2);
    String report = out.toString();
    size_t firstCrash = report.find("FAIL crash-on-request/first: crashed with "_s);
    size_t secondCrash = report.find("FAIL crash-on-request/second: crashed with "_s);
    AOT_TEST_CHECK(firstCrash != notFound && secondCrash != notFound && firstCrash < secondCrash);
}

static void testNativeTestsAfterNamedTest()
{
    ChildProcessLauncher launcher = createLauncher();
    StringPrintStream out;
    NativeTestSummary summary = runNativeTests({ crashOnRequestFilter, "crash-on-request/first"_s, &launcher }, out);
    AOT_TEST_CHECK(summary.passed == 2 && !summary.failed);
}

static void testNativeTestsInParts()
{
    ChildProcessLauncher launcher = createLauncher();
    auto runPart = [&](unsigned index, unsigned count, StringView previousTest, PrintStream& out) {
        return runNativeTests({ crashOnRequestFilter, previousTest, &launcher, { index, count } }, out);
    };

    StringPrintStream quietOut;
    for (unsigned index = 0; index < 2; ++index) {
        NativeTestSummary summary = runPart(index, 2, { }, quietOut);
        AOT_TEST_CHECK(summary.passed == 2 && !summary.failed);
    }
    NativeTestSummary fifthOfFour = runPart(4, 5, { }, quietOut);
    AOT_TEST_CHECK(!fifthOfFour.passed && !fifthOfFour.failed);

    // Of before, first, second, after the second part is first and after, also for whoever starts behind before. Counted from there it would be second alone.
    NativeTestSummary behindBefore = runPart(1, 2, "crash-on-request/before"_s, quietOut);
    AOT_TEST_CHECK(behindBefore.passed == 2 && !behindBefore.failed);
    AOT_TEST_CHECK(quietOut.toString().isEmpty());

    setCrashRequest(true);
    StringPrintStream firstOut;
    NativeTestSummary firstPart = runPart(0, 2, { }, firstOut);
    StringPrintStream secondOut;
    NativeTestSummary secondPart = runPart(1, 2, { }, secondOut);
    setCrashRequest(false);

    AOT_TEST_CHECK(firstPart.passed == 1 && firstPart.failed == 1);
    AOT_TEST_CHECK(secondPart.passed == 1 && secondPart.failed == 1);
    AOT_TEST_CHECK(firstOut.toString().contains("FAIL crash-on-request/second: crashed with "_s) && !firstOut.toString().contains("crash-on-request/first"_s));
    AOT_TEST_CHECK(secondOut.toString().contains("FAIL crash-on-request/first: crashed with "_s) && !secondOut.toString().contains("crash-on-request/second"_s));
}

static void testPartOnCommandLine()
{
    ChildProcessLauncher launcher = createLauncher();
    for (ASCIILiteral part : { "--part=1/2"_s, "--part=2/2"_s, "--part=1/1"_s, "--part=5/5"_s })
        AOT_TEST_CHECK(runWith(launcher, { String(nativeSubcommandName), String(part), String(crashOnRequestFilter) }).exitedWith(exitCodeForSuccess));
    for (ASCIILiteral part : { "--part"_s, "--part="_s, "--part=1"_s, "--part=0/2"_s, "--part=3/2"_s, "--part=1/0"_s, "--part=a/2"_s, "--part=1/b"_s, "--part=-1/2"_s })
        AOT_TEST_CHECK(runWith(launcher, { String(nativeSubcommandName), String(part), String(crashOnRequestFilter) }).exitedWith(exitCodeForInvalidCommandLine));
}

std::span<const NativeTest> childProcessTests()
{
    static constexpr NativeTest tests[] = {
        { "child-process/exit-codes"_s, testExitCodes },
        { "child-process/shared-arguments"_s, testSharedArguments },
        { "child-process/crash"_s, testCrash },
        { "child-process/not-started"_s, testNotStarted },
        { "runner/files-after-crash"_s, testFilesAfterCrashStillRun },
        { "runner/shows-output-of-children"_s, testOutputOfPassingFileIsShown },
        { "runner/todo-lines"_s, testTodoLines },
        { "crash-on-request/before"_s, testDoesNothing },
        { "crash-on-request/first"_s, testCrashesOnRequest },
        { "crash-on-request/second"_s, testCrashesOnRequest },
        { "crash-on-request/after"_s, testDoesNothing },
        { "native-tests/after-crash"_s, testNativeTestsAfterCrashStillRun },
        { "native-tests/after-named-test"_s, testNativeTestsAfterNamedTest },
        { "native-tests/parts"_s, testNativeTestsInParts },
        { "native-tests/part-on-command-line"_s, testPartOnCommandLine },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
