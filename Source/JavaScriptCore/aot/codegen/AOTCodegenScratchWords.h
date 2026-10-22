/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC {

class JSCell;

namespace AOT {

// Under validation: what the compiler believes about the call by convention that follows. NativeCallExpectation encodes the words.
struct ExpectedCallWords {
    // Not a root: the run time compares it and never dereferences it.
    JSCell* callee;
    uint64_t effects;
    uint64_t site;
    uint64_t builtins;
};

struct CodegenScratchWords {
    // Not a root: compiled code compares it and never dereferences it.
    JSCell* pendingBarrierOwner;
    ExpectedCallWords expectedCall;
    // Under validation: set whenever the run time enters compiled code.
    uint64_t didEnterCompiledCode;
    // Not a root: the stub ReturnMisfit leaves the value here, and the landing takes it before anything else runs.
    uint64_t misfit;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
