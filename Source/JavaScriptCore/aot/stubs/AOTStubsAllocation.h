/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubRegisters.h"
#include <optional>

namespace JSC { namespace AOT {

class StubAssembler;

// With zeros, every byte of the cell behind the header, or behind the words, is zero.
enum class CellInitialization : uint8_t { Header, HeaderAndWords, HeaderAndZeros, HeaderAndWordsAndZeros };
constexpr bool zeroesBody(CellInitialization initialization) { return initialization == CellInitialization::HeaderAndZeros || initialization == CellInitialization::HeaderAndWordsAndZeros; }
constexpr bool storesWords(CellInitialization initialization) { return initialization == CellInitialization::HeaderAndWords || initialization == CellInitialization::HeaderAndWordsAndZeros; }

constexpr unsigned copyChunkBytes = 16;
constexpr unsigned maxExtensionSizeFromStub = 256;

// cellSize is the size of the allocator's cells, where the caller knows it.
constexpr unsigned cellSizeStep = 16;
constexpr unsigned maxCellSizeInKey = 80;
constexpr StubKey allocateCell(CellInitialization initialization, std::optional<size_t> cellSize = std::nullopt)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!cellSize || (*cellSize && *cellSize <= maxCellSizeInKey && !(*cellSize % cellSizeStep)));
    return StubKey(StubFamily::AllocateCell).withForm(static_cast<uint8_t>(initialization)).withCount(cellSize ? *cellSize / cellSizeStep : 0);
}

// The second operand points to the template: what the cell is to hold from offsetOfTemplateInCell() to the end of its last chunk, which the stub copies. `size` is what is asked of the allocator.
constexpr unsigned maxCellSizeFromTemplate = 256;
constexpr unsigned offsetOfTemplateInCell(CellInitialization initialization) { return storesWords(initialization) ? copyChunkBytes : copyChunkBytes / 2; }
constexpr size_t sizeOfCellTemplate(CellInitialization initialization, size_t size) { return (size + copyChunkBytes - 1) / copyChunkBytes * copyChunkBytes - offsetOfTemplateInCell(initialization); }
constexpr StubKey allocateCellFromTemplate(CellInitialization initialization, size_t size)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!zeroesBody(initialization) && size > copyChunkBytes && size <= maxCellSizeFromTemplate);
    return StubKey(StubFamily::AllocateCellFromTemplate).withForm(static_cast<uint8_t>(initialization)).withCount((size + copyChunkBytes - 1) / copyChunkBytes);
}

// The second operand is the place in the caller's frame, which is also the result. It gets what allocateCell(HeaderAndWords) stores, with the state of a cell in a frame.
constexpr StubKey initializeObjectInFrame() { return StubKey(StubFamily::InitializeObjectInFrame); }

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

// The caller of allocateCellFromTemplate() names the template by its address in the image.
constexpr unsigned instructionsToNameTemplate = isX86_64() ? 1 : 2;
// How many instructions the copy takes: the name, then a load and a store for each chunk behind the first, and for the word beside a header without words.
constexpr unsigned instructionsToCopyBody(CellInitialization initialization, size_t bytes)
{
    unsigned chunks = static_cast<unsigned>((bytes + copyChunkBytes - 1) / copyChunkBytes);
    return instructionsToNameTemplate + 2 * (chunks - storesWords(initialization));
}

// The second operand is the byte offset of the hidden Extension word in the owner. Its upper 32 bits are zero. The third is the bits of the granules, which fit into the size.
constexpr StubKey allocateExtension(unsigned size)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(size && !(size % copyChunkBytes) && size <= maxExtensionSizeFromStub);
    return StubKey(StubFamily::AllocateExtension).withCount(size / copyChunkBytes);
}

// Behind AllocateExtensionSlow, the Extension of its owner.
inline constexpr GPRReg extensionMadeBySlowStub = gprOf(StubRegister::R0);

// The first operand is the TypedArrayType, the second the length as the program wrote it: any number.
constexpr StubKey newTypedArray() { return StubKey(StubFamily::NewTypedArray); }
// The cell has room for twice so many properties beyond the default.
constexpr StubKey newDictionary(unsigned addedPairsOfSlots) { return StubKey(StubFamily::NewDictionary).withCount(addedPairsOfSlots); }

// The upper 32 bits of allocation are zero. scratch may be allocation, which is then lost.
void loadAllocationSiteRecord(StubAssembler&, GPRReg allocation, GPRReg record, GPRReg scratch);

static_assert(exists(allocateCell(CellInitialization::HeaderAndWordsAndZeros)) && !exists(allocateCell(CellInitialization::HeaderAndWords).withForm(4)));
static_assert(exists(allocateCell(CellInitialization::Header, maxCellSizeInKey)) && !exists(allocateCell(CellInitialization::Header, maxCellSizeInKey).withCount(maxCellSizeInKey / cellSizeStep + 1)));
static_assert(exists(allocateCellFromTemplate(CellInitialization::Header, copyChunkBytes + 1)) && exists(allocateCellFromTemplate(CellInitialization::HeaderAndWords, maxCellSizeFromTemplate)));
static_assert(!exists(allocateCellFromTemplate(CellInitialization::Header, maxCellSizeFromTemplate).withCount(1)) && !exists(allocateCellFromTemplate(CellInitialization::Header, maxCellSizeFromTemplate).withCount(maxCellSizeFromTemplate / copyChunkBytes + 1)));
static_assert(!exists(allocateCellFromTemplate(CellInitialization::HeaderAndWords, maxCellSizeFromTemplate).withForm(static_cast<uint8_t>(CellInitialization::HeaderAndZeros))));
static_assert(exists(allocateInstance()) && exists(allocateInstance(maxCellSizeInKey)) && !exists(allocateInstance(maxCellSizeInKey).withCount(maxCellSizeInKey / cellSizeStep + 1)));
static_assert(exists(allocateExtension(copyChunkBytes)) && exists(allocateExtension(maxExtensionSizeFromStub)));
static_assert(!exists(allocateExtension(maxExtensionSizeFromStub).withCount(maxExtensionSizeFromStub / copyChunkBytes + 1)) && !exists(allocateExtension(copyChunkBytes).withCount(0)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
