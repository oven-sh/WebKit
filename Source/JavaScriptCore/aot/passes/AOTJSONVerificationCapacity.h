/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class CFG;

struct BuilderCapacityViolation {
    BlockID block;
    ValueID value;
    uint32_t requiredBytes { 0 };
};

std::optional<BuilderCapacityViolation> findBuilderCapacityViolation(const Program&, const Function&, const CFG&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
