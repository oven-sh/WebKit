/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_LAYOUT_TESTS() do { \
        RUN(testBlockOrderInvertsBranches()); \
        RUN(testBlockOrderPlacesRareBlocksInGaps()); \
        RUN(testBlockOrderComparesEdges()); \
        RUN(testBlockOrderKeepsUnlikelySideOutOfLine()); \
        RUN(testRareBlockAfterGuard()); \
        RUN(testDefaultLayoutIsUpstream()); \
    } while (false)

static void checkBlockOrder(Code& code, std::initializer_list<BasicBlock*> expectedOrder)
{
    CHECK(code.size() == expectedOrder.size());
    unsigned index = 0;
    for (BasicBlock* block : expectedOrder)
        CHECK(code[index++] == block);
}

template<typename Functor>
static void checkBranchFallsThroughToLikelySuccessor(const Functor& appendBranch, int32_t (*runWith)(Compilation&, bool shouldBeTaken))
{
    for (bool takenIsLikely : { true, false }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* taken = code.addBlock(takenIsLikely ? 1 : 0.01);
        BasicBlock* notTaken = code.addBlock(takenIsLikely ? 0.01 : 1);
        appendBranch(root);
        root->setSuccessors(FrequentedBlock(taken, takenIsLikely ? B3::FrequencyClass::Normal : B3::FrequencyClass::Rare), FrequentedBlock(notTaken, takenIsLikely ? B3::FrequencyClass::Rare : B3::FrequencyClass::Normal));
        taken->append(Move, nullptr, Arg::imm(1), Tmp(GPRInfo::returnValueGPR));
        taken->append(Ret32, nullptr, Tmp(GPRInfo::returnValueGPR));
        notTaken->append(Move, nullptr, Arg::imm(2), Tmp(GPRInfo::returnValueGPR));
        notTaken->append(Ret32, nullptr, Tmp(GPRInfo::returnValueGPR));

        auto compilation = compile(proc);
        CHECK(code.findNextBlock(root) == (takenIsLikely ? taken : notTaken));
        CHECK(root->successorBlock(1) == code.findNextBlock(root));
        CHECK(runWith(*compilation, true) == 1);
        CHECK(runWith(*compilation, false) == 2);
    }
}

void testBlockOrderInvertsBranches()
{
#if CPU(X86_64)
    for (B3::Air::Opcode opcode : { BranchTestBit32, BranchTestBit64 }) {
        checkBranchFallsThroughToLikelySuccessor([&](BasicBlock* root) {
            root->append(opcode, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR0), Arg::imm(3));
        }, [](Compilation& compilation, bool shouldBeTaken) {
            return invoke<int32_t>(compilation, shouldBeTaken ? 0x18 : 0x17);
        });
    }
#elif CPU(ARM64)
    checkBranchFallsThroughToLikelySuccessor([&](BasicBlock* root) {
        root->append(BranchDoubleWithZero, nullptr, Arg::doubleCond(MacroAssembler::DoubleEqualAndOrdered), Tmp(FPRInfo::argumentFPR0));
    }, [](Compilation& compilation, bool shouldBeTaken) {
        return invoke<int32_t>(compilation, shouldBeTaken ? 0.0 : 1.5);
    });
    checkBranchFallsThroughToLikelySuccessor([&](BasicBlock* root) {
        root->append(BranchFloatWithZero, nullptr, Arg::doubleCond(MacroAssembler::DoubleGreaterThanAndOrdered), Tmp(FPRInfo::argumentFPR0));
    }, [](Compilation& compilation, bool shouldBeTaken) {
        return invoke<int32_t>(compilation, shouldBeTaken ? 1.5f : -1.5f);
    });
#endif
}

struct BlocksAroundGap {
    explicit BlocksAroundGap(Code& code)
        : root(code.addBlock())
        , guard(code.addBlock())
        , afterGuard(code.addBlock())
        , otherSuccessor(code.addBlock(0.5))
        , rare(code.addBlock(0.01))
    {
        root->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR0), Tmp(GPRInfo::argumentGPR0));
        root->setSuccessors(guard, otherSuccessor);
        guard->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR1), Tmp(GPRInfo::argumentGPR1));
        guard->setSuccessors(rarely(rare), afterGuard);
        appendReturn(otherSuccessor, 2);
    }

    static void appendReturn(BasicBlock* block, int32_t value)
    {
        block->append(Move, nullptr, Arg::imm(value), Tmp(GPRInfo::returnValueGPR));
        block->append(Ret32, nullptr, Tmp(GPRInfo::returnValueGPR));
    }

    static void appendAdditions(BasicBlock* block, unsigned number)
    {
        for (unsigned i = 0; i < number; ++i)
            block->append(Add32, nullptr, Arg::imm(1), Tmp(GPRInfo::argumentGPR2));
    }

    bool rareBlockPrecedesOtherSuccessor(B3::Procedure& proc)
    {
        auto compilation = compile(proc);
        CHECK(invoke<int32_t>(*compilation, 1, 0, 0) == 1);
        CHECK(invoke<int32_t>(*compilation, 0, 0, 0) == 2);
        CHECK(invoke<int32_t>(*compilation, 1, 1, 0) == 3);
        return rare->index() < otherSuccessor->index();
    }

    BasicBlock* root;
    BasicBlock* guard;
    BasicBlock* afterGuard;
    BasicBlock* otherSuccessor;
    BasicBlock* rare;
};

