/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLoadedProgramTestSupport.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeTable.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <wtf/FastMalloc.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> runtimeCounterTests();

static constexpr ASCIILiteral programText =
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    Return\n"
    "}\n"_s;

namespace {

struct CountLine {
    uint64_t count;
    String kind;
    String name;
};

} // anonymous namespace

static Vector<CountLine> dumpedLines(VM& vm)
{
    StringPrintStream out;
    dumpCounts(vm, out);
    String text = out.toString();
    AOT_TEST_CHECK(text.endsWith('\n'));

    Vector<CountLine> lines;
    for (StringView line : StringView(text).left(text.length() - 1).splitAllowingEmptyEntries('\n')) {
        size_t tab = line.find('\t');
        AOT_TEST_CHECK(tab != notFound);
        size_t colon = line.find(':', tab);
        AOT_TEST_CHECK(colon != notFound);
        std::optional<uint64_t> count = parseInteger<uint64_t>(line.left(tab));
        AOT_TEST_CHECK(count);
        lines.append({ *count, line.substring(tab + 1, colon - tab - 1).toString(), line.substring(colon + 1).toString() });
    }
    return lines;
}

static Vector<CountLine> linesOfKind(const Vector<CountLine>& lines, ASCIILiteral kind)
{
    Vector<CountLine> result;
    for (const CountLine& line : lines) {
        if (line.kind == kind)
            result.append(line);
    }
    return result;
}

static uint64_t dumpedCount(VM& vm, ASCIILiteral kind, ASCIILiteral name)
{
    std::optional<uint64_t> found;
    for (const CountLine& line : dumpedLines(vm)) {
        if (line.kind != kind || line.name != name)
            continue;
        AOT_TEST_CHECK(!found);
        found = line.count;
    }
    AOT_TEST_CHECK(found);
    return *found;
}

namespace {

class BranchCounts {
    WTF_MAKE_NONCOPYABLE(BranchCounts);
public:
    explicit BranchCounts(LoadedProgram& program)
        : m_words(program.instance().runtime.counters)
        , m_size(static_cast<size_t>(runtimeLimitValue(program.vm(), LimitID::BranchCounters)))
    {
        AOT_TEST_CHECK(!m_words.branchCounts);
        m_words.branchCounts = static_cast<uint64_t*>(fastZeroedMalloc(m_size * sizeof(uint64_t)));
    }

    ~BranchCounts()
    {
        fastFree(m_words.branchCounts);
        m_words.branchCounts = nullptr;
    }

    size_t size() const { return m_size; }
    uint64_t& operator[](size_t index) { return m_words.branchCounts[index]; }

private:
    CounterWords& m_words;
    size_t m_size;
};

} // anonymous namespace

static void testFiveKindsInOrder(LoadedProgram& program)
{
    VM& vm = program.vm();
    RuntimeTable& table = *vm.aotState().runtimeTable;
    BranchCounts branchCounts(program);

    count(vm, RuntimeCounterID::SiteCacheRefills);
    count(program.instance(), RuntimeCounterID::StubCannotCallSetter);
    countLimitHit(vm, LimitID::HashedTableEntries);
    table.didCall(Entry::operationAOTValueAdd);
    branchCounts[5] = 9;

    static constexpr ASCIILiteral kindsInOrder[] = { "counter"_s, "exit"_s, "limit"_s, "operation"_s, "made"_s, "branch"_s };
    size_t kind = 0;
    unsigned numberOfLinesOfKind[std::size(kindsInOrder)] = { };
    for (const CountLine& line : dumpedLines(vm)) {
        while (kind < std::size(kindsInOrder) && line.kind != kindsInOrder[kind])
            ++kind;
        AOT_TEST_CHECK(kind < std::size(kindsInOrder));
        ++numberOfLinesOfKind[kind];
    }
    for (unsigned numberOfLines : numberOfLinesOfKind)
        AOT_TEST_CHECK(numberOfLines);
}

