/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

static Tmp argumentTmp(unsigned index) { return Tmp(GPRInfo::toArgumentRegister(index)); }
static Tmp returnTmp() { return Tmp(GPRInfo::returnValueGPR); }

static unsigned numberOfInsts(BasicBlock* block, B3::Air::Opcode opcode)
{
    unsigned result = 0;
    for (Inst& inst : *block)
        result += inst.kind.opcode == opcode;
    return result;
}

static unsigned numberOfInsts(Code& code, B3::Air::Opcode opcode)
{
    unsigned result = 0;
    for (BasicBlock* block : code)
        result += numberOfInsts(block, opcode);
    return result;
}

static unsigned numberOfBlocks(Code& code)
{
    unsigned result = 0;
    for ([[maybe_unused]] BasicBlock* block : code)
        ++result;
    return result;
}

static void appendJump(BasicBlock* from, BasicBlock* to)
{
    from->append(Jump, nullptr);
    from->setSuccessors(to);
}

static void appendBranchTest(BasicBlock* from, Tmp tmp, FrequentedBlock ifNonZero, FrequentedBlock ifZero)
{
    from->append(BranchTest64, nullptr, Arg::resCond(MacroAssembler::NonZero), tmp, tmp);
    from->setSuccessors(ifNonZero, ifZero);
}

static void appendReturn(BasicBlock* block)
{
    block->append(Ret64, nullptr, returnTmp());
}

static void appendReturn(BasicBlock* block, int32_t value)
{
    block->append(Move, nullptr, Arg::imm(value), returnTmp());
    appendReturn(block);
}

static B3::Air::Special* patchpointSpecialOf(Code& code)
{
    return code.addSpecial(makeUniqueWithoutFastMallocCheck<B3::PatchpointSpecial>());
}

static B3::PatchpointValue* addPatchpoint(B3::Procedure& proc, B3::Type type = B3::Void)
{
    B3::BasicBlock* b3Root = proc.size() ? proc.at(0) : proc.addBlock();
    return b3Root->appendNew<B3::PatchpointValue>(proc, type, B3::Origin());
}

static B3::PatchpointValue* addPatchpointWithoutFrame(B3::Procedure& proc, B3::Type type = B3::Void)
{
    B3::PatchpointValue* patchpoint = addPatchpoint(proc, type);
    patchpoint->setRunsWithoutFrame(true);
    return patchpoint;
}

static RegisterSet callClobberedRegisters()
{
    RegisterSet result = RegisterSet::allGPRs();
    result.exclude(RegisterSet::calleeSaveRegisters());
    result.exclude(RegisterSet::stackRegisters());
    result.exclude(RegisterSet::reservedHardwareRegisters());
    return result;
}

static B3::PatchpointValue* addCallLikePatchpoint(B3::Procedure& proc)
{
    B3::PatchpointValue* patchpoint = addPatchpoint(proc);
    patchpoint->clobberLate(callClobberedRegisters());
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        callClobberedRegisters().forEach([&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm32(-1), reg.gpr());
        });
    });
    return patchpoint;
}

static B3::PatchpointValue* addTerminalPatchpoint(B3::Procedure& proc)
{
    B3::PatchpointValue* patchpoint = addPatchpoint(proc, B3::Int64);
    patchpoint->effects.terminal = true;
    patchpoint->resultConstraints = { B3::ValueRep::reg(GPRInfo::returnValueGPR) };
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        CCallHelpers::Jump toSecond = jit.branchTest64(CCallHelpers::NonZero, GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImm32(42), GPRInfo::returnValueGPR);
        CCallHelpers::Jump toFirst;
        if (!params.fallsThroughToSuccessor(0))
            toFirst = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            toSecond.linkTo(*labels[1], &jit);
            if (toFirst.isSet())
                toFirst.linkTo(*labels[0], &jit);
        });
    });
    return patchpoint;
}

static FrequentedBlock rarely(BasicBlock* block) { return FrequentedBlock(block, B3::FrequencyClass::Rare); }

static void letAirEmitFrame(B3::Air::Policy& policy)
{
    policy.frame.emitsFrame = true;
    policy.frame.shrinkWraps = true;
    policy.frame.maxBytesAllocatedBeforeStackCheck = 4096;
    if (isARM64())
        policy.frame.registersClobberedByFrameSetup = RegisterSet::macroClobberedGPRs();
}

static constexpr bool RegisterPolicy::* allocatorRegisterFields[] = { &RegisterPolicy::chargesForCalleeSaves, &RegisterPolicy::discountsPreferredRegisterMoves, &RegisterPolicy::hintsTwoAddressOperands, &RegisterPolicy::usesSecondPreferredRegister, &RegisterPolicy::evictsHolderOfPreferredRegister };
static constexpr bool LiveRangeSplitPolicy::* allocatorSplitFields[] = { &LiveRangeSplitPolicy::splitsAroundClobbersEarly, &LiveRangeSplitPolicy::spansStretchesOfClobbers, &LiveRangeSplitPolicy::limitsClobberSitesPerSplit, &LiveRangeSplitPolicy::splitsIntoClustersOverBlocks };

static void enableAllAllocatorFields(B3::Air::Policy& policy)
{
    for (auto field : allocatorRegisterFields)
        policy.registers.*field = true;
    for (auto field : allocatorSplitFields)
        policy.splitting.*field = true;
}

class PhaseRecordingSpecial final : public B3::Air::Special {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PhaseRecordingSpecial);
public:
    bool didRun(ASCIILiteral phaseName) const { return m_phaseNames.contains(phaseName); }

