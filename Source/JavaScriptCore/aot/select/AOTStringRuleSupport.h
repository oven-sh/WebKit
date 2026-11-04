/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInductionVariables.h"
#include "AOTRuleSupport.h"
#include "AOTStringRanges.h"
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct CharacterRead {
    ValueID user { };
    Opcode rangeRead { Opcode::StringRangeCodeUnitOrNaN };
    ValueID position { };
    ValueID radix { };
    StringID literal { };
    bool needsBounds { false };
};

std::optional<CharacterRead> characterReadOf(SelectionContext&, const Use& useOfString);
Value makeCharacterRead(const CharacterRead&, ValueID string, std::optional<StringRangeBounds>);

struct LiteralArgument {
    StringID literal;
    String text;
};

std::optional<LiteralArgument> literalArgumentOf(SelectionContext&, ValueID);
std::optional<LiteralArgument> comparedLiteralOf(SelectionContext&, const Use& useInComparison);

inline constexpr std::array relationalOpcodesOfStrings { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

static_assert([] {
    OpcodeSet opcodes;
    for (Opcode opcode : relationalOpcodesOfStrings)
        opcodes.add(opcode);
    return opcodes == relationalOperators;
}());

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
