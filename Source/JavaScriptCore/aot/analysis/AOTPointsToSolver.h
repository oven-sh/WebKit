/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEscapeAnalysis.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

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

struct PointsToResults {
    bool gaveUp { false };
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
