/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <limits>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

AOT_THRESHOLD(unsigned, maxFrameArrayParameters, 32);

class FrameArrayParameters {
public:
    FrameArrayParameters() = default;

    bool mayTake(FunctionID function, uint32_t entryParameter) const
    {
        return entryParameter < maxFrameArrayParameters && function.index() < m_words.size() && m_words[function.index()] >> entryParameter & 1;
    }

    bool endsItsTailCalls(FunctionID function) const { return function.index() < m_endsItsTailCalls.size() && m_endsItsTailCalls.quickGet(function.index()); }

    bool isOlderThan(FunctionID function) const { return function.index() >= m_numberOfFunctions; }

    friend bool operator==(const FrameArrayParameters&, const FrameArrayParameters&) = default;

private:
    friend FrameArrayParameters computeFrameArrayParameters(const Program&, BuildContext&);

    Vector<uint32_t> m_words;
    BitVector m_endsItsTailCalls;
    uint32_t m_numberOfFunctions { std::numeric_limits<uint32_t>::max() };
};

FrameArrayParameters computeFrameArrayParameters(const Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
