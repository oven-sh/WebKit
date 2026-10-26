/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTLayoutData.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTMappedCopyInlines.h"
#include "AOTObjectWords.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubWords.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsClosures.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSString.h"
#include "NativeExecutable.h"
#include "PureNaN.h"
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <limits>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/DataLog.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> stubRuntimeTests();

static constexpr ASCIILiteral emptyProgramText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

static JSC_DECLARE_HOST_FUNCTION(hostFunctionWithoutBoxedCall);

JSC_DEFINE_HOST_FUNCTION(hostFunctionWithoutBoxedCall, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsUndefined());
}

static void testBoxedCallHandlers()
{
    WTF::BitSet<numberOfBuiltinBoxedStubs> stubsInTable;
    for (const BuiltinBoxedCall& call : BuiltinTable::singleton().boxedCalls()) {
        AOT_TEST_CHECK(!stubsInTable.testAndSet(static_cast<size_t>(call.stub)));
        std::optional<BuiltinNativeFunction> binding = BuiltinBindings::nativeFunctionOf(call.builtin);
        AOT_TEST_CHECK(binding && binding->function);

        BoxedCallForm form = boxedCallFormOf(call.stub);
        unsigned numberOfAcceptedCounts = 0;
        for (unsigned count = BoxedCallEntry::minArgumentCount; count <= BoxedCallEntry::maxArgumentCount; ++count) {
            StubKey handler = callBoxedBuiltin(form, count);
            AOT_TEST_CHECK(exists(handler) && bodyOf(handler) == handler);
            AOT_TEST_CHECK(descriptorOf(handler.family()).linkage == StubLinkage::Handler);
            AOT_TEST_CHECK(signatureFor(handler).usesCallConvention);
            numberOfAcceptedCounts += acceptsArgumentCount(form, count);
        }
        AOT_TEST_CHECK(numberOfAcceptedCounts);
    }
    AOT_TEST_CHECK(stubsInTable.count() == numberOfBuiltinBoxedStubs);
}

static const BoxedCallEntry* probeBoxedCalls(std::span<const BoxedCallEntry> entries, const void* nativeFunction)
{
    uint64_t entry = std::bit_cast<uintptr_t>(nativeFunction) >> StubWords::boxedCallHashShift;
    for (unsigned step = 0; step < StubWords::numberOfBoxedCallEntries; ++step) {
        entry &= StubWords::numberOfBoxedCallEntries - 1;
        const BoxedCallEntry& candidate = entries[entry];
        if (candidate.nativeFunction == nativeFunction)
            return &candidate;
        if (!candidate.nativeFunction)
            return nullptr;
        ++entry;
    }
    failNativeTest(__FILE__, __LINE__, "a probe of the boxed calls ends at a free entry");
}

static const void* nativeFunctionWordOf(LoadedProgram& program, BuiltinID builtin)
{
    bool isAccessor = BuiltinTable::singleton().at(builtin).kind == BuiltinKind::Accessor;
    std::expected<JSCell*, BuiltinLookupFailure> cell = findInRealm(program.realm(), isAccessor ? BuiltinCellKey::getter(builtin) : BuiltinCellKey::object(builtin));
    if (!cell.has_value())
        dataLogLn("the realm has no function for built-in ", builtin.index());
    AOT_TEST_CHECK(cell.has_value());
    auto* function = dynamicDowncast<JSFunction>(*cell);
    AOT_TEST_CHECK(function && function->isHostFunction());
    return uncheckedDowncast<NativeExecutable>(function->executable())->nativeFunctionFor(CodeSpecializationKind::CodeForCall).taggedPtr();
}

static void testBoxedCallTable(LoadedProgram& program)
{
    std::span<const BoxedCallEntry> entries = program.instance().boxedCalls();
    for (const BuiltinBoxedCall& call : BuiltinTable::singleton().boxedCalls()) {
        const BoxedCallEntry* entry = probeBoxedCalls(entries, nativeFunctionWordOf(program, call.builtin));
        AOT_TEST_CHECK(entry);
        for (unsigned count = BoxedCallEntry::minArgumentCount; count <= BoxedCallEntry::maxArgumentCount; ++count) {
            const void* handler = addressOfStubInCurrentImage(callBoxedBuiltin(boxedCallFormOf(call.stub), count));
            AOT_TEST_CHECK(entry->targets[count - BoxedCallEntry::minArgumentCount] == handler);
        }
    }
    AOT_TEST_CHECK(!probeBoxedCalls(entries, TaggedNativeFunction(hostFunctionWithoutBoxedCall).taggedPtr()));

    unsigned numberOfUsedEntries = 0;
    for (const BoxedCallEntry& entry : entries)
        numberOfUsedEntries += !!entry.nativeFunction;
    AOT_TEST_CHECK(numberOfUsedEntries == numberOfBuiltinBoxedStubs);

#if defined(BUN_HARDENED) && OS(UNIX)
    // An executable names its entry by a byte: one more than the entry's number.
    for (unsigned byte = 1; byte <= UINT8_MAX; ++byte) {
        const BoxedCallEntry& entry = entries.data()[byte - 1];
        AOT_TEST_CHECK(byte <= entries.size() || std::ranges::all_of(entry.targets, [](const void* target) { return !target; }));
        AOT_TEST_CHECK(runInChild([&] { const_cast<const void* volatile&>(entry.targets[0]) = nullptr; }) == ChildOutcome::Crashed);
    }
#endif
}

namespace {

enum class CharacterWidth : bool { Is8Bit, Is16Bit };

struct alignas(16) FakeString {
    WTF_MAKE_NONCOPYABLE(FakeString);
public:
    FakeString(ASCIILiteral characters, CharacterWidth width)
        : text(characters)
    {
        if (width == CharacterWidth::Is16Bit)
            text.convertTo16Bit();
        AOT_TEST_CHECK(text.impl() && (text.isEmpty() || text.is8Bit() == (width == CharacterWidth::Is8Bit)));
        fiber = std::bit_cast<uintptr_t>(text.impl());
    }

    FakeString()
        : fiber(JSString::isRopeInPointer)
    {
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }

    uint32_t structureID { 0 };
    uint8_t indexingTypeAndMisc { 0 };
    uint8_t type { StringType };
    uint8_t flags { 0 };
    uint8_t cellState { 0 };
    uintptr_t fiber { 0 };
    String text;
};
static_assert(OBJECT_OFFSETOF(FakeString, type) == JSCell::typeInfoTypeOffset() && OBJECT_OFFSETOF(FakeString, fiber) == JSString::offsetOfValue());

struct OrderOperator {
    BinaryOperator operation;
    Entry entry;
    bool (*holds)(std::strong_ordering);
};

struct TextPair {
    ASCIILiteral left;
    ASCIILiteral right;
};

} // anonymous namespace

static constexpr OrderOperator orderOperators[] = {
    { BinaryOperator::Less, Entry::operationAOTCompareLess, [](std::strong_ordering order) { return order < 0; } },
    { BinaryOperator::LessEq, Entry::operationAOTCompareLessEq, [](std::strong_ordering order) { return order <= 0; } },
    { BinaryOperator::Greater, Entry::operationAOTCompareGreater, [](std::strong_ordering order) { return order > 0; } },
    { BinaryOperator::GreaterEq, Entry::operationAOTCompareGreaterEq, [](std::strong_ordering order) { return order >= 0; } },
};

static constexpr TextPair textPairs[] = {
    { "order"_s, "order"_s },
    { "order"_s, "orders"_s },
    { "orders"_s, "order"_s },
    { "order"_s, "ordeS"_s },
    { "ordes"_s, "order"_s },
    { ""_s, ""_s },
    { ""_s, "o"_s },
};