private:
    // Air::validate() asks this after each phase of a Procedure whose validation level is AtEachPhase.
    bool isValid(Inst&) final
    {
        m_phaseNames.append(String::fromLatin1(code().lastPhaseName()));
        return true;
    }

    void forEachArg(Inst&, NOESCAPE const ScopedLambda<Inst::EachArgCallback>&) final { }
    bool admitsStack(Inst&, unsigned) final { return false; }
    bool admitsExtendedOffsetAddr(Inst&, unsigned) final { return false; }
    void reportUsedRegisters(Inst&, const RegisterSet&) final { }
    MacroAssembler::Jump generate(Inst&, CCallHelpers&, GenerationContext&) final { return { }; }
    RegisterSet extraEarlyClobberedRegs(Inst&) final { return { }; }
    RegisterSet extraClobberedRegs(Inst&) final { return { }; }
    bool needsFrame(Inst&) final { return false; }
    void dumpImpl(PrintStream& out) const final { out.print("PhaseRecording"); }
    void deepDumpImpl(PrintStream& out) const final { out.print("Records the name of the last phase at each validation."); }

    Vector<String> m_phaseNames;
};

static PhaseRecordingSpecial* appendPhaseRecorder(Code& code, BasicBlock* block)
{
    auto* recorder = static_cast<PhaseRecordingSpecial*>(code.addSpecial(makeUnique<PhaseRecordingSpecial>()));
    block->append(Patch, nullptr, Arg::special(recorder));
    return recorder;
}

struct GeneratedCode {
    Vector<uint8_t> bytes;
    std::unique_ptr<Compilation> compilation;
};

static GeneratedCode generateAndLink(B3::Procedure& proc, CCallHelpers& jit)
{
    generate(proc.code(), jit);
    LinkBuffer linkBuffer(jit, nullptr);
    Vector<uint8_t> bytes(std::span { static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size() });
    return { WTF::move(bytes), makeUnique<Compilation>(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "testair compilation"), proc.releaseByproducts()) };
}

static GeneratedCode generateCode(B3::Procedure& proc, InstObserver* observer = nullptr)
{
    proc.code().setInstObserver(observer);
    CCallHelpers jit;
    return generateAndLink(proc, jit);
}

static int64_t buildSumAcrossCalls(B3::Procedure& proc, unsigned numberOfValues, unsigned numberOfCalls, const int64_t* memory)
{
    proc.setOptLevel(2);
    Code& code = proc.code();
    B3::Air::Special* special = patchpointSpecialOf(code);
    B3::PatchpointValue* patchpoint = addCallLikePatchpoint(proc);

    BasicBlock* root = code.addBlock();
    Tmp base = code.newTmp(GP);
    Tmp sum = code.newTmp(GP);
    Vector<Tmp> values;
    root->append(Move, nullptr, Tmp(GPRInfo::argumentGPR0), base);
    for (unsigned i = 0; i < numberOfValues; ++i) {
        values.append(code.newTmp(GP));
        root->append(Move, nullptr, Arg::addr(base, static_cast<int32_t>(i * sizeof(int64_t))), values.last());
    }
    root->append(Move, nullptr, Arg::imm(0), sum);
    int64_t expected = 0;
    for (unsigned round = 0; round < std::max(numberOfCalls, 1u); ++round) {
        if (numberOfCalls)
            root->append(Patch, patchpoint, Arg::special(special));
        for (unsigned i = 0; i < numberOfValues; ++i) {
            for (unsigned k = 0; k <= i % 5; ++k)
                root->append(Add64, nullptr, values[i], sum);
            expected += memory[i] * (i % 5 + 1);
        }
    }
    root->append(Move, nullptr, sum, Tmp(GPRInfo::returnValueGPR));
    root->append(Ret64, nullptr, Tmp(GPRInfo::returnValueGPR));
    return expected;
}

struct InstRange {
    B3::Air::Opcode opcode;
    ptrdiff_t begin;
    ptrdiff_t end;
};

struct ObservedBlock {
    unsigned index;
    size_t numberOfRangesBefore;
};

class RecordingObserver final : public InstObserver {
public:
    void willGenerate(const BasicBlock& block) final
    {
        m_blocks.append({ block.index(), m_generatedInsts.size() });
    }

    void didGenerate(const Inst& inst, MacroAssembler::Label begin, MacroAssembler::Label end) final
    {
        m_generatedInsts.append({ inst.kind.opcode, begin, end });
    }

    const Vector<ObservedBlock>& blocks() const { return m_blocks; }

    Vector<InstRange> ranges(MacroAssembler::Label start) const
    {
        Vector<InstRange> result;
        for (const GeneratedInst& inst : m_generatedInsts)
            result.append({ inst.opcode, CCallHelpers::differenceBetween(start, inst.begin), CCallHelpers::differenceBetween(start, inst.end) });
        return result;
    }

private:
    struct GeneratedInst {
        B3::Air::Opcode opcode;
        MacroAssembler::Label begin;
        MacroAssembler::Label end;
    };

    Vector<GeneratedInst> m_generatedInsts;
    Vector<ObservedBlock> m_blocks;
};

struct ObservedCode {
    GeneratedCode generated;
    MacroAssembler::Label start;
    Vector<InstRange> ranges;
    Vector<ObservedBlock> blocks;
};

