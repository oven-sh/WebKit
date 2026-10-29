/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTForwardedCalls.h"
#include "AOTIR.h"
#include "AOTInductionVariables.h"
#include "AOTMethodDispatch.h"
#include "AOTOpcode.h"
#include "AOTPass.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

struct OptimizationInputs;
class PassRunner;
class ProgramEditor;

inline constexpr OpcodeSet constructionAllocations { Opcode::NewArrayLike, Opcode::NewArrayWithCapacity };
inline constexpr OpcodeSet arrayCapacityOpcodes = constructionAllocations | OpcodeSet { Opcode::NewArray, Opcode::AppendElement, Opcode::CallBuiltin, Opcode::HasArrayStorage };

struct MeasuredStorage {
    ValueID owner;
    ValueID guard;
    AbstractHeap heap;
};

std::optional<MeasuredStorage> storageMeasuredBy(const Program&, const Value& lengthRead);

// A variable that is stored once, by code that runs once, with a value that nobody can tell when it was made, stays empty until its first read.
void deferInitializers(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

inline constexpr OpcodeSet testsThatProveForStretch = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        if (provesForStretch(provesOf(static_cast<Opcode>(index))))
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

// EliminateGuards folds branches on these tests and tidies lists of guards. A body that holds none of them gives it nothing to do.
inline constexpr OpcodeSet opcodesOfGuardElimination = testsThatProveForStretch | OpcodeSet { Opcode::BelongsToInstance, Opcode::AllGuards };

inline constexpr AbstractHeapSet accessedHeapSet { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields, AbstractHeap::ObjectFields };

// What memoryAccessOf() may take for a read of one of the heaps.
inline constexpr OpcodeSet opcodesReadingAccessedHeaps = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (resultRuleOf(opcode) == ResultRule::Declared && effectRuleOf(opcode) == EffectRule::Declared && declaredBoundOf(opcode).reads.containsAny(accessedHeapSet))
            opcodes.add(opcode);
    }
    return opcodes;
}();

// EliminateRedundantReads replaces such reads and drops a call of a built-in that repeats the call in front of it, a test for a brand that the object has taken and a comparison of two values that were compared before. A body that holds none of these gives it nothing to do.
inline constexpr OpcodeSet opcodesOfRedundantReadElimination = opcodesReadingAccessedHeaps | OpcodeSet { Opcode::CallBuiltin, Opcode::InitializeBuiltin, Opcode::PrivateGet, Opcode::StrictEq, Opcode::GetStringLength };

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

enum class OutlinedIteration : uint32_t { AppendIterable = 1, OpenIterator, CloseIterator, CloseIteratorAfterThrow, OpenWrappedIterator, OpenStringOrIterator, StepString, CloseStringOrIterator, CloseStringOrIteratorAfterThrow };

bool isOutlinedIteration(const Function&);

// A parameter, a constant and a cast stay what they are, and so does a test whose proof lasts for a stretch.
inline constexpr OpcodeSet foldableOpcodes = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (opcode != Opcode::Parameter && resultRuleOf(opcode) != ResultRule::Constant && !isCast(opcode) && !provesForStretch(provesOf(opcode)))
            opcodes.add(opcode);
    }
    return opcodes;
}();

inline constexpr OpcodeSet hoistableReadOpcodes { Opcode::GetModuleVariable, Opcode::GetEncodedModuleVariable, Opcode::GetEnvironmentField };
inline constexpr OpcodeSet lookupsHoistedWhereTheyRunNoCode { Opcode::PropGet };
// Whether a value is truthy and whether two values are strictly equal never changes, and neither test runs code of the program.
inline constexpr OpcodeSet testsOfValuesThemselves { Opcode::ToBoolean, Opcode::StrictEq };
inline constexpr OpcodeSet hoistableOpcodes = hoistableReadOpcodes | lookupsHoistedWhereTheyRunNoCode | testsOfValuesThemselves | OpcodeSet { Opcode::ForInStep, Opcode::CallBuiltin };

inline constexpr AbstractHeapSet heapsOfVariables { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields };

// `if (n % d === 0) n /= d`: the quotient, which stands in the block, comes behind a test that the same two numbers leave no remainder.
bool followsTestForNoRemainder(const Program&, const Function&, const CFG&, BlockID, const Value& quotient);

// Up to the first operation of this kind, the first turn of a loop is sure to go on, and it finds in every variable what was there ahead of the loop.
constexpr bool mayLeaveOrStoreToVariable(const Effects& effects)
{
    return effects.mayThrowByRule() || effects.mayRunOtherCode() || !effects.writes.restrictedTo(heapsOfVariables).isEmpty();
}

// These convert a number to int32, which costs nothing where the number is an integer.
inline constexpr OpcodeSet operatorsOnInt32 { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };

inline constexpr OpcodeSet operatorsHoistedIntoPredecessors = relationalOperators | operatorsOnInt32;

// The test of a parameter for truthiness that a block branches on, or the first operator or guard of a block.
inline constexpr OpcodeSet operationsHoistedIntoPredecessors = operatorsHoistedIntoPredecessors | OpcodeSet { Opcode::ToBoolean, Opcode::Expect };

inline constexpr OpcodeSet objectCoercibleCheckOpcodes {
    Opcode::PropGet, Opcode::ElemGet, Opcode::PropSet, Opcode::ElemSet, Opcode::IteratorOpen, Opcode::RequireObjectCoercible
};

void removeUnusedParameters(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

void removeEntryParameters(Function&, const BitVector& removedEntryParameters);

void removeUnusedResults(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

void proveResults(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

// The operations that ResolveCalls looks at.
inline constexpr OpcodeSet callSiteOpcodes {
    Opcode::Call,
    Opcode::CallList,
    Opcode::Construct,
    Opcode::ConstructList,
    Opcode::SuperConstruct,
    Opcode::SuperConstructList,
    Opcode::PropGet,
    Opcode::PropSet,
    Opcode::SuperGet,
    Opcode::SuperSet,
    Opcode::InstanceOf,
    Opcode::CallBuiltin,
};

bool canTakeAllocatedThis(const Program&, FunctionID constructor);
// The class of the constructor, or the layout derived from it that this version of the constructor allocates.
LayoutID layoutAllocatedBy(const Program&, FunctionID constructor);
bool isConstructorFunction(const Program&, const Function&);
// What `new` hands a constructor function that is its own new.target: an object with the layout of the function. `closure` is that function.
ValueID appendThisOfConstructorFunction(IRBuilder&, FunctionID constructor, ValueID closure);
// Ends the block: the object is returned unless the constructor has returned another.
void emitReturnOfConstruction(IRBuilder&, ValueID returned, ValueID object);

struct MemberRead {
    Vector<ValueSite, 2> links;
    // What a validating build looks at the links with. It goes with them.
    Vector<ValueID, 2> validations;
    bool canSinkToCall { false };

    bool isEmpty() const { return links.isEmpty(); }
    const ValueSite& read() const LIFETIME_BOUND { return links.first(); }
};

MemberRead findMemberRead(FunctionPassContext&, const ValueSite& call);

// SimplifyCasts has nothing to do in a function without any of these.
inline constexpr OpcodeSet simplifiableCasts = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (isCast(opcode) && opcode != Opcode::FinishObject)
            opcodes.add(opcode);
    }
    return opcodes;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
