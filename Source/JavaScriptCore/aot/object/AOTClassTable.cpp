/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTClassMemberTable.h"
#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImmortalCells.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "FunctionPrototype.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>

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

static std::optional<uint32_t> keyOf(const State& state, UniquedStringImpl* uid)
{
    return state.instance.vm->aotState().identifiers->numberOf(uid);
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
    std::optional<uint32_t> key = keyOf(m_state, name);
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
    // What the record cannot say, the parent or the name of a member, is computed by code, and NewClass follows that code.
    bool recordNamesParent = !classLayout.hasUntypedBase || classLayout.classData().builtinBase != LayoutData::noBase;
    if (!recordNamesParent || hasComputedMemberNames(classLayout))
        return created;
    ASSERT(hasOneClassObject(classLayout));
    count(m_state.instance, RuntimeCounterID::ClassesMadeWhenFirstTouched);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    defineClass(m_state.instance.globalObject, created, classLayout, JSValue(), JSValue());
    scope.assertNoException();
    return created;
}

bool ClassTable::hasStaticMethodNamed(const LayoutData& classLayout, UniquedStringImpl* name) const
{
    std::optional<uint32_t> key = keyOf(m_state, name);
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
    // What findOwnDeclaredMember() asks of a class before it answers from the record.
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
    if (isDefinedByClass(classLayout) || classPrototypeIfExists(classLayout))
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
    if (classLayout.evaluation() == EvaluationState::PredictedOnce) {
        if (!isDefinedByClass(classLayout))
            return defineClassOfConstructorFunction(vm, classLayout, UseOfFunction::MakesObjects);
        // The guards of such a class describe its first evaluation, whose class object is the registered one.
        FunctionID constructor(classLayout.classData().classConstructor);
        JSCell* registered = constructor && functionKindData(constructor).isConstructor ? m_state.objects->closures.closureIfCreated(constructor) : nullptr;
        JSValue prototype = registered ? asObject(registered)->getDirect(vm, vm.propertyNames->prototype) : JSValue();
        return prototype && prototype.isObject() ? asObject(prototype) : nullptr;
    }
    if (classLayout.evaluation() != EvaluationState::ProvenOnce)
        return nullptr;
    if (classLayout.createsPrototypeBeforeClassObject) {
        // What a constructor function has as its prototype property when the class below it is defined is not known before that definition runs.
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

// The class of a constructor function is that of the first function of its text that gets a prototype property, which comes before its first construction.
// A second function that gets one tells that the text was evaluated again. What it constructs is born below another prototype object, of which the guards say nothing,
// and compiled code asks no object that it has made itself which one it was born below. So the guards end for good, as those of a class that is evaluated again.
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

// No operation defines the class of a constructor function. Its holders are the function and the object in its prototype property when the prototype of the instances is first asked for,
// which is at the first construction: a program fills or replaces that object before. The guards can tell the names of an ordinary object and of a compiled one.
// Where that object can hold no class, the function never gets one: the Structures and the allocation sites of its instances name another prototype from then on.
// A function may get its class because a function or a class below it gets one. Its own guards speak of objects that it makes itself, and most such functions make none
// (nobody constructs a Stream): until it does, its holders are entered and report, and its guards stand in the way of no set, as those of a class that does not exist yet.
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
    // The program has filled the object by now, one name after the other.
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
    // Guards that hold while they are clear have nothing to begin.
    if (!classLayout.classData().guardsHoldWhileClear)
        didCreateHolder(classLayout, ClassSide::Prototype);
}

void ClassTable::finishLazyParts(VM& vm, const LayoutData& classLayout)
{
    FunctionID constructor(classLayout.classData().classConstructor);
    if (!constructor)
        return;
    ClosureTable& closures = m_state.objects->closures;
    JSObject* prototype = classPrototypeIfExists(classLayout);
    if (prototype && (closures.closureIfCreated(constructor) || hasOneClassObject(classLayout)))
        ClassPrototype::ensureMembers(prototype);
    if (auto* classObject = dynamicDowncast<Closure>(closures.closureIfCreated(constructor))) {
        classObject->ensureClassObject(vm);
        StaticFields::copyToProperties(vm, *classObject);
    }
}

// A member that an operation of the definition creates is added to the Structure in place, and a Structure that is kept says what the record alone creates.
// So the first evaluation of a class with such members keeps none: most classes have no second one.
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

void ClassTable::forgetDefinition(VM& vm, const LayoutData& classLayout)
{
    ASSERT(classLayout.evaluation() == EvaluationState::ProvenOnce);
    ObjectModel& objects = *m_state.objects;
    m_classesWithForgottenDefinition.add(classLayout.index);
    m_instancesOfClassesBelow.fill({ });
    disarmMemberGuards(classLayout);

    auto forgetFunction = [&](FunctionID function) {
        objects.closures.forgetClosure(function);
        m_declaredMemberLocations.remove(function.index());
    };
    if (FunctionID constructor { classLayout.classData().classConstructor }) {
        if (JSCell* classObject = objects.closures.closureIfCreated(constructor)) {
            m_classSidesByHolder.remove(asObject(classObject));
            if (JSValue prototype = asObject(classObject)->getDirect(vm, vm.propertyNames->prototype); prototype && prototype.isObject())
                m_classSidesByHolder.remove(asObject(prototype));
        }
        forgetFunction(constructor);
    }
    // The closure that code stores as a member belongs to its creator: a function that a module declares keeps its cell.
    auto forgetMembersCreatedByRecord = [&](std::span<const MethodData> methods) {
        for (const MethodData& method : methods) {
            if (!(method.flags & methodIsCreatedAtDefinition))
                forgetFunction(FunctionID(method.function));
        }
    };
    forgetMembersCreatedByRecord(classLayout.methods());
    forgetMembersCreatedByRecord(classLayout.staticMethods());

    objects.structures.forgetStructuresOf(classLayout);
    if (JSObject* prototype = classPrototypeIfExists(classLayout))
        m_classSidesByHolder.remove(prototype);
    prototypeCellOf(m_state, classLayout) = nullptr;
    m_constructorFunctionsThatAreOnlyBases.remove(classLayout.index);
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
