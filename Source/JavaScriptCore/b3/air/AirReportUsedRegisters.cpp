/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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
#include "AirReportUsedRegisters.h"

#if ENABLE(B3_JIT)

#include "AirCFG.h"
#include "AirCode.h"
#include "AirFormTable.h"
#include "AirInstInlines.h"
#include "AirPadInterference.h"
#include "AirRegLiveness.h"
#include "AirPhaseScope.h"
#include <wtf/GraphOrdering.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

#if USE(BUN_JSC_ADDITIONS)
namespace {

class UsedRegistersReporter {
public:
    UsedRegistersReporter(Code& code)
        : m_code(code)
    {
    }

    bool tryRun(bool& changed)
    {
        if (m_code.usesSIMD() || !decode())
            return false;
        solve();
        changed = report();
        return true;
    }

private:
    static constexpr uint8_t representsPadding = 1;
    static constexpr uint8_t mayBeDeleted = 2;
    static constexpr uint8_t isPatch = 4;

    static constexpr unsigned earlyUse = 1;
    static constexpr unsigned lateUse = 2;
    static constexpr unsigned earlyDef = 4;
    static constexpr unsigned lateDef = 8;
    static constexpr unsigned isWide = 16;

    struct Boundary {
        uint64_t use;
        uint64_t def;
    };

    struct Sets {
        uint64_t gen { 0 };
        uint64_t kill { 0 };
        uint64_t head { 0 };
        uint64_t tail { 0 };
        unsigned firstSlot { 0 };
        unsigned firstBoundary { 0 };
    };

    static constexpr unsigned timingOf(Arg::Role role)
    {
        switch (role) {
        case Arg::Use:
        case Arg::ColdUse:
            return earlyUse;
        case Arg::LateUse:
        case Arg::LateColdUse:
            return lateUse;
        case Arg::Def:
        case Arg::ZDef:
            return lateDef;
        case Arg::UseDef:
        case Arg::UseZDef:
            return earlyUse | lateDef;
        case Arg::EarlyDef:
        case Arg::EarlyZDef:
            return earlyDef;
        case Arg::Scratch:
            return earlyDef | lateUse;
        case Arg::UseAddr:
            return 0;
        }
        return 0;
    }

    static constexpr std::array<uint8_t, 256> computeFormTimings()
    {
        std::array<uint8_t, 256> result { };
        for (unsigned form = 0; form < 256; ++form) {
            unsigned role = (form >> formRoleShift) & formRoleMask;
            if (role > Arg::UseAddr)
                continue;
            result[form] = timingOf(static_cast<Arg::Role>(role)) | (((form >> formWidthShift) & formWidthMask) > encodeFormWidth(Width64) ? isWide : 0);
        }
        return result;
    }

    struct MentionDecodedSlowly {
        Arg* arg;
        Tmp tmp;
        uint8_t argTiming;
        uint8_t tmpTiming;
    };

    NEVER_INLINE static void decodeSlowly(Inst& inst, Vector<MentionDecodedSlowly, 16>& mentions)
    {
        inst.forEachArg(
            [&](Arg& arg, Arg::Role argRole, Bank argBank, Width argWidth) {
                bool namesTmp = false;
                arg.forEachTmp(argRole, argBank, argWidth,
                    [&](Tmp& tmp, Arg::Role role, Bank, Width width) {
                        namesTmp = true;
                        mentions.append(MentionDecodedSlowly { &arg, tmp, static_cast<uint8_t>(timingOf(argRole)), static_cast<uint8_t>(timingOf(role) | (width > Width64 ? isWide : 0)) });
                    });
                if (!namesTmp)
                    mentions.append(MentionDecodedSlowly { &arg, Tmp(), static_cast<uint8_t>(timingOf(argRole)), 0 });
            });
    }

