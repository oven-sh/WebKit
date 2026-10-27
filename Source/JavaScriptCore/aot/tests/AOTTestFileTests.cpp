/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
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

    static constexpr ASCIILiteral codeOfWindows =
        "# RUN: print\n"
        "# RUN(ARM): print\n"
        "# REQUIRES(ARM): arm64 windows\n"
        "# ARM: a\n"_s;
#if OS(WINDOWS)
    expectRunLines(codeOfWindows, Architecture::ARM64, { 1, 2 });
#else
    expectRunLines(codeOfWindows, Architecture::ARM64, { 1 });
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

    static constexpr ASCIILiteral loadsCodeOutsideImage =
        "# RUN: print\n"
        "# RUN(LOADS): print\n"
        "# REQUIRES(LOADS): code-outside-image\n"
        "# LOADS: a\n"_s;
    if (runsOnlyCodeOfImage)
        expectRunLines(loadsCodeOutsideImage, Architecture::X86_64, { 1 });
    else
        expectRunLines(loadsCodeOutsideImage, Architecture::X86_64, { 1, 2 });

    static constexpr ASCIILiteral convertsWithSlowPath =
        "# RUN: print\n"
        "# RUN(SLOW): print\n"
        "# REQUIRES(SLOW): no-jscvt\n"
        "# SLOW: a\n"_s;
    if (targetHasJSCVT)
        expectRunLines(convertsWithSlowPath, Architecture::ARM64, { 1 });
    else
        expectRunLines(convertsWithSlowPath, Architecture::ARM64, { 1, 2 });
}

