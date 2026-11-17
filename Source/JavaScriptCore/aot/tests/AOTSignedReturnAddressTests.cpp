/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"

#if ENABLE(RETURN_ADDRESS_SIGNING)
#include "AOTConvention.h"
#include "AOTImageTestSupport.h"
#include "AOTSavedCodeAddresses.h"
#include "AOTSavedFrame.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTStubTestSupport.h"
#include "AOTStubWords.h"
#include "ARM64LinkRegisterUse.h"
#include "JSCPtrTag.h"
#include <sys/auxv.h>
#include <sys/syscall.h>
#endif

namespace JSC { namespace AOT {

#if ENABLE(RETURN_ADDRESS_SIGNING)

using namespace ARM64Registers;
using Address = CCallHelpers::Address;
using Emit = void (*)(CCallHelpers&);

static constexpr uint32_t signWithStackPointer = 0xd503237f;
static constexpr uint32_t authenticateWithStackPointer = 0xd50323ff;
static constexpr uint32_t signWithX16 = 0xd503215f;
static constexpr uint32_t authenticateWithX16 = 0xd50321df;
static constexpr uint32_t strip = 0xd50320ff;
static constexpr uint32_t loadThroughLinkRegister = 0x394003df;
static constexpr uint32_t supervisorCall = 0xd4000001;

static bool processorSignsAddresses()
{
    constexpr unsigned long hwcapPACA = 1UL << 30;
    return getauxval(AT_HWCAP) & hwcapPACA;
}

static Vector<uint32_t> wordsIn(CCallHelpers& jit)
{
    return Vector<uint32_t>(unsafeMakeSpan(static_cast<const uint32_t*>(jit.m_assembler.buffer().readOnlyData()), jit.m_assembler.codeSize() / sizeof(uint32_t)));
}

static Vector<uint32_t> wordsForJIT(const Invocable<void(CCallHelpers&)> auto& emit)
{
    CCallHelpers jit;
    emit(jit);
    return wordsIn(jit);
}

static Vector<uint32_t> wordsForImage(const Invocable<void(CCallHelpers&)> auto& emit)
{
    ImageAssembler jit;
    RelocationRecorder relocations(jit, ImageTests::testAssemblerPolicy());
    emit(jit);
    return wordsIn(jit);
}

static ImageTests::ExecutableCopy codeForImage(const Invocable<void(CCallHelpers&)> auto& emit)
{
    ImageAssembler jit;
    RelocationRecorder relocations(jit, ImageTests::testAssemblerPolicy());
    emit(jit);
    std::expected<RelocatableCode, LinkFailure> code = relocations.finalize(LinkBuffer::Profile::Thunk, nullptr);
    AOT_TEST_CHECK(code.has_value() && code->relocations.isEmpty());
    return ImageTests::ExecutableCopy(code->bytes.span());
}

#define INSTRUCTION(...) [](CCallHelpers& jit) { jit.m_assembler.__VA_ARGS__; }

static void testWhatLeavesNoTraceIsEmittedAsItIs()
{
    static constexpr Emit instructions[] = {
        INSTRUCTION(adr(lr, 8)),
        INSTRUCTION(adrp(lr, 4 * KB)),
        INSTRUCTION(movz<64>(lr, 1)),
        INSTRUCTION(movk<64>(lr, 1, 16)),
        INSTRUCTION(add<64>(lr, lr, UInt12(4))),
        INSTRUCTION(sub<64>(lr, lr, UInt12(4))),
        INSTRUCTION(and_<64>(lr, lr, LogicalImmediate::create64(~3ull))),
        INSTRUCTION(movi<64>(lr, LogicalImmediate::create64(0xff))),
        INSTRUCTION(ubfx<64>(lr, lr, 0, 48)),
        INSTRUCTION(extr<64>(lr, lr, zr, 1)),
        INSTRUCTION(mov<64>(lr, zr)),
        INSTRUCTION(add<64>(lr, lr, zr, ARM64Assembler::UXTX, 0)),
        INSTRUCTION(adc<64>(lr, lr, zr)),
        INSTRUCTION(csel<64>(lr, lr, zr, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(rbit<64>(lr, lr)),
        INSTRUCTION(cmp<64>(lr, x9)),
        INSTRUCTION(cmp<64>(x9, lr)),
        INSTRUCTION(cmp<64>(lr, UInt12(1))),
        INSTRUCTION(cmp<64>(sp, lr)),
        INSTRUCTION(tst<64>(lr, x9)),
        INSTRUCTION(tst<64>(lr, LogicalImmediate::create64(3))),
        INSTRUCTION(ccmp<64>(lr, x9, 0, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(ccmp<64>(x9, UInt5(30), 14, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(cbz<64>(lr, 8)),
        INSTRUCTION(tbz(lr, 1, 8)),
        INSTRUCTION(b_cond(ARM64Assembler::ConditionAL, 30 * 4)),
        INSTRUCTION(br(lr)),
        INSTRUCTION(blr(lr)),
        INSTRUCTION(ret()),
        INSTRUCTION(brk(30)),
        INSTRUCTION(ldr<32>(x9, lr, 4u)),
        INSTRUCTION(ldr<32>(x9, lr, PostIndex(4))),
        INSTRUCTION(ldr<64>(x9, x10, lr)),
        INSTRUCTION(ldp<32>(x9, x10, lr, 4)),
        INSTRUCTION(str<64>(x9, lr, 8u)),
        INSTRUCTION(ldar<64>(x9, lr)),
        INSTRUCTION(ldr<64>(x9, x10, 30u * 8)),
        INSTRUCTION(ldr<64>(x9, x10, (30u << 6) * 8)),
        INSTRUCTION(ldur<64>(x9, x10, -32)),
        INSTRUCTION(stp<64>(x9, x10, x11, PairPreIndex(-32))),
        INSTRUCTION(add<64>(x9, x10, UInt12(30))),
        INSTRUCTION(add<64>(x9, x10, x11, ARM64Assembler::LSL, 30)),
        INSTRUCTION(ubfx<64>(x9, x10, 30, 1)),
        INSTRUCTION(movz<64>(x9, 30)),
        INSTRUCTION(ldr<64>(q30, x9, 8u)),
        INSTRUCTION(str<64>(q30, x9, x10)),
        INSTRUCTION(stp<64>(q29, q30, x9)),
        INSTRUCTION(ldr_literal<64>(q30, 8)),
        INSTRUCTION(ld1r<64>(q30, lr)),
        INSTRUCTION(fmov<64>(q30, x9)),
        INSTRUCTION(fmov<64>(x9, q30)),
        INSTRUCTION(fmov<64>(q30, q30)),
        INSTRUCTION(scvtf<64, 64>(q30, x9)),
        INSTRUCTION(fcvtzs<64, 64>(x9, q30)),
        INSTRUCTION(fadd<64>(q30, q30, q30)),
        INSTRUCTION(ins(q30, x9, SIMDLane::i64x2, 1)),
        INSTRUCTION(umov(x9, q30, SIMDLane::i64x2, 1)),
        INSTRUCTION(dupElement(q30, q30, SIMDLane::i64x2, 1)),
    };
    for (Emit emit : instructions)
        AOT_TEST_CHECK(wordsForImage(emit) == wordsForJIT(emit));
}

static void testWhatIsCopiedOutIsSigned()
{
    static constexpr Emit instructions[] = {
        INSTRUCTION(mov<64>(x9, lr)),
        INSTRUCTION(mov<32>(x9, lr)),
        INSTRUCTION(mov<64>(sp, lr)),
        INSTRUCTION(add<64>(x9, lr, UInt12(0))),
        INSTRUCTION(add<64, ARM64Assembler::S>(x9, lr, UInt12(0))),
        INSTRUCTION(and_<64>(x9, lr, LogicalImmediate::create64(~3ull))),
        INSTRUCTION(and_<64>(sp, lr, LogicalImmediate::create64(~15ull))),
        INSTRUCTION(ubfx<64>(x9, lr, 0, 48)),
        INSTRUCTION(bfi<64>(x9, lr, 0, 48)),
        INSTRUCTION(extr<64>(x9, lr, x10, 1)),
        INSTRUCTION(extr<64>(x9, x10, lr, 1)),
        INSTRUCTION(add<64>(x9, lr, x10)),
        INSTRUCTION(add<64>(x9, x10, lr)),
        INSTRUCTION(add<64>(x9, sp, lr)),
        INSTRUCTION(add<64>(sp, lr, x10, ARM64Assembler::UXTX, 0)),
        INSTRUCTION(adc<64>(x9, lr, zr)),
        INSTRUCTION(csel<64>(x9, lr, x10, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(csel<64>(x9, x10, lr, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(rbit<64>(x9, lr)),
        INSTRUCTION(lslv<64>(x9, lr, zr)),
        INSTRUCTION(udiv<64>(x9, x10, lr)),
        INSTRUCTION(madd<64>(x9, lr, x10, zr)),
        INSTRUCTION(madd<64>(x9, x10, lr, zr)),
        INSTRUCTION(madd<64>(x9, zr, zr, lr)),
        INSTRUCTION(fmov<64>(q0, lr)),
        INSTRUCTION(fmov_top(q0, lr)),
        INSTRUCTION(scvtf<64, 64>(q0, lr)),
        INSTRUCTION(ucvtf<64, 64>(q0, lr)),
        INSTRUCTION(ins(q0, lr, SIMDLane::i64x2, 1)),
        INSTRUCTION(dupGeneral(q0, lr, SIMDLane::i64x2)),
        INSTRUCTION(str<32>(lr, x9, 0u)),
        INSTRUCTION(strh(lr, x9, x10)),
        INSTRUCTION(stp<32>(x10, lr, x9)),
        INSTRUCTION(stp<64>(fp, lr, sp, PairPreIndex(-16))),
        INSTRUCTION(stp<64>(zr, lr, sp, PairPreIndex(-16))),
    };
    for (Emit emit : instructions)
        AOT_TEST_CHECK(wordsForImage(emit) == (Vector<uint32_t> { signWithStackPointer, wordsForJIT(emit)[0], strip }));
}

static void testWhatOverwritesIsAuthenticated()
{
    static constexpr Emit instructions[] = {
        INSTRUCTION(mov<64>(lr, x9)),
        INSTRUCTION(mov<64>(lr, sp)),
        INSTRUCTION(add<64>(lr, fp, UInt12(16))),
        INSTRUCTION(and_<64>(lr, x9, LogicalImmediate::create64(~3ull))),
        INSTRUCTION(ubfx<64>(lr, x9, 0, 48)),
        INSTRUCTION(bfi<64>(lr, x9, 0, 16)),
        INSTRUCTION(extr<64>(lr, lr, x9, 1)),
        INSTRUCTION(extr<64>(lr, x9, lr, 1)),
        INSTRUCTION(add<64>(lr, lr, x9)),
        INSTRUCTION(add<64>(lr, x9, lr)),
        INSTRUCTION(add<64>(lr, sp, lr)),
        INSTRUCTION(add<64>(lr, lr, x9, ARM64Assembler::SXTW, 2)),
        INSTRUCTION(adc<64>(lr, lr, x9)),
        INSTRUCTION(csel<64>(lr, lr, x9, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(csel<64>(lr, x9, lr, ARM64Assembler::ConditionEQ)),
        INSTRUCTION(rbit<64>(lr, x9)),
        INSTRUCTION(lslv<64>(lr, lr, x9)),
        INSTRUCTION(madd<64>(lr, x9, x10, lr)),
        INSTRUCTION(fmov<64>(lr, q0)),
        INSTRUCTION(fmov_top(lr, q0)),
        INSTRUCTION(fcvtzs<64, 64>(lr, q0)),
        INSTRUCTION(fjcvtzs(lr, q0)),
        INSTRUCTION(umov(lr, q0, SIMDLane::i64x2, 1)),
        INSTRUCTION(smov(lr, q0, SIMDLane::i8x16, 1)),
        INSTRUCTION(ldr<32>(lr, x9, 0u)),
        INSTRUCTION(ldrsw<64>(lr, x9, 0u)),
        INSTRUCTION(ldrb(lr, x9, x10)),
        INSTRUCTION(ldp<32>(x10, lr, x9)),
        INSTRUCTION(ldr_literal<64>(lr, 8)),
        INSTRUCTION(ldr<64>(lr, lr, 8u)),
        INSTRUCTION(ldr<64>(lr, x9, lr)),
        INSTRUCTION(ldp<64>(fp, lr, sp, PairPostIndex(16))),
        INSTRUCTION(ldp<64>(zr, lr, sp, PairPostIndex(16))),
    };
    for (Emit emit : instructions)
        AOT_TEST_CHECK(wordsForImage(emit) == (Vector<uint32_t> { wordsForJIT(emit)[0], authenticateWithStackPointer, loadThroughLinkRegister }));
}

static void testAtomicAccessIsSignedAndAuthenticated()
{
    static constexpr Emit instructions[] = {
        INSTRUCTION(ldar<64>(lr, x9)),
        INSTRUCTION(stlr<64>(lr, x9)),
        INSTRUCTION(ldxr<64>(lr, x9)),
        INSTRUCTION(stxr<64>(x10, lr, x9)),
        INSTRUCTION(stxr<64>(lr, x10, x9)),
        INSTRUCTION(swpal<64>(lr, x10, x9)),
        INSTRUCTION(swpal<64>(x10, lr, x9)),
        INSTRUCTION(casal<64>(lr, x10, x9)),
        INSTRUCTION(casal<64>(x10, lr, x9)),
        INSTRUCTION(ldaddal<64>(lr, x10, x9)),
    };
    for (Emit emit : instructions)
        AOT_TEST_CHECK(wordsForImage(emit) == (Vector<uint32_t> { signWithStackPointer, wordsForJIT(emit)[0], authenticateWithStackPointer, loadThroughLinkRegister }));
}

static void testPlainAddressIsNotStoredForAMoment()
{
    static constexpr std::pair<Emit, Emit> instructions[] = {
        { INSTRUCTION(str<64>(lr, sp, PreIndex(-16))), INSTRUCTION(str<64>(zr, sp, PreIndex(-16))) },
        { INSTRUCTION(stp<64>(lr, x9, x10)), INSTRUCTION(stp<64>(zr, x9, x10)) },
        { INSTRUCTION(stp<64>(x9, lr, x10)), INSTRUCTION(stp<64>(x9, zr, x10)) },
        { INSTRUCTION(stp<64>(lr, lr, x10)), INSTRUCTION(stp<64>(zr, zr, x10)) },
    };
    for (auto [asked, emittedFirst] : instructions)
        AOT_TEST_CHECK(wordsForImage(asked)[0] == wordsForJIT(emittedFirst)[0]);
}

#undef INSTRUCTION

static void testInstructionsThatTheAssemblerLacks()
{
    using ARM64LinkRegister::Use;
    static constexpr Use nothing { };
    static constexpr Use copiedOut { .isCopiedOut = true };
    static constexpr Use overwritten { .isOverwritten = true };
    static constexpr Use both { true, true };
    static constexpr std::pair<uint32_t, Use> instructions[] = {
        { 0xd53bd05e, overwritten }, // mrs x30, tpidr_el0
        { 0xd51bd05e, copiedOut }, // msr tpidr_el0, x30
        { 0xd58003c9, both }, // tchangef x9, x30
        { 0x918003c9, copiedOut }, // addg x9, x30, #0, #0
        { 0x9180013e, overwritten }, // addg x30, x9, #0, #0
        { 0x9a0923c9, copiedOut }, // addpt x9, x30, x9
        { 0xba0007c1, nothing }, // rmif x30, #0, #1
        { 0x3a000bcd, nothing }, // setf8 w30
        { 0xbac903df, nothing }, // cmpp x30, x9
        { 0x9e59fc1e, overwritten }, // fcvtzu x30, d0, #1
        { 0x9e43ffc0, copiedOut }, // ucvtf d0, x30, #1
        { 0x4cdf7fc0, nothing }, // ld1 { v0.2d }, [x30], #16
        { 0x4cc97fc0, overwritten }, // ld1 { v0.2d }, [x30], x9
        { 0x4cde7d20, copiedOut }, // ld1 { v0.2d }, [x9], x30
        { 0xc87f7949, both }, // ldxp x9, x30, [x10]
        { 0xd800001e, nothing }, // prfm #30, .
        { 0xf980013e, nothing }, // prfm #30, [x9]
        { 0xd940013e, overwritten }, // ldapur x30, [x9]
        { 0xd900013e, copiedOut }, // stlur x30, [x9]
        { 0xd95e0149, nothing }, // ldapur x9, [x10, #-32]
        { 0xd920093e, both }, // stg x30, [x9]
        { 0xd93e0949, nothing }, // stg x9, [x10, #-512]
        { 0x190a07c9, both }, // cpyfp [x9]!, [x10]!, x30!
        { 0xf820053e, overwritten }, // ldraa x30, [x9]
        { 0x05e03bc0, both }, // mov z0.d, x30
        { 0x0000001e, nothing }, // udf #30
    };
    for (auto [instruction, use] : instructions)
        AOT_TEST_CHECK(ARM64LinkRegister::useBy(instruction) == use);

    for (uint32_t instruction : { 0xdac10549, 0xdac11549, 0xdac143e9, 0xf3a0001f }) // pacib x9, x10; autib x9, x10; xpaci x9; autibsppc .
        AOT_TEST_CHECK(ARM64LinkRegister::signsAuthenticatesOrStrips(instruction));
}

static void testNothingElseSignsAuthenticatesOrStrips()
{
    for (int number = 0; number < 128; ++number) {
        bool isOnPointer = number == 7 || number == 8 || number == 10 || number == 12 || number == 14 || (number >= 24 && number <= 31);
        AOT_TEST_CHECK(wordsForImage([&](CCallHelpers& jit) { jit.m_assembler.hint(number); }).size() == !isOnPointer);
    }
    AOT_TEST_CHECK(wordsForImage([](CCallHelpers& jit) {
        jit.tagReturnAddress();
        jit.untagReturnAddress();
        jit.tagPtr(NoPtrTag, lr);
        jit.untagPtr(NoPtrTag, lr);
        jit.validateUntaggedPtr(lr);
        for (GPRReg pointer : { x9, lr }) {
            jit.tagPtr(x10, pointer);
            jit.untagPtr(x10, pointer);
        }
        jit.removePtrTag(x9);
    }).isEmpty());
}

static_assert(!std::is_constructible_v<ImageLinkBuffer, CCallHelpers&, LinkBuffer::Profile> && !std::is_constructible_v<RelocationRecorder, CCallHelpers&, const AssemblerPolicy&>);

static void testNoPolicySwitchesSigningOff()
{
    ImageAssembler jit;
    AOT_TEST_CHECK(jit.m_assembler.signsLinkRegister());
    jit.setPolicy(AssemblerPolicy::forJIT());
    AOT_TEST_CHECK(jit.m_assembler.signsLinkRegister());
}

static void testLoadBehindAuthenticationStaysUnlessJumpFollows()
{
    auto keepsLoad = [](Emit inBetween, Emit leave = [](CCallHelpers& jit) { jit.ret(); }) {
        return wordsForImage([&](CCallHelpers& jit) {
            jit.emitFunctionEpilogue();
            inBetween(jit);
            leave(jit);
        }).contains(loadThroughLinkRegister);
    };
    constexpr Emit nothing = [](CCallHelpers&) { };
    AOT_TEST_CHECK(!keepsLoad(nothing));
    AOT_TEST_CHECK(!keepsLoad(nothing, [](CCallHelpers& jit) { jit.farJump(lr, NoPtrTag); }));
    AOT_TEST_CHECK(keepsLoad(nothing, nothing));
    AOT_TEST_CHECK(keepsLoad(nothing, [](CCallHelpers& jit) { jit.farJump(x9, NoPtrTag); }));
    AOT_TEST_CHECK(keepsLoad(nothing, [](CCallHelpers& jit) { jit.m_assembler.ret(x9); }));
    AOT_TEST_CHECK(keepsLoad(nothing, [](CCallHelpers& jit) { jit.nearTailCall(); }));

    static constexpr Emit whatKeepsIt[] = {
        [](CCallHelpers& jit) { (void)jit.label(); },
        [](CCallHelpers& jit) { (void)jit.labelIgnoringWatchpoints(); },
        [](CCallHelpers& jit) { (void)jit.m_assembler.codeSize(); },
        [](CCallHelpers& jit) { (void)jit.debugOffset(); },
        [](CCallHelpers& jit) { (void)jit.m_assembler.buffer().isAligned(16); },
        [](CCallHelpers& jit) { (void)jit.labelAt(jit.m_assembler.buffer().placeToDescribe()); },
        [](CCallHelpers& jit) { jit.add64(x1, x0); },
        [](CCallHelpers& jit) { jit.load64(Address(x9), x9); },
        [](CCallHelpers& jit) { jit.store64(x9, Address(x9)); },
        [](CCallHelpers& jit) { jit.m_assembler.cbz<64>(x9, 8); },
        [](CCallHelpers& jit) { jit.m_assembler.buffer().putInt(0); },
        [](CCallHelpers& jit) { jit.move(lr, x9); },
        [](CCallHelpers& jit) { jit.pushPair(fp, lr); },
        [](CCallHelpers& jit) { jit.add64(CCallHelpers::TrustedImm32(4), lr); },
    };
    for (Emit emit : whatKeepsIt)
        AOT_TEST_CHECK(keepsLoad(emit));

    AOT_TEST_CHECK(wordsForImage([](CCallHelpers& jit) {
        jit.emitFunctionEpilogue();
        jit.ret();
        jit.ret();
    }).size() == 5);
}

static void testDescriptionDoesNotKeepLoad()
{
    static constexpr Emit waysOut[] = {
        [](CCallHelpers& jit) { jit.ret(); },
        [](CCallHelpers& jit) { jit.farJump(x9, NoPtrTag); },
    };
    for (Emit leave : waysOut) {
        ptrdiff_t bytesBehindPlace = 0;
        Vector<uint32_t> words = wordsForImage([&](CCallHelpers& jit) {
            jit.emitFunctionEpilogue();
            AssemblerBuffer::PlaceToDescribe place = jit.m_assembler.buffer().placeToDescribe();
            leave(jit);
            bytesBehindPlace = CCallHelpers::differenceBetween(jit.labelAt(place), jit.label());
        });
        AOT_TEST_CHECK(bytesBehindPlace == sizeof(uint32_t));
        AOT_TEST_CHECK(words == wordsForImage([&](CCallHelpers& jit) {
            jit.emitFunctionEpilogue();
            leave(jit);
        }));
    }
}

namespace {

struct Visit {
    void** word;
    const void* modifier;
    bool spoils;
    const void* returnAddress { nullptr };
    bool wasSigned { false };
    std::array<uint64_t, 3> behindStore { };
    std::array<uint64_t, 3> behindLoad { };
};

} // anonymous namespace

static constexpr GPRReg visitGPR = x19;
static constexpr GPRReg wordGPR = x20;
static constexpr GPRReg neighbourGPR = x9;
static constexpr std::array<GPRReg, 3> watchedGPRs { x16, x17, neighbourGPR };
static constexpr std::array<uint64_t, 3> watchedValues { 0x1616, 0x1717, 0x0909 };

static void visitSavedWord(Visit* visit, void** wordOnStack)
{
    void** word = visit->word ? visit->word : wordOnStack;
    void* address = removeReturnAddressSignature(*word);
    visit->returnAddress = address;
    visit->wasSigned = *word == JSC::signReturnAddress(address, visit->modifier ? visit->modifier : word + 1);
    if (!visit->spoils)
        return;
    constexpr uintptr_t bitOfSignature = 1ULL << 54;
    *word = *word != address ? address : std::bit_cast<void*>(std::bit_cast<uintptr_t>(address) ^ bitOfSignature);
}

static void emitVisit(CCallHelpers& jit)
{
    jit.move(visitGPR, x0);
    jit.move(wordGPR, x1);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(&visitSavedWord)), x2);
    jit.call(x2, NoPtrTag);
}

static ImageTests::ExecutableCopy codeThatVisits(int32_t distanceOfWordOnStack, const Invocable<void(CCallHelpers&)> auto& save, const Invocable<void(CCallHelpers&)> auto& restore)
{
    return codeForImage([&](CCallHelpers& jit) {
        auto watch = [&](size_t offset, const auto& emit) {
            for (size_t i = 0; i < watchedGPRs.size(); ++i)
                jit.move(CCallHelpers::TrustedImm64(watchedValues[i]), watchedGPRs[i]);
            emit(jit);
            for (size_t i = 0; i < watchedGPRs.size(); ++i)
                jit.m_assembler.str<64>(watchedGPRs[i], visitGPR, offset + i * sizeof(uint64_t));
        };
        jit.pushPair(visitGPR, wordGPR);
        jit.pushPair(fp, zr);
        jit.move(x0, visitGPR);
        if (distanceOfWordOnStack)
            jit.subPtr(sp, CCallHelpers::TrustedImm32(distanceOfWordOnStack), wordGPR);
        else
            jit.loadPtr(Address(visitGPR, OBJECT_OFFSETOF(Visit, word)), wordGPR);
        watch(OBJECT_OFFSETOF(Visit, behindStore), save);
        emitVisit(jit);
        watch(OBJECT_OFFSETOF(Visit, behindLoad), restore);
        jit.popPair(fp, zr);
        jit.popPair(visitGPR, wordGPR);
        jit.ret();
    });
}

static void checkThatSpoiledWordEndsProcess(const ImageTests::ExecutableCopy& code, void** word, const void* modifier = nullptr)
{
    if (!processorSignsAddresses())
        return;
    AOT_TEST_CHECK(ChildOutcome::Crashed == runInChild([&] {
        Visit visit { word, modifier, true };
        code.call<void>(0, &visit);
    }));
}

namespace {

// The base is x10 unless the way says otherwise. The neighbour of a pair is what neighbourGPR holds.
struct WayToReachWord {
    Emit store;
    Emit load;
    GPRReg lost { GPRReg::InvalidGPRReg };
    GPRReg alsoLost { GPRReg::InvalidGPRReg };
};

} // anonymous namespace

template<int offset, GPRReg base = x10>
static constexpr WayToReachWord atOffset()
{
    return {
        [](CCallHelpers& jit) { jit.m_assembler.sub<64>(base, wordGPR, UInt12(offset & 0xfff)); jit.m_assembler.sub<64>(base, base, UInt12(offset >> 12), 12); jit.m_assembler.str<64>(lr, base, static_cast<unsigned>(offset)); },
        [](CCallHelpers& jit) { jit.m_assembler.sub<64>(base, wordGPR, UInt12(offset & 0xfff)); jit.m_assembler.sub<64>(base, base, UInt12(offset >> 12), 12); jit.m_assembler.ldr<64>(lr, base, static_cast<unsigned>(offset)); },
        base,
    };
}

template<ARM64Assembler::ExtendType extend, int shift, int64_t index, int distance, GPRReg base = x10, GPRReg indexGPR = x11>
static constexpr WayToReachWord atIndex()
{
    static constexpr Emit prepare = [](CCallHelpers& jit) {
        jit.addPtr(CCallHelpers::TrustedImm32(-distance), wordGPR, base);
        jit.move(CCallHelpers::TrustedImm64(index), indexGPR);
    };
    return {
        [](CCallHelpers& jit) { prepare(jit); jit.m_assembler.str<64>(lr, base, indexGPR, extend, shift); },
        [](CCallHelpers& jit) { prepare(jit); jit.m_assembler.ldr<64>(lr, base, indexGPR, extend, shift); },
        base,
        indexGPR,
    };
}

// Traps unless the base is where the instruction has to leave it.
static void emitCheckOfBase(CCallHelpers& jit, int32_t distanceFromWord)
{
    jit.addPtr(CCallHelpers::TrustedImm32(distanceFromWord), wordGPR, x11);
    CCallHelpers::Jump isRight = jit.branchPtr(CCallHelpers::Equal, x10, x11);
    jit.breakpoint();
    isRight.link(&jit);
}

template<typename Index, int step>
static constexpr WayToReachWord withWriteBack()
{
    static constexpr int before = std::is_same_v<Index, PreIndex> ? -step : 0;
    return {
        [](CCallHelpers& jit) { jit.addPtr(CCallHelpers::TrustedImm32(before), wordGPR, x10); jit.m_assembler.str<64>(lr, x10, Index(step)); emitCheckOfBase(jit, before + step); },
        [](CCallHelpers& jit) { jit.addPtr(CCallHelpers::TrustedImm32(before), wordGPR, x10); jit.m_assembler.ldr<64>(lr, x10, Index(step)); emitCheckOfBase(jit, before + step); },
    };
}

template<typename Index, int step, GPRReg first, GPRReg second>
static constexpr WayToReachWord inPair()
{
    static constexpr int before = (std::is_same_v<Index, PairPostIndex> ? 0 : -step) - (first == lr ? 0 : 8);
    static constexpr int after = before + (std::is_same_v<Index, int> ? 0 : step);
    static constexpr GPRReg other = first == lr ? second : first;
    static constexpr int placeOfOther = first == lr ? 8 : -8;
    static constexpr Emit checkOther = [](CCallHelpers& jit) {
        jit.load64(Address(wordGPR, placeOfOther), x11);
        CCallHelpers::Jump isRight = jit.branch64(CCallHelpers::Equal, x11, other);
        jit.breakpoint();
        isRight.link(&jit);
    };
    return {
        [](CCallHelpers& jit) {
            jit.addPtr(CCallHelpers::TrustedImm32(before), wordGPR, x10);
            jit.m_assembler.stp<64>(first, second, x10, Index(step));
            checkOther(jit);
            emitCheckOfBase(jit, after);
        },
        [](CCallHelpers& jit) {
            jit.addPtr(CCallHelpers::TrustedImm32(before), wordGPR, x10);
            jit.store64(wordGPR, Address(wordGPR, placeOfOther));
            jit.m_assembler.ldp<64>(first, second, x10, Index(step));
            if (other != zr)
                checkOther(jit);
            emitCheckOfBase(jit, after);
        },
        other,
    };
}

static void checkEveryStoreWithEveryLoad(std::span<const WayToReachWord> ways, void** word, int32_t distanceOfWordOnStack)
{
    auto keeps = [](const WayToReachWord& way, const std::array<uint64_t, 3>& found) {
        for (size_t i = 0; i < watchedGPRs.size(); ++i) {
            if (watchedGPRs[i] != way.lost && watchedGPRs[i] != way.alsoLost && found[i] != watchedValues[i])
                return false;
        }
        return true;
    };
    for (const WayToReachWord& stored : ways) {
        for (const WayToReachWord& loaded : ways) {
            Visit visit { word, nullptr, false };
            codeThatVisits(distanceOfWordOnStack, stored.store, loaded.load).call<void>(0, &visit);
            AOT_TEST_CHECK(visit.returnAddress && visit.wasSigned);
            AOT_TEST_CHECK(keeps(stored, visit.behindStore) && keeps(loaded, visit.behindLoad));
        }
        checkThatSpoiledWordEndsProcess(codeThatVisits(distanceOfWordOnStack, stored.store, stored.load), word);
    }
}

static void testEveryStoreAgreesWithEveryLoad()
{
    static constexpr WayToReachWord ways[] = {
        atOffset<0>(),
        atOffset<8>(),
        atOffset<4080>(),
        atOffset<4088>(),
        atOffset<32760>(),
        atOffset<8, x16>(),
        atOffset<8, x17>(),
        atOffset<8, fp>(),
        {
            [](CCallHelpers& jit) { jit.addPtr(CCallHelpers::TrustedImm32(256), wordGPR, x10); jit.m_assembler.stur<64>(lr, x10, -256); },
            [](CCallHelpers& jit) { jit.addPtr(CCallHelpers::TrustedImm32(8), wordGPR, x10); jit.m_assembler.ldur<64>(lr, x10, -8); },
        },
        atIndex<ARM64Assembler::UXTX, 0, 24, 24>(),
        atIndex<ARM64Assembler::UXTX, 3, 5, 40>(),
        atIndex<ARM64Assembler::UXTW, 3, 0x7700000002, 16>(),
        atIndex<ARM64Assembler::SXTW, 3, 0xfffffffe, -16>(),
        atIndex<ARM64Assembler::UXTX, 0, 24, 24, x10, x17>(),
        atIndex<ARM64Assembler::UXTX, 0, 24, 24, x16, x17>(),
        atIndex<ARM64Assembler::UXTX, 0, 24, 24, x17, x16>(),
        {
            [](CCallHelpers& jit) { jit.m_assembler.str<64>(lr, wordGPR, zr); },
            [](CCallHelpers& jit) { jit.m_assembler.ldr<64>(lr, wordGPR, zr); },
        },
        withWriteBack<PreIndex, 16>(),
        withWriteBack<PreIndex, -16>(),
        withWriteBack<PreIndex, -8>(),
        withWriteBack<PostIndex, 8>(),
        withWriteBack<PostIndex, -256>(),
        inPair<int, 0, neighbourGPR, lr>(),
        inPair<int, 0, lr, neighbourGPR>(),
        inPair<int, -512, neighbourGPR, lr>(),
        inPair<int, 504, lr, neighbourGPR>(),
        inPair<int, 16, x16, lr>(),
        inPair<int, 16, lr, x17>(),
        inPair<PairPreIndex, -16, neighbourGPR, lr>(),
        inPair<PairPreIndex, 32, lr, neighbourGPR>(),
        inPair<PairPreIndex, -16, zr, lr>(),
        inPair<PairPostIndex, 16, neighbourGPR, lr>(),
        inPair<PairPostIndex, -512, lr, neighbourGPR>(),
        inPair<PairPostIndex, 16, zr, lr>(),
        {
            [](CCallHelpers& jit) { jit.m_assembler.stp<64>(lr, lr, wordGPR); },
            [](CCallHelpers& jit) { jit.m_assembler.ldnp<64>(lr, x10, wordGPR); },
        },
        {
            [](CCallHelpers& jit) { jit.m_assembler.stp<64>(lr, lr, wordGPR, -8); },
            [](CCallHelpers& jit) { jit.m_assembler.ldnp<64>(x10, lr, wordGPR, -8); },
        },
        {
            [](CCallHelpers& jit) { jit.subPtr(wordGPR, CCallHelpers::TrustedImm32(8), x10); HeldReturnAddress::inLinkRegister().storeInFrameRecord(jit, x10); },
            [](CCallHelpers& jit) { jit.subPtr(wordGPR, CCallHelpers::TrustedImm32(8), x10); HeldReturnAddress::loadLosingLinkRegister(jit, x10, x11); },
            x16,
            x17,
        },
    };
    std::array<void*, 4> words { };
    checkEveryStoreWithEveryLoad(ways, &words[1], 0);
    checkEveryStoreWithEveryLoad(std::span { ways }.first(2), &words[2], 0);
}

static void testEveryPushAgreesWithEveryPop()
{
    static constexpr WayToReachWord waysToUpperWord[] = {
        { [](CCallHelpers& jit) { jit.emitFunctionPrologue(); }, [](CCallHelpers& jit) { jit.emitFunctionEpilogueWithEmptyFrame(); } },
        { [](CCallHelpers& jit) { jit.emitFunctionPrologue(); }, [](CCallHelpers& jit) { jit.restoreReturnAddressBeforeReturn(Address(sp, 8)); jit.addPtr(CCallHelpers::TrustedImm32(16), sp); } },
        { emitPushLinkRegister, emitPopLinkRegister },
        { [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(16), sp); jit.storePtr(lr, Address(sp, 8)); }, [](CCallHelpers& jit) { jit.loadPair64(sp, x10, lr); jit.addPtr(CCallHelpers::TrustedImm32(16), sp); } },
        { [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(16), sp); jit.storePair64(fp, lr, sp); }, [](CCallHelpers& jit) { jit.loadPtr(Address(wordGPR), lr); jit.addPtr(CCallHelpers::TrustedImm32(16), sp); } },
        { [](CCallHelpers& jit) { jit.move(lr, x10); jit.pushPair(fp, x10); }, [](CCallHelpers& jit) { jit.popPair(x11, x10); jit.move(x10, lr); } },
        { [](CCallHelpers& jit) { jit.move64ToDouble(lr, q0); jit.moveDoubleTo64(q0, x10); jit.pushPair(fp, x10); }, [](CCallHelpers& jit) { jit.popPair(x11, x10); jit.move64ToDouble(x10, q0); jit.moveDoubleTo64(q0, lr); } },
        { [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(16), sp); jit.move(CCallHelpers::TrustedImm32(1), x10); jit.store64(lr, CCallHelpers::BaseIndex(sp, x10, CCallHelpers::TimesEight)); },
            [](CCallHelpers& jit) { jit.move(CCallHelpers::TrustedImm32(1), x10); jit.load64(CCallHelpers::BaseIndex(sp, x10, CCallHelpers::TimesEight), lr); jit.addPtr(CCallHelpers::TrustedImm32(16), sp); } },
    };
    checkEveryStoreWithEveryLoad(waysToUpperWord, nullptr, 8);

    static constexpr WayToReachWord waysToLowerWord[] = {
        { [](CCallHelpers& jit) { jit.pushToSave(lr); }, [](CCallHelpers& jit) { jit.popToRestore(lr); } },
        { [](CCallHelpers& jit) { jit.pushPair(lr, neighbourGPR); }, [](CCallHelpers& jit) { jit.popPair(lr, x10); } },
        { [](CCallHelpers& jit) { jit.pushPair(lr, lr); }, [](CCallHelpers& jit) { jit.popPair(lr, zr); } },
        { [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(16), sp); jit.storePtr(lr, Address(sp)); }, [](CCallHelpers& jit) { jit.loadPtr(Address(sp), lr); jit.addPtr(CCallHelpers::TrustedImm32(16), sp); } },
    };
    checkEveryStoreWithEveryLoad(waysToLowerWord, nullptr, 16);

    static constexpr WayToReachWord waysToRecordBelowSlots[] = {
        { [](CCallHelpers& jit) { jit.m_assembler.stp<64>(fp, lr, sp, PairPreIndex(-64)); }, [](CCallHelpers& jit) { jit.m_assembler.ldp<64>(x10, lr, sp, PairPostIndex(64)); } },
        { [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(48), sp); jit.emitFunctionPrologue(); }, [](CCallHelpers& jit) { jit.popPair(x10, lr); jit.addPtr(CCallHelpers::TrustedImm32(48), sp); } },
    };
    checkEveryStoreWithEveryLoad(waysToRecordBelowSlots, nullptr, 56);
}

static void testLinkRegisterIsPlainBehindSave()
{
    static constexpr Emit saves[] = {
        [](CCallHelpers& jit) { jit.pushPair(x9, lr); },
        [](CCallHelpers& jit) { jit.pushToSave(lr); },
        [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(16), sp); jit.move(lr, x9); },
        [](CCallHelpers& jit) { jit.subPtr(CCallHelpers::TrustedImm32(16), sp); jit.move(sp, x9); jit.storeRel64(lr, Address(x9)); },
    };
    for (Emit save : saves) {
        // A signature of zero bits would hide the mistake, and it depends on the stack pointer.
        for (int32_t depth = 0; depth < 8 * 16; depth += 16) {
            AOT_TEST_CHECK(7 == codeForImage([&](CCallHelpers& jit) {
                jit.subPtr(CCallHelpers::TrustedImm32(depth), sp);
                save(jit);
                jit.addPtr(CCallHelpers::TrustedImm32(depth + 16), sp);
                jit.move(CCallHelpers::TrustedImm32(7), x0);
                jit.ret();
            }).call<uint64_t>(0));
        }
    }
}

static void testWordThatIsCopiedElsewhereIsNotReturnedTo()
{
    if (!processorSignsAddresses())
        return;
    // Two places have the same signature once in 128 times.
    unsigned crashes = 0;
    constexpr unsigned numberOfPlaces = 4;
    for (unsigned place = 1; place <= numberOfPlaces; ++place) {
        ImageTests::ExecutableCopy code = codeForImage([&](CCallHelpers& jit) {
            jit.storePtr(lr, Address(x0));
            jit.loadPtr(Address(x0), x9);
            jit.storePtr(x9, Address(x0, place * sizeof(void*)));
            jit.loadPtr(Address(x0, place * sizeof(void*)), lr);
            jit.ret();
        });
        std::array<void*, numberOfPlaces + 1> words { };
        crashes += runInChild([&] { code.call<void>(0, words.data()); }) == ChildOutcome::Crashed;
    }
    AOT_TEST_CHECK(crashes);
}

static void testReturnToOldLinkRegister()
{
    using EmitPair = std::pair<Emit, Emit>;
    for (auto [push, popAndReturn] : { EmitPair { emitPushLinkRegister, emitPopLinkRegisterAndReturnToOldOne }, EmitPair { [](CCallHelpers& jit) { jit.emitFunctionPrologue(); }, emitPopFrameRecordAndReturnToOldLinkRegister } }) {
        // Nothing uses the popped address in front of the write.
        ImageTests::ExecutableCopy code = codeThatVisits(8, push, [&](CCallHelpers& jit) {
            CCallHelpers::Label site = jit.movePCRelativeAddress(lr, PCRelativeReach::Near);
            popAndReturn(jit);
            CCallHelpers::Label goesOn = jit.label();
            jit.addLinkTask([=](LinkBuffer& linkBuffer) { AOT_TEST_CHECK(linkBuffer.link(site, addressKindFor(PCRelativeReach::Near), goesOn)); });
            jit.move(CCallHelpers::TrustedImm32(0), x0);
            jit.move(CCallHelpers::TrustedImm32(__NR_exit_group), x8);
            jit.load8(Address(visitGPR, OBJECT_OFFSETOF(Visit, spoils)), x10);
            CCallHelpers::Jump returns = jit.branchTest32(CCallHelpers::Zero, x10);
            jit.m_assembler.buffer().putInt(supervisorCall);
            returns.link(&jit);
        });
        Visit visit { nullptr, nullptr, false };
        code.call<void>(0, &visit);
        AOT_TEST_CHECK(visit.wasSigned);
        checkThatSpoiledWordEndsProcess(code, nullptr);
    }
}

static void testAddressThatFailedIsNotSignedAgain()
{
    static constexpr Emit loads[] = {
        [](CCallHelpers& jit) { jit.emitFunctionEpilogue(); },
        [](CCallHelpers& jit) { jit.loadPtr(Address(fp, 8), lr); },
        [](CCallHelpers& jit) { HeldReturnAddress::loadLosingLinkRegister(jit, fp, x10); },
        [](CCallHelpers& jit) { jit.popPair(fp, x10); jit.move(x10, lr); },
        [](CCallHelpers& jit) { jit.addPtr(CCallHelpers::TrustedImm32(16), fp, x16); jit.loadPtr(Address(fp, 8), x17); jit.m_assembler.authenticateX17AndCompare(x10); },
        [](CCallHelpers& jit) { ReturnAddressToLookAt::load(jit, fp, x10); },
    };
    for (Emit load : loads) {
        checkThatSpoiledWordEndsProcess(codeThatVisits(8, [](CCallHelpers& jit) { jit.emitFunctionPrologue(); }, [&](CCallHelpers& jit) {
            load(jit);
            jit.move(CCallHelpers::TrustedImm32(0), x0);
            jit.move(CCallHelpers::TrustedImm32(__NR_exit_group), x8);
            jit.m_assembler.buffer().putInt(supervisorCall);
        }), nullptr);
    }
}

static void testAddressToLookAtIsReadAndNotJumpedTo()
{
    constexpr uint32_t wordBehindCall = 0xd503201f; // NOP
    auto codeThatCalls = [](Emit callee) {
        return codeForImage([&](CCallHelpers& jit) {
            jit.emitFunctionPrologue();
            CCallHelpers::Call call = jit.nearCall();
            jit.nop();
            jit.emitFunctionEpilogue();
            jit.ret();
            CCallHelpers::Label entry = jit.label();
            jit.addLinkTask([=](LinkBuffer& linkBuffer) { linkBuffer.link(call, linkBuffer.locationOf<NoPtrTag>(entry)); });
            jit.emitFunctionPrologue();
            callee(jit);
        });
    };
    AOT_TEST_CHECK(wordBehindCall == codeThatCalls([](CCallHelpers& jit) {
        jit.load32(ReturnAddressToLookAt::load(jit, fp, x9).at(), x0);
        jit.emitFunctionEpilogue();
        jit.ret();
    }).call<uint32_t>(0));

    ImageTests::ExecutableCopy jumps = codeThatCalls([](CCallHelpers& jit) {
        GPRReg address = ReturnAddressToLookAt::load(jit, fp, x9).gpr();
        jit.emitFunctionEpilogue();
        jit.farJump(address, NoPtrTag);
    });
    AOT_TEST_CHECK(ChildOutcome::Crashed == runInChild([&] { jumps.call<void>(0); }));
}

static void testResumeAddressComesBackInEveryWay()
{
    alignas(SavedFrame) std::array<uint8_t, sizeof(SavedFrame)> bytesOfFrame { };
    void** word = std::bit_cast<void**>(bytesOfFrame.data() + SavedFrame::offsetOfResumeAddressForTesting());
    const void* modifier = std::bit_cast<const SavedFrame*>(bytesOfFrame.data())->modifierOfResumeAddress();
    constexpr GPRReg frame = x10;
    auto findFrame = [](CCallHelpers& jit) { jit.subPtr(wordGPR, CCallHelpers::TrustedImm32(SavedFrame::offsetOfResumeAddressForTesting()), frame); };

    static constexpr Emit jumps[] = {
        [](CCallHelpers& jit) { jit.farJump(emitLoadResumeAddressLosingLinkRegister(jit, frame, x11), NoPtrTag); },
        [](CCallHelpers& jit) { emitLoadResumeAddressAsStored(jit, frame, x11); emitJumpToResumeAddress(jit, x11, frame); },
    };
    for (std::optional<PCRelativeReach> reachOfSignedAddress : { std::optional<PCRelativeReach>(), std::optional(PCRelativeReach::Near) }) {
        for (Emit jump : jumps) {
            CCallHelpers::Label site;
            ImageTests::ExecutableCopy code = codeThatVisits(0, [&](CCallHelpers& jit) {
                jit.emitFunctionPrologue();
                findFrame(jit);
                if (reachOfSignedAddress) {
                    emitModifierOfResumeAddress(jit, frame);
                    site = jit.moveSignedPCRelativeAddress(*reachOfSignedAddress);
                    emitStoreResumeAddressThatCallerHasSigned(jit, x17, frame);
                    return;
                }
                site = jit.movePCRelativeAddress(lr, PCRelativeReach::Near);
                emitStoreLinkRegisterAsResumeAddress(jit, frame);
            }, [&](CCallHelpers& jit) {
                findFrame(jit);
                jump(jit);
                CCallHelpers::Label resumes = jit.label();
                jit.addLinkTask([=](LinkBuffer& linkBuffer) { AOT_TEST_CHECK(linkBuffer.link(site, addressKindFor(PCRelativeReach::Near), resumes)); });
                jit.emitFunctionEpilogue();
            });
            Visit visit { word, modifier, false };
            code.call<void>(0, &visit);
            AOT_TEST_CHECK(visit.wasSigned);
            checkThatSpoiledWordEndsProcess(code, word, modifier);
        }
    }
    AOT_TEST_CHECK(wordsForImage([](CCallHelpers& jit) { jit.moveSignedPCRelativeAddress(PCRelativeReach::Far); }) == [] {
        Vector<uint32_t> words = wordsForImage([](CCallHelpers& jit) { jit.movePCRelativeAddress(x17, PCRelativeReach::Far); });
        words.append(signWithX16);
        return words;
    }());
}

static void testWordOfInstanceHoldsSignedAddress()
{
    constexpr size_t indexOfWord = 600;
    constexpr CodeSymbol symbol { SymbolSpace::InstanceWord, 0 };
    ImageAssembler jit;
    RelocationRecorder relocations(jit, ImageTests::testAssemblerPolicy());
    relocations.storeInstanceWord(symbol, InstanceWordWidth::Word64, lr, x0);
    jit.move(zr, lr);
    relocations.loadInstanceWord(symbol, InstanceWordWidth::Word64, x0, lr);
    jit.ret();
    std::expected<RelocatableCode, LinkFailure> code = relocations.finalize(LinkBuffer::Profile::Thunk, nullptr);
    AOT_TEST_CHECK(code.has_value() && code->relocations.size() == 2);
    for (const Relocation& relocation : code->relocations)
        AOT_TEST_CHECK(applyRelocation(code->bytes.mutableSpan(), 0, relocation, indexOfWord * sizeof(void*)));

    std::array<void*, indexOfWord + 1> instance { };
    ImageTests::ExecutableCopy(code->bytes.span()).call<void>(0, instance.data());
    void* address = removeReturnAddressSignature(instance.back());
    AOT_TEST_CHECK(address && instance.back() == JSC::signReturnAddress(address, &instance.back() + 1));
}

static void testNoPlainAddressIsLeftInX17()
{
    static constexpr Emit uses[] = {
        [](CCallHelpers& jit) { jit.m_assembler.authenticateX17IntoLinkRegister(); },
        [](CCallHelpers& jit) { jit.m_assembler.authenticateX17AndCompare(zr); },
        [](CCallHelpers& jit) { jit.m_assembler.authenticateX17ToLookAt(x9); },
    };
    for (Emit use : uses) {
        AOT_TEST_CHECK(codeForImage([&](CCallHelpers& jit) {
            jit.m_assembler.signLinkRegisterInX17();
            use(jit);
            jit.compare64(CCallHelpers::NotEqual, lr, x17, x0);
            jit.ret();
        }).call<uint64_t>(0));
    }
}

static void testWhatOnlyLooksTakesSignatureOff()
{
    StubWords words { };
    void* address = &words;
    words.returnAddresses[static_cast<unsigned>(StubFrameKind::Adapter)] = address;
    for (uintptr_t modifier = 0; modifier < 8; ++modifier) {
        void* signedAddress = JSC::signReturnAddress(address, std::bit_cast<const void*>(modifier));
        AOT_TEST_CHECK(removeCodePtrTag(signedAddress) == address && removeCodePtrTag<uintptr_t>(signedAddress) == std::bit_cast<uintptr_t>(address));
        AOT_TEST_CHECK(words.isReturnAddress(StubFrameKind::Adapter, std::bit_cast<uint64_t>(signedAddress)));
    }
}

static constexpr ASCIILiteral programWhoseAddressesAreSpoiled = R"IR(# LIBRARY: own
layout L0 name="Holder" fields=[{key="x" declared={type=anything} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
variable g0 name="yieldsToLoop" module=m0 declared={type=f12 flags=[IsFromCompiler]} initialization=Function function=f12 flags=[IsImmutable] place=1
variable g1 name="rejects" module=m0 declared={type=f14 flags=[IsFromCompiler]} initialization=Function function=f14 flags=[IsImmutable] place=2
variable g2 name="reads" module=m0 declared={type=f18 flags=[IsFromCompiler]} initialization=Function function=f18 flags=[IsImmutable] place=3
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="register" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = NewClosure function=f6
    v8 = NewClosure function=f7
    v9 = NewClosure function=f8
    v10 = NewClosure function=f9
    v11 = NewClosure function=f10
    v12 = NewClosure function=f11
    v13 = NewClosure function=f13
    v14 = GetModuleVariable variable=g0
    v15 = NewClosure function=f15
    v16 = NewClosure function=f16
    v17 = NewClosure function=f17
    v18 = NewClosure function=f19
    v19 = NewClosure function=f20
    v20 = NewArray v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19 contents=anything
    v21 = Call v1, -, v20
    Return
}
function f1 name="calls" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0, -, v1
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="print" [isFreeName]
    v5 = String string="caller goes on"
    v6 = Call v4, -, v5
    Return
}
function f2 name="returns" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="aotSpoilReturnAddress" [isFreeName]
    v2 = PropGet v0 key="depth" [isFreeName]
    v3 = PropGet v0 key="print" [isFreeName]
    v4 = Call v1, -, v2
    v5 = String string="spoiled"
    v6 = Call v3, -, v5, v4
    Return
}
function f3 name="catches" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1() handler=b2():
    v2 = Call v0, -, v1
    Jump -> next b3()
  b2() role=CatchHandler:
    v3 = CatchValue
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="print" [isFreeName]
    v6 = String string="in handler"
    v7 = Call v5, -, v6
    Jump -> next b3()
  b3():
    Return
}
function f4 name="throws" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="aotSpoilReturnAddress" [isFreeName]
    v2 = PropGet v0 key="depth" [isFreeName]
    v3 = PropGet v0 key="print" [isFreeName]
    v4 = Call v1, -, v2
    v5 = String string="spoiled"
    v6 = Call v3, -, v5, v4
    Throw v5
}
function f5 name="leaves" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="aotSpoilReturnAddress" [isFreeName]
    v3 = PropGet v1 key="depth" [isFreeName]
    v4 = PropGet v1 key="print" [isFreeName]
    v5 = Call v2, -, v3
    v6 = String string="spoiled"
    v7 = Call v4, -, v6, v5
    v8 = Call v0
    ReturnCall v8
}
function f6 name="isEntered" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = String string="entered"
    v3 = Call v1, -, v2
    Return
}
function f7 name="waits" flags=[IsAsync] parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Await v0
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="print" [isFreeName]
    v4 = String string="resumed"
    v5 = Call v3, -, v4
    Return
}
function f8 name="yields" flags=[IsGenerator, HasThis, TakesCallee] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything, v1: function):
    Jump -> next b1()
  b1():
    InitialYield
    v3 = Integer integer=1
    v4 = Yield v3
    v5 = ResultAt v4
    v6 = ResultAt v4 index=1
    Switch v6 cases=[1, 2] -> case b2(), case b3(), otherwise b4()
  b2():
    Throw v5
  b3():
    Return v5
  b4():
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = String string="resumed"
    v10 = Call v8, -, v9
    Return v10
}
function f9 name="steps" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0
    v3 = IteratorOpen v2
    Jump -> next b1()
  b1():
    LoopHint
    v5 = IteratorResume v3
    v6 = ResultAt v5
    Branch v6 -> taken b2(), notTaken b3()
  b2():
    Return
  b3():
    Jump -> next b4()
  b4() handler=b5():
    v7 = Call v1
    Jump -> next b1()
  b5() role=IterationCleanup subject=v3:
    v8 = CatchException
    IteratorClose v3 completion=Throw
    Rethrow v8
}
function f10 name="stays" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="aotSpoilReturnAddress" [isFreeName]
    v3 = PropGet v1 key="depth" [isFreeName]
    v4 = PropGet v1 key="print" [isFreeName]
    v5 = Call v2, -, v3
    v6 = String string="spoiled"
    v7 = Call v4, -, v6, v5
    v8 = Call v0
    Return
}
function f11 name="catchesRejection" flags=[IsAsync] parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1() handler=b2():
    v1 = Await v0
    Jump -> next b3()
  b2() role=CatchHandler:
    v2 = CatchValue
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="print" [isFreeName]
    v5 = String string="resumed"
    v6 = Call v4, -, v5
    Jump -> next b3()
  b3():
    Return
}
function f12 name="yieldsToLoop" flags=[IsGenerator, HasThis, TakesCallee] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything, v1: function):
    Jump -> next b1()
  b1():
    InitialYield
    v3 = Integer integer=1
    v4 = Yield v3
    v5 = ResultAt v4
    v6 = ResultAt v4 index=1
    Switch v6 cases=[1, 2] -> case b2(), case b3(), otherwise b4()
  b2():
    Throw v5
  b3():
    Return v5
  b4():
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = String string="resumed"
    v10 = Call v8, -, v9
    Return v10
}
function f13 name="breaks" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = GetModuleVariable variable=g0
    v2 = Call v1
    v3 = IteratorOpen v2
    Jump -> next b1()
  b1():
    LoopHint
    v5 = IteratorResume v3
    v6 = ResultAt v5
    Branch v6 -> taken b5(), notTaken b2()
  b2():
    Jump -> next b3()
  b3() handler=b6():
    v7 = Call v0
    Branch v7 -> taken b4(), notTaken b1()
  b4():
    IteratorClose v3
    Jump -> next b5()
  b5():
    Return
  b6() role=IterationCleanup subject=v3:
    v9 = CatchException
    IteratorClose v3 completion=Throw
    Rethrow v9
}
function f14 name="rejects" flags=[IsAsync] parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Call v0
    Return
}
function f15 name="awaitsCall" flags=[IsAsync] parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1() handler=b2():
    v1 = GetModuleVariable variable=g1
    v2 = Call v1, -, v0
    v3 = Await v2
    Jump -> next b3()
  b2() role=CatchHandler:
    v4 = CatchValue
    v5 = Builtin builtin=GlobalThis
    v6 = PropGet v5 key="print" [isFreeName]
    v7 = String string="in handler"
    v8 = Call v6, -, v7
    Jump -> next b3()
  b3():
    Return
}
function f16 name="cleansUp" parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1() handler=b2():
    Throw v0
  b2() role=CleanupHandler:
    v1 = CatchValue
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="print" [isFreeName]
    v4 = String string="cleaned up"
    v5 = Call v3, -, v4
    Throw v1
}
function f17 name="counts" flags=[HasRestParameter] parameters=[{type=Array flags=[IsFromCompiler, OmitsNull, OmitsUndefined]}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v0 key="length"
    v4 = Call v2, -, v3
    Return
}
function f18 name="reads" flags=[IsChecked] parameters=[{type=undefined|null|L0 flags=[OmitsNull, OmitsUndefined]}] results=[{type=anything}] capabilities=[MayChangeSignature, HasOrdinaryFrame, IsNeverCalledByHost] {
    v1 = PropGet v0 key="x"
    Return v1
}
function f19 name="readsNothing" flags=[IsChecked] parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1() handler=b5():
    v1 = GetModuleVariable variable=g2
    v2 = ToBoolean v0
    Branch v2 -> taken b2(), notTaken b3()
  b2() handler=b5():
    v3 = Undefined
    Jump -> next b4(v3)
  b3() handler=b5():
    v4 = NewObject -, v0 layout=L0 fields=[0]
    v5 = FinishObject v4
    Jump -> next b4(v5)
  b4(v6) handler=b5():
    v7 = Call v1, -, v6
    Return v7
  b5() role=CatchHandler:
    v8 = CatchValue
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = String string="in handler"
    v12 = Call v10, -, v11
    Return
}
function f20 name="throwsAtOnce" flags=[IsGenerator, HasThis, TakesCallee] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything, v1: function):
    InitialYield v0
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="Error" [isFreeName]
    v5 = Convert v4 declared={type=builtin(Error) flags=[IsExpected]}
    v6 = String string="thrown"
    v7 = Construct v5, -, v6 calleeName="Error"
    Throw v7
}
)IR"_s;

