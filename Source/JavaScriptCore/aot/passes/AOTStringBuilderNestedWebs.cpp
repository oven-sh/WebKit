/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTRuleSupport.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t stateOperandIndex = 0;
static_assert(OperandIndex::Add::left == stateOperandIndex);
static_assert(OperandIndex::Strcat::parts == stateOperandIndex);

bool isUnobservable(const Effects& effects)
{
    return effects.writes.containsOnly(bookkeepingHeaps) && !effects.mayThrow && !effects.mayRunOtherCode();
}

void retypeAsString(Value& state)
{
    state.type = TypeTable::string;
    state.setExpected({ });
}

struct OperandSite {
    ValueID user;
    uint32_t operandIndex { 0 };
};

struct NestedWeb {
    const StringWeb* web { nullptr };
    ValueID lastState;
    Vector<ValueID, 2> casts;
    Vector<ValueID, 8> front;

    OperandSite readOfLastState(OperandSite part) const { return casts.isEmpty() ? part : OperandSite { casts.last(), OperandIndex::Upcast::value }; }
};

struct MergedConcatenation {
    ValueID concatenation;
    Vector<NestedWeb, 1> nestedWebs;
    Vector<ValueID, 8> remainingOperands;
};

// a + b + (c ? d + e : f) + g is computed as ((c ? a + b + d + e : a + b + f) + g), so that one web holds every state and nothing is built apart.
class NestedWebMerger {
public:
    NestedWebMerger(FunctionPassContext&, const StringBuilderPlan&);

    bool run();

private:
    const Type& typeOf(ValueID id) const { return m_context.program.typeOf(m_function, id); }
    bool isEmptyString(ValueID id) const { return stringConstantOf(m_context.program, m_function, id) == m_context.program.strings.emptyString(); }
    BlockID siteOf(const StringWebEntry& entry) const { return entry.concatenation ? m_useLists.blockOf(entry.concatenation) : entry.edge->from; }

    void planConcatenation(uint32_t web, ValueID);
    std::optional<NestedWeb> findNestedWeb(OperandSite part) const;
    bool hasOnlyPartsFrom(ValueID concatenation, uint32_t firstOperandIndex) const;
    bool canMoveToEntries(const StringWeb&, std::span<const ValueID> front);
    bool isDefinedBefore(ValueID, const StringWeb&);
    bool buildsOnlyStringsUntil(ValueID concatenation, const StringWeb&);
    bool buildsOnlyStrings(BlockID, uint32_t firstIndex, uint32_t endIndex);
    bool buildsOnlyStringsToBlockEnd(BlockID, uint32_t firstIndex, const std::optional<Target>& handler);
    bool needsBlocksOnEntryEdges(const StringWeb&);
    bool isUsedAfterEntries(ValueID state, OperandSite readOfState, const StringWeb&) const;

    void applyMerges();
    void prependToEntry(std::span<const ValueID> front, const StringWebEntry&);
    bool canTakeMoreParts(ValueID concatenation, size_t numberOfParts);
    void erase(ValueID);
    void eraseMarkedValues();

    FunctionPassContext& m_context;
    const StringBuilderPlan& m_plan;
    Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    const Dominators& m_dominators;
    IRBuilder m_builder;
    Vector<MergedConcatenation, 4> m_merges;
    Vector<Edge, 4> m_edgesToSplit;
    Vector<ValueID, 4> m_declinedConcatenations;
    BitVector m_outerWebs;
    BitVector m_nestedWebs;
    BitVector m_valuesToErase;
    BitVector m_blocksWithErasedValues;
};

NestedWebMerger::NestedWebMerger(FunctionPassContext& context, const StringBuilderPlan& plan)
    : m_context(context)
    , m_plan(plan)
    , m_function(context.function)
    , m_cfg(context.analyses.cfg())
    , m_useLists(context.analyses.useLists())
    , m_dominators(context.analyses.dominators())
    , m_builder(context.analyses.context(), context.function, context.types)
{
}

bool NestedWebMerger::run()
{
    std::span<const StringWeb> webs = m_plan.webs();
    for (uint32_t web = 0; web < webs.size(); ++web) {
        if (webs[web].decision != StringBuilderDeclineReason::Created)
            continue;
        for (ValueID member : webs[web].members) {
            if (m_function.at(member).opcode != Opcode::Parameter)
                planConcatenation(web, member);
        }
    }
    if (!m_edgesToSplit.isEmpty()) {
        for (Edge edge : m_edgesToSplit)
            m_builder.splitEdge(edge);
        return true;
    }
    if (m_merges.isEmpty()) {
        for (ValueID concatenation : m_declinedConcatenations)
            m_context.addDecline(Wire::DeclineKind::StringBuildersNotJoined, concatenation);
        return false;
    }
    applyMerges();
    return true;
}

