/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTGrowthOfWork.h"
#include "AOTMachineTestSupport.h"
#include "AllowMacroScratchRegisterUsage.h"
#include <array>
#include <bit>
#include <cstring>
#include <wtf/DataLog.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

[[noreturn]] static void callsOnWaysTestFail(const TestProcedure& test, const char* file, int line, const char* condition)
{
    dataLogLn(test.printedProcedure(), test.printedLiveness(), test.printedAllocation());
    failNativeTest(file, line, condition);
}

#define AOT_CALLS_ON_WAYS_TEST_CHECK(test, ...) do { \
        if (!(__VA_ARGS__)) [[unlikely]] \
            callsOnWaysTestFail(test, __FILE__, __LINE__, #__VA_ARGS__); \
    } while (false)

static Reg callsOnWaysTestLowestOf(RegisterMask mask) { return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(mask))); }
static Reg callsOnWaysTestHighestOf(RegisterMask mask) { return Reg::fromIndex(static_cast<unsigned>(63 - std::countl_zero(mask))); }

static RegisterMask callsOnWaysTestGPRsThatNoCalleeSaves(TestProcedure& test)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    return file.allocatableIn(Bank::GP) & ~file.calleeSaved;
}

static bool callsOnWaysTestIsSavedByCallees(TestProcedure& test, Reg reg)
{
    return reg && (maskOf(reg) & registerFileFor(test.procedure().traits()).calleeSaved);
}

static TypedPointer callsOnWaysTestWordAt(TestProcedure& test, LValue base, unsigned index)
{
    return test.out().address(test.heaps().argumentBuffer, base, static_cast<ptrdiff_t>(index * sizeof(uint64_t)));
}

static unsigned callsOnWaysTestNumberOfSteps(TestProcedure& test, StepKind kind)
{
    unsigned result = 0;
    for (const Step& step : test.procedure().allocation().steps())
        result += step.kind == kind;
    return result;
}

static void callsOnWaysTestMakeNoCall(PatchOperation* patch)
{
    patch->effects = Effects::none();
    patch->effects.writesLocalState = true;
}

static void callsOnWaysTestPoison(CCallHelpers& jit, RegisterMask gprs, RegisterMask fprs)
{
    forEachRegister(gprs, [&](Reg reg) {
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(poison)), reg.gpr());
    });
    if (!fprs)
        return;
    GPRReg poisoned = callsOnWaysTestLowestOf(gprs).gpr();
    forEachRegister(fprs, [&](Reg reg) {
        jit.move64ToDouble(poisoned, reg.fpr());
    });
}

// Adds one to the word that `counter` points at, if there is one, and leaves poison in every register that no callee saves.
static PatchOperation* callsOnWaysTestAppendCall(TestProcedure& test, LValue counter)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    RegisterMask clobberedGPRs = file.allocatableIn(Bank::GP) & ~file.calleeSaved;
    RegisterMask clobberedFPRs = file.allocatableIn(Bank::FP) & ~file.calleeSaved;

    PatchOperation* call = test.out().patchpoint(Void);
    if (counter)
        call->appendSomeRegister(counter);
    call->clobberLate(clobberedGPRs | clobberedFPRs);
    call->setGenerator([clobberedGPRs, clobberedFPRs, counts = !!counter](CCallHelpers& jit, const PatchParameters& parameters) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (counts)
            jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::Address(parameters[0].gpr()));
        callsOnWaysTestPoison(jit, clobberedGPRs, clobberedFPRs);
    });
    return call;
}

// Hands its operand back in the return register and leaves poison in every other register that no callee saves.
static PatchOperation* callsOnWaysTestAppendCallThatReturns(TestProcedure& test, LValue operand)
{
    RegisterFile file = registerFileFor(test.procedure().traits());
    RegisterMask clobberedGPRs = file.allocatableIn(Bank::GP) & ~file.calleeSaved;
    RegisterMask clobberedFPRs = file.allocatableIn(Bank::FP) & ~file.calleeSaved;
    Reg result(GPRInfo::returnValueGPR);

    PatchOperation* call = test.out().patchpoint(Int64);
    call->appendSomeRegister(operand);
    call->resultConstraints = { ValueRep::reg(result) };
    call->clobberLate(clobberedGPRs | clobberedFPRs);
    call->setGenerator([clobberedGPRs, clobberedFPRs, result](CCallHelpers& jit, const PatchParameters& parameters) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(parameters[1].gpr(), result.gpr());
        callsOnWaysTestPoison(jit, clobberedGPRs & ~maskOf(result), clobberedFPRs);
    });
    return call;
}

