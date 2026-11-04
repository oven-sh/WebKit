/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinMembers.h"

#if ENABLE(AOT)

#include "AOTBuiltinOwnSlots.h"
#include "AOTPrimitives.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BuiltinClassOfPrimitives {
    NamedBuiltinClass builtinClass;
    uint16_t primitives;
};

} // anonymous namespace

static constexpr BuiltinClassOfPrimitives builtinClassesOfPrimitives[] = {
    { BuiltinClasses::String, Primitive::AnyString },
    { BuiltinClasses::Symbol, Primitive::AnySymbol },
    { BuiltinClasses::BigInt, Primitive::BigInt },
    { BuiltinClasses::Number, Primitive::Number },
    { BuiltinClasses::Boolean, Primitive::Boolean },
};

BuiltinMemberName builtinMemberNameOf(const Program& program, PropertyKeyID id)
{
    const PropertyKey& key = program.at(id);
    switch (key.kind) {
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return { program.builtinName(key.text), true };
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return { };
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return { program.builtinNameOf(id), false };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinID findOwnMember(const BuiltinTable& table, BuiltinID holder, BuiltinMemberName name)
{
    if (!name)
        return { };
    BuiltinID member = table.findOwnMember(holder, name.name);
    if (name.isPrivateAlias && member && !table.at(member).flags.contains(BuiltinFlag::HasPrivateAlias))
        return { };
    return member;
}

BuiltinID findOwnMember(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    return findOwnMember(program.builtins(), holder, builtinMemberNameOf(program, key));
}

bool listsEveryMemberUnder(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    constexpr OptionSet<BuiltinFlag> flagsOfCompleteLists { BuiltinFlag::ListsAllMembers, BuiltinFlag::IsGlobalObject };
    return program.builtins().at(holder).flags.containsAny(flagsOfCompleteLists) && program.at(key).kind != PropertyKeyKind::EnginePrivateName;
}

BuiltinClassID primitiveClassOf(uint16_t primitives)
{
    if (!primitives)
        return { };
    for (const BuiltinClassOfPrimitives& candidate : builtinClassesOfPrimitives) {
        if (!(primitives & ~candidate.primitives))
            return candidate.builtinClass;
    }
    return { };
}

TypeID typeOfValuesThatLookUpAsInstancesOf(TypeArena& types, BuiltinClassID builtinClass)
{
    static constexpr uint16_t primitivesWithClassOfTheirOwn[] { Primitive::String, Primitive::Symbol, Primitive::BigInt, Primitive::Number, Primitive::Boolean };
    for (uint16_t primitives : primitivesWithClassOfTheirOwn) {
        if (primitiveClassOf(primitives) == builtinClass)
            return TypeArena::fromPrimitives(primitives);
    }
    return isFunctionClass(types.builtins(), builtinClass) ? TypeID(TypeTable::function) : types.builtinClass(builtinClass);
}

MembersUnderKey::MembersUnderKey(const Program& program, PropertyKeyID key)
    : m_program(program)
    , m_table(program.builtins())
    , m_memberName(builtinMemberNameOf(program, key))
    , m_name(program.builtinNameOf(key))
    , m_kind(program.at(key).kind)
    , m_isIndexLike(program.isIndexLike(key))
{
    if (program.openedNames.isEmpty() || m_kind != PropertyKeyKind::Text) [[likely]]
        return;
    // A large test run opens names by the hundred thousand, and this is asked for every access by name.
    StringID text = program.at(key).text;
    if (program.isOpenedSomewhere(text)) [[unlikely]]
        m_textOpenedSomewhere = text;
}

BuiltinID MembersUnderKey::findOwnMember(BuiltinID holder) const
{
    return Typed::findOwnMember(m_table, holder, m_memberName);
}

bool MembersUnderKey::isClosedKey(BuiltinID holder) const
{
    if (m_textOpenedSomewhere && m_program.isOpened(holder, m_textOpenedSomewhere)) [[unlikely]]
        return false;
    if (m_table.at(holder).flags.contains(BuiltinFlag::IsLockedObject))
        return true;
    if (m_isIndexLike)
        return m_table.closesIndexKeys(holder);
    if (m_name)
        return m_table.isClosedKey(holder, m_name);
    return m_table.at(holder).flags.contains(BuiltinFlag::IsClosedToOtherNames) && m_kind == PropertyKeyKind::Text;
}

bool MembersUnderKey::isLockedLocation(BuiltinID holder) const
{
    BuiltinID member = findOwnMember(holder);
    if (!member)
        return isClosedKey(holder);
    if (m_kind == PropertyKeyKind::EnginePrivateName)
        return true;
    if (m_table.at(member).mutability == BuiltinMutability::AlwaysLocked || m_table.isLocationOfLockedObject(member))
        return true;
    return m_table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) && m_table.at(holder).flags.contains(BuiltinFlag::HasLockedMembers);
}

bool isClosedKey(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    return MembersUnderKey(program, key).isClosedKey(holder);
}

bool isLockedLocation(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    return MembersUnderKey(program, key).isLockedLocation(holder);
}

Vector<MemberOfClasses, 4> methodsNamed(const BuiltinTable& table, BuiltinName name)
{
    Vector<MemberOfClasses, 4> methods;
    for (BuiltinClassID builtinClass : table.classesWithInstanceMember(name)) {
        BuiltinMemberLookup found = table.findForInstance(builtinClass, name);
        if (found.outcome != BuiltinMemberLookup::Outcome::Found)
            continue;
        BuiltinID member = table.canonicalID(found.member);
        if (table.at(member).kind != BuiltinKind::Method)
            continue;
        auto existing = std::ranges::find(methods, member, &MemberOfClasses::member);
        if (existing == methods.end()) {
            methods.append(MemberOfClasses { member, { builtinClass } });
            continue;
        }
        existing->classes.append(builtinClass);
    }
    AOT::sortInLine(methods, { }, &MemberOfClasses::member);
    return methods;
}

BuiltinOverloadID findOwnSlotOfInstances(const BuiltinTable& table, BuiltinClassID builtinClass, BuiltinName name)
{
    BuiltinID member = table.findOwnPropertyOfInstances(builtinClass, name);
    if (!member || table.receiverClassOf(member) != builtinClass)
        return { };
    for (BuiltinOverloadID id : table.overloadIDsOf(member)) {
        if (table.at(id).code.implementation.kind == BuiltinImplementationKind::OwnSlot)
            return id;
    }
    return { };
}

bool liesAtSamePlace(const BuiltinTable& table, BuiltinOverloadID ownSlot, BuiltinOverloadID otherOwnSlot)
{
    auto placeOf = [&](BuiltinOverloadID id) {
        const BuiltinOverload& overload = table.at(id);
        return propertyOffsetOfOwnSlot(table.at(table.receiverClassOf(overload.builtin)).type, overload.code.implementation.ownSlot.offset);
    };
    return placeOf(ownSlot) == placeOf(otherOwnSlot);
}

// The holder adds the member behind the properties that it has when the member is first read. From the first copy of the holder on, its keys stand in that order.
bool firstReadChangesListedKeys(const BuiltinTable& table, BuiltinID member)
{
    return table.at(member).isAddedAtFirstRead;
}

bool standsForFirstReadOffHolder(const BuiltinTable& table, BuiltinID written, bool isNotReadFromHolder)
{
    if (!written || isNotReadFromHolder)
        return false;
    return isMadeWithCodeOfLibrary(table, written) || (table.at(written).isAddedAtFirstRead && table.at(written).holder);
}

bool onlyAddsMemberToHolder(const BuiltinTable& table, BuiltinID member)
{
    return !isMadeWithCodeOfLibrary(table, member);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
