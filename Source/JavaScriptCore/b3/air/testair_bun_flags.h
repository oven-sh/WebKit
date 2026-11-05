/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_FLAG_TESTS() do { \
        RUN(testFlagAttributes()); \
        RUN(testFlagsAreDefinedAtUses()); \
        RUN(testFlagOpcodesRun()); \
        RUN(testReuseFlagsAcrossBlocks()); \
        RUN(testReuseFlagsOfBranches()); \
        RUN(testReuseFlagsForConditionalMoves()); \
        RUN(testTestAgainstItselfIsCompareWithZero()); \
        RUN(testReuseFlagsOfTestsAgainstMasks()); \
        RUN(testCompareOnFlagsWithZeroIsTest()); \
        RUN(testReuseFlagsIsIdempotent()); \
        RUN(testBranchOnFlagsOfResult()); \
    } while (false)

struct ComparisonOpcodes {
    B3::Air::Opcode compareOnFlags;
    B3::Air::Opcode testOnFlags;
    B3::Air::Opcode branch;
    B3::Air::Opcode branchTest;
    B3::Air::Opcode moveConditionally;
    B3::Air::Opcode moveConditionallyTest;
};

static constexpr ComparisonOpcodes comparisonOpcodes[] = {
    { CompareOnFlags32, TestOnFlags32, Branch32, BranchTest32, MoveConditionally32, MoveConditionallyTest32 },
    { CompareOnFlags64, TestOnFlags64, Branch64, BranchTest64, MoveConditionally64, MoveConditionallyTest64 },
};

static constexpr MacroAssembler::RelationalCondition relationalConditions[] = {
    MacroAssembler::Equal, MacroAssembler::NotEqual, MacroAssembler::Above, MacroAssembler::AboveOrEqual, MacroAssembler::Below,
    MacroAssembler::BelowOrEqual, MacroAssembler::GreaterThan, MacroAssembler::GreaterThanOrEqual, MacroAssembler::LessThan, MacroAssembler::LessThanOrEqual,
};

struct TestCondition {
    MacroAssembler::ResultCondition condition;
    std::optional<MacroAssembler::RelationalCondition> conditionAgainstZero;
};

static constexpr TestCondition testConditions[] = {
    { MacroAssembler::Zero, MacroAssembler::Equal },
    { MacroAssembler::NonZero, MacroAssembler::NotEqual },
    { MacroAssembler::Signed, MacroAssembler::LessThan },
    { MacroAssembler::PositiveOrZero, MacroAssembler::GreaterThanOrEqual },
    { MacroAssembler::Overflow, std::nullopt },
    { MacroAssembler::Carry, std::nullopt },
};

static void checkFlagReuse(Code& code, bool isExpected)
{
    if (!isExpected) {
        checkPassLeavesCodeUnchanged(code, reuseConditionFlags);
        return;
    }
    checkPassChangesCode(code, reuseConditionFlags);
    CHECK(!findUseOfUndefinedFlags(code));
}

void testFlagAttributes()
{
    CHECK(definesFlags(CompareOnFlags32));
    CHECK(definesFlags(CompareOnFlags64));
    CHECK(definesFlags(TestOnFlags32));
    CHECK(definesFlags(TestOnFlags64));
    CHECK(usesFlags(BranchOnFlags));
    CHECK(usesFlags(MoveConditionallyOnFlags64));
    CHECK(usesFlags(CompareConditionallyOnFlags64) && definesFlags(CompareConditionallyOnFlags64));

    CHECK(preservesFlags(Jump));
    CHECK(preservesFlags(Padding));
    CHECK(preservesFlags(Nop));
    CHECK(preservesFlags(BranchOnFlags));
    CHECK(preservesFlags(MoveConditionallyOnFlags64));
    CHECK(preservesFlags(Move) == isARM64());

    CHECK(!preservesFlags(Add32));
    CHECK(!preservesFlags(Branch32));
    CHECK(!preservesFlags(Patch));
    CHECK(!preservesFlags(CompareOnFlags32));
    CHECK(!preservesFlags(TestOnFlags32));
    CHECK(!usesFlags(TestOnFlags64));
    CHECK(!definesFlags(Move));
    CHECK(!usesFlags(Move));
    CHECK(!definesFlags(Branch32));
}

enum class FlagsGraph : uint8_t { SameBlock, Predecessor, ThroughJump, BothPredecessorsDefine, ClobberedByAdd, ClobberedByPatch, OnePredecessorDefines, NoDefinition, ClobberedOnBackEdge };

static void appendCompareOnFlags(BasicBlock* block)
{
    appendChecked(block, CompareOnFlags32, argumentTmp(0), argumentTmp(1));
}

static void appendBranchOnFlags(BasicBlock* block, BasicBlock* taken, BasicBlock* notTaken)
{
    appendChecked(block, BranchOnFlags, Arg::relCond(MacroAssembler::Equal));
    block->setSuccessors(taken, notTaken);
}

static void appendBranchOnFlagsToReturns(Code& code, BasicBlock* block)
{
    BasicBlock* taken = code.addBlock();
    BasicBlock* notTaken = code.addBlock();
    appendBranchOnFlags(block, taken, notTaken);
    appendReturn(taken, 1);
    appendReturn(notTaken, 2);
}

static BasicBlock* buildFlagsDiamond(Code& code, BasicBlock* root, bool rightDefinesFlags)
{
    BasicBlock* left = code.addBlock();
    BasicBlock* right = code.addBlock();
    BasicBlock* join = code.addBlock();
    appendBranchTest(root, argumentTmp(2), left, right);
    appendCompareOnFlags(left);
    appendJump(left, join);
    if (rightDefinesFlags)
        appendCompareOnFlags(right);
    appendJump(right, join);
    appendBranchOnFlagsToReturns(code, join);
    return join;
}

