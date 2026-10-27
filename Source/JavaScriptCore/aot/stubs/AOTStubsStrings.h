/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include <initializer_list>

namespace JSC { namespace AOT {

constexpr StubKey ropeCharacters() { return StubKey(StubFamily::RopeCharacters); }
constexpr StubKey flattenString() { return StubKey(StubFamily::FlattenString); }
constexpr StubKey stringsAreEqual() { return StubKey(StubFamily::StringsAreEqual); }
constexpr StubKey distinctStringsAreEqual() { return StubKey(StubFamily::DistinctStringsAreEqual); }
constexpr StubKey compareStrings() { return StubKey(StubFamily::CompareStrings); }
// The operands: a string and a position, which is in range where it is below the length as an unsigned 64-bit number. The result: the code point there, or undefinedInteger.
constexpr StubKey stringCodePointAt() { return StubKey(StubFamily::StringCodePointAt); }
// The operands: a builder and a string.
constexpr StubKey stringBuilderAppendString() { return StubKey(StubFamily::StringBuilderAppendString); }
// The operand: a builder.
constexpr StubKey stringBuilderDiscard() { return StubKey(StubFamily::StringBuilderDiscard); }
// The operands: a StackStringBuilder, its cursor and a string. The result: the cursor behind the string.
constexpr StubKey stackStringBuilderAppendString() { return StubKey(StubFamily::StackStringBuilderAppendString); }
constexpr StubKey integerToString() { return StubKey(StubFamily::IntegerToString); }
constexpr StubKey substring() { return StubKey(StubFamily::Substring); }
constexpr StubKey findCharacter8() { return StubKey(StubFamily::FindCharacter8); }

constexpr StubKey concatenate(unsigned count)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(count == 2 || count == 3);
    return StubKey(StubFamily::Concatenate).withCount(count);
}

static_assert(exists(ropeCharacters()) && exists(flattenString()) && exists(stringCodePointAt()) && exists(stringBuilderAppendString()) && exists(stringBuilderDiscard()) && exists(stackStringBuilderAppendString()));
static_assert(exists(stringsAreEqual()) && exists(compareStrings()) && exists(integerToString()) && exists(substring()) && exists(findCharacter8()));
static_assert(exists(distinctStringsAreEqual()) && bodyOf(distinctStringsAreEqual()) == stringsAreEqual());
static_assert(exists(concatenate(2)) && exists(concatenate(3)));
static_assert(!exists(concatenate(2).withCount(1)) && !exists(concatenate(3).withCount(4)));

// value is a register or an immediate.
void returnResult(StubAssembler& jit, auto value)
{
    jit.move(value, jit.result());
    jit.returnToCaller();
}

// AOTStubsStrings.cpp fiber holds the value word of string. length may be fiber.
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
// The impls are those of the two cells. Falls through when the first string comes after the second. At needsLeaf the impls are intact. Only the cells survive.
void branchOnOrderOfFlatStrings(StubAssembler&, const OrderedFlatStrings&, MacroAssembler::JumpList& needsLeaf, MacroAssembler::JumpList& isLess, MacroAssembler::JumpList& isEqual);

// AOTStubsStringHelpers.cpp string is a substring rope.
void loadSubstringBaseAndOffset(StubAssembler&, GPRReg string, GPRReg base, GPRReg offset);
// The inverse: lengthWord gets the word at offsetOfLength(), base the word after it. Overwrites offset. Bits 32 to 63 of length are zero.
void packSubstringFibers(StubAssembler&, GPRReg base, GPRReg offset, GPRReg length, GPRReg lengthWord);
// impl arrives with the value word of string, which is a rope. For a substring, impl gets the impl of the base and characters the address of the first unit.
void loadSubstringCharacters(StubAssembler&, GPRReg string, GPRReg impl, GPRReg characters, GPRReg length, JumpList& isNotSubstring);
// fiber arrives with the value word of string, which is a rope, and gets the address of the first character of a piece of an 8-bit string.
void load8BitCharactersOfSubstring(StubAssembler&, GPRReg string, GPRReg fiber, GPRReg offset, MacroAssembler::JumpList& isOther);
struct ComparedBytes {
    GPRReg first;
    GPRReg second;
    GPRReg size;
    GPRReg firstWord;
    GPRReg secondWord;
};
// Falls through when the bytes are the same.
enum class DifferingWords : bool { AreLoaded, MayBeUnloaded };
void branchIfBytesDiffer(StubAssembler&, const ComparedBytes&, MacroAssembler::JumpList& differ, DifferingWords = DifferingWords::AreLoaded);
struct ComparedImpls {
    GPRReg first;
    GPRReg second;
    GPRReg length;
    GPRReg flagsOfBoth;
    GPRReg word;
};
// Two impls of one length, with the flags that both have. Falls through when the characters are the same. At needsLeaf the impls are untouched, elsewhere all five registers are overwritten.
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
// Two strings of one length with their value words. Falls through when the characters are the same. At needsOperation the strings are untouched. word may be one of the strings.
void branchIfCharactersInPlaceDiffer(StubAssembler&, const ComparedStrings&, MacroAssembler::JumpList& differ, MacroAssembler::JumpList& needsOperation);
struct ScannedBytes {
    GPRReg bytes;
    GPRReg size;
    GPRReg word;
    GPRReg atLeastFirst;
    GPRReg aboveLast;
    GPRReg addendForFirst;
    GPRReg addendForLast;
};
// Falls through when every byte is below 0x80 and none is in first..last. Bits 32 to 63 of size are zero. Overwrites all seven registers.
void branchUnlessASCIIOutsideRange(StubAssembler&, const ScannedBytes&, uint8_t first, uint8_t last, MacroAssembler::JumpList& isNot);
// Calls operationAOTFlatten in a frame of its own, which only preservedGPRs survive, then jumps to continuation or to the exception handler. Overwrites T9.
void flattenSubject(StubAssembler&, GPRReg string, std::initializer_list<GPRReg> preservedGPRs, Label continuation);

PreludeExit emitJSONAppendValuePrelude(StubAssembler&);

// AOTStubsStringConversions.cpp A flat 8-bit string of ASCII characters without a letter of the other case is its own conversion.
PreludeExit emitToLowerCasePrelude(StubAssembler&);
PreludeExit emitToUpperCasePrelude(StubAssembler&);

// AOTStubsStringLiterals.cpp
struct LiteralStringProbeRegisters {
    GPRReg string;
    GPRReg impl;
    GPRReg entry;
    GPRReg tables;
    GPRReg maxLength;
};
// Probes the literal string cache of the instance and falls through with the word of the string in entry.
void findLiteralStringWord(StubAssembler&, const LiteralStringProbeRegisters&, JumpList& isEmpty, JumpList& isNotLiteral, JumpList& isNotCached);

// AOTStubsStringSubstrings.cpp
struct FlatSubstringRegisters {
    GPRReg string;
    GPRReg start;
    GPRReg length;
    GPRReg result;
    GPRReg firstScratch;
    GPRReg secondScratch;
};
// string is no rope, and bits 32 to 63 of start and length are zero.
[[nodiscard]] AssemblyHelpers::FreeListRefill makeSubstringOfFlatString(StubAssembler&, const FlatSubstringRegisters&, MacroAssembler::JumpList& fails);

// AOTStubsStringConcatenation.cpp Both are boxed values.
void makeRopeOfTwoValuesAndReturn(StubAssembler&, GPRReg first, GPRReg second, MacroAssembler::JumpList& slow);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
