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

bool needsStorageFor(const PlacedSlot&, const Type& stored);

class SlotEmitter final : public OperationEmitter {
public:
    explicit SlotEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    Operand load(TypedPointer, SlotRepresentation);
    void store(TypedPointer, SlotRepresentation, Operand);
    void storeAbsent(TypedPointer, SlotRepresentation);
    void copyBits(TypedPointer from, TypedPointer to, SlotRepresentation slot) { storeBits(to, slot, loadBits(from, slot)); }
    Condition holdsAbsent(TypedPointer, SlotRepresentation);
    Condition isAbsent(Operand loaded, SlotRepresentation);
    Operand absentAsUndefined(Operand loaded, SlotRepresentation);

    TypedPointer addressOf(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Operand loadSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Operand loadSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord, const Type& loadedType);
    Operand loadSlotOrUndefined(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    LValue findSlotWord(LValue loadedOrUndefined) const { return m_wordsOfSlots.get(loadedOrUndefined); }
    Operand loadSlotThatHoldsValue(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Operand loadSlotOrLeaveIfEmpty(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord, BlockEdge isEmpty);
    static constexpr bool storeLooksAtType(SlotRepresentation slot) { return slot == SlotRepresentation::OptionalInt32; }
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord, const Type& storedType);
    void storeSlot(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot* extensionWord = nullptr);
    Condition isFilled(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Condition holdsValue(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    Condition holdsDeclaredType(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot* extensionWord = nullptr);
    bool holdsRawValueFromBirthInSomeMember(LayoutSetID receivers, PropertyKeyID) const;
    void markCellIfMarkerIsStored(CellPointer, ValueID stored, LayoutSetID possible = { });

    Pointer extensionOf(CellPointer, const PlacedSlot& extensionWord);
    CellPointer extraPropertiesOf(CellPointer, const PlacedSlot& extraPropertiesWord, LBasicBlock hasNone);
    void storeInExtension(Pointer extension, const PlacedSlot&, const MemoryLocation&, Operand);
    void storeIfExtensionHolds(CellPointer, const PlacedSlot&, const MemoryLocation&, Operand, const PlacedSlot& extensionWord);
    void ensureExtensionHolds(const OperationContext&, CellPointer, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits, OwnerBarrier, bool mayCollect = true);
    void store(TypedPointer, SlotRepresentation, Operand, const Type& storedType);
    void ensureExtensionHoldsWithoutCollecting(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits)
    {
        ensureExtensionHolds(context, cell, extensionWord, testedBits, reservedBits, OwnerBarrier::Emitted, false);
    }

private:
    enum class AbsentReadsAs : uint8_t { Absent, Undefined };

    Operand loadOutOfLine(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord, bool absentReadsAsUndefined = false);
    Operand wordOutOfLine(CellPointer, const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord);
    bool readsExtensionByCall(const PlacedSlot&, const MemoryLocation&, const PlacedSlot& extensionWord) const;
    LValue readFieldInExtension(CellPointer, const PlacedSlot&, const MemoryLocation&);
    Operand operandOfBits(LValue bits, SlotRepresentation);
    Operand loadWithoutMarkers(TypedPointer, SlotRepresentation);
    Condition isNoMarker(Operand loaded, SlotRepresentation);
    TypedPointer addressInExtension(const Machine::AbstractHeap&, Pointer extension, uint32_t offset);
    Integer granuleBitsUpTo(Pointer extension, uint32_t granule);
    Condition holdsGranules(Pointer extension, uint64_t granuleBits);

    Operand extensionWordOf(CellPointer, const PlacedSlot& extensionWord);
    Pointer extensionAt(Operand word);
    struct TestedExtension {
        Pointer extension;
        ScopeFork fork;
    };
    TestedExtension branchUnlessExtensionHolds(CellPointer, const PlacedSlot& extensionWord, uint64_t granuleBits, BlockEdge lacksGranule, bool asksForExtensionFirst = false);
    void reserveGranules(const OperationContext&, CellPointer, const PlacedSlot& extensionWord, uint64_t reservedBits, OwnerBarrier, bool mayCollect);
    void keepOwnerOfExtensionAlive(CellPointer cell) { ensureStillAliveHere(cell); }

    OptionalInteger widenOptionalInt32(Word32, AbsentReadsAs);
    Word32 narrowOptionalInt32(OptionalInteger);

    LValue loadBits(TypedPointer, SlotRepresentation);
    void storeBits(TypedPointer, SlotRepresentation, LValue);
    LValue constantWithBits(LType, uint64_t bits);

    UncheckedKeyHashMap<LValue, LValue> m_wordsOfSlots;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
