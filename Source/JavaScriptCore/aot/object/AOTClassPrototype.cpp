/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassPrototype.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include <algorithm>

namespace JSC { namespace AOT {

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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
