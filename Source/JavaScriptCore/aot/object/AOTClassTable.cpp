/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTChainDependents.h"
#include "AOTClassGuards.h"
#include "AOTClassMemberTable.h"
#include "AOTClassPrototype.h"
#include "AOTClassStructureCache.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTEffectsNet.h"
#include "AOTEnvironment.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTVMState.h"
#include "BrandedStructure.h"
#include "AbstractSlotVisitorInlines.h"
#include "ArrayPrototype.h"
#include "DeferTermination.h"
#include "FunctionPrototype.h"
#include "FunctionRareData.h"
#include "GetterSetter.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSMapInlines.h"
#include "JSSetInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "SlotVisitorInlines.h"
#include "TopExceptionScope.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Scope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static uint32_t findOffsetOfClassConstructedBy(const ProgramTables& tables, FunctionID function)
{
    auto constructors = tables.constructors();
    auto found = std::ranges::lower_bound(constructors, function.index(), { }, &ConstructorData::function);
    if (found == constructors.end() || found->function != function.index())
        return 0;
    return tables.offsetOfClass(found->layoutIndex);
}

static std::optional<uint32_t> keyOf(State& state, UniquedStringImpl* uid)
{
    return identifiersOf(state.instance).numberOf(uid);
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
    state.instance.classes.privateFieldPlaces = m_privateFieldPlaces.data();
    state.instance.classes.heirTemplates = m_heirTemplates.data();
    state.caches->add(CacheKind::WeakByKey, "class sides by holder"_s, *this);
    state.caches->add(CacheKind::StrongRoots, "prototype objects of class objects that are reached"_s, *this);
    state.caches->addClearedEachCollection("places of prototype properties"_s, asWritableBytes(std::span { m_placesOfPrototypeProperties }));
    state.caches->addClearedEachCollection("prototypes of class objects"_s, asWritableBytes(std::span { m_prototypesOfClassObjects }));
    state.caches->addClearedEachCollection("this beside callees"_s, asWritableBytes(std::span { m_thisBesideCallees }));
    state.caches->addClearedEachCollection("instances below Object"_s, asWritableBytes(std::span { m_instancesBelowObject }));
    state.caches->addClearedEachCollection("the last evaluation of a class"_s, asWritableBytes(singleElementSpan(m_lastEvaluation)));
    state.caches->addClearedEachCollection("structures with brands of heirs"_s, asWritableBytes(std::span { m_structuresWithBrandsOfHeirs }));
    state.caches->addClearedEachCollection("templates of heirs"_s, asWritableBytes(std::span { m_heirTemplates }));
    m_brandsOfHeirs.fill(nullptr, m_tables.numberOfClasses());
}

ClassTable::~ClassTable() = default;

Symbol* ClassTable::brandOfHeirsFor(VM& vm, JSCell* parent, JSCell* newTarget)
{
    auto* classObject = dynamicDowncast<Closure>(newTarget);
    const LayoutData* classLayout = classObject ? classObject->classData() : nullptr;
    uint32_t offsetOfProperties = classLayout ? classLayout->classData().propertiesOfHeirsOffset : 0;
    if (!offsetOfProperties)
        return nullptr;
    BuiltinID constructor = BuiltinTable::singleton().at(BuiltinClassID(m_tables.at<PropertiesOfHeirsData>(offsetOfProperties)->builtinClass())).constructor;
    if (!constructor || m_state.builtinCells->findObject(constructor) != parent)
        return nullptr;
    uint32_t rank = m_tables.rankOfClass(classLayout->index);
    if (Symbol* brand = m_brandsOfHeirs[rank])
        return brand;
    Symbol* brand = Symbol::create(vm, PrivateSymbolImpl::create(*StringImpl::empty()).get());
    m_state.immortalCells->add(brand);
    m_brandsOfHeirs[rank] = brand;
    return brand;
}

Structure* ClassTable::structureWithBrandOfHeirs(VM& vm, JSCell* parent, JSCell* newTarget, Structure* structure)
{
    StructureWithBrandOfHeirs& last = m_structuresWithBrandsOfHeirs[indexOfStructureWithBrandOfHeirs(newTarget)];
    if (last.withoutBrand == structure && last.newTarget == newTarget && last.parent == parent) [[likely]]
        return last.withBrand;
    Symbol* brand = brandOfHeirsFor(vm, parent, newTarget);
    bool holdsProperties = brand && m_tables.at<PropertiesOfHeirsData>(uncheckedDowncast<Closure>(newTarget)->classData()->classData().propertiesOfHeirsOffset)->numberOfNames;
    auto* classObject = dynamicDowncast<Closure>(newTarget);
    bool isOfClass = classObject && classObject->classData() && isDefinedByClass(*classObject->classData());
    last = { parent, newTarget, structure, brand ? Structure::setBrandTransition(vm, structure, brand) : structure, nullptr, nullptr, 0, holdsProperties, isOfClass };
    if (isOfClass && (structure->typeInfo().type() == JSMapType || structure->typeInfo().type() == JSSetType))
        cacheEmptyCollectionTemplate(vm, parent, newTarget, structure, last.withBrand);
    if (brand && isTypedArrayType(structure->typeInfo().type()) && !isResizableOrGrowableSharedTypedArrayIncludingDataView(structure->classInfoForCells()))
        rememberTemplateOfViews(parent, newTarget, structure, last.withBrand);
    return last.withBrand;
}

void ClassTable::rememberTemplateOfViews(JSCell* parent, JSCell* newTarget, Structure* withoutBrand, Structure* structure)
{
    TypedArrayType type = typedArrayType(structure->typeInfo().type());
    prepareTypedArrayAllocation(&m_state.instance, type);
    HeirTemplate& made = m_heirTemplates[subclassTemplateIndex(newTarget)];
    made = { newTarget, parent, nullptr, 0, 0, withoutBrand->id().bits(), HeirTemplate::isOfViews, { } };
    made.headerOfViews = cellHeaderFor(structure);
    made.typedArrayType = type;
}

void ClassTable::cacheEmptyCollectionTemplate(VM& vm, JSCell* parent, JSCell* newTarget, Structure* withoutBrand, Structure* structure)
{
    static_assert(sizeof(JSMap) == sizeof(JSSet) && !(sizeof(JSMap) % sizeof(uint64_t)) && sizeof(JSMap) <= HeirTemplate::maxWords * sizeof(uint64_t));
    bool isMap = structure->typeInfo().type() == JSMapType;
    Allocator allocator = isMap ? allocatorForConcurrently<JSMap>(vm, sizeof(JSMap), AllocatorForMode::EnsureAllocator) : allocatorForConcurrently<JSSet>(vm, sizeof(JSSet), AllocatorForMode::EnsureAllocator);
    if (!allocator)
        return;
    HeirTemplate& made = m_heirTemplates[subclassTemplateIndex(newTarget)];
    made = { newTarget, parent, std::bit_cast<const uint8_t*>(allocator.localAllocator()), 0, 0, withoutBrand->id().bits(), static_cast<uint32_t>(sizeof(JSMap) / sizeof(uint64_t)) | HeirTemplate::takesNoArguments, { } };
    std::ranges::fill(made.words, 0);
    made.words[0] = cellHeaderFor(structure);
}

static Structure* structureOfShape(VM& vm, Structure* structure, IndexingType shape)
{
    RELEASE_ASSERT(shape == Int32Shape || shape == DoubleShape || shape == ContiguousShape);
    TransitionKind kind = shape == Int32Shape ? TransitionKind::AllocateInt32 : shape == DoubleShape ? TransitionKind::AllocateDouble : TransitionKind::AllocateContiguous;
    DeferredStructureTransitionWatchpointFire deferred(vm, structure);
    return Structure::nonPropertyTransition(vm, structure, kind, &deferred);
}

JSArray* ClassTable::tryCreateArrayOfHeir(VM& vm, JSCell* newTarget, Structure* structure, unsigned length)
{
    StructureWithBrandOfHeirs& last = m_structuresWithBrandsOfHeirs[indexOfStructureWithBrandOfHeirs(newTarget)];
    if (structure->indexingType() != ArrayWithUndecided || last.withBrand != structure || last.newTarget != newTarget)
        return JSArray::tryCreate(vm, structure, length);
    if (JSArray* newest = std::exchange(last.newestArray, nullptr)) {
        IndexingType shape = newest->indexingType() & IndexingShapeMask;
        if (shape == UndecidedShape)
            shape = Int32Shape;
        if (shape == Int32Shape || shape == DoubleShape || shape == ContiguousShape)
            last.learnedShape = std::max(last.learnedShape, shape);
    }
    JSArray* array = nullptr;
    unsigned propertyRoom = last.holdsProperties ? propertyRoomOfArray : 0;
    if (last.learnedShape) {
        if (!last.ofNewArrays || (last.ofNewArrays->indexingType() & IndexingShapeMask) != last.learnedShape)
            last.ofNewArrays = structureOfShape(vm, structure, last.learnedShape);
        array = tryCreateArrayWithStorage(vm, last.ofNewArrays, length, (length ? length : BASE_CONTIGUOUS_VECTOR_LEN_EMPTY) | 1, propertyRoom);
    }
    bool isOfTemplate = array;
    if (!array)
        array = JSArray::tryCreate(vm, structure, length);
    if (last.withBrand != structure || last.newTarget != newTarget)
        return array;
    last.newestArray = array;
    if (isOfTemplate && last.isOfClass) {
        constexpr uint32_t allocationsBetweenReviews = 64;
        static_assert(!(propertyRoomOfArray % MarkedSpace::sizeStep) && sizeof(JSArray) == MarkedSpace::sizeStep);
        uint64_t hole = last.learnedShape == DoubleShape ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
        const uint8_t* allocators = std::bit_cast<const uint8_t*>(&vm.cellSpace()) + CompleteSubspace::offsetOfAllocatorForSizeStep() + (2 + propertyRoom / MarkedSpace::sizeStep) * sizeof(Allocator);
        m_heirTemplates[subclassTemplateIndex(newTarget)] = { newTarget, last.parent, allocators, propertyRoom, allocationsBetweenReviews, last.withoutBrand->id().bits(), 0, { { cellHeaderFor(last.ofNewArrays), hole } } };
    }
    return array;
}

