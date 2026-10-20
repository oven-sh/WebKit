/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsOperations.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "JSString.h"
#include "NumericStrings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <span>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubStringTests();

namespace {

constexpr unsigned maxLengthComparedWithoutCall = defaultLimitValue(LimitID::MaxInlineStringCompareLength, hostArchitecture());
constexpr unsigned maxTestedLength = maxLengthComparedWithoutCall + 2 * sizeof(uint64_t) + 1;

enum class CharacterWidth : bool { Is8Bit, Is16Bit };
enum class Placement : bool { AtStartOfPage, AtEndOfPage };

using Units = Vector<char16_t>;

struct alignas(16) FakeString {
    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

    static FakeString flat(StringImpl& impl) { return { .fiber = std::bit_cast<uintptr_t>(&impl) }; }
    static FakeString rope(uint32_t length) { return { .fiber = JSString::isRopeInPointer, .ropeLength = length }; }

    uint32_t structureID { 0 };
    uint8_t indexingTypeAndMisc { 0 };
    uint8_t type { StringType };
    uint8_t flags { 0 };
    uint8_t cellState { 0 };
    uintptr_t fiber { 0 };
    uint32_t ropeLength { 0 };
};
static_assert(OBJECT_OFFSETOF(FakeString, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeString, fiber) == JSString::offsetOfValue());
static_assert(OBJECT_OFFSETOF(FakeString, ropeLength) == JSRopeString::offsetOfLength());

// A read before the first or after the last character of a text at that end of the page faults.
class GuardedString {
    WTF_MAKE_NONCOPYABLE(GuardedString);
public:
    GuardedString()
        : m_pages(static_cast<uint8_t*>(OSAllocator::reserveAndCommit(3 * pageSize())))
    {
        OSAllocator::protect(m_pages, pageSize(), false, false);
        OSAllocator::protect(m_pages + 2 * pageSize(), pageSize(), false, false);
    }

    ~GuardedString()
    {
        m_impl = nullptr;
        OSAllocator::protect(m_pages, 3 * pageSize(), true, true);
        OSAllocator::decommitAndRelease(m_pages, 3 * pageSize());
    }

    void set(const Units& units, CharacterWidth width, Placement placement)
    {
        m_impl = width == CharacterWidth::Is8Bit ? write<Latin1Character>(units, placement) : write<char16_t>(units, placement);
        m_cell = FakeString::flat(*m_impl);
    }

    uint64_t address() const { return m_cell.address(); }
    StringImpl* impl() const { return m_impl.get(); }

private:
    template<typename Character>
    Ref<StringImpl> write(const Units& units, Placement placement)
    {
        size_t size = units.size() * sizeof(Character);
        AOT_TEST_CHECK(size <= pageSize());
        uint8_t* page = m_pages + pageSize();
        auto* characters = reinterpret_cast<Character*>(placement == Placement::AtStartOfPage ? page : page + pageSize() - size);
        for (size_t i = 0; i < units.size(); ++i) {
            characters[i] = static_cast<Character>(units[i]);
            AOT_TEST_CHECK(characters[i] == units[i]);
        }
        return StringImpl::createWithoutCopying(std::span<const Character>(characters, units.size()));
    }

    uint8_t* m_pages;
    RefPtr<StringImpl> m_impl;
    FakeString m_cell;
};

unsigned numberOfLeafCalls;

size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES countedStringImplsAreEqual(void* first, void* second)
{
    ++numberOfLeafCalls;
    return operationAOTStringImplsAreEqual(first, second);
}

int64_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES countedCompareStringImpls(void* first, void* second)
{
    ++numberOfLeafCalls;
    return operationAOTCompareStringImpls(first, second);
}

struct Relation {
    BinaryOperator operation;
    bool (*holdsFor)(int64_t order);
};

constexpr Relation relations[] = {
    { BinaryOperator::Less, [](int64_t order) { return order < 0; } },
    { BinaryOperator::LessEq, [](int64_t order) { return order <= 0; } },
    { BinaryOperator::Greater, [](int64_t order) { return order > 0; } },
    { BinaryOperator::GreaterEq, [](int64_t order) { return order >= 0; } },
};

class ComparisonStubs {
public:
    ComparisonStubs()
    {
        m_runner.install(Entry::operationAOTStringImplsAreEqual, countedStringImplsAreEqual);
        m_runner.install(Entry::operationAOTCompareStringImpls, countedCompareStringImpls);
    }

