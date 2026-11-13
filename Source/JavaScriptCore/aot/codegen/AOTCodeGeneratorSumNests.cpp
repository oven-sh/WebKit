/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTOperationContext.h"
#include "AOTVectorOutput.h"
#include <algorithm>
#include <array>
#include <bit>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

struct SumNest {
    struct CountedTest {
        ValueID counter;
        ValueID bound;
        ValueID distance;
        bool isDistanceSubtracted { false };
        int64_t added { 0 };
        bool isAtEnd { false };
        bool mayWrap { false };
    };
    struct Sum {
        ValueID parameter;
        ValueID first;
        ValueID next;
        ValueID result;
        ValueID firstRead;
        ValueID term;
        bool isTermSubtracted { false };
    };
    struct Step {
        enum class Kind : uint8_t { Value, Name, Selection };
        Kind kind { Kind::Value };
        BlockID block;
        ValueID value;
        ValueID source;
        bool isNeeded { false };
    };
    struct Selection {
        struct Side {
            BlockID block;
            std::span<const ValueID> values;
            std::span<const ValueID> passed;
        };
        size_t step { 0 };
        std::array<Side, 2> sides;
        std::span<const ValueID> parameters;
    };
    BlockID head;
    BlockID innerHead;
    ValueID poll;
    CountedTest columns;
    CountedTest terms;
    Vector<BlockID, 8> blocks;
    Vector<BlockID, 4> innerBlocks;
    Vector<std::pair<ValueID, ValueID>, 8> aliases;
    std::span<const ValueID> back;
    std::span<const ValueID> innerEntry;
    std::span<const ValueID> innerBack;
    Vector<Sum, 2> sums;
    bool sumsWords { false };
    Vector<ValueID, 4> reads;
    Vector<ValueID, 4> stores;
    ValueID storedInArray;
    Vector<ValueID, 2> columnReads;
    Vector<std::pair<BlockID, bool>, 8> guards;
    Vector<Step, 32> steps;
    Vector<Selection, 2> selections;
    Vector<std::pair<ValueID, bool>, 4> invariantTests;
    struct ColumnTest {
        ValueID comparison;
        bool answer { true };
        bool isMinimum { false };
    };
    Vector<ColumnTest, 4> columnTests;
    Vector<std::pair<ValueID, bool>, 4> termTests;
    BlockID exit;
};

