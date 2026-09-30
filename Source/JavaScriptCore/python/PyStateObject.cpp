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
#include "PyStateObject.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

namespace Python {

WTF_MAKE_TZONE_ALLOCATED_IMPL(NativeState);

} // namespace Python

const ClassInfo PyStateObject::s_info = { "object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PyStateObject) };

Structure* PyStateObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags | pythonCellFlags), info());
}

PyStateObject* PyStateObject::create(VM& vm, Structure* structure, std::unique_ptr<Python::NativeState>&& state)
{
    ASSERT(state);
    auto* object = new (NotNull, allocateCell<PyStateObject>(vm)) PyStateObject(vm, structure, WTF::move(state));
    object->finishCreation(vm);
    if (size_t size = object->m_state->memoryOutsideTheHeap())
        vm.heap.reportExtraMemoryAllocated(object, size);
    return object;
}

size_t PyStateObject::estimatedSize(JSCell* cell, VM& vm)
{
    return Base::estimatedSize(cell, vm) + uncheckedDowncast<PyStateObject>(cell)->m_state->memoryOutsideTheHeap();
}

void PyStateObject::destroy(JSCell* cell)
{
    static_cast<PyStateObject*>(cell)->~PyStateObject();
}

CallData PyStateObject::getCallData(JSCell* cell)
{
    return PyNativeObject::getCallData(cell);
}

template<typename Visitor>
void PyStateObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyStateObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    thisObject->m_state->visitChildren(visitor);
    visitor.reportExtraMemoryVisited(thisObject->m_state->memoryOutsideTheHeap());
}

DEFINE_VISIT_CHILDREN(PyStateObject);

} // namespace JSC
