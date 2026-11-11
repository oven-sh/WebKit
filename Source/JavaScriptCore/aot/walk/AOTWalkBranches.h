/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

bool isUnseenWhenUnused(const Walker&, const BodyValue&);

inline constexpr uint32_t mostBlocksPassedBehindTerminator = 3;

struct WayBehindTerminator {
    BlockID from;
    uint32_t targetIndex { 0 };
    BodyTarget target;
    std::array<BlockID, mostBlocksPassedBehindTerminator> passed;
    uint32_t numberOfPassedBlocks { 0 };
    BodyTarget arrival;
};
WayBehindTerminator wayBehindTarget(const Walker&, BlockID from, uint32_t targetIndex, BodyTarget);

bool comeToSamePlace(const BodyIndex&, const WayBehindTerminator&, const WayBehindTerminator&, ValueID falseOnFirstWay = { }, ValueID falseOnSecondWay = { });

bool readsNothingOfWhatItCatches(const BodyIndex&, BodyBlock handler);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
