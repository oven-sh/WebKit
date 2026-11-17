/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3InsertionSetInlines.h"
#include "B3LowerMacros.h"
#include "B3RelocatableValue.h"
#include "B3RelocationLowering.h"
#include <array>

namespace {

class CompileOnlyRelocationLowering final : public RelocationLowering {
public:
    Value* lower(Procedure& proc, InsertionSet& insertionSet, size_t index, const RelocatableValue& value) final
    {
        uint32_t space = value.symbol().space;
        if (space == testJumpTableSpace)
            return insertionSet.insert<Const64Value>(index, value.origin(), placeholderJumpTableAddress);
        if (space == runtimeFunctionSpace)
            return insertPlaceholderCallee(insertionSet, index, value.origin());
        return m_baseLowering.lower(proc, insertionSet, index, value);
    }

private:
    static constexpr int64_t placeholderJumpTableAddress = 0x1000;

    static Value* insertPlaceholderCallee(InsertionSet& insertionSet, size_t index, Origin origin)
    {
        PatchpointValue* result = insertionSet.insert<PatchpointValue>(index, pointerType(), origin);
        result->effects = Effects::none();
        result->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.move(CCallHelpers::TrustedImm32(0), params[0].gpr());
        });
        return result;
    }

    RandomProgramState m_state;
    RandomProgramRelocationLowering m_baseLowering { m_state };
};

} // anonymous namespace

static constexpr std::array<double, 3> tableConstants { 1.1, 2.2, 3.3 };
static constexpr std::array<double, 1> immediateConstants { 0.0 };
static constexpr int64_t fallThroughResult = -1;

static int64_t resultOfCase(int64_t caseValue) { return caseValue * 3 + 1; }

static Policy positionIndependentPolicy(RelocationLowering* lowering = nullptr)
{
    return policyWith([&](Policy& policy) {
        policy.addressing.isPositionIndependent = true;
        policy.addressing.dataSectionSpace = testDataSectionSpace;
        policy.addressing.jumpTableSpace = testJumpTableSpace;
        policy.addressing.relocationLowering = lowering;
    });
}

static void checkRelocatable(Value* value, RelocationSymbol symbol)
{
    RelocatableValue* relocatable = value->as<RelocatableValue>();
    CHECK(relocatable);
    CHECK(relocatable->symbol() == symbol);
    CHECK(!relocatable->addend());
}

static Value* appendSum(Graph& graph, BasicBlock* block, Value* sum, std::span<const double> constants)
{
    for (double constant : constants)
        sum = graph.make(block, Add, sum, graph.number(block, constant));
    return sum;
}

static void buildSum(Procedure& proc, std::span<const double> constants)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    graph.ret(root, appendSum(graph, root, cCallArgumentValues<double>(proc, root)[0], constants));
}

static void checkSums(Compilation& code, std::span<const double> constants)
{
    for (double argument : { 0.0, -1.5, 10.25, 1e300 }) {
        double expected = argument;
        for (double constant : constants)
            expected += constant;
        CHECK_EQ(invoke<double>(code, argument), expected);
    }
}

template<typename Functor>
static void appendSwitch(Procedure& proc, BasicBlock* block, Value* subject, const Vector<int64_t>& caseValues, const Functor& appendReturn)
{
    SwitchValue* switchValue = block->appendNew<SwitchValue>(proc, numberedOrigin(1), subject);
    BasicBlock* fallThrough = proc.addBlock();
    switchValue->setFallThrough(FrequentedBlock(fallThrough));
    appendReturn(fallThrough, fallThroughResult);
    for (int64_t caseValue : caseValues) {
        BasicBlock* target = proc.addBlock();
        appendReturn(target, resultOfCase(caseValue));
        switchValue->appendCase(SwitchCase(caseValue, FrequentedBlock(target)));
    }
}

static void buildSwitch(Procedure& proc, const Vector<int64_t>& caseValues)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    appendSwitch(proc, root, cCallArgumentValues<int64_t>(proc, root)[0], caseValues, [&](BasicBlock* target, int64_t result) {
        graph.ret(target, graph.int64(target, result));
    });
}

static void lowerSwitch(Procedure& proc, const Vector<int64_t>& caseValues)
{
    buildSwitch(proc, caseValues);
    proc.resetReachability();
    CHECK(lowerMacros(proc));
    validate(proc);
}

static Vector<int64_t> consecutiveCaseValues(unsigned numberOfCases)
{
    Vector<int64_t> result;
    for (unsigned i = 0; i < numberOfCases; ++i)
        result.append(i + 10);
    return result;
}

