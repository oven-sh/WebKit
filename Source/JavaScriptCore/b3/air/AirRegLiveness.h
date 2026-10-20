/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirBasicBlock.h"
#include "AirCode.h"
#include "AirInst.h"
#include "RegisterSet.h"
#include <optional>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

class RegLiveness {
public:
    typedef Reg Thing;

    struct RegisterBits {
        uint64_t registers { 0 };
        uint64_t upperHalves { 0 };

        friend bool operator==(const RegisterBits&, const RegisterBits&) = default;
    };

    JS_EXPORT_PRIVATE explicit RegLiveness(Code&);
    JS_EXPORT_PRIVATE ~RegLiveness();

    JS_EXPORT_PRIVATE static RegisterSet toRegisterSet(RegisterBits);

private:
    static_assert(Reg::maxIndex() < 64);

    static constexpr uint8_t representsPadding = 1;
    static constexpr uint8_t definesOnlyRegistersLate = 2;

    struct Boundary {
        uint64_t use { 0 };
        uint64_t def { 0 };
    };

    struct BlockRecord {
        RegisterBits liveAtHead;
        RegisterBits liveAtTail;
        unsigned firstSlot { 0 };
        unsigned firstBoundary { 0 };
        unsigned numberOfSlots { 0 };
    };

public:
    class LocalCalc {
    public:
        LocalCalc(RegLiveness& liveness, BasicBlock* block)
            : m_boundaries(liveness.m_boundaries.span().data() + liveness.m_blocks[block->index()].firstBoundary)
            , m_upperHalfUses(liveness.m_upperHalfUses.isEmpty() ? nullptr : liveness.m_upperHalfUses.span().data() + liveness.m_blocks[block->index()].firstBoundary)
            , m_registerArgLateDefs(liveness.m_registerArgLateDefs.span().data() + liveness.m_blocks[block->index()].firstSlot)
            , m_slotFlags(liveness.m_slotFlags.span().data() + liveness.m_blocks[block->index()].firstSlot)
            , m_numberOfSlotsLeft(liveness.m_blocks[block->index()].numberOfSlots)
            , m_numberOfInstsLeft(block->size())
            , m_live(liveness.m_blocks[block->index()].liveAtTail)
        {
        }

        const RegisterSet& live() const
        {
            if (!m_liveSetBits || *m_liveSetBits != m_live) {
                m_liveSet = toRegisterSet(m_live);
                m_liveSetBits = m_live;
            }
            return m_liveSet;
        }

        bool isLive(Reg reg) const { return m_live.registers >> reg.index() & 1; }

        bool definesOnlyDeadRegisters(unsigned instIndex) const
        {
            ASSERT_UNUSED(instIndex, instIndex + 1 == m_numberOfInstsLeft);
            unsigned slot = m_numberOfSlotsLeft - 1;
            return (m_slotFlags[slot] & definesOnlyRegistersLate) && !(m_live.registers & m_registerArgLateDefs[slot]);
        }

        void execute(unsigned instIndex)
        {
            ASSERT_UNUSED(instIndex, instIndex + 1 == m_numberOfInstsLeft);
            --m_numberOfInstsLeft;
            executeSlot();
            while (m_numberOfSlotsLeft && (m_slotFlags[m_numberOfSlotsLeft - 1] & representsPadding))
                executeSlot();
        }

    private:
        ALWAYS_INLINE void executeSlot()
        {
            unsigned slot = --m_numberOfSlotsLeft;
            uint64_t def = m_boundaries[slot + 1].def;
            m_live.registers = (m_live.registers & ~def) | m_boundaries[slot].use;
            if (m_upperHalfUses) [[unlikely]]
                m_live.upperHalves = (m_live.upperHalves & ~def) | m_upperHalfUses[slot];
        }

        const Boundary* m_boundaries;
        const uint64_t* m_upperHalfUses;
        const uint64_t* m_registerArgLateDefs;
        const uint8_t* m_slotFlags;
        unsigned m_numberOfSlotsLeft;
        unsigned m_numberOfInstsLeft;
        RegisterBits m_live;
        mutable RegisterSet m_liveSet;
        mutable std::optional<RegisterBits> m_liveSetBits;
    };

    RegisterBits bitsLiveAtHead(BasicBlock* block) const { return m_blocks[block->index()].liveAtHead; }
    RegisterBits bitsLiveAtTail(BasicBlock* block) const { return m_blocks[block->index()].liveAtTail; }
    RegisterSet liveAtHead(BasicBlock* block) const { return toRegisterSet(bitsLiveAtHead(block)); }
    RegisterSet liveAtTail(BasicBlock* block) const { return toRegisterSet(bitsLiveAtTail(block)); }

private:
    struct TransferSets {
        RegisterBits gen;
        RegisterBits usedAtTail;
        uint64_t kill { 0 };
    };

    enum class UpperHalves : bool { Ignored, Tracked };

    struct DecodedInst;
    struct RegisterRows;

    bool tryDecode(Code&, Vector<TransferSets>&, UpperHalves);
    void solve(Code&, const Vector<TransferSets>&);

    Vector<BlockRecord> m_blocks;
    Vector<Boundary> m_boundaries;
    Vector<uint64_t> m_upperHalfUses;
    Vector<uint64_t> m_registerArgLateDefs;
    Vector<uint8_t> m_slotFlags;
};

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
