/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFoldingOfConstants.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTWalk.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// ---- The doors. AOTWalkStrings.cpp.
// For buildTypedOperation() and typedFormAsReceiverStands(), in front of buildTypedElementAccess(). It builds where the inputs are what the walk has built, and only answers where they are types.
// ASKED AHEAD IT ANSWERS FOR ElemGet ALONE (a character of a proven string under a proven integer: noteFormAskedAhead(Opcode::Substring, KnowledgeLevel::Proven)); of any other opcode false, and buildTypedOperator() answers as today.
// The order. ElemGet: buildReadByPartsOfKey > buildAccessUnderOneOfFewTexts > buildCharacterByIndex > buildLookupUnderAnyKeyOnPlainRecords. ElemSet ElemHas ElemDelete ElemDefine: buildAccessUnderOneOfFewTexts > buildLookupUnderAnyKeyOnPlainRecords.
// allEqualities: buildEqualityOnStrings. allRelations: buildRelationOnStrings. ToBoolean: buildTruthinessOfLengthOfString. TypeTest: buildTestOfCharacterByCodeUnit. False: nothing was built.
inline constexpr OpcodeSet opcodesThatBuildTypedOperationOnStringsTakes = allEqualities | allRelations | OpcodeSet { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine,
    Opcode::PropGet, Opcode::ToBoolean, Opcode::TypeTest, Opcode::ToNumber, Opcode::ToNumeric };
AOT_LINK_OF_WALK_WITH_INPUTS(buildTypedOperationOnStrings, opcodesThatBuildTypedOperationOnStringsTakes)

// What BuiltinCalls has planned of a call that goes by ONE row (BuiltinCallForm::Whole), where its operands are settled: converted as the row converts them, undefined in the place of an argument that is left out.
struct CallByOneRow {
    const BodyValue& call; // The Call or Construct of the body, or the PropGet of a row of the mode Get.
    BuiltinID builtin; // Canonical.
    BuiltinOverloadID overload;
    BuiltinCallMode mode;
    std::optional<InlineBuiltin> codeInLine; // BuiltinImplementation::inlineBuiltin of the row, whichever form was chosen.
    Reference receiver; // Null: the row has none.
    std::span<const Reference> arguments;
    Origin origin; // isTailCall is false.
    ExpectedType expected;
    bool isForwarded { false }; // `Math.floor.call(a, b)`: `call` writes other operands than the function takes, and nobody reads them off it.
};
// For CallInWalkBuiltins::build(), behind buildWithRegExpWrittenForCall(). True: what stands for the call is built and bound to call.id(), or the walk stands nowhere. False: the call goes by its row; what was built by then nobody reads.
// The order: foldCallByOneRowOnKnownOperands > buildThrowOfNewError > buildFormatterCallByPattern > buildURLCallByScan > by codeInLine: buildCharacterReadByMethod, buildPieceByMethod, buildTestOfEndOfString, buildSearchInString.
bool buildCallByOneRowForStrings(Walker&, const CallByOneRow&);
// Beside it, of BuiltinCallForm::AnyReceiver: `new URL(req.url, base)`. Nothing says what the operands are, and the row only says which function is constructed. Whoever takes it tests what it needs.
bool buildConstructionOnAnyOperandsForStrings(Walker&, const CallByOneRow&);

// AOTWalkStringOperands.cpp. For callBuiltin(), in front of call.build(), where !call.goesByOneRow(). The callee is known, and ONE operand keeps the call from going by one row:
// the test of that operand, the call walked again on the Narrowed, the call as written in a Rare block under Walker::ClosedArm, the two meet. True: both arms are built. False: nothing was built.
// It asks opensGuesses(), has(Guesses), wouldCallBuiltin() with the tested type == ByOneRow, and shouldOpenGuess() in front of the first build().
bool openGuessAtOperandOfBuiltinCall(Walker&, const BodyValue& call);
// For findExpectationAnewForGuesses(), of a Call or Construct of which nothing is written and nothing is held. Null: nothing is expected. NO PROOF.
TypeID typeExpectedOfResultOfCall(Walker&, const BodyValue& call);

