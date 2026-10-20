/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSplitUnions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTValueSubstitution.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueID appendCopy(IRBuilder& builder, Value&& copy)
{
    if (resultRuleOf(copy.opcode) == ResultRule::Declared) {
        TypeID type = copy.type;
        return builder.appendWithType(WTF::move(copy), type);
    }
    // A narrower operand may narrow the type, and a prediction has to lie strictly inside the type: the builder computes both again.
    copy.setWritten({ });
    copy.setPrediction({ });
    return builder.append(WTF::move(copy));
}

static BlockFrequency frequencyOfRemainingPaths(const ValueSplit& split, uint32_t firstRemainingArm)
{
    BlockFrequency frequency = split.hasRest ? split.restFrequency : BlockFrequency::Rare;
    for (const SplitArm& arm : split.arms.subspan(firstRemainingArm))
        frequency = moreFrequentOf(frequency, arm.frequency);
    return frequency;
}

void forEachValueSite(FunctionAnalyses& analyses, const Function& function, NOESCAPE const ScopedLambda<void(const ValueSite&)>& functor)
{
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index)
            functor(ValueSite { block, index, values[index] });
    }
}

void checkRewrite(FunctionPassContext& context, const ValueSite& site, const ReplacedEffects& replaced)
{
    const Value& value = context.function.at(site.value);
    if (replacedOperationsOf(value.opcode).empty())
        return;
    checkReplacement({ context.program, context.inputs.effects }, context.function, { context.id, site.block, site.value }, value, replaced, context.build.diagnostics());
}

SiteExpansion::SiteExpansion(FunctionPassContext& context, IRBuilder& builder)
    : m_context(context)
    , m_builder(builder)
{
}

SiteExpansion::~SiteExpansion()
{
    RELEASE_ASSERT(!isOpen());
}

void SiteExpansion::begin(const ValueSite& site)
{
    RELEASE_ASSERT(!isOpen());
    RELEASE_ASSERT(function().at(site.block).values[site.index] == site.value);

    m_site = site;
    m_original = function().at(site.value);
    m_frequency = function().at(site.block).frequency;
    m_continuationPredecessors.clear();
    m_operationsBeforeOriginal.clear();
    m_operandIndexInOriginal = 0;
    m_appended.clear();
    m_replaced = { };
    addReplaced(m_original);

    m_expansion = m_builder.beginExpansion(site.point());
    takeTailCall();
}

void SiteExpansion::takeTailCall()
{
    Block& continuation = function().at(m_expansion.continuation);
    if (continuation.terminator.kind != TerminatorKind::ReturnCall || continuation.terminator.callOperand() != m_site.value)
        return;

    // Each arm returns on its own, so nothing enters the continuation.
    m_tailCall = WTF::move(continuation.terminator);
    continuation = Block { };
    becomeUndefined(function(), m_site.value);
    m_context.analyses.didChangeControlFlow();
}

void SiteExpansion::end()
{
    RELEASE_ASSERT(isOpen() && !m_builder.currentBlock());
    checkAppendedOperations();
    if (m_continuationPredecessors.size() == 1)
        m_builder.mergeContinuation(m_expansion, m_continuationPredecessors[0]);
    m_builder.setHandler(std::nullopt);
    m_tailCall = std::nullopt;
    m_site = { };
}

void SiteExpansion::addReplaced(const Value& value)
{
    m_replaced.add({ m_context.program, m_context.inputs.effects }, function(), value);
}

void SiteExpansion::setOperationsBeforeOriginal(Vector<Value, 2>&& values, uint32_t operandIndexInOriginal)
{
    RELEASE_ASSERT(isOpen());
    m_operationsBeforeOriginal = WTF::move(values);
    m_operandIndexInOriginal = operandIndexInOriginal;
}

ValueID SiteExpansion::append(Value&& value)
{
    ValueID id = m_builder.append(WTF::move(value));
    m_appended.append(id);
    return id;
}

ValueID SiteExpansion::appendOperationsBeforeOriginal(const OriginalCopy& copy)
{
    Origin originOfOriginal = m_builder.origin();
    ValueID previous;
    for (const Value& operation : m_operationsBeforeOriginal) {
        Value value = operation;
        if (previous)
            value.setOperand(0, previous);
        else {
            if (copy.operand) {
                std::optional<uint8_t> receiver = receiverOf(value.opcode);
                RELEASE_ASSERT(receiver);
                value.setOperand(*receiver, copy.operand);
            }
            if (copy.receivers && PayloadWord::receivers(value.opcode))
                value.setReceivers(copy.receivers);
        }
        m_builder.setOrigin(operation.origin());
        previous = appendCopy(m_builder, WTF::move(value));
    }
    m_builder.setOrigin(originOfOriginal);
    return previous;
}

