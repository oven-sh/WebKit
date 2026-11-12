/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTID.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct StepAlongRow {
    std::optional<bool> answer;
    LayoutID next;
};

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

Vector<LayoutID> findNearestWantedAbove(size_t numberOfLayouts, NOESCAPE const Invocable<LayoutID(LayoutID)> auto& layoutAbove, NOESCAPE const Invocable<bool(LayoutID)> auto& isWanted)
{
    enum class State : uint8_t { Unknown, OnTheWay, Known };
    Vector<State> states(FillWith { }, numberOfLayouts, State::Unknown);
    Vector<LayoutID> nearest(numberOfLayouts);
    Vector<LayoutID, 32> passed;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID current(index);
        if (!layoutAbove(current))
            continue;
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
