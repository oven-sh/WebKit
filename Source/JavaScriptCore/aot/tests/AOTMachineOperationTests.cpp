/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTestSupport.h"
#include "ExecutableAllocator.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/Int128.h>
#include <wtf/RawHex.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> machineOperationTests();

namespace Machine {

namespace {

// Operands and results travel as 64 bits. One of 32 bits lies in the low half, and in a table or an expectation the high half is zero.
template<typename T>
constexpr T operationTestValueOf(uint64_t bits)
{
    if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<float>(static_cast<uint32_t>(bits));
    else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<double>(bits);
    else
        return static_cast<T>(bits);
}

template<typename T>
constexpr uint64_t operationTestBitsOf(T value)
{
    if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<uint32_t>(value);
    else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<uint64_t>(value);
    else
        return static_cast<std::make_unsigned_t<T>>(value);
}

template<typename T>
constexpr Type operationTestTypeOf()
{
    if constexpr (std::is_same_v<T, float>)
        return Float;
    else if constexpr (std::is_same_v<T, double>)
        return Double;
    else if constexpr (sizeof(T) == 4)
        return Int32;
    else
        return Int64;
}

// Where a list is cut short its head is taken, so the values that tell most come first: the first six, then the next six.
constexpr size_t operationTestFewValues = 6;
constexpr size_t operationTestSomeValues = 12;

constexpr uint64_t operationTestInt32s[] = {
    0, 1, 0xffffffff, 4096, 0x12345678, 0x80000000,
    4095, 0xfffff000, 0x7fffffff, 0x00ff00ff, 255, 2,
    3, 10, 31, 32, 65535, 0x00fff000, 0x80000001, 0xfffffff5, 0xfffffff9, 0xfffffffe,
};

constexpr uint64_t operationTestInt64s[] = {
    0, 1, 0xffffffffffffffff, 4096, 0x123456789abcdef0, 0x8000000000000000,
    4095, 0xfffffffffffff000, 0x7fffffff, 0x80000000, 0xff00ff00ff00ff00, 0xffffffff80000000,
    2, 3, 10, 63, 64, 255, 65535, 0x00fff000, 0xffffffff, 0x100000000, 0x100000001, 0x123456789, 0xffffffff7fffffff,
    0x7fffffffffffffff, 0x8000000000000001, 0xfffffffffffffff5, 0xfffffffffffffff9, 0xfffe000000000000, 0xfffe000000000002, 0x0002000000000000,
};

constexpr uint64_t operationTestShiftAmounts[] = { 0, 1, 31, 32, 63, 64, 7, 33, 65, 0x80000000, 0xffffffff };

constexpr double operationTestDoubles[] = {
    0.0, -0.0, 1.0, -1.5, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
    -std::numeric_limits<double>::infinity(), 0.5, 4503599627370496.0, std::numeric_limits<double>::denorm_min(), 3.0e38, -1.0,
    -std::numeric_limits<double>::quiet_NaN(), -0.5, 0.49999999999999994, -0.49999999999999994, 1.5, 2.5, -2.5, 8388607.5, 2147483647.5, -2147483648.5,
    4503599627370495.5, -4503599627370495.5, 4503599627370497.0, 9007199254740992.0, 9223372036854775808.0, -9223372036854775808.0, 18446744073709551616.0,
    -std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(), std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(),
};

constexpr float operationTestFloats[] = {
    0.0f, -0.0f, 1.0f, -1.5f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
    -std::numeric_limits<float>::infinity(), 0.5f, 8388608.0f, std::numeric_limits<float>::denorm_min(), 3.0e38f, -1.0f,
    -std::numeric_limits<float>::quiet_NaN(), -0.5f, 0.49999997f, -0.49999997f, 1.5f, 2.5f, -2.5f, 8388607.5f, -8388607.5f, 8388609.0f, 16777216.0f,
    2147483648.0f, -2147483648.0f, 9223372036854775808.0f, -9223372036854775808.0f,
    -std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
};

template<typename T, size_t size>
constexpr std::array<uint64_t, size> operationTestBitsOfAll(const T (&values)[size])
{
    std::array<uint64_t, size> result { };
    for (size_t index = 0; index < size; ++index)
        result[index] = operationTestBitsOf(values[index]);
    return result;
}

constexpr auto operationTestBitsOfDoubles = operationTestBitsOfAll(operationTestDoubles);
constexpr auto operationTestBitsOfFloats = operationTestBitsOfAll(operationTestFloats);

template<typename T>
constexpr std::span<const uint64_t> operationTestValuesOf()
{
    if constexpr (std::is_same_v<T, float>)
        return operationTestBitsOfFloats;
    else if constexpr (std::is_same_v<T, double>)
        return operationTestBitsOfDoubles;
    else if constexpr (sizeof(T) == 4)
        return operationTestInt32s;
    else
        return operationTestInt64s;
}

uint64_t operationTestWithDirtAbove(uint64_t bits, unsigned bytes)
{
    constexpr uint64_t dirt = 0xbad0beefbad0beefull;
    if (bytes == sizeof(uint64_t))
        return bits;
    return (bits & ((uint64_t { 1 } << (8 * bytes)) - 1)) | dirt << (8 * bytes);
}

class OperationTestProcedure final : public TestProcedure {
public:
    explicit OperationTestProcedure(CPUFeatureSet features = { })
        : TestProcedure(traitsFor(features))
    {
    }

