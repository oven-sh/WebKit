/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define RUN_BUN_PASS_TESTS() do { \
        RUN(testHoistCommonMoves()); \
        RUN(testHoistMovesFromBackEdgeBlock()); \
        RUN(testHoistMovesFromOneBackEdgeBlockPerLatch()); \
        RUN(testTerminalResultsOnEdge()); \
        RUN(testMovesBeforePatch()); \
        RUN(testForwardRegisterCopies()); \
        RUN(testForwardRegisterCopiesDropsZeroExtensions()); \
        RUN(testMergeIdenticalBlocks()); \
        RUN(testMergeChainsOfIdenticalBlocks()); \
        RUN(testMergeInvertsBranchIntoFallThrough()); \
        RUN(testDuplicateReturns()); \
        RUN(testShuffleOrderFollowsPairs()); \
        RUN(testShuffleMovesValuesInEitherOrder()); \
        RUN(testSpilledOperandIsReadInPlaceOnRequest()); \
    } while (false)

struct PassRegisters {
    Tmp a0 { argumentTmp(0) };
    Tmp a1 { argumentTmp(1) };
    Tmp a2 { argumentTmp(2) };
    Tmp a3 { argumentTmp(3) };
    Tmp a4 { argumentTmp(4) };
    Tmp ret { returnTmp() }; // The same register as a0 on ARM64.
};

static void appendCheckedInst(BasicBlock* block, Inst&& inst)
{
    CHECK(inst.isValidForm(CPUFeatureSet::host()));
    block->appendInst(WTF::move(inst));
}

template<typename... Arguments>
static void appendChecked(BasicBlock* block, B3::Air::Opcode opcode, Arguments... arguments)
{
    appendCheckedInst(block, Inst(opcode, nullptr, arguments...));
}

template<typename... Arguments>
static void appendCheckedPatch(BasicBlock* block, B3::PatchpointValue* value, B3::Air::Special* special, Arguments... arguments)
{
    appendCheckedInst(block, Inst(Patch, value, Arg::special(special), arguments...));
}

static void appendWasmBoundsCheck(B3::Procedure& proc, BasicBlock* block, Tmp pointer, Tmp size)
{
    B3::Value* origin = proc.add<B3::WasmBoundsCheckValue>(B3::Origin(), size.gpr(), proc.addConstant(B3::Origin(), B3::Int64, 0), 0);
    appendCheckedInst(block, Inst(WasmBoundsCheck, origin, pointer, size));
}

static B3::Value* appendTrappingLoad(B3::Procedure& proc, BasicBlock* block, Arg address, Tmp loaded)
{
    B3::Value* origin = proc.add<B3::MemoryValue>(B3::trapping(B3::Load8Z), B3::Int32, B3::Origin(), proc.addConstant(B3::Origin(), B3::Int64, 0));
    Inst inst(Load8, origin, address, loaded);
    inst.kind.effects = true;
    appendCheckedInst(block, WTF::move(inst));
    return origin;
}

static void appendMoveAndJump(BasicBlock* block, int32_t value, BasicBlock* target)
{
    appendChecked(block, Move, Arg::imm(value), returnTmp());
    appendJump(block, target);
}

static void appendAddAndJump(BasicBlock* block, int32_t addend, BasicBlock* target)
{
    appendChecked(block, Add64, Arg::imm(addend), returnTmp());
    appendJump(block, target);
}

static void appendAddAndReturn(BasicBlock* block, int32_t addend)
{
    appendChecked(block, Add64, Arg::imm(addend), returnTmp());
    appendReturn(block);
}

static void appendRegisterChild(B3::Procedure& proc, B3::PatchpointValue* value, Tmp tmp)
{
    value->append(proc.addConstant(B3::Origin(), B3::Int64, 0), B3::ValueRep::reg(tmp.reg()));
}

static B3::PatchpointValue* addEmptyPatchpoint(B3::Procedure& proc)
{
    B3::PatchpointValue* value = addPatchpoint(proc);
    value->effects = B3::Effects::none();
    value->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
    return value;
}

static bool isInst(const Inst& inst, B3::Air::Opcode opcode, std::initializer_list<Arg> arguments)
{
    return inst.kind.opcode == opcode && std::ranges::equal(inst.args(), arguments);
}

static unsigned numberOfMovesInto(Code& code, Tmp destination)
{
    unsigned result = 0;
    for (BasicBlock* block : code) {
        for (const Inst& inst : *block)
            result += inst.kind.opcode == Move && inst.args().size() == 2 && inst.args()[1] == Arg(destination);
    }
    return result;
}

static Vector<unsigned> blockSizes(Code& code)
{
    Vector<unsigned> result;
    for (BasicBlock* block : code)
        result.append(block->size());
    return result;
}

static void checkPassChangesCode(Code& code, bool (*pass)(Code&))
{
    code.resetReachability();
    CHECK(pass(code));
    validate(code);
}

static void checkPassLeavesCodeUnchanged(Code& code, bool (*pass)(Code&))
{
    code.resetReachability();
    Vector<unsigned> sizes = blockSizes(code);
    CHECK(!pass(code));
    CHECK(blockSizes(code) == sizes);
}

template<typename... Arguments>
static int64_t invokeInt64(const Compilation& compilation, Arguments... arguments)
{
    return invoke<int64_t>(compilation, static_cast<int64_t>(arguments)...);
}

// The hash of an argument on the stack is the hash of the address of its slot, and two builds of one program must give the same bytes.
void testShuffleOrderFollowsPairs()
{
    constexpr unsigned numberOfPairs = 8;
    for (unsigned round = 0; round < 16; ++round) {
        bool followsPairs = round < 8;
        B3::Policy policy;
        policy.air.peepholes.emitsShufflesInPairOrder = followsPairs;
        B3::Procedure proc(policy);
        Code& code = proc.code();
        const Vector<Reg>& registers = code.regsInPriorityOrder(GP);
        CHECK(registers.size() >= numberOfPairs);

        Vector<ShufflePair> pairs;
        for (unsigned index = 0; index < numberOfPairs; ++index) {
            for (unsigned unused = 0; unused < (round * (index + 1)) % 5; ++unused)
                code.addStackSlot(8, StackSlotKind::Spill);
            pairs.append(ShufflePair(Arg::stack(code.addStackSlot(8, StackSlotKind::Spill)), Tmp(registers[index]), Width64));
        }

        Vector<Inst> insts = emitShuffle(code, pairs, { Arg(), Arg() }, GP, nullptr);
        CHECK(insts.size() == numberOfPairs);
        for (unsigned index = 0; index < numberOfPairs; ++index) {
            const ShufflePair& pair = pairs[numberOfPairs - 1 - index];
            auto movesPair = [&](const Inst& inst) {
                return inst.kind.opcode == Move && inst.args()[0] == pair.src() && inst.args()[1] == pair.dst();
            };
            CHECK(followsPairs ? movesPair(insts[index]) : insts.containsIf(movesPair));
        }
    }
}

