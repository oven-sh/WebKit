/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClassToken.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeReporter.h"
#include "AOTState.h"
#include "AOTThisAllocationInlines.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "ObjectConstructor.h"
#include "StructureCache.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct ThisPrototype {
    JSObject* prototype;
    const LayoutData* layout;
    bool wasReadWithoutSideEffects;
};

static std::optional<FunctionID> functionInInstance(const Instance& instance, const JSCell* callee)
{
    std::optional<uint32_t> number = closureNumberOf(callee);
    if (!number || callee->structure()->realm() != instance.globalObject)
        return std::nullopt;
    return FunctionID(instance.tables().closureFunctionWithNumber(*number));
}

enum class Ancestry : uint8_t { NotBelow, Unknown, SameOrBelow };

static Ancestry ancestryOf(const ClassTable& classes, const LayoutData& layout, const LayoutData& base)
{
    for (const LayoutData* current = &layout; current; current = classes.baseOf(*current)) {
        if (current->index == base.index)
            return Ancestry::SameOrBelow;
        if (current->hasUntypedBase)
            return Ancestry::Unknown;
    }
    return Ancestry::NotBelow;
}

static unsigned inlineCapacityFor(unsigned numberOfProperties)
{
    numberOfProperties = std::min<unsigned>(numberOfProperties, JSFinalObject::maxInlineCapacity);
    size_t inlineCapacity = (MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(numberOfProperties)) - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue);
    return std::min<size_t>(inlineCapacity, JSFinalObject::maxInlineCapacity);
}

static JSValue ownPrototypeProperty(VM& vm, JSObject* compiledCallee)
{
    ASSERT(CompiledCallee::isCompiledCallee(compiledCallee));
    auto* closure = dynamicDowncast<Closure>(compiledCallee);
    if (!closure)
        CompiledCallee::reifyProperties(vm, compiledCallee);
    unsigned attributes = 0;
    JSValue prototype = compiledCallee->getDirect(vm, vm.propertyNames->prototype, attributes);
    if (!prototype)
        return closure ? closure->prototypeProperty(vm) : JSValue();
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return { };
    return prototype;
}

static std::optional<ThisPrototype> resolveThisPrototype(JSGlobalObject* globalObject, ClassTable& classes, Instance& instance, const LayoutData* constructorLayout, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ThisPrototype result { nullptr, nullptr, false };
    JSValue prototype;
    if (std::optional<FunctionID> target = functionInInstance(instance, newTarget)) {
        prototype = ownPrototypeProperty(vm, newTarget);
        result.wasReadWithoutSideEffects = !!prototype;
        const LayoutData* targetLayout = classes.classConstructedBy(*target);
        if (targetLayout && constructorLayout && ancestryOf(classes, *targetLayout, *constructorLayout) != Ancestry::NotBelow)
            result.layout = targetLayout;
    }
    if (!prototype) {
        if (JSObject* remembered = classes.tryReadPrototypePropertyAtRememberedPlace(newTarget)) {
            result.prototype = remembered;
            return result;
        }
        prototype = newTarget->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
    }
    if (prototype.isObject()) {
        result.prototype = asObject(prototype);
        return result;
    }
    JSGlobalObject* realm = getFunctionRealm(globalObject, newTarget);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    result.prototype = realm->objectPrototype();
    return result;
}

void ClassTable::keepWordsOfObjectWithoutFields(const LayoutData& layout, const Object& born)
{
    RELEASE_ASSERT(layout.size > sizeof(JSCell) && !(layout.size % sizeof(uint64_t)));
    size_t numberOfWords = (layout.size - sizeof(JSCell)) / sizeof(uint64_t);
    UniqueArray<uint64_t> words = makeUniqueArray<uint64_t>(numberOfWords);
    memcpySpan(std::span { words.get(), numberOfWords }, std::span { reinterpret_cast<const uint64_t*>(&born) + sizeof(JSCell) / sizeof(uint64_t), numberOfWords });
    m_wordsOfObjectsWithoutFields.set(layout.index, WTF::move(words));
}

