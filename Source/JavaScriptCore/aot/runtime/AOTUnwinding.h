/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"

namespace JSC {

class VM;
struct VMEntryRecord;

namespace AOT {

enum class CompiledHandlerKind : uint8_t {
    Catch,
    Finally,
    RejectPromise,
};

struct CompiledHandler {
    CallFrame* callFrame { nullptr };
    void* code { nullptr };
    CompiledHandlerKind kind { CompiledHandlerKind::Catch };
};

enum class CompiledUnwindStatus : uint8_t {
    FoundHandler,
    LeftCompiledCode,
    ReachedEntryFrame,
};

struct CompiledUnwindResult {
    CompiledUnwindStatus status;
    CompiledHandler handler;
};

bool needsExceptionForEveryThrow(const Instance&);
std::optional<CompiledFrameIterator> findCompiledCallers(VM&, const Instance&, CallFrame* stubFrame);
CompiledUnwindResult unwindCompiledFrames(VM&, CompiledFrameIterator&, VMEntryRecord&, bool isTermination);
bool isCaughtByCompiledCode(VM&, CompiledFrameIterator);
bool nearestHandlerIgnoresValue(VM&, const Instance&, CallFrame* stubFrame);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