// Upstream's tests of Shuffle run under the default policy, which takes shifts and rotates in the order of a hash table.
void testShuffleMovesValuesInEitherOrder()
{
    constexpr unsigned numberOfRegisters = 6;
    WeakRandom random(1);
    for (unsigned round = 0; round < 100; ++round) {
        std::array<unsigned, numberOfRegisters> sourceOf;
        for (unsigned destination = 0; destination < numberOfRegisters; ++destination) {
            unsigned source = random.getUint32(numberOfRegisters + 2);
            sourceOf[destination] = source < numberOfRegisters ? source : destination;
        }

        for (bool followsPairs : { false, true }) {
            B3::Policy policy;
            policy.air.peepholes.emitsShufflesInPairOrder = followsPairs;
            B3::Procedure proc(policy);
            proc.setOptLevel(2);
            proc.setUsesShuffle(true);
            Code& code = proc.code();
            const Vector<Reg>& registers = code.regsInPriorityOrder(GP);
            CHECK(registers.size() > numberOfRegisters);

            BasicBlock* root = code.addBlock();
            Vector<ShufflePair> pairs;
            for (unsigned index = 0; index < numberOfRegisters; ++index) {
                appendChecked(root, Move, Arg::imm(index + 1), Tmp(registers[index]));
                pairs.append(ShufflePair(Tmp(registers[sourceOf[index]]), Tmp(registers[index]), Width64));
            }
            appendCheckedInst(root, createShuffle(nullptr, pairs.span()));

            int64_t memory[numberOfRegisters] = { };
            Tmp base = code.newTmp(GP);
            appendChecked(root, Move, Arg::bigImm(std::bit_cast<intptr_t>(&memory)), base);
            for (unsigned index = 0; index < numberOfRegisters; ++index)
                appendChecked(root, Move, Tmp(registers[index]), Arg::addr(base, static_cast<int32_t>(index * sizeof(int64_t))));
            appendReturn(root, 0);

            CHECK(!compileAndRun<int64_t>(proc));
            for (unsigned index = 0; index < numberOfRegisters; ++index)
                CHECK(memory[index] == sourceOf[index] + 1);
        }
    }
}

// Upstream's Air has no form of these opcodes that reads the operand from memory, and the JIT tiers emit upstream's code.
static bool readsSpilledOperandInPlace(B3::Air::Opcode opcode, bool extendsInstructionSelection)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    B3::Policy policy;
    policy.selection.extendsInstructionSelection = extendsInstructionSelection;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();

    RegisterSet clobbered;
    for (Reg reg : code.regsInPriorityOrder(GP))
        clobbered.add(reg, IgnoreVectors);
    clobbered.exclude(RegisterSet { a1.reg(), a2.reg(), a3.reg() });
    B3::PatchpointValue* patchpoint = addPatchpoint(proc);
    patchpoint->clobberLate(clobbered);
    patchpoint->setGenerator([clobbered](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
        clobbered.forEach([&](Reg reg) {
            jit.move(CCallHelpers::TrustedImm32(-1), reg.gpr());
        });
    });

    BasicBlock* root = code.addBlock();
    Tmp operand = code.newTmp(GP);
    Arg nonZero = Arg::resCond(MacroAssembler::NonZero);
    appendChecked(root, Move, a0, operand);
    appendCheckedPatch(root, patchpoint, patchpointSpecialOf(code));
    switch (opcode) {
    case SignExtend32To64:
        appendChecked(root, SignExtend32To64, operand, ret);
        break;
    case Test64:
        appendChecked(root, Test64, nonZero, operand, a1, ret);
        break;
    case MoveConditionallyTest64:
        appendChecked(root, MoveConditionallyTest64, nonZero, operand, a1, a2, a3, ret);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    for (Tmp liveAcrossPatch : { a1, a2, a3 })
        appendChecked(root, Add64, liveAcrossPatch, ret);
    appendReturn(root);

    auto compilation = compile(proc);
    for (int64_t value : { 0ll, 4ll, 0x180000001ll }) {
        constexpr int64_t mask = 5;
        constexpr int64_t ifNonZero = 100;
        constexpr int64_t ifZero = 1000;
        int64_t expected = static_cast<int32_t>(value);
        if (opcode == Test64)
            expected = !!(value & mask);
        else if (opcode == MoveConditionallyTest64)
            expected = (value & mask) ? ifNonZero : ifZero;
        CHECK(invokeInt64(*compilation, value, mask, ifNonZero, ifZero) == expected + mask + ifNonZero + ifZero);
    }

    bool result = false;
    for (const Inst& inst : *root) {
        if (inst.kind.opcode == opcode)
            result = std::ranges::any_of(inst.args(), &Arg::isMemory);
    }
    return result;
}

void testSpilledOperandIsReadInPlaceOnRequest()
{
    for (B3::Air::Opcode opcode : { SignExtend32To64, Test64, MoveConditionallyTest64 }) {
        CHECK(!readsSpilledOperandInPlace(opcode, false));
        CHECK(readsSpilledOperandInPlace(opcode, true) == (isX86() || opcode == SignExtend32To64));
    }
}

