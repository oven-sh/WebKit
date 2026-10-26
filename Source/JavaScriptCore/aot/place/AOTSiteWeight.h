/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTIR.h"
#include <algorithm>
#include <limits>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class Heat;

using SiteWeight = uint64_t;

inline constexpr unsigned siteWeightFractionBits = 8;
inline constexpr SiteWeight minSiteWeight = 1;
inline constexpr SiteWeight maxSiteWeight = 1ull << 40;
inline constexpr double maxSiteActivations = static_cast<double>(maxSiteWeight >> siteWeightFractionBits);

constexpr SiteWeight quantizeHeat(double activations, double frequency)
{
    constexpr double scale = static_cast<double>(1u << siteWeightFractionBits);
    double heat = std::min(activations, maxSiteActivations) * frequency;
    if (!(heat * scale >= static_cast<double>(minSiteWeight)))
        return minSiteWeight;
    if (heat * scale >= static_cast<double>(maxSiteWeight))
        return maxSiteWeight;
    return static_cast<SiteWeight>(heat * scale);
}

static_assert(quantizeHeat(std::numeric_limits<double>::infinity(), 1) == maxSiteWeight);
static_assert(quantizeHeat(std::numeric_limits<double>::infinity(), 0.5) == maxSiteWeight / 2);
static_assert(quantizeHeat(std::numeric_limits<double>::infinity(), frequencyConstants.minFrequency) > minSiteWeight);

inline SiteWeight saturatedSum(SiteWeight a, SiteWeight b)
{
    CheckedUint64 sum = a;
    sum += b;
    return sum.hasOverflowed() ? std::numeric_limits<SiteWeight>::max() : sum.value();
}

inline SiteWeight saturatedProduct(SiteWeight a, SiteWeight b)
{
    CheckedUint64 product = a;
    product *= b;
    return product.hasOverflowed() ? std::numeric_limits<SiteWeight>::max() : product.value();
}

class FunctionSiteWeights {
public:
    FunctionSiteWeights(const Program&, const Heat&, FunctionID, FunctionAnalyses&);

    SiteWeight weightOf(BlockID) const;
    double heatOf(BlockID) const;
    bool isInLoop(BlockID) const;

private:
    double m_activations;
    bool m_isLeastInEveryBlock;
    FunctionAnalyses& m_analyses;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
