/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCheckLines.h"
#include "AOTChildProcess.h"
#include "AOTCompileOptionWords.h"
#include "AOTDiagnostics.h"
#include "AOTLimits.h"
#include "AOTNativeTest.h"
#include "AOTPassID.h"
#include "AOTProgramFile.h"
#include "AOTReaderFixture.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaEnums.h"
#include "AOTTemporaryFile.h"
#include "AOTTestCommand.h"
#include "AOTTestFile.h"
#include "AOTTestRunner.h"
#include "AOTTestSubcommands.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <wtf/DataLog.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using namespace Typed;

namespace {

struct MalformedCase {
    ASCIILiteral text;
    unsigned line;
};

struct MalformedSentenceCase {
    ASCIILiteral directive;
    ASCIILiteral sentence;
};

} // anonymous namespace

static_assert(defaultLimitValue(LimitID::MaxOptimizationRounds, hostArchitecture()) == 8);
static_assert(canBeLowered(LimitID::MaxFoldedStringBytes));
static_assert(canBeLowered(LimitID::MaxWorkers));

static constexpr ASCIILiteral sampleOutput =
    "function f0 name=\"sum\"\n"
    "  v2 = Add v0, v1\n"
    "  v3 = Mul v2, v2\n"
    "  v12 = Sub v3, v0\n"
    "  Return v12\n"
    "function f1 name=\"sum2\"\n"
    "  v1 = Negate v0\n"
    "  Return v1\n"_s;

static constexpr ASCIILiteral sampleProgram =
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"sum\" parameters=[{type=number}, {type=number}] results=[{type=number}] {\n"
    "  b0(v0: number, v1: number):\n"
    "    v2 = Add v0, v1\n"
    "    v3 = Mul v2, v2\n"
    "    v4 = Sub v3, v0\n"
    "    Return v4\n"
    "}\n"
    "entry\n"_s;

static constexpr ASCIILiteral concatenationProgram =
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"concatenate\" results=[{type=string}] {\n"
    "  b0():\n"
    "    v0 = String string=\"a\"\n"
    "    v1 = String string=\"b\"\n"
    "    v2 = Add v0, v1\n"
    "    Return v2\n"
    "}\n"
    "entry\n"_s;

// An error of the user ends the pipeline in front of the first verification. Code is generated for each function whatever happens to the others, so here one run has a diagnostic of each list.
static constexpr ASCIILiteral inlineFrameAndOperationWithoutCodeProgram =
    "# FORM: Selected\n"
    "# LOSS: InlineFramesPerFunction\n"
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"hasInlineFrame\" parameters=[{type=anything}] results=[{type=anything}] inlineFrames=[{}, {callee=f2 callPosition=7 module=m0}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = Call v0 @3:1\n"
    "    Return v1\n"
    "}\n"
    "function f1 name=\"hasNoMissEdge\" parameters=[{type=Array<anything, pristine[Storage, Names]>}, {type=i32}] results=[{type=anything}] {\n"
    "  b0(v0: Array<anything, pristine[Storage, Names]>, v1: i32):\n"
    "    v2 = CallInline v0, v1 overload=ArrayPrototypeAt/0\n"
    "    Return v2\n"
    "}\n"
    "function f2 name=\"callsArgument\" parameters=[{type=anything}] results=[{type=anything}] {\n"
    "  b0(v0: anything):\n"
    "    v1 = Call v0 @3\n"
    "    Return v1\n"
    "}\n"_s;

static constexpr ASCIILiteral invalidUpcastProgram =
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"valid\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = String string=\"a\"\n"
    "    Return v0\n"
    "}\n"
    "function f1 name=\"invalidUpcast\" results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = String string=\"a\"\n"
    "    v1 = Upcast v0 castType=number\n"
    "    Return v1\n"
    "}\n"
    "entry\n"_s;

static constexpr ASCIILiteral positionsProgram =
    "module m0 path=\"/a.ts\" displayPath=\"a.ts\"\n"
    "function f0 name=\"sum\" parameters=[{type=number}, {type=number}] results=[{type=number}] {\n"
    "  b0(v0: number, v1: number):\n"
    "    v2 = Add v0, v1 @12\n"
    "    v3 = Mul v2, v2\n"
    "    Return v3\n"
    "}\n"
    "entry\n"_s;

static CheckLine makeLine(CheckKind kind, ASCIILiteral pattern, unsigned line, unsigned count = 1)
{
    CheckLine checkLine { kind, count, line, pattern, { } };
    AOT_TEST_CHECK(!parseCheckPattern(pattern, checkLine.elements));
    return checkLine;
}

static std::optional<CheckFailure> match(std::initializer_list<CheckLine> checkLines, ASCIILiteral output)
{
    std::span<const CheckLine> span(checkLines);
    AOT_TEST_CHECK(!validateCheckLines(span));
    return matchCheckLines(span, output);
}

static void expectFailure(const std::optional<CheckFailure>& failure, CheckFailureKind kind, unsigned line)
{
    AOT_TEST_CHECK(failure);
    if (failure->kind != kind || failure->line != line)
        dataLogLn("Check line ", failure->line, " failed: ", failure->message);
    AOT_TEST_CHECK(failure->kind == kind);
    AOT_TEST_CHECK(failure->line == line);
}

static void expectInvalidSequence(std::initializer_list<CheckLine> checkLines, unsigned line)
{
    std::optional<CheckLineError> error = validateCheckLines(std::span<const CheckLine>(checkLines));
    AOT_TEST_CHECK(error);
    AOT_TEST_CHECK(error->line == line);
    AOT_TEST_CHECK(!error->message.isEmpty());
}

template<typename Failure>
static void expectFailureLines(const String& text, const Vector<Failure>& failures, std::initializer_list<unsigned> lines)
{
    bool isExpected = std::ranges::equal(failures, lines, { }, &Failure::line);
    if (!isExpected) {
        dataLogLn("Unexpected failures in this test file:\n", text);
        for (const Failure& failure : failures)
            dataLogLn("line ", failure.line, ": ", failure.message);
    }
    AOT_TEST_CHECK(isExpected);
}

static void expectMalformed(const String& text, unsigned line)
{
    expectFailureLines(text, parseTestFile(String { text }).errors, { line });
}

static void expectFailures(const String& text, std::initializer_list<unsigned> lines)
{
    expectFailureLines(text, runTestText(String { text }), lines);
}

static String withDirectives(ASCIILiteral directives, const String& program = sampleProgram)
{
    return makeString(directives, program);
}

