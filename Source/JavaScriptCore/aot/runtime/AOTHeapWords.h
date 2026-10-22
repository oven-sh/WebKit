/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC {

class MarkStackArray;
class StoredCellLog;

namespace AOT {

struct HeapWords {
    StoredCellLog* storedCellLog;
    MarkStackArray* mutatorMarkStack;
    uintptr_t* barriersExecuted;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
