/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;

void removeUnusedParameters(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

void removeEntryParameters(Function&, const BitVector& removedEntryParameters);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
