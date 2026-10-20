/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMemoryLocation.h"
#include "AOTOperationEmitter.h"
#include "AOTSlotRepresentation.h"

namespace JSC { namespace AOT { namespace Typed {

struct PlacedSlot;

inline constexpr Range inlineInt32Range { minInlineInt32, INT32_MAX };

// Whether a store of any value of the type writes the cell alone: the slot is not in the extension, and the word of an Int32OrDouble slot can hold the value.
bool isStoredInCell(const PlacedSlot&, const Type& stored);

class SlotEmitter final : public OperationEmitter {
public:
    explicit SlotEmitter(CodeGenerator&);

    Operand load(TypedPointer, SlotRepresentation);
    void store(TypedPointer, SlotRepresentation, Operand);
    void storeAbsent(TypedPointer, SlotRepresentation);
    Condition holdsAbsent(TypedPointer, SlotRepresentation);
    Condition isAbsent(Operand loaded, SlotRepresentation);
    Operand absentAsUndefined(Operand loaded, SlotRepresentation);

    TypedPointer addressOf(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Operand loadSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // What the slot holds has the type, so a slot for integers and markers that holds no marker is read as its word.
    Operand loadSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord, const Type& loadedType);
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord, const Type& storedType);
    // Requires an extension that holds the granule of the slot, or of the double of an Int32OrDouble slot.
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord = nullptr);
    void storeSlot(const OperationContext&, CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord = nullptr);
    Condition isFilled(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);

    Pointer extensionOf(CellPointer, const PlacedSlot& extensionWord);
    // For stores in a row with no call between them: every store to a field makes B3 load the hidden word again, and the caller knows that it has not changed.
    void storeInExtension(Pointer extension, const PlacedSlot&, const MemoryLocation&, Operand);
    void ensureExtensionHolds(const OperationContext&, CellPointer, const PlacedSlot& extensionWord, uint64_t granuleBits);

private:
    Operand loadOutOfLine(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord);
    Operand loadInt32OrDouble(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord);
    void storeInt32OrDouble(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, NOESCAPE const Invocable<void(Number)> auto& storeDouble);
    void storeDoubleInExtension(const OperationContext&, CellPointer, const PlacedSlot&, const MemoryLocation&, Number, const PlacedSlot& extensionWord);
    TypedPointer addressOfDouble(Pointer extension, const PlacedSlot&, const MemoryLocation&);
    TypedPointer addressInExtension(const B3::AbstractHeap&, Pointer extension, uint32_t offset);
    Integer granuleBitsUpTo(Pointer extension, uint32_t granule);
    Condition holdsGranules(Pointer extension, uint64_t granuleBits);

    Operand extensionWordOf(CellPointer, const PlacedSlot& extensionWord);
    Pointer extensionAt(Operand word);
    Pointer branchUnlessExtensionHolds(CellPointer, const PlacedSlot& extensionWord, uint64_t granuleBits, B3::FrequentedBlock lacksGranule);

    OptionalInteger widenOptionalInt32(Word32);
    Word32 narrowOptionalInt32(OptionalInteger);

    LValue loadBits(TypedPointer, SlotRepresentation);
    void storeBits(TypedPointer, SlotRepresentation, LValue);
    LValue constantWithBits(LType, uint64_t bits);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