static void testCountersAndExitsAreAllListed(LoadedProgram& program)
{
    VM& vm = program.vm();
    uint64_t refillsBefore = valueOf(vm, RuntimeCounterID::SiteCacheRefills);
    count(vm, RuntimeCounterID::SiteCacheRefills);
    count(vm, RuntimeCounterID::SiteCacheRefills);
    count(vm, RuntimeCounterID::StubCannotGrowStorage);

    Vector<CountLine> lines = dumpedLines(vm);
    Vector<CountLine> counters = linesOfKind(lines, "counter"_s);
    Vector<CountLine> exits = linesOfKind(lines, "exit"_s);
    size_t nextCounter = 0;
    size_t nextExit = 0;
    for (RuntimeCounterID counter : allMembersOf<RuntimeCounterID>) {
        if (counter == RuntimeCounterID::None)
            continue;
        Vector<CountLine>& expectedIn = countsStubExit(counter) ? exits : counters;
        size_t& next = countsStubExit(counter) ? nextExit : nextCounter;
        AOT_TEST_CHECK(next < expectedIn.size());
        AOT_TEST_CHECK(expectedIn[next].name == Wire::nameOf(counter));
        AOT_TEST_CHECK(expectedIn[next].count == valueOf(vm, counter));
        ++next;
    }
    AOT_TEST_CHECK(nextCounter == counters.size() && nextExit == exits.size());
    AOT_TEST_CHECK(exits.size() == 6);

    AOT_TEST_CHECK(dumpedCount(vm, "counter"_s, "SiteCacheRefills"_s) == refillsBefore + 2);
    AOT_TEST_CHECK(dumpedCount(vm, "exit"_s, "StubCannotGrowStorage"_s) == 1);
    AOT_TEST_CHECK(!dumpedCount(vm, "exit"_s, "StubCannotReadField"_s));
    AOT_TEST_CHECK(!dumpedCount(vm, "counter"_s, "ClassesEvaluatedAgain"_s));
}

static void testRunTimeLimitsAreAllListed(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(!limitHitsOf(vm, LimitID::CustomAccessorRecords));
    countLimitHit(vm, LimitID::CustomAccessorRecords);
    countLimitHit(vm, LimitID::CustomAccessorRecords);
    countLimitHit(vm, LimitID::CustomAccessorRecords);
    AOT_TEST_CHECK(limitHitsOf(vm, LimitID::CustomAccessorRecords) == 3);

    Vector<CountLine> limits = linesOfKind(dumpedLines(vm), "limit"_s);
    size_t next = 0;
    for (LimitID limit : allMembersOf<LimitID>) {
        if (stageOf(limit) != Stage::RunTime)
            continue;
        AOT_TEST_CHECK(next < limits.size());
        AOT_TEST_CHECK(limits[next].name == Wire::nameOf(limit));
        AOT_TEST_CHECK(limits[next].count == limitHitsOf(vm, limit));
        ++next;
    }
    AOT_TEST_CHECK(next == limits.size() && next > 2);
    AOT_TEST_CHECK(dumpedCount(vm, "limit"_s, "CustomAccessorRecords"_s) == 3);
    AOT_TEST_CHECK(!dumpedCount(vm, "limit"_s, "LookupTableBases"_s));
}

static void testCalledOperationsAreSortedByName(LoadedProgram& program)
{
    VM& vm = program.vm();
    RuntimeTable& table = *vm.aotState().runtimeTable;
    AOT_TEST_CHECK(linesOfKind(dumpedLines(vm), "operation"_s).isEmpty());

    table.didCall(Entry::operationAOTValueAdd);
    table.didCall(Entry::operationAOTNewArray);
    table.didCall(Entry::operationAOTNewArray);
    table.didCall(Entry::operationAOTLookupExceptionHandler);
    table.didCall(Entry::HandleException);

    Vector<CountLine> operations = linesOfKind(dumpedLines(vm), "operation"_s);
    AOT_TEST_CHECK(operations.size() == 3);
    AOT_TEST_CHECK(operations[0].name == "operationAOTLookupExceptionHandler"_s && operations[0].count == 1);
    AOT_TEST_CHECK(operations[1].name == "operationAOTNewArray"_s && operations[1].count == 2);
    AOT_TEST_CHECK(operations[2].name == "operationAOTValueAdd"_s && operations[2].count == 1);
}

static void testBranchesThatRanAreListed(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(linesOfKind(dumpedLines(vm), "branch"_s).isEmpty());

    BranchCounts branchCounts(program);
    AOT_TEST_CHECK(linesOfKind(dumpedLines(vm), "branch"_s).isEmpty());

    branchCounts[0] = 4;
    branchCounts[7] = 1;
    branchCounts[branchCounts.size() - 1] = 2;
    Vector<CountLine> branches = linesOfKind(dumpedLines(vm), "branch"_s);
    AOT_TEST_CHECK(branches.size() == 3);
    AOT_TEST_CHECK(branches[0].name == "0"_s && branches[0].count == 4);
    AOT_TEST_CHECK(branches[1].name == "7"_s && branches[1].count == 1);
    AOT_TEST_CHECK(branches[2].name == makeString(branchCounts.size() - 1) && branches[2].count == 2);
}

