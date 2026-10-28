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
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilderJSON.h>
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

#if !defined(NDEBUG)
// toString() gives a buffer with much spare capacity the size of its characters. The characters are all
// there when the allocator refuses that, so it is neither an overflow nor a crash.
template<typename CharacterType> static void expectToStringWithBufferThatCannotShrink(OverflowPolicy policy, bool shareBuffer)
{
    constexpr unsigned length = 4096;
    Vector<CharacterType> characters(FillWith { }, length, 'x');
    String expected { characters.span() };

    StringBuilder builder(policy);
    builder.append(characters.span().first(length - 1));
    builder.reserveCapacity(8 * length);
    // A buffer that a string is a part of has another owner. It is not reallocated but copied into a new one.
    String partOfBuffer = shareBuffer ? builder.toStringPreserveCapacity() : String();
    builder.append(characters.last());
    EXPECT_TRUE(builder.shouldShrinkToFit());

    fastSetMaxSingleAllocationSize(length / 2);
    String string = builder.toString();
    fastSetMaxSingleAllocationSize(std::numeric_limits<size_t>::max());

    EXPECT_FALSE(builder.hasOverflowed());
    EXPECT_EQ(length, builder.length());
    EXPECT_EQ(expected, string);
    EXPECT_EQ(sizeof(CharacterType) == 1, string.is8Bit());
    EXPECT_EQ(string.impl(), builder.toString().impl());

    // The string is a part of the buffer, which the builder goes on to use.
    builder.append("yz"_s);
    EXPECT_EQ(expected, string);
    EXPECT_EQ(makeString(expected, "yz"_s), builder.toString());
}

TEST(StringBuilderTest, ToStringWithBufferThatCannotShrink)
{
    for (auto policy : { OverflowPolicy::CrashOnOverflow, OverflowPolicy::RecordOverflow }) {
        for (bool shareBuffer : { false, true }) {
            SCOPED_TRACE(::testing::Message() << "records overflow " << (policy == OverflowPolicy::RecordOverflow) << ", shared buffer " << shareBuffer);
            expectToStringWithBufferThatCannotShrink<Latin1Character>(policy, shareBuffer);
            expectToStringWithBufferThatCannotShrink<char16_t>(policy, shareBuffer);
        }
    }
}
#endif

TEST(StringBuilderTest, AppendAfterOverflow)
{
    constexpr std::array<char16_t, 2> wide { 0x3042, 0x3044 };
    for (bool hasBuffer : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "has a buffer " << hasBuffer);
        StringBuilder builder(OverflowPolicy::RecordOverflow);
        if (hasBuffer)
            builder.append("abc"_s);
        builder.didOverflow();

        builder.append('x');
        builder.append(wide[0]);
        builder.append("abc"_s);
        builder.append(std::span { wide });
        builder.append("abc"_s, 'x', 1);
        builder.append(StringView { std::span { wide } }, 'x');
        builder.append(String { "abc"_s });
        builder.appendQuotedJSONString("abc"_s);
        builder.reserveCapacity(64);
        builder.shrink(1);
        EXPECT_TRUE(builder.hasOverflowed());
        EXPECT_FALSE(builder.shouldShrinkToFit());

        builder.clear();
        EXPECT_FALSE(builder.hasOverflowed());
        builder.append(StringView { std::span { wide } }, 'x');
        EXPECT_EQ(3U, builder.length());
    }
}

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

static std::optional<char> shortJSONEscape(char16_t codeUnit)
{
    switch (codeUnit) {
    case '"':
        return '"';
    case '\\':
        return '\\';
    case '\b':
        return 'b';
    case '\t':
        return 't';
    case '\n':
        return 'n';
    case '\f':
        return 'f';
    case '\r':
        return 'r';
    default:
        return std::nullopt;
    }
}

