/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

// Columns and families of ir/AOTSchema.def as sets. Whoever asks one of these questions names the set: a list that a file keeps for itself answers differently as soon as an operation is added.

template<typename Holds>
constexpr OpcodeSet opcodesWhere(const Holds& holds)
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (holds(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}

constexpr OpcodeSet opcodesThatHandOn(std::initializer_list<HandedOn> ways)
{
    return opcodesWhere([&](Opcode opcode) {
        for (HandedOn way : ways) {
            if (handsOnOf(opcode) == way)
                return true;
        }
        return false;
    });
}

constexpr OpcodeSet operatorsOfKinds(std::initializer_list<OperatorKind> kinds)
{
    return opcodesWhere([&](Opcode opcode) {
        for (OperatorKind kind : kinds) {
            if (operatorKindOf(opcode) == kind)
                return true;
        }
        return false;
    });
}

constexpr OpcodeSet opcodesOfFamily(OperationFamily family)
{
    return opcodesWhere([&](Opcode opcode) { return familyOf(opcode) == family; });
}

// ---- handsOn. Each gives its first operand: the same value of JavaScript.

// A conversion is no name of what it converts: Known::rootOf() stops at it, and Known::cellBehindConversions() goes on.
inline constexpr OpcodeSet conversionsOfForm = opcodesThatHandOn({ HandedOn::Form });
// Known::rootOf() follows them. A check among them does not complete where it fails.
inline constexpr OpcodeSet opcodesThatNameTheirOperand = opcodesThatHandOn({ HandedOn::NarrowerType, HandedOn::PassedCheck, HandedOn::Mark });
inline constexpr OpcodeSet checksThatHandOnOrThrow = opcodesThatHandOn({ HandedOn::PassedCheck });
// It always completes.
inline constexpr OpcodeSet opcodesThatGiveSameValue = opcodesThatHandOn({ HandedOn::Form, HandedOn::NarrowerType });
// Every way in which a value goes on in Code, for whoever has no Known to ask.
inline constexpr OpcodeSet opcodesThatGiveSameCell = conversionsOfForm | opcodesThatNameTheirOperand;
// They stand in the input alone.
inline constexpr OpcodeSet opcodesThatStateTypes = opcodesThatHandOn({ HandedOn::UnclaimedType, HandedOn::ClaimedType, HandedOn::TestedField });
inline constexpr OpcodeSet castsThatClaimNothing = opcodesThatHandOn({ HandedOn::UnclaimedType });
// Every way in which a value goes on in the input, and the ones among them that never throw.
inline constexpr OpcodeSet inputOperationsThatHandOn = opcodesThatHandOn({ HandedOn::UnclaimedType, HandedOn::ClaimedType, HandedOn::TestedField, HandedOn::PassedCheck, HandedOn::Mark });
inline constexpr OpcodeSet inputOperationsThatOnlyHandOn = opcodesThatHandOn({ HandedOn::UnclaimedType, HandedOn::ClaimedType, HandedOn::TestedField, HandedOn::Mark });

static_assert(conversionsOfForm == opcodesOfFamily(OperationFamily::Representation));
static_assert(opcodesWhere([](Opcode opcode) { return resultRuleOf(opcode) == ResultRule::Cast && handsOnOf(opcode) == HandedOn::Nothing; }).isEmpty());
static_assert(opcodesWhere([](Opcode opcode) { return interpretedOf(opcode) == Interpretation::Cast && handsOnOf(opcode) == HandedOn::Nothing; }).isEmpty());

// ---- operatorKind, joinsTexts

inline constexpr OpcodeSet allSums = operatorsOfKinds({ OperatorKind::Additive });
inline constexpr OpcodeSet allBitwiseOperators = operatorsOfKinds({ OperatorKind::Bitwise });
inline constexpr OpcodeSet allRelations = operatorsOfKinds({ OperatorKind::Relation });
inline constexpr OpcodeSet allEqualities = operatorsOfKinds({ OperatorKind::Equality });
inline constexpr OpcodeSet allNumericOperators = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise, OperatorKind::NumberConversion });
inline constexpr OpcodeSet operationsThatJoinTexts = opcodesWhere([](Opcode opcode) { return joinsTextsOf(opcode); });
// Each makes a number or a BigInt of whatever it is given.
inline constexpr OpcodeSet operatorsThatMakeNumbers = opcodesWhere([](Opcode opcode) { return allNumericOperators.contains(opcode) && !joinsTextsOf(opcode); });

// ---- entersByValue, access

inline constexpr OpcodeSet callsByValue = opcodesWhere([](Opcode opcode) { return entersByValueOf(opcode) == EntryByValue::Call; });
inline constexpr OpcodeSet constructionsByValue = opcodesWhere([](Opcode opcode) { return entersByValueOf(opcode) == EntryByValue::Construction; });
// Under a name that the program can spell. A private name has operations of its own.
inline constexpr OpcodeSet storesByName = opcodesWhere([](Opcode opcode) {
    return familyOf(opcode) == familyOf(Opcode::PropGet) && accessClassOf(opcode) == AccessClass::ByName && (accessOf(opcode) == Access::Store || accessOf(opcode) == Access::Define);
});
static_assert(storesByName == OpcodeSet { Opcode::PropSet, Opcode::PropDefine });
// Each gives the cursor behind the part. A part of a builder in the heap gives nothing.
inline constexpr OpcodeSet partsOfStackStringBuilder = opcodesWhere([](Opcode opcode) { return familyOf(opcode) == OperationFamily::StringBuilderPart && resultRuleOf(opcode) != ResultRule::None; });

// ---- Families

// The primitives that the input writes out and whose value the walk holds as a KnownValue. A BigInt is a cell.
inline constexpr OpcodeSet primitiveConstants = opcodesWhere([](Opcode opcode) { return familyOf(opcode) == OperationFamily::Constant && hasKnownValueRuleOf(opcode); });
static_assert(primitiveConstants == OpcodeSet { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String });

inline constexpr OpcodeSet catchOpcodes = opcodesOfFamily(OperationFamily::Exception);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
