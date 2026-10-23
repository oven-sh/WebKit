/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/OSAllocator.h>

namespace JSC { namespace AOT {

// Zeroed pages for what lives as long as a VM or the process. With ENABLE(AOT_GUARDED_PAGES) they lie between two pages that cannot be touched, on Linux at an address drawn for them alone.
void* reserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void releaseGuardedPages(void* pages, size_t bytes);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
