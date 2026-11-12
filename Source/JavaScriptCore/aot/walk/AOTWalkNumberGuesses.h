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
inline constexpr Range integersAddedWithoutRounding { -(1ll << 49), 1ll << 49 };
bool isOnlyTruncated(Walker&, ValueID input);
enum class TruncatedUse : uint8_t { None, OperandOfBits, TermOfSums };
TruncatedUse findTruncatedUse(Walker&, ValueID input);
bool isTruncatedInLoop(const BodyIndex&, ValueID read);
bool isOnlyComparedWithIntegers(Walker&, ValueID);
bool isSignOfZeroIgnored(Walker&, ValueID input);
bool isZeroSignUnused(Walker&, ValueID input);
bool isOnlyCalculatedWith(Walker&, ValueID input);
ExpectationOfNumber expectedIndexAddend(Walker&, const BodyValue& operation, ValueID operand, ExpectationOfNumber found);
ExpectationOfNumber numbersExpectedOfSlotThatTurnsStep(Walker&, ValueID input, ExpectationOfNumber found);
bool readsFieldOfIntegers(Walker&, ValueID operand);
ExpectationOfNumber expectedIntegerField(Walker&, const BodyValue& operation, ValueID operand, ExpectationOfNumber found);
bool readsSumOfWords(Walker&, ValueID operand);
ExpectationOfNumber numbersExpectedOfWord(Walker&, ValueID operand, ExpectationOfNumber found);

struct TestOfNumber {
    ValueID input;
    TypeID tested;
};
enum class EndOfNumberArms : uint8_t { Meet, MayTakeBranch };
struct NumberReadBeforeArms {
    ValueID read;
    Reference found;
    std::optional<Draft> readAgain;
};
bool walkStretchBehindTestsOfNumbers(Walker&, std::span<const TestOfNumber>, BlockID inputBlock, uint32_t begin, uint32_t end, BlockFrequency ofOtherArm, EndOfNumberArms = EndOfNumberArms::Meet, std::span<const NumberReadBeforeArms> = { });
uint32_t limitOfStretchOfNumbers(const Walker&, const BodyBlock&);
bool walkNumberPathAfterRead(Walker&, NumberReadBeforeArms&&);
NumberReadBeforeArms readAtSite(Walker&, const BodyValue& read);
void noteCheckedRead(Walker&, Reference found, Reference checked);
std::optional<uint32_t> findPlaceBehindUpdateOfElement(Walker&, const BodyValue& read);
Reference buildTestOfNumber(Walker&, Reference value, TypeID tested, Origin);
Reference buildProofOfNumber(Walker&, Reference value, Reference test, Origin, bool passes = true);
bool tryGuardNumericOperand(Walker&, Draft&, ValueID written);
bool tryGuardCounterBound(Walker&, Draft& relation, ValueID written);
bool tryGuardAddendOfIndex(Walker&, Draft& sum, ValueID written);

inline constexpr OpcodeSet opcodesThatBuildArmsInFrontOfTypedFormsTakes = allSums | allBitwiseOperators | allRelations | OpcodeSet { Opcode::Mul, Opcode::Negate, Opcode::StrictEq };
AOT_LINK_OF_WALK(buildArmsInFrontOfTypedForms, opcodesThatBuildArmsInFrontOfTypedFormsTakes)
inline constexpr OpcodeSet opcodesThatBuildOperatorBesideOneNumberTakes { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
AOT_LINK_OF_WALK(buildOperatorBesideOneNumber, opcodesThatBuildOperatorBesideOneNumberTakes)
bool isTaggedNumber(Walker&, Reference);
bool sumStaysBoxed(Walker&, const BodyValue& written, const Draft& sum);
bool keepsDifferenceBoxed(Walker&, const BodyValue& written, const Draft& difference);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
