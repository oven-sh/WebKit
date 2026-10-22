/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <limits>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

inline constexpr unsigned maxFrameArrayParameters = 32;

// An array that lies in a frame has the original Structure of arrays of values, no hole and at most MaxFrameArrayLength elements, and nothing ever writes to it.
// So it passes every test for a pristine array and for contiguous storage for as long as it exists.
// A function may be handed one in an entry parameter if nobody holds the pointer any more when the call returns, nothing writes to the array or makes it grow, user code never gets it,
// and native code gets no more than a copy of it: the operations that take it read it where it lies (readsArrayWhereItLies), and every function it is handed to may be handed one too.
// A path that such an array never takes, like the failing side of a test for a pristine array, does not count.
// The table answers for the entry parameters that some call hands a literal or a rest array, as the functions are when it is computed.
class FrameArrayParameters {
public:
    FrameArrayParameters() = default;

    bool mayTake(FunctionID function, uint32_t entryParameter) const
    {
        return entryParameter < maxFrameArrayParameters && function.index() < m_words.size() && m_words[function.index()] >> entryParameter & 1;
    }

    // A caller that keeps its frame for a cell in it gives up a tail call, and the frame stays until the callee returns. A program may count on tail calls that go round for ever without a frame staying behind.
    // So the callee has to be one whose tail calls come to an end: they go to functions that are all known, and to none of them twice.
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
