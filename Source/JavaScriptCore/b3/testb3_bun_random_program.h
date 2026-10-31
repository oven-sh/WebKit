/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include "LinkBuffer.h"
#include <wtf/StringPrintStream.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>

inline B3::Opcode relationWithSwappedOperands(B3::Opcode relation)
{
    switch (relation) {
    case LessThan:
        return GreaterThan;
    case LessEqual:
        return GreaterEqual;
    case GreaterThan:
        return LessThan;
    case GreaterEqual:
        return LessEqual;
    case Below:
        return Above;
    case BelowEqual:
        return AboveEqual;
    case Above:
        return Below;
    case AboveEqual:
        return BelowEqual;
    default:
        return relation;
    }
}

inline B3::Opcode negatedRelation(B3::Opcode relation)
{
    switch (relation) {
    case Equal:
        return NotEqual;
    case NotEqual:
        return Equal;
    case LessThan:
        return GreaterEqual;
    case LessEqual:
        return GreaterThan;
    case GreaterThan:
        return LessEqual;
    case GreaterEqual:
        return LessThan;
    case Below:
        return AboveEqual;
    case BelowEqual:
        return Above;
    case Above:
        return BelowEqual;
    case AboveEqual:
        return Below;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return relation;
    }
}

struct RandomProgramState {
    PollState poll;
    int64_t effectHash { 0 };
    std::array<int64_t, 3> failedCheckOperands { };
};

inline int64_t JIT_OPERATION_ATTRIBUTES randomProgramCallee(RandomProgramState* state, int64_t operand)
{
    state->effectHash = static_cast<int64_t>(static_cast<uint64_t>(state->effectHash) * 3 + 1000 + static_cast<uint64_t>(operand));
    return static_cast<int64_t>(static_cast<uint64_t>(operand) + 5);
}

struct PatchpointModel {
    enum class Kind : uint8_t { Nothing, Constant, AddConstant, LoadWord, IncrementWord, TruncateDouble, Observable, Poll };

    Kind kind { Kind::Nothing };
    int64_t immediate { 0 };
};

using PatchpointModels = HashMap<Value*, PatchpointModel>;

inline bool isUncloneable(Value* value)
{
    return value->kind().isCloningForbidden();
}

inline constexpr uint32_t randomProgramSpace = 1;
inline constexpr uint32_t testDataSectionSpace = 2;
inline constexpr uint32_t testJumpTableSpace = 3;

enum class RandomProgramSymbol : uint32_t {
    Callee,
    State,
};

inline constexpr uint64_t conversionFunctionID = 200;
inline constexpr uint64_t addressFunctionID = 300;

class RandomProgramRelocationLowering final : public RelocationLowering {
public:
    explicit RandomProgramRelocationLowering(RandomProgramState& state)
        : m_state(state)
    {
    }

    Value* lower(Procedure&, InsertionSet&, size_t index, const RelocatableValue&) final;

private:
    Value* lowerProgramSymbol(InsertionSet&, size_t index, const RelocatableValue&);

    RandomProgramState& m_state;
};

inline Value* RandomProgramRelocationLowering::lower(Procedure& proc, InsertionSet& insertionSet, size_t index, const RelocatableValue& value)
{
    RelocationSymbol symbol = value.symbol();
    switch (symbol.space) {
    case randomProgramSpace:
        return lowerProgramSymbol(insertionSet, index, value);
    case testDataSectionSpace: {
        int64_t address = std::bit_cast<int64_t>(proc.dataSections()[symbol.index].data) + value.addend();
        return insertionSet.insert<Const64Value>(index, value.origin(), address);
    }
    case testJumpTableSpace:
    default:
        return nullptr;
    }
}

