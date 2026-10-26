/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3HeapSet.h"

namespace {

struct ObservablePatchpointShape {
    int32_t number { 0 };
    Vector<bool, 3> isInt32;
    RegisterSet lateClobbers;
    RegisterSet rareSuccessorClobbers;
    bool isCallShaped { false };
    bool isTerminal { false };
    RandomProgramState* state { nullptr };
};

} // anonymous namespace

static constexpr int64_t clobberedRegisterBits = 0x0bad0bad0bad0badll;

static Vector<bool, 3> appendColdOperands(StackmapValue* stackmap, const Vector<Value*, 3>& operands, bool inArgumentRegisters = false)
{
    Vector<bool, 3> isInt32;
    unsigned nextGPR = 1;
    unsigned nextFPR = 0;
    for (Value* operand : operands) {
        ValueRep rep = ValueRep::ColdAny;
        if (inArgumentRegisters && operand->type() == Double)
            rep = ValueRep::reg(FPRInfo::toArgumentRegister(nextFPR++));
        else if (inArgumentRegisters)
            rep = ValueRep::reg(GPRInfo::toArgumentRegister(nextGPR++));
        stackmap->append(ConstrainedValue(operand, rep));
        isInt32.append(operand->type() == Int32);
    }
    return isInt32;
}

static void emitClobbers(CCallHelpers& jit, const RegisterSet& registers, GPRReg clobberSource)
{
    registers.forEach([&](Reg reg) {
        if (reg.isFPR())
            jit.move64ToDouble(clobberSource, reg.fpr());
        else
            jit.move(clobberSource, reg.gpr());
    });
}

static void emitJumpsToSuccessors(CCallHelpers& jit, const StackmapGenerationParams& params, const RegisterSet& rareSuccessorClobbers, GPRReg operand, GPRReg clobberSource)
{
    CCallHelpers::Jump toRareSuccessor = jit.branchTest64(CCallHelpers::NonZero, operand, CCallHelpers::TrustedImm32(1));
    CCallHelpers::Jump toLikelySuccessor;
    if (!params.fallsThroughToSuccessor(0))
        toLikelySuccessor = jit.jump();
    Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
    params.addLatePath([=](CCallHelpers& jit) {
        if (rareSuccessorClobbers.isEmpty())
            toRareSuccessor.linkTo(*labels[1], &jit);
        else {
            toRareSuccessor.link(&jit);
            emitClobbers(jit, rareSuccessorClobbers, clobberSource);
            jit.jump().linkTo(*labels[1], &jit);
        }
        if (toLikelySuccessor.isSet())
            toLikelySuccessor.linkTo(*labels[0], &jit);
    });
}

static void generateObservablePatchpoint(CCallHelpers& jit, const StackmapGenerationParams& params, const ObservablePatchpointShape& shape)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    GPRReg address = shape.isCallShaped ? GPRInfo::argumentGPR4 : params.gpScratch(0);
    GPRReg order = shape.isCallShaped ? GPRInfo::argumentGPR5 : params.gpScratch(1);
    GPRReg twice = shape.isCallShaped ? GPRInfo::nonPreservedNonArgumentGPR0 : params.gpScratch(2);
    jit.move(CCallHelpers::TrustedImmPtr(&shape.state->effectHash), address);
    jit.load64(CCallHelpers::Address(address), order);
    jit.add64(order, order, twice);
    jit.add64(twice, order);
    jit.add64(CCallHelpers::TrustedImm32(shape.number), order);
    jit.add64(params[1].gpr(), order);
    for (unsigned i = 0; i < shape.isInt32.size(); ++i) {
        params[2 + i].emitRestore(jit, twice);
        if (shape.isInt32[i])
            jit.zeroExtend32ToWord(twice, twice);
        jit.add64(order, order);
        jit.add64(twice, order);
    }
    jit.store64(order, CCallHelpers::Address(address));
    jit.move(params[1].gpr(), twice);
    jit.add64(CCallHelpers::TrustedImm32(3), params[1].gpr(), params[0].gpr());
    jit.move(CCallHelpers::TrustedImm64(clobberedRegisterBits), order);
    RegisterSet clobbers = shape.lateClobbers;
    clobbers.remove(params[0].gpr());
    clobbers.remove(twice);
    emitClobbers(jit, clobbers, order);
    if (shape.isTerminal)
        emitJumpsToSuccessors(jit, params, shape.rareSuccessorClobbers, twice, order);
}