static BasicBlock* buildFlagsGraph(B3::Procedure& proc, FlagsGraph graph)
{
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* user = root;

    switch (graph) {
    case FlagsGraph::SameBlock:
        appendCompareOnFlags(root);
        break;
    case FlagsGraph::Predecessor:
        user = code.addBlock();
        appendCompareOnFlags(root);
        appendJump(root, user);
        break;
    case FlagsGraph::ThroughJump: {
        BasicBlock* middle = code.addBlock();
        user = code.addBlock();
        appendCompareOnFlags(root);
        appendJump(root, middle);
        appendJump(middle, user);
        break;
    }
    case FlagsGraph::BothPredecessorsDefine:
        return buildFlagsDiamond(code, root, true);
    case FlagsGraph::ClobberedByAdd:
        appendCompareOnFlags(root);
        appendChecked(root, Add32, Arg::imm(1), argumentTmp(2));
        break;
    case FlagsGraph::ClobberedByPatch:
        appendCompareOnFlags(root);
        appendCheckedPatch(root, addEmptyPatchpoint(proc), patchpointSpecialOf(code));
        break;
    case FlagsGraph::OnePredecessorDefines:
        return buildFlagsDiamond(code, root, false);
    case FlagsGraph::NoDefinition:
        break;
    case FlagsGraph::ClobberedOnBackEdge: {
        BasicBlock* header = code.addBlock();
        BasicBlock* body = code.addBlock();
        BasicBlock* exit = code.addBlock();
        appendCompareOnFlags(root);
        appendJump(root, header);
        appendBranchOnFlags(header, body, exit);
        appendChecked(body, Add32, Arg::imm(1), argumentTmp(2));
        appendJump(body, header);
        appendReturn(exit, 2);
        return header;
    }
    }
    appendBranchOnFlagsToReturns(code, user);
    return user;
}

void testFlagsAreDefinedAtUses()
{
    auto check = [](FlagsGraph graph, bool areFlagsDefined) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        BasicBlock* user = buildFlagsGraph(proc, graph);
        proc.code().resetReachability();
        CHECK(findUseOfUndefinedFlags(proc.code()) == (areFlagsDefined ? nullptr : &user->last()));
    };

    for (FlagsGraph graph : { FlagsGraph::SameBlock, FlagsGraph::Predecessor, FlagsGraph::ThroughJump, FlagsGraph::BothPredecessorsDefine })
        check(graph, true);
    for (FlagsGraph graph : { FlagsGraph::ClobberedByAdd, FlagsGraph::ClobberedByPatch, FlagsGraph::OnePredecessorDefines, FlagsGraph::NoDefinition, FlagsGraph::ClobberedOnBackEdge })
        check(graph, false);
}

