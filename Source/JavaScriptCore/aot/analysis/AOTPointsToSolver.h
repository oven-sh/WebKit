/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEscapeAnalysis.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

using ObjectSlot = std::optional<uint32_t>;

inline constexpr EscapeReasons callReasons { EscapeReason::PassedToUnknownCode, EscapeReason::RetainedByCallee };

struct ValueSet {
    BitVector objects;
    bool mayBeUntracked { true };
};

struct SolvedObject {
    ValueID definition;
    ObjectKind kind { ObjectKind::Allocation };
    EscapeReasons reasons;
    OperandUses appliedUses;
};

struct SolvedUse {
    ObjectID object;
    Use use;
    OperandUses uses;
    EscapeReasons reasons;
    ValueID flowsTo;
    ValueID retainedBy;
    bool isContentsStore { false };
    bool readsContents { false };
};

struct SolvedContents {
    ObjectID container;
    ObjectSlot slot;
    ObjectID object;
    bool isLeaked { false };
};

AnswerFromTypes answerFromTypes(const EscapeAnalysisInputs&, AnswerFromTypes::Question, uint32_t subject, uint32_t operandIndex);

struct PointsToResults {
    bool gaveUp { false };
    uint64_t work { 0 };
    Vector<AnswerFromTypes> answersFromTypes;
    Vector<SolvedObject> objects;
    Vector<SolvedUse> uses;
    Vector<SolvedContents> contents;
    Vector<ValueSetID> setOfValue;
    Vector<ValueSet> sets;

    const ValueSet* setOf(ValueID id) const LIFETIME_BOUND
    {
        ValueSetID set = setOfValue[id.index()];
        return set ? &sets[set.index()] : nullptr;
    }
};

PointsToResults solvePointsTo(const EscapeAnalysisInputs&);

// The solver takes a value of no other opcode for an object of its own. In a body that holds none of them, and no terminator whose result may be fresh, the objects are the entry parameters.
const OpcodeSet& opcodesThatMayDefineObjects();
// Of those, the ones whose result is an object of its own though it may be an operand, or what the parent has made.
const OpcodeSet& opcodesThatDefineObjectsThatNeedNotBeFresh();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
