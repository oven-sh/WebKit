/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include <optional>

namespace JSC { namespace AOT {

enum class Receiver : uint8_t;

namespace Typed {

enum class PristineTest : uint8_t { NeverPasses, MayPass, ClosureAsCreated, InstanceStructure };

PristineTest pristineTestOf(const BuiltinTable&, BuiltinClassID);
std::optional<Receiver> receiverWithTestedStructure(BuiltinClassID);
BuiltinClassID classBelowWithTestedStructure(const BuiltinTable&, BuiltinClassID);
bool standsAboveClassesOfViews(const BuiltinTable&, BuiltinClassID);

inline constexpr PristineAspectSet aspectsWithoutTest { PristineAspects::AsCreated, PristineAspects::Dense, PristineAspects::Prototype };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
