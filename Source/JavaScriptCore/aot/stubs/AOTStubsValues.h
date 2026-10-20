/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeTable.h"
#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class Equality : uint8_t { Loose, Strict };

constexpr StubKey toBoolean() { return StubKey(StubFamily::ToBoolean); }
constexpr StubKey valueIsTruthy() { return StubKey(StubFamily::ValueIsTruthy); }
constexpr StubKey equal(bool isStrict) { return StubKey(StubFamily::Equal).withForm(static_cast<uint8_t>(isStrict ? Equality::Strict : Equality::Loose)); }

static_assert(exists(toBoolean()) && exists(valueIsTruthy()));
static_assert(exists(equal(false)) && exists(equal(true)));
static_assert(!exists(equal(true).withForm(static_cast<uint8_t>(Equality::Strict) + 1)));

void callBinaryOperation(StubAssembler&, Entry);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