static void checkOrderOfFlatStrings(StubRunner& runner, StubKey key, const OrderOperator& orderOperator)
{
    for (const TextPair& pair : textPairs) {
        for (CharacterWidth leftWidth : { CharacterWidth::Is8Bit, CharacterWidth::Is16Bit }) {
            for (CharacterWidth rightWidth : { CharacterWidth::Is8Bit, CharacterWidth::Is16Bit }) {
                FakeString left(pair.left, leftWidth);
                FakeString right(pair.right, rightWidth);
                StubRun run = runner.run(key, { left.address(), right.address() });
                AOT_TEST_CHECK(!run.calledOperation);
                AOT_TEST_CHECK(run.result == static_cast<uint64_t>(orderOperator.holds(codePointCompare(StringView(left.text), StringView(right.text)))));
            }
        }
    }
}

static void checkOrderLeftToOperation(StubRunner& runner, StubKey key, uint64_t left, uint64_t right)
{
    for (uint64_t resultOfOperation : { 0, 1 }) {
        runner.setResultOfRecordedOperation(resultOfOperation);
        StubRun run = runner.run(key, { left, right });
        AOT_TEST_CHECK(run.calledOperation && run.numberOfRecordedCalls == 1);
        AOT_TEST_CHECK(run.recordedArguments[0] == left && run.recordedArguments[1] == right);
        AOT_TEST_CHECK(run.result == resultOfOperation);
    }
}

static void testBinaryOperationStringOrder()
{
    StubRunner runner;
    runner.install(Entry::operationAOTCompareStringImpls, operationAOTCompareStringImpls);

    FakeString flat("order"_s, CharacterWidth::Is8Bit);
    FakeString wideFlat("order"_s, CharacterWidth::Is16Bit);
    FakeString rope;
    const uint64_t number = static_cast<uint64_t>(JSValue::encode(jsNumber(7)));
    const uint64_t fraction = static_cast<uint64_t>(JSValue::encode(jsNumber(0.5)));

    for (const OrderOperator& orderOperator : orderOperators) {
        runner.recordCallsOf(orderOperator.entry);
        for (bool isAfterInt32Check : { false, true }) {
            StubKey key = binaryOperation(orderOperator.operation, isAfterInt32Check);
            checkOrderOfFlatStrings(runner, key, orderOperator);
            checkOrderLeftToOperation(runner, key, rope.address(), flat.address());
            checkOrderLeftToOperation(runner, key, wideFlat.address(), rope.address());
            checkOrderLeftToOperation(runner, key, flat.address(), number);
            checkOrderLeftToOperation(runner, key, fraction, wideFlat.address());
        }
        runner.countCallsOf(orderOperator.entry);
    }
}

static uint64_t encoded(JSValue value)
{
    return static_cast<uint64_t>(JSValue::encode(value));
}

static bool isNumberInCanonicalForm(uint64_t result, double expected)
{
    if (std::isnan(expected)) {
        JSValue value = JSValue::decode(static_cast<EncodedJSValue>(result));
        return value.isDouble() && std::isnan(value.asDouble());
    }
    return result == encoded(jsNumber(expected));
}

static bool isNumberInCanonicalFormWithSign(uint64_t result, double expected)
{
    return isNumberInCanonicalForm(result, expected) && std::signbit(JSValue::decode(static_cast<EncodedJSValue>(result)).asNumber()) == std::signbit(expected);
}

static void checkLeftToOperation(StubRunner& runner, StubKey key, Entry entry, std::initializer_list<uint64_t> operands)
{
    constexpr uint64_t resultOfOperation = 0x7777;
    runner.recordCallsOf(entry);
    runner.setResultOfRecordedOperation(resultOfOperation);
    StubRun run = runner.run(key, operands);
    AOT_TEST_CHECK(run.calledOperation && run.numberOfRecordedCalls == 1 && run.result == resultOfOperation);
    AOT_TEST_CHECK(std::ranges::equal(operands, std::span(run.recordedArguments).first(operands.size())));
    runner.countCallsOf(entry);
}

namespace {

struct UnaryCase {
    UnaryOperationForm form;
    Entry entry;
    double (*apply)(double);
};

struct BinaryCase {
    BinaryOperator operation;
    double left;
    double right;
    double expected;
};

} // anonymous namespace

static constexpr UnaryCase unaryCases[] = {
    { UnaryOperationForm::Negate, Entry::operationAOTValueNegate, [](double number) { return -number; } },
    { UnaryOperationForm::Increment, Entry::operationAOTIncrement, [](double number) { return number + 1; } },
    { UnaryOperationForm::Decrement, Entry::operationAOTDecrement, [](double number) { return number - 1; } },
    { UnaryOperationForm::BitNot, Entry::operationAOTValueBitNot, [](double number) { return static_cast<double>(~toInt32(number)); } },
    { UnaryOperationForm::ToNumeric, Entry::operationAOTToNumeric, [](double number) { return number; } },
};

#if CPU(ARM64)
static constexpr bool stubsConvertEveryDouble = MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(CPUFeatureSet::buildTarget());
#else
static constexpr bool stubsConvertEveryDouble = false;
#endif

static void testUnaryOperations()
{
    StubRunner runner;
    FakeString string("7"_s, CharacterWidth::Is8Bit);
    constexpr double insideInt64[] = { 0, 1, -1, 7, 2147483647, -2147483648.0, 2147483648.0, -2147483649.0, 4294967296.0, 0.5, -0.5, -0.0, 1e15, -1e15 };
    constexpr double outsideInt64[] = { 1e300, -1e300, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), PNaN, -PNaN };

    for (const UnaryCase& unaryCase : unaryCases) {
        StubKey key = unaryOperation(unaryCase.form);
        for (double number : insideInt64) {
            StubRun run = runner.run(key, { encoded(jsNumber(number)) });
            AOT_TEST_CHECK(!run.calledOperation && isNumberInCanonicalFormWithSign(run.result, unaryCase.apply(number)));
        }
        for (double number : outsideInt64) {
            // Without FJCVTZS a stub converts through int64, and ARM64 converts NaN to 0.
            bool isConvertedByStub = stubsConvertEveryDouble || (isARM64() && std::isnan(number));
            if (unaryCase.form == UnaryOperationForm::BitNot && !isConvertedByStub) {
                checkLeftToOperation(runner, key, unaryCase.entry, { encoded(jsNumber(number)) });
                continue;
            }
            StubRun run = runner.run(key, { encoded(jsNumber(number)) });
            AOT_TEST_CHECK(!run.calledOperation && isNumberInCanonicalFormWithSign(run.result, unaryCase.apply(number)));
        }
        for (uint64_t value : { encoded(jsUndefined()), encoded(jsNull()), encoded(jsBoolean(true)), encoded(jsBoolean(false)), string.address() })
            checkLeftToOperation(runner, key, unaryCase.entry, { value });
    }
}

