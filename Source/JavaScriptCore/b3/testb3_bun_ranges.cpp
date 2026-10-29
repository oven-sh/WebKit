/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3RangeReductions.h"
#include <wtf/TriState.h>

static constexpr uint64_t tag = 0xfffe000000000000ull;
static constexpr int64_t tagBits = static_cast<int64_t>(tag);

namespace {

struct RangeTestGraph {
    explicit RangeTestGraph(const Policy& policy = policyWith([](Policy&) { }))
        : proc(policy)
        , root(proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int32_t, int64_t*>(proc, root);
        x = arguments[0];
        y = arguments[1];
        pointer = arguments[2];
    }

    Value* int32(int32_t value) { return graph.int32(root, value); }
    Value* int64(int64_t value) { return graph.int64(root, value); }
    Value* number(double value) { return graph.number(root, value); }
    Value* make(B3::Opcode opcode, Value* child) { return graph.make(root, opcode, child); }
    Value* make(B3::Opcode opcode, Value* left, Value* right) { return graph.make(root, opcode, left, right); }
    Value* phi(Type type) { return graph.phi(root, type); }
    Value* loadByte() { return root->appendNew<MemoryValue>(proc, Load8Z, Int32, Origin(), pointer, 0, firstHeap); }
    Value* tagConstant() { return int64(tagBits); }
    Value* taggedWord() { return make(Add, make(ZExt32, y), tagConstant()); }
    void ret(Value* value) { graph.ret(root, value); }

    Procedure proc;
    Graph graph { proc };
    BasicBlock* root;
    Value* x;
    Value* y;
    Value* pointer;
};

template<typename Range>
struct SampleRanges {
    Range low { 0, 5 };
    Range high { 6, 9 };
    Range wide { 3, 7 };
    Range five { 5, 5 };
};

} // anonymous namespace

static Policy rangeReductionPolicy(bool reducesWithRanges)
{
    Policy policy = strengthReductionPolicy();
    policy.optimization.reducesWithRanges = reducesWithRanges;
    return policy;
}

static void testRangeForReadsKnownRange()
{
    RangeTestGraph graph;
    Value* phi = graph.phi(Int64);
    graph.proc.setKnownRange(phi, IntRange(0, 100));
    CHECK_EQ(rangeFor(graph.proc, graph.make(Add, phi, graph.int64(1))), IntRange(1, 101));

    Value* phiWithoutRange = graph.phi(Int64);
    CHECK_EQ(rangeFor(graph.proc, phiWithoutRange), IntRange::top<int64_t>());
    CHECK_EQ(rangeFor(graph.proc, graph.make(Add, phiWithoutRange, graph.int64(1))), IntRange::top<int64_t>());
}

static void testRangeForIntersectsOwnAndKnownRange()
{
    RangeTestGraph graph;
    Value* overlapping = graph.loadByte();
    graph.proc.setKnownRange(overlapping, IntRange(10, 1000));
    CHECK_EQ(rangeFor(graph.proc, overlapping), IntRange(10, 255));
}

static void testImplausibleKnownRangeIsIgnored()
{
    RangeTestGraph graph;
    Procedure& proc = graph.proc;
    Value* byte = graph.loadByte();
    graph.ret(byte);

    proc.setKnownRange(byte, IntRange(255, 400));
    CHECK_EQ(rangeFor(proc, byte), IntRange(255, 255));
    CHECK(isKnownRangePlausible(proc, byte));
    CHECK(!findViolatedRule(proc));

    proc.setKnownRange(byte, IntRange(256, 400));
    CHECK_EQ(rangeFor(proc, byte), IntRange(0, 255));
    CHECK(!isKnownRangePlausible(proc, byte));
    CHECK(findViolatedRule(proc) == ValidationRule::KnownRange);
}

static void testRangeForTrunc()
{
    RangeTestGraph graph;
    Value* lowByte = graph.make(BitAnd, graph.x, graph.int64(0xff));
    CHECK_EQ(rangeFor(graph.proc, graph.make(Trunc, lowByte)), IntRange(0, 255));
    CHECK_EQ(rangeFor(graph.proc, graph.make(Trunc, graph.x)), IntRange::top<int32_t>());
    CHECK_EQ(rangeFor(graph.proc, graph.make(Trunc, graph.make(ZExt32, graph.y))), IntRange::top<int32_t>());
}

static void testRangeForDepthLimit()
{
    auto incrementedByte = [](RangeTestGraph& graph, unsigned numberOfAdditions) {
        Value* sum = graph.loadByte();
        sum->setOrigin(numberedOrigin(1));
        for (unsigned i = 0; i < numberOfAdditions; ++i)
            sum = graph.make(Add, sum, graph.int32(1));
        return sum;
    };

    for (unsigned depthLimit : { static_cast<unsigned>(defaultValueOf(Limit::MaxRangeAnalysisDepth)), 2u }) {
        Policy policy = policyWith([&](Policy& configured) {
            configured.limits.set(Limit::MaxRangeAnalysisDepth, depthLimit);
        });
        unsigned maxExactAdditions = depthLimit - 1;

        RangeTestGraph shallow(policy);
        CHECK_EQ(rangeFor(shallow.proc, incrementedByte(shallow, maxExactAdditions)), IntRange(maxExactAdditions, 255 + maxExactAdditions));
        CHECK_EQ(limitHits(shallow.proc, Limit::MaxRangeAnalysisDepth), 0u);

        RangeTestGraph deep(policy);
        CHECK_EQ(rangeFor(deep.proc, incrementedByte(deep, maxExactAdditions + 1)), IntRange::top<int32_t>());
        CHECK(limitHits(deep.proc, Limit::MaxRangeAnalysisDepth) > 0);
        CHECK(hasNumber(deep.proc.report().firstOriginOf(Limit::MaxRangeAnalysisDepth), 1));
    }
}

