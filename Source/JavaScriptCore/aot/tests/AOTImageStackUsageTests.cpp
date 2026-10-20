/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTImageTestSupport.h"
#include "AOTOutliner.h"
#include "AOTStubsBarriers.h"
#include <span>
#include <utility>

namespace JSC::AOT::ImageTests {

static constexpr uint32_t sizeOfPlainWord = sizeof(uint32_t);

using StackCalls = Vector<std::pair<uint32_t, bool>>;

namespace {

struct StackBody {
    uint32_t function;
    unsigned frameSize;
    uint32_t stackUsedAtCalls;
    bool isEnteredWithoutStackCheck;
    bool isProvenToFitOnStack;
    StackCalls calls;
};

struct StackVerdict {
    unsigned limitExceeded;
    unsigned cycles;
    FunctionID function;
    uint32_t count;
};

} // anonymous namespace

static StackCalls callTo(uint32_t callee)
{
    return { { callee, false } };
}

static StackCalls tailCallTo(uint32_t callee)
{
    return { { callee, true } };
}

static StackBody provenBody(uint32_t function, unsigned frameSize, uint32_t stackUsedAtCalls, StackCalls calls = { })
{
    return { function, frameSize, stackUsedAtCalls, false, true, WTF::move(calls) };
}

static StackBody uncheckedBody(uint32_t function, unsigned frameSize, uint32_t stackUsedAtCalls, StackCalls calls = { })
{
    return { function, frameSize, stackUsedAtCalls, true, false, WTF::move(calls) };
}

static CompiledCode sketchStackBody(const StackBody& body)
{
    CodeSketch sketch(body.function);
    if (body.frameSize)
        sketch.withFrame(body.frameSize);
    FrameDescription& frame = sketch.code().frame;
    frame.stackUsedAtCalls = body.stackUsedAtCalls;
    frame.isEnteredWithoutStackCheck = body.isEnteredWithoutStackCheck;
    frame.isProvenToFitOnStack = body.isProvenToFitOnStack;
    sketch.plain(body.function);
    for (auto [callee, isTailCall] : body.calls) {
        if (isTailCall)
            sketch.jumpToFunction(callee);
        else
            sketch.callFunction(callee);
    }
    if (body.calls.isEmpty() || !body.calls.last().second)
        sketch.end();
    return sketch.take();
}

static StackVerdict checkStackCode(Vector<CompiledCode>&& code, std::span<const EntryAlias> aliases, const TestOptions& options)
{
    BuildContext context(wireOptions(options));
    LinkedFunctions functions = createLinkedFunctions(WTF::move(code), context);
    CodeIndex index(functions.span());
    ResolvedCallees callees = resolveCallees(functions, index, aliases, context);
    checkUncheckedStackUsage(functions, callees, aliases, index, context);
    context.diagnostics().finish(nullptr);

    StackVerdict verdict { };
    AOT_TEST_CHECK(context.diagnostics().userDiagnostics().isEmpty());
    for (const InternalError& error : context.diagnostics().internalErrors()) {
        if (error.kind == InternalErrorKind::UncheckedStackTooDeep) {
            AOT_TEST_CHECK(error.limit == LimitID::UncheckedStackBytes);
            if (!verdict.limitExceeded++) {
                verdict.function = error.function;
                verdict.count = error.count;
            }
            continue;
        }
        AOT_TEST_CHECK(error.kind == InternalErrorKind::UncheckedStackCycle);
        if (!verdict.cycles++ && !verdict.limitExceeded)
            verdict.function = error.function;
    }
    return verdict;
}

static StackVerdict checkStack(Vector<StackBody>&& stackBodies, Vector<EntryAlias>&& aliases = { }, const TestOptions& options = { })
{
    Vector<CompiledCode> code;
    code.reserveInitialCapacity(stackBodies.size());
    for (const StackBody& body : stackBodies)
        code.append(sketchStackBody(body));
    return checkStackCode(WTF::move(code), aliases.span(), options);
}

static bool isClean(const StackVerdict& verdict)
{
    return !verdict.limitExceeded && !verdict.cycles;
}

static bool isTooDeep(const StackVerdict& verdict)
{
    return verdict.limitExceeded == 1 && !verdict.cycles;
}

static bool isCycleAt(const StackVerdict& verdict, uint32_t function)
{
    return !verdict.limitExceeded && verdict.cycles == 1 && verdict.function == FunctionID(function);
}

static StackVerdict checkChainEndingWith(uint32_t stackUsedAtCalls)
{
    return checkStack({ provenBody(1, 1024, 0, callTo(2)), uncheckedBody(2, 1024, 0, callTo(3)), uncheckedBody(3, 2000, stackUsedAtCalls) });
}

static void testAtLimit()
{
    static_assert(maxStackUsedWithoutCheck == 1040 + 1040 + 2016);
    AOT_TEST_CHECK(isClean(checkChainEndingWith(0)));
}

static void testOneByteOver()
{
    StackVerdict verdict = checkChainEndingWith(1);
    AOT_TEST_CHECK(isTooDeep(verdict));
    AOT_TEST_CHECK(verdict.function == FunctionID(1u));
    AOT_TEST_CHECK(verdict.count == maxStackUsedWithoutCheck + 1);
}

static StackVerdict checkWithLoweredLimit(uint64_t limit)
{
    return checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 64, 0) }, { }, TestOptions { .loweredLimit = LimitID::UncheckedStackBytes, .loweredLimitValue = limit });
}

