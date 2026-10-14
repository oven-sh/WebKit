/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "JSCInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

void Lowering::findHandlersEnteredByBranch(const Vector<BlockID>& order)
{
    BitVector isHandler(m_function.blocks.size());
    BitVector isRefused(m_function.blocks.size());
    for (BlockID id : order) {
        auto& block = m_function.at(id);
        if (block.terminator.kind == TerminatorKind::Throw && !block.terminator.targets.isEmpty())
            isRefused.set(block.terminator.targets[0].block.index());
        if (block.terminator.kind != TerminatorKind::Invoke)
            continue;
        auto& target = block.terminator.targets[1];
        isHandler.set(target.block.index());
        bool isClaim = !block.values.isEmpty() && target.arguments.isEmpty();
        if (isClaim) {
            auto& last = m_function.at(block.values.last());
            isClaim = last.opcode == Opcode::Cast && last.castReason() == CastReason::Claim && !last.string() && (m_function.isGenericEntrypoint() || m_function.has(Format::FunctionIsHiddenInStackTraces) || !claimMayMakeObjects(m_program, m_function, last));
        }
        if (!isClaim)
            isRefused.set(target.block.index());
    }
    isHandler.exclude(isRefused);
    if (isHandler.isEmpty() || Options::auditAOTTypedAssertions())
        return;
    BitVector isParameterOfEntry(m_function.values.size());
    for (ValueID parameter : m_function.blocks[0].parameters)
        isParameterOfEntry.set(parameter.index());
    for (unsigned index : isHandler) {
        auto& handler = m_function.blocks[index];
        if (!handler.parameters.isEmpty() || handler.values.isEmpty() || m_function.at(handler.values[0]).opcode != Opcode::CatchValue || handler.terminator.kind != TerminatorKind::Return || exceptionCaughtIn(BlockID(index)))
            continue;
        BitVector isThere = isParameterOfEntry;
        bool isClosed = true;
        auto look = [&](ValueID child) {
            isClosed &= isThere.get(child.index());
        };
        for (unsigned i = 1; i < handler.values.size(); ++i) {
            m_function.at(handler.values[i]).forEachChild(look);
            isThere.set(handler.values[i].index());
        }
        if (handler.terminator.value)
            look(handler.terminator.value);
        if (isClosed)
            m_handlersEnteredByBranch.set(index);
    }
}

LBasicBlock Lowering::handlerEnteredByBranch() const
{
    if (m_handlersEnteredByBranch.isEmpty() || !m_currentBlock || m_currentBlock->terminator.kind != TerminatorKind::Invoke || m_currentIndexInBlock + 1 != m_currentBlock->values.size())
        return nullptr;
    unsigned handler = m_currentBlock->terminator.targets[1].block.index();
    return m_handlersEnteredByBranch.get(handler) ? m_blocks[handler] : nullptr;
}

void Lowering::define(ValueID id, Operand operand)
{
    m_values[id.index()] = operand;
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
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
    auto& narrowed = withoutNil(m_function.at(id).type);
    if (repOf(narrowed) == Rep::Cell)
        return asCell(operand.as<Boxed>());
    return repOf(narrowed) == Rep::JSValue ? operand : convert(operand, narrowed, repOf(narrowed));
}

ValueID Lowering::exceptionCaughtIn(BlockID handler) const
{
    if (!m_program.passesExceptionsOn)
        return { };
    for (ValueID id : m_function.at(handler).values) {
        if (m_function.at(id).yieldsExceptionItself())
            return id;
    }
    return { };
}

Boxed Lowering::catchException(BlockID handler)
{
    Boxed caught = exceptionCaughtIn(handler) ? AOT_TYPED_OPERATION(CatchException) : AOT_TYPED_OPERATION(Catch);
    m_heaps.decoratePatchpointWrite(&m_heaps.root, &m_heaps.root, caught.value());
    LBasicBlock isTermination = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.isZero64(caught.value()), rarely(isTermination), usually(continuation));
    m_out.appendTo(isTermination);
    AOT_TYPED_OPERATION(Throw, undefined());
    m_out.unreachable();
    m_out.appendTo(continuation);
    return caught;
}

LBasicBlock Lowering::landingPadFor(const Target& handler)
{
    static constexpr unsigned maxLandingPadsLookedAt = 4;
    auto& landingPads = m_landingPads.add(handler.block.index(), Vector<LandingPad, 2> { }).iterator->value;
    for (size_t i = landingPads.size(); i-- && i + maxLandingPadsLookedAt >= landingPads.size();) {
        if (landingPads[i].handler->arguments == handler.arguments && dominates(landingPads[i].source, m_currentBlockID)) {
            m_isCurrentLandingPadShared = true;
            return landingPads[i].block;
        }
    }
    landingPads.append({ m_currentBlockID, &handler, newColdBlock() });
    return landingPads.last().block;
}

void Lowering::didLowerWatchedOperation()
{
    RELEASE_ASSERT(!m_callSitesAwaitingLandingPad);
    const Target* handler = std::exchange(m_watchingHandler, nullptr);
    LBasicBlock landingPad = std::exchange(m_currentLandingPad, nullptr);
    if (!landingPad || std::exchange(m_isCurrentLandingPadShared, false))
        return;
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(landingPad);
    LBasicBlock target = edgeTo(*handler, false);
    if (LValue caught = m_caughtValues.get(handler->block.index())) {
        target = m_blocksThatCatch.ensure(handler->block.index(), [&] {
            LBasicBlock edge = m_out.m_block;
            LBasicBlock block = newColdBlock();
            m_out.appendTo(block);
            Boxed value = catchException(handler->block);
            m_out.addIncomingToPhi(caught, m_out.anchor(value.value()));
            m_out.jump(target);
            m_out.appendTo(edge);
            return block;
        }).iterator->value;
    }
    m_out.jump(target);
    m_out.appendTo(continuation);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
