/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSArray.h"
#include "PyObject.h"

namespace JSC {
namespace Python {

// A list is a JSArray, the same one JS sees. A hole, which only JS can make, reads as None, and so does an item that JS has made an
// accessor of. What JS has forbidden (Object.freeze()) is refused as JS refuses it in strict code.

// An instance of a class derived from list.
class PyListObject final : public JSArray {
public:
    using Base = JSArray;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertyNames | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    // To JS it is an array that also has the attributes that are not those of every list: see PyJSView.h.
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static PyListObject* create(VM&, Structure*, PyType*);

    static bool is(JSCell* cell) { return cell->type() == DerivedArrayType && cell->inherits<PyListObject>(); }

    PyType* type() const { return m_type.get(); }
    void setType(VM&, PyType*);

private:
    PyListObject(VM&, Structure*, PyType*, Butterfly*);

    WriteBarrier<PyType> m_type;
};

ALWAYS_INLINE bool isExactList(JSValue value) { return value && value.isCell() && value.asCell()->type() == ArrayType; }
// isJSArray() is false for DerivedArrayType, which is what the instances of classes derived from list are.
ALWAYS_INLINE bool isList(JSValue value) { return value && value.isCell() && (value.asCell()->type() == ArrayType || value.asCell()->type() == DerivedArrayType); }
ALWAYS_INLINE JSArray* asList(JSValue value)
{
    ASSERT(isList(value));
    return uncheckedDowncast<JSArray>(value.asCell());
}
ALWAYS_INLINE JSArray* tryAsList(JSValue value) { return isList(value) ? asList(value) : nullptr; }

// Null: out of memory, with an exception.
JSArray* newList(JSGlobalObject*, unsigned capacity = 0);
JSArray* newList(JSGlobalObject*, std::span<const JSValue>);

ALWAYS_INLINE unsigned listLength(JSArray* list) { return list->length(); }
// For an array whose items are not in a row: JS made it sparse, or froze it.
JSValue listGetSlow(JSArray*, unsigned index);
ALWAYS_INLINE JSValue listGet(JSArray* list, unsigned index)
{
    if (JSValue value = list->tryGetIndexQuickly(index)) [[likely]]
        return value;
    return listGetSlow(list, index);
}
void listSet(JSGlobalObject*, JSArray*, unsigned index, JSValue);
void listAppend(JSGlobalObject*, JSArray*, JSValue);
void listInsert(JSGlobalObject*, JSArray*, unsigned index, JSValue);
// Makes room for `count` items at `index`, which are None until they are set.
void listInsertGap(JSGlobalObject*, JSArray*, unsigned index, unsigned count);
void listRemove(JSGlobalObject*, JSArray*, unsigned index, unsigned count = 1);
void listTruncate(JSGlobalObject*, JSArray*, unsigned length);
// list.extend(iterable)
void listExtend(JSGlobalObject*, JSArray*, JSValue iterable);
// list(iterable)
JSArray* listFromIterable(JSGlobalObject*, JSValue iterable);
// tuple(iterable)
class PyTuple;
PyTuple* tupleFromIterable(JSGlobalObject*, JSValue iterable);
PyTuple* tupleFromList(JSGlobalObject*, JSArray*);
// list.sort(key=key, reverse=reverse). An empty key is none.
void listSort(JSGlobalObject*, JSArray*, JSValue key, bool reverse);

} // namespace Python
} // namespace JSC
