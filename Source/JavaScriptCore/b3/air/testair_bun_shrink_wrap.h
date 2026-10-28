/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

struct FrameSites {
    unsigned setups;
    unsigned teardowns;

    friend bool operator==(const FrameSites&, const FrameSites&) = default;
};

static FrameSites countFrameSites(Code& code)
{
    return { numberOfInsts(code, EnterFrame), numberOfInsts(code, LeaveFrame) };
}

static FrameState stateAtHead(Code& code, BasicBlock* block)
{
    return FrameStates(code).atHead(block);
}

static B3::Policy shrinkWrappingPolicy()
{
    B3::Policy policy;
    letAirEmitFrame(policy.air);
    return policy;
}

static void prepare(B3::Procedure& proc)
{
    proc.setOptLevel(2);
    prepareForGeneration(proc.code());
}

static FramePlacement placeFrame(Code& code, const StackColoring& coloring)
{
    FramePlacement placement = shrinkWrapFrame(code, coloring);
    planFrame(code, coloring, placement);
    code.setEntrypoints(Vector<FrequentedBlock> { FrequentedBlock(code[0]) });
    return placement;
}

struct ExpectedFrame {
    FrameCoverage coverage;
    FrameSites sites;
    Vector<BasicBlock*> setUpAtHead { };
    Vector<BasicBlock*> notSetUpAtHead { };
};

static void checkFrame(Code& code, const ExpectedFrame& expected)
{
    const FrameLayout& layout = code.frameLayout();
    CHECK(layout.isPlanned);
    CHECK(layout.coverage == expected.coverage);
    CHECK(layout.hasFrame == (expected.coverage != FrameCoverage::NoPath));
    CHECK(countFrameSites(code) == expected.sites);
    for (BasicBlock* block : expected.setUpAtHead)
        CHECK(stateAtHead(code, block) == FrameState::SetUp);
    for (BasicBlock* block : expected.notSetUpAtHead)
        CHECK(stateAtHead(code, block) == FrameState::NotSetUp);
}

static void checkEdgeBlock(BasicBlock* edge, B3::Air::Opcode opcode, BasicBlock* target)
{
    CHECK(edge != target);
    CHECK(edge->size() == 2);
    CHECK(edge->at(0).kind.opcode == opcode);
    CHECK(edge->last().kind.opcode == Jump);
    CHECK(edge->successorBlock(0) == target);
}

static bool isScratchOperand(const Inst& enterFrame, Reg reg)
{
    std::span<const Arg> scratchOperands = enterFrame.args().subspan(1, EnterFrameCustom::numberOfScratchRegisters(enterFrame));
    return std::ranges::find(scratchOperands, Arg(Tmp(reg))) != scratchOperands.end();
}

static bool isInFrameRange(Code& code, MacroAssembler::Label start, ptrdiff_t offset)
{
    return code.frameRanges().containsIf([&](const FrameRange& range) {
        return CCallHelpers::differenceBetween(start, range.begin) <= offset && offset < CCallHelpers::differenceBetween(start, range.end);
    });
}

template<typename... Arguments>
static int64_t invokeAt(CodeLocationLabel<JITCompilationPtrTag> entrypoint, Arguments... arguments)
{
    auto* function = std::bit_cast<int64_t (SYSV_ABI*)(Arguments...)>(untagCFunctionPtr<JITCompilationPtrTag>(entrypoint.taggedPtr()));
    return function(arguments...);
}

struct ExpectedRun {
    int64_t first { 0 };
    int64_t second { 0 };
    int64_t result { 0 };
    int64_t numberOfCalls { 0 };
};

enum class Calls : bool { No, Yes };

struct FrameGraph {
    enum class Word : uint8_t { First, Second, NumberOfCalls, NumberOfIterations, Sum };
    static constexpr size_t numberOfWords = 5;

    explicit FrameGraph(const B3::Policy& policy = shrinkWrappingPolicy())
        : proc(policy)
        , code(proc.code())
    {
        proc.setOptLevel(2);
    }

    int64_t& wordAt(Word word) { return memory[static_cast<size_t>(word)]; }
    Arg addressOf(Word word) const { return Arg::addr(base, static_cast<int32_t>(static_cast<size_t>(word) * sizeof(int64_t))); }

    void appendAddTo(BasicBlock* block, Word word, Arg addend)
    {
        block->append(Move, nullptr, addressOf(word), scratch);
        block->append(Add64, nullptr, addend, scratch);
        block->append(Move, nullptr, scratch, addressOf(word));
    }

    void appendAdd(BasicBlock* block, Arg addend) { appendAddTo(block, Word::Sum, addend); }

    void appendCallAndReload(BasicBlock* block)
    {
        appendCall(proc, block);
        block->append(Move, nullptr, Arg::immPtr(memory.data()), base);
        appendAddTo(block, Word::NumberOfCalls, Arg::imm(1));
        block->append(Move, nullptr, addressOf(Word::First), first);
        block->append(Move, nullptr, addressOf(Word::Second), second);
    }

    void appendReturnOfSum(BasicBlock* block)
    {
        block->append(Move, nullptr, addressOf(Word::Sum), returnTmp());
        appendReturn(block);
    }

    void appendLoopBranch(BasicBlock* latch, BasicBlock* header, BasicBlock* exit)
    {
        appendAddTo(latch, Word::NumberOfIterations, Arg::imm(1));
        latch->append(Branch64, nullptr, Arg::relCond(MacroAssembler::LessThan), scratch, first);
        latch->setSuccessors(header, exit);
    }

    BasicBlock* appendBody(BasicBlock* block, int32_t addend, Calls calls)
    {
        if (calls == Calls::Yes)
            appendCallAndReload(block);
        appendAdd(block, Arg::imm(addend));
        return block;
    }

    BasicBlock* appendAddAndReturn(BasicBlock* block, int32_t addend, Calls calls = Calls::No)
    {
        appendReturnOfSum(appendBody(block, addend, calls));
        return block;
    }

    BasicBlock* appendAddAndJump(BasicBlock* block, int32_t addend, BasicBlock* target, Calls calls = Calls::No)
    {
        appendJump(appendBody(block, addend, calls), target);
        return block;
    }

    BasicBlock* appendAddAndBranch(BasicBlock* block, int32_t addend, Tmp condition, FrequentedBlock ifNonZero, FrequentedBlock ifZero, Calls calls = Calls::No)
    {
        appendBranchTest(appendBody(block, addend, calls), condition, ifNonZero, ifZero);
        return block;
    }

    BasicBlock* addReturn(int32_t addend, Calls calls = Calls::No) { return appendAddAndReturn(code.addBlock(), addend, calls); }
    BasicBlock* addJump(int32_t addend, BasicBlock* target, Calls calls = Calls::No) { return appendAddAndJump(code.addBlock(), addend, target, calls); }
    BasicBlock* addBranch(int32_t addend, Tmp condition, FrequentedBlock ifNonZero, FrequentedBlock ifZero, Calls calls = Calls::No)
    {
        return appendAddAndBranch(code.addBlock(), addend, condition, ifNonZero, ifZero, calls);
    }

    void prepareAndCheck(const ExpectedFrame& expected)
    {
        prepare(proc);
        checkFrame(code, expected);
    }

    const Compilation& compilation()
    {
        if (!generated.compilation)
            generated = generateCode(proc);
        return *generated.compilation;
    }

    template<typename Invoker>
    void checkRun(const ExpectedRun& expected, const Invoker& invoker)
    {
        memory.fill(0);
        wordAt(Word::First) = expected.first;
        wordAt(Word::Second) = expected.second;
        int64_t result = invoker(expected.first, expected.second, static_cast<int64_t>(0), memory.data());
        CHECK(result == expected.result);
        CHECK(wordAt(Word::NumberOfCalls) == expected.numberOfCalls);
    }

    void checkRun(const ExpectedRun& expected)
    {
        const Compilation& compiledCode = compilation();
        checkRun(expected, [&](auto... arguments) {
            return invoke<int64_t>(compiledCode, arguments...);
        });
    }

    B3::Procedure proc;
    Code& code;
    BasicBlock* entry { code.addBlock() };
    Arg special { Arg::special(patchpointSpecialOf(code)) };
    Tmp first { argumentTmp(0) };
    Tmp second { argumentTmp(1) };
    Tmp scratch { argumentTmp(2) };
    Tmp base { argumentTmp(3) };
    std::array<int64_t, numberOfWords> memory { };
    GeneratedCode generated;
};

struct EarlyExitGraph : FrameGraph {
    explicit EarlyExitGraph(const B3::Policy& policy = shrinkWrappingPolicy())
        : FrameGraph(policy)
    {
        appendBranchTest(entry, first, caller, early);
    }

    void checkRuns()
    {
        checkRun({ .first = 0, .result = 1 });
        checkRun({ .first = 1, .result = 2, .numberOfCalls = 1 });
    }