    StubRunner& runner() { return m_runner; }

    struct Answer {
        uint64_t result;
        unsigned numberOfLeafCalls;
    };

    Answer runWithoutOperation(StubKey key, uint64_t first, uint64_t second)
    {
        numberOfLeafCalls = 0;
        StubRun run = m_runner.run(key, { first, second });
        AOT_TEST_CHECK(!run.calledOperation);
        return { run.result, numberOfLeafCalls };
    }

    void checkFlatStrings(const Units& first, const Units& second)
    {
        bool fitIn8Bits = std::ranges::all_of(first, isLatin1) && std::ranges::all_of(second, isLatin1);
        for (CharacterWidth firstWidth : { CharacterWidth::Is8Bit, CharacterWidth::Is16Bit }) {
            for (CharacterWidth secondWidth : { CharacterWidth::Is8Bit, CharacterWidth::Is16Bit }) {
                if (!fitIn8Bits && (firstWidth == CharacterWidth::Is8Bit || secondWidth == CharacterWidth::Is8Bit))
                    continue;
                for (Placement placement : { Placement::AtStartOfPage, Placement::AtEndOfPage }) {
                    m_first.set(first, firstWidth, placement);
                    m_second.set(second, secondWidth, placement);
                    checkStrings(first, second);
                }
            }
        }
    }

private:
    static bool isLatin1(char16_t unit) { return unit <= 0xff; }

    static int64_t orderOf(const Units& first, const Units& second)
    {
        size_t commonLength = std::min(first.size(), second.size());
        for (size_t i = 0; i < commonLength; ++i) {
            if (first[i] != second[i])
                return first[i] < second[i] ? -1 : 1;
        }
        if (first.size() == second.size())
            return 0;
        return first.size() < second.size() ? -1 : 1;
    }

    void checkStrings(const Units& first, const Units& second)
    {
        const int64_t order = orderOf(first, second);
        const bool areBoth8Bit = m_first.impl()->is8Bit() && m_second.impl()->is8Bit();
        const bool haveSameWidth = m_first.impl()->is8Bit() == m_second.impl()->is8Bit();

        Answer equality = runWithoutOperation(stringsAreEqual(), m_first.address(), m_second.address());
        AOT_TEST_CHECK(equality.result == static_cast<uint64_t>(!order));
        bool comparesCharacters = first.size() == second.size() && m_first.impl() != m_second.impl();
        AOT_TEST_CHECK(equality.numberOfLeafCalls == (comparesCharacters && (!haveSameWidth || first.size() > maxLengthComparedWithoutCall)));
        for (bool isStrict : { false, true }) {
            Answer answer = runWithoutOperation(JSC::AOT::equal(isStrict), m_first.address(), m_second.address());
            AOT_TEST_CHECK(answer.result == equality.result && answer.numberOfLeafCalls == equality.numberOfLeafCalls);
        }

        const bool orderNeedsLeaf = !areBoth8Bit || std::min(first.size(), second.size()) > maxLengthComparedWithoutCall;
        Answer comparison = runWithoutOperation(compareStrings(), m_first.address(), m_second.address());
        AOT_TEST_CHECK(comparison.result == static_cast<uint64_t>(order));
        AOT_TEST_CHECK(comparison.numberOfLeafCalls == orderNeedsLeaf);

        for (const Relation& relation : relations) {
            Answer answer = runWithoutOperation(binaryOperation(relation.operation, false), m_first.address(), m_second.address());
            AOT_TEST_CHECK(answer.result == static_cast<uint64_t>(relation.holdsFor(order)));
            AOT_TEST_CHECK(answer.numberOfLeafCalls == orderNeedsLeaf);
        }
    }

    StubRunner m_runner;
    GuardedString m_first;
    GuardedString m_second;
};

} // anonymous namespace

// Neighbors are on both sides of 0x80, where a signed comparison of bytes goes wrong.
static Units textOfLength(unsigned length, char16_t highBits)
{
    Units units;
    for (unsigned i = 0; i < length; ++i)
        units.append(highBits | (0x7f + i % 3));
    return units;
}

