/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT)

#include "AllowMacroScratchRegisterUsage.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Machine {

static_assert(EdgeClass::Normal < EdgeClass::Unlikely && EdgeClass::Unlikely < EdgeClass::Rare);

static ALWAYS_INLINE Test testOfEncodedOperands(Encoder& encoder, Test::Kind kind, Opcode relation, const Operation* fused, LeafCursor& cursor)
{
    const Operation* left = childWithoutCheck(fused, 0);
    const Operation* right = childWithoutCheck(fused, 1);
    bool leftIsFused = encoder.m_liveness.isFused(left) && left->opcode() != Trunc;
    bool rightIsFused = encoder.m_liveness.isFused(right) && right->opcode() != Trunc;

    if (leftIsFused && left->opcode() == Patch) {
        RELEASE_ASSERT(isX86_64() && kind == Test::TestBits && right->isInt(1));
        Test test { kind, relation, widthOf(left->type()), Location(), Location(), 0 };
        test.left = cursor.take();
        test.right = cursor.take();
        test.rightIsNumberOfBit = true;
        cursor.take();
        RELEASE_ASSERT(test.left.isRegister() && test.right.isRegister());
        return test;
    }

    if (leftIsFused || rightIsFused) {
        RELEASE_ASSERT(isX86_64() && kind != Test::CompareNumbers && leftIsFused != rightIsFused);
        Test test { kind, relation, widthOf(left->type()), Location(), Location(), 0 };
        const Operation* other = right;
        if (leftIsFused) {
            test.loadOnLeft = left;
            test.addressOnLeft = encoder.resolveAddress(left, cursor);
            test.right = cursor.take();
        } else {
            other = left;
            test.right = cursor.take();
            test.loadOnLeft = right;
            test.addressOnLeft = encoder.resolveAddress(right, cursor);
            if (kind == Test::CompareIntegers)
                test.relation = flippedComparison(relation);
        }
        RELEASE_ASSERT(test.right);
        if (test.right.isImmediate()) {
            RELEASE_ASSERT(other->hasInt());
            test.rightConstant = other->asInt();
        }
        return test;
    }

    Location leftLocation = cursor.take();
    Location rightLocation = cursor.take();
    RELEASE_ASSERT(leftLocation && rightLocation && !leftLocation.isImmediate());

    int64_t rightConstant = 0;
    if (rightLocation.isImmediate()) {
        RELEASE_ASSERT(right->isConstant());
        rightConstant = right->hasInt() ? right->asInt() : right->constantBits();
    }
    return Test { kind, relation, widthOf(left->type()), leftLocation, rightLocation, rightConstant };
}

