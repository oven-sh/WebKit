/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTNativeTest.h"

#if ENABLE(RETURN_ADDRESS_SIGNING)
#include "AOTReturnAddressCheck.h"
#include "AOTSavedCodeAddresses.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubTestSupport.h"
#include <sys/auxv.h>
#include <wtf/UnalignedAccess.h>
#endif

namespace JSC { namespace AOT {

#if ENABLE(RETURN_ADDRESS_SIGNING)

using RuleOfReturnAddresses = ReturnAddressCheck::Rule;

static constexpr GPRReg framePointer = CCallHelpers::framePointerRegister;
static constexpr GPRReg linkRegister = CCallHelpers::linkRegister;
static constexpr GPRReg otherRegister = ARM64Registers::x9;

template<typename Emit>
static Vector<uint8_t> codeEmittedBy(const Emit& emit, const AssemblerPolicy& policy = AssemblerPolicy::forImage(CPUFeatureSet::buildTarget()))
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, policy);
    emit(jit);
    emitPadding(jit);
    std::expected<RelocatableCode, LinkFailure> code = relocations.finalize(LinkBuffer::Profile::Thunk, nullptr);
    AOT_TEST_CHECK(code.has_value() && code->relocations.isEmpty());
    return WTF::move(code->bytes);
}

template<typename Emit>
static std::optional<RuleOfReturnAddresses> ruleBrokenBy(const Emit& emit, const AssemblerPolicy& policy = AssemblerPolicy::forImage(CPUFeatureSet::buildTarget()))
{
    Vector<uint8_t> code = codeEmittedBy(emit, policy);
    ReturnAddressCheck check(code.span());
    check.addCalledEntry(0);
    std::optional<ReturnAddressCheck::Violation> violation = check.run();
    return violation ? std::optional(violation->rule) : std::nullopt;
}

template<typename Emit>
static void checkRuleBrokenBehindJoinWhereOnePathSigned(RuleOfReturnAddresses rule, const Emit& emit)
{
    for (bool signsOnFallThrough : { false, true }) {
        AOT_TEST_CHECK(rule == ruleBrokenBy([&](CCallHelpers& jit) {
            CCallHelpers::JumpList join;
            CCallHelpers::Jump isTaken = jit.branchTest64(CCallHelpers::Zero, ARM64Registers::x0);
            if (signsOnFallThrough)
                jit.tagReturnAddress();
            join.append(jit.jump());
            isTaken.link(&jit);
            if (!signsOnFallThrough)
                jit.tagReturnAddress();
            join.append(jit.jump());
            emitPadding(jit);
            join.link(&jit);
            emit(jit);
        }));
    }
}

static void callUnknownCode(CCallHelpers& jit)
{
    jit.call(ARM64Registers::x8, NoPtrTag);
}

static void jumpToUnknownCode(CCallHelpers& jit)
{
    jit.farJump(ARM64Registers::x8, NoPtrTag);
}

static void testCheckAcceptsWhatTheAssemblerOffers()
{
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.ret();
    }));
    AOT_TEST_CHECK(!ruleBrokenBy(jumpToUnknownCode));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        CCallHelpers::Jump needsNoFrame = jit.branchTest64(CCallHelpers::Zero, ARM64Registers::x0);
        jit.emitFunctionPrologue(SavedLinkRegister::IsDead);
        callUnknownCode(jit);
        jit.emitFunctionEpilogue(LoadedLinkRegister::IsOnlyReturnedTo);
        needsNoFrame.link(&jit);
        jit.ret();
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionPrologue();
        jit.move(linkRegister, ARM64Registers::x1);
        callUnknownCode(jit);
        jit.emitFunctionEpilogue();
        jit.emitFunctionPrologue();
        callUnknownCode(jit);
        jit.emitFunctionEpilogue();
        jumpToUnknownCode(jit);
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionPrologue(SavedLinkRegister::IsDead);
        callUnknownCode(jit);
        jit.emitFunctionEpilogue(LoadedLinkRegister::IsOnlyReturnedTo);
        CCallHelpers::Jump isRare = jit.branchTest64(CCallHelpers::NonZero, ARM64Registers::x1);
        jit.ret();
        isRare.link(&jit);
        jit.validateLoadedReturnAddress();
        jumpToUnknownCode(jit);
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        emitPushLinkRegister(jit);
        callUnknownCode(jit);
        emitPopLinkRegister(jit);
        jit.add64(CCallHelpers::TrustedImm32(4), linkRegister);
        jit.ret();
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        emitLoadReturnAddressOfFrameRecord(jit, framePointer, otherRegister);
        emitStoreReturnAddressInFrameRecord(jit, otherRegister, ARM64Registers::x1);
        emitStoreResumeAddress(jit, linkRegister, ARM64Registers::x2);
        emitLoadResumeAddress(jit, ARM64Registers::x2, otherRegister);
        jit.m_assembler.ret(otherRegister);
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), otherRegister);
        CCallHelpers::Jump differs = jit.branchPtr(CCallHelpers::NotEqual, otherRegister, linkRegister);
        jit.m_assembler.ret(otherRegister);
        differs.link(&jit);
        jit.ret();
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionPrologue();
        callUnknownCode(jit);
        jit.emitFunctionEpilogue();
        jit.ret();
    }, AssemblerPolicy::forImage(CPUFeatureSet::buildTarget()).withLinkRegisterSignedUntilReturn()));
}