template<typename Integer>
static bool evaluateCondition(MacroAssembler::RelationalCondition condition, Integer left, Integer right)
{
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned unsignedLeft = static_cast<Unsigned>(left);
    Unsigned unsignedRight = static_cast<Unsigned>(right);
    switch (condition) {
    case MacroAssembler::Equal:
        return left == right;
    case MacroAssembler::NotEqual:
        return left != right;
    case MacroAssembler::Above:
        return unsignedLeft > unsignedRight;
    case MacroAssembler::AboveOrEqual:
        return unsignedLeft >= unsignedRight;
    case MacroAssembler::Below:
        return unsignedLeft < unsignedRight;
    case MacroAssembler::BelowOrEqual:
        return unsignedLeft <= unsignedRight;
    case MacroAssembler::GreaterThan:
        return left > right;
    case MacroAssembler::GreaterThanOrEqual:
        return left >= right;
    case MacroAssembler::LessThan:
        return left < right;
    case MacroAssembler::LessThanOrEqual:
        return left <= right;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Integer>
static void checkBranchOnFlags(B3::Air::Opcode definerOpcode, MacroAssembler::RelationalCondition condition, std::optional<int32_t> immediate = std::nullopt)
{
    constexpr int64_t operands[] = { 0, 1, -1, 3, 0x80, 0x80000000ll, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() };
    bool isTest = definerOpcode == TestOnFlags32 || definerOpcode == TestOnFlags64;
    Arg rightOperand = argumentTmp(1);
    if (immediate)
        rightOperand = isTest ? Arg::bitImm(*immediate) : Arg::imm(*immediate);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* one = code.addBlock();
    BasicBlock* zero = code.addBlock();
    appendChecked(root, definerOpcode, argumentTmp(0), rightOperand);
    appendChecked(root, BranchOnFlags, Arg::relCond(condition));
    root->setSuccessors(one, zero);
    appendReturn(one, 1);
    appendReturn(zero, 0);
    auto compilation = compile(proc);
    for (int64_t left : operands) {
        for (int64_t operand : operands) {
            int64_t right = immediate ? *immediate : operand;
            bool expected = isTest ? evaluateCondition<Integer>(condition, static_cast<Integer>(left & right), 0) : evaluateCondition(condition, static_cast<Integer>(left), static_cast<Integer>(right));
            CHECK(invokeInt64(*compilation, left, right) == static_cast<int64_t>(expected));
        }
    }
}

static void checkConditionalMovesOnFlags(Arg elseCase, int64_t elseValue)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    B3::Procedure proc;
    proc.setOptLevel(2);
    BasicBlock* root = proc.code().addBlock();
    appendChecked(root, CompareOnFlags64, a0, a1);
    appendChecked(root, MoveConditionallyOnFlags64, Arg::relCond(MacroAssembler::LessThan), a2, elseCase, ret);
    appendChecked(root, MoveConditionallyOnFlags64, Arg::relCond(MacroAssembler::Equal), a1, ret);
    appendReturn(root);
    auto compilation = compile(proc);
    CHECK(invokeInt64(*compilation, 1, 2, 10, 20) == 10);
    CHECK(invokeInt64(*compilation, 2, 1, 10, 20) == elseValue);
    CHECK(invokeInt64(*compilation, 7, 7, 10, 20) == 7);
}

void testFlagOpcodesRun()
{
    checkConditionalMovesOnFlags(argumentTmp(3), 20);
    if (isARM64())
        checkConditionalMovesOnFlags(Arg::zeroReg(), 0);

    for (MacroAssembler::RelationalCondition condition : relationalConditions) {
        checkBranchOnFlags<int64_t>(CompareOnFlags64, condition);
        checkBranchOnFlags<int32_t>(CompareOnFlags32, condition);
        for (int32_t immediate : { 3, 0 }) {
            checkBranchOnFlags<int64_t>(CompareOnFlags64, condition, immediate);
            checkBranchOnFlags<int32_t>(CompareOnFlags32, condition, immediate);
        }
    }

    for (const TestCondition& test : testConditions) {
        if (!test.conditionAgainstZero)
            continue;
        checkBranchOnFlags<int64_t>(TestOnFlags64, *test.conditionAgainstZero);
        checkBranchOnFlags<int32_t>(TestOnFlags32, *test.conditionAgainstZero);
        for (int32_t mask : { 6, 0x80, -2 }) {
            checkBranchOnFlags<int32_t>(TestOnFlags32, *test.conditionAgainstZero, mask);
            if (isX86())
                checkBranchOnFlags<int64_t>(TestOnFlags64, *test.conditionAgainstZero, mask);
        }
    }
}

enum class ComparisonChain : uint8_t { Reused, BothSuccessors, MoveBetween, AddBetween, OperandRedefinedBetween, SecondPredecessor, LoopHeader, DifferentWidth, CommutedOperands, ComparedWithZero };

static std::pair<BasicBlock*, BasicBlock*> buildComparisonChain(Code& code, ComparisonChain chain)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    BasicBlock* entry = chain == ComparisonChain::SecondPredecessor ? code.addBlock() : nullptr;
    BasicBlock* root = code.addBlock();
    BasicBlock* next = code.addBlock();
    BasicBlock* equal = code.addBlock();
    BasicBlock* less = code.addBlock();
    BasicBlock* greater = code.addBlock();

    if (entry)
        appendBranchTest(entry, a3, root, next);

    Arg immediate = Arg::imm(chain == ComparisonChain::ComparedWithZero ? 0 : 5);
    appendChecked(root, Branch32, Arg::relCond(MacroAssembler::Equal), a0, chain == ComparisonChain::CommutedOperands ? Arg(a1) : immediate);
    root->setSuccessors(equal, next);

    if (chain == ComparisonChain::MoveBetween)
        appendChecked(next, Move, a2, a3);
    if (chain == ComparisonChain::AddBetween)
        appendChecked(next, Add32, Arg::imm(1), a2);
    if (chain == ComparisonChain::OperandRedefinedBetween)
        appendChecked(next, Move, a1, a0);
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    if (chain == ComparisonChain::CommutedOperands)
        appendChecked(next, Branch32, lessThan, a1, a0);
    else
        appendChecked(next, chain == ComparisonChain::DifferentWidth ? Branch64 : Branch32, lessThan, a0, immediate);
    next->setSuccessors(less, greater);

    if (chain == ComparisonChain::BothSuccessors) {
        BasicBlock* equalAndGreater = code.addBlock();
        BasicBlock* equalAndNotGreater = code.addBlock();
        appendChecked(equal, Branch32, Arg::relCond(MacroAssembler::GreaterThan), a0, immediate);
        equal->setSuccessors(equalAndGreater, equalAndNotGreater);
        appendReturn(equalAndGreater, 4);
        appendReturn(equalAndNotGreater, 1);
    } else
        appendReturn(equal, 1);
    appendReturn(less, 2);
    if (chain == ComparisonChain::LoopHeader) {
        appendChecked(greater, Add32, Arg::imm(-1), a0);
        appendJump(greater, next);
    } else
        appendReturn(greater, 3);
    return { root, next };
}

static void checkComparisonChainResults(const Compilation& compilation, ComparisonChain chain)
{
    switch (chain) {
    case ComparisonChain::Reused:
    case ComparisonChain::BothSuccessors:
    case ComparisonChain::MoveBetween:
    case ComparisonChain::AddBetween:
    case ComparisonChain::SecondPredecessor:
    case ComparisonChain::DifferentWidth:
        CHECK(invokeInt64(compilation, 5, 0, 0, 1) == 1);
        CHECK(invokeInt64(compilation, 4, 0, 0, 1) == 2);
        CHECK(invokeInt64(compilation, 6, 0, 0, 1) == 3);
        CHECK(invokeInt64(compilation, 4, 0, 0, 0) == 2);
        CHECK(invokeInt64(compilation, 6, 0, 0, 0) == 3);
        break;
    case ComparisonChain::LoopHeader:
        CHECK(invokeInt64(compilation, 5) == 1);
        CHECK(invokeInt64(compilation, 4) == 2);
        CHECK(invokeInt64(compilation, 6) == 2);
        break;
    case ComparisonChain::OperandRedefinedBetween:
        CHECK(invokeInt64(compilation, 5, 0) == 1);
        CHECK(invokeInt64(compilation, 4, 3) == 2);
        CHECK(invokeInt64(compilation, 4, 5) == 3);
        break;
    case ComparisonChain::CommutedOperands:
        CHECK(invokeInt64(compilation, 5, 5) == 1);
        CHECK(invokeInt64(compilation, 5, 4) == 2);
        CHECK(invokeInt64(compilation, 4, 5) == 3);
        break;
    case ComparisonChain::ComparedWithZero:
        CHECK(invokeInt64(compilation, 0) == 1);
        CHECK(invokeInt64(compilation, -1) == 2);
        CHECK(invokeInt64(compilation, 1) == 3);
        break;
    }
}