// The units after the position differ the other way, so a comparison that starts at the wrong end of a word gives the opposite order.
static Units textDifferingFrom(const Units& text, unsigned position, int difference)
{
    Units units = text;
    units[position] += difference;
    for (unsigned i = position + 1; i < units.size(); ++i)
        units[i] -= difference;
    return units;
}

static void testDifferenceAtEveryPosition()
{
    ComparisonStubs stubs;
    for (unsigned length = 0; length <= maxTestedLength; ++length) {
        for (char16_t highBits : { 0, 0x300 }) {
            Units text = textOfLength(length, highBits);
            stubs.checkFlatStrings(text, text);
            for (unsigned position = 0; position < length; ++position) {
                for (int difference : { 1, -1 })
                    stubs.checkFlatStrings(text, textDifferingFrom(text, position, difference));
                if (highBits)
                    stubs.checkFlatStrings(text, textDifferingFrom(text, position, 0x100));
            }
        }
    }
}

static void testDifferenceInLastUnitOnly()
{
    ComparisonStubs stubs;
    for (unsigned length = 1; length <= maxTestedLength; ++length) {
        Units text = textOfLength(length, 0);
        Units other = text;
        ++other.last();
        stubs.checkFlatStrings(text, other);
        stubs.checkFlatStrings(other, text);
    }
}

static void testCommonPrefixes()
{
    ComparisonStubs stubs;
    for (unsigned firstLength = 0; firstLength <= maxTestedLength; ++firstLength) {
        for (unsigned secondLength = 0; secondLength <= maxTestedLength; ++secondLength)
            stubs.checkFlatStrings(textOfLength(firstLength, 0), textOfLength(secondLength, 0));
    }
}

static void testOrderOfWideUnits()
{
    ComparisonStubs stubs;
    stubs.checkFlatStrings({ 0x0100 }, { 0x00ff });
    stubs.checkFlatStrings({ 0x00ff }, { 0x0100 });
    stubs.checkFlatStrings({ 0x0100, 0x0041 }, { 0x0041, 0x0100 });
    stubs.checkFlatStrings({ 0xd83d, 0xde00 }, { 0xff5e });
    stubs.checkFlatStrings({ 0xff5e }, { 0xd83d, 0xde00 });
    stubs.checkFlatStrings({ 0x0041, 0x0100 }, { 0x0041 });
}

static void testSameCellsAndImpls()
{
    ComparisonStubs stubs;
    Ref<StringImpl> impl = StringImpl::create("the same characters"_s);
    FakeString cell = FakeString::flat(impl.get());
    FakeString cellOfSameImpl = FakeString::flat(impl.get());
    FakeString rope = FakeString::rope(impl->length());

    for (const FakeString* same : { &cell, &rope }) {
        ComparisonStubs::Answer equality = stubs.runWithoutOperation(stringsAreEqual(), same->address(), same->address());
        AOT_TEST_CHECK(equality.result == 1 && !equality.numberOfLeafCalls);
        ComparisonStubs::Answer comparison = stubs.runWithoutOperation(compareStrings(), same->address(), same->address());
        AOT_TEST_CHECK(!comparison.result && !comparison.numberOfLeafCalls);
    }

    ComparisonStubs::Answer equality = stubs.runWithoutOperation(stringsAreEqual(), cell.address(), cellOfSameImpl.address());
    AOT_TEST_CHECK(equality.result == 1 && !equality.numberOfLeafCalls);
    ComparisonStubs::Answer comparison = stubs.runWithoutOperation(compareStrings(), cell.address(), cellOfSameImpl.address());
    AOT_TEST_CHECK(!comparison.result && !comparison.numberOfLeafCalls);
}