static Test nonZeroTestOfEncodedValue(Encoder& encoder, const Operation* value, Location location)
{
    RELEASE_ASSERT(value->type().isInt());
    Width width = widthOf(value->type());

    switch (location.kind()) {
    case Location::Register:
        return Test { Test::TestBits, NotEqual, width, location, location, 0 };

    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord:
        return Test { Test::TestBits, NotEqual, width, location, Location::immediate(), -1 };

    case Location::Immediate: {
        AllowMacroScratchRegisterUsage allowScratch(encoder.m_jit);
        Reg scratch = encoder.m_jit.scratchRegister();
        encoder.encodeMaterialize(value, scratch);
        return Test { Test::TestBits, NotEqual, width, Location::reg(scratch), Location::reg(scratch), 0 };
    }

    case Location::None:
    case Location::SlotBytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Test testOfFusedOperation(Encoder& encoder, const Operation* tested, LeafCursor& cursor)
{
    RELEASE_ASSERT(encoder.m_liveness.isFused(tested));
    Opcode opcode = tested->opcode();
    if (isLoad(opcode)) {
        RELEASE_ASSERT(isX86_64() && tested->type().isInt());
        Test test { Test::TestBits, NotEqual, widthOf(tested->type()), Location(), Location::immediate(), -1 };
        test.loadOnLeft = tested;
        test.addressOnLeft = encoder.resolveAddress(tested, cursor);
        return test;
    }

    Test::Kind kind = Test::TestBits;
    Opcode relation = NotEqual;
    const Operation* fused = tested;
    bool comparesBitsWithZero = false;
    if (opcode != BitAnd) {
        RELEASE_ASSERT(isComparison(opcode));
        relation = opcode;
        const Operation* left = tested->child(0);
        if (left->opcode() == BitAnd && encoder.m_liveness.isFused(left)) {
            RELEASE_ASSERT((opcode == Equal || opcode == NotEqual) && tested->child(1)->isInt(0));
            fused = left;
            comparesBitsWithZero = true;
        } else
            kind = left->type().isFloat() ? Test::CompareNumbers : Test::CompareIntegers;
    }
    Test test = testOfEncodedOperands(encoder, kind, relation, fused, cursor);
    if (comparesBitsWithZero)
        cursor.take();
    return test;
}

Test Encoder::resolveTest(const Operation* user, LeafCursor& cursor)
{
    ASSERT(user->opcode() == Branch || user->opcode() == Select || user->opcode() == Check);

    const Operation* predicate = user->child(0);
    if (!m_liveness.isFused(predicate))
        return nonZeroTestOfEncodedValue(*this, predicate, cursor.take());
    return testOfFusedOperation(*this, predicate, cursor);
}

void Encoder::encodeTerminator(Block* block, const Step& step)
{
    ASSERT(step.kind == StepKind::Execute && step.operation == block->last());

    Opcode opcode = step.operation->opcode();
    if (step.operation->isUnconditionalJump()) {
        const Block* target = block->successor(0).block();
        if (!fallsThroughTo(block, target))
            jumpTo(target);
        return;
    }
    if (opcode == Branch) {
        encodeBranch(block, step);
        return;
    }
    if (opcode == Switch) {
        encodeSwitch(block, step);
        return;
    }
    if (opcode == Return) {
        encodeReturn(step);
        return;
    }

    RELEASE_ASSERT(opcode == Oops);
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    m_jit.oops();
}

static ALWAYS_INLINE void addPendingJumpsOnTest(Encoder& encoder, const Test& test, bool whenItHolds, const Block* target)
{
#if CPU(X86_64)
    if (test.kind != Test::CompareNumbers) {
        encoder.addPendingJump(encoder.jumpOnIntegers(test, whenItHolds), target);
        return;
    }
#endif
    encoder.addPendingJumps(encoder.branchOn(test, whenItHolds), target);
}

void Encoder::encodeBranch(Block* block, const Step& step)
{
    RELEASE_ASSERT(block->numSuccessors() == 2);
    const Edge& whenNotZero = block->successor(0);
    const Edge& whenZero = block->successor(1);
    RELEASE_ASSERT(whenNotZero.block() != whenZero.block());

    if (m_liveness.worksInMemory(step.operation)) {
        encodeBranchInMemory(block, step);
        return;
    }
    if (m_liveness.goesByOverflow(step.operation)) {
        encodeBranchOnOverflow(block, step);
        return;
    }

#if CPU(X86_64)
    if (std::optional<MacroAssembler::RelationalCondition> condition = tryEmitFlagsOfPredicate(step)) {
        if (fallsThroughTo(block, whenNotZero.block())) {
            addPendingJump(m_jit.branchOnFlags(MacroAssembler::invert(*condition)), whenZero.block());
            return;
        }
        if (fallsThroughTo(block, whenZero.block())) {
            addPendingJump(m_jit.branchOnFlags(*condition), whenNotZero.block());
            return;
        }

        unsigned successorOfBranch = m_order.successorOfConditionalJump(block);
        addPendingJump(m_jit.branchOnFlags(successorOfBranch ? MacroAssembler::invert(*condition) : *condition), block->successor(successorOfBranch).block());
        jumpTo(block->successor(1 - successorOfBranch).block());
        return;
    }
#endif

    LeafCursor cursor(step);
    const Operation* predicate = step.operation->child(0);
    bool predicateIsFused = m_liveness.isFused(predicate);
    bool testsTwice = false;
    if (predicateIsFused && (predicate->opcode() == BitAnd || predicate->opcode() == BitOr)) {
        const Operation* first = predicate->child(0);
        testsTwice = m_liveness.isFused(first) && (isComparison(first->opcode()) || first->opcode() == BitAnd);
    }
    Test test = predicateIsFused ? testOfFusedOperation(*this, testsTwice ? predicate->child(0) : predicate, cursor) : nonZeroTestOfEncodedValue(*this, predicate, cursor.take());
    if (testsTwice) {
        bool isOr = predicate->opcode() == BitOr;
        addPendingJumpsOnTest(*this, test, isOr, isOr ? whenNotZero.block() : whenZero.block());
        test = testOfFusedOperation(*this, predicate->child(1), cursor);
    }
    if (fallsThroughTo(block, whenNotZero.block())) {
        addPendingJumpsOnTest(*this, test, false, whenZero.block());
        return;
    }
    if (fallsThroughTo(block, whenZero.block())) {
        addPendingJumpsOnTest(*this, test, true, whenNotZero.block());
        return;
    }

    unsigned successorOfBranch = m_order.successorOfConditionalJump(block);
    addPendingJumpsOnTest(*this, test, !successorOfBranch, block->successor(successorOfBranch).block());
    jumpTo(block->successor(1 - successorOfBranch).block());
}

void Encoder::encodeBranchOnOverflow(Block* block, const Step& step)
{
    SumTestedByOverflow test = sumTestedByOverflowOf(m_liveness, step.operation);
    const Block* passes = block->successor(test.successorThatPasses).block();
    const Block* fails = block->successor(!test.successorThatPasses).block();

    GPRReg sum = InvalidGPRReg;
    if (test.branchReadsSum()) {
        RELEASE_ASSERT(step.sources[0].isRegister());
        sum = step.sources[0].gpr();
    }

    bool widensWhereItOverflowed = test.isWidenedWhereTestFails && test.mayFallBelowInt32;
    bool widensBelowLeastResult = test.isWidenedWhereTestFails && test.leastResult;
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    if (!step.other && !widensWhereItOverflowed && !widensBelowLeastResult) {
        if (test.leastResult) {
            addPendingJump(jumpOnOverflow(true), fails);
            addPendingJump(m_jit.branch32(MacroAssembler::LessThan, sum, MacroAssembler::TrustedImm32(*test.leastResult)), fails);
            if (!fallsThroughTo(block, passes))
                jumpTo(passes);
            return;
        }
        if (fallsThroughTo(block, passes)) {
            addPendingJump(jumpOnOverflow(true), fails);
            return;
        }
        if (fallsThroughTo(block, fails)) {
            addPendingJump(jumpOnOverflow(false), passes);
            return;
        }
        unsigned successorOfBranch = m_order.successorOfConditionalJump(block);
        addPendingJump(jumpOnOverflow(successorOfBranch != test.successorThatPasses), block->successor(successorOfBranch).block());
        jumpTo(block->successor(1 - successorOfBranch).block());
        return;
    }

    GPRReg operandTakenBack = InvalidGPRReg;
    Location otherOperand;
    int32_t otherConstant = 0;
    if (step.other) {
        const Step* stepOfSum = m_allocation.findExecuteOf(test.sum);
        unsigned index = step.other == test.left() ? 0 : 1;
        RELEASE_ASSERT(stepOfSum && stepOfSum->destination.isRegister() && stepOfSum->sources[index] == stepOfSum->destination && !test.isWidenedWhereTestFails);
        operandTakenBack = stepOfSum->destination.gpr();
        otherOperand = stepOfSum->sources[1 - index];
        RELEASE_ASSERT(otherOperand.isRegister() || (otherOperand.isImmediate() && !index));
        if (otherOperand.isImmediate())
            otherConstant = static_cast<int32_t>(test.right()->asInt());
    }
    bool storesSumAnew = test.isWidenedWhereTestFails && m_allocation.isStoredBehindDefinition(test.sum);
    MacroAssembler::Address spillOfSum = storesSumAnew ? addressOf(Location::spill(m_allocation.spillOf(test.sum))) : MacroAssembler::Address(InvalidGPRReg, 0);

    MacroAssembler::Jump overflowed = jumpOnOverflow(true);
    MacroAssembler::Jump liesBelowLeastResult;
    if (test.leastResult)
        liesBelowLeastResult = m_jit.branch32(MacroAssembler::LessThan, sum, MacroAssembler::TrustedImm32(*test.leastResult));
    if (!fallsThroughTo(block, passes))
        jumpTo(passes);

    bool subtracts = test.subtracts();
    MacroAssembler::Label* labelOfFails = labelOf(fails);
    auto goOnToFailingSuccessor = [=](CCallHelpers& jit) {
        AllowMacroScratchRegisterUsage allowScratchOnLatePath(jit);
        auto finish = [&](bool hasWidened) {
            if (operandTakenBack != InvalidGPRReg) {
                if (otherOperand.isImmediate()) {
                    if (subtracts)
                        jit.add32(MacroAssembler::TrustedImm32(otherConstant), operandTakenBack);
                    else
                        jit.sub32(MacroAssembler::TrustedImm32(otherConstant), operandTakenBack);
                } else if (subtracts)
                    jit.add32(otherOperand.gpr(), operandTakenBack);
                else
                    jit.sub32(otherOperand.gpr(), operandTakenBack);
            }
            if (hasWidened && storesSumAnew)
                jit.store64(sum, spillOfSum);
            jit.jump().linkTo(*labelOfFails, &jit);
        };

        overflowed.link(&jit);
        if (widensWhereItOverflowed) {
            jit.signExtend32To64(sum, sum);
            jit.xor64(MacroAssembler::TrustedImm64(static_cast<int64_t>(0xffffffff00000000ull)), sum);
        }
        finish(widensWhereItOverflowed);
        if (!liesBelowLeastResult.isSet())
            return;
        liesBelowLeastResult.link(&jit);
        if (widensBelowLeastResult)
            jit.signExtend32To64(sum, sum);
        finish(widensBelowLeastResult);
    };
    addLatePath(m_arena.create<LatePathOf<decltype(goOnToFailingSuccessor)>>(goOnToFailingSuccessor));
}

static GPRReg scratchOfEncodedSwitch(const Encoder& encoder, const Step& step)
{
    if (step.patchAssignment == UINT32_MAX)
        return InvalidGPRReg;
    RegisterMask scratch = encoder.m_allocation.patchAssignment(step.patchAssignment).gpScratch;
    if (!scratch)
        return InvalidGPRReg;
    return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(scratch))).gpr();
}

