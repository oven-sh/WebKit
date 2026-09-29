/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#pragma once

#include "JSObject.h"
#include "PyInstance.h"

namespace JSC {

// weakref.ref, and weakref.proxy, which is the same with another class: PyWeakReference of CPython's Include/cpython/weakrefobject.h.
//
// It has what it refers to without keeping it, as JSWeakObjectRef does: the collector is not told of it, and when it has found what there is to keep, each of these that it is keeping looks whether what it refers to is
// among that, and lets go of it if not.
//
// What is referred to has all the references to it, in a list, so that ref(x) is ref(x) and weakref.getweakrefs(x) can be asked for. It does not keep them either. It has a PyWeakReferenceList, in a property that
// no program can name, and that has the first of them, each of which has the next, none of which the collector is told of. When it has found what there is to keep, the list drops what is not among it.
class PyWeakReference final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyWeakReferenceSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;
    PYTHON_OVERLOADS_OPERATORS
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static CallData getCallData(JSCell*);

    // It is in no list yet.
    static PyWeakReference* create(VM&, Structure*, JSObject* referent, JSValue callback);

    // Null if it is no more.
    JSObject* referent() const { return m_referent; }
    // Empty if there is none.
    JSValue callback() const { return m_callback.get(); }
    JSValue takeCallback()
    {
        JSValue callback = m_callback.get();
        m_callback.clear();
        return callback;
    }

    // What hash() came to when it was first asked for, which is what it comes to from then on, or -1.
    int64_t cachedHash() const { return m_hash; }
    void setCachedHash(int64_t hash) { m_hash = hash; }

    PyWeakReference* next() const { return m_next; }
    // Those that were made later have more.
    uint64_t order() const { return m_order; }

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

    // Those whose callbacks are to be called are in a list of their own, which does keep them: VM::m_pythonReferencesToCall.
    PyWeakReference* takeNextToCall()
    {
        PyWeakReference* next = m_nextToCall.get();
        m_nextToCall.clear();
        return next;
    }

private:
    friend class PyWeakReferenceList;

    PyWeakReference(VM& vm, Structure* structure, JSObject* referent)
        : Base(vm, structure)
        , m_referent(referent)
    {
    }

    JSObject* m_referent;
    PyWeakReference* m_previous { nullptr };
    PyWeakReference* m_next { nullptr };
    WriteBarrier<Unknown> m_callback;
    WriteBarrier<PyWeakReference> m_nextToCall;
    int64_t m_hash { -1 };
    uint64_t m_order { 0 };
};

class PyWeakReferenceList final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyWeakReferenceListSpace<mode>();
    }

    DECLARE_INFO;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // That of an object. Null if there has never been a reference to it.
    static PyWeakReferenceList* of(VM&, JSObject*);
    static PyWeakReferenceList* ensure(VM&, JSObject*);

    PyWeakReference* head() const { return m_head; }
    void insertAtHead(PyWeakReference*);
    static void insertAfter(PyWeakReference* previous, PyWeakReference*);

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

private:
    PyWeakReferenceList(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    PyWeakReference* m_head { nullptr };
};

} // namespace JSC
