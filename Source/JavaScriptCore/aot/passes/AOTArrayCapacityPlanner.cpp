/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayCapacities.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static const NaturalLoop& loopOf(FunctionAnalyses& analyses, const AppendSite& site)
{
    return *analyses.naturalLoops().innerMostLoopOf(site.block);
}

BitVector blocksRarerThanHeader(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());
    BitVector blocks;
    for (unsigned i = 0; i < loop.size(); ++i) {
        if (frequencyOfLoop < frequencies.classOf(loop.at(i)))
            blocks.set(loop.at(i).index());
    }
    return blocks;
}

static bool isAppendedArray(const Value& append, uint32_t operandIndex)
{
    if (append.opcode == Opcode::CallBuiltin)
        return builtinOperandAt(append, operandIndex).isReceiver();
    return isWrittenOperand(append.opcode, operandIndex);
}

static bool canMeasure(FunctionAnalyses& analyses, const PlannedLiteral& literal, const AppendSite& site)
{
    const Value& append = analyses.function().at(site.value);
    RELEASE_ASSERT(append.opcode == Opcode::AppendElements);
    if (!analyses.dominates(append.sourceOperand(), literal.point))
        return false;
    ValueID guard = append.guardOperand();
    return !guard || (analyses.dominates(guard, literal.point) && analyses.isGuardAvailable(guard, literal.point));
}

