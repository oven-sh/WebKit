/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionTables.h"
#include "AOTLayoutData.h"
#include "AOTObjectWords.h"
#include "AOTPagedBitSet.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "JSCJSValue.h"
#include "JSObject.h"
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <optional>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class GetterSetter;
class JSFinalObject;
class JSGlobalObject;
class JSObject;
class Structure;
class VM;

namespace AOT {

enum class GuardsOfNameToEnd : uint8_t;

class ChainDependentOwner;
enum class ChainDependentOwnerID : uint8_t;
class ClassPrototype;
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
    const LayoutData* classOf(const ClassPrototype&) const;
    const LayoutData* classConstructedBy(FunctionID) const;
    const LayoutData* classConstructedByClosureWithNumber(uint32_t closureNumber) const { return classConstructedBy(FunctionID(m_tables.closureFunctionWithNumber(closureNumber))); }
    const LayoutData* baseOf(const LayoutData& layout) const { return layout.base() == LayoutData::noBase ? nullptr : classWithIndex(layout.base()); }
    const LayoutData& classOfInstances(const LayoutData& instanceLayout) const
    {
        const LayoutData* current = &instanceLayout;
        while (current->isDerived && current->isClass())
            current = baseOf(*current);
        return *current;
    }
    const LayoutData* assumedBaseOf(const LayoutData& layout) const { return layout.assumedBase() == LayoutData::noBase ? nullptr : classWithIndex(layout.assumedBase()); }
    bool hasOneClassObject(const LayoutData& classLayout) const
    {
        FunctionID constructor(classLayout.classData().classConstructor);
        return constructor && classLayout.evaluation() == EvaluationState::ProvenOnce && !m_tables.captureCountsOf(constructor.index()).captures;
    }
    FunctionID functionOfOwnClosure(const Closure&) const;
    bool keepsStructuresForNextEvaluation(const Closure& classObject, const LayoutData&) const;
    static bool hasComputedMemberNames(const LayoutData& classLayout)
    {
        auto hasComputedName = [](const MethodData& method) {
            return !!(method.flags & methodHasComputedName);
        };
        return std::ranges::any_of(classLayout.methods(), hasComputedName) || std::ranges::any_of(classLayout.staticMethods(), hasComputedName);
    }
    static bool canBeDefinedFromRecord(const LayoutData& classLayout)
    {
        bool recordNamesParent = !classLayout.hasUntypedBase || classLayout.classData().builtinBase != LayoutData::noBase;
        return recordNamesParent && !hasComputedMemberNames(classLayout);
    }
    bool isDefinedByClass(const LayoutData& layout) const
    {
        if (!layout.isClass())
            return false;
        FunctionID constructor(layout.classData().classConstructor);
        return constructor && m_tables.kindOfFunction(constructor.index()).isConstructor;
    }
    static bool isClassOfCreatedObjects(const LayoutData& layout) { return layout.isClass() && !FunctionID(layout.classData().classConstructor); }
    bool hasStaticMethodNamed(const LayoutData&, UniquedStringImpl*) const;
    const FieldData* staticField(const LayoutData&, UniquedStringImpl*) const;
    JSValue readStaticField(const FieldData&) const;
    void storeStaticField(const FieldData&, JSValue);
    uint32_t& createdStaticFieldsEnd(const LayoutData&);
    void didEmptyStaticFields(const LayoutData&);

