/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRegExpRecord.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsRegExp.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "Butterfly.h"
#include "CompleteSubspace.h"
#include "JSArray.h"
#include "JSGlobalObject.h"
#include "JSString.h"
#include "MarkedSpace.h"
#include "MatchResult.h"
#include "RegExp.h"
#include "RegExpCachedResult.h"
#include "RegExpGlobalData.h"
#include "RegExpMatchesArray.h"
#include "RegExpObject.h"
#include "YarrFlags.h"
#include "YarrJIT.h"
#include <array>
#include <bit>
#include <memory>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/text/StringImpl.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubRegExpTests();

namespace {

constexpr uint64_t stringHeader = 0x0102030400000011ULL;
constexpr uint64_t matchArrayHeader = 0x0504030200000022ULL;
constexpr uint64_t anyArrayHeader = 0x33;
constexpr unsigned maxGroupsInTests = 3;
constexpr size_t sizeOfRope = 4 * sizeof(uint64_t);
constexpr size_t wordsInFrontOfElements = initialOutOfLineCapacity + 1;
constexpr uint32_t tooManyGroups = MarkedSpace::largeCutoff / sizeof(EncodedJSValue);
constexpr uint32_t structureOfRegExpObjects = 0x40;

uint64_t bitsOfValue(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

struct alignas(16) FakeSubject {
    explicit FakeSubject(StringImpl& impl)
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
static_assert(OBJECT_OFFSETOF(FakeSubject, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeSubject, fiber) == JSString::offsetOfValue());

struct MatchingCodeScript {
    int64_t answer { 0 };
    Vector<int> offsets;
    unsigned numberOfCalls { 0 };
    uint64_t start { 0 };
    uint64_t length { 0 };
    int* output { nullptr };
};

MatchingCodeScript* scriptOfMatchingCode;

UGPRPair SYSV_ABI runMatchingCodeScript(const void*, uint64_t start, uint64_t length, int* output, void*)
{
    MatchingCodeScript& script = *scriptOfMatchingCode;
    ++script.numberOfCalls;
    script.start = start;
    script.length = length;
    script.output = output;
    std::ranges::copy(script.offsets, output);
    return makeUGPRPair(static_cast<UCPURegister>(script.answer), 0);
}

class Matches {
public:
    static constexpr ptrdiff_t cachedResult = JSGlobalObject::regExpGlobalDataOffset() + RegExpGlobalData::offsetOfCachedResult();
    static constexpr ptrdiff_t matchResult = cachedResult + RegExpCachedResult::offsetOfResult();
    static constexpr unsigned kindOfMatchArray = static_cast<unsigned>(InlineAllocation::RegExpMatchesArray);

    explicit Matches(const StubOptions& options = { })
        : runner(options)
        , m_globalObject(makeUniqueWithoutFastMallocCheck<FakeObject<sizeof(JSGlobalObject)>>())
    {
        scriptOfMatchingCode = &script;
        Instance& instance = runner.instance();
        instance.globalObject = reinterpret_cast<JSGlobalObject*>(m_globalObject->bytes.data());
        *reinterpret_cast<int**>(reinterpret_cast<uint8_t*>(instance.vm) + VM::offsetOfAOTState() + VMState::offsetOfRegExpMatchOutput()) = outputOfVM.data();
        for (unsigned unit = 0; unit < m_singleCharacterStrings.size(); ++unit)
            m_singleCharacterStrings[unit] = addressOfSingleCharacterString(unit);
        instance.singleCharacterStrings = m_singleCharacterStrings.data();
        instance.prefix().stringHeader = stringHeader;
        instance.prefix().emptyString = std::bit_cast<JSCell*>(addressOfEmptyString);
        instance.prefix().ropeStringAllocator = &ropeAllocator;
        instance.prefix().newArrayWithContiguousHeader = anyArrayHeader;
        instance.allocation.auxiliarySpace = m_auxiliarySpace.bytes.data();
        instance.allocation.inlineAllocations[kindOfMatchArray] = { &arrayAllocator, matchArrayHeader };
        instance.receiverStructureID(Receiver::RegExp) = structureOfRegExpObjects;
        regExpObject.at<uint32_t>(JSCell::structureIDOffset()) = structureOfRegExpObjects;

        m_codeBlock.at<void*>(Yarr::YarrCodeBlock::offsetOf8BitCode()) = tagCFunctionPtr<void*, Yarr8BitPtrTag>(runMatchingCodeScript);
        regExp.at<void*>(RegExp::offsetOfRegExpJITCode()) = m_codeBlock.bytes.data();
        reset({ }, jsNumber(0));
    }

    void reset(OptionSet<Yarr::Flags> flags, JSValue lastIndex, unsigned numberOfGroups = 0)
    {
        regExp.at<uint16_t>(RegExp::offsetOfFlags()) = flags.toRaw();
        regExp.at<uint8_t>(RegExp::offsetOfState()) = RegExp::stateWithImageCode();
        regExp.at<uint32_t>(RegExp::offsetOfNumSubpatterns()) = numberOfGroups;
        regExp.at<void*>(RegExp::offsetOfRareData()) = nullptr;
        regExp.at<void*>(RegExp::offsetOfAtom()) = nullptr;
        regExpObject.at<uint64_t>(RegExpObject::offsetOfRegExpAndFlags()) = regExp.address();
        regExpObject.at<uint64_t>(RegExpObject::offsetOfLastIndex()) = bitsOfValue(lastIndex);
        m_globalObject->bytes.fill(0);
        m_globalObject->at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
        recorded<uint8_t>(cachedResult + RegExpCachedResult::offsetOfReified()) = 1;
        recorded<uint8_t>(cachedResult + RegExpCachedResult::offsetOfOneCharacterMatch()) = 1;
        outputOfVM.fill(-2);
        script = { };
    }

    void addWideCode() { m_codeBlock.at<void*>(Yarr::YarrCodeBlock::offsetOf16BitCode()) = tagCFunctionPtr<void*, Yarr8BitPtrTag>(runMatchingCodeScript); }
    void makeLastIndexReadOnly() { regExpObject.at<uint64_t>(RegExpObject::offsetOfRegExpAndFlags()) |= RegExpObject::lastIndexIsNotWritableFlag; }
    void markGlobalObjectBlack() { m_globalObject->at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::PossiblyBlack); }
    void setStorageAllocator(unsigned numberOfGroups, FakeAllocator* allocator)
    {
        size_t words = wordsInFrontOfElements + numberOfGroups + 1;
        m_auxiliarySpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + (words + 1) / 2 * sizeof(void*)) = allocator;
    }

    StubRun match(RegExpMatchResult form, const FakeSubject& subject)
    {
        if (takesStringInFrontOfRegExp(form))
            return runner.run(regExpMatch(form), { subject.address(), regExpObject.address() });
        return runner.run(regExpMatch(form), { regExpObject.address(), subject.address() });
    }

    template<typename T> T& recorded(ptrdiff_t offset) { return m_globalObject->at<T>(offset); }
    uint64_t addressOfGlobalObject() const { return m_globalObject->address(); }
    uint64_t lastIndex() { return regExpObject.at<uint64_t>(RegExpObject::offsetOfLastIndex()); }

    bool hasRecorded(const FakeSubject& subject, uint64_t start, uint64_t end)
    {
        return recorded<uint64_t>(cachedResult + RegExpCachedResult::offsetOfLastRegExp()) == regExp.address()
            && recorded<uint64_t>(cachedResult + RegExpCachedResult::offsetOfLastInput()) == subject.address()
            && recorded<uint64_t>(matchResult + OBJECT_OFFSETOF(MatchResult, start)) == start
            && recorded<uint64_t>(matchResult + OBJECT_OFFSETOF(MatchResult, end)) == end
            && !recorded<uint8_t>(cachedResult + RegExpCachedResult::offsetOfReified())
            && !recorded<uint8_t>(cachedResult + RegExpCachedResult::offsetOfOneCharacterMatch());
    }
    bool hasRecordedNothing()
    {
        return !recorded<uint64_t>(cachedResult + RegExpCachedResult::offsetOfLastRegExp()) && recorded<uint8_t>(cachedResult + RegExpCachedResult::offsetOfReified()) == 1;
    }

    static uint64_t addressOfSingleCharacterString(unsigned unit) { return 0x700000 + 16 * unit; }
    static constexpr uint64_t addressOfEmptyString = 0x600000;

    StubRunner runner;
    MatchingCodeScript script;
    FakeObject<sizeof(RegExp)> regExp;
    FakeObject<sizeof(RegExpObject)> regExpObject;
    std::array<int, 2 * (maxGroupsInTests + 1)> outputOfVM;
    FakeAllocator ropeAllocator { sizeOfRope };
    FakeAllocator arrayAllocator { sizeof(JSArray) };

private:
    std::unique_ptr<FakeObject<sizeof(JSGlobalObject)>> m_globalObject;
    FakeObject<sizeof(Yarr::YarrCodeBlock)> m_codeBlock;
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + 16 * sizeof(void*)> m_auxiliarySpace;
    std::array<uint64_t, maxSingleCharacterString + 1> m_singleCharacterStrings;
};

struct TestForm {
    RegExpMatchResult form;
    uint64_t match;
    uint64_t miss;
};

const TestForm formsWithoutArray[] = {
    { RegExpMatchResult::Boolean, 1, 0 },
    { RegExpMatchResult::BoxedBoolean, JSValue::ValueTrue, JSValue::ValueFalse },
};

} // anonymous namespace

