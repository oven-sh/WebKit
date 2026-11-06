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
#include <wtf/TZoneMalloc.h>

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
    // The instances are laid out differently from those of the base, so no class can derive from this and from another such, nor can the mro() of a
    // metaclass put it among the bases of a class that does not derive from it: its methods take the instances for what they are. A built-in
    // type that cannot be derived from has the flag without asking, unless it says NothingOfItsOwn. (In CPython: the size of the instances says so.)
    OwnLayout = 1 << 10,
    Mutable = 1 << 11, // A built-in type whose attributes can be set all the same.
    // The instances are the runtime's own: each is a part of the one object that refers to it (it may refer back), and no program
    // gets to see it, or the type. See isObjectOfPython() in PyGC.cpp and PyFinalizationOrder.cpp.
    Internal = 1 << 12,
    // The instances are records (PyRecord), and the lists, tuples, dicts and sets in their fields are their memory: parts of them,
    // which no program gets to see. See forEachPartInFields() in PyGC.cpp.
    OwnsContainers = 1 << 13,
    // A built-in type whose instances hold nothing but what those of a class may: a __dict__, weak references. So __new__ without
    // arguments and the __dict__ make a copy (PyReduce.cpp). In CPython: the size of the instances says so.
    NothingOfItsOwn = 1 << 14,
    // __eq__ and __hash__, though written in C++, pass the question on to another object, which may be a str, or have something else to
    // say to a str. See PyDict::searchAmongStringsFor().
    AnswersForAnother = 1 << 15,
    // The instances are objects of JS's, and their attributes are their properties: they have no __dict__, and no class that derives
    // from this one adds one. What object.__getattribute__() and its kin look for in the __dict__ of another object, they look for there.
    AttributesAreProperties = 1 << 25,

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

    // For generators that only a temporary register refers to: see "Generators" in PyTemporaries.cpp. Of the type itself, not of what derives from it.
    // The instances are iterators over what the type is called with, which they let nobody else have: enumerate, zip, map, most of itertools. They are PyMultiIterators or records.
    HasIteratorsToItself = 1 << 26,
    // To call the type is to go through the first argument, of which nothing is kept: list, tuple, set, dict.
    GoesThroughArgument = 1 << 27,
};