ValueID SiteExpansion::appendOriginal(const OriginalCopy& copy)
{
    RELEASE_ASSERT(isOpen());
    Value value = m_original;
    bool hasOperationsBefore = !m_operationsBeforeOriginal.isEmpty();
    if (hasOperationsBefore)
        value.setOperand(m_operandIndexInOriginal, appendOperationsBeforeOriginal(copy));
    if (copy.operandIndex) {
        ValueID replaced = value.operand(*copy.operandIndex);
        value.setOperand(*copy.operandIndex, copy.operand);
        if (copy.isNarrowedValue) {
            value.forEachOperand([&](ValueID& operand, uint32_t) {
                if (operand == replaced)
                    operand = copy.operand;
            });
        }
    }
    if (copy.receivers && !hasOperationsBefore && PayloadWord::receivers(value.opcode))
        value.setReceivers(copy.receivers);
    if (copy.callees && PayloadWord::callees(value.opcode))
        value.setCallees(copy.callees);
    return appendCopy(m_builder, WTF::move(value));
}

void SiteExpansion::finishArm(ValueID result)
{
    RELEASE_ASSERT(isOpen());
    if (result && m_context.program.typeOf(function(), result).isNever()) {
        finishArmAsUnreachable();
        return;
    }
    if (m_tailCall) {
        finishArmWithReturn(result);
        return;
    }
    if (m_expansion.results.isEmpty()) {
        finishArmWithoutResult();
        return;
    }

    RELEASE_ASSERT(result && m_expansion.results.size() == 1);
    ValueID converted = convertTo(m_builder, result, DeclaredType { .type = m_original.type });
    if (function().at(converted).type == TypeTable::never) {
        finishArmAsUnreachable();
        return;
    }
    m_continuationPredecessors.append(m_builder.currentBlock());
    m_builder.endExpansion(m_expansion, converted);
}

bool SiteExpansion::canEndWithTailCall(ValueID result)
{
    const Program& program = m_context.program;
    const Block& block = function().at(m_builder.currentBlock());
    Opcode opcode = function().at(result).opcode;
    bool isCall = opcode == m_original.opcode || opcode == Opcode::CallDirect;
    bool endsBlock = !block.values.isEmpty() && block.values.last() == result;
    // The casts of the terminator were resolved against the type of the original call.
    return isCall && endsBlock && !block.handler && program.typeOf(function(), result).isSubsetOf(program.at(m_original.type), program);
}

void SiteExpansion::finishArmWithReturn(ValueID result)
{
    RELEASE_ASSERT(result);
    bool endsWithTailCall = canEndWithTailCall(result);
    // An arm marks its call before it is known whether the call can stay a tail call.
    if (endsWithTailCall)
        function().at(result).setIsDemotedTailCall(false);
    else
        result = convertTo(m_builder, result, DeclaredType { .type = m_original.type });
    if (function().at(result).type == TypeTable::never) {
        finishArmAsUnreachable();
        return;
    }

    BlockID block = m_builder.currentBlock();
    Origin origin = m_builder.origin();
    Terminator terminator = *m_tailCall;
    terminator.setCallOperand(result);
    m_builder.setOrigin(terminator.origin());
    m_builder.emitTerminator(WTF::move(terminator));
    if (!endsWithTailCall) {
        m_builder.demoteReturnCall(block);
        m_context.build.statistics().count(StatisticID::ReturnCallsDemoted);
    }
    m_builder.setOrigin(origin);
}

void SiteExpansion::finishArmWithoutResult()
{
    RELEASE_ASSERT(isOpen() && m_expansion.results.isEmpty());
    m_continuationPredecessors.append(m_builder.currentBlock());
    m_builder.endExpansion(m_expansion, std::span<const ValueID> { });
}

void SiteExpansion::finishArmAsUnreachable()
{
    RELEASE_ASSERT(isOpen());
    m_builder.emitUnreachable();
}

void SiteExpansion::checkAppendedOperations()
{
    for (ValueID id : m_appended)
        checkRewrite(m_context, ValueSite { m_site.block, m_site.index, id }, m_replaced);
}

void emitSplit(SiteExpansion& expansion, const ValueSplit& split, NOESCAPE const EmitSplitArm& emitArm, NOESCAPE const EmitSplitRest& emitRest)
{
    IRBuilder& builder = expansion.builder();
    const Program& program = expansion.context().program;

    ValueID current = split.value;
    for (uint32_t index = 0; index < split.arms.size(); ++index) {
        const SplitArm& arm = split.arms[index];
        bool entersWithoutTest = !split.hasRest && index + 1 == split.arms.size();
        if (entersWithoutTest) {
            RELEASE_ASSERT(program.typeOf(expansion.function(), current).isSubsetOf(program.at(arm.testedType), program));
            emitArm(index, current);
            RELEASE_ASSERT(!builder.currentBlock());
            return;
        }

        ValueID test = builder.appendTypeTest(current, arm.testedType);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, arm.frequency, frequencyOfRemainingPaths(split, index + 1));
        emitArm(index, branch.provenOnTaken);
        RELEASE_ASSERT(!builder.currentBlock());
        builder.moveToEndOf(branch.notTaken);
        current = branch.provenOnNotTaken;
    }

    emitRest(current);
    RELEASE_ASSERT(!builder.currentBlock());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
