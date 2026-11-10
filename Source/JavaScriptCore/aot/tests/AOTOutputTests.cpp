/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineOrder.h"
#include "AOTMachineTarget.h"
#include "AOTMachineTestSupport.h"
#include "AOTNativeTest.h"
#include "AOTOperand.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRegisters.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

struct OutputTestEmitter {
    WTF_MAKE_NONCOPYABLE(OutputTestEmitter);

    explicit OutputTestEmitter(ProcedureTraits traits = { })
        : test(traits)
        , out(test.out())
        , heaps(test.heaps())
        , numberTag(out.constInt64(JSValue::NumberTag))
        , notCellMask(out.constInt64(JSValue::NotCellMask))
    {
    }

    LValue argument(unsigned index) { return test.argumentGPR(index); }
    LValue numberArgument(unsigned index) { return test.argumentFPR(index); }

    template<typename Usual, typename Rare>
    void ifElse(Typed::Condition condition, const Usual& usual, const Rare& rare)
    {
        LBasicBlock yes = out.newBlock();
        LBasicBlock no = out.newBlock();
        LBasicBlock join = out.newBlock();
        ScopeFork fork = out.forkScope();
        out.branch(condition.value(), normalEdge(yes), rareEdge(no));
        out.appendToArm(yes, fork);
        usual();
        out.jump(join);
        out.appendToArm(no, fork);
        rare();
        out.jump(join);
        out.appendToJoin(join, fork);
    }

    template<typename T, typename Usual, typename Rare>
    T either(Typed::Condition condition, const Usual& usual, const Rare& rare)
    {
        LBasicBlock yes = out.newBlock();
        LBasicBlock no = out.newBlock();
        Typed::Join<T> join(out);
        ScopeFork fork = out.forkScope();
        out.branch(condition.value(), normalEdge(yes), rareEdge(no));
        out.appendToArm(yes, fork);
        join.add(usual());
        out.appendToArm(no, fork);
        join.add(rare());
        return join.finishBehind(fork);
    }

    void branchUnless(Typed::Condition condition, LBasicBlock rareTarget)
    {
        LBasicBlock continuation = out.newBlock();
        out.branch(condition.value(), normalEdge(continuation), rareEdge(rareTarget));
        out.appendToContinuation(continuation);
    }

    Typed::Condition isInt32(Typed::Boxed value) { return Typed::Condition(out.aboveOrEqual(value.value(), numberTag)); }
    Typed::Condition isNumber(Typed::Boxed value) { return Typed::Condition(out.testNonZero64(value.value(), numberTag)); }
    Typed::Condition isCell(Typed::Boxed value) { return Typed::Condition(out.testIsZero64(value.value(), notCellMask)); }
    Typed::Boxed boxInt32(Typed::Word32 value) { return Typed::Boxed(out.add(out.zeroExt(value.value(), Int64), numberTag)); }
    Typed::Word32 unboxInt32(Typed::Boxed value) { return Typed::Word32(out.castToInt32(value.value())); }
    Typed::Boxed boxDouble(Typed::Number value) { return Typed::Boxed(out.sub(out.bitCast(value.value(), Int64), numberTag)); }
    Typed::Number unboxDouble(Typed::Boxed value) { return Typed::Number(out.bitCast(out.add(value.value(), numberTag), Double)); }

    Typed::Number numberToDouble(Typed::Boxed value)
    {
        Typed::Condition holdsInt32 = isInt32(value);
        Typed::Number fromInt32(out.intToDouble(unboxInt32(value).value()));
        return Typed::Number(out.select(holdsInt32.value(), fromInt32.value(), unboxDouble(value).value()));
    }

    Typed::Number purified(Typed::Number value)
    {
        return either<Typed::Number>(Typed::Condition(out.doubleEqual(value.value(), value.value())), [&] {
            return value;
        }, [&] {
            return Typed::Number(out.constDouble(PNaN));
        });
    }

    void compile()
    {
        Procedure& procedure = test.procedure();
        for (uint32_t index = 0; index < procedure.numberOfOperations(); ++index) {
            const Operation* operation = procedure.operationAt(index);
            if (!operation->isConstant())
                continue;
            uint64_t bits = static_cast<uint64_t>(operation->constantBits());
            std::optional<PinnedConstant> pinned = findPinnedConstant(bits);
            if (!pinned || !pinned->offset)
                continue;
            int32_t offset = *pinned->offset;
            AOT_TEST_CHECK(pinned->reg == instanceGPR);
            AOT_TEST_CHECK(offset >= -static_cast<int32_t>(sizeof(instance.wordsBelow)));
            AOT_TEST_CHECK(offset + static_cast<int32_t>(sizeof(bits)) <= static_cast<int32_t>(sizeof(instance.words)));
            std::memcpy(static_cast<uint8_t*>(instance.pointer()) + offset, &bits, sizeof(bits));
        }
        compileOrFail(test, instance.pointer());
    }

    template<typename Result, typename... Arguments>
    Result call(Arguments... arguments) { return test.call<Result>(arguments...); }

    TestProcedure test;
    Output& out;
    const AOTHeaps& heaps;
    TestInstance instance;
    LValue numberTag;
    LValue notCellMask;
};

static EdgeClass classOfEdgeInOutputTest(LBasicBlock from, LBasicBlock to)
{
    for (const Edge& edge : from->successors()) {
        if (edge.block() == to)
            return edge.edgeClass();
    }
    failNativeTest(__FILE__, __LINE__, "an edge leads from the one block to the other");
}

template<typename T>
static void checkOutputWrapperNamesItsValue(LValue value)
{
    T wrapped(value);
    Typed::Operand operand = wrapped;
    AOT_TEST_CHECK(wrapped && !T() && wrapped.value() == value);
    AOT_TEST_CHECK(value->type() == Typed::machineTypeOf(T::representation));
    AOT_TEST_CHECK(operand.representation == T::representation && operand.value == value);
    AOT_TEST_CHECK(operand.is<T>() && operand.as<T>().value() == value);
}