bool CapacityPlan::tryAdd(const CapacityTerm& term)
{
    switch (term.kind) {
    case CapacityTermKind::Unknown:
        return false;
    case CapacityTermKind::Constant:
        constant += term.constant;
        return true;
    case CapacityTermKind::Integer:
        added.append(term.value);
        return true;
    case CapacityTermKind::Number:
        roundedUp.append(term.value);
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CapacityPlan::trySubtract(const CapacityTerm& term)
{
    switch (term.kind) {
    case CapacityTermKind::Constant:
        constant -= term.constant;
        return true;
    case CapacityTermKind::Integer:
        subtracted.append(term.value);
        return true;
    case CapacityTermKind::Unknown:
    case CapacityTermKind::Number:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ArrayCapacityPlanner::ArrayCapacityPlanner(FunctionPassContext& context)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
{
}

Vector<CapacityPlan, 4> ArrayCapacityPlanner::collectPlans()
{
    Vector<ValueID, 8> candidates;
    bool hasAppend = false;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (isCandidate(value))
                candidates.append(id);
            else if (!hasAppend)
                hasAppend = appendKindOf(value).has_value();
        }
    }

    Vector<CapacityPlan, 4> plans;
    if (!hasAppend)
        return plans;
    for (ValueID candidate : candidates) {
        if (std::optional<CapacityPlan> planned = plan(candidate))
            plans.append(WTF::move(*planned));
    }
    return plans;
}

bool ArrayCapacityPlanner::isCandidate(const Value& value) const
{
    if (value.opcode != Opcode::NewArray || !value.holes().empty() || value.capacityOperand())
        return false;
    uint64_t maxLength = std::min(m_context.limit(LimitID::MaxInlineArrayLiteralLength), m_context.limit(LimitID::MaxArrayLiteralBatch));
    return value.elementsOperands().size() <= maxLength;
}

std::optional<CapacityPlan> ArrayCapacityPlanner::plan(ValueID id)
{
    std::optional<ObjectID> object = m_analyses.escape().objectDefinedBy(id);
    if (!object)
        return std::nullopt;
    Vector<AppendSite, 4> sites = collectSites(*object);
    if (sites.isEmpty())
        return std::nullopt;

    ValueLocation location = m_analyses.useLists().locationOf(id);
    PlannedLiteral literal { id, { location.block, location.index }, m_analyses.naturalLoops().innerMostLoopOf(location.block) };
    BlockChain chain = chainFrom(location.block, literal.loop);

    int64_t numberOfElements = m_function.at(id).elementsOperands().size();
    CapacityPlan result;
    result.literal = id;
    result.constant = numberOfElements;

    int64_t numberOfConditionalElements = 0;
    Vector<AppendSite, 2> loopSites;
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    for (const AppendSite& site : sites) {
        if (frequencies.classOf(location.block) < frequencies.classOf(site.block))
            continue;
        switch (executionOf(literal, chain, site)) {
        case SiteExecution::Unknown:
            return std::nullopt;
        case SiteExecution::Always:
            if (site.kind == AppendKind::StoreAtIndex)
                return std::nullopt;
            if (site.kind == AppendKind::List && (!canMeasure(m_analyses, literal, site) || !result.tryAdd({ CapacityTermKind::Integer, site.value })))
                return std::nullopt;
            result.constant += site.count;
            break;
        case SiteExecution::Conditional:
            if (site.kind != AppendKind::Elements)
                return std::nullopt;
            numberOfConditionalElements += site.count;
            break;
        case SiteExecution::InNestedLoop:
            if (site.kind == AppendKind::List)
                return std::nullopt;
            loopSites.append(site);
            break;
        }
    }

    if (!loopSites.isEmpty()) {
        if (!tryAddLoopSites(result, literal, loopSites))
            return std::nullopt;
    } else if (!result.hasRunTimeTerm() && numberOfConditionalElements) {
        result.constant += numberOfConditionalElements;
        if (result.constant - numberOfElements > 1)
            return std::nullopt;
    }

    if (result.hasRunTimeTerm())
        return result;
    if (result.constant <= numberOfElements || m_context.exceeds(LimitID::MaxPlannedArrayCapacity, static_cast<uint64_t>(result.constant), id))
        return std::nullopt;
    return result;
}

std::optional<AppendKind> ArrayCapacityPlanner::appendKindOf(const Value& value) const
{
    switch (value.opcode) {
    case Opcode::AppendElement:
    case Opcode::AppendHole:
        return AppendKind::Elements;
    case Opcode::CallBuiltin:
        if (inlineImplementationOf(m_context.program, value) != InlineBuiltin::ArrayPush)
            return std::nullopt;
        return AppendKind::Elements;
    case Opcode::AppendElements:
        return AppendKind::List;
    case Opcode::ElemSet:
    case Opcode::PutArrayElement:
        return AppendKind::StoreAtIndex;
    default:
        return std::nullopt;
    }
}

Vector<AppendSite, 4> ArrayCapacityPlanner::collectSites(ObjectID object)
{
    Vector<AppendSite, 4> sites;
    for (const ObjectUse& objectUse : m_analyses.escape().usesOf(object)) {
        const Use& use = objectUse.use;
        if (use.kind != Use::Kind::Operand)
            continue;
        const Value& user = m_function.at(use.user);
        std::optional<AppendKind> kind = appendKindOf(user);
        if (!kind || !isAppendedArray(user, use.operandIndex))
            continue;

        AppendSite site { *kind, use.user, use.block, 0, { } };
        switch (*kind) {
        case AppendKind::Elements:
            site.count = user.opcode == Opcode::CallBuiltin ? static_cast<unsigned>(builtinCallOf(m_context.program, user).arguments.size()) : 1;
            break;
        case AppendKind::List:
            site.operand = user.sourceOperand();
            break;
        case AppendKind::StoreAtIndex:
            site.operand = user.opcode == Opcode::ElemSet ? user.propertyOperand() : user.positionOperand();
            break;
        }
        sites.append(site);
    }
    return sites;
}

SiteExecution ArrayCapacityPlanner::executionOf(const PlannedLiteral& literal, const BlockChain& chain, const AppendSite& site)
{
    const NaturalLoops& loops = m_analyses.naturalLoops();
    const NaturalLoop* loop = loops.innerMostLoopOf(site.block);
    if (!loop && m_analyses.blockCycles().isInCycle(site.block))
        return SiteExecution::Unknown;

    if (loop == literal.loop) {
        if (!chain.contains(site.block))
            return SiteExecution::Conditional;
        if (site.block == literal.point.block && m_analyses.useLists().locationOf(site.value).index < literal.point.index)
            return SiteExecution::Conditional;
        return SiteExecution::Always;
    }

    if (loop && loops.innerMostOuterLoop(*loop) == literal.loop)
        return SiteExecution::InNestedLoop;
    return SiteExecution::Unknown;
}

BlockChain ArrayCapacityPlanner::chainFrom(BlockID first, const NaturalLoop* loop)
{
    const NaturalLoops& loops = m_analyses.naturalLoops();
    BlockChain chain;
    BlockID block = first;
    while (block && loops.innerMostLoopOf(block) == loop && chain.members.add(block.index()))
        block = chainSuccessorOf(block);
    return chain;
}

BlockID ArrayCapacityPlanner::chainSuccessorOf(BlockID block) const
{
    const Terminator& terminator = m_function.at(block).terminator;
    switch (terminator.kind) {
    case TerminatorKind::Jump:
        return terminator.next().block;
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ArrayCapacityPlanner::tryAddLoopSites(CapacityPlan& plan, const PlannedLiteral& literal, Vector<AppendSite, 2>& sites)
{
    const CFG& cfg = m_analyses.cfg();
    std::sort(sites.begin(), sites.end(), [&](const AppendSite& a, const AppendSite& b) {
        return cfg.reversePostOrderIndex(loopOf(m_analyses, a).header()) < cfg.reversePostOrderIndex(loopOf(m_analyses, b).header());
    });

    for (size_t i = 0; i < sites.size(); ++i) {
        if (i && &loopOf(m_analyses, sites[i]) == &loopOf(m_analyses, sites[i - 1]))
            return false;
        if (!appendsOncePerIteration(literal, sites[i]))
            return false;
    }

    const NaturalLoop& firstLoop = loopOf(m_analyses, sites[0]);
    if (sites.size() > 1 && !areAlternativesTo(firstLoop.header(), literal, sites.subspan(1)))
        return false;

    TripCount count = tripCount(literal, firstLoop, sites[0]);
    plan.constant += count.adjustment;
    return plan.tryAdd(count.added) && plan.trySubtract(count.subtracted);
}

bool ArrayCapacityPlanner::appendsOncePerIteration(const PlannedLiteral& literal, const AppendSite& site)
{
    if (site.kind == AppendKind::Elements && site.count != 1)
        return false;

    const NaturalLoop& loop = loopOf(m_analyses, site);
    return m_analyses.dominators().dominates(literal.point.block, loop.header()) && runsOncePerIteration(loop, site.block);
}

bool ArrayCapacityPlanner::runsOncePerIteration(const NaturalLoop& loop, BlockID block)
{
    if (block == loop.header())
        return true;

    const CFG& cfg = m_analyses.cfg();
    const NaturalLoops& loops = m_analyses.naturalLoops();
    BitVector barrier = blocksRarerThanHeader(m_analyses, loop);
    if (barrier.get(block.index()))
        return false;
    barrier.set(block.index());
    BitVector reachedWithoutBlock = cfg.blocksReachableFrom(loop.header(), barrier);
    for (Edge edge : cfg.incomingEdges(loop.header())) {
        if (loops.belongsTo(edge.from, loop) && reachedWithoutBlock.get(edge.from.index()))
            return false;
    }
    return true;
}

bool ArrayCapacityPlanner::areAlternativesTo(BlockID header, const PlannedLiteral& literal, std::span<const AppendSite> others)
{
    const CFG& cfg = m_analyses.cfg();
    BitVector nextAllocation;
    nextAllocation.set(literal.point.block.index());
    BitVector reachableFromHeader = cfg.blocksReachableFrom(header, nextAllocation);
    for (const AppendSite& other : others) {
        BlockID otherHeader = loopOf(m_analyses, other).header();
        if (reachableFromHeader.get(otherHeader.index()) || cfg.blocksReachableFrom(otherHeader, nextAllocation).get(header.index()))
            return false;
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