void testHoistCommonMoves()
{
    enum class Case : uint8_t { Hoisted, BranchOnFlags, BranchTestsDestination, BranchDefinesSource, SecondPredecessor, DifferentSources, Move32 };
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    for (Case testCase : { Case::Hoisted, Case::BranchOnFlags, Case::BranchTestsDestination, Case::BranchDefinesSource, Case::SecondPredecessor, Case::DifferentSources, Case::Move32 }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        bool hasSecondPredecessor = testCase == Case::SecondPredecessor;
        BasicBlock* entry = hasSecondPredecessor ? code.addBlock() : nullptr;
        BasicBlock* root = code.addBlock();
        BasicBlock* other = hasSecondPredecessor ? code.addBlock() : nullptr;
        BasicBlock* taken = code.addBlock();
        BasicBlock* notTaken = code.addBlock();

        if (hasSecondPredecessor) {
            appendBranchTest(entry, a3, root, other);
            appendJump(other, taken);
        }
        if (testCase == Case::BranchOnFlags) {
            appendChecked(root, CompareOnFlags32, a0, a1);
            appendChecked(root, BranchOnFlags, Arg::relCond(MacroAssembler::Equal));
            root->setSuccessors(taken, notTaken);
        } else if (testCase == Case::BranchDefinesSource) {
            appendChecked(root, BranchAdd32, Arg::resCond(MacroAssembler::Overflow), Arg::imm(1), a1);
            root->setSuccessors(taken, notTaken);
        } else
            appendBranchTest(root, testCase == Case::BranchTestsDestination ? a2 : a0, taken, notTaken);
        auto appendSuccessor = [&](BasicBlock* block, Tmp source, int32_t addend) {
            appendChecked(block, testCase == Case::Move32 ? Move32 : Move, source, a2);
            appendChecked(block, Add64, Arg::imm(addend), a2);
            appendChecked(block, Move, a2, ret);
            appendReturn(block);
        };
        appendSuccessor(taken, a1, 1);
        appendSuccessor(notTaken, testCase == Case::DifferentSources ? a3 : a1, 2);

        unsigned branchIndex = root->size() - 1;
        if (testCase == Case::Hoisted || testCase == Case::Move32 || (testCase == Case::BranchOnFlags && preservesFlags(Move))) {
            checkPassChangesCode(code, hoistCommonMoves);
            CHECK(root->size() == branchIndex + 2);
            CHECK(isInst(root->at(branchIndex), testCase == Case::Move32 ? Move32 : Move, { a1, a2 }));
            CHECK(root->last().isTerminal());
            CHECK(taken->size() == 3);
            CHECK(notTaken->size() == 3);
        } else
            checkPassLeavesCodeUnchanged(code, hoistCommonMoves);
        CHECK(!findUseOfUndefinedFlags(code));
        auto compilation = compile(proc);
        if (testCase == Case::BranchDefinesSource) {
            CHECK(invokeInt64(*compilation, 0, 10) == 13);
            CHECK(invokeInt64(*compilation, 0, maxInt32) == maxInt32 + 2);
            continue;
        }
        CHECK(invokeInt64(*compilation, 10, 10, 1, 20) == 11);
        CHECK(invokeInt64(*compilation, 0, 10, 0, 20) == (testCase == Case::DifferentSources ? 22 : 12));
        if (hasSecondPredecessor)
            CHECK(invokeInt64(*compilation, 0, 10, 0, 0) == 11);
    }
}

// sum = 0; do { previous = current; current += step; sum += previous; } while (--count); with `Move next, current` on the back edge, as the register allocator leaves it.
void testHoistMovesFromBackEdgeBlock()
{
    enum class Case : uint8_t { Hoisted, DestinationIsLiveAtExit, BranchTestsDestination, BackEdgeIsRare, BackEdgeBlockAdds, JumpsForward };
    auto [count, current, step, next, sum, ret] = PassRegisters { };
    for (Case testCase : { Case::Hoisted, Case::DestinationIsLiveAtExit, Case::BranchTestsDestination, Case::BackEdgeIsRare, Case::BackEdgeBlockAdds, Case::JumpsForward }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* body = code.addBlock(10);
        BasicBlock* backEdgeBlock = code.addBlock(9);
        BasicBlock* exit = code.addBlock();
        BasicBlock* join = code.addBlock();

        appendChecked(root, Move, Arg::imm(0), sum);
        appendJump(root, body);
        appendChecked(body, Add64, current, step, next);
        appendChecked(body, Add64, current, sum);
        appendChecked(body, Add64, Arg::imm(-1), count);
        FrequentedBlock backEdge(backEdgeBlock, testCase == Case::BackEdgeIsRare ? B3::FrequencyClass::Rare : B3::FrequencyClass::Normal);
        appendBranchTest(body, testCase == Case::BranchTestsDestination ? current : count, backEdge, exit);
        if (testCase == Case::BackEdgeBlockAdds)
            appendChecked(backEdgeBlock, Add64, Arg::imm(0), next);
        appendChecked(backEdgeBlock, Move, next, current);
        appendJump(backEdgeBlock, testCase == Case::JumpsForward ? join : body);
        if (testCase == Case::DestinationIsLiveAtExit)
            appendChecked(exit, Add64, current, sum);
        appendJump(exit, join);
        appendChecked(join, Move, sum, ret);
        appendReturn(join);

        unsigned branchIndex = body->size() - 1;
        if (testCase == Case::Hoisted) {
            checkPassChangesCode(code, hoistCommonMoves);
            CHECK(body->size() == branchIndex + 2);
            CHECK(isInst(body->at(branchIndex), Move, { next, current }));
            CHECK(body->last().isTerminal());
            CHECK(backEdgeBlock->size() == 1);
        } else
            checkPassLeavesCodeUnchanged(code, hoistCommonMoves);
        if (testCase == Case::BranchTestsDestination || testCase == Case::JumpsForward)
            continue;
        auto compilation = compile(proc);
        CHECK(invokeInt64(*compilation, 3, 10, 5) == (testCase == Case::DestinationIsLiveAtExit ? 65 : 45));
        CHECK(invokeInt64(*compilation, 1, 10, 5) == (testCase == Case::DestinationIsLiveAtExit ? 20 : 10));
    }
}