static void testKnownRangeDiesWithOpcode()
{
    RangeTestGraph graph;
    Procedure& proc = graph.proc;
    Value* byte = graph.loadByte();
    proc.setKnownRange(byte, IntRange(0, 200));
    Value* copy = proc.clone(byte);
    CHECK(proc.knownRange(copy) == IntRange(0, 200));
    copy->setOpcodeUnsafely(Load8S);
    CHECK(!proc.knownRange(copy));
    CHECK(proc.knownRange(byte) == IntRange(0, 200));

    Value* phi = graph.phi(Int64);
    proc.setKnownRange(phi, IntRange(0, 100));
    CHECK(proc.knownRange(phi) == IntRange(0, 100));
    phi->replaceWithIdentity(graph.x);
    CHECK(!proc.knownRange(phi));
}

static void testUnsignedIntervalFor()
{
    RangeTestGraph graph;
    Procedure& proc = graph.proc;
    Value* lowFourBits = graph.make(BitAnd, graph.y, graph.int32(15));
    CHECK(intervalFor(proc, graph.taggedWord(), Signedness::Unsigned) == (IntegerInterval { tag, tag + 0xffffffff }));
    CHECK(intervalFor(proc, graph.make(Add, lowFourBits, graph.int32(1)), Signedness::Unsigned) == (IntegerInterval { 1, 16 }));
    CHECK(intervalFor(proc, graph.int64(-1), Signedness::Unsigned) == (IntegerInterval { ~0ull, ~0ull }));
    CHECK(intervalFor(proc, graph.int32(-1), Signedness::Unsigned) == (IntegerInterval { 0xffffffff, 0xffffffff }));

    CHECK(intervalFor(proc, graph.make(Add, graph.make(SExt32, graph.y), graph.int64(1)), Signedness::Unsigned) == fullInterval(Int64, Signedness::Unsigned));
    CHECK(intervalFor(proc, graph.make(Add, graph.y, graph.int32(1)), Signedness::Unsigned) == fullInterval(Int32, Signedness::Unsigned));
}

template<typename Range>
static void checkOrderedComparisons(B3::Opcode less, B3::Opcode lessEqual, B3::Opcode greater, B3::Opcode greaterEqual)
{
    auto [low, high, wide, five] = SampleRanges<Range> { };
    for (B3::Opcode opcode : { less, lessEqual }) {
        CHECK(evaluateComparison(opcode, low, high) == TriState::True);
        CHECK(evaluateComparison(opcode, high, low) == TriState::False);
        CHECK(evaluateComparison(opcode, low, wide) == TriState::Indeterminate);
    }
    for (B3::Opcode opcode : { greater, greaterEqual }) {
        CHECK(evaluateComparison(opcode, high, low) == TriState::True);
        CHECK(evaluateComparison(opcode, low, high) == TriState::False);
        CHECK(evaluateComparison(opcode, wide, low) == TriState::Indeterminate);
    }

    CHECK(evaluateComparison(lessEqual, low, five) == TriState::True);
    CHECK(evaluateComparison(less, low, five) == TriState::Indeterminate);
    CHECK(evaluateComparison(less, five, low) == TriState::False);
    CHECK(evaluateComparison(lessEqual, five, low) == TriState::Indeterminate);
    CHECK(evaluateComparison(greaterEqual, five, low) == TriState::True);
    CHECK(evaluateComparison(greater, five, low) == TriState::Indeterminate);
    CHECK(evaluateComparison(greater, low, five) == TriState::False);
    CHECK(evaluateComparison(greaterEqual, low, five) == TriState::Indeterminate);

    CHECK(evaluateComparison(Add, low, high) == TriState::Indeterminate);
}

template<typename Range, typename Integer>
static void checkSmallRangeComparisons(B3::Opcode opcode, Integer smallest)
{
    constexpr unsigned numberOfIntegers = 4;
    auto forEachRange = [&](const auto& functor) {
        for (unsigned min = 0; min < numberOfIntegers; ++min) {
            for (unsigned max = min; max < numberOfIntegers; ++max)
                functor(smallest + min, smallest + max);
        }
    };
    forEachRange([&](Integer leftMin, Integer leftMax) {
        forEachRange([&](Integer rightMin, Integer rightMax) {
            bool canHold = false;
            bool canFail = false;
            for (Integer left = leftMin; left <= leftMax; ++left) {
                for (Integer right = rightMin; right <= rightMax; ++right)
                    (holds(opcode, static_cast<int64_t>(left), static_cast<int64_t>(right)) ? canHold : canFail) = true;
            }
            TriState expected = canHold && canFail ? TriState::Indeterminate : triState(canHold);
            CHECK(evaluateComparison(opcode, Range { leftMin, leftMax }, Range { rightMin, rightMax }) == expected);
        });
    });
}

