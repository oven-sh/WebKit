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

constexpr OpcodeSet opcodesWithPassThrough(std::initializer_list<PassThroughKind> ways)
{
    return opcodesWhere([&](Opcode opcode) {
        for (PassThroughKind way : ways) {
            if (passThroughOf(opcode) == way)
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

inline constexpr OpcodeSet conversionsOfForm = opcodesWithPassThrough({ PassThroughKind::Form });
inline constexpr OpcodeSet opcodesThatNameTheirOperand = opcodesWithPassThrough({ PassThroughKind::NarrowerType, PassThroughKind::PassedCheck, PassThroughKind::Mark });
inline constexpr OpcodeSet passThroughChecks = opcodesWithPassThrough({ PassThroughKind::PassedCheck });
inline constexpr OpcodeSet valuePreservingOpcodes = opcodesWithPassThrough({ PassThroughKind::Form, PassThroughKind::NarrowerType });
inline constexpr OpcodeSet cellPreservingOpcodes = conversionsOfForm | opcodesThatNameTheirOperand;
inline constexpr OpcodeSet opcodesThatStateTypes = opcodesWithPassThrough({ PassThroughKind::UnclaimedType, PassThroughKind::ClaimedType, PassThroughKind::TestedField });
inline constexpr OpcodeSet castsThatClaimNothing = opcodesWithPassThrough({ PassThroughKind::UnclaimedType });
inline constexpr OpcodeSet inputPassThroughOpcodes = opcodesWithPassThrough({ PassThroughKind::UnclaimedType, PassThroughKind::ClaimedType, PassThroughKind::TestedField, PassThroughKind::PassedCheck, PassThroughKind::Mark });
inline constexpr OpcodeSet inputIdentityOpcodes = opcodesWithPassThrough({ PassThroughKind::UnclaimedType, PassThroughKind::ClaimedType, PassThroughKind::TestedField, PassThroughKind::Mark });

static_assert(conversionsOfForm == opcodesOfFamily(OperationFamily::Representation));
static_assert(opcodesWhere([](Opcode opcode) { return resultRuleOf(opcode) == ResultRule::Cast && passThroughOf(opcode) == PassThroughKind::Nothing; }).isEmpty());
static_assert(opcodesWhere([](Opcode opcode) { return interpretedOf(opcode) == Interpretation::Cast && passThroughOf(opcode) == PassThroughKind::Nothing; }).isEmpty());

inline constexpr OpcodeSet allSums = operatorsOfKinds({ OperatorKind::Additive });
inline constexpr OpcodeSet allBitwiseOperators = operatorsOfKinds({ OperatorKind::Bitwise });
inline constexpr OpcodeSet allRelations = operatorsOfKinds({ OperatorKind::Relation });
inline constexpr OpcodeSet allEqualities = operatorsOfKinds({ OperatorKind::Equality });
inline constexpr OpcodeSet allNumericOperators = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise, OperatorKind::NumberConversion });
inline constexpr OpcodeSet operationsThatJoinTexts = opcodesWhere([](Opcode opcode) { return joinsTextsOf(opcode); });
inline constexpr OpcodeSet numberProducingOperators = opcodesWhere([](Opcode opcode) { return allNumericOperators.contains(opcode) && !joinsTextsOf(opcode); });

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