static MacroAssembler::Jump branchOnEncodedSwitchValue(CCallHelpers& jit, MacroAssembler::RelationalCondition condition, GPRReg value, Width width, int64_t constant)
{
    if (width == Width32)
        return jit.branch32(condition, value, MacroAssembler::TrustedImm32(static_cast<int32_t>(constant)));
    if (isRepresentableAs<int32_t>(constant))
        return jit.branch64(condition, value, MacroAssembler::TrustedImm32(static_cast<int32_t>(constant)));
    return jit.branch64(condition, value, MacroAssembler::TrustedImm64(constant));
}

static unsigned endOfDenseRunInEncodedSwitch(std::span<const int64_t> values, unsigned begin)
{
    unsigned end = begin + 1;
    for (; end < values.size(); ++end) {
        uint64_t distanceToNext = static_cast<uint64_t>(values[end]) - static_cast<uint64_t>(values[begin]);
        uint64_t numberOfCasesWithNext = end - begin + 1;
        if (distanceToNext >= maxEntriesOfJumpTablePerCase * numberOfCasesWithNext)
            break;
    }
    return end;
}

struct EncodedSwitchSegment {
    unsigned begin;
    unsigned end;
    bool goesThroughTable;
};

// Upper segments are emitted last, so only the very last segment may fall through to the next block.
static void encodeSwitchSegments(Encoder& encoder, Block* block, GPRReg value, Width width, std::span<const EncodedSwitchSegment> segments)
{
    std::span<const int64_t> values = block->last()->switchData().caseValues;
    if (segments.size() > 1) {
        size_t middle = segments.size() / 2;
        MacroAssembler::Jump isInUpperSegments = branchOnEncodedSwitchValue(encoder.m_jit, MacroAssembler::GreaterThanOrEqual, value, width, values[segments[middle].begin]);
        encodeSwitchSegments(encoder, block, value, width, segments.first(middle));
        isInUpperSegments.link(&encoder.m_jit);
        encodeSwitchSegments(encoder, block, value, width, segments.subspan(middle));
        return;
    }

    const EncodedSwitchSegment& segment = segments.front();
    if (!segment.goesThroughTable) {
        encoder.encodeBinarySearch(block, value, width, values.subspan(segment.begin, segment.end - segment.begin), segment.begin);
        return;
    }
    uint64_t numberOfEntries = static_cast<uint64_t>(values[segment.end - 1]) - static_cast<uint64_t>(values[segment.begin]) + 1;
    encoder.encodeSwitchThroughTable(block, value, values[segment.begin], numberOfEntries);
}