static constexpr int64_t fewestColumns = 4;
static constexpr int64_t mostColumns = 1 << 20;
static constexpr int64_t fewestTerms = 8;
static constexpr int64_t mostTerms = 1 << 20;
static constexpr size_t mostSums = 2;
static constexpr size_t mostReads = 4;
static constexpr unsigned mostDepth = 16;
static constexpr int64_t greatestConstant = int64_t { 1 } << 31;
static constexpr OpcodeSet namesOfIntegers { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
static constexpr OpcodeSet arrayAccesses { Opcode::GetArrayElement, Opcode::PutArrayElement };
static constexpr OpcodeSet arrayQuestions { Opcode::IsPristine, Opcode::HasArrayStorage, Opcode::GetArrayLength, Opcode::GetArrayStorage };

static unsigned logElementSizeOf(const Operation& access)
{
    return arrayAccesses.contains(access.opcode) ? 3 : logElementSize(access.typedArrayType());
}

static ValueID holderOf(const Operation& access)
{
    return arrayAccesses.contains(access.opcode) ? access.arrayOperand() : access.vectorOperand();
}

struct KnownInteger {
    int64_t constant { 0 };
    bool isConstant { true };
    LValue value { nullptr };
    unsigned trailingZeros { 0 };

    bool isZero() const { return isConstant && !constant; }
    unsigned knownTrailingZeros() const { return isConstant ? constant ? std::countr_zero(static_cast<uint64_t>(constant)) : 63 : trailingZeros; }
};

struct LinearInteger {
    KnownInteger first;
    KnownInteger inner;
    KnownInteger outer;
};

class LinearIntegers {
public:
    LinearIntegers(CodeGenerator& generator, const SumNest& nest, Output* out)
        : m_generator(generator)
        , m_code(generator.code())
        , m_nest(nest)
        , m_out(out)
        , m_checksProofs(generator.emitter<ElementAccessEmitter>().checksBounds())
    {
    }

    std::optional<LinearInteger> find(ValueID, unsigned depth = 0);
    std::optional<LinearInteger> findParameter(ValueID parameter, size_t index, bool isInner, unsigned depth);
    KnownInteger constantOf(int64_t constant) const { return { constant, true, nullptr, 0 }; }
    LValue valueOf(const KnownInteger& integer) const { return integer.isConstant ? m_out->constInt64(integer.constant) : integer.value; }
    KnownInteger add(const KnownInteger&, const KnownInteger&) const;
    KnownInteger multiply(const KnownInteger&, const KnownInteger&) const;
    KnownInteger shiftLeft(const KnownInteger& integer, unsigned amount) const { return multiply(integer, constantOf(int64_t { 1 } << amount)); }
    KnownInteger shiftRight(const KnownInteger&, unsigned amount) const;
    std::optional<LinearInteger> multiply(LinearInteger left, LinearInteger right) const
    {
        if (!right.inner.isZero() || !right.outer.isZero())
            std::swap(left, right);
        if (!right.inner.isZero() || !right.outer.isZero())
            return std::nullopt;
        return LinearInteger { multiply(left.first, right.first), multiply(left.inner, right.first), multiply(left.outer, right.first) };
    }
    KnownInteger negate(const KnownInteger& integer) const { return multiply(integer, constantOf(-1)); }
    LValue wide(LValue value) const { return value->type() == Machine::Int32 ? m_out->signExt32To64(value) : value; }

    Vector<LinearInteger, 8> wrapped;
    WTF::Function<LValue(ValueID)> emitInteger;

    static bool isInteger(const CodeGenerator& generator, ValueID value)
    {
        Representation representation = generator.representationOf(value);
        return (representation == Representation::Int32 || representation == Representation::Int64) && generator.typeOf(value).isInteger();
    }
    static bool mayBeInteger(const CodeGenerator& generator, ValueID value)
    {
        Representation representation = generator.representationOf(value);
        bool isNumber = representation == Representation::Double || representation == Representation::OptionalDouble || representation == Representation::OptionalInt64;
        return isNumber || (representation == Representation::JSValue && generator.typeOf(value).primitives & Primitive::Integer) || isInteger(generator, value);
    }
    static bool isKnownBefore(const CodeGenerator&, const SumNest&, ValueID, unsigned depth = 0);

private:
    static bool isInNest(const CodeGenerator& generator, const SumNest& nest, ValueID value)
    {
        BlockID home = generator.code().isConstant(value) ? BlockID() : generator.code().blockOf(value);
        return home && (nest.blocks.contains(home) || nest.innerBlocks.contains(home));
    }
    bool isInNest(ValueID value) const { return isInNest(m_generator, m_nest, value); }
    bool isInteger(ValueID value) const { return isInteger(m_generator, value); }
    void requireInt32(const LinearInteger& linear, ValueID value)
    {
        Range range = m_generator.typeOf(value).integers;
        if (m_checksProofs || range.minimum < std::numeric_limits<int32_t>::min() || range.maximum > std::numeric_limits<int32_t>::max())
            wrapped.append(linear);
    }
    std::optional<LinearInteger> findAnew(ValueID, unsigned depth);

    CodeGenerator& m_generator;
    const Code& m_code;
    const SumNest& m_nest;
    Output* m_out;
    const bool m_checksProofs;
    Vector<std::pair<ValueID, LinearInteger>, 32> m_known;
};

bool LinearIntegers::isKnownBefore(const CodeGenerator& generator, const SumNest& nest, ValueID value, unsigned depth)
{
    static constexpr OpcodeSet computations { Opcode::Unbox, Opcode::Box, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::GetArrayLength };
    const Code& code = generator.code();
    if (code.isConstant(value))
        return true;
    if (!isInNest(generator, nest, value))
        return generator.m_values[value.index()];
    const Operation& operation = code.at(value);
    if (depth == mostDepth)
        return false;
    if (operation.opcode == Opcode::Narrowed)
        return isKnownBefore(generator, nest, operation.valueOperand(), depth + 1);
    if (!computations.contains(operation.opcode) || (operation.opcode != Opcode::GetArrayLength && !operation.traits().isPure()))
        return false;
    bool isKnown = true;
    operation.forEachChild([&](ValueID operand) {
        isKnown &= isKnownBefore(generator, nest, operand, depth + 1);
    });
    return isKnown;
}

KnownInteger LinearIntegers::add(const KnownInteger& left, const KnownInteger& right) const
{
    if (left.isConstant && right.isConstant)
        return constantOf(left.constant + right.constant);
    if (left.isZero())
        return right;
    if (right.isZero())
        return left;
    return { 0, false, m_out ? m_out->add(valueOf(left), valueOf(right)) : nullptr, std::min(left.knownTrailingZeros(), right.knownTrailingZeros()) };
}

KnownInteger LinearIntegers::multiply(const KnownInteger& left, const KnownInteger& right) const
{
    if (left.isConstant && right.isConstant)
        return constantOf(left.constant * right.constant);
    if (left.isZero() || right.isZero())
        return constantOf(0);
    if (left.isConstant && left.constant == 1)
        return right;
    if (right.isConstant && right.constant == 1)
        return left;
    return { 0, false, m_out ? m_out->mul(valueOf(left), valueOf(right)) : nullptr, std::min(63u, left.knownTrailingZeros() + right.knownTrailingZeros()) };
}

KnownInteger LinearIntegers::shiftRight(const KnownInteger& integer, unsigned amount) const
{
    if (integer.isConstant)
        return constantOf(integer.constant >> amount);
    return { 0, false, m_out ? m_out->aShr(integer.value, static_cast<int>(amount)) : nullptr, integer.trailingZeros > amount ? integer.trailingZeros - amount : 0 };
}

std::optional<LinearInteger> LinearIntegers::findParameter(ValueID parameter, size_t index, bool isInner, unsigned depth)
{
    static constexpr OpcodeSet passingOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32 };
    if (!isInteger(parameter))
        return std::nullopt;
    ValueID next = isInner ? m_nest.innerBack[index] : m_nest.back[index];
    bool mayWrap = false;
    while (!m_code.isConstant(next) && passingOn.contains(m_generator.opcodeOf(next)) && isInteger(next) && isInteger(m_code.at(next).valueOperand())) {
        mayWrap |= m_generator.opcodeOf(next) == Opcode::ToInt32;
        next = m_code.at(next).valueOperand();
    }
    KnownInteger step = constantOf(0);
    if (next != parameter) {
        const Operation& operation = m_code.at(next);
        auto passed = [&](ValueID value) {
            while (!m_code.isConstant(value) && namesOfIntegers.contains(m_generator.opcodeOf(value)))
                value = m_code.at(value).valueOperand();
            return value;
        };
        static constexpr OpcodeSet stepping { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
        if (m_code.isConstant(next) || !stepping.contains(operation.opcode) || operation.form() != OperatorForm::Integers)
            return std::nullopt;
        bool isSubtracted = operation.opcode == Opcode::Decrement || operation.opcode == Opcode::Sub;
        if (operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement) {
            if (passed(operation.valueOperand()) != parameter)
                return std::nullopt;
            step = constantOf(1);
        } else {
            ValueID added = operation.rightOperand();
            if (passed(operation.leftOperand()) != parameter) {
                if (isSubtracted || passed(added) != parameter)
                    return std::nullopt;
                added = operation.leftOperand();
            }
            if (!isInteger(added) || isInNest(passed(added)))
                return std::nullopt;
            std::optional<LinearInteger> linear = find(added, depth + 1);
            if (!linear || !linear->inner.isZero() || !linear->outer.isZero())
                return std::nullopt;
            step = linear->first;
        }
        if (isSubtracted)
            step = negate(step);
    }
    LinearInteger result;
    if (isInner) {
        std::optional<LinearInteger> first = find(m_nest.innerEntry[index], depth + 1);
        if (!first || !first->inner.isZero())
            return std::nullopt;
        result = *first;
        result.inner = step;
    } else {
        result.first = { 0, false, m_out ? wide(m_generator.m_values[parameter.index()]) : nullptr, 0 };
        result.outer = step;
    }
    if (mayWrap) {
        if (!isInner && m_generator.representationOf(parameter) != Representation::Int32 && parameter != m_nest.columns.counter)
            return std::nullopt;
        wrapped.append(result);
    }
    return result;
}

std::optional<LinearInteger> LinearIntegers::find(ValueID value, unsigned depth)
{
    for (bool isChanged = true; isChanged;) {
        isChanged = false;
        for (const auto& [name, source] : m_nest.aliases) {
            if (name != value)
                continue;
            value = source;
            isChanged = true;
        }
    }
    for (const auto& [known, linear] : m_known) {
        if (known == value)
            return linear;
    }
    std::optional<LinearInteger> linear = findAnew(value, depth);
    if (linear)
        m_known.append({ value, *linear });
    return linear;
}

std::optional<LinearInteger> LinearIntegers::findAnew(ValueID value, unsigned depth)
{
    const Operation& operation = m_code.at(value);
    auto constantAt = [&](ValueID id) -> std::optional<int64_t> {
        if (!m_code.isConstant(id) || m_generator.opcodeOf(id) != Opcode::Integer || std::abs(m_code.at(id).integer()) > greatestConstant)
            return std::nullopt;
        return m_code.at(id).integer();
    };
    if (m_code.isConstant(value)) {
        std::optional<int64_t> constant = constantAt(value);
        return constant ? std::optional(LinearInteger { constantOf(*constant), { }, { } }) : std::nullopt;
    }
    if (depth >= mostDepth || !mayBeInteger(m_generator, value))
        return std::nullopt;
    if (!isInNest(value)) {
        if (!m_generator.m_values[value.index()])
            return std::nullopt;
        return LinearInteger { { 0, false, m_out ? emitInteger(value) : nullptr, 0 }, { }, { } };
    }
    if (operation.opcode == Opcode::Parameter) {
        BlockID home = m_code.blockOf(value);
        if (home != m_nest.head && home != m_nest.innerHead)
            return std::nullopt;
        return findParameter(value, m_code.at(home).placeOfParameter(value), home == m_nest.innerHead, depth);
    }
    auto operandAt = [&](ValueID id) { return find(id, depth + 1); };
    switch (operation.opcode) {
    case Opcode::Narrowed:
    case Opcode::WidenInteger:
    case Opcode::NarrowInteger:
    case Opcode::IntegerToNumber:
    case Opcode::NumberToInteger:
    case Opcode::Unbox:
    case Opcode::Box:
        return operandAt(operation.valueOperand());
    case Opcode::ToInt32: {
        ValueID cut = operation.valueOperand();
        if (const Operation& product = m_code.at(cut); product.opcode == Opcode::Mul && product.form() == OperatorForm::CheckedIntegers && m_generator.representationOf(cut) == Representation::Double && isInNest(cut)) {
            std::optional<LinearInteger> left = operandAt(product.leftOperand());
            std::optional<LinearInteger> right = operandAt(product.rightOperand());
            std::optional<LinearInteger> linear = left && right ? multiply(*left, *right) : std::nullopt;
            if (linear)
                wrapped.append(*linear);
            return linear;
        }
        std::optional<LinearInteger> linear = operandAt(cut);
        if (linear)
            requireInt32(*linear, cut);
        return linear;
    }
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul: {
        bool isNumber = m_generator.representationOf(value) == Representation::Double && (operation.form() == OperatorForm::CheckedIntegers || operation.form() == OperatorForm::Numbers);
        bool isChecked = isNumber || (operation.form() == OperatorForm::CheckedIntegers && m_generator.representationOf(value) == Representation::Int32);
        if (operation.form() != OperatorForm::Integers && !isChecked)
            return std::nullopt;
        bool hasOneOperand = operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement;
        std::optional<LinearInteger> left = operandAt(hasOneOperand ? operation.valueOperand() : operation.leftOperand());
        std::optional<LinearInteger> right = hasOneOperand ? LinearInteger { constantOf(1), { }, { } } : operandAt(operation.rightOperand());
        if (!left || !right)
            return std::nullopt;
        LinearInteger result;
        if (operation.opcode == Opcode::Mul) {
            std::optional<LinearInteger> product = multiply(*left, *right);
            if (!product)
                return std::nullopt;
            result = *product;
        } else {
            if (operation.opcode == Opcode::Sub || operation.opcode == Opcode::Decrement)
                right = LinearInteger { negate(right->first), negate(right->inner), negate(right->outer) };
            result = { add(left->first, right->first), add(left->inner, right->inner), add(left->outer, right->outer) };
        }
        if (isChecked)
            wrapped.append(result);
        return result;
    }
    case Opcode::LShift: {
        std::optional<int64_t> amount = constantAt(operation.rightOperand());
        std::optional<LinearInteger> linear = operandAt(operation.leftOperand());
        if (!linear || !amount || *amount < 0 || *amount > 16)
            return std::nullopt;
        requireInt32(*linear, operation.leftOperand());
        LinearInteger result { shiftLeft(linear->first, *amount), shiftLeft(linear->inner, *amount), shiftLeft(linear->outer, *amount) };
        wrapped.append(result);
        return result;
    }
    case Opcode::RShift: {
        std::optional<int64_t> amount = constantAt(operation.rightOperand());
        std::optional<LinearInteger> linear = operandAt(operation.leftOperand());
        if (!linear || !amount || *amount < 0 || *amount > 16)
            return std::nullopt;
        if (linear->inner.knownTrailingZeros() < *amount || linear->outer.knownTrailingZeros() < *amount)
            return std::nullopt;
        requireInt32(*linear, operation.leftOperand());
        return LinearInteger { shiftRight(linear->first, *amount), shiftRight(linear->inner, *amount), shiftRight(linear->outer, *amount) };
    }
    default:
        return std::nullopt;
    }
}

class SumNestFinder {
public:
    using Step = SumNest::Step;
    using CountedTest = SumNest::CountedTest;

    SumNestFinder(CodeGenerator& generator, BlockID head)
        : m_generator(generator)
        , m_code(generator.code())
        , m_checksProofs(generator.emitter<ElementAccessEmitter>().checksBounds())
    {
        m_found.head = head;
    }

    std::optional<SumNest> find();

private:
    const Operation& at(ValueID value) const { return m_code.at(value); }
    Opcode opcodeOf(ValueID value) const { return m_generator.opcodeOf(value); }
    ValueID passed(ValueID value) const
    {
        while (!m_code.isConstant(value) && namesOfIntegers.contains(opcodeOf(value)))
            value = at(value).valueOperand();
        return value;
    }
    bool isPlainBlock(BlockID id) const
    {
        const Block& block = m_code.at(id);
        return m_code.isReached(id) && block.frequency == BlockFrequency::Normal && !m_code.handlerOf(id) && !block.subject && !block.isEntry && !block.isHandlerBlock();
    }
    bool isInInnerLoop(ValueID value) const { return !m_code.isConstant(value) && m_found.innerBlocks.contains(m_code.blockOf(value)); }
    bool isInNest(ValueID value) const { return !m_code.isConstant(value) && (m_found.blocks.contains(m_code.blockOf(value)) || m_found.innerBlocks.contains(m_code.blockOf(value))); }
    bool isAvailable(ValueID value) const { return m_code.isConstant(value) || (!isInNest(value) && m_generator.m_values[value.index()]); }
    bool isInteger(ValueID value) const { return LinearIntegers::isInteger(m_generator, value); }
    bool isKnownBefore(ValueID value) const { return LinearIntegers::isKnownBefore(m_generator, m_found, value); }
    bool isOfCheck(ValueID value, unsigned depth = 0) const
    {
        if (!m_checksProofs || opcodeOf(value) == Opcode::CheckProof)
            return m_checksProofs;
        ValueID user = depth < mostDepth && m_code.useCountOf(value) == 1 ? m_code.onlyUserOf(value) : ValueID();
        return user && isOfCheck(user, depth + 1);
    }
    bool hasNoEffects(ValueID value) const { return at(value).traits().isPure() || isOfCheck(value); }
    std::optional<EdgeID> findTest(BlockID, CountedTest&) const;
    std::optional<EdgeID> findInvariantTest(BlockID);
    std::optional<EdgeID> findTermTest(BlockID);
    bool isArray(ValueID) const;
    ValueID storedNumber(const Operation& store) const;
    bool isCarriedInOtherForm(ValueID parameter) const { return m_generator.isCarriedBoxed(parameter) || m_generator.isCarriedUnboxed(parameter); }
    bool readsArray(const Operation&) const;
    std::optional<EdgeID> findGuard(BlockID);
    bool isEnteredRarely() const;
    ValueID findFirstRead(ValueID) const;
    bool readsView(const Operation&) const;
    bool findCounter(CountedTest&, BlockID head, std::span<const ValueID> back) const;
    bool take(BlockID, ValueID);
    bool follow(const Edge&);
    bool findInnerLoop(const Edge& entry);
    bool followToEnd(BlockID);
    ValueID resolved(ValueID) const;
    bool isColumnTest(BlockID) const;
    bool isMinimum(BlockID) const;
    BlockID takeSelection(BlockID);
    bool isFalseAfterInnerLoop(ValueID comparison) const;
    bool findSums();
    bool findSumsOfWords();
    bool isTerm(ValueID, unsigned depth = 0);
    bool isTermOfWords(ValueID, unsigned depth = 0);
    bool findAccesses();
    bool isComputedAfter(ValueID, unsigned depth = 0) const;
    bool findNeededSteps();
    void findExit();

    CodeGenerator& m_generator;
    const Code& m_code;
    const bool m_checksProofs;
    SumNest m_found;
    Vector<ValueID, 2> m_testedReads;
    BlockID m_blockOfExit;
    BlockID m_exit;
    std::span<const ValueID> m_exitArguments;
    bool m_isBehindTest { false };
};

std::optional<EdgeID> SumNestFinder::findTest(BlockID id, CountedTest& test) const
{
    const Terminator& terminator = m_code.at(id).terminator;
    ValueID condition = terminator.conditionOperand();
    if (m_code.isConstant(condition) || m_code.blockOf(condition) != id)
        return std::nullopt;
    const Operation& comparison = at(condition);
    bool isEquality = comparison.opcode == Opcode::StrictEq || comparison.opcode == Opcode::Eq;
    if (!isEquality && comparison.opcode != Opcode::Less && comparison.opcode != Opcode::LessEq)
        return std::nullopt;
    bool comparesNumbers = comparison.form() == OperatorForm::Numbers;
    if (!comparesNumbers && comparison.form() != OperatorForm::Integers)
        return std::nullopt;
    test.counter = comparison.leftOperand();
    test.bound = comparison.rightOperand();
    test.added = comparison.opcode == Opcode::LessEq;
    if (!isInteger(test.counter) || !(comparesNumbers ? LinearIntegers::mayBeInteger(m_generator, test.bound) : isInteger(test.bound)))
        return std::nullopt;
    const Operation& moved = at(passed(test.counter));
    if (!m_code.isConstant(test.counter) && (moved.opcode == Opcode::Add || moved.opcode == Opcode::Sub) && moved.form() == OperatorForm::Integers) {
        bool isRight = isAvailable(moved.rightOperand());
        if (isRight != isAvailable(moved.leftOperand()) && (isRight || moved.opcode == Opcode::Add)) {
            ValueID distance = isRight ? moved.rightOperand() : moved.leftOperand();
            ValueID counter = isRight ? moved.leftOperand() : moved.rightOperand();
            bool isCounter = opcodeOf(passed(counter)) == Opcode::Parameter && m_code.at(m_code.blockOf(passed(counter))).isLoopHead;
            bool isStep = m_code.isConstant(distance) && opcodeOf(distance) == Opcode::Integer && at(distance).integer() == 1;
            if (isCounter && !isStep && isInteger(distance) && isInteger(counter)) {
                test.counter = counter;
                test.distance = distance;
                test.isDistanceSubtracted = moved.opcode == Opcode::Sub;
            }
        }
    }
    return isEquality ? terminator.notTaken() : terminator.taken();
}

std::optional<EdgeID> SumNestFinder::findInvariantTest(BlockID id)
{
    const Terminator& terminator = m_code.at(id).terminator;
    if (terminator.kind != TerminatorKind::Branch)
        return std::nullopt;
    ValueID condition = terminator.conditionOperand();
    bool isExpected = m_code.at(m_code.at(terminator.notTaken()).to).frequency == BlockFrequency::Rare;
    if (m_code.isConstant(condition) || m_code.blockOf(condition) != id || isExpected == (m_code.at(m_code.at(terminator.taken()).to).frequency == BlockFrequency::Rare))
        return std::nullopt;
    static constexpr OpcodeSet comparisons { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq, Opcode::Eq };
    const Operation& operation = at(condition);
    if (!comparisons.contains(operation.opcode) || operation.form() != OperatorForm::Integers || !isAvailable(operation.leftOperand()) || !isAvailable(operation.rightOperand()))
        return std::nullopt;
    m_found.invariantTests.append({ condition, isExpected });
    return isExpected ? terminator.taken() : terminator.notTaken();
}

ValueID SumNestFinder::storedNumber(const Operation& store) const
{
    auto named = [&](ValueID value) {
        for (bool isChanged = true; isChanged;) {
            auto alias = std::ranges::find(m_found.aliases, value, &std::pair<ValueID, ValueID>::first);
            isChanged = alias != m_found.aliases.end();
            if (isChanged)
                value = alias->second;
        }
        return value;
    };
    ValueID stored = named(store.valueOperand());
    bool isBox = !m_code.isConstant(stored) && m_found.blocks.contains(m_code.blockOf(stored)) && opcodeOf(stored) == Opcode::Box;
    return isBox ? named(at(stored).valueOperand()) : stored;
}

bool SumNestFinder::isArray(ValueID value) const
{
    Representation representation = m_generator.representationOf(value);
    return !m_code.isConstant(value) && (representation == Representation::Cell || representation == Representation::JSValue) && m_generator.typeOf(value).isObject() && isKnownBefore(value);
}

bool SumNestFinder::readsArray(const Operation& read) const
{
    bool mayReadDouble = read.opcode == Opcode::GetArrayElement && (read.level() != KnowledgeLevel::Proven || read.arrayStorage() == ArrayStorage::Double || !findArrayStorageForm(read.arrayStorage()));
    return mayReadDouble && isArray(read.arrayOperand());
}

std::optional<EdgeID> SumNestFinder::findGuard(BlockID id)
{
    static constexpr size_t mostGuards = 12;
    const Terminator& terminator = m_code.at(id).terminator;
    if (terminator.kind != TerminatorKind::Branch || m_found.guards.size() == mostGuards)
        return std::nullopt;
    ValueID condition = terminator.conditionOperand();
    if (m_code.isConstant(condition) || m_code.blockOf(condition) != id)
        return std::nullopt;
    const Operation& test = at(condition);
    if (test.opcode == Opcode::TypeTest && test.testedType() == TypeTable::number && m_found.blocks.contains(m_code.blockOf(condition))) {
        ValueID read = passed(test.valueOperand());
        if (isInNest(read) && opcodeOf(read) == Opcode::GetArrayElement && !isInInnerLoop(read)) {
            m_testedReads.append(read);
            return terminator.taken();
        }
    }
    bool isGuard = false;
    if (test.opcode == Opcode::IsPristine)
        isGuard = test.builtinClass() == BuiltinClassID(BuiltinClasses::Array) && isArray(test.valueOperand());
    else if (test.opcode == Opcode::TypeTest)
        isGuard = isKnownBefore(test.valueOperand());
    else if (test.opcode == Opcode::HasArrayStorage)
        isGuard = test.arrayStorage() == ArrayStorage::Double && !test.data() && isArray(test.arrayOperand());
    else if ((test.opcode == Opcode::Less || test.opcode == Opcode::LessEq) && test.form() == OperatorForm::Integers) {
        bool comparesLength = opcodeOf(passed(test.leftOperand())) == Opcode::GetArrayLength || opcodeOf(passed(test.rightOperand())) == Opcode::GetArrayLength;
        isGuard = comparesLength && isKnownBefore(test.leftOperand()) && isKnownBefore(test.rightOperand());
    }
    if (!isGuard)
        return std::nullopt;
    bool isTaken = m_code.at(m_code.at(terminator.taken()).to).frequency == BlockFrequency::Normal;
    m_found.guards.append({ id, isTaken });
    return isTaken ? terminator.taken() : terminator.notTaken();
}

std::optional<EdgeID> SumNestFinder::findTermTest(BlockID id)
{
    static constexpr size_t mostTests = 4;
    const Terminator& terminator = m_code.at(id).terminator;
    if (terminator.kind != TerminatorKind::Branch || m_found.termTests.size() == mostTests)
        return std::nullopt;
    ValueID condition = terminator.conditionOperand();
    bool isExpected = m_code.at(m_code.at(terminator.notTaken()).to).frequency == BlockFrequency::Rare;
    if (m_code.isConstant(condition) || m_code.blockOf(condition) != id || isExpected == (m_code.at(m_code.at(terminator.taken()).to).frequency == BlockFrequency::Rare))
        return std::nullopt;
    const Operation& operation = at(condition);
    if ((operation.opcode != Opcode::Less && operation.opcode != Opcode::LessEq) || operation.form() != OperatorForm::Integers)
        return std::nullopt;
    m_found.termTests.append({ condition, isExpected });
    return isExpected ? terminator.taken() : terminator.notTaken();
}

bool SumNestFinder::isEnteredRarely() const
{
    bool isRare = true;
    m_code.forEachIncomingEdgeUnsealed(m_found.head, [&](const Edge& edge) {
        if (edge.to != m_found.head || !m_code.isReached(edge.from) || m_found.blocks.contains(edge.from))
            return;
        static constexpr unsigned mostBlocks = 4;
        BlockID from = edge.from;
        for (unsigned number = 0; number < mostBlocks && from && m_code.at(from).frequency == BlockFrequency::Normal; ++number)
            from = m_code.at(from).terminator.kind == TerminatorKind::Jump ? m_generator.m_singlePredecessors[from.index()] : BlockID();
        isRare &= from && m_code.at(from).frequency == BlockFrequency::Rare;
    });
    return isRare;
}

bool SumNestFinder::findCounter(CountedTest& test, BlockID head, std::span<const ValueID> back) const
{
    static constexpr OpcodeSet passingOn { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32 };
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    auto stripped = [&](ValueID value, bool& mayWrap) {
        while (!m_code.isConstant(value) && passingOn.contains(opcodeOf(value))) {
            mayWrap |= opcodeOf(value) == Opcode::ToInt32;
            value = at(value).valueOperand();
        }
        return value;
    };
    bool testMayWrap = false;
    ValueID tested = stripped(test.counter, testMayWrap);
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        bool mayWrap = testMayWrap;
        ValueID next = stripped(back[index], mayWrap);
        bool isNext = next == tested && tested != parameter;
        if ((tested != parameter && !isNext) || (isNext && !test.isAtEnd) || m_code.isConstant(next) || !m_generator.m_values[parameter.index()])
            continue;
        const Operation& step = at(next);
        bool isIncrement = step.opcode == Opcode::Increment && step.form() == OperatorForm::Integers && passed(step.valueOperand()) == parameter;
        bool isAddition = step.opcode == Opcode::Add && step.form() == OperatorForm::Integers && passed(step.leftOperand()) == parameter && m_code.isConstant(step.rightOperand()) && opcodeOf(step.rightOperand()) == Opcode::Integer && at(step.rightOperand()).integer() == 1;
        if ((!isIncrement && !isAddition) || (mayWrap && (test.added || m_generator.typeOf(test.bound).integers.maximum > std::numeric_limits<int32_t>::max())))
            continue;
        test.counter = parameter;
        test.added += test.isAtEnd && !isNext;
        test.mayWrap = mayWrap;
        return true;
    }
    return false;
}