static void testCheckFindsSaveWithoutSignature()
{
    constexpr auto rule = RuleOfReturnAddresses::SavesOnlyWhatIsSigned;
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.pushPair(framePointer, linkRegister);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.pushToSave(linkRegister);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.storePtr(linkRegister, CCallHelpers::BaseIndex(ARM64Registers::x0, ARM64Registers::x1, CCallHelpers::TimesEight));
    }));
    static constexpr void (*storesInFrameRecord[])(CCallHelpers&) = {
        [](CCallHelpers& jit) { jit.pushPair(framePointer, otherRegister); },
        [](CCallHelpers& jit) { jit.storePair64(framePointer, otherRegister, ARM64Registers::x0, CCallHelpers::TrustedImm32(16)); },
        [](CCallHelpers& jit) { jit.storePair64(framePointer, otherRegister, CCallHelpers::PostIndexAddress(ARM64Registers::x0, 16)); },
        [](CCallHelpers& jit) { jit.storePair64WithNonTemporalAccess(framePointer, otherRegister, ARM64Registers::x0); },
        [](CCallHelpers& jit) { jit.storePair64(ARM64Registers::x0, otherRegister, framePointer); },
        [](CCallHelpers& jit) { jit.m_assembler.stur<64>(otherRegister, framePointer, CallFrame::returnPCOffset()); },
        [](CCallHelpers& jit) { jit.m_assembler.str<64>(otherRegister, framePointer, static_cast<unsigned>(CallFrame::returnPCOffset())); },
    };
    for (auto store : storesInFrameRecord) {
        AOT_TEST_CHECK(rule == ruleBrokenBy([&](CCallHelpers& jit) {
            jit.move(linkRegister, otherRegister);
            store(jit);
        }));
        AOT_TEST_CHECK(!ruleBrokenBy([&](CCallHelpers& jit) {
            jit.tagReturnAddress();
            jit.move(linkRegister, otherRegister);
            store(jit);
        }));
    }
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.popPair(framePointer, linkRegister);
        jit.removeTagAfterSavingReturnAddress();
        jit.pushPair(framePointer, linkRegister);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        emitPopLinkRegister(jit, LoadedLinkRegister::IsOnlyReturnedTo);
        jit.add64(CCallHelpers::TrustedImm32(4), linkRegister);
        jit.pushPair(framePointer, linkRegister);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionPrologue();
        callUnknownCode(jit);
        jit.pushToSave(linkRegister);
    }));
    checkRuleBrokenBehindJoinWhereOnePathSigned(rule, [](CCallHelpers& jit) {
        jit.pushPair(framePointer, linkRegister);
    });
    checkRuleBrokenBehindJoinWhereOnePathSigned(rule, [](CCallHelpers& jit) {
        jit.removeTagAfterSavingReturnAddress();
        jit.pushPair(framePointer, linkRegister);
    });
}