static void checkSwitchResults(Compilation& code, const Vector<int64_t>& caseValues)
{
    for (int64_t caseValue : caseValues)
        CHECK_EQ(invoke<int64_t>(code, caseValue), resultOfCase(caseValue));
    for (int64_t other : std::initializer_list<int64_t> { std::numeric_limits<int64_t>::min(), -1, 0, caseValues.first() - 1, caseValues.last() + 1, 1ll << 40 }) {
        if (!caseValues.contains(other))
            CHECK_EQ(invoke<int64_t>(code, other), fallThroughResult);
    }
}

static void buildProcedureWithAllRelocations(Procedure& proc)
{
    Graph graph { proc };
    BasicBlock* root = proc.addBlock();
    auto arguments = cCallArgumentValues<int64_t, double, double>(proc, root);
    Value* selector = arguments[0];
    Value* left = arguments[1];
    Value* right = arguments[2];
    Value* sum = appendSum(graph, root, graph.make(root, Mod, left, right), tableConstants);
    appendSwitch(proc, root, selector, consecutiveCaseValues(7), [&](BasicBlock* target, int64_t result) {
        graph.ret(target, graph.make(target, Add, sum, graph.number(target, static_cast<double>(result) + 0.5)));
    });
}

static void testDataSectionsAreListedWhenPositionIndependent()
{
    Procedure independent(positionIndependentPolicy());
    CHECK(independent.dataSections().empty());
    void* first = independent.addDataSection(16);
    void* second = independent.addDataSection(40);
    CHECK(first);
    CHECK(second);
    CHECK(!independent.addDataSection(0));

    std::span<const Procedure::DataSectionRecord> sections = independent.dataSections();
    CHECK_EQ(sections.size(), 2u);
    CHECK(sections[0].data == first);
    CHECK_EQ(sections[0].size, 16u);
    CHECK(sections[1].data == second);
    CHECK_EQ(sections[1].size, 40u);
    CHECK(!independent.report().failure);

    Procedure plain;
    CHECK(plain.addDataSection(16));
    CHECK(!plain.addDataSection(0));
    CHECK(plain.dataSections().empty());
}

static void testDataSectionAddress()
{
    Procedure plain;
    void* section = plain.addDataSection(8);
    Value* constant = plain.addDataSectionAddress(numberedOrigin(1), section);
    CHECK(constant->opcode() == Const64);
    CHECK(constant->isInt64(std::bit_cast<int64_t>(section)));
    CHECK(hasNumber(constant->origin(), 1));
    CHECK(!constant->owner);

    Procedure independent(positionIndependentPolicy());
    void* first = independent.addDataSection(8);
    void* second = independent.addDataSection(8);
    Value* secondAddress = independent.addDataSectionAddress(numberedOrigin(2), second);
    checkRelocatable(secondAddress, { testDataSectionSpace, 1 });
    CHECK(hasNumber(secondAddress->origin(), 2));
    CHECK(!secondAddress->owner);
    checkRelocatable(independent.addDataSectionAddress(Origin(), first), { testDataSectionSpace, 0 });
    CHECK(!independent.report().failure);
}

static void testJumpTableAddress()
{
    Procedure proc(positionIndependentPolicy());
    CHECK_EQ(proc.numberOfJumpTables(), 0u);
    unsigned first = proc.addJumpTable(3);
    Procedure::JumpTableRecord* firstRecord = &proc.jumpTable(first);
    unsigned second = proc.addJumpTable(11);
    CHECK_EQ(first, 0u);
    CHECK_EQ(second, 1u);
    CHECK_EQ(proc.numberOfJumpTables(), 2u);
    CHECK(&proc.jumpTable(first) == firstRecord);
    CHECK_EQ(proc.jumpTable(first).targetOffsets.size(), 3u);
    CHECK_EQ(proc.jumpTable(second).targetOffsets.size(), 11u);

    Value* secondAddress = proc.addJumpTableAddress(numberedOrigin(1), second);
    checkRelocatable(secondAddress, { testJumpTableSpace, second });
    CHECK(hasNumber(secondAddress->origin(), 1));
    CHECK(!secondAddress->owner);
    checkRelocatable(proc.addJumpTableAddress(Origin(), first), { testJumpTableSpace, first });
    CHECK(proc.dataSections().empty());
    CHECK(!proc.report().failure);
}

