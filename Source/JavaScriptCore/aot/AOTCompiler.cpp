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
#include "AirCode.h"
#include "AirGenerate.h"
#include "AirInstInlines.h"
#include "AssemblyHelpersSpoolers.h"
#include "B3BasicBlockInlines.h"
#include "B3CycleForest.h"
#include "B3Generate.h"
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

void estimateFrequencies(B3::Procedure& proc, bool returnsAreUnlikely)
{
    constexpr double coldFrequency = 1.0 / 1024;
    proc.resetReachability();
    proc.setReachabilityIsFresh();
    B3::CycleForest cycles(proc);
    constexpr double iterations = 10;
    constexpr double minFrequency = 1e-6;
    constexpr double maxFrequency = 1e6;

    IndexMap<B3::BasicBlock*, double> weight(proc.size(), 1.0);
    constexpr double returningBlockWeight = 0.25;
    for (B3::BasicBlock* block : proc)
        weight[block] = std::clamp(block->frequency(), coldFrequency, returnsAreUnlikely && block->last()->opcode() == B3::Return ? returningBlockWeight : 1.0);

    IndexMap<B3::BasicBlock*, double> incoming(proc.size(), 0.0);
    Vector<double, 16> entering(FillWith { }, cycles.size(), 0.0);
    incoming[proc[0]] = 1;
    for (unsigned cycle = cycles.innermost(proc[0]); cycle != B3::CycleForest::none; cycle = cycles[cycle].parent)
        entering[cycle] = 1;
    auto& postOrder = cycles.blocksInPostOrder();
    for (unsigned index = postOrder.size(); index--;) {
        B3::BasicBlock* block = postOrder[index];
        unsigned blockCycle = cycles.innermost(block);
        double frequency = cycles.isHeader(block) ? entering[blockCycle] * iterations : incoming[block];
        block->setFrequency(static_cast<float>(std::clamp(frequency, minFrequency, maxFrequency)));

        auto send = [&](B3::BasicBlock* successor, double amount) {
            unsigned cycle = cycles.innermost(successor);
            unsigned common = cycles.nearestCommon(blockCycle, cycle);
            if (cycle == common) {
                incoming[successor] += amount;
                return;
            }
            for (; cycle != common; cycle = cycles[cycle].parent)
                entering[cycle] += amount;
        };

        if (block->last()->opcode() == B3::EntrySwitch) {
            for (unsigned i = 0; i < block->numSuccessors(); ++i)
                send(block->successorBlock(i), i ? frequency * coldFrequency : frequency);
            continue;
        }

        auto shareWhenLeavingLoops = [&](B3::BasicBlock* successor) {
            double share = 1;
            for (unsigned left = cycles.depth(blockCycle) - cycles.depth(cycles.nearestCommon(blockCycle, cycles.innermost(successor))); left--;)
                share /= iterations;
            return share;
        };
        double weightStaying = 0;
        double leaving = 0;
        double rare = 0;
        for (auto& successor : block->successors()) {
            if (successor.frequency() == B3::FrequencyClass::Rare) {
                rare += shareWhenLeavingLoops(successor.block()) * coldFrequency;
                continue;
            }
            if (double share = shareWhenLeavingLoops(successor.block()); share < 1)
                leaving += share;
            else
                weightStaying += weight[successor.block()];
        }
        double leavingScale = leaving > 0.5 ? 0.5 / leaving : 1;
        rare = std::min(rare, 0.5);
        for (auto& successor : block->successors()) {
            double share = shareWhenLeavingLoops(successor.block());
            if (successor.frequency() == B3::FrequencyClass::Rare)
                share *= coldFrequency;
            else if (share < 1)
                share *= leavingScale * (1 - rare);
            else
                share = (1 - rare) * (1 - leaving * leavingScale) * weight[successor.block()] / weightStaying;
            send(successor.block(), frequency * share);
        }
    }
}

static constexpr unsigned maxFrameSizeToMoveFirst = 4 * KB;

CCallHelpers::JumpList emitStackCheck(CCallHelpers& jit, unsigned frameSize)
{
    static_assert(maxFrameSizeToMoveFirst + maxStackUsedWithoutCheck < minimumReservedZoneSize);
    CCallHelpers::Address limit(instanceGPR, Instance::offsetOfSoftStackLimit());
    if (frameSize <= maxFrameSizeToMoveFirst) {
        if (frameSize)
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(frameSize), CCallHelpers::stackPointerRegister);
        return jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, CCallHelpers::Address(instanceGPR, Instance::offsetOfTrapAwareSoftStackLimit()));
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