template<typename T, typename Raw>
static void checkOutputWrapperRoundTrips(std::initializer_list<Raw> values)
{
    constexpr bool isNumber = std::is_same_v<Raw, double>;
    using Passed = std::conditional_t<isNumber, double, int64_t>;
    using Bits = std::conditional_t<sizeof(Raw) == sizeof(uint32_t), uint32_t, uint64_t>;

    OutputTestEmitter emitter;
    Output& out = emitter.out;
    auto wrappedArgument = [&](unsigned index) {
        LValue word = isNumber ? emitter.numberArgument(index) : emitter.argument(index);
        return T(Typed::machineTypeOf(T::representation) == Int32 ? out.castToInt32(word) : word);
    };
    T first = wrappedArgument(0);
    T second = wrappedArgument(1);
    LValue chooser = isNumber ? emitter.numberArgument(2) : emitter.argument(2);
    Typed::Condition takesFirst(isNumber ? out.doubleGreaterThan(chooser, out.constDouble(0)) : out.notZero64(chooser));
    checkOutputWrapperNamesItsValue<T>(first.value());

    T joined = emitter.either<T>(takesFirst, [&] {
        return first;
    }, [&] {
        return second;
    });
    AOT_TEST_CHECK(joined.value()->opcode() == Phi && joined.value()->owner() == out.m_block);
    checkOutputWrapperNamesItsValue<T>(joined.value());
    out.ret(joined.value());
    emitter.compile();

    for (Raw a : values) {
        for (Raw b : values) {
            Raw whenFirst = emitter.call<Raw>(static_cast<Passed>(a), static_cast<Passed>(b), Passed { 1 });
            Raw whenSecond = emitter.call<Raw>(static_cast<Passed>(a), static_cast<Passed>(b), Passed { 0 });
            AOT_TEST_CHECK(std::bit_cast<Bits>(whenFirst) == std::bit_cast<Bits>(a));
            AOT_TEST_CHECK(std::bit_cast<Bits>(whenSecond) == std::bit_cast<Bits>(b));
        }
    }
}

static void testOutputWrappersRoundTripTheirValues()
{
    alignas(16) uint64_t firstCell[2] { };
    alignas(16) uint64_t secondCell[2] { };
    checkOutputWrapperRoundTrips<Typed::Word32, int32_t>({ 0, 1, -1, INT32_MAX, INT32_MIN });
    checkOutputWrapperRoundTrips<Typed::Integer, int64_t>({ 0, -1, int64_t { 1 } << 32, INT64_MAX, INT64_MIN });
    checkOutputWrapperRoundTrips<Typed::Number, double>({ 0.0, -0.0, 1.5, std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::infinity(), PNaN });
    checkOutputWrapperRoundTrips<Typed::Boxed, EncodedJSValue>({ JSValue::encode(jsNumber(-7)), JSValue::encode(jsDoubleNumber(0.5)), JSValue::encode(jsUndefined()), JSValue::encode(JSValue()) });
    checkOutputWrapperRoundTrips<Typed::CellPointer, uint64_t>({ std::bit_cast<uint64_t>(&firstCell), std::bit_cast<uint64_t>(&secondCell) });
}

static void testOutputNarrowReferencesAreFlagged()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue word = out.castToInt32(emitter.argument(0));
    LValue otherWord = out.castToInt32(emitter.argument(1));

    Typed::Word32 plain(word);
    AOT_TEST_CHECK(!plain.value()->isNarrowReference());
    Typed::NarrowReference reference(word);
    AOT_TEST_CHECK(reference.value() == word && word->isNarrowReference());
    Typed::Operand operand(Typed::Representation::Cell32, otherWord);
    AOT_TEST_CHECK(operand.is<Typed::NarrowReference>() && otherWord->isNarrowReference());
    Typed::NarrowReference marker(out.constInt32(undefinedCell32));
    AOT_TEST_CHECK(!marker.value()->isNarrowReference());
}

static void testOutputBoxesAndUnboxesInt32()
{
    OutputTestEmitter boxer;
    boxer.out.ret(boxer.boxInt32(Typed::Word32(boxer.out.castToInt32(boxer.argument(0)))).value());
    boxer.compile();

    OutputTestEmitter unboxer;
    unboxer.out.ret(unboxer.out.signExt32To64(unboxer.unboxInt32(Typed::Boxed(unboxer.argument(0))).value()));
    unboxer.compile();

    for (int32_t integer : { 0, 1, -1, 42, INT32_MAX, INT32_MIN }) {
        uint64_t wordWithUpperHalf = static_cast<uint32_t>(integer) | 0x5a5a5a5aull << 32;
        EncodedJSValue boxed = boxer.call<EncodedJSValue>(wordWithUpperHalf);
        AOT_TEST_CHECK(boxed == JSValue::encode(jsNumber(integer)));
        AOT_TEST_CHECK(unboxer.call<int64_t>(boxed) == integer);
    }
}

static void testOutputBoxesAndUnboxesDoubles()
{
    OutputTestEmitter boxer;
    boxer.out.ret(boxer.boxDouble(Typed::Number(boxer.numberArgument(0))).value());
    boxer.compile();

    OutputTestEmitter unboxer;
    unboxer.out.ret(unboxer.unboxDouble(Typed::Boxed(unboxer.argument(0))).value());
    unboxer.compile();

    OutputTestEmitter converter;
    converter.out.ret(converter.numberToDouble(Typed::Boxed(converter.argument(0))).value());
    converter.compile();

    constexpr double infinity = std::numeric_limits<double>::infinity();
    const double numbers[] = { 0.0, -0.0, 1.0, -1.5, 4294967296.0, 1e300, std::numeric_limits<double>::denorm_min(), infinity, -infinity, PNaN };
    for (double number : numbers) {
        EncodedJSValue boxed = boxer.call<EncodedJSValue>(number);
        AOT_TEST_CHECK(boxed == JSValue::encode(jsDoubleNumber(number)));
        AOT_TEST_CHECK(std::bit_cast<uint64_t>(unboxer.call<double>(boxed)) == std::bit_cast<uint64_t>(number));
        AOT_TEST_CHECK(std::bit_cast<uint64_t>(converter.call<double>(boxed)) == std::bit_cast<uint64_t>(number));
    }
    for (int32_t integer : { 0, -1, 7, INT32_MAX, INT32_MIN })
        AOT_TEST_CHECK(converter.call<double>(JSValue::encode(jsNumber(integer))) == integer);
}

