/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTWalkStrings.h"

namespace JSC { namespace AOT { namespace Typed {

class ScratchRealm;

inline constexpr OpcodeSet opcodesThatFoldOperationOnKnownTextsTakes { Opcode::Strcat, Opcode::ElemGet };
AOT_LINK_OF_WALK(foldOperationOnKnownTexts, opcodesThatFoldOperationOnKnownTextsTakes)
inline constexpr OpcodeSet opcodesThatAnswerFromURLThatIsNeverMadeTakes = opcodesWhere([](Opcode opcode) { return opcodesThatStateTypes.contains(opcode) || opcode == Opcode::PropGet || opcode == Opcode::Call; });
AOT_LINK_OF_WALK(answerFromURLThatIsNeverMade, opcodesThatAnswerFromURLThatIsNeverMadeTakes)
bool foldCallByOneRowOnKnownOperands(Walker&, const CallByOneRow&);
bool isKnownForTerminatorOfFolding(const Walker&, Reference operand);
void mergeKnownPartsOfJoinedText(Walker&, Vector<Reference, 8>& parts);

PropertyKeyID keyOfKnownTextThatAccessGoesBy(const Walker&, const BodyValue& access);
bool buildReadByPartsOfKey(Walker&, const BodyValue& read, const InputsOfTypedOperation&);
bool buildLookupUnderAnyKeyOnPlainRecords(Walker&, const BodyValue& access, const InputsOfTypedOperation&);
extern const OpcodeSet opcodesThatTakeTextsAsKeys;
void takeTextsUsedAsKeysAsAtomsOfItsOpcodes(Walker&, Draft&);
ALWAYS_INLINE void takeTextsUsedAsKeysAsAtoms(Walker& walker, Draft& draft)
{
    if (opcodesThatTakeTextsAsKeys.contains(draft.opcode))
        takeTextsUsedAsKeysAsAtomsOfItsOpcodes(walker, draft);
}

bool buildThrowOfNewError(Walker&, const CallByOneRow&);
bool buildFormatterCallByPattern(Walker&, const CallByOneRow&);
bool buildURLCallByScan(Walker&, const CallByOneRow&);
bool standsForMethodOfURLThatIsNeverMade(Walker&, const BodyValue& read);

ScratchRealm* scratchRealmOfWalk(Walker&);
std::optional<Effects> effectsOfProtocolsResolvedOn(Walker&, OptionSet<Protocol>, TypeID operand);

StringID textOfKnownPatternOfRegExp(Walker&, Reference patternOrNull);

inline constexpr size_t mostBytesOfTextMadeByStringFolds = defaultLimitValue(LimitID::MaxFoldedStringBytes, hostArchitecture());
inline constexpr size_t mostBytesOfPatternMadeByStringFolds = 1048576;

PrimitiveKnownToFolding joinKnownTextsForStrings(Walker&, std::span<const PrimitiveKnownToFolding> parts, size_t mostBytes = mostBytesOfTextMadeByStringFolds);
PrimitiveKnownToFolding pieceOfKnownTextForStrings(Walker&, StringID text, int64_t start, int64_t end);
bool textFeedsOnlyPatternOfRegExp(Walker&, ValueID text);
std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker&, BuiltinOverloadID, TypeID receiverOrNull, std::span<const TypeID> arguments);
std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker&, const CallByOneRow&);
TypeID typeGivenByRowForStrings(Walker&, const CallByOneRow&);
bool bindAnswerOfFoldedCall(Walker&, const CallByOneRow&, PrimitiveKnownToFolding answer);
Reference buildProofOfTypeTestForStrings(Walker&, Reference value, Reference test, Origin);
bool canCallHostHelperForStrings(const Walker&, BuiltinID helper);
Reference buildCallOfHostHelperForStrings(Walker&, BuiltinID helper, std::span<const Reference> arguments, Origin);
uint32_t placeInBlockForStrings(const BodyIndex&, ValueID);

bool foldCallAnsweredByEngine(Walker&, const CallByOneRow&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