bool SumNestFinder::take(BlockID block, ValueID value)
{
    const Operation& operation = at(value);
    if (isNeverType(operation.type))
        return false;
    if (operation.opcode == Opcode::LoopHint) {
        if (m_found.poll || m_found.innerHead || operation.throwsOwedError())
            return false;
        m_found.poll = value;
        return true;
    }
    if (operation.opcode == Opcode::PutTypedArrayElement) {
        const Type& stored = m_generator.typeOf(operation.valueOperand());
        if (operation.typedArrayType() == NotTypedArray || operation.level() != KnowledgeLevel::Proven || stored.hasObjects() || stored.primitives & cellPrimitives)
            return false;
        m_found.stores.append(value);
    } else if (operation.opcode == Opcode::PutArrayElement) {
        if (!isArray(operation.arrayOperand()) || m_generator.representationOf(storedNumber(operation)) != Representation::Double)
            return false;
        m_found.stores.append(value);
    } else if (operation.opcode == Opcode::GetArrayElement) {
        if (!readsArray(operation))
            return false;
    } else if (operation.opcode == Opcode::GetTypedArrayElement) {
        if (operation.typedArrayType() == NotTypedArray || operation.level() != KnowledgeLevel::Proven || (operation.lengthOperands().empty() && !operation.isInsideView()))
            return false;
    } else if (arrayQuestions.contains(operation.opcode))
        return isArray(operation.operand(0));
    else if (!hasNoEffects(value))
        return false;
    m_found.steps.append({ Step::Kind::Value, block, value, { }, false });
    return true;
}

bool SumNestFinder::findInnerLoop(const Edge& entry)
{
    static constexpr size_t mostBlocks = 4;
    BlockID head = entry.to;
    if (!isPlainBlock(head) || entry.arguments().size() != m_code.at(head).parameters().size())
        return false;
    m_found.innerHead = head;
    m_found.innerEntry = entry.arguments();
    bool hasTest = false;
    BlockID latch;
    for (BlockID block = head; !latch;) {
        if (m_found.innerBlocks.size() == mostBlocks || m_found.blocks.contains(block) || m_found.innerBlocks.contains(block))
            return false;
        m_found.innerBlocks.append(block);
        for (ValueID value : m_code.valuesOf(block)) {
            const Operation& operation = at(value);
            if (isOfCheck(value)) [[unlikely]]
                continue;
            if (!namesOfIntegers.contains(operation.opcode))
                m_found.terms.isAtEnd = false;
            if (operation.opcode == Opcode::GetTypedArrayElement) {
                if (operation.typedArrayType() == NotTypedArray || operation.level() != KnowledgeLevel::Proven)
                    return false;
            } else if (operation.opcode == Opcode::GetArrayElement) {
                if (!readsArray(operation))
                    return false;
            } else if (operation.opcode == Opcode::LoopHint ? operation.throwsOwedError() : !hasNoEffects(value) || isNeverType(operation.type))
                return false;
        }
        const Terminator& terminator = m_code.at(block).terminator;
        EdgeID next;
        if (terminator.kind == TerminatorKind::Jump)
            next = terminator.next();
        else if (std::optional<EdgeID> usual = findInvariantTest(block))
            next = *usual;
        else if (std::optional<EdgeID> usual = hasTest ? findTermTest(block) : std::nullopt)
            next = *usual;
        else if (terminator.kind == TerminatorKind::Branch && !hasTest) {
            std::optional<EdgeID> stays = findTest(block, m_found.terms);
            if (!stays)
                return false;
            hasTest = true;
            next = *stays;
            const Edge& exit = m_code.at(next == terminator.taken() ? terminator.notTaken() : terminator.taken());
            m_blockOfExit = block;
            m_exit = exit.to;
            m_exitArguments = exit.arguments();
            m_found.terms.isAtEnd = true;
        } else
            return false;
        const Edge& edge = m_code.at(next);
        if (edge.to == head) {
            latch = block;
            m_found.innerBack = edge.arguments();
            break;
        }
        if (!isPlainBlock(edge.to) || m_code.at(edge.to).isLoopHead || !edge.arguments().empty() || m_generator.m_singlePredecessors[edge.to.index()] != block)
            return false;
        block = edge.to;
    }
    if (!hasTest || m_found.innerBack.size() != m_found.innerEntry.size())
        return false;
    unsigned numberOfEntries = 0;
    m_code.forEachIncomingEdgeUnsealed(head, [&](const Edge& edge) {
        numberOfEntries += edge.to == head && m_code.isReached(edge.from) && edge.from != latch;
    });
    return numberOfEntries == 1 && findCounter(m_found.terms, head, m_found.innerBack);
}

bool SumNestFinder::follow(const Edge& edge)
{
    std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
    if (!isPlainBlock(edge.to) || m_code.at(edge.to).isLoopHead || edge.arguments().size() != parameters.size() || m_found.blocks.contains(edge.to) || m_found.innerBlocks.contains(edge.to))
        return false;
    for (size_t index = 0; index < parameters.size(); ++index) {
        m_found.aliases.append({ parameters[index], edge.arguments()[index] });
        m_found.steps.append({ Step::Kind::Name, edge.from, parameters[index], edge.arguments()[index], false });
    }
    return true;
}

ValueID SumNestFinder::resolved(ValueID value) const
{
    for (bool isChanged = true; isChanged;) {
        isChanged = false;
        value = passed(value);
        for (const auto& [name, source] : m_found.aliases) {
            if (name != value)
                continue;
            value = source;
            isChanged = true;
        }
    }
    return value;
}

bool SumNestFinder::isColumnTest(BlockID id) const
{
    static constexpr size_t mostTests = 4;
    ValueID condition = m_code.at(id).terminator.conditionOperand();
    if (m_found.columnTests.size() == mostTests || m_code.isConstant(condition) || m_code.blockOf(condition) != id)
        return false;
    const Operation& comparison = at(condition);
    return (comparison.opcode == Opcode::Less || comparison.opcode == Opcode::LessEq) && comparison.form() == OperatorForm::Integers;
}

bool SumNestFinder::isMinimum(BlockID id) const
{
    const Terminator& terminator = m_code.at(id).terminator;
    const Operation& comparison = at(terminator.conditionOperand());
    std::array<const Edge*, 2> edges { };
    for (uint32_t side = 0; side < 2; ++side) {
        const Edge& into = m_code.at(terminator.edge(side));
        const Block& arm = m_code.at(into.to);
        bool namesAlone = std::ranges::all_of(m_code.valuesOf(into.to), [&](ValueID value) { return namesOfIntegers.contains(opcodeOf(value)); });
        if (!into.arguments().empty() || !namesAlone || arm.terminator.kind != TerminatorKind::Jump || m_generator.m_singlePredecessors[into.to.index()] != id)
            return false;
        edges[side] = &m_code.at(arm.terminator.next());
    }
    return edges[0]->to == edges[1]->to && edges[0]->arguments().size() == 1 && edges[1]->arguments().size() == 1 && resolved(edges[0]->arguments()[0]) == resolved(comparison.leftOperand()) && resolved(edges[1]->arguments()[0]) == resolved(comparison.rightOperand());
}

BlockID SumNestFinder::takeSelection(BlockID id)
{
    static constexpr size_t mostSelections = 2;
    static constexpr size_t mostValues = 4;
    static constexpr OpcodeSet comparisons { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::StrictEq, Opcode::Eq };
    const Terminator& terminator = m_code.at(id).terminator;
    ValueID condition = terminator.conditionOperand();
    if (m_found.selections.size() == mostSelections || m_code.isConstant(condition) || m_code.blockOf(condition) != id)
        return { };
    const Operation& comparison = at(condition);
    if (!comparisons.contains(comparison.opcode) || (comparison.form() != OperatorForm::Integers && comparison.form() != OperatorForm::Numbers))
        return { };
    SumNest::Selection selection;
    BlockID join;
    for (uint32_t side = 0; side < 2; ++side) {
        const Edge* edge = &m_code.at(terminator.edge(side));
        SumNest::Selection::Side& found = selection.sides[side];
        found.block = id;
        const Block& arm = m_code.at(edge->to);
        if (m_generator.m_singlePredecessors[edge->to.index()] == id) {
            bool isPure = std::ranges::all_of(m_code.valuesOf(edge->to), [&](ValueID value) { return hasNoEffects(value) && !isNeverType(at(value).type); });
            if (!isPlainBlock(edge->to) || arm.isLoopHead || !edge->arguments().empty() || !arm.parameters().empty() || arm.terminator.kind != TerminatorKind::Jump || !isPure || m_code.valuesOf(edge->to).size() > mostValues)
                return { };
            found.block = edge->to;
            found.values = m_code.valuesOf(edge->to);
            edge = &m_code.at(arm.terminator.next());
        }
        found.passed = edge->arguments();
        if (side && join != edge->to)
            return { };
        join = edge->to;
    }
    selection.parameters = m_code.at(join).parameters();
    bool isJoin = isPlainBlock(join) && !m_code.at(join).isLoopHead && join != m_found.head && !m_found.blocks.contains(join) && !selection.parameters.empty();
    if (!isJoin || std::ranges::any_of(selection.sides, [&](const SumNest::Selection::Side& side) { return side.passed.size() != selection.parameters.size(); }))
        return { };
    if (std::ranges::any_of(selection.parameters, [&](ValueID parameter) { return isCarriedInOtherForm(parameter); }))
        return { };
    selection.step = m_found.steps.size();
    for (const SumNest::Selection::Side& side : selection.sides) {
        if (side.block != id)
            m_found.blocks.append(side.block);
    }
    m_found.steps.append({ Step::Kind::Selection, id, condition, { }, false });
    m_found.selections.append(selection);
    return join;
}

