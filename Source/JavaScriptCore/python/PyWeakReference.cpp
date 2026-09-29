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


#include "config.h"
#include "PyWeakReference.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PythonCommonNames.h"
#include "PythonOperations.h"

namespace JSC {

const ClassInfo PyWeakReference::s_info = { "weakref"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyWeakReference) };
const ClassInfo PyWeakReferenceList::s_info = { "PyWeakReferenceList"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(PyWeakReferenceList) };

Structure* PyWeakReference::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

Structure* PyWeakReferenceList::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

PyWeakReference* PyWeakReference::create(VM& vm, Structure* structure, JSObject* referent, JSValue callback)
{
    auto* reference = new (NotNull, allocateCell<PyWeakReference>(vm)) PyWeakReference(vm, structure, referent);
    reference->finishCreation(vm);
    if (callback)
        reference->m_callback.set(vm, reference, callback);
    reference->m_order = vm.nextPythonWeakReferenceOrder();
    return reference;
}

CallData PyWeakReference::getCallData(JSCell* cell)
{
    return PyNativeObject::getCallData(cell);
}

template<typename Visitor>
void PyWeakReference::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyWeakReference>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_callback);
    visitor.append(thisObject->m_nextToCall);
}

DEFINE_VISIT_CHILDREN(PyWeakReference);

void PyWeakReference::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    if (!m_referent || vm.heap.isMarked(m_referent))
        return;
    m_referent = nullptr;
    // The list has gone with it, and so may the others that were in it have.
    m_previous = nullptr;
    m_next = nullptr;
    // This is no time to be running anything. It is called when Python code next looks whether there is anything of the kind to be done.
    if (m_callback) {
        m_nextToCall.setWithoutWriteBarrier(vm.pythonReferencesToCall());
        vm.addPythonReferenceToCall(this);
    }
}

PyWeakReferenceList* PyWeakReferenceList::of(VM& vm, JSObject* object)
{
    JSValue list = object->getDirect(vm, vm.pythonNames().private_weakReferences);
    return list ? uncheckedDowncast<PyWeakReferenceList>(list.asCell()) : nullptr;
}

PyWeakReferenceList* PyWeakReferenceList::ensure(VM& vm, JSObject* object)
{
    if (auto* list = of(vm, object))
        return list;
    auto* list = new (NotNull, allocateCell<PyWeakReferenceList>(vm)) PyWeakReferenceList(vm, vm.pythonWeakReferenceListStructure.get());
    list->finishCreation(vm);
    object->putDirect(vm, vm.pythonNames().private_weakReferences, list, PropertyAttribute::DontEnum | 0);
    return list;
}

void PyWeakReferenceList::insertAtHead(PyWeakReference* reference)
{
    reference->m_previous = nullptr;
    reference->m_next = m_head;
    if (m_head)
        m_head->m_previous = reference;
    m_head = reference;
}

void PyWeakReferenceList::insertAfter(PyWeakReference* previous, PyWeakReference* reference)
{
    reference->m_previous = previous;
    reference->m_next = previous->m_next;
    if (previous->m_next)
        previous->m_next->m_previous = reference;
    previous->m_next = reference;
}

void PyWeakReferenceList::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope)
{
    PyWeakReference* last = nullptr;
    PyWeakReference* next;
    for (PyWeakReference* reference = std::exchange(m_head, nullptr); reference; reference = next) {
        // What is not being kept has not been swept yet, so it still says what came after it.
        next = reference->m_next;
        if (!vm.heap.isMarked(reference))
            continue;
        reference->m_previous = last;
        reference->m_next = nullptr;
        if (last)
            last->m_next = reference;
        else
            m_head = reference;
        last = reference;
    }
}

} // namespace JSC
