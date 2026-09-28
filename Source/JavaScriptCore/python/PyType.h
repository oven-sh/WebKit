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

#include "JSObject.h"
#include "PyTuple.h"

namespace JSC {

// A class. Its attributes are its properties. It is the prototype of its instances, and its own prototype is its first base.
class PyType final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData | ImplementsHasInstance | ImplementsDefaultHasInstance | OverridesGetOwnPropertySlot | GetOwnPropertySlotIsImpureForPropertyAbsence | GetOwnPropertySlotMayBeWrongAboutDontEnum;

    // What kind of cell an instance is. A class has the layout of its bases, of which only one may be other than Object.
    enum class Layout : uint8_t {
        Object, // A PyInstance.
        Type,
        Tuple,
        Dict,
        Set,
        List, // A JSArray.
        Boxed, // A PyBoxedValue: an instance of a class derived from int, float, str or bool's like, whose own instances are not cells.
        Native, // Some cell of its own. It cannot be derived from.
    };

    enum Flag : unsigned {
        IsHeapType = 1 << 0, // Made by a class statement, and so its attributes can be set.
        IsBaseType = 1 << 1, // It can be derived from.
        IsExceptionType = 1 << 2, // BaseException or derived from it.
        IsAbstract = 1 << 3,
        HasNoInstanceDict = 1 << 4, // __slots__, or a built-in type whose instances have no attributes of their own.
        IsTypeSubclass = 1 << 5, // A metaclass.
        IsSequence = 1 << 6, // A sequence pattern can match it.
        IsMapping = 1 << 7, // A mapping pattern can match it.
        IsBytes = 1 << 12, // bytes or derived from it: a Uint8Array that is not to be changed.
        MatchesSelf = 1 << 11, // In a class pattern, int(x) binds x to the subject itself.

        // What follows depends on the attributes of the class and of its bases, which can be set at any time. See hooks().
        HasCustomGetAttribute = 1 << 8, // __getattribute__ is not object's or type's.
        HasGetAttr = 1 << 9, // It has __getattr__.
        HasCustomSetAttr = 1 << 10, // __setattr__ or __delattr__ is not object's or type's.
    };
    static constexpr unsigned hookFlags = HasCustomGetAttribute | HasGetAttr | HasCustomSetAttr;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    // A built-in one. `base` is null for object only. It is not whole until finishBuiltin(), which needs there to be tuples.
    static PyType* createBuiltin(VM&, JSGlobalObject*, ASCIILiteral name, PyType* base, Layout, unsigned flags);
    void finishBuiltin(VM&, JSGlobalObject*, PyType* metatype);
    // What a class statement makes. The order of resolution has been worked out, and `bases` found to go together.
    static PyType* create(VM&, JSGlobalObject*, PyType* metatype, JSString* name, PyTuple* bases, PyType* base, PyTuple* mro);

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

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

    // What instances are made with. Null if there is no making one but by the type's own __new__.
    Structure* instanceStructure() const { return m_instanceStructure.get(); }
    void setInstanceStructure(VM& vm, Structure* structure) { m_instanceStructure.set(vm, this, structure); }

    // The attribute as it is stored, in this class or the first after it in the order of resolution that has it. Empty if none has.
    JSValue lookup(VM&, PropertyName) const;
    // The same, beginning after `after`, which is what super() does.
    JSValue lookupAfter(VM&, PyType* after, PropertyName) const;
    JSValue lookupOwn(VM& vm, PropertyName name) const { return getDirect(vm, name); }

    bool isSubtypeOf(const PyType*) const;

    // Sets an attribute of the class itself.
    void setAttribute(VM&, PropertyName, JSValue);
    bool deleteAttribute(VM&, JSGlobalObject*, PropertyName);

    // Which of hookFlags it has.
    unsigned hooks(JSGlobalObject*);

    // The structure that instances of a class with this layout and this class for a prototype have.
    static Structure* createInstanceStructure(VM&, JSGlobalObject*, Layout, PyType* prototype);

    static CallData getCallData(JSCell*);
    // What JavaScript finds when it looks for a property of an instance and comes to the class, or looks for one of the class. See
    // "What JavaScript sees" in README.md.
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static CallData getConstructData(JSCell*);

private:
    PyType(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    WriteBarrier<PyType> m_metatype;
    WriteBarrier<PyType> m_base;
    WriteBarrier<PyTuple> m_bases;
    WriteBarrier<PyTuple> m_mro;
    WriteBarrier<JSString> m_name;
    WriteBarrier<Structure> m_instanceStructure;
    Layout m_layout { Layout::Object };
    unsigned m_flags { 0 };
    unsigned m_hooksEpoch { 0 }; // CommonNames::typeEpoch when hookFlags were worked out.
};

inline bool isType(JSValue value) { return value.isCell() && value.asCell()->type() == PyTypeType; }
inline PyType* asType(JSValue value) { return uncheckedDowncast<PyType>(value.asCell()); }

} // namespace JSC
