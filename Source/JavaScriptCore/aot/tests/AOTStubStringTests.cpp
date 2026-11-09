/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "AOTVector.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include "NumericStrings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubStringTests();

namespace {

constexpr unsigned maxLengthComparedByStringStubs = 32;
constexpr unsigned maxTestedLength = maxLengthComparedByStringStubs + 2 * sizeof(uint64_t) + 1;

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
        const size_t unitsInWord = sizeof(uint64_t) / (areBoth8Bit ? sizeof(Latin1Character) : sizeof(char16_t));
        bool endsAreSame = first.size() >= unitsInWord && first.size() == second.size() && std::ranges::equal(first.span().first(unitsInWord), second.span().first(unitsInWord)) && std::ranges::equal(first.span().last(unitsInWord), second.span().last(unitsInWord));
        AOT_TEST_CHECK(equality.numberOfLeafCalls == (comparesCharacters && (!haveSameWidth || (first.size() > maxLengthComparedByStringStubs && endsAreSame))));
        Answer equalityOfDistinctCells = runWithoutOperation(distinctStringsAreEqual(), m_first.address(), m_second.address());
        AOT_TEST_CHECK(equalityOfDistinctCells.result == equality.result && equalityOfDistinctCells.numberOfLeafCalls == equality.numberOfLeafCalls);
        for (bool isStrict : { false, true }) {
            Answer answer = runWithoutOperation(JSC::AOT::equal(isStrict), m_first.address(), m_second.address());
            AOT_TEST_CHECK(answer.result == equality.result && answer.numberOfLeafCalls == equality.numberOfLeafCalls);
            Answer answerOfNarrowEntry = runWithoutOperation(equalOfNullableStrings(isStrict), m_first.address(), m_second.address());
            AOT_TEST_CHECK(answerOfNarrowEntry.result == equality.result && answerOfNarrowEntry.numberOfLeafCalls == equality.numberOfLeafCalls);
        }
        Answer answerInFlags = runWithoutOperation(strictEqualInFlags(), m_first.address(), m_second.address());
        AOT_TEST_CHECK(answerInFlags.result == equality.result && answerInFlags.numberOfLeafCalls == equality.numberOfLeafCalls);

        const bool orderNeedsLeaf = !areBoth8Bit || std::min(first.size(), second.size()) > maxLengthComparedByStringStubs;
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

static Units textOfLength(unsigned length, char16_t highBits)
{
    Units units;
    for (unsigned i = 0; i < length; ++i)
        units.append(highBits | (0x7f + i % 3));
    return units;
}

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

static void testDifferenceInOneUnitOnly()
{
    ComparisonStubs stubs;
    for (unsigned length = 1; length <= maxTestedLength; ++length) {
        for (char16_t highBits : { 0, 0x300 }) {
            Units text = textOfLength(length, highBits);
            for (unsigned position = 0; position < length; ++position) {
                Units other = text;
                ++other[position];
                stubs.checkFlatStrings(text, other);
                stubs.checkFlatStrings(other, text);
            }
        }
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
    }

    ComparisonStubs::Answer equality = stubs.runWithoutOperation(stringsAreEqual(), cell.address(), cellOfSameImpl.address());
    AOT_TEST_CHECK(equality.result == 1 && !equality.numberOfLeafCalls);
}

static void testNullableStrings()
{
    ComparisonStubs stubs;
    Ref<StringImpl> impl = StringImpl::create("the same characters"_s);
    Ref<StringImpl> otherImpl = StringImpl::create("some other characters"_s);
    FakeString cell = FakeString::flat(impl.get());
    FakeString cellOfSameImpl = FakeString::flat(impl.get());
    FakeString otherCell = FakeString::flat(otherImpl.get());
    FakeString rope = FakeString::rope(impl->length());
    const uint64_t undefined = JSValue::encode(jsUndefined());
    const uint64_t null = JSValue::encode(jsNull());

    struct Case {
        uint64_t first;
        uint64_t second;
        uint64_t isStrictlyEqual;
        uint64_t isLooselyEqual;
    };
    const Case cases[] = {
        { undefined, undefined, 1, 1 }, { null, null, 1, 1 }, { undefined, null, 0, 1 }, { null, undefined, 0, 1 },
        { cell.address(), undefined, 0, 0 }, { undefined, cell.address(), 0, 0 }, { cell.address(), null, 0, 0 }, { null, cell.address(), 0, 0 },
        { rope.address(), undefined, 0, 0 }, { null, rope.address(), 0, 0 },
        { cell.address(), cell.address(), 1, 1 }, { rope.address(), rope.address(), 1, 1 }, { cell.address(), cellOfSameImpl.address(), 1, 1 }, { cell.address(), otherCell.address(), 0, 0 },
    };
    for (const Case& compared : cases) {
        for (bool isStrict : { false, true }) {
            ComparisonStubs::Answer narrow = stubs.runWithoutOperation(equalOfNullableStrings(isStrict), compared.first, compared.second);
            AOT_TEST_CHECK(narrow.result == (isStrict ? compared.isStrictlyEqual : compared.isLooselyEqual) && !narrow.numberOfLeafCalls);
        }
        ComparisonStubs::Answer general = stubs.runWithoutOperation(JSC::AOT::equal(true), compared.first, compared.second);
        AOT_TEST_CHECK(general.result == compared.isStrictlyEqual);
    }
}

static void testAtoms()
{
    ComparisonStubs stubs;
    constexpr ASCIILiteral text = "an atom that is longer than what a stub compares by itself: a"_s;
    constexpr ASCIILiteral otherText = "an atom that is longer than what a stub compared by itself: a"_s;
    static_assert(text.length() == otherText.length() && text.length() > maxLengthComparedByStringStubs);
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

static void checkLeftToOperation(StubRunner& runner, StubKey key, Entry entry, const FakeString& first, const FakeString& second, std::initializer_list<uint64_t> results, bool swapsOperands = false)
{
    runner.recordCallsOf(entry);
    for (uint64_t resultOfOperation : results) {
        runner.setResultOfRecordedOperation(resultOfOperation);
        numberOfLeafCalls = 0;
        StubRun run = runner.run(key, { first.address(), second.address() });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && !numberOfLeafCalls);
        AOT_TEST_CHECK(run.recordedArguments[swapsOperands] == first.address() && run.recordedArguments[!swapsOperands] == second.address());
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
        checkLeftToOperation(stubs.runner(), distinctStringsAreEqual(), Entry::operationAOTStringsAreEqual, *first, *second, { 0, 1 });
        for (StubKey strictEqual : { JSC::AOT::equal(true), strictEqualInFlags(), equalOfNullableStrings(true) })
            checkLeftToOperation(stubs.runner(), strictEqual, Entry::operationAOTStringsAreEqual, *first, *second, { 0, 1 }, true);
    }

    const std::pair<const FakeString*, const FakeString*> pairsOfOtherLength[] = { { &flat, &longerRope }, { &longerRope, &flat }, { &rope, &longerRope }, { &longerRope, &rope } };
    for (auto [first, second] : pairsOfOtherLength) {
        ComparisonStubs::Answer equality = stubs.runWithoutOperation(stringsAreEqual(), first->address(), second->address());
        AOT_TEST_CHECK(!equality.result && !equality.numberOfLeafCalls);
    }
}

namespace {

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
    static JSString* cellWithNumber(uint32_t number) { return std::bit_cast<JSString*>(MarkedBlockCage::decode(0, number)); }
    static constexpr size_t numberOfIntegerKeys = 4;