Object* ClassTable::createWithoutFields(VM& vm, Structure* structure, const LayoutData& layout)
{
    Object* object = Object::create(vm, structure, layout, layout.size, orderWordOf(0, 0));
    validateMutation(vm, *object, ObjectMutation::ConstructDirectly);
    CompiledStructure* compiled = uncheckedDowncast<CompiledStructure>(structure);
    if (!compiled->wordsOfObjectWithoutFields()) {
        if (!wordsOfObjectWithoutFields(layout))
            keepWordsOfObjectWithoutFields(layout, *object);
        compiled->setWordsOfObjectWithoutFields(wordsOfObjectWithoutFields(layout));
    }
    return object;
}

Object* ClassTable::createClassInstance(VM& vm, const LayoutData& classLayout, JSObject* prototype)
{
    RELEASE_ASSERT(classLayout.shape);
    return createWithoutFields(vm, m_state.objects->structures.objectStructure(vm, classLayout.shape, prototype), classLayout);
}

static bool isRememberedFor(FunctionRareData& rareData, Structure* remembered, JSObject* constructor, JSObject* newTarget)
{
    uint32_t rememberedConstructor = rareData.compiledAllocationConstructor();
    if (constructor == newTarget)
        return isConstructorThatIsNewTarget(rememberedConstructor);
    return closureNumberOf(constructor) == rememberedConstructor && constructor->structure()->realm() == remembered->realm();
}

JSObject* ClassTable::createThis(JSGlobalObject* globalObject, JSObject* constructor, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSFunction* target = dynamicDowncast<JSFunction>(newTarget);
    FunctionRareData* rareData = target ? target->rareData() : nullptr;
    Structure* remembered = rareData ? rareData->compiledAllocationStructure() : nullptr;
    if (remembered && isRememberedFor(*rareData, remembered, constructor, newTarget)) {
        if (const LayoutData* layout = compiledLayoutOrNull(remembered))
            return createWithoutFields(vm, remembered, *layout);
        return constructEmptyObject(vm, remembered);
    }

    std::optional<FunctionID> function = functionInInstance(m_state.instance, constructor);
    const LayoutData* constructorLayout = function ? classConstructedBy(*function) : nullptr;
    RELEASE_ASSERT(!constructorLayout || !constructorLayout->hasUntypedBase);

    std::optional<ThisPrototype> resolved = resolveThisPrototype(globalObject, *this, m_state.instance, constructorLayout, newTarget);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* prototype = resolved->prototype;
    const LayoutData* layout = resolved->layout;
    bool canRemember = resolved->wasReadWithoutSideEffects && target && function;

    JSObject* object = layout && layout->shape
        ? static_cast<JSObject*>(createClassInstance(vm, *layout, prototype))
        : createOrdinaryInstance(globalObject, layout ? layout->numberOfFields : 0, prototype);
    if (canRemember) {
        Structure* structure = object->structure();
        uint32_t number = m_tables.closureNumberOf(function->index());
        if (constructor == newTarget) {
            bool isWholeConstruction = !compiledLayoutOrNull(structure) && structure->inlineCapacity() == JSFinalObject::defaultInlineCapacity && m_tables.closureNumberOnlyReturns(number);
            number = isWholeConstruction ? trivialConstructorIsNewTarget : constructorIsNewTarget;
        }
        target->ensureRareData(vm)->setCompiledAllocation(vm, structure, number);
    } else if (resolved->wasReadWithoutSideEffects && function && constructor == newTarget)
        rememberThisBesideCallee(vm, newTarget, object->structure());
    return object;
}

JSObject* ClassTable::tryCreateInstanceBelowObject(VM& vm, JSObject* newTarget)
{
    std::optional<FunctionID> target = functionInInstance(m_state.instance, newTarget);
    const LayoutData* layout = target ? classConstructedBy(*target) : nullptr;
    if (!layout || !layout->shape)
        return nullptr;
    const LayoutData* topmost = layout;
    while (const LayoutData* base = baseOf(*topmost))
        topmost = base;
    JSValue prototype = topmost->hasUntypedBase ? ownPrototypeProperty(vm, newTarget) : JSValue();
    return prototype && prototype.isObject() ? createClassInstance(vm, *layout, asObject(prototype)) : nullptr;
}

