/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_FLAG_MEMORY_TESTS() do { \
        RUN(testReuseFlagsOfComparesWithMemory()); \
        RUN(testTestsAgainstMaskInMemory()); \
    } while (false)

enum class MemoryOperandForm : uint8_t { RegisterAndAddress, AddressAndRegister, AddressAndImmediate, IndexAndOther };
enum class MemoryComparisonChain : uint8_t { Reused, OtherOffset, OtherBase };

static constexpr int32_t comparedImmediate = 5;

struct ComparedOperands {
    Arg left;
    Arg right;
    bool isMemoryOnLeft;
    bool hasImmediate;
};

static ComparedOperands comparedOperands(const ComparisonOpcodes& opcodes, MemoryOperandForm form, Tmp base, int32_t offset)
{
    Tmp value = argumentTmp(0);
    Tmp index = argumentTmp(3);
    Arg address = Arg::addr(base, offset);
    switch (form) {
    case MemoryOperandForm::RegisterAndAddress:
        return { value, address, false, false };
    case MemoryOperandForm::AddressAndRegister:
        return { address, value, true, false };
    case MemoryOperandForm::AddressAndImmediate:
        return { address, Arg::imm(comparedImmediate), true, true };
    case MemoryOperandForm::IndexAndOther: {
        bool hasImmediate = opcodes.branch == Branch32;
        return { Arg::index(base, index, 1, offset), hasImmediate ? Arg::imm(comparedImmediate) : Arg(value), true, hasImmediate };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkComparisonChainWithMemory(const ComparisonOpcodes& opcodes, MemoryOperandForm form, MemoryComparisonChain chain)
{
    constexpr int32_t secondWord = sizeof(int64_t);
    Arg equalCondition = Arg::relCond(MacroAssembler::Equal);
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    ComparedOperands first = comparedOperands(opcodes, form, argumentTmp(1), 0);
    ComparedOperands second = comparedOperands(opcodes, form, argumentTmp(chain == MemoryComparisonChain::OtherBase ? 2 : 1), chain == MemoryComparisonChain::OtherOffset ? secondWord : 0);

    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* next = code.addBlock();
    BasicBlock* equal = code.addBlock();
    BasicBlock* less = code.addBlock();
    BasicBlock* greater = code.addBlock();
    appendChecked(root, opcodes.branch, equalCondition, first.left, first.right);
    root->setSuccessors(equal, next);
    appendChecked(next, opcodes.branch, lessThan, second.left, second.right);
    next->setSuccessors(less, greater);
    appendReturn(equal, 1);
    appendReturn(less, 2);
    appendReturn(greater, 3);

    bool isReused = chain == MemoryComparisonChain::Reused;
    checkFlagReuse(code, isReused);
    if (isReused) {
        CHECK(root->size() == 2);
        CHECK(isInst(root->at(0), opcodes.compareOnFlags, { first.left, first.right }));
        CHECK(isInst(root->at(1), BranchOnFlags, { equalCondition }));
        CHECK(next->size() == 1);
        CHECK(isInst(next->at(0), BranchOnFlags, { lessThan }));
    }

    auto compilation = compile(proc);
    for (int64_t inFirstWord : { 4, 5, 6 }) {
        for (int64_t inSecondWord : { 4, 6 }) {
            for (int64_t inRegister : { 4, 5, 6 }) {
                int64_t words[] = { inFirstWord, inSecondWord };
                int64_t otherWords[] = { inSecondWord, inSecondWord };
                int64_t other = first.hasImmediate ? comparedImmediate : inRegister;
                int64_t readBySecond = isReused ? inFirstWord : inSecondWord;
                int64_t expected = 1;
                if (inFirstWord != other)
                    expected = (first.isMemoryOnLeft ? readBySecond < other : other < readBySecond) ? 2 : 3;
                CHECK(invokeInt64(*compilation, inRegister, std::bit_cast<int64_t>(&words[0]), std::bit_cast<int64_t>(&otherWords[0]), 0) == expected);
            }
        }
    }
}

static void checkRedefinedBaseEndsReuse()
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* less = code.addBlock();
    BasicBlock* notLess = code.addBlock();
    appendChecked(root, CompareOnFlags64, a0, Arg::addr(a1));
    appendChecked(root, MoveConditionallyOnFlags64, lessThan, a2, a1);
    appendChecked(root, Branch64, lessThan, a0, Arg::addr(a1));
    root->setSuccessors(less, notLess);
    appendReturn(less, 1);
    appendReturn(notLess, 2);

    checkFlagReuse(code, false);
    auto compilation = compile(proc);
    int64_t large = 10;
    int64_t small = 0;
    int64_t addressOfLarge = std::bit_cast<int64_t>(&large);
    int64_t addressOfSmall = std::bit_cast<int64_t>(&small);
    CHECK(invokeInt64(*compilation, 5, addressOfLarge, addressOfSmall) == 2);
    CHECK(invokeInt64(*compilation, 5, addressOfLarge, addressOfLarge) == 1);
    CHECK(invokeInt64(*compilation, 5, addressOfSmall, addressOfLarge) == 2);
}

void testReuseFlagsOfComparesWithMemory()
{
    if (!isX86())
        return;
    for (const ComparisonOpcodes& opcodes : comparisonOpcodes) {
        for (MemoryOperandForm form : { MemoryOperandForm::RegisterAndAddress, MemoryOperandForm::AddressAndRegister, MemoryOperandForm::AddressAndImmediate, MemoryOperandForm::IndexAndOther }) {
            for (MemoryComparisonChain chain : { MemoryComparisonChain::Reused, MemoryComparisonChain::OtherOffset, MemoryComparisonChain::OtherBase })
                checkComparisonChainWithMemory(opcodes, form, chain);
        }
    }
    checkRedefinedBaseEndsReuse();
}

static constexpr int64_t testedValues[] = { 0ll, 1ll, 2ll, 6ll, -1ll, 0x80000000ll, 1ll << 40, std::numeric_limits<int64_t>::min() };
static constexpr int64_t masksInMemory[] = { 6ll, 1ll, -1ll, -11ll, 1ll << 40, std::numeric_limits<int64_t>::min() };

static void checkTestsAgainstMaskInMemoryRun(const TestCondition& test)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg condition = Arg::resCond(test.condition);
    Arg mask = Arg::addr(a1, 8);
    B3::Procedure proc;
    proc.setOptLevel(2);
    BasicBlock* root = proc.code().addBlock();
    appendChecked(root, Test64, condition, mask, a0, ret);
    appendChecked(root, MoveConditionallyTest64, condition, mask, a0, a2, a3, a4);
    appendChecked(root, Add64, a4, ret);
    appendChecked(root, MoveConditionallyTest64, condition, mask, a0, a2, a3);
    appendChecked(root, Add64, a3, ret);
    appendReturn(root);

    auto compilation = compile(proc);
    for (int64_t value : testedValues) {
        for (int64_t maskValue : masksInMemory) {
            int64_t words[2] = { ~maskValue, maskValue };
            bool holds = evaluateMaskedTest(comparisonOpcodes[1], *test.conditionAgainstZero, value, maskValue);
            CHECK(invokeInt64(*compilation, value, std::bit_cast<int64_t>(&words[0]), 10, 200) == holds + 2 * (holds ? 10 : 200));
        }
    }
}

enum class MaskInMemoryNeighbor : uint8_t { SameTest, OtherOffset, OtherValue };

static void checkReuseOfTestAgainstMaskInMemory(MaskInMemoryNeighbor neighbor)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg nonZero = Arg::resCond(MacroAssembler::NonZero);
    Arg mask = Arg::addr(a1, 8);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendChecked(root, MoveConditionallyTest64, nonZero, mask, a0, a2, a3, a4);
    appendChecked(root, MoveConditionallyTest64, Arg::resCond(MacroAssembler::Zero), neighbor == MaskInMemoryNeighbor::OtherOffset ? Arg::addr(a1, 0) : mask, neighbor == MaskInMemoryNeighbor::OtherValue ? a2 : a0, a2, a3, ret);
    appendChecked(root, Add64, a4, a4);
    appendChecked(root, Add64, a4, ret);
    appendReturn(root);

    bool isReused = neighbor == MaskInMemoryNeighbor::SameTest;
    checkFlagReuse(code, isReused);
    if (isReused) {
        CHECK(root->size() == 6);
        CHECK(isInst(root->at(0), TestOnFlags64, { mask, a0 }));
        CHECK(isInst(root->at(1), MoveConditionallyOnFlags64, { Arg::relCond(MacroAssembler::NotEqual), a2, a3, a4 }));
        CHECK(isInst(root->at(2), MoveConditionallyOnFlags64, { Arg::relCond(MacroAssembler::Equal), a2, a3, ret }));
    }
    auto compilation = compile(proc);
    for (int64_t value : testedValues) {
        for (int64_t maskValue : masksInMemory) {
            int64_t words[2] = { ~maskValue, maskValue };
            int64_t secondValue = neighbor == MaskInMemoryNeighbor::OtherValue ? 10 : value;
            int64_t secondMask = neighbor == MaskInMemoryNeighbor::OtherOffset ? ~maskValue : maskValue;
            int64_t first = value & maskValue ? 10 : 200;
            int64_t second = secondValue & secondMask ? 200 : 10;
            CHECK(invokeInt64(*compilation, value, std::bit_cast<int64_t>(&words[0]), 10, 200) == second + 2 * first);
        }
    }
}

void testTestsAgainstMaskInMemory()
{
    if (!isX86())
        return;
    for (const TestCondition& test : testConditions) {
        if (test.conditionAgainstZero)
            checkTestsAgainstMaskInMemoryRun(test);
    }
    for (MaskInMemoryNeighbor neighbor : { MaskInMemoryNeighbor::SameTest, MaskInMemoryNeighbor::OtherOffset, MaskInMemoryNeighbor::OtherValue })
        checkReuseOfTestAgainstMaskInMemory(neighbor);
}
