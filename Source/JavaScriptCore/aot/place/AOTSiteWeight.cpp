/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSiteWeight.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"

namespace JSC { namespace AOT { namespace Typed {

FunctionSiteWeights::FunctionSiteWeights(const Program& program, const Heat& heat, FunctionID function, FunctionAnalyses& analyses)
    : m_activations(heat.activationsOf(program, function))
    , m_frequencies(analyses.blockFrequencies())
{
}

SiteWeight FunctionSiteWeights::weightOf(BlockID block) const
{
    return quantizeHeat(m_activations * m_frequencies.frequencyOf(block));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
