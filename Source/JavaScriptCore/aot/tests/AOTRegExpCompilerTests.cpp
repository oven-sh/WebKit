/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTCompileOptionWords.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpTestSupport.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "YarrClassTable.h"
#include "YarrJIT.h"
#include "YarrPattern.h"
#include "YarrSyntaxChecker.h"
#include <algorithm>
#include <memory>
#include <span>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC::AOT {

using namespace RegExpTests;

static RegExpRequest makeRequest(Typed::Program& program, const Source& source)
{
    return {
        .pattern = program.strings.intern(StringView(source.pattern)),
        .flags = source.flags,
    };
}

static CompiledRegExps compileRequests(std::span<const Source> sources, BuildContext& context)
{
    std::unique_ptr<Typed::Program> program = Typed::createEmptyProgram(context);
    AOT_TEST_CHECK(program);
    Vector<RegExpRequest> ascendingRequests;
    for (const Source& source : sources) {
        ascendingRequests.append(makeRequest(*program, source));
        AOT_TEST_CHECK(ascendingRequests.size() == 1 || ascendingRequests[ascendingRequests.size() - 2].pattern.index() < ascendingRequests.last().pattern.index());
    }
    RegExpRequests requests(WTF::move(ascendingRequests));
    CompiledRegExps compiled = compileRegExps(*program, requests, context);
    AOT_TEST_CHECK(compiled.results.size() == sources.size());
    return compiled;
}

static CompiledRegExp compileRegExpWithNestingLimit(const Source& source, unsigned limit)
{
    AssemblerPolicy policy = policyFor(nullptr);
    RegExpSummary summary = summarizeRegExp(source.pattern, source.flags, limit);
    return compileRegExp({ source.pattern, source.flags, summary, policy, nullptr });
}

static bool isDeclined(const CompiledRegExp& regExp, Form form, RegExpDeclineReason reason)
{
    return regExp.form == form && regExp.reason == reason && !regExp.code[0] && !regExp.code[1];
}

static void testEveryRequestGetsItsCode()
{
    Source large { "(\\w+)@[a-z\\xe0-\\xff]+\\.(com|org)"_s, { } };
    Vector<Source> sources;
    for (unsigned index = 0; index < 400; ++index)
        sources.append(Source { makeString(large.pattern, '-', index, '-', large.pattern), { } });
    BuildContext context(makeDefaultCompileOptions());
    CompiledRegExps compiled = compileRequests(sources.span(), context);
    uint64_t bytes = 0;
    for (const CompiledRegExp& result : compiled.results) {
        AOT_TEST_CHECK(result.form == Form::Code);
        bytes += result.codeSize();
    }
    AOT_TEST_CHECK(compiled.results.size() == 400 && bytes > 256 * KB);
    AOT_TEST_CHECK(compiled.numberWithForm(Form::Code) == 400);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
}

static void testCompiledForms()
{
    CompiledRegExp code = compile({ "ab+c"_s, { } });
    AOT_TEST_CHECK(code.form == Form::Code && !code.reason);
    AOT_TEST_CHECK(code.code[0] && !code.code[0]->body.bytes.isEmpty());
    AOT_TEST_CHECK(code.code[1] && !code.code[1]->body.bytes.isEmpty());

    CompiledRegExp atom = compile({ "abc"_s, { } });
    AOT_TEST_CHECK(atom.form == Form::Atom && !atom.reason);
    AOT_TEST_CHECK(!atom.code[0] && !atom.code[1]);
}

static String makeNestedGroups(uint64_t depth)
{
    StringBuilder pattern;
    for (uint64_t level = 0; level < depth; ++level)
        pattern.append('(');
    pattern.append("a+"_s);
    for (uint64_t level = 0; level < depth; ++level)
        pattern.append(')');
    return pattern.toString();
}

static String makePrefixChain(unsigned numberOfWords)
{
    StringBuilder pattern;
    for (unsigned length = 1; length <= numberOfWords; ++length) {
        if (length > 1)
            pattern.append('|');
        for (unsigned index = 0; index < length; ++index)
            pattern.append(static_cast<char>('a' + index % 26));
    }
    return pattern.toString();
}