template<typename Range>
static void checkEqualityComparisons()
{
    auto [low, high, wide, five] = SampleRanges<Range> { };
    CHECK(evaluateComparison(Equal, five, five) == TriState::True);
    CHECK(evaluateComparison(Equal, low, high) == TriState::False);
    CHECK(evaluateComparison(Equal, low, wide) == TriState::Indeterminate);
    CHECK(evaluateComparison(Equal, low, low) == TriState::Indeterminate);
    CHECK(evaluateComparison(NotEqual, five, five) == TriState::False);
    CHECK(evaluateComparison(NotEqual, low, high) == TriState::True);
    CHECK(evaluateComparison(NotEqual, low, wide) == TriState::Indeterminate);
    CHECK(evaluateComparison(NotEqual, low, low) == TriState::Indeterminate);
}

static void testEvaluateComparisonOnRanges()
{
    checkOrderedComparisons<IntRange>(LessThan, LessEqual, GreaterThan, GreaterEqual);
    checkOrderedComparisons<IntegerInterval>(LessThan, LessEqual, GreaterThan, GreaterEqual);
    checkOrderedComparisons<IntegerInterval>(Below, BelowEqual, Above, AboveEqual);
    checkEqualityComparisons<IntRange>();
    checkEqualityComparisons<IntegerInterval>();
    for (B3::Opcode opcode : { Equal, NotEqual, LessThan, LessEqual, GreaterThan, GreaterEqual }) {
        checkSmallRangeComparisons<IntRange, int64_t>(opcode, -2);
        checkSmallRangeComparisons<IntegerInterval, int64_t>(opcode, -2);
    }
    for (B3::Opcode opcode : { Equal, NotEqual, Below, BelowEqual, Above, AboveEqual })
        checkSmallRangeComparisons<IntegerInterval, uint64_t>(opcode, (1ull << 63) - 2);

    SampleRanges<IntRange> signedRanges;
    for (B3::Opcode opcode : { Below, BelowEqual, Above, AboveEqual })
        CHECK(evaluateComparison(opcode, signedRanges.low, signedRanges.high) == TriState::Indeterminate);

    SampleRanges<IntegerInterval> lowerHalf;
    IntegerInterval upperHalf { 1ull << 63, ~0ull };
    IntegerInterval negativeHalf { std::numeric_limits<int64_t>::min(), -1 };
    for (IntegerInterval interval : { lowerHalf.low, lowerHalf.high, lowerHalf.wide, lowerHalf.five }) {
        for (B3::Opcode opcode : { Below, BelowEqual }) {
            CHECK(evaluateComparison(opcode, interval, upperHalf) == TriState::True);
            CHECK(evaluateComparison(opcode, upperHalf, interval) == TriState::False);
        }
        for (B3::Opcode opcode : { Above, AboveEqual }) {
            CHECK(evaluateComparison(opcode, upperHalf, interval) == TriState::True);
            CHECK(evaluateComparison(opcode, interval, upperHalf) == TriState::False);
        }
        for (B3::Opcode opcode : { LessThan, LessEqual }) {
            CHECK(evaluateComparison(opcode, negativeHalf, interval) == TriState::True);
            CHECK(evaluateComparison(opcode, interval, negativeHalf) == TriState::False);
        }
        for (B3::Opcode opcode : { GreaterThan, GreaterEqual }) {
            CHECK(evaluateComparison(opcode, interval, negativeHalf) == TriState::True);
            CHECK(evaluateComparison(opcode, negativeHalf, interval) == TriState::False);
        }
    }
}

static void testEvaluateComparisonOfValues()
{
    RangeTestGraph graph;
    Value* phi = graph.phi(Int64);
    graph.proc.setKnownRange(phi, IntRange(0, 10));
    auto evaluate = [&](B3::Opcode opcode, Value* left, Value* right) {
        return evaluateComparison(graph.proc, graph.make(opcode, left, right));
    };

    CHECK(evaluate(Below, graph.loadByte(), graph.int32(256)) == TriState::True);
    CHECK(evaluate(LessThan, graph.int64(11), phi) == TriState::False);
    CHECK(evaluate(Equal, phi, graph.int64(11)) == TriState::False);
    CHECK(evaluate(AboveEqual, graph.taggedWord(), graph.tagConstant()) == TriState::True);

    CHECK(evaluate(LessThan, graph.phi(Int64), graph.int64(11)) == TriState::Indeterminate);
    CHECK(evaluate(LessThan, graph.number(1), graph.number(2)) == TriState::Indeterminate);
    CHECK(evaluate(Add, graph.x, graph.x) == TriState::Indeterminate);
}

