/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CApiHooks.h"
#include "PyObject.h"

#if PYTHON_HAS_CAPI

namespace JSC {
namespace Python {
namespace CApi {

// An instance of a type that an extension has made, or of a class that derives from one (TypeFlag::InstancesHaveDataOfC), IS TWO CELLS.
//
// THE FACE is what Python and JS see and hold: the type, the __dict__ and the __slots__ of classes that derive from the type, what weak references refer to, what `is`
// compares. It is an object like any other, and lives and dies by the collector's rules like any other.
//
// THE BODY is what C sees: its data, and the address that is the PyObject* for the whole life of the instance, tp_dealloc included. id() says that address too. No code of
// Python's or JS's is ever given a body: toJS() gives the face, and is the only way from a handle to a value.
//
// The face refers to the body, and the body to the face. So the two are found dead together.
// WHY TWO. tp_dealloc is the end of what C keeps, and nobody may have the object then. Whether somebody can still get at a cell that is kept alive for a call to come is more than anybody can tell:
// JSC and Bun hold addresses in many ways without marking them (weak handles, WeakRef, tables that are gone through when a collection ends, bare pointers in native code that go by
// the mark), and all of them take a cell that is marked for alive. A face is not kept. When it has died, each of them has been told, as of any cell that dies. What is kept
// for tp_dealloc is the body, of which none of them has ever heard.

class Body;

class Face final : public PyObject {
public:
    PY_DECLARE_CELL(Face, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static Face* create(VM&, Structure*, PyType*, Body*);

    // Null: it is something else. (Not by the type: __class__ can be assigned to.)
    static ALWAYS_INLINE Face* tryFromCell(JSCell* cell)
    {
        return cell->type() == PyObjectType && cell->classInfo() == info() ? uncheckedDowncast<Face>(cell) : nullptr;
    }

    Body* body() const { return m_body.get(); }

private:
    Face(VM&, Structure*, PyType*, Body*);

    WriteBarrier<Body> m_body;
};

// What C keeps lies behind the cell, in the cell's own memory: each type of the chain says how much it adds to its base, and gets at its part with PyObject_GetTypeData(). Cells do
// not move, so the address is good for the life of the object. The memory is zero at first.
//
// The collector does not look into the data while it marks: C stores there without telling anybody, and only C knows what is a reference. It
// asks tp_traverse, with the world stopped: see "The C API" in ../README.md.
class Body final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
    }
    // Null: there is no room for it.
    static Body* tryCreate(VM&, Structure*, PyType*, size_t dataSize);

    // Null: it is something else.
    static ALWAYS_INLINE Body* tryFromCell(JSCell* cell)
    {
        return cell->type() == CellType && cell->classInfo() == info() ? uncheckedDowncast<Body>(cell) : nullptr;
    }
    // The same for a cell that C has a handle for. Such a cell is an object of Python's or a body, and no object of Python's is of this JSType.
    static ALWAYS_INLINE Body* tryFromCellOfHandle(JSCell* cell)
    {
        if (cell->type() != CellType) [[likely]]
            return nullptr;
        ASSERT(cell->classInfo() == info());
        return uncheckedDowncast<Body>(cell);
    }

    // The face's.
    PyType* type() const { return m_type.get(); }
    void setType(VM& vm, PyType* type) { m_type.set(vm, this, type); }

    Face* face() const { return m_face.get(); }
    void setFace(VM& vm, Face* face) { m_face.set(vm, this, face); }

    // 16-byte aligned. (A cell that is too large for a block lies at an address that is 8 more than a multiple of 16.)
    void* data() { return std::bit_cast<void*>(roundUpToMultipleOf<dataAlignment>(std::bit_cast<uintptr_t>(this + 1))); }
    static constexpr size_t dataAlignment = 16;

    // All of this is read and written with the GIL, or by the collector with the world stopped.

    // PyObject_GC_Track(), PyObject_GC_UnTrack(): whether tp_traverse may be called.
    bool isTracked : 1 { false };
    // For the collector, within a collection: it has been asked what it holds since it was marked, and since the program last ran.
    bool wasAsked : 1 { false };

private:
    Body(VM&, Structure*, PyType*);

    WriteBarrier<PyType> m_type;
    WriteBarrier<Face> m_face;
};

static_assert(sizeof(Face) == 32);
static_assert(sizeof(Body) == 32 && !(sizeof(Body) % Body::dataAlignment));

// gc.is_tracked()
bool isTracked(Body*);

ALWAYS_INLINE Face* faceOf(Body* body) { return body->face(); }

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
