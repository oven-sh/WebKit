/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTJSONSerializerPlan.h"
#include "AOTMemberLookup.h"
#include "AOTWalkState.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// How JSON.stringify writes a value of a type, where compiled code can write it in place. Pure functions of types, the program's tables, Facts and Plan, like AOTMemberLookup.h: they build nothing.
// Nothing that is planned runs code of the program. So whatever a test turns away while the text is written is left to the call as written, which starts from the beginning.

inline constexpr uint16_t primitivesWithJSONText = Primitive::String | Primitive::Number | Primitive::Boolean | Primitive::Null;
inline constexpr uint16_t primitivesWithoutJSONText = Primitive::Undefined | Primitive::Absent | Primitive::AnySymbol;
inline constexpr PristineAspectSet aspectsOfArrayWrittenAsJSON { PristineAspects::Storage, PristineAspects::Names };

enum class JSONValueKind : uint8_t {
    // No plan. Of such a value a primitive is written in place, and anything else is turned away.
    Generic,
    // Undefined and null alone.
    Nullish,
    Scalar,
    Array,
    Objects,
    // The kinds of object in a union, each behind its test.
    ObjectParts,
};

struct JSONValuePlan {
    JSONValueKind kind { JSONValueKind::Generic };
    // The plan follows from the expected type: a value that fails its tests is no surprise to the proven one.
    bool isExpected { false };
    bool mayBeNullish { false };
    // Of a Scalar.
    Opcode row { Opcode::JSONAppendValue };
    // Of a Scalar and an Array: what a TypeTest asks for.
    TypeID tested;
    // Of an Array: what is written of its elements. Anything where nothing is.
    TypeID element;
    // Of Objects: in the order in which their shapes are tested.
    Vector<LayoutID, 4> layouts;
    // The layouts come last: what fails their tests has no plan.
    Vector<JSONValuePlan> parts;
};

struct JSONMemberPlan {
    PropertyKeyID key;
    uint32_t field { 0 };
};

// `class U { name = 'a'; nick?: string }`: the objects that have got a nick and those that have not are told apart by one test of the word for the order of their keys.
struct JSONKeyOrderPlan {
    // What HasKeyOrder asks for: the object got these fields in this order, and none of its other fields. Of a property that is no field the test says nothing: JSONObjectPlan::looksAtCell.
    Vector<uint32_t, 8> created;
    // In key order. A field whose slot makes its function at the first read is left out: it has no text.
    Vector<JSONMemberPlan, 8> members;
};

struct JSONObjectPlan {
    LayoutID layout;
    // In the order in which they are tested.
    Vector<JSONKeyOrderPlan, 2> orders;
    // `u.nick = n` behind the making of u: objects come in orders at which no maker stops. Such an object is written by its own order word, key by key, and each of these fields where the order lists it.
    // Nothing: an object in an order that is not among `orders` is turned away.
    std::optional<Vector<JSONMemberPlan, 8>> membersInAnyOrder;
    // What has to be SEEN of the cell in front of the members, beside the shape and the order of the keys. Each is one test, and no text of the object is written in front of the last of them:
    //   NoFieldOutsideSlot   HasNoFieldOutsideSlot: no field has become an accessor, has been hidden from enumeration, or has left its slot for a value that does not fit
    //   OrderWordIsKnown     HasNoExtraProperties: the object has no property but those that its order lists: no `o[k] = v`, no toJSON of its own
    //   HasLayoutPrototype   HasLayoutPrototype: it stands below the prototype object that `classesGuardedAgainstToJSON` speak of
    // The first two come together and are one test: HasNoExtraProperties [hasNoFieldOutsideSlot]. No other fact is ever asked for. A frozen object is written like any other,
    // so what passes is NOT known to be NotFrozen, and nobody may tell the state that it knows the whole order word.
    OptionSet<FactOfCell> looksAtCell;
    // The guards of classes that have to hold: no toJSON is found above the object. Of each, in AreMembersUnchanged: MemberConditionRecord { MemberConditionKind::MemberUnchanged, the class, ClassSide::Prototype, the key `toJSON` },
    // which answers for the whole row of classes above it. It speaks of an object that HasLayoutPrototype. Empty with a proof of Link.
    Vector<LayoutID, 2> classesGuardedAgainstToJSON;
};

// Of an object that has the field.
enum class JSONFieldListing : uint8_t {
    // An accessor, a key that nobody knows, an index.
    Unknown,
    // JSON.stringify passes it by: a symbol, a private name, what is hidden from enumeration from its definition on.
    LeftOut,
    Listed,
};
JSONFieldListing listingOfJSONField(const AccessContext&, LayoutID, uint32_t field);
// `looksAtCell` and `classesGuardedAgainstToJSON` of the plan: behind them an object of the layout finds no toJSON, has no property that is no field, and each of its fields is what listingOfJSONField() says.
// False: nothing that can be seen says so. It asks nothing of the order of the keys.
bool planLooksAtJSONObject(const AccessContext&, LayoutID, JSONObjectPlan&);

