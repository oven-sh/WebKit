/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTLinkStrings.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// `usesFeature: { feature: "IntlCollation", when: b.whenOperand("key", "collation") }`: a line of BuiltinTable::featureUsesOf() that holds of a call by what one argument of it is, and by nothing else.
// A line on the receiver (calledOn), on the build or on what the program stores holds as before.
bool isFeatureLineDecidedByOperandOfCall(const BuiltinFeatureUse&);

// What a body shows of one argument of a call that it writes.
struct OperandOfCallInSight {
    enum class Kind : uint8_t { NobodyKnows, IsLeftOut, IsText, BeginsWithText, ListsItsKeys };

    Kind kind { Kind::NobodyKnows };
    // IsText, BeginsWithText.
    StringID text;
    // ListsItsKeys: a literal of the block of the call that nothing else has touched.
    std::span<const PropertyKeyID> keys;
};

// `definition`: of the argument, behind casts. `definitionOf`: of a value of the body, behind casts. It knows `undefined`, a written text, and a sum or a template that begins with one.
OperandOfCallInSight describeOperandOfCallInSight(const Program&, const BodyValue& definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf);

// MUST BE TRUE WHERE THE CONDITION CAN HOLD: a feature that is left out wrongly ends the process. The parts of a condition count each by itself.
// A text that is left out may have a default in native code, so it may be any text. An operand that is left out has no property.
bool conditionOnOperandMayHold(const Program&, const BuiltinCondition&, const OperandOfCallInSight&);

// By canonical built-in. A line is left out only where EVERY call of the built-in stands in a reached body that names it, and none of them may fulfil the condition.
class FeatureLinesAtCalls {
public:
    // `line`: the place in featureUsesOf(row).
    void noteLineMayHold(BuiltinID row, uint32_t line) { m_linesThatMayHold.add(wordOf(row, line)); }
    // Nobody else holds the function: it has not left a body, no name finds it on a value that nobody knows, and no code runs that nobody has read.
    void noteEveryCallIsInSight(BuiltinID row) { m_rowsWhoseCallsAreAllInSight.set(row.index()); }

    bool mayHold(BuiltinID row, std::span<const BuiltinFeatureUse> linesOfRow, const BuiltinFeatureUse& line) const
    {
        if (!isFeatureLineDecidedByOperandOfCall(line) || !m_rowsWhoseCallsAreAllInSight.get(row.index()))
            return true;
        return m_linesThatMayHold.contains(wordOf(row, static_cast<uint32_t>(&line - linesOfRow.data())));
    }

private:
    static uint64_t wordOf(BuiltinID row, uint32_t line) { return static_cast<uint64_t>(row.index() + 1) << 32 | line; }

    BitVector m_rowsWhoseCallsAreAllInSight;
    HashSet<uint64_t> m_linesThatMayHold;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