    const AbstractHeap& memory() const { return heaps().root; }

private:
    static ProcedureTraits traitsFor(CPUFeatureSet features)
    {
        ProcedureTraits traits;
        traits.features = features;
        return traits;
    }
};

template<typename... Details>
void checkOperationTest(bool condition, const TestProcedure& test, const Details&... details)
{
    if (condition) [[likely]]
        return;
    dataLogLn("Machine operation test: ", details...);
    dataLogLn(test.printedProcedure());
    dataLogLn(test.printedAllocation());
    AOT_TEST_CHECK(condition);
}

Vector<CPUFeatureSet, 2> operationTestFeatureSets()
{
    Vector<CPUFeatureSet, 2> result { CPUFeatureSet() };
    if (isX86_64() && CPUFeatureSet::host() != CPUFeatureSet())
        result.append(CPUFeatureSet::host());
    return result;
}

LValue operationTestValueOfBits(Output& out, LValue bits, Type type)
{
    switch (type.kind()) {
    case Int32:
        return out.castToInt32(bits);
    case Int64:
        return bits;
    case Float:
        return out.bitCast(out.castToInt32(bits), Float);
    case Double:
        return out.bitCast(bits, Double);
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LValue operationTestConstant(Output& out, uint64_t bits, Type type)
{
    switch (type.kind()) {
    case Int32:
        return out.constInt32(static_cast<int32_t>(bits));
    case Int64:
        return out.constInt64(static_cast<int64_t>(bits));
    case Float:
        return out.constFloat(operationTestValueOf<float>(bits));
    case Double:
        return out.constDouble(operationTestValueOf<double>(bits));
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LValue operationTestBitsOfValue(Output& out, LValue value)
{
    switch (value->type().kind()) {
    case Int32:
        return out.zeroExtPtr(value);
    case Int64:
        return value;
    case Float:
        return out.zeroExtPtr(out.bitCast(value, Int32));
    case Double:
        return out.bitCast(value, Int64);
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- One operation, with its operands in every kind of place.

enum class OperationTestPlace : uint8_t {
    Register,
    Constant,
    // Between its definition and the operation more values are live than its bank has registers, and none is read later than it.
    Spilled,
    // A load right in front of the operation.
    Loaded,
};

struct OperationTestPlaces {
    bool areUnderPressure() const { return left == OperationTestPlace::Spilled || right == OperationTestPlace::Spilled; }

    OperationTestPlace left { OperationTestPlace::Register };
    OperationTestPlace right { OperationTestPlace::Register };
    bool leftIsReadAgain { false };
    bool rightIsReadAgain { false };
};

struct OperationTestPlaceLists {
    using Place = OperationTestPlace;

    // The last numberWithLoadedOperand read an operand from memory.
    static constexpr size_t numberWithLoadedOperand = 3;
    static constexpr OperationTestPlaces withoutPressure[] = {
        { Place::Register, Place::Register },
        { Place::Register, Place::Register, true, false },
        { Place::Register, Place::Register, false, true },
        { Place::Register, Place::Register, true, true },
        { Place::Loaded, Place::Register },
        { Place::Register, Place::Loaded },
        { Place::Loaded, Place::Loaded },
    };
    static constexpr OperationTestPlaces underPressure[] = {
        { Place::Spilled, Place::Spilled },
        { Place::Spilled, Place::Register },
        { Place::Register, Place::Spilled },
        { Place::Spilled, Place::Spilled, true, true },
    };
    // In both: the first reads an operand from memory, the last is under pressure.
    static constexpr OperationTestPlaces withConstantLeft[] = {
        { Place::Constant, Place::Loaded },
        { Place::Constant, Place::Register },
        { Place::Constant, Place::Register, false, true },
        { Place::Constant, Place::Spilled },
    };
    static constexpr OperationTestPlaces withConstantRight[] = {
        { Place::Loaded, Place::Constant },
        { Place::Register, Place::Constant },
        { Place::Register, Place::Constant, true, false },
        { Place::Spilled, Place::Constant },
    };
    static constexpr OperationTestPlaces ofOneOperand[] = {
        { Place::Register },
        { Place::Register, Place::Register, true },
        { Place::Loaded },
        { Place::Loaded, Place::Register, true },
    };
    static constexpr OperationTestPlaces ofOneSpilledOperand[] = { { Place::Spilled } };
    static constexpr OperationTestPlaces ofOneConstant[] = { { Place::Constant } };
};

struct OperationTestSubject {
    ASCIILiteral name;
    Type left;
    // Void where the operation has one operand.
    Type right;
    Type result;
    ASCIILiteral use { ""_s };
    CPUFeatureSet features { };
    // A NaN comes out with the bits that went in.
    bool keepsBitsOfNaN { false };
    // What reads an operand in OperationTestPlace::Loaded.
    Opcode load { Load };
    // An operand in OperationTestPlace::Register or Spilled is the result of an Opaque. Otherwise a number there is the result of the BitwiseCast that makes it of the bits of an argument.
    bool hidesOriginOfOperands { false };
};

struct OperationTestOperands {
    OperationTestOperands withConstants(size_t count) const
    {
        OperationTestOperands result = *this;
        result.constantLefts = constantLefts.first(std::min(count, constantLefts.size()));
        result.constantRights = constantRights.first(std::min(count, constantRights.size()));
        return result;
    }

    OperationTestOperands cutShort() const
    {
        OperationTestOperands result = withConstants(operationTestFewValues);
        result.triesEveryPlaceUnderPressure = false;
        return result;
    }

    std::span<const uint64_t> lefts;
    std::span<const uint64_t> rights;
    std::span<const uint64_t> constantLefts;
    std::span<const uint64_t> constantRights;
    bool triesEveryPlaceUnderPressure { true };
    Opcode load { Load };
};

template<typename Left, typename Right = Left>
constexpr OperationTestOperands operationTestOperandsOf()
{
    return { operationTestValuesOf<Left>(), operationTestValuesOf<Right>(), operationTestValuesOf<Left>(), operationTestValuesOf<Right>() };
}

struct OperationTestRecord {
    uint64_t result;
    uint64_t leftReadAgain;
    uint64_t rightReadAgain;
    uint64_t wordsAhead;
    uint64_t numbersAhead;
    uint64_t wordsBehind;
    uint64_t numbersBehind;
};
static_assert(OBJECT_OFFSETOF(OperationTestRecord, numbersAhead) == OBJECT_OFFSETOF(OperationTestRecord, wordsAhead) + sizeof(uint64_t));
static_assert(OBJECT_OFFSETOF(OperationTestRecord, numbersBehind) == OBJECT_OFFSETOF(OperationTestRecord, wordsBehind) + sizeof(uint64_t));

constexpr size_t maxEvaluationsInOperationTest = 8;

struct OperationTestMemory {
    uint64_t left;
    uint64_t right;
    uint64_t seedOfWords;
    double seedOfNumbers;
    std::array<OperationTestRecord, maxEvaluationsInOperationTest> records;
};

constexpr uint64_t operationTestSeedOfWords = 0x1000;
constexpr double operationTestSeedOfNumbers = 4096;

unsigned numberOfValuesUnderPressure(TestProcedure& test, Bank bank)
{
    return static_cast<unsigned>(std::popcount(registerFileFor(test.procedure().traits()).allocatableIn(bank))) + 2;
}

uint64_t mixOfWordsUnderPressure(unsigned count)
{
    uint64_t mix = 0;
    for (unsigned index = 1; index <= count; ++index)
        mix ^= operationTestSeedOfWords + index;
    return mix;
}

uint64_t sumOfNumbersUnderPressure(unsigned count)
{
    double sum = 0;
    for (unsigned index = 1; index <= count; ++index)
        sum += operationTestSeedOfNumbers + index;
    return std::bit_cast<uint64_t>(sum);
}

// In both banks: defines more values than there are registers, then reads them all. The outcome goes to the word at the offset and to the one behind it.
void appendOperationTestPressure(OperationTestProcedure& test, LValue memory, ptrdiff_t offset)
{
    Output& out = test.out();
    LValue seedOfWords = out.load64(out.address(test.memory(), memory, OBJECT_OFFSETOF(OperationTestMemory, seedOfWords)));
    LValue seedOfNumbers = out.loadDouble(out.address(test.memory(), memory, OBJECT_OFFSETOF(OperationTestMemory, seedOfNumbers)));
    Vector<LValue> words;
    Vector<LValue> numbers;
    for (unsigned index = 1; index <= numberOfValuesUnderPressure(test, Bank::GP); ++index)
        words.append(out.add(seedOfWords, index));
    for (unsigned index = 1; index <= numberOfValuesUnderPressure(test, Bank::FP); ++index)
        numbers.append(out.doubleAdd(seedOfNumbers, out.constDouble(index)));
    LValue mix = words[0];
    for (size_t index = 1; index < words.size(); ++index)
        mix = out.bitXor(mix, words[index]);
    LValue sum = numbers[0];
    for (size_t index = 1; index < numbers.size(); ++index)
        sum = out.doubleAdd(sum, numbers[index]);
    out.store64(mix, out.address(test.memory(), memory, offset));
    out.storeDouble(sum, out.address(test.memory(), memory, offset + sizeof(uint64_t)));
}

// No operation purifies its result, so a NaN that one makes must still be a number once it is boxed.
bool operationTestIsPureNaN(double value)
{
    return std::isnan(value) && !isImpureNaN(value);
}

// A Float is widened ahead of boxing, which keeps its sign and the high bits of its payload.
bool operationTestResultsAgree(const OperationTestSubject& subject, uint64_t actual, uint64_t expected)
{
    if (actual == expected || subject.keepsBitsOfNaN)
        return actual == expected;
    if (subject.result == Double)
        return operationTestIsPureNaN(operationTestValueOf<double>(actual)) && std::isnan(operationTestValueOf<double>(expected));
    if (subject.result == Float)
        return !(actual >> 32) && operationTestIsPureNaN(static_cast<double>(operationTestValueOf<float>(actual))) && std::isnan(operationTestValueOf<float>(expected));
    return false;
}

// build(Output&, LValue left, LValue right) appends the operation and answers its result. reference(left, right) answers the bits of the result, or nothing where the operation promises none.
// One procedure evaluates the operation once for each entry of `places`. An operand in OperationTestPlace::Constant has one value in its list.
template<typename Build, typename Reference>
void checkOperationInPlaces(const OperationTestSubject& subject, std::span<const OperationTestPlaces> places, std::span<const uint64_t> lefts, std::span<const uint64_t> rights, const Build& build, const Reference& reference)
{
    AOT_TEST_CHECK(!places.empty() && places.size() <= maxEvaluationsInOperationTest);
    bool isEverDefined = false;
    for (uint64_t left : lefts) {
        for (uint64_t right : rights)
            isEverDefined |= reference(left, right).has_value();
    }
    if (!isEverDefined)
        return;

    OperationTestProcedure test(subject.features);
    Output& out = test.out();
    Builder& builder = out;
    bool hasRight = subject.right != Void;
    LValue leftBits = test.argumentGPR(0);
    LValue rightBits = test.argumentGPR(1);
    LValue memory = test.argumentGPR(2);

    auto operandIn = [&](OperationTestPlace place, Type type, LValue bits, std::span<const uint64_t> values, ptrdiff_t offset) -> LValue {
        switch (place) {
        case OperationTestPlace::Register:
        case OperationTestPlace::Spilled: {
            LValue value = operationTestValueOfBits(out, out.opaque(bits), type);
            return subject.hidesOriginOfOperands ? out.opaque(value) : value;
        }
        case OperationTestPlace::Constant:
            AOT_TEST_CHECK(values.size() == 1);
            return operationTestConstant(out, values[0], type);
        case OperationTestPlace::Loaded:
            return builder.load(subject.load, type, memory, offset, test.memory());
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto leftIn = [&](OperationTestPlace place) { return operandIn(place, subject.left, leftBits, lefts, OBJECT_OFFSETOF(OperationTestMemory, left)); };
    auto rightIn = [&](OperationTestPlace place) { return operandIn(place, subject.right, rightBits, rights, OBJECT_OFFSETOF(OperationTestMemory, right)); };

    LValue resultBits = nullptr;
    for (size_t index = 0; index < places.size(); ++index) {
        const OperationTestPlaces& place = places[index];
        ptrdiff_t record = OBJECT_OFFSETOF(OperationTestMemory, records) + index * sizeof(OperationTestRecord);
        LValue left = place.left == OperationTestPlace::Spilled ? leftIn(place.left) : nullptr;
        LValue right = hasRight && place.right == OperationTestPlace::Spilled ? rightIn(place.right) : nullptr;
        if (place.areUnderPressure())
            appendOperationTestPressure(test, memory, record + OBJECT_OFFSETOF(OperationTestRecord, wordsAhead));
        if (!left)
            left = leftIn(place.left);
        if (hasRight && !right)
            right = rightIn(place.right);
        LValue result = build(out, left, right);
        AOT_TEST_CHECK(result->type() == subject.result);
        if (place.areUnderPressure())
            appendOperationTestPressure(test, memory, record + OBJECT_OFFSETOF(OperationTestRecord, wordsBehind));
        if (place.leftIsReadAgain)
            out.store(left, out.address(test.memory(), memory, record + OBJECT_OFFSETOF(OperationTestRecord, leftReadAgain)));
        if (hasRight && place.rightIsReadAgain)
            out.store(right, out.address(test.memory(), memory, record + OBJECT_OFFSETOF(OperationTestRecord, rightReadAgain)));
        resultBits = operationTestBitsOfValue(out, result);
        out.store64(resultBits, out.address(test.memory(), memory, record + OBJECT_OFFSETOF(OperationTestRecord, result)));
    }
    out.ret(resultBits);
    compileOrFail(test);

    uint64_t mixOfWords = mixOfWordsUnderPressure(numberOfValuesUnderPressure(test, Bank::GP));
    uint64_t sumOfNumbers = sumOfNumbersUnderPressure(numberOfValuesUnderPressure(test, Bank::FP));
    for (uint64_t left : lefts) {
        for (uint64_t right : rights) {
            std::optional<uint64_t> expected = reference(left, right);
            if (!expected)
                continue;
            OperationTestMemory words { };
            words.left = operationTestWithDirtAbove(left, bytesForWidth(accessWidthOf(subject.load, subject.left)));
            words.right = hasRight ? operationTestWithDirtAbove(right, bytesForWidth(accessWidthOf(subject.load, subject.right))) : 0;
            words.seedOfWords = operationTestSeedOfWords;
            words.seedOfNumbers = operationTestSeedOfNumbers;
            uint64_t leftArgument = operationTestWithDirtAbove(left, sizeOf(subject.left));
            uint64_t rightArgument = hasRight ? operationTestWithDirtAbove(right, sizeOf(subject.right)) : 0;
            uint64_t returned = test.call<uint64_t>(leftArgument, rightArgument, &words);
            for (size_t index = 0; index < places.size(); ++index) {
                const OperationTestPlaces& place = places[index];
                const OperationTestRecord& record = words.records[index];
                checkOperationTest(operationTestResultsAgree(subject, record.result, *expected), test, subject.name, " ", subject.use, " of ", subject.left, ", evaluation ", index,
                    ": (", RawHex(left), ", ", RawHex(right), ") gave ", RawHex(record.result), " and not ", RawHex(*expected));
                checkOperationTest((!place.leftIsReadAgain || record.leftReadAgain == left) && (!hasRight || !place.rightIsReadAgain || record.rightReadAgain == right), test,
                    subject.name, " ", subject.use, " of ", subject.left, ", evaluation ", index, ": an operand of (", RawHex(left), ", ", RawHex(right), ") was lost");
                bool keptValuesUnderPressure = record.wordsAhead == mixOfWords && record.numbersAhead == sumOfNumbers && record.wordsBehind == mixOfWords && record.numbersBehind == sumOfNumbers;
                checkOperationTest(!place.areUnderPressure() || keptValuesUnderPressure, test, subject.name, " ", subject.use, " of ", subject.left, ", evaluation ", index, ": a value under pressure was lost");
            }
            checkOperationTest(returned == words.records[places.size() - 1].result, test, subject.name, " ", subject.use, " of ", subject.left, ": returned ", RawHex(returned));
        }
    }
}

template<typename Build, typename Reference>
void checkOperationEverywhere(OperationTestSubject subject, const OperationTestOperands& operands, const Build& build, const Reference& reference)
{
    using Lists = OperationTestPlaceLists;
    subject.load = operands.load;
    auto check = [&](std::span<const OperationTestPlaces> places, std::span<const uint64_t> lefts, std::span<const uint64_t> rights) {
        checkOperationInPlaces(subject, places, lefts, rights, build, reference);
    };

    if (subject.right == Void) {
        static constexpr uint64_t noOperand[] = { 0 };
        check(Lists::ofOneOperand, operands.lefts, noOperand);
        check(Lists::ofOneSpilledOperand, operands.lefts, noOperand);
        for (const uint64_t& constant : operands.constantLefts)
            check(Lists::ofOneConstant, singleElementSpan(constant), noOperand);
        return;
    }

    std::span<const OperationTestPlaces> withoutPressure(Lists::withoutPressure);
    std::span<const OperationTestPlaces> underPressure(Lists::underPressure);
    std::span<const OperationTestPlaces> withConstantLeft(Lists::withConstantLeft);
    std::span<const OperationTestPlaces> withConstantRight(Lists::withConstantRight);
    if (operands.load != Load) {
        withoutPressure = withoutPressure.last(Lists::numberWithLoadedOperand);
        underPressure = { };
        withConstantLeft = withConstantLeft.first(1);
        withConstantRight = withConstantRight.first(1);
    } else if (!operands.triesEveryPlaceUnderPressure) {
        underPressure = underPressure.first(1);
        withConstantLeft = withConstantLeft.first(withConstantLeft.size() - 1);
        withConstantRight = withConstantRight.first(withConstantRight.size() - 1);
    }

    check(withoutPressure, operands.lefts, operands.rights);
    for (const OperationTestPlaces& places : underPressure)
        check(singleElementSpan(places), operands.lefts, operands.rights);
    for (const uint64_t& constant : operands.constantLefts)
        check(withConstantLeft, singleElementSpan(constant), operands.rights);
    for (const uint64_t& constant : operands.constantRights)
        check(withConstantRight, operands.lefts, singleElementSpan(constant));
}

template<typename Result, typename Left, typename Right>
struct OperationTestBinaryRow {
    Opcode opcode;
    Result (*reference)(Left, Right);
    bool isChill { false };
    // Null: every pair of operands has an answer.
    bool (*isDefined)(Left, Right) { nullptr };
};

template<typename Result, typename Left, typename Right>
void checkBinaryOperation(const OperationTestBinaryRow<Result, Left, Right>& row, const OperationTestOperands& operands, CPUFeatureSet features = { })
{
    OperationTestSubject subject { descriptionOf(row.opcode).name, operationTestTypeOf<Left>(), operationTestTypeOf<Right>(), operationTestTypeOf<Result>() };
    if (row.isChill)
        subject.use = "(chill)"_s;
    subject.features = features;
    checkOperationEverywhere(subject, operands, [&](Output& out, LValue left, LValue right) {
        return row.isChill ? out.chill(row.opcode, left, right) : out.binary(row.opcode, left, right);
    }, [&](uint64_t left, uint64_t right) -> std::optional<uint64_t> {
        Left a = operationTestValueOf<Left>(left);
        Right b = operationTestValueOf<Right>(right);
        if (row.isDefined && !row.isDefined(a, b))
            return std::nullopt;
        return operationTestBitsOf(row.reference(a, b));
    });
}

template<typename Result, typename Operand>
struct OperationTestUnaryRow {
    Opcode opcode;
    Result (*reference)(Operand);
};

template<typename Result, typename Operand>
void checkUnaryOperation(const OperationTestUnaryRow<Result, Operand>& row, CPUFeatureSet features = { }, bool keepsBitsOfNaN = false)
{
    OperationTestSubject subject { descriptionOf(row.opcode).name, operationTestTypeOf<Operand>(), Void, operationTestTypeOf<Result>() };
    subject.features = features;
    subject.keepsBitsOfNaN = keepsBitsOfNaN;
    // A cast of a cast is folded to what lies below both. A number comes here through one cast, and its bits leave through another.
    subject.hidesOriginOfOperands = row.opcode == BitwiseCast;
    OperationTestOperands operands { operationTestValuesOf<Operand>(), { }, operationTestValuesOf<Operand>(), { } };
    checkOperationEverywhere(subject, operands, [&](Output& out, LValue value, LValue) {
        if (row.opcode == BitwiseCast)
            return out.opaque(out.bitCast(value, operationTestTypeOf<Result>()));
        if (row.opcode == Opaque)
            return out.opaque(value);
        return out.unary(row.opcode, value);
    }, [&](uint64_t bits, uint64_t) -> std::optional<uint64_t> {
        return operationTestBitsOf(row.reference(operationTestValueOf<Operand>(bits)));
    });
}

// ---- Arithmetic.

template<typename T>
void checkIntegerArithmeticOf()
{
    using U = std::make_unsigned_t<T>;
    static constexpr OperationTestBinaryRow<T, T, T> rows[] = {
        { Add, [](T a, T b) { return static_cast<T>(static_cast<U>(a) + static_cast<U>(b)); } },
        { Sub, [](T a, T b) { return static_cast<T>(static_cast<U>(a) - static_cast<U>(b)); } },
        { Mul, [](T a, T b) { return static_cast<T>(static_cast<U>(a) * static_cast<U>(b)); } },
        { BitAnd, [](T a, T b) { return static_cast<T>(a & b); } },
        { BitOr, [](T a, T b) { return static_cast<T>(a | b); } },
        { BitXor, [](T a, T b) { return static_cast<T>(a ^ b); } },
    };
    for (const auto& row : rows)
        checkBinaryOperation(row, operationTestOperandsOf<T>());
}

template<typename T>
bool operationTestQuotientFits(T dividend, T divisor)
{
    return divisor && !(dividend == std::numeric_limits<T>::min() && divisor == -1);
}

template<typename T>
bool operationTestDivisorIsNotZero(T, T divisor)
{
    return !!divisor;
}

template<typename T>
void checkDivisionsOf()
{
    using U = std::make_unsigned_t<T>;
    static constexpr OperationTestBinaryRow<T, T, T> rows[] = {
        { Div, [](T a, T b) { return static_cast<T>(a / b); }, false, operationTestQuotientFits<T> },
        { Mod, [](T a, T b) { return static_cast<T>(a % b); }, false, operationTestQuotientFits<T> },
        { UDiv, [](T a, T b) { return static_cast<T>(static_cast<U>(a) / static_cast<U>(b)); }, false, operationTestDivisorIsNotZero<T> },
        { UMod, [](T a, T b) { return static_cast<T>(static_cast<U>(a) % static_cast<U>(b)); }, false, operationTestDivisorIsNotZero<T> },
        { Div, [](T a, T b) { return operationTestQuotientFits(a, b) ? static_cast<T>(a / b) : b ? a : static_cast<T>(0); }, true },
        { Mod, [](T a, T b) { return operationTestQuotientFits(a, b) ? static_cast<T>(a % b) : static_cast<T>(0); }, true },
    };
    for (const auto& row : rows)
        checkBinaryOperation(row, operationTestOperandsOf<T>());
}

template<typename T>
T operationTestHighHalfOfProduct(T a, T b)
{
    if constexpr (sizeof(T) == 4) {
        using Wide = std::conditional_t<std::is_signed_v<T>, int64_t, uint64_t>;
        return static_cast<T>((static_cast<Wide>(a) * static_cast<Wide>(b)) >> 32);
    } else {
        using Wide = std::conditional_t<std::is_signed_v<T>, Int128, UInt128>;
        return static_cast<T>((static_cast<Wide>(a) * static_cast<Wide>(b)) >> 64);
    }
}

template<typename T>
void checkHighHalvesOfProductsOf()
{
    using U = std::make_unsigned_t<T>;
    static constexpr OperationTestBinaryRow<T, T, T> rows[] = {
        { MulHigh, [](T a, T b) { return operationTestHighHalfOfProduct(a, b); } },
        { UMulHigh, [](T a, T b) { return static_cast<T>(operationTestHighHalfOfProduct(static_cast<U>(a), static_cast<U>(b))); } },
    };
    for (const auto& row : rows)
        checkBinaryOperation(row, operationTestOperandsOf<T>());
}

template<typename T>
void checkShiftsOf()
{
    using U = std::make_unsigned_t<T>;
    static constexpr int32_t mask = sizeof(T) * 8 - 1;
    static constexpr OperationTestBinaryRow<T, T, int32_t> rows[] = {
        { Shl, [](T a, int32_t b) { return static_cast<T>(static_cast<U>(a) << (b & mask)); } },
        { SShr, [](T a, int32_t b) { return static_cast<T>(a >> (b & mask)); } },
        { ZShr, [](T a, int32_t b) { return static_cast<T>(static_cast<U>(a) >> (b & mask)); } },
    };
    OperationTestOperands operands { operationTestValuesOf<T>(), operationTestShiftAmounts, operationTestValuesOf<T>().first(operationTestSomeValues), operationTestShiftAmounts };
    for (const auto& row : rows)
        checkBinaryOperation(row, operands);
}

template<typename T>
void checkIntegerUnaryOperationsOf(CPUFeatureSet features)
{
    using U = std::make_unsigned_t<T>;
    checkUnaryOperation<T, T>({ Neg, [](T a) { return static_cast<T>(U { 0 } - static_cast<U>(a)); } }, features);
    checkUnaryOperation<T, T>({ Clz, [](T a) { return static_cast<T>(std::countl_zero(static_cast<U>(a))); } }, features);
    checkUnaryOperation<T, T>({ PopCount, [](T a) { return static_cast<T>(std::popcount(static_cast<U>(a))); } }, features);
}

template<typename T>
T operationTestMinOrMax(T a, T b, bool isMax)
{
    if (std::isnan(a) || std::isnan(b))
        return std::numeric_limits<T>::quiet_NaN();
    if (a == b)
        return std::signbit(a) == isMax ? b : a;
    return (a < b) == isMax ? b : a;
}

template<typename T>
void checkNumberArithmeticOf(CPUFeatureSet features)
{
    static constexpr OperationTestBinaryRow<T, T, T> rows[] = {
        { Add, [](T a, T b) { return a + b; } },
        { Sub, [](T a, T b) { return a - b; } },
        { Mul, [](T a, T b) { return a * b; } },
        { Div, [](T a, T b) { return a / b; } },
        { FMin, [](T a, T b) { return operationTestMinOrMax(a, b, false); } },
        { FMax, [](T a, T b) { return operationTestMinOrMax(a, b, true); } },
    };
    for (const auto& row : rows)
        checkBinaryOperation(row, operationTestOperandsOf<T>().withConstants(operationTestSomeValues), features);
}

template<typename T>
void checkNumberUnaryOperationsOf(CPUFeatureSet features)
{
    checkUnaryOperation<T, T>({ Neg, [](T a) { return -a; } }, features);
    checkUnaryOperation<T, T>({ Abs, [](T a) { return std::fabs(a); } }, features);
    checkUnaryOperation<T, T>({ Ceil, [](T a) { return std::ceil(a); } }, features);
    checkUnaryOperation<T, T>({ Floor, [](T a) { return std::floor(a); } }, features);
    checkUnaryOperation<T, T>({ FTrunc, [](T a) { return std::trunc(a); } }, features);
    checkUnaryOperation<T, T>({ Sqrt, [](T a) { return std::sqrt(a); } }, features);
}

// ---- What a Branch or a Select tests.

struct OperationTestUse {
    enum class Kind : uint8_t { Value, Branch, Select, ValueAndBranch };

    ASCIILiteral name;
    Kind kind;
    bool isInverted { false };
    EdgeClass whenNotZero { EdgeClass::Normal };
    EdgeClass whenZero { EdgeClass::Normal };
    bool triesConstants { true };
};

// An inverted comparison of integers is folded into another comparison, so for those the list ends ahead of the inverted uses.
constexpr size_t numberOfOperationTestUsesAsWritten = 7;
constexpr OperationTestUse operationTestUses[] = {
    { "as a value"_s, OperationTestUse::Kind::Value },
    { "as a branch"_s, OperationTestUse::Kind::Branch },
    { "as a select"_s, OperationTestUse::Kind::Select },
    { "as a branch to a rare block"_s, OperationTestUse::Kind::Branch, false, EdgeClass::Rare, EdgeClass::Normal, false },
    { "as a branch past a rare block"_s, OperationTestUse::Kind::Branch, false, EdgeClass::Normal, EdgeClass::Rare, false },
    { "as a branch to an unlikely block"_s, OperationTestUse::Kind::Branch, false, EdgeClass::Unlikely, EdgeClass::Normal, false },
    { "as a value and a branch"_s, OperationTestUse::Kind::ValueAndBranch, false, EdgeClass::Normal, EdgeClass::Normal, false },
    { "inverted, as a branch"_s, OperationTestUse::Kind::Branch, true },
    { "inverted, as a select"_s, OperationTestUse::Kind::Select, true },
    { "inverted, as a branch to a rare block"_s, OperationTestUse::Kind::Branch, true, EdgeClass::Rare, EdgeClass::Normal, false },
    { "inverted, as a branch past a rare block"_s, OperationTestUse::Kind::Branch, true, EdgeClass::Normal, EdgeClass::Rare, false },
};

// One where the predicate as written holds, else zero.
LValue appendBranchOfOperationTest(Output& out, const OperationTestUse& use, LValue tested)
{
    LBasicBlock whenNotZero = out.newBlock();
    LBasicBlock whenZero = out.newBlock();
    LBasicBlock join = out.newBlock();
    out.branch(tested, Edge(whenNotZero, use.whenNotZero), Edge(whenZero, use.whenZero));
    out.appendTo(whenNotZero);
    ValueFromBlock fromNotZero = out.anchor(out.constInt32(!use.isInverted));
    out.jump(join);
    out.appendTo(whenZero);
    ValueFromBlock fromZero = out.anchor(out.constInt32(use.isInverted));
    out.jump(join);
    out.appendTo(join);
    return out.phi(Int32, fromNotZero, fromZero);
}

LValue appendUseOfPredicate(Output& out, const OperationTestUse& use, LValue predicate)
{
    LValue tested = use.isInverted ? out.equal(predicate, 0) : predicate;
    switch (use.kind) {
    case OperationTestUse::Kind::Value:
        return predicate;
    case OperationTestUse::Kind::Branch:
        return appendBranchOfOperationTest(out, use, tested);
    case OperationTestUse::Kind::Select:
        return out.select(tested, out.opaque(out.constInt32(!use.isInverted)), out.opaque(out.constInt32(use.isInverted)));
    case OperationTestUse::Kind::ValueAndBranch:
        return out.bitOr(out.shl(appendBranchOfOperationTest(out, use, tested), 1), predicate);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// buildPredicate(Output&, LValue left, LValue right) answers an Int32 that is zero or one.
template<typename T, typename BuildPredicate>
void checkPredicate(ASCIILiteral name, bool (*reference)(T, T), std::span<const OperationTestUse> uses, const OperationTestOperands& operands, const BuildPredicate& buildPredicate)
{
    for (const OperationTestUse& use : uses) {
        OperationTestSubject subject { name, operationTestTypeOf<T>(), operationTestTypeOf<T>(), Int32, use.name };
        OperationTestOperands operandsOfUse = use.kind == OperationTestUse::Kind::Value ? operands : operands.cutShort();
        if (!use.triesConstants)
            operandsOfUse = operandsOfUse.withConstants(0);
        checkOperationEverywhere(subject, operandsOfUse, [&](Output& out, LValue left, LValue right) {
            return appendUseOfPredicate(out, use, buildPredicate(out, left, right));
        }, [&](uint64_t left, uint64_t right) -> std::optional<uint64_t> {
            uint64_t holds = reference(operationTestValueOf<T>(left), operationTestValueOf<T>(right));
            return use.kind == OperationTestUse::Kind::ValueAndBranch ? 3 * holds : holds;
        });
    }
}

template<typename T>
struct OperationTestComparison {
    Opcode opcode;
    bool (*reference)(T, T);
};

template<typename T>
std::span<const OperationTestComparison<T>> operationTestComparisonsOf()
{
    if constexpr (std::is_integral_v<T>) {
        using U = std::make_unsigned_t<T>;
        static constexpr OperationTestComparison<T> rows[] = {
            { Equal, [](T a, T b) { return a == b; } },
            { NotEqual, [](T a, T b) { return a != b; } },
            { LessThan, [](T a, T b) { return a < b; } },
            { GreaterThan, [](T a, T b) { return a > b; } },
            { LessEqual, [](T a, T b) { return a <= b; } },
            { GreaterEqual, [](T a, T b) { return a >= b; } },
            { Above, [](T a, T b) { return static_cast<U>(a) > static_cast<U>(b); } },
            { Below, [](T a, T b) { return static_cast<U>(a) < static_cast<U>(b); } },
            { AboveEqual, [](T a, T b) { return static_cast<U>(a) >= static_cast<U>(b); } },
            { BelowEqual, [](T a, T b) { return static_cast<U>(a) <= static_cast<U>(b); } },
        };
        return rows;
    } else {
        static constexpr OperationTestComparison<T> rows[] = {
            { Equal, [](T a, T b) { return a == b; } },
            { NotEqual, [](T a, T b) { return a != b; } },
            { LessThan, [](T a, T b) { return a < b; } },
            { GreaterThan, [](T a, T b) { return a > b; } },
            { LessEqual, [](T a, T b) { return a <= b; } },
            { GreaterEqual, [](T a, T b) { return a >= b; } },
            { EqualOrUnordered, [](T a, T b) { return a == b || std::isnan(a) || std::isnan(b); } },
        };
        return rows;
    }
}

template<typename T>
void checkComparisonsOf()
{
    std::span<const OperationTestUse> uses(operationTestUses);
    if (std::is_integral_v<T>)
        uses = uses.first(numberOfOperationTestUsesAsWritten);
    for (const OperationTestComparison<T>& row : operationTestComparisonsOf<T>()) {
        checkPredicate<T>(descriptionOf(row.opcode).name, row.reference, uses, operationTestOperandsOf<T>().withConstants(operationTestSomeValues), [&](Output& out, LValue left, LValue right) {
            return out.binary(row.opcode, left, right);
        });
    }
}

template<typename T>
void checkBitTestsOf()
{
    checkPredicate<T>("BitAnd that is not zero"_s, [](T a, T b) { return !!(a & b); }, operationTestUses, operationTestOperandsOf<T>(), [](Output& out, LValue left, LValue right) {
        return out.notEqual(out.bitAnd(left, right), 0);
    });
}

template<typename Predicate, typename T>
void checkSelectOnValuesOf()
{
    constexpr uint64_t other = operationTestBitsOf(static_cast<T>(77));
    Type type = operationTestTypeOf<T>();
    for (bool otherIsConstant : { false, true }) {
        for (bool isChosenWhenNotZero : { true, false }) {
            OperationTestSubject subject { "Select"_s, operationTestTypeOf<Predicate>(), type, type, isChosenWhenNotZero ? "of the operand where not zero"_s : "of the operand where zero"_s };
            subject.keepsBitsOfNaN = true;
            checkOperationEverywhere(subject, operationTestOperandsOf<Predicate, T>().cutShort(), [&](Output& out, LValue predicate, LValue chosen) {
                LValue otherValue = otherIsConstant ? operationTestConstant(out, other, type) : operationTestValueOfBits(out, out.opaque(out.constInt64(other)), type);
                return isChosenWhenNotZero ? out.select(predicate, chosen, otherValue) : out.select(predicate, otherValue, chosen);
            }, [&](uint64_t predicate, uint64_t chosen) -> std::optional<uint64_t> {
                return !!predicate == isChosenWhenNotZero ? chosen : other;
            });
        }
    }
}

template<typename Predicate>
void checkSelectOnValuesOfEveryType()
{
    checkSelectOnValuesOf<Predicate, int32_t>();
    checkSelectOnValuesOf<Predicate, int64_t>();
    checkSelectOnValuesOf<Predicate, float>();
    checkSelectOnValuesOf<Predicate, double>();
}

template<typename T>
void checkSelectBetweenComparedOperandsOf()
{
    Type type = operationTestTypeOf<T>();
    for (const OperationTestComparison<T>& row : operationTestComparisonsOf<T>()) {
        if (std::is_integral_v<T> && row.opcode != Equal && row.opcode != LessThan && row.opcode != Above)
            continue;
        for (bool comparesInOrder : { true, false }) {
            OperationTestSubject subject { descriptionOf(row.opcode).name, type, type, type, comparesInOrder ? "that chooses between its operands"_s : "that chooses between its operands, swapped"_s };
            subject.keepsBitsOfNaN = true;
            checkOperationEverywhere(subject, operationTestOperandsOf<T>().cutShort(), [&](Output& out, LValue left, LValue right) {
                return out.select(comparesInOrder ? out.binary(row.opcode, left, right) : out.binary(row.opcode, right, left), left, right);
            }, [&](uint64_t left, uint64_t right) -> std::optional<uint64_t> {
                T a = operationTestValueOf<T>(left);
                T b = operationTestValueOf<T>(right);
                return (comparesInOrder ? row.reference(a, b) : row.reference(b, a)) ? left : right;
            });
        }
    }
}

// ---- Memory.

struct OperationTestAccess {
    Opcode load;
    Opcode store;
    Type type;
    unsigned bytes;
    bool extendsSign;
};

constexpr OperationTestAccess operationTestAccesses[] = {
    { Load8Z, Store8, Int32, 1, false },
    { Load8S, Store8, Int32, 1, true },
    { Load16Z, Store16, Int32, 2, false },
    { Load16S, Store16, Int32, 2, true },
    { Load, Store, Int32, 4, false },
    { Load, Store, Int64, 8, false },
    { Load, Store, Float, 4, false },
    { Load, Store, Double, 8, false },
};

// What the load gives where the low bytes of the word lie in memory.
uint64_t bitsLoadedByOperationTestAccess(const OperationTestAccess& access, uint64_t word)
{
    if (access.bytes < sizeof(uint64_t))
        word &= (uint64_t { 1 } << (8 * access.bytes)) - 1;
    if (!access.extendsSign)
        return word;
    return static_cast<uint32_t>(access.bytes == 1 ? static_cast<int8_t>(word) : static_cast<int16_t>(word));
}

struct OperationTestAddress {
    // What the load and the store are told.
    int64_t offset { 0 };
    // Of an Add of a constant to the base.
    int64_t addend { 0 };
    bool hasIndex { false };
    unsigned shift { 0 };
    IndexExtension extension { IndexExtension::None };
};

LValue appendBaseOfOperationTestAddress(Output& out, const OperationTestAddress& address, LValue base, LValue indexBits)
{
    if (address.addend)
        base = out.add(base, out.constInt64(address.addend));
    if (!address.hasIndex)
        return base;
    LValue index = indexBits;
    switch (address.extension) {
    case IndexExtension::None:
        break;
    case IndexExtension::ZeroExtend32:
        index = out.zeroExtPtr(out.castToInt32(indexBits));
        break;
    case IndexExtension::SignExtend32:
        index = out.signExt32ToPtr(out.castToInt32(indexBits));
        break;
    }
    return out.add(base, address.shift ? out.shl(index, address.shift) : index);
}

uint64_t distanceOfOperationTestAddress(const OperationTestAddress& address, uint64_t index)
{
    uint64_t distance = static_cast<uint64_t>(address.offset) + static_cast<uint64_t>(address.addend);
    if (!address.hasIndex)
        return distance;
    switch (address.extension) {
    case IndexExtension::None:
        break;
    case IndexExtension::ZeroExtend32:
        index = static_cast<uint32_t>(index);
        break;
    case IndexExtension::SignExtend32:
        index = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(index)));
        break;
    }
    return distance + (index << address.shift);
}

// Copies one value from one buffer to another, both times through an address of the given form. Each base is the place in the buffer less the distance, so any distance will do.
void checkAccessThroughAddress(const OperationTestAccess& access, const OperationTestAddress& address)
{
    OperationTestProcedure test;
    Output& out = test.out();
    Builder& builder = out;
    LValue sourceBase = test.argumentGPR(0);
    LValue destinationBase = test.argumentGPR(1);
    LValue sourceIndex = test.argumentGPR(2);
    LValue destinationIndex = test.argumentGPR(3);
    LValue value = builder.load(access.load, access.type, appendBaseOfOperationTestAddress(out, address, sourceBase, sourceIndex), address.offset, test.memory());
    builder.store(access.store, value, appendBaseOfOperationTestAddress(out, address, destinationBase, destinationIndex), address.offset, test.memory());
    out.ret(operationTestBitsOfValue(out, value));
    compileOrFail(test);

    static constexpr uint64_t indexes[] = { 0, 1, 5, 0xfffffffffffffffdull };
    constexpr size_t place = 32;
    constexpr uint8_t untouched = 0xa5;
    for (uint64_t index : std::span<const uint64_t>(indexes).first(address.hasIndex ? std::size(indexes) : 1)) {
        for (unsigned flippedBits : { 0x00u, 0xffu }) {
            alignas(16) std::array<uint8_t, 64> source;
            alignas(16) std::array<uint8_t, 64> destination;
            alignas(16) std::array<uint8_t, 64> expectedDestination;
            for (size_t position = 0; position < source.size(); ++position)
                source[position] = static_cast<uint8_t>((17 + 29 * position) ^ flippedBits);
            destination.fill(untouched);
            expectedDestination.fill(untouched);
            memcpy(expectedDestination.data() + place, source.data() + place, access.bytes);
            uint64_t word = 0;
            memcpy(&word, source.data() + place, access.bytes);

            uint64_t distance = distanceOfOperationTestAddress(address, index);
            uint64_t indexArgument = address.extension == IndexExtension::None ? index : operationTestWithDirtAbove(index, sizeof(uint32_t));
            uint64_t returned = test.call<uint64_t>(std::bit_cast<uintptr_t>(source.data()) + place - distance, std::bit_cast<uintptr_t>(destination.data()) + place - distance, indexArgument, indexArgument);
            checkOperationTest(returned == bitsLoadedByOperationTestAccess(access, word) && destination == expectedDestination, test, access.load, " and ", access.store, " of ", access.type,
                " at offset ", address.offset, ", addend ", address.addend, ", index ", RawHex(index), " << ", address.shift, ": loaded ", RawHex(returned));
        }
    }
}

struct OperationTestStore {
    // The place, in values of the stored type from the base.
    int element;
    // 0 and 1: the arguments. 2: the constant zero.
    unsigned value;
};

struct OperationTestStores {
    std::array<OperationTestStore, 4> stores;
    unsigned size;
};

void checkStoresInARow(Type type, const OperationTestStores& sequence, int bytesOffAlignment)
{
    OperationTestProcedure test;
    Output& out = test.out();
    LValue base = test.argumentGPR(0);
    LValue firstBits = test.argumentGPR(1);
    LValue secondBits = test.argumentGPR(2);
    LValue values[] = { operationTestValueOfBits(out, firstBits, type), operationTestValueOfBits(out, secondBits, type), operationTestConstant(out, 0, type) };
    int width = static_cast<int>(sizeOf(type));
    std::span<const OperationTestStore> stores = std::span<const OperationTestStore>(sequence.stores).first(sequence.size);
    for (const OperationTestStore& store : stores)
        out.store(values[store.value], out.address(test.memory(), base, store.element * width + bytesOffAlignment));
    out.retVoid();
    compileOrFail(test);

    constexpr int elementsOnEachSide = 80;
    constexpr uint64_t bits[] = { 0x1122334455667788ull, 0x99aabbccddeeff00ull, 0 };
    Vector<uint8_t> bytes(static_cast<size_t>(2 * elementsOnEachSide * width), [](size_t) -> uint8_t { return 0xa5; });
    Vector<uint8_t> expectedBytes = bytes;
    int middle = elementsOnEachSide * width;
    for (const OperationTestStore& store : stores)
        memcpy(expectedBytes.mutableSpan().data() + middle + store.element * width + bytesOffAlignment, &bits[store.value], width);
    test.call<void>(bytes.mutableSpan().data() + middle, bits[0], bits[1]);
    checkOperationTest(bytes == expectedBytes, test, "stores of ", type, " in a row, the first at element ", stores[0].element, ", ", bytesOffAlignment, " bytes off");
}

// A frame of the code under test is larger than a page, and on Windows the pages of the stack have to be touched in order.
NEVER_INLINE void touchStackBelowOperationTest()
{
    volatile uint8_t bytes[64 * KB];
    for (size_t offset = sizeof(bytes); offset; offset -= 4 * KB)
        bytes[offset - 1] = 0;
}

// ---- Switch.

constexpr uint64_t markerOfSwitchTarget(unsigned target) { return static_cast<uint64_t>(target + 1) << 44; }

// Each target returns the subject, which so lives across the Switch, plus its marker. Case i goes to target i modulo the number of targets.
void checkSwitchOn(Type type, std::span<const int64_t> caseValues, unsigned numberOfTargets)
{
    OperationTestProcedure test;
    Output& out = test.out();
    LValue subject = operationTestValueOfBits(out, test.argumentGPR(0), type);
    Vector<LBasicBlock> targets;
    for (unsigned target = 0; target < numberOfTargets; ++target)
        targets.append(out.newBlock());
    LBasicBlock otherwise = out.newBlock();
    Vector<SwitchCase> cases;
    for (size_t index = 0; index < caseValues.size(); ++index)
        cases.append(SwitchCase { caseValues[index], Edge(targets[index % numberOfTargets]) });
    out.switchOn(subject, cases.span(), Edge(otherwise));
    auto returnFrom = [&](LBasicBlock block, uint64_t marker) {
        out.appendTo(block);
        LValue wide = type == Int32 ? out.signExt32To64(subject) : subject;
        out.ret(out.add(wide, out.constInt64(static_cast<int64_t>(marker))));
    };
    for (unsigned target = 0; target < numberOfTargets; ++target)
        returnFrom(targets[target], markerOfSwitchTarget(target));
    returnFrom(otherwise, 0);
    compileOrFail(test);

    auto check = [&](uint64_t bits) {
        int64_t value = type == Int32 ? static_cast<int32_t>(bits) : static_cast<int64_t>(bits);
        uint64_t expected = static_cast<uint64_t>(value);
        for (size_t index = 0; index < caseValues.size(); ++index) {
            if (caseValues[index] == value)
                expected += markerOfSwitchTarget(static_cast<unsigned>(index % numberOfTargets));
        }
        uint64_t returned = test.call<uint64_t>(operationTestWithDirtAbove(bits, sizeOf(type)));
        checkOperationTest(returned == expected, test, "Switch on ", type, " with ", caseValues.size(), " cases from ", caseValues[0], ": ", value, " gave ", RawHex(returned));
    };
    for (int64_t caseValue : caseValues) {
        uint64_t bits = static_cast<uint64_t>(caseValue);
        for (uint64_t distance : { uint64_t { 0 }, uint64_t { 1 }, ~uint64_t { 0 }, uint64_t { 1 } << 32, uint64_t { 1 } << 63 })
            check(bits + distance);
    }
    for (uint64_t bits : { uint64_t { 0 }, uint64_t { 0x7fffffff }, uint64_t { 0x80000000 }, uint64_t { 0xffffffff }, ~uint64_t { 0 } >> 1, uint64_t { 1 } << 63, ~uint64_t { 0 } })
        check(bits);
}

void checkSwitchOnEachType(std::span<const int64_t> caseValues)
{
    bool fitsInt32 = std::ranges::all_of(caseValues, [](int64_t value) { return value == static_cast<int32_t>(value); });
    unsigned numberOfCases = static_cast<unsigned>(caseValues.size());
    for (unsigned numberOfTargets : { numberOfCases, std::min(numberOfCases, 3u) }) {
        if (fitsInt32)
            checkSwitchOn(Int32, caseValues, numberOfTargets);
        checkSwitchOn(Int64, caseValues, numberOfTargets);
    }
}

void checkSwitchOnRun(int64_t first, unsigned count)
{
    Vector<int64_t> caseValues;
    for (unsigned index = 0; index < count; ++index)
        caseValues.append(static_cast<int64_t>(static_cast<uint64_t>(first) + index));
    checkSwitchOnEachType(caseValues.span());
}

template<typename T>
void checkConstantsOf()
{
    for (uint64_t bits : operationTestValuesOf<T>()) {
        OperationTestProcedure test;
        test.out().ret(operationTestConstant(test.out(), bits, operationTestTypeOf<T>()));
        compileOrFail(test);
        uint64_t returned = operationTestBitsOf(test.call<T>());
        checkOperationTest(returned == bits, test, "constant ", RawHex(bits), " of ", operationTestTypeOf<T>(), " came back as ", RawHex(returned));
    }
}

template<void (*run)()>
void runOperationTestWhereCodeRuns()
{
    if (ExecutableAllocator::singleton().isValid())
        run();
}

} // anonymous namespace

static void testMachineOperationsOfIntegerArithmetic()
{
    checkIntegerArithmeticOf<int32_t>();
    checkIntegerArithmeticOf<int64_t>();
}

static void testMachineOperationsOfDivision()
{
    checkDivisionsOf<int32_t>();
    checkDivisionsOf<int64_t>();
}

static void testMachineOperationsOfHighHalvesOfProducts()
{
    checkHighHalvesOfProductsOf<int32_t>();
    checkHighHalvesOfProductsOf<int64_t>();
}

static void testMachineOperationsOfShifting()
{
    checkShiftsOf<int32_t>();
    checkShiftsOf<int64_t>();
}

static void testMachineOperationsOnOneInteger()
{
    for (CPUFeatureSet features : operationTestFeatureSets()) {
        checkIntegerUnaryOperationsOf<int32_t>(features);
        checkIntegerUnaryOperationsOf<int64_t>(features);
    }
}

static void testMachineOperationsOfConversion()
{
    checkUnaryOperation<int32_t, int32_t>({ SExt8, [](int32_t a) { return static_cast<int32_t>(static_cast<int8_t>(a)); } });
    checkUnaryOperation<int32_t, int32_t>({ SExt16, [](int32_t a) { return static_cast<int32_t>(static_cast<int16_t>(a)); } });
    checkUnaryOperation<int64_t, int32_t>({ SExt32, [](int32_t a) { return static_cast<int64_t>(a); } });
    checkUnaryOperation<int64_t, int32_t>({ ZExt32, [](int32_t a) { return static_cast<int64_t>(static_cast<uint32_t>(a)); } });
    checkUnaryOperation<int32_t, int64_t>({ Trunc, [](int64_t a) { return static_cast<int32_t>(a); } });
    for (CPUFeatureSet features : operationTestFeatureSets()) {
        checkUnaryOperation<double, int32_t>({ IToD, [](int32_t a) { return static_cast<double>(a); } }, features);
        checkUnaryOperation<double, int64_t>({ IToD, [](int64_t a) { return static_cast<double>(a); } }, features);
        checkUnaryOperation<double, float>({ FloatToDouble, [](float a) { return static_cast<double>(a); } }, features);
        checkUnaryOperation<float, double>({ DoubleToFloat, [](double a) { return static_cast<float>(a); } }, features);
        checkUnaryOperation<float, int32_t>({ BitwiseCast, [](int32_t a) { return std::bit_cast<float>(a); } }, features, true);
        checkUnaryOperation<int32_t, float>({ BitwiseCast, [](float a) { return std::bit_cast<int32_t>(a); } }, features, true);
        checkUnaryOperation<double, int64_t>({ BitwiseCast, [](int64_t a) { return std::bit_cast<double>(a); } }, features, true);
        checkUnaryOperation<int64_t, double>({ BitwiseCast, [](double a) { return std::bit_cast<int64_t>(a); } }, features, true);
    }
}

static void testMachineOperationsThatAreOpaque()
{
    checkUnaryOperation<int32_t, int32_t>({ Opaque, [](int32_t a) { return a; } });
    checkUnaryOperation<int64_t, int64_t>({ Opaque, [](int64_t a) { return a; } });
    checkUnaryOperation<float, float>({ Opaque, [](float a) { return a; } }, { }, true);
    checkUnaryOperation<double, double>({ Opaque, [](double a) { return a; } }, { }, true);
}

static void testMachineOperationsOfNumberArithmetic()
{
    for (CPUFeatureSet features : operationTestFeatureSets()) {
        checkNumberArithmeticOf<float>(features);
        checkNumberArithmeticOf<double>(features);
    }
}

static void testMachineOperationsOnOneNumber()
{
    for (CPUFeatureSet features : operationTestFeatureSets()) {
        checkNumberUnaryOperationsOf<float>(features);
        checkNumberUnaryOperationsOf<double>(features);
    }
}

static void testMachineOperationsThatTestBits()
{
    checkBitTestsOf<int32_t>();
    checkBitTestsOf<int64_t>();
}

static void testMachineOperationsThatSelectBetweenComparedOperands()
{
    checkSelectBetweenComparedOperandsOf<int32_t>();
    checkSelectBetweenComparedOperandsOf<int64_t>();
    checkSelectBetweenComparedOperandsOf<float>();
    checkSelectBetweenComparedOperandsOf<double>();
}

static void testMachineOperationsThatSwitch()
{
    constexpr int64_t least = std::numeric_limits<int64_t>::min();
    constexpr int64_t greatest = std::numeric_limits<int64_t>::max();
    static constexpr int64_t withHoles[] = { 10, 11, 13, 14, 16, 17, 18, 20 };
    static constexpr int64_t halfFilled[] = { 0, 2, 4, 6, 8, 10, 12 };
    static constexpr int64_t lessThanHalfFilled[] = { 0, 3, 6, 9, 12, 15, 18, 21 };
    static constexpr int64_t sparse[] = { -1000000, -5, 0, 7, 1000, 65536, 0x7fffffff };
    static constexpr int64_t asFarApartAsTableTakes[] = { 0, 31, 47, 63, 79, 95, 111 };
    static constexpr int64_t fartherApartThanTableTakes[] = { 0, 32, 64, 96, 128, 160, 192 };
    static constexpr int64_t kindsOfNodes[] = { 174, 175, 176, 177, 178, 179, 180, 181, 182, 185, 186, 195, 201, 219, 220, 242, 249, 250, 251, 263, 266, 268, 270, 300, 308, 318, 324, 339, 341, 347 };
    static constexpr int64_t shortRunsAmongOthers[] = { -70000, 10, 13, 40, 41, 8232, 8233, 8234, 8290, 1 << 20 };
    static constexpr int64_t runsAmongOthers[] = { -100, 0, 1, 2, 3, 4, 5, 6, 7, 500, 1000, 1001, 1002, 1003, 1004, 1005, 1006, 100000 };
    static constexpr int64_t outOfOrder[] = { 5, 3, 9, 1, 7, 0, 8, 2, 6, 4 };
    static constexpr int64_t single[] = { 42 };
    static constexpr int64_t farApart[] = { least, -(int64_t { 1 } << 32), -1, 0, int64_t { 1 } << 32, (int64_t { 1 } << 32) + 1, greatest };
    for (std::span<const int64_t> caseValues : std::initializer_list<std::span<const int64_t>> { withHoles, halfFilled, lessThanHalfFilled, sparse, asFarApartAsTableTakes, fartherApartThanTableTakes, kindsOfNodes, shortRunsAmongOthers, runsAmongOthers, outOfOrder, single, farApart })
        checkSwitchOnEachType(caseValues);

    checkSwitchOnRun(0, 10);
    checkSwitchOnRun(-3, 8);
    checkSwitchOnRun(100, minCasesForJumpTable);
    checkSwitchOnRun(100, minCasesForJumpTable - 1);
    checkSwitchOnRun(std::numeric_limits<int32_t>::max() - 7, 8);
    checkSwitchOnRun(std::numeric_limits<int32_t>::min(), 8);
    checkSwitchOnRun(int64_t { 1 } << 40, 9);
    checkSwitchOnRun(greatest - 7, 8);
    checkSwitchOnRun(least, 8);
}

static void testMachineOperationsOnMemoryAtOffsets()
{
    static constexpr int64_t offsets[] = {
        0, 1, 8, 255, 256, 257, -1, -8, -256, -257, 4095, 4096, 8190, 8192, 16380, 16384, 32760, 32768, 65536, 1 << 20, -(1 << 20),
        0x7fffffff, -0x80000000ll, 0x80000000ll, -0x80000001ll, 0x123456789ll,
    };
    for (const OperationTestAccess& access : operationTestAccesses) {
        for (int64_t offset : offsets)
            checkAccessThroughAddress(access, { offset });
        checkAccessThroughAddress(access, { 8, 16 });
        checkAccessThroughAddress(access, { 8, -8 });
        checkAccessThroughAddress(access, { 0x7fffffff, 0x7fffffff });
        checkAccessThroughAddress(access, { -8, 0x123456789ll });
    }
}

static void testMachineOperationsOnMemoryThroughIndexes()
{
    for (const OperationTestAccess& access : operationTestAccesses) {
        for (unsigned shift = 0; shift <= 4; ++shift) {
            for (IndexExtension extension : { IndexExtension::None, IndexExtension::ZeroExtend32, IndexExtension::SignExtend32 }) {
                for (int64_t offset : { 0, 16, 4096 })
                    checkAccessThroughAddress(access, { offset, 0, true, shift, extension });
            }
        }
    }
}

static void testMachineOperationsThatStoreConstants()
{
    static constexpr uint64_t constants[] = {
        0, 1, 0x7f, 0x80, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0x123456789abcdef0, 0xffffffff80000000, 0xffffffffffffffff, 0x3ff0000000000000, 0xfffe000000000000,
    };
    for (const OperationTestAccess& access : operationTestAccesses) {
        if (access.extendsSign)
            continue;
        for (uint64_t constant : constants) {
            OperationTestProcedure test;
            Builder& builder = test.out();
            builder.store(access.store, operationTestConstant(test.out(), constant, access.type), test.argumentGPR(0), 8, test.memory());
            builder.retVoid();
            compileOrFail(test);

            std::array<uint8_t, 24> bytes;
            std::array<uint8_t, 24> expectedBytes;
            bytes.fill(0xa5);
            expectedBytes.fill(0xa5);
            memcpy(expectedBytes.data() + 8, &constant, access.bytes);
            test.call<void>(bytes.data());
            checkOperationTest(bytes == expectedBytes, test, access.store, " of the constant ", RawHex(constant), " as ", access.type);
        }
    }
}

static void testMachineOperationsThatStoreInARow()
{
    static constexpr OperationTestStores sequences[] = {
        { { { { 1, 0 }, { 2, 1 } } }, 2 },
        { { { { 2, 0 }, { 1, 1 } } }, 2 },
        { { { { 1, 0 }, { 1, 1 } } }, 2 },
        { { { { 1, 0 }, { 2, 0 } } }, 2 },
        { { { { 1, 2 }, { 2, 2 } } }, 2 },
        { { { { 1, 0 }, { 2, 2 } } }, 2 },
        { { { { -1, 0 }, { 0, 1 } } }, 2 },
        { { { { 1, 0 }, { 3, 1 } } }, 2 },
        { { { { 62, 0 }, { 63, 1 } } }, 2 },
        { { { { 63, 0 }, { 64, 1 } } }, 2 },
        { { { { -64, 0 }, { -63, 1 } } }, 2 },
        { { { { -65, 0 }, { -64, 1 } } }, 2 },
        { { { { 1, 0 }, { 2, 1 }, { 3, 0 } } }, 3 },
        { { { { 1, 0 }, { 2, 1 }, { 3, 0 }, { 4, 1 } } }, 4 },
        { { { { 1, 0 }, { 2, 1 }, { 2, 0 }, { 3, 1 } } }, 4 },
    };
    for (Type type : { Type(Int32), Type(Int64), Type(Float), Type(Double) }) {
        for (const OperationTestStores& sequence : sequences) {
            for (int bytesOffAlignment : { 0, 1 })
                checkStoresInARow(type, sequence, bytesOffAlignment);
        }
    }
}

static void testMachineOperationsOnNarrowLoads()
{
    struct NarrowLoad {
        Opcode opcode;
        std::span<const uint64_t> values;
    };
    static constexpr uint64_t bytes[] = { 0, 1, 0x7f, 0x80, 0xff };
    static constexpr uint64_t signedBytes[] = { 0, 1, 0x7f, 0xffffff80, 0xffffffff };
    static constexpr uint64_t halves[] = { 0, 1, 0x1234, 0x7fff, 0x8000, 0xffff };
    static constexpr uint64_t signedHalves[] = { 0, 1, 0x1234, 0x7fff, 0xffff8000, 0xffffffff };
    static constexpr OperationTestBinaryRow<int32_t, int32_t, int32_t> rows[] = {
        { Add, [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); } },
        { Sub, [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); } },
        { BitAnd, [](int32_t a, int32_t b) { return a & b; } },
    };
    for (const NarrowLoad& load : { NarrowLoad { Load8Z, bytes }, NarrowLoad { Load8S, signedBytes }, NarrowLoad { Load16Z, halves }, NarrowLoad { Load16S, signedHalves } }) {
        OperationTestOperands operands { load.values, load.values, load.values, load.values };
        operands.load = load.opcode;
        for (const auto& row : rows)
            checkBinaryOperation(row, operands);
        for (const OperationTestComparison<int32_t>& row : operationTestComparisonsOf<int32_t>()) {
            if (row.opcode != Equal && row.opcode != LessThan && row.opcode != Below)
                continue;
            checkPredicate<int32_t>(descriptionOf(row.opcode).name, row.reference, std::span<const OperationTestUse>(operationTestUses).first(3), operands, [&](Output& out, LValue left, LValue right) {
                return out.binary(row.opcode, left, right);
            });
        }
    }
}

static void testMachineOperationsOnStackSlots()
{
    touchStackBelowOperationTest();
    constexpr uint64_t guard = 0xa5a5a5a5a5a5a5a5ull;
    for (unsigned size : { 16u, 4112u, 40000u }) {
        for (const OperationTestAccess& access : operationTestAccesses) {
            for (unsigned offset : { 0u, 8u, size - 8 }) {
                OperationTestProcedure test;
                Output& out = test.out();
                Builder& builder = out;
                const AbstractHeap& heap = test.heaps().frameSlots;
                LValue bits = test.argumentGPR(0);
                LValue results = test.argumentGPR(1);
                LValue distance = test.argumentGPR(2);
                LValue slot = out.lockedStackSlot(size);
                out.store64(out.constInt64(static_cast<int64_t>(guard)), out.address(heap, slot, offset));
                builder.store(access.store, operationTestValueOfBits(out, bits, access.type), slot, offset, heap);
                out.store64(out.load64(out.address(heap, slot, offset)), out.address(test.memory(), results, 0));
                out.store64(operationTestBitsOfValue(out, builder.load(access.load, access.type, out.add(slot, distance), 0, heap)), out.address(test.memory(), results, sizeof(uint64_t)));
                out.ret(operationTestBitsOfValue(out, builder.load(access.load, access.type, slot, offset, heap)));
                compileOrFail(test);

                for (uint64_t word : { 0x8899aabbccddeeffull, 0x0011223344556677ull }) {
                    uint64_t wholeWord = guard;
                    memcpy(&wholeWord, &word, access.bytes);
                    uint64_t loaded = bitsLoadedByOperationTestAccess(access, word);
                    uint64_t stored[2] = { 0, 0 };
                    uint64_t returned = test.call<uint64_t>(word, stored, uint64_t { offset });
                    checkOperationTest(returned == loaded && stored[0] == wholeWord && stored[1] == loaded, test, access.load, " and ", access.store, " of ", access.type,
                        " at ", offset, " in a slot of ", size, " bytes: ", RawHex(returned), ", ", RawHex(stored[0]), ", ", RawHex(stored[1]));
                }
            }
        }
    }
}

static void testMachineOperationsThatAreConstants()
{
    checkConstantsOf<int32_t>();
    checkConstantsOf<int64_t>();
    checkConstantsOf<float>();
    checkConstantsOf<double>();
}

static void testMachineOperationsOnArgumentsAndReturns()
{
    {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue arguments[] = { test.argumentGPR(0), test.argumentGPR(1), test.argumentGPR(2), test.argumentGPR(3) };
        LValue digits = arguments[0];
        for (unsigned index = 1; index < std::size(arguments); ++index)
            digits = out.add(out.mul(digits, 10), arguments[index]);
        out.ret(digits);
        compileOrFail(test);
        checkOperationTest(test.call<int64_t>(int64_t { 1 }, int64_t { 2 }, int64_t { 3 }, int64_t { 4 }) == 1234, test, "four integer arguments");
    }
    {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue arguments[] = { test.argumentFPR(0), test.argumentFPR(1), test.argumentFPR(2), test.argumentFPR(3) };
        LValue digits = arguments[0];
        for (unsigned index = 1; index < std::size(arguments); ++index)
            digits = out.doubleAdd(out.doubleMul(digits, out.constDouble(10)), arguments[index]);
        out.ret(digits);
        compileOrFail(test);
        checkOperationTest(test.call<double>(1.0, 2.0, 3.0, 4.0) == 1234.0, test, "four number arguments");
    }
    constexpr uint64_t bits = 0xc0490fdb40490fdbull;
    {
        OperationTestProcedure test;
        test.out().ret(test.out().castToInt32(test.argumentGPR(0)));
        compileOrFail(test);
        checkOperationTest(test.call<int32_t>(bits) == static_cast<int32_t>(bits), test, "Return of an Int32");
    }
    {
        OperationTestProcedure test;
        test.out().ret(operationTestValueOfBits(test.out(), test.argumentGPR(0), Float));
        compileOrFail(test);
        checkOperationTest(operationTestBitsOf(test.call<float>(bits)) == static_cast<uint32_t>(bits), test, "Return of a Float");
    }
    {
        OperationTestProcedure test;
        test.out().ret(operationTestValueOfBits(test.out(), test.argumentGPR(0), Double));
        compileOrFail(test);
        checkOperationTest(operationTestBitsOf(test.call<double>(bits)) == bits, test, "Return of a Double");
    }
    {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue pointer = test.argumentGPR(0);
        LValue word = test.argumentGPR(1);
        out.store64(word, out.address(test.memory(), pointer));
        out.retVoid();
        compileOrFail(test);
        uint64_t stored = 0;
        test.call<void>(&stored, bits);
        checkOperationTest(stored == bits, test, "Return of nothing");
    }
}

static void testMachineOperationsInLoopWithPhisInBothBanks()
{
    OperationTestProcedure test;
    Output& out = test.out();
    LValue words = test.argumentGPR(0);
    LValue count = test.argumentGPR(1);
    LValue placeOfNumber = test.argumentGPR(2);
    LBasicBlock head = out.newBlock();
    LBasicBlock body = out.newBlock();
    LBasicBlock end = out.newBlock();
    ValueFromBlock firstIndex = out.anchor(out.int64Zero);
    ValueFromBlock firstSum = out.anchor(out.int64Zero);
    ValueFromBlock firstNumber = out.anchor(out.constDouble(0));
    out.jump(head);

    out.appendTo(head);
    LValue index = out.phi(Int64, firstIndex);
    LValue sum = out.phi(Int64, firstSum);
    LValue number = out.phi(Double, firstNumber);
    out.branch(out.below(index, count), normalEdge(body), normalEdge(end));

    out.appendTo(body);
    LValue word = out.load64(out.baseIndex(test.memory(), words, index, ScaleEight));
    out.addIncomingToPhi(index, out.anchor(out.add(index, 1)));
    out.addIncomingToPhi(sum, out.anchor(out.add(sum, word)));
    out.addIncomingToPhi(number, out.anchor(out.doubleAdd(number, out.intToDouble(word))));
    out.jump(head);

    out.appendTo(end);
    out.storeDouble(number, out.address(test.memory(), placeOfNumber));
    out.ret(sum);
    compileOrFail(test);

    std::array<int64_t, 17> memory;
    for (size_t position = 0; position < memory.size(); ++position)
        memory[position] = static_cast<int64_t>(position * position) - 40;
    for (uint64_t length : { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 2 }, uint64_t { memory.size() } }) {
        int64_t expected = 0;
        for (uint64_t position = 0; position < length; ++position)
            expected += memory[position];
        double sumOfNumbers = -1;
        int64_t returned = test.call<int64_t>(memory.data(), length, &sumOfNumbers);
        checkOperationTest(returned == expected && sumOfNumbers == static_cast<double>(expected), test, "a loop of ", length, " turns gave ", returned, " and ", sumOfNumbers);
    }
}

static void testMachineOperationsFenceOopsFramePointerAndAddressOf()
{
    {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue pointer = test.argumentGPR(0);
        LValue word = test.argumentGPR(1);
        out.store64(word, out.address(test.memory(), pointer));
        out.fence();
        out.ret(out.load64(out.address(test.memory(), pointer)));
        compileOrFail(test);
        uint64_t stored = 0;
        checkOperationTest(test.call<uint64_t>(&stored, uint64_t { 99 }) == 99 && stored == 99, test, "Fence between a store and a load");
    }
    {
        OperationTestProcedure test;
        Output& out = test.out();
        LBasicBlock never = out.newBlock();
        LBasicBlock always = out.newBlock();
        out.branch(test.argumentGPR(0), rareEdge(never), normalEdge(always));
        out.appendTo(never);
        out.unreachable();
        out.appendTo(always);
        out.ret(out.constInt64(7));
        compileOrFail(test);
        checkOperationTest(test.call<uint64_t>(uint64_t { 0 }) == 7, test, "Oops in a block that is not entered");
    }
    {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue word = test.argumentGPR(0);
        LValue slot = out.lockedStackSlot(16);
        out.store64(word, out.address(test.heaps().frameSlots, slot));
        out.ret(out.sub(out.framePointer(), slot));
        compileOrFail(test);
        uint64_t distance = test.call<uint64_t>(uint64_t { 1 });
        checkOperationTest(distance >= 16 && distance <= 256 && !(distance % 8), test, "a slot lies ", distance, " bytes below the FramePointer");
    }
    {
        static constexpr uint64_t data[] = { 0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull };
        OperationTestProcedure test;
        Output& out = test.out();
        LValue index = test.argumentGPR(0);
        LValue table = out.relocatable(SymbolSpace::Constant, out.addConstantData(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), sizeof(data)), ConstantKind::Unlabeled));
        out.ret(out.load64(out.baseIndex(test.memory(), table, index, ScaleEight)));
        compileOrFail(test);
        for (uint64_t position = 0; position < std::size(data); ++position)
            checkOperationTest(test.call<uint64_t>(position) == data[position], test, "AddressOf constant data, word ", position);
    }
}

static void testMachineOperationsPatchExtractAndCheck()
{
    constexpr int32_t answerOfLatePath = 77;
    OperationTestProcedure test;
    Output& out = test.out();
    LValue argument = test.argumentGPR(0);

    PatchOperation* check = out.checkIsZero(out.equal(argument, 5));
    check->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        jit.move(CCallHelpers::TrustedImm32(answerOfLatePath), GPRInfo::returnValueGPR);
        if (parameters.frameIsSetUp())
            emitFrameTeardown(jit, parameters.frame());
        jit.ret();
    });

    PatchOperation* patch = out.patchpoint(test.procedure().addTuple({ Int64, Int64 }));
    patch->effects = Effects::none();
    patch->resultConstraints = { ValueRep::SomeEarlyRegister, ValueRep::SomeEarlyRegister };
    patch->appendSomeRegister(argument);
    patch->setRunsWithoutFrame(true);
    patch->setGenerator([](CCallHelpers& jit, const PatchParameters& parameters) {
        jit.add64(CCallHelpers::TrustedImm32(1), parameters[2].gpr(), parameters[0].gpr());
        jit.add64(CCallHelpers::TrustedImm32(2), parameters[2].gpr(), parameters[1].gpr());
    });
    out.ret(out.mul(out.extract(patch, 0), out.extract(patch, 1)));
    compileOrFail(test);

    for (int64_t value : { int64_t { 0 }, int64_t { 4 }, int64_t { 5 }, int64_t { 6 }, int64_t { -3 } }) {
        int64_t returned = test.call<int64_t>(value);
        checkOperationTest(returned == (value == 5 ? answerOfLatePath : (value + 1) * (value + 2)), test, "Check, Patch and Extract of ", value, " gave ", returned);
    }
}

static void testMachineOperationsOnLowHalfOfLoadThroughScaledIndex()
{
    OperationTestProcedure test;
    Output& out = test.out();
    LValue words = test.argumentGPR(0);
    LValue index = test.argumentGPR(1);
    out.ret(out.bitAnd(out.load64(out.baseIndex(test.memory(), words, index, ScaleEight)), out.constInt64(0xffffffff)));
    compileOrFail(test);
    uint64_t memory[2] = { 0x1111111122222222ull, 0x3333333344444444ull };
    checkOperationTest(test.call<uint64_t>(memory, uint64_t { 0 }) == 0x22222222, test, "the low half of word 0");
    checkOperationTest(test.call<uint64_t>(memory, uint64_t { 1 }) == 0x44444444, test, "the low half of word 1");
}

static void testMachineOperationsThatStoreSumThroughItsOwnBase()
{
    constexpr int32_t increment = 152;
    for (int32_t offsetOfStore : { 0, increment }) {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue base = test.argumentGPR(0);
        LValue sum = out.add(base, increment);
        out.store64(sum, out.address(test.memory(), base, offsetOfStore));
        out.ret(sum);
        compileOrFail(test);
        std::array<uint64_t, increment / sizeof(uint64_t) + 1> words { };
        uint64_t expected = std::bit_cast<uintptr_t>(words.data()) + increment;
        checkOperationTest(test.call<uint64_t>(words.data()) == expected && words[offsetOfStore / sizeof(uint64_t)] == expected, test, "a sum stored at offset ", offsetOfStore, " from its own base");
    }
}

static void testMachineOperationsThatBranchOnExtendedWords()
{
    for (Opcode extension : { ZExt32, SExt32 }) {
        for (bool isReadAgain : { false, true }) {
            OperationTestProcedure test;
            Output& out = test.out();
            LValue extended = out.unary(extension, out.castToInt32(test.argumentGPR(0)));
            LBasicBlock taken = out.newBlock();
            LBasicBlock notTaken = out.newBlock();
            out.branch(extended, normalEdge(taken), normalEdge(notTaken));
            out.appendTo(taken);
            out.ret(isReadAgain ? extended : out.constInt64(1));
            out.appendTo(notTaken);
            out.ret(out.constInt64(2));
            compileOrFail(test);
            for (uint64_t argument : { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 0x80000000 }, uint64_t { 0x700000000 }, uint64_t { 0xffffffff00000000 } }) {
                uint32_t low = static_cast<uint32_t>(argument);
                uint64_t whenTaken = !isReadAgain ? 1 : extension == ZExt32 ? low : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(low)));
                checkOperationTest(test.call<uint64_t>(argument) == (low ? whenTaken : 2), test, "Branch on ", extension, " of ", RawHex(argument));
            }
        }
    }
}