namespace {

class ProgramWhoseAddressesAreSpoiled {
public:
    explicit ProgramWhoseAddressesAreSpoiled(ASCIILiteral way)
        : m_compiled(compile(way))
        , m_image(".image"_s, m_compiled->section(Wire::ResultSection::Image))
    {
    }

    ChildProcessResult run(const String& statements, int depth = std::numeric_limits<int32_t>::max()) const
    {
        Vector<String> arguments {
            makeString("--aotImagePath="_s, m_image.path()),
            String("-e"_s),
            makeString("globalThis.depth = "_s, depth, "; globalThis.register = ([calls, returns, catches, throws, leaves, isEntered, waits, yields, steps, stays, catchesRejection, breaks, yieldsToLoop, awaitsCall, cleansUp, counts, readsNothing, throwsAtOnce]) => Promise.resolve().then(() => {"_s, statements, "; print('done'); });"_s,
                "runCompiledEntryPoint("_s, m_compiled->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ");"_s),
        };
        return ChildProcessLauncher(shellPath(), { }).run(arguments.span());
    }

    void checkOutputWithNothingSpoiled(const String& statements, ASCIILiteral expected) const
    {
        ChildProcessResult result = run(statements);
        checkThatChildSucceeded(__FILE__, __LINE__, statements, result, EXIT_SUCCESS);
        checkPrintedLines(__FILE__, __LINE__, statements, result.output, expected);
    }

