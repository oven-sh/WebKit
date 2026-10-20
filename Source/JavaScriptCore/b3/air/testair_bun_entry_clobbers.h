/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_ENTRY_CLOBBER_TESTS() do { \
        RUN(testBlockEntryClobbers()); \
        RUN(testSplitAroundTerminalWithBlockEntryClobbers()); \
    } while (false)

static constexpr unsigned padValueCount = 4;
static constexpr unsigned padFastTmpCount = 6;
static constexpr int32_t padIterationCount = 5;
static constexpr int32_t padAddend = 1000;

static void emitTrashedRegisters(CCallHelpers& jit, const RegisterSet& registers)
{
    registers.forEach([&](Reg reg) {
        jit.move(CCallHelpers::TrustedImm32(-1), reg.gpr());
    });
}

static B3::PatchpointValue* addBranchToPad(B3::Procedure& proc, const RegisterSet& trashedOnWayToPad, const RegisterSet& trashedOnBothWays = { })
{
    B3::PatchpointValue* patchpoint = addPatchpoint(proc);
    patchpoint->effects.terminal = true;
    patchpoint->clobberLate(trashedOnBothWays);
    for (unsigned i = 0; i < 2; ++i)
        patchpoint->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::SomeRegister);
    patchpoint->setGenerator([=](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        CCallHelpers::Jump toPad = jit.branch64(CCallHelpers::Equal, params[0].gpr(), params[1].gpr());
        emitTrashedRegisters(jit, trashedOnBothWays);
        CCallHelpers::Jump toContinuation;
        if (!params.fallsThroughToSuccessor(0))
            toContinuation = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            toPad.link(&jit);
            emitTrashedRegisters(jit, trashedOnBothWays);
            emitTrashedRegisters(jit, trashedOnWayToPad);
            jit.jump().linkTo(*labels[1], &jit);
            if (toContinuation.isSet())
                toContinuation.linkTo(*labels[0], &jit);
        });
    });
    return patchpoint;
}

static BasicBlock* buildLoopWithPad(B3::Procedure& proc, bool limitsRegisters, const RegisterSet& clobberedAtPad)
{
    proc.setOptLevel(2);
    Code& code = proc.code();
    if (limitsRegisters) {
        Vector<Reg> registers = code.regsInPriorityOrder(GP);
        for (size_t i = padFastTmpCount + 2; i < registers.size(); ++i)
            code.pinRegister(registers[i]);
    }
    BasicBlock* root = code.addBlock();
    BasicBlock* header = code.addBlock(10);
    BasicBlock* body = code.addBlock(10);
    BasicBlock* done = code.addBlock();
    BasicBlock* pad = code.addBlock();
    Tmp base = code.newTmp(GP);
    Tmp stopAt = code.newTmp(GP);
    Tmp counter = code.newTmp(GP);
    Tmp sum = code.newTmp(GP);
    Vector<Tmp> values;
    Vector<Tmp> fastTmps;

    root->append(Move, nullptr, argumentTmp(0), base);
    root->append(Move, nullptr, argumentTmp(1), stopAt);
    root->append(Move, nullptr, Arg::imm(0), sum);
    for (unsigned i = 0; i < padValueCount + padFastTmpCount; ++i) {
        Vector<Tmp>& tmps = i < padValueCount ? values : fastTmps;
        tmps.append(code.newTmp(GP));
        root->append(Move, nullptr, Arg::addr(base, static_cast<int32_t>(i * sizeof(int64_t))), tmps.last());
    }
    for (Tmp fastTmp : fastTmps) {
        code.addFastTmp(fastTmp);
        root->append(Add64, nullptr, fastTmp, sum);
    }
    root->append(Move, nullptr, Arg::imm(padIterationCount), counter);
    appendJump(root, header);

    header->append(Branch32, nullptr, Arg::relCond(MacroAssembler::GreaterThan), counter, Arg::imm(0));
    header->setSuccessors(body, done);

    for (Tmp value : values) {
        body->append(Add64, nullptr, value, sum);
        body->append(Add64, nullptr, counter, value);
    }
    body->append(Sub32, nullptr, Arg::imm(1), counter);
    body->append(Patch, addBranchToPad(proc, clobberedAtPad), Arg::special(patchpointSpecialOf(code)), counter, stopAt);
    body->setSuccessors(header, pad);

    if (!clobberedAtPad.isEmpty())
        pad->append(Patch, nullptr, Arg::special(code.addSpecial(makeUnique<BlockEntryClobberSpecial>(clobberedAtPad))));
    pad->append(Add64, nullptr, Arg::imm(padAddend), sum);
    for (BasicBlock* exit : { done, pad }) {
        for (Tmp value : values)
            exit->append(Add64, nullptr, value, sum);
        exit->append(Move, nullptr, sum, returnTmp());
        appendReturn(exit);
    }
    return pad;
}

static int64_t evaluateLoopWithPad(const int64_t* memory, int64_t stopAt)
{
    int64_t sum = 0;
    std::array<int64_t, padValueCount> values;
    for (unsigned i = 0; i < padValueCount; ++i)
        values[i] = memory[i];
    for (unsigned i = 0; i < padFastTmpCount; ++i)
        sum += memory[padValueCount + i];
    for (int64_t counter = padIterationCount; counter > 0;) {
        for (int64_t& value : values) {
            sum += value;
            value += counter;
        }
        if (--counter == stopAt) {
            sum += padAddend;
            break;
        }
    }
    for (int64_t value : values)
        sum += value;
    return sum;
}

