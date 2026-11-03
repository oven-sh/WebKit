/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "InternalFunction.h"
#include "PyDict.h"
#include "PyNames.h"
#include "PyNative.h"
#include "PyTuple.h"

namespace JSC {
namespace Python {

JSC_DECLARE_HOST_FUNCTION(callType);
JSC_DECLARE_HOST_FUNCTION(constructType);

enum class TypeFlag : uint32_t {
    Heap = 1 << 0, // Made by a class statement, or by calling type().
    Base = 1 << 1, // Can be derived from.
    HasDict = 1 << 2, // The instances have a __dict__.
    HasWeakref = 1 << 3, // The instances can be weakly referenced.
    Abstract = 1 << 4, // __abstractmethods__ is not empty.
    Immutable = 1 << 5, // The attributes of the type cannot be set.
    DisallowInstantiation = 1 << 6,
    Sequence = 1 << 7, // A sequence pattern matches the instances.
    Mapping = 1 << 8, // A mapping pattern does.
    MatchSelf = 1 << 9, // In `case int(x)`, x is the subject itself.
    OwnLayout = 1 << 10, // The instances are laid out differently from those of the base, so no class can derive from this and from another such.
    Mutable = 1 << 11, // A built-in type whose attributes can be set all the same.
    // The instances are the runtime's own: each is a part of the one object that refers to it (it may refer back), and no program
    // gets to see it, or the type. See isObjectOfPython() in PyGC.cpp and PyFinalizationOrder.cpp.
    Internal = 1 << 12,
    // The instances are records (PyRecord), and the lists, tuples, dicts and sets in their fields are their memory: parts of them,
    // which no program gets to see. See forEachPartInFields() in PyGC.cpp.
    OwnsContainers = 1 << 13,

    // Derives from:
    IntSubclass = 1 << 16,
    FloatSubclass = 1 << 17,
    StrSubclass = 1 << 18,
    BytesSubclass = 1 << 19,
    ListSubclass = 1 << 20,
    TupleSubclass = 1 << 21,
    DictSubclass = 1 << 22,
    TypeSubclass = 1 << 23,
    ExceptionSubclass = 1 << 24, // BaseException
};

static constexpr OptionSet<TypeFlag> inheritedTypeFlags {
    TypeFlag::HasDict, TypeFlag::HasWeakref, TypeFlag::Sequence, TypeFlag::Mapping, TypeFlag::MatchSelf, TypeFlag::OwnsContainers,
    TypeFlag::IntSubclass, TypeFlag::FloatSubclass, TypeFlag::StrSubclass, TypeFlag::BytesSubclass, TypeFlag::ListSubclass,
    TypeFlag::TupleSubclass, TypeFlag::DictSubclass, TypeFlag::TypeSubclass, TypeFlag::ExceptionSubclass
};

// A built-in type, as its source file describes it.
struct TypeDef {
    ASCIILiteral name; // With the module if that is not builtins: "collections.deque".
    OptionSet<TypeFlag> flags;
    std::span<const NativeDef> methods;
    std::span<const NativeGetSet> getsets;
    ASCIILiteral doc;
};

class PyType final : public InternalFunction {
public:
    using Base = InternalFunction;
    static constexpr unsigned StructureFlags = (Base::StructureFlags & ~ImplementsDefaultHasInstance) | jsViewStructureFlags;
    static_assert(StructureFlags & ImplementsHasInstance);
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    PY_DECLARE_JS_VIEW;
    // `value instanceof Type` in JS: isinstance(value, Type).
    static bool customHasInstance(JSObject*, JSGlobalObject*, JSValue);

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(InternalFunctionType, StructureFlags), info());
    }

    static bool is(JSCell* cell) { return cell->type() == InternalFunctionType && uncheckedDowncast<InternalFunction>(cell)->nativeFunctionFor(CodeSpecializationKind::CodeForCall) == TaggedNativeFunction(callType); }
    static bool is(JSValue value) { return value && value.isCell() && is(value.asCell()); }
    static PyType* from(JSValue value)
    {
        ASSERT(is(value));
        return uncheckedDowncast<PyType>(value.asCell());
    }
    static PyType* tryFrom(JSValue value) { return is(value) ? uncheckedDowncast<PyType>(value.asCell()) : nullptr; }

    // A type with nothing in it yet. The runtime makes the built-in ones so, all of them before it fills any.
    static PyType* createBare(VM&, Structure*);
    // `base` is null only for object.
    void initializeBuiltin(JSGlobalObject*, const TypeDef&, PyType* metatype, PyType* base);
    void addMethods(JSGlobalObject*, std::span<const NativeDef>);
    void addGetSets(JSGlobalObject*, std::span<const NativeGetSet>);

    PyType* type() const { return m_type.get(); } // The metaclass.
    void setType(VM& vm, PyType* type) { m_type.set(vm, this, type); }
    JSString* nameString() const { return m_name.get(); } // __name__
    void setNameString(VM& vm, JSString* name)
    {
        m_name.set(vm, this, name);
        m_originalName.set(vm, this, name); // What JSC calls it: InternalFunction::name().
    }
    String name() const; // __name__
    // How messages name the type (tp_name in CPython): a built-in type outside builtins goes with its module, "collections.deque".
    String nameInMessages() const;
    JSString* qualname() const { return m_qualname ? m_qualname.get() : m_name.get(); } // __qualname__
    void setQualname(VM& vm, JSString* qualname) { m_qualname.set(vm, this, qualname); }
    // For a built-in type: __module__. (That of a class is in its dict.)
    JSString* builtinModule() const { return m_module.get(); }
    // The names in __slots__ that this type itself adds to its instances, mangled and sorted. Null: none.
    PyTuple* slots() const { return m_slots.get(); }
    void setSlots(VM& vm, PyTuple* slots) { m_slots.setMayBeNull(vm, this, slots); }
    PyType* base() const { return m_base.get(); }
    PyTuple* bases() const { return m_bases.get(); }
    PyTuple* mro() const { return m_mro.get(); }
    PyDict* dict() const { return m_dict.get(); }
    // The most derived of the bases that decides how instances are laid out.
    PyType* solidBase() const { return m_solidBase.get(); }

