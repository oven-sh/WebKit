/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

static constexpr unsigned numberOfProgramPairs = 20;
static constexpr unsigned seedsPerSwitch = 3;
static constexpr unsigned numberOfSwitchParts = 4;
static constexpr unsigned switchesPerPart = (numberOfSwitches + numberOfSwitchParts - 1) / numberOfSwitchParts;

static unsigned firstSeedAtDefaultLevel(unsigned numberOfSeeds)
{
    return Options::defaultB3OptLevel() * numberOfSeeds + 1;
}

static void buildAddition(Procedure& proc, int64_t addend)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.ret(root, graph.make(root, Add, x, graph.int64(root, addend)));
}

struct ProgramPair {
    WTF_MAKE_NONCOPYABLE(ProgramPair);

    template<typename Functor>
    ProgramPair(unsigned seed, const Functor& makePolicy, CompilationScratch* secondScratch = nullptr)
        : lowering(state)
        , first(makePolicy(lowering, false))
        , second(makePolicy(lowering, true), secondScratch)
    {
        RandomProgram(first, seed, state, { });
        RandomProgram(second, seed, state, { });
        first.setOptLevel(2);
        second.setOptLevel(2);
    }

    RandomProgramState state;
    ReproducibleRelocationLowering lowering;
    Procedure first;
    Procedure second;
};

struct GeneratedPair {
    Vector<uint8_t> first;
    Vector<uint8_t> second;
};

static GeneratedPair generatePair(Procedure& first, Procedure& second)
{
    GeneratedPair result;
    generateAndLink(first, &result.first);
    generateAndLink(second, &result.second);
    return result;
}

static void checkBytesAreEqual(unsigned seed, const GeneratedPair& bytes)
{
    if (bytes.first == bytes.second)
        return;
    crashLock.lock();
    dataLogLn("The two copies of the random program with seed ", seed, " compiled to different code: ", bytes.first.size(), " and ", bytes.second.size(), " bytes");
    CRASH();
}

static void checkBatchEqualsOneAtATime(CompilationScratch* scratch)
{
    unsigned firstSeed = firstSeedAtDefaultLevel(numberOfProgramPairs);
    Vector<std::unique_ptr<ProgramPair>> pairs;
    for (unsigned index = 0; index < numberOfProgramPairs; ++index) {
        pairs.append(makeUniqueWithoutFastMallocCheck<ProgramPair>(firstSeed + index, [](RelocationLowering& lowering, bool) {
            return randomProgramPolicy(lowering);
        }, scratch));
    }

    Vector<Procedure*> batch;
    for (auto& pair : pairs) {
        prepareForGeneration(pair->first);
        batch.append(&pair->second);
    }
    prepareForGeneration(batch.span());

    Vector<Vector<uint8_t>> bytesBySeed;
    for (unsigned index = 0; index < pairs.size(); ++index) {
        GeneratedPair bytes = generatePair(pairs[index]->first, pairs[index]->second);
        checkBytesAreEqual(firstSeed + index, bytes);
        bytesBySeed.append(WTF::move(bytes.second));
    }
    CHECK(bytesBySeed[0] != bytesBySeed[1]);
}

static void testBatchEqualsOneAtATime()
{
    prepareForGeneration(std::span<Procedure* const>());
    checkBatchEqualsOneAtATime(nullptr);
}

static void testBatchOnOneScratchEqualsOneAtATime()
{
    CompilationScratch scratch;
    checkBatchEqualsOneAtATime(&scratch);
    checkBatchEqualsOneAtATime(&scratch);
}

static void disableEverySwitchExcept(Policy& policy, std::initializer_list<PolicySwitch> enabledSwitches)
{
    for (unsigned i = 0; i < numberOfSwitches; ++i) {
        auto policySwitch = static_cast<PolicySwitch>(i);
        if (std::ranges::find(enabledSwitches, policySwitch) == enabledSwitches.end())
            policy.disable(policySwitch);
    }
}

static GeneratedPair compileAgainstUpstreamPolicy(unsigned seed, bool disablesSwitches)
{
    ProgramPair pair(seed, [&](RelocationLowering& lowering, bool isSecond) {
        Policy policy = Policy::fromProcessOptions();
        if (isSecond) {
            policy = policyWithEverySwitchOn();
            if (disablesSwitches)
                disableEverySwitchExcept(policy, { PolicySwitch::HoistLoopInvariantValues });
        }
        policy.addressing.isPositionIndependent = true;
        policy.addressing.dataSectionSpace = testDataSectionSpace;
        policy.addressing.jumpTableSpace = testJumpTableSpace;
        policy.addressing.relocationLowering = &lowering;
        policy.switchLowering.minCasesForJumpTable = std::numeric_limits<unsigned>::max();
        return policy;
    });
    prepareForGeneration(pair.first);
    prepareForGeneration(pair.second);
    return generatePair(pair.first, pair.second);
}

static void testDefaultPolicyIsUpstream()
{
    Procedure plain;
    Procedure fromOptions(Policy::fromProcessOptions());
    for (Procedure* proc : { &plain, &fromOptions }) {
        buildAddition(*proc, 1);
        prepareForGeneration(*proc);
    }
    GeneratedPair additions = generatePair(plain, fromOptions);
    CHECK(additions.first == additions.second);

    unsigned firstSeed = firstSeedAtDefaultLevel(numberOfProgramPairs);
    unsigned endSeed = firstSeed + numberOfProgramPairs;
    for (unsigned seed = firstSeed; seed < endSeed; ++seed)
        checkBytesAreEqual(seed, compileAgainstUpstreamPolicy(seed, true));

    bool switchesChangeCode = false;
    for (unsigned seed = firstSeed; seed < endSeed && !switchesChangeCode; ++seed) {
        GeneratedPair bytes = compileAgainstUpstreamPolicy(seed, false);
        switchesChangeCode = bytes.first != bytes.second;
    }
    CHECK(switchesChangeCode);
}

