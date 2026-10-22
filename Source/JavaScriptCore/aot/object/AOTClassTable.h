/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "AOTLayoutData.h"
#include "AOTObjectWords.h"
#include "CollectionScope.h"
#include "JSCJSValue.h"
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class GetterSetter;
class JSFinalObject;
class JSGlobalObject;
class JSObject;
class Structure;
class VM;

namespace AOT {

class Closure;
class CompiledStructure;
class Object;
struct State;

class ClassTable {
    WTF_MAKE_NONCOPYABLE(ClassTable);
public:
    explicit ClassTable(State&);
    ~ClassTable();

    const LayoutData* classWithIndex(uint32_t) const;
    const LayoutData* classConstructedBy(FunctionID) const;
    const LayoutData* baseOf(const LayoutData& layout) const { return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base()); }
    const LayoutData& classOfInstances(const LayoutData& instanceLayout) const
    {
        const LayoutData* current = &instanceLayout;
        while (current->isDerived && current->isClass())
            current = baseOf(*current);
        return *current;
    }
    // The proven base, else the expected one, which holds only while the class's link guards are set.
    const LayoutData* assumedBaseOf(const LayoutData& layout) const { return layout.assumedBase() == LayoutData::noBase ? nullptr : classWithIndex(layout.assumedBase()); }
    static bool hasOneClassObject(const LayoutData&);
    bool keepsStructuresForNextEvaluation(const Closure& classObject, const LayoutData&) const;
    static bool hasComputedMemberNames(const LayoutData&);
    static bool isDefinedByClass(const LayoutData&);
    bool hasStaticMethodNamed(const LayoutData&, UniquedStringImpl*) const;
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    JSValue readStaticField(const FieldData&) const;
    bool tryStoreStaticField(VM&, const FieldData&, JSValue);

    JSObject* classPrototype(VM&, const LayoutData&);
    // For whoever is about to make an object of the class. The guards of a constructor function speak of such objects, and are armed here where the function has only been a base so far.
    JSObject* classPrototypeOfInstances(VM&, const LayoutData&);
    // For a class or a function below this one, whose guards look here for what it inherits.
    JSObject* classPrototypeOfBase(VM&, const LayoutData&);
    // No function is constructed before it has its prototype property.
    void didCreatePrototypePropertyOf(Closure*);
    JSObject* classPrototypeIfExists(const LayoutData&) const;
    JSObject* createClassPrototype(VM&, const LayoutData&, JSObject* parent);
    void registerClassPrototype(const LayoutData&, JSObject*);
    // What a class object has while nothing has looked at its properties. Class objects with one parent share it.
    Structure* lazyClassObjectStructureInheritingFrom(VM&, JSObject* parent);
    // What a class object has whose only property is `prototype`. Class objects with one parent share it, so a property is added to such an object with a transition.
    Structure* classObjectStructureInheritingFrom(VM&, JSObject* parent);
    size_t removeDeadEntries(VM&, CollectionScope);

    enum class ClassPart : uint8_t { Everything, ClassObject, PrototypeMembers };
    JSValue resolveHeritage(JSGlobalObject*, JSValue heritage);
    // For a class that has one class object, when the closure table is first asked for it: by a read of a variable that is born as the class, by the first allocation of an instance,
    // by the definition of a class below. Where the record says all that the definition of the class does, no operation defines the class, and this does.
    Closure* createClassObject(VM&, const LayoutData&);
    void defineClass(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue parent, JSValue prototypeParent);
    void defineClass(JSGlobalObject*, Closure* classObject, JSValue parent, JSValue prototypeParent);
    void createClassPart(JSGlobalObject*, Closure* classObject, ClassPart);
    JSObject* createMethodClosure(VM&, Closure* classObject, const LayoutData&, const MethodData&);
    // For a class proven to be evaluated once, before the run that evaluates it starts again. After the first call, what the earlier run made asks this table for nothing.
    void finishLazyParts(VM&, const LayoutData&);
    void forgetDefinition(VM&, const LayoutData&);

