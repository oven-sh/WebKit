/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodiesThatCodeDependsOn.h"
#include "AOTCodeSites.h"
#include "AOTCompileOptions.h"
#include "AOTNativeTest.h"
#include "AOTTestFile.h"
#include "AOTTestRunner.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <wtf/DataLog.h>
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
    if (ASAN_ENABLED)
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

    static constexpr ASCIILiteral codeWithCallingPolls =
        "# RUN: print\n"
        "# RUN(CALLS): print\n"
        "# REQUIRES(CALLS): calling-loop-polls\n"
        "# CALLS: a\n"_s;
    if (hasFaultingLoopPolls)
        expectRunLines(codeWithCallingPolls, Architecture::ARM64, { 1 });
    else
        expectRunLines(codeWithCallingPolls, Architecture::ARM64, { 1, 2 });

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
    expectErrorLines("# RUN: print\n# REQUIRES: calling-loop-polls calling-loop-polls\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: code-outside-image code-outside-image\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES: no-jscvt no-jscvt\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# REQUIRES(OTHER): arm64\n"_s, { 2 });
    expectErrorLines("# RUN(ONLY): print\n# ONLY: x\n# REQUIRES: arm64\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# REQUIRES: arm64\n# REQUIRES: x86_64\n"_s, { 3 });
    expectErrorLines("# RUN(REQUIRES): print\n"_s, { 1 });

    expectErrorLines("# RUN(ONLY): print\n# REQUIRES(ONLY): arm64\n# ONLY-NXT: x\n"_s, { 3 });
    expectErrorLines("# RUN(ONLY): print\n# REQUIRES(ONLY): arm64\n"_s, { 1 });
}

static void expectErrorInThirdLine(ASCIILiteral line, bool isError = true)
{
    String text = makeString("# RUN: print\n# CHECK: a\n"_s, line, '\n');
    TestFile file = parseTestFile(WTF::move(text), Architecture::X86_64);
    if (file.errors.size() != isError || (isError && file.errors[0].line != 3))
        dataLogLn(line, ": ", file.errors.size(), " errors", file.errors.isEmpty() ? ""_s : ": "_s, file.errors.isEmpty() ? String() : file.errors[0].message);
    AOT_TEST_CHECK(file.errors.size() == isError && (!isError || file.errors[0].line == 3));
}

static void testDirectiveThatWentWrong()
{
    for (ASCIILiteral line : {
        "# CHECK-NEXT v0 = Add"_s, "# CHECK v0 = Add"_s, "# RUN print"_s, "# STATISTIC ValuesWalked = 1"_s, "# CHECK"_s,
        "# check: a"_s, "# Check-Next: a"_s, "# check-next a"_s, "# Options: x"_s,
        "#CHECK: a"_s, "#  CHECK: a"_s, "#\tCHECK: a"_s, "# CHECK : a"_s, "# CHECK-NOT : a"_s, "    #CHECK: a"_s,
        "# CHEKC: a"_s, "# CHCK: a"_s, "# CHECK-NXT: a"_s, "# CHECK-NEX a"_s, "# CHECKNEXT: a"_s, "# STATISTC: ValuesWalked = 1"_s, "# OPTION: x"_s, "# TYPE: f0"_s,
        "# NOSUCH: a"_s, "# NOSUCH : a"_s, "# FORM: AsRead"_s, "# FORM: Prepared optimizing"_s, "# LOSS: MaxWorkers"_s, "# LOSS: MaxWorkers x1 f0 v3"_s,
    })
        expectErrorInThirdLine(line);
}

static void testProseIsNoDirective()
{
    for (ASCIILiteral line : {
        "# Check that the sum is there"_s, "# Run it twice"_s, "# check the sum"_s, "# Options are none"_s, "# types of the values"_s,
        "# JSON text"_s, "# IR text"_s, "# NOTE a word in capitals"_s, "# A sum"_s, "# CHEKC without a colon or a suffix"_s, "# Note: a word with a colon"_s,
        "#"_s, "# "_s, "# 3 values"_s, "# -1 is the sum"_s, "v2 = Add v0, v1   # CHECK NoSuchText"_s,
    })
        expectErrorInThirdLine(line, false);
}

