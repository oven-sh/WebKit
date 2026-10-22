/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
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
    Identifier keyFor(const MethodData&);
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
    if (m_requestedPart != ClassPart::Everything || m_part != ClassPart::ClassObject || !m_classLayout.createsClassObjectOnMutation || m_classObject->mayBePrototype() || m_classObject->butterfly())
        return false;
    // A class without a parent stays in the Structure that every class object is created with.
    if (!m_classObjectParent)
        return m_classObject->structure() == m_state.objects->closures.closureStructure(m_vm, classObjectClosureStructure);
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

Identifier ClassDefinition::keyFor(const MethodData& method)
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
    // The other half of an accessor that the definition has just put there. Nobody has seen the object, and the property stays what the Structure says.
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

void ClassDefinition::restorePrototypeMarks()
{
    if (m_classObjectMayBePrototype)
        m_classObject->structure()->setMayBePrototype(true);
    // No other object has the Structure that the members were added to, so the mark needs no transition.
    if (createsPrototypeMembers() && !isShared())
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
    bool holdersHaveWhatRecordSays = m_hasOnlyPlainMembers && !isShared() && m_earlyStores.keysAndAttributes.isEmpty() && m_earlyPrototypeStores.keysAndAttributes.isEmpty();
    HolderState stateOfPrototype = holdersHaveWhatRecordSays ? HolderState::AsDefinitionMadeIt : HolderState::Unknown;
    // A static field may have the name of a static method, and lies in a variable.
    HolderState stateOfClassObject = holdersHaveWhatRecordSays && m_classLayout.staticFields().empty() ? HolderState::AsDefinitionMadeIt : HolderState::Unknown;
    // A constructor function gets its class at its first construction, which may never come: nobody constructs a Duplex. The guards of the class look up through its holders.
    if (const LayoutData* base = m_classes.assumedBaseOf(m_classLayout); base && hasAssumedBase())
        m_classes.classPrototypeOfBase(m_vm, *base);
    // The guard of the prototype property asks for the prototype object of the class, which the first call enters.
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
    // A class object that nobody has looked at has the property as the record says: no code runs, and nothing but the stand-in for that object needs to be there.
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
            didChangeDisplayName(vm, classObject);
    }
}

// Where the record says all that the definition does, the definition makes nothing.
bool ClassTable::tryLeaveDefinitionToRecord(VM& vm, Closure* classObject, const LayoutData& classLayout, JSValue heritage, JSValue prototypeParent)
{
    if (!leavesDefinitionToRecord(classLayout))
        return false;
    if (!m_classesWithForgottenDefinition.isEmpty() || classObject->butterfly() || classObject->mayBePrototype())
        return false;
    // Somebody may have asked for the prototype object first.
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
    // Only the definition knows what stands above a chain that ends with a heritage.
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
        // Where a base is only assumed, the definition compares.
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

// A library makes a class for each of its kinds out of one text: `class Kind extends Parent {}` in a function.
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
    auto storageFor = [&](Structure* structure) -> Butterfly* {
        return structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    };
    JSObject* prototype = JSFinalObject::createWithButterfly(vm, kept.prototypeStructure, storageFor(kept.prototypeStructure));
    if (Butterfly* storage = storageFor(kept.classObjectStructure))
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
