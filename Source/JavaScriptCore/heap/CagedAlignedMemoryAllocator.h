/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AlignedMemoryAllocator.h"
#include "MarkedBlockCage.h"
#include <memory>

namespace JSC {

class CagedAlignedMemoryAllocator final : public AlignedMemoryAllocator {
public:
    CagedAlignedMemoryAllocator();
    ~CagedAlignedMemoryAllocator() final;

    void* tryAllocateAlignedMemory(size_t alignment, size_t) final;
    void freeAlignedMemory(void*) final;
    void* tryAllocateBlockToFill() final;

    void dump(PrintStream&) const final;

    void* tryAllocateMemory(size_t) final;
    void freeMemory(void*) final;
    void* tryReallocateMemory(void*, size_t) final;

    MarkedBlockCage& cage() LIFETIME_BOUND { return m_cage; }

private:
    class ResidentBlocks;

    MarkedBlockCage m_cage;
    std::unique_ptr<ResidentBlocks> m_residentBlocks;
    unsigned m_numberOfBlocksToFillRequested { 0 };
};

} // namespace JSC
