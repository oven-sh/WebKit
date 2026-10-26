/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEscapeAnalysis.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

ValueID appendAwait(IRBuilder&, ValueID awaited, std::span<const ValueID> held);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