static void testFailedProcedureLeavesBatch()
{
    Policy checkingPolicy = policyWith([](Policy& policy) {
        policy.optimization.checksVariablesAreSet = true;
    });
    Procedure first(checkingPolicy);
    Procedure failing(checkingPolicy);
    Procedure last(checkingPolicy);
    buildAddition(first, 1);
    buildAddition(last, 2);

    Graph graph { failing };
    BasicBlock* root = failing.addBlock();
    Value* read = graph.get(root, failing.addVariable(Int64));
    read->setOrigin(numberedOrigin(4));
    graph.ret(root, read);

    Procedure* batch[] = { &first, &failing, &last };
    prepareForGeneration(std::span<Procedure* const>(batch));

    CHECK(hasFailure(failing, FailureKind::VariableReadBeforeSet));
    CHECK(hasNumber(failing.report().failure->origin, 4));
    CHECK(!failing.code().size());

    int64_t argument = 5;
    CHECK_EQ(invoke<int64_t>(*generateAndLink(first), argument), 6);
    CHECK_EQ(invoke<int64_t>(*generateAndLink(last), argument), 7);
}

static Policy policyWithRecordingPhases(Vector<ASCIILiteral>& names, bool failsAfterOptimization)
{
    using Point = ClientPhase::Point;
    return policyWith([&](Policy& policy) {
        auto appendPhase = [&](Point point, ASCIILiteral name, bool fails = false) {
            policy.clientPhases.append(ClientPhase { point, name, createSharedTask<bool(Procedure&)>([&names, name, fails](Procedure& proc) {
                names.append(name);
                if (fails)
                    proc.fail(FailureKind::RelocationNotLowered);
                return false;
            }) });
        };
        appendPhase(Point::AfterOptimization, "after1"_s, failsAfterOptimization);
        appendPhase(Point::BeforeOptimization, "before1"_s);
        appendPhase(Point::BeforeLowerToAir, "lower1"_s);
        appendPhase(Point::BeforeOptimization, "before2"_s);
        appendPhase(Point::AfterOptimization, "after2"_s);
        appendPhase(Point::BeforeLowerToAir, "lower2"_s);
    });
}

static void checkPhaseNames(const Vector<ASCIILiteral>& names, const Vector<ASCIILiteral>& expected)
{
    if (names == expected)
        return;
    crashLock.lock();
    dataLogLn("Expected the client phases ", listDump(expected), " to run but ", listDump(names), " ran");
    CRASH();
}

static void testClientPhasesRunInOrder()
{
    unsigned optLevel = Options::defaultB3OptLevel();
    int64_t argument = 1;
    Vector<ASCIILiteral> names;
    Procedure proc(policyWithRecordingPhases(names, false));
    buildAddition(proc, 1);
    auto code = compileAndCheckReport(proc, optLevel);
    checkPhaseNames(names, { "before1"_s, "before2"_s, "after1"_s, "after2"_s, "lower1"_s, "lower2"_s });
    CHECK_EQ(invoke<int64_t>(*code, argument), 2);

    names.clear();
    Procedure plain;
    buildAddition(plain, 1);
    auto plainCode = compileAndCheckReport(plain, optLevel);
    CHECK(names.isEmpty());
    CHECK_EQ(invoke<int64_t>(*plainCode, argument), 2);
}

static void testClientPhasesStopAfterFailure()
{
    Vector<ASCIILiteral> names;
    Procedure proc(policyWithRecordingPhases(names, true));
    buildAddition(proc, 1);
    prepareForGeneration(proc);
    CHECK(hasFailure(proc, FailureKind::RelocationNotLowered));
    checkPhaseNames(names, { "before1"_s, "before2"_s, "after1"_s });
    CHECK(!proc.code().size());
}

static void appendReturningPatchpoint(Graph& graph, BasicBlock* block, int32_t result)
{
    PatchpointValue* patchpoint = graph.emptyPatchpoint(block);
    patchpoint->effects.terminal = true;
    patchpoint->effects.exitsSideways = true;
    patchpoint->effects.reads = HeapRange::top();
    patchpoint->setGenerator([result](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(CCallHelpers::TrustedImm32(result), GPRInfo::returnValueGPR);
        params.code().emitEpilogue(jit);
    });
}

static void checkSharedExitCopies(bool threadsJumps, unsigned expectedCopies)
{
    unsigned numberOfPatchpoints = 0;
    auto countPatchpoints = [&](Procedure& optimized) {
        numberOfPatchpoints = numberOfValues(optimized, Patchpoint);
    };
    Policy threadingPolicy = policyWith([](Policy& policy) {
        policy.optimization.threadsJumps = true;
        policy.upstream.maxTailDuplicationBlockSize = 1;
    });
    if (!threadsJumps)
        threadingPolicy.disable(PolicySwitch::ThreadJumps);

    Procedure proc(policyWithInspection(threadingPolicy, countPatchpoints));
    Graph graph { proc };
    std::array<BasicBlock*, 3> tests { proc.addBlock(), proc.addBlock(), proc.addBlock() };
    BasicBlock* shared = proc.addBlock();
    BasicBlock* done = proc.addBlock();
    Vector<Value*> operands = cCallArgumentValues<int64_t, int64_t, int64_t>(proc, tests[0]).eager();
    for (unsigned i = 0; i < tests.size(); ++i) {
        BasicBlock* next = i + 1 < tests.size() ? tests[i + 1] : done;
        graph.branch(tests[i], graph.make(tests[i], Equal, operands[i], graph.int64(tests[i], 1)), i ? rarely(shared) : FrequentedBlock(shared), FrequentedBlock(next));
    }
    appendReturningPatchpoint(graph, shared, 99);
    graph.ret(done, graph.int64(done, 1));

    auto code = compileAndCheckReport(proc, 2);
    CHECK_EQ(numberOfPatchpoints, expectedCopies);

    std::array<int64_t, 3> arguments { };
    CHECK_EQ(invoke<int64_t>(*code, arguments[0], arguments[1], arguments[2]), 1);
    for (unsigned i = 0; i < arguments.size(); ++i) {
        arguments = { };
        arguments[i] = 1;
        CHECK_EQ(invoke<int64_t>(*code, arguments[0], arguments[1], arguments[2]), 99);
    }
}

static void testThreadJumpsReplacesTailDuplication()
{
    checkSharedExitCopies(true, 1);
    checkSharedExitCopies(false, 3);
}