static void testMachineOperationsOnSumsTestedForInt32()
{
    enum class Right : uint8_t { Word, Constant, WideConstant };
    constexpr int64_t constant = 100;
    constexpr int64_t wideConstant = int64_t { 1 } << 31;
    constexpr uint64_t overflowMarker = uint64_t { 1 } << 40;
    for (Opcode opcode : { Add, Sub, Mul }) {
        for (Opcode comparison : { Equal, NotEqual }) {
            for (Right kindOfRight : { Right::Word, Right::Constant, Right::WideConstant }) {
                OperationTestProcedure test;
                Output& out = test.out();
                LValue leftBits = test.argumentGPR(0);
                LValue rightBits = test.argumentGPR(1);
                LValue left = out.signExt32To64(out.castToInt32(leftBits));
                LValue right = kindOfRight == Right::Word ? out.signExt32To64(out.castToInt32(rightBits)) : out.constInt64(kindOfRight == Right::Constant ? constant : wideConstant);
                LValue exact = out.binary(opcode, left, right);
                LValue low = out.castToInt32(exact);
                LBasicBlock fits = out.newBlock();
                LBasicBlock overflows = out.newBlock();
                LValue test32 = out.binary(comparison, out.signExt32To64(low), exact);
                if (comparison == Equal)
                    out.branch(test32, normalEdge(fits), rareEdge(overflows));
                else
                    out.branch(test32, rareEdge(overflows), normalEdge(fits));
                out.appendTo(fits);
                out.ret(out.zeroExtPtr(low));
                out.appendTo(overflows);
                out.ret(out.add(out.zeroExtPtr(low), out.constInt64(static_cast<int64_t>(overflowMarker))));
                compileOrFail(test);

                for (uint64_t a : operationTestInt32s) {
                    for (uint64_t b : operationTestInt32s) {
                        int64_t x = static_cast<int32_t>(a);
                        int64_t y = kindOfRight == Right::Word ? static_cast<int32_t>(b) : kindOfRight == Right::Constant ? constant : wideConstant;
                        int64_t sum = opcode == Add ? x + y : opcode == Sub ? x - y : x * y;
                        uint64_t expected = static_cast<uint32_t>(sum) + (sum == static_cast<int32_t>(sum) ? 0 : overflowMarker);
                        uint64_t returned = test.call<uint64_t>(operationTestWithDirtAbove(a, sizeof(uint32_t)), operationTestWithDirtAbove(b, sizeof(uint32_t)));
                        checkOperationTest(returned == expected, test, opcode, " of (", RawHex(a), ", ", RawHex(b), ") tested for int32 by ", comparison, " gave ", RawHex(returned));
                    }
                }
            }
        }
    }
}

