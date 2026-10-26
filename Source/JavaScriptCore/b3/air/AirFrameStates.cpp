/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirFrameStates.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "CallFrame.h"
#include "GPRInfo.h"

namespace JSC { namespace B3 { namespace Air {

static_assert(Reg::maxIndex() < 64);

static uint64_t bitOf(Reg reg)
{
    return static_cast<uint64_t>(1) << reg.index();
}

static uint64_t bitsOf(const RegisterSet& registers)
{
    uint64_t result = 0;
    registers.forEach([&](Reg reg) {
        result |= bitOf(reg);
    });
    return result;
}

static FrameState stateAfterInst(const Inst& inst, FrameState state)
{
    if (inst.kind.opcode == EnterFrame)
        return FrameState::SetUp;
    if (inst.kind.opcode == LeaveFrame)
        return FrameState::NotSetUp;
    return state;
}

static void recordFirstViolation(std::optional<FrameStateViolation>& violation, FrameStateViolation::Kind kind, BasicBlock* block, unsigned instIndex)
{
    if (!violation)
        violation = FrameStateViolation { kind, block, instIndex };
}

uint64_t savedCalleeSaveBits(const Code& code)
{
    uint64_t result = 0;
    RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
    for (const RegisterAtOffset& entry : saves)
        result |= bitOf(entry.reg());
    return result;
}

bool isIncomingArgumentAddress(const Arg& arg)
{
    return arg.isAddr()
        && arg.base() == Tmp(GPRInfo::callFrameRegister)
        && arg.offset() >= static_cast<Value::OffsetType>(sizeof(CallerFrameAndPC));
}

FrameNeeds frameNeedsOf(Inst& inst, uint64_t savedCalleeSaves)
{
    FrameNeeds result;
    if (inst.kind.opcode == EnterFrame || inst.kind.opcode == LeaveFrame)
        return result;

    result.needsFrame = inst.needsFrame();
    bool isPatch = inst.kind.opcode == Patch;
    for (Arg& arg : inst.args()) {
        if (arg.isStack() || arg.isCallArg()) {
            result.needsFrame = true;
            continue;
        }
        if (!isPatch && isIncomingArgumentAddress(arg)) {
            result.addressesIncomingArguments = true;
            continue;
        }
        arg.forEachTmpFast([&](Tmp tmp) {
            if (!tmp.isReg())
                return;
            Reg reg = tmp.reg();
            if (reg == Reg(GPRInfo::callFrameRegister) || (savedCalleeSaves & bitOf(reg)))
                result.needsFrame = true;
            else if (reg == Reg(MacroAssembler::stackPointerRegister))
                result.namesStackPointer = true;
        });
    }

    if (isPatch && !result.needsFrame && savedCalleeSaves)
        result.needsFrame = !!((bitsOf(inst.extraEarlyClobberedRegs()) | bitsOf(inst.extraClobberedRegs())) & savedCalleeSaves);
    return result;
}

FrameStates::FrameStates(Code& code)
    : m_atHead(code.size(), FrameState::Unreachable)
    , m_atTail(code.size(), FrameState::Unreachable)
{
    ASSERT(code.policy().frame.emitsFrame);

    Vector<BasicBlock*, 16> worklist;
    auto addRoot = [&](BasicBlock* block) {
        if (m_atHead[block] != FrameState::Unreachable)
            return;
        m_atHead[block] = FrameState::NotSetUp;
        worklist.append(block);
    };
    if (code.numEntrypoints()) {
        for (const FrequentedBlock& entrypoint : code.entrypoints())
            addRoot(entrypoint.block());
    } else
        addRoot(code[0]);

    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        FrameState tail = stateAfter(block, m_atHead[block]);
        m_atTail[block] = tail;
        for (BasicBlock* successor : block->successorBlocks()) {
            FrameState& head = m_atHead[successor];
            if (head == FrameState::Unreachable) {
                head = tail;
                worklist.append(successor);
            } else if (head != tail)
                recordFirstViolation(m_violation, FrameStateViolation::Kind::StatesDifferAtMerge, successor, 0);
        }
    }
}

FrameState FrameStates::stateAfter(BasicBlock* block, FrameState state)
{
    for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
        const Inst& inst = block->at(instIndex);
        if (inst.kind.opcode == EnterFrame && state == FrameState::SetUp)
            recordFirstViolation(m_violation, FrameStateViolation::Kind::FrameSetUpTwice, block, instIndex);
        else if (inst.kind.opcode == LeaveFrame && state == FrameState::NotSetUp)
            recordFirstViolation(m_violation, FrameStateViolation::Kind::FrameTornDownTwice, block, instIndex);
        else if (isReturn(inst.kind.opcode) && state == FrameState::SetUp)
            recordFirstViolation(m_violation, FrameStateViolation::Kind::ReturnsWithFrame, block, instIndex);
        state = stateAfterInst(inst, state);
    }
    return state;
}

std::optional<FrameStateViolation> findFrameStateViolation(Code& code)
{
    FrameStates states(code);
    if (states.violation())
        return states.violation();

    uint64_t savedCalleeSaves = savedCalleeSaveBits(code);
    for (BasicBlock* block : code) {
        FrameState state = states.atHead(block);
        if (state == FrameState::Unreachable)
            continue;
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);
            if (state == FrameState::NotSetUp) {
                FrameNeeds needs = frameNeedsOf(inst, savedCalleeSaves);
                if (needs.needsFrame || needs.addressesIncomingArguments)
                    return FrameStateViolation { FrameStateViolation::Kind::NeedsFrame, block, instIndex };
            }
            state = stateAfterInst(inst, state);
        }
    }
    return std::nullopt;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