static String makeWideAlternation(const String& firstAlternative)
{
    StringBuilder pattern;
    pattern.append(firstAlternative);
    for (char first = 'b'; first <= 'r'; ++first)
        pattern.append('|', first, 'x');
    return pattern.toString();
}

static void checkNestingAnswers()
{
    constexpr uint64_t limit = Limits::regExpParenthesesDepth;
    AOT_TEST_CHECK(compile({ makeNestedGroups(limit), { } }).form == Form::Code);
    AOT_TEST_CHECK(isDeclined(compile({ makeNestedGroups(limit + 1), { } }), Form::NoMetadata, RegExpDeclineReason::NestedTooDeep));

    String wrapped = makeWideAlternation(makeNestedGroups(limit));
    AOT_TEST_CHECK(Yarr::checkSyntax(wrapped, OptionSet<Yarr::Flags> { }).maxParenthesesDepth == limit);
    AOT_TEST_CHECK(isDeclined(compile({ wrapped, { } }), Form::NoMetadata, RegExpDeclineReason::NestedTooDeep));
    AOT_TEST_CHECK(compile({ makeWideAlternation(makeNestedGroups(limit - 1)), { } }).form == Form::Code);

    String prefixChain = makePrefixChain(40);
    AOT_TEST_CHECK(!Yarr::checkSyntax(prefixChain, OptionSet<Yarr::Flags> { }).maxParenthesesDepth);
    AOT_TEST_CHECK(isDeclined(compileRegExpWithNestingLimit({ prefixChain, { } }, 8), Form::NoMetadata, RegExpDeclineReason::NestedTooDeep));
    AOT_TEST_CHECK(compileRegExpWithNestingLimit({ makePrefixChain(8), { } }, 8).form == Form::Code);

    RegExpSummary deep = summarizeRegExp(makeNestedGroups(limit + 1), { });
    AOT_TEST_CHECK(deep.isValid() && !deep.canBeCreated() && !Yarr::hasError(deep.constructionError));
    RegExpSummary tooLong = summarizeRegExp("a{4294967295}a"_s, { });
    AOT_TEST_CHECK(tooLong.isValid() && !tooLong.canBeCreated() && tooLong.constructionError == Yarr::ErrorCode::OffsetTooLarge);
}

static void runOnThreadWithStack(size_t stackSize, Function<void()>&& function)
{
    Thread::create("AOT regexp test"_s, WTF::move(function), ThreadType::Unknown, Thread::defaultQOS, Thread::defaultSchedulingPolicy, StackAllocationSpecification::RequestSize(stackSize))->waitForCompletion();
}

static void testDeepNestingDeclines()
{
    AOT_TEST_CHECK(hasStackForRegExpCompiler());
    checkNestingAnswers();
    runOnThreadWithStack(1 * MB, [] {
        if (hasStackForRegExpCompiler())
            checkNestingAnswers();
    });
}

static void testCompilationIsThreadIndependent()
{
    Source source { "(\\w+)@[a-z\\xe0-\\xff]+\\.(com|org)"_s, { } };
    CompiledRegExp onThisThread = compileToCode(source);
    CompiledRegExp onOtherThread;
    runOnThreadWithStack(ParallelLoops::workerStackSize, [&] {
        AOT_TEST_CHECK(hasStackForRegExpCompiler());
        onOtherThread = compileToCode(source);
    });
    for (size_t index = 0; index < onThisThread.code.size(); ++index)
        AOT_TEST_CHECK(isSameCode(*onThisThread.code[index], *onOtherThread.code[index]));
}

static std::unique_ptr<RegExpCode> generate(const Yarr::YarrPattern& pattern, StringView patternString, Yarr::CharSize charSize)
{
    AssemblerPolicy assemblerPolicy = policyFor(nullptr);
    Yarr::YarrCodeBlock codeBlock(nullptr);
    Yarr::CodeGenerationPolicy policy;
    policy.sizePolicy = Yarr::CodeSizePolicy::Compact;
    Yarr::CodeGenerationRequest request {
        .pattern = pattern,
        .patternString = patternString,
        .charSize = charSize,
        .executionMode = Yarr::ExecutionMode::IncludeSubpatterns,
        .sampleString = std::nullopt,
        .codeBlock = &codeBlock,
        .policy = policy,
        .createCodeAddressing = [&](CCallHelpers& jit) -> std::unique_ptr<Yarr::CodeAddressing> {
            return makeUnique<ImageCodeAddressing>(jit, PCRelativeReach::Near, assemblerPolicy, nullptr);
        },
    };
    auto generated = Yarr::generateCode(request);
    AOT_TEST_CHECK(generated.has_value());
    auto code = static_cast<ImageCodeAddressing&>(generated->addressing()).takeCode();
    AOT_TEST_CHECK(code.has_value());
    return WTF::move(*code);
}

