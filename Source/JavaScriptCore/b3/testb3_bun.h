/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3.h"

#if ENABLE(B3_JIT)

#include "B3ClampLoopBound.h"
#include "B3CompilationReport.h"
#include "B3DuplicateTails.h"
#include "B3Limits.h"
#include "B3NaturalLoops.h"
#include "B3Policy.h"
#include "B3RotateLoops.h"
#include "B3ThreadJumps.h"
#include "B3Variable.h"
#include "B3VersionLoops.h"


struct Graph {
    Value* int32(BasicBlock* block, int32_t value) { return block->appendNew<Const32Value>(proc, Origin(), value); }
    Value* int64(BasicBlock* block, int64_t value) { return block->appendNew<Const64Value>(proc, Origin(), value); }
    Value* number(BasicBlock* block, double value) { return block->appendNew<ConstDoubleValue>(proc, Origin(), value); }
    Value* make(BasicBlock* block, B3::Opcode opcode, Value* child) { return block->appendNew<Value>(proc, opcode, Origin(), child); }
    Value* make(BasicBlock* block, B3::Opcode opcode, Value* left, Value* right) { return block->appendNew<Value>(proc, opcode, Origin(), left, right); }
    Value* select(BasicBlock* block, Value* condition, Value* ifTrue, Value* ifFalse) { return block->appendNew<Value>(proc, Select, Origin(), condition, ifTrue, ifFalse); }

    Value* get(BasicBlock* block, Variable* variable) { return block->appendNew<VariableValue>(proc, B3::Get, Origin(), variable); }
    void set(BasicBlock* block, Variable* variable, Value* value) { block->appendNew<VariableValue>(proc, B3::Set, Origin(), variable, value); }
    void increase(BasicBlock* block, Variable* variable, Value* by) { set(block, variable, make(block, Add, get(block, variable), by)); }
    void increase(BasicBlock* block, Variable* variable, int64_t by) { increase(block, variable, int64(block, by)); }

    Value* phi(BasicBlock* block, Type type) { return block->appendNew<Value>(proc, Phi, type, Origin()); }
    void upsilon(BasicBlock* block, Value* value, Value* phi) { block->appendNew<UpsilonValue>(proc, Origin(), value, phi); }

    MemoryValue* load(BasicBlock* block, Type type, Value* pointer, int32_t offset, HeapRange range)
    {
        return block->appendNew<MemoryValue>(proc, Load, type, Origin(), pointer, offset, range);
    }

    void store(BasicBlock* block, Value* value, Value* pointer, int32_t offset, HeapRange range)
    {
        block->appendNew<MemoryValue>(proc, Store, Origin(), value, pointer, offset, range);
    }

    PatchpointValue* emptyPatchpoint(BasicBlock* block, Type type = Void)
    {
        PatchpointValue* result = block->appendNew<PatchpointValue>(proc, type, Origin());
        result->effects = Effects::none();
        result->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            if (params.size())
                jit.move(CCallHelpers::TrustedImm32(0), params[0].gpr());
        });
        return result;
    }

    PatchpointValue* callLikePatchpoint(BasicBlock* block)
    {
        PatchpointValue* result = emptyPatchpoint(block);
        result->effects.writesLocalState = true;
        result->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allRegisters()));
        return result;
    }

    void jump(BasicBlock* from, BasicBlock* to)
    {
        from->appendNew<Value>(proc, Jump, Origin());
        from->setSuccessors(FrequentedBlock(to));
    }

    void branch(BasicBlock* from, Value* condition, FrequentedBlock taken, FrequentedBlock notTaken)
    {
        from->appendNew<Value>(proc, Branch, Origin(), condition);
        from->setSuccessors(taken, notTaken);
    }

    void branch(BasicBlock* from, Value* condition, BasicBlock* taken, BasicBlock* notTaken)
    {
        branch(from, condition, FrequentedBlock(taken), FrequentedBlock(notTaken));
    }

    void ret(BasicBlock* block, Value* value) { block->appendNew<Value>(proc, Return, Origin(), value); }

    Procedure& proc;
};

