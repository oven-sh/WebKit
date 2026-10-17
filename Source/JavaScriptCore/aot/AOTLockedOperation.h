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

enum class LockedOperation : uint8_t { Change, Delete, ChangePrototype };

#if ENABLE(AOT)
JS_EXPORT_PRIVATE String lockedOperationErrorMessage(VM&, JSObject*, LockedOperation, UniquedStringImpl* name = nullptr);
#else
inline String lockedOperationErrorMessage(VM&, JSObject*, LockedOperation, UniquedStringImpl* = nullptr) { return "Cannot change a built-in: built-ins are locked"_s; }
#endif

} // namespace AOT

} // namespace JSC
