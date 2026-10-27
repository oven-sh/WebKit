/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTChainDependents.h"
#include "AOTClassMemberTable.h"
#include "AOTClassPrototype.h"
#include "AOTClassStructureCache.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTEnvironment.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionNames.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTMemberGuards.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "CustomGetterSetter.h"
#include "DeferTermination.h"
#include "FunctionPrototype.h"
#include "FunctionRareData.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include <bit>
#include <ranges>
#include <wtf/ForbidHeapAllocation.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static const LayoutData* findClassConstructedBy(const ProgramTables& tables, FunctionID function)
{
    auto constructors = tables.constructors();
    auto found = std::ranges::lower_bound(constructors, function.index(), { }, &ConstructorData::function);
    if (found == constructors.end() || found->function != function.index())
        return nullptr;
    uint32_t offset = tables.offsetOfClass(found->layoutIndex);
    return offset ? tables.at<LayoutData>(offset) : nullptr;
}

static std::optional<uint32_t> keyOf(VM& vm, UniquedStringImpl* uid)
{
    return vm.aotState().identifiers->numberOf(uid);
}

static JSCell*& prototypeCellOf(State& state, const LayoutData& classLayout)
{
    RELEASE_ASSERT(state.tables.offsetOfClass(classLayout.index));
    return state.instance.classes.classPrototypes[state.tables.rankOfClass(classLayout.index)];
}

ClassTable::ClassTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
{
    state.instance.classes.classPrototypes = state.tableArena.allocate<JSCell*>(m_tables.numberOfClasses());
    state.instance.classes.placesOfPrototypeProperties = m_placesOfPrototypeProperties.data();
    state.instance.classes.prototypesOfClassObjects = m_prototypesOfClassObjects.data();
    state.instance.classes.instancesOfClassesBelow = m_instancesOfClassesBelow.data();
    state.caches->add(CacheKind::WeakByKey, "class sides by holder"_s, *this);
    state.caches->addClearedEachCollection("places of prototype properties"_s, asWritableBytes(std::span { m_placesOfPrototypeProperties }));
    state.caches->addClearedEachCollection("prototypes of class objects"_s, asWritableBytes(std::span { m_prototypesOfClassObjects }));
    state.caches->addClearedEachCollection("instances of classes below"_s, asWritableBytes(std::span { m_instancesOfClassesBelow }));
}

ClassTable::~ClassTable() = default;

const LayoutData* ClassTable::classWithIndex(uint32_t index) const
{
    uint32_t offset = m_tables.offsetOfClass(index);
    return offset ? m_tables.at<LayoutData>(offset) : nullptr;
}

const LayoutData* ClassTable::classConstructedBy(FunctionID function) const
{
    auto& recentLookup = m_recentClassLookups[function.index() % m_recentClassLookups.size()];
    uint64_t known = recentLookup.load(std::memory_order_relaxed);
    if (known >> 32 == static_cast<uint64_t>(function.index()) + 1) [[likely]]
        return static_cast<uint32_t>(known) ? m_tables.at<LayoutData>(static_cast<uint32_t>(known)) : nullptr;
    const LayoutData* found = findClassConstructedBy(m_tables, function);
    uint64_t offset = found ? reinterpret_cast<const uint8_t*>(found) - m_tables.at<uint8_t>(0) : 0;
    if (offset <= UINT32_MAX)
        recentLookup.store((static_cast<uint64_t>(function.index()) + 1) << 32 | offset, std::memory_order_relaxed);
    return found;
}

const FieldData* ClassTable::staticField(const LayoutData& classLayout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> key = keyOf(*m_state.instance.vm, name);
    if (!key)
        return nullptr;
    for (const LayoutData* declaring = &classLayout; declaring; declaring = baseOf(*declaring)) {
        for (auto& field : declaring->staticFields()) {
            if (field.name == *key)
                return &field;
        }
    }
    return nullptr;
}

JSValue ClassTable::readStaticField(const FieldData& field) const
{
    return ProgramStatics::readVariable(m_state.instance, field.slot);
}

bool ClassTable::tryStoreStaticField(VM&, const FieldData& field, JSValue value)
{
    if (!m_state.objects->layouts.fieldAccepts(field, value))
        return false;
    ProgramStatics::writeVariable(m_state.instance, field.slot, value);
    return true;
}

Structure* ClassTable::lazyClassObjectStructure(VM& vm, const LayoutData& classLayout)
{
    const LayoutData* baseClass = baseOf(classLayout);
    ClosureTable& closures = m_state.objects->closures;
    if (!baseClass)
        return closures.closureStructure(vm, classObjectClosureStructure);
    return lazyClassObjectStructureInheritingFrom(vm, uncheckedDowncast<Closure>(closures.closure(vm, FunctionID(baseClass->classData().classConstructor))));
}

Structure* ClassTable::lazyClassObjectStructureInheritingFrom(VM& vm, JSObject* parent)
{
    Structure* structure = m_lazyClassObjectStructures.get(parent);
    if (!structure) {
        structure = Closure::createStructure(vm, m_state.instance.globalObject, parent);
        m_lazyClassObjectStructures.set(parent, structure);
        m_state.immortalCells->add(structure);
    }
    return structure;
}

Structure* ClassTable::classObjectStructureInheritingFrom(VM& vm, JSObject* parent)
{
    Structure* structure = m_classObjectStructures.get(parent);
    if (!structure) {
        bool hasNoParentClass = parent == m_state.instance.globalObject->functionPrototype();
        Structure* withoutProperties = hasNoParentClass ? m_state.objects->closures.closureStructure(vm, classObjectClosureStructure) : lazyClassObjectStructureInheritingFrom(vm, parent);
        PropertyOffset offset;
        structure = Structure::addPropertyTransition(vm, withoutProperties, vm.propertyNames->prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly, offset);
        m_classObjectStructures.set(parent, structure);
        m_state.immortalCells->add(structure);
    }
    return structure;
}

Closure* ClassTable::createClassObject(VM& vm, const LayoutData& classLayout)
{
    ClosureTable& closures = m_state.objects->closures;
    FunctionID constructor(classLayout.classData().classConstructor);
    bool isCreatedOnChange = classLayout.createsClassObjectOnMutation && classLayout.createsPrototypeBeforeClassObject;
    Structure* structure = isCreatedOnChange ? lazyClassObjectStructure(vm, classLayout) : closures.closureStructureFor(vm, constructor);
    RELEASE_ASSERT(!closures.closureIfCreated(constructor));
    Closure* created = Closure::create(vm, structure, constructor);
    closures.registerClassObject(created);
    bool recordNamesParent = !classLayout.hasUntypedBase || classLayout.classData().builtinBase != LayoutData::noBase;
    if (!recordNamesParent || hasComputedMemberNames(classLayout))
        return created;
    ASSERT(hasOneClassObject(classLayout));
    count(m_state.instance, RuntimeCounterID::ClassesMadeWhenFirstTouched);
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    defineClass(m_state.instance.globalObject, created, classLayout, JSValue(), JSValue());
    scope.assertNoException();
    return created;
}

bool ClassTable::hasStaticMethodNamed(const LayoutData& classLayout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> key = keyOf(*m_state.instance.vm, name);
    if (!key)
        return false;
    return std::ranges::any_of(classLayout.staticMethods(), [&](auto& method) {
        return !(method.flags & methodHasComputedName) && method.name == *key;
    });
}

JSObject* ClassTable::createClassPrototype(VM& vm, const LayoutData& classLayout, JSObject* parent)
{
    Structure* structure = m_classPrototypeStructures.get(parent);
    if (!structure) {
        structure = ClassPrototype::createStructure(vm, m_state.instance.globalObject, parent);
        m_classPrototypeStructures.set(parent, structure);
        m_state.immortalCells->add(structure);
    }
    uint32_t memberRow = hasOneClassObject(classLayout) ? m_tables.classMemberRows()[m_tables.rankOfClass(classLayout.index)] : ClassRowEntry::noRow;
    JSObject* prototype = ClassPrototype::create(vm, structure, classLayout.index, memberRow);
    registerClassPrototype(classLayout, prototype);
    return prototype;
}

JSObject* ClassTable::classPrototypeIfExists(const LayoutData& classLayout) const
{
    return static_cast<JSObject*>(prototypeCellOf(m_state, classLayout));
}

JSObject* ClassTable::classPrototypeOfInstances(VM& vm, const LayoutData& classLayout)
{
    JSObject* prototype = classPrototype(vm, classLayout);
    if (!m_constructorFunctionsThatAreOnlyBases.isEmpty() && m_constructorFunctionsThatAreOnlyBases.remove(classLayout.index)) [[unlikely]] {
        MemberGuardWatchpoints::MethodTableUpdate update(m_state);
        armGuardsOfConstructorFunction(vm, classLayout, prototype, uncheckedDowncast<Closure>(m_state.objects->closures.closureIfCreated(FunctionID(classLayout.classData().classConstructor))));
    }
    return prototype;
}

