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
static constexpr uint32_t bigIntHashMask = 0x3f3fffff;

#if ENABLE(AOT)

static_assert(OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) > 32 && OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) <= 48);
static constexpr uint32_t pointerUpperHalfBits = (1u << (OS_CONSTANT(EFFECTIVE_ADDRESS_WIDTH) - 32)) - 1;

static constexpr uint32_t shapeBlockMarkerBesideMark = shapeBlockMarker & ~(1u << 23);
static constexpr bool canPassShapeTest(uint32_t wordAtShapeOffset)
{
    return (wordAtShapeOffset & compiledObjectShapeBit) || (wordAtShapeOffset & shapeBlockMarkerBesideMark) == shapeBlockMarkerBesideMark;
}
static_assert(canPassShapeTest(shapeBlockMarkerBesideMark) && !canPassShapeTest(bigIntHashMask) && !canPassShapeTest(bigIntHashMask | 1u << 23));

static constexpr bool mayBeUpperHalfOfSlimClosureWord(uint32_t wordAtShapeOffset)
{
    return (wordAtShapeOffset & (compiledObjectShapeBit | closureWordMarker)) == closureWordMarker;
}

JS_EXPORT_PRIVATE void validateShapeWord(const JSCell*);

static constexpr uint32_t offsetOfOrderWord = 8;
static constexpr uint32_t offsetOfFirstField = 16;

static constexpr uint8_t frameCellState = UINT8_MAX;

static constexpr uint32_t shapeWordOf(uint32_t shape) { return 0 - shape; }
static constexpr uint32_t shapeOf(uint32_t shapeWord) { return 0 - shapeWord; }

static constexpr uint32_t shapeWordIsUnmarked = 1u << 23;
static constexpr uint64_t orderAndShapeWordsAreUnmarked = static_cast<uint64_t>(shapeWordIsUnmarked) << 32;
static constexpr ptrdiff_t offsetOfByteWithMark = offsetOfShapeWord + 2;
static constexpr uint8_t byteWithMarkIsUnmarked = 1u << 7;
static constexpr uint32_t maxNumberOfShapes = shapeWordIsUnmarked;
static constexpr uint32_t shapeWordWithoutMark(uint32_t shapeWordWithMark) { return shapeWordWithMark | shapeWordIsUnmarked; }
static_assert(shapeWordWithoutMark(shapeWordOf(1)) == shapeWordOf(1) && shapeWordWithoutMark(shapeWordOf(maxNumberOfShapes)) == shapeWordOf(maxNumberOfShapes) && shapeWordWithoutMark(shapeWordOf(maxNumberOfShapes + 1)) != shapeWordOf(maxNumberOfShapes + 1));
static_assert(shapeWordWithoutMark(shapeBlockMarker) == shapeBlockMarker && shapeWordIsUnmarked == static_cast<uint32_t>(byteWithMarkIsUnmarked) << 8 * (offsetOfByteWithMark - offsetOfShapeWord));
static_assert(shapeBlockMarkerBesideMark == (shapeBlockMarker & ~shapeWordIsUnmarked));

enum class KeyListKind : uint8_t { Strings, StringsAndSymbols, Symbols };
static constexpr uint32_t keyListWordStep = (1u << 29) + 341;
static constexpr uint32_t keyListWordOf(uint32_t shapeWord, KeyListKind kind) { return shapeWord - static_cast<uint32_t>(kind) * keyListWordStep; }
static constexpr bool hasKeyListsInTable(KeyListKind kind, uint32_t numberOfShapes) { return kind == KeyListKind::Strings || numberOfShapes < keyListWordStep; }

static constexpr unsigned orderWordCountShift = 15;
static constexpr uint32_t orderWordOrderMask = (1u << orderWordCountShift) - 1;
static constexpr uint32_t maxFieldsInOrder = (1 << 12) - 1;
static constexpr uint32_t orderStoredOutOfLine = orderWordOrderMask;
static constexpr uint32_t maxKeyOrdersPerLayout = orderStoredOutOfLine;