inline FrequentedBlock rarely(BasicBlock* block) { return FrequentedBlock(block, FrequencyClass::Rare); }
inline FrequentedBlock unlikely(BasicBlock* block) { return FrequentedBlock(block, FrequencyClass::Unlikely); }

inline Origin numberedOrigin(uintptr_t number) { return Origin(std::bit_cast<const DFG::Node*>(number << 12)); }
inline bool hasNumber(Origin origin, uintptr_t number) { return origin.dfgOrigin() == numberedOrigin(number).dfgOrigin(); }

inline constexpr HeapRange firstHeap { 10, 11 };
inline constexpr HeapRange secondHeap { 11, 12 };
inline constexpr HeapRange bothHeaps { 10, 12 };
inline constexpr HeapRange thirdHeap { 12, 13 };
inline constexpr HeapRange allHeaps { 10, 13 };
inline constexpr HeapRange pollHeap { 30, 31 };

template<typename Functor>
inline unsigned numberOfValues(Procedure& proc, const Functor& functor)
{
    unsigned result = 0;
    for (BasicBlock* block : proc) {
        for (Value* value : *block)
            result += functor(value);
    }
    return result;
}

inline unsigned numberOfValues(Procedure& proc, B3::Opcode opcode)
{
    return numberOfValues(proc, [&](Value* value) {
        return value->opcode() == opcode;
    });
}

inline unsigned numberOfValues(BasicBlock* block, B3::Opcode opcode)
{
    unsigned result = 0;
    for (Value* value : *block)
        result += value->opcode() == opcode;
    return result;
}

inline unsigned numberOfBlocks(Procedure& proc)
{
    unsigned result = 0;
    for (BasicBlock* block : proc) {
        UNUSED_PARAM(block);
        ++result;
    }
    return result;
}

inline bool hasInt64Constant(Procedure& proc, int64_t constant)
{
    return numberOfValues(proc, [&](Value* value) {
        return value->isInt64(constant);
    });
}

inline unsigned valueCountInLoops(Procedure& proc, B3::Opcode opcode)
{
    B3::NaturalLoops& loops = proc.naturalLoops();
    unsigned result = 0;
    for (BasicBlock* block : proc) {
        if (loops.innerMostLoopOf(block))
            result += numberOfValues(block, opcode);
    }
    return result;
}

template<typename Functor>
inline Vector<unsigned> valueCountInEachLoop(Procedure& proc, const Functor& functor)
{
    B3::NaturalLoops& loops = proc.naturalLoops();
    Vector<unsigned> result;
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex) {
        const B3::NaturalLoop& loop = loops.loop(loopIndex);
        unsigned number = 0;
        for (unsigned i = 0; i < loop.size(); ++i) {
            for (Value* value : *loop.at(i))
                number += functor(value);
        }
        result.append(number);
    }
    std::ranges::sort(result);
    return result;
}

inline Vector<unsigned> valueCountInEachLoop(Procedure& proc, B3::Opcode opcode)
{
    return valueCountInEachLoop(proc, [&](Value* value) {
        return value->opcode() == opcode;
    });
}

template<typename Functor, typename Description>
inline void checkValueCountInEachLoop(Procedure& proc, const Functor& functor, const Description& description, const Vector<unsigned>& expected)
{
    Vector<unsigned> found = valueCountInEachLoop(proc, functor);
    if (found == expected)
        return;
    crashLock.lock();
    dataLogLn("Expected ", listDump(expected), " of ", description, " in the loops but found ", listDump(found), ":\n", proc);
    CRASH();
}

inline void checkValueCountInEachLoop(Procedure& proc, B3::Opcode opcode, const Vector<unsigned>& expected)
{
    checkValueCountInEachLoop(proc, [&](Value* value) {
        return value->opcode() == opcode;
    }, opcode, expected);
}