static void testCheckLinesNoMatch()
{
    using enum CheckKind;
    using enum CheckFailureKind;

    expectFailure(match({ makeLine(Plain, "Div"_s, 1) }, sampleOutput), NoMatch, 1);
    expectFailure(match({ makeLine(Plain, "Mul"_s, 1), makeLine(Plain, "Add"_s, 2) }, sampleOutput), NoMatch, 2);
    expectFailure(match({ makeLine(Plain, "b"_s, 1), makeLine(Plain, "a"_s, 2) }, "a b\n"_s), NoMatch, 2);
    expectFailure(match({ makeLine(Count, "Mul"_s, 1, 2) }, sampleOutput), NoMatch, 1);
    expectFailure(match({ makeLine(Plain, "A dd"_s, 1) }, sampleOutput), NoMatch, 1);
    expectFailure(match({ makeLine(Plain, "Add{{*}}Mul"_s, 1) }, sampleOutput), NoMatch, 1);
}

static void testCheckLinesNext()
{
    using enum CheckKind;
    using enum CheckFailureKind;

    expectFailure(match({ makeLine(Plain, "Add"_s, 1), makeLine(Next, "Sub"_s, 2) }, sampleOutput), NotOnNextLine, 2);
    expectFailure(match({ makeLine(Plain, "Negate"_s, 1), makeLine(Next, "Return"_s, 2), makeLine(Next, "x"_s, 3) }, sampleOutput), NotOnNextLine, 3);
    AOT_TEST_CHECK(!match({ makeLine(Plain, "Add"_s, 1), makeLine(Next, "Mul"_s, 2), makeLine(Next, "Sub"_s, 3) }, sampleOutput));
}

static void testCheckLinesNot()
{
    using enum CheckKind;
    using enum CheckFailureKind;

    expectFailure(match({ makeLine(Plain, "Add"_s, 1), makeLine(Not, "Mul"_s, 2), makeLine(Plain, "Sub"_s, 3) }, sampleOutput), ExcludedTextFound, 2);
    expectFailure(match({ makeLine(Plain, "Negate"_s, 1), makeLine(Not, "Return"_s, 2) }, sampleOutput), ExcludedTextFound, 2);
    AOT_TEST_CHECK(!match({ makeLine(Plain, "Add"_s, 1), makeLine(Not, "Negate"_s, 2), makeLine(Plain, "Sub"_s, 3) }, sampleOutput));

    AOT_TEST_CHECK(!match({ makeLine(Plain, "x"_s, 1), makeLine(Not, "y"_s, 2), makeLine(Plain, "a"_s, 3) }, "x a y\n"_s));
    expectFailure(match({ makeLine(Plain, "x"_s, 1), makeLine(Not, "a"_s, 2), makeLine(Plain, "y"_s, 3) }, "x a y\n"_s), ExcludedTextFound, 2);

    AOT_TEST_CHECK(!match({ makeLine(Plain, "valid"_s, 1), makeLine(Not, "invalid"_s, 2) }, "invalid\n"_s));
    AOT_TEST_CHECK(!match({ makeLine(Not, "invalid"_s, 1), makeLine(Plain, "valid"_s, 2) }, "invalid\n"_s));
    expectFailure(match({ makeLine(Not, "invalid"_s, 1) }, "invalid\n"_s), ExcludedTextFound, 1);
}

static void testCheckLinesLabel()
{
    using enum CheckKind;
    using enum CheckFailureKind;

    expectFailure(match({ makeLine(Label, "f2"_s, 1) }, sampleOutput), LabelNotFound, 1);
    expectFailure(match({ makeLine(Label, "\"sum\""_s, 1), makeLine(Plain, "Negate"_s, 2), makeLine(Label, "\"sum2\""_s, 3) }, sampleOutput), NoMatch, 2);
    AOT_TEST_CHECK(!match({ makeLine(Label, "\"sum\""_s, 1), makeLine(Not, "Negate"_s, 2), makeLine(Label, "\"sum2\""_s, 3), makeLine(Plain, "Negate"_s, 4) }, sampleOutput));
    expectFailure(match({ makeLine(Label, "\"sum2\""_s, 1), makeLine(Label, "\"sum\""_s, 2) }, sampleOutput), LabelNotFound, 2);
}

static void testCheckLinesVariables()
{
    using enum CheckKind;
    using enum CheckFailureKind;

    expectFailure(match({ makeLine(Plain, "a [[x]]"_s, 1), makeLine(Plain, "b [[x]]"_s, 2) }, "a v1\nb v12\n"_s), NoMatch, 2);
    expectFailure(match({ makeLine(Plain, "su[[x]]"_s, 1) }, "sum2\n"_s), NoMatch, 1);
    AOT_TEST_CHECK(!match({ makeLine(Plain, "p [[x]] r"_s, 1), makeLine(Plain, "s [[x]]"_s, 2) }, "p v1 q\np v2 r\ns v2\n"_s));
    AOT_TEST_CHECK(!match({ makeLine(Plain, "a"_s, 1), makeLine(Not, "z [[x]]"_s, 2), makeLine(Plain, "b [[x]]"_s, 3), makeLine(Plain, "c [[x]]"_s, 4) }, "a v1\nb v2\nc v2\n"_s));

    AOT_TEST_CHECK(!match({ makeLine(Plain, "a"_s, 1), makeLine(Not, "m [[x]]"_s, 2), makeLine(Plain, "b [[x]]"_s, 3) }, "a v1\nm v2\nb v1\n"_s));
    expectFailure(match({ makeLine(Plain, "a"_s, 1), makeLine(Not, "m [[x]]"_s, 2), makeLine(Plain, "b [[x]]"_s, 3) }, "a v1\nm v1\nb v1\n"_s), ExcludedTextFound, 2);
}

static void testCheckLinesInvalidPatterns()
{
    using Kind = PatternElement::Kind;

    static constexpr ASCIILiteral invalidPatterns[] = {
        ""_s,
        "   "_s,
        "{{a}}"_s,
        "a{{"_s,
        "[[1x]]"_s,
        "[[x"_s,
        "[[]]"_s,
        "{{*}}"_s,
        "{{*}} {{*}}"_s,
    };
    for (ASCIILiteral pattern : invalidPatterns) {
        Vector<PatternElement> elements { PatternElement { Kind::Text, "stale"_s } };
        std::optional<String> sentence = parseCheckPattern(pattern, elements);
        if (!sentence)
            dataLogLn("Pattern ", pattern, " was not rejected");
        AOT_TEST_CHECK(sentence);
        AOT_TEST_CHECK(!sentence->isEmpty());
        AOT_TEST_CHECK(elements.isEmpty());
    }

    static constexpr std::array texts { "a"_s, "}}"_s, "]]"_s, "{"_s, "["_s, "b"_s };
    Vector<PatternElement> elements;
    AOT_TEST_CHECK(!parseCheckPattern("a }} ]] { [ b"_s, elements));
    AOT_TEST_CHECK(elements.size() == 2 * texts.size() - 1);
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i % 2) {
            AOT_TEST_CHECK(elements[i].kind == Kind::Blanks);
            continue;
        }
        AOT_TEST_CHECK(elements[i].kind == Kind::Text);
        AOT_TEST_CHECK(elements[i].text == texts[i / 2]);
    }
}

