/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTFieldMoves.h"
#include "AOTLayoutData.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTMappedCopyInlines.h"
#include "AOTObjectWords.h"
#include "AOTSlotAccess.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubWords.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsArithmetic.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsClosures.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"
#include "CompleteSubspace.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include "MarkedSpace.h"
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

static int64_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES findNoOrderInPlace(JSCell*, JSCell*)
{
    return noOrderOfStringsInPlace;
}

static void testBinaryOperationStringOrder()
{
    StubRunner runner;
    runner.install(Entry::operationAOTCompareStringImpls, operationAOTCompareStringImpls);
    runner.install(Entry::operationAOTCompareStringsInPlace, findNoOrderInPlace);

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

    for (double number : insideInt64) {
        StubRun run = runner.run(numberOfAnyValue(), { encoded(jsNumber(number)) });
        AOT_TEST_CHECK(!run.calledOperation && run.result == encoded(jsNumber(number)));
    }
    for (uint64_t value : { encoded(jsUndefined()), encoded(jsNull()), encoded(jsBoolean(true)), encoded(jsBoolean(false)), string.address() })
        checkLeftToOperation(runner, numberOfAnyValue(), Entry::operationAOTNumberOfAnyValue, { value });
    for (const UnaryCase& unaryCase : unaryCases) {
        StubKey key = unaryOperation(unaryCase.form);
        for (double number : insideInt64) {
            StubRun run = runner.run(key, { encoded(jsNumber(number)) });
            AOT_TEST_CHECK(!run.calledOperation && isNumberInCanonicalFormWithSign(run.result, unaryCase.apply(number)));
        }
        for (double number : outsideInt64) {
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
        for (bool isAfterInt32Check : { false, true }) {
            if (isAfterInt32Check && !hasFormAfterInt32Check(binaryCase.operation))
                continue;
            StubRun run = runner.run(binaryOperation(binaryCase.operation, isAfterInt32Check), { encoded(jsNumber(binaryCase.left)), encoded(jsNumber(binaryCase.right)) });
            AOT_TEST_CHECK(!run.calledOperation && isNumberInCanonicalForm(run.result, binaryCase.expected));
        }
    }

    const uint64_t seven = encoded(jsNumber(7));
    for (uint64_t other : { seven, encoded(jsNumber(2.5)), encoded(jsBoolean(true)), encoded(jsUndefined()) }) {
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mod, false), Entry::operationAOTValueMod, { string.address(), other });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mod, false), Entry::operationAOTValueMod, { other, string.address() });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mul, false), Entry::operationAOTValueMul, { string.address(), other });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Mul, true), Entry::operationAOTValueMul, { other, string.address() });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Div, false), Entry::operationAOTValueDiv, { string.address(), other });
        checkLeftToOperation(runner, binaryOperation(BinaryOperator::Div, false), Entry::operationAOTValueDiv, { other, string.address() });
        const std::pair<BinaryOperator, Entry> operatorsOfTwoForms[] = { { BinaryOperator::Add, Entry::operationAOTValueAdd }, { BinaryOperator::Sub, Entry::operationAOTValueSub }, { BinaryOperator::Less, Entry::operationAOTCompareLess }, { BinaryOperator::LessEq, Entry::operationAOTCompareLessEq }, { BinaryOperator::Greater, Entry::operationAOTCompareGreater }, { BinaryOperator::GreaterEq, Entry::operationAOTCompareGreaterEq } };
        for (auto [operation, entry] : operatorsOfTwoForms) {
            for (bool isAfterInt32Check : { false, true }) {
                checkLeftToOperation(runner, binaryOperation(operation, isAfterInt32Check), entry, { string.address(), other });
                checkLeftToOperation(runner, binaryOperation(operation, isAfterInt32Check), entry, { other, string.address() });
            }
        }
    }

    constexpr double notANumber = std::numeric_limits<double>::quiet_NaN();
    const std::pair<uint64_t, double> valuesThatAreNoCells[] = { { encoded(jsUndefined()), notANumber }, { encoded(jsNull()), 0 }, { encoded(jsBoolean(true)), 1 }, { encoded(jsBoolean(false)), 0 }, { encoded(jsNumber(2.5)), 2.5 }, { encoded(jsNumber(-0.0)), -0.0 }, { encoded(jsNaN()), notANumber }, { encoded(jsNumber(-1)), -1 }, { encoded(jsNumber(7)), 7 } };
    for (auto [left, leftAsNumber] : valuesThatAreNoCells) {
        for (auto [right, rightAsNumber] : valuesThatAreNoCells) {
            const std::pair<BinaryOperator, double> arithmetic[] = { { BinaryOperator::Add, leftAsNumber + rightAsNumber }, { BinaryOperator::Sub, leftAsNumber - rightAsNumber }, { BinaryOperator::Mul, leftAsNumber * rightAsNumber }, { BinaryOperator::Div, leftAsNumber / rightAsNumber }, { BinaryOperator::Mod, std::fmod(leftAsNumber, rightAsNumber) } };
            for (auto [operation, expected] : arithmetic) {
                for (bool isAfterInt32Check : { false, true }) {
                    if (isAfterInt32Check && !hasFormAfterInt32Check(operation))
                        continue;
                    StubRun run = runner.run(binaryOperation(operation, isAfterInt32Check), { left, right });
                    AOT_TEST_CHECK(!run.calledOperation && (std::isnan(expected) ? isNumberInCanonicalForm(run.result, expected) : isNumberInCanonicalFormWithSign(run.result, expected)));
                }
            }
            const std::pair<BinaryOperator, bool> relations[] = { { BinaryOperator::Less, leftAsNumber < rightAsNumber }, { BinaryOperator::LessEq, leftAsNumber <= rightAsNumber }, { BinaryOperator::Greater, leftAsNumber > rightAsNumber }, { BinaryOperator::GreaterEq, leftAsNumber >= rightAsNumber } };
            for (auto [operation, expected] : relations) {
                for (bool isAfterInt32Check : { false, true }) {
                    if (isAfterInt32Check && !hasFormAfterInt32Check(operation))
                        continue;
                    StubRun run = runner.run(binaryOperation(operation, isAfterInt32Check), { left, right });
                    AOT_TEST_CHECK(!run.calledOperation && static_cast<uint32_t>(run.result) == static_cast<uint32_t>(expected));
                }
            }
        }
    }
}

namespace {

struct OperatorWithDouble {
    BinaryOperator operation;
    Entry entry;
    double (*apply)(double left, double right);
};

} // anonymous namespace

static constexpr OperatorWithDouble operatorsWithDouble[] = {
    { BinaryOperator::Add, Entry::operationAOTAddNumberAndValueAsNumber, [](double left, double right) { return left + right; } },
    { BinaryOperator::Sub, Entry::operationAOTSubNumberAndValueAsNumber, [](double left, double right) { return left - right; } },
    { BinaryOperator::Mul, Entry::operationAOTMulNumberAndValueAsNumber, [](double left, double right) { return left * right; } },
    { BinaryOperator::Div, Entry::operationAOTDivNumberAndValueAsNumber, [](double left, double right) { return left / right; } },
};

static double JIT_OPERATION_ATTRIBUTES negateNumberBesideValue(Instance*, EncodedJSValue, uint32_t, double number)
{
    return -number;
}

