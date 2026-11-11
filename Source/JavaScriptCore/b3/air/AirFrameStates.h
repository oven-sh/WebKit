/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirBasicBlock.h"
#include <optional>
#include <wtf/IndexMap.h>

namespace JSC { namespace B3 { namespace Air {

class Code;

enum class FrameState : uint8_t {
    Unreachable,
    NotSetUp,
    SetUp,
};

struct FrameNeeds {
    bool needsFrame { false };
    bool addressesIncomingArguments { false };
    bool namesStackPointer { false };
};

struct FrameStateViolation {
    enum class Kind : uint8_t {
        StatesDifferAtMerge,
        FrameSetUpTwice,
        FrameTornDownTwice,
        ReturnsWithFrame,
        NeedsFrame,
        WritesAheadOfFrame,
    };

    Kind kind;
    BasicBlock* block;
    unsigned instIndex;
};

JS_EXPORT_PRIVATE uint64_t savedCalleeSaveBits(const Code&);
JS_EXPORT_PRIVATE FrameNeeds frameNeedsOf(Inst&, uint64_t savedCalleeSaves);
JS_EXPORT_PRIVATE bool isIncomingArgumentAddress(const Arg&);
JS_EXPORT_PRIVATE bool writesMemoryOutsideFrame(Inst&);

class FrameStates {
public:
    JS_EXPORT_PRIVATE explicit FrameStates(Code&);

    FrameState atHead(BasicBlock* block) const { return m_atHead[block]; }
    FrameState atTail(BasicBlock* block) const { return m_atTail[block]; }
    const std::optional<FrameStateViolation>& violation() const { return m_violation; }

private:
    FrameState stateAfter(BasicBlock*, FrameState atHead);

    IndexMap<BasicBlock*, FrameState> m_atHead;
    IndexMap<BasicBlock*, FrameState> m_atTail;
    std::optional<FrameStateViolation> m_violation;
};

JS_EXPORT_PRIVATE std::optional<FrameStateViolation> findFrameStateViolation(Code&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
