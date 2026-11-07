/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSObject.h"
#include <wtf/ScopedLambda.h>

namespace JSC {
namespace Python {

class PyDict;
class PyType;
class Runtime;

JSC_DECLARE_HOST_FUNCTION(callPythonObject);

// How Python values are JSValues:
//
//   None            undefined (and null, which only JS makes)
//   True, False     true, false
//   int             a number that is an integer no larger than Number.MAX_SAFE_INTEGER, however it is tagged; beyond that, a JSBigInt
//   float           any other number; one with the value of such an integer is a PyFloat
//   str             JSString
//   list            JSArray
//   function        PyFunction, a JSFunction
//   type            PyType, an InternalFunction
//   built-in function or method, method descriptor, slot wrapper: PyNativeFunction, an InternalFunction
//   anything else   a PyObject
//
// An instance of a class that derives from int, float, str or list is a PyObject (for list, a JSArray) that knows its type.

// To JS, a Python object is an exotic object whose properties are its attributes or items: see PyJSView.h.
// What a class of such cells adds to its StructureFlags, declares, and defines with PY_DEFINE_JS_VIEW().
static constexpr unsigned jsViewStructureFlags = OverridesGetOwnPropertySlot | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero
    | OverridesGetOwnPropertyNames | OverridesPut | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence | IsImmutablePrototypeExoticObject;
#define PY_DECLARE_JS_VIEW \
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&); \
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&); \
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&); \
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow); \
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&); \
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned); \
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode); \
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow)

// The prototype of every PyObject but the exceptions.
JSObject* prototypeOfPythonObjects(JSGlobalObject*);

// What is kept right behind a cell. It has to be aligned, and values above all: the collector reads them while the program stores to
// them, and only a store to an aligned address is sure to be seen whole. Half of an int looks like the address of a cell.
template<typename T, typename Cell>
ALWAYS_INLINE T* storageBehind(Cell* cell)
{
    static_assert(!(sizeof(Cell) % alignof(T)), "alignas(T) for the class of the cell");
    return reinterpret_cast<T*>(cell + 1);
}

// An object that knows its Python type. No PyObject has a destructor: what one owns is in cells of its own.
class PyObject : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData | jsViewStructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM&, JSGlobalObject*);
    static PyObject* create(VM&, Structure*, PyType*);

    // An object whose type has __call__ is a function to JS, and to the call instructions.
    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);
    PY_DECLARE_JS_VIEW;

    PyType* type() const { return m_type.get(); }
    void setType(VM&, PyType*);

    static constexpr ptrdiff_t offsetOfType() { return OBJECT_OFFSETOF(PyObject, m_type); }

protected:
    PyObject(VM&, Structure*, PyType*);

    WriteBarrier<PyType> m_type;
};

// What every class of cells declares. `jsType`: what tells its cells from others, or PyObjectType.
#define PY_DECLARE_CELL(Class, BaseClass, jsType) \
    using Base = BaseClass; \
    DECLARE_INFO; \
    static constexpr JSType cellType = jsType; \
    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject) \
    { \
        return Structure::create(vm, globalObject, prototypeOfPythonObjects(globalObject), TypeInfo(jsType, StructureFlags), info()); \
    } \
    static bool is(JSCell* cell) \
    { \
        if constexpr (jsType == PyObjectType) \
            return cell->inherits<Class>(); \
        else \
            return cell->type() == jsType; \
    } \
    static bool is(JSValue value) { return value && value.isCell() && is(value.asCell()); } \
    static Class* from(JSValue value) \
    { \
        ASSERT(is(value)); \
        return uncheckedDowncast<Class>(value.asCell()); \
    } \
    static Class* tryFrom(JSValue value) { return is(value) ? uncheckedDowncast<Class>(value.asCell()) : nullptr; }

