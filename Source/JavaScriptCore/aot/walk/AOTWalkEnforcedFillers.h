/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTLayout.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class Walker;

std::optional<Draft> fillerThatFitsSlot(Walker&, FieldReference);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
