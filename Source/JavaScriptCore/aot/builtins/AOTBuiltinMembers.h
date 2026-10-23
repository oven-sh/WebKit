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

// The member that the holder has of its own under the key. An engine private name finds the member that it is the private alias of.
BuiltinID findOwnMember(const Program&, BuiltinID holder, PropertyKeyID);
// Of what the host keeps under engine private names the table lists the private aliases alone, so it never says that a holder has nothing under such a key.
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
// The type a receiver is tested for to count as an instance of the class. Nothing where no test finds the instances.
std::optional<Type> testedTypeOfInstances(const Program&, BuiltinClassID, const Type& receiver);

enum class LocationProof : uint8_t { Unknown, Constant };

struct BuiltinMemberAnswer {
    BuiltinMemberLookup::Outcome outcome { BuiltinMemberLookup::Outcome::Unknown };
    BuiltinID member;
    LocationProof proof { LocationProof::Unknown };

    bool isFound() const { return outcome == BuiltinMemberLookup::Outcome::Found; }
    bool isConstant() const { return isFound() && proof == LocationProof::Constant; }
};

// A key that the holder never gains: any key for an object that is locked or not extensible. For another object with locked members, an index-like key and the keys that the table closes.
bool isClosedKey(const Program&, BuiltinID holder, PropertyKeyID);
// What a lookup of the key finds on the holder never changes: a member that start-up locks, a private alias, or a closed key.
bool isLockedLocation(const Program&, BuiltinID holder, PropertyKeyID);

LocationProof locationProofOf(const BuiltinLookupContext&, BuiltinID member);
LocationProof absenceProofOf(const BuiltinLookupContext&, BuiltinID holder, PropertyKeyID);

BuiltinMemberAnswer findMemberFrom(const BuiltinLookupContext&, BuiltinID holder, PropertyKeyID);
BuiltinMemberAnswer findMemberOfInstances(const BuiltinLookupContext&, BuiltinClassID, PropertyKeyID);

// ByName: the lookup is made at run time, each time. Constant: it finds the member, always. Absent: it finds nothing, ever. Bound: a record of the name remembers where the lookup ended.
enum class FreeNameAnswerKind : uint8_t { ByName, Constant, Absent, Bound };

struct FreeNameAnswer {
    FreeNameAnswerKind kind { FreeNameAnswerKind::ByName };
    BuiltinID member;
};

// What `name` and `globalThis.name` find.
FreeNameAnswer freeNameAnswerOf(const BuiltinLookupContext&, PropertyKeyID, bool seesScriptBindings);

struct MemberOfClasses {
    BuiltinID member;
    Vector<BuiltinClassID, 2> classes;
};

Vector<MemberOfClasses, 4> methodsNamed(const BuiltinTable&, BuiltinName);
// The same, found once for a name while a ProgramCaches stands.
Vector<MemberOfClasses, 4> methodsNamed(const Program&, BuiltinName);

BuiltinID singleBuiltinOf(const Type&);
BuiltinID singleBuiltinCalleeOf(const Program&, const Function&, const Value& call);

std::optional<Value> tryMakeBuiltinConstant(const BuiltinTable&, BuiltinID member);
// Also a member that holds a text.
std::optional<Value> tryMakeBuiltinConstant(const Program&, BuiltinID member);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