static ObservedCode generateObservedCode(B3::Procedure& proc)
{
    RecordingObserver observer;
    proc.code().setInstObserver(&observer);
    CCallHelpers jit;
    MacroAssembler::Label start = jit.label();
    GeneratedCode generated = generateAndLink(proc, jit);
    proc.code().setInstObserver(nullptr);
    return { WTF::move(generated), start, observer.ranges(start), observer.blocks() };
}

void testDefaultPolicyKeepsUpstreamRegisterOrder()
{
    B3::Procedure first;
    first.setOptLevel(2);
    B3::Procedure second { B3::Policy { } };
    second.setOptLevel(2);
    for (B3::Bank bank : { GP, FP })
        CHECK(first.code().regsInPriorityOrder(bank) == second.code().regsInPriorityOrder(bank));
}

void testSuspensionRegistersComeLast()
{
    constexpr unsigned numberOfClobberedRegisters = 3;
    B3::Procedure defaultProcedure;
    defaultProcedure.setOptLevel(2);
    const Vector<Reg>& defaultOrder = defaultProcedure.code().regsInPriorityOrder(GP);

    RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
    B3::Policy policy;
    Vector<Reg> clobbered;
    Vector<Reg> expected;
    for (Reg reg : defaultOrder) {
        if (clobbered.size() == numberOfClobberedRegisters || !calleeSaves.contains(reg, IgnoreVectors)) {
            expected.append(reg);
            continue;
        }
        clobbered.append(reg);
        policy.air.registers.calleeSavesClobberedAtSuspensions.add(reg, IgnoreVectors);
    }
    CHECK(clobbered.size() == numberOfClobberedRegisters);
    std::ranges::sort(clobbered, std::ranges::greater { }, &Reg::index);
    expected.appendVector(clobbered);

    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    CHECK(proc.code().regsInPriorityOrder(GP) == expected);

    B3::Procedure procedureWithEmptySet { B3::Policy { } };
    procedureWithEmptySet.setOptLevel(2);
    CHECK(procedureWithEmptySet.code().regsInPriorityOrder(GP) == defaultOrder);
}

template<typename T, typename Functor>
static void checkOptionSetsOnlyField(T& option, std::type_identity_t<T> value, const Functor& fieldOf)
{
    B3::Air::Policy expected;
    fieldOf(expected) = value;
    CHECK(!toString(expected).isEmpty());
    SetForScope scope(option, value);
    CHECK(toString(B3::Air::Policy::fromProcessOptions()) == toString(expected));
}

void testPolicyFromProcessOptions()
{
    using AirPolicy = B3::Air::Policy;
    CHECK(toString(AirPolicy::fromProcessOptions()).isEmpty());
    checkOptionSetsOnlyField(Options::airRandomizeRegs(), true, [](AirPolicy& policy) -> auto& { return policy.registers.randomizesRegisterOrder; });
    checkOptionSetsOnlyField(Options::airRandomizeRegsSeed(), 7, [](AirPolicy& policy) -> auto& { return policy.registers.registerOrderSeed; });
    checkOptionSetsOnlyField(Options::rareBlockPenalty(), 0.5, [](AirPolicy& policy) -> auto& { return policy.registers.rareBlockPenalty; });
    checkOptionSetsOnlyField(Options::airGreedyRegAllocSpillsEverything(), true, [](AirPolicy& policy) -> auto& { return policy.registers.spillsEverything; });
    checkOptionSetsOnlyField(Options::airGreedyRegAllocSplitAroundLoops(), true, [](AirPolicy& policy) -> auto& { return policy.splitting.splitsAroundLoops; });
    checkOptionSetsOnlyField(Options::airGreedyRegAllocSplitMultiplier(), 3.0, [](AirPolicy& policy) -> auto& { return policy.splitting.splitMultiplier; });
    checkOptionSetsOnlyField(Options::airGreedyRegAllocLoopSplitMaxLoopFraction(), 0.5, [](AirPolicy& policy) -> auto& { return policy.splitting.maxLoopFractionOfLiveRange; });
    checkOptionSetsOnlyField(Options::coalesceSpillSlots(), false, [](AirPolicy& policy) -> auto& { return policy.frame.coalescesSpillSlots; });
    checkOptionSetsOnlyField(Options::useAirOptimizePairedLoadStore(), false, [](AirPolicy& policy) -> auto& { return policy.peepholes.pairsSpillAccesses; });
    SetForScope seedScope(Options::airRandomizeRegsSeed(), 0u);
    CHECK(!AirPolicy::fromProcessOptions().registers.registerOrderSeed);
}