static void testOutputTellsBoxedValuesApartByTheTags()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    Typed::Boxed value(emitter.argument(0));
    LValue answers = out.bitOr(emitter.isInt32(value).value(), out.bitOr(out.shl(emitter.isNumber(value).value(), 1), out.shl(emitter.isCell(value).value(), 2)));
    out.ret(out.zeroExtPtr(answers));
    emitter.compile();

    alignas(16) uint64_t cell[2] { };
    const EncodedJSValue words[] = {
        JSValue::encode(jsNumber(0)), JSValue::encode(jsNumber(-1)), JSValue::encode(jsNumber(INT32_MAX)), JSValue::encode(jsNumber(INT32_MIN)),
        JSValue::encode(jsDoubleNumber(0.5)), JSValue::encode(jsDoubleNumber(-0.0)), JSValue::encode(jsDoubleNumber(PNaN)), JSValue::encode(jsDoubleNumber(-std::numeric_limits<double>::infinity())),
        JSValue::encode(jsUndefined()), JSValue::encode(jsNull()), JSValue::encode(jsBoolean(true)), JSValue::encode(jsBoolean(false)), JSValue::encode(JSValue()),
        std::bit_cast<EncodedJSValue>(&cell),
    };
    for (EncodedJSValue word : words) {
        JSValue decoded = JSValue::decode(word);
        int64_t expected = static_cast<int64_t>(decoded.isInt32()) | static_cast<int64_t>(decoded.isNumber()) << 1 | static_cast<int64_t>(decoded.isCell()) << 2;
        AOT_TEST_CHECK(emitter.call<int64_t>(word) == expected);
    }
}

enum class OutputTestRawNumber : uint8_t { Double, Float };

static void compileReadOfRawNumberInOutputTest(OutputTestEmitter& emitter, OutputTestRawNumber width, bool purifies)
{
    Output& out = emitter.out;
    TypedPointer bytes = out.address(emitter.heaps.TypedArrayProperties, emitter.argument(0));
    Typed::Number read(width == OutputTestRawNumber::Double ? out.loadDouble(bytes) : out.floatToDouble(out.loadFloat(bytes)));
    AOT_TEST_CHECK(!out.doubleEqual(read.value(), read.value())->hasInt());
    if (purifies)
        read = emitter.purified(read);
    out.ret(emitter.boxDouble(read).value());
    emitter.compile();
}

static void testOutputPurifiesNumbersReadFromRawBytes()
{
    constexpr uint64_t bitsThatBoxToNoNumber = 0xfffe000000000005ull;
    const EncodedJSValue boxedNaN = JSValue::encode(jsDoubleNumber(PNaN));

    OutputTestEmitter doubles;
    compileReadOfRawNumberInOutputTest(doubles, OutputTestRawNumber::Double, true);
    const uint64_t bitsOfNaNs[] = { bitsThatBoxToNoNumber, UINT64_MAX, ImpureNaNAsBits, 0x7ff0000000000001ull, 0xfff8000000000000ull, undefinedDouble, nullDouble, absentDouble };
    for (uint64_t bits : bitsOfNaNs)
        AOT_TEST_CHECK(doubles.call<EncodedJSValue>(&bits) == boxedNaN);
    const double numbers[] = { 0.0, -0.0, 1.5, -1e300, std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
    for (double number : numbers)
        AOT_TEST_CHECK(doubles.call<EncodedJSValue>(&number) == JSValue::encode(jsDoubleNumber(number)));

    OutputTestEmitter floats;
    compileReadOfRawNumberInOutputTest(floats, OutputTestRawNumber::Float, true);
    const uint32_t bitsOfFloatNaNs[] = { 0xffc00001u, 0x7f800001u, 0xffffffffu };
    for (uint32_t bits : bitsOfFloatNaNs)
        AOT_TEST_CHECK(floats.call<EncodedJSValue>(&bits) == boxedNaN);
    float number = -2.5f;
    AOT_TEST_CHECK(floats.call<EncodedJSValue>(&number) == JSValue::encode(jsDoubleNumber(-2.5)));

    OutputTestEmitter withoutPurification;
    compileReadOfRawNumberInOutputTest(withoutPurification, OutputTestRawNumber::Double, false);
    uint64_t bits = bitsThatBoxToNoNumber;
    AOT_TEST_CHECK(!JSValue::decode(withoutPurification.call<EncodedJSValue>(&bits)).isNumber());
}

static void testOutputIfElseRestoresTheScopeOfTheBranch()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    const IndexedAbstractHeap& elements = emitter.heaps.indexedContiguousProperties;
    LValue base = emitter.argument(0);
    LValue x = emitter.argument(1);

    LValue sum = out.add(x, 3);
    LValue untouched = out.load64(base, elements[0]);
    LValue writtenInUsualArm = out.load64(base, elements[1]);
    LValue writtenInRareArm = out.load64(base, elements[2]);
    LValue square = nullptr;
    emitter.ifElse(Typed::Condition(out.notZero64(x)), [&] {
        AOT_TEST_CHECK(out.m_block->hasOnlyOnePredecessor());
        AOT_TEST_CHECK(out.add(x, 3) == sum && out.load64(base, elements[1]) == writtenInUsualArm);
        square = out.mul(x, x);
        out.store64(square, base, elements[1]);
        AOT_TEST_CHECK(out.load64(base, elements[1]) != writtenInUsualArm);
    }, [&] {
        AOT_TEST_CHECK(out.m_block->hasOnlyOnePredecessor());
        AOT_TEST_CHECK(out.add(x, 3) == sum && out.load64(base, elements[2]) == writtenInRareArm);
        AOT_TEST_CHECK(out.mul(x, x) != square);
        out.store64(sum, base, elements[2]);
    });

    AOT_TEST_CHECK(out.m_block->numberOfIncomingEdges() == 2 && !out.m_block->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(out.add(x, 3) == sum && out.mul(x, x) != square);
    AOT_TEST_CHECK(out.load64(base, elements[0]) == untouched);
    LValue second = out.load64(base, elements[1]);
    LValue third = out.load64(base, elements[2]);
    AOT_TEST_CHECK(second != writtenInUsualArm && third != writtenInRareArm);
    out.ret(out.add(untouched, out.add(second, third)));
    emitter.compile();

    std::array<int64_t, 3> words { 100, 200, 300 };
    AOT_TEST_CHECK(emitter.call<int64_t>(words.data(), int64_t { 5 }) == 100 + 25 + 300);
    AOT_TEST_CHECK(words[0] == 100 && words[1] == 25 && words[2] == 300);
    words = { 100, 200, 300 };
    AOT_TEST_CHECK(emitter.call<int64_t>(words.data(), int64_t { 0 }) == 100 + 200 + 3);
    AOT_TEST_CHECK(words[0] == 100 && words[1] == 200 && words[2] == 3);
}

static void testOutputEitherJoinsValuesAndStrikesWrittenHeaps()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    const IndexedAbstractHeap& elements = emitter.heaps.indexedContiguousProperties;
    LValue base = emitter.argument(0);
    LValue x = emitter.argument(1);

    LValue written = out.load64(base, elements[0]);
    LValue untouched = out.load64(base, elements[1]);
    Typed::Integer joined = emitter.either<Typed::Integer>(Typed::Condition(out.greaterThan(x, out.int64Zero)), [&] {
        return Typed::Integer(out.add(written, x));
    }, [&] {
        out.store64(x, base, elements[0]);
        return Typed::Integer(out.sub(untouched, x));
    });

    LBasicBlock continuation = out.m_block;
    AOT_TEST_CHECK(joined.value()->opcode() == Phi && joined.value()->owner() == continuation);
    AOT_TEST_CHECK(continuation->phis().size() == 1 && continuation->isEmpty() && continuation->numberOfIncomingEdges() == 2);
    AOT_TEST_CHECK(out.load64(base, elements[1]) == untouched);
    LValue readAgain = out.load64(base, elements[0]);
    AOT_TEST_CHECK(readAgain != written);
    out.ret(out.add(joined.value(), readAgain));
    emitter.compile();

    std::array<int64_t, 2> words { 10, 20 };
    AOT_TEST_CHECK(emitter.call<int64_t>(words.data(), int64_t { 5 }) == (10 + 5) + 10);
    AOT_TEST_CHECK(emitter.call<int64_t>(words.data(), int64_t { -4 }) == (20 + 4) - 4);
    AOT_TEST_CHECK(words[0] == -4 && words[1] == 20);
}