static void checkComparisonChain(ComparisonChain chain)
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    auto [root, next] = buildComparisonChain(code, chain);

    switch (chain) {
    case ComparisonChain::Reused:
        checkFlagReuse(code, true);
        CHECK(root->size() == 2);
        CHECK(isInst(root->at(0), CompareOnFlags32, { argumentTmp(0), Arg::imm(5) }));
        CHECK(isInst(root->at(1), BranchOnFlags, { Arg::relCond(MacroAssembler::Equal) }));
        CHECK(next->size() == 1);
        CHECK(isInst(next->at(0), BranchOnFlags, { Arg::relCond(MacroAssembler::LessThan) }));
        break;
    case ComparisonChain::BothSuccessors:
        checkFlagReuse(code, true);
        CHECK(numberOfInsts(code, CompareOnFlags32) == 1);
        CHECK(!numberOfInsts(code, Branch32));
        break;
    case ComparisonChain::MoveBetween:
        checkFlagReuse(code, preservesFlags(Move));
        break;
    case ComparisonChain::ComparedWithZero:
        checkFlagReuse(code, !isARM64());
        break;
    case ComparisonChain::AddBetween:
    case ComparisonChain::OperandRedefinedBetween:
    case ComparisonChain::SecondPredecessor:
    case ComparisonChain::LoopHeader:
    case ComparisonChain::DifferentWidth:
    case ComparisonChain::CommutedOperands:
        checkFlagReuse(code, false);
        break;
    }
    auto compilation = compile(proc);
    checkComparisonChainResults(*compilation, chain);
}

void testReuseFlagsAcrossBlocks()
{
    for (ComparisonChain chain : { ComparisonChain::Reused, ComparisonChain::BothSuccessors, ComparisonChain::MoveBetween, ComparisonChain::AddBetween, ComparisonChain::OperandRedefinedBetween,
        ComparisonChain::SecondPredecessor, ComparisonChain::LoopHeader, ComparisonChain::DifferentWidth, ComparisonChain::CommutedOperands, ComparisonChain::ComparedWithZero })
        checkComparisonChain(chain);
}

static bool isFlaglessBranchOnARM64(MacroAssembler::RelationalCondition condition, int32_t immediate)
{
    switch (condition) {
    case MacroAssembler::Equal:
    case MacroAssembler::NotEqual:
    case MacroAssembler::Above:
    case MacroAssembler::BelowOrEqual:
    case MacroAssembler::LessThan:
    case MacroAssembler::GreaterThanOrEqual:
        return !immediate;
    case MacroAssembler::LessThanOrEqual:
    case MacroAssembler::GreaterThan:
        return immediate == -1;
    case MacroAssembler::AboveOrEqual:
    case MacroAssembler::Below:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkBranchThenConditionalMoves(const ComparisonOpcodes& opcodes, Inst branch, std::optional<MacroAssembler::RelationalCondition> condition, int32_t immediate, bool isReuseExpected)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* taken = code.addBlock();
    BasicBlock* notTaken = code.addBlock();
    CHECK(branch.isValidForm(code.targetFeatures()));
    root->appendInst(WTF::move(branch));
    root->setSuccessors(taken, notTaken);
    appendChecked(taken, opcodes.moveConditionally, lessThan, a0, Arg::imm(immediate), a1, a2, ret);
    appendReturn(taken);
    appendChecked(notTaken, opcodes.moveConditionally, lessThan, a0, Arg::imm(immediate), a3, a4, ret);
    appendReturn(notTaken);

    checkFlagReuse(code, isReuseExpected);
    if (isReuseExpected) {
        CHECK(root->size() == 2);
        CHECK(isInst(root->at(0), opcodes.compareOnFlags, { a0, Arg::imm(immediate) }));
        CHECK(isInst(root->at(1), BranchOnFlags, { Arg::relCond(*condition) }));
        CHECK(isInst(taken->at(0), MoveConditionallyOnFlags64, { lessThan, a1, a2, ret }));
        CHECK(isInst(notTaken->at(0), MoveConditionallyOnFlags64, { lessThan, a3, a4, ret }));
    }
    auto compilation = compile(proc);
    for (int64_t value : { -2, -1, 0, 1, 5, 6 }) {
        bool isTaken = condition && evaluateCondition<int64_t>(*condition, value, immediate);
        bool isLess = value < immediate;
        int64_t expected = isTaken ? (isLess ? 10 : 20) : (isLess ? 30 : 40);
        CHECK(invokeInt64(*compilation, value, 10, 20, 30, 40) == expected);
    }
}

void testReuseFlagsOfBranches()
{
    Tmp a0 = argumentTmp(0);
    for (const ComparisonOpcodes& opcodes : comparisonOpcodes) {
        for (MacroAssembler::RelationalCondition condition : relationalConditions) {
            for (int32_t immediate : { 0, -1, 5 }) {
                Inst branch(opcodes.branch, nullptr, Arg::relCond(condition), a0, Arg::imm(immediate));
                checkBranchThenConditionalMoves(opcodes, branch, condition, immediate, !isARM64() || !isFlaglessBranchOnARM64(condition, immediate));
            }
        }
        for (const TestCondition& test : testConditions) {
            Inst branch(opcodes.branchTest, nullptr, Arg::resCond(test.condition), a0, a0);
            bool isFlaglessOnARM64 = test.condition == MacroAssembler::Zero || test.condition == MacroAssembler::NonZero;
            checkBranchThenConditionalMoves(opcodes, branch, test.conditionAgainstZero, 0, test.conditionAgainstZero && (!isARM64() || !isFlaglessOnARM64));
        }
    }
}

static void checkInPlaceConditionalMoves()
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    Arg equal = Arg::relCond(MacroAssembler::Equal);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendChecked(root, MoveConditionally32, lessThan, a0, a1, a2, a3);
    appendChecked(root, MoveConditionally32, equal, a0, a1, a2, a4);
    appendChecked(root, Add64, a3, a4);
    appendChecked(root, Move, a4, ret);
    appendReturn(root);

    checkFlagReuse(code, true);
    CHECK(root->size() == 6);
    CHECK(isInst(root->at(0), CompareOnFlags32, { a0, a1 }));
    CHECK(isInst(root->at(1), MoveConditionallyOnFlags64, { lessThan, a2, a3 }));
    CHECK(isInst(root->at(2), MoveConditionallyOnFlags64, { equal, a2, a4 }));
    auto compilation = compile(proc);
    CHECK(invokeInt64(*compilation, 1, 2, 10, 20, 300) == 10 + 300);
    CHECK(invokeInt64(*compilation, 2, 1, 10, 20, 300) == 20 + 300);
    CHECK(invokeInt64(*compilation, 2, 2, 10, 20, 300) == 20 + 10);
}