static int64_t randomArgument(WeakRandom& random)
{
    if (random.getUint32(2))
        return random.getUint32(100);
    uint64_t highBits = random.getUint32();
    return static_cast<int64_t>(highBits << 32 | random.getUint32());
}

static void checkSwitchAlone(PolicySwitch policySwitch, unsigned seed, RandomProgramVersion& reference)
{
    RandomProgramVersion alone("with one switch alone"_s, [&](RelocationLowering&) {
        Policy policy = policyWithEverySwitchOn();
        disableEverySwitchExcept(policy, { policySwitch, PolicySwitch::HoistLoopInvariantValues });
        policy.validation.level = ValidationLevel::AtEachPhase;
        return policy;
    });
    RandomProgram(alone.proc, seed, alone.state, { });
    alone.code = compileAndCheckReport(alone.proc, 2);

    WeakRandom random(seed);
    for (int64_t limit : { 0, -1, 0 }) {
        Vector<int64_t> memory = randomInitialMemory(random);
        int64_t first = randomArgument(random);
        int64_t second = randomArgument(random);
        reference.call(first, second, memory, limit);
        alone.call(first, second, memory, limit);
        if (alone.agreesWith(reference))
            continue;
        crashLock.lock();
        dataLogLn("With ", nameOf(policySwitch), " as the only switch, the random program with seed ", seed, " ", alone.differenceFrom(reference, first, second));
        CRASH();
    }
}

static void testEachSwitchAlone(unsigned part)
{
    unsigned firstSeed = firstSeedAtDefaultLevel(seedsPerSwitch);
    for (unsigned seed = firstSeed; seed < firstSeed + seedsPerSwitch; ++seed) {
        RandomProgramVersion reference("at level 0"_s);
        RandomProgram(reference.proc, seed, reference.state, { true, true });
        reference.code = compileAndCheckReport(reference.proc, 0);
        for (unsigned i = part * switchesPerPart; i < std::min((part + 1) * switchesPerPart, numberOfSwitches); ++i)
            checkSwitchAlone(static_cast<PolicySwitch>(i), seed, reference);
    }
}

static unsigned g_numberOfCountedRepositories;

static std::unique_ptr<AbstractHeapRepository> createCountedRepository()
{
    ++g_numberOfCountedRepositories;
    return makeUnique<AbstractHeapRepository>();
}

static void testScratchHandsOutDistinctRepositories()
{
    g_numberOfCountedRepositories = 0;
    CompilationScratch plainScratch;
    CompilationScratch countingScratch(createCountedRepository);
    for (CompilationScratch* scratch : { &plainScratch, &countingScratch, &countingScratch }) {
        AbstractHeapRepository* firstRepository = nullptr;
        AbstractHeapRepository* secondRepository = nullptr;
        {
            Procedure first(Policy { }, scratch);
            Procedure second(Policy { }, scratch);
            countingScratch.setHeapRepositoryFactory(createCountedRepository);
            firstRepository = &first.heaps();
            secondRepository = &second.heaps();
            CHECK(firstRepository != secondRepository);
        }

        Procedure third(Policy { }, scratch);
        CHECK(&third.heaps() == firstRepository || &third.heaps() == secondRepository);
        CHECK_EQ(g_numberOfCountedRepositories, scratch == &countingScratch ? 2u : 0u);
    }
}

static HeapRange computeStructureIDRange(Procedure& proc)
{
    proc.heaps().computeRangesAndDecorateInstructions();
    return proc.heaps().JSCell_structureID.range();
}

static MemoryValue* appendWordLoad(Procedure& proc, BasicBlock* block, const int64_t& word)
{
    AbstractHeapRepository& heaps = proc.heaps();
    Value* pointer = block->appendNew<ConstPtrValue>(proc, Origin(), &word);
    MemoryValue* load = block->appendNew<MemoryValue>(proc, Load, Int64, Origin(), pointer);
    heaps.decorateMemory(&heaps.absolute[&word], load);
    return load;
}

static void testScratchRepositoryIsDetached()
{
    CompilationScratch freshScratch;
    Procedure fresh(Policy { }, &freshScratch);
    HeapRange freshStructureIDRange = computeStructureIDRange(fresh);
    HeapRange freshRootRange = fresh.heaps().root.range();

    const int64_t words[2] = { };
    CompilationScratch scratch;
    AbstractHeapRepository* firstRepository = nullptr;
    {
        Procedure first(Policy { }, &scratch);
        AbstractHeapRepository& heaps = first.heaps();
        firstRepository = &heaps;
        BasicBlock* root = first.addBlock();
        MemoryValue* load = appendWordLoad(first, root, words[0]);
        appendWordLoad(first, root, words[1]);
        heaps.computeRangesAndDecorateInstructions();
        HeapRange loadRange = load->range();
        CHECK_EQ(loadRange, heaps.absolute[&words[0]].range());
        CHECK(heaps.absolute.hasTemporaryHeaps());
        CHECK(heaps.root.range() != freshRootRange);

        Procedure second(Policy { }, &scratch);
        CHECK(&second.heaps() != &heaps);
        CHECK_EQ(computeStructureIDRange(second), freshStructureIDRange);
        CHECK_EQ(load->range(), loadRange);
    }

    Procedure next(Policy { }, &scratch);
    CHECK(&next.heaps() == firstRepository);
    CHECK(!next.heaps().absolute.hasTemporaryHeaps());
    CHECK_EQ(computeStructureIDRange(next), freshStructureIDRange);
    CHECK_EQ(next.heaps().root.range(), freshRootRange);
}

static void testArenaReusesFirstChunk()
{
    Policy arenaPolicy = policyWith([](Policy& policy) {
        policy.allocation.usesArena = true;
    });

    CompilationScratch scratch;
    void* address = nullptr;
    {
        Procedure first(arenaPolicy, &scratch);
        address = first.add<Const32Value>(Origin(), 1);
    }
    Procedure second(arenaPolicy, &scratch);
    CHECK(second.add<Const32Value>(Origin(), 1) == address);
    Procedure third(arenaPolicy, &scratch);
    CHECK(third.add<Const32Value>(Origin(), 1) != address);

    Procedure firstWithoutScratch(arenaPolicy);
    Procedure secondWithoutScratch(arenaPolicy);
    buildAddition(firstWithoutScratch, 1);
    buildAddition(secondWithoutScratch, 1);
    int64_t argument = 5;
    CHECK_EQ(invoke<int64_t>(*compileAndCheckReport(firstWithoutScratch, Options::defaultB3OptLevel()), argument), 6);
    CHECK_EQ(invoke<int64_t>(*compileAndCheckReport(secondWithoutScratch, Options::defaultB3OptLevel()), argument), 6);
}