// Every value of the type is left out of an object and written as null in an array, and no code runs to find that out:
// undefined, a symbol, and a function under which no toJSON is found.
bool hasNoJSONText(const AccessContext&, TypeID);
// `expected` may be null.
JSONValuePlan planJSONValue(const AccessContext&, TypeID proven, TypeID expected);
// Nothing: an object of the layout may have a toJSON, a getter, a key that no slot stands for, or its keys in an order that nobody knows.
std::optional<JSONObjectPlan> planJSONObject(const AccessContext&, LayoutID);
// What lies in the slot of a field that the object has, by a proof: what Plan holds the slot to, and all that Link has seen arrive. Anything where neither speaks.
TypeID provenTypeOfJSONMember(const AccessContext&, LayoutID, uint32_t field);
// What the program writes of the member, which writesWholeJSONValue() goes by. Null: nothing is written.
TypeID writtenTypeOfJSONMember(const AccessContext&, LayoutID, uint32_t field);
// The overload of Date.prototype.getTime that takes a Date and asks nothing else of it. Null: there is none.
BuiltinOverloadID findGetTimeForJSON(const AccessContext&);


// The text of a member is written out wherever an object of its layout is written in place, so each unit writes out so many and no more. An object for whose members there is no room left is written by the serializer of its layout,
// and so is an object of a layout that is being written around it: `type T = { v: number; next?: T }`. CodeRole::Serializer.
inline constexpr uint32_t mostJSONMembersWrittenOutInOneUnit = 64;
// Each order has the text of its members written out, and so has the walk by the order word. A member that never has text takes none.
uint32_t numberOfJSONMembersWrittenOutFor(const AccessContext&, const JSONObjectPlan&);
// A list is written by a loop, and only a serializer has loops: at a site the run time writes it, and looks at each value to learn what it is. So a record with a list among its members is written by the serializer of its layout.
bool holdsListAmongJSONMembers(const AccessContext&, const JSONObjectPlan&);
// What the serializer of the layout has left for the objects inside its own, which it writes out whatever it takes.
inline uint32_t roomForJSONMembersInsideObjectOfSerializer(const AccessContext& context, const JSONObjectPlan& plan)
{
    return mostJSONMembersWrittenOutInOneUnit - std::min(mostJSONMembersWrittenOutInOneUnit, numberOfJSONMembersWrittenOutFor(context, plan));
}

// JSON.stringify throws at one more object or list inside one another than this, on every machine: maximumSideStackRecursion of runtime/JSONObject.cpp.
inline constexpr int64_t mostJSONLevels = 40000;

// A list that is being written: its proven type, and JSONValuePlan::element of its plan.
struct JSONListAround {
    TypeID list;
    TypeID element;

    friend bool operator==(const JSONListAround&, const JSONListAround&) = default;
};

// `b = [b]`, `tree = [tree, leaf]`: what is found in a list may be a list of the same literal, so the types of lists that lie directly in one another need not come to an end.
// What is read out of a list is typed by the type of the list, and there are finitely many types: the way from list to list ends, or it comes back to where it has been.
// `listsAround`: lists that lie directly in one another, with no object between them. A layout between two lists is written out once, which ends the way through it.
// True: it has come back, and from here on it repeats. Such a list is not written out inside itself.
bool repeatsJSONListAround(std::span<const JSONListAround> listsAround, const JSONListAround&);

// By the types, whatever the value reaches is written by compiled code. A member that may hold an object without a plan would turn the value away at every call, behind work that was for nothing.
// `callsSerializers`: some object that the value reaches is written by the serializer of its layout.
bool writesWholeJSONValue(const AccessContext&, const JSONValuePlan&, TypeID typeOfPlan, bool& callsSerializers);

// AOTWalkJSONWriteTrust.cpp. By proofs alone, with no test in front.
struct CodeRunByJSONStringify {
    // Native code lists the keys and reads the values of all that the operand reaches, and calls nothing: no toJSON, no getter, no proxy, no conversion of a boxed primitive.
    bool runsNoCode { false };
    // Beside that: no BigInt, no way back to an object that is being written, and a text that fits in a string. What is left is the lack of memory.
    bool cannotThrow { false };
};
// Of an operand of which isSerializedAsJSONStringifyDoes() holds: plan/AOTJSONSerializerPlan.h.
CodeRunByJSONStringify codeRunByJSONStringifyOf(const AccessContext&, TypeID operand);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