    JSObject* classPrototype(VM&, const LayoutData&);
    JSObject* classPrototypeOfInstances(VM&, const LayoutData&);
    JSObject* classPrototypeOfBase(VM&, const LayoutData&);
    void didCreatePrototypePropertyOf(Closure*);
    JSObject* classPrototypeIfExists(const LayoutData&) const;
    bool prototypeAwaitsFirstTouch(const LayoutData& classLayout) const { return m_classesWhosePrototypeAwaitsFirstTouch.get(m_tables.rankOfClass(classLayout.index)); }
    JSObject* createClassPrototype(VM&, const LayoutData&, JSObject* parent);
    void registerClassPrototype(const LayoutData&, JSObject*);
    ChainDependentOwnerID ownerOfGuardsThatHangOnChains();
    void endGuardThatHangsOnChain(uint32_t classIndex, UniquedStringImpl* uidOrNull);
    bool guardThatHangsOnChainHolds(uint32_t classIndex, UniquedStringImpl* uidOrNull) const;
    void willLinkCreatedObject(VM& vm, const LayoutData& instanceLayout, JSObject* parent)
    {
        if (isClassOfCreatedObjects(instanceLayout) && !classPrototypeIfExists(instanceLayout)) [[unlikely]]
            defineClassOfCreatedObjects(vm, instanceLayout, parent);
    }
    void defineClassOfCreatedObjects(VM&, const LayoutData&, JSObject* parent);
    bool takesObjectsLaidBelowPrototype(VM&, const LayoutData& instanceLayout, JSObject* firstAbove);
    Structure* lazyClassObjectStructureInheritingFrom(VM&, JSObject* parent);
    Structure* classObjectStructureInheritingFrom(VM&, JSObject* parent);
    size_t removeDeadEntries(VM&, CollectionScope);

    enum class ClassPart : uint8_t { Everything, ClassObject, PrototypeMembers };
    JSValue resolveHeritage(JSGlobalObject*, JSValue heritage);
    Closure* createClassObject(VM&, const LayoutData&);
    Closure* classObjectForEvaluation(Closure* allocated, const LayoutData&) const;
    void defineClass(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue parent, JSValue prototypeParent);
    void defineClass(JSGlobalObject*, Closure* classObject, JSValue parent, JSValue prototypeParent);
    void createClassPart(JSGlobalObject*, Closure* classObject, ClassPart);
    void forgetEvaluationOf(const LayoutData&);
    JSObject* createMethodClosure(VM&, Closure* classObject, const LayoutData&, const MethodData&);

    enum class HolderState : uint8_t { Unknown, AsDefinitionMadeIt, NotTouched };
    void armMemberGuards(VM&, const LayoutData&, ClassSide, JSObject* holder, HolderState = HolderState::Unknown);
    bool tryLeaveHoldersOut(VM&, const LayoutData&, Closure* classObject, JSObject* prototype);
    bool guardHasEnded(const LayoutData& classLayout, ClassSide side) const { return m_endedGuards.get(indexOfGuard(classLayout, side)); }
    bool hasNoGuardLeft(const LayoutData& classLayout, ClassSide side) const { return !classLayout.classData().hasGuardsHereOrBelow(side) || guardHasEnded(classLayout, side); }
    bool hasInstanceBelowOtherPrototype(const LayoutData& classLayout) const { return !m_classesWithInstanceBelowOtherPrototype.isEmpty() && m_classesWithInstanceBelowOtherPrototype.contains(classLayout.index); }
    bool guardsAwaitFirstInstance(const LayoutData& classLayout) const { return !m_constructorFunctionsThatAreOnlyBases.isEmpty() && m_constructorFunctionsThatAreOnlyBases.contains(classLayout.index); }
    static bool leavesDefinitionToRecord(const LayoutData& classLayout) { return classLayout.createsMembersLazily && classLayout.createsClassObjectOnMutation && classLayout.classData().guardsHoldWhileClear; }
    void disarmMemberGuards(const LayoutData&);
    void didChangeMember(const LayoutData&, ClassSide, UniquedStringImpl*);
    void holdDefinitionToNameInRecord(const LayoutData&, ClassSide, uint32_t index, UniquedStringImpl*, const JSObject* function) const;
    void didDefineMember(VM&, const LayoutData&, ClassSide, uint32_t index, JSObject* holder, UniquedStringImpl*, JSObject* function);
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
    class MethodTableUpdate {
        WTF_MAKE_NONCOPYABLE(MethodTableUpdate);
    public:
        explicit MethodTableUpdate(ClassTable& classes)
            : m_classes(classes)
        {
            ++classes.m_numberOfOpenMethodTableUpdates;
        }

