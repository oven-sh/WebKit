/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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
#include "B3Validate.h"
#include "B3HeapRange.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "B3ArgumentRegValue.h"
#include "B3AtomicValue.h"
#include "B3CountedLoops.h"
#include "B3Dominators.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3Procedure.h"
#include "B3ProcedureInlines.h"
#include "B3SlotBaseValue.h"
#include "B3SwitchValue.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include "B3WasmBoundsCheckValue.h"
#include <algorithm>
#include <bit>
#include <span>
#include <utility>
#include <wtf/HashSet.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/CString.h>

namespace JSC { namespace B3 {

namespace {

enum class ViolationAction : uint8_t { Crash, Record };

class Validater {
public:
    // What follows from the control flow is computed into `form` where it is not there yet.
    Validater(Procedure& procedure, UTF8CStringView dumpBefore, ViolationAction violationAction, ValidatedForm& form)
        : m_procedure(procedure)
        , m_form(form)
        , m_dumpBefore(dumpBefore)
        , m_violationAction(violationAction)
    {
    }

    std::optional<ValidationRule> violatedRule() const { return m_violatedRule; }

#define VALIDATE_RULE(rule, condition, message) do {                    \
        if (m_violatedRule)                                             \
            return;                                                     \
        if (condition)                                                  \
            break;                                                      \
        fail(rule, __FILE__, __LINE__, WTF_PRETTY_FUNCTION, #condition, toUTF8CString message); \
        return;                                                         \
    } while (false)

#define VALIDATE(condition, message) VALIDATE_RULE(ValidationRule::Upstream, condition, message)

