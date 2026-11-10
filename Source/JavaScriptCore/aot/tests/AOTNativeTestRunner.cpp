/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
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

static constexpr ASCIILiteral whereChecksOfNativeTestsFail = "native test"_s;

void failNativeTest(const char* file, int line, const char* condition)
{
    crashLock.lock();
    WTFReportAssertionFailure(file, line, whereChecksOfNativeTestsFail.characters(), condition);
    CRASH();
}

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

static constexpr ASCIILiteral longNativeTests[] = {
    "slow/compact-table/compiled-code-probes-index-wider-than-the-hash"_s,
    "id-sets/operations-lazy-lists"_s,
    "id-sets/operations-chunks"_s,
    "slow/compact-table/index-wider-than-the-hash"_s,
    "regexp-image/long-subject-allows-match-limit-turns"_s,
};

static void forEachNativeTest(NOESCAPE const Invocable<void(const NativeTest&)> auto& functor)
{
    auto forEachInWrittenOrder = [](NOESCAPE const Invocable<void(const NativeTest&)> auto& visit) {
        for (std::span<const NativeTestProvider> providers : { nativeTestProviders(), nativeTestProvidersOfHost() }) {
            for (NativeTestProvider provider : providers) {
                for (const NativeTest& test : provider())
                    visit(test);
            }
        }
    };
    for (ASCIILiteral name : longNativeTests) {
        forEachInWrittenOrder([&](const NativeTest& test) {
            if (StringView(test.name) == StringView(name))
                functor(test);
        });
    }
    forEachInWrittenOrder([&](const NativeTest& test) {
        if (!std::ranges::contains(longNativeTests, StringView(test.name), [](ASCIILiteral name) { return StringView(name); }))
            functor(test);
    });
}

static void checkNativeTests()
{
    HashSet<ASCIILiteral> names;
    forEachNativeTest([&](const NativeTest& test) {
        RELEASE_ASSERT_WITH_MESSAGE(!test.name.isEmpty(), "A native test has no name");
        RELEASE_ASSERT_WITH_MESSAGE(test.run, "Native test %s has no function to run", test.name.characters());
        bool isNewName = names.add(test.name).isNewEntry;
        RELEASE_ASSERT_WITH_MESSAGE(isNewName, "Two native tests are named %s", test.name.characters());
        RELEASE_ASSERT_WITH_MESSAGE(test.todo.isNull() || StringView(test.todo).find(": "_s) != notFound, "The todo of native test %s names no owner", test.name.characters());
    });
}

using NativeTestList = std::span<const NativeTest* const>;

static Vector<const NativeTest*> selectNativeTests(const NativeTestRequest& request)
{
    Vector<const NativeTest*> tests;
    bool isPastPreviousTest = request.previousTest.isEmpty();
    unsigned ordinal = 0;
    forEachNativeTest([&](const NativeTest& test) {
        bool isSelected = StringView(test.name).contains(request.filter) && request.part.contains(ordinal++);
        if (!isPastPreviousTest) {
            isPastPreviousTest = request.previousTest == test.name;
            return;
        }
        if (isSelected)
            tests.append(&test);
    });
    return tests;
}

static NativeTestSummary runInThisProcess(NativeTestList tests, PrintStream& out)
{
    printLineAfterEachChildProcess();
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

static StringView failedCheckIn(StringView output)
{
    constexpr ASCIILiteral start = "ASSERTION FAILED: "_s;
    size_t lineStart = output.find(start);
    if (lineStart == notFound)
        return { };
    size_t lineEnd = output.find('\n', lineStart);
    return output.substring(lineStart + start.length(), lineEnd == notFound ? lineEnd : lineEnd - lineStart - start.length());
}

static bool hasEndedAtCheckOfTest(const ChildProcessResult& result, StringView output)
{
    size_t lineStart = output.find("ASSERTION FAILED: "_s);
    if (result.outcome != ChildProcessOutcome::Crashed || lineStart == notFound || output.contains("ERROR: AddressSanitizer"_s))
        return false;
    size_t placeStart = output.find('\n', lineStart);
    if (placeStart == notFound)
        return false;
    size_t placeEnd = output.find('\n', placeStart + 1);
    StringView place = output.substring(placeStart + 1, placeEnd == notFound ? placeEnd : placeEnd - placeStart - 1);
    return place.endsWith(makeString(" : "_s, whereChecksOfNativeTestsFail));
}

static ASCIILiteral todoOf(const NativeTestRequest& request, const NativeTest& test)
{
    bool isTakenAsTodo = std::ranges::contains(request.testsTakenAsTodo, StringView(test.name), [](ASCIILiteral name) { return StringView(name); });
    return isTakenAsTodo ? "the request: it says so"_s : test.todo;
}

static NativeTestSummary runInChildProcesses(const NativeTestRequest& request, NativeTestList tests, PrintStream& out)
{
    NativeTestSummary summary;
    auto countPassed = [&](NativeTestList passed) {
        for (const NativeTest* test : passed) {
            if (todoOf(request, *test).isNull()) {
                ++summary.passed;
                continue;
            }
            out.println("FAIL ", test->name, ": passes: remove its todo");
            ++summary.failed;
        }
    };

    StringView previousTest = request.previousTest;
    while (!tests.empty()) {
        ChildProcessResult result = request.launcher->run(childArguments(request, previousTest).span());
        if (result.exitedWith(0)) {
            countPassed(tests);
            break;
        }

        ChildProgress progress = progressOf(result.output, tests);
        if (!progress.startedTestCount) {
            out.print(result.output);
            out.println("FAIL ", tests[0]->name, ": ", result.description(), " before the test");
            summary.failed += tests.size();
            break;
        }

        const NativeTest& failed = *tests[progress.startedTestCount - 1];
        ASCIILiteral failedTest = failed.name;
        ASCIILiteral todo = todoOf(request, failed);
        if (todo.isNull() || !hasEndedAtCheckOfTest(result, progress.lastTestOutput)) {
            out.print(progress.lastTestOutput);
            out.println("FAIL ", failedTest, ": ", result.description());
            ++summary.failed;
        } else {
            out.println("TODO ", failedTest, ": ", todo);
            out.println("  still fails: ", result.description(), ": ", failedCheckIn(progress.lastTestOutput));
            ++summary.todo;
        }
        countPassed(tests.first(progress.startedTestCount - 1));
        previousTest = failedTest;
        tests = tests.subspan(progress.startedTestCount);
    }
    return summary;
}

NativeTestSummary runNativeTests(const NativeTestRequest& request, PrintStream& out)
{
    checkNativeTests();

    Vector<const NativeTest*> tests = selectNativeTests(request);
    return request.launcher ? runInChildProcesses(request, tests.span(), out) : runInThisProcess(tests.span(), out);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