    void cache(int32_t key, JSString* cell)
    {
        for (auto& entry : m_integers) {
            entry.key = key;
            entry.jsString = cell;
        }
    }

    void enterKey(uint32_t integer, uint32_t numberOfCell)
    {
        m_integerKeys[integer % numberOfIntegerKeys] = static_cast<uint64_t>(integer) << 32 | numberOfCell;
        Instance& instance = m_runner.instance();
        instance.realm.integerKeys = m_integerKeys.data();
        instance.realm.integerKeyMask = numberOfIntegerKeys - 1;
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
    std::array<uint64_t, numberOfIntegerKeys> m_integerKeys { };
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

static void testIntegerKeys()
{
    constexpr uint32_t numberOfCell = 0x300000;
    constexpr int64_t integerInSamePlace = firstIntegerInHashedCache + IntegerStrings::numberOfIntegerKeys;
    IntegerStrings strings;
    strings.cache(static_cast<int32_t>(firstIntegerInHashedCache), IntegerStrings::cachedCell());
    strings.enterKey(static_cast<uint32_t>(firstIntegerInHashedCache), numberOfCell);
    strings.checkIsFound(firstIntegerInHashedCache, IntegerStrings::cellWithNumber(numberOfCell));
    strings.checkIsLeftToOperation(integerInSamePlace);
    strings.enterKey(static_cast<uint32_t>(integerInSamePlace), numberOfCell);
    strings.checkIsFound(firstIntegerInHashedCache, IntegerStrings::cachedCell());
    strings.checkIsFound(7, IntegerStrings::cellOfSmallInteger(7));
    strings.cache(-1, IntegerStrings::cachedCell());
    strings.checkIsFound(-1, IntegerStrings::cachedCell());
}

std::span<const NativeTest> stubStringTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/integer-to-string-cached-integers"_s, ImageTests::whenExecutable<testCachedIntegers> },
        { "stubs/integer-to-string-integers-outside-the-caches"_s, ImageTests::whenExecutable<testIntegersOutsideTheCaches> },
        { "stubs/integer-to-string-integers-beyond-int32"_s, ImageTests::whenExecutable<testIntegersBeyondInt32> },
        { "stubs/integer-to-string-integer-keys"_s, ImageTests::whenExecutable<testIntegerKeys> },
        { "stubs/string-comparison-difference-at-every-position"_s, ImageTests::whenExecutable<testDifferenceAtEveryPosition> },
        { "stubs/string-comparison-difference-in-last-unit-only"_s, ImageTests::whenExecutable<testDifferenceInLastUnitOnly> },
        { "stubs/string-comparison-difference-in-one-unit-only"_s, ImageTests::whenExecutable<testDifferenceInOneUnitOnly> },
        { "stubs/string-comparison-common-prefixes"_s, ImageTests::whenExecutable<testCommonPrefixes> },
        { "stubs/string-comparison-order-of-wide-units"_s, ImageTests::whenExecutable<testOrderOfWideUnits> },
        { "stubs/string-comparison-same-cells-and-impls"_s, ImageTests::whenExecutable<testSameCellsAndImpls> },
        { "stubs/string-comparison-nullable-strings"_s, ImageTests::whenExecutable<testNullableStrings> },
        { "stubs/string-comparison-atoms"_s, ImageTests::whenExecutable<testAtoms> },
        { "stubs/string-comparison-ropes"_s, ImageTests::whenExecutable<testRopes> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
