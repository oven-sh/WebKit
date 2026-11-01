/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3ConvertBranchesToSelects.h"
#include "B3CycleForest.h"
#include "B3EliminateDeadCode.h"
#include "B3EliminateRedundantPhiCycles.h"
#include "B3FixSSA.h"
#include "B3FoldComparisonsWithBounds.h"
#include "B3FoldPathConstants.h"
#include "B3HoistLoopInvariantValues.h"
#include "B3InferSwitches.h"
#include "B3MathExtras.h"
#include "B3MergeAdjacentConstantStores.h"
#include "B3ReduceStrength.h"
#include "B3RotateLoops.h"
#include "B3SimplifyTerminals.h"
#include "B3SinkValues.h"
#include "B3SplitBranchConditions.h"
#include "B3ThreadJumps.h"
#include "B3Validate.h"
#include "B3VersionLoops.h"
#include <wtf/IndexMap.h>

namespace {

class RandomProgramEvaluator {
public:
    RandomProgramEvaluator(Procedure& proc, const PatchpointModels& models, RandomProgramState& state)
        : m_proc(proc)
        , m_models(models)
        , m_state(state)
    {
        m_values.fill(0, proc.values().size());
        m_variables.fill(0, proc.variables().size());
    }

    int64_t run(int64_t first, int64_t* pointer, int64_t second)
    {
        m_arguments = { static_cast<uint64_t>(first), std::bit_cast<uint64_t>(pointer), static_cast<uint64_t>(second) };
        BasicBlock* block = m_proc.at(0);
        for (;;) {
            for (Value* value : *block)
                evaluate(value);
            if (m_isDone)
                return m_result;
            block = block->successorBlock(m_successor);
        }
    }

private:
    uint64_t in(Value* value, unsigned index) const { return m_values[value->child(index)->index()]; }
    double number(Value* value, unsigned index) const { return std::bit_cast<double>(in(value, index)); }
    int64_t integer(Value* value, unsigned index) const
    {
        uint64_t bits = in(value, index);
        return value->child(index)->type() == Int32 ? static_cast<int32_t>(bits) : static_cast<int64_t>(bits);
    }

    void set(Value* value, uint64_t bits) { m_values[value->index()] = value->type() == Int32 ? static_cast<uint32_t>(bits) : bits; }
    void set(Value* value, double result) { m_values[value->index()] = std::bit_cast<uint64_t>(result); }

    void finish(int64_t result)
    {
        m_isDone = true;
        m_result = result;
    }

    template<typename T>
    static T& at(uint64_t address) { return *std::bit_cast<T*>(address); }

    static int64_t truncate(double value)
    {
        constexpr double limit = 9223372036854775808.0;
        if (value >= -limit && value < limit)
            return static_cast<int64_t>(value);
        if (isX86() || value < 0)
            return std::numeric_limits<int64_t>::min();
        return value != value ? 0 : std::numeric_limits<int64_t>::max();
    }

    template<typename Functor>
    void arithmetic(Value* value, const Functor& functor)
    {
        if (value->type() == Double)
            set(value, functor(number(value, 0), number(value, 1)));
        else
            set(value, functor(in(value, 0), in(value, 1)));
    }

    template<typename Functor>
    void compare(Value* value, const Functor& functor)
    {
        if (value->child(0)->type() == Double)
            set(value, static_cast<uint64_t>(functor(number(value, 0), number(value, 1))));
        else
            set(value, static_cast<uint64_t>(functor(integer(value, 0), integer(value, 1))));
    }

    template<typename Functor>
    void compareUnsigned(Value* value, const Functor& functor) { set(value, static_cast<uint64_t>(functor(in(value, 0), in(value, 1)))); }