static void testMarkers()
{
    auto file = [](ASCIILiteral comments, ASCIILiteral below = ""_s) {
        return makeString("# RUN: print\n"_s, comments, sampleProgram, below);
    };
    auto expectLines = [](const String& text, std::initializer_list<unsigned> lines) {
        TestFile file = parseTestFile(String(text), Architecture::X86_64);
        if (!std::ranges::equal(file.errors, lines, { }, &TestFileError::line)) {
            for (const TestFileError& error : file.errors)
                dataLogLn(error.line, ": ", error.message);
        }
        AOT_TEST_CHECK(std::ranges::equal(file.errors, lines, { }, &TestFileError::line));
    };
    constexpr ASCIILiteral labelOfSum = "# CHECK-LABEL: function {{*}}name=\"sum\"\n# CHECK-NEXT: b0\n"_s;

    // Whole, and a line of a source is no directive whatever it looks like.
    expectLines(file("# -- source: main.ts\n# CHECK without a colon\n# -- source: other.ts\n# check: x\n# -- end of source\n# CHECK: Add\n"_s), { });
    expectLines(makeString(file(""_s, "# -- generated: sum\n"_s), labelOfSum), { });
    expectLines(makeString(file(""_s, "# -- generated: f0\n"_s), "# CHECK-LABEL: function f0 name=\n"_s), { });
    expectLines(makeString(file(""_s, "# -- generated: *\n"_s), labelOfSum), { });

    expectLines(file("# CHECK: Add\n# -- sorce: main.ts\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# --source: main.ts\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- source:\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- source: two words\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- end\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- generated:\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- source: main.ts\n# 1;\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- end of source\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- source: a.ts\n# -- end of source\n# -- source: b.ts\n# -- end of source\n"_s), { 5 });
    expectLines(file("# CHECK: Add\n# -- source: a.ts\n# -- generated: sum\n# -- end of source\n"_s), { 4 });
    expectLines(makeString(file(""_s, "# -- generated: sum\n"_s), labelOfSum, "# -- generated: sum\n"_s), { 11 });
    expectLines(makeString(file(""_s, "# -- generated: sum\n"_s), labelOfSum, "# -- source: a.ts\n# -- end of source\n"_s), { 11 });
    expectLines(makeString(file(""_s, "# -- generated: sum product\n"_s), labelOfSum), { 8 });
    expectLines(file("# CHECK: Add\n"_s, "# -- generated: sum\n"_s), { 9 });
    expectLines(makeString(file(""_s, "# -- generated: sum\n"_s), labelOfSum, "# written by hand\n"_s), { 11 });
    expectLines(makeString(file(""_s, "# -- generated: sum\n"_s), labelOfSum, "module m1 path=\"/b.ts\" displayPath=\"b.ts\"\n"_s), { 11 });

    expectLines(file("# CHECK: Add\n# -- cod\n"_s), { 3 });
    expectLines(file("# CHECK: Add\n# -- source: a.ts\n# -- code\n# -- end of source\n"_s), { 4 });
    expectLines(makeString(file(""_s, "# -- generated: sum\n"_s), labelOfSum, "# -- code\n# -- end of code\n"_s), { 11 });
}

static void testSectionIsReadWithItsFirstLine()
{
    TestFile file = parseTestFile(
        "# RUN: print\n"
        "# -- code\n"
        "# code f0:Body\n"
        "#\n"
        "#   CHECK without a colon\n"
        "# RUN: no-such-command\n"
        "# -- end of code\n"
        "# CHECK: Add\n"_s);
    AOT_TEST_CHECK(file.errors.isEmpty());
    AOT_TEST_CHECK(file.runs.size() == 1);
    AOT_TEST_CHECK(file.checkLinesFor("CHECK"_s).size() == 1);
    AOT_TEST_CHECK(file.sections.size() == 1);
    AOT_TEST_CHECK(file.sections[0].name == "code"_s && file.sections[0].firstLine == 3);
    AOT_TEST_CHECK(file.sections[0].text == "code f0:Body\n\n  CHECK without a colon\nRUN: no-such-command\n"_s);

    // firstLine counts lines in the file itself, not in the text with REPEAT written out.
    TestFile fileWithRepeat = parseTestFile("# RUN: print\n# REPEAT: i = 1..2\n# CHECK: %(i)\n# END REPEAT\n# -- code\n# code f0:Body\n# -- end of code\n"_s);
    AOT_TEST_CHECK(fileWithRepeat.errors.isEmpty());
    AOT_TEST_CHECK(fileWithRepeat.sections.size() == 1);
    AOT_TEST_CHECK(fileWithRepeat.sections[0].name == "code"_s && fileWithRepeat.sections[0].firstLine == 6);
}