// The inner loop's bottom test branches to the inner header on one side and to the outer header on the other, and both back edge blocks write the same register.
// sum = 0; while (--count) { left = 2; do { sum += carried; carried = --left ? step : sum; } while (left); }
void testHoistMovesFromOneBackEdgeBlockPerLatch()
{
    auto [count, carried, left, step, sum, ret] = PassRegisters { };
    for (bool secondMoveReadsFirstDestination : { false, true }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* outer = code.addBlock(10);
        BasicBlock* inner = code.addBlock(10);
        BasicBlock* backToInner = code.addBlock(9);
        BasicBlock* backToOuter = code.addBlock(9);
        BasicBlock* exit = code.addBlock();

        appendChecked(root, Move, Arg::imm(0), sum);
        appendJump(root, outer);
        appendChecked(outer, Add64, Arg::imm(-1), count);
        appendChecked(outer, Move, Arg::imm(2), left);
        appendBranchTest(outer, count, inner, exit);
        appendChecked(inner, Add64, carried, sum);
        appendChecked(inner, Add64, Arg::imm(-1), left);
        appendBranchTest(inner, left, backToInner, backToOuter);
        appendChecked(backToInner, Move, step, carried);
        appendJump(backToInner, inner);
        if (secondMoveReadsFirstDestination)
            appendChecked(backToOuter, Move, carried, step);
        else
            appendChecked(backToOuter, Move, sum, carried);
        appendJump(backToOuter, outer);
        appendChecked(exit, Move, sum, ret);
        appendReturn(exit);

        unsigned branchIndex = inner->size() - 1;
        if (secondMoveReadsFirstDestination)
            checkPassLeavesCodeUnchanged(code, hoistCommonMoves);
        else {
            checkPassChangesCode(code, hoistCommonMoves);
            CHECK(inner->size() == branchIndex + 2);
            CHECK(backToInner->size() + backToOuter->size() == 3);
        }
        auto compilation = compile(proc);
        CHECK(invokeInt64(*compilation, 3, 10, 0, 5) == (secondMoveReadsFirstDestination ? 25 : 35));
    }
}

static void checkTerminalWithoutSuccessors()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    appendCheckedPatch(root, addTerminalPatchpoint(proc), patchpointSpecialOf(code), returnTmp());
    appendChecked(root, Move, returnTmp(), code.newTmp(GP));

    checkPassChangesCode(code, fixSpillsAfterTerminals);
    CHECK(root->size() == 1);
    CHECK(root->last().kind.opcode == Patch);
}

static void checkSuccessorWithEntryClobber(bool padHasSecondPredecessor)
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* entry = padHasSecondPredecessor ? code.addBlock() : nullptr;
    BasicBlock* root = code.addBlock();
    BasicBlock* other = padHasSecondPredecessor ? code.addBlock() : nullptr;
    BasicBlock* continuation = code.addBlock();
    BasicBlock* pad = code.addBlock();
    Arg slot = Arg::stack(code.addStackSlot(8, StackSlotKind::Spill));
    Arg entryClobber = Arg::special(code.addSpecial(makeUnique<BlockEntryClobberSpecial>(RegisterSet { argumentTmp(2).reg() })));

    if (padHasSecondPredecessor) {
        appendBranchTest(entry, argumentTmp(1), root, other);
        appendJump(other, pad);
    }
    appendCheckedPatch(root, addTerminalPatchpoint(proc), patchpointSpecialOf(code), returnTmp());
    root->setSuccessors(continuation, pad);
    appendChecked(root, Move, returnTmp(), slot);
    appendChecked(continuation, Move, slot, returnTmp());
    appendReturn(continuation);
    appendCheckedInst(pad, Inst(Patch, nullptr, entryClobber));
    appendReturn(pad, 7);

    checkPassChangesCode(code, fixSpillsAfterTerminals);
    CHECK(isInst(continuation->at(0), Move, { returnTmp(), slot }));
    CHECK(isInst(pad->at(0), Patch, { entryClobber }));
    BasicBlock* blockWithMove = root->successorBlock(1);
    CHECK((blockWithMove != pad) == padHasSecondPredecessor);
    CHECK(isInst(blockWithMove->at(0), Patch, { entryClobber }));
    CHECK(isInst(blockWithMove->at(1), Move, { returnTmp(), slot }));
    if (padHasSecondPredecessor) {
        CHECK(blockWithMove->size() == 3);
        CHECK(blockWithMove->successorBlock(0) == pad);
        CHECK(!isInst(pad->at(1), Move, { returnTmp(), slot }));
    }
    CHECK(!findMisplacedBlockEntryClobber(code));
    auto compilation = compile(proc);
    CHECK(invokeInt64(*compilation, 0, 1) == 42);
    CHECK(invokeInt64(*compilation, 1, 1) == 7);
    if (padHasSecondPredecessor)
        CHECK(invokeInt64(*compilation, 0, 0) == 7);
}

static void checkManyTerminalResults()
{
    constexpr unsigned numberOfTerminals = 150;
    enum class Reader : uint8_t { LastBlock, NextBlock, Nobody };
    auto readerOf = [](unsigned index) {
        return static_cast<Reader>(index % 3);
    };

    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    B3::Air::Special* special = patchpointSpecialOf(code);
    Vector<BasicBlock*> blocks;
    for (unsigned i = 0; i <= numberOfTerminals; ++i)
        blocks.append(code.addBlock());
    BasicBlock* exit = code.addBlock();
    Tmp sum = code.newTmp(GP);
    Vector<Tmp> results;

    appendChecked(blocks[0], Move, Arg::imm(0), sum);
    for (unsigned i = 0; i < numberOfTerminals; ++i) {
        results.append(code.newTmp(GP));
        appendCheckedPatch(blocks[i], addTerminalPatchpoint(proc), special, returnTmp());
        blocks[i]->setSuccessors(blocks[i + 1], exit);
        appendChecked(blocks[i], Move, returnTmp(), results[i]);
        if (readerOf(i) == Reader::NextBlock)
            appendChecked(blocks[i + 1], Add64, results[i], sum);
    }
    for (unsigned i = 0; i < numberOfTerminals; ++i) {
        if (readerOf(i) == Reader::LastBlock)
            appendChecked(blocks.last(), Add64, results[i], sum);
    }
    appendChecked(blocks.last(), Move, sum, returnTmp());
    appendReturn(blocks.last());
    appendChecked(exit, Move, results[0], returnTmp());
    appendReturn(exit);

    checkPassChangesCode(code, fixSpillsAfterTerminals);
    for (unsigned i = 0; i < numberOfTerminals; ++i) {
        CHECK(blocks[i]->last().kind.opcode == Patch);
        CHECK(blocks[i]->successorBlock(0) == blocks[i + 1]);
        CHECK(isInst(blocks[i + 1]->at(0), Move, { returnTmp(), results[i] }) == (readerOf(i) != Reader::Nobody));
        CHECK((blocks[i]->successorBlock(1) == exit) == !!i);
    }
    CHECK(isInst(blocks[0]->successorBlock(1)->at(0), Move, { returnTmp(), results[0] }));
}