JSObject* ClassTable::classPrototypeOfBase(VM& vm, const LayoutData& classLayout)
{
    if (isDefinedByClass(classLayout) || classPrototypeIfExists(classLayout) || isClassOfCreatedObjects(classLayout))
        return classPrototype(vm, classLayout);
    if (classLayout.evaluation() == EvaluationState::SeenTwice)
        return nullptr;
    if (classLayout.evaluation() == EvaluationState::ProvenOnce && hasOneClassObject(classLayout))
        m_state.objects->closures.closure(vm, FunctionID(classLayout.classData().classConstructor));
    return defineClassOfConstructorFunction(vm, classLayout, UseOfFunction::IsBase);
}

JSObject* ClassTable::classPrototype(VM& vm, const LayoutData& classLayout)
{
    if (JSObject* prototype = classPrototypeIfExists(classLayout))
        return prototype;
    if (isClassOfCreatedObjects(classLayout))
        return nullptr;
    if (classLayout.evaluation() == EvaluationState::PredictedOnce) {
        if (!isDefinedByClass(classLayout))
            return defineClassOfConstructorFunction(vm, classLayout, UseOfFunction::MakesObjects);
        FunctionID constructor(classLayout.classData().classConstructor);
        JSCell* registered = constructor && functionKindData(constructor).isConstructor ? m_state.objects->closures.closureIfCreated(constructor) : nullptr;
        JSValue prototype = registered ? asObject(registered)->getDirect(vm, vm.propertyNames->prototype) : JSValue();
        return prototype && prototype.isObject() ? asObject(prototype) : nullptr;
    }
    if (classLayout.evaluation() != EvaluationState::ProvenOnce)
        return nullptr;
    if (classLayout.createsPrototypeBeforeClassObject) {
        const LayoutData* baseClass = baseOf(classLayout);
        if (baseClass && !isDefinedByClass(*baseClass))
            return nullptr;
        JSObject* parent = baseClass ? classPrototype(vm, *baseClass) : m_state.instance.globalObject->objectPrototype();
        if (!parent)
            return nullptr;
        JSObject* prototype = createClassPrototype(vm, classLayout, parent);
        if (leavesDefinitionToRecord(classLayout))
            didCreatePrototypeLeftToRecord(classLayout);
        return prototype;
    }
    if (hasOneClassObject(classLayout))
        m_state.objects->closures.closure(vm, FunctionID(classLayout.classData().classConstructor));
    if (!isDefinedByClass(classLayout))
        return defineClassOfConstructorFunction(vm, classLayout, UseOfFunction::MakesObjects);
    return classPrototypeIfExists(classLayout);
}

void ClassTable::didCreatePrototypePropertyOf(Closure* function)
{
    const LayoutData* classLayout = classConstructedBy(function->function());
    if (!classLayout || isDefinedByClass(*classLayout) || classLayout->evaluation() != EvaluationState::PredictedOnce)
        return;
    ClosureTable& closures = m_state.objects->closures;
    if (function->function() != FunctionID(classLayout->classData().classConstructor) || !closures.hasCellFor(function->function()))
        return;
    JSCell* registered = closures.closureIfCreated(function->function());
    if (!registered || registered == function) {
        closures.registerClassObject(function);
        return;
    }
    m_constructorFunctionsWithoutClass.add(classLayout->index);
    didEvaluateClassAgain(m_state, *classLayout);
}

JSObject* ClassTable::defineClassOfConstructorFunction(VM& vm, const LayoutData& classLayout, UseOfFunction use)
{
    if (m_constructorFunctionsWithoutClass.contains(classLayout.index))
        return nullptr;
    auto* classObject = dynamicDowncast<Closure>(m_state.objects->closures.closureIfCreated(FunctionID(classLayout.classData().classConstructor)));
    if (!classObject)
        return nullptr;
    JSValue stored = classObject->prototypeProperty(vm);
    JSObject* prototype = stored && stored.isObject() ? asObject(stored) : nullptr;
    ClassSide sideOfOtherClass;
    if (!prototype || (prototype->type() != FinalObjectType && !isCompiledObject(prototype)) || classHeldBy(prototype, sideOfOtherClass)) {
        m_constructorFunctionsWithoutClass.add(classLayout.index);
        disarmMemberGuards(classLayout);
        return nullptr;
    }

    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    registerClassPrototype(classLayout, prototype);
    prototype->structure()->dropTransitionChain(vm);
    if (const LayoutData* base = assumedBaseOf(classLayout))
        classPrototypeOfBase(vm, *base);
    if (use == UseOfFunction::MakesObjects) {
        armGuardsOfConstructorFunction(vm, classLayout, prototype, classObject);
        return prototype;
    }
    m_constructorFunctionsThatAreOnlyBases.add(classLayout.index);
    enterHolderWithoutGuards(vm, classLayout, ClassSide::Prototype, prototype);
    enterHolderWithoutGuards(vm, classLayout, ClassSide::Static, classObject);
    checkParentsOfConstructorFunction(classLayout, prototype, classObject);
    return prototype;
}

bool ClassTable::awaitsPrototypeOfCreatedObjects(const LayoutData& layout) const
{
    const LayoutData& classLayout = classOfInstances(layout);
    return isClassOfCreatedObjects(classLayout) && !classPrototypeIfExists(classLayout) && !m_constructorFunctionsWithoutClass.contains(classLayout.index);
}

bool ClassTable::defineClassOfCreatedObjects(VM& vm, const LayoutData& classLayout, JSObject* prototype)
{
    if (!awaitsPrototypeOfCreatedObjects(classLayout))
        return false;
    ClassSide sideOfOtherClass;
    if (classLayout.evaluation() != EvaluationState::ProvenOnce || (prototype->type() != FinalObjectType && !isCompiledObject(prototype)) || classHeldBy(prototype, sideOfOtherClass)) {
        m_constructorFunctionsWithoutClass.add(classLayout.index);
        disarmMemberGuards(classLayout);
        return false;
    }

    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    registerClassPrototype(classLayout, prototype);
    const LayoutData* base = assumedBaseOf(classLayout);
    JSValue parent = prototype->getPrototypeDirect();
    if (base && parent.isObject())
        defineClassOfCreatedObjects(vm, *base, asObject(parent));
    armMemberGuards(vm, classLayout, ClassSide::Prototype, prototype);
    JSObject* assumedParent = base ? classPrototypeIfExists(*base) : m_state.instance.globalObject->objectPrototype();
    if (!assumedParent || parent != JSValue(assumedParent))
        didDefineWithOtherParent(classLayout, ClassSide::Prototype);
    return true;
}

void ClassTable::checkParentsOfConstructorFunction(const LayoutData& classLayout, JSObject* prototype, Closure* classObject)
{
    const LayoutData* base = assumedBaseOf(classLayout);
    JSObject* assumedParent = base ? classPrototypeIfExists(*base) : m_state.instance.globalObject->objectPrototype();
    if (!assumedParent || prototype->getPrototypeDirect() != JSValue(assumedParent))
        didDefineWithOtherParent(classLayout, ClassSide::Prototype);
    if (classObject->getPrototypeDirect() != JSValue(m_state.instance.globalObject->functionPrototype()))
        didDefineWithOtherParent(classLayout, ClassSide::Static);
}

void ClassTable::armGuardsOfConstructorFunction(VM& vm, const LayoutData& classLayout, JSObject* prototype, Closure* classObject)
{
    armMemberGuards(vm, classLayout, ClassSide::Prototype, prototype);
    armMemberGuards(vm, classLayout, ClassSide::Static, classObject);
    checkParentsOfConstructorFunction(classLayout, prototype, classObject);
}

void ClassTable::registerClassPrototype(const LayoutData& classLayout, JSObject* prototype)
{
    ASSERT((classLayout.evaluation() == EvaluationState::ProvenOnce || !isDefinedByClass(classLayout)) && !classPrototypeIfExists(classLayout));
    prototypeCellOf(m_state, classLayout) = prototype;
    m_state.immortalCells->add(prototype);
    if (!classLayout.classData().guardsHoldWhileClear) {
        MemberGuardWatchpoints::MethodTableUpdate update(m_state);
        m_state.objects->caches->memberGuardWatchpoints.didCreateHolder(classLayout, ClassSide::Prototype);
    }
}

bool ClassTable::keepsStructuresForNextEvaluation(const Closure& classObject, const LayoutData& classLayout) const
{
    ASSERT(&classLayout == classObject.classData());
    if (classLayout.evaluation() == EvaluationState::ProvenOnce)
        return false;
    if (m_state.objects->closures.closureIfCreated(classObject.function()) != &classObject)
        return true;
    auto isCreatedAtDefinition = [](const MethodData& method) {
        return !!(method.flags & methodIsCreatedAtDefinition);
    };
    return std::ranges::none_of(classLayout.methods(), isCreatedAtDefinition) && std::ranges::none_of(classLayout.staticMethods(), isCreatedAtDefinition);
}

