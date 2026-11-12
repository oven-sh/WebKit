/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuiltinTable.h"
#include "AOTFoldingOfConstants.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTWalk.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatBuildTypedOperationOnStringsTakes = allEqualities | allRelations | OpcodeSet { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine,
    Opcode::PropGet, Opcode::ToBoolean, Opcode::ToNumber, Opcode::ToNumeric };
AOT_LINK_OF_WALK_WITH_INPUTS(buildTypedOperationOnStrings, opcodesThatBuildTypedOperationOnStringsTakes)

struct CallByOneRow {
    const BodyValue& call;
    BuiltinID builtin;
    BuiltinOverloadID overload;
    BuiltinCallMode mode;
    std::optional<InlineBuiltin> codeInLine;
    Reference receiver;
    std::span<const Reference> arguments;
    Origin origin;
    ExpectedType expected;
    bool isForwarded { false };

    std::span<const ValueID> writtenArguments() const { return call.opcode() == Opcode::Call || call.opcode() == Opcode::Construct ? call.argumentsOperands() : std::span<const ValueID>(); }
};
bool buildCallByOneRowForStrings(Walker&, const CallByOneRow&);
bool buildConstructionOnAnyOperandsForStrings(Walker&, const CallByOneRow&);

enum class OperandsTestedAtBuiltinCall : bool { All, Arguments };
bool openGuessAtOperandOfBuiltinCall(Walker&, const BodyValue& call, OperandsTestedAtBuiltinCall = OperandsTestedAtBuiltinCall::All);
bool openGuessAtOperandThatSpecialCaseTakesAsMade(Walker&, const BodyValue& call, FunctionID enteredByValue);
TypeID typeExpectedOfResultOfCall(Walker&, const BodyValue& call);

inline constexpr uint32_t startOfStringRange = 0;
inline constexpr uint32_t endOfStringRange = 1;
Reference rootForStrings(const Walker&, Reference);
PrimitiveKnownToFolding primitiveKnownForStrings(const Walker&, Reference);
StringID textKnownForStrings(const Walker&, Reference);
std::optional<char16_t> onlyCodeUnitOfText(const Walker&, StringID);
Reference integerForStrings(Walker&, int64_t);
bool isProvenString(const Walker&, TypeID);
std::optional<uint32_t> operandsNarrowedBehindTestForStrings(const Walker&, ValueID test);
struct TestThatLedHere {
    ValueID test;
    bool holds { false };
};
std::optional<TestThatLedHere> testThatLedToBlockOf(Walker&, const BodyValue&);
Reference buildOperatorForStrings(Walker&, Opcode, Reference left, Reference right, Origin);
Reference buildLengthOfString(Walker&, Reference string, Origin);
Reference buildCodeUnitOfString(Walker&, Reference string, Reference position, Origin);
Reference buildPieceOfString(Walker&, Reference string, Reference start, Reference end, Origin);
using ArmForStrings = ScopedLambda<void(std::span<Reference> results)>;
bool buildArmsForStrings(Walker&, Reference test, BlockFrequency ofFailing, const ArmForStrings& passing, const ArmForStrings& failing, std::span<Reference> met);

struct PieceOfString {
    Reference string;
    Reference start;
    Reference end;
};
std::optional<PieceOfString> pieceInSight(Walker&, Reference text, bool alsoWhereNothingMayStand = false);
bool isRangeOfPieceInOrder(Walker&, const PieceOfString&);
Reference buildLengthOfPiece(Walker&, const PieceOfString&, Origin);
std::optional<PieceOfString> pieceReadInLoop(Walker&, Reference text, Origin);
Reference codePointOfCharacterInSight(Walker&, Reference character);
Reference buildOnlyCharacter(Walker&, Reference string, Origin);
void noteCodePointBesideCharacter(Walker&, Reference character, Reference codePoint);
Reference buildLengthOfCharacterInSight(Walker&, Reference character, Origin);
Reference buildLengthOnlyJoin(Walker&, ValueID text, Origin, Vector<ValueID, 2>& joinsThatFailWhereReaderFails);
Reference buildLengthOfIntegerString(Walker&, Reference string, Origin);
Reference codeUnitOfCharCodeInSight(Walker&, Reference number);
Reference stringBehindFlatString(const Walker&, Reference);
Reference flatLiteralBeforeLoop(Walker&, Reference literal, Origin);
Reference buildComparisonOfRangeWithText(Walker&, const PieceOfString&, StringID, Origin);
Reference buildNumberOfRange(Walker&, const PieceOfString&, Origin);
Reference buildParsedIntegerOfRange(Walker&, const PieceOfString&, Reference radixOrNull, Origin);

Reference buildSearchForAtomOfRegExp(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference string, Origin);

