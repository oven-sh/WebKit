/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BlockCycles;
class CFG;
class FunctionAnalyses;
class PendingTyping;
class UseLists;
struct ValueLocation;

struct TypePropagationInputs {
    const Program& program;
    FunctionID id;
    Function& function;
    FunctionAnalyses& analyses;
    TypeArena& types;
    BuildContext& build;
    TypingMemo* memo { nullptr };
    const KnownPositionTypes* knownPositions { nullptr };
};

struct TypePropagationOutcome {
    uint32_t retypedValues { 0 };
    uint32_t changedExpectedTypes { 0 };
    uint32_t cutBlocks { 0 };
    uint32_t replacedUpcasts { 0 };
    bool waitsToCutCode { false };
    bool didLookForCodeToCut { false };
    bool gaveUpOnRounds { false };
    bool leftTypesAboveTheirRules { false };

    bool didChange() const { return retypedValues || changedExpectedTypes || cutBlocks; }
    bool didChangeControlFlow() const { return cutBlocks; }

    TypePropagationOutcome& operator+=(const TypePropagationOutcome& other)
    {
        retypedValues += other.retypedValues;
        changedExpectedTypes += other.changedExpectedTypes;
        cutBlocks += other.cutBlocks;
        replacedUpcasts += other.replacedUpcasts;
        gaveUpOnRounds |= other.gaveUpOnRounds;
        leftTypesAboveTheirRules |= other.leftTypesAboveTheirRules;
        if (other.didLookForCodeToCut) {
            didLookForCodeToCut = true;
            waitsToCutCode = other.waitsToCutCode;
        }
        return *this;
    }
};

