/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class CFG;

// What is made from the given values: whatever takes one as an operand, the parameter that takes one as an argument, and what the edges of a terminator hand on that takes one.
BitVector valuesBehind(const Function&, const CFG&, std::span<const ValueID>);
// The given values themselves wherever they go: their casts, the parameters that take them, and what a test leaves of them.
BitVector valuesThatAre(const Function&, const CFG&, std::span<const ValueID>);

// Where the predictions of a body stand, and what is made from the values they are about. It holds until the values of the body get other numbers.
class PredictedValues {
public:
    struct Options {
        // A comparison with a number of any size leaves the same of the other side whatever that number is. Asked for by whoever reads the types on the edges.
        const Program* programToReadComparisonsWith { nullptr };
        const BitVector* checksToLeaveOut { nullptr };
    };

    // Makes the graph itself, where the body holds a check.
    explicit PredictedValues(const Function&);
    PredictedValues(const Function&, const CFG&);
    PredictedValues(const Function&, const CFG&, Options);

    bool hasChecks() const { return m_hasChecks; }
    // The type of the value may be owed to a check, which stands until the function gets its generic version. Of a value that was put into the body afterwards nothing is known here: it may be.
    bool isBehindCheck(ValueID value) const { return value && (value.index() >= m_numberOfValues || has(value, BehindCheck)); }

    BitVector valuesBehindChecks() const;

private:
    enum Mark : uint8_t { BehindCheck = 1 << 0, BehindExpectation = 1 << 1, BehindTest = 1 << 2 };

    bool has(ValueID value, uint8_t marks) const { return value && value.index() < m_marks.size() && m_marks[value.index()] & marks; }
    void walk(const Function&, const CFG&, Options);

    Vector<uint8_t> m_marks;
    size_t m_numberOfValues;
    bool m_hasChecks;
};

enum class ExpectedTypes : uint8_t { Kept, Dropped };

struct ForgottenProofs {
    bool hasErasedValidation { false };
};

// Keeps what the edits of a body replace, for whoever only asks what the body would be like.
class UndoLog {
public:
    void willEdit(const Function& function, ValueID id) { m_values.append({ id, function.at(id) }); }
    void willEditTerminator(const Function& function, BlockID id) { m_terminators.append({ id, function.at(id).terminator }); }
    void undo(Function&);

private:
    Vector<std::pair<ValueID, Value>, 16> m_values;
    Vector<std::pair<BlockID, Terminator>, 1> m_terminators;
};

// A claim that has lost its test is a prediction. In what is made from the given values: a claim that holds behind checks and an Upcast become an Expect, a validation becomes nothing,
// the receivers and callees of an operation are no longer all there are, and a tail call keeps the claim of the result alone. Checks stay unless they are listed.
ForgottenProofs forgetProofsBehind(Function&, const CFG&, TypeArena&, std::span<const ValueID> values, const BitVector& checksThatGo, ExpectedTypes);
// The same for whoever has those values at hand: the copies of what is made from a check that has failed.
ForgottenProofs forgetProofsOf(Function&, TypeArena&, const BitVector& values, const BitVector& checksThatGo, ExpectedTypes, UndoLog* = nullptr);

// The only way to take a test away without a proof.
ForgottenProofs expectInsteadOfChecking(Function&, const CFG&, TypeArena&, std::span<const ValueID> checks, ExpectedTypes);

// For whoever has answered a check by a proof, or has put a test that stands in its place: what was proven behind that check alone holds in every version from now on.
void settleProofsBehindAnsweredChecks(Function&, const CFG&);
void settleProofsBehindAnsweredChecks(Function&);
// For whoever has taken from an operation an operand that it only kept, as a closure keeps a capture. The type of such an operation says nothing of that operand, and whoever reads what was kept
// reads a position with a type of its own. So what is made from the operation never rested on the checks in front of that operand.
void settleProofsBehindOperandsThatWereOnlyKept(Function&);

// What is proven of the result of a function holds of what a tail call hands on, in every version.
bool isClaimOfResult(const Function&, const TailCast&);

void becomeExpect(Value&, ValueID operand, ExpectedType);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