static constexpr uint32_t orderWordOf(uint32_t order, uint32_t count) { return order | count << orderWordCountShift; }
static constexpr uint32_t orderOf(uint32_t orderWord) { return orderWord & orderWordOrderMask; }
static constexpr uint32_t countOf(uint32_t orderWord) { return orderWord >> orderWordCountShift & maxFieldsInOrder; }
static constexpr bool isOrderStoredOutOfLine(uint32_t orderWord) { return orderOf(orderWord) == orderStoredOutOfLine; }

static constexpr uint32_t orderWordHasFieldOutsideSlot = 1u << 27;
static constexpr uint32_t orderWordHasExtraProperties = 1u << 28;
static constexpr uint32_t orderWordIsFrozen = 1u << 29;
static constexpr uint32_t orderWordIsSealed = 1u << 30;
static constexpr uint32_t orderWordIsNotExtensible = 1u << 31;
static constexpr uint32_t orderWordIntegrityBits = orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible;
static constexpr uint32_t orderAndCountMask = orderWordHasFieldOutsideSlot - 1;
static constexpr uint64_t orderCountAndShapeMask = UINT64_MAX << 32 | orderAndCountMask;
static constexpr uint32_t outOfLineOrderWord = orderWordOf(orderStoredOutOfLine, 0);
static constexpr uint32_t outOfLineOrderWordFor(size_t numberOfEntries) { return orderWordOf(orderStoredOutOfLine, numberOfEntries <= maxFieldsInOrder ? static_cast<uint32_t>(numberOfEntries) : 0); }
static constexpr uint32_t orderWordCountBits = maxFieldsInOrder << orderWordCountShift;
static constexpr uint32_t unorderedOrderWord = orderWordOf(0, 0);

static constexpr uint32_t offsetOfExtensionGranuleBits = 0;
static constexpr uint32_t offsetOfFirstExtensionSlot = sizeof(uint64_t);
static constexpr uint32_t extensionGranuleSize = sizeof(uint64_t);
static constexpr uint32_t maxExtensionGranules = 64;
static constexpr uint32_t maxExtensionSize = offsetOfFirstExtensionSlot + maxExtensionGranules * extensionGranuleSize;
static constexpr uint32_t minExtensionSize = 16;
static constexpr uint32_t offsetOfEmptyExtensionInCage = 8 << 20;

static constexpr uint32_t granuleOf(uint32_t offsetInExtension) { return offsetInExtension / extensionGranuleSize - 1; }
static constexpr uint64_t granuleBitOf(uint32_t offsetInExtension) { return 1ull << granuleOf(offsetInExtension); }
static constexpr uint32_t offsetInGranule(uint32_t offsetInExtension) { return offsetInExtension % extensionGranuleSize; }
static constexpr uint32_t extensionCapacities[] = { 1, 3, 5, 9, 13, 19, 27, 39, maxExtensionGranules };
static constexpr uint32_t extensionCapacityFor(uint32_t numberOfGranules)
{
    for (uint32_t capacity : extensionCapacities) {
        if (numberOfGranules <= capacity)
            return capacity;
    }
    return maxExtensionGranules;
}
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
static constexpr unsigned environmentShapesPerPageShift = 15;
static constexpr uint8_t environmentIsEscapedBit = 1u << 7;
static constexpr uint8_t lowByteOfEnvironmentShape(uint32_t shape) { return static_cast<uint8_t>(shape); }
static constexpr uint8_t highByteOfEnvironmentShape(uint32_t shape) { return shape >> 8 & (environmentIsEscapedBit - 1); }
static constexpr uint32_t environmentShapeInPage(uint8_t lowByte, uint8_t highByte) { return lowByte | (highByte & (environmentIsEscapedBit - 1)) << 8; }
static constexpr uint32_t tagOfLinkBetweenEnvironmentsHandedBack = 2;

