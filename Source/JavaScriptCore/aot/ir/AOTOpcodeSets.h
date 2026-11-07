/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

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

inline constexpr OpcodeSet conversionsOfForm = opcodesThatHandOn({ HandedOn::Form });
inline constexpr OpcodeSet opcodesThatNameTheirOperand = opcodesThatHandOn({ HandedOn::NarrowerType, HandedOn::PassedCheck, HandedOn::Mark });
inline constexpr OpcodeSet checksThatHandOnOrThrow = opcodesThatHandOn({ HandedOn::PassedCheck });
inline constexpr OpcodeSet opcodesThatGiveSameValue = opcodesThatHandOn({ HandedOn::Form, HandedOn::NarrowerType });
inline constexpr OpcodeSet opcodesThatGiveSameCell = conversionsOfForm | opcodesThatNameTheirOperand;
inline constexpr OpcodeSet opcodesThatStateTypes = opcodesThatHandOn({ HandedOn::UnclaimedType, HandedOn::ClaimedType, HandedOn::TestedField });
inline constexpr OpcodeSet castsThatClaimNothing = opcodesThatHandOn({ HandedOn::UnclaimedType });
inline constexpr OpcodeSet inputOperationsThatHandOn = opcodesThatHandOn({ HandedOn::UnclaimedType, HandedOn::ClaimedType, HandedOn::TestedField, HandedOn::PassedCheck, HandedOn::Mark });
inline constexpr OpcodeSet inputOperationsThatOnlyHandOn = opcodesThatHandOn({ HandedOn::UnclaimedType, HandedOn::ClaimedType, HandedOn::TestedField, HandedOn::Mark });

static_assert(conversionsOfForm == opcodesOfFamily(OperationFamily::Representation));
static_assert(opcodesWhere([](Opcode opcode) { return resultRuleOf(opcode) == ResultRule::Cast && handsOnOf(opcode) == HandedOn::Nothing; }).isEmpty());
static_assert(opcodesWhere([](Opcode opcode) { return interpretedOf(opcode) == Interpretation::Cast && handsOnOf(opcode) == HandedOn::Nothing; }).isEmpty());

inline constexpr OpcodeSet allSums = operatorsOfKinds({ OperatorKind::Additive });
inline constexpr OpcodeSet allBitwiseOperators = operatorsOfKinds({ OperatorKind::Bitwise });
inline constexpr OpcodeSet allRelations = operatorsOfKinds({ OperatorKind::Relation });
inline constexpr OpcodeSet allEqualities = operatorsOfKinds({ OperatorKind::Equality });
inline constexpr OpcodeSet allNumericOperators = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise, OperatorKind::NumberConversion });
inline constexpr OpcodeSet operationsThatJoinTexts = opcodesWhere([](Opcode opcode) { return joinsTextsOf(opcode); });
inline constexpr OpcodeSet operatorsThatMakeNumbers = opcodesWhere([](Opcode opcode) { return allNumericOperators.contains(opcode) && !joinsTextsOf(opcode); });

inline constexpr OpcodeSet callsByValue = opcodesWhere([](Opcode opcode) { return entersByValueOf(opcode) == EntryByValue::Call; });
inline constexpr OpcodeSet constructionsByValue = opcodesWhere([](Opcode opcode) { return entersByValueOf(opcode) == EntryByValue::Construction; });
inline constexpr OpcodeSet storesByName = opcodesWhere([](Opcode opcode) {
    return familyOf(opcode) == familyOf(Opcode::PropGet) && accessClassOf(opcode) == AccessClass::ByName && (accessOf(opcode) == Access::Store || accessOf(opcode) == Access::Define);
});
static_assert(storesByName == OpcodeSet { Opcode::PropSet, Opcode::PropDefine });
inline constexpr OpcodeSet partsOfStackStringBuilder = opcodesWhere([](Opcode opcode) { return familyOf(opcode) == OperationFamily::StringBuilderPart && resultRuleOf(opcode) != ResultRule::None; });

inline constexpr OpcodeSet primitiveConstants = opcodesWhere([](Opcode opcode) { return familyOf(opcode) == OperationFamily::Constant && hasKnownValueRuleOf(opcode); });
static_assert(primitiveConstants == OpcodeSet { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String });

inline constexpr OpcodeSet catchOpcodes = opcodesOfFamily(OperationFamily::Exception);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