size_t ClassTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t sizeBefore = m_classSidesByHolder.size();
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_classSidesByHolder.removeIf([&](auto& entry) {
            return !vm.heap.isMarked(entry.key);
        });
    });
    return sizeBefore - m_classSidesByHolder.size();
}

static JSObject* closureObject(VM& vm, State& state, FunctionID function)
{
    return asObject(state.objects->closures.closure(vm, function));
}

static GetterSetter* createGetterSetter(VM& vm, State& state, const ClassTable::DeclaredMember& accessor)
{
    JSObject* getter = accessor.getter ? closureObject(vm, state, FunctionID(accessor.getter->function)) : nullptr;
    JSObject* setter = accessor.setter ? closureObject(vm, state, FunctionID(accessor.setter->function)) : nullptr;
    return GetterSetter::create(vm, state.instance.globalObject, getter, setter);
}

void ClassTable::DeclaredMember::define(const MethodData& definition)
{
    switch (definition.kind()) {
    case MethodKind::Method:
        *this = DeclaredMember { &definition };
        break;
    case MethodKind::Getter:
        method = nullptr;
        getter = &definition;
        break;
    case MethodKind::Setter:
        method = nullptr;
        setter = &definition;
        break;
    }
}

static std::optional<ClassTable::DeclaredMember> findOwnDeclaredMember(const LayoutData& classLayout, std::optional<uint32_t> key)
{
    if (!ClassTable::hasOneClassObject(classLayout))
        return std::nullopt;
    ClassTable::DeclaredMember member;
    for (auto& method : classLayout.methods()) {
        if (method.flags & (methodHasComputedName | methodIsCreatedAtDefinition))
            return std::nullopt;
        if (key && method.name == *key)
            member.define(method);
    }
    return member;
}

bool ClassTable::declaresOwnMember(const LayoutData& classLayout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> key = keyOf(*m_state.instance.vm, name);
    return key && declaresMember(classLayout, ClassSide::Prototype, *key);
}

std::optional<JSObject*> ClassTable::findDeclaredMethod(VM& vm, const LayoutData& classLayout, UniquedStringImpl* name)
{
    auto member = findOwnDeclaredMember(classLayout, keyOf(vm, name));
    if (!member || member->isAccessor())
        return std::nullopt;
    if (!member->method && name != vm.propertyNames->constructor.impl())
        return static_cast<JSObject*>(nullptr);
    return closureObject(vm, m_state, FunctionID(member->method ? member->method->function : classLayout.classData().classConstructor));
}

static FunctionID keyOfAccessor(const ClassTable::DeclaredMember& accessor)
{
    ASSERT(accessor.isAccessor());
    return FunctionID((accessor.getter ? accessor.getter : accessor.setter)->function);
}

std::optional<ClassTable::LazyPrototypeLookupResult> ClassTable::findThroughLazyPrototypes(VM& vm, JSObject* base, UniquedStringImpl* name, bool isAbsentFromBase, MemberCreation creation)
{
    auto resultAt = [](std::optional<DeclaredMemberLocation> location) -> std::optional<LazyPrototypeLookupResult> {
        if (!location)
            return std::nullopt;
        return LazyPrototypeLookupResult { location, nullptr };
    };
    Structure* structure = base->structure();
    JSObject* prototype = structure->hasMonoProto() ? structure->storedPrototypeObject() : nullptr;
    if (!prototype || (prototype->classInfo() != ClassPrototype::info() && !isAbsentFromBase) || name == vm.propertyNames->underscoreProto.impl())
        return std::nullopt;
    bool structureGuaranteesAbsent = isAbsentFromBase ? true : compiledLayoutOrNull(structure) ? Object::structureGuaranteesAbsence(structure, name)
        : base->type() == FinalObjectType && !structure->isDictionary() && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence() && !structure->typeInfo().overridesGetOwnPropertySlot() && !isValidOffset(structure->get(vm, name));
    if (!structureGuaranteesAbsent)
        return std::nullopt;
    for (; prototype && prototype->classInfo() == ClassPrototype::info() && Instance::of(prototype->structure()->realm()) == &m_state.instance && !prototype->butterfly(); prototype = prototype->getPrototypeDirect().getObject()) {
        const LayoutData& classLayout = *classWithIndex(uncheckedDowncast<ClassPrototype>(prototype)->classIndex());
        auto member = findOwnDeclaredMember(classLayout, keyOf(vm, name));
        if (!member)
            return std::nullopt;
        if (member->method)
            return resultAt(declaredMemberLocation(vm, FunctionID(member->method->function), nullptr, creation));
        if (member->isAccessor())
            return resultAt(declaredMemberLocation(vm, keyOfAccessor(*member), &*member, creation));
        if (name == vm.propertyNames->constructor.impl())
            return resultAt(declaredMemberLocation(vm, FunctionID(classLayout.classData().classConstructor), nullptr, creation));
    }
    if (prototype && (prototype->classInfo() == ClassPrototype::info() || compiledLayoutOrNull(prototype->structure())))
        return std::nullopt;
    return LazyPrototypeLookupResult { std::nullopt, prototype };
}

std::optional<ClassTable::DeclaredMemberLocation> ClassTable::declaredMemberLocation(VM& vm, FunctionID key, const DeclaredMember* accessorOrNull, MemberCreation creation)
{
    static_assert(declaredMembersPerHolder <= JSFinalObject::maxInlineCapacity);
    auto known = m_declaredMemberLocations.find(key.index());
    bool isKnown = known != m_declaredMemberLocations.end();
    if (!isKnown && creation == MemberCreation::Forbidden)
        return std::nullopt;
    uint32_t location = isKnown ? known->value : m_numberOfDeclaredMemberLocations;
    if (!isKnown) {
        if (location == m_declaredMemberHolders.size() * declaredMembersPerHolder) {
            Structure* structure = location ? m_declaredMemberHolders[0]->structure() : JSFinalObject::createStructure(vm, m_state.instance.globalObject, jsNull(), declaredMembersPerHolder);
            m_declaredMemberHolders.append(constructEmptyObject(vm, structure));
            m_state.immortalCells->add(m_declaredMemberHolders.last());
        }
        JSCell* member = accessorOrNull ? static_cast<JSCell*>(createGetterSetter(vm, m_state, *accessorOrNull)) : closureObject(vm, m_state, key);
        if (accessorOrNull)
            m_state.immortalCells->add(member);
        m_declaredMemberHolders.last()->putDirectOffset(vm, location % declaredMembersPerHolder, member);
        m_declaredMemberLocations.add(key.index(), location);
        ++m_numberOfDeclaredMemberLocations;
    }
    return DeclaredMemberLocation { m_declaredMemberHolders[location / declaredMembersPerHolder], static_cast<PropertyOffset>(location % declaredMembersPerHolder), !!accessorOrNull };
}

GetterSetter* ClassTable::createDeclaredAccessor(VM& vm, const LayoutData& classLayout, UniquedStringImpl* name)
{
    auto member = findOwnDeclaredMember(classLayout, keyOf(vm, name));
    if (!member || !member->isAccessor())
        return nullptr;
    return createGetterSetter(vm, m_state, *member);
}

ClassStructureCache::ClassStructureCache(State& state)
{
    state.caches->add(CacheKind::WeakByKey, "class structures"_s, *this);
}

ClassStructures ClassStructureCache::find(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull) const
{
    return m_structures.get(keyFor(classIndex, classObjectParentOrNull, prototypeParentOrNull));
}

void ClassStructureCache::add(uint32_t classIndex, JSObject* classObjectParentOrNull, JSCell* prototypeParentOrNull, ClassStructures structures)
{
    ASSERT(structures.classObjectStructure && structures.prototypeStructure);
    m_structures.set(keyFor(classIndex, classObjectParentOrNull, prototypeParentOrNull), structures);
    m_writes.recordAll();
}

size_t ClassStructureCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t sizeBefore = m_structures.size();
    m_writes.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_structures.removeIf([&](auto& entry) {
            return !vm.heap.isMarked(entry.value.classObjectStructure) || !vm.heap.isMarked(entry.value.prototypeStructure);
        });
    });
    return sizeBefore - m_structures.size();
}

namespace {

using ClassPart = ClassTable::ClassPart;

class ClassDefinition {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(ClassDefinition);
public:
    ClassDefinition(ClassTable&, JSGlobalObject*, Closure* classObject, ClassPart);

    void run(JSValue heritage, JSValue prototypeParent);

private:
    enum class StoreTime : uint8_t { BeforeDefinition, AfterDefinition };
    enum class ParentResolution : uint8_t { Resolved, BaseNotDefined };

