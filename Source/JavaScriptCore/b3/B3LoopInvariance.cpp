/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LoopInvariance.h"

#if ENABLE(B3_JIT)

#include "B3Dominators.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3PhiChildren.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

struct LoopInvariance::ExecutedValueSearch {
    void add(Value* value)
    {
        if (seen.add(value))
            candidates.append(value);
    }

    IndexSet<Value*> seen;
    Vector<Value*, 32> candidates;
    IndexSet<Value*> loadsNotOverwritten;
};

struct LoopInvariance::PeelingAttempt {
    UncheckedKeyHashMap<Value*, FirstIterationValue> values;
    Vector<HoistStep, 16> steps;
    unsigned numberOfVisitedValues { 0 };
};

LoopInvariance::LoopInvariance(Procedure& proc, NaturalLoops& loops, const HotRegion& region, PhiChildren& phiChildren, PhiResolution resolution)
    : m_proc(proc)
    , m_loops(loops)
    , m_region(region)
    , m_phiChildren(phiChildren)
    , m_resolution(resolution)
{
}

std::optional<GaveUp> LoopInvariance::scanRegion()
{
    m_registerBudget = allocatableRegisters(m_proc);
    for (BasicBlock* block : m_region.blocks) {
        for (Value* value : *block) {
            Effects effects = value->effects();
            if (!canCloneForLoop(value) || effects.fence)
                return GaveUp(GaveUpReason::UncloneableValue);
            m_numberOfClonedValues += emitsInstructions(value);
            m_registerBudget.restrictTo(registersPreservedBy(m_proc, value));
            effects.writes.addTo(m_writes);
            m_writesPinned |= effects.writesPinned;
        }
    }
    if (m_numberOfClonedValues > m_proc.limit(Limit::MaxValuesPerHotRegion)) {
        m_proc.didReachLimit(Limit::MaxValuesPerHotRegion, m_region.header->at(0)->origin());
        return GaveUp::atLimit(Limit::MaxValuesPerHotRegion);
    }
    collectExecutedValues();
    return std::nullopt;
}

bool LoopInvariance::isInvariant(Value* value) const
{
    return !isInLoop(value) || m_plan.hoistedValues.contains(value);
}

bool LoopInvariance::hasInvariantChildren(Value* value) const
{
    for (Value* child : value->children()) {
        if (!isInvariant(child))
            return false;
    }
    return true;
}

bool LoopInvariance::isInSiblingLoop(BasicBlock* block) const
{
    const NaturalLoop* inner = m_loops.innerMostLoopOf(block);
    return inner && !m_loops.belongsTo(m_region.header, *inner);
}

bool LoopInvariance::isAvailableInPreHeader(Value* value) const
{
    return m_plan.isHoisted(value) || !isInLoop(value);
}

void LoopInvariance::addChildrenBeforeLoop(Value* value, ExecutedValueSearch& search) const
{
    for (Value* child : value->children()) {
        if (emitsInstructions(child) && !isInLoop(child))
            search.add(child);
    }
}

void LoopInvariance::scanBlocksBeforeLoop(ExecutedValueSearch& search) const
{
    RangeSet<HeapRange> writesSince;
    bool fenceSince = false;
    unsigned numberOfScannedBlocks = 0;
    unsigned numberOfScannedValues = 0;
    for (BasicBlock* block = m_region.preHeader;; block = block->predecessor(0)) {
        if (numberOfScannedBlocks++ == m_proc.limit(Limit::MaxBlocksScannedBeforeLoop)) {
            m_proc.didReachLimit(Limit::MaxBlocksScannedBeforeLoop, m_region.header->at(0)->origin());
            return;
        }
        for (unsigned index = block->size(); index--;) {
            if (numberOfScannedValues++ == m_proc.limit(Limit::MaxValuesScannedForExecutedLoad)) {
                m_proc.didReachLimit(Limit::MaxValuesScannedForExecutedLoad, m_region.header->at(0)->origin());
                return;
            }
            Value* value = block->at(index);
            if (emitsInstructions(value) && value->type() != Void)
                search.add(value);
            Effects effects = value->effects();
            auto* load = value->as<MemoryValue>();
            if (load && load->isLoad() && !fenceSince && !effects.reads.overlaps(writesSince))
                search.loadsNotOverwritten.add(value);
            effects.writes.addTo(writesSince);
            fenceSince |= effects.fence;
        }
        if (block->numPredecessors() != 1)
            return;
    }
}

