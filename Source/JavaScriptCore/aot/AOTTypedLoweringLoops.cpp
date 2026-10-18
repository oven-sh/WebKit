/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

void Lowering::computeLoops(const Vector<BlockID>& order, const PredecessorLists& predecessors)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    m_innermostLoop.fill(noLoop, numberOfBlocks);
    m_definingBlock.fill(BlockID(), m_function.values.size());
    for (BlockID block : order) {
        for (ValueID parameter : m_function.at(block).parameters)
            m_definingBlock[parameter.index()] = block;
        for (ValueID value : m_function.at(block).values)
            m_definingBlock[value.index()] = block;
    }
    for (BlockID header : order) {
        bool hasBackEdge = std::ranges::any_of(predecessors[header.index()], [&](unsigned predecessor) {
            return dominates(header, BlockID(predecessor));
        });
        if (!hasBackEdge)
            continue;
        Loop loop;
        loop.header = header;
        loop.blocks.ensureSize(numberOfBlocks);
        loop.blocks.set(header.index());
        Vector<unsigned, 16> worklist;
        for (unsigned predecessor : predecessors[header.index()]) {
            if (dominates(header, BlockID(predecessor)) && !loop.blocks.set(predecessor))
                worklist.append(predecessor);
        }
        while (!worklist.isEmpty()) {
            for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                if (!loop.blocks.set(predecessor))
                    worklist.append(predecessor);
            }
        }
        for (size_t block : loop.blocks)
            m_innermostLoop[block] = m_loops.size();
        m_loops.append(WTF::move(loop));
    }
}

bool Lowering::isLoopInvariant(const Loop& loop, ValueID id) const
{
    BlockID definingBlock = m_definingBlock[id.index()];
    return definingBlock && !loop.blocks.get(definingBlock.index());
}

bool Lowering::isGuaranteedToExecute(const Loop& loop) const
{
    static constexpr unsigned maxNumberOfBlocks = 32;
    LBasicBlock block = m_blocks[loop.header.index()];
    for (unsigned i = 0; i < maxNumberOfBlocks && block != m_out.m_block; ++i) {
        LBasicBlock next = nullptr;
        for (auto& successor : block->successors()) {
            if (successor.frequency() == FrequencyClass::Rare)
                continue;
            if (next)
                return false;
            next = successor.block();
        }
        if (!next)
            return false;
        block = next;
    }
    return block == m_out.m_block;
}

bool Lowering::willHoistComputationOn(ValueID operand) const
{
    unsigned index = m_innermostLoop[m_currentBlockID.index()];
    if (index == noLoop)
        return false;
    return isLoopInvariant(m_loops[index], operand) && isGuaranteedToExecute(m_loops[index]);
}

void Lowering::findCalleesCalledInLoops()
{
    if (m_loops.isEmpty())
        return;
    for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
        if (m_innermostLoop[b] == noLoop)
            continue;
        for (ValueID id : m_function.blocks[b].values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::Call || value.arguments.size() > numberOfArgumentGPRs || value.speculatedMethod != SpeculatedMethod::None || value.isSignatureCall)
                continue;
            ValueID callee = value.child(0);
            if (m_function.at(callee).predictedClosure())
                continue;
            while (m_function.at(callee).opcode == Opcode::Cast)
                callee = m_function.at(callee).child(0);
            if (m_function.at(callee).predictedClosure() || repOf(typeOf(callee)) != Rep::JSValue)
                continue;
            auto& enclosingFunction = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
            if (!enclosingFunction.has(Format::FunctionIsBuiltin))
                continue;
            for (unsigned i = 0; i < m_loops.size(); ++i) {
                auto& loop = m_loops[i];
                if (!loop.blocks.get(b) || !loop.header.index() || !isLoopInvariant(loop, callee))
                    continue;
                if (m_calleesResolvedBeforeLoops.isEmpty())
                    m_calleesResolvedBeforeLoops.grow(m_loops.size());
                auto& resolvedBeforeLoop = m_calleesResolvedBeforeLoops[i];
                unsigned numberOfArguments = value.arguments.size();
                size_t index = resolvedBeforeLoop.findIf([&](auto& resolved) {
                    return resolved.callee == callee && resolved.numberOfArguments == numberOfArguments;
                });
                if (index == notFound) {
                    index = resolvedBeforeLoop.size();
                    resolvedBeforeLoop.append({ callee, numberOfArguments, m_proc.addVariable(Int64) });
                }
                m_callsOfResolvedCallees.add(id.index(), resolvedBeforeLoop[index]);
                break;
            }
        }
    }
}

