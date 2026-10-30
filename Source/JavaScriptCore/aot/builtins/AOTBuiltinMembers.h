/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;

struct BuiltinLookupContext {
    const Program& program;
    const FlowResults& flow;
};

struct BuiltinMemberName {
    BuiltinName name;
    bool isPrivateAlias { false };

    explicit operator bool() const { return !!name; }
};

BuiltinMemberName builtinMemberNameOf(const Program&, PropertyKeyID);

BuiltinID findOwnMember(const Program&, BuiltinID holder, PropertyKeyID);
bool listsEveryMemberUnder(const Program&, BuiltinID holder, PropertyKeyID);

enum class BuiltinReceiverKind : uint8_t { Unknown, BuiltinObject, Instance, Primitive };

struct BuiltinReceiver {
    BuiltinReceiverKind kind { BuiltinReceiverKind::Unknown };
    BuiltinID object;
    BuiltinClassID builtinClass;

    explicit operator bool() const { return kind != BuiltinReceiverKind::Unknown; }
};

BuiltinReceiver builtinReceiverOf(const Program&, const Type&);
BuiltinClassID primitiveClassOf(uint16_t primitives);
Type typeOfInstances(const BuiltinTable&, BuiltinClassID, const Type& receiver);
std::optional<Type> testedTypeOfInstances(const Program&, BuiltinClassID, const Type& receiver);

enum class LocationProof : uint8_t { Unknown, Constant };

struct BuiltinMemberAnswer {
    BuiltinMemberLookup::Outcome outcome { BuiltinMemberLookup::Outcome::Unknown };
    BuiltinID member;
    LocationProof proof { LocationProof::Unknown };
    // The entry under the name that was looked for. `member` is the object that it holds, which other names may hold too.
    BuiltinID location { };

    bool isFound() const { return outcome == BuiltinMemberLookup::Outcome::Found; }
    bool isConstant() const { return isFound() && proof == LocationProof::Constant; }
};

bool isClosedKey(const Program&, BuiltinID holder, PropertyKeyID);
bool isLockedLocation(const Program&, BuiltinID holder, PropertyKeyID);

class MembersUnderKey {
public:
    MembersUnderKey(const Program&, PropertyKeyID);

    BuiltinID findOwnMember(BuiltinID holder) const;
    bool isClosedKey(BuiltinID holder) const;
    bool isLockedLocation(BuiltinID holder) const;
    // The objects that take no new name but this one: Program::openedNames.
    std::span<const BuiltinID> holdersOpenedForKey() const LIFETIME_BOUND { return m_holdersOpenedForKey.span(); }

    static constexpr unsigned numberOfClasses = 256;
    unsigned classOfKey() const;

private:
    const BuiltinTable& m_table;
    BuiltinMemberName m_memberName;
    BuiltinName m_name;
    PropertyKeyKind m_kind;
    bool m_isIndexLike;
    Vector<BuiltinID, 2> m_holdersOpenedForKey;
};

LocationProof locationProofOf(const BuiltinLookupContext&, BuiltinID member);
LocationProof absenceProofOf(const BuiltinLookupContext&, BuiltinID holder, PropertyKeyID);

BuiltinMemberAnswer findMemberFrom(const BuiltinLookupContext&, BuiltinID holder, PropertyKeyID);
BuiltinMemberAnswer findMemberOfInstances(const BuiltinLookupContext&, BuiltinClassID, PropertyKeyID);
// The read of the slot where the constructor of the class leaves the property. The instances of a class below have a Structure of their own, so each class answers for itself.
BuiltinOverloadID findOwnSlotOfInstances(const BuiltinTable&, BuiltinClassID, BuiltinName);
// Whether one load reads either slot.
bool liesAtSamePlace(const BuiltinTable&, BuiltinOverloadID ownSlot, BuiltinOverloadID otherOwnSlot);

enum class FreeNameAnswerKind : uint8_t { ByName, Constant, Absent, Bound };

struct FreeNameAnswer {
    FreeNameAnswerKind kind { FreeNameAnswerKind::ByName };
    BuiltinID member;
};

FreeNameAnswer freeNameAnswerOf(const BuiltinLookupContext&, PropertyKeyID, bool seesScriptBindings);

struct MemberOfClasses {
    BuiltinID member;
    Vector<BuiltinClassID, 2> classes;
};

Vector<MemberOfClasses, 4> methodsNamed(const BuiltinTable&, BuiltinName);
Vector<MemberOfClasses, 4> methodsNamed(const Program&, BuiltinName);

BuiltinID singleBuiltinOf(const Type&);
BuiltinID singleBuiltinCalleeOf(const Program&, const Function&, const Value& call);

std::optional<Value> tryMakeBuiltinConstant(const BuiltinTable&, BuiltinID member);
std::optional<Value> tryMakeBuiltinConstant(const Program&, BuiltinID member);
// The host creates some members when they are first read, with code of the library, and the program can see what that code does. Goes in front of the constant that replaces a read.
bool firstReadChangesListedKeys(const BuiltinTable&, const FlowResults&, BuiltinID member);
// Of a member that InitializeBuiltin names: no code of the library runs to make it.
bool onlyAddsMemberToHolder(const BuiltinTable&, BuiltinID member);
std::optional<Value> tryMakeInitializeBuiltin(const BuiltinTable&, const FlowResults&, BuiltinID member);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