static void testBinaryOperationsWithDouble()
{
    constexpr double number = 6.5;
    constexpr uint64_t bitsOfNumber = std::bit_cast<uint64_t>(number);
    FakeString string("7"_s, CharacterWidth::Is8Bit);
    const uint64_t cell = string.address();
    const std::pair<uint64_t, double> valuesThatAreNoCells[] = { { encoded(jsNull()), 0 }, { encoded(jsBoolean(true)), 1 }, { encoded(jsBoolean(false)), 0 } };

    StubRunner runner;
    for (const OperatorWithDouble& operatorWithDouble : operatorsWithDouble) {
        for (bool numberIsOnRight : { false, true }) {
            StubKey key = binaryWithDouble(operatorWithDouble.operation, numberIsOnRight);
            const uint64_t side = NumberAndValueBits::encode(0, numberIsOnRight);
            for (double other : { 4.0, -0.25, 0.0, -0.0 }) {
                double expected = numberIsOnRight ? operatorWithDouble.apply(other, number) : operatorWithDouble.apply(number, other);
                StubRun run = runner.run(key, { bitsOfNumber, encoded(jsNumber(other)), side });
                AOT_TEST_CHECK(!run.calledOperation && run.result == std::bit_cast<uint64_t>(expected));
            }

            for (auto [other, otherAsNumber] : valuesThatAreNoCells) {
                double expected = numberIsOnRight ? operatorWithDouble.apply(otherAsNumber, number) : operatorWithDouble.apply(number, otherAsNumber);
                StubRun run = runner.run(key, { bitsOfNumber, other, side });
                AOT_TEST_CHECK(!run.calledOperation && run.result == std::bit_cast<uint64_t>(expected));
            }
            StubRun ofUndefined = runner.run(key, { bitsOfNumber, encoded(jsUndefined()), side });
            AOT_TEST_CHECK(!ofUndefined.calledOperation && std::isnan(std::bit_cast<double>(ofUndefined.result)));

            runner.recordCallsOf(operatorWithDouble.entry);
            StubRun run = runner.run(key, { bitsOfNumber, cell, side });
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == cell && static_cast<uint32_t>(run.recordedArguments[1]) == side);
            runner.install(operatorWithDouble.entry, negateNumberBesideValue);
            AOT_TEST_CHECK(runner.run(key, { bitsOfNumber, cell, side }).result == std::bit_cast<uint64_t>(-number));
            runner.countCallsOf(operatorWithDouble.entry);
        }
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

static void testAllocationFromTemplates()
{
    struct PlaceOfPattern {
        SlotRepresentation representation;
        size_t offset;
    };

    StubRunner runner;
    runner.recordCallsOf(Entry::operationAOTAllocateSlowFromTemplate);
    AllocationSites sites(runner);

    constexpr uint8_t byteOfNoSlot = 0x5c;
    FakeObject<sizeof(FakeCell)> body;
    body.bytes.fill(byteOfNoSlot);
    WTF::Vector<PlaceOfPattern> places;
    size_t endOfPatterns = 0;
    unsigned numberOfPatternsThatAreNotZero = 0;
    for (unsigned width : { 8u, 4u, 2u, 1u }) {
        for (unsigned index = 0; index < numberOfEnumerators<SlotRepresentation>; ++index) {
            auto representation = static_cast<SlotRepresentation>(index);
            SlotTraits traits = traitsOf(representation);
            if (!traits.hasAbsent || traits.width != width)
                continue;
            clearSlot(representation, body.bytes.data() + endOfPatterns);
            places.append({ representation, endOfPatterns });
            endOfPatterns += width;
            numberOfPatternsThatAreNotZero += !!traits.absentBits;
        }
    }
    AOT_TEST_CHECK(numberOfPatternsThatAreNotZero >= 4 && endOfPatterns <= sizeOfCellTemplate(CellInitialization::HeaderAndWords, sizeof(FakeCell)));

    for (CellInitialization initialization : { CellInitialization::Header, CellInitialization::HeaderAndWords }) {
        const size_t offsetOfBody = offsetOfTemplateInCell(initialization);
        for (size_t size = 2 * copyChunkBytes; size <= sizeof(FakeCell); size += copyChunkBytes) {
            StubKey key = allocateCellFromTemplate(initialization, size - sizeof(uint64_t));
            AOT_TEST_CHECK(key == allocateCellFromTemplate(initialization, size) && sizeOfCellTemplate(initialization, size - sizeof(uint64_t)) == size - offsetOfBody);

            FakeCell cell;
            sites.makeRoomFor(cell);
            StubRun run = runner.run(key, { AllocationSites::siteWithAllocator, body.address() });
            AOT_TEST_CHECK(!run.calledOperation && run.result == cell.address());
            AOT_TEST_CHECK(sites.allocator.intervalStart() == cell.address() + (size <= maxCellSizeInKey ? size : sizeof(FakeCell)));
            sites.allocator.makeRoomAt(0, 0);

            AOT_TEST_CHECK(cell.words[0] == AllocationSites::header);
            AOT_TEST_CHECK(!storesWords(initialization) || cell.words[1] == AllocationSites::orderAndShapeWords);
            std::span<const uint8_t> bytesOfCell = asBytes(std::span<const uint64_t>(cell.words));
            AOT_TEST_CHECK(equalSpans(bytesOfCell.subspan(offsetOfBody, size - offsetOfBody), std::span<const uint8_t>(body.bytes).first(size - offsetOfBody)));
            AOT_TEST_CHECK(std::ranges::all_of(std::span(cell.words).subspan(size / sizeof(uint64_t)), [](uint64_t word) { return word == FakeCell::untouched; }));
            unsigned numberOfPatternsInCell = 0;
            for (const PlaceOfPattern& place : places) {
                SlotTraits traits = traitsOf(place.representation);
                if (place.offset + traits.width > size - offsetOfBody)
                    continue;
                uint64_t bits = 0;
                memcpy(&bits, bytesOfCell.data() + offsetOfBody + place.offset, traits.width);
                AOT_TEST_CHECK(bits == traits.absentBits);
                ++numberOfPatternsInCell;
            }
            AOT_TEST_CHECK(size < sizeof(FakeCell) || numberOfPatternsInCell == places.size());

            for (uint32_t site : { AllocationSites::siteWithAllocator, AllocationSites::siteWithoutAllocator, AllocationSites::siteWithoutRecord }) {
                FakeCell fromOperation;
                runner.setResultOfRecordedOperation(fromOperation.address());
                run = runner.run(key, { site, body.address() });
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == site && run.recordedArguments[1] == body.address());
                AOT_TEST_CHECK(run.result == fromOperation.address() && fromOperation.isUntouched());
            }
        }
    }

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && !ASSERT_ENABLED
    runner.setCountsInstructions(true);
    auto instructionsOf = [&](StubKey key, std::initializer_list<uint64_t> operands) {
        FakeCell cell;
        sites.makeRoomFor(cell);
        StubRun run = runner.run(key, operands);
        sites.allocator.makeRoomAt(0, 0);
        AOT_TEST_CHECK(!run.calledOperation && run.instructionsInStubs);
        return *run.instructionsInStubs;
    };
    for (CellInitialization initialization : { CellInitialization::Header, CellInitialization::HeaderAndWords }) {
        CellInitialization withZeros = storesWords(initialization) ? CellInitialization::HeaderAndWordsAndZeros : CellInitialization::HeaderAndZeros;
        for (size_t size = 2 * copyChunkBytes; size <= maxCellSizeInKey; size += copyChunkBytes) {
            uint32_t bare = instructionsOf(allocateCell(initialization, size), { AllocationSites::siteWithAllocator });
            uint32_t zeroed = instructionsOf(allocateCell(withZeros, size), { AllocationSites::siteWithAllocator });
            uint32_t copied = instructionsOf(allocateCellFromTemplate(initialization, size), { AllocationSites::siteWithAllocator, body.address() });
            if (copied - bare + instructionsToNameTemplate > instructionsToCopyBody(initialization, size) || zeroed - bare < instructionsToZeroBody(withZeros, size, true))
                dataLogLn("a cell of ", size, " bytes, form ", static_cast<unsigned>(initialization), ": ", bare, " instructions bare, ", zeroed, " with zeros, ", copied, " from a template");
            AOT_TEST_CHECK(copied - bare + instructionsToNameTemplate <= instructionsToCopyBody(initialization, size));
            AOT_TEST_CHECK(zeroed - bare >= instructionsToZeroBody(withZeros, size, true));
        }
    }
#endif
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

    constexpr uint64_t headerInFrame = header | static_cast<uint64_t>(frameCellState) << 8 * JSCell::cellStateOffset();
    static_assert(headerInFrame != header && frameCellState == UINT8_MAX);
    FakeCell inFrame;
    run = runner.run(newSlimClosureInFrame(kind, 1), { function, inFrame.address(), capture });
    AOT_TEST_CHECK(!run.calledOperation && run.result == inFrame.address());
    AOT_TEST_CHECK(inFrame.words[0] == headerInFrame && inFrame.words[1] == closureWords[function] && inFrame.words[2] == capture && !inFrame.words[3] && inFrame.words[4] == FakeCell::untouched);

    FakeCell withTwoCaptures;
    run = runner.run(newSlimClosureInFrame(kind, 2), { function, withTwoCaptures.address(), capture, ~capture });
    AOT_TEST_CHECK(!run.calledOperation && run.result == withTwoCaptures.address());
    AOT_TEST_CHECK(withTwoCaptures.words[0] == headerInFrame && withTwoCaptures.words[1] == closureWords[function] && withTwoCaptures.words[2] == capture && withTwoCaptures.words[3] == ~capture && !withTwoCaptures.words[4] && withTwoCaptures.words[5] == FakeCell::untouched);

    FakeCell withoutCaptures;
    run = runner.run(newSlimClosureInFrame(kind, 0), { function, withoutCaptures.address() });
    AOT_TEST_CHECK(!run.calledOperation && run.result == withoutCaptures.address());
    AOT_TEST_CHECK(withoutCaptures.words[0] == headerInFrame && withoutCaptures.words[1] == closureWords[function] && !withoutCaptures.words[2] && withoutCaptures.words[3] == FakeCell::untouched);
}

static void testSlotOfFrameThatIsUsedAgainForgetsCopyInHeap()
{
    constexpr uint32_t function = 3;
    constexpr uint64_t header = 0x0123456789abcdefULL;
    constexpr std::array<uint64_t, ClosureRequest::maxSlimClosureCapturesInRegisters> captures { 0x5555666677778888ULL, 0x1111222233334444ULL, 0x9999aaaabbbbccccULL, 0xddddeeeeffff0000ULL };
    constexpr unsigned kind = 1;
    constexpr unsigned count = 7;
    constexpr size_t firstCapture = offsetOfSlimClosureCaptures / sizeof(uint64_t);
    constexpr size_t copyBehindOneCapture = offsetOfCopyInHeapOfClosureInFrame(offsetOfSlimClosureCaptures, 1) / sizeof(uint64_t);
    constexpr size_t copyBehindAllCaptures = offsetOfCopyInHeapOfClosureInFrame(offsetOfSlimClosureCaptures, count) / sizeof(uint64_t);
    static_assert(count > captures.size() && sizeOfClosureInFrame(offsetOfSlimClosureCaptures, count) < sizeof(FakeCell));
    const std::array<uint64_t, 4> closureWords { 0, 0, 0, 0x1122334455667788ULL };

    StubRunner runner;
    runner.instance().functionTables.closureWords = closureWords.data();
    runner.instance().slimClosureHeaders[kind] = header;

    FakeCell copyInHeap;
    FakeCell slot;
    for (uint64_t capture : captures) {
        StubRun run = runner.run(newSlimClosureInFrame(kind, 1), { function, slot.address(), capture });
        AOT_TEST_CHECK(!run.calledOperation && run.result == slot.address());
        AOT_TEST_CHECK(slot.words[firstCapture] == capture && !slot.words[copyBehindOneCapture] && slot.words[copyBehindOneCapture + 1] == FakeCell::untouched);
        slot.words[copyBehindOneCapture] = copyInHeap.address();
    }

    FakeCell largeSlot;
    for (unsigned making = 0; making < 2; ++making) {
        StubRun run = runner.run(newSlimClosureInFrame(kind, count), { function, largeSlot.address(), captures[0], captures[1], captures[2], captures[3] });
        AOT_TEST_CHECK(!run.calledOperation && run.result == largeSlot.address());
        AOT_TEST_CHECK(std::ranges::equal(std::span(largeSlot.words).subspan(firstCapture, captures.size()), captures));
        AOT_TEST_CHECK(std::ranges::all_of(std::span(largeSlot.words).subspan(firstCapture + captures.size(), count - captures.size()), [](uint64_t word) { return word == FakeCell::untouched; }));
        AOT_TEST_CHECK(!largeSlot.words[copyBehindAllCaptures] && largeSlot.words[copyBehindAllCaptures + 1] == FakeCell::untouched);
        largeSlot.words[copyBehindAllCaptures] = copyInHeap.address();
    }
    AOT_TEST_CHECK(copyInHeap.isUntouched());
}

static void testSlimClosuresWithMoreCapturesThanRegisters()
{
    constexpr uint32_t function = 3;
    constexpr uint64_t header = 0x0123456789abcdefULL;
    constexpr std::array<uint64_t, ClosureRequest::maxSlimClosureCapturesInRegisters> captures { 0x5555666677778888ULL, 0x1111222233334444ULL, 0x9999aaaabbbbccccULL, 0xddddeeeeffff0000ULL };
    constexpr unsigned kind = 1;
    constexpr unsigned count = 7;
    constexpr uint64_t request = function | static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift;
    constexpr unsigned cellSize = 5 * MarkedSpace::sizeStep;
    constexpr size_t firstCapture = offsetOfSlimClosureCaptures / sizeof(uint64_t);
    static_assert(offsetOfSlimClosureCaptures + count * sizeof(uint64_t) <= cellSize && cellSize <= sizeof(FakeCell));
    const std::array<uint64_t, 4> closureWords { 0, 0, 0, 0x1122334455667788ULL };

    StubRunner runner;
    runner.recordCallsOf(Entry::operationAOTNewClosureSlow);
    FakeAllocator allocator { cellSize };
    runner.instance().functionTables.closureWords = closureWords.data();
    runner.instance().closures.slimClosureAllocators[4] = reinterpret_cast<LocalAllocator*>(&allocator);
    runner.instance().slimClosureHeaders[kind] = header;

    FakeCell cell;
    allocator.makeRoomAt(cell.address(), cellSize);
    StubRun run = runner.run(newSlimClosure(kind, count), { function, captures[0], captures[1], captures[2], captures[3] });
    AOT_TEST_CHECK(!run.calledOperation && run.result == cell.address() && !allocator.hasRoom());
    AOT_TEST_CHECK(cell.words[0] == header && cell.words[1] == closureWords[function]);
    AOT_TEST_CHECK(std::ranges::equal(std::span(cell.words).subspan(firstCapture, captures.size()), captures));
    AOT_TEST_CHECK(std::ranges::all_of(std::span(cell.words).subspan(firstCapture + captures.size()), [](uint64_t word) { return word == FakeCell::untouched; }));

    FakeCell fromOperation;
    runner.setResultOfRecordedOperation(fromOperation.address());
    run = runner.run(newSlimClosure(kind, count), { function, captures[0], captures[1], captures[2], captures[3] });
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == request);
    AOT_TEST_CHECK(std::ranges::equal(std::span(run.recordedArguments).subspan(1), captures));
    AOT_TEST_CHECK(run.result == fromOperation.address() && fromOperation.isUntouched());
}