#define PY_DEFINE_CELL(Class) \
    const ClassInfo Class::s_info = { #Class ""_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Class) }

// An instance of a class whose bases, object aside, are all written in Python.
//
// Its attributes are properties of the JS object that it is, named by the attribute's name: they are laid out by Structures, as
// those of any object of JS are, and the instructions that get and set attributes cache the Structure and the offset. Each
// class has a Structure for instances without attributes, from which those of its instances descend, so a Structure also tells
// the class. (To JS code the properties of the object are its Python attributes, which takes more than a look at these: the
// object answers for itself, see PyJSView.h.)
//
// obj.__dict__ is a dict like any other. When a program asks for it, the attributes move into one, for good, and the object has
// a Structure that says so. The same happens for an attribute whose name cannot name a property: "0", an instance of a class
// derived from str.
//
// The first two of the properties that are stored in the object itself are the fields m_type and m_dict.
class PyInstance final : public PyObject {
public:
    PY_DECLARE_CELL(PyInstance, PyObject, PyInstanceType);
    DECLARE_VISIT_CHILDREN;

    static constexpr unsigned numberOfFields = 2;
    static constexpr unsigned inlineCapacity = numberOfFields + 6;
    static constexpr size_t allocationSize(unsigned inlineCapacity) { return sizeof(JSObjectWithButterfly) + inlineCapacity * sizeof(WriteBarrierBase<Unknown>); }
    // For PyType::instanceStructure().
    static Structure* createStructure(VM&, JSGlobalObject*, Runtime&);

    static PyInstance* create(VM&, PyType*);
    static PyInstance* create(VM&, PyType*, Structure*); // PyType::instanceStructure()

    // Null: the attributes are properties.
    PyDict* dict() const { return m_dict.get(); }
    // The attributes move into a dict, if they are not in one.
    PyDict* ensureDict(JSGlobalObject*);
    // obj.__dict__ = dict
    void setDict(JSGlobalObject*, PyDict*);

    // The name is not a rope. These look no further than the object.
    JSValue getAttribute(VM&, JSString* name); // Empty: it has none of that name.
    void setAttribute(JSGlobalObject*, JSString* name, JSValue);
    bool deleteAttribute(JSGlobalObject*, JSString* name); // False: it had none of that name.
    bool hasAttributes(VM&);
    // In the order in which they were set. For as long as the function returns true.
    void forEachAttribute(JSGlobalObject*, const ScopedLambda<bool(JSString* name, JSValue)>&);

    static constexpr ptrdiff_t offsetOfDict() { return OBJECT_OFFSETOF(PyInstance, m_dict); }
    static constexpr PropertyOffset offsetOfDictAmongProperties = 1;

private:
    PyInstance(VM&, Structure*, PyType*);

    WriteBarrier<PyDict> m_dict;
};

// For the classes whose objects have something to finalize at times only (a file while it is open): what
// Runtime::registerFinalizable() and unregisterFinalizable() keep in the object itself.
struct FinalizerState {
    bool isWanted { false }; // There is something to finalize.
    bool isListed { false }; // The runtime has it in its list. It stays there until a collection finds that it is not wanted.
    bool wasFoundDead { false }; // A collection has queued it to be finalized, some time.
};

// An instance of a type written in C++ that needs no more than a few values: what each field holds is up to the type.
// (A __dict__, for a type whose instances have one, is kept as for any object without a field for it.)
class PyRecord final : public PyObject {
public:
    PY_DECLARE_CELL(PyRecord, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static size_t allocationSize(unsigned fieldCount) { return sizeof(PyRecord) + fieldCount * sizeof(WriteBarrier<Unknown>); }

    // The fields are empty.
    static PyRecord* create(VM&, Structure*, PyType*, unsigned fieldCount);
    static PyRecord* create(JSGlobalObject*, PyType*, unsigned fieldCount);

    // The record, if the value is an instance of the type, which must be one whose instances are records.
    static PyRecord* tryFrom(JSValue, PyType*);

    unsigned fieldCount() const { return m_fieldCount; }
    JSValue get(unsigned index) const
    {
        ASSERT(index < m_fieldCount);
        return fields()[index].get();
    }
    void set(VM& vm, unsigned index, JSValue value)
    {
        ASSERT(index < m_fieldCount);
        fields()[index].set(vm, this, value);
    }

    // For what is not a value.
    int64_t number { 0 };
    // TypeFlag::HasIteratorsToItself: there may be generators in there that are owned with it. See "Generators" in PyTemporaries.cpp.
    bool hasGeneratorsToItself { false };

    FinalizerState& finalizerState() { return m_finalizerState; }

private:
    PyRecord(VM&, Structure*, PyType*, unsigned fieldCount);

    WriteBarrier<Unknown>* fields() { return storageBehind<WriteBarrier<Unknown>>(this); }
    const WriteBarrier<Unknown>* fields() const { return storageBehind<const WriteBarrier<Unknown>>(this); }

    unsigned m_fieldCount;
    FinalizerState m_finalizerState;
};

inline bool isPyObject(JSCell* cell) { return isPyObjectType(cell->type()); }
inline bool isPyObject(JSValue value) { return value && value.isCell() && isPyObject(value.asCell()); }

// An operator of JS with an operand of Python's is answered in the realm of that operand: the code may be of a realm that has no Python.
inline JSGlobalObject* realmOfOperands(JSValue a, JSValue b = { })
{
    ASSERT(isPyObject(a) || isPyObject(b));
    return asObject(isPyObject(a) ? a : b)->realm();
}

inline bool isNone(JSValue value) { return value.isUndefinedOrNull(); }
inline JSValue none() { return jsUndefined(); }

} // namespace Python
} // namespace JSC
