/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Identifier.h"
#include "JSCJSValue.h"
#include <optional>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;

namespace AOT {

enum class KeyAtomization : bool { OnlyExisting, Atomize };
enum class KeyNameKind : uint8_t { NotAName, HasNoAtom, Index, Name };

struct KeyName {
    KeyNameKind kind { KeyNameKind::NotAName };
    UniquedStringImpl* uid { nullptr };
    uint32_t index { 0 };
};

KeyName nameOfKeyWithoutAllocating(JSGlobalObject*, JSCell* key, KeyAtomization);
std::optional<uint32_t> indexOfKey(JSValue key);
std::optional<Identifier> nameForKeyedCache(JSGlobalObject*, JSValue key);
UniquedStringImpl* atomOfKey(JSValue key);
UniquedStringImpl* uidOfKey(JSCell* key);
std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes = true);

enum class ChainLookups : uint8_t { Unknown, Ordinary };

ChainLookups classifyChainLookups(JSObject* base);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
