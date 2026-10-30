/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTState.h"
#include "AOTThreshold.h"
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
    if (Instance::of(callee->structure()->realm()) != &instance)
        return std::nullopt;
    return functionOf(callee);
}

enum class Ancestry : uint8_t { NotBelow, Unknown, SameOrBelow };

// A class whose base is not proven to be a compiled class may be below any class.
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

// new.target selects the layout.
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

// The run time lists no field when it allocates. The stores of the constructor create the fields and record their keys.
static Object* createWithoutFields(VM& vm, RuntimeLayoutTable& layouts, Structure* structure, const LayoutData& layout)
{
    if (const RuntimeLayoutTable::BodyAtBirth* body = layouts.bodyAtBirthIfKnown(layout))
        return Object::createAsAtBirth(vm, structure, body->span(), body->isBornUnfinished);
    Object* object = Object::create(vm, structure, layout, layout.size, orderWordOf(0, 0));
    layouts.rememberBodyAtBirth(layout, std::span { reinterpret_cast<const uint8_t*>(object) + sizeof(JSCell), layout.size - sizeof(JSCell) }, object->perCellBit());
    return object;
}

Object* ClassTable::createClassInstance(VM& vm, const LayoutData& classLayout, JSObject* prototype)
{
    RELEASE_ASSERT(classLayout.shape);
    return createWithoutFields(vm, m_state.objects->layouts, m_state.objects->structures.objectStructure(vm, classLayout.shape, prototype), classLayout);
}

// What FunctionRareData::compiledAllocationConstructor() holds for a function that was constructed with itself as new.target. Any other constructor is remembered by its closure number.
static constexpr uint32_t constructorIsNewTarget = FunctionRareData::maxCompiledAllocationConstructor;
static_assert(ClosureWord::functionMask < constructorIsNewTarget);

// What createThis() allocates depends on new.target, on which function is constructed and on the instance of that function.
static bool isRememberedFor(FunctionRareData& rareData, Structure* remembered, JSObject* constructor, JSObject* newTarget)
{
    uint32_t rememberedConstructor = rareData.compiledAllocationConstructor();
    if (constructor == newTarget)
        return rememberedConstructor == constructorIsNewTarget;
    std::optional<FunctionID> function = functionOf(constructor);
    return function && rememberedConstructor == closureNumberOf(*function) && constructor->structure()->realm() == remembered->realm();
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
            return createWithoutFields(vm, m_state.objects->layouts, remembered, *layout);
        return constructEmptyObject(vm, remembered);
    }

    std::optional<FunctionID> function = functionOf(constructor);
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
    if (canRemember)
        target->ensureRareData(vm)->setCompiledAllocation(vm, object->structure(), constructor == newTarget ? constructorIsNewTarget : closureNumberOf(*function));
    return object;
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
    if (!flattenOnce(vm, function) || vm.aotState().validatesRuntime)
        return;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, vm.propertyNames->prototype, attributes);
    if (!isValidOffset(offset)) {
        const LayoutData* classLayout = classConstructedBy(function->function());
        if (classLayout && isDefinedByClass(*classLayout))
            m_prototypesOfClassObjects[indexOfClassObject(function)] = { function, prototype };
        return;
    }
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return;
    uint32_t structureID = structure->id().bits();
    m_placesOfPrototypeProperties[indexOfPlace(structureID)] = { structureID, offset };
}

// The object that the definition of a class made its prototype. What a program stores as the prototype of a constructor function names no class.
const LayoutData* ClassTable::classWithPrototype(JSObject* prototype) const
{
    if (auto* lazyPrototype = dynamicDowncast<ClassPrototype>(prototype))
        return classWithIndex(lazyPrototype->classIndex());
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
    return state.objects->closures.createClosureOver(vm, closureFunctionWithNumber(allocated.layout().deferredClosureNumberOf(field)), captured);
}

static void shareClosureEnvironment(VM& vm, State& state, const Object& allocated, const FieldData& field, Object& instance, const FieldData& target)
{
    JSCell* environment = closureEnvironmentOf(state, allocated, field);
    if (!environment)
        return;
    SlotData slot = instance.layout().closureEnvironmentSlotOf(target);
    bool isStored = tryWriteSlot(*state.strings, slot.representation(), &instance.at<uint8_t>(slot.offset), environment);
    RELEASE_ASSERT(isStored);
    vm.writeBarrier(&instance, environment);
}

static bool looksFilledWhenNotListed(const FieldData& field)
{
    return !field.canLeaveSlot();
}

// A subclass may declare the name again with another type, so the field of the instance tests what compiled code stored.
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
    // Until its first store such a slot holds a placeholder, which need not be a value of the field's type. The slot of the new instance holds one as well.
    if (looksFilledWhenNotListed(field))
        return;
    instance.putDirectFromNativeCode(vm, name, value, allocated.attributesOf(field));
}

// The instance declares other fields or none.
static void defineFieldOnInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, JSObject& instance)
{
    if (looksFilledWhenNotListed(field))
        return;
    UniquedStringImpl* name = propertyNameOf(vm, allocated, field);
    JSValue value = valueForInstance(vm, state, allocated, field, instance);
    ASSERT(value);
    if (isCompiledObject(&instance))
        uncheckedDowncast<Object>(&instance)->putDirectFromNativeCode(vm, name, value, allocated.attributesOf(field));
    else
        instance.putDirect(vm, name, value, allocated.attributesOf(field));
}

