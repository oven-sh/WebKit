/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinearScanInternals.h"

#if ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)

#include "AOTLinearScanOperations.h"
#include "AOTRelocation.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStubRegisters.h"
#include "AOTVector.h"
#include "AirFrameEmission.h"
#include "AirStackSlot.h"
#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3FenceValue.h"
#include "B3SlotBaseValue.h"
#include "B3SwitchValue.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using namespace LinearScan;

namespace {

constexpr LinearScan::OperandKind secondOperandKind = isX86_64() ? LinearScan::OperandKind::LateUse : LinearScan::OperandKind::Use;

Reg placeholderFor(bool isDouble)
{
    return isDouble ? Reg(FPRInfo::fpRegT0) : Reg(GPRInfo::regT0);
}

} // anonymous namespace

uint32_t LinearScanCodeGenerator::ensureVirtualRegister(NodeID id)
{
    Node& n = node(id);
    if (n.vreg)
        return n.vreg;
    VirtualRegister v;
    v.node = id;
    v.isDouble = n.isDouble();
    v.isRematerialized = n.form == Form::Constant || n.form == Form::Rematerialized;
    m_virtualRegisters.append(v);
    n.vreg = m_virtualRegisters.size() - 1;
    return n.vreg;
}

void LinearScanCodeGenerator::noteUse(NodeID id, Reg hint)
{
    Node& n = node(id);
    if (n.uses != UINT16_MAX)
        ++n.uses;
    uint32_t vreg = ensureVirtualRegister(id);
    VirtualRegister& v = m_virtualRegisters[vreg];
    if (n.block != m_currentBlock && !v.isRematerialized) {
        if (!v.global) {
            m_globals.append(vreg);
            v.global = m_globals.size();
            Block& definingBlock = m_blocks[n.block];
            v.nextDefinedInBlock = definingBlock.firstDefinedGlobal;
            definingBlock.firstDefinedGlobal = vreg;
        }
        if (v.lastBlockWithExposedUse != m_currentBlock + 1) {
            v.lastBlockWithExposedUse = m_currentBlock + 1;
            m_exposedUses.append(v.global - 1);
        }
    }
    if (hint && !(n.hint && m_blocks[m_currentBlock].isRare && !m_blocks[n.block].isRare))
        n.hint = encode(hint);
}

void LinearScanCodeGenerator::appendOperand(NodeID id, OperandKind kind, uint8_t fixed, uint8_t flags)
{
    m_operands.append(Operand { id, kind, fixed, 0, flags });
    ++node(m_currentNode).numberOfOperands;
}

LinearScan::Operand& LinearScanCodeGenerator::takeOperand(NodeID id, OperandKind kind)
{
    Operand& operand = m_operands[m_operandCursor++];
    RELEASE_ASSERT(operand.node == id && operand.kind == kind);
    return operand;
}

void LinearScanCodeGenerator::addClobber(Position from, Position to, uint64_t registers)
{
    registers &= m_generalRegisters | m_doubleRegisters | m_calleeSaves;
    if (registers)
        m_clobbers.append(ClobberEvent { from, to, registers });
}

void LinearScanCodeGenerator::noteFrameUse(bool needsRecord)
{
    m_needsFrame = true;
    m_needsFrameRecord |= needsRecord;
    m_blocks[m_currentBlock].usesFrame = true;
}

template<Pass pass>
Reg LinearScanCodeGenerator::use(NodeID id, OperandKind kind, Reg hint)
{
    const Node& n = node(id);
    if (n.form == Form::FixedRegister) {
        if constexpr (pass == Pass::Select) {
            if (decode(n.base) == Reg(MacroAssembler::framePointerRegister))
                noteFrameUse(true);
        }
        return decode(n.base);
    }
    if constexpr (pass == Pass::Select) {
        if (n.form != Form::Plain && n.form != Form::Constant && n.form != Form::Rematerialized) {
            refuse();
            return placeholderFor(n.isDouble());
        }
        if (n.form == Form::Rematerialized && (n.opcode == B3::SlotBase || decode(n.base) == Reg(MacroAssembler::framePointerRegister)))
            noteFrameUse(true);
        appendOperand(id, kind, hint ? encode(hint) : 0);
        noteUse(id, hint);
        return placeholderFor(n.isDouble());
    } else
        return decode(takeOperand(id, kind).location);
}

template<Pass pass>
ValueLocation LinearScanCodeGenerator::useAnywhere(NodeID id, OperandKind kind, Reg hint)
{
    const Node& n = node(id);
    switch (n.form) {
    case Form::FixedRegister:
        if constexpr (pass == Pass::Select) {
            if (decode(n.base) == Reg(MacroAssembler::framePointerRegister))
                noteFrameUse(true);
        }
        return { n.base, id };
    case Form::Rematerialized:
        if constexpr (pass == Pass::Select) {
            if (n.opcode == B3::SlotBase || decode(n.base) == Reg(MacroAssembler::framePointerRegister))
                noteFrameUse(true);
        }
        return { locationMaterialized, id };
    case Form::Constant:
        return { locationMaterialized, id };
    case Form::Plain:
        break;
    default:
        refuse();
        fail();
        return { locationMaterialized, id };
    }
    if constexpr (pass == Pass::Select) {
        appendOperand(id, kind, hint ? encode(hint) : 0);
        noteUse(id, hint);
        return { encode(placeholderFor(n.isDouble())), id };
    } else
        return { takeOperand(id, kind).location, id };
}

template<Pass pass>
ValueLocation LinearScanCodeGenerator::useRegisterOrSlot(NodeID id, bool isLate, Reg hint)
{
    if (isX86_64() && node(id).form == Form::Plain)
        return useAnywhere<pass>(id, isLate ? OperandKind::LateUseOrSlot : OperandKind::UseOrSlot, hint);
    return { encode(use<pass>(id, isLate ? OperandKind::LateUse : OperandKind::Use, hint)), id };
}

template<Pass pass>
Reg LinearScanCodeGenerator::define(NodeID id, OperandKind kind, Reg fixed, NodeID sharedWith)
{
    if constexpr (pass == Pass::Select) {
        uint32_t vreg = ensureVirtualRegister(id);
        appendOperand(id, kind, fixed ? encode(fixed) : 0);
        if (sharedWith && node(sharedWith).vreg && !m_virtualRegisters[node(sharedWith).vreg].isRematerialized)
            m_virtualRegisters[vreg].hintVirtualRegister = node(sharedWith).vreg;
        return placeholderFor(node(id).isDouble());
    } else
        return decode(takeOperand(id, kind).location);
}

template<Pass pass>
Reg LinearScanCodeGenerator::temporary(bool isDouble)
{
    if constexpr (pass == Pass::Select) {
        appendOperand(0, OperandKind::Temporary, 0, isDouble ? IsDoubleOperand : 0);
        return placeholderFor(isDouble);
    } else
        return decode(takeOperand(0, OperandKind::Temporary).location);
}

template<Pass pass>
void LinearScanCodeGenerator::fold(NodeID id)
{
    if constexpr (pass == Pass::Select) {
        if (!isTestedAnew(node(id), node(m_currentNode)))
            node(id).form = Form::Folded;
    }
}

template<Pass pass>
void LinearScanCodeGenerator::chooseOperandOrder(NodeID id)
{
    if constexpr (pass == Pass::Select) {
        Node& n = node(id);
        bool commutes = n.isDouble() ? n.opcode == B3::Add || n.opcode == B3::Mul : isCommutative(n.opcode);
        const Node& left = node(n.a);
        const Node& right = node(n.b);
        if (!commutes || left.form != Form::Plain || right.form != Form::Plain || n.a == n.b)
            return;
        bool leftDies = !left.uses;
        bool rightDies = !right.uses;
        if (leftDies != rightDies ? rightDies : right.slot > left.slot)
            n.flags |= IsCommuted;
    }
}

template<Pass pass>
void LinearScanCodeGenerator::prepareArguments(Vector<Transfer, 8>& transfers)
{
    if constexpr (pass == Pass::Select)
        node(m_currentNode).flags |= HasArgumentUses;
    else {
        emitParallelMoves(transfers);
        emitMovesAt(mainPositionOf(node(m_currentNode).slot));
    }
}

std::pair<NodeID, uint8_t> LinearScanCodeGenerator::scaledOperandOf(NodeID candidate, const Node& user) const
{
    const Node& n = node(candidate);
    if ((n.form != Form::Plain && n.form != Form::Dead) || n.epoch != user.epoch)
        return { 0, 0 };
    int64_t constant;
    if (n.opcode == B3::Shl && n.isWide() && constantOf(n.b, constant) && constant >= 1 && constant <= 3 && !isConstant(n.a))
        return { n.a, static_cast<uint8_t>(constant) };
    if (n.opcode == B3::Mul && n.isWide()) {
        if (constantOf(n.a, constant) && (constant == 2 || constant == 4 || constant == 8) && !isConstant(n.b))
            return { n.b, static_cast<uint8_t>(std::countr_zero(static_cast<uint64_t>(constant))) };
        if (constantOf(n.b, constant) && (constant == 2 || constant == 4 || constant == 8) && !isConstant(n.a))
            return { n.a, static_cast<uint8_t>(std::countr_zero(static_cast<uint64_t>(constant))) };
    }
    return { 0, 0 };
}

AddressPlan LinearScanCodeGenerator::planAddress(NodeID pointer, int64_t offset, const Node& user) const
{
    AddressPlan plan;
    for (;;) {
        const Node& n = node(pointer);
        if (n.form == Form::FixedRegister) {
            plan.fixedBase = decode(n.base).gpr();
            break;
        }
        if (n.form == Form::Rematerialized) {
            if (n.opcode == B3::SlotBase) {
                plan.fixedBase = MacroAssembler::framePointerRegister;
                plan.slot = b3ValueOf(pointer)->as<B3::SlotBaseValue>()->slot();
                break;
            }
            if (n.opcode == B3::Add && fitsInt32(offset + n.constant)) {
                plan.fixedBase = decode(n.base).gpr();
                offset += n.constant;
                break;
            }
            plan.base = pointer;
            break;
        }
        if ((n.form == Form::Plain || n.form == Form::Dead) && n.opcode == B3::Add && n.isWide()) {
            int64_t constant;
            if (constantOf(n.b, constant) && fitsInt32(offset + constant)) {
                offset += constant;
                pointer = n.a;
                continue;
            }
            if (constantOf(n.a, constant) && fitsInt32(offset + constant)) {
                offset += constant;
                pointer = n.b;
                continue;
            }
            if (!plan.index && n.epoch == user.epoch && !isConstant(n.a) && !isConstant(n.b)) {
                if (auto [operand, shift] = scaledOperandOf(n.b, user); operand) {
                    plan.index = operand;
                    plan.shift = shift;
                    pointer = n.a;
                    continue;
                }
                if (auto [operand, shift] = scaledOperandOf(n.a, user); operand) {
                    plan.index = operand;
                    plan.shift = shift;
                    pointer = n.b;
                    continue;
                }
                plan.index = n.b;
                pointer = n.a;
                continue;
            }
        }
        plan.base = pointer;
        break;
    }
    plan.offset = static_cast<int32_t>(offset);
    return plan;
}

bool LinearScanCodeGenerator::canFoldLoad(NodeID load, const Node& user) const
{
    if constexpr (!isX86_64())
        return false;
    const Node& n = node(load);
    if (n.form != Form::Plain || n.rawUses != 1 || n.memoryEpoch != user.memoryEpoch || n.block != user.block)
        return false;
    if (n.has(TakesTrap) && (user.opcode != B3::Branch || user.slot > n.next))
        return false;
    return B3::isLoad(n.opcode) && !n.has(Traps);
}

bool LinearScanCodeGenerator::isTestedAnew(const Node& comparison, const Node& user) const
{
    UNUSED_PARAM(user);
    return isComparison(comparison.opcode) && (comparison.form == Form::Plain || comparison.form == Form::Dead) && comparison.rawUses > 1;
}

bool LinearScanCodeGenerator::testsBitsAnew(const Node& comparison) const
{
    if ((comparison.opcode != B3::Equal && comparison.opcode != B3::NotEqual) || !isZero(comparison.b))
        return false;
    const Node& bits = node(comparison.a);
    return bits.opcode == B3::BitAnd && bits.rawUses == 1 && bits.type == comparison.operandType && (bits.form == Form::Plain || bits.form == Form::Dead);
}

void LinearScanCodeGenerator::noteFlagsOf(NodeID id)
{
    m_nodeInFlags = id;
    m_sizeWhenFlagsWereSet = m_jit->m_assembler.codeSize();
}

bool LinearScanCodeGenerator::flagsTell(NodeID id, bool isWide) const
{
    return isX86_64() && m_nodeInFlags == id && node(id).isWide() == isWide && m_sizeWhenFlagsWereSet == m_jit->m_assembler.codeSize();
}

void LinearScanCodeGenerator::noteTrapTakenBy(const Node& access, const MemoryOperand& operand)
{
    m_pendingTrap = access.c;
    m_pendingTrapBegin = m_jit->labelIgnoringWatchpoints();
    m_pendingTrapIsNarrow = operand.hasIndex();
    m_pendingTrapRegister = m_pendingTrapIsNarrow ? operand.index : operand.base;
}

template<Pass pass>
bool LinearScanCodeGenerator::loadIsFolded(NodeID load, const Node& user) const
{
    if constexpr (pass == Pass::Select)
        return canFoldLoad(load, user);
    else
        return node(load).form == Form::Folded;
}

bool LinearScanCodeGenerator::isFoldableCondition(NodeID condition, NodeType expected, const Node& user) const
{
    const Node& n = node(condition);
    if (n.type == expected && isTestedAnew(n, user))
        return true;
    if (n.form != Form::Plain || n.rawUses != 1 || n.type != expected)
        return false;
    if (n.opcode == B3::BitXor) {
        const Node& negated = node(n.a);
        return isConstant(n.b) && node(n.b).constant == 1 && (isComparison(negated.opcode) || negated.opcode == B3::BitXor) && isFoldableCondition(n.a, n.type, user);
    }
    if (B3::isLoad(n.opcode))
        return n.opcode != B3::Load8S && n.opcode != B3::Load16S && !n.isDouble() && canFoldLoad(condition, user);
    return isComparison(n.opcode) || n.opcode == B3::BitAnd;
}

