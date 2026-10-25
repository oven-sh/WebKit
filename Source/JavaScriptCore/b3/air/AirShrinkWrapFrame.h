/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirFrameLayout.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 { namespace Air {

class BasicBlock;
class Code;
struct StackColoring;

struct FrameSetupSite {
    BasicBlock* block { nullptr };
    unsigned instIndex { 0 };
};

struct FramePlacement {
    FrameCoverage coverage { FrameCoverage::NoPath };
    Vector<FrameSetupSite, 2> setups;
    unsigned numberOfTeardowns { 0 };
    bool addressesIncomingArgumentsInFrame { false };
    bool didAddBlocks { false };
};

JS_EXPORT_PRIVATE FramePlacement shrinkWrapFrame(Code&, const StackColoring&);

// Before register allocation: the blocks that shrinkWrapFrame will cover whatever the allocator does.
// Nothing where they leave out no path.
std::optional<BitVector> findFrameRegionBeforeAllocation(Code&);

// The same where shrinkWrapFrame also covers `crowdedBlocks`. `region` is what findFrameRegionBeforeAllocation() found in this code.
std::optional<BitVector> growFrameRegionBeforeAllocation(Code&, const BitVector& region, const BitVector& crowdedBlocks);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