void LoopInvariance::collectExecutedValues()
{
    ExecutedValueSearch search;
    for (BasicBlock* block : m_region.blocks) {
        for (Value* value : *block)
            addChildrenBeforeLoop(value, search);
    }
    scanBlocksBeforeLoop(search);
    for (unsigned i = 0; i < search.candidates.size(); ++i) {
        Value* value = search.candidates[i];
        if (isInSiblingLoop(value->owner))
            continue;
        Effects effects = value->effects();
        if (effects.mustExecute() || effects.readsLocalState || effects.readsPinned)
            continue;
        if (effects.reads && effects.readsMutability != Mutability::Immutable && !search.loadsNotOverwritten.contains(value))
            continue;
        if (m_executedValues.size() == m_proc.limit(Limit::MaxValuesScannedForExecutedLoad)) {
            m_proc.didReachLimit(Limit::MaxValuesScannedForExecutedLoad, m_region.header->at(0)->origin());
            return;
        }
        m_executedValues.append(value);
        addChildrenBeforeLoop(value, search);
    }
}

Value* LoopInvariance::canonicalValue(Value* value) const
{
    Value* repeated = repetitions().get(value);
    return repeated ? repeated : value;
}

bool LoopInvariance::isSameValue(Value* a, Value* b) const
{
    a = canonicalValue(a);
    b = canonicalValue(b);
    return a == b || (a->isConstant() && a->key() == b->key());
}

Value* LoopInvariance::findEquivalentExecutedLoad(Value* value) const
{
    auto* load = value->as<MemoryValue>();
    if (!load || !load->isLoad())
        return nullptr;
    for (Value* executed : m_executedValues) {
        auto* other = executed->as<MemoryValue>();
        if (!other || other->opcode() != load->opcode() || other->type() != load->type() || other->offset() != load->offset())
            continue;
        if (canonicalValue(other->lastChild()) != canonicalValue(load->lastChild()))
            continue;
        return executed;
    }
    return nullptr;
}

Value* LoopInvariance::canonicalChild(Value* child)
{
    child = canonicalValue(child);
    if (!child->isConstant())
        return child;
    return m_canonicalConstants.add(child->key(), child).iterator->value;
}

ValueKey LoopInvariance::canonicalKey(Value* value)
{
    ValueKey key = value->key();
    if (!key || value->numChildren() > 3 || key.simdInfo() != SIMDInfo { })
        return ValueKey();
    v128_t bits = key.vectorValue();
    for (unsigned i = 0; i < value->numChildren(); ++i) {
        if (key.childIndex(i) != value->child(i)->index())
            return ValueKey();
        bits.u32x4[i] = canonicalChild(value->child(i))->index();
    }
    return ValueKey(key.kind(), key.type(), bits);
}

bool LoopInvariance::isSameComputation(Value* value, const ValueKey& key, Value* executed)
{
    auto* patchpoint = value->as<PatchpointValue>();
    if (!patchpoint)
        return key && canonicalKey(executed) == key;
    return isSamePureCall(patchpoint, executed->as<PatchpointValue>(), [&](Value* child) {
        return canonicalChild(child);
    });
}

void LoopInvariance::recordRepeatedComputation(Value* value)
{
    ValueKey key = canonicalKey(value);
    auto* patchpoint = value->as<PatchpointValue>();
    if (!key && !(patchpoint && patchpoint->pureFunction))
        return;
    for (Value* executed : m_executedValues) {
        if (executed->kind() != value->kind() || executed->type() != value->type() || m_plan.repeatedValues.contains(executed))
            continue;
        if (!isSameComputation(value, key, executed))
            continue;
        m_plan.repeatedValues.add(value, executed);
        return;
    }
}

Value* LoopInvariance::findSelectedOperand(Value* value) const
{
    if (value->opcode() != Select)
        return nullptr;
    auto known = m_hoistedGuardConditions.find(canonicalValue(value->child(0)));
    if (known == m_hoistedGuardConditions.end())
        return nullptr;
    Value* selected = canonicalValue(value->child(known->value ? 1 : 2));
    return isAvailableInPreHeader(selected) ? selected : nullptr;
}

Value* LoopInvariance::singleInputInFastVersion(Value* phi) const
{
    Value* result = nullptr;
    for (UpsilonValue* upsilon : m_phiChildren[phi]) {
        if (isInLoop(upsilon) && !m_region.contains(upsilon->owner))
            continue;
        Value* child = canonicalValue(upsilon->child(0));
        if (result && result != child)
            return nullptr;
        result = child;
    }
    return result;
}