static RegisterSet chooseRareSuccessorClobbers(WeakRandom& random)
{
    RegisterSet volatileRegisters = RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters());
    volatileRegisters.exclude(RegisterSet::macroClobberedGPRs());
    switch (random.getUint32(4)) {
    case 0:
    case 1:
        return { };
    case 2:
        return volatileRegisters;
    default: {
        RegisterSet result;
        volatileRegisters.forEach([&](Reg reg) {
            if (random.getUint32(2))
                result.add(reg, IgnoreVectors);
        });
        return result;
    }
    }
}

static HeapSet createTwoRangeSet(Procedure& proc, HeapRange heap)
{
    std::array<HeapRange, 2> ranges { heap, heap == firstHeap ? thirdHeap : pollHeap };
    return proc.heapSets().create(ranges);
}

static Effects createObservableEffects(Procedure& proc, unsigned kind, bool writesTwoRanges)
{
    Effects result = Effects::none();
    switch (kind) {
    case 0:
        result.writesLocalState = true;
        break;
    case 1:
        result.exitsSideways = true;
        result.reads = HeapRange::top();
        break;
    case 2:
        result.reads = firstHeap;
        result.writes = writesTwoRanges ? createTwoRangeSet(proc, firstHeap) : HeapSet(firstHeap);
        break;
    case 3:
        result.fence = true;
        break;
    case 4:
        result.writesPinned = true;
        break;
    case 5:
        result.writesLocalState = true;
        result.readsLocalState = true;
        break;
    default:
        result = Effects::forCall();
        break;
    }
    return result;
}

static PatchpointValue* appendWordIncrement(Procedure& proc, BasicBlock* block, Value* pointer, int32_t offset)
{
    PatchpointValue* result = block->appendNew<PatchpointValue>(proc, Void, Origin());
    result->append(ConstrainedValue(pointer, ValueRep::SomeRegister));
    result->effects = Effects::none();
    result->clobber(RegisterSet::macroClobberedGPRs());
    result->setGenerator([offset](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(params[0].gpr(), offset));
    });
    return result;
}

static void generateFailedCheck(CCallHelpers& jit, const StackmapGenerationParams& params, const Vector<bool, 3>& isInt32, RandomProgramState* state)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    GPRReg operands = InvalidGPRReg;
    for (unsigned i = 0; i < GPRInfo::numberOfArgumentRegisters && operands == InvalidGPRReg; ++i) {
        GPRReg candidate = GPRInfo::toArgumentRegister(i);
        bool isFree = candidate != GPRInfo::returnValueGPR;
        for (unsigned j = 0; j < isInt32.size(); ++j)
            isFree &= !params[j].isGPR() || params[j].gpr() != candidate;
        if (isFree)
            operands = candidate;
    }
    jit.move(CCallHelpers::TrustedImmPtr(state->failedCheckOperands.data()), operands);
    for (bool storesRegisters : { true, false }) {
        for (unsigned i = 0; i < isInt32.size(); ++i) {
            if (params[i].isReg() != storesRegisters)
                continue;
            CCallHelpers::Address slot(operands, static_cast<int32_t>(i * sizeof(int64_t)));
            if (params[i].isFPR()) {
                jit.storeDouble(params[i].fpr(), slot);
                continue;
            }
            GPRReg source = GPRInfo::returnValueGPR;
            if (params[i].isGPR())
                source = params[i].gpr();
            else
                params[i].emitRestore(jit, source);
            if (isInt32[i])
                jit.store32(source, slot);
            else
                jit.store64(source, slot);
        }
    }
    jit.move(CCallHelpers::TrustedImm64(RandomProgram::failedCheckResult), GPRInfo::returnValueGPR);
    params.code().emitEpilogue(jit);
}

