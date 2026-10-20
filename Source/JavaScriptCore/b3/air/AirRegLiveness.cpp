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

#include "config.h"
#include "AirRegLiveness.h"

#if ENABLE(B3_JIT)

#include "AirBitMatrixLiveness.h"
#include "AirFormTraits.h"
#include "AirInstInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

namespace {

using Mentions = Greedy::Mentions;

} // anonymous namespace

struct RegLiveness::DecodedInst {
    ALWAYS_INLINE void noteTmp(Tmp tmp, unsigned traits)
    {
        if (!tmp.isReg())
            return;
        uint64_t bit = static_cast<uint64_t>(1) << tmp.reg().index();
        if (tmp.isFP() && static_cast<Width>((traits >> Mentions::widthShift) & Mentions::widthMask) > Width64) [[unlikely]] {
            namesWideRegister = true;
            upperHalfUses[0] |= traits & Mentions::earlyUse ? bit : 0;
            upperHalfUses[1] |= traits & Mentions::lateUse ? bit : 0;
        }
        regs[0] |= traits & Mentions::earlyUse ? bit : 0;
        regs[1] |= traits & Mentions::lateUse ? bit : 0;
        regs[2] |= traits & Mentions::earlyDef ? bit : 0;
        regs[3] |= traits & Mentions::lateDef ? bit : 0;
    }

    ALWAYS_INLINE void noteArg(Arg& arg, unsigned traits)
    {
        argTimings |= traits & Mentions::timingMask;
        if (traits & Mentions::lateDef) {
            if (arg.isReg())
                regArgLateDefs |= static_cast<uint64_t>(1) << arg.reg().index();
            else
                definesNonRegister = true;
        }
        forEachTmpWithTraits(arg, traits,
            [&](Tmp tmp, unsigned tmpTraits) ALWAYS_INLINE_LAMBDA {
                noteTmp(tmp, tmpTraits);
            });
    }

    NEVER_INLINE static DecodedInst decodeSlowly(Inst& inst)
    {
        DecodedInst result;
        inst.forEachArg(
            [&](Arg& arg, Arg::Role role, Bank bank, Width width) {
                result.noteArg(arg, Mentions::traitsOf(role, bank, width));
            });
        return result;
    }

    ALWAYS_INLINE static DecodedInst decodeInst(Inst& inst)
    {
        DecodedInst result;
        unsigned argIndex = 0;
        if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
            Arg* args = inst.args().data();
            const unsigned numberOfArgs = inst.args().size();
            for (; argIndex < numberOfArgs; ++argIndex)
                result.noteArg(args[argIndex], Greedy::formTraits[forms[argIndex]]);
        } else
            result = decodeSlowly(inst);
        return result;
    }

    std::array<uint64_t, 4> regs { };
    std::array<uint64_t, 2> upperHalfUses { };
    uint64_t regArgLateDefs { 0 };
    unsigned argTimings { 0 };
    bool namesWideRegister { false };
    bool definesNonRegister { false };
};

struct RegLiveness::RegisterRows {
    RegisterRows(const Vector<TransferSets>& transferSets, bool tracksUpperHalves)
        : transferSets(transferSets)
        , tracksUpperHalves(tracksUpperHalves)
    {
    }

    unsigned numIndices() const { return tracksUpperHalves ? 128 : 64; }
    bool needsLiveAtTail(BasicBlock*) const { return true; }

    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail) const
    {
        ASSERT((firstWord + numberOfWords) * 64 <= numIndices());
        const TransferSets& sets = transferSets[block->index()];
        uint64_t filled = 0;
        for (size_t i = 0; i < numberOfWords; ++i) {
            const bool holdsUpperHalves = firstWord + i;
            gen[i] |= holdsUpperHalves ? sets.gen.upperHalves : sets.gen.registers;
            kill[i] |= sets.kill;
            tail[i] |= holdsUpperHalves ? sets.usedAtTail.upperHalves : sets.usedAtTail.registers;
            filled |= gen[i] | kill[i] | tail[i];
        }
        return filled;
    }

    const Vector<TransferSets>& transferSets;
    bool tracksUpperHalves;
};

RegLiveness::RegLiveness(Code& code)
{
    Vector<TransferSets> transferSets;
    if (!tryDecode(code, transferSets, code.usesSIMD() ? UpperHalves::Tracked : UpperHalves::Ignored)) {
        bool didDecode = tryDecode(code, transferSets, UpperHalves::Tracked);
        RELEASE_ASSERT(didDecode);
    }
    solve(code, transferSets);
}

RegLiveness::~RegLiveness() = default;

