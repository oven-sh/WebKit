/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTNameClass.h"
#include "AOTNativeTest.h"
#include "AOTStringTable.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

std::span<const uint8_t> bytesOf(ASCIILiteral text)
{
    return byteCast<uint8_t>(text.span());
}

std::array<uint8_t, 5> distinctText(unsigned index)
{
    auto letterAt = [&](unsigned shift) { return static_cast<uint8_t>('a' + ((index >> shift) & 15)); };
    return { '~', letterAt(12), letterAt(8), letterAt(4), letterAt(0) };
}

StringAsRead asRead(std::span<const uint8_t> bytes)
{
    return { bytes, classifyName(bytes), BuiltinTable::singleton().nameOf(bytes) };
}

StringAsRead asRead(ASCIILiteral text)
{
    return asRead(bytesOf(text));
}

struct TestStrings {
    explicit TestStrings(Vector<StringAsRead>&& stringsAsRead = { }, unsigned width = 1)
        : loops(width)
    {
        strings.initialize(BuiltinTable::singleton(), loops, WTF::move(stringsAsRead));
    }

    ParallelLoops loops;
    StringTable strings;
};

} // anonymous namespace

static void stringsRejectOverlongAndSplitPairs()
{
    const Vector<uint8_t> cases[] = {
        { 0xc0, 0x80 },
        { 0xed, 0xa0, 0x80, 0xed, 0xb0, 0x80 },
        { 0xe2, 0x82 },
        { 0xf4, 0x90, 0x80, 0x80 },
        { 0x80 },
    };
    for (const Vector<uint8_t>& bytes : cases) {
        AOT_TEST_CHECK(!isCanonicalProgramString(bytes.span()));
        AOT_TEST_CHECK(encodeProgramString(decodeProgramString(bytes.span()).span()) != bytes);
    }
}

static void stringTableInternJoinsSurrogates()
{
    TestStrings test;
    StringTable& strings = test.strings;
    constexpr std::array<uint8_t, 6> splitPair { 0xed, 0xa0, 0xbd, 0xed, 0xb8, 0x80 };
    constexpr std::array<uint8_t, 4> codePoint { 0xf0, 0x9f, 0x98, 0x80 };
    StringID id = strings.intern(splitPair);
    AOT_TEST_CHECK(id == strings.intern(codePoint) && std::ranges::equal(strings[id], codePoint));
    AOT_TEST_CHECK(id != strings.intern(std::span(splitPair).first(3)));
}

static void stringTableHoldsEveryCodeUnit()
{
    constexpr std::array<uint8_t, 5> textAsRead { '~', 0xf0, 0x9f, 0x98, 0x80 };
    constexpr std::array<uint8_t, 3> leadSurrogate { 0xed, 0xa0, 0xbd };
    constexpr std::array<uint8_t, 3> trailSurrogate { 0xed, 0xb8, 0x80 };
    TestStrings test({ asRead(textAsRead) });
    StringTable& strings = test.strings;
    StringID tilde = strings.find(bytesOf("~"_s));
    AOT_TEST_CHECK(tilde && strings.find(leadSurrogate) && strings.find(trailSurrogate));
    AOT_TEST_CHECK(strings.codeUnitAt(StringID(0u), 0) == tilde && strings.codeUnitAt(tilde, 0) == tilde);
    AOT_TEST_CHECK(strings.codeUnitAt(StringID(0u), 1) == strings.find(leadSurrogate));
    AOT_TEST_CHECK(strings.codeUnitAt(StringID(0u), 2) == strings.find(trailSurrogate));
    AOT_TEST_CHECK(!strings.codeUnitAt(StringID(0u), 3) && !strings.codeUnitAt(tilde, 1) && !strings.codeUnitAt(strings.emptyString(), 0));

    constexpr std::array<uint8_t, 4> internedText { 0xe2, 0x98, 0x83, '~' };
    std::span<const uint8_t> snowman = std::span(internedText).first(3);
    AOT_TEST_CHECK(!strings.find(snowman));
    size_t size = strings.size();
    StringID interned = strings.intern(internedText);
    AOT_TEST_CHECK(strings.size() == size + 2);
    AOT_TEST_CHECK(strings.find(snowman) && strings.codeUnitAt(interned, 0) == strings.find(snowman));
    AOT_TEST_CHECK(strings.codeUnitAt(interned, 1) == tilde && !strings.codeUnitAt(interned, 2));
}

static void stringTableGrowsShards()
{
    constexpr unsigned count = 10000;
    TestStrings test;
    StringTable& strings = test.strings;
    strings.intern(bytesOf("~abcdefghijklmnop"_s));
    size_t size = strings.size();
    Vector<StringID> ids;
    for (unsigned index = 0; index < count; ++index)
        ids.append(strings.intern(distinctText(index)));
    AOT_TEST_CHECK(strings.size() == size + count);
    for (unsigned index = 0; index < count; ++index)
        AOT_TEST_CHECK(strings.intern(distinctText(index)) == ids[index]);
    AOT_TEST_CHECK(strings.size() == size + count);
}