static void testMatchesWithoutArray()
{
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());

    for (auto [form, match, miss] : formsWithoutArray) {
        matches.reset({ }, jsNumber(4));
        matches.script.answer = 1;
        matches.script.offsets = { 1, 3 };
        StubRun run = matches.match(form, subject);
        AOT_TEST_CHECK(!run.calledOperation && run.result == match && matches.script.numberOfCalls == 1);
        AOT_TEST_CHECK(!matches.script.start && matches.script.length == text->length());
        AOT_TEST_CHECK(matches.script.output == matches.outputOfVM.data());
        AOT_TEST_CHECK(matches.hasRecorded(subject, 1, 3) && matches.lastIndex() == bitsOfValue(jsNumber(4)));

        matches.reset({ }, jsNumber(4));
        matches.script.answer = -1;
        run = matches.match(form, subject);
        AOT_TEST_CHECK(!run.calledOperation && run.result == miss && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(4)));

        for (Yarr::Flags flag : { Yarr::Flags::Global, Yarr::Flags::Sticky }) {
            matches.reset({ flag }, jsNumber(2));
            matches.script.answer = 3;
            matches.script.offsets = { 3, 5 };
            run = matches.match(form, subject);
            AOT_TEST_CHECK(!run.calledOperation && run.result == match && matches.script.start == 2);
            AOT_TEST_CHECK(matches.hasRecorded(subject, 3, 5) && matches.lastIndex() == bitsOfValue(jsNumber(5)));

            matches.reset({ flag }, jsNumber(2));
            matches.script.answer = -1;
            run = matches.match(form, subject);
            AOT_TEST_CHECK(!run.calledOperation && run.result == miss && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(0)));

            matches.reset({ flag }, jsNumber(2));
            matches.makeLastIndexReadOnly();
            run = matches.match(form, subject);
            AOT_TEST_CHECK(run.calledOperation && !matches.script.numberOfCalls && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(2)));
        }
    }
}

