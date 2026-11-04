/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

bool hasPrototypeProperty(const Program&, const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
