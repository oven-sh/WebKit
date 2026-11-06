/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CApi.h"
#include "CApiSlots.h"

#if PYTHON_HAS_CAPI

namespace JSC {
namespace Python {
namespace CApi {

// ---- What C sees of a type: these are part of the ABI ----

struct TypeSlot {
    int slot;
    void* value;
};

struct TypeSpec {
    const char* name;
    int basicSize;
    int itemSize;
    unsigned flags;
    TypeSlot* slots;
};

// Py_TPFLAGS_*
enum TypeFlagOfC : unsigned long {
    DisallowInstantiationFlag = 1ul << 7,
    ImmutableTypeFlag = 1ul << 8,
    HeapTypeFlag = 1ul << 9,
    BaseTypeFlag = 1ul << 10,
    HaveGC = 1ul << 14,
};

using VisitFunction = int (*)(Handle, void*);
using TraverseFunction = int (*)(Handle, VisitFunction, void*);
using ClearFunction = int (*)(Handle);
using DestructorFunction = void (*)(Handle);
using FreeFunction = void (*)(void*);
using AllocFunction = Handle (*)(Handle type, ssize_t count);
using NewFunction = Handle (*)(Handle type, Handle arguments, Handle keywords);

// ---- What an extension has said about a type it has made ----

// PyType::extension() of such a type.
class MadeByExtension final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
    }
    static MadeByExtension* create(VM&, Structure*);

    // Null: no extension has made this very type.
    static MadeByExtension* of(PyType* type) { return static_cast<MadeByExtension*>(type->extension()); }

    // In the data of an instance: where the part of this type begins, how much the type asked for, and where the part of a type derived from it
    // begins, which is also how much an instance of this type has. Parts are 16-byte aligned, as in CPython.
    uint32_t dataOffset { 0 };
    uint32_t dataSize { 0 };
    uint32_t dataEnd { 0 };
    unsigned long flags { 0 };

    // What the extension gave, or what the type has inherited from the type of an extension that it derives from. Null: neither.
    template<typename Function>
    Function slot(Slot which) const { return reinterpret_cast<Function>(slots[which]); }
    std::array<void*, numberOfSlots> slots { };

private:
    MadeByExtension(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

// What PyType_GetSlot() answers where a type says nothing.
PY_CAPI Handle PyType_GenericAlloc(Handle type, ssize_t count);
PY_CAPI void PyObject_GC_Del(void*);
PY_CAPI void PyObject_Free(void*);

// Of the type, or of the nearest of those it derives from, along the bases that decide how instances are laid out. Null: its instances have nothing of C's.
MadeByExtension* madeByExtension(PyType*);

// Calls the function for each cell that tp_traverse names and to which C owns a reference, with its entry in the table. With the
// world stopped or with the GIL. (What an instance holds it owns. Of anything else that a tp_traverse may name, nobody knows whether it is still the address of a cell.)
using TraverseVisitor = ScopedLambda<void(JSCell*, CountTable::Counts&)>;
void traverse(Body*, const TraverseVisitor&);

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