    BasicBlock* early { addReturn(1) };
    BasicBlock* caller { addReturn(2, Calls::Yes) };
};

struct RareCallGraph : FrameGraph {
    RareCallGraph() { appendBranchTest(entry, first, rarely(rare), join); }

    void checkRuns()
    {
        checkRun({ .first = 0, .result = 2 });
        checkRun({ .first = 1, .result = 3, .numberOfCalls = 1 });
    }

    BasicBlock* join { addReturn(2) };
    BasicBlock* rare { addJump(1, join, Calls::Yes) };
};

struct TwoRareCallsGraph : FrameGraph {
    TwoRareCallsGraph() { appendBranchTest(entry, first, rarely(firstRare), middle); }

    void checkRuns()
    {
        checkRun({ .first = 0, .second = 0, .result = 10 });
        checkRun({ .first = 1, .second = 0, .result = 11, .numberOfCalls = 1 });
        checkRun({ .first = 0, .second = 1, .result = 14, .numberOfCalls = 1 });
        checkRun({ .first = 1, .second = 1, .result = 15, .numberOfCalls = 2 });
    }

    BasicBlock* exit { addReturn(8) };
    BasicBlock* secondRare { addJump(4, exit, Calls::Yes) };
    BasicBlock* middle { addBranch(2, second, rarely(secondRare), exit) };
    BasicBlock* firstRare { addJump(1, middle, Calls::Yes) };
};

void testLeafHasNoFrame()
{
    FrameGraph graph;
    graph.appendAdd(graph.entry, graph.first);
    graph.appendAdd(graph.entry, graph.second);
    graph.appendReturnOfSum(graph.entry);

    graph.prepareAndCheck({ .coverage = FrameCoverage::NoPath, .sites = { 0, 0 } });
    CHECK(!graph.code.frameSize());
    graph.checkRun({ .first = 3, .second = 4, .result = 7 });
    CHECK(graph.code.frameRanges().isEmpty());
}

void testPatchpointWithoutFrame()
{
    for (bool runsWithoutFrame : { true, false }) {
        FrameGraph graph;
        B3::PatchpointValue* patchpoint = runsWithoutFrame ? addPatchpointWithoutFrame(graph.proc) : addPatchpoint(graph.proc);
        patchpoint->clobberLate(RegisterSet(GPRInfo::argumentGPR1));
        patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
            jit.move(CCallHelpers::TrustedImm32(5), GPRInfo::argumentGPR1);
        });
        graph.entry->append(Patch, patchpoint, graph.special);
        graph.appendAdd(graph.entry, graph.second);
        graph.appendReturnOfSum(graph.entry);

        unsigned numberOfSites = runsWithoutFrame ? 0 : 1;
        graph.prepareAndCheck({ .coverage = runsWithoutFrame ? FrameCoverage::NoPath : FrameCoverage::EveryPathNeedsFrame, .sites = { numberOfSites, numberOfSites } });
        graph.checkRun({ .result = 5 });
    }
}

void testEarlyExitHasNoFrame()
{
    EarlyExitGraph graph;
    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, graph.early } });
    CHECK(graph.caller->at(0).kind.opcode == EnterFrame);
    graph.checkRuns();
}

void testRareCallRejoins()
{
    RareCallGraph graph;
    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, graph.join } });
    CHECK(numberOfInsts(graph.rare, EnterFrame) == 1);
    CHECK(numberOfInsts(graph.rare, LeaveFrame) == 1);
    graph.checkRuns();
}

void testTwoRareCalls()
{
    TwoRareCallsGraph graph;
    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 2, 2 }, .notSetUpAtHead = { graph.entry, graph.middle, graph.exit } });
    graph.checkRuns();
}

void testLikelyCallsShareFrame()
{
    FrameGraph graph;
    BasicBlock* early = graph.addReturn(1);
    BasicBlock* secondCaller = graph.addReturn(16, Calls::Yes);
    BasicBlock* left = graph.addJump(4, secondCaller);
    BasicBlock* right = graph.addJump(8, secondCaller);
    BasicBlock* firstCaller = graph.addBranch(2, graph.second, left, right, Calls::Yes);
    appendBranchTest(graph.entry, graph.first, firstCaller, early);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .setUpAtHead = { left, right, secondCaller }, .notSetUpAtHead = { graph.entry, early } });
    graph.checkRun({ .first = 0, .result = 1 });
    graph.checkRun({ .first = 1, .second = 1, .result = 22, .numberOfCalls = 2 });
    graph.checkRun({ .first = 1, .second = 0, .result = 26, .numberOfCalls = 2 });
}

void testLoopWithLikelyCallHoldsFrame()
{
    FrameGraph graph;
    BasicBlock* early = graph.addReturn(1);
    BasicBlock* exit = graph.addReturn(0);
    BasicBlock* header = graph.code.addBlock();
    BasicBlock* latch = graph.code.addBlock();
    BasicBlock* body = graph.addJump(0, latch, Calls::Yes);
    appendBranchTest(graph.entry, graph.first, header, early);
    graph.appendAddAndBranch(header, 2, graph.second, body, latch);
    graph.appendLoopBranch(latch, header, exit);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .setUpAtHead = { header, body, latch, exit }, .notSetUpAtHead = { graph.entry, early } });
    graph.checkRun({ .first = 0, .result = 1 });
    graph.checkRun({ .first = 3, .second = 1, .result = 6, .numberOfCalls = 3 });
    graph.checkRun({ .first = 3, .second = 0, .result = 6 });
}

void testLoopWithRareCall()
{
    FrameGraph graph;
    BasicBlock* exit = graph.addReturn(0);
    BasicBlock* header = graph.code.addBlock();
    BasicBlock* latch = graph.code.addBlock();
    BasicBlock* rare = graph.addJump(0, latch, Calls::Yes);
    graph.appendAddAndJump(graph.entry, 1, header);
    graph.appendAddAndBranch(header, 2, graph.second, rarely(rare), latch);
    graph.appendLoopBranch(latch, header, exit);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, header, latch, exit } });
    CHECK(rare->at(0).kind.opcode == EnterFrame);
    graph.checkRun({ .first = 3, .second = 0, .result = 7 });
    graph.checkRun({ .first = 3, .second = 1, .result = 7, .numberOfCalls = 3 });
}

void testFrameCertainToFollow()
{
    FrameGraph graph;
    BasicBlock* early = graph.addReturn(1);
    BasicBlock* left = graph.addReturn(4, Calls::Yes);
    BasicBlock* right = graph.addReturn(8, Calls::Yes);
    BasicBlock* fork = graph.addBranch(2, graph.second, left, right);
    appendBranchTest(graph.entry, graph.first, fork, early);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 2 }, .setUpAtHead = { left, right }, .notSetUpAtHead = { graph.entry, early } });
    CHECK(fork->at(0).kind.opcode == EnterFrame);
    graph.checkRun({ .first = 0, .result = 1 });
    graph.checkRun({ .first = 1, .second = 1, .result = 6, .numberOfCalls = 1 });
    graph.checkRun({ .first = 1, .second = 0, .result = 10, .numberOfCalls = 1 });
}

void testFrameCertainToPrecede()
{
    FrameGraph graph;
    BasicBlock* early = graph.addReturn(1);
    BasicBlock* exit = graph.addReturn(16);
    BasicBlock* left = graph.addJump(4, exit);
    BasicBlock* right = graph.addJump(8, exit);
    BasicBlock* caller = graph.addBranch(2, graph.second, left, right, Calls::Yes);
    appendBranchTest(graph.entry, graph.first, caller, early);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .setUpAtHead = { left, right, exit }, .notSetUpAtHead = { graph.entry, early } });
    CHECK(exit->at(exit->size() - 2).kind.opcode == LeaveFrame);
    graph.checkRun({ .first = 0, .result = 1 });
    graph.checkRun({ .first = 1, .second = 1, .result = 22, .numberOfCalls = 1 });
    graph.checkRun({ .first = 1, .second = 0, .result = 26, .numberOfCalls = 1 });
}

void testCalleeSaveLiveAcrossBlocks()
{
    FrameGraph graph;
    Tmp saved(GPRInfo::regCS0);
    BasicBlock* early = graph.addReturn(1);
    BasicBlock* user = graph.code.addBlock();
    BasicBlock* through = graph.addJump(2, user);
    BasicBlock* caller = graph.addJump(4, user, Calls::Yes);
    BasicBlock* definer = graph.code.addBlock();
    appendBranchTest(graph.entry, graph.first, definer, early);
    definer->append(Move, nullptr, graph.first, saved);
    appendBranchTest(definer, graph.second, through, caller);
    graph.appendAdd(user, saved);
    graph.appendReturnOfSum(user);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .setUpAtHead = { through, caller, user }, .notSetUpAtHead = { graph.entry, early } });
    CHECK(graph.code.calleeSaveRegisterAtOffsetList().registerCount() == 1);
    CHECK(definer->at(0).kind.opcode == EnterFrame);
    CHECK(definer->at(0).args().back() == Arg(saved));
    graph.checkRun({ .first = 0, .result = 1 });
    graph.checkRun({ .first = 7, .second = 1, .result = 9 });
    graph.checkRun({ .first = 7, .second = 0, .result = 11, .numberOfCalls = 1 });
}