void Encoder::encodeSwitch(Block* block, const Step& step)
{
    const Operation* operation = step.operation;
    const Operation* child = operation->child(0);
    std::span<const int64_t> values = operation->switchData().caseValues;
    RELEASE_ASSERT(block->numSuccessors() == values.size() + 1);
    RELEASE_ASSERT(child->type().isInt() && !m_liveness.isFused(child) && step.sources[0].isRegister());

    GPRReg value = step.sources[0].gpr();
    Width width = widthOf(child->type());
    unsigned numberOfCases = static_cast<unsigned>(values.size());
    RELEASE_ASSERT(value != scratchGPR);
    for (unsigned index = 0; index < numberOfCases; ++index) {
        RELEASE_ASSERT(!index || values[index - 1] < values[index]);
        RELEASE_ASSERT(width == Width64 || isRepresentableAs<int32_t>(values[index]));
    }

    if (!numberOfCases) {
        const Block* otherwise = block->successor(0).block();
        if (!fallsThroughTo(block, otherwise))
            jumpTo(otherwise);
        return;
    }

    ArenaVector<EncodedSwitchSegment> segments;
    unsigned beginOfSearch = 0;
    if (scratchOfEncodedSwitch(*this, step) != InvalidGPRReg) {
        for (unsigned begin = 0; begin < numberOfCases;) {
            unsigned end = endOfDenseRunInEncodedSwitch(values, begin);
            if (end - begin < minCasesForJumpTable) {
                ++begin;
                continue;
            }
            if (beginOfSearch < begin)
                segments.append(m_arena, EncodedSwitchSegment { beginOfSearch, begin, false });
            segments.append(m_arena, EncodedSwitchSegment { begin, end, true });
            begin = end;
            beginOfSearch = end;
        }
    }
    if (beginOfSearch < numberOfCases)
        segments.append(m_arena, EncodedSwitchSegment { beginOfSearch, numberOfCases, false });

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    encodeSwitchSegments(*this, block, value, width, segments.span());
}

