/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include "AOTResultRules.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FunctionAnalyses;

struct TypePropagationInputs {
    const Program& program;
    FunctionID id;
    Function& function;
    FunctionAnalyses& analyses;
    TypeArena& types;
    BuildContext& build;
    // Of the caller that types one function several times while the program stays as it is, with one arena.
    FieldReadTypes* fieldReadTypes { nullptr };
};

struct TypePropagationOutcome {
    uint32_t retypedValues { 0 };
    uint32_t changedWrittenTypes { 0 };
    uint32_t changedPredictions { 0 };
    uint32_t cutBlocks { 0 };

    bool didChange() const { return retypedValues || changedWrittenTypes || changedPredictions || cutBlocks; }
    bool didChangeControlFlow() const { return cutBlocks; }

    TypePropagationOutcome& operator+=(const TypePropagationOutcome& other)
    {
        retypedValues += other.retypedValues;
        changedWrittenTypes += other.changedWrittenTypes;
        changedPredictions += other.changedPredictions;
        cutBlocks += other.cutBlocks;
        return *this;
    }
};

class TypePropagation {
    WTF_MAKE_NONCOPYABLE(TypePropagation);
public:
    explicit TypePropagation(const TypePropagationInputs&);

    void restartAllValues();
    void restartDependentsOf(std::span<const ValueID>);

    void solveTypes();
    void solvePredictions();
    TypePropagationOutcome finish();

private:
    enum class Direction : uint8_t { Ascending, Descending };

    struct ParameterPosition {
        BlockID block;
        uint32_t index { 0 };
    };

    struct StoredTypes {
        TypeID type;
        TypeID written;
        TypeID prediction;
    };

    void collectPreviousTypes();
    void collectParameters();
    void restart(ValueID);
    bool isEntryParameter(ValueID) const;
    bool isJoin(ValueID) const;

    void enqueue(ValueID);
    void enqueueUsersOf(ValueID);
    void enqueueIndirectUsersOf(ValueID);
    void enqueueEdgeResultParameters(BlockID from);
    void enqueueRestartedValues();

    void solve(TypeLevel);
    void propagate(TypeLevel, Direction);
    TypeID computeValue(const TypingContext&, ValueID);
    TypeID computeParameter(const TypingContext&, ValueID);
    TypeID computeRequest(const TypingContext&, const Value&);
    TypeID joinIntoParameter(const TypingContext&, ValueID parameter, TypeID current, TypeID incoming);
    TypeID widenGrownParameter(ValueID parameter, TypeID old, TypeID grown);
    bool update(TypeLevel, Direction, ValueID, TypeID computed);
    void giveUpOnRounds(TypeLevel, Direction);

    void countChanges();
    void cutAfterNeverValues();
    void cutBlock(BlockID, uint32_t firstErasedIndex, Origin);

    TypePropagationInputs m_inputs;
    TypePropagationOutcome m_outcome;
    Vector<ValueID> m_worklist;
    BitVector m_isQueued;
    BitVector m_isRestarted;
    Vector<ValueID> m_valuesAboveTheirRules;
    Vector<std::optional<ParameterPosition>> m_parameterPositions;
    Vector<StoredTypes> m_previousTypes;
    Vector<uint32_t> m_numberOfUpdates;
    FieldReadTypes m_fieldReadTypes;
};

TypePropagationOutcome typeValuesFromNever(const TypePropagationInputs&);

// For a caller that may still widen what the function reads: the code behind a value of type never stays. finishTypedValues() cuts it.
bool hasTypedValues(const Function&);
void typeValuesWithoutCutting(const TypePropagationInputs&);
TypePropagationOutcome finishTypedValues(const TypePropagationInputs&);

TypePropagationOutcome narrowValueTypes(const TypePropagationInputs&, std::span<const ValueID> changedValues);

// What the types of a function were last computed from, value by value.
class TypedDefinitions {
public:
    // The values that are defined otherwise, or typed otherwise, than before the function was last typed. Nothing if every value has to be typed.
    std::optional<Vector<ValueID>> recordAndFindChanges(const TypePropagationInputs&);
    void forget() { m_isRecorded = false; }

private:
    struct Definition {
        uint64_t hash { 0 };
        TypeID type;
        TypeID written;
        TypeID prediction;

        friend bool operator==(const Definition&, const Definition&) = default;
    };

    struct Signature {
        FunctionKind kind { FunctionKind::Plain };
        OptionSet<FunctionFlags> flags;
        Vector<DeclaredType> captures;
        DeclaredType thisType;
        Vector<DeclaredType> parameters;
        Vector<DeclaredType> results;
        bool asksForWrittenTypes { false };

        friend bool operator==(const Signature&, const Signature&) = default;
    };

    Vector<Definition> m_definitions;
    Signature m_signature;
    bool m_isRecorded { false };
};

// Gives the types of typeValuesFromNever(). A value that nothing changed reaches got the type it has when the function was last typed,
// from what it would be typed from now, so only the others start over.
TypePropagationOutcome retypeChangedValues(const TypePropagationInputs&, TypedDefinitions&);

struct RequestBound {
    enum class Source : uint8_t { NotFound, Prediction, Written, Type };

    bool allowsClaim() const { return source == Source::Written || source == Source::Type; }

    Source source { Source::NotFound };
    TypeID type;
};

RequestBound boundOfClaimRead(const TypingContext&, const Value& claimRead);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
