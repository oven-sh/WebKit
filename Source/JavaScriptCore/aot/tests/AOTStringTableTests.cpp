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

StringAsRead asReadWithoutClass(std::span<const uint8_t> bytes)
{
    return { bytes, NameClass::Ordinary, { } };
}

struct TestStrings {
    explicit TestStrings(Vector<StringAsRead>&& stringsAsRead = { }, unsigned width = 1)
        : loops(width)
        , result(strings.initialize(BuiltinTable::singleton(), loops, WTF::move(stringsAsRead)))
    {
    }

    bool failsWith(StringTable::Error error, uint32_t string) const { return result.error == error && result.string == StringID(string); }
    bool isInitialized() const { return result.error == StringTable::Error::None && !result.string; }

    ParallelLoops loops;
    StringTable strings;
    StringTable::InitializationResult result;
};

constexpr std::array<uint8_t, 2> overlongSequence { 0xc0, 0x80 };
constexpr std::array<uint8_t, 1> continuationByte { 0x80 };

} // anonymous namespace

static void stringsRoundTripThroughEncoding()
{
    const std::pair<Vector<char16_t>, Vector<uint8_t>> cases[] = {
        { { u'a', u'z' }, { 0x61, 0x7a } },
        { { 0x00e9 }, { 0xc3, 0xa9 } },
        { { 0x20ac }, { 0xe2, 0x82, 0xac } },
        { { 0xd83d, 0xde00 }, { 0xf0, 0x9f, 0x98, 0x80 } },
        { { 0xd83d }, { 0xed, 0xa0, 0xbd } },
        { { 0xde00 }, { 0xed, 0xb8, 0x80 } },
        { { 0xde00, 0xd83d }, { 0xed, 0xb8, 0x80, 0xed, 0xa0, 0xbd } },
    };
    for (auto& [units, bytes] : cases) {
        AOT_TEST_CHECK(encodeProgramString(units.span()) == bytes);
        AOT_TEST_CHECK(encodeProgramString(StringView(units.span())) == bytes);
        AOT_TEST_CHECK(decodeProgramString(bytes.span()) == units);
        AOT_TEST_CHECK(isCanonicalProgramString(bytes.span()));
    }
}

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

static void stringsEncodeLatin1Views()
{
    constexpr std::array<Latin1Character, 2> characters { 'a', 0xe9 };
    AOT_TEST_CHECK(encodeProgramString(StringView(std::span<const Latin1Character>(characters))) == Vector<uint8_t>({ 0x61, 0xc3, 0xa9 }));
    AOT_TEST_CHECK(encodeProgramString(StringView("az"_s)) == Vector<uint8_t>({ 0x61, 0x7a }));
    AOT_TEST_CHECK(encodeProgramString(StringView()).isEmpty());
}

static void stringsCompareByCodeUnits()
{
    constexpr std::array<uint8_t, 3> fullwidthTilde { 0xef, 0xbd, 0x9e };
    constexpr std::array<uint8_t, 4> supplementaryCharacter { 0xf0, 0x9f, 0x98, 0x80 };
    constexpr std::array<uint8_t, 3> leadSurrogate { 0xed, 0xa0, 0xbd };
    AOT_TEST_CHECK(compareProgramStrings(supplementaryCharacter, fullwidthTilde) == std::strong_ordering::less);
    AOT_TEST_CHECK(compareProgramStrings(fullwidthTilde, supplementaryCharacter) == std::strong_ordering::greater);
    AOT_TEST_CHECK(compareProgramStrings(leadSurrogate, supplementaryCharacter) == std::strong_ordering::less);
    AOT_TEST_CHECK(compareProgramStrings(bytesOf("a"_s), bytesOf("b"_s)) == std::strong_ordering::less);
    AOT_TEST_CHECK(compareProgramStrings(bytesOf("a"_s), bytesOf("ab"_s)) == std::strong_ordering::less);
    AOT_TEST_CHECK(compareProgramStrings(bytesOf("z"_s), fullwidthTilde) == std::strong_ordering::less);
    AOT_TEST_CHECK(compareProgramStrings(fullwidthTilde, fullwidthTilde) == std::strong_ordering::equal);
    AOT_TEST_CHECK(compareProgramStrings({ }, bytesOf("a"_s)) == std::strong_ordering::less);
}

static void stringTableReportsLowestDuplicate()
{
    TestStrings test({ asRead("a"_s), asRead("b"_s), asRead("a"_s), asRead("b"_s) });
    AOT_TEST_CHECK(test.failsWith(StringTable::Error::DuplicateString, 2));
}