template<typename Functor>
static std::optional<bool> comparisonResult(B3::Opcode comparison, int64_t constant, bool constantIsFirst, const Functor& makeOperand)
{
    Procedure proc(strengthReductionPolicy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, int32_t>(proc, root);
    Value* operand = makeOperand(graph, root, arguments[0], arguments[1]);
    Value* other = operand->type() == Int32 ? graph.int32(root, static_cast<int32_t>(constant)) : graph.int64(root, constant);
    graph.ret(root, constantIsFirst ? graph.make(root, comparison, other, operand) : graph.make(root, comparison, operand, other));
    reduceStrengthOnce(proc);
    Value* returned = proc.at(0)->last()->child(0);
    if (!returned->hasInt32())
        return std::nullopt;
    return !!returned->asInt32();
}

static void testComparisonsFoldedByIntegerRange()
{
    auto lowFourBits = [](Graph& graph, BasicBlock* block, Value* wide, Value*) {
        return graph.make(block, BitAnd, wide, graph.int64(block, 15));
    };
    auto zeroExtended = [](Graph& graph, BasicBlock* block, Value*, Value* word) {
        return graph.make(block, ZExt32, word);
    };
    auto zeroExtendedPlusFive = [](Graph& graph, BasicBlock* block, Value*, Value* word) {
        return graph.make(block, Add, graph.make(block, ZExt32, word), graph.int64(block, 5));
    };
    auto signExtendedMinusOne = [](Graph& graph, BasicBlock* block, Value*, Value* word) {
        return graph.make(block, Add, graph.make(block, SExt32, word), graph.int64(block, -1));
    };
    auto anything = [](Graph&, BasicBlock*, Value* wide, Value*) {
        return wide;
    };

    CHECK(comparisonResult(LessThan, 16, false, lowFourBits) == true);
    CHECK(comparisonResult(LessThan, 0, false, lowFourBits) == false);
    CHECK(comparisonResult(LessEqual, 15, false, lowFourBits) == true);
    CHECK(comparisonResult(GreaterThan, 15, false, lowFourBits) == false);
    CHECK(comparisonResult(GreaterThan, -1, false, lowFourBits) == true);
    CHECK(comparisonResult(GreaterEqual, 0, false, lowFourBits) == true);
    CHECK(comparisonResult(GreaterEqual, 16, false, lowFourBits) == false);
    CHECK(comparisonResult(Equal, 16, false, lowFourBits) == false);
    CHECK(comparisonResult(NotEqual, -1, false, lowFourBits) == true);
    CHECK(comparisonResult(Below, 1ll << 32, false, zeroExtended) == true);
    CHECK(comparisonResult(AboveEqual, 1ll << 32, false, zeroExtended) == false);
    CHECK(comparisonResult(Above, 4, false, zeroExtendedPlusFive) == true);
    CHECK(comparisonResult(BelowEqual, 4, false, zeroExtendedPlusFive) == false);
    CHECK(comparisonResult(Below, 4, true, zeroExtendedPlusFive) == true);
    CHECK(comparisonResult(AboveEqual, 1ll << 33, true, zeroExtendedPlusFive) == true);

    CHECK(!comparisonResult(LessThan, 15, false, lowFourBits));
    CHECK(!comparisonResult(LessEqual, 14, false, lowFourBits));
    CHECK(!comparisonResult(GreaterThan, 0, false, lowFourBits));
    CHECK(!comparisonResult(GreaterEqual, 1, false, lowFourBits));
    CHECK(!comparisonResult(Equal, 15, false, lowFourBits));
    CHECK(!comparisonResult(NotEqual, 0, false, lowFourBits));
    CHECK(!comparisonResult(Below, 0xffffffffll, false, zeroExtended));
    CHECK(!comparisonResult(Above, 5, false, zeroExtendedPlusFive));
    CHECK(!comparisonResult(Below, 1ll << 40, false, signExtendedMinusOne));
    CHECK(!comparisonResult(LessThan, 16, false, anything));
    CHECK(!comparisonResult(Below, 16, false, anything));
}

static void testBoxedInt32ComparisonsFoldByRange()
{
    struct Comparison {
        B3::Opcode opcode;
        bool hasConstantOnLeft;
    };
    struct Variant {
        bool reducesWithRanges;
        B3::Opcode extension;
        unsigned remainingComparisonCount;
    };
    for (Comparison comparison : { Comparison { AboveEqual, false }, Comparison { Below, false }, Comparison { BelowEqual, true }, Comparison { Above, true } }) {
        for (Variant variant : { Variant { true, ZExt32, 0 }, Variant { false, ZExt32, 1 }, Variant { true, SExt32, 1 } }) {
            Procedure proc(rangeReductionPolicy(variant.reducesWithRanges));
            Graph graph { proc };
            BasicBlock* root = proc.addBlock();
            Value* operand = cCallArgumentValues<int64_t>(proc, root)[0];
            Value* extended = graph.make(root, variant.extension, graph.make(root, Trunc, operand));
            Value* boxed = graph.make(root, Add, extended, graph.int64(root, tagBits));
            Value* constant = graph.int64(root, tagBits);
            Value* result = comparison.hasConstantOnLeft ? graph.make(root, comparison.opcode, constant, boxed) : graph.make(root, comparison.opcode, boxed, constant);
            graph.ret(root, graph.make(root, ZExt32, result));
            resetAndValidate(proc);
            reduceStrengthOnce(proc);
            CHECK_EQ(numberOfValues(proc, [](Value* value) {
                return value->opcode() == AboveEqual || value->opcode() == Below || value->opcode() == BelowEqual || value->opcode() == Above;
            }), variant.remainingComparisonCount);

            auto code = compileProc(proc);
            for (int64_t value : { 0, 1, -1 }) {
                int64_t word = variant.extension == ZExt32 ? static_cast<int64_t>(static_cast<uint32_t>(value)) : value;
                int64_t boxedValue = tagBits + word;
                bool expected = comparison.hasConstantOnLeft ? holds(comparison.opcode, tagBits, boxedValue) : holds(comparison.opcode, boxedValue, tagBits);
                CHECK_EQ(invoke<int64_t>(*code, value), static_cast<int64_t>(expected));
            }
        }
    }
}

static void checkExtensionOfTruncation(B3::Opcode extension, IntRange range, bool isRedundant, bool reducesWithRanges)
{
    Procedure proc(rangeReductionPolicy(reducesWithRanges));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* wide = cCallArgumentValues<int64_t>(proc, root)[0];
    proc.setKnownRange(wide, range);
    Value* extended = graph.make(root, extension, graph.make(root, Trunc, wide));
    graph.ret(root, extended);
    CHECK(findRangeReduction(proc, extended).value == (isRedundant ? wide : nullptr));

    reduceStrengthOnce(proc);
    unsigned remainingCount = isRedundant && reducesWithRanges ? 0 : 1;
    CHECK_EQ(numberOfValues(proc, extension), remainingCount);
    CHECK_EQ(numberOfValues(proc, Trunc), remainingCount);

    auto code = compileProc(proc);
    for (int64_t input : { range.min(), static_cast<int64_t>(1), range.max() }) {
        int64_t expected = extension == SExt32 ? static_cast<int64_t>(static_cast<int32_t>(input)) : static_cast<int64_t>(static_cast<uint32_t>(input));
        CHECK_EQ(invoke<int64_t>(*code, input), expected);
    }
}

static void testExtensionOfTruncationFoldsByRange()
{
    constexpr int64_t minInt32 = std::numeric_limits<int32_t>::min();
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
    constexpr int64_t maxUInt32 = std::numeric_limits<uint32_t>::max();
    for (bool reducesWithRanges : { true, false }) {
        checkExtensionOfTruncation(SExt32, IntRange(minInt32, maxInt32), true, reducesWithRanges);
        checkExtensionOfTruncation(SExt32, IntRange(minInt32, maxInt32 + 1), false, reducesWithRanges);
        checkExtensionOfTruncation(SExt32, IntRange(minInt32 - 1, maxInt32), false, reducesWithRanges);
        checkExtensionOfTruncation(ZExt32, IntRange(0, maxUInt32), true, reducesWithRanges);
        checkExtensionOfTruncation(ZExt32, IntRange(0, maxUInt32 + 1), false, reducesWithRanges);
        checkExtensionOfTruncation(ZExt32, IntRange(-1, maxUInt32), false, reducesWithRanges);
    }
}

static void checkSumOfIntegersBehindDoubles(B3::Opcode opcode, IntRange range, bool isExact, bool reducesWithRanges)
{
    Procedure proc(rangeReductionPolicy(reducesWithRanges));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t*, int32_t>(proc, root);
    Value* wide = graph.load(root, Int64, arguments[0], 0, firstHeap);
    proc.setKnownRange(wide, range);
    Value* sum = graph.make(root, opcode, graph.make(root, IToD, wide), graph.make(root, IToD, arguments[1]));
    PatchpointValue* truncated = root->appendNew<PatchpointValue>(proc, Int64, Origin());
    truncated->append(sum, ValueRep::SomeRegister);
    truncated->effects = Effects::none();
    truncated->pureFunction = PureFunction { 1, true, true };
    truncated->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    graph.ret(root, truncated);

    reduceStrengthOnce(proc);
    reduceStrengthOnce(proc);
    bool isReduced = isExact && reducesWithRanges;
    CHECK_EQ(numberOfValues(proc, Patchpoint), isReduced ? 0u : 1u);

    auto code = compileProc(proc);
    for (int64_t left : { range.min(), static_cast<int64_t>(0), range.max() }) {
        for (int32_t right : { std::numeric_limits<int32_t>::min(), -1, 0, 7, std::numeric_limits<int32_t>::max() }) {
            double expected = opcode == Add ? static_cast<double>(left) + right : static_cast<double>(left) - right;
            CHECK_EQ(invoke<int64_t>(*code, &left, right), static_cast<int64_t>(expected));
        }
    }
}

static void testSumOfIntegersBehindDoublesIsAddedInIntegers()
{
    constexpr int64_t limit = 1ll << 52;
    for (bool reducesWithRanges : { true, false }) {
        for (B3::Opcode opcode : { Add, Sub }) {
            checkSumOfIntegersBehindDoubles(opcode, IntRange(-limit, limit), true, reducesWithRanges);
            checkSumOfIntegersBehindDoubles(opcode, IntRange(-limit, limit + 1), false, reducesWithRanges);
            checkSumOfIntegersBehindDoubles(opcode, IntRange(-limit - 1, limit), false, reducesWithRanges);
        }
    }
}

enum class DoubleExpression : uint8_t { IntegerTimesHalf, SumOfProducts, IntegerTimesInfinity, IntegerDividedByInteger, Argument, ManyLargeFactors };

static void checkDoubleComparedWithItself(DoubleExpression expression, bool isNeverNaN, bool reducesWithRanges)
{
    for (B3::Opcode comparison : { Equal, NotEqual }) {
        Procedure proc(rangeReductionPolicy(reducesWithRanges));
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int32_t, int64_t, double>(proc, root);
        auto constant = [&](double number) -> Value* { return root->appendNew<ConstDoubleValue>(proc, Origin(), number); };
        Value* narrow = graph.make(root, IToD, arguments[0]);
        Value* wide = graph.make(root, IToD, arguments[1]);
        Value* number = nullptr;
        switch (expression) {
        case DoubleExpression::IntegerTimesHalf:
            number = graph.make(root, Mul, narrow, constant(0.5));
            break;
        case DoubleExpression::SumOfProducts:
            number = graph.make(root, Sub, graph.make(root, Add, graph.make(root, Mul, narrow, wide), constant(-1e300)), graph.make(root, Neg, graph.make(root, Abs, wide)));
            break;
        case DoubleExpression::IntegerTimesInfinity:
            number = graph.make(root, Mul, narrow, constant(std::numeric_limits<double>::infinity()));
            break;
        case DoubleExpression::IntegerDividedByInteger:
            number = graph.make(root, Div, narrow, wide);
            break;
        case DoubleExpression::Argument:
            number = graph.make(root, Add, arguments[2], narrow);
            break;
        case DoubleExpression::ManyLargeFactors:
            number = graph.make(root, Mul, graph.make(root, Mul, constant(1e300), constant(1e300)), graph.make(root, Sub, narrow, narrow));
            break;
        }
        graph.ret(root, graph.make(root, comparison, number, number));

        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfValues(proc, comparison), isNeverNaN && reducesWithRanges ? 0u : 1u);

        auto code = compileProc(proc);
        for (int32_t first : { std::numeric_limits<int32_t>::min(), 0, 3, std::numeric_limits<int32_t>::max() }) {
            for (int64_t second : { std::numeric_limits<int64_t>::min(), static_cast<int64_t>(0), static_cast<int64_t>(5), std::numeric_limits<int64_t>::max() }) {
                for (double third : { 0.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() }) {
                    double a = first;
                    double b = static_cast<double>(second);
                    double expected = 0;
                    switch (expression) {
                    case DoubleExpression::IntegerTimesHalf:
                        expected = a * 0.5;
                        break;
                    case DoubleExpression::SumOfProducts:
                        expected = a * b + -1e300 - -std::abs(b);
                        break;
                    case DoubleExpression::IntegerTimesInfinity:
                        expected = a * std::numeric_limits<double>::infinity();
                        break;
                    case DoubleExpression::IntegerDividedByInteger:
                        expected = a / b;
                        break;
                    case DoubleExpression::Argument:
                        expected = third + a;
                        break;
                    case DoubleExpression::ManyLargeFactors:
                        expected = std::numeric_limits<double>::infinity() * (a - a);
                        break;
                    }
                    bool isNaN = expected != expected;
                    CHECK(!isNaN || !isNeverNaN);
                    CHECK_EQ(invoke<int32_t>(*code, first, second, third), static_cast<int32_t>(comparison == Equal ? !isNaN : isNaN));
                }
            }
        }
    }
}