static UGPRPair JIT_OPERATION_ATTRIBUTES recordTypedArrayRequest(Instance* instance, uint32_t type, double length)
{
    StubEnvironment& environment = StubEnvironment::of(instance);
    ++environment.numberOfOperationCalls;
    ++environment.numberOfRecordedCalls;
    environment.recordedArguments = { type, std::bit_cast<uint64_t>(length), 0, 0, 0 };
    return makeUGPRPair(environment.resultOfRecordedOperation, 0);
}

static void testTypedArraysWithLength()
{
    constexpr uint64_t header = 0x0123456789abcdefULL;
    constexpr uint32_t type = TypeFloat32;
    constexpr uint32_t typeWithoutRecord = TypeInt8;
    constexpr unsigned elementSize = sizeof(float);
    constexpr size_t maxBytesFromStub = 256;
    constexpr size_t numberOfSizeSteps = maxBytesFromStub / MarkedSpace::sizeStep + 1;
    constexpr uint8_t untouchedByte = 0xab;
    static_assert(sizeof(JSArrayBufferView) < sizeof(FakeCell) && !(sizeof(JSArrayBufferView) % sizeof(uint64_t)) && !(JSArrayBufferView::offsetOfMode() % sizeof(uint64_t)));

    StubRunner runner;
    runner.install(Entry::operationAOTNewTypedArray, recordTypedArrayRequest);
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + numberOfSizeSteps * sizeof(void*)> space;
    FakeObject<maxBytesFromStub + MarkedSpace::sizeStep> room;
    FakeAllocator views { sizeof(JSArrayBufferView) };
    runner.instance().allocation.primitiveAuxiliarySpace = space.bytes.data();
    runner.instance().allocation.typedArrayAllocations[type] = { &views, header, elementSize, 0 };

    auto allocatorOfStorage = [&](size_t bytes) -> void*& {
        return space.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(bytes) * sizeof(void*));
    };
    auto wordOf = [](const FakeCell& cell, ptrdiff_t offset) {
        return cell.words[offset / sizeof(uint64_t)];
    };

    for (uint32_t length : { 1u, 3u, 5u, 64u }) {
        const size_t bytes = JSArrayBufferView::sizeOf(length, elementSize);
        const size_t sizeOfStorageCell = MarkedSpace::sizeClassToIndex(bytes) * MarkedSpace::sizeStep;
        FakeAllocator storages { static_cast<uint32_t>(sizeOfStorageCell) };
        FakeCell view;
        room.bytes.fill(untouchedByte);
        storages.makeRoomAt(room.address(), sizeOfStorageCell);
        views.makeRoomAt(view.address(), sizeof(JSArrayBufferView));
        allocatorOfStorage(bytes) = &storages;

        StubRun run = runner.run(newTypedArray(), { type, std::bit_cast<uint64_t>(static_cast<double>(length)) });
        AOT_TEST_CHECK(!run.calledOperation && run.result == view.address() && !storages.hasRoom() && !views.hasRoom());
        AOT_TEST_CHECK(view.words[0] == header && !wordOf(view, JSObject::butterflyOffset()));
        AOT_TEST_CHECK(wordOf(view, JSArrayBufferView::offsetOfVector()) == room.address());
        AOT_TEST_CHECK(wordOf(view, JSArrayBufferView::offsetOfLength()) == length && !wordOf(view, JSArrayBufferView::offsetOfByteOffset()));
        AOT_TEST_CHECK(static_cast<uint8_t>(wordOf(view, JSArrayBufferView::offsetOfMode())) == FastTypedArray);
        AOT_TEST_CHECK(wordOf(view, sizeof(JSArrayBufferView)) == FakeCell::untouched);
        AOT_TEST_CHECK(std::ranges::all_of(std::span(room.bytes).first(bytes), [](uint8_t byte) { return !byte; }));
        AOT_TEST_CHECK(std::ranges::all_of(std::span(room.bytes).subspan(bytes), [](uint8_t byte) { return byte == untouchedByte; }));
        allocatorOfStorage(bytes) = nullptr;
    }

    FakeAllocator storages { maxBytesFromStub };
    FakeCell view;
    auto makeRoom = [&] {
        storages.makeRoomAt(room.address(), maxBytesFromStub);
        views.makeRoomAt(view.address(), sizeof(JSArrayBufferView));
    };
    auto tookNothing = [&] {
        return view.isUntouched() && storages.intervalStart() == room.address() && views.intervalStart() == view.address();
    };
    auto isLeftToOperation = [&](uint32_t requestedType, double length) {
        FakeCell fromOperation;
        runner.setResultOfRecordedOperation(fromOperation.address());
        StubRun run = runner.run(newTypedArray(), { requestedType, std::bit_cast<uint64_t>(length) });
        return run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == requestedType && run.recordedArguments[1] == std::bit_cast<uint64_t>(length) && run.result == fromOperation.address() && fromOperation.isUntouched();
    };
    for (size_t bytes = MarkedSpace::sizeStep; bytes <= maxBytesFromStub; bytes += MarkedSpace::sizeStep)
        allocatorOfStorage(bytes) = &storages;
    makeRoom();

    constexpr double infinity = std::numeric_limits<double>::infinity();
    for (double length : { 0.0, -0.0, -1.0, 0.5, 1.5, 65.0, 257.0, 4294967297.0, -4294967295.0, 1e300, infinity, -infinity, PNaN, -PNaN })
        AOT_TEST_CHECK(isLeftToOperation(type, length) && tookNothing());
    AOT_TEST_CHECK(isLeftToOperation(typeWithoutRecord, 1) && tookNothing());

    allocatorOfStorage(sizeof(uint64_t)) = nullptr;
    AOT_TEST_CHECK(isLeftToOperation(type, 1) && tookNothing());
    allocatorOfStorage(sizeof(uint64_t)) = &storages;

    storages.makeRoomAt(0, 0);
    AOT_TEST_CHECK(isLeftToOperation(type, 1) && view.isUntouched() && views.intervalStart() == view.address());

    makeRoom();
    views.makeRoomAt(0, 0);
    AOT_TEST_CHECK(isLeftToOperation(type, 1) && view.isUntouched());
}

namespace {

struct ObjectWithKeyOrder {
    static constexpr uint32_t shape = 9;
    static constexpr uint32_t structureID = 0x100;
    static constexpr std::array<uint32_t, 2> slotKeys { offsetOfFirstField, offsetOfFirstField + sizeof(uint64_t) };

    explicit ObjectWithKeyOrder(StubRunner& runner)
        : runner(runner)
        , structure(reinterpret_cast<uint8_t*>(runner.instance().prefix().structureIDBase) + structureID)
        , runs(std::make_unique<KeyOrderRun[]>(numberOfKeyOrderRuns))
    {
        auto offsetInTables = [&](const void* address) {
            return static_cast<uint32_t>(static_cast<const uint8_t*>(address) - reinterpret_cast<const uint8_t*>(this));
        };
        offsetOfOrder = offsetInTables(order.data());
        layout.shape = shape;
        layout.numberOfFields = slotKeys.size();
        layout.numberOfOrders = 1;
        layout.ordersOffset = offsetInTables(&offsetOfOrder);
        for (size_t index = 0; index < slotKeys.size(); ++index)
            fields[index].slot = SlotData::decode(slotKeys[index]);
        AOT_TEST_CHECK(layout.fields().data() == fields.data());

        Instance& instance = runner.instance();
        instance.image.tables = reinterpret_cast<const ProgramTables*>(this);
        instance.objects.keyOrderTransitions = transitions.data();
        instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(transitions.size() - 1);
        instance.objects.keyOrderRuns = runs.get();
        inStructure<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &layout;
        inStructure<uint16_t>(CompiledStructure::offsetOfNumberOfExtraKeys()) = CompiledStructure::unknownNumberOfExtraKeys;
        object.setHeader(structureID, CompiledObjectType);
        object.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(shape);
    }

    template<typename T> T& inStructure(ptrdiff_t offset) { return *reinterpret_cast<T*>(structure + offset); }
    uint32_t& orderWord() { return object.at<uint32_t>(offsetOfOrderWord); }

    void publish(uint32_t orderAndCountBefore, uint32_t key, uint32_t orderAndCountAfter)
    {
        uint64_t wordsBefore = static_cast<uint64_t>(shapeWordOf(shape)) << 32 | orderAndCountBefore;
        size_t place = KeyOrderTransition::hashOf(wordsBefore, key) % transitions.size();
        while (!transitions[place].isEmpty())
            place = (place + 1) % transitions.size();
        transitions[place] = { wordsBefore, key, orderAndCountAfter };
    }
    void forgetTransitions() { transitions.fill({ }); }
    void forgetRuns() { std::fill_n(runs.get(), numberOfKeyOrderRuns, KeyOrderRun { }); }

    bool create(uint32_t orderWordBefore, uint64_t key)
    {
        orderWord() = orderWordBefore;
        runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
        StubRun run = runner.run(didCreateField(), { object.address(), key });
        runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
        AOT_TEST_CHECK(!run.calledOperation || (run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == key));
        AOT_TEST_CHECK(!run.calledOperation || orderWord() == orderWordBefore);
        return !run.calledOperation;
    }

    uint32_t offsetOfOrder { 0 };
    std::array<uint16_t, 3> order { 2, 0, 1 };
    LayoutData layout { };
    std::array<FieldData, 2> fields { };
    std::array<KeyOrderTransition, 16> transitions { };
    FakeObject<offsetOfFirstField + 2 * sizeof(uint64_t)> object;
    StubRunner& runner;
    uint8_t* structure;
    std::unique_ptr<KeyOrderRun[]> runs;
};

} // anonymous namespace

