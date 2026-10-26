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

// Where the predictions of a body stand, and what is made from the values they are about.
class PredictedValues {
public:
    struct Options {
        // A comparison with a number of any size leaves the same of the other side whatever that number is. Asked for by whoever reads the types on the edges.
        const Program* programToReadComparisonsWith { nullptr };
        const BitVector* checksToLeaveOut { nullptr };
    };

    PredictedValues(const Function&, const CFG&);
    PredictedValues(const Function&, const CFG&, Options);

    bool hasChecks() const { return m_hasChecks; }
    // The type of the value may be owed to a check, which stands until the function gets its generic version.
    bool isBehindCheck(ValueID value) const { return has(value, BehindCheck); }
    // The value is made from one that was expected to have a type and that nothing has tested.
    bool isBehindExpectation(ValueID value) const { return has(value, BehindExpectation); }
    // A check or a test of the program stands in front of the value.
    bool isBehindTest(ValueID value) const { return has(value, BehindCheck | BehindTest); }
    // The value is the very one that a check would have tested: what stands in place of the check, a cast of it, or a parameter that takes it.
    // What is proven of a position that takes it may rest on that check.
    bool isUncheckedValueItself(ValueID value) const { return has(value, UncheckedItself); }

    BitVector valuesBehindChecks() const;

private:
    enum Mark : uint8_t { BehindCheck = 1 << 0, BehindExpectation = 1 << 1, BehindTest = 1 << 2, UncheckedItself = 1 << 3 };
    static constexpr uint8_t marksOfWhatIsMade = BehindCheck | BehindExpectation | BehindTest;

    bool has(ValueID value, uint8_t marks) const { return value && value.index() < m_marks.size() && m_marks[value.index()] & marks; }

    Vector<uint8_t> m_marks;
    size_t m_numberOfValues;
    bool m_hasChecks { false };
};

enum class ExpectedTypes : uint8_t { Kept, Dropped };

struct ForgottenProofs {
    bool hasErasedValidation { false };
    // The claims that went, with the value each was about.
    Vector<std::pair<ValueID, TypeID>, 4> claims;
};

// A claim that has lost its test is a prediction. In what is made from the given values: a claim that holds behind checks and an Upcast become an Expect, a validation becomes nothing,
// the receivers and callees of an operation are no longer all there are, and a tail call casts nothing. Checks stay unless they are listed.
ForgottenProofs forgetProofsBehind(Function&, const CFG&, TypeArena&, std::span<const ValueID> values, const BitVector& checksThatGo, ExpectedTypes);
// The same for whoever has those values at hand: the copies of what is made from a check that has failed.
ForgottenProofs forgetProofsOf(Function&, TypeArena&, const BitVector& values, const BitVector& checksThatGo, ExpectedTypes);

// The only way to take a test away without a proof.
ForgottenProofs expectInsteadOfChecking(Function&, const CFG&, TypeArena&, std::span<const ValueID> checks, ExpectedTypes);

// For whoever has answered a check by a proof, or has put a test that stands in its place: what was proven behind that check alone holds in every version from now on.
void settleProofsBehindAnsweredChecks(Function&, const CFG&);

enum class InPlaceOfCheck : bool { No, Yes };
void becomeExpect(Value&, ValueID operand, ExpectedType, InPlaceOfCheck);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