void testRareCallSpillsInsideRegion()
{
    B3::Policy policy = shrinkWrappingPolicy();
    enableAllAllocatorFields(policy.air);
    FrameGraph graph(policy);
    BasicBlock* rare = graph.code.addBlock();
    BasicBlock* join = graph.code.addBlock();
    Tmp left = graph.code.newTmp(GP);
    Tmp right = graph.code.newTmp(GP);
    graph.entry->append(Move, nullptr, graph.first, left);
    graph.entry->append(Move, nullptr, graph.second, right);
    appendBranchTest(graph.entry, left, rarely(rare), join);
    rare->append(Add64, nullptr, Arg::imm(1), right);
    appendCall(graph.proc, rare);
    appendJump(rare, join);
    join->append(Add64, nullptr, left, right);
    join->append(Move, nullptr, right, returnTmp());
    appendReturn(join);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, join } });
    CHECK(!graph.code.calleeSaveRegisterAtOffsetList().registerCount());
    CHECK(graph.code.frameSize());
    CHECK(numberOfInsts(rare, EnterFrame) == 1);
    graph.checkRun({ .first = 0, .second = 5, .result = 5 });
    graph.checkRun({ .first = 3, .second = 5, .result = 9 });
}

void testLiveScratchRegisterGrowsRegion()
{
    constexpr GPRReg liveGPR = GPRInfo::nonArgGPR0;
    for (bool isClobberedByFrameSetup : { false, true }) {
        B3::Policy policy = shrinkWrappingPolicy();
        if (isClobberedByFrameSetup)
            policy.air.frame.registersClobberedByFrameSetup.add(liveGPR);
        FrameGraph graph(policy);
        BasicBlock* early = graph.addReturn(1);
        BasicBlock* join = graph.addReturn(4);
        BasicBlock* rare = graph.code.addBlock();
        BasicBlock* definer = graph.code.addBlock();
        appendBranchTest(graph.entry, graph.first, definer, early);
        definer->append(Move, nullptr, graph.second, Tmp(liveGPR));
        graph.appendAddAndBranch(definer, 2, graph.second, rarely(rare), join);
        graph.appendAdd(rare, Tmp(liveGPR));
        graph.appendAddAndJump(rare, 0, join, Calls::Yes);
        graph.code.setStackCheckGenerator(createSharedTask<StackCheckGeneratorFunction>([=](CCallHelpers& jit, GenerationContext&, const StackCheckSite&) {
            if (isClobberedByFrameSetup)
                jit.move(CCallHelpers::TrustedImm32(-1), liveGPR);
        }));

        graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, early } });
        const Inst& enterFrame = (isClobberedByFrameSetup ? definer : rare)->at(0);
        CHECK(enterFrame.kind.opcode == EnterFrame);
        CHECK(isScratchOperand(enterFrame, liveGPR) == isClobberedByFrameSetup);
        graph.checkRun({ .first = 0, .result = 1 });
        graph.checkRun({ .first = 1, .second = 0, .result = 6 });
        graph.checkRun({ .first = 1, .second = 5, .result = 11, .numberOfCalls = 1 });
    }
}

static std::unique_ptr<Compilation> compileTrampolineWithStackArgument()
{
    CCallHelpers jit;
    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(stackAlignmentBytes()), CCallHelpers::stackPointerRegister);
    jit.storePtr(GPRInfo::argumentGPR4, CCallHelpers::Address(CCallHelpers::stackPointerRegister));
    jit.call(GPRInfo::argumentGPR5, JITCompilationPtrTag);
    jit.emitFunctionEpilogue();
    jit.ret();
    LinkBuffer linkBuffer(jit, nullptr);
    return makeUnique<Compilation>(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "testair trampoline"), nullptr);
}

void testIncomingArgumentsWithoutFrame()
{
    constexpr int64_t stackArgument = 77;
    constexpr int32_t offsetFromFramePointer = sizeof(CallerFrameAndPC);
    constexpr int32_t offsetFromStackPointer = offsetFromFramePointer - static_cast<int32_t>(prologueStackPointerDelta());
    Arg addressInFrame = Arg::addr(Tmp(GPRInfo::callFrameRegister), offsetFromFramePointer);
    std::unique_ptr<Compilation> trampoline = compileTrampolineWithStackArgument();

    for (bool readsInFrame : { true, false }) {
        FrameGraph graph;
        Arg sourceInFrame = readsInFrame ? addressInFrame : Arg::imm(0);
        BasicBlock* leaf = graph.code.addBlock();
        BasicBlock* caller = graph.code.addBlock();
        appendBranchTest(graph.entry, graph.first, caller, leaf);
        leaf->append(Move, nullptr, addressInFrame, graph.second);
        graph.appendAdd(leaf, graph.second);
        graph.appendReturnOfSum(leaf);
        graph.appendCallAndReload(caller);
        unsigned loadIndex = caller->size();
        caller->append(Move, nullptr, sourceInFrame, graph.second);
        graph.appendAdd(caller, graph.second);
        graph.appendReturnOfSum(caller);

        graph.code.resetReachability();
        FramePlacement placement = placeFrame(graph.code, allocateStackByGraphColoring(graph.code));
        checkFrame(graph.code, { .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, leaf } });
        CHECK(placement.addressesIncomingArgumentsInFrame == readsInFrame);
        CHECK(leaf->at(0).args()[0] == Arg::addr(Tmp(MacroAssembler::stackPointerRegister), offsetFromStackPointer));
        CHECK(caller->at(0).kind.opcode == EnterFrame);
        CHECK(caller->at(loadIndex + 1).args()[0] == sourceInFrame);

        void* target = graph.compilation().code().taggedPtr();
        auto invokeThroughTrampoline = [&](auto... arguments) {
            return invoke<int64_t>(*trampoline, arguments..., stackArgument, target);
        };
        graph.checkRun({ .first = 0, .result = stackArgument }, invokeThroughTrampoline);
        graph.checkRun({ .first = 1, .result = readsInFrame ? stackArgument : 0, .numberOfCalls = 1 }, invokeThroughTrampoline);
    }
}

void testSeveralEntrypoints()
{
    constexpr unsigned numberOfEntrypoints = 2;
    FrameGraph graph;
    graph.proc.setNumEntrypoints(numberOfEntrypoints);
    graph.proc.setUsesEntrySwitch(true);
    graph.entry->append(EntrySwitch, nullptr);
    graph.entry->setSuccessors(graph.addReturn(1), graph.addReturn(2, Calls::Yes));

    graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathBecauseOfSeveralEntrypoints, .sites = { numberOfEntrypoints, numberOfEntrypoints } });
    CHECK(graph.code.numEntrypoints() == numberOfEntrypoints);
    for (const FrequentedBlock& entrypoint : graph.code.entrypoints())
        CHECK(entrypoint.block()->at(0).kind.opcode == EnterFrame);

    CCallHelpers jit;
    generate(graph.code, jit);
    LinkBuffer linkBuffer(jit, nullptr);
    std::array<CodeLocationLabel<JITCompilationPtrTag>, numberOfEntrypoints> entrypoints;
    for (unsigned index = 0; index < numberOfEntrypoints; ++index)
        entrypoints[index] = linkBuffer.locationOf<JITCompilationPtrTag>(graph.code.entrypointLabel(index));
    Compilation compilation(FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "testair compilation"), graph.proc.releaseByproducts());
    for (unsigned index = 0; index < numberOfEntrypoints; ++index) {
        graph.checkRun({ .result = index + 1, .numberOfCalls = index }, [&](auto... arguments) {
            return invokeAt(entrypoints[index], arguments...);
        });
    }
}

void testLargeFrameCoversEveryPath()
{
    constexpr unsigned slotSize = 8192;
    FrameGraph graph;
    StackSlot* slot = graph.code.addStackSlot(slotSize, StackSlotKind::Locked);
    BasicBlock* join = graph.addReturn(1);
    BasicBlock* rare = graph.code.addBlock();
    appendBranchTest(graph.entry, graph.first, rarely(rare), join);
    for (Arg word : { Arg::stack(slot), Arg::stack(slot, static_cast<int32_t>(slotSize - sizeof(int64_t))) }) {
        rare->append(Move, nullptr, graph.second, word);
        rare->append(Move, nullptr, word, graph.first);
        graph.appendAdd(rare, graph.first);
    }
    appendJump(rare, join);

    graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathBecauseFrameIsLarge, .sites = { 1, 1 }, .setUpAtHead = { rare, join } });
    CHECK(graph.code.frameSize() >= slotSize);
    const Inst& enterFrame = graph.entry->at(0);
    CHECK(enterFrame.kind.opcode == EnterFrame);
    RegisterSet::macroClobberedGPRs().forEach([&](Reg reg) {
        CHECK(isScratchOperand(enterFrame, reg));
    });
    graph.checkRun({ .first = 0, .second = 5, .result = 1 });
    graph.checkRun({ .first = 1, .second = 5, .result = 11 });
}

