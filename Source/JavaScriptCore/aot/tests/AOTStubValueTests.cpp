/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTStubRunner.h"
#include "AOTClosureWord.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeType.h"
#include "AOTSlimClosure.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsClosures.h"
#include "AOTStubsElements.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "FreeList.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"
#include "PureNaN.h"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubValueTests();

namespace {

constexpr uint64_t stringHeader = 0x0102030400000011ULL;
constexpr size_t sizeOfRope = 4 * sizeof(uint64_t);
constexpr uint64_t secretOfFreeList = 0x1234567890abcdefULL;

struct alignas(16) FakeFlatString {
    explicit FakeFlatString(StringImpl& impl)
        : fiber(std::bit_cast<uintptr_t>(&impl))
    {
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

    uint32_t structureID { 0 };
    uint8_t indexingTypeAndMisc { 0 };
    uint8_t type { StringType };
    uint8_t flags { 0 };
    uint8_t cellState { 0 };
    uintptr_t fiber;
};
static_assert(OBJECT_OFFSETOF(FakeFlatString, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeFlatString, fiber) == JSString::offsetOfValue());

struct RopeAllocation {
    RopeAllocation()
    {
        allocator.at<uint64_t>(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret()) = secretOfFreeList;
        runner.instance().prefix().ropeStringAllocator = &allocator;
        runner.instance().prefix().stringHeader = stringHeader;
    }

    void leaveOnlyNextInterval()
    {
        room.bytes.fill(0);
        room.at<uint64_t>(FreeCell::offsetOfScrambledBits()) = (static_cast<uint64_t>(sizeof(room.bytes)) << 32 | FakeAllocator::noNextInterval) ^ secretOfFreeList;
        allocator.makeRoomAt(0, 0);
        allocator.nextInterval() = room.address();
    }

    bool tookFirstCellOfNextInterval()
    {
        return allocator.intervalStart() == room.address() + sizeOfRope && allocator.intervalEnd() == room.address() + sizeof(room.bytes) && allocator.nextInterval() == room.address() + FakeAllocator::noNextInterval;
    }

    uint64_t word(unsigned index) { return room.at<uint64_t>(index * sizeof(uint64_t)); }

    StubRunner runner;
    FakeAllocator allocator { sizeOfRope };
    FakeObject<2 * sizeOfRope> room;
};

uint64_t bitsOfDouble(double number) { return static_cast<uint64_t>(JSValue::encode(JSValue(JSValue::EncodeAsDouble, number))); }

} // anonymous namespace

static void testRopesFromEveryStateOfTheFreeList()
{
    using CompactFibers = JSRopeString::CompactFibers;
    RopeAllocation ropes;
    Ref<StringImpl> firstText = StringImpl::create("ab"_span);
    Ref<StringImpl> secondText = StringImpl::create("cde"_span);
    FakeFlatString first(firstText.get());
    FakeFlatString second(secondText.get());
    auto isRopeOfBoth = [&] {
        return ropes.word(0) == stringHeader && ropes.word(1) == (first.address() | JSString::isRopeInPointer | JSRopeString::is8BitInPointer)
            && ropes.word(2) == (second.address() << CompactFibers::shiftOfFiber1LowerInLengthWord | 5) && ropes.word(3) == second.address() >> CompactFibers::widthOfFiber1LowerInLengthWord;
    };

    ropes.allocator.makeRoomAt(ropes.room.address(), sizeof(ropes.room.bytes));
    StubRun run = ropes.runner.run(concatenate(2), { first.address(), second.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isRopeOfBoth());
    AOT_TEST_CHECK(ropes.allocator.intervalStart() == ropes.room.address() + sizeOfRope);

    ropes.leaveOnlyNextInterval();
    run = ropes.runner.run(concatenate(2), { first.address(), second.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isRopeOfBoth() && ropes.tookFirstCellOfNextInterval());

    ropes.allocator.makeRoomAt(0, 0);
    ropes.allocator.nextInterval() = FakeAllocator::noNextInterval;
    ropes.runner.recordCallsOf(Entry::operationAOTConcat);
    run = ropes.runner.run(concatenate(2), { first.address(), second.address() });
    ropes.runner.countCallsOf(Entry::operationAOTConcat);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == first.address() && run.recordedArguments[1] == second.address());
}

static void testSubstringsFromEveryStateOfTheFreeList()
{
    using CompactFibers = JSRopeString::CompactFibers;
    RopeAllocation ropes;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeFlatString string(text.get());
    auto isSubstring = [&] {
        return ropes.word(0) == stringHeader && ropes.word(1) == (JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | JSRopeString::is8BitInPointer)
            && ropes.word(2) == (string.address() << CompactFibers::shiftOfFiber1LowerInLengthWord | 3)
            && ropes.word(3) == (string.address() >> CompactFibers::widthOfFiber1LowerInLengthWord | 2ull << CompactFibers::shiftOfFiber2InUpperWord);
    };

    ropes.allocator.makeRoomAt(ropes.room.address(), sizeof(ropes.room.bytes));
    StubRun run = ropes.runner.run(substring(), { string.address(), 2, 5 });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isSubstring());

    ropes.leaveOnlyNextInterval();
    run = ropes.runner.run(substring(), { string.address(), 2, 5 });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && isSubstring() && ropes.tookFirstCellOfNextInterval());

