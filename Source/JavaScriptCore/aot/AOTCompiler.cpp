/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiler.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTEmitter.h"
#include "AOTProgram.h"
#include "AOTStubs.h"
#include "AOTTypeTable.h"
#include "AirCode.h"
#include "AirGenerate.h"
#include "AirInstInlines.h"
#include "B3BasicBlockInlines.h"
#include "B3Generate.h"
#include "B3NaturalLoops.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include "CodeBlock.h"
#include "JSCInlines.h"
#include "JSLexicalEnvironment.h"
#include "JSModuleEnvironment.h"
#include "JSWithScope.h"
#include "LinkBuffer.h"
#include "MinimumReservedZoneSize.h"
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/Lock.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringHash.h>

namespace JSC { namespace AOT {

#if CPU(ARM64) || CPU(X86_64)

// The ways out of a block that are not rare are as likely as one another, except that a loop goes around ten times before it is left.
void estimateFrequencies(B3::Procedure& proc)
{
    constexpr double coldFrequency = 1.0 / 1024;
    proc.resetReachability();
    auto& loops = proc.naturalLoops();
    constexpr double iterations = 10;
    constexpr double leastFrequency = 1e-6;
    constexpr double mostFrequency = 1e6;

    // What a block starts with is its weight among the successors of a branch: see Typed::Lowering::newUnlikelyBlock().
    IndexMap<B3::BasicBlock*, double> weight(proc.size(), 1.0);
    for (B3::BasicBlock* block : proc)
        weight[block] = std::clamp(block->frequency(), coldFrequency, 1.0);

    IndexMap<B3::BasicBlock*, double> incoming(proc.size(), 0.0);
    incoming[proc[0]] = 1;
    Vector<B3::BasicBlock*> postOrder = proc.blocksInPostOrder();
    for (unsigned index = postOrder.size(); index--;) {
        B3::BasicBlock* block = postOrder[index];
        double frequency = incoming[block];
        if (loops.headerOf(block))
            frequency *= iterations;
        block->setFrequency(static_cast<float>(std::clamp(frequency, leastFrequency, mostFrequency)));

        if (block->last()->opcode() == B3::EntrySwitch) {
            for (unsigned i = 0; i < block->numSuccessors(); ++i)
                incoming[block->successorBlock(i)] += i ? frequency * coldFrequency : frequency;
            continue;
        }

        auto shareIfLeavingLoops = [&](B3::BasicBlock* successor) {
            double share = 1;
            for (auto* loop = loops.innerMostLoopOf(block); loop && !loops.belongsTo(successor, *loop); loop = loops.innerMostOuterLoop(*loop))
                share /= iterations;
            return share;
        };
        double weightStaying = 0;
        double leaving = 0;
        // What goes the rare ways does not go the others: where the ways meet again no more arrives than has set out.
        double rare = 0;
        for (auto& successor : block->successors()) {
            if (successor.frequency() == B3::FrequencyClass::Rare) {
                rare += shareIfLeavingLoops(successor.block()) * coldFrequency;
                continue;
            }
            if (double share = shareIfLeavingLoops(successor.block()); share < 1)
                leaving += share;
            else
                weightStaying += weight[successor.block()];
        }
        double scaleOfLeaving = leaving > 0.5 ? 0.5 / leaving : 1;
        // A switch may have a thousand rare cases.
        rare = std::min(rare, 0.5);
        for (auto& successor : block->successors()) {
            double share = shareIfLeavingLoops(successor.block());
            if (successor.frequency() == B3::FrequencyClass::Rare)
                share *= coldFrequency;
            else if (share < 1)
                share *= scaleOfLeaving * (1 - rare);
            else
                share = (1 - rare) * (1 - leaving * scaleOfLeaving) * weight[successor.block()] / weightStaying;
            incoming[successor.block()] += frequency * share;
        }
    }
}

// After the frame pointer is set. Leaves the stack pointer at the bottom of the frame. The test falls through: where the frame does not fit it goes to what emitStackOverflow() puts behind the code.
CCallHelpers::JumpList emitStackCheck(CCallHelpers& jit, unsigned frameSize)
{
    // Below the limit lies the reserved zone, so for a frame much smaller than that the stack pointer may move before the test.
    constexpr unsigned maxFrameSizeToMoveFirst = 4 * KB;
    static_assert(maxFrameSizeToMoveFirst < minimumReservedZoneSize);
    CCallHelpers::Address limit(instanceGPR, Instance::offsetOfSoftStackLimit());
    if (frameSize <= maxFrameSizeToMoveFirst) {
        if (frameSize)
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(frameSize), CCallHelpers::stackPointerRegister);
        return jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, limit);
    }
#if CPU(X86_64)
    constexpr GPRReg newStackPointer = CCallHelpers::s_scratchRegister;
#else
    constexpr GPRReg newStackPointer = stubTemporaryGPRs[3];
#endif
    jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(frameSize), newStackPointer);
    CCallHelpers::JumpList doesNotFit;
    doesNotFit.append(jit.branchPtr(CCallHelpers::Above, newStackPointer, GPRInfo::callFrameRegister));
    doesNotFit.append(jit.branchPtr(CCallHelpers::Below, newStackPointer, limit));
    jit.move(newStackPointer, CCallHelpers::stackPointerRegister);
    return doesNotFit;
}