JSArray* ClassTable::tryCreateRememberedHeir(VM& vm, JSCell* parent, JSCell* newTarget, unsigned length)
{
    const StructureWithBrandOfHeirs& last = m_structuresWithBrandsOfHeirs[indexOfStructureWithBrandOfHeirs(newTarget)];
    if (last.newTarget != newTarget || last.parent != parent || !last.isOfClass || !last.learnedShape)
        return nullptr;
    FunctionRareData* rareData = uncheckedDowncast<JSFunction>(newTarget)->rareData();
    if (!rareData || rareData->internalFunctionAllocationStructure() != last.withoutBrand)
        return nullptr;
    return tryCreateArrayOfHeir(vm, newTarget, last.withBrand, length);
}

Structure* RuntimeHooks::addBrandForSuperCall(VM& vm, JSFunction* newTarget, Structure* structure)
{
    VMState& vmState = vm.aotState();
    vmState.newTargetOfSuperCall = nullptr;
    Instance* instance = Instance::owning(newTarget);
    State* state = instance ? instance->state : nullptr;
    return state && state->objects ? state->objects->classes.structureWithBrandOfHeirs(vm, vmState.parentOfSuperCall, newTarget, structure) : structure;
}

const LayoutData* ClassTable::classOf(const ClassPrototype& prototype) const
{
    return classWithIndex(prototype.classNumber());
}

const LayoutData* ClassTable::classWithIndex(uint32_t index) const
{
    uint32_t offset = m_tables.offsetOfClass(index);
    return offset ? m_tables.at<LayoutData>(offset) : nullptr;
}

FunctionID ClassTable::functionOfOwnClosure(const Closure& closure) const
{
    RELEASE_ASSERT(m_tables.hasClosureNumber(closure.closureNumber()));
    return FunctionID(m_tables.closureFunctionWithNumber(closure.closureNumber()));
}

static constexpr uint64_t isAnswered = 1;

void ClassTable::rememberClassConstructedBy(FunctionID function, const LayoutData& classLayout) const
{
    uint32_t offset = m_tables.offsetOf(&classLayout);
    ASSERT(offset == findOffsetOfClassConstructedBy(m_tables, function));
    m_classesOfFunctionsAsked[function.index() % numberOfClassesOfFunctionsAsked].store(static_cast<uint64_t>(function.index()) << 32 | isAnswered | offset, std::memory_order_relaxed);
}

