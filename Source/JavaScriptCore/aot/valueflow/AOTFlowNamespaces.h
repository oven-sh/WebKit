/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

enum class FlowOwnListing : uint8_t {
    MayHaveOthers,
    Complete,
};

FlowOwnListing exportListingOf(const Program&, ModuleID);

VariableID findStaticFieldVariable(const Program&, FunctionID classConstructor, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