static constexpr int64_t overflowTestMinInt32 = std::numeric_limits<int32_t>::min();
static constexpr int64_t overflowTestMaxInt32 = std::numeric_limits<int32_t>::max();
static constexpr int64_t overflowTestNoLeast = std::numeric_limits<int64_t>::min();
static constexpr int64_t overflowTestNoGreatest = std::numeric_limits<int64_t>::max();
static constexpr int64_t overflowTestMarkerOfFailingSide = int64_t { 1 } << 40;

enum class OverflowTestOperands : uint8_t {
    WordPlusWord,
    WordMinusWord,
    WordPlusConstant,
    ConstantMinusWord,
    // Each comes with proof that it is not negative.
    WordsThatAreNotNegative,
};

enum class OverflowTestFailingSide : uint8_t {
    ReadsNothing,
    ReadsWholeSum,
    ReadsLowHalf,
    ReadsLeft,
    ReadsRight,
};

struct OverflowTestRelation {
    ASCIILiteral name;
    LValue (*make)(Output&, LValue sum);
    bool passesWhereItHolds;
    bool addsBias;
    int64_t leastThatPasses;
    int64_t greatestThatPasses;
};

struct OverflowTestCase {
    OverflowTestOperands operands;
    int64_t constant;
    OverflowTestFailingSide failingSide;
    // Otherwise each is the low half of an argument, with dirt above it.
    bool wordsAreValuesOf32Bits;
};