void testShrinkWrappingOff()
{
    B3::Policy policy = shrinkWrappingPolicy();
    policy.air.frame.shrinkWraps = false;

    EarlyExitGraph graph(policy);
    graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathBecauseShrinkWrappingIsOff, .sites = { 1, 2 }, .setUpAtHead = { graph.early, graph.caller } });
    CHECK(graph.entry->at(0).kind.opcode == EnterFrame);
    graph.checkRuns();

    FrameGraph leaf(policy);
    leaf.appendAddAndReturn(leaf.entry, 1);
    leaf.prepareAndCheck({ .coverage = FrameCoverage::EveryPathBecauseShrinkWrappingIsOff, .sites = { 1, 1 } });
    leaf.checkRun({ .result = 1 });
}

void testFrameIsSetUpAtEntry()
{
    B3::Policy policy = shrinkWrappingPolicy();
    policy.air.frame.setsUpFrameAtEntry = true;

    EarlyExitGraph graph(policy);
    graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathNeedsFrame, .sites = { 1, 2 }, .setUpAtHead = { graph.early, graph.caller } });
    CHECK(graph.entry->at(0).kind.opcode == EnterFrame);
    graph.checkRuns();

    FrameGraph leaf(policy);
    leaf.appendAddAndReturn(leaf.entry, 1);
    leaf.prepareAndCheck({ .coverage = FrameCoverage::NoPath, .sites = { 0, 0 } });
    leaf.checkRun({ .result = 1 });
}

static B3::Policy policyThatTestsFrames(unsigned smallestFrameTested = 0)
{
    B3::Policy policy = shrinkWrappingPolicy();
    policy.air.frame.smallestFrameTestedAtSetup = smallestFrameTested;
    return policy;
}

void testTerminalPatchSuccessorsKeepFrame()
{
    for (bool runsWithoutFrame : { false, true }) {
        FrameGraph graph;
        B3::PatchpointValue* patchpoint = addTerminalPatchpoint(graph.proc);
        patchpoint->setRunsWithoutFrame(runsWithoutFrame);
        BasicBlock* early = graph.addReturn(1);
        BasicBlock* continuation = graph.addReturn(2);
        BasicBlock* exit = graph.addReturn(4);
        BasicBlock* terminal = graph.code.addBlock();
        appendBranchTest(graph.entry, graph.second, terminal, early);
        terminal->append(Patch, patchpoint, graph.special, returnTmp());
        terminal->setSuccessors(continuation, exit);

        if (runsWithoutFrame)
            graph.prepareAndCheck({ .coverage = FrameCoverage::NoPath, .sites = { 0, 0 }, .notSetUpAtHead = { continuation, exit } });
        else
            graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 2 }, .setUpAtHead = { continuation, exit }, .notSetUpAtHead = { graph.entry, early } });
        graph.checkRun({ .second = 0, .result = 1 });
        graph.checkRun({ .first = 0, .second = 1, .result = 2 });
        graph.checkRun({ .first = 1, .second = 1, .result = 4 });
    }
}

void testEntryBlockWithPredecessors()
{
    FrameGraph graph;
    BasicBlock* header = graph.entry;
    BasicBlock* exit = graph.addReturn(1);
    graph.appendCallAndReload(header);
    graph.appendLoopBranch(header, header, exit);

    graph.code.resetReachability();
    FramePlacement placement = placeFrame(graph.code, StackColoring { });
    checkFrame(graph.code, { .coverage = FrameCoverage::EveryPathNeedsFrame, .sites = { 1, 1 }, .setUpAtHead = { header, exit } });
    checkEdgeBlock(graph.code[0], EnterFrame, header);
    CHECK(placement.didAddBlocks);
    CHECK(placement.setups.size() == 1);
    CHECK(placement.setups[0].block == graph.code[0] && !placement.setups[0].instIndex);
    graph.checkRun({ .first = 0, .result = 1, .numberOfCalls = 1 });
    graph.checkRun({ .first = 3, .result = 1, .numberOfCalls = 3 });
}

void testCriticalEdgesAreSplit()
{
    FrameGraph graph;
    BasicBlock* early = graph.addReturn(16);
    BasicBlock* tail = graph.addReturn(8);
    BasicBlock* inside = graph.addBranch(4, graph.second, tail, early, Calls::Yes);
    BasicBlock* caller = graph.addJump(1, inside, Calls::Yes);
    BasicBlock* fork = graph.addBranch(2, graph.second, rarely(inside), early);
    appendBranchTest(graph.entry, graph.first, caller, fork);

    graph.code.resetReachability();
    FramePlacement placement = placeFrame(graph.code, allocateStackByGraphColoring(graph.code));
    checkFrame(graph.code, { .coverage = FrameCoverage::SomePaths, .sites = { 2, 2 }, .setUpAtHead = { inside, tail }, .notSetUpAtHead = { graph.entry, fork, early } });
    CHECK(placement.setups.size() == 2);
    CHECK(placement.numberOfTeardowns == 2);
    CHECK(caller->at(0).kind.opcode == EnterFrame);
    checkEdgeBlock(fork->successorBlock(0), EnterFrame, inside);
    CHECK(fork->successor(0).frequency() == B3::FrequencyClass::Rare);
    checkEdgeBlock(inside->successorBlock(1), LeaveFrame, early);
    CHECK(inside->successor(1).frequency() == B3::FrequencyClass::Normal);
    graph.checkRun({ .first = 1, .second = 1, .result = 13, .numberOfCalls = 2 });
    graph.checkRun({ .first = 1, .second = 0, .result = 21, .numberOfCalls = 2 });
    graph.checkRun({ .first = 0, .second = 1, .result = 14, .numberOfCalls = 1 });
    graph.checkRun({ .first = 0, .second = 0, .result = 18 });
}

void testOopsGetsNoTeardown()
{
    FrameGraph graph;
    BasicBlock* done = graph.addReturn(1);
    BasicBlock* trap = graph.code.addBlock();
    appendBranchTest(graph.entry, graph.first, rarely(trap), done);
    appendCall(graph.proc, trap);
    trap->append(Oops, nullptr);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 0 }, .notSetUpAtHead = { graph.entry, done } });
    CHECK(trap->at(0).kind.opcode == EnterFrame);
    graph.checkRun({ .first = 0, .result = 1 });
}

void testFrameRanges()
{
    RareCallGraph graph;
    prepare(graph.proc);
    ObservedCode observed = generateObservedCode(graph.proc);
    const Vector<FrameRange>& frameRanges = graph.code.frameRanges();
    CHECK(!frameRanges.isEmpty());
    for (size_t i = 0; i < frameRanges.size(); ++i) {
        CHECK(CCallHelpers::differenceBetween(frameRanges[i].begin, frameRanges[i].end) >= 0);
        if (i)
            CHECK(CCallHelpers::differenceBetween(frameRanges[i - 1].end, frameRanges[i].begin) >= 0);
    }
    for (const InstRange& inst : observed.ranges) {
        if (inst.begin == inst.end)
            continue;
        bool isInRange = isInFrameRange(graph.code, observed.start, inst.begin);
        CHECK(isInFrameRange(graph.code, observed.start, inst.end - 1) == isInRange);
        if (inst.opcode == Patch)
            CHECK(isInRange);
        if (isReturn(inst.opcode) || inst.opcode == EnterFrame || inst.opcode == LeaveFrame)
            CHECK(!isInRange);
    }
    graph.generated = WTF::move(observed.generated);
    graph.checkRuns();
}

struct LatePathRecord {
    bool frameIsSetUp { false };
    MacroAssembler::Label label;
};

static B3::PatchpointValue* recordLatePath(B3::PatchpointValue* patchpoint, LatePathRecord& record)
{
    patchpoint->setGenerator([&record](CCallHelpers&, const B3::StackmapGenerationParams& params) {
        GenerationContext& context = params.context();
        params.addLatePath([&record, &context](CCallHelpers& jit) {
            record = { context.frameIsSetUp, jit.label() };
            jit.breakpoint();
        });
    });
    return patchpoint;
}

