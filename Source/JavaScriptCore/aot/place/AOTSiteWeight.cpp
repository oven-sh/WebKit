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

// Nobody can say whether a construction by value ever finds a constructor function.
static double activationsThatCount(const Program& program, const Heat& heat, FunctionID id)
{
    const Function& function = program.at(id);
    if (function.role == FunctionRole::GenericEntrypoint && function.derivation.parameter == allocatingConstructEntrypointParameter)
        return 0;
    return heat.estimatedActivationsOf(program, id);
}

FunctionSiteWeights::FunctionSiteWeights(const Program& program, const Heat& heat, FunctionID function, FunctionAnalyses& analyses)
    : m_activations(activationsThatCount(program, heat, function))
    , m_isLeastInEveryBlock(quantizeHeat(m_activations, frequencyConstants.maxFrequency) == minSiteWeight)
    , m_analyses(analyses)
{
}

SiteWeight FunctionSiteWeights::weightOf(BlockID block) const
{
    if (m_isLeastInEveryBlock)
        return minSiteWeight;
    return quantizeHeat(m_activations, m_analyses.blockFrequencies().frequencyOf(block));
}

double FunctionSiteWeights::heatOf(BlockID block) const
{
    // Never nothing, like the weight: a store that is there fills its field, however seldom it is taken to run.
    constexpr double leastHeat = static_cast<double>(minSiteWeight) / (1u << siteWeightFractionBits);
    if (!(m_activations * frequencyConstants.maxFrequency >= leastHeat))
        return leastHeat;
    double heat = m_activations * m_analyses.blockFrequencies().frequencyOf(block);
    return heat >= leastHeat ? heat : leastHeat;
}

bool FunctionSiteWeights::isInLoop(BlockID block) const
{
    return m_analyses.blockFrequencies().frequencyOf(block) > 1;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