    OptionSet<TypeFlag> flags() const { return m_flags; }
    bool has(TypeFlag flag) const { return m_flags.contains(flag); }
    void add(TypeFlag flag) { m_flags.add(flag); }
    void remove(TypeFlag flag) { m_flags.remove(flag); }
    bool isHeapType() const { return has(TypeFlag::Heap); }

    bool isSubtypeOf(const PyType* other) const;

    // What the name is bound to in the first class of the MRO that has it, as it is there. Empty: in none.
    // The name is not a rope.
    ALWAYS_INLINE JSValue lookup(JSString* name)
    {
        StringImpl* impl = name->tryGetValueImpl();
        LookupCacheEntry& entry = lookupCacheEntry(impl);
        if (entry.impl == impl) [[likely]]
            return entry.value.get();
        return lookupSlow(name);
    }
    ALWAYS_INLINE JSValue lookup(Special special)
    {
        unsigned index = static_cast<unsigned>(special);
        if (m_specialIsCached[index]) [[likely]]
            return m_specials[index].get();
        return lookupSpecialSlow(special);
    }

    // After a change to the dict of the type, or to its bases.
    void didChange(VM&);
    // A number that no other type has or had, nor this one before it last changed: what was found by looking a name up in the
    // type holds for as long as the type has the number.
    uint32_t version() const { return m_version; }
    static constexpr ptrdiff_t offsetOfVersion() { return OBJECT_OFFSETOF(PyType, m_version); }

    // Of an instance without attributes, for a type whose instances are PyInstances.
    Structure* instanceStructure(VM& vm)
    {
        if (Structure* structure = m_instanceStructure.get()) [[likely]]
            return structure;
        return createInstanceStructure(vm);
    }

    // The classes derived directly from this one, oldest first. It does not keep them alive.
    void appendSubclassesTo(MarkedArgumentBuffer&) const;
    JSValue subclasses() const { return m_subclasses.get(); } // The list they are kept in, which is for nobody else. Empty: there is none.
    void addSubclass(JSGlobalObject*, PyType*);
    void removeSubclass(JSGlobalObject*, PyType*);
    // A type that has this one in its MRO without deriving from it, by an mro() of its metaclass: it is told of changes.
    void addDependent(JSGlobalObject*, PyType*);

    // ---- For type.__new__ and the setters of __bases__ and the like ----

    void setBases(VM& vm, PyTuple* bases) { m_bases.set(vm, this, bases); }
    void setBase(VM& vm, PyType* base) { m_base.set(vm, this, base); }
    void setMRO(VM& vm, PyTuple* mro) { m_mro.setMayBeNull(vm, this, mro); }
    void setDict(VM& vm, PyDict* dict) { m_dict.set(vm, this, dict); }
    void setSolidBase(VM& vm, PyType* solidBase) { m_solidBase.set(vm, this, solidBase); }
    void setFlags(OptionSet<TypeFlag> flags) { m_flags = flags; }

private:
    PyType(VM&, Structure*);

    // What lookup() found last, for some names.
    struct LookupCacheEntry {
        StringImpl* impl { nullptr };
        WriteBarrier<JSString> name; // Keeps impl alive.
        WriteBarrier<Unknown> value;
    };
    static constexpr unsigned lookupCacheSize = 16;
    LookupCacheEntry& lookupCacheEntry(StringImpl* impl)
    {
        uintptr_t bits = reinterpret_cast<uintptr_t>(impl);
        return m_lookupCache[((bits >> 4) ^ (bits >> 9)) % lookupCacheSize];
    }

    JSValue lookupSlow(JSString* name);
    JSValue lookupSpecialSlow(Special);
    Structure* createInstanceStructure(VM&);
    static uint32_t nextVersion();

    WriteBarrier<PyType> m_type;
    WriteBarrier<JSString> m_name;
    WriteBarrier<JSString> m_qualname;
    WriteBarrier<JSString> m_module;
    WriteBarrier<PyTuple> m_slots;
    WriteBarrier<PyType> m_base;
    WriteBarrier<PyType> m_solidBase;
    WriteBarrier<PyTuple> m_bases;
    WriteBarrier<PyTuple> m_mro;
    WriteBarrier<PyDict> m_dict;
    WriteBarrier<Unknown> m_subclasses; // A list, or empty: of built-in types, of weak references to classes, and of tuples of one such for dependents.
    WriteBarrier<Structure> m_instanceStructure;
    OptionSet<TypeFlag> m_flags;
    uint32_t m_version;
    std::array<bool, numberOfSpecials> m_specialIsCached { };
    std::array<WriteBarrier<Unknown>, numberOfSpecials> m_specials;
    std::array<LookupCacheEntry, lookupCacheSize> m_lookupCache;
};

// A PyDict is a dict or a frozendict, or an instance of a class derived from either.
inline bool isDict(JSValue value)
{
    auto* dict = PyDict::tryFrom(value);
    return dict && dict->type()->has(TypeFlag::DictSubclass);
}
inline bool isFrozenDict(JSValue value)
{
    auto* dict = PyDict::tryFrom(value);
    return dict && !dict->type()->has(TypeFlag::DictSubclass);
}

// The C3 linearization of the type, from its bases. Null: an exception.
PyTuple* computeMRO(JSGlobalObject*, PyType*);

} // namespace Python
} // namespace JSC