static void testOutputNestedForksRestoreTheirOwnScopes()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue x = emitter.argument(0);
    LValue y = emitter.argument(1);

    LValue sum = out.add(x, y);
    LValue product = nullptr;
    Typed::Integer joined = emitter.either<Typed::Integer>(Typed::Condition(out.notZero64(x)), [&] {
        product = out.mul(x, y);
        LValue difference = nullptr;
        Typed::Integer inner = emitter.either<Typed::Integer>(Typed::Condition(out.notZero64(y)), [&] {
            AOT_TEST_CHECK(out.add(x, y) == sum && out.mul(x, y) == product);
            difference = out.sub(product, sum);
            return Typed::Integer(difference);
        }, [&] {
            AOT_TEST_CHECK(out.sub(product, sum) != difference);
            return Typed::Integer(sum);
        });
        AOT_TEST_CHECK(out.add(x, y) == sum && out.mul(x, y) == product && out.sub(product, sum) != difference);
        return Typed::Integer(out.add(inner.value(), product));
    }, [&] {
        AOT_TEST_CHECK(out.add(x, y) == sum && out.mul(x, y) != product);
        return Typed::Integer(sum);
    });
    AOT_TEST_CHECK(out.add(x, y) == sum);
    LValue productBehindJoin = out.mul(x, y);
    AOT_TEST_CHECK(productBehindJoin != product);
    out.ret(out.add(joined.value(), productBehindJoin));
    emitter.compile();

    for (int64_t left : { 0, 3, -5 }) {
        for (int64_t right : { 0, 4, -7 }) {
            int64_t inner = right ? left * right - (left + right) : left + right;
            int64_t expected = (left ? inner + left * right : left + right) + left * right;
            AOT_TEST_CHECK(emitter.call<int64_t>(left, right) == expected);
        }
    }
}

static void testOutputBranchUnlessContinuesTheScope()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    const IndexedAbstractHeap& elements = emitter.heaps.indexedContiguousProperties;
    LValue base = emitter.argument(0);
    LValue x = emitter.argument(1);
    LValue y = emitter.argument(2);
    LBasicBlock entry = out.m_block;
    LBasicBlock miss = out.newBlock();
    LBasicBlock neverMissed = out.newBlock();

    LValue sum = out.add(x, y);
    LValue loaded = out.load64(base, elements[0]);
    emitter.branchUnless(Typed::Condition(out.notZero64(x)), miss);
    LBasicBlock behindFirst = out.m_block;
    AOT_TEST_CHECK(behindFirst != entry && behindFirst->hasOnlyOnePredecessor() && entry->numSuccessors() == 2);
    AOT_TEST_CHECK(classOfEdgeInOutputTest(entry, behindFirst) == EdgeClass::Normal && classOfEdgeInOutputTest(entry, miss) == EdgeClass::Rare);
    AOT_TEST_CHECK(out.add(x, y) == sum && out.load64(base, elements[0]) == loaded);

    emitter.branchUnless(Typed::Condition(out.booleanTrue), neverMissed);
    AOT_TEST_CHECK(behindFirst->last()->opcode() == Jump && behindFirst->numSuccessors() == 1 && !neverMissed->numberOfIncomingEdges());
    AOT_TEST_CHECK(out.m_block->hasOnlyOnePredecessor() && out.add(x, y) == sum && out.load64(base, elements[0]) == loaded);

    emitter.branchUnless(Typed::Condition(out.notZero64(y)), miss);
    AOT_TEST_CHECK(miss->numberOfIncomingEdges() == 2 && !miss->hasOnlyOnePredecessor());
    AOT_TEST_CHECK(out.add(x, y) == sum && out.load64(base, elements[0]) == loaded);
    out.ret(out.add(sum, loaded));

    out.appendTo(miss);
    AOT_TEST_CHECK(out.add(x, y) != sum && out.load64(base, elements[0]) != loaded);
    out.ret(out.constInt64(-1));
    emitter.compile();

    int64_t word = 1000;
    AOT_TEST_CHECK(emitter.call<int64_t>(&word, int64_t { 2 }, int64_t { 3 }) == 1005);
    AOT_TEST_CHECK(emitter.call<int64_t>(&word, int64_t { 0 }, int64_t { 3 }) == -1);
    AOT_TEST_CHECK(emitter.call<int64_t>(&word, int64_t { 2 }, int64_t { 0 }) == -1);
}

