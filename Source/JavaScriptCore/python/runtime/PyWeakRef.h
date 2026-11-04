/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

namespace JSC {
namespace Python {

// weakref.ref, weakref.proxy: a pointer the collector does not follow.
//
// At the end of a collection every weak reference that survived it is asked to look at its referent (as JSWeakObjectRef is):
// if that did not survive, the reference is cleared, and its callback is queued for the runtime to call at the next safe point.
class PyWeakRef final : public PyObject {
public:
    PY_DECLARE_CELL(PyWeakRef, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm) { return vm.pyWeakRefSpace<mode>(); }

    static PyWeakRef* create(VM&, Structure*, PyType*, JSCell* referent, JSValue callback);

    JSCell* referent() const { return m_referent; } // Null: it is gone.
    JSValue callback() const { return m_callback.get(); } // Empty: none, or it has been called.
    void clearCallback() { m_callback.clear(); }

    bool hasHash() const { return m_hasHash; }
    int64_t hash() const { return m_hash; }
    void setHash(int64_t hash)
    {
        m_hash = hash;
        m_hasHash = true;
    }

    // Later ones have greater numbers.
    uint64_t serial() const { return m_serial; }
    // That made it (Runtime::currentThread()), which is the one to call the callback.
    uint32_t thread() const { return m_thread; }
    // Not one that a program made: see newWeakRef().
    bool isInternal() const { return m_isInternal; }
    void setIsInternal() { m_isInternal = true; }
    void setReferent(JSCell* referent) { m_referent = referent; }

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

private:
    PyWeakRef(VM&, Structure*, PyType*, JSCell* referent);

    JSCell* m_referent;
    WriteBarrier<Unknown> m_callback;
    int64_t m_hash { 0 };
    uint64_t m_serial;
    uint32_t m_thread { 0 };
    bool m_hasHash { false };
    bool m_isInternal { false };
};

// The referent of a ref or a proxy, or None.
JSValue weakRefGet(JSValue weakRef);
// weakref.ref(object). Null: a TypeError, the object cannot be referred to weakly.
PyWeakRef* makeWeakRef(JSGlobalObject*, JSValue object, JSValue callback = { });
// object.__weakref__: the first of weakref.getweakrefs(object), or None.
JSValue firstWeakRefTo(JSGlobalObject*, JSValue object);

} // namespace Python
} // namespace JSC
