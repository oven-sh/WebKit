/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTSSARepair.h"
#include "AOTUseLists.h"
#include "AOTVector.h"
#include "IterationKind.h"
#include "TypedArrayType.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ExpansionStep : uint8_t {
    ForInPositions = 1 << 0,
    ArrayIteratorSteps = 1 << 1,
    Spreads = 1 << 2,
    Iterations = 1 << 3,
};

constexpr OptionSet<ExpansionStep> stepThatExpands(Opcode opcode)
{
    switch (opcode) {
    case Opcode::ForInStep:
        return ExpansionStep::ForInPositions;
    case Opcode::StepArrayIterator:
        return ExpansionStep::ArrayIteratorSteps;
    case Opcode::AppendSpread:
        return ExpansionStep::Spreads;
    case Opcode::IteratorOpen:
    case Opcode::IteratorResume:
    case Opcode::IteratorClose:
        return ExpansionStep::Iterations;
    default:
        return { };
    }
}

constexpr OpcodeSet opcodesExpandedIn(OptionSet<ExpansionStep> steps)
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (stepThatExpands(opcode).containsAny(steps))
            opcodes.add(opcode);
    }
    return opcodes;
}

// ExpandIteration has nothing to do in a function without any of the first, ExpandProvenIterations in one without any of the second.
inline constexpr OpcodeSet expandedOpcodes = opcodesExpandedIn({ ExpansionStep::ForInPositions, ExpansionStep::ArrayIteratorSteps, ExpansionStep::Spreads, ExpansionStep::Iterations });
inline constexpr OpcodeSet iterationOpcodes = opcodesExpandedIn(ExpansionStep::Iterations);

enum class IterationForm : uint8_t { Protocol, Array, TypedArray, String, SplitString, OwnEntries, Map, Set, Generator, AsyncGenerator };
enum class IterationOrigin : uint8_t { Source, Spread };
enum class ArmDispatch : uint8_t { SingleArm, ByForm, Versioned };
enum class PatternRead : uint8_t { BySteps, ByIndex, InOneRead };
// Proven: the plan has one arm, the arm has no test, and closing does nothing.
enum class ExpandedIterations : uint8_t { All, OnlyProven };
enum class OutlinedIteration : uint32_t { AppendIterable = 1, OpenIterator, CloseIterator, CloseIteratorAfterThrow, OpenWrappedIterator, OpenStringOrIterator, StepString, CloseStringOrIterator, CloseStringOrIteratorAfterThrow };