inline constexpr OpcodeSet opcodesWithIndirectUsers = []() consteval {
    OpcodeSet opcodes = opcodesThatMayHaveSeveralResults;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        if (provesOf(static_cast<Opcode>(index)) != Proof::None)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

class TypePropagation {
    WTF_MAKE_NONCOPYABLE(TypePropagation);
public:
    explicit TypePropagation(const TypePropagationInputs&);
    ~TypePropagation();

    void typeAllValues();
    void typeDependentsOf(std::span<const ValueID> changedValues, bool everyOtherValueIsAsTyped);
    void typePendingValues(PendingTyping&);
    void tellWhatIsTyped();

    void restartAllValues();
    void restartDependentsOf(std::span<const ValueID>);

    void solveTypes() { solve(TypeLevel::Proven); }
    void solveExpectedTypes();
    TypePropagationOutcome finish();
    TypePropagationOutcome finishWithoutCompacting(Vector<ValueID>& parametersBehindRemovedEdges);

private:
    enum class Direction : uint8_t { Ascending, Descending };
    enum class QueuedValues : uint8_t { All, OnlyListed, AllButListed };

    struct StateBeforeRestart {
        TypeID type;
        TypeID expected;
        TypeID expectedOrType;
        uint32_t numberOfUpdates { 0 };
    };

    enum class SweepFlag : uint8_t {
        IsChangedValue = 1 << 0,
        IsTyped = 1 << 1,
        HasOtherType = 1 << 2,
        IsExpectedTyped = 1 << 3,
        HasOtherExpectedType = 1 << 4,
    };

    struct PlaceOfParameter {
        uint32_t block;
        uint32_t index;
    };

    void forEachSourceOf(ValueID id, NOESCAPE const Invocable<void(ValueID)> auto& functor) const { forEachSourceOf(m_inputs.function.at(id), functor); }
    void forEachSourceOfParameter(BlockID, uint32_t index, NOESCAPE const Invocable<void(ValueID)> auto&) const;
    void forEachSourceOf(const Instruction&, NOESCAPE const Invocable<void(ValueID)> auto&) const;
    bool isEveryValueTypedBehindItsSources() const;
    void sweep(TypeLevel);
    void storeSweptType(TypeLevel, ValueID, TypeID computed);

    TypeID expectedTypeToStore(ValueID, const Value&, TypeID computed);
    bool hasTypesOfEveryArgument(BlockID, uint32_t index, const Value& parameter) const;
    void enqueueParametersOf(BlockID);
    void enqueuePendingValues(PendingTyping&);
    uint32_t componentOf(ValueID, unsigned cycle);
    void findComponentsOf(unsigned cycle);
    void typeComponent(uint32_t component);

    void willTypeValues();
    void willRestartValues();
    void rememberState(ValueID);
    bool isRemembered(ValueID id) const { return !m_rememberedValues.isEmpty() && m_isRemembered.quickGet(id.index()); }
    ALWAYS_INLINE Value& editTypeOf(ValueID id)
    {
        if (isRemembered(id)) [[likely]]
            return m_inputs.function.editAndReportLater(id);
        return m_inputs.function.edit(id, ValueEdit::Type);
    }
    void reportRetypedValues(bool countsThem);
    void restart(ValueID);
    void listFromLastToFirst(std::span<const ValueID>);
    void listRestartedValuesInOrder();

    void enqueue(ValueID);
    void enqueueUsersOf(ValueID);
    void enqueueIndirectUsersOf(ValueID);
    void enqueueEdgeResultParameters(BlockID from);
    void willTakeValuesFromWorklist();

    TypingContext typingContext(TypeLevel level) { return { m_inputs.program, m_inputs.function, m_inputs.types, level, m_inputs.memo, m_inputs.knownPositions, true }; }
    void addTypeOutOfDate(ValueID, ASCIILiteral detail);
    void solve(TypeLevel);
    void propagate(TypeLevel, Direction);
    TypeID compute(const TypingContext&, ValueID, const ValueLocation&);
    TypeID computeValue(const TypingContext&, ValueID);
    TypeID computeParameter(const TypingContext&, ValueID, BlockID, uint32_t index);
    TypeID computeTypeOnEdge(const TypingContext&, EdgeResultPosition, const EdgeResult&);
    TypeID meetWithPreviousExpectation(ValueID, TypeID computed);
    TypeID joinIntoParameter(const TypingContext&, ValueID parameter, TypeID current, TypeID incoming);
    TypeID widenGrownParameter(ValueID parameter, TypeID old, TypeID grown);
    bool update(TypeLevel, Direction, ValueID, TypeID computed);
    void giveUpOnRounds(TypeLevel, Direction);

    TypeID narrowerTypeOrNull(TypeID narrower, TypeID);
    void replaceUpcastsOfWidenedValues();
    void dropExpectedTypesThatSayNothing();
    bool mayHaveCodeToCut() const;
    void cutAfterNeverValues();

    TypePropagationInputs m_inputs;
    TypePropagationOutcome m_outcome;
    const CFG& m_cfg;
    const bool m_verifies;
    const UseLists* m_uses { nullptr };
    const BlockCycles* m_cycles { nullptr };
    ChangeLog::Cursor m_logAtStart;
    TypingMemo m_ownMemo;

    Vector<ValueID, 64> m_worklist;
    BitVector m_isQueued;
    QueuedValues m_queuedValues { QueuedValues::All };
    const BitVector* m_listedValues { nullptr };
    BitVector m_wasTakenFromWorklist;
    BitVector m_hasUserThatWasTakenFirst;
    Vector<ValueID, 16> m_valuesAboveTheirRules;
    Vector<OptionSet<SweepFlag>, 64> m_sweepFlags;

    BitVector m_isRemembered;
    Vector<StateBeforeRestart, 64> m_statesBeforeRestart;
    Vector<ValueID, 64> m_rememberedValues;
    size_t m_numberOfReportedValues { 0 };
    BitVector m_isRestarted;
    Vector<ValueID, 64> m_restartedValuesFromLastToFirst;

    bool m_didListRestartedValues { false };
    bool m_didMarkSourcesOfUnreachableValues { false };
    bool m_everyOtherValueIsAsTyped { false };
    bool m_didTypeAllValues { false };
    bool m_dropsExpectedTypesAtOnce { false };

    BitVector m_didEnqueueParametersOf;
    Vector<PlaceOfParameter> m_placesOfParameters;
    Vector<uint32_t> m_componentOfValue;
    BitVector m_isSearchedCycle;
    Vector<ValueID, 32> m_membersOfComponents;
    Vector<uint32_t, 8> m_firstMemberOfComponent;
    Vector<uint32_t, 8> m_waitingComponents;
    BitVector m_isWaitingComponent;
    BitVector m_isMemberOfTypedComponent;
};

void TypePropagation::forEachSourceOf(const Instruction& user, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
{
    user.forEachOperand([&](ValueID source, uint32_t) {
        functor(source);
        if (opcodesWithIndirectUsers.contains(m_inputs.function.at(source).opcode)) {
            m_inputs.function.at(source).forEachOperand([&](ValueID indirectSource, uint32_t) {
                functor(indirectSource);
            });
        }
    });
}

void TypePropagation::forEachSourceOfParameter(BlockID block, uint32_t index, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
{
    for (Edge edge : m_cfg.incomingEdges(block)) {
        const Target& target = targetOf(m_inputs.function, edge);
        if (index < target.arguments.size())
            functor(target.arguments[index]);
        else if (!edge.isHandlerEdge())
            forEachSourceOf(m_inputs.function.at(edge.from).terminator, functor);
    }
}

enum class TypedFirst : uint8_t { Nothing, DependentsOfChangedValues, AllValues, PendingValues };
TypePropagationOutcome typeUntilNothingIsCut(const TypePropagationInputs&, TypedFirst, std::span<const ValueID> changedValues = { }, bool everyOtherValueIsAsTyped = false);

inline TypePropagationOutcome typeValuesFromNever(const TypePropagationInputs& inputs) { return typeUntilNothingIsCut(inputs, TypedFirst::AllValues); }
inline TypePropagationOutcome narrowValueTypes(const TypePropagationInputs& inputs, std::span<const ValueID> changedValues) { return typeUntilNothingIsCut(inputs, TypedFirst::DependentsOfChangedValues, changedValues); }
inline TypePropagationOutcome narrowValueTypesBehindChanges(const TypePropagationInputs& inputs, std::span<const ValueID> changedValues) { return typeUntilNothingIsCut(inputs, TypedFirst::DependentsOfChangedValues, changedValues, true); }

bool hasTypedValues(const Function&);
void typeValuesWithoutCutting(const TypePropagationInputs&);
inline TypePropagationOutcome finishTypedValues(const TypePropagationInputs& inputs) { return typeUntilNothingIsCut(inputs, TypedFirst::Nothing); }

bool waitsForGenericVersion(const Program&, const Function&);

class PendingTyping {
public:
    bool isEverythingPending(const ChangeLog& log) const { return m_isEverythingPending || log.hasForgottenSince(m_seen); }
    bool waitsToCutCode() const { return m_waitsToCutCode; }
    bool holdsTypesLeftWhenRoundsRanOut() const { return m_holdsTypesLeftWhenRoundsRanOut; }
    void setWaitsToCutCode(bool waits) { m_waitsToCutCode = waits; }
    void setHoldsTypesLeftWhenRoundsRanOut(bool holds) { m_holdsTypesLeftWhenRoundsRanOut = holds; }
    bool isEmpty(const ChangeLog& log) const { return !isEverythingPending(log) && !log.hasChangesSince(m_seen) && m_values.isEmpty() && m_blocks.isEmpty() && !m_valuesTypedFromRecordsArePending; }

    void didTypeEverything(const ChangeLog& log) { didTypeEverythingBefore(log.end()); }
    void didEnterScheduleTyped(const ChangeLog&);
    bool compact(Function&, Renumbering* = nullptr);
    PendingTyping forCopyWith(const ChangeLog& logOfCopy, const ChangeLog&) const;

    void takeEntries(const ChangeLog&);
    void forgetChangesOfTypesAlone();
    void didTypePendingValues();
    void didTypeEverythingBefore(const ChangeLog::Cursor&);

private:
    friend class TypePropagation;

    void moveEntriesOutOfUnreachableBlocks(const Function&);

    ChangeLog::Cursor m_seen;
    Vector<ValueChange, 16> m_values;
    Vector<BlockChange, 8> m_blocks;
    bool m_isEverythingPending { true };
    bool m_valuesTypedFromRecordsArePending { false };
    bool m_waitsToCutCode { false };
    bool m_holdsTypesLeftWhenRoundsRanOut { false };
};

TypePropagationOutcome typeChangedValues(const TypePropagationInputs&);

bool becomeResolved(Function&, ValueID convert, const ConvertResolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