    void run()
    {
        // Values and blocks are numbered, so the tables are vectors, which stay cheap on large procedures.
        const SparseCollection<Value>& values = m_procedure.values();
        m_ownerOfValue.fill(nullptr, values.size());
        Vector<unsigned> indexInBlock;
        indexInBlock.fill(0, values.size());
        UncheckedKeyHashMap<Value*, Vector<std::optional<Type>>> extractions;
        m_blocksWithPhis.ensureSize(m_procedure.size());

        for (unsigned tuple = 0; tuple < m_procedure.tuples().size(); ++tuple) {
            VALIDATE(m_procedure.tuples()[tuple].size(), ("In tuple ", tuple));
            for (unsigned i = 0; i < m_procedure.tuples()[tuple].size(); ++i)
                VALIDATE(m_procedure.tuples()[tuple][i].isNumeric(), ("In tuple ", tuple, " at index", i));
        }

        for (BasicBlock* block : m_procedure) {
            for (unsigned i = 0; i < block->size(); ++i) {
                Value* value = block->at(i);
                VALIDATE(value, ("At ", *block, ", index ", i));
                VALIDATE(isInProcedure(value), ("At ", *block, ", index ", i));
                VALIDATE(!m_ownerOfValue[value->index()], ("At ", *value));
                m_ownerOfValue[value->index()] = block;
                indexInBlock[value->index()] = i;
                if (value->opcode() == Phi)
                    m_blocksWithPhis.quickSet(block->index());
                VALIDATE(value->effects().terminal == (i + 1 == block->size()), ("At ", *value));
            }
        }

        for (unsigned index = 0; index < values.size(); ++index)
            VALIDATE(!values[index] || m_ownerOfValue[index], ("At ", *values[index]));

        // Compute dominators ourselves to avoid perturbing Procedure.
        if (!m_form.dominators)
            m_form.dominators = makeUnique<Dominators>(m_procedure);
        Dominators& dominators = *m_form.dominators;

        for (Value* value : values) {
            for (Value* child : value->children()) {
                VALIDATE(child, ("At ", *value));
                VALIDATE(isInProcedure(child), ("At ", *value, "->", pointerDump(child)));
                if (ownerOf(child) == ownerOf(value))
                    VALIDATE(indexInBlock[value->index()] > indexInBlock[child->index()], ("At ", *value, "->", pointerDump(child)));
                else
                    VALIDATE(dominators.dominates(ownerOf(child), ownerOf(value)), ("at ", *value, "->", pointerDump(child)));
                if (child->effects().terminal)
                    VALIDATE_RULE(ValidationRule::TerminalResult, firstSuccessorEdgeDominates(dominators, ownerOf(child), ownerOf(value)), ("At ", *value, "->", pointerDump(child)));
            }
        }

        // The blocks that jump to each block, each of them once.
        Vector<unsigned> firstJumperTo;
        firstJumperTo.fill(0, m_procedure.size() + 1);
        Vector<BasicBlock*> latestJumperTo;
        latestJumperTo.fill(nullptr, m_procedure.size());
        for (BasicBlock* block : m_procedure) {
            VALIDATE(block->size() >= 1, ("At ", *block));

            for (BasicBlock* successor : block->successorBlocks()) {
                VALIDATE(isInProcedure(successor), ("At ", *block, "->", pointerDump(successor)));
                if (std::exchange(latestJumperTo[successor->index()], block) != block)
                    ++firstJumperTo[successor->index() + 1];
            }
        }
        for (unsigned index = 0; index < m_procedure.size(); ++index)
            firstJumperTo[index + 1] += firstJumperTo[index];
        Vector<BasicBlock*> jumpers;
        jumpers.fill(nullptr, firstJumperTo.last());
        Vector<unsigned> nextJumperTo(firstJumperTo.span().first(m_procedure.size()));
        latestJumperTo.fill(nullptr);
        for (BasicBlock* block : m_procedure) {
            for (BasicBlock* successor : block->successorBlocks()) {
                if (std::exchange(latestJumperTo[successor->index()], block) != block)
                    jumpers[nextJumperTo[successor->index()]++] = block;
            }
        }

        // The pointers are compared and not followed: the list of a dead block may name blocks that are gone.
        Vector<BasicBlock*, 8> predecessors;
        for (BasicBlock* block : m_procedure) {
            if (block->numPredecessors() == 1 && firstJumperTo[block->index() + 1] - firstJumperTo[block->index()] == 1) {
                VALIDATE(block->predecessor(0) == jumpers[firstJumperTo[block->index()]], ("At ", *block));
                continue;
            }
            predecessors.shrink(0);
            predecessors.appendRange(block->predecessors().begin(), block->predecessors().end());
            std::ranges::sort(predecessors);
            // We expect the predecessor list to be de-duplicated.
            VALIDATE(std::ranges::adjacent_find(predecessors) == predecessors.end(), ("At ", *block));

            // Note that this totally allows dead code.
            std::span<BasicBlock*> jumpersToBlock = jumpers.mutableSpan().subspan(firstJumperTo[block->index()], firstJumperTo[block->index() + 1] - firstJumperTo[block->index()]);
            if (jumpersToBlock.empty())
                continue;
            std::ranges::sort(jumpersToBlock);
            VALIDATE(std::ranges::equal(predecessors, jumpersToBlock), ("At ", *block));
        }

        for (Value* value : m_procedure.values()) {
            VALIDATE_RULE(ValidationRule::OpcodeEverAdded, m_procedure.hasEverAdded(value->opcode()), ("At ", *value));
            for (Value* child : value->children())
                VALIDATE(child->type() != Void, ("At ", *value, "->", *child));
            switch (value->opcode()) {
            case Nop:
            case Fence:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                break;
            case Identity:
            case Opaque:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type() != Void, ("At ", *value));
                break;
            case Const32:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                break;
            case Const64:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Int64, ("At ", *value));
                break;
            case ConstDouble:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Double, ("At ", *value));
                break;
            case ConstFloat:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Float, ("At ", *value));
                break;
            case Const128:
                RELEASE_ASSERT(m_procedure.policy().upstream.allowsSIMD);
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                break;
            case BottomTuple:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type().isTuple(), ("At ", *value));
                break;
            case Set:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == value->as<VariableValue>()->variable()->type(), ("At ", *value));
                break;
            case Get:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == value->as<VariableValue>()->variable()->type(), ("At ", *value));
                break;
            case SlotBase:
            case FramePointer:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == pointerType(), ("At ", *value));
                break;
            case Relocatable:
                VALIDATE_RULE(ValidationRule::Relocatable, !value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE_RULE(ValidationRule::Relocatable, !value->numChildren(), ("At ", *value));
                VALIDATE_RULE(ValidationRule::Relocatable, value->type() == pointerType(), ("At ", *value));
                break;
            case ArgumentReg:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(
                    (value->as<ArgumentRegValue>()->argumentReg().isGPR() ? (value->type() == pointerType())
                        : (value->type() == V128 || value->type() == Double))
                    , ("At ", *value));
                break;
            case Add:
            case Sub:
            case Mul:
            case MulHigh:
            case UMulHigh:
            case Div:
            case UDiv:
            case Mod:
            case UMod:
            case BitAnd:
            case BitOr:
            case BitXor:
                VALIDATE(!value->kind().traps(), ("At ", *value));
                switch (value->opcode()) {
                case Div:
                case Mod:
                    if (value->isChill()) {
                        VALIDATE(value->opcode() == Div || value->opcode() == Mod, ("At ", *value));
                        VALIDATE(value->type().isInt(), ("At ", *value));
                    }
                    break;
                default:
                    VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                    break;
                }
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->type().isNumeric(), ("At ", *value));
                break;
            case FMax:
            case FMin:
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->type() == Float || value->type() == Double, ("At ", *value));
                break;
            case Neg:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type().isNumeric(), ("At ", *value));
                break;
            case PurifyNaN:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type().isFloat(), ("At ", *value));
                break;
            case Shl:
            case SShr:
            case ZShr:
            case RotR:
            case RotL:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->child(1)->type() == Int32, ("At ", *value));
                VALIDATE(value->type().isInt(), ("At ", *value));
                break;
            case BitwiseCast:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() != value->child(0)->type(), ("At ", *value));
                VALIDATE(
                    (value->type() == Int64 && value->child(0)->type() == Double)
                    || (value->type() == Double && value->child(0)->type() == Int64)
                    || (value->type() == Float && value->child(0)->type() == Int32)
                    || (value->type() == Int32 && value->child(0)->type() == Float),
                    ("At ", *value));
                break;
            case SExt8:
            case SExt16:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int32, ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                break;
            case SExt8To64:
            case SExt16To64:
            case SExt32:
            case ZExt32:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int32, ("At ", *value));
                VALIDATE(value->type() == Int64, ("At ", *value));
                break;
            case Clz:
            case PopCount:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type().isInt(), ("At ", *value));
                break;
            case Trunc:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(
                    (value->type() == Int32 && value->child(0)->type() == Int64)
                    || (value->type() == Float && value->child(0)->type() == Double),
                    ("At ", *value));
                break;
            case Abs:
            case Ceil:
            case Floor:
            case FTrunc:
            case Sqrt:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isFloat(), ("At ", *value));
                VALIDATE(value->type().isFloat(), ("At ", *value));
                break;
            case IToD:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type() == Double, ("At ", *value));
                break;
            case IToF:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type() == Float, ("At ", *value));
                break;
            case FloatToDouble:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Float, ("At ", *value));
                VALIDATE(value->type() == Double, ("At ", *value));
                break;
            case DoubleToFloat:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Double, ("At ", *value));
                VALIDATE(value->type() == Float, ("At ", *value));
                break;
            case Equal:
            case NotEqual:
            case LessThan:
            case GreaterThan:
            case LessEqual:
            case GreaterEqual:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                break;
            case Above:
            case Below:
            case AboveEqual:
            case BelowEqual:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                break;
            case EqualOrUnordered:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->child(0)->type().isFloat(), ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                break;
            case Select:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->type() == value->child(2)->type(), ("At ", *value));
                break;
            case Load8Z:
            case Load8S:
            case Load16Z:
            case Load16S:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                validateFence(value);
                validateStackAccess(value);
                break;
            case Load:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type().isNumeric(), ("At ", *value));
                validateFence(value);
                validateStackAccess(value);
                break;
            case Store8:
            case Store16:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int32, ("At ", *value));
                VALIDATE(value->child(1)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                validateFence(value);
                validateStackAccess(value);
                break;
            case Store:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(1)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                validateFence(value);
                validateStackAccess(value);
                break;
            case MemoryMove:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->child(0)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->child(1)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->child(2)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                break;
            case MemoryFill:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->child(0)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->child(1)->type() == Int32, ("At ", *value));
                VALIDATE(value->child(2)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                break;
            case AtomicWeakCAS:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == Int32, ("At ", *value));
                VALIDATE(value->child(0)->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->child(2)->type() == pointerType(), ("At ", *value));
                validateAtomic(value);
                validateStackAccess(value);
                break;
            case AtomicStrongCAS:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type() == value->child(1)->type(), ("At ", *value));
                VALIDATE(value->type().isInt(), ("At ", *value));
                VALIDATE(value->child(2)->type() == pointerType(), ("At ", *value));
                validateAtomic(value);
                validateStackAccess(value);
                break;
            case AtomicXchgAdd:
            case AtomicXchgAnd:
            case AtomicXchgOr:
            case AtomicXchgSub:
            case AtomicXchgXor:
            case AtomicXchg:
                VALIDATE(!value->kind().isChill(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type().isInt(), ("At ", *value));
                VALIDATE(value->child(1)->type() == pointerType(), ("At ", *value));
                validateAtomic(value);
                validateStackAccess(value);
                break;
            case Depend:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == value->child(0)->type(), ("At ", *value));
                VALIDATE(value->type().isInt(), ("At ", *value));
                break;
            case WasmAddress:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == pointerType(), ("At ", *value));
                VALIDATE(value->type() == pointerType(), ("At ", *value));
                break;
            case VectorExtractLane:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == simdB3ScalarType(value->asSIMDValue()->simdLane()), ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                break;
            case VectorReplaceLane:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == simdB3ScalarType(value->asSIMDValue()->simdLane()), ("At ", *value));
                break;
            case VectorDupElement:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->immediate() < (16 / elementByteSize(value->asSIMDValue()->simdLane())), ("At ", *value));
                break;
            case VectorNot:
            case VectorAbs:
            case VectorNeg:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                break;

            case VectorSplat:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == simdB3ScalarType(value->asSIMDValue()->simdLane()), ("At ", *value));
                break;

            case VectorPopcnt:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i8x16, ("At ", *value));
                break;

            case VectorMulSat:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i16x8, ("At ", *value));
                break;

            case VectorSwizzle:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2 || value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->numChildren() == 2 || value->child(2)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i8x16, ("At ", *value));
                break;

            case VectorUnzipEven:
            case VectorUnzipOdd:
            case VectorZipLower:
            case VectorZipHigher:
            case VectorTransposeEven:
            case VectorTransposeOdd:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                break;

            case VectorReverse: {
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                unsigned groupSize = value->as<SIMDValue>()->immediate();
                VALIDATE(groupSize == 2 || groupSize == 4 || groupSize == 8, ("At ", *value));
                break;
            }

            case VectorExtractPair:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                break;

            case VectorMulByElement:
                VALIDATE(isARM64(), ("At ", *value));
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::f64x2 || value->asSIMDValue()->simdLane() == SIMDLane::f32x4, ("At ", *value));
                VALIDATE(value->asSIMDValue()->immediate() < (16 / elementByteSize(value->asSIMDValue()->simdLane())), ("At ", *value));
                break;

            case VectorBitmask:
            case VectorAllTrue:
            case VectorAnyTrue:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type().isInt(), ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                if (value->opcode() == VectorAnyTrue)
                    VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::v128, ("At ", *value));
                else
                    VALIDATE(scalarTypeIsIntegral(value->asSIMDValue()->simdLane()), ("At ", *value));
                break;

            case VectorCeil:
            case VectorFloor:
            case VectorTrunc:
            case VectorTruncSat:
            case VectorRelaxedTruncSat:
            case VectorNearest:
            case VectorSqrt:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(scalarTypeIsFloatingPoint(value->asSIMDValue()->simdLane()), ("At ", *value));
                break;
            case VectorConvert:
            case VectorConvertLow:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(scalarTypeIsIntegral(value->asSIMDValue()->simdLane()), ("At ", *value));
                VALIDATE(elementByteSize(value->asSIMDValue()->simdLane()) == 4, ("At ", *value));
                break;

            case VectorExtendLow:
            case VectorExtendHigh:
            case VectorExtaddPairwise:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128 || value->child(0)->type() == Double, ("At ", *value));
                VALIDATE(scalarTypeIsIntegral(value->asSIMDValue()->simdLane()), ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() != SIMDSignMode::None, ("At ", *value));
                break;

            case VectorPromote:
            case VectorDemote:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(scalarTypeIsFloatingPoint(value->asSIMDValue()->simdLane()), ("At ", *value));
                if (value->opcode() == VectorPromote)
                    VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::f32x4, ("At ", *value));
                else
                    VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::f64x2, ("At ", *value));
                break;

            case VectorNotEqual:
            case VectorLessThan:
            case VectorLessThanOrEqual:
            case VectorBelow:
            case VectorBelowOrEqual:
            case VectorGreaterThan:
            case VectorGreaterThanOrEqual:
            case VectorAbove:
            case VectorAboveOrEqual:
            case VectorEqual:
            case VectorAdd:
            case VectorSub:
            case VectorAddSat:
            case VectorSubSat:
            case VectorMul:
            case VectorAnd:
            case VectorAndnot:
            case VectorOr:
            case VectorXor:
            case VectorAvgRound:
            case VectorDotProduct:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                if (value->opcode() == VectorAvgRound)
                    VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i8x16 || value->asSIMDValue()->simdLane() == SIMDLane::i16x8,  ("At ", *value));
                else if (value->opcode() == VectorDotProduct)
                    VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i32x4,  ("At ", *value));
                break;
            case VectorMulHigh:
            case VectorMulLow:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(scalarTypeIsIntegral(value->asSIMDValue()->simdLane()), ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() != SIMDLane::i8x16, ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() != SIMDSignMode::None, ("At ", *value));
                break;
            case VectorShl:
            case VectorShr:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == Int32, ("At ", *value));
                if (value->opcode() == VectorShr)
                    VALIDATE(value->asSIMDValue()->signMode() != SIMDSignMode::None, ("At ", *value));
                break;
            case VectorMin:
            case VectorMax:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                break;
            case VectorPmin:
            case VectorPmax:
            case VectorDiv:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(scalarTypeIsFloatingPoint(value->asSIMDValue()->simdLane()), ("At ", *value));
                break;
            case VectorNarrow:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(scalarTypeIsIntegral(value->asSIMDValue()->simdLane()), ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() != SIMDSignMode::None, ("At ", *value));
                break;
            case VectorBitwiseSelect:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->child(2)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::v128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::None, ("At ", *value));
                break;

            // Relaxed SIMD
            case VectorRelaxedSwizzle:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i8x16, ("At ", *value));
                break;

            case VectorRelaxedMAdd:
            case VectorRelaxedNMAdd:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->child(2)->type() == V128, ("At ", *value));
                VALIDATE((value->asSIMDValue()->simdLane() == SIMDLane::f32x4) || (value->asSIMDValue()->simdLane() == SIMDLane::f64x2), ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::None, ("At ", *value));
                break;

            case VectorRelaxedLaneSelect:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->child(2)->type() == V128, ("At ", *value));
                VALIDATE((value->asSIMDValue()->simdLane() == SIMDLane::i8x16)
                    || (value->asSIMDValue()->simdLane() == SIMDLane::i16x8)
                    || (value->asSIMDValue()->simdLane() == SIMDLane::i32x4)
                    || (value->asSIMDValue()->simdLane() == SIMDLane::i64x2), ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::None, ("At ", *value));
                break;

            case VectorRelaxedMin:
            case VectorRelaxedMax:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE((value->asSIMDValue()->simdLane() == SIMDLane::f32x4) || (value->asSIMDValue()->simdLane() == SIMDLane::f64x2), ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::None, ("At ", *value));
                break;

            case VectorRelaxedQ15Mulr:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i16x8, ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::Signed, ("At ", *value));
                break;

            case VectorRelaxedDotI8x16I7x16:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i16x8, ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::Signed, ("At ", *value));
                break;

            case VectorRelaxedDotI8x16I7x16Add:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->type() == V128, ("At ", *value));
                VALIDATE(value->child(0)->type() == V128, ("At ", *value));
                VALIDATE(value->child(1)->type() == V128, ("At ", *value));
                VALIDATE(value->child(2)->type() == V128, ("At ", *value));
                VALIDATE(value->asSIMDValue()->simdLane() == SIMDLane::i32x4, ("At ", *value));
                VALIDATE(value->asSIMDValue()->signMode() == SIMDSignMode::Signed, ("At ", *value));
                break;

            case CCall:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() >= 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == pointerType(), ("At ", *value));
                if (value->type().isTuple()) {
                    // FIXME: Right now we only support a pair of two GPR values since on every calling
                    // convention we support that's returned in returnValueGPR/returnValueGPR2, respectively.
                    VALIDATE(m_procedure.resultCount(value->type()) == 2, ("At ", *value));
                    VALIDATE(m_procedure.typeAtOffset(value->type(), 0).isInt(), ("At ", *value));
                    VALIDATE(m_procedure.typeAtOffset(value->type(), 1) == registerType(), ("At ", *value));
                }

                break;
            case Patchpoint:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                if (value->type() == Void) {
                    VALIDATE(value->as<PatchpointValue>()->resultConstraints.size() == 1, ("At ", *value));
                    VALIDATE(value->as<PatchpointValue>()->resultConstraints[0] == ValueRep::WarmAny, ("At ", *value));
                } else {
                    if (value->type().isNumeric()) {
                        VALIDATE(value->as<PatchpointValue>()->resultConstraints.size() == 1, ("At ", *value));
                        validateStackmapConstraint(value, ConstrainedValue(value, value->as<PatchpointValue>()->resultConstraints[0]), ConstraintRole::Def);
                    } else {
                        VALIDATE(m_procedure.isValidTuple(value->type()), ("At ", *value));
                        VALIDATE(value->as<PatchpointValue>()->resultConstraints.size() == m_procedure.tupleForType(value->type()).size(), ("At ", *value));
                        for (unsigned i = 0; i < value->as<PatchpointValue>()->resultConstraints.size(); ++i)
                            validateStackmapConstraint(value, ConstrainedValue(value, value->as<PatchpointValue>()->resultConstraints[i]), ConstraintRole::Def, i);
                    }
                }
                validateStackmap(value);
                validatePatchpointContract(value->as<PatchpointValue>(), ownerOf(value));
                break;
            case Extract: {
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isTuple(), ("At ", *value));
                VALIDATE(value->type().isNumeric(), ("At ", *value));
                break;
            }
            case CheckAdd:
            case CheckSub:
            case CheckMul:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() >= 2, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->child(1)->type().isInt(), ("At ", *value));
                VALIDATE(value->as<StackmapValue>()->constrainedChild(0).rep() == ValueRep::WarmAny, ("At ", *value));
                VALIDATE(value->as<StackmapValue>()->constrainedChild(1).rep() == ValueRep::WarmAny, ("At ", *value));
                validateStackmap(value);
                break;
            case Check:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() >= 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->as<StackmapValue>()->constrainedChild(0).rep() == ValueRep::WarmAny, ("At ", *value));
                validateStackmap(value);
                break;
            case WasmBoundsCheck:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int32 || value->child(0)->type() == Int64, ("At ", *value));
                switch (value->as<WasmBoundsCheckValue>()->boundsType()) {
                case WasmBoundsCheckValue::Type::Pinned:
                    VALIDATE(m_procedure.code().isPinned(value->as<WasmBoundsCheckValue>()->bounds().pinnedSize), ("At ", *value));
                    break;
                case WasmBoundsCheckValue::Type::Maximum:
                    break;
                }
                VALIDATE(m_procedure.code().wasmBoundsCheckGenerator(), ("At ", *value));
                break;
            case WasmStructGet:
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // struct pointer
                break;
            case WasmStructSet:
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // struct pointer
                VALIDATE(value->type() == Void, ("At ", *value));
                break;
            case WasmStructNew:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->type() == Int64, ("At ", *value)); // returns struct pointer
                break;
            case WasmArrayGet:
                VALIDATE(value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // array pointer
                VALIDATE(value->child(1)->type() == Int32, ("At ", *value)); // index
                break;
            case WasmArraySet:
                VALIDATE(value->numChildren() == 3, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // array pointer
                VALIDATE(value->child(1)->type() == Int32, ("At ", *value)); // index
                VALIDATE(value->type() == Void, ("At ", *value));
                break;
            case WasmArrayNew:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 3 || value->numChildren() == 4, ("At ", *value));
                VALIDATE(value->type() == Int64, ("At ", *value)); // returns array pointer
                break;
            case WasmArrayLength:
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // array pointer
                VALIDATE(value->type() == Int32, ("At ", *value)); // returns size
                break;
            case WasmRefCast:
                VALIDATE(value->numChildren() == 1 || value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // reference input
                VALIDATE(value->type() == Int64, ("At ", *value)); // returns reference
                break;
            case WasmRefTest:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1 || value->numChildren() == 2, ("At ", *value));
                VALIDATE(value->child(0)->type() == Int64, ("At ", *value)); // reference input
                VALIDATE(value->type() == Int32, ("At ", *value)); // returns boolean
                break;
            case Upsilon:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->as<UpsilonValue>()->phi(), ("At ", *value));
                VALIDATE(value->as<UpsilonValue>()->phi()->opcode() == Phi, ("At ", *value));
                VALIDATE(value->child(0)->type() != Void, ("At ", *value));
                VALIDATE(value->child(0)->type() == value->as<UpsilonValue>()->phi()->type(), ("At ", *value));
                VALIDATE(isInProcedure(value->as<UpsilonValue>()->phi()), ("At ", *value));
                break;
            case Phi:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() != Void, ("At ", *value));
                break;
            case Jump:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                VALIDATE(ownerOf(value)->numSuccessors() == 1, ("At ", *value));
                break;
            case Oops:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                VALIDATE(!ownerOf(value)->numSuccessors(), ("At ", *value));
                break;
            case Return:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() <= 1, ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                VALIDATE(!ownerOf(value)->numSuccessors(), ("At ", *value));
                break;
            case Branch:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                VALIDATE(ownerOf(value)->numSuccessors() == 2, ("At ", *value));
                break;
            case Switch: {
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(value->numChildren() == 1, ("At ", *value));
                VALIDATE(value->child(0)->type().isInt(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                VALIDATE(value->as<SwitchValue>()->hasFallThrough(ownerOf(value)), ("At ", *value));
                // This validates the same thing as hasFallThrough, but more explicitly. We want to
                // make sure that if anyone tries to change the definition of hasFallThrough, they
                // will feel some pain here, since this is fundamental.
                VALIDATE(ownerOf(value)->numSuccessors() == value->as<SwitchValue>()->numCaseValues() + 1, ("At ", *value));
                
                // Check that there are no duplicate cases.
                Vector<int64_t> caseValues = value->as<SwitchValue>()->caseValues();
                std::ranges::sort(caseValues);
                for (unsigned i = 1; i < caseValues.size(); ++i)
                    VALIDATE(caseValues[i - 1] != caseValues[i], ("At ", *value, ", caseValue = ", caseValues[i]));
                break;
            }
            case EntrySwitch:
                VALIDATE(!value->kind().hasExtraBits(), ("At ", *value));
                VALIDATE(!value->numChildren(), ("At ", *value));
                VALIDATE(value->type() == Void, ("At ", *value));
                VALIDATE(ownerOf(value)->numSuccessors() == m_procedure.numEntrypoints(), ("At ", *value));
                break;
            }

            VALIDATE(!(value->effects().writes && value->key()), ("At ", *value));
            if (std::optional<IntRange> range = m_procedure.knownRange(value)) {
                VALIDATE_RULE(ValidationRule::KnownRange, mayHaveKnownRange(value->opcode()), ("At ", *value, ": ", *range));
                VALIDATE_RULE(ValidationRule::KnownRange, value->type().isInt(), ("At ", *value, ": ", *range));
                VALIDATE_RULE(ValidationRule::KnownRange, range->min() <= range->max() && range->isSubsetOf(IntRange::top(value->type())), ("At ", *value, ": ", *range));
                VALIDATE_RULE(ValidationRule::KnownRange, isKnownRangePlausible(m_procedure, value), ("At ", *value, ": ", *range));
            }
            if (m_procedure.knownDereferenceableBytes(value))
                VALIDATE_RULE(ValidationRule::KnownDereferenceableBytes, value->type() == pointerType(), ("At ", *value));
            // Conceptually, if you exit sideways, you will almost certainly read Top or (AbstractHeapRepository) root. 
            // Since we can't easily find out what root is here, we settle for checking that you at least read *something*.
            // This is technically overly strict, but in practice violating this is almost certainly a bug.
            VALIDATE((!value->effects().exitsSideways || value->effects().reads != HeapRange()), ("At ", *value));
        }

        for (Variable* variable : m_procedure.variables())
            VALIDATE(variable->type() != Void, ("At ", *variable));

        validatePhisAreDominatedByUpsilons();
        validateKnownInductions(dominators);
    }

