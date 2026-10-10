/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3VariableValue.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

// A handler is an entry point of its own, entered with nothing in the registers: what is live into it is kept in the frame (to the back end: a variable), and what an edge passes is stored before the operation that may throw.

void Lowering::computeCatchEntrypoints(const Vector<BlockID>& order)
{
    auto handlerOf = [&](const Terminator& terminator) -> const Target* {
        if (terminator.kind == TerminatorKind::Invoke)
            return &terminator.targets[1];
        if (terminator.kind == TerminatorKind::Throw && !terminator.targets.isEmpty())
            return &terminator.targets[0];
        return nullptr;
    };
    for (BlockID block : order) {
        const Target* target = handlerOf(m_function.at(block).terminator);
        if (!target)
            continue;
        if (m_catchEntrypointForBlock.add(target->block.index(), m_catchEntrypoints.size()).isNewEntry)
            m_catchEntrypoints.append(Handler { target->block, { }, 0, nullptr });
    }
    if (m_catchEntrypoints.isEmpty())
        return;

    unsigned numberOfValues = m_function.values.size();
    Vector<BitVector> liveAtHead(m_function.blocks.size());
    bool changed;
    do {
        changed = false;
        for (unsigned i = order.size(); i--;) {
            auto& block = m_function.at(order[i]);
            BitVector live(numberOfValues);
            for (auto& target : successorsOf(block.terminator)) {
                live.merge(liveAtHead[target.block.index()]);
                for (ValueID argument : target.arguments)
                    live.set(argument.index());
            }
            if (block.terminator.value)
                live.set(block.terminator.value.index());
            for (unsigned v = block.values.size(); v--;) {
                live.clear(block.values[v].index());
                m_function.at(block.values[v]).forEachChild([&](ValueID child) {
                    live.set(child.index());
                });
            }
            for (ValueID parameter : block.parameters)
                live.clear(parameter.index());
            if (live != liveAtHead[order[i].index()]) {
                liveAtHead[order[i].index()] = WTF::move(live);
                changed = true;
            }
        }
    } while (changed);

    unsigned numberOfWords = 0;
    for (auto& handler : m_catchEntrypoints) {
        for (unsigned index : liveAtHead[handler.block.index()]) {
            handler.liveIn.append(ValueID(index));
            m_catchSpills.ensure(index, [&] {
                return CatchSpill { m_proc.addVariable(b3TypeOf(m_function.values[index].rep)), numberOfWords++ };
            });
        }
        handler.firstParameterWord = numberOfWords;
        numberOfWords += m_function.at(handler.block).parameters.size();
    }
    if (m_function.has(FunctionUsesArgumentBuffer)) {
        m_argumentCountVariable = m_proc.addVariable(Int32);
        m_argumentBufferVariable = m_proc.addVariable(Int64);
        m_firstArgumentWord = numberOfWords;
        numberOfWords += 2;
    }
    m_catchSpillSlotCount = numberOfWords;
}

TypedPointer Lowering::catchSpillSlot(unsigned word)
{
    return m_out.address(m_heaps.AOTTyped_catchSpillSlots[word], m_catchSpillArea, word * sizeof(Register));
}

