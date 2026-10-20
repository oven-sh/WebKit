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
#include <span>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;
class FunctionAnalyses;

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

// Native code enters a closure through a stub that finds the VM in the header of the block the callee lies in, so it can call no cell of a frame.
// A built-in whose implementation is compiled code calls its operand as any compiled code does.
inline constexpr OperandUses nativeUsesOfFrameCells { OperandUse::Inspects };
inline constexpr OperandUses compiledBuiltinUsesOfFrameCells { OperandUse::Inspects, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };

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

class FramePointers {
public:
    explicit FramePointers(const Function&);
    // Leaves out the values whose type holds no reference, as the escape analysis does.
    FramePointers(const Program&, const Function&);

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
    FramePointers(const Program*, const Function&);

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
bool propagatesFramePointers(Opcode);
ValueID cellAccessedBy(const Value& followedAccess);

void forEachReturnedOperand(const Function&, const Value&, NOESCAPE const ScopedLambda<void(ValueID)>&);

// No use of an Unspecified value is reached. Whoever moves a value to where paths run that never reach a use of it, or hands it to another function, asks first.
bool mayBeUnspecified(FunctionAnalyses&, ValueID);

OperandSummary summarizeFramePointerUse(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
OperandSummary summarizeFramePointerUse(const Program&, const FlowResults*, const Function&, const Terminator& user, uint32_t operandIndex);

OperandUses usesOfFrameCellsAllowedIn(const Program&, const Value& builtinCall);
bool mayLeakFramePointer(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
bool mayLeakFramePointer(const Program&, const FlowResults*, const Function&, const Terminator& user, uint32_t operandIndex);
bool mayLeakContents(const Program&, const FlowResults*, const Function&, const Value& user, uint32_t operandIndex);
bool mayLeakCapture(const FlowResults*, const Value& closure, uint32_t captureIndex);
bool mayLeakStoredValue(const FlowResults*, FieldReference environmentField);

void verifyFramePointers(FunctionAnalyses&, Vector<InternalError>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