inline Value* RandomProgramRelocationLowering::lowerProgramSymbol(InsertionSet& insertionSet, size_t index, const RelocatableValue& value)
{
    RelocationSymbol symbol = value.symbol();
    void* base = nullptr;
    switch (static_cast<RandomProgramSymbol>(symbol.index)) {
    case RandomProgramSymbol::Callee:
        base = std::bit_cast<void*>(tagCFunction<OperationPtrTag>(randomProgramCallee));
        break;
    case RandomProgramSymbol::State:
        base = &m_state;
        break;
    }
    RELEASE_ASSERT(base);
    void* address = static_cast<uint8_t*>(base) + value.addend();

    PatchpointValue* result = insertionSet.insert<PatchpointValue>(index, pointerType(), value.origin());
    result->effects = Effects::none();
    if (!value.addend())
        result->pureFunction = PureFunction { addressFunctionID + symbol.index };
    result->setGenerator([address](CCallHelpers& jit, const StackmapGenerationParams& params) {
        jit.move(CCallHelpers::TrustedImmPtr(address), params[0].gpr());
    });
    return result;
}

class ReproducibleRelocationLowering final : public RelocationLowering {
public:
    explicit ReproducibleRelocationLowering(RandomProgramState& state)
        : m_programLowering(state)
    {
    }

    Value* lower(Procedure&, InsertionSet&, size_t index, const RelocatableValue&) final;

private:
    static constexpr int64_t placeholderBytesPerDataSection = 1ll << 32;

    RandomProgramRelocationLowering m_programLowering;
};

inline Value* ReproducibleRelocationLowering::lower(Procedure& proc, InsertionSet& insertionSet, size_t index, const RelocatableValue& value)
{
    RelocationSymbol symbol = value.symbol();
    if (symbol.space != testDataSectionSpace)
        return m_programLowering.lower(proc, insertionSet, index, value);
    int64_t placeholder = (static_cast<int64_t>(symbol.index) + 1) * placeholderBytesPerDataSection + value.addend();
    return insertionSet.insert<Const64Value>(index, value.origin(), placeholder);
}

inline Policy randomProgramPolicy(RelocationLowering& lowering)
{
    Policy policy = optimizingPolicy();

    policy.allocation.keepsValuesAfterLowering = true;

    policy.upstream.allowsSIMD = true;
    policy.loops.vectorizes = true;
    policy.loops.unrolls = true;

    policy.addressing.isPositionIndependent = true;
    policy.addressing.namesPinnedRegistersDirectly = true;
    policy.addressing.dataSectionSpace = testDataSectionSpace;
    policy.addressing.jumpTableSpace = testJumpTableSpace;
    policy.addressing.relocationLowering = &lowering;

    policy.switchLowering.minCasesForJumpTable = std::numeric_limits<unsigned>::max();

    policy.air.registers.chargesForCalleeSaves = true;
    policy.air.registers.discountsPreferredRegisterMoves = true;
    policy.air.registers.hintsTwoAddressOperands = true;
    policy.air.registers.usesSecondPreferredRegister = true;
    policy.air.registers.evictsHolderOfPreferredRegister = true;

    policy.air.splitting.splitsAroundClobbersEarly = true;
    policy.air.splitting.splitsAroundClobbersThatRead = true;
    policy.air.splitting.keepsUnusedTmpsOnStackThroughLoops = true;
    policy.air.splitting.spillsTmpsReadInPlaceFirst = true;
    policy.air.splitting.spansStretchesOfClobbers = true;
    policy.air.splitting.splitsIntoClustersOverBlocks = true;
    policy.air.splitting.splitsAroundFrame = true;

    policy.air.frame.ordersSlotsByUse = true;
    policy.air.frame.prefersStackPointerAddresses = true;

    policy.air.layout.movesBlocksWithoutSuccessorsToEnd = true;
    policy.air.layout.placesRareBlocksInGaps = true;
    policy.air.layout.ordersBlocksByEdgeFrequency = true;
    policy.air.layout.recordsRareCodeStart = true;
    policy.air.layout.mergesIdenticalBlocks = true;

    policy.air.peepholes.forwardsRegisterCopies = true;
    policy.air.peepholes.hoistsCommonMoves = true;
    policy.air.peepholes.reusesConditionFlags = true;
    policy.air.peepholes.pairsMemoryAccesses = true;
    policy.air.peepholes.resequencesMovesBeforePatches = true;
    policy.air.peepholes.trapReach = 4096;
    return policy;
}