static void stringTableInternsKeptTextsAsAnyOther()
{
    constexpr std::array<uint8_t, 28> run { '~', 'q', 'r', 'n', 'u', 'l', 'l', 's', 't', 'q', 'u', 'v', 'w', 'x', 'y', 'z', 0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80, 0xed, 0xa0, 0xbd };
    constexpr std::array<size_t, 12> ends { 1, 2, 3, 7, 8, 9, 16, 18, 21, 25, 28, 15 };
    Vector<size_t> ascending(std::span<const size_t> { ends });
    std::ranges::sort(ascending);
    Vector<size_t> descending = ascending;
    std::ranges::reverse(descending);
    for (std::span<const size_t> order : { ascending.span(), descending.span(), std::span<const size_t> { ends } }) {
        TestStrings copied;
        TestStrings kept;
        std::span<const uint8_t> bytes = kept.strings.keep(run);
        AOT_TEST_CHECK(bytes.data() != run.data() && std::ranges::equal(bytes, run));
        StringTable::FrontsOfKeptTexts fronts;
        for (size_t end : order) {
            StringID expected = copied.strings.intern(std::span<const uint8_t> { run }.first(end));
            StringID id = kept.strings.internKept(bytes.first(end), fronts);
            AOT_TEST_CHECK(id == expected && kept.strings.size() == copied.strings.size());
            AOT_TEST_CHECK((end == 1 || kept.strings[id].data() == bytes.data()) && kept.strings[id].size() == end);
            AOT_TEST_CHECK(kept.strings.internKept(bytes.first(end), fronts) == id && kept.strings.find(std::span<const uint8_t> { run }.first(end)) == id && kept.strings.intern(std::span<const uint8_t> { run }.first(end)) == id);
            AOT_TEST_CHECK(kept.strings.nameClass(id) == copied.strings.nameClass(expected));
        }
        for (size_t index = 0; index < kept.strings.size(); ++index)
            AOT_TEST_CHECK(std::ranges::equal(kept.strings[StringID(index)], copied.strings[StringID(index)]));
        StringID longest = kept.strings.find(run);
        size_t numberOfCodeUnits = decodeProgramString(run).size();
        for (size_t position = 0; position < numberOfCodeUnits; ++position)
            AOT_TEST_CHECK(kept.strings.codeUnitAt(longest, position));
        AOT_TEST_CHECK(!kept.strings.codeUnitAt(longest, numberOfCodeUnits));
    }
}

static void stringTableFindsKeptTextsBehindGrowthOfShards()
{
    constexpr size_t count = 20000;
    Vector<uint8_t> run;
    for (size_t index = 0; index < count; ++index)
        run.append(static_cast<uint8_t>('a' + index * 7 % 26));
    TestStrings test;
    std::span<const uint8_t> bytes = test.strings.keep(run.span());
    StringTable::FrontsOfKeptTexts fronts;
    Vector<StringID> ids;
    for (size_t end = 2; end <= count; ++end)
        ids.append(test.strings.internKept(bytes.first(end), fronts));
    for (size_t end = 2; end <= count; end += 97)
        AOT_TEST_CHECK(test.strings.find(run.span().first(end)) == ids[end - 2]);
}

static void nameClassOfNumbers()
{
    const std::pair<ASCIILiteral, NameClass> cases[] = {
        { "0"_s, NameClass::Index },
        { "7"_s, NameClass::Index },
        { "4294967294"_s, NameClass::Index },
        { "4294967295"_s, NameClass::CanonicalNumericString },
        { "4294967296"_s, NameClass::CanonicalNumericString },
        { "01"_s, NameClass::Ordinary },
        { "00"_s, NameClass::Ordinary },
        { "1e21"_s, NameClass::Ordinary },
        { "1e+21"_s, NameClass::CanonicalNumericString },
        { "1000000000000000000000"_s, NameClass::Ordinary },
        { "-0"_s, NameClass::CanonicalNumericString },
        { "-1"_s, NameClass::CanonicalNumericString },
        { "-01"_s, NameClass::Ordinary },
        { "+1"_s, NameClass::Ordinary },
        { "1.5"_s, NameClass::CanonicalNumericString },
        { "1.50"_s, NameClass::Ordinary },
        { ".5"_s, NameClass::Ordinary },
        { "Infinity"_s, NameClass::CanonicalNumericString },
        { "-Infinity"_s, NameClass::CanonicalNumericString },
        { "NaN"_s, NameClass::CanonicalNumericString },
        { "-NaN"_s, NameClass::Ordinary },
        { "infinity"_s, NameClass::Ordinary },
        { "1 "_s, NameClass::Ordinary },
    };
    for (auto& [text, nameClass] : cases) {
        AOT_TEST_CHECK(classifyName(bytesOf(text)) == nameClass);
    }
}

std::span<const NativeTest> stringTableTests()
{
    static constexpr NativeTest tests[] = {
        { "string-table/strings-reject-overlong-and-split-pairs"_s, stringsRejectOverlongAndSplitPairs },
        { "string-table/intern-joins-surrogates"_s, stringTableInternJoinsSurrogates },
        { "string-table/holds-every-code-unit"_s, stringTableHoldsEveryCodeUnit },
        { "string-table/grows-shards"_s, stringTableGrowsShards },
        { "string-table/interns-kept-texts-as-any-other"_s, stringTableInternsKeptTextsAsAnyOther },
        { "string-table/finds-kept-texts-behind-the-growth-of-shards"_s, stringTableFindsKeptTextsBehindGrowthOfShards },
        { "name-class/numbers"_s, nameClassOfNumbers },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
