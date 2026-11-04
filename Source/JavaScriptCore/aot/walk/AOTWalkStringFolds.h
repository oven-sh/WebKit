/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkStrings.h"

namespace JSC { namespace AOT { namespace Typed {

class ScratchRealm;

// AOTWalkStringFolds.cpp . Folding. A link of the chain, behind foldOperation(): Strcat, ElemGet, Substring, GetStringCodeUnit of known operands. False: nothing was built.
inline constexpr OpcodeSet opcodesThatFoldOperationOnKnownTextsTakes { Opcode::Strcat, Opcode::ElemGet, Opcode::Substring, Opcode::GetStringCodeUnit };
AOT_LINK_OF_WALK(foldOperationOnKnownTexts, opcodesThatFoldOperationOnKnownTextsTakes)
// AOTWalkStringURLs.cpp . BuiltinCalls with Guesses. The first link of the chain, where Walker::hasURLsThatAreNeverMade(): a name of such a URL, a read off it, `get` and `has` of its searchParams. False: nothing was built.
inline constexpr OpcodeSet opcodesThatAnswerFromURLThatIsNeverMadeTakes = opcodesWhere([](Opcode opcode) { return opcodesThatStateTypes.contains(opcode) || opcode == Opcode::PropGet || opcode == Opcode::Call; });
AOT_LINK_OF_WALK(answerFromURLThatIsNeverMade, opcodesThatAnswerFromURLThatIsNeverMadeTakes)
// Folding and BuiltinCalls. By code units first, then by the engine (AOTWalkStringFoldsByEngine.cpp). True: a constant, a piece of the receiver or a literal list is bound.
bool foldCallByOneRowOnKnownOperands(Walker&, const CallByOneRow&);
// For onlyTargetKnownToFolding(), in the place of `operand.isConstant()`: the operand is one known primitive, by its record or by its proven type.
bool isKnownForTerminatorOfFolding(const Walker&, Reference operand);
// For takePartsInWalkTexts() | bindJoinedTextInWalkTexts() of AOTWalkTypedTexts.cpp: known parts side by side become one constant, the empty text drops out. One part is left: the join is that part if it is a string.
void mergeKnownPartsOfJoinedText(Walker&, Vector<Reference, 8>& parts);
// For findJoinThatAloneTakesInWalkTexts(): the operation may stand between two links though its declared bound is observable. False: the row ends, as today.
bool mayStandBetweenLinksOfJoinedText(Walker&, const BodyValue& between, std::span<const Reference> partsSoFar);

// AOTWalkStringKeys.cpp.
// For keyThatAccessGoesBy(), where Link names no key. referenceBoundTo() of the key is one known text that is a name of the program's table and no element key, or undefined, null, true, false. Null: the access goes by the value.
PropertyKeyID keyOfKnownTextThatAccessGoesBy(const Walker&, const BodyValue& access);
bool buildReadByPartsOfKey(Walker&, const BodyValue& read, const InputsOfTypedOperation&); // it only builds: false when asked ahead
bool buildAccessUnderOneOfFewTexts(Walker&, const BodyValue& access, const InputsOfTypedOperation&); // it only builds
bool buildLookupUnderAnyKeyOnPlainRecords(Walker&, const BodyValue& access, const InputsOfTypedOperation&); // it only builds
// For Walker::build(), in front of the conversions of operands: the String constants that the row of the draft takes as keys or compares are replaced by their copies with isAtom.
extern const OpcodeSet opcodesThatTakeTextsAsKeys;
void takeTextsUsedAsKeysAsAtomsOfItsOpcodes(Walker&, Draft&);
ALWAYS_INLINE void takeTextsUsedAsKeysAsAtoms(Walker& walker, Draft& draft)
{
    if (opcodesThatTakeTextsAsKeys.contains(draft.opcode))
        takeTextsUsedAsKeysAsAtomsOfItsOpcodes(walker, draft);
}

// One file each: AOTWalkStringErrors.cpp, AOTWalkStringFormatters.cpp, AOTWalkStringURLs.cpp. As buildCallByOneRowForStrings().
bool buildThrowOfNewError(Walker&, const CallByOneRow&);
bool buildFormatterCallByPattern(Walker&, const CallByOneRow&);
bool buildURLCallByScan(Walker&, const CallByOneRow&);

// AOTWalkStringFoldsByEngine.cpp. The realm of this worker, made at the first question. Null: there is none (WalkInputs::scratchRealm is null: tests, the plan's own Walker).
ScratchRealm* scratchRealmOfWalk(Walker&);
// AOTWalkStringKeys.cpp. For CallInWalkBuiltins::addEffectsOn(), in the place of effectsOfProtocolsOn() on an operand that may be an object. Nothing: nobody resolves the members, as today.
std::optional<Effects> effectsOfProtocolsResolvedOn(Walker&, OptionSet<Protocol>, TypeID operand);

// ---- Called by hooks, beside what the plan lists.
// AOTWalkStringFolds.cpp. For buildTypedJoinOfTexts() of AOTWalkTypedTexts.cpp, where findJoinThatAloneTakesInWalkTexts() finds none: the join that alone takes the text, further on in the stretch that is being walked,
// past whatever mayStandBetweenLinksOfJoinedText(). Null: the row ends.
ValueID findJoinThatTakesLinkPastCode(Walker&, ValueID text, std::span<const Reference> partsSoFar);
// AOTWalkStringFolds.cpp. For buildRegExpOfKnownTexts() of AOTWalkRegExps.cpp. A pattern that is no text is spelled as ToString spells it: `new RegExp(404)`, `new RegExp(null)`. None, or undefined: the empty text. Null: the text is not known.
StringID textOfKnownPatternOfRegExp(Walker&, Reference patternOrNull);

// ---- From one file of strings-2 to another, and to nobody else.
inline constexpr size_t mostBytesOfTextMadeByStringFolds = 65536;
// Image compiles no longer pattern: FRESH.md, the rehearsal of RegExp's texts.
inline constexpr size_t mostBytesOfPatternMadeByStringFolds = 1048576;

// AOTWalkStringFolds.cpp.
// Each part as ToString spells it: joinTextsInFolding() of AOTWalkFolding.cpp, with a bound that the caller sets. Unknown: a part is not known, the text is too long, or narrow parts stand beside wide ones.
PrimitiveKnownToFolding joinKnownTextsForStrings(Walker&, std::span<const PrimitiveKnownToFolding> parts, size_t mostBytes = mostBytesOfTextMadeByStringFolds);
// The code units of a known text from `start` up to `end`. Unknown: the range is empty or lies outside, or the piece is more than one code unit of a text that is held in 16 bits.
PrimitiveKnownToFolding pieceOfKnownTextForStrings(Walker&, StringID text, int64_t start, int64_t end);
// By the only-user links of the body being read: nothing but the pattern of a RegExp is made of the text. It decides what pays, never what is right.
bool textFeedsOnlyPatternOfRegExp(Walker&, ValueID text);
// What a call by the row does where none of its operands runs code of the program: each is a primitive, or one of `objectsThatRunNoCode`, of which the caller has seen that the protocols of the row find nothing of the program's.
// Nothing: the row may run code that no operand names, or says too little. As CallInWalkBuiltins::effectsOfRow() of a CallBuiltin.
std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker&, BuiltinOverloadID, TypeID receiverOrNull, std::span<const TypeID> arguments, std::span<const BuiltinOperand> objectsThatRunNoCode = { });
std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker&, const CallByOneRow&, std::span<const BuiltinOperand> objectsThatRunNoCode = { });
// What the row gives the call with the operands at hand.
TypeID typeGivenByRowForStrings(Walker&, const CallByOneRow&);
// The constant stands in the place of the call where the row names its type. False: nothing was bound.
bool bindAnswerOfFoldedCall(Walker&, const CallByOneRow&, PrimitiveKnownToFolding answer);
// The walk stands in a block that the taken edge of a Branch on the TypeTest alone enters.
Reference buildProofOfTypeTestForStrings(Walker&, Reference value, Reference test, Origin);
// A function of the host that the table lists among %CompilerHelpers% and that no body names. The first builds nothing. The second: a CallBuiltin by its one row, beside the operation being walked.
bool canCallHostHelperForStrings(const Walker&, BuiltinID helper);
Reference buildCallOfHostHelperForStrings(Walker&, BuiltinID helper, std::span<const Reference> arguments, Origin);
// The place of an operation among the operations of its block.
uint32_t placeInBlockForStrings(const BodyIndex&, ValueID);

// AOTWalkStringFoldsByEngine.cpp. For foldCallByOneRowOnKnownOperands(), last.
bool foldCallAnsweredByEngine(Walker&, const CallByOneRow&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