void testTerminalResultsOnEdge()
{
    enum class Case : uint8_t { OnePredecessor, TwoPredecessors, SameSuccessorTwice, LiveAtBothSuccessors, StoresToStackSlot, NothingTrails };
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    for (Case testCase : { Case::OnePredecessor, Case::TwoPredecessors, Case::SameSuccessorTwice, Case::LiveAtBothSuccessors, Case::StoresToStackSlot, Case::NothingTrails }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        Tmp result = code.newTmp(GP);
        Arg destination = testCase == Case::StoresToStackSlot ? Arg::stack(code.addStackSlot(8, StackSlotKind::Spill)) : Arg(result);

        bool hasTwoPredecessors = testCase == Case::TwoPredecessors;
        BasicBlock* entry = hasTwoPredecessors ? code.addBlock() : nullptr;
        BasicBlock* root = code.addBlock();
        BasicBlock* other = hasTwoPredecessors ? code.addBlock() : nullptr;
        BasicBlock* continuation = code.addBlock();
        BasicBlock* exit = testCase == Case::SameSuccessorTwice ? continuation : code.addBlock();

        if (hasTwoPredecessors) {
            appendBranchTest(entry, a1, root, other);
            appendChecked(other, Move, Arg::imm(100), result);
            appendJump(other, continuation);
        }
        appendCheckedPatch(root, addTerminalPatchpoint(proc), patchpointSpecialOf(code), ret);
        root->setSuccessors(continuation, exit);
        if (testCase == Case::NothingTrails)
            appendAddAndReturn(continuation, 1);
        else {
            appendChecked(root, Move, ret, destination);
            if (testCase == Case::StoresToStackSlot)
                appendChecked(continuation, Move, destination, result);
            appendChecked(continuation, Add64, Arg::imm(1), result);
            appendChecked(continuation, Move, result, ret);
            appendReturn(continuation);
        }
        if (testCase == Case::LiveAtBothSuccessors)
            appendChecked(exit, Move, result, a2);
        if (exit != continuation)
            appendReturn(exit, 7);

        unsigned initialNumberOfBlocks = numberOfBlocks(code);
        auto startsWithMove = [&](BasicBlock* block) {
            return isInst(block->at(0), Move, { ret, destination });
        };
        auto countMovesOnEdge = [&](unsigned successorIndex) {
            unsigned count = startsWithMove(continuation);
            BasicBlock* successor = root->successorBlock(successorIndex);
            if (successor == continuation)
                return count;
            CHECK(successor->size() == 2);
            CHECK(successor->last().kind.opcode == Jump);
            CHECK(successor->successorBlock(0) == continuation);
            return count + startsWithMove(successor);
        };

        switch (testCase) {
        case Case::OnePredecessor:
            checkPassChangesCode(code, fixSpillsAfterTerminals);
            CHECK(numberOfBlocks(code) == initialNumberOfBlocks);
            CHECK(startsWithMove(continuation));
            CHECK(!startsWithMove(exit));
            break;
        case Case::TwoPredecessors:
            checkPassChangesCode(code, fixSpillsAfterTerminals);
            CHECK(numberOfBlocks(code) == initialNumberOfBlocks + 1);
            CHECK(!startsWithMove(continuation));
            CHECK(countMovesOnEdge(0) == 1);
            CHECK(root->successorBlock(1) == exit);
            CHECK(!startsWithMove(exit));
            break;
        case Case::SameSuccessorTwice:
            checkPassChangesCode(code, fixSpillsAfterTerminals);
            CHECK(countMovesOnEdge(0) == 1);
            CHECK(countMovesOnEdge(1) == 1);
            break;
        case Case::LiveAtBothSuccessors:
        case Case::StoresToStackSlot:
            checkPassChangesCode(code, fixSpillsAfterTerminals);
            CHECK(numberOfBlocks(code) == initialNumberOfBlocks);
            CHECK(startsWithMove(continuation));
            CHECK(startsWithMove(exit));
            break;
        case Case::NothingTrails:
            checkPassLeavesCodeUnchanged(code, fixSpillsAfterTerminals);
            break;
        }
        CHECK(root->last().kind.opcode == Patch);
        auto compilation = compile(proc);
        CHECK(invokeInt64(*compilation, 0, 1) == 43);
        if (exit != continuation)
            CHECK(invokeInt64(*compilation, 1, 1) == 7);
        if (hasTwoPredecessors)
            CHECK(invokeInt64(*compilation, 0, 0) == 101);
    }

    checkTerminalWithoutSuccessors();
    checkManyTerminalResults();
    checkSuccessorWithEntryClobber(false);
    checkSuccessorWithEntryClobber(true);
}