inline std::unique_ptr<Compilation> generateAndLink(Procedure& proc, Vector<uint8_t>* bytes = nullptr)
{
    CHECK(!proc.report().failure);
    CCallHelpers jit;
    generate(proc, jit);
    if (bytes) {
        AssemblerBuffer& buffer = jit.m_assembler.buffer();
        *bytes = Vector<uint8_t>(unsafeMakeSpan(static_cast<const uint8_t*>(buffer.readOnlyData()), buffer.codeSize()));
    }
    LinkBuffer linkBuffer(jit, nullptr);
    auto code = FINALIZE_CODE(linkBuffer, JITCompilationPtrTag, nullptr, "Compilation");
    CHECK(!proc.report().failure);
    return makeUnique<Compilation>(WTF::move(code), proc.releaseByproducts());
}

inline std::unique_ptr<Compilation> compileAndCheckReport(Procedure& proc, unsigned optLevel)
{
    proc.setOptLevel(optLevel);
    if (g_dumpB3AfterGeneration)
        proc.setShouldDumpIR();
    prepareForGeneration(proc);
    return generateAndLink(proc);
}

struct RandomProgramOptions {
    bool hasCallShapes { true };
    bool isForLevel0 { false };
};

class RandomProgram {
public:
    static constexpr unsigned wordsPerRegion = 16;
    static constexpr unsigned numberOfWords = 3 * wordsPerRegion;
    static constexpr unsigned numberOfSmallWords = 8;
    static constexpr unsigned numberOfGuardWords = 16;
    static constexpr unsigned smallWordLimit = 40;
    static constexpr int64_t failedCheckResult = -77;

    RandomProgram(Procedure& proc, unsigned seed, RandomProgramState& state, RandomProgramOptions options, PatchpointModels* models = nullptr)
        : m_graph { proc }
        , m_proc(proc)
        , m_random(seed)
        , m_coldOperandRandom(seed + 0x85ebca6bu)
        , m_promiseRandom(seed + 0xc2b2ae35u)
        , m_state(state)
        , m_options(options)
        , m_models(models)
    {
        m_block = m_proc.addBlock();
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(m_proc, m_block);
        m_arguments = { arguments[0], arguments[2] };
        m_pointer = arguments[1];
        unsigned numberOfVariables = 3 + pick(10);
        for (unsigned i = 0; i < numberOfVariables; ++i) {
            m_variables.append(m_proc.addVariable(Int64));
            set(m_variables[i], i < m_arguments.size() ? m_arguments[i] : constant());
        }
        m_pointerOrNull = m_graph.select(m_block, make(chance(50) ? LessThan : NotEqual, m_arguments[pick(2)], constant()), m_pointer, int64(0));
        m_statementsLeft = 8 + pick(28);
        while (m_statementsLeft)
            statements(3);
        Value* hash = int64(0);
        for (Variable* variable : m_variables)
            hash = make(Add, make(Mul, hash, int64(31)), get(variable));
        m_graph.ret(m_block, hash);
        m_proc.resetReachability();
        validate(m_proc);
    }

private:
    struct Loop {
        BasicBlock* continueTarget { nullptr };
        BasicBlock* breakTarget { nullptr };
        BasicBlock* headerForExtraLatch { nullptr };
        Variable* index { nullptr };
        int64_t delta { 0 };
    };

    struct MemoryOperand {
        Value* base;
        int32_t offset;
        HeapRange range;
    };

    struct CountedLoopShape;
    struct FuelBoundedLoopShape;
    struct LoopParts;

    enum class PollVariant { None, TwoPolls, WithoutRole, TwoPollsInTwoBlocks, OnLikelyPath, OnRandomPath, PollAndLookAlike, OnlyLocalEffects, WritesOnlyPollState, OnRarePathOfVariable, InOwnBlock, ClobbersArgumentRegisters, Count };

