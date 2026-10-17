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
    BitVector hasUnflaggedClaim(m_function.blocks.size());
    Vector<std::pair<ValueID, unsigned>, 4> flaggedClaims;
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
            isClaim = last.opcode == Opcode::Cast && last.castReason() == CastReason::Claim && !last.string() && (m_function.isGenericEntrypoint() || m_function.has(Format::FunctionIsHiddenInStackTraces) || last.reportsMismatchAtCaller() || !claimMayAllocate(m_program, m_function, last));
            if (last.reportsMismatchAtCaller())
                flaggedClaims.append({ block.values.last(), target.block.index() });
            else
                hasUnflaggedClaim.set(target.block.index());
        }
        if (!isClaim)
            isRefused.set(target.block.index());
    }
    if (!flaggedClaims.isEmpty()) {
        BitVector isUsed(m_function.values.size());
        auto use = [&](ValueID child) {
            isUsed.set(child.index());
        };
        for (BlockID id : order) {
            auto& block = m_function.at(id);
            for (ValueID value : block.values)
                m_function.at(value).forEachChild(use);
            if (block.terminator.value)
                use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
            }
        }
        for (auto [claim, handler] : flaggedClaims) {
            if (isUsed.get(claim.index()))
                isRefused.set(handler);
        }
    }
    isHandler.exclude(isRefused);
    if (isHandler.isEmpty() || Options::auditAOTTypedAssertions())
        return;
    BitVector isEntryParameter(m_function.values.size());
    for (ValueID parameter : m_function.blocks[0].parameters)
        isEntryParameter.set(parameter.index());
    for (unsigned index : isHandler) {
        auto& handler = m_function.blocks[index];
        bool isPassThrough = handler.terminator.kind == TerminatorKind::Jump && handler.values.size() == 1 && handler.terminator.targets[0].arguments.isEmpty() && !hasUnflaggedClaim.get(index);
        if (!handler.parameters.isEmpty() || handler.values.isEmpty() || m_function.at(handler.values[0]).opcode != Opcode::CatchValue || (handler.terminator.kind != TerminatorKind::Return && !isPassThrough) || exceptionCaughtIn(BlockID(index)))
            continue;
        BitVector isDefined = isEntryParameter;
        bool isClosed = true;
        auto look = [&](ValueID child) {
            isClosed &= isDefined.get(child.index());
        };
        for (unsigned i = 1; i < handler.values.size(); ++i) {
            m_function.at(handler.values[i]).forEachChild(look);
            isDefined.set(handler.values[i].index());
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

bool Lowering::encodesUndefinedAsZero(ValueID id) const
{
    auto& value = m_function.at(id);
    return value.opcode == Opcode::GetModuleVariable && m_program.variables[value.variable().index()].storesUndefinedAsZero;
}

bool Lowering::isKnownDefinedHere(ValueID read) const
{
    if (read == m_readKnownDefinedOnEdge)
        return true;
    auto found = m_blocksWhereReadIsDefined.find(read.index());
    if (found == m_blocksWhereReadIsDefined.end())
        return false;
    unsigned enter = m_dominatorTreeEnter[m_currentBlockID.index()];
    return std::ranges::any_of(found->value, [&](BlockID block) {
        return m_dominatorTreeEnter[block.index()] <= enter && enter <= m_dominatorTreeLeave[block.index()];
    });
}

Operand Lowering::decodeUndefined(ValueID read, Operand encoded)
{
    auto& decoded = m_decodedReads.add(read.index(), std::pair<LBasicBlock, Operand> { }).iterator->value;
    if (decoded.first != m_out.m_block)
        decoded = { m_out.m_block, Operand(Boxed(m_out.select(m_out.isZero64(encoded.value), undefined().value(), encoded.value))) };
    return decoded.second;
}

ValueID Lowering::readProvenDefinedWhenFalse(const Terminator& terminator) const
{
    if (terminator.targets[0].block == terminator.targets[1].block)
        return { };
    auto& test = m_function.at(terminator.value);
    if (test.opcode == Opcode::IsUndefinedOrNull)
        return encodesUndefinedAsZero(test.child(0)) ? test.child(0) : ValueID { };
    if (test.opcode != Opcode::StrictEq)
        return { };
    for (auto [read, constant] : { std::pair { test.child(0), test.child(1) }, std::pair { test.child(1), test.child(0) } }) {
        if (m_function.at(constant).opcode == Opcode::Undefined && encodesUndefinedAsZero(read))
            return read;
    }
    return { };
}

Operand Lowering::get(ValueID id)
{
    if (!m_deferredLiterals.isEmpty()) [[unlikely]] {
        if (auto* literal = deferredLiteralFor(id); literal && !literal->isBeingMaterialized)
            return materializeDeferredLiteral(*literal);
    }
    for (size_t i = m_assumptions.size(); i--;) {
        if (m_assumptions[i].value == id)
            return m_assumptions[i].operand;
    }
    if (isLazilyFetched(id)) {
        auto& fetched = m_fetchedLeaves.add(id.index(), std::pair<LBasicBlock, Operand> { }).iterator->value;
        if (fetched.first != m_out.m_block) {
            auto& value = m_function.at(id);
            CellPointer cell = value.opcode == Opcode::String ? stringConstant(value.string()) : value.opcode == Opcode::BuiltinFunction ? builtinCell(m_builtins.cellOf(value.builtin())) : concatenateIfNeeded(value);
            Operand operand = convert(cell, m_program.at(value.type), value.rep);
            if (operand.rep == Rep::Cell)
                m_cellTypes.add(operand.value, value.type);
            m_fetchedLeaves.set(id.index(), std::pair { m_out.m_block, operand });
            return operand;
        }
        return fetched.second;
    }
    Operand operand = m_values[id.index()];
    if (encodesUndefinedAsZero(id) && !isKnownDefinedHere(id))
        return decodeUndefined(id, operand);
    auto& declared = m_program.at(m_function.at(id).type);
    if (!declared.mayBeUndefinedOrNull() || declared.isUndefinedOrNull() || !hasPassedNullishTest(id))
        return operand;
    if (operand.rep != Rep::JSValue) {
        auto& narrowed = withoutNullish(m_function.at(id).type);
        return repOf(narrowed) == operand.rep ? operand : convert(operand, narrowed, repOf(narrowed));
    }
    if (Options::validateAOTTypedFacts()) [[unlikely]] {
        LBasicBlock doesNotHold = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isUndefinedOrNull(operand.as<Boxed>()).value(), rarely(doesNotHold), usually(continuation));
        m_out.appendTo(doesNotHold);
        AOT_TYPED_OPERATION(FactViolated, operand.as<Boxed>(), word32(m_id.index()), word32(id.index()));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    }
    auto& narrowed = withoutNullish(m_function.at(id).type);
    if (repOf(narrowed) == Rep::Cell)
        return asCell(operand.as<Boxed>());
    return repOf(narrowed) == Rep::JSValue ? operand : convert(operand, narrowed, repOf(narrowed));
}

ValueID Lowering::exceptionCaughtIn(BlockID handler) const
{
    if (!m_program.propagatesExceptionObjects)
        return { };
    for (ValueID id : m_function.at(handler).values) {
        if (m_function.at(id).catchesExceptionObject())
            return id;
    }
    return { };
}

bool Lowering::ignoresCaughtValue(BlockID handler)
{
    bool catches = false;
    for (ValueID id : m_function.at(handler).values) {
        if (m_function.at(id).opcode != Opcode::CatchValue)
            continue;
        if (useCountOf(id))
            return false;
        catches = true;
    }
    return catches;
}

Boxed Lowering::catchException(BlockID handler)
{
    Boxed caught = exceptionCaughtIn(handler) ? AOT_TYPED_OPERATION(CatchException) : AOT_TYPED_OPERATION(Catch);
    m_heaps.decoratePatchpointWrite(&m_heaps.root, &m_heaps.root, caught.value());
    LBasicBlock isTermination = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.isZero64(caught.value()), rarely(isTermination), usually(continuation));
    m_out.appendTo(isTermination);
    AOT_TYPED_OPERATION(ThrowToHandlerInFunction, undefined());
    m_out.unreachable();
    m_out.appendTo(continuation);
    return caught;
}

