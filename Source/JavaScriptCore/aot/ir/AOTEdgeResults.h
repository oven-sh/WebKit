/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

enum class EdgeSide : uint8_t { Taken, NotTaken };
enum class EdgeResultKind : uint8_t { NarrowedOperand, Guard, DeclaredResult };

struct EdgeResultPosition {
    BlockID from;
    uint32_t targetIndex { 0 };
    uint32_t resultIndex { 0 };

    friend bool operator==(const EdgeResultPosition&, const EdgeResultPosition&) = default;
};

struct EdgeResult {
    EdgeResultKind kind { EdgeResultKind::DeclaredResult };
    Proof proof { Proof::None };
    EdgeSide side { EdgeSide::Taken };
    ValueID test;
    ValueID subject;
    uint32_t operandIndex { 0 };
    std::optional<uint32_t> caseIndex;
};

enum class TargetResults : uint8_t { None, Proofs, DeclaredResults };

const TargetInfo* infoOfTarget(TerminatorKind, uint32_t targetIndex, uint32_t numberOfTargets);
TargetResults targetResultsOf(TerminatorKind, uint32_t targetIndex, uint32_t numberOfTargets);

uint32_t numberOfEdgeResults(const Function&, BlockID from, uint32_t targetIndex);
uint32_t firstEdgeResultParameter(const Function&, BlockID from, uint32_t targetIndex);

std::optional<Proof> proofOfTerminator(const Function&, const Block&);
std::optional<EdgeResult> tryGetEdgeResult(const Function&, EdgeResultPosition);
EdgeResult edgeResultAt(const Function&, EdgeResultPosition);
ValueID operandNarrowedOnEdge(const Function&, EdgeResultPosition);

Type computeTypeOnEdge(const TypingContext&, EdgeResultPosition);
TypeID typeOnEdge(const Program&, const Function&, EdgeResultPosition, TypeArena&);
TypeID expectedTypeOnEdge(const Program&, const Function&, EdgeResultPosition, TypeID provenType, TypeArena&);

Type narrowOperandByTest(const TypingContext&, const Value& test, uint32_t operandIndex, EdgeSide);
Type narrowSubjectByCase(const TypingContext&, const Terminator&, uint32_t caseIndex);

Type narrowTypeByTest(const TypingContext&, const Type& source, const Value& test, uint32_t operandIndex, EdgeSide);
Type narrowTypeByCase(const TypingContext&, const Type& source, const Terminator&, uint32_t caseIndex);
Type narrowTypeByOtherwise(const Type& source, const Terminator&);

Type truthyPartOf(const Type&, TypeArena&);
Type falsyPartOf(const Type&, TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