#define CHECK_POLICY_DUMP_NAMES(part, field, value) do { \
        B3::Air::Policy policy; \
        policy.part.field = value; \
        CHECK(toString(policy).contains(#field ""_s)); \
    } while (false)

void testPolicyDumpNamesEveryField()
{
    CHECK(toString(B3::Air::Policy { }).isEmpty());
    RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
    CHECK_POLICY_DUMP_NAMES(registers, randomizesRegisterOrder, true);
    CHECK_POLICY_DUMP_NAMES(registers, registerOrderSeed, 7);
    CHECK_POLICY_DUMP_NAMES(registers, rareBlockPenalty, 0.5);
    CHECK_POLICY_DUMP_NAMES(registers, spillsEverything, true);
    CHECK_POLICY_DUMP_NAMES(registers, chargesForCalleeSaves, true);
    CHECK_POLICY_DUMP_NAMES(registers, discountsPreferredRegisterMoves, true);
    CHECK_POLICY_DUMP_NAMES(registers, hintsTwoAddressOperands, true);
    CHECK_POLICY_DUMP_NAMES(registers, usesSecondPreferredRegister, true);
    CHECK_POLICY_DUMP_NAMES(registers, evictsHolderOfPreferredRegister, true);
    CHECK_POLICY_DUMP_NAMES(registers, unsavedCalleeSaves, calleeSaves);
    CHECK_POLICY_DUMP_NAMES(registers, savedByCallee, calleeSaves);
    CHECK_POLICY_DUMP_NAMES(registers, calleeSavesClobberedAtSuspensions, calleeSaves);
    CHECK_POLICY_DUMP_NAMES(registers, pinned, calleeSaves);
    CHECK_POLICY_DUMP_NAMES(splitting, splitsAroundLoops, true);
    CHECK_POLICY_DUMP_NAMES(splitting, splitMultiplier, 3.0);
    CHECK_POLICY_DUMP_NAMES(splitting, maxLoopFractionOfLiveRange, 0.5);
    CHECK_POLICY_DUMP_NAMES(splitting, splitsAroundClobbersEarly, true);
    CHECK_POLICY_DUMP_NAMES(splitting, spansStretchesOfClobbers, true);
    CHECK_POLICY_DUMP_NAMES(splitting, limitsClobberSitesPerSplit, true);
    CHECK_POLICY_DUMP_NAMES(splitting, splitsIntoClustersOverBlocks, true);
    CHECK_POLICY_DUMP_NAMES(splitting, splitsAroundFrame, true);
    CHECK_POLICY_DUMP_NAMES(frame, coalescesSpillSlots, false);
    CHECK_POLICY_DUMP_NAMES(frame, emitsFrame, true);
    CHECK_POLICY_DUMP_NAMES(frame, shrinkWraps, true);
    CHECK_POLICY_DUMP_NAMES(frame, maxBytesAllocatedBeforeStackCheck, 4096);
    CHECK_POLICY_DUMP_NAMES(frame, registersClobberedByFrameSetup, calleeSaves);
    CHECK_POLICY_DUMP_NAMES(frame, mayPlaceSlotsAboveFrameRecord, true);
    CHECK_POLICY_DUMP_NAMES(frame, mayPushCalleeSaves, true);
    CHECK_POLICY_DUMP_NAMES(frame, mayPushSpillsAtFrameSetup, true);
    CHECK_POLICY_DUMP_NAMES(frame, ordersSlotsByUse, true);
    CHECK_POLICY_DUMP_NAMES(frame, prefersStackPointerAddresses, true);
    CHECK_POLICY_DUMP_NAMES(layout, movesBlocksWithoutSuccessorsToEnd, true);
    CHECK_POLICY_DUMP_NAMES(layout, placesRareBlocksInGaps, true);
    CHECK_POLICY_DUMP_NAMES(layout, placesRareBlocksAfterGuards, true);
    CHECK_POLICY_DUMP_NAMES(layout, makesGapsForRareBlocks, true);
    CHECK_POLICY_DUMP_NAMES(layout, ordersBlocksByEdgeFrequency, true);
    CHECK_POLICY_DUMP_NAMES(layout, recordsRareCodeStart, true);
    CHECK_POLICY_DUMP_NAMES(layout, mergesIdenticalBlocks, true);
    CHECK_POLICY_DUMP_NAMES(layout, duplicatesReturns, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, pairsSpillAccesses, false);
    CHECK_POLICY_DUMP_NAMES(peepholes, forwardsRegisterCopies, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, hoistsCommonMoves, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, reusesConditionFlags, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, pairsMemoryAccesses, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, resequencesMovesBeforePatches, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, emitsShufflesInPairOrder, true);
    CHECK_POLICY_DUMP_NAMES(peepholes, trapReach, 4096);
}

static void checkAirBatchEqualsOneAtATime(std::optional<size_t> failedGraph)
{
    struct Graph {
        unsigned numberOfValues;
        unsigned numberOfCalls;
    };
    constexpr size_t numberOfGraphs = 8;
    constexpr size_t unoptimizedGraph = 2;
    constexpr std::array<Graph, numberOfGraphs> graphs { { { 1, 0 }, { 5, 1 }, { 20, 1 }, { 40, 2 }, { 60, 2 }, { 3, 3 }, { 10, 0 }, { 30, 4 } } };

    std::array<int64_t, 60> memory;
    for (size_t i = 0; i < memory.size(); ++i)
        memory[i] = (i + 1) * 1000003ll;

    std::array<B3::Procedure, numberOfGraphs> separateProcedures;
    std::array<B3::Procedure, numberOfGraphs> batchedProcedures;
    std::array<int64_t, numberOfGraphs> sums;
    Vector<Code*> batch;
    for (size_t i = 0; i < numberOfGraphs; ++i) {
        sums[i] = buildSumAcrossCalls(separateProcedures[i], graphs[i].numberOfValues, graphs[i].numberOfCalls, memory.data());
        buildSumAcrossCalls(batchedProcedures[i], graphs[i].numberOfValues, graphs[i].numberOfCalls, memory.data());
        if (i == unoptimizedGraph) {
            separateProcedures[i].setOptLevel(0);
            batchedProcedures[i].setOptLevel(0);
        }
        prepareForGeneration(separateProcedures[i].code());
        batch.append(&batchedProcedures[i].code());
    }
    std::optional<unsigned> unpreparedHash;
    if (failedGraph) {
        unpreparedHash = batchedProcedures[*failedGraph].code().jsHash();
        batchedProcedures[*failedGraph].fail(B3::FailureKind::AbsoluteAddress);
    }
    prepareForGeneration(batch.span());

    Vector<Vector<uint8_t>> bytes;
    for (size_t i = 0; i < numberOfGraphs; ++i) {
        unsigned separateHash = separateProcedures[i].code().jsHash();
        unsigned batchedHash = batchedProcedures[i].code().jsHash();
        if (i == failedGraph) {
            CHECK(batchedHash == *unpreparedHash);
            CHECK(separateHash != *unpreparedHash);
            continue;
        }
        CHECK(separateHash == batchedHash);
        GeneratedCode separate = generateCode(separateProcedures[i]);
        GeneratedCode batched = generateCode(batchedProcedures[i]);
        CHECK(separate.bytes == batched.bytes);
        CHECK(invoke<int64_t>(*separate.compilation, memory.data()) == sums[i]);
        CHECK(invoke<int64_t>(*batched.compilation, memory.data()) == sums[i]);
        bytes.append(WTF::move(separate.bytes));
    }
    CHECK(bytes[0] != bytes[1]);
}

void testAirBatchEqualsOneAtATime()
{
    checkAirBatchEqualsOneAtATime(std::nullopt);
}

void testAirBatchSkipsFailedProcedure()
{
    constexpr size_t failedGraph = 3;
    checkAirBatchEqualsOneAtATime(failedGraph);
}

static void enableLayoutAndPeepholesExceptMerging(B3::Air::Policy& policy)
{
    policy.layout.movesBlocksWithoutSuccessorsToEnd = true;
    policy.layout.placesRareBlocksInGaps = true;
    policy.layout.placesRareBlocksAfterGuards = true;
    policy.layout.makesGapsForRareBlocks = true;
    policy.layout.ordersBlocksByEdgeFrequency = true;
    policy.layout.recordsRareCodeStart = true;
    policy.layout.duplicatesReturns = true;
    policy.peepholes.forwardsRegisterCopies = true;
    policy.peepholes.hoistsCommonMoves = true;
    policy.peepholes.reusesConditionFlags = true;
    policy.peepholes.pairsMemoryAccesses = true;
    policy.peepholes.resequencesMovesBeforePatches = true;
    policy.peepholes.emitsShufflesInPairOrder = true;
}

static void checkTwoOopsTrap(bool enablesPolicy, bool followsPatch)
{
    B3::Policy policy;
    if (enablesPolicy)
        enableLayoutAndPeepholesExceptMerging(policy.air);
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();

    B3::Air::Special* patchpointSpecial = patchpointSpecialOf(code);
    B3::PatchpointValue* patchpoint = addPatchpoint(proc);
    patchpoint->setGenerator(
        [] (CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            jit.nop();
        });

    BasicBlock* root = code.addBlock();
    BasicBlock* second = code.addBlock();
    BasicBlock* firstOops = code.addBlock();
    BasicBlock* secondOops = code.addBlock();
    BasicBlock* done = code.addBlock();

    root->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR0), Tmp(GPRInfo::argumentGPR0));
    root->setSuccessors(firstOops, second);
    second->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR1), Tmp(GPRInfo::argumentGPR1));
    second->setSuccessors(secondOops, done);
    for (BasicBlock* block : { firstOops, secondOops }) {
        if (followsPatch)
            block->append(Patch, patchpoint, Arg::special(patchpointSpecial));
        block->append(Oops, nullptr);
    }
    done->append(Move, nullptr, Arg::imm(42), Tmp(GPRInfo::returnValueGPR));
    done->append(Ret32, nullptr, Tmp(GPRInfo::returnValueGPR));

    prepareForGeneration(code);
    ObservedCode observed = generateObservedCode(proc);

    unsigned numberOfTraps = 0;
    for (const InstRange& range : observed.ranges) {
        if (range.opcode != Oops)
            continue;
        ++numberOfTraps;
        CHECK(range.end > range.begin);
    }
    CHECK(numberOfTraps == 2);
    CHECK(invoke<int32_t>(*observed.generated.compilation, 0, 0) == 42);
}

