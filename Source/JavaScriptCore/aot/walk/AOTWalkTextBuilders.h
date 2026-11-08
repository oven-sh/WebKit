/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

void findGrowingTexts(Walker&, BlockID inputHead, const LoopPreview&, Vector<GrowingText, 2>&);

bool mayKeepTextsInStringBuilders(const Walker&);
bool isTextInStringBuilder(const Walker&, Reference);
TextInStringBuilder* findTextNamedBy(Walker&, ValueID input);
bool isNameOfTextInStringBuilder(Walker&, ValueID input);

void typeTextsInStringBuildersForLooks(Walker&, std::span<const Reference> entering, Vector<TypeID, 8>& entryTypes);
void beginTextsInStringBuilders(Walker&, BlockID inputHead, LoopPreview&, Vector<Reference, 8>& entering, Vector<TypeID, 8>& entryTypes);
TypeID typeOfRecordForLooks(const Walker&, TypeID type);

bool bindCastOfTextInStringBuilder(Walker&, const BodyValue&);
bool isGrowthOfTextInStringBuilder(Walker&, const BodyValue&);
class StringsInPlaceOfTextsInBuilders {
    WTF_MAKE_NONCOPYABLE(StringsInPlaceOfTextsInBuilders);
public:
    StringsInPlaceOfTextsInBuilders(Walker&, const BodyValue&);
    StringsInPlaceOfTextsInBuilders(Walker&, const BodyTerminator&);
    ~StringsInPlaceOfTextsInBuilders();
    bool hasBoundOperation() const { return m_hasBoundOperation; }
    bool bindsWhatNoLookHasSeen() const { return m_namesText || m_hasBoundOperation; }
private:
    Walker& m_walker;
    Vector<std::pair<ValueID, Reference>, 2> m_boundBefore;
    bool m_hasBoundOperation { false };
    bool m_namesText { false };
};
Reference referenceOfOperandOfTerminator(Walker&, ValueID operand);
bool terminatorTakesTextInStringBuilder(Walker&, const BodyTerminator&);
Reference stringOfTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
bool appendRowToTextInStringBuilder(Walker&, const BodyValue& join, Vector<Reference, 8>& parts);

void appendPartsToStringBuilder(Walker&, TextInStringBuilder&, Reference name, std::span<const Reference> parts, Origin);
bool partHasFormOfItsOwnInStringBuilder(const Type&);
bool handlerInForceReadsTextInStringBuilder(Walker&, const TextInStringBuilder&);
StringID textOfKnownPartOfStringBuilder(Walker&, Reference part);

bool edgeLeavesLoopOfTextInStringBuilder(Walker&, BlockID to);
bool edgeBeginsTextAnewInStringBuilder(Walker&, BlockID to, std::span<const Reference> arguments);
void beginTextsAnewInStringBuildersOnEdge(Walker&, BlockID to, Vector<Reference, 8>& arguments);
bool edgeHandsTextInStringBuilderToReader(Walker&, BlockID to, std::span<const Reference> writtenArguments);
void handStringsOfTextsInStringBuildersToReadersOnEdge(Walker&, BlockID to, std::span<Reference> writtenArguments);
struct TextsTakenOutOnEdge {
    Vector<std::pair<Reference, Reference>, 2> strings;
    Vector<ValueID, 2> names;
};
void takeTextsOutOfStringBuildersOnEdge(Walker&, BlockID to, BodyTarget, Vector<Reference, 8>& arguments, TextsTakenOutOnEdge&);
void takeTextsOutOfStringBuildersAtEndOfWay(Walker&, const BodyTerminator&);
Reference finishTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
void discardTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);

bool readsTextWithoutString(Walker&, const BodyValue& reader, TextInStringBuilder&, Reference name);
Reference truthOfTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
bool textStaysInStringBuilderOnEdge(Walker&, const TextInStringBuilder&, BlockID to, BodyTarget);
bool mayKeepTextUnderHandlers(Walker&, BlockID inputHead, const GrowingText&);
bool everyHandlerLetsTextGo(Walker&, const GrowingText&);
void appendPartsRolledBackWhereTheyFail(Walker&, TextInStringBuilder&, Reference name, std::span<const Reference> parts, Origin);
Reference primitivePartOfStringBuilder(Walker&, Opcode join, Reference part, Origin);
bool convertPartsAndAppendToTextInStringBuilder(Walker&, const BodyValue& join, std::span<const Reference> parts);
bool growthJoinsTextsInStringBuilder(Walker&, const TextInStringBuilder&, std::span<const Reference> partsBehindFront);
bool textBegunHereMayLieInFrame(Walker&, BlockID inputHead, const GrowingText&);
void becomeOperationOfStringBuilderInFrame(Walker&, TextInStringBuilder&, Reference name, Draft&, Opcode inFrame);
Reference finishTextInFrame(Walker&, Reference name, Origin);
bool appendPieceWithoutCutting(Walker&, TextInStringBuilder&, Reference name, Reference part, Origin);
bool appendResultOfBuiltinStraight(Walker&, TextInStringBuilder&, Reference name, Reference part, Origin);
bool takePartsOfJoinMadeForOneReader(Walker&, ValueID operand, Vector<Reference, 8>& parts, Vector<ValueID, 2>* joinsThatFailWhereReaderFails);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