static void testMatchesOfAtoms()
{
    Matches matches;
    matches.runner.install(Entry::operationAOTStringFindCharacter8, operationAOTStringFindCharacter8);
    matches.runner.install(Entry::operationAOTStringFind8, operationAOTStringFind8);
    matches.runner.install(Entry::operationAOTRegExpMatchFlatProgram8, operationAOTRegExpMatchFlatProgram8);
    matches.runner.install(Entry::operationAOTRegExpMatchFlatProgram16, operationAOTRegExpMatchFlatProgram16);
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());

    for (auto [form, match, miss] : formsWithoutArray) {
        for (auto [atomText, start] : { std::pair { "c"_span, 2u }, std::pair { "def"_span, 3u } }) {
            Ref<StringImpl> atom = StringImpl::create(atomText);
            matches.reset({ }, jsNumber(0));
            matches.regExp.at<uint8_t>(RegExp::offsetOfState()) = RegExp::stateWithImageCode() + 1;
            matches.regExp.at<StringImpl*>(RegExp::offsetOfAtom()) = atom.ptr();
            StubRun run = matches.match(form, subject);
            AOT_TEST_CHECK(!run.calledOperation && run.result == match && !matches.script.numberOfCalls);
            AOT_TEST_CHECK(matches.hasRecorded(subject, start, start + atom->length()));
        }
        Ref<StringImpl> absent = StringImpl::create("x"_span);
        matches.reset({ }, jsNumber(0));
        matches.regExp.at<uint8_t>(RegExp::offsetOfState()) = RegExp::stateWithImageCode() + 1;
        matches.regExp.at<StringImpl*>(RegExp::offsetOfAtom()) = absent.ptr();
        StubRun run = matches.match(form, subject);
        AOT_TEST_CHECK(!run.calledOperation && run.result == miss && matches.hasRecordedNothing());
    }
}

static void testBarrierOfRecordedMatch()
{
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());

    for (auto [form, match, miss] : formsWithoutArray) {
        UNUSED_VARIABLE(miss);
        matches.reset({ Yarr::Flags::Global }, jsNumber(0));
        matches.markGlobalObjectBlack();
        matches.script.answer = 0;
        matches.script.offsets = { 0, 6 };
        matches.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
        StubRun run = matches.match(form, subject);
        matches.runner.countCallsOf(Entry::operationAOTBarrierSlow);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == matches.addressOfGlobalObject());
        AOT_TEST_CHECK(run.result == match && matches.hasRecorded(subject, 0, 6) && matches.lastIndex() == bitsOfValue(jsNumber(6)));
    }
}

namespace {

struct MatchArrayMemory {
    static constexpr size_t wordsOfStorage = wordsInFrontOfElements + maxGroupsInTests + 1;

    explicit MatchArrayMemory(Matches& matches, unsigned numberOfGroups)
        : storageAllocator(static_cast<uint32_t>(WTF::roundUpToMultipleOf<2>(wordsInFrontOfElements + numberOfGroups + 1) * sizeof(uint64_t)))
    {
        storage.bytes.fill(0xab);
        storageAllocator.makeRoomAt(storage.address(), sizeof(storage.bytes));
        matches.setStorageAllocator(numberOfGroups, &storageAllocator);
        matches.ropeAllocator.makeRoomAt(ropes.address(), sizeof(ropes.bytes));
        matches.arrayAllocator.makeRoomAt(array.address(), sizeof(array.bytes));
    }

    uint64_t butterfly() const { return storage.address() + wordsInFrontOfElements * sizeof(uint64_t); }
    uint64_t element(unsigned index) { return storage.at<uint64_t>((wordsInFrontOfElements + index) * sizeof(uint64_t)); }
    uint64_t property(PropertyOffset offset) { return storage.at<uint64_t>((wordsInFrontOfElements + offsetInButterfly(offset)) * sizeof(uint64_t)); }
    uint32_t publicLength() { return storage.at<uint32_t>(wordsInFrontOfElements * sizeof(uint64_t) + Butterfly::offsetOfPublicLength()); }
    uint32_t vectorLength() { return storage.at<uint32_t>(wordsInFrontOfElements * sizeof(uint64_t) + Butterfly::offsetOfVectorLength()); }
    uint64_t addressOfRope(unsigned index) const { return ropes.address() + index * sizeOfRope; }

    bool isSubstring(unsigned rope, const FakeSubject& base, uint64_t offset, uint64_t length, bool is8Bit)
    {
        using CompactFibers = JSRopeString::CompactFibers;
        auto word = [&](unsigned index) {
            return ropes.at<uint64_t>(rope * sizeOfRope + index * sizeof(uint64_t));
        };
        uint64_t flags = JSString::isRopeInPointer | JSRopeString::isSubstringInPointer | (is8Bit ? JSRopeString::is8BitInPointer : 0);
        return word(0) == stringHeader && word(1) == flags
            && word(2) == (base.address() << CompactFibers::shiftOfFiber1LowerInLengthWord | length)
            && word(3) == (base.address() >> CompactFibers::widthOfFiber1LowerInLengthWord | offset << CompactFibers::shiftOfFiber2InUpperWord);
    }

    FakeAllocator storageAllocator;
    FakeObject<WTF::roundUpToMultipleOf<2>(wordsOfStorage) * sizeof(uint64_t)> storage;
    FakeObject<(maxGroupsInTests + 1) * sizeOfRope> ropes;
    FakeObject<sizeof(JSArray)> array;
};

} // anonymous namespace

static void checkMatchArray(Matches& matches, MatchArrayMemory& memory, const FakeSubject& subject, const StubRun& run, unsigned numberOfGroups, int index)
{
    AOT_TEST_CHECK(!run.calledOperation && run.result == memory.array.address());
    AOT_TEST_CHECK(memory.array.at<uint64_t>(0) == matchArrayHeader && memory.array.at<uint64_t>(JSObject::butterflyOffset()) == memory.butterfly());
    AOT_TEST_CHECK(memory.publicLength() == numberOfGroups + 1 && memory.vectorLength() == numberOfGroups + 1);
    AOT_TEST_CHECK(memory.property(RegExpMatchesArrayIndexPropertyOffset) == bitsOfValue(jsNumber(index)));
    AOT_TEST_CHECK(memory.property(RegExpMatchesArrayInputPropertyOffset) == subject.address());
    AOT_TEST_CHECK(memory.property(RegExpMatchesArrayGroupsPropertyOffset) == bitsOfValue(jsUndefined()));
    AOT_TEST_CHECK(!memory.property(RegExpMatchesArrayGroupsPropertyOffset + 1));
    AOT_TEST_CHECK(!matches.arrayAllocator.hasRoom());
}