static void testAtoms()
{
    ComparisonStubs stubs;
    constexpr ASCIILiteral text = "an atom that is longer than what a stub compares by itself: a"_s;
    constexpr ASCIILiteral otherText = "an atom that is longer than what a stub compares by itself: b"_s;
    static_assert(text.length() == otherText.length() && text.length() > maxLengthComparedWithoutCall);
    AtomString atom(text);
    AtomString otherAtom(otherText);
    Ref<StringImpl> copy = StringImpl::create(text.span8());
    Ref<StringImpl> otherCopy = StringImpl::create(otherText.span8());
    AOT_TEST_CHECK(atom.impl()->isAtom() && otherAtom.impl()->isAtom() && !copy->isAtom() && !otherCopy->isAtom());

    struct Case {
        StringImpl* first;
        StringImpl* second;
        bool areEqual;
        bool comparesCharacters;
    };
    const Case cases[] = {
        { atom.impl(), otherAtom.impl(), false, false },
        { otherAtom.impl(), atom.impl(), false, false },
        { atom.impl(), copy.ptr(), true, true },
        { copy.ptr(), atom.impl(), true, true },
        { atom.impl(), otherCopy.ptr(), false, true },
        { copy.ptr(), otherCopy.ptr(), false, true },
    };
    for (const Case& testCase : cases) {
        FakeString first = FakeString::flat(*testCase.first);
        FakeString second = FakeString::flat(*testCase.second);
        ComparisonStubs::Answer equality = stubs.runWithoutOperation(stringsAreEqual(), first.address(), second.address());
        AOT_TEST_CHECK(equality.result == testCase.areEqual && equality.numberOfLeafCalls == testCase.comparesCharacters);
    }
}

static void checkLeftToOperation(StubRunner& runner, StubKey key, Entry entry, const FakeString& first, const FakeString& second, std::initializer_list<uint64_t> results)
{
    runner.recordCallsOf(entry);
    for (uint64_t resultOfOperation : results) {
        runner.setResultOfRecordedOperation(resultOfOperation);
        numberOfLeafCalls = 0;
        StubRun run = runner.run(key, { first.address(), second.address() });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && !numberOfLeafCalls);
        AOT_TEST_CHECK(run.recordedArguments[0] == first.address() && run.recordedArguments[1] == second.address());
        AOT_TEST_CHECK(run.result == resultOfOperation);
    }
    runner.countCallsOf(entry);
}

static void testRopes()
{
    ComparisonStubs stubs;
    Ref<StringImpl> impl = StringImpl::create("flat"_s);
    FakeString flat = FakeString::flat(impl.get());
    FakeString rope = FakeString::rope(impl->length());
    FakeString otherRope = FakeString::rope(impl->length());
    FakeString longerRope = FakeString::rope(impl->length() + 1);

    const std::pair<const FakeString*, const FakeString*> pairsOfSameLength[] = { { &flat, &rope }, { &rope, &flat }, { &rope, &otherRope } };
    for (auto [first, second] : pairsOfSameLength) {
        checkLeftToOperation(stubs.runner(), stringsAreEqual(), Entry::operationAOTStringsAreEqual, *first, *second, { 0, 1 });
        checkLeftToOperation(stubs.runner(), compareStrings(), Entry::operationAOTCompareStrings, *first, *second, { static_cast<uint64_t>(-1), 0, 1 });
    }

    const std::pair<const FakeString*, const FakeString*> pairsOfOtherLength[] = { { &flat, &longerRope }, { &longerRope, &flat }, { &rope, &longerRope }, { &longerRope, &rope } };
    for (auto [first, second] : pairsOfOtherLength) {
        ComparisonStubs::Answer equality = stubs.runWithoutOperation(stringsAreEqual(), first->address(), second->address());
        AOT_TEST_CHECK(!equality.result && !equality.numberOfLeafCalls);
        checkLeftToOperation(stubs.runner(), compareStrings(), Entry::operationAOTCompareStrings, *first, *second, { static_cast<uint64_t>(-1), 1 });
    }
}

namespace {

struct CaseConversion {
    Entry operation;
    char16_t firstConvertedLetter;
    char16_t lastConvertedLetter;

    StubKey call() const { return callOperationFor(operation, OperationCall::Returns::Value, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(operation)); }
};

constexpr CaseConversion caseConversions[] = {
    { Entry::operationAOTStringToLowerCase, 'A', 'Z' },
    { Entry::operationAOTStringToUpperCase, 'a', 'z' },
};

class CaseConversionCalls {
public:
    CaseConversionCalls()
        : m_runner(StubPolicy { }, std::array { caseConversions[0].call(), caseConversions[1].call() })
    {
    }

    void check(const CaseConversion& conversion, uint64_t string, bool isAnsweredWithoutOperation)
    {
        constexpr uint64_t resultOfOperation = 0x7777;
        m_runner.recordCallsOf(conversion.operation);
        m_runner.setResultOfRecordedOperation(resultOfOperation);
        StubRun run = m_runner.runOperationCall(conversion.call(), { string });
        if (isAnsweredWithoutOperation)
            AOT_TEST_CHECK(!run.calledOperation && run.result == string);
        else
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == string && run.result == resultOfOperation);
        m_runner.countCallsOf(conversion.operation);
    }

