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

enum class CellInitialization : uint8_t { Header, HeaderAndWords, HeaderAndZeros, HeaderAndWordsAndZeros };
constexpr bool zeroesBody(CellInitialization initialization) { return initialization == CellInitialization::HeaderAndZeros || initialization == CellInitialization::HeaderAndWordsAndZeros; }
constexpr bool storesWords(CellInitialization initialization) { return initialization == CellInitialization::HeaderAndWords || initialization == CellInitialization::HeaderAndWordsAndZeros; }

constexpr unsigned copyChunkBytes = 16;
constexpr unsigned maxExtensionSizeFromStub = 256;

constexpr unsigned cellSizeStep = 16;
constexpr unsigned maxCellSizeInKey = 80;
constexpr StubKey allocateCell(CellInitialization initialization, std::optional<size_t> cellSize = std::nullopt)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!cellSize || (*cellSize && *cellSize <= maxCellSizeInKey && !(*cellSize % cellSizeStep)));
    return StubKey(StubFamily::AllocateCell).withForm(static_cast<uint8_t>(initialization)).withCount(cellSize ? *cellSize / cellSizeStep : 0);
}

constexpr unsigned maxCellSizeFromTemplate = 256;
constexpr unsigned offsetOfTemplateInCell(CellInitialization initialization) { return storesWords(initialization) ? copyChunkBytes : copyChunkBytes / 2; }
constexpr size_t sizeOfCellTemplate(CellInitialization initialization, size_t size) { return (size + copyChunkBytes - 1) / copyChunkBytes * copyChunkBytes - offsetOfTemplateInCell(initialization); }
constexpr StubKey allocateCellFromTemplate(CellInitialization initialization, size_t size)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!zeroesBody(initialization) && size > copyChunkBytes && size <= maxCellSizeFromTemplate);
    return StubKey(StubFamily::AllocateCellFromTemplate).withForm(static_cast<uint8_t>(initialization)).withCount((size + copyChunkBytes - 1) / copyChunkBytes);
}

constexpr StubKey initializeObjectInFrame() { return StubKey(StubFamily::InitializeObjectInFrame); }

constexpr StubKey allocateInstance(std::optional<size_t> cellSize = std::nullopt)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(!cellSize || (*cellSize && *cellSize <= maxCellSizeInKey && !(*cellSize % cellSizeStep)));
    return StubKey(StubFamily::AllocateInstance).withCount(cellSize ? *cellSize / cellSizeStep : 0);
}

constexpr unsigned instructionsToZeroBody(CellInitialization initialization, size_t bytes, bool knowsSize)
{
    unsigned chunks = static_cast<unsigned>((bytes + copyChunkBytes - 1) / copyChunkBytes);
    return (knowsSize ? chunks : 2 * chunks + 3) + !storesWords(initialization);
}

constexpr unsigned instructionsToNameTemplate = isX86_64() ? 1 : 2;
constexpr unsigned instructionsToCopyBody(CellInitialization initialization, size_t bytes)
{
    unsigned chunks = static_cast<unsigned>((bytes + copyChunkBytes - 1) / copyChunkBytes);
    return instructionsToNameTemplate + 2 * (chunks - storesWords(initialization));
}

constexpr StubKey allocateExtension(unsigned size)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(size && !(size % copyChunkBytes) && size <= maxExtensionSizeFromStub);
    return StubKey(StubFamily::AllocateExtension).withCount(size / copyChunkBytes);
}

inline constexpr GPRReg extensionMadeBySlowStub = gprOf(StubRegister::R0);

constexpr StubKey newTypedArray() { return StubKey(StubFamily::NewTypedArray); }
constexpr StubKey newDictionary() { return StubKey(StubFamily::NewDictionary); }

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