static void stringTableReportsLowestNonCanonicalString()
{
    TestStrings test({ asRead("a"_s), asReadWithoutClass(overlongSequence), asRead("a"_s), asReadWithoutClass(continuationByte) });
    AOT_TEST_CHECK(test.failsWith(StringTable::Error::StringNotCanonical, 1));
}

static void stringTableReportsNameClassMismatch()
{
    AOT_TEST_CHECK(TestStrings({ asRead("~first"_s), { bytesOf("length"_s), NameClass::Length, BuiltinNames::length } }).isInitialized());
    AOT_TEST_CHECK(TestStrings({ asRead("~first"_s), { bytesOf("length"_s), NameClass::Ordinary, BuiltinNames::length } }).failsWith(StringTable::Error::NameClassMismatch, 1));
    AOT_TEST_CHECK(TestStrings({ { bytesOf("~first"_s), NameClass::Length, { } } }).failsWith(StringTable::Error::NameClassMismatch, 0));
    AOT_TEST_CHECK(TestStrings({ { bytesOf("7"_s), NameClass::CanonicalNumericString, BuiltinTable::singleton().nameOf(bytesOf("7"_s)) } }).failsWith(StringTable::Error::NameClassMismatch, 0));
}

static void stringTableReportsWrongBuiltinName()
{
    AOT_TEST_CHECK(TestStrings({ { bytesOf("length"_s), NameClass::Length, { } } }).failsWith(StringTable::Error::NameClassMismatch, 0));
    AOT_TEST_CHECK(TestStrings({ { bytesOf("length"_s), NameClass::Length, BuiltinNames::size } }).failsWith(StringTable::Error::NameClassMismatch, 0));
    AOT_TEST_CHECK(TestStrings({ asRead("a"_s), { bytesOf("~first"_s), NameClass::Ordinary, BuiltinNames::length } }).failsWith(StringTable::Error::NameClassMismatch, 1));
    AOT_TEST_CHECK(TestStrings({ { bytesOf("iterator"_s), NameClass::Ordinary, BuiltinNames::iteratorSymbol } }).failsWith(StringTable::Error::NameClassMismatch, 0));
    AOT_TEST_CHECK(TestStrings({ { bytesOf("iterator"_s), NameClass::Ordinary, BuiltinNames::iterator } }).isInitialized());
}

static void stringTableOrdersErrors()
{
    AOT_TEST_CHECK(TestStrings({ { overlongSequence, NameClass::Length, BuiltinNames::length } }).failsWith(StringTable::Error::StringNotCanonical, 0));
    AOT_TEST_CHECK(TestStrings({ asReadWithoutClass(bytesOf("length"_s)), asReadWithoutClass(overlongSequence) }).failsWith(StringTable::Error::NameClassMismatch, 0));
    AOT_TEST_CHECK(TestStrings({ asRead("a"_s), asRead("a"_s), asReadWithoutClass(overlongSequence) }).failsWith(StringTable::Error::StringNotCanonical, 2));
    AOT_TEST_CHECK(TestStrings({ asRead("a"_s), asRead("a"_s), asReadWithoutClass(bytesOf("length"_s)) }).failsWith(StringTable::Error::NameClassMismatch, 2));
}

static void stringTableKeepsStringsAsRead()
{
    constexpr ASCIILiteral first = "~first"_s;
    constexpr ASCIILiteral second = "then"_s;
    TestStrings test({ asRead(first), asRead(second) });
    StringTable& strings = test.strings;
    AOT_TEST_CHECK(test.isInitialized() && !strings.hasOpenPhase());
    AOT_TEST_CHECK(strings[StringID(0u)].data() == bytesOf(first).data() && strings[StringID(0u)].size() == first.length());
    AOT_TEST_CHECK(strings[StringID(1u)].data() == bytesOf(second).data() && strings[StringID(1u)].size() == second.length());
    AOT_TEST_CHECK(strings.nameClass(StringID(0u)) == NameClass::Ordinary && !strings.builtinName(StringID(0u)));
    AOT_TEST_CHECK(strings.nameClass(StringID(1u)) == NameClass::Then && strings.builtinName(StringID(1u)) == BuiltinNames::then);
    AOT_TEST_CHECK(strings.intern(bytesOf(first)) == StringID(0u) && strings.find(bytesOf(second)) == StringID(1u));
}