const LayoutData* ClassTable::classConstructedBy(FunctionID function) const
{
    static_assert(alignof(LayoutData) > 1);
    std::atomic<uint64_t>& place = m_classesOfFunctionsAsked[function.index() % numberOfClassesOfFunctionsAsked];
    uint64_t question = static_cast<uint64_t>(function.index()) << 32 | isAnswered;
    uint64_t answer = place.load(std::memory_order_relaxed);
    if ((answer & (UINT64_MAX << 32 | isAnswered)) != question) {
        answer = question | findOffsetOfClassConstructedBy(m_tables, function);
        place.store(answer, std::memory_order_relaxed);
    }
    uint32_t offset = static_cast<uint32_t>(answer & ~isAnswered);
    return offset ? m_tables.at<LayoutData>(offset) : nullptr;
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

bool ClassTable::sharesStructureOfRootClasses(Structure* structure) const
{
    return structure && structure == m_classObjectStructures.get(m_state.instance.globalObject->functionPrototype());
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
    if (!canBeDefinedFromRecord(classLayout))
        return created;
    ASSERT(hasOneClassObject(classLayout));
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    defineClass(m_state.instance.globalObject, created, classLayout, JSValue(), JSValue());
    scope.assertNoException();
    return created;
}

bool ClassTable::hasStaticMethodNamed(const LayoutData& classLayout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> key = classLayout.staticMethods().empty() ? std::nullopt : keyOf(m_state, name);
    return key && m_state.objects->layouts.staticMethod(classLayout, *key);
}

const FieldData* ClassTable::staticField(const LayoutData& classLayout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> key = classLayout.staticFields().empty() ? std::nullopt : keyOf(m_state, name);
    return key ? m_state.objects->layouts.staticField(classLayout, *key) : nullptr;
}

uint32_t& ClassTable::createdStaticFieldsEnd(const LayoutData& classLayout)
{
    if (m_createdStaticFieldsEnds.isEmpty())
        m_createdStaticFieldsEnds.fill(0, m_tables.numberOfClasses());
    return m_createdStaticFieldsEnds[m_tables.rankOfClass(classLayout.index)];
}

void ClassTable::didEmptyStaticFields(const LayoutData& classLayout)
{
    m_classesWithKeptStaticFields.clear(m_tables.rankOfClass(classLayout.index));
    if (!m_createdStaticFieldsEnds.isEmpty())
        m_createdStaticFieldsEnds[m_tables.rankOfClass(classLayout.index)] = 0;
}

void ClassTable::keepStaticFieldsInVariables(const LayoutData& classLayout)
{
    m_classesWithKeptStaticFields.set(m_tables.rankOfClass(classLayout.index));
    m_leavesSomeStaticFields = true;
}

bool ClassTable::hasStaticFieldsLeft(const LayoutData& classLayout) const
{
    return m_classesWithKeptStaticFields.get(m_tables.rankOfClass(classLayout.index));
}

JSValue ClassTable::readStaticField(const FieldData& field) const
{
    return ProgramStatics::readVariable(m_state.instance, field.slot);
}

void ClassTable::storeStaticField(const FieldData& field, JSValue value)
{
    ProgramStatics::writeVariable(m_state.instance, field.slot, value);
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

bool ClassTable::structureTellsInstancesOf(const LayoutData& classLayout, JSCell* cell, std::span<int32_t> displacementsOfFields) const
{
    uint32_t offsetOfProperties = isDefinedByClass(classLayout) && classLayout.evaluation() == EvaluationState::ProvenOnce ? classLayout.classData().propertiesOfHeirsOffset : 0;
    JSObject* prototype = offsetOfProperties ? classPrototypeIfExists(classLayout) : nullptr;
    if (!prototype || !cell->isObject())
        return false;

    const PropertiesOfHeirsData& properties = *m_tables.at<PropertiesOfHeirsData>(offsetOfProperties);
    RELEASE_ASSERT(displacementsOfFields.size() == properties.numberOfNames);
    const BuiltinClass& builtinClass = BuiltinTable::singleton().at(BuiltinClassID(properties.builtinClass()));
    Structure* structure = cell->structure();
    TypeInfo typeInfo = structure->typeInfo();
    if (builtinClass.instanceTest != BuiltinInstanceTest::JSTypeEquals || typeInfo.type() != builtinClass.type)
        return false;
    if (structure->isDictionary() || structure->hasPolyProto() || structure->realm() != m_state.instance.globalObject || structure->storedPrototype() != JSValue(prototype))
        return false;
    if (structure->isWatchingReplacement() || structure->mayBePrototype() || structure->mayHaveImmutableProperties() || isCopyOnWrite(structure->indexingMode()))
        return false;
    Symbol* brand = m_brandsOfHeirs[m_tables.rankOfClass(classLayout.index)];
    if (!brand || !structure->isBrandedStructure() || !uncheckedDowncast<BrandedStructure>(structure)->checkBrand(brand))
        return false;
    const BuiltinTable& table = BuiltinTable::singleton();
    bool isArray = builtinClass.id == BuiltinClasses::Array;
    bool isError = table.isSameOrSubclassOf(builtinClass.id, BuiltinClasses::Error);
    bool isView = isTypedView(table.typedArrayTypeOf(builtinClass.id));
    if (typeInfo.hasStaticPropertyTable() || typeInfo.getOwnPropertySlotIsImpure() || typeInfo.prohibitsPropertyCaching() || ((typeInfo.overridesGetOwnPropertySlot() || typeInfo.overridesPut()) && !isArray && !isError && !isView))
        return false;
    BuiltinID prototypeOfBase = builtinClass.prototype ? table.canonicalID(builtinClass.prototype) : BuiltinID();
    const BuiltinCells& builtinCells = *m_state.builtinCells;
    if (!prototypeOfBase || (!properties.isBelowMixin() && builtinCells.builtinsToLock().get(prototypeOfBase.index()) && prototype->getPrototypeDirect() != JSValue(builtinCells.lockedObjects().get(prototypeOfBase.index()))))
        return false;
    auto mayLieOutsideStructure = [&](UniquedStringImpl* uid) {
        if (isArray)
            return equal(uid, "length"_s) || isCanonicalNumericIndexString(uid);
        if (isView)
            return isCanonicalNumericIndexString(uid);
        if (!isError)
            return false;
        for (BuiltinClassID current = builtinClass.id; current; current = table.at(current).parent) {
            if (!table.at(current).listsNamesDefinedOnNewInstances)
                return true;
            for (BuiltinName name : table.at(current).namesDefinedOnNewInstances) {
                if (equal(uid, byteCast<Latin1Character>(table.textOf(name))))
                    return true;
            }
        }
        return false;
    };

    ProgramIdentifiers& identifiers = identifiersOf(m_state.instance);
    for (const MethodData& method : classLayout.methods()) {
        if (method.flags & methodHasComputedName)
            return false;
        unsigned attributes = 0;
        std::optional<PropertyOffset> own = structure->getWithoutMaterializing(identifiers.uid(method.name), attributes);
        if (!own || isValidOffset(*own) || mayLieOutsideStructure(identifiers.uid(method.name)))
            return false;
    }
    Vector<int32_t, 16> displacementsInStructure;
    for (uint32_t name : properties.names()) {
        if (name & PropertiesOfHeirsData::mayBeStoredLate) {
            displacementsInStructure.append(0);
            continue;
        }
        unsigned attributes = 0;
        std::optional<PropertyOffset> own = structure->getWithoutMaterializing(identifiers.uid(name), attributes);
        if (!own || !isOutOfLineOffset(*own) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || mayLieOutsideStructure(identifiers.uid(name)))
            return false;
        int32_t displacement = static_cast<int32_t>(offsetInButterfly(*own) * static_cast<intptr_t>(sizeof(EncodedJSValue)));
        int32_t learned = displacementsOfFields[displacementsInStructure.size()];
        if (learned && learned != displacement)
            return false;
        displacementsInStructure.append(displacement);
    }
    auto isListedField = [&](UniquedStringImpl* uid) {
        return std::ranges::any_of(properties.names(), [&](uint32_t name) {
            return identifiers.uid(name & ~PropertiesOfHeirsData::mayBeStoredLate) == uid;
        });
    };
    const MembersAboveHeirs& membersAbove = *m_membersAboveHeirs.ensure(properties.builtinClass(), [&] {
        auto members = makeUnique<MembersAboveHeirs>();
        for (BuiltinID holder = prototypeOfBase; holder;) {
            for (const Builtin& member : table.memberEntriesOf(holder)) {
                if (table.isSymbolName(member.name) || member.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
                    continue;
                Identifier name = table.identifierOf(*m_state.instance.vm, member.name);
                members->canBeToldByStructure &= !mayLieOutsideStructure(name.impl());
                members->names.append(name.impl());
            }
            const Builtin& object = table.at(holder);
            members->canBeToldByStructure &= object.prototypeLink != BuiltinPrototypeLink::Unknown;
            holder = object.prototypeLink == BuiltinPrototypeLink::Builtin ? table.canonicalID(object.inheritsFrom) : BuiltinID();
        }
        return members;
    }).iterator->value;
    if (!membersAbove.canBeToldByStructure)
        return false;
    unsigned attributesOfIterator = 0;
    std::optional<PropertyOffset> iterator = structure->getWithoutMaterializing(m_state.instance.vm->propertyNames->iteratorSymbol.impl(), attributesOfIterator);
    if (!iterator || isValidOffset(*iterator))
        return false;
    if (isArray) {
        bool namesElement = false;
        structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
            namesElement |= isCanonicalNumericIndexString(entry.key());
            return !namesElement;
        });
        if (namesElement)
            return false;
    }
    for (const RefPtr<UniquedStringImpl>& name : membersAbove.names) {
        unsigned attributes = 0;
        std::optional<PropertyOffset> own = structure->getWithoutMaterializing(name.get(), attributes);
        if (!own || (isValidOffset(*own) && !isListedField(name.get())))
            return false;
    }
    std::ranges::copy(displacementsInStructure, displacementsOfFields.begin());
    return true;
}

bool ClassTable::rememberStructureOfInstances(VM& vm, JSCell* cell, const LayoutData& classLayout, uint32_t offsetOfStructures)
{
    uint32_t offsetOfProperties = classLayout.classData().propertiesOfHeirsOffset;
    RELEASE_ASSERT(offsetOfProperties);
    size_t numberOfFields = m_tables.at<PropertiesOfHeirsData>(offsetOfProperties)->numberOfNames;
    RELEASE_ASSERT(!(offsetOfStructures % alignof(StructuresOfHeir)) && offsetOfStructures + StructuresOfHeir::sizeWithDisplacementsOfFields(numberOfFields) <= m_tables.instanceSizes.sizeOfStatics);
    auto& structures = *reinterpret_cast<StructuresOfHeir*>(m_state.instance.statics() + offsetOfStructures);
    uint32_t structureID = cell->structureID().bits();
    VerdictOnStructureOfHeir& verdict = m_newestVerdictsOnStructuresOfHeirs[indexOfVerdictOnStructureOfHeir(structureID, offsetOfStructures)];
    if (verdict.speaksOf(structureID, offsetOfStructures)) {
        structures.layAtBack(structureID, verdict.passes());
        return verdict.passes();
    }
    uint64_t keyOfVerdict = static_cast<uint64_t>(structureID) << 32 | offsetOfStructures;
    if (auto earlier = m_verdictsOnStructuresOfHeirs.find(keyOfVerdict); earlier != m_verdictsOnStructuresOfHeirs.end()) {
        verdict = { structureID, static_cast<uint32_t>(*earlier) };
        structures.layAtBack(structureID, verdict.passes());
        return verdict.passes();
    }
    countEvent(vm, RuntimeCounterID::BuiltinSubclassStructuresExamined);
    bool tells = structureTellsInstancesOf(classLayout, cell, structures.displacementsOfFields(numberOfFields));
    verdict = { structureID, offsetOfStructures | (tells ? VerdictOnStructureOfHeir::passesBit : 0) };
    m_verdictsOnStructuresOfHeirs.add(keyOfVerdict | (tells ? VerdictOnStructureOfHeir::passesBit : 0));
    if (structures.holdsNoStructure())
        m_offsetsOfStructuresOfHeirsInUse.append(offsetOfStructures);
    structures.layInFront(structureID, tells);
    return tells;
}

bool ClassTable::rememberPrototypeWithoutElements(JSCell* cell, uint32_t offsetOfStructures)
{
    JSObject* prototype = cell->structure()->storedPrototypeObject();
    Structure* structure = prototype->structure();
    if (structure->isDictionary() || structure->hasPolyProto() || hasIndexedProperties(prototype->indexingType()) || structure->mayInterceptIndexedAccesses() || structure->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero())
        return false;
    if (structure->storedPrototype() != JSValue(m_state.instance.globalObject->arrayPrototype()))
        return false;
    bool namesElement = false;
    structure->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        namesElement |= isCanonicalNumericIndexString(entry.key());
        return !namesElement;
    });
    if (namesElement)
        return false;
    reinterpret_cast<StructuresOfHeir*>(m_state.instance.statics() + offsetOfStructures)->prototypeWithoutElements = structure->id().bits();
    return true;
}

void ClassTable::forgetStructuresOfHeirs()
{
    if (m_offsetsOfStructuresOfHeirsInUse.isEmpty())
        return;
    for (uint32_t offset : m_offsetsOfStructuresOfHeirsInUse)
        *reinterpret_cast<StructuresOfHeir*>(m_state.instance.statics() + offset) = { };
    m_offsetsOfStructuresOfHeirsInUse.shrink(0);
    m_newestVerdictsOnStructuresOfHeirs.fill({ });
    m_verdictsOnStructuresOfHeirs.clear();
}

JSObject* ClassTable::classPrototypeOfInstances(VM& vm, const LayoutData& classLayout)
{
    JSObject* prototype = classPrototype(vm, classLayout);
    beginGuards(classLayout);
    return prototype;
}

