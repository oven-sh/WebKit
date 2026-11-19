/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCallEmitter.h"
#include "AOTDecimalEmitter.h"
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
    // A landing pad serves the whole block.
    if (m_isPassingHandlerArguments || !m_decimalsLetGo.contains(value))
        emitter<DecimalEmitter>().letGo(value);
}

auto CodeGenerator::beginDecimalRun(BlockID block, std::span<const ValueID> values, size_t first) -> std::optional<EndOfDecimalRun>
{
    using Step = DecimalRun::Step;
    DecimalEmitter& decimals = emitter<DecimalEmitter>();
    std::optional<DecimalRun> run = decimals.findRun(block, values.subspan(first));
    if (!run)
        return std::nullopt;
    build().statistics().count(StatisticID::DecimalRuns);

    EndOfDecimalRun end { first + run->parts.size() - run->endsInReturnedCall };
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
    size_t numberLetGo = m_decimalsLetGo.size();
    decimals.beginRun(*run, block, asWritten);
    for (const DecimalRun::Part& part : run->parts) {
        if (m_hasFailed)
            return std::nullopt;
        if (part.step == Step::IsMadeTwice) {
            if (m_reusedDecimals)
                letGoOfDecimalsReadBy(code().at(part.value));
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
    m_decimalsLetGo.shrink(numberLetGo);
    m_out.appendToJoin(asWritten, scope);
    return end;
}

void CodeGenerator::endDecimalRun(std::optional<EndOfDecimalRun>& run)
{
    if (run->continuation) {
        Vector<ValueFromBlock, 4> asWritten = run->results.map<Vector<ValueFromBlock, 4>>([&](auto& result) {
            return m_out.anchor(m_values[result.first.index()]);
        });
        m_out.jump(normalEdge(run->continuation));
        m_out.appendTo(run->continuation);
        for (size_t index = 0; index < asWritten.size(); ++index) {
            auto [value, forDecimals] = run->results[index];
            LValue& word = m_values[value.index()];
            word = Operand(representationOf(value), m_out.phi(word->type(), forDecimals, asWritten[index])).value;
        }
    }
    m_decimalsLetGo.shrink(0);
    run = std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
