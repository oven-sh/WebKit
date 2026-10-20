/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClassStructureCache.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTEnvironment.h"
#include "AOTFunctionNames.h"
#include "AOTMemberGuards.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "FunctionRareData.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

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
    const Identifier& keyFor(const MethodData&);
    void defineMembers(JSObject* target, std::span<const MethodData>);
    void defineMember(JSObject* target, const MethodData&, const Identifier& key, JSObject* function);
    void restorePrototypeMarks();
    void finish();
    bool hasAssumedBase() const;
    void armGuards();
#if ASSERT_ENABLED
    bool hasMemberEnvironment() const;
#endif

    Butterfly* allocateOutOfLineStorage(Structure*);
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
    if (!m_classObject->kind().isInRuntimeModule)
        m_state.statics->ensureProgramStaticsInitialized();
    ASSERT(m_requestedPart == ClassPart::Everything || m_classLayout.createsMembersLazily);
    ASSERT(!createsClassObject() || !m_classObject->getDirect(m_vm, m_vm.propertyNames->prototype));
    ASSERT(hasMemberEnvironment());

    if (resolveParents(heritage, prototypeParent) == ParentResolution::BaseNotDefined)
        return;
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
    if (scope.exception()) [[unlikely]] {
        restorePrototypeMarks();
        return;
    }
    finish();
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
        RELEASE_ASSERT(heritage && (heritage.isObject() || heritage.isNull()));
        RELEASE_ASSERT(prototypeParent && (prototypeParent.isObject() || prototypeParent.isNull()));
        m_classObjectParent = heritage.getObject();
        m_prototypeParent = prototypeParent;
    }
    if (m_prototypeParent.isObject())
        asObject(m_prototypeParent)->didBecomePrototype(m_vm);
    return ParentResolution::Resolved;
}