        ~MethodTableUpdate();
        void willStoreEntry();

    private:
        ClassTable& m_classes;
    };
    void didChangeEveryMember(const LayoutData&, ClassSide);
    void didFreezeHolder(VM&, const LayoutData&, ClassSide, JSObject* holder);
    void didChangePrototypeLink(const LayoutData&, ClassSide);
    void didDefineWithOtherParent(const LayoutData&, ClassSide);
    void didShadowMember(const LayoutData& instanceLayout, UniquedStringImpl*);
    ALWAYS_INLINE void didShadowMember(const LayoutData& instanceLayout, const FieldData& field)
    {
        ASSERT(instanceLayout.indexOf(field) < instanceLayout.numberOfFields && &instanceLayout.fields()[instanceLayout.indexOf(field)] == &field);
        if (!creationEndsNoGuard(field)) [[unlikely]]
            didShadowMemberSlowCase(instanceLayout, field);
    }
    bool creationEndsNoGuard(const FieldData& field) const { return m_fieldsWhoseCreationEndsNoGuard.contains(numberOfFieldRecord(field)); }
    uint64_t numberOfWalksOverPrototypeHolders() const { return m_numberOfWalksOverPrototypeHolders; }
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
    std::optional<LazyPrototypeLookupResult> findThroughLazyPrototypes(VM&, JSObject* base, UniquedStringImpl*);
    std::optional<LazyPrototypeLookupResult> findFromLazyPrototype(VM&, JSObject* prototypeOrNull, UniquedStringImpl*, MemberCreation);
    Structure* structureOfDeclaredMemberHolders() const { return m_declaredMemberHolders.isEmpty() ? nullptr : m_declaredMemberHolders[0]->structure(); }

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
    JSObject* instanceWithPrototype(VM&, Object* allocated, JSObject* prototype, bool holdsNoField = false);

private:
    friend bool classGuardHolds(const State&, const LayoutData&, ClassSide);
    friend bool classGuardOfNameHolds(const State&, const LayoutData&, ClassSide, uint32_t key);
    friend void endClassGuard(State&, const LayoutData&, ClassSide);
    friend void endClassGuardsOfName(State&, const LayoutData&, ClassSide, std::optional<uint32_t> key, GuardsOfNameToEnd);

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
    void checkParentOfPrototypeObject(const LayoutData&, JSObject* prototype);
    void enterHolderWithoutGuards(VM&, const LayoutData&, ClassSide, JSObject* holder);
    bool tryLeaveDefinitionToRecord(VM&, Closure* classObject, const LayoutData&, JSValue heritage, JSValue prototypeParent);
    bool tryDefineAgainWithKeptStructures(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue heritage, JSValue prototypeParent);
    bool hasNoGuardLeft(const LayoutData& classLayout) const { return hasNoGuardLeft(classLayout, ClassSide::Prototype) && hasNoGuardLeft(classLayout, ClassSide::Static); }
    bool everyGuardHasEnded(const LayoutData& classLayout) const { return guardHasEnded(classLayout, ClassSide::Prototype) && guardHasEnded(classLayout, ClassSide::Static); }
    const LayoutData* classOfHolderLeftOut(JSObject* holder, ClassSide&) const;
    bool endGuardsOfShadowedMember(const LayoutData& instanceLayout, UniquedStringImpl*);
    void didShadowMemberSlowCase(const LayoutData& instanceLayout, const FieldData&);
    void forgetWhatCreatedFieldsHide() { m_fieldsWhoseCreationEndsNoGuard.clear(); }
    uint32_t numberOfFieldRecord(const FieldData& field) const { return m_tables.offsetOf(&field) / static_cast<uint32_t>(sizeof(FieldData)); }

