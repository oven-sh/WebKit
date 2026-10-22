/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTOpcode.h"
#include "AOTPassID.h"
#include <initializer_list>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class BodyTrait : uint8_t {
    SeveralBlocks = 1 << 0,
    Handlers = 1 << 1,
    // Of a block other than the entry block.
    BlockParameters = 1 << 2,
    // A terminator with more than one target.
    SeveralTargets = 1 << 3,
    // A value with a result whose type is never.
    NeverValues = 1 << 4,
};

// What a pass can tell from that it has nothing to do in a body. All values count, listed in a block or not, and all blocks, reachable or not: it says too much rather than too little.
struct BodyContents {
    OpcodeSet opcodes;
    TerminatorKindSet terminators;
    OptionSet<BodyTrait> traits;
};

BodyContents contentsOf(const Function&);

// In a body that holds none of these the pass leaves the function as it is and reports nothing, whatever its other inputs say, on its first run as on a later one.
struct PassTriggers {
    static constexpr PassTriggers always()
    {
        PassTriggers result;
        result.isAlways = true;
        return result;
    }

    static constexpr PassTriggers anyOf(std::initializer_list<Opcode> opcodes)
    {
        PassTriggers result;
        result.opcodes = OpcodeSet(opcodes);
        return result;
    }

    constexpr PassTriggers orOpcodes(const OpcodeSet& others) const
    {
        PassTriggers result = *this;
        result.opcodes |= others;
        return result;
    }

    constexpr PassTriggers orTerminators(std::initializer_list<TerminatorKind> kinds) const
    {
        PassTriggers result = *this;
        result.terminators |= TerminatorKindSet(kinds);
        return result;
    }

    constexpr PassTriggers orTraits(OptionSet<BodyTrait> others) const
    {
        PassTriggers result = *this;
        result.traits = result.traits | others;
        return result;
    }

    // Fires where both fire. Each of the two is a row of one list.
    static constexpr PassTriggers bothOf(const PassTriggers& first, const PassTriggers& second)
    {
        if (first.isAlways)
            return second;
        PassTriggers result = first;
        if (second.isAlways)
            return result;
        result.needsOneOfOthersToo = true;
        result.otherOpcodes = second.opcodes;
        result.otherTerminators = second.terminators;
        result.otherTraits = second.traits;
        return result;
    }

    constexpr bool mayFire(const BodyContents& contents) const
    {
        if (isAlways)
            return true;
        if (!opcodes.intersects(contents.opcodes) && !terminators.intersects(contents.terminators) && !traits.containsAny(contents.traits))
            return false;
        return !needsOneOfOthersToo || otherOpcodes.intersects(contents.opcodes) || otherTerminators.intersects(contents.terminators) || otherTraits.containsAny(contents.traits);
    }

    OpcodeSet opcodes;
    TerminatorKindSet terminators;
    OptionSet<BodyTrait> traits;
    OpcodeSet otherOpcodes;
    TerminatorKindSet otherTerminators;
    OptionSet<BodyTrait> otherTraits;
    bool needsOneOfOthersToo { false };
    bool isAlways { false };
};

const PassTriggers& triggersOf(PassID);

// The passes whose triggers may fire in such a body: triggersOf(pass).mayFire(contents) for all passes at once.
using PassesByTrigger = EnumeratorBitSet<PassID, numberOfPasses>;
PassesByTrigger passesThatMayFireIn(const BodyContents&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
