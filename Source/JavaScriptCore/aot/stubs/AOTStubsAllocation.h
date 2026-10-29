/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include <optional>

namespace JSC { namespace AOT {

class StubAssembler;

// With zeros, every byte of the cell behind the header, or behind the words, is zero.
enum class CellInitialization : uint8_t { Header, HeaderAndWords, HeaderAndZeros, HeaderAndWordsAndZeros };
constexpr bool zeroesBody(CellInitialization initialization) { return initialization == CellInitialization::HeaderAndZeros || initialization == CellInitialization::HeaderAndWordsAndZeros; }
constexpr bool storesWords(CellInitialization initialization) { return initialization == CellInitialization::HeaderAndWords || initialization == CellInitialization::HeaderAndWordsAndZeros; }

constexpr unsigned copyChunkBytes = 16;
constexpr unsigned maxBytesCopiedWithoutLoop = 64;
constexpr unsigned maxExtensionSizeFromStub = 256;

// cellSize is the size of the allocator's cells, where the caller knows it.
constexpr unsigned cellSizeStep = 16;
constexpr unsigned maxCellSizeInKey = 80;
constexpr StubKey allocateCell(CellInitialization initialization, std::optional<size_t> cellSize = std::nullopt)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!cellSize || (*cellSize && *cellSize <= maxCellSizeInKey && !(*cellSize % cellSizeStep)));
    return StubKey(StubFamily::AllocateCell).withForm(static_cast<uint8_t>(initialization)).withCount(cellSize ? *cellSize / cellSizeStep : 0);
}

// The second operand is the prototype that the object is to have. With the one of the allocation's record the cell is as from allocateCell(HeaderAndWordsAndZeros) and the second result is not zero.
// With any other the object is whole, of whatever layout the prototype selects, and the second result is zero.
constexpr StubKey allocateInstance(std::optional<size_t> cellSize = std::nullopt)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!cellSize || (*cellSize && *cellSize <= maxCellSizeInKey && !(*cellSize % cellSizeStep)));
    return StubKey(StubFamily::AllocateInstance).withCount(cellSize ? *cellSize / cellSizeStep : 0);
}

// How many instructions the zeros take: one store a chunk where the size is known, a loop of two chunks where it is not.
constexpr unsigned instructionsToZeroBody(CellInitialization initialization, size_t bytes, bool knowsSize)
{
    unsigned chunks = static_cast<unsigned>((bytes + copyChunkBytes - 1) / copyChunkBytes);
    return (knowsSize ? chunks : 2 * chunks + 3) + !storesWords(initialization);
}

// Without fieldBytes, the size is an operand.
constexpr StubKey copyObject(std::optional<unsigned> fieldBytes)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!fieldBytes || (*fieldBytes && !(*fieldBytes % copyChunkBytes) && *fieldBytes <= maxBytesCopiedWithoutLoop));
    return StubKey(StubFamily::CopyObject).withCount(fieldBytes ? *fieldBytes / copyChunkBytes : 0);
}

// The second operand is the byte offset of the hidden Extension word in the owner. Its upper 32 bits are zero. The third is the bits of the granules, which fit into the size.
constexpr StubKey allocateExtension(unsigned size)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(size && !(size % copyChunkBytes) && size <= maxExtensionSizeFromStub);
    return StubKey(StubFamily::AllocateExtension).withCount(size / copyChunkBytes);
}

constexpr StubKey newTypedArray() { return StubKey(StubFamily::NewTypedArray); }
// The cell has room for twice so many properties beyond the default.
constexpr StubKey newDictionary(unsigned addedPairsOfSlots) { return StubKey(StubFamily::NewDictionary).withCount(addedPairsOfSlots); }

// The upper 32 bits of allocation are zero. scratch may be allocation, which is then lost.
void loadAllocationSiteRecord(StubAssembler&, GPRReg allocation, GPRReg record, GPRReg scratch);

static_assert(exists(allocateCell(CellInitialization::HeaderAndWordsAndZeros)) && !exists(allocateCell(CellInitialization::HeaderAndWords).withForm(4)));
static_assert(exists(allocateCell(CellInitialization::Header, maxCellSizeInKey)) && !exists(allocateCell(CellInitialization::Header, maxCellSizeInKey).withCount(maxCellSizeInKey / cellSizeStep + 1)));
static_assert(exists(allocateInstance()) && exists(allocateInstance(maxCellSizeInKey)) && !exists(allocateInstance(maxCellSizeInKey).withCount(maxCellSizeInKey / cellSizeStep + 1)));
static_assert(exists(copyObject(maxBytesCopiedWithoutLoop)) && exists(copyObject(std::nullopt)));
static_assert(!exists(copyObject(maxBytesCopiedWithoutLoop).withCount(maxBytesCopiedWithoutLoop / copyChunkBytes + 1)));
static_assert(exists(allocateExtension(copyChunkBytes)) && exists(allocateExtension(maxExtensionSizeFromStub)));
static_assert(!exists(allocateExtension(maxExtensionSizeFromStub).withCount(maxExtensionSizeFromStub / copyChunkBytes + 1)) && !exists(allocateExtension(copyChunkBytes).withCount(0)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