struct TopTestedLoop {
    explicit TopTestedLoop(Graph& graph, B3::Opcode relation = LessThan, int64_t step = 1, int64_t firstIndex = 0)
        : graph(graph)
        , step(step)
        , root(graph.proc.addBlock())
        , header(graph.proc.addBlock())
        , body(graph.proc.addBlock())
        , exit(graph.proc.addBlock())
        , index(graph.proc.addVariable(Int64))
        , sum(graph.proc.addVariable(Int64))
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t*, int64_t>(graph.proc, root);
        bound = arguments[0];
        pointer = arguments[1];
        extra = arguments[2];
        start = graph.int64(root, firstIndex);
        graph.set(root, index, start);
        graph.set(root, sum, graph.int64(root, 0));
        graph.jump(root, header);
        graph.branch(header, graph.make(header, relation, graph.get(header, index), bound), body, exit);
        graph.ret(exit, graph.get(exit, sum));
    }

    void endBody(BasicBlock* bodyLastBlock)
    {
        graph.increase(bodyLastBlock, index, step);
        graph.jump(bodyLastBlock, header);
        graph.proc.resetReachability();
        fixSSA(graph.proc);
        publishInduction();
        validate(graph.proc);
    }

    Value* counter() const { return header->last()->child(0)->child(0)->foldIdentity(); }

    Value* nextCounter() const
    {
        Value* result = nullptr;
        for (Value* value : graph.proc.values()) {
            auto* upsilon = value->as<UpsilonValue>();
            if (upsilon && upsilon->phi() == counter() && upsilon->child(0) != start)
                result = upsilon->child(0);
        }
        return result;
    }

    void publishInduction(bool doesNotWrap = false, bool relationHoldsOnEntry = false)
    {
        CHECK(counter()->opcode() == Phi);
        graph.proc.setKnownInduction({ counter(), nextCounter(), header->last()->child(0), step, doesNotWrap, relationHoldsOnEntry });
    }

    void rotate()
    {
        CHECK(rotateLoops(graph.proc));
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    Graph& graph;
    int64_t step;
    BasicBlock* root;
    BasicBlock* header;
    BasicBlock* body;
    BasicBlock* exit;
    Variable* index;
    Variable* sum;
    Value* bound;
    Value* pointer;
    Value* extra;
    Value* start;
};

inline int64_t run(Compilation& code, int64_t bound, int64_t* pointer = nullptr, int64_t extra = 0)
{
    return invoke<int64_t>(code, bound, pointer, extra);
}

inline int64_t sumBelow(int64_t bound) { return bound <= 0 ? 0 : bound * (bound - 1) / 2; }

inline void checkSumsIntegersBelowBound(Procedure& proc)
{
    auto code = compileProc(proc);
    for (int64_t bound : { -1, 0, 1, 2, 10, 1000 })
        CHECK_EQ(run(*code, bound), sumBelow(bound));
}

template<typename Functor>
inline Policy policyWith(const Functor& configure)
{
    Policy policy;
    policy.validation.level = ValidationLevel::AtEachPhase;
    policy.knownProperties.trustsKnownRanges = true;
    policy.knownProperties.trustsKnownDereferenceableBytes = true;
    policy.knownProperties.trustsKnownInductions = true;
    configure(policy);
    return policy;
}

inline Policy strengthReductionPolicy()
{
    return policyWith([](Policy& policy) {
        policy.optimization.redundancyElimination = RedundancyElimination::MemoryNumbering;
        policy.optimization.reducesWithRanges = true;
        policy.optimization.narrowsIntegers = true;
        policy.optimization.foldsImpliedComparisons = true;
        policy.optimization.mergesBlocksAfterForwardingJumps = true;
    });
}

inline Policy remarkPolicy()
{
    return policyWith([](Policy& policy) {
        policy.collectsRemarks = true;
    });
}

inline Policy loopPolicy()
{
    Policy policy = remarkPolicy();
    policy.loops.clampsLoopBounds = true;
    return policy;
}

