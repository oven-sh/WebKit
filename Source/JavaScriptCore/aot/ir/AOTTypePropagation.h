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
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BlockCycles;
class CFG;
class FunctionAnalyses;
class PendingTyping;
class UseLists;

struct TypePropagationInputs {
    const Program& program;
    FunctionID id;
    Function& function;
    FunctionAnalyses& analyses;
    TypeArena& types;
    BuildContext& build;
    // Of the caller that types one function several times while the program stays as it is, with one arena.
    TypingMemo* memo { nullptr };
    const KnownPositionTypes* knownPositions { nullptr };
};

struct TypePropagationOutcome {
    uint32_t retypedValues { 0 };
    uint32_t changedExpectedTypes { 0 };
    uint32_t cutBlocks { 0 };
    uint32_t replacedUpcasts { 0 };

    bool didChange() const { return retypedValues || changedExpectedTypes || cutBlocks; }
    bool didChangeControlFlow() const { return cutBlocks; }

    TypePropagationOutcome& operator+=(const TypePropagationOutcome& other)
    {
        retypedValues += other.retypedValues;
        changedExpectedTypes += other.changedExpectedTypes;
        cutBlocks += other.cutBlocks;
        replacedUpcasts += other.replacedUpcasts;
        return *this;
    }
};

class TypePropagation {
    WTF_MAKE_NONCOPYABLE(TypePropagation);
public:
    explicit TypePropagation(const TypePropagationInputs&);
    ~TypePropagation();

    enum class ChangedValues : uint8_t { MayLeaveOthersOutOfDate, EveryOtherValueIsAsTyped };

    void typeAllValues();
    void typeDependentsOf(std::span<const ValueID> changedValues, ChangedValues);
    // A value that was edited is computed again, and so is what is typed from a value whose types have come out different. What is computed to the same types ends there.
    void typePendingValues(PendingTyping&);
    // For whoever has typed values in another way.
    void tellWhatIsTyped();

    void restartAllValues();
    void restartDependentsOf(std::span<const ValueID>);
    // For a caller that asks about the listed values alone, which hold whatever one of them is typed from and is to be typed anew: they start over, and nothing else is typed from here on.
    void restartListedValues(std::span<const ValueID>);

    // What the rules give for one value at the proven level, from the types that the values have which it is typed from. Nothing is stored.
    TypeID provenTypeByRule(ValueID);
    TypeID provenTypeOfParameterByRule(ValueID, BlockID, uint32_t index);
    // Whatever the value is typed from, beside the records of the program. One may come more than once.
    void forEachSourceOf(ValueID, NOESCAPE const Invocable<void(ValueID)> auto&) const;
    void forEachSourceOfParameter(BlockID, uint32_t index, NOESCAPE const Invocable<void(ValueID)> auto&) const;

    void solveTypes();
    // Expected types come from the positions that are only expected to hold their type: entry parameters, fields, variables, captures, results.
    void solveExpectedTypes();
    TypePropagationOutcome finish();
    // Leaves the ids as they are. A cut removes edges, and the parameters they led to were joined over them: those are to be typed again.
    TypePropagationOutcome finishWithoutCompacting(Vector<ValueID>& parametersBehindRemovedEdges);

private:
    enum class Direction : uint8_t { Ascending, Descending };

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

    std::span<const Edge> incomingEdgesOf(BlockID) const;
    bool hasRetreatingEdge() const;
    bool isEveryValueTypedBehindItsSources() const;
    void sweep(TypeLevel);
    bool hasOperandTypedOtherwise(TypeLevel, const Instruction&) const;
    bool isValueToSweep(TypeLevel, ValueID) const;
    bool isParameterToSweep(TypeLevel, BlockID, uint32_t index, ValueID) const;
    void storeSweptType(TypeLevel, ValueID, TypeID computed);

    TypeID partOfExpectedTypeInside(TypeID type, TypeID expected);
    void typeAgain(ValueID, bool isParameter, BlockID, uint32_t index);
    bool hasTypesOfEveryArgument(BlockID, uint32_t index, const Value& parameter) const;
    void enqueueParametersOf(BlockID);
    void enqueuePendingValues(PendingTyping&);
    void typeQueuedValues();
    uint32_t componentOf(ValueID, unsigned cycle);
    void findComponentsOf(unsigned cycle);
    void typeComponent(uint32_t component);

