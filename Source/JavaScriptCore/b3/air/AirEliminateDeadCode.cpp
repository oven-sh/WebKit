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
#include "AirEliminateDeadCode.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirFormTraits.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "AirTmpInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

bool eliminateDeadCode(Code& code)
{
    PhaseScope phaseScope(code, "eliminateDeadCode"_s);

    const unsigned gpIndicesEnd = Tmp::absoluteIndexEnd(code, GP);
    Vector<uint64_t, 64> liveTmpStore(FillWith { }, (gpIndicesEnd + Tmp::absoluteIndexEnd(code, FP) + 63) / 64, static_cast<uint64_t>(0));
    uint64_t* liveTmps = liveTmpStore.mutableSpan().data();
    Vector<uint64_t, 8> liveStackSlotStore(FillWith { }, (code.stackSlots().size() + 63) / 64, static_cast<uint64_t>(0));
    uint64_t* liveStackSlots = liveStackSlotStore.mutableSpan().data();

    auto indexOf = [&](Tmp tmp) ALWAYS_INLINE_LAMBDA -> unsigned {
        return tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : gpIndicesEnd + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
    };

    auto isArgLive = [&](const Arg& arg) ALWAYS_INLINE_LAMBDA -> bool {
        switch (arg.kind()) {
        case Arg::Tmp: {
            if (arg.isReg())
                return true;
            unsigned index = indexOf(arg.tmp());
            return liveTmps[index / 64] >> (index % 64) & 1;
        }
        case Arg::Stack: {
            if (arg.stackSlot()->isLocked())
                return true;
            unsigned index = arg.stackSlot()->index();
            return liveStackSlots[index / 64] >> (index % 64) & 1;
        }
        default:
            return true;
        }
    };

    auto isInstLiveByDefs = [&](Inst& inst) -> bool {
        // This instruction should be presumed dead, if its Args are all dead.
        unsigned argIndex = 0;
        if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
            Arg* args = inst.args().data();
            for (unsigned numberOfArgs = inst.args().size(); argIndex < numberOfArgs; ++argIndex) {
                if ((Greedy::formTraits[forms[argIndex]] & Greedy::Mentions::anyDef) && decodeFormRole(forms[argIndex]) != Arg::Scratch && isArgLive(args[argIndex]))
                    return true;
            }
            return false;
        }
        bool storesToLive = false;
        inst.forEachArg(
            [&] (Arg& arg, Arg::Role role, Bank, Width) {
                if (!Arg::isAnyDef(role))
                    return;
                if (role == Arg::Scratch)
                    return;
                storesToLive |= isArgLive(arg);
            });
        return storesToLive;
    };

    auto isInstLive = [&](Inst& inst) -> bool {
        return isInstLiveByDefs(inst) || inst.hasNonArgEffects();
    };

    auto markArgsLive = [&](Inst& inst) {
        for (Arg& arg : inst.args()) {
            if (arg.isStack() && !arg.stackSlot()->isLocked()) {
                unsigned index = arg.stackSlot()->index();
                liveStackSlots[index / 64] |= static_cast<uint64_t>(1) << (index % 64);
            }
            arg.forEachTmpFast(
                [&] (Tmp& tmp) {
                    unsigned index = indexOf(tmp);
                    liveTmps[index / 64] |= static_cast<uint64_t>(1) << (index % 64);
                });
        }
    };

    Vector<Inst*> backingStore;
    bool changed { false };

    for (unsigned blockIndex = code.size(); blockIndex--;) {
        BasicBlock* block = code[blockIndex];
        if (!block)
            continue;
        for (unsigned instIndex = block->size(); instIndex--;) {
            Inst& inst = block->at(instIndex);
            if (!isInstLive(inst))
                backingStore.append(&inst);
            else
                markArgsLive(inst);
        }
    }

    auto deadCandidatesInBackwardOrder = backingStore.mutableSpan();

    // Inst in deadCandidatesInBackwardOrder already have hasNonArgEffects() == false, invariant
    // under live-set growth, so the fixpoint skips that re-check.
    auto handleInst = [&](Inst& inst) -> bool {
        if (!isInstLiveByDefs(inst))
            return false;
        markArgsLive(inst);
        return true;
    };

    auto runBackward = [&] -> bool {
        changed = false;
        auto [begin, end] = std::ranges::remove_if(deadCandidatesInBackwardOrder,
            [&] (Inst* inst) -> bool {
                bool result = handleInst(*inst);
                changed |= result;
                return result;
            });
        deadCandidatesInBackwardOrder = deadCandidatesInBackwardOrder.first(begin - deadCandidatesInBackwardOrder.begin());
        return changed;
    };

    auto runForward = [&] -> bool {
        changed = false;
        auto [begin, end] = std::ranges::remove_if(deadCandidatesInBackwardOrder | std::views::reverse,
            [&] (Inst* inst) -> bool {
                bool result = handleInst(*inst);
                changed |= result;
                return result;
            });
        deadCandidatesInBackwardOrder = deadCandidatesInBackwardOrder.last(deadCandidatesInBackwardOrder.end() - begin.base());
        return changed;
    };

    for (;;) {
        // Propagating backward is most likely to be profitable.
        if (!runBackward())
            break;
        if (!runBackward())
            break;

        // Occasionally propagating forward greatly reduces the likelihood of pathologies.
        if (!runForward())
            break;
    }

    // After the fixpoint, deadCandidatesInBackwardOrder holds exactly the dead Insts. Null them
    // out and sweep with the cheap !inst predicate.
    if (deadCandidatesInBackwardOrder.empty())
        return false;

    for (Inst* inst : deadCandidatesInBackwardOrder)
        *inst = Inst();

    for (BasicBlock* block : code) {
        block->insts().removeAllMatching(
            [&] (Inst& inst) -> bool {
                return !inst;
            });
    }

    return true;
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)

