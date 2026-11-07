/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "ArgList.h"
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
// The items of a list where they lie, if they lie there as values, for as long as nothing changes the list. An empty one is a hole.
// No data: they lie there in another way.
ALWAYS_INLINE std::span<const JSValue> listValues(JSArray* list)
{
    IndexingType indexingType = list->indexingType();
    if (!hasContiguous(indexingType) && !hasInt32(indexingType))
        return { };
    Butterfly* butterfly = list->butterfly();
    return { std::bit_cast<const JSValue*>(butterfly->contiguous().data()), butterfly->publicLength() };
}
// All the items, as listGet() gives them, to the front of `to`, which has room for them. What is left of it is returned. Allocates nothing.
std::span<JSValue> listCopyTo(std::span<JSValue> to, JSArray*);
void listSet(JSGlobalObject*, JSArray*, unsigned index, JSValue);
void listAppend(JSGlobalObject*, JSArray*, JSValue);
void listInsert(JSGlobalObject*, JSArray*, unsigned index, JSValue);
// Makes room for `count` items at `index`, which are None until they are set.
void listInsertGap(JSGlobalObject*, JSArray*, unsigned index, unsigned count);
void listRemove(JSGlobalObject*, JSArray*, unsigned index, unsigned count = 1);
void listTruncate(JSGlobalObject*, JSArray*, unsigned length);
// list.extend(iterable)
// list.extend() asks the iterable how many items are to come. NotAsked: an instance of a class of the program's is not.
enum class LengthHint : bool { Asked, NotAsked };
void listExtend(JSGlobalObject*, JSArray*, JSValue iterable, LengthHint = LengthHint::Asked);
// list(iterable)
JSArray* listFromIterable(JSGlobalObject*, JSValue iterable);
// tuple(iterable)
class PyTuple;
PyTuple* tupleFromIterable(JSGlobalObject*, JSValue iterable);
PyTuple* tupleFromList(JSGlobalObject*, JSArray*);
// list.sort(key=key, reverse=reverse). An empty key is none.
void listSort(JSGlobalObject*, JSArray*, JSValue key, bool reverse);

// All the items of an iterable, for what takes them before it does anything with them. A list or a tuple itself is not asked for an
// iterator, and its items stay where they are. Anything else is gone through as list(iter(iterable)) goes through it.
class AllItems {
    WTF_MAKE_NONCOPYABLE(AllItems);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // If code may run while the span is in use, it may change the list: then the items of a list are copied as well.
    enum class WhileInUse : bool { NothingRuns, CodeMayRun };
    // May throw. Whatever TypeError asking for the iterator ends in becomes one with this message.
    AllItems(JSGlobalObject* globalObject, JSValue iterable, ASCIILiteral messageIfNotIterable, WhileInUse whileInUse = WhileInUse::NothingRuns) { take(globalObject, iterable, messageIfNotIterable, whileInUse); }
    ~AllItems() { ensureStillAliveHere(m_owner); }
    // An empty value is None: a hole in a list.
    std::span<const JSValue> span() const { return m_span; }

private:
    void take(JSGlobalObject*, JSValue iterable, ASCIILiteral messageIfNotIterable, WhileInUse);

    std::span<const JSValue> m_span;
    JSValue m_owner; // The list or tuple that the span lies in.
    MarkedArgumentBuffer m_kept; // Or it lies here.
};

} // namespace Python
} // namespace JSC