void testLatePathsKeepFrameState()
{
    FrameGraph graph;
    LatePathRecord withoutFrame;
    LatePathRecord withFrame;
    BasicBlock* join = graph.addReturn(2);
    BasicBlock* rare = graph.code.addBlock();
    graph.entry->append(Patch, recordLatePath(addPatchpointWithoutFrame(graph.proc), withoutFrame), graph.special);
    appendBranchTest(graph.entry, graph.first, rarely(rare), join);
    rare->append(Patch, recordLatePath(addPatchpoint(graph.proc), withFrame), graph.special);
    graph.appendAddAndJump(rare, 1, join);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, join } });
    ObservedCode observed = generateObservedCode(graph.proc);
    CHECK(withFrame.frameIsSetUp);
    CHECK(isInFrameRange(graph.code, observed.start, CCallHelpers::differenceBetween(observed.start, withFrame.label)));
    CHECK(!withoutFrame.frameIsSetUp);
    CHECK(!isInFrameRange(graph.code, observed.start, CCallHelpers::differenceBetween(observed.start, withoutFrame.label)));
    graph.generated = WTF::move(observed.generated);
    graph.checkRun({ .first = 0, .result = 2 });
    graph.checkRun({ .first = 1, .result = 3 });
}

void testStackCheckPerSetup()
{
    TwoRareCallsGraph graph;
    Vector<StackCheckSite> sites;
    unsigned numberOfLatePathsWithFrame = 0;
    graph.code.setStackCheckGenerator(createSharedTask<StackCheckGeneratorFunction>([&](CCallHelpers&, GenerationContext& context, const StackCheckSite& site) {
        sites.append(site);
        context.addLatePath(B3::Origin(), [&numberOfLatePathsWithFrame, &context](CCallHelpers&) {
            numberOfLatePathsWithFrame += context.frameIsSetUp;
        });
    }));

    prepare(graph.proc);
    graph.checkRuns();
    CHECK(sites.size() == 2);
    for (const StackCheckSite& site : sites) {
        CHECK(site.bytesAllocatedBelowFrameRecord == graph.code.frameSize());
        CHECK(site.bytesBelowFrameRecord == graph.code.frameSize());
    }
    CHECK(numberOfLatePathsWithFrame == 2);
}

static B3::PatchpointValue* recordUnavailableRegisters(B3::PatchpointValue* patchpoint, RegisterSet& result)
{
    patchpoint->setGenerator([&result](CCallHelpers&, const B3::StackmapGenerationParams& params) {
        result = params.unavailableRegisters();
    });
    return patchpoint;
}

void testUnavailableRegistersWithoutFrame()
{
    FrameGraph graph;
    Tmp saved(GPRInfo::regCS0);
    RegisterSet unavailableWithoutFrame;
    RegisterSet unavailableWithFrame;
    BasicBlock* rare = graph.code.addBlock();
    BasicBlock* join = graph.code.addBlock();
    appendBranchTest(graph.entry, graph.first, rarely(rare), join);
    rare->append(Patch, recordUnavailableRegisters(addPatchpoint(graph.proc), unavailableWithFrame), graph.special);
    rare->append(Move, nullptr, graph.second, saved);
    graph.appendCallAndReload(rare);
    graph.appendAdd(rare, saved);
    appendJump(rare, join);
    join->append(Patch, recordUnavailableRegisters(addPatchpointWithoutFrame(graph.proc), unavailableWithoutFrame), graph.special);
    graph.appendAddAndReturn(join, 1);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, join } });
    graph.checkRun({ .first = 0, .second = 5, .result = 1 });
    graph.checkRun({ .first = 1, .second = 5, .result = 6, .numberOfCalls = 1 });
    CHECK(graph.code.calleeSaveRegisters().contains(GPRInfo::regCS0, IgnoreVectors));
    CHECK(unavailableWithoutFrame.subsumes(RegisterSet::calleeSaveRegisters()));
    CHECK(!unavailableWithFrame.contains(GPRInfo::regCS0, IgnoreVectors));
}

void testMergeKeepsFrameStates()
{
    B3::Policy policy = shrinkWrappingPolicy();
    policy.air.layout.mergesIdenticalBlocks = true;
    FrameGraph graph(policy);
    auto addTrap = [&] {
        BasicBlock* trap = graph.code.addBlock();
        trap->append(Oops, nullptr);
        return rarely(trap);
    };
    BasicBlock* afterCall = graph.addBranch(4, graph.first, addTrap(), graph.addReturn(8));
    BasicBlock* caller = graph.addBranch(0, graph.first, addTrap(), afterCall, Calls::Yes);
    BasicBlock* fork = graph.addBranch(2, graph.second, rarely(caller), graph.addReturn(16));
    BasicBlock* next = graph.addBranch(1, graph.first, addTrap(), fork);
    appendBranchTest(graph.entry, graph.first, addTrap(), next);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .setUpAtHead = { afterCall }, .notSetUpAtHead = { graph.entry, next, fork } });
    unsigned numberOfTrapsWithFrame = 0;
    for (BasicBlock* block : graph.code)
        numberOfTrapsWithFrame += block->last().kind.opcode == Oops && stateAtHead(graph.code, block) == FrameState::SetUp;
    CHECK(numberOfTrapsWithFrame == 1);
    CHECK(numberOfInsts(graph.code, Oops) == 2);
    graph.checkRun({ .first = 0, .second = 0, .result = 19 });
    graph.checkRun({ .first = 0, .second = 1, .result = 15, .numberOfCalls = 1 });
}

#if CPU(X86_64)
void testDuplicateReturnsWithFrames()
{
    for (bool hasSaves : { false, true }) {
        B3::Policy policy = shrinkWrappingPolicy();
        policy.air.layout.duplicatesReturns = true;
        FrameGraph graph(policy);
        Tmp saved(GPRInfo::regCS0);
        BasicBlock* left = graph.code.addBlock();
        BasicBlock* right = graph.code.addBlock();
        BasicBlock* exit = graph.code.addBlock();
        if (hasSaves)
            graph.entry->append(Move, nullptr, graph.second, saved);
        graph.appendCallAndReload(graph.entry);
        appendBranchTest(graph.entry, graph.first, left, right);
        left->append(Move, nullptr, hasSaves ? Arg(saved) : Arg::imm(1), returnTmp());
        appendJump(left, exit);
        right->append(Move, nullptr, Arg::imm(2), returnTmp());
        appendJump(right, exit);
        appendReturn(exit);

        unsigned expectedNumberOfReturns = hasSaves ? 1 : 2;
        graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathNeedsFrame, .sites = { 1, expectedNumberOfReturns } });
        CHECK(graph.code.calleeSaveRegisterAtOffsetList().registerCount() == hasSaves);
        CHECK(numberOfInsts(graph.code, Ret64) == expectedNumberOfReturns);
        graph.checkRun({ .first = 1, .second = 9, .result = hasSaves ? 9 : 1, .numberOfCalls = 1 });
        graph.checkRun({ .first = 0, .second = 9, .result = 2, .numberOfCalls = 1 });
    }
}
#endif

static void checkFrameStateViolation(Code& code, std::optional<FrameStateViolation::Kind> kind, BasicBlock* block, unsigned instIndex)
{
    std::optional<FrameStateViolation> violation = findFrameStateViolation(code);
    CHECK(violation.has_value() == kind.has_value());
    if (!violation)
        return;
    CHECK(violation->kind == *kind);
    CHECK(violation->block == block);
    CHECK(violation->instIndex == instIndex);
}

void testFrameStateViolations()
{
    using Kind = FrameStateViolation::Kind;
    struct StraightLineCase {
        std::optional<Kind> kind;
        Vector<Inst> insts;
        unsigned instIndex;
    };
    const Inst enterFrame(EnterFrame, nullptr, Arg::imm(0));
    const Inst leaveFrame(LeaveFrame, nullptr);
    const Inst storeCallArgument(Move, nullptr, returnTmp(), Arg::callArg(0));
    const Inst loadIncomingArgument(Move, nullptr, Arg::addr(Tmp(GPRInfo::callFrameRegister), static_cast<int32_t>(sizeof(CallerFrameAndPC))), returnTmp());
    const Inst storeOutsideFrame(Move, nullptr, returnTmp(), Arg::addr(argumentTmp(3)));
    const StraightLineCase cases[] = {
        { std::nullopt, { enterFrame, storeCallArgument, loadIncomingArgument, leaveFrame }, 0 },
        { Kind::FrameSetUpTwice, { enterFrame, enterFrame, leaveFrame }, 1 },
        { Kind::FrameTornDownTwice, { enterFrame, leaveFrame, leaveFrame }, 2 },
        { Kind::ReturnsWithFrame, { enterFrame }, 1 },
        { Kind::NeedsFrame, { enterFrame, leaveFrame, storeCallArgument }, 2 },
        { Kind::NeedsFrame, { loadIncomingArgument }, 0 },
    };
    for (const StraightLineCase& testCase : cases) {
        FrameGraph graph;
        for (const Inst& inst : testCase.insts)
            graph.entry->appendInst(inst);
        appendReturn(graph.entry);
        checkFrameStateViolation(graph.code, testCase.kind, graph.entry, testCase.instIndex);
    }

    FrameGraph graph;
    BasicBlock* withFrame = graph.code.addBlock();
    BasicBlock* merge = graph.code.addBlock();
    appendBranchTest(graph.entry, graph.first, withFrame, merge);
    withFrame->appendInst(enterFrame);
    appendJump(withFrame, merge);
    merge->append(Oops, nullptr);
    checkFrameStateViolation(graph.code, Kind::StatesDifferAtMerge, merge, 0);

    struct WriteCase {
        B3::Policy policy;
        std::optional<Kind> kind;
        Vector<Inst> insts;
        unsigned instIndex;
    };
    const WriteCase writeCases[] = {
        { shrinkWrappingPolicy(), std::nullopt, { storeOutsideFrame, enterFrame, leaveFrame }, 0 },
        { policyThatTestsFrames(), Kind::WritesAheadOfFrame, { storeOutsideFrame, enterFrame, leaveFrame }, 1 },
        { policyThatTestsFrames(), std::nullopt, { enterFrame, storeOutsideFrame, leaveFrame, storeOutsideFrame }, 0 },
        { policyThatTestsFrames(), std::nullopt, { enterFrame, leaveFrame, storeOutsideFrame, enterFrame, leaveFrame }, 0 },
    };
    for (const WriteCase& testCase : writeCases) {
        FrameGraph written(testCase.policy);
        for (const Inst& inst : testCase.insts)
            written.entry->appendInst(inst);
        appendReturn(written.entry);
        checkFrameStateViolation(written.code, testCase.kind, written.entry, testCase.instIndex);
    }
}