void Lowering::resolveCalleesBeforeLoops(const Terminator& terminator)
{
    if (m_calleesResolvedBeforeLoops.isEmpty())
        return;
    for (auto& target : successorsOf(terminator)) {
        for (unsigned i = 0; i < m_loops.size(); ++i) {
            if (m_loops[i].header != target.block || m_loops[i].blocks.get(m_currentBlockID.index()))
                continue;
            for (auto& resolved : m_calleesResolvedBeforeLoops[i]) {
                m_context.emitsCalls = true;
                PatchpointValue* patchpoint = m_out.patchpoint(Int64);
                applyEffects(patchpoint, { Writes::Nothing, false });
                patchpoint->append(ConstrainedValue(in<Boxed>(resolved.callee).value(), ValueRep::reg(calleeGPR)));
                patchpoint->clobber(RegisterSet::macroClobberedGPRs());
                patchpoint->clobberLate(RegisterSet { callMarkerGPR });
                for (GPRReg reg : stubTemporaryGPRs)
                    patchpoint->clobberLate(RegisterSet { reg });
                patchpoint->resultConstraints = { ValueRep::reg(stubTemporaryGPRs[2]) };
                patchpoint->setGenerator([stubCalls = &m_context.stubCalls, count = resolved.numberOfArguments](CCallHelpers& jit, const StackmapGenerationParams&) {
                    AllowMacroScratchRegisterUsage allowScratch(jit);
                    stubCalls->call(jit, Stub::ResolveCallValue, count, CallSite { });
                });
                m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), resolved.target, patchpoint);
            }
        }
    }
}

std::optional<Lowering::ResolvedCall> Lowering::resolvedCallAt(ValueID call)
{
    auto found = m_callsOfResolvedCallees.find(call.index());
    if (found == m_callsOfResolvedCallees.end())
        return std::nullopt;
    LValue target = m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), found->value.target);
    return ResolvedCall { in<Boxed>(found->value.callee).value(), target, found->value.numberOfArguments };
}

Integer Lowering::integerBound(Opcode opcode, Number number)
{
    bool roundsUp = opcode == Opcode::Less || opcode == Opcode::GreaterEq;
    bool holdsForMinimum = opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
    constexpr int64_t minimum = std::numeric_limits<int64_t>::min();
    constexpr int64_t greatest = std::numeric_limits<int64_t>::max();
    if (number.value()->hasDouble()) {
        double constant = number.value()->asDouble();
        if (std::isnan(constant))
            return integer(holdsForMinimum ? greatest : minimum);
        double rounded = roundsUp ? std::ceil(constant) : std::floor(constant);
        return integer(rounded >= 0x1p63 ? greatest : rounded < -0x1p63 ? minimum : static_cast<int64_t>(rounded));
    }
    Integer rounded = truncateToInteger(roundsUp ? ceil(number) : floor(number));
    Number beyond = this->number(0x1p63);
    if (holdsForMinimum)
        return select(lessThan(number, beyond), rounded, integer(greatest));
    return select(greaterThanOrEqual(number, beyond), integer(greatest), select(equal(number, number), rounded, integer(minimum)));
}

std::optional<Word32> Lowering::lowerRelationalWithIntegerBound(const Value& value)
{
    bool integerIsFirst = typeOf(value.child(0)).isInteger();
    ValueID integerOperand = value.child(integerIsFirst ? 0 : 1);
    ValueID numberOperand = value.child(integerIsFirst ? 1 : 0);
    auto& numberType = typeOf(numberOperand);
    if (!typeOf(integerOperand).isInteger() || !numberType.isNumber() || numberType.isInteger())
        return std::nullopt;
    if (m_function.at(numberOperand).opcode != Opcode::Double && !willHoistComputationOn(numberOperand))
        return std::nullopt;
    Opcode opcode = value.opcode;
    if (!integerIsFirst) {
        switch (opcode) {
        case Opcode::Less:
            opcode = Opcode::Greater;
            break;
        case Opcode::LessEq:
            opcode = Opcode::GreaterEq;
            break;
        case Opcode::Greater:
            opcode = Opcode::Less;
            break;
        default:
            opcode = Opcode::LessEq;
            break;
        }
    }
    Integer x = in<Integer>(integerOperand);
    Integer bound = integerBound(opcode, in<Number>(numberOperand));
    switch (opcode) {
    case Opcode::Less:
        return lessThan(x, bound);
    case Opcode::LessEq:
        return lessThanOrEqual(x, bound);
    case Opcode::Greater:
        return greaterThan(x, bound);
    default:
        return greaterThanOrEqual(x, bound);
    }
}