static void checkReadsFollowSets(Procedure& proc)
{
    CHECK(checkVariablesAreSet(proc));
    CHECK(!proc.report().failure);
}

enum class PathsWithSet : uint8_t {
    Left,
    Right,
    Both,
};

static void buildReadAfterDiamond(Procedure& proc, PathsWithSet pathsWithSet)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* left = proc.addBlock();
    BasicBlock* right = proc.addBlock();
    BasicBlock* join = proc.addBlock();
    BasicBlock* tail = proc.addBlock();
    Variable* variable = proc.addVariable(Int64);
    Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
    graph.branch(root, x, left, right);
    if (pathsWithSet != PathsWithSet::Right)
        graph.set(left, variable, x);
    graph.jump(left, join);
    if (pathsWithSet != PathsWithSet::Left)
        graph.set(right, variable, x);
    graph.jump(right, join);
    graph.jump(join, tail);
    Value* read = graph.get(tail, variable);
    read->setOrigin(numberedOrigin(6));
    graph.ret(tail, read);
    proc.resetReachability();
}

static void testCheckVariablesAreSetOnePath()
{
    for (PathsWithSet pathsWithSet : { PathsWithSet::Left, PathsWithSet::Right }) {
        Procedure proc;
        buildReadAfterDiamond(proc, pathsWithSet);
        CHECK(!checkVariablesAreSet(proc));
        CHECK(hasFailure(proc, FailureKind::VariableReadBeforeSet));
        CHECK(hasNumber(proc.report().failure->origin, 6));
    }
}

static void testCheckVariablesAreSetBothPaths()
{
    Procedure proc;
    buildReadAfterDiamond(proc, PathsWithSet::Both);
    checkReadsFollowSets(proc);
}

static void testCheckVariablesAreSetBeforeLoop()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    BasicBlock* header = proc.addBlock();
    BasicBlock* body = proc.addBlock();
    BasicBlock* exit = proc.addBlock();
    Variable* variable = proc.addVariable(Int64);
    graph.set(root, variable, cCallArgumentValues<int64_t>(proc, root)[0]);
    graph.jump(root, header);
    graph.branch(header, graph.get(header, variable), body, exit);
    graph.jump(body, header);
    graph.ret(exit, graph.int64(exit, 0));
    proc.resetReachability();
    checkReadsFollowSets(proc);
}

static void testCheckVariablesAreSetUnreachable()
{
    Procedure proc;
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    Variable* variable = proc.addVariable(Int64);
    graph.ret(root, graph.int64(root, 0));
    proc.resetReachability();

    BasicBlock* unreachable = proc.addBlock();
    graph.ret(unreachable, graph.get(unreachable, variable));
    checkReadsFollowSets(proc);
}

static void testCheckVariablesAreSetWithoutVariables()
{
    Procedure proc;
    buildAddition(proc, 1);
    proc.resetReachability();
    checkReadsFollowSets(proc);
}

static constexpr int64_t pinnedWord = 0x1234567890abcdefll;
static constexpr int64_t pinnedSmallWord = 8;
static constexpr int64_t pinnedMask = 0xf0;

struct PinnedConstantTable {
    int64_t word { pinnedWord };
    int64_t smallWord { pinnedSmallWord };
    int64_t mask { pinnedMask };
};

template<bool isInMemory, bool replacesImmediates>
static std::optional<PinnedConstant> findTestConstant(uint64_t bits)
{
    auto pinnedAt = [](size_t offset) {
        return PinnedConstant { GPRInfo::argumentGPR1, isInMemory ? std::optional<int32_t>(static_cast<int32_t>(offset)) : std::nullopt, replacesImmediates };
    };
    if (bits == static_cast<uint64_t>(pinnedWord))
        return pinnedAt(OBJECT_OFFSETOF(PinnedConstantTable, word));
    if (isInMemory && bits == static_cast<uint64_t>(pinnedSmallWord))
        return pinnedAt(OBJECT_OFFSETOF(PinnedConstantTable, smallWord));
    if (isInMemory && bits == static_cast<uint64_t>(pinnedMask))
        return pinnedAt(OBJECT_OFFSETOF(PinnedConstantTable, mask));
    return std::nullopt;
}

static Policy pinnedConstantPolicy(PinnedConstantFinder finder)
{
    return policyWith([&](Policy& policy) {
        policy.air.registers.pinned.add(GPRInfo::argumentGPR1, IgnoreVectors);
        policy.addressing.namesPinnedRegistersDirectly = true;
        policy.addressing.findPinnedConstant = finder;
    });
}

static bool materializeAndValidate(Procedure& proc)
{
    proc.resetReachability();
    bool changed = materializePinnedConstants(proc);
    validate(proc);
    return changed;
}

static bool isPinnedRegister(Value* value)
{
    auto* argument = value->as<ArgumentRegValue>();
    return argument && argument->argumentReg() == Reg(GPRInfo::argumentGPR1);
}

static void checkReadsPinnedTable(BasicBlock* block, Value* user, unsigned childIndex, Type type, size_t offset)
{
    auto* load = user->child(childIndex)->as<MemoryValue>();
    CHECK(load);
    CHECK(load->opcode() == Load);
    CHECK(load->type() == type);
    CHECK_EQ(load->offset(), static_cast<int32_t>(offset));
    CHECK(isPinnedRegister(load->lastChild()));
    CHECK(!load->controlDependent());
    CHECK(load->readsMutability() == Mutability::Immutable);
    size_t userIndex = block->values().find(user);
    CHECK(userIndex != notFound && userIndex);
    CHECK(block->at(userIndex - 1) == load);
}

enum class ExistingRegisterRead : uint8_t {
    None,
    BeforeUsers,
    AfterUsers,
};