JSObject* ClassTable::classPrototype(VM& vm, const LayoutData& classLayout)
{
    if (JSObject* prototype = classPrototypeIfExists(classLayout))
        return prototype;
    if (isClassOfCreatedObjects(classLayout))
        return nullptr;
    if (classLayout.evaluation() == EvaluationState::PredictedOnce) {
        if (!isDefinedByClass(classLayout))
            return defineClassOfConstructorFunction(vm, classLayout);
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
        return createClassPrototype(vm, classLayout, parent);
    }
    if (prototypeAwaitsFirstTouch(classLayout)) {
        JSObject* parent = classPrototype(vm, *assumedBaseOf(classLayout));
        RELEASE_ASSERT(parent);
        parent->didBecomePrototype(vm);
        return createClassPrototype(vm, classLayout, parent);
    }
    if (isDefinedByClass(classLayout) && !canBeDefinedFromRecord(classLayout))
        return nullptr;
    if (hasOneClassObject(classLayout))
        m_state.objects->closures.closure(vm, FunctionID(classLayout.classData().classConstructor));
    if (!isDefinedByClass(classLayout))
        return defineClassOfConstructorFunction(vm, classLayout);
    return classPrototypeIfExists(classLayout);
}

void ClassTable::didCreatePrototypePropertyOf(Closure* function)
{
    FunctionID constructor = functionOfOwnClosure(*function);
    const LayoutData* classLayout = classConstructedBy(constructor);
    if (!classLayout || isDefinedByClass(*classLayout) || classLayout->evaluation() != EvaluationState::PredictedOnce)
        return;
    ClosureTable& closures = m_state.objects->closures;
    if (constructor != FunctionID(classLayout->classData().classConstructor) || !closures.hasCellFor(constructor))
        return;
    JSCell* registered = closures.closureIfCreated(constructor);
    if (!registered || registered == function) {
        closures.registerClassObject(function);
        return;
    }
    m_constructorFunctionsWithoutClass.add(classLayout->index);
    disarmMemberGuards(*classLayout);
}

JSObject* ClassTable::defineClassOfConstructorFunction(VM& vm, const LayoutData& classLayout)
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

    registerClassPrototype(classLayout, prototype);
    prototype->structure()->dropTransitionChain(vm);
    if (const LayoutData* base = assumedBaseOf(classLayout))
        classPrototypeOfInstances(vm, *base);
    armMemberGuards(vm, classLayout, ClassSide::Prototype, prototype);
    armMemberGuards(vm, classLayout, ClassSide::Static, classObject);
    checkParentOfPrototypeObject(classLayout, prototype);
    return prototype;
}

void ClassTable::checkParentOfPrototypeObject(const LayoutData& classLayout, JSObject* prototype)
{
    const LayoutData* base = assumedBaseOf(classLayout);
    JSObject* assumedParent = base ? classPrototypeIfExists(*base) : m_state.instance.globalObject->objectPrototype();
    if (!assumedParent || prototype->getPrototypeDirect() != JSValue(assumedParent))
        didDefineWithOtherParent(classLayout, ClassSide::Prototype);
}

void ClassTable::registerClassPrototype(const LayoutData& classLayout, JSObject* prototype)
{
    ASSERT((classLayout.evaluation() == EvaluationState::ProvenOnce || !isDefinedByClass(classLayout)) && !classPrototypeIfExists(classLayout));
    FunctionID constructor(classLayout.classData().classConstructor);
    auto evaluation = constructor ? m_evaluationsHeldWhileReached.find(constructor.index()) : m_evaluationsHeldWhileReached.end();
    if (evaluation == m_evaluationsHeldWhileReached.end() || evaluation->value.rank != m_tables.rankOfClass(classLayout.index)) {
        prototypeCellOf(m_state, classLayout) = prototype;
        m_state.immortalCells->add(prototype);
        return;
    }
    Locker locker { m_state.caches->lock() };
    prototypeCellOf(m_state, classLayout) = prototype;
    m_constructorsOfEvaluationsChangedSinceCollection.append(constructor.index());
}

bool ClassTable::tryHoldClassObjectWhileReached(FunctionID function, JSCell* classObject)
{
    const LayoutData* constructed = classConstructedBy(function);
    return constructed && tryHoldClassObjectWhileReached(*constructed, function, classObject);
}

bool ClassTable::tryHoldClassObjectWhileReached(const LayoutData& constructed, FunctionID function, JSCell* classObject)
{
    if (!livesWhileReached(constructed) || FunctionID(constructed.classData().classConstructor) != function)
        return false;
    if (!m_markOfEvaluationThatDied) {
        JSObject* mark = constructEmptyObject(*m_state.instance.vm, m_state.instance.globalObject->nullPrototypeObjectStructure());
        m_state.immortalCells->add(mark);
        m_markOfEvaluationThatDied = mark;
    }
    Locker locker { m_state.caches->lock() };
    bool isFirst = m_evaluationsHeldWhileReached.add(function.index(), EvaluationHeldWhileReached { classObject, m_tables.rankOfClass(constructed.index) }).isNewEntry;
    RELEASE_ASSERT(isFirst);
    m_constructorsOfEvaluationsChangedSinceCollection.append(function.index());
    return true;
}

template<typename Visitor>
void ClassTable::visitRoots(Visitor& visitor)
{
    auto visitPrototypeOf = [&](const EvaluationHeldWhileReached& evaluation) {
        if (!evaluation.classObject || evaluation.classObject == m_markOfEvaluationThatDied)
            return;
        JSCell* prototype = m_state.instance.classes.classPrototypes[evaluation.rank];
        if (prototype && visitor.isMarked(evaluation.classObject))
            visitor.appendUnbarriered(prototype);
    };
    if (visitor.collectionScope() == CollectionScope::Eden) {
        for (uint32_t constructor : m_constructorsOfEvaluationsChangedSinceCollection)
            visitPrototypeOf(m_evaluationsHeldWhileReached.get(constructor));
        return;
    }
    for (auto& evaluation : m_evaluationsHeldWhileReached.values())
        visitPrototypeOf(evaluation);
}

template void ClassTable::visitRoots(AbstractSlotVisitor&);
template void ClassTable::visitRoots(SlotVisitor&);

void ClassTable::endEvaluationsThatDied(VM& vm, CollectionScope scope)
{
    auto endWhatDied = [&](EvaluationHeldWhileReached& evaluation) {
        JSCell*& classObject = evaluation.classObject;
        if (!vm.heap.isMarked(classObject))
            classObject = m_markOfEvaluationThatDied;
        JSCell*& prototype = m_state.instance.classes.classPrototypes[evaluation.rank];
        if (prototype && !vm.heap.isMarked(prototype))
            prototype = nullptr;
    };
    if (scope == CollectionScope::Eden) {
        for (uint32_t constructor : m_constructorsOfEvaluationsChangedSinceCollection) {
            if (auto entry = m_evaluationsHeldWhileReached.find(constructor); entry != m_evaluationsHeldWhileReached.end())
                endWhatDied(entry->value);
        }
    } else {
        for (auto& evaluation : m_evaluationsHeldWhileReached.values())
            endWhatDied(evaluation);
    }
    m_constructorsOfEvaluationsChangedSinceCollection.shrink(0);
}

void ClassTable::clearDeadAssignedMembers(VM& vm, CollectionScope scope)
{
    auto clearIfDead = [&](uint32_t offset) {
        EncodedJSValue& word = m_state.statics->at<EncodedJSValue>(offset);
        if (JSValue member = JSValue::decode(word); member.isCell() && !vm.heap.isMarked(member.asCell()))
            word = JSValue::encode(jsUndefined());
    };
    if (scope == CollectionScope::Eden) {
        for (uint32_t offset : m_newAssignedMembers)
            clearIfDead(offset);
    } else {
        for (const AssignedMemberData& member : m_tables.assignedMembers())
            clearIfDead(byteOffsetOfClassGuardBit(member.guardBit));
    }
    m_newAssignedMembers.shrink(0);
}

bool ClassTable::isLaterEvaluation(const Closure& classObject, const LayoutData& classLayout) const
{
    return classLayout.evaluation() != EvaluationState::ProvenOnce && m_state.objects->closures.closureIfCreated(functionOfOwnClosure(classObject)) != &classObject;
}

bool ClassTable::keepsStructuresForNextEvaluation(const Closure& classObject, const LayoutData& classLayout) const
{
    ASSERT(&classLayout == classObject.classData());
    if (classLayout.evaluation() == EvaluationState::ProvenOnce)
        return false;
    if (isLaterEvaluation(classObject, classLayout))
        return true;
    auto isCreatedAtDefinition = [](const MethodData& method) {
        return !!(method.flags & methodIsCreatedAtDefinition);
    };
    return std::ranges::none_of(classLayout.methods(), isCreatedAtDefinition) && std::ranges::none_of(classLayout.staticMethods(), isCreatedAtDefinition);
}

size_t ClassTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    forgetInstancesBelowPrototypes();
    forgetStructuresOfHeirs();
    m_structuresWithBrandsOfHeirs.fill({ });
    m_heirTemplates.fill({ });
    endEvaluationsThatDied(vm, scope);
    clearDeadAssignedMembers(vm, scope);
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

static GetterSetter* createDeclaredGetterSetter(VM& vm, State& state, const ClassTable::DeclaredMember& accessor)
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

static std::optional<ClassTable::DeclaredMember> findOwnDeclaredMember(const ClassTable& classes, const LayoutData& classLayout, std::optional<uint32_t> key)
{
    if (!classes.hasOneClassObject(classLayout))
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
    std::optional<uint32_t> key = keyOf(m_state, name);
    return key && declaresMember(classLayout, ClassSide::Prototype, *key);
}

