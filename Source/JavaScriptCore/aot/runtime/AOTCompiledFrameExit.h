/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC {

class CallFrame;
struct EntryFrame;

namespace AOT {

struct CompiledFrameExit {
    CallFrame* callFrame { nullptr };
    void* returnPC { nullptr };
    EntryFrame* entryFrame { nullptr };
    bool crossedEntryFrame { false };
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