// ---- What every file of the mechanism reads and builds with. AOTWalkStrings.cpp. None opens a guess, none asks the state.
inline constexpr uint32_t startOfStringRange = 0;
inline constexpr uint32_t endOfStringRange = 1;
// The record behind every Narrowed. Of a constant: itself.
Reference rootForStrings(const Walker&, Reference);
// As primitiveHeldInFolding(), which is static: a Double or String constant by its record, else the one value of the proven type. Unknown: it is not known.
PrimitiveKnownToFolding primitiveKnownForStrings(const Walker&, Reference);
// Null: the value is not one known text.
StringID textKnownForStrings(const Walker&, Reference);
std::optional<char16_t> onlyCodeUnitOfText(const Walker&, StringID);
// As constantForFolding(). Null for Unknown.
Reference constantForStrings(Walker&, PrimitiveKnownToFolding);
Reference integerForStrings(Walker&, int64_t);
bool isProvenString(const Walker&, TypeID);
// Bit i: an edge behind the Branch on the test hands on operand i of the test, narrowed. Nothing: the test has several readers.
std::optional<uint32_t> operandsNarrowedBehindTestForStrings(const Walker&, ValueID test);
// The condition of the Branch whose edge is the only way into the block of `value`, behind LogicalNot and ToBoolean, and whether that edge is taken where the condition holds. Nothing: several ways lead there, or no Branch.
// Nothing too unless the walk stands in the block of Code that the edge of the Branch on referenceBoundTo(test) enters: only there does that record, with its operands, dominate.
struct TestThatLedHere {
    ValueID test;
    bool holds { false };
};
std::optional<TestThatLedHere> testThatLedToBlockOf(Walker&, const BodyValue&);
// An operator on proven numbers in the form that they allow (formAllowedForTypedOperator(), boxedUnlessNumbersAreUnboxed()). `right` null: it has one operand. Null: the walk stands nowhere.
Reference buildOperatorForStrings(Walker&, Opcode, Reference left, Reference right, Origin);
Reference buildLengthOfString(Walker&, Reference string, Origin); // GetStringLength, or the constant
// WHOEVER CALLS HAS 0 <= position < length PROVEN ON THIS EDGE: nothing checks it.
Reference buildCodeUnitOfString(Walker&, Reference string, Reference position, Origin); // GetStringCodeUnit
// WHOEVER CALLS HAS 0 <= start, start <= end or the piece is empty, end <= length.
Reference buildPieceOfString(Walker&, Reference string, Reference start, Reference end, Origin); // Substring
// test ? passing() : failing(). Each arm hands back `count` values, which meet in as many parameters of one new block, where the walk goes on. An arm that ended is left out.
using ArmForStrings = ScopedLambda<void(std::span<Reference> results)>;
bool buildArmsForStrings(Walker&, Reference test, BlockFrequency ofFailing, const ArmForStrings& passing, const ArmForStrings& failing, std::span<Reference> met);

// ---- What is in sight of a string. FOR OTHER MECHANISMS TOO (numbers: switches on characters, char codes that are no NaN . loops: the length of a piece, a piece of a piece).
// AOTWalkStringPieces.cpp. `text` is a Substring of this unit, and a read of characters may stand in its place: both stand under no handler, or in one block of Code. Nothing: it is none.
struct PieceOfString {
    Reference string;
    Reference start;
    Reference end;
};
std::optional<PieceOfString> pieceInSight(Walker&, Reference text);
bool isRangeOfPieceInOrder(Walker&, const PieceOfString&);
// AOTWalkStringCharacters.cpp. The integer that a character read has left beside what it gave: the code unit, and -1 outside the string. Null: the value is no such character.
Reference codeUnitOfCharacterInSight(Walker&, Reference character);
// The same beside what charCodeAt() gave, where that may be NaN.
Reference codeUnitOfCharCodeInSight(Walker&, Reference number);
// The subject in hand is a FlattenString: the string that it was made of. Null: it is none.
Reference stringBehindFlatString(const Walker&, Reference);
// AOTWalkStringRanges.cpp, WHICH ALONE NAMES THE THREE OPERATIONS THAT COME BACK: StringRangeEqualsLiteral, StringRangeToNumber, StringRangeParseInt. Until their rows are in the schema each of the three answers null.
// The range is in order (isRangeOfPieceInOrder(), or the end made so). Null: nothing was built.
Reference buildComparisonOfRangeWithText(Walker&, const PieceOfString&, StringID, Origin);
Reference buildNumberOfRange(Walker&, const PieceOfString&, Origin);
Reference buildParsedIntegerOfRange(Walker&, const PieceOfString&, Reference radixOrNull, Origin);