    bool decode()
    {
        static constexpr std::array<uint8_t, 256> formTimings = computeFormTimings();
        constexpr unsigned kindsWithBase = 1u << Arg::SimpleAddr | 1u << Arg::Addr | 1u << Arg::ExtendedOffsetAddr | 1u << Arg::Index | 1u << Arg::PreIndex | 1u << Arg::PostIndex;

        size_t numberOfInsts = 0;
        for (BasicBlock* block : m_code)
            numberOfInsts += block->size();
        m_sets.grow(m_code.size());
        m_boundaries.grow(numberOfInsts * 2 + m_code.size() + 1);
        m_argLateDefs.grow(numberOfInsts * 2 + 1);
        m_flags.grow(numberOfInsts * 2 + 1);
        Boundary* boundaries = m_boundaries.mutableSpan().data();
        uint64_t* argLateDefs = m_argLateDefs.mutableSpan().data();
        uint8_t* flags = m_flags.mutableSpan().data();
        unsigned slot = 0;
        unsigned boundary = 0;
        Vector<MentionDecodedSlowly, 16> mentionsDecodedSlowly;

        for (BasicBlock* block : m_code) {
            Sets& sets = m_sets[block->index()];
            sets.firstSlot = slot;
            sets.firstBoundary = boundary;
            uint64_t gen = 0;
            uint64_t kill = 0;
            boundaries[boundary] = { 0, 0 };
            bool previousHasLateUseOrDef = false;

            for (Inst& inst : *block) {
                Arg* args = inst.args().data();
                const unsigned numberOfArgs = inst.args().size();
                std::array<uint64_t, 4> regs { };
                unsigned argTimings = 0;
                unsigned tmpTimings = 0;
                uint64_t regArgLateDefs = 0;
                bool definesNonRegister = false;

                auto note = [&](Tmp tmp, unsigned timing) ALWAYS_INLINE_LAMBDA {
                    if (!tmp.isReg())
                        return;
                    uint64_t bit = static_cast<uint64_t>(1) << tmp.reg().index();
                    tmpTimings |= tmp.isGP() ? timing & ~isWide : timing;
                    regs[0] |= timing & earlyUse ? bit : 0;
                    regs[1] |= timing & lateUse ? bit : 0;
                    regs[2] |= timing & earlyDef ? bit : 0;
                    regs[3] |= timing & lateDef ? bit : 0;
                };

                unsigned argIndex = 0;
                if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
                    for (; argIndex < numberOfArgs; ++argIndex) {
                        Arg& arg = args[argIndex];
                        unsigned timing = formTimings[forms[argIndex]];
                        argTimings |= timing;
                        unsigned kind = arg.kind();
                        if (kind == Arg::Tmp) {
                            note(arg.tmp(), timing);
                            if (timing & lateDef) {
                                if (arg.isReg())
                                    regArgLateDefs |= static_cast<uint64_t>(1) << arg.reg().index();
                                else
                                    definesNonRegister = true;
                            }
                            continue;
                        }
                        definesNonRegister |= timing & lateDef;
                        if (!(kindsWithBase >> kind & 1))
                            continue;
                        unsigned tmpTiming = kind == Arg::PreIndex || kind == Arg::PostIndex ? earlyUse | lateDef : earlyUse;
                        note(arg.base(), tmpTiming);
                        if (kind == Arg::Index)
                            note(arg.index(), tmpTiming);
                    }
                } else {
                    mentionsDecodedSlowly.shrink(0);
                    decodeSlowly(inst, mentionsDecodedSlowly);
                    for (auto& mention : mentionsDecodedSlowly) {
                        argTimings |= mention.argTiming;
                        if (mention.tmp)
                            note(mention.tmp, mention.tmpTiming);
                        if (mention.argTiming & lateDef) {
                            if (mention.arg->isReg())
                                regArgLateDefs |= static_cast<uint64_t>(1) << mention.arg->reg().index();
                            else
                                definesNonRegister = true;
                        }
                    }
                }
                if (tmpTimings & isWide) [[unlikely]]
                    return false;

                bool hasEarlyDef = argTimings & earlyDef;
                bool hasLateUseOrDef = argTimings & (lateUse | lateDef);
                uint8_t instFlags = 0;
                if (inst.kind.opcode == Patch) [[unlikely]] {
                    instFlags = isPatch;
                    hasEarlyDef |= !inst.extraEarlyClobberedRegs().isEmpty();
                    hasLateUseOrDef |= !inst.extraClobberedRegs().isEmpty();
                }

                if (previousHasLateUseOrDef && hasEarlyDef) {
                    kill |= boundaries[boundary].def;
                    gen |= boundaries[boundary].use & ~kill;
                    boundaries[++boundary] = { 0, 0 };
                    flags[slot++] = representsPadding;
                }
                previousHasLateUseOrDef = hasLateUseOrDef;

                if (!(argTimings & earlyDef) && !definesNonRegister && !inst.hasNonArgEffects())
                    instFlags |= mayBeDeleted;

                boundaries[boundary].use |= regs[0];
                boundaries[boundary].def |= regs[2];
                kill |= boundaries[boundary].def;
                gen |= boundaries[boundary].use & ~kill;
                boundaries[++boundary] = { regs[1], regs[3] };
                argLateDefs[slot] = regArgLateDefs;
                flags[slot++] = instFlags;
            }
            sets.gen = gen;
            sets.kill = kill | boundaries[boundary].def;
            sets.tail = boundaries[boundary].use;
            ++boundary;
        }
        m_numberOfSlots = slot;
        return true;
    }

