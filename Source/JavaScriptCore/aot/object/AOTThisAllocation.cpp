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
#include "AOTState.h"
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

static JSObject* prototypeOrObjectPrototype(JSValue prototype, JSGlobalObject* realm)
{
    return prototype && prototype.isObject() ? asObject(prototype) : realm->objectPrototype();
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

// new.target selects the layout. Only what is or may be below the class of the constructor gets the shape of its class: the flow analysis has an allocation for nothing else,
// so any other new.target gets an ordinary object, which passes no test for a shape.
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
Object* ClassTable::createClassInstance(VM& vm, const LayoutData& classLayout, JSObject* prototype)
{
    RELEASE_ASSERT(classLayout.shape);
    CompiledStructure* structure = m_state.objects->structures.objectStructure(vm, classLayout.shape, prototype);
    return Object::create(vm, structure, classLayout, classLayout.size, orderWordOf(0, 0));
}

JSObject* ClassTable::createThis(JSGlobalObject* globalObject, JSObject* constructor, JSObject* newTarget)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSFunction* target = dynamicDowncast<JSFunction>(newTarget);
    FunctionRareData* rareData = target ? target->rareData() : nullptr;
    if (rareData && rareData->compiledAllocationConstructor() == constructor) {
        Structure* structure = rareData->compiledAllocationStructure();
        if (const LayoutData* layout = compiledLayoutOrNull(structure))
            return Object::create(vm, structure, *layout, layout->size, rareData->compiledAllocationOrderWord());
        return constructEmptyObject(vm, structure);
    }

    std::optional<FunctionID> function = functionOf(constructor);
    const LayoutData* constructorLayout = function ? classConstructedBy(*function) : nullptr;
    RELEASE_ASSERT(!constructorLayout || !constructorLayout->hasUntypedBase);

    std::optional<ThisPrototype> resolved = resolveThisPrototype(globalObject, *this, m_state.instance, constructorLayout, newTarget);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* prototype = resolved->prototype;
    const LayoutData* layout = resolved->layout;
    bool canRemember = resolved->wasReadWithoutSideEffects && target;

    if (layout && layout->shape) {
        Object* object = createClassInstance(vm, *layout, prototype);
        if (canRemember)
            target->ensureRareData(vm)->setCompiledAllocation(vm, object->structure(), object->orderWord(), constructor);
        return object;
    }

    JSFinalObject* object = createOrdinaryInstance(globalObject, layout ? layout->numberOfFields : 0, prototype);
    if (canRemember)
        target->ensureRareData(vm)->setCompiledAllocation(vm, object->structure(), 0, constructor);
    return object;
}

JSFinalObject* ClassTable::createOrdinaryInstance(JSGlobalObject* realm, unsigned numberOfProperties, JSObject* prototype)
{
    unsigned capacity = inlineCapacityFor(std::max<unsigned>(JSFinalObject::defaultInlineCapacity, numberOfProperties));
    return constructEmptyObject(realm->vm(), realm->structureCache().emptyObjectStructureForPrototype(realm, prototype, capacity));
}

JSObject* ClassTable::prototypeForInstancesOf(VM& vm, JSObject* compiledCallee)
{
    return prototypeOrObjectPrototype(ownPrototypeProperty(vm, compiledCallee), m_state.instance.globalObject);
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

static JSValue valueForInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, JSObject& instance)
{
    if (field.slot.representation() != SlotRepresentation::DeferredClosure)
        return allocated.read(field);
    JSValue captured = field.flags & fieldDeferredClosureCapturesObject ? JSValue(&instance) : JSValue();
    return state.objects->closures.createClosureOver(vm, allocated.layout().deferredClosureOf(field), captured);
}

// Where the field of the instance has the same type, what compiled code stored moves over without a test: no run-time type describes a class token. A subclass that declares the name again tests it.
// A slot without an absent state looks filled whether the allocation listed its field or not, and compiled code lists no field where the prototype may select another class.
// So what such a slot holds creates no field: it moves only into a slot that has no absent state either. A subclass that declares the name again may have one.
static void copyFieldToInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, Object& instance)
{
    UniquedStringImpl* name = propertyNameOf(vm, allocated, field);
    const FieldData* target = instance.fieldNamed(state, name);
    if (target && target->slot.representation() == SlotRepresentation::DeferredClosure)
        return;
    if (!field.canLeaveSlot() && (!target || target->canLeaveSlot()))
        return;
    JSValue value = valueForInstance(vm, state, allocated, field, instance);
    ASSERT(value);
    bool isAccepted = target && (target->type == field.type || state.objects->layouts.fieldAccepts(*target, value));
    if (isAccepted && instance.store(vm, *target, value)) {
        if (target->canLeaveSlot())
            instance.didCreateField(state, *target);
        return;
    }
    instance.putDirectFromNativeCode(vm, name, value, allocated.attributesOf(field));
}

// The instance declares other fields or none. A slot without an absent state looks filled whether the allocation listed its field or not, and compiled code
// lists no field where the prototype may select another class.
static void defineFieldOnInstance(VM& vm, State& state, const Object& allocated, const FieldData& field, JSObject& instance)
{
    if (!field.canLeaveSlot())
        return;
    UniquedStringImpl* name = propertyNameOf(vm, allocated, field);
    JSValue value = valueForInstance(vm, state, allocated, field, instance);
    ASSERT(value);
    if (isCompiledObject(&instance))
        uncheckedDowncast<Object>(&instance)->putDirectFromNativeCode(vm, name, value, allocated.attributesOf(field));
    else
        instance.putDirect(vm, name, value, allocated.attributesOf(field));
}

// The prototype object of another class never leaves the object with the allocated shape: what the program does to that object is recorded for the other class.
// A class that is or may be below the allocated one gets an instance of its own where it has a shape. For any other class the flow analysis has no allocation, or one whose shape
// it does not prove, and its constructor does not run: the object is an ordinary one, which passes no test for a shape.
// The layout of a constructor function is allocated for that function as new.target alone, and it stays whatever the program has stored as the prototype of the function.
JSObject* ClassTable::instanceWithPrototype(VM& vm, Object* allocated, JSObject* prototype)
{
    const LayoutData& allocatedLayout = allocated->layout();
    const LayoutData& allocatedClass = classOfInstances(allocatedLayout);
    const LayoutData* requested = isDefinedByClass(allocatedClass) ? classWithPrototype(prototype) : nullptr;
    if (!requested || requested->index == allocatedClass.index) {
        allocated->setPrototypeAtCreation(vm, prototype);
        return allocated;
    }

    Ancestry ancestry = ancestryOf(*this, *requested, allocatedClass);
    if (ancestry == Ancestry::SameOrBelow && requested->shape) {
        Object* instance = createClassInstance(vm, *requested, prototype);
        allocated->forEachFieldInKeyOrder(m_state, [&](uint16_t index) {
            copyFieldToInstance(vm, m_state, *allocated, allocatedLayout.fields()[index], *instance);
        });
        return instance;
    }

    bool hasShapeOfRequestedClass = ancestry == Ancestry::Unknown && requested->shape;
    JSObject* instance = hasShapeOfRequestedClass ? static_cast<JSObject*>(createClassInstance(vm, *requested, prototype)) : createOrdinaryInstance(m_state.instance.globalObject, allocatedLayout.numberOfFields, prototype);
    allocated->forEachFieldInKeyOrder(m_state, [&](uint16_t index) {
        defineFieldOnInstance(vm, m_state, *allocated, allocatedLayout.fields()[index], *instance);
    });
    return instance;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