void testMovesBeforePatch()
{
    enum class Case : uint8_t { Chain, Swap, WideImmediate, ClobberedAndUsed };
    constexpr int64_t wideImmediate = 0x123456789;
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    for (Case testCase : { Case::Chain, Case::Swap, Case::WideImmediate, Case::ClobberedAndUsed }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        B3::Air::Special* special = patchpointSpecialOf(code);

        B3::PatchpointValue* value = addPatchpoint(proc, B3::Int64);
        value->effects = B3::Effects::none();
        value->resultConstraints = { B3::ValueRep::reg(GPRInfo::returnValueGPR) };

        if (testCase == Case::Swap) {
            appendChecked(root, Move, a0, a2);
            appendChecked(root, Move, a1, a0);
            appendChecked(root, Move, a2, a1);
            appendRegisterChild(proc, value, a0);
            appendRegisterChild(proc, value, a1);
            value->clobberLate(RegisterSet { a2.reg() });
            value->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
                jit.sub64(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);
            });
            appendCheckedPatch(root, value, special, ret, a0, a1);
        } else {
            appendChecked(root, Move, testCase == Case::WideImmediate ? Arg::bigImm(wideImmediate) : Arg(a0), a1);
            appendChecked(root, Move, a1, a2);
            appendRegisterChild(proc, value, a2);
            value->clobberLate(RegisterSet { a1.reg() });
            value->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
                jit.add64(CCallHelpers::TrustedImm32(1), GPRInfo::argumentGPR2, GPRInfo::returnValueGPR);
                jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::argumentGPR1);
            });
            if (testCase == Case::ClobberedAndUsed) {
                appendRegisterChild(proc, value, a1);
                appendCheckedPatch(root, value, special, ret, a2, a1);
            } else
                appendCheckedPatch(root, value, special, ret, a2);
        }
        appendReturn(root);

        if (testCase == Case::Chain) {
            checkPassChangesCode(code, resequenceMovesBeforePatches);
            CHECK(numberOfInsts(root, Move) == 1);
            CHECK(isInst(root->at(0), Move, { a0, a2 }));
            CHECK(root->at(1).kind.opcode == Patch);
        } else
            checkPassLeavesCodeUnchanged(code, resequenceMovesBeforePatches);
        auto compilation = compile(proc);
        if (testCase == Case::Swap)
            CHECK(invokeInt64(*compilation, 3, 10) == 10 - 3);
        else
            CHECK(invokeInt64(*compilation, 5) == (testCase == Case::WideImmediate ? wideImmediate + 1 : 6));
    }
}

static void appendCopiesAndAdd(BasicBlock* root, bool sourceChanges)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    appendChecked(root, Move, a0, a1);
    if (sourceChanges) {
        appendChecked(root, Add64, Arg::imm(1), a0);
        appendChecked(root, Add64, a1, a3);
    } else {
        appendChecked(root, Move, a1, a2);
        appendChecked(root, Add64, a2, a3);
    }
    appendChecked(root, Move, a3, ret);
    appendReturn(root);
}

void testForwardRegisterCopies()
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    for (bool sourceChanges : { false, true }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        appendCopiesAndAdd(root, sourceChanges);

        code.resetReachability();
        forwardRegisterCopies(code);
        if (sourceChanges)
            CHECK(root->at(2).args()[0] == Arg(a1));
        else {
            CHECK(root->at(1).args()[0] == Arg(a0));
            CHECK(root->at(2).args()[0] == Arg(a0));
        }
        auto compilation = compile(proc);
        CHECK(invokeInt64(*compilation, 5, 0, 0, 7) == 12);
    }

    for (bool forwardsRegisterCopies : { true, false }) {
        B3::Policy policy;
        policy.air.peepholes.forwardsRegisterCopies = forwardsRegisterCopies;
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        appendCopiesAndAdd(code.addBlock(), false);
        auto compilation = compile(proc);
        unsigned expectedMoves = forwardsRegisterCopies ? 0 : 1;
        CHECK(numberOfMovesInto(code, a1) == expectedMoves);
        CHECK(numberOfMovesInto(code, a2) == expectedMoves);
        CHECK(invokeInt64(*compilation, 5, 0, 0, 7) == 12);
    }
}

static void checkZeroExtensionAfter(const Vector<Inst>& definitions, bool isDropped)
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    auto build = [&](B3::Procedure& proc) {
        proc.setOptLevel(2);
        BasicBlock* root = proc.code().addBlock();
        for (const Inst& definition : definitions)
            appendCheckedInst(root, Inst(definition));
        appendChecked(root, Move32, a1, a1);
        appendChecked(root, Move, a1, ret);
        appendReturn(root);
        proc.code().resetReachability();
        return root;
    };

    B3::Procedure proc;
    BasicBlock* root = build(proc);
    unsigned numberOfInstsBefore = root->size();
    forwardRegisterCopies(proc.code());
    CHECK(root->size() == numberOfInstsBefore - (isDropped ? 1 : 0));

    B3::Procedure reference;
    build(reference);
    auto compilation = compile(proc);
    auto referenceCompilation = compile(reference);
    for (int64_t first : { 0ll, 3ll, -1ll, 0x123456789abcdef0ll }) {
        for (int64_t second : { 0ll, 1ll, -1ll, 0x7fffffffll, 0x0fedcba987654321ll })
            CHECK(invokeInt64(*compilation, first, second, 5) == invokeInt64(*referenceCompilation, first, second, 5));
    }
}

void testForwardRegisterCopiesDropsZeroExtensions()
{
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    Arg three = Arg::imm(3);
    Vector<Inst> wordDefinitions {
        Inst(Add32, nullptr, three, a1), Inst(Add32, nullptr, a0, a1), Inst(Add32, nullptr, a0, a1, a1), Inst(Sub32, nullptr, a0, a1), Inst(Mul32, nullptr, a0, a1),
        Inst(And32, nullptr, a0, a1), Inst(Or32, nullptr, a0, a1), Inst(Xor32, nullptr, a0, a1), Inst(Xor32, nullptr, a0, a1, a1), Inst(Neg32, nullptr, a1),
        Inst(Move32, nullptr, a0, a1), Inst(SignExtend8To32, nullptr, a0, a1), Inst(ZeroExtend16To32, nullptr, a0, a1),
    };
    for (B3::Air::Opcode shift : { Lshift32, Rshift32, Urshift32 })
        wordDefinitions.append(isX86() ? Inst(shift, nullptr, three, a1) : Inst(shift, nullptr, a1, three, a1));
    wordDefinitions.append(isX86() ? Inst(Not32, nullptr, a1) : Inst(Not32, nullptr, a1, a1));
    for (const Inst& definition : wordDefinitions) {
        CHECK(mayZeroExtendWord(definition.kind.opcode));
        checkZeroExtensionAfter({ definition }, true);
    }

    Inst addWord(Add32, nullptr, three, a1);
    CHECK(!mayZeroExtendWord(Add64) && !mayZeroExtendWord(Move) && !mayZeroExtendWord(SignExtend32To64));
    checkZeroExtensionAfter({ }, false);
    checkZeroExtensionAfter({ Inst(Add64, nullptr, three, a1) }, false);
    checkZeroExtensionAfter({ Inst(Move, nullptr, a0, a1) }, false);
    checkZeroExtensionAfter({ Inst(SignExtend32To64, nullptr, a0, a1) }, false);
    checkZeroExtensionAfter({ Inst(Add32, nullptr, three, a2) }, false);
    checkZeroExtensionAfter({ addWord, Inst(Add64, nullptr, a0, a1) }, false);
    checkZeroExtensionAfter({ addWord, Inst(Move, nullptr, a0, a1) }, false);
    checkZeroExtensionAfter({ Inst(Move, nullptr, a0, a1), addWord }, true);
}