void ClassTable::rememberThisBesideCallee(VM& vm, JSObject* callee, Structure* structureOfThis)
{
    Structure* structure = callee->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->isDictionary() ? invalidOffset : structure->get(vm, vm.propertyNames->prototype, attributes);
    if (!isValidOffset(offset) || (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return;
    uint32_t structureID = structure->id().bits();
    m_placesOfPrototypeProperties[indexOfPlace(structureID)] = { structureID, offset };
    m_thisBesideCallees[indexOfCalleeWithThisBeside(callee)] = { callee, structureOfThis };
}

JSFinalObject* ClassTable::createOrdinaryInstance(JSGlobalObject* realm, unsigned numberOfProperties, JSObject* prototype)
{
    unsigned capacity = inlineCapacityFor(std::max<unsigned>(JSFinalObject::defaultInlineCapacity, numberOfProperties));
    return constructEmptyObject(realm->vm(), realm->structureCache().emptyObjectStructureForPrototype(realm, prototype, capacity));
}

JSObject* ClassTable::prototypeForInstancesOf(VM& vm, JSObject* compiledCallee)
{
    JSValue prototype = ownPrototypeProperty(vm, compiledCallee);
    return prototype && prototype.isObject() ? asObject(prototype) : m_state.instance.globalObject->objectPrototype();
}

void ClassTable::rememberPlaceOfPrototypeProperty(VM& vm, Closure* function, JSObject* prototype)
{
    Structure* structure = function->structure();
    if (!flattenOnce(vm, function))
        return;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, vm.propertyNames->prototype, attributes);
    if (!isValidOffset(offset)) {
        const LayoutData* classLayout = classConstructedBy(functionOfOwnClosure(*function));
        if (classLayout && isDefinedByClass(*classLayout))
            m_prototypesOfClassObjects[indexOfClassObject(function)] = { function, prototype };
        return;
    }
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return;
    uint32_t structureID = structure->id().bits();
    m_placesOfPrototypeProperties[indexOfPlace(structureID)] = { structureID, offset };
}

JSObject* ClassTable::prototypeOfClassObject(VM& vm, Closure* classObject)
{
    Structure* structure = classObject->structure();
    unsigned attributes = 0;
    PropertyOffset offset = structure->isDictionary() ? invalidOffset : structure->get(vm, vm.propertyNames->prototype, attributes);
    if (isValidOffset(offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)) [[likely]] {
        if (JSValue stored = classObject->getDirect(offset); stored.isObject()) [[likely]] {
            uint32_t structureID = structure->id().bits();
            m_placesOfPrototypeProperties[indexOfPlace(structureID)] = { structureID, offset };
            return asObject(stored);
        }
    }
    JSValue prototype = classObject->prototypeProperty(vm);
    if (!prototype || !prototype.isObject())
        return nullptr;
    rememberPlaceOfPrototypeProperty(vm, classObject, asObject(prototype));
    return asObject(prototype);
}

const LayoutData* ClassTable::classWithPrototype(JSObject* prototype) const
{
    if (prototype == m_lastEvaluation.prototype)
        return m_lastEvaluation.classLayout;
    if (auto* lazyPrototype = dynamicDowncast<ClassPrototype>(prototype))
        return classOf(*lazyPrototype);
    ClassSide side;
    const LayoutData* held = classHeldBy(prototype, side);
    return held && side == ClassSide::Prototype && isDefinedByClass(*held) ? held : nullptr;
}

static JSCell* closureEnvironmentOf(State& state, const Object& object, const FieldData& field)
{
    SlotData slot = object.layout().closureEnvironmentSlotOf(field);
    return cellInSlot(slot.representation(), &object.at<uint8_t>(slot.offset), state.instance.cellCageBase);
}

static JSValue valueForInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, JSObject& instance)
{
    if (field.slot.representation() != SlotRepresentation::DeferredClosure)
        return allocated.read(field);
    if (allocated.at<uint32_t>(field.slot.offset) != uncreatedDeferredClosure)
        return allocated.read(field);
    JSValue captured;
    if (field.flags & fieldDeferredClosureCapturesObject)
        captured = &instance;
    else if (field.flags & fieldDeferredClosureCapturesEnvironment)
        captured = closureEnvironmentOf(state, allocated, field);
    return state.objects->closures.createClosureOver(vm, FunctionID(state.tables.closureFunctionWithNumber(allocated.layout().deferredClosureNumberOf(field))), captured);
}