static void testCheckLinesInvalidSequences()
{
    using enum CheckKind;

    expectInvalidSequence({ makeLine(Next, "x"_s, 1) }, 1);
    expectInvalidSequence({ makeLine(Plain, "a"_s, 1), makeLine(Not, "b"_s, 2), makeLine(Next, "c"_s, 3) }, 3);
    expectInvalidSequence({ makeLine(Label, "[[x]]"_s, 1) }, 1);
    AOT_TEST_CHECK(!match({ makeLine(Label, "a"_s, 1), makeLine(Next, "b"_s, 2) }, "a\nb\n"_s));
}

static void checkWellFormedRuns(const TestFile& file)
{
    AOT_TEST_CHECK(file.runs.size() == 2);
    AOT_TEST_CHECK(file.runs[0].prefix == "CHECK"_s);
    AOT_TEST_CHECK(file.runs[0].command == "print"_s);
    AOT_TEST_CHECK((file.runs[0].arguments == Vector<String> { "a"_s, "b"_s }));
    AOT_TEST_CHECK(file.runs[0].line == 2);
    AOT_TEST_CHECK(file.runs[1].prefix == "SECOND"_s);
    AOT_TEST_CHECK(file.runs[1].command == "verify"_s);
    AOT_TEST_CHECK(file.runs[1].arguments.isEmpty());
    AOT_TEST_CHECK(file.runs[1].line == 3);

    AOT_TEST_CHECK(file.form == IRForm::AsRead);
    AOT_TEST_CHECK(!file.isOptimizing);
    AOT_TEST_CHECK(file.formLine == 5);

    AOT_TEST_CHECK(file.optionWords.size() == 3);
    AOT_TEST_CHECK(file.optionWords[0].word == "validate"_s && file.optionWords[0].line == 6);
    AOT_TEST_CHECK(file.optionWords[1].word == "threads=1"_s && file.optionWords[1].line == 6);
    AOT_TEST_CHECK(file.optionWords[2].word == "count-branches"_s && file.optionWords[2].line == 7);
}

static void checkWellFormedCheckGroups(const TestFile& file)
{
    AOT_TEST_CHECK(file.checkGroups.size() == 2);
    const CheckGroup& defaultGroup = file.checkGroups[0];
    AOT_TEST_CHECK(defaultGroup.prefix == "CHECK"_s);
    AOT_TEST_CHECK(defaultGroup.lines.size() == 2);
    AOT_TEST_CHECK(defaultGroup.lines[0].kind == CheckKind::Plain);
    AOT_TEST_CHECK(defaultGroup.lines[0].pattern == "Add"_s);
    AOT_TEST_CHECK(defaultGroup.lines[0].line == 1);
    AOT_TEST_CHECK(defaultGroup.lines[1].kind == CheckKind::Count);
    AOT_TEST_CHECK(defaultGroup.lines[1].count == 3);
    AOT_TEST_CHECK(defaultGroup.lines[1].line == 8);
    const CheckGroup& secondGroup = file.checkGroups[1];
    AOT_TEST_CHECK(secondGroup.prefix == "SECOND"_s);
    AOT_TEST_CHECK(secondGroup.lines.size() == 1);
    AOT_TEST_CHECK(secondGroup.lines[0].kind == CheckKind::Not);
    AOT_TEST_CHECK(secondGroup.lines[0].line == 4);
}

static void checkWellFormedExpectations(const TestFile& file)
{
    AOT_TEST_CHECK(file.diagnostics.size() == 2);
    AOT_TEST_CHECK(file.diagnostics[0].kind == DiagnosticName { InternalErrorKind::IRTextError });
    AOT_TEST_CHECK(nameOf(file.diagnostics[0].kind) == "IRTextError"_s);
    AOT_TEST_CHECK(file.diagnostics[0].count == 2u);
    AOT_TEST_CHECK(file.diagnostics[0].line == 9);
    AOT_TEST_CHECK(file.diagnostics[1].kind == DiagnosticName { UserDiagnosticKind::InvalidRegExpLiteral });
    AOT_TEST_CHECK(nameOf(file.diagnostics[1].kind) == "InvalidRegExpLiteral"_s);
    AOT_TEST_CHECK(!file.diagnostics[1].count);
    AOT_TEST_CHECK(file.diagnostics[1].line == 10);

    AOT_TEST_CHECK(file.statistics.size() == 1);
    AOT_TEST_CHECK(file.statistics[0].statistic == StatisticID::TypesInterned);
    AOT_TEST_CHECK(file.statistics[0].comparison == StatisticComparison::AtLeast);
    AOT_TEST_CHECK(file.statistics[0].value == 1);
    AOT_TEST_CHECK(file.statistics[0].line == 11);

    AOT_TEST_CHECK(file.losses.size() == 2);
    AOT_TEST_CHECK(file.losses[0].limit == LimitID::MaxOptimizationRounds);
    AOT_TEST_CHECK(file.losses[0].count == 2u);
    AOT_TEST_CHECK(file.losses[0].function == FunctionID(1u));
    AOT_TEST_CHECK(file.losses[0].value == ValueID(3u));
    AOT_TEST_CHECK(file.losses[0].layout == LayoutID(0u));
    AOT_TEST_CHECK(file.losses[0].line == 12);
    AOT_TEST_CHECK(file.losses[1].limit == LimitID::MaxSinkingRounds);
    AOT_TEST_CHECK(!file.losses[1].count && !file.losses[1].function && !file.losses[1].value && !file.losses[1].layout);
    AOT_TEST_CHECK(file.losses[1].line == 13);
}

static void testTestFileWellFormed()
{
    TestFile file = parseTestFile(
        "# CHECK: Add\r\n"
        "#RUN: print a b\r\n"
        "# RUN(SECOND): verify\r\n"
        "# SECOND-NOT: x\r\n"
        "# FORM: AsRead\r\n"
        "# OPTIONS: validate threads=1\r\n"
        "# OPTIONS: count-branches\r\n"
        "# CHECK-COUNT-3: x\r\n"
        "# DIAGNOSTIC: IRTextError 2\r\n"
        "# DIAGNOSTIC: InvalidRegExpLiteral\r\n"
        "# STATISTIC: TypesInterned >= 1\r\n"
        "# LOSS: MaxOptimizationRounds x2 f1 v3 L0\r\n"
        "# LOSS: MaxSinkingRounds\r\n"
        "# A comment\r\n"
        "v1 = Add v0, v0   # CHECK: no\r\n"_s);
    AOT_TEST_CHECK(file.errors.isEmpty());
    checkWellFormedRuns(file);
    checkWellFormedCheckGroups(file);
    checkWellFormedExpectations(file);

    TestFile optimizingFile = parseTestFile("# RUN: verify\n# FORM: Prepared optimizing\n"_s);
    AOT_TEST_CHECK(optimizingFile.errors.isEmpty());
    AOT_TEST_CHECK(optimizingFile.form == IRForm::Prepared);
    AOT_TEST_CHECK(optimizingFile.isOptimizing);
}