static void testReduceStrengthFoldsSelfComparisonOfFiniteDouble()
{
    for (bool reducesWithRanges : { true, false }) {
        checkDoubleComparedWithItself(DoubleExpression::IntegerTimesHalf, true, reducesWithRanges);
        checkDoubleComparedWithItself(DoubleExpression::SumOfProducts, true, reducesWithRanges);
        checkDoubleComparedWithItself(DoubleExpression::IntegerTimesInfinity, false, reducesWithRanges);
        checkDoubleComparedWithItself(DoubleExpression::IntegerDividedByInteger, false, reducesWithRanges);
        checkDoubleComparedWithItself(DoubleExpression::Argument, false, reducesWithRanges);
        checkDoubleComparedWithItself(DoubleExpression::ManyLargeFactors, false, reducesWithRanges);
    }
}

static void checkSignExtension(IntRange range, bool becomesZeroExtension, bool reducesWithRanges)
{
    Procedure proc(rangeReductionPolicy(reducesWithRanges));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* word = graph.load(root, Int32, cCallArgumentValues<int32_t*>(proc, root)[0], 0, firstHeap);
    proc.setKnownRange(word, range);
    Value* extended = graph.make(root, SExt32, word);
    graph.ret(root, extended);
    RangeReduction reduction = findRangeReduction(proc, extended);
    CHECK(reduction.kind == (becomesZeroExtension ? RangeReduction::Kind::ZeroExtension : RangeReduction::Kind::None));
    CHECK(reduction.value == (becomesZeroExtension ? word : nullptr));

    reduceStrengthOnce(proc);
    CHECK_EQ(numberOfValues(proc, ZExt32), becomesZeroExtension && reducesWithRanges ? 1u : 0u);
    CHECK_EQ(numberOfValues(proc, SExt32), becomesZeroExtension && reducesWithRanges ? 0u : 1u);

    auto code = compileProc(proc);
    for (int64_t input : { range.min(), static_cast<int64_t>(1), range.max() }) {
        int32_t memory = static_cast<int32_t>(input);
        CHECK_EQ(invoke<int64_t>(*code, &memory), input);
    }
}