    struct EarlyStores {
        void takeFrom(JSGlobalObject*, JSObject*);
        void restoreTo(JSGlobalObject*, JSObject*, StoreTime);

        Vector<std::pair<RefPtr<UniquedStringImpl>, unsigned>, 2> keysAndAttributes;
        MarkedArgumentBuffer values;
    };

    ParentResolution resolveParents(JSValue heritage, JSValue prototypeParent);
    Closure* findBaseClassObject(const LayoutData& baseClass, JSValue heritage);
    void chooseStructures();
    void createPrototype();
    bool tryDeferClassObject();
    void prepareObjects();
    void defineMembers(JSObject* target, std::span<const MethodData>);
    void defineMember(JSObject* target, const MethodData&, const Identifier& key, JSObject* function);
    bool hasAssumedBase() const;
    void armGuards();
#if ASSERT_ENABLED
    bool hasMemberEnvironment() const;
#endif

    void storeShared(JSObject* target, UniquedStringImpl* key, JSValue);

    bool createsClassObject() const { return m_part != ClassPart::PrototypeMembers; }
    bool createsPrototypeMembers() const { return m_part != ClassPart::ClassObject; }
    bool isShared() const { return m_shared.classObjectStructure; }
    JSCell* prototypeParentCell() const { return m_prototypeParent.isCell() ? m_prototypeParent.asCell() : nullptr; }

    ClassTable& m_classes;
    State& m_state;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    Closure* m_classObject;
    const LayoutData& m_classLayout;
    ClassPart m_part;
    const ClassPart m_requestedPart;
    JSValue m_prototypeParent;
    JSObject* m_classObjectParent { nullptr };
    JSObject* m_prototype { nullptr };
    ClassStructures m_shared;
    bool m_mayShareStructures { false };
    bool m_hasOnlyPlainMembers { true };
    bool m_hasMembersCreatedAtDefinition { false };
    bool m_classObjectMayBePrototype { false };
    EarlyStores m_earlyStores;
    EarlyStores m_earlyPrototypeStores;
};

ClassDefinition::ClassDefinition(ClassTable& classes, JSGlobalObject* globalObject, Closure* classObject, ClassPart part)
    : m_classes(classes)
    , m_state(classObject->state())
    , m_vm(globalObject->vm())
    , m_globalObject(globalObject)
    , m_classObject(classObject)
    , m_classLayout(*classObject->classData())
    , m_part(part)
    , m_requestedPart(part)
{
}

void ClassDefinition::EarlyStores::takeFrom(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        keysAndAttributes.append({ entry.key(), entry.attributes() });
        values.append(object->getDirect(entry.offset()));
        return true;
    });
    PropertyNameArrayBuilder indices(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    object->getOwnIndexedPropertyNames(globalObject, indices, DontEnumPropertiesMode::Include);
    for (const Identifier& key : indices) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isPresent = JSObject::getOwnPropertySlotByIndex(object, globalObject, *parseIndex(key), slot);
        RELEASE_ASSERT(isPresent);
        keysAndAttributes.append({ key.impl(), slot.attributes() });
        values.append(slot.isAccessor() ? JSValue(slot.getterSetter()) : slot.getValue(globalObject, key));
    }
}

static bool hasOwnIndexedProperty(JSGlobalObject* globalObject, JSObject* object, uint32_t index)
{
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &globalObject->vm());
    return JSObject::getOwnPropertySlotByIndex(object, globalObject, index, slot);
}

void ClassDefinition::EarlyStores::restoreTo(JSGlobalObject* globalObject, JSObject* object, StoreTime storeTime)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (size_t i = 0; i < keysAndAttributes.size(); ++i) {
        auto& [key, attributes] = keysAndAttributes[i];
        if (std::optional<uint32_t> index = parseIndex(PropertyName(key.get()))) {
            if (storeTime == StoreTime::BeforeDefinition && hasOwnIndexedProperty(globalObject, object, *index))
                continue;
            object->putDirectIndex(globalObject, *index, values.at(i), attributes, PutDirectIndexLikePutDirect);
            scope.assertNoException();
            continue;
        }
        if (storeTime == StoreTime::BeforeDefinition && isValidOffset(object->structure()->get(vm, key.get())))
            continue;
        if (attributes & PropertyAttribute::Accessor)
            object->putDirectNonIndexAccessor(vm, key.get(), uncheckedDowncast<GetterSetter>(values.at(i)), attributes);
        else if (attributes & PropertyAttribute::CustomAccessorOrValue)
            object->putDirectCustomAccessor(vm, key.get(), values.at(i), attributes);
        else
            object->putDirect(vm, key.get(), values.at(i), attributes);
    }
}

#if ASSERT_ENABLED
bool ClassDefinition::hasMemberEnvironment() const
{
    if (m_classLayout.classData().environment != LayoutData::noBase)
        return m_classObject->captures().size() == 1 && m_classObject->environment() && uncheckedDowncast<Environment>(m_classObject->environment())->layout().index == m_classLayout.classData().environment;
    auto takesEnvironment = [](const MethodData& method) {
        return !(method.flags & methodIsCreatedAtDefinition) && (method.flags & methodTakesEnvironment);
    };
    return std::ranges::none_of(m_classLayout.methods(), takesEnvironment) && std::ranges::none_of(m_classLayout.staticMethods(), takesEnvironment);
}
#endif

void ClassDefinition::run(JSValue heritage, JSValue prototypeParent)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    ClassTable::InternalPutScope internalPut(m_classes);
    if (!m_classObject->kind().isInRuntimeModule)
        m_state.statics->ensureProgramStaticsInitialized();
    ASSERT(m_requestedPart == ClassPart::Everything || m_classLayout.createsMembersLazily);
    ASSERT(!createsClassObject() || !m_classObject->getDirect(m_vm, m_vm.propertyNames->prototype));
    ASSERT(hasMemberEnvironment());

    if (resolveParents(heritage, prototypeParent) == ParentResolution::BaseNotDefined)
        return;
    MemberGuardWatchpoints::MethodTableUpdate update(m_state);
    chooseStructures();
    createPrototype();
    if (tryDeferClassObject()) {
        armGuards();
        return;
    }
    prepareObjects();
    if (createsPrototypeMembers())
        defineMembers(m_prototype, m_classLayout.methods());
    if (!scope.exception() && createsClassObject())
        defineMembers(m_classObject, m_classLayout.staticMethods());
    if (m_classObjectMayBePrototype)
        m_classObject->structure()->setMayBePrototype(true);
    if (createsPrototypeMembers() && !isShared())
        m_prototype->structure()->setMayBePrototype(true);
    if (scope.exception()) [[unlikely]]
        return;
    if (m_mayShareStructures && !isShared() && m_hasOnlyPlainMembers)
        m_state.objects->caches->classStructures.add(m_classLayout.index, m_classObjectParent, prototypeParentCell(), { m_classObject->structure(), m_prototype->structure() });
    m_earlyStores.restoreTo(m_globalObject, m_classObject, m_requestedPart == ClassPart::Everything ? StoreTime::BeforeDefinition : StoreTime::AfterDefinition);
    m_earlyPrototypeStores.restoreTo(m_globalObject, m_prototype, StoreTime::AfterDefinition);
    armGuards();
}

Closure* ClassDefinition::findBaseClassObject(const LayoutData& baseClass, JSValue heritage)
{
    ClosureTable& closures = m_state.objects->closures;
    FunctionID constructor(baseClass.classData().classConstructor);
    if (ClassTable::hasOneClassObject(baseClass))
        return uncheckedDowncast<Closure>(closures.closure(m_vm, constructor));
    Closure* baseClassObject = heritage ? dynamicDowncast<Closure>(heritage) : nullptr;
    if (!baseClassObject && baseClass.evaluation() == EvaluationState::ProvenOnce)
        baseClassObject = dynamicDowncast<Closure>(closures.closureIfCreated(constructor));
    return baseClassObject;
}