void Encoder::encodeSwitchThroughTable(Block* block, GPRReg value, int64_t firstValue, uint64_t numberOfEntries)
{
    const Operation* operation = block->last();
    std::span<const int64_t> values = operation->switchData().caseValues;
    const Block* otherwise = block->successor(block->numSuccessors() - 1).block();
    Width width = widthOf(operation->child(0)->type());
    GPRReg index = scratchOfEncodedSwitch(*this, m_allocation.step(m_allocation.stepsOf(block).terminator));
    RELEASE_ASSERT(index != InvalidGPRReg && index != value && index != scratchGPR && value != scratchGPR);
    RELEASE_ASSERT(numberOfEntries && numberOfEntries <= static_cast<uint64_t>(INT32_MAX));

    size_t firstCase = static_cast<size_t>(std::ranges::lower_bound(values, firstValue) - values.begin());
    RELEASE_ASSERT(firstCase < values.size() && values[firstCase] == firstValue);
    std::span<const Block*> targets = m_arena.createSpan<const Block*>(static_cast<size_t>(numberOfEntries));
    std::ranges::fill(targets, otherwise);
    for (size_t place = firstCase; place < values.size(); ++place) {
        uint64_t entry = static_cast<uint64_t>(values[place]) - static_cast<uint64_t>(firstValue);
        if (entry >= numberOfEntries)
            break;
        targets[entry] = block->successor(static_cast<unsigned>(place)).block();
    }

    uint32_t tableIndex = m_proc.m_jumpTables.size();
    m_proc.m_jumpTables.append(m_arena, JumpTableRecord { m_arena.createSpan<MacroAssembler::Label>(static_cast<size_t>(numberOfEntries)) });
    m_pendingJumpTables.append(m_arena, PendingJumpTable { tableIndex, targets });

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    MacroAssembler::TrustedImm32 endOfTable(static_cast<int32_t>(numberOfEntries));
    if (width == Width32) {
        m_jit.add32(MacroAssembler::TrustedImm32(static_cast<int32_t>(0u - static_cast<uint32_t>(firstValue))), value, index);
        addPendingJump(m_jit.branch32(MacroAssembler::AboveOrEqual, index, endOfTable), otherwise);
    } else {
        m_jit.add64(MacroAssembler::TrustedImm64(static_cast<int64_t>(uint64_t { 0 } - static_cast<uint64_t>(firstValue))), value, index);
        addPendingJump(m_jit.branch64(MacroAssembler::AboveOrEqual, index, endOfTable), otherwise);
    }

    GPRReg table = m_jit.scratchRegister();
    m_relocations.moveAddress(CodeSymbol { SymbolSpace::JumpTable, tableIndex }, table, PCRelativeReach::Far);
    m_jit.load32SignedExtendTo64(MacroAssembler::BaseIndex(table, index, MacroAssembler::TimesFour), index);
    m_jit.add64(index, table);
    m_jit.farJump(table, NoPtrTag);
}