template<Pass pass>
bool LinearScanCodeGenerator::conditionIsFolded(NodeID condition, NodeType expected, const Node& user) const
{
    if constexpr (pass == Pass::Select)
        return isFoldableCondition(condition, expected, user);
    else
        return node(condition).form == Form::Folded || (node(condition).type == expected && isTestedAnew(node(condition), user));
}

template<Pass pass>
bool LinearScanCodeGenerator::isLogicalCondition(NodeID condition, const Node& user) const
{
    const Node& n = node(condition);
    if (n.opcode != B3::BitOr && n.opcode != B3::BitAnd)
        return false;
    const Node& first = node(n.a);
    const Node& second = node(n.b);
    if (!isComparison(first.opcode) || !isComparison(second.opcode) || n.a == n.b || isTestedAnew(first, user) || isTestedAnew(second, user))
        return false;
    if (node(first.a).opcode == B3::BitXor || node(second.a).opcode == B3::BitXor)
        return false;
    if constexpr (pass == Pass::Emit)
        return n.form == Form::Folded && first.form == Form::Folded && second.form == Form::Folded;
    else
        return n.form == Form::Plain && n.rawUses == 1 && n.block == user.block && n.type == user.operandType && isFoldableCondition(n.a, n.type, user) && isFoldableCondition(n.b, n.type, user);
}

bool LinearScanCodeGenerator::replacesImmediate(int64_t constant, bool hasOneByteForm, OperandPlan& plan) const
{
    if (hasOneByteForm && constant == static_cast<int8_t>(constant))
        return false;
    std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(constant);
    if (!pinned || !pinned->replacesImmediates || (pinned->offset && (!isX86_64() || *pinned->offset != static_cast<int8_t>(*pinned->offset))))
        return false;
    plan.pinned = *pinned;
    plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
    return true;
}

template<Pass pass>
OperandPlan LinearScanCodeGenerator::planComparison(const Node& comparison, const Node& user, bool mayFoldLoads) const
{
    OperandPlan plan;
    NodeID left = comparison.a;
    NodeID right = comparison.b;
    bool isWide = comparison.operandType == NodeType::Int64;
    if (isConstant(left) && !isConstant(right)) {
        std::swap(left, right);
        plan.isCommuted = true;
    }
    plan.left = left;
    plan.right = right;
    MacroAssembler::RelationalCondition condition = relationalConditionOf(comparison.opcode);
    auto isWholeLoad = [&](NodeID candidate) {
        return mayFoldLoads && node(candidate).opcode == B3::Load && node(candidate).type == comparison.operandType && loadIsFolded<pass>(candidate, user);
    };
    int64_t constant;
    if (constantOf(right, constant)) {
        if (!isWide || fitsInt32(constant)) {
            plan.shape = OperandShape::Immediate;
            plan.immediate = constant;
            if (!mayFoldLoads)
                return plan;
            if (isWide && !isWholeLoad(left) && replacesImmediate(constant, true, plan))
                return plan;
            B3::Opcode leftOpcode = node(left).opcode;
            if (leftOpcode == B3::Load8Z && comparesWithoutSign(condition) && constant >= 0 && constant <= 255 && loadIsFolded<pass>(left, user)) {
                plan.load = left;
                plan.loadWidth = 8;
            } else if (leftOpcode == B3::Load16Z && comparesWithoutSign(condition) && constant >= 0 && constant <= 65535 && loadIsFolded<pass>(left, user)) {
                plan.load = left;
                plan.loadWidth = 16;
            } else if (isWholeLoad(left)) {
                plan.load = left;
                plan.loadWidth = isWide ? 64 : 32;
            }
            plan.loadIsLeft = !!plan.load;
            return plan;
        }
        if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(constant)) {
            plan.pinned = *pinned;
            plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
            if (plan.shape == OperandShape::PinnedRegister || isX86_64())
                return plan;
        }
        plan.shape = OperandShape::Register;
        return plan;
    }
    if (isWholeLoad(right)) {
        plan.shape = OperandShape::Load;
        plan.load = right;
        plan.loadWidth = isWide ? 64 : 32;
        return plan;
    }
    if (isWholeLoad(left)) {
        plan.shape = OperandShape::Load;
        plan.load = left;
        plan.loadIsLeft = true;
        plan.loadWidth = isWide ? 64 : 32;
        return plan;
    }
    return plan;
}

template<Pass pass>
OperandPlan LinearScanCodeGenerator::planTest(NodeID left, NodeID right, bool isWide, const Node& user, bool mayFoldLoads) const
{
    OperandPlan plan;
    if (isConstant(left) && !isConstant(right))
        std::swap(left, right);
    plan.left = left;
    plan.right = right;
    int64_t constant;
    if (!constantOf(right, constant))
        return plan;
    const Node& leftNode = node(left);
    if (isX86_64() && mayFoldLoads && constant == 1 && leftNode.opcode == B3::ZShr && leftNode.isWide() == isWide && !isConstant(leftNode.b)) {
        bool isFolded = pass == Pass::Select ? leftNode.form == Form::Plain && leftNode.rawUses == 1 && leftNode.block == user.block : leftNode.form == Form::Folded;
        if (isFolded) {
            plan.shape = OperandShape::BitTest;
            plan.foldedShift = left;
            plan.left = leftNode.a;
            plan.right = leftNode.b;
            return plan;
        }
    }
    if (!isWide || fitsInt32(constant)) {
        plan.shape = OperandShape::Immediate;
        plan.immediate = constant;
        if (!mayFoldLoads)
            return plan;
        bool isWholeLoad = leftNode.opcode == B3::Load && !leftNode.isDouble() && leftNode.isWide() == isWide;
        if (isWide && !(isWholeLoad && loadIsFolded<pass>(left, user)) && (constant < 0 || constant > 127) && replacesImmediate(constant, false, plan))
            return plan;
        if (leftNode.opcode == B3::Load8Z && constant >= 0 && constant <= 255 && loadIsFolded<pass>(left, user)) {
            plan.load = left;
            plan.loadWidth = 8;
        } else if (leftNode.opcode == B3::Load16Z && constant >= 0 && constant <= 65535 && loadIsFolded<pass>(left, user)) {
            plan.load = left;
            plan.loadWidth = 16;
        } else if (isWholeLoad && loadIsFolded<pass>(left, user)) {
            plan.load = left;
            plan.loadWidth = isWide ? 64 : 32;
        }
        plan.loadIsLeft = !!plan.load;
        return plan;
    }
    if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(constant)) {
        plan.pinned = *pinned;
        plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
        if (plan.shape == OperandShape::PinnedRegister || isX86_64())
            return plan;
    }
    plan.shape = OperandShape::Register;
    return plan;
}

template<Pass pass>
OperandPlan LinearScanCodeGenerator::planBinary(const Node& n) const
{
    OperandPlan plan;
    B3::Opcode opcode = n.opcode;
    NodeID left = n.a;
    NodeID right = n.b;
    bool isWide = n.isWide();
    if (isCommutative(opcode) && isConstant(left) && !isConstant(right))
        std::swap(left, right);
    else if (n.has(IsCommuted))
        std::swap(left, right);
    plan.left = left;
    plan.right = right;
    int64_t constant;
    if (constantOf(right, constant)) {
        std::optional<B3::PinnedConstant> pinned;
        if (isWide && opcode != B3::Mul)
            pinned = pinnedConstantFor(constant);
        bool isImmediate = !isWide || fitsInt32(constant);
        if (pinned && (!isImmediate || pinned->replacesImmediates) && (!pinned->offset || isX86_64())) {
            plan.pinned = *pinned;
            plan.shape = pinned->offset ? OperandShape::PinnedMemory : OperandShape::PinnedRegister;
            return plan;
        }
        if (isImmediate) {
            plan.shape = OperandShape::Immediate;
            plan.immediate = constant;
        }
        return plan;
    }
    if (opcode == B3::Add && isWide) {
        if (auto [operand, shift] = scaledOperandOf(right, n); operand) {
            plan.shape = OperandShape::ScaledIndex;
            plan.right = operand;
            plan.shift = shift;
        } else if (auto [operand, shift] = scaledOperandOf(left, n); operand) {
            plan.shape = OperandShape::ScaledIndex;
            plan.left = right;
            plan.right = operand;
            plan.shift = shift;
        }
        return plan;
    }
    if (opcode != B3::Mul && node(right).opcode == B3::Load && node(right).type == n.type && loadIsFolded<pass>(right, n)) {
        plan.shape = OperandShape::Load;
        plan.load = right;
        return plan;
    }
    if (isCommutative(opcode) && opcode != B3::Mul && node(left).opcode == B3::Load && node(left).type == n.type && loadIsFolded<pass>(left, n)) {
        plan.shape = OperandShape::Load;
        plan.load = left;
        plan.left = right;
        plan.right = left;
        return plan;
    }
    return plan;
}

bool LinearScanCodeGenerator::storesImmediate(const Node& store) const
{
    int64_t constant;
    if (!constantOf(store.a, constant) || store.operandType == NodeType::Double)
        return false;
    // An immediate is four bytes of the store. Zero in a register is two, and another constant pays from its second user on.
    if (isX86_64() && m_prefersSmallCode && store.opcode == B3::Store && (!constant || node(store.a).rawUses >= 2))
        return false;
    return store.opcode != B3::Store || store.operandType == NodeType::Int32 || fitsInt32(constant);
}

template<Pass pass>
MemoryOperand LinearScanCodeGenerator::operandFor(const AddressPlan& plan, OperandKind kind)
{
    MemoryOperand operand;
    operand.offset = plan.offset;
    if constexpr (pass == Pass::Select) {
        if (plan.fixedBase == MacroAssembler::framePointerRegister)
            noteFrameUse(true);
    } else if (plan.slot)
        operand.offset += static_cast<int32_t>(plan.slot->offsetFromFP());
    operand.base = plan.base ? use<pass>(plan.base, kind).gpr() : plan.fixedBase;
    if (plan.index) {
        operand.index = use<pass>(plan.index, kind).gpr();
        operand.shift = plan.shift;
    }
    return operand;
}

template<Pass pass>
MemoryOperand LinearScanCodeGenerator::operandForLoad(NodeID load, const Node& user, OperandKind kind)
{
    MemoryOperand operand = operandFor<pass>(planAddress(node(load).a, node(load).offset(), user), kind);
    if constexpr (pass == Pass::Emit) {
        if (node(load).has(TakesTrap))
            noteTrapTakenBy(node(load), operand);
    }
    return operand;
}

template<Pass pass>
void LinearScanCodeGenerator::lowerIntegerBinary(NodeID id)
{
    chooseOperandOrder<pass>(id);
    const Node& n = node(id);
    B3::Opcode opcode = n.opcode;
    bool isWide = n.isWide();
    Reg hint = decode(n.hint);
    auto isSumOffFixedRegister = [&](NodeID operand) {
        return node(operand).form == Form::Rematerialized && node(operand).opcode == B3::Add;
    };
    if (opcode == B3::Add && isWide && (isSumOffFixedRegister(n.a) || isSumOffFixedRegister(n.b))) {
        AddressPlan address = planAddress(id, 0, n);
        if (address.fixedBase != InvalidGPRReg && address.index && !address.base && !address.slot) {
            MemoryOperand operand = operandFor<pass>(address);
            GPRReg result = define<pass>(id).gpr();
            if constexpr (pass == Pass::Emit) {
                m_jit->getEffectiveAddress(operand.baseIndex(), result);
                finishUnit();
            }
            return;
        }
    }
    OperandPlan plan = planBinary<pass>(n);

    if (plan.shape == OperandShape::Load || plan.shape == OperandShape::PinnedMemory) {
        MemoryOperand operand;
        if (plan.shape == OperandShape::Load) {
            fold<pass>(plan.load);
            operand = operandForLoad<pass>(plan.load, n, OperandKind::LateUse);
        } else {
            operand.base = plan.pinned.reg.gpr();
            operand.offset = *plan.pinned.offset;
        }
        GPRReg left = use<pass>(plan.left, OperandKind::Use, hint).gpr();
        GPRReg result = define<pass>(id, OperandKind::Definition, Reg(), plan.left).gpr();
#if CPU(X86_64)
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            moveRegister(Reg(left), Reg(result));
            withAddress(operand, [&](auto address) {
                switch (opcode) {
                case B3::Add:
                    isWide ? jit.add64(address, result) : jit.add32(address, result);
                    break;
                case B3::Sub:
                    isWide ? jit.sub64(address, result) : jit.sub32(address, result);
                    break;
                case B3::BitAnd:
                    isWide ? jit.and64(address, result) : jit.and32(address, result);
                    break;
                case B3::BitOr:
                    isWide ? jit.or64(address, result) : jit.or32(address, result);
                    break;
                case B3::BitXor:
                    isWide ? jit.xor64(address, result) : jit.xor32(address, result);
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }
            });
            finishUnit();
            if (opcode == B3::BitAnd || opcode == B3::BitOr || opcode == B3::BitXor)
                noteFlagsOf(id);
        }
#else
        UNUSED_VARIABLE(left);
        UNUSED_VARIABLE(result);
        RELEASE_ASSERT_NOT_REACHED();
#endif
        return;
    }

    GPRReg left = use<pass>(plan.left, OperandKind::Use, hint).gpr();
    if (plan.shape == OperandShape::Immediate) {
        GPRReg result = define<pass>(id, OperandKind::Definition, Reg(), plan.left).gpr();
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
            switch (opcode) {
            case B3::Add:
                isWide ? jit.add64(immediate, left, result) : jit.add32(immediate, left, result);
                break;
            case B3::Sub:
                isWide ? jit.sub64(left, immediate, result) : jit.sub32(left, immediate, result);
                break;
            case B3::Mul:
                if (plan.immediate > 1 && !(plan.immediate & (plan.immediate - 1))) {
                    TrustedImm32 amount(std::countr_zero(static_cast<uint64_t>(plan.immediate)));
                    isWide ? jit.lshift64(left, amount, result) : jit.lshift32(left, amount, result);
                    break;
                }
                if (!isWide) {
                    jit.mul32(immediate, left, result);
                    break;
                }
#if CPU(X86_64)
                jit.mul64(immediate, left, result);
#else
                jit.move(TrustedImm64(static_cast<int64_t>(plan.immediate)), jit.scratchRegister());
                jit.mul64(left, jit.scratchRegister(), result);
#endif
                break;
            case B3::BitAnd:
                isWide ? jit.and64(immediate, left, result) : jit.and32(immediate, left, result);
                break;
            case B3::BitOr:
                isWide ? jit.or64(immediate, left, result) : jit.or32(immediate, left, result);
                break;
            case B3::BitXor:
                isWide ? jit.xor64(immediate, left, result) : jit.xor32(immediate, left, result);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            finishUnit();
            bool isMadeByMove = !plan.immediate || plan.immediate == -1 || plan.immediate == 0xff || plan.immediate == 0xffff;
            if ((opcode == B3::BitAnd || opcode == B3::BitOr || opcode == B3::BitXor) && !isMadeByMove)
                noteFlagsOf(id);
        }
        return;
    }

    bool mayShareWithRight = isCommutative(opcode) || plan.shape == OperandShape::ScaledIndex;
    GPRReg right = InvalidGPRReg;
    ValueLocation rightLocation;
    if (plan.shape == OperandShape::PinnedRegister)
        right = plan.pinned.reg.gpr();
    else if (plan.shape == OperandShape::Register && opcode != B3::Mul) {
        rightLocation = useRegisterOrSlot<pass>(plan.right, !mayShareWithRight && isX86_64());
        if (rightLocation.isRegister())
            right = rightLocation.reg().gpr();
    } else
        right = use<pass>(plan.right, mayShareWithRight ? OperandKind::Use : secondOperandKind).gpr();
    GPRReg result = define<pass>(id, OperandKind::Definition, Reg(), plan.left).gpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