static void checkDoubleLoadsShareAddress(Procedure& proc)
{
    CHECK_EQ(numberOfValues(proc, Relocatable), 0u);
    CHECK_EQ(numberOfValues(proc, ConstDouble), 0u);

    Value* address = nullptr;
    Vector<int32_t> offsets;
    for (BasicBlock* block : proc) {
        for (Value* value : *block) {
            if (value->opcode() != Load)
                continue;
            MemoryValue* load = value->as<MemoryValue>();
            CHECK(load->type() == Double);
            CHECK(!address || address == load->lastChild());
            address = load->lastChild();
            offsets.append(load->offset());
        }
    }
    std::ranges::sort(offsets);
    CHECK(offsets == (Vector<int32_t> { 0, 8, 16 }));
}

static void testDoubleConstantsComeFromDataSection()
{
    for (double constant : tableConstants)
        CHECK(!Air::Arg::isValidFPImm64Form(std::bit_cast<int64_t>(constant)));
    for (double constant : immediateConstants)
        CHECK(Air::Arg::isValidFPImm64Form(std::bit_cast<int64_t>(constant)));

    {
        RandomProgramState state;
        RandomProgramRelocationLowering lowering(state);
        bool didInspect = false;
        Policy policy = positionIndependentPolicy(&lowering);
        policy.clientPhases.append(ClientPhase { ClientPhase::Point::BeforeLowerToAir, "checkDoubleLoadsShareAddress"_s, createSharedTask<bool(Procedure&)>([&](Procedure& proc) {
            checkDoubleLoadsShareAddress(proc);
            didInspect = true;
            return false;
        }) });
        Procedure proc(policy);
        buildSum(proc, tableConstants);
        std::unique_ptr<Compilation> code = compileAndCheckReport(proc, 2);
        CHECK(didInspect);

        CHECK_EQ(proc.dataSections().size(), 1u);
        Procedure::DataSectionRecord section = proc.dataSections()[0];
        CHECK_EQ(section.size, tableConstants.size() * sizeof(double));
        Vector<double> storedConstants(unsafeMakeSpan(static_cast<const double*>(section.data), tableConstants.size()));
        std::ranges::sort(storedConstants);
        CHECK(std::ranges::equal(storedConstants, tableConstants));
        checkSums(*code, tableConstants);
    }
    {
        Procedure proc(positionIndependentPolicy());
        buildSum(proc, immediateConstants);
        std::unique_ptr<Compilation> code = compileAndCheckReport(proc, 2);
        CHECK(proc.dataSections().empty());
        checkSums(*code, immediateConstants);
    }
    {
        Procedure proc;
        buildSum(proc, tableConstants);
        std::unique_ptr<Compilation> code = compileProc(proc);
        CHECK(proc.dataSections().empty());
        checkSums(*code, tableConstants);
    }
}

static void testJumpTableOffsets()
{
    CompileOnlyRelocationLowering lowering;
    Procedure proc(positionIndependentPolicy(&lowering));
    buildSwitch(proc, { 0, 1, 2, 3, 5, 6, 7, 8, 9 });
    std::unique_ptr<Compilation> code = compileAndCheckReport(proc, 2);

    CHECK(proc.dataSections().empty());
    CHECK_EQ(proc.numberOfJumpTables(), 1u);
    const Vector<uint32_t>& offsets = proc.jumpTable(0).targetOffsets;
    CHECK_EQ(offsets.size(), 11u);
    size_t codeSize = code->codeRef().size();
    for (unsigned i = 0; i < offsets.size(); ++i) {
        CHECK(offsets[i] < codeSize);
        for (unsigned j = i + 1; j < offsets.size(); ++j)
            CHECK_EQ(offsets[i] == offsets[j], i == 4 && j == 10);
    }
}

static void testJumpTableIsOneTerminal()
{
    constexpr unsigned numberOfCases = 7;
    Procedure proc(positionIndependentPolicy());
    lowerSwitch(proc, consecutiveCaseValues(numberOfCases));
    CHECK(!proc.report().failure);
    CHECK_EQ(proc.numberOfJumpTables(), 1u);
    CHECK_EQ(proc.jumpTable(0).targetOffsets.size(), numberOfCases + 1);

    BasicBlock* root = proc.at(0);
    CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
    PatchpointValue* terminal = root->last()->as<PatchpointValue>();
    CHECK(terminal);
    CHECK(terminal->type() == Void);
    CHECK(terminal->effects.terminal);
    CHECK(terminal->kind().isCloningForbidden());
    CHECK(terminal->runsWithoutFrame());
    CHECK_EQ(root->numSuccessors(), numberOfCases + 1);
    CHECK_EQ(terminal->numChildren(), 1u);

    Value* target = terminal->child(0);
    CHECK(target->opcode() == Add);
    CHECK_EQ(numberOfValues(proc, Relocatable), 1u);
    Value* table = target->child(0);
    checkRelocatable(table, { testJumpTableSpace, 0 });
    CHECK(hasNumber(table->origin(), 1));
    CHECK(target->child(1)->opcode() == SExt32);

    CHECK_EQ(numberOfValues(proc, Load), 1u);
    MemoryValue* entry = target->child(1)->child(0)->as<MemoryValue>();
    CHECK(entry);
    CHECK(entry->opcode() == Load);
    CHECK(entry->type() == Int32);
    CHECK(entry->readsMutability() == Mutability::Immutable);
    CHECK(!entry->controlDependent());
}