static void testSignExtensionOfNonNegativeIsZeroExtension()
{
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
    for (bool reducesWithRanges : { true, false }) {
        checkSignExtension(IntRange(0, maxInt32), true, reducesWithRanges);
        checkSignExtension(IntRange(5, 9), true, reducesWithRanges);
        checkSignExtension(IntRange(-1, maxInt32), false, reducesWithRanges);
        checkSignExtension(IntRange::top<int32_t>(), false, reducesWithRanges);
    }
}

struct ExtendedOperand {
    B3::Opcode extension;
    bool isKnownNonNegative;
};

static int64_t extendWord(const ExtendedOperand& operand, int32_t word)
{
    return operand.extension == SExt32 ? static_cast<int64_t>(word) : static_cast<int64_t>(static_cast<uint32_t>(word));
}

static void checkComparisonOfExtensions(B3::Opcode opcode, ExtendedOperand left, ExtendedOperand right, bool reducesWithRanges)
{
    constexpr int32_t maxInt32 = std::numeric_limits<int32_t>::max();
    constexpr int32_t minInt32 = std::numeric_limits<int32_t>::min();
    Procedure proc(rangeReductionPolicy(reducesWithRanges));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* pointer = cCallArgumentValues<int32_t*>(proc, root)[0];
    Value* leftWord = graph.load(root, Int32, pointer, 0, firstHeap);
    Value* rightWord = graph.load(root, Int32, pointer, 4, firstHeap);
    if (left.isKnownNonNegative)
        proc.setKnownRange(leftWord, IntRange(0, maxInt32));
    if (right.isKnownNonNegative)
        proc.setKnownRange(rightWord, IntRange(0, maxInt32));
    Value* comparison = graph.make(root, opcode, graph.make(root, left.extension, leftWord), graph.make(root, right.extension, rightWord));
    graph.ret(root, graph.make(root, ZExt32, comparison));

    bool isNarrowed = left.extension == right.extension || left.isKnownNonNegative || right.isKnownNonNegative;
    RangeReduction reduction = findRangeReduction(proc, comparison);
    CHECK(reduction.kind == (isNarrowed ? RangeReduction::Kind::Comparison : RangeReduction::Kind::None));
    if (isNarrowed) {
        CHECK(reduction.value == leftWord);
        CHECK(reduction.right == rightWord);
    }

    reduceStrengthOnce(proc);
    CHECK_EQ(numberOfValues(proc, [](Value* value) {
        return isIntegerComparison(value->opcode()) && value->child(0)->type() == Int32;
    }), isNarrowed && reducesWithRanges ? 1u : 0u);

    auto code = compileProc(proc);
    for (int32_t leftInput : { 0, 1, 7, maxInt32, -1, -7, minInt32 }) {
        for (int32_t rightInput : { 0, 1, 7, maxInt32, -1, -7, minInt32 }) {
            if ((left.isKnownNonNegative && leftInput < 0) || (right.isKnownNonNegative && rightInput < 0))
                continue;
            int32_t memory[2] = { leftInput, rightInput };
            CHECK_EQ(invoke<int64_t>(*code, memory), static_cast<int64_t>(holds(opcode, extendWord(left, leftInput), extendWord(right, rightInput))));
        }
    }
}