    using Model = PatchpointModel::Kind;
    using ColdOperands = Vector<Value*, 3>;

    template<typename T>
    T* modeled(T* patchpoint, Model kind, int64_t immediate = 0)
    {
        if (m_models)
            m_models->add(patchpoint, PatchpointModel { kind, immediate });
        return patchpoint;
    }

    unsigned pick(unsigned limit) { return m_random.getUint32(limit); }
    bool chance(unsigned percent) { return pick(100) < percent; }

    BasicBlock* newBlock() { return m_proc.addBlock(); }
    Value* int32(int32_t value) { return m_graph.int32(m_block, value); }
    Value* int64(int64_t value) { return m_graph.int64(m_block, value); }
    Value* make(B3::Opcode opcode, Value* child) { return m_graph.make(m_block, opcode, child); }
    Value* make(B3::Opcode opcode, Value* left, Value* right) { return m_graph.make(m_block, opcode, left, right); }
    Value* get(Variable* variable) { return m_graph.get(m_block, variable); }
    void set(Variable* variable, Value* value) { m_graph.set(m_block, variable, value); }
    void jump(BasicBlock* target) { m_graph.jump(m_block, target); }
    void branch(Value* predicate, FrequentedBlock taken, FrequentedBlock notTaken) { m_graph.branch(m_block, predicate, taken, notTaken); }
    Variable* randomVariable() { return m_variables[pick(m_variables.size())]; }

    Value* constant();
    Value* randomLoopIndex();
    Value* leaf();
    Value* expression(unsigned depth);
    Value* doubleExpression(unsigned depth);
    Value* convertToInteger(Value* number, Type);
    Value* condition(unsigned depth);
    MemoryOperand memoryOperand(unsigned region, unsigned depth);
    Value* wordAddress(Value* index);
    Value* load(unsigned depth);
    Value* loadSmallWord();
    void store();

    ColdOperands coldOperands();
    Value* observablePatchpoint(Value* operand);
    Value* movablePatchpoint(unsigned depth);
    void storeBurst();
    void patchpoint();
    void check();
    static void clobberArgumentRegisters(PatchpointValue* poll);
    PatchpointValue* appendPollVariant(bool isLoopPoll, PollVariant);
    void continueInNewBlock();
    void appendPollOnPath(Value* predicate, FrequentedBlock polling, PollVariant);
    void poll();
    Value* address(RandomProgramSymbol);

    void returnStatement();
    void exitLoop();
    void branchTarget(BasicBlock*, FrequencyClass, unsigned depth, BasicBlock* join);
    void ifStatement(unsigned depth);
    void switchStatement(unsigned depth);
    void longEqualityChain();
    void boundsCheckedAccess();
    void nullCheckedAccess();
    void flagTestedAfterJoin(unsigned depth);
    void branchToSameBlock();
    void statements(unsigned depth);
    void statement(unsigned depth);

    CountedLoopShape chooseCountedLoopShape();
    LoopParts appendCountedLoopEntry(const CountedLoopShape&);
    Value* appendCountedLoopTest(const CountedLoopShape&, const LoopParts&);
    Value* appendCountedLoopBody(const CountedLoopShape&, const LoopParts&, unsigned depth);
    void countedLoop(unsigned depth);
    FuelBoundedLoopShape chooseFuelBoundedLoopShape();
    LoopParts appendFuelBoundedLoopEntry(const FuelBoundedLoopShape&);
    Value* appendFuelBoundedLoopTest(const FuelBoundedLoopShape&, const LoopParts&);
    Value* appendFuelBoundedLoopBody(const FuelBoundedLoopShape&, const LoopParts&, unsigned depth);
    void fuelBoundedLoop(unsigned depth);
    void whileLoop(unsigned depth);

