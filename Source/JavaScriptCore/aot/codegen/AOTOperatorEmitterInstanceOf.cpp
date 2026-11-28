/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

bool OperatorEmitter::leaveOperandToInstanceOf(const ValueContext& read, InstanceOfOperand operand, NumberedImmediate number)
{
    std::span<const ValueID> behind = read.operationsBehind();
    while (!behind.empty() && operationAt(behind.front()).traits().isPure())
        behind = behind.subspan(1);
    if (behind.empty() || behind.front() != m_code.onlyUserOf(read.id()))
        return false;
    const Operation& test = operationAt(behind.front());
    bool walksChain = operand == InstanceOfOperand::PrototypeThatIsBuiltin;
    if (test.opcode != (walksChain ? Opcode::HasInPrototypeChain : Opcode::InstanceOf) || (walksChain ? test.prototypeOperand() : test.constructorOperand()) != read.id())
        return false;
    if (!test.traits().mayCallUnknownCode())
        return false;
    m_operandLeftToInstanceOf = OperandLeftToInstanceOf { read.id(), operand, number };
    return true;
}

void OperatorEmitter::generateInstanceOf(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    if (std::optional left = std::exchange(m_operandLeftToInstanceOf, std::nullopt)) {
        RELEASE_ASSERT(left->read == value->constructorOperand());
        defineTest(value, Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(left->operand), { object.value() }, { .numberedImmediate = left->number }).first)));
        return;
    }
    Boxed constructor = in<Boxed>(value->constructorOperand());
    defineTest(value, Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::Constructor), { object.value(), constructor.value() }).first)));
}

bool OperatorEmitter::walksChainInLine(const ValueContext& value) const
{
    return m_generator.isInLoop(value.block()) && !standsInRareBlock(value);
}

void OperatorEmitter::branchOnChain(ValueID object, NOESCAPE const Invocable<void(Boxed, BranchTargets)> auto& branchOnPrototype, LBasicBlock slowPath, BranchTargets targets)
{
    Boxed first = in<Boxed>(object);
    if (typeOf(object).primitives) {
        LBasicBlock isCellBlock = newBlock();
        branch(isCell(first), { normalEdge(isCellBlock), targets.notTaken });
        m_out.appendTo(isCellBlock);
        LBasicBlock isObjectBlock = newBlock();
        branch(isObjectCell(unboxCell(first)), { normalEdge(isObjectBlock), targets.notTaken });
        m_out.appendTo(isObjectBlock);
    }
    LBasicBlock loop = newBlock();
    ValueFromBlock start = m_out.anchor(first.value());
    m_out.jump(loop);
    m_out.appendTo(loop);
    Boxed current(m_out.phi(Machine::Int64, start));
    CellPointer cell = unboxCell(current);
    branchUnless(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), OverridesGetPrototype), slowPath);
    Boxed next = loadField<Boxed>(loadStructure(cell), m_heaps.Structure_prototype);
    LBasicBlock isOther = newBlock();
    branchOnPrototype(next, { targets.taken, normalEdge(isOther) });
    m_out.appendTo(isOther);
    LBasicBlock isNotLast = newBlock();
    branch(equal(next, Boxed(loadImmutablePointer(AOT_INSTANCE_OFFSET(realm.objectPrototype)).value())), { targets.notTaken, normalEdge(isNotLast) });
    m_out.appendTo(isNotLast);
    branchUnless(isNotEmpty(next), slowPath);
    branchUnless(isCell(next), slowPath);
    m_out.addIncomingToPhi(current.value(), m_out.anchor(next.value()));
    m_out.jump(loop);
}

void OperatorEmitter::branchOnPrototypeAbove(ValueID object, CellPointer prototype, LBasicBlock slowPath, BranchTargets targets)
{
    Boxed wanted = boxCell(prototype);
    auto branchOnPrototype = [&](Boxed found, BranchTargets targets) {
        branch(equal(found, wanted), targets);
    };
    branchOnChain(object, branchOnPrototype, slowPath, targets);
}

void OperatorEmitter::generateHasInPrototypeChain(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    ValueID prototypeID = value->prototypeOperand();
    std::optional left = std::exchange(m_operandLeftToInstanceOf, std::nullopt);
    RELEASE_ASSERT(!left || left->read == prototypeID);
    auto walksChain = [&] {
        if (left)
            return Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(left->operand), { object.value() }, { .numberedImmediate = left->number, .runsCodeOfHost = true }).first));
        CellPointer prototype = in<CellPointer>(prototypeID);
        return Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::Prototype), { object.value(), boxCell(prototype).value() }).first));
    };
    if (walksChainInLine(value)) {
        auto branchOnPrototype = [&](Boxed found, BranchTargets targets) {
            if (left)
                emitter<ConstantEmitter>().branchOnIsBuiltinCell(found.value(), m_builtins.canonicalID(operationAt(left->read).builtin()), targets);
            else
                branch(equal(found, boxCell(in<CellPointer>(prototypeID))), targets);
        };
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock slowPath = newBlock();
            branchOnChain(value->valueOperand(), branchOnPrototype, slowPath, targets);
            m_out.appendTo(slowPath);
            branch(walksChain(), targets);
        });
        return;
    }
    if (!typeOf(value->valueOperand()).primitives) {
        defineTest(value, walksChain());
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isCellBlock = newBlock();
        branch(isCell(object), { normalEdge(isCellBlock), targets.notTaken });
        m_out.appendToContinuation(isCellBlock);
        branch(walksChain(), targets);
    });
}

void OperatorEmitter::generateTryFindInPrototypeChain(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    CellPointer prototype = in<CellPointer>(value->prototypeOperand());
    auto walksChain = [&] {
        return Word32(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::PrototypeWithoutTraps), { object.value(), boxCell(prototype).value() }).first));
    };
    if (!walksChainInLine(value)) {
        define(value, walksChain());
        return;
    }
    auto branchOnPrototype = [&](Boxed found, BranchTargets targets) {
        branch(equal(found, boxCell(prototype)), targets);
    };
    LBasicBlock slowPath = newBlock();
    LBasicBlock isFound = newBlock();
    LBasicBlock isAbsent = newBlock();
    branchOnChain(value->valueOperand(), branchOnPrototype, slowPath, { normalEdge(isFound), normalEdge(isAbsent) });
    Join<Word32> join(m_out);
    m_out.appendTo(isFound);
    join.add(word32(1));
    m_out.appendTo(isAbsent);
    join.add(word32(0));
    m_out.appendTo(slowPath);
    join.add(walksChain());
    define(value, join.finish());
}

void OperatorEmitter::generateHasDefaultHasInstance(const ValueContext& value)
{
    defineTest(value, callLeaf<Condition>(RuntimeOperation::HasDefaultHasInstance, { in<Boxed>(value->constructorOperand()) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
