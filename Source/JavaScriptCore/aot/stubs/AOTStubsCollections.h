/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

// AnyValue is always right. String: a string, undefined or null. Bits: SameValueZero is equality of the 64 bits.
enum class KeyComparison : uint8_t { AnyValue, String, Bits };
enum class CompactTableOwner : uint8_t { Map, Set };

constexpr StubKey mapGet(KeyComparison comparison) { return StubKey(StubFamily::MapGet).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey mapHas(KeyComparison comparison) { return StubKey(StubFamily::MapHas).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey setHas(KeyComparison comparison) { return StubKey(StubFamily::SetHas).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey mapSet(KeyComparison comparison) { return StubKey(StubFamily::MapSet).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey setAdd(KeyComparison comparison) { return StubKey(StubFamily::SetAdd).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey mapDelete(KeyComparison comparison) { return StubKey(StubFamily::MapDelete).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey setDelete(KeyComparison comparison) { return StubKey(StubFamily::SetDelete).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey weakMapGet() { return StubKey(StubFamily::WeakMapGet); }
constexpr StubKey weakMapHas() { return StubKey(StubFamily::WeakMapHas); }
constexpr StubKey weakSetHas() { return StubKey(StubFamily::WeakSetHas); }
constexpr StubKey newMap() { return StubKey(StubFamily::NewMap); }
constexpr StubKey newCompactTable(CompactTableOwner owner) { return StubKey(StubFamily::NewCompactTable).withForm(static_cast<uint8_t>(owner)); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