    // No run with a spoiled frame record gets to its end. Returns how many of them end between the two lines.
    unsigned spoilEachFrameRecord(ASCIILiteral statements, ASCIILiteral lastLine, ASCIILiteral lineThatWouldFollow) const
    {
        constexpr int moreRecordsThanThereAre = 16;
        unsigned count = 0;
        for (int depth = 0; depth < moreRecordsThanThereAre; ++depth) {
            ChildProcessResult result = run(statements, depth);
            if (result.output.contains("spoiled false"_s))
                return count;
            checkCrashed(result);
            count += result.output.contains(lastLine) && !result.output.contains(lineThatWouldFollow);
        }
        failNativeTest(__FILE__, __LINE__, "the frame records come to an end");
    }

    static void checkCrashed(const ChildProcessResult& result)
    {
        if (result.outcome == ChildProcessOutcome::Crashed)
            return;
        dataLogLn(result.output, "\n", result.description());
        failNativeTest(__FILE__, __LINE__, "the process ends at the spoiled address");
    }

private:
    static std::unique_ptr<CompileOutput> compile(ASCIILiteral way)
    {
        std::unique_ptr<CompileOutput> output;
        runOnBuildThread([&] {
            output = compileForShellOnThisThread(programWhoseAddressesAreSpoiled, { }, way);
        });
        return output;
    }

