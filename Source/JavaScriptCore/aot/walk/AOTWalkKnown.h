/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkKnownAnswers.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- PACKAGE 1 . walk/AOTWalkKnownEffects.cpp
// What a CallDirect of a body of `callee` with these entry arguments may do: effectsOnReturn; of effectsOnThrow the reads and the marks always, the writes only where a handler of the unit stands round the call;
// LinkedEffectsGivenArguments in their place where the proven types of the arguments allow. While LinkTables::known is empty: the two of LinkedFunction merged, as today.
Effects effectsOfDirectCall(Walker&, FunctionID callee, std::span<const Reference> entryArguments);
// Of a draft that build() is about to append. objectsOfAnyLayout: nobody can tell, the answer for whatever is no CallDirect of a body.
uint64_t layoutsWrittenBy(Walker&, const Draft&);
// The bits of every layout that an object of the type may have. objectsOfAnyLayout unless all that the type holds beside primitives are objects of layouts.
uint64_t bitsOfLayoutsOf(Walker&, TypeID);

// ---- PACKAGE 2 . walk/AOTWalkKnownAnswers.cpp . Folding
// WHAT A FILE KNOWS BEYOND WalkState's OWN RECORDS. One enumerator has one owner, named behind it.
enum class KnownNote : uint8_t {
    Outcome,            // K2 . subject: a Boolean value. noted: 0 | 1
    Relation,           // K3 . subject < other (word 0) or subject <= other (word 1), as numbers. noted: 0 | 1
    LengthOfList,       // K9 . noted: Reference::bits() of the record that holds the length
    FlatString,         // K5 . word 0: noted is Reference::bits() of the FlattenString. word 1: a reader of characters has been here
    CharactersOfText,   // K21 . noted: StringID::bits()
    SlotIsFilled,       // J1 notes, K13 and the typed accesses read . word: PropertyKeyID::bits() of the field
    Birth,              // K13 . word: the group of birth fields. noted: a bit a field, and the marks of the first group
    ValueKeptByPlace,   // a variable: no subject, word: its number . a scope slot: subject: the holder, word: the field . noted: Reference::bits()
    ListTestedInFrontOfLoop, // subject: the list. noted: Reference::bits() of the name that the row has tested, from bit 32 on the ArrayStorage that was asked for, bit 40: Int32 storage passes beside Double
    NumberReadOutOfList, // subject: the list. other: the position. noted: Reference::bits() of the number that was read there behind tests of the storage and of the element
};
// Where the walk stands, until something may write `restsOn`: in `subject` for a heap of perObjectHeaps, anywhere for any other heap. No heap: for good. What is fresh (WalkState::isFresh()) forgets nothing.
// Each takes any name of a value: the key holds roots. `endsAtSuspension`: what is noted names a record in a form that no saved frame keeps (a flat string, a narrow reference), or is not worth keeping in one.
void noteKnown(Walker&, KnownNote, Reference subject, Reference other, uint32_t word, const LocationMasks& restsOn, uint64_t noted, bool endsAtSuspension = false);
// Nothing: it is not known here, or Folding is withheld.
std::optional<uint64_t> findKnown(Walker&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
void endKnown(Walker&, KnownNote, Reference subject, Reference other = { }, uint32_t word = 0);
// build(), in front of the append: an operation with the same key (opcode, the flags and fields that decide its answer, operands by their roots, commutative operands in one order) was built on every way to here,
// and nothing has written what it reads. Null: none, or the opcode is not among those that are numbered.
// build() asks both at every operation, and few operations are numbered: the head of each stands here, in line.
extern const OpcodeSet opcodesThatFindEqualOperationBuiltTakes;
extern const OpcodeSet opcodesThatNoteOperationBuiltTakes;
Reference findEqualOperationBuiltOfItsOpcodes(Walker&, const Draft&, const Effects&);
ALWAYS_INLINE Reference findEqualOperationBuilt(Walker& walker, const Draft& draft, const Effects& effects) { return opcodesThatFindEqualOperationBuiltTakes.contains(draft.opcode) ? findEqualOperationBuiltOfItsOpcodes(walker, draft, effects) : Reference(); }
// build(), behind the append.
void noteOperationBuiltOfItsOpcodes(Walker&, const Draft&, const Effects&, Reference built);
ALWAYS_INLINE void noteOperationBuilt(Walker& walker, const Draft& draft, const Effects& effects, Reference built)
{
    if (opcodesThatNoteOperationBuiltTakes.contains(draft.opcode))
        noteOperationBuiltOfItsOpcodes(walker, draft, effects, built);
}
// Walker::convert(). Null: none was made on every way to here.
Reference findConversionMade(Walker&, Reference value, Representation wanted);
void noteConversionMade(Walker&, Reference value, Representation wanted, Reference converted);
// Walker::leadEdge(): walker.state() is what holds on the edge number `targetIndex` of the terminator of `from`, a block of Code that has ended.
// The condition, what LogicalNot and ToBoolean made it of, and the relation or equality that it is get their outcome on this edge.
void noteWhatBranchDecidesOnEdge(Walker&, BlockID from, uint32_t targetIndex);
// onlyTargetKnownToFolding(), and whoever is about to branch. Nothing: no branch on the way has decided it.
std::optional<bool> outcomeKnownOf(Walker&, Reference condition);

// ---- PACKAGE 2 . walk/AOTWalkKnownWriters.cpp . TypedOperations
// build(), in place of m_state.invalidate(effects). Withheld: that line.
void endWhatOperationWrites(Walker&, const Draft&, const Effects&);
// False: in no run are the two one object. Two names of one root: true. Whatever is no cell: false.
bool mayBeSameObject(Walker&, Reference, Reference);


// ---- PACKAGE 3 . walk/AOTWalkKnownLeaving.cpp . EscapeAnalysis
// Walker::walkOperation(), round the chain, at one of the first set in a body that holds one of the second. `call` names an `arguments` that another block has made, that only this code has,
// and that the body only reads and hands out last: the allocation is walked once more here, and while this lives the name stands for the new object. Nothing to do: nothing is built and nothing bound.
inline constexpr OpcodeSet opcodesThatMayTakeArgumentsLast { Opcode::Call, Opcode::Construct, Opcode::CallDirect };
inline constexpr OpcodeSet opcodesThatMakeArguments { Opcode::CreateUnmappedArguments, Opcode::CreateMappedArguments };
class ArgumentsMadeWhereTheyLeave {
    WTF_MAKE_NONCOPYABLE(ArgumentsMadeWhereTheyLeave);
public:
    ArgumentsMadeWhereTheyLeave(Walker&, const BodyValue& call);
    ~ArgumentsMadeWhereTheyLeave();

private:
    Walker& m_walker;
    ValueID m_name;
    Reference m_boundBefore;
};

// ================= BEYOND THE TEXT OF THE PLAN . PACKAGE 2, part (a): what its hooks call, and what the owners of texts, lists and loops may ask. None of them builds anything unless it says so.

// ---- walk/AOTWalkKnownAnswers.cpp . Folding
// onlyTargetKnownToFolding(), of a Branch whose condition is no constant: the one target, where a branch on every way to here has decided the condition.
// In a loop only where the edge that is taken proves nothing: the looks at the loop have promised what it proves, and a jump proves nothing.
std::optional<uint32_t> onlyTargetDecidedOnWay(Walker&, const BodyTerminator&);
// K3. As numbers, in every run that comes here: by the ranges of the two, or by a comparison of the two that was branched on.
bool isKnownToLieBelow(Walker&, Reference value, Reference bound);
// `position` is an integer, at least 0 and below `length`.
bool isKnownToLieInside(Walker&, Reference position, Reference length);

// ---- walk/AOTWalkKnownElements.cpp . TypedOperations
// `position` is an index below the length that is noted of the list.
bool isKnownToLieInsideList(Walker&, Reference list, Reference position);
// K9. appendElementsOf(), appendMembersOrAllOutOfLine(): what an AppendElements, AppendMembers or AppendIterable does to a list that nothing but this code has and that is as it was made: it fails only for lack of memory.
// Nothing: the declared effects stand.
std::optional<Effects> effectsOfAppendToListAsMade(Walker&, const Draft& append);

// ---- walk/AOTWalkKnownTexts.cpp . Folding
// K21. The characters of the text in every run: a constant, a literal type, or what noteCharactersOf() was told. Null: nobody knows. NO CONSTANT STANDS FOR A TEXT THAT THE THIRD ALONE ANSWERS FOR: it may be held in another way.
StringID charactersKnownOf(Walker&, Reference text);
void noteCharactersOf(Walker&, Reference text, StringID characters);
// noteOperationBuilt(): `joined` is a Strcat or an Add that was built just now. Where the characters of every part are known, so are its own.
void noteCharactersOfJoin(Walker&, const Draft& join, Reference joined);
// answerKnownToFolding(), in front of its last line: a comparison, a test for truth or a length of texts whose characters are known. It never answers with a text. Null: nothing is known.
Reference answerKnownOfTexts(Walker&, const BodyValue&);
// findEqualOperationBuilt(), of a GetStringLength.
Reference lengthOfKnownCharacters(Walker&, Reference text);
// Of a Strcat whose parts all have known characters: it converts nothing and is not too long, so it fails only for lack of memory, and the sweep may drop it. Nothing: the declared effects stand.
std::optional<Effects> effectsOfJoinOfKnownTexts(Walker&, const Draft& join);

// ---- walk/AOTWalkKnownKeys.cpp . TypedOperations
// K17. The one name that the key is in every run, by its proven type: one text that is no index. Null: there is none.
PropertyKeyID nameThatKeyIs(Walker&, Reference key);
// The same by the body as it is written, so that every copy of it and both arms of a guess say the same: behind casts stands a String.
PropertyKeyID nameWrittenAsKey(const Walker&, ValueID key);
// replaceOperation(), of a DefineGetter or a DefineSetter under a computed key that nameThatKeyIs() answers for: the definition is built by that name. False: nothing was built.
bool defineAccessorUnderNameThatKeyIs(Walker&, const BodyValue& definition);
} } }

#endif // ENABLE(AOT)