enum class GapBeforeGuard : uint8_t { IsNotRequested, Fits, IsTooFar, HasLikelyPredecessor };

static void checkGapBeforeGuard(GapBeforeGuard testCase)
{
    constexpr unsigned additionsPastDistanceLimit = 30;
    constexpr unsigned additionsPastSizeLimit = 20;
    static_assert((additionsPastDistanceLimit - 1) * B3::AirLimits::estimatedBytesPerInst > B3::AirLimits::maxBytesFromGuardToRareBlock);
    static_assert((additionsPastSizeLimit - 1) * B3::AirLimits::estimatedBytesPerInst > B3::AirLimits::maxBytesOfRareBlockNearGuard);
    B3::Policy policy;
    policy.air.layout.placesRareBlocksInGaps = testCase != GapBeforeGuard::IsNotRequested;
    policy.air.layout.recordsRareCodeStart = true;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* chainEnd = code.addBlock();
    BasicBlock* guard = code.addBlock(0.5);
    BasicBlock* afterGuard = code.addBlock(0.5);
    BasicBlock* rare = code.addBlock(0.01);
    bool hasLikelyPredecessor = testCase == GapBeforeGuard::HasLikelyPredecessor;
    BasicBlock* last = hasLikelyPredecessor ? code.addBlock(0.5) : afterGuard;
    BasicBlock* largeRare = hasLikelyPredecessor ? code.addBlock(0.01) : nullptr;

    appendBranchTest(root, argumentTmp(0), chainEnd, guard);
    appendReturn(chainEnd, 1);
    if (testCase == GapBeforeGuard::IsTooFar)
        BlocksAroundGap::appendAdditions(guard, additionsPastDistanceLimit);
    appendBranchTest(guard, argumentTmp(1), rarely(rare), afterGuard);
    BlocksAroundGap::appendAdditions(afterGuard, additionsPastDistanceLimit);
    if (hasLikelyPredecessor) {
        appendBranchTest(afterGuard, argumentTmp(3), rarely(largeRare), last);
        BlocksAroundGap::appendAdditions(largeRare, additionsPastSizeLimit);
        appendJump(largeRare, rare);
    }
    appendReturn(last, 2);
    appendReturn(rare, 3);

    auto compilation = compile(proc);
    bool fillsGap = testCase == GapBeforeGuard::Fits && isX86();
    if (hasLikelyPredecessor)
        checkBlockOrder(code, { root, chainEnd, guard, afterGuard, last, largeRare, rare });
    else if (fillsGap)
        checkBlockOrder(code, { root, chainEnd, rare, guard, afterGuard });
    else
        checkBlockOrder(code, { root, chainEnd, guard, afterGuard, rare });
    CHECK(code.numberOfBlocksInChains() == (hasLikelyPredecessor || fillsGap ? 5u : 4u));

    auto run = [&](int64_t first, int64_t second, int64_t fourth) {
        return invoke<int64_t>(*compilation, first, second, static_cast<int64_t>(0), fourth);
    };
    CHECK(run(1, 0, 0) == 1);
    CHECK(run(0, 0, 0) == 2);
    CHECK(run(0, 1, 0) == 3);
    if (hasLikelyPredecessor)
        CHECK(run(0, 0, 1) == 3);
}

