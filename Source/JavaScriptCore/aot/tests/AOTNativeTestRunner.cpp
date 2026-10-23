/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTHostEntriesTestSupport.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/Assertions.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static Lock crashLock;

void failNativeTest(const char* file, int line, const char* condition)
{
    crashLock.lock();
    WTFReportAssertionFailure(file, line, "native test", condition);
    CRASH();
}

using NativeTestProvider = std::span<const NativeTest> (*)();

static std::span<const NativeTestProvider> nativeTestProviders()
{
#define AOT_IR_NATIVE_TEST_PROVIDER_ADDRESS(name) &Typed::name,
#define AOT_ENGINE_NATIVE_TEST_PROVIDER_ADDRESS(name) &name,
#define AOT_IMAGE_NATIVE_TEST_PROVIDER_ADDRESS(name) &ImageTests::name,
    static constexpr NativeTestProvider providers[] = {
        FOR_EACH_AOT_IR_NATIVE_TEST_PROVIDER(AOT_IR_NATIVE_TEST_PROVIDER_ADDRESS)
        FOR_EACH_AOT_ENGINE_NATIVE_TEST_PROVIDER(AOT_ENGINE_NATIVE_TEST_PROVIDER_ADDRESS)
        FOR_EACH_AOT_IMAGE_NATIVE_TEST_PROVIDER(AOT_IMAGE_NATIVE_TEST_PROVIDER_ADDRESS)
    };
#undef AOT_IR_NATIVE_TEST_PROVIDER_ADDRESS
#undef AOT_ENGINE_NATIVE_TEST_PROVIDER_ADDRESS
#undef AOT_IMAGE_NATIVE_TEST_PROVIDER_ADDRESS
    return providers;
}

static void forEachNativeTest(NOESCAPE const Invocable<void(const NativeTest&)> auto& functor)
{
    for (auto provider : nativeTestProviders()) {
        for (const NativeTest& test : provider())
            functor(test);
    }
}

static void checkNativeTests()
{
    HashSet<ASCIILiteral> names;
    forEachNativeTest([&](const NativeTest& test) {
        RELEASE_ASSERT_WITH_MESSAGE(!test.name.isEmpty(), "A native test has no name");
        RELEASE_ASSERT_WITH_MESSAGE(test.run, "Native test %s has no function to run", test.name.characters());
        bool isNewName = names.add(test.name).isNewEntry;
        RELEASE_ASSERT_WITH_MESSAGE(isNewName, "Two native tests are named %s", test.name.characters());
    });
}

using NativeTestList = std::span<const NativeTest* const>;

// They name entries of Bun, or need roles that the stand-in host leaves out. A program that is linked with Bun's table runs them.
static constexpr ASCIILiteral testsOfHostEntries[] = {
#include "AOTNativeTestsOfHostEntries.inc"
};

static bool needsHostEntries(const NativeTest& test)
{
    return std::ranges::contains(testsOfHostEntries, StringView(test.name), [](ASCIILiteral name) { return StringView(name); });
}

struct SelectedNativeTests {
    Vector<const NativeTest*> run;
    Vector<const NativeTest*> leftOut;
};

static SelectedNativeTests selectNativeTests(const NativeTestRequest& request)
{
    SelectedNativeTests tests;
    bool leavesOutTestsOfHostEntries = hasStandInHost();
    bool isPastPreviousTest = request.previousTest.isEmpty();
    // Counted from the start of the list: the child that goes on behind a crash has to take the same tests.
    unsigned ordinal = 0;
    forEachNativeTest([&](const NativeTest& test) {
        bool isSelected = StringView(test.name).contains(request.filter) && request.part.contains(ordinal++);
        if (!isPastPreviousTest) {
            isPastPreviousTest = request.previousTest == test.name;
            return;
        }
        if (isSelected)
            (leavesOutTestsOfHostEntries && needsHostEntries(test) ? tests.leftOut : tests.run).append(&test);
    });
    return tests;
}

static NativeTestSummary runInThisProcess(NativeTestList tests, PrintStream& out)
{
    for (const NativeTest* test : tests) {
        out.println(test->name);
        out.flush();
        test->run();
    }
    return { static_cast<unsigned>(tests.size()), 0, 0 };
}

struct ChildProgress {
    size_t startedTestCount { 0 };
    StringView lastTestOutput;
};

static ChildProgress progressOf(StringView output, NativeTestList tests)
{
    ChildProgress progress;
    size_t lineStart = 0;
    while (progress.startedTestCount < tests.size()) {
        size_t lineEnd = output.find('\n', lineStart);
        if (lineEnd == notFound)
            break;
        StringView line = output.substring(lineStart, lineEnd - lineStart);
        // C's stdout ends a line with \r\n on Windows.
        if (line.endsWith('\r'))
            line = line.left(line.length() - 1);
        if (line == tests[progress.startedTestCount]->name) {
            ++progress.startedTestCount;
            progress.lastTestOutput = output.substring(lineEnd + 1);
        }
        lineStart = lineEnd + 1;
    }
    return progress;
}

static Vector<String> childArguments(const NativeTestRequest& request, StringView previousTest)
{
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag) };
    if (!previousTest.isEmpty())
        arguments.append(makeString(previousNativeTestFlag, '=', previousTest));
    if (request.part.count > 1)
        arguments.append(makeString(nativeTestPartFlag, '=', request.part.index + 1, '/', request.part.count));
    if (!request.filter.isEmpty())
        arguments.append(request.filter.toString());
    return arguments;
}

static NativeTestSummary runInChildProcesses(const NativeTestRequest& request, NativeTestList tests, PrintStream& out)
{
    NativeTestSummary summary;
    StringView previousTest = request.previousTest;
    while (!tests.empty()) {
        ChildProcessResult result = request.launcher->run(childArguments(request, previousTest).span());
        if (result.exitedWith(0)) {
            summary.passed += tests.size();
            break;
        }

        ChildProgress progress = progressOf(result.output, tests);
        if (!progress.startedTestCount) {
            out.print(result.output);
            out.println("FAIL ", tests[0]->name, ": ", result.description(), " before the test");
            summary.failed += tests.size();
            break;
        }

        ASCIILiteral failedTest = tests[progress.startedTestCount - 1]->name;
        out.print(progress.lastTestOutput);
        out.println("FAIL ", failedTest, ": ", result.description());
        summary.passed += progress.startedTestCount - 1;
        ++summary.failed;
        previousTest = failedTest;
        tests = tests.subspan(progress.startedTestCount);
    }
    return summary;
}

NativeTestSummary runNativeTests(const NativeTestRequest& request, PrintStream& out)
{
    checkNativeTests();

    SelectedNativeTests tests = selectNativeTests(request);
    for (const NativeTest* test : tests.leftOut)
        out.println("LEFT OUT ", test->name);
    NativeTestSummary summary = request.launcher ? runInChildProcesses(request, tests.run.span(), out) : runInThisProcess(tests.run.span(), out);
    summary.leftOut = tests.leftOut.size();
    return summary;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
