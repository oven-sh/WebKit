/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/text/WTFString.h>

namespace JSC {

class JSObject;
class VM;

namespace AOT {

enum class RefusedByLock : uint8_t { Change, Delete, ChangeOfPrototype };

#if ENABLE(AOT)
JS_EXPORT_PRIVATE String wordsOfLock(VM&, JSObject*, RefusedByLock, UniquedStringImpl* name = nullptr);
#else
inline String wordsOfLock(VM&, JSObject*, RefusedByLock, UniquedStringImpl* = nullptr) { return "Cannot change a built-in: built-ins are locked"_s; }
#endif

} // namespace AOT

} // namespace JSC