void testNothingIsWrittenAheadOfTestedFrame()
{
    {
        EarlyExitGraph graph(policyThatTestsFrames());
        graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, graph.early } });
        graph.checkRuns();
    }

    auto checkWriteInEntry = [](const B3::Policy& policy, bool coversEntry) {
        FrameGraph graph(policy);
        BasicBlock* early = graph.addReturn(1);
        BasicBlock* caller = graph.addReturn(2, Calls::Yes);
        graph.appendAddAndBranch(graph.entry, 4, graph.first, caller, early);
        if (coversEntry) {
            graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathNeedsFrame, .sites = { 1, 2 }, .setUpAtHead = { early, caller } });
            CHECK(graph.entry->at(0).kind.opcode == EnterFrame);
        } else
            graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, early } });
        graph.checkRun({ .first = 0, .result = 5 });
        graph.checkRun({ .first = 1, .result = 6, .numberOfCalls = 1 });
    };
    checkWriteInEntry(shrinkWrappingPolicy(), false);
    checkWriteInEntry(policyThatTestsFrames(), true);
    checkWriteInEntry(policyThatTestsFrames(4096), false);

    {
        B3::Policy policy = policyThatTestsFrames();
        policy.validation.setsUpFrameBehindWrites = true;
        FrameGraph graph(policy);
        BasicBlock* early = graph.addReturn(1);
        BasicBlock* caller = graph.addReturn(2, Calls::Yes);
        graph.appendAddAndBranch(graph.entry, 4, graph.first, caller, early);
        graph.code.resetReachability();
        if (!shouldValidateIRAtEachPhase(graph.proc)) {
            placeFrame(graph.code, allocateStackByGraphColoring(graph.code));
            checkFrameStateViolation(graph.code, FrameStateViolation::Kind::WritesAheadOfFrame, caller, 0);
        }
    }

    // The write stands on a way that sets up no frame behind it.
    {
        FrameGraph graph(policyThatTestsFrames());
        BasicBlock* early = graph.addReturn(1);
        BasicBlock* caller = graph.addReturn(2, Calls::Yes);
        appendBranchTest(graph.entry, graph.first, caller, early);
        graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, early } });
        graph.checkRun({ .first = 0, .result = 1 });
        graph.checkRun({ .first = 1, .result = 2, .numberOfCalls = 1 });
    }

    // The frame begins in front of the first of two writes, not between them.
    {
        FrameGraph graph(policyThatTestsFrames());
        BasicBlock* early = graph.addReturn(1);
        BasicBlock* exit = graph.addReturn(8);
        BasicBlock* caller = graph.addReturn(2, Calls::Yes);
        BasicBlock* writer = graph.addBranch(4, graph.second, caller, exit);
        appendBranchTest(graph.entry, graph.first, writer, early);
        graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 2 }, .setUpAtHead = { caller, exit }, .notSetUpAtHead = { graph.entry, early } });
        CHECK(writer->at(0).kind.opcode == EnterFrame);
        graph.checkRun({ .first = 0, .result = 1 });
        graph.checkRun({ .first = 1, .second = 0, .result = 12 });
        graph.checkRun({ .first = 1, .second = 1, .result = 6, .numberOfCalls = 1 });
    }
}

static B3::Policy frameSplittingPolicy(bool splitsAroundFrame = true)
{
    B3::Policy policy = shrinkWrappingPolicy();
    enableAllAllocatorFields(policy.air);
    policy.air.splitting.splitsAroundFrame = splitsAroundFrame;
    return policy;
}

// first ? calls and returns kept + 2 : returns kept + 1, where kept is a Tmp that holds `second` from the entry on.
struct KeptValueGraph : FrameGraph {
    explicit KeptValueGraph(const B3::Policy& policy)
        : FrameGraph(policy)
    {
        entry->append(Move, nullptr, second, kept);
    }

    void appendReturnOfKept(BasicBlock* block, int32_t addend)
    {
        block->append(Move, nullptr, kept, returnTmp());
        block->append(Add64, nullptr, Arg::imm(addend), returnTmp());
        appendReturn(block);
    }

    Tmp kept { code.newTmp(GP) };
};

void testValueKeptAcrossCallsLeavesEarlyExitWithoutFrame()
{
    for (bool splitsAroundFrame : { true, false }) {
        KeptValueGraph graph(frameSplittingPolicy(splitsAroundFrame));
        BasicBlock* early = graph.code.addBlock();
        BasicBlock* caller = graph.code.addBlock();
        appendBranchTest(graph.entry, graph.first, caller, early);
        graph.appendReturnOfKept(early, 1);
        graph.appendCallAndReload(caller);
        graph.appendCallAndReload(caller);
        graph.appendReturnOfKept(caller, 2);

        if (splitsAroundFrame)
            graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, early } });
        else
            graph.prepareAndCheck({ .coverage = FrameCoverage::EveryPathNeedsFrame, .sites = { 1, 2 } });
        graph.checkRun({ .first = 0, .second = 40, .result = 41 });
        graph.checkRun({ .first = 1, .second = 40, .result = 42, .numberOfCalls = 2 });
    }
}

void testValueKeptThroughCallsReturnsToItsTmp()
{
    KeptValueGraph graph(frameSplittingPolicy());
    BasicBlock* caller = graph.code.addBlock();
    BasicBlock* join = graph.code.addBlock();
    appendBranchTest(graph.entry, graph.first, caller, join);
    graph.appendCallAndReload(caller);
    caller->append(Add64, nullptr, Arg::imm(100), graph.kept);
    graph.appendCallAndReload(caller);
    appendJump(caller, join);
    graph.appendReturnOfKept(join, 1);

    graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, join } });
    graph.checkRun({ .first = 0, .second = 40, .result = 41 });
    graph.checkRun({ .first = 1, .second = 40, .result = 141, .numberOfCalls = 2 });
}

// With `kept`, which lives through it without being mentioned, the arm holds one value more than the registers that need no saving, or exactly as many.
void testValueKeptThroughCrowdedBlockLeavesOtherPathWithoutFrame()
{
    for (bool isCrowded : { true, false }) {
        KeptValueGraph graph(frameSplittingPolicy());
        Code& code = graph.code;
        BasicBlock* arm = code.addBlock();
        BasicBlock* join = code.addBlock();
        appendBranchTest(graph.entry, graph.first, arm, join);
        unsigned numberOfValues = 0;
        for (Reg reg : code.regsInPriorityOrder(GP))
            numberOfValues += !RegisterSet::calleeSaveRegisters().contains(reg, IgnoreVectors);
        numberOfValues -= !isCrowded;
        Vector<Tmp> values;
        for (unsigned i = 0; i < numberOfValues; ++i) {
            values.append(code.newTmp(GP));
            arm->append(Move, nullptr, graph.addressOf(FrameGraph::Word::Second), values.last());
        }
        for (Tmp value : values)
            graph.appendAdd(arm, value);
        appendJump(arm, join);
        graph.appendAdd(join, graph.kept);
        graph.appendReturnOfSum(join);
        code.resetReachability();

        if (!isCrowded) {
            CHECK(!splitLiveRangesAroundFrame(code));
            continue;
        }
        graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, join } });
        graph.checkRun({ .first = 0, .second = 3, .result = 3 });
        graph.checkRun({ .first = 1, .second = 3, .result = 3 + 3 * static_cast<int64_t>(numberOfValues) });
    }
}

