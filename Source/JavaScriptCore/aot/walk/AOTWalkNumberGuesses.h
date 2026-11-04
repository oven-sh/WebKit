/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkNumberForms.h"

namespace JSC { namespace AOT { namespace Typed {

// Numbers by expectation. A `ValueID` is a value of the body being read, a `Reference` is of Code.
// A null Reference or TypeID and `false` always mean: nothing was built, nothing is claimed, the caller goes on as if it had not asked.

// ---- AOTWalkNumberExpectations.cpp
struct ExpectationOfNumber {
    TypeID type;
    bool isDeclared { false };
    bool isHeld { false };
};
// For findExpectationForGuesses(). `found`: what was found, a null type where nothing was. It answers `found` or something better. NO PROOF: every user tests.
ExpectationOfNumber numbersExpectedOf(Walker&, ValueID input, ExpectationOfNumber found);
// Facts::held() of the entry parameter, field, variable, capture or result that `input` is read from. Null: none, or nothing is held.
TypeID typeHeldAtPositionOf(Walker&, ValueID input);
// By the body: behind casts and sums every mention of the value leads to a bit operator, an index, or a position or a count of a row of the table.
bool usesSpeakForInt32(Walker&, ValueID input);

// ---- AOTWalkNumberArms.cpp
struct TestOfNumber {
    ValueID input;
    TypeID tested;
};
// The tests in a row > operations [begin, end) of the block with every proof bound > ONE other arm, walked under Walker::ClosedArm with the values as they stand > the two meet.
// What the stretch defines and later code reads becomes a parameter. False: no test could be built, and nothing is walked. True: both arms have walked the stretch.
bool walkStretchBehindTestsOfNumbers(Walker&, std::span<const TestOfNumber>, BlockID inputBlock, uint32_t begin, uint32_t end, BlockFrequency ofOtherArm);
// For openGuessAt(), among the guesses that build their own arms: `x !== x`, `step(s) | 0`.
bool openGuessOfNumbers(Walker&, const BodyValue&);
// The same, a term of its own: `arguments[k]` with any key.
bool openGuessAtReadOfArguments(Walker&, const BodyValue&);
// For findStretchOfGuess(): an arithmetic or a bit operator whose form in the closed arm is Generic.
bool closedOperatorCallsStub(Walker&, const BodyValue&, const TypesOfInputs& inClosedArm);
// For the files of this package. Among the operations of the block: no stretch goes on to this place or past it.
uint32_t limitOfStretchOfNumbers(const Walker&, const BodyBlock&);
Reference buildTestOfNumber(Walker&, Reference value, TypeID tested, Origin);
Reference buildProofOfNumber(Walker&, Reference value, Reference test, Origin);

// ---- AOTWalkNumberUpdatesOfFields.cpp
// For buildReadForTypedAccess(), asked ahead. By the body: what the PropGet gives is sure to be stored back under the same name on the same object behind arithmetic alone,
// with nothing between that can ask for the property: `o.n++`, `o.n += s`.
bool isReadOfUpdateOfField(const Walker&, const BodyValue& read);

// ---- AOTWalkNumberPaths.cpp
// A link of the chain in front of callBuiltin(), behind `has(Mechanism::Guesses) && m_opensGuesses`.
inline constexpr OpcodeSet opcodesThatBuildArmsInFrontOfTypedFormsTakes = allSums | allBitwiseOperators | allRelations | OpcodeSet { Opcode::Mul, Opcode::Negate, Opcode::StrictEq }
    | operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise });
AOT_LINK_OF_WALK(buildArmsInFrontOfTypedForms, opcodesThatBuildArmsInFrontOfTypedFormsTakes)
// Behind openGuessAt(), in front of replaceOperation().
inline constexpr OpcodeSet opcodesThatBuildOperatorBesideOneNumberTakes { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
AOT_LINK_OF_WALK(buildOperatorBesideOneNumber, opcodesThatBuildOperatorBesideOneNumberTakes)
// Proven a number, boxed, may be an int32 and may be none.
bool isTaggedNumber(Walker&, Reference);
// For buildOperatorOnNumbers(). `sum`: the draft of the operation in the form that was chosen, with its operands as they are. True: the one user of the sum takes a box, and the code of the sum looks at the tags itself.
bool sumStaysBoxed(Walker&, const BodyValue& written, const Draft& sum);
// For buildOperatorOnNumbers(), of a bit operator in the form Numbers: `x | 0`, `x >>> 0` on a boxed number. True: the conversion is built and bound.
bool buildTruncationOfBoxedNumber(Walker&, const BodyValue&);

// ---- Struck by RULINGS.md, and kept as what gives the default: AOTWalkNumberArms.cpp.
// known-state-and-effects-1 E14 types the result of a call by the arguments at hand.
TypeID resultOfCallByArgumentsAtHand(const Walker&, TypeLevel, uint32_t resultIndex);
// strings-2 K8 answers a deterministic built-in in a realm of the build.
Reference foldCallInRealm(Walker&, const CallByOneRow&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