static void testSectionsMalformed()
{
    expectErrorLines("# RUN: print\n# -- code\n# code f0:Body\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# -- code\n# code f0:Body\nmodule m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# -- code\n# code f0:Body\n# -- code\n# code f0:Body\n# -- end of code\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# -- end of code\n"_s, { 2 });

    expectErrorLines("# RUN: print\n# -- code\n# code f0:Body\n# -- end of code\n# -- code\n# code f1:Body\n# -- end of code\n"_s, { 5 });
}

static void testTypesLineMalformed()
{
    AOT_TEST_CHECK(!parseTestFile("# RUN: print\n"_s).functionOfTypesLine);

    expectErrorLines("# RUN: print\n# TYPES: f0\n# TYPES: f1\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# TYPES: f0\n# TYPES: f0\n"_s, { 3 });
    expectErrorLines("# RUN: print\n# TYPES:\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# TYPES: f\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# TYPES: 0\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# TYPES: v0\n"_s, { 2 });
    expectErrorLines("# RUN: print\n# TYPES: f0 f1\n"_s, { 2 });
    // The bits of the null FunctionID.
    expectErrorLines("# RUN: print\n# TYPES: f4294967295\n"_s, { 2 });
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

static Vector<FunctionID> functionsNumberedBelow(uint32_t end)
{
    Vector<FunctionID> functions;
    for (uint32_t index = 0; index < end; ++index)
        functions.append(FunctionID(index));
    return functions;
}

static void testBodiesThatCodeDependsOnAreFoundByHalving()
{
    Vector<FunctionID> candidates = functionsNumberedBelow(100);
    unsigned numberOfQuestions = 0;
    std::optional<Vector<FunctionID>> dependedOn = findBodiesThatCodeDependsOn(candidates.span(), [&](std::span<const FunctionID> others) -> std::optional<bool> {
        ++numberOfQuestions;
        return std::ranges::contains(others, FunctionID(3u)) && std::ranges::contains(others, FunctionID(57u));
    });
    AOT_TEST_CHECK(dependedOn);
    AOT_TEST_CHECK(std::ranges::equal(*dependedOn, std::initializer_list<FunctionID> { FunctionID(57u), FunctionID(3u) }));
    AOT_TEST_CHECK(numberOfQuestions < 20);

    dependedOn = findBodiesThatCodeDependsOn(candidates.span(), [](std::span<const FunctionID>) -> std::optional<bool> {
        return true;
    });
    AOT_TEST_CHECK(dependedOn && dependedOn->isEmpty());
}

static void testBodiesThatCodeDependsOnAreNotFound()
{
    Vector<FunctionID> candidates = functionsNumberedBelow(100);
    AOT_TEST_CHECK(!findBodiesThatCodeDependsOn(candidates.span(), [](std::span<const FunctionID>) -> std::optional<bool> {
        return false;
    }));
    AOT_TEST_CHECK(!findBodiesThatCodeDependsOn({ }, [](std::span<const FunctionID>) -> std::optional<bool> {
        return false;
    }));

    unsigned numberOfQuestions = 0;
    AOT_TEST_CHECK(!findBodiesThatCodeDependsOn(candidates.span(), [&](std::span<const FunctionID> others) -> std::optional<bool> {
        if (++numberOfQuestions == 4)
            return std::nullopt;
        return std::ranges::contains(others, FunctionID(57u));
    }));
    AOT_TEST_CHECK(numberOfQuestions == 4);
}

std::span<const NativeTest> testFileTests()
{
    static constexpr NativeTest tests[] = {
        { "test-file/requires-selects-runs"_s, testRequiresSelectsRuns },
        { "test-file/requires-malformed"_s, testRequiresMalformed },
        { "test-file/directive-that-went-wrong"_s, testDirectiveThatWentWrong },
        { "test-file/prose-is-no-directive"_s, testProseIsNoDirective },
        { "test-file/markers"_s, testMarkers },
        { "test-file/section-is-read-with-its-first-line"_s, testSectionIsReadWithItsFirstLine },
        { "test-file/sections-malformed"_s, testSectionsMalformed },
        { "test-file/types-line-malformed"_s, testTypesLineMalformed },
        { "test-file/repeat-is-written-out"_s, testRepeatIsWrittenOut },
        { "test-file/repeat-malformed"_s, testRepeatMalformed },
        { "test-file/check-lines-inside-function-have-label"_s, testCheckLinesInsideFunctionHaveLabel },
        { "runner/skips-other-architecture"_s, testRunnerSkipsOtherArchitecture },
        { "commands/bodies-that-code-depends-on-are-found-by-halving"_s, testBodiesThatCodeDependsOnAreFoundByHalving },
        { "commands/bodies-that-code-depends-on-are-not-found"_s, testBodiesThatCodeDependsOnAreNotFound },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