bool LoopInvariance::isBetweenExecutedLoads(Value* value) const
{
    auto* load = value->as<MemoryValue>();
    if (!load || !load->isLoad())
        return false;
    int64_t begin = load->offset();
    int64_t end = begin + load->accessByteSize();
    std::optional<int64_t> beginOfLower;
    std::optional<int64_t> endOfUpper;
    for (Value* executed : m_executedValues) {
        auto* other = executed->as<MemoryValue>();
        if (!other || !other->isLoad() || m_plan.repeatedValues.contains(executed) || canonicalValue(other->lastChild()) != canonicalValue(load->lastChild()))
            continue;
        int64_t beginOfOther = other->offset();
        int64_t endOfOther = beginOfOther + other->accessByteSize();
        if (beginOfOther <= begin && (!beginOfLower || beginOfOther > *beginOfLower))
            beginOfLower = beginOfOther;
        if (endOfOther >= end && (!endOfUpper || endOfOther < *endOfUpper))
            endOfUpper = endOfOther;
    }
    return beginOfLower && endOfUpper && *endOfUpper - *beginOfLower < Limits::minPageSize;
}

Value* LoopInvariance::findEnteringValue(Value* headerPhi) const
{
    Value* entering = nullptr;
    for (UpsilonValue* upsilon : m_phiChildren[headerPhi]) {
        if (isInLoop(upsilon))
            continue;
        if (entering)
            return nullptr;
        entering = upsilon->child(0);
    }
    return entering;
}

bool LoopInvariance::matchesValueBeforeLoop(Value* inside, Value* before, InvalidationSet& walked) const
{
    while (inside != before) {
        if (!m_region.contains(inside->owner) || isInLoop(before) || inside->kind() != before->kind() || inside->type() != before->type())
            return false;
        if (inside->opcode() != ZExt32 && inside->opcode() != SExt32) {
            auto* load = inside->as<MemoryValue>();
            if (!load || !load->isLoad() || load->offset() != before->as<MemoryValue>()->offset() || !m_executedValues.contains(before))
                return false;
            Effects effects = load->effects();
            if (effects.readsMutability != Mutability::Immutable && effects.reads.overlaps(m_writes))
                return false;
            walked.add(effects);
        }
        inside = inside->lastChild();
        before = before->lastChild();
    }
    return !isInLoop(inside);
}

Value* LoopInvariance::invariantValueOfHeaderPhi(Value* phi, InvalidationSet& walked) const
{
    Value* entering = findEnteringValue(phi);
    if (!entering || entering->isConstant())
        return nullptr;
    for (UpsilonValue* upsilon : m_phiChildren[phi]) {
        if (isInLoop(upsilon) && !matchesValueBeforeLoop(upsilon->child(0), entering, walked))
            return nullptr;
    }
    return entering;
}

HoistDecision LoopInvariance::checkEffects(Value* value, Effects effects) const
{
    bool isTerminal = effects.terminal;
    effects.terminal = false;
    if (effects.mustExecute() || effects.readsLocalState || (!isTerminal && value->type() == Void))
        return HoistDecision::HasEffects;
    if (effects.readsPinned && m_writesPinned)
        return HoistDecision::Clobbered;
    if (effects.reads && effects.readsMutability != Mutability::Immutable && effects.reads.overlaps(m_writes))
        return HoistDecision::Clobbered;
    return HoistDecision::Hoist;
}

// What is known of a phi is known of whichever input it takes. In the fast version it stands for one of them, and the guard that keeps the others out is still in the loop.
bool LoopInvariance::isChosenByPendingGuard(Value* phi) const
{
    Dominators& dominators = m_proc.dominators();
    return std::ranges::any_of(m_pendingGuards, [&](const PendingGuard& pending) {
        return pending.likelySuccessor && dominators.dominates(pending.guard->owner, phi->owner) && !dominators.dominates(pending.likelySuccessor, phi->owner);
    });
}

bool LoopInvariance::readsThroughPhiChosenByPendingGuard(Value* value) const
{
    auto* memory = value->as<MemoryValue>();
    return memory && m_phisChosenByPendingGuards.contains(memory->lastChild()->foldIdentity());
}

HoistDecision LoopInvariance::canHoist(Value* value)
{
    if (!hasInvariantChildren(value))
        return HoistDecision::NotInvariant;
    Effects effects = value->effects();
    HoistDecision decision = checkEffects(value, effects);
    if (decision != HoistDecision::Hoist)
        return decision;
    if (!effects.controlDependent || m_pendingGuards.isEmpty())
        return HoistDecision::Hoist;
    if (!m_proc.effectsOf(value).controlDependent && !readsThroughPhiChosenByPendingGuard(value))
        return HoistDecision::HoistSpeculatively;
    if (findEquivalentExecutedLoad(value))
        return HoistDecision::Hoist;
    if (m_resolution == PhiResolution::Keep)
        return HoistDecision::ControlDependent;
    if (isBetweenExecutedLoads(value))
        return HoistDecision::Hoist;
    if (tryPeelPendingGuards())
        return HoistDecision::Hoist;
    return HoistDecision::ControlDependent;
}