static constexpr ptrdiff_t offsetOfClosureEntry = 32;
static constexpr ptrdiff_t offsetOfClosureCaptures = 40;
static constexpr ptrdiff_t offsetOfSlimClosureWord = 8;
static constexpr ptrdiff_t offsetOfSlimClosureCaptures = 16;
static constexpr ptrdiff_t offsetOfCopyInHeapOfClosureInFrame(ptrdiff_t offsetOfCaptures, unsigned numberOfCaptures) { return offsetOfCaptures + static_cast<ptrdiff_t>(numberOfCaptures * sizeof(uint64_t)); }
static constexpr size_t sizeOfClosureInFrame(ptrdiff_t offsetOfCaptures, unsigned numberOfCaptures) { return offsetOfCopyInHeapOfClosureInFrame(offsetOfCaptures, numberOfCaptures) + sizeof(JSCell*); }

static_assert(offsetOfCopyInHeapOfClosureInFrame(offsetOfSlimClosureCaptures, 0) == offsetOfSlimClosureCaptures && sizeOfClosureInFrame(offsetOfClosureCaptures, 2) == offsetOfClosureCaptures + 3 * sizeof(uint64_t));
static_assert(!granuleOf(offsetOfFirstExtensionSlot) && granuleOf(maxExtensionSize - 1) == maxExtensionGranules - 1);
static_assert(extensionCellSizeFor(1) == minExtensionSize && extensionCellSizeFor(2) == 2 * minExtensionSize && extensionCellSizeFor(3) == 2 * minExtensionSize && extensionCellSizeFor(4) == 3 * minExtensionSize);
static_assert(extensionCellSizeFor(maxExtensionGranules) >= maxExtensionSize);
static_assert((granuleCountsWithoutRoom & 0b1111) == 0b1010);
static_assert(offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t));
static_assert(offsetOfFirstField == offsetOfShapeWord + sizeof(uint32_t));
static_assert(orderWordOf(maxKeyOrdersPerLayout - 1, maxFieldsInOrder) <= orderAndCountMask);
static_assert(outOfLineOrderWord <= orderAndCountMask);
static_assert(outOfLineOrderWordFor(maxFieldsInOrder) <= orderAndCountMask && countOf(outOfLineOrderWordFor(maxFieldsInOrder)) == maxFieldsInOrder);
static_assert(outOfLineOrderWordFor(maxFieldsInOrder + 1) == outOfLineOrderWord && (outOfLineOrderWordFor(1) & ~orderWordCountBits) == outOfLineOrderWord);
static_assert(isOrderStoredOutOfLine(outOfLineOrderWord | orderWordIntegrityBits | orderWordHasExtraProperties));
static_assert(!isOrderStoredOutOfLine(orderWordOf(maxKeyOrdersPerLayout - 1, maxFieldsInOrder)));
static_assert(shapeWordOf(1) & compiledObjectShapeBit);
static_assert(!canPassShapeTest(pointerUpperHalfBits));
static_assert(!canPassShapeTest(bigIntHashMask));
static_assert(!mayBeUpperHalfOfSlimClosureWord(pointerUpperHalfBits) && !mayBeUpperHalfOfSlimClosureWord(bigIntHashMask) && !mayBeUpperHalfOfSlimClosureWord(shapeWordOf(1)));
static_assert(environmentShapeInPage(UINT8_MAX, UINT8_MAX) + 1 == 1u << environmentShapesPerPageShift);
static_assert(environmentShapeInPage(lowByteOfEnvironmentShape(0x2abcd), highByteOfEnvironmentShape(0x2abcd)) == 0x2bcd && environmentShapeInPage(lowByteOfEnvironmentShape(0x2abcd), highByteOfEnvironmentShape(0x2abcd) | environmentIsEscapedBit) == 0x2bcd);
static_assert((0x2abcdu >> environmentShapesPerPageShift << environmentShapesPerPageShift) + 0x2bcd == 0x2abcd);

#endif // ENABLE(AOT)

} } // namespace JSC::AOT
