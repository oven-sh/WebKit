/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSiteCaches.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTRuntime.h"
#include "VM.h"

namespace JSC { namespace AOT {

size_t SiteCaches::numberOfWords()
{
    Image* image = Image::withCode();
    return image ? image->header().numberOfSiteCacheWords : 0;
}

size_t SiteCaches::endOffset(size_t endOfStates)
{
    if (!numberOfWords())
        return endOfStates;
    RELEASE_ASSERT(endOfStates <= offsetOfTouchedPages);
    return offsetOfWords + roundUpToMultipleOf(WTF::pageSize(), numberOfPages() << pageShift);
}

void SiteCaches::clear(Instance& instance)
{
    char* base = reinterpret_cast<char*>(&instance);
    uint8_t* touched = reinterpret_cast<uint8_t*>(base + offsetOfTouchedPages);
    size_t pages = numberOfPages();
    for (size_t first = 0; first < pages; first += sizeof(uint64_t)) {
        uint64_t group;
        memcpy(&group, touched + first, sizeof(group));
        if (!group)
            continue;
        for (size_t page = first; page < first + sizeof(uint64_t); ++page) {
            if (touched[page])
                memset(base + offsetOfWords + (page << pageShift), 0, pageSize);
        }
        memset(touched + first, 0, sizeof(group));
    }
}

void SiteCaches::clear(VM& vm)
{
    for (Instance* instance : vm.m_aotInstances)
        clear(*instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
