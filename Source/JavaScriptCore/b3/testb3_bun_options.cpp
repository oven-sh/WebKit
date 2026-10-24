/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3InsertionSetInlines.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include "Options.h"
#include <algorithm>
#include <wtf/StringPrintStream.h>

namespace {

struct ProcessOption {
    ASCIILiteral name;
    void (*flip)();
    void (*restore)();
};

#define BOOL_OPTION(option) { #option ""_s, [] { Options::option() = !Options::option(); }, [] { Options::option() = !Options::option(); } }
#define UNSIGNED_OPTION(option) { #option ""_s, [] { ++Options::option(); }, [] { --Options::option(); } }
#define DOUBLE_OPTION(option) { #option ""_s, [] { Options::option() /= 2; }, [] { Options::option() *= 2; } }

constexpr ProcessOption processOptions[] = {
    UNSIGNED_OPTION(defaultB3OptLevel),
    UNSIGNED_OPTION(maxB3TailDupBlockSize),
    UNSIGNED_OPTION(maxB3TailDupBlockSuccessors),
    UNSIGNED_OPTION(maxB3WasmGCEpochSnapshotEntries),
    BOOL_OPTION(useB3SpecializeSelect),
    BOOL_OPTION(useB3TailDup),
    BOOL_OPTION(useB3CanonicalizePrePostIncrements),
    BOOL_OPTION(useB3HoistLoopInvariantValues),
    BOOL_OPTION(useB3EliminateWasmGCAllocations),
    BOOL_OPTION(useWasmFaultSignalHandler),
    BOOL_OPTION(useWasmSIMD),
    BOOL_OPTION(forceAllFunctionsToUseSIMD),
    BOOL_OPTION(airRandomizeRegs),
    UNSIGNED_OPTION(airRandomizeRegsSeed),
    DOUBLE_OPTION(rareBlockPenalty),
    BOOL_OPTION(airGreedyRegAllocSpillsEverything),
    BOOL_OPTION(airGreedyRegAllocSplitAroundLoops),
    DOUBLE_OPTION(airGreedyRegAllocSplitMultiplier),
    DOUBLE_OPTION(airGreedyRegAllocLoopSplitMaxLoopFraction),
    BOOL_OPTION(coalesceSpillSlots),
    BOOL_OPTION(useAirOptimizePairedLoadStore),
};

#undef BOOL_OPTION
#undef UNSIGNED_OPTION
#undef DOUBLE_OPTION

class ReproducibleRelocationLowering final : public RelocationLowering {
public:
    explicit ReproducibleRelocationLowering(RandomProgramState& state)
        : m_programLowering(state)
    {
    }

    Value* lower(Procedure& proc, InsertionSet& insertionSet, size_t index, const RelocatableValue& value) final
    {
        RelocationSymbol symbol = value.symbol();
        if (symbol.space != proc.policy().addressing.dataSectionSpace)
            return m_programLowering.lower(proc, insertionSet, index, value);
        int64_t placeholder = (static_cast<int64_t>(symbol.index) + 1) * placeholderBytesPerDataSection + value.addend();
        return insertionSet.insert<Const64Value>(index, value.origin(), placeholder);
    }

private:
    static constexpr int64_t placeholderBytesPerDataSection = 1ll << 32;

    RandomProgramRelocationLowering m_programLowering;
};

constexpr unsigned numberOfSeeds = 8;

using GeneratedCode = Vector<Vector<uint8_t>>;

struct ProgramSet {
    WTF_MAKE_NONCOPYABLE(ProgramSet);

    ProgramSet() = default;

    GeneratedCode compile()
    {
        GeneratedCode result;
        for (unsigned index = 0; index < numberOfSeeds; ++index) {
            Procedure proc(randomProgramPolicy(lowering));
            RandomProgram(proc, firstSeed + index, state, { });
            prepareForGeneration(proc);
            Vector<uint8_t> bytes;
            generateAndLink(proc, &bytes);
            result.append(WTF::move(bytes));
        }
        return result;
    }

    RandomProgramState state;
    ReproducibleRelocationLowering lowering { state };
    unsigned firstSeed { Options::defaultB3OptLevel() * numberOfSeeds + 1 };
};

struct ExpectedStep {
    ASCIILiteral name;
    bool isEnabled;
};

} // anonymous namespace

template<typename... Types>
static NO_RETURN_DUE_TO_CRASH void reportFailure(const ProcessOption& option, const Types&... problem)
{
    crashLock.lock();
    dataLogLn("Options::", option.name, " ", problem...);
    CRASH();
}