    void willTypeValues();
    void willRestartValues();
    void rememberState(ValueID);
    // The log hears of a remembered value when all is typed, and only where its types are no longer those that were remembered.
    ALWAYS_INLINE Value& editTypeOf(ValueID id)
    {
        if (!m_rememberedValues.isEmpty() && m_isRemembered.quickGet(id.index())) [[likely]]
            return m_inputs.function.editAndReportLater(id);
        return m_inputs.function.edit(id, ValueEdit::Type);
    }
    void reportRetypedValues();
    void restart(ValueID);
    void listRestartedValuesInOrder();
    bool isJoin(ValueID) const;

    void enqueue(ValueID);
    void enqueueUsersOf(ValueID);
    void enqueueIndirectUsersOf(ValueID);
    void enqueueEdgeResultParameters(BlockID from);
    void enqueueRestartedValues();
    void willTakeValuesFromWorklist();
    void markSourcesThatWereNotTaken(ValueID, bool isParameter, BlockID, uint32_t index);

    TypingContext typingContext(TypeLevel);
    void solve(TypeLevel);
    void propagate(TypeLevel, Direction);
    TypeID computeValue(const TypingContext&, ValueID);
    TypeID computeParameter(const TypingContext&, ValueID, BlockID, uint32_t index);
    TypeID computeTypeOnEdge(const TypingContext&, EdgeResultPosition, const EdgeResult&);
    TypeID meetWithPreviousExpectation(ValueID, TypeID computed);
    TypeID joinIntoParameter(const TypingContext&, ValueID parameter, TypeID current, TypeID incoming);
    TypeID widenGrownParameter(ValueID parameter, TypeID old, TypeID grown);
    bool update(TypeLevel, Direction, ValueID, TypeID computed);
    void giveUpOnRounds(TypeLevel, Direction);

    TypeID narrowerTypeOrNull(TypeID narrower, TypeID);
    void dropExpectedTypesThatSayNothing();
    void countChanges();
    bool mayHaveCodeToCut() const;
    void cutAfterNeverValues();
    void cutBlock(BlockID, uint32_t firstErasedIndex, Origin);
    void didCutBlocks(Vector<ValueID>& parametersBehindRemovedEdges);

    TypePropagationInputs m_inputs;
    TypePropagationOutcome m_outcome;
    const CFG& m_cfg;
    const UseLists* m_uses { nullptr };
    Vector<ValueID, 64> m_worklist;
    BitVector m_isQueued;
    BitVector m_isRestarted;
    BitVector m_wasTakenFromWorklist;
    BitVector m_hasUserThatWasTakenFirst;
    BitVector m_isRemembered;
    Vector<OptionSet<SweepFlag>, 64> m_sweepFlags;
    Vector<ValueID, 16> m_valuesAboveTheirRules;
    Vector<StateBeforeRestart, 64> m_statesBeforeRestart;
    Vector<ValueID, 64> m_rememberedValues;
    size_t m_numberOfReportedValues { 0 };
    Vector<ValueID, 64> m_restartedValuesFromLastToFirst;
    bool m_didListRestartedValues { false };
    bool m_didMarkSourcesOfUnreachableValues { false };
    bool m_everyOtherValueIsAsTyped { false };
    bool m_didTypeAllValues { false };
    bool m_didGiveUp { false };
    bool m_dropsExpectedTypesAtOnce { false };
    BitVector m_didEnqueueParametersOf;
    ChangeLog::Cursor m_logAtStart;
    const BlockCycles* m_cycles { nullptr };
    bool m_queuesOnlySomeValues { false };
    bool m_queuesAllButSomeValues { false };
    BitVector m_mayBeQueued;
    struct PlaceOfParameter {
        uint32_t block;
        uint32_t index;
    };
    // Of the parameters that were put into the list with their block. Nothing is written for any other value.
    Vector<PlaceOfParameter> m_placesOfParameters;
    static constexpr uint32_t notVisited = UINT32_MAX;
    static constexpr uint32_t noComponent = UINT32_MAX - 1;
    Vector<uint32_t> m_componentOfValue;
    BitVector m_isSearchedCycle;
    Vector<ValueID, 32> m_membersOfComponents;
    Vector<uint32_t, 8> m_firstMemberOfComponent;
    Vector<uint32_t, 8> m_waitingComponents;
    BitVector m_isWaitingComponent;
    bool m_typesRestartedValuesAlone { false };
    TypingMemo m_ownMemo;
};