void LoopInvariance::visit(Value* value)
{
    if (isLoopPoll(value))
        return;
    if (value->opcode() == Phi) {
        if (m_resolution == PhiResolution::Resolve)
            resolvePhi(value);
        return;
    }
    if (value->kind() == Kind(Check)) {
        visitCheck(value);
        return;
    }
    HoistDecision decision = canHoist(value);
    if (isHoist(decision)) {
        hoistValue(value, decision);
        return;
    }
    if (value->effects().exitsSideways) {
        m_pendingGuards.append(PendingGuard { value, nullptr });
        m_peelingFailed = true;
    }
}

void LoopInvariance::visitCheck(Value* check)
{
    Value* condition = check->child(0);
    if (isInvariant(condition)) {
        hoist(check, HoistStep::Kind::Check);
        m_plan.hoistsGuard = true;
        m_hoistedGuardConditions.add(canonicalValue(condition), false);
        return;
    }
    auto known = m_loopGuardConditions.add(canonicalValue(condition), false);
    if (!known.isNewEntry && !known.iterator->value) {
        m_plan.hoistedValues.add(check);
        ++m_plan.numberOfHoistedValues;
        return;
    }
    m_pendingGuards.append(PendingGuard { check, nullptr });
}

void LoopInvariance::resolvePhi(Value* phi)
{
    bool isHeaderPhi = phi->owner == m_region.header;
    InvalidationSet walked;
    Value* only = isHeaderPhi ? invariantValueOfHeaderPhi(phi, walked) : singleInputInFastVersion(phi);
    if (!only || !isAvailableInPreHeader(only))
        return;
    if (isHeaderPhi || m_speculativelyHoisted.contains(only))
        m_speculativelyHoisted.add(phi);
    if (!isHeaderPhi && isChosenByPendingGuard(phi))
        m_phisChosenByPendingGuards.add(phi);
    m_plan.repeatedValues.add(phi, only);
    hoist(phi, HoistStep::Kind::Value);
    m_plan.invalidation.merge(walked);
}

void LoopInvariance::hoistValue(Value* value, HoistDecision decision)
{
    if (Value* executed = findEquivalentExecutedLoad(value))
        repetitions().add(value, canonicalValue(executed));
    if (m_resolution == PhiResolution::Resolve) {
        bool isSpeculative = decision == HoistDecision::HoistSpeculatively;
        isSpeculative |= m_speculativelyHoisted.contains(canonicalValue(value));
        for (Value* child : value->children())
            isSpeculative |= m_speculativelyHoisted.contains(child);
        if (isSpeculative && m_speculativelyHoisted.add(value))
            m_plan.numberOfSpeculativeHoists += emitsInstructions(value);
        if (Value* selected = findSelectedOperand(value))
            m_plan.repeatedValues.add(value, selected);
        else
            recordRepeatedComputation(value);
    }
    hoist(value, HoistStep::Kind::Value);
}

void LoopInvariance::hoist(Value* value, HoistStep::Kind kind)
{
    m_plan.steps.append(HoistStep { kind, value });
    m_plan.hoistedValues.add(value);
    m_plan.numberOfHoistedValues += emitsInstructions(value);
    if (kind != HoistStep::Kind::Check) {
        m_executedValues.append(value);
        m_plan.invalidation.add(value->effects());
    }
    bool isLikelyPathOnly = value->opcode() == Phi;
    for (Value* child : value->children())
        isLikelyPathOnly |= m_likelyPathOnlyValues.contains(child);
    if (isLikelyPathOnly)
        m_likelyPathOnlyValues.add(value);
    else
        m_plan.valuesValidInExcursion.add(value);
}

void LoopInvariance::hoistGuard(Value* terminal, BasicBlock* likelySuccessor)
{
    hoist(terminal, HoistStep::Kind::Guard);
    m_plan.hoistsGuard = true;
    m_plan.likelySuccessors.add(terminal, likelySuccessor);
    if (terminal->opcode() == Branch)
        m_hoistedGuardConditions.add(canonicalValue(terminal->child(0)), likelySuccessor == terminal->owner->successorBlock(0));
}