static void testComparisonOfExtensionsIsNarrow()
{
    for (B3::Opcode opcode : { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Below, BelowEqual, Above, AboveEqual }) {
        for (B3::Opcode leftExtension : { SExt32, ZExt32 }) {
            for (B3::Opcode rightExtension : { SExt32, ZExt32 }) {
                for (bool isLeftNonNegative : { false, true }) {
                    for (bool isRightNonNegative : { false, true }) {
                        for (bool reducesWithRanges : { true, false })
                            checkComparisonOfExtensions(opcode, { leftExtension, isLeftNonNegative }, { rightExtension, isRightNonNegative }, reducesWithRanges);
                    }
                }
            }
        }
    }
}

static void checkMask(Type type, IntRange range, int64_t mask, bool isRedundant, bool reducesWithRanges)
{
    Procedure proc(rangeReductionPolicy(reducesWithRanges));
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* wide = cCallArgumentValues<int64_t>(proc, root)[0];
    proc.setKnownRange(wide, range);
    Value* operand = type == Int32 ? graph.make(root, Trunc, wide) : wide;
    Value* masked = graph.make(root, BitAnd, operand, type == Int32 ? graph.int32(root, static_cast<int32_t>(mask)) : graph.int64(root, mask));
    graph.ret(root, type == Int32 ? graph.make(root, ZExt32, masked) : masked);
    CHECK(findRangeReduction(proc, masked).value == (isRedundant ? operand : nullptr));

    reduceStrengthOnce(proc);
    CHECK_EQ(numberOfValues(proc, BitAnd), isRedundant && reducesWithRanges ? 0u : 1u);

    auto code = compileProc(proc);
    for (int64_t input : { range.min(), static_cast<int64_t>(1), range.max() })
        CHECK_EQ(invoke<int64_t>(*code, input), input & mask);
}

static void testMaskFoldsByRange()
{
    for (Type type : { Int32, Int64 }) {
        for (bool reducesWithRanges : { true, false }) {
            checkMask(type, IntRange(0, 255), 255, true, reducesWithRanges);
            checkMask(type, IntRange(0, 256), 255, false, reducesWithRanges);
            checkMask(type, IntRange(-1, 255), 255, false, reducesWithRanges);
            checkMask(type, IntRange(0, 254), 254, false, reducesWithRanges);
        }
    }
}

template<typename T>
static void testVariableShiftRange(B3::Opcode opcode, int32_t amount, bool usesPolicy)
{
    using UnsignedT = std::make_unsigned_t<T>;
    constexpr bool is32 = sizeof(T) == 4;
    Procedure proc(usesPolicy ? optimizingPolicy() : Policy());
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto integer = [&](int32_t value) {
        return is32 ? graph.int32(root, value) : graph.int64(root, value);
    };
    Value* argument = cCallArgumentValues<int64_t>(proc, root)[0];
    Value* shifted = graph.make(root, opcode, is32 ? graph.make(root, Trunc, argument) : argument, graph.int32(root, amount));
    Value* isLess = graph.make(root, LessThan, shifted, integer(-8));
    Value* lowByte = graph.make(root, BitAnd, shifted, integer(255));
    graph.ret(root, graph.make(root, Add, graph.make(root, ZExt32, isLess), graph.make(root, Shl, is32 ? graph.make(root, ZExt32, lowByte) : lowByte, graph.int32(root, 1))));
    auto code = compileProc(proc, 2);

    unsigned amountTaken = amount & (sizeof(T) * 8 - 1);
    for (int64_t input : { static_cast<int64_t>(0), static_cast<int64_t>(5), static_cast<int64_t>(-1), static_cast<int64_t>(-9), static_cast<int64_t>(-1000), static_cast<int64_t>(0x7fffffff), static_cast<int64_t>(-0x80000000ll), std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min() }) {
        T operand = static_cast<T>(input);
        T expected;
        if (opcode == Shl)
            expected = static_cast<T>(static_cast<UnsignedT>(operand) << amountTaken);
        else if (opcode == SShr)
            expected = operand >> amountTaken;
        else
            expected = static_cast<T>(static_cast<UnsignedT>(operand) >> amountTaken);
        CHECK_EQ(invoke<int64_t>(*code, input), (expected < -8) + 2 * static_cast<int64_t>(expected & 255));
    }
}

