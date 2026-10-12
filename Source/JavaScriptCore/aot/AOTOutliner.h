/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubs.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct CompiledCode;

struct OutliningOptions {
    static constexpr uint8_t isNotHotByText = 1;
    static constexpr uint8_t isColdByGraph = 2;
    static constexpr uint8_t isFromTypedLowering = 4;

    uint8_t region { isFromTypedLowering };
    bool takesFunctionsWhole { true };
    bool takesRareCode { false };
    unsigned leastLength { 2 };
    bool mayEndInCall { false };
    bool onlyCounts { false };

    static const std::optional<OutliningOptions>& asked();
    bool takesWhole(uint8_t regionsOfFunction) const { return takesFunctionsWhole && (regionsOfFunction & region) == region; }
    bool takes(uint8_t regionsOfFunction) const { return takesWhole(regionsOfFunction) || (takesRareCode && (regionsOfFunction & isFromTypedLowering)); }
    bool takesEverything() const { return takesFunctionsWhole && region == isFromTypedLowering; }
};

Outlining chooseWindows(const Vector<const CompiledCode*>&, Vector<std::pair<uint32_t, uint32_t>>&& entriesInsideFunctions, const OutliningOptions&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