// ---- R1

struct CallsOnWaysTestArms {
    explicit CallsOnWaysTestArms(TestProcedure& test)
        : left(test.out().newBlock())
        , right(test.out().newBlock())
    {
        Output& out = test.out();
        LValue flag = test.argumentGPR(0);
        forLeft = out.add(test.argumentGPR(1), out.constInt64(5));
        forRight = out.add(test.argumentGPR(2), out.constInt64(7));
        out.branch(out.testNonZero64(flag, 1), normalEdge(left), normalEdge(right));
    }

    LBasicBlock left;
    LBasicBlock right;
    LValue forLeft { nullptr };
    LValue forRight { nullptr };
};

// Each arm hands one of two values to its call and reads nothing behind it. In the order one arm stands behind the other, so the value of the later arm counts as live across the call of the earlier one.
// No way leads from that call to a read of it: it takes no register that the function would have to save, and is not stored.
static void valueThatLaterArmHandsToItsCallTakesNoSavedRegister()
{
    TestProcedure test;
    Output& out = test.out();
    CallsOnWaysTestArms arms(test);
    out.appendTo(arms.left);
    out.ret(callsOnWaysTestAppendCallThatReturns(test, arms.forLeft));
    out.appendTo(arms.right);
    out.ret(callsOnWaysTestAppendCallThatReturns(test, arms.forRight));
    compileOrFail(test);

    const Procedure& procedure = test.procedure();
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, procedure.liveness().of(arms.forLeft).has(ValueFlag::LivesAcrossCall) != procedure.liveness().of(arms.forRight).has(ValueFlag::LivesAcrossCall));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().usedCalleeSaves());
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().isSpilled(arms.forLeft) && !procedure.allocation().isSpilled(arms.forRight));

    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }, uint64_t { 20 }) == 15);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0 }, uint64_t { 10 }, uint64_t { 20 }) == 27);
}

// The same of two Phis. The call ahead of them sets up the frame at the entry, so no framed part sees to what the arms spill.
static void phiThatLaterArmHandsToItsCallIsNotStored()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock one = out.newBlock();
    LBasicBlock other = out.newBlock();
    LBasicBlock join = out.newBlock();
    LBasicBlock left = out.newBlock();
    LBasicBlock right = out.newBlock();
    LValue forLeft = out.phiIn(join, Int64);
    LValue forRight = out.phiIn(join, Int64);
    LValue flag = callsOnWaysTestAppendCallThatReturns(test, test.argumentGPR(0));
    out.branch(out.testNonZero64(flag, 2), normalEdge(one), normalEdge(other));
    out.appendTo(one);
    out.upsilon(out.add(flag, out.constInt64(5)), forLeft);
    out.upsilon(out.add(flag, out.constInt64(7)), forRight);
    out.jump(normalEdge(join));
    out.appendTo(other);
    out.upsilon(out.add(flag, out.constInt64(50)), forLeft);
    out.upsilon(out.add(flag, out.constInt64(70)), forRight);
    out.jump(normalEdge(join));
    out.appendTo(join);
    out.branch(out.testNonZero64(flag, 1), normalEdge(left), normalEdge(right));
    out.appendTo(left);
    out.ret(callsOnWaysTestAppendCallThatReturns(test, forLeft));
    out.appendTo(right);
    out.ret(callsOnWaysTestAppendCallThatReturns(test, forRight));
    compileOrFail(test);

    const Procedure& procedure = test.procedure();
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, procedure.liveness().of(forLeft).has(ValueFlag::LivesAcrossCall) != procedure.liveness().of(forRight).has(ValueFlag::LivesAcrossCall));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().hasFramedPart());
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().usedCalleeSaves());
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().isSpilled(forLeft) && !procedure.allocation().isSpilled(forRight));

    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }) == 8 && test.call<uint64_t>(uint64_t { 2 }) == 9);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }) == 51 && test.call<uint64_t>(uint64_t { 0 }) == 70);
}