// Random control flow over values that are read and changed on both sides of the frame's border: calls, crowded blocks, rare edges and a loop.
struct RandomFrameBorders {
    struct Step {
        enum class Kind : uint8_t { Add, Call, Crowd };
        Kind kind;
        unsigned target { 0 };
        unsigned source { 0 };
    };
    struct Block {
        Vector<Step> steps;
        unsigned ifSet { 0 };
        unsigned ifClear { 0 };
        unsigned pathBit { 0 };
        bool isLoopLatch { false };
    };
    static constexpr unsigned numberOfArguments = 4;
    static constexpr unsigned numberOfLoopIterations = 3;

    explicit RandomFrameBorders(unsigned seed)
        : random(seed)
    {
        unsigned numberOfBlocks = 3 + random.getUint32(10);
        numberOfValues = 2 + random.getUint32(8);
        numberOfDoubles = random.getUint32(4);
        blocks.grow(numberOfBlocks);
        unsigned latch = random.getUint32(2) ? 1 + random.getUint32(numberOfBlocks - 2) : numberOfBlocks;
        for (unsigned i = 0; i + 1 < numberOfBlocks; ++i) {
            Block& block = blocks[i];
            bool mayNeedFrame = i && random.getUint32(3);
            unsigned numberOfSteps = random.getUint32(5);
            for (unsigned step = 0; step < numberOfSteps; ++step) {
                Step::Kind kind = Step::Kind::Add;
                if (mayNeedFrame && !random.getUint32(3))
                    kind = random.getUint32(4) ? Step::Kind::Call : Step::Kind::Crowd;
                block.steps.append({ kind, random.getUint32(numberOfValues), random.getUint32(numberOfValues) });
            }
            auto laterBlock = [&] {
                return i + 1 + random.getUint32(numberOfBlocks - i - 1);
            };
            block.ifClear = laterBlock();
            block.isLoopLatch = i == latch;
            block.ifSet = block.isLoopLatch ? 1 + random.getUint32(i) : laterBlock();
            block.pathBit = random.getUint32(8);
        }
    }

    uint64_t run(uint64_t path, const std::array<uint64_t, numberOfArguments>& arguments) const
    {
        Vector<uint64_t> values;
        for (unsigned i = 0; i < numberOfValues; ++i)
            values.append(arguments[i % numberOfArguments] + i);
        Vector<double> doubles;
        for (unsigned i = 0; i < numberOfDoubles; ++i)
            doubles.append(static_cast<double>(static_cast<int64_t>(arguments[i % numberOfArguments])));
        uint64_t counter = numberOfLoopIterations;
        unsigned index = 0;
        while (index + 1 < blocks.size()) {
            const Block& block = blocks[index];
            for (const Step& step : block.steps) {
                if (step.kind == Step::Kind::Add) {
                    values[step.target] += values[step.source];
                    if (numberOfDoubles)
                        doubles[step.target % numberOfDoubles] += doubles[step.source % numberOfDoubles];
                } else if (step.kind == Step::Kind::Crowd)
                    values[step.target] += crowdSize * values[step.source] + crowdSize * (crowdSize - 1) / 2;
            }
            if (block.isLoopLatch)
                index = counter && --counter ? block.ifSet : block.ifClear;
            else
                index = path >> block.pathBit & 1 ? block.ifSet : block.ifClear;
        }
        uint64_t sum = 0;
        for (uint64_t value : values)
            sum = sum * 3 + value;
        for (double value : doubles)
            sum = sum * 3 + std::bit_cast<uint64_t>(value);
        return sum;
    }

    void build(B3::Procedure& proc)
    {
        proc.setOptLevel(2);
        Code& code = proc.code();
        crowdSize = code.regsInPriorityOrder(GP).size() + 1;
        Vector<BasicBlock*> airBlocks;
        for (unsigned i = 0; i < blocks.size(); ++i)
            airBlocks.append(code.addBlock(i ? 1.0 / 64 : 1));
        Tmp path = code.newTmp(GP);
        Tmp counter = code.newTmp(GP);
        Vector<Tmp> values;
        airBlocks[0]->append(Move, nullptr, argumentTmp(numberOfArguments), path);
        airBlocks[0]->append(Move, nullptr, Arg::imm(numberOfLoopIterations), counter);
        for (unsigned i = 0; i < numberOfValues; ++i) {
            values.append(code.newTmp(GP));
            airBlocks[0]->append(Move, nullptr, argumentTmp(i % numberOfArguments), values[i]);
            airBlocks[0]->append(Add64, nullptr, Arg::imm(i), values[i]);
        }
        Vector<Tmp> doubles;
        for (unsigned i = 0; i < numberOfDoubles; ++i) {
            doubles.append(code.newTmp(FP));
            airBlocks[0]->append(ConvertInt64ToDouble, nullptr, argumentTmp(i % numberOfArguments), doubles[i]);
        }
        for (unsigned i = 0; i + 1 < blocks.size(); ++i) {
            const Block& block = blocks[i];
            BasicBlock* airBlock = airBlocks[i];
            for (const Step& step : block.steps) {
                switch (step.kind) {
                case Step::Kind::Add:
                    airBlock->append(Add64, nullptr, values[step.source], values[step.target]);
                    if (numberOfDoubles) {
                        Tmp target = doubles[step.target % numberOfDoubles];
                        airBlock->append(AddDouble, nullptr, doubles[step.source % numberOfDoubles], target, target);
                    }
                    break;
                case Step::Kind::Call:
                    appendCall(proc, airBlock);
                    break;
                case Step::Kind::Crowd: {
                    Vector<Tmp> crowd;
                    for (unsigned k = 0; k < crowdSize; ++k) {
                        crowd.append(code.newTmp(GP));
                        airBlock->append(Move, nullptr, values[step.source], crowd[k]);
                        airBlock->append(Add64, nullptr, Arg::imm(k), crowd[k]);
                    }
                    for (Tmp tmp : crowd)
                        airBlock->append(Add64, nullptr, tmp, values[step.target]);
                    break;
                }
                }
            }
            FrequentedBlock ifSet(airBlocks[block.ifSet], random.getUint32(4) ? B3::FrequencyClass::Normal : B3::FrequencyClass::Rare);
            FrequentedBlock ifClear(airBlocks[block.ifClear], random.getUint32(4) ? B3::FrequencyClass::Normal : B3::FrequencyClass::Rare);
            if (block.isLoopLatch) {
                BasicBlock* decrement = code.addBlock();
                appendBranchTest(airBlock, counter, decrement, ifClear);
                decrement->append(Sub64, nullptr, Arg::imm(1), counter);
                appendBranchTest(decrement, counter, ifSet, ifClear);
                continue;
            }
            Tmp mask = code.newTmp(GP);
            airBlock->append(Move, nullptr, Arg::imm(1 << block.pathBit), mask);
            airBlock->append(BranchTest64, nullptr, Arg::resCond(MacroAssembler::NonZero), path, mask);
            airBlock->setSuccessors(ifSet, ifClear);
        }
        Tmp sum = code.newTmp(GP);
        Tmp previousSum = code.newTmp(GP);
        BasicBlock* last = airBlocks.last();
        last->append(Move, nullptr, Arg::imm(0), sum);
        auto appendToSum = [&](Tmp value) {
            last->append(Move, nullptr, sum, previousSum);
            last->append(Add64, nullptr, previousSum, sum);
            last->append(Add64, nullptr, previousSum, sum);
            last->append(Add64, nullptr, value, sum);
        };
        for (Tmp value : values)
            appendToSum(value);
        for (Tmp value : doubles) {
            Tmp bits = code.newTmp(GP);
            last->append(MoveDoubleTo64, nullptr, value, bits);
            appendToSum(bits);
        }
        last->append(Move, nullptr, sum, returnTmp());
        appendReturn(last);
    }

    WeakRandom random;
    Vector<Block> blocks;
    unsigned numberOfValues { 0 };
    unsigned numberOfDoubles { 0 };
    uint64_t crowdSize { 0 };
};

void testValuesSurviveRandomFrameBorders()
{
    unsigned numberOfSplitFunctions = 0;
    for (unsigned seed = 1; seed <= 400; ++seed) {
        B3::Policy policy = frameSplittingPolicy();
        policy.air.frame.mayPushCalleeSaves = seed & 1;
        policy.air.frame.mayPushSpillsAtFrameSetup = seed & 1;
        policy.air.frame.mayPlaceSlotsAboveFrameRecord = seed & 2;
        policy.air.frame.ordersSlotsByUse = seed & 4;
        policy.air.frame.prefersStackPointerAddresses = seed & 4;
        B3::Procedure proc(policy);
        RandomFrameBorders program(seed);
        program.build(proc);
        prepare(proc);
        numberOfSplitFunctions += proc.usesShuffle();
        GeneratedCode generated = generateCode(proc);
        std::array<uint64_t, RandomFrameBorders::numberOfArguments> arguments { 1000003, 2000029, 3000017, 4000037 };
        for (uint64_t path : { 0ull, 255ull, 85ull, 170ull, 51ull, 204ull, 15ull, 240ull, 1ull + seed % 254 }) {
            int64_t actual = invoke<int64_t>(*generated.compilation, static_cast<int64_t>(arguments[0]), static_cast<int64_t>(arguments[1]),
                static_cast<int64_t>(arguments[2]), static_cast<int64_t>(arguments[3]), static_cast<int64_t>(path));
            CHECK(static_cast<uint64_t>(actual) == program.run(path, arguments));
        }
    }
    CHECK(numberOfSplitFunctions >= 40);
}