    ropes.allocator.makeRoomAt(0, 0);
    ropes.allocator.nextInterval() = FakeAllocator::noNextInterval;
    AOT_TEST_CHECK(ropes.runner.run(substring(), { string.address(), 2, 5 }).calledOperation);
}

static void testJoinedTextCharacter()
{
    RopeAllocation ropes;
    std::array<uint64_t, maxSingleCharacterString + 1> singleCharacterStrings;
    std::iota(singleCharacterStrings.begin(), singleCharacterStrings.end(), 1);
    ropes.runner.instance().singleCharacterStrings = singleCharacterStrings.data();
    Ref<StringImpl> firstText = StringImpl::create("ab"_span);
    Ref<StringImpl> secondText = StringImpl::create("cde"_span);
    FakeFlatString first(firstText.get());
    FakeFlatString second(secondText.get());
    ropes.allocator.makeRoomAt(ropes.room.address(), sizeof(ropes.room.bytes));
    uint64_t joined = ropes.runner.run(concatenate(2), { first.address(), second.address() }).result;
    constexpr std::string_view letters = "abcde";
    for (uint64_t position = 0; position < letters.size(); ++position) {
        StubRun run = ropes.runner.run(substring(), { joined, position, position + 1 });
        AOT_TEST_CHECK(!run.calledOperation && run.result == singleCharacterStrings[letters[position]]);
    }
    Ref<StringImpl> longText = StringImpl::create("vwxyz"_span);
    FakeFlatString whole(longText.get());
    uint64_t piece = ropes.runner.run(substring(), { whole.address(), 1, 5 }).result;
    StubRun run = ropes.runner.run(substring(), { piece, 2, 3 });
    AOT_TEST_CHECK(!run.calledOperation && run.result == singleCharacterStrings['y']);
}