static void checkPatternServesBothWidthsAndRetries(ASCIILiteral patternString)
{
    Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
    const Yarr::YarrPattern pattern(patternString, { }, error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::Performed);
    AOT_TEST_CHECK(!Yarr::hasError(error));

    auto narrow = generate(pattern, patternString, Yarr::CharSize::Char8);
    AOT_TEST_CHECK(isSameCode(*narrow, *generate(pattern, patternString, Yarr::CharSize::Char8)));
    auto wide = generate(pattern, patternString, Yarr::CharSize::Char16);
    AOT_TEST_CHECK(isSameCode(*narrow, *generate(pattern, patternString, Yarr::CharSize::Char8)));
    AOT_TEST_CHECK(isSameCode(*wide, *generate(pattern, patternString, Yarr::CharSize::Char16)));
}

static void testOnePatternServesBothWidthsAndRetries()
{
    checkPatternServesBothWidthsAndRetries("[a-c]xyz"_s);
    AOT_TEST_CHECK(compileToCode({ "a*e"_s, { } }).code[0]->hasElidedFrame);
    checkPatternServesBothWidthsAndRetries("a*e"_s);
    AOT_TEST_CHECK(!compileToCode({ "a*b"_s, { } }).code[0]->hasElidedFrame);
    checkPatternServesBothWidthsAndRetries("a*b"_s);
    for (ASCIILiteral pattern : { "T\\d\\d:\\d\\d:\\d\\d\\+\\d\\d:\\d\\d"_s, "T\\d\\d:\\d\\d:\\d\\d\\.00Z"_s, "abcd[xy]e[xy][xy]fghi"_s, "abcd[xy]ef[xy]ghij"_s })
        checkPatternServesBothWidthsAndRetries(pattern);
}

static bool searchesForOneOfAClass(ASCIILiteral patternString)
{
    CompiledRegExp regExp = compileToCode({ patternString, { } });
    return std::ranges::any_of(regExp.code[1]->symbols, [](const RegExpSymbol& symbol) {
        auto* routine = std::get_if<Yarr::SharedRoutine>(&symbol);
        return routine && (routine->kind() == Yarr::SharedRoutine::Kind::FindFirstOfClass || routine->kind() == Yarr::SharedRoutine::Kind::FindFirstOfTwoUnits);
    });
}

static void testOnlyARareFirstCharacterIsSearchedFor()
{
    AOT_TEST_CHECK(searchesForOneOfAClass("[#%&~]+"_s));
    AOT_TEST_CHECK(searchesForOneOfAClass("\\b[0-9]+"_s));
    AOT_TEST_CHECK(!searchesForOneOfAClass("[a-z]+"_s));
    AOT_TEST_CHECK(!searchesForOneOfAClass("\\b[a-z0-9-]+"_s));
    AOT_TEST_CHECK(searchesForOneOfAClass("\\u001b\\[[0-9;]*m"_s));
    AOT_TEST_CHECK(searchesForOneOfAClass("~~[a-z]+"_s));
    AOT_TEST_CHECK(!searchesForOneOfAClass("needle[0-9]"_s));
    AOT_TEST_CHECK(!searchesForOneOfAClass("//+"_s));
}

static std::optional<Yarr::SharedRoutine::FindFirstPairOfUnits> pairSearchedFor(ASCIILiteral patternString, OptionSet<Yarr::Flags> flags, size_t width)
{
    CompiledRegExp regExp = compileToCode({ patternString, flags });
    for (const RegExpSymbol& symbol : regExp.code[width]->symbols) {
        auto* routine = std::get_if<Yarr::SharedRoutine>(&symbol);
        if (routine && routine->kind() == Yarr::SharedRoutine::Kind::FindFirstPairOfUnits)
            return std::get<Yarr::SharedRoutine::FindFirstPairOfUnits>(routine->payload);
    }
    return std::nullopt;
}

