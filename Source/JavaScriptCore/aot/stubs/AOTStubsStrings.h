/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "RegisterSet.h"
#include <initializer_list>

namespace JSC { namespace AOT {

constexpr StubKey ropeCharacters() { return StubKey(StubFamily::RopeCharacters); }
constexpr StubKey substringCharacters() { return StubKey(StubFamily::SubstringCharacters); }
constexpr StubKey flattenString() { return StubKey(StubFamily::FlattenString); }
constexpr StubKey stringCodeUnitAt() { return StubKey(StubFamily::StringCodeUnitAt); }
constexpr StubKey stringsAreEqual() { return StubKey(StubFamily::StringsAreEqual); }
constexpr StubKey stringBuilderAppendString() { return StubKey(StubFamily::StringBuilderAppendString); }
constexpr StubKey stringBuilderDiscard() { return StubKey(StubFamily::StringBuilderDiscard); }
constexpr StubKey stackStringBuilderAppendString() { return StubKey(StubFamily::StackStringBuilderAppendString); }
constexpr StubKey distinctStringsAreEqual() { return StubKey(StubFamily::DistinctStringsAreEqual); }
constexpr StubKey stringEqualsValue() { return StubKey(StubFamily::StringEqualsValue); }
constexpr StubKey stringEqualsNullable() { return StubKey(StubFamily::StringEqualsNullable); }
constexpr StubKey nullableStringsAreEqual(bool isStrict) { return StubKey(isStrict ? StubFamily::NullableStringsAreEqual : StubFamily::NullableStringsAreLooselyEqual); }
constexpr StubKey compareStrings() { return StubKey(StubFamily::CompareStrings); }
constexpr StubKey integerToString() { return StubKey(StubFamily::IntegerToString); }
enum class ConcatenatedKeyForm : uint8_t { Key, Name };
constexpr StubKey concatenateKeyOfPair(ConcatenatedKeyForm form) { return StubKey(StubFamily::ConcatenateKeyOfPair).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey substring() { return StubKey(StubFamily::Substring); }
constexpr StubKey characterAt() { return StubKey(StubFamily::CharacterAt); }
constexpr StubKey findCharacter8() { return StubKey(StubFamily::FindCharacter8); }
constexpr StubKey findText8() { return StubKey(StubFamily::FindText8); }

constexpr StubKey concatenate(unsigned count)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(count == 2 || count == 3);
    return StubKey(StubFamily::Concatenate).withCount(count);
}

enum class ConstantsOfConcatenation : uint8_t { FirstOfTwo, SecondOfTwo, FirstOfThree, SecondOfThree, ThirdOfThree, FirstAndThirdOfThree };

constexpr unsigned numberOfOperands(ConstantsOfConcatenation constants)
{
    return constants == ConstantsOfConcatenation::FirstOfTwo || constants == ConstantsOfConcatenation::SecondOfTwo ? 2 : 3;
}

constexpr bool isConstantOperand(ConstantsOfConcatenation constants, unsigned operand)
{
    switch (constants) {
    case ConstantsOfConcatenation::FirstOfTwo:
    case ConstantsOfConcatenation::FirstOfThree:
        return !operand;
    case ConstantsOfConcatenation::SecondOfTwo:
    case ConstantsOfConcatenation::SecondOfThree:
        return operand == 1;
    case ConstantsOfConcatenation::ThirdOfThree:
        return operand == 2;
    case ConstantsOfConcatenation::FirstAndThirdOfThree:
        return operand != 1;
    }
    return false;
}

inline constexpr unsigned numberOfConstantsOfConcatenation = 6;

constexpr StubKey convertAndConcatenate(std::optional<ConstantsOfConcatenation> constants, unsigned count)
{
    unsigned form = constants ? static_cast<unsigned>(*constants) : numberOfConstantsOfConcatenation + count - 2;
    return StubKey(StubFamily::ConvertAndConcatenate).withForm(static_cast<uint8_t>(form));
}

constexpr unsigned lastOperandThatIsNoConstant(std::optional<ConstantsOfConcatenation> constants, unsigned count)
{
    unsigned operand = count - 1;
    while (constants && isConstantOperand(*constants, operand))
        --operand;
    return operand;
}

constexpr StubKey concatenateWithConstants(ConstantsOfConcatenation constants) { return StubKey(StubFamily::ConcatenateWithConstants).withForm(static_cast<uint8_t>(constants)); }
enum class ConcatenatedInteger : uint8_t { StringThenInteger, IntegerThenString, StringIntegerString, StringsThenInteger, ConstantThenInteger, IntegerThenConstant, ConstantIntegerString, StringIntegerConstant, ConstantIntegerConstant, StringConstantInteger, ConstantStringInteger };
constexpr StubKey concatenateInteger(ConcatenatedInteger form) { return StubKey(StubFamily::ConcatenateInteger).withForm(static_cast<uint8_t>(form)); }
constexpr unsigned numberOfOperands(ConcatenatedInteger form)
{
    switch (form) {
    case ConcatenatedInteger::StringThenInteger:
    case ConcatenatedInteger::IntegerThenString:
    case ConcatenatedInteger::ConstantThenInteger:
    case ConcatenatedInteger::IntegerThenConstant:
        return 2;
    default:
        return 3;
    }
}

static_assert(static_cast<unsigned>(ConstantsOfConcatenation::FirstAndThirdOfThree) + 1 == numberOfConstantsOfConcatenation && descriptorOf(StubFamily::ConvertAndConcatenate).numberOfForms == numberOfConstantsOfConcatenation + 2);
static_assert(exists(ropeCharacters()) && exists(substringCharacters()) && exists(flattenString()));
static_assert(exists(stringsAreEqual()) && exists(compareStrings()) && exists(integerToString()) && exists(substring()));
static_assert(exists(distinctStringsAreEqual()) && bodyOf(distinctStringsAreEqual()) == stringsAreEqual());
static_assert(exists(stringEqualsValue()) && bodyOf(stringEqualsValue()) == stringsAreEqual());
static_assert(exists(stringEqualsNullable()) && bodyOf(stringEqualsNullable()) == stringsAreEqual());
static_assert(bodyOf(nullableStringsAreEqual(false)) == stringsAreEqual() && bodyOf(nullableStringsAreEqual(true)) == stringsAreEqual());
static_assert(exists(findCharacter8()) && exists(stringCodeUnitAt()));
static_assert(exists(concatenateKeyOfPair(ConcatenatedKeyForm::Key)) && exists(concatenateKeyOfPair(ConcatenatedKeyForm::Name)));
static_assert(exists(concatenate(2)) && exists(concatenate(3)));
static_assert(exists(concatenateWithConstants(ConstantsOfConcatenation::FirstAndThirdOfThree)) && !exists(concatenateWithConstants(static_cast<ConstantsOfConcatenation>(6))));
static_assert(!exists(concatenate(2).withCount(1)) && !exists(concatenate(3).withCount(4)));

void returnResult(StubAssembler& jit, auto value)
{
    jit.move(value, jit.result());
    jit.returnToCaller();
}

void loadStringLength(StubAssembler&, GPRReg string, GPRReg fiber, GPRReg length);
struct OrderedFlatStrings {
    GPRReg first;
    GPRReg second;
    GPRReg firstImpl;
    GPRReg secondImpl;
    GPRReg length;
    GPRReg firstWord;
    GPRReg secondWord;
};
void branchOnOrderOfFlatStrings(StubAssembler&, const OrderedFlatStrings&, MacroAssembler::JumpList& needsLeaf, MacroAssembler::JumpList& isLess, MacroAssembler::JumpList& isEqual);
PreludeExit emitToLowerCasePrelude(StubAssembler&);
PreludeExit emitToUpperCasePrelude(StubAssembler&);
PreludeExit emitStringLocaleComparePrelude(StubAssembler&);

struct LocaleCompareRegisters {
    GPRReg left;
    GPRReg right;
    GPRReg leftCharacters;
    GPRReg rightCharacters;
    GPRReg remaining;
    GPRReg leftUnit;
    GPRReg rightUnit;
    GPRReg weights;
};
void emitLocaleCompareOfASCII(StubAssembler&, const LocaleCompareRegisters&, const ScopedLambda<void(int32_t)>& returnOrder, JumpList& needsCollator);

void loadSubstringBaseAndOffset(StubAssembler&, GPRReg string, GPRReg base, GPRReg offset);
void packSubstringFibers(StubAssembler&, GPRReg base, GPRReg offset, GPRReg length, GPRReg lengthWord);
void loadSubstringCharacters(StubAssembler&, GPRReg string, GPRReg impl, GPRReg characters, GPRReg length, JumpList& isNotSubstring);
void load8BitCharactersOfSubstring(StubAssembler&, GPRReg string, GPRReg fiber, GPRReg offset, MacroAssembler::JumpList& isOther);
struct ComparedBytes {
    GPRReg first;
    GPRReg second;
    GPRReg size;
    GPRReg firstWord;
    GPRReg secondWord;
};
enum class DifferingWords : bool { AreLoaded, MayBeUnloaded };
enum class StartsOfEqualBytes : bool { EndAnywhere, EndBehindBytes };
void branchIfBytesDiffer(StubAssembler&, const ComparedBytes&, MacroAssembler::JumpList& differ, DifferingWords = DifferingWords::AreLoaded, StartsOfEqualBytes = StartsOfEqualBytes::EndAnywhere);
struct ComparedImpls {
    GPRReg first;
    GPRReg second;
    GPRReg length;
    GPRReg flagsOfBoth;
    GPRReg word;
};
void branchIfCharactersDiffer(StubAssembler&, const ComparedImpls&, MacroAssembler::JumpList& differ, MacroAssembler::JumpList& needsLeaf);
struct ComparedStrings {
    GPRReg first;
    GPRReg second;
    GPRReg firstFiber;
    GPRReg secondFiber;
    GPRReg length;
    GPRReg offset;
    GPRReg word;
};
void branchIfCharactersInPlaceDiffer(StubAssembler&, const ComparedStrings&, MacroAssembler::JumpList& differ, MacroAssembler::JumpList& needsOperation);
struct TextSoughtAtOnePlace {
    GPRReg string;
    GPRReg sought;
    GPRReg place;
    GPRReg characters;
    GPRReg soughtCharacters;
    GPRReg length;
    GPRReg word;
};
enum class PlaceOfSoughtText : uint8_t { IsStart, IsEnd, IsGiven, IsGivenAndLast };
void branchUnlessTextStandsAtPlace(StubAssembler&, const TextSoughtAtOnePlace&, PlaceOfSoughtText, MacroAssembler::JumpList& isNotThere, MacroAssembler::JumpList& needsOperation);
struct ScannedBytes {
    GPRReg bytes;
    GPRReg size;
    GPRReg word;
    GPRReg atLeastFirst;
    GPRReg aboveLast;
    GPRReg addendForFirst;
    GPRReg addendForLast;
};
void branchUnlessASCIIOutsideRange(StubAssembler&, const ScannedBytes&, uint8_t first, uint8_t last, MacroAssembler::JumpList& isNot);
void flattenSubject(StubAssembler&, GPRReg string, std::initializer_list<GPRReg> preservedGPRs, Label continuation);

struct FlatSubstringRegisters {
    GPRReg string;
    GPRReg start;
    GPRReg length;
    GPRReg result;
    GPRReg firstScratch;
    GPRReg secondScratch;
    RegisterSet alsoKept { };
};
[[nodiscard]] AssemblyHelpers::FreeListRefill makeSubstringOfFlatString(StubAssembler&, const FlatSubstringRegisters&, MacroAssembler::JumpList& fails);

void locateKeysOfPair(StubAssembler&, GPRReg first, GPRReg second, GPRReg set, GPRReg scratch);

void makeRopeOfTwoValuesAndReturn(StubAssembler&, GPRReg first, GPRReg second, MacroAssembler::JumpList& slow);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
