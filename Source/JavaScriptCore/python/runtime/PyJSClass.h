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
//   constructor() {}     __new__: it makes the instance. `super(...)` in it is the __new__ and the __init__ that follow Derived in the MRO
//                        of the class that is being instantiated. Derived.__init__ does nothing then, unless the class has one.
//   no constructor, and nothing that one would have to do (fields, private methods): neither __new__ nor __init__, as for
//                        `class Derived(Base): pass`. So __init__(a, b) {} can be written as in Python.
//   super.method()       what follows Derived in the MRO of `this`, bound to it: super().method()

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
    // What `super` is in the methods of the type, which is written in JS: it has what the types have that follow the type in the MRO of
    // `this`. It is the prototype of their home object.
    static PyPrototypeView* createSuper(JSGlobalObject*, PyType*);

    PyType* type() const { return m_type.get(); }
    bool isSuper() const { return m_isSuper; }

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

private:
    PyPrototypeView(VM&, Structure*);

    WriteBarrier<PyType> m_type;
    bool m_isSuper { false };
};

// What `super` is in the constructor and in the static methods of a type that is written in JS: the prototype of the constructor.
//
//   super(...)        the __new__, and then the __init__, that follow the type in the MRO of new.target
//   super.method()    what follows the type in the MRO of `this`, which is a class: super().method() in a classmethod
class PySuperConstructor final : public InternalFunction {
public:
    using Base = InternalFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(InternalFunctionType, StructureFlags), info());
    }

    static PySuperConstructor* create(JSGlobalObject*, PyType*);

    PyType* type() const { return m_type.get(); }

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);

private:
    PySuperConstructor(VM&, Structure*);

    WriteBarrier<PyType> m_type;
};

// The other way round: `class Derived(Base)` in Python, where Base is a class of JS.
//
//     class Lines(stream.Readable):
//         def _read(self, size): self.push(...)
//
// Base stands among the bases for a class of Python's that is made for it (JSFunction.__mro_entries__, typeForClassOfJS()), which derives
// from the one of the class that Base extends, and in the end from JSObject. An instance is an object of JS, made by Base itself
// (`Reflect.construct(Base, arguments, Derived)`: that is Base.__new__), so it is all that an instance of Base is to JS: a Map, an
// EventTarget, with Base's fields and private names. Its prototype is Derived.prototype, behind which comes Base.prototype. By
// that Python knows its class (Runtime::typeOf()), and JS finds what Derived defines: Base's own code calls this._read().
// As with every class that has a __new__, it is __new__ that gets the arguments for Base. Base.__init__ does nothing, but objects to
// arguments other than those.

// The class of Python's that stands for the class of JS. Null: an exception; it is not something to derive from.
PyType* typeForClassOfJS(JSGlobalObject*, JSObject* constructor);
// Base.prototype, if the type is one of those. Null: it is not.
JSObject* prototypeOfClassOfJS(PyType*);
// What Base has itself (its static members), read for a class derived from it, which is `this` in them. Empty: nothing, or an exception.
JSValue staticMemberOfClassOfJS(JSGlobalObject*, PyType*, JSString* name);
// Type.prototype: that, or the PyPrototypeView.
JSObject* prototypeForJS(JSGlobalObject*, PyType*);
// The class of Python's that the object of JS is an instance of, going by its prototype. Null: none.
PyType* typeByPrototype(JSObject*);

// A property of an instance whose key is a symbol, if a class of its that is written in JS has it. False: none has, or an exception.
bool symbolPropertyOfClasses(JSGlobalObject*, PyType*, PropertyName, PropertySlot&);

// _js.JSMethod(function): a function of Python's that passes its first argument to the function of JS as `this`.
PyType* jsMethodType(Runtime&);
JSValue makeJSMethod(JSGlobalObject*, JSValue function);

// A function of JS, or a JSMethod of one: what is a plain function where a class is made. (__init_subclass__ is a classmethod then.)
bool isFunctionOfJS(Runtime&, JSValue);

// Whether a class derived from one written in JS, or written in JS itself, has an __init__ other than the one that those are given,
// which does nothing.
bool hasInitOfItsOwn(PyType*);
// Whether the class itself, which is written in JS, has the __init__ that does nothing: its constructor sees to those of its bases.
bool constructorInitializes(PyType*);

// `new.target` is a class derived from the type: super(...) in a constructor, Reflect.construct(Type, arguments, Derived).
JSValue constructBaseOfDerivedClass(JSGlobalObject*, PyType*, PyType* derived, const CallArgs&);

} // namespace Python
} // namespace JSC