struct PinnedWordArithmetic {
    PinnedWordArithmetic(const Policy& policy, int64_t constant, ExistingRegisterRead existingRead = ExistingRegisterRead::None)
        : proc(policy)
    {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
        if (existingRead == ExistingRegisterRead::BeforeUsers)
            registerRead = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
        sum = graph.make(root, Add, x, graph.int64(root, constant));
        result = graph.make(root, BitXor, sum, graph.int64(root, constant));
        if (existingRead == ExistingRegisterRead::AfterUsers)
            registerRead = root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1);
        graph.ret(root, registerRead ? graph.make(root, Add, result, registerRead) : result);
    }

    static int64_t evaluate(int64_t x, int64_t constant, int64_t addend = 0)
    {
        return static_cast<int64_t>(((static_cast<uint64_t>(x) + static_cast<uint64_t>(constant)) ^ static_cast<uint64_t>(constant)) + static_cast<uint64_t>(addend));
    }

    void checkKeepsConstant(int64_t constant)
    {
        unsigned numberOfValuesBefore = proc.values().size();
        CHECK(!materializeAndValidate(proc));
        CHECK_EQ(proc.values().size(), numberOfValuesBefore);
        CHECK(sum->child(1)->isInt64(constant));
        CHECK(result->child(1)->isInt64(constant));
    }

    Procedure proc;
    Value* sum;
    Value* result;
    Value* registerRead { nullptr };
};

static void checkPinnedWordInRegister(ExistingRegisterRead existingRead)
{
    Policy policy = pinnedConstantPolicy(findTestConstant<false, false>);

    PinnedWordArithmetic inspected(policy, pinnedWord, existingRead);
    CHECK(materializeAndValidate(inspected.proc));
    Value* pinnedRegister = inspected.sum->child(1);
    CHECK(isPinnedRegister(pinnedRegister));
    CHECK(inspected.result->child(1) == pinnedRegister);
    if (existingRead == ExistingRegisterRead::BeforeUsers)
        CHECK(pinnedRegister == inspected.registerRead);
    else
        CHECK(pinnedRegister == inspected.proc.at(0)->at(0));
    CHECK_EQ(numberOfValues(inspected.proc, isPinnedRegister), existingRead == ExistingRegisterRead::AfterUsers ? 2u : 1u);

    PinnedWordArithmetic compiled(policy, pinnedWord, existingRead);
    auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
    for (int64_t registerValue : { pinnedWord, static_cast<int64_t>(5) }) {
        int64_t addend = existingRead == ExistingRegisterRead::None ? 0 : registerValue;
        for (int64_t x : { 0, 41, -1 })
            CHECK_EQ(invoke<int64_t>(*code, x, registerValue), PinnedWordArithmetic::evaluate(x, registerValue, addend));
    }
}

static void testPinnedConstantInRegister()
{
    checkPinnedWordInRegister(ExistingRegisterRead::None);
    checkPinnedWordInRegister(ExistingRegisterRead::BeforeUsers);
    checkPinnedWordInRegister(ExistingRegisterRead::AfterUsers);

    PinnedWordArithmetic withUnknownConstant(pinnedConstantPolicy(findTestConstant<false, false>), pinnedWord + 1);
    withUnknownConstant.checkKeepsConstant(pinnedWord + 1);
}

struct PinnedWordComparisonPair {
    explicit PinnedWordComparisonPair(const Policy& policy)
        : proc(policy)
        , root(proc.addBlock())
    {
        Graph graph { proc };
        Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
        below = graph.make(root, Below, x, graph.int64(root, pinnedWord));
        above = graph.make(root, Above, x, graph.int64(root, pinnedWord));
        graph.ret(root, graph.make(root, Add, below, above));
    }

    Procedure proc;
    BasicBlock* root;
    Value* below;
    Value* above;
};

struct PinnedBitsInBothBanks {
    explicit PinnedBitsInBothBanks(const Policy& policy)
        : proc(policy)
        , root(proc.addBlock())
    {
        Graph graph { proc };
        auto arguments = cCallArgumentValues<int64_t, PinnedConstantTable*, double>(proc, root);
        difference = graph.make(root, BitXor, arguments[0], graph.int64(root, pinnedWord));
        sum = graph.make(root, Add, arguments[2], graph.number(root, std::bit_cast<double>(pinnedWord)));
        graph.ret(root, graph.make(root, Add, sum, graph.make(root, IToD, difference)));
    }

    Procedure proc;
    BasicBlock* root;
    Value* difference;
    Value* sum;
};

static void testPinnedConstantInMemory()
{
    Policy policy = pinnedConstantPolicy(findTestConstant<true, false>);

    PinnedWordComparisonPair inspected(policy);
    CHECK(materializeAndValidate(inspected.proc));
    checkReadsPinnedTable(inspected.root, inspected.below, 1, Int64, OBJECT_OFFSETOF(PinnedConstantTable, word));
    checkReadsPinnedTable(inspected.root, inspected.above, 1, Int64, OBJECT_OFFSETOF(PinnedConstantTable, word));
    CHECK(inspected.below->child(1) != inspected.above->child(1));
    CHECK_EQ(numberOfValues(inspected.proc, isPinnedRegister), 1u);

    PinnedWordComparisonPair compiled(policy);
    auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
    PinnedConstantTable table;
    for (int64_t word : { pinnedWord, static_cast<int64_t>(10) }) {
        table.word = word;
        CHECK_EQ(invoke<int32_t>(*code, word - 1, &table), 1);
        CHECK_EQ(invoke<int32_t>(*code, word, &table), 0);
        CHECK_EQ(invoke<int32_t>(*code, word + 1, &table), 1);
    }

    PinnedBitsInBothBanks inspectedBanks(policy);
    CHECK(materializeAndValidate(inspectedBanks.proc));
    checkReadsPinnedTable(inspectedBanks.root, inspectedBanks.difference, 1, Int64, OBJECT_OFFSETOF(PinnedConstantTable, word));
    checkReadsPinnedTable(inspectedBanks.root, inspectedBanks.sum, 1, Double, OBJECT_OFFSETOF(PinnedConstantTable, word));

    PinnedBitsInBothBanks compiledBanks(policy);
    auto banksCode = compileAndCheckReport(compiledBanks.proc, Options::defaultB3OptLevel());
    table.word = pinnedWord;
    CHECK_EQ(invoke<double>(*banksCode, pinnedWord, &table, 0.0), std::bit_cast<double>(pinnedWord));
    CHECK_EQ(invoke<double>(*banksCode, pinnedWord ^ 2, &table, 1.0), 3.0);
    table.word = std::bit_cast<int64_t>(2.0);
    CHECK_EQ(invoke<double>(*banksCode, table.word, &table, 1.0), 3.0);
    CHECK_EQ(invoke<double>(*banksCode, table.word ^ 4, &table, 1.0), 7.0);
}

