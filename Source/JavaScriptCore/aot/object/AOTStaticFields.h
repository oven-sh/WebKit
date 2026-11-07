/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class VM;

namespace AOT {

class Closure;

namespace StaticFields {

JSValue tryRead(const Closure& classObject, UniquedStringImpl*);
bool tryStore(Closure& classObject, UniquedStringImpl*, JSValue);
bool tryRedefine(Closure& classObject, UniquedStringImpl*, JSValue valueOrEmpty);
bool tryCreate(Closure& classObject, UniquedStringImpl*, JSValue);
void moveToProperties(VM&, Closure& classObject);

} // namespace StaticFields

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
