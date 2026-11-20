/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/RegularExpression.h>
#include <JavaScriptCore/YarrFlags.h>
#include <JavaScriptCore/YarrInterpreter.h>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace TestWebKitAPI {

using JSC::Yarr::RegularExpression;

TEST(JavaScriptCore_RegularExpression, DuplicateNamedCaptureGroupSimple)
{
    JSC::initialize();

    RegularExpression re("(?<a>x)|(?<a>y)"_s, { JSC::Yarr::Flags::UnicodeSets });
    EXPECT_TRUE(re.isValid());

    int matchLength = 0;
    EXPECT_EQ(0, re.match("x"_s, 0, &matchLength));
    EXPECT_EQ(1, matchLength);

    EXPECT_EQ(0, re.match("y"_s, 0, &matchLength));
    EXPECT_EQ(1, matchLength);
}

TEST(JavaScriptCore_RegularExpression, DuplicateNamedCaptureGroupMultiple)
{
    JSC::initialize();

    RegularExpression re("(?<a>x)|(?<a>y)|(?<b>x)|(?<b>y)|(?<c>x)|(?<c>y)"_s, { JSC::Yarr::Flags::UnicodeSets });
    EXPECT_TRUE(re.isValid());

    int matchLength = 0;
    EXPECT_EQ(0, re.match("x"_s, 0, &matchLength));
    EXPECT_EQ(1, matchLength);

    EXPECT_EQ(0, re.match("y"_s, 0, &matchLength));
    EXPECT_EQ(1, matchLength);
}

TEST(JavaScriptCore_RegularExpression, DuplicateNamedCaptureGroupNoMatch)
{
    JSC::initialize();

    RegularExpression re("(?<a>x)|(?<a>y)"_s, { JSC::Yarr::Flags::UnicodeSets });
    EXPECT_TRUE(re.isValid());

    int matchLength = 0;
    EXPECT_EQ(-1, re.match("z"_s, 0, &matchLength));
}

TEST(JavaScriptCore_RegularExpression, DuplicateNamedCaptureGroupSearchRev)
{
    JSC::initialize();

    RegularExpression re("(?<a>x)|(?<a>y)"_s, { JSC::Yarr::Flags::UnicodeSets });
    EXPECT_TRUE(re.isValid());

    EXPECT_EQ(3, re.searchRev("abxyz"_s));
    EXPECT_EQ(1, re.matchedLength());
}

static String repeated(ASCIILiteral text, unsigned count)
{
    StringBuilder builder;
    while (count--)
        builder.append(text);
    return builder.toString();
}

static unsigned matchWithinBudget(StringView source, StringView input, size_t& remainingSteps)
{
    using namespace JSC::Yarr;
    ErrorCode error { ErrorCode::NoError };
    YarrPattern pattern(source, { Flags::UnicodeSets }, error);
    BumpPointerAllocator allocator;
    auto bytecode = byteCompile(pattern, &allocator, error);
    Vector<unsigned> offsets(bytecode->m_offsetsSize);
    return interpretWithinBudget(bytecode.get(), input, 0, offsets.mutableSpan().data(), remainingSteps);
}

TEST(JavaScriptCore_RegularExpression, MatchWithinBudget)
{
    JSC::initialize();

    size_t steps = 1000;
    EXPECT_EQ(1u, matchWithinBudget("(a+)b\\1"_s, "xaabaa"_s, steps));
    EXPECT_LT(0u, steps);
    EXPECT_GT(1000u, steps);

    steps = 1000;
    EXPECT_EQ(JSC::Yarr::offsetNoMatch, matchWithinBudget("a+b"_s, "aac"_s, steps));
    EXPECT_LT(0u, steps);

    steps = 0;
    EXPECT_EQ(JSC::Yarr::offsetNoMatch, matchWithinBudget("a"_s, "a"_s, steps));
}

TEST(JavaScriptCore_RegularExpression, GiveUpWithoutBudget)
{
    JSC::initialize();

    auto input = repeated("a"_s, 100000);
    Vector<String> sources { "^a*a*a*a*a*a*a*a*b"_s, "^(a*)*b"_s, "^a*?a*?a*?a*?b"_s, "^[a]*[a]*[a]*[a]*b"_s, "^a*a{50000}a*b"_s, "^(a*)a*\\1\\1b"_s, "(?<=ba*)b"_s, repeated("(?=(.*))"_s, 5000) };
    for (auto& source : sources) {
        size_t steps = 1000000;
        EXPECT_EQ(JSC::Yarr::offsetNoMatch, matchWithinBudget(source, input, steps));
        EXPECT_EQ(0u, steps);
    }
}

TEST(JavaScriptCore_RegularExpression, ExpansionLimit)
{
    using namespace JSC::Yarr;
    JSC::initialize();

    auto compile = [](StringView source) {
        ErrorCode error { ErrorCode::NoError };
        YarrPattern pattern(source, { Flags::UnicodeSets }, error, ExecutionMode::IncludeSubpatterns, PatternAnalysis::Skipped, 100000);
        return error;
    };
    EXPECT_EQ(ErrorCode::NoError, compile("\\p{RGI_Emoji}[\\p{L}--[a-z]]"_s));
    EXPECT_EQ(ErrorCode::NoError, compile(repeated("\\p{L}"_s, 1000)));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(repeated("a"_s, 100001)));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(repeated("\\p{RGI_Emoji}"_s, 1000)));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(repeated("[\\p{RGI_Emoji}]+"_s, 1000)));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(repeated("[\\p{L}]"_s, 1000)));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(makeString('[', repeated("\\P{L}"_s, 1000), ']')));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(makeString("[\\p{L}"_s, repeated("--a"_s, 1000), ']')));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(makeString("[\\p{L}"_s, repeated("[a]"_s, 1000), ']')));
    EXPECT_EQ(ErrorCode::PatternTooLarge, compile(makeString("[\\p{RGI_Emoji}"_s, repeated("--\\q{ab}"_s, 1000), ']')));
}

} // namespace TestWebKitAPI
