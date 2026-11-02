/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

static constexpr uint32_t mappedByteCount = RandomProgram::numberOfWords * sizeof(int64_t);
static_assert(RandomProgram::numberOfGuardWords >= RandomProgram::wordsPerRegion - 1);

static bool promisesMappedMemory(const WeakRandom& promiseRandom)
{
    return WeakRandom(promiseRandom.seed() + 0x165667b1u).getUint32(100) < 50;
}

Value* RandomProgram::constant()
{
    static constexpr int64_t constants[] = {
        0, 1, -1, 2, 3, 5, 7, 8, 15, 16, 31, 32, 63, 64, 255, 256, 1000, -2, -8, -128, 0x7fffffff, -0x80000000ll, 0xffffffffll, 0x100000000ll,
        0x123456789abcdefll, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()
    };
    return int64(constants[pick(std::size(constants))]);
}

Value* RandomProgram::randomLoopIndex()
{
    Vector<Variable*, 4> indices;
    for (auto& loop : m_loops) {
        if (loop.index)
            indices.append(loop.index);
    }
    if (indices.isEmpty())
        return nullptr;
    Variable* index = chance(70) ? indices.last() : indices[pick(indices.size())];
    Value* result = get(index);
    if (index->type() == Int32)
        return make(chance(50) ? SExt32 : ZExt32, result);
    return result;
}

Value* RandomProgram::leaf()
{
    switch (pick(10)) {
    case 0:
    case 1:
        return constant();
    case 2:
    case 3:
    case 4:
        return get(randomVariable());
    case 5:
        return m_arguments[pick(m_arguments.size())];
    case 6:
        if (!m_available.isEmpty())
            return m_available[pick(m_available.size())];
        return constant();
    case 7:
    case 8:
        if (Value* index = randomLoopIndex())
            return index;
        return get(randomVariable());
    default:
        return load(0);
    }
}

Value* RandomProgram::expression(unsigned depth)
{
    static constexpr B3::Opcode arithmetic[] = { Add, Sub, Mul, BitAnd, BitOr, BitXor };
    if (!depth || chance(20))
        return leaf();
    --depth;
    switch (pick(15)) {
    case 0:
    case 1:
    case 2:
    case 3: {
        B3::Opcode opcode = arithmetic[pick(std::size(arithmetic))];
        Value* left = expression(depth);
        Value* right = expression(depth);
        return make(opcode, left, right);
    }
    case 4: {
        static constexpr B3::Opcode shifts[] = { Shl, SShr, ZShr };
        B3::Opcode opcode = shifts[pick(std::size(shifts))];
        Value* value = expression(depth);
        Value* amount = chance(70) ? int32(pick(64)) : make(Trunc, expression(depth));
        return make(opcode, value, amount);
    }
    case 5:
        return make(ZExt32, condition(depth));
    case 6: {
        Value* predicate = condition(depth);
        Value* ifTrue = expression(depth);
        Value* ifFalse = expression(depth);
        return m_graph.select(m_block, predicate, ifTrue, ifFalse);
    }
    case 7:
    case 8: {
        B3::Opcode opcode = arithmetic[pick(std::size(arithmetic))];
        Value* left = make(Trunc, expression(depth));
        Value* right = make(Trunc, expression(depth));
        Value* result = make(opcode, left, right);
        return make(chance(50) ? SExt32 : ZExt32, result);
    }
    case 9: {
        Value* left = expression(depth);
        Value* right = expression(depth);
        return m_block->appendNew<Value>(m_proc, chill(chance(50) ? Div : Mod), Origin(), left, right);
    }
    case 10: {
        Value* result = doubleExpression(depth + 1);
        switch (pick(4)) {
        case 0:
            return make(BitwiseCast, result);
        case 1:
            return make(SExt32, convertToInteger(result, Int32));
        default:
            return convertToInteger(result, Int64);
        }
    }
    case 11:
    case 12:
        return load(depth);
    case 13:
        switch (pick(4)) {
        case 0:
            return make(Opaque, expression(depth));
        case 1:
            return make(Identity, expression(depth));
        default:
            return make(Neg, expression(depth));
        }
    default:
        return make(SExt32, make(chance(50) ? SExt8 : SExt16, make(Trunc, expression(depth))));
    }
}

Value* RandomProgram::doubleExpression(unsigned depth)
{
    static constexpr double numbers[] = { 0, -0.0, 0.5, 1, -1, 2, 1.5, 100, 2147483647, 2147483648.0, -2147483648.0, -2147483649.0, 4294967296.0, 1e10 };
    static constexpr B3::Opcode opcodes[] = { Add, Sub, Mul };
    unsigned kind = pick(10);
    if (depth && kind < 3) {
        B3::Opcode opcode = opcodes[pick(std::size(opcodes))];
        Value* left = doubleExpression(depth - 1);
        Value* right = doubleExpression(depth - 1);
        return make(opcode, left, right);
    }
    if (depth && kind < 4)
        return make(chance(50) ? Neg : Abs, doubleExpression(depth - 1));
    if (kind < 6)
        return m_graph.number(m_block, numbers[pick(std::size(numbers))]);
    if (kind < 9)
        return make(IToD, make(Trunc, expression(depth)));
    return make(IToD, expression(depth));
}