auto RandomProgram::coldOperands() -> ColdOperands
{
    ColdOperands result;
    if (!m_options.hasCallShapes)
        return result;
    auto variable = [&] {
        return get(m_variables[m_coldOperandRandom.getUint32(m_variables.size())]);
    };
    for (unsigned count = m_coldOperandRandom.getUint32(4); count--;) {
        Value* value = variable();
        switch (m_coldOperandRandom.getUint32(6)) {
        case 0:
            value = make(Trunc, value);
            break;
        case 1:
            value = make(Add, value, variable());
            break;
        case 2:
            value = make(IToD, value);
            break;
        case 3: {
            uint64_t bits = m_coldOperandRandom.getUint32();
            value = int64(static_cast<int64_t>(bits << m_coldOperandRandom.getUint32(33)));
            break;
        }
        default:
            break;
        }
        result.append(value);
    }
    return result;
}

Value* RandomProgram::observablePatchpoint(Value* operand)
{
    ColdOperands cold = coldOperands();
    bool isCallShaped = m_options.hasCallShapes && m_coldOperandRandom.getUint32(100) < 35;
    bool hasTwoSuccessors = m_options.hasCallShapes && m_coldOperandRandom.getUint32(100) < 15;
    bool isTerminal = hasTwoSuccessors && !m_options.isForLevel0;
    RegisterSet rareSuccessorClobbers;
    if (hasTwoSuccessors)
        rareSuccessorClobbers = chooseRareSuccessorClobbers(m_coldOperandRandom);
    bool isCloningForbidden = m_promiseRandom.getUint32(100) < 20;
    bool writesTwoRanges = m_promiseRandom.getUint32(100) < 50;
    PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin(), isCloningForbidden ? cloningForbidden(Patchpoint) : B3::Kind(Patchpoint));
    result->append(ConstrainedValue(operand, isCallShaped ? ValueRep::reg(GPRInfo::argumentGPR0) : ValueRep::SomeRegister));
    Vector<bool, 3> isInt32 = appendColdOperands(result, cold, isCallShaped);
    RegisterSet lateClobbers;
    if (isCallShaped) {
        result->resultConstraints = { ValueRep::reg(GPRInfo::returnValueGPR) };
        lateClobbers = RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters());
    }
    result->effects = createObservableEffects(m_proc, pick(7), writesTwoRanges);
    for (unsigned i = 0; i < GPRInfo::numberOfArgumentRegisters; ++i) {
        if (chance(50))
            lateClobbers.add(GPRInfo::toArgumentRegister(i));
    }
    result->clobberLate(lateClobbers);
    result->clobber(RegisterSet::macroClobberedGPRs());
    result->effects.terminal = isTerminal;
    if (isTerminal && !rareSuccessorClobbers.isEmpty())
        result->clobberLateOnSuccessor(1, rareSuccessorClobbers);
    result->numGPScratchRegisters = isCallShaped ? 0 : 3;
    lateClobbers.exclude(RegisterSet::macroClobberedGPRs());
    if (!m_options.hasCallShapes)
        lateClobbers = { };
    int32_t number = ++m_observablePatchpointCount;
    modeled(result, Model::Observable, number);
    result->setGenerator([shape = ObservablePatchpointShape { number, isInt32, lateClobbers, rareSuccessorClobbers, isCallShaped, isTerminal, &m_state }](CCallHelpers& jit, const StackmapGenerationParams& params) {
        generateObservablePatchpoint(jit, params, shape);
    });
    if (hasTwoSuccessors) {
        BasicBlock* usual = newBlock();
        BasicBlock* rare = newBlock();
        BasicBlock* next = newBlock();
        if (isTerminal)
            m_block->setSuccessors(FrequentedBlock(usual), rarely(rare));
        else
            branch(make(BitAnd, operand, int64(1)), rarely(rare), FrequentedBlock(usual));
        Variable* outcome = m_proc.addVariable(Int64);
        m_block = usual;
        set(outcome, result);
        jump(next);
        m_block = rare;
        set(outcome, int64(1 + m_coldOperandRandom.getUint32(1000)));
        jump(next);
        m_block = next;
        return get(outcome);
    }
    return result;
}

