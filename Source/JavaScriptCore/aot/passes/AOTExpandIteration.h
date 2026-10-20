/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTSSARepair.h"
#include "AOTUseLists.h"
#include "IterationKind.h"
#include "TypedArrayType.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class IterationForm : uint8_t { Protocol, Array, TypedArray, String, SplitString, Map, Set, Generator, AsyncGenerator };
enum class IterationOrigin : uint8_t { Source, Spread };
enum class ArmDispatch : uint8_t { SingleArm, ByForm, Versioned };
enum class PatternRead : uint8_t { BySteps, ByIndex, InOneRead };

constexpr bool closingHasNoEffect(IterationForm form)
{
    switch (form) {
    case IterationForm::Array:
    case IterationForm::TypedArray:
    case IterationForm::String:
    case IterationForm::SplitString:
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

struct StateVariable {
    SSARepair::Variable variable { 0 };
    ValueID anyDefinition;
};

struct ArmState {
    IterationArm arm;
    PatternRead patternRead { PatternRead::BySteps };
    ValueID subject;
    ValueID nextMethod;
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

    FunctionPassContext& pass() { return m_pass; }
    const Program& program() const { return m_pass.program; }
    Function& function() { return m_pass.function; }
    FunctionAnalyses& analyses() { return m_pass.analyses; }
    TypeArena& types() { return m_pass.types; }
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
    FunctionPassContext& m_pass;
    IRBuilder& m_builder;
    SSARepair& m_repair;
    IterationOrigin m_origin;
    const IRBuilder::Expansion* m_operation { nullptr };
};

IterationPlan planIteration(FunctionPassContext&, const Iteration&);
IterationPlan planSpread(FunctionPassContext&, ValueID appendSpread);
ArmDispatch chooseArmDispatch(FunctionPassContext&, const Iteration&, const IterationPlan&, std::span<const BlockID> liveRegion);
Vector<BlockID> findLiveRegion(FunctionPassContext&, ValueID open);

Vector<Iteration> collectIterations(FunctionPassContext&);
void expandOneIteration(FunctionPassContext&, IRBuilder&, const Iteration&);

void emitArmOpen(IterationExpansion&, ArmState&);
void emitArmStep(IterationExpansion&, ArmState&, const StepRequest&);
void emitArmClose(IterationExpansion&, ArmState&, IteratorKind, IteratorCompletion);

void emitIndexedOpen(IterationExpansion&, ArmState&);
void emitArrayStep(IterationExpansion&, ArmState&);
void emitArrayStepByIndex(IterationExpansion&, ArmState&, uint32_t stepIndex);
void emitTypedArrayStep(IterationExpansion&, ArmState&);
void emitStringStep(IterationExpansion&, ArmState&);
void emitSplitStringStep(IterationExpansion&, ArmState&);
void emitCollectionOpen(IterationExpansion&, ArmState&);
void emitCollectionStep(IterationExpansion&, ArmState&);

void emitGeneratorStep(IterationExpansion&, ArmState&, const StepRequest&);
void emitGeneratorClose(IterationExpansion&, ArmState&, IteratorCompletion);
void emitAsyncGeneratorStep(IterationExpansion&, ArmState&, const StepRequest&);

void emitProtocolOpen(IterationExpansion&, ArmState&, IteratorKind);
void emitProtocolStep(IterationExpansion&, ArmState&, const StepRequest&);
void emitProtocolClose(IterationExpansion&, ArmState&, IteratorKind, IteratorCompletion);
void emitIteratorResultReads(IterationExpansion&, ValueID result, bool readsValueWhenDone);

PatternRead choosePatternRead(FunctionPassContext&, const Iteration&, const IterationArm&);
bool hasProvenPlainElements(const Type& iterable);
void expandPatternInOneRead(FunctionPassContext&, IRBuilder&, const Iteration&, ValueID iterable);

void dropCleanupRegions(FunctionPassContext&, IRBuilder&, std::span<const BlockID> cleanupBlocks);
void releaseCleanupBlocks(FunctionPassContext&, std::span<const BlockID> cleanupBlocks);

PassOutcome expandSpreads(FunctionPassContext&, IRBuilder&, Vector<ValueID>& opensOfSpreads);
PassOutcome expandArrayIteratorSteps(FunctionPassContext&, IRBuilder&);

PassOutcome threadForInPositions(FunctionPassContext&, IRBuilder&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
