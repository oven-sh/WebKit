/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPointsToSolver.h"
#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OperandUses retainedContentsUses { OperandUse::Inspects, OperandUse::Retains };
inline constexpr OperandUses leakingUses { OperandUse::Retains, OperandUse::Returns, OperandUse::PassesToUserCode };

// OperandUses::all() sets all 32 bits, so a loop that switches over OperandUse iterates over (uses & declaredOperandUses).
inline constexpr OperandUses declaredOperandUses = [] {
    OperandUses uses;
    for (OperandUse use : allMembersOf<OperandUse>)
        uses.add(use);
    return uses;
}();

struct ContentsApplication {
    ObjectSlot slot;
    OperandUses uses;
    EscapeReason reason;

    friend bool operator==(const ContentsApplication&, const ContentsApplication&) = default;
};

struct QueuedApplication {
    ObjectID object;
    OperandUses uses;
    EscapeReason reason;
};

struct ContainerState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ContainerState);

    LayoutID environment;
    Vector<SolvedContents, 2> contents;
    Vector<std::pair<ValueID, ObjectSlot>, 2> readers;
    Vector<ContentsApplication, 1> applications;
};

struct UserOperand {
    const Value& user;
    uint32_t index;
    const EffectsOnEdges& effects;
    OperandSummary summary;
};

struct NarrowedCopy {
    ValueID subject;
    ValueID parameter;

    friend auto operator<=>(const NarrowedCopy&, const NarrowedCopy&) = default;
};

constexpr bool slotsOverlap(ObjectSlot a, ObjectSlot b)
{
    return !a || !b || *a == *b;
}

inline bool isSummarizedCall(const Value& value)
{
    switch (effectRuleOf(value.opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isOrderedBefore(const SolvedContents&, const SolvedContents&);

void forEachOverlappingEntry(ContainerState& state, ObjectSlot slot, NOESCAPE const Invocable<void(SolvedContents&)> auto& functor)
{
    auto& contents = state.contents;
    size_t index = 0;
    if (slot) {
        for (; index < contents.size() && !contents[index].slot; ++index)
            functor(contents[index]);
        index = std::ranges::lower_bound(contents, slot, { }, &SolvedContents::slot) - contents.begin();
    }
    for (; index < contents.size() && slotsOverlap(contents[index].slot, slot); ++index)
        functor(contents[index]);
}

class PointsToSolver {
    WTF_MAKE_NONCOPYABLE(PointsToSolver);
public:
    explicit PointsToSolver(const EscapeAnalysisInputs&);

    PointsToResults solve();

private:
    SolvedObject& at(ObjectID object) { return m_results.objects[object.index()]; }
    const SolvedObject& at(ObjectID object) const { return m_results.objects[object.index()]; }
    const Value& definitionOf(ObjectID object) const { return m_function.at(at(object).definition); }
    bool definesObject(ValueID) const;
    bool mayHoldReference(ValueID) const;
    Vector<ObjectID, 8> objectsOf(ValueID) const;
    ValueID parameterFor(const Use& edgeUse) const;
    std::span<const NarrowedCopy> narrowedCopiesOf(ValueID subject) const;
    bool takeWork();
    void giveUp();

    void findObjects();
    void findNarrowedCopies();
    void addObject(ValueID definition, ObjectKind);
    void addObjectSharingItsValue(ValueID);
    bool makesObjectOnEdge(BlockID from) const;
    void add(ValueID, ObjectID);

    void solveUse(ObjectID, const Use&);
    void solveEdgeUse(SolvedUse&);
    void solveTerminatorOperand(SolvedUse&);
    void solveTerminatorOperandUse(SolvedUse&, TerminatorKind, OperandUse);
    void addToDeclaredResults(SolvedUse&);
    void solveOperand(SolvedUse&);
    void solveOperandUse(SolvedUse&, const UserOperand&, OperandUse);
    void solveRetainedOperand(SolvedUse&, const UserOperand&);
    void addStoredOperands(ObjectID container, const UserOperand&);
    bool isContentsStore(const UserOperand&) const;

    LayoutID environmentLayoutOf(ObjectID) const;
    ContainerState& ensureContainer(ObjectID);
    OperandUses environmentFieldUses(const ContainerState&, ObjectSlot) const;
    void addContents(ObjectID container, ObjectSlot, ObjectID);
    bool mayReadContents(const SolvedUse&, const UserOperand&) const;
    void solveContentsRead(SolvedUse&, const UserOperand&);
    void addReader(ObjectID container, ValueID reader, ObjectSlot);
    ObjectSlot slotInContainer(ObjectID container, const Value& user) const;

    void callObject(ObjectID);
    void applyToContents(ObjectID container, ObjectSlot, OperandUses, EscapeReason);
    void applyToObject(ObjectID, OperandUses, EscapeReason);
    void applyQueuedUses();

    bool isExactCapable(ValueID) const;
    bool handsOperandOn(ValueID user, uint32_t operandIndex) const;
    bool mayBeUntracked(ValueID) const;
    bool hasUntrackedSource(ValueID) const;
    void forEachExactAlias(ValueID, const Use&, NOESCAPE const ScopedLambda<void(ValueID)>&) const;
    void computeMayBeUntracked();

    void markLeakedEntries(ContainerState&) const;
    void collectContents();

    const EscapeAnalysisInputs& m_inputs;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    const uint64_t m_maxWork;
    uint64_t m_work { 0 };
    PointsToResults m_results;
    Vector<NarrowedCopy> m_narrowedCopies;
    BitVector m_valuesSharedByObjects;
    Vector<std::pair<ValueID, ObjectID>, 16> m_flowWorklist;
    Vector<QueuedApplication, 8> m_applicationWorklist;
    Vector<std::unique_ptr<ContainerState>> m_containers;
    BitVector m_calledObjects;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
