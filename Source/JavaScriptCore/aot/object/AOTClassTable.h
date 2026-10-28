/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "AOTLayoutData.h"
#include "AOTObjectWords.h"
#include "AOTVector.h"
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
    static bool hasOneClassObject(const LayoutData& classLayout)
    {
        FunctionID constructor(classLayout.classData().classConstructor);
        return constructor && classLayout.evaluation() == EvaluationState::ProvenOnce && !captureCountsOf(constructor).captures;
    }
    bool keepsStructuresForNextEvaluation(const Closure& classObject, const LayoutData&) const;
    static bool hasComputedMemberNames(const LayoutData& classLayout)
    {
        auto hasComputedName = [](const MethodData& method) {
            return !!(method.flags & methodHasComputedName);
        };
        return std::ranges::any_of(classLayout.methods(), hasComputedName) || std::ranges::any_of(classLayout.staticMethods(), hasComputedName);
    }
    static bool isDefinedByClass(const LayoutData& layout)
    {
        if (!layout.isClass())
            return false;
        FunctionID constructor(layout.classData().classConstructor);
        return constructor && functionKindData(constructor).isConstructor;
    }
    static bool isClassOfCreatedObjects(const LayoutData& layout) { return layout.isClass() && !FunctionID(layout.classData().classConstructor); }
    bool awaitsPrototypeOfCreatedObjects(const LayoutData&) const;
    bool defineClassOfCreatedObjects(VM&, const LayoutData&, JSObject* prototype);
    bool hasStaticMethodNamed(const LayoutData&, UniquedStringImpl*) const;
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    JSValue readStaticField(const FieldData&) const;
    bool tryStoreStaticField(VM&, const FieldData&, JSValue);

    JSObject* classPrototype(VM&, const LayoutData&);
    JSObject* classPrototypeOfInstances(VM&, const LayoutData&);
    JSObject* classPrototypeOfBase(VM&, const LayoutData&);
    // No function is constructed before it has its prototype property.
    void didCreatePrototypePropertyOf(Closure*);
    JSObject* classPrototypeIfExists(const LayoutData&) const;
    JSObject* createClassPrototype(VM&, const LayoutData&, JSObject* parent);
    void registerClassPrototype(const LayoutData&, JSObject*);
    Structure* lazyClassObjectStructureInheritingFrom(VM&, JSObject* parent);
    Structure* classObjectStructureInheritingFrom(VM&, JSObject* parent);
    size_t removeDeadEntries(VM&, CollectionScope);

    enum class ClassPart : uint8_t { Everything, ClassObject, PrototypeMembers };
    JSValue resolveHeritage(JSGlobalObject*, JSValue heritage);
    Closure* createClassObject(VM&, const LayoutData&);
    void defineClass(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue parent, JSValue prototypeParent);
    void defineClass(JSGlobalObject*, Closure* classObject, JSValue parent, JSValue prototypeParent);
    void createClassPart(JSGlobalObject*, Closure* classObject, ClassPart);
    JSObject* createMethodClosure(VM&, Closure* classObject, const LayoutData&, const MethodData&);

    uint64_t* guardWords(const LayoutData&, ClassSide) const;
    // AsDefinitionMadeIt: the definition has made the holder an ordinary object just now, and the holder has what the record says it creates and nothing else.
    enum class HolderState : uint8_t { Unknown, AsDefinitionMadeIt, NotTouched };
    void armMemberGuards(VM&, const LayoutData&, ClassSide, JSObject* holder, HolderState = HolderState::Unknown);
    static bool leavesDefinitionToRecord(const LayoutData& classLayout) { return classLayout.createsMembersLazily && classLayout.createsClassObjectOnMutation && classLayout.classData().guardsHoldWhileClear; }
    void didCreatePrototypeLeftToRecord(const LayoutData&);
    void disarmMemberGuards(const LayoutData&);
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
    class InternalPutScope {
        WTF_MAKE_NONCOPYABLE(InternalPutScope);
    public:
        explicit InternalPutScope(ClassTable& classes)
            : m_classes(classes)
        {
            ++classes.m_internalPutDepth;
        }

        ~InternalPutScope() { --m_classes.m_internalPutDepth; }

    private:
        ClassTable& m_classes;
    };
    bool isInInternalPut() const { return m_internalPutDepth; }
    void didChangeEveryMember(const LayoutData&, ClassSide);
    void didChangePrototypeLink(const LayoutData&, ClassSide);
    void didDefineWithOtherParent(const LayoutData&, ClassSide);
    void didShadowMember(const LayoutData& instanceLayout, UniquedStringImpl*);
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
    Object* createInstanceOfClassBelow(VM&, size_t indexOfRecord);
    // What an allocation makes for a prototype is remembered under its number.
    JSObject* instanceWithPrototype(VM&, Object* allocated, JSObject* prototype, bool holdsNoField = false, std::optional<uint32_t> allocation = std::nullopt);

private:
    using ByIndex = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    static constexpr unsigned declaredMembersPerHolder = 62;

    std::optional<DeclaredMemberLocation> declaredMemberLocation(VM&, FunctionID key, const DeclaredMember* accessorOrNull, MemberCreation);
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
    const InstanceOfClassBelow* findInstanceOfClassBelow(const JSObject* prototype, uint32_t allocation) const;
    void rememberInstanceOfClassBelow(InstanceOfClassBelow);
    void rememberInstanceOfOtherEvaluation(VM&, const Object& instance, JSObject* prototype, uint32_t allocation);
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
    struct {
        const LayoutData* classLayout { nullptr };
        ClassSide side { ClassSide::Prototype };
        UniquedStringImpl* uid { nullptr };
    } m_memberBeingDefined;
    unsigned m_internalPutDepth { 0 };
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constructorFunctionsWithoutClass;
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
