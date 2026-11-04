/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// `for (..) s += "k" + i + ","`: a text that grows in a loop lies in a builder. NameOfGrowingText, GrowingText and TextInStringBuilder stand in AOTWalk.h, in front of the Walker that keeps them.
// While the walk stands in the loop every name of the text that it has walked is bound to a record whose own type is the opaque kind StringBuilder, and only the functions below build an operation that takes such a record.
// Every edge out of the loop gives the builder back once, and behind the loop no value of the body is bound to such a record. The looks at loops see strings, and effects that read and write StringBuilderContents.

// ---- The scan. AOTWalkTextBuilderWebs.cpp. By the input, the types of the looks and liveness alone: it builds nothing, binds nothing, asks no state.
// What it proves of each text that it gives:
// (a) at each point of the loop one version is current, and only names of that version are mentioned there;
// (b) an edge inside the loop hands a name only to a parameter that is a name, and every edge into such a parameter brings the same text;
// (c) on each edge out of the loop, every name that is live at the head of the target and every argument that is a name is of the version current on that edge;
// (d) a growth is an Add or a Strcat with a name as operand 0 that gives a string by the types of the looks, or, with growsInCallees, a call for which placeOfArgumentHandedBackWithMoreBehind() answers;
// (e) the flow is reducible, and no handler edge enters the head.
void findGrowingTexts(Walker&, BlockID inputHead, const LoopPreview&, Vector<GrowingText, 2>&);

// ---- AOTWalkTextBuilders.cpp
// TypedOperations and LoopInvariants are on, the unit is no SlowBody, the walk stands in no copy built for size.
bool mayKeepTextsInStringBuilders(const Walker&);
// The record's own type is the opaque kind StringBuilder. The one test by which a name is known: it asks no table. False for a null reference and for a constant.
bool isTextInStringBuilder(const Walker&, Reference);
// Of the frame on top, from the last record back: `input` is among web.names. Null: it names no text.
TextInStringBuilder* findTextNamedBy(Walker&, ValueID input);
// findTextNamedBy(), and isTextInStringBuilder() of what the value is bound to.
bool isNameOfTextInStringBuilder(Walker&, ValueID input);

// ---- At the head of a loop
// Walker::enterLoopHead(), behind the loop that fills `entering` and `entryTypes`: a text that enters in its builder is a string to the looks.
void typeTextsInStringBuildersForLooks(Walker&, std::span<const Reference> entering, Vector<TypeID, 8>& entryTypes);
// Walker::enterLoopHead(), behind the line of buildLoopTwice(), in front of whatever moves preview.typesOfValues. The walk stands in the block in front of the loop.
// For each text of findGrowingTexts() that passes the questions below: the builder is begun, what enters is appended (nothing for ""), a record is made, and the names that are read behind the loop are bound anew at every block.
// entering[i] becomes the builder, entryTypes[i] and preview.typesOfParameters[i] its type. For a parameter that takes a text in its builder from a loop around: the three alone.
// What enters in a builder and may not stay there is finished here: the place runs once for each builder.
// preview.effects is widened by the bound of an append wherever a text in a builder may grow in the loop.
void beginTextsInStringBuilders(Walker&, BlockID inputHead, LoopPreview&, Vector<Reference, 8>& entering, Vector<TypeID, 8>& entryTypes);
// LooksAtLoops::typeOf(), of the type of a record from in front of the stretch: TypeTable::string in the place of the opaque kind, else `type`.
TypeID typeOfRecordForLooks(const Walker&, TypeID type);