static constexpr OptionSet<TypeFlag> inheritedTypeFlags {
    TypeFlag::HasDict, TypeFlag::HasWeakref, TypeFlag::Sequence, TypeFlag::Mapping, TypeFlag::MatchSelf, TypeFlag::OwnsContainers,
    TypeFlag::AttributesAreProperties,
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

// What PyType::lookup() found last: one table for all the types of a runtime, as CPython has one for all its types, and as the VM has one for
// all Structures (HasOwnPropertyCache, MegamorphicCache). A type has a key that no other type has or has had, and takes a new one whenever
// it changes, or one of its bases: what was found before that is never found again.
// The table keeps nothing alive but the text of the names, each until its place is taken. After each collection it forgets the values
// that are gone: the dict of a class can be written to behind the back of the class (gc.get_referents(C.__dict__)). An Eden collection
// frees nothing that has survived a collection, so after one only what has been written since the last is looked at, as in
// Heap::reconcileWeakGCHashTables().
class TypeLookupCache {
    WTF_MAKE_NONCOPYABLE(TypeLookupCache);
    WTF_MAKE_TZONE_ALLOCATED(TypeLookupCache);
public:
    TypeLookupCache() = default;

    struct Entry {
        uint64_t key { 0 };
        RefPtr<StringImpl> impl;
        JSValue value; // Empty: no class of the MRO has the name.
    };
    static constexpr unsigned size = 4096;

    ALWAYS_INLINE Entry& entryFor(uint64_t key, StringImpl* impl)
    {
        uintptr_t bits = reinterpret_cast<uintptr_t>(impl);
        return m_entries[((bits >> 4) ^ (bits >> 9) ^ (key >> 32)) % size];
    }
    ALWAYS_INLINE void remember(uint64_t key, StringImpl* impl, JSValue value)
    {
        Entry& entry = entryFor(key, impl);
        entry.key = key;
        entry.impl = impl;
        entry.value = value;
        if (value && value.isCell())
            m_written |= 1ull << (static_cast<size_t>(&entry - m_entries.data()) / entriesPerBit);
    }
    void removeUnmarked(Heap&);

private:
    static constexpr unsigned entriesPerBit = size / 64;
    static_assert(size && !(size % 64));

    std::array<Entry, size> m_entries;
    uint64_t m_written { 0 }; // A bit for each run of entries: one of them was given a cell since the last collection.
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
    static constexpr ptrdiff_t offsetOfType() { return OBJECT_OFFSETOF(PyType, m_type); }
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
    // A built-in type, or a class that the runtime has made for itself.
    bool isImmutable() const { return has(TypeFlag::Immutable) || (!isHeapType() && !has(TypeFlag::Mutable)); }

    bool isSubtypeOf(const PyType* other) const;

    // What the name is bound to in the first class of the MRO that has it, as it is there. Empty: in none.
    // The name is not a rope.
    ALWAYS_INLINE JSValue lookup(JSString* name)
    {
        StringImpl* impl = name->tryGetValueImpl();
        TypeLookupCache::Entry& entry = m_lookupCache->entryFor(m_lookupKey, impl);
        if (entry.key == m_lookupKey && entry.impl == impl) [[likely]]
            return entry.value;
        return lookupSlow(name);
    }
    // The same for a name that is an instance of a class derived from str: getattr(o, name). It is looked for as a dict looks for any
    // key, with one hash for all the classes. What its __hash__ or its __eq__ raises is lost, as in CPython: the name is then in no
    // class. All but the end of the program: after an empty answer a termination may be pending, and no code must be run then.
    JSValue lookupKey(JSGlobalObject*, JSValue name);
    // lookup() runs code of the program in one case: a key that is no str, in the dict of a class, is asked whether it equals the name.
    // Whoever looks things up, in whatever classes, and then counts on it that nothing has changed meanwhile sees by this number
    // whether that has happened, here or in another thread.
    static uint64_t timesLookupRanCode() { return s_timesLookupRanCode.load(std::memory_order_relaxed); }
    // AsksKeys::No is for whoever JSC takes to have no effects (getCallData()): no code of the program runs. A key that is no str and would say
    // that it equals the name is passed over, whether or not it has been asked before: the answer does not depend on what was looked up earlier.
    enum class AsksKeys : bool { No, Yes };
    ALWAYS_INLINE JSValue lookup(Special special, AsksKeys asksKeys = AsksKeys::Yes)
    {
        unsigned index = static_cast<unsigned>(special);
        if (m_specialStates[index] != SpecialState::Unknown && (asksKeys == AsksKeys::Yes || !m_hasAskedKeys)) [[likely]]
            return m_specials[index].get();
        return lookupSpecialSlow(special, asksKeys);
    }
    // Whether `method` is what lookup(special) finds now (code of the program's may have run since it was looked up), and that is a
    // method written in C++, of this type or of one it is derived from, that takes `self` and exactly `count` values more. To
    // call it on an instance of this type is to call the C++ function.
    ALWAYS_INLINE bool isNativeSpecialTaking(Special special, JSValue method, unsigned count) const
    {
        unsigned index = static_cast<unsigned>(special);
        return static_cast<unsigned>(m_specialStates[index]) == static_cast<unsigned>(SpecialState::NativeTakingNone) + count && m_specials[index].get() == method;
    }

    // What calling the type comes to, once typeCall() has found out. It holds until the type changes.
    enum class Construction : uint8_t {
        Unknown,
        General,
        NativeNew, // A built-in type whose __new__ does it all.
        NativeNewAndInit, // A built-in type with an __init__ of its own as well.
    };
    Construction construction() const { return m_construction; }
    void setConstruction(Construction construction) { m_construction = construction; }

    // An instance of this very class is an attribute of it, or of a class derived from it, and something is remembered about that.
    bool hasInstancesAmongAttributes() const { return m_hasInstancesAmongAttributes; }
    void setHasInstancesAmongAttributes() { m_hasInstancesAmongAttributes = true; }

    // After a change to the dict of the type, or to its bases; those derived from it are told. HowAttributesAreRead: __getattr__ or
    // __getattribute__ has been set or deleted, or __bases__ has been assigned to.
    enum class Change : bool { Any, HowAttributesAreRead };
    void didChange(VM&, Change = Change::Any);
    // A number that no other type has or had, nor this one before it last changed: what was found by looking a name up in the
    // type holds for as long as the type has the number. 0: it has none, and nothing is to be remembered about it. A type gets
    // a number when it is asked for one, and a type that keeps changing gets no more of them after a while: they are finite.
    uint32_t version()
    {
        if (m_version) [[likely]]
            return m_version;
        return takeVersion();
    }
    uint32_t versionIfAny() const { return m_version; }
    static constexpr ptrdiff_t offsetOfVersion() { return OBJECT_OFFSETOF(PyType, m_version); }
    // For the interpreter: what lookup(special) finds, if the state is not 0, and whether that is a function written in Python.
    static constexpr ptrdiff_t offsetOfSpecials() { return OBJECT_OFFSETOF(PyType, m_specials); }
    static constexpr ptrdiff_t offsetOfSpecialStates() { return OBJECT_OFFSETOF(PyType, m_specialStates); }
    static constexpr uint8_t specialIsOfNoParticularKind = 1;
    static constexpr uint8_t specialIsFunction = 2;

    // Of an instance without attributes, for a type whose instances are PyInstances.
    Structure* instanceStructure(VM& vm)
    {
        if (Structure* structure = m_instanceStructure.get()) [[likely]]
            return structure;
        return createInstanceStructure(vm);
    }

    // Type.prototype in JS: see PyJSClass.h. Null: nobody has asked for it.
    JSObject* jsPrototype() const { return m_jsPrototype.get(); }
    void setJSPrototype(VM& vm, JSObject* prototype) { m_jsPrototype.set(vm, this, prototype); }
    // Where the type is for C, if C names it by a symbol of the executable (&PyLong_Type, PyExc_ValueError: ../capi/CApi.h). That is its id() then, in every interpreter. Null: it is where the cell is.
    const void* symbolForC() const { return m_symbolForC; }
    void setSymbolForC(const void* symbol) { m_symbolForC = symbol; }

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
    // For a built-in type that cannot be derived from, whose instances are those of its base in all but the type.
    void shareLayoutOfBase(VM& vm)
    {
        m_flags.remove(TypeFlag::OwnLayout);
        setSolidBase(vm, base()->solidBase());
    }
    void setFlags(OptionSet<TypeFlag> flags) { m_flags = flags; }

private:
    PyType(VM&, Structure*);

    void takeNewLookupKey();

    JSValue lookupSlow(JSString* name, AsksKeys = AsksKeys::Yes);
    static std::atomic<uint64_t> s_timesLookupRanCode;
    JSValue lookupSpecialSlow(Special, AsksKeys);
    Structure* createInstanceStructure(VM&);
    uint32_t takeVersion();

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
    WriteBarrier<JSObject> m_jsPrototype;
    const void* m_symbolForC { nullptr };
    TypeLookupCache* m_lookupCache { nullptr }; // The runtime's.
    // Another after every change, like the version. But that is of 32 bits, for the instructions that remember it, so there are not enough for
    // every change of every type. Of these there are.
    uint64_t m_lookupKey { 0 };
    OptionSet<TypeFlag> m_flags;
    uint32_t m_version { 0 };
    uint16_t m_versionsLeft { 1000 };
    bool m_isBeingToldOfChange { false };
    bool m_hasInstancesAmongAttributes { false };
    // __getattr__ was looked for and not found. Then it is not looked for again, whatever else changes, as in CPython, where what
    // reads the attributes of the instances is replaced by something simpler. It shows where a key that is no str raised when it
    // was asked whether it is "__getattr__".
    bool m_hasGivenUpGetAttr { false };
    // What is remembered of the type may be what a key that is no str has said. For good: it only costs AsksKeys::No a walk.
    bool m_hasAskedKeys { false };
    Construction m_construction { Construction::Unknown };
    enum class SpecialState : uint8_t { Unknown, Known, Function, NativeTakingNone, NativeTakingOne, NativeTakingTwo };
    static_assert(static_cast<uint8_t>(SpecialState::Known) == specialIsOfNoParticularKind && static_cast<uint8_t>(SpecialState::Function) == specialIsFunction);
    std::array<SpecialState, numberOfSpecials> m_specialStates { };
    std::array<WriteBarrier<Unknown>, numberOfSpecials> m_specials;
};

// The 64 bits that say which object it is: what id() returns and the default hash() goes by, for every object but a str, and what C has for a PyObject* (../capi/CApi.h). For all but strs,
// `a is b` if the bits are equal. They are those of the value, but:
//  - the same number may be tagged in two ways, and None is two values;
//  - a built-in type that C names by a symbol is where the symbol is, as in CPython.
ALWAYS_INLINE uint64_t identityBits(JSValue value)
{
    EncodedJSValue bits = JSValue::encode(value);
    if (!(bits & JSValue::NotCellMask)) {
        if (bits && value.asCell()->type() == InternalFunctionType) [[unlikely]] {
            if (PyType* type = PyType::tryFrom(value.asCell())) {
                if (const void* symbol = type->symbolForC())
                    return std::bit_cast<uintptr_t>(symbol);
            }
        }
    } else if (value.isDouble())
        bits = JSValue::encode(jsNumber(value.asDouble() + 0.0)); // See numberValue().
    else if (bits == JSValue::ValueNull) [[unlikely]]
        bits = JSValue::ValueUndefined;
    return static_cast<uint64_t>(bits);
}

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