static void stringTableInternReturnsExistingID()
{
    TestStrings test;
    StringTable& strings = test.strings;
    AOT_TEST_CHECK(test.isInitialized());
    StringID id = strings.intern(bytesOf("~first"_s));
    size_t size = strings.size();
    AOT_TEST_CHECK(equalSpans(strings[id], bytesOf("~first"_s)));
    AOT_TEST_CHECK(strings.intern(bytesOf("~first"_s)) == id && strings.size() == size);
    AOT_TEST_CHECK(strings.intern(StringView("~first"_s)) == id && strings.size() == size);
    AOT_TEST_CHECK(strings.intern(std::span<const uint8_t> { }) == strings.emptyString() && strings.size() == size);
    AOT_TEST_CHECK(strings.intern(StringView()) == strings.emptyString() && strings[strings.emptyString()].empty());
    AOT_TEST_CHECK(strings.intern(bytesOf("~second"_s)) != id && strings.size() == size + 1);
}

static void stringTableClassifiesInternedStrings()
{
    TestStrings test;
    StringTable& strings = test.strings;
    AOT_TEST_CHECK(strings.nameClass(strings.intern(bytesOf("~first"_s))) == NameClass::Ordinary);
    AOT_TEST_CHECK(strings.nameClass(strings.intern(bytesOf("12"_s))) == NameClass::Index);
    AOT_TEST_CHECK(strings.nameClass(strings.intern(bytesOf("1.5"_s))) == NameClass::CanonicalNumericString);
    AOT_TEST_CHECK(strings.nameClass(strings.stringOf(BuiltinNames::length)) == NameClass::Length);
    AOT_TEST_CHECK(strings.nameClass(strings.emptyString()) == NameClass::Ordinary);
}

