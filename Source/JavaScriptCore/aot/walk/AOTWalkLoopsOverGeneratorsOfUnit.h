/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>

namespace JSC { namespace AOT { namespace Typed {

struct LoopsOverGeneratorsOfUnit {
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> states;
    std::optional<bool> engineAnswers;
    bool hasStepsWrittenByHand { false };
    HashMap<uint32_t, BitVector, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> callsOfNextWhoseResultIsOnlyRead;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
