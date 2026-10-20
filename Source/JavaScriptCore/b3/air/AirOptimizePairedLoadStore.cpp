/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "AirOptimizePairedLoadStore.h"

#if ENABLE(B3_JIT)
#if CPU(ARM64)

#include "AirArgInlines.h"
#include "AirCode.h"
#include "AirFrameStates.h"
#include "AirInst.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "CCallHelpers.h"
#include <wtf/Range.h>

namespace JSC { namespace B3 { namespace Air {
namespace AirOptimizePairedLoadStoreInternal {
static constexpr bool verbose = false;
static constexpr unsigned scanInstructions = 16;
}

static inline Width LEGACY_NODELETE accessWidth(Opcode opcode)
{
    switch (opcode) {
    case Move:
        return pointerWidth();
    case Move32:
        return Width32;
    case MoveFloat:
        return Width32;
    case MoveDouble:
        return Width64;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return Width8;
    }
}

static bool tryStorePair(Code& code, BasicBlock* block, unsigned current, Inst& inst, bool frameIsSetUp)
{
    Width instWidth = accessWidth(inst.kind.opcode);
    int64_t instOffset = static_cast<int64_t>(inst.args()[1].offset());
    int64_t bytesBelowFrameRecord = code.bytesBelowFrameRecord();
    unsigned limit = std::min(current, AirOptimizePairedLoadStoreInternal::scanInstructions);
    RegisterSet clobbered;
    for (unsigned count = 1; count <= limit; ++count) {
        unsigned index = current - count;
        Inst& target = block->at(index);

        auto logFound = [&](const Inst& newInst) {
            if (AirOptimizePairedLoadStoreInternal::verbose) {
                dataLogLn("FOUND  ", inst, " ", target, " => ", newInst);
                for (unsigned i = 1; i < count; ++i)
                    dataLogLn("    ", block->at(current - i));
            }
        };

        auto logFailed = [&]() {
            if (AirOptimizePairedLoadStoreInternal::verbose) {
                dataLogLn("FAILED ", inst, " ", target);
                for (unsigned i = 1; i < count; ++i)
                    dataLogLn("    ", block->at(current - i));
            }
        };

        // If the instruction has some special effect (including Patchpoint), we give up since we cannot model the effect of this.
        if (target.hasNonArgEffects()) {
            logFailed();
            return false;
        }

        // If some instructions between the current and target clobbers registers used for current / target,
        // then we cannot merge them since the current instruction's registers are changed.
        //
        //     stur %x1, [%fp]
        //     movz %x2, #0
        //     stur %x2, [%fp, #8]
        //
        // Then we cannot make it to
        //
        //     stp %x1, %x2, [%fp]
        //     movz %x2, #0
        Inst::forEachDefWithExtraClobberedRegs<Tmp>(
            &block->at(index), &block->at(index + 1),
            [&] (const Tmp& arg, Arg::Role, Bank, Width, PreservedWidth) {
                clobbered.add(arg.reg(), IgnoreVectors);
            });
        if (clobbered.contains(inst.args()[1].base().reg(), IgnoreVectors) || (inst.args()[0].isTmp() && clobbered.contains(inst.args()[0].reg(), IgnoreVectors))) {
            logFailed();
            return false;
        }

        {
            // If some instructions between the current and target have memory-load or memory-store,
            // then we cannot merge them since reordering can change the results.
            // But this is really pessimistic: if the base is the same to the current instruction, and if the offset
            // is different from the current instruction, it is OK actually.

            bool interfere = false;

            auto clobberMemory = [&](const Tmp& argBase, int64_t argOffset, Width argWidth) {
                if (argBase == inst.args()[1].base()) {
                    Range<int64_t> argRange(argOffset, argOffset + bytesForWidth(argWidth));
                    Range<int64_t> instRange(instOffset, instOffset + bytesForWidth(instWidth));
                    return argRange.overlaps(instRange);
                }

                if ((argBase == Tmp(CCallHelpers::stackPointerRegister) || argBase == Tmp(GPRInfo::callFrameRegister)) && (inst.args()[1].base() == Tmp(CCallHelpers::stackPointerRegister) || inst.args()[1].base() == Tmp(GPRInfo::callFrameRegister))) {
                    int64_t instOffsetFromFP = instOffset;
                    if (inst.args()[1].base() == Tmp(CCallHelpers::stackPointerRegister))
                        instOffsetFromFP = instOffset - bytesBelowFrameRecord;

                    int64_t argOffsetFromFP = argOffset;
                    if (argBase == Tmp(CCallHelpers::stackPointerRegister))
                        argOffsetFromFP = argOffset - bytesBelowFrameRecord;

                    Range<int64_t> argRange(argOffsetFromFP, argOffsetFromFP + bytesForWidth(argWidth));
                    Range<int64_t> instRange(instOffsetFromFP, instOffsetFromFP + bytesForWidth(instWidth));
                    return argRange.overlaps(instRange);
                }

                return true;
            };

            auto checkInterfere = [&](const Arg& arg, Arg::Role, Bank, Width width) {
                if (!arg.isMemory())
                    return;
                if (arg.isAddr()) {
                    if (!clobberMemory(arg.base(), static_cast<int64_t>(arg.offset()), width))
                        return;
                }
                if (arg.isSimpleAddr()) {
                    if (!clobberMemory(arg.base(), 0, width))
                        return;
                }
                interfere = true;
            };

            Inst::forEachUse<Arg>(&block->at(index), &block->at(index + 1), checkInterfere);
            if (interfere) {
                logFailed();
                return false;
            }
            Inst::forEachDef<Arg>(&block->at(index), &block->at(index + 1), checkInterfere);
            if (interfere) {
                logFailed();
                return false;
            }
        }

        if (target.kind != inst.kind)
            continue;

        if (target.args().size() != 2)
            continue;

        if ((!target.args()[0].isTmp() && !target.args()[0].isZeroReg()) || !target.args()[1].isAddr())
            continue;

        if (target.args()[1].base() != inst.args()[1].base())
            continue;

        Opcode pairOpcode = StorePair32;
        switch (inst.kind.opcode) {
        case Move32:
            pairOpcode = StorePair32;
            break;
        case Move:
            pairOpcode = StorePair64;
            break;
        case MoveDouble:
            pairOpcode = StorePairDouble;
            break;
        case MoveFloat:
            pairOpcode = StorePairFloat;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }

        auto isValidOffset = [&](auto offset) {
            switch (inst.kind.opcode) {
            case Move32:
                return ARM64Assembler::isValidSTPImm<32>(offset);
            case Move:
                return ARM64Assembler::isValidSTPImm<64>(offset);
            case MoveDouble:
                return ARM64Assembler::isValidSTPFPImm<64>(offset);
            case MoveFloat:
                return ARM64Assembler::isValidSTPFPImm<32>(offset);
            default:
                RELEASE_ASSERT_NOT_REACHED();
                return false;
            }
        };

        int64_t targetOffset = static_cast<int64_t>(target.args()[1].offset());

        if (isValidOffset(instOffset) && targetOffset == (instOffset + bytesForWidth(instWidth))) {
            Inst newInst(pairOpcode, target.origin, inst.args()[0], target.args()[0], inst.args()[1]);
            logFound(newInst);
            target = newInst;
            return true;
        }

        if (isValidOffset(targetOffset) && (targetOffset + bytesForWidth(instWidth)) == instOffset) {
            Inst newInst(pairOpcode, target.origin, target.args()[0], inst.args()[0], target.args()[1]);
            logFound(newInst);
            target = newInst;
            return true;
        }

        // Because str pimm only takes unsigned offset, we tend to pick stackPointerRegister based offsetting.
        // But it is possible that framePointerRegister based offsetting can offer a benefit here.
        if (frameIsSetUp && target.args()[1].base() == Tmp(CCallHelpers::stackPointerRegister)) {
            int64_t instOffsetFromFP = instOffset - bytesBelowFrameRecord;
            int64_t targetOffsetFromFP = targetOffset - bytesBelowFrameRecord;

            if (isValidOffset(instOffsetFromFP) && targetOffsetFromFP == (instOffsetFromFP + bytesForWidth(instWidth))) {
                Inst newInst(pairOpcode, target.origin, inst.args()[0], target.args()[0], Arg::addr(Air::Tmp(GPRInfo::callFrameRegister), static_cast<int32_t>(instOffsetFromFP)));
                logFound(newInst);
                target = newInst;
                return true;
            }

            if (isValidOffset(targetOffsetFromFP) && (targetOffsetFromFP + bytesForWidth(instWidth)) == instOffsetFromFP) {
                Inst newInst(pairOpcode, target.origin, target.args()[0], inst.args()[0], Arg::addr(Air::Tmp(GPRInfo::callFrameRegister), static_cast<int32_t>(targetOffsetFromFP)));
                logFound(newInst);
                target = newInst;
                return true;
            }
        }
    }

    return false;
}

static bool tryLoadPair(Code& code, BasicBlock* block, unsigned current, Inst& inst)
{
    Width instWidth = accessWidth(inst.kind.opcode);
    int64_t size = bytesForWidth(instWidth);
    int64_t instOffset = static_cast<int64_t>(inst.args()[0].offset());
    Tmp base = inst.args()[0].base();
    Tmp loaded = inst.args()[1].tmp();
    if (loaded == base)
        return false;
    auto isFrameRegister = [](Tmp tmp) {
        return tmp == Tmp(CCallHelpers::stackPointerRegister) || tmp == Tmp(GPRInfo::callFrameRegister);
    };
    int64_t bytesBelowFrameRecord = code.bytesBelowFrameRecord();
    auto fromFP = [&](Tmp tmp, int64_t offset) {
        return tmp == Tmp(CCallHelpers::stackPointerRegister) ? offset - bytesBelowFrameRecord : offset;
    };
    auto mayOverlap = [&](const Arg& arg, Width width) {
        if (!arg.isAddr() && !arg.isSimpleAddr())
            return true;
        int64_t offset = arg.isAddr() ? static_cast<int64_t>(arg.offset()) : 0;
        if (arg.base() == base)
            return Range<int64_t>(offset, offset + bytesForWidth(width)).overlaps(Range<int64_t>(instOffset, instOffset + size));
        if (isFrameRegister(arg.base()) && isFrameRegister(base))
            return Range<int64_t>(fromFP(arg.base(), offset), fromFP(arg.base(), offset) + bytesForWidth(width)).overlaps(Range<int64_t>(fromFP(base, instOffset), fromFP(base, instOffset) + size));
        return true;
    };
    unsigned limit = std::min(current, AirOptimizePairedLoadStoreInternal::scanInstructions);
    for (unsigned count = 1; count <= limit; ++count) {
        Inst& target = block->at(current - count);
        if (target.hasNonArgEffects())
            return false;
        bool blocksPairing = false;
        target.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width width) {
            if (arg.isMemory() && Arg::isAnyDef(role) && mayOverlap(arg, width))
                blocksPairing = true;
            if (arg.isTmp() && Arg::isAnyDef(role) && arg.tmp() == base)
                blocksPairing = true;
            arg.forEachTmpFast([&](Tmp& tmp) {
                if (tmp == loaded)
                    blocksPairing = true;
            });
        });
        if (blocksPairing)
            return false;
        if (target.kind != inst.kind || target.args().size() != 2 || !target.args()[0].isAddr() || !target.args()[1].isGPTmp() || target.args()[0].base() != base)
            continue;
        int64_t targetOffset = static_cast<int64_t>(target.args()[0].offset());
        bool targetIsFirst = targetOffset + size == instOffset;
        if (!targetIsFirst && instOffset + size != targetOffset)
            continue;
        int64_t offset = targetIsFirst ? targetOffset : instOffset;
        if (!(instWidth == Width64 ? ARM64Assembler::isValidLDPImm<64>(static_cast<int>(offset)) : ARM64Assembler::isValidLDPImm<32>(static_cast<int>(offset))))
            continue;
        Tmp other = target.args()[1].tmp();
        target = Inst(instWidth == Width64 ? LoadPair64 : LoadPair32, target.origin, Arg::addr(base, static_cast<int32_t>(offset)), targetIsFirst ? other : loaded, targetIsFirst ? loaded : other);
        return true;
    }
    return false;
}

bool optimizePairedLoadStore(Code& code)
{
    constexpr bool verbose = false;

    PhaseScope phaseScope(code, "optimizePairedLoadStore"_s);
    const bool pairsMemoryAccesses = code.policy().peepholes.pairsMemoryAccesses;
    std::optional<FrameStates> frameStates;
    if (code.policy().frame.emitsFrame)
        frameStates.emplace(code);

    if (verbose) {
        dataLog("Air before an iteration of optimizePairedLoadStore:\n");
        dataLog(code);
    }

    bool changed = false;
    for (BasicBlock* block : code) {
        // Bun: removed all at once below. Removing each where it is found is quadratic in the size of the block.
        bool removedAny = false;
        bool frameIsSetUp = !frameStates || frameStates->atTail(block) == FrameState::SetUp;
        unsigned index = block->size();
        while (index--) {
            Inst& inst = block->at(index);
            if (inst.kind.opcode == EnterFrame || inst.kind.opcode == LeaveFrame)
                frameIsSetUp = inst.kind.opcode == LeaveFrame;
            if (inst.hasNonArgEffects())
                continue;

            switch (inst.kind.opcode) {
            case Move:
            case Move32: {
                if (inst.args().size() != 2)
                    continue;

                if ((inst.args()[0].isGPTmp() || inst.args()[0].isZeroReg()) && inst.args()[1].isAddr()) {
                    // sp & fp slot usage is, in particular, different for call args and spills.
                    // We would like to do stp merging only for spills.
                    if ((inst.args()[1].base() == Tmp(CCallHelpers::stackPointerRegister) || inst.args()[1].base() == Tmp(CCallHelpers::framePointerRegister)) && !inst.kind.spill && !pairsMemoryAccesses)
                        continue;
                    if (tryStorePair(code, block, index, inst, frameIsSetUp)) {
                        block->at(index) = Inst();
                        removedAny = true;
                        changed = true;
                    }
                    continue;
                }
                if (pairsMemoryAccesses && inst.args()[0].isAddr() && inst.args()[1].isGPTmp() && tryLoadPair(code, block, index, inst)) {
                    block->at(index) = Inst();
                    removedAny = true;
                    changed = true;
                    continue;
                }
                break;
            }

            default:
                continue;
            }
        }
        if (removedAny) {
            block->insts().removeAllMatching([](const Inst& inst) {
                return !inst;
            });
        }
    }

    return changed;
}

} } } // namespace JSC::B3::Air

#endif // CPU(ARM64)
#endif // ENABLE(B3_JIT)
