/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkBirths.h"

namespace JSC { namespace AOT { namespace Typed {

struct Walker::MergedHeaderObject {
    uint32_t position { 0 };
    ValueID holder;
    ValueID entering;
    ValueID parameter;
    MergeableObject object;
    ValueID allocated { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