    std::unique_ptr<CompileOutput> m_compiled;
    TemporaryFile m_image;
};

} // anonymous namespace

static void checkSpoiledFrameRecords(ASCIILiteral statements, ASCIILiteral outputWithNothingSpoiled, ASCIILiteral lineBehindSpoiling, unsigned recordsUsedInFrontOfLine)
{
    for (ASCIILiteral way : waysToCompilePrograms) {
        ProgramWhoseAddressesAreSpoiled program(way);
        program.checkOutputWithNothingSpoiled(statements, outputWithNothingSpoiled);
        if (processorSignsAddresses())
            AOT_TEST_CHECK(program.spoilEachFrameRecord(statements, "spoiled true"_s, lineBehindSpoiling) >= recordsUsedInFrontOfLine);
    }
}

static void testSpoiledReturnAddressIsNotReturnedTo()
{
    checkSpoiledFrameRecords("calls(returns)"_s, "spoiled false\ncaller goes on\ndone\n"_s, "caller goes on"_s, 1);
    checkSpoiledFrameRecords("calls(returns)"_s, "spoiled false\ncaller goes on\ndone\n"_s, "done"_s, 3);
}

static void testSpoiledReturnAddressChoosesNoHandler()
{
    for (ASCIILiteral statements : { "catches(throws)"_s, "catches(stays, JSON.parse)"_s, "catches(stays, () => { throw 1; })"_s })
        checkSpoiledFrameRecords(statements, "spoiled false\nin handler\ndone\n"_s, "in handler"_s, 1);
    checkSpoiledFrameRecords("awaitsCall(throws)"_s, "spoiled false\ndone\nin handler\n"_s, "done"_s, 3);
}

