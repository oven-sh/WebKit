/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTNativeTest.h"
#include "AOTRuntimeTable.h"
#include "AOTTestFile.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using namespace Typed;

static constexpr ASCIILiteral sampleProgram =
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"sum\" parameters=[{type=number}, {type=number}] results=[{type=number}] {\n"
    "  b0(v0: number, v1: number):\n"
    "    v2 = Add v0, v1\n"
    "    Return v2\n"
    "}\n"_s;

static void expectRunLines(ASCIILiteral text, Architecture architecture, std::initializer_list<unsigned> lines)
{
    TestFile file = parseTestFile(text, architecture);
    AOT_TEST_CHECK(file.errors.isEmpty());
    AOT_TEST_CHECK(std::ranges::equal(file.runs, lines, { }, &RunDirective::line));
}

static void expectErrorLines(ASCIILiteral text, std::initializer_list<unsigned> lines)
{
    for (Architecture architecture : { Architecture::X86_64, Architecture::ARM64 }) {
        TestFile file = parseTestFile(text, architecture);
        AOT_TEST_CHECK(std::ranges::equal(file.errors, lines, { }, &TestFileError::line));
    }
}

static void testRequiresSelectsRuns()
{
    static constexpr ASCIILiteral prefixes =
        "# RUN: print\n"
        "# RUN(X64): print\n"
        "# RUN(ARM): print\n"
        "# RUN(X64): verify\n"
        "# REQUIRES(X64): x86_64\n"
        "# REQUIRES(ARM): arm64\n"
        "# X64: a\n"
        "# ARM: b\n"_s;
    expectRunLines(prefixes, Architecture::X86_64, { 1, 2, 4 });
    expectRunLines(prefixes, Architecture::ARM64, { 1, 3 });
    AOT_TEST_CHECK(parseTestFile(prefixes, Architecture::X86_64).checkLinesFor("ARM"_s).size() == 1);

    static constexpr ASCIILiteral defaultPrefix =
        "# REQUIRES: arm64\n"
        "# RUN: print\n"_s;
    expectRunLines(defaultPrefix, Architecture::X86_64, { });
    expectRunLines(defaultPrefix, Architecture::ARM64, { 2 });

    static constexpr ASCIILiteral exactCode =
        "# RUN: print\n"
        "# RUN(X64): print\n"
        "# REQUIRES(X64): x86_64 unsanitized-code\n"
        "# REQUIRES: unsanitized-code\n"
        "# X64: a\n"_s;
    if (AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE)
        expectRunLines(exactCode, Architecture::X86_64, { });
    else
        expectRunLines(exactCode, Architecture::X86_64, { 1, 2 });

    static constexpr ASCIILiteral codeOfOtherSystems =
        "# RUN: print\n"
        "# RUN(X64): print\n"
        "# REQUIRES(X64): x86_64 not-windows\n"
        "# X64: a\n"_s;
#if OS(WINDOWS)
    expectRunLines(codeOfOtherSystems, Architecture::X86_64, { 1 });
#else
    expectRunLines(codeOfOtherSystems, Architecture::X86_64, { 1, 2 });
#endif

    static constexpr ASCIILiteral codeWithFaultingPolls =
        "# RUN: print\n"
        "# RUN(X64): print\n"
        "# REQUIRES(X64): x86_64 faulting-loop-polls\n"
        "# X64: a\n"_s;
    if (hasFaultingLoopPolls)
        expectRunLines(codeWithFaultingPolls, Architecture::X86_64, { 1, 2 });
    else
        expectRunLines(codeWithFaultingPolls, Architecture::X86_64, { 1 });
}

static void testRequiresMalformed()
{
    expectErrorLines("# RUN: print\n# REQUIRES: mips\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES:\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: x86_64 arm64\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: unsanitized-code unsanitized-code\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: not-windows not-windows\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: faulting-loop-polls faulting-loop-polls\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES(OTHER): arm64\n"_s, { 2 });
    expectErrorLines("# RUN(ONLY): print\n# ONLY: x\n# REQUIRES: arm64\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# REQUIRES: arm64\n# REQUIRES: x86_64\n"_s, { 3 });
    expectErrorLines("# RUN(REQUIRES): print\n"_s, { 1 });

    expectErrorLines("# RUN(ONLY): print\n# REQUIRES(ONLY): arm64\n# ONLY-NXT: x\n"_s, { 3 });
    expectErrorLines("# RUN(ONLY): print\n# REQUIRES(ONLY): arm64\n"_s, { 1 });
}

static void testRunnerSkipsOtherArchitecture()
{
    constexpr bool targetsX86_64 = hostArchitecture() == Architecture::X86_64;

    Vector<TestFailure> failures = runTestText(makeString(
        "# RUN(X64): print\n"
        "# RUN(ARM): print\n"
        "# REQUIRES(X64): x86_64\n"
        "# REQUIRES(ARM): arm64\n"
        "# X64: Negate\n"
        "# ARM: Negate\n"_s, sampleProgram));
    AOT_TEST_CHECK(failures.size() == 1);
    AOT_TEST_CHECK(failures[0].line == (targetsX86_64 ? 5u : 6u));

    ASCIILiteral otherArchitecture = targetsX86_64 ? "arm64"_s : "x86_64"_s;
    AOT_TEST_CHECK(runTestText(makeString("# RUN: print\n# CHECK: Negate\n# REQUIRES: "_s, otherArchitecture, '\n', sampleProgram)).isEmpty());
}

std::span<const NativeTest> testFileTests()
{
    static constexpr NativeTest tests[] = {
        { "test-file/requires-selects-runs"_s, testRequiresSelectsRuns },
        { "test-file/requires-malformed"_s, testRequiresMalformed },
        { "runner/skips-other-architecture"_s, testRunnerSkipsOtherArchitecture },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