#if CPU(X86_64)
        if (rightLocation.isSlot()) {
            int32_t offset = offsetOfSlotOf(plan.right);
            Address address = addressOfSlot(offset);
            moveRegister(Reg(left), Reg(result));
            switch (opcode) {
            case B3::Add:
                isWide ? jit.add64(address, result) : jit.add32(address, result);
                break;
            case B3::Sub:
                isWide ? jit.sub64(address, result) : jit.sub32(address, result);
                break;
            case B3::BitAnd:
                isWide ? jit.and64(address, result) : jit.and32(address, result);
                break;
            case B3::BitOr:
                isWide ? jit.or64(address, result) : jit.or32(address, result);
                break;
            case B3::BitXor:
                isWide ? jit.xor64(address, result) : jit.xor32(address, result);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            finishUnit(kindOfFrameAccess(offset));
            return;
        }
#endif
        if (plan.shape == OperandShape::ScaledIndex) {
            jit.getEffectiveAddress(MacroAssembler::BaseIndex(left, right, static_cast<MacroAssembler::Scale>(plan.shift)), result);
            finishUnit();
            return;
        }
        switch (opcode) {
        case B3::Add:
            isWide ? jit.add64(left, right, result) : jit.add32(left, right, result);
            break;
        case B3::Sub:
            isWide ? jit.sub64(left, right, result) : jit.sub32(left, right, result);
            break;
        case B3::Mul:
            isWide ? jit.mul64(left, right, result) : jit.mul32(left, right, result);
            break;
        case B3::BitAnd:
            isWide ? jit.and64(left, right, result) : jit.and32(left, right, result);
            break;
        case B3::BitOr:
            isWide ? jit.or64(left, right, result) : jit.or32(left, right, result);
            break;
        case B3::BitXor:
            isWide ? jit.xor64(left, right, result) : jit.xor32(left, right, result);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
        if ((opcode == B3::BitAnd || opcode == B3::BitOr || opcode == B3::BitXor) && left != right)
            noteFlagsOf(id);
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerShift(NodeID id)
{
    const Node& n = node(id);
    B3::Opcode opcode = n.opcode;
    bool isWide = n.isWide();
    Reg hint = decode(n.hint);
    int64_t constant;
    if (constantOf(n.b, constant)) {
        GPRReg left = use<pass>(n.a, OperandKind::Use, hint).gpr();
        GPRReg result = define<pass>(id, OperandKind::Definition, Reg(), n.a).gpr();
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            TrustedImm32 amount(static_cast<int32_t>(constant & (isWide ? 63 : 31)));
            switch (opcode) {
            case B3::Shl:
                isWide ? jit.lshift64(left, amount, result) : jit.lshift32(left, amount, result);
                break;
            case B3::SShr:
                isWide ? jit.rshift64(left, amount, result) : jit.rshift32(left, amount, result);
                break;
            case B3::ZShr:
                isWide ? jit.urshift64(left, amount, result) : jit.urshift32(left, amount, result);
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            finishUnit();
            if (amount.m_value)
                noteFlagsOf(id);
        }
        return;
    }

    GPRReg amount;
#if CPU(X86_64)
    Reg counter(X86Registers::ecx);
    ValueLocation amountLocation = useAnywhere<pass>(n.b, OperandKind::ArgumentUse, counter);
    if constexpr (pass == Pass::Select)
        addClobber(argumentPositionOf(n.slot) + 1, mainPositionOf(n.slot) + 2, bitOf(counter));
    amount = counter.gpr();
#else
    amount = use<pass>(n.b).gpr();
#endif
    GPRReg left = use<pass>(n.a, OperandKind::Use, hint).gpr();
    GPRReg result = define<pass>(id, OperandKind::Definition, Reg(), n.a).gpr();
#if CPU(X86_64)
    Vector<Transfer, 8> transfers;
    if constexpr (pass == Pass::Emit)
        transfers.append(Transfer { encode(counter), amountLocation.location, n.b, n.b, 0, false });
    prepareArguments<pass>(transfers);
#endif
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        switch (opcode) {
        case B3::Shl:
            isWide ? jit.lshift64(left, amount, result) : jit.lshift32(left, amount, result);
            break;
        case B3::SShr:
            isWide ? jit.rshift64(left, amount, result) : jit.rshift32(left, amount, result);
            break;
        case B3::ZShr:
            isWide ? jit.urshift64(left, amount, result) : jit.urshift32(left, amount, result);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerLoad(NodeID id)
{
    const Node& n = node(id);
    AddressPlan plan = planAddress(n.a, n.offset(), n);
    if constexpr (pass == Pass::Select) {
        if (n.has(Traps)) {
            bool isNarrow = plan.index && plan.fixedBase == cellCageBaseGPR && plan.shift == cell32Shift;
            if ((plan.index && !isNarrow) || (!plan.index && (!plan.base || node(plan.base).form != Form::Plain))) {
                refuse();
                return;
            }
        }
    }
    MemoryOperand operand = operandFor<pass>(plan);
    Reg result = define<pass>(id);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        Label begin = jit.labelIgnoringWatchpoints();
        withAddress(operand, [&](auto address) {
            switch (n.opcode) {
            case B3::Load8Z:
                jit.load8(address, result.gpr());
                break;
            case B3::Load8S:
                jit.load8SignedExtendTo32(address, result.gpr());
                break;
            case B3::Load16Z:
                jit.load16(address, result.gpr());
                break;
            case B3::Load16S:
                jit.load16SignedExtendTo32(address, result.gpr());
                break;
            case B3::Load:
                if (n.isDouble())
                    jit.loadDouble(address, result.fpr());
                else if (n.isWide())
                    jit.load64(address, result.gpr());
                else
                    jit.load32(address, result.gpr());
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        });
        if (n.has(Traps) || n.has(TakesTrap)) {
            bool isNarrow = operand.hasIndex();
            B3::Origin origin = b3ValueOf(n.has(Traps) ? id : n.c)->origin();
            m_records->traps.trappingAccesses.append(TrappingAccess { begin, valueOfOrigin(origin), isNarrow ? operand.index : operand.base, isNarrow });
            m_records->units.didGenerateTrappingAccess(begin, origin);
            finishUnit(CodeUnit::Kind::Pinned);
        } else
            finishUnit();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerStore(NodeID id)
{
    const Node& n = node(id);
    if constexpr (pass == Pass::Select) {
        if (n.has(Traps)) {
            refuse();
            return;
        }
    }
    MemoryOperand operand = operandFor<pass>(planAddress(n.b, n.offset(), n));
    if (storesImmediate(n)) {
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            TrustedImm32 immediate(static_cast<int32_t>(node(n.a).constant));
            withAddress(operand, [&](auto address) {
                switch (n.opcode) {
                case B3::Store8:
                    jit.store8(immediate, address);
                    break;
                case B3::Store16:
                    jit.store16(immediate, address);
                    break;
                default:
                    if (n.operandType == NodeType::Int64)
                        jit.store64(immediate, address);
                    else
                        jit.store32(immediate, address);
                    break;
                }
            });
            finishUnit();
        }
        return;
    }
    Reg source = use<pass>(n.a);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        withAddress(operand, [&](auto address) {
            switch (n.opcode) {
            case B3::Store8:
                jit.store8(source.gpr(), address);
                break;
            case B3::Store16:
                jit.store16(source.gpr(), address);
                break;
            default:
                if (n.operandType == NodeType::Double)
                    jit.storeDouble(source.fpr(), address);
                else if (n.operandType == NodeType::Int64)
                    jit.store64(source.gpr(), address);
                else
                    jit.store32(source.gpr(), address);
                break;
            }
        });
        finishUnit();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerComparison(NodeID id)
{
    const Node& n = node(id);
    if (n.operandType == NodeType::Double) {
        FPRReg left = use<pass>(n.a).fpr();
        FPRReg right = use<pass>(n.b).fpr();
        GPRReg result = define<pass>(id).gpr();
        if constexpr (pass == Pass::Emit) {
            m_jit->compareDouble(doubleConditionOf(n.opcode), left, right, result);
            finishUnit();
        }
        return;
    }
    bool isWide = n.operandType == NodeType::Int64;
    OperandPlan plan = planComparison<pass>(n, n, false);
    GPRReg left = use<pass>(plan.left).gpr();
    GPRReg right = InvalidGPRReg;
    if (plan.shape == OperandShape::Register)
        right = use<pass>(plan.right).gpr();
    GPRReg result = define<pass>(id).gpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        MacroAssembler::RelationalCondition condition = relationalConditionOf(n.opcode);
        if (plan.isCommuted)
            condition = MacroAssembler::commute(condition);
        if (plan.shape == OperandShape::Immediate) {
            TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
            isWide ? jit.compare64(condition, left, immediate, result) : jit.compare32(condition, left, immediate, result);
            finishUnit();
            return;
        }
        if (plan.shape == OperandShape::PinnedRegister)
            right = plan.pinned.reg.gpr();
        else if (plan.shape == OperandShape::PinnedMemory) {
            right = jit.scratchRegister();
            jit.load64(Address(plan.pinned.reg.gpr(), *plan.pinned.offset), right);
        }
        isWide ? jit.compare64(condition, left, right, result) : jit.compare32(condition, left, right, result);
        finishUnit();
    }
}

template<Pass pass>
MacroAssembler::Jump LinearScanCodeGenerator::branchOnTest(const Node& test, const Node& user, bool whenNonZero, bool isMadeOnce)
{
    MacroAssembler::ResultCondition condition = whenNonZero ? MacroAssembler::NonZero : MacroAssembler::Zero;
    bool isWide = test.isWide();
    OperandPlan plan = planTest<pass>(test.a, test.b, isWide, user, isMadeOnce);
    if (plan.foldedShift)
        fold<pass>(plan.foldedShift);
    if (plan.load) {
        fold<pass>(plan.load);
        MemoryOperand operand = operandForLoad<pass>(plan.load, user);
#if CPU(X86_64)
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
            return withAddress(operand, [&](auto address) {
                switch (plan.loadWidth) {
                case 8:
                    return jit.branchTest8(condition, address, mask);
                case 16:
                    return jit.branchTest16(condition, address, mask);
                case 32:
                    return jit.branchTest32(condition, address, mask);
                default:
                    return jit.branchTest64(condition, address, mask);
                }
            });
        }
#else
        UNUSED_VARIABLE(operand);
#endif
        return Jump();
    }
    if (plan.shape == OperandShape::Immediate) {
        ValueLocation location = useRegisterOrSlot<pass>(plan.left);
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
#if CPU(X86_64)
            if (location.isSlot()) {
                m_kindOfBranchUnit = kindOfFrameAccess(offsetOfSlotOf(plan.left));
                Address address = addressOfSlot(offsetOfSlotOf(plan.left));
                return isWide ? jit.branchTest64(condition, address, mask) : jit.branchTest32(condition, address, mask);
            }
#endif
            GPRReg left = location.reg().gpr();
            return isWide ? jit.branchTest64(condition, left, mask) : jit.branchTest32(condition, left, mask);
        }
        return Jump();
    }
    GPRReg left = use<pass>(plan.left).gpr();
    GPRReg right = InvalidGPRReg;
    if (plan.shape == OperandShape::Register || plan.shape == OperandShape::BitTest)
        right = use<pass>(plan.right).gpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        switch (plan.shape) {
        case OperandShape::PinnedRegister:
            return jit.branchTest64(condition, left, plan.pinned.reg.gpr());
#if CPU(X86_64)
        case OperandShape::BitTest:
            return isWide ? jit.branchTestBit64(condition, left, right) : jit.branchTestBit32(condition, left, right);
        case OperandShape::PinnedMemory:
            return jit.branchTest64(condition, left, Address(plan.pinned.reg.gpr(), *plan.pinned.offset));
#endif
        default:
            break;
        }
        return isWide ? jit.branchTest64(condition, left, right) : jit.branchTest32(condition, left, right);
    }
    return Jump();
}

template<Pass pass>
MacroAssembler::Jump LinearScanCodeGenerator::branchOnComparison(NodeID id, const Node& user, bool whenTrue)
{
    const Node& comparison = node(id);
    B3::Opcode opcode = comparison.opcode;
    if (comparison.operandType == NodeType::Double) {
        FPRReg left = use<pass>(comparison.a).fpr();
        FPRReg right = use<pass>(comparison.b).fpr();
        if constexpr (pass == Pass::Emit) {
            MacroAssembler::DoubleCondition condition = doubleConditionOf(opcode);
            return m_jit->branchDouble(whenTrue ? condition : MacroAssembler::invert(condition), left, right);
        }
        return Jump();
    }
    bool isMadeOnce = !isTestedAnew(comparison, user);
    if (!isMadeOnce && testsBitsAnew(comparison))
        return branchOnTest<pass>(node(comparison.a), user, opcode == B3::NotEqual ? whenTrue : !whenTrue, false);
    if (isMadeOnce && (opcode == B3::Equal || opcode == B3::NotEqual) && isZero(comparison.b) && !B3::isLoad(node(comparison.a).opcode) && conditionIsFolded<pass>(comparison.a, comparison.operandType, user)) {
        fold<pass>(comparison.a);
        return branchOnFolded<pass>(comparison.a, user, opcode == B3::NotEqual ? whenTrue : !whenTrue);
    }

    bool isWide = comparison.operandType == NodeType::Int64;
    OperandPlan plan = planComparison<pass>(comparison, user, isMadeOnce);
    MacroAssembler::RelationalCondition condition = relationalConditionOf(opcode);
    if (plan.isCommuted)
        condition = MacroAssembler::commute(condition);
    if (!whenTrue)
        condition = MacroAssembler::invert(condition);

    if (plan.load) {
        fold<pass>(plan.load);
        MemoryOperand operand = operandForLoad<pass>(plan.load, user);
        GPRReg other = InvalidGPRReg;
        if (plan.shape != OperandShape::Immediate)
            other = use<pass>(plan.loadIsLeft ? plan.right : plan.left).gpr();
#if CPU(X86_64)
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            if (plan.shape == OperandShape::Immediate) {
                TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
                if (operand.hasIndex() && plan.loadWidth == 64) {
                    GPRReg scratch = jit.scratchRegister();
                    jit.load64(operand.baseIndex(), scratch);
                    return jit.branch64(condition, scratch, immediate);
                }
                switch (plan.loadWidth) {
                case 8:
                    return withAddress(operand, [&](auto address) { return jit.branch8(condition, address, immediate); });
                case 16:
                    return withAddress(operand, [&](auto address) { return jit.branch16(condition, address, immediate); });
                case 32:
                    return withAddress(operand, [&](auto address) { return jit.branch32(condition, address, immediate); });
                default:
                    return jit.branch64(condition, operand.address(), immediate);
                }
            }
            if (!plan.loadIsLeft)
                condition = MacroAssembler::commute(condition);
            return withAddress(operand, [&](auto address) {
                return isWide ? jit.branch64(condition, address, other) : jit.branch32(condition, address, other);
            });
        }
#else
        UNUSED_VARIABLE(operand);
        UNUSED_VARIABLE(other);
#endif
        return Jump();
    }

    if (plan.shape == OperandShape::Immediate) {
        ValueLocation location = useRegisterOrSlot<pass>(plan.left);
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
#if CPU(X86_64)
            if (location.isSlot()) {
                m_kindOfBranchUnit = kindOfFrameAccess(offsetOfSlotOf(plan.left));
                Address address = addressOfSlot(offsetOfSlotOf(plan.left));
                return isWide ? jit.branch64(condition, address, immediate) : jit.branch32(condition, address, immediate);
            }
#endif
#if CPU(X86_64)
            if (!plan.immediate && flagsTell(plan.left, isWide)) {
                switch (condition) {
                case MacroAssembler::Equal:
                    return jit.branchOnFlags(MacroAssembler::Zero);
                case MacroAssembler::NotEqual:
                    return jit.branchOnFlags(MacroAssembler::NonZero);
                case MacroAssembler::LessThan:
                    return jit.branchOnFlags(MacroAssembler::Signed);
                case MacroAssembler::GreaterThanOrEqual:
                    return jit.branchOnFlags(MacroAssembler::PositiveOrZero);
                default:
                    break;
                }
            }
#endif
            GPRReg left = location.reg().gpr();
            return isWide ? jit.branch64(condition, left, immediate) : jit.branch32(condition, left, immediate);
        }
        return Jump();
    }
    GPRReg left = use<pass>(plan.left).gpr();
    ValueLocation rightLocation;
    if (plan.shape == OperandShape::Register)
        rightLocation = useRegisterOrSlot<pass>(plan.right);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        switch (plan.shape) {
        case OperandShape::PinnedRegister:
            return jit.branch64(condition, left, plan.pinned.reg.gpr());
        case OperandShape::PinnedMemory:
            return jit.branch64(condition, left, Address(plan.pinned.reg.gpr(), *plan.pinned.offset));
        default:
            break;
        }
#if CPU(X86_64)
        if (rightLocation.isSlot()) {
            m_kindOfBranchUnit = kindOfFrameAccess(offsetOfSlotOf(plan.right));
            Address address = addressOfSlot(offsetOfSlotOf(plan.right));
            return isWide ? jit.branch64(condition, left, address) : jit.branch32(condition, left, address);
        }
#endif
        GPRReg right = rightLocation.reg().gpr();
        return isWide ? jit.branch64(condition, left, right) : jit.branch32(condition, left, right);
    }
    return Jump();
}