static constexpr BinaryCase binaryCasesOnNumbers[] = {
    { BinaryOperator::Add, 2147483647, 1, 2147483648.0 },
    { BinaryOperator::Add, -2147483648.0, -1, -2147483649.0 },
    { BinaryOperator::Add, 2147483648.0, -1, 2147483647 },
    { BinaryOperator::Sub, -2147483648.0, 1, -2147483649.0 },
    { BinaryOperator::Sub, 2147483647, -1, 2147483648.0 },
    { BinaryOperator::Mul, 65536, 65536, 4294967296.0 },
    { BinaryOperator::Mul, 0, -5, -0.0 },
    { BinaryOperator::Mul, -5, 0, -0.0 },
    { BinaryOperator::Mul, 0, 5, 0 },
    { BinaryOperator::Mul, 3, -5, -15 },
    { BinaryOperator::Mod, 7, 3, 1 },
    { BinaryOperator::Mod, -7, 3, -1 },
    { BinaryOperator::Mod, 7, -3, 1 },
    { BinaryOperator::Mod, -6, 3, -0.0 },
    { BinaryOperator::Mod, 0, 3, 0 },
    { BinaryOperator::Mod, -0.0, 3, -0.0 },
    { BinaryOperator::Mod, 7, 0, PNaN },
    { BinaryOperator::Mod, -2147483648.0, -1, -0.0 },
    { BinaryOperator::Mod, 5.5, 2, 1.5 },
    { BinaryOperator::Mod, 1e15, 7, 6 },
    { BinaryOperator::Mod, 1e300, 7, 1 },
    { BinaryOperator::Mod, 7, std::numeric_limits<double>::infinity(), 7 },
    { BinaryOperator::Mod, std::numeric_limits<double>::infinity(), 7, PNaN },
    { BinaryOperator::Mod, PNaN, 7, PNaN },
    { BinaryOperator::URShift, -1, 0, 4294967295.0 },
    { BinaryOperator::URShift, -1, 1, 2147483647 },
    { BinaryOperator::URShift, -1.5, 0, 4294967295.0 },
    { BinaryOperator::URShift, 4294967295.0, 32, 4294967295.0 },
};

static void testBinaryOperationsOnNumbers()
{
    StubRunner runner;
    runner.install(Entry::operationAOTFmod, operationAOTFmod);
    FakeString string("7"_s, CharacterWidth::Is8Bit);

    for (const BinaryCase& binaryCase : binaryCasesOnNumbers) {
        AOT_TEST_CHECK(binaryCase.operation != BinaryOperator::Mod || std::isnan(binaryCase.expected) || std::fmod(binaryCase.left, binaryCase.right) == binaryCase.expected);
        StubRun run = runner.run(binaryOperation(binaryCase.operation, false), { encoded(jsNumber(binaryCase.left)), encoded(jsNumber(binaryCase.right)) });
        AOT_TEST_CHECK(!run.calledOperation && isNumberInCanonicalForm(run.result, binaryCase.expected));
    }

    const uint64_t seven = encoded(jsNumber(7));
    for (uint64_t value : { encoded(jsUndefined()), encoded(jsBoolean(true)), string.address() }) {
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mod, false), Entry::operationAOTValueMod, { value, seven });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mod, false), Entry::operationAOTValueMod, { seven, value });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mul, false), Entry::operationAOTValueMul, { value, seven });
    }
}

namespace {

struct alignas(16) FakeCell {
    static constexpr unsigned numberOfWords = 12;
    static constexpr uint64_t untouched = 0xa5a5a5a5a5a5a5a5ULL;

    FakeCell() { words.fill(untouched); }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }
    bool isUntouched() const { return std::ranges::all_of(words, [](uint64_t word) { return word == untouched; }); }

    std::array<uint64_t, numberOfWords> words;
};

struct alignas(16) AllocationSites {
    static constexpr uint32_t siteWithoutRecord = 0;
    static constexpr uint32_t siteWithAllocator = 1;
    static constexpr uint32_t siteWithoutAllocator = 2;
    static constexpr uint64_t header = 0x0123456789abcdefULL;
    static constexpr uint64_t orderAndShapeWords = 0x1122334455667788ULL;

    explicit AllocationSites(StubRunner& runner)
    {
        records[2] = { 0, 0, &allocatorWithoutRoom, 0 };
        records[1] = { header, orderAndShapeWords, &allocator, 0 };
        records[0] = { header, orderAndShapeWords, &allocatorWithoutRoom, 0 };
        distances[siteWithAllocator] = -static_cast<int32_t>(sizeof(AllocationSiteRecord));
        distances[siteWithoutAllocator] = -2 * static_cast<int32_t>(sizeof(AllocationSiteRecord));
        runner.instance().objects.distancesToAllocationSites = distances.data();
        AOT_TEST_CHECK(runner.instance().objects.emptyAllocationSite() == &records[2]);
        AOT_TEST_CHECK(!runner.instance().objects.allocationSite(siteWithoutRecord));
        AOT_TEST_CHECK(runner.instance().objects.allocationSite(siteWithAllocator) == &records[1]);
        AOT_TEST_CHECK(runner.instance().objects.allocationSite(siteWithoutAllocator) == &records[0]);
    }

    void makeRoomFor(FakeCell& cell) { allocator.makeRoomAt(cell.address(), sizeof(FakeCell)); }
    bool hasRoom() { return allocator.hasRoom(); }

    std::array<AllocationSiteRecord, 3> records { };
    std::array<int32_t, 4> distances { };
    FakeAllocator allocator { sizeof(FakeCell) };
    FakeAllocator allocatorWithoutRoom { sizeof(FakeCell) };
};
static_assert(OBJECT_OFFSETOF(AllocationSites, distances) == OBJECT_OFFSETOF(AllocationSites, records) + 3 * sizeof(AllocationSiteRecord));

} // anonymous namespace

static void checkCellFromOperation(StubRunner& runner, StubKey key, uint32_t site)
{
    FakeCell cell;
    runner.setResultOfRecordedOperation(cell.address());
    StubRun run = runner.run(key, { site });
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == site);
    AOT_TEST_CHECK(run.result == cell.address() && cell.isUntouched());
}

static void testAllocationAtSites()
{
    StubRunner runner;
    runner.recordCallsOf(Entry::operationAOTAllocateSlow);
    AllocationSites sites(runner);

    for (CellInitialization initialization : { CellInitialization::Header, CellInitialization::HeaderAndWords, CellInitialization::HeaderAndZeros, CellInitialization::HeaderAndWordsAndZeros }) {
        const uint64_t wordOfBody = zeroesBody(initialization) ? 0 : FakeCell::untouched;
        auto holdsWordOfBody = [&](std::span<const uint64_t> words) {
            return std::ranges::all_of(words, [&](uint64_t word) { return word == wordOfBody; });
        };
        StubKey key = allocateCell(initialization);
        FakeCell cell;
        sites.makeRoomFor(cell);
        StubRun run = runner.run(key, { AllocationSites::siteWithAllocator });
        AOT_TEST_CHECK(!run.calledOperation && run.result == cell.address() && !sites.hasRoom());
        AOT_TEST_CHECK(cell.words[0] == AllocationSites::header);
        AOT_TEST_CHECK(cell.words[1] == (storesWords(initialization) ? AllocationSites::orderAndShapeWords : wordOfBody));
        AOT_TEST_CHECK(holdsWordOfBody(std::span(cell.words).subspan(2)));

        // The cells of the allocator have an odd number of chunks, and nothing behind the cell is written.
        constexpr uint32_t sizeWithOddChunks = sizeof(FakeCell) - cellSizeStep;
        static_assert(sizeWithOddChunks / copyChunkBytes % 2 && cellSizeStep == 2 * sizeof(uint64_t));
        FakeCell shorter;
        sites.makeRoomFor(shorter);
        sites.allocator.at<uint32_t>(LocalAllocator::offsetOfCellSize()) = sizeWithOddChunks;
        run = runner.run(key, { AllocationSites::siteWithAllocator });
        sites.allocator.at<uint32_t>(LocalAllocator::offsetOfCellSize()) = sizeof(FakeCell);
        sites.allocator.makeRoomAt(0, 0);
        AOT_TEST_CHECK(!run.calledOperation && run.result == shorter.address() && shorter.words[0] == AllocationSites::header);
        AOT_TEST_CHECK(holdsWordOfBody(std::span(shorter.words).subspan(2, FakeCell::numberOfWords - 4)));
        AOT_TEST_CHECK(shorter.words[FakeCell::numberOfWords - 2] == FakeCell::untouched && shorter.words[FakeCell::numberOfWords - 1] == FakeCell::untouched);

        checkCellFromOperation(runner, key, AllocationSites::siteWithAllocator);
        checkCellFromOperation(runner, key, AllocationSites::siteWithoutAllocator);
        checkCellFromOperation(runner, key, AllocationSites::siteWithoutRecord);

        // With the size in the key the allocator's own word for it is not read.
        constexpr size_t sizeInKey = 2 * cellSizeStep;
        static_assert(sizeInKey < sizeof(FakeCell));
        StubKey sizedKey = allocateCell(initialization, sizeInKey);
        FakeCell first;
        sites.makeRoomFor(first);
        run = runner.run(sizedKey, { AllocationSites::siteWithAllocator });
        AOT_TEST_CHECK(!run.calledOperation && run.result == first.address() && sites.allocator.intervalStart() == first.address() + sizeInKey);
        AOT_TEST_CHECK(first.words[0] == AllocationSites::header);
        AOT_TEST_CHECK(holdsWordOfBody(std::span(first.words).subspan(2, 2)) && first.words[4] == FakeCell::untouched);
        sites.allocator.makeRoomAt(0, 0);
        checkCellFromOperation(runner, sizedKey, AllocationSites::siteWithAllocator);
        checkCellFromOperation(runner, sizedKey, AllocationSites::siteWithoutRecord);
    }
}