static LValue overflowTestOwnLowHalfOf(Output& out, LValue sum)
{
    return out.signExt32To64(out.castToInt32(sum));
}

static LValue overflowTestDistanceFrom(Output& out, LValue sum, int64_t least)
{
    return out.sub(sum, out.constInt64(least));
}

static constexpr OverflowTestRelation overflowTestRelations[] = {
    { "sum == its own low half"_s, [](Output& out, LValue sum) { return out.equal(overflowTestOwnLowHalfOf(out, sum), sum); }, true, false, overflowTestMinInt32, overflowTestMaxInt32 },
    { "sum != its own low half"_s, [](Output& out, LValue sum) { return out.notEqual(overflowTestOwnLowHalfOf(out, sum), sum); }, false, false, overflowTestMinInt32, overflowTestMaxInt32 },
    { "sum <= max"_s, [](Output& out, LValue sum) { return out.lessThanOrEqual(sum, out.constInt64(overflowTestMaxInt32)); }, true, false, overflowTestNoLeast, overflowTestMaxInt32 },
    { "sum < max + 1"_s, [](Output& out, LValue sum) { return out.lessThan(sum, out.constInt64(overflowTestMaxInt32 + 1)); }, true, false, overflowTestNoLeast, overflowTestMaxInt32 },
    { "sum > max"_s, [](Output& out, LValue sum) { return out.greaterThan(sum, out.constInt64(overflowTestMaxInt32)); }, false, false, overflowTestNoLeast, overflowTestMaxInt32 },
    { "sum >= max + 1"_s, [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt64(overflowTestMaxInt32 + 1)); }, false, false, overflowTestNoLeast, overflowTestMaxInt32 },
    { "sum >= min"_s, [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt64(overflowTestMinInt32)); }, true, false, overflowTestMinInt32, overflowTestNoGreatest },
    { "sum > min - 1"_s, [](Output& out, LValue sum) { return out.greaterThan(sum, out.constInt64(overflowTestMinInt32 - 1)); }, true, false, overflowTestMinInt32, overflowTestNoGreatest },
    { "sum < min"_s, [](Output& out, LValue sum) { return out.lessThan(sum, out.constInt64(overflowTestMinInt32)); }, false, false, overflowTestMinInt32, overflowTestNoGreatest },
    { "sum <= min - 1"_s, [](Output& out, LValue sum) { return out.lessThanOrEqual(sum, out.constInt64(overflowTestMinInt32 - 1)); }, false, false, overflowTestMinInt32, overflowTestNoGreatest },
    { "sum - min <= max - min, without sign"_s, [](Output& out, LValue sum) { return out.belowOrEqual(overflowTestDistanceFrom(out, sum, overflowTestMinInt32), out.constInt64(overflowTestMaxInt32 - overflowTestMinInt32)); }, true, true, overflowTestMinInt32, overflowTestMaxInt32 },
    { "sum - min < max - min + 1, without sign"_s, [](Output& out, LValue sum) { return out.below(overflowTestDistanceFrom(out, sum, overflowTestMinInt32), out.constInt64(overflowTestMaxInt32 - overflowTestMinInt32 + 1)); }, true, true, overflowTestMinInt32, overflowTestMaxInt32 },
    { "sum - min > max - min, without sign"_s, [](Output& out, LValue sum) { return out.above(overflowTestDistanceFrom(out, sum, overflowTestMinInt32), out.constInt64(overflowTestMaxInt32 - overflowTestMinInt32)); }, false, true, overflowTestMinInt32, overflowTestMaxInt32 },
    { "sum - min >= max - min + 1, without sign"_s, [](Output& out, LValue sum) { return out.aboveOrEqual(overflowTestDistanceFrom(out, sum, overflowTestMinInt32), out.constInt64(overflowTestMaxInt32 - overflowTestMinInt32 + 1)); }, false, true, overflowTestMinInt32, overflowTestMaxInt32 },
    { "sum + 5 <= max + 5, without sign"_s, [](Output& out, LValue sum) { return out.belowOrEqual(overflowTestDistanceFrom(out, sum, -5), out.constInt64(overflowTestMaxInt32 + 5)); }, true, true, -5, overflowTestMaxInt32 },
    { "sum >= -5"_s, [](Output& out, LValue sum) { return out.greaterThanOrEqual(sum, out.constInt64(-5)); }, true, false, -5, overflowTestNoGreatest },
    { "sum <= 100"_s, [](Output& out, LValue sum) { return out.lessThanOrEqual(sum, out.constInt64(100)); }, true, false, overflowTestNoLeast, 100 },
};