struct PinnedBitsAtJoin {
    explicit PinnedBitsAtJoin(const Policy& policy)
        : proc(policy)
    {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        upsilonBlock = proc.addBlock();
        BasicBlock* otherBlock = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t, double>(proc, root);
        Value* other = arguments[2];
        graph.branch(root, arguments[0], upsilonBlock, otherBlock);
        Value* phi = graph.phi(next, Double);
        upsilon = upsilonBlock->appendNew<UpsilonValue>(proc, Origin(), graph.number(upsilonBlock, std::bit_cast<double>(pinnedWord)), phi);
        graph.jump(upsilonBlock, next);
        graph.upsilon(otherBlock, other, phi);
        graph.jump(otherBlock, next);
        graph.ret(next, phi);
    }

    Procedure proc;
    BasicBlock* upsilonBlock;
    Value* upsilon;
};

static void checkReadsPinnedRegisterAsDouble(BasicBlock* block, Value* user, unsigned childIndex)
{
    Value* cast = user->child(childIndex);
    CHECK(cast->opcode() == BitwiseCast);
    CHECK(cast->type() == Double);
    CHECK(isPinnedRegister(cast->child(0)));
    size_t userIndex = block->values().find(user);
    CHECK(userIndex != notFound && userIndex);
    CHECK(block->at(userIndex - 1) == cast);
}

static void testPinnedConstantInRegisterOfOtherBank()
{
    Policy policy = pinnedConstantPolicy(findTestConstant<false, false>);
    int64_t two = std::bit_cast<int64_t>(2.0);

    PinnedBitsInBothBanks inspected(policy);
    CHECK(materializeAndValidate(inspected.proc));
    CHECK(isPinnedRegister(inspected.difference->child(1)));
    checkReadsPinnedRegisterAsDouble(inspected.root, inspected.sum, 1);
    CHECK_EQ(numberOfValues(inspected.proc, isPinnedRegister), 1u);

    PinnedBitsInBothBanks compiled(policy);
    auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
    CHECK_EQ(invoke<double>(*code, pinnedWord, pinnedWord, 0.0), std::bit_cast<double>(pinnedWord));
    CHECK_EQ(invoke<double>(*code, two, two, 1.0), 3.0);
    CHECK_EQ(invoke<double>(*code, two ^ 4, two, 1.0), 7.0);

    PinnedBitsAtJoin inspectedJoin(policy);
    CHECK(materializeAndValidate(inspectedJoin.proc));
    checkReadsPinnedRegisterAsDouble(inspectedJoin.upsilonBlock, inspectedJoin.upsilon, 0);

    PinnedBitsAtJoin compiledJoin(policy);
    auto joinCode = compileAndCheckReport(compiledJoin.proc, Options::defaultB3OptLevel());
    CHECK_EQ(invoke<double>(*joinCode, static_cast<int64_t>(1), two, 5.0), 2.0);
    CHECK_EQ(invoke<double>(*joinCode, static_cast<int64_t>(0), two, 5.0), 5.0);
}

struct PinnedSmallWordUses {
    explicit PinnedSmallWordUses(const Policy& policy)
        : proc(policy)
    {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        upsilonBlock = proc.addBlock();
        BasicBlock* next = proc.addBlock();
        Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
        smallWord = graph.int64(root, pinnedSmallWord);
        sum = graph.make(root, Add, x, smallWord);
        graph.jump(root, upsilonBlock);
        Value* phi = graph.phi(next, Int64);
        upsilon = upsilonBlock->appendNew<UpsilonValue>(proc, Origin(), smallWord, phi);
        graph.jump(upsilonBlock, next);
        graph.ret(next, graph.make(next, Add, sum, phi));
    }

    Procedure proc;
    BasicBlock* upsilonBlock;
    Value* smallWord;
    Value* sum;
    Value* upsilon;
};

static void testPinnedConstantKeepsImmediates()
{
    Policy policy = pinnedConstantPolicy(findTestConstant<true, false>);

    PinnedSmallWordUses inspected(policy);
    CHECK(materializeAndValidate(inspected.proc));
    CHECK(inspected.sum->child(1) == inspected.smallWord);
    checkReadsPinnedTable(inspected.upsilonBlock, inspected.upsilon, 0, Int64, OBJECT_OFFSETOF(PinnedConstantTable, smallWord));
    CHECK(isPinnedRegister(inspected.proc.at(0)->at(0)));

    PinnedSmallWordUses compiled(policy);
    auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
    PinnedConstantTable table;
    CHECK_EQ(invoke<int64_t>(*code, static_cast<int64_t>(3), &table), 3 + 2 * pinnedSmallWord);
}

enum class MaskedOperand : uint8_t {
    Argument,
    LoadWithOneUse,
    LoadWithTwoUses,
};

enum class MaskSelection : uint8_t {
    Immediate,
    MemoryOperand,
    Register,
};

struct PinnedMaskTest {
    PinnedMaskTest(const Policy& policy, MaskedOperand operand, int64_t mask)
        : proc(policy)
        , root(proc.addBlock())
    {
        Graph graph { proc };
        BasicBlock* taken = proc.addBlock();
        BasicBlock* notTaken = proc.addBlock();
        Value* x = cCallArgumentValues<int64_t>(proc, root)[0];
        if (operand != MaskedOperand::Argument)
            x = graph.load(root, Int64, x, 0, firstHeap);
        masked = graph.make(root, BitAnd, x, graph.int64(root, mask));
        graph.branch(root, masked, taken, notTaken);
        Value* takenResult = graph.int64(taken, 1);
        if (operand == MaskedOperand::LoadWithTwoUses)
            takenResult = graph.make(taken, Add, x, takenResult);
        graph.ret(taken, takenResult);
        graph.ret(notTaken, graph.int64(notTaken, 0));
    }