constexpr bool closingHasNoEffect(IterationForm form)
{
    switch (form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
    case IterationForm::String:
    case IterationForm::SplitString:
    case IterationForm::OwnEntries:
    case IterationForm::Map:
    case IterationForm::Set:
        return true;
    case IterationForm::Protocol:
    case IterationForm::Generator:
    case IterationForm::AsyncGenerator:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct IterationArm {
    IterationForm form { IterationForm::Protocol };
    JSC::IterationKind kind { JSC::IterationKind::Values };
    BuiltinClassID builtinClass;
    JSC::TypedArrayType typedArrayType { JSC::NotTypedArray };
    TypeID testedType;
    ValueID separator;
    BuiltinOverloadID separatorSearch;
    bool classIsProven { false };
    bool namesAreProven { false };
    bool bypassesIterator { false };
    // The protocol arm of a value that may be a string nobody expected.
    bool alsoServesStrings { false };
    // A class arm of an async iteration: it stands for the wrapper around the synchronous iterator.
    bool awaitsEachValue { false };

    bool needsNoTest() const { return classIsProven && namesAreProven; }

    friend bool operator==(const IterationArm&, const IterationArm&) = default;
};

// Control that leaves this block for the open takes this arm. A class arm walks the subject, and nobody needs the iterator that was passed on.
struct ArmSource {
    BlockID block;
    unsigned arm { 0 };
    ValueID subject;
    std::optional<Use> passedIterator;
};

struct IterationPlan {
    ValueID source;
    Vector<IterationArm, 2> arms;
    Vector<ValueID, 2> replacedValues;
    // Where the iterable merges what several blocks made: control comes through one of them last, so no arm needs a test.
    Vector<ArmSource, 2> armSources;

    bool hasProtocolArm() const { return !arms.isEmpty() && arms.last().form == IterationForm::Protocol; }
    bool hasClassArm() const { return !arms.isEmpty() && arms.first().form != IterationForm::Protocol; }
};

struct Iteration {
    ValueID open;
    IteratorKind kind { IteratorKind::Sync };
    IterationOrigin origin { IterationOrigin::Source };
    Vector<ValueID, 2> resumes;
    Vector<ValueID, 2> closes;
    Vector<BlockID, 1> cleanupBlocks;
};

struct PatternInOneRead {
    Iteration operations;
    ValueID iterable;
};

struct StateVariable {
    SSARepair::Variable variable { 0 };
    ValueID anyDefinition;
};

struct ArmState {
    IterationArm arm;
    PatternRead patternRead { PatternRead::BySteps };
    ValueID subject;
    ValueID nextMethod;
    ValueID isSteppedWithoutCall;
    bool callsSharedFunctions { false };
    std::optional<StateVariable> position;
    std::optional<StateVariable> table;
};

struct StepRequest {
    IteratorKind kind { IteratorKind::Sync };
    ResumeMode mode { ResumeMode::Next };
    ValueID sent;
    bool readsValueWhenDone { false };
};

class IterationExpansion {
    WTF_MAKE_NONCOPYABLE(IterationExpansion);
public:
    IterationExpansion(FunctionPassContext&, IRBuilder&, SSARepair&, IterationOrigin);
    IterationExpansion(const Program&, Function& sharedFunction, TypeArena&, IRBuilder&);

    FunctionPassContext& pass() { return *m_pass; }
    const Program& program() const { return m_program; }
    Function& function() { return m_function; }
    FunctionAnalyses& analyses() { return m_pass->analyses; }
    TypeArena& types() { return m_types; }
    bool isMakingSharedFunction() const { return !m_pass; }
    void returnResultsOfStep(ValueID position)
    {
        m_returnsResultsOfStep = true;
        m_positionInFrontOfStep = position;
        m_positionBehindStep = position;
    }
    IRBuilder& builder() { return m_builder; }
    IterationOrigin origin() const { return m_origin; }

    StateVariable addVariable(ValueID initialDefinition);
    void define(const StateVariable&, ValueID definition);

    void beginOperation(const IRBuilder::Expansion&);
    void finishStep(ValueID done, ValueID value);
    void finishStepAsDone();
    void finishClose();

    ValueID appendResultAt(ValueID producer, uint32_t index);
    ValueID appendIncrement(ValueID integer, int64_t amount);
    ValueID appendIsEqualToInteger(ValueID, int64_t);
    ValueID appendBuiltin(BuiltinID);
    ValueID appendCall(ValueID callee, ValueID thisValue, std::span<const ValueID> arguments);
    ValueID appendPropGet(ValueID object, PropertyKeyID);
    ValueID appendRequireObject(ValueID, RuntimeErrorKind);
    ValueID appendUnspecified(TypeID);
    ValueID appendPair(ValueID first, ValueID second);
    ValueID appendValueOfKind(JSC::IterationKind, ValueID key, ValueID value);

    PropertyKeyID keyOf(WellKnownSymbol);
    PropertyKeyID keyOf(BuiltinName);

private:
    FunctionPassContext* m_pass;
    const Program& m_program;
    Function& m_function;
    TypeArena& m_types;
    IRBuilder& m_builder;
    SSARepair* m_repair;
    IterationOrigin m_origin;
    const IRBuilder::Expansion* m_operation { nullptr };
    bool m_returnsResultsOfStep { false };
    ValueID m_positionInFrontOfStep;
    ValueID m_positionBehindStep;
};

IterationPlan planIteration(FunctionPassContext&, const Iteration&);
IterationPlan planSpread(FunctionPassContext&, ValueID appendSpread);
FunctionID findGeneratorMethodOpening(FunctionPassContext&, ValueID iterable);
ArmDispatch chooseArmDispatch(FunctionPassContext&, const Iteration&, const IterationPlan&, std::span<const BlockID> liveRegion);
Vector<BlockID> findLiveRegion(FunctionPassContext&, ValueID open);
// What a passed test leaves of a type may lie outside it: beside a class that may be related, the part of the tested class loses what it proves.
ValueID chooseArgumentOfProtocolEntry(FunctionPassContext&, BlockID protocolEntry, ValueID narrowed, ValueID tested);

Vector<Iteration> collectIterations(FunctionPassContext&);
PassOutcome expandIterations(FunctionPassContext&, IRBuilder&, std::span<const ValueID> opensOfSpreads, ExpandedIterations);

void emitArmOpen(IterationExpansion&, ArmState&);
void emitArmStep(IterationExpansion&, ArmState&, const StepRequest&);
void emitArmClose(IterationExpansion&, ArmState&, IteratorKind, IteratorCompletion);

void emitIndexedOpen(IterationExpansion&, ArmState&);
void emitArrayStep(IterationExpansion&, ArmState&);
void emitArrayStepByIndex(IterationExpansion&, ArmState&, uint32_t stepIndex);
void emitTypedArrayStep(IterationExpansion&, ArmState&);
void emitStringStep(IterationExpansion&, ArmState&);
TypeID positionTypeOfStringWalk(TypeArena&);
void emitSplitStringStep(IterationExpansion&, ArmState&);
void emitOwnEntriesOpen(IterationExpansion&, ArmState&);
void emitOwnEntriesStep(IterationExpansion&, ArmState&);
void emitCollectionOpen(IterationExpansion&, ArmState&);
void emitCollectionStep(IterationExpansion&, ArmState&);

void emitGeneratorStep(IterationExpansion&, ArmState&, const StepRequest&);
void emitGeneratorClose(IterationExpansion&, ArmState&, IteratorCompletion);
void emitAsyncGeneratorStep(IterationExpansion&, ArmState&, const StepRequest&);
void finishAsyncIteratorStep(IterationExpansion&, ValueID step, const StepRequest&);

void emitProtocolOpen(IterationExpansion&, ArmState&, IteratorKind);
ValueID openWrappedIterator(IterationExpansion&, ValueID iterable);
void emitProtocolStep(IterationExpansion&, ArmState&, const StepRequest&);
void emitProtocolClose(IterationExpansion&, ArmState&, IteratorKind, IteratorCompletion);
void emitIteratorResultReads(IterationExpansion&, ValueID result, bool readsValueWhenDone);
bool isOutlinedIteration(const Function&);
Function createOutlinedIteration(DerivedFunctionInputs&, ASCIILiteral name, std::span<const TypeID> parameters, std::span<const TypeID> results);
Function buildSharedProtocolFunction(OutlinedIteration, DerivedFunctionInputs&);

PatternRead choosePatternRead(FunctionPassContext&, const Iteration&, const IterationArm&, std::span<const BlockID> liveRegion);
bool hasProvenPlainElements(const Type& iterable);
void expandPatternsInOneRead(FunctionPassContext&, IRBuilder&, std::span<const PatternInOneRead>);

void passParametersOfCleanupBlocksOn(FunctionPassContext&, IRBuilder&);
void dropCleanupRegions(FunctionPassContext&, IRBuilder&, std::span<const BlockID> cleanupBlocks);
void releaseCleanupBlocks(FunctionPassContext&, std::span<const BlockID> cleanupBlocks);

PassOutcome expandSpreads(FunctionPassContext&, IRBuilder&, Vector<ValueID>& opensOfSpreads);
PassOutcome expandArrayIteratorSteps(FunctionPassContext&, IRBuilder&);

PassOutcome threadForInPositions(FunctionPassContext&, IRBuilder&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