static void testLongPairKeyJoined()
{
    RopeAllocation ropes;
    std::array<Latin1Character, StringKeyCache::maxCachedKeyLength + 1> letters;
    letters.fill('x');
    Ref<StringImpl> longText = StringImpl::create(std::span<const Latin1Character> { letters });
    Ref<StringImpl> shortText = StringImpl::create("ab"_span);
    FakeFlatString first(longText.get());
    FakeFlatString second(shortText.get());
    StubKey stub = concatenateKeyOfPair(ConcatenatedKeyForm::Name);
    ropes.allocator.makeRoomAt(ropes.room.address(), sizeof(ropes.room.bytes));
    StubRun run = ropes.runner.run(stub, { first.address(), second.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == ropes.room.address() && ropes.word(1) == (first.address() | JSString::isRopeInPointer | JSRopeString::is8BitInPointer));
    AOT_TEST_CHECK(ropes.runner.run(stub, { second.address(), second.address() }).calledOperation);
}

static void testTruthinessOfDoubles()
{
    StubCatalog catalog(StubOptions { });
    for (StubKey key : { toBoolean(), valueIsTruthy() }) {
        for (FPRReg marked : markedFPRs)
            AOT_TEST_CHECK(!containsRegister(catalog.summaryOf(key).clobbered, marked));
    }

    StubRunner runner;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    for (double truthy : { 0.5, -1.5, 1e300, std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(), infinity, -infinity }) {
        StubRun run = runner.run(toBoolean(), { bitsOfDouble(truthy) });
        AOT_TEST_CHECK(!run.calledOperation && run.result == 1);
    }
    for (double falsy : { 0.0, -0.0, PNaN, -PNaN }) {
        StubRun run = runner.run(toBoolean(), { bitsOfDouble(falsy) });
        AOT_TEST_CHECK(!run.calledOperation && !run.result);
    }
}

static void testKindsThatPassTheCheckOfAProof()
{
    namespace Primitive = Typed::Primitive;
    struct alignas(16) FakeCell {
        uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

        uint32_t structureID { 0 };
        uint8_t indexingTypeAndMisc { 0 };
        uint8_t type;
        uint8_t flags { 0 };
        uint8_t cellState { 0 };
    };
    static_assert(OBJECT_OFFSETOF(FakeCell, type) == JSCell::typeInfoTypeOffset());
    auto bitsOf = [](JSValue value) {
        return static_cast<uint64_t>(JSValue::encode(value));
    };
    FakeCell string { .type = StringType };
    FakeCell symbol { .type = SymbolType };
    FakeCell bigInt { .type = HeapBigIntType };
    FakeCell firstObject { .type = ObjectType };
    FakeCell lastObject { .type = UINT8_MAX };
    const std::pair<uint64_t, uint32_t> values[] = {
        { bitsOf(jsUndefined()), Primitive::Undefined },
        { bitsOf(jsNull()), Primitive::Null },
        { bitsOf(jsBoolean(true)), Primitive::True },
        { bitsOf(jsBoolean(false)), Primitive::False },
        { bitsOf(jsNumber(0)), KindsOfProof::everyNumber },
        { bitsOf(jsNumber(-1)), KindsOfProof::everyNumber },
        { bitsOfDouble(0.5), KindsOfProof::everyNumber },
        { bitsOfDouble(PNaN), KindsOfProof::everyNumber },
        { bitsOf(JSValue()), Primitive::Absent },
        { string.address(), Primitive::String },
        { symbol.address(), Primitive::Symbol },
        { bigInt.address(), Primitive::BigInt },
        { firstObject.address(), KindsOfProof::everyObject },
        { lastObject.address(), KindsOfProof::everyObject },
    };

    auto wordsWith = [](uint32_t kinds) {
        return KindsOfProof::withSource(kinds, Typed::ProofSource::Lifetime) | static_cast<uint64_t>(UINT32_MAX) << KindsOfProof::shiftOfNumber;
    };

    StubCatalog catalog(StubOptions { });
    StubRunner runner;
    forEachRegisterVariant(checkProofOfKinds(), [&](StubKey key) {
        for (GPRReg reg : allocatableGPRs)
            AOT_TEST_CHECK(isARM64() || !containsRegister(catalog.summaryOf(key).clobbered, reg));
        for (auto [value, kind] : values) {
            for (uint32_t kinds : { kind, KindsOfProof::all })
                AOT_TEST_CHECK(!runner.run(key, { value, wordsWith(kinds) }).calledOperation);
        }
    });

    FakeCell cellOfNoKind { .type = CellType };
    FakeCell structure { .type = StructureType };
    FakeCell lastCellThatIsNoObject { .type = ObjectType - 1 };
    static_assert(SymbolType < ObjectType - 1);
    runner.countWordsNobodyWroteAndLeaveStubAtCallsOf(Entry::operationAOTThrowAtSite);
    forEachRegisterVariant(checkProofOfKinds(), [&](StubKey key) {
        for (auto [value, kind] : values) {
            for (uint32_t kinds : { KindsOfProof::all & ~kind, 0u })
                AOT_TEST_CHECK(runner.run(key, { value, wordsWith(kinds) }).calledOperation);
            AOT_TEST_CHECK(!runner.run(key, { value, wordsWith(kind) }).calledOperation);
        }
        for (uint64_t wordThatIsNoValue : { cellOfNoKind.address(), structure.address(), lastCellThatIsNoObject.address(), static_cast<uint64_t>(JSValue::ValueNull | 1), static_cast<uint64_t>(JSValue::ValueUndefined | 0x10) })
            AOT_TEST_CHECK(runner.run(key, { wordThatIsNoValue, wordsWith(KindsOfProof::all) }).calledOperation);
    });
}

static void testClosuresThatPassTheCheckOfAProof()
{
    namespace Primitive = Typed::Primitive;
    struct alignas(16) FakeClosure {
        uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

        uint32_t structureID { 0 };
        uint8_t indexingTypeAndMisc { 0 };
        uint8_t type;
        uint8_t flags { 0 };
        uint8_t cellState { 0 };
        uint64_t word;
    };
    static_assert(OBJECT_OFFSETOF(FakeClosure, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeClosure, word) == offsetOfSlimClosureWord);
    constexpr uint32_t number = 77;
    constexpr uint32_t upperHalf = ClosureWord::encodeUpperHalf(2, number, { });
    static_assert((upperHalf & ClosureWord::functionMask) == number && upperHalf != number);
    alignas(8) std::array<uint8_t, 128> storage { };
    uint8_t* butterfly = storage.data() + storage.size() / 2;
    memcpy(butterfly + ReifiedSlimClosure::offsetOfFunctionIndexInButterfly(), &number, sizeof(number));
    FakeClosure asCreated { .type = SlimClosureType, .word = static_cast<uint64_t>(upperHalf) << 32 | 5 };
    FakeClosure changed { .type = SlimClosureWithPropertiesType, .word = std::bit_cast<uintptr_t>(butterfly) };
    auto wordsWith = [&](uint32_t kinds) {
        return KindsOfProof::withSource(kinds, Typed::ProofSource::Variable) | static_cast<uint64_t>(upperHalf) << KindsOfProof::shiftOfNumber;
    };

    StubRunner runner;
    forEachRegisterVariant(checkProofOfClosure(), [&](StubKey key) {
        for (uint64_t closure : { asCreated.address(), changed.address() })
            AOT_TEST_CHECK(!runner.run(key, { closure, wordsWith(0) }).calledOperation);
        AOT_TEST_CHECK(!runner.run(key, { static_cast<uint64_t>(JSValue::encode(JSValue())), wordsWith(Primitive::Absent) }).calledOperation);
        AOT_TEST_CHECK(!runner.run(key, { static_cast<uint64_t>(JSValue::encode(jsUndefined())), wordsWith(Primitive::Undefined) }).calledOperation);
    });

    constexpr uint32_t otherNumber = number + 1;
    alignas(8) std::array<uint8_t, 128> otherStorage { };
    uint8_t* otherButterfly = otherStorage.data() + otherStorage.size() / 2;
    memcpy(otherButterfly + ReifiedSlimClosure::offsetOfFunctionIndexInButterfly(), &otherNumber, sizeof(otherNumber));
    FakeClosure ofOtherFunction { .type = SlimClosureType, .word = static_cast<uint64_t>(ClosureWord::encodeUpperHalf(2, otherNumber, { })) << 32 | 5 };
    FakeClosure withOtherFlags { .type = SlimClosureType, .word = static_cast<uint64_t>(ClosureWord::encodeUpperHalf(3, number, { })) << 32 | 5 };
    FakeClosure changedOfOtherFunction { .type = SlimClosureWithPropertiesType, .word = std::bit_cast<uintptr_t>(otherButterfly) };
    FakeClosure plainObject { .type = FinalObjectType, .word = 0 };
    FakeClosure cellOfNoKindWithTheWord { .type = CellType, .word = asCreated.word };
    FakeClosure textWithTheWord { .type = StringType, .word = asCreated.word };
    runner.countWordsNobodyWroteAndLeaveStubAtCallsOf(Entry::operationAOTThrowAtSite);
    forEachRegisterVariant(checkProofOfClosure(), [&](StubKey key) {
        for (const FakeClosure* other : { &ofOtherFunction, &withOtherFlags, &changedOfOtherFunction, &plainObject, &cellOfNoKindWithTheWord, &textWithTheWord })
            AOT_TEST_CHECK(runner.run(key, { other->address(), wordsWith(KindsOfProof::primitives & ~Primitive::String) }).calledOperation);
        AOT_TEST_CHECK(!runner.run(key, { textWithTheWord.address(), wordsWith(Primitive::String) }).calledOperation);
        AOT_TEST_CHECK(runner.run(key, { static_cast<uint64_t>(JSValue::encode(JSValue())), wordsWith(KindsOfProof::primitives & ~Primitive::Absent) }).calledOperation);
        AOT_TEST_CHECK(runner.run(key, { static_cast<uint64_t>(JSValue::encode(jsNull())), wordsWith(Primitive::Undefined) }).calledOperation);
        for (uint64_t closure : { asCreated.address(), changed.address() })
            AOT_TEST_CHECK(!runner.run(key, { closure, wordsWith(0) }).calledOperation);
    });

    struct alignas(16) FakeFullClosure {
        uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

        uint32_t structureID { 0 };
        uint8_t indexingTypeAndMisc { 0 };
        uint8_t type { CompiledClosureType };
        uint8_t flags { 0 };
        uint8_t cellState { 0 };
        std::array<uint64_t, 7> words { };
    };
    static_assert(offsetOfClosureEntry % sizeof(uint64_t) == 0 && offsetOfClosureEntry < sizeof(FakeFullClosure));
    auto fullClosureOf = [](uint32_t closureNumber) {
        FakeFullClosure closure;
        closure.words[offsetOfClosureEntry / sizeof(uint64_t) - 1] = static_cast<uint64_t>(ClosureWord::encodeUpperHalf(0, closureNumber, { })) << 32 | 9;
        return closure;
    };
    FakeFullClosure full = fullClosureOf(number);
    FakeFullClosure fullOfOtherFunction = fullClosureOf(otherNumber);
    forEachRegisterVariant(checkProofOfClosure(), [&](StubKey key) {
        AOT_TEST_CHECK(!runner.run(key, { full.address(), wordsWith(0) }).calledOperation);
        AOT_TEST_CHECK(runner.run(key, { fullOfOtherFunction.address(), wordsWith(KindsOfProof::primitives) }).calledOperation);
    });
    JSGlobalObject* realm = std::exchange(runner.instance().globalObject, std::bit_cast<JSGlobalObject*>(static_cast<uintptr_t>(16)));
    forEachRegisterVariant(checkProofOfClosure(), [&](StubKey key) {
        AOT_TEST_CHECK(runner.run(key, { full.address(), wordsWith(KindsOfProof::primitives) }).calledOperation);
        AOT_TEST_CHECK(!runner.run(key, { asCreated.address(), wordsWith(0) }).calledOperation);
    });
    runner.instance().globalObject = realm;
}

static void testTypesInTablesThatPassTheCheckOfAProof()
{
    namespace Primitive = Typed::Primitive;
    struct alignas(16) FakeCell {
        uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

        uint32_t structureID { 0 };
        uint8_t indexingTypeAndMisc { 0 };
        uint8_t type;
        uint8_t flags { 0 };
        uint8_t cellState { 0 };
    };
    static_assert(OBJECT_OFFSETOF(FakeCell, type) == JSCell::typeInfoTypeOffset());
    auto bitsOf = [](JSValue value) {
        return static_cast<uint64_t>(JSValue::encode(value));
    };

    constexpr uint32_t zeroOrWhole = 64;
    constexpr uint32_t fromOneToNine = 128;
    constexpr uint32_t listOrNumber = 192;
    alignas(8) std::array<uint8_t, 256> tables { };
    auto lay = [&](uint32_t offset, const auto& record) {
        memcpy(tables.data() + offset, &record, sizeof(record));
    };
    lay(zeroOrWhole, RuntimeType { Primitive::Undefined | Primitive::False | Primitive::Integer | Primitive::WholeNumber | Primitive::String, RuntimeType::hasIntegerRange, 0, 0, 0 });
    lay(zeroOrWhole + sizeof(RuntimeType), std::array<int64_t, 2> { 0, 0 });
    lay(fromOneToNine, RuntimeType { Primitive::Integer, RuntimeType::hasIntegerRange, 0, 0, 0 });
    lay(fromOneToNine + sizeof(RuntimeType), std::array<int64_t, 2> { 1, 9 });
    lay(listOrNumber, RuntimeType { Primitive::Number, 0, 1, 1, 0 });
    lay(listOrNumber + sizeof(RuntimeType), RuntimeCellTest::cellTypeIs(ArrayType, DerivedArrayType));

    FakeCell string { .type = StringType };
    FakeCell symbol { .type = SymbolType };
    FakeCell list { .type = ArrayType };
    FakeCell derivedList { .type = DerivedArrayType };
    FakeCell plainObject { .type = FinalObjectType };
    FakeCell cellOfNoKind { .type = CellType };
    struct Case {
        uint32_t type;
        uint32_t kinds;
        Vector<uint64_t> passInStub;
        Vector<uint64_t> areLeftToRunTime;
    };
    const Case cases[] = {
        { zeroOrWhole, Primitive::Undefined | Primitive::False | Primitive::String,
            { bitsOf(jsUndefined()), bitsOf(jsBoolean(false)), string.address(), bitsOf(jsNumber(0)) },
            { bitsOf(jsNumber(1)), bitsOf(jsNumber(-1)), bitsOfDouble(-0.0), bitsOfDouble(0.5), bitsOfDouble(PNaN), bitsOf(jsNull()), bitsOf(jsBoolean(true)), bitsOf(JSValue()), symbol.address(), plainObject.address(), cellOfNoKind.address(), static_cast<uint64_t>(JSValue::ValueNull | 1) } },
        { zeroOrWhole, Primitive::Undefined | Primitive::False | Primitive::String | Primitive::Absent,
            { bitsOf(JSValue()) },
            { } },
        { fromOneToNine, 0,
            { bitsOf(jsNumber(1)), bitsOf(jsNumber(9)) },
            { bitsOf(jsNumber(0)), bitsOf(jsNumber(10)), bitsOf(jsNumber(-1)), bitsOfDouble(1.5), bitsOf(jsUndefined()), string.address() } },
        { listOrNumber, KindsOfProof::everyNumber,
            { bitsOf(jsNumber(7)), bitsOfDouble(0.5), list.address(), derivedList.address() },
            { plainObject.address(), string.address(), cellOfNoKind.address(), bitsOf(jsUndefined()), bitsOf(JSValue()) } },
    };

    StubCatalog catalog(StubOptions { });
    StubRunner runner;
    runner.instance().image.tables = reinterpret_cast<const ProgramTables*>(tables.data());
    runner.recordCallsOf(Entry::operationAOTStopUnlessValueHasType);
    forEachRegisterVariant(checkProofOfType(), [&](StubKey key) {
        for (GPRReg reg : allocatableGPRs)
            AOT_TEST_CHECK(!containsRegister(catalog.summaryOf(key).clobbered, reg));
        for (const Case& tried : cases) {
            uint64_t words = KindsOfProof::withSource(tried.kinds, Typed::ProofSource::Contents);
            for (uint64_t value : tried.passInStub)
                AOT_TEST_CHECK(!runner.run(key, { value, words, tried.type }).calledOperation);
            for (uint64_t value : tried.areLeftToRunTime) {
                StubRun run = runner.run(key, { value, words, tried.type });
                AOT_TEST_CHECK(run.calledOperation && run.recordedArguments[0] == words && run.recordedArguments[1] == tried.type && run.recordedArguments[2] == value);
            }
        }
    });
}

static void testLooseEqualityOfPrimitives()
{
    StubRunner runner;
    auto bitsOf = [](JSValue value) {
        return static_cast<uint64_t>(JSValue::encode(value));
    };
    const uint64_t primitives[] = {
        bitsOf(jsNumber(0)), bitsOf(jsNumber(1)), bitsOf(jsNumber(-1)), bitsOfDouble(0.0), bitsOfDouble(-0.0), bitsOfDouble(1.0), bitsOfDouble(1.5), bitsOfDouble(PNaN), bitsOfDouble(-PNaN),
        bitsOf(jsUndefined()), bitsOf(jsNull()), bitsOf(jsBoolean(true)), bitsOf(jsBoolean(false)),
    };
    auto numberOf = [](JSValue value) {
        return value.isNumber() ? value.asNumber() : value.isTrue() ? 1 : 0;
    };
    for (uint64_t left : primitives) {
        for (uint64_t right : primitives) {
            JSValue leftValue = JSValue::decode(static_cast<EncodedJSValue>(left));
            JSValue rightValue = JSValue::decode(static_cast<EncodedJSValue>(right));
            const bool areEqual = leftValue.isUndefinedOrNull() || rightValue.isUndefinedOrNull() ? leftValue.isUndefinedOrNull() && rightValue.isUndefinedOrNull() : numberOf(leftValue) == numberOf(rightValue);
            StubRun run = runner.run(equal(false), { left, right });
            AOT_TEST_CHECK(!run.calledOperation && run.result == static_cast<uint64_t>(areEqual));
        }
    }

    Ref<StringImpl> text = StringImpl::create("ab"_span);
    FakeFlatString cell(text.get());
    FakeFlatString object(text.get());
    object.type = FinalObjectType;
    for (uint64_t nullish : { bitsOf(jsUndefined()), bitsOf(jsNull()) }) {
        AOT_TEST_CHECK(!runner.run(equal(false), { cell.address(), nullish }).calledOperation && !runner.run(equal(false), { nullish, cell.address() }).result);
        AOT_TEST_CHECK(runner.run(equal(false), { object.address(), nullish }).calledOperation && runner.run(equal(false), { nullish, object.address() }).calledOperation);
    }
    for (uint64_t number : { bitsOf(jsNumber(1)), bitsOfDouble(1.5), bitsOf(jsBoolean(true)) })
        AOT_TEST_CHECK(runner.run(equal(false), { cell.address(), number }).calledOperation && runner.run(equal(false), { number, cell.address() }).calledOperation);
}

static void testStrictEqualityInBothForms()
{
    StubRunner runner;
    auto check = [&](uint64_t left, uint64_t right, bool areEqual) {
        for (StubKey key : { equal(true), strictEqualInFlags() }) {
            StubRun run = runner.run(key, { left, right });
            AOT_TEST_CHECK(!run.calledOperation && run.result == static_cast<uint64_t>(areEqual));
        }
    };
    auto bitsOf = [](JSValue value) {
        return static_cast<uint64_t>(JSValue::encode(value));
    };

    const uint64_t primitives[] = {
        bitsOf(jsNumber(0)), bitsOf(jsNumber(1)), bitsOf(jsNumber(-1)), bitsOfDouble(0.0), bitsOfDouble(-0.0), bitsOfDouble(1.0), bitsOfDouble(-1.0), bitsOfDouble(1.5), bitsOfDouble(PNaN), bitsOfDouble(-PNaN),
        bitsOf(jsUndefined()), bitsOf(jsNull()), bitsOf(jsBoolean(true)), bitsOf(jsBoolean(false)),
    };
    for (uint64_t left : primitives) {
        for (uint64_t right : primitives) {
            JSValue leftValue = JSValue::decode(static_cast<EncodedJSValue>(left));
            JSValue rightValue = JSValue::decode(static_cast<EncodedJSValue>(right));
            check(left, right, leftValue.isNumber() && rightValue.isNumber() ? leftValue.asNumber() == rightValue.asNumber() : left == right);
        }
    }

    Ref<StringImpl> text = StringImpl::create("ab"_span);
    auto cellOfType = [&](JSType type) {
        FakeFlatString cell(text.get());
        cell.type = type;
        return cell;
    };
    const FakeFlatString cells[] = { cellOfType(StringType), cellOfType(HeapBigIntType), cellOfType(SymbolType), cellOfType(SymbolType), cellOfType(ObjectType), cellOfType(ObjectType), cellOfType(JSFunctionType) };
    for (const FakeFlatString& left : cells) {
        for (const FakeFlatString& right : cells)
            check(left.address(), right.address(), &left == &right);
        for (uint64_t primitive : primitives) {
            check(left.address(), primitive, false);
            check(primitive, left.address(), false);
        }
    }

    const FakeBigInt zero({ });
    const FakeBigInt otherZero({ });
    const FakeBigInt five({ 5 });
    const FakeBigInt otherFive({ 5 });
    const FakeBigInt six({ 6 });
    const FakeBigInt negativeFive({ 5 }, true);
    const FakeBigInt fiveAndHighDigit({ 5, 1 });
    const FakeBigInt sixAndHighDigit({ 6, 1 });
    check(zero.address(), otherZero.address(), true);
    check(five.address(), otherFive.address(), true);
    check(five.address(), zero.address(), false);
    check(five.address(), six.address(), false);
    check(five.address(), negativeFive.address(), false);
    check(five.address(), fiveAndHighDigit.address(), false);
    check(fiveAndHighDigit.address(), sixAndHighDigit.address(), false);

    FakeBigInt bigInt({ 5, 1 });
    FakeBigInt otherBigInt({ 5, 2 });
    runner.recordCallsOf(Entry::operationAOTCompareStrictEq);
    for (uint64_t answer : { 0, 1 }) {
        runner.setResultOfRecordedOperation(answer);
        for (StubKey key : { equal(true), strictEqualInFlags() }) {
            StubRun run = runner.run(key, { bigInt.address(), otherBigInt.address() });
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == answer);
            AOT_TEST_CHECK(run.recordedArguments[0] == otherBigInt.address() && run.recordedArguments[1] == bigInt.address());
        }
    }
    runner.countCallsOf(Entry::operationAOTCompareStrictEq);
}