    static int64_t evaluate(MaskedOperand operand, int64_t mask, int64_t x)
    {
        if (!(x & mask))
            return 0;
        return operand == MaskedOperand::LoadWithTwoUses ? x + 1 : 1;
    }

    Procedure proc;
    BasicBlock* root;
    Value* masked;
};

static bool hasBranchTestOnPinnedTable(Procedure& proc)
{
    for (Air::BasicBlock* block : proc.code()) {
        for (Air::Inst& inst : *block) {
            if (inst.kind.opcode != Air::BranchTest64)
                continue;
            for (const Air::Arg& arg : inst.args()) {
                if (arg.isAddr() && arg.base() == Air::Tmp(GPRInfo::argumentGPR1))
                    return true;
            }
        }
    }
    return false;
}

static void checkMaskSelection(MaskedOperand operand, int64_t mask, MaskSelection expectedSelection)
{
    Policy policy = pinnedConstantPolicy(findTestConstant<true, true>);
    bool isReplaced = expectedSelection != MaskSelection::Immediate;

    PinnedMaskTest inspected(policy, operand, mask);
    CHECK_EQ(materializeAndValidate(inspected.proc), isReplaced);
    if (isReplaced)
        checkReadsPinnedTable(inspected.root, inspected.masked, 1, Int64, *findTestConstant<true, true>(static_cast<uint64_t>(mask))->offset);
    else
        CHECK(inspected.masked->child(1)->isInt64(mask));
    if (isX86_64()) {
        lowerToAirForTesting(inspected.proc);
        CHECK_EQ(hasBranchTestOnPinnedTable(inspected.proc), expectedSelection == MaskSelection::MemoryOperand);
    }

    PinnedMaskTest compiled(policy, operand, mask);
    auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
    PinnedConstantTable table;
    for (int64_t x : { 0x10, 0x0f }) {
        int64_t argument = operand == MaskedOperand::Argument ? x : std::bit_cast<int64_t>(&x);
        CHECK_EQ(invoke<int64_t>(*code, argument, &table), PinnedMaskTest::evaluate(operand, mask, x));
    }
}

static void testPinnedConstantReplacesImmediate()
{
    checkMaskSelection(MaskedOperand::Argument, pinnedMask, MaskSelection::MemoryOperand);
    checkMaskSelection(MaskedOperand::LoadWithOneUse, pinnedMask, MaskSelection::Immediate);
    checkMaskSelection(MaskedOperand::LoadWithTwoUses, pinnedMask, MaskSelection::MemoryOperand);
    checkMaskSelection(MaskedOperand::LoadWithOneUse, pinnedWord, MaskSelection::Register);
}

enum class MaskTestUsers : uint8_t {
    Boolean,
    Select,
    TwoBranches,
    BranchAndBoolean,
    BranchAndSelect,
};

enum class MaskSource : uint8_t {
    PinnedConstant,
    MutableWordOfPinnedTable,
};

struct PinnedMaskUsers {
    PinnedMaskUsers(const Policy& policy, MaskTestUsers users, MaskSource source)
        : proc(policy)
    {
        Graph graph { proc };
        BasicBlock* root = proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, PinnedConstantTable*, int64_t>(proc, root);
        Value* x = arguments[0];
        Value* y = arguments[2];
        Value* mask = graph.int64(root, pinnedMask);
        if (source == MaskSource::MutableWordOfPinnedTable)
            mask = graph.load(root, Int64, root->appendNew<ArgumentRegValue>(proc, Origin(), GPRInfo::argumentGPR1), OBJECT_OFFSETOF(PinnedConstantTable, mask), firstHeap);
        Value* masked = graph.make(root, BitAnd, x, mask);
        Value* isSet = graph.make(root, NotEqual, masked, graph.int64(root, 0));
        auto branchToReturns = [&](BasicBlock* block, Value* condition, Value* ifTrue, Value* ifFalse) {
            BasicBlock* taken = proc.addBlock();
            BasicBlock* notTaken = proc.addBlock();
            graph.branch(block, condition, taken, notTaken);
            graph.ret(taken, ifTrue);
            graph.ret(notTaken, ifFalse);
        };
        switch (users) {
        case MaskTestUsers::Boolean:
            graph.ret(root, graph.make(root, ZExt32, isSet));
            return;
        case MaskTestUsers::Select:
            graph.ret(root, graph.select(root, masked, y, x));
            return;
        case MaskTestUsers::TwoBranches: {
            BasicBlock* left = proc.addBlock();
            BasicBlock* right = proc.addBlock();
            Value* sum = graph.make(root, Add, x, y);
            Value* difference = graph.make(root, Sub, x, y);
            graph.branch(root, y, left, right);
            branchToReturns(left, masked, sum, x);
            branchToReturns(right, masked, difference, y);
            return;
        }
        case MaskTestUsers::BranchAndBoolean: {
            Value* sum = graph.make(root, Add, graph.make(root, ZExt32, isSet), y);
            branchToReturns(root, isSet, sum, graph.make(root, Sub, sum, x));
            return;
        }
        case MaskTestUsers::BranchAndSelect: {
            Value* selected = graph.select(root, masked, y, x);
            branchToReturns(root, masked, selected, graph.make(root, Add, selected, y));
            return;
        }
        }
    }

