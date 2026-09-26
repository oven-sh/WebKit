/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "MoveOnly.h"
#include "Helpers/Test.h"
#include "Helpers/WTFTestUtilities.h"
#include <sstream>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>
#include <wtf/unicode/CharacterNames.h>

namespace TestWebKitAPI {

static String builderContent(const StringBuilder& builder)
{
    // Not using builder.toString() or builder.toStringPreserveCapacity() because they all
    // change internal state of builder.
    if (builder.is8Bit())
        return builder.span<Latin1Character>();
    return builder.span<char16_t>();
}

void expectEmpty(const StringBuilder& builder)
{
    EXPECT_EQ(0U, builder.length());
    EXPECT_TRUE(builder.isEmpty());
    EXPECT_EQ(0, builder.span8().data());
}

TEST(StringBuilderTest, DefaultConstructor)
{
    StringBuilder builder;
    expectEmpty(builder);
}

TEST(StringBuilderTest, Append)
{
    StringBuilder builder;
    builder.append(String("0123456789"_s));
    EXPECT_EQ("0123456789"_s, builderContent(builder));
    builder.append("abcd"_s);
    EXPECT_EQ("0123456789abcd"_s, builderContent(builder));
    builder.append(std::span { reinterpret_cast<const Latin1Character*>("efgh"), 3 });
    EXPECT_EQ("0123456789abcdefg"_s, builderContent(builder));
    builder.append(""_s);
    EXPECT_EQ("0123456789abcdefg"_s, builderContent(builder));
    builder.append('#');
    EXPECT_EQ("0123456789abcdefg#"_s, builderContent(builder));

    builder.toString(); // Test after reifyString().
    StringBuilder builder1;
    builder.append(""_span);
    EXPECT_EQ("0123456789abcdefg#"_s, builderContent(builder));
    builder1.append(builder.span<Latin1Character>());
    builder1.append("XYZ"_s);
    builder.append(builder1.span<Latin1Character>());
    EXPECT_EQ("0123456789abcdefg#0123456789abcdefg#XYZ"_s, builderContent(builder));

    StringBuilder builder2;
    builder2.reserveCapacity(100);
    builder2.append("xyz"_s);
    const Latin1Character* characters = builder2.span8().data();
    builder2.append("0123456789"_s);
    EXPECT_EQ(characters, builder2.span8().data());
    builder2.toStringPreserveCapacity(); // Test after reifyString with buffer preserved.
    builder2.append("abcd"_s);
    EXPECT_EQ(characters, builder2.span8().data());

    // Test appending char32_t characters to StringBuilder.
    StringBuilder builderForUTF32Append;
    char32_t frakturAChar = 0x1D504;
    builderForUTF32Append.append(frakturAChar); // The fraktur A is not in the BMP, so it's two UTF-16 code units long.
    EXPECT_EQ(2U, builderForUTF32Append.length());
    builderForUTF32Append.append(U'A');
    EXPECT_EQ(3U, builderForUTF32Append.length());
    const char16_t resultArray[] = { U16_LEAD(frakturAChar), U16_TRAIL(frakturAChar), 'A' };
    EXPECT_EQ(String({ resultArray, std::size(resultArray) }), builderContent(builderForUTF32Append));
    {
        StringBuilder builder;
        StringBuilder builder2;
        char32_t frakturAChar = 0x1D504;
        const char16_t data[] = { U16_LEAD(frakturAChar), U16_TRAIL(frakturAChar) };
        builder2.append(std::span { data });
        EXPECT_EQ(2U, builder2.length());
        String result2 = builder2.toString();
        EXPECT_EQ(2U, result2.length());
        builder.append(builder2);
        builder.append(std::span { data });
        EXPECT_EQ(4U, builder.length());
        const char16_t resultArray[] = { U16_LEAD(frakturAChar), U16_TRAIL(frakturAChar), U16_LEAD(frakturAChar), U16_TRAIL(frakturAChar) };
        EXPECT_EQ(String({ resultArray, std::size(resultArray) }), builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append(u8"Water🍉Melon"_span);
        EXPECT_EQ(builder.toString(), u8"Water🍉Melon"_span);
    }
}

TEST(StringBuilderTest, AppendIntMin)
{
    constexpr int intMin = std::numeric_limits<int>::min();
    StringBuilder builder;
    builder.append(intMin);

    std::stringstream stringStream;
    stringStream << intMin;
    std::string expectedString;
    stringStream >> expectedString;

    EXPECT_EQ(String::fromLatin1(expectedString.c_str()), builderContent(builder));
}

TEST(StringBuilderTest, VariadicAppend)
{
    {
        StringBuilder builder;
        builder.append(String("0123456789"_s));
        EXPECT_EQ("0123456789"_s, builderContent(builder));
        builder.append("abcd"_s);
        EXPECT_EQ("0123456789abcd"_s, builderContent(builder));
        builder.append('e');
        EXPECT_EQ("0123456789abcde"_s, builderContent(builder));
        builder.append(""_s);
        EXPECT_EQ("0123456789abcde"_s, builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append(String("0123456789"_s), "abcd"_s, 'e', ""_s);
        EXPECT_EQ("0123456789abcde"_s, builderContent(builder));
        builder.append(String("A"_s), "B"_s, 'C', ""_s);
        EXPECT_EQ("0123456789abcdeABC"_s, builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append(String("0123456789"_s), "abcd"_s, bullseye, ""_s);
        EXPECT_EQ(makeString("0123456789abcd"_s, bullseye), builderContent(builder));
        builder.append(String("A"_s), "B"_s, 'C', ""_s);
        EXPECT_EQ(makeString("0123456789abcd"_s, bullseye, "ABC"_s), builderContent(builder));
    }

    {
        // Test where we upconvert the StringBuilder from 8-bit to 16-bit, and don't fit in the existing capacity.
        StringBuilder builder;
        builder.append(String("0123456789"_s), "abcd"_s, 'e', ""_s);
        EXPECT_EQ("0123456789abcde"_s, builderContent(builder));
        EXPECT_TRUE(builder.is8Bit());
        EXPECT_LT(builder.capacity(), builder.length() + 3);
        builder.append(String("A"_s), "B"_s, bullseye, ""_s);
        EXPECT_EQ(makeString("0123456789abcdeAB"_s, bullseye), builderContent(builder));
    }

    {
        // Test where we upconvert the StringBuilder from 8-bit to 16-bit, but would have fit in the capacity if the upconvert wasn't necessary.
        StringBuilder builder;
        builder.append(String("0123456789"_s), "abcd"_s, 'e', ""_s);
        EXPECT_EQ("0123456789abcde"_s, builderContent(builder));
        builder.reserveCapacity(32);
        EXPECT_TRUE(builder.is8Bit());
        EXPECT_GE(builder.capacity(), builder.length() + 3);
        builder.append(String("A"_s), "B"_s, bullseye, ""_s);
        EXPECT_EQ(makeString("0123456789abcdeAB"_s, bullseye), builderContent(builder));
    }
}

TEST(StringBuilderTest, Interleave)
{
    std::array strings { "a"_s, "b"_s, "c"_s };

    {
        StringBuilder builder;
        builder.append('[', interleave(strings, [](auto& builder, auto& s) { builder.append(s); }, ", "_s), ']');

        EXPECT_EQ(String("[a, b, c]"_s), builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append('[', interleave(strings, [](auto& s) { return s; }, ", "_s), ']');

        EXPECT_EQ(String("[a, b, c]"_s), builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append('[', interleave(strings, ", "_s), ']');

        EXPECT_EQ(String("[a, b, c]"_s), builderContent(builder));
    }
}

struct A { int value; };
struct B { int value; };

[[maybe_unused]] static void serializeOverloadBuilder(StringBuilder& builder, const A& a)
{
    builder.append(a.value);
}

[[maybe_unused]] static void serializeOverloadBuilder(StringBuilder& builder, const B& b)
{
    builder.append(b.value);
}

[[maybe_unused]] static int serializeOverloadReturn(const A& a)
{
    return a.value;
}

[[maybe_unused]] static int serializeOverloadReturn(const B& b)
{
    return b.value;
}

TEST(StringBuilderTest, InterleaveOverload)
{
    std::array as { A { 1 }, A { 2 }, A { 3 } };

    {
        StringBuilder builder;
        builder.append('[', interleave(as, serializeOverloadBuilder, ", "_s), ']');

        EXPECT_EQ(String("[1, 2, 3]"_s), builderContent(builder));
    }
}

TEST(StringBuilderTest, InterleaveNoCopies)
{
    std::array values { MoveOnly { 1 }, MoveOnly { 2 }, MoveOnly { 3 } };

    {
        StringBuilder builder;
        builder.append('[', interleave(values, [](auto& builder, auto& moveOnly) { builder.append(moveOnly.value()); }, ", "_s), ']');

        EXPECT_EQ(String("[1, 2, 3]"_s), builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append('[', interleave(values, [](auto& moveOnly) { return moveOnly.value(); }, ", "_s), ']');

        EXPECT_EQ(String("[1, 2, 3]"_s), builderContent(builder));
    }

    {
        StringBuilder builder;
        builder.append('[', interleave(values, ", "_s), ']');

        EXPECT_EQ(String("[1, 2, 3]"_s), builderContent(builder));
    }
}

TEST(StringBuilderTest, ToString)
{
    StringBuilder builder;
    builder.append("0123456789"_s);
    String string = builder.toString();
    EXPECT_EQ(String("0123456789"_s), string);
    EXPECT_EQ(string.impl(), builder.toString().impl());

    // Changing the StringBuilder should not affect the original result of toString().
    builder.append("abcdefghijklmnopqrstuvwxyz"_s);
    EXPECT_EQ(String("0123456789"_s), string);

    // Changing the StringBuilder should not affect the original result of toString() in case the capacity is not changed.
    builder.reserveCapacity(200);
    string = builder.toString();
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyz"_s), string);
    builder.append("ABC"_s);
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyz"_s), string);

    // Changing the original result of toString() should not affect the content of the StringBuilder.
    String string1 = builder.toString();
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyzABC"_s), string1);
    string1 = makeStringByReplacingAll(string1, '0', 'a');
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyzABC"_s), builder.toString());
    EXPECT_EQ(String("a123456789abcdefghijklmnopqrstuvwxyzABC"_s), string1);

    // Resizing the StringBuilder should not affect the original result of toString().
    string1 = builder.toString();
    builder.shrink(10);
    builder.append("###"_s);
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyzABC"_s), string1);
}

TEST(StringBuilderTest, ToStringPreserveCapacity)
{
    StringBuilder builder;
    builder.append("0123456789"_s);
    unsigned capacity = builder.capacity();
    String string = builder.toStringPreserveCapacity();
    EXPECT_EQ(capacity, builder.capacity());
    EXPECT_EQ(String("0123456789"_s), string);
    EXPECT_EQ(string.impl(), builder.toStringPreserveCapacity().impl());
    EXPECT_EQ(string.span8().data(), builder.span8().data());

    // Changing the StringBuilder should not affect the original result of toStringPreserveCapacity().
    builder.append("abcdefghijklmnopqrstuvwxyz"_s);
    EXPECT_EQ(String("0123456789"_s), string);

    // Changing the StringBuilder should not affect the original result of toStringPreserveCapacity() in case the capacity is not changed.
    builder.reserveCapacity(200);
    capacity = builder.capacity();
    string = builder.toStringPreserveCapacity();
    EXPECT_EQ(capacity, builder.capacity());
    EXPECT_EQ(string.span8().data(), builder.span8().data());
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyz"_s), string);
    builder.append("ABC"_s);
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyz"_s), string);

    // Changing the original result of toStringPreserveCapacity() should not affect the content of the StringBuilder.
    capacity = builder.capacity();
    String string1 = builder.toStringPreserveCapacity();
    EXPECT_EQ(capacity, builder.capacity());
    EXPECT_EQ(string1.span8().data(), builder.span8().data());
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyzABC"_s), string1);
    string1 = makeStringByReplacingAll(string1, '0', 'a');
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyzABC"_s), builder.toStringPreserveCapacity());
    EXPECT_EQ(String("a123456789abcdefghijklmnopqrstuvwxyzABC"_s), string1);

    // Resizing the StringBuilder should not affect the original result of toStringPreserveCapacity().
    capacity = builder.capacity();
    string1 = builder.toStringPreserveCapacity();
    EXPECT_EQ(capacity, builder.capacity());
    EXPECT_EQ(string.span8().data(), builder.span8().data());
    builder.shrink(10);
    builder.append("###"_s);
    EXPECT_EQ(String("0123456789abcdefghijklmnopqrstuvwxyzABC"_s), string1);
}

TEST(StringBuilderTest, Clear)
{
    StringBuilder builder;
    builder.append("0123456789"_s);
    builder.clear();
    expectEmpty(builder);
}

TEST(StringBuilderTest, Array)
{
    StringBuilder builder;
    builder.append("0123456789"_s);
    EXPECT_EQ('0', static_cast<char>(builder[0]));
    EXPECT_EQ('9', static_cast<char>(builder[9]));
    builder.toString(); // Test after reifyString().
    EXPECT_EQ('0', static_cast<char>(builder[0]));
    EXPECT_EQ('9', static_cast<char>(builder[9]));
}

TEST(StringBuilderTest, Resize)
{
    StringBuilder builder;
    builder.append("0123456789"_s);
    builder.shrink(10);
    EXPECT_EQ(10U, builder.length());
    EXPECT_EQ("0123456789"_s, builderContent(builder));
    builder.shrink(8);
    EXPECT_EQ(8U, builder.length());
    EXPECT_EQ("01234567"_s, builderContent(builder));

    builder.toString();
    builder.shrink(7);
    EXPECT_EQ(7U, builder.length());
    EXPECT_EQ("0123456"_s, builderContent(builder));
    builder.shrink(0);
    expectEmpty(builder);
}

TEST(StringBuilderTest, Equal)
{
    StringBuilder builder1;
    StringBuilder builder2;
    EXPECT_TRUE(builder1 == builder2);
    EXPECT_TRUE(equal(builder1, std::span<const Latin1Character>()));
    EXPECT_TRUE(builder1 == String());
    EXPECT_TRUE(String() == builder1);
    EXPECT_TRUE(builder1 != String("abc"_s));

    builder1.append("123"_s);
    builder1.reserveCapacity(32);
    builder2.append("123"_s);
    builder1.reserveCapacity(64);
    EXPECT_TRUE(builder1 == builder2);
    EXPECT_TRUE(builder1 == String("123"_s));
    EXPECT_TRUE(String("123"_s) == builder1);

    builder2.append("456"_s);
    EXPECT_TRUE(builder1 != builder2);
    EXPECT_TRUE(builder2 != builder1);
    EXPECT_TRUE(String("123"_s) != builder2);
    EXPECT_TRUE(builder2 != String("123"_s));
    builder2.toString(); // Test after reifyString().
    EXPECT_TRUE(builder1 != builder2);

    builder2.shrink(3);
    EXPECT_TRUE(builder1 == builder2);

    builder1.toString(); // Test after reifyString().
    EXPECT_TRUE(builder1 == builder2);
}

TEST(StringBuilderTest, ShouldShrinkToFit)
{
    StringBuilder builder;
    builder.reserveCapacity(256);
    EXPECT_TRUE(builder.shouldShrinkToFit());
    for (int i = 0; i < 256; i++)
        builder.append('x');
    EXPECT_EQ(builder.length(), builder.capacity());
    EXPECT_FALSE(builder.shouldShrinkToFit());
}

#if USE(BUN_JSC_ADDITIONS)

static String repeatedCharacter(Latin1Character character, unsigned length)
{
    std::span<Latin1Character> characters;
    auto string = StringImpl::createUninitialized(length, characters);
    memsetSpan(characters, character);
    return string;
}

TEST(StringBuilderTest, TryAppend)
{
    const char16_t nonLatin1[] = { 0x1234, 0x5678 };

    StringBuilder builder;
    String first { "0123456789"_s };
    EXPECT_TRUE(builder.tryAppend(first));
    // An empty builder retains the string, as append() does.
    EXPECT_EQ(first.impl(), builder.tryToString().impl());
    EXPECT_TRUE(builder.tryAppend("abcd"_s.span8()));
    EXPECT_EQ("0123456789abcd"_s, builderContent(builder));
    EXPECT_TRUE(builder.tryAppend(StringView { "efg"_s }));
    EXPECT_TRUE(builder.tryAppend(String { }));
    EXPECT_TRUE(builder.tryAppend(""_s.span8()));
    EXPECT_EQ("0123456789abcdefg"_s, builderContent(builder));
    EXPECT_TRUE(builder.is8Bit());

    // One Latin-1 character in 16 bits keeps the builder 8-bit.
    const char16_t latin1 = 0xE9;
    EXPECT_TRUE(builder.tryAppend(std::span { &latin1, 1 }));
    EXPECT_TRUE(builder.is8Bit());
    EXPECT_EQ(18U, builder.length());

    EXPECT_TRUE(builder.tryAppend(std::span { nonLatin1 }));
    EXPECT_FALSE(builder.is8Bit());
    EXPECT_TRUE(builder.tryAppend("xyz"_s.span8()));
    EXPECT_TRUE(builder.tryAppend(String { std::span { nonLatin1 } }));

    StringBuilder expected;
    expected.append("0123456789abcdefg"_s, latin1, std::span { nonLatin1 }, "xyz"_s, std::span { nonLatin1 });
    EXPECT_EQ(expected.toString(), builderContent(builder));
    EXPECT_EQ(expected.toString(), builder.tryToString());
    EXPECT_EQ(builder.tryToString().impl(), builder.tryToString().impl());
    EXPECT_FALSE(builder.hasOverflowed());

    // The string that tryToString() returned does not change.
    String string = builder.tryToString();
    EXPECT_TRUE(builder.tryAppend("###"_s.span8()));
    EXPECT_EQ(expected.toString(), string);
    EXPECT_EQ(makeString(expected.toString(), "###"_s), builder.tryToString());

    // The first string is 16-bit.
    StringBuilder builder16;
    EXPECT_TRUE(builder16.tryAppend(String { std::span { nonLatin1 } }));
    EXPECT_TRUE(builder16.tryAppend("ab"_s.span8()));
    EXPECT_FALSE(builder16.is8Bit());
    const char16_t expected16[] = { 0x1234, 0x5678, 'a', 'b' };
    EXPECT_EQ(String { std::span { expected16 } }, builder16.tryToString());
}

TEST(StringBuilderTest, TryAppendWithASharedBuffer)
{
    // A string of the builder shares the buffer only when it is longer than a pointer. A shorter one is a copy.
    StringBuilder builder;
    EXPECT_TRUE(builder.tryReserveCapacity(64));
    EXPECT_EQ(64U, builder.capacity());
    EXPECT_TRUE(builder.tryAppend("0123456789abcdefghijklmnopqrstuvwxyz"_s.span8()));
    const Latin1Character* characters = builder.span8().data();
    String sharesBuffer = builder.toStringPreserveCapacity();
    EXPECT_EQ(characters, sharesBuffer.span8().data());

    // This fits in the buffer. The builder writes after the characters of the string.
    EXPECT_TRUE(builder.tryAppend("ABCDEF"_s.span8()));
    EXPECT_EQ(characters, builder.span8().data());
    EXPECT_EQ("0123456789abcdefghijklmnopqrstuvwxyz"_s, sharesBuffer);
    EXPECT_EQ("0123456789abcdefghijklmnopqrstuvwxyzABCDEF"_s, builderContent(builder));

    // This does not fit. The builder copies its characters to a buffer of its own.
    sharesBuffer = builder.toStringPreserveCapacity();
    EXPECT_EQ(characters, sharesBuffer.span8().data());
    EXPECT_TRUE(builder.tryAppend("GHIJKLMNOPQRSTUVWXYZ0123456789"_s.span8()));
    EXPECT_NE(characters, builder.span8().data());
    EXPECT_EQ(characters, sharesBuffer.span8().data());
    EXPECT_EQ("0123456789abcdefghijklmnopqrstuvwxyzABCDEF"_s, sharesBuffer);
    EXPECT_EQ("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"_s, builder.tryToString());
}

TEST(StringBuilderTest, TryReserveCapacity)
{
    StringBuilder builder;
    EXPECT_TRUE(builder.tryReserveCapacity(0));
    EXPECT_EQ(0U, builder.capacity());
    EXPECT_TRUE(builder.tryReserveCapacity(100));
    EXPECT_EQ(100U, builder.capacity());
    EXPECT_TRUE(builder.tryAppend("xyz"_s.span8()));
    const Latin1Character* characters = builder.span8().data();
    EXPECT_TRUE(builder.tryAppend("0123456789"_s.span8()));
    EXPECT_EQ(characters, builder.span8().data());
    EXPECT_TRUE(builder.tryReserveCapacity(50));
    EXPECT_EQ(100U, builder.capacity());
    EXPECT_TRUE(builder.tryReserveCapacity(200));
    EXPECT_EQ(200U, builder.capacity());
    EXPECT_EQ("xyz0123456789"_s, builderContent(builder));

    // The builder retains a 16-bit string and has no buffer.
    const char16_t nonLatin1[] = { 0x1234, 0x5678 };
    StringBuilder builder16;
    EXPECT_TRUE(builder16.tryAppend(String { std::span { nonLatin1 } }));
    EXPECT_TRUE(builder16.tryReserveCapacity(64));
    EXPECT_EQ(64U, builder16.capacity());
    EXPECT_FALSE(builder16.is8Bit());
    EXPECT_EQ(String { std::span { nonLatin1 } }, builderContent(builder16));

    // No string is this long.
    EXPECT_FALSE(builder.tryReserveCapacity(String::MaxLength + 1U));
    EXPECT_FALSE(builder16.tryReserveCapacity(StringImpl::maxValidLength<char16_t>() + 1U));
    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_FALSE(builder16.hasOverflowed());
    EXPECT_EQ(200U, builder.capacity());
    EXPECT_EQ(64U, builder16.capacity());
    EXPECT_EQ("xyz0123456789"_s, builder.tryToString());
    EXPECT_EQ(String { std::span { nonLatin1 } }, builder16.tryToString());
}

TEST(StringBuilderTest, TryToString)
{
    StringBuilder builder;
    EXPECT_FALSE(builder.tryToString().isNull());
    EXPECT_TRUE(builder.tryToString().isEmpty());

    // An empty builder that has a buffer.
    EXPECT_TRUE(builder.tryReserveCapacity(64));
    EXPECT_FALSE(builder.tryToString().isNull());
    EXPECT_TRUE(builder.tryToString().isEmpty());
    EXPECT_TRUE(builder.tryAppend("abc"_s.span8()));
    EXPECT_EQ("abc"_s, builder.tryToString());

    // The buffer is more than 125% of the characters, so the string gets a buffer of its own size.
    StringBuilder shrinks;
    EXPECT_TRUE(shrinks.tryReserveCapacity(256));
    EXPECT_TRUE(shrinks.tryAppend("0123456789"_s.span8()));
    EXPECT_TRUE(shrinks.shouldShrinkToFit());
    String string = shrinks.tryToString();
    EXPECT_EQ("0123456789"_s, string);
    EXPECT_EQ(10U, shrinks.capacity());
    EXPECT_FALSE(string.impl()->bufferOwnership() == StringImpl::BufferSubstring);
    EXPECT_TRUE(shrinks.tryAppend("abc"_s.span8()));
    EXPECT_EQ("0123456789"_s, string);
    EXPECT_EQ("0123456789abc"_s, shrinks.tryToString());
}

TEST(StringBuilderTest, TryMethodsAfterAnOverflow)
{
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    builder.append("abc"_s);
    builder.didOverflow();
    ASSERT_TRUE(builder.hasOverflowed());
    EXPECT_FALSE(builder.tryAppend("d"_s.span8()));
    EXPECT_FALSE(builder.tryAppend(String { "d"_s }));
    const char16_t nonLatin1 = 0x1234;
    EXPECT_FALSE(builder.tryAppend(std::span { &nonLatin1, 1 }));
    EXPECT_FALSE(builder.tryReserveCapacity(16));
    EXPECT_TRUE(builder.tryToString().isNull());
    EXPECT_TRUE(builder.hasOverflowed());
}

// The builder of this test has the default policy: an overflow that it records is a crash.
TEST(StringBuilderTest, TryAppendRefusesALengthAboveMaxLength)
{
    // Nobody writes or reads the characters of these strings, so the test touches almost none of their memory.
    constexpr unsigned halfLength = String::MaxLength / 2 + 1;
    std::span<Latin1Character> characters8;
    std::span<char16_t> characters16;
    auto half8 = StringImpl::tryCreateUninitialized(halfLength, characters8);
    auto half16 = StringImpl::tryCreateUninitialized(halfLength, characters16);
    if (!half8 || !half16) {
        dataLogLn("Cannot allocate the strings for this test, so the test did not run.");
        return;
    }

    StringBuilder builder8;
    EXPECT_TRUE(builder8.tryAppend(String { *half8 }));
    EXPECT_FALSE(builder8.tryAppend(String { *half8 }));
    EXPECT_FALSE(builder8.tryAppend(String { *half16 }));
    EXPECT_FALSE(builder8.tryReserveCapacity(String::MaxLength + 1U));
    EXPECT_FALSE(builder8.hasOverflowed());
    EXPECT_EQ(halfLength, builder8.length());
    EXPECT_EQ(half8.get(), builder8.tryToString().impl());

    StringBuilder builder16;
    EXPECT_TRUE(builder16.tryAppend(String { *half16 }));
    EXPECT_FALSE(builder16.tryAppend(String { *half16 }));
    EXPECT_FALSE(builder16.tryAppend(String { *half8 }));
    EXPECT_FALSE(builder16.hasOverflowed());
    EXPECT_EQ(halfLength, builder16.length());
    EXPECT_EQ(half16.get(), builder16.tryToString().impl());
}

// Twice the capacity of this builder is more than the longest 16-bit string. append() asks for that much and overflows.
TEST(StringBuilderTest, TryAppendUpconvertsAboveHalfOfTheLongest16BitString)
{
    constexpr unsigned maxLength16Bit = StringImpl::maxValidLength<char16_t>();
    static_assert(maxLength16Bit < String::MaxLength);
    static_assert(StringImpl::isValidLength<char16_t>(maxLength16Bit) && !StringImpl::isValidLength<char16_t>(static_cast<size_t>(maxLength16Bit) + 1));
    static_assert(StringImpl::maxValidLength<Latin1Character>() == String::MaxLength);
    constexpr unsigned capacity = maxLength16Bit / 2 + 1;
    const char16_t nonLatin1 = 0x1234;

    // The test reserves up to 5GB of address space and writes to almost none of it. On a machine that cannot
    // reserve that much, the builder refuses and keeps what it holds.
    StringBuilder builder;
    if (!builder.tryReserveCapacity(capacity)) {
        dataLogLn("Cannot allocate the buffer for this test, so the test did not run.");
        return;
    }
    EXPECT_TRUE(builder.tryAppend("ab"_s.span8()));
    EXPECT_EQ(capacity, builder.capacity());
    if (builder.tryAppend(std::span { &nonLatin1, 1 })) {
        EXPECT_FALSE(builder.is8Bit());
        EXPECT_EQ(3U, builder.length());
        EXPECT_GE(builder.capacity(), capacity);
        EXPECT_LE(builder.capacity(), maxLength16Bit);
        EXPECT_EQ(nonLatin1, builder[2]);
    } else {
        dataLogLn("Cannot allocate the 16-bit buffer for this test.");
        EXPECT_TRUE(builder.is8Bit());
        EXPECT_EQ(2U, builder.length());
        EXPECT_EQ(capacity, builder.capacity());
    }
    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_EQ('a', static_cast<char>(builder[0]));
    EXPECT_EQ('b', static_cast<char>(builder[1]));
}

#if !defined(NDEBUG)

// While one of these exists, an allocation of more bytes than this fails. Only debug builds have the limit.
class MaxSingleAllocationSize {
public:
    explicit MaxSingleAllocationSize(size_t size) { fastSetMaxSingleAllocationSize(size); }
    ~MaxSingleAllocationSize() { fastSetMaxSingleAllocationSize(std::numeric_limits<size_t>::max()); }
};

static constexpr unsigned megabyte = 1024 * 1024;

TEST(StringBuilderTest, TryAppendKeepsTheCharactersWhenTheBufferCannotGrow)
{
    String megabyteOfA = repeatedCharacter('a', megabyte);
    String megabyteOfB = repeatedCharacter('b', megabyte);
    String megabyteOfC = repeatedCharacter('c', megabyte);
    String expected = makeString(megabyteOfA, megabyteOfB, megabyteOfC);

    StringBuilder builder;
    MaxSingleAllocationSize limit { 4 * megabyte };
    EXPECT_TRUE(builder.tryAppend(megabyteOfA));
    EXPECT_TRUE(builder.tryAppend(megabyteOfB));
    EXPECT_EQ(2 * megabyte, builder.capacity());
    // Twice the capacity plus the header is more than the limit. The required length fits.
    EXPECT_TRUE(builder.tryAppend(megabyteOfC));
    EXPECT_EQ(3 * megabyte, builder.length());
    EXPECT_EQ(3 * megabyte, builder.capacity());

    // The builder is the only owner of its buffer, and no buffer for 4MB of characters fits.
    const Latin1Character* characters = builder.span8().data();
    EXPECT_FALSE(builder.tryAppend(megabyteOfA));
    EXPECT_FALSE(builder.tryReserveCapacity(4 * megabyte));
    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_EQ(3 * megabyte, builder.length());
    EXPECT_EQ(3 * megabyte, builder.capacity());
    EXPECT_EQ(characters, builder.span8().data());
    EXPECT_TRUE(expected == builderContent(builder));

    // A 16-bit buffer for these characters is 6MB.
    const char16_t nonLatin1 = 0x1234;
    EXPECT_FALSE(builder.tryAppend(std::span { &nonLatin1, 1 }));
    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_TRUE(builder.is8Bit());
    EXPECT_TRUE(expected == builderContent(builder));

    // The builder is not the only owner of its buffer.
    String sharesBuffer = builder.toStringPreserveCapacity();
    EXPECT_FALSE(builder.tryAppend(megabyteOfA));
    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_EQ(3 * megabyte, builder.length());
    EXPECT_EQ(sharesBuffer.span8().data(), builder.span8().data());
    EXPECT_TRUE(expected == sharesBuffer);
    sharesBuffer = { };

    // A shorter string still fits.
    EXPECT_TRUE(builder.tryAppend("0123456789"_s.span8()));
    EXPECT_EQ(3 * megabyte + 10, builder.length());
    String string = builder.tryToString();
    EXPECT_TRUE(makeString(expected, "0123456789"_s) == string);
}

TEST(StringBuilderTest, TryAppendKeepsTheStringItRetainsWhenItCannotAllocate)
{
    String megabyteOfA = repeatedCharacter('a', megabyte);

    StringBuilder builder;
    MaxSingleAllocationSize limit { megabyte };
    EXPECT_TRUE(builder.tryAppend(megabyteOfA));
    // The first buffer of the builder is for 2MB of characters.
    EXPECT_FALSE(builder.tryAppend(megabyteOfA));
    EXPECT_FALSE(builder.tryAppend("b"_s.span8()));
    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_EQ(megabyte, builder.length());
    EXPECT_EQ(megabyteOfA.impl(), builder.tryToString().impl());
}

TEST(StringBuilderTest, TryToStringKeepsTheBufferThatCannotShrink)
{
    constexpr unsigned capacity = megabyte;
    constexpr unsigned length = megabyte / 2;
    String halfMegabyteOfA = repeatedCharacter('a', length);

    StringBuilder builder;
    EXPECT_TRUE(builder.tryReserveCapacity(capacity));
    EXPECT_TRUE(builder.tryAppend(halfMegabyteOfA));
    EXPECT_TRUE(builder.shouldShrinkToFit());
    const Latin1Character* characters = builder.span8().data();

    String string;
    {
        MaxSingleAllocationSize limit { 1024 };
        string = builder.tryToString();
    }
    ASSERT_FALSE(string.isNull());
    EXPECT_FALSE(builder.hasOverflowed());
    // The string is a part of the buffer.
    EXPECT_EQ(characters, string.span8().data());
    EXPECT_EQ(length, string.length());
    EXPECT_TRUE(halfMegabyteOfA == string);
    EXPECT_EQ(capacity, builder.capacity());
    EXPECT_EQ(string.impl(), builder.tryToString().impl());

    EXPECT_TRUE(builder.tryAppend("0123456789"_s.span8()));
    EXPECT_TRUE(halfMegabyteOfA == string);
    EXPECT_TRUE(makeString(halfMegabyteOfA, "0123456789"_s) == builder.tryToString());
}

#endif // !defined(NDEBUG)

#endif // USE(BUN_JSC_ADDITIONS)

TEST(StringBuilderTest, ToAtomString)
{
    StringBuilder builder;
    builder.append("123"_s);
    AtomString atomString = builder.toAtomString();
    EXPECT_EQ(String("123"_s), atomString);

    builder.reserveCapacity(256);
    EXPECT_TRUE(builder.shouldShrinkToFit());
    for (int i = builder.length(); i < 128; i++)
        builder.append('x');
    AtomString atomString1 = builder.toAtomString();
    EXPECT_EQ(128u, atomString1.length());
    EXPECT_EQ('x', atomString1[127]);

    // Later change of builder should not affect the atomic string.
    for (int i = builder.length(); i < 256; i++)
        builder.append('x');
    EXPECT_EQ(128u, atomString1.length());

    EXPECT_FALSE(builder.shouldShrinkToFit());
    String string = builder.toString();
    AtomString atomString2 = builder.toAtomString();
    // They should share the same StringImpl.
    EXPECT_EQ(atomString2.impl(), string.impl());
}

TEST(StringBuilderTest, ToAtomStringOnEmpty)
{
    { // Default constructed.
        StringBuilder builder;
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
    { // With capacity.
        StringBuilder builder;
        builder.reserveCapacity(64);
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
    { // AtomString constructed from a null string.
        StringBuilder builder;
        builder.append(String());
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
    { // AtomString constructed from an empty string.
        StringBuilder builder;
        builder.append(emptyString());
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
    { // AtomString constructed from an empty StringBuilder.
        StringBuilder builder;
        StringBuilder emptyBuilder;
        builder.append(emptyBuilder);
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
    { // AtomString constructed from an empty char* string.
        StringBuilder builder;
        builder.append(""_span);
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
    { // Cleared StringBuilder.
        StringBuilder builder;
        builder.append("WebKit"_s);
        builder.clear();
        AtomString atomString = builder.toAtomString();
        EXPECT_EQ(emptyAtom(), atomString);
    }
}

} // namespace