static void testSlimClosures()
{
    constexpr uint32_t function = 3;
    constexpr uint64_t header = 0x0123456789abcdefULL;
    constexpr uint64_t capture = 0x5555666677778888ULL;
    constexpr unsigned kind = 1;
    constexpr uint64_t request = function | static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | 1ULL << ClosureRequest::countShift;
    constexpr unsigned cellSize = 2 * MarkedSpace::sizeStep;
    const std::array<uint64_t, 4> closureWords { 0, 0, 0, 0x1122334455667788ULL };

    StubRunner runner;
    runner.recordCallsOf(Entry::operationAOTNewClosureSlow);
    FakeAllocator allocator { cellSize };
    runner.instance().functionTables.closureWords = closureWords.data();
    runner.instance().closures.slimClosureAllocators[1] = reinterpret_cast<LocalAllocator*>(&allocator);
    runner.instance().slimClosureHeaders[kind] = header;

    FakeCell cell;
    allocator.makeRoomAt(cell.address(), cellSize);
    StubRun run = runner.run(newSlimClosure(kind, 1), { function, capture });
    AOT_TEST_CHECK(!run.calledOperation && run.result == cell.address() && !allocator.hasRoom());
    AOT_TEST_CHECK(cell.words[0] == header && cell.words[1] == closureWords[function] && cell.words[2] == capture && cell.words[3] == FakeCell::untouched);

    FakeCell fromOperation;
    runner.setResultOfRecordedOperation(fromOperation.address());
    run = runner.run(newSlimClosure(kind, 1), { function, capture });
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == request && run.recordedArguments[1] == capture);
    AOT_TEST_CHECK(run.result == fromOperation.address() && fromOperation.isUntouched());
}

static FakeCell createCopiedObject(unsigned fieldBytes)
{
    FakeCell source;
    source.words[0] = 0xfedcba9876543210ULL;
    source.words[1] = UINT64_MAX;
    for (unsigned word = offsetOfFirstField / sizeof(uint64_t); word < (offsetOfFirstField + fieldBytes) / sizeof(uint64_t); ++word)
        source.words[word] = 0x1000 + word;
    return source;
}

static void checkCopy(const FakeCell& copy, const FakeCell& source, unsigned fieldBytes, uint64_t header)
{
    unsigned endOfFields = (offsetOfFirstField + fieldBytes) / sizeof(uint64_t);
    AOT_TEST_CHECK(copy.words[0] == header && copy.words[1] == (source.words[1] & orderCountAndShapeMask));
    for (unsigned word = offsetOfFirstField / sizeof(uint64_t); word < FakeCell::numberOfWords; ++word)
        AOT_TEST_CHECK(copy.words[word] == (word < endOfFields ? source.words[word] : FakeCell::untouched));
}

static StubRun runCopyObject(StubRunner& runner, bool sizeIsOperand, uint32_t site, const FakeCell& source, unsigned fieldBytes)
{
    if (sizeIsOperand)
        return runner.run(copyObject(std::nullopt), { site, source.address(), fieldBytes - copyChunkBytes });
    return runner.run(copyObject(fieldBytes), { site, source.address() });
}

static void testObjectCopies()
{
    constexpr uint64_t headerFromOperation = 0x0f0f0f0f0f0f0f0fULL;
    StubRunner runner;
    runner.recordCallsOf(Entry::operationAOTAllocateSlow);
    AllocationSites sites(runner);

    for (unsigned fieldBytes = copyChunkBytes; fieldBytes <= maxBytesCopiedWithoutLoop; fieldBytes += copyChunkBytes) {
        FakeCell source = createCopiedObject(fieldBytes);
        for (bool sizeIsOperand : { false, true }) {
            FakeCell copy;
            sites.makeRoomFor(copy);
            StubRun run = runCopyObject(runner, sizeIsOperand, AllocationSites::siteWithAllocator, source, fieldBytes);
            AOT_TEST_CHECK(!run.calledOperation && run.result == copy.address() && !sites.hasRoom());
            checkCopy(copy, source, fieldBytes, AllocationSites::header);

            for (uint32_t site : { AllocationSites::siteWithAllocator, AllocationSites::siteWithoutAllocator, AllocationSites::siteWithoutRecord }) {
                FakeCell copyFromOperation;
                copyFromOperation.words[0] = headerFromOperation;
                runner.setResultOfRecordedOperation(copyFromOperation.address());
                StubRun slowRun = runCopyObject(runner, sizeIsOperand, site, source, fieldBytes);
                AOT_TEST_CHECK(slowRun.numberOfRecordedCalls == 1 && slowRun.recordedArguments[0] == site && slowRun.result == copyFromOperation.address());
                checkCopy(copyFromOperation, source, fieldBytes, headerFromOperation);
            }
        }
    }
}

