/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTCFG.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

enum class PristineTest : uint8_t { NeverPasses, MayPass };

PristineTest pristineTestOf(const BuiltinTable&, BuiltinClassID);

// Nothing in an object says that it stays as created, or that it has no holes. Only flow proves these.
inline constexpr PristineAspectSet aspectsWithoutTest { PristineAspects::AsCreated, PristineAspects::Dense };

enum class PristineProof : uint8_t { Unknown, Proven };

struct PristineQuery {
    const Program& program;
    const Function& function;
    FunctionAnalyses* analyses { nullptr };
    std::span<const Value> passedTests { };
};

PristineProof pristineProofOf(const PristineQuery&, ValueID, const PristineRequirement&, ProgramPoint);
PristineAspectSet aspectsProvenAt(const PristineQuery&, ValueID, BuiltinClassID, ProgramPoint);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
