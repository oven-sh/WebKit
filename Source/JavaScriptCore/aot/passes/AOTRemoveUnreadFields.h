/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;
struct OptimizationInputs;

void removeUnreadFields(ProgramEditor&, const OptimizationInputs&, const PassRunner&);

constexpr bool hasNoAccesses(const LocationFlow& flow)
{
    return flow.writers == WriterState::AllKnown && flow.stores == StoreCount::Never && flow.reads == ReadState::NeverRead;
}

inline bool isRemovedField(const Program& program, FieldReference field)
{
    return hasNoAccesses(flowOf(program, field).location);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
