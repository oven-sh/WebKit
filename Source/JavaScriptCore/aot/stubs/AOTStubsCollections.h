/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"

namespace JSC { namespace AOT {

enum class KeyComparison : uint8_t { AnyValue, String, Bits };
enum class CompactTableOwner : uint8_t { Map, Set };
enum class IteratedClass : uint8_t { Map, Set, Array };

constexpr StubKey mapGet(KeyComparison comparison) { return StubKey(StubFamily::MapGet).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey findKeyInMap(KeyComparison comparison) { return StubKey(StubFamily::FindKeyInMap).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey findCharactersInMap() { return StubKey(StubFamily::FindCharactersInMap); }
constexpr StubKey mapHas(KeyComparison comparison) { return StubKey(StubFamily::MapHas).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey setHas(KeyComparison comparison) { return mapHas(comparison); }
constexpr StubKey mapSet(KeyComparison comparison) { return StubKey(StubFamily::MapSet).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey setAdd(KeyComparison comparison) { return StubKey(StubFamily::SetAdd).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey mapDelete(KeyComparison comparison) { return StubKey(StubFamily::MapDelete).withForm(static_cast<uint8_t>(comparison)); }
constexpr StubKey setDelete(KeyComparison comparison) { return StubKey(StubFamily::SetDelete).withForm(static_cast<uint8_t>(comparison)); }
enum class TableMethod : uint8_t { MapDelete, MapGet, MapHas, MapSet, SetAdd, SetDelete, SetHas };
constexpr unsigned numberOfKeyComparisons = 3;
constexpr StubKey callMethodOfTable(TableMethod method, KeyComparison comparison) { return StubKey(StubFamily::CallMethodOfTable).withForm(static_cast<uint8_t>(method) * numberOfKeyComparisons + static_cast<uint8_t>(comparison)); }
constexpr StubKey callMethodOfProvenTable(TableMethod method, KeyComparison comparison) { return callMethodOfTable(method, comparison).withFamily(StubFamily::CallMethodOfProvenTable); }
constexpr bool isCallOfMethodOfTable(StubKey key) { return key.family() == StubFamily::CallMethodOfTable || key.family() == StubFamily::CallMethodOfProvenTable; }
constexpr TableMethod tableMethodOf(StubKey key) { return static_cast<TableMethod>(key.form() / numberOfKeyComparisons); }
constexpr KeyComparison keyComparisonOf(StubKey key) { return static_cast<KeyComparison>(isCallOfMethodOfTable(key) ? key.form() % numberOfKeyComparisons : key.form()); }
constexpr StubKey stubOfTableMethod(TableMethod method, KeyComparison comparison)
{
    switch (method) {
    case TableMethod::MapDelete: return mapDelete(comparison);
    case TableMethod::MapGet: return mapGet(comparison);
    case TableMethod::MapHas: return mapHas(comparison);
    case TableMethod::MapSet: return mapSet(comparison);
    case TableMethod::SetAdd: return setAdd(comparison);
    case TableMethod::SetDelete: return setDelete(comparison);
    case TableMethod::SetHas: return setHas(comparison);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}
constexpr StubKey formForAnyValueOf(StubKey key) { return key.withForm(static_cast<uint8_t>(KeyComparison::AnyValue)); }
constexpr StubKey weakMapGet() { return StubKey(StubFamily::WeakMapGet); }
constexpr StubKey weakMapHas() { return StubKey(StubFamily::WeakMapHas); }
constexpr StubKey weakSetHas() { return StubKey(StubFamily::WeakSetHas); }
constexpr StubKey newMap() { return StubKey(StubFamily::NewMap); }
constexpr StubKey newSet() { return StubKey(StubFamily::NewSet); }
constexpr StubKey newIterator(IteratedClass iterated) { return StubKey(StubFamily::NewIterator).withForm(static_cast<uint8_t>(iterated)); }
constexpr StubKey nextSetElement() { return StubKey(StubFamily::NextSetElement); }
constexpr StubKey newCompactTable(CompactTableOwner owner) { return StubKey(StubFamily::NewCompactTable).withForm(static_cast<uint8_t>(owner)); }
constexpr StubKey growCompactTable(CompactTableOwner owner) { return StubKey(StubFamily::GrowCompactTable).withForm(static_cast<uint8_t>(owner)); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