    class SetOfFieldRecords {
    public:
        ALWAYS_INLINE bool contains(uint32_t number) const
        {
            uint32_t page = number >> PagedBitSet::logBitsPerPage;
            size_t group = page >> logPagesPerGroup;
            return group < m_groups.size() && m_groups[group] && ((*m_groups[group])[page % pagesPerGroup] >> number % PagedBitSet::bitsPerPage & 1);
        }
        void add(uint32_t number);
        void clear()
        {
            for (uint32_t page : m_pagesInUse)
                (*m_groups[page >> logPagesPerGroup])[page % pagesPerGroup] = 0;
            m_pagesInUse.shrink(0);
        }

    private:
        static constexpr unsigned logPagesPerGroup = 6;
        static constexpr uint32_t pagesPerGroup = 1u << logPagesPerGroup;
        using Group = std::array<uint64_t, pagesPerGroup>;

        Vector<std::unique_ptr<Group>> m_groups;
        Vector<uint32_t> m_pagesInUse;
    };

    static_assert(std::is_same_v<PropertyOffset, decltype(PlaceOfPrototypeProperty::offset)>);
    static size_t indexOfPlace(uint32_t structureID) { return structureID >> shiftOfStructureIDForPlaceOfPrototypeProperty & (numberOfPlacesOfPrototypeProperties - 1); }

    static size_t indexOfClassObject(const JSCell* classObject) { return std::bit_cast<uintptr_t>(classObject) >> shiftOfClassObjectForPrototype & (numberOfPrototypesOfClassObjects - 1); }

    size_t indexOfGuard(const LayoutData& classLayout, ClassSide side) const { return static_cast<size_t>(m_tables.rankOfClass(classLayout.index)) * numberOfClassSides + static_cast<size_t>(side); }

    std::array<PlaceOfPrototypeProperty, numberOfPlacesOfPrototypeProperties> m_placesOfPrototypeProperties { };
    std::array<PrototypeOfClassObject, numberOfPrototypesOfClassObjects> m_prototypesOfClassObjects { };
    std::array<PrivateFieldPlace, numberOfPrivateFieldPlaces> m_privateFieldPlaces { };
    static constexpr size_t numberOfClassesOfFunctionsAsked = 64;
    mutable std::array<std::atomic<uint64_t>, numberOfClassesOfFunctionsAsked> m_classesOfFunctionsAsked { };
    State& m_state;
    const ProgramTables& m_tables;
    UncheckedKeyHashMap<JSObject*, Structure*> m_classPrototypeStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_lazyClassObjectStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_classObjectStructures;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_classSidesByHolder;
    bool m_hasHoldersLeftOut { false };
    RefPtr<PrivateSymbolImpl> m_classObjectOfHolderLeftOut;
    BitVector m_endedGuards;
    BitVector m_classesWhosePrototypeAwaitsFirstTouch;
    BitVector m_sidesWhereNamesEnded;
    static constexpr uint64_t storesStillPassUnderEndedName = 1ull << 63;
    static constexpr uint64_t readsStillFindUnderEndedName = 1ull << 62;
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_endedNames;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_endedGuardsOfNames;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_classesWithInstanceBelowOtherPrototype;
    struct OwnerOfGuardsThatHangOnChains;
    std::unique_ptr<ChainDependentOwner> m_ownerOfGuardsThatHangOnChains;
    ChainDependentOwnerID m_numberOfOwnerOfGuardsThatHangOnChains { };
    SetOfFieldRecords m_fieldsWhoseCreationEndsNoGuard;
    Vector<uint32_t> m_createdStaticFieldsEnds;
    uint64_t m_numberOfWalksOverPrototypeHolders { 0 };
    unsigned m_numberOfOpenMethodTableUpdates { 0 };
    bool m_methodTableIsWritable { false };
    RememberedSlotSet m_writes;
    Vector<JSObject*> m_declaredMemberHolders;
    ByIndex m_declaredMemberLocations;
    uint32_t m_numberOfDeclaredMemberLocations { 0 };
    struct {
        const LayoutData* classLayout { nullptr };
        ClassSide side { ClassSide::Prototype };
        UniquedStringImpl* uid { nullptr };
    } m_memberBeingDefined;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constructorFunctionsWithoutClass;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constructorFunctionsThatAreOnlyBases;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
