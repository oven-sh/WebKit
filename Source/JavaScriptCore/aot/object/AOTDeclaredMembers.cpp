/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClassTable.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClosureTable.h"
#include "AOTImage.h"
#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTMemberGuards.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC { namespace AOT {

static std::optional<uint32_t> keyOf(VM& vm, UniquedStringImpl* uid)
{
    return vm.aotState().identifiers->numberOf(uid);
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
    auto locationOfFunction = [&](FunctionID function) -> std::optional<DeclaredMemberLocation> {
        return creation == MemberCreation::Allowed ? ensureDeclaredMemberLocation(vm, function) : findDeclaredMemberLocation(function, false);
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
            return resultAt(locationOfFunction(FunctionID(member->method->function)));
        if (member->isAccessor())
            return resultAt(creation == MemberCreation::Allowed ? ensureDeclaredMemberLocation(vm, *member) : findDeclaredMemberLocation(keyOfAccessor(*member), true));
        if (name == vm.propertyNames->constructor.impl())
            return resultAt(locationOfFunction(FunctionID(classLayout.classData().classConstructor)));
    }
    if (prototype && (prototype->classInfo() == ClassPrototype::info() || compiledLayoutOrNull(prototype->structure())))
        return std::nullopt;
    return LazyPrototypeLookupResult { std::nullopt, prototype };
}

ClassTable::DeclaredMemberLocation ClassTable::declaredMemberLocationAt(uint32_t location, bool isAccessor) const
{
    return { m_declaredMemberHolders[location / declaredMembersPerHolder], static_cast<PropertyOffset>(location % declaredMembersPerHolder), isAccessor };
}

std::optional<ClassTable::DeclaredMemberLocation> ClassTable::findDeclaredMemberLocation(FunctionID key, bool isAccessor) const
{
    auto known = m_declaredMemberLocations.find(key.index());
    if (known == m_declaredMemberLocations.end())
        return std::nullopt;
    return declaredMemberLocationAt(known->value, isAccessor);
}

template<typename CreateMember>
ClassTable::DeclaredMemberLocation ClassTable::ensureDeclaredMemberLocation(VM& vm, FunctionID key, bool isAccessor, const CreateMember& createMember)
{
    static_assert(declaredMembersPerHolder <= JSFinalObject::maxInlineCapacity);
    uint32_t location = m_numberOfDeclaredMemberLocations;
    if (auto known = m_declaredMemberLocations.find(key.index()); known != m_declaredMemberLocations.end())
        location = known->value;
    else {
        if (location == m_declaredMemberHolders.size() * declaredMembersPerHolder) {
            Structure* structure = location ? m_declaredMemberHolders[0]->structure() : JSFinalObject::createStructure(vm, m_state.instance.globalObject, jsNull(), declaredMembersPerHolder);
            m_declaredMemberHolders.append(constructEmptyObject(vm, structure));
            m_state.immortalCells->add(m_declaredMemberHolders.last());
        }
        JSCell* member = createMember();
        m_declaredMemberHolders.last()->putDirectOffset(vm, location % declaredMembersPerHolder, member);
        m_declaredMemberLocations.add(key.index(), location);
        ++m_numberOfDeclaredMemberLocations;
    }
    return declaredMemberLocationAt(location, isAccessor);
}

ClassTable::DeclaredMemberLocation ClassTable::ensureDeclaredMemberLocation(VM& vm, FunctionID function)
{
    return ensureDeclaredMemberLocation(vm, function, false, [&] {
        return closureObject(vm, m_state, function);
    });
}

ClassTable::DeclaredMemberLocation ClassTable::ensureDeclaredMemberLocation(VM& vm, const DeclaredMember& accessor)
{
    return ensureDeclaredMemberLocation(vm, keyOfAccessor(accessor), true, [&] {
        GetterSetter* getterSetter = createGetterSetter(vm, m_state, accessor);
        m_state.immortalCells->add(getterSetter);
        return getterSetter;
    });
}

GetterSetter* ClassTable::createDeclaredAccessor(VM& vm, const LayoutData& classLayout, UniquedStringImpl* name)
{
    auto member = findOwnDeclaredMember(classLayout, keyOf(vm, name));
    if (!member || !member->isAccessor())
        return nullptr;
    return createGetterSetter(vm, m_state, *member);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