static void testMovesOfFewBytes()
{
    StubCatalog catalog(StubOptions { });
    RegisterSet lost = catalog.summaryOf(moveFewBytes()).clobbered;
    lost.exclude(RegisterSet(gprOf(StubRegister::T9), gprOf(StubRegister::T10)));
    lost.exclude(RegisterSet::macroClobberedGPRs());
    lost.exclude(RegisterSet::macroClobberedFPRs());
    AOT_TEST_CHECK(lost.isEmpty() && catalog.isLeaf(moveFewBytes()));

    StubRunner runner;
    constexpr int maxCount = maxBytesMovedByStub;
    for (int count = 1; count <= maxCount; ++count) {
        for (int distance = -count; distance <= count; ++distance) {
            std::array<uint8_t, 3 * maxCount> bytes;
            std::iota(bytes.begin(), bytes.end(), 1);
            std::array<uint8_t, 3 * maxCount> expected = bytes;
            std::memmove(expected.data() + maxCount + distance, expected.data() + maxCount, count);
            auto addressAt = [&](int offset) -> uint64_t {
                return std::bit_cast<uintptr_t>(bytes.data() + offset);
            };
            StubRun run = runner.run(moveFewBytes(), { addressAt(maxCount + distance), addressAt(maxCount), static_cast<uint64_t>(count) });
            AOT_TEST_CHECK(!run.calledOperation && bytes == expected);
        }
    }
}

