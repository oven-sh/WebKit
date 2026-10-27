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

using SiteWeight = Expected<uint64_t>;
using SiteHeat = Expected<double>;

template<typename T, typename U> constexpr Expected<T>& operator+=(Expected<T>& sum, const U& more) { return sum = sum + more; }
template<typename U> constexpr Expected<bool>& operator|=(Expected<bool>& either, const U& other) { return either = either.predicted() | valueUnderPrediction(other); }
template<typename T> constexpr Expected<T> greaterOf(Expected<T> a, Expected<T> b) { return a.predicted() < b.predicted() ? b : a; }
template<typename T> constexpr Expected<T> lesserOf(Expected<T> a, Expected<T> b) { return b.predicted() < a.predicted() ? b : a; }

inline constexpr unsigned siteWeightFractionBits = 8;
inline constexpr SiteWeight minSiteWeight = 1;
inline constexpr SiteWeight maxSiteWeight = 1ull << 40;
inline constexpr double maxSiteActivations = static_cast<double>(maxSiteWeight.predicted() >> siteWeightFractionBits);

constexpr SiteWeight quantizeHeat(double activations, double frequency)
{
    constexpr double scale = static_cast<double>(1u << siteWeightFractionBits);
    double heat = std::min(activations, maxSiteActivations) * frequency;
    if (!(heat * scale >= static_cast<double>(minSiteWeight.predicted())))
        return minSiteWeight;
    if (heat * scale >= static_cast<double>(maxSiteWeight.predicted()))
        return maxSiteWeight;
    return static_cast<uint64_t>(heat * scale);
}

static_assert(quantizeHeat(std::numeric_limits<double>::infinity(), 1).isSamePredictionAs(maxSiteWeight));
static_assert(quantizeHeat(std::numeric_limits<double>::infinity(), 0.5).isSamePredictionAs(maxSiteWeight / 2));
static_assert((quantizeHeat(std::numeric_limits<double>::infinity(), frequencyConstants.minFrequency) > minSiteWeight).predicted());

inline SiteWeight saturatedSum(SiteWeight a, SiteWeight b)
{
    CheckedUint64 sum = a.predicted();
    sum += b.predicted();
    return sum.hasOverflowed() ? std::numeric_limits<uint64_t>::max() : sum.value();
}

inline SiteWeight saturatedProduct(SiteWeight a, SiteWeight b)
{
    CheckedUint64 product = a.predicted();
    product *= b.predicted();
    return product.hasOverflowed() ? std::numeric_limits<uint64_t>::max() : product.value();
}

class FunctionSiteWeights {
public:
    FunctionSiteWeights(const Program&, const Heat&, FunctionID, FunctionAnalyses&);

    SiteWeight weightOf(BlockID) const;
    SiteHeat heatOf(BlockID) const;
    Expected<bool> isInLoop(BlockID) const;

private:
    double m_activations;
    bool m_isLeastInEveryBlock;
    FunctionAnalyses& m_analyses;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