// Each arm reads its value behind its call, so both are born in registers that calls keep and nothing is moved at the calls.
static void valueThatArmReadsBehindItsCallIsBornInSavedRegister()
{
    TestProcedure test;
    Output& out = test.out();
    CallsOnWaysTestArms arms(test);
    out.appendTo(arms.left);
    callsOnWaysTestAppendCall(test, nullptr);
    out.ret(arms.forLeft);
    out.appendTo(arms.right);
    callsOnWaysTestAppendCall(test, nullptr);
    out.ret(arms.forRight);
    compileOrFail(test);

    const Allocation& allocation = test.procedure().allocation();
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, callsOnWaysTestIsSavedByCallees(test, allocation.registerAtDefinitionOf(arms.forLeft)));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, callsOnWaysTestIsSavedByCallees(test, allocation.registerAtDefinitionOf(arms.forRight)));

    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }, uint64_t { 20 }) == 15);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 0 }, uint64_t { 10 }, uint64_t { 20 }) == 27);
}

// The call stands in the block of the definition, behind it.
static void valueThatLivesAcrossCallOfItsBlockIsBornInSavedRegister()
{
    TestProcedure test;
    Output& out = test.out();
    LValue kept = out.add(test.argumentGPR(0), out.constInt64(5));
    callsOnWaysTestAppendCall(test, nullptr);
    out.ret(kept);
    compileOrFail(test);

    AOT_CALLS_ON_WAYS_TEST_CHECK(test, callsOnWaysTestIsSavedByCallees(test, test.procedure().allocation().registerAtDefinitionOf(kept)));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 10 }) == 15);
}

// function pick(c, v) { if (c & 1) { count(v); return 0; } if (c & 2) { count(v); return 1; } return v; }
// Two parts call, each below a head of its own, and the way past both needs no frame. Below each head the call alone reads v.
static void valueThatCallAloneReadsBelowHeadOfFramedPartStaysWhereItLies()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock first = out.newBlock();
    LBasicBlock rest = out.newBlock();
    LBasicBlock second = out.newBlock();
    LBasicBlock last = out.newBlock();
    LValue choice = test.argumentGPR(0);
    LValue counter = test.argumentGPR(1);
    out.branch(out.testNonZero64(choice, 1), normalEdge(first), normalEdge(rest));

    out.appendTo(first);
    callsOnWaysTestAppendCall(test, counter);
    out.ret(out.constInt64(0));

    out.appendTo(rest);
    out.branch(out.testNonZero64(choice, 2), normalEdge(second), normalEdge(last));

    out.appendTo(second);
    callsOnWaysTestAppendCall(test, counter);
    out.ret(out.constInt64(1));

    out.appendTo(last);
    out.ret(counter);
    compileOrFail(test);

    const Procedure& procedure = test.procedure();
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, procedure.liveness().of(counter).has(ValueFlag::LivesAcrossCall));
    // On ARM64 the whole function has the frame.
    if (isX86_64())
        AOT_CALLS_ON_WAYS_TEST_CHECK(test, procedure.allocation().headsOfFramedPart().size() == 2 && !procedure.allocation().needsFrameAheadOfFramedPart());
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().usedCalleeSaves() && !procedure.allocation().isSpilled(counter));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !callsOnWaysTestNumberOfSteps(test, StepKind::Save) && !callsOnWaysTestNumberOfSteps(test, StepKind::Reload));

    uint64_t word = 0;
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !test.call<uint64_t>(uint64_t { 1 }, &word) && word == 1);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 2 }, &word) == 1 && word == 2);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t*>(uint64_t { 4 }, &word) == &word && word == 2);
}

