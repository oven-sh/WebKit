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
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/UniqueArray.h>
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
    bool livesWhileReached(const LayoutData& classLayout) const
    {
        if (!classLayout.isClass() || classLayout.evaluation() != EvaluationState::PredictedOnce)
            return false;
        FunctionID constructor(classLayout.classData().classConstructor);
        return constructor && m_tables.captureCountsOf(constructor.index()).captures;
    }
    JSCell* classObjectHeldWhileReached(FunctionID constructor) const { return constructor && !m_evaluationsHeldWhileReached.isEmpty() ? m_evaluationsHeldWhileReached.get(constructor.index()).classObject : nullptr; }
    bool tryHoldClassObjectWhileReached(FunctionID, JSCell* classObject);
    bool tryHoldClassObjectWhileReached(const LayoutData& constructed, FunctionID, JSCell* classObject);
    bool isMarkOfEvaluationThatDied(const JSCell* cell) const { return cell && cell == m_markOfEvaluationThatDied; }
    template<typename Visitor> void visitRoots(Visitor&);
    static bool hasComputedMemberNames(const LayoutData& classLayout)
    {
        auto hasComputedName = [](const MethodData& method) {
            return !!(method.flags & methodHasComputedName);
        };
        return std::ranges::any_of(classLayout.methods(), hasComputedName) || std::ranges::any_of(classLayout.staticMethods(), hasComputedName);
    }
    static bool canBeDefinedFromRecord(const LayoutData& classLayout)
    {
        return !classLayout.hasUntypedBase && !hasComputedMemberNames(classLayout);
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

    Structure* structureWithBrandOfHeirs(VM&, JSCell* parent, JSCell* newTarget, Structure*);

    JSObject* classPrototype(VM&, const LayoutData&);
    JSObject* classPrototypeOfInstances(VM&, const LayoutData&);
    void didCreatePrototypePropertyOf(Closure*);
    JSObject* classPrototypeIfExists(const LayoutData&) const;
    bool structureTellsInstancesOf(const LayoutData&, JSCell*, std::span<int32_t> displacementsOfFields) const;
    bool rememberStructureOfInstances(VM&, JSCell*, const LayoutData&, uint32_t offsetOfStructures);
    void forgetStructuresOfHeirs();
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
    bool accessorAboveCreatedObjectsIsWhatRecordNames(VM&, const LayoutData&, Structure*, uint32_t name);
    bool takesObjectsLaidBelowPrototype(VM&, const LayoutData& instanceLayout, JSObject* firstAbove);
    Structure* lazyClassObjectStructureInheritingFrom(VM&, JSObject* parent);
    Structure* classObjectStructureInheritingFrom(VM&, JSObject* parent);
    size_t removeDeadEntries(VM&, CollectionScope);

    enum class ClassPart : uint8_t { Everything, ClassObject, PrototypeMembers };
    JSValue resolveHeritage(JSGlobalObject*, JSValue heritage);
    Closure* createClassObject(VM&, const LayoutData&);
    Closure* defineClassWhereItIsEvaluated(JSGlobalObject*, Closure* allocated, const LayoutData&, JSValue parent, JSValue prototypeParent);
    void defineClass(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue parent, JSValue prototypeParent);
    void defineClass(JSGlobalObject*, Closure* classObject, JSValue parent, JSValue prototypeParent);
    void createClassPart(JSGlobalObject*, Closure* classObject, ClassPart);
    void forgetEvaluationOf(const LayoutData&);
    JSObject* createMethodClosure(VM&, Closure* classObject, const LayoutData&, const MethodData&);

    enum class HolderState : uint8_t { Unknown, AsDefinitionMadeIt, NotTouched };
    void armMemberGuards(VM&, const LayoutData&, ClassSide, JSObject* holder, HolderState = HolderState::Unknown);
    bool holdersAreLeftOut(const LayoutData&);
    bool tryLeaveHoldersOut(VM&, const LayoutData&, Closure* classObject, JSObject* prototype);
    void nameClassObjectOnPrototypeMadeLeftOut(VM&, const LayoutData&, Closure* classObject, JSObject* prototype);
    void didLeaveOutHoldersOfEvaluation(const LayoutData& classLayout, JSObject* prototype) { m_lastEvaluation = { prototype, &classLayout, true }; }
    bool takeWordThatNothingHasAskedBelow(JSValue prototype)
    {
        if (!m_lastEvaluation.nothingHasAskedBelowPrototype || prototype != JSValue(m_lastEvaluation.prototype))
            return false;
        m_lastEvaluation.nothingHasAskedBelowPrototype = false;
        return true;
    }
    bool guardHasEnded(const LayoutData& classLayout, ClassSide side) const { return m_endedGuards.get(indexOfGuard(classLayout, side)); }
    bool hasNoGuardLeft(const LayoutData& classLayout, ClassSide side) const { return !classLayout.classData().hasGuardsHereOrBelow(side) || guardHasEnded(classLayout, side); }
    bool hasInstanceBelowOtherPrototype(const LayoutData& classLayout) const { return !m_classesWithInstanceBelowOtherPrototype.isEmpty() && m_classesWithInstanceBelowOtherPrototype.contains(classLayout.index); }
    bool guardsHaveBegun(const LayoutData& classLayout) const { return m_classesWhoseGuardsHaveBegun.get(m_tables.rankOfClass(classLayout.index)); }
    bool countsGuardsOfNamesBehindArming(const LayoutData& classLayout) const { return m_classesThatCountGuardsOfNamesBehindArming.get(m_tables.rankOfClass(classLayout.index)); }
    static bool leavesDefinitionToRecord(const LayoutData& classLayout) { return classLayout.createsMembersLazily && classLayout.createsClassObjectOnMutation && classLayout.classData().guardsHoldWhileClear; }
    void disarmMemberGuards(const LayoutData&);
    bool addsStaticUnobserved(VM&, const Closure& classObject, const LayoutData&, Structure* before, UniquedStringImpl*) const;
    bool sharesStructureOfRootClasses(Structure*) const;
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
    class DefinitionFromRecord {
        WTF_MAKE_NONCOPYABLE(DefinitionFromRecord);
    public:
        DefinitionFromRecord(ClassTable& classes, const LayoutData& classLayout)
            : m_classes(classes)
            , m_outer(std::exchange(classes.m_classDefinedFromRecord, &classLayout))
        {
        }

        ~DefinitionFromRecord() { m_classes.m_classDefinedFromRecord = m_outer; }

    private:
        ClassTable& m_classes;
        const LayoutData* m_outer;
    };
    bool nobodyHearsOfValueUnder(const LayoutData&, ClassSide, UniquedStringImpl*) const;
    void holdMemberToProof(VM&, const LayoutData&, ClassSide, JSObject* holder, UniquedStringImpl*);
    void holdLinkToProof(VM&, const LayoutData&, ClassSide, JSObject* holder);
    class MethodTableUpdate {
        WTF_MAKE_NONCOPYABLE(MethodTableUpdate);
    public:
        explicit MethodTableUpdate(ClassTable& classes)
            : m_classes(classes)
        {
            ++classes.m_numberOfOpenMethodTableUpdates;
        }

        ~MethodTableUpdate();
        void willStoreEntry(size_t index);

    private:
        ClassTable& m_classes;
    };
    void didChangeEveryMember(const LayoutData&, ClassSide);
    void didFreezeHolder(VM&, const LayoutData&, ClassSide, JSObject* holder);
    void didLayWhatSeesAccesses(VM&, const LayoutData&, ClassSide);
    static bool seesAccesses(VM&, UniquedStringImpl*, unsigned attributes);
    bool holdsMemberOfRecordUnder(VM&, const LayoutData&, ClassSide, JSObject* holder, UniquedStringImpl*) const;
    void beginGuardsAtEndOfModule(VM&, const LayoutData&);
    void beginGuardsAheadOfFirstObject(VM&, const LayoutData&);
    void didRegisterClosure(FunctionID function)
    {
        if (!m_classesThatWaitForFunctions.isEmpty()) [[unlikely]]
            letGuardsBeAskedForAgain(function);
    }
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
    const uint64_t* wordsOfObjectWithoutFields(const LayoutData& layout) const
    {
        auto found = m_wordsOfObjectsWithoutFields.find(layout.index);
        return found == m_wordsOfObjectsWithoutFields.end() ? nullptr : found->value.get();
    }
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
    ALWAYS_INLINE Structure* structureOfThisRememberedBesideCallee(JSCell* callee) const
    {
        const ThisBesideCallee& remembered = m_thisBesideCallees[indexOfCalleeWithThisBeside(callee)];
        if (remembered.callee != callee)
            return nullptr;
        JSObject* prototype = tryReadPrototypePropertyAtRememberedPlace(callee);
        return prototype && remembered.structure->storedPrototype() == prototype ? remembered.structure : nullptr;
    }
    void rememberPlaceOfPrototypeProperty(VM&, Closure* function, JSObject* prototype);
    JSObject* prototypeOfClassObject(VM&, Closure* classObject);
    JSObject* instanceWithPrototype(VM&, Object* allocated, JSObject* prototype, bool holdsNoField = false);
    void rememberInstanceBelow(VM&, uint32_t allocationOfSite, JSObject* prototype, const Object& instanceWithoutFields, size_t size);
    void forgetInstancesBelowPrototypes();

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
    Object* createWithoutFields(VM&, Structure*, const LayoutData&);
    void keepWordsOfObjectWithoutFields(const LayoutData&, const Object& born);
    static JSFinalObject* createOrdinaryInstance(JSGlobalObject*, unsigned numberOfProperties, JSObject* prototype);
    const LayoutData* classWithPrototype(JSObject* prototype) const;
    JSObject* defineClassOfConstructorFunction(VM&, const LayoutData&);
    void beginGuards(const LayoutData& classLayout)
    {
        if (!guardsHaveBegun(classLayout)) [[unlikely]]
            beginGuardsSlowCase(classLayout);
    }
    void beginGuardsSlowCase(const LayoutData&);
    void countGuardsOfNamesThatDoNotHold(const LayoutData&);
    void checkParentOfPrototypeObject(const LayoutData&, JSObject* prototype);
    bool tryLeaveDefinitionToRecord(VM&, Closure* classObject, const LayoutData&, JSValue heritage, JSValue prototypeParent, FunctionID constructor, JSCell* registered);
    void defineClass(JSGlobalObject*, Closure* classObject, const LayoutData&, JSValue parent, JSValue prototypeParent, FunctionID constructor, JSCell* registered);
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
    static size_t indexOfPlace(uint32_t structureID) { return indexOfPlaceOfPrototypeProperty(structureID); }

    static size_t indexOfClassObject(const JSCell* classObject) { return std::bit_cast<uintptr_t>(classObject) >> shiftOfClassObjectForPrototype & (numberOfPrototypesOfClassObjects - 1); }

    size_t indexOfGuard(const LayoutData& classLayout, ClassSide side) const { return static_cast<size_t>(m_tables.rankOfClass(classLayout.index)) * numberOfClassSides + static_cast<size_t>(side); }

    std::array<PlaceOfPrototypeProperty, numberOfPlacesOfPrototypeProperties> m_placesOfPrototypeProperties { };
    std::array<PrototypeOfClassObject, numberOfPrototypesOfClassObjects> m_prototypesOfClassObjects { };
    struct ThisBesideCallee {
        JSCell* callee;
        Structure* structure;
    };
    static constexpr size_t numberOfCalleesWithThisBeside = 64;
    static size_t indexOfCalleeWithThisBeside(const JSCell* callee) { return std::bit_cast<uintptr_t>(callee) >> shiftOfClassObjectForPrototype & (numberOfCalleesWithThisBeside - 1); }
    void rememberThisBesideCallee(VM&, JSObject* callee, Structure* structureOfThis);
    std::array<ThisBesideCallee, numberOfCalleesWithThisBeside> m_thisBesideCallees { };
    UncheckedKeyHashMap<uint32_t, UniqueArray<uint64_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_wordsOfObjectsWithoutFields;
    std::array<PrivateFieldPlace, numberOfPrivateFieldPlaces> m_privateFieldPlaces { };
    Vector<InstanceBelowPrototype> m_instancesBelowPrototypes;
    size_t m_numberOfInstancesBelowPrototypes { 0 };
    Vector<Vector<uint64_t>> m_wordsOfInstancesWithoutFields;
    static constexpr size_t numberOfClassesOfFunctionsAsked = 64;
    mutable std::array<std::atomic<uint64_t>, numberOfClassesOfFunctionsAsked> m_classesOfFunctionsAsked { };
    State& m_state;
    const ProgramTables& m_tables;
    UncheckedKeyHashMap<JSObject*, Structure*> m_classPrototypeStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_lazyClassObjectStructures;
    UncheckedKeyHashMap<JSObject*, Structure*> m_classObjectStructures;
    UncheckedKeyHashMap<JSObject*, uint32_t> m_classSidesByHolder;
    bool m_hasHoldersLeftOut { false };
    struct LastEvaluation {
        JSObject* prototype;
        const LayoutData* classLayout;
        bool nothingHasAskedBelowPrototype;
    };
    LastEvaluation m_lastEvaluation { };

    Symbol* brandOfHeirsFor(VM&, JSCell* parent, JSCell* newTarget);
    struct StructureWithBrandOfHeirs {
        JSCell* parent;
        JSCell* newTarget;
        Structure* withoutBrand;
        Structure* withBrand;
    };
    static constexpr size_t numberOfStructuresWithBrandsOfHeirs = 16;
    static size_t indexOfStructureWithBrandOfHeirs(const JSCell* newTarget) { return std::bit_cast<uintptr_t>(newTarget) >> 4 & (numberOfStructuresWithBrandsOfHeirs - 1); }
    std::array<StructureWithBrandOfHeirs, numberOfStructuresWithBrandsOfHeirs> m_structuresWithBrandsOfHeirs { };
    Vector<Symbol*> m_brandsOfHeirs;
    RefPtr<PrivateSymbolImpl> m_classObjectOfHolderLeftOut;
    BitVector m_endedGuards;
    BitVector m_classesWhoseGuardsHaveBegun;
    bool m_beginsGuardsOfFunctionsNotMadeYet { false };
    HashMap<uint32_t, Vector<const LayoutData*, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_classesThatWaitForFunctions;
    void letGuardsBeAskedForAgain(FunctionID);
    BitVector m_classesThatCountGuardsOfNamesBehindArming;
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
    Vector<uint32_t> m_offsetsOfStructuresOfHeirsInUse;
    struct VerdictOnStructureOfHeir {
        static constexpr uint32_t passesBit = 1;
        static_assert(alignof(StructuresOfHeir) > passesBit);

        bool speaksOf(uint32_t askedStructureID, uint32_t offsetOfStructures) const { return structureID == askedStructureID && (offsetOfStructuresAndVerdict & ~passesBit) == offsetOfStructures; }
        bool passes() const { return offsetOfStructuresAndVerdict & passesBit; }

        uint32_t structureID;
        uint32_t offsetOfStructuresAndVerdict;
    };
    static constexpr unsigned logNumberOfVerdictsOnStructuresOfHeirs = 6;
    static size_t indexOfVerdictOnStructureOfHeir(uint32_t structureID, uint32_t offsetOfStructures) { return ((structureID ^ offsetOfStructures) * multiplierOfPlaceOfPrototypeProperty) >> (32 - logNumberOfVerdictsOnStructuresOfHeirs); }
    std::array<VerdictOnStructureOfHeir, 1u << logNumberOfVerdictsOnStructuresOfHeirs> m_newestVerdictsOnStructuresOfHeirs { };
    struct HashOfVerdictOnStructureOfHeir {
        static unsigned hash(uint64_t verdict) { return DefaultHash<uint64_t>::hash(verdict & ~static_cast<uint64_t>(VerdictOnStructureOfHeir::passesBit)); }
        static bool equal(uint64_t first, uint64_t second) { return !((first ^ second) & ~static_cast<uint64_t>(VerdictOnStructureOfHeir::passesBit)); }
        static constexpr bool safeToCompareToEmptyOrDeleted = true;
    };
    UncheckedKeyHashSet<uint64_t, HashOfVerdictOnStructureOfHeir> m_verdictsOnStructuresOfHeirs;
    struct MembersAboveHeirs {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(MembersAboveHeirs);
        bool canBeToldByStructure { true };
        Vector<RefPtr<UniquedStringImpl>> names;
    };
    mutable UncheckedKeyHashMap<uint32_t, std::unique_ptr<MembersAboveHeirs>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_membersAboveHeirs;
    uint64_t m_numberOfWalksOverPrototypeHolders { 0 };
    unsigned m_numberOfOpenMethodTableUpdates { 0 };
    BitVector m_writablePagesOfMethodTable;
    RememberedSlotSet m_writes;
    Vector<JSObject*> m_declaredMemberHolders;
    ByIndex m_declaredMemberLocations;
    uint32_t m_numberOfDeclaredMemberLocations { 0 };
    struct {
        const LayoutData* classLayout { nullptr };
        ClassSide side { ClassSide::Prototype };
        UniquedStringImpl* uid { nullptr };
    } m_memberBeingDefined;
    const LayoutData* m_classWhoseModuleEnds { nullptr };
    std::optional<BitVector> m_classesAtOrAboveGuardOfWordOnWhatSeesAccesses;
    std::optional<uint32_t> m_numberOfWordOnWhatSeesAccesses;
    const LayoutData* m_classDefinedFromRecord { nullptr };
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constructorFunctionsWithoutClass;
    void rememberClassConstructedBy(FunctionID, const LayoutData&) const;
    void endEvaluationsThatDied(VM&, CollectionScope);
    struct EvaluationHeldWhileReached {
        JSCell* classObject { nullptr };
        uint32_t rank { 0 };
    };
    UncheckedKeyHashMap<uint32_t, EvaluationHeldWhileReached, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_evaluationsHeldWhileReached;
    Vector<uint32_t> m_constructorsOfEvaluationsChangedSinceCollection;
    JSObject* m_markOfEvaluationThatDied { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