private:
    bool isInProcedure(Value* value) const { return value->index() < m_procedure.values().size() && m_procedure.values()[value->index()] == value; }
    bool isInProcedure(BasicBlock* block) const { return block->index() < m_procedure.size() && m_procedure[block->index()] == block; }
    BasicBlock* ownerOf(Value* value) const { return m_ownerOfValue[value->index()]; }

    void validateStackmap(Value* value)
    {
        StackmapValue* stackmap = value->as<StackmapValue>();
        VALIDATE(stackmap, ("At ", *value));
        VALIDATE(stackmap->numChildren() >= stackmap->reps().size(), ("At ", *stackmap));
        for (ConstrainedValue child : stackmap->constrainedChildren())
            validateStackmapConstraint(stackmap, child);
    }
    
    enum class ConstraintRole {
        Use,
        Def
    };
    void validateStackmapConstraint(Value* context, const ConstrainedValue& value, ConstraintRole role = ConstraintRole::Use, unsigned tupleIndex = 0)
    {
        switch (value.rep().kind()) {
        case ValueRep::WarmAny:
        case ValueRep::SomeRegister:
        case ValueRep::StackArgument:
            break;
        case ValueRep::LateColdAny:
        case ValueRep::ColdAny:
            VALIDATE(role == ConstraintRole::Use, ("At ", *context, ": ", value));
            break;
        case ValueRep::SomeRegisterWithClobber:
            VALIDATE(role == ConstraintRole::Use, ("At ", *context, ": ", value));
            VALIDATE(context->as<PatchpointValue>(), ("At ", *context));
            break;
        case ValueRep::SomeEarlyRegister:
            VALIDATE(role == ConstraintRole::Def, ("At ", *context, ": ", value));
            break;
        case ValueRep::Register:
        case ValueRep::LateRegister:
        case ValueRep::SomeLateRegister:
            if (value.rep().kind() == ValueRep::LateRegister)
                VALIDATE(role == ConstraintRole::Use, ("At ", *context, ": ", value));
            if (value.rep().reg().isGPR()) {
                if (value.value()->type().isTuple())
                    VALIDATE(m_procedure.extractFromTuple(value.value()->type(), tupleIndex).isInt(), ("At ", *context, ": ", value));
                else
                    VALIDATE(value.value()->type().isInt(), ("At ", *context, ": ", value));
            } else {
                if (value.value()->type().isTuple())
                    VALIDATE(m_procedure.extractFromTuple(value.value()->type(), tupleIndex).isFloat() 
                        || m_procedure.extractFromTuple(value.value()->type(), tupleIndex).isVector(), ("At ", *context, ": ", value));
                else
                    VALIDATE(value.value()->type().isFloat() || value.value()->type().isVector(), ("At ", *context, ": ", value));
            }
            break;
        case ValueRep::Constant:
        case ValueRep::Stack:
            VALIDATE(false, ("At ", *context, ": ", value));
            break;
        }
    }

    void validateFence(Value* value)
    {
        MemoryValue* memory = value->as<MemoryValue>();
        if (memory->hasFence())
            VALIDATE(memory->accessBank() == GP, ("Fence at ", *memory));
    }

    void validateAtomic(Value* value)
    {
        AtomicValue* atomic = value->as<AtomicValue>();

        VALIDATE(bestType(GP, atomic->accessWidth()) == atomic->accessType(), ("At ", *value));
    }

    void validateStackAccess(Value* value)
    {
        MemoryValue* memory = value->as<MemoryValue>();
        SlotBaseValue* slotBase = value->lastChild()->as<SlotBaseValue>();
        if (!slotBase)
            return;

        VALIDATE(memory->offset() >= 0, ("At ", *value));
    }

    static bool firstSuccessorEdgeDominates(const Dominators& dominators, BasicBlock* block, BasicBlock* to)
    {
        if (!block->numSuccessors())
            return false;
        BasicBlock* successor = block->successorBlock(0);
        for (unsigned i = 1; i < block->numSuccessors(); ++i) {
            if (block->successorBlock(i) == successor)
                return false;
        }
        if (dominators.dominates(successor, block))
            return false;
        for (BasicBlock* predecessor : successor->predecessors()) {
            if (predecessor != block && !dominators.dominates(successor, predecessor))
                return false;
        }
        return dominators.dominates(successor, to);
    }

    void validatePatchpointContract(PatchpointValue* patchpoint, BasicBlock* owner)
    {
        const Effects& effects = patchpoint->effects;

        if (patchpoint->isLoopPoll) {
            VALIDATE_RULE(ValidationRule::LoopPoll, patchpoint->type() == Void, ("At ", *patchpoint));
            VALIDATE_RULE(ValidationRule::LoopPoll, !patchpoint->numChildren(), ("At ", *patchpoint));
            VALIDATE_RULE(ValidationRule::LoopPoll, !effects.terminal, ("At ", *patchpoint));
            VALIDATE_RULE(ValidationRule::LoopPoll, !patchpoint->pureFunction, ("At ", *patchpoint));
            VALIDATE_RULE(ValidationRule::LoopPoll, effects.reads == HeapRange::top(), ("At ", *patchpoint));
        }

        if (patchpoint->numberOfSuccessorClobbers()) {
            VALIDATE_RULE(ValidationRule::SuccessorClobbers, effects.terminal, ("At ", *patchpoint));
            VALIDATE_RULE(ValidationRule::SuccessorClobbers, patchpoint->numberOfSuccessorClobbers() <= owner->numSuccessors(), ("At ", *patchpoint));
            VALIDATE_RULE(ValidationRule::SuccessorClobbers, patchpoint->type() == Void || patchpoint->lateClobberedOnSuccessor(0).isEmpty(), ("At ", *patchpoint));
        }

        if (!patchpoint->pureFunction)
            return;

        VALIDATE_RULE(ValidationRule::PureFunction, !effects.reads, ("At ", *patchpoint));
        VALIDATE_RULE(ValidationRule::PureFunction, !effects.exitsSideways, ("At ", *patchpoint));
        VALIDATE_RULE(ValidationRule::PureFunction, !effects.fence, ("At ", *patchpoint));
        VALIDATE_RULE(ValidationRule::PureFunction, !effects.terminal, ("At ", *patchpoint));
        VALIDATE_RULE(ValidationRule::PureFunction, !effects.readsPinned && !effects.writesPinned, ("At ", *patchpoint));
        VALIDATE_RULE(ValidationRule::PureFunction, !effects.readsLocalState && !effects.writesLocalState, ("At ", *patchpoint));
    }

    void validateKnownInductions(Dominators& dominators)
    {
        if (m_violatedRule || m_procedure.knownInductions().isEmpty())
            return;
        m_procedure.knownInductions().forEach([&](const InductionRecord& record) {
            validateKnownInduction(record);
        });
        if (m_violatedRule)
            return;
        NaturalLoops loops(m_procedure, dominators);
        for (Value* value : m_procedure.values()) {
            if (auto* upsilon = value->as<UpsilonValue>())
                validateKnownInductionStep(upsilon, loops);
        }
    }

    void validateKnownInduction(const InductionRecord& record)
    {
        VALIDATE_RULE(ValidationRule::KnownInduction, isInProcedure(record.phi) && isInProcedure(record.next), ("At ", *record.phi));
        VALIDATE_RULE(ValidationRule::KnownInduction, !record.test || isInProcedure(record.test), ("At ", *record.phi));
        VALIDATE_RULE(ValidationRule::KnownInduction, record.phi->type().isInt() && record.step, ("At ", *record.phi));
    }

    void validateKnownInductionStep(UpsilonValue* upsilon, const NaturalLoops& loops)
    {
        std::optional<InductionRecord> record = m_procedure.knownInduction(upsilon->phi());
        if (!record)
            return;
        const NaturalLoop* loop = loops.headerOf(ownerOf(record->phi));
        if (!loop || !loops.belongsTo(ownerOf(upsilon), *loop))
            return;
        auto form = affineFormOf(m_procedure, upsilon->child(0), record->phi, Signedness::Signed);
        VALIDATE_RULE(ValidationRule::KnownInduction, !form || form->offset == record->step, ("At ", *upsilon, " of ", *record->phi));
    }

    // A simple backwards analysis to check that we cannot reach a Phi without going through a corresponding Upsilon
    // We cannot use the dominator tree, since we are checking that each Phi is dominated by a the set of all of its upsilons, and not by a single node.
    void validatePhisAreDominatedByUpsilons()
    {
        bool changed = true;
        BitVector blocksToVisit;
        IndexMap<BasicBlock*, UncheckedKeyHashSet<Value*>> undominatedPhisAtTail(m_procedure.size());
        // A BitVector grows by no more than it is asked for.
        blocksToVisit.ensureSize(m_procedure.size());
        for (BasicBlock* block : m_procedure)
            blocksToVisit.quickSet(block->index());
        if (m_form.blocksInPostOrder.isEmpty())
            m_form.blocksInPostOrder = m_procedure.blocksInPostOrder();
        while (changed) {
            changed = false;
            for (BasicBlock* block : m_form.blocksInPostOrder) {
                if (!blocksToVisit.quickClear(block->index()))
                    continue;
                if (undominatedPhisAtTail[block].isEmpty() && !m_blocksWithPhis.quickGet(block->index()))
                    continue;
                UncheckedKeyHashSet<Value*> undominatedPhis = undominatedPhisAtTail[block];
                for (unsigned index = block->size()-1; index--;) {
                    Value* value = block->at(index);
                    switch (value->opcode()) {
                    case Upsilon:
                        undominatedPhis.remove(value->as<UpsilonValue>()->phi());
                        break;
                    case Phi:
                        undominatedPhis.add(value);
                        break;
                    default:
                        break;
                    }
                }
                for (BasicBlock* predecessor : block->predecessors()) {
                    bool changedSet = false;
                    for (Value* phi : undominatedPhis)
                        changedSet |= undominatedPhisAtTail[predecessor].add(phi).isNewEntry;
                    if (changedSet) {
                        blocksToVisit.quickSet(predecessor->index());
                        changed = true;
                    }
                }
                if (!block->index())
                    VALIDATE(undominatedPhis.isEmpty(), ("Undominated phi at top of entry block: ", **undominatedPhis.begin()));
            }
        }
    }

    void fail(
        ValidationRule rule, const char* filename, int lineNumber, const char* function, const char* condition,
        UTF8CString message)
    {
        if (m_violationAction == ViolationAction::Record) {
            m_violatedRule = rule;
            return;
        }

        UTF8CString failureMessage;
        {
            StringPrintStream out;
            out.print("B3 VALIDATION FAILURE\n");
            out.print("    ", condition, " (", filename, ":", lineNumber, ")\n");
            out.print("    ", message, "\n");
            out.print("    After ", m_procedure.lastPhaseName(), "\n");
            failureMessage = out.toUTF8CString();
        }

        dataLog(failureMessage);
        if (m_dumpBefore) {
            dataLog("Before ", m_procedure.lastPhaseName(), ":\n");
            dataLog(m_dumpBefore);
        }
        dataLog("At time of failure:\n");
        dataLog(m_procedure);

        dataLog(failureMessage);
        WTFReportAssertionFailure(filename, lineNumber, function, condition);
        CRASH();
    }
    
    Procedure& m_procedure;
    ValidatedForm& m_form;
    Vector<BasicBlock*> m_ownerOfValue;
    BitVector m_blocksWithPhis;
    UTF8CStringView m_dumpBefore;
    ViolationAction m_violationAction;
    std::optional<ValidationRule> m_violatedRule;
};