// Two halves, each with an arm that calls and returns, and a join that reads the value of the half. Whichever half comes first in the order, its call stands ahead of the join of the other,
// which has two predecessors: nothing is passed on every path to it but blocks that stand ahead of both halves. No way leads there from the arm.
static void valueReadAtJoinFurtherOnIsNotCarriedAcrossCallOfArmThatReturns()
{
    TestProcedure test;
    Output& out = test.out();
    LValue flag = test.argumentGPR(0);
    LValue values[2] { out.add(test.argumentGPR(1), out.constInt64(5)), out.add(test.argumentGPR(2), out.constInt64(7)) };
    LBasicBlock halves[2] { out.newBlock(), out.newBlock() };
    out.branch(out.testNonZero64(flag, 1), normalEdge(halves[0]), normalEdge(halves[1]));
    for (unsigned half = 0; half < 2; ++half) {
        LBasicBlock calls = out.newBlock();
        LBasicBlock goesOn = out.newBlock();
        LBasicBlock left = out.newBlock();
        LBasicBlock right = out.newBlock();
        LBasicBlock join = out.newBlock();
        LValue phi = out.phiIn(join, Int64);

        out.appendTo(halves[half]);
        out.branch(out.testNonZero64(flag, 2), normalEdge(calls), normalEdge(goesOn));
        out.appendTo(calls);
        callsOnWaysTestAppendCall(test, nullptr);
        out.ret(out.constInt64(half + 1));
        out.appendTo(goesOn);
        out.branch(out.testNonZero64(flag, 4), normalEdge(left), normalEdge(right));
        out.appendTo(left);
        out.upsilon(out.constInt64(1), phi);
        out.jump(normalEdge(join));
        out.appendTo(right);
        out.upsilon(out.constInt64(2), phi);
        out.jump(normalEdge(join));
        out.appendTo(join);
        out.ret(out.add(values[half], phi));
    }
    compileOrFail(test);

    const Procedure& procedure = test.procedure();
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, procedure.liveness().of(values[0]).has(ValueFlag::LivesAcrossCall) || procedure.liveness().of(values[1]).has(ValueFlag::LivesAcrossCall));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, !procedure.allocation().usedCalleeSaves() && procedure.allocation().spills().empty());

    constexpr uint64_t first = 10;
    constexpr uint64_t second = 20;
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }, first, second) == 1 && test.call<uint64_t>(uint64_t { 2 }, first, second) == 2);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 5 }, first, second) == 16 && test.call<uint64_t>(uint64_t { 1 }, first, second) == 17);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 4 }, first, second) == 28 && test.call<uint64_t>(uint64_t { 0 }, first, second) == 29);
}

// The arm that calls leads on to the join, which reads the value.
static void valueReadBehindJoinOutlivesCallOfArmThatLeadsThere()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock calls = out.newBlock();
    LBasicBlock join = out.newBlock();
    LValue kept = out.add(test.argumentGPR(1), out.constInt64(5));
    out.branch(out.testNonZero64(test.argumentGPR(0), 1), normalEdge(calls), normalEdge(join));

    out.appendTo(calls);
    callsOnWaysTestAppendCall(test, nullptr);
    out.jump(normalEdge(join));

    out.appendTo(join);
    out.ret(kept);
    compileOrFail(test);

    const Procedure& procedure = test.procedure();
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, procedure.liveness().endOfWhatWaysLeadToFrom(calls) == procedure.order().endOf(join));
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 1 }, uint64_t { 10 }) == 15 && test.call<uint64_t>(uint64_t { 0 }, uint64_t { 10 }) == 15);
}

// Each turn reads the value and then calls. Nothing behind the call reads it but the next turn.
static void valueThatEveryTurnReadsAheadOfItsCallOutlivesCall()
{
    TestProcedure test;
    Output& out = test.out();
    LBasicBlock loop = out.newBlock();
    LBasicBlock behindLoop = out.newBlock();
    LValue kept = out.add(test.argumentGPR(1), out.constInt64(5));
    LValue turns = out.phiIn(loop, Int64);
    LValue sum = out.phiIn(loop, Int64);
    out.upsilon(test.argumentGPR(0), turns);
    out.upsilon(out.constInt64(0), sum);
    out.jump(normalEdge(loop));

    out.appendTo(loop);
    LValue nextSum = out.add(sum, kept);
    callsOnWaysTestAppendCall(test, nullptr);
    LValue nextTurns = out.sub(turns, out.constInt64(1));
    out.upsilon(nextTurns, turns);
    out.upsilon(nextSum, sum);
    out.branch(out.notZero64(nextTurns), normalEdge(loop), normalEdge(behindLoop));

    out.appendTo(behindLoop);
    out.ret(nextSum);
    compileOrFail(test);

    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.procedure().liveness().endOfWhatWaysLeadToFrom(loop) == noPosition);
    AOT_CALLS_ON_WAYS_TEST_CHECK(test, test.call<uint64_t>(uint64_t { 3 }, uint64_t { 10 }) == 45);
}