static void testCheckTakesSavedRegistersForNoFrameRecord()
{
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionPrologue();
        jit.m_assembler.movz<64>(otherRegister, 10);
        CCallHelpers::Jump differs = jit.branch64(CCallHelpers::NotEqual, otherRegister, GPRInfo::notCellMaskRegister);
        CCallHelpers::Call call = jit.nearCall();
        differs.link(&jit);
        jit.breakpoint();
        CCallHelpers::Label savesEveryRegister = jit.label();
        jit.emitFunctionPrologue();
        jit.storePair64(GPRInfo::notCellMaskRegister, framePointer, CCallHelpers::stackPointerRegister, CCallHelpers::TrustedImm32(224));
        jit.addLinkTask([=](LinkBuffer& linkBuffer) {
            linkBuffer.link(call, linkBuffer.locationOf<NoPtrTag>(savesEveryRegister));
        });
    }));
    AOT_TEST_CHECK(!ruleBrokenBy([](CCallHelpers& jit) {
        jit.move(linkRegister, otherRegister);
        jit.storePair64(ARM64Registers::x8, otherRegister, framePointer, CCallHelpers::TrustedImm32(-16));
        jit.storePair64(otherRegister, ARM64Registers::x8, framePointer);
        jit.storePtr(otherRegister, CCallHelpers::Address(framePointer, -8));
        jit.storePtr(otherRegister, CCallHelpers::Address(ARM64Registers::x0, CallFrame::returnPCOffset()));
    }));
}

static void testCheckFindsReturnWithoutAuthentication()
{
    constexpr auto rule = RuleOfReturnAddresses::ReturnsOnlyToWhatIsAuthenticated;
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionPrologue();
        jit.popPair(framePointer, linkRegister);
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.popPair(framePointer, linkRegister);
        jit.removeTagAfterSavingReturnAddress();
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), otherRegister);
        jit.move(otherRegister, linkRegister);
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), otherRegister);
        jit.m_assembler.ret(otherRegister);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), otherRegister);
        CCallHelpers::Jump isSame = jit.branchPtr(CCallHelpers::Equal, otherRegister, linkRegister);
        jit.m_assembler.ret(otherRegister);
        isSame.link(&jit);
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.move(linkRegister, ARM64Registers::x19);
        callUnknownCode(jit);
        jit.move(ARM64Registers::x19, linkRegister);
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.moveDoubleTo64(ARM64Registers::q0, linkRegister);
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        CCallHelpers::Jump keepsIt = jit.branchTest64(CCallHelpers::Zero, ARM64Registers::x0);
        jit.popToRestore(linkRegister);
        keepsIt.link(&jit);
        jit.ret();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        CCallHelpers::Label head = jit.label();
        CCallHelpers::Jump goesOn = jit.branchTest64(CCallHelpers::Zero, ARM64Registers::x0);
        jit.ret();
        goesOn.link(&jit);
        jit.popToRestore(linkRegister);
        jit.jump().linkTo(head, &jit);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.tagReturnAddress();
        jit.ret();
    }));
    for (auto leave : { jumpToUnknownCode, callUnknownCode }) {
        AOT_TEST_CHECK(rule == ruleBrokenBy([&](CCallHelpers& jit) {
            jit.loadPtr(CCallHelpers::Address(framePointer, CallFrame::returnPCOffset()), otherRegister);
            jit.removePtrTag(otherRegister);
            jit.add64(CCallHelpers::TrustedImm32(4), otherRegister, ARM64Registers::x8);
            leave(jit);
        }));
    }
}

static void testCheckFindsSignatureForWhatWasInMemory()
{
    constexpr auto rule = RuleOfReturnAddresses::SignsOnlyWhatWasNeverInMemory;
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.popToRestore(linkRegister);
        jit.emitFunctionPrologue();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionEpilogue(LoadedLinkRegister::IsOnlyReturnedTo);
        jit.emitFunctionPrologue();
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), otherRegister);
        emitStoreReturnAddressInFrameRecord(jit, otherRegister, framePointer);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), otherRegister);
        emitStoreResumeAddress(jit, otherRegister, ARM64Registers::x1);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.tagReturnAddress();
        jit.tagReturnAddress();
    }));
}