Object* ClassTable::createInstanceOfClassBelow(VM& vm, size_t indexOfRecord)
{
    auto* structure = uncheckedDowncast<CompiledStructure>(std::bit_cast<StructureID>(static_cast<uint32_t>(m_instancesOfClassesBelow[indexOfRecord].header)).decode());
    return createWithoutFields(vm, m_state.objects->layouts, structure, structure->layout());
}

static std::array<size_t, 2> placesOf(const InstanceOfClassBelow& record)
{
    uintptr_t prototype = std::bit_cast<uintptr_t>(record.prototype);
    return { firstPlaceOfInstanceOfClassBelow(prototype), secondPlaceOfInstanceOfClassBelow(prototype) };
}

const InstanceOfClassBelow* ClassTable::findInstanceOfClassBelow(const JSObject* prototype, uint32_t allocation) const
{
    for (size_t place : placesOf({ const_cast<JSObject*>(prototype), 0, nullptr, allocation, 0 })) {
        const InstanceOfClassBelow& record = m_instancesOfClassesBelow[place];
        if (record.prototype == prototype && record.allocation == allocation)
            return &record;
    }
    return nullptr;
}

// Where both places are taken, the record at the first one makes room: it moves to its other place, and so does the record that it finds there.
void ClassTable::rememberInstanceOfClassBelow(InstanceOfClassBelow record)
{
    std::array<size_t, 2> places = placesOf(record);
    for (size_t place : places) {
        InstanceOfClassBelow& held = m_instancesOfClassesBelow[place];
        if (!held.prototype || (held.prototype == record.prototype && held.allocation == record.allocation)) {
            held = record;
            return;
        }
    }
    AOT_THRESHOLD(unsigned, maxNumberOfMoves, 8);
    size_t place = places[0];
    for (unsigned moves = 0; moves < maxNumberOfMoves; ++moves) {
        std::swap(record, m_instancesOfClassesBelow[place]);
        if (!record.prototype)
            return;
        auto [first, second] = placesOf(record);
        place = place == first ? second : first;
    }
}

// Each evaluation of a class has a prototype object of its own, and the record of the allocation names one of them.
void ClassTable::rememberInstanceOfOtherEvaluation(VM& vm, const Object& instance, JSObject* prototype, uint32_t allocation)
{
    const LayoutData& layout = instance.layout();
    const AllocationData& site = m_tables.allocations()[allocation];
    if (layout.size > MarkedSpace::largeCutoff || site.size() != layout.size || site.orderWord() != orderWordOf(0, 0))
        return;
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    if (!layouts.bodyAtBirthIfKnown(layout))
        createWithoutFields(vm, layouts, instance.structure(), layout);
    const RuntimeLayoutTable::BodyAtBirth& body = *layouts.bodyAtBirthIfKnown(layout);
    if (body.isBornUnfinished != site.isBornUnfinished)
        return;
    static_assert(JSCell::typeInfoFlagsOffset() == 6);
    uint64_t header = cellHeaderFor(instance.structure()) | (body.isBornUnfinished ? static_cast<uint64_t>(TypeInfoPerCellBit) << 48 : 0);
    rememberInstanceOfClassBelow({ prototype, header, body.cell.span().data(), allocation, static_cast<uint32_t>(body.cell.size() / 2) });
}

// The prototype object of another class never leaves the object with the allocated shape: what the program does to that object is recorded for the other class.
JSObject* ClassTable::instanceWithPrototype(VM& vm, Object* allocated, JSObject* prototype, bool holdsNoField, std::optional<uint32_t> allocation)
{
    const LayoutData& allocatedLayout = allocated->layout();
    // Where the keys are never enumerated, the order word lists nothing whatever the slots hold: only who allocated the object knows.
    bool listsNoField = holdsNoField || (!allocatedLayout.keysAreNeverEnumerated && allocated->orderWord() == orderWordOf(0, 0));
    if (listsNoField && allocation) {
        if (const InstanceOfClassBelow* remembered = findInstanceOfClassBelow(prototype, *allocation))
            return createInstanceOfClassBelow(vm, remembered - m_instancesOfClassesBelow.data());
    }
    const LayoutData& allocatedClass = classOfInstances(allocatedLayout);
    const LayoutData* requested = isDefinedByClass(allocatedClass) ? classWithPrototype(prototype) : nullptr;
    if (!requested || requested->index == allocatedClass.index) {
        allocated->setPrototypeAtCreation(vm, prototype);
        if (requested && listsNoField && allocation)
            rememberInstanceOfOtherEvaluation(vm, *allocated, prototype, *allocation);
        return allocated;
    }

    Ancestry ancestry = ancestryOf(*this, *requested, allocatedClass);
    bool hasShapeOfRequestedClass = ancestry != Ancestry::NotBelow && requested->shape;
    JSObject* instance = hasShapeOfRequestedClass ? static_cast<JSObject*>(createClassInstance(vm, *requested, prototype)) : createOrdinaryInstance(m_state.instance.globalObject, allocatedLayout.numberOfFields, prototype);
    if (listsNoField) {
        if (hasShapeOfRequestedClass && allocation && requested->size <= MarkedSpace::largeCutoff) {
            const RuntimeLayoutTable::BodyAtBirth& body = *m_state.objects->layouts.bodyAtBirthIfKnown(*requested);
            static_assert(JSCell::typeInfoFlagsOffset() == 6);
            uint64_t header = cellHeaderFor(instance->structure()) | (body.isBornUnfinished ? static_cast<uint64_t>(TypeInfoPerCellBit) << 48 : 0);
            rememberInstanceOfClassBelow({ prototype, header, body.cell.span().data(), *allocation, static_cast<uint32_t>(body.cell.size() / 2) });
        }
        return instance;
    }
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