void emitStackOverflow(CCallHelpers& jit, StubCalls& stubCalls, const CCallHelpers::JumpList& doesNotFit)
{
    if (doesNotFit.empty())
        return;
    doesNotFit.link(&jit);
    stubCalls.tailCall(jit, Stub::ThrowStackOverflowAtPrologue);
}

// What branch compaction saved is left behind the code as nops: see LinkBuffer::copyCompactAndLinkCode(). A breakpoint at the end that no call comes back to is not reached.
void dropPaddingAtEnd(CompiledFunctionInfo& info, const void* start, B3::Air::Code& code)
{
#if CPU(ARM64)
    constexpr uint32_t breakpoint = 0xd4200000;
    constexpr uint32_t nop = 0xd503201f;
    while (info.codeSize > sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == nop)
        info.codeSize -= sizeof(uint32_t);
    unsigned minimumSize = sizeof(uint32_t);
    for (auto& call : info.stubCalls) {
        if (!call.isTailCall)
            minimumSize = std::max<unsigned>(minimumSize, call.offset + 2 * sizeof(uint32_t));
    }
    auto dropBreakpointAtEnd = [&] {
        if (info.codeSize > minimumSize && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == breakpoint)
            info.codeSize -= sizeof(uint32_t);
    };
    dropBreakpointAtEnd();
    B3::Air::BasicBlock* lastBlock = nullptr;
    for (B3::Air::BasicBlock* block : code)
        lastBlock = block;
    if (lastBlock->size() >= 2 && lastBlock->last().kind.opcode == B3::Air::Oops && lastBlock->at(lastBlock->size() - 2).kind.opcode == B3::Air::Patch)
        dropBreakpointAtEnd();
#else
    UNUSED_PARAM(info);
    UNUSED_PARAM(start);
    UNUSED_PARAM(code);
#endif
}

void usePinnedRegistersDirectly(B3::Air::Code& code)
{
    using namespace B3::Air;
    Vector<std::pair<Tmp, Tmp>, 4> copies;
    for (Inst& inst : *code[0]) {
        if (inst.kind.opcode != Move || inst.args().size() != 2 || !inst.args()[0].isTmp() || !inst.args()[1].isTmp())
            continue;
        Tmp from = inst.args()[0].tmp();
        Tmp to = inst.args()[1].tmp();
        if (!from.isReg() || !code.isPinned(from.reg()) || from.reg() == Reg(MacroAssembler::framePointerRegister) || to.isReg())
            continue;
        copies.append({ to, from });
        inst = Inst();
    }
    if (copies.isEmpty())
        return;
    code[0]->insts().removeAllMatching([](const Inst& inst) { return !inst; });
    for (B3::Air::BasicBlock* block : code) {
        for (Inst& inst : *block) {
            inst.forEachTmpFast([&](Tmp& tmp) {
                for (auto& [copy, reg] : copies) {
                    if (tmp == copy)
                        tmp = reg;
                }
            });
        }
    }
}

#endif // CPU(ARM64) || CPU(X86_64)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
