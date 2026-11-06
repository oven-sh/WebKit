/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApiInstance.h"

#include "CApiType.h"
#include "JSCInlines.h"

#if PYTHON_HAS_CAPI

namespace JSC {
namespace Python {
namespace CApi {

PY_DEFINE_CELL(Face);
const ClassInfo Body::s_info = { "PythonCApiBody"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(Body) };

Face::Face(VM& vm, Structure* structure, PyType* type, Body* body)
    : Base(vm, structure, type)
    , m_body(body, WriteBarrierEarlyInit)
{
}

Face* Face::create(VM& vm, Structure* structure, PyType* type, Body* body)
{
    auto* face = new (NotNull, allocateCell<Face>(vm)) Face(vm, structure, type, body);
    face->finishCreation(vm);
    return face;
}

Body::Body(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure)
    , m_type(type, WriteBarrierEarlyInit)
{
}

Body* Body::tryCreate(VM& vm, Structure* structure, PyType* type, size_t dataSize)
{
    ASSERT(!(dataSize % dataAlignment));
    size_t size = sizeof(Body) + dataSize;
    // See data(). The question is the one that the allocation itself asks.
    if (!vm.cellSpace().allocatorFor(size, AllocatorForMode::EnsureAllocator))
        size += dataAlignment / 2;
    void* cell = tryAllocateCell<Body>(vm, size);
    if (!cell)
        return nullptr;
    auto* body = new (NotNull, cell) Body(vm, structure, type);
    ASSERT(static_cast<uint8_t*>(body->data()) + dataSize <= static_cast<uint8_t*>(cell) + size);
    memset(body->data(), 0, dataSize);
    body->finishCreation(vm);
    return body;
}

template<typename Visitor>
void Face::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<Face>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_body);
}

DEFINE_VISIT_CHILDREN(Face);

template<typename Visitor>
void Body::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<Body>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
    // THE FACE LIVES AS LONG AS THE BODY DOES, whoever it is that keeps the body (C, or another instance in its data): with the face go the attributes, the identity and the weak references.
    visitor.append(thisObject->m_face);
}

DEFINE_VISIT_CHILDREN(Body);

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