ClassDefinition::ParentResolution ClassDefinition::resolveParents(JSValue heritage, JSValue prototypeParent)
{
    bool wasDeclared = m_requestedPart == ClassPart::ClassObject && !m_classLayout.createsPrototypeBeforeClassObject;
    bool isDefinedBeforeEvaluation = m_requestedPart == ClassPart::Everything && !heritage;
    m_prototypeParent = m_globalObject->objectPrototype();
    if (!createsClassObject())
        m_prototypeParent = m_classes.classPrototypeIfExists(m_classLayout)->getPrototypeDirect();
    else if (wasDeclared) {
        m_classObjectParent = m_classObject->getPrototypeDirect().getObject();
        m_prototypeParent = m_classes.classPrototypeIfExists(m_classLayout)->getPrototypeDirect();
    } else if (const LayoutData* baseClass = m_classes.baseOf(m_classLayout)) {
        Closure* baseClassObject = findBaseClassObject(*baseClass, heritage);
        if (!baseClassObject && isDefinedBeforeEvaluation)
            return ParentResolution::BaseNotDefined;
        RELEASE_ASSERT(baseClassObject && m_classes.classConstructedBy(baseClassObject->function()) == baseClass);
        m_classObjectParent = baseClassObject;
        m_prototypeParent = baseClassObject->prototypeProperty(m_vm);
        if (!m_prototypeParent && isDefinedBeforeEvaluation)
            return ParentResolution::BaseNotDefined;
        RELEASE_ASSERT(m_prototypeParent);
    } else if (heritage || m_classLayout.hasUntypedBase) {
        if (uint32_t builtinBase = m_classLayout.classData().builtinBase; !heritage && builtinBase != LayoutData::noBase) {
            heritage = m_state.builtinCells->findObject(BuiltinID(builtinBase));
            prototypeParent = asObject(heritage)->getDirect(m_vm, m_vm.propertyNames->prototype);
        }
        RELEASE_ASSERT(heritage && (heritage.isObject() || heritage.isNull()));
        RELEASE_ASSERT(prototypeParent && (prototypeParent.isObject() || prototypeParent.isNull()));
        m_classObjectParent = heritage.getObject();
        m_prototypeParent = prototypeParent;
    }
    if (m_prototypeParent.isObject())
        asObject(m_prototypeParent)->didBecomePrototype(m_vm);
    return ParentResolution::Resolved;
}

static Butterfly* allocateOutOfLineStorage(VM& vm, Structure* structure)
{
    return structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
}

void ClassDefinition::storeShared(JSObject* target, UniquedStringImpl* key, JSValue value)
{
    PropertyOffset offset = target->structure()->get(m_vm, key);
    RELEASE_ASSERT(isValidOffset(offset));
    target->putDirectOffset(m_vm, offset, value);
}

void ClassDefinition::chooseStructures()
{
    if (m_part == ClassPart::Everything && m_classLayout.createsMembersLazily && m_prototypeParent.isObject())
        m_part = ClassPart::ClassObject;
    m_mayShareStructures = m_part == ClassPart::Everything && m_classes.keepsStructuresForNextEvaluation(*m_classObject, m_classLayout) && !m_classObject->mayBePrototype() && !m_classObject->butterfly();
    if (!m_mayShareStructures)
        return;
    m_shared = m_state.objects->caches->classStructures.find(m_classLayout.index, m_classObjectParent, prototypeParentCell());
    if (m_shared.classObjectStructure && (m_shared.classObjectStructure->realm() != m_globalObject || m_shared.classObjectStructure->storedPrototype() != (m_classObjectParent ? JSValue(m_classObjectParent) : m_classObject->getPrototypeDirect())))
        m_shared = { };
}

void ClassDefinition::createPrototype()
{
    if (isShared()) {
        m_prototype = JSFinalObject::createWithButterfly(m_vm, m_shared.prototypeStructure, allocateOutOfLineStorage(m_vm, m_shared.prototypeStructure));
        return;
    }
    if (m_part == ClassPart::Everything) {
        m_prototype = constructEmptyObject(m_vm, JSFinalObject::createStructure(m_vm, m_globalObject, m_prototypeParent, JSFinalObject::defaultInlineCapacity));
        if (m_classLayout.evaluation() == EvaluationState::ProvenOnce)
            m_classes.registerClassPrototype(m_classLayout, m_prototype);
        return;
    }
    m_prototype = m_classes.classPrototypeIfExists(m_classLayout);
    if (!m_prototype)
        m_prototype = m_classes.createClassPrototype(m_vm, m_classLayout, asObject(m_prototypeParent));
    m_prototypeParent = m_prototype->getPrototypeDirect();
}

bool ClassDefinition::tryDeferClassObject()
{
    if (m_requestedPart != ClassPart::Everything || m_part != ClassPart::ClassObject || !m_classLayout.createsClassObjectOnMutation || m_classObject->mayBePrototype() || m_classObject->butterfly())
        return false;
    if (!m_classObjectParent)
        return m_classObject->structure() == m_state.objects->closures.closureStructure(m_vm, classObjectClosureStructure);
    m_classObject->setStructure(m_vm, m_classes.lazyClassObjectStructureInheritingFrom(m_vm, m_classObjectParent));
    return true;
}

