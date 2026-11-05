/*
 * Copyright (C) 2015-2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3LowerMacrosAfterOptimizations.h"

#if ENABLE(B3_JIT)

#include "AirArg.h"
#include "B3BlockInsertionSet.h"
#include "B3BulkMemoryValue.h"
#include "B3CCallValue.h"
#include "B3ConstDoubleValue.h"
#include "B3ConstFloatValue.h"
#include "B3InsertionSetInlines.h"
#include "B3Operations.h"
#include "B3PhaseScope.h"
#include "B3RelocatableValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

using Arg = Air::Arg;
using Code = Air::Code;
using Tmp = Air::Tmp;

namespace {

constexpr bool isLoweredByLowerMacrosAfterOptimizations(Opcode opcode)
{
    switch (opcode) {
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Neg:
    case PurifyNaN:
    case RotL:
    case PopCount:
    case MemoryMove:
    case MemoryFill:
        return true;
    default:
        return false;
    }
}

class LowerMacrosAfterOptimizations {
public:
    LowerMacrosAfterOptimizations(Procedure& proc)
        : m_proc(proc)
        , m_blockInsertionSet(proc)
        , m_insertionSet(proc)
    {
    }

    bool run()
    {
        bool mayFindWork = m_proc.hasEverAddedAny(isLoweredByLowerMacrosAfterOptimizations);
        if (!mayFindWork && !shouldValidateIR(m_proc))
            return false;
        for (BasicBlock* block : m_proc) {
            m_block = block;
            processCurrentBlock();
        }
        m_changed |= m_blockInsertionSet.execute();
        RELEASE_ASSERT(mayFindWork || (!m_changed && !m_didInsertValues));
        if (m_changed) {
            m_proc.resetReachability();
            m_proc.invalidateCFG();
        }
        return m_changed;
    }
    
private:
    void processCurrentBlock()
    {
        for (m_index = 0; m_index < m_block->size(); ++m_index) {
            m_value = m_block->at(m_index);
            m_origin = m_value->origin();
            switch (m_value->opcode()) {
            case Abs: {
                // ARM supports this instruction natively.
                if (isARM64())
                    break;

                Value* mask = nullptr;
                if (m_value->type() == Double)
                    mask = m_insertionSet.insert<ConstDoubleValue>(m_index, m_origin, std::bit_cast<double>(~(1ll << 63)));
                else if (m_value->type() == Float)
                    mask = m_insertionSet.insert<ConstFloatValue>(m_index, m_origin, std::bit_cast<float>(~(1 << 31)));
                else
                    RELEASE_ASSERT_NOT_REACHED();
                Value* result = m_insertionSet.insert<Value>(m_index, BitAnd, m_origin, m_value->child(0), mask);
                m_value->replaceWithIdentity(result);
                break;
            }
            case Ceil: {
                if (m_proc.policy().supportsFloatingPointRounding())
                    break;

                Value* functionAddress = nullptr;
                if (m_value->type() == Double)
                    functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::CeilDouble, m_origin));
                else if (m_value->type() == Float)
                    functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::CeilFloat, m_origin));
                else
                    RELEASE_ASSERT_NOT_REACHED();

                Value* result = m_insertionSet.insert<CCallValue>(m_index,
                    m_value->type(),
                    m_origin,
                    Effects::none(),
                    functionAddress,
                    m_value->child(0));
                m_value->replaceWithIdentity(result);
                break;
            }
            case Floor: {
                if (m_proc.policy().supportsFloatingPointRounding())
                    break;

                Value* functionAddress = nullptr;
                if (m_value->type() == Double)
                    functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::FloorDouble, m_origin));
                else if (m_value->type() == Float)
                    functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::FloorFloat, m_origin));
                else
                    RELEASE_ASSERT_NOT_REACHED();

                Value* result = m_insertionSet.insert<CCallValue>(m_index,
                    m_value->type(),
                    m_origin,
                    Effects::none(),
                    functionAddress,
                    m_value->child(0));
                m_value->replaceWithIdentity(result);
                break;
            }
            case FTrunc: {
                if (m_proc.policy().supportsFloatingPointRounding())
                    break;

                Value* functionAddress = nullptr;
                if (m_value->type() == Double)
                    functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::TruncDouble, m_origin));
                else if (m_value->type() == Float)
                    functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::TruncFloat, m_origin));
                else
                    RELEASE_ASSERT_NOT_REACHED();

                Value* result = m_insertionSet.insert<CCallValue>(m_index,
                    m_value->type(),
                    m_origin,
                    Effects::none(),
                    functionAddress,
                    m_value->child(0));
                m_value->replaceWithIdentity(result);
                break;
            }

            case Neg: {
                if (!m_value->type().isFloat())
                    break;
                
                // X86 is odd in that it requires this.
                if (!isX86())
                    break;

                Value* mask = nullptr;
                if (m_value->type() == Double)
                    mask = m_insertionSet.insert<ConstDoubleValue>(m_index, m_origin, -0.0);
                else {
                    RELEASE_ASSERT(m_value->type() == Float);
                    mask = m_insertionSet.insert<ConstFloatValue>(m_index, m_origin, -0.0f);
                }

                Value* result = m_insertionSet.insert<Value>(
                    m_index, BitXor, m_origin, m_value->child(0), mask);
                m_value->replaceWithIdentity(result);
                break;
            }

            case PurifyNaN: {
                if (m_value->type() == Double) {
                    auto* pureNaN = m_insertionSet.insert<ConstDoubleValue>(m_index, m_value->origin(), PNaN);
                    auto* compare = m_insertionSet.insert<Value>(m_index, Equal, m_value->origin(), m_value->child(0), m_value->child(0));
                    auto* result = m_insertionSet.insert<Value>(m_index, Select, m_value->origin(), compare, m_value->child(0), pureNaN);
                    m_value->replaceWithIdentity(result);
                } else {
                    auto* pureNaN = m_insertionSet.insert<ConstFloatValue>(m_index, m_value->origin(), static_cast<float>(PNaN));
                    auto* compare = m_insertionSet.insert<Value>(m_index, Equal, m_value->origin(), m_value->child(0), m_value->child(0));
                    auto* result = m_insertionSet.insert<Value>(m_index, Select, m_value->origin(), compare, m_value->child(0), pureNaN);
                    m_value->replaceWithIdentity(result);
                }
                break;
            }

            case RotL: {
                // ARM64 doesn't have a rotate left.
                if (isARM64()) {
                    Value* newShift = m_insertionSet.insert<Value>(m_index, Neg, m_value->origin(), m_value->child(1));
                    Value* rotate = m_insertionSet.insert<Value>(m_index, RotR, m_value->origin(), m_value->child(0), newShift);
                    m_value->replaceWithIdentity(rotate);
                    break;
                }
                break;
            }

            case PopCount: {
                if (MacroAssembler::supportsCountPopulation(m_proc.policy().targetFeatures))
                    break;
                Type type = m_value->type();
                auto constant = [&](uint64_t bits) {
                    return m_insertionSet.insertIntConstant(m_index, m_origin, type, static_cast<int64_t>(type == Int32 ? static_cast<int32_t>(bits) : static_cast<int64_t>(bits)));
                };
                auto binary = [&](Opcode opcode, Value* left, Value* right) {
                    return m_insertionSet.insert<Value>(m_index, opcode, m_origin, left, right);
                };
                auto shiftedRight = [&](Value* value, int32_t amount) {
                    return binary(ZShr, value, m_insertionSet.insert<Const32Value>(m_index, m_origin, amount));
                };
                Value* bits = m_value->child(0);
                Value* pairs = binary(Sub, bits, binary(BitAnd, shiftedRight(bits, 1), constant(0x5555555555555555ull)));
                Value* nibbles = binary(Add, binary(BitAnd, pairs, constant(0x3333333333333333ull)), binary(BitAnd, shiftedRight(pairs, 2), constant(0x3333333333333333ull)));
                Value* bytes = binary(BitAnd, binary(Add, nibbles, shiftedRight(nibbles, 4)), constant(0x0f0f0f0f0f0f0f0full));
                m_value->replaceWithIdentity(shiftedRight(binary(Mul, bytes, constant(0x0101010101010101ull)), type == Int32 ? 24 : 56));
                break;
            }

            case MemoryMove: {
                RuntimeFunction function = m_value->as<BulkMemoryValue>()->isGCSafe() ? RuntimeFunction::GCSafeMemoryMove : RuntimeFunction::MemoryMove;
                Value* functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, function, m_origin));
                Value* result = m_insertionSet.insert<CCallValue>(
                    m_index,
                    m_value->type(),
                    m_origin,
                    m_value->effects(),
                    functionAddress,
                    m_value->child(0),
                    m_value->child(1),
                    m_value->child(2));
                m_value->replaceWithIdentity(result);
                break;
            }

            case MemoryFill: {
                Value* functionAddress = m_insertionSet.insertValue(m_index, addressOf(m_proc, RuntimeFunction::MemoryFill, m_origin));
                Value* result = m_insertionSet.insert<CCallValue>(
                    m_index,
                    m_value->type(),
                    m_origin,
                    m_value->effects(),
                    functionAddress,
                    m_value->child(0),
                    m_value->child(1),
                    m_value->child(2));
                m_value->replaceWithIdentity(result);
                break;
            }

            default:
                RELEASE_ASSERT(!isLoweredByLowerMacrosAfterOptimizations(m_value->opcode()));
                break;
            }
        }
        m_didInsertValues |= !m_insertionSet.isEmpty();
        m_insertionSet.execute(m_block);
    }
    
    Procedure& m_proc;
    BlockInsertionSet m_blockInsertionSet;
    InsertionSet m_insertionSet;
    BasicBlock* m_block;
    unsigned m_index;
    Value* m_value;
    Origin m_origin;
    bool m_changed { false };
    bool m_didInsertValues { false };
};

bool lowerMacrosImpl(Procedure& proc)
{
    LowerMacrosAfterOptimizations lowerMacros(proc);
    return lowerMacros.run();
}

} // anonymous namespace

bool lowerMacrosAfterOptimizations(Procedure& proc)
{
    PhaseScope phaseScope(proc, "lowerMacrosAfterOptimizations"_s);
    bool result = lowerMacrosImpl(proc);
    if (shouldValidateIR(proc))
        RELEASE_ASSERT(!lowerMacrosImpl(proc));
    return result;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

