/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTUseLists.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct FunctionPassContext;

enum class StringStateUse : uint8_t { Unknown, Length, Truthiness, Read };

struct StringStateRead {
    Use use;
    ValueID member;
    StringStateUse kind { StringStateUse::Unknown };
    bool endsWeb { false };
    // Set where a builder cursor is live in front of the user: the string is made in front of this operation of the same block, where none is.
    ValueID operationBeforeCursors { };
};

struct StringWebEntry {
    ValueID value;
    ValueID concatenation;
    std::optional<Edge> edge;
    ValueID parameter;
};

struct StringStateEdgeDiscard {
    ValueID member;
    Edge edge;
};

struct StringStateDiscard {
    ValueID member;
    ValueID after;
};

struct StringWeb {
    StringBuilderDeclineReason decision { StringBuilderDeclineReason::Unknown };
    ValueID decidedAt;
    uint32_t numberOfAppends { 0 };
    Vector<ValueID, 8> members;
    Vector<StringWebEntry, 2> entries;
    Vector<StringStateRead, 4> reads;
    Vector<StringStateEdgeDiscard, 2> discardedEdges;
    Vector<StringStateDiscard, 2> discardedAfter;
};

class StringBuilderPlan {
public:
    std::span<const StringWeb> webs() const LIFETIME_BOUND { return m_webs.span(); }
    std::optional<uint32_t> webOf(ValueID) const;
    void dump(PrintStream&) const;

private:
    friend StringBuilderPlan analyzeStringBuilders(FunctionPassContext&);

    Vector<StringWeb> m_webs;
    Vector<uint32_t> m_webOfValue;
};

inline constexpr OpcodeSet concatenationOpcodes { Opcode::Add, Opcode::Strcat };

bool isStringConcatenation(const Program&, const Function&, const Value&);
bool isStringBuilderPart(const Type&);
StringBuilderPlan analyzeStringBuilders(FunctionPassContext&);
bool mergeNestedStringWebs(FunctionPassContext&, const StringBuilderPlan&);
bool markLikelyKeys(FunctionPassContext&);
bool canBeMarkedAsLikelyKey(const Program&, const Function&, const Value&);
void markAsLikelyKey(Function&, ValueID);
bool isKeyOperand(const Program&, const Function&, const Use&);

inline constexpr uint32_t stateOperandIndex = 0;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