LBasicBlock Lowering::landingPadFor(const Target& handler)
{
    static constexpr unsigned maxLandingPadsSearched = 4;
    auto& landingPads = m_landingPads.add(handler.block.index(), Vector<LandingPad, 2> { }).iterator->value;
    for (size_t i = landingPads.size(); i-- && i + maxLandingPadsSearched >= landingPads.size();) {
        if (landingPads[i].handler->arguments == handler.arguments && dominates(landingPads[i].source, m_currentBlockID)) {
            m_isCurrentLandingPadShared = true;
            return landingPads[i].block;
        }
    }
    landingPads.append({ m_currentBlockID, &handler, newColdBlock() });
    return landingPads.last().block;
}

void Lowering::didLowerOperationWithHandler()
{
    RELEASE_ASSERT(!m_callSitesAwaitingLandingPad);
    const Target* handler = std::exchange(m_activeHandler, nullptr);
    LBasicBlock landingPad = std::exchange(m_currentLandingPad, nullptr);
    if (!landingPad || std::exchange(m_isCurrentLandingPadShared, false))
        return;
    LBasicBlock continuation = m_out.m_block;
    m_out.appendTo(landingPad);
    LBasicBlock target = edgeTo(*handler, false);
    if (LValue caught = m_caughtValues.get(handler->block.index())) {
        target = m_catchBlocks.ensure(handler->block.index(), [&] {
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