class Hasher {
public:
    void add(uint64_t word)
    {
        m_hash = (m_hash ^ word) * 0x9e3779b97f4a7c15ull;
        m_hash ^= m_hash >> 29;
    }

    void add(const void* pointer) { add(static_cast<uint64_t>(std::bit_cast<uintptr_t>(pointer))); }

    uint64_t hash() const { return m_hash; }

private:
    uint64_t m_hash { 0 };
};

// Reads blocks and the values in them, and follows no other pointer: a broken procedure is for the validation to report.
void hashProcedure(Procedure& procedure, uint64_t& hashOfControlFlow, uint64_t& hashOfValues)
{
    Hasher controlFlow;
    Hasher values;
    controlFlow.add(procedure.size());
    values.add(procedure.values().size());
    values.add(procedure.variables().size());
    values.add(procedure.tuples().size());
    for (BasicBlock* block : procedure) {
        controlFlow.add(block);
        controlFlow.add(static_cast<uint64_t>(block->numSuccessors()) << 32 | block->numPredecessors());
        for (const FrequentedBlock& successor : block->successors())
            controlFlow.add(successor.block());
        for (BasicBlock* predecessor : block->predecessors())
            controlFlow.add(predecessor);

        values.add(block);
        for (Value* value : *block) {
            values.add(value);
            if (!value)
                continue;
            values.add(static_cast<uint64_t>(value->kind().hash()) << 32 | value->type().hash());
            values.add(value->numChildren());
            for (Value* child : value->children())
                values.add(child);
            if (auto* memory = value->as<MemoryValue>()) {
                values.add(static_cast<uint64_t>(memory->offset()));
                values.add(static_cast<uint64_t>(memory->range().begin()) << 32 | memory->range().end());
            } else if (auto* upsilon = value->as<UpsilonValue>())
                values.add(upsilon->phi());
            else if (auto* access = value->as<VariableValue>())
                values.add(access->variable());
        }
    }
    hashOfControlFlow = controlFlow.hash();
    hashOfValues = values.hash();
}