// A web changes in one way in a round: its entries move, or its concatenations lose their fronts. The next round sees the other.
void NestedWebMerger::planConcatenation(uint32_t web, ValueID id)
{
    if (m_nestedWebs.get(web))
        return;

    const Value& concatenation = m_function.at(id);
    MergedConcatenation merge { id, { }, { concatenation.operand(stateOperandIndex) } };
    OperandSite readOfState { id, stateOperandIndex };
    for (uint32_t operandIndex = stateOperandIndex + 1; operandIndex < concatenation.numberOfOperands(); ++operandIndex) {
        OperandSite part { id, operandIndex };
        std::optional<NestedWeb> nested = findNestedWeb(part);
        if (!nested || m_outerWebs.get(*m_plan.webOf(nested->lastState))) {
            merge.remainingOperands.append(concatenation.operand(operandIndex));
            continue;
        }
        ValueID state = merge.remainingOperands[stateOperandIndex];
        bool canMerge = hasOnlyPartsFrom(id, operandIndex + 1) && canMoveToEntries(*nested->web, merge.remainingOperands.span()) && buildsOnlyStringsUntil(id, *nested->web);
        if (canMerge && needsBlocksOnEntryEdges(*nested->web))
            return;
        if (!canMerge || (m_plan.webOf(state) && isUsedAfterEntries(state, readOfState, *nested->web))) {
            if (nested->web->decision != StringBuilderDeclineReason::Created)
                m_declinedConcatenations.append(id);
            merge.remainingOperands.append(concatenation.operand(operandIndex));
            continue;
        }
        m_nestedWebs.set(*m_plan.webOf(nested->lastState));
        readOfState = nested->readOfLastState(part);
        nested->front = std::exchange(merge.remainingOperands, { nested->lastState });
        merge.nestedWebs.append(WTF::move(*nested));
    }
    if (merge.nestedWebs.isEmpty())
        return;
    m_outerWebs.set(web);
    m_merges.append(WTF::move(merge));
}

std::optional<NestedWeb> NestedWebMerger::findNestedWeb(OperandSite part) const
{
    NestedWeb nested;
    nested.lastState = m_function.at(part.user).operand(part.operandIndex);
    while (m_function.at(nested.lastState).opcode == Opcode::Upcast && m_useLists.useCount(nested.lastState) == 1) {
        nested.casts.append(nested.lastState);
        nested.lastState = m_function.at(nested.lastState).valueOperand();
    }

    std::optional<uint32_t> web = m_plan.webOf(nested.lastState);
    if (!web || web == m_plan.webOf(part.user))
        return std::nullopt;

    nested.web = &m_plan.webs()[*web];
    if (nested.web->reads.size() != 1 || nested.web->entries.isEmpty())
        return std::nullopt;
    const StringStateRead& read = nested.web->reads.first();
    OperandSite expected = nested.readOfLastState(part);
    if (read.use.kind != Use::Kind::Operand || read.use.user != expected.user || read.use.operandIndex != expected.operandIndex || !read.endsWeb)
        return std::nullopt;
    return nested;
}

// A concatenation converts its operands before it joins them. The code that the conversion of an object runs would come after the front is written.
bool NestedWebMerger::hasOnlyPartsFrom(ValueID concatenation, uint32_t firstOperandIndex) const
{
    const Value& value = m_function.at(concatenation);
    for (uint32_t operandIndex = firstOperandIndex; operandIndex < value.numberOfOperands(); ++operandIndex) {
        if (!isStringBuilderPart(typeOf(value.operand(operandIndex))))
            return false;
    }
    return true;
}

bool NestedWebMerger::canMoveToEntries(const StringWeb& web, std::span<const ValueID> front)
{
    for (const StringWebEntry& entry : web.entries) {
        if ((entry.edge && entry.edge->isHandlerEdge()) || !isStringBuilderPart(typeOf(entry.value)))
            return false;
        if (entry.concatenation && !hasOnlyPartsFrom(entry.concatenation, stateOperandIndex + 1))
            return false;
    }
    for (ValueID part : front) {
        if (!isStringBuilderPart(typeOf(part)) || m_function.at(part).opcode == Opcode::Unspecified)
            return false;
        if (!isRematerializable(m_function.at(part)) && !isDefinedBefore(part, web))
            return false;
    }
    return true;
}

