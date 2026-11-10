/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkNumberForms.h"

namespace JSC { namespace AOT { namespace Typed {

struct ExpectationOfNumber {
    TypeID type;
    bool isDeclared { false };
    bool isHeld { false };
};
ExpectationOfNumber numbersExpectedOf(Walker&, ValueID input, ExpectationOfNumber found);
TypeID typeHeldAtPositionOf(Walker&, ValueID input);
bool usesSpeakForInt32(Walker&, ValueID input);
ExpectationOfNumber numbersExpectedOfSlotThatTurnsStep(Walker&, ValueID input, ExpectationOfNumber found);

struct TestOfNumber {
    ValueID input;
    TypeID tested;
};
bool walkStretchBehindTestsOfNumbers(Walker&, std::span<const TestOfNumber>, BlockID inputBlock, uint32_t begin, uint32_t end, BlockFrequency ofOtherArm);
uint32_t limitOfStretchOfNumbers(const Walker&, const BodyBlock&);
std::optional<uint32_t> findPlaceBehindUpdateOfElement(Walker&, const BodyValue& read);
Reference buildTestOfNumber(Walker&, Reference value, TypeID tested, Origin);
Reference buildProofOfNumber(Walker&, Reference value, Reference test, Origin);
bool tryGuardNumericOperand(Walker&, Draft&, ValueID written);

inline constexpr OpcodeSet opcodesThatBuildArmsInFrontOfTypedFormsTakes = allSums | allBitwiseOperators | allRelations | OpcodeSet { Opcode::Mul, Opcode::Negate, Opcode::StrictEq };
AOT_LINK_OF_WALK(buildArmsInFrontOfTypedForms, opcodesThatBuildArmsInFrontOfTypedFormsTakes)
inline constexpr OpcodeSet opcodesThatBuildOperatorBesideOneNumberTakes { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
AOT_LINK_OF_WALK(buildOperatorBesideOneNumber, opcodesThatBuildOperatorBesideOneNumberTakes)
bool isTaggedNumber(Walker&, Reference);
bool sumStaysBoxed(Walker&, const BodyValue& written, const Draft& sum);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