void testReuseFlagsForConditionalMoves()
{
    enum class Case : uint8_t { Shared, DestinationIsCompared };
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    Arg equal = Arg::relCond(MacroAssembler::Equal);
    for (Case testCase : { Case::Shared, Case::DestinationIsCompared }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();

        bool isShared = testCase == Case::Shared;
        Tmp firstDestination = isShared ? a4 : a0;
        appendChecked(root, MoveConditionally32, lessThan, a0, a1, a2, a3, firstDestination);
        appendChecked(root, MoveConditionally32, equal, a0, a1, a2, firstDestination, ret);
        appendReturn(root);

        checkFlagReuse(code, isShared);
        if (isShared) {
            CHECK(root->size() == 4);
            CHECK(isInst(root->at(0), CompareOnFlags32, { a0, a1 }));
            CHECK(isInst(root->at(1), MoveConditionallyOnFlags64, { lessThan, a2, a3, a4 }));
            CHECK(isInst(root->at(2), MoveConditionallyOnFlags64, { equal, a2, a4, ret }));
        }
        auto compilation = compile(proc);
        auto checkResult = [&](int64_t left, int64_t right) {
            int64_t first = left < right ? 10 : 20;
            int64_t secondLeft = isShared ? left : first;
            CHECK(invokeInt64(*compilation, left, right, 10, 20) == (secondLeft == right ? 10 : first));
        };
        checkResult(1, 2);
        checkResult(2, 1);
        checkResult(2, 2);
    }

    checkInPlaceConditionalMoves();
}

static void checkTestAgainstItself(const ComparisonOpcodes& opcodes, const TestCondition& test)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg lessThan = Arg::relCond(MacroAssembler::LessThan);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendChecked(root, opcodes.moveConditionallyTest, Arg::resCond(test.condition), a0, a0, a1, a2, a3);
    appendChecked(root, opcodes.moveConditionally, lessThan, a0, Arg::imm(0), a1, a2, a4);
    appendChecked(root, Add64, a4, a4);
    appendChecked(root, Add64, a3, a4);
    appendChecked(root, Move, a4, ret);
    appendReturn(root);

    if (!test.conditionAgainstZero) {
        checkFlagReuse(code, false);
        return;
    }
    checkFlagReuse(code, true);
    CHECK(root->size() == 7);
    CHECK(isInst(root->at(0), opcodes.compareOnFlags, { a0, Arg::imm(0) }));
    CHECK(isInst(root->at(1), MoveConditionallyOnFlags64, { Arg::relCond(*test.conditionAgainstZero), a1, a2, a3 }));
    CHECK(isInst(root->at(2), MoveConditionallyOnFlags64, { lessThan, a1, a2, a4 }));
    auto compilation = compile(proc);
    for (int64_t value : { -1, 0, 1 }) {
        int64_t first = evaluateCondition<int64_t>(*test.conditionAgainstZero, value, 0) ? 10 : 20;
        int64_t second = value < 0 ? 10 : 20;
        CHECK(invokeInt64(*compilation, value, 10, 20) == first + 2 * second);
    }
}

void testTestAgainstItselfIsCompareWithZero()
{
    for (const ComparisonOpcodes& opcodes : comparisonOpcodes) {
        for (const TestCondition& test : testConditions)
            checkTestAgainstItself(opcodes, test);
    }
}

static bool evaluateMaskedTest(const ComparisonOpcodes& opcodes, MacroAssembler::RelationalCondition condition, int64_t value, int64_t mask)
{
    if (opcodes.testOnFlags == TestOnFlags32)
        return evaluateCondition<int32_t>(condition, static_cast<int32_t>(value & mask), 0);
    return evaluateCondition<int64_t>(condition, value & mask, 0);
}

static bool hasImmediateMasks(const ComparisonOpcodes& opcodes)
{
    return opcodes.testOnFlags == TestOnFlags32 || isX86();
}

static Arg immediateMaskOfConditionalMove(const ComparisonOpcodes& opcodes, int32_t mask)
{
    return opcodes.testOnFlags == TestOnFlags32 ? Arg::bitImm(mask) : Arg::imm(mask);
}