static void checkLoopWithPad(const B3::Policy& policy, bool limitsRegisters)
{
    int64_t memory[padValueCount + padFastTmpCount];
    for (unsigned i = 0; i < std::size(memory); ++i)
        memory[i] = (i + 1) * 1000003ll;

    B3::Procedure proc(policy);
    BasicBlock* pad = buildLoopWithPad(proc, limitsRegisters, callClobberedRegisters());
    prepareForGeneration(proc.code());
    CHECK(RegLiveness(proc.code()).liveAtHead(pad).filter(callClobberedRegisters()).isEmpty());

    GeneratedCode generated = generateCode(proc);
    for (int64_t stopAt : { -1, 3, 0 })
        CHECK(invoke<int64_t>(*generated.compilation, memory, stopAt) == evaluateLoopWithPad(memory, stopAt));
}

void testBlockEntryClobbers()
{
    B3::Policy policy;
    checkLoopWithPad(policy, false);
    policy.air.splitting.splitsAroundLoops = true;
    checkLoopWithPad(policy, true);
    for (bool hasEntryClobbers : { false, true }) {
        B3::Procedure proc(policy);
        buildLoopWithPad(proc, true, hasEntryClobbers ? callClobberedRegisters() : RegisterSet());
        proc.code().resetReachability();
        allocateRegistersByGreedy(proc.code());
        CHECK(!numberOfInsts(proc.code(), Shuffle) == hasEntryClobbers);
    }
    enableAllAllocatorFields(policy.air);
    checkLoopWithPad(policy, false);
    checkLoopWithPad(policy, true);
}

static constexpr unsigned valuesAcrossTerminalCount = 4;
static constexpr unsigned usesBeforeTerminalCount = 3;

static BasicBlock* buildClobberingTerminalWithPad(B3::Procedure& proc, bool padReadsValues)
{
    proc.setOptLevel(2);
    Code& code = proc.code();
    RegisterSet clobberedAtPad;
    RegisterSet clobberedByTerminal;
    for (Reg reg : code.regsInPriorityOrder(GP))
        (clobberedAtPad.numberOfSetGPRs() < 2 ? clobberedAtPad : clobberedByTerminal).add(reg, IgnoreVectors);

    BasicBlock* root = code.addBlock(10);
    BasicBlock* site = code.addBlock();
    BasicBlock* continuation = code.addBlock();
    BasicBlock* pad = code.addBlock();
    Tmp base = code.newTmp(GP);
    Tmp selector = code.newTmp(GP);
    Tmp padSelector = code.newTmp(GP);
    Tmp sum = code.newTmp(GP);
    Vector<Tmp> values;

    root->append(Move, nullptr, argumentTmp(0), base);
    root->append(Move, nullptr, argumentTmp(1), selector);
    root->append(Move, nullptr, Arg::imm(1), padSelector);
    root->append(Move, nullptr, Arg::imm(0), sum);
    for (unsigned i = 0; i < valuesAcrossTerminalCount; ++i) {
        values.append(code.newTmp(GP));
        root->append(Move, nullptr, Arg::addr(base, static_cast<int32_t>(i * sizeof(int64_t))), values[i]);
    }
    for (unsigned i = 0; i < usesBeforeTerminalCount; ++i) {
        for (Tmp value : values)
            root->append(Add64, nullptr, value, sum);
    }
    appendJump(root, site);

    site->append(Patch, addBranchToPad(proc, clobberedAtPad, clobberedByTerminal), Arg::special(patchpointSpecialOf(code)), selector, padSelector);
    site->setSuccessors(continuation, pad);

    pad->append(Patch, nullptr, Arg::special(code.addSpecial(makeUnique<BlockEntryClobberSpecial>(clobberedAtPad))));
    if (padReadsValues)
        pad->append(Add64, nullptr, Arg::imm(padAddend), sum);
    else
        appendReturn(pad, padAddend);
    for (BasicBlock* exit : { continuation, pad }) {
        if (exit == pad && !padReadsValues)
            continue;
        for (Tmp value : values)
            exit->append(Add64, nullptr, value, sum);
        exit->append(Move, nullptr, sum, returnTmp());
        appendReturn(exit);
    }
    return root;
}

static bool namesStack(BasicBlock* block)
{
    for (Inst& inst : *block) {
        for (Arg& arg : inst.args()) {
            if (arg.isStack())
                return true;
        }
    }
    return false;
}

void testSplitAroundTerminalWithBlockEntryClobbers()
{
    int64_t memory[valuesAcrossTerminalCount];
    int64_t sumOfValues = 0;
    for (unsigned i = 0; i < valuesAcrossTerminalCount; ++i) {
        memory[i] = (i + 1) * 1000003ll;
        sumOfValues += memory[i];
    }
    int64_t sumAtExit = (usesBeforeTerminalCount + 1) * sumOfValues;

    B3::Policy policy;
    for (bool enablesAllAllocatorFields : { false, true }) {
        if (enablesAllAllocatorFields)
            enableAllAllocatorFields(policy.air);
        for (bool padReadsValues : { false, true }) {
            B3::Procedure allocated(policy);
            BasicBlock* root = buildClobberingTerminalWithPad(allocated, padReadsValues);
            allocated.code().resetReachability();
            allocateRegistersByGreedy(allocated.code());
            CHECK(padReadsValues || !namesStack(root));

            B3::Procedure proc(policy);
            buildClobberingTerminalWithPad(proc, padReadsValues);
            prepareForGeneration(proc.code());
            GeneratedCode generated = generateCode(proc);
            CHECK(invoke<int64_t>(*generated.compilation, memory, 0) == sumAtExit);
            CHECK(invoke<int64_t>(*generated.compilation, memory, 1) == (padReadsValues ? sumAtExit + padAddend : padAddend));
        }
    }
}