void testBlockOrderPlacesRareBlocksInGaps()
{
    enum class Case : uint8_t { IsNotRequested, Fits, IsTooLarge, IsTooFar, HasLikelyPredecessor };
    for (Case testCase : { Case::IsNotRequested, Case::Fits, Case::IsTooLarge, Case::IsTooFar, Case::HasLikelyPredecessor }) {
        B3::Policy policy;
        if (testCase != Case::IsNotRequested)
            policy.air.layout.placesRareBlocksInGaps = true;
        policy.air.layout.recordsRareCodeStart = true;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        BlocksAroundGap blocks(code);
        if (testCase == Case::IsTooFar)
            BlocksAroundGap::appendAdditions(blocks.afterGuard, 30);
        BlocksAroundGap::appendReturn(blocks.afterGuard, 1);
        if (testCase == Case::IsTooLarge)
            BlocksAroundGap::appendAdditions(blocks.rare, 20);
        BlocksAroundGap::appendReturn(blocks.rare, 3);
        if (testCase == Case::HasLikelyPredecessor) {
            blocks.otherSuccessor->insts().clear();
            blocks.otherSuccessor->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR2), Tmp(GPRInfo::argumentGPR2));
            BasicBlock* last = code.addBlock();
            blocks.otherSuccessor->setSuccessors(blocks.rare, last);
            BlocksAroundGap::appendReturn(last, 2);
        }

        CHECK(blocks.rareBlockPrecedesOtherSuccessor(proc) == (testCase == Case::Fits));
        if (testCase != Case::HasLikelyPredecessor)
            CHECK(code.numberOfBlocksInChains() == (testCase == Case::Fits ? 5u : 4u));
    }

    for (GapBeforeGuard testCase : { GapBeforeGuard::IsNotRequested, GapBeforeGuard::Fits, GapBeforeGuard::IsTooFar, GapBeforeGuard::HasLikelyPredecessor })
        checkGapBeforeGuard(testCase);
}

void testBlockOrderComparesEdges()
{
    enum class Case : uint8_t { IsNotRequested, EdgesAreLevel, BodyIsUnlikely };
    for (Case testCase : { Case::IsNotRequested, Case::EdgesAreLevel, Case::BodyIsUnlikely }) {
        B3::Policy policy;
        if (testCase != Case::IsNotRequested)
            policy.air.layout.ordersBlocksByEdgeFrequency = true;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        double bodyFrequency = testCase == Case::BodyIsUnlikely ? 0.2 : 0.5;
        BasicBlock* root = code.addBlock();
        BasicBlock* body = code.addBlock(bodyFrequency);
        BasicBlock* firstTarget = code.addBlock(bodyFrequency / 2);
        BasicBlock* secondTarget = code.addBlock(bodyFrequency / 2);
        BasicBlock* join = code.addBlock();
        root->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR0), Tmp(GPRInfo::argumentGPR0));
        root->setSuccessors(body, join);
        body->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR1), Tmp(GPRInfo::argumentGPR1));
        body->setSuccessors(firstTarget, secondTarget);
        firstTarget->append(Add32, nullptr, Arg::imm(1), Tmp(GPRInfo::argumentGPR2));
        firstTarget->append(Jump, nullptr);
        firstTarget->setSuccessors(join);
        secondTarget->append(Add32, nullptr, Arg::imm(2), Tmp(GPRInfo::argumentGPR2));
        secondTarget->append(Jump, nullptr);
        secondTarget->setSuccessors(join);
        join->append(Move, nullptr, Tmp(GPRInfo::argumentGPR2), Tmp(GPRInfo::returnValueGPR));
        join->append(Ret32, nullptr, Tmp(GPRInfo::returnValueGPR));

        auto compilation = compile(proc);
        CHECK(invoke<int32_t>(*compilation, 0, 0, 5) == 5);
        CHECK(invoke<int32_t>(*compilation, 1, 1, 5) == 6);
        CHECK(invoke<int32_t>(*compilation, 1, 0, 5) == 7);
        CHECK((body->index() < join->index()) == (testCase == Case::EdgesAreLevel));
    }
}

