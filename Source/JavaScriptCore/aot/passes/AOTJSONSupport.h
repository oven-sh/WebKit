/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

class FlowResults;

enum class JSONSupport : uint8_t {
    Unknown,
    Compiled,
};

enum class JSONMemberSupport : uint8_t {
    NotSerialized,
    // The key is among those that are serialized, whatever the slot can hold has no text, and finding that out runs no code.
    WithoutText,
    ByName,
    Slot,
};

inline constexpr uint16_t primitivesWithoutJSONText = Primitive::Undefined | Primitive::Absent | Primitive::AnySymbol;

JSONSupport jsonSupportOf(const Program&, const FlowResults&, LayoutID);
JSONMemberSupport jsonMemberSupportOf(const Program&, const FlowResults&, LayoutID, uint32_t field);

// How likely objects of a layout are the data that a program writes as JSON, the likeliest first.
struct JSONLikelihood {
    bool isOfLibrary { false };
    bool isInstance { false };
    uint32_t membersWithoutText { 0 };
    uint32_t members { 0 };
    uint32_t module { 0 };
    uint32_t position { 0 };

    friend auto operator<=>(const JSONLikelihood&, const JSONLikelihood&) = default;
};
JSONLikelihood jsonLikelihoodOf(const Program&, const FlowResults&, LayoutID);

bool isJSONSerializedKey(const Program&, PropertyKeyID);
bool hasNoToJSON(const Program&, const FlowResults&, LayoutID);
// Every value of the type is left out of an object and written as null in an array, and no code runs to find that out.
bool hasNoJSONText(const Program&, const FlowResults&, const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