bool SumNestFinder::isFalseAfterInnerLoop(ValueID comparison) const
{
    const SumNest::CountedTest& terms = m_found.terms;
    const Operation& operation = at(comparison);
    return m_found.innerHead && !terms.isAtEnd && !terms.added && !terms.distance && operation.opcode == Opcode::Less && passed(operation.leftOperand()) == terms.counter && resolved(operation.rightOperand()) == resolved(terms.bound);
}

bool SumNestFinder::followToEnd(BlockID block)
{
    static constexpr size_t mostBlocks = 24;
    while (m_found.blocks.size() < mostBlocks) {
        m_found.blocks.append(block);
        for (ValueID value : m_code.valuesOf(block)) {
            if (m_isBehindTest ? !hasNoEffects(value) : !take(block, value))
                return false;
        }
        const Terminator& terminator = m_code.at(block).terminator;
        auto goOn = [&](EdgeID id) {
            const Edge& edge = m_code.at(id);
            if (edge.to == m_found.head) {
                m_found.back = edge.arguments();
                return m_found.innerHead && m_found.columns.bound;
            }
            if (!m_code.at(edge.to).isLoopHead)
                return follow(edge) && followToEnd(edge.to);
            if (m_found.innerHead || m_isBehindTest || !findInnerLoop(edge))
                return false;
            std::span<const ValueID> parameters = m_code.at(m_exit).parameters();
            if (!isPlainBlock(m_exit) || m_code.at(m_exit).isLoopHead || m_exitArguments.size() != parameters.size() || m_found.blocks.contains(m_exit) || m_exit == m_found.head)
                return false;
            for (size_t index = 0; index < parameters.size(); ++index) {
                m_found.aliases.append({ parameters[index], m_exitArguments[index] });
                m_found.steps.append({ Step::Kind::Name, m_blockOfExit, parameters[index], m_exitArguments[index], false });
            }
            return followToEnd(m_exit);
        };
        if (terminator.kind == TerminatorKind::Jump)
            return goOn(terminator.next());
        if (terminator.kind != TerminatorKind::Branch)
            return false;
        if (std::optional<EdgeID> usual = m_isBehindTest ? std::nullopt : findInvariantTest(block))
            return goOn(*usual);
        if (std::optional<EdgeID> usual = m_isBehindTest ? std::nullopt : findGuard(block))
            return goOn(*usual);
        if (!m_isBehindTest && m_code.blockOf(terminator.conditionOperand()) == block && isFalseAfterInnerLoop(terminator.conditionOperand()))
            return goOn(terminator.notTaken());
        SumNest before = m_found;
        bool isBeforeInnerLoop = !m_found.innerHead;
        if (!m_found.columns.bound && !(isBeforeInnerLoop && !m_found.stores.isEmpty())) {
            std::optional<EdgeID> stays = findTest(block, m_found.columns);
            bool isCounter = stays && (!isBeforeInnerLoop || m_code.blockOf(passed(m_found.columns.counter)) == m_found.head);
            if (isCounter) {
                m_found.columns.isAtEnd = !isBeforeInnerLoop;
                m_isBehindTest = !isBeforeInnerLoop;
                const Edge& exit = m_code.at(*stays == terminator.taken() ? terminator.notTaken() : terminator.taken());
                if (m_isBehindTest && exit.arguments().empty() && m_code.at(exit.to).parameters().empty() && m_generator.m_singlePredecessors[exit.to.index()] == block && !m_code.at(exit.to).isLoopHead && m_generator.m_blocks[exit.to.index()])
                    m_found.exit = exit.to;
                if (goOn(*stays))
                    return true;
                m_isBehindTest = false;
            }
            m_found = before;
        }
        if (m_found.innerHead && !m_isBehindTest) {
            if (BlockID join = takeSelection(block))
                return followToEnd(join);
        }
        if (m_isBehindTest || !isColumnTest(block))
            return false;
        for (bool answer : { true, false }) {
            m_found.columnTests.append({ terminator.conditionOperand(), answer, isMinimum(block) });
            if (goOn(answer ? terminator.taken() : terminator.notTaken()))
                return true;
            m_found = before;
        }
        return false;
    }
    return false;
}

bool SumNestFinder::readsView(const Operation& read) const
{
    static constexpr std::array kinds { TypeInt8, TypeUint8, TypeUint8Clamped, TypeInt16, TypeUint16, TypeInt32, TypeFloat32, TypeFloat64 };
    if (!std::ranges::contains(kinds, read.typedArrayType()) || !read.vectorOperand() || !isAvailable(read.vectorOperand()) || !isAvailable(read.viewOperand()))
        return false;
    return read.isInsideView() || (!read.lengthOperands().empty() && isAvailable(read.lengthOperands()[0]));
}

ValueID SumNestFinder::findFirstRead(ValueID value) const
{
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Unbox, Opcode::ToNumber };
    while (!m_code.isConstant(value) && names.contains(opcodeOf(value)) && (opcodeOf(value) != Opcode::ToNumber || at(value).form() == OperatorForm::Numbers))
        value = at(value).valueOperand();
    size_t place = m_found.steps.findIf([&](const Step& step) { return step.value == value; });
    if (place == notFound || (opcodeOf(value) == Opcode::GetTypedArrayElement ? !readsView(at(value)) : opcodeOf(value) != Opcode::GetArrayElement))
        return { };
    bool isAfterStore = std::ranges::any_of(m_found.stores, [&](ValueID store) {
        return m_found.steps.findIf([&](const Step& step) { return step.value == store; }) < place;
    });
    return isAfterStore ? ValueID() : value;
}

bool SumNestFinder::isTerm(ValueID value, unsigned depth)
{
    if (m_code.isConstant(value) && (opcodeOf(value) == Opcode::Integer || opcodeOf(value) == Opcode::Double))
        return true;
    if (isAvailable(value))
        return m_generator.representationOf(value) == Representation::Double;
    if (depth == mostDepth || !isInInnerLoop(value))
        return false;
    const Operation& operation = at(value);
    switch (operation.opcode) {
    case Opcode::Narrowed:
    case Opcode::Unbox:
        return isTerm(operation.valueOperand(), depth + 1);
    case Opcode::ToNumber:
        return operation.form() == OperatorForm::Numbers && isTerm(operation.valueOperand(), depth + 1);
    case Opcode::IntegerToNumber:
        return opcodeOf(passed(operation.valueOperand())) == Opcode::GetTypedArrayElement && isTerm(operation.valueOperand(), depth + 1);
    case Opcode::GetArrayElement:
    case Opcode::GetTypedArrayElement: {
        if (operation.opcode == Opcode::GetArrayElement ? !readsArray(operation) : !readsView(operation))
            return false;
        if (!m_found.reads.contains(value))
            m_found.reads.append(value);
        return m_found.reads.size() <= mostReads;
    }
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
        return operation.form() == OperatorForm::Numbers && m_generator.representationOf(value) == Representation::Double && isTerm(operation.leftOperand(), depth + 1) && isTerm(operation.rightOperand(), depth + 1);
    default:
        return false;
    }
}

static constexpr OpcodeSet namesOfWords { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32 };

bool SumNestFinder::isTermOfWords(ValueID value, unsigned depth)
{
    if (m_code.isConstant(value))
        return opcodeOf(value) == Opcode::Integer;
    if (isAvailable(value))
        return m_generator.representationOf(value) == Representation::Int32;
    if (depth == mostDepth || !isInInnerLoop(value))
        return false;
    const Operation& operation = at(value);
    switch (operation.opcode) {
    case Opcode::Narrowed:
    case Opcode::WidenInteger:
    case Opcode::NarrowInteger:
    case Opcode::ToInt32:
    case Opcode::IntegerToNumber:
    case Opcode::NumberToInteger:
    case Opcode::Box:
    case Opcode::Unbox:
        return isTermOfWords(operation.valueOperand(), depth + 1);
    case Opcode::ToNumber:
        return operation.form() == OperatorForm::Numbers && isTermOfWords(operation.valueOperand(), depth + 1);
    case Opcode::GetTypedArrayElement: {
        if (!readsView(operation) || isFloat(operation.typedArrayType()))
            return false;
        if (!m_found.reads.contains(value))
            m_found.reads.append(value);
        return m_found.reads.size() <= mostReads;
    }
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
        return operation.form() == OperatorForm::Integers && isTermOfWords(operation.leftOperand(), depth + 1) && isTermOfWords(operation.rightOperand(), depth + 1);
    case Opcode::CallInline: {
        bool isProduct = operation.inlineBuiltin() == InlineBuiltin::MathImul;
        operation.forEachChild([&](ValueID operand) {
            isProduct = isProduct && m_generator.representationOf(operand) == Representation::Int32 && isTermOfWords(operand, depth + 1);
        });
        return isProduct;
    }
    default:
        return false;
    }
}

bool SumNestFinder::findSumsOfWords()
{
    std::span<const ValueID> parameters = m_code.at(m_found.innerHead).parameters();
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (parameter == m_found.terms.counter || !m_generator.m_values[parameter.index()] || m_generator.representationOf(parameter) != Representation::Int32)
            continue;
        Vector<ValueID, 8> chain;
        auto stripped = [&](ValueID value) {
            for (; isInInnerLoop(value) && namesOfWords.contains(opcodeOf(value)); value = at(value).valueOperand())
                chain.append(value);
            return value;
        };
        ValueID next = m_found.innerBack[index];
        ValueID sum = stripped(next);
        if (!isInInnerLoop(sum) || m_generator.representationOf(next) != Representation::Int32)
            continue;
        const Operation& addition = at(sum);
        if ((addition.opcode != Opcode::Add && addition.opcode != Opcode::Sub) || addition.form() != OperatorForm::Integers)
            continue;
        chain.append(sum);
        bool isLeft = stripped(addition.leftOperand()) == parameter;
        if (!isLeft && (addition.opcode == Opcode::Sub || stripped(addition.rightOperand()) != parameter))
            continue;
        ValueID term = isLeft ? addition.rightOperand() : addition.leftOperand();
        size_t numberOfReads = m_found.reads.size();
        if (!isTermOfWords(term) || m_found.reads.size() == numberOfReads) {
            m_found.reads.shrink(numberOfReads);
            continue;
        }
        ValueID first = m_found.innerEntry[index];
        if (!isAvailable(first) || m_generator.representationOf(first) != Representation::Int32)
            return false;
        for (BlockID block : m_found.innerBlocks) {
            for (ValueID value : m_code.valuesOf(block)) {
                bool readsSum = false;
                at(value).forEachChild([&](ValueID operand) {
                    readsSum |= operand == parameter || chain.contains(operand);
                });
                if (readsSum && !chain.contains(value) && !isOfCheck(value))
                    return false;
            }
        }
        m_found.sums.append({ parameter, first, next, m_found.terms.isAtEnd ? next : parameter, { }, term, addition.opcode == Opcode::Sub });
    }
    m_found.sumsWords = true;
    return !m_found.sums.isEmpty();
}

bool SumNestFinder::findSums()
{
    std::span<const ValueID> parameters = m_code.at(m_found.innerHead).parameters();
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (!m_generator.m_values[parameter.index()] || m_generator.isCarriedBoxed(parameter) || m_generator.representationOf(parameter) != Representation::Double) {
            if (m_generator.isCarriedBoxed(parameter))
                return false;
            continue;
        }
        ValueID first = m_found.innerEntry[index];
        ValueID next = m_found.innerBack[index];
        ValueID firstRead = isAvailable(first) ? ValueID() : findFirstRead(first);
        if ((!isAvailable(first) && !firstRead) || m_code.isConstant(next) || !isInInnerLoop(next) || m_generator.representationOf(next) != Representation::Double)
            return false;
        const Operation& addition = at(next);
        if ((addition.opcode != Opcode::Add && addition.opcode != Opcode::Sub) || addition.form() != OperatorForm::Numbers)
            return false;
        bool isLeft = addition.leftOperand() == parameter;
        if (!isLeft && (addition.opcode == Opcode::Sub || addition.rightOperand() != parameter))
            return false;
        if (!isTerm(isLeft ? addition.rightOperand() : addition.leftOperand()))
            return false;
        for (BlockID block : m_found.innerBlocks) {
            for (ValueID value : m_code.valuesOf(block)) {
                bool readsSum = false;
                at(value).forEachChild([&](ValueID operand) {
                    readsSum |= operand == parameter || operand == next;
                });
                if (readsSum && value != next)
                    return false;
            }
        }
        m_found.sums.append({ parameter, first, next, m_found.terms.isAtEnd ? next : parameter, firstRead, isLeft ? addition.rightOperand() : addition.leftOperand(), addition.opcode == Opcode::Sub });
    }
    if (m_found.sums.isEmpty() && !findSumsOfWords())
        return false;
    for (BlockID block : m_found.innerBlocks) {
        if (std::ranges::any_of(m_code.valuesOf(block), [&](ValueID value) { return opcodeOf(value) == Opcode::GetArrayElement && !m_found.reads.contains(value); }))
            return false;
    }
    return !m_found.sums.isEmpty() && m_found.sums.size() <= mostSums && !m_found.reads.isEmpty();
}

