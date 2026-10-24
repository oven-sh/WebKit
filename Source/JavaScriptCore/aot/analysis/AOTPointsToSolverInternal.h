/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPointsToSolver.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>

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

// A set of objects is a row of words, as many as hold a bit for each object of the function: one word in nearly every function.
using ObjectWord = uint64_t;
inline constexpr unsigned objectsPerWord = 64;

void forEachObjectIn(std::span<const ObjectWord> words, NOESCAPE const Invocable<void(ObjectID)> auto& functor)
{
    for (size_t index = 0; index < words.size(); ++index) {
        for (ObjectWord word = words[index]; word; word &= word - 1)
            functor(ObjectID(index * objectsPerWord + std::countr_zero(word)));
    }
}

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

struct SlotState {
    ObjectSlot slot;
    Vector<ObjectWord, 1> objects;
    Vector<SolvedContents, 1> contents;
    Vector<ValueID, 1> readers;
    Vector<ContentsApplication, 1> applications;
};

struct ContainerState {
    static constexpr size_t maxSearchedSlots = 8;

    SlotState* find(uint32_t slot)
    {
        if (slots.size() <= maxSearchedSlots) {
            for (SlotState& state : slots) {
                if (*state.slot == slot)
                    return &state;
            }
            return nullptr;
        }
        auto found = indexOfSlot.find(slot);
        return found != indexOfSlot.end() ? &slots[found->value] : nullptr;
    }

    SlotState& ensure(ObjectSlot slot)
    {
        if (!slot)
            return unknownSlot;
        if (SlotState* found = find(*slot))
            return *found;
        RELEASE_ASSERT(*slot < std::numeric_limits<uint32_t>::max() - 1);
        slots.append(SlotState { slot, Vector<ObjectWord, 1>(FillWith { }, unknownSlot.objects.size(), ObjectWord { 0 }), { }, { }, { } });
        if (slots.size() == maxSearchedSlots + 1) {
            for (uint32_t index = 0; index < slots.size(); ++index)
                indexOfSlot.add(*slots[index].slot, index);
        } else if (slots.size() > maxSearchedSlots)
            indexOfSlot.add(*slot, static_cast<uint32_t>(slots.size() - 1));
        return slots.last();
    }

    LayoutID environment;
    SlotState unknownSlot;
    Vector<SlotState, 1> slots;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> indexOfSlot;
};

struct UserOperand {
    const Value& user;
    uint32_t index;
    OperandSummary summary;
};

// The objects that have arrived at a value since its uses were last looked at.
struct ArrivedObjects {
    std::span<const ObjectID> objects;
    std::span<const ObjectWord> words;
};

struct NarrowedCopy {
    ValueID subject;
    ValueID parameter;

    friend auto operator<=>(const NarrowedCopy&, const NarrowedCopy&) = default;
};