static void checkEntrypointIsNotMerged()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* done = code.addBlock();
    BasicBlock* exit = code.addBlock();
    BasicBlock* again = code.addBlock();
    appendMoveAndJump(root, 1, done);
    appendBranchTest(done, argumentTmp(1), again, exit);
    appendMoveAndJump(again, 1, done);
    appendReturn(exit);

    checkPassLeavesCodeUnchanged(code, mergeIdenticalBlocks);
    auto compilation = compile(proc);
    CHECK(invokeInt64(*compilation, 0, 0) == 1);
}

void testMergeIdenticalBlocks()
{
    enum class Case : uint8_t { Merged, DifferentImmediates, DifferentSuccessors, WithPatch, WithWasmBoundsCheck, WithTrap };
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    for (Case testCase : { Case::Merged, Case::DifferentImmediates, Case::DifferentSuccessors, Case::WithPatch, Case::WithWasmBoundsCheck, Case::WithTrap }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* next1 = code.addBlock();
        BasicBlock* next2 = code.addBlock();
        BasicBlock* first = code.addBlock();
        BasicBlock* second = code.addBlock();
        BasicBlock* third = code.addBlock();
        BasicBlock* other = code.addBlock();
        BasicBlock* done = code.addBlock();

        bool hasOwnSuccessors = testCase == Case::DifferentSuccessors;
        BasicBlock* firstDone = hasOwnSuccessors ? code.addBlock() : done;
        BasicBlock* secondDone = hasOwnSuccessors ? code.addBlock() : done;
        BasicBlock* thirdDone = hasOwnSuccessors ? code.addBlock() : done;
        int32_t firstAddend = hasOwnSuccessors ? 20 : 10;
        int32_t secondAddend = hasOwnSuccessors ? 30 : 10;
        int32_t thirdAddend = hasOwnSuccessors ? 40 : 10;
        int32_t secondValue = testCase == Case::DifferentImmediates ? 3 : 1;
        int32_t thirdValue = testCase == Case::DifferentImmediates ? 4 : 1;

        appendBranchTest(root, a0, first, next1);
        appendBranchTest(next1, a1, second, next2);
        appendBranchTest(next2, a2, third, other);
        if (testCase == Case::WithPatch) {
            B3::PatchpointValue* value = addEmptyPatchpoint(proc);
            B3::Air::Special* special = patchpointSpecialOf(code);
            for (BasicBlock* block : { first, second, third })
                appendCheckedPatch(block, value, special);
        }
        if (testCase == Case::WithWasmBoundsCheck) {
            for (BasicBlock* block : { first, second, third })
                appendWasmBoundsCheck(proc, block, a3, a4);
        }
        if (testCase == Case::WithTrap) {
            for (BasicBlock* block : { first, second, third })
                appendTrappingLoad(proc, block, Arg::addr(a3), a4);
        }
        appendMoveAndJump(first, 1, firstDone);
        appendMoveAndJump(second, secondValue, secondDone);
        appendMoveAndJump(third, thirdValue, thirdDone);
        appendMoveAndJump(other, 2, done);
        appendAddAndReturn(done, 10);
        if (hasOwnSuccessors) {
            appendAddAndReturn(firstDone, firstAddend);
            appendAddAndReturn(secondDone, secondAddend);
            appendAddAndReturn(thirdDone, thirdAddend);
        }

        if (testCase == Case::Merged) {
            checkPassChangesCode(code, mergeIdenticalBlocks);
            CHECK(numberOfBlocks(code) == 6);
            CHECK(root->successorBlock(0) == first);
            CHECK(next1->successorBlock(0) == first);
            CHECK(next2->successorBlock(0) == first);
        } else
            checkPassLeavesCodeUnchanged(code, mergeIdenticalBlocks);
        if (testCase == Case::WithWasmBoundsCheck || testCase == Case::WithTrap)
            continue;
        auto compilation = compile(proc);
        CHECK(invokeInt64(*compilation, 1, 0, 0) == 1 + firstAddend);
        CHECK(invokeInt64(*compilation, 0, 1, 0) == secondValue + secondAddend);
        CHECK(invokeInt64(*compilation, 0, 0, 1) == thirdValue + thirdAddend);
        CHECK(invokeInt64(*compilation, 0, 0, 0) == 12);
    }

    checkEntrypointIsNotMerged();
}

void testMergeChainsOfIdenticalBlocks()
{
    B3::Procedure proc;
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* done = code.addBlock();
    BasicBlock* tailB = code.addBlock();
    BasicBlock* tailA = code.addBlock();
    BasicBlock* headB = code.addBlock();
    BasicBlock* headA = code.addBlock();

    appendChecked(root, Move, Arg::imm(0), returnTmp());
    appendBranchTest(root, argumentTmp(1), headA, headB);
    appendAddAndJump(headA, 1, tailA);
    appendAddAndJump(headB, 1, tailB);
    appendAddAndJump(tailA, 2, done);
    appendAddAndJump(tailB, 2, done);
    appendReturn(done);

    checkPassChangesCode(code, mergeIdenticalBlocks);
    CHECK(numberOfBlocks(code) == 4);
    CHECK(root->successorBlock(0) == headB);
    CHECK(root->successorBlock(1) == headB);
    CHECK(headB->successorBlock(0) == tailB);
    auto compilation = compile(proc);
    CHECK(invokeInt64(*compilation, 0, 1) == 3);
    CHECK(invokeInt64(*compilation, 0, 0) == 3);
}