// A value that is defined before every state of a web is not defined a second time while the web lives, so each entry sees what the concatenation would have seen.
bool NestedWebMerger::isDefinedBefore(ValueID value, const StringWeb& web)
{
    BlockID block = m_useLists.blockOf(value);
    for (ValueID member : web.members) {
        ValueLocation location = m_useLists.locationOf(member);
        if (location.placement == ValuePlacement::Parameter) {
            if (location.block == block || !m_dominators.dominates(block, location.block))
                return false;
        } else if (!m_context.analyses.dominates(value, ProgramPoint { location.block, location.index }))
            return false;
    }
    for (const StringWebEntry& entry : web.entries) {
        if (entry.edge && !m_context.analyses.dominates(value, pointBeforeTerminatorOf(m_function, entry.edge->from)))
            return false;
    }
    return true;
}

// The front is written at the entries, so a text that grows too long throws there and not at the concatenation.
bool NestedWebMerger::buildsOnlyStringsUntil(ValueID concatenation, const StringWeb& web)
{
    if (!web.discardedEdges.isEmpty() || !web.discardedAfter.isEmpty())
        return false;

    ValueLocation end = m_useLists.locationOf(concatenation);
    const std::optional<Target>& handler = m_function.at(end.block).handler;
    BitVector barrier(m_function.blocks.size());
    barrier.set(end.block.index());
    if (handler)
        barrier.set(handler->block.index());

    BitVector blocksBetween(m_function.blocks.size());
    bool entersBlockOfConcatenation = false;
    for (const StringWebEntry& entry : web.entries) {
        BlockID site = siteOf(entry);
        uint32_t firstIndex = entry.concatenation ? m_useLists.locationOf(entry.concatenation).index + 1 : m_function.at(site).values.size();
        if (site == end.block) {
            if (firstIndex > end.index || !buildsOnlyStrings(site, firstIndex, end.index))
                return false;
            continue;
        }
        if (!buildsOnlyStringsToBlockEnd(site, firstIndex, handler))
            return false;
        BitVector reachedBlocks = m_cfg.blocksReachableFrom(site, barrier);
        if (!m_cfg.isInCycleAvoiding(site, end.block))
            reachedBlocks.clear(site.index());
        blocksBetween.merge(reachedBlocks);
        entersBlockOfConcatenation = true;
    }
    for (size_t index : blocksBetween) {
        if (!buildsOnlyStringsToBlockEnd(BlockID(index), 0, handler))
            return false;
    }
    return !entersBlockOfConcatenation || buildsOnlyStrings(end.block, 0, end.index);
}

bool NestedWebMerger::buildsOnlyStringsToBlockEnd(BlockID block, uint32_t firstIndex, const std::optional<Target>& handler)
{
    return m_function.at(block).handler == handler
        && buildsOnlyStrings(block, firstIndex, m_function.at(block).values.size())
        && isUnobservable(m_context.analyses.effectsOfTerminator(block).onAnyEdge());
}

bool NestedWebMerger::buildsOnlyStrings(BlockID block, uint32_t firstIndex, uint32_t endIndex)
{
    for (ValueID id : m_function.at(block).values.subspan(firstIndex, endIndex - firstIndex)) {
        if (isUnobservable(m_context.analyses.effectsOf(id).onAnyEdge()))
            continue;
        // To convert a primitive and to join primitives can only run out of memory, which is the error of a text that grows too long.
        const Value& value = m_function.at(id);
        bool createsStringOfPrimitives = value.opcode == Opcode::ToString || isStringConcatenation(m_context.program, m_function, value);
        value.forEachOperand([&](ValueID part, uint32_t) {
            createsStringOfPrimitives &= isStringBuilderPart(typeOf(part));
        });
        if (!createsStringOfPrimitives)
            return false;
    }
    return true;
}

// Each entry takes the state of the outer web. Whatever reads that state later would need a second copy of the text.
bool NestedWebMerger::isUsedAfterEntries(ValueID state, OperandSite readOfState, const StringWeb& web) const
{
    BitVector blocksWithUses(m_function.blocks.size());
    Vector<BlockID, 8> sites;
    for (const StringWebEntry& entry : web.entries) {
        if (blocksWithUses.set(siteOf(entry).index()))
            return true;
        sites.append(siteOf(entry));
    }
    for (const Use& use : m_useLists.usesOf(state)) {
        if (use.kind == Use::Kind::Operand && use.user == readOfState.user && use.operandIndex == readOfState.operandIndex)
            continue;
        if (sites.contains(use.block))
            return true;
        blocksWithUses.set(use.block.index());
    }

    BlockID definition = m_useLists.blockOf(state);
    BitVector barrier(m_function.blocks.size());
    barrier.set(definition.index());
    for (BlockID site : sites) {
        if (m_cfg.isInCycleAvoiding(site, definition))
            return true;
        BitVector reachedUses = m_cfg.blocksReachableFrom(site, barrier);
        reachedUses.clear(site.index());
        reachedUses.filter(blocksWithUses);
        if (!reachedUses.isEmpty())
            return true;
    }
    return false;
}

