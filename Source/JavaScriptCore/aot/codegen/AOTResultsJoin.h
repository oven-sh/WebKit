/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperand.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

// Joins paths that each end with one value for every result of an operation.
template<typename... Results>
class ResultsJoin {
public:
    explicit ResultsJoin(Output& out)
        : m_out(out)
        , m_continuation(out.newBlock())
    {
    }

    void add(Results... results)
    {
        size_t index = 0;
        (m_incoming[index++].append(m_out.anchor(results.value())), ...);
        m_out.jump(m_continuation);
    }

    std::array<Operand, sizeof...(Results)> finish()
    {
        m_out.appendTo(m_continuation);
        size_t index = 0;
        return { Results(m_out.phi(b3TypeOf(Results::representation), m_incoming[index++]))... };
    }

private:
    Output& m_out;
    LBasicBlock m_continuation;
    std::array<Vector<ValueFromBlock, 4>, sizeof...(Results)> m_incoming;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