static void testOutputComesBackToBlocksThatItLeft()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue x = emitter.argument(0);
    LValue y = emitter.argument(1);
    LBasicBlock entry = out.m_block;
    LBasicBlock fast = out.newBlock();
    LBasicBlock slow = out.newBlock();
    LBasicBlock restOfSlow = out.newBlock();
    LBasicBlock slowest = out.newBlock();
    LBasicBlock done = out.newBlock();
    LValue result = out.phiIn(done, Int64);
    LValue sum = out.add(x, y);

    out.appendTo(slow);
    AOT_TEST_CHECK(out.add(x, y) != sum);
    LValue product = out.mul(x, y);

    out.appendTo(slowest);
    AOT_TEST_CHECK(out.mul(x, y) != product);
    out.upsilon(out.sub(x, y), result);
    out.jump(done);

    out.appendTo(slow);
    AOT_TEST_CHECK(out.m_block == slow && !slow->isTerminated() && out.mul(x, y) == product);
    out.branch(out.lessThan(y, out.int64Zero), rareEdge(slowest), normalEdge(restOfSlow));

    out.appendTo(entry);
    AOT_TEST_CHECK(out.add(x, y) == sum && out.mul(x, y) != product);
    out.branch(out.lessThan(x, out.int64Zero), unlikelyEdge(slow), normalEdge(fast));

    out.appendTo(done);
    out.ret(result);

    out.appendTo(restOfSlow);
    out.upsilon(product, result);
    out.jump(done);

    out.appendTo(fast);
    out.upsilon(sum, result);
    out.jump(done);

    AOT_TEST_CHECK(done->numberOfIncomingEdges() == 3 && done->phis().size() == 1 && done->size() == 1);
    emitter.compile();
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 3 }, int64_t { 4 }) == 7);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 3 }, int64_t { -4 }) == -1);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { -3 }, int64_t { 4 }) == -12);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { -3 }, int64_t { -4 }) == 1);
}

static std::optional<int64_t> outputTestConstantIntegerOf(LValue value)
{
    if (value->hasInt())
        return value->asInt();
    if (value->opcode() == SExt32 && value->child(0)->hasInt32())
        return value->child(0)->asInt32();
    return std::nullopt;
}

static std::optional<double> outputTestConstantNumberOf(LValue value)
{
    if (value->hasDouble())
        return value->asDouble();
    if (value->opcode() != IToD)
        return std::nullopt;
    if (std::optional<int64_t> integer = outputTestConstantIntegerOf(value->child(0)))
        return static_cast<double>(*integer);
    return std::nullopt;
}

static void testOutputValuesAnswerWhatTheEmittersAsk()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    const AbstractHeap& heap = emitter.heaps.argumentBuffer;
    LValue base = emitter.argument(0);
    LValue x = emitter.argument(1);
    LValue a = emitter.numberArgument(0);
    LValue b = emitter.numberArgument(1);

    LValue seven = out.constInt32(7);
    AOT_TEST_CHECK(seven->opcode() == Const32 && seven->type() == Int32 && seven->isConstant() && seven->hasNoPosition() && !seven->numChildren());
    AOT_TEST_CHECK(seven->hasInt() && seven->hasInt32() && !seven->hasInt64() && !seven->hasDouble() && seven->asInt() == 7 && seven->isInt32(7) && !seven->isInt32(8));
    LValue minusOne = out.constInt64(-1);
    AOT_TEST_CHECK(minusOne->hasInt() && minusOne->hasInt64() && !minusOne->hasInt32() && minusOne->asInt() == -1 && minusOne->isInt64(-1));
    AOT_TEST_CHECK(out.constDouble(1.5)->hasDouble() && !out.constDouble(1.5)->hasInt() && out.constDouble(1.5)->asDouble() == 1.5);
    AOT_TEST_CHECK(out.constInt32(7) == seven && out.constInt32(0) == out.booleanFalse && out.constIntPtr(8) == out.intPtrEight);
    AOT_TEST_CHECK(out.constInt32(-1) != minusOne && out.constDouble(0.0) != out.constDouble(-0.0));

    AOT_TEST_CHECK(!x->isConstant() && !x->hasInt() && !x->hasNoPosition() && x->owner() == out.m_block);
    AOT_TEST_CHECK(x->opcode() == ArgumentReg && x->isArgumentReg(GPRInfo::toArgumentRegister(1)) && !x->isArgumentReg(GPRInfo::toArgumentRegister(0)) && emitter.argument(1) == x);
    LValue cageBase = out.argumentRegister(cellCageBaseGPR);
    AOT_TEST_CHECK(cageBase->isArgumentReg(cellCageBaseGPR) && cageBase->hasNoPosition() && out.argumentRegister(cellCageBaseGPR) == cageBase);

    AOT_TEST_CHECK(out.add(out.constInt32(2), out.constInt32(3))->isInt32(5) && out.equal(minusOne, minusOne)->isInt(1) && out.below(seven, 3)->isInt(0));
    AOT_TEST_CHECK(outputTestConstantIntegerOf(out.signExt32To64(out.constInt32(-5))) == -5 && !outputTestConstantIntegerOf(x));
    AOT_TEST_CHECK(outputTestConstantNumberOf(out.intToDouble(seven)) == 7.0 && !outputTestConstantNumberOf(a));

    LValue narrow = out.load32(out.address(heap, base, 4));
    LValue wide = out.zeroExt(narrow, Int64);
    AOT_TEST_CHECK(wide->opcode() == ZExt32 && wide->type() == Int64 && wide->numChildren() == 1 && wide->child(0) == narrow && wide->lastChild() == narrow);
    AOT_TEST_CHECK(out.zeroExt(wide, Int64) == wide && out.castToInt32(narrow) == narrow && out.castToInt32(wide) == narrow && out.castToInt32(x)->opcode() == Trunc);
    LValue sum = out.add(out.constInt64(8), x);
    AOT_TEST_CHECK(sum->opcode() == Add && sum->numChildren() == 2 && sum->child(0) == x && sum->child(1)->isInt64(8) && out.add(x, 8) == sum);

    LValue decoded = out.add(cageBase, out.shl(wide, cell32Shift));

    AOT_TEST_CHECK(!out.doubleEqual(a, a)->hasInt() && out.logicalNot(out.doubleLessThan(a, b))->opcode() != GreaterEqual);

    LValue loaded = out.load64(out.address(heap, out.add(base, 16), 8));
    AOT_TEST_CHECK(loaded->opcode() == Load && loaded->type() == Int64 && loaded->child(0) == base && loaded->offset() == 24 && loaded->heap() == heap.index());
    LValue stored = out.store64(x, out.address(heap, base, 8));
    AOT_TEST_CHECK(stored->opcode() == Store && stored->type() == Void && stored->child(0) == x && stored->lastChild() == base && stored->offset() == 8 && stored->heap() == heap.index());
    AOT_TEST_CHECK(out.load8ZeroExt32(out.address(heap, base))->opcode() == Load8Z && out.load16SignExt32(out.address(heap, base))->type() == Int32);
    AOT_TEST_CHECK(out.valueOf(out.address(heap, base)) == base && out.valueOf(out.address(heap, base, 16)) == out.add(base, 16));

    AOT_TEST_CHECK(out.valueKnownToBeReadThrough(out.address(heap, out.constIntPtr(JSValue::ValueNull))) == int64_t { JSValue::ValueNull });
    AOT_TEST_CHECK(out.valueKnownToBeReadThrough(out.address(heap, out.add(cageBase, undefinedCell32 << cell32Shift))) == int64_t { undefinedCell32 << cell32Shift });
    AOT_TEST_CHECK(!out.valueKnownToBeReadThrough(out.address(heap, base)) && !out.valueKnownToBeReadThrough(out.address(heap, decoded)));

    out.setOrigin(Origin(7));
    AOT_TEST_CHECK(out.origin() == Origin(7) && out.sub(x, base)->origin() == Origin(7) && !out.constInt64(1234)->origin());
}

