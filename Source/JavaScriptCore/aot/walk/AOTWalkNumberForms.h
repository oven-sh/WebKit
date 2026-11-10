/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFoldingOfConstants.h"
#include "AOTWalk.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>

#include "AOTWalkStrings.h"

namespace JSC { namespace AOT { namespace Typed {

bool hasMechanismsOfNumberForms(const Walker&);
PrimitiveKnownToFolding primitiveKnownInNumberForms(const Walker&, Reference);
PrimitiveKnownToFolding primitiveKnownInNumberForms(const Walker&, Reference, const Type&);
std::optional<int64_t> integerKnownInNumberForms(const Walker&, Reference);
Reference integerInNumberForms(Walker&, int64_t);
Reference booleanInNumberForms(Walker&, bool);
Reference buildOperatorInNumberForms(Walker&, Opcode, Reference left, Reference right, Origin);
Reference buildInAllowedFormInNumberForms(Walker&, Draft&&);
void continueIfTrue(Walker&, Reference passes, BlockID otherwise);
Reference buildNarrowedInNumberForms(Walker&, Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge, Origin);
struct WayInNumberForms {
    BlockID end;
    Reference brought;
    Reference broughtBeside;
};
Reference meetInNumberForms(Walker&, BlockID meeting, std::span<const WayInNumberForms>, Reference* metBeside = nullptr);

struct DivisionAsWritten {
    ValueID dividend;
    ValueID divisor;
};
std::optional<DivisionAsWritten> findQuotientOfIntegers(Walker&, ValueID number, const InputsOfTypedOperation&);
struct WholeRemainder {
    ValueID dividend;
    ValueID divisor;
    bool mayBeNaN { false };
};
std::optional<WholeRemainder> findWholeRemainder(Walker&, ValueID number, const InputsOfTypedOperation&);
Reference buildDivisionOfIntegers(Walker&, Origin, Reference dividend, Reference divisor, QuotientRounding);
Reference buildWholeRemainder(Walker&, Origin, const WholeRemainder&);
TypeID typeOfWholeRemainder(Walker&, TypeID dividend, TypeID divisor);
Reference buildZeroWhereWholeRemainderIsZero(Walker&, Origin, const WholeRemainder&);
struct BitsOfDividendCompared {
    Reference bits;
    Reference comparedWith;
};
std::optional<BitsOfDividendCompared> tryCompareBitsOfDividend(Walker&, Origin, const WholeRemainder&, int64_t comparedWith);
Reference buildComparisonWhereDivisorsAreNotZero(Walker&, Draft&& comparison, std::span<const ValueID> divisorsThatMayBeZero);
bool buildNumberRemainderWithItsTest(Walker&, const BodyValue& remainder);

inline constexpr OpcodeSet opcodesThatBuildNumberFormTakes = allBitwiseOperators | allEqualities | allRelations | OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Pow,
    Opcode::LogicalNot, Opcode::ToBoolean };
AOT_LINK_OF_WALK(buildNumberForm, opcodesThatBuildNumberFormTakes)
inline constexpr OpcodeSet opcodesThatBuildOperatorOnWhatWalkHasBuiltTakes = opcodesThatBuildNumberFormTakes | opcodesThatBuildTypedOperatorTakes;
AOT_LINK_OF_WALK(buildOperatorOnWhatWalkHasBuilt, opcodesThatBuildOperatorOnWhatWalkHasBuiltTakes)


Reference buildCallOnNumbers(Walker&, const CallByOneRow&);
std::optional<PrimitiveKnownToFolding> foldExactly(Walker&, const CallByOneRow&);

Reference buildCharCode(Walker&, const CallByOneRow&);
Reference codeUnitBesideCharCode(Walker&, Reference number);
Reference codePointBesideCharacter(Walker&, Reference character);
Reference integerThatCharCodeIsKnownToBe(Walker&, Reference number);
Reference integerThatCharCodeIsKnownToBe(Walker&, const BodyValue& user, ValueID number);
bool buildComparisonByCodeUnit(Walker&, const BodyValue& comparison, const InputsOfTypedOperation&);

bool buildElementAccessUnderNumber(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool tellsNumberFromUndefinedInKeyOfView(Walker&, const BodyValue&, const TypesOfInputs&);

struct SwitchOnNumbers {
    Reference subject;
    Vector<int64_t, 8> cases;
};
std::optional<SwitchOnNumbers> planSwitchOnNumbers(Walker&, const BodyTerminator&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