static void stringTableFindsOnlyInternedStrings()
{
    TestStrings test;
    StringTable& strings = test.strings;
    size_t size = strings.size();
    AOT_TEST_CHECK(!strings.find(bytesOf("~first"_s)) && strings.size() == size);
    StringID id = strings.intern(bytesOf("~first"_s));
    AOT_TEST_CHECK(strings.find(bytesOf("~first"_s)) == id && !strings.find(bytesOf("~firs"_s)) && !strings.find(bytesOf("~first~"_s)));
    AOT_TEST_CHECK(strings.find({ }) == strings.emptyString());
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

static void stringTableRoundTripsLoneSurrogates()
{
    TestStrings test;
    StringTable& strings = test.strings;
    const Vector<char16_t> cases[] = {
        { 0xd83d },
        { 0xde00 },
        { u'a', 0xde00, 0xd83d, u'b' },
        { 0xd83d, 0xd83d, 0xde00 },
    };
    for (const Vector<char16_t>& units : cases) {
        StringID id = strings.intern(StringView(units.span()));
        AOT_TEST_CHECK(strings[id].size() && isCanonicalProgramString(strings[id]) && !strings.isAllASCII(id));
        AOT_TEST_CHECK(std::ranges::equal(strings[id], encodeProgramString(units.span())));
        Vector<char16_t> buffer;
        AOT_TEST_CHECK(strings.viewOf(id, buffer) == StringView(units.span()) && buffer == units);
        String string = strings.toWTFString(id);
        AOT_TEST_CHECK(string == StringView(units.span()) && strings.intern(StringView(string)) == id);
    }
}

static void stringTableViewsASCIIInPlace()
{
    constexpr ASCIILiteral asReadText = "~first"_s;
    TestStrings test({ asRead(asReadText) });
    StringTable& strings = test.strings;
    for (StringID id : { StringID(0u), strings.intern(bytesOf("~second"_s)) }) {
        Vector<char16_t> buffer;
        StringView view = strings.viewOf(id, buffer);
        AOT_TEST_CHECK(strings.isAllASCII(id) && view.is8Bit() && buffer.isEmpty());
        AOT_TEST_CHECK(byteCast<uint8_t>(view.span8()).data() == strings[id].data() && view.length() == strings[id].size());
    }
    Vector<char16_t> buffer;
    AOT_TEST_CHECK(byteCast<uint8_t>(strings.viewOf(StringID(0u), buffer).span8()).data() == bytesOf(asReadText).data());
    AOT_TEST_CHECK(strings.viewOf(strings.emptyString(), buffer).isEmpty() && buffer.isEmpty());
    AOT_TEST_CHECK(strings.toWTFString(StringID(0u)) == asReadText && strings.toWTFString(strings.emptyString()).isEmpty());
}

static void stringTableDecodesOtherViewsIntoBuffer()
{
    TestStrings test;
    StringTable& strings = test.strings;
    constexpr std::array<uint8_t, 3> text { 0x61, 0xc3, 0xa9 };
    StringID id = strings.intern(text);
    Vector<char16_t> buffer;
    StringView view = strings.viewOf(id, buffer);
    AOT_TEST_CHECK(!strings.isAllASCII(id) && !view.is8Bit() && view.span16().data() == buffer.span().data());
    AOT_TEST_CHECK(buffer == Vector<char16_t>({ u'a', 0x00e9 }) && view.length() == 2);
    AOT_TEST_CHECK(strings.toWTFString(id) == view);
}

static void stringTableHasEveryBuiltinTextName()
{
    TestStrings test;
    StringTable& strings = test.strings;
    const BuiltinTable& table = BuiltinTable::singleton();
    AOT_TEST_CHECK(table.nameCount());
    for (unsigned index = 0; index < table.nameCount(); ++index) {
        BuiltinName name(index);
        StringID string = strings.stringOf(name);
        if (table.isSymbolName(name) || table.isRegisteredSymbolName(name)) {
            AOT_TEST_CHECK(!string);
            continue;
        }
        AOT_TEST_CHECK(string && strings.builtinName(string) == name && equalSpans(strings[string], table.textOf(name)));
    }
    AOT_TEST_CHECK(!strings.builtinName(strings.intern(bytesOf("~first"_s))));
    AOT_TEST_CHECK(strings.stringOf(BuiltinNames::iterator) && !strings.stringOf(BuiltinNames::iteratorSymbol));
    AOT_TEST_CHECK(strings.find(table.textOf(BuiltinNames::iteratorSymbol)) == strings.stringOf(BuiltinNames::iterator));
}

static void stringTableUsesProgramStringForBuiltinName()
{
    TestStrings test({ asRead("~first"_s), asRead("length"_s) });
    StringTable& strings = test.strings;
    AOT_TEST_CHECK(test.isInitialized());
    AOT_TEST_CHECK(strings.stringOf(BuiltinNames::length) == StringID(1u) && strings.builtinName(StringID(1u)) == BuiltinNames::length);
    AOT_TEST_CHECK(strings.intern(bytesOf("length"_s)) == StringID(1u));
}

static void stringTableGrowsShards()
{
    constexpr unsigned count = 10000;
    TestStrings test;
    StringTable& strings = test.strings;
    size_t size = strings.size();
    Vector<StringID> ids;
    for (unsigned index = 0; index < count; ++index)
        ids.append(strings.intern(distinctText(index)));
    AOT_TEST_CHECK(strings.size() == size + count);
    for (unsigned index = 0; index < count; ++index)
        AOT_TEST_CHECK(strings.intern(distinctText(index)) == ids[index]);
    AOT_TEST_CHECK(strings.size() == size + count);
}

static void stringTableFillsShardsInParallel()
{
    constexpr unsigned count = 5000;
    Vector<std::array<uint8_t, 5>> texts;
    for (unsigned index = 0; index < count; ++index)
        texts.append(distinctText(index));
    auto stringsAsRead = [&] {
        return texts.map([](auto& text) {
            return asRead(text);
        });
    };

    size_t sizeOnOneThread = 0;
    for (unsigned width : { 1u, 8u }) {
        TestStrings test(stringsAsRead(), width);
        AOT_TEST_CHECK(test.isInitialized());
        for (unsigned index = 0; index < count; ++index)
            AOT_TEST_CHECK(test.strings.find(texts[index]) == StringID(index));
        if (width == 1)
            sizeOnOneThread = test.strings.size();
        AOT_TEST_CHECK(test.strings.size() == sizeOnOneThread && sizeOnOneThread > count);
    }

    texts[4000] = texts[10];
    texts[3000] = texts[20];
    texts[4999] = texts[3000];
    for (unsigned width : { 1u, 8u })
        AOT_TEST_CHECK(TestStrings(stringsAsRead(), width).failsWith(StringTable::Error::DuplicateString, 3000));
}

static void nameClassOfEveryWord()
{
    unsigned numberOfWords = 0;
    for (NameClass nameClass : allMembersOf<NameClass>) {
        std::span<const char> words = wordsOf(nameClass).span();
        while (!words.empty()) {
            size_t length = std::ranges::find(words, ' ') - words.begin();
            AOT_TEST_CHECK(length && classifyName(byteCast<uint8_t>(words.first(length))) == nameClass);
            words = words.subspan(std::min(length + 1, words.size()));
            ++numberOfWords;
        }
    }
    AOT_TEST_CHECK(numberOfWords >= 20);

    const std::pair<ASCIILiteral, NameClass> cases[] = {
        { "__proto__"_s, NameClass::Proto },
        { "constructor"_s, NameClass::Constructor },
        { "hasOwnProperty"_s, NameClass::ObjectPrototypeMember },
        { "__lookupSetter__"_s, NameClass::ObjectPrototypeMember },
        { "length"_s, NameClass::Length },
        { "prototype"_s, NameClass::Prototype },
        { "call"_s, NameClass::CallOrApply },
        { "apply"_s, NameClass::CallOrApply },
        { "bind"_s, NameClass::Bind },
        { "lastIndex"_s, NameClass::LastIndex },
        { "then"_s, NameClass::Then },
        { "toJSON"_s, NameClass::ToJSON },
    };
    for (auto& [text, nameClass] : cases)
        AOT_TEST_CHECK(classifyName(bytesOf(text)) == nameClass);
}

static void nameClassOfOtherWords()
{
    constexpr std::array<uint8_t, 2> nonASCIIText { 0xc3, 0xa9 };
    AOT_TEST_CHECK(classifyName({ }) == NameClass::Ordinary);
    AOT_TEST_CHECK(classifyName(nonASCIIText) == NameClass::Ordinary);
    for (ASCIILiteral text : { "Length"_s, "lengths"_s, "lengt"_s, "call apply"_s, "call "_s, " "_s, "thenthenthenthenthenthenthenthenthen"_s })
        AOT_TEST_CHECK(classifyName(bytesOf(text)) == NameClass::Ordinary);
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
        AOT_TEST_CHECK(mayBeElementKey(nameClass) == (accessPathOf(nameClass) == NameAccessPath::ByKey));
    }
}

