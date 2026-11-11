/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBigIntValues.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet passesOperandOn { Opcode::Box, Opcode::Unbox, Opcode::Narrowed };
static constexpr OpcodeSet arithmeticInFrame { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Negate, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet mayWriteOverOperands { Opcode::Add, Opcode::Sub, Opcode::Div, Opcode::Negate, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet comparisons = allRelations | allEqualities;

static bool hasBigIntRepresentation(const Operation& operation)
{
    return operation.form() == OperatorForm::Generic || operation.form() == OperatorForm::BigInts;
}

static bool isNumberRepresentation(Representation representation)
{
    switch (representation) {
    case Representation::Int32:
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Double:
    case Representation::OptionalDouble:
        return true;
    default:
        return false;
    }
}

static bool mayHoldBigInt(const Operation& operation)
{
    return operation.representation == Representation::JSValue || operation.representation == Representation::Cell;
}

bool BigIntValues::isArithmetic(const Operation& operation)
{
    return arithmeticInFrame.contains(operation.opcode) && hasBigIntRepresentation(operation) && (operation.representation == Representation::JSValue || operation.representation == Representation::Cell);
}

BigIntValues::BigIntValues(const Code& code, const TypeArena& types, FrameUse frameUse)
    : m_code(code)
    , m_types(types)
    , m_frameUse(frameUse)
{
    bool hasBigInt = false;
    for (uint32_t index = 0; !hasBigInt && index < code.numberOfValues(); ++index)
        hasBigInt = code.at(ValueID(index)).type == TypeTable::bigInt;
    if (!hasBigInt)
        return;
    findLikelyValues();
    findBlocksForBigInts();
    if (frameUse != FrameUse::Never)
        findValuesForFrame();
}

uint32_t BigIntValues::classOf(uint32_t index)
{
    while (m_classes[index] != index) {
        m_classes[index] = m_classes[m_classes[index]];
        index = m_classes[index];
    }
    return index;
}

void BigIntValues::unite(ValueID left, ValueID right)
{
    m_classes[classOf(left.index())] = classOf(right.index());
}

void BigIntValues::findLikelyValues()
{
    static constexpr OpcodeSet unitesOperands = comparisons | OpcodeSet {
        Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift,
        Opcode::Negate, Opcode::Increment, Opcode::Decrement, Opcode::BitNot,
    };
    uint32_t numberOfValues = m_code.numberOfValues();
    m_classes.grow(numberOfValues);
    for (uint32_t index = 0; index < numberOfValues; ++index)
        m_classes[index] = index;

    Vector<ValueID> remaindersOfProducts;
    for (uint32_t number = 0; number < m_code.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!m_code.isReached(block))
            continue;
        for (ValueID value : m_code.valuesOf(block)) {
            const Operation& operation = m_code.at(value);
            if (passesOperandOn.contains(operation.opcode)) {
                unite(operation.operand(0), value);
                continue;
            }
            if (!unitesOperands.contains(operation.opcode) || !hasBigIntRepresentation(operation))
                continue;
            ValueID first = operation.operand(0);
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                unite(operand, first);
            });
            if (!comparisons.contains(operation.opcode))
                unite(value, first);
            if (operation.opcode == Opcode::Mod && m_code.at(first).opcode == Opcode::Mul)
                remaindersOfProducts.append(value);
        }
        const Terminator& terminator = m_code.at(block).terminator;
        for (uint32_t target = 0; target < terminator.numberOfEdges; ++target) {
            const Edge& edge = m_code.at(terminator.edge(target));
            std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
            for (size_t index = 0; index < edge.arguments().size(); ++index)
                unite(edge.arguments()[index], parameters[index]);
        }
    }

    BitVector likelyClasses(numberOfValues);
    BitVector classesOfNumbers(numberOfValues);
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        const Operation& operation = m_code.at(ValueID(index));
        if (operation.type == TypeTable::bigInt)
            likelyClasses.quickSet(classOf(index));
        else if (isNumberRepresentation(operation.representation) || operation.opcode == Opcode::Integer || operation.opcode == Opcode::Double)
            classesOfNumbers.quickSet(classOf(index));
    }
    for (ValueID value : remaindersOfProducts) {
        if (uint32_t found = classOf(value.index()); !classesOfNumbers.quickGet(found))
            likelyClasses.quickSet(found);
    }
    m_isLikely.ensureSize(numberOfValues);
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        if (likelyClasses.quickGet(classOf(index)))
            m_isLikely.quickSet(index);
    }
}

