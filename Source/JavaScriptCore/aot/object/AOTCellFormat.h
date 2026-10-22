/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <cstddef>
#include <cstdint>

#if ENABLE(AOT)
#include <wtf/MathExtras.h>

#if OS(DARWIN)
#include <mach/vm_param.h>
#endif
#endif

namespace JSC {

class JSCell;

namespace AOT {

static constexpr ptrdiff_t offsetOfShapeWord = 12;
static constexpr uint32_t compiledObjectShapeBit = 1u << 31;
static constexpr uint32_t shapeBlockMarker = 0xffu << 16;
static constexpr uint16_t blockTestHalfword(uint8_t blockByte) { return static_cast<uint16_t>(shapeBlockMarker >> 8 | blockByte); }
static constexpr uint32_t closureWordMarker = 1u << 30;
static constexpr uint32_t bigIntHashMask = 0x3f7fffff;

#if ENABLE(AOT)

static_assert(OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) > 32 && OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) <= 48);
static constexpr uint32_t pointerUpperHalfBits = (1u << (OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) - 32)) - 1;

static constexpr bool canPassShapeTest(uint32_t wordAtShapeOffset)
{
    return (wordAtShapeOffset & compiledObjectShapeBit) || (wordAtShapeOffset & shapeBlockMarker) == shapeBlockMarker;
}

// A slim closure that is as it was created has the upper half of its closure word where other cells have these words.
static constexpr bool mayBeUpperHalfOfSlimClosureWord(uint32_t wordAtShapeOffset)
{
    return (wordAtShapeOffset & (compiledObjectShapeBit | closureWordMarker)) == closureWordMarker;
}

JS_EXPORT_PRIVATE void validateShapeWord(const JSCell*);

static constexpr uint32_t offsetOfOrderWord = 8;
static constexpr uint32_t offsetOfFirstField = 16;

// At JSCell::cellStateOffset() of a cell that lies in a frame. No barrier takes it for the state of a marked cell.
static constexpr uint8_t frameCellState = UINT8_MAX;

static constexpr uint32_t shapeWordOf(uint32_t shape) { return 0 - shape; }
static constexpr uint32_t shapeOf(uint32_t shapeWord) { return 0 - shapeWord; }

static constexpr unsigned orderWordCountShift = 15;
static constexpr uint32_t orderWordOrderMask = (1u << orderWordCountShift) - 1;
static constexpr uint32_t maxFieldsInOrder = (1 << 12) - 1;
static constexpr uint32_t orderStoredOutOfLine = orderWordOrderMask;
static constexpr uint32_t maxKeyOrdersPerLayout = orderStoredOutOfLine;

static constexpr uint32_t orderWordOf(uint32_t order, uint32_t count) { return order | count << orderWordCountShift; }
static constexpr uint32_t orderOf(uint32_t orderWord) { return orderWord & orderWordOrderMask; }
static constexpr uint32_t countOf(uint32_t orderWord) { return orderWord >> orderWordCountShift & maxFieldsInOrder; }
static constexpr bool isOrderStoredOutOfLine(uint32_t orderWord) { return orderOf(orderWord) == orderStoredOutOfLine; }

// A property that the layout has a field for lies among the extra properties: something was stored that the slot cannot hold, or the property was redefined.
static constexpr uint32_t orderWordHasFieldOutsideSlot = 1u << 27;
static constexpr uint32_t orderWordHasExtraProperties = 1u << 28;
static constexpr uint32_t orderWordIsFrozen = 1u << 29;
static constexpr uint32_t orderWordIsSealed = 1u << 30;
static constexpr uint32_t orderWordIsNotExtensible = 1u << 31;
static constexpr uint32_t orderWordIntegrityBits = orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible;
static constexpr uint32_t orderAndCountMask = orderWordHasFieldOutsideSlot - 1;
static constexpr uint64_t orderCountAndShapeMask = UINT64_MAX << 32 | orderAndCountMask;
static constexpr uint32_t outOfLineOrderWord = orderWordOf(orderStoredOutOfLine, 0);
static constexpr uint32_t unorderedOrderWord = orderWordOf(0, 0);