static void checkConditionalMovesOnMaskedTest(const ComparisonOpcodes& opcodes, const TestCondition& test, std::optional<int32_t> immediate)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg mask = immediate ? immediateMaskOfConditionalMove(opcodes, *immediate) : Arg(a1);
    Arg maskOfDefiner = immediate ? Arg::bitImm(*immediate) : Arg(a1);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendChecked(root, opcodes.moveConditionallyTest, Arg::resCond(test.condition), a0, mask, a2, a3, a4);
    appendChecked(root, opcodes.moveConditionallyTest, Arg::resCond(MacroAssembler::NonZero), a0, mask, a3, a2, ret);
    appendChecked(root, Add64, a4, a4);
    appendChecked(root, Add64, a4, ret);
    appendReturn(root);

    if (!test.conditionAgainstZero) {
        checkFlagReuse(code, false);
        return;
    }
    checkFlagReuse(code, true);
    CHECK(root->size() == 6);
    CHECK(isInst(root->at(0), opcodes.testOnFlags, { a0, maskOfDefiner }));
    CHECK(isInst(root->at(1), MoveConditionallyOnFlags64, { Arg::relCond(*test.conditionAgainstZero), a2, a3, a4 }));
    CHECK(isInst(root->at(2), MoveConditionallyOnFlags64, { Arg::relCond(MacroAssembler::NotEqual), a3, a2, ret }));
    auto compilation = compile(proc);
    for (int64_t value : { 0ll, 1ll, 2ll, 6ll, -1ll, 0x80000000ll, 1ll << 40 }) {
        for (int64_t operand : { 6ll, 1ll, -1ll, 1ll << 40 }) {
            int64_t maskValue = immediate ? *immediate : operand;
            int64_t first = evaluateMaskedTest(opcodes, *test.conditionAgainstZero, value, maskValue) ? 10 : 20;
            int64_t second = evaluateMaskedTest(opcodes, MacroAssembler::NotEqual, value, maskValue) ? 20 : 10;
            CHECK(invokeInt64(*compilation, value, maskValue, 10, 20) == second + 2 * first);
        }
    }
}

static void checkInPlaceConditionalMoveOnMaskedTest()
{
    if (!isX86())
        return;
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg nonZero = Arg::resCond(MacroAssembler::NonZero);
    Arg notEqual = Arg::relCond(MacroAssembler::NotEqual);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendChecked(root, MoveConditionallyTest32, nonZero, a0, Arg::bitImm(6), a1, a2, ret);
    appendChecked(root, MoveConditionallyTest32, nonZero, a0, Arg::imm(6), a3, a4);
    appendChecked(root, Add64, a4, ret);
    appendReturn(root);

    checkFlagReuse(code, true);
    CHECK(root->size() == 5);
    CHECK(isInst(root->at(0), TestOnFlags32, { a0, Arg::bitImm(6) }));
    CHECK(isInst(root->at(1), MoveConditionallyOnFlags64, { notEqual, a1, a2, ret }));
    CHECK(isInst(root->at(2), MoveConditionallyOnFlags64, { notEqual, a3, a4 }));
    auto compilation = compile(proc);
    CHECK(invokeInt64(*compilation, 2, 10, 20, 300, 400) == 10 + 300);
    CHECK(invokeInt64(*compilation, 1, 10, 20, 300, 400) == 20 + 400);
}

enum class MaskedTestNeighbor : uint8_t { OtherMask, MaskRedefinedBetween, OtherWidth, ComparedWithZero };

static void checkMaskedTestIsNotReused(MaskedTestNeighbor neighbor)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg nonZero = Arg::resCond(MacroAssembler::NonZero);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendChecked(root, MoveConditionallyTest64, nonZero, a0, a1, a2, a3, neighbor == MaskedTestNeighbor::MaskRedefinedBetween ? a1 : a4);
    switch (neighbor) {
    case MaskedTestNeighbor::OtherMask:
        appendChecked(root, MoveConditionallyTest64, nonZero, a0, a2, a3, a2, ret);
        break;
    case MaskedTestNeighbor::MaskRedefinedBetween:
        appendChecked(root, MoveConditionallyTest64, nonZero, a0, a1, a3, a2, ret);
        break;
    case MaskedTestNeighbor::OtherWidth:
        appendChecked(root, MoveConditionallyTest32, nonZero, a0, a1, a3, a2, ret);
        break;
    case MaskedTestNeighbor::ComparedWithZero:
        appendChecked(root, MoveConditionally64, Arg::relCond(MacroAssembler::NotEqual), a0, Arg::imm(0), a3, a2, ret);
        break;
    }
    appendReturn(root);

    checkFlagReuse(code, false);
    auto compilation = compile(proc);
    for (int64_t value : { 0ll, 1ll, 2ll, 4ll, 1ll << 40 }) {
        for (int64_t mask : { 1ll, 6ll, 1ll << 40 }) {
            int64_t first = (value & mask) ? 10 : 20;
            bool second = false;
            switch (neighbor) {
            case MaskedTestNeighbor::OtherMask:
                second = value & 10;
                break;
            case MaskedTestNeighbor::MaskRedefinedBetween:
                second = value & first;
                break;
            case MaskedTestNeighbor::OtherWidth:
                second = static_cast<int32_t>(value & mask);
                break;
            case MaskedTestNeighbor::ComparedWithZero:
                second = value;
                break;
            }
            CHECK(invokeInt64(*compilation, value, mask, 10, 20) == (second ? 20 : 10));
        }
    }
}