// ---- Names, growths, readers
// The first link of Walker::walkThroughChain(). An operation that hands its operand on and is a name of the text that the operand names: it is bound to what the operand is bound to. False: nothing was bound.
bool bindCastOfTextInStringBuilder(Walker&, const BodyValue&);
// `value` is a growth of the text that its operand 0 names: it is itself among the names, its version is itself, it joins texts or is a call that grows.
bool isGrowthOfTextInStringBuilder(Walker&, const BodyValue&);
// Walker::walkOperation(), round its calls of walkThroughChain(), where hasTextsInStringBuilders(). For its life each operand of the operation that isTextInStringBuilder() is bound to stringOfTextInStringBuilder(),
// unless the operation is a growth or a cast of that text, or readsTextWithoutString() has bound the operation. At its end the operands are bound as before: a growth further on in the block takes the builder.
// A link may walk on to such a growth while the object lives: the growth finds the builder behind the string, and takes that for its own life.
class StringsInPlaceOfTextsInBuilders {
    WTF_MAKE_NONCOPYABLE(StringsInPlaceOfTextsInBuilders);
public:
    StringsInPlaceOfTextsInBuilders(Walker&, const BodyValue&);
    // Of a terminator, for as long as anybody decides by its operands where it leads. The arguments of its edges are names of the text, so it is gone before an edge is led.
    StringsInPlaceOfTextsInBuilders(Walker&, const BodyTerminator&);
    ~StringsInPlaceOfTextsInBuilders();
    // Walker::walkOperation() goes on behind the chain as if it had answered false with the operation built.
    bool hasBoundOperation() const { return m_hasBoundOperation; }
    // The operation is bound to a builder, or to what a look at a builder gave. The looks at loops have seen a string there, so nobody holds what is handed on against what they have settled.
    bool bindsWhatNoLookHasSeen() const { return m_namesText || m_hasBoundOperation; }
private:
    Walker& m_walker;
    Vector<std::pair<ValueID, Reference>, 2> m_boundBefore;
    bool m_hasBoundOperation { false };
    bool m_namesText { false };
};
// What a terminator reads of an operand: the string of a text in a builder, else referenceOf(operand). Arguments of edges do not come by here.
Reference referenceOfOperandOfTerminator(Walker&, ValueID operand);
bool terminatorTakesTextInStringBuilder(Walker&, const BodyTerminator&);
// StringBuilderToString where the walk stands, or the string that this block has made of the same state. `name`: what names the builder here. The builder goes on. Null: unreachable or failed.
Reference stringOfTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
// AOTWalkTypedTexts.cpp, of a row of joins that is about to become one Strcat. parts[0] is a text in a builder and `join` a growth of it: the other parts are appended, `join` is bound to parts[0]; true.
// Else each part that is a text in a builder becomes its string, in `parts`; false, and the caller builds its Strcat. True as well where no way leads on.
bool appendRowToTextInStringBuilder(Walker&, const BodyValue& join, Vector<Reference, 8>& parts);

// ---- Parts. AOTWalkTextBuilderParts.cpp
// Each part in the narrowest form that its proven type allows, a constant as bytes, "" as nothing. Known parts side by side stay operations side by side, which the emitter writes as one. It ends stringOfState and lengthOfState.
// It asks, in this order, of each part: appendPieceWithoutCutting() . appendResultOfBuiltinStraight() . becomeOperationOfStringBuilderInFrame() for the draft that it has made.
void appendPartsToStringBuilder(Walker&, TextInStringBuilder&, Reference name, std::span<const Reference> parts, Origin);
// False: a part of the type goes by the call that takes any primitive, and it may be a string, which the call copies at a higher price than the stub for strings. Without strings, the code in front of the call writes a small integer.
bool partHasFormOfItsOwnInStringBuilder(const Type&);
// The text that ToString makes of the part, where it is at hand: a written or made text, undefined, null, a boolean. Null: it is not.
StringID textOfKnownPartOfStringBuilder(Walker&, Reference part);

// ---- The ways out. AOTWalkTextBuilders.cpp
// Walker::leadEdge(), behind the loop that collects `arguments`: `to` lies outside the loop of some text of the frame on top, and the block being left lies in that loop.
bool edgeLeavesLoopOfTextInStringBuilder(Walker&, BlockID to);
// What named the builder . the string.
using TextsTakenOutOnEdge = Vector<std::pair<Reference, Reference>, 2>;
// Walker::leadEdge(), in a block on the edge. For each such text, unless textStaysInStringBuilderOnEdge(): it is finished where a name is live at the head of `to` or among the arguments, else discarded.
// The arguments that named the text become the string.
void takeTextsOutOfStringBuildersOnEdge(Walker&, BlockID to, BodyTarget, Vector<Reference, 8>& arguments, TextsTakenOutOnEdge&);
// Walker::leadEdge(), behind takeNamesOntoEdge().
void bringStringsInPlaceOfTexts(const TextsTakenOutOnEdge&, Vector<std::pair<ValueID, Reference>, 2>& namesOnEdge);
// Walker::walkTerminator(), behind the lines that set the origin. A block that ends a way lies in a loop only by its edge to a handler, which may go on with the builder.
// So a text that such a block returns gets a string, its name is bound to that for good (it is bound anew at every block), and the builder is left as it is.
void takeTextsOutOfStringBuildersAtEndOfWay(Walker&, const BodyTerminator&);
// It asks finishTextInFrame() and lastPartOrFinishedText().
Reference finishTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
void discardTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
// For whoever leaves the loop of a text by a way that is no edge of the body, with what a turn begins with or brings round: each text in a builder among the values is finished where the walk stands, and its string takes its place.
void finishTextsInStringBuildersHandedOver(Walker&, Vector<Reference, 8>& values);