#if CPU(ARM64)
static constexpr uint64_t collapsibleFrameReturnCode = 0x910003bfaa1103f1ull;
#else
static constexpr uint64_t collapsibleFrameReturnCode = 0xc3c90000441f0f66ull;
#endif

void emitCollapsibleFrameReturn(CCallHelpers& jit)
{
#if CPU(ARM64)
    jit.m_assembler.mov<64>(ARM64Registers::x17, ARM64Registers::x17);
    jit.emitFunctionEpilogue();
    jit.ret();
#else
    for (unsigned shift = 0; shift < 64; shift += 8)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(collapsibleFrameReturnCode >> shift));
#endif
}

CCallHelpers::Jump branchIfReturnAddressIsNotInCollapsibleFrame(CCallHelpers& jit, GPRReg returnAddress)
{
    return jit.branch64(CCallHelpers::NotEqual, CCallHelpers::Address(returnAddress), CCallHelpers::TrustedImm64(static_cast<int64_t>(collapsibleFrameReturnCode)));
}

void emitDroppableFrameExit(CCallHelpers& jit, const ScopedLambda<void()>& emitFrameTeardown)
{
    auto& buffer = jit.m_assembler.buffer();
    size_t returnAddress = buffer.codeSize();
#if CPU(ARM64)
    CCallHelpers::Jump resume = jit.jump();
    RELEASE_ASSERT(buffer.codeSize() - returnAddress == droppableFrameSignatureOffset);
    jit.m_assembler.mov<64>(ARM64Registers::lr, ARM64Registers::lr);
    RELEASE_ASSERT(buffer.codeSize() - returnAddress == droppableFrameExitOffset);
    jit.m_assembler.mov<64>(ARM64Registers::x17, ARM64Registers::lr);
    emitFrameTeardown();
    jit.m_assembler.br(ARM64Registers::x17);
    resume.link(&jit);
#else
    buffer.putByte(static_cast<int8_t>(0xeb));
    buffer.putByte(0);
    for (unsigned shift = 0; shift < 64; shift += 8)
        buffer.putByte(static_cast<int8_t>(droppableFrameSignature >> shift));
    RELEASE_ASSERT(buffer.codeSize() - returnAddress == droppableFrameExitOffset);
    jit.pop(X86Registers::r11);
    emitFrameTeardown();
    jit.m_assembler.jmp_r(X86Registers::r11);
    size_t distance = buffer.codeSize() - returnAddress - droppableFrameSignatureOffset;
    RELEASE_ASSERT(distance <= std::numeric_limits<int8_t>::max());
    static_cast<int8_t*>(buffer.data())[returnAddress + 1] = static_cast<int8_t>(distance);
#endif
}

void dropDroppableCallerFrame(CCallHelpers& jit)
{
#if CPU(ARM64)
    jit.m_assembler.add<64>(ARM64Registers::x16, ARM64Registers::lr, UInt12(droppableFrameExitOffset));
    jit.m_assembler.blr(ARM64Registers::x16);
#else
    jit.pop(X86Registers::r11);
    jit.add64(CCallHelpers::TrustedImm32(droppableFrameExitOffset), X86Registers::r11);
    jit.m_assembler.call(X86Registers::r11);
#endif
}

void dropCollapsibleCallerFrames(CCallHelpers& jit, GPRReg scratch)
{
    CCallHelpers::Label again = jit.label();
#if CPU(ARM64)
    jit.move(CCallHelpers::linkRegister, scratch);
#else
    jit.loadPtr(CCallHelpers::Address(CCallHelpers::stackPointerRegister), scratch);
#endif
    CCallHelpers::Jump isNot = branchIfReturnAddressIsNotInCollapsibleFrame(jit, scratch);
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(again, &jit);
    isNot.link(&jit);
}

#if CPU(ARM64)
static unsigned savesMovedWithStackPointerCount(const RegisterAtOffsetList& list)
{
    return list.registerCount() > 1 && list.at(0).reg().isGPR() == list.at(1).reg().isGPR() ? 2 : 1;
}