constexpr bool isSummarizedCall(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
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

inline bool isSummarizedCall(const Value& value) { return isSummarizedCall(value.opcode); }

bool isOrderedBefore(const SolvedContents&, const SolvedContents&);

void forEachOverlappingSlot(ContainerState& state, ObjectSlot slot, NOESCAPE const Invocable<void(SlotState&)> auto& functor)
{
    functor(state.unknownSlot);
    if (!slot) {
        for (SlotState& known : state.slots)
            functor(known);
        return;
    }
    if (SlotState* known = state.find(*slot))
        functor(*known);
}

void forEachOverlappingEntry(ContainerState& state, ObjectSlot slot, NOESCAPE const Invocable<void(SolvedContents&)> auto& functor)
{
    forEachOverlappingSlot(state, slot, [&](SlotState& overlapping) {
        for (SolvedContents& entry : overlapping.contents)
            functor(entry);
    });
}

enum class Sweep : uint8_t { Forward, BackwardToVerify };

class PointsToSolver {
    WTF_MAKE_NONCOPYABLE(PointsToSolver);
public:
    PointsToSolver(const EscapeAnalysisInputs&, Sweep);

    PointsToResults solve();

private:
    SolvedObject& at(ObjectID object) { return m_results.objects[object.index()]; }
    const SolvedObject& at(ObjectID object) const { return m_results.objects[object.index()]; }
    const Value& definitionOf(ObjectID object) const { return m_function.at(at(object).definition); }
    // Of a pair the first says yes and the second no. Neither is set until somebody has asked.
    enum class ValueFlag : uint16_t {
        DefinesObject = 1 << 0,
        IsSharedByObjects = 1 << 1,
        HasNarrowedCopies = 1 << 2,
        MayHoldReference = 1 << 3,
        HoldsNoReference = 1 << 4,
        IsReference = 1 << 5,
        IsNoReference = 1 << 6,
        RefersToOperand = 1 << 7,
        RefersToNoOperand = 1 << 8,
        MayRunOtherCode = 1 << 9,
        RunsNoOtherCode = 1 << 10,
    };
    using WordsOfSet = Vector<ObjectWord, 2>;

    bool definesObject(ValueID value) const { return (m_valueFlags[value.index()] & OptionSet<ValueFlag> { ValueFlag::DefinesObject, ValueFlag::IsSharedByObjects }) == ValueFlag::DefinesObject; }
    bool isSharedByObjects(ValueID value) const { return m_valueFlags[value.index()].contains(ValueFlag::IsSharedByObjects); }
    bool hasSet(ValueID value) const { return !!m_results.setOfValue[value.index()]; }
    AnswerFromTypes ask(AnswerFromTypes::Question, uint32_t subject, uint32_t operandIndex = 0) const;
    bool askOnce(ValueID, AnswerFromTypes::Question, ValueFlag yes, ValueFlag no);
    bool mayHoldReference(ValueID value) { return askOnce(value, AnswerFromTypes::Question::MayHoldReference, ValueFlag::MayHoldReference, ValueFlag::HoldsNoReference); }
    bool isReference(ValueID value) { return askOnce(value, AnswerFromTypes::Question::IsReference, ValueFlag::IsReference, ValueFlag::IsNoReference); }
    bool refersToOperand(ValueID value) { return askOnce(value, AnswerFromTypes::Question::RefersToOperand, ValueFlag::RefersToOperand, ValueFlag::RefersToNoOperand); }
    bool mayRunOtherCode(ValueID);
    std::span<ObjectWord> wordsOf(ValueSetID set) LIFETIME_BOUND { return m_wordsOfSets.mutableSpan().subspan(size_t { set.index() } * m_wordsPerSet, m_wordsPerSet); }
    std::span<ObjectWord> arrivalsOf(ValueSetID set) LIFETIME_BOUND { return m_arrivalsOfSets.mutableSpan().subspan(size_t { set.index() } * m_wordsPerSet, m_wordsPerSet); }
    // A copy: a set grows, and the table of all sets moves, while its members are looked at.
    WordsOfSet objectsOf(ValueID) const;
    ValueSetID ensureSet(ValueID);
    ValueID parameterFor(const Use& edgeUse) const;
    std::span<const NarrowedCopy> narrowedCopiesOf(ValueID subject) const;
    bool takeWork(uint64_t amount = 1);
    void giveUp();
    void forgetAllButObjects();

    void findObjects();
    void findNarrowedCopies();
    void addObject(ValueID definition, ObjectKind);
    void addObjectSharingItsValue(ValueID);
    bool makesObjectOnEdge(BlockID from) const;
    void add(ValueID, ObjectID);
    void addAll(ValueID, std::span<const ObjectWord>);
    void addAll(ValueID, const ArrivedObjects&);
    void takeSets();
    size_t mirroredWhenBackward(size_t index) const { return m_sweep == Sweep::Forward ? index : m_function.values.size() - 1 - index; }

    // What is found about a use holds for every object that arrives at the used value: the record is filled once and written down for each.
    void solveUse(const ArrivedObjects&, ValueID usedValue, const Use&);
    void record(SolvedUse&, const ArrivedObjects&);
    void solveEdgeUse(const ArrivedObjects&, const Use&);
    void solveTerminatorOperand(const ArrivedObjects&, const Use&);
    void solveTerminatorOperandUse(SolvedUse&, const ArrivedObjects&, TerminatorKind, OperandUse);
    void addToDeclaredResults(SolvedUse&, const ArrivedObjects&);
    void solveOperand(const ArrivedObjects&, ValueID usedValue, const Use&);
    void solveOperandWith(const ArrivedObjects&, const Use&, const OperandSummary&);
    void solveOperandUse(SolvedUse&, const ArrivedObjects&, const UserOperand&, OperandUse);
    void solveRetainedOperand(SolvedUse&, const ArrivedObjects&, const UserOperand&);
    Vector<ValueID, 4> operandsStoredIn(ValueID user, const UserOperand&);
    void addStoredOperands(ObjectID container, const Value& user, std::span<const ValueID> storedOperands);
    bool isContentsStore(const UserOperand&) const;
    void addOperandsRetainedInResult(ValueID result, const ArrivedObjects& containers);

    LayoutID environmentLayoutOf(ObjectID) const;
    ContainerState& ensureContainer(ObjectID);
    OperandUses environmentFieldUses(const ContainerState&, ObjectSlot) const;
    void addContents(ObjectID container, ObjectSlot, std::span<const ObjectWord> objects);
    bool mayReadContentsIntoReference(const SolvedUse&, const UserOperand&) const;
    OperandSummary summaryOf(ValueID user, uint32_t operandIndex);
    void solveContentsRead(SolvedUse&, const ArrivedObjects&, const UserOperand&);
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
    // A word for each value is no more than the function itself takes. Past 64 objects the table grows with sets times objects.
    const uint64_t m_maxWordsOfSets;
    const Sweep m_sweep;
    uint64_t m_work { 0 };
    PointsToResults m_results;
    mutable Vector<AnswerFromTypes> m_answersFromTypes;
    Vector<NarrowedCopy> m_narrowedCopies;
    Vector<OptionSet<ValueFlag>> m_valueFlags;
    unsigned m_wordsPerSet { 1 };
    Vector<ObjectWord> m_wordsOfSets;
    Vector<ObjectWord> m_arrivalsOfSets;
    // The words of a set's arrivals that may hold a bit. In a function of thousands of objects a row is long and nearly every set holds one object.
    struct WordRange {
        uint32_t first { std::numeric_limits<uint32_t>::max() };
        uint32_t end { 0 };
    };
    Vector<WordRange> m_arrivedWordsOfSets;
    Vector<bool> m_setsThatMayBeUntracked;
    Vector<ObjectWord, 2> m_newContents;
    BitVector m_valuesWithArrivals;
    Vector<QueuedApplication, 8> m_applicationWorklist;
    // An object has a place in the list once something is known of its contents. The list never moves: it has room for all objects from the start.
    static constexpr uint32_t noContainer = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> m_containerOfObject;
    Vector<ContainerState> m_containers;
    // The last applications to all the contents of each object. Most are repeated at every call that the object is passed to.
    struct ApplicationToAll {
        OperandUses uses;
        EscapeReason reason { EscapeReason::LimitReached };
        bool isSet { false };
    };
    Vector<std::array<ApplicationToAll, 2>> m_lastApplicationsToAll;
    // The summaries of the operands of a user are asked for by each of its operands that an object arrives at.
    struct KeptSummary {
        OperandSummary summary;
        bool isKnown { false };
    };
    Vector<uint32_t> m_firstSummaryOfUser;
    Vector<KeptSummary> m_summaries;
    Vector<ValueID> m_valueOfSet;
    BitVector m_calledObjects;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
