/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyType.h"

namespace JSC {
namespace Python {

// Classes of Python's, as classes of JS: what it takes for
//
//     class Derived extends Base { constructor(a) { super(a); this.b = 1; } method() { return super.method() + this.b; } }
//
// to be `class Derived(Base)` when Base is a class of Python's.
//
// `Derived` is a class of Python's (a PyType), made by the metaclass of Base when the class definition is evaluated: see
// deriveClassFromJS(). So __init_subclass__, __set_name__ and the metaclass run then, issubclass() and isinstance() hold, a class in
// Python can derive from it in turn, and Python finds its methods as it finds any.
//
//   method() {}          a function whose first parameter is `this` (JSMethod): instance.method(), Derived.method(instance)
//   get x() {} set x(v)  property(getter, setter)
//   static method() {}   a classmethod, whose `this` is the class it is called on
//   static x = 1         Derived.x = 1, once the class is made
//   x = 1, this.x = 1    an attribute of the instance
//   [Symbol.x]() {}      kept for JS, which finds it on the instances
//   constructor() {}     __new__: it makes the instance. `super(...)` in it is Base's __new__ and __init__.
//   super.method()       what follows Base in the MRO of the instance, Base included, bound to `this`

// Type.prototype: an object that has what the type and its bases have in their namespaces.
//
//   super.name, Reflect.get(Type.prototype, name, instance)   bound to the instance, as super() binds
//   Type.prototype.name                                       a function takes the instance as `this`: Type.prototype.name.call(instance)
//   Type.prototype.name = function () {}                      Type.name = ...
//   Object.defineProperty(Type.prototype, name, { get })      Type.name = property(...)
class PyPrototypeView final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero
        | OverridesGetOwnPropertyNames | OverridesPut | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    // The one of the type.
    static PyPrototypeView* of(JSGlobalObject*, PyType*);

    PyType* type() const { return m_type.get(); }

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

private:
    PyPrototypeView(VM&, Structure*);

    WriteBarrier<PyType> m_type;
};

// A property of an instance whose key is a symbol, if a class of its that is written in JS has it. False: none has, or an exception.
bool symbolPropertyOfClasses(JSGlobalObject*, PyType*, PropertyName, PropertySlot&);

// _js.JSMethod(function): a function of Python's that passes its first argument to the function of JS as `this`.
PyType* jsMethodType(Runtime&);
JSValue makeJSMethod(JSGlobalObject*, JSValue function);

// Whether a class derived from one written in JS, or written in JS itself, has an __init__ other than the one that those are given,
// which does nothing.
bool hasInitOfItsOwn(PyType*);

// `new.target` is a class derived from the type: super(...) in a constructor, Reflect.construct(Type, arguments, Derived).
JSValue constructBaseOfDerivedClass(JSGlobalObject*, PyType*, PyType* derived, const CallArgs&);

} // namespace Python
} // namespace JSC