bool RegLiveness::tryDecode(Code& code, Vector<TransferSets>& transferSets, UpperHalves upperHalves)
{
    const bool tracksUpperHalves = upperHalves == UpperHalves::Tracked;
    size_t numberOfInsts = 0;
    for (BasicBlock* block : code)
        numberOfInsts += block->size();
    m_blocks.grow(code.size());
    transferSets.grow(code.size());
    m_boundaries.grow(numberOfInsts * 2 + code.size() + 1);
    m_registerArgLateDefs.grow(numberOfInsts * 2 + 1);
    m_slotFlags.grow(numberOfInsts * 2 + 1);
    if (tracksUpperHalves)
        m_upperHalfUses.fill(0, m_boundaries.size());
    else
        m_upperHalfUses.clear();
    Boundary* boundaries = m_boundaries.mutableSpan().data();
    uint64_t* upperHalfUses = m_upperHalfUses.mutableSpan().data();
    uint64_t* registerArgLateDefs = m_registerArgLateDefs.mutableSpan().data();
    uint8_t* slotFlags = m_slotFlags.mutableSpan().data();
    unsigned slot = 0;
    unsigned boundary = 0;

    for (BasicBlock* block : code) {
        const unsigned firstSlot = slot;
        const unsigned firstBoundary = boundary;
        RegisterBits gen;
        uint64_t kill = 0;
        boundaries[boundary] = { 0, 0 };
        bool previousHasLateUseOrDef = false;

        auto finishBoundary = [&]() ALWAYS_INLINE_LAMBDA {
            kill |= boundaries[boundary].def;
            gen.registers |= boundaries[boundary].use & ~kill;
            if (tracksUpperHalves) [[unlikely]]
                gen.upperHalves |= upperHalfUses[boundary] & ~kill;
        };

        for (Inst& inst : *block) {
            const DecodedInst decoded = DecodedInst::decodeInst(inst);
            if (decoded.namesWideRegister && !tracksUpperHalves) [[unlikely]]
                return false;

            bool hasEarlyDef = decoded.argTimings & Mentions::earlyDef;
            bool hasLateUseOrDef = decoded.argTimings & (Mentions::lateUse | Mentions::lateDef);
            if (inst.kind.opcode == Patch) [[unlikely]] {
                hasEarlyDef |= !inst.extraEarlyClobberedRegs().isEmpty();
                hasLateUseOrDef |= !inst.extraClobberedRegs().isEmpty();
            }

            if (previousHasLateUseOrDef && hasEarlyDef) {
                finishBoundary();
                boundaries[++boundary] = { 0, 0 };
                registerArgLateDefs[slot] = 0;
                slotFlags[slot++] = representsPadding;
            }
            previousHasLateUseOrDef = hasLateUseOrDef;

            boundaries[boundary].use |= decoded.regs[0];
            boundaries[boundary].def |= decoded.regs[2];
            if (tracksUpperHalves) [[unlikely]]
                upperHalfUses[boundary] |= decoded.upperHalfUses[0];
            finishBoundary();
            boundaries[++boundary] = { decoded.regs[1], decoded.regs[3] };
            if (tracksUpperHalves) [[unlikely]]
                upperHalfUses[boundary] = decoded.upperHalfUses[1];
            registerArgLateDefs[slot] = decoded.regArgLateDefs;
            slotFlags[slot++] = !(decoded.argTimings & Mentions::earlyDef) && !decoded.definesNonRegister ? definesOnlyRegistersLate : 0;
        }

        RegisterBits usedAtTail { boundaries[boundary].use, tracksUpperHalves ? upperHalfUses[boundary] : 0 };
        m_blocks[block->index()] = BlockRecord { { }, { }, firstSlot, firstBoundary, slot - firstSlot };
        transferSets[block->index()] = TransferSets { gen, usedAtTail, kill | boundaries[boundary].def };
        ++boundary;
    }
    return true;
}

void RegLiveness::solve(Code& code, const Vector<TransferSets>& transferSets)
{
    const bool tracksUpperHalves = !m_upperHalfUses.isEmpty();
    BitMatrixLiveness<RegisterRows> liveness(code, transferSets, tracksUpperHalves);
    liveness.compute();
    RELEASE_ASSERT(liveness.status() == BitMatrixLivenessStatus::Solved);

    auto workset = liveness.makeWorkset();
    for (BasicBlock* block : code) {
        liveness.copyLiveAtTailInto(workset, block);
        const TransferSets& sets = transferSets[block->index()];
        BlockRecord& record = m_blocks[block->index()];
        record.liveAtTail = { workset.words()[0], tracksUpperHalves ? workset.words()[1] : 0 };
        record.liveAtHead.registers = sets.gen.registers | (record.liveAtTail.registers & ~sets.kill);
        record.liveAtHead.upperHalves = sets.gen.upperHalves | (record.liveAtTail.upperHalves & ~sets.kill);
    }
}

RegisterSet RegLiveness::toRegisterSet(RegisterBits bits)
{
    RegisterSet result;
    for (uint64_t registers = bits.registers; registers; registers &= registers - 1) {
        unsigned index = std::countr_zero(registers);
        Reg reg = Reg::fromIndex(index);
        result.add(reg, (bits.upperHalves >> index & 1) ? conservativeWidth(reg) : conservativeWidthWithoutVectors(reg));
    }
    return result;
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