static void testCreatedFieldsInOrdersOfImage()
{
    constexpr auto slotKeys = ObjectWithKeyOrder::slotKeys;
    constexpr uint32_t otherOrder = orderWordOf(1, 1);
    StubRunner runner;
    ObjectWithKeyOrder object(runner);

    AOT_TEST_CHECK(object.create(orderWordOf(0, 0), slotKeys[0]) && object.orderWord() == orderWordOf(0, 1));
    AOT_TEST_CHECK(object.create(orderWordOf(0, 1), slotKeys[1]) && object.orderWord() == orderWordOf(0, 2));
    object.forgetRuns();

    AOT_TEST_CHECK(!object.create(orderWordOf(0, 0), slotKeys[1]));
    AOT_TEST_CHECK(!object.create(orderWordOf(0, 2), slotKeys[0]));
    AOT_TEST_CHECK(!object.create(otherOrder, slotKeys[0]));
    object.publish(orderWordOf(0, 0), slotKeys[1], otherOrder);
    AOT_TEST_CHECK(object.create(orderWordOf(0, 0), slotKeys[1]) && object.orderWord() == otherOrder);

    object.forgetTransitions();
    AOT_TEST_CHECK(object.create(orderWordOf(0, 0), slotKeys[1]) && object.orderWord() == otherOrder);
    AOT_TEST_CHECK(object.create(orderWordOf(0, 0) | orderWordHasFieldOutsideSlot, slotKeys[1]) && object.orderWord() == (otherOrder | orderWordHasFieldOutsideSlot));
    object.forgetRuns();
    AOT_TEST_CHECK(!object.create(orderWordOf(0, 0), slotKeys[1]));

    AOT_TEST_CHECK(!object.create(orderWordOf(0, 0) | orderWordHasFieldOutsideSlot, slotKeys[0]));
    object.publish(orderWordOf(0, 0), slotKeys[0], orderWordOf(0, 1));
    AOT_TEST_CHECK(object.create(orderWordOf(0, 0) | orderWordHasFieldOutsideSlot, slotKeys[0]) && object.orderWord() == (orderWordOf(0, 1) | orderWordHasFieldOutsideSlot));
    object.forgetTransitions();
    object.forgetRuns();
}

struct ExtensionGivenByOperation {
    uint32_t* extensionWord;
    uint32_t wordOfExtension;
};
static ExtensionGivenByOperation* extensionGivenByOperation;

static UGPRPair JIT_OPERATION_ATTRIBUTES giveExtensionByOperation(Instance* instance, uint64_t object, uint64_t granuleBits)
{
    StubEnvironment& environment = StubEnvironment::of(instance);
    ++environment.numberOfOperationCalls;
    ++environment.numberOfRecordedCalls;
    environment.recordedArguments = { object, granuleBits, 0, 0, 0 };
    *extensionGivenByOperation->extensionWord = extensionGivenByOperation->wordOfExtension;
    return makeUGPRPair(0, 0);
}

static void testFirstFieldInExtensionOfObjectWithoutOne()
{
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField;
    constexpr uint32_t offsetInExtension = offsetOfFirstExtensionSlot + 2 * extensionGranuleSize;
    constexpr uint32_t slotKey = slotKeyFor(offsetInExtension, true);
    constexpr uint64_t granuleBit = granuleBitOf(offsetInExtension);
    constexpr uint32_t orderBefore = orderWordOf(0, 0);
    constexpr uint32_t orderAfter = orderWordOf(0, 1);
    constexpr uint64_t untouched = 0xabababababababab;
    const StubKey store = putSlotOrAdd(PlaceOfCreatedSlot::OutOfLine, SlotRepresentation::JSValue);
    const uint64_t value = JSValue::encode(jsNumber(42));
    StubRunner runner;
    ObjectWithKeyOrder object(runner);
    Instance& instance = runner.instance();

    struct {
        ExtensionLayoutData extensionData { };
        LayoutData layout { };
    } withExtensionData;
    static_assert(offsetof(decltype(withExtensionData), layout) == sizeof(ExtensionLayoutData));
    withExtensionData.layout = object.layout;
    withExtensionData.layout.hasExtensionData = true;
    object.inStructure<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &withExtensionData.layout;
    object.inStructure<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfExtensionWord, SlotRepresentation::Cell32).encoded();
    object.publish(orderBefore, slotKey, orderAfter);

    struct {
        FakeObject<minExtensionSize> empty;
        FakeObject<minExtensionSize> given;
    } extensions;
    FakeObject<minExtensionSize>& emptyExtension = extensions.empty;
    FakeObject<2 * minExtensionSize> room;
    FakeAllocator allocator { minExtensionSize };
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + (extensionCellSizeFor(maxGranulesOfExtensionFromStub) / MarkedSpace::sizeStep + 1) * sizeof(void*)> space;
    void*& allocatorInSpace = space.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(minExtensionSize) * sizeof(void*));
    allocatorInSpace = &allocator;
    instance.allocation.cagedAuxiliarySpace = space.bytes.data();
    instance.cellCageBase = emptyExtension.address() - offsetOfEmptyExtensionInCage;
    instance.barrierThreshold = blackThreshold;
    const uint32_t wordOfRoom = static_cast<uint32_t>((room.address() - offsetOfEmptyExtensionInCage) >> cell32Shift);
    uint32_t& extensionWord = object.object.at<uint32_t>(offsetOfExtensionWord);
    ExtensionGivenByOperation given { &extensionWord, static_cast<uint32_t>((extensions.given.address() - emptyExtension.address()) >> cell32Shift) };
    extensionGivenByOperation = &given;

    auto begin = [&] {
        object.orderWord() = orderBefore;
        object.object.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
        extensionWord = 0;
        extensions.given.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBit;
        extensions.given.at<uint64_t>(offsetOfFirstExtensionSlot) = 0;
        for (size_t offset = 0; offset < 2 * minExtensionSize; offset += sizeof(uint64_t))
            room.at<uint64_t>(offset) = untouched;
        allocator.makeRoomAt(room.address(), minExtensionSize);
    };
    auto isStoredInLine = [&] {
        runner.install(Entry::operationAOTEnsureExtensionGranules, giveExtensionByOperation);
        StubRun run = runner.run(store, { object.object.address(), value }, slotKey);
        runner.countCallsOf(Entry::operationAOTEnsureExtensionGranules);
        if (run.numberOfRecordedCalls) {
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.object.address() && run.recordedArguments[1] == granuleBit);
            AOT_TEST_CHECK(extensionWord == given.wordOfExtension && extensions.given.at<uint64_t>(offsetOfFirstExtensionSlot) == value && object.orderWord() == orderAfter);
            AOT_TEST_CHECK(room.at<uint64_t>(offsetOfExtensionGranuleBits) == untouched && room.at<uint64_t>(offsetOfFirstExtensionSlot) == untouched);
            return false;
        }
        AOT_TEST_CHECK(extensionWord == wordOfRoom && !allocator.hasRoom() && object.orderWord() == orderAfter);
        AOT_TEST_CHECK(room.at<uint64_t>(offsetOfExtensionGranuleBits) == granuleBit && room.at<uint64_t>(offsetOfFirstExtensionSlot) == value);
        AOT_TEST_CHECK(room.at<uint64_t>(minExtensionSize) == untouched && !emptyExtension.at<uint64_t>(offsetOfExtensionGranuleBits));
        return true;
    };

    begin();
    AOT_TEST_CHECK(isStoredInLine());

    begin();
    allocator.makeRoomAt(0, 0);
    AOT_TEST_CHECK(!isStoredInLine());
    begin();
    allocatorInSpace = nullptr;
    AOT_TEST_CHECK(!isStoredInLine());
    allocatorInSpace = &allocator;
    begin();
    withExtensionData.extensionData.granulesOfFirstExtensionLow = static_cast<uint32_t>(granuleBit << 1);
    AOT_TEST_CHECK(!isStoredInLine());
    withExtensionData.extensionData.granulesOfFirstExtensionLow = static_cast<uint32_t>(granuleBit);
    begin();
    AOT_TEST_CHECK(isStoredInLine());
    withExtensionData.extensionData.granulesOfFirstExtensionLow = 0;

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    constexpr uint32_t budget = 81;
    begin();
    runner.setCountsInstructions(true);
    StubRun run = runner.run(store, { object.object.address(), value }, slotKey);
    runner.setCountsInstructions(false);
    AOT_TEST_CHECK(!run.calledOperation && run.instructionsInStubs && extensionWord == wordOfRoom);
    if (*run.instructionsInStubs != budget)
        dataLogLn("the first field in the extension of an object without one: ", *run.instructionsInStubs, " instructions, the budget is ", budget);
    AOT_TEST_CHECK(*run.instructionsInStubs <= budget);
#endif
}