// As many arms as the Switch has cases, each of which calls with one value. Every head asks about that value, which every later arm reads.
static std::optional<uint64_t> callsOnWaysTestInstructionsForArmsThatCall(unsigned numberOfArms)
{
    TestProcedure test;
    Output& out = test.out();
    LValue selector = test.argumentGPR(0);
    LValue counter = test.argumentGPR(1);
    Vector<LBasicBlock> arms;
    Vector<SwitchCase> cases;
    for (unsigned number = 0; number < numberOfArms; ++number) {
        arms.append(out.newBlock());
        cases.append(SwitchCase { static_cast<int64_t>(number), normalEdge(arms.last()) });
    }
    LBasicBlock otherwise = out.newBlock();
    out.switchOn(selector, cases.span(), normalEdge(otherwise));
    out.appendTo(otherwise);
    out.ret(counter);
    for (unsigned number = 0; number < numberOfArms; ++number) {
        out.appendTo(arms[number]);
        callsOnWaysTestAppendCall(test, counter);
        out.ret(out.constInt64(number));
    }
    return instructionsOfCurrentThreadIn([&] {
        prepareOrFail(test);
    });
}

static void workGrowsAsArmsThatCallDo()
{
    constexpr unsigned numberOfArms = 5000;
    std::optional<uint64_t> small = callsOnWaysTestInstructionsForArmsThatCall(numberOfArms);
    std::optional<uint64_t> large = callsOnWaysTestInstructionsForArmsThatCall(4 * numberOfArms);
    AOT_TEST_CHECK(growsAsSizeDoes(small, large));
}

#undef AOT_CALLS_ON_WAYS_TEST_CHECK

} // namespace Machine

std::span<const NativeTest> machineCallsOnWaysTests()
{
    static constexpr NativeTest tests[] = {
        { "machine-calls-on-ways/value-that-a-later-arm-hands-to-its-call-takes-no-saved-register"_s, Machine::valueThatLaterArmHandsToItsCallTakesNoSavedRegister },
        { "machine-calls-on-ways/phi-that-a-later-arm-hands-to-its-call-is-not-stored"_s, Machine::phiThatLaterArmHandsToItsCallIsNotStored },
        { "machine-calls-on-ways/value-that-an-arm-reads-behind-its-call-is-born-in-a-saved-register"_s, Machine::valueThatArmReadsBehindItsCallIsBornInSavedRegister },
        { "machine-calls-on-ways/value-that-lives-across-a-call-of-its-block-is-born-in-a-saved-register"_s, Machine::valueThatLivesAcrossCallOfItsBlockIsBornInSavedRegister },
        { "machine-calls-on-ways/value-that-the-call-alone-reads-below-a-head-of-the-framed-part-stays-where-it-lies"_s, Machine::valueThatCallAloneReadsBelowHeadOfFramedPartStaysWhereItLies },
        { "machine-calls-on-ways/value-read-at-a-join-further-on-is-not-carried-across-the-call-of-an-arm-that-returns"_s, Machine::valueReadAtJoinFurtherOnIsNotCarriedAcrossCallOfArmThatReturns },
        { "machine-calls-on-ways/value-read-behind-a-join-outlives-the-call-of-the-arm-that-leads-there"_s, Machine::valueReadBehindJoinOutlivesCallOfArmThatLeadsThere },
        { "machine-calls-on-ways/value-that-every-turn-reads-ahead-of-its-call-outlives-the-call"_s, Machine::valueThatEveryTurnReadsAheadOfItsCallOutlivesCall },
        { "machine-calls-on-ways/work-grows-as-the-arms-that-call-do"_s, Machine::workGrowsAsArmsThatCallDo },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
