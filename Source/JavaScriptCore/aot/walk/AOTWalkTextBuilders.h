/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

void findGrowingTexts(Walker&, BlockID inputHead, const LoopPreview&, Vector<GrowingText, 2>&);

bool canKeepStringsInBuilders(const Walker&);
ALWAYS_INLINE bool isStringInBuilder(const Walker& walker, Reference reference)
{
    if (!reference.isValue())
        return false;
    TypeID type = walker.code().at(reference.id()).type;
    return type && !holdsOnlyPrimitives(type) && walker.types().at(type).isOpaque(OpaqueKind::StringBuilder);
}
StringInBuilder* findStringInBuilder(Walker&, ValueID input);
bool isBoundToStringInBuilder(Walker&, ValueID input);

void typeBuildersAsStrings(Walker&, std::span<const Reference> entering, Vector<TypeID, 8>& entryTypes);
void beginStringsInBuilders(Walker&, BlockID inputHead, LoopPreview&, Vector<Reference, 8>& entering, Vector<TypeID, 8>& entryTypes);
TypeID typeOfRecordForLooks(const Walker&, TypeID type);

bool bindCastOfStringInBuilder(Walker&, const BodyValue&);
bool isAppendToStringInBuilder(Walker&, const BodyValue&);
class MaterializedStringsScope {
    WTF_MAKE_NONCOPYABLE(MaterializedStringsScope);
public:
    MaterializedStringsScope(Walker&, const BodyValue&);
    MaterializedStringsScope(Walker&, const BodyTerminator&);
    ~MaterializedStringsScope();
    bool hasBoundOperation() const { return m_hasBoundOperation; }
    bool bindsWhatNoLookHasSeen() const { return m_namesText || m_hasBoundOperation; }
private:
    Walker& m_walker;
    Vector<std::pair<ValueID, Reference>, 2> m_boundBefore;
    bool m_hasBoundOperation { false };
    bool m_namesText { false };
};
Reference referenceOfOperandOfTerminator(Walker&, ValueID operand);
bool terminatorUsesStringInBuilder(Walker&, const BodyTerminator&);
Reference materializeStringInBuilder(Walker&, StringInBuilder&, Reference name, Origin);
bool tryAppendJoinToStringInBuilder(Walker&, const BodyValue& join, Vector<Reference, 8>& parts);

void appendPartsToStringBuilder(Walker&, StringInBuilder&, Reference name, std::span<const Reference> parts, Origin);
bool partHasFormOfItsOwnInStringBuilder(const Type&);
bool handlerReadsStringInBuilder(Walker&, const StringInBuilder&);
StringID textOfKnownPartOfStringBuilder(Walker&, Reference part);

bool edgeExitsBuilderLoop(Walker&, BlockID to);
bool edgeResetsStringInBuilder(Walker&, BlockID to, std::span<const Reference> arguments);
void resetStringsInBuildersOnEdge(Walker&, BlockID to, Vector<Reference, 8>& arguments);
bool edgePassesStringInBuilder(Walker&, BlockID to, std::span<const Reference> writtenArguments);
void materializeStringsPassedOnEdge(Walker&, BlockID to, std::span<Reference> writtenArguments);
struct StringsFinishedOnEdge {
    Vector<std::pair<Reference, Reference>, 2> strings;
    Vector<ValueID, 2> names;
};
void finishStringsInBuildersOnEdge(Walker&, BlockID to, BodyTarget, Vector<Reference, 8>& arguments, StringsFinishedOnEdge&);
void materializeReturnedStringInBuilder(Walker&, const BodyTerminator&);
Reference finishStringInBuilder(Walker&, StringInBuilder&, Reference name, Origin);
void discardStringInBuilder(Walker&, StringInBuilder&, Reference name, Origin);

bool readsTextWithoutString(Walker&, const BodyValue& reader, StringInBuilder&, Reference name);
Reference buildTruthinessOfStringInBuilder(Walker&, StringInBuilder&, Reference name, Origin);
bool canStayInBuilderOnEdge(Walker&, const StringInBuilder&, BlockID to, BodyTarget);
bool mayKeepTextUnderHandlers(Walker&, BlockID inputHead, const GrowingText&);
bool everyHandlerLetsTextGo(Walker&, const GrowingText&);
void appendPartsRolledBackWhereTheyFail(Walker&, StringInBuilder&, Reference name, std::span<const Reference> parts, Origin);
Reference primitivePartOfStringBuilder(Walker&, Opcode join, Reference part, Origin);
bool tryConvertAndAppendParts(Walker&, const BodyValue& join, std::span<const Reference> parts);
bool ensureAppendIsConcatenation(Walker&, const StringInBuilder&, std::span<const Reference> partsBehindFront);
bool textBegunHereMayLieInFrame(Walker&, BlockID inputHead, const GrowingText&);
void becomeOperationOfStringBuilderInFrame(Walker&, StringInBuilder&, Reference name, Draft&, Opcode inFrame);
Reference finishTextInFrame(Walker&, Reference name, Origin);
bool appendPieceWithoutCutting(Walker&, StringInBuilder&, Reference name, Reference part, Origin);
bool appendResultOfBuiltinStraight(Walker&, StringInBuilder&, Reference name, Reference part, Origin);
bool takePartsOfJoinMadeForOneReader(Walker&, ValueID operand, Vector<Reference, 8>& parts, Vector<ValueID, 2>* joinsThatFailWhereReaderFails);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