void ClassDefinition::prepareObjects()
{
    if (isShared()) {
        RELEASE_ASSERT(!m_classObject->butterfly());
        if (Butterfly* storage = allocateOutOfLineStorage(m_vm, m_shared.classObjectStructure))
            m_classObject->nukeStructureAndSetButterfly(m_vm, m_classObject->structureID(), storage);
        m_classObject->setStructure(m_vm, m_shared.classObjectStructure);
        storeShared(m_classObject, m_vm.propertyNames->prototype.impl(), m_prototype);
        storeShared(m_prototype, m_vm.propertyNames->constructor.impl(), m_classObject);
        return;
    }
    if (createsClassObject()) {
        m_classObjectMayBePrototype = m_classObject->mayBePrototype();
        bool wasReachable = m_requestedPart != ClassPart::Everything || m_classObjectMayBePrototype;
        RELEASE_ASSERT(!wasReachable || !m_classObjectParent || !m_classLayout.createsClassObjectOnMutation || m_classObject->getPrototypeDirect() == m_classObjectParent);
        if (m_classObject->butterfly())
            m_earlyStores.takeFrom(m_globalObject, m_classObject);
        m_classObject->initializeClassObject(m_vm, m_classObjectParent ? m_classObjectParent : m_classObject->getPrototypeDirect().getObject(), m_prototype);
    }
    if (m_part == ClassPart::PrototypeMembers) {
        Structure* ordinaryStructure = JSFinalObject::createStructure(m_vm, m_globalObject, m_prototypeParent, ClassPrototype::inlineCapacity);
        if (m_prototype->butterfly())
            m_earlyPrototypeStores.takeFrom(m_globalObject, m_prototype);
        uncheckedDowncast<ClassPrototype>(m_prototype)->convertToOrdinaryObject(m_vm, ordinaryStructure);
    }
    if (createsPrototypeMembers())
        m_prototype->putDirectWithoutTransition(m_vm, m_vm.propertyNames->constructor, m_classObject, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

void ClassDefinition::defineMembers(JSObject* target, std::span<const MethodData> methods)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    for (auto& method : methods) {
        if (method.flags & methodIsCreatedAtDefinition) {
            m_hasMembersCreatedAtDefinition = true;
            continue;
        }
        ASSERT(!(method.flags & methodHasComputedName));
        JSObject* function = m_classes.createMethodClosure(m_vm, m_classObject, m_classLayout, method);
        Identifier key = m_vm.aotState().identifiers->identifier(method.name);
        defineMember(target, method, key, function);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void ClassDefinition::defineMember(JSObject* target, const MethodData& method, const Identifier& key, JSObject* function)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    MethodKind kind = method.kind();
    auto createAccessor = [&] {
        bool isGetter = kind == MethodKind::Getter;
        return GetterSetter::create(m_vm, m_globalObject, isGetter ? function : nullptr, isGetter ? nullptr : function);
    };
    if (isShared()) {
        storeShared(target, key.impl(), kind == MethodKind::Method ? static_cast<JSCell*>(function) : createAccessor());
        return;
    }
    constexpr unsigned accessorAttributes = PropertyAttribute::Accessor | PropertyAttribute::DontEnum;
    bool isLazyFunctionProperty = target == m_classObject && (key == m_vm.propertyNames->name || key == m_vm.propertyNames->length || key == m_vm.propertyNames->prototype);
    bool isOrdinaryName = !isLazyFunctionProperty && !parseIndex(key);
    unsigned attributes = 0;
    PropertyOffset offset = isOrdinaryName ? target->structure()->get(m_vm, key, attributes) : invalidOffset;
    if (isOrdinaryName && !isValidOffset(offset)) {
        if (kind == MethodKind::Method)
            target->putDirectWithoutTransition(m_vm, key, function, static_cast<unsigned>(PropertyAttribute::DontEnum));
        else
            target->putDirectNonIndexAccessorWithoutTransition(m_vm, key, createAccessor(), accessorAttributes);
        return;
    }
    m_hasOnlyPlainMembers = false;
    if (isValidOffset(offset) && kind != MethodKind::Method && attributes == accessorAttributes && target->structure()->isPinnedPropertyTable()) {
        auto* current = uncheckedDowncast<GetterSetter>(target->getDirect(offset));
        JSObject* getter = kind == MethodKind::Getter ? function : current->isGetterNull() ? nullptr : current->getter();
        JSObject* setter = kind == MethodKind::Setter ? function : current->isSetterNull() ? nullptr : current->setter();
        target->structure()->didReplaceProperty(offset);
        target->putDirectOffset(m_vm, offset, GetterSetter::create(m_vm, m_globalObject, getter, setter));
        return;
    }
    PropertyDescriptor descriptor;
    switch (kind) {
    case MethodKind::Method:
        descriptor.setValue(function);
        descriptor.setWritable(true);
        break;
    case MethodKind::Getter:
        descriptor.setGetter(function);
        break;
    case MethodKind::Setter:
        descriptor.setSetter(function);
        break;
    }
    descriptor.setEnumerable(false);
    descriptor.setConfigurable(true);
    JSObject::defineOwnProperty(target, m_globalObject, key, descriptor, true);
    RETURN_IF_EXCEPTION(scope, void());
    if (target == m_classObject && key == m_vm.propertyNames->length)
        m_classObject->ensureRareData(m_vm)->setHasModifiedLengthForBoundOrNonHostFunction();
    if (target == m_classObject && key == m_vm.propertyNames->name)
        m_classObject->ensureRareData(m_vm)->setHasModifiedNameForBoundOrNonHostFunction();
}

bool ClassDefinition::hasAssumedBase() const
{
    uint32_t assumedBase = m_classLayout.assumedBase();
    if (assumedBase == LayoutData::noBase)
        return true;
    const LayoutData* baseClass = m_classes.classWithIndex(assumedBase);
    if (!baseClass || !m_classObjectParent)
        return false;
    FunctionID baseConstructor(baseClass->classData().classConstructor);
    return baseConstructor && m_classObjectParent == m_state.objects->closures.closureIfCreated(baseConstructor);
}

void ClassDefinition::armGuards()
{
    using HolderState = ClassTable::HolderState;
    bool holdersHaveWhatRecordSays = m_hasOnlyPlainMembers && !m_hasMembersCreatedAtDefinition && !isShared() && m_earlyStores.keysAndAttributes.isEmpty() && m_earlyPrototypeStores.keysAndAttributes.isEmpty();
    HolderState stateOfPrototype = holdersHaveWhatRecordSays ? HolderState::AsDefinitionMadeIt : HolderState::Unknown;
    HolderState stateOfClassObject = holdersHaveWhatRecordSays && m_classLayout.staticFields().empty() ? HolderState::AsDefinitionMadeIt : HolderState::Unknown;
    if (const LayoutData* base = m_classes.assumedBaseOf(m_classLayout); base && hasAssumedBase())
        m_classes.classPrototypeOfBase(m_vm, *base);
    m_classes.armMemberGuards(m_vm, m_classLayout, ClassSide::Prototype, m_prototype, createsPrototypeMembers() ? stateOfPrototype : HolderState::NotTouched);
    m_classes.armMemberGuards(m_vm, m_classLayout, ClassSide::Static, m_classObject, createsClassObject() ? stateOfClassObject : HolderState::NotTouched);
    if (m_requestedPart != ClassPart::Everything || hasAssumedBase())
        return;
    m_classes.didChangePrototypeLink(m_classLayout, ClassSide::Static);
    m_classes.didChangePrototypeLink(m_classLayout, ClassSide::Prototype);
}

} // namespace

JSValue ClassTable::resolveHeritage(JSGlobalObject* globalObject, JSValue heritage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (heritage.isNull())
        return jsNull();
    if (!heritage.isConstructor()) {
        throwException(globalObject, scope, createSuperclassNotConstructorError(globalObject));
        return { };
    }
    if (auto* classObject = dynamicDowncast<Closure>(heritage.asCell()); classObject && classObject->isLazyClassObject()) {
        if (JSValue prototype = classObject->prototypeProperty(vm))
            return prototype;
    }
    JSValue prototypeParent = asObject(heritage)->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    if (!prototypeParent.isObject() && !prototypeParent.isNull()) {
        throwTypeError(globalObject, scope, "The value of the superclass's prototype property is not an object or null."_s);
        return { };
    }
    return prototypeParent;
}

static void markFunctionPropertiesDeclaredAsFields(VM& vm, Closure* classObject, const LayoutData& classLayout)
{
    for (const FieldData& field : classLayout.staticFields()) {
        const Identifier& key = vm.aotState().identifiers->identifier(field.name);
        if (key == vm.propertyNames->name)
            classObject->ensureRareData(vm)->setHasModifiedNameForBoundOrNonHostFunction();
        else if (key == vm.propertyNames->length)
            classObject->ensureRareData(vm)->setHasModifiedLengthForBoundOrNonHostFunction();
        else if (key == vm.propertyNames->displayName)
            didChangeDisplayName(vm, classObject);
    }
}

bool ClassTable::tryLeaveDefinitionToRecord(VM& vm, Closure* classObject, const LayoutData& classLayout, JSValue heritage, JSValue prototypeParent)
{
    if (!leavesDefinitionToRecord(classLayout))
        return false;
    if (classObject->butterfly() || classObject->mayBePrototype())
        return false;
    if (JSObject* prototype = classPrototypeIfExists(classLayout); prototype && (!classLayout.createsPrototypeBeforeClassObject || prototype->classInfo() != ClassPrototype::info()))
        return false;
    ClosureTable& closures = m_state.objects->closures;
    FunctionID constructor = classObject->function();
    JSCell* registered = closures.closureIfCreated(constructor);
    if (registered && registered != classObject)
        return false;

    Structure* structure = closures.closureStructure(vm, classObjectClosureStructure);
    if (classObject->structure() != structure)
        return false;
    JSObject* parentOfPrototype = nullptr;
    if (const LayoutData* baseClass = baseOf(classLayout)) {
        FunctionID baseConstructor(baseClass->classData().classConstructor);
        JSCell* baseClassObject = hasOneClassObject(*baseClass) ? closures.closure(vm, baseConstructor) : closures.closureIfCreated(baseConstructor);
        if (!baseClassObject || (heritage && heritage != JSValue(baseClassObject)))
            return false;
        if (!classLayout.createsPrototypeBeforeClassObject) {
            JSValue prototypeOfBase = uncheckedDowncast<Closure>(baseClassObject)->prototypeProperty(vm);
            if (!prototypeOfBase || !prototypeOfBase.isObject())
                return false;
            parentOfPrototype = asObject(prototypeOfBase);
        }
        structure = lazyClassObjectStructureInheritingFrom(vm, asObject(baseClassObject));
    } else if (heritage || classLayout.hasUntypedBase) {
        if (!heritage || !heritage.isObject() || !prototypeParent || !prototypeParent.isObject() || classLayout.assumedBase() != LayoutData::noBase)
            return false;
        parentOfPrototype = asObject(prototypeParent);
        structure = lazyClassObjectStructureInheritingFrom(vm, asObject(heritage));
    }
    if (!parentOfPrototype && !classLayout.createsPrototypeBeforeClassObject)
        return false;

    if (!registered)
        closures.registerClassObject(classObject);
    if (!classLayout.staticFields().empty())
        markFunctionPropertiesDeclaredAsFields(vm, classObject, classLayout);
    classObject->setStructure(vm, structure);
    if (parentOfPrototype) {
        parentOfPrototype->didBecomePrototype(vm);
        createClassPrototype(vm, classLayout, parentOfPrototype);
        didCreatePrototypeLeftToRecord(classLayout);
    }
    return true;
}

bool ClassTable::tryDefineAgainWithKeptStructures(JSGlobalObject* globalObject, Closure* classObject, const LayoutData& classLayout, JSValue heritage, JSValue prototypeParent)
{
    VM& vm = globalObject->vm();
    const ClassData& classData = classLayout.classData();
    if (classData.numberOfMethods || classData.numberOfStaticMethods || classData.numberOfStaticFields || classLayout.assumedBase() != LayoutData::noBase)
        return false;
    if (classObject->butterfly() || classObject->mayBePrototype())
        return false;
    JSObject* classObjectParent = nullptr;
    JSValue parentOfPrototype = globalObject->objectPrototype();
    if (heritage) {
        if (!prototypeParent)
            return false;
        classObjectParent = heritage.getObject();
        parentOfPrototype = prototypeParent;
    } else if (classLayout.hasUntypedBase)
        return false;

    ClassStructures kept = m_state.objects->caches->classStructures.find(classLayout.index, classObjectParent, parentOfPrototype.isCell() ? parentOfPrototype.asCell() : nullptr);
    if (!kept || kept.classObjectStructure->realm() != globalObject || kept.classObjectStructure->storedPrototype() != (classObjectParent ? JSValue(classObjectParent) : classObject->getPrototypeDirect()))
        return false;
    PropertyOffset offsetOfPrototype = kept.classObjectStructure->get(vm, vm.propertyNames->prototype);
    PropertyOffset offsetOfConstructor = kept.prototypeStructure->get(vm, vm.propertyNames->constructor);
    RELEASE_ASSERT(isValidOffset(offsetOfPrototype) && isValidOffset(offsetOfConstructor));

    if (!classObject->kind().isInRuntimeModule)
        m_state.statics->ensureProgramStaticsInitialized();
    if (parentOfPrototype.isObject())
        asObject(parentOfPrototype)->didBecomePrototype(vm);
    JSObject* prototype = JSFinalObject::createWithButterfly(vm, kept.prototypeStructure, allocateOutOfLineStorage(vm, kept.prototypeStructure));
    if (Butterfly* storage = allocateOutOfLineStorage(vm, kept.classObjectStructure))
        classObject->nukeStructureAndSetButterfly(vm, classObject->structureID(), storage);
    classObject->setStructure(vm, kept.classObjectStructure);
    classObject->putDirectOffset(vm, offsetOfPrototype, prototype);
    prototype->putDirectOffset(vm, offsetOfConstructor, classObject);
    armMemberGuards(vm, classLayout, ClassSide::Prototype, prototype);
    armMemberGuards(vm, classLayout, ClassSide::Static, classObject);
    return true;
}

void ClassTable::defineClass(JSGlobalObject* globalObject, Closure* classObject, const LayoutData& classLayout, JSValue parent, JSValue prototypeParent)
{
    VM& vm = globalObject->vm();
    if (tryLeaveDefinitionToRecord(vm, classObject, classLayout, parent, prototypeParent))
        return;
    bool isProvenOnce = classLayout.evaluation() == EvaluationState::ProvenOnce;
    ClosureTable& closures = m_state.objects->closures;
    if (isProvenOnce && (classObject->getDirect(vm, vm.propertyNames->prototype) || (!classLayout.createsPrototypeBeforeClassObject && classPrototypeIfExists(classLayout))))
        return;
    JSCell* registered = closures.closureIfCreated(classObject->function());
    if (registered && registered != classObject) {
        RELEASE_ASSERT(!isProvenOnce);
        didEvaluateClassAgain(m_state, classLayout);
        if (tryDefineAgainWithKeptStructures(globalObject, classObject, classLayout, parent, prototypeParent))
            return;
    } else if (!registered && classLayout.evaluation() != EvaluationState::SeenTwice)
        closures.registerClassObject(classObject);
    markFunctionPropertiesDeclaredAsFields(vm, classObject, classLayout);
    ClassDefinition(*this, globalObject, classObject, ClassPart::Everything).run(parent, prototypeParent);
}

void ClassTable::defineClass(JSGlobalObject* globalObject, Closure* classObject, JSValue parent, JSValue prototypeParent)
{
    defineClass(globalObject, classObject, *classObject->classData(), parent, prototypeParent);
}

void ClassTable::createClassPart(JSGlobalObject* globalObject, Closure* classObject, ClassPart part)
{
    ASSERT(part != ClassPart::Everything);
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    ClassDefinition(*this, globalObject, classObject, part).run(JSValue(), JSValue());
    scope.assertNoException();
}

JSObject* ClassTable::createMethodClosure(VM& vm, Closure* classObject, const LayoutData& classLayout, const MethodData& method)
{
    ClosureTable& closures = m_state.objects->closures;
    FunctionID function(method.function);
    bool takesEnvironment = method.flags & methodTakesEnvironment;
    if (classLayout.evaluation() == EvaluationState::ProvenOnce && !takesEnvironment)
        return asObject(closures.closure(vm, function));
    return closures.createClosure(vm, function, classObject->captures().first(takesEnvironment), takesEnvironment);
}

const ClassInfo ClassPrototype::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ClassPrototype) };