    void check(const CaseConversion& conversion, const Units& units, CharacterWidth width, bool isAnsweredWithoutOperation)
    {
        for (Placement placement : { Placement::AtStartOfPage, Placement::AtEndOfPage }) {
            m_string.set(units, width, placement);
            check(conversion, m_string.address(), isAnsweredWithoutOperation);
        }
    }

private:
    StubRunner m_runner;
    GuardedString m_string;
};

} // anonymous namespace

static void testCaseConversionOfEveryUnit()
{
    CaseConversionCalls calls;
    for (const CaseConversion& conversion : caseConversions) {
        for (char16_t unit = 0; unit <= 0xff; ++unit) {
            bool staysAsItIs = unit < 0x80 && (unit < conversion.firstConvertedLetter || unit > conversion.lastConvertedLetter);
            calls.check(conversion, Units { unit }, CharacterWidth::Is8Bit, staysAsItIs);
        }
    }
}

static void testCaseConversionAtEveryPosition()
{
    CaseConversionCalls calls;
    constexpr unsigned maxLength = 3 * sizeof(uint64_t) + 1;
    for (const CaseConversion& conversion : caseConversions) {
        // The neighbors of the converted letters on both sides.
        const char16_t unchangedUnits[] = { static_cast<char16_t>(conversion.firstConvertedLetter - 1), static_cast<char16_t>(conversion.lastConvertedLetter + 1), 0, 0x7f };
        for (unsigned length = 0; length <= maxLength; ++length) {
            Units units;
            for (unsigned i = 0; i < length; ++i)
                units.append(unchangedUnits[i % std::size(unchangedUnits)]);
            calls.check(conversion, units, CharacterWidth::Is8Bit, true);
            calls.check(conversion, units, CharacterWidth::Is16Bit, !length);
            for (unsigned position = 0; position < length; ++position) {
                for (char16_t changing : { conversion.firstConvertedLetter, conversion.lastConvertedLetter, static_cast<char16_t>(0x80), static_cast<char16_t>(0xff) }) {
                    Units changed = units;
                    changed[position] = changing;
                    calls.check(conversion, changed, CharacterWidth::Is8Bit, false);
                }
            }
        }
    }
}

static void testCaseConversionOfRopes()
{
    CaseConversionCalls calls;
    FakeString rope = FakeString::rope(4);
    for (const CaseConversion& conversion : caseConversions)
        calls.check(conversion, rope.address(), false);
}

namespace {

// Every entry of the hashed cache has one key, so a probe hits whatever the hash is.
class IntegerStrings {
public:
    IntegerStrings()
    {
        for (size_t index = 0; index < NumericStrings::cacheSize; ++index)
            m_smallIntegers[index].jsString = cellOfSmallInteger(index);
        Instance& instance = m_runner.instance();
        instance.realm.smallIntStrings = m_smallIntegers.data();
        instance.realm.intStrings = m_integers.data();
        m_runner.recordCallsOf(Entry::operationAOTIntToString);
        m_runner.setResultOfRecordedOperation(resultOfOperation);
    }

    static JSString* cellOfSmallInteger(size_t integer) { return std::bit_cast<JSString*>(0x10000 + 16 * integer); }
    static JSString* cachedCell() { return std::bit_cast<JSString*>(static_cast<uintptr_t>(0x7770)); }

    void cache(int32_t key, JSString* cell)
    {
        for (auto& entry : m_integers) {
            entry.key = key;
            entry.jsString = cell;
        }
    }

    void checkIsFound(int64_t integer, JSString* cell)
    {
        StubRun run = m_runner.run(integerToString(), { static_cast<uint64_t>(integer) });
        AOT_TEST_CHECK(!run.numberOfRecordedCalls && run.result == std::bit_cast<uint64_t>(cell));
    }

    void checkIsLeftToOperation(int64_t integer)
    {
        StubRun run = m_runner.run(integerToString(), { static_cast<uint64_t>(integer) });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == static_cast<uint64_t>(integer) && run.result == resultOfOperation);
    }

private:
    static constexpr uint64_t resultOfOperation = 0x5550;