void BigIntValues::findBlocksForBigInts()
{
    for (uint32_t number = 0; number < m_code.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!m_code.isReached(block) || m_code.at(block).frequency != BlockFrequency::Rare)
            continue;
        bool isForBigInts = std::ranges::any_of(m_code.valuesOf(block), [&](ValueID value) {
            const Operation& operation = m_code.at(value);
            return isArithmetic(operation) && isLikely(value) && std::ranges::any_of(operation.operands(), [&](ValueID operand) {
                uint32_t primitives = m_types.at(m_code.at(operand).type).primitives;
                return (primitives & Primitive::BigInt) && !(primitives & Primitive::Number);
            });
        });
        if (isForBigInts)
            m_rareBlocksForBigInts.set(number);
    }
}

bool BigIntValues::readsOperands(const Operation& operation) const
{
    return isArithmetic(operation) || (comparisons.contains(operation.opcode) && hasBigIntRepresentation(operation));
}

void BigIntValues::findValuesForFrame()
{
    if (m_code.hasSuspensions())
        return;
    uint32_t numberOfValues = m_code.numberOfValues();
    BitVector isCandidate(numberOfValues);
    BitVector isProducer(numberOfValues);
    bool hasProducer = false;
    for (uint32_t number = 0; number < m_code.numberOfBlocks(); ++number) {
        BlockID id(number);
        if (!m_code.isReached(id) || isRare(id))
            continue;
        const Block& block = m_code.at(id);
        if (!block.isHandlerBlock() && !block.isEntry) {
            for (ValueID parameter : block.parameters()) {
                if (isLikely(parameter) && mayHoldBigInt(m_code.at(parameter)))
                    isCandidate.quickSet(parameter.index());
            }
        }
        for (ValueID value : m_code.valuesOf(id)) {
            const Operation& operation = m_code.at(value);
            if (!isLikely(value) || !mayHoldBigInt(operation))
                continue;
            if (isArithmetic(operation) && mayUseFrame(id)) {
                isCandidate.quickSet(value.index());
                isProducer.quickSet(value.index());
                hasProducer = true;
            } else if (passesOperandOn.contains(operation.opcode))
                isCandidate.quickSet(value.index());
        }
    }
    if (!hasProducer)
        return;

    BitVector escapes(numberOfValues);
    BitVector leavesBlock(numberOfValues);
    Vector<Flow> flows;
    Vector<CopyInUnlikelyBlock> copies;
    for (uint32_t number = 0; number < m_code.numberOfBlocks(); ++number) {
        BlockID id(number);
        if (!m_code.isReached(id))
            continue;
        const Block& block = m_code.at(id);
        bool blockIsRare = isRare(id);
        auto escapeHere = [&](ValueID value) {
            if (!isCandidate.quickGet(value.index()))
                return;
            leavesBlock.quickSet(value.index());
            if (!blockIsRare)
                escapes.quickSet(value.index());
            else if (copies.isEmpty() || copies.last().block != id || !std::ranges::any_of(copies, [&](const CopyInUnlikelyBlock& copy) { return copy.block == id && copy.value == value; }))
                copies.append({ id, value, { } });
        };
        for (ValueID value : m_code.valuesOf(id)) {
            const Operation& operation = m_code.at(value);
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                if (!isCandidate.quickGet(operand.index()) || (passesOperandOn.contains(operation.opcode) && !mayHoldBigInt(operation)))
                    return;
                if (readsOperands(operation)) {
                    if (m_code.blockOf(operand) != id || operation.opcode == Opcode::Mul)
                        leavesBlock.quickSet(operand.index());
                } else if (isCandidate.quickGet(value.index()) && passesOperandOn.contains(operation.opcode)) {
                    leavesBlock.quickSet(operand.index());
                    flows.append({ operand, value });
                } else
                    escapeHere(operand);
            });
        }
        block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            escapeHere(operand);
        });
        for (uint32_t target = 0; target < block.terminator.numberOfEdges; ++target) {
            const Edge& edge = m_code.at(block.terminator.edge(target));
            std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
            for (size_t index = 0; index < edge.arguments().size(); ++index) {
                ValueID argument = edge.arguments()[index];
                if (!isCandidate.quickGet(argument.index()))
                    continue;
                if (!isCandidate.quickGet(parameters[index].index())) {
                    escapeHere(argument);
                    continue;
                }
                leavesBlock.quickSet(argument.index());
                flows.append({ argument, parameters[index] });
            }
        }
        if (block.handler) {
            for (ValueID argument : m_code.at(block.handler).arguments())
                escapes.quickSet(argument.index());
        }
        if (block.subject)
            escapes.quickSet(block.subject.index());
    }

    for (bool changes = true; changes;) {
        changes = false;
        for (const Flow& flow : flows) {
            if (escapes.quickGet(flow.to.index()) && !escapes.quickGet(flow.from.index())) {
                escapes.quickSet(flow.from.index());
                changes = true;
            }
        }
    }
    m_mayBeInFrame.ensureSize(numberOfValues);
    Vector<ValueID> producers;
    producers.grow(numberOfValues);
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        if (isProducer.quickGet(index) && !escapes.quickGet(index)) {
            m_mayBeInFrame.quickSet(index);
            producers[index] = ValueID(index);
        }
    }
    for (bool changes = true; changes;) {
        changes = false;
        for (const Flow& flow : flows) {
            if (m_mayBeInFrame.quickGet(flow.from.index()) && !m_mayBeInFrame.quickGet(flow.to.index())) {
                m_mayBeInFrame.quickSet(flow.to.index());
                producers[flow.to.index()] = producers[flow.from.index()];
                changes = true;
            }
        }
    }
    m_passesOn.ensureSize(numberOfValues);
    for (const Flow& flow : flows)
        m_passesOn.quickSet(flow.from.index());
    findCarriedValues(flows.span(), isProducer);
    m_staysInBlock.ensureSize(numberOfValues);
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        if (isProducer.quickGet(index) && m_mayBeInFrame.quickGet(index) && !leavesBlock.quickGet(index))
            m_staysInBlock.quickSet(index);
    }
    for (CopyInUnlikelyBlock& copy : copies) {
        if (m_mayBeInFrame.quickGet(copy.value.index())) {
            copy.producer = producers[copy.value.index()];
            m_copies.append(copy);
        }
    }
}