static void testGrowthOfExtensions()
{
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField;
    constexpr unsigned mostGranules = maxGranulesOfExtensionFromStub;
    constexpr size_t sizeOfRoom = extensionCellSizeFor(mostGranules) + minExtensionSize;
    constexpr uint64_t untouched = 0xabababababababab;
    const StubKey grow(StubFamily::AllocateExtensionSlow);
    StubRunner runner;
    Instance& instance = runner.instance();

    struct {
        ExtensionLayoutData extensionData { };
        LayoutData layout { };
    } withExtensionData;
    withExtensionData.layout.hasExtensionData = true;
    FakeObject<sizeof(CompiledStructure)> structure;
    structure.at<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &withExtensionData.layout;
    structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfExtensionWord, SlotRepresentation::Cell32).encoded();

    FakeObject<offsetOfFirstField + 2 * sizeof(uint64_t)> owner;
    FakeObject<sizeOfRoom> extension;
    FakeObject<sizeOfRoom> room;
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + (sizeOfRoom / MarkedSpace::sizeStep + 1) * sizeof(void*)> space;
    auto allocatorFor = [&](size_t bytes) -> void*& {
        return space.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(bytes) * sizeof(void*));
    };
    instance.allocation.cagedAuxiliarySpace = space.bytes.data();
    instance.prefix().structureIDBase = structure.address() & ~static_cast<uint64_t>(UINT32_MAX);
    instance.cellCageBase = MarkedBlockCage::baseOf(std::bit_cast<const void*>(static_cast<uintptr_t>(extension.address() - offsetOfEmptyExtensionInCage)));
    owner.setHeader(static_cast<uint32_t>(structure.address()), CompiledObjectType);
    uint32_t& extensionWord = owner.at<uint32_t>(offsetOfExtensionWord);
    auto wordOf = [&](auto& cell) {
        return MarkedBlockCage::encode(instance.cellCageBase, std::bit_cast<JSCell*>(cell.address() - offsetOfEmptyExtensionInCage));
    };
    auto valueOf = [](unsigned granule) {
        return 0x0101010101010101ULL * (granule + 1);
    };

    auto growsTo = [&](uint64_t held, uint64_t added) {
        const uint64_t all = held | added;
        const unsigned numberHeld = std::popcount(held);
        const size_t bytesHeld = held ? extensionCellSizeFor(numberHeld) : 0;
        const size_t bytes = extensionCellSizeFor(std::popcount(all));
        for (size_t offset = 0; offset < sizeOfRoom; offset += sizeof(uint64_t)) {
            extension.at<uint64_t>(offset) = offset < bytesHeld ? 0 : untouched;
            room.at<uint64_t>(offset) = untouched;
        }
        extension.at<uint64_t>(offsetOfExtensionGranuleBits) = held ? held : untouched;
        unsigned place = 0;
        for (unsigned granule = 0; granule < maxExtensionGranules; ++granule) {
            if (held >> granule & 1)
                extension.at<uint64_t>(offsetOfFirstExtensionSlot + place++ * extensionGranuleSize) = valueOf(granule);
        }
        extensionWord = held ? wordOf(extension) : 0;
        FakeAllocator allocator { static_cast<uint32_t>(bytes) };
        allocator.makeRoomAt(room.address(), bytes);
        allocatorFor(bytes) = &allocator;
        runner.recordCallsOf(Entry::operationAOTEnsureExtensionGranules);
        StubRun run = runner.run(grow, { owner.address(), added });
        runner.countCallsOf(Entry::operationAOTEnsureExtensionGranules);
        allocatorFor(bytes) = nullptr;
        AOT_TEST_CHECK(!run.numberOfRecordedCalls);

        const bool staysInPlace = allocator.hasRoom();
        AOT_TEST_CHECK(!staysInPlace || bytes == bytesHeld);
        auto& cell = staysInPlace ? extension : room;
        AOT_TEST_CHECK(extensionWord == wordOf(cell) && cell.template at<uint64_t>(offsetOfExtensionGranuleBits) == all);
        place = 0;
        for (unsigned granule = 0; granule < maxExtensionGranules; ++granule) {
            if (all >> granule & 1)
                AOT_TEST_CHECK(cell.template at<uint64_t>(offsetOfFirstExtensionSlot + place++ * extensionGranuleSize) == (held >> granule & 1 ? valueOf(granule) : 0));
        }
        for (size_t offset = offsetOfFirstExtensionSlot + place * extensionGranuleSize; offset < sizeOfRoom; offset += sizeof(uint64_t))
            AOT_TEST_CHECK(cell.template at<uint64_t>(offset) == (offset < bytes ? 0 : untouched));
        if (!staysInPlace && held)
            AOT_TEST_CHECK(extension.at<uint64_t>(offsetOfExtensionGranuleBits) == held && extension.at<uint64_t>(offsetOfFirstExtensionSlot) == valueOf(std::countr_zero(held)));
        return !staysInPlace;
    };

    for (bool isFenced : { false, true }) {
        instance.mutatorShouldBeFenced = isFenced;
        for (unsigned numberHeld = 0; numberHeld < mostGranules; ++numberHeld) {
            uint64_t held = 0;
            for (unsigned index = 0; index < numberHeld; ++index)
                held |= 1ULL << (4 * index + 2);
            const unsigned above = 4 * numberHeld + 2;
            const bool hasRoom = numberHeld && extensionCellSizeFor(numberHeld + 1) == extensionCellSizeFor(numberHeld);
            AOT_TEST_CHECK(growsTo(held, 1ULL << above) == !hasRoom);
            AOT_TEST_CHECK(growsTo(held, 1ULL << (maxExtensionGranules - 1)) == !hasRoom);
            AOT_TEST_CHECK(growsTo(held, 1) == (!hasRoom || (isFenced && numberHeld)));
            if (numberHeld > 1)
                AOT_TEST_CHECK(growsTo(held, 1ULL << (4 * (numberHeld / 2) + 1)) == (!hasRoom || isFenced));
            if (numberHeld + 2 <= mostGranules) {
                growsTo(held, 3ULL << above);
                growsTo(held, 1ULL << above | 1);
                growsTo(held, 1ULL << (above + 3) | 1ULL << above);
            }
            if (numberHeld + 3 <= mostGranules && numberHeld)
                growsTo(held, 1ULL << above | 1ULL << 3 | 1);
        }
    }
    instance.mutatorShouldBeFenced = false;

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    for (auto [numberHeld, budget] : { std::pair { 3u, 106u }, std::pair { 5u, 126u } }) {
        uint64_t held = (1ULL << numberHeld) - 1;
        const size_t bytes = extensionCellSizeFor(numberHeld + 1);
        growsTo(held, 1ULL << numberHeld);
        extensionWord = wordOf(extension);
        FakeAllocator allocator { static_cast<uint32_t>(bytes) };
        allocator.makeRoomAt(room.address(), bytes);
        allocatorFor(bytes) = &allocator;
        runner.setCountsInstructions(true);
        StubRun run = runner.run(grow, { owner.address(), 1ULL << numberHeld });
        runner.setCountsInstructions(false);
        allocatorFor(bytes) = nullptr;
        AOT_TEST_CHECK(!run.calledOperation && run.instructionsInStubs && extensionWord == wordOf(room));
        if (*run.instructionsInStubs != budget)
            dataLogLn("an extension of ", numberHeld, " granules grows by one above them: ", *run.instructionsInStubs, " instructions, the budget is ", budget);
        AOT_TEST_CHECK(*run.instructionsInStubs <= budget);
    }
#endif
}

static void testCreatedFieldsOfObjectsWithExtraProperties()
{
    constexpr uint32_t slotKey = ObjectWithKeyOrder::slotKeys[0];
    constexpr uint32_t orderBefore = orderWordOf(3, 1);
    constexpr uint32_t orderAfter = orderWordOf(3, 3);
    constexpr uint32_t numberOfExtraKeys = 2;
    StubRunner runner;
    ObjectWithKeyOrder object(runner);
    uint16_t& numberInStructure = object.inStructure<uint16_t>(CompiledStructure::offsetOfNumberOfExtraKeys());
    object.publish(orderBefore, slotKey, orderWordOf(3, 2));
    object.publish(orderBefore, transitionKeyFor(slotKey, numberOfExtraKeys), orderAfter);

    AOT_TEST_CHECK(object.create(orderBefore, slotKey) && object.orderWord() == orderWordOf(3, 2));
    object.forgetRuns();

    AOT_TEST_CHECK(!object.create(orderBefore | orderWordHasExtraProperties, slotKey));
    numberInStructure = numberOfExtraKeys;
    for (uint32_t flags : { orderWordHasExtraProperties, orderWordHasExtraProperties | orderWordHasFieldOutsideSlot })
        AOT_TEST_CHECK(object.create(orderBefore | flags, slotKey) && object.orderWord() == (orderAfter | flags));
    numberInStructure = numberOfExtraKeys + 1;
    AOT_TEST_CHECK(!object.create(orderBefore | orderWordHasExtraProperties, slotKey));

    AOT_TEST_CHECK(!object.create(orderWordOf(0, 0) | orderWordHasExtraProperties, slotKey));

    numberInStructure = numberOfExtraKeys;
    object.forgetTransitions();
    AOT_TEST_CHECK(object.create(orderBefore | orderWordHasExtraProperties, slotKey) && object.orderWord() == (orderAfter | orderWordHasExtraProperties));
    numberInStructure = numberOfExtraKeys + 1;
    AOT_TEST_CHECK(!object.create(orderBefore | orderWordHasExtraProperties, slotKey));
    numberInStructure = numberOfExtraKeys;
    AOT_TEST_CHECK(!object.create(orderBefore | orderWordHasExtraProperties | orderWordHasFieldOutsideSlot, slotKey));
    object.forgetRuns();
    AOT_TEST_CHECK(!object.create(orderBefore | orderWordHasExtraProperties, slotKey));

    object.publish(orderBefore, transitionKeyFor(slotKey, numberOfExtraKeys), orderAfter);
    object.inStructure<uint32_t>(Structure::bitFieldOffset()) |= Structure::s_mayBePrototypeBits;
    AOT_TEST_CHECK(!object.create(orderBefore | orderWordHasExtraProperties, slotKey));
    object.inStructure<uint32_t>(Structure::bitFieldOffset()) &= ~Structure::s_mayBePrototypeBits;

}

static void testCreatedFieldsOfSlotsKnownToCaller()
{
    constexpr auto slotKeys = ObjectWithKeyOrder::slotKeys;
    constexpr uint32_t otherOrder = orderWordOf(1, 1);
    constexpr std::array stubsOfSlots {
        didCreateFieldOfSlot().withImmediate(slotKeys[0]),
        didCreateFieldOfSlot().withImmediate(slotKeys[1]),
    };
    StubRunner runner { std::span<const StubKey>(stubsOfSlots) };
    ObjectWithKeyOrder object(runner);
    const uint64_t cell = object.object.address();

    auto run = [&](uint32_t slotKey, bool slotKeyIsInKeyOfStub) {
        StubKey stub = didCreateFieldOfSlot();
        return slotKeyIsInKeyOfStub ? runner.run(stub.withImmediate(slotKey), { cell }) : runner.run(stub, { cell }, slotKey);
    };

    for (bool slotKeyIsInKeyOfStub : { true, false }) {
        auto create = [&](uint32_t orderWordBefore, uint32_t slotKey) {
            object.orderWord() = orderWordBefore;
            runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
            StubRun result = run(slotKey, slotKeyIsInKeyOfStub);
            runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
            AOT_TEST_CHECK(!result.calledOperation || (result.numberOfRecordedCalls == 1 && result.recordedArguments[0] == cell && result.recordedArguments[1] == slotKey && object.orderWord() == orderWordBefore));
            return !result.calledOperation;
        };
        object.forgetTransitions();
        object.forgetRuns();

        AOT_TEST_CHECK(create(orderWordOf(0, 0), slotKeys[0]) && object.orderWord() == orderWordOf(0, 1));
        AOT_TEST_CHECK(create(orderWordOf(0, 1), slotKeys[1]) && object.orderWord() == orderWordOf(0, 2));

        AOT_TEST_CHECK(!create(orderWordOf(0, 0), slotKeys[1]));
        object.publish(orderWordOf(0, 0), slotKeys[1], otherOrder);
        AOT_TEST_CHECK(create(orderWordOf(0, 0), slotKeys[1]) && object.orderWord() == otherOrder);

        object.forgetTransitions();
        AOT_TEST_CHECK(create(orderWordOf(0, 0), slotKeys[1]) && object.orderWord() == otherOrder);
        object.forgetRuns();
        AOT_TEST_CHECK(!create(orderWordOf(0, 0), slotKeys[1]));

        AOT_TEST_CHECK(!create(orderWordOf(0, 0) | orderWordHasFieldOutsideSlot, slotKeys[0]));
    }
}