static void shareClosureEnvironment(VM& vm, State& state, const Object& allocated, const FieldData& field, Object& instance, const FieldData& target)
{
    JSCell* environment = closureEnvironmentOf(state, allocated, field);
    if (!environment)
        return;
    SlotData slot = instance.layout().closureEnvironmentSlotOf(target);
    bool isStored = tryWriteSlot(state.instance, slot.representation(), &instance.at<uint8_t>(slot.offset), environment);
    RELEASE_ASSERT(isStored);
    vm.writeBarrier(&instance, environment);
}

static bool looksFilledWhenNotListed(const FieldData& field)
{
    return !field.canLeaveSlot();
}

static void copyFieldToInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, Object& instance)
{
    UniquedStringImpl* name = propertyNameOf(vm, allocated, field);
    const FieldData* target = instance.fieldNamed(state, name);
    if (target && target->slot.representation() == SlotRepresentation::DeferredClosure && target->flags & field.flags & fieldDeferredClosureCapturesEnvironment)
        shareClosureEnvironment(vm, state, allocated, field, instance, *target);
    if (looksFilledWhenNotListed(field) && (!target || target->canLeaveSlot()))
        return;
    JSValue value = valueForInstance(vm, state, allocated, field, instance);
    ASSERT(value);
    bool hasSameAttributes = target && instance.attributesOf(*target) == allocated.attributesOf(field);
    std::optional<AcceptedValue> accepted = hasSameAttributes ? state.objects->layouts.accept(*target, value) : std::nullopt;
    if (accepted && instance.store(vm, *target, *accepted)) {
        if (target->canLeaveSlot())
            instance.didCreateField(state, *target, !!instance.slotOf(*target));
        return;
    }
    if (looksFilledWhenNotListed(field))
        return;
    instance.putDirectFromNativeCode(vm, name, value, allocated.attributesOf(field));
}

static void defineFieldOnInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, JSObject& instance)
{
    if (looksFilledWhenNotListed(field))
        return;
    UniquedStringImpl* name = field.isPrivate() ? ClassToken::nameBesideFields(vm, allocated, field, isCompiledObject(&instance)) : propertyNameOf(vm, allocated, field);
    JSValue value = valueForInstance(vm, state, allocated, field, instance);
    ASSERT(value);
    if (isCompiledObject(&instance))
        uncheckedDowncast<Object>(&instance)->putDirectFromNativeCode(vm, name, value, allocated.attributesOf(field));
    else
        instance.putDirect(vm, name, value, allocated.attributesOf(field));
}

void ClassTable::forgetInstancesBelowPrototypes()
{
    m_state.instance.classes.instancesBelowPrototypes = { };
    m_instancesBelowPrototypes.clear();
    m_numberOfInstancesBelowPrototypes = 0;
    m_wordsOfInstancesWithoutFields.clear();
}