static void testOutputKeepsThreeClassesOfEdges()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue x = emitter.argument(0);
    LBasicBlock entry = out.m_block;
    LBasicBlock usual = out.newBlock();
    LBasicBlock unusual = out.newBlock();
    LBasicBlock seldom = out.newBlock();
    LBasicBlock rare = out.newBlock();
    LBasicBlock behindRare = out.newBlock();

    AOT_TEST_CHECK(normalEdge(usual).edgeClass() == EdgeClass::Normal && unlikelyEdge(usual).edgeClass() == EdgeClass::Unlikely && rareEdge(usual).edgeClass() == EdgeClass::Rare);
    AOT_TEST_CHECK(!normalEdge(usual).isRare() && !unlikelyEdge(usual).isRare() && rareEdge(usual).isRare() && BlockEdge(usual) == normalEdge(usual));
    BranchTargets targets { rareEdge(rare), normalEdge(seldom) };
    AOT_TEST_CHECK(targets.swapped().taken == normalEdge(seldom) && targets.swapped().notTaken == rareEdge(rare));

    out.branch(out.below(x, 10), normalEdge(usual), unlikelyEdge(unusual));
    out.appendTo(usual);
    out.ret(out.add(x, 1));
    out.appendTo(unusual);
    out.branch(out.logicalNot(out.below(x, 20)), targets);
    out.appendTo(seldom);
    out.ret(out.add(x, 2));
    out.appendTo(rare);
    LValue tripled = out.mul(x, 3);
    out.jump(behindRare);
    out.appendTo(behindRare);
    out.ret(tripled);

    AOT_TEST_CHECK(classOfEdgeInOutputTest(entry, usual) == EdgeClass::Normal && classOfEdgeInOutputTest(entry, unusual) == EdgeClass::Unlikely);
    AOT_TEST_CHECK(classOfEdgeInOutputTest(unusual, seldom) == EdgeClass::Normal && classOfEdgeInOutputTest(unusual, rare) == EdgeClass::Rare);
    emitter.compile();

    const Order& order = emitter.test.procedure().order();
    for (LBasicBlock block : { entry, usual, unusual, seldom })
        AOT_TEST_CHECK(order.of(block).isReached() && !order.of(block).isRare() && order.of(block).layoutIndex < order.numberOfBlocksOutsideRareRegions());
    for (LBasicBlock block : { rare, behindRare })
        AOT_TEST_CHECK(order.of(block).isReached() && order.of(block).isRare() && order.of(block).layoutIndex >= order.numberOfBlocksOutsideRareRegions());
    AOT_TEST_CHECK(order.of(rare).entersRareRegion && !order.of(behindRare).entersRareRegion);
    AOT_TEST_CHECK(order.of(usual).orderIndex < order.of(unusual).orderIndex);

    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 5 }) == 6);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 15 }) == 17);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 25 }) == 75);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { -1 }) == -3);
}

static void checkOutputBlockJumpsAlongNormalEdge(LBasicBlock from, LBasicBlock target)
{
    AOT_TEST_CHECK(from->last()->opcode() == Jump && from->numSuccessors() == 1 && from->successor(0) == normalEdge(target));
    AOT_TEST_CHECK(target->numberOfIncomingEdges() == 1);
}