std::optional<JSObject*> ClassTable::findDeclaredMethod(VM& vm, const LayoutData& classLayout, UniquedStringImpl* name)
{
    auto member = findOwnDeclaredMember(*this, classLayout, keyOf(m_state, name));
    if (!member || member->isAccessor())
        return std::nullopt;
    if (!member->method && name != vm.propertyNames->constructor.impl())
        return static_cast<JSObject*>(nullptr);
    return closureObject(vm, m_state, FunctionID(member->method ? member->method->function : classLayout.classData().classConstructor));
}

static FunctionID keyOfDeclaredAccessor(const ClassTable::DeclaredMember& accessor)
{
    ASSERT(accessor.isAccessor());
    return FunctionID((accessor.getter ? accessor.getter : accessor.setter)->function);
}

static bool structureProvesAbsenceBelowLazyPrototypes(VM& vm, JSObject* base, Structure* structure, UniquedStringImpl* name)
{
    if (compiledLayoutOrNull(structure))
        return Object::structureGuaranteesAbsence(structure, name);
    return base->type() == FinalObjectType && !structure->isDictionary() && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence()
        && !structure->typeInfo().overridesGetOwnPropertySlot() && !isValidOffset(structure->get(vm, name));
}

std::optional<ClassTable::LazyPrototypeLookupResult> ClassTable::findThroughLazyPrototypes(VM& vm, JSObject* base, UniquedStringImpl* name)
{
    Structure* structure = base->structure();
    JSObject* prototype = structure->hasMonoProto() ? structure->storedPrototypeObject() : nullptr;
    if (!prototype || prototype->classInfo() != ClassPrototype::info() || !structureProvesAbsenceBelowLazyPrototypes(vm, base, structure, name))
        return std::nullopt;
    return findFromLazyPrototype(vm, prototype, name, MemberCreation::Allowed);
}

std::optional<ClassTable::LazyPrototypeLookupResult> ClassTable::findFromLazyPrototype(VM& vm, JSObject* prototype, UniquedStringImpl* name, MemberCreation creation)
{
    auto resultAt = [](std::optional<DeclaredMemberLocation> location) -> std::optional<LazyPrototypeLookupResult> {
        if (!location)
            return std::nullopt;
        return LazyPrototypeLookupResult { location, nullptr };
    };
    if (name == vm.propertyNames->underscoreProto.impl() || parseIndex(PropertyName(name)))
        return std::nullopt;
    for (; prototype && prototype->classInfo() == ClassPrototype::info() && !prototype->butterfly(); prototype = prototype->getPrototypeDirect().getObject()) {
        if (Instance* owner = Instance::owning(prototype); owner != &m_state.instance) [[unlikely]]
            return owner->state->objects->classes.findFromLazyPrototype(vm, prototype, name, creation);
        const LayoutData& classLayout = *classOf(*uncheckedDowncast<ClassPrototype>(prototype));
        auto member = findOwnDeclaredMember(*this, classLayout, keyOf(m_state, name));
        if (!member)
            return std::nullopt;
        if (member->method)
            return resultAt(declaredMemberLocation(vm, FunctionID(member->method->function), nullptr, creation));
        if (member->isAccessor())
            return resultAt(declaredMemberLocation(vm, keyOfDeclaredAccessor(*member), &*member, creation));
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
    std::optional<uint32_t> known = m_declaredMemberLocations.getOptional(key.index());
    if (!known && creation == MemberCreation::Forbidden)
        return std::nullopt;
    if (!known) {
        JSCell* member = accessorOrNull ? static_cast<JSCell*>(createDeclaredGetterSetter(vm, m_state, *accessorOrNull)) : closureObject(vm, m_state, key);
        if (accessorOrNull)
            m_state.immortalCells->add(member);
        if (m_numberOfDeclaredMemberLocations == m_declaredMemberHolders.size() * declaredMembersPerHolder) {
            Structure* structure = m_declaredMemberHolders.isEmpty() ? JSFinalObject::createStructure(vm, m_state.instance.globalObject, jsNull(), declaredMembersPerHolder) : m_declaredMemberHolders[0]->structure();
            m_declaredMemberHolders.append(constructEmptyObject(vm, structure));
            m_state.immortalCells->add(m_declaredMemberHolders.last());
        }
        known = m_numberOfDeclaredMemberLocations++;
        m_declaredMemberHolders.last()->putDirectOffset(vm, *known % declaredMembersPerHolder, member);
        m_declaredMemberLocations.set(key.index(), *known);
    }
    return DeclaredMemberLocation { m_declaredMemberHolders[*known / declaredMembersPerHolder], static_cast<PropertyOffset>(*known % declaredMembersPerHolder), !!accessorOrNull };
}

GetterSetter* ClassTable::createDeclaredAccessor(VM& vm, const LayoutData& classLayout, UniquedStringImpl* name)
{
    auto member = findOwnDeclaredMember(*this, classLayout, keyOf(m_state, name));
    if (!member || !member->isAccessor())
        return nullptr;
    return createDeclaredGetterSetter(vm, m_state, *member);
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
            if (entry.value.prototypeStructureOfHolderLeftOut && !vm.heap.isMarked(entry.value.prototypeStructureOfHolderLeftOut))
                entry.value.prototypeStructureOfHolderLeftOut = nullptr;
            return !vm.heap.isMarked(entry.value.classObjectStructure) || !vm.heap.isMarked(entry.value.prototypeStructure);
        });
    });
    return sizeBefore - m_structures.size();
}

namespace {

using ClassPart = ClassTable::ClassPart;
using StaticFieldsOfPart = ClassTable::StaticFieldsOfPart;

class ClassDefinition {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(ClassDefinition);
public:
    ClassDefinition(ClassTable&, JSGlobalObject*, Closure* classObject, ClassPart, StaticFieldsOfPart = StaticFieldsOfPart::MoveToProperties);

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
    bool hasMemberEnvironment() const;

    void storeShared(JSObject* target, UniquedStringImpl* key, JSValue);

    bool createsClassObject() const { return m_part != ClassPart::PrototypeMembers; }
    bool createsPrototypeMembers() const { return m_part != ClassPart::ClassObject; }
    bool isShared() const { return !!m_shared; }
    JSCell* prototypeParentCell() const { return m_prototypeParent.isCell() ? m_prototypeParent.asCell() : nullptr; }

    ClassTable& m_classes;
    State& m_state;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    Closure* m_classObject;
    const LayoutData& m_classLayout;
    ClassPart m_part;
    const ClassPart m_requestedPart;
    const StaticFieldsOfPart m_staticFields;
    JSValue m_prototypeParent;
    JSObject* m_classObjectParent { nullptr };
    JSObject* m_prototype { nullptr };
    ClassStructures m_shared;
    bool m_mayShareStructures { false };
    bool m_hasOnlyPlainMembers { true };
    bool m_hasMembersCreatedAtDefinition { false };
    bool m_hasDefinedMembers { false };
    bool m_classObjectMayBePrototype { false };
    EarlyStores m_earlyStores;
    EarlyStores m_earlyPrototypeStores;
};

ClassDefinition::ClassDefinition(ClassTable& classes, JSGlobalObject* globalObject, Closure* classObject, ClassPart part, StaticFieldsOfPart staticFields)
    : m_classes(classes)
    , m_state(classObject->state())
    , m_vm(globalObject->vm())
    , m_globalObject(globalObject)
    , m_classObject(classObject)
    , m_classLayout(*classObject->classData())
    , m_part(part)
    , m_requestedPart(part)
    , m_staticFields(staticFields)
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
    if (keysAndAttributes.isEmpty())
        return;
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
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

bool ClassDefinition::hasMemberEnvironment() const
{
    if (m_classLayout.classData().environment != LayoutData::noBase)
        return m_classObject->captures().size() == 1 && m_classObject->environment() && uncheckedDowncast<Environment>(m_classObject->environment())->layout().index == m_classLayout.classData().environment;
    auto takesEnvironment = [](const MethodData& method) {
        return !(method.flags & methodIsCreatedAtDefinition) && (method.flags & methodTakesEnvironment);
    };
    return std::ranges::none_of(m_classLayout.methods(), takesEnvironment) && std::ranges::none_of(m_classLayout.staticMethods(), takesEnvironment);
}

void ClassDefinition::run(JSValue heritage, JSValue prototypeParent)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    ASSERT(m_requestedPart == ClassPart::Everything || m_classLayout.createsMembersLazily);
    ASSERT(!createsClassObject() || !m_classObject->getDirect(m_vm, m_vm.propertyNames->prototype));
    RELEASE_ASSERT(hasMemberEnvironment());

