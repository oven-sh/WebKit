/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTOpcodeSets.h"
#include "AOTType.h"
#include "PureNaN.h"
#include <bit>
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;

// What Folding knows of a body without walking it. The rules are those of AOTWalkFolding.cpp, one set for the walk and for the scan.
// The scan looks into no block that they rule out, and Link hears of nothing that stands there. So the walk rules out at least as much: it asks the same rules, and knows more of the operands.

// A symbol and a BigInt are never known: each is a cell.
class PrimitiveKnownToFolding {
public:
    enum class Kind : uint8_t { Unknown, Undefined, Null, Boolean, Number, Text };

    PrimitiveKnownToFolding() = default;

    static PrimitiveKnownToFolding undefined() { return { Kind::Undefined, 0 }; }
    static PrimitiveKnownToFolding null() { return { Kind::Null, 0 }; }
    static PrimitiveKnownToFolding boolean(bool value) { return { Kind::Boolean, value }; }
    static PrimitiveKnownToFolding number(double value) { return { Kind::Number, std::bit_cast<uint64_t>(purifyNaN(value)) }; }
    // Integer types reach past 2^53, where not every integer is a number of JavaScript: such an integer is not known.
    static PrimitiveKnownToFolding integer(int64_t value)
    {
        double asNumber = static_cast<double>(value);
        return static_cast<int64_t>(asNumber) == value ? number(asNumber) : PrimitiveKnownToFolding();
    }
    // Of the program's table, or made by TypeArena::internText(). The two homes may hold one text under two ids: TypeArena::spellSameText().
    static PrimitiveKnownToFolding text(StringID value) { return { Kind::Text, value.bits() }; }

    explicit operator bool() const { return m_kind != Kind::Unknown; }
    Kind kind() const { return m_kind; }
    bool isNullish() const { return m_kind == Kind::Undefined || m_kind == Kind::Null; }
    bool isText() const { return m_kind == Kind::Text; }

    bool asBoolean() const
    {
        RELEASE_ASSERT(m_kind == Kind::Boolean);
        return m_bits;
    }
    double asNumber() const
    {
        RELEASE_ASSERT(m_kind == Kind::Number);
        return std::bit_cast<double>(m_bits);
    }
    StringID asText() const
    {
        RELEASE_ASSERT(m_kind == Kind::Text);
        return StringID::fromBits(static_cast<uint32_t>(m_bits));
    }

private:
    PrimitiveKnownToFolding(Kind kind, uint64_t bits)
        : m_kind(kind)
        , m_bits(bits)
    {
    }

    Kind m_kind { Kind::Unknown };
    uint64_t m_bits { 0 };
};

constexpr bool hasRuleForFolding(Opcode opcode) { return hasKnownValueRuleOf(opcode) || hasOwnFoldingRuleOf(opcode); }

inline constexpr OpcodeSet operationsOnPrimitivesInFolding = opcodesWhere([](Opcode opcode) {
    constexpr OpcodeSet evaluated = allNumericOperators | allRelations | allEqualities
        | OpcodeSet { Opcode::ToBoolean, Opcode::LogicalNot, Opcode::ToString, Opcode::ToPropertyKey, Opcode::ToInt32, Opcode::ToUint32, Opcode::GetStringLength };
    return evaluated.contains(opcode) && hasRuleForFolding(opcode);
});
inline constexpr OpcodeSet castsOfFolding = opcodesWhere([](Opcode opcode) { return opcodesThatStateTypes.contains(opcode) && hasKnownValueRuleOf(opcode); });
inline constexpr OpcodeSet primitivesWrittenForFolding { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };
// Of any other operation primitiveMadeOfConstants() knows nothing.
inline constexpr OpcodeSet operationsThatConstantsMayDecide = primitivesWrittenForFolding | castsOfFolding | operationsOnPrimitivesInFolding;

// What the operation gives in every run: it writes a primitive, or is made of such operations alone and neither throws nor runs code. `heldBy`: what this has answered for the definition of an operand.
// It makes no text. An operation that would have to is unknown here and known to the walk.
PrimitiveKnownToFolding primitiveMadeOfConstants(const Program&, const BodyValue&, NOESCAPE const ScopedLambda<PrimitiveKnownToFolding(ValueID)>& heldBy);

// The terminators that an operand may decide.
inline bool goesByOneOperand(TerminatorKind kind) { return kind == TerminatorKind::Branch || kind == TerminatorKind::Switch || kind == TerminatorKind::SwitchString; }
// The one target that such a terminator goes to with that operand. Nothing: the operand is not known.
std::optional<uint32_t> onlyTargetWith(TypeReader, const BodyTerminator&, PrimitiveKnownToFolding operand);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
