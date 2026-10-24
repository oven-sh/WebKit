/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTIRTestSupport.h"
#include "AOTRegExpCodeAddressing.h"
#include "AOTRegExpTestSupport.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "YarrClassTable.h"
#include "YarrJIT.h"
#include "YarrPattern.h"
#include "YarrSyntaxChecker.h"
#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC::AOT {

using namespace RegExpTests;

static RegExpRequest makeRequest(Typed::Program& program, const Source& source, RegExpRequestOrigin origin)
{
    return {
        .pattern = program.strings.intern(StringView(source.pattern)),
        .flags = source.flags,
        .origin = origin,
        .sites = { RegExpSite { FunctionID(0u), ValueID(0u) } },
    };
}

namespace {

struct FoundText {
    Source source;
    RegExpRequestOrigin origin { RegExpRequestOrigin::Candidate };
};

} // anonymous namespace

static CompiledRegExps compileRequests(std::span<const Source> literals, std::span<const FoundText> foundTexts, BuildContext& context)
{
    Typed::TestProgram program;
    auto append = [&](Vector<RegExpRequest>& requests, const Source& source, RegExpRequestOrigin origin) {
        requests.append(makeRequest(program.program, source, origin));
        AOT_TEST_CHECK(requests.size() == 1 || requests[requests.size() - 2].pattern.index() < requests.last().pattern.index());
    };
    Vector<RegExpRequest> requestsForFoundTexts;
    for (const FoundText& text : foundTexts)
        append(requestsForFoundTexts, text.source, text.origin);
    Vector<RegExpRequest> requestsForLiterals;
    for (const Source& literal : literals)
        append(requestsForLiterals, literal, RegExpRequestOrigin::Literal);
    RegExpRequests requests(WTF::move(requestsForLiterals), WTF::move(requestsForFoundTexts));
    CompiledRegExps compiled = compileRegExps(program.program, requests, context);
    AOT_TEST_CHECK(compiled.results.size() == literals.size() + foundTexts.size());
    return compiled;
}

static bool isDeclined(const CompiledRegExp& regExp, Form form, RegExpDeclineReason reason)
{
    return regExp.form == form && regExp.reason == reason && !regExp.code[0] && !regExp.code[1];
}

static void testRequestPastSectionSizeLimitLosesCode()
{
    std::array<Source, 3> sources { Source { "ab+c"_s, { } }, Source { "\\w+\\s"_s, { } }, Source { "x[yz]+"_s, { } } };
    uint64_t codeSize = 0;
    {
        BuildContext context(makeDefaultCompileOptions());
        CompiledRegExps compiled = compileRequests(sources, { }, context);
        for (const CompiledRegExp& regExp : compiled.results) {
            AOT_TEST_CHECK(regExp.form == Form::Code);
            codeSize += regExp.codeSize();
        }
    }
    {
        BuildContext context(makeOptions(makeString("limit=RegExpSectionSize:"_s, 2 * codeSize)));
        AOT_TEST_CHECK(compileRequests(sources, { }, context).numberWithForm(Form::Code) == 3);
        AOT_TEST_CHECK(!limitHits(context, LimitID::RegExpSectionSize));
    }

    BuildContext context(makeOptions(makeString("limit=RegExpSectionSize:"_s, 2 * codeSize - 2)));
    CompiledRegExps compiled = compileRequests(sources, { }, context);
    AOT_TEST_CHECK(compiled.results[0].form == Form::Code && compiled.results[1].form == Form::Code);
    AOT_TEST_CHECK(isDeclined(compiled.results[2], Form::NoCode, RegExpDeclineReason::SectionFull));
    AOT_TEST_CHECK(limitHits(context, LimitID::RegExpSectionSize) == 1);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
}

