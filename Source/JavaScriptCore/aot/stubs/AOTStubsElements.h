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
constexpr StubKey getByValKeepingRegisters() { return StubKey(StubFamily::GetByValKeepingRegisters); }
constexpr StubKey getByValAtConstantIndex() { return StubKey(StubFamily::GetByValAtConstantIndex); }
constexpr StubKey putByVal(bool indexIsInteger, ECMAMode mode) { return StubKey(indexIsInteger ? StubFamily::PutByValAtIndex : StubFamily::PutByVal).withForm(static_cast<uint8_t>(mode.isStrict() ? ElementStoreMode::Strict : ElementStoreMode::Sloppy)); }
constexpr StubKey putByValAtConstantIndex(ECMAMode mode) { return StubKey(StubFamily::PutByValAtConstantIndex).withForm(static_cast<uint8_t>(mode.isStrict() ? ElementStoreMode::Strict : ElementStoreMode::Sloppy)); }

constexpr bool isKnownIndex(int64_t index) { return index >= 0 && index <= static_cast<int64_t>(StubKey::maxCount); }
constexpr StubKey getByValAtKnownIndex(unsigned index) { return StubKey(StubFamily::GetByValAtKnownIndex).withCount(index); }
constexpr StubKey putByValAtKnownIndex(unsigned index, ECMAMode mode) { return StubKey(StubFamily::PutByValAtKnownIndex).withForm(static_cast<uint8_t>(mode.isStrict() ? ElementStoreMode::Strict : ElementStoreMode::Sloppy)).withCount(index); }

enum class ExpectedElements : uint8_t { Int32Array, Uint8Array, Float64Array };
inline constexpr uint8_t numberOfExpectedElements = 3;

constexpr StubKey getByValExpecting(bool indexIsInteger, ExpectedElements expected) { return StubKey(indexIsInteger ? StubFamily::GetByValAtIndexExpecting : StubFamily::GetByValExpecting).withForm(static_cast<uint8_t>(expected)); }
constexpr StubKey putByValExpecting(bool indexIsInteger, ECMAMode mode, ExpectedElements expected)
{
    return StubKey(indexIsInteger ? StubFamily::PutByValAtIndexExpecting : StubFamily::PutByValExpecting).withForm((mode.isStrict() ? 0 : numberOfExpectedElements) + static_cast<uint8_t>(expected));
}

inline constexpr unsigned maxBytesMovedByStub = 16;
constexpr StubKey moveFewBytes() { return StubKey(StubFamily::MoveFewBytes); }
constexpr StubKey moveElementsDown() { return StubKey(StubFamily::MoveElementsDown); }

static_assert(exists(getByVal(false)) && exists(getByVal(true)) && exists(getByValAtConstantIndex()) && exists(moveFewBytes()) && exists(moveElementsDown()));
static_assert(exists(getByValAtKnownIndex(0)) && exists(getByValAtKnownIndex(StubKey::maxCount)) && exists(StubKey(StubFamily::PutByValAtKnownIndex).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy)).withCount(StubKey::maxCount)));
static_assert(exists(getByValExpecting(true, ExpectedElements::Float64Array)) && !exists(StubKey(StubFamily::GetByValExpecting).withForm(numberOfExpectedElements)));
static_assert(exists(StubKey(StubFamily::PutByValAtIndexExpecting).withForm(2 * numberOfExpectedElements - 1)) && !exists(StubKey(StubFamily::PutByValExpecting).withForm(2 * numberOfExpectedElements)));
static_assert(exists(StubKey(StubFamily::PutByVal).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy))) && !exists(StubKey(StubFamily::PutByVal).withForm(2)));
static_assert(exists(StubKey(StubFamily::PutByValAtIndex).withForm(static_cast<uint8_t>(ElementStoreMode::Sloppy))) && !exists(StubKey(StubFamily::PutByValAtIndex).withForm(2)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
