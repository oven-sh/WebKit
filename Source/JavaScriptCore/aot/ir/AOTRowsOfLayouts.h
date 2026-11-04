/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// A question about a layout is settled at the layout, or it has the answer of one other layout, mostly the class above.
struct StepAlongRow {
    std::optional<bool> answer;
    LayoutID next;
};

// Each layout is asked once, however long the rows of classes are: a walk to the top of its row for each class takes a step for each pair of classes.
// A walk that comes back to where it has been meets no answer. For whoever asks about some of the layouts, one at a time, always with the same step.
class AnswersAlongRows {
public:
    AnswersAlongRows(size_t numberOfLayouts, bool answerInRing)
        : m_states(FillWith { }, numberOfLayouts, State::Unknown)
        , m_answerInRing(answerInRing)
    {
    }

    bool answerFor(LayoutID id, NOESCAPE const Invocable<StepAlongRow(LayoutID)> auto& step)
    {
        bool answer = m_answerInRing;
        for (uint32_t current = id.index();;) {
            State state = m_states[current];
            if (state != State::Unknown) {
                if (state != State::OnTheWay)
                    answer = state == State::Yes;
                break;
            }
            m_states[current] = State::OnTheWay;
            m_passed.append(current);
            StepAlongRow taken = step(LayoutID(current));
            if (taken.answer) {
                answer = *taken.answer;
                break;
            }
            RELEASE_ASSERT(taken.next && taken.next.index() < m_states.size());
            current = taken.next.index();
        }
        for (uint32_t layout : m_passed)
            m_states[layout] = answer ? State::Yes : State::No;
        m_passed.shrink(0);
        return answer;
    }

private:
    enum class State : uint8_t { Unknown, OnTheWay, No, Yes };

    Vector<State> m_states;
    Vector<uint32_t, 32> m_passed;
    bool m_answerInRing;
};

// The layouts for which the answer is yes.
BitVector answerAlongRows(size_t numberOfLayouts, bool answerInRing, NOESCAPE const Invocable<StepAlongRow(LayoutID)> auto& step)
{
    AnswersAlongRows answers(numberOfLayouts, answerInRing);
    BitVector result(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (answers.answerFor(LayoutID(index), step))
            result.quickSet(index);
    }
    return result;
}

// For each layout the nearest layout above it that is wanted, or none: whoever goes from one to the next takes a step for each layout that he wants, not for each class of the row.
// In a ring, and on the way into one, a layout names the one above it, wanted or not.
Vector<LayoutID> findNearestWantedAbove(size_t numberOfLayouts, NOESCAPE const Invocable<LayoutID(LayoutID)> auto& layoutAbove, NOESCAPE const Invocable<bool(LayoutID)> auto& isWanted)
{
    enum class State : uint8_t { Unknown, OnTheWay, Known };
    Vector<State> states(FillWith { }, numberOfLayouts, State::Unknown);
    Vector<LayoutID> nearest(numberOfLayouts);
    Vector<LayoutID, 32> passed;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID current(index);
        for (; current && states[current.index()] == State::Unknown; current = layoutAbove(current)) {
            states[current.index()] = State::OnTheWay;
            passed.append(current);
        }
        bool hasMetRing = current && states[current.index()] == State::OnTheWay;
        while (!passed.isEmpty()) {
            LayoutID id = passed.takeLast();
            LayoutID above = layoutAbove(id);
            nearest[id.index()] = !above || hasMetRing || isWanted(above) ? above : nearest[above.index()];
            states[id.index()] = State::Known;
        }
    }
    return nearest;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