// An Extension is a word of bits and, in ascending order, the granules whose bits are set. The offset of a slot counts every granule, held or not.
static constexpr uint32_t offsetOfExtensionGranuleBits = 0;
static constexpr uint32_t offsetOfFirstExtensionSlot = sizeof(uint64_t);
static constexpr uint32_t extensionGranuleSize = sizeof(uint64_t);
static constexpr uint32_t maxExtensionGranules = 64;
static constexpr uint32_t maxExtensionSize = offsetOfFirstExtensionSlot + maxExtensionGranules * extensionGranuleSize;
static constexpr uint32_t minExtensionSize = 16;
// A narrow hidden word names an Extension by its distance from a page of zeros, which lies in the part of the cell cage where nothing is allocated.
static constexpr uint32_t offsetOfEmptyExtensionInCage = 8 << 20;

static constexpr uint32_t granuleOf(uint32_t offsetInExtension) { return offsetInExtension / extensionGranuleSize - 1; }
static constexpr uint64_t granuleBitOf(uint32_t offsetInExtension) { return 1ull << granuleOf(offsetInExtension); }
static constexpr uint32_t offsetInGranule(uint32_t offsetInExtension) { return offsetInExtension % extensionGranuleSize; }
// Most Extensions hold one granule for good, so an Extension with one granule is the smallest cell there is and has no room.
static constexpr uint32_t extensionCapacities[] = { 1, 3, 5, 9, 13, 19, 27, 39, maxExtensionGranules };
static constexpr uint32_t extensionCapacityFor(uint32_t numberOfGranules)
{
    for (uint32_t capacity : extensionCapacities) {
        if (numberOfGranules <= capacity)
            return capacity;
    }
    return maxExtensionGranules;
}
// Bit n is set where an Extension that holds n granules is full.
static constexpr uint64_t granuleCountsWithoutRoom = [] {
    uint64_t counts = 0;
    for (uint32_t capacity : extensionCapacities) {
        if (capacity < maxExtensionGranules)
            counts |= 1ull << capacity;
    }
    return counts;
}();
static constexpr uint32_t extensionCellSizeFor(uint32_t numberOfGranules) { return (offsetOfFirstExtensionSlot + extensionCapacityFor(numberOfGranules) * extensionGranuleSize + minExtensionSize - 1) / minExtensionSize * minExtensionSize; }

static constexpr uint32_t offsetOfEnvironmentShapeLowByte = 4;
static constexpr uint32_t offsetOfEnvironmentShapeHighByte = 6;
static constexpr uint32_t offsetOfFirstEnvironmentVariable = 8;
static constexpr unsigned environmentShapesPerPageShift = 16;

static constexpr ptrdiff_t offsetOfClosureEntry = 32;
static constexpr ptrdiff_t offsetOfClosureCaptures = 40;
static constexpr ptrdiff_t offsetOfSlimClosureWord = 8;
static constexpr ptrdiff_t offsetOfSlimClosureCaptures = 16;

static_assert(!granuleOf(offsetOfFirstExtensionSlot) && granuleOf(maxExtensionSize - 1) == maxExtensionGranules - 1);
static_assert(extensionCellSizeFor(1) == minExtensionSize && extensionCellSizeFor(2) == 2 * minExtensionSize && extensionCellSizeFor(3) == 2 * minExtensionSize && extensionCellSizeFor(4) == 3 * minExtensionSize);
static_assert(extensionCellSizeFor(maxExtensionGranules) >= maxExtensionSize);
static_assert((granuleCountsWithoutRoom & 0b1111) == 0b1010);
static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t));
static_assert(offsetOfFirstField == offsetOfShapeWord + sizeof(uint32_t));
static_assert(orderWordOf(maxKeyOrdersPerLayout - 1, maxFieldsInOrder) <= orderAndCountMask);
static_assert(outOfLineOrderWord <= orderAndCountMask);
static_assert(isOrderStoredOutOfLine(outOfLineOrderWord | orderWordIntegrityBits | orderWordHasExtraProperties));
static_assert(!isOrderStoredOutOfLine(orderWordOf(maxKeyOrdersPerLayout - 1, maxFieldsInOrder)));
static_assert(shapeWordOf(1) & compiledObjectShapeBit);
static_assert(!canPassShapeTest(pointerUpperHalfBits));
static_assert(!canPassShapeTest(bigIntHashMask));
static_assert(!mayBeUpperHalfOfSlimClosureWord(pointerUpperHalfBits) && !mayBeUpperHalfOfSlimClosureWord(bigIntHashMask) && !mayBeUpperHalfOfSlimClosureWord(shapeWordOf(1)));

#endif // ENABLE(AOT)

} } // namespace JSC::AOT
