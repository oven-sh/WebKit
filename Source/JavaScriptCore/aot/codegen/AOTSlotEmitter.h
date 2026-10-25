/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTMemoryLocation.h"
#include "AOTOperationEmitter.h"
#include "AOTSlotRepresentation.h"

namespace JSC { namespace AOT { namespace Typed {

struct PlacedSlot;

// A field that may exist without storage reads as undefined while it has none, so a store of undefined asks for no bytes.
bool needsStorageFor(const PlacedSlot&, const Type& stored);

class SlotEmitter final : public OperationEmitter {
public:
    explicit SlotEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    Operand load(TypedPointer, SlotRepresentation);
    void store(TypedPointer, SlotRepresentation, Operand);
    void storeAbsent(TypedPointer, SlotRepresentation);
    Condition holdsAbsent(TypedPointer, SlotRepresentation);
    Condition isAbsent(Operand loaded, SlotRepresentation);
    Operand absentAsUndefined(Operand loaded, SlotRepresentation);

    TypedPointer addressOf(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Operand loadSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // What the slot holds has the type, so a slot for numbers or booleans and markers that holds no marker is read as its bits.
    Operand loadSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord, const Type& loadedType);
    // For a slot that has a marker for undefined.
    Operand loadSlotOrUndefined(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // Both require an extension that holds the granule of the slot.
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord, const Type& storedType);
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord = nullptr);
    Condition isFilled(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // Whether the bytes of the slot say that it is filled. Where they do not, a field that may exist without storage may still exist.
    Condition holdsValue(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // Whether the slot holds a value of the type of its field, which lists neither undefined nor null: it is filled, and with no marker.
    Condition holdsDeclaredType(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    bool holdsRawValueFromBirthInSomeMember(LayoutSetID receivers, PropertyKeyID) const;
    void markCellIfMarkerIsStored(CellPointer, ValueID stored);

    Pointer extensionOf(CellPointer, const PlacedSlot& extensionWord);
    // For stores in a row with no call between them: every store to a field makes B3 load the hidden word again, and the caller knows that it has not changed.
    void storeInExtension(Pointer extension, const PlacedSlot&, const MemoryLocation&, Operand);
    void storeIfExtensionHolds(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot& extensionWord);
    Condition hasNoExtension(CellPointer, const PlacedSlot& extensionWord);
    // An extension that lacks one of the tested granules gets the reserved ones, which include them. A granule that nothing was stored into reads as absent.
    void ensureExtensionHolds(const OperationContext&, CellPointer, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits, OwnerBarrier, bool mayCollect = true);
    // For an operation that declares no collection: neither the stub nor the operation behind it collects.
    void ensureExtensionHoldsWithoutCollecting(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t granuleBits)
    {
        ensureExtensionHolds(context, cell, extensionWord, granuleBits, granuleBits, OwnerBarrier::Emitted, false);
    }
    void ensureExtensionHolds(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t granuleBits, OwnerBarrier barrier) { ensureExtensionHolds(context, cell, extensionWord, granuleBits, granuleBits, barrier); }

private:
    enum class AbsentReadsAs : uint8_t { Absent, Undefined };

    Operand loadOutOfLine(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord);
    Operand loadWithoutMarkers(TypedPointer, SlotRepresentation);
    Condition isNoMarker(Operand loaded, SlotRepresentation);
    TypedPointer addressInExtension(const B3::AbstractHeap&, Pointer extension, uint32_t offset);
    Integer granuleBitsUpTo(Pointer extension, uint32_t granule);
    Condition holdsGranules(Pointer extension, uint64_t granuleBits);

    Operand extensionWordOf(CellPointer, const PlacedSlot& extensionWord);
    Pointer extensionAt(Operand word);
    Pointer branchUnlessExtensionHolds(CellPointer, const PlacedSlot& extensionWord, uint64_t granuleBits, B3::FrequentedBlock lacksGranule);

    OptionalInteger widenOptionalInt32(Word32, AbsentReadsAs);
    Word32 narrowOptionalInt32(OptionalInteger);

    LValue loadBits(TypedPointer, SlotRepresentation);
    void storeBits(TypedPointer, SlotRepresentation, LValue);
    LValue constantWithBits(LType, uint64_t bits);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