Value* RandomProgram::movablePatchpoint(unsigned depth)
{
    switch (pick(5)) {
    case 0: {
        unsigned region = pick(3);
        int32_t offset = (region * wordsPerRegion + pick(3)) * sizeof(int64_t);
        PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
        result->append(ConstrainedValue(m_pointer, ValueRep::SomeRegister));
        result->effects = Effects::none();
        result->effects.reads = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
        result->setGenerator([offset](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.load64(CCallHelpers::Address(params[1].gpr(), offset), params[0].gpr());
        });
        return modeled(result, Model::LoadWord, offset);
    }
    case 1: {
        uint64_t key = 1 + pick(3);
        PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
        result->effects = Effects::none();
        result->pureFunction = PureFunction { key };
        if (m_promiseRandom.getUint32(100) < 50)
            m_proc.setKnownRange(result, IntRange(key * 11, key * 11));
        result->setGenerator([key](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(key * 11)), params[0].gpr());
        });
        return modeled(result, Model::Constant, key * 11);
    }
    default: {
        int32_t key = chance(50) ? 0 : 100 + pick(2);
        Value* operand = expression(depth);
        PatchpointValue* result = m_block->appendNew<PatchpointValue>(m_proc, Int64, Origin());
        result->append(ConstrainedValue(operand, ValueRep::SomeRegister));
        result->effects = Effects::none();
        if (key)
            result->pureFunction = PureFunction { static_cast<uint64_t>(key), true };
        if (!key && chance(30))
            result->effects.readsLocalState = true;
        if (!key && chance(30))
            result->effects.readsPinned = true;
        result->setGenerator([key](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.add64(CCallHelpers::TrustedImm32(key + 3), params[1].gpr(), params[0].gpr());
        });
        return modeled(result, Model::AddConstant, key + 3);
    }
    }
}

void RandomProgram::storeBurst()
{
    unsigned count = 6 + pick(10);
    for (unsigned i = 0; i < count; ++i) {
        unsigned region = pick(2);
        HeapRange range = chance(70) ? (region ? secondHeap : firstHeap) : chance(50) ? bothHeaps : allHeaps;
        m_graph.store(m_block, chance(50) ? constant() : get(randomVariable()), m_pointer, (region * wordsPerRegion + pick(wordsPerRegion)) * sizeof(int64_t), range);
    }
}

void RandomProgram::patchpoint()
{
    switch (pick(8)) {
    case 0:
        modeled(m_graph.callLikePatchpoint(m_block), Model::Nothing);
        return;
    case 1: {
        unsigned region = pick(2);
        int32_t offset = (region * wordsPerRegion + pick(chance(60) ? 3 : wordsPerRegion)) * sizeof(int64_t);
        HeapRange heap = region ? secondHeap : firstHeap;
        PatchpointValue* result = appendWordIncrement(m_proc, m_block, m_pointer, offset);
        result->effects.reads = heap;
        result->effects.writes = m_promiseRandom.getUint32(100) < 50 ? createTwoRangeSet(m_proc, heap) : HeapSet(heap);
        if (chance(50))
            result->clobberLate(RegisterSet::argumentGPRs());
        modeled(result, Model::IncrementWord, offset);
        return;
    }
    case 2:
        switch (pick(3)) {
        case 0:
            m_block->appendNew<FenceValue>(m_proc, Origin());
            return;
        case 1:
            m_block->appendNew<Value>(m_proc, Nop, Origin());
            return;
        default: {
            Value* operand = expression(1);
            Value* result = m_block->appendNew<CCallValue>(m_proc, Int64, Origin(),
                address(RandomProgramSymbol::Callee),
                address(RandomProgramSymbol::State), operand);
            if (chance(50))
                m_available.append(result);
            else
                set(randomVariable(), result);
            return;
        }
        }
    case 3:
    case 4:
    case 5: {
        Value* result = observablePatchpoint(expression(1));
        if (chance(50))
            m_available.append(result);
        else if (chance(70))
            set(randomVariable(), result);
        return;
    }
    default: {
        Value* result = movablePatchpoint(1);
        if (chance(50))
            m_available.append(result);
        else
            set(randomVariable(), result);
        return;
    }
    }
}

void RandomProgram::check()
{
    Value* left = expression(2);
    ColdOperands cold = coldOperands();
    CheckValue* result = m_block->appendNew<CheckValue>(m_proc, Check, Origin(), make(Equal, left, constant()));
    Vector<bool, 3> isInt32 = appendColdOperands(result, cold);
    result->setGenerator([isInt32, state = &m_state](CCallHelpers& jit, const StackmapGenerationParams& params) {
        generateFailedCheck(jit, params, isInt32, state);
    });
}

