/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFrameAllocation.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CFG;
class FlowResults;
class FunctionAnalyses;
class UseLists;
struct Use;

inline constexpr OperandUses usesLeakingFramePointers {
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::RetainsContents,
    OperandUse::BecomesPrototype,
    OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

// These say what becomes of the values a cell holds. The pointer to the cell goes nowhere.
inline constexpr OperandUses usesLeakingContents {
    OperandUse::RetainsContents,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

// Code that the compiler emits in place of a built-in may look at a cell of a frame.
inline constexpr OperandUses inlineUsesOfFrameCells { OperandUse::Inspects };

// Native code gets no cell of a frame, not even to look at.
constexpr bool mayLookAtFrameCells(EffectRule rule) { return rule == EffectRule::Inline; }

inline constexpr OperandUses usesChangingFrameCells {
    OperandUse::MovesElements,
    OperandUse::StoresByKey,
    OperandUse::DeletesByKey,
    OperandUse::AssignsFrom,
    OperandUse::DefinesName,
    OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity,
    OperandUse::ChangesPrototype,
};

inline constexpr OpcodeSet storesFollowedByFramePointers { Opcode::PutEnvironmentField, Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::PutSlots };
inline constexpr OpcodeSet readsFollowedByFramePointers { Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::GetSlot, Opcode::GetSlotOrUndefined };

enum class FrameCells : uint8_t { All, Arrays, AllButArrays };

class FramePointers {
public:
    explicit FramePointers(const Function&);
    // Leaves out the values whose type holds no reference, as the escape analysis does.
    FramePointers(const Program&, const Function&, FrameCells = FrameCells::All);

    bool isEmpty() const { return m_allocations.isEmpty(); }
    bool contains(ValueID id) const { return m_values.get(id.index()); }
    const BitVector& values() const LIFETIME_BOUND { return m_values; }
    std::span<const ValueID> allocations() const LIFETIME_BOUND { return m_allocations.span(); }

    std::span<const FieldReference> environmentFieldsWithPointers() const LIFETIME_BOUND { return m_environmentFieldsWithPointers.span(); }
    std::span<const PropertyKeyID> keysWithPointers() const LIFETIME_BOUND { return m_keysWithPointers.span(); }
    std::span<const CaptureReference> capturesWithPointers() const LIFETIME_BOUND { return m_capturesWithPointers.span(); }

    // A store is known by its reference alone, so a cell counts as holding what any cell with the same reference holds.
    bool mayHoldPointers(const Program&, const Function&, ValueID cell) const;

private:
    FramePointers(const Program*, const Function&, FrameCells);

    bool propagate(const Function&, ValueID);
    bool propagateStore(const Function&, const Value&);
    bool readsPointer(const Function&, const Value&) const;
    bool add(const Function&, ValueID);

    const Program* m_program;
    BitVector m_values;
    Vector<ValueID> m_allocations;
    Vector<FieldReference, 4> m_environmentFieldsWithPointers;
    Vector<PropertyKeyID, 4> m_keysWithPointers;
    Vector<CaptureReference, 4> m_capturesWithPointers;
};

bool hasFrameStorage(const Value&);
bool isFrameCell(const Function&, ValueID);

// A closure that is lent lies in the frame and is an argument of one call by value, which nobody knows the callees of.
bool mayLendArgument(const Value& call, uint32_t operandIndex);
bool isLentTo(const Function&, const Value& user, uint32_t operandIndex);
// One bit for each argument that is lent.
uint8_t argumentsLentBy(const Function&, const Value& call);
// Whether nothing takes the value any more once this use of it has run, until the value is made again. The uses are all uses of the value, those of its casts among them.
bool isLastUse(const Function&, const CFG&, const UseLists&, ValueID definition, std::span<const Use> uses, const Use&);

inline constexpr OpcodeSet arrayAllocationsWithStorage { Opcode::NewArray, Opcode::CreateRest };

// What AOTFrameArrayParameters.h says of an array in a frame holds for these: a literal of values without holes that is as long as it will ever be, and the list of the surplus arguments of a frame.
bool canLieInFrame(const Program&, const BuildContext&, const Value& allocation);
// Those that no operation but a call stands in the way of, as far as the operations say that take the allocation itself for an operand. Most arrays are stored or returned where they are made, and are left out here for the price of one walk.
Vector<ValueID, 8> findArraysThatMayLieInFrame(const Program&, const BuildContext&, const Function&);
// The result is the operand.
bool givesSameArray(Opcode, uint32_t operandIndex);
// Compiled code and stubs look at the header, the length and the elements.
bool readsArrayWhereItLies(const Program&, const Function&, const Value& user, uint32_t operandIndex);
bool mayHandFrameArrayTo(const Program&, const FlowResults*, const Value& call, uint32_t operandIndex);
// Every use that an array in a frame may have, in the function that makes it and in those it is handed to. The planner asks before it puts one there, the verifier behind every pass, and a validating build at run time, at every other use.
bool mayBeGivenFrameArray(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);

struct FrameArrayTest {
    ValueID tested;
    bool passes { false };
};
// How the test ends for every array in a frame. Nothing where that depends on the array, or the operation is no such test.
std::optional<FrameArrayTest> testOfFrameArray(const Program&, const Value& test);
bool propagatesFramePointers(Opcode);
ValueID cellAccessedBy(const Value& followedAccess);

void forEachReturnedOperand(const Function&, const Value&, NOESCAPE const ScopedLambda<void(ValueID)>&);
// False where forEachReturnedOperand() names no operand of any value with the opcode.
bool mayReturnOperand(Opcode);

// No use of an Unspecified value is reached. Whoever moves a value to where paths run that never reach a use of it, or hands it to another function, asks first.
bool mayBeUnspecified(FunctionAnalyses&, ValueID);

// The same answers for many values of a function that stays as it is: what was found to be specified is not walked again.
class UnspecifiedValues {
public:
    explicit UnspecifiedValues(FunctionAnalyses&);

    bool mayContain(ValueID);

private:
    FunctionAnalyses& m_analyses;
    bool m_hasSource;
    BitVector m_isSpecified;
    Vector<ValueID, 16> m_visited;
};

OperandSummary summarizeFramePointerUse(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
OperandSummary summarizeFramePointerUse(const Program&, const FlowResults*, const Function&, const Terminator& user, uint32_t operandIndex);

bool mayLeakFramePointer(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
bool mayLeakFramePointer(const Program&, const FlowResults*, const Function&, const Terminator& user, uint32_t operandIndex);
bool mayLeakContents(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
bool mayLeakCapture(const Program&, const FlowResults*, const Value& closure, uint32_t captureIndex);
bool mayLeakStoredValue(const FlowResults*, FieldReference environmentField);

// True of an environment no field of which may hold a pointer into a frame, so what is read out of it may go anywhere. False of every other type.
bool holdsOnlyHeapValues(const Program&, const FlowResults*, TypeID);

void verifyFramePointers(FunctionAnalyses&, Vector<InternalError>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
