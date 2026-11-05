/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class TypeArena;

struct BuiltinMemberName {
    BuiltinName name;
    bool isPrivateAlias { false };

    explicit operator bool() const { return !!name; }
};

BuiltinMemberName builtinMemberNameOf(const Program&, PropertyKeyID);

BuiltinID findOwnMember(const Program&, BuiltinID holder, PropertyKeyID);
bool listsEveryMemberUnder(const Program&, BuiltinID holder, PropertyKeyID);
bool lookupPassesHolderLinkedAnew(const Program&, BuiltinID start, PropertyKeyID);

BuiltinClassID primitiveClassOf(uint16_t primitives);
bool isClassOfPrimitives(BuiltinClassID);
TypeID typeOfValuesThatLookUpAsInstancesOf(TypeArena&, BuiltinClassID);

bool isClosedKey(const Program&, BuiltinID holder, PropertyKeyID);
bool isLockedLocation(const Program&, BuiltinID holder, PropertyKeyID);

class MembersUnderKey {
public:
    MembersUnderKey(const Program&, PropertyKeyID);

    BuiltinID findOwnMember(BuiltinID holder) const;
    enum class NamesOpenedForTestRun : bool { MayLieOnHolder, AreAbsent };
    bool isClosedKey(BuiltinID holder, NamesOpenedForTestRun = NamesOpenedForTestRun::MayLieOnHolder) const;
    bool isLockedLocation(BuiltinID holder) const;

private:
    const Program& m_program;
    PropertyKeyID m_key;
    const BuiltinTable& m_table;
    BuiltinMemberName m_memberName;
    BuiltinName m_name;
    PropertyKeyKind m_kind;
    bool m_isIndexLike;
    StringID m_textOpenedSomewhere;
};

struct MemberOfClasses {
    BuiltinID member;
    Vector<BuiltinClassID, 2> classes;
};

bool isMadeWithCodeOfLibrary(const BuiltinTable&, BuiltinID member);
bool standsForFirstReadOffHolder(const BuiltinTable&, BuiltinID written, bool isNotReadFromHolder);
Opcode opcodeOfFirstReadOffHolder(const BuiltinTable&, BuiltinID location);
bool hasFeatureGuard(const BuiltinTable&, BuiltinID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