static_assert(sizeof(ClassPrototype) == sizeof(JSObjectWithButterfly) + ClassPrototype::inlineCapacity * sizeof(WriteBarrierBase<Unknown>));

static State& stateOf(const JSObject* classPrototype)
{
    return *Instance::of(classPrototype->structure()->realm())->state;
}

Structure* ClassPrototype::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    structure->setMayBePrototype(true);
    return structure;
}

ClassPrototype* ClassPrototype::create(VM& vm, Structure* structure, uint32_t classIndex, uint32_t memberRow)
{
    ClassPrototype* object = new (NotNull, allocateCell<ClassPrototype>(vm)) ClassPrototype(vm, structure, classIndex, memberRow);
    object->finishCreation(vm);
    return object;
}

void ClassPrototype::convertToOrdinaryObject(VM& vm, Structure* ordinary)
{
    RELEASE_ASSERT(ordinary->classInfoForCells() == JSFinalObject::info() && ordinary->inlineCapacity() == inlineCapacity && ordinary->storedPrototype() == structure()->storedPrototype());
    m_classIndex = 0;
    m_numberOfLookups = 0;
    m_hasRepeatedLookup = false;
    m_flags = { };
    m_namesLookedUp = 0;
    m_memberRow = 0;
    WTF::storeStoreFence();
    if (butterfly())
        nukeStructureAndSetButterfly(vm, structureID(), nullptr);
    setStructure(vm, ordinary);
}

void ClassPrototype::createMembers(JSObject* object)
{
    VM& vm = object->vm();
    State& state = stateOf(object);
    ClassTable& classes = state.objects->classes;
    const LayoutData& classLayout = *classes.classWithIndex(uncheckedDowncast<ClassPrototype>(object)->classIndex());
    Closure* classObject = uncheckedDowncast<Closure>(state.objects->closures.closure(vm, FunctionID(classLayout.classData().classConstructor)));
    VMPropertyCaches::UnreportedChange change(*vm.aotState().propertyCaches, object);
    classes.createClassPart(state.instance.globalObject, classObject, ClassTable::ClassPart::PrototypeMembers);
    RELEASE_ASSERT(object->type() == FinalObjectType);
    state.chainDependents->didChange(object, nullptr);
}

bool ClassPrototype::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    if (object->vm().heap.currentThreadIsDoingGCWork()) [[unlikely]] {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    if (auto answer = uncheckedDowncast<ClassPrototype>(object)->tryGetDeclaredMember(globalObject->vm(), name, slot))
        return *answer;
    createMembers(object);
    if (!slot.isVMInquiry())
        slot.setIsTaintedByOpaqueObject();
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

std::optional<bool> ClassPrototype::tryGetDeclaredMember(VM& vm, PropertyName name, PropertySlot& slot)
{
    auto methodType = slot.internalMethodType();
    if ((methodType != PropertySlot::InternalMethodType::Get && methodType != PropertySlot::InternalMethodType::HasProperty) || butterfly() || !name.uid())
        return std::nullopt;
    ClassTable& classes = stateOf(this).objects->classes;
    const LayoutData& classLayout = *classes.classWithIndex(m_classIndex);
    if (name.uid()->isSymbol() && std::ranges::none_of(classLayout.methods(), [](auto& method) { return method.flags & methodHasComputedName; }) && !classes.declaresOwnMember(classLayout, name.uid())) {
        slot.setIsTaintedByOpaqueObject();
        return false;
    }
    uint32_t nameBit = 1u << (name.uid()->hash() % numberOfNameBits);
    bool isRepeated = m_namesLookedUp & nameBit;
    if (m_numberOfLookups >= std::max<size_t>(minEarlyAnswers, classLayout.methods().size()) || (isRepeated && m_hasRepeatedLookup))
        return std::nullopt;
    auto method = classes.findDeclaredMethod(vm, classLayout, name.uid());
    GetterSetter* accessor = method ? nullptr : classes.createDeclaredAccessor(vm, classLayout, name.uid());
    if (!method && !accessor)
        return std::nullopt;
    RELEASE_ASSERT(classInfo() == info());
    ++m_numberOfLookups;
    m_hasRepeatedLookup |= isRepeated;
    m_namesLookedUp |= nameBit;
    slot.setIsTaintedByOpaqueObject();
    if (accessor) {
        slot.setGetterSlot(this, PropertyAttribute::Accessor | PropertyAttribute::DontEnum, accessor);
        return true;
    }
    if (!*method)
        return false;
    slot.setValue(this, static_cast<unsigned>(PropertyAttribute::DontEnum), *method);
    return true;
}

bool ClassPrototype::hasMember(JSObject* object, PropertyName name)
{
    UniquedStringImpl* uid = name.uid();
    if (!uid || uid == object->vm().propertyNames->constructor.impl())
        return !!uid;
    auto* prototype = uncheckedDowncast<ClassPrototype>(object);
    bool isThen = uid == object->vm().propertyNames->then.impl();
    if (isThen && prototype->m_flags.contains(Flag::HasCheckedThen))
        return prototype->m_flags.contains(Flag::DeclaresThen);
    ClassTable& classes = stateOf(object).objects->classes;
    bool isDeclared = classes.declaresOwnMember(*classes.classWithIndex(prototype->classIndex()), uid);
    if (isThen) {
        prototype->m_flags.add(Flag::HasCheckedThen);
        prototype->m_flags.set(Flag::DeclaresThen, isDeclared);
    }
    return isDeclared;
}

void ClassPrototype::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    if (mode == DontEnumPropertiesMode::Exclude && !object->butterfly())
        return;
    createMembers(object);
    JSObject::getOwnPropertyNames(object, globalObject, names, mode);
}

