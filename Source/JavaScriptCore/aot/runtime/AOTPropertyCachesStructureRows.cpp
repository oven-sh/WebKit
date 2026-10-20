/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesStructureRows.h"

#if ENABLE(AOT)

#include "JSCConfig.h"
#include <mutex>
#include <wtf/AlignedStorage.h>
#include <wtf/OSAllocator.h>
#include <wtf/StdLibExtras.h>
#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

StructureRows& StructureRows::singleton()
{
    static AlignedStorage<StructureRows> rows;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        new (NotNull, rows.get()) StructureRows;
    });
    return *rows;
}

StructureRows::StructureRows()
{
    uintptr_t start = g_jscConfig.startOfStructureHeap;
    size_t size = g_jscConfig.sizeOfStructureHeap;
    RELEASE_ASSERT(start && size);
    RELEASE_ASSERT((start & StructureID::structureIDMask) + (size - 1) <= StructureID::structureIDMask);

    size_t bytes = (size >> StructureRowWords::granuleShift) << StructureRowWords::wordShift;
    void* words = OSAllocator::reserveAndCommit(bytes, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(words, bytes, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif

    // SAFETY: The pointer lies outside the allocation. It is only indexed with the bits of a StructureID shifted by granuleShift: the low 32 bits of an address in the structure heap, which does not cross a 4 GB boundary.
    m_rowsForStructureIDZero = static_cast<uint32_t*>(words) - (static_cast<uint32_t>(start) >> StructureRowWords::granuleShift);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