static void testMatchArrays()
{
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());
    {
        matches.reset({ }, jsNumber(9), 3);
        MatchArrayMemory memory(matches, 3);
        matches.script.answer = 1;
        matches.script.offsets = { 1, 5, 2, 3, -1, -1, 4, 4 };
        StubRun run = matches.match(RegExpMatchResult::Array, subject);
        checkMatchArray(matches, memory, subject, run, 3, 1);
        AOT_TEST_CHECK(memory.element(0) == memory.addressOfRope(0) && memory.isSubstring(0, subject, 1, 4, true));
        AOT_TEST_CHECK(memory.element(1) == Matches::addressOfSingleCharacterString('c'));
        AOT_TEST_CHECK(memory.element(2) == bitsOfValue(jsUndefined()));
        AOT_TEST_CHECK(memory.element(3) == Matches::addressOfEmptyString);
        AOT_TEST_CHECK(matches.ropeAllocator.intervalStart() == memory.addressOfRope(1));
        AOT_TEST_CHECK(matches.hasRecorded(subject, 1, 5) && matches.lastIndex() == bitsOfValue(jsNumber(9)));
    }
    {
        matches.reset({ Yarr::Flags::Global }, jsNumber(0), 1);
        MatchArrayMemory memory(matches, 1);
        matches.script.answer = 0;
        matches.script.offsets = { 0, 6, 3, 6 };
        StubRun run = matches.match(RegExpMatchResult::Array, subject);
        checkMatchArray(matches, memory, subject, run, 1, 0);
        AOT_TEST_CHECK(memory.element(0) == subject.address());
        AOT_TEST_CHECK(memory.element(1) == memory.addressOfRope(0) && memory.isSubstring(0, subject, 3, 3, true));
        AOT_TEST_CHECK(matches.hasRecorded(subject, 0, 6) && matches.lastIndex() == bitsOfValue(jsNumber(6)));
    }
    {
        matches.reset({ }, jsNumber(0), 0);
        matches.markGlobalObjectBlack();
        MatchArrayMemory memory(matches, 0);
        matches.script.answer = 2;
        matches.script.offsets = { 2, 4 };
        matches.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
        StubRun run = matches.match(RegExpMatchResult::Array, subject);
        matches.runner.countCallsOf(Entry::operationAOTBarrierSlow);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == matches.addressOfGlobalObject() && run.result == memory.array.address());
        AOT_TEST_CHECK(memory.element(0) == memory.addressOfRope(0) && memory.isSubstring(0, subject, 2, 2, true) && matches.hasRecorded(subject, 2, 4));
    }
}

static void testStringMatchAndSearch()
{
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());
    {
        matches.reset({ }, jsNumber(9), 1);
        MatchArrayMemory memory(matches, 1);
        matches.script.answer = 1;
        matches.script.offsets = { 1, 4, 2, 4 };
        StubRun run = matches.match(RegExpMatchResult::ArrayForStringMatch, subject);
        checkMatchArray(matches, memory, subject, run, 1, 1);
        AOT_TEST_CHECK(!matches.script.start && matches.hasRecorded(subject, 1, 4) && matches.lastIndex() == bitsOfValue(jsNumber(9)));
    }
    {
        matches.reset({ Yarr::Flags::Sticky }, jsNumber(2), 0);
        MatchArrayMemory memory(matches, 0);
        matches.script.answer = 2;
        matches.script.offsets = { 2, 4 };
        StubRun run = matches.match(RegExpMatchResult::ArrayForStringMatch, subject);
        checkMatchArray(matches, memory, subject, run, 0, 2);
        AOT_TEST_CHECK(matches.script.start == 2 && matches.hasRecorded(subject, 2, 4) && matches.lastIndex() == bitsOfValue(jsNumber(4)));
    }
    matches.reset({ }, jsNumber(0));
    matches.script.answer = -1;
    StubRun run = matches.match(RegExpMatchResult::ArrayForStringMatch, subject);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOfValue(jsNull()) && matches.hasRecordedNothing());

    matches.reset({ }, jsNumber(0));
    matches.script.answer = 2;
    matches.script.offsets = { 2, 4 };
    run = matches.match(RegExpMatchResult::IndexForStringSearch, subject);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOfValue(jsNumber(2)) && matches.hasRecorded(subject, 2, 4) && matches.lastIndex() == bitsOfValue(jsNumber(0)));

    matches.reset({ }, jsNumber(0));
    matches.script.answer = -1;
    run = matches.match(RegExpMatchResult::IndexForStringSearch, subject);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOfValue(jsNumber(-1)) && matches.hasRecordedNothing());

    auto isLeftToOperation = [&](RegExpMatchResult form, Entry operation, OptionSet<Yarr::Flags> flags, JSValue lastIndex) {
        matches.reset(flags, lastIndex);
        matches.runner.recordCallsOf(operation);
        matches.runner.setResultOfRecordedOperation(0x1234);
        StubRun left = matches.match(form, subject);
        matches.runner.countCallsOf(operation);
        return left.numberOfRecordedCalls == 1 && left.result == 0x1234 && left.recordedArguments[0] == subject.address() && left.recordedArguments[1] == matches.regExpObject.address()
            && !matches.script.numberOfCalls && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(lastIndex);
    };
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::ArrayForStringMatch, Entry::operationAOTStringMatchRegExp, { Yarr::Flags::Global }, jsNumber(0)));
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::ArrayForStringMatch, Entry::operationAOTStringMatchRegExp, { Yarr::Flags::Global, Yarr::Flags::Sticky }, jsNumber(0)));
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::ArrayForStringMatch, Entry::operationAOTStringMatchRegExp, { }, jsUndefined()));
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::IndexForStringSearch, Entry::operationAOTStringSearchRegExp, { Yarr::Flags::Global }, jsNumber(0)));
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::IndexForStringSearch, Entry::operationAOTStringSearchRegExp, { Yarr::Flags::Sticky }, jsNumber(0)));
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::IndexForStringSearch, Entry::operationAOTStringSearchRegExp, { }, jsNumber(3)));

    matches.regExpObject.at<uint32_t>(JSCell::structureIDOffset()) = structureOfRegExpObjects + 1;
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::ArrayForStringMatch, Entry::operationAOTStringMatchRegExp, { }, jsNumber(0)));
    AOT_TEST_CHECK(isLeftToOperation(RegExpMatchResult::IndexForStringSearch, Entry::operationAOTStringSearchRegExp, { }, jsNumber(0)));
}

