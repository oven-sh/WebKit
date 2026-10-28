/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSiteWeight.h"

#if ENABLE(AOT)

#include "AOTCreateEntrypoints.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"

namespace JSC { namespace AOT { namespace Typed {

static bool countsForNothing(const Function& function)
{
    return function.role == FunctionRole::GenericEntrypoint && function.derivation.parameter == allocatingConstructEntrypointParameter;
}

FunctionSiteWeights::FunctionSiteWeights(const Program& program, const Heat& heat, FunctionID function, FunctionAnalyses& analyses)
    : FunctionSiteWeights(countsForNothing(program.at(function)) ? 0 : heat.estimatedActivationsOf(program, function).predicted(), analyses)
{
}

FunctionSiteWeights::FunctionSiteWeights(const Program& program, FunctionID function, FunctionAnalyses& analyses)
    : FunctionSiteWeights(countsForNothing(program.at(function)) ? 0 : 1, analyses)
{
}

FunctionSiteWeights::FunctionSiteWeights(double activations, FunctionAnalyses& analyses)
    : m_activations(activations)
    , m_isLeastInEveryBlock(quantizeHeat(m_activations, frequencyConstants.maxFrequency).isSamePredictionAs(minSiteWeight))
    , m_analyses(analyses)
{
}

SiteWeight FunctionSiteWeights::weightOf(BlockID block) const
{
    if (m_isLeastInEveryBlock)
        return minSiteWeight;
    return quantizeHeat(m_activations, m_analyses.blockFrequencies().frequencyOf(block).predicted());
}

SiteHeat FunctionSiteWeights::heatOf(BlockID block) const
{
    constexpr double leastHeat = static_cast<double>(minSiteWeight.predicted()) / (1u << siteWeightFractionBits);
    if (!(m_activations * frequencyConstants.maxFrequency >= leastHeat))
        return leastHeat;
    double heat = m_activations * m_analyses.blockFrequencies().frequencyOf(block).predicted();
    return heat >= leastHeat ? heat : leastHeat;
}

Expected<bool> FunctionSiteWeights::isInLoop(BlockID block) const
{
    return m_analyses.blockFrequencies().frequencyOf(block) > 1.0;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