Value* RandomProgram::convertToInteger(Value* number, Type type)
{
    PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, type, Origin());
    result->effects = Effects::none();
    if (type == Int32 || chance(50))
        result->pureFunction = PureFunction { conversionFunctionID, true };
    result->appendSomeRegister(number);
    result->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    return modeled(result, Model::TruncateDouble);
}

Value* RandomProgram::condition(unsigned depth)
{
    static constexpr B3::Opcode relations[] = { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual };
    switch (pick(8)) {
    case 0: {
        B3::Opcode relation = relations[pick(std::size(relations))];
        Value* left = make(Trunc, expression(depth));
        Value* right = chance(50) ? int32(static_cast<int32_t>(pick(20)) - 4) : make(Trunc, expression(depth));
        return make(relation, left, right);
    }
    case 1: {
        B3::Opcode relation = relations[pick(6)];
        Value* left = doubleExpression(depth);
        Value* right = doubleExpression(depth);
        return make(relation, left, right);
    }
    case 2:
        return make(chance(50) ? NotEqual : Equal, make(BitAnd, expression(depth), int64(1ll << pick(10))), int64(0));
    case 3:
        if (depth)
            return make(BitXor, condition(depth - 1), int32(1));
        [[fallthrough]];
    default: {
        B3::Opcode relation = relations[pick(std::size(relations))];
        Value* left = expression(depth);
        Value* right = chance(40) ? constant() : expression(depth);
        return make(relation, left, right);
    }
    }
}

auto RandomProgram::memoryOperand(unsigned region, unsigned depth) -> MemoryOperand
{
    int32_t offset = region * wordsPerRegion * sizeof(int64_t);
    HeapRange range = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
    if (chance(15))
        range = region < 2 && chance(50) ? bothHeaps : allHeaps;
    else if (chance(5))
        range = HeapRange::top();
    if (chance(55))
        return { m_pointer, static_cast<int32_t>(offset + pick(chance(60) ? 3 : wordsPerRegion) * sizeof(int64_t)), range };
    Value* index = chance(60) ? randomLoopIndex() : nullptr;
    if (!index)
        index = expression(depth);
    return { wordAddress(index), offset, range };
}

Value* RandomProgram::wordAddress(Value* index)
{
    return make(Add, m_pointer, make(Shl, make(BitAnd, index, int64(wordsPerRegion - 1)), int32(3)));
}

Value* RandomProgram::load(unsigned depth)
{
    static constexpr B3::Opcode narrowLoads[] = { Load8Z, Load8S, Load16Z, Load16S };
    unsigned region = pick(3);
    MemoryOperand operand = memoryOperand(region, depth);
    unsigned width = pick(100);
    MemoryValue* result;
    if (width < 70)
        result = m_graph.load(m_block, Int64, operand.base, operand.offset, operand.range);
    else if (width < 90)
        result = m_graph.load(m_block, Int32, operand.base, operand.offset + (chance(50) ? 4 : 0), operand.range);
    else
        result = m_block->appendNew<MemoryValue>(m_proc, narrowLoads[pick(std::size(narrowLoads))], Origin(), operand.base, operand.offset + static_cast<int32_t>(2 * pick(4)), operand.range);
    if (region == 2 && chance(50))
        result->setReadsMutability(Mutability::Immutable);
    if (promisesMappedMemory(m_promiseRandom) && mayHaveKnownDereferenceableBytes(operand.base))
        m_proc.setKnownDereferenceableBytes(operand.base, mappedByteCount);
    if (result->type() == Int32)
        return make(chance(50) ? SExt32 : ZExt32, result);
    return result;
}

Value* RandomProgram::loadSmallWord()
{
    MemoryValue* result = m_graph.load(m_block, Int32, m_pointer, (2 * wordsPerRegion + pick(numberOfSmallWords)) * sizeof(int64_t), thirdHeap);
    if (m_promiseRandom.getUint32(100) < 50)
        m_proc.setKnownRange(result, IntRange(0, smallWordLimit - 1));
    if (chance(50))
        result->setReadsMutability(Mutability::Immutable);
    if (promisesMappedMemory(m_promiseRandom))
        m_proc.setKnownDereferenceableBytes(m_pointer, mappedByteCount);
    return make(ZExt32, result);
}

void RandomProgram::store()
{
    MemoryOperand operand = memoryOperand(pick(2), 1);
    Value* value = expression(2);
    unsigned width = pick(100);
    if (width < 70)
        m_graph.store(m_block, value, operand.base, operand.offset, operand.range);
    else if (width < 90)
        m_graph.store(m_block, make(Trunc, value), operand.base, operand.offset + (chance(50) ? 4 : 0), operand.range);
    else
        m_block->appendNew<MemoryValue>(m_proc, chance(50) ? Store8 : Store16, Origin(), make(Trunc, value), operand.base, operand.offset + static_cast<int32_t>(2 * pick(4)), operand.range);
}

#endif // ENABLE(B3_JIT)