static void testNoRecordWhereNobodyReadsIt()
{
    Matches matches(StubOptions { .recordsLastMatch = false });
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());

    for (auto [form, match, miss] : formsWithoutArray) {
        UNUSED_VARIABLE(miss);
        for (bool isGlobal : { false, true }) {
            matches.reset(isGlobal ? OptionSet<Yarr::Flags> { Yarr::Flags::Global } : OptionSet<Yarr::Flags> { }, jsNumber(0));
            matches.script.answer = 1;
            matches.script.offsets = { 1, 3 };
            StubRun run = matches.match(form, subject);
            AOT_TEST_CHECK(!run.calledOperation && run.result == match && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(isGlobal ? 3 : 0)));
        }
    }

    matches.reset({ }, jsNumber(0));
    matches.script.answer = 1;
    matches.script.offsets = { 1, 3 };
    StubRun run = matches.match(RegExpMatchResult::IndexForStringSearch, subject);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOfValue(jsNumber(1)) && matches.hasRecordedNothing());

    matches.reset({ Yarr::Flags::Global }, jsNumber(0), 0);
    MatchArrayMemory memory(matches, 0);
    matches.script.answer = 2;
    matches.script.offsets = { 2, 4 };
    run = matches.match(RegExpMatchResult::Array, subject);
    checkMatchArray(matches, memory, subject, run, 0, 2);
    AOT_TEST_CHECK(matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(4)));
}

static void testWideUnitsInMatchArrays()
{
    Matches matches;
    const char16_t units[] = { 'a', 0x4e2d, 'c', 'd' };
    Ref<StringImpl> text = StringImpl::create(std::span<const char16_t>(units));
    FakeSubject subject(text.get());
    FakeObject<sizeof(Yarr::YarrCodeBlock)> codeBlock;
    codeBlock.at<void*>(Yarr::YarrCodeBlock::offsetOf16BitCode()) = tagCFunctionPtr<void*, Yarr16BitPtrTag>(runMatchingCodeScript);
    matches.regExp.at<void*>(RegExp::offsetOfRegExpJITCode()) = codeBlock.bytes.data();

    matches.reset({ }, jsNumber(0), 2);
    MatchArrayMemory memory(matches, 2);
    matches.script.answer = 0;
    matches.script.offsets = { 0, 3, 1, 2, 2, 3 };
    StubRun run = matches.match(RegExpMatchResult::Array, subject);
    checkMatchArray(matches, memory, subject, run, 2, 0);
    AOT_TEST_CHECK(memory.element(0) == memory.addressOfRope(0) && memory.isSubstring(0, subject, 0, 3, false));
    AOT_TEST_CHECK(memory.element(1) == memory.addressOfRope(1) && memory.isSubstring(1, subject, 1, 1, false));
    AOT_TEST_CHECK(memory.element(2) == Matches::addressOfSingleCharacterString('c'));
}

static void testMatchArraysLeftToOperation()
{
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());
    Instance& instance = matches.runner.instance();

    auto isLeftToOperation = [&](OptionSet<Yarr::Flags> flags, const Invocable<void(MatchArrayMemory&)> auto& change) {
        matches.reset(flags, jsNumber(0), 1);
        MatchArrayMemory memory(matches, 1);
        matches.script.answer = 1;
        matches.script.offsets = { 1, 4, 2, 4 };
        change(memory);
        matches.runner.recordCallsOf(Entry::operationAOTRegExpMatchLiteral);
        matches.runner.setResultOfRecordedOperation(0x1234);
        StubRun run = matches.match(RegExpMatchResult::Array, subject);
        matches.runner.countCallsOf(Entry::operationAOTRegExpMatchLiteral);
        const RecordedArguments expected { matches.regExpObject.address(), subject.address(), noRegExpRecord, static_cast<uint64_t>(Typed::RegExpMatchForm::Exec), run.recordedArguments[4] };
        return run.numberOfRecordedCalls == 1 && run.result == 0x1234 && static_cast<uint32_t>(run.recordedArguments[2]) == noRegExpRecord
            && run.recordedArguments[0] == expected[0] && run.recordedArguments[1] == expected[1] && static_cast<uint32_t>(run.recordedArguments[3]) == expected[3]
            && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(0));
    };

    AOT_TEST_CHECK(!isLeftToOperation({ }, [](MatchArrayMemory&) { }));
    AOT_TEST_CHECK(isLeftToOperation({ Yarr::Flags::HasIndices }, [](MatchArrayMemory&) { }));
    AOT_TEST_CHECK(isLeftToOperation({ }, [&](MatchArrayMemory&) { matches.regExp.at<uint64_t>(RegExp::offsetOfRareData()) = 16; }));
    AOT_TEST_CHECK(isLeftToOperation({ }, [&](MatchArrayMemory&) { matches.regExp.at<uint32_t>(RegExp::offsetOfNumSubpatterns()) = tooManyGroups; }));
    AOT_TEST_CHECK(isLeftToOperation({ Yarr::Flags::Global }, [&](MatchArrayMemory& memory) { memory.storageAllocator.makeRoomAt(0, 0); }));
    AOT_TEST_CHECK(isLeftToOperation({ Yarr::Flags::Global }, [&](MatchArrayMemory& memory) { matches.ropeAllocator.makeRoomAt(memory.ropes.address(), sizeOfRope); }));
    AOT_TEST_CHECK(isLeftToOperation({ Yarr::Flags::Global }, [&](MatchArrayMemory&) { matches.arrayAllocator.makeRoomAt(0, 0); }));
    AOT_TEST_CHECK(isLeftToOperation({ }, [&](MatchArrayMemory&) { matches.setStorageAllocator(1, nullptr); }));

    instance.prefix().newArrayWithContiguousHeader = 0;
    AOT_TEST_CHECK(isLeftToOperation({ }, [](MatchArrayMemory&) { }));
    instance.prefix().newArrayWithContiguousHeader = anyArrayHeader;

    instance.allocation.inlineAllocations[Matches::kindOfMatchArray] = { };
    AOT_TEST_CHECK(isLeftToOperation({ }, [](MatchArrayMemory&) { }));
}