static void testCreatedFieldsOfObjectsWithExtraProperties()
{
    constexpr uint32_t slotKey = offsetOfFirstField;
    constexpr uint32_t numberOfExtraKeys = 2;
    constexpr uint32_t orderBefore = orderWordOf(3, 1);
    constexpr uint32_t orderAfter = orderWordOf(5, 3);
    constexpr uint32_t orderAfterWithoutExtraKeys = orderWordOf(3, 2);
    constexpr uint64_t wordsBefore = static_cast<uint64_t>(shapeWordOf(9)) << 32 | orderBefore;
    StubRunner runner;
    Instance& instance = runner.instance();
    std::array<KeyOrderTransition, 16> transitions { };
    instance.objects.keyOrderTransitions = transitions.data();
    instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(transitions.size() - 1);
    Vector<KeyOrderRun> runs(numberOfKeyOrderRuns);
    instance.objects.keyOrderRuns = runs.mutableSpan().data();
    auto publish = [&](uint32_t key, uint32_t after) {
        uint32_t index = KeyOrderTransition::hashOf(wordsBefore, key) & instance.objects.keyOrderTransitionMask;
        while (!transitions[index].isEmpty())
            index = (index + 1) & instance.objects.keyOrderTransitionMask;
        transitions[index] = { wordsBefore, key, after };
    };
    publish(transitionKeyFor(slotKey, 0), orderAfterWithoutExtraKeys);
    publish(transitionKeyFor(slotKey, numberOfExtraKeys), orderAfter);

    FakeObject<sizeof(CompiledStructure)> structure;
    instance.prefix().structureIDBase = structure.address() & ~static_cast<uint64_t>(UINT32_MAX);
    // The layout lists no order.
    FakeObject<sizeof(LayoutData)> layout;
    structure.at<uint64_t>(CompiledStructure::offsetOfLayout()) = layout.address();
    uint16_t& numberInStructure = structure.at<uint16_t>(CompiledStructure::offsetOfNumberOfExtraKeys());
    uint32_t& bitsOfStructure = structure.at<uint32_t>(Structure::bitFieldOffset());
    FakeObject<offsetOfFirstField + sizeof(uint64_t)> object;
    object.setHeader(static_cast<uint32_t>(structure.address()), CompiledObjectType);
    object.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
    uint32_t& orderWord = object.at<uint32_t>(offsetOfOrderWord);
    auto create = [&](uint32_t flags) {
        orderWord = orderBefore | flags;
        runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
        StubRun run = runner.run(didCreateField(CreatedFields::One), { object.address(), slotKey });
        runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
        AOT_TEST_CHECK(!run.calledOperation || (run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == slotKey && orderWord == (orderBefore | flags)));
        return !run.calledOperation;
    };

    numberInStructure = CompiledStructure::unknownNumberOfExtraKeys;
    AOT_TEST_CHECK(create(0) && orderWord == orderAfterWithoutExtraKeys);
    AOT_TEST_CHECK(!create(orderWordHasExtraProperties));

    // The run of an object without extra properties is remembered: it needs its transition no more, and keeps the flags of the order word. Nothing is remembered of the others.
    const KeyOrderRun& remembered = runs[indexOfKeyOrderRun(wordsBefore, slotKey)];
    AOT_TEST_CHECK(remembered.orderAndShapeWordsBefore == wordsBefore && remembered.slotKeys == slotKey && remembered.orderAndCountAfter == orderAfterWithoutExtraKeys);
    std::array<KeyOrderTransition, 16> published = transitions;
    transitions.fill({ });
    AOT_TEST_CHECK(create(0) && orderWord == orderAfterWithoutExtraKeys);
    AOT_TEST_CHECK(create(orderWordHasFieldOutsideSlot) && orderWord == (orderAfterWithoutExtraKeys | orderWordHasFieldOutsideSlot));
    runs[indexOfKeyOrderRun(wordsBefore, slotKey)].slotKeys = slotKey + 1;
    AOT_TEST_CHECK(!create(0));
    transitions = published;
    AOT_TEST_CHECK(create(0) && orderWord == orderAfterWithoutExtraKeys && remembered.slotKeys == slotKey);

    numberInStructure = numberOfExtraKeys;
    AOT_TEST_CHECK(create(orderWordHasExtraProperties) && orderWord == (orderAfter | orderWordHasExtraProperties));
    numberInStructure = 0;
    AOT_TEST_CHECK(create(orderWordHasExtraProperties) && orderWord == (orderAfterWithoutExtraKeys | orderWordHasExtraProperties));
    numberInStructure = numberOfExtraKeys + 1;
    AOT_TEST_CHECK(!create(orderWordHasExtraProperties));

    // The run time reports a field that a prototype gets.
    numberInStructure = numberOfExtraKeys;
    bitsOfStructure = Structure::s_mayBePrototypeBits;
    AOT_TEST_CHECK(!create(orderWordHasExtraProperties));
}

static void testCreatedFieldsRememberedAtTheirPlace()
{
    constexpr uint32_t slotKey = offsetOfFirstField;
    constexpr uint32_t orderBefore = orderWordOf(3, 1);
    constexpr uint32_t orderAfter = orderWordOf(3, 2);
    constexpr uint64_t wordsBefore = static_cast<uint64_t>(shapeWordOf(9)) << 32 | orderBefore;
    StubRunner runner;
    Instance& instance = runner.instance();
    std::array<KeyOrderTransition, 16> transitions { };
    instance.objects.keyOrderTransitions = transitions.data();
    instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(transitions.size() - 1);
    Vector<KeyOrderRun> runs(numberOfKeyOrderRuns);
    instance.objects.keyOrderRuns = runs.mutableSpan().data();
    transitions[KeyOrderTransition::hashOf(wordsBefore, transitionKeyFor(slotKey, 0)) & instance.objects.keyOrderTransitionMask] = { wordsBefore, transitionKeyFor(slotKey, 0), orderAfter };

    FakeObject<sizeof(CompiledStructure)> structure;
    instance.prefix().structureIDBase = structure.address() & ~static_cast<uint64_t>(UINT32_MAX);
    // The layout lists no order.
    FakeObject<sizeof(LayoutData)> layout;
    structure.at<uint64_t>(CompiledStructure::offsetOfLayout()) = layout.address();
    FakeObject<offsetOfFirstField + sizeof(uint64_t)> object;
    object.setHeader(static_cast<uint32_t>(structure.address()), CompiledObjectType);
    object.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
    uint32_t& orderWord = object.at<uint32_t>(offsetOfOrderWord);

    auto stepOf = [&](uint32_t place) -> KeyOrderStepCache& {
        return reinterpret_cast<KeyOrderStepCache*>(reinterpret_cast<uint8_t*>(&instance) + InstanceLayout::offsetOfKeyOrderSteps)[indexOfKeyOrderStepCache(place, static_cast<uint64_t>(shapeWordOf(9)) << 32)];
    };
    auto create = [&](CreatedFieldsAtPlace fields, uint32_t place, uint32_t flags) {
        Entry operation = fields == CreatedFieldsAtPlace::One ? Entry::operationAOTDidCreateFieldAt : Entry::operationAOTDidCreateFieldsAt;
        orderWord = orderBefore | flags;
        runner.recordCallsOf(operation);
        StubRun run = runner.run(didCreateFieldAtPlace(fields), { object.address(), slotKey, place + 1 });
        runner.countCallsOf(operation);
        return !run.calledOperation;
    };

    // The first call of each pair finds the transition or the run that the call in front of it has remembered. Either way the place remembers the step, under its turn through the entries.
    for (uint32_t place : { 0u, 5u, numberOfKeyOrderStepCaches + 5, 3 * numberOfKeyOrderStepCaches + 5, maxPlacesWithKeyOrderStepCache - 1 }) {
        for (CreatedFieldsAtPlace fields : { CreatedFieldsAtPlace::One, CreatedFieldsAtPlace::Run }) {
            KeyOrderStepCache& step = stepOf(place);
            step = { };
            AOT_TEST_CHECK(create(fields, place, 0) && orderWord == orderAfter);
            AOT_TEST_CHECK(step.orderWordBefore == orderBefore && step.shapeWord == shapeWordOf(9) && step.orderWordAfter == orderAfter);
            AOT_TEST_CHECK(step.turnPlusOne == place / numberOfKeyOrderStepCaches + 1 && step.turnPlusOne <= UINT8_MAX);
        }
    }

    // Nothing is remembered of an object with a flag in its order word, and nothing where the run time is asked.
    for (CreatedFieldsAtPlace fields : { CreatedFieldsAtPlace::One, CreatedFieldsAtPlace::Run }) {
        stepOf(5) = { };
        AOT_TEST_CHECK(create(fields, 5, orderWordHasFieldOutsideSlot) && orderWord == (orderAfter | orderWordHasFieldOutsideSlot));
        AOT_TEST_CHECK(!stepOf(5).turnPlusOne && !stepOf(5).shapeWord);
    }
    transitions.fill({ });
    runs.fill(KeyOrderRun { });
    for (CreatedFieldsAtPlace fields : { CreatedFieldsAtPlace::One, CreatedFieldsAtPlace::Run }) {
        AOT_TEST_CHECK(!create(fields, 5, 0) && orderWord == orderBefore);
        AOT_TEST_CHECK(!stepOf(5).turnPlusOne && !stepOf(5).shapeWord);
    }
}