static void testOutputTerminatorsFoldedToJumpsTakeNormalEdges()
{
    OutputTestEmitter decidedBranch;
    LBasicBlock taken = decidedBranch.out.newBlock();
    LBasicBlock notTaken = decidedBranch.out.newBlock();
    decidedBranch.out.branch(decidedBranch.out.booleanFalse, normalEdge(taken), rareEdge(notTaken));
    checkOutputBlockJumpsAlongNormalEdge(decidedBranch.out.m_block, notTaken);
    AOT_TEST_CHECK(!taken->numberOfIncomingEdges());

    OutputTestEmitter sameTargetBothWays;
    LBasicBlock target = sameTargetBothWays.out.newBlock();
    sameTargetBothWays.out.branch(sameTargetBothWays.argument(0), rareEdge(target), unlikelyEdge(target));
    checkOutputBlockJumpsAlongNormalEdge(sameTargetBothWays.out.m_block, target);

    OutputTestEmitter decidedSwitch;
    LBasicBlock chosen = decidedSwitch.out.newBlock();
    LBasicBlock otherwise = decidedSwitch.out.newBlock();
    SwitchTarget cases[] = { { 4, normalEdge(otherwise) }, { 5, rareEdge(chosen) } };
    decidedSwitch.out.switchOn(decidedSwitch.out.constInt32(5), cases, normalEdge(otherwise));
    checkOutputBlockJumpsAlongNormalEdge(decidedSwitch.out.m_block, chosen);
    AOT_TEST_CHECK(!otherwise->numberOfIncomingEdges());

    OutputTestEmitter switchWithoutCases;
    LBasicBlock onlyTarget = switchWithoutCases.out.newBlock();
    switchWithoutCases.out.switchOn(switchWithoutCases.argument(0), { }, unlikelyEdge(onlyTarget));
    checkOutputBlockJumpsAlongNormalEdge(switchWithoutCases.out.m_block, onlyTarget);

    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue x = emitter.argument(0);
    Typed::Integer joined = emitter.either<Typed::Integer>(Typed::Condition(out.booleanFalse), [&] {
        return Typed::Integer(out.add(x, 1));
    }, [&] {
        return Typed::Integer(out.add(x, 2));
    });
    LBasicBlock join = out.m_block;
    out.ret(joined.value());
    emitter.compile();

    const Order& order = emitter.test.procedure().order();
    AOT_TEST_CHECK(order.rareRegions().empty() && order.of(join).isReached() && !order.of(join).isRare());
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 5 }) == 7);
}

static void testOutputHandlerTakesItsParameterThroughAPhi()
{
    ProcedureTraits traits;
    traits.hasHandlers = true;
    OutputTestEmitter emitter(traits);
    Output& out = emitter.out;
    LValue x = emitter.argument(0);
    LValue y = emitter.argument(1);
    LBasicBlock thrower = out.newBlock();
    LBasicBlock site = out.newBlock();
    LBasicBlock landingPad = out.newBlock();
    LBasicBlock returned = out.newBlock();
    LBasicBlock handler = out.newBlock();
    out.branch(out.lessThan(x, out.int64Zero), unlikelyEdge(thrower), normalEdge(site));

    out.appendTo(handler);
    LValue addend = out.add(y, 7);

    out.appendTo(thrower);
    LValue caught = out.phiIn(handler, Int64);
    AOT_TEST_CHECK(caught->opcode() == Phi && caught->type() == Int64 && caught->owner() == handler && out.m_block == thrower);
    AOT_TEST_CHECK(handler->phis().size() == 1 && handler->phis()[0] == caught && handler->size() == 1 && handler->at(0) == addend);
    LValue caughtUpsilon = out.upsilon(out.constInt64(77), caught);
    AOT_TEST_CHECK(caughtUpsilon->opcode() == Upsilon && caughtUpsilon->phi() == caught && thrower->upsilons().size() == 1 && thrower->isEmpty());
    out.jump(handler);

    out.appendTo(site);
    out.appendTo(landingPad);
    ValueFromBlock nothing = out.anchor(out.int64Zero);
    AOT_TEST_CHECK(nothing && nothing.block() == landingPad && nothing.value()->child(0) == out.int64Zero);
    out.addIncomingToPhi(caught, nothing);
    out.jump(handler);
    out.appendTo(site);

    ConstrainedValue operands[] = { ConstrainedValue(x, ValueRep::SomeRegister), ConstrainedValue(y, ValueRep::SomeRegister) };
    TestPatch call;
    call.kind = TestPatchKind::SumOrThrow;
    call.operands = operands;
    call.normalSuccessor = normalEdge(returned);
    call.landingPad = rareEdge(landingPad);
    LValue sum = appendTestPatch(emitter.test, call);
    AOT_TEST_CHECK(site->isTerminated() && landingPad->isLandingPad() && !handler->isLandingPad());
    out.appendToContinuation(returned);
    out.ret(sum);

    out.appendTo(handler);
    LValue thrown = out.select(out.notZero64(caught), caught, out.add(x, 1000));
    out.ret(out.add(thrown, addend));
    emitter.compile();

    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { -1 }, int64_t { 10 }) == 77 + (10 + 7));
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 2 }, int64_t { 4 }) == 2 + 4);
    AOT_TEST_CHECK(emitter.call<int64_t>(int64_t { 2 }, int64_t { 5 }) == (2 + 1000) + (5 + 7));
}

static void testOutputImmediatesTakeTheTypeOfTheOperandBeforeThem()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue wide = emitter.argument(0);
    LValue narrow = out.castToInt32(emitter.argument(1));

    AOT_TEST_CHECK(out.add(wide, 5)->type() == Int64 && out.add(wide, 5)->child(1)->isInt64(5));
    AOT_TEST_CHECK(out.add(narrow, 5)->type() == Int32 && out.add(narrow, 5)->child(1)->isInt32(5));
    AOT_TEST_CHECK(out.shl(wide, 3)->type() == Int64 && out.shl(wide, 3)->child(1)->isInt32(3));
    AOT_TEST_CHECK(out.above(narrow, undefinedCell32)->child(1)->isInt32(undefinedCell32) && out.bitAnd(narrow, UINT32_MAX) == narrow);

    LValue fromWide = out.bitOr(out.bitAnd(out.bitXor(out.sub(out.mul(out.add(wide, 5), 3), 7), 0x55), -16), out.lShr(wide, 60));
    LValue fromNarrow = out.aShr(out.shl(out.bitXor(out.add(narrow, 5), -1), 4), 2);
    out.ret(out.add(fromWide, out.signExt32To64(fromNarrow)));
    emitter.compile();

    struct {
        uint64_t wide;
        uint32_t narrow;
    } const cases[] = { { 0, 0 }, { 1, UINT32_MAX }, { UINT64_MAX, 0x7fffffffu }, { 1ull << 63, 0x80000000u }, { 0x123456789abcdef0ull, 0xfffffffau } };
    for (const auto& operands : cases) {
        uint64_t expectedFromWide = ((((operands.wide + 5) * 3 - 7) ^ 0x55) & ~uint64_t { 15 }) | operands.wide >> 60;
        int32_t expectedFromNarrow = static_cast<int32_t>(~(operands.narrow + 5) << 4) >> 2;
        uint64_t expected = expectedFromWide + static_cast<uint64_t>(static_cast<int64_t>(expectedFromNarrow));
        AOT_TEST_CHECK(emitter.call<uint64_t>(operands.wide, operands.narrow | 0x5a5a5a5aull << 32) == expected);
    }
}