static void testMatchArraysWithoutObject()
{
    constexpr StubKey finish = finishRegExpExec(FinishRegExpExecForm::WithoutObject);
    constexpr uint64_t record = 7;
    constexpr std::array offsets { 1, 4, 2, 4 };
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcdef"_span);
    FakeSubject subject(text.get());
    {
        matches.reset({ }, jsNumber(9));
        MatchArrayMemory memory(matches, 1);
        std::ranges::copy(offsets, matches.outputOfVM.begin());
        StubRun run = matches.runner.run(finish, { record, subject.address(), 1 });
        checkMatchArray(matches, memory, subject, run, 1, 1);
        AOT_TEST_CHECK(memory.element(0) == memory.addressOfRope(0) && memory.isSubstring(0, subject, 1, 3, true));
        AOT_TEST_CHECK(memory.element(1) == memory.addressOfRope(1) && memory.isSubstring(1, subject, 2, 2, true));
        AOT_TEST_CHECK(matches.hasRecordedNothing());
    }

    auto isLeftToOperation = [&](uint64_t numberOfGroups, const Invocable<void()> auto& change) {
        matches.reset({ }, jsNumber(9));
        MatchArrayMemory memory(matches, 1);
        std::ranges::copy(offsets, matches.outputOfVM.begin());
        change();
        matches.runner.recordCallsOf(Entry::operationAOTRegExpMatchLiteral);
        matches.runner.setResultOfRecordedOperation(0x1234);
        StubRun run = matches.runner.run(finish, { record, subject.address(), numberOfGroups });
        matches.runner.countCallsOf(Entry::operationAOTRegExpMatchLiteral);
        return run.numberOfRecordedCalls == 1 && run.result == 0x1234 && !run.recordedArguments[0] && run.recordedArguments[1] == subject.address()
            && static_cast<uint32_t>(run.recordedArguments[2]) == record && static_cast<uint32_t>(run.recordedArguments[3]) == static_cast<uint32_t>(Typed::RegExpMatchForm::Exec);
    };
    AOT_TEST_CHECK(!isLeftToOperation(1, [] { }));
    AOT_TEST_CHECK(isLeftToOperation(tooManyGroups, [] { }));
    AOT_TEST_CHECK(isLeftToOperation(1, [&] { matches.arrayAllocator.makeRoomAt(0, 0); }));
    AOT_TEST_CHECK(isLeftToOperation(1, [&] { matches.runner.instance().allocation.inlineAllocations[Matches::kindOfMatchArray] = { }; }));
}

static void testSubjectsWithoutCodeAreLeftToOperation()
{
    Matches matches;
    const char16_t units[] = { 'a', 'b', 'c', 'd' };
    Ref<StringImpl> text = StringImpl::create(std::span<const char16_t>(units));
    Ref<StringImpl> replacementText = StringImpl::create("x"_span);
    FakeSubject subject(text.get());
    FakeSubject replacement(replacementText.get());
    AOT_TEST_CHECK(!text->is8Bit());

    for (OptionSet<Yarr::Flags> flags : { OptionSet<Yarr::Flags> { }, OptionSet<Yarr::Flags> { Yarr::Flags::Global } }) {
        matches.reset(flags, jsNumber(1));
        StubRun run = matches.match(RegExpMatchResult::BoxedBoolean, subject);
        AOT_TEST_CHECK(run.calledOperation && !matches.script.numberOfCalls && matches.hasRecordedNothing() && matches.lastIndex() == bitsOfValue(jsNumber(1)));
    }

    Ref<StringImpl> narrowText = StringImpl::create("abcd"_span);
    FakeSubject narrowSubject(narrowText.get());
    matches.runner.recordCallsOf(Entry::operationAOTStringReplaceRegExpString);
    matches.runner.setResultOfRecordedOperation(0x1234);

    matches.reset({ }, jsNumber(1));
    matches.script.answer = -1;
    StubRun run = matches.runner.run(stringReplaceRegExpMayNotMatch(), { narrowSubject.address(), matches.regExpObject.address(), replacement.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == narrowSubject.address() && matches.script.numberOfCalls == 1);

    matches.reset({ }, jsNumber(1));
    run = matches.runner.run(stringReplaceRegExpMayNotMatch(), { subject.address(), matches.regExpObject.address(), replacement.address() });
    matches.runner.countCallsOf(Entry::operationAOTStringReplaceRegExpString);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 0x1234 && !matches.script.numberOfCalls);
    AOT_TEST_CHECK(run.recordedArguments[0] == subject.address() && run.recordedArguments[1] == matches.regExpObject.address() && run.recordedArguments[2] == replacement.address() && !static_cast<uint32_t>(run.recordedArguments[3]));
}

static void testWideTextsUnderUnicode()
{
    Matches matches;
    matches.addWideCode();
    const char16_t units[] = { 'a', 0xd83d, 0xde00, 'b' };
    Ref<StringImpl> text = StringImpl::create(std::span<const char16_t>(units));
    FakeSubject subject(text.get());

    for (auto [form, match, miss] : formsWithoutArray) {
        for (Yarr::Flags flag : { Yarr::Flags::Unicode, Yarr::Flags::UnicodeSets }) {
            for (int start = -1; start <= 4; ++start) {
                bool isInsidePair = start == 2;
                matches.reset({ flag }, jsNumber(0));
                matches.script.answer = start;
                matches.script.offsets = { start, 4 };
                StubRun run = matches.match(form, subject);
                AOT_TEST_CHECK(matches.script.numberOfCalls == 1 && run.calledOperation == isInsidePair);
                if (start < 0)
                    AOT_TEST_CHECK(run.result == miss && matches.hasRecordedNothing());
                else if (!isInsidePair)
                    AOT_TEST_CHECK(run.result == match && matches.hasRecorded(subject, start, 4));
            }
        }
    }
}