std::span<const ValueID> BigIntValues::valuesKeptBeside(ValueID value) const
{
    for (const CarriedValue& carried : m_carriedValues) {
        if (carried.value == value)
            return carried.keptBeside.span();
    }
    return { };
}

ValueID BigIntValues::quotientForRemainder(ValueID remainder) const
{
    const Operation& operation = m_code.at(remainder);
    if (operation.opcode != Opcode::Mod || !isArithmetic(operation))
        return { };
    for (ValueID value : m_code.valuesOf(m_code.blockOf(remainder))) {
        if (value == remainder)
            break;
        const Operation& quotient = m_code.at(value);
        if (quotient.opcode == Opcode::Div && isArithmetic(quotient) && isLikely(value) && !m_passesOn.get(value.index()) && quotient.leftOperand() == operation.leftOperand() && quotient.rightOperand() == operation.rightOperand())
            return value;
    }
    return { };
}

class BigIntValues::Liveness {
public:
    Liveness(const Code& code, const BitVector& values)
        : m_code(code)
    {
        m_numbers.grow(code.numberOfValues());
        for (uint32_t index = 0; index < code.numberOfValues(); ++index)
            m_numbers[index] = values.get(index) ? m_count++ : noNumber;
        m_liveAtHead.grow(code.numberOfBlocks());
        for (bool changes = true; changes;) {
            changes = false;
            for (uint32_t number = code.numberOfBlocks(); number--;) {
                BlockID block(number);
                if (!code.isReached(block))
                    continue;
                BitVector live = liveAtEnd(block);
                std::span<const ValueID> values = code.valuesOf(block);
                for (size_t index = values.size(); index--;)
                    stepOver(values[index], live);
                for (ValueID parameter : code.at(block).parameters())
                    end(parameter, live);
                live.merge(m_liveAtHead[number]);
                if (live.bitCount() != m_liveAtHead[number].bitCount()) {
                    m_liveAtHead[number] = WTF::move(live);
                    changes = true;
                }
            }
        }
    }