void testBlockOrderKeepsUnlikelySideOutOfLine()
{
    enum class Shape : uint8_t { Triangle, Diamond };
    for (Shape shape : { Shape::Triangle, Shape::Diamond }) {
        for (bool hasFrequencies : { false, true }) {
            for (auto [sideFrequency, rootFrequency] : { std::pair { 0.5, 1.0 }, std::pair { 0.2, 1.0 }, std::pair { 0.5, 1.4 }, std::pair { 0.2, 1.4 } }) {
                B3::Policy policy;
                if (hasFrequencies)
                    policy.air.layout.ordersBlocksByEdgeFrequency = true;
                B3::Procedure proc(policy);
                proc.setOptLevel(2);
                Code& code = proc.code();
                BasicBlock* root = code.addBlock(rootFrequency);
                BasicBlock* side = code.addBlock(sideFrequency);
                BasicBlock* otherSide = shape == Shape::Diamond ? code.addBlock(1 - sideFrequency) : nullptr;
                BasicBlock* join = code.addBlock();
                root->append(BranchTest32, nullptr, Arg::resCond(MacroAssembler::NonZero), Tmp(GPRInfo::argumentGPR0), Tmp(GPRInfo::argumentGPR0));
                root->setSuccessors(side, otherSide ? otherSide : join);
                side->append(Add32, nullptr, Arg::imm(1), Tmp(GPRInfo::argumentGPR1));
                side->append(Jump, nullptr);
                side->setSuccessors(join);
                if (otherSide) {
                    otherSide->append(Add32, nullptr, Arg::imm(2), Tmp(GPRInfo::argumentGPR1));
                    otherSide->append(Jump, nullptr);
                    otherSide->setSuccessors(join);
                }
                join->append(Move, nullptr, Tmp(GPRInfo::argumentGPR1), Tmp(GPRInfo::returnValueGPR));
                join->append(Ret32, nullptr, Tmp(GPRInfo::returnValueGPR));

                auto compilation = compile(proc);
                CHECK(invoke<int32_t>(*compilation, 1, 5) == 6);
                CHECK(invoke<int32_t>(*compilation, 0, 5) == (otherSide ? 7 : 5));
                CHECK((side->index() > join->index()) == (hasFrequencies && sideFrequency < 0.5));
            }
        }
    }
}

struct GuardInLoop {
    enum class Entries : uint8_t { AtGuard, AtGuardAndLatch };

    explicit GuardInLoop(Code& code, Entries entries = Entries::AtGuard)
        : entry(code.addBlock())
        , guard(code.addBlock(10))
        , latch(code.addBlock(10))
        , exit(code.addBlock())
        , rare(code.addBlock(0.01))
    {
        if (entries == Entries::AtGuardAndLatch)
            appendBranchTest(entry, argumentTmp(2), latch, guard);
        else
            appendJump(entry, guard);
        appendBranchTest(guard, argumentTmp(0), rarely(rare), latch);
        latch->append(Sub64, nullptr, Arg::imm(1), argumentTmp(1));
        appendBranchTest(latch, argumentTmp(1), guard, exit);
        appendReturn(exit, 1);
        appendReturn(rare, 3);
    }

    static void checkResults(const Compilation& compilation)
    {
        for (int64_t entersAtLatch : { 0, 1 }) {
            CHECK((invoke<int64_t, int64_t, int64_t, int64_t>(compilation, 0, 3, entersAtLatch)) == 1);
            CHECK((invoke<int64_t, int64_t, int64_t, int64_t>(compilation, 1, 3, entersAtLatch)) == 3);
        }
    }

    BasicBlock* entry;
    BasicBlock* guard;
    BasicBlock* latch;
    BasicBlock* exit;
    BasicBlock* rare;
};

