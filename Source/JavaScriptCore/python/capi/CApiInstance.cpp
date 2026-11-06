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

// What marks, SlotVisitor, runs beside the program and on several threads: it sees the cells' own fields and nothing of C's (that is for markWhatCHolds()).
//
// Every other visitor asks what an OBJECT refers to, with the world stopped or on the thread that has the GIL: the graph of what has died (PyFinalizationOrder.cpp),
// gc.get_referents(), JSC's verifier. To them the two cells are one, which refers to what tp_traverse names, and they come to a body only if it has no face.
static JSCell* objectOf(JSCell* cell)
{
    Body* body = Body::tryFromCellOfHandle(cell);
    return body && body->face() && body->life() != Body::Life::LetGo ? body->face() : cell;
}

template<typename Visitor>
static void appendWhatCHolds(Body* body, Visitor& visitor)
{
    traverse(body, [&](JSCell* target, CountTable::Counts&) {
        visitor.appendUnbarriered(objectOf(target));
    });
}

template<typename Visitor>
void Face::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<Face>(cell);
    Base::visitChildren(thisObject, visitor);
    if constexpr (std::is_same_v<Visitor, SlotVisitor>)
        visitor.append(thisObject->m_body);
    else
        appendWhatCHolds(thisObject->body(), visitor);
}

DEFINE_VISIT_CHILDREN(Face);

template<typename Visitor>
void Body::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<Body>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_type);
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        // THE FACE LIVES AS LONG AS THE BODY DOES, whoever it is that keeps the body (C, or another instance in its data): with the face go the attributes, the identity and the weak references.
        // But for the one time when the body is kept for its end.
        if (thisObject->life() != Life::LetGo)
            visitor.append(thisObject->m_face);
    } else
        appendWhatCHolds(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN(Body);

struct TraverseArgument {
    const TraverseVisitor& visit;
    JSCell* namedButNotOwned;
};

static int callVisitor(Handle object, void* pointer)
{
    auto& argument = *static_cast<TraverseArgument*>(pointer);
    if (JSCell* cell = countedCell(object); cell && cell != argument.namedButNotOwned) {
        if (auto* counts = interpreter().counts.find(cell))
            argument.visit(cell, *counts);
    }
    return 0;
}

void traverse(Body* body, const TraverseVisitor& visit)
{
    // It may be any instance: one that C is still filling in, one in the middle of its tp_dealloc, one whose data is gone.
    if (!body->isTracked || body->life() >= Body::Life::BeingDestroyed)
        return;
    SetForScope isInTraverse(t_isInTraverse, true);
    SetForScope hasNoGIL(t_thisThread, nullptr);
    MadeByExtension* extension = madeByExtension(body->type());
    // A tp_traverse names Py_TYPE(self), as it has to in CPython, where every instance owns a reference to its type. Here those do that have a tp_dealloc to give it back.
    TraverseArgument argument { visit, ownsReferenceToType(extension) ? nullptr : body->type() };
    extension->slot<TraverseFunction>(tp_traverse)(toC(body), callVisitor, &argument);
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
