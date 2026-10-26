/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CPUFeatureSet.h"

#if ENABLE(ASSEMBLER)
#include "MacroAssembler.h"
#endif

namespace JSC {

static CPUFeatureSet computeHostFeatures()
{
#if ENABLE(ASSEMBLER) && (CPU(X86_64) || CPU(ARM64))
    return MacroAssembler::collectCPUFeatures();
#else
    return CPUFeatureSet::buildTarget();
#endif
}

CPUFeatureSet CPUFeatureSet::host()
{
    static const CPUFeatureSet features = computeHostFeatures();
    return features;
}

} // namespace JSC
