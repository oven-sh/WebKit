/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSExportMacros.h"

namespace JSC {

// Asked about every millisecond until cells start to move. Once it says yes, HeapCompaction::collectAndCompact() returns: nothing
// has moved, and the collection may still be under way. It must not touch the heap.
JS_EXPORT_PRIVATE void setHeapCompactionInterruptCheck(bool (*isThreadWanted)());

} // namespace JSC