static void testSmallSwitchHasNoTable()
{
    struct Case {
        unsigned numberOfCases;
        unsigned minCasesForJumpTable;
        bool hasTable;
    };
    constexpr unsigned never = std::numeric_limits<unsigned>::max();
    CHECK_EQ(Policy().switchLowering.minCasesForJumpTable, 7u);
    for (Case testCase : { Case { 6, 7, false }, Case { 7, 7, true }, Case { 5, 6, false }, Case { 6, 6, true }, Case { 300, never, false } }) {
        Policy policy = positionIndependentPolicy();
        policy.switchLowering.minCasesForJumpTable = testCase.minCasesForJumpTable;
        Vector<int64_t> caseValues = consecutiveCaseValues(testCase.numberOfCases);
        unsigned numberOfTables = testCase.hasTable;

        Procedure lowered(policy);
        lowerSwitch(lowered, caseValues);
        CHECK_EQ(lowered.numberOfJumpTables(), numberOfTables);
        CHECK_EQ(numberOfValues(lowered, Patchpoint), numberOfTables);
        CHECK_EQ(numberOfValues(lowered, Relocatable), numberOfTables);
        if (testCase.hasTable)
            continue;

        Procedure proc(policy);
        buildSwitch(proc, caseValues);
        std::unique_ptr<Compilation> code = compileAndCheckReport(proc, 2);
        checkSwitchResults(*code, caseValues);
    }
}

static void testJITKeepsTableOfCodePointers()
{
    Vector<int64_t> caseValues = consecutiveCaseValues(7);
    {
        Procedure proc;
        lowerSwitch(proc, caseValues);
        CHECK_EQ(proc.numberOfJumpTables(), 0u);
        CHECK(proc.dataSections().empty());
        CHECK_EQ(numberOfValues(proc, Relocatable), 0u);
        CHECK_EQ(numberOfValues(proc, Patchpoint), 1u);
        Value* terminal = proc.at(0)->last();
        CHECK(terminal->opcode() == Patchpoint);
        CHECK_EQ(terminal->numChildren(), 1u);
        CHECK(terminal->child(0)->opcode() == Load);
        CHECK(terminal->child(0)->type() == pointerType());
    }
    {
        Procedure proc;
        lowerSwitch(proc, consecutiveCaseValues(6));
        CHECK_EQ(numberOfValues(proc, Patchpoint), 0u);
    }
    {
        Procedure proc;
        buildSwitch(proc, caseValues);
        std::unique_ptr<Compilation> code = compileProc(proc);
        CHECK_EQ(proc.numberOfJumpTables(), 0u);
        checkSwitchResults(*code, caseValues);
    }
}

static void testNoAbsoluteAddressWhenPositionIndependent()
{
    {
        CompileOnlyRelocationLowering lowering;
        Procedure proc(positionIndependentPolicy(&lowering));
        buildProcedureWithAllRelocations(proc);
        std::unique_ptr<Compilation> code = compileAndCheckReport(proc, 2);
        CHECK(code);
        CHECK(!proc.report().failure);
        CHECK_EQ(proc.dataSections().size(), 1u);
        CHECK_EQ(proc.numberOfJumpTables(), 1u);
    }
    {
        Procedure proc(positionIndependentPolicy());
        buildProcedureWithAllRelocations(proc);
        prepareForGeneration(proc);
        const std::optional<Failure>& failure = proc.report().failure;
        CHECK(failure);
        CHECK(failure->kind == FailureKind::RelocationNotLowered);
    }
}

void addBunReadOnlyDataTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testDataSectionsAreListedWhenPositionIndependent());
    RUN(testDataSectionAddress());
    RUN(testJumpTableAddress());
    RUN(testDoubleConstantsComeFromDataSection());
    RUN(testJumpTableOffsets());
    RUN(testJumpTableIsOneTerminal());
    RUN(testSmallSwitchHasNoTable());
    RUN(testJITKeepsTableOfCodePointers());
    RUN(testNoAbsoluteAddressWhenPositionIndependent());
}

#endif // ENABLE(B3_JIT)