std::optional<Word32> Lowering::lowerRelationalWithLikelyInteger(const Value& value)
{
    if (m_likelyIntegerNumbers.isEmpty())
        return std::nullopt;
    bool integerIsFirst = typeOf(value.child(0)).isInteger();
    ValueID integerOperand = value.child(integerIsFirst ? 0 : 1);
    ValueID numberOperand = value.child(integerIsFirst ? 1 : 0);
    if (!typeOf(integerOperand).isInteger())
        return std::nullopt;
    auto found = m_likelyIntegerNumbers.find(numberOperand.index());
    if (found == m_likelyIntegerNumbers.end() || get(numberOperand).value != found->value.number)
        return std::nullopt;
    Integer savedInteger(found->value.integer);
    bool savedIsNonNegative = found->value.minimum >= 0;
    bool neitherIsNegative = savedIsNonNegative && typeOf(integerOperand).integers.minimum >= 0;
    return either<Word32>(savedIsNonNegative ? greaterThanOrEqual(savedInteger, integer(0)) : greaterThan(savedInteger, integer(INT64_MIN)), [&] {
        Integer x = integerIsFirst ? in<Integer>(integerOperand) : savedInteger;
        Integer y = integerIsFirst ? savedInteger : in<Integer>(integerOperand);
        switch (value.opcode) {
        case Opcode::Less:
            return neitherIsNegative ? below(x, y) : lessThan(x, y);
        case Opcode::Greater:
            return neitherIsNegative ? below(y, x) : greaterThan(x, y);
        case Opcode::LessEq:
            return neitherIsNegative ? belowOrEqual(x, y) : lessThanOrEqual(x, y);
        default:
            return neitherIsNegative ? belowOrEqual(y, x) : greaterThanOrEqual(x, y);
        }
    }, [&] {
        Number x = asNumericOperand(value.child(0));
        Number y = asNumericOperand(value.child(1));
        switch (value.opcode) {
        case Opcode::Less:
            return lessThan(x, y);
        case Opcode::Greater:
            return greaterThan(x, y);
        case Opcode::LessEq:
            return lessThanOrEqual(x, y);
        default:
            return greaterThanOrEqual(x, y);
        }
    });
}

std::optional<Word32> Lowering::lowerRelationalWithOptionalInteger(const Value& value)
{
    auto isKeptAsOptionalInteger = [&](ValueID operand) {
        auto& type = typeOf(operand);
        return !type.isInteger() && isNumericOperand(type) && !(type.primitives & Primitive::Null) && get(operand).rep == Rep::OptionalInt64;
    };
    if (isKeptAsOptionalInteger(value.child(0)) && isKeptAsOptionalInteger(value.child(1))) {
        static_assert(undefinedInteger > maxExactInteger);
        Integer x = assumedInteger(get(value.child(0)).as<OptionalInteger>());
        Integer y = assumedInteger(get(value.child(1)).as<OptionalInteger>());
        bool asksWhetherFirstIsGreater = value.opcode == Opcode::Greater || value.opcode == Opcode::GreaterEq;
        Word32 greaterIsDefined = lessThan(asksWhetherFirstIsGreater ? x : y, integer(undefinedInteger));
        switch (value.opcode) {
        case Opcode::Less:
            return logicalAnd(lessThan(x, y), greaterIsDefined);
        case Opcode::Greater:
            return logicalAnd(greaterThan(x, y), greaterIsDefined);
        case Opcode::LessEq:
            return logicalAnd(lessThanOrEqual(x, y), greaterIsDefined);
        default:
            return logicalAnd(greaterThanOrEqual(x, y), greaterIsDefined);
        }
    }
    bool integerIsFirst = typeOf(value.child(0)).isInteger();
    ValueID integerOperand = value.child(integerIsFirst ? 0 : 1);
    ValueID optionalOperand = value.child(integerIsFirst ? 1 : 0);
    auto& optionalType = typeOf(optionalOperand);
    if (!typeOf(integerOperand).isInteger() || !isNumericOperand(optionalType) || optionalType.primitives & Primitive::Null)
        return std::nullopt;
    Operand optional = get(optionalOperand);
    if (optional.rep != Rep::OptionalInt64)
        return std::nullopt;
    Integer rawInteger = assumedInteger(optional.as<OptionalInteger>());
    bool neitherIsNegative = optionalType.integers.minimum >= 0 && typeOf(integerOperand).integers.minimum >= 0;
    static_assert(undefinedInteger > maxExactInteger);
    bool asksWhetherOptionalIsSmaller = integerIsFirst ? value.opcode == Opcode::Greater || value.opcode == Opcode::GreaterEq : value.opcode == Opcode::Less || value.opcode == Opcode::LessEq;
    if (asksWhetherOptionalIsSmaller && typeOf(integerOperand).integers.maximum <= maxExactInteger) {
        Integer bound = in<Integer>(integerOperand);
        if (value.opcode == Opcode::Less || value.opcode == Opcode::Greater)
            return neitherIsNegative ? below(rawInteger, bound) : lessThan(rawInteger, bound);
        return neitherIsNegative ? belowOrEqual(rawInteger, bound) : lessThanOrEqual(rawInteger, bound);
    }
    return either<Word32>(lessThan(rawInteger, integer(undefinedInteger)), [&] {
        Integer x = integerIsFirst ? in<Integer>(integerOperand) : rawInteger;
        Integer y = integerIsFirst ? rawInteger : in<Integer>(integerOperand);
        switch (value.opcode) {
        case Opcode::Less:
            return neitherIsNegative ? below(x, y) : lessThan(x, y);
        case Opcode::Greater:
            return neitherIsNegative ? below(y, x) : greaterThan(x, y);
        case Opcode::LessEq:
            return neitherIsNegative ? belowOrEqual(x, y) : lessThanOrEqual(x, y);
        default:
            return neitherIsNegative ? belowOrEqual(y, x) : greaterThanOrEqual(x, y);
        }
    }, [&] {
        return boolean(false);
    });
}