void Lowering::storeToCatchSpillSlot(unsigned word, Operand operand)
{
    switch (b3TypeOf(operand.rep).kind()) {
    case Int32:
        m_out.store32(operand.value, catchSpillSlot(word));
        return;
    case Int64:
        m_out.store64(operand.value, catchSpillSlot(word));
        return;
    case Double:
        m_out.storeDouble(operand.value, catchSpillSlot(word));
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Operand Lowering::loadFromCatchSpillSlot(unsigned word, Rep rep)
{
    switch (b3TypeOf(rep).kind()) {
    case Int32:
        return Operand(rep, m_out.load32(catchSpillSlot(word)));
    case Int64:
        return Operand(rep, m_out.load64(catchSpillSlot(word)));
    case Double:
        return Operand(rep, m_out.loadDouble(catchSpillSlot(word)));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void Lowering::define(ValueID id, Operand operand)
{
    m_values[id.index()] = operand;
    if (m_catchSpills.isEmpty())
        return;
    auto iterator = m_catchSpills.find(id.index());
    if (iterator == m_catchSpills.end())
        return;
    storeToCatchSpillSlot(iterator->value.word, operand);
    m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), iterator->value.variable, operand.value);
}

Operand Lowering::get(ValueID id)
{
    for (size_t i = m_assumptions.size(); i--;) {
        if (m_assumptions[i].value == id)
            return m_assumptions[i].operand;
    }
    if (isFetchedWhereNeeded(id)) {
        auto& fetched = m_fetchedLeaves.add(id.index(), std::pair<LBasicBlock, Operand> { }).iterator->value;
        if (fetched.first != m_out.m_block) {
            auto& value = m_function.at(id);
            // The fetch may end the block.
            CellPointer cell = value.opcode == Opcode::String ? stringConstant(value.string()) : value.opcode == Opcode::BuiltinFunction ? builtinCell(m_builtins.cellOf(value.builtin())) : concatenateWhereNeeded(value);
            Operand operand = convert(cell, m_program.at(value.type), value.rep);
            if (operand.rep == Rep::Cell)
                m_typeOfCell.add(operand.value, value.type);
            m_fetchedLeaves.set(id.index(), std::pair { m_out.m_block, operand });
            return operand;
        }
        return fetched.second;
    }
    Operand operand = m_values[id.index()];
    if (auto iterator = m_catchSpills.find(id.index()); iterator != m_catchSpills.end())
        operand = Operand(m_function.at(id).rep, m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), iterator->value.variable));
    // As typeOf() has it.
    auto& declared = m_program.at(m_function.at(id).type);
    if (!declared.mayBeUndefinedOrNull() || declared.isUndefinedOrNull() || !hasPassedNilTest(id))
        return operand;
    // It is kept as its type is: whoever goes by typeOf() finds what it expects.
    if (operand.rep != Rep::JSValue) {
        auto& narrowed = withoutNil(m_function.at(id).type);
        return repOf(narrowed) == operand.rep ? operand : convert(operand, narrowed, repOf(narrowed));
    }
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        LBasicBlock doesNotHold = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isUndefinedOrNull(operand.as<Boxed>()).value(), rarely(doesNotHold), usually(continuation));
        m_out.appendTo(doesNotHold);
        AOT_TYPED_OPERATION(FactDoesNotHold, operand.as<Boxed>(), word32(m_id.index()), word32(id.index()));
        m_out.unreachable();
        m_out.appendTo(continuation);
    }
    auto& narrowed = withoutNil(m_function.at(id).type);
    if (repOf(narrowed) == Rep::Cell)
        return asCell(operand.as<Boxed>());
    return repOf(narrowed) == Rep::JSValue ? operand : convert(operand, narrowed, repOf(narrowed));
}

// Before what may throw: what the handler is passed if it does.
void Lowering::spillForCatch(const Target& target)
{
    unsigned index = m_catchEntrypointForBlock.get(target.block.index());
    auto& handler = m_catchEntrypoints[index];
    auto& parameters = m_function.at(target.block).parameters;
    for (unsigned i = 0; i < target.arguments.size(); ++i)
        storeToCatchSpillSlot(handler.firstParameterWord + i, in(target.arguments[i], m_function.at(parameters[i]).rep));
    m_currentCatchEntrypoint = index;
}

void Lowering::lowerCatchEntrypoint(Handler& handler)
{
    m_out.appendTo(handler.entry);
    if (m_argumentCountVariable) {
        m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), m_argumentCountVariable, m_out.load32(catchSpillSlot(m_firstArgumentWord)));
        m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), m_argumentBufferVariable, m_out.load64(catchSpillSlot(m_firstArgumentWord + 1)));
    }
    for (ValueID id : handler.liveIn) {
        auto& kept = m_catchSpills.find(id.index())->value;
        Operand value = loadFromCatchSpillSlot(kept.word, m_function.at(id).rep);
        m_out.m_block->appendNew<VariableValue>(m_proc, Set, m_out.origin(), kept.variable, value.value);
    }
    restoreIterationsOfStrings();
    auto& parameters = m_function.at(handler.block).parameters;
    auto& phis = m_phis[handler.block.index()];
    for (unsigned i = 0; i < parameters.size(); ++i)
        m_out.addIncomingToPhi(phis[i], m_out.anchor(loadFromCatchSpillSlot(handler.firstParameterWord + i, m_function.at(parameters[i]).rep).value));
    m_out.jump(m_blocks[handler.block.index()]);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