    static int64_t evaluate(MaskTestUsers users, int64_t x, int64_t y)
    {
        bool isSet = x & pinnedMask;
        switch (users) {
        case MaskTestUsers::Boolean:
            return isSet;
        case MaskTestUsers::Select:
            return isSet ? y : x;
        case MaskTestUsers::TwoBranches:
            if (y)
                return isSet ? x + y : x;
            return isSet ? x - y : y;
        case MaskTestUsers::BranchAndBoolean:
            return isSet ? 1 + y : y - x;
        case MaskTestUsers::BranchAndSelect:
            return isSet ? y : x + y;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Procedure proc;
};

struct ReadsOfPinnedTable {
    unsigned total { 0 };
    unsigned intoRegisters { 0 };
};

static ReadsOfPinnedTable readsOfPinnedTable(Procedure& proc)
{
    ReadsOfPinnedTable result;
    for (Air::BasicBlock* block : proc.code()) {
        for (Air::Inst& inst : *block) {
            bool readsTable = false;
            for (const Air::Arg& arg : inst.args())
                readsTable |= arg.isAddr() && arg.base() == Air::Tmp(GPRInfo::argumentGPR1);
            result.total += readsTable;
            result.intoRegisters += readsTable && inst.kind.opcode == Air::Move;
        }
    }
    return result;
}

static void testPinnedMaskIsOperandOfEveryTest()
{
    Policy policy = pinnedConstantPolicy(findTestConstant<true, true>);
    policy.selection.extendsInstructionSelection = true;
    for (MaskTestUsers users : { MaskTestUsers::Boolean, MaskTestUsers::Select, MaskTestUsers::TwoBranches, MaskTestUsers::BranchAndBoolean, MaskTestUsers::BranchAndSelect }) {
        for (MaskSource source : { MaskSource::PinnedConstant, MaskSource::MutableWordOfPinnedTable }) {
            PinnedMaskUsers compiled(policy, users, source);
            auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
            bool isShared = users != MaskTestUsers::Boolean && users != MaskTestUsers::Select;
            if (isX86_64() && Options::defaultB3OptLevel()) {
                ReadsOfPinnedTable reads = readsOfPinnedTable(compiled.proc);
                if (source == MaskSource::PinnedConstant) {
                    CHECK_EQ(reads.intoRegisters, 0u);
                    CHECK(reads.total >= (isShared ? 2u : 1u));
                } else
                    CHECK_EQ(reads.total, 1u);
            }
            PinnedConstantTable table;
            for (int64_t x : { 0x10, 0x0f, 0xf0, 0 }) {
                for (int64_t y : { 0, 5 })
                    CHECK_EQ(invoke<int64_t>(*code, x, &table, y), PinnedMaskUsers::evaluate(users, x, y));
            }
        }
    }
}

struct PinnedWordInStackmap {
    PinnedWordInStackmap(const Policy& policy, ValueRep rep)
        : proc(policy)
        , root(proc.addBlock())
    {
        Graph graph { proc };
        word = graph.int64(root, pinnedWord);
        patchpoint = graph.emptyPatchpoint(root, Int64);
        patchpoint->append(word, rep);
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            params[1].emitRestore(jit, params[0].gpr());
        });
        graph.ret(root, patchpoint);
    }

    Procedure proc;
    BasicBlock* root;
    Value* word;
    PatchpointValue* patchpoint;
};

static void testPinnedConstantInStackmap()
{
    Policy policy = pinnedConstantPolicy(findTestConstant<true, false>);

    PinnedWordInStackmap inspectedCold(policy, ValueRep::ColdAny);
    CHECK(!materializeAndValidate(inspectedCold.proc));
    CHECK(inspectedCold.patchpoint->child(0) == inspectedCold.word);

    PinnedWordInStackmap inspectedRegister(policy, ValueRep::SomeRegister);
    CHECK(materializeAndValidate(inspectedRegister.proc));
    checkReadsPinnedTable(inspectedRegister.root, inspectedRegister.patchpoint, 0, Int64, OBJECT_OFFSETOF(PinnedConstantTable, word));

    PinnedConstantTable table;
    for (ValueRep rep : { ValueRep(ValueRep::ColdAny), ValueRep(ValueRep::SomeRegister) }) {
        PinnedWordInStackmap compiled(policy, rep);
        CHECK_EQ(invoke<int64_t>(*compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel()), static_cast<int64_t>(0), &table), pinnedWord);
    }
}

static void testPinnedConstantsSwitchedOff()
{
    Policy policy = pinnedConstantPolicy(findTestConstant<false, false>);
    policy.disable(PolicySwitch::PinnedConstants);

    PinnedWordArithmetic inspected(policy, pinnedWord);
    CHECK(!inspected.proc.policy().addressing.findPinnedConstant);
    inspected.checkKeepsConstant(pinnedWord);

    PinnedWordArithmetic compiled(policy, pinnedWord);
    auto code = compileAndCheckReport(compiled.proc, Options::defaultB3OptLevel());
    int64_t garbage = 0x0bad0bad0bad0badll;
    for (int64_t x : { 0, 41, -1 })
        CHECK_EQ(invoke<int64_t>(*code, x, garbage), PinnedWordArithmetic::evaluate(x, pinnedWord));
}

static void addPipelineTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testBatchEqualsOneAtATime());
    RUN(testBatchOnOneScratchEqualsOneAtATime());
    RUN(testDefaultPolicyIsUpstream());
    RUN(testFailedProcedureLeavesBatch());
    RUN(testClientPhasesRunInOrder());
    RUN(testClientPhasesStopAfterFailure());
    RUN(testThreadJumpsReplacesTailDuplication());
    RUN(testEachSwitchAlone(0));
    RUN(testEachSwitchAlone(1));
    RUN(testEachSwitchAlone(2));
    RUN(testEachSwitchAlone(3));
    RUN(testScratchHandsOutDistinctRepositories());
    RUN(testScratchRepositoryIsDetached());
    RUN(testArenaReusesFirstChunk());
    RUN(testCheckVariablesAreSetOnePath());
    RUN(testCheckVariablesAreSetBothPaths());
    RUN(testCheckVariablesAreSetBeforeLoop());
    RUN(testCheckVariablesAreSetUnreachable());
    RUN(testCheckVariablesAreSetWithoutVariables());
    RUN(testPinnedConstantInRegister());
    RUN(testPinnedConstantInRegisterOfOtherBank());
    RUN(testPinnedConstantInMemory());
    RUN(testPinnedConstantKeepsImmediates());
    RUN(testPinnedConstantReplacesImmediate());
    RUN(testPinnedMaskIsOperandOfEveryTest());
    RUN(testPinnedConstantInStackmap());
    RUN(testPinnedConstantsSwitchedOff());
}
