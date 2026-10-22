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

// The one definition of a location that nothing touches. A field of this kind has no storage: the verifier, the slot representations and whoever plans an access go by isRemovedField().
// RemoveUnreadFields lists every such field among its candidates, so that the field gets its storage back where an operation names it all the same.
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
