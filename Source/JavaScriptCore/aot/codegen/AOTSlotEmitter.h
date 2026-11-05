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
    // Behind holdsValue(): an Extension holds the granule of the slot.
    Operand loadSlotThatHoldsValue(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // For a slot that has a marker for absent: what lies there, markers among it, and absent where the slot has no storage. No branch stands in it for a slot in the cell.
    // Both require an extension that holds the granule of the slot.
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord, const Type& storedType);
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord = nullptr);
    Condition isFilled(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // Whether the bytes of the slot say that it is filled. Where they do not, a field that may exist without storage may still exist.
    Condition holdsValue(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    // Whether the slot holds a value of the type of its field, which lists neither undefined nor null: it is filled, and with no marker.
    Condition holdsDeclaredType(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    bool holdsRawValueFromBirthInSomeMember(LayoutSetID receivers, PropertyKeyID) const;
    // possible: the cell is of one of these layouts. None: of a layout that may bear the mark.
    void markCellIfMarkerIsStored(CellPointer, ValueID stored, LayoutSetID possible = { });

    Pointer extensionOf(CellPointer, const PlacedSlot& extensionWord);
    // Goes to hasNone where the object has no extra properties.
    CellPointer extraPropertiesOf(CellPointer, const PlacedSlot& extraPropertiesWord, LBasicBlock hasNone);
    // For stores in a row with no call between them: nothing has given the object another extension since the caller asked for this one. The caller keeps the cell alive until the last of them.
    void storeInExtension(Pointer extension, const PlacedSlot&, const MemoryLocation&, Operand);
    void storeIfExtensionHolds(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot& extensionWord);
    // An extension that lacks one of the tested granules gets the reserved ones, which include them. A granule that nothing was stored into reads as absent.
    void ensureExtensionHolds(const OperationContext&, CellPointer, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits, OwnerBarrier, bool mayCollect = true);
    void store(TypedPointer, SlotRepresentation, Operand, const Type& storedType);
    // For an operation that declares no collection: neither the stub nor the operation behind it collects.
    void ensureExtensionHoldsWithoutCollecting(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits)
    {
        ensureExtensionHolds(context, cell, extensionWord, testedBits, reservedBits, OwnerBarrier::Emitted, false);
    }

private:
    enum class AbsentReadsAs : uint8_t { Absent, Undefined };

    Operand loadOutOfLine(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord, bool absentReadsAsUndefined = false);
    // Outside the loops of the unit a slot out of line is read by one call: AOT::readFieldInExtension().
    bool readsExtensionByCall(const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord) const;
    // The bytes of the slot, or zero where they have no storage: an Int64 for eight bytes, else an Int32.
    LValue readFieldInExtension(CellPointer, const PlacedSlot&, const MemoryLocation&);
    // What load() makes of the bits that it has read out of a slot that holds neither a float nor a double.
    Operand operandOfBits(LValue bits, SlotRepresentation);
    Operand loadWithoutMarkers(TypedPointer, SlotRepresentation);
    Condition isNoMarker(Operand loaded, SlotRepresentation);
    TypedPointer addressInExtension(const Machine::AbstractHeap&, Pointer extension, uint32_t offset);
    Integer granuleBitsUpTo(Pointer extension, uint32_t granule);
    Condition holdsGranules(Pointer extension, uint64_t granuleBits);

    Operand extensionWordOf(CellPointer, const PlacedSlot& extensionWord);
    Pointer extensionAt(Operand word);
    // The fork stands at the first branch to lacksGranule, so every way on from the test passes it.
    struct TestedExtension {
        Pointer extension;
        ScopeFork fork;
    };
    // `asksForExtensionFirst`: the object is likely to have none. A narrow word that names none leads to a page of zeros, so nothing has to ask.
    TestedExtension branchUnlessExtensionHolds(CellPointer, const PlacedSlot& extensionWord, uint64_t granuleBits, BlockEdge lacksGranule, bool asksForExtensionFirst = false);
    void reserveGranules(const OperationContext&, CellPointer, const PlacedSlot& extensionWord, uint64_t reservedBits, OwnerBarrier, bool mayCollect);
    // Behind an access through an extension. The collector reaches an extension through its cell alone, and a numbered load of the hidden word may be all that the access takes from the cell.
    void keepOwnerOfExtensionAlive(CellPointer cell) { ensureStillAliveHere(cell); }

    OptionalInteger widenOptionalInt32(Word32, AbsentReadsAs);
    Word32 narrowOptionalInt32(OptionalInteger);

    LValue loadBits(TypedPointer, SlotRepresentation);
    void storeBits(TypedPointer, SlotRepresentation, LValue);
    LValue constantWithBits(LType, uint64_t bits);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
