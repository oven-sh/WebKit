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

// What the table says, and no more. Whether a member still lies where the table lists it is for Facts to say: walk/AOTMemberLookup.h asks both.
// Every PropertyKeyID passed in must come from the program's table. A key that Compile interned into a thread's arena (liesInThread()) crashes the build in Program::at().

struct BuiltinMemberName {
    BuiltinName name;
    bool isPrivateAlias { false };

    explicit operator bool() const { return !!name; }
};

BuiltinMemberName builtinMemberNameOf(const Program&, PropertyKeyID);

BuiltinID findOwnMember(const Program&, BuiltinID holder, PropertyKeyID);
bool listsEveryMemberUnder(const Program&, BuiltinID holder, PropertyKeyID);

BuiltinClassID primitiveClassOf(uint16_t primitives);
// The values whose lookups start where those of an instance of the class start: the primitives themselves for String, Number and the like, every function for a class of functions.
TypeID typeOfValuesThatLookUpAsInstancesOf(TypeArena&, BuiltinClassID);

bool isClosedKey(const Program&, BuiltinID holder, PropertyKeyID);
bool isLockedLocation(const Program&, BuiltinID holder, PropertyKeyID);

class MembersUnderKey {
public:
    MembersUnderKey(const Program&, PropertyKeyID);

    BuiltinID findOwnMember(BuiltinID holder) const;
    bool isClosedKey(BuiltinID holder) const;
    bool isLockedLocation(BuiltinID holder) const;

private:
    const Program& m_program;
    PropertyKeyID m_key;
    const BuiltinTable& m_table;
    BuiltinMemberName m_memberName;
    BuiltinName m_name;
    PropertyKeyKind m_kind;
    bool m_isIndexLike;
    // The key, where some object takes no new name but this one: Program::isOpened(). Else null.
    StringID m_textOpenedSomewhere;
};

struct MemberOfClasses {
    BuiltinID member;
    Vector<BuiltinClassID, 2> classes;
};

// The host makes the member with code of the library when it is first read. Defined in facts/AOTFlowTablesMembers.cpp, which includes this header.
bool isMadeWithCodeOfLibrary(const BuiltinTable&, BuiltinID member);
// Of a Builtin of the input: it stands for a read that does more than find the object. Whether anybody can tell: Facts::listsKeysOf().
bool standsForFirstReadOffHolder(const BuiltinTable&, BuiltinID written, bool isNotReadFromHolder);
// `--trim` may put a function that throws in place of the entry.
bool hasFeatureGuard(const BuiltinTable&, BuiltinID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
