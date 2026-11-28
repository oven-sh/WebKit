/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTHostFunctionSources.h"
#include "AOTSchemaEnums.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

std::optional<Vector<uint8_t>> encodeByteLiteral(const TypeArena&, const HostStringEncoders&, ByteLiteralKind, StringID string, StringID encodingOrNull);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
