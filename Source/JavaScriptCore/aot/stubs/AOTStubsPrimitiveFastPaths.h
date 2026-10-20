/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

constexpr StubKey toNumberAsDouble() { return StubKey(StubFamily::ToNumberAsDouble); }
constexpr StubKey toStringFrontEnd() { return StubKey(StubFamily::ToStringFrontEnd); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
