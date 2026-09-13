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

// A run of 16-bit characters that a StringBuilder makes room for and that nobody writes.
// A test can make a builder need a very long buffer with it and touch none of the memory.
struct UnwrittenCharacters {
    unsigned length;
};

} // namespace TestWebKitAPI

namespace WTF {

template<> class StringTypeAdapter<TestWebKitAPI::UnwrittenCharacters> {
public:
    StringTypeAdapter(TestWebKitAPI::UnwrittenCharacters characters)
        : m_length { characters.length }
    {
    }

    unsigned length() const { return m_length; }
    bool is8Bit() const { return false; }
    template<typename CharacterType> void writeTo(std::span<CharacterType>) const { }

private:
    unsigned m_length;
};

} // namespace WTF

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

// The longest 16-bit string is shorter than String::MaxLength, because the size of a StringImpl has to fit in an unsigned.
static constexpr unsigned maxLength16Bit = StringImpl::maxValidLength<char16_t>();
static_assert(maxLength16Bit < String::MaxLength);
static_assert(StringImpl::isValidLength<char16_t>(maxLength16Bit) && !StringImpl::isValidLength<char16_t>(static_cast<size_t>(maxLength16Bit) + 1));
static_assert(StringImpl::maxValidLength<Latin1Character>() == String::MaxLength);

// Twice this capacity is more than the longest 16-bit string.
static constexpr unsigned capacityAboveHalfOfMaxLength16Bit = maxLength16Bit / 2 + 1;
static_assert(capacityAboveHalfOfMaxLength16Bit * 2 > maxLength16Bit);

// The tests that call this reserve up to 6GB of address space and write to almost none of it. They return early
// on a machine that cannot reserve that much. They do not call GTEST_SKIP(), because the TestWebKitAPI listener
// reports a skipped test as a failure.
template<typename FirstCharacterType, typename SecondCharacterType>
static bool canAllocateBothBuffers(unsigned firstLength, unsigned secondLength)
{
    std::span<FirstCharacterType> firstCharacters;
    std::span<SecondCharacterType> secondCharacters;
    auto firstBuffer = StringImpl::tryCreateUninitialized(firstLength, firstCharacters);
    auto secondBuffer = StringImpl::tryCreateUninitialized(secondLength, secondCharacters);
    if (firstBuffer && secondBuffer)
        return true;
    dataLogLn("Cannot allocate the buffers for this test, so the test did not run.");
    return false;
}

TEST(StringBuilderTest, UpconvertCapacityAboveHalfOfMaxLength)
{
    constexpr unsigned capacity8Bit = capacityAboveHalfOfMaxLength16Bit;
    if (!canAllocateBothBuffers<Latin1Character, char16_t>(capacity8Bit, maxLength16Bit))
        return;

    StringBuilder builder(OverflowPolicy::RecordOverflow);
    builder.reserveCapacity(capacity8Bit);
    builder.append("ab"_s);
    ASSERT_FALSE(builder.hasOverflowed());
    EXPECT_TRUE(builder.is8Bit());
    EXPECT_EQ(capacity8Bit, builder.capacity());

    const char16_t nonLatin1 = 0x1234;
    builder.append(nonLatin1);
    ASSERT_FALSE(builder.hasOverflowed());
    EXPECT_FALSE(builder.is8Bit());
    EXPECT_GE(builder.capacity(), capacity8Bit);
    EXPECT_LE(builder.capacity(), maxLength16Bit);
    EXPECT_EQ(3U, builder.length());
    EXPECT_EQ('a', static_cast<char>(builder[0]));
    EXPECT_EQ('b', static_cast<char>(builder[1]));
    EXPECT_EQ(nonLatin1, builder[2]);
}

TEST(StringBuilderTest, Grow16BitCapacityAboveHalfOfMaxLength)
{
    constexpr unsigned capacity = capacityAboveHalfOfMaxLength16Bit;
    if (!canAllocateBothBuffers<char16_t, char16_t>(capacity, maxLength16Bit))
        return;

    const char16_t nonLatin1 = 0x1234;
    constexpr unsigned writtenLength = 16;

    StringBuilder builder(OverflowPolicy::RecordOverflow);
    for (unsigned i = 0; i < writtenLength; ++i)
        builder.append(nonLatin1);
    builder.reserveCapacity(capacity);
    ASSERT_FALSE(builder.hasOverflowed());
    EXPECT_FALSE(builder.is8Bit());
    EXPECT_EQ(capacity, builder.capacity());

    // This string shares the buffer with the builder. A builder that is not the only owner of its buffer copies
    // its characters to a new buffer. A builder that is the only owner reallocates, and that can copy all 2GB.
    String sharesBuffer = builder.toStringPreserveCapacity();

    builder.append(UnwrittenCharacters { capacity - writtenLength + 1 });
    ASSERT_FALSE(builder.hasOverflowed());
    EXPECT_EQ(capacity + 1, builder.length());
    EXPECT_GE(builder.capacity(), builder.length());
    EXPECT_LE(builder.capacity(), maxLength16Bit);
    EXPECT_EQ(nonLatin1, builder[0]);
    EXPECT_EQ(nonLatin1, builder[writtenLength - 1]);
    EXPECT_EQ(writtenLength, sharesBuffer.length());
    EXPECT_EQ(nonLatin1, sharesBuffer[0]);
    EXPECT_EQ(nonLatin1, sharesBuffer[writtenLength - 1]);
}

TEST(StringBuilderTest, LengthAboveMaxLength16BitOverflows)
{
    const char16_t nonLatin1 = 0x1234;
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    builder.append(nonLatin1);
    ASSERT_FALSE(builder.hasOverflowed());
    EXPECT_FALSE(builder.is8Bit());

    // The longest 16-bit string plus one character. The builder has to refuse this length, whatever capacity it picks.
    builder.append(UnwrittenCharacters { maxLength16Bit });
    EXPECT_TRUE(builder.hasOverflowed());
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

} // namespace
