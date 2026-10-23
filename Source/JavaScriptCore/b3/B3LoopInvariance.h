/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Effects.h"
#include "B3LoopRegions.h"
#include "B3RegisterPressure.h"
#include "B3Value.h"
#include "B3ValueKey.h"
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/RangeSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class PhiChildren;

enum class PhiResolution : uint8_t { Keep, Resolve };

enum class HoistDecision : uint8_t {
    Hoist,
    HoistSpeculatively,
    NotInvariant,
    HasEffects,
    Clobbered,
    ControlDependent,
};

inline bool isHoist(HoistDecision decision)
{
    return decision == HoistDecision::Hoist || decision == HoistDecision::HoistSpeculatively;
}

enum class LoopGuardKind : uint8_t { Required, Redundant };

struct InvalidationSet {
    RangeSet<HeapRange> reads;
    bool readsPinned { false };

    void add(const Effects& effects)
    {
        if (effects.readsMutability != Mutability::Immutable)
            effects.reads.addTo(reads);
        readsPinned |= effects.readsPinned;
    }

    void merge(const InvalidationSet& other)
    {
        reads.addAll(other.reads);
        readsPinned |= other.readsPinned;
    }

    bool isInvalidatedBy(const Effects& effects) const
    {
        return effects.fence || (effects.writesPinned && readsPinned) || effects.writes.overlaps(reads);
    }
};

struct HoistStep {
    enum class Kind : uint8_t {
        Value,
        Guard,
        Check,
        PeeledValue,
        PeeledGuard,
        PeeledCheck,
    };

    Kind kind;
    Value* value;
};

struct HoistPlan {
    Vector<HoistStep, 16> steps;
    IndexSet<Value*> hoistedValues;
    IndexSet<Value*> valuesValidInExcursion;
    UncheckedKeyHashMap<Value*, Value*> repeatedValues;
    UncheckedKeyHashMap<Value*, BasicBlock*> likelySuccessors;
    InvalidationSet invalidation;
    unsigned numberOfHoistedValues { 0 };
    unsigned numberOfSpeculativeHoists { 0 };
    unsigned numberOfPeeledValues { 0 };
    unsigned numberOfHoistsBeyondRegisters { 0 };
    bool hoistsGuard { false };

    bool isHoisted(Value* value) const { return hoistedValues.contains(value); }
};

class LoopInvariance {
    WTF_MAKE_NONCOPYABLE(LoopInvariance);
public:
    LoopInvariance(Procedure&, NaturalLoops&, const HotRegion&, PhiChildren&, PhiResolution);

    std::optional<GaveUp> scanRegion();
    unsigned numberOfClonedValues() const { return m_numberOfClonedValues; }

    bool isInvariant(Value*) const;
    bool hasInvariantChildren(Value*) const;
    Value* canonicalValue(Value*) const;
    bool isSameValue(Value*, Value*) const;
    Value* singleInputInFastVersion(Value* phi) const;
    bool isSpeculativelyHoisted(Value* value) const { return m_speculativelyHoisted.contains(value); }
    bool isAvailableInPreHeader(Value*) const;

    HoistDecision canHoist(Value*);
    void visit(Value*);
    void hoistGuard(Value* terminal, BasicBlock* likelySuccessor);
    LoopGuardKind addLoopGuard(Value* terminal, BasicBlock* likelySuccessor);

    const HoistPlan& plan() const { return m_plan; }
    HoistPlan takePlan();

private:
    enum class FirstIterationValue : uint8_t { Unknown, Constant, NotConstant };

    struct PendingGuard {
        Value* guard;
        BasicBlock* likelySuccessor;
    };

    struct ExecutedValueSearch;
    struct PeelingAttempt;

    bool isInLoop(Value* value) const { return m_loops.belongsTo(value->owner, *m_region.loop); }
    bool isInSiblingLoop(BasicBlock*) const;
    UncheckedKeyHashMap<Value*, Value*>& repetitions() { return m_resolution == PhiResolution::Resolve ? m_plan.repeatedValues : m_repeatedLoads; }
    const UncheckedKeyHashMap<Value*, Value*>& repetitions() const { return m_resolution == PhiResolution::Resolve ? m_plan.repeatedValues : m_repeatedLoads; }

    void collectExecutedValues();
    void addChildrenBeforeLoop(Value*, ExecutedValueSearch&) const;
    void scanBlocksBeforeLoop(ExecutedValueSearch&) const;

    Value* findEquivalentExecutedLoad(Value*) const;
    bool isBetweenExecutedLoads(Value*) const;
    Value* canonicalChild(Value*);
    ValueKey canonicalKey(Value*);
    bool isSameComputation(Value*, const ValueKey&, Value* executed);
    void recordRepeatedComputation(Value*);
    Value* findSelectedOperand(Value*) const;

    Value* findEnteringValue(Value* headerPhi) const;
    bool matchesValueBeforeLoop(Value* inside, Value* before, InvalidationSet& walked) const;
    Value* invariantValueOfHeaderPhi(Value* phi, InvalidationSet& walked) const;

    HoistDecision checkEffects(Value*, Effects) const;
    bool isChosenByPendingGuard(Value* phi) const;
    bool readsThroughPhiChosenByPendingGuard(Value*) const;
    void resolvePhi(Value*);
    void visitCheck(Value*);
    void hoistValue(Value*, HoistDecision);
    void hoist(Value*, HoistStep::Kind);
    unsigned countHoistsBeyondRegisters() const;

    bool tryPeelPendingGuards();
    FirstIterationValue stagePeeledCopy(Value*, HoistStep::Kind, PeelingAttempt&) const;

    Procedure& m_proc;
    NaturalLoops& m_loops;
    const HotRegion& m_region;
    PhiChildren& m_phiChildren;
    PhiResolution m_resolution;

    RangeSet<HeapRange> m_writes;
    bool m_writesPinned { false };
    unsigned m_numberOfClonedValues { 0 };
    RegisterBudget m_registerBudget;

    Vector<Value*, 16> m_executedValues;
    UncheckedKeyHashMap<Value*, Value*> m_repeatedLoads;
    UncheckedKeyHashMap<ValueKey, Value*> m_canonicalConstants;
    IndexSet<Value*> m_speculativelyHoisted;
    IndexSet<Value*> m_likelyPathOnlyValues;
    UncheckedKeyHashMap<Value*, bool> m_hoistedGuardConditions;
    UncheckedKeyHashMap<Value*, bool> m_loopGuardConditions;

    Vector<PendingGuard, 4> m_pendingGuards;
    IndexSet<Value*> m_phisChosenByPendingGuards;
    UncheckedKeyHashMap<Value*, FirstIterationValue> m_peeledValues;
    bool m_peelingFailed { false };

    HoistPlan m_plan;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