// Counts the instructions of the stub between compiled code and an operation that saves no registers. The budget may only go down.
namespace {

struct CopiedField {
    SlotRepresentation from;
    SlotRepresentation to;
    uint64_t bits;
};

class CopiesByMoves {
public:
    static constexpr Entry operation = Entry::operationAOTCopyDataProperties;
    static constexpr Entry operationByFieldMap = Entry::operationAOTCopyDataPropertiesByFieldMap;
    static constexpr uint32_t excludedRun = 3;
    static constexpr uint32_t orderAfter = orderWordOf(2, 3);
    static constexpr uint64_t untouched = 0xababababababababULL;
    static constexpr unsigned maxNumberOfFields = 4;

    CopiesByMoves()
        : m_runner(StubPolicy { }, std::array { call() })
    {
        Instance& instance = m_runner.instance();
        instance.prefix().structureIDBase = m_structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
        instance.cellCageBase = cageBase();
        instance.barrierThreshold = blackThreshold;
        m_target.setHeader(static_cast<uint32_t>(m_structures[0].address()), CompiledObjectType);
        m_source.setHeader(static_cast<uint32_t>(m_structures[1].address()), CompiledObjectType);
        m_target.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
        m_target.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
        m_source.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(12);
        m_source.at<uint32_t>(offsetOfOrderWord) = orderWordOf(1, 4);
    }

    static StubKey call() { return callOperationFor(operation, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(operation)); }
    uintptr_t cageBase() const { return MarkedBlockCage::baseOf(std::bit_cast<const void*>(static_cast<uintptr_t>(m_source.address()))); }
    StubRunner& runner() { return m_runner; }
    MappedCopy& copy() { return m_copy; }
    auto& target() { return m_target; }
    auto& source() { return m_source; }
    auto& structureOfTarget() { return m_structures[0]; }

    void describe(std::span<const CopiedField> fields)
    {
        AOT_TEST_CHECK(fields.size() <= maxNumberOfFields);
        m_copy = { };
        m_copy.targetWords = wordsOf(m_target) & ~static_cast<uint64_t>(UINT32_MAX);
        m_copy.sourceWords = wordsOf(m_source);
        m_copy.excludedRunNumberPlusOne = excludedRun;
        m_copy.orderWordAfter = orderAfter;
        m_copy.state = MappedCopy::State::IsServed;
        m_copy.takesNoFrame = true;
        m_copy.structureOfTarget = std::bit_cast<StructureID>(m_target.at<uint32_t>(JSCell::structureIDOffset()));
        m_copy.structureOfSource = std::bit_cast<StructureID>(m_source.at<uint32_t>(JSCell::structureIDOffset()));
        // The fields lie the other way round in the target.
        for (unsigned index = 0; index < fields.size(); ++index) {
            const CopiedField& field = fields[index];
            uint32_t from = offsetOfFirstField + index * sizeof(uint64_t);
            uint32_t to = offsetOfFirstField + (maxNumberOfFields - 1 - index) * sizeof(uint64_t);
            m_copy.moves.append(MappedCopy::Move { from, to, moveKindFor(field.from, field.to, false), static_cast<uint8_t>(widthOf(field.to)), false, false, field.from, field.to, 0, static_cast<uint16_t>(index) });
            m_source.at<uint64_t>(from) = field.bits;
        }
        describeMovesForStub(m_copy);
        place() = &m_copy;
    }

    const MappedCopy*& place() { return m_runner.instance().allocation.copiesWithoutFrame[AllocationWords::placeOfCopyWithoutFrame(m_copy.targetWords, m_copy.sourceWords, excludedRun)]; }

    // True where the stub has made the copy. Else it has handed what it was handed to the operation that looks for the copy again or, where none is kept at the place or keys are handed over, to the one that takes a frame at once.
    bool run(uint32_t run = excludedRun, uint32_t count = 0)
    {
        bool isKept = !count && m_runner.instance().allocation.copiesWithoutFrame[AllocationWords::placeOfCopyWithoutFrame(wordsOf(m_target), wordsOf(m_source), run)];
        m_runner.recordCallsOf(isKept ? operationByFieldMap : operation);
        m_lastRun = m_runner.runOperationCall(call(), { m_target.address(), m_source.address(), run, count });
        m_runner.countCallsOf(operation);
        m_runner.countCallsOf(operationByFieldMap);
        if (!m_lastRun.calledOperation)
            return true;
        AOT_TEST_CHECK(m_lastRun.numberOfRecordedCalls == 1 && m_lastRun.recordedArguments[0] == m_target.address() && m_lastRun.recordedArguments[1] == m_source.address());
        AOT_TEST_CHECK(static_cast<uint32_t>(m_lastRun.recordedArguments[2]) == run && static_cast<uint32_t>(m_lastRun.recordedArguments[3]) == count);
        return false;
    }

    const StubRun& lastRun() const { return m_lastRun; }

    void emptyTarget()
    {
        m_target.at<uint32_t>(offsetOfOrderWord) = unorderedOrderWord;
        for (unsigned index = 0; index < maxNumberOfFields; ++index)
            m_target.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) = untouched;
    }

    // The stub does what tryMove() does, move by move, and writes nothing else.
    void check(std::span<const CopiedField> fields)
    {
        describe(fields);
        AOT_TEST_CHECK(m_copy.structureOfSourceForStub == m_copy.structureOfSource);
        FakeObject<sizeOfCell> expected = m_target;
        bool allMove = true;
        emptyTarget();
        expected = m_target;
        for (const MappedCopy::Move& move : m_copy.moves) {
            allMove = tryMove(cageBase(), move, &m_source.at<uint8_t>(move.from), [&]<typename T>(T bits) {
                expected.at<T>(move.to) = bits;
            }, [] {
                return false;
            });
            if (!allMove)
                break;
        }
        if (allMove)
            expected.at<uint32_t>(offsetOfOrderWord) = orderAfter;
        AOT_TEST_CHECK(run() == allMove);
        AOT_TEST_CHECK(m_target.bytes == expected.bytes);
    }

private:
    static constexpr size_t sizeOfCell = offsetOfFirstField + maxNumberOfFields * sizeof(uint64_t);
    static uint64_t wordsOf(FakeObject<sizeOfCell>& cell) { return cell.at<uint64_t>(offsetOfOrderWord); }

    StubRunner m_runner;
    std::array<FakeObject<sizeof(CompiledStructure)>, 2> m_structures;
    FakeObject<sizeOfCell> m_target;
    FakeObject<sizeOfCell> m_source;
    MappedCopy m_copy;
    StubRun m_lastRun { };
};

} // anonymous namespace