static void testRequiresMalformed()
{
    expectErrorLines("# RUN: print\n# REQUIRES: mips\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES:\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: x86_64 arm64\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: unsanitized-code unsanitized-code\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: not-windows not-windows\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: faulting-loop-polls faulting-loop-polls\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: code-outside-image code-outside-image\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: no-jscvt no-jscvt\n"_s, { 2 });
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

static void testRepeatIsWrittenOut()
{
    TestFile file = parseTestFile("# RUN: print\n# REPEAT: i = 1..2; word = a b\n# CHECK: %(word)%(i*2-1:2)\n# REPEAT: j = 8..9\n# CHECK: %(i)%(j)\n# END REPEAT\n# END REPEAT\n# CHECK: end\n"_s);
    AOT_TEST_CHECK(file.errors.isEmpty());
    AOT_TEST_CHECK(file.text == "# RUN: print\n# CHECK: a01\n# CHECK: 18\n# CHECK: 19\n# CHECK: b03\n# CHECK: 28\n# CHECK: 29\n# CHECK: end\n"_s);
    AOT_TEST_CHECK(std::ranges::equal(file.checkLinesFor("CHECK"_s), std::initializer_list<unsigned> { 3, 5, 5, 3, 5, 5, 8 }, { }, &CheckLine::line));
}

static void testCheckLinesInsideFunctionHaveLabel()
{
    TestFile file = parseTestFile("# RUN: print\n# RUN(OTHER): print\nfunction f0 name=\"a b\" results=[] {\n    Return\n# CHECK: x\n# plain words\n# OTHER-NOT: y\n# CHECK-NEXT: z\n}\n# CHECK: end\nfunction f1 name=\"c\" {\n    Return\n# CHECK: w\n}\n"_s);
    AOT_TEST_CHECK(file.errors.isEmpty());
    std::span<const CheckLine> lines = file.checkLinesFor("CHECK"_s);
    AOT_TEST_CHECK(std::ranges::equal(lines, std::initializer_list<unsigned> { 5, 5, 8, 10, 13, 13 }, { }, &CheckLine::line));
    AOT_TEST_CHECK(lines[0].kind == CheckKind::Label && lines[0].pattern == "function f0 name=\"a b\""_s);
    AOT_TEST_CHECK(lines[4].kind == CheckKind::Label && lines[4].pattern == "function f1 name=\"c\""_s);
    std::span<const CheckLine> otherLines = file.checkLinesFor("OTHER"_s);
    AOT_TEST_CHECK(otherLines.size() == 2 && otherLines[0].kind == CheckKind::Label && otherLines[1].kind == CheckKind::Not);
    expectErrorLines("# RUN: print\nfunction f0 results=[] {\n    Return\n# CHECK: x\n}\n"_s, { 4 });
}

static void testRepeatMalformed()
{
    expectErrorLines("# RUN: print\n# REPEAT: i = 0..1\n# CHECK: x\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# CHECK: x\n# END REPEAT\n# REPEAT: i = 0..1\n# END REPEAT\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# REPEAT:\n# CHECK: x\n# END REPEAT\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REPEAT: i\n# CHECK: x\n# END REPEAT\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REPEAT: i =\n# CHECK: x\n# END REPEAT\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REPEAT: i = 0..2; word = a b\n# CHECK: x\n# END REPEAT\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REPEAT: i = 0..9999999\n# END REPEAT\n# CHECK: x\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REPEAT: i = 0..1\n# CHECK: %(j)\n# END REPEAT\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# REPEAT: i = 0..1\n# CHECK: %(i\n# END REPEAT\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# REPEAT: i = 0..1\n# CHECK: %(i*)\n# END REPEAT\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# REPEAT: word = a b\n# CHECK: %(word+1)\n# END REPEAT\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# CHECK: x\n# PROGRAM-OF: a-text-has-no-folder.ir\n"_s, { 3 });
}

static void testWordsOfCommands()
{
    Vector<String> words = String("3 f0 v3 f0:v3 f4:v0 b0:2 f0 v3:f0 f:v3 f0:"_s).split(' ');

    AOT_TEST_CHECK(parseIndexBelow(words[0], 4) == 3u);
    AOT_TEST_CHECK(!parseIndexBelow(words[0], 3));
    AOT_TEST_CHECK(parseIndexBelow(words[0], UINT32_MAX) == 3u);

    AOT_TEST_CHECK(idParser<FunctionID>('f', 1)(words[1]) == FunctionID(0u));
    AOT_TEST_CHECK(idParser<FunctionID>('f', UINT32_MAX)(words[1]) == FunctionID(0u));
    AOT_TEST_CHECK(idParser<ValueID>('v', UINT32_MAX)(words[2]) == ValueID(3u));
    AOT_TEST_CHECK(!idParser<ValueID>('v', UINT32_MAX)(words[1]));

    AOT_TEST_CHECK(pairParser(':', idParser<FunctionID>('f', 1), idParser<ValueID>('v', 4))(words[3]) == std::pair(FunctionID(0u), ValueID(3u)));
    AOT_TEST_CHECK(pairParser(':', idParser<BlockID>('b', 1), indexParser(UINT32_MAX))(words[5]) == std::pair(BlockID(0u), 2u));

    auto parseValueOfFunction = pairParser(':', idParser<FunctionID>('f', UINT32_MAX), idParser<ValueID>('v', UINT32_MAX));
    AOT_TEST_CHECK(parseValueOfFunction(words[3]) == std::pair(FunctionID(0u), ValueID(3u)));
    AOT_TEST_CHECK(parseValueOfFunction(words[4]) == std::pair(FunctionID(4u), ValueID(0u)));
    unsigned taken = 0;
    for (const String& word : words)
        taken += !!parseValueOfFunction(word);
    AOT_TEST_CHECK(taken == 2);
}

std::span<const NativeTest> testFileTests()
{
    static constexpr NativeTest tests[] = {
        { "test-file/requires-selects-runs"_s, testRequiresSelectsRuns },
        { "test-file/requires-malformed"_s, testRequiresMalformed },
        { "test-file/repeat-is-written-out"_s, testRepeatIsWrittenOut },
        { "test-file/repeat-malformed"_s, testRepeatMalformed },
        { "test-file/check-lines-inside-function-have-label"_s, testCheckLinesInsideFunctionHaveLabel },
        { "runner/skips-other-architecture"_s, testRunnerSkipsOtherArchitecture },
        { "test-commands/words"_s, testWordsOfCommands },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
