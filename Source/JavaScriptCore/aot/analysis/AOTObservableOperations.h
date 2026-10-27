/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTIR.h"
#include "AOTPredictedValues.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class CFG;

// A program can tell that an operation is there though nobody uses its result: the operation throws by a rule of the language, or runs code of the program.
// Whether it does goes by the types of its operands. A type that is owed to a check holds only until the function gets its generic version, which is made from the same body: it does not count.
class ObservableOperations {
public:
    ObservableOperations(const EffectContext& context, const Function& function)
        : m_context(context)
        , m_function(function)
    {
    }

    // The one question in front of taking an operation out of a body, or putting one in its place that does less.
    bool neitherThrowsNorRunsCode(ValueID);

private:
    bool isOwedToCheck(const Instruction&);

    EffectContext m_context;
    const Function& m_function;
    std::optional<PredictedValues> m_predicted;
};

// Why an operation that may throw or run code is no longer where it was, for a pass that knows more than the types of the operands say. The closed list: grep "InPlaceOfOperation::" finds every such place.
enum class InPlaceOfOperation : uint8_t {
    // What the pass has put there throws what the operation threw and runs what it ran, in the same order.
    ItsExpansion,
    // The same operation on the same operands has completed in front of it on every way here, and nothing in between changes what it does.
    AnEarlierOneThatCompleted,
    // It runs elsewhere: on every way on which it ran, once, and with nothing that the program sees moved across it.
    TheSameOperationElsewhere,
    // Control never comes here.
    NothingBecauseNothingComesHere,
    // All that it does is complete or throw, and the same code runs next either way, with the same values, and reads neither its result nor what it threw.
    NothingBecauseNobodySeesHowItEnds,
    // The pass knows of the operands what their types do not say: their values, the objects that it has made itself, what the flow analysis holds of the place.
    NothingByProofOfThePass,
};

// What a pass says about one function while somebody listens. Nobody does in an ordinary build.
class ReasonsForOperationsThatWent {
public:
    ReasonsForOperationsThatWent() = default;
    // Whoever listened before goes on hearing.
    explicit ReasonsForOperationsThatWent(ReasonsForOperationsThatWent* outer)
        : m_outer(outer)
    {
        if (outer)
            m_valuesOfOuterAtStart = outer->m_currentValues;
    }

    ReasonsForOperationsThatWent* outer() const { return m_outer; }

    void add(ValueID, InPlaceOfOperation);
    // For an operation that is gone, under the number that it had when this one began to listen. Whoever listened before hears it too.
    void addForOperationThatWent(ValueID idBeforeRun, InPlaceOfOperation);
    void didRenumber(std::span<const BlockID> newBlocks, std::span<const ValueID> newValues);

    void didReplaceBody();

    // The body was replaced by one that was made elsewhere: its numbers say nothing about those before the run.
    bool hasLostTrack() const { return m_hasLostTrack; }
    bool hasReasonFor(ValueID idBeforeRun) const { return m_explained.get(idBeforeRun.index()); }
    // Where a value of the body before the run is now. Nothing where it has no number any more.
    ValueID currentIDOf(ValueID idBeforeRun) const { return currentIDIn(m_currentValues, idBeforeRun); }
    BlockID currentIDOf(BlockID idBeforeRun) const { return currentIDIn(m_currentBlocks, idBeforeRun); }

private:
    template<typename SomeID>
    static SomeID currentIDIn(const Vector<SomeID>& current, SomeID idBeforeRun)
    {
        if (current.isEmpty())
            return idBeforeRun;
        return idBeforeRun.index() < current.size() ? current[idBeforeRun.index()] : SomeID();
    }

    BitVector m_explained;
    Vector<ValueID> m_currentValues;
    Vector<BlockID> m_currentBlocks;
    Vector<ValueID> m_valuesOfOuterAtStart;
    ReasonsForOperationsThatWent* m_outer { nullptr };
    bool m_hasLostTrack { false };
};

ALWAYS_INLINE void explainOperationThatGoes(const Function& function, ValueID id, InPlaceOfOperation reason)
{
    if (ReasonsForOperationsThatWent* reasons = function.listenerForOperationsThatGo()) [[unlikely]]
        reasons->add(id, reason);
}

struct OperationThatWent {
    enum class How : uint8_t { IsGone, LostItsHandler };
    enum class Under : uint8_t { TypesAsTheyStand, TypesWithoutChecks };

    ValueID value;
    BlockID block;
    Opcode opcode;
    How how;
    Under under;
};

// Between two passes the operations that may throw or run code, on a way from the entry, only go with a reason. One walk over each body.
Vector<OperationThatWent> findOperationsThatWentWithoutReason(const EffectContext&, const Function& before, const Function& after, const ReasonsForOperationsThatWent&);
String describe(const OperationThatWent&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