void testOopsTraps()
{
    for (bool enablesPolicy : { false, true }) {
        for (bool followsPatch : { false, true })
            checkTwoOopsTrap(enablesPolicy, followsPatch);
    }
}

void testDeadCodeIsEliminated()
{
    enum class Case : uint8_t { UnreadTmp, TmpReadByDeadCode, LockedSlot };
    for (Case testCase : { Case::UnreadTmp, Case::TmpReadByDeadCode, Case::LockedSlot }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        if (testCase == Case::LockedSlot)
            root->append(Move, nullptr, argumentTmp(1), Arg::stack(code.addStackSlot(8, StackSlotKind::Locked)));
        else {
            Tmp dead = code.newTmp(GP);
            root->append(Move, nullptr, Arg::imm(1), dead);
            if (testCase == Case::TmpReadByDeadCode) {
                root->append(Add64, nullptr, Arg::imm(1), dead);
                root->append(Add64, nullptr, Arg::imm(1), dead);
            }
        }
        root->append(Move, nullptr, argumentTmp(1), returnTmp());
        appendReturn(root);

        prepareForGeneration(code);
        CHECK(numberOfInsts(code, Move) == (testCase == Case::LockedSlot ? 2u : 1u));
        CHECK(!numberOfInsts(code, Add64));
    }
}