static void testDiagnosticNames()
{
    for (UserDiagnosticKind kind : allMembersOf<UserDiagnosticKind>) {
        std::optional<DiagnosticName> name = parseDiagnosticName(nameOf(kind));
        AOT_TEST_CHECK(kind == UserDiagnosticKind { } ? !name : name == DiagnosticName { kind });
    }
    for (InternalErrorKind kind : allMembersOf<InternalErrorKind>) {
        std::optional<DiagnosticName> name = parseDiagnosticName(nameOf(kind));
        AOT_TEST_CHECK(kind == InternalErrorKind { } ? !name : name == DiagnosticName { kind });
    }
    AOT_TEST_CHECK(!parseDiagnosticName("Nothing"_s));
    AOT_TEST_CHECK(!parseDiagnosticName(""_s));
}

static constexpr MalformedCase malformedRunCases[] = {
    { "# A comment\n"_s, 1 },
    { "# RUN:\n"_s, 1 },
    { "# RUN(lower): print\n"_s, 1 },
    { "# RUN(CHECK): print\n"_s, 1 },
    { "# RUN(LOSS): print\n"_s, 1 },
    { "# RUN(A-B): print\n"_s, 1 },
    { "# RUN(ONLY): print\n"_s, 1 },
    { "# RUN(ONLY): print\n# ONLY: x\n# CHECK: x\n"_s, 3 },
};

static constexpr MalformedCase malformedDirectiveCases[] = {
    { "# FORM(X): AsRead\n"_s, 2 },
    { "# NOTE: x\n"_s, 2 },
    { "# CHECK-NXT: x\n"_s, 2 },
    { "# OTHER: x\n"_s, 2 },
    { "# CHECK-COUNT-0: x\n"_s, 2 },
    { "# CHECK-COUNT-1001: x\n"_s, 2 },
    { "# CHECK-COUNT-: x\n"_s, 2 },
    { "# CHECK:\n"_s, 2 },
    { "# CHECK: {{x}}\n"_s, 2 },
    { "# CHECK-NEXT: x\n"_s, 2 },
    { "# FORM:\n"_s, 2 },
    { "# FORM: Later\n"_s, 2 },
    { "# FORM: AsRead\n# FORM: AsRead\n"_s, 3 },
    { "# FORM: Prepared optimizing optimizing\n"_s, 2 },
    { "# OPTIONS:\n"_s, 2 },
    { "# DIAGNOSTIC: Nothing\n"_s, 2 },
    { "# DIAGNOSTIC: IRTextError 0\n"_s, 2 },
    { "# DIAGNOSTIC: IRTextError\n# DIAGNOSTIC: IRTextError\n"_s, 3 },
    { "# DIAGNOSTIC: InvalidRegExpLiteral\n# DIAGNOSTIC: InvalidRegExpLiteral 1\n"_s, 3 },
    { "# STATISTIC: TypesInterned 1\n"_s, 2 },
    { "# STATISTIC: Nothing = 1\n"_s, 2 },
    { "# STATISTIC: None = 0\n"_s, 2 },
    { "# STATISTIC: TypesInterned == 1\n"_s, 2 },
    { "# STATISTIC: TypesInterned = x\n"_s, 2 },
    { "# LOSS(X): MaxOptimizationRounds\n"_s, 2 },
    { "# LOSS: MaxOptimizationRounds\n# LOSS: MaxOptimizationRounds x1\n"_s, 3 },
};

static constexpr MalformedSentenceCase malformedSentenceCases[] = {
    { "# FORM: AsRead optimizing\n"_s, "'optimizing' goes with Prepared"_s },
    { "# FORM: Prepared slowly\n"_s, "'optimizing' goes with Prepared"_s },
    { "# DIAGNOSTIC: Unknown\n"_s, "unknown diagnostic kind 'Unknown'"_s },
    { "# LOSS:\n"_s, "LOSS needs a limit"_s },
    { "# LOSS: Nothing\n"_s, "unknown limit 'Nothing'"_s },
    { "# LOSS: None\n"_s, "unknown limit 'None'"_s },
    { "# LOSS: MaxOptimizationRounds y1\n"_s, "LOSS takes x<count>, f<n>, v<n> or L<n>, not 'y1'"_s },
    { "# LOSS: MaxOptimizationRounds f\n"_s, "LOSS takes x<count>, f<n>, v<n> or L<n>, not 'f'"_s },
    { "# LOSS: MaxOptimizationRounds v-1\n"_s, "LOSS takes x<count>, f<n>, v<n> or L<n>, not 'v-1'"_s },
    { "# LOSS: MaxOptimizationRounds f1 v2 f3\n"_s, "'f' is given twice"_s },
    { "# LOSS: MaxOptimizationRounds x1 x1\n"_s, "'x' is given twice"_s },
    { "# LOSS: MaxOptimizationRounds x0 f1\n"_s, "a limit that is not hit has no subject"_s },
};

static void testTestFileMalformed()
{
    static constexpr ASCIILiteral runLine = "# RUN: print\n"_s;

    AOT_TEST_CHECK(parseTestFile(runLine).errors.isEmpty());
    AOT_TEST_CHECK(parseTestFile("# RUN(ONLY): print\n# ONLY: x\n"_s).errors.isEmpty());

    for (const MalformedCase& testCase : malformedRunCases)
        expectMalformed(testCase.text, testCase.line);
    for (const MalformedCase& testCase : malformedDirectiveCases)
        expectMalformed(makeString(runLine, testCase.text), testCase.line);
    for (const MalformedSentenceCase& testCase : malformedSentenceCases) {
        String text = makeString(runLine, testCase.directive);
        expectMalformed(text, 2);
        String sentence = parseTestFile(WTF::move(text)).errors[0].message;
        if (sentence != testCase.sentence)
            dataLogLn("Directive ", testCase.directive, "is refused with: ", sentence);
        AOT_TEST_CHECK(sentence == testCase.sentence);
    }
}

