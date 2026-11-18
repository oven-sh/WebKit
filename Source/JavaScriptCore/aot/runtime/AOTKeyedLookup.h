/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSCJSValue.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class JSString;

namespace AOT {

struct Instance;
struct ReturnedCaptureData;

class KeyedLookup {
    WTF_MAKE_NONCOPYABLE(KeyedLookup);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    KeyedLookup(const JSCell* callee, JSGlobalObject* realmOfCallee);

    explicit operator bool() const { return m_row; }
    bool hasObject() const { return m_object; }
    JSValue tryGet(JSGlobalObject*, JSString* key);
    void didCall() { m_object = objectInVariable(); }

private:
    JSObject* objectInVariable() const;
    JSValue tryGetFromObject(JSGlobalObject*, JSString* key);
    JSValue tryGetOwn(JSGlobalObject*, JSString* key);

    const JSCell* m_callee;
    Instance* m_instance { nullptr };
    const ReturnedCaptureData* m_row { nullptr };
    JSObject* m_object { nullptr };
};

inline JSValue KeyedLookup::tryGet(JSGlobalObject* globalObject, JSString* key)
{
    return m_object ? tryGetFromObject(globalObject, key) : JSValue();
}

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