static void checkSumTestedByOverflow(const OverflowTestRelation& relation, const OverflowTestCase& testCase)
{
    OperationTestProcedure test;
    Output& out = test.out();
    auto wordOfArgument = [&](unsigned argument) {
        LValue word = out.castToInt32(test.argumentGPR(argument));
        return testCase.wordsAreValuesOf32Bits ? out.opaque(word) : word;
    };
    LValue leftWord = wordOfArgument(0);
    LValue rightWord = wordOfArgument(1);
    LValue constant = out.constInt64(testCase.constant);

    LValue sum = nullptr;
    int64_t minSum = 0;
    int64_t maxSum = 0;
    switch (testCase.operands) {
    case OverflowTestOperands::WordPlusWord:
        sum = out.add(out.signExt32To64(leftWord), out.signExt32To64(rightWord));
        minSum = 2 * overflowTestMinInt32;
        maxSum = 2 * overflowTestMaxInt32;
        break;
    case OverflowTestOperands::WordMinusWord:
        sum = out.sub(out.signExt32To64(leftWord), out.signExt32To64(rightWord));
        minSum = overflowTestMinInt32 - overflowTestMaxInt32;
        maxSum = overflowTestMaxInt32 - overflowTestMinInt32;
        break;
    case OverflowTestOperands::WordPlusConstant:
        sum = out.add(out.signExt32To64(leftWord), constant);
        minSum = overflowTestMinInt32 + testCase.constant;
        maxSum = overflowTestMaxInt32 + testCase.constant;
        break;
    case OverflowTestOperands::ConstantMinusWord:
        sum = out.sub(constant, out.signExt32To64(rightWord));
        minSum = testCase.constant - overflowTestMaxInt32;
        maxSum = testCase.constant - overflowTestMinInt32;
        break;
    case OverflowTestOperands::WordsThatAreNotNegative:
        sum = out.add(out.extendWordThatIsNotNegative(leftWord), out.extendWordThatIsNotNegative(rightWord));
        minSum = 0;
        maxSum = 2 * overflowTestMaxInt32;
        break;
    }

    LBasicBlock tests = out.m_block;
    LBasicBlock passes = out.newBlock();
    LBasicBlock fails = out.newBlock();
    LValue predicate = relation.make(out, sum);
    if (relation.passesWhereItHolds)
        out.branch(predicate, normalEdge(passes), rareEdge(fails));
    else
        out.branch(predicate, rareEdge(fails), normalEdge(passes));
    const Operation* end = tests->last();

    out.appendTo(passes);
    out.ret(out.zeroExtPtr(out.castToInt32(sum)));

    out.appendTo(fails);
    LValue readOnFailingSide = out.constInt64(0);
    switch (testCase.failingSide) {
    case OverflowTestFailingSide::ReadsNothing:
        break;
    case OverflowTestFailingSide::ReadsWholeSum:
        // Otherwise the marker and a constant operand would be added up, and the sum of the marker and the other operand would read that operand.
        readOnFailingSide = out.opaque(sum);
        break;
    case OverflowTestFailingSide::ReadsLowHalf:
        readOnFailingSide = out.zeroExtPtr(out.castToInt32(sum));
        break;
    case OverflowTestFailingSide::ReadsLeft:
        readOnFailingSide = out.zeroExtPtr(leftWord);
        break;
    case OverflowTestFailingSide::ReadsRight:
        readOnFailingSide = out.zeroExtPtr(rightWord);
        break;
    }
    out.ret(out.add(readOnFailingSide, out.constInt64(overflowTestMarkerOfFailingSide)));
    compileOrFail(test);

    auto check = [&](bool condition, const auto&... details) {
        checkOperationTest(condition, test, relation.name, ", operands ", static_cast<unsigned>(testCase.operands), ", constant ", testCase.constant, ", failing side ", static_cast<unsigned>(testCase.failingSide),
            testCase.wordsAreValuesOf32Bits ? ", words of 32 bits: " : ", low halves: ", details...);
    };

    // The sums from `least` to `greatest`, among those from `lowest` to `highest` that there are, are the ones of which the operation of 32 bits does not overflow and gives no less than `least`.
    auto areToldByOverflow = [](int64_t least, int64_t greatest, int64_t lowest, int64_t highest) {
        bool upperBoundIsThatOfInt32 = greatest == overflowTestMaxInt32 || (greatest >= highest && highest <= overflowTestMaxInt32);
        bool lowerBoundLiesWithinInt32 = least >= overflowTestMinInt32 || lowest >= overflowTestMinInt32;
        return upperBoundIsThatOfInt32 && lowerBoundLiesWithinInt32 && least <= overflowTestMaxInt32;
    };
    int64_t least = relation.leastThatPasses;
    int64_t greatest = relation.greatestThatPasses;
    bool biasIsFoldedIntoOperand = relation.addsBias && testCase.operands == OverflowTestOperands::WordPlusConstant;
    // Where every sum passes or none does, the Branch is folded.
    bool someSumsPassAndSomeFail = (least > minSum || greatest < maxSum) && least <= maxSum && greatest >= minSum;
    bool passingSideIsTold = someSumsPassAndSomeFail && !biasIsFoldedIntoOperand && areToldByOverflow(least, greatest, minSum, maxSum);
    // A relation with one bound holds or fails for a row of sums either way, so the test may as well tell the sums that fail. Their side then finds the sum of 32 bits, so it reads no upper half.
    bool failingSideIsTold = false;
    if (someSumsPassAndSomeFail && !passingSideIsTold && !biasIsFoldedIntoOperand && testCase.failingSide != OverflowTestFailingSide::ReadsWholeSum) {
        if (least == overflowTestNoLeast)
            failingSideIsTold = areToldByOverflow(greatest + 1, overflowTestNoGreatest, minSum, maxSum);
        else if (greatest == overflowTestNoGreatest)
            failingSideIsTold = areToldByOverflow(overflowTestNoLeast, least - 1, minSum, maxSum);
    }
    bool goesByOverflow = passingSideIsTold || failingSideIsTold;
    // A bias and a constant operand are folded into one constant, and that sum is another one, which the test alone reads. It passes from 0 on.
    bool otherSumIsTold = false;
    if (biasIsFoldedIntoOperand) {
        int64_t foldedConstant = testCase.constant - least;
        bool isOperandOf32Bits = foldedConstant && foldedConstant >= overflowTestMinInt32 && foldedConstant <= overflowTestMaxInt32;
        otherSumIsTold = someSumsPassAndSomeFail && isOperandOf32Bits && areToldByOverflow(0, greatest - least, minSum - least, maxSum - least);
    }
    const Liveness& liveness = test.procedure().liveness();
    check(liveness.goesByOverflow(end) == (goesByOverflow || otherSumIsTold), "the Branch");
    check(liveness.goesByOverflow(sum) == goesByOverflow, "the sum");

    // The side of the sums that are not told alone reads the operand, so the sum takes its register and the way there takes the sum back.
    const Operation* operandTakenBack = nullptr;
    if ((passingSideIsTold || otherSumIsTold) && testCase.wordsAreValuesOf32Bits) {
        bool adds = testCase.operands == OverflowTestOperands::WordPlusWord || testCase.operands == OverflowTestOperands::WordsThatAreNotNegative;
        if (testCase.failingSide == OverflowTestFailingSide::ReadsLeft && testCase.operands != OverflowTestOperands::ConstantMinusWord)
            operandTakenBack = leftWord;
        if (testCase.failingSide == OverflowTestFailingSide::ReadsRight && adds)
            operandTakenBack = rightWord;
    }
    const Allocation& allocation = test.procedure().allocation();
    check(allocation.step(allocation.stepsOf(tests).terminator).other == operandTakenBack, "the operand that is taken back");

    for (uint64_t a : operationTestInt32s) {
        for (uint64_t b : operationTestInt32s) {
            int64_t x = static_cast<int32_t>(a);
            int64_t y = static_cast<int32_t>(b);
            int64_t exact = 0;
            switch (testCase.operands) {
            case OverflowTestOperands::WordPlusWord:
                exact = x + y;
                break;
            case OverflowTestOperands::WordMinusWord:
                exact = x - y;
                break;
            case OverflowTestOperands::WordPlusConstant:
                exact = x + testCase.constant;
                break;
            case OverflowTestOperands::ConstantMinusWord:
                exact = testCase.constant - y;
                break;
            case OverflowTestOperands::WordsThatAreNotNegative:
                exact = x + y;
                break;
            }
            if (testCase.operands == OverflowTestOperands::WordsThatAreNotNegative && (x < 0 || y < 0))
                continue;

            uint64_t expected = static_cast<uint32_t>(exact);
            if (exact < relation.leastThatPasses || exact > relation.greatestThatPasses) {
                expected = 0;
                switch (testCase.failingSide) {
                case OverflowTestFailingSide::ReadsNothing:
                    break;
                case OverflowTestFailingSide::ReadsWholeSum:
                    expected = static_cast<uint64_t>(exact);
                    break;
                case OverflowTestFailingSide::ReadsLowHalf:
                    expected = static_cast<uint32_t>(exact);
                    break;
                case OverflowTestFailingSide::ReadsLeft:
                    expected = static_cast<uint32_t>(a);
                    break;
                case OverflowTestFailingSide::ReadsRight:
                    expected = static_cast<uint32_t>(b);
                    break;
                }
                expected += static_cast<uint64_t>(overflowTestMarkerOfFailingSide);
            }
            uint64_t returned = test.call<uint64_t>(operationTestWithDirtAbove(a, sizeof(uint32_t)), operationTestWithDirtAbove(b, sizeof(uint32_t)));
            check(returned == expected, "(", x, ", ", y, ") gave ", RawHex(returned), " and not ", RawHex(expected));
        }
    }
}