static void testCheckFindsAuthenticationWithoutSignature()
{
    AOT_TEST_CHECK(RuleOfReturnAddresses::AuthenticatesOnlyWhatIsSigned == ruleBrokenBy([](CCallHelpers& jit) {
        jit.ret();
    }, AssemblerPolicy::forImage(CPUFeatureSet::buildTarget()).withLinkRegisterSignedUntilReturn()));
    AOT_TEST_CHECK(RuleOfReturnAddresses::AuthenticatesOnlyWhatIsSigned == ruleBrokenBy([](CCallHelpers& jit) {
        jit.popPair(framePointer, linkRegister);
        jit.removeTagAfterSavingReturnAddress();
        jit.untagAfterLoadingReturnAddress();
    }));
    checkRuleBrokenBehindJoinWhereOnePathSigned(RuleOfReturnAddresses::AuthenticatesOnlyWhatIsSigned, [](CCallHelpers& jit) {
        jit.untagAfterLoadingReturnAddress();
    });
}

static void testCheckFindsOtherKeyOrNoModifier()
{
    for (unsigned hint : { 8, 12, 24, 25, 26, 28, 29, 30 }) {
        AOT_TEST_CHECK(RuleOfReturnAddresses::UsesOneKeyAndModifier == ruleBrokenBy([&](CCallHelpers& jit) {
            jit.m_assembler.hint(hint);
        }));
    }
}

static void testCheckFindsFaultThatSignsWhatWasInMemory()
{
    for (LoadedLinkRegister loaded : { LoadedLinkRegister::MayBeSavedAgain, LoadedLinkRegister::IsOnlyReturnedTo }) {
        Vector<uint8_t> code = codeEmittedBy([&](CCallHelpers& jit) {
            jit.emitFunctionEpilogueWithEmptyFrame(loaded);
            jit.loadPtr(CCallHelpers::Address(ARM64Registers::x0), ARM64Registers::x0);
            jit.ret();
        });
        ReturnAddressCheck check(code.span());
        check.addEntryInsideFrame(0);
        check.addCodeWhoseFaultsSignLinkRegister(0, code.size());
        std::optional<ReturnAddressCheck::Violation> violation = check.run();
        if (loaded == LoadedLinkRegister::MayBeSavedAgain)
            AOT_TEST_CHECK(!violation);
        else
            AOT_TEST_CHECK(violation && violation->rule == RuleOfReturnAddresses::SignsOnlyWhatWasNeverInMemory);
    }
}

static void testCheckFindsLinkRegisterHandedOn()
{
    constexpr auto rule = RuleOfReturnAddresses::LeavesNothingUncheckedInLinkRegister;
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.popPair(framePointer, linkRegister);
        jumpToUnknownCode(jit);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.emitFunctionEpilogue(LoadedLinkRegister::IsOnlyReturnedTo);
        jumpToUnknownCode(jit);
    }));
    AOT_TEST_CHECK(rule == ruleBrokenBy([](CCallHelpers& jit) {
        jit.move(linkRegister, otherRegister);
        jit.popPair(framePointer, linkRegister);
        jit.m_assembler.ret(otherRegister);
    }));
}

static void testCheckEntersLabelWhoseAddressIsTaken()
{
    constexpr unsigned wordsOfSeveralPages = 3000;
    for (PCRelativeReach reach : { PCRelativeReach::Near, PCRelativeReach::Far }) {
        for (bool labelComesFirst : { false, true }) {
            Vector<uint8_t> code = codeEmittedBy([&](CCallHelpers& jit) {
                CCallHelpers::Label site;
                CCallHelpers::Label label;
                auto takeAddress = [&] {
                    site = jit.movePCRelativeAddress(ARM64Registers::x0, reach);
                    jit.ret();
                };
                auto breakRule = [&] {
                    label = jit.label();
                    jit.pushToSave(linkRegister);
                };
                CCallHelpers::Jump skip = jit.jump();
                if (labelComesFirst)
                    breakRule();
                else {
                    skip.link(&jit);
                    takeAddress();
                }
                for (unsigned index = 0; index < wordsOfSeveralPages; ++index)
                    jit.breakpoint();
                if (labelComesFirst) {
                    skip.link(&jit);
                    takeAddress();
                } else
                    breakRule();
                jit.addLinkTask([=](LinkBuffer& linkBuffer) {
                    AOT_TEST_CHECK(linkBuffer.link(site, addressKindFor(reach), label));
                });
            });
            ReturnAddressCheck check(code.span());
            check.addCalledEntry(0);
            check.addCodeThatTakesAddressesOfItsLabels(0, code.size(), ReturnAddressCheck::Labels::AreCalledOrReturnedTo);
            std::optional<ReturnAddressCheck::Violation> violation = check.run();
            AOT_TEST_CHECK(violation && violation->rule == RuleOfReturnAddresses::SavesOnlyWhatIsSigned);
        }
    }
}