static void testTestFileComparisons()
{
    using enum StatisticComparison;

    AOT_TEST_CHECK(!holds(Equal, 4, 5));
    AOT_TEST_CHECK(holds(Equal, 5, 5));
    AOT_TEST_CHECK(!holds(Equal, 6, 5));

    AOT_TEST_CHECK(!holds(AtLeast, 4, 5));
    AOT_TEST_CHECK(holds(AtLeast, 5, 5));
    AOT_TEST_CHECK(holds(AtLeast, 6, 5));

    AOT_TEST_CHECK(holds(AtMost, 4, 5));
    AOT_TEST_CHECK(holds(AtMost, 5, 5));
    AOT_TEST_CHECK(!holds(AtMost, 6, 5));
}

static void testRunnerCheckLines()
{
    expectFailures(withDirectives("# RUN: print\n# CHECK: Add\n# CHECK-NEXT: Mul\n"_s), { });
    expectFailures(withDirectives("# RUN: print\n# CHECK: Negate\n"_s), { 2 });
}

static const TestCommand* findCommandWithoutProgram()
{
#define AOT_TEST_COMMAND_PROVIDER_ADDRESS(name) &name,
    static constexpr std::array providers { FOR_EACH_AOT_TEST_COMMAND_PROVIDER(AOT_TEST_COMMAND_PROVIDER_ADDRESS) };
#undef AOT_TEST_COMMAND_PROVIDER_ADDRESS

    for (auto provider : providers) {
        for (const TestCommand& command : provider()) {
            if (!command.defaultForm)
                return &command;
        }
    }
    return nullptr;
}

static void testRunnerCommands()
{
    expectFailures(withDirectives("# RUN: no-such-command\n"_s), { 1 });

    if (const TestCommand* command = findCommandWithoutProgram())
        expectFailures(makeString("# RUN: "_s, command->name, "\n# FORM: AsRead\n"_s, sampleProgram), { 2 });
}

static void testRunnerOptions()
{
    expectFailures(withDirectives("# RUN: print\n# OPTIONS: threads=1 no-such-word\n"_s), { 2 });
    expectFailures(withDirectives("# RUN: print\n# OPTIONS: threads=1\n"_s), { });
}

static void testRunnerDiagnostics()
{
    expectFailures(withDirectives("# RUN: print\n# DIAGNOSTIC: IRTextError\n"_s), { 2 });
    expectFailures(withDirectives("# RUN: print\n# DIAGNOSTIC: InvalidRegExpLiteral\n"_s), { 2 });

    String unparsableProgram = makeStringByReplacingAll(sampleProgram, "Add"_s, "NoSuchOpcode"_s);
    expectFailures(withDirectives("# RUN: print\n"_s, unparsableProgram), { 1 });
    expectFailures(withDirectives("# RUN: print\n# DIAGNOSTIC: IRTextError\n"_s, unparsableProgram), { });
    expectFailures(withDirectives("# RUN: print\n# DIAGNOSTIC: IRTextError 2\n"_s, unparsableProgram), { 2 });

    String staleTypeProgram = makeStringByReplacingAll(makeStringByReplacingAll(sampleProgram, "v2 = Add"_s, "v2: boolean = Add"_s), "Mul v2, v2"_s, "Mul v0, v0"_s);
    expectFailures(withDirectives("# RUN: verify\n# FORM: Prepared\n# DIAGNOSTIC: TypeOutOfDate\n"_s, staleTypeProgram), { });
    expectFailures(withDirectives("# RUN: verify\n# FORM: Prepared\n"_s, staleTypeProgram), { 1 });
    expectFailures(withDirectives("# RUN: verify\n# CHECK: valid\n"_s, staleTypeProgram), { });
}

static void testRunnerOnlyDiagnostic()
{
    auto expectFailuresFor = [](ASCIILiteral kind, std::initializer_list<ASCIILiteral> optionWords, const String& text, std::initializer_list<unsigned> lines) {
        TestVariant variant;
        for (ASCIILiteral word : optionWords)
            variant.optionWords.append(String(word));
        variant.onlyDiagnostic = parseDiagnosticName(kind);
        AOT_TEST_CHECK(variant.onlyDiagnostic);
        expectFailureLines(text, runTestText(String { text }, nullptr, variant), lines);
    };

    String unparsableProgram = makeStringByReplacingAll(sampleProgram, "Add"_s, "NoSuchOpcode"_s);
    expectFailuresFor("IRTextError"_s, { }, withDirectives("# RUN(FIRST): print\n# RUN(SECOND): print\n# FIRST: Mul\n# SECOND: Mul\n"_s, unparsableProgram), { 1, 2 });
    expectFailuresFor("IRTextError"_s, { }, withDirectives("# RUN: print\n# DIAGNOSTIC: IRTextError\n"_s, unparsableProgram), { });
    expectFailuresFor("TypeOutOfDate"_s, { }, withDirectives("# RUN: print\n"_s, unparsableProgram), { });

    // Nothing else of the file is compared.
    expectFailuresFor("IRTextError"_s, { }, withDirectives("# RUN: print\n# CHECK: NoSuchLine\n# DIAGNOSTIC: TypeOutOfDate\n# STATISTIC: FlowAllocationSites = 7\n"_s), { });
    expectFailuresFor("IRTextError"_s, { }, withDirectives("# NOSUCHDIRECTIVE: print\n"_s), { });

    expectFailuresFor("IRTextError"_s, { "check=VerifyEachPass"_s }, withDirectives("# RUN: print\n"_s), { });
    expectFailuresFor("IRTextError"_s, { "no-such-word"_s }, withDirectives("# RUN: print\n"_s), { 1 });
}

static void testRunnerDiagnosticsOfBothLists()
{
    static constexpr ASCIILiteral run = "# RUN: generate-code\n# OPTIONS: limit=InlineFramesPerFunction:1\n"_s;
    auto expectFailuresWith = [](ASCIILiteral expectations, std::initializer_list<unsigned> lines) {
        expectFailures(makeString(run, expectations, inlineFrameAndOperationWithoutCodeProgram), lines);
    };

    expectFailuresWith("# DIAGNOSTIC: LimitExceeded 1\n# DIAGNOSTIC: OpcodeWithoutCode 1\n"_s, { });
    expectFailuresWith("# DIAGNOSTIC: OpcodeWithoutCode\n"_s, { 1 });
    expectFailuresWith("# DIAGNOSTIC: LimitExceeded\n"_s, { 1 });
    expectFailuresWith(""_s, { 1, 1 });
    expectFailuresWith("# DIAGNOSTIC: LimitExceeded 2\n# DIAGNOSTIC: OpcodeWithoutCode\n"_s, { 3 });
    expectFailuresWith("# DIAGNOSTIC: LimitExceeded\n# DIAGNOSTIC: OpcodeWithoutCode 2\n"_s, { 4 });
}

