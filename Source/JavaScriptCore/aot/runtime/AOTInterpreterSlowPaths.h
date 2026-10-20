/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "AOTSlotRepresentation.h"
#include "ArrayAllocationProfile.h"
#include "ArrayConventions.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "JSCJSValue.h"
#include "PureNaN.h"
#include <algorithm>
#include <optional>

namespace JSC {

class Identifier;
class JSCell;
class JSGlobalObject;
class JSObject;
class VM;

namespace AOT {

inline constexpr uint32_t kindsOfPresentProperties = [] {
    uint32_t kinds = 0;
    for (Slot::Kind kind : allSlotKinds) {
        if (Slot::presenceOf(kind) == SlotPresence::Present)
            kinds |= 1u << static_cast<unsigned>(kind);
    }
    return kinds;
}();
static_assert(numberOfSlotKinds <= 32);

// What llint/LowLevelInterpreterAOT.asm spells out by hand.
static_assert(HashedSlot::ways == 4 && HashedSlot::offsetOfWay(0, 1) == sizeof(HashedSlot) && HashedSlot::offsetOfWay(0, 2) == 2 * sizeof(HashedSlot) && HashedSlot::offsetOfWay(0, 3) == 3 * sizeof(HashedSlot));
static_assert(!static_cast<uintptr_t>(SlotAccess::Read));
static_assert(undefinedCell32 == JSValue::ValueUndefined && emptyLiteralStringWord > undefinedCell32 && minCell32 > undefinedCell32);
static_assert(std::max({ absentInt32, undefinedInt32, nullInt32, doubleInExtensionInt32 }) < minInlineInt32);

// Up to MarkedSpace::preciseCutoff every multiple of MarkedSpace::sizeStep is a size class, so Butterfly::optimalContiguousVectorLength()
// rounds the indexing header and the elements up to two words.
inline constexpr unsigned maxLengthOfArrayLiteralAllocatedInline = (MarkedSpace::preciseCutoff - sizeof(IndexingHeader)) / sizeof(EncodedJSValue);
static_assert(MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(IndexingHeader) == sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*));
static_assert(maxLengthOfArrayLiteralAllocatedInline % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN_EMPTY % 2 == 1 && BASE_CONTIGUOUS_VECTOR_LEN_EMPTY <= maxLengthOfArrayLiteralAllocatedInline);
static_assert(MarkedSpace::sizeClassToIndex(MarkedSpace::preciseCutoff) * MarkedSpace::sizeStep == MarkedSpace::preciseCutoff);
static_assert(!IndexingHeader::offsetOfPublicLength() && IndexingHeader::offsetOfVectorLength() == 4);
inline constexpr uint64_t holeInDoubleArray = std::bit_cast<uint64_t>(PNaN);

// An ArrayAllocationProfile is a CompactPointerTuple of the last array and (indexing type << 8 | vector length).
inline constexpr unsigned bitsOfPointerInArrayAllocationProfile = 48;
inline constexpr unsigned bitsOfTypeInArrayAllocationProfile = 64 - bitsOfPointerInArrayAllocationProfile;
inline constexpr unsigned shiftOfIndexingTypeInArrayAllocationProfile = bitsOfPointerInArrayAllocationProfile + 8;
static_assert(sizeof(ArrayAllocationProfile) == sizeof(uint64_t) && CompactPointerTuple<JSArray*, uint16_t>::maxNumberOfBitsInPointer == bitsOfPointerInArrayAllocationProfile);
static_assert(ArrayWithUndecided < ArrayWithContiguous && ArrayWithInt32 < ArrayWithContiguous && ArrayWithDouble < ArrayWithContiguous && ArrayWithArrayStorage > ArrayWithContiguous && CopyOnWrite > ArrayWithContiguous);

namespace Interpreter {

// Each answers nothing in a realm that runs no compiled code, and the keyed ones for a key that the property caches take for no name.
std::optional<JSValue> tryGetById(JSGlobalObject*, JSCell* base, const Identifier&);
std::optional<JSValue> tryGetByVal(JSGlobalObject*, JSCell* base, JSValue key);
std::optional<bool> tryInById(JSGlobalObject*, JSObject* base, const Identifier&);
std::optional<bool> tryInByVal(JSGlobalObject*, JSObject* base, JSValue key);
bool tryPutById(JSGlobalObject*, JSCell* base, const Identifier&, JSValue, bool isStrict);
bool tryPutByVal(JSGlobalObject*, JSCell* base, JSValue key, JSValue, bool isStrict);

void didLookUpCompiledCallee(VM&);
void didLeaveInlineAllocation(VM&);
void didLeaveInlineBranch(VM&);

} // namespace Interpreter

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