bool SumNestFinder::isComputedAfter(ValueID value, unsigned depth) const
{
    if (isAvailable(value) || std::ranges::any_of(m_found.sums, [&](const SumNest::Sum& sum) { return sum.result == value; }))
        return true;
    if (opcodeOf(value) == Opcode::GetArrayElement)
        return false;
    if (opcodeOf(value) == Opcode::Parameter && m_code.blockOf(value) == m_found.head)
        return m_generator.m_values[value.index()];
    size_t place = m_found.steps.findIf([&](const Step& step) { return step.value == value && step.kind != Step::Kind::Selection; });
    if (place == notFound || depth == mostDepth)
        return false;
    if (m_found.steps[place].kind == Step::Kind::Name)
        return isComputedAfter(m_found.steps[place].source, depth + 1);
    bool isComputed = true;
    at(value).forEachChild([&](ValueID operand) {
        isComputed &= isComputedAfter(operand, depth + 1);
    });
    return isComputed;
}

bool SumNestFinder::findNeededSteps()
{
    auto isResult = [&](ValueID value) { return std::ranges::any_of(m_found.sums, [&](const SumNest::Sum& sum) { return sum.result == value; }); };
    bool isComplete = true;
    auto need = [&](ValueID value) {
        if (m_code.isConstant(value) || isResult(value))
            return;
        for (const SumNest::Selection& selection : m_found.selections) {
            if (!std::ranges::contains(selection.parameters, value))
                continue;
            m_found.steps[selection.step].isNeeded = true;
            return;
        }
        size_t place = m_found.steps.findIf([&](const Step& step) { return step.value == value && step.kind != Step::Kind::Selection; });
        if (place != notFound) {
            m_found.steps[place].isNeeded = true;
            return;
        }
        bool isParameter = opcodeOf(value) == Opcode::Parameter && m_code.blockOf(value) == m_found.head && m_generator.m_values[value.index()];
        isComplete &= isParameter || isAvailable(value);
    };
    bool readsBehind = false;
    for (size_t place = m_found.steps.size(); place--;) {
        Step& step = m_found.steps[place];
        if (step.kind == Step::Kind::Name) {
            if (step.isNeeded) {
                isComplete &= !isCarriedInOtherForm(step.value);
                need(step.source);
            }
            continue;
        }
        if (step.kind == Step::Kind::Selection) {
            if (!step.isNeeded)
                continue;
            at(step.value).forEachChild(need);
            size_t selected = m_found.selections.findIf([&](const SumNest::Selection& selection) { return selection.step == place; });
            for (const SumNest::Selection::Side& side : m_found.selections[selected].sides) {
                auto needOutside = [&](ValueID value) {
                    if (!std::ranges::contains(side.values, value))
                        need(value);
                };
                for (ValueID value : side.values)
                    at(value).forEachChild(needOutside);
                for (ValueID value : side.passed)
                    needOutside(value);
            }
            continue;
        }
        const Operation& operation = at(step.value);
        if (operation.opcode == Opcode::CheckProof)
            step.isNeeded = isComputedAfter(operation.valueOperand());
        if (operation.opcode == Opcode::PutArrayElement) {
            step.isNeeded = true;
            m_found.storedInArray = storedNumber(operation);
            need(m_found.storedInArray);
            continue;
        }
        if (operation.opcode == Opcode::PutTypedArrayElement) {
            bool isReplaced = !readsBehind && std::ranges::any_of(m_found.stores, [&](ValueID store) {
                size_t later = m_found.steps.findIf([&](const Step& other) { return other.value == store; });
                const Operation& other = at(store);
                return later > place && m_found.steps[later].isNeeded && other.viewOperand() == operation.viewOperand() && other.positionOperand() == operation.positionOperand() && other.typedArrayType() == operation.typedArrayType();
            });
            step.isNeeded = !isReplaced;
        }
        if (!step.isNeeded)
            continue;
        readsBehind |= operation.opcode == Opcode::GetTypedArrayElement;
        if (operation.opcode == Opcode::GetArrayElement) {
            bool isNumber = m_generator.representationOf(step.value) == Representation::Double || m_generator.representationOf(step.value) == Representation::OptionalDouble;
            isComplete &= isNumber && m_found.storedInArray && m_found.columnReads.size() < mostReads;
            m_found.columnReads.append(step.value);
            continue;
        }
        operation.forEachChild(need);
    }
    LinearIntegers integers(m_generator, m_found, nullptr);
    for (ValueID read : m_found.columnReads) {
        std::optional<LinearInteger> position = isComplete ? integers.find(at(read).positionOperand()) : std::nullopt;
        isComplete &= position && position->inner.isZero();
    }
    for (ValueID read : m_testedReads)
        isComplete &= m_found.columnReads.contains(read) || std::ranges::contains(m_found.sums, read, &SumNest::Sum::firstRead);
    return isComplete;
}

bool SumNestFinder::findAccesses()
{
    LinearIntegers integers(m_generator, m_found, nullptr);
    for (ValueID read : m_found.reads) {
        if (!integers.find(at(read).positionOperand()))
            return false;
    }
    for (ValueID store : m_found.stores) {
        const Operation& operation = at(store);
        if (operation.opcode == Opcode::PutArrayElement ? m_found.stores.size() > 1 : !operation.vectorOperand() || !isAvailable(operation.vectorOperand()) || !isAvailable(operation.viewOperand()))
            return false;
        std::optional<LinearInteger> position = integers.find(operation.positionOperand());
        if (!position || !position->inner.isZero())
            return false;
    }
    for (const SumNest::Sum& sum : m_found.sums) {
        std::optional<LinearInteger> position = sum.firstRead ? integers.find(at(sum.firstRead).positionOperand()) : std::nullopt;
        if (sum.firstRead && (!position || !position->inner.isZero() || !position->outer.isConstant || position->outer.isZero()))
            return false;
    }
    for (auto [test, answer] : m_found.termTests) {
        if (!integers.find(at(test).leftOperand()) || !integers.find(at(test).rightOperand()))
            return false;
    }
    for (const SumNest::ColumnTest& test : m_found.columnTests) {
        for (ValueID operand : { at(test.comparison).leftOperand(), at(test.comparison).rightOperand() }) {
            std::optional<LinearInteger> integer = integers.find(operand);
            if (!integer || !integer->inner.isZero())
                return false;
        }
    }
    std::span<const ValueID> parameters = m_code.at(m_found.head).parameters();
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        if (m_generator.m_values[parameter.index()] && (m_generator.isCarriedBoxed(parameter) || !integers.find(parameter)))
            return false;
    }
    std::optional<LinearInteger> bound = integers.find(m_found.terms.bound);
    if (!bound || !bound->inner.isZero() || !bound->outer.isZero() || !integers.find(m_found.columns.bound))
        return false;
    size_t indexOfCounter = m_code.at(m_found.innerHead).placeOfParameter(m_found.terms.counter);
    std::optional<LinearInteger> first = integers.find(m_found.innerEntry[indexOfCounter]);
    return first && first->inner.isZero() && first->outer.isZero();
}

void SumNestFinder::findExit()
{
    if (!m_found.exit)
        return;
    Vector<BlockID, 16> blocks;
    blocks.append(m_found.blocks.span());
    blocks.append(m_found.innerBlocks.span());
    std::ranges::sort(blocks);
    Vector<ValueID, 32> values;
    for (BlockID block : blocks) {
        values.append(m_code.at(block).parameters());
        values.append(m_code.valuesOf(block));
    }
    std::ranges::sort(values);
    if (!m_generator.findValuesReadOutside(blocks.span(), values.span()).isEmpty())
        m_found.exit = { };
}

std::optional<SumNest> SumNestFinder::find()
{
    const Block& head = m_code.at(m_found.head);
    if (!isPlainBlock(m_found.head) || !head.isLoopHead || head.parameters().empty())
        return std::nullopt;
    if (!followToEnd(m_found.head) || m_found.back.size() != head.parameters().size() || m_found.stores.isEmpty() || isEnteredRarely())
        return std::nullopt;
    m_found.terms.bound = resolved(m_found.terms.bound);
    if (!findCounter(m_found.columns, m_found.head, m_found.back) || !isAvailable(m_found.columns.bound))
        return std::nullopt;
    if (std::ranges::any_of(std::array { m_found.columns.distance, m_found.terms.distance }, [&](ValueID distance) { return distance && !isAvailable(distance); }))
        return std::nullopt;
    if (!findSums() || !findAccesses() || !findNeededSteps())
        return std::nullopt;
    findExit();
    return WTF::move(m_found);
}

class SumNestEmitter {
public:
    using Step = SumNest::Step;

    using ContextOfEnd = TerminatorContext (*)(const CodeGenerator&, BlockID);

    SumNestEmitter(CodeGenerator& generator, const SumNest& nest, ContextOfEnd contextOfEnd)
        : m_generator(generator)
        , m_code(generator.code())
        , m_out(generator.m_out)
        , m_vectors(generator.m_out)
        , m_elements(generator.emitter<ElementAccessEmitter>())
        , m_nest(nest)
        , m_integers(generator, nest, &generator.m_out)
        , m_contextOfEnd(contextOfEnd)
    {
        m_integers.emitInteger = [this](ValueID value) { return emitInteger(value); };
    }

    void emit();

private:
    struct Read {
        ValueID access;
        LinearInteger position;
        LValue address { nullptr };
        KnownInteger bytesPerTerm;
        KnownInteger bytesPerColumn;
        Machine::AbstractHeap heap;
        Vector<LValue, 4> distances;
        LValue pointer { nullptr };

        bool isShared() const { return bytesPerColumn.isZero(); }
    };
    struct AddressRange {
        LValue first { nullptr };
        LValue end { nullptr };
    };
    struct Array {
        ValueID value;
        bool isStored { false };
        LValue storage { nullptr };
        LValue length { nullptr };
    };
    void leaveUnless(LValue condition, LValue column);
    void emitExits();
    void name(ValueID, LValue);
    LValue numberOfIterations(const SumNest::CountedTest&, LValue first);
    std::pair<LValue, LValue> extremesOf(const LinearInteger&);
    void requireInt32(const LinearInteger&);
    LValue columnsWithAnswer(const SumNest::ColumnTest&, LValue columns);
    void requireInt32(const SumNest::CountedTest&, const LinearInteger& counter);
    AddressRange addressesOf(ValueID access, const LinearInteger& position);
    Read readAt(ValueID access);
    Operand emitBefore(ValueID);
    LValue emitInteger(ValueID);
    void emitGuards();
    const Array& requireDoubles(const Operation& access);
    void emitArrayStore(ValueID);
    void emitColumnRead(ValueID);
    void emitFront();
    void emitTile(int64_t width, LValue column);
    unsigned lanes() const { return m_nest.sumsWords ? 4 : 2; }
    LValue emitSplat(ValueID);
    LValue emitWord(const Read&, LValue address, ptrdiff_t offset);
    LValue emitWords(const Read&, unsigned column);
    LValue emitNumber(const Read&, LValue address, ptrdiff_t offset);
    LValue emitAdjacentPair(const Read&, ptrdiff_t offset);
    LValue emitElements(const Read&, unsigned vector);
    LValue emitTerm(ValueID, std::span<const LValue> elements);
    void emitSelection(const SumNest::Selection&);
    void emitStores();

    CodeGenerator& m_generator;
    const Code& m_code;
    Output& m_out;
    VectorOutput m_vectors;
    ElementAccessEmitter& m_elements;
    const SumNest& m_nest;
    LinearIntegers m_integers;
    ContextOfEnd m_contextOfEnd;
    LBasicBlock m_written { nullptr };
    LValue m_columnOfWritten { nullptr };
    Vector<std::pair<LBasicBlock, LValue>, 32> m_exits;
    Vector<std::pair<ValueID, LValue>, 32> m_saved;
    Vector<std::pair<ValueID, LValue>, 4> m_splats;
    Vector<std::optional<LinearInteger>, 4> m_parameters;
    Vector<Read, mostReads> m_reads;
    Vector<Read, mostSums> m_firstReads;
    std::optional<Read> m_arrayStore;
    Vector<Read, 2> m_columnReads;
    Vector<Array, 4> m_arrays;
    LValue m_allColumns { nullptr };
    LValue m_columns { nullptr };
    LValue m_terms { nullptr };
    LValue m_lastColumn { nullptr };
    LValue m_lastTerm { nullptr };
    Machine::StackSlot* m_slotOfSums { nullptr };
    LBasicBlock m_tiles { nullptr };
    LValue m_nextColumn { nullptr };
    LBasicBlock m_stores { nullptr };
    LValue m_storedColumn { nullptr };
    LValue m_storedPlace { nullptr };
    LValue m_endOfTile { nullptr };
};

static constexpr int64_t widestTile = 8;
static constexpr unsigned bytesOfSumVector = 16;

void SumNestEmitter::leaveUnless(LValue condition, LValue column)
{
    LBasicBlock holds = m_out.newBlock();
    LBasicBlock fails = m_out.newBlock();
    m_out.branch(condition, normalEdge(holds), unlikelyEdge(fails));
    m_exits.append({ fails, column });
    ScopeFork fork = m_out.forkScope();
    m_out.appendToSuccessor(holds, fork);
}

void SumNestEmitter::emitExits()
{
    for (auto [block, column] : std::exchange(m_exits, { })) {
        m_out.appendTo(block);
        m_out.upsilon(column, m_columnOfWritten);
        m_out.jump(normalEdge(m_written));
    }
}

void SumNestEmitter::name(ValueID value, LValue lowered)
{
    LValue& place = m_generator.m_values[value.index()];
    if (!std::ranges::contains(m_saved, value, &std::pair<ValueID, LValue>::first))
        m_saved.append({ value, place });
    place = lowered;
}