inline Policy optimizingPolicy()
{
    return policyWith([](Policy& policy) {
        policy.allocation.usesArena = true;
        policy.upstream.maxTailDuplicationBlockSize = 1;

        OptimizationPolicy& optimization = policy.optimization;
        optimization.redundancyElimination = RedundancyElimination::MemoryNumbering;
        optimization.splitsBranchConditions = true;
        optimization.estimatesFrequencies = true;
        optimization.checksVariablesAreSet = true;
        optimization.threadsJumps = true;
        optimization.foldsImpliedComparisons = true;
        optimization.reducesWithRanges = true;
        optimization.narrowsIntegers = true;
        optimization.chainsAssociativeOperations = true;
        optimization.stopsSwitchInferenceAtRareEdges = true;
        optimization.convertsBranchesToSelects = true;
        optimization.mergesAdjacentConstantStores = true;
        optimization.sinksValues = true;
        optimization.placesConstantsByFrequency = true;
        optimization.mergesBlocksAfterForwardingJumps = true;
        optimization.keepsOriginsOfDemotedValues = true;

        policy.selection.extendsInstructionSelection = true;
        policy.selection.attributesTrapsToAccesses = true;

        policy.loops.rotates = true;
        policy.loops.foldsComparisonsWithBounds = true;
        policy.loops.versions = true;
        policy.loops.clampsLoopBounds = true;
        policy.loops.hoistsOnlyFromLikelyPaths = true;
        policy.loops.hoistsImmutableLoadsPastWrites = true;

        policy.switchLowering.usesBitTests = true;

        policy.air.peepholes.hoistsCommonMoves = true;
        policy.air.peepholes.reusesConditionFlags = true;
        policy.air.peepholes.pairsMemoryAccesses = true;
        policy.air.peepholes.emitsShufflesInPairOrder = true;
        policy.air.frame.prefersStackPointerAddresses = true;

        policy.collectsRemarks = true;
    });
}

template<typename Functor>
inline Policy policyWithInspection(Policy policy, const Functor& inspect)
{
    policy.clientPhases.append(ClientPhase { ClientPhase::Point::AfterOptimization, "inspectOptimizedProcedure"_s, createSharedTask<bool(Procedure&)>([inspect](Procedure& proc) {
        inspect(proc);
        return false;
    }) });
    return policy;
}

inline void resetAndValidate(Procedure& proc)
{
    proc.resetReachability();
    validate(proc);
}

inline bool reduceStrengthOnce(Procedure& proc)
{
    proc.resetReachability();
    proc.setOptLevel(2);
    bool result = reduceStrength(proc, ReduceStrengthPass::Initial);
    validate(proc);
    return result;
}

inline bool versionLoopsAndValidate(Procedure& proc)
{
    Vector<BasicBlock*> topTestedVersionHeaders;
    bool result = versionLoops(proc, topTestedVersionHeaders);
    fixSSA(proc);
    validate(proc);
    return result;
}

// So many rounds that a loop which loses count of them, or a counter of thirty-two bits less than it needs, shows.
inline constexpr int64_t longRun = 1 << 16;

struct PollState {
    int64_t limit { 0 };
    int64_t numberOfPolls { 0 };
    int64_t numberOfStops { 0 };
};

inline PatchpointValue* appendPoll(Graph& graph, BasicBlock* block, PollState& state)
{
    PatchpointValue* poll = block->appendNew<PatchpointValue>(graph.proc, Void, Origin());
    poll->isLoopPoll = true;
    poll->effects = Effects::none();
    poll->effects.reads = HeapRange::top();
    poll->effects.writes = pollHeap;
    poll->numGPScratchRegisters = 1;
    poll->clobber(RegisterSet::macroClobberedGPRs());
    poll->setGenerator([&state](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        GPRReg scratch = params.gpScratch(0);
        CCallHelpers::Address limit(scratch, OBJECT_OFFSETOF(PollState, limit));
        jit.move(CCallHelpers::TrustedImmPtr(&state), scratch);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(scratch, OBJECT_OFFSETOF(PollState, numberOfPolls)));
        auto limitIsNotNegative = jit.branchTest64(CCallHelpers::PositiveOrZero, limit);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(scratch, OBJECT_OFFSETOF(PollState, numberOfStops)));
        jit.store64(CCallHelpers::TrustedImm32(0), limit);
        limitIsNotNegative.link(&jit);
    });
    return poll;
}

inline bool hasRemark(Procedure& proc, RemarkKind kind, GaveUpReason reason)
{
    return proc.report().remarks.containsIf([&](const Remark& remark) {
        return remark.kind == kind && remark.reason == reason;
    });
}

