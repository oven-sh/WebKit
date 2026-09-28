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

#pragma once

#include "ErrorType.h"
#include "JSObject.h"
#include "PyTuple.h"
#include "Watchpoint.h"
#include "Weak.h"

namespace JSC {

// A class. Its attributes are its properties. It is the prototype of its instances, and its own prototype is its first base.
//
// To JavaScript it is an exotic object, and so are its instances: getting, setting and deleting a property, and asking whether there is one,
// are getattr(), setattr(), delattr() and hasattr(). "The two languages" in README.md says why.
//
// A class that JavaScript made is a class to Python as it is: a constructor, and the object that is the prototype of what it makes. Nothing is made of it
// that a program can see. But there is as much to know about it as about any class, and that is kept in one of these, which the constructor has under a
// private name, as a function has its FunctionRareData. What a program is given for such a class is the constructor: object().
class PyType final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData | ImplementsHasInstance | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    // What kind of cell an instance is. A class has the layout of its bases, of which only one may be other than Object.
    enum class Layout : uint8_t {
        Object, // A PyInstance.
        Type,
        Tuple,
        Dict,
        Set,
        List, // A JSArray.
        Exception, // A PyException, which is an ErrorInstance.
        Boxed, // A PyBoxedValue: an instance of a class derived from int, float, str or bool's like, whose own instances are not cells.
        Native, // Some cell of its own. It cannot be derived from.
        JavaScript, // Whatever a constructor of JavaScript's makes, the class being derived from one and from nothing of Python's but object.
    };

    enum Flag : unsigned {
        IsHeapType = 1 << 0, // Made by a class statement, and so its attributes can be set.
        IsBaseType = 1 << 1, // It can be derived from.
        IsExceptionType = 1 << 2, // BaseException or derived from it.
        IsAbstract = 1 << 3,
        HasInstanceDict = 1 << 4, // Its instances have a __dict__: attributes besides those that the class provides for.
        IsTypeSubclass = 1 << 5, // A metaclass.
        IsSequence = 1 << 6, // A sequence pattern can match it.
        IsMapping = 1 << 7, // A mapping pattern can match it.
        IsBytes = 1 << 12, // bytes or derived from it: a Uint8Array that is not to be changed.
        MatchesSelf = 1 << 11, // In a class pattern, int(x) binds x to the subject itself.
        HasWeakReferences = 1 << 13, // There can be weak references to its instances.
        MayHaveForeignDict = 1 << 14, // Some instance has been given a __dict__ that is another object's too. See attributeStorage().
        IsJavaScript = 1 << 15, // JavaScript made it, and everything that it is derived from. So the attributes of an instance are its properties, as JavaScript finds them.

        // What follows depends on the attributes of the class and of its bases, which can be set at any time. See hooks().
        HasCustomGetAttribute = 1 << 8, // __getattribute__ is not object's or type's.
        HasGetAttr = 1 << 9, // It has __getattr__.
        HasCustomSetAttr = 1 << 10, // __setattr__ or __delattr__ is not object's or type's.
    };
    static constexpr unsigned hookFlags = HasCustomGetAttribute | HasGetAttr | HasCustomSetAttr;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyTypeSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    PYTHON_OVERLOADS_OPERATORS
    DECLARE_VISIT_CHILDREN;

    // A built-in one. `base` is null for object only. It is not whole until finishBuiltin(), which needs there to be tuples.
    static PyType* createBuiltin(VM&, JSGlobalObject*, ASCIILiteral name, PyType* base, Layout, unsigned flags);
    void finishBuiltin(VM&, JSGlobalObject*, PyType* metatype);
    // What a class statement makes. The order of resolution has been worked out, and `bases` found to go together.
    static PyType* create(VM&, JSGlobalObject*, PyType* metatype, JSString* name, PyTuple* bases, PyType* base, PyTuple* mro);