static void testCandidateBudget()
{
    Source large { "(\\w+)@[a-z\\xe0-\\xff]+\\.(com|org)"_s, { } };
    Source evaluated { makeString(large.pattern, '-', large.pattern, '-', large.pattern), { } };
    std::array<FoundText, 4> foundTexts {
        FoundText { { "ab+c"_s, { } }, RegExpRequestOrigin::Candidate },
        FoundText { large, RegExpRequestOrigin::Candidate },
        FoundText { { "de+f"_s, { } }, RegExpRequestOrigin::Candidate },
        FoundText { evaluated, RegExpRequestOrigin::Evaluated },
    };
    std::array<uint64_t, 4> sizes;
    {
        BuildContext context(makeDefaultCompileOptions());
        CompiledRegExps compiled = compileRequests({ }, foundTexts, context);
        for (size_t index = 0; index < sizes.size(); ++index) {
            AOT_TEST_CHECK(compiled.results[index].form == Form::Code);
            sizes[index] = compiled.results[index].codeSize();
        }
    }
    uint64_t budget = sizes[0] + sizes[2];
    AOT_TEST_CHECK(sizes[1] > sizes[2] && sizes[3] > budget);

    // A text that the build evaluated counts as a literal does: the budget of a site is for what flow names.
    BuildContext context(makeOptions(makeString("limit=RegExpCandidateCodeBytesPerSite:"_s, budget)));
    CompiledRegExps compiled = compileRequests(singleElementSpan(large), foundTexts, context);
    AOT_TEST_CHECK(compiled.results[0].form == Form::Code);
    AOT_TEST_CHECK(compiled.results[1].form == Form::Code);
    AOT_TEST_CHECK(isDeclined(compiled.results[2], Form::NoCode, RegExpDeclineReason::CandidateBudgetExhausted));
    AOT_TEST_CHECK(compiled.results[3].form == Form::Code);
    AOT_TEST_CHECK(compiled.results[4].form == Form::Code && compiled.results[4].codeSize() == sizes[3]);
    AOT_TEST_CHECK(limitHits(context, LimitID::RegExpCandidateCodeBytesPerSite) == 1);
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

static void checkWithShortPatternLimit(ASCIILiteral pattern, bool isTooLong)
{
    Source source { pattern, { } };
    BuildContext context(makeOptions("limit=RegExpPatternLengthWithCode:4"_s));
    CompiledRegExps compiled = compileRequests(singleElementSpan(source), { }, context);
    if (isTooLong)
        AOT_TEST_CHECK(isDeclined(compiled.results[0], Form::NoCode, RegExpDeclineReason::PatternTooLong));
    else
        AOT_TEST_CHECK(compiled.results[0].form == Form::Code && !compiled.results[0].reason);
    AOT_TEST_CHECK(limitHits(context, LimitID::RegExpPatternLengthWithCode) == (isTooLong ? 1u : 0u));
}

static void testPatternLengthLimit()
{
    checkWithShortPatternLimit("abcdef+"_s, true);
    checkWithShortPatternLimit("(?<n>abcdef+)"_s, true);
    checkWithShortPatternLimit("ab+"_s, false);
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

    BuildContext context(makeOptions("limit=RegExpNestingDepthWithCode:8"_s));
    String prefixChain = makePrefixChain(40);
    AOT_TEST_CHECK(!Yarr::checkSyntax(prefixChain, OptionSet<Yarr::Flags> { }).maxParenthesesDepth);
    AOT_TEST_CHECK(isDeclined(compile({ prefixChain, { } }, &context), Form::NoMetadata, RegExpDeclineReason::NestedTooDeep));
    AOT_TEST_CHECK(compile({ makePrefixChain(8), { } }, &context).form == Form::Code);
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
    // Where the b that every match holds was last found lies in the frame.
    AOT_TEST_CHECK(!compileToCode({ "a*b"_s, { } }).code[0]->hasElidedFrame);
    checkPatternServesBothWidthsAndRetries("a*b"_s);
}

// In the code for 16-bit subjects, where every CPU calls the routine. For 8-bit subjects ARM64 has the search in line.
static bool searchesForOneOfAClass(ASCIILiteral patternString)
{
    CompiledRegExp regExp = compileToCode({ patternString, { } });
    return std::ranges::any_of(regExp.code[1]->symbols, [](const RegExpSymbol& symbol) {
        auto* routine = std::get_if<Yarr::SharedRoutine>(&symbol);
        return routine && routine->kind() == Yarr::SharedRoutine::Kind::FindFirstOfClass;
    });
}

// A search for the place where a match can begin stops at once where most characters of a subject can begin one.
static void testOnlyARareFirstCharacterIsSearchedFor()
{
    AOT_TEST_CHECK(searchesForOneOfAClass("[#%&~]+"_s));
    AOT_TEST_CHECK(searchesForOneOfAClass("\\b[0-9]+"_s));
    AOT_TEST_CHECK(!searchesForOneOfAClass("[a-z]+"_s));
    AOT_TEST_CHECK(!searchesForOneOfAClass("\\b[a-z0-9-]+"_s));
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

// Where the text of the class parsed last follows once more, the parser reads none of it and the pattern gets no second class. The flags in force are part of what a class holds.
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
        { "regexp-compiler/pattern-length-limit"_s, testPatternLengthLimit },
        { "regexp-compiler/deep-nesting-declines"_s, testDeepNestingDeclines },
        { "regexp-compiler/request-past-section-size-limit-loses-code"_s, testRequestPastSectionSizeLimitLosesCode },
        { "regexp-compiler/candidate-budget"_s, testCandidateBudget },
        { "regexp-compiler/compilation-is-thread-independent"_s, testCompilationIsThreadIndependent },
        { "regexp-compiler/one-pattern-serves-both-widths-and-retries"_s, testOnePatternServesBothWidthsAndRetries },
        { "regexp-compiler/only-a-rare-first-character-is-searched-for"_s, testOnlyARareFirstCharacterIsSearchedFor },
        { "regexp-compiler/unicode-property-lookup"_s, testUnicodePropertyLookup },
        { "regexp-compiler/class-table-limits-are-counted"_s, testClassTableLimitsAreCounted },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