std::optional<Word32> Lowering::sumToInt32(ValueID id)
{
    auto& value = m_function.at(id);
    if ((value.opcode != Opcode::Add && value.opcode != Opcode::Sub) || !operandsAreAvailable(id))
        return std::nullopt;
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (!isNumericOperand(left) || !isNumericOperand(right) || (!left.isInteger() && !right.isInteger()))
        return std::nullopt;
    if ((!left.isInteger() || !right.isInteger()) && (useCountOf(id) != 1 || m_function.at(left.isInteger() ? a : b).opcode == Opcode::Integer))
        return std::nullopt;

    bool isAdd = value.opcode == Opcode::Add;
    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    std::array<Integer, 2> savedWithNumber;
    auto asInteger = [&](ValueID operand, const Type& type) {
        if (type.isInteger())
            return in<Integer>(operand);
        if (auto found = m_numbersAsIntegers.find(operand.index()); found != m_numbersAsIntegers.end() && found->value.minimum && *found->value.minimum >= 0 && get(operand).value == found->value.number) {
            Integer savedInteger(found->value.integer);
            branchUnless(below(savedInteger, integer(1ll << 31)), otherwise);
            savedWithNumber[operand == b] = savedInteger;
            return savedInteger;
        }
        Number number = asNumericOperand(operand);
        Word32 low = low32(truncateToInteger(number));
        branchUnless(equal(toNumber(low), number), otherwise);
        return signExtended(low);
    };
    Integer x = asInteger(a, left);
    Integer y = asInteger(b, right);
    Range leftRange = left.isInteger() ? left.integers : Range::int32();
    Range rightRange = right.isInteger() ? right.integers : Range::int32();
    Int128 minimum = isAdd ? static_cast<Int128>(leftRange.minimum) + rightRange.minimum : static_cast<Int128>(leftRange.minimum) - rightRange.maximum;
    Int128 maximum = isAdd ? static_cast<Int128>(leftRange.maximum) + rightRange.maximum : static_cast<Int128>(leftRange.maximum) - rightRange.minimum;
    Integer result = isAdd ? add(x, y) : sub(x, y);
    if (maximum > maxExactInteger)
        branchUnless(lessThanOrEqual(result, integer(maxExactInteger)), otherwise);
    if (minimum < -maxExactInteger || maximum >= static_cast<Int128>(1) << 63)
        branchUnless(greaterThanOrEqual(result, integer(-maxExactInteger)), otherwise);
    join.add(low32(result));

    m_out.appendTo(otherwise);
    auto asNumber = [&](ValueID operand, Integer savedInteger) {
        if (!savedInteger)
            return asNumericOperand(operand);
        return select(equal(savedInteger, integer(INT64_MIN)), number(PNaN), toNumber(savedInteger));
    };
    Number p = asNumber(a, a == b ? Integer() : savedWithNumber[0]);
    Number q = asNumber(b, savedWithNumber[1]);
    join.add(toInt32(isAdd ? add(p, q) : sub(p, q)));
    return join.finish().as<Word32>();
}

Integer Lowering::clampToMaximum(Integer value, int64_t limit)
{
    Word32 isWithinLimit = lessThanOrEqual(value, integer(limit));
    if (!m_isInLoop)
        return select(isWithinLimit, value, integer(limit));
    return either<Integer>(isWithinLimit, [&] {
        return value;
    }, [&] {
        return integer(limit);
    });
}

Integer Lowering::clampToMinimum(Integer value, int64_t limit)
{
    Word32 isWithinLimit = greaterThanOrEqual(value, integer(limit));
    if (!m_isInLoop)
        return select(isWithinLimit, value, integer(limit));
    return either<Integer>(isWithinLimit, [&] {
        return value;
    }, [&] {
        return integer(limit);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