template<Pass pass>
MacroAssembler::Jump LinearScanCodeGenerator::branchOnFolded(NodeID id, const Node& user, bool whenTrue)
{
    const Node& condition = node(id);
    if (condition.opcode == B3::BitAnd)
        return branchOnTest<pass>(condition, user, whenTrue);
    if (condition.opcode == B3::BitXor) {
        fold<pass>(condition.a);
        return branchOnFolded<pass>(condition.a, user, !whenTrue);
    }
    if (B3::isLoad(condition.opcode)) {
        MemoryOperand operand = operandForLoad<pass>(id, user);
#if CPU(X86_64)
        if constexpr (pass == Pass::Emit) {
            MacroAssembler::ResultCondition result = whenTrue ? MacroAssembler::NonZero : MacroAssembler::Zero;
            return withAddress(operand, [&](auto address) {
                switch (condition.opcode) {
                case B3::Load8Z:
                    return m_jit->branchTest8(result, address);
                case B3::Load16Z:
                    return m_jit->branchTest16(result, address);
                default:
                    return condition.isWide() ? m_jit->branchTest64(result, address) : m_jit->branchTest32(result, address);
                }
            });
        }
#else
        UNUSED_VARIABLE(operand);
#endif
        return Jump();
    }
    return branchOnComparison<pass>(id, user, whenTrue);
}

template<Pass pass>
MacroAssembler::Jump LinearScanCodeGenerator::branchOn(NodeID id, NodeType expected, const Node& user, bool whenTrue)
{
    if (conditionIsFolded<pass>(id, expected, user)) {
        fold<pass>(id);
        return branchOnFolded<pass>(id, user, whenTrue);
    }
    GPRReg reg = use<pass>(id).gpr();
    if constexpr (pass == Pass::Emit) {
        MacroAssembler::ResultCondition result = whenTrue ? MacroAssembler::NonZero : MacroAssembler::Zero;
#if CPU(X86_64)
        if (flagsTell(id, expected == NodeType::Int64))
            return m_jit->branchOnFlags(result);
#endif
        return expected == NodeType::Int64 ? m_jit->branchTest64(result, reg) : m_jit->branchTest32(result, reg);
    }
    return Jump();
}

template<Pass pass>
bool LinearScanCodeGenerator::canSetFlagsFor(NodeID id, NodeType expected, const Node& user) const
{
    if (!conditionIsFolded<pass>(id, expected, user))
        return true;
    const Node& condition = node(id);
    if (condition.opcode == B3::BitXor)
        return canSetFlagsFor<pass>(condition.a, condition.type, user);
    if (B3::isLoad(condition.opcode))
        return false;
    if (condition.opcode == B3::BitAnd)
        return true;
    if (condition.operandType == NodeType::Double)
        return false;
    if (!isTestedAnew(condition, user) && (condition.opcode == B3::Equal || condition.opcode == B3::NotEqual) && isZero(condition.b) && !B3::isLoad(node(condition.a).opcode) && conditionIsFolded<pass>(condition.a, condition.operandType, user))
        return canSetFlagsFor<pass>(condition.a, condition.operandType, user);
    return true;
}

template<Pass pass>
MacroAssembler::RelationalCondition LinearScanCodeGenerator::setFlagsFor(NodeID id, NodeType expected, const Node& user, bool whenTrue)
{
    auto byZero = [&](bool whenNonZero) {
        return whenNonZero ? MacroAssembler::NotEqual : MacroAssembler::Equal;
    };
    if (!conditionIsFolded<pass>(id, expected, user)) {
        GPRReg reg = use<pass>(id).gpr();
        if constexpr (pass == Pass::Emit)
            expected == NodeType::Int64 ? m_jit->testOnFlags64(reg, reg) : m_jit->testOnFlags32(reg, reg);
        return byZero(whenTrue);
    }
    fold<pass>(id);
    const Node& condition = node(id);
    if (condition.opcode == B3::BitXor)
        return setFlagsFor<pass>(condition.a, condition.type, user, !whenTrue);
    if (condition.opcode == B3::BitAnd) {
        bool isWide = condition.isWide();
        OperandPlan plan = planTest<pass>(condition.a, condition.b, isWide, user, false);
        GPRReg left = use<pass>(plan.left).gpr();
        GPRReg right = InvalidGPRReg;
        if (plan.shape == OperandShape::Register)
            right = use<pass>(plan.right).gpr();
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            switch (plan.shape) {
            case OperandShape::Immediate: {
                TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
                isWide ? jit.testOnFlags64(left, mask) : jit.testOnFlags32(left, mask);
                break;
            }
            case OperandShape::PinnedRegister:
                jit.testOnFlags64(left, plan.pinned.reg.gpr());
                break;
#if CPU(X86_64)
            case OperandShape::PinnedMemory:
                jit.testOnFlags64(Address(plan.pinned.reg.gpr(), *plan.pinned.offset), left);
                break;
#endif
            default:
                isWide ? jit.testOnFlags64(left, right) : jit.testOnFlags32(left, right);
                break;
            }
        }
        return byZero(whenTrue);
    }
    B3::Opcode opcode = condition.opcode;
    if (isTestedAnew(condition, user) && testsBitsAnew(condition)) {
        const Node& bits = node(condition.a);
        bool isWide = bits.isWide();
        OperandPlan plan = planTest<pass>(bits.a, bits.b, isWide, user, false);
        GPRReg left = use<pass>(plan.left).gpr();
        GPRReg right = InvalidGPRReg;
        if (plan.shape == OperandShape::Register)
            right = use<pass>(plan.right).gpr();
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            switch (plan.shape) {
            case OperandShape::Immediate: {
                TrustedImm32 mask(static_cast<int32_t>(plan.immediate));
                isWide ? jit.testOnFlags64(left, mask) : jit.testOnFlags32(left, mask);
                break;
            }
            case OperandShape::PinnedRegister:
                jit.testOnFlags64(left, plan.pinned.reg.gpr());
                break;
#if CPU(X86_64)
            case OperandShape::PinnedMemory:
                jit.testOnFlags64(Address(plan.pinned.reg.gpr(), *plan.pinned.offset), left);
                break;
#endif
            default:
                isWide ? jit.testOnFlags64(left, right) : jit.testOnFlags32(left, right);
                break;
            }
        }
        return byZero(opcode == B3::NotEqual ? whenTrue : !whenTrue);
    }
    if (!isTestedAnew(condition, user) && (opcode == B3::Equal || opcode == B3::NotEqual) && isZero(condition.b) && !B3::isLoad(node(condition.a).opcode) && conditionIsFolded<pass>(condition.a, condition.operandType, user))
        return setFlagsFor<pass>(condition.a, condition.operandType, user, opcode == B3::NotEqual ? whenTrue : !whenTrue);
    bool isWide = condition.operandType == NodeType::Int64;
    OperandPlan plan = planComparison<pass>(condition, user, false);
    MacroAssembler::RelationalCondition relation = relationalConditionOf(opcode);
    if (plan.isCommuted)
        relation = MacroAssembler::commute(relation);
    if (!whenTrue)
        relation = MacroAssembler::invert(relation);
    GPRReg left = use<pass>(plan.left).gpr();
    GPRReg right = InvalidGPRReg;
    if (plan.shape == OperandShape::Register)
        right = use<pass>(plan.right).gpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        switch (plan.shape) {
        case OperandShape::Immediate: {
            TrustedImm32 immediate(static_cast<int32_t>(plan.immediate));
            isWide ? jit.compareOnFlags64(left, immediate) : jit.compareOnFlags32(left, immediate);
            break;
        }
        case OperandShape::PinnedRegister:
            jit.compareOnFlags64(left, plan.pinned.reg.gpr());
            break;
#if CPU(X86_64)
        case OperandShape::PinnedMemory:
            jit.compareOnFlags64(left, Address(plan.pinned.reg.gpr(), *plan.pinned.offset));
            break;
#endif
        default:
            isWide ? jit.compareOnFlags64(left, right) : jit.compareOnFlags32(left, right);
            break;
        }
    }
    return relation;
}

