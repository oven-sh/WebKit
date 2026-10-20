/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

enum class ConstructSyntax : uint8_t { NewExpression, SuperCall };

constexpr StubKey constructFrontEnd() { return StubKey(StubFamily::ConstructFrontEnd); }
constexpr StubKey superConstructFrontEnd() { return StubKey(StubFamily::SuperConstructFrontEnd); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