// Code for an entry stands at the end of the block the edge leaves. The next plan names the blocks on these edges.
bool NestedWebMerger::needsBlocksOnEntryEdges(const StringWeb& web)
{
    bool needsBlocks = false;
    for (const StringWebEntry& entry : web.entries) {
        if (!entry.edge || m_function.at(entry.edge->from).terminator.kind == TerminatorKind::Jump)
            continue;
        needsBlocks = true;
        if (!m_edgesToSplit.contains(*entry.edge))
            m_edgesToSplit.append(*entry.edge);
    }
    return needsBlocks;
}

void NestedWebMerger::applyMerges()
{
    ValueSubstitution substitution(m_function);
    size_t numberOfNestedWebs = 0;
    for (const MergedConcatenation& merge : m_merges) {
        for (const NestedWeb& nested : merge.nestedWebs) {
            for (const StringWebEntry& entry : nested.web->entries) {
                // No use of an Unspecified value is reached, and a concatenation at the entry would be one that is.
                if (m_function.at(entry.value).opcode != Opcode::Unspecified)
                    prependToEntry(nested.front.span(), entry);
            }
            for (ValueID member : nested.web->members)
                retypeAsString(m_function.edit(member));
            for (ValueID cast : nested.casts)
                erase(cast);
        }
        numberOfNestedWebs += merge.nestedWebs.size();

        if (merge.remainingOperands.size() > 1) {
            m_function.edit(merge.concatenation, ValueEdit::Definition).setVariadicOperands(merge.remainingOperands.span());
            continue;
        }
        substitution.replace(merge.concatenation, merge.remainingOperands.first());
        erase(merge.concatenation);
    }
    m_builder.applyInsertions();
    eraseMarkedValues();
    substitution.apply(m_function, nullptr);
    m_context.build.statistics().count(StatisticID::StringBuildersJoined, numberOfNestedWebs);
}

void NestedWebMerger::prependToEntry(std::span<const ValueID> front, const StringWebEntry& entry)
{
    ProgramPoint point = entry.concatenation ? m_useLists.pointOf(entry.concatenation) : pointBeforeTerminatorOf(m_function, entry.edge->from);
    m_builder.setOrigin(entry.concatenation ? m_function.at(entry.concatenation).origin() : m_function.at(entry.edge->from).terminator.origin());

    Vector<ValueID, 8> parts;
    for (ValueID part : front)
        parts.append(isRematerializable(m_function.at(part)) ? m_builder.insertBefore(point, Value(m_function.at(part))) : part);
    if (!isEmptyString(entry.value))
        parts.append(entry.value);

    if (entry.concatenation && canTakeMoreParts(entry.concatenation, parts.size())) {
        Value& concatenation = m_function.edit(entry.concatenation);
        for (uint32_t operandIndex = stateOperandIndex + 1; operandIndex < concatenation.numberOfOperands(); ++operandIndex)
            parts.append(concatenation.operand(operandIndex));
        concatenation.become(Opcode::Strcat);
        concatenation.setVariadicOperands(parts.span());
        return;
    }

    ValueID state = parts.first();
    if (parts.size() > 1 || !typeOf(state).isAnyString()) {
        Value concatenation = IRBuilder::make(Opcode::Strcat);
        concatenation.setVariadicOperands(parts.span());
        state = m_builder.insertBefore(point, WTF::move(concatenation));
    }

    if (entry.concatenation)
        m_function.edit(entry.concatenation, ValueEdit::Definition).setOperand(stateOperandIndex, state);
    else
        editTarget(m_function, *entry.edge).arguments[m_useLists.locationOf(entry.parameter).index] = state;
}

bool NestedWebMerger::canTakeMoreParts(ValueID concatenation, size_t numberOfParts)
{
    return !m_context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfParts + m_function.at(concatenation).numberOfOperands() - 1, concatenation);
}

void NestedWebMerger::erase(ValueID value)
{
    m_valuesToErase.set(value.index());
    m_blocksWithErasedValues.set(m_useLists.blockOf(value).index());
}

void NestedWebMerger::eraseMarkedValues()
{
    for (size_t blockIndex : m_blocksWithErasedValues) {
        BlockID block(blockIndex);
        const BlockValues& values = m_function.at(block).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (m_valuesToErase.get(values[index].index()))
                erasedIndexes.set(index);
        }
        m_builder.eraseValues(block, erasedIndexes);
    }
}

} // anonymous namespace

bool mergeNestedStringWebs(FunctionPassContext& context, const StringBuilderPlan& plan)
{
    return NestedWebMerger(context, plan).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