// ---- Called by hooks in files of other owners.
// AOTWalkStringSearches.cpp. For buildMatchByCodeOfRegExp(), where RegExpSummary::atom is set. Null: nothing was built.
Reference buildSearchForAtomOfRegExp(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference string, Origin);
// AOTWalkStringCharacters.cpp. For stepByIndex() of AOTWalkIteration.cpp: `piece`, `codePoint` and `numberOfCodeUnits` are parameters of one block.
void noteCodePointOfSteppedPiece(Walker&, Reference piece, Reference codePoint, Reference numberOfCodeUnits);

// ---- From one file of the mechanism to another, and to nobody else.
bool buildCharacterByIndex(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringCharacters.cpp
bool buildEqualityOnStrings(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringComparisons.cpp
bool buildRelationOnStrings(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringComparisons.cpp
bool buildTruthinessOfLengthOfString(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringComparisons.cpp
bool buildTestOfCharacterByCodeUnit(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringComparisons.cpp
bool buildNumberOfPiece(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringPieces.cpp. ToNumber, ToNumeric
bool buildLengthOfPieceInSight(Walker&, const BodyValue&, const InputsOfTypedOperation&); // AOTWalkStringPieces.cpp. PropGet of `length`
bool buildCharacterReadByMethod(Walker&, const CallByOneRow&); // AOTWalkStringCharacters.cpp
bool buildPieceByMethod(Walker&, const CallByOneRow&); // AOTWalkStringPieces.cpp
bool buildNumberOfPieceByCall(Walker&, const CallByOneRow&); // AOTWalkStringPieces.cpp. Number(), parseInt()
bool buildTestOfEndOfString(Walker&, const CallByOneRow&); // AOTWalkStringSearches.cpp
bool buildSearchInString(Walker&, const CallByOneRow&); // AOTWalkStringSearches.cpp
// AOTWalkStringJoins.cpp. For buildTypedOperation(), in front of buildTypedJoinOfTexts(), of an Add or a Strcat that one call of startsWith() or endsWith() alone takes. True: the row up to that call is walked. False: nothing was built.
inline constexpr OpcodeSet opcodesThatBuildTestAgainstJoinOfTextsTakes = operationsThatJoinTexts;
AOT_LINK_OF_WALK(buildTestAgainstJoinOfTexts, opcodesThatBuildTestAgainstJoinOfTextsTakes)
// The part has been looked for at the end of the string, and `holdsOfPart` says whether it lies there: whether the code unit lies beside it too. It builds arms. Null: the walk stands nowhere.
Reference buildTestOfCodeUnitBesidePartOfJoin(Walker&, Reference string, Reference lengthOfPart, Reference holdsOfPart, char16_t unit, bool testsStart, Origin);
// AOTWalkStringJoins.cpp. For guessThatOperandBesideStringHasText(): the call of startsWith | endsWith that buildTestAgainstJoinOfTexts() would walk with the join, were that one part proven to have text. Null: none.
ValueID findTestOfEndThatTakesJoinApart(Walker&, const BodyValue& join, Reference takenToHaveText);
// AOTWalkStringOperandsAsMade.cpp. Both for openGuessAtOperandOfBuiltinCall(). With `value` of the type `behindTest` and as its class makes it the call reaches one row. It builds nothing.
bool goesByOneRowOnOperandAsMade(Walker&, const BodyValue& call, BuiltinID, Reference value, TypeID behindTest);
// The test of the class, the look at the cell, the call walked again behind the two, the call as written in one Rare block, and the two meet.
void openGuessAtOperandTakenAsMade(Walker&, const BodyValue& call, Reference value, TypeID tested);

// Walker::keptByStrings(), dropped where a unit begins. Each entry ties records of Code that stand in one block, so it holds wherever its key is in sight: nothing is met, copied or ended. By the index of the record.
struct NotesOfStrings final : Walker::KeptForBuild {
    template<typename Noted> using ByRecord = HashMap<uint32_t, Noted, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    struct CodePointOfPiece {
        Reference codePoint;
        Reference numberOfCodeUnits;
    };
    ByRecord<Reference> codeUnitsBesideCharacters;
    ByRecord<Reference> codeUnitsBesideCharCodes;
    ByRecord<CodePointOfPiece> codePointsOfSteppedPieces;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> charactersOnlyAskedAbout;
    // AOTWalkStringURLs.cpp. `new URL(text, base)` that nothing gets hold of but reads which the text answers. It counts while `made` is bound to `object`.
    struct URLThatIsNeverMade {
        // Origin::inlineFrame of the body. No two copies have one frame of Code, so the record of a copy that has ended is nobody's.
        uint32_t inlineFrame { 0 };
        ValueID made;
        // The object where the scan has turned the text away, else undefined.
        Reference object;
        // Where the scan has turned the text away: the empty text and PlainURL::notPlain.
        Reference text;
        Reference scan;
        TypeID typeOfObject;
        TypeID typeOfQuery;
        // What the look at the whole body has found, in the order of the body: every name of the object, every searchParams read off it under all its names, every method read off those.
        enum class Mention : uint8_t { None, URL, Query, Method };
        Vector<std::pair<ValueID, Mention>, 8> mentions;
    };
    Vector<URLThatIsNeverMade, 1> urlsThatAreNeverMade;
    // openGuessAtOperandOfBuiltinCall() is at work on this call, in the copy at this depth.
    ValueID callWhoseOperandIsTested;
    size_t depthOfCopiesAroundThatCall { 0 };
};
NotesOfStrings& notesOfStrings(Walker&);

Reference referenceToRecordForStrings(const Walker&, ValueID record);
unsigned lengthOfTextForStrings(const Walker&, StringID);
// The root, and of a flat string the root of the string that it was made of.
Reference rootOfStringForStrings(const Walker&, Reference string);
// `length` is GetStringLength of the string, under whatever names.
bool isLengthOfStringForStrings(const Walker&, Reference length, Reference string);
// Less or LessEq between two proven integers, by their ranges and by their roots alone. Nothing: neither says.
std::optional<bool> relationDecidedForStrings(const Walker&, Opcode, Reference left, Reference right);
// The walk stands at the head of the block that the taken edge of a Branch on the test alone enters. Of a constant, and behind a test that buildArmsForStrings() does not branch on: the value itself.
Reference buildProofBehindTestForStrings(Walker&, Reference value, Reference test, uint32_t operandIndex, Origin);
Reference buildTypeTestForStrings(Walker&, Reference value, TypeID tested, Origin);
Reference buildResultOfRangeForStrings(Walker&, Reference range, uint32_t index, Origin);
// LogicalNot, or the other constant.
Reference buildNegationForStrings(Walker&, Reference boolean, Origin);
void bindForStrings(Walker&, ValueID input, Reference built, Origin);
Opcode relationWithSidesSwappedForStrings(Opcode);
// AOTWalkStringPieces.cpp. The position as a method clamps it into 0 .. limit, counted from the limit where it is negative and `countsFromEnd`. Null: the ranges do not decide it.
Reference clampedPositionForStrings(Walker&, Reference position, Reference limit, bool countsFromEnd, Origin);
// The name of the position hangs on a test that says position < length of this string, so whatever takes the name stands behind that test wherever it is moved.
bool nameProvesPositionBelowLengthForStrings(const Walker&, Reference position, Reference string);
// The one edge into the block of `value` is taken only where position < length of the string.
bool testThatLedHereProvesPositionBelowLength(Walker&, const BodyValue&, Reference string, Reference position);

// AOTWalkStringRangesAbsent.cpp. For a piece from the start of its string or up to its end, where buildComparisonOfRangeWithText() has answered null. It may build arms. Null: nothing was built.
Reference buildComparisonOfPieceAtEndOfStringWithText(Walker&, const PieceOfString&, StringID, Origin);

// AOTWalkStringFlattening.cpp, which calls what loops-3 and known-state-and-effects-2 add where they are in.
// For a reader of characters. The flat string to read through: made in front of the innermost open loop that the string stands in front of, or where the second reader on one way stands. Null: the reader takes the string.
Reference flatStringForReaderOfCharacters(Walker&, Reference string, Origin);
// It builds nothing. Null: none is known where the walk stands.
Reference flatStringInSight(Walker&, Reference string);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