static void testSpoiledReturnAddressIsNotHandedToTailCall()
{
    checkSpoiledFrameRecords("calls(leaves, isEntered)"_s, "spoiled false\nentered\ncaller goes on\ndone\n"_s, "entered"_s, 1);
    checkSpoiledFrameRecords("calls(leaves, () => print('entered'))"_s, "spoiled false\nentered\ncaller goes on\ndone\n"_s, "entered"_s, 1);
    checkSpoiledFrameRecords("leaves(() => print('entered'))"_s, "spoiled false\nentered\ndone\n"_s, "entered"_s, 1);
    checkSpoiledFrameRecords("calls(() => { 'use strict'; returns(); return isEntered(); })"_s, "spoiled false\nentered\ncaller goes on\ndone\n"_s, "entered"_s, 3);
}

using PlaceOfFrameByWay = std::array<ASCIILiteral, std::size(waysToCompilePrograms)>;

static void checkSpoiledResumeAddress(ASCIILiteral statements, ASCIILiteral outputWithNothingSpoiled, const PlaceOfFrameByWay& placeOfFrameByWay)
{
    for (size_t way = 0; way < placeOfFrameByWay.size(); ++way) {
        ProgramWhoseAddressesAreSpoiled program(waysToCompilePrograms[way]);
        program.checkOutputWithNothingSpoiled(makeString("const spoil = () => { }; "_s, statements), outputWithNothingSpoiled);
        if (!processorSignsAddresses())
            continue;
        ChildProcessResult result = program.run(makeString("const spoil = () => print('spoiled', aotSpoilResumeAddresses()); "_s, statements));
        ProgramWhoseAddressesAreSpoiled::checkCrashed(result);
        if (!result.output.contains(placeOfFrameByWay[way]) || result.output.contains("resumed"_s)) {
            dataLogLn(result.output);
            failNativeTest(__FILE__, __LINE__, "the process ends in front of the place where the frame resumes");
        }
    }
}