static void testTwoPlacesOfOneUnitEachAreSearchedForAtOnce()
{
    using Pair = Yarr::SharedRoutine::FindFirstPairOfUnits;
    using enum Yarr::PositionUnits;
    AOT_TEST_CHECK((pairSearchedFor("e[a-z]{2}[0-9]{3}-[a-z]+"_s, { }, 1) == Pair { Yarr::CharSize::Char16, 6, One, One }));
    AOT_TEST_CHECK((pairSearchedFor("ab+c"_s, { }, 1) == Pair { Yarr::CharSize::Char16, 1, One, One }));
    AOT_TEST_CHECK((pairSearchedFor("k[0-9]s[0-9]+"_s, { Yarr::Flags::IgnoreCase }, 1) == Pair { Yarr::CharSize::Char16, 2, IgnoreCase, IgnoreCase }));
    AOT_TEST_CHECK((pairSearchedFor("x-[a-z]{2}:[0-9]+"_s, { Yarr::Flags::IgnoreCase }, 1) == Pair { Yarr::CharSize::Char16, 3, One, One }));
    AOT_TEST_CHECK((pairSearchedFor("\"(?:_|\\\\u005[Ff])(?:_|\\\\u005[Ff])p"_s, { }, 1) == Pair { Yarr::CharSize::Char16, 1, One, Two }));
    AOT_TEST_CHECK((pairSearchedFor("[cd]:[a-z]+e"_s, { }, 1) == Pair { Yarr::CharSize::Char16, 1, Two, One }));
#if CPU(X86_64)
    AOT_TEST_CHECK((pairSearchedFor("\"(?:c|\\\\u0063)(?:o|\\\\u006[Ff])\"\\s*:"_s, { }, 0) == Pair { Yarr::CharSize::Char8, 1, One, Two }));
    AOT_TEST_CHECK((pairSearchedFor("e[a-z]{2}[0-9]{3}-[a-z]+"_s, { }, 0) == Pair { Yarr::CharSize::Char8, 6, One, One }));
    AOT_TEST_CHECK((pairSearchedFor("k[0-9]s[0-9]+"_s, { Yarr::Flags::IgnoreCase }, 0) == Pair { Yarr::CharSize::Char8, 2, IgnoreCase, IgnoreCase }));
#endif
    AOT_TEST_CHECK((!pairSearchedFor("[a-z]bz?.7"_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("[a-z]:#?[^=]+"_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("\\w(?:@|.)=[0-9]"_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("[a-z]#=|[a-z].="_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("[a-z](?:#|[\\u0100-\\u0fff])="_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("[ab][cd]+e"_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("aaa+[bc]"_s, { }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("e[a-z]{2}[0-9]{3}-[a-z]+"_s, { Yarr::Flags::Unicode }, 1)));
    AOT_TEST_CHECK((!pairSearchedFor("e[a-z]{2}[0-9]{3}-[a-z]+"_s, { Yarr::Flags::Sticky }, 1)));
}

static void testUnicodePropertyLookup()
{
    OptionSet<Yarr::Flags> unicode { Yarr::Flags::Unicode };
    AOT_TEST_CHECK(summarizeRegExp("\\p{L}"_s, unicode).hasUnicodePropertyLookup);
    AOT_TEST_CHECK(summarizeRegExp("\\p{L}"_s, { }).hasUnicodePropertyLookup);
    AOT_TEST_CHECK(!summarizeRegExp("[\\w-.]"_s, { }).hasUnicodePropertyLookup);
    AOT_TEST_CHECK(!summarizeRegExp("\\u{61}"_s, unicode).hasUnicodePropertyLookup);

    RegExpSummary unknownProperty = summarizeRegExp("\\p{Nope}"_s, unicode);
    AOT_TEST_CHECK(!unknownProperty.isValid() && unknownProperty.hasUnicodePropertyLookup);
    RegExpSummary unbalanced = summarizeRegExp("("_s, { });
    AOT_TEST_CHECK(!unbalanced.isValid() && !unbalanced.hasUnicodePropertyLookup);
}

static size_t numberOfClassesMadeFor(ASCIILiteral patternString, OptionSet<Yarr::Flags> flags = { })
{
    Yarr::ErrorCode error = Yarr::ErrorCode::NoError;
    const Yarr::YarrPattern pattern(patternString, flags, error, Yarr::ExecutionMode::IncludeSubpatterns, Yarr::PatternAnalysis::CharacterClassForms);
    AOT_TEST_CHECK(!Yarr::hasError(error));
    return pattern.m_userCharacterClasses.size();
}

static void testARepeatedClassIsParsedOnce()
{
    AOT_TEST_CHECK(numberOfClassesMadeFor("[ab]"_s) == 1);
    AOT_TEST_CHECK(numberOfClassesMadeFor("[ab][ab]"_s) == 1);
    AOT_TEST_CHECK(numberOfClassesMadeFor("x[ab]*y[ab]+z([ab])|[ab]{2}"_s) == 1);
    AOT_TEST_CHECK(numberOfClassesMadeFor("[ab][cd][ab]"_s) == 3);
    AOT_TEST_CHECK(numberOfClassesMadeFor("[ab][ab]c][ab-]"_s) == 2);
    AOT_TEST_CHECK(numberOfClassesMadeFor("[^ab][^ab][ab]"_s) == 2);
    AOT_TEST_CHECK(numberOfClassesMadeFor("[ab](?i:[ab][ab])[ab]"_s) == 3);
    AOT_TEST_CHECK(numberOfClassesMadeFor("(?<name>[ab])\\k<name>[ab]"_s) == 1);
    AOT_TEST_CHECK(!summarizeRegExp("[ab][ab"_s, { }).isValid() && !summarizeRegExp("[b-a][b-a]"_s, { }).isValid() && summarizeRegExp("[a-b][a-b]"_s, { }).isValid());
}

static unsigned analysisLimitHits(const CompiledRegExp& regExp, Yarr::CharSize charSize)
{
    return regExp.code[static_cast<size_t>(charSize)]->analysisLimitHits;
}

static void testClassTableLimitsAreCounted()
{
    OptionSet<Yarr::Flags> unicode { Yarr::Flags::Unicode };

    static_assert(0x100100 / Yarr::ClassTableLayout::charactersInBlock > Yarr::ClassTablePacker::maxPages);
    CompiledRegExp tooManyPages = compileToCode({ "[\\u{100100}\\u{100300}\\u{100500}\\u{100700}\\u{100900}\\u{100b00}\\u{100d00}\\u{100f00}]+x"_s, unicode });
    AOT_TEST_CHECK(!analysisLimitHits(tooManyPages, Yarr::CharSize::Char8));
    AOT_TEST_CHECK(analysisLimitHits(tooManyPages, Yarr::CharSize::Char16));

    CompiledRegExp withTable = compileToCode({ "[\\u0401\\u0501\\u0601\\u0701\\u0801\\u0901\\u0a01\\u0b01]+x"_s, unicode });
    AOT_TEST_CHECK(!analysisLimitHits(withTable, Yarr::CharSize::Char16));
}

std::span<const NativeTest> regExpCompilerTests()
{
    static constexpr NativeTest tests[] = {
        { "regexp-compiler/compiled-forms"_s, testCompiledForms },
        { "regexp-compiler/a-repeated-class-is-parsed-once"_s, testARepeatedClassIsParsedOnce },
        { "regexp-compiler/deep-nesting-declines"_s, testDeepNestingDeclines },
        { "regexp-compiler/every-request-gets-its-code"_s, testEveryRequestGetsItsCode },
        { "regexp-compiler/compilation-is-thread-independent"_s, testCompilationIsThreadIndependent },
        { "regexp-compiler/one-pattern-serves-both-widths-and-retries"_s, testOnePatternServesBothWidthsAndRetries },
        { "regexp-compiler/only-a-rare-first-character-is-searched-for"_s, testOnlyARareFirstCharacterIsSearchedFor },
        { "regexp-compiler/two-places-of-one-unit-each-are-searched-for-at-once"_s, testTwoPlacesOfOneUnitEachAreSearchedForAtOnce },
        { "regexp-compiler/unicode-property-lookup"_s, testUnicodePropertyLookup },
        { "regexp-compiler/class-table-limits-are-counted"_s, testClassTableLimitsAreCounted },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