    Graph m_graph;
    Procedure& m_proc;
    WeakRandom m_random;
    WeakRandom m_coldOperandRandom;
    WeakRandom m_promiseRandom;
    RandomProgramState& m_state;
    RandomProgramOptions m_options;
    PatchpointModels* m_models;
    BasicBlock* m_block { nullptr };
    unsigned m_statementsLeft { 0 };
    int32_t m_observablePatchpointCount { 0 };
    bool m_isInLongLoop { false };
    std::array<Value*, 2> m_arguments;
    Value* m_pointer { nullptr };
    Value* m_pointerOrNull { nullptr };
    Vector<Variable*, 12> m_variables;
    Vector<Value*> m_available;
    Vector<Loop, 4> m_loops;
};

inline Vector<int64_t> randomInitialMemory(WeakRandom& random)
{
    Vector<int64_t> memory(RandomProgram::numberOfWords);
    for (unsigned i = 0; i < memory.size(); ++i) {
        bool isSmall = i >= 2 * RandomProgram::wordsPerRegion && i < 2 * RandomProgram::wordsPerRegion + RandomProgram::numberOfSmallWords;
        if (isSmall || random.getUint32(3))
            memory[i] = random.getUint32(RandomProgram::smallWordLimit);
        else
            memory[i] = static_cast<int64_t>(static_cast<uint64_t>(random.getUint32()) << 32 | random.getUint32());
    }
    return memory;
}

struct RandomProgramVersion {
    WTF_MAKE_NONCOPYABLE(RandomProgramVersion);

    explicit RandomProgramVersion(ASCIILiteral name)
        : name(name)
        , lowering(state)
        , proc(Policy { })
    {
    }

    template<typename Functor>
    RandomProgramVersion(ASCIILiteral name, const Functor& makePolicy)
        : name(name)
        , lowering(state)
        , proc(makePolicy(lowering))
    {
    }

    void prepare(const Vector<int64_t>& initialMemory, int64_t limit)
    {
        memory.fill(guardWord, initialMemory.size() + 2 * RandomProgram::numberOfGuardWords);
        for (unsigned i = 0; i < initialMemory.size(); ++i)
            memory[RandomProgram::numberOfGuardWords + i] = initialMemory[i];
        state = { };
        state.poll.limit = limit;
    }

    void call(int64_t first, int64_t second, const Vector<int64_t>& initialMemory, int64_t limit)
    {
        prepare(initialMemory, limit);
        result = invoke<int64_t>(*code, first, memory.mutableSpan().subspan(RandomProgram::numberOfGuardWords).data(), second);
    }

    bool hasIntactGuards() const
    {
        for (unsigned i = 0; i < RandomProgram::numberOfGuardWords; ++i) {
            if (memory[i] != guardWord || memory[memory.size() - 1 - i] != guardWord)
                return false;
        }
        return true;
    }

    bool agreesWith(const RandomProgramVersion& reference) const
    {
        return result == reference.result && memory == reference.memory && hasIntactGuards()
            && state.effectHash == reference.state.effectHash && state.failedCheckOperands == reference.state.failedCheckOperands;
    }

    String differenceFrom(const RandomProgramVersion& reference, int64_t first, int64_t second) const
    {
        String versionName = disabledSwitch ? makeString(name, " without "_s, nameOf(*disabledSwitch)) : String(name);
        return makeString("called with "_s, first, " and "_s, second, " returned "_s, reference.result, ' ', reference.name, " but "_s, result, ' ', versionName,
            "; effect hashes: "_s, reference.state.effectHash, " and "_s, state.effectHash,
            "; failed check operands: "_s, toString(listDump(reference.state.failedCheckOperands)), " and "_s, toString(listDump(state.failedCheckOperands)),
            "; memory: "_s, toString(listDump(reference.memory)), " and "_s, toString(listDump(memory)));
    }

    static constexpr int64_t guardWord = 0x5a5a5a5a5a5a5a5all;

    ASCIILiteral name;
    std::optional<PolicySwitch> disabledSwitch;
    RandomProgramState state;
    RandomProgramRelocationLowering lowering;
    Procedure proc;
    std::unique_ptr<Compilation> code;
    Vector<int64_t> memory;
    int64_t result { 0 };
};

#endif // ENABLE(B3_JIT)
