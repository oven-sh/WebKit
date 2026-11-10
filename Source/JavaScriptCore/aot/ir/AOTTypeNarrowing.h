/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

enum class EdgeSide : uint8_t { Taken, NotTaken };

template<typename Test>
bool canFailForMarkedObject(const Test& test) { return opcodeOf(test) == Opcode::ShapeTest && test.rejectsMarkedObjects(); }

template<typename Operation> TypeID typeOfEdgeResult(const TypingContext&, const Operation& test, OperandTypes operandsOfTest, uint32_t narrowedOperandIndex, EdgeSide);
template<typename Terminator> TypeID typeOfCaseResult(const TypingContext&, const Terminator&, TypeID subject, uint32_t targetIndex);

TypeID truthyPartOf(TypeArena&, TypeID);
TypeID falsyPartOf(TypeArena&, TypeID);

TypeID narrowByTruthiness(const TypingContext&, TypeID source, EdgeSide);
TypeID narrowByTest(const TypingContext&, TypeID source, TypeID tested, EdgeSide);
TypeID withUndescribedObjectsInside(TypeArena&, TypeID passing, TypeID holder);
bool mayRefuseUntestableBuiltins(const TypingContext&, TypeID source, TypeID tested);
TypeID narrowByComparison(const TypingContext&, Opcode comparison, bool sourceIsLeft, TypeID source, TypeID other, EdgeSide);
TypeID narrowByNotNullish(TypeArena&, TypeID source, EdgeSide);

TypeID narrowByIntegerCase(const TypingContext&, TypeID source, int64_t caseValue);
TypeID narrowByStringCase(const TypingContext&, TypeID source, StringID caseValue);
TypeID narrowByOtherwise(TypeArena&, TypeID source, std::span<const int64_t> cases);
TypeID narrowByShapeCase(const TypingContext&, TypeID source, LayoutSetID caseLayouts);
TypeID narrowByOtherwise(const TypingContext&, TypeID source, std::span<const LayoutSetID> cases);

enum class ReadOfEmptySlot : uint8_t { IsRuledOut, GivesUndefined };
TypeID narrowHolderByFieldValue(const TypingContext&, TypeID holder, PropertyKeyID, TypeID read, ReadOfEmptySlot = ReadOfEmptySlot::IsRuledOut);

TypeID narrowResultByOtherResult(const TypingContext&, Opcode producer, OperandTypes operandsOfProducer, uint32_t testedResult, uint32_t narrowedResult, TypeID source, EdgeSide);

enum class TargetResults : uint8_t { None, Proofs, DeclaredResults };

const TargetInfo* infoOfTarget(TerminatorKind, uint32_t targetIndex, uint32_t numberOfTargets);
TargetResults targetResultsOf(TerminatorKind, uint32_t targetIndex, uint32_t numberOfTargets);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