void testRareBlockAfterGuard()
{
    enum class Case : uint8_t { Placed, AtSizeLimit, FallsThroughToRareBlock, InLoop, InIrreducibleCycle, TooLarge, PatchTerminal, ConditionIsNotInvertible, WithoutGaps, TwoEntrypoints, HasLikelyPredecessor };
    constexpr unsigned maxInstsNearGuard = B3::AirLimits::maxBytesOfRareBlockNearGuard / B3::AirLimits::estimatedBytesPerInst;
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
    for (Case testCase : { Case::Placed, Case::AtSizeLimit, Case::FallsThroughToRareBlock, Case::InLoop, Case::InIrreducibleCycle, Case::TooLarge, Case::PatchTerminal, Case::ConditionIsNotInvertible,
        Case::WithoutGaps, Case::TwoEntrypoints, Case::HasLikelyPredecessor }) {
        B3::Policy policy;
        policy.air.layout.placesRareBlocksInGaps = testCase != Case::WithoutGaps;
        policy.air.layout.placesRareBlocksAfterGuards = true;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        if (testCase == Case::TwoEntrypoints)
            proc.setNumEntrypoints(2);

        if (testCase == Case::InLoop || testCase == Case::InIrreducibleCycle) {
            GuardInLoop blocks(code, testCase == Case::InLoop ? GuardInLoop::Entries::AtGuard : GuardInLoop::Entries::AtGuardAndLatch);
            auto compilation = compile(proc);
            CHECK(code.findNextBlock(blocks.guard) != blocks.rare);
            GuardInLoop::checkResults(*compilation);
            continue;
        }

        BasicBlock* guard = code.addBlock();
        BasicBlock* next = code.addBlock();
        if (testCase == Case::HasLikelyPredecessor) {
            BasicBlock* last = code.addBlock();
            BasicBlock* rare = code.addBlock(0.01);
            appendBranchTest(guard, argumentTmp(0), rarely(rare), next);
            appendBranchTest(next, argumentTmp(1), rare, last);
            appendReturn(last, 1);
            appendReturn(rare, 3);

            auto compilation = compile(proc);
            CHECK(code.findNextBlock(guard) != rare);
            CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 0, 0)) == 1);
            CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 1, 0)) == 3);
            CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 0, 1)) == 3);
            continue;
        }

        BasicBlock* rare = code.addBlock(0.01);
        int64_t likelyArgument = 0;
        int64_t rareArgument = 1;
        if (testCase == Case::PatchTerminal) {
            guard->append(Patch, addTerminalPatchpoint(proc), Arg::special(patchpointSpecialOf(code)), returnTmp());
            guard->setSuccessors(next, rarely(rare));
            next->append(Ret64, nullptr, returnTmp());
        } else if (testCase == Case::ConditionIsNotInvertible || testCase == Case::FallsThroughToRareBlock) {
            guard->append(BranchAdd32, nullptr, Arg::resCond(MacroAssembler::Overflow), Arg::imm(1), argumentTmp(0));
            if (testCase == Case::ConditionIsNotInvertible) {
                guard->setSuccessors(rarely(rare), next);
                rareArgument = maxInt32;
            } else {
                guard->setSuccessors(next, rarely(rare));
                likelyArgument = maxInt32;
                rareArgument = 0;
            }
            appendReturn(next, 1);
        } else {
            appendBranchTest(guard, argumentTmp(0), rarely(rare), next);
            appendReturn(next, 1);
        }
        unsigned numberOfAdditions = 0;
        if (testCase == Case::AtSizeLimit)
            numberOfAdditions = maxInstsNearGuard - 1;
        if (testCase == Case::TooLarge)
            numberOfAdditions = maxInstsNearGuard;
        BlocksAroundGap::appendAdditions(rare, numberOfAdditions);
        appendReturn(rare, 3);

        auto compilation = compile(proc);
        // reportUsedRegisters() removes the last addition, whose result nothing reads.
        if (numberOfAdditions)
            CHECK(rare->size() == numberOfAdditions + 1);
        bool followsGuard = testCase == Case::Placed || testCase == Case::AtSizeLimit || testCase == Case::FallsThroughToRareBlock;
        CHECK((code.findNextBlock(guard) == rare) == followsGuard);
        CHECK((invoke<int64_t, int64_t>(*compilation, likelyArgument)) == (testCase == Case::PatchTerminal ? 42 : 1));
        CHECK((invoke<int64_t, int64_t>(*compilation, rareArgument)) == 3);
    }
}

void testDefaultLayoutIsUpstream()
{
    for (bool placesRareBlocksInGaps : { false, true }) {
        B3::Policy policy;
        if (placesRareBlocksInGaps)
            policy.air.layout.placesRareBlocksInGaps = true;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        BlocksAroundGap blocks(code);
        BlocksAroundGap::appendReturn(blocks.afterGuard, 1);
        BlocksAroundGap::appendReturn(blocks.rare, 3);
        CHECK(blocks.rareBlockPrecedesOtherSuccessor(proc) == placesRareBlocksInGaps);
        if (placesRareBlocksInGaps)
            checkBlockOrder(code, { blocks.root, blocks.guard, blocks.afterGuard, blocks.rare, blocks.otherSuccessor });
        else
            checkBlockOrder(code, { blocks.root, blocks.guard, blocks.afterGuard, blocks.otherSuccessor, blocks.rare });
    }

    {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* first = code.addBlock();
        BasicBlock* second = code.addBlock();
        BasicBlock* last = code.addBlock();
        BasicBlock* rareA = code.addBlock(0.01);
        BasicBlock* rareB = code.addBlock(0.01);
        appendBranchTest(first, argumentTmp(0), rarely(rareA), second);
        appendBranchTest(second, argumentTmp(1), rarely(rareB), last);
        appendReturn(last, 1);
        appendReturn(rareA, 2);
        appendReturn(rareB, 3);

        auto compilation = compile(proc);
        checkBlockOrder(code, { first, second, last, rareB, rareA });
        CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 0, 0)) == 1);
        CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 1, 0)) == 2);
        CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 1, 1)) == 2);
        CHECK((invoke<int64_t, int64_t, int64_t>(*compilation, 0, 1)) == 3);
    }

    {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        GuardInLoop blocks(code);
        auto compilation = compile(proc);
        checkBlockOrder(code, { blocks.entry, blocks.guard, blocks.latch, blocks.exit, blocks.rare });
        GuardInLoop::checkResults(*compilation);
    }
}