void pushCalleeSaves(CCallHelpers& jit, const RegisterAtOffsetList& list)
{
    size_t count = list.registerCount();
    if (!count)
        return;
    constexpr auto sp = CCallHelpers::stackPointerRegister;
    int size = -static_cast<int>(list.at(0).offset());
    RELEASE_ASSERT(size > 0 && !(size % stackAlignmentBytes()));
    Reg first = list.at(0).reg();
    size_t i = savesMovedWithStackPointerCount(list);
    if (i == 2 && first.isGPR())
        jit.m_assembler.stp<64>(first.gpr(), list.at(1).reg().gpr(), sp, PairPreIndex(-size));
    else if (i == 2)
        jit.m_assembler.stp<64>(first.fpr(), list.at(1).reg().fpr(), sp, PairPreIndex(-size));
    else if (first.isGPR())
        jit.m_assembler.str<64>(first.gpr(), sp, PreIndex(-size));
    else
        jit.m_assembler.str<64>(first.fpr(), sp, PreIndex(-size));
    AssemblyHelpers::StoreRegSpooler spooler(jit, GPRInfo::callFrameRegister);
    for (; i < count && list.at(i).reg().isGPR(); ++i)
        spooler.storeGPR(list.at(i));
    spooler.finalizeGPR();
    for (; i < count; ++i)
        spooler.storeFPR(list.at(i));
    spooler.finalizeFPR();
}

void popCalleeSaves(CCallHelpers& jit, const RegisterAtOffsetList& list)
{
    size_t count = list.registerCount();
    if (!count)
        return;
    constexpr auto sp = CCallHelpers::stackPointerRegister;
    int size = -static_cast<int>(list.at(0).offset());
    Reg first = list.at(0).reg();
    size_t moved = savesMovedWithStackPointerCount(list);
    size_t i = moved;
    AssemblyHelpers::LoadRegSpooler spooler(jit, GPRInfo::callFrameRegister);
    for (; i < count && list.at(i).reg().isGPR(); ++i)
        spooler.loadGPR(list.at(i));
    spooler.finalizeGPR();
    for (; i < count; ++i)
        spooler.loadFPR(list.at(i));
    spooler.finalizeFPR();
    if (moved == 2 && first.isGPR())
        jit.m_assembler.ldp<64>(first.gpr(), list.at(1).reg().gpr(), sp, PairPostIndex(size));
    else if (moved == 2)
        jit.m_assembler.ldp<64>(first.fpr(), list.at(1).reg().fpr(), sp, PairPostIndex(size));
    else if (first.isGPR())
        jit.m_assembler.ldr<64>(first.gpr(), sp, PostIndex(size));
    else
        jit.m_assembler.ldr<64>(first.fpr(), sp, PostIndex(size));
}
#endif

void emitStackOverflow(CCallHelpers& jit, StubCalls& stubCalls, const CCallHelpers::JumpList& doesNotFit, unsigned frameSize)
{
    if (doesNotFit.empty())
        return;
    doesNotFit.link(&jit);
    if (frameSize <= maxFrameSizeToMoveFirst) {
#if CPU(X86_64) && OS(LINUX)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(trapInstruction));
#else
        stubCalls.call(jit, Stub::PollAtPrologue, CallSite { });
#endif
    } else
        stubCalls.tailCall(jit, Stub::ThrowStackOverflowAtPrologue);
}

void dropPaddingAtEnd(CompiledFunctionInfo& info, const void* start, B3::Air::Code& code)
{
#if CPU(ARM64)
    constexpr uint32_t breakpoint = 0xd4200000;
    constexpr uint32_t nop = 0xd503201f;
    while (info.codeSize > sizeof(uint32_t) && *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(start) + info.codeSize - sizeof(uint32_t)) == nop)
        info.codeSize -= sizeof(uint32_t);
    unsigned minimumSize = sizeof(uint32_t);
    for (auto& call : info.stubCalls) {
        if (!call.isTailCall && call.stub != Stub::PollAtPrologue)
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
    UNUSED_PARAM(code);
    constexpr uint8_t breakpoint = 0xcc;
    auto* bytes = static_cast<const uint8_t*>(start);
    for (unsigned filling = 0; filling < sizeof(uint32_t) && filling < info.codeSize; ++filling) {
        std::array<uint8_t, sizeof(uint32_t)> nops;
        X86Assembler::fillNops(nops.data(), filling);
        if (bytes[info.codeSize - filling - 1] != breakpoint || memcmp(bytes + info.codeSize - filling, nops.data(), filling))
            continue;
        info.codeSize -= filling;
        break;
    }
    unsigned minimumSize = 1;
    for (auto& call : info.stubCalls) {
        if (!call.isTailCall && call.stub != Stub::PollAtPrologue)
            minimumSize = std::max<unsigned>(minimumSize, call.offset + sizeOfNearCall + 1);
    }
    if (info.codeSize > minimumSize && bytes[info.codeSize - 1] == breakpoint)
        --info.codeSize;
#endif
}

#endif // CPU(ARM64) || CPU(X86_64)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