namespace {

struct CopiedField {
    SlotRepresentation from;
    SlotRepresentation to;
    uint64_t bits;
    SlotFit fit { SlotFit::Always };
};

class CopiesByMoves {
public:
    static constexpr Entry operation = Entry::operationAOTCopyDataProperties;
    static constexpr Entry operationByFieldMap = Entry::operationAOTCopyDataPropertiesByFieldMap;
    static constexpr uint32_t excludedRun = 3;
    static constexpr uint32_t orderAfter = orderWordOf(2, 3);
    static constexpr uint64_t untouched = 0xababababababababULL;
    static constexpr unsigned maxNumberOfFields = 4;

    explicit CopiesByMoves(StubKey head)
        : m_head(head)
        , m_runner(StubPolicy { }, std::array { head })
    {
        Instance& instance = m_runner.instance();
        zeroSpan(instance.copiesWithoutFrame());
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

    static StubKey headThatPreservesNothing() { return callOperationFor(operation, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(operation)); }
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
        for (unsigned index = 0; index < fields.size(); ++index) {
            const CopiedField& field = fields[index];
            uint32_t from = offsetOfFirstField + index * sizeof(uint64_t);
            uint32_t to = offsetOfFirstField + (maxNumberOfFields - 1 - index) * sizeof(uint64_t);
            m_copy.moves.append(MappedCopy::Move { from, to, moveKindFor(field.from, field.to, field.fit), static_cast<uint8_t>(widthOf(field.to)), false, false, field.from, field.to, 0, static_cast<uint16_t>(index) });
            m_copy.moves.last().testsUndefinedAndNull = field.fit == SlotFit::UnlessUndefinedOrNull;
            m_source.at<uint64_t>(from) = field.bits;
        }
        describeMovesForStub(m_copy);
        place() = &m_copy;
    }

    const MappedCopy*& place() { return m_runner.instance().copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(m_copy.targetWords, m_copy.sourceWords, excludedRun)]; }

    bool run(uint32_t run = excludedRun, uint32_t count = 0, Entry operationWhereCopyIsKept = operationByFieldMap)
    {
        bool isKept = !count && m_runner.instance().copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(wordsOf(m_target), wordsOf(m_source), run)];
        m_runner.recordCallsOf(isKept ? operationWhereCopyIsKept : operation);
        m_lastRun = m_runner.runOperationCall(m_head, { m_target.address(), m_source.address(), run, count });
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

    StubKey m_head;
    StubRunner m_runner;
    std::array<FakeObject<sizeof(CompiledStructure)>, 2> m_structures;
    FakeObject<sizeOfCell> m_target;
    FakeObject<sizeOfCell> m_source;
    MappedCopy m_copy;
    StubRun m_lastRun { };
};

} // anonymous namespace

static void testCopiesOfPropertiesByMovesFrom(StubKey head, [[maybe_unused]] uint32_t budget)
{
    using Slot = SlotRepresentation;
    CopiesByMoves copies(head);
    const uint64_t cageBase = copies.cageBase();
    const uint64_t bitsOfAnyKind[] = {
        0, 1, 2, 6, 7, 10, 0x80000000, 0x80000001, 0x80000002, 0x80000004, 0x80000005, 0x7fffffff, 0xffffffff, 0x100000000, 0x180000001,
        static_cast<uint64_t>(absentInteger), static_cast<uint64_t>(undefinedInteger), static_cast<uint64_t>(absentInteger) + 1, absentDouble, nullDouble, absentDouble - 1, std::bit_cast<uint64_t>(1.5), std::bit_cast<uint64_t>(-0.0),
        encoded(jsNumber(7)), encoded(jsNumber(-1)), cageBase, cageBase + 8, cageBase + 0x12345678, cageBase + MarkedBlockCage::maxSize - 8, cageBase + MarkedBlockCage::maxSize, cageBase - 8, UINT64_MAX,
    };
    const std::pair<SlotRepresentation, SlotRepresentation> pairs[] = {
        { Slot::Int32, Slot::Int32 }, { Slot::Uint32, Slot::Uint32 }, { Slot::Float, Slot::Float }, { Slot::Int32, Slot::OptionalInt32 }, { Slot::OptionalInt32, Slot::OptionalInt32 }, { Slot::Cell32, Slot::Cell32 }, { Slot::Cell32, Slot::OptionalCell32 }, { Slot::OptionalCell32, Slot::OptionalCell32 },
        { Slot::Cell, Slot::Cell32 }, { Slot::Cell, Slot::OptionalCell32 }, { Slot::JSValue, Slot::OptionalCell32 },
        { Slot::Int64, Slot::Int64 }, { Slot::Double, Slot::Double }, { Slot::OptionalInt64, Slot::OptionalInt64 }, { Slot::OptionalDouble, Slot::OptionalDouble }, { Slot::JSValue, Slot::JSValue }, { Slot::Cell, Slot::Cell },
        { Slot::Boolean, Slot::Boolean }, { Slot::OptionalBoolean, Slot::OptionalBoolean }, { Slot::Uint8, Slot::Uint8 }, { Slot::Int8, Slot::Int8 },
        { Slot::Int32, Slot::Double }, { Slot::Int32, Slot::OptionalDouble }, { Slot::OptionalInt32, Slot::OptionalDouble },
        { Slot::Int32, Slot::JSValue }, { Slot::OptionalInt32, Slot::JSValue },
        { Slot::Cell32, Slot::JSValue }, { Slot::OptionalCell32, Slot::JSValue }, { Slot::Cell32, Slot::Cell }, { Slot::OptionalCell32, Slot::Cell },
    };
    const CopiedField alwaysMoved[] = { { Slot::Int32, Slot::Int32, 5 }, { Slot::Double, Slot::Double, std::bit_cast<uint64_t>(2.5) }, { Slot::Boolean, Slot::Boolean, 1 } };
    for (auto [from, to] : pairs) {
        for (uint64_t bits : bitsOfAnyKind) {
            copies.check(std::array { CopiedField { from, to, bits } });
            copies.check(std::array { CopiedField { from, to, bits }, alwaysMoved[0], alwaysMoved[1], alwaysMoved[2] });
            copies.check(std::array { alwaysMoved[2], alwaysMoved[1], CopiedField { from, to, bits }, alwaysMoved[0] });
            copies.check(std::array { alwaysMoved[1], alwaysMoved[0], alwaysMoved[2], CopiedField { from, to, bits } });
        }
    }
    copies.check({ });
    for (auto [from, to] : std::initializer_list<std::pair<SlotRepresentation, SlotRepresentation>> { { Slot::OptionalInt32, Slot::OptionalInt32 }, { Slot::OptionalInt64, Slot::OptionalInt64 }, { Slot::OptionalDouble, Slot::OptionalDouble }, { Slot::OptionalBoolean, Slot::OptionalBoolean }, { Slot::JSValue, Slot::JSValue }, { Slot::OptionalInt32, Slot::JSValue }, { Slot::OptionalInt32, Slot::OptionalDouble } }) {
        const SlotTraits traits = traitsOf(from);
        for (uint64_t marker : { traits.absentBits, traits.undefinedBits, traits.nullBits }) {
            copies.describe(std::array { alwaysMoved[0], CopiedField { from, to, marker, SlotFit::UnlessUndefinedOrNull } });
            copies.emptyTarget();
            AOT_TEST_CHECK(copies.copy().structureOfSourceForStub && !copies.run());
        }
        for (uint64_t bits : bitsOfAnyKind) {
            copies.check(std::array { CopiedField { from, to, bits, SlotFit::UnlessUndefinedOrNull } });
            copies.check(std::array { alwaysMoved[2], alwaysMoved[1], CopiedField { from, to, bits, SlotFit::UnlessUndefinedOrNull }, alwaysMoved[0] });
        }
    }

    for (auto [from, to] : std::initializer_list<std::pair<SlotRepresentation, SlotRepresentation>> { { Slot::Uint16, Slot::Uint16 }, { Slot::Double, Slot::OptionalDouble }, { Slot::Boolean, Slot::OptionalBoolean } }) {
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
    auto isCopiedByStub = [&](uint32_t run = CopiesByMoves::excludedRun, uint32_t count = 0, Entry operationWhereCopyIsKept = CopiesByMoves::operationByFieldMap) {
        uint32_t orderWordBefore = copies.target().at<uint32_t>(offsetOfOrderWord);
        bool isCopied = copies.run(run, count, operationWhereCopyIsKept);
        AOT_TEST_CHECK(copies.target().at<uint32_t>(offsetOfOrderWord) == (isCopied ? CopiesByMoves::orderAfter : orderWordBefore));
        copies.emptyTarget();
        return isCopied;
    };
    auto isCopiedOnlyWithout = [&](auto& word, auto otherValue, Entry operationCalled = CopiesByMoves::operation) {
        auto value = std::exchange(word, otherValue);
        bool isCopiedWith = isCopiedByStub(CopiesByMoves::excludedRun, 0, operationCalled);
        word = value;
        return !isCopiedWith && isCopiedByStub();
    };
    copies.describe(fields);
    copies.emptyTarget();
    AOT_TEST_CHECK(isCopiedByStub());
    AOT_TEST_CHECK(!isCopiedByStub(CopiesByMoves::excludedRun + 1) && !isCopiedByStub(0) && !isCopiedByStub(CopiesByMoves::excludedRun, 1));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.place(), static_cast<const MappedCopy*>(nullptr)));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().structureOfSourceForStub, StructureID(), CopiesByMoves::operationByFieldMap));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().structureOfSourceForStub, copies.copy().structureOfTarget, CopiesByMoves::operationByFieldMap));
    {
        StructureID structureOfSource = std::exchange(copies.copy().structureOfSource, copies.copy().structureOfTarget);
        copies.copy().structureOfSourceForStub = copies.copy().structureOfTarget;
        AOT_TEST_CHECK(!isCopiedByStub(CopiesByMoves::excludedRun, 0, CopiesByMoves::operation));
        copies.copy().structureOfSource = copies.copy().structureOfSourceForStub = structureOfSource;
        AOT_TEST_CHECK(isCopiedByStub());
    }
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().structureOfTarget, copies.copy().structureOfSource));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().sourceWords, copies.copy().sourceWords + 64));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().targetWords, copies.copy().targetWords + 64));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.copy().excludedRunNumberPlusOne, CopiesByMoves::excludedRun + 64));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.target().at<uint8_t>(JSCell::cellStateOffset()), static_cast<uint8_t>(CellState::PossiblyBlack)));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.structureOfTarget().at<uint32_t>(Structure::bitFieldOffset()), static_cast<uint32_t>(Structure::s_mayBePrototypeBits)));
    AOT_TEST_CHECK(isCopiedOnlyWithout(copies.runner().instance().barrierThreshold, static_cast<uint32_t>(tautologicalThreshold)));

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && !ASSERT_ENABLED
    copies.runner().setCountsInstructions(true);
    AOT_TEST_CHECK(copies.run() && copies.lastRun().instructionsInStubs);
    if (*copies.lastRun().instructionsInStubs != budget)
        dataLogLn("a copy of three fields by moves: ", *copies.lastRun().instructionsInStubs, " instructions in the stub, the budget is ", budget);
    AOT_TEST_CHECK(*copies.lastRun().instructionsInStubs <= budget);
