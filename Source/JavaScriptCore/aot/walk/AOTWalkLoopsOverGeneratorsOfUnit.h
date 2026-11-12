/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTCode.h"
#include <array>
#include <optional>
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
    struct OnlyReadSteps {
        BitVector values;
        Vector<uint32_t, 2> positionsOfCalls;
    };
    HashMap<uint32_t, OnlyReadSteps, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> onlyReadSteps;
    HashMap<uint32_t, OnlyReadSteps, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> onlyReadAwaitedSteps;
    static constexpr uint32_t numberOfParts = 4;
    struct CarriedParts {
        std::array<uint32_t, numberOfParts> references;
        bool isRarelyAnswered { false };
    };
    HashMap<uint32_t, CarriedParts, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> partsOfParameters;
    HashMap<uint32_t, Vector<uint32_t, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> stepPositionsOfHeaders;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