static void buildObservedGraph(Code& code)
{
    BasicBlock* root = code.addBlock();
    BasicBlock* left = code.addBlock();
    BasicBlock* right = code.addBlock();
    BasicBlock* join = code.addBlock();
    appendBranchTest(root, argumentTmp(0), left, right);
    left->append(Add64, nullptr, Arg::imm(1), argumentTmp(1));
    appendJump(left, join);
    right->append(Add64, nullptr, Arg::imm(2), argumentTmp(1));
    appendJump(right, join);
    join->append(Move, nullptr, argumentTmp(1), returnTmp());
    appendReturn(join);
}

void testInstObserver()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    buildObservedGraph(code);
    prepareForGeneration(code);
    ObservedCode observed = generateObservedCode(proc);

    size_t expectedRanges = 0;
    size_t expectedBlocks = 0;
    for (BasicBlock* block : code) {
        CHECK(expectedBlocks < observed.blocks.size());
        CHECK(observed.blocks[expectedBlocks].index == block->index());
        CHECK(observed.blocks[expectedBlocks].numberOfRangesBefore == expectedRanges);
        ++expectedBlocks;
        expectedRanges += block->size();
        if (block->last().kind.opcode == Jump && block->successorBlock(0) == code.findNextBlock(block))
            --expectedRanges;
    }
    CHECK(observed.blocks.size() == expectedBlocks);
    CHECK(observed.ranges.size() == expectedRanges);
    for (size_t i = 0; i < observed.ranges.size(); ++i) {
        CHECK(observed.ranges[i].begin <= observed.ranges[i].end);
        if (i)
            CHECK(observed.ranges[i - 1].end <= observed.ranges[i].begin);
    }

    B3::Procedure unobserved;
    unobserved.setOptLevel(2);
    buildObservedGraph(unobserved.code());
    prepareForGeneration(unobserved.code());
    CHECK(generateCode(unobserved).bytes == observed.generated.bytes);

    B3::Procedure unoptimized;
    unoptimized.setOptLevel(0);
    buildObservedGraph(unoptimized.code());
    prepareForGeneration(unoptimized.code());
    ObservedCode observedUnoptimized = generateObservedCode(unoptimized);
    CHECK(observedUnoptimized.ranges.isEmpty());
    CHECK(observedUnoptimized.blocks.isEmpty());
}

static unsigned numberOfBlocksReturningOne(Code& code)
{
    unsigned result = 0;
    for (BasicBlock* block : code) {
        bool returnsOne = false;
        for (Inst& inst : *block)
            returnsOne |= inst.kind.opcode == Move && inst.args()[0] == Arg::imm(1) && inst.args()[1] == Arg(returnTmp());
        result += returnsOne;
    }
    return result;
}

void testDefaultPolicyRunsNoForkStep()
{
    constexpr ASCIILiteral requestedForkSteps[] = { "shrinkWrapFrame"_s, "planFrame"_s, "mergeIdenticalBlocks"_s, "duplicateReturns"_s, "reuseConditionFlags"_s };
    constexpr ASCIILiteral otherForkSteps[] = { "moveBlocksWithoutSuccessorsToEnd"_s, "hoistCommonMoves"_s, "forwardRegisterCopies"_s, "resequenceMovesBeforePatches"_s, "foldTrapsIntoAccesses"_s };
    for (bool runsForkSteps : { false, true }) {
        B3::Policy policy;
        policy.validation.level = B3::ValidationLevel::AtEachPhase;
        if (runsForkSteps) {
            policy.air.layout.mergesIdenticalBlocks = true;
            policy.air.layout.duplicatesReturns = true;
            policy.air.peepholes.reusesConditionFlags = true;
            letAirEmitFrame(policy.air);
        }
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* next = code.addBlock();
        BasicBlock* first = code.addBlock();
        BasicBlock* second = code.addBlock();
        BasicBlock* third = code.addBlock();
        BasicBlock* exit = code.addBlock();
        PhaseRecordingSpecial* recorder = appendPhaseRecorder(code, root);
        root->append(Branch32, nullptr, Arg::relCond(MacroAssembler::Equal), argumentTmp(0), Arg::imm(5));
        root->setSuccessors(first, next);
        next->append(Branch32, nullptr, Arg::relCond(MacroAssembler::LessThan), argumentTmp(0), Arg::imm(5));
        next->setSuccessors(second, third);
        for (BasicBlock* block : { first, second, third }) {
            block->append(Move, nullptr, Arg::imm(block == third ? 2 : 1), returnTmp());
            appendJump(block, exit);
        }
        appendReturn(exit);

        prepareForGeneration(code);
        CHECK(recorder->didRun("allocateRegistersByGreedy"_s));
        for (ASCIILiteral step : requestedForkSteps)
            CHECK(recorder->didRun(step) == runsForkSteps);
        for (ASCIILiteral step : otherForkSteps)
            CHECK(!recorder->didRun(step));
        if (runsForkSteps) {
            CHECK(!code.frameLayout().hasFrame);
            CHECK(!numberOfInsts(code, Branch32));
            CHECK(numberOfBlocksReturningOne(code) == 1);
            CHECK(numberOfInsts(code, Ret64) > 1);
        } else {
            CHECK(!code.frameLayout().isPlanned);
            CHECK(!numberOfInsts(code, EnterFrame));
            CHECK(!numberOfInsts(code, LeaveFrame));
            CHECK(numberOfInsts(code, Branch32) == 2);
            CHECK(!numberOfInsts(code, CompareOnFlags32));
            CHECK(!numberOfInsts(code, BranchOnFlags));
            CHECK(numberOfBlocksReturningOne(code) == 2);
            CHECK(numberOfInsts(code, Ret64) == 1);
        }

        GeneratedCode generated = generateCode(proc);
        CHECK(invoke<int64_t>(*generated.compilation, static_cast<int64_t>(5)) == 1);
        CHECK(invoke<int64_t>(*generated.compilation, static_cast<int64_t>(4)) == 1);
        CHECK(invoke<int64_t>(*generated.compilation, static_cast<int64_t>(6)) == 2);
    }
}