static void checkBranchTestThenConditionalMoves(const ComparisonOpcodes& opcodes, const TestCondition& test, int32_t mask)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg nonZero = Arg::resCond(MacroAssembler::NonZero);
    Arg notEqual = Arg::relCond(MacroAssembler::NotEqual);
    Arg maskOfMove = immediateMaskOfConditionalMove(opcodes, mask);
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* taken = code.addBlock();
    BasicBlock* notTaken = code.addBlock();
    appendChecked(root, opcodes.branchTest, Arg::resCond(test.condition), a0, Arg::bitImm(mask));
    root->setSuccessors(taken, notTaken);
    appendChecked(taken, opcodes.moveConditionallyTest, nonZero, a0, maskOfMove, a1, a2, ret);
    appendReturn(taken);
    appendChecked(notTaken, opcodes.moveConditionallyTest, nonZero, a0, maskOfMove, a3, a4, ret);
    appendReturn(notTaken);

    if (!test.conditionAgainstZero) {
        checkFlagReuse(code, false);
        return;
    }
    bool testsForZero = test.condition == MacroAssembler::Zero || test.condition == MacroAssembler::NonZero;
    bool isFlaglessOnARM64 = testsForZero && hasOneBitSet(mask);
    bool isReuseExpected = !isARM64() || !isFlaglessOnARM64;
    checkFlagReuse(code, isReuseExpected);
    if (isReuseExpected) {
        CHECK(root->size() == 2);
        CHECK(isInst(root->at(0), opcodes.testOnFlags, { a0, Arg::bitImm(mask) }));
        CHECK(isInst(root->at(1), BranchOnFlags, { Arg::relCond(*test.conditionAgainstZero) }));
        CHECK(isInst(taken->at(0), MoveConditionallyOnFlags64, { notEqual, a1, a2, ret }));
        CHECK(isInst(notTaken->at(0), MoveConditionallyOnFlags64, { notEqual, a3, a4, ret }));
    }
    auto compilation = compile(proc);
    for (int64_t value : { -1, 0, 1, 2, 4, 8 }) {
        bool isTaken = evaluateMaskedTest(opcodes, *test.conditionAgainstZero, value, mask);
        bool isNonZero = value & mask;
        int64_t expected = isTaken ? (isNonZero ? 10 : 20) : (isNonZero ? 30 : 40);
        CHECK(invokeInt64(*compilation, value, 10, 20, 30, 40) == expected);
    }
}

void testReuseFlagsOfTestsAgainstMasks()
{
    for (const ComparisonOpcodes& opcodes : comparisonOpcodes) {
        for (const TestCondition& test : testConditions) {
            checkConditionalMovesOnMaskedTest(opcodes, test, std::nullopt);
            if (!hasImmediateMasks(opcodes))
                continue;
            checkConditionalMovesOnMaskedTest(opcodes, test, 6);
            for (int32_t mask : { 6, 4 })
                checkBranchTestThenConditionalMoves(opcodes, test, mask);
        }
    }
    checkInPlaceConditionalMoveOnMaskedTest();

    for (MaskedTestNeighbor neighbor : { MaskedTestNeighbor::OtherMask, MaskedTestNeighbor::MaskRedefinedBetween, MaskedTestNeighbor::OtherWidth, MaskedTestNeighbor::ComparedWithZero })
        checkMaskedTestIsNotReused(neighbor);
}

void testCompareOnFlagsWithZeroIsTest()
{
    if (!isX86())
        return;
    for (const ComparisonOpcodes& opcodes : comparisonOpcodes) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        appendChecked(root, opcodes.compareOnFlags, argumentTmp(0), Arg::imm(0));
        appendChecked(root, opcodes.testOnFlags, argumentTmp(0), argumentTmp(0));
        appendChecked(root, opcodes.compareOnFlags, argumentTmp(0), Arg::imm(1));
        appendReturn(root, 0);

        prepareForGeneration(code);
        ObservedCode observed = generateObservedCode(proc);
        Vector<ptrdiff_t, 3> sizes;
        for (const InstRange& range : observed.ranges) {
            if (definesFlags(range.opcode))
                sizes.append(range.end - range.begin);
        }
        CHECK(sizes.size() == 3);
        CHECK(sizes[0] == sizes[1]);
        CHECK(sizes[0] < sizes[2]);
    }
}

void testReuseFlagsIsIdempotent()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    buildComparisonChain(code, ComparisonChain::Reused);

    checkFlagReuse(code, true);
    checkFlagReuse(code, false);
    auto compilation = compile(proc);
    checkComparisonChainResults(*compilation, ComparisonChain::Reused);
}

enum class ResultOperation : uint8_t { ShiftLeft, ShiftLeftByNothing, AndWithMask, AndWithNegativeMask, AndWithMaskOfByte, AndWithMaskOfHalfWord, AndWithAllBits, AndWithRegister, AndIntoOtherRegister, AndOfRegisterWithItself };
enum class ResultTestNeighbor : uint8_t { None, MoveBetween, OtherRegisterTested, OtherWidthTested };
enum class ResultTestForm : uint8_t { TestAgainstItself, TestAgainstAllBits, CompareWithZero };