static void testVariableShiftRanges()
{
    for (B3::Opcode opcode : { Shl, SShr, ZShr }) {
        for (int32_t amount : { 0, 1, 31, 32, 33, 63, 64, 65, 96, 128, -1, -32, -64, -128 }) {
            for (bool usesPolicy : { true, false }) {
                testVariableShiftRange<int32_t>(opcode, amount, usesPolicy);
                testVariableShiftRange<int64_t>(opcode, amount, usesPolicy);
            }
        }
    }
}

static void testTruncOfVariableShiftLeftAndBack()
{
    for (int32_t amount : { 0, 1, 12, 30, 31, 32, 33, 60, 63, 64, 76, -1, -4, -33, -52, -60, -64 }) {
        Procedure proc;
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* word = graph.make(root, SExt32, graph.make(root, Trunc, cCallArgumentValues<int64_t>(proc, root)[0]));
        Value* shifted = graph.make(root, SShr, graph.make(root, Shl, word, graph.int32(root, amount)), graph.int32(root, amount));
        graph.ret(root, graph.make(root, SExt32, graph.make(root, Trunc, shifted)));
        auto code = compileProc(proc, 2);

        unsigned amountTaken = amount & 63;
        for (int32_t input : { 0, 1, -1, 7, 8, -9, 0x12345678, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() }) {
            int64_t expected = static_cast<int64_t>(static_cast<uint64_t>(static_cast<int64_t>(input)) << amountTaken) >> amountTaken;
            CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(input)), static_cast<int64_t>(static_cast<int32_t>(expected)));
        }
    }
}

static void testReduceStrengthKnowsNarrowLoadRange()
{
    for (B3::Opcode opcode : { Load8Z, Load8S, Load16Z, Load16S, Load }) {
        Procedure proc(strengthReductionPolicy());
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* pointer = cCallArgumentValues<int32_t*>(proc, root)[0];
        Value* loaded = root->appendNew<MemoryValue>(proc, opcode, Int32, Origin(), pointer, 0, firstHeap);
        Value* wide = graph.make(root, opcode == Load8S || opcode == Load16S ? SExt32 : ZExt32, loaded);
        Value* digit = graph.make(root, Sub, wide, graph.int64(root, 48));
        Value* fits = graph.make(root, Equal, graph.make(root, SExt32, graph.make(root, Trunc, digit)), digit);
        graph.ret(root, graph.make(root, Add, graph.make(root, ZExt32, fits), graph.make(root, Shl, digit, graph.int32(root, 1))));
        reduceStrengthOnce(proc);
        CHECK_EQ(numberOfValues(proc, Equal), opcode == Load ? 1u : 0u);
        CHECK_EQ(numberOfValues(proc, Trunc), opcode == Load ? 1u : 0u);
        auto code = compileProc(proc);
        for (int32_t word : { 0, 1, 47, 48, 127, 128, 255, 256, 32767, 32768, 65535, 65536, -1, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() }) {
            int64_t value = opcode == Load8Z ? static_cast<uint8_t>(word) : opcode == Load8S ? static_cast<int8_t>(word) : opcode == Load16Z ? static_cast<uint16_t>(word) : opcode == Load16S ? static_cast<int16_t>(word) : static_cast<int64_t>(static_cast<uint32_t>(word));
            int64_t difference = value - 48;
            CHECK_EQ(invoke<int64_t>(*code, &word), (difference == static_cast<int32_t>(difference)) + difference * 2);
        }
    }
}

void addBunRangeTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testRangeForReadsKnownRange());
    RUN(testRangeForIntersectsOwnAndKnownRange());
    RUN(testImplausibleKnownRangeIsIgnored());
    RUN(testRangeForTrunc());
    RUN(testRangeForDepthLimit());
    RUN(testKnownRangeDiesWithOpcode());
    RUN(testUnsignedIntervalFor());
    RUN(testEvaluateComparisonOnRanges());
    RUN(testEvaluateComparisonOfValues());
    RUN(testComparisonsFoldedByIntegerRange());
    RUN(testBoxedInt32ComparisonsFoldByRange());
    RUN(testExtensionOfTruncationFoldsByRange());
    RUN(testSumOfIntegersBehindDoublesIsAddedInIntegers());
    RUN(testSignExtensionOfNonNegativeIsZeroExtension());
    RUN(testComparisonOfExtensionsIsNarrow());
    RUN(testMaskFoldsByRange());
    RUN(testVariableShiftRanges());
    RUN(testTruncOfVariableShiftLeftAndBack());
    RUN(testReduceStrengthKnowsNarrowLoadRange());
    RUN(testReduceStrengthFoldsSelfComparisonOfFiniteDouble());
}

#endif // ENABLE(B3_JIT)
