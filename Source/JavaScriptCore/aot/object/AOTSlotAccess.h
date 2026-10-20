/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "JSCJSValue.h"
#include "MarkedBlockCage.h"
#include <wtf/Assertions.h>

namespace JSC { namespace AOT {

class ProgramStrings;

ALWAYS_INLINE JSCell* decodeCell32(const HeapCell* holder, uint32_t word)
{
    ASSERT(word >= minCell32);
    return MarkedBlockCage::decode(cageBaseOf(holder), word);
}

ALWAYS_INLINE uint32_t encodeCell32(const HeapCell* holder, const JSCell* cell)
{
    return MarkedBlockCage::encode(cageBaseOf(holder), cell);
}

inline uint64_t readSlotBits(SlotRepresentation representation, const void* slot)
{
    switch (widthOf(representation)) {
    case 1:
        return *static_cast<const uint8_t*>(slot);
    case 2:
        return *static_cast<const uint16_t*>(slot);
    case 4:
        return *static_cast<const uint32_t*>(slot);
    case 8:
        return *static_cast<const uint64_t*>(slot);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline bool slotHoldsAbsent(SlotRepresentation representation, const void* slot)
{
    SlotTraits traits = traitsOf(representation);
    return traits.hasAbsent && readSlotBits(representation, slot) == traits.absentBits;
}

JSValue readSlot(SlotRepresentation, const void* slot, const ProgramStrings&);
bool tryWriteSlot(ProgramStrings&, SlotRepresentation, void* slot, JSValue);
bool tryWriteInt32(SlotRepresentation, void* slot, int32_t);
bool tryWriteDouble(SlotRepresentation, void* slot, double);
bool tryWriteBoolean(SlotRepresentation, void* slot, bool);
void clearSlot(SlotRepresentation, void* slot);
void writeSlotBits(SlotRepresentation, void* slot, uint64_t bits);
JSCell* cellInSlot(SlotRepresentation, const void* slot, uintptr_t cageBase);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
