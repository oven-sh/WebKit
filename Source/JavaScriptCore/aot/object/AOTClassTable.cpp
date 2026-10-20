/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTClassHierarchy.h"
#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImmortalCells.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "FunctionPrototype.h"
#include "JSCInlines.h"
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
    state.caches->add(CacheKind::WeakByKey, "class sides by holder"_s, *this);
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
    JSObject* prototype = ClassPrototype::create(vm, structure, classLayout.index);
    registerClassPrototype(classLayout, prototype);
    return prototype;
}

JSObject* ClassTable::classPrototypeIfExists(const LayoutData& classLayout) const
{
    return static_cast<JSObject*>(prototypeCellOf(m_state, classLayout));
}

JSObject* ClassTable::classPrototype(VM& vm, const LayoutData& classLayout)
{
    if (JSObject* prototype = classPrototypeIfExists(classLayout))
        return prototype;
    if (classLayout.evaluation() == EvaluationState::PredictedOnce) {
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
        return parent ? createClassPrototype(vm, classLayout, parent) : nullptr;
    }
    if (hasOneClassObject(classLayout))
        m_state.objects->closures.closure(vm, FunctionID(classLayout.classData().classConstructor));
    if (!isDefinedByClass(classLayout))
        return defineClassOfConstructorFunction(vm, classLayout);
    return classPrototypeIfExists(classLayout);
}

// No operation defines the class of a constructor function. Its holders are the function and the object in its prototype property when the prototype of the instances is first asked for,
// which is at the first construction: a program fills or replaces that object before. The guards can tell the names of an ordinary object and of a compiled one.
JSObject* ClassTable::defineClassOfConstructorFunction(VM& vm, const LayoutData& classLayout)
{
    auto* classObject = dynamicDowncast<Closure>(m_state.objects->closures.closureIfCreated(FunctionID(classLayout.classData().classConstructor)));
    if (!classObject)
        return nullptr;
    JSValue stored = classObject->prototypeProperty(vm);
    if (!stored || !stored.isObject())
        return nullptr;
    JSObject* prototype = asObject(stored);
    if (prototype->type() != FinalObjectType && !isCompiledObject(prototype))
        return nullptr;
    ClassSide sideOfOtherClass;
    if (classHeldBy(prototype, sideOfOtherClass))
        return nullptr;

    registerClassPrototype(classLayout, prototype);
    const LayoutData* base = assumedBaseOf(classLayout);
    JSObject* assumedParent = base ? classPrototype(vm, *base) : m_state.instance.globalObject->objectPrototype();
    armMemberGuards(vm, classLayout, ClassSide::Prototype, prototype);
    armMemberGuards(vm, classLayout, ClassSide::Static, classObject);
    if (assumedParent && prototype->getPrototypeDirect() == JSValue(assumedParent))
        m_state.objects->caches->classHierarchy.didDefineClass(classLayout);
    else
        didChangePrototypeLink(classLayout, ClassSide::Prototype);
    if (classObject->getPrototypeDirect() != JSValue(m_state.instance.globalObject->functionPrototype()))
        didChangePrototypeLink(classLayout, ClassSide::Static);
    return prototype;
}

void ClassTable::registerClassPrototype(const LayoutData& classLayout, JSObject* prototype)
{
    ASSERT(classLayout.evaluation() == EvaluationState::ProvenOnce && !classPrototypeIfExists(classLayout));
    prototypeCellOf(m_state, classLayout) = prototype;
    m_state.immortalCells->add(prototype);
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

void ClassTable::forgetDefinition(VM& vm, const LayoutData& classLayout)
{
    ASSERT(classLayout.evaluation() == EvaluationState::ProvenOnce);
    ObjectModel& objects = *m_state.objects;
    m_classesWithForgottenDefinition.add(classLayout.index);
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