void testMergeInvertsBranchIntoFallThrough()
{
    enum class Case : uint8_t { Branch, BranchAdd, ConditionIsNotInvertible };
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
    auto [a0, a1, a2, a3, a4, ret] = PassRegisters { };
    for (Case testCase : { Case::Branch, Case::BranchAdd, Case::ConditionIsNotInvertible }) {
        B3::Procedure proc;
        proc.setOptLevel(2);
        Code& code = proc.code();
        BasicBlock* root = code.addBlock();
        BasicBlock* source = code.addBlock();
        BasicBlock* duplicate = code.addBlock();
        BasicBlock* next = code.addBlock();
        BasicBlock* canonical = code.addBlock();
        BasicBlock* done = code.addBlock();

        appendBranchTest(root, a1, source, canonical);
        switch (testCase) {
        case Case::Branch:
            appendChecked(source, Branch32, Arg::relCond(MacroAssembler::Equal), a0, Arg::imm(5));
            break;
        case Case::BranchAdd:
            appendChecked(source, BranchAdd32, Arg::resCond(MacroAssembler::NonZero), Arg::imm(1), a0);
            break;
        case Case::ConditionIsNotInvertible:
            appendChecked(source, BranchAdd32, Arg::resCond(MacroAssembler::Overflow), Arg::imm(1), a0);
            break;
        }
        source->setSuccessors(next, duplicate);
        appendMoveAndJump(duplicate, 1, done);
        appendMoveAndJump(next, 2, done);
        appendMoveAndJump(canonical, 1, done);
        appendReturn(done);

        if (testCase == Case::ConditionIsNotInvertible)
            checkPassLeavesCodeUnchanged(code, mergeIdenticalBlocks);
        else {
            checkPassChangesCode(code, mergeIdenticalBlocks);
            CHECK(numberOfBlocks(code) == 5);
            if (testCase == Case::Branch)
                CHECK(isInst(source->last(), Branch32, { Arg::relCond(MacroAssembler::NotEqual), a0, Arg::imm(5) }));
            else
                CHECK(isInst(source->last(), BranchAdd32, { Arg::resCond(MacroAssembler::Zero), Arg::imm(1), a0 }));
            CHECK(source->successorBlock(0) == canonical);
            CHECK(source->successorBlock(1) == next);
        }
        auto compilation = compile(proc);
        switch (testCase) {
        case Case::Branch:
            CHECK(invokeInt64(*compilation, 5, 1) == 2);
            CHECK(invokeInt64(*compilation, 4, 1) == 1);
            break;
        case Case::BranchAdd:
            CHECK(invokeInt64(*compilation, 4, 1) == 2);
            CHECK(invokeInt64(*compilation, -1, 1) == 1);
            break;
        case Case::ConditionIsNotInvertible:
            CHECK(invokeInt64(*compilation, maxInt32, 1) == 2);
            CHECK(invokeInt64(*compilation, 4, 1) == 1);
            break;
        }
        CHECK(invokeInt64(*compilation, 0, 0) == 1);
    }
}

static unsigned countDefaultFrameReturns(bool savesRegister)
{
    B3::Policy policy;
    policy.air.layout.duplicatesReturns = true;
    B3::Procedure proc(policy);
    proc.setOptLevel(2);
    Code& code = proc.code();
    BasicBlock* root = code.addBlock();
    BasicBlock* first = code.addBlock();
    BasicBlock* second = code.addBlock();
    BasicBlock* exit = code.addBlock();

    Tmp condition = argumentTmp(0);
    if (savesRegister) {
        condition = Tmp(GPRInfo::regCS0);
        appendChecked(root, Move, argumentTmp(0), condition);
    }
    appendBranchTest(root, condition, first, second);
    appendMoveAndJump(first, 1, exit);
    appendMoveAndJump(second, 2, exit);
    appendReturn(exit);

    auto compilation = compile(proc);
    CHECK(!!code.calleeSaveRegisterAtOffsetList().registerCount() == savesRegister);
    CHECK(invokeInt64(*compilation, 1) == 1);
    CHECK(invokeInt64(*compilation, 0) == 2);
    return numberOfInsts(code, Ret64);
}

void testDuplicateReturns()
{
    enum class Case : uint8_t { Leaf, ReturnBlockHasTwoInsts, ReturnBlockIsNext, DefaultFrame };
    for (Case testCase : { Case::Leaf, Case::ReturnBlockHasTwoInsts, Case::ReturnBlockIsNext, Case::DefaultFrame }) {
        B3::Policy policy;
        if (testCase != Case::DefaultFrame)
            letAirEmitFrame(policy.air);
        B3::Procedure proc(policy);
        proc.setOptLevel(2);
        Code& code = proc.code();
        bool hasBranch = testCase != Case::ReturnBlockIsNext;
        BasicBlock* root = code.addBlock();
        BasicBlock* first = hasBranch ? code.addBlock() : root;
        BasicBlock* second = hasBranch ? code.addBlock() : nullptr;
        BasicBlock* exit = code.addBlock();

        if (hasBranch) {
            appendBranchTest(root, argumentTmp(0), first, second);
            appendMoveAndJump(second, 2, exit);
        }
        appendMoveAndJump(first, 1, exit);
        if (testCase == Case::ReturnBlockHasTwoInsts)
            appendAddAndReturn(exit, 0);
        else
            appendReturn(exit);

        switch (testCase) {
        case Case::Leaf:
            checkPassChangesCode(code, duplicateReturns);
            CHECK(isInst(first->last(), Ret64, { returnTmp() }));
            CHECK(!first->numSuccessors());
            CHECK(second->last().kind.opcode == Jump);
            CHECK(second->successorBlock(0) == exit);
            break;
        case Case::ReturnBlockHasTwoInsts:
        case Case::ReturnBlockIsNext:
            checkPassLeavesCodeUnchanged(code, duplicateReturns);
            break;
        case Case::DefaultFrame:
            code.resetReachability();
            CHECK(duplicateReturns(code) == isX86());
            break;
        }
        auto compilation = compile(proc);
        CHECK(code.frameLayout().isPlanned == (testCase != Case::DefaultFrame));
        CHECK(code.frameLayout().hasFrame == (testCase == Case::DefaultFrame));
        CHECK(invokeInt64(*compilation, 1) == 1);
        if (hasBranch)
            CHECK(invokeInt64(*compilation, 0) == 2);
    }

    CHECK(countDefaultFrameReturns(false) == (isX86() ? 2u : 1u));
    CHECK(countDefaultFrameReturns(true) == 1);
}
