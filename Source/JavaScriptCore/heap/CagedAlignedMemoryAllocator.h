/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AlignedMemoryAllocator.h"
#include "MarkedBlockCage.h"

namespace JSC {

class CagedAlignedMemoryAllocator final : public AlignedMemoryAllocator {
public:
    CagedAlignedMemoryAllocator();
    ~CagedAlignedMemoryAllocator() final;

    void* tryAllocateAlignedMemory(size_t alignment, size_t) final;
    void freeAlignedMemory(void*) final;

    void dump(PrintStream&) const final;

    void* tryAllocateMemory(size_t) final;
    void freeMemory(void*) final;
    void* tryReallocateMemory(void*, size_t) final;

    MarkedBlockCage& cage() LIFETIME_BOUND { return m_cage; }

private:
    MarkedBlockCage m_cage;
};

} // namespace JSC