    StubRunner m_runner;
    std::array<NumericStrings::StringWithJSString, NumericStrings::cacheSize> m_smallIntegers { };
    std::array<NumericStrings::CacheEntryWithJSString<int>, NumericStrings::cacheSize> m_integers { };
};

} // anonymous namespace

static constexpr int64_t firstIntegerInHashedCache = NumericStrings::cacheSize;

static void testCachedIntegers()
{
    IntegerStrings strings;
    for (int64_t integer : { int64_t { 0 }, int64_t { 7 }, firstIntegerInHashedCache - 1 })
        strings.checkIsFound(integer, IntegerStrings::cellOfSmallInteger(integer));
    for (int64_t integer : { firstIntegerInHashedCache, int64_t { -1 }, int64_t { INT32_MAX }, int64_t { INT32_MIN } }) {
        strings.cache(static_cast<int32_t>(integer), IntegerStrings::cachedCell());
        strings.checkIsFound(integer, IntegerStrings::cachedCell());
    }
}

static void testIntegersOutsideTheCaches()
{
    IntegerStrings strings;
    strings.cache(static_cast<int32_t>(firstIntegerInHashedCache), IntegerStrings::cachedCell());
    strings.checkIsLeftToOperation(firstIntegerInHashedCache + 1);
    strings.cache(static_cast<int32_t>(firstIntegerInHashedCache), nullptr);
    strings.checkIsLeftToOperation(firstIntegerInHashedCache);
}

// The low half of each is a key that the caches hold.
static void testIntegersBeyondInt32()
{
    IntegerStrings strings;
    for (int64_t low : { int64_t { 0 }, int64_t { 7 }, firstIntegerInHashedCache, int64_t { INT32_MAX }, int64_t { 0x80000000 }, int64_t { 0xffffffff } }) {
        strings.cache(static_cast<int32_t>(low), IntegerStrings::cachedCell());
        for (int64_t high : { int64_t { 1 }, int64_t { -1 }, int64_t { 0x1fffff }, int64_t { INT32_MIN } }) {
            int64_t integer = static_cast<int64_t>(static_cast<uint64_t>(high) << 32) | low;
            if (integer != static_cast<int32_t>(integer))
                strings.checkIsLeftToOperation(integer);
        }
    }
    strings.cache(INT32_MIN, IntegerStrings::cachedCell());
    strings.checkIsLeftToOperation(int64_t { 0x80000000 });
    strings.cache(-1, IntegerStrings::cachedCell());
    strings.checkIsLeftToOperation(int64_t { 0xffffffff });
}

std::span<const NativeTest> stubStringTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/integer-to-string-cached-integers"_s, ImageTests::whenExecutable<testCachedIntegers> },
        { "stubs/integer-to-string-integers-outside-the-caches"_s, ImageTests::whenExecutable<testIntegersOutsideTheCaches> },
        { "stubs/integer-to-string-integers-beyond-int32"_s, ImageTests::whenExecutable<testIntegersBeyondInt32> },
        { "stubs/string-case-conversion-of-every-unit"_s, ImageTests::whenExecutable<testCaseConversionOfEveryUnit> },
        { "stubs/string-case-conversion-at-every-position"_s, ImageTests::whenExecutable<testCaseConversionAtEveryPosition> },
        { "stubs/string-case-conversion-of-ropes"_s, ImageTests::whenExecutable<testCaseConversionOfRopes> },
        { "stubs/string-comparison-difference-at-every-position"_s, ImageTests::whenExecutable<testDifferenceAtEveryPosition> },
        { "stubs/string-comparison-difference-in-last-unit-only"_s, ImageTests::whenExecutable<testDifferenceInLastUnitOnly> },
        { "stubs/string-comparison-common-prefixes"_s, ImageTests::whenExecutable<testCommonPrefixes> },
        { "stubs/string-comparison-order-of-wide-units"_s, ImageTests::whenExecutable<testOrderOfWideUnits> },
        { "stubs/string-comparison-same-cells-and-impls"_s, ImageTests::whenExecutable<testSameCellsAndImpls> },
        { "stubs/string-comparison-atoms"_s, ImageTests::whenExecutable<testAtoms> },
        { "stubs/string-comparison-ropes"_s, ImageTests::whenExecutable<testRopes> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
