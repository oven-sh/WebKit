/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBranchCounterNumbers.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIR.h"
#include "AOTThreads.h"
#include <algorithm>
#include <limits>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::BranchCounters, Architecture::ARM64) <= std::numeric_limits<uint32_t>::max());
static_assert(defaultLimitValue(LimitID::BranchCounters, Architecture::X86_64) <= std::numeric_limits<uint32_t>::max());

namespace {

struct CountedTest {
    const Instruction* instruction { nullptr };
    bool isNegated { false };
    ValueID value;
};

} // anonymous namespace

static CountedTest countedTestOf(const Function& function, const Terminator& terminator)
{
    switch (terminator.kind) {
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch: {
        bool isNegated = false;
        ValueID id = terminator.conditionOperand();
        for (; function.at(id).opcode == Opcode::LogicalNot; id = function.at(id).valueOperand())
            isNegated = !isNegated;
        return { &function.at(id), isNegated, id };
    }
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
        return { &terminator, false, { } };
    case TerminatorKind::Jump:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<CountedBranch> findCountedBranches(const Program& program, const Function& function)
{
    Vector<CountedBranch> branches;
    for (unsigned index = 0; index < function.blocks.size(); ++index) {
        CountedTest test = countedTestOf(function, function.blocks[index].terminator);
        if (!test.instruction || !test.instruction->hasPosition())
            continue;
        ModuleID module = program.sourceFunctionOf(function, *test.instruction).module;
        if (!module)
            continue;
        branches.append(CountedBranch { .block = BlockID(index), .module = module, .position = test.instruction->position, .isNegated = test.isNegated, .test = test.value });
    }
    return branches;
}

unsigned counterSideOf(const Terminator& terminator, uint32_t targetIndex)
{
    RELEASE_ASSERT(targetIndex < terminator.targets.size());
    switch (terminator.kind) {
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::TryCallInline:
        return targetIndex;
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
        return targetIndex == terminator.caseTargets().size() ? 1 : 0;
    case TerminatorKind::Jump:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BranchCounterNumbers numberBranchCounters(const Program& program, BuildContext& build)
{
    if (!build.recordedOptions().countsBranches)
        return { };

    unsigned count = program.functions.size();
    Vector<uint32_t> branchCounts(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        branchCounts[index] = static_cast<uint32_t>(findCountedBranches(program, program.functions[index]).size());
    });

    BranchCounterNumbers numbers;
    numbers.m_limit = static_cast<uint32_t>(build.limit(LimitID::BranchCounters));
    numbers.m_first.reserveInitialCapacity(count);
    uint64_t total = 0;
    FunctionID firstFunctionPastLimit;
    for (unsigned index = 0; index < count; ++index) {
        numbers.m_first.append(static_cast<uint32_t>(std::min<uint64_t>(total, numbers.m_limit)));
        total += BranchCounterNumbers::numberOfSides * static_cast<uint64_t>(branchCounts[index]);
        if (!firstFunctionPastLimit && total > numbers.m_limit)
            firstFunctionPastLimit = FunctionID(index);
    }
    numbers.m_firstOfEntries = total;
    if (!firstFunctionPastLimit && total + count > numbers.m_limit)
        firstFunctionPastLimit = FunctionID(static_cast<uint32_t>(numbers.m_limit - total));
    build.exceeds(LimitID::BranchCounters, total + count, { .function = firstFunctionPastLimit });
    return numbers;
}

uint32_t BranchCounterNumbers::firstOf(FunctionID function) const
{
    return function.index() < m_first.size() ? m_first[function.index()] : 0;
}

std::optional<uint32_t> BranchCounterNumbers::counterFor(FunctionID function, uint32_t ordinal, unsigned side) const
{
    RELEASE_ASSERT(side < numberOfSides);
    if (function.index() >= m_first.size())
        return std::nullopt;
    uint64_t counter = static_cast<uint64_t>(m_first[function.index()]) + numberOfSides * static_cast<uint64_t>(ordinal) + side;
    if (counter >= m_limit)
        return std::nullopt;
    return static_cast<uint32_t>(counter);
}

std::optional<uint32_t> BranchCounterNumbers::counterOfEntries(FunctionID function) const
{
    uint64_t counter = m_firstOfEntries + function.index();
    if (function.index() >= m_first.size() || counter >= m_limit)
        return std::nullopt;
    return static_cast<uint32_t>(counter);
}

static void printCounter(PrintStream& out, std::optional<uint32_t> counter)
{
    if (counter)
        out.print(*counter);
    else
        out.print("-");
}

void BranchCounterNumbers::dump(PrintStream& out, const Program& program) const
{
    if (!isCounting())
        return;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID function(index);
        Vector<CountedBranch> branches = findCountedBranches(program, program.functions[index]);
        if (branches.isEmpty())
            continue;
        out.print("function f", index, "\n");
        for (uint32_t ordinal = 0; ordinal < branches.size(); ++ordinal) {
            const CountedBranch& branch = branches[ordinal];
            out.print("b", branch.block.index(), " counters: [");
            printCounter(out, counterFor(function, ordinal, 0));
            out.print(" ");
            printCounter(out, counterFor(function, ordinal, 1));
            out.print("] position: ", branch.position, " negated: ", branch.isNegated ? "yes" : "no", "\n");
        }
    }
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        out.print("f", index, " entries: ");
        printCounter(out, counterOfEntries(FunctionID(index)));
        out.print("\n");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