static void testValueIsTheSumOverVMAndInstances(LoadedProgram& program)
{
    VM& vm = program.vm();
    Instance& instance = program.instance();
    constexpr RuntimeCounterID counter = RuntimeCounterID::FieldsLeftSlotByDelete;
    constexpr RuntimeCounterID neighbor = RuntimeCounterID::FieldsLeftSlotByRedefinition;
    AOT_TEST_CHECK(!valueOf(vm, counter) && !valueOf(vm, neighbor));

    count(vm, counter);
    AOT_TEST_CHECK(valueOf(vm, counter) == 1);
    count(instance, counter);
    count(instance, counter);
    AOT_TEST_CHECK(valueOf(vm, counter) == 3);
    AOT_TEST_CHECK(vm.aotState().counters->values[static_cast<size_t>(counter)] == 1);
    AOT_TEST_CHECK(instance.runtime.counters.values[static_cast<size_t>(counter)] == 2);
    AOT_TEST_CHECK(!valueOf(vm, neighbor));
}

static void testCountsSurviveTheInstance(LoadedProgram& program)
{
    VM& vm = program.vm();
    VMState& state = vm.aotState();
    Instance* instance = &program.instance();
    constexpr RuntimeCounterID counter = RuntimeCounterID::MemberGuardsCleared;
    constexpr RuntimeCounterID exit = RuntimeCounterID::StubCannotStoreField;

    count(vm, counter);
    for (unsigned i = 0; i < 4; ++i)
        count(*instance, counter);
    count(*instance, exit);
    AOT_TEST_CHECK(valueOf(vm, counter) == 5 && valueOf(vm, exit) == 1);

    {
        Locker locker { state.instancesLock };
        AOT_TEST_CHECK(state.instances.removeFirst(instance));
        state.instancesToDestroy.append(instance);
    }
    AOT_TEST_CHECK(valueOf(vm, counter) == 5 && valueOf(vm, exit) == 1);
    AOT_TEST_CHECK(state.counters->values[static_cast<size_t>(counter)] == 1);

    program.realm()->setAOTInstance(nullptr);
    Instance::destroyDeadInstances(vm);
    AOT_TEST_CHECK(state.instances.isEmpty() && state.instancesToDestroy.isEmpty());
    AOT_TEST_CHECK(valueOf(vm, counter) == 5 && valueOf(vm, exit) == 1);
    AOT_TEST_CHECK(state.counters->values[static_cast<size_t>(counter)] == 5);
    AOT_TEST_CHECK(dumpedCount(vm, "counter"_s, "MemberGuardsCleared"_s) == 5);
    AOT_TEST_CHECK(dumpedCount(vm, "exit"_s, "StubCannotStoreField"_s) == 1);
}

static void testNothingIsCountedWithoutAnImage()
{
    Realms realms;
    VM& vm = realms.vm();
    AOT_TEST_CHECK(!vm.aotState().counters);
    count(vm, RuntimeCounterID::SiteCacheRefills);
    countLimitHit(vm, LimitID::HashedTableEntries);
    AOT_TEST_CHECK(!valueOf(vm, RuntimeCounterID::SiteCacheRefills));
    AOT_TEST_CHECK(!limitHitsOf(vm, LimitID::HashedTableEntries));
    AOT_TEST_CHECK(runtimeLimitValue(vm, LimitID::HashedTableEntries) == defaultLimitValue(LimitID::HashedTableEntries, hostArchitecture()));
}

static void testCounterOfInterpretedCode()
{
    AOT_TEST_CHECK(counterFor(GlobalCode) == RuntimeCounterID::InterpretedGlobalCode);
    AOT_TEST_CHECK(counterFor(EvalCode) == RuntimeCounterID::InterpretedEvalCode);
    AOT_TEST_CHECK(counterFor(FunctionCode) == RuntimeCounterID::InterpretedFunctionCode);
    AOT_TEST_CHECK(counterFor(ModuleCode) == RuntimeCounterID::InterpretedModuleCode);
}

template<LoadedProgramTest body>
static constexpr void (*withProgram)() = runNativeTestWithLoadedProgram<runtimeCounterTests, programText, body>;

std::span<const NativeTest> runtimeCounterTests()
{
    static constexpr NativeTest tests[] = {
        { "runtime-counters/five-kinds-in-order"_s, withProgram<testFiveKindsInOrder> },
        { "runtime-counters/counters-and-exits-are-all-listed"_s, withProgram<testCountersAndExitsAreAllListed> },
        { "runtime-counters/run-time-limits-are-all-listed"_s, withProgram<testRunTimeLimitsAreAllListed> },
        { "runtime-counters/called-operations-are-sorted-by-name"_s, withProgram<testCalledOperationsAreSortedByName> },
        { "runtime-counters/branches-that-ran-are-listed"_s, withProgram<testBranchesThatRanAreListed> },
        { "runtime-counters/value-is-the-sum-over-vm-and-instances"_s, withProgram<testValueIsTheSumOverVMAndInstances> },
        { "runtime-counters/counts-survive-the-instance"_s, withProgram<testCountsSurviveTheInstance> },
        { "runtime-counters/nothing-is-counted-without-an-image"_s, testNothingIsCountedWithoutAnImage },
        { "runtime-counters/counter-of-interpreted-code"_s, testCounterOfInterpretedCode },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
