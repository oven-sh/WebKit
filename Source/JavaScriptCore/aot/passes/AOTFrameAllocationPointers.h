/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTDiagnostics.h"
#include "AOTEdgeResults.h"
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
inline constexpr OperandUses unfollowedLeakingUses = usesLeakingFramePointers - usesLeakingContents - OperandUses { OperandUse::Retains, OperandUse::Returns };

// Code that the compiler emits in place of a built-in may look at a cell of a frame. Native code gets none, not even to look at.
inline constexpr OperandUses inlineUsesOfFrameCells { OperandUse::Inspects };
constexpr bool mayLookAtFrameCells(EffectRule rule) { return rule == EffectRule::Inline; }

inline constexpr OpcodeSet storesFollowedByFramePointers { Opcode::PutEnvironmentField, Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::PutSlots };
inline constexpr OpcodeSet readsFollowedByFramePointers { Opcode::GetEnvironmentField, Opcode::GetCapture, Opcode::GetSlot, Opcode::GetSlotOrUndefined };
inline constexpr OpcodeSet arrayAllocationsWithStorage { Opcode::NewArray, Opcode::CreateRest };

// Where a value lies in a cell, as a store or a read names it: a field of a layout, a capture of a function, or a key. Which cell it is they do not say, so a cell counts as holding what any cell holds at the same place.
struct CellPlace {
    enum class Kind : uint8_t { Field, Capture, Key };

    Kind kind { Kind::Key };
    uint32_t owner { 0 };
    uint32_t index { 0 };

    friend bool operator==(const CellPlace&, const CellPlace&) = default;
};
// Of an allocation with initial values, or of a followed store. Nothing for an operand that is not put into the cell.
std::optional<CellPlace> placeStoredTo(const Value& store, uint32_t operandIndex);
CellPlace placeReadBy(const Value& followedRead);
bool cellHasPlace(const Program&, const Value& allocation, const CellPlace&);
ValueID cellAccessedBy(const Value& followedAccess);

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
    std::span<const CellPlace> placesWithPointers() const LIFETIME_BOUND { return m_placesWithPointers.span(); }
    bool mayHoldPointers(const Program&, const Function&, ValueID cell) const;

private:
    FramePointers(const Program*, const Function&, FrameCells);

    bool propagate(const Function&, ValueID);
    bool add(const Function&, ValueID);

    const Program* m_program;
    BitVector m_values;
    Vector<ValueID> m_allocations;
    Vector<CellPlace, 4> m_placesWithPointers;
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

// What AOTFrameArrayParameters.h says of an array in a frame holds for these: a literal of values without holes that is as long as it will ever be, and the list of the surplus arguments of a frame.
bool canLieInFrame(const Program&, const BuildContext&, const Value& allocation);
// Those that no operation but a call stands in the way of, as far as the operations say that take the allocation itself for an operand.
Vector<ValueID, 8> findArraysThatMayLieInFrame(const Program&, const BuildContext&, const Function&);
bool givesSameArray(Opcode, uint32_t operandIndex);
// Compiled code and stubs look at the header, the length and the elements.
bool readsArrayWhereItLies(const Program&, const Function&, const Value& user, uint32_t operandIndex);
// Every use that an array in a frame may have, in the function that makes it and in those it is handed to. The planner asks before it puts one there, the verifier behind every pass, and a validating build at run time, at every other use.
bool mayBeGivenFrameArray(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);

struct FrameArrayTest {
    ValueID tested;
    bool passes { false };
};
// How the test ends for every array in a frame. Nothing where that depends on the array, or the operation is no such test.
std::optional<FrameArrayTest> testOfFrameArray(const Program&, const Value& test);
bool propagatesFramePointers(Opcode);

void forEachReturnedOperand(const Function&, const Value&, NOESCAPE const ScopedLambda<void(ValueID)>&);
// False where forEachReturnedOperand() names no operand of any value with the opcode.
bool mayReturnOperand(Opcode);

// functor(passed, parameter) for the arguments of the edge and for the operands that its terminator narrows.
void forEachValuePassedOn(const Function& function, Edge edge, const Target& target, NOESCAPE const Invocable<void(ValueID, ValueID)> auto& functor)
{
    const CompactList<ValueID>& parameters = function.at(target.block).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (ValueID passed = valuePassedOn(function, edge, index))
            functor(passed, parameters[index]);
    }
}

// Both visitors say whether they changed anything: visitValue(block, value), visitPassedValue(edge, passed, parameter). The blocks are BlockIDs or their numbers.
void propagateUntilNothingChanges(const Function& function, const auto& blocks, NOESCAPE const Invocable<bool(BlockID, ValueID)> auto& visitValue, NOESCAPE const Invocable<bool(Edge, ValueID, ValueID)> auto& visitPassedValue)
{
    for (bool changed = true; changed;) {
        changed = false;
        for (auto entry : blocks) {
            BlockID block(entry);
            for (ValueID id : function.at(block).values)
                changed |= visitValue(block, id);
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                forEachValuePassedOn(function, edge, target, [&](ValueID passed, ValueID parameter) {
                    changed |= visitPassedValue(edge, passed, parameter);
                });
            });
        }
    }
}

OperandSummary summarizeFramePointerUse(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
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