enum class After : uint8_t { Phase, AnythingElse };

void validate(Procedure& procedure, UTF8CStringView dumpBefore, After after)
{
    // Whoever turns validation on with an option gets what upstream does.
    if (procedure.policy().validation.level != ValidationLevel::AtEachPhase) {
        ValidatedForm form;
        Validater(procedure, dumpBefore, ViolationAction::Crash, form).run();
        return;
    }

    uint64_t hashOfControlFlow;
    uint64_t hashOfValues;
    hashProcedure(procedure, hashOfControlFlow, hashOfValues);
    std::unique_ptr<ValidatedForm>& form = procedure.validatedForm();
    if (!form || form->hashOfControlFlow != hashOfControlFlow)
        form = makeUnique<ValidatedForm>();
    else if (after == After::Phase && form->hashOfValues == hashOfValues)
        return;
    form->hashOfControlFlow = hashOfControlFlow;
    form->hashOfValues = hashOfValues;
    Validater(procedure, dumpBefore, ViolationAction::Crash, *form).run();
}

} // anonymous namespace

ValidatedForm::ValidatedForm() = default;
ValidatedForm::~ValidatedForm() = default;

void validate(Procedure& procedure, UTF8CStringView dumpBefore)
{
    validate(procedure, dumpBefore, After::AnythingElse);
}

void validateAfterPhase(Procedure& procedure, UTF8CStringView dumpBefore)
{
    validate(procedure, dumpBefore, After::Phase);
}

std::optional<ValidationRule> findViolatedRule(Procedure& procedure)
{
    ValidatedForm form;
    Validater validater(procedure, { }, ViolationAction::Record, form);
    validater.run();
    return validater.violatedRule();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