static void testMachineOperationsOnSumsTestedByOverflow()
{
    struct Operands {
        OverflowTestOperands operands;
        int64_t constant;
    };
    // 0x1001 is no immediate of ARM64.
    static constexpr Operands allOperands[] = {
        { OverflowTestOperands::WordPlusWord, 0 },
        { OverflowTestOperands::WordMinusWord, 0 },
        { OverflowTestOperands::WordsThatAreNotNegative, 0 },
        { OverflowTestOperands::WordPlusConstant, 1 },
        { OverflowTestOperands::WordPlusConstant, 0x1001 },
        { OverflowTestOperands::WordPlusConstant, overflowTestMaxInt32 },
        { OverflowTestOperands::WordPlusConstant, -1 },
        { OverflowTestOperands::WordPlusConstant, -0x1001 },
        { OverflowTestOperands::WordPlusConstant, overflowTestMinInt32 },
        { OverflowTestOperands::ConstantMinusWord, 100 },
        { OverflowTestOperands::ConstantMinusWord, -100 },
        { OverflowTestOperands::ConstantMinusWord, overflowTestMaxInt32 },
        { OverflowTestOperands::ConstantMinusWord, overflowTestMinInt32 },
    };
    for (const OverflowTestRelation& relation : overflowTestRelations) {
        for (const Operands& operands : allOperands) {
            for (OverflowTestFailingSide failingSide : { OverflowTestFailingSide::ReadsNothing, OverflowTestFailingSide::ReadsWholeSum, OverflowTestFailingSide::ReadsLowHalf, OverflowTestFailingSide::ReadsLeft, OverflowTestFailingSide::ReadsRight }) {
                for (bool wordsAreValuesOf32Bits : { false, true })
                    checkSumTestedByOverflow(relation, { operands.operands, operands.constant, failingSide, wordsAreValuesOf32Bits });
            }
        }
    }
}