static void testCopiesOfPropertiesByMoves()
{
    using Slot = SlotRepresentation;
    CopiesByMoves copies;
    const uint64_t cageBase = copies.cageBase();
    const uint64_t bitsOfAnyKind[] = {
        0, 1, 2, 6, 7, 10, 0x80000000, 0x80000001, 0x80000002, 0x80000004, 0x80000005, 0x7fffffff, 0xffffffff, 0x100000000, 0x180000001,
        static_cast<uint64_t>(absentInteger), static_cast<uint64_t>(undefinedInteger), static_cast<uint64_t>(absentInteger) + 1, absentDouble, nullDouble, absentDouble - 1, std::bit_cast<uint64_t>(1.5), std::bit_cast<uint64_t>(-0.0),
        encoded(jsNumber(7)), encoded(jsNumber(-1)), cageBase, cageBase + 8, cageBase + 0x12345678, cageBase + MarkedBlockCage::maxSize - 8, cageBase + MarkedBlockCage::maxSize, cageBase - 8, UINT64_MAX,
    };
    const std::pair<SlotRepresentation, SlotRepresentation> pairs[] = {
        { Slot::Int32, Slot::Int32 }, { Slot::Uint32, Slot::Uint32 }, { Slot::Float, Slot::Float }, { Slot::Int32, Slot::OptionalInt32 }, { Slot::OptionalInt32, Slot::OptionalInt32 }, { Slot::Cell32, Slot::Cell32 }, { Slot::Cell32, Slot::OptionalCell32 }, { Slot::OptionalCell32, Slot::OptionalCell32 }, { Slot::LiteralString32, Slot::LiteralString32 },
        { Slot::Cell, Slot::Cell32 }, { Slot::Cell, Slot::OptionalCell32 }, { Slot::JSValue, Slot::OptionalCell32 },
        { Slot::Int64, Slot::Int64 }, { Slot::Double, Slot::Double }, { Slot::OptionalInt64, Slot::OptionalInt64 }, { Slot::OptionalDouble, Slot::OptionalDouble }, { Slot::JSValue, Slot::JSValue }, { Slot::Cell, Slot::Cell }, { Slot::LiteralString, Slot::LiteralString },
        { Slot::Boolean, Slot::Boolean }, { Slot::OptionalBoolean, Slot::OptionalBoolean }, { Slot::Uint8, Slot::Uint8 }, { Slot::Int8, Slot::Int8 },
    };
    const CopiedField alwaysMoved[] = { { Slot::Int32, Slot::Int32, 5 }, { Slot::Double, Slot::Double, std::bit_cast<uint64_t>(2.5) }, { Slot::Boolean, Slot::Boolean, 1 } };
    for (auto [from, to] : pairs) {
        for (uint64_t bits : bitsOfAnyKind) {
            copies.check(std::array { CopiedField { from, to, bits } });
            // In front of, between and behind moves of every other run.
            copies.check(std::array { CopiedField { from, to, bits }, alwaysMoved[0], alwaysMoved[1], alwaysMoved[2] });
            copies.check(std::array { alwaysMoved[2], alwaysMoved[1], CopiedField { from, to, bits }, alwaysMoved[0] });
            copies.check(std::array { alwaysMoved[1], alwaysMoved[0], alwaysMoved[2], CopiedField { from, to, bits } });
        }
    }
    copies.check({ });

    // What takes more than a load and a store is left to the operation.
    for (auto [from, to] : std::initializer_list<std::pair<SlotRepresentation, SlotRepresentation>> { { Slot::Int32, Slot::JSValue }, { Slot::OptionalInt32, Slot::JSValue }, { Slot::Int32, Slot::Double }, { Slot::Cell32, Slot::JSValue }, { Slot::Uint16, Slot::Uint16 }, { Slot::Double, Slot::OptionalDouble }, { Slot::Boolean, Slot::OptionalBoolean } }) {
        copies.describe(std::array { alwaysMoved[0], CopiedField { from, to, 1 } });
        copies.emptyTarget();
        AOT_TEST_CHECK(!copies.copy().structureOfSourceForStub && !copies.run());
    }
    for (uint64_t MappedCopy::* granuleBits : { &MappedCopy::granuleBitsOfSource, &MappedCopy::granuleBitsFilled }) {
        MappedCopy withExtension;
        withExtension.structureOfSource = std::bit_cast<StructureID>(1u << 4);
        withExtension.*granuleBits = 1;
        describeMovesForStub(withExtension);
        AOT_TEST_CHECK(!withExtension.structureOfSourceForStub);
    }

    const std::array fields { CopiedField { Slot::Int32, Slot::OptionalInt32, 5 }, CopiedField { Slot::Cell, Slot::OptionalCell32, cageBase + 64 }, CopiedField { Slot::Int32, Slot::OptionalInt32, 6 } };
    auto isCopiedByStub = [&](uint32_t run = CopiesByMoves::excludedRun, uint32_t count = 0) {
        uint32_t orderWordBefore = copies.target().at<uint32_t>(offsetOfOrderWord);
        bool isCopied = copies.run(run, count);
        AOT_TEST_CHECK(copies.target().at<uint32_t>(offsetOfOrderWord) == (isCopied ? CopiesByMoves::orderAfter : orderWordBefore));
        copies.emptyTarget();
        return isCopied;
    };
    auto isCopiedOnlyWithout = [&](auto& word, auto otherValue) {
        auto value = std::exchange(word, otherValue);
        bool isCopiedWith = isCopiedByStub();
        word = value;
        return !isCopiedWith && isCopiedByStub();
    };
    copies.describe(fields);
    copies.emptyTarget();
    AOT_TEST_CHECK(isCopiedByStub());
    AOT_TEST_CHECK(!isCopiedByStub(CopiesByMoves::excludedRun + 1) && !isCopiedByStub(0) && !isCopiedByStub(CopiesByMoves::excludedRun, 1));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.place(), static_cast<const MappedCopy*>(nullptr)));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().structureOfSourceForStub, StructureID()));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().structureOfTarget, copies.copy().structureOfSource));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().sourceWords, copies.copy().sourceWords + 64));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().targetWords, copies.copy().targetWords + 64));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().excludedRunNumberPlusOne, CopiesByMoves::excludedRun + 64));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.target().at<uint8_t>(JSCell::cellStateOffset()), static_cast<uint8_t>(CellState::PossiblyBlack)));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.structureOfTarget().at<uint32_t>(Structure::bitFieldOffset()), static_cast<uint32_t>(Structure::s_mayBePrototypeBits)));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.runner().instance().barrierThreshold, static_cast<uint32_t>(tautologicalThreshold)));

    // A build with assertions puts more instructions into the stubs, so the budget is held against the other builds only.
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && !ASSERT_ENABLED
    constexpr uint32_t budget = 78;
    copies.runner().setCountsInstructions(true);
    AOT_TEST_CHECK(copies.run() && copies.lastRun().instructionsInStubs);
    if (*copies.lastRun().instructionsInStubs != budget)
        dataLogLn("a copy of three fields by moves: ", *copies.lastRun().instructionsInStubs, " instructions in the stub, the budget is ", budget);
    AOT_TEST_CHECK(*copies.lastRun().instructionsInStubs <= budget);
#endif
}