static void testFromProcessOptionsMirrorsOptions()
{
    const UpstreamPolicy upstream = Policy::fromProcessOptions().upstream;
    CHECK_EQ(upstream.optLevel, Options::defaultB3OptLevel());
    CHECK_EQ(upstream.maxTailDuplicationBlockSize, Options::maxB3TailDupBlockSize());
    CHECK_EQ(upstream.maxTailDuplicationBlockSuccessors, Options::maxB3TailDupBlockSuccessors());
    CHECK_EQ(upstream.maxWasmGCEpochSnapshotEntries, Options::maxB3WasmGCEpochSnapshotEntries());
    CHECK_EQ(upstream.specializesSelects, Options::useB3SpecializeSelect());
    CHECK_EQ(upstream.duplicatesTails, Options::useB3TailDup());
    CHECK_EQ(upstream.canonicalizesIncrements, Options::useB3CanonicalizePrePostIncrements());
    CHECK_EQ(upstream.hoistsLoopInvariantValues, Options::useB3HoistLoopInvariantValues());
    CHECK_EQ(upstream.eliminatesWasmGCAllocations, Options::useB3EliminateWasmGCAllocations());
    CHECK_EQ(upstream.usesWasmFaultSignalHandler, Options::useWasmFaultSignalHandler());
    CHECK_EQ(upstream.allowsSIMD, Options::useWasmSIMD());
    CHECK_EQ(upstream.forcesSIMD, Options::forceAllFunctionsToUseSIMD());
}

static void testNoProcessOptionsChangeCode()
{
    ProgramSet programs;
    const GeneratedCode expected = programs.compile();
    CHECK(expected[0] != expected[1]);

    const String jitPolicyDump = toString(Policy::fromProcessOptions());
    for (const ProcessOption& option : processOptions) {
        option.flip();
        testFromProcessOptionsMirrorsOptions();
        String changedJITPolicyDump = toString(Policy::fromProcessOptions());
        GeneratedCode found = programs.compile();
        option.restore();

        if (changedJITPolicyDump == jitPolicyDump)
            reportFailure(option, "does not reach Policy::fromProcessOptions()");
        if (toString(Policy::fromProcessOptions()) != jitPolicyDump)
            reportFailure(option, "was not restored");
        for (unsigned index = 0; index < numberOfSeeds; ++index) {
            if (found[index] != expected[index])
                reportFailure(option, "changed the code of the random program with seed ", programs.firstSeed + index, ": ", expected[index].size(), " bytes became ", found[index].size());
        }
    }
}

static bool isValidationOrDump(ASCIILiteral stepName)
{
    return stepName == "validate"_s || stepName == "dumpInitialGraph"_s || stepName == "dumpFinalGraph"_s;
}

static void testDefaultPolicyRunsUpstreamSteps()
{
    const ExpectedStep upstreamSteps[] = {
        { "resetReachability"_s, true },
        { "reduceDoubleToFloat"_s, true },
        { "specializeSelect"_s, Options::useB3SpecializeSelect() },
        { "reduceStrength"_s, true },
        { "hoistLoopInvariantValues"_s, Options::useB3HoistLoopInvariantValues() },
        { "eliminateCommonSubexpressions"_s, true },
        { "inferSwitches"_s, true },
        { "duplicateTails"_s, Options::useB3TailDup() },
        { "fixSSA"_s, true },
        { "foldPathConstants"_s, true },
        { "lowerMacros"_s, true },
        { "optimizeAssociativeExpressionTrees"_s, true },
        { "reduceStrength"_s, true },
        { "lowerMacrosAfterOptimizations"_s, true },
        { "lowerRelocatableValues"_s, true },
        { "legalizeMemoryOffsets"_s, true },
        { "moveConstants"_s, true },
        { "legalizeMemoryOffsets"_s, true },
        { "eliminateDeadCode"_s, true },
        { "canonicalizePrePostIncrements"_s, Options::useB3CanonicalizePrePostIncrements() },
        { "lowerToAir"_s, true },
    };
    Vector<ASCIILiteral> expected;
    for (const ExpectedStep& step : upstreamSteps) {
        if (step.isEnabled)
            expected.append(step.name);
    }

    Procedure proc;
    proc.setOptLevel(2);
    Vector<ASCIILiteral> found = listEnabledSteps(proc);
    found.removeAllMatching(isValidationOrDump);
    if (!std::ranges::equal(found, expected)) {
        crashLock.lock();
        dataLogLn("Expected the default policy to run ", listDump(expected), " but it runs ", listDump(found));
        CRASH();
    }

    Procedure threading(policyWith([](Policy& policy) {
        policy.optimization.threadsJumps = true;
    }));
    Vector<ASCIILiteral> threadingSteps = listEnabledSteps(threading);
    CHECK(threadingSteps.contains("threadJumps"_s));
    CHECK(!threadingSteps.contains("duplicateTails"_s));
}

void addBunOptionTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN_NOW(testNoProcessOptionsChangeCode());
    RUN(testFromProcessOptionsMirrorsOptions());
    RUN(testDefaultPolicyRunsUpstreamSteps());
}

#endif // ENABLE(B3_JIT)