    // What there is to know about a class that JavaScript made. See Python::classFor().
    static PyType* createForJavaScript(VM&, JSGlobalObject*, JSObject* constructor, JSObject* prototype, PyType* base);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // For a built-in one that stands for a built-in class of JavaScript's, as the class of all objects does for Object.
    void setJavaScriptClass(VM&, JSObject* constructor, JSObject* prototype);

    // Null unless JavaScript made it.
    JSObject* javaScriptConstructor() const { return m_javaScriptConstructor.get(); }
    JSObject* javaScriptPrototype() const { return m_javaScriptPrototype.get(); }
    // The class, as a program has it. Nothing else is to be given to one.
    JSObject* object() { return m_javaScriptConstructor ? m_javaScriptConstructor.get() : this; }
    // What its instances have for a prototype.
    JSObject* prototypeObject() { return m_javaScriptPrototype ? m_javaScriptPrototype.get() : this; }

    PyType* metatype() const { return m_metatype.get(); }
    void setMetatype(VM& vm, PyType* metatype) { m_metatype.set(vm, this, metatype); }
    PyType* base() const { return m_base.get(); }
    PyTuple* bases() const { return m_bases.get(); }
    PyTuple* mro() const { return m_mro.get(); }
    JSString* name() const { return m_name.get(); }
    void setName(VM& vm, JSString* name) { m_name.set(vm, this, name); }
    String nameString(JSGlobalObject*) const;
    Layout layout() const { return m_layout; }

    bool hasFlag(Flag flag) const { return m_flags & flag; }
    void setFlag(Flag flag) { m_flags |= flag; }
    void clearFlag(Flag flag) { m_flags &= ~flag; }
    bool isExceptionType() const { return hasFlag(IsExceptionType); }

    // type.__basicsize__, __itemsize__, __dictoffset__ and __weakrefoffset__. Nothing here is laid out as in CPython, so as sizes they mean nothing. But
    // they are what CPython goes by to tell whether two classes can both be derived from, which is if the instances of one are laid out as those of
    // the other and then some, and whether instances can be given __slots__. So they are kept as CPython would have them, and gone by for the same.
    int basicSize() const { return m_basicSize; }
    int itemSize() const { return m_itemSize; }
    int dictOffset() const { return m_dictOffset; }
    int weakReferenceOffset() const { return m_weakReferenceOffset; }
    // type.__flags__, as CPython would have it. What is gone by here is flags().
    unsigned long flagsForPython() const;
    // For a class that is being made: it has so many __slots__, and its instances have these where those of its base have not.
    void addToLayout(unsigned slots, bool addsDict, bool addsWeakReferences);
    // A built-in class that is derived from object and nothing else, and has no __new__ of its own, does not have object's either, though that is what
    // looking it up finds. Instances of it are made by other means: an iterator is what iter() gives.
    bool cannotBeInstantiated(VM&) const;
    // For a class of exceptions: which of JavaScript's kinds of Error its instances are.
    ErrorType errorType() const { return m_errorType; }
    void setErrorType(ErrorType errorType) { m_errorType = errorType; }

    // What instances are made with. Null if there is no making one but by the type's own __new__.
    Structure* instanceStructure() const { return m_instanceStructure.get(); }
    void setInstanceStructure(VM& vm, Structure* structure) { m_instanceStructure.set(vm, this, structure); }

    // The attribute as it is stored, in this class or the first after it in the order of resolution that has it. Empty if none has.
    JSValue lookup(VM&, PropertyName) const;
    // The same, beginning after `after`, which is what super() does.
    JSValue lookupAfter(VM&, PyType* after, PropertyName) const;
    // The same, beginning with `from`.
    JSValue lookupFrom(VM&, PyType* from, PropertyName) const;
    JSValue lookupOwn(VM&, PropertyName) const;
    // The same as lookup(), for an attribute of the class itself. That takes in what is `static` in a class of JavaScript's, which is a property of the
    // constructor.
    JSValue lookupOnClass(VM&, PropertyName, bool& isStatic, PyType* from = nullptr) const;

