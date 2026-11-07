/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>

namespace JSC { namespace AOT { namespace Typed {

struct LoopsOverGeneratorsOfUnit {
    struct StateOpened {
        uint32_t listOfWords { 0 };
        std::optional<PassedTest> generatorWasAsMade;
    };
    HashMap<uint32_t, StateOpened, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> statesOpened;
    std::optional<bool> engineAnswers;
    bool hasStepsWrittenByHand { false };
    HashMap<uint32_t, BitVector, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> callsOfNextWhoseResultIsOnlyRead;
    HashMap<uint32_t, BitVector, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> awaitedCallsOfNextWhoseResultIsOnlyRead;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