    if (resolveParents(heritage, prototypeParent) == ParentResolution::BaseNotDefined)
        return;
    chooseStructures();
    createPrototype();
    if (tryDeferClassObject()) {
        armGuards();
        return;
    }
    ClassTable::DefinitionFromRecord definition(m_classes, m_classLayout);
    prepareObjects();
    if (createsPrototypeMembers())
        defineMembers(m_prototype, m_classLayout.methods());
    if (!scope.exception() && createsClassObject())
        defineMembers(m_classObject, m_classLayout.staticMethods());
    if (!scope.exception() && m_requestedPart == ClassPart::ClassObject) {
        if (m_staticFields == StaticFieldsOfPart::StayInVariables && m_earlyStores.keysAndAttributes.isEmpty() && StaticFields::areReadInVariables(*m_classObject))
            m_classes.keepStaticFieldsInVariables(m_classLayout);
        else
            StaticFields::moveToProperties(m_vm, *m_classObject);
    }
    m_hasDefinedMembers = true;
    if (m_classObjectMayBePrototype)
        m_classObject->structure()->setMayBePrototype(true);
    if (createsPrototypeMembers() && !isShared())
        m_prototype->structure()->setMayBePrototype(true);
    if (scope.exception()) [[unlikely]]
        return;
    if (m_mayShareStructures && !isShared() && m_hasOnlyPlainMembers && !m_classes.sharesStructureOfRootClasses(m_classObject->structure()))
        m_state.objects->caches->classStructures.add(m_classLayout.index, m_classObjectParent, prototypeParentCell(), { m_classObject->structure(), m_prototype->structure() });
    m_earlyStores.restoreTo(m_globalObject, m_classObject, m_requestedPart == ClassPart::Everything ? StoreTime::BeforeDefinition : StoreTime::AfterDefinition);
    m_earlyPrototypeStores.restoreTo(m_globalObject, m_prototype, StoreTime::AfterDefinition);
    armGuards();
}

Closure* ClassDefinition::findBaseClassObject(const LayoutData& baseClass, JSValue heritage)
{
    ClosureTable& closures = m_state.objects->closures;
    FunctionID constructor(baseClass.classData().classConstructor);
    if (m_classes.hasOneClassObject(baseClass))
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
        m_prototypeParent = m_classes.classPrototype(m_vm, m_classLayout)->getPrototypeDirect();
    else if (wasDeclared) {
        m_classObjectParent = m_classObject->getPrototypeDirect().getObject();
        m_prototypeParent = m_classes.classPrototype(m_vm, m_classLayout)->getPrototypeDirect();
    } else if (const LayoutData* baseClass = m_classes.baseOf(m_classLayout)) {
        Closure* baseClassObject = findBaseClassObject(*baseClass, heritage);
        if (!baseClassObject && isDefinedBeforeEvaluation)
            return ParentResolution::BaseNotDefined;
        RELEASE_ASSERT(baseClassObject && m_classes.classConstructedBy(m_classes.functionOfOwnClosure(*baseClassObject)) == baseClass);
        m_classObjectParent = baseClassObject;
        m_prototypeParent = baseClassObject->prototypeProperty(m_vm);
        if (!m_prototypeParent && isDefinedBeforeEvaluation)
            return ParentResolution::BaseNotDefined;
        RELEASE_ASSERT(m_prototypeParent);
    } else if (heritage || m_classLayout.hasUntypedBase) {
        RELEASE_ASSERT(heritage && (heritage.isObject() || heritage.isNull()));
        RELEASE_ASSERT(prototypeParent && (prototypeParent.isObject() || prototypeParent.isNull()));
        m_classObjectParent = heritage.getObject();
        m_prototypeParent = prototypeParent;
    }
    if (m_prototypeParent.isObject())
        asObject(m_prototypeParent)->didBecomePrototype(m_vm);
    return ParentResolution::Resolved;
}

static Butterfly* allocateOutOfLineStorageOfClassHolder(VM& vm, Structure* structure)
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
    if (m_shared && (m_shared.classObjectStructure->realm() != m_globalObject || m_shared.classObjectStructure->storedPrototype() != (m_classObjectParent ? JSValue(m_classObjectParent) : m_classObject->getPrototypeDirect())))
        m_shared = { };
}

static Structure* createStructureOfOrdinaryPrototype(VM& vm, JSGlobalObject* globalObject, JSValue parent, unsigned inlineCapacity)
{
    Structure* structure = JSFinalObject::createStructure(vm, globalObject, parent, inlineCapacity);
    structure->setDidTransition(true);
    return structure;
}