// Every head of a call that returns nothing copies between records of one Structure by itself, also the heads that preserve registers.
static void testCopiesOfPropertiesByChunks()
{
    constexpr Entry operation = Entry::operationAOTCopyDataProperties;
    constexpr Entry operationByFieldMap = Entry::operationAOTCopyDataPropertiesByFieldMap;
    constexpr unsigned maxNumberOfFields = 5;
    constexpr size_t sizeOfCell = offsetOfFirstField + (maxNumberOfFields + 1) * sizeof(uint64_t);
    constexpr uint64_t untouched = 0xababababababababULL;
    constexpr uint32_t orderOfSource = orderWordOf(3, 2);
    WTF::Vector<StubKey> heads { callOperationFor(operation, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(operation)) };
    for (OperationCall::Saves saves : { OperationCall::Saves::CallerSavedRegisters, OperationCall::Saves::CallerSavedGPRs })
        heads.append(callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = saves }).withImmediate(offsetInRuntimeTable(operation)));
    StubRunner runner(StubPolicy { }, heads.span());

    std::array<FakeObject<sizeof(CompiledStructure)>, 2> structures;
    LayoutData layout { };
    layout.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    for (auto& structure : structures) {
        structure.at<uint8_t>(CompiledStructure::offsetOfFlags()) = static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyPlainSlots);
        structure.at<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &layout;
    }
    Instance& instance = runner.instance();
    instance.prefix().structureIDBase = structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
    instance.barrierThreshold = blackThreshold;

    FakeObject<sizeOfCell> target;
    FakeObject<sizeOfCell> source;
    source.setHeader(static_cast<uint32_t>(structures[0].address()), CompiledObjectType);
    source.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
    source.at<uint32_t>(offsetOfOrderWord) = orderOfSource;
    for (unsigned index = 0; index <= maxNumberOfFields; ++index)
        source.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) = 0x1111111111111111ULL * (index + 1);
    auto emptyTarget = [&] {
        target.setHeader(static_cast<uint32_t>(structures[0].address()), CompiledObjectType);
        target.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
        target.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
        target.at<uint32_t>(offsetOfOrderWord) = unorderedOrderWord;
        for (unsigned index = 0; index <= maxNumberOfFields; ++index)
            target.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) = untouched;
    };
    // True where the stub has made the copy. Else it has written nothing and has handed the operation what it was handed.
    Entry operationCalled = operation;
    auto isCopiedByStub = [&](StubKey head, uint32_t excludedRun = 0, uint32_t count = 0) {
        FakeObject<sizeOfCell> before = target;
        runner.countCallsOf(operation);
        runner.countCallsOf(operationByFieldMap);
        runner.recordCallsOf(operationCalled);
        StubRun run = runner.runOperationCall(head, { target.address(), source.address(), excludedRun, count });
        if (!run.calledOperation)
            return true;
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == target.address() && run.recordedArguments[1] == source.address());
        AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[2]) == excludedRun && static_cast<uint32_t>(run.recordedArguments[3]) == count && target.bytes == before.bytes);
        return false;
    };

    for (StubKey head : heads) {
        for (unsigned numberOfFields = 0; numberOfFields <= maxNumberOfFields; ++numberOfFields) {
            layout.size = offsetOfFirstField + numberOfFields * sizeof(uint64_t);
            emptyTarget();
            AOT_TEST_CHECK(isCopiedByStub(head) && target.at<uint32_t>(offsetOfOrderWord) == orderOfSource);
            for (unsigned index = 0; index <= maxNumberOfFields; ++index)
                AOT_TEST_CHECK(target.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) == (index < numberOfFields ? source.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) : untouched));
        }

        auto isCopiedOnlyWithout = [&](auto& word, auto otherValue) {
            emptyTarget();
            auto value = std::exchange(word, otherValue);
            bool isCopiedWith = isCopiedByStub(head);
            word = value;
            return !isCopiedWith && isCopiedByStub(head);
        };
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(JSCell::structureIDOffset()), static_cast<uint32_t>(structures[1].address())));
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfShapeWord), shapeWordOf(10)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfOrderWord), static_cast<uint32_t>(orderStoredOutOfLine)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(structures[0].at<uint8_t>(CompiledStructure::offsetOfFlags()), static_cast<uint8_t>(0)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(structures[0].at<uint32_t>(Structure::bitFieldOffset()), static_cast<uint32_t>(Structure::s_mayBePrototypeBits)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(instance.barrierThreshold, static_cast<uint32_t>(tautologicalThreshold)));
        emptyTarget();
        AOT_TEST_CHECK(!isCopiedByStub(head, 1) && !isCopiedByStub(head, 0, 1));
        target.at<uint32_t>(offsetOfOrderWord) = orderOfSource;
        AOT_TEST_CHECK(!isCopiedByStub(head));

        // Where a copy that takes no frame is kept at the place of this one, the operation is called that looks for it.
        MappedCopy kept;
        const MappedCopy*& place = instance.allocation.copiesWithoutFrame[AllocationWords::placeOfCopyWithoutFrame(target.at<uint64_t>(offsetOfOrderWord), source.at<uint64_t>(offsetOfOrderWord), 0)];
        place = &kept;
        operationCalled = operationByFieldMap;
        AOT_TEST_CHECK(!isCopiedByStub(head));
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(JSCell::structureIDOffset()), static_cast<uint32_t>(structures[1].address())));
        place = nullptr;
        operationCalled = operation;
    }
}

static void testInstructionsOfOperationCall()
{
    // A build with assertions puts more instructions into the stubs, so the budget is held against the other builds only.
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && !ASSERT_ENABLED
    constexpr uint32_t budget = 10;
    StubRunner runner;
    runner.setCountsInstructions(true);
    StubRun run = runner.run(callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance), { }, offsetInRuntimeTable(Entry::operationAOTValueAdd));
    AOT_TEST_CHECK(run.calledOperation && run.instructionsInStubs);
    if (*run.instructionsInStubs != budget)
        dataLogLn("a call of an operation: ", *run.instructionsInStubs, " instructions in the stub, the budget is ", budget);
    AOT_TEST_CHECK(*run.instructionsInStubs <= budget);
#endif
}

std::span<const NativeTest> stubRuntimeTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/allocation-at-sites"_s, ImageTests::whenExecutable<testAllocationAtSites> },
        { "stubs/slim-closures"_s, ImageTests::whenExecutable<testSlimClosures> },
        { "stubs/created-fields-of-objects-with-extra-properties"_s, ImageTests::whenExecutable<testCreatedFieldsOfObjectsWithExtraProperties> },
        { "stubs/created-fields-remembered-at-their-place"_s, ImageTests::whenExecutable<testCreatedFieldsRememberedAtTheirPlace> },
        { "stubs/object-copies"_s, ImageTests::whenExecutable<testObjectCopies> },
        { "stubs/copies-of-properties-by-moves"_s, ImageTests::whenExecutable<testCopiesOfPropertiesByMoves> },
        { "stubs/copies-of-properties-by-chunks"_s, ImageTests::whenExecutable<testCopiesOfPropertiesByChunks> },
        { "stubs/binary-operation-string-order"_s, ImageTests::whenExecutable<testBinaryOperationStringOrder> },
        { "stubs/binary-operations-on-numbers"_s, ImageTests::whenExecutable<testBinaryOperationsOnNumbers> },
        { "stubs/unary-operations"_s, ImageTests::whenExecutable<testUnaryOperations> },
        { "stubs/instructions-of-an-operation-call"_s, ImageTests::whenExecutable<testInstructionsOfOperationCall> },
        { "stubs/boxed-call-handlers"_s, testBoxedCallHandlers },
        { "stubs/boxed-call-table"_s, runNativeTestWithLoadedProgram<stubRuntimeTests, emptyProgramText, testBoxedCallTable> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