static void testLoweredLimit()
{
    AOT_TEST_CHECK(isTooDeep(checkWithLoweredLimit(100)));
    AOT_TEST_CHECK(isClean(checkWithLoweredLimit(112)));
}

static void testDeepestCalleeCounts()
{
    StackCalls calls { { 2, false }, { 3, false } };
    AOT_TEST_CHECK(isTooDeep(checkStack({ provenBody(1, 16, 0, WTF::move(calls)), uncheckedBody(2, 16, 0), uncheckedBody(3, 4080, 0) })));
}

static StackVerdict checkFramelessCallee(uint32_t stackUsedAtCalls)
{
    return checkStack({ provenBody(1, 4064, 0, callTo(2)), uncheckedBody(2, 0, stackUsedAtCalls) });
}

static void testFramelessCalleeReturnAddress()
{
    AOT_TEST_CHECK(isClean(checkFramelessCallee(8)));
    AOT_TEST_CHECK(isTooDeep(checkFramelessCallee(9)));
}

static void testStubCallsIgnored()
{
    CodeSketch sketch(1);
    sketch.withFrame(16);
    sketch.code().frame.isProvenToFitOnStack = true;
    sketch.plain(1);
    for (unsigned i = 0; i < 10; ++i)
        sketch.callStub(writeBarrier());
    sketch.end();
    AOT_TEST_CHECK(isClean(checkStackCode(bodies(sketch.take()), { }, { })));
}

static StackVerdict checkGuestJumpingBack(Vector<EntryAlias>&& aliases)
{
    return checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 16, 0, callTo(3)), uncheckedBody(3, 16, 0, tailCallTo(2)) }, WTF::move(aliases));
}

static EntryAlias guestOfLastBody(AliasPlacement placement)
{
    return { .function = FunctionID(4u), .body = FunctionID(3u), .entry = { }, .placement = placement, .start = sizeOfPlainWord, .prefixSize = 0 };
}

static void testGuestJumpsBack()
{
    AOT_TEST_CHECK(isClean(checkGuestJumpingBack({ guestOfLastBody(AliasPlacement::AfterBody) })));
    AOT_TEST_CHECK(isCycleAt(checkGuestJumpingBack({ }), 2));
    AOT_TEST_CHECK(isCycleAt(checkGuestJumpingBack({ guestOfLastBody(AliasPlacement::BeforeBody) }), 2));
    AOT_TEST_CHECK(isCycleAt(checkGuestJumpingBack({ guestOfLastBody(AliasPlacement::AtPrimaryEntry) }), 2));
}