static unsigned numberOfSavedRegisters(bool enablesAllocatorFields, unsigned numberOfCalls, bool hasUnsavedCalleeSaves = false)
{
    B3::Policy policy;
    if (enablesAllocatorFields)
        enableAllAllocatorFields(policy.air);
    if (hasUnsavedCalleeSaves)
        policy.air.registers.unsavedCalleeSaves = RegisterSet::calleeSaveRegisters();
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    B3::Air::Special* special = code.addSpecial(makeUniqueWithoutFastMallocCheck<B3::PatchpointSpecial>());
    B3::PatchpointValue* patchpoint = addCallLikePatchpoint(proc);

    BasicBlock* root = code.addBlock();
    StackSlot* slot = code.addStackSlot(8, StackSlotKind::Locked);
    Tmp liveValue = code.newTmp(GP);
    root->append(Move, nullptr, Tmp(GPRInfo::argumentGPR0), liveValue);
    for (unsigned i = 0; i < numberOfCalls; ++i) {
        root->append(Patch, patchpoint, Arg::special(special));
        root->append(Move, nullptr, liveValue, Arg::stack(slot));
    }
    root->append(Move, nullptr, Arg::stack(slot), Tmp(GPRInfo::returnValueGPR));
    root->append(Ret64, nullptr, Tmp(GPRInfo::returnValueGPR));

    auto compilation = compile(proc);
    if (!hasUnsavedCalleeSaves)
        CHECK(invoke<int64_t>(*compilation, 7) == 7);
    return code.calleeSaveRegisterAtOffsetList().registerCount();
}

void testCalleeSaveCost()
{
    CHECK(numberOfSavedRegisters(false, 1) == 1);
    CHECK(numberOfSavedRegisters(true, 1) == 0);
    CHECK(numberOfSavedRegisters(true, 6) == 1);
    CHECK(numberOfSavedRegisters(false, 6) == 1);
}

void testUnsavedCalleeSaves()
{
    CHECK(numberOfSavedRegisters(false, 6, true) == 0);
}

static void checkSumAcrossCalls(const B3::Policy& policy, unsigned numberOfValues, unsigned numberOfCalls)
{
    Vector<int64_t> memory;
    for (unsigned i = 0; i < numberOfValues; ++i)
        memory.append((i + 1) * 1000003ll);
    B3::Procedure proc(policy);
    int64_t expected = buildSumAcrossCalls(proc, numberOfValues, numberOfCalls, memory.span().data());
    CHECK(compileAndRun<int64_t>(proc, memory.span().data()) == expected);
}

void testManyValuesLiveAcrossCall()
{
    for (bool enablesAllocatorFields : { false, true }) {
        B3::Policy policy;
        if (enablesAllocatorFields)
            enableAllAllocatorFields(policy.air);
        checkSumAcrossCalls(policy, 60, 2);
    }
}

void testEachAllocatorFieldAloneIsSound()
{
    constexpr unsigned numberOfValues = 12;
    constexpr unsigned numberOfCalls = 3;
    B3::Policy validatingPolicy;
    validatingPolicy.validation.level = B3::ValidationLevel::AtEachPhase;
    for (auto field : allocatorRegisterFields) {
        B3::Policy policy = validatingPolicy;
        policy.air.registers.*field = true;
        checkSumAcrossCalls(policy, numberOfValues, numberOfCalls);
    }
    for (auto field : allocatorSplitFields) {
        B3::Policy policy = validatingPolicy;
        policy.air.splitting.*field = true;
        checkSumAcrossCalls(policy, numberOfValues, numberOfCalls);
    }
    enableAllAllocatorFields(validatingPolicy.air);
    checkSumAcrossCalls(validatingPolicy, numberOfValues, numberOfCalls);
}

#if CPU(ARM64)
enum class BetweenLoads : uint8_t { Nothing, PossiblyOverlappingStore, StoreToOtherOffsetOfSameBase, BaseIsChanged, FirstIsUsed };

static bool loadsPair(bool pairsMemoryAccesses, BetweenLoads between, int32_t secondOffset = 8)
{
    B3::Policy policy;
    policy.air.peepholes.pairsMemoryAccesses = pairsMemoryAccesses;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    Tmp base(GPRInfo::argumentGPR0);
    Tmp first(GPRInfo::argumentGPR2);
    Tmp second(GPRInfo::argumentGPR3);
    root->append(Move, nullptr, Arg::addr(base, 0), first);
    switch (between) {
    case BetweenLoads::Nothing:
        break;
    case BetweenLoads::PossiblyOverlappingStore:
        root->append(Move, nullptr, first, Arg::addr(Tmp(GPRInfo::argumentGPR1), 8));
        break;
    case BetweenLoads::StoreToOtherOffsetOfSameBase:
        root->append(Move, nullptr, first, Arg::addr(base, 16));
        break;
    case BetweenLoads::BaseIsChanged:
        root->append(Move, nullptr, Tmp(GPRInfo::argumentGPR1), base);
        break;
    case BetweenLoads::FirstIsUsed:
        root->append(Add64, nullptr, first, Tmp(GPRInfo::argumentGPR4));
        break;
    }
    root->append(Move, nullptr, Arg::addr(base, secondOffset), second);
    root->append(Add64, nullptr, first, second, Tmp(GPRInfo::returnValueGPR));
    root->append(Ret64, nullptr, Tmp(GPRInfo::returnValueGPR));

    auto compilation = compile(proc);
    int64_t memory[4] = { 100, 20, 3, 4000 };
    int64_t result = invoke<int64_t>(*compilation, memory, memory, 0, 0, 0);
    CHECK(result == 100 + (between == BetweenLoads::PossiblyOverlappingStore && secondOffset == 8 ? 100 : memory[secondOffset / 8]));
    bool hasPair = false;
    for (Inst& inst : *root)
        hasPair |= inst.kind.opcode == LoadPair64;
    return hasPair;
}