static void testShiftArrayStart()
{
    constexpr size_t numberOfElements = 5;
    constexpr uint16_t noProperties = std::numeric_limits<uint16_t>::max() - 1;
    StubRunner runner;
    runner.recordCallsOf(Entry::operationAOTShiftArrayStart);
    FakeObject<sizeof(Structure)> structure;
    runner.instance().prefix().structureIDBase = structure.address() & ~static_cast<uint64_t>(UINT32_MAX);
    FakeObject<sizeof(JSObjectWithButterfly)> array;
    array.setHeader(static_cast<uint32_t>(structure.address()), ArrayType);
    std::array<uint64_t, 2 + numberOfElements> words;
    auto runWith = [&](uint32_t bits, uint16_t maxOffset) {
        structure.at<uint32_t>(Structure::bitFieldOffset()) = bits;
        structure.at<uint16_t>(Structure::maxOffsetOffset()) = maxOffset;
        words = { 3, uint64_t { 9 } << 32 | numberOfElements, 11, 12, 13, 14, 15 };
        array.at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(words.data() + 2);
        return runner.run(shiftArrayStart(), { array.address() });
    };
    StubRun moved = runWith(Structure::s_hasElementsStartOffsetBits, noProperties);
    AOT_TEST_CHECK(!moved.calledOperation && array.at<uint64_t>(JSObject::butterflyOffset()) == std::bit_cast<uintptr_t>(words.data() + 3));
    AOT_TEST_CHECK(words[1] == 4 && words[2] == (uint64_t { 8 } << 32 | (numberOfElements - 1)) && words[3] == 12 && words[6] == 15);

    auto isLeftToOperation = [&](uint32_t bits, uint16_t maxOffset) {
        StubRun run = runWith(bits, maxOffset);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == array.address());
        AOT_TEST_CHECK(array.at<uint64_t>(JSObject::butterflyOffset()) == std::bit_cast<uintptr_t>(words.data() + 2) && words[0] == 3 && words[2] == 11);
    };
    isLeftToOperation(0, noProperties);
    isLeftToOperation(~Structure::s_hasElementsStartOffsetBits, noProperties);
    isLeftToOperation(Structure::s_hasElementsStartOffsetBits, firstOutOfLineOffset);
    runner.instance().mutatorShouldBeFenced = true;
    isLeftToOperation(Structure::s_hasElementsStartOffsetBits, noProperties);
}

