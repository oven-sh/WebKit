/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "ECMAMode.h"

namespace JSC { namespace AOT {

enum class ElementStoreMode : uint8_t { Strict, Sloppy };

constexpr StubKey getByVal(bool indexIsInteger) { return StubKey(indexIsInteger ? StubFamily::GetByValAtIndex : StubFamily::GetByVal); }
constexpr StubKey putByVal(bool indexIsInteger, ECMAMode mode) { return StubKey(indexIsInteger ? StubFamily::PutByValAtIndex : StubFamily::PutByVal).withForm(static_cast<uint8_t>(mode.isStrict() ? ElementStoreMode::Strict : ElementStoreMode::Sloppy)); }

static_assert(exists(getByVal(false)) && exists(getByVal(true)));
static_assert(exists(StubKey(StubFamily::PutByVal).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy))) && !exists(StubKey(StubFamily::PutByVal).withForm(2)));
static_assert(exists(StubKey(StubFamily::PutByValAtIndex).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy))) && !exists(StubKey(StubFamily::PutByValAtIndex).withForm(2)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