static void testSpoiledResumeAddressOfAsyncFunction()
{
    checkSpoiledResumeAddress("waits(Promise.resolve()); spoil()"_s, "done\nresumed\n"_s, { "stack"_s, "stack"_s });
    checkSpoiledResumeAddress("catchesRejection(Promise.reject(1)); spoil()"_s, "done\nresumed\n"_s, { "stack"_s, "stack"_s });
}

static void testSpoiledResumeAddressOfGenerator()
{
    for (ASCIILiteral statements : {
        "steps(yields, spoil)"_s,
        "const generator = yields(); generator.next(); spoil(); generator.next()"_s,
        "const generator = yields(); spoil(); generator.next(); print('resumed')"_s,
        "let kept; breaks(() => yieldsToLoop.prototype.return = function () { return kept = this; }); spoil(); kept.next()"_s,
    })
        checkSpoiledResumeAddress(statements, "resumed\ndone\n"_s, { "heap"_s, "stack"_s });
}

static void checkOutputInEveryWay(ASCIILiteral statements, ASCIILiteral expected)
{
    for (ASCIILiteral way : waysToCompilePrograms)
        ProgramWhoseAddressesAreSpoiled(way).checkOutputWithNothingSpoiled(statements, expected);
}

std::span<const NativeTest> signedReturnAddressTests()
{
    static constexpr NativeTest tests[] = {
        { "signed-return-addresses/assembler/what-leaves-no-trace-is-emitted-as-it-is"_s, testWhatLeavesNoTraceIsEmittedAsItIs },
        { "signed-return-addresses/assembler/what-is-copied-out-is-signed"_s, testWhatIsCopiedOutIsSigned },
        { "signed-return-addresses/assembler/what-overwrites-is-authenticated"_s, testWhatOverwritesIsAuthenticated },
        { "signed-return-addresses/assembler/atomic-access-is-signed-and-authenticated"_s, testAtomicAccessIsSignedAndAuthenticated },
        { "signed-return-addresses/assembler/plain-address-is-not-stored-for-a-moment"_s, testPlainAddressIsNotStoredForAMoment },
        { "signed-return-addresses/assembler/instructions-that-it-lacks"_s, testInstructionsThatTheAssemblerLacks },
        { "signed-return-addresses/assembler/nothing-else-signs-authenticates-or-strips"_s, testNothingElseSignsAuthenticatesOrStrips },
        { "signed-return-addresses/assembler/no-policy-switches-signing-off"_s, testNoPolicySwitchesSigningOff },
        { "signed-return-addresses/assembler/load-behind-authentication-stays-unless-jump-follows"_s, testLoadBehindAuthenticationStaysUnlessJumpFollows },
        { "signed-return-addresses/assembler/description-does-not-keep-load"_s, testDescriptionDoesNotKeepLoad },
        { "signed-return-addresses/assembler/every-store-agrees-with-every-load"_s, ImageTests::whenExecutable<testEveryStoreAgreesWithEveryLoad> },
        { "signed-return-addresses/assembler/every-push-agrees-with-every-pop"_s, ImageTests::whenExecutable<testEveryPushAgreesWithEveryPop> },
        { "signed-return-addresses/assembler/link-register-is-plain-behind-save"_s, ImageTests::whenExecutable<testLinkRegisterIsPlainBehindSave> },
        { "signed-return-addresses/assembler/word-that-is-copied-elsewhere-is-not-returned-to"_s, ImageTests::whenExecutable<testWordThatIsCopiedElsewhereIsNotReturnedTo> },
        { "signed-return-addresses/assembler/return-to-old-link-register"_s, ImageTests::whenExecutable<testReturnToOldLinkRegister> },
        { "signed-return-addresses/assembler/address-that-failed-is-not-signed-again"_s, ImageTests::whenExecutable<testAddressThatFailedIsNotSignedAgain> },
        { "signed-return-addresses/assembler/address-to-look-at-is-read-and-not-jumped-to"_s, ImageTests::whenExecutable<testAddressToLookAtIsReadAndNotJumpedTo> },
        { "signed-return-addresses/assembler/resume-address-comes-back-in-every-way"_s, ImageTests::whenExecutable<testResumeAddressComesBackInEveryWay> },
        { "signed-return-addresses/assembler/word-of-instance-holds-signed-address"_s, ImageTests::whenExecutable<testWordOfInstanceHoldsSignedAddress> },
        { "signed-return-addresses/assembler/no-plain-address-is-left-in-x17"_s, ImageTests::whenExecutable<testNoPlainAddressIsLeftInX17> },
        { "signed-return-addresses/what-only-looks-takes-signature-off"_s, testWhatOnlyLooksTakesSignatureOff },
        { "signed-return-addresses/spoiled-address-is-not-returned-to"_s, testSpoiledReturnAddressIsNotReturnedTo },
        { "signed-return-addresses/spoiled-address-chooses-no-handler"_s, testSpoiledReturnAddressChoosesNoHandler },
        { "signed-return-addresses/spoiled-address-is-not-handed-to-tail-call"_s, testSpoiledReturnAddressIsNotHandedToTailCall },
        { "signed-return-addresses/spoiled-resume-address-of-async-function"_s, testSpoiledResumeAddressOfAsyncFunction },
        { "signed-return-addresses/spoiled-resume-address-of-generator"_s, testSpoiledResumeAddressOfGenerator },
        { "signed-return-addresses/fault-in-function-without-frame"_s, [] { checkOutputInEveryWay("readsNothing(true)"_s, "in handler\ndone\n"_s); } },
        { "signed-return-addresses/adapter-frame-moved-above-long-list"_s, [] { checkOutputInEveryWay("counts(...Array(5000))"_s, "5000\ndone\n"_s); } },
        { "signed-return-addresses/generator-throws-at-once"_s, [] { checkOutputInEveryWay("try { throwsAtOnce().next(); } catch { print('caught'); }"_s, "caught\ndone\n"_s); } },
        { "signed-return-addresses/throw-after-cleanup-leaves-compiled-code"_s, [] { checkOutputInEveryWay("try { cleansUp(1); } catch { print('caught'); }"_s, "cleaned up\ncaught\ndone\n"_s); } },
    };
    return tests;
}

#else

std::span<const NativeTest> signedReturnAddressTests()
{
    return { };
}

#endif // ENABLE(RETURN_ADDRESS_SIGNING)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