static void testReplacementByFunctionLooksFirst()
{
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcd"_span);
    FakeSubject subject(text.get());
    FakeSubject function(text.get());
    function.type = JSFunctionType;
    matches.runner.recordCallsOf(Entry::operationAOTStringReplaceRegExpWithFunction);
    matches.runner.setResultOfRecordedOperation(0x1234);

    for (OptionSet<Yarr::Flags> flags : { OptionSet<Yarr::Flags> { }, OptionSet<Yarr::Flags> { Yarr::Flags::Global } }) {
        matches.reset(flags, jsNumber(1));
        matches.script.answer = -1;
        StubRun run = matches.runner.run(stringReplaceRegExpMayNotMatch(), { subject.address(), matches.regExpObject.address(), function.address() });
        AOT_TEST_CHECK(!run.calledOperation && !run.numberOfRecordedCalls && run.result == subject.address() && matches.script.numberOfCalls == 1);
        AOT_TEST_CHECK(matches.lastIndex() == bitsOfValue(jsNumber(flags.contains(Yarr::Flags::Global) ? 0 : 1)));

        matches.reset(flags, jsNumber(1));
        matches.script.answer = 2;
        run = matches.runner.run(stringReplaceRegExpMayNotMatch(), { subject.address(), matches.regExpObject.address(), function.address() });
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 0x1234 && matches.script.numberOfCalls == 1);
        AOT_TEST_CHECK(run.recordedArguments[0] == subject.address() && run.recordedArguments[1] == matches.regExpObject.address() && run.recordedArguments[2] == function.address());
    }
    matches.runner.countCallsOf(Entry::operationAOTStringReplaceRegExpWithFunction);
}

static void testReplacementOfAnyKindIsLookedAtFirst()
{
    constexpr StubKey stub = stringReplaceRegExpMayNotMatch(ReplacementOfMatches::AnyValue);
    Matches matches;
    Ref<StringImpl> text = StringImpl::create("abcd"_span);
    FakeSubject subject(text.get());
    FakeSubject cell(text.get());

    for (JSType type : { StringType, JSFunctionType, slimClosureTypes.first }) {
        cell.type = type;
        matches.reset({ Yarr::Flags::Global }, jsNumber(1));
        matches.script.answer = -1;
        StubRun run = matches.runner.run(stub, { subject.address(), matches.regExpObject.address(), cell.address() });
        AOT_TEST_CHECK(!run.calledOperation && run.result == subject.address() && matches.script.numberOfCalls == 1 && matches.lastIndex() == bitsOfValue(jsNumber(0)));
    }

    matches.runner.recordCallsOf(Entry::operationAOTStringReplaceRegExp);
    matches.runner.setResultOfRecordedOperation(0x1234);
    auto isLeftToOperation = [&](uint64_t replacement) {
        matches.reset({ Yarr::Flags::Global }, jsNumber(1));
        StubRun run = matches.runner.run(stub, { subject.address(), matches.regExpObject.address(), replacement });
        return run.numberOfRecordedCalls == 1 && run.result == 0x1234 && !matches.script.numberOfCalls && matches.lastIndex() == bitsOfValue(jsNumber(1))
            && run.recordedArguments[0] == subject.address() && run.recordedArguments[1] == matches.regExpObject.address() && run.recordedArguments[2] == replacement;
    };
    AOT_TEST_CHECK(isLeftToOperation(bitsOfValue(jsUndefined())) && isLeftToOperation(bitsOfValue(jsNumber(7))) && isLeftToOperation(bitsOfValue(jsNumber(0.5))));
    for (JSType type : { SymbolType, HeapBigIntType, ObjectType, InternalFunctionType, ProxyObjectType }) {
        cell.type = type;
        AOT_TEST_CHECK(isLeftToOperation(cell.address()));
    }

    cell.type = JSFunctionType;
    matches.reset({ }, jsNumber(1));
    matches.script.answer = 2;
    StubRun found = matches.runner.run(stub, { subject.address(), matches.regExpObject.address(), cell.address() });
    AOT_TEST_CHECK(found.numberOfRecordedCalls == 1 && found.result == 0x1234 && matches.script.numberOfCalls == 1);
    AOT_TEST_CHECK(found.recordedArguments[0] == subject.address() && found.recordedArguments[1] == matches.regExpObject.address() && found.recordedArguments[2] == cell.address());
    matches.runner.countCallsOf(Entry::operationAOTStringReplaceRegExp);
}