LValue SumNestEmitter::numberOfIterations(const SumNest::CountedTest& test, LValue first)
{
    LValue number = m_out.sub(m_integers.valueOf(m_integers.find(test.bound)->first), first);
    if (test.distance) {
        LValue distance = m_integers.wide(m_generator.get(test.distance).value);
        number = test.isDistanceSubtracted ? m_out.add(number, distance) : m_out.sub(number, distance);
    }
    return test.added ? m_out.add(number, test.added) : number;
}

std::pair<LValue, LValue> SumNestEmitter::extremesOf(const LinearInteger& linear)
{
    LValue least = m_integers.valueOf(linear.first);
    LValue greatest = least;
    for (auto [step, last] : { std::pair(&linear.inner, m_lastTerm), std::pair(&linear.outer, m_lastColumn) }) {
        if (step->isZero())
            continue;
        LValue distance = m_out.mul(m_integers.valueOf(*step), last);
        if (step->isConstant) {
            LValue& moved = step->constant < 0 ? least : greatest;
            moved = m_out.add(moved, distance);
            continue;
        }
        static constexpr int64_t greatestStep = int64_t { 1 } << 32;
        leaveUnless(m_out.below(m_out.add(m_integers.valueOf(*step), greatestStep), m_out.constInt64(2 * greatestStep)), m_out.int64Zero);
        LValue isNegative = m_out.lessThan(distance, m_out.int64Zero);
        least = m_out.add(least, m_out.select(isNegative, distance, m_out.int64Zero));
        greatest = m_out.add(greatest, m_out.select(isNegative, m_out.int64Zero, distance));
    }
    return { least, greatest };
}

void SumNestEmitter::requireInt32(const LinearInteger& linear)
{
    static constexpr int64_t numberOfNegatives = int64_t { 1 } << 31;
    auto [least, greatest] = extremesOf(linear);
    leaveUnless(m_out.greaterThanOrEqual(least, m_out.constInt64(-numberOfNegatives)), m_out.int64Zero);
    leaveUnless(m_out.lessThan(greatest, m_out.constInt64(numberOfNegatives)), m_out.int64Zero);
}

LValue SumNestEmitter::columnsWithAnswer(const SumNest::ColumnTest& test, LValue columns)
{
    const Operation& comparison = m_code.at(test.comparison);
    LinearInteger left = *m_integers.find(comparison.leftOperand());
    LinearInteger right = *m_integers.find(comparison.rightOperand());
    bool isLess = (comparison.opcode == Opcode::Less) == test.answer && !test.isMinimum;
    if (!test.answer)
        std::swap(left, right);
    KnownInteger room = m_integers.add(right.first, m_integers.negate(left.first));
    KnownInteger step = m_integers.add(right.outer, m_integers.negate(left.outer));
    LValue columnsInRoom = m_out.add(m_integers.valueOf(room), m_out.constInt64(isLess ? 0 : 1));
    if (step.isConstant && step.constant == -1)
        return m_out.select(m_out.lessThan(columnsInRoom, columns), columnsInRoom, columns);
    leaveUnless(m_out.greaterThan(columnsInRoom, m_out.int64Zero), m_out.int64Zero);
    if (step.isConstant && step.constant >= 0)
        return columns;
    LValue roomOfLast = m_out.add(columnsInRoom, m_out.mul(m_integers.valueOf(step), m_out.sub(columns, m_out.constInt64(1))));
    leaveUnless(m_out.greaterThan(roomOfLast, m_out.int64Zero), m_out.int64Zero);
    return columns;
}

void SumNestEmitter::requireInt32(const SumNest::CountedTest& test, const LinearInteger& counter)
{
    if (test.mayWrap && (test.distance || m_generator.representationOf(test.counter) != Representation::Int32))
        requireInt32(counter);
}

SumNestEmitter::AddressRange SumNestEmitter::addressesOf(ValueID id, const LinearInteger& position)
{
    const Operation& access = m_code.at(id);
    auto [least, greatest] = extremesOf(position);
    LValue storage = nullptr;
    if (arrayAccesses.contains(access.opcode)) {
        const Array& array = requireDoubles(access);
        storage = array.storage;
        leaveUnless(m_out.greaterThanOrEqual(least, m_out.int64Zero), m_out.int64Zero);
        leaveUnless(m_out.lessThan(greatest, array.length), m_out.int64Zero);
    } else if (access.opcode == Opcode::PutTypedArrayElement) {
        if (!access.isInsideView())
            least = m_out.select(m_out.lessThan(least, m_out.int64Zero), m_out.int64Zero, least);
    } else {
        if (!access.isInsideView()) {
            leaveUnless(m_out.greaterThanOrEqual(least, m_out.int64Zero), m_out.int64Zero);
            leaveUnless(m_out.lessThan(greatest, m_out.zeroExt(m_generator.get(access.lengthOperands()[0]).value, Machine::Int64)), m_out.int64Zero);
        }
        if (ElementAccessEmitter& accesses = m_generator.emitter<ElementAccessEmitter>(); accesses.checksBounds()) {
            ValueContext context = m_generator.contextOf(m_code.blockOf(id), id);
            SetForScope<const OperationContext*> whileChecking(m_generator.m_operation, &context);
            m_generator.declareAccessesOf(context);
            leaveUnless(accesses.isRangeInsideView(context, Integer(least), Integer(greatest)).value(), m_out.int64Zero);
            m_out.clearDeclaredAccesses();
        }
    }
    if (!storage)
        storage = m_generator.get(access.vectorOperand()).value;
    int logSize = logElementSizeOf(access);
    return { m_out.add(storage, m_out.shl(least, logSize)), m_out.add(storage, m_out.shl(m_out.add(greatest, 1), logSize)) };
}

Operand SumNestEmitter::emitBefore(ValueID value)
{
    bool isInNest = !m_code.isConstant(value) && (m_nest.blocks.contains(m_code.blockOf(value)) || m_nest.innerBlocks.contains(m_code.blockOf(value)));
    if (!isInNest)
        return m_generator.get(value);
    BlockID block = m_code.blockOf(value);
    if (!m_generator.m_values[value.index()]) {
        const Operation& operation = m_code.at(value);
        if (operation.opcode == Opcode::Narrowed)
            emitBefore(operation.valueOperand());
        else {
            operation.forEachChild([&](ValueID operand) {
                emitBefore(operand);
            });
        }
        name(value, nullptr);
        m_generator.generateValue(block, value, operation, false, { });
        m_out.clearDeclaredAccesses();
    }
    ValueContext context = m_generator.contextOf(block, value);
    SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
    return m_generator.get(value);
}

LValue SumNestEmitter::emitInteger(ValueID value)
{
    Operand operand = emitBefore(value);
    switch (operand.representation) {
    case Representation::Int32:
    case Representation::Int64:
        return m_integers.wide(operand.value);
    case Representation::OptionalInt64: {
        LValue word = m_out.signExt32To64(m_out.castToInt32(operand.value));
        leaveUnless(m_out.equal(word, operand.value), m_out.int64Zero);
        return word;
    }
    case Representation::JSValue:
        leaveUnless(m_out.aboveOrEqual(operand.value, m_generator.pinned().numberTag), m_out.int64Zero);
        return m_out.signExt32To64(m_out.castToInt32(operand.value));
    default: {
        LValue word = m_out.castToInt32(m_generator.emitter<ConversionEmitter>().truncateUnchecked(Number(operand.value)).value());
        leaveUnless(m_out.doubleEqual(m_out.intToDouble(word), operand.value), m_out.int64Zero);
        return m_out.signExt32To64(word);
    }
    }
}

void SumNestEmitter::emitGuards()
{
    for (auto [block, isTaken] : m_nest.guards) {
        ValueID condition = m_code.at(block).terminator.conditionOperand();
        m_code.at(condition).forEachChild([&](ValueID operand) {
            emitBefore(operand);
        });
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock fails = m_out.newBlock();
        name(condition, nullptr);
        BranchTargets targets { normalEdge(holds), unlikelyEdge(fails) };
        if (!isTaken)
            std::swap(targets.taken, targets.notTaken);
        m_generator.generateBranchCondition(m_contextOfEnd(m_generator, block), condition, targets);
        if (m_generator.m_hasFailed)
            return;
        m_exits.append({ fails, m_out.int64Zero });
        m_out.appendTo(holds);
        m_out.clearDeclaredAccesses();
    }
}

const SumNestEmitter::Array& SumNestEmitter::requireDoubles(const Operation& access)
{
    ValueID value = access.arrayOperand();
    bool isStored = access.opcode == Opcode::PutArrayElement;
    if (Array* known = std::ranges::find_if(m_arrays, [&](const Array& array) { return array.value == value && array.isStored == isStored; }); known != m_arrays.end())
        return *known;
    CellPointer cell(emitBefore(value).value);
    if (isStored)
        leaveUnless(m_elements.hasArrayStorage(cell, ArrayStorage::Double, true).value(), m_out.int64Zero);
    else
        leaveUnless(m_out.equal(m_elements.indexingMode(cell, IsArray | IndexingShapeMask).value(), m_out.constInt32(ArrayWithDouble)), m_out.int64Zero);
    ArrayElements elements = m_elements.elementsOf(cell);
    m_arrays.append({ value, isStored, elements.butterfly.value(), m_elements.publicLength(elements).value() });
    return m_arrays.last();
}

SumNestEmitter::Read SumNestEmitter::readAt(ValueID id)
{
    const Operation& access = m_code.at(id);
    Read read { id, *m_integers.find(access.positionOperand()), nullptr, { }, { }, m_generator.heaps().TypedArrayProperties, { }, nullptr };
    unsigned logSize = logElementSizeOf(access);
    LValue first = m_integers.valueOf(read.position.first);
    if (arrayAccesses.contains(access.opcode)) {
        TypedPointer element = m_elements.elementOfStorage(ArrayStorage::Double, requireDoubles(access).storage, first);
        read.heap = *element.heap();
        read.address = m_out.valueOf(element);
    } else
        read.address = m_out.add(m_generator.get(access.vectorOperand()).value, m_out.shl(first, static_cast<int>(logSize)));
    read.bytesPerTerm = m_integers.shiftLeft(read.position.inner, logSize);
    read.bytesPerColumn = m_integers.shiftLeft(read.position.outer, logSize);
    return read;
}

void SumNestEmitter::emitFront()
{
    for (ValueID parameter : m_code.at(m_nest.head).parameters())
        m_parameters.append(m_generator.m_values[parameter.index()] ? m_integers.find(parameter) : std::nullopt);
    for (auto [test, isExpected] : m_nest.invariantTests) {
        name(test, nullptr);
        m_generator.generateValue(m_code.blockOf(test), test, m_code.at(test), false, { });
        if (m_generator.m_hasFailed)
            return;
        LValue answer = m_generator.m_values[test.index()];
        leaveUnless(isExpected ? answer : m_out.isZero32(answer), m_out.int64Zero);
    }
    m_out.clearDeclaredAccesses();
    size_t indexOfCounter = m_code.at(m_nest.innerHead).placeOfParameter(m_nest.terms.counter);
    LValue first = m_integers.wide(m_generator.m_values[m_nest.columns.counter.index()]);
    m_allColumns = numberOfIterations(m_nest.columns, first);
    LValue columns = m_out.sub(m_allColumns, m_out.constInt64(m_nest.columns.isAtEnd && !m_nest.exit ? 1 : 0));
    if (!m_nest.guards.isEmpty()) {
        leaveUnless(m_out.greaterThanOrEqual(columns, m_out.constInt64(fewestColumns)), m_out.int64Zero);
        emitGuards();
        if (m_generator.m_hasFailed)
            return;
    }
    for (const SumNest::ColumnTest& test : m_nest.columnTests)
        columns = columnsWithAnswer(test, columns);
    leaveUnless(m_out.greaterThanOrEqual(columns, m_out.constInt64(fewestColumns)), m_out.int64Zero);
    m_columns = m_out.select(m_out.lessThan(columns, m_out.constInt64(mostColumns)), columns, m_out.constInt64(mostColumns));
    std::optional<LinearInteger> firstTerm = m_integers.find(m_nest.innerEntry[indexOfCounter]);
    RELEASE_ASSERT(firstTerm);
    m_terms = numberOfIterations(m_nest.terms, m_integers.valueOf(firstTerm->first));
    leaveUnless(m_out.below(m_out.sub(m_terms, m_out.constInt64(fewestTerms)), m_out.constInt64(mostTerms - fewestTerms)), m_out.int64Zero);
    m_lastColumn = m_out.sub(m_columns, m_out.constInt64(1));
    m_lastTerm = m_out.sub(m_terms, m_out.constInt64(1));

    for (ValueID access : m_nest.reads)
        m_reads.append(readAt(access));
    for (const SumNest::Sum& sum : m_nest.sums) {
        if (sum.firstRead)
            m_firstReads.append(readAt(sum.firstRead));
    }
    for (ValueID access : m_nest.columnReads)
        m_columnReads.append(readAt(access));
    Vector<LinearInteger, 4> positionsOfStores;
    for (ValueID store : m_nest.stores) {
        positionsOfStores.append(*m_integers.find(m_code.at(store).positionOperand()));
        if (m_code.at(store).opcode == Opcode::PutArrayElement)
            m_arrayStore = readAt(store);
    }
    Vector<std::pair<LinearInteger, LinearInteger>, 4> sidesOfTermTests;
    for (auto [test, answer] : m_nest.termTests) {
        std::pair sides { *m_integers.find(m_code.at(test).leftOperand()), *m_integers.find(m_code.at(test).rightOperand()) };
        if (!answer)
            std::swap(sides.first, sides.second);
        sidesOfTermTests.append(sides);
    }
    for (const LinearInteger& linear : m_integers.wrapped)
        requireInt32(linear);
    requireInt32(m_nest.columns, { { 0, false, first, 0 }, { }, m_integers.constantOf(1) });
    requireInt32(m_nest.terms, { firstTerm->first, m_integers.constantOf(1), { } });
    for (size_t place = 0; place < sidesOfTermTests.size(); ++place) {
        auto [test, answer] = m_nest.termTests[place];
        auto& [left, right] = sidesOfTermTests[place];
        auto less = [&](const KnownInteger& first, const KnownInteger& second) { return m_integers.add(first, m_integers.negate(second)); };
        bool isLess = (m_code.at(test).opcode == Opcode::Less) == answer;
        LValue greatest = extremesOf({ less(left.first, right.first), less(left.inner, right.inner), less(left.outer, right.outer) }).second;
        leaveUnless(m_out.lessThan(greatest, m_out.constInt64(isLess ? 0 : 1)), m_out.int64Zero);
    }

    Vector<AddressRange, mostReads> rangesOfReads;
    for (Read& read : m_reads)
        rangesOfReads.append(addressesOf(read.access, read.position));
    Vector<AddressRange, mostSums> rangesOfFirstReads;
    for (Read& read : m_firstReads)
        rangesOfFirstReads.append(addressesOf(read.access, read.position));
    for (Read& read : m_columnReads)
        addressesOf(read.access, read.position);
    Vector<std::pair<ValueID, ValueID>, 4> comparedStores;
    for (size_t place = 0; place < m_nest.stores.size(); ++place) {
        const Operation& store = m_code.at(m_nest.stores[place]);
        std::pair<ValueID, ValueID> element { holderOf(store), store.positionOperand() };
        if (comparedStores.contains(element))
            continue;
        comparedStores.append(element);
        AddressRange stored = addressesOf(m_nest.stores[place], positionsOfStores[place]);
        auto requireApart = [&](const AddressRange& read) {
            leaveUnless(m_out.bitOr(m_out.belowOrEqual(stored.end, read.first), m_out.belowOrEqual(read.end, stored.first)), m_out.int64Zero);
        };
        for (const AddressRange& read : rangesOfReads)
            requireApart(read);
        for (size_t index = 0; index < m_firstReads.size(); ++index) {
            const Operation& read = m_code.at(m_firstReads[index].access);
            bool isSameKind = store.opcode == Opcode::PutArrayElement ? read.opcode == Opcode::GetArrayElement : read.opcode == Opcode::GetTypedArrayElement && read.typedArrayType() == store.typedArrayType();
            if (!isSameKind || holderOf(read) != holderOf(store) || read.positionOperand() != store.positionOperand())
                requireApart(rangesOfFirstReads[index]);
        }
    }
    for (Read& read : m_reads) {
        for (int64_t column = 1; !read.bytesPerColumn.isConstant && column < widestTile / 2; ++column)
            read.distances.append(m_out.mul(m_integers.valueOf(read.bytesPerColumn), m_out.constInt64(column)));
    }
}

