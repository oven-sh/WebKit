/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCallEmitter.h"
#include "AOTDecimalEmitter.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

bool CodeGenerator::mayComputeWithDecimals(BlockID id) const
{
    return code().at(id).frequency != BlockFrequency::Rare && !m_keepsValuesInHeapFrame && !m_checksEffects && !m_interleaving && !m_headOfTurnsLaidOut && !m_rotatingIteration && !m_rows && !m_lanes;
}

void CodeGenerator::findReusedDecimals()
{
    m_reusedDecimals = nullptr;
    if (!code().hasLoops() || code().hasSuspensions() || m_keepsValuesInHeapFrame || m_checksEffects || m_rows || build().options().admitsUnseenCode())
        return;
    const ReusedDecimals& reused = emitter<DecimalEmitter>().findReusedDecimals(order(), scopedLambda<bool(BlockID, BlockID)>([&](BlockID home, BlockID user) {
        return dominates(home, user);
    }));
    if (!reused.numberOfObjects())
        return;
    build().statistics().count(StatisticID::DecimalsReused);
    m_reusedDecimals = &reused;
}

Boxed CodeGenerator::getReusedDecimal(ValueID value)
{
    SetForScope withoutLettingGo(m_watchesReads, false);
    return get(value).as<Boxed>();
}

bool CodeGenerator::isReusedDecimal(ValueID value) const
{
    return m_reusedDecimals->contains(value);
}

bool CodeGenerator::handsOnReusedDecimal(ValueID argument, ValueID parameter) const
{
    return m_reusedDecimals->shareObject(argument, parameter);
}

void CodeGenerator::letGoOfDecimal(ValueID value)
{
    DecimalEmitter& decimals = emitter<DecimalEmitter>();
    // A landing pad serves the whole block.
    if (m_isPassingHandlerArguments || !decimals.letGoForRestOfBlock().contains(value))
        decimals.letGo(value);
}

void CodeGenerator::letGoOfDecimalForRestOfBlock(ValueID operand)
{
    if (!isReusedDecimal(operand))
        return;
    letGoOfDecimal(operand);
    emitter<DecimalEmitter>().letGoForRestOfBlock().appendIfNotContains(operand);
}

void CodeGenerator::letGoOfDecimalsReadBy(ValueID reader)
{
    code().at(reader).forEachChild([&](ValueID operand) {
        letGoOfDecimalForRestOfBlock(operand);
    });
}

void CodeGenerator::letGoOfDecimalsReadAtEndOf(const Block& block, size_t numberOfValuesLeftToTerminator)
{
    for (ValueID value : code().valuesOf(block).last(numberOfValuesLeftToTerminator))
        letGoOfDecimalsReadBy(value);
    block.terminator.forEachChild([&](ValueID operand) {
        letGoOfDecimalForRestOfBlock(operand);
    });
}

void CodeGenerator::forgetDecimalsLetGo()
{
    emitter<DecimalEmitter>().letGoForRestOfBlock().shrink(0);
}

size_t CodeGenerator::beginDecimalRun(BlockID block, std::span<const ValueID> values, size_t first)
{
    using Step = DecimalRun::Step;
    if (!mayComputeWithDecimals(block))
        return notFound;
    DecimalEmitter& decimals = emitter<DecimalEmitter>();
    std::optional<DecimalRun> run = decimals.findRun(block, values.subspan(first));
    if (!run)
        return notFound;
    build().statistics().count(StatisticID::DecimalRuns);

    DecimalEmitter::RunAsWritten& end = decimals.runAsWritten();
    end = { };
    for (const DecimalRun::Part& part : run->parts) {
        if (part.step != Step::IsMadeAhead)
            continue;
        end.madeAhead.append(part.value);
        generateValue(block, part.value, code().at(part.value), false, { });
    }

    auto generateFor = [&](ValueID value, NOESCAPE const Invocable<void(const ValueContext&)> auto& functor) {
        ValueContext context(block, value, code().at(value), handlerOf(block));
        SetForScope<const OperationContext*> operation(m_operation, &context);
        m_out.setOrigin(machineOriginOf(value));
        m_out.clearDeclaredAccesses();
        functor(context);
        m_out.setOrigin(Machine::Origin());
    };
    m_isAheadOfWhatMayThrow = false;
    LBasicBlock asWritten = m_out.newBlock();
    ScopeFork scope = m_out.forkScope();
    size_t numberLetGo = decimals.letGoForRestOfBlock().size();
    decimals.beginRun(*run, block, asWritten);
    auto endRun = makeScopeExit([&] { decimals.endRun(); });
    for (const DecimalRun::Part& part : run->parts) {
        if (m_hasFailed)
            return notFound;
        if (part.step == Step::IsMadeTwice) {
            if (m_reusedDecimals)
                letGoOfDecimalsReadBy(part.value);
            generateValue(block, part.value, code().at(part.value), false, { });
        } else if (part.step != Step::IsMadeAhead)
            generateFor(part.value, [&](const ValueContext& context) { decimals.generate(context, part); });
    }
    for (const DecimalRun::Part& part : run->parts) {
        if (part.isReadBehindRun)
            generateFor(part.value, [&](const ValueContext& context) { decimals.makeObject(context); });
    }

    if (run->endsInReturnedCall) {
        generateFor(run->parts.last().value, [&](const ValueContext& context) {
            Operand result = get(context.id());
            emitter<CallEmitter>().returnValues(singleElementSpan(result));
        });
    } else
        end.continuation = m_out.newBlock();
    for (const DecimalRun::Part& part : run->parts) {
        if (part.step == Step::IsMadeAhead)
            continue;
        LValue made = std::exchange(m_values[part.value.index()], nullptr);
        forgetBoxed(part.value);
        if (made && part.isReadBehindRun && end.continuation)
            end.results.append({ part.value, m_out.anchor(made) });
    }
    if (end.continuation)
        m_out.jump(normalEdge(end.continuation));
    decimals.letGoForRestOfBlock().shrink(numberLetGo);
    m_out.appendToJoin(asWritten, scope);
    return first + run->parts.size() - run->endsInReturnedCall;
}

bool CodeGenerator::isMadeAheadOfDecimalRun(ValueID value)
{
    return emitter<DecimalEmitter>().runAsWritten().madeAhead.contains(value);
}

void CodeGenerator::endDecimalRun()
{
    DecimalEmitter::RunAsWritten run = std::exchange(emitter<DecimalEmitter>().runAsWritten(), { });
    if (!run.continuation)
        return;
    Vector<ValueFromBlock, 4> asWritten = run.results.map<Vector<ValueFromBlock, 4>>([&](auto& result) {
        return m_out.anchor(m_values[result.first.index()]);
    });
    m_out.jump(normalEdge(run.continuation));
    m_out.appendTo(run.continuation);
    for (size_t index = 0; index < asWritten.size(); ++index) {
        auto [value, forDecimals] = run.results[index];
        LValue& word = m_values[value.index()];
        word = Operand(representationOf(value), m_out.phi(word->type(), forDecimals, asWritten[index])).value;
    }
}

void CodeGenerator::endElementMovesOrDecimalRun(std::optional<ElementMoves>& moves)
{
    if (moves)
        endElementMoves(moves);
    else
        endDecimalRun();
    if (m_reusedDecimals)
        forgetDecimalsLetGo();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
