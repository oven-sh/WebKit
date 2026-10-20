/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class CreatedConstant : uint8_t { String, NewString, Key };

// The immediate: the string constant's number, the StringID's index, the property key's number.
constexpr StubKey createConstant(CreatedConstant constant) { return StubKey(StubFamily::CreateConstant).withForm(static_cast<uint8_t>(constant)); }
// The immediate: the built-in cell's number.
constexpr StubKey builtinCell() { return StubKey(StubFamily::BuiltinCell); }

static_assert(exists(createConstant(CreatedConstant::Key)) && !exists(createConstant(CreatedConstant::Key).withForm(3)));
static_assert(exists(builtinCell()) && exists(builtinCell().withImmediate(1)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