    void evaluatePatchpoint(Value* value)
    {
        PatchpointModel model = m_models.get(value);
        switch (model.kind) {
        case PatchpointModel::Kind::Nothing:
            return;
        case PatchpointModel::Kind::Constant:
            set(value, static_cast<uint64_t>(model.immediate));
            return;
        case PatchpointModel::Kind::AddConstant:
            set(value, in(value, 0) + static_cast<uint64_t>(model.immediate));
            return;
        case PatchpointModel::Kind::LoadWord:
            set(value, at<uint64_t>(in(value, 0) + model.immediate));
            return;
        case PatchpointModel::Kind::IncrementWord:
            ++at<uint64_t>(in(value, 0) + model.immediate);
            return;
        case PatchpointModel::Kind::TruncateDouble:
            set(value, static_cast<uint64_t>(truncate(number(value, 0))));
            return;
        case PatchpointModel::Kind::Observable: {
            uint64_t hash = static_cast<uint64_t>(m_state.effectHash) * 3 + static_cast<uint64_t>(model.immediate) + in(value, 0);
            for (unsigned i = 1; i < value->numChildren(); ++i)
                hash = hash * 2 + in(value, i);
            m_state.effectHash = static_cast<int64_t>(hash);
            set(value, in(value, 0) + 3);
            m_successor = static_cast<unsigned>(in(value, 0) & 1);
            return;
        }
        case PatchpointModel::Kind::Poll:
            ++m_state.poll.numberOfPolls;
            if (m_state.poll.limit < 0) {
                ++m_state.poll.numberOfStops;
                m_state.poll.limit = 0;
            }
            return;
        }
    }