std::span<const NativeTest> stubValueTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/move-few-bytes/every-length-and-overlap"_s, ImageTests::whenExecutable<testMovesOfFewBytes> },
        { "stubs/shift-array-start/moves-the-start-of-the-elements"_s, ImageTests::whenExecutable<testShiftArrayStart> },
        { "stubs/concatenate/ropes-from-every-state-of-the-free-list"_s, ImageTests::whenExecutable<testRopesFromEveryStateOfTheFreeList> },
        { "stubs/concatenate/long-key-of-pair"_s, ImageTests::whenExecutable<testLongPairKeyJoined> },
        { "stubs/substring/one-character-of-joined-text"_s, ImageTests::whenExecutable<testJoinedTextCharacter> },
        { "stubs/substring/ropes-from-every-state-of-the-free-list"_s, ImageTests::whenExecutable<testSubstringsFromEveryStateOfTheFreeList> },
        { "stubs/to-boolean/doubles"_s, ImageTests::whenExecutable<testTruthinessOfDoubles> },
        { "stubs/check-proof-of-kinds/a-value-passes-where-its-kind-is-among-them-and-nowhere-else"_s, ImageTests::whenExecutable<testKindsThatPassTheCheckOfAProof> },
        { "stubs/check-proof-of-closure/the-closure-asked-for-passes-and-no-other-object"_s, ImageTests::whenExecutable<testClosuresThatPassTheCheckOfAProof> },
        { "stubs/check-proof-of-type/what-the-record-accepts-passes-and-the-rest-is-left-to-the-run-time"_s, ImageTests::whenExecutable<testTypesInTablesThatPassTheCheckOfAProof> },
        { "stubs/loose-equality-of-primitives"_s, ImageTests::whenExecutable<testLooseEqualityOfPrimitives> },
        { "stubs/equal/strict-in-the-register-and-in-the-flags"_s, ImageTests::whenExecutable<testStrictEqualityInBothForms> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