    void computeLiveAfter(ValueID value)
    {
        BlockID block = m_code.blockOf(value);
        m_live = liveAtEnd(block);
        std::span<const ValueID> values = m_code.valuesOf(block);
        for (size_t index = values.size(); index-- && values[index] != value;)
            stepOver(values[index], m_live);
    }
    bool isLive(uint32_t index) const { return m_numbers[index] != noNumber && m_live.get(m_numbers[index]); }

private:
    static constexpr uint32_t noNumber = std::numeric_limits<uint32_t>::max();

    BitVector liveAtEnd(BlockID id) const
    {
        const Block& block = m_code.at(id);
        BitVector live(m_count);
        for (uint32_t target = 0; target < block.terminator.numberOfEdges; ++target) {
            const Edge& edge = m_code.at(block.terminator.edge(target));
            live.merge(m_liveAtHead[edge.to.index()]);
            for (ValueID argument : edge.arguments())
                read(argument, live);
        }
        if (block.handler)
            live.merge(m_liveAtHead[m_code.at(block.handler).to.index()]);
        block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            read(operand, live);
        });
        return live;
    }

    void stepOver(ValueID value, BitVector& live) const
    {
        end(value, live);
        m_code.at(value).forEachOperand([&](ValueID operand, uint32_t) {
            read(operand, live);
        });
    }

    void end(ValueID value, BitVector& live) const
    {
        if (uint32_t number = m_numbers[value.index()]; number != noNumber)
            live.clear(number);
    }

    void read(ValueID value, BitVector& live) const
    {
        auto readOne = [&](ValueID one) {
            if (uint32_t number = m_numbers[one.index()]; number != noNumber)
                live.set(number);
        };
        readOne(value);
        const Operation& operation = m_code.at(value);
        if (operation.opcode == Opcode::Mul)
            operation.forEachOperand([&](ValueID factor, uint32_t) {
                readOne(factor);
            });
    }

    const Code& m_code;
    uint32_t m_count { 0 };
    Vector<uint32_t> m_numbers;
    Vector<BitVector> m_liveAtHead;
    BitVector m_live;
};

void BigIntValues::findCarriedValues(std::span<const Flow> flows, const BitVector& isProducer)
{
    uint32_t numberOfValues = m_code.numberOfValues();
    auto isParameterOfLoop = [&](ValueID value) {
        BlockID block = m_code.blockOf(value);
        return block && m_code.at(block).isLoopHead && std::ranges::contains(m_code.at(block).parameters(), value);
    };
    BitVector all(numberOfValues);
    Vector<BitVector> reachedByCarried;
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        if (!isProducer.quickGet(index) || !m_mayBeInFrame.quickGet(index) || !m_passesOn.quickGet(index))
            continue;
        BitVector reached(numberOfValues);
        reached.quickSet(index);
        bool isCarried = false;
        for (bool changes = true; changes;) {
            changes = false;
            for (const Flow& flow : flows) {
                if (!reached.quickGet(flow.from.index()) || reached.quickGet(flow.to.index()))
                    continue;
                reached.quickSet(flow.to.index());
                isCarried |= isParameterOfLoop(flow.to);
                changes = true;
            }
        }
        if (!isCarried)
            continue;
        all.merge(reached);
        reached.quickClear(index);
        m_carriedValues.append({ ValueID(index), { } });
        reachedByCarried.append(WTF::move(reached));
    }
    if (m_carriedValues.isEmpty())
        return;

    Liveness liveness(m_code, all);
    for (size_t place = 0; place < m_carriedValues.size(); ++place) {
        CarriedValue& carried = m_carriedValues[place];
        const Operation& operation = m_code.at(carried.value);
        bool mayWriteOverItsOperands = mayWriteOverOperands.contains(operation.opcode) || quotientForRemainder(carried.value);
        liveness.computeLiveAfter(carried.value);
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            if (!reachedByCarried[place].quickGet(index))
                continue;
            if (liveness.isLive(index) || (!mayWriteOverItsOperands && std::ranges::contains(operation.operands(), ValueID(index))))
                carried.keptBeside.append(ValueID(index));
        }
        if (carried.keptBeside.size() > maxValuesKept) {
            m_mayBeInFrame.quickClear(carried.value.index());
            carried.keptBeside.clear();
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