    bool tryEvaluateConversion(Value* value)
    {
        switch (value->opcode()) {
        case Identity:
        case Opaque:
        case BitwiseCast:
        case Trunc:
        case ZExt32:
            set(value, in(value, 0));
            return true;
        case SExt8:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(in(value, 0)))));
            return true;
        case SExt16:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(in(value, 0)))));
            return true;
        case SExt32:
            set(value, static_cast<uint64_t>(integer(value, 0)));
            return true;
        case IToD:
            set(value, static_cast<double>(integer(value, 0)));
            return true;
        default:
            return false;
        }
    }

    bool tryEvaluateArithmetic(Value* value)
    {
        switch (value->opcode()) {
        case Add:
            arithmetic(value, [](auto left, auto right) { return left + right; });
            return true;
        case Sub:
            arithmetic(value, [](auto left, auto right) { return left - right; });
            return true;
        case Mul:
            arithmetic(value, [](auto left, auto right) { return left * right; });
            return true;
        case Div:
            RELEASE_ASSERT(value->kind().isChill() && value->type() == Int64);
            set(value, static_cast<uint64_t>(chillDiv(integer(value, 0), integer(value, 1))));
            return true;
        case Mod:
            RELEASE_ASSERT(value->kind().isChill() && value->type() == Int64);
            set(value, static_cast<uint64_t>(chillMod(integer(value, 0), integer(value, 1))));
            return true;
        case Neg:
            if (value->type() == Double)
                set(value, -number(value, 0));
            else
                set(value, -in(value, 0));
            return true;
        case Abs:
            set(value, std::fabs(number(value, 0)));
            return true;
        case BitAnd:
            set(value, in(value, 0) & in(value, 1));
            return true;
        case BitOr:
            set(value, in(value, 0) | in(value, 1));
            return true;
        case BitXor:
            set(value, in(value, 0) ^ in(value, 1));
            return true;
        case Shl:
            set(value, in(value, 0) << (in(value, 1) & (value->type() == Int32 ? 31 : 63)));
            return true;
        case SShr:
            set(value, static_cast<uint64_t>(integer(value, 0) >> (in(value, 1) & (value->type() == Int32 ? 31 : 63))));
            return true;
        case ZShr:
            set(value, in(value, 0) >> (in(value, 1) & (value->type() == Int32 ? 31 : 63)));
            return true;
        case Select:
            set(value, in(value, 0) ? in(value, 1) : in(value, 2));
            return true;
        default:
            return false;
        }
    }

    bool tryEvaluateComparison(Value* value)
    {
        switch (value->opcode()) {
        case Equal:
            compare(value, [](auto left, auto right) { return left == right; });
            return true;
        case NotEqual:
            compare(value, [](auto left, auto right) { return left != right; });
            return true;
        case LessThan:
            compare(value, [](auto left, auto right) { return left < right; });
            return true;
        case GreaterThan:
            compare(value, [](auto left, auto right) { return left > right; });
            return true;
        case LessEqual:
            compare(value, [](auto left, auto right) { return left <= right; });
            return true;
        case GreaterEqual:
            compare(value, [](auto left, auto right) { return left >= right; });
            return true;
        case Above:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left > right; });
            return true;
        case Below:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left < right; });
            return true;
        case AboveEqual:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left >= right; });
            return true;
        case BelowEqual:
            compareUnsigned(value, [](uint64_t left, uint64_t right) { return left <= right; });
            return true;
        default:
            return false;
        }
    }

    bool tryEvaluateMemoryAccess(Value* value)
    {
        switch (value->opcode()) {
        case Load: {
            uint64_t address = in(value, 0) + value->as<MemoryValue>()->offset();
            set(value, value->type() == Int32 ? at<uint32_t>(address) : at<uint64_t>(address));
            return true;
        }
        case Load8Z:
            set(value, static_cast<uint64_t>(at<uint8_t>(in(value, 0) + value->as<MemoryValue>()->offset())));
            return true;
        case Load8S:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(at<int8_t>(in(value, 0) + value->as<MemoryValue>()->offset()))));
            return true;
        case Load16Z:
            set(value, static_cast<uint64_t>(at<uint16_t>(in(value, 0) + value->as<MemoryValue>()->offset())));
            return true;
        case Load16S:
            set(value, static_cast<uint64_t>(static_cast<int64_t>(at<int16_t>(in(value, 0) + value->as<MemoryValue>()->offset()))));
            return true;
        case Store: {
            uint64_t address = in(value, 1) + value->as<MemoryValue>()->offset();
            if (value->child(0)->type() == Int32)
                at<uint32_t>(address) = static_cast<uint32_t>(in(value, 0));
            else
                at<uint64_t>(address) = in(value, 0);
            return true;
        }
        case Store8:
            at<uint8_t>(in(value, 1) + value->as<MemoryValue>()->offset()) = static_cast<uint8_t>(in(value, 0));
            return true;
        case Store16:
            at<uint16_t>(in(value, 1) + value->as<MemoryValue>()->offset()) = static_cast<uint16_t>(in(value, 0));
            return true;
        default:
            return false;
        }
    }

    void evaluate(Value* value)
    {
        if (m_isDone)
            return;
        if (tryEvaluateConversion(value) || tryEvaluateArithmetic(value) || tryEvaluateComparison(value) || tryEvaluateMemoryAccess(value))
            return;
        switch (value->opcode()) {
        case Nop:
        case Fence:
            return;
        case Const32:
            set(value, static_cast<uint64_t>(value->asInt32()));
            return;
        case Const64:
            set(value, static_cast<uint64_t>(value->asInt64()));
            return;
        case ConstDouble:
            set(value, value->asDouble());
            return;
        case ArgumentReg:
            for (unsigned i = 0; i < m_arguments.size(); ++i) {
                if (value->as<ArgumentRegValue>()->argumentReg() == Reg(GPRInfo::toArgumentRegister(i)))
                    set(value, m_arguments[i]);
            }
            return;
        case B3::Get:
            set(value, m_variables[value->as<VariableValue>()->variable()->index()]);
            return;
        case B3::Set:
            m_variables[value->as<VariableValue>()->variable()->index()] = in(value, 0);
            return;
        case CCall:
            set(value, static_cast<uint64_t>(randomProgramCallee(std::bit_cast<RandomProgramState*>(in(value, 1)), static_cast<int64_t>(in(value, 2)))));
            return;
        case Patchpoint:
            evaluatePatchpoint(value);
            return;
        case Check:
            if (!in(value, 0))
                return;
            for (unsigned i = 1; i < value->numChildren(); ++i)
                m_state.failedCheckOperands[i - 1] = static_cast<int64_t>(in(value, i));
            finish(RandomProgram::failedCheckResult);
            return;
        case Jump:
            m_successor = 0;
            return;
        case Branch:
            m_successor = !in(value, 0);
            return;
        case Switch: {
            SwitchValue* switchValue = value->as<SwitchValue>();
            m_successor = switchValue->numCaseValues();
            for (unsigned i = 0; i < switchValue->numCaseValues(); ++i) {
                if (switchValue->caseValue(i) == integer(value, 0))
                    m_successor = i;
            }
            return;
        }
        case Return:
            finish(static_cast<int64_t>(in(value, 0)));
            return;
        default:
            dataLogLn("The evaluator does not know ", deepDump(m_proc, value));
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    Procedure& m_proc;
    const PatchpointModels& m_models;
    RandomProgramState& m_state;
    Vector<uint64_t> m_values;
    Vector<uint64_t> m_variables;
    std::array<uint64_t, 3> m_arguments { };
    unsigned m_successor { 0 };
    bool m_isDone { false };
    int64_t m_result { 0 };
};

static void evaluate(RandomProgramVersion& version, const PatchpointModels& models, int64_t first, int64_t second, const Vector<int64_t>& initialMemory, int64_t limit)
{
    version.prepare(initialMemory, limit);
    version.result = RandomProgramEvaluator(version.proc, models, version.state).run(first, version.memory.mutableSpan().subspan(RandomProgram::numberOfGuardWords).data(), second);
}

class RandomProgramChecker {
public:
    explicit RandomProgramChecker(unsigned seed, bool hasCallShapes = true, bool disablesEachSwitch = false)
        : m_seed(seed)
        , m_hasCallShapes(hasCallShapes)
        , m_disablesEachSwitch(disablesEachSwitch)
        , m_random(seed + 0x9e3779b9u)
        , m_policyRandom(seed + 0x27d4eb2fu)
    {
    }

    void run()
    {
        RandomProgramVersion reference("at level 0"_s);
        RandomProgram(reference.proc, m_seed, reference.state, { m_hasCallShapes, true });
        m_uncloneableValueCount = numberOfValues(reference.proc, isUncloneable);
        checkCycleForest(reference.proc);
        reference.code = compileProc(reference.proc, 0);

        RandomProgramVersion evaluated("when evaluated"_s);
        PatchpointModels models;
        RandomProgram(evaluated.proc, m_seed, evaluated.state, { m_hasCallShapes, false }, &models);

        Vector<std::unique_ptr<RandomProgramVersion>> versions;
        versions.append(makeVersion("at level 2"_s, { }));
        versions.append(makeVersion("at level 1"_s, { }));
        versions.append(makeVersion("after a random subset of the phases, then at level 0"_s, { .isForLevel0 = true }));
        versions.append(makeVersion("after a random subset of the phases, then at level 2"_s, { }));
        versions.append(makeVersion("after a random subset of the phases, then at level 2 with known properties checked at run time"_s, { .checksKnownProperties = true }));
        versions[0]->code = compileAndCheckReport(versions[0]->proc, 2);
        versions[1]->code = compileAndCheckReport(versions[1]->proc, 1);
        runRandomPhases(versions[2]->proc, versions[2]->name);
        versions[2]->code = compileAndCheckReport(versions[2]->proc, 0);
        runRandomPhases(versions[3]->proc, versions[3]->name);
        versions[3]->code = compileAndCheckReport(versions[3]->proc, 2);
        runRandomPhases(versions[4]->proc, versions[4]->name);
        versions[4]->code = compileAndCheckReport(versions[4]->proc, 2);

        auto defaultPolicyVersion = makeUniqueWithoutFastMallocCheck<RandomProgramVersion>("at level 2 under a default policy"_s);
        RandomProgram(defaultPolicyVersion->proc, m_seed, defaultPolicyVersion->state, { m_hasCallShapes, false });
        defaultPolicyVersion->code = compileAndCheckReport(defaultPolicyVersion->proc, 2);
        versions.append(WTF::move(defaultPolicyVersion));

        auto appendVersionWithoutSwitch = [&](PolicySwitch disabledSwitch) {
            auto version = makeVersion("at level 2 with one switch off"_s, { .disabledSwitch = disabledSwitch });
            version->code = compileAndCheckReport(version->proc, 2);
            versions.append(WTF::move(version));
        };
        if (m_disablesEachSwitch) {
            for (unsigned i = 0; i < numberOfSwitches; ++i)
                appendVersionWithoutSwitch(static_cast<PolicySwitch>(i));
        } else
            appendVersionWithoutSwitch(static_cast<PolicySwitch>(m_seed % numberOfSwitches));

        static constexpr int64_t arguments[] = { 0, 1, -1, 2, 5, 13, 31, 100, 65537, 0x7fffffff, -0x80000000ll, 0x123456789ll, std::numeric_limits<int64_t>::min() };
        for (unsigned round = 0; round < 6; ++round) {
            Vector<int64_t> memory = randomInitialMemory(m_random);
            int64_t first = arguments[m_random.getUint32(std::size(arguments))];
            int64_t second = arguments[m_random.getUint32(std::size(arguments))];
            int64_t limit = round & 1 ? -1 : 0;

            reference.call(first, second, memory, limit);
            evaluate(evaluated, models, first, second, memory, limit);
            if (!evaluated.agreesWith(reference))
                fail(makeString("in round "_s, round, ' ', evaluated.differenceFrom(reference, first, second)));
            for (auto& version : versions) {
                version->call(first, second, memory, limit);
                if (version->agreesWith(reference))
                    continue;
                fail(makeString("in round "_s, round, ' ', version->differenceFrom(reference, first, second)));
            }
        }
    }

private:
    struct VersionOptions {
        bool isForLevel0 { false };
        bool checksKnownProperties { false };
        std::optional<PolicySwitch> disabledSwitch { std::nullopt };
    };

    NO_RETURN_DUE_TO_CRASH void fail(const String& what)
    {
        crashLock.lock();
        RandomProgramState state;
        Procedure proc;
        RandomProgram(proc, m_seed, state, { m_hasCallShapes, true });
        dataLogLn("The random program with seed ", m_seed, " ", what, ":\n", proc);
        CRASH();
    }

    std::unique_ptr<RandomProgramVersion> makeVersion(ASCIILiteral name, VersionOptions options)
    {
        auto version = makeUniqueWithoutFastMallocCheck<RandomProgramVersion>(name, [&](RelocationLowering& lowering) {
            Policy policy = randomProgramPolicy(lowering);
            policy.validation.checksKnownPropertiesAtRunTime = options.checksKnownProperties;
            if (!chance(70)) {
                policy.air.registers.chargesForCalleeSaves = false;
                policy.air.layout.placesRareBlocksInGaps = false;
                policy.air.layout.recordsRareCodeStart = false;
            }
            if (chance(50)) {
                policy.air.registers.pinned.add(GPRInfo::numberTagRegister, IgnoreVectors);
                policy.air.registers.pinned.add(GPRInfo::notCellMaskRegister, IgnoreVectors);
            }
            auto policyChance = [&] {
                return m_policyRandom.getUint32(100) < 50;
            };
            if (policyChance())
                policy.optimization.returnsAreUnlikely = true;
            if (policyChance())
                policy.air.layout.placesRareBlocksAfterGuards = true;
            if (policyChance())
                policy.air.layout.makesGapsForRareBlocks = true;
            if (policyChance())
                policy.air.layout.duplicatesReturns = true;
            if (policyChance())
                policy.allocation.usesArena = false;
            if (options.disabledSwitch)
                policy.disable(*options.disabledSwitch);
            return policyWithInspection(WTF::move(policy), [this, name](Procedure& optimized) {
                didRunPhase(optimized, name);
            });
        });
        version->disabledSwitch = options.disabledSwitch;
        RandomProgram(version->proc, m_seed, version->state, { m_hasCallShapes, options.isForLevel0 });
        return version;
    }

    void didRunPhase(Procedure& proc, ASCIILiteral name)
    {
        validate(proc);
        if (numberOfValues(proc, isUncloneable) > m_uncloneableValueCount)
            fail(makeString("has more values that must not be cloned than it started with "_s, name));
    }

    void checkCycleForest(Procedure& proc)
    {
        CycleForest forest(proc);
        auto require = [&](bool condition, ASCIILiteral what) {
            if (!condition)
                fail(makeString("has an invalid cycle forest: "_s, what));
        };
        auto reaches = [&](BasicBlock* from, BasicBlock* to, const auto& mayPass) {
            IndexSet<BasicBlock*> seen;
            Vector<BasicBlock*> worklist { from };
            while (!worklist.isEmpty()) {
                for (BasicBlock* successor : worklist.takeLast()->successorBlocks()) {
                    if (successor == to)
                        return true;
                    if (mayPass(successor) && seen.add(successor))
                        worklist.append(successor);
                }
            }
            return false;
        };

        bool isReducible = true;
        for (unsigned cycle = 0; cycle < forest.size(); ++cycle) {
            auto& entries = forest[cycle].entries;
            BasicBlock* header = forest[cycle].header();
            unsigned parent = forest[cycle].parent;
            auto isMember = [&](BasicBlock* block) {
                return forest.contains(cycle, block);
            };
            isReducible &= forest[cycle].isReducible();
            require(isMember(header), "a header is outside its cycle"_s);
            require(forest[cycle].depth == forest.depth(parent) + 1, "a cycle is not one deeper than its parent"_s);
            require(reaches(header, header, isMember), "a header does not reach itself"_s);
            for (BasicBlock* block : proc) {
                if (!isMember(block)) {
                    require(!entries.contains(block), "an entry is outside its cycle"_s);
                    continue;
                }
                require(parent == CycleForest::none || (forest.contains(parent, block) && block != forest[parent].header()), "a cycle is not inside its parent"_s);
                require(block == header || (reaches(header, block, isMember) && reaches(block, header, isMember)), "a cycle is not strongly connected"_s);
                bool hasOutsidePredecessor = false;
                for (BasicBlock* predecessor : block->predecessors())
                    hasOutsidePredecessor |= !isMember(predecessor);
                require(entries.contains(block) == (hasOutsidePredecessor || block == header), "the entries of a cycle are not the blocks with outside predecessors"_s);
                if (forest.innermost(block) == cycle && block != header) {
                    require(!reaches(block, block, [&](BasicBlock* other) {
                        return isMember(other) && other != header;
                    }), "an inner cycle is missing"_s);
                }
            }
        }
        for (BasicBlock* block : proc) {
            if (forest.innermost(block) == CycleForest::none) {
                require(!reaches(block, block, [](BasicBlock*) {
                    return true;
                }), "a cycle is missing"_s);
            }
        }

        IndexMap<BasicBlock*, unsigned> position(proc.size(), UINT_MAX);
        unsigned positionCount = 0;
        for (BasicBlock* block : forest.nodesInPostOrder()) {
            require(position[block] == UINT_MAX, "a block is in the post order twice"_s);
            position[block] = positionCount++;
        }
        require(positionCount == numberOfBlocks(proc), "a block is missing from the post order"_s);
        for (BasicBlock* block : proc) {
            for (BasicBlock* successor : block->successorBlocks()) {
                unsigned cycle = forest.innermost(successor);
                unsigned common = forest.nearestCommon(forest.innermost(block), cycle);
                BasicBlock* target = successor;
                if (cycle == common) {
                    if (forest.isHeader(successor))
                        continue;
                } else {
                    while (forest[cycle].parent != common)
                        cycle = forest[cycle].parent;
                    target = forest[cycle].header();
                }
                require(position[target] < position[block], "a block comes before one of its successors in the post order"_s);
            }
        }

        if (!isReducible)
            return;
        B3::NaturalLoops& loops = proc.naturalLoops();
        require(loops.numLoops() == forest.size(), "the number of cycles differs from the number of natural loops"_s);
        for (BasicBlock* block : proc) {
            const B3::NaturalLoop* loop = loops.innerMostLoopOf(block);
            unsigned cycle = forest.innermost(block);
            require(!!loop == (cycle != CycleForest::none), "a block is in a cycle but not in a natural loop, or vice versa"_s);
            require(!loop || loop->header() == forest[cycle].header(), "a cycle and its natural loop have different headers"_s);
        }
    }

    bool chance(unsigned percent) { return m_random.getUint32(100) < percent; }

    template<typename Functor>
    bool maybe(unsigned percent, Procedure& proc, ASCIILiteral name, const Functor& phase)
    {
        if (!chance(percent))
            return false;
        bool result = true;
        if constexpr (std::is_void_v<decltype(phase())>)
            phase();
        else
            result = phase();
        didRunPhase(proc, name);
        return result;
    }

    void runRandomPhases(Procedure& proc, ASCIILiteral name)
    {
        auto always = [&](const auto& phase) {
            return maybe(100, proc, name, phase);
        };
        auto sometimes = [&](const auto& phase) {
            return maybe(50, proc, name, phase);
        };
        auto often = [&](const auto& phase) {
            return maybe(75, proc, name, phase);
        };
        auto runUpdateFrequencies = [&] { proc.updateFrequencies(); };
        auto runFixSSA = [&] { return fixSSA(proc); };
        auto runHoisting = [&] { return hoistLoopInvariantValues(proc); };
        auto runFoldPathConstants = [&] { return foldPathConstants(proc); };
        auto runSimplifyTerminals = [&] { return simplifyTerminals(proc); };
        auto runEliminateDeadCode = [&] { return eliminateDeadCode(proc); };
        auto runEliminatePhiCycles = [&] { return eliminateRedundantPhiCycles(proc); };
        auto runFoldComparisonsWithBounds = [&] { return foldComparisonsWithBounds(proc); };

        proc.setOptLevel(2);
        proc.resetReachability();
        sometimes([&] { return splitBranchConditions(proc); });
        sometimes(runUpdateFrequencies);
        sometimes([&] { return reduceStrength(proc, ReduceStrengthPass::Initial); });
        sometimes([&] { return inferSwitches(proc); });
        sometimes([&] { return threadJumps(proc); });
        always(runFixSSA);
        sometimes(runFoldPathConstants);
        sometimes(runUpdateFrequencies);
        sometimes(runSimplifyTerminals);
        sometimes(runEliminatePhiCycles);
        sometimes(runHoisting);
        if (often([&] { return rotateLoops(proc); })) {
            always(runFixSSA);
            sometimes(runHoisting);
            sometimes(runFoldPathConstants);
            sometimes(runSimplifyTerminals);
            sometimes(runEliminateDeadCode);
        }
        if (often(runFoldComparisonsWithBounds)) {
            sometimes(runSimplifyTerminals);
            sometimes(runEliminateDeadCode);
        }
        Vector<BasicBlock*> topTestedVersionHeaders;
        if (often([&] { return versionLoops(proc, topTestedVersionHeaders); })) {
            always(runFixSSA);
            sometimes(runEliminatePhiCycles);
            sometimes(runEliminateDeadCode);
            if (!topTestedVersionHeaders.isEmpty() && often([&] { return rotateLoops(proc, &topTestedVersionHeaders); })) {
                always(runFixSSA);
                sometimes(runFoldPathConstants);
                sometimes(runSimplifyTerminals);
                sometimes(runEliminateDeadCode);
                if (sometimes(runFoldComparisonsWithBounds)) {
                    sometimes(runSimplifyTerminals);
                    sometimes(runEliminateDeadCode);
                }
            }
        }
        often([&] { return convertBranchesToSelects(proc); });
        sometimes([&] { return mergeAdjacentConstantStores(proc); });
        sometimes([&] {
            proc.updateFrequencies();
            return sinkValues(proc);
        });
    }

    unsigned m_seed;
    bool m_hasCallShapes;
    bool m_disablesEachSwitch;
    WeakRandom m_random;
    WeakRandom m_policyRandom;
    unsigned m_uncloneableValueCount { 0 };
};

static void testRandomPrograms(unsigned part)
{
    constexpr unsigned numberOfParts = 16;
    constexpr unsigned numberOfProgramsInPart = 60;
    unsigned firstSeed = (Options::defaultB3OptLevel() * numberOfParts + part) * numberOfProgramsInPart;
    for (unsigned i = 0; i < numberOfProgramsInPart; ++i)
        RandomProgramChecker(firstSeed + i).run();
}

static void testRandomProgramRegressions()
{
    for (unsigned seed : { 198695u, 209039u, 343103u, 371708u, 471095u, 537269u, 835893u, 872292u, 1108466u, 1306836u, 1516518u, 1694723u, 1931155u, 2322767u, 2531676u, 3410061u, 3489678u }) {
        RandomProgramChecker(seed, false, true).run();
        RandomProgramChecker(seed, true, true).run();
    }
    for (unsigned seed : { 31131u, 41501u, 12388748u, 13785840u })
        RandomProgramChecker(seed).run();
}

static void testRandomProgramVersionsCanDisagree()
{
    constexpr int64_t first = 5;
    constexpr int64_t second = 13;
    constexpr int64_t limit = 0;
    constexpr RandomProgramOptions options { true, true };

    RandomProgramVersion reference("reference"_s);
    RandomProgramVersion other("other"_s);
    RandomProgram(reference.proc, 1, reference.state, options);
    RandomProgram(other.proc, 1, other.state, options);
    reference.code = compileAndCheckReport(reference.proc, 0);
    other.code = compileAndCheckReport(other.proc, 0);

    WeakRandom random(1);
    Vector<int64_t> memory = randomInitialMemory(random);
    reference.call(first, second, memory, limit);
    other.call(first, second, memory, limit);
    CHECK(other.agreesWith(reference));

    bool hasDifferentResult = false;
    for (int64_t delta = 1; delta <= 5 && !hasDifferentResult; ++delta) {
        other.call(first + delta, second, memory, limit);
        hasDifferentResult = other.result != reference.result;
    }
    CHECK(hasDifferentResult);
    CHECK(!other.agreesWith(reference));

    other.call(first, second, memory, limit);
    CHECK(other.agreesWith(reference));
    other.memory[0] = ~RandomProgramVersion::guardWord;
    CHECK(!other.hasIntactGuards());
}

} // anonymous namespace

void addBunRandomTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testRandomPrograms(0));
    RUN(testRandomPrograms(1));
    RUN(testRandomPrograms(2));
    RUN(testRandomPrograms(3));
    RUN(testRandomPrograms(4));
    RUN(testRandomPrograms(5));
    RUN(testRandomPrograms(6));
    RUN(testRandomPrograms(7));
    RUN(testRandomPrograms(8));
    RUN(testRandomPrograms(9));
    RUN(testRandomPrograms(10));
    RUN(testRandomPrograms(11));
    RUN(testRandomPrograms(12));
    RUN(testRandomPrograms(13));
    RUN(testRandomPrograms(14));
    RUN(testRandomPrograms(15));
    RUN(testRandomProgramRegressions());
    RUN(testRandomProgramVersionsCanDisagree());
}

#endif // ENABLE(B3_JIT)