struct EncodedSwitchSearch {
    Block* block;
    GPRReg value;
    Width width;
    std::span<const int64_t> values;
    unsigned firstSuccessor;
    bool isEmittedLast;
};

static void encodeSearchOfSwitchValues(Encoder& encoder, const EncodedSwitchSearch& search, unsigned begin, unsigned end, bool isAtLeastFirst, bool isBelowNext)
{
    constexpr unsigned maxCasesComparedLinearly = 3;

    CCallHelpers& jit = encoder.m_jit;
    Block* block = search.block;
    std::span<const int64_t> values = search.values;
    ASSERT(begin < end && end <= values.size());

    if (end - begin > maxCasesComparedLinearly) {
        unsigned middle = begin + (end - begin) / 2;
        MacroAssembler::Jump isInUpperHalf = branchOnEncodedSwitchValue(jit, MacroAssembler::GreaterThanOrEqual, search.value, search.width, values[middle]);
        encodeSearchOfSwitchValues(encoder, search, begin, middle, isAtLeastFirst, true);
        isInUpperHalf.link(&jit);
        encodeSearchOfSwitchValues(encoder, search, middle, end, true, isBelowNext);
        return;
    }

    const Block* otherwise = block->successor(block->numSuccessors() - 1).block();
    bool leafIsEmittedLast = search.isEmittedLast && end == values.size();
    bool mustMatchOneCase = isAtLeastFirst && isBelowNext && static_cast<uint64_t>(values[end]) - static_cast<uint64_t>(values[begin]) == end - begin;
    for (unsigned place = begin; place < end; ++place) {
        const Block* target = block->successor(search.firstSuccessor + place).block();
        bool isLastValue = place + 1 == end;
        if (isLastValue && mustMatchOneCase) {
            encoder.jumpTo(target);
            return;
        }
        if (isLastValue && leafIsEmittedLast && encoder.fallsThroughTo(block, target)) {
            encoder.addPendingJump(branchOnEncodedSwitchValue(jit, MacroAssembler::NotEqual, search.value, search.width, values[place]), otherwise);
            return;
        }
        encoder.addPendingJump(branchOnEncodedSwitchValue(jit, MacroAssembler::Equal, search.value, search.width, values[place]), target);
    }
    if (!leafIsEmittedLast || !encoder.fallsThroughTo(block, otherwise))
        encoder.jumpTo(otherwise);
}

void Encoder::encodeBinarySearch(Block* block, GPRReg value, Width width, std::span<const int64_t> values, unsigned firstSuccessor)
{
    RELEASE_ASSERT(!values.empty() && firstSuccessor + values.size() < block->numSuccessors());
    RELEASE_ASSERT(value != scratchGPR);

    bool isEmittedLast = firstSuccessor + values.size() + 1 == block->numSuccessors();
    EncodedSwitchSearch search { block, value, width, values, firstSuccessor, isEmittedLast };
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    encodeSearchOfSwitchValues(*this, search, 0, static_cast<unsigned>(values.size()), false, false);
}

void Encoder::encodeReturn(const Step& step)
{
    const Operation* operation = step.operation;
    if (operation->numChildren()) {
        Reg result = bankOf(operation->child(0)->type()) == Bank::GP ? Reg(GPRInfo::returnValueGPR) : Reg(FPRInfo::returnValueFPR);
        RELEASE_ASSERT(step.sources[0] == Location::reg(result));
    }

    if (!m_frameIsSetUp) {
        m_jit.ret();
        return;
    }

    if (m_teardownSharedByReturns.isSet() && m_order.of(m_block).isRare()) {
        m_jit.jump().linkTo(m_teardownSharedByReturns, &m_jit);
        return;
    }
    MacroAssembler::Label teardown = m_jit.label();
    unsigned offsetOfTeardown = m_jit.debugOffset();
    leaveFrameIfSetUp();
    m_jit.ret();
    if (!m_teardownSharedByReturns.isSet() && m_jit.debugOffset() - offsetOfTeardown >= minBytesOfTeardownSharedByReturns)
        m_teardownSharedByReturns = teardown;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