void ClassDefinition::createPrototype()
{
    if (isShared()) {
        Structure* structure = m_shared.prototypeStructureOfHolderLeftOut && m_classes.holdersAreLeftOut(m_classLayout) ? m_shared.prototypeStructureOfHolderLeftOut : m_shared.prototypeStructure;
        m_prototype = JSFinalObject::createWithButterfly(m_vm, structure, allocateOutOfLineStorageOfClassHolder(m_vm, structure));
        return;
    }
    if (m_part == ClassPart::Everything) {
        m_prototype = constructEmptyObject(m_vm, createStructureOfOrdinaryPrototype(m_vm, m_globalObject, m_prototypeParent, JSFinalObject::defaultInlineCapacity));
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
        if (Butterfly* storage = allocateOutOfLineStorageOfClassHolder(m_vm, m_shared.classObjectStructure))
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
        Structure* ordinaryStructure = createStructureOfOrdinaryPrototype(m_vm, m_globalObject, m_prototypeParent, ClassPrototype::inlineCapacity);
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
        Identifier key = identifiersOf(m_state.instance).identifier(method.name);
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
    if (isShared() && m_prototype->structure() == m_shared.prototypeStructureOfHolderLeftOut) {
        m_classes.nameClassObjectOnPrototypeMadeLeftOut(m_vm, m_classLayout, m_classObject, m_prototype);
        return;
    }
    if (m_requestedPart == ClassPart::Everything && m_classes.tryLeaveHoldersOut(m_vm, m_classLayout, m_classObject, m_prototype)) {
        if (Structure* made = m_prototype->structure(); isShared() && !made->isDictionary() && made->previousID() == m_shared.prototypeStructure) {
            m_shared.prototypeStructureOfHolderLeftOut = made;
            m_state.objects->caches->classStructures.add(m_classLayout.index, m_classObjectParent, prototypeParentCell(), m_shared);
        }
        if (m_part == ClassPart::Everything)
            m_classes.didLeaveOutHoldersOfEvaluation(m_classLayout, m_prototype);
        return;
    }
    bool holdersHaveWhatRecordSays = m_hasDefinedMembers && m_hasOnlyPlainMembers && !m_hasMembersCreatedAtDefinition && !isShared() && m_earlyStores.keysAndAttributes.isEmpty() && m_earlyPrototypeStores.keysAndAttributes.isEmpty();
    HolderState stateOfPrototype = holdersHaveWhatRecordSays ? HolderState::AsDefinitionMadeIt : HolderState::Unknown;
    HolderState stateOfClassObject = holdersHaveWhatRecordSays && m_classLayout.staticFields().empty() ? HolderState::AsDefinitionMadeIt : HolderState::Unknown;
    bool hasAssumedBase = this->hasAssumedBase();
    if (const LayoutData* base = m_classes.assumedBaseOf(m_classLayout); base && hasAssumedBase)
        m_classes.classPrototypeOfInstances(m_vm, *base);
    m_classes.armMemberGuards(m_vm, m_classLayout, ClassSide::Prototype, m_prototype, createsPrototypeMembers() ? stateOfPrototype : HolderState::NotTouched);
    m_classes.armMemberGuards(m_vm, m_classLayout, ClassSide::Static, m_classObject, createsClassObject() ? stateOfClassObject : HolderState::NotTouched);
    if (!m_earlyPrototypeStores.keysAndAttributes.isEmpty())
        endClassGuard(m_state, m_classLayout, ClassSide::Prototype);
    if (!m_earlyStores.keysAndAttributes.isEmpty())
        endClassGuard(m_state, m_classLayout, ClassSide::Static);
    if (m_requestedPart != ClassPart::Everything || hasAssumedBase)
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
    if (auto* classObject = heritage.isCell() ? dynamicDowncast<Closure>(heritage.asCell()) : nullptr; classObject && classObject->kind().isConstructor) {
        if (JSValue prototype = classObject->getDirect(vm, vm.propertyNames->prototype))
            return prototype;
    }
    if (!heritage.isConstructor()) {
        throwException(globalObject, scope, createSuperclassNotConstructorError(globalObject));
        return { };
    }
    if (auto* classObject = dynamicDowncast<Closure>(heritage.asCell()); classObject && classObject->isLazyClassObject()) {
        if (JSObject* prototype = classPrototype(vm, *classObject->classData()))
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

static void registerFirstClassObjectOf(ClosureTable& closures, Closure* classObject, FunctionID constructor, const LayoutData& classLayout)
{
    if (FunctionID(classLayout.classData().classConstructor) == constructor) [[likely]]
        closures.registerFirstClassObject(classObject, constructor, classLayout);
    else
        closures.registerClassObject(classObject);
}

static void markFunctionPropertiesDeclaredAsFields(VM& vm, Closure* classObject, const LayoutData& classLayout)
{
    for (const FieldData& field : classLayout.staticFields()) {
        const Identifier& key = identifiersOfRealm(vm, classObject->structure()->realm())->identifier(field.name);
        if (key == vm.propertyNames->name)
            classObject->ensureRareData(vm)->setHasModifiedNameForBoundOrNonHostFunction();
        else if (key == vm.propertyNames->length)
            classObject->ensureRareData(vm)->setHasModifiedLengthForBoundOrNonHostFunction();
    }
}

bool ClassTable::tryLeaveDefinitionToRecord(VM& vm, Closure* classObject, const LayoutData& classLayout, JSValue heritage, JSValue prototypeParent, FunctionID constructor, JSCell* registered)
{
    if (!leavesDefinitionToRecord(classLayout))
        return false;
    if (classObject->butterfly() || classObject->mayBePrototype())
        return false;
    if (JSObject* prototype = classPrototypeIfExists(classLayout); prototype && (!classLayout.createsPrototypeBeforeClassObject || prototype->classInfo() != ClassPrototype::info()))
        return false;
    ClosureTable& closures = m_state.objects->closures;
    if (registered && registered != classObject)
        return false;

    Structure* structure = closures.closureStructure(vm, classObjectClosureStructure);
    if (classObject->structure() != structure)
        return false;
    JSObject* parentOfPrototype = nullptr;
    bool leavesPrototypeToFirstTouch = false;
    if (const LayoutData* baseClass = heritage ? assumedBaseOf(classLayout) : baseOf(classLayout)) {
        FunctionID baseConstructor(baseClass->classData().classConstructor);
        JSCell* baseClassObject = baseOf(classLayout) && hasOneClassObject(*baseClass) ? closures.closure(vm, baseConstructor) : closures.closureIfCreated(baseConstructor);
        if (!baseClassObject || isMarkOfEvaluationThatDied(baseClassObject) || (heritage && heritage != JSValue(baseClassObject)))
            return false;
        leavesPrototypeToFirstTouch = !classLayout.createsPrototypeBeforeClassObject;
        structure = lazyClassObjectStructureInheritingFrom(vm, asObject(baseClassObject));
    } else if (heritage || classLayout.hasUntypedBase) {
        if (!heritage || !heritage.isObject() || !prototypeParent || !prototypeParent.isObject() || classLayout.assumedBase() != LayoutData::noBase)
            return false;
        parentOfPrototype = asObject(prototypeParent);
        structure = lazyClassObjectStructureInheritingFrom(vm, asObject(heritage));
    }
    if (!parentOfPrototype && !classLayout.createsPrototypeBeforeClassObject && !leavesPrototypeToFirstTouch)
        return false;

    if (!registered)
        registerFirstClassObjectOf(closures, classObject, constructor, classLayout);
    if (!classLayout.staticFields().empty())
        markFunctionPropertiesDeclaredAsFields(vm, classObject, classLayout);
    classObject->setStructure(vm, structure);
    if (leavesPrototypeToFirstTouch)
        m_classesWhosePrototypeAwaitsFirstTouch.set(m_tables.rankOfClass(classLayout.index));
    if (parentOfPrototype) {
        parentOfPrototype->didBecomePrototype(vm);
        createClassPrototype(vm, classLayout, parentOfPrototype);
    }
    updateAssignedMembers(classLayout, ClassSide::Prototype);
    return true;
}

bool ClassTable::tryDefineAgainWithKeptStructures(JSGlobalObject* globalObject, Closure* classObject, const LayoutData& classLayout, JSValue heritage, JSValue prototypeParent)
{
    VM& vm = globalObject->vm();
    const ClassData& classData = classLayout.classData();
    if (classData.numberOfMethods || classData.numberOfStaticMethods || classLayout.assumedBase() != LayoutData::noBase)
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

    if (parentOfPrototype.isObject())
        asObject(parentOfPrototype)->didBecomePrototype(vm);
    Structure* structureOfHolderLeftOut = holdersAreLeftOut(classLayout) ? kept.prototypeStructureOfHolderLeftOut : nullptr;
    Structure* structureOfPrototype = structureOfHolderLeftOut ? structureOfHolderLeftOut : kept.prototypeStructure;
    JSObject* prototype = JSFinalObject::createWithButterfly(vm, structureOfPrototype, allocateOutOfLineStorageOfClassHolder(vm, structureOfPrototype));
    if (Butterfly* storage = allocateOutOfLineStorageOfClassHolder(vm, kept.classObjectStructure))
        classObject->nukeStructureAndSetButterfly(vm, classObject->structureID(), storage);
    classObject->setStructure(vm, kept.classObjectStructure);
    classObject->putDirectOffset(vm, offsetOfPrototype, prototype);
    prototype->putDirectOffset(vm, offsetOfConstructor, classObject);
    if (classData.numberOfStaticFields)
        markFunctionPropertiesDeclaredAsFields(vm, classObject, classLayout);
    if (structureOfHolderLeftOut) {
        nameClassObjectOnPrototypeMadeLeftOut(vm, classLayout, classObject, prototype);
        return true;
    }
    if (tryLeaveHoldersOut(vm, classLayout, classObject, prototype)) {
        if (Structure* made = prototype->structure(); !made->isDictionary() && made->previousID() == kept.prototypeStructure) {
            kept.prototypeStructureOfHolderLeftOut = made;
            m_state.objects->caches->classStructures.add(classLayout.index, classObjectParent, parentOfPrototype.isCell() ? parentOfPrototype.asCell() : nullptr, kept);
        }
        didLeaveOutHoldersOfEvaluation(classLayout, prototype);
        return true;
    }
    armMemberGuards(vm, classLayout, ClassSide::Prototype, prototype);
    armMemberGuards(vm, classLayout, ClassSide::Static, classObject);
    return true;
}

Closure* ClassTable::defineClassWhereItIsEvaluated(JSGlobalObject* globalObject, Closure* allocated, const LayoutData& classLayout, JSValue parent, JSValue prototypeParent)
{
    FunctionID constructor = functionOfOwnClosure(*allocated);
    JSCell* registered = m_state.objects->closures.closureIfCreated(constructor);
    auto* only = hasOneClassObject(classLayout) ? dynamicDowncast<Closure>(registered) : nullptr;
    Closure* classObject = only ? only : allocated;
    defineClass(globalObject, classObject, classLayout, parent, prototypeParent, constructor, registered);
    return classObject;
}

void ClassTable::defineClass(JSGlobalObject* globalObject, Closure* classObject, const LayoutData& classLayout, JSValue parent, JSValue prototypeParent)
{
    FunctionID constructor = functionOfOwnClosure(*classObject);
    defineClass(globalObject, classObject, classLayout, parent, prototypeParent, constructor, m_state.objects->closures.closureIfCreated(constructor));
}

void ClassTable::defineClass(JSGlobalObject* globalObject, Closure* classObject, const LayoutData& classLayout, JSValue parent, JSValue prototypeParent, FunctionID constructor, JSCell* registered)
{
    VM& vm = globalObject->vm();
    beginGuards(classLayout);
    ClosureTable& closures = m_state.objects->closures;
    if (FunctionID(classLayout.classData().classConstructor) == constructor) [[likely]]
        rememberClassConstructedBy(constructor, classLayout);
    if (tryLeaveDefinitionToRecord(vm, classObject, classLayout, parent, prototypeParent, constructor, registered))
        return;
    bool isProvenOnce = classLayout.evaluation() == EvaluationState::ProvenOnce;
    RELEASE_ASSERT(!isProvenOnce || !registered || registered == classObject);
    if (isProvenOnce && (classObject->getDirect(vm, vm.propertyNames->prototype) || (!classLayout.createsPrototypeBeforeClassObject && (classPrototypeIfExists(classLayout) || prototypeAwaitsFirstTouch(classLayout)))))
        return;
    if (registered && registered != classObject) {
        disarmMemberGuards(classLayout);
        if (tryDefineAgainWithKeptStructures(globalObject, classObject, classLayout, parent, prototypeParent))
            return;
    } else if (!registered && classLayout.evaluation() != EvaluationState::SeenTwice)
        registerFirstClassObjectOf(closures, classObject, constructor, classLayout);
    else if (!registered && tryDefineAgainWithKeptStructures(globalObject, classObject, classLayout, parent, prototypeParent))
        return;
    markFunctionPropertiesDeclaredAsFields(vm, classObject, classLayout);
    ClassDefinition(*this, globalObject, classObject, ClassPart::Everything).run(parent, prototypeParent);
}

void ClassTable::defineClass(JSGlobalObject* globalObject, Closure* classObject, JSValue parent, JSValue prototypeParent)
{
    defineClass(globalObject, classObject, *classObject->classData(), parent, prototypeParent);
}

void ClassTable::forgetEvaluationOf(const LayoutData& classLayout)
{
    FunctionID constructor(classLayout.classData().classConstructor);
    if (!classPrototypeIfExists(classLayout) && !(constructor && m_state.objects->closures.hasCellFor(constructor) && m_state.objects->closures.closureIfCreated(constructor)))
        return;
    if (!classLayout.staticFields().empty() && constructor && m_state.objects->closures.hasCellFor(constructor)) {
        if (auto* classObject = dynamicDowncast<Closure>(m_state.objects->closures.closureIfCreated(constructor)))
            classObject->ensureClassObject(*m_state.instance.vm);
        m_reevaluatedClasses.set(m_tables.rankOfClass(classLayout.index));
    }
    disarmMemberGuards(classLayout);
    {
        Locker locker { m_state.caches->lock() };
        prototypeCellOf(m_state, classLayout) = nullptr;
        if (constructor)
            m_evaluationsHeldWhileReached.remove(constructor.index());
    }
    m_classesWhosePrototypeAwaitsFirstTouch.clear(m_tables.rankOfClass(classLayout.index));
    if (constructor)
        m_declaredMemberLocations.remove(constructor.index());
    for (std::span<const MethodData> methods : { classLayout.methods(), classLayout.staticMethods() }) {
        for (const MethodData& method : methods)
            m_declaredMemberLocations.remove(method.function);
    }
    m_state.objects->structures.forgetStructuresOf(classLayout);
}

void ClassTable::createClassPart(JSGlobalObject* globalObject, Closure* classObject, ClassPart part, StaticFieldsOfPart staticFields)
{
    ASSERT(part != ClassPart::Everything);
    VM& vm = globalObject->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    ClassDefinition(*this, globalObject, classObject, part, staticFields).run(JSValue(), JSValue());
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
    return *Instance::owning(classPrototype)->state;
}

Structure* ClassPrototype::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    structure->setMayBePrototype(true);
    structure->setDidTransition(true);
    return structure;
}

ClassPrototype* ClassPrototype::create(VM& vm, Structure* structure, uint32_t classNumber, uint32_t memberRow)
{
    ClassPrototype* object = new (NotNull, allocateCell<ClassPrototype>(vm)) ClassPrototype(vm, structure, classNumber, memberRow);
    object->finishCreation(vm);
    return object;
}

void ClassPrototype::convertToOrdinaryObject(VM& vm, Structure* ordinary)
{
    RELEASE_ASSERT(ordinary->classInfoForCells() == JSFinalObject::info() && ordinary->inlineCapacity() == inlineCapacity && ordinary->storedPrototype() == structure()->storedPrototype());
    m_classNumber = 0;
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

JSObject* ClassPrototype::classObject() const
{
    State& state = stateOf(this);
    const LayoutData& classLayout = *state.objects->classes.classOf(*this);
    return asObject(state.objects->closures.closure(vm(), FunctionID(classLayout.classData().classConstructor)));
}

void ClassPrototype::createMembers(JSObject* object)
{
    State& state = stateOf(object);
    ClassTable& classes = state.objects->classes;
    Closure* classObject = uncheckedDowncast<Closure>(uncheckedDowncast<ClassPrototype>(object)->classObject());
    bool answersAsBefore = !object->butterfly();
    classes.createClassPart(state.instance.globalObject, classObject, ClassTable::ClassPart::PrototypeMembers);
    RELEASE_ASSERT(object->type() == FinalObjectType);
    CreatedMembers created { object, classes.structureOfDeclaredMemberHolders() };
    for (Instance* instance : object->vm().aotState().instances) {
        instance->propertyCaches.accessTable.didChangeObjectOnSomeChain();
        State* stateOfRealm = instance->state;
        if (!stateOfRealm || !stateOfRealm->chainDependents)
            continue;
        if (answersAsBefore)
            stateOfRealm->chainDependents->didCreateMembers(created);
        else
            stateOfRealm->chainDependents->didChange(object, nullptr);
    }
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
    const LayoutData& classLayout = *classes.classOf(*this);
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
    bool isDeclared = classes.declaresOwnMember(*classes.classOf(*prototype), uid);
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

bool areReadInVariables(const Closure& classObject)
{
    const LayoutData* classLayout = classObject.classData();
    if (!classLayout || classLayout->staticFields().empty())
        return false;
    State& state = classObject.state();
    ClassTable& classes = state.objects->classes;
    return !classes.isEvaluatedAnew(*classLayout) && state.objects->closures.closureIfCreated(classes.functionOfOwnClosure(classObject)) == &classObject;
}

static const FieldData* findFieldInVariable(const Closure& classObject, UniquedStringImpl* uid)
{
    if (!classObject.hasStaticFieldsLeft() && !(classObject.isLazyClassObject() && areReadInVariables(classObject)))
        return nullptr;
    if (parseIndex(PropertyName(uid)))
        return nullptr;
    return classObject.state().objects->classes.staticField(*classObject.classData(), uid);
}

static const FieldData* findFieldCreatedInVariable(const Closure& classObject, UniquedStringImpl* uid)
{
    if (classObject.butterfly() && (!classObject.hasStaticFieldsLeft() || isValidOffset(classObject.structure()->get(classObject.vm(), PropertyName(uid)))))
        return nullptr;
    return findFieldInVariable(classObject, uid);
}

JSValue tryRead(const Closure& classObject, UniquedStringImpl* uid)
{
    const FieldData* field = findFieldInVariable(classObject, uid);
    return field ? classObject.state().objects->classes.readStaticField(*field) : JSValue();
}

const FieldData* findFieldKeptInVariable(const Closure& classObject, UniquedStringImpl* uid)
{
    if (!classObject.hasStaticFieldsLeft())
        return nullptr;
    const FieldData* field = findFieldInVariable(classObject, uid);
    return field && classObject.state().objects->classes.readStaticField(*field) ? field : nullptr;
}

bool tryRedefine(Closure& classObject, UniquedStringImpl* uid, JSValue valueOrEmpty)
{
    const FieldData* field = findFieldCreatedInVariable(classObject, uid);
    ClassTable& classes = classObject.state().objects->classes;
    if (!field || !classes.readStaticField(*field))
        return false;
    if (valueOrEmpty)
        classes.storeStaticField(*field, valueOrEmpty);
    return true;
}

bool tryStore(Closure& classObject, UniquedStringImpl* uid, JSValue value)
{
    ASSERT(value);
    return tryRedefine(classObject, uid, value);
}

bool tryCreate(Closure& classObject, UniquedStringImpl* uid, JSValue value)
{
    ASSERT(value);
    const FieldData* field = findFieldCreatedInVariable(classObject, uid);
    if (!field)
        return false;
    ClassTable& classes = classObject.state().objects->classes;
    const LayoutData& classLayout = *classObject.classData();
    if (uid == classObject.vm().propertyNames->displayName.impl() || classes.hasStaticMethodNamed(classLayout, uid))
        return false;
    uint32_t index = static_cast<uint32_t>(field - classLayout.staticFields().data());
    uint32_t& end = classes.createdStaticFieldsEnd(classLayout);
    if (index < end)
        return false;
    end = index + 1;
    classes.storeStaticField(*field, value);
    return true;
}

bool tryCreateByStore(VM& vm, Closure& classObject, PropertyName name, JSValue value)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    if (name == names.name || name == names.length || name == names.prototype || !findFieldCreatedInVariable(classObject, name.uid()))
        return false;
    JSGlobalObject* globalObject = classObject.globalObject();
    JSObject* above = classObject.getPrototypeDirect().getObject();
    if (above != globalObject->functionPrototype())
        return false;
    for (; above; above = above->getPrototypeDirect().getObject()) {
        PropertySlot slot(above, PropertySlot::InternalMethodType::VMInquiry, &vm);
        if (above->type() == ProxyObjectType || above->getOwnPropertySlot(above, globalObject, name, slot))
            return false;
    }
    return tryCreate(classObject, name.uid(), value);
}

static void didPutFieldInPlaceOfFunctionProperty(VM& vm, Closure& classObject, UniquedStringImpl* uid)
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

void moveToProperties(VM& vm, Closure& classObject)
{
    State& state = classObject.state();
    ClassTable& classes = state.objects->classes;
    if (state.objects->closures.closureIfCreated(classes.functionOfOwnClosure(classObject)) != &classObject)
        return;
    const LayoutData& classLayout = *classObject.classData();
    bool cachesReadVariables = classes.hasStaticFieldsLeft(classLayout);
    bool isMarkedAsPrototype = classObject.structure()->mayBePrototype();
    if (isMarkedAsPrototype)
        classObject.structure()->setMayBePrototype(false);
    auto restoreMark = makeScopeExit([&] {
        if (isMarkedAsPrototype)
            classObject.structure()->setMayBePrototype(true);
    });
    for (const FieldData& field : classLayout.staticFields()) {
        JSValue value = classes.readStaticField(field);
        if (!value)
            continue;
        Identifier name = identifiersOf(state.instance).identifier(field.name);
        ProgramStatics::emptyVariableWhoseValueHasMoved(state.instance, field.slot);
        if (std::exchange(cachesReadVariables, false))
            classObject.putDirect(vm, name, value, static_cast<unsigned>(PropertyAttribute::None));
        else
            classObject.putDirectWithoutTransition(vm, name, value, static_cast<unsigned>(PropertyAttribute::None));
        didPutFieldInPlaceOfFunctionProperty(vm, classObject, name.impl());
        state.countWriteForEffectsNet(Typed::CountInEffectsNet::StaticFieldsMoved);
    }
    classes.didEmptyStaticFields(classLayout);
}

} // namespace StaticFields

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