    void solve()
    {
        CFG cfg(m_code);
        Vector<unsigned, 64> worklist;
        BitVector dirtyBlocks(m_code.size());
        worklist.reserveInitialCapacity(m_code.size());
        appendNodeIndicesInOrder(cfg, GraphOrder::PostOrder, dirtyBlocks, worklist);
        worklist.reverse();
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            if (m_code[blockIndex] && !dirtyBlocks.quickSet(blockIndex))
                worklist.append(blockIndex);
        }

        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            dirtyBlocks.quickClear(blockIndex);
            Sets& sets = m_sets[blockIndex];
            uint64_t head = sets.gen | (sets.tail & ~sets.kill);
            if (!(head & ~sets.head))
                continue;
            sets.head = head;
            for (BasicBlock* predecessor : m_code[blockIndex]->predecessors()) {
                uint64_t& tail = m_sets[predecessor->index()].tail;
                if (!(head & ~tail))
                    continue;
                tail |= head;
                if (!dirtyBlocks.quickSet(predecessor->index()))
                    worklist.append(predecessor->index());
            }
        }
    }

    bool report()
    {
        const Boundary* boundaries = m_boundaries.span().data();
        const uint64_t* argLateDefs = m_argLateDefs.span().data();
        const uint8_t* flags = m_flags.span().data();
        uint64_t reported = 0;
        RegisterSet reportedSet;
        bool changed = false;
        unsigned endOfSlots = m_numberOfSlots;

        for (unsigned blockIndex = m_code.size(); blockIndex--;) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;
            const Sets& sets = m_sets[blockIndex];
            uint64_t live = sets.tail;
            unsigned instIndex = block->size();
            unsigned removed = 0;
            const Boundary* blockBoundaries = boundaries + sets.firstBoundary;
            for (unsigned slot = endOfSlots; slot-- > sets.firstSlot;) {
                uint8_t instFlags = flags[slot];
                unsigned slotInBlock = slot - sets.firstSlot;
                if (!(instFlags & representsPadding)) {
                    --instIndex;
                    if ((instFlags & mayBeDeleted) && !(live & argLateDefs[slot])) {
                        block->at(instIndex) = Inst();
                        ++removed;
                    } else if (instFlags & isPatch) {
                        if (live != reported) {
                            reported = live;
                            reportedSet = { };
                            for (uint64_t regs = live; regs; regs &= regs - 1)
                                reportedSet.add(Reg::fromIndex(std::countr_zero(regs)), IgnoreVectors);
                        }
                        block->at(instIndex).reportUsedRegisters(reportedSet);
                    }
                }
                live = (live & ~blockBoundaries[slotInBlock + 1].def) | blockBoundaries[slotInBlock].use;
            }
            ASSERT(!instIndex);
            endOfSlots = sets.firstSlot;

            if (!removed)
                continue;
            unsigned sizeBefore = block->size();
            block->insts().removeAllMatching(
                [&](const Inst& inst) -> bool {
                    return !inst;
                });
            if (sizeBefore - removed <= 1)
                changed = true;
        }
        return changed;
    }

    Code& m_code;
    Vector<Sets> m_sets;
    Vector<Boundary> m_boundaries;
    Vector<uint64_t> m_argLateDefs;
    Vector<uint8_t> m_flags;
    unsigned m_numberOfSlots { 0 };
};

} // anonymous namespace
#endif

bool reportUsedRegisters(Code& code)
{
    PhaseScope phaseScope(code, "reportUsedRegisters"_s);

    static constexpr bool verbose = false;

#if USE(BUN_JSC_ADDITIONS)
    {
        bool changed = false;
        UsedRegistersReporter reporter(code);
        if (reporter.tryRun(changed))
            return changed;
    }
#endif

    // The RegLiveness below reasons about interference at instruction boundaries, so it is only
    // correct once padding separates a late use or def from a following early def.
    padInterference(code);

    if (verbose)
        dataLog("Doing reportUsedRegisters on:\n", code);

    RegLiveness liveness(code);

    bool changed = false;
    for (BasicBlock* block : code) {
        if (verbose)
            dataLog("Looking at: ", *block, "\n");

        RegLiveness::LocalCalc localCalc(liveness, block);

        for (unsigned instIndex = block->size(); instIndex--;) {
            Inst& inst = block->at(instIndex);

            if (verbose)
                dataLog("   Looking at: ", inst, "\n");

            // Kill dead assignments to registers. For simplicity we say that a store is killable if
            // it has only late defs and those late defs are to registers that are dead right now.
            if (!inst.hasNonArgEffects()) {
                bool canDelete = true;
                inst.forEachArg(
                    [&] (Arg& arg, Arg::Role role, Bank, Width) {
                        if (Arg::isEarlyDef(role)) {
                            if (verbose)
                                dataLog("        Cannot delete because of ", arg, "\n");
                            canDelete = false;
                            return;
                        }
                        if (!Arg::isLateDef(role))
                            return;
                        if (!arg.isReg()) {
                            if (verbose)
                                dataLog("        Cannot delete because arg is not reg: ", arg, "\n");
                            canDelete = false;
                            return;
                        }
                        if (localCalc.isLive(arg.reg())) {
                            if (verbose)
                                dataLog("        Cannot delete because arg is live: ", arg, "\n");
                            canDelete = false;
                            return;
                        }
                    });
                if (canDelete)
                    inst = Inst();
            }

            if (inst.kind.opcode == Patch)
                inst.reportUsedRegisters(localCalc.live());
            localCalc.execute(instIndex);
        }

        unsigned sizeBefore = block->size();
        unsigned removed = block->insts().removeAllMatching(
            [&](const Inst& inst) -> bool {
                return !inst;
            });
        if (removed && sizeBefore - removed <= 1)
            changed = true;
    }

    if (verbose)
        dataLog("After reportUsedRegisters:\n", code);

    return changed;
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)