template<Pass pass>
void LinearScanCodeGenerator::lowerSelect(NodeID id)
{
    const Node& n = node(id);
    Reg hint = decode(n.hint);
    if (!n.isDouble() && canSetFlagsFor<pass>(n.a, n.operandType, n)) {
        GPRReg whenTrue = use<pass>(n.b).gpr();
        GPRReg whenFalse = use<pass>(n.c, OperandKind::Use, hint).gpr();
        MacroAssembler::RelationalCondition flags = setFlagsFor<pass>(n.a, n.operandType, n, true);
        GPRReg result = define<pass>(id, OperandKind::Definition, Reg(), n.c).gpr();
        if constexpr (pass == Pass::Emit) {
            m_jit->moveConditionallyOnFlags64(flags, whenTrue, whenFalse, result);
            finishUnit();
        }
        return;
    }
    Reg whenTrue = use<pass>(n.b);
    Reg whenFalse = use<pass>(n.c);
    Reg result = define<pass>(id, OperandKind::EarlyDefinition);
    if constexpr (pass == Pass::Emit)
        moveRegister(whenFalse, result);
    Jump skip = branchOn<pass>(n.a, n.operandType, n, false);
    if constexpr (pass == Pass::Emit) {
        finishBranchUnit();
        moveRegister(whenTrue, result);
        skip.link(m_jit);
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerDoubleBinary(NodeID id)
{
    chooseOperandOrder<pass>(id);
    const Node& n = node(id);
    B3::Opcode opcode = n.opcode;
    bool commutes = opcode == B3::Add || opcode == B3::Mul;
    Reg hint = decode(n.hint);
    NodeID leftID = n.a;
    NodeID rightID = n.b;
    if (commutes && isConstant(leftID) && !isConstant(rightID))
        std::swap(leftID, rightID);
    else if (n.has(IsCommuted))
        std::swap(leftID, rightID);

#if CPU(X86_64)
    std::optional<MemoryOperand> memory;
    if (isConstant(rightID)) {
        if (std::optional<B3::PinnedConstant> pinned = pinnedConstantFor(node(rightID).constant); pinned && pinned->offset) {
            memory = MemoryOperand();
            memory->base = pinned->reg.gpr();
            memory->offset = *pinned->offset;
        }
    } else if (node(rightID).opcode == B3::Load && node(rightID).isDouble() && loadIsFolded<pass>(rightID, n)) {
        fold<pass>(rightID);
        memory = operandForLoad<pass>(rightID, n, OperandKind::LateUse);
        if (memory->hasIndex() && opcode == B3::Div) {
            refuse();
            return;
        }
    }
    if (memory) {
        FPRReg left = use<pass>(leftID, OperandKind::Use, hint).fpr();
        FPRReg result = define<pass>(id, OperandKind::Definition, Reg(), leftID).fpr();
        if constexpr (pass == Pass::Emit) {
            CCallHelpers& jit = *m_jit;
            if (opcode == B3::Div) {
                jit.moveDouble(left, result);
                jit.divDouble(memory->address(), result);
            } else {
                withAddress(*memory, [&](auto address) {
                    switch (opcode) {
                    case B3::Add:
                        jit.addDouble(address, left, result);
                        break;
                    case B3::Sub:
                        jit.subDouble(left, address, result);
                        break;
                    default:
                        jit.mulDouble(address, left, result);
                        break;
                    }
                });
            }
            finishUnit();
        }
        return;
    }
#endif

    FPRReg left = use<pass>(leftID, OperandKind::Use, hint).fpr();
    ValueLocation rightLocation = useRegisterOrSlot<pass>(rightID, !commutes && isX86_64());
    FPRReg result = define<pass>(id, OperandKind::Definition, Reg(), leftID).fpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
#if CPU(X86_64)
        if (rightLocation.isSlot()) {
            int32_t offset = offsetOfSlotOf(rightID);
            Address address = addressOfSlot(offset);
            moveRegister(Reg(left), Reg(result));
            switch (opcode) {
            case B3::Add:
                jit.addDouble(address, result);
                break;
            case B3::Sub:
                jit.subDouble(address, result);
                break;
            case B3::Mul:
                jit.mulDouble(address, result);
                break;
            default:
                jit.divDouble(address, result);
                break;
            }
            finishUnit(kindOfFrameAccess(offset));
            return;
        }
#endif
        FPRReg right = rightLocation.reg().fpr();
        switch (opcode) {
        case B3::Add:
            jit.addDouble(left, right, result);
            break;
        case B3::Sub:
            jit.subDouble(left, right, result);
            break;
        case B3::Mul:
            jit.mulDouble(left, right, result);
            break;
        case B3::Div:
            jit.divDouble(left, right, result);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerDoubleExtremum(NodeID id)
{
    const Node& n = node(id);
    bool isMax = n.opcode == B3::FMax;
    FPRReg left = use<pass>(n.a).fpr();
    FPRReg right = use<pass>(n.b).fpr();
#if CPU(ARM64)
    FPRReg result = define<pass>(id).fpr();
    if constexpr (pass == Pass::Emit) {
        isMax ? m_jit->doubleMax(left, right, result) : m_jit->doubleMin(left, right, result);
        finishUnit();
    }
#else
    FPRReg result = define<pass>(id, OperandKind::EarlyDefinition).fpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        MacroAssembler::JumpList done;
        Jump notEqual = jit.branchDouble(MacroAssembler::DoubleNotEqualOrUnordered, left, right);
        isMax ? jit.andDouble(left, right, result) : jit.orDouble(left, right, result);
        done.append(jit.jump());
        notEqual.link(&jit);
        Jump notLess = jit.branchDouble(MacroAssembler::DoubleGreaterThanOrEqualOrUnordered, left, right);
        jit.moveDouble(isMax ? right : left, result);
        done.append(jit.jump());
        notLess.link(&jit);
        Jump isUnordered = jit.branchDouble(MacroAssembler::DoubleLessThanOrEqualOrUnordered, left, right);
        jit.moveDouble(isMax ? left : right, result);
        done.append(jit.jump());
        isUnordered.link(&jit);
        jit.addDouble(left, right, result);
        done.link(&jit);
        finishUnit();
    }
#endif
}

template<Pass pass>
void LinearScanCodeGenerator::lowerDivision(NodeID id)
{
#if CPU(X86_64)
    const Node& n = node(id);
    B3::Opcode opcode = n.opcode;
    bool isWide = n.isWide();
    bool isMultiplication = opcode == B3::UMulHigh || opcode == B3::MulHigh;
    bool isSigned = opcode == B3::Div || opcode == B3::Mod;
    bool takesHighHalf = opcode == B3::Mod || opcode == B3::UMod || isMultiplication;
    Reg low(X86Registers::eax);
    Reg high(X86Registers::edx);
    Reg resultRegister = takesHighHalf ? high : low;
    ValueLocation dividend = useAnywhere<pass>(n.a, OperandKind::ArgumentUse, low);
    GPRReg right = use<pass>(n.b, OperandKind::LateUse).gpr();
    define<pass>(id, OperandKind::Definition, resultRegister);
    if constexpr (pass == Pass::Select)
        addClobber(argumentPositionOf(n.slot) + 1, mainPositionOf(n.slot) + 2, bitOf(low) | bitOf(high));
    Vector<Transfer, 8> transfers;
    if constexpr (pass == Pass::Emit)
        transfers.append(Transfer { encode(low), dividend.location, n.a, n.a, 0, false });
    prepareArguments<pass>(transfers);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        MacroAssembler::JumpList done;
        if (n.has(IsChill)) {
            GPRReg scratch = jit.scratchRegister();
            MacroAssembler::JumpList isUsual;
            if (isWide) {
                jit.add64(TrustedImm32(1), right, scratch);
                isUsual.append(jit.branch64(MacroAssembler::Above, scratch, TrustedImm32(1)));
            } else {
                jit.add32(TrustedImm32(1), right, scratch);
                isUsual.append(jit.branch32(MacroAssembler::Above, scratch, TrustedImm32(1)));
            }
            Jump isNotZero = isWide ? jit.branchTest64(MacroAssembler::NonZero, right) : jit.branchTest32(MacroAssembler::NonZero, right);
            jit.move(TrustedImm32(0), resultRegister.gpr());
            done.append(jit.jump());
            isNotZero.link(&jit);
            if (isWide) {
                jit.move(TrustedImm64(std::numeric_limits<int64_t>::min()), scratch);
                isUsual.append(jit.branch64(MacroAssembler::NotEqual, low.gpr(), scratch));
            } else
                isUsual.append(jit.branch32(MacroAssembler::NotEqual, low.gpr(), TrustedImm32(std::numeric_limits<int32_t>::min())));
            if (opcode == B3::Mod)
                jit.move(TrustedImm32(0), resultRegister.gpr());
            else if (!isWide)
                jit.zeroExtend32ToWord(low.gpr(), low.gpr());
            done.append(jit.jump());
            isUsual.link(&jit);
        }
        if (isMultiplication) {
            if (opcode == B3::UMulHigh)
                isWide ? jit.x86UMulHigh64(right, low.gpr(), high.gpr()) : jit.x86UMulHigh32(right, low.gpr(), high.gpr());
            else
                isWide ? jit.x86MulHigh64(right, low.gpr(), high.gpr()) : jit.x86MulHigh32(right, low.gpr(), high.gpr());
        } else if (isSigned) {
            if (isWide) {
                jit.x86ConvertToQuadWord64();
                jit.x86Div64(right);
            } else {
                jit.x86ConvertToDoubleWord32();
                jit.x86Div32(right);
            }
        } else {
            jit.xor32(high.gpr(), high.gpr());
            isWide ? jit.x86UDiv64(right) : jit.x86UDiv32(right);
        }
        done.link(&jit);
        finishUnit();
    }
#else
    const Node& n = node(id);
    B3::Opcode opcode = n.opcode;
    bool isWide = n.isWide();
    bool isRemainder = opcode == B3::Mod || opcode == B3::UMod;
    GPRReg left = use<pass>(n.a).gpr();
    GPRReg right = use<pass>(n.b, isRemainder && n.has(IsChill) ? OperandKind::LateUse : OperandKind::Use).gpr();
    GPRReg result = define<pass>(id).gpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        GPRReg quotient = isRemainder ? jit.scratchRegister() : result;
        switch (opcode) {
        case B3::MulHigh:
            if (isWide)
                jit.mulHigh64(left, right, result);
            else {
                jit.mulHigh32(left, right, result);
                jit.zeroExtend32ToWord(result, result);
            }
            break;
        case B3::UMulHigh:
            isWide ? jit.uMulHigh64(left, right, result) : jit.uMulHigh32(left, right, result);
            break;
        case B3::Div:
        case B3::Mod:
            isWide ? jit.div64(left, right, quotient) : jit.div32(left, right, quotient);
            break;
        default:
            isWide ? jit.uDiv64(left, right, quotient) : jit.uDiv32(left, right, quotient);
            break;
        }
        if (isRemainder) {
            isWide ? jit.multiplySub64(quotient, right, left, result) : jit.multiplySub32(quotient, right, left, result);
            if (n.has(IsChill)) {
                Jump isNotZero = isWide ? jit.branchTest64(MacroAssembler::NonZero, right) : jit.branchTest32(MacroAssembler::NonZero, right);
                jit.move(TrustedImm32(0), result);
                isNotZero.link(&jit);
            }
        }
        finishUnit();
    }
#endif
}

template<Pass pass>
void LinearScanCodeGenerator::lowerUnary(NodeID id)
{
    const Node& n = node(id);
    bool sharesBank = n.isDouble() == (n.operandType == NodeType::Double);
    Reg source = use<pass>(n.a, OperandKind::Use, sharesBank ? decode(n.hint) : Reg());
    Reg result = define<pass>(id, OperandKind::Definition, Reg(), sharesBank ? n.a : 0);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        bool isWide = n.isWide();
        switch (n.opcode) {
        case B3::PopCount:
            isWide ? jit.countPopulation64(source.gpr(), result.gpr(), MacroAssembler::fpTempRegister) : jit.countPopulation32(source.gpr(), result.gpr(), MacroAssembler::fpTempRegister);
            break;
        case B3::Ceil:
            jit.ceilDouble(source.fpr(), result.fpr());
            break;
        case B3::Floor:
            jit.floorDouble(source.fpr(), result.fpr());
            break;
        case B3::FTrunc:
            jit.truncDouble(source.fpr(), result.fpr());
            break;
        case B3::Neg:
            if (n.isDouble()) {
#if CPU(X86_64)
                B3::PinnedConstant signBit = *pinnedConstantFor(std::numeric_limits<int64_t>::min());
                jit.loadDouble(Address(signBit.reg.gpr(), *signBit.offset), MacroAssembler::fpTempRegister);
                jit.xorDouble(source.fpr(), MacroAssembler::fpTempRegister, result.fpr());
#else
                jit.negateDouble(source.fpr(), result.fpr());
#endif
            } else
                isWide ? jit.neg64(source.gpr(), result.gpr()) : jit.neg32(source.gpr(), result.gpr());
            break;
        case B3::Clz:
            isWide ? jit.countLeadingZeros64(source.gpr(), result.gpr()) : jit.countLeadingZeros32(source.gpr(), result.gpr());
            break;
        case B3::Sqrt:
            jit.sqrtDouble(source.fpr(), result.fpr());
            break;
        case B3::Abs:
            jit.absDouble(source.fpr(), result.fpr());
            break;
        case B3::BitwiseCast:
            if (n.isDouble())
                jit.move64ToDouble(source.gpr(), result.fpr());
            else
                jit.moveDoubleTo64(source.fpr(), result.gpr());
            break;
        case B3::IToD:
            n.operandType == NodeType::Int64 ? jit.convertInt64ToDouble(source.gpr(), result.fpr()) : jit.convertInt32ToDouble(source.gpr(), result.fpr());
            break;
        case B3::SExt8:
            jit.signExtend8To32(source.gpr(), result.gpr());
            break;
        case B3::SExt16:
            jit.signExtend16To32(source.gpr(), result.gpr());
            break;
        case B3::SExt32:
            jit.signExtend32To64(source.gpr(), result.gpr());
            break;
        case B3::ZExt32:
            jit.zeroExtend32ToWord(source.gpr(), result.gpr());
            break;
        case B3::Trunc:
            jit.move(source.gpr(), result.gpr());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::noteStackmap(B3::StackmapValue* stackmap)
{
    if constexpr (pass == Pass::Select) {
        if (!stackmap->runsWithoutFrame() || stackmap->followsFrameSetup() || stackmap->needsFrameRecord())
            noteFrameUse(stackmap->needsFrameRecord());
    }
}

template<Pass pass>
B3::ValueRep LinearScanCodeGenerator::repForStackmapChild(NodeID child, NodeType type, const B3::ValueRep& constraint)
{
    const Node& n = node(child);
    switch (constraint.kind()) {
    case B3::ValueRep::SomeRegister:
        return B3::ValueRep::reg(use<pass>(child));
    case B3::ValueRep::SomeLateRegister:
        return B3::ValueRep::reg(use<pass>(child, OperandKind::LateUse));
    case B3::ValueRep::SomeRegisterWithClobber: {
        ValueLocation source = useAnywhere<pass>(child, OperandKind::AnyUse);
        Reg copy = temporary<pass>(n.isDouble());
        if constexpr (pass == Pass::Emit)
            m_copiesBeforeInstruction.append(Transfer { encode(copy), source.location, child, child, 0, false });
        return B3::ValueRep::reg(copy);
    }
    case B3::ValueRep::WarmAny:
    case B3::ValueRep::ColdAny:
    case B3::ValueRep::LateColdAny: {
        bool isLate = constraint.kind() == B3::ValueRep::LateColdAny;
        if (n.form == Form::Constant)
            return B3::ValueRep::constant(type == NodeType::Int32 ? static_cast<int64_t>(static_cast<int32_t>(n.constant)) : n.constant);
        if (n.form != Form::Plain)
            return B3::ValueRep::reg(use<pass>(child, isLate ? OperandKind::LateUse : OperandKind::Use));
        ValueLocation location = useAnywhere<pass>(child, isLate ? OperandKind::LateAnyUse : OperandKind::AnyUse);
        if constexpr (pass == Pass::Emit) {
            if (location.isSlot())
                return B3::ValueRep::stack(offsetOfSlotOf(child) + m_slotBias);
        }
        return B3::ValueRep::reg(location.reg());
    }
    default:
        refuse();
        fail();
        return B3::ValueRep::constant(0);
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerCheck(NodeID id)
{
    const Node& n = node(id);
    B3::CheckValue* check = b3ValueOf(id)->as<B3::CheckValue>();
    int64_t constant;
    bool isConstantCondition = constantOf(n.a, constant);
    if constexpr (pass == Pass::Select) {
        if (isConstantCondition && !constant) {
            node(id).form = Form::Dead;
            return;
        }
    }
    noteStackmap<pass>(check);
    WTF::Vector<B3::ValueRep> reps;
    for (unsigned index = 1; index < check->numChildren(); ++index) {
        B3::ConstrainedValue child = check->constrainedChild(index);
        if (child.rep().kind() == B3::ValueRep::Register || child.rep().kind() == B3::ValueRep::StackArgument || child.rep().kind() == B3::ValueRep::SomeRegisterWithClobber) {
            refuse();
            return;
        }
        reps.append(repForStackmapChild<pass>(resolved(child.value()), typeOfChild(child.value()), child.rep()));
    }
    if constexpr (pass == Pass::Select)
        addClobber(mainPositionOf(n.slot), mainPositionOf(n.slot) + 1, bitsOf(check->earlyClobbered()));
    Jump failure;
    if (!isConstantCondition)
        failure = branchOn<pass>(n.a, n.operandType, n, true);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        if (isConstantCondition)
            failure = jit.jump();
        finishBranchUnit();
        Box<MacroAssembler::JumpList> sharedFailures;
        if (check->sharesLatePath()) {
            auto& latePaths = m_context.sharedLatePathsByGenerator.add(check->generator().get(), Vector<B3::Air::GenerationContext::SharedLatePath, 1>()).iterator->value;
            for (auto& latePath : latePaths) {
                if (latePath.frameIsSetUp == m_context.frameIsSetUp && latePath.reps == reps) {
                    latePath.jumps->append(failure);
                    return;
                }
            }
            sharedFailures = Box<MacroAssembler::JumpList>::create();
            sharedFailures->append(failure);
            latePaths.append({ reps, m_context.frameIsSetUp, sharedFailures });
        }
        m_context.latePaths.append(std::tuple {
            check->origin(),
            createSharedTask<B3::Air::GenerationContext::LatePathFunction>([=](CCallHelpers& jit, B3::Air::GenerationContext& context) {
                if (sharedFailures)
                    sharedFailures->link(&jit);
                else
                    failure.link(&jit);
                check->generator()->run(jit, B3::StackmapGenerationParams(check, reps, context));
            })
        });
        noteNewLatePaths();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerCheckedArithmetic(NodeID id)
{
    const Node& n = node(id);
    B3::CheckValue* check = b3ValueOf(id)->as<B3::CheckValue>();
    bool isWide = n.isWide();
    noteStackmap<pass>(check);
    WTF::Vector<B3::ValueRep> reps;
    for (unsigned index = 2; index < check->numChildren(); ++index) {
        B3::ConstrainedValue child = check->constrainedChild(index);
        B3::ValueRep constraint = child.rep();
        switch (constraint.kind()) {
        case B3::ValueRep::SomeRegister:
            constraint = B3::ValueRep::SomeLateRegister;
            break;
        case B3::ValueRep::WarmAny:
        case B3::ValueRep::ColdAny:
            constraint = B3::ValueRep::LateColdAny;
            break;
        case B3::ValueRep::SomeLateRegister:
        case B3::ValueRep::LateColdAny:
            break;
        default:
            refuse();
            return;
        }
        reps.append(repForStackmapChild<pass>(resolved(child.value()), typeOfChild(child.value()), constraint));
    }
    GPRReg left = use<pass>(n.a, OperandKind::LateUse).gpr();
    int64_t constant = 0;
    bool hasImmediate = constantOf(n.b, constant) && !isWide && n.opcode != B3::CheckMul;
    GPRReg right = hasImmediate ? InvalidGPRReg : use<pass>(n.b, OperandKind::LateUse).gpr();
    GPRReg result = define<pass>(id).gpr();
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        Jump failure;
        switch (n.opcode) {
        case B3::CheckAdd:
            if (hasImmediate)
                failure = jit.branchAdd32(MacroAssembler::Overflow, left, TrustedImm32(static_cast<int32_t>(constant)), result);
            else
                failure = isWide ? jit.branchAdd64(MacroAssembler::Overflow, left, right, result) : jit.branchAdd32(MacroAssembler::Overflow, left, right, result);
            break;
        case B3::CheckSub:
            if (hasImmediate)
                failure = jit.branchSub32(MacroAssembler::Overflow, left, TrustedImm32(static_cast<int32_t>(constant)), result);
            else if (isWide) {
                jit.move(left, result);
                failure = jit.branchSub64(MacroAssembler::Overflow, right, result);
            } else
                failure = jit.branchSub32(MacroAssembler::Overflow, left, right, result);
            break;
        case B3::CheckMul:
            failure = isWide ? jit.branchMul64(MacroAssembler::Overflow, left, right, result) : jit.branchMul32(MacroAssembler::Overflow, left, right, result);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        finishUnit();
        m_context.latePaths.append(std::tuple {
            check->origin(),
            createSharedTask<B3::Air::GenerationContext::LatePathFunction>([=](CCallHelpers& jit, B3::Air::GenerationContext& context) {
                failure.link(&jit);
                check->generator()->run(jit, B3::StackmapGenerationParams(check, reps, context));
            })
        });
        noteNewLatePaths();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerPatchpoint(NodeID id)
{
    const Node& n = node(id);
    B3::PatchpointValue* patchpoint = b3ValueOf(id)->as<B3::PatchpointValue>();
    const Block& block = m_blocks[m_currentBlock];
    bool isTerminal = n.has(IsTerminal);
    Position argumentPosition = argumentPositionOf(n.slot);
    Position position = mainPositionOf(n.slot);
    uint64_t early = bitsOf(patchpoint->earlyClobbered());
    uint64_t late = bitsOf(patchpoint->lateClobbered());
    for (unsigned index = 0; index < patchpoint->numberOfSuccessorClobbers(); ++index)
        late |= bitsOf(patchpoint->lateClobberedOnSuccessor(index));
    unsigned numberOfChildren = patchpoint->numChildren();
    unsigned numberOfResults = m_procedure.resultCount(patchpoint->type());
    uint64_t allocatable = m_generalRegisters | m_doubleRegisters;
    bool keepsSomeRegister = allocatable & ~(early | late);

    if constexpr (pass == Pass::Select) {
        if (isTerminal) {
            if (patchpoint->runsWithoutFrame())
                ++m_numberOfTeardowns;
            if (block.numberOfSuccessors > 31) {
                refuse();
                return;
            }
        }
        noteStackmap<pass>(patchpoint);
        uint64_t fixedArguments = 0;
        uint64_t fixedResults = 0;
        for (unsigned index = 0; index < numberOfChildren; ++index) {
            B3::ValueRep constraint = patchpoint->constrainedChild(index).rep();
            if (constraint.kind() == B3::ValueRep::Register)
                fixedArguments |= bitOf(constraint.reg());
        }
        for (unsigned index = 0; index < numberOfResults; ++index) {
            if (patchpoint->resultConstraints[index].kind() == B3::ValueRep::Register)
                fixedResults |= bitOf(patchpoint->resultConstraints[index].reg());
        }
        addClobber(argumentPosition + 1, position + 1, fixedArguments);
        addClobber(position, position + 2, early);
        addClobber(position + 1, position + 2, late | fixedResults);
        m_clobberedRegisters |= fixedArguments;
        if ((early | late | fixedArguments | fixedResults) & m_calleeSaves)
            noteFrameUse(false);
        if (keepsSomeRegister)
            m_probes.append(position + 1);
    }

    B3::StackmapGenerationParams::Reps reps;
    reps.grow(numberOfResults + numberOfChildren);
    Vector<Transfer, 8> transfers;
    for (unsigned index = 0; index < numberOfChildren; ++index) {
        B3::ConstrainedValue child = patchpoint->constrainedChild(index);
        NodeID childID = resolved(child.value());
        switch (child.rep().kind()) {
        case B3::ValueRep::Register: {
            ValueLocation location = useAnywhere<pass>(childID, OperandKind::ArgumentUse, child.rep().reg());
            if constexpr (pass == Pass::Emit)
                transfers.append(Transfer { encode(child.rep().reg()), location.location, childID, childID, 0, false });
            reps[numberOfResults + index] = B3::ValueRep::reg(child.rep().reg());
            break;
        }
        case B3::ValueRep::StackArgument: {
            ValueLocation location = useAnywhere<pass>(childID, OperandKind::ArgumentUse);
            if constexpr (pass == Pass::Select) {
                m_bytesOfStackArguments = std::max<unsigned>(m_bytesOfStackArguments, child.rep().offsetFromSP() + sizeof(CPURegister));
                noteFrameUse(true);
            } else {
                int32_t offset = child.rep().offsetFromSP() - static_cast<int32_t>(m_code.frameSize());
                transfers.append(Transfer { locationStackArgument, location.location, childID, childID, offset, false });
                reps[numberOfResults + index] = B3::ValueRep::stack(offset);
            }
            break;
        }
        default:
            reps[numberOfResults + index] = repForStackmapChild<pass>(childID, typeOfChild(child.value()), child.rep());
            break;
        }
        if (m_isRefused)
            return;
    }

    Vector<GPRReg, 4> generalScratches;
    Vector<FPRReg, 4> doubleScratches;
    for (unsigned index = 0; index < patchpoint->numGPScratchRegisters; ++index)
        generalScratches.append(temporary<pass>(false).gpr());
    for (unsigned index = 0; index < patchpoint->numFPScratchRegisters; ++index)
        doubleScratches.append(temporary<pass>(true).fpr());

    for (unsigned index = 0; index < numberOfResults; ++index) {
        const B3::ValueRep& constraint = patchpoint->resultConstraints[index];
        NodeID result = id;
        if (n.type == NodeType::Tuple) {
            result = 0;
            for (NodeID extract = n.c; extract; extract = node(extract).next) {
                if (node(extract).constant == static_cast<int64_t>(index) && node(extract).form == Form::Plain)
                    result = extract;
            }
        } else if (!n.uses)
            result = 0;
        bool isDouble = m_procedure.typeAtOffset(patchpoint->type(), index) == B3::Double;
        if (constraint.kind() == B3::ValueRep::Register) {
            if (result)
                define<pass>(result, OperandKind::Definition, constraint.reg());
            reps[index] = B3::ValueRep::reg(constraint.reg());
            continue;
        }
        if (!result) {
            reps[index] = B3::ValueRep::reg(temporary<pass>(isDouble));
            continue;
        }
        reps[index] = B3::ValueRep::reg(define<pass>(result, constraint.kind() == B3::ValueRep::SomeEarlyRegister ? OperandKind::EarlyDefinition : OperandKind::Definition));
    }

    prepareArguments<pass>(transfers);

    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        if (!m_copiesBeforeInstruction.isEmpty()) {
            emitParallelMoves(m_copiesBeforeInstruction);
            m_copiesBeforeInstruction.shrink(0);
        }

        uint64_t used = m_usedCalleeSaves;
        if (keepsSomeRegister) {
            auto found = std::ranges::lower_bound(m_probes, position + 1);
            RELEASE_ASSERT(found != m_probes.end() && *found == position + 1);
            used |= m_registersLiveAtProbes[found - m_probes.begin()];
        }
        RegisterSet usedRegisters;
        for (uint64_t bits = used; bits; bits &= bits - 1)
            usedRegisters.add(Reg::fromIndex(std::countr_zero(bits)), IgnoreVectors);
        patchpoint->m_usedRegisters = usedRegisters;

        B3::StackmapGenerationParams params(patchpoint, WTF::move(reps), m_context);
        for (GPRReg scratch : generalScratches)
            params.m_gpScratch.append(scratch);
        for (FPRReg scratch : doubleScratches)
            params.m_fpScratch.append(scratch);
        WTF::Vector<Box<Label>> labels;
        if (isTerminal) {
            for (unsigned index = 0; index < block.numberOfSuccessors; ++index) {
                labels.append(sharedLabelOfEdge(m_currentBlock, index));
                if (!edgeHasMovesOnIt(m_currentBlock, index) && isNext(targetOf(block, index), m_currentBlock))
                    params.m_successorsFallenThroughTo |= 1u << index;
            }
            params.m_labelsOfSuccessors = &labels;
        }
        m_unitBegin = jit.labelIgnoringWatchpoints();
        patchpoint->generator()->run(jit, params);
        noteNewLatePaths();
        finishUnit(CodeUnit::Kind::Pinned);
        if (isTerminal)
            m_codeIsReachable = true;
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerUpsilon(NodeID id)
{
    Node& n = node(id);
    Node& phi = node(n.b);
    if constexpr (pass == Pass::Select) {
        bool phiComesLater = phi.slot > n.slot;
        if (phi.form == Form::Alias || phi.form == Form::Dead || (phiComesLater && !phi.uses)) {
            n.form = Form::Dead;
            return;
        }
        if (!phiComesLater && phi.uses != UINT16_MAX)
            ++phi.uses;
    }
    if (phi.has(ArrivesInSlot)) {
        Reg source = use<pass>(n.a);
        if constexpr (pass == Pass::Select)
            noteFrameUse(false);
        else
            storeToSlot(source, phi, static_cast<int32_t>(phi.constant));
        return;
    }
    useAnywhere<pass>(n.a, OperandKind::EndUse, decode(phi.hint));
    if constexpr (pass == Pass::Select) {
        const Node& source = node(n.a);
        if (source.form != Form::Plain)
            return;
        uint32_t phiRegister = ensureVirtualRegister(n.b);
        bool phiComesLater = phi.slot > n.slot;
        VirtualRegister& phiVirtual = m_virtualRegisters[phiRegister];
        VirtualRegister& sourceVirtual = m_virtualRegisters[source.vreg];
        if (phiComesLater) {
            double frequency = m_blocks[m_currentBlock].frequency;
            if (!phiVirtual.hintVirtualRegister || frequency > phiVirtual.hintFrequency) {
                phiVirtual.hintVirtualRegister = source.vreg;
                phiVirtual.hintFrequency = static_cast<float>(frequency);
            }
        }
        if (!sourceVirtual.hintVirtualRegister)
            sourceVirtual.hintVirtualRegister = phiRegister;
        sourceVirtual.targetVirtualRegister = phiRegister;
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerPhi(NodeID id)
{
    const Node& n = node(id);
    if (n.has(ArrivesInSlot)) {
        Reg result = define<pass>(id);
        if constexpr (pass == Pass::Select)
            noteFrameUse(false);
        else
            loadFromSlot(result, n, static_cast<int32_t>(n.constant));
        return;
    }
    define<pass>(id, OperandKind::BlockDefinition);
}

template<Pass pass>
void LinearScanCodeGenerator::lowerJump(NodeID)
{
    if constexpr (pass == Pass::Emit)
        jumpToEdge(m_currentBlock, 0);
}

template<Pass pass>
void LinearScanCodeGenerator::lowerBranch(NodeID id)
{
    const Node& n = node(id);
    Block& block = m_blocks[m_currentBlock];
    if (block.onlySuccessor) {
        if constexpr (pass == Pass::Emit)
            jumpToEdge(m_currentBlock, block.onlySuccessor - 1);
        return;
    }
    if constexpr (pass == Pass::Select)
        block.branchesToOneBlock = successorOf(block, 0) == successorOf(block, 1);
    if (block.branchesToOneBlock) {
        if constexpr (pass == Pass::Emit)
            jumpToEdge(m_currentBlock, 0);
        return;
    }

    bool fallsIntoTaken = false;
    if constexpr (pass == Pass::Emit) {
        bool takenHasMoves = edgeHasMovesOnIt(m_currentBlock, 0);
        bool notTakenHasMoves = edgeHasMovesOnIt(m_currentBlock, 1);
        if (takenHasMoves != notTakenHasMoves) {
            unsigned edgeWithMoves = takenHasMoves ? 0 : 1;
            unsigned edgeWrittenHere = m_blocks[targetOf(block, edgeWithMoves)].isRare ? 1 - edgeWithMoves : edgeWithMoves;
            fallsIntoTaken = !edgeWrittenHere;
        } else
            fallsIntoTaken = isNext(targetOf(block, 0), m_currentBlock);
    }

    if (isLogicalCondition<pass>(n.a, n)) {
        const Node& condition = node(n.a);
        bool isOr = condition.opcode == B3::BitOr;
        fold<pass>(n.a);
        fold<pass>(condition.a);
        fold<pass>(condition.b);
        Jump first = branchOnFolded<pass>(condition.a, n, isOr);
        if constexpr (pass == Pass::Emit) {
            linkEdge(first, m_currentBlock, isOr ? 0 : 1);
            finishBranchUnit();
        }
        Jump second = branchOnFolded<pass>(condition.b, n, !fallsIntoTaken);
        if constexpr (pass == Pass::Emit) {
            linkEdge(second, m_currentBlock, fallsIntoTaken ? 1 : 0);
            finishBranchUnit();
            jumpToEdge(m_currentBlock, fallsIntoTaken ? 0 : 1);
        }
        return;
    }

    Jump jump = branchOn<pass>(n.a, n.operandType, n, !fallsIntoTaken);
    if constexpr (pass == Pass::Emit) {
        linkEdge(jump, m_currentBlock, fallsIntoTaken ? 1 : 0);
        finishBranchUnit();
        jumpToEdge(m_currentBlock, fallsIntoTaken ? 0 : 1);
    }
}

bool LinearScanCodeGenerator::planBitTests(std::span<const CaseOfSwitch> cases, unsigned start, unsigned end, Vector<BitTestOfSwitch, 4>& tests) const
{
    if constexpr (!isX86_64())
        return false;
    constexpr unsigned minCases = 4;
    constexpr unsigned maxTargets = 3;
    unsigned size = end - start;
    if (!m_procedure.policy().switchLowering.usesBitTests || size < minCases)
        return false;
    int64_t first = cases[start].value;
    uint64_t limit = static_cast<uint64_t>(cases[end - 1].value) - static_cast<uint64_t>(first);
    if (limit >= 64 || !fitsInt32(first) || first == std::numeric_limits<int32_t>::min())
        return false;
    const Block& block = m_blocks[m_currentBlock];
    for (unsigned index = start; index < end; ++index) {
        uint32_t target = successorOf(block, cases[index].successor);
        size_t position = tests.findIf([&](const BitTestOfSwitch& test) { return successorOf(block, test.successor) == target; });
        if (position == notFound) {
            if (tests.size() == maxTargets)
                return false;
            position = tests.size();
            tests.append(BitTestOfSwitch { cases[index].successor, 0 });
        }
        tests[position].mask |= static_cast<uint64_t>(1) << (cases[index].value - first);
    }
    AOT::stableSort(tests, std::ranges::greater { }, [](const BitTestOfSwitch& test) {
        return std::popcount(test.mask);
    });
    unsigned costOfTree = 4;
    for (unsigned remaining = size; remaining > 3; remaining = (remaining + 1) / 2)
        costOfTree += 2;
    unsigned costOfTests = 2 + !!first + 3 * tests.size();
    return costOfTests <= costOfTree + 2;
}

bool LinearScanCodeGenerator::usesJumpTable(std::span<const CaseOfSwitch> cases, unsigned start, unsigned end) const
{
    constexpr uint64_t densityLimit = 4;
    unsigned size = end - start;
    if (size < m_procedure.policy().switchLowering.minCasesForJumpTable || !m_procedure.policy().addressing.isPositionIndependent)
        return false;
    int64_t first = cases[start].value;
    uint64_t span = static_cast<uint64_t>(cases[end - 1].value) - static_cast<uint64_t>(first);
    return fitsInt32(first) && first != std::numeric_limits<int32_t>::min() && span < (1u << 16) && (span + 1) / size < densityLimit;
}

bool LinearScanCodeGenerator::switchNeedsTemporaries(std::span<const CaseOfSwitch> cases, unsigned start, unsigned end) const
{
    constexpr unsigned leafThreshold = 3;
    Vector<BitTestOfSwitch, 4> tests;
    if (planBitTests(cases, start, end, tests) || usesJumpTable(cases, start, end))
        return true;
    if (end - start <= leafThreshold)
        return false;
    unsigned median = std::midpoint(start, end);
    return switchNeedsTemporaries(cases, start, median) || switchNeedsTemporaries(cases, median, end);
}

void LinearScanCodeGenerator::emitCasesOfSwitch(std::span<const CaseOfSwitch> cases, unsigned start, bool hasHardStart, unsigned end, bool isLast)
{
    constexpr unsigned leafThreshold = 3;
    CCallHelpers& jit = *m_jit;
    GPRReg subject = m_switchSubject;
    bool isWide = m_switchIsWide;
    unsigned otherwise = m_blocks[m_currentBlock].numberOfSuccessors - 1;
    auto goTo = [&](unsigned successor) {
        if (isLast) {
            jumpToEdge(m_currentBlock, successor);
            return;
        }
        linkEdge(jit.jump(), m_currentBlock, successor);
        finishUnit();
    };
    auto compare = [&](MacroAssembler::RelationalCondition condition, GPRReg reg, int64_t constant) {
        if (!isWide)
            return jit.branch32(condition, reg, TrustedImm32(static_cast<int32_t>(constant)));
        if (fitsInt32(constant))
            return jit.branch64(condition, reg, TrustedImm32(static_cast<int32_t>(constant)));
        return jit.branch64(condition, reg, TrustedImm64(constant));
    };
    auto subtractFirst = [&](int64_t first) {
        TrustedImm32 negated(-static_cast<int32_t>(first));
        isWide ? jit.add64(negated, subject, m_switchIndex) : jit.add32(negated, subject, m_switchIndex);
    };

    Vector<BitTestOfSwitch, 4> tests;
    if (planBitTests(cases, start, end, tests)) {
#if CPU(X86_64)
        int64_t first = cases[start].value;
        GPRReg index = subject;
        if (first) {
            subtractFirst(first);
            index = m_switchIndex;
        }
        linkEdge(compare(MacroAssembler::Above, index, cases[end - 1].value - first), m_currentBlock, otherwise);
        finishUnit();
        for (const BitTestOfSwitch& test : tests) {
            jit.move(TrustedImm64(static_cast<int64_t>(test.mask)), m_switchTable);
            linkEdge(jit.branchTestBit64(MacroAssembler::NonZero, m_switchTable, index), m_currentBlock, test.successor);
            finishUnit();
        }
        goTo(otherwise);
#endif
        return;
    }

    if (usesJumpTable(cases, start, end)) {
        int64_t first = cases[start].value;
        uint64_t span = static_cast<uint64_t>(cases[end - 1].value) - static_cast<uint64_t>(first);
        subtractFirst(first);
        linkEdge(compare(MacroAssembler::Above, m_switchIndex, static_cast<int64_t>(span)), m_currentBlock, otherwise);
        finishUnit();
        unsigned jumpTable = m_procedure.addJumpTable(static_cast<unsigned>(span + 1));
        m_records->relocations.moveAddress({ SymbolSpace::JumpTable, jumpTable }, m_switchTable, PCRelativeReach::Far);
        finishUnit(CodeUnit::Kind::Pinned);
        jit.load32SignedExtendTo64(MacroAssembler::BaseIndex(m_switchTable, m_switchIndex, MacroAssembler::TimesFour), m_switchIndex);
        jit.add64(m_switchTable, m_switchIndex);
        jit.farJump(m_switchIndex, NoPtrTag);
        finishUnit(CodeUnit::Kind::Pinned);
        m_codeIsReachable = false;
        Vector<std::pair<uint32_t, Box<Label>>> entries;
        for (unsigned index = start; index < end; ++index)
            entries.append({ static_cast<uint32_t>(cases[index].value - first), sharedLabelOfEdge(m_currentBlock, cases[index].successor) });
        Box<Label> labelOfOtherwise = sharedLabelOfEdge(m_currentBlock, otherwise);
        B3::Procedure::JumpTableRecord* record = &m_procedure.jumpTable(jumpTable);
        jit.addLinkTask([record, entries = WTF::move(entries), labelOfOtherwise](LinkBuffer& linkBuffer) {
            record->targetOffsets.fill(linkBuffer.offsetOf(*labelOfOtherwise));
            for (const auto& [entry, label] : entries)
                record->targetOffsets[entry] = linkBuffer.offsetOf(*label);
        });
        return;
    }

    unsigned size = end - start;
    if (size <= leafThreshold) {
        bool followsPrevious = hasHardStart || (start && cases[start - 1].value == cases[start].value - 1);
        bool areAllConsecutive = size && followsPrevious && end < cases.size() && cases[end - 1].value == cases[end].value - 1;
        for (unsigned index = start; index + 1 < end && areAllConsecutive; ++index)
            areAllConsecutive = cases[index].value + 1 == cases[index + 1].value;
        unsigned limit = areAllConsecutive ? size - 1 : size;
        if (isLast && limit && !areAllConsecutive && isNext(targetOf(m_blocks[m_currentBlock], cases[start + limit - 1].successor), m_currentBlock) && !edgeHasMovesOnIt(m_currentBlock, cases[start + limit - 1].successor)) {
            for (unsigned index = 0; index + 1 < limit; ++index) {
                linkEdge(compare(MacroAssembler::Equal, subject, cases[start + index].value), m_currentBlock, cases[start + index].successor);
                finishUnit();
            }
            linkEdge(compare(MacroAssembler::NotEqual, subject, cases[start + limit - 1].value), m_currentBlock, otherwise);
            finishUnit();
            jumpToEdge(m_currentBlock, cases[start + limit - 1].successor);
            return;
        }
        for (unsigned index = 0; index < limit; ++index) {
            linkEdge(compare(MacroAssembler::Equal, subject, cases[start + index].value), m_currentBlock, cases[start + index].successor);
            finishUnit();
        }
        goTo(areAllConsecutive ? cases[end - 1].successor : otherwise);
        return;
    }

    unsigned median = std::midpoint(start, end);
    Jump toLowerHalf = compare(MacroAssembler::LessThan, subject, cases[median].value);
    finishUnit();
    emitCasesOfSwitch(cases, median, true, end, false);
    toLowerHalf.link(&jit);
    emitCasesOfSwitch(cases, start, hasHardStart, median, isLast);
}

template<Pass pass>
void LinearScanCodeGenerator::lowerSwitch(NodeID id)
{
    const Node& n = node(id);
    Vector<CaseOfSwitch, 16> cases;
    if (n.constant) {
        const InferredSwitch& inferred = m_inferredSwitches[n.constant - 1];
        cases.append(m_casesOfSwitches.subspan(inferred.firstCase, inferred.numberOfCases));
    } else {
        B3::SwitchValue* value = b3ValueOf(id)->as<B3::SwitchValue>();
        for (unsigned index = 0; index < value->numCaseValues(); ++index)
            cases.append(CaseOfSwitch { value->caseValue(index), index });
    }
    AOT::sort(cases, { }, &CaseOfSwitch::value);
    GPRReg subject = use<pass>(n.a).gpr();
    GPRReg index = InvalidGPRReg;
    GPRReg table = InvalidGPRReg;
    if (switchNeedsTemporaries(cases.span(), 0, cases.size())) {
        index = temporary<pass>(false).gpr();
        table = temporary<pass>(false).gpr();
    }
    if constexpr (pass == Pass::Emit) {
        m_switchSubject = subject;
        m_switchIndex = index;
        m_switchTable = table;
        m_switchIsWide = n.operandType == NodeType::Int64;
        emitCasesOfSwitch(cases.span(), 0, false, cases.size(), true);
    }
}

MacroAssembler::Jump LinearScanCodeGenerator::branchWhereLeaving(const LeavingTest& test, GPRReg subject, int64_t constant)
{
    CCallHelpers& jit = *m_jit;
    bool isWide = test.type == NodeType::Int64;
    TrustedImm32 immediate(static_cast<int32_t>(constant));
    if (test.relation == B3::BitAnd)
        return isWide ? jit.branchTest64(MacroAssembler::NonZero, subject, immediate) : jit.branchTest32(MacroAssembler::NonZero, subject, immediate);
    MacroAssembler::RelationalCondition condition = relationalConditionOf(test.relation);
    if (!test.leavesWhenItHolds)
        condition = MacroAssembler::invert(condition);
    return isWide ? jit.branch64(condition, subject, immediate) : jit.branch32(condition, subject, immediate);
}

template<Pass pass>
void LinearScanCodeGenerator::lowerLeavingTests(NodeID id)
{
    const Node& n = node(id);
    GPRReg subject = use<pass>(n.a).gpr();
    if constexpr (pass == Pass::Emit) {
        const InferredSwitch& inferred = m_inferredSwitches[n.constant - 1];
        std::span<const LeavingTest> tests = m_leavingTests.subspan(inferred.firstCase, inferred.numberOfCases);
        const LeavingTest& last = tests.back();
        int64_t constant = last.constant;
        if (last.relation == B3::BitAnd) {
            for (const LeavingTest& test : tests)
                constant |= test.constant;
        }
        m_dispatchesOfLeavingTests.append(DispatchOfLeavingTests { branchWhereLeaving(last, subject, constant), m_currentBlock, id, subject });
        finishUnit();
        jumpToEdge(m_currentBlock, m_blocks[m_currentBlock].numberOfSuccessors - 1);
    }
}

void LinearScanCodeGenerator::emitDispatchesOfLeavingTests()
{
    for (const DispatchOfLeavingTests& dispatch : m_dispatchesOfLeavingTests) {
        const Block& block = m_blocks[dispatch.block];
        BlockColdness coldness = m_coldness->of(block.frequency / 16, !block.isReachedNormally);
        m_records->units.willGenerateBlock(coldness.isCold, coldness.isRarelyRun);
        m_unitBegin = m_jit->labelIgnoringWatchpoints();
        m_context.setFrameIsSetUp(*m_jit, m_hasFrame && block.runsInFrame);
        dispatch.entrance.link(m_jit);
        recordOrigin(dispatch.terminal);
        const InferredSwitch& inferred = m_inferredSwitches[node(dispatch.terminal).constant - 1];
        std::span<const LeavingTest> tests = m_leavingTests.subspan(inferred.firstCase, inferred.numberOfCases);
        for (size_t index = 0; index + 1 < tests.size(); ++index) {
            linkEdge(branchWhereLeaving(tests[index], dispatch.subject, tests[index].constant), dispatch.block, tests[index].successor);
            finishUnit();
        }
        linkEdge(m_jit->jump(), dispatch.block, tests.back().successor);
        finishUnit();
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lowerReturn(NodeID id)
{
    const Node& n = node(id);
    Vector<Transfer, 8> transfers;
    if constexpr (pass == Pass::Select)
        ++m_numberOfTeardowns;
    if (n.a) {
        Reg destination = n.operandType == NodeType::Double ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR);
        ValueLocation location = useAnywhere<pass>(n.a, OperandKind::ArgumentUse, destination);
        if constexpr (pass == Pass::Emit)
            transfers.append(Transfer { encode(destination), location.location, n.a, n.a, 0, false });
    }
    prepareArguments<pass>(transfers);
    if constexpr (pass == Pass::Emit) {
        CCallHelpers& jit = *m_jit;
        if (m_context.frameIsSetUp) {
            // Compact code leaves the frame in one place where a jump there is shorter than to leave it here.
            constexpr size_t reachOfShortJump = 120;
            constexpr size_t bytesOfShortJump = isX86_64() ? 2 : 4;
            constexpr size_t bytesOfLongJump = isX86_64() ? 5 : 4;
            if (m_bytesOfSharedTeardown > (bytesSoFar() - m_offsetOfSharedTeardown <= reachOfShortJump ? bytesOfShortJump : bytesOfLongJump)) {
                jit.jump().linkTo(m_sharedTeardown, &jit);
                m_codeIsReachable = false;
                finishUnit(CodeUnit::Kind::Pinned);
                return;
            }
            size_t offset = bytesSoFar();
            Label teardown = jit.label();
            m_context.setFrameIsSetUp(jit, false);
            B3::Air::emitFrameTeardown(jit, m_code, B3::Air::registerDeadAtReturns);
            finishUnit(CodeUnit::Kind::Pinned);
            if (m_prefersSmallCode) {
                m_sharedTeardown = teardown;
                m_offsetOfSharedTeardown = offset;
                m_bytesOfSharedTeardown = bytesSoFar() - offset + 1;
            }
        }
        jit.ret();
        m_codeIsReachable = false;
        finishUnit(CodeUnit::Kind::End);
    }
}

template<Pass pass>
void LinearScanCodeGenerator::lower(NodeID id)
{
    Node& n = node(id);
    B3::Opcode opcode = n.opcode;
    if constexpr (pass == Pass::Select) {
        bool mustBeGenerated = false;
        switch (opcode) {
        case B3::Set:
        case B3::Store8:
        case B3::Store16:
        case B3::Store:
        case B3::Check:
        case B3::CheckAdd:
        case B3::CheckSub:
        case B3::CheckMul:
        case B3::Jump:
        case B3::Branch:
        case B3::Switch:
        case B3::EntrySwitch:
        case B3::Return:
        case B3::Oops:
        case B3::Upsilon:
        case B3::Fence:
            mustBeGenerated = true;
            break;
        case B3::Patchpoint:
            mustBeGenerated = b3ValueOf(id)->as<B3::PatchpointValue>()->effects.mustExecute();
            for (NodeID extract = n.type == NodeType::Tuple ? n.c : 0; extract; extract = node(extract).next)
                mustBeGenerated |= node(extract).form == Form::Plain;
            break;
        default:
            mustBeGenerated = n.has(Traps) || n.has(TakesTrap);
            break;
        }
        if (!mustBeGenerated && !n.uses) {
            n.form = Form::Dead;
            return;
        }
        n.firstOperand = m_operands.size();
        n.numberOfOperands = 0;
    } else
        m_operandCursor = n.firstOperand;
    m_currentNode = id;

    switch (opcode) {
    case B3::ArgumentReg:
        define<pass>(id, OperandKind::BlockDefinition, decode(n.base));
        return;
    case B3::Extract:
        return;
    case B3::Set: {
        Reg source = use<pass>(n.a);
        if constexpr (pass == Pass::Select)
            noteFrameUse(true);
        else {
            const B3::Variable* variable = b3ValueOf(id)->as<B3::VariableValue>()->variable();
            Node width { };
            width.type = variable->type() == B3::Double ? NodeType::Double : variable->type() == B3::Int64 ? NodeType::Int64 : NodeType::Int32;
            storeToSlot(source, width, m_variableOffsets[variable->index()]);
        }
        return;
    }
    case B3::Get: {
        Reg result = define<pass>(id);
        if constexpr (pass == Pass::Select)
            noteFrameUse(true);
        else
            loadFromSlot(result, n, m_variableOffsets[b3ValueOf(id)->as<B3::VariableValue>()->variable()->index()]);
        return;
    }
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
    case B3::Div:
        if (n.isDouble())
            lowerDoubleBinary<pass>(id);
        else if (opcode == B3::Div)
            lowerDivision<pass>(id);
        else
            lowerIntegerBinary<pass>(id);
        return;
    case B3::Mod:
    case B3::UDiv:
    case B3::UMod:
    case B3::UMulHigh:
    case B3::MulHigh:
        lowerDivision<pass>(id);
        return;
    case B3::FMax:
    case B3::FMin:
        lowerDoubleExtremum<pass>(id);
        return;
    case B3::BitAnd:
    case B3::BitOr:
    case B3::BitXor:
        lowerIntegerBinary<pass>(id);
        return;
    case B3::Shl:
    case B3::SShr:
    case B3::ZShr:
        lowerShift<pass>(id);
        return;
    case B3::PopCount:
    case B3::Ceil:
    case B3::Floor:
    case B3::FTrunc:
    case B3::Neg:
    case B3::Clz:
    case B3::Sqrt:
    case B3::Abs:
    case B3::BitwiseCast:
    case B3::IToD:
    case B3::SExt8:
    case B3::SExt16:
    case B3::SExt32:
    case B3::ZExt32:
    case B3::Trunc:
        lowerUnary<pass>(id);
        return;
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Above:
    case B3::Below:
    case B3::AboveEqual:
    case B3::BelowEqual:
    case B3::EqualOrUnordered:
        lowerComparison<pass>(id);
        return;
    case B3::Select:
        lowerSelect<pass>(id);
        return;
    case B3::Load8Z:
    case B3::Load8S:
    case B3::Load16Z:
    case B3::Load16S:
    case B3::Load:
        lowerLoad<pass>(id);
        return;
    case B3::Store8:
    case B3::Store16:
    case B3::Store:
        lowerStore<pass>(id);
        return;
    case B3::Fence:
        if constexpr (pass == Pass::Emit) {
            const B3::FenceValue* fence = b3ValueOf(id)->as<B3::FenceValue>();
            if (!fence->write && !fence->read)
                return;
            if (!fence->write)
                m_jit->storeFence();
            else if (!fence->read)
                m_jit->loadFence();
            else
                m_jit->memoryFence();
            finishUnit(CodeUnit::Kind::Pinned);
        }
        return;
    case B3::Check:
        lowerCheck<pass>(id);
        return;
    case B3::CheckAdd:
    case B3::CheckSub:
    case B3::CheckMul:
        lowerCheckedArithmetic<pass>(id);
        return;
    case B3::Upsilon:
        lowerUpsilon<pass>(id);
        return;
    case B3::Phi:
        lowerPhi<pass>(id);
        return;
    case B3::Patchpoint:
        lowerPatchpoint<pass>(id);
        return;
    case B3::Jump:
        lowerJump<pass>(id);
        return;
    case B3::Branch:
        lowerBranch<pass>(id);
        return;
    case B3::EntrySwitch:
        lowerLeavingTests<pass>(id);
        return;
    case B3::Switch:
        lowerSwitch<pass>(id);
        return;
    case B3::Return:
        lowerReturn<pass>(id);
        return;
    case B3::Oops:
        if constexpr (pass == Pass::Emit) {
            m_jit->breakpoint();
            m_codeIsReachable = false;
            finishUnit(CodeUnit::Kind::End);
        }
        return;
    default:
        refuse();
        fail();
        return;
    }
}

void LinearScanCodeGenerator::select()
{
    for (uint32_t order = m_blocks.size(); order--;) {
        Block& block = m_blocks[order];
        if (!block.isReachable)
            continue;
        m_currentBlock = order;
        block.firstExposedUse = m_exposedUses.size();
        for (uint32_t slot = block.endSlot; slot-- > block.firstSlot;) {
            NodeID id = m_sequence[slot];
            if (node(id).form != Form::Plain)
                continue;
            lower<Pass::Select>(id);
            if (m_isRefused)
                return;
        }
        block.numberOfExposedUses = m_exposedUses.size() - block.firstExposedUse;
    }
    for (uint32_t vreg = 1; vreg < m_virtualRegisters.size(); ++vreg)
        m_virtualRegisters[vreg].hint = node(m_virtualRegisters[vreg].node).hint;
}

void LinearScanCodeGenerator::emitBlock(Block& block)
{
    CCallHelpers& jit = *m_jit;
    uint32_t order = static_cast<uint32_t>(&block - m_blocks.span().data());
    block.jumps.link(&jit);
    block.label = jit.label();
    if (block.sharedLabel)
        *block.sharedLabel = block.label;
    m_context.setFrameIsSetUp(jit, m_hasFrame && block.runsInFrame && !block.setsUpFrame);
    BlockColdness coldness = m_coldness->of(block.frequency, !block.isReachedNormally);
    m_records->units.willGenerateBlock(coldness.isCold, coldness.isRarelyRun);
    m_unitBegin = jit.labelIgnoringWatchpoints();
    m_currentBlock = order;
    m_codeIsReachable = true;
    m_moveCursor = block.firstMove;
    if (block.setsUpFrame) {
        emitFrameSetup();
        if (order && block.numberOfPredecessors == 1 && edgeMovesLieAtStartOfTarget(block.incomingEdge))
            emitEdgeMoves(block.incomingEdge);
        emitMoves(block.firstMoveAtStart, block.numberOfMovesAtStart);
        block.jumpsFromInsideFrame.link(&jit);
        block.labelInsideFrame = jit.label();
        m_unitBegin = jit.labelIgnoringWatchpoints();
    } else {
        if (order && block.numberOfPredecessors == 1 && edgeMovesLieAtStartOfTarget(block.incomingEdge))
            emitEdgeMoves(block.incomingEdge);
        emitMoves(block.firstMoveAtStart, block.numberOfMovesAtStart);
    }

    for (uint32_t slot = block.firstSlot; slot < block.endSlot && !m_hasFailed; ++slot) {
        NodeID id = m_sequence[slot];
        const Node& n = node(id);
        emitMovesAt(argumentPositionOf(slot));
        if (n.form != Form::Plain) {
            emitMovesAt(mainPositionOf(slot));
            continue;
        }
        if (!n.has(HasArgumentUses))
            emitMovesAt(mainPositionOf(slot));
        recordOrigin(id);
        lower<Pass::Emit>(id);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT) && (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