#endif
}

static void testCopiesOfPropertiesByMoves()
{
    testCopiesOfPropertiesByMovesFrom(CopiesByMoves::headThatPreservesNothing(), 78);
    for (OperationCall::Saves saves : { OperationCall::Saves::CallerSavedRegisters, OperationCall::Saves::CallerSavedGPRs })
        testCopiesOfPropertiesByMovesFrom(callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = saves }).withImmediate(offsetInRuntimeTable(CopiesByMoves::operation)), 106);
}

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
    zeroSpan(instance.copiesWithoutFrame());
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

        MappedCopy kept;
        const MappedCopy*& place = instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(target.at<uint64_t>(offsetOfOrderWord), source.at<uint64_t>(offsetOfOrderWord), 0)];
        place = &kept;
        AOT_TEST_CHECK(!isCopiedByStub(head));
        kept.structureOfSource = std::bit_cast<StructureID>(source.at<uint32_t>(JSCell::structureIDOffset()));
        operationCalled = operationByFieldMap;
        AOT_TEST_CHECK(!isCopiedByStub(head));
        operationCalled = operation;
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(JSCell::structureIDOffset()), static_cast<uint32_t>(structures[1].address())));
        place = nullptr;

        using Flag = CompiledStructure::Flag;
        using HeldKeyOrder = CompiledStructure::HeldKeyOrder;
        constexpr uint8_t flagsOfOrdered = static_cast<uint8_t>(Flag::HasOnlyPlainSlots) | static_cast<uint8_t>(Flag::HasOutOfLineKeyOrder);
        auto& unordered = structures[0];
        auto& ordered = structures[1];
        uint8_t& flags = ordered.at<uint8_t>(CompiledStructure::offsetOfFlags());
        uint8_t& heldKeyOrder = ordered.at<uint8_t>(CompiledStructure::offsetOfHeldKeyOrder());
        uint32_t& orderWordOfSource = source.at<uint32_t>(offsetOfOrderWord);
        flags = flagsOfOrdered;
        source.at<uint32_t>(JSCell::structureIDOffset()) = static_cast<uint32_t>(ordered.address());
        orderWordOfSource = outOfLineOrderWord;
        emptyTarget();
        MappedCopy copyOfNoSource;
        const MappedCopy*& placeOfOrdered = instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(target.at<uint64_t>(offsetOfOrderWord), source.at<uint64_t>(offsetOfOrderWord), 0)];
        AOT_TEST_CHECK(!isCopiedByStub(head));
        placeOfOrdered = &copyOfNoSource;
        const MappedCopy*& placeOfCounted = instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(target.at<uint64_t>(offsetOfOrderWord), (source.at<uint64_t>(offsetOfOrderWord) & ~static_cast<uint64_t>(orderAndCountMask)) | outOfLineOrderWordFor(3), 0)];
        placeOfCounted = &copyOfNoSource;
        for (uint32_t orderWord : { outOfLineOrderWord, outOfLineOrderWord | orderWordIsFrozen, outOfLineOrderWord | orderWordIntegrityBits, outOfLineOrderWordFor(3), outOfLineOrderWordFor(3) | orderWordIsFrozen }) {
            orderWordOfSource = orderWord;
            for (unsigned numberOfFields = 0; numberOfFields <= maxNumberOfFields; ++numberOfFields) {
                layout.size = offsetOfFirstField + numberOfFields * sizeof(uint64_t);
                emptyTarget();
                AOT_TEST_CHECK(isCopiedByStub(head) && target.at<uint32_t>(offsetOfOrderWord) == (orderWord & orderAndCountMask) && target.at<uint32_t>(JSCell::structureIDOffset()) == static_cast<uint32_t>(ordered.address()));
                for (unsigned index = 0; index <= maxNumberOfFields; ++index)
                    AOT_TEST_CHECK(target.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) == (index < numberOfFields ? source.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) : untouched));
            }
        }
        orderWordOfSource = outOfLineOrderWord;
        AOT_TEST_CHECK(isCopiedOnlyWithout(heldKeyOrder, static_cast<uint8_t>(HeldKeyOrder::IsOfOneObject)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(heldKeyOrder, static_cast<uint8_t>(HeldKeyOrder::IsBesideExtraProperties)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(flags, static_cast<uint8_t>(Flag::HasOnlyPlainSlots)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(flags, static_cast<uint8_t>(flagsOfOrdered | static_cast<uint8_t>(Flag::HasExtraProperties))));
        AOT_TEST_CHECK(isCopiedOnlyWithout(flags, static_cast<uint8_t>(flagsOfOrdered | static_cast<uint8_t>(Flag::HasOtherPrototype))));
        AOT_TEST_CHECK(isCopiedOnlyWithout(ordered.at<uint32_t>(Structure::bitFieldOffset()), static_cast<uint32_t>(Structure::s_mayBePrototypeBits)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(ordered.at<uint64_t>(Structure::realmOffset()), static_cast<uint64_t>(16)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(ordered.at<uint64_t>(Structure::prototypeOffset()), static_cast<uint64_t>(16)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(unordered.at<uint8_t>(CompiledStructure::offsetOfFlags()), static_cast<uint8_t>(0)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(unordered.at<uint8_t>(CompiledStructure::offsetOfFlags()), static_cast<uint8_t>(static_cast<uint8_t>(Flag::HasOnlyPlainSlots) | static_cast<uint8_t>(Flag::HasOtherPrototype))));
        AOT_TEST_CHECK(isCopiedOnlyWithout(unordered.at<uint32_t>(Structure::bitFieldOffset()), static_cast<uint32_t>(Structure::s_mayBePrototypeBits)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfShapeWord), shapeWordOf(10)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint8_t>(JSCell::typeInfoTypeOffset()), static_cast<uint8_t>(FinalObjectType)));
        AOT_TEST_CHECK(isCopiedOnlyWithout(orderWordOfSource, outOfLineOrderWord | orderWordHasExtraProperties));
        AOT_TEST_CHECK(isCopiedOnlyWithout(orderWordOfSource, outOfLineOrderWord | orderWordHasFieldOutsideSlot));
        AOT_TEST_CHECK(isCopiedOnlyWithout(instance.barrierThreshold, static_cast<uint32_t>(tautologicalThreshold)));
        emptyTarget();
        AOT_TEST_CHECK(!isCopiedByStub(head, 1) && !isCopiedByStub(head, 0, 1));
        target.at<uint32_t>(offsetOfOrderWord) = orderOfSource;
        AOT_TEST_CHECK(!isCopiedByStub(head));
        placeOfOrdered = nullptr;
        placeOfCounted = nullptr;
        flags = static_cast<uint8_t>(Flag::HasOnlyPlainSlots);
        source.at<uint32_t>(JSCell::structureIDOffset()) = static_cast<uint32_t>(unordered.address());
        orderWordOfSource = orderOfSource;
    }
}

static void testCopiesOfPropertiesByChunksWithExtension()
{
    constexpr Entry operation = Entry::operationAOTCopyDataProperties;
    constexpr Entry operationByFieldMap = Entry::operationAOTCopyDataPropertiesByFieldMap;
    constexpr size_t offsetOfExtensionWord = offsetOfFirstField + 2 * sizeof(uint64_t);
    constexpr size_t sizeOfCell = offsetOfExtensionWord + 2 * sizeof(uint64_t);
    constexpr unsigned mostGranules = maxGranulesOfExtensionFromStub + 1;
    constexpr size_t sizeOfRoom = extensionCellSizeFor(mostGranules) + minExtensionSize;
    constexpr uint8_t untouchedByte = 0xab;
    WTF::Vector<StubKey> heads { callOperationFor(operation, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(operation)) };
    for (OperationCall::Saves saves : { OperationCall::Saves::CallerSavedRegisters, OperationCall::Saves::CallerSavedGPRs })
        heads.append(callOperationPreservingRegisters({ .returns = OperationCall::Returns::Void, .saves = saves }).withImmediate(offsetInRuntimeTable(operation)));
    StubRunner runner(StubPolicy { }, heads.span());
    Instance& instance = runner.instance();
    zeroSpan(instance.copiesWithoutFrame());

    using Flag = CompiledStructure::Flag;
    std::array<FakeObject<sizeof(CompiledStructure)>, 2> structures;
    LayoutData layout { };
    layout.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    layout.hasExtensionData = true;
    layout.size = offsetOfExtensionWord + 2 * sizeof(uint32_t);
    for (auto& structure : structures) {
        structure.at<uint8_t>(CompiledStructure::offsetOfFlags()) = static_cast<uint8_t>(Flag::HasOnlyPlainSlots);
        structure.at<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &layout;
        structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = offsetOfExtensionWord;
    }
    structures[1].at<uint8_t>(CompiledStructure::offsetOfFlags()) |= static_cast<uint8_t>(Flag::HasOutOfLineKeyOrder);

    FakeObject<sizeOfCell> target;
    FakeObject<sizeOfCell> source;
    FakeObject<sizeOfRoom> extension;
    FakeObject<sizeOfRoom> room;
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + (sizeOfRoom / MarkedSpace::sizeStep + 1) * sizeof(void*)> space;
    auto allocatorFor = [&](size_t bytes) -> void*& {
        return space.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(bytes) * sizeof(void*));
    };
    instance.allocation.cagedAuxiliarySpace = space.bytes.data();
    instance.prefix().structureIDBase = structures[0].address() & ~static_cast<uint64_t>(UINT32_MAX);
    instance.cellCageBase = MarkedBlockCage::baseOf(std::bit_cast<const void*>(static_cast<uintptr_t>(extension.address() - offsetOfEmptyExtensionInCage)));
    instance.barrierThreshold = blackThreshold;
    auto wordOf = [&](auto& cell) {
        return MarkedBlockCage::encode(instance.cellCageBase, std::bit_cast<JSCell*>(cell.address() - offsetOfEmptyExtensionInCage));
    };
    for (size_t offset = 0; offset < sizeOfRoom; offset += sizeof(uint64_t))
        extension.at<uint64_t>(offset) = 0x0101010101010101ULL * (offset / sizeof(uint64_t) + 1);

    MappedCopy copyOfNoSource;
    for (bool takesStructure : { false, true }) {
        const uint32_t orderWordOfSource = takesStructure ? outOfLineOrderWord | orderWordIsFrozen : orderWordOf(3, 2);
        source.setHeader(static_cast<uint32_t>(structures[takesStructure].address()), CompiledObjectType);
        source.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
        source.at<uint32_t>(offsetOfOrderWord) = orderWordOfSource;
        source.at<uint64_t>(offsetOfFirstField) = std::bit_cast<uint64_t>(-0.0);
        source.at<uint64_t>(offsetOfFirstField + sizeof(uint64_t)) = 0x7ff0000000000001;
        source.at<uint32_t>(offsetOfExtensionWord + sizeof(uint32_t)) = 0x80000000;
        auto begin = [&] {
            target.setHeader(static_cast<uint32_t>(structures[0].address()), CompiledObjectType);
            target.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
            target.at<uint32_t>(offsetOfShapeWord) = shapeWordOf(9);
            target.at<uint32_t>(offsetOfOrderWord) = unorderedOrderWord;
            for (size_t offset = offsetOfFirstField; offset < sizeOfCell; offset += sizeof(uint64_t))
                target.at<uint64_t>(offset) = 0;
            room.bytes.fill(untouchedByte);
        };
        begin();
        instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(target.at<uint64_t>(offsetOfOrderWord), source.at<uint64_t>(offsetOfOrderWord), 0)] = takesStructure ? &copyOfNoSource : nullptr;
        for (StubKey head : heads) {
            auto isCopiedByStub = [&](uint32_t excludedRun = 0, uint32_t count = 0) {
                FakeObject<sizeOfCell> before = target;
                runner.countCallsOf(operationByFieldMap);
                runner.recordCallsOf(operation);
                StubRun run = runner.runOperationCall(head, { target.address(), source.address(), excludedRun, count });
                if (!run.calledOperation)
                    return true;
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == target.address() && run.recordedArguments[1] == source.address());
                AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[2]) == excludedRun && static_cast<uint32_t>(run.recordedArguments[3]) == count && target.bytes == before.bytes);
                return false;
            };
            auto checkCell = [&](uint32_t wordOfExtension) {
                AOT_TEST_CHECK(target.at<uint32_t>(offsetOfOrderWord) == (orderWordOfSource & orderAndCountMask) && target.at<uint32_t>(JSCell::structureIDOffset()) == source.at<uint32_t>(JSCell::structureIDOffset()));
                for (size_t index = 0; index < 2; ++index)
                    AOT_TEST_CHECK(target.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)) == source.at<uint64_t>(offsetOfFirstField + index * sizeof(uint64_t)));
                AOT_TEST_CHECK(target.at<uint32_t>(offsetOfExtensionWord) == wordOfExtension && target.at<uint32_t>(offsetOfExtensionWord + sizeof(uint32_t)) == 0x80000000 && !target.at<uint64_t>(offsetOfExtensionWord + sizeof(uint64_t)));
            };

            source.at<uint32_t>(offsetOfExtensionWord) = 0;
            begin();
            AOT_TEST_CHECK(isCopiedByStub());
            checkCell(0);

            source.at<uint32_t>(offsetOfExtensionWord) = wordOf(extension);
            for (unsigned granules = 1; granules <= mostGranules; ++granules) {
                for (unsigned shift : { 0u, maxExtensionGranules - granules }) {
                    const size_t bytes = extensionCellSizeFor(granules);
                    FakeAllocator allocator { static_cast<uint32_t>(bytes) };
                    allocator.makeRoomAt(room.address(), bytes);
                    allocatorFor(bytes) = &allocator;
                    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = ((1ULL << granules) - 1) << shift;
                    begin();
                    const bool isCopied = isCopiedByStub();
                    AOT_TEST_CHECK(isCopied == (granules < mostGranules) && allocator.hasRoom() == !isCopied);
                    if (isCopied) {
                        checkCell(wordOf(room));
                        AOT_TEST_CHECK(equalSpans(std::span<const uint8_t>(room.bytes).first(bytes), std::span<const uint8_t>(extension.bytes).first(bytes)));
                    }
                    AOT_TEST_CHECK(std::ranges::all_of(std::span(room.bytes).subspan(isCopied ? bytes : 0), [](uint8_t byte) { return byte == untouchedByte; }));
                    begin();
                    AOT_TEST_CHECK(!isCopiedByStub());
                    allocatorFor(bytes) = nullptr;
                    AOT_TEST_CHECK(!isCopiedByStub());
                }
            }

            FakeAllocator allocator { minExtensionSize };
            extension.at<uint64_t>(offsetOfExtensionGranuleBits) = 1;
            allocatorFor(minExtensionSize) = &allocator;
            auto isCopiedOnlyWithout = [&](auto& word, auto otherValue) {
                begin();
                allocator.makeRoomAt(room.address(), minExtensionSize);
                auto value = std::exchange(word, otherValue);
                bool isCopiedWith = isCopiedByStub();
                word = value;
                return !isCopiedWith && isCopiedByStub();
            };
            AOT_TEST_CHECK(isCopiedOnlyWithout(instance.barrierThreshold, static_cast<uint32_t>(tautologicalThreshold)));
            AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfShapeWord), shapeWordOf(10)));
            AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfShapeWord), shapeWordOf(9) & ~shapeWordIsUnmarked));
            AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfOrderWord), orderWordOfSource | orderWordHasExtraProperties));
            AOT_TEST_CHECK(isCopiedOnlyWithout(source.at<uint32_t>(offsetOfOrderWord), orderWordOfSource | orderWordHasFieldOutsideSlot));
            AOT_TEST_CHECK(isCopiedOnlyWithout(structures[0].at<uint32_t>(Structure::bitFieldOffset()), static_cast<uint32_t>(Structure::s_mayBePrototypeBits)));
            begin();
            AOT_TEST_CHECK(!isCopiedByStub(1) && !isCopiedByStub(0, 1));
            target.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::PossiblyBlack);
            AOT_TEST_CHECK(!isCopiedByStub());
            allocatorFor(minExtensionSize) = nullptr;
        }
    }
}