static String nameOfStubAt(const StubBlob& blob, uint32_t offset)
{
    const StubPlacement* found = nullptr;
    for (const StubPlacement& placement : blob.placements) {
        if (placement.offset <= offset && (!found || placement.offset > found->offset))
            found = &placement;
    }
    return found ? makeString(descriptorOf(found->key.family()).name, '+', offset - found->offset) : String::number(offset);
}

static void testEveryStubKeepsTheRules()
{
    Vector<StubKey> keys = stubKeysWithLinkage(StubLinkage::CalledByCode);
    forEachKeyWithoutImmediate([&](StubKey key) {
        if (descriptorOf(key.family()).binding != ImmediateBinding::OperationOffset)
            return;
        for (const OperationPrelude& prelude : operationPreludes()) {
            if (StubKey head = key.withImmediate(offsetInRuntimeTable(prelude.operation)); exists(head))
                keys.append(head);
        }
    });
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options);
        WTF::BitSet<numberOfEntries> everyEntry;
        everyEntry.setAll();
        BuildContext context(makeDefaultCompileOptions());
        auto blob = buildStubBlob(context, catalog, keys.span(), everyEntry);
        AOT_TEST_CHECK(blob.has_value());

        std::span<const uint8_t> code = blob->code.bytes.span();
        ReturnAddressCheck check(code);
        check.addStubs(*blob, 0);
        if (std::optional<ReturnAddressCheck::Violation> violation = check.run()) {
            dataLogLn(nameOfStubAt(*blob, violation->offset), ": ", nameOf(violation->rule));
            failNativeTest(__FILE__, __LINE__, "no stub breaks a rule");
        }

        // Some stubs have code behind a jump that nothing jumps to.
        unsigned reached = 0;
        unsigned leftOut = 0;
        for (uint32_t offset = 0; offset < code.size(); offset += sizeof(uint32_t))
            ++(check.hasReached(offset) ? reached : leftOut);
        AOT_TEST_CHECK(leftOut < reached / 20);
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

static bool processorSignsAddresses()
{
    constexpr unsigned long hwcapPACA = 1UL << 30;
    return getauxval(AT_HWCAP) & hwcapPACA;
}

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
            output = compileForShellOnThisThread(programWhoseAddressesAreSpoiled, { "check=VerifyEachStage"_s }, way);
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
        { "signed-return-addresses/check/accepts-what-the-assembler-offers"_s, testCheckAcceptsWhatTheAssemblerOffers },
        { "signed-return-addresses/check/finds-save-without-signature"_s, testCheckFindsSaveWithoutSignature },
        { "signed-return-addresses/check/takes-saved-registers-for-no-frame-record"_s, testCheckTakesSavedRegistersForNoFrameRecord },
        { "signed-return-addresses/check/finds-return-without-authentication"_s, testCheckFindsReturnWithoutAuthentication },
        { "signed-return-addresses/check/finds-signature-for-what-was-in-memory"_s, testCheckFindsSignatureForWhatWasInMemory },
        { "signed-return-addresses/check/finds-authentication-without-signature"_s, testCheckFindsAuthenticationWithoutSignature },
        { "signed-return-addresses/check/finds-other-key-or-no-modifier"_s, testCheckFindsOtherKeyOrNoModifier },
        { "signed-return-addresses/check/finds-fault-that-signs-what-was-in-memory"_s, testCheckFindsFaultThatSignsWhatWasInMemory },
        { "signed-return-addresses/check/finds-link-register-handed-on"_s, testCheckFindsLinkRegisterHandedOn },
        { "signed-return-addresses/check/enters-label-whose-address-is-taken"_s, testCheckEntersLabelWhoseAddressIsTaken },
        { "signed-return-addresses/every-stub-keeps-the-rules"_s, testEveryStubKeepsTheRules },
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