std::span<const NativeTest> stringTableTests()
{
    static constexpr NativeTest tests[] = {
        { "string-table/strings-round-trip-through-encoding"_s, stringsRoundTripThroughEncoding },
        { "string-table/strings-reject-overlong-and-split-pairs"_s, stringsRejectOverlongAndSplitPairs },
        { "string-table/strings-encode-latin1-views"_s, stringsEncodeLatin1Views },
        { "string-table/strings-compare-by-code-units"_s, stringsCompareByCodeUnits },
        { "string-table/reports-lowest-duplicate"_s, stringTableReportsLowestDuplicate },
        { "string-table/reports-lowest-non-canonical-string"_s, stringTableReportsLowestNonCanonicalString },
        { "string-table/reports-name-class-mismatch"_s, stringTableReportsNameClassMismatch },
        { "string-table/reports-wrong-builtin-name"_s, stringTableReportsWrongBuiltinName },
        { "string-table/orders-errors"_s, stringTableOrdersErrors },
        { "string-table/keeps-strings-as-read"_s, stringTableKeepsStringsAsRead },
        { "string-table/intern-returns-existing-id"_s, stringTableInternReturnsExistingID },
        { "string-table/classifies-interned-strings"_s, stringTableClassifiesInternedStrings },
        { "string-table/finds-only-interned-strings"_s, stringTableFindsOnlyInternedStrings },
        { "string-table/intern-joins-surrogates"_s, stringTableInternJoinsSurrogates },
        { "string-table/round-trips-lone-surrogates"_s, stringTableRoundTripsLoneSurrogates },
        { "string-table/views-ascii-in-place"_s, stringTableViewsASCIIInPlace },
        { "string-table/decodes-other-views-into-buffer"_s, stringTableDecodesOtherViewsIntoBuffer },
        { "string-table/has-every-builtin-text-name"_s, stringTableHasEveryBuiltinTextName },
        { "string-table/uses-program-string-for-builtin-name"_s, stringTableUsesProgramStringForBuiltinName },
        { "string-table/grows-shards"_s, stringTableGrowsShards },
        { "string-table/fills-shards-in-parallel"_s, stringTableFillsShardsInParallel },
        { "name-class/every-word"_s, nameClassOfEveryWord },
        { "name-class/other-words"_s, nameClassOfOtherWords },
        { "name-class/numbers"_s, nameClassOfNumbers },
    };
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
