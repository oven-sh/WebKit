/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CPU.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class Procedure;

} // namespace B3

namespace AOT {

struct MutatorFence {
    B3::BasicBlock* branchBlock;
    B3::BasicBlock* continuation;
};

struct MutatorFenceRecords {
    static constexpr bool isUsed = !isX86_64();

    Vector<MutatorFence, 4> fences;
    // By the index of the B3 value.
    BitVector allocations;
    BitVector initializingStores;
};

JS_EXPORT_PRIVATE bool mergeMutatorFences(B3::Procedure&, const MutatorFenceRecords&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