// A ResultAt and what a terminator says about an edge are typed from the operands of the value they name.
inline constexpr OpcodeSet opcodesWithIndirectUsers = []() consteval {
    OpcodeSet opcodes = opcodesThatMayHaveSeveralResults;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        if (provesOf(static_cast<Opcode>(index)) != Proof::None)
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

void TypePropagation::forEachSourceOf(ValueID id, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
{
    const Function& function = m_inputs.function;
    function.at(id).forEachOperand([&](ValueID source, uint32_t) {
        if (!source)
            return;
        functor(source);
        if (!opcodesWithIndirectUsers.contains(function.at(source).opcode))
            return;
        function.at(source).forEachOperand([&](ValueID indirectSource, uint32_t) {
            if (indirectSource)
                functor(indirectSource);
        });
    });
}

void TypePropagation::forEachSourceOfParameter(BlockID block, uint32_t index, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
{
    const Function& function = m_inputs.function;
    for (Edge edge : incomingEdgesOf(block)) {
        const Target& target = targetOf(function, edge);
        if (index < target.arguments.size()) {
            functor(target.arguments[index]);
            continue;
        }
        if (edge.isHandlerEdge())
            continue;
        function.at(edge.from).terminator.forEachOperand([&](ValueID source, uint32_t) {
            if (!source)
                return;
            functor(source);
            if (!opcodesWithIndirectUsers.contains(function.at(source).opcode))
                return;
            function.at(source).forEachOperand([&](ValueID indirectSource, uint32_t) {
                if (indirectSource)
                    functor(indirectSource);
            });
        });
    }
}

TypePropagationOutcome typeValuesFromNever(const TypePropagationInputs&);

// What is typed behind a Check holds where the Check has passed.
bool waitsForGenericVersion(const Program&, const Function&);

// For a caller that may still widen what the function reads: the code behind a value of type never stays. finishTypedValues() cuts it.
bool hasTypedValues(const Function&);
void typeValuesWithoutCutting(const TypePropagationInputs&);
TypePropagationOutcome finishTypedValues(const TypePropagationInputs&);

TypePropagationOutcome narrowValueTypes(const TypePropagationInputs&, std::span<const ValueID> changedValues);
// For a caller that knows every other value to be typed as the rules type it: nothing is typed behind a value that keeps its types.
TypePropagationOutcome narrowValueTypesBehindChanges(const TypePropagationInputs&, std::span<const ValueID> changedValues);

// Which values of a function have still to be typed again: what the log of changes has named since types were last propagated, less what somebody has typed since.
class PendingTyping {
public:
    // Nothing is known of the function, or the log has forgotten: every value has to be typed.
    bool isEverythingPending(const ChangeLog& log) const { return m_isEverythingPending || log.hasForgottenSince(m_seen); }
    bool isEmpty(const ChangeLog& log) const { return !isEverythingPending(log) && !log.hasChangesSince(m_seen) && m_values.isEmpty() && m_blocks.isEmpty() && !m_valuesTypedFromRecordsArePending; }

    void didTypeEverything(const ChangeLog&);
    // A function that says so enters a schedule typed, but for what is typed from its signature, from a variable or from the record of another function.
    void didEnterScheduleTyped(const ChangeLog&);
    // compactFunction(), for whoever keeps track of what is pending.
    bool compact(Function&, Renumbering* = nullptr);
    // For a copy of the function as it is now, which has a log of its own.
    PendingTyping forCopyWith(const ChangeLog& logOfCopy, const ChangeLog&) const;

private:
    friend class TypePropagation;

public:
    // For the propagation of types.
    void takeEntries(const ChangeLog&);
    void forgetChangesOfTypesAlone();
    void didTypePendingValues();
    void didTypeEverythingBefore(const ChangeLog::Cursor&);

private:
    ChangeLog::Cursor m_seen;
    Vector<ValueChange, 16> m_values;
    Vector<BlockChange, 8> m_blocks;
    bool m_isEverythingPending { true };
    bool m_valuesTypedFromRecordsArePending { false };
};

// Types what is pending.
TypePropagationOutcome typeChangedValues(const TypePropagationInputs&);

// Turns a typed Convert into what resolveConvert() answers for it, with the types it has. False: nothing is left of it, and its uses are to read its operand.
bool becomeResolved(Function&, ValueID convert, const ConvertResolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