Butterfly* ClassDefinition::allocateOutOfLineStorage(Structure* structure)
{
    return structure->outOfLineCapacity() ? Butterfly::create(m_vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
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
    m_mayShareStructures = m_part == ClassPart::Everything && m_classLayout.evaluation() != EvaluationState::ProvenOnce && !m_classObject->mayBePrototype() && !m_classObject->butterfly();
    if (!m_mayShareStructures)
        return;
    m_shared = m_state.objects->caches->classStructures.find(m_classLayout.index, m_classObjectParent, prototypeParentCell());
    if (m_shared.classObjectStructure && (m_shared.classObjectStructure->realm() != m_globalObject || m_shared.classObjectStructure->storedPrototype() != (m_classObjectParent ? JSValue(m_classObjectParent) : m_classObject->getPrototypeDirect())))
        m_shared = { };
}

void ClassDefinition::createPrototype()
{
    if (isShared()) {
        m_prototype = JSFinalObject::createWithButterfly(m_vm, m_shared.prototypeStructure, allocateOutOfLineStorage(m_shared.prototypeStructure));
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
    if (m_requestedPart != ClassPart::Everything || m_part != ClassPart::ClassObject || !m_classObjectParent || !m_classLayout.createsClassObjectOnMutation || m_classObject->mayBePrototype() || m_classObject->butterfly())
        return false;
    m_classObject->setStructure(m_vm, m_classes.lazyClassObjectStructureInheritingFrom(m_vm, m_classObjectParent));
    return true;
}

void ClassDefinition::prepareObjects()
{
    if (isShared()) {
        RELEASE_ASSERT(!m_classObject->butterfly());
        if (Butterfly* storage = allocateOutOfLineStorage(m_shared.classObjectStructure))
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
        Structure* classObjectStructure = Closure::createStructure(m_vm, m_globalObject, m_classObjectParent ? m_classObjectParent : m_classObject->getPrototypeDirect().getObject());
        if (m_classObject->butterfly()) {
            m_earlyStores.takeFrom(m_globalObject, m_classObject);
            m_classObject->nukeStructureAndSetButterfly(m_vm, m_classObject->structureID(), nullptr);
        }
        m_classObject->setStructure(m_vm, classObjectStructure);
        m_classObject->putDirectWithoutTransition(m_vm, m_vm.propertyNames->prototype, m_prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
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

const Identifier& ClassDefinition::keyFor(const MethodData& method)
{
    return m_vm.aotState().identifiers->identifier(method.name);
}

void ClassDefinition::defineMembers(JSObject* target, std::span<const MethodData> methods)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    for (auto& method : methods) {
        if (method.flags & methodIsCreatedAtDefinition)
            continue;
        ASSERT(!(method.flags & methodHasComputedName));
        JSObject* function = m_classes.createMethodClosure(m_vm, m_classObject, m_classLayout, method);
        defineMember(target, method, keyFor(method), function);
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
    bool isLazyFunctionProperty = target == m_classObject && (key == m_vm.propertyNames->name || key == m_vm.propertyNames->length || key == m_vm.propertyNames->prototype);
    if (!isLazyFunctionProperty && !parseIndex(key) && !isValidOffset(target->structure()->get(m_vm, key))) {
        if (kind == MethodKind::Method)
            target->putDirectWithoutTransition(m_vm, key, function, static_cast<unsigned>(PropertyAttribute::DontEnum));
        else
            target->putDirectNonIndexAccessorWithoutTransition(m_vm, key, createAccessor(), PropertyAttribute::Accessor | PropertyAttribute::DontEnum);
        return;
    }
    m_hasOnlyPlainMembers = false;
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

void ClassDefinition::restorePrototypeMarks()
{
    if (m_classObjectMayBePrototype)
        m_classObject->structure()->setMayBePrototype(true);
    if (m_part == ClassPart::PrototypeMembers)
        m_prototype->structure()->setMayBePrototype(true);
}

void ClassDefinition::finish()
{
    restorePrototypeMarks();
    if (m_mayShareStructures && !isShared() && m_hasOnlyPlainMembers)
        m_state.objects->caches->classStructures.add(m_classLayout.index, m_classObjectParent, prototypeParentCell(), { m_classObject->structure(), m_prototype->structure() });
    m_earlyStores.restoreTo(m_globalObject, m_classObject, m_requestedPart == ClassPart::Everything ? StoreTime::BeforeDefinition : StoreTime::AfterDefinition);
    m_earlyPrototypeStores.restoreTo(m_globalObject, m_prototype, StoreTime::AfterDefinition);
    armGuards();
}

bool ClassDefinition::hasAssumedBase() const
{
    const ClassData& classData = m_classLayout.classData();
    uint32_t assumedBase = classData.base == LayoutData::noBase ? classData.predictedBase : classData.base;
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
    // The guard of the prototype property asks for the prototype object of the class, which the first call enters.
    m_classes.armMemberGuards(m_vm, m_classLayout, ClassSide::Prototype, m_prototype);
    m_classes.armMemberGuards(m_vm, m_classLayout, ClassSide::Static, m_classObject);
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
        throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
        return { };
    }
    JSValue prototypeParent = asObject(heritage)->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    if (!prototypeParent.isObject() && !prototypeParent.isNull()) {
        throwTypeError(globalObject, scope, "The value of the superclass's prototype property is not an object or null."_s);
        return { };
    }
    return prototypeParent;
}

// The definition of a static field named "name" or "length" is a store to its variable, which no hook of the class object sees.
static void markFunctionPropertiesDeclaredAsFields(VM& vm, Closure* classObject, const LayoutData& classLayout)
{
    for (const FieldData& field : classLayout.staticFields()) {
        const Identifier& key = vm.aotState().identifiers->identifier(field.name);
        if (key == vm.propertyNames->name)
            classObject->ensureRareData(vm)->setHasModifiedNameForBoundOrNonHostFunction();
        else if (key == vm.propertyNames->length)
            classObject->ensureRareData(vm)->setHasModifiedLengthForBoundOrNonHostFunction();
        else if (key == vm.propertyNames->displayName)
            didStoreDisplayName(vm, classObject);
    }
}

void ClassTable::defineClass(JSGlobalObject* globalObject, Closure* classObject, JSValue parent, JSValue prototypeParent)
{
    VM& vm = globalObject->vm();
    const LayoutData& classLayout = *classObject->classData();
    bool isProvenOnce = classLayout.evaluation() == EvaluationState::ProvenOnce;
    ClosureTable& closures = m_state.objects->closures;
    // What escaped from a run that threw may have used the class since the reset, and the run time then created it from its record.
    if (m_classesWithForgottenDefinition.contains(classLayout.index) && closures.closureIfCreated(classObject->function()) != classObject) [[unlikely]] {
        finishLazyParts(vm, classLayout);
        forgetDefinition(vm, classLayout);
    }
    if (isProvenOnce && (classObject->getDirect(vm, vm.propertyNames->prototype) || (!classLayout.createsPrototypeBeforeClassObject && classPrototypeIfExists(classLayout))))
        return;
    JSCell* registered = closures.closureIfCreated(classObject->function());
    if (registered && registered != classObject) {
        RELEASE_ASSERT(!isProvenOnce);
        didEvaluateClassAgain(m_state, classLayout);
    } else if (!registered && classLayout.evaluation() != EvaluationState::SeenTwice)
        closures.registerClassObject(classObject);
    markFunctionPropertiesDeclaredAsFields(vm, classObject, classLayout);
    ClassDefinition(*this, globalObject, classObject, ClassPart::Everything).run(parent, prototypeParent);
}

void ClassTable::createClassPart(JSGlobalObject* globalObject, Closure* classObject, ClassPart part)
{
    ASSERT(part != ClassPart::Everything);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    ClassDefinition(*this, globalObject, classObject, part).run(JSValue(), JSValue());
    scope.assertNoException();
}

JSObject* ClassTable::createMethodClosure(VM& vm, Closure* classObject, const LayoutData& classLayout, const MethodData& method)
{
    ClosureTable& closures = m_state.objects->closures;
    FunctionID function(method.function);
    if (hasOneClassObject(classLayout))
        return asObject(closures.closure(vm, function));
    bool takesEnvironment = method.flags & methodTakesEnvironment;
    return closures.createClosure(vm, function, classObject->captures().first(takesEnvironment), takesEnvironment);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
