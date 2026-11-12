/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinMembers.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTInliningAcrossFiles.h"
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

ALWAYS_INLINE_INTO_OTHER_FILES BuiltinMemberName builtinMemberNameOf(const Program& program, PropertyKeyID id)
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
    if (BuiltinName name = program.builtinNameOf(key); name && program.builtins().isClosedOnHostFunctions(name) && program.builtins().isNativeFunctionOfHost(holder))
        return true;
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

bool lookupPassesHolderLinkedAnew(const Program& program, BuiltinID start, PropertyKeyID key)
{
    const BuiltinTable& table = program.builtins();
    auto climb = [&](const Invocable<bool(BuiltinID)> auto& answers) {
        BuiltinID holder = table.canonicalID(start);
        while (true) {
            if (answers(holder))
                return false;
            const Builtin& object = table.at(holder);
            switch (object.prototypeLink) {
            case BuiltinPrototypeLink::Unknown:
                return object.kind == BuiltinKind::Prototype;
            case BuiltinPrototypeLink::Null:
                return false;
            case BuiltinPrototypeLink::Builtin:
                holder = table.canonicalID(object.inheritsFrom);
                break;
            }
        }
    };
    if (!climb([](BuiltinID) { return false; }))
        return false;
    return !key || climb([&](BuiltinID holder) { return !!findOwnMember(program, holder, key); });
}

bool isClassOfPrimitives(BuiltinClassID builtinClass)
{
    return std::ranges::any_of(builtinClassesOfPrimitives, [&](const BuiltinClassOfPrimitives& candidate) {
        return candidate.builtinClass == builtinClass;
    });
}

bool isPrototypeOfPrimitives(const BuiltinTable& table, BuiltinID builtin)
{
    const Builtin& entry = table.at(table.canonicalID(builtin));
    return entry.kind == BuiltinKind::Prototype && entry.instanceClass && isClassOfPrimitives(entry.instanceClass);
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
    , m_key(key)
    , m_table(program.builtins())
    , m_memberName(builtinMemberNameOf(program, key))
    , m_name(program.builtinNameOf(key))
    , m_kind(program.at(key).kind)
    , m_isIndexLike(program.isIndexLike(key))
{
    if (program.openedNames.isEmpty() || m_kind != PropertyKeyKind::Text) [[likely]]
        return;
    StringID text = program.at(key).text;
    if (program.isOpenedSomewhere(text)) [[unlikely]]
        m_textOpenedSomewhere = text;
}

BuiltinID MembersUnderKey::findOwnMember(BuiltinID holder) const
{
    return Typed::findOwnMember(m_table, holder, m_memberName);
}

bool MembersUnderKey::isClosedKey(BuiltinID holder, NamesOpenedForTestRun opened) const
{
    if (opened == NamesOpenedForTestRun::MayLieOnHolder && m_textOpenedSomewhere && m_program.isOpened(holder, m_textOpenedSomewhere)) [[unlikely]]
        return false;
    if (m_table.at(holder).flags.contains(BuiltinFlag::IsLockedObject))
        return true;
    if (m_isIndexLike)
        return m_table.closesIndexKeys(holder);
    bool isClosedToOtherNames = m_table.at(holder).flags.contains(BuiltinFlag::IsClosedToOtherNames) && m_kind == PropertyKeyKind::Text;
    bool isClosed = m_name ? m_table.isClosedKey(holder, m_name) : isClosedToOtherNames;
    if (isClosed && isClosedToOtherNames && !m_program.admitsReplacedBuiltins)
        m_program.noteTakenForAbsent(m_key);
    return isClosed;
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

Opcode opcodeOfFirstReadOffHolder(const BuiltinTable& table, BuiltinID location)
{
    bool runsCodeOfLibrary = isMadeWithCodeOfLibrary(table, location) && BuiltinCellKey::value(table.canonicalID(location)).hasCell(table);
    return runsCodeOfLibrary ? Opcode::GetWatchedBuiltinValue : Opcode::InitializeBuiltin;
}

bool standsForFirstReadOffHolder(const BuiltinTable& table, BuiltinID written, bool isNotReadFromHolder)
{
    if (!written || isNotReadFromHolder)
        return false;
    return isMadeWithCodeOfLibrary(table, written) || (table.at(written).isAddedAtFirstRead && table.at(written).holder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