bool ClassPrototype::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    createMembers(asObject(cell));
    return JSObject::put(cell, globalObject, name, value, slot);
}

bool ClassPrototype::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    createMembers(asObject(cell));
    return JSObject::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool ClassPrototype::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    createMembers(asObject(cell));
    return JSObject::deleteProperty(cell, globalObject, name, slot);
}

bool ClassPrototype::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    createMembers(asObject(cell));
    return JSObject::deletePropertyByIndex(cell, globalObject, index);
}

bool ClassPrototype::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    createMembers(object);
    return JSObject::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
}

bool ClassPrototype::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    createMembers(object);
    return JSObject::preventExtensions(object, globalObject);
}

bool ClassPrototype::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    createMembers(object);
    return JSObject::setPrototype(object, globalObject, prototype, shouldThrowIfCantSet);
}

namespace StaticFields {

static JSC_DECLARE_CUSTOM_GETTER(staticFieldGetter);
static JSC_DECLARE_CUSTOM_SETTER(staticFieldSetter);

bool ownsVariables(const Closure& classObject)
{
    return classObject.kind().isConstructor && classObject.state().objects->closures.closureIfCreated(classObject.function()) == &classObject;
}

const FieldData* findOwnField(const Closure& classObject, UniquedStringImpl* uid)
{
    if (PropertyName(uid).isPrivateName())
        return nullptr;
    const LayoutData* layout = classObject.classData();
    if (!layout || layout->staticFields().empty() || !ownsVariables(classObject))
        return nullptr;
    const FieldData* field = classObject.state().objects->classes.staticField(*layout, uid);
    if (!field)
        return nullptr;
    std::span<const FieldData> ownFields = layout->staticFields();
    bool isOwn = std::bit_cast<uintptr_t>(field) - std::bit_cast<uintptr_t>(ownFields.data()) < ownFields.size_bytes();
    return isOwn ? field : nullptr;
}

bool isInVariable(const Closure& classObject, const FieldData& field)
{
    ProgramStatics& statics = *classObject.state().statics;
    statics.ensureProgramStaticsInitialized();
    return !slotHoldsAbsent(field.slot.representation(), &statics.at<uint8_t>(field.slot.offset));
}

bool canEnterVariable(VM& vm, const Closure& classObject, const FieldData& field)
{
    ASSERT(!isInVariable(classObject, field));
    State& state = classObject.state();
    const LayoutData& layout = *classObject.classData();
    UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
    Structure* structure = classObject.structure();
    if (!structure->isStructureExtensible() || isValidOffset(structure->get(vm, uid)) || state.objects->classes.hasStaticMethodNamed(layout, uid))
        return false;
    bool isDeclaredLater = false;
    for (const FieldData& other : layout.staticFields()) {
        if (isDeclaredLater && isInVariable(classObject, other))
            return false;
        isDeclaredLater |= &other == &field;
    }
    return true;
}

static CustomGetterSetter* accessor(VM& vm, State& state)
{
    CustomGetterSetter*& created = state.objects->caches->staticFieldAccessor;
    if (!created) {
        created = CustomGetterSetter::create(vm, staticFieldGetter, staticFieldSetter);
        state.immortalCells->add(created);
    }
    return created;
}

// A field enters its variable only while no later field is in its own, so the last one in its variable answers for all before it.
static bool hasEveryProperty(VM& vm, const Closure& classObject, std::span<const FieldData> fields)
{
    RuntimeLayoutTable& layouts = classObject.state().objects->layouts;
    for (const FieldData& field : fields | std::views::reverse) {
        if (!isInVariable(classObject, field))
            continue;
        UniquedStringImpl* uid = layouts.nameOf(vm, field);
        if (PropertyName(uid).isPrivateName())
            continue;
        unsigned attributes;
        return isValidOffset(classObject.structure()->get(vm, uid, attributes)) && (attributes & PropertyAttribute::CustomValue);
    }
    return true;
}

static void setHasReifiedProperty(VM& vm, Closure& classObject, UniquedStringImpl* uid)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    if (uid == names.name.impl()) {
        FunctionRareData* rareData = classObject.ensureRareData(vm);
        rareData->setHasReifiedName();
        rareData->setHasModifiedNameForBoundOrNonHostFunction();
    } else if (uid == names.length.impl()) {
        FunctionRareData* rareData = classObject.ensureRareData(vm);
        rareData->setHasReifiedLength();
        rareData->setHasModifiedLengthForBoundOrNonHostFunction();
    }
}

void ensureProperties(VM& vm, Closure& classObject)
{
    const LayoutData* layout = classObject.classData();
    if (!layout || layout->staticFields().empty() || !ownsVariables(classObject))
        return;
    if (hasEveryProperty(vm, classObject, layout->staticFields()))
        return;
    State& state = classObject.state();
    ClassTable::InternalPutScope internalPut(state.objects->classes);
    classObject.ensureClassObject(vm);
    for (const FieldData& field : layout->staticFields()) {
        UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
        if (PropertyName(uid).isPrivateName() || !isInVariable(classObject, field) || isValidOffset(classObject.structure()->get(vm, uid)))
            continue;
        classObject.putDirectCustomAccessor(vm, uid, accessor(vm, state), static_cast<unsigned>(PropertyAttribute::CustomValue));
        setHasReifiedProperty(vm, classObject, uid);
    }
}

void moveToProperty(VM& vm, Closure& classObject, const FieldData& field)
{
    RELEASE_ASSERT(field.canLeaveSlot());
    State& state = classObject.state();
    ClassTable& classes = state.objects->classes;
    UniquedStringImpl* uid = state.objects->layouts.nameOf(vm, field);
    JSValue value = classes.readStaticField(field);
    ASSERT(value);
    ClassTable::InternalPutScope internalPut(classes);
    ensureProperties(vm, classObject);
    classObject.putDirect(vm, uid, value, 0);
    ProgramStatics::writeVariable(state.instance, field.slot, JSValue());
    classes.didChangeMember(*classObject.classData(), ClassSide::Static, uid);
}

void willPutDirect(VM& vm, Closure& classObject, UniquedStringImpl* uid)
{
    const LayoutData* layout = classObject.classData();
    if (!layout || layout->staticFields().empty() || !ownsVariables(classObject) || classObject.state().objects->classes.isInInternalPut())
        return;
    const FieldData* field = findOwnField(classObject, uid);
    if (field && isInVariable(classObject, *field))
        moveToProperty(vm, classObject, *field);
    else
        ensureProperties(vm, classObject);
}

void didDeleteProperty(VM& vm, Closure& classObject, const FieldData& field)
{
    RELEASE_ASSERT(field.canLeaveSlot());
    State& state = classObject.state();
    ProgramStatics::writeVariable(state.instance, field.slot, JSValue());
    state.objects->classes.didChangeMember(*classObject.classData(), ClassSide::Static, state.objects->layouts.nameOf(vm, field));
}

const FieldData* findFieldForCache(VM& vm, JSCell* cell, UniquedStringImpl* uid)
{
    if (!isCompiledClosure(cell))
        return nullptr;
    Closure& classObject = *uncheckedDowncast<Closure>(cell);
    const FieldData* field = findOwnField(classObject, uid);
    if (!field || !isInVariable(classObject, *field))
        return nullptr;
    ensureProperties(vm, classObject);
    return field;
}

JSC_DEFINE_CUSTOM_GETTER(staticFieldGetter, (JSGlobalObject*, EncodedJSValue thisValue, PropertyName name))
{
    Closure& classObject = *uncheckedDowncast<Closure>(JSValue::decode(thisValue).asCell());
    const FieldData* field = findOwnField(classObject, name.uid());
    JSValue value = field ? classObject.state().objects->classes.readStaticField(*field) : JSValue();
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_CUSTOM_SETTER(staticFieldSetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, EncodedJSValue encodedValue, PropertyName name))
{
    VM& vm = globalObject->vm();
    Closure& classObject = *uncheckedDowncast<Closure>(JSValue::decode(thisValue).asCell());
    JSValue value = JSValue::decode(encodedValue);
    const FieldData* field = findOwnField(classObject, name.uid());
    RELEASE_ASSERT(field);
    if (!classObject.state().objects->classes.tryStoreStaticField(vm, *field, value)) {
        moveToProperty(vm, classObject, *field);
        classObject.putDirect(vm, name, value);
    }
    return true;
}

} // namespace StaticFields

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