// total += step for as many turns as are asked for, or until the sum is no int32. Whoever is sent the failing way finds the total and the number of the turn as they were in front of the addition.
static void testMachineOperationsOnSumThatGoesRoundLoop()
{
    constexpr uint64_t markerOfFailingSide = uint64_t { 1 } << 62;
    struct Run {
        int32_t start;
        int32_t step;
        uint32_t turns;
    };
    static constexpr Run runs[] = {
        { 0, 1, 0 }, { 0, 1, 10 }, { 5, 0, 3 }, { INT32_MAX, 0, 3 }, { INT32_MAX - 1, 1, 1 }, { INT32_MAX - 1, 1, 2 }, { INT32_MAX - 5, 1, 10 }, { 5, INT32_MAX, 2 }, { 0, INT32_MAX, 1 }, { INT32_MAX, INT32_MAX, 1 },
        { INT32_MIN, 0, 2 }, { INT32_MIN + 1, -1, 1 }, { INT32_MIN + 1, -1, 2 }, { INT32_MIN + 3, -1, 10 }, { -5, INT32_MIN, 1 }, { 0, INT32_MIN, 1 }, { 0, INT32_MIN, 2 }, { -1, 1, 3 }, { INT32_MIN, INT32_MAX, 3 },
    };

    for (bool wordsAreNotNegative : { false, true }) {
        OperationTestProcedure test;
        Output& out = test.out();
        LValue start = out.castToInt32(test.argumentGPR(0));
        LValue step = out.opaque(out.castToInt32(test.argumentGPR(1)));
        LValue turns = out.opaque(out.castToInt32(test.argumentGPR(2)));
        LBasicBlock head = out.newBlock();
        LBasicBlock body = out.newBlock();
        LBasicBlock next = out.newBlock();
        LBasicBlock leaves = out.newBlock();
        LBasicBlock end = out.newBlock();
        ValueFromBlock firstTotal = out.anchor(start);
        ValueFromBlock firstTurn = out.anchor(out.int32Zero);
        out.jump(head);

        out.appendTo(head);
        LValue total = out.phi(Int32, firstTotal);
        LValue turn = out.phi(Int32, firstTurn);
        out.branch(out.below(turn, turns), normalEdge(body), normalEdge(end));

        out.appendTo(body);
        auto widen = [&](LValue word) {
            return wordsAreNotNegative ? out.extendWordThatIsNotNegative(word) : out.signExt32To64(word);
        };
        LValue sum = out.add(widen(total), widen(step));
        LValue fits = wordsAreNotNegative ? out.lessThanOrEqual(sum, out.constInt64(overflowTestMaxInt32)) : out.equal(overflowTestOwnLowHalfOf(out, sum), sum);
        out.branch(fits, normalEdge(next), rareEdge(leaves));

        out.appendTo(next);
        out.addIncomingToPhi(total, out.anchor(out.castToInt32(sum)));
        out.addIncomingToPhi(turn, out.anchor(out.add(turn, 1)));
        out.jump(head);

        out.appendTo(leaves);
        LValue found = out.bitOr(out.zeroExtPtr(total), out.shl(out.zeroExtPtr(turn), out.constInt32(32)));
        out.ret(out.add(found, out.constInt64(static_cast<int64_t>(markerOfFailingSide))));

        out.appendTo(end);
        out.ret(out.zeroExtPtr(total));
        compileOrFail(test);

        const Liveness& liveness = test.procedure().liveness();
        const Allocation& allocation = test.procedure().allocation();
        checkOperationTest(liveness.goesByOverflow(body->last()) && liveness.goesByOverflow(sum), test, "the sum that goes round goes by overflow");
        // So a turn adds to the register of the Phi, and no move stands on the edge back.
        checkOperationTest(allocation.step(allocation.stepsOf(body).terminator).other == total, test, "the total is taken back on the failing way");
        const Step* stepOfSum = allocation.findExecuteOf(sum);
        checkOperationTest(stepOfSum && stepOfSum->destination == Location::reg(allocation.registerAtDefinitionOf(total)), test, "the sum lies in the register of the Phi");

        for (const Run& run : runs) {
            if (wordsAreNotNegative && (run.start < 0 || run.step < 0))
                continue;
            int64_t expectedTotal = run.start;
            uint32_t expectedTurn = 0;
            bool leavesLoop = false;
            for (; expectedTurn < run.turns; ++expectedTurn) {
                int64_t exact = expectedTotal + run.step;
                if (exact < overflowTestMinInt32 || exact > overflowTestMaxInt32) {
                    leavesLoop = true;
                    break;
                }
                expectedTotal = exact;
            }
            uint64_t expected = static_cast<uint32_t>(expectedTotal);
            if (leavesLoop)
                expected = (expected | static_cast<uint64_t>(expectedTurn) << 32) + markerOfFailingSide;
            uint64_t returned = test.call<uint64_t>(operationTestWithDirtAbove(static_cast<uint32_t>(run.start), sizeof(uint32_t)), operationTestWithDirtAbove(static_cast<uint32_t>(run.step), sizeof(uint32_t)), static_cast<uint64_t>(run.turns));
            checkOperationTest(returned == expected, test, "from ", run.start, " by ", run.step, ", ", run.turns, " turns gave ", RawHex(returned), " and not ", RawHex(expected));
        }
    }
}

// The sum lives across a call on the passing side and finds no register that callees save, so it is stored behind its definition, with its upper half clear.
// The failing side calls too, and reads the whole sum from that place afterward.
static void testMachineOperationsOnSpilledSumThatIsWidened()
{
    // More than any target keeps in registers across a call.
    constexpr unsigned numberOfValues = 10;
    constexpr uint64_t seedOfRun = 0x1234;

    OperationTestProcedure test;
    Output& out = test.out();
    LValue left = out.opaque(out.castToInt32(test.argumentGPR(0)));
    LValue right = out.opaque(out.castToInt32(test.argumentGPR(1)));
    LValue seed = test.argumentGPR(2);
    Vector<Operation*> values = defineValues(test, seed, numberOfValues, Int64);
    LValue sum = out.add(out.signExt32To64(left), out.signExt32To64(right));
    LBasicBlock tests = out.m_block;
    LBasicBlock passes = out.newBlock();
    LBasicBlock fails = out.newBlock();
    out.branch(out.equal(overflowTestOwnLowHalfOf(out, sum), sum), normalEdge(passes), rareEdge(fails));

    auto callWithSeed = [&] {
        ConstrainedValue operands[] = { ConstrainedValue(seed, ValueRep::SomeRegister) };
        TestPatch call;
        call.kind = TestPatchKind::SumThroughCall;
        call.operands = operands;
        return appendTestPatch(test, call);
    };
    out.appendTo(passes);
    LValue calledOnPassingSide = callWithSeed();
    out.ret(out.add(out.add(calledOnPassingSide, sumOf(test, values.span())), out.zeroExtPtr(out.castToInt32(sum))));
    out.appendTo(fails);
    LValue calledOnFailingSide = callWithSeed();
    out.ret(out.add(out.add(calledOnFailingSide, sum), out.constInt64(overflowTestMarkerOfFailingSide)));
    compileOrFail(test);

    checkOperationTest(test.procedure().liveness().goesByOverflow(tests->last()), test, "the Branch goes by overflow");
    checkOperationTest(sumTestedByOverflowOf(test.procedure().liveness(), sum).isWidenedWhereTestFails, test, "the sum is widened");
    checkOperationTest(test.procedure().allocation().isStoredBehindDefinition(sum), test, "the sum is stored behind its definition");

    uint64_t returnedForZero = test.call<uint64_t>(uint64_t { 0 }, uint64_t { 0 }, seedOfRun);
    for (uint64_t a : operationTestInt32s) {
        for (uint64_t b : operationTestInt32s) {
            int64_t exact = static_cast<int64_t>(static_cast<int32_t>(a)) + static_cast<int32_t>(b);
            bool fits = exact >= overflowTestMinInt32 && exact <= overflowTestMaxInt32;
            uint64_t expected = fits ? returnedForZero + static_cast<uint32_t>(exact) : seedOfRun + static_cast<uint64_t>(exact) + static_cast<uint64_t>(overflowTestMarkerOfFailingSide);
            uint64_t returned = test.call<uint64_t>(a, b, seedOfRun);
            checkOperationTest(returned == expected, test, "(", RawHex(a), ", ", RawHex(b), ") gave ", RawHex(returned), " and not ", RawHex(expected));
        }
    }
}

// The failing side is as likely as not to be taken and rejoins: `a + b` of two boxed int32, whose sum is made a number where it is no int32.
static void testMachineOperationsOnSumThatIsMadeNumberWhereItOverflows()
{
    for (Opcode opcode : { Add, Sub }) {
        for (EdgeClass classOfFailingEdge : { EdgeClass::Normal, EdgeClass::Unlikely, EdgeClass::Rare }) {
            OperationTestProcedure test;
            Output& out = test.out();
            LValue left = out.signExt32To64(out.castToInt32(test.argumentGPR(0)));
            LValue right = out.signExt32To64(out.castToInt32(test.argumentGPR(1)));
            LValue sum = out.binary(opcode, left, right);
            LValue low = out.castToInt32(sum);
            LBasicBlock tests = out.m_block;
            LBasicBlock fits = out.newBlock();
            LBasicBlock overflows = out.newBlock();
            LBasicBlock join = out.newBlock();
            out.branch(out.equal(out.signExt32To64(low), sum), normalEdge(fits), Edge(overflows, classOfFailingEdge));

            out.appendTo(fits);
            ValueFromBlock whenItFits = out.anchor(out.intToDouble(low));
            out.jump(join);
            out.appendTo(overflows);
            ValueFromBlock whenItOverflows = out.anchor(out.intToDouble(sum));
            out.jump(join);
            out.appendTo(join);
            out.ret(out.phi(Double, whenItFits, whenItOverflows));
            compileOrFail(test);

            checkOperationTest(test.procedure().liveness().goesByOverflow(tests->last()), test, opcode, " whose failing side rejoins goes by overflow");
            for (uint64_t a : operationTestInt32s) {
                for (uint64_t b : operationTestInt32s) {
                    int64_t x = static_cast<int32_t>(a);
                    int64_t y = static_cast<int32_t>(b);
                    double expected = static_cast<double>(opcode == Add ? x + y : x - y);
                    double returned = test.call<double>(operationTestWithDirtAbove(a, sizeof(uint32_t)), operationTestWithDirtAbove(b, sizeof(uint32_t)));
                    checkOperationTest(returned == expected, test, opcode, " of (", x, ", ", y, ") as a number gave ", returned);
                }
            }
        }
    }
}

static std::span<const NativeTest> listOfMachineOperationTests()
{
    static constexpr NativeTest tests[] = {
        { "machine/operations/arguments-and-returns"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnArgumentsAndReturns> },
        { "machine/operations/constants"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatAreConstants> },
        { "machine/operations/integers/arithmetic"_s, runOperationTestWhereCodeRuns<testMachineOperationsOfIntegerArithmetic> },
        { "machine/operations/integers/divisions"_s, runOperationTestWhereCodeRuns<testMachineOperationsOfDivision> },
        { "machine/operations/integers/high-halves-of-products"_s, runOperationTestWhereCodeRuns<testMachineOperationsOfHighHalvesOfProducts> },
        { "machine/operations/integers/shifts"_s, runOperationTestWhereCodeRuns<testMachineOperationsOfShifting> },
        { "machine/operations/integers/unary"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnOneInteger> },
        { "machine/operations/conversions"_s, runOperationTestWhereCodeRuns<testMachineOperationsOfConversion> },
        { "machine/operations/opaque"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatAreOpaque> },
        { "machine/operations/numbers/arithmetic"_s, runOperationTestWhereCodeRuns<testMachineOperationsOfNumberArithmetic> },
        { "machine/operations/numbers/unary"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnOneNumber> },
        { "machine/operations/comparisons/int32"_s, runOperationTestWhereCodeRuns<checkComparisonsOf<int32_t>> },
        { "machine/operations/comparisons/int64"_s, runOperationTestWhereCodeRuns<checkComparisonsOf<int64_t>> },
        { "machine/operations/comparisons/float"_s, runOperationTestWhereCodeRuns<checkComparisonsOf<float>> },
        { "machine/operations/comparisons/double"_s, runOperationTestWhereCodeRuns<checkComparisonsOf<double>> },
        { "machine/operations/comparisons/bit-tests"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatTestBits> },
        { "machine/operations/select/on-int32"_s, runOperationTestWhereCodeRuns<checkSelectOnValuesOfEveryType<int32_t>> },
        { "machine/operations/select/on-int64"_s, runOperationTestWhereCodeRuns<checkSelectOnValuesOfEveryType<int64_t>> },
        { "machine/operations/select/between-compared-operands"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatSelectBetweenComparedOperands> },
        { "machine/operations/switch"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatSwitch> },
        { "machine/operations/memory/offsets"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnMemoryAtOffsets> },
        { "machine/operations/memory/indexes"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnMemoryThroughIndexes> },
        { "machine/operations/memory/stores-of-constants"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatStoreConstants> },
        { "machine/operations/memory/stores-in-a-row"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatStoreInARow> },
        { "machine/operations/memory/narrow-loads-as-operands"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnNarrowLoads> },
        { "machine/operations/memory/stack-slots"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnStackSlots> },
        { "machine/operations/loop-with-phis-in-both-banks"_s, runOperationTestWhereCodeRuns<testMachineOperationsInLoopWithPhisInBothBanks> },
        { "machine/operations/fence-oops-frame-pointer-address-of"_s, runOperationTestWhereCodeRuns<testMachineOperationsFenceOopsFramePointerAndAddressOf> },
        { "machine/operations/patch-extract-check"_s, runOperationTestWhereCodeRuns<testMachineOperationsPatchExtractAndCheck> },
        { "machine/operations/low-half-of-load-through-scaled-index"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnLowHalfOfLoadThroughScaledIndex> },
        { "machine/operations/store-of-sum-through-its-own-base"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatStoreSumThroughItsOwnBase> },
        { "machine/operations/branches-on-extended-words"_s, runOperationTestWhereCodeRuns<testMachineOperationsThatBranchOnExtendedWords> },
        { "machine/operations/sums-tested-for-int32"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnSumsTestedForInt32> },
        { "machine/operations/sums-tested-by-overflow/relations-and-operands"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnSumsTestedByOverflow> },
        { "machine/operations/sums-tested-by-overflow/sum-that-goes-round-a-loop"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnSumThatGoesRoundLoop> },
        { "machine/operations/sums-tested-by-overflow/spilled-sum-is-widened"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnSpilledSumThatIsWidened> },
        { "machine/operations/sums-tested-by-overflow/sum-is-made-a-number-where-it-overflows"_s, runOperationTestWhereCodeRuns<testMachineOperationsOnSumThatIsMadeNumberWhereItOverflows> },
    };
    return tests;
}

} // namespace Machine

std::span<const NativeTest> machineOperationTests()
{
    return Machine::listOfMachineOperationTests();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