    uint64_t* guardWords(const LayoutData&, ClassSide) const;
    // AsDefinitionMadeIt: the definition has made the holder an ordinary object just now, and the holder has what the record says it creates and nothing else.
    // NotTouched: the definition has made the other holder.
    enum class HolderState : uint8_t { Unknown, AsDefinitionMadeIt, NotTouched };
    void armMemberGuards(VM&, const LayoutData&, ClassSide, JSObject* holder, HolderState = HolderState::Unknown);
    // The definition of such a class makes nothing but, where only the definition knows its parent, the stand-in for the prototype object. While nobody has looked at them, its two holders
    // are in no table: each names its class, reports every change by itself, and the guards hold while their bits are clear.
    static bool leavesDefinitionToRecord(const LayoutData& classLayout) { return classLayout.createsMembersLazily && classLayout.createsClassObjectOnMutation && classLayout.classData().guardsHoldWhileClear; }
    void didCreatePrototypeLeftToRecord(const LayoutData&);
    void didCreateHolder(const LayoutData&, ClassSide);
    void disarmMemberGuards(const LayoutData&);
    // The failing side of the test that stores pass a member stores by name, and its first store comes to the run time.
    void didStoreByName(VM&, const LayoutData& instanceLayout, UniquedStringImpl*);
    // Whoever files the store in a cache has walked the chain above an object that has the prototype object of its class: the store creates the property until one of these objects reports a change.
    void didFindThatStoresPass(VM&, const LayoutData& instanceLayout, UniquedStringImpl*, std::span<JSObject* const> changeableObjects);
    void didChangeMember(const LayoutData&, ClassSide, UniquedStringImpl*);
    void didDefineMember(VM&, const LayoutData&, ClassSide, JSObject* holder, UniquedStringImpl*, JSObject* function);
    class MemberDefinition {
        WTF_MAKE_NONCOPYABLE(MemberDefinition);
    public:
        MemberDefinition(ClassTable& classes, const LayoutData& classLayout, ClassSide side, UniquedStringImpl* uid)
            : m_classes(classes)
        {
            classes.m_memberBeingDefined = { &classLayout, side, uid };
        }

        ~MemberDefinition() { m_classes.m_memberBeingDefined = { }; }

    private:
        ClassTable& m_classes;
    };
    void didChangeEveryMember(const LayoutData&, ClassSide);
    void didChangePrototypeLink(const LayoutData&, ClassSide);
    void didDefineWithOtherParent(const LayoutData&, ClassSide);
    void didShadowMember(const LayoutData& instanceLayout, UniquedStringImpl*);
    void didChangeInstancePrototype(const LayoutData& instanceLayout);
    void didChangeInstancePrototype(VM&, const LayoutData& instanceLayout, JSValue prototype);
    const LayoutData* classHeldBy(JSObject* holder, ClassSide&) const;

    bool declaresOwnMember(const LayoutData&, UniquedStringImpl*) const;
    std::optional<JSObject*> findDeclaredMethod(VM&, const LayoutData&, UniquedStringImpl*);
    GetterSetter* createDeclaredAccessor(VM&, const LayoutData&, UniquedStringImpl*);
    struct DeclaredMember {
        void define(const MethodData&);
        bool isAccessor() const { return getter || setter; }

        const MethodData* method { nullptr };
        const MethodData* getter { nullptr };
        const MethodData* setter { nullptr };
    };
    struct DeclaredMemberLocation {
        JSObject* holder;
        PropertyOffset offset;
        bool isAccessor;
    };
    struct LazyPrototypeLookupResult {
        std::optional<DeclaredMemberLocation> member;
        JSObject* firstObjectAbove;
    };
    enum class MemberCreation : uint8_t { Allowed, Forbidden };
    std::optional<LazyPrototypeLookupResult> findThroughLazyPrototypes(VM&, JSObject* base, UniquedStringImpl*, bool isAbsentFromBase, MemberCreation = MemberCreation::Allowed);
    bool hasPrototypeMembers(const LayoutData&) const;

    JSObject* createThis(JSGlobalObject*, JSObject* constructor, JSObject* newTarget);
    JSObject* prototypeForInstancesOf(VM&, JSObject* compiledCallee);
    ALWAYS_INLINE JSObject* tryReadPrototypePropertyAtRememberedPlace(JSCell* function) const
    {
        uint32_t structureID = function->structureID().bits();
        const PlaceOfPrototypeProperty& place = m_placesOfPrototypeProperties[indexOfPlace(structureID)];
        if (place.structureID != structureID) {
            const PrototypeOfClassObject& remembered = m_prototypesOfClassObjects[indexOfClassObject(function)];
            return remembered.classObject == function ? asObject(remembered.prototype) : nullptr;
        }
        JSValue prototype = asObject(function)->getDirect(place.offset);
        return prototype.isObject() ? asObject(prototype) : nullptr;
    }
    void rememberPlaceOfPrototypeProperty(VM&, Closure* function, JSObject* prototype);
    JSObject* instanceWithPrototype(VM&, Object* allocated, JSObject* prototype);

private:
    using ByIndex = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    static constexpr unsigned declaredMembersPerHolder = 62;