// ---- Questions that the functions above ask. Each keeps the text out of a builder, or takes it out, while nobody answers more.
// Asked by StringsInPlaceOfTextsInBuilders in front of making a string. The operation reads only the length, the emptiness or the truth of the text `name`: it is built on StringBuilderLength and bound.
bool readsTextWithoutString(Walker&, const BodyValue& reader, TextInStringBuilder&, Reference name);
// Of a Branch. Null: the string is made.
Reference truthOfTextInStringBuilder(Walker&, TextInStringBuilder&, Reference name, Origin);
// `to` is the head of a loop, or leads there by jumps alone; the argument goes to a parameter that comes round as a join with itself in front; no name is live at the head of `to`.
bool textStaysInStringBuilderOnEdge(Walker&, const TextInStringBuilder&, BlockID to, BodyTarget);
// The text may lie in a builder though blocks of its loop have handlers. False: no builder is begun for a text that standsUnderHandlers.
bool mayKeepTextUnderHandlers(Walker&, BlockID inputHead, const GrowingText&);
// Every handler that a block of the loop names lies behind the loop, is handed no name of the text and finds none alive: `for (const c of row) line += c`, whose handler closes the iterator and throws on.
bool everyHandlerLetsTextGo(Walker&, const GrowingText&);
// In the place of appendPartsToStringBuilder() under a handler that is handed the text: a checkpoint, the parts, and a roll back where a part fails.
void appendPartsRolledBackWhereTheyFail(Walker&, TextInStringBuilder&, Reference name, std::span<const Reference> parts, Origin);
// For the scan. The place among the variadic operands of the call of the argument that the only callee hands back with more behind it, and the walk is sure to copy the callee here. Nothing: the call is a reader.
std::optional<uint32_t> placeOfArgumentHandedBackWithMoreBehind(Walker&, const BodyIndex&, const BodyValue& call);
// False: no builder is begun for a text that hasPartsThatMayRunCode.
bool convertsPartsThatMayRunCode();
// The part as written may run code: ToPrimitive (of an Add) or ToString (of a Strcat) is built where the walk stands, in the order of the parts. What to append.
Reference primitivePartOfStringBuilder(Walker&, Opcode join, Reference part, Origin);
// AOTWalkTypedTexts.cpp, buildTypedJoinOfTexts(), in the branch where takePartsInWalkTexts() has refused, in front of the question for a key. `join` is a growth: every part is converted, then all are appended, `join` is bound; true.
bool convertPartsAndAppendToTextInStringBuilder(Walker&, const BodyValue& join, std::span<const Reference> parts);
// Null: the text is finished.
Reference lastPartOrFinishedText(Walker&, TextInStringBuilder&, Reference name, Origin);
// By the input and the unit: no coroutine body, no handler. It says what pays too.
bool textBegunHereMayLieInFrame(Walker&, BlockID inputHead, const GrowingText&);
// True: text.liesInFrame, and the draft has become `inFrame` with the builder and the newest cursor. Whoever builds it then calls noteCursorBehind().
bool becomeOperationOfStringBuilderInFrame(Walker&, TextInStringBuilder&, Reference name, Draft&, Opcode inFrame);
void noteCursorBehind(Walker&, TextInStringBuilder&, Reference cursor);
Reference finishTextInFrame(Walker&, TextInStringBuilder&, Reference name, Origin);
// `part` was made by a Substring, or by a call of slice, substring, charAt or at, that nothing else reads: StringBuilderAppendStringRange on what it was cut from. The sweep drops the cut.
bool appendPieceWithoutCutting(Walker&, TextInStringBuilder&, Reference name, Reference part, Origin);
// `part` was made by Number.prototype.toString with a radix, and nothing else reads it.
bool appendResultOfBuiltinStraight(Walker&, TextInStringBuilder&, Reference name, Reference part, Origin);
// AOTWalkTypedTexts.cpp, takePartsInWalkTexts(), in front of the read of the operand. The operand is what a copied callee has returned, a Strcat that nothing else reads: its parts are appended to `parts`; true.
bool takePartsOfJoinReturnedByCopy(Walker&, ValueID operand, Vector<Reference, 8>& parts);
// Asked by appendRowToTextInStringBuilder() of each part: it is what a call of JSON.stringify gave, the call waits, nothing else reads it: the text is written through the builder's cell.
bool appendJSONTextStraight(Walker&, TextInStringBuilder&, Reference name, ValueID part, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