static constexpr uint32_t firstInRing = 2;
static constexpr uint32_t sizeOfRing = 40;

static Vector<StackBody> ringOfJumps(uint32_t memberWithFrame)
{
    auto member = [](uint32_t index) {
        return firstInRing + index % sizeOfRing;
    };
    Vector<StackBody> stackBodies { provenBody(1, 16, 0, callTo(firstInRing)) };
    for (uint32_t index = 0; index < sizeOfRing; ++index) {
        StackCalls jumps { { member(index + 7), true }, { member(index + 13), true }, { member(index + sizeOfRing - 1), true }, { member(index + 1), true } };
        stackBodies.append(uncheckedBody(member(index), member(index) == memberWithFrame ? 16 : 0, 0, WTF::move(jumps)));
    }
    return stackBodies;
}

static void testRingWithCrossEdges()
{
    AOT_TEST_CHECK(isClean(checkStack(ringOfJumps(0))));
    AOT_TEST_CHECK(isCycleAt(checkStack(ringOfJumps(firstInRing + sizeOfRing / 2)), firstInRing));
}

static void testSelfEdge()
{
    AOT_TEST_CHECK(isCycleAt(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 16, 0, callTo(2)) }), 2));
    AOT_TEST_CHECK(isCycleAt(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 16, 0, tailCallTo(2)) }), 2));
    AOT_TEST_CHECK(isCycleAt(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 0, 0, callTo(2)) }), 2));
    AOT_TEST_CHECK(isClean(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 0, 0, tailCallTo(2)) })));
}

std::span<const NativeTest> stackUsageTests()
{
    static constexpr NativeTest tests[] = {
        { "image/stack-usage/at-limit"_s, testAtLimit },
        { "image/stack-usage/one-byte-over"_s, testOneByteOver },
        { "image/stack-usage/lowered-limit"_s, testLoweredLimit },
        { "image/stack-usage/deepest-callee-counts"_s, testDeepestCalleeCounts },
        { "image/stack-usage/frameless-callee-return-address"_s, testFramelessCalleeReturnAddress },
        { "image/stack-usage/checked-callee-stops-walk"_s, [] { AOT_TEST_CHECK(isClean(checkStack({ provenBody(1, 16, 0, callTo(2)), { 2, 60000, 0, false, false, { } } }))); } },
        { "image/stack-usage/no-root"_s, [] { AOT_TEST_CHECK(isClean(checkStack({ uncheckedBody(1, 16, 0, callTo(2)), uncheckedBody(2, 16, 0, callTo(1)) }))); } },
        { "image/stack-usage/stub-calls-ignored"_s, testStubCallsIgnored },
        { "image/stack-usage/cycle-with-frames"_s, [] { AOT_TEST_CHECK(isCycleAt(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 16, 0, callTo(3)), uncheckedBody(3, 16, 0, callTo(2)) }), 2)); } },
        { "image/stack-usage/cycle-of-frameless-calls"_s, [] { AOT_TEST_CHECK(isCycleAt(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 0, 0, callTo(3)), uncheckedBody(3, 0, 0, callTo(2)) }), 2)); } },
        { "image/stack-usage/ring-of-frameless-jumps"_s, [] { AOT_TEST_CHECK(isClean(checkStack({ provenBody(1, 16, 0, callTo(2)), uncheckedBody(2, 0, 0, tailCallTo(3)), uncheckedBody(3, 0, 0, tailCallTo(2)) }))); } },
        { "image/stack-usage/ring-with-cross-edges"_s, testRingWithCrossEdges },
        { "image/stack-usage/self-edge"_s, testSelfEdge },
        { "image/stack-usage/guest-jumps-back"_s, testGuestJumpsBack },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