LValue SumNestEmitter::emitNumber(const Read& read, LValue address, ptrdiff_t offset)
{
    TypedPointer element(read.heap, address, offset);
    const Operation& access = m_code.at(read.access);
    if (arrayAccesses.contains(access.opcode))
        return m_out.loadDouble(element);
    switch (access.typedArrayType()) {
    case TypeInt8:
        return m_out.intToDouble(m_out.load8SignExt32(element));
    case TypeUint8:
    case TypeUint8Clamped:
        return m_out.intToDouble(m_out.load8ZeroExt32(element));
    case TypeInt16:
        return m_out.intToDouble(m_out.load16SignExt32(element));
    case TypeUint16:
        return m_out.intToDouble(m_out.load16ZeroExt32(element));
    case TypeInt32:
        return m_out.intToDouble(m_out.load32(element));
    case TypeFloat32:
        return m_out.floatToDouble(m_out.loadFloat(element));
    default:
        return m_out.loadDouble(element);
    }
}

LValue SumNestEmitter::emitSplat(ValueID value)
{
    if (!m_nest.sumsWords)
        return m_vectors.splat(m_generator.getIn(value, Representation::Double).value);
    bool isConstant = m_code.isConstant(value);
    return m_vectors.splatInteger(isConstant ? m_out.constInt32(static_cast<int32_t>(m_code.at(value).integer())) : m_generator.get(value).value, lanes());
}

LValue SumNestEmitter::emitWord(const Read& read, LValue address, ptrdiff_t offset)
{
    TypedPointer element(read.heap, address, offset);
    switch (m_code.at(read.access).typedArrayType()) {
    case TypeInt8:
        return m_out.load8SignExt32(element);
    case TypeUint8:
    case TypeUint8Clamped:
        return m_out.load8ZeroExt32(element);
    case TypeInt16:
        return m_out.load16SignExt32(element);
    case TypeUint16:
        return m_out.load16ZeroExt32(element);
    default:
        return m_out.load32(element);
    }
}

LValue SumNestEmitter::emitWords(const Read& read, unsigned column)
{
    TypedArrayType type = m_code.at(read.access).typedArrayType();
    int64_t size = elementSize(type);
    if (read.isShared())
        return m_vectors.splatInteger(emitWord(read, read.pointer, 0), lanes());
    if (read.bytesPerColumn.isConstant && read.bytesPerColumn.constant == size) {
        TypedPointer element(read.heap, read.pointer, column * size);
        return size == 4 ? m_out.load(element, Machine::Vector128) : m_generator.loadWords(element, type, lanes());
    }
    auto wordOfColumn = [&](unsigned place) {
        if (read.bytesPerColumn.isConstant)
            return emitWord(read, read.pointer, place * read.bytesPerColumn.constant);
        return emitWord(read, place ? m_out.add(read.pointer, read.distances[place - 1]) : read.pointer, 0);
    };
    auto pairOfColumns = [&](unsigned place) {
        LValue first = m_out.zeroExt(wordOfColumn(place), Machine::Int64);
        LValue second = m_out.zeroExt(wordOfColumn(place + 1), Machine::Int64);
        return m_out.bitOr(first, m_out.shl(second, 32));
    };
    LValue firstPair = pairOfColumns(column);
    return m_vectors.joinWords(firstPair, pairOfColumns(column + 2));
}

LValue SumNestEmitter::emitAdjacentPair(const Read& read, ptrdiff_t offset)
{
    const Operation& access = m_code.at(read.access);
    TypedPointer element(read.heap, read.pointer, offset);
    if (logElementSizeOf(access) == 3)
        return m_out.load(element, Machine::Vector128);
    TypedArrayType type = access.typedArrayType();
    if (type == TypeFloat32)
        return m_vectors.singlesToNumbers(m_out.loadDouble(element));
    LValue integers = elementSize(type) == 1 ? m_out.load16ZeroExt32(element) : elementSize(type) == 2 ? m_out.load32(element) : m_out.loadDouble(element);
    return m_vectors.toNumbers(m_vectors.widenToWords(integers, type), 0);
}

LValue SumNestEmitter::emitElements(const Read& read, unsigned vector)
{
    const Operation& access = m_code.at(read.access);
    ValueContext context = m_generator.contextOf(m_code.blockOf(read.access), read.access);
    SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
    m_out.setOrigin(machineOriginOf(read.access));
    m_generator.declareAccessesOf(context);
    LValue result = nullptr;
    int64_t size = 1 << logElementSizeOf(access);
    unsigned column = vector * lanes();
    if (m_nest.sumsWords)
        result = emitWords(read, column);
    else if (read.isShared())
        result = m_vectors.splat(emitNumber(read, read.pointer, 0));
    else if (read.bytesPerColumn.isConstant && read.bytesPerColumn.constant == size)
        result = emitAdjacentPair(read, column * size);
    else {
        auto numberOfColumn = [&](unsigned place) {
            if (read.bytesPerColumn.isConstant)
                return emitNumber(read, read.pointer, place * read.bytesPerColumn.constant);
            return emitNumber(read, place ? m_out.add(read.pointer, read.distances[place - 1]) : read.pointer, 0);
        };
        LValue first = numberOfColumn(column);
        result = m_vectors.insert(first, numberOfColumn(column + 1));
    }
    m_out.clearDeclaredAccesses();
    return result;
}

LValue SumNestEmitter::emitTerm(ValueID value, std::span<const LValue> elements)
{
    size_t read = m_nest.reads.find(value);
    if (read != notFound)
        return elements[read];
    auto splat = std::ranges::find(m_splats, value, &std::pair<ValueID, LValue>::first);
    if (splat != m_splats.end())
        return splat->second;
    const Operation& operation = m_code.at(value);
    if (m_nest.sumsWords) {
        static constexpr OpcodeSet computations { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::CallInline };
        if (!computations.contains(operation.opcode))
            return emitTerm(operation.valueOperand(), elements);
        Vector<LValue, 2> operands;
        operation.forEachChild([&](ValueID operand) {
            operands.append(emitTerm(operand, elements));
        });
        m_out.setOrigin(machineOriginOf(value));
        return m_vectors.computeIntegers(operation.opcode == Opcode::CallInline ? Opcode::Mul : operation.opcode, lanes(), operands[0], operands[1]);
    }
    VectorOutput::Arithmetic arithmetic;
    switch (operation.opcode) {
    case Opcode::Add:
        arithmetic = VectorOutput::Arithmetic::Add;
        break;
    case Opcode::Sub:
        arithmetic = VectorOutput::Arithmetic::Sub;
        break;
    case Opcode::Mul:
        arithmetic = VectorOutput::Arithmetic::Mul;
        break;
    case Opcode::Div:
        arithmetic = VectorOutput::Arithmetic::Div;
        break;
    default:
        return emitTerm(operation.valueOperand(), elements);
    }
    LValue left = emitTerm(operation.leftOperand(), elements);
    LValue right = emitTerm(operation.rightOperand(), elements);
    m_out.setOrigin(machineOriginOf(value));
    return m_vectors.compute(arithmetic, left, right);
}