void testLoadPair()
{
    CHECK(loadsPair(true, BetweenLoads::Nothing));
    CHECK(loadsPair(true, BetweenLoads::StoreToOtherOffsetOfSameBase));
    CHECK(loadsPair(true, BetweenLoads::FirstIsUsed));

    CHECK(!loadsPair(false, BetweenLoads::Nothing));
    CHECK(!loadsPair(true, BetweenLoads::Nothing, 16));
    CHECK(!loadsPair(true, BetweenLoads::PossiblyOverlappingStore));
    CHECK(!loadsPair(true, BetweenLoads::BaseIsChanged));
}

static unsigned numberOfStorePairsOfCallArguments(bool pairsMemoryAccesses)
{
    B3::Policy policy;
    policy.air.peepholes.pairsMemoryAccesses = pairsMemoryAccesses;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    code.requestCallArgAreaSizeInBytes(2 * sizeof(int64_t));
    BasicBlock* root = code.addBlock();
    root->append(Move, nullptr, argumentTmp(0), Arg::callArg(0));
    root->append(Move, nullptr, argumentTmp(1), Arg::callArg(8));
    root->append(Move, nullptr, Arg::callArg(0), argumentTmp(2));
    root->append(Move, nullptr, Arg::callArg(8), argumentTmp(3));
    root->append(Add64, nullptr, argumentTmp(2), argumentTmp(3), returnTmp());
    appendReturn(root);

    auto compilation = compile(proc);
    CHECK(invoke<int64_t>(*compilation, static_cast<int64_t>(3), static_cast<int64_t>(4)) == 7);
    return numberOfInsts(code, StorePair64);
}

void testStorePairOfCallArguments()
{
    CHECK(numberOfStorePairsOfCallArguments(true) == 1);
    CHECK(!numberOfStorePairsOfCallArguments(false));
}
#endif

#include "testair_bun_layout.h"
#include "testair_bun_frame.h"
#include "testair_bun_shrink_wrap.h"
#include "testair_bun_passes.h"
#include "testair_bun_flags.h"
#include "testair_bun_flags_memory.h"
#include "testair_bun_traps.h"
#include "testair_bun_liveness.h"
#include "testair_bun_allocation.h"
#include "testair_bun_entry_clobbers.h"
#include "testair_bun_validate.h"

#if CPU(ARM64)
#define RUN_BUN_TESTS_OF_CPU() do { \
        RUN(testLoadPair()); \
        RUN(testStorePairOfCallArguments()); \
    } while (false)
#else
#define RUN_BUN_TESTS_OF_CPU() do { } while (false)
#endif

// Runs the test at once, on the main thread, while no test thread exists: it changes process options that every B3::Procedure() reads.
#define RUN_ALONE(test) do { \
        if (!shouldRun(#test)) \
            break; \
        anyTestsScheduled = true; \
        dataLog(PREFIX #test "...\n"); \
        test; \
        dataLog(PREFIX #test ": OK!\n"); \
    } while (false)

#define RUN_BUN_TESTS() do { \
        RUN_ALONE(testPolicyFromProcessOptions()); \
        RUN(testPolicyDumpNamesEveryField()); \
        RUN(testDefaultPolicyKeepsUpstreamRegisterOrder()); \
        RUN(testSuspensionRegistersComeLast()); \
        RUN(testAirBatchEqualsOneAtATime()); \
        RUN(testAirBatchSkipsFailedProcedure()); \
        RUN(testOopsTraps()); \
        RUN(testDeadCodeIsEliminated()); \
        RUN(testInstObserver()); \
        RUN(testDefaultPolicyRunsNoForkStep()); \
        RUN(testCalleeSaveCost()); \
        RUN(testUnsavedCalleeSaves()); \
        RUN(testManyValuesLiveAcrossCall()); \
        RUN(testEachAllocatorFieldAloneIsSound()); \
        RUN_BUN_TESTS_OF_CPU(); \
        RUN_BUN_LAYOUT_TESTS(); \
        RUN_BUN_FRAME_TESTS(); \
        RUN_BUN_SHRINK_WRAP_TESTS(); \
        RUN_BUN_PASS_TESTS(); \
        RUN_BUN_FLAG_TESTS(); \
        RUN_BUN_FLAG_MEMORY_TESTS(); \
        RUN_BUN_TRAP_TESTS(); \
        RUN_BUN_LIVENESS_TESTS(); \
        RUN_BUN_ALLOCATION_TESTS(); \
        RUN_BUN_ENTRY_CLOBBER_TESTS(); \
        RUN_BUN_VALIDATE_TESTS(); \
    } while (false)