inline bool hasRemark(Procedure& proc, RemarkKind kind)
{
    return proc.report().remarks.containsIf([&](const Remark& remark) {
        return remark.kind == kind;
    });
}

inline uint32_t limitHits(Procedure& proc, Limit limit) { return proc.report().hitsOf(limit); }

inline bool holds(B3::Opcode relation, int64_t left, int64_t right)
{
    switch (relation) {
    case Equal:
        return left == right;
    case NotEqual:
        return left != right;
    case LessThan:
        return left < right;
    case LessEqual:
        return left <= right;
    case GreaterThan:
        return left > right;
    case GreaterEqual:
        return left >= right;
    case Below:
        return static_cast<uint64_t>(left) < static_cast<uint64_t>(right);
    case BelowEqual:
        return static_cast<uint64_t>(left) <= static_cast<uint64_t>(right);
    case Above:
        return static_cast<uint64_t>(left) > static_cast<uint64_t>(right);
    case AboveEqual:
        return static_cast<uint64_t>(left) >= static_cast<uint64_t>(right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

inline void addIfComparisonHolds(Graph& graph, BasicBlock*& block, Variable* sum, Value* comparison, int64_t ifTrue, int64_t ifFalse)
{
    BasicBlock* holds = graph.proc.addBlock();
    BasicBlock* fails = graph.proc.addBlock();
    BasicBlock* next = graph.proc.addBlock();
    graph.branch(block, comparison, holds, fails);
    graph.increase(holds, sum, ifTrue);
    graph.jump(holds, next);
    graph.increase(fails, sum, ifFalse);
    graph.jump(fails, next);
    block = next;
}

struct BranchToRareBlocks {
    explicit BranchToRareBlocks(Graph& graph, double rareBlockFrequency = 0.01)
        : root(graph.proc.addBlock())
        , firstRare(graph.proc.addBlock(rareBlockFrequency))
        , next(graph.proc.addBlock())
        , secondRare(graph.proc.addBlock(rareBlockFrequency))
        , hot(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(graph.proc, root);
        selector = arguments[0];
        operand = arguments[1];
        pointer = arguments[2];
    }

    void finish(Graph& graph, Value* inFirstRare, Value* inSecondRare, Value* onHotPath)
    {
        graph.branch(root, graph.make(root, Equal, selector, graph.int64(root, 1)), rarely(firstRare), FrequentedBlock(next));
        graph.ret(firstRare, inFirstRare);
        graph.branch(next, graph.make(next, Equal, selector, graph.int64(next, 2)), rarely(secondRare), FrequentedBlock(hot));
        graph.ret(secondRare, inSecondRare);
        graph.ret(hot, onHotPath);
        graph.proc.resetReachability();
        validate(graph.proc);
    }

    BasicBlock* root;
    BasicBlock* firstRare;
    BasicBlock* next;
    BasicBlock* secondRare;
    BasicBlock* hot;
    Value* selector;
    Value* operand;
    Value* pointer;
};

struct TwoLoads {
    explicit TwoLoads(Mutability mutability = Mutability::Mutable, const Policy& policy = strengthReductionPolicy())
        : proc(policy)
        , mutability(mutability)
        , root(proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t*, int64_t*, int64_t>(proc, root);
        pointer = arguments[0];
        otherPointer = arguments[1];
        operand = arguments[2];
        first = load(root);
    }

    Value* load(BasicBlock* block)
    {
        MemoryValue* result = graph.load(block, Int64, pointer, 0, firstHeap);
        result->setReadsMutability(mutability);
        return result;
    }

    void returnBothLoads(BasicBlock* block, Value* addend = nullptr)
    {
        Value* result = graph.make(block, Add, first, graph.make(block, Mul, load(block), graph.int64(block, 100)));
        graph.ret(block, addend ? graph.make(block, Add, result, addend) : result);
    }

    unsigned reduceAndCountLoads()
    {
        resetAndValidate(proc);
        reduceStrengthOnce(proc);
        return numberOfValues(proc, Load);
    }

    unsigned remainingLoadCount(BasicBlock* block, Value* addend = nullptr)
    {
        returnBothLoads(block, addend);
        unsigned result = reduceAndCountLoads();
        code = compileProc(proc);
        return result;
    }

    int64_t run(int64_t* pointer, int64_t* otherPointer, int64_t operand)
    {
        if (!code)
            code = compileProc(proc);
        return invoke<int64_t>(*code, pointer, otherPointer, operand);
    }

    Procedure proc;
    Graph graph { proc };
    Mutability mutability;
    BasicBlock* root;
    Value* pointer;
    Value* otherPointer;
    Value* operand;
    Value* first;
    std::unique_ptr<Compilation> code;
};

struct TailWithTwoPredecessors {
    explicit TailWithTwoPredecessors(Graph& graph)
        : root(graph.proc.addBlock())
        , left(graph.proc.addBlock())
        , right(graph.proc.addBlock())
        , tail(graph.proc.addBlock())
        , taken(graph.proc.addBlock())
        , notTaken(graph.proc.addBlock())
    {
        auto arguments = cCallArgumentValues<int64_t, int64_t, int64_t*>(graph.proc, root);
        first = arguments[0];
        second = arguments[1];
        pointer = arguments[2];
        graph.store(left, first, pointer, 0, firstHeap);
        graph.store(right, second, pointer, 0, firstHeap);
        graph.ret(taken, graph.int64(taken, 10));
        graph.ret(notTaken, graph.int64(notTaken, 20));
    }

    void joinPaths(Graph& graph)
    {
        graph.jump(left, tail);
        graph.jump(right, tail);
        resetAndValidate(graph.proc);
    }

    void duplicate(Graph& graph)
    {
        joinPaths(graph);
        duplicateTails(graph.proc);
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    void thread(Graph& graph)
    {
        joinPaths(graph);
        threadJumps(graph.proc);
        fixSSA(graph.proc);
        validate(graph.proc);
    }

    BasicBlock* root;
    BasicBlock* left;
    BasicBlock* right;
    BasicBlock* tail;
    BasicBlock* taken;
    BasicBlock* notTaken;
    Value* first;
    Value* second;
    Value* pointer;
};

using BunTestTasks = Deque<RefPtr<SharedTask<void()>>>;

void addBunLoopTests(const TestConfig*, BunTestTasks&);
void addBunCountedLoopTests(const TestConfig*, BunTestTasks&);
void addBunComparisonFoldingTests(const TestConfig*, BunTestTasks&);
void addBunLoopPhaseTests(const TestConfig*, BunTestTasks&);
void addBunLoopVersioningTests(const TestConfig*, BunTestTasks&);
void addBunLoopPlanTests(const TestConfig*, BunTestTasks&);
void addBunCountedLoopVersioningTests(const TestConfig*, BunTestTasks&);
void addBunPlacementTests(const TestConfig*, BunTestTasks&);
void addBunHoistingTests(const TestConfig*, BunTestTasks&);
void addBunNumberingTests(const TestConfig*, BunTestTasks&);
void addBunAnalysisTests(const TestConfig*, BunTestTasks&);
void addBunRegionClonerTests(const TestConfig*, BunTestTasks&);
void addBunReductionTests(const TestConfig*, BunTestTasks&);
void addBunRangeTests(const TestConfig*, BunTestTasks&);
void addBunStoreMergingTests(const TestConfig*, BunTestTasks&);
void addBunJumpThreadingTests(const TestConfig*, BunTestTasks&);
void addBunControlFlowTests(const TestConfig*, BunTestTasks&);
void addBunSelectTests(const TestConfig*, BunTestTasks&);
void addBunSelectionTests(const TestConfig*, BunTestTasks&);
void addBunPolicyTests(const TestConfig*, BunTestTasks&);
void addBunSharedStepTests(const TestConfig*, BunTestTasks&);
void addBunRandomTests(const TestConfig*, BunTestTasks&);
void addBunHeapSetTests(const TestConfig*, BunTestTasks&);
void addBunKnownPropertyTests(const TestConfig*, BunTestTasks&);
void addBunReadOnlyDataTests(const TestConfig*, BunTestTasks&);
void addBunOptionTests(const TestConfig*, BunTestTasks&);

#endif // ENABLE(B3_JIT)