void SumNestEmitter::emitTile(int64_t width, LValue column)
{
    unsigned vectors = width / lanes();
    TerminatorContext context = m_contextOfEnd(m_generator, m_nest.head);
    SetForScope<const OperationContext*> whileSumming(m_generator.m_operation, &context);
    m_out.setOrigin(Machine::Origin());
    m_splats.shrink(0);
    auto isInInnerLoop = [&](ValueID value) { return !m_code.isConstant(value) && m_nest.innerBlocks.contains(m_code.blockOf(value)); };
    auto addSplats = [&](auto& self, ValueID value) -> void {
        if (m_nest.reads.contains(value) || std::ranges::contains(m_splats, value, &std::pair<ValueID, LValue>::first))
            return;
        if (!isInInnerLoop(value)) {
            m_splats.append({ value, emitSplat(value) });
            return;
        }
        m_code.at(value).forEachChild([&](ValueID operand) { self(self, operand); });
    };
    for (const SumNest::Sum& sum : m_nest.sums)
        addSplats(addSplats, sum.term);

    LBasicBlock terms = m_out.newBlock();
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    Vector<LValue, 8> sums;
    size_t numberOfFirstReads = 0;
    for (const SumNest::Sum& sum : m_nest.sums) {
        Read* read = sum.firstRead ? &m_firstReads[numberOfFirstReads++] : nullptr;
        if (read)
            read->pointer = m_out.add(read->address, m_out.mul(column, m_integers.valueOf(read->bytesPerColumn)));
        LValue first = read ? nullptr : emitSplat(sum.first);
        for (unsigned vector = 0; vector < vectors; ++vector) {
            LValue firstOfVector = read ? emitElements(*read, vector) : first;
            sums.append(m_out.phiIn(terms, Machine::Vector128));
            m_out.upsilon(firstOfVector, sums.last());
        }
    }
    m_out.setOrigin(Machine::Origin());
    LValue end = nullptr;
    LValue counter = nullptr;
    Read* counted = nullptr;
    for (Read& read : m_reads) {
        read.pointer = read.address;
        if (!read.isShared())
            read.pointer = m_out.add(read.pointer, m_out.mul(column, m_integers.valueOf(read.bytesPerColumn)));
        if (read.bytesPerTerm.isZero())
            continue;
        LValue first = read.pointer;
        read.pointer = m_out.phiIn(terms, Machine::Int64);
        m_out.upsilon(first, read.pointer);
        if (!end && read.bytesPerTerm.isConstant) {
            end = m_out.add(first, m_out.mul(m_terms, m_integers.valueOf(read.bytesPerTerm)));
            counted = &read;
        }
    }
    if (!end) {
        counter = m_out.phiIn(terms, Machine::Int64);
        m_out.upsilon(m_out.int64Zero, counter);
    }
    m_out.jump(normalEdge(terms));

    m_out.appendTo(terms);
    Vector<LValue, 8> nextSums;
    nextSums.fill(nullptr, sums.size());
    Vector<LValue, mostReads> elements;
    elements.fill(nullptr, m_reads.size());
    for (unsigned vector = 0; vector < vectors; ++vector) {
        for (size_t place = 0; place < m_reads.size(); ++place) {
            if (!elements[place] || !m_reads[place].isShared())
                elements[place] = emitElements(m_reads[place], vector);
        }
        for (size_t place = 0; place < m_nest.sums.size(); ++place) {
            const SumNest::Sum& sum = m_nest.sums[place];
            LValue term = emitTerm(sum.term, elements.span());
            LValue before = sums[place * vectors + vector];
            m_out.setOrigin(machineOriginOf(sum.next));
            LValue& next = nextSums[place * vectors + vector];
            if (m_nest.sumsWords)
                next = m_vectors.computeIntegers(sum.isTermSubtracted ? Opcode::Sub : Opcode::Add, lanes(), before, term);
            else
                next = m_vectors.compute(sum.isTermSubtracted ? VectorOutput::Arithmetic::Sub : VectorOutput::Arithmetic::Add, before, term);
        }
    }
    m_out.setOrigin(Machine::Origin());
    Vector<LValue, mostReads> nextPointers;
    for (Read& read : m_reads)
        nextPointers.append(read.bytesPerTerm.isZero() ? nullptr : m_out.add(read.pointer, m_integers.valueOf(read.bytesPerTerm)));
    LValue nextCounter = counter ? m_out.add(counter, 1) : nullptr;
    m_out.branch(counter ? m_out.lessThan(nextCounter, m_terms) : m_out.notEqual(nextPointers[counted - m_reads.begin()], end), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    for (size_t place = 0; place < sums.size(); ++place)
        m_out.upsilon(nextSums[place], sums[place]);
    for (size_t place = 0; place < m_reads.size(); ++place) {
        if (nextPointers[place])
            m_out.upsilon(nextPointers[place], m_reads[place].pointer);
    }
    if (counter)
        m_out.upsilon(nextCounter, counter);
    m_out.jump(normalEdge(terms));

    m_out.appendTo(ended);
    LValue total = nextSums[0];
    for (size_t place = 1; place < nextSums.size() && !m_nest.sumsWords; ++place)
        total = m_vectors.compute(VectorOutput::Arithmetic::Add, total, nextSums[place]);
    for (size_t place = 0; place < nextSums.size(); ++place) {
        ptrdiff_t offset = place / vectors * widestTile * sizeof(double) + place % vectors * bytesOfSumVector;
        m_out.store(nextSums[place], m_out.address(m_generator.heaps().frameSlots, m_out.slotBase(m_slotOfSums), offset));
    }
    if (!m_nest.sumsWords) {
        LValue number = m_out.doubleAdd(m_vectors.lane(total, 0), m_vectors.lane(total, 1));
        leaveUnless(m_out.doubleEqual(number, number), column);
    }
    m_out.upsilon(column, m_storedColumn);
    m_out.upsilon(m_out.int64Zero, m_storedPlace);
    m_out.upsilon(m_out.add(column, m_out.constInt64(width)), m_endOfTile);
    m_out.jump(normalEdge(m_stores));
}

void SumNestEmitter::emitSelection(const SumNest::Selection& selection)
{
    const Step& step = m_nest.steps[selection.step];
    std::array<LBasicBlock, 2> sides { m_out.newBlock(), m_out.newBlock() };
    LBasicBlock join = m_out.newBlock();
    name(step.value, nullptr);
    m_generator.generateBranchCondition(m_contextOfEnd(m_generator, step.block), step.value, { normalEdge(sides[0]), normalEdge(sides[1]) });
    Vector<LValue, 2> phis;
    for (size_t place = 0; place < sides.size(); ++place) {
        const SumNest::Selection::Side& side = selection.sides[place];
        m_out.appendTo(sides[place]);
        for (ValueID value : side.values) {
            name(value, nullptr);
            m_generator.generateValue(side.block, value, m_code.at(value), false, { });
            if (m_generator.m_hasFailed)
                return;
        }
        TerminatorContext context = m_contextOfEnd(m_generator, side.block);
        SetForScope<const OperationContext*> whilePassing(m_generator.m_operation, &context);
        m_out.setOrigin(Machine::Origin());
        Vector<LValue, 2> passed;
        for (size_t index = 0; index < selection.parameters.size(); ++index)
            passed.append(m_generator.getIn(side.passed[index], m_generator.representationOf(selection.parameters[index])).value);
        for (size_t index = 0; index < passed.size(); ++index) {
            if (!place)
                phis.append(m_out.phiIn(join, passed[index]->type()));
            m_out.upsilon(passed[index], phis[index]);
        }
        m_out.jump(normalEdge(join));
    }
    m_out.appendTo(join);
    for (size_t index = 0; index < phis.size(); ++index)
        name(selection.parameters[index], phis[index]);
}

void SumNestEmitter::emitArrayStore(ValueID store)
{
    LValue number = nullptr;
    {
        TerminatorContext context = m_contextOfEnd(m_generator, m_code.isConstant(m_nest.storedInArray) ? m_nest.head : m_code.blockOf(m_nest.storedInArray));
        SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
        number = m_generator.getIn(m_nest.storedInArray, Representation::Double).value;
    }
    leaveUnless(m_out.doubleEqual(number, number), m_storedColumn);
    ValueContext context = m_generator.contextOf(m_code.blockOf(store), store);
    SetForScope<const OperationContext*> whileStoring(m_generator.m_operation, &context);
    m_out.setOrigin(machineOriginOf(store));
    m_generator.declareAccessesOf(context);
    LValue address = m_out.add(m_arrayStore->address, m_out.mul(m_storedColumn, m_integers.valueOf(m_arrayStore->bytesPerColumn)));
    m_out.storeDouble(number, m_out.address(m_arrayStore->heap, address));
}

void SumNestEmitter::emitColumnRead(ValueID access)
{
    const Read& read = *std::ranges::find(m_columnReads, access, &Read::access);
    LValue number = nullptr;
    {
        ValueContext context = m_generator.contextOf(m_code.blockOf(access), access);
        SetForScope<const OperationContext*> whileReading(m_generator.m_operation, &context);
        m_out.setOrigin(machineOriginOf(access));
        m_generator.declareAccessesOf(context);
        number = emitNumber(read, m_out.add(read.address, m_out.mul(m_storedColumn, m_integers.valueOf(read.bytesPerColumn))), 0);
        m_out.clearDeclaredAccesses();
    }
    leaveUnless(m_out.doubleEqual(number, number), m_storedColumn);
    name(access, number);
}

void SumNestEmitter::emitStores()
{
    m_out.appendTo(m_stores);
    m_out.setOrigin(Machine::Origin());
    std::span<const ValueID> parameters = m_code.at(m_nest.head).parameters();
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (!m_parameters[index])
            continue;
        LValue current = m_integers.valueOf(m_parameters[index]->first);
        if (!m_parameters[index]->outer.isZero())
            current = m_out.add(current, m_out.mul(m_storedColumn, m_integers.valueOf(m_parameters[index]->outer)));
        name(parameters[index], m_generator.representationOf(parameters[index]) == Representation::Int32 ? m_out.castToInt32(current) : current);
    }
    for (size_t place = 0; place < m_nest.sums.size(); ++place) {
        TypedPointer sum = m_out.baseIndex(m_generator.heaps().frameSlots, m_out.slotBase(m_slotOfSums), m_storedPlace, m_nest.sumsWords ? ScaleFour : ScaleEight, place * widestTile * sizeof(double));
        name(m_nest.sums[place].result, m_nest.sumsWords ? m_out.load32(sum) : m_out.loadDouble(sum));
    }
    for (const Step& step : m_nest.steps) {
        if (!step.isNeeded)
            continue;
        if (step.kind == Step::Kind::Selection)
            emitSelection(*std::ranges::find(m_nest.selections, static_cast<size_t>(&step - m_nest.steps.begin()), &SumNest::Selection::step));
        else if (step.kind == Step::Kind::Name) {
            TerminatorContext context = m_contextOfEnd(m_generator, step.block);
            SetForScope<const OperationContext*> whileNaming(m_generator.m_operation, &context);
            name(step.value, m_generator.getIn(step.source, m_generator.representationOf(step.value)).value);
        } else if (m_generator.opcodeOf(step.value) == Opcode::PutArrayElement)
            emitArrayStore(step.value);
        else if (m_generator.opcodeOf(step.value) == Opcode::GetArrayElement)
            emitColumnRead(step.value);
        else {
            name(step.value, nullptr);
            m_generator.generateValue(step.block, step.value, m_code.at(step.value), false, { });
        }
        m_out.clearDeclaredAccesses();
        if (m_generator.m_hasFailed)
            return;
    }
    m_out.setOrigin(Machine::Origin());
    LValue next = m_out.add(m_storedColumn, 1);
    LBasicBlock again = m_out.newBlock();
    LBasicBlock ended = m_out.newBlock();
    m_out.branch(m_out.lessThan(next, m_endOfTile), normalEdge(again), normalEdge(ended));
    m_out.appendTo(again);
    m_out.upsilon(next, m_storedColumn);
    m_out.upsilon(m_out.add(m_storedPlace, 1), m_storedPlace);
    m_out.upsilon(m_endOfTile, m_endOfTile);
    m_out.jump(normalEdge(m_stores));
    m_out.appendTo(ended);
    m_out.upsilon(m_endOfTile, m_nextColumn);
    m_out.jump(normalEdge(m_tiles));
}

void SumNestEmitter::emit()
{
    m_generator.build().statistics().count(StatisticID::ViewSumNests);
    m_generator.m_proc.noteVectors();
    const Block& head = m_code.at(m_nest.head);
    std::span<const ValueID> parameters = head.parameters();
    m_written = m_out.newBlock();
    LBasicBlock writtenHead = m_out.newBlock();
    m_columnOfWritten = m_out.phiIn(m_written, Machine::Int64);
    Vector<LValue, 4> phisOfWrittenHead;
    for (ValueID parameter : parameters) {
        LValue phi = m_generator.m_values[parameter.index()];
        phisOfWrittenHead.append(phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
    }
    m_slotOfSums = m_generator.m_proc.addStackSlot(m_nest.sums.size() * widestTile * sizeof(double));
    SetForScope<BlockID> whileEmitting(m_generator.m_headOfTurnsLaidOut, m_nest.head);
    m_out.appendTo(m_generator.blockFor(m_nest.head));
    m_out.setOrigin(Machine::Origin());
    m_out.clearDeclaredAccesses();
    {
        TerminatorContext context = m_contextOfEnd(m_generator, m_nest.head);
        SetForScope<const OperationContext*> whileChecking(m_generator.m_operation, &context);
        emitFront();
        if (m_generator.m_hasFailed)
            return;
    }
    m_out.clearDeclaredAccesses();
    m_tiles = m_out.newBlock();
    m_stores = m_out.newBlock();
    m_nextColumn = m_out.phiIn(m_tiles, Machine::Int64);
    m_storedColumn = m_out.phiIn(m_stores, Machine::Int64);
    m_storedPlace = m_out.phiIn(m_stores, Machine::Int64);
    m_endOfTile = m_out.phiIn(m_stores, Machine::Int64);
    m_out.upsilon(m_out.int64Zero, m_nextColumn);
    m_out.jump(normalEdge(m_tiles));

    m_out.appendTo(m_tiles);
    if (m_nest.poll) {
        name(m_nest.poll, nullptr);
        m_generator.generateValue(m_code.blockOf(m_nest.poll), m_nest.poll, m_code.at(m_nest.poll), false, { });
        m_out.clearDeclaredAccesses();
        m_out.setOrigin(Machine::Origin());
    }
    LValue remaining = m_out.sub(m_columns, m_nextColumn);
    bool isSpacedAtRunTime = std::ranges::any_of(m_reads, [](const Read& read) { return !read.bytesPerColumn.isConstant; });
    for (int64_t width = isSpacedAtRunTime || m_nest.sums.size() > 1 ? widestTile / 2 : widestTile; width >= static_cast<int64_t>(lanes()); width /= 2) {
        LBasicBlock isWide = m_out.newBlock();
        LBasicBlock isNarrow = m_out.newBlock();
        m_out.branch(m_out.lessThan(remaining, m_out.constInt64(width)), normalEdge(isNarrow), normalEdge(isWide));
        m_out.appendTo(isWide);
        emitTile(width, m_nextColumn);
        if (m_generator.m_hasFailed)
            return;
        m_out.appendTo(isNarrow);
    }
    m_out.setOrigin(Machine::Origin());
    if (m_nest.exit) {
        m_generator.expectOtherJumps(m_nest.exit);
        LBasicBlock columnsRemain = m_out.newBlock();
        m_out.branch(m_out.equal(m_nextColumn, m_allColumns), normalEdge(m_generator.m_blocks[m_nest.exit.index()]), normalEdge(columnsRemain));
        m_out.appendTo(columnsRemain);
    }
    m_out.upsilon(m_nextColumn, m_columnOfWritten);
    m_out.jump(normalEdge(m_written));
    emitStores();
    if (m_generator.m_hasFailed)
        return;
    emitExits();
    for (const auto& [value, lowered] : m_saved)
        m_generator.m_values[value.index()] = lowered;

    m_out.appendTo(m_written);
    m_out.setOrigin(Machine::Origin());
    for (size_t index = 0; index < parameters.size(); ++index) {
        if (!phisOfWrittenHead[index])
            continue;
        LValue current = m_integers.valueOf(m_parameters[index]->first);
        if (!m_parameters[index]->outer.isZero())
            current = m_out.add(current, m_out.mul(m_columnOfWritten, m_integers.valueOf(m_parameters[index]->outer)));
        m_out.upsilon(phisOfWrittenHead[index]->type() == Machine::Int32 ? m_out.castToInt32(current) : current, phisOfWrittenHead[index]);
    }
    m_out.jump(normalEdge(writtenHead));
    m_generator.m_blocks[m_nest.head.index()] = writtenHead;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_generator.m_values[parameters[index].index()] = phisOfWrittenHead[index];
}

void CodeGenerator::tryEmitSumNest(BlockID head)
{
    if (m_keepsValuesInHeapFrame || m_interleaving || m_headOfTurnsLaidOut)
        return;
    if (std::optional<SumNest> nest = SumNestFinder(*this, head).find()) [[unlikely]]
        SumNestEmitter(*this, *nest, [](const CodeGenerator& generator, BlockID block) { return TerminatorContext(block, generator.code().at(block).terminator, nullptr); }).emit();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