static void testWideSubjectsOfLiterals()
{
    using Typed::RegExpMatchForm;
    constexpr uint32_t distanceOfMatchingCode = 64;
    constexpr uint32_t withCodeForBothWidths = 0;
    constexpr uint32_t withCodeFor8BitSubjectsAlone = 1;
    Matches matches;
    std::array<ImageRegExp, 2> records { };
    for (ImageRegExp& record : records) {
        record.rawForm = static_cast<uint8_t>(RegExpRecordForm::Code);
        record.isLiteral = 1;
        record.codeFor8Bit = distanceOfMatchingCode;
        record.codeFor16Bit = distanceOfMatchingCode;
    }
    records[withCodeFor8BitSubjectsAlone].hasCodeFor8BitSubjectsAlone = 1;
    Instance& instance = matches.runner.instance();
    instance.image.regExps = records.data();
    instance.image.regExpCode = std::bit_cast<const uint8_t*>(std::bit_cast<uintptr_t>(tagCFunctionPtr<void*, Yarr8BitPtrTag>(runMatchingCodeScript)) - distanceOfMatchingCode);

    const char16_t units[] = { 'a', 'b', 'c' };
    Ref<StringImpl> narrowText = StringImpl::create("abc"_span);
    Ref<StringImpl> wideText = StringImpl::create(std::span<const char16_t>(units));
    FakeSubject narrow(narrowText.get());
    FakeSubject wide(wideText.get());
    AOT_TEST_CHECK(narrowText->is8Bit() && !wideText->is8Bit());

    auto prepare = [&](OptionSet<Yarr::Flags> flags, JSValue lastIndex, Entry operation) {
        matches.reset(flags, lastIndex);
        matches.script.answer = 0;
        matches.script.offsets = { 0, 3 };
        matches.runner.recordCallsOf(operation);
        matches.runner.setResultOfRecordedOperation(0x1234);
    };

    auto testWithoutObject = [&](const FakeSubject& subject, uint32_t record) {
        prepare({ }, jsNumber(0), Entry::operationAOTRegExpMatchLiteralWithoutObject);
        StubRun result = matches.runner.run(regExpMatchLiteralWithoutObject(RegExpMatchForm::Test), { subject.address() }, record);
        matches.runner.countCallsOf(Entry::operationAOTRegExpMatchLiteralWithoutObject);
        return result;
    };
    for (auto [subject, record] : { std::pair { &narrow, withCodeForBothWidths }, std::pair { &narrow, withCodeFor8BitSubjectsAlone }, std::pair { &wide, withCodeForBothWidths } }) {
        StubRun matched = testWithoutObject(*subject, record);
        AOT_TEST_CHECK(!matched.calledOperation && matched.result == JSValue::ValueTrue && matches.script.numberOfCalls == 1 && matches.script.length == 3);
    }
    StubRun run = testWithoutObject(wide, withCodeFor8BitSubjectsAlone);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 0x1234 && !matches.script.numberOfCalls);
    AOT_TEST_CHECK(run.recordedArguments[0] == wide.address() && static_cast<uint32_t>(run.recordedArguments[1]) == withCodeFor8BitSubjectsAlone && static_cast<uint32_t>(run.recordedArguments[2]) == static_cast<uint32_t>(RegExpMatchForm::Test));

    for (bool isGlobal : { false, true }) {
        auto testWithObject = [&](const FakeSubject& subject, uint32_t record) {
            prepare(isGlobal ? OptionSet<Yarr::Flags> { Yarr::Flags::Global } : OptionSet<Yarr::Flags> { }, jsNumber(0), Entry::operationAOTRegExpTestBoxed);
            StubRun result = matches.runner.run(regExpMatchLiteral(RegExpMatchForm::Test, isGlobal, false), { matches.regExpObject.address(), subject.address() }, record);
            matches.runner.countCallsOf(Entry::operationAOTRegExpTestBoxed);
            return result;
        };
        run = testWithObject(narrow, withCodeFor8BitSubjectsAlone);
        AOT_TEST_CHECK(!run.calledOperation && run.result == JSValue::ValueTrue && matches.script.numberOfCalls == 1 && matches.lastIndex() == bitsOfValue(jsNumber(isGlobal ? 3 : 0)));
        run = testWithObject(wide, withCodeForBothWidths);
        AOT_TEST_CHECK(!run.calledOperation && run.result == JSValue::ValueTrue && matches.script.numberOfCalls == 1 && matches.lastIndex() == bitsOfValue(jsNumber(isGlobal ? 3 : 0)));
        run = testWithObject(wide, withCodeFor8BitSubjectsAlone);
        AOT_TEST_CHECK(run.calledOperation && !matches.script.numberOfCalls && matches.lastIndex() == bitsOfValue(jsNumber(0)));
        if (!isGlobal) {
            AOT_TEST_CHECK(!run.numberOfRecordedCalls && run.result == JSValue::ValueFalse);
            continue;
        }
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 0x1234);
        AOT_TEST_CHECK(run.recordedArguments[0] == matches.regExpObject.address() && run.recordedArguments[1] == wide.address());
    }
}

std::span<const NativeTest> stubRegExpTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/regexp-match/matches-without-array"_s, ImageTests::whenExecutable<testMatchesWithoutArray> },
        { "stubs/regexp-match/matches-of-atoms"_s, ImageTests::whenExecutable<testMatchesOfAtoms> },
        { "stubs/regexp-match/barrier-of-recorded-match"_s, ImageTests::whenExecutable<testBarrierOfRecordedMatch> },
        { "stubs/regexp-match/arrays"_s, ImageTests::whenExecutable<testMatchArrays> },
        { "stubs/regexp-match/string-match-and-search"_s, ImageTests::whenExecutable<testStringMatchAndSearch> },
        { "stubs/regexp-match/no-record-where-nobody-reads-it"_s, ImageTests::whenExecutable<testNoRecordWhereNobodyReadsIt> },
        { "stubs/regexp-match/wide-units-in-arrays"_s, ImageTests::whenExecutable<testWideUnitsInMatchArrays> },
        { "stubs/regexp-match/arrays-left-to-operation"_s, ImageTests::whenExecutable<testMatchArraysLeftToOperation> },
        { "stubs/regexp-match/arrays-without-object"_s, ImageTests::whenExecutable<testMatchArraysWithoutObject> },
        { "stubs/regexp-match/subjects-without-code-are-left-to-operation"_s, ImageTests::whenExecutable<testSubjectsWithoutCodeAreLeftToOperation> },
        { "stubs/regexp-match/wide-texts-under-unicode"_s, ImageTests::whenExecutable<testWideTextsUnderUnicode> },
        { "stubs/regexp-match/replacement-by-function-looks-first"_s, ImageTests::whenExecutable<testReplacementByFunctionLooksFirst> },
        { "stubs/regexp-match/replacement-of-any-kind-is-looked-at-first"_s, ImageTests::whenExecutable<testReplacementOfAnyKindIsLookedAtFirst> },
        { "stubs/regexp-match/wide-subjects-of-literals"_s, ImageTests::whenExecutable<testWideSubjectsOfLiterals> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
