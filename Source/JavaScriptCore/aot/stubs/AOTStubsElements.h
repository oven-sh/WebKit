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

// What the type of the receiver at a site leads one to expect.
enum class ExpectedElements : uint8_t { Int32Array, Uint8Array };
inline constexpr uint8_t numberOfExpectedElements = 2;

constexpr StubKey getByValExpecting(bool indexIsInteger, ExpectedElements expected) { return StubKey(indexIsInteger ? StubFamily::GetByValAtIndexExpecting : StubFamily::GetByValExpecting).withForm(static_cast<uint8_t>(expected)); }
constexpr StubKey putByValExpecting(bool indexIsInteger, ECMAMode mode, ExpectedElements expected)
{
    return StubKey(indexIsInteger ? StubFamily::PutByValAtIndexExpecting : StubFamily::PutByValExpecting).withForm((mode.isStrict() ? 0 : numberOfExpectedElements) + static_cast<uint8_t>(expected));
}

// Handles 1 to this many bytes.
inline constexpr unsigned maxBytesMovedByStub = 16;
constexpr StubKey moveFewBytes() { return StubKey(StubFamily::MoveFewBytes); }

static_assert(exists(getByVal(false)) && exists(getByVal(true)) && exists(moveFewBytes()));
static_assert(exists(getByValExpecting(true, ExpectedElements::Uint8Array)) && !exists(StubKey(StubFamily::GetByValExpecting).withForm(numberOfExpectedElements)));
static_assert(exists(StubKey(StubFamily::PutByValAtIndexExpecting).withForm(2 * numberOfExpectedElements - 1)) && !exists(StubKey(StubFamily::PutByValExpecting).withForm(2 * numberOfExpectedElements)));
static_assert(exists(StubKey(StubFamily::PutByVal).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy))) && !exists(StubKey(StubFamily::PutByVal).withForm(2)));
static_assert(exists(StubKey(StubFamily::PutByValAtIndex).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy))) && !exists(StubKey(StubFamily::PutByValAtIndex).withForm(2)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