// The front end lists a limited number of internal errors, the lines that name a pass first. Such a line says what the verifier found.
static void testVerificationFailureNamesCheck()
{
    Vector<TestFailure> failures = runTestText(makeString("# RUN: pipeline VerifyProgram\n# OPTIONS: check=VerifyEachPass\n# DIAGNOSTIC: UpcastNotSubset\n"_s, invalidUpcastProgram));
    AOT_TEST_CHECK(failures.size() == 1);
    AOT_TEST_CHECK(failures[0].message.contains("VerificationFailedAfterPass: verification failed after NarrowFieldTypes; function 'invalidUpcast' (f1); UpcastNotSubset, block b0, value v1 (Upcast)"_s));
}

static void testRunnerStatistics()
{
    expectFailures(withDirectives(
        "# RUN: print\n"
        "# STATISTIC: TypesInterned <= 1000000\n"
        "# STATISTIC: TypesInterned >= 1000000\n"
        "# STATISTIC: TypesInterned = 1000000\n"_s), { 3, 4 });
}

static void testRunnerLosses()
{
    static constexpr ASCIILiteral run = "# RUN: pass FoldConstants\n# OPTIONS: limit=MaxFoldedStringBytes:0\n"_s;
    auto expectFailuresWith = [](ASCIILiteral expectations, std::initializer_list<unsigned> lines) {
        expectFailures(makeString(run, expectations, concatenationProgram), lines);
    };

    expectFailuresWith("# LOSS: MaxFoldedStringBytes\n# CHECK: outcome: Unchanged\n"_s, { });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes x1 f0 v2\n"_s, { });
    expectFailuresWith(""_s, { 1 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes x2\n"_s, { 3 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes f1\n"_s, { 3 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes v1\n"_s, { 3 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes L0\n"_s, { 3 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes x2 f1 v1\n"_s, { 3, 3, 3 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes\n# LOSS: MaxSinkingRounds\n"_s, { 4 });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes\n# LOSS: MaxSinkingRounds x0\n"_s, { });
    expectFailuresWith("# LOSS: MaxFoldedStringBytes x0\n"_s, { 3 });

    expectFailures(withDirectives("# RUN: pass FoldConstants\n# LOSS: MaxFoldedStringBytes\n# CHECK: outcome: Changed\n"_s, concatenationProgram), { 2 });
}

static void testRunnerLossesWithoutSubject()
{
    expectFailures("# RUN: limits MaxOptimizationRounds 9 8\n# LOSS: MaxOptimizationRounds x2\n# CHECK: exceeds 9: yes\n# CHECK: exceeds 8: no\n"_s, { });
    expectFailures("# RUN: limits MaxOptimizationRounds 9 8\n"_s, { 1 });
    expectFailures("# RUN: limits MaxOptimizationRounds 9 8\n# LOSS: MaxOptimizationRounds x1\n"_s, { 2 });
    expectFailures("# RUN: limits MaxOptimizationRounds\n# LOSS: MaxOptimizationRounds f0\n"_s, { 2 });
    expectFailures("# RUN: limits MaxOptimizationRounds\n# RUN: limits MaxSinkingRounds\n# LOSS: MaxOptimizationRounds\n"_s, { 2, 3 });
}

static void testRunnerLossesOfWholeBuild()
{
    static constexpr ASCIILiteral run = "# RUN: compile\n# OPTIONS: verify-only threads=2 limit=MaxWorkers:1\n"_s;

    expectFailures(makeString(run, "# LOSS: MaxWorkers x1\n# CHECK: loss MaxWorkers x1\n# CHECK-NEXT: image empty\n"_s, sampleProgram), { });
    expectFailures(makeString(run, sampleProgram), { });
    expectFailures(makeString(run, "# LOSS: MaxWorkers x2\n"_s, sampleProgram), { 3 });
    expectFailures(makeString(run, "# LOSS: MaxWorkers x0\n"_s, sampleProgram), { 3 });
    expectFailures(makeString(run, "# LOSS: MaxOptimizationRounds x0\n"_s, sampleProgram), { });
    expectFailures(makeString(run, "# LOSS: MaxOptimizationRounds\n"_s, sampleProgram), { 3 });
}

static void testRunnerSeveralRuns()
{
    expectFailures(withDirectives(
        "# RUN(FIRST): print\n"
        "# RUN(SECOND): print\n"
        "# FIRST: Add\n"
        "# SECOND: Negate\n"_s), { 4 });
}

static void testRunnerPipelineCommands()
{
    static constexpr ASCIILiteral rejectedRuns[] = {
        "# RUN: pass\n"_s,
        "# RUN: pass NoSuchPass\n"_s,
        "# RUN: pass None\n"_s,
        "# RUN: pass FoldConstants NoSuchPass\n"_s,
        "# RUN: pipeline\n"_s,
        "# RUN: pipeline VerifyProgram TypeValues\n"_s,
        "# RUN: compile x\n"_s,
        "# RUN: pipeline VerifyProgram\n# FORM: Prepared\n"_s,
        "# RUN: compile\n# FORM: Prepared\n"_s,
        "# RUN: pass TypeValues\n# FORM: Prepared\n"_s,
        "# RUN: pass-context x\n"_s,
        "# RUN: derived-functions\n"_s,
        "# RUN: derived-functions f0\n"_s,
        "# RUN: derived-functions NoSuchRole:f0\n"_s,
        "# RUN: derived-functions Source:f0\n"_s,
    };
    for (ASCIILiteral directives : rejectedRuns)
        expectFailures(makeString(directives, "# CHECK: error:\n"_s, sampleProgram), { });

    expectFailures(withDirectives("# RUN: pipeline VerifyProgram\n# CHECK: Add\n"_s), { });
    expectFailures(withDirectives("# RUN: pass TypeValues CheckInt32Promises\n# FORM: AsRead\n# CHECK-COUNT-2: outcome:\n# CHECK: Add\n"_s), { });
    expectFailures(withDirectives("# RUN: derived-functions Source:f0\n# CHECK: error: Source is not derived on request\n"_s), { });
}

static void testRunnerListsPasses()
{
    Vector<RunOutput> outputs;
    expectFailureLines("list-passes"_s, runTestText("# RUN: list-passes\n"_s, &outputs), { });
    AOT_TEST_CHECK(outputs.size() == 1);
    AOT_TEST_CHECK(outputs[0].line == 1);

    const String& listing = outputs[0].text;
    AOT_TEST_CHECK(listing.startsWith("AllocateTaggedLayouts Program Required AsRead..AsRead\nCheckTypesAtEntry Program Required AsRead..AsRead\nTypeValues Function Required AsRead..AsRead\n"_s));
    AOT_TEST_CHECK(listing.contains("\nAnalyzeFlow Program Optional AsRead..AsRead\n"_s));
    AOT_TEST_CHECK(listing.contains("\nApplyFlowResults Program Required AsRead..AsRead -> Prepared\n"_s));
    AOT_TEST_CHECK(listing.contains("\nVerifyProgram Program Required AsRead..Placed\n"_s));
    AOT_TEST_CHECK(!listing.contains("None "_s));

    unsigned numberOfLines = 0;
    for (StringView line : StringView(listing).split('\n')) {
        AOT_TEST_CHECK(line.startsWith(makeString(nameOf(static_cast<PassID>(numberOfLines + 1)), ' ')));
        ++numberOfLines;
    }
    AOT_TEST_CHECK(numberOfLines == numberOfPasses - 1);
}

static void testRunnerPassContext()
{
    expectFailures(withDirectives(
        "# RUN: pass-context\n"
        "# CHECK: f0:\n"
        "# CHECK-NEXT: b0 heat={{*}} runsAtMostOnce=\n"
        "# CHECK-NEXT: v2 subject=f0 v2 @12\n"
        "# CHECK-NOT: subject=\n"_s, positionsProgram), { });
}

static void expectCommand(ASCIILiteral name, IRForm defaultForm)
{
    const TestCommand* command = findTestCommand(name);
    AOT_TEST_CHECK(command);
    AOT_TEST_CHECK(command->name == name);
    AOT_TEST_CHECK(command->defaultForm == defaultForm);
}

static void testCommandsLookup()
{
    expectCommand("print"_s, IRForm::AsRead);
    expectCommand("verify"_s, IRForm::AsRead);
    expectCommand("round-trip"_s, IRForm::AsRead);
    expectCommand("pass"_s, IRForm::Prepared);
    expectCommand("function-passes"_s, IRForm::Prepared);
    expectCommand("pipeline"_s, IRForm::AsRead);
    expectCommand("compile"_s, IRForm::AsRead);
    expectCommand("pass-context"_s, IRForm::Prepared);
    expectCommand("derived-functions"_s, IRForm::Prepared);
    AOT_TEST_CHECK(findTestCommand("crash"_s));
    AOT_TEST_CHECK(findTestCommand("list-passes"_s));

    AOT_TEST_CHECK(!findTestCommand("no-such-command"_s));
    AOT_TEST_CHECK(!findTestCommand(""_s));

    StringPrintStream out;
    listTestCommands(out);
    String listing = out.toString();
    AOT_TEST_CHECK(listing.contains("print AsRead\n"_s));
    AOT_TEST_CHECK(listing.contains("pass Prepared\n"_s));
}

static void testProgramFileKinds()
{
    using Kind = ProgramFile::Kind;
    using MagicBytes = std::array<uint8_t, sizeof(Wire::Magic)>;

    MagicBytes magic = std::bit_cast<MagicBytes>(Wire::Magic);
    std::span<const uint8_t> magicBytes { magic };
    Vector<uint8_t> magicWithSuffix(magicBytes);
    magicWithSuffix.append(0xff);

    AOT_TEST_CHECK(programKindOf({ }) == Kind::Text);
    AOT_TEST_CHECK(programKindOf(magicBytes.first(magicBytes.size() - 1)) == Kind::Text);
    AOT_TEST_CHECK(programKindOf(byteCast<uint8_t>("function f0"_s.span())) == Kind::Text);

    AOT_TEST_CHECK(programKindOf(magicBytes) == Kind::Bytes);
    AOT_TEST_CHECK(programKindOf(magicWithSuffix.span()) == Kind::Bytes);
}

static void testProgramTextBecomesBytes()
{
    BuildContext build(makeDefaultCompileOptions());
    ProgramFile bytesFile { ProgramFile::Kind::Bytes, encodeProgramText(sampleProgram, build), { } };
    ProgramFile textFile { ProgramFile::Kind::Text, { }, sampleProgram };
    AOT_TEST_CHECK(programKindOf(bytesFile.bytes.span()) == ProgramFile::Kind::Bytes);
    AOT_TEST_CHECK(bytesFile.bytes == encodeProgramText(sampleProgram, build));

    std::unique_ptr<Program> fromBytes = createProgram(bytesFile, build);
    std::unique_ptr<Program> fromText = createProgram(textFile, build);
    AOT_TEST_CHECK(fromBytes && fromText);
    AOT_TEST_CHECK(fromBytes->functions.size() == 1 && fromText->functions.size() == 1);
    AOT_TEST_CHECK(!build.diagnostics().hasErrors());

    AOT_TEST_CHECK(encodeProgramText("function"_s, build).isEmpty());
    AOT_TEST_CHECK(build.diagnostics().hasErrors());
}

static void testPrintedDiagnostics()
{
    Diagnostics diagnostics;
    diagnostics.add(InternalError { .kind = InternalErrorKind::NoEntryPoint });
    diagnostics.add(UserDiagnostic { .kind = UserDiagnosticKind::InvalidRegExpLiteral });
    diagnostics.finish(nullptr);

    StringPrintStream out;
    printDiagnostics(diagnostics, out);
    AOT_TEST_CHECK(out.toString() == "InvalidRegExpLiteral: invalid regular expression literal\nNoEntryPoint: the program has no entry point\n"_s);
}

static void testReaderFuzzerShortRun()
{
    StringPrintStream out;
    unsigned failureCount = 0;
    runOnBuildThread([&] {
        failureCount = fuzzReader({ 1, 300, { } }, out);
    });
    if (failureCount)
        dataLog(out.toString());
    AOT_TEST_CHECK(!failureCount);
}

static ChildProcessResult runPrintWire(const TemporaryFile& program, std::initializer_list<ASCIILiteral> otherArguments = { })
{
    Vector<String> arguments { "print"_s, program.path(), "--wire"_s };
    for (ASCIILiteral argument : otherArguments)
        arguments.append(argument);
    return ChildProcessLauncher(String(programPath()), { }).run(arguments.span());
}

static void expectExitCode(const ChildProcessResult& result, int exitCode)
{
    if (!result.exitedWith(exitCode))
        dataLogLn("testaot ", result.description(), ":\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(exitCode));
}

static StringView lastLineOf(StringView output)
{
    StringView lastLine;
    for (StringView line : output.split('\n'))
        lastLine = line;
    return lastLine;
}

static void expectRejectedWith(const ChildProcessResult& result, InternalErrorKind kind)
{
    expectExitCode(result, exitCodeForFailure);
    AOT_TEST_CHECK(lastLineOf(result.output).startsWith(makeString(nameOf(kind), ": "_s)));
}

static bool statesEngineFieldsOfValue(StringView line)
{
    size_t assignment = line.find(" = "_s);
    if (assignment == notFound)
        return false;
    StringView definition = line.left(assignment);
    return definition.contains(':') || definition.contains('~') || definition.contains('@');
}

static void expectWireText(StringView text, std::span<const uint8_t> bytes)
{
    for (StringView line : text.split('\n'))
        AOT_TEST_CHECK(!statesEngineFieldsOfValue(line));

    BuildContext build(makeDefaultCompileOptions());
    AOT_TEST_CHECK(equalSpans(encodeProgramText(text, build).span(), bytes));
}

static void testPrintWireAccepts()
{
    ReaderFixture fixture;
    TemporaryFile program(".bin"_s, fixture.bytes.span());
    ChildProcessResult result = runPrintWire(program);
    expectExitCode(result, exitCodeForSuccess);
    AOT_TEST_CHECK(result.output.contains(" = Convert v9 declared={type=number flags=[IsExpected]}"_s));
    expectWireText(result.output, fixture.bytes.span());
}

static void testPrintWireAcceptsText()
{
    // A function whose first value has a type counts as typed, so the text states the type of each value.
    String typedProgram = sampleProgram;
    for (ASCIILiteral opcode : { "Add"_s, "Mul"_s, "Sub"_s })
        typedProgram = makeStringByReplacingAll(typedProgram, makeString(" = "_s, opcode), makeString(": number = "_s, opcode));
    AOT_TEST_CHECK(typedProgram.contains("v2: number = Add v0, v1\n    v3: number = Mul v2, v2\n    v4: number = Sub v3, v0\n"_s));
    CString typedProgramText = typedProgram.utf8();
    TemporaryFile program(".ir"_s, byteCast<uint8_t>(typedProgramText.span()));
    ChildProcessResult result = runPrintWire(program);
    expectExitCode(result, exitCodeForSuccess);
    AOT_TEST_CHECK(result.output.contains("v2 = Add v0, v1\n    v3 = Mul v2, v2\n    v4 = Sub v3, v0\n"_s));

    BuildContext build(makeDefaultCompileOptions());
    expectWireText(result.output, encodeProgramText(sampleProgram, build).span());
}

static void testPrintWireRejects()
{
    ReaderFixture fixture;
    constexpr size_t damagedOffset = offsetof(Wire::Header, schemaHash);
    setWordAt(fixture.bytes, damagedOffset, ~wordAt(fixture.bytes.span(), damagedOffset));
    TemporaryFile damagedProgram(".bin"_s, fixture.bytes.span());
    expectRejectedWith(runPrintWire(damagedProgram), InternalErrorKind::SchemaMismatch);

    String withoutEntry = makeStringByReplacingAll(sampleProgram, "entry\n"_s, ""_s);
    CString withoutEntryText = withoutEntry.utf8();
    TemporaryFile programWithoutEntry(".ir"_s, byteCast<uint8_t>(withoutEntryText.span()));
    expectRejectedWith(runPrintWire(programWithoutEntry), InternalErrorKind::NoEntryPoint);
}

static void testPrintWireTakesNoOtherFlag()
{
    TemporaryFile program(".ir"_s, byteCast<uint8_t>(sampleProgram.span8()));
    for (ASCIILiteral flag : { "--until=TypeValues"_s, "--function=f0"_s }) {
        ChildProcessResult result = runPrintWire(program, { flag });
        expectExitCode(result, exitCodeForInvalidCommandLine);
        AOT_TEST_CHECK(result.output.contains("--wire takes no other flag"_s));
    }
    expectExitCode(runPrintWire(program, { "no-such-word"_s }), exitCodeForInvalidCommandLine);
}

static void testValidationFailureNames()
{
    for (unsigned i = 0; i < numberOfValidationFailureKinds; ++i) {
        ASCIILiteral name = nameOf(static_cast<ValidationFailureKind>(i));
        AOT_TEST_CHECK(!name.isEmpty());
        for (unsigned j = 0; j < i; ++j)
            AOT_TEST_CHECK(name != nameOf(static_cast<ValidationFailureKind>(j)));
    }
}

std::span<const NativeTest> driverTests()
{
    static constexpr NativeTest tests[] = {
        { "check-lines/no-match"_s, testCheckLinesNoMatch },
        { "check-lines/next"_s, testCheckLinesNext },
        { "check-lines/not"_s, testCheckLinesNot },
        { "check-lines/label"_s, testCheckLinesLabel },
        { "check-lines/variables"_s, testCheckLinesVariables },
        { "check-lines/invalid-patterns"_s, testCheckLinesInvalidPatterns },
        { "check-lines/invalid-sequences"_s, testCheckLinesInvalidSequences },
        { "test-file/well-formed"_s, testTestFileWellFormed },
        { "test-file/diagnostic-names"_s, testDiagnosticNames },
        { "test-file/malformed"_s, testTestFileMalformed },
        { "test-file/comparisons"_s, testTestFileComparisons },
        { "runner/check-lines"_s, testRunnerCheckLines },
        { "runner/commands"_s, testRunnerCommands },
        { "runner/options"_s, testRunnerOptions },
        { "runner/diagnostics"_s, testRunnerDiagnostics },
        { "runner/only-diagnostic"_s, testRunnerOnlyDiagnostic },
        { "runner/diagnostics-of-both-lists"_s, testRunnerDiagnosticsOfBothLists },
        { "runner/verification-failure-names-check"_s, testVerificationFailureNamesCheck },
        { "runner/statistics"_s, testRunnerStatistics },
        { "runner/losses"_s, testRunnerLosses },
        { "runner/losses-without-subject"_s, testRunnerLossesWithoutSubject },
        { "runner/losses-of-whole-build"_s, testRunnerLossesOfWholeBuild },
        { "runner/several-runs"_s, testRunnerSeveralRuns },
        { "runner/pipeline-commands"_s, testRunnerPipelineCommands },
        { "runner/list-passes"_s, testRunnerListsPasses },
        { "runner/pass-context"_s, testRunnerPassContext },
        { "commands/lookup"_s, testCommandsLookup },
        { "program-file/kinds"_s, testProgramFileKinds },
        { "program-file/text-becomes-bytes"_s, testProgramTextBecomesBytes },
        { "program-file/printed-diagnostics"_s, testPrintedDiagnostics },
        { "reader-fuzzer/short-run"_s, testReaderFuzzerShortRun },
        { "subcommands/print-wire-accepts"_s, testPrintWireAccepts },
        { "subcommands/print-wire-accepts-text"_s, testPrintWireAcceptsText },
        { "subcommands/print-wire-rejects"_s, testPrintWireRejects },
        { "subcommands/print-wire-takes-no-other-flag"_s, testPrintWireTakesNoOtherFlag },
        { "validation-failure/names"_s, testValidationFailureNames },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
