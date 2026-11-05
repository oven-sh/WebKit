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

inline constexpr uint16_t primitivesWithJSONText = Primitive::String | Primitive::Number | Primitive::Boolean | Primitive::Null;
inline constexpr uint16_t primitivesWithoutJSONText = Primitive::Undefined | Primitive::Absent | Primitive::AnySymbol;
inline constexpr PristineAspectSet aspectsOfArrayWrittenAsJSON { PristineAspects::Storage, PristineAspects::Names };

enum class JSONValueKind : uint8_t {
    Generic,
    Nullish,
    Scalar,
    Array,
    Objects,
    ObjectParts,
};

struct JSONValuePlan {
    JSONValueKind kind { JSONValueKind::Generic };
    bool isExpected { false };
    bool mayBeNullish { false };
    Opcode row { Opcode::JSONAppendValue };
    TypeID tested;
    TypeID element;
    Vector<LayoutID, 4> layouts;
    Vector<JSONValuePlan> parts;
};

struct JSONMemberPlan {
    PropertyKeyID key;
    uint32_t field { 0 };
};

struct JSONKeyOrderPlan {
    Vector<uint32_t, 8> created;
    Vector<JSONMemberPlan, 8> members;
};

struct JSONObjectPlan {
    LayoutID layout;
    Vector<JSONKeyOrderPlan, 2> orders;
    std::optional<Vector<JSONMemberPlan, 8>> membersInAnyOrder;
    OptionSet<FactOfCell> looksAtCell;
    Vector<LayoutID, 2> classesGuardedAgainstToJSON;
};

enum class JSONFieldListing : uint8_t {
    Unknown,
    LeftOut,
    Listed,
};
JSONFieldListing listingOfJSONField(const AccessContext&, LayoutID, uint32_t field);
bool planLooksAtJSONObject(const AccessContext&, LayoutID, JSONObjectPlan&);

bool hasNoJSONText(const AccessContext&, TypeID);
JSONValuePlan planJSONValue(const AccessContext&, TypeID proven, TypeID expected);
std::optional<JSONObjectPlan> planJSONObject(const AccessContext&, LayoutID);
TypeID provenTypeOfJSONMember(const AccessContext&, LayoutID, uint32_t field);
TypeID writtenTypeOfJSONMember(const AccessContext&, LayoutID, uint32_t field);


inline constexpr uint32_t mostJSONMembersWrittenOutInOneUnit = 64;
uint32_t numberOfJSONMembersWrittenOutFor(const AccessContext&, const JSONObjectPlan&);
bool holdsListAmongJSONMembers(const AccessContext&, const JSONObjectPlan&);
inline uint32_t roomForJSONMembersInsideObjectOfSerializer(const AccessContext& context, const JSONObjectPlan& plan)
{
    return mostJSONMembersWrittenOutInOneUnit - std::min(mostJSONMembersWrittenOutInOneUnit, numberOfJSONMembersWrittenOutFor(context, plan));
}

inline constexpr int64_t mostJSONLevels = 40000;

struct JSONListAround {
    TypeID list;
    TypeID element;

    friend bool operator==(const JSONListAround&, const JSONListAround&) = default;
};

bool repeatsJSONListAround(std::span<const JSONListAround> listsAround, const JSONListAround&);

bool writesWholeJSONValue(const AccessContext&, const JSONValuePlan&, TypeID typeOfPlan, bool& callsSerializers);

struct CodeRunByJSONStringify {
    bool runsNoCode { false };
    bool cannotThrow { false };
};
CodeRunByJSONStringify codeRunByJSONStringifyOf(const AccessContext&, TypeID operand);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
