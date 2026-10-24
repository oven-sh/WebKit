/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include "JSPropertyNameEnumerator.h"

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSString;

namespace AOT {

struct Instance;

struct EnumeratedKey {
    JSString* key { nullptr };
    uint64_t position { 0 };
};

struct EnumeratorCursor {
    uint32_t index { 0 };
    JSPropertyNameEnumerator::Flag mode { JSPropertyNameEnumerator::InitMode };
};

EnumeratorCursor cursorBefore(const JSPropertyNameEnumerator&, uint64_t position);
uint64_t positionOf(const JSPropertyNameEnumerator&, EnumeratorCursor);

JSPropertyNameEnumerator* enumeratorFor(JSGlobalObject*, JSValue object);
EnumeratedKey findEnumeratedKey(JSGlobalObject*, JSPropertyNameEnumerator*, JSValue object, uint64_t position);

void destructureIterable(JSGlobalObject*, JSValue iterable, uint32_t count, EncodedJSValue* results);

void initializeIterationWords(Instance&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