void ClassTable::rememberInstanceBelow(VM& vm, uint32_t allocationOfSite, JSObject* prototype, const Object& instance, size_t size)
{
    LocalAllocator* allocator = vm.cellSpace().allocatorFor(size, AllocatorForMode::EnsureAllocator).localAllocator();
    if (!allocator)
        return;

    auto placeOf = [&](const JSCell* wanted, uint64_t site) -> InstanceBelowPrototype& {
        uint64_t mask = (m_instancesBelowPrototypes.size() - 1) << shiftOfInstanceBelowPrototype;
        for (uint64_t offset = firstOffsetOfInstanceBelowPrototype(std::bit_cast<uintptr_t>(wanted), static_cast<uint32_t>(site));; offset += sizeof(InstanceBelowPrototype)) {
            InstanceBelowPrototype& entry = m_instancesBelowPrototypes[(offset & mask) >> shiftOfInstanceBelowPrototype];
            if (!entry.prototype || (entry.prototype == wanted && entry.allocationOfSite == site))
                return entry;
        }
    };
    if (2 * (m_numberOfInstancesBelowPrototypes + 1) > m_instancesBelowPrototypes.size()) {
        Vector<InstanceBelowPrototype> former = std::exchange(m_instancesBelowPrototypes, Vector<InstanceBelowPrototype>(FillWith { }, std::max<size_t>(8, 2 * m_instancesBelowPrototypes.size()), InstanceBelowPrototype { }));
        for (const InstanceBelowPrototype& entry : former) {
            if (entry.prototype)
                placeOf(entry.prototype, entry.allocationOfSite) = entry;
        }
    }
    if (InstanceBelowPrototype& entry = placeOf(prototype, allocationOfSite); !entry.prototype) {
        Vector<uint64_t> words(FillWith { }, allocator->cellSize() / sizeof(uint64_t), static_cast<uint64_t>(0));
        memcpySpan(asMutableByteSpan(words.mutableSpan()), std::span { reinterpret_cast<const uint8_t*>(&instance), size });
        words[0] = cellHeaderFor(instance.structure());
        m_wordsOfInstancesWithoutFields.append(WTF::move(words));
        entry = { prototype, allocationOfSite, allocator, m_wordsOfInstancesWithoutFields.last().span().data() };
        ++m_numberOfInstancesBelowPrototypes;
        countEvent(vm, RuntimeCounterID::InstanceTemplatesCached);
    }
    m_state.instance.classes.instancesBelowPrototypes = { m_instancesBelowPrototypes.span().data(), (m_instancesBelowPrototypes.size() - 1) << shiftOfInstanceBelowPrototype };
}

JSObject* ClassTable::instanceWithPrototype(VM& vm, Object* allocated, JSObject* prototype, bool holdsNoField)
{
    const LayoutData& allocatedLayout = allocated->layout();
    bool listsNoField = holdsNoField || allocated->orderWord() == orderWordOf(0, 0);
    const LayoutData& allocatedClass = classOfInstances(allocatedLayout);
    const LayoutData* requested = isDefinedByClass(allocatedClass) ? classWithPrototype(prototype) : nullptr;
    if (!requested || requested->index == allocatedClass.index) {
        validateMutation(vm, *allocated, ObjectMutation::ConstructDirectly);
        if (!requested && isDefinedByClass(allocatedClass))
            validateMutation(vm, *allocated, ObjectMutation::ChangePrototype);
        allocated->setPrototypeAtCreation(vm, prototype);
        return allocated;
    }

    Ancestry ancestry = ancestryOf(*this, *requested, allocatedClass);
    bool hasShapeOfRequestedClass = ancestry != Ancestry::NotBelow && requested->shape;
    JSObject* instance = hasShapeOfRequestedClass ? static_cast<JSObject*>(createClassInstance(vm, *requested, prototype)) : createOrdinaryInstance(m_state.instance.globalObject, allocatedLayout.numberOfFields, prototype);
    if (listsNoField)
        return instance;
    allocated->forEachFieldInKeyOrder(m_state, [&](uint16_t index) {
        const FieldData& field = allocatedLayout.fields()[index];
        if (hasShapeOfRequestedClass && ancestry == Ancestry::SameOrBelow)
            copyFieldToInstance(vm, m_state, *allocated, field, *uncheckedDowncast<Object>(instance));
        else
            defineFieldOnInstance(vm, m_state, *allocated, field, *instance);
    });
    return instance;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