    bool isSubtypeOf(const PyType*) const;

    // Sets an attribute of the class itself.
    void setAttribute(VM&, PropertyName, JSValue);
    bool deleteAttribute(VM&, JSGlobalObject*, PropertyName);

    // Which of hookFlags it has.
    unsigned hooks(JSGlobalObject*);

    // The classes that are derived from it directly and are still there, in the order that they were made.
    Vector<PyType*> subclasses() const;

    // An attribute that an instance has of its own is what getattr() gives, and setattr() sets it, unless the class has something to say about
    // it: a descriptor of that name with __set__ or __delete__, or its own __getattribute__, __setattr__ or __delattr__. Whether it has is
    // looked into when the attribute is first asked for, and what is found holds for as long as this does. It stops holding when the class,
    // or one that it is derived from, is given such a thing after it was made, which hardly ever happens.
    WatchpointSet& instanceAccessIsAsFound() { return m_instanceAccessIsAsFound.get(); }

    // The structure that instances of a class with this layout and this class for a prototype have.
    static Structure* createInstanceStructure(VM&, JSGlobalObject*, Layout, JSObject* prototype);
    // The same, for a class that is derived from `base`.
    static Structure* createInstanceStructure(VM&, JSGlobalObject*, PyType* base, JSObject* prototype);

    static CallData getCallData(JSCell*);
    // What JavaScript finds when it looks for a property of an instance and comes to the class, or looks for one of the class. See
    // "What JavaScript sees" in README.md.
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    // x instanceof C is isinstance(x, C).
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);
    static CallData getConstructData(JSCell*);

private:
    PyType(VM& vm, Structure* structure)
        : Base(vm, structure)
        , m_instanceAccessIsAsFound(WatchpointSet::create(IsWatched))
    {
    }

    void addSubclass(PyType*);
    void instanceAccessMayHaveChanged(VM&);
    void attributeDidChange(VM&, PropertyName);
    void forgetHooks();

    WriteBarrier<PyType> m_metatype;
    WriteBarrier<PyType> m_base;
    WriteBarrier<PyTuple> m_bases;
    WriteBarrier<PyTuple> m_mro;
    WriteBarrier<JSString> m_name;
    WriteBarrier<Structure> m_instanceStructure;
    WriteBarrier<JSObject> m_javaScriptConstructor;
    WriteBarrier<JSObject> m_javaScriptPrototype;
    Vector<Weak<PyType>> m_subclasses;
    const Ref<WatchpointSet> m_instanceAccessIsAsFound;
    Layout m_layout { Layout::Object };
    int m_basicSize { 0 };
    int m_itemSize { 0 };
    int m_dictOffset { 0 };
    int m_weakReferenceOffset { 0 };
    unsigned long m_flagsForPython { 0 };
    ErrorType m_errorType { ErrorType::Error };
    unsigned m_flags { 0 };
    bool m_knowsHooks { false }; // Whether hookFlags are as they would be worked out to be.
};

// What a class of JavaScript's calls when it has been defined, if it may be derived from one of Python's.
JSC_DECLARE_HOST_FUNCTION(pythonClassWasDefined);

namespace Python {
JS_EXPORT_PRIVATE bool isJavaScriptClass(JSCell*);
JS_EXPORT_PRIVATE PyType* classFor(JSObject* constructor);
}

// Whether it is one of these cells. What a program has for a class need not be.
inline bool isType(JSValue value) { return value.isCell() && value.asCell()->type() == PyTypeType; }
// Whether it is a class, of either language's making.
inline bool isClass(JSValue value) { return value.isCell() && (value.asCell()->type() == PyTypeType || Python::isJavaScriptClass(value.asCell())); }
// What there is to know about a class.
inline PyType* asType(JSValue value)
{
    JSCell* cell = value.asCell();
    if (cell->type() == PyTypeType) [[likely]]
        return uncheckedDowncast<PyType>(cell);
    return Python::classFor(asObject(cell));
}

} // namespace JSC