LoopGuardKind LoopInvariance::addLoopGuard(Value* terminal, BasicBlock* likelySuccessor)
{
    if (terminal->opcode() == Branch) {
        bool isTaken = likelySuccessor == terminal->owner->successorBlock(0);
        auto known = m_loopGuardConditions.add(canonicalValue(terminal->child(0)), isTaken);
        if (!known.isNewEntry && known.iterator->value == isTaken) {
            m_plan.hoistedValues.add(terminal);
            m_plan.likelySuccessors.add(terminal, likelySuccessor);
            ++m_plan.numberOfHoistedValues;
            return LoopGuardKind::Redundant;
        }
    }
    m_pendingGuards.append(PendingGuard { terminal, likelySuccessor });
    return LoopGuardKind::Required;
}

LoopInvariance::FirstIterationValue LoopInvariance::stagePeeledCopy(Value* value, HoistStep::Kind kind, PeelingAttempt& attempt) const
{
    if (value->isConstant())
        return FirstIterationValue::Constant;
    if (isInvariant(value))
        return FirstIterationValue::NotConstant;
    if (auto staged = attempt.values.find(value); staged != attempt.values.end())
        return staged->value;
    if (attempt.numberOfVisitedValues++ == m_proc.limit(Limit::MaxSourcesOfLoopTest)) {
        m_proc.didReachLimit(Limit::MaxSourcesOfLoopTest, value->origin());
        return FirstIterationValue::Unknown;
    }

    FirstIterationValue result = FirstIterationValue::Constant;
    if (value->opcode() == Phi) {
        Value* entering = value->owner == m_region.header ? findEnteringValue(value) : nullptr;
        if (!entering)
            return FirstIterationValue::Unknown;
        if (!entering->isConstant())
            result = FirstIterationValue::NotConstant;
    } else if (kind == HoistStep::Kind::PeeledCheck) {
        result = stagePeeledCopy(value->child(0), HoistStep::Kind::PeeledValue, attempt);
        if (result == FirstIterationValue::Unknown)
            return result;
    } else {
        if (!m_region.contains(value->owner) || checkEffects(value, value->effects()) != HoistDecision::Hoist)
            return FirstIterationValue::Unknown;
        for (Value* child : value->children()) {
            switch (stagePeeledCopy(child, HoistStep::Kind::PeeledValue, attempt)) {
            case FirstIterationValue::Unknown:
                return FirstIterationValue::Unknown;
            case FirstIterationValue::Constant:
                break;
            case FirstIterationValue::NotConstant:
                result = FirstIterationValue::NotConstant;
                break;
            }
        }
    }
    attempt.values.add(value, result);
    attempt.steps.append(HoistStep { kind, value });
    return result;
}

bool LoopInvariance::tryPeelPendingGuards()
{
    if (m_peelingFailed)
        return false;
    PeelingAttempt attempt;
    attempt.values = m_peeledValues;
    for (const PendingGuard& guard : m_pendingGuards) {
        attempt.numberOfVisitedValues = 0;
        HoistStep::Kind kind = guard.guard->opcode() == Check ? HoistStep::Kind::PeeledCheck : HoistStep::Kind::PeeledGuard;
        if (stagePeeledCopy(guard.guard, kind, attempt) != FirstIterationValue::NotConstant) {
            m_peelingFailed = true;
            return false;
        }
    }
    for (const HoistStep& step : attempt.steps)
        m_plan.numberOfPeeledValues += emitsInstructions(step.value);
    m_plan.steps.appendVector(attempt.steps);
    for (const PendingGuard& guard : m_pendingGuards) {
        if (guard.likelySuccessor)
            m_plan.likelySuccessors.add(guard.guard, guard.likelySuccessor);
    }
    m_peeledValues = WTF::move(attempt.values);
    m_pendingGuards.clear();
    m_phisChosenByPendingGuards = { };
    return true;
}

unsigned LoopInvariance::countHoistsBeyondRegisters() const
{
    IndexSet<Value*> liveThroughLoop;
    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (BasicBlock* block : m_region.blocks) {
        for (Value* value : *block) {
            if (m_plan.isHoisted(value))
                continue;
            for (Value* child : value->children()) {
                if (!m_plan.isHoisted(child))
                    continue;
                Value* canonical = canonicalValue(child);
                if (!emitsInstructions(canonical) || !liveThroughLoop.add(canonical))
                    continue;
                if (canonical->resultBank() == GP)
                    ++numberOfGPRs;
                else
                    ++numberOfFPRs;
            }
        }
    }
    return numberOfGPRs - std::min(numberOfGPRs, m_registerBudget.gprs) + numberOfFPRs - std::min(numberOfFPRs, m_registerBudget.fprs);
}

HoistPlan LoopInvariance::takePlan()
{
    m_plan.numberOfHoistsBeyondRegisters = countHoistsBeyondRegisters();
    return WTF::move(m_plan);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