template<typename OutputCharacterType, typename InputCharacterType>
static Vector<OutputCharacterType> escapeJSONScalar(std::span<const InputCharacterType> input)
{
    Vector<OutputCharacterType> result;
    auto append = [&](char16_t codeUnit) {
        result.append(static_cast<OutputCharacterType>(codeUnit));
    };
    auto appendHexEscape = [&](char16_t codeUnit) {
        append('\\');
        append('u');
        for (int shift = 12; shift >= 0; shift -= 4) {
            unsigned nibble = (codeUnit >> shift) & 0xf;
            append(static_cast<char16_t>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
        }
    };
    for (size_t i = 0; i < input.size(); ++i) {
        char16_t codeUnit = input[i];
        if (auto shortForm = shortJSONEscape(codeUnit)) {
            append('\\');
            append(*shortForm);
        } else if (codeUnit < ' ')
            appendHexEscape(codeUnit);
        else if (U16_IS_LEAD(codeUnit) && i + 1 < input.size() && U16_IS_TRAIL(input[i + 1])) {
            append(codeUnit);
            append(input[++i]);
        } else if (U16_IS_SURROGATE(codeUnit))
            appendHexEscape(codeUnit);
        else
            append(codeUnit);
    }
    return result;
}

template<typename OutputCharacterType, typename InputCharacterType>
static std::optional<Vector<OutputCharacterType>> escapeIntoGuardedBuffer(std::span<const InputCharacterType> input)
{
    constexpr size_t guardSize = 64;
    constexpr OutputCharacterType guardValue { 0x5a };
    size_t outputSize = 6 * input.size();
    Vector<OutputCharacterType> buffer(FillWith { }, outputSize + guardSize, guardValue);
    auto output = buffer.mutableSpan().first(outputSize);
    bool succeeded = WTF::appendEscapedJSONStringContent(output, input);
    bool isGuardIntact = std::ranges::all_of(buffer.span().subspan(outputSize), [&](auto element) {
        return element == guardValue;
    });
    EXPECT_TRUE(isGuardIntact);
    if (!succeeded)
        return std::nullopt;
    buffer.shrink(outputSize - output.size());
    return buffer;
}

template<typename OutputCharacterType, typename InputCharacterType>
static void expectEscapedWithinBuffer(std::span<const InputCharacterType> input)
{
    auto escaped = escapeIntoGuardedBuffer<OutputCharacterType>(input);
    ASSERT_TRUE(escaped.has_value());
    auto expected = escapeJSONScalar<OutputCharacterType>(input);
    EXPECT_EQ(expected.size(), escaped->size());
    EXPECT_TRUE(equalSpans(expected.span(), escaped->span()));
}

static void expectEscapedAllWidths(std::span<const Latin1Character> input)
{
    auto wideInput = Vector<char16_t>::map(input, [](Latin1Character character) -> char16_t {
        return character.value;
    });
    expectEscapedWithinBuffer<Latin1Character>(input);
    expectEscapedWithinBuffer<char16_t>(input);
    expectEscapedWithinBuffer<Latin1Character>(wideInput.span());
    expectEscapedWithinBuffer<char16_t>(wideInput.span());
}

TEST(StringBuilderTest, JSONEscapeFillsWholeBuffer)
{
    for (size_t length : std::initializer_list<size_t> { 0, 1, 15, 16, 17, 31, 32, 33, 511, 512, 513, 1040 }) {
        SCOPED_TRACE(::testing::Message() << "length " << length);
        Vector<Latin1Character> input(FillWith { }, length, 0x01);
        expectEscapedAllWidths(input.span());
    }
}

TEST(StringBuilderTest, JSONEscapeLastVectorGroup)
{
    constexpr size_t groupSize = 16;
    for (size_t length = groupSize; length <= 3 * groupSize; ++length) {
        SCOPED_TRACE(::testing::Message() << "length " << length);
        Vector<Latin1Character> input(FillWith { }, length, 'a');
        expectEscapedAllWidths(input.span());

        auto lastGroup = input.mutableSpan().last(groupSize);
        for (auto& character : lastGroup) {
            character = 0x1f;
            expectEscapedAllWidths(input.span());
            character = 'a';
        }

        std::ranges::fill(lastGroup, Latin1Character { '"' });
        expectEscapedAllWidths(input.span());
    }
}

TEST(StringBuilderTest, JSONEscapeMixedContent)
{
    constexpr std::array<Latin1Character, 7> alphabet { 'a', '"', '\\', '\n', 0x01, 0x7f, 0xe9 };
    constexpr std::array<unsigned, 4> sparsities { 1, 4, 64, 1024 };
    constexpr size_t stringCount = 200;
    constexpr size_t maxLength = 600;
    WeakRandom random(0x9e3779b9);
    for (size_t index = 0; index < stringCount; ++index) {
        SCOPED_TRACE(::testing::Message() << "string " << index);
        unsigned sparsity = sparsities[index % sparsities.size()];
        Vector<Latin1Character> input(index * maxLength / (stringCount - 1), [&](size_t) -> Latin1Character {
            if (random.getUint32(sparsity))
                return 'a';
            return alphabet[random.getUint32(alphabet.size())];
        });
        expectEscapedAllWidths(input.span());
    }
}

TEST(StringBuilderTest, JSONEscapeSurrogates)
{
    constexpr char16_t lead = 0xd83d;
    constexpr char16_t trail = 0xde00;
    constexpr std::array<std::array<char16_t, 2>, 6> patterns { {
        { lead, trail },
        { lead, u'a' },
        { trail, u'a' },
        { trail, lead },
        { lead, lead },
        { trail, trail },
    } };
    for (size_t length = 1; length <= 24; ++length) {
        for (size_t index = 0; index < length; ++index) {
            SCOPED_TRACE(::testing::Message() << "length " << length << ", index " << index);
            for (auto& pattern : patterns) {
                Vector<char16_t> input(FillWith { }, length, u'a');
                input[index] = pattern[0];
                if (index + 1 < length)
                    input[index + 1] = pattern[1];
                expectEscapedWithinBuffer<char16_t>(input.span());
            }
        }
    }
}

TEST(StringBuilderTest, JSONEscapeWideInputFails)
{
    for (size_t length = 1; length <= 24; ++length) {
        for (size_t index = 0; index < length; ++index) {
            SCOPED_TRACE(::testing::Message() << "length " << length << ", index " << index);
            Vector<char16_t> input(FillWith { }, length, u'a');
            input[index] = 0x100;
            EXPECT_FALSE(escapeIntoGuardedBuffer<Latin1Character>(input.span()).has_value());
        }
    }
}

TEST(StringBuilderDeathTest, JSONEscapeOutputTooSmall)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    Vector<Latin1Character> input(FillWith { }, 16, 'a');
    Vector<Latin1Character> buffer(FillWith { }, 6 * input.size(), 'b');
    auto output = buffer.mutableSpan().first(buffer.size() - 1);
    ASSERT_DEATH_IF_SUPPORTED(WTF::appendEscapedJSONStringContent(output, input.span()), "");
}

} // namespace