static void testOutputComparesNumbersUnderTheNamesOfTheEmitters()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    LValue a = emitter.numberArgument(0);
    LValue b = emitter.numberArgument(1);
    LValue answers[] = {
        out.doubleEqual(a, b), out.doubleNotEqualOrUnordered(a, b), out.doubleEqualOrUnordered(a, b), out.doubleNotEqualAndOrdered(a, b),
        out.doubleLessThan(a, b), out.doubleLessThanOrEqual(a, b), out.doubleGreaterThan(a, b), out.doubleGreaterThanOrEqual(a, b),
        out.logicalNot(out.doubleLessThan(a, b)),
    };
    LValue bits = out.int32Zero;
    for (unsigned index = 0; index < std::size(answers); ++index)
        bits = out.bitOr(bits, out.shl(answers[index], index));
    out.ret(out.zeroExtPtr(bits));
    emitter.compile();

    constexpr double infinity = std::numeric_limits<double>::infinity();
    const double numbers[] = { 0.0, -0.0, 1.5, -1.5, infinity, -infinity, PNaN };
    for (double left : numbers) {
        for (double right : numbers) {
            bool isUnordered = left != left || right != right;
            const bool expectedAnswers[] = {
                left == right, left != right, isUnordered || left == right, !isUnordered && left != right,
                left < right, left <= right, left > right, left >= right,
                !(left < right),
            };
            int64_t expected = 0;
            for (unsigned index = 0; index < std::size(expectedAnswers); ++index)
                expected |= static_cast<int64_t>(expectedAnswers[index]) << index;
            AOT_TEST_CHECK(emitter.call<int64_t>(left, right) == expected);
        }
    }
}

static void testOutputAddressesElementsByIndex()
{
    OutputTestEmitter emitter;
    Output& out = emitter.out;
    const IndexedAbstractHeap& elements = emitter.heaps.indexedContiguousProperties;
    LValue base = emitter.argument(0);
    LValue index = emitter.argument(1);

    LValue next = out.load64(out.baseIndex(elements.atAnyIndex(), base, index, ScaleEight, sizeof(uint64_t)));
    LValue element = out.load64(out.baseIndex(elements, base, index));
    LValue fourth = out.load64(out.baseIndex(elements, base, index, jsNumber(3)));
    LValue masked = out.load64(out.baseIndex(elements, base, index, JSValue(), 0, out.constIntPtr(1)));
    LValue character = out.load8ZeroExt32(out.baseIndex(emitter.heaps.characters8.atAnyIndex(), base, index, ScaleOne));
    AOT_TEST_CHECK(next->offset() == 8 && next->heap() == elements.atAnyIndex().index() && element->heap() == elements.atAnyIndex().index());
    AOT_TEST_CHECK(fourth->child(0) == base && fourth->offset() == 24 && fourth->heap() == elements[3].index());

    LValue sum = out.add(next, out.mul(element, 3));
    sum = out.add(sum, out.mul(fourth, 5));
    sum = out.add(sum, out.mul(masked, 7));
    out.ret(out.add(sum, out.mul(out.zeroExtPtr(character), 11)));
    emitter.compile();

    const std::array<uint64_t, 5> words { 0x0403020111ull, 22, 33, 44, 55 };
    for (uint64_t place = 0; place < 4; ++place) {
        uint64_t expected = words[place + 1] + 3 * words[place] + 5 * words[3] + 7 * words[place & 1] + 11 * (words[0] >> (8 * place) & 0xff);
        AOT_TEST_CHECK(emitter.call<uint64_t>(words.data(), place) == expected);
    }
}

} // namespace Machine

std::span<const NativeTest> outputTests()
{
    static constexpr NativeTest tests[] = {
        { "output/wrappers/round-trip-their-values"_s, Machine::testOutputWrappersRoundTripTheirValues },
        { "output/wrappers/narrow-references-are-flagged"_s, Machine::testOutputNarrowReferencesAreFlagged },
        { "output/boxing/int32"_s, Machine::testOutputBoxesAndUnboxesInt32 },
        { "output/boxing/doubles"_s, Machine::testOutputBoxesAndUnboxesDoubles },
        { "output/boxing/values-are-told-apart-by-the-tags"_s, Machine::testOutputTellsBoxedValuesApartByTheTags },
        { "output/boxing/numbers-read-from-raw-bytes-are-purified"_s, Machine::testOutputPurifiesNumbersReadFromRawBytes },
        { "output/scopes/if-else-restores-the-scope-of-the-branch"_s, Machine::testOutputIfElseRestoresTheScopeOfTheBranch },
        { "output/scopes/either-joins-values-and-strikes-written-heaps"_s, Machine::testOutputEitherJoinsValuesAndStrikesWrittenHeaps },
        { "output/scopes/nested-forks-restore-their-own-scopes"_s, Machine::testOutputNestedForksRestoreTheirOwnScopes },
        { "output/scopes/branch-unless-continues-the-scope"_s, Machine::testOutputBranchUnlessContinuesTheScope },
        { "output/blocks/come-back-to-blocks-that-were-left"_s, Machine::testOutputComesBackToBlocksThatItLeft },
        { "output/blocks/three-classes-of-edges"_s, Machine::testOutputKeepsThreeClassesOfEdges },
        { "output/blocks/terminators-folded-to-jumps-take-normal-edges"_s, Machine::testOutputTerminatorsFoldedToJumpsTakeNormalEdges },
        { "output/blocks/handler-takes-its-parameter-through-a-phi"_s, Machine::testOutputHandlerTakesItsParameterThroughAPhi },
        { "output/values/answer-what-the-emitters-ask"_s, Machine::testOutputValuesAnswerWhatTheEmittersAsk },
        { "output/values/immediates-take-the-type-of-the-operand-before-them"_s, Machine::testOutputImmediatesTakeTheTypeOfTheOperandBeforeThem },
        { "output/values/comparisons-of-numbers"_s, Machine::testOutputComparesNumbersUnderTheNamesOfTheEmitters },
        { "output/values/elements-by-index"_s, Machine::testOutputAddressesElementsByIndex },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