void testLiveRangesAreNotSplitWithoutBorderToCross()
{
    enum class Shape : uint8_t { CallInEntry, NoCall, RareCall, NothingLive };
    for (Shape shape : { Shape::CallInEntry, Shape::NoCall, Shape::RareCall, Shape::NothingLive }) {
        KeptValueGraph graph(frameSplittingPolicy());
        BasicBlock* early = graph.code.addBlock();
        BasicBlock* other = graph.code.addBlock();
        if (shape == Shape::CallInEntry)
            graph.appendCallAndReload(graph.entry);
        appendBranchTest(graph.entry, graph.first, shape == Shape::RareCall ? rarely(other) : FrequentedBlock(other), early);
        graph.appendReturnOfKept(early, 1);
        if (shape != Shape::NoCall)
            graph.appendCallAndReload(other);
        if (shape == Shape::NothingLive)
            graph.appendReturnOfSum(other);
        else
            graph.appendReturnOfKept(other, 2);

        graph.code.resetReachability();
        unsigned numberOfTmps = graph.code.numTmps(GP);
        CHECK(!splitLiveRangesAroundFrame(graph.code));
        CHECK(graph.code.numTmps(GP) == numberOfTmps);
        CHECK(!numberOfInsts(graph.code, Shuffle));
    }
}

// Each block of the row hands one value to the next, so there are as many Tmps as blocks, and `kept` lives through all of them.
void testLiveRangeSplitInFunctionWithManyBlocksAndTmps()
{
    constexpr unsigned numberOfBlocksInRow = 9000;
    static_assert(numberOfBlocksInRow * (numberOfBlocksInRow / 64) > B3::AirLimits::maxWordsPerLivenessMatrix);
    for (bool runsEveryPhase : { false, true }) {
        KeptValueGraph graph(frameSplittingPolicy());
        Code& code = graph.code;
        BasicBlock* caller = code.addBlock();
        BasicBlock* join = code.addBlock();
        appendBranchTest(graph.entry, graph.first, caller, join);
        graph.appendCallAndReload(caller);
        appendJump(caller, join);

        BasicBlock* last = join;
        Tmp sum = code.newTmp(GP);
        last->append(Move, nullptr, Arg::imm(0), sum);
        for (unsigned i = 0; i < numberOfBlocksInRow; ++i) {
            BasicBlock* next = code.addBlock();
            appendJump(last, next);
            Tmp nextSum = code.newTmp(GP);
            next->append(Move, nullptr, sum, nextSum);
            next->append(Add64, nullptr, Arg::imm(1), nextSum);
            sum = nextSum;
            last = next;
        }
        last->append(Add64, nullptr, sum, graph.kept);
        graph.appendReturnOfKept(last, 1);

        if (runsEveryPhase) {
            graph.prepareAndCheck({ .coverage = FrameCoverage::SomePaths, .sites = { 1, 1 }, .notSetUpAtHead = { graph.entry, join } });
            graph.checkRun({ .first = 0, .second = 40, .result = 41 + numberOfBlocksInRow });
            graph.checkRun({ .first = 1, .second = 40, .result = 41 + numberOfBlocksInRow, .numberOfCalls = 1 });
            continue;
        }
        code.resetReachability();
        unsigned numberOfTmps = code.numTmps(GP);
        CHECK(splitLiveRangesAroundFrame(code));
        CHECK(code.numTmps(GP) == numberOfTmps + 1);
        CHECK(numberOfInsts(code, Shuffle) == 2);
        Inst& entering = caller->at(0);
        CHECK(entering.kind.opcode == Shuffle && entering.args().size() == 3 && entering.args()[0] == Arg(graph.kept));
        Tmp inRegion = entering.args()[1].tmp();
        Inst& leaving = caller->at(caller->size() - 2);
        CHECK(leaving.kind.opcode == Shuffle && leaving.args().size() == 3 && leaving.args()[0] == Arg(inRegion) && leaving.args()[1] == Arg(graph.kept));
    }
}

void testLiveRangeSplitPutsShufflesOnBorder()
{
    KeptValueGraph graph(frameSplittingPolicy());
    Code& code = graph.code;
    BasicBlock* caller = code.addBlock();
    BasicBlock* secondCaller = code.addBlock();
    BasicBlock* join = code.addBlock();
    appendBranchTest(graph.entry, graph.first, caller, join);
    graph.appendCallAndReload(caller);
    appendBranchTest(caller, graph.first, secondCaller, join);
    graph.appendCallAndReload(secondCaller);
    appendJump(secondCaller, join);
    graph.appendReturnOfKept(join, 1);
    code.resetReachability();

    CHECK(splitLiveRangesAroundFrame(code));
    Inst& entering = caller->at(0);
    CHECK(entering.kind.opcode == Shuffle && entering.args().size() == 3 && entering.args()[0] == Arg(graph.kept));
    Tmp inRegion = entering.args()[1].tmp();
    CHECK(inRegion != graph.kept && !inRegion.isReg());

    Inst& leaving = secondCaller->at(secondCaller->size() - 2);
    CHECK(leaving.kind.opcode == Shuffle && leaving.args()[0] == Arg(inRegion) && leaving.args()[1] == Arg(graph.kept));
    BasicBlock* edge = caller->successorBlock(1);
    CHECK(edge != join && edge->size() == 2 && edge->successorBlock(0) == join);
    CHECK(edge->at(0).kind.opcode == Shuffle && edge->at(0).args()[0] == Arg(inRegion) && edge->at(0).args()[1] == Arg(graph.kept));
    CHECK(graph.entry->successorBlock(1) == join);
}

#if CPU(X86_64)
#define RUN_BUN_SHRINK_WRAP_TESTS_OF_CPU() do { \
        RUN(testDuplicateReturnsWithFrames()); \
    } while (false)
#else
#define RUN_BUN_SHRINK_WRAP_TESTS_OF_CPU() do { } while (false)
#endif

#define RUN_BUN_SHRINK_WRAP_TESTS() do { \
        RUN(testLeafHasNoFrame()); \
        RUN(testPatchpointWithoutFrame()); \
        RUN(testEarlyExitHasNoFrame()); \
        RUN(testValueKeptAcrossCallsLeavesEarlyExitWithoutFrame()); \
        RUN(testValueKeptThroughCallsReturnsToItsTmp()); \
        RUN(testValueKeptThroughCrowdedBlockLeavesOtherPathWithoutFrame()); \
        RUN(testValuesSurviveRandomFrameBorders()); \
        RUN(testLiveRangesAreNotSplitWithoutBorderToCross()); \
        RUN(testLiveRangeSplitInFunctionWithManyBlocksAndTmps()); \
        RUN(testLiveRangeSplitPutsShufflesOnBorder()); \
        RUN(testRareCallRejoins()); \
        RUN(testTwoRareCalls()); \
        RUN(testLikelyCallsShareFrame()); \
        RUN(testLoopWithLikelyCallHoldsFrame()); \
        RUN(testLoopWithRareCall()); \
        RUN(testFrameCertainToFollow()); \
        RUN(testFrameCertainToPrecede()); \
        RUN(testCalleeSaveLiveAcrossBlocks()); \
        RUN(testRareCallSpillsInsideRegion()); \
        RUN(testLiveScratchRegisterGrowsRegion()); \
        RUN(testIncomingArgumentsWithoutFrame()); \
        RUN(testSeveralEntrypoints()); \
        RUN(testLargeFrameCoversEveryPath()); \
        RUN(testShrinkWrappingOff()); \
        RUN(testFrameIsSetUpAtEntry()); \
        RUN(testNothingIsWrittenAheadOfTestedFrame()); \
        RUN(testTerminalPatchSuccessorsKeepFrame()); \
        RUN(testEntryBlockWithPredecessors()); \
        RUN(testCriticalEdgesAreSplit()); \
        RUN(testOopsGetsNoTeardown()); \
        RUN(testFrameRanges()); \
        RUN(testLatePathsKeepFrameState()); \
        RUN(testStackCheckPerSetup()); \
        RUN(testUnavailableRegistersWithoutFrame()); \
        RUN(testMergeKeepsFrameStates()); \
        RUN(testFrameStateViolations()); \
        RUN_BUN_SHRINK_WRAP_TESTS_OF_CPU(); \
    } while (false)