void RandomProgram::clobberArgumentRegisters(PatchpointValue* poll)
{
    poll->clobberLate(RegisterSet::argumentGPRs());
    RefPtr<StackmapGenerator> inner = poll->generator();
    poll->setGenerator([inner](CCallHelpers& jit, const StackmapGenerationParams& params) {
        inner->run(jit, params);
        RegisterSet::argumentGPRs().forEach([&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm64(clobberedRegisterBits), reg.gpr());
        });
    });
}

PatchpointValue* RandomProgram::appendPollVariant(bool isLoopPoll, PollVariant variant)
{
    PatchpointValue* result = modeled(::appendPoll(m_graph, m_block, m_state.poll), Model::Poll);
    result->isLoopPoll = isLoopPoll;
    if (variant == PollVariant::WritesOnlyPollState)
        return result;
    if (variant == PollVariant::OnlyLocalEffects) {
        result->effects = Effects::none();
        result->effects.reads = HeapRange::top();
        result->effects.writesLocalState = true;
        return result;
    }
    result->effects.writes = HeapRange::top();
    if (variant == PollVariant::ClobbersArgumentRegisters)
        clobberArgumentRegisters(result);
    return result;
}

void RandomProgram::continueInNewBlock()
{
    BasicBlock* next = newBlock();
    jump(next);
    m_block = next;
}

void RandomProgram::appendPollOnPath(Value* predicate, FrequentedBlock polling, PollVariant variant)
{
    constexpr bool isLoopPoll = true;
    BasicBlock* continuation = newBlock();
    branch(predicate, polling, FrequentedBlock(continuation));
    m_block = polling.block();
    appendPollVariant(isLoopPoll, variant);
    jump(continuation);
    m_block = continuation;
}

void RandomProgram::poll()
{
    constexpr bool isLoopPoll = true;
    PollVariant variant = chance(70) ? PollVariant::None : static_cast<PollVariant>(1 + pick(static_cast<unsigned>(PollVariant::Count) - 1));

    switch (variant) {
    case PollVariant::None:
    case PollVariant::OnlyLocalEffects:
    case PollVariant::WritesOnlyPollState:
    case PollVariant::ClobbersArgumentRegisters:
        appendPollVariant(isLoopPoll, variant);
        return;
    case PollVariant::TwoPolls:
        appendPollVariant(isLoopPoll, variant);
        appendPollVariant(isLoopPoll, variant);
        return;
    case PollVariant::WithoutRole:
        appendPollVariant(!isLoopPoll, variant);
        return;
    case PollVariant::TwoPollsInTwoBlocks:
        appendPollVariant(isLoopPoll, variant);
        continueInNewBlock();
        appendPollVariant(isLoopPoll, variant);
        return;
    case PollVariant::OnLikelyPath: {
        Value* predicate = make(NotEqual, get(m_variables[0]), int64(0));
        appendPollOnPath(predicate, FrequentedBlock(newBlock()), variant);
        return;
    }
    case PollVariant::OnRandomPath: {
        Value* predicate = condition(1);
        appendPollOnPath(predicate, rarely(newBlock()), variant);
        return;
    }
    case PollVariant::PollAndLookAlike:
        appendPollVariant(isLoopPoll, variant);
        appendPollVariant(!isLoopPoll, variant);
        return;
    case PollVariant::OnRarePathOfVariable: {
        Value* predicate = make(Equal, get(randomVariable()), int64(0));
        appendPollOnPath(predicate, rarely(newBlock()), variant);
        return;
    }
    case PollVariant::InOwnBlock:
        continueInNewBlock();
        appendPollVariant(isLoopPoll, variant);
        continueInNewBlock();
        return;
    case PollVariant::Count:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value* RandomProgram::address(RandomProgramSymbol symbol)
{
    if (m_proc.policy().addressing.isPositionIndependent)
        return m_block->appendNew<RelocatableValue>(m_proc, Origin(), RelocationSymbol { randomProgramSpace, static_cast<uint32_t>(symbol) }, static_cast<int64_t>(0));
    switch (symbol) {
    case RandomProgramSymbol::Callee:
        return m_block->appendNew<ConstPtrValue>(m_proc, Origin(), tagCFunction<OperationPtrTag>(randomProgramCallee));
    case RandomProgramSymbol::State:
        return m_block->appendNew<ConstPtrValue>(m_proc, Origin(), &m_state);
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

#endif // ENABLE(B3_JIT)