static int64_t maskOf(ResultOperation operation)
{
    switch (operation) {
    case ResultOperation::AndWithMask:
        return 0x18100000;
    case ResultOperation::AndWithNegativeMask:
        return -16;
    case ResultOperation::AndWithMaskOfByte:
        return 0xff;
    case ResultOperation::AndWithMaskOfHalfWord:
        return 0xffff;
    case ResultOperation::AndWithAllBits:
        return -1;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static constexpr int32_t shiftAmountOfResultTests = 3;

// Says whether the operation leaves the flags of its result.
static bool appendTestOfResult(Code& code, bool is64Bit, ResultOperation operation, const TestCondition& test, ResultTestForm form, ResultTestNeighbor neighbor)
{
    constexpr int32_t shiftAmount = shiftAmountOfResultTests;
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    B3::Air::Opcode shift = is64Bit ? Lshift64 : Lshift32;
    B3::Air::Opcode bitAnd = is64Bit ? And64 : And32;
    bool tests64Bits = is64Bit != (neighbor == ResultTestNeighbor::OtherWidthTested);
    BasicBlock* root = code.addBlock();
    BasicBlock* taken = code.addBlock();
    BasicBlock* notTaken = code.addBlock();

    Tmp result = a0;
    bool leavesFlags = true;
    switch (operation) {
    case ResultOperation::ShiftLeft:
        appendChecked(root, shift, Arg::imm(shiftAmount), a0);
        break;
    case ResultOperation::ShiftLeftByNothing:
        appendChecked(root, shift, Arg::imm(is64Bit ? 64 : 32), a0);
        leavesFlags = false;
        break;
    case ResultOperation::AndWithMask:
    case ResultOperation::AndWithNegativeMask:
        appendChecked(root, bitAnd, Arg::imm(maskOf(operation)), a0);
        break;
    case ResultOperation::AndWithMaskOfByte:
    case ResultOperation::AndWithMaskOfHalfWord:
    case ResultOperation::AndWithAllBits:
        appendChecked(root, bitAnd, Arg::imm(maskOf(operation)), a0);
        leavesFlags = false;
        break;
    case ResultOperation::AndWithRegister:
        appendChecked(root, bitAnd, a1, a0);
        break;
    case ResultOperation::AndIntoOtherRegister:
        appendChecked(root, bitAnd, a0, a1, a2);
        result = a2;
        break;
    case ResultOperation::AndOfRegisterWithItself:
        appendChecked(root, bitAnd, a0, a0, a2);
        result = a2;
        leavesFlags = false;
        break;
    }
    if (neighbor == ResultTestNeighbor::MoveBetween)
        appendChecked(root, Move, a3, a4);
    Tmp tested = neighbor == ResultTestNeighbor::OtherRegisterTested ? a3 : result;
    switch (form) {
    case ResultTestForm::TestAgainstItself:
        appendChecked(root, tests64Bits ? BranchTest64 : BranchTest32, Arg::resCond(test.condition), tested, tested);
        break;
    case ResultTestForm::TestAgainstAllBits:
        appendChecked(root, tests64Bits ? BranchTest64 : BranchTest32, Arg::resCond(test.condition), tested, Arg::bitImm(-1));
        break;
    case ResultTestForm::CompareWithZero:
        appendChecked(root, tests64Bits ? Branch64 : Branch32, Arg::relCond(*test.conditionAgainstZero), tested, Arg::imm(0));
        break;
    }
    root->setSuccessors(taken, notTaken);
    appendReturn(taken, 1);
    appendReturn(notTaken, 2);
    return leavesFlags;
}

static void checkBranchOnFlagsOfResult(bool is64Bit, ResultOperation operation, const TestCondition& test, ResultTestForm form = ResultTestForm::TestAgainstItself, ResultTestNeighbor neighbor = ResultTestNeighbor::None)
{
    constexpr int32_t shiftAmount = shiftAmountOfResultTests;
    if (!test.conditionAgainstZero && form == ResultTestForm::CompareWithZero)
        return;
    {
        B3::Procedure proc;
        proc.setOptLevel(2);
        bool leavesFlags = appendTestOfResult(proc.code(), is64Bit, operation, test, form, neighbor);
        bool isExpected = leavesFlags && test.conditionAgainstZero && neighbor == ResultTestNeighbor::None;
        checkFlagReuse(proc.code(), isExpected);
        if (isExpected)
            CHECK(isInst(proc.code()[0]->last(), BranchOnFlags, { Arg::resCond(test.condition) }));
    }
    if (!test.conditionAgainstZero || neighbor != ResultTestNeighbor::None)
        return;

    // The step is the last of the pipeline: nothing behind it may come between the operation and the branch.
    B3::Policy policy;
    policy.air.peepholes.reusesConditionFlags = true;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    appendTestOfResult(proc.code(), is64Bit, operation, test, form, neighbor);
    auto compilation = compile(proc);
    constexpr int64_t values[] = { 0, 1, -1, 15, 16, 0xff, 0x100, 0x10000, 0x100000, 0x10000000, 0x7fffffff, 0x80000000ll, 0xffffffffll, 0x100000000ll, 0x1000000000000000ll, INT64_MIN, INT64_MAX };
    for (int64_t value : values) {
        for (int64_t other : { int64_t { -1 }, int64_t { 0 }, int64_t { 0x80000010ll }, INT64_MIN }) {
            uint64_t bits = static_cast<uint64_t>(value);
            switch (operation) {
            case ResultOperation::ShiftLeft:
                bits <<= shiftAmount;
                break;
            case ResultOperation::ShiftLeftByNothing:
            case ResultOperation::AndOfRegisterWithItself:
                break;
            case ResultOperation::AndWithRegister:
            case ResultOperation::AndIntoOtherRegister:
                bits &= static_cast<uint64_t>(other);
                break;
            default:
                bits &= static_cast<uint64_t>(maskOf(operation));
                break;
            }
            int64_t signedResult = is64Bit ? static_cast<int64_t>(bits) : static_cast<int32_t>(static_cast<uint32_t>(bits));
            bool isTaken = evaluateCondition<int64_t>(*test.conditionAgainstZero, signedResult, 0);
            CHECK(invokeInt64(*compilation, value, other, 0, 0, 0) == (isTaken ? 1 : 2));
        }
    }
}

void testBranchOnFlagsOfResult()
{
    constexpr ResultOperation operations[] = {
        ResultOperation::ShiftLeft, ResultOperation::ShiftLeftByNothing, ResultOperation::AndWithMask, ResultOperation::AndWithNegativeMask, ResultOperation::AndWithMaskOfByte,
        ResultOperation::AndWithMaskOfHalfWord, ResultOperation::AndWithAllBits, ResultOperation::AndWithRegister, ResultOperation::AndIntoOtherRegister, ResultOperation::AndOfRegisterWithItself,
    };
    if (!isX86())
        return;
    for (bool is64Bit : { false, true }) {
        for (const TestCondition& test : testConditions) {
            for (ResultOperation operation : operations) {
                for (ResultTestForm form : { ResultTestForm::TestAgainstItself, ResultTestForm::TestAgainstAllBits, ResultTestForm::CompareWithZero })
                    checkBranchOnFlagsOfResult(is64Bit, operation, test, form);
            }
        }
        for (ResultTestNeighbor neighbor : { ResultTestNeighbor::MoveBetween, ResultTestNeighbor::OtherRegisterTested, ResultTestNeighbor::OtherWidthTested })
            checkBranchOnFlagsOfResult(is64Bit, ResultOperation::ShiftLeft, testConditions[0], ResultTestForm::TestAgainstItself, neighbor);
    }
}