    DeclaredMemberLocation declaredMemberLocationAt(uint32_t location, bool isAccessor) const;
    std::optional<DeclaredMemberLocation> findDeclaredMemberLocation(FunctionID key, bool isAccessor) const;
    DeclaredMemberLocation ensureDeclaredMemberLocation(VM&, FunctionID);
    DeclaredMemberLocation ensureDeclaredMemberLocation(VM&, const DeclaredMember& accessor);
    template<typename CreateMember> DeclaredMemberLocation ensureDeclaredMemberLocation(VM&, FunctionID key, bool isAccessor, const CreateMember&);
    Structure* lazyClassObjectStructure(VM&, const LayoutData&);
    Object* createClassInstance(VM&, const LayoutData&, JSObject* prototype);
    static JSFinalObject* createOrdinaryInstance(JSGlobalObject*, unsigned numberOfProperties, JSObject* prototype);
    const LayoutData* classWithPrototype(JSObject* prototype) const;
    enum class UseOfFunction : bool { MakesObjects, IsBase };
    JSObject* defineClassOfConstructorFunction(VM&, const LayoutData&, UseOfFunction);
    void armGuardsOfConstructorFunction(VM&, const LayoutData&, JSObject* prototype, Closure* classObject);
    void checkParentsOfConstructorFunction(const LayoutData&, JSObject* prototype, Closure* classObject);
    void enterHolderWithoutGuards(VM&, const LayoutData&, ClassSide, JSObject* holder);
    bool tryLeaveDefinitionToRecord(VM&, Closure* classObject, const LayoutData&, JSValue heritage, JSValue prototypeParent);
    bool tryDefineAgainWithKeptStructures(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue heritage, JSValue prototypeParent);
    const LayoutData* classLeftToRecordHeldBy(JSObject* holder, ClassSide&) const;

    static_assert(std::is_same_v<PropertyOffset, decltype(PlaceOfPrototypeProperty::offset)>);
    static size_t indexOfPlace(uint32_t structureID) { return structureID >> shiftOfStructureIDForPlaceOfPrototypeProperty & (numberOfPlacesOfPrototypeProperties - 1); }

    static size_t indexOfClassObject(const JSCell* classObject) { return std::bit_cast<uintptr_t>(classObject) >> shiftOfClassObjectForPrototype & (numberOfPrototypesOfClassObjects - 1); }

    std::array<PlaceOfPrototypeProperty, numberOfPlacesOfPrototypeProperties> m_placesOfPrototypeProperties { };
    std::array<PrototypeOfClassObject, numberOfPrototypesOfClassObjects> m_prototypesOfClassObjects { };
    // What instanceWithPrototype() last answered where the prototype object of a class below the allocated one selected that class: `super(..)` out of one subclass asks the same every time.
    struct InstanceOfClassBelow {
        JSObject* prototype;
        const LayoutData* allocatedLayout;
        CompiledStructure* structure;
    };
    static constexpr size_t numberOfInstancesOfClassesBelow = 16;
    static size_t indexOfInstanceOfClassBelow(const JSObject* prototype) { return std::bit_cast<uintptr_t>(prototype) >> 4 & (numberOfInstancesOfClassesBelow - 1); }
    std::array<InstanceOfClassBelow, numberOfInstancesOfClassesBelow> m_instancesOfClassesBelow { };
    State& m_state;
    const ProgramTables& m_tables;
    mutable std::array<std::atomic<uint64_t>, 64> m_recentClassLookups { };
    UncheckedKeyHashMap<JSObject*, Structure*> m_classPrototypeStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_lazyClassObjectStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_classObjectStructures;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_classSidesByHolder;
    RememberedSlotSet m_writes;
    Vector<JSObject*> m_declaredMemberHolders;
    ByIndex m_declaredMemberLocations;
    uint32_t m_numberOfDeclaredMemberLocations { 0 };
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_classesWithForgottenDefinition;
    struct {
        const LayoutData* classLayout { nullptr };
        ClassSide side { ClassSide::Prototype };
        UniquedStringImpl* uid { nullptr };
    } m_memberBeingDefined;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constructorFunctionsWithoutClass;
    // Their holders are entered and report what happens to them, for the classes below. No object of their own has been made, so no guard of their own is armed.
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constructorFunctionsThatAreOnlyBases;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sidesEnteredWithoutGuards;
    struct NamedGuardsBelow {
        bool hasWalked { false };
        bool isListed { false };
        Vector<uint64_t> namesAndClasses;
    };
    std::span<const uint64_t> classesBelowWithGuardsNamed(NamedGuardsBelow&, const LayoutData&, ClassSide, uint32_t key);
    UncheckedKeyHashMap<uint32_t, NamedGuardsBelow, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_namedGuardsBelow;
};

// The closure table has one cell for each function, and only the program can create a closure with captures.
inline bool ClassTable::hasOneClassObject(const LayoutData& classLayout)
{
    FunctionID constructor(classLayout.classData().classConstructor);
    return constructor && classLayout.evaluation() == EvaluationState::ProvenOnce && !captureCountsOf(constructor).captures;
}

// The other classes are those of constructor functions.
inline bool ClassTable::isDefinedByClass(const LayoutData& layout)
{
    if (!layout.isClass())
        return false;
    FunctionID constructor(layout.classData().classConstructor);
    return constructor && functionKindData(constructor).isConstructor;
}

inline bool ClassTable::hasComputedMemberNames(const LayoutData& classLayout)
{
    auto hasComputedName = [](const MethodData& method) {
        return !!(method.flags & methodHasComputedName);
    };
    return std::ranges::any_of(classLayout.methods(), hasComputedName) || std::ranges::any_of(classLayout.staticMethods(), hasComputedName);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