static void testInstructionsOfOperationCall()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && !ASSERT_ENABLED
    constexpr uint32_t budget = 9;
    StubRunner runner;
    runner.setCountsInstructions(true);
    StubRun run = runner.run(callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance), { }, offsetInRuntimeTable(Entry::operationAOTValueAdd));
    AOT_TEST_CHECK(run.calledOperation && run.instructionsInStubs);
    if (*run.instructionsInStubs != budget)
        dataLogLn("a call of an operation: ", *run.instructionsInStubs, " instructions in the stub, the budget is ", budget);
    AOT_TEST_CHECK(*run.instructionsInStubs <= budget);
#endif
}

static void testFramesOfSlowPathsHaveNoWordThatNobodyWrote()
{
    constexpr Entry operation = Entry::operationAOTValueAdd;
    constexpr uint64_t wordsThatAlignFrame = 1;
    WTF::Vector<StubKey> heads;
    for (uint8_t form = 0; form < std::size(preservingCallShapes); ++form) {
        if (preservingCallShapes[form].returns != OperationCall::Returns::Double)
            heads.append(StubKey(StubFamily::CallOperationPreservingRegisters).withForm(form).withImmediate(offsetInRuntimeTable(operation)));
    }
    StubRunner runner(heads.span());
    runner.countWordsNobodyWroteAtCallsOf(operation);
    for (StubKey head : heads) {
        AOT_TEST_CHECK(runner.runOperationCall(head, { }).calledOperation);
        if (runner.wordsNobodyWrote() > wordsThatAlignFrame)
            dataLogLn("form ", head.form(), " of the call that preserves registers: nobody wrote ", runner.wordsNobodyWrote(), " words of its frame");
        AOT_TEST_CHECK(runner.wordsNobodyWrote() <= wordsThatAlignFrame);
    }

    runner.countWordsNobodyWroteAndLeaveStubAtCallsOf(Entry::operationAOTThrowAtSite);
    AOT_TEST_CHECK(runner.run(throwAtSite(ThrowSiteSource::WordInRegister, CallerFrame::HasFrame), { 0 }).calledOperation);
    if (runner.wordsNobodyWrote() > wordsThatAlignFrame)
        dataLogLn("a throw at a site: nobody wrote ", runner.wordsNobodyWrote(), " words of its frame");
    AOT_TEST_CHECK(runner.wordsNobodyWrote() <= wordsThatAlignFrame);
}

std::span<const NativeTest> stubRuntimeTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/allocation-at-sites"_s, ImageTests::whenExecutable<testAllocationAtSites> },
        { "stubs/allocation-from-templates"_s, ImageTests::whenExecutable<testAllocationFromTemplates> },
        { "stubs/slim-closures"_s, ImageTests::whenExecutable<testSlimClosures> },
        { "stubs/slot-of-the-frame-that-is-used-again-forgets-the-copy-in-the-heap"_s, ImageTests::whenExecutable<testSlotOfFrameThatIsUsedAgainForgetsCopyInHeap> },
        { "stubs/slim-closures-with-more-captures-than-registers"_s, ImageTests::whenExecutable<testSlimClosuresWithMoreCapturesThanRegisters> },
        { "stubs/typed-arrays-with-length"_s, ImageTests::whenExecutable<testTypedArraysWithLength> },
        { "stubs/created-fields-in-orders-of-the-image"_s, ImageTests::whenExecutable<testCreatedFieldsInOrdersOfImage> },
        { "stubs/growth-of-extensions"_s, ImageTests::whenExecutable<testGrowthOfExtensions> },
        { "stubs/first-field-in-the-extension-of-an-object-without-one"_s, ImageTests::whenExecutable<testFirstFieldInExtensionOfObjectWithoutOne> },
        { "stubs/created-fields-of-objects-with-extra-properties"_s, ImageTests::whenExecutable<testCreatedFieldsOfObjectsWithExtraProperties> },
        { "stubs/created-fields-of-slots-known-to-the-caller"_s, ImageTests::whenExecutable<testCreatedFieldsOfSlotsKnownToCaller> },
        { "stubs/binary-operation-string-order"_s, ImageTests::whenExecutable<testBinaryOperationStringOrder> },
        { "stubs/binary-operations-on-numbers"_s, ImageTests::whenExecutable<testBinaryOperationsOnNumbers> },
        { "stubs/binary-operations-with-double"_s, ImageTests::whenExecutable<testBinaryOperationsWithDouble> },
        { "stubs/unary-operations"_s, ImageTests::whenExecutable<testUnaryOperations> },
        { "stubs/copies-of-properties-by-moves"_s, ImageTests::whenExecutable<testCopiesOfPropertiesByMoves> },
        { "stubs/copies-of-properties-by-chunks"_s, ImageTests::whenExecutable<testCopiesOfPropertiesByChunks> },
        { "stubs/copies-of-properties-by-chunks-with-extension"_s, ImageTests::whenExecutable<testCopiesOfPropertiesByChunksWithExtension> },
        { "stubs/instructions-of-an-operation-call"_s, ImageTests::whenExecutable<testInstructionsOfOperationCall> },
        { "stubs/frames-of-slow-paths-have-no-word-that-nobody-wrote"_s, ImageTests::whenExecutable<testFramesOfSlowPathsHaveNoWordThatNobodyWrote> },
        { "stubs/boxed-call-handlers"_s, testBoxedCallHandlers },
        { "stubs/boxed-call-table"_s, runNativeTestWithLoadedProgram<stubRuntimeTests, emptyProgramText, testBoxedCallTable> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