bool buildCharacterByIndex(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool readsCharacterOfText(InlineBuiltin);
bool buildEqualityOnStrings(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool buildRelationOnStrings(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool buildTruthinessOfLengthOfString(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool buildNumberOfPiece(Walker&, const BodyValue&, const InputsOfTypedOperation&);
void noteOrderedPieceBounds(Walker&, Reference start, Reference end);
bool buildLengthOfPieceInSight(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool buildKnownPieceTruthiness(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool buildCharacterReadByMethod(Walker&, const CallByOneRow&);
bool buildPieceByMethod(Walker&, const CallByOneRow&);
bool buildNumberOfPieceByCall(Walker&, const CallByOneRow&);
bool buildLookupOfPieceInMap(Walker&, const CallByOneRow&);
bool buildTestForKeyLookedUpBehindIt(Walker&, const CallByOneRow&);
bool buildStoreUnderPieceInMap(Walker&, const CallByOneRow&);
bool buildTestOfEndOfString(Walker&, const CallByOneRow&);
bool buildSearchInString(Walker&, const CallByOneRow&);
inline constexpr OpcodeSet opcodesThatBuildTestAgainstJoinOfTextsTakes = operationsThatJoinTexts;
AOT_LINK_OF_WALK(buildTestAgainstJoinOfTexts, opcodesThatBuildTestAgainstJoinOfTextsTakes)
Reference buildTestOfCodeUnitBesidePartOfJoin(Walker&, Reference string, Reference lengthOfPart, Reference holdsOfPart, char16_t unit, bool testsStart, Origin);
ValueID findTestOfEndThatTakesJoinApart(Walker&, const BodyValue& join, Reference takenToHaveText);
bool goesByOneRowOnOperandAsMade(Walker&, const BodyValue& call, BuiltinID, Reference value, TypeID behindTest, BuiltinOverloadID* onlyRow = nullptr);
ValueID findLastCallInMethodChain(Walker&, const BodyValue& firstCall, uint32_t behindStretch);
void openGuessAtOperandTakenAsMade(Walker&, const BodyValue& call, Reference value, TypeID tested, std::optional<uint32_t> firstPlace = std::nullopt, std::span<const ValueID> readsOffHolders = { }, ValueID last = { });

struct NotesOfStrings final : Walker::KeptForBuild {
    template<typename Noted> using ByRecord = HashMap<uint32_t, Noted, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    ByRecord<Reference> codePointsBesideCharacters;
    ByRecord<Reference> codeUnitsBesideCharCodes;
    struct CodeUnitChanged {
        uint32_t record { 0 };
        Reference before;
        bool isBesideCharacter { false };
    };
    Vector<CodeUnitChanged> codeUnitsChanged;
    HashMap<const BodyIndex*, Vector<std::pair<uint32_t, uint32_t>>> callsWithTwoArgumentsByFirst;
    HashMap<const BodyIndex*, Vector<std::array<uint32_t, 3>>> callsThatGetUnderOneKey;
    HashMap<const BodyIndex*, BitVector> valuesThatBodiesStore;
    struct URLThatIsNeverMade {
        uint32_t inlineFrame { 0 };
        ValueID made;
        Reference object;
        Reference text;
        Reference scan;
        std::optional<Draft> constructionAsWritten;
        TypeID typeOfObject;
        TypeID typeOfQuery;
        enum class Mention : uint8_t { None, URL, Query, Method };
        Vector<std::pair<ValueID, Mention>, 8> mentions;
    };
    Vector<URLThatIsNeverMade, 1> urlsThatAreNeverMade;
    ValueID callWhoseOperandIsTested;
    size_t depthOfCopiesAroundThatCall { 0 };
    std::unique_ptr<Walker::KeptForBuild> looksAtResultsInParts;
};
NotesOfStrings& notesOfStrings(Walker&);

Reference referenceToRecordForStrings(const Walker&, ValueID record);
unsigned lengthOfTextForStrings(const Walker&, StringID);
Reference rootOfStringForStrings(const Walker&, Reference string);
bool isLengthOfStringForStrings(const Walker&, Reference length, Reference string);
std::optional<bool> relationDecidedForStrings(const Walker&, Opcode, Reference left, Reference right);
bool relationIsShownBySumsForStrings(const Walker&, Opcode, Reference left, Reference right);
Reference buildProofBehindTestForStrings(Walker&, Reference value, Reference test, uint32_t operandIndex, Origin);
Reference buildTypeTestForStrings(Walker&, Reference value, TypeID tested, Origin);
Reference buildResultOfRangeForStrings(Walker&, Reference range, uint32_t index, Origin);
Reference buildNegationForStrings(Walker&, Reference boolean, Origin);
void bindForStrings(Walker&, ValueID input, Reference built, Origin);
Opcode relationWithOperandsSwappedForStrings(Opcode);
Reference clampedPositionForStrings(Walker&, Reference position, Reference limit, bool countsFromEnd, Origin);
bool nameProvesPositionBelowLengthForStrings(const Walker&, Reference position, Reference string);
bool readsCharacterBeforeTestedNumber(Walker&, const BodyValue& read);
bool readsCharacterUnderLoweredCounter(Walker&, const BodyValue& read);
bool readsCharacterAtOuterCounter(Walker&, const BodyValue& read);


Reference flatStringForReaderOfCharacters(Walker&, Reference string, Origin, bool readsCodeUnitsAlone = false);
void flattenStringsReadByLoop(Walker&, BlockID inputHead, std::span<const Reference> entering, Origin);
Reference flatStringInSight(Walker&, Reference string);
bool isOrCanBeFlattenedBeforeLoop(Walker&, Reference string);
Reference flatStringMadeWhereWalkStands(Walker&, Reference string, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
