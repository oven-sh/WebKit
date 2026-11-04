/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// What a test says of a value on the edge that follows it. Beside a fact this is the only way in which a type gets narrower.
// In Code the edge hands over a new value, an edge result, that has the old value and the test as operands. Its type is the old one cut by what was tested: one of the functions below.
// Cut is said of the values that the type stands for, not of isSubset(): see narrowByTest().
// Whoever needs the narrower type uses that value, so it cannot stand where the test has not passed. Checking builds compute the type again by typeOfEdgeResult() and compare.
// Facts' scan turns the same functions into Filter statements. Which operand of which test an edge of the input narrows is the body's business: BodyIndex::edgeResult().
//
// The side Taken narrows to what may pass. The side NotTaken takes away only what would surely have passed, so a value that a test names and may still refuse stays.
// Nothing is ever narrowed on the refusal of another test.

enum class EdgeSide : uint8_t { Taken, NotTaken };

// By the schema's `proves` of the test: TestedType, Shape, ExactFunction, Truthiness, Equality, IntegerBound, NotNullish. A test that proves something else of its operand
// (a filled slot, a storage kind, a bound) leaves the type as it is: Known remembers that it passed.
// `operandsOfTest` are the proven types in front of the test, `narrowedOperandIndex` the operand that the edge result stands for.
template<typename Operation> TypeID typeOfEdgeResult(const TypingContext&, const Operation& test, OperandTypes operandsOfTest, uint32_t narrowedOperandIndex, EdgeSide);
// Of Switch, SwitchString and SwitchOnShape. A target at or behind the number of cases is `otherwise`. A value that a Branch asks directly is its own test: narrowByTruthiness().
template<typename Terminator> TypeID typeOfCaseResult(const TypingContext&, const Terminator&, TypeID subject, uint32_t targetIndex);

TypeID truthyPartOf(TypeArena&, TypeID);
TypeID falsyPartOf(TypeArena&, TypeID);

TypeID narrowByTruthiness(const TypingContext&, TypeID source, EdgeSide);
// TypeTest, and with forLayoutSet(), forFunction() and forBuiltin() as `tested`: ShapeTest, IsClosureOf, IsFunctionOfBuiltin.
// On the side Taken in an arena of Compile (ArenaKeeps::UntilReset), a named object that only may have one of the tested layouts gives way to `unknown` within those it may have,
// so that the answer proves the receivers of the slot operations behind the test. Such an answer lies inside `tested` and need not lie inside `source`. Every other answer lies inside `source`.
// Facts and Plan find an object's cells by its name, so their arenas keep the object. narrowByShapeCase() and narrowHolderByFieldValue() ask this function, so the same holds of them.
// There an object whose cell nobody describes gives way as well: withUndescribedObjectsInside().
// So does a class that no test tells from a tested class and that does not stand below it, with the objects that are named with it: an arguments object that has passed a test for arrays is an array, which proveOrTrap() rests on.
TypeID narrowByTest(const TypingContext&, TypeID source, TypeID tested, EdgeSide);
// `passing` holds what has passed a test for `holder`, or what the compiler says lies in `holder`. intersectionOf() keeps the name of an object that only may lie there.
// Where the table of objects has no shape for the object, its name proves nothing of its cell, and an entry that lets it in by its name lets it in whatever its cell is. `unknown` within `holder` takes its place.
TypeID withUndescribedObjectsInside(TypeArena&, TypeID passing, TypeID holder);
// No test finds a built-in that each module has one of. Among other objects the test refuses it. Where the value may be no other object, the test asks for an object.
bool mayRefuseUntestableBuiltins(const TypingContext&, TypeID source, TypeID tested);
// StrictEq, Eq, Less, LessEq, Greater, GreaterEq. `sourceIsLeft` says on which side of the comparison the narrowed operand stands.
TypeID narrowByComparison(const TypingContext&, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide);
TypeID narrowByNotNullish(TypeArena&, TypeID source, EdgeSide);

// Switch and SwitchString.
TypeID narrowByIntegerCase(const TypingContext&, TypeID source, int64_t caseValue);
TypeID narrowByStringCase(const TypingContext&, TypeID source, StringID caseValue);
TypeID narrowByOtherwise(TypeArena&, TypeID source, std::span<const int64_t> cases);
TypeID narrowByOtherwise(TypeArena&, TypeID source, std::span<const StringID> cases);
// SwitchOnShape.
TypeID narrowByShapeCase(const TypingContext&, TypeID source, LayoutSetID caseLayouts);
TypeID narrowByOtherwise(const TypingContext&, TypeID source, std::span<const LayoutSetID> cases);

// `node.kind === K`, NarrowByField on the wire: what was read out of a slot says which layouts of the holder's family can have held it, because what lies in a typed slot always fits it.
// `read` is the type of the value read behind its test. The edge result for the holder has the read and the test as operands.
// An object keeps its layout for life, so the edge result stands whatever has been written since the read.
// GetSlotOrUndefined gives undefined for a slot that is empty, whatever the slot is laid out for: undefined tells none of its layouts apart.
// AOTFieldValueNarrowing.cpp.
enum class ReadOfEmptySlot : uint8_t { IsRuledOut, GivesUndefined };
TypeID narrowHolderByFieldValue(const TypingContext&, TypeID holder, PropertyKeyID, TypeID read, ReadOfEmptySlot = ReadOfEmptySlot::IsRuledOut);
// No layout of the holder can have held the value, so no run comes to the edge. narrowHolderByFieldValue() then leaves the layouts as they are.
bool narrowsHolderToNothing(const TypingContext&, TypeID holder, PropertyKeyID, TypeID read, ReadOfEmptySlot = ReadOfEmptySlot::IsRuledOut);

// MapNext and SetNext give several results, and HasEntry says whether the step has found an entry. Where it has, a result that holds a part of the entry is what the collection holds:
// undefined no longer stands in its type for a step that found nothing. A collection that holds nothing has no such edge: the answer is never.
// The edge result has the ResultAt of that result and the ResultAt of HasEntry as operands.
// `operandsOfProducer` are the types of the step's operands, `source` the type of the narrowed result in front of the test.
// Null: the producer is no such step, the tested result is not HasEntry or is false on the edge, the other result holds no part of an entry, or nothing is known of the contents.
// AOTResultRulesDeclared.cpp.
TypeID narrowResultByOtherResult(const TypingContext&, Opcode producer, OperandTypes operandsOfProducer, uint32_t testedResult, uint32_t narrowedResult, TypeID source, EdgeSide);

// Of the schema's tables alone.
enum class TargetResults : uint8_t { None, Proofs, DeclaredResults };

const TargetInfo* infoOfTarget(TerminatorKind, uint32_t targetIndex, uint32_t numberOfTargets);
TargetResults targetResultsOf(TerminatorKind, uint32_t targetIndex, uint32_t numberOfTargets);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
