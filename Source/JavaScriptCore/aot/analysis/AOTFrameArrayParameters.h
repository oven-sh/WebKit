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

// An array that lies in a frame has the original Structure of arrays of values, no hole and at most MaxFrameArrayLength elements, and nothing ever writes to it.
class FrameArrayParameters {
public:
    FrameArrayParameters() = default;

    bool mayTake(FunctionID function, uint32_t entryParameter) const
    {
        return entryParameter < maxFrameArrayParameters && function.index() < m_words.size() && m_words[function.index()] >> entryParameter & 1;
    }

    // A caller that keeps its frame for a cell in it gives up a tail call, and the frame stays until the callee returns.
    bool endsItsTailCalls(FunctionID function) const { return function.index() < m_endsItsTailCalls.size() && m_endsItsTailCalls.quickGet(function.index()); }

    // A function that was made since holds operations that the table has seen where they were.
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
