/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class VM;

namespace AOT::CompactStorage {

struct Position {
    JSCell* storage { nullptr };
    uint32_t entry { 0 };
    JSValue key;
    JSValue value;
};

JS_EXPORT_PRIVATE JSCell* tryCreate(JSGlobalObject*, bool hasValues);
JS_EXPORT_PRIVATE JSCell* tryCreateForFirstEntry(JSGlobalObject*, bool hasValues, JSValue key, JSValue value);
JS_EXPORT_PRIVATE JSCell* copy(JSGlobalObject*, JSCell* storage);
JS_EXPORT_PRIVATE bool has(JSGlobalObject*, JSCell* storage, JSValue key);
JS_EXPORT_PRIVATE JSValue find(JSGlobalObject*, JSCell* storage, JSValue key);
JS_EXPORT_PRIVATE void add(JSGlobalObject*, JSObject* owner, JSCell* storage, JSValue key, JSValue value);
JS_EXPORT_PRIVATE bool remove(JSGlobalObject*, JSObject* owner, JSCell* storage, JSValue key);
JS_EXPORT_PRIVATE void clear(JSGlobalObject*, JSObject* owner, JSCell* storage);
JS_EXPORT_PRIVATE uint32_t aliveEntryCount(JSCell* storage);

JS_EXPORT_PRIVATE Position transitAndNext(VM&, JSCell* storage, uint32_t entry);
JS_EXPORT_PRIVATE void setIterationEntry(JSCell* storage, uint32_t entry);
JS_EXPORT_PRIVATE uint32_t iterationEntry(JSCell* storage);
JS_EXPORT_PRIVATE JSValue keyAt(JSCell* storage, uint32_t entry);
JS_EXPORT_PRIVATE JSValue valueAt(JSCell* storage, uint32_t entry);

} // namespace AOT::CompactStorage
} // namespace JSC

#endif // ENABLE(AOT)
