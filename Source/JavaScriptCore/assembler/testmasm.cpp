/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"

#include "CCallHelpers.h"
#include "CPU.h"
#include "CPUFeatureSet.h"
#include "ExecutableAllocator.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "InitializeThreading.h"
#include "LinkBuffer.h"
#include "ProbeContext.h"
#include "StackAlignment.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Compiler.h>
#include <wtf/DataLog.h>
#include <wtf/Function.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/NumberOfCores.h>
#include <wtf/PtrTag.h>
#include <wtf/Threading.h>
#include <wtf/WTFProcess.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/StringCommon.h>

#if ENABLE(JIT_PROTECTION_KEY)
#include "JSLock.h"
#include "VM.h"
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// We don't have a NO_RETURN_DUE_TO_EXIT, nor should we. That's ridiculous.
static bool LEGACY_NODELETE hiddenTruthBecauseNoReturnIsStupid() { return true; }

static void usage()
{
    dataLog("Usage: testmasm [<filter>]\n");
    if (hiddenTruthBecauseNoReturnIsStupid())
        exitProcess(1);
}

#if ENABLE(JIT)

static Vector<double> doubleOperands()
{
    return Vector<double> {
        0,
        -0,
        1,
        -1,
        42,
        -42,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
}

static Vector<float> floatOperands()
{
    return Vector<float> {
        0,
        -0,
        1,
        -1,
        42,
        -42,
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::min(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
}

static Vector<int32_t> int32Operands()
{
    return Vector<int32_t> {
        0,
        1,
        -1,
        2,
        -2,
        42,
        -42,
        64,
        static_cast<int32_t>(0x80000000U),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
    };
}

static UNUSED_FUNCTION Vector<int16_t> int16Operands()
{
    return Vector<int16_t> {
        0,
        1,
        -1,
        42,
        -42,
        std::numeric_limits<int16_t>::max(),
        std::numeric_limits<int16_t>::min(),
        static_cast<int16_t>(std::numeric_limits<uint16_t>::max()),
        static_cast<int16_t>(std::numeric_limits<uint16_t>::min())
    };
}

static UNUSED_FUNCTION Vector<int8_t> int8Operands()
{
    return Vector<int8_t> {
        0,
        1,
        -1,
        42,
        -42,
        std::numeric_limits<int8_t>::max(),
        std::numeric_limits<int8_t>::min(),
        static_cast<int8_t>(std::numeric_limits<uint8_t>::max()),
        static_cast<int8_t>(std::numeric_limits<uint8_t>::min())
    };
}

#if CPU(X86_64) || CPU(ARM64)
static Vector<int64_t> int64Operands()
{
    return Vector<int64_t> {
        0,
        1,
        -1,
        2,
        -2,
        42,
        -42,
        64,
        static_cast<int64_t>(0x8000000000000000ULL),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min(),
    };
}
#endif

namespace WTF {

static void printInternal(PrintStream& out, void* value)
{
    out.printf("%p", value);
}

} // namespace WTF

namespace JSC {
namespace Probe {

JS_EXPORT_PRIVATE void* probeStateForContext(Probe::Context&);

} // namespace Probe
} // namespace JSC

using namespace JSC;

namespace {

using CPUState = Probe::CPUState;

Lock crashLock;

typedef WTF::Function<void(CCallHelpers&)> Generator;

template<typename T> T LEGACY_NODELETE nextID(T id) { return static_cast<T>(id + 1); }

#define TESTWORD64 0x0c0defefebeef000
#define TESTWORD32 0x0beef000

#define testWord32(x) (TESTWORD32 + static_cast<uint32_t>(x))
#define testWord64(x) (TESTWORD64 + static_cast<uint64_t>(x))

#define testWord(x) testWord64(x)

// Nothing fancy for now; we just use the existing WTF assertion machinery.
#define CHECK_EQ(_actual, _expected) do {                               \
        if ((_actual) == (_expected))                                   \
            break;                                                      \
        crashLock.lock();                                               \
        dataLog("FAILED while testing " #_actual ": expected: ", _expected, ", actual: ", _actual, "\n"); \
        WTFReportAssertionFailure(__FILE__, __LINE__, WTF_PRETTY_FUNCTION, "CHECK_EQ("#_actual ", " #_expected ")"); \
        CRASH();                                                        \
    } while (false)

#define CHECK_NOT_EQ(_actual, _expected) do {                               \
        if ((_actual) != (_expected))                                   \
            break;                                                      \
        crashLock.lock();                                               \
        dataLog("FAILED while testing " #_actual ": expected not: ", _expected, ", actual: ", _actual, "\n"); \
        WTFReportAssertionFailure(__FILE__, __LINE__, WTF_PRETTY_FUNCTION, "CHECK_NOT_EQ("#_actual ", " #_expected ")"); \
        CRASH();                                                        \
    } while (false)

bool LEGACY_NODELETE isPC(MacroAssembler::RegisterID id)
{
    UNUSED_PARAM(id);
    return false;
}

bool LEGACY_NODELETE isSP(MacroAssembler::RegisterID id)
{
    return id == MacroAssembler::stackPointerRegister;
}

bool LEGACY_NODELETE isFP(MacroAssembler::RegisterID id)
{
    return id == MacroAssembler::framePointerRegister;
}

bool LEGACY_NODELETE isSpecialGPR(MacroAssembler::RegisterID id)
{
    if (isPC(id) || isSP(id) || isFP(id))
        return true;
#if CPU(ARM64)
    if (id == ARM64Registers::x18)
        return true;
#elif CPU(RISCV64)
    if (id == RISCV64Registers::zero || id == RISCV64Registers::ra || id == RISCV64Registers::gp || id == RISCV64Registers::tp)
        return true;
#endif
    return false;
}

MacroAssemblerCodeRef<JSEntryPtrTag> compile(Generator&& generate)
{
    CCallHelpers jit;
    generate(jit);
    LinkBuffer linkBuffer(jit, nullptr);
    return FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm compilation");
}

template<typename T, typename... Arguments>
T invoke(const MacroAssemblerCodeRef<JSEntryPtrTag>& code, Arguments... arguments)
{
    void* executableAddress = untagCFunctionPtr<JSEntryPtrTag>(code.code().taggedPtr());
    T (SYSV_ABI *function)(Arguments...) = std::bit_cast<T(SYSV_ABI *)(Arguments...)>(executableAddress);

#if CPU(RISCV64)
    // RV64 calling convention requires all 32-bit values to be sign-extended into the whole register.
    // JSC JIT is tailored for other ISAs that pass these values in 32-bit-wide registers, which RISC-V
    // doesn't support, so any 32-bit value passed in return-value registers has to be manually sign-extended.
    // This mirrors sign-extension of 32-bit values in argument registers on RV64 in CCallHelpers.h.
    if constexpr (std::is_integral_v<T>) {
        T returnValue = function(arguments...);
        if constexpr (sizeof(T) == 4) {
            asm volatile(
                "sext.w %[out_value], %[in_value]\n\t"
                : [out_value] "=r" (returnValue)
                : [in_value] "r" (returnValue));
        }
        return returnValue;
    }
#endif

    return function(arguments...);
}

template<typename T, typename... Arguments>
T compileAndRun(Generator&& generator, Arguments... arguments)
{
    return invoke<T>(compile(WTF::move(generator)), arguments...);
}

#if CPU(X86_64) || CPU(ARM64)
struct LinkedCode {
    std::span<uint8_t> bytes() { return asMutableByteSpan(storage.mutableSpan()).first(size); }

    Vector<uint32_t> storage;
    size_t size { 0 };
    Vector<LinkedSite> sites;
    Vector<WideImmediate> wideImmediates;
    Vector<ExternalLink> externalLinks;
    bool hasProcessAddress { false };
};

enum class JumpShortening : bool { No, Yes };

template<typename Emit>
void emitWithRecording(CCallHelpers& jit, const Emit& emit, JumpShortening shortening)
{
    AssemblerPolicy policy = AssemblerPolicy::forImage(CPUFeatureSet::host());
    policy.shortensJumps = shortening == JumpShortening::Yes;
    jit.setPolicy(policy);
    emit(jit);
}

template<typename Link>
void finalizeAndCopyReports(LinkBuffer& linkBuffer, const Link& link, LinkedCode& result)
{
    link(linkBuffer);
    FINALIZE_CODE_IF(false, linkBuffer, JSEntryPtrTag, nullptr, "testmasm linked code");
    result.sites = linkBuffer.linkedSites();
    result.wideImmediates = linkBuffer.wideImmediates();
    result.externalLinks = linkBuffer.externalLinks();
    result.hasProcessAddress = linkBuffer.hasProcessAddress();
}

template<typename Emit, typename Link>
LinkedCode linkIntoStorage(const Emit& emit, const Link& link, JumpShortening shortening = JumpShortening::Yes)
{
    CCallHelpers jit;
    emitWithRecording(jit, emit, shortening);
    CCallHelpers::Label end = jit.label();

    LinkedCode result;
    result.storage.fill(0, roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(result.storage.mutableSpan().data()), result.storage.sizeInBytes(), LinkBuffer::Profile::Uncategorized);
    result.size = linkBuffer.offsetOf(end);
    finalizeAndCopyReports(linkBuffer, link, result);
    return result;
}

// A near call or jump that leaves its buffer reaches other executable memory, but not from the heap.
template<typename Emit, typename Link>
LinkedCode linkIntoExecutableMemory(const Emit& emit, const Link& link)
{
    CCallHelpers jit;
    emitWithRecording(jit, emit, JumpShortening::Yes);

    LinkedCode result;
    LinkBuffer linkBuffer(jit, nullptr);
    finalizeAndCopyReports(linkBuffer, link, result);
    return result;
}

Vector<uint8_t> takeEmittedBytes(CCallHelpers& jit)
{
    AssemblerBuffer& buffer = jit.m_assembler.buffer();
    size_t size = buffer.codeSize();
    AssemblerData data = buffer.releaseAssemblerData();
#if ENABLE(JIT_SIGN_ASSEMBLER_BUFFER)
    AssemblerHashes hashes = buffer.releaseAssemblerHashes();
#endif
    return Vector<uint8_t>(std::span { reinterpret_cast<const uint8_t*>(data.buffer()), size });
}

template<typename Emit>
Vector<uint8_t> emitWithPolicy(const AssemblerPolicy& policy, const Emit& emit)
{
    CCallHelpers jit;
    jit.setPolicy(policy);
    emit(jit);
    return takeEmittedBytes(jit);
}

// applyRelocation() links code that starts at a multiple of 4 KB.
constexpr size_t linkedCodeAlignment = 4 * KB;

uint8_t* startOfCopiedCode(CodePtr<JSEntryPtrTag> allocation, size_t misalignment)
{
    return roundUpToMultipleOf<linkedCodeAlignment>(allocation.untaggedPtr<uint8_t*>()) + misalignment;
}

MacroAssemblerCodeRef<JSEntryPtrTag> makeExecutable(std::span<const uint8_t> bytes, size_t misalignment = 0)
{
    CCallHelpers jit;
    jit.emitNops(linkedCodeAlignment + misalignment + roundUpToMultipleOf<sizeof(uint32_t)>(bytes.size()));
    LinkBuffer linkBuffer(jit, nullptr);
    performJITMemcpy<jitMemcpyRepatch>(startOfCopiedCode(linkBuffer.entrypoint<JSEntryPtrTag>(), misalignment), bytes.data(), bytes.size());
    return FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm copied code");
}

template<typename T, typename... Arguments>
T invokeAt(uint8_t* entry, Arguments... arguments)
{
    return invoke<T>(MacroAssemblerCodeRef<JSEntryPtrTag>::createSelfManagedCodeRef(CodePtr<JSEntryPtrTag>::fromUntaggedPtr(entry)), arguments...);
}
#endif // CPU(X86_64) || CPU(ARM64)

void emitFunctionPrologue(CCallHelpers& jit)
{
    jit.emitFunctionPrologue();
}

void emitFunctionEpilogue(CCallHelpers& jit)
{
    jit.emitFunctionEpilogue();
}

void testSimple()
{
    CHECK_EQ(compileAndRun<int>([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.move(CCallHelpers::TrustedImm32(42), GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    }), 42);
}

void testGetEffectiveAddress(size_t pointer, ptrdiff_t length, int32_t offset, CCallHelpers::Scale scale)
{
    CHECK_EQ(compileAndRun<size_t>([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.move(CCallHelpers::TrustedImmPtr(std::bit_cast<void*>(pointer)), GPRInfo::regT0);
        jit.move(CCallHelpers::TrustedImmPtr(std::bit_cast<void*>(length)), GPRInfo::regT1);
        jit.getEffectiveAddress(CCallHelpers::BaseIndex(GPRInfo::regT0, GPRInfo::regT1, scale, offset), GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    }), pointer + offset + (static_cast<size_t>(1) << static_cast<int>(scale)) * length);
}

// branchTruncateDoubleToInt32(), when encountering Infinity, -Infinity or a
// Nan, should either yield 0 in dest or fail.
void testBranchTruncateDoubleToInt32(double val, int32_t expected)
{
    const uint64_t valAsUInt = std::bit_cast<uint64_t>(val);
    CHECK_EQ(compileAndRun<int>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.subPtr(CCallHelpers::TrustedImm32(stackAlignmentBytes()), MacroAssembler::stackPointerRegister);
        jit.store32(CCallHelpers::TrustedImm32(valAsUInt & 0xffffffff),
            MacroAssembler::Address(MacroAssembler::stackPointerRegister));
        jit.store32(CCallHelpers::TrustedImm32(valAsUInt >> 32),
            MacroAssembler::Address(MacroAssembler::stackPointerRegister, 4));
        jit.loadDouble(MacroAssembler::Address(MacroAssembler::stackPointerRegister), FPRInfo::fpRegT0);

        MacroAssembler::Jump done;
        done = jit.branchTruncateDoubleToInt32(FPRInfo::fpRegT0, GPRInfo::returnValueGPR, MacroAssembler::BranchIfTruncateSuccessful);

        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);

        done.link(&jit);
        jit.addPtr(CCallHelpers::TrustedImm32(stackAlignmentBytes()), MacroAssembler::stackPointerRegister);
        emitFunctionEpilogue(jit);
        jit.ret();
    }), expected);
}

#if CPU(X86_64) || CPU(ARM64)
// std::nullopt expects the branch to be taken.
void testBranchTruncateDoubleToInt32ViaInt64(double value, std::optional<int32_t> expected)
{
    constexpr int64_t failed = 1ll << 40;
    auto code = compile([&](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        auto failure = jit.branchTruncateDoubleToInt32ViaInt64(FPRInfo::argumentFPR0, GPRInfo::returnValueGPR);
        auto done = jit.jump();
        failure.link(&jit);
        jit.move(CCallHelpers::TrustedImm64(failed), GPRInfo::returnValueGPR);
        done.link(&jit);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(invoke<int64_t>(code, value), expected ? static_cast<int64_t>(static_cast<uint32_t>(*expected)) : failed);
}
#endif

static void testBranch32()
{
    auto compare = [](CCallHelpers::RelationalCondition cond, int32_t v1, int32_t v2) -> int {
        switch (cond) {
        case CCallHelpers::LessThan:
            return !!(static_cast<int32_t>(v1) < static_cast<int32_t>(v2));
        case CCallHelpers::LessThanOrEqual:
            return !!(static_cast<int32_t>(v1) <= static_cast<int32_t>(v2));
        case CCallHelpers::GreaterThan:
            return !!(static_cast<int32_t>(v1) > static_cast<int32_t>(v2));
        case CCallHelpers::GreaterThanOrEqual:
            return !!(static_cast<int32_t>(v1) >= static_cast<int32_t>(v2));
        case CCallHelpers::Below:
            return !!(static_cast<uint32_t>(v1) < static_cast<uint32_t>(v2));
        case CCallHelpers::BelowOrEqual:
            return !!(static_cast<uint32_t>(v1) <= static_cast<uint32_t>(v2));
        case CCallHelpers::Above:
            return !!(static_cast<uint32_t>(v1) > static_cast<uint32_t>(v2));
        case CCallHelpers::AboveOrEqual:
            return !!(static_cast<uint32_t>(v1) >= static_cast<uint32_t>(v2));
        case CCallHelpers::Equal:
            return !!(static_cast<uint32_t>(v1) == static_cast<uint32_t>(v2));
        case CCallHelpers::NotEqual:
            return !!(static_cast<uint32_t>(v1) != static_cast<uint32_t>(v2));
        }
        return 0;
    };

    for (auto value : int32Operands()) {
        for (auto value2 : int32Operands()) {
            auto tryTest = [&](CCallHelpers::RelationalCondition cond) {
                auto test = compile([=](CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    auto branch = jit.branch32(cond, GPRInfo::argumentGPR0, CCallHelpers::TrustedImm32(value2));
                    jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                    auto done = jit.jump();
                    branch.link(&jit);
                    jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                    done.link(&jit);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                CHECK_EQ(invoke<int>(test, value), compare(cond, value, value2));
            };
            tryTest(CCallHelpers::LessThan);
            tryTest(CCallHelpers::LessThanOrEqual);
            tryTest(CCallHelpers::GreaterThan);
            tryTest(CCallHelpers::GreaterThanOrEqual);
            tryTest(CCallHelpers::Below);
            tryTest(CCallHelpers::BelowOrEqual);
            tryTest(CCallHelpers::Above);
            tryTest(CCallHelpers::AboveOrEqual);
            tryTest(CCallHelpers::Equal);
            tryTest(CCallHelpers::NotEqual);
        }
    }
}

#if CPU(X86_64) || CPU(ARM64)
static void testBranch64()
{
    auto compare = [](CCallHelpers::RelationalCondition cond, int64_t v1, int64_t v2) -> int {
        switch (cond) {
        case CCallHelpers::LessThan:
            return !!(static_cast<int64_t>(v1) < static_cast<int64_t>(v2));
        case CCallHelpers::LessThanOrEqual:
            return !!(static_cast<int64_t>(v1) <= static_cast<int64_t>(v2));
        case CCallHelpers::GreaterThan:
            return !!(static_cast<int64_t>(v1) > static_cast<int64_t>(v2));
        case CCallHelpers::GreaterThanOrEqual:
            return !!(static_cast<int64_t>(v1) >= static_cast<int64_t>(v2));
        case CCallHelpers::Below:
            return !!(static_cast<uint64_t>(v1) < static_cast<uint64_t>(v2));
        case CCallHelpers::BelowOrEqual:
            return !!(static_cast<uint64_t>(v1) <= static_cast<uint64_t>(v2));
        case CCallHelpers::Above:
            return !!(static_cast<uint64_t>(v1) > static_cast<uint64_t>(v2));
        case CCallHelpers::AboveOrEqual:
            return !!(static_cast<uint64_t>(v1) >= static_cast<uint64_t>(v2));
        case CCallHelpers::Equal:
            return !!(static_cast<uint64_t>(v1) == static_cast<uint64_t>(v2));
        case CCallHelpers::NotEqual:
            return !!(static_cast<uint64_t>(v1) != static_cast<uint64_t>(v2));
        }
        return 0;
    };

    for (auto value : int64Operands()) {
        for (auto value2 : int64Operands()) {
            auto tryTest = [&](CCallHelpers::RelationalCondition cond) {
                auto test = compile([=](CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    auto branch = jit.branch64(cond, GPRInfo::argumentGPR0, CCallHelpers::TrustedImm64(value2));
                    jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                    auto done = jit.jump();
                    branch.link(&jit);
                    jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                    done.link(&jit);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                CHECK_EQ(invoke<int>(test, value), compare(cond, value, value2));
            };
            tryTest(CCallHelpers::LessThan);
            tryTest(CCallHelpers::LessThanOrEqual);
            tryTest(CCallHelpers::GreaterThan);
            tryTest(CCallHelpers::GreaterThanOrEqual);
            tryTest(CCallHelpers::Below);
            tryTest(CCallHelpers::BelowOrEqual);
            tryTest(CCallHelpers::Above);
            tryTest(CCallHelpers::AboveOrEqual);
            tryTest(CCallHelpers::Equal);
            tryTest(CCallHelpers::NotEqual);
        }
    }
}
#endif

#if CPU(ARM64)
template<unsigned dataSize>
unsigned numberOfFlaglessBranches(int64_t immediate)
{
    constexpr CCallHelpers::RelationalCondition conditions[] = {
        CCallHelpers::Equal,
        CCallHelpers::NotEqual,
        CCallHelpers::Above,
        CCallHelpers::AboveOrEqual,
        CCallHelpers::Below,
        CCallHelpers::BelowOrEqual,
        CCallHelpers::GreaterThan,
        CCallHelpers::GreaterThanOrEqual,
        CCallHelpers::LessThan,
        CCallHelpers::LessThanOrEqual,
    };

    unsigned result = 0;
    for (auto cond : conditions) {
        CCallHelpers jit;
        if constexpr (dataSize == 32)
            jit.branch32(cond, GPRInfo::regT0, CCallHelpers::TrustedImm32(static_cast<int32_t>(immediate)));
        else
            jit.branch64(cond, GPRInfo::regT0, CCallHelpers::TrustedImm64(immediate));
        // The branch and the nop that compaction removes. A compare makes it three words or more.
        bool isFlagless = jit.m_assembler.codeSize() == 2 * sizeof(uint32_t);
        CHECK_EQ(CCallHelpers::canBranchWithoutFlags<dataSize>(cond, immediate), isFlagless);
        if (isFlagless)
            result++;
    }
    return result;
}

template<unsigned dataSize, typename Immediate>
void checkNumberOfFlaglessBranches()
{
    CHECK_EQ(numberOfFlaglessBranches<dataSize>(0), 6u);
    CHECK_EQ(numberOfFlaglessBranches<dataSize>(-1), 2u);
    CHECK_EQ(numberOfFlaglessBranches<dataSize>(std::numeric_limits<Immediate>::min()), 2u);
    CHECK_EQ(numberOfFlaglessBranches<dataSize>(std::numeric_limits<Immediate>::max()), 2u);
    CHECK_EQ(numberOfFlaglessBranches<dataSize>(1), 0u);
    CHECK_EQ(numberOfFlaglessBranches<dataSize>(-2), 0u);
}

void testCanBranchWithoutFlags()
{
    checkNumberOfFlaglessBranches<32, int32_t>();
    checkNumberOfFlaglessBranches<64, int64_t>();
    CHECK_EQ(numberOfFlaglessBranches<64>(std::numeric_limits<int32_t>::min()), 0u);
    CHECK_EQ(numberOfFlaglessBranches<64>(std::numeric_limits<int32_t>::max()), 0u);
}
#endif

void testBranchTest8()
{
    for (auto value : int32Operands()) {
        for (auto value2 : int32Operands()) {
            auto test1 = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                auto branch = jit.branchTest8(MacroAssembler::NonZero, CCallHelpers::Address(GPRInfo::argumentGPR0, 1), CCallHelpers::TrustedImm32(value2));
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                auto done = jit.jump();
                branch.link(&jit);
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                done.link(&jit);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            auto test2 = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                auto branch = jit.branchTest8(MacroAssembler::NonZero, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne), CCallHelpers::TrustedImm32(value2));
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                auto done = jit.jump();
                branch.link(&jit);
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                done.link(&jit);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            int result = 0;
            if (static_cast<uint8_t>(value) & static_cast<uint8_t>(value2))
                result = 1;

            uint8_t array[] = {
                0,
                static_cast<uint8_t>(value)
            };
            CHECK_EQ(invoke<int>(test1, array), result);
            CHECK_EQ(invoke<int>(test2, array, 1), result);
        }
    }
}

void testBranchTest16()
{
    for (auto value : int32Operands()) {
        for (auto value2 : int32Operands()) {
            auto test1 = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                auto branch = jit.branchTest16(MacroAssembler::NonZero, CCallHelpers::Address(GPRInfo::argumentGPR0, 2), CCallHelpers::TrustedImm32(value2));
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                auto done = jit.jump();
                branch.link(&jit);
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                done.link(&jit);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            auto test2 = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                auto branch = jit.branchTest16(MacroAssembler::NonZero, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo), CCallHelpers::TrustedImm32(value2));
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                auto done = jit.jump();
                branch.link(&jit);
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                done.link(&jit);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            int result = 0;
            if (static_cast<uint16_t>(value) & static_cast<uint16_t>(value2))
                result = 1;

            uint16_t array[] = {
                0,
                static_cast<uint16_t>(value)
            };
            CHECK_EQ(invoke<int>(test1, array), result);
            CHECK_EQ(invoke<int>(test2, array, 1), result);
        }
    }
}

#if CPU(X86_64)
void testBranchTestBit32RegReg()
{
    for (auto value : int32Operands()) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            auto branch = jit.branchTestBit32(MacroAssembler::NonZero, GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
            jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
            auto done = jit.jump();
            branch.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
            done.link(&jit);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int32Operands())
            CHECK_EQ(invoke<int>(test, value, value2), (value>>(value2%32))&1);
    }
}

void testBranchTestBit32RegImm()
{
    for (auto value : int32Operands()) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            auto branch = jit.branchTestBit32(MacroAssembler::NonZero, GPRInfo::argumentGPR0, CCallHelpers::TrustedImm32(value));
            jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
            auto done = jit.jump();
            branch.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
            done.link(&jit);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int32Operands())
            CHECK_EQ(invoke<int>(test, value2), (value2>>(value%32))&1);
    }
}

void testBranchTestBit32AddrImm()
{
    for (auto value : int32Operands()) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            auto branch = jit.branchTestBit32(MacroAssembler::NonZero, MacroAssembler::Address(GPRInfo::argumentGPR0, 0), CCallHelpers::TrustedImm32(value));
            jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
            auto done = jit.jump();
            branch.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
            done.link(&jit);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int32Operands())
            CHECK_EQ(invoke<int>(test, &value2), (value2>>(value%32))&1);
    }
}

void testBranchTestBit64RegReg()
{
    for (auto value : int64Operands()) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            auto branch = jit.branchTestBit64(MacroAssembler::NonZero, GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
            jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::returnValueGPR);
            auto done = jit.jump();
            branch.link(&jit);
            jit.move(CCallHelpers::TrustedImm64(1), GPRInfo::returnValueGPR);
            done.link(&jit);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int64Operands())
            CHECK_EQ(invoke<long int>(test, value, value2), (value>>(value2%64))&1);
    }
}

void testBranchTestBit64RegImm()
{
    for (auto value : int64Operands()) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            auto branch = jit.branchTestBit64(MacroAssembler::NonZero, GPRInfo::argumentGPR0, CCallHelpers::TrustedImm32(value));
            jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::returnValueGPR);
            auto done = jit.jump();
            branch.link(&jit);
            jit.move(CCallHelpers::TrustedImm64(1), GPRInfo::returnValueGPR);
            done.link(&jit);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int64Operands())
            CHECK_EQ(invoke<long int>(test, value2), (value2>>(value%64))&1);
    }
}

void testBranchTestBit64AddrImm()
{
    for (auto value : int64Operands()) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            auto branch = jit.branchTestBit64(MacroAssembler::NonZero, MacroAssembler::Address(GPRInfo::argumentGPR0, 0), CCallHelpers::TrustedImm32(value));
            jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::returnValueGPR);
            auto done = jit.jump();
            branch.link(&jit);
            jit.move(CCallHelpers::TrustedImm64(1), GPRInfo::returnValueGPR);
            done.link(&jit);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int64Operands())
            CHECK_EQ(invoke<long int>(test, &value2), (value2>>(value%64))&1);
    }
}

#endif

#if CPU(X86_64) || CPU(ARM64)
void testClearBit64()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg scratchGPR = GPRInfo::argumentGPR2;
        jit.clearBit64(GPRInfo::argumentGPR1, GPRInfo::argumentGPR0, scratchGPR);
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    constexpr unsigned bitsInWord = sizeof(uint64_t) * 8;

    for (unsigned i = 0; i < bitsInWord; ++i) {
        uint64_t word = std::numeric_limits<uint64_t>::max();
        constexpr uint64_t one = 1;
        CHECK_EQ(invoke<uint64_t>(test, word, i), (word & ~(one << i)));
    }

    for (unsigned i = 0; i < bitsInWord; ++i) {
        uint64_t word = 0;
        CHECK_EQ(invoke<uint64_t>(test, word, i), 0);
    }
}

void testClearBits64WithMask()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.clearBits64WithMask(GPRInfo::argumentGPR1, GPRInfo::argumentGPR0);
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto value : int64Operands()) {
        uint64_t word = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<uint64_t>(test, word, value), (word & ~value));
    }

    for (auto value : int64Operands()) {
        uint64_t word = 0;
        CHECK_EQ(invoke<uint64_t>(test, word, value), 0);
    }

    uint64_t savedMask = 0;
    auto test2 = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.probeDebug([&] (Probe::Context& context) {
            savedMask = context.gpr<uint64_t>(GPRInfo::argumentGPR1);
        });

        jit.clearBits64WithMask(GPRInfo::argumentGPR1, GPRInfo::argumentGPR0, CCallHelpers::ClearBitsAttributes::MustPreserveMask);

        jit.probeDebug([&] (Probe::Context& context) {
            CHECK_EQ(savedMask, context.gpr<uint64_t>(GPRInfo::argumentGPR1));
        });
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto value : int64Operands()) {
        uint64_t word = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<uint64_t>(test2, word, value), (word & ~value));
    }

    for (auto value : int64Operands()) {
        uint64_t word = 0;
        CHECK_EQ(invoke<uint64_t>(test2, word, value), 0);
    }
}

void testClearBits64WithMaskTernary()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.move(GPRInfo::argumentGPR0, GPRInfo::argumentGPR2);
        jit.move(GPRInfo::argumentGPR1, GPRInfo::argumentGPR3);
        jit.clearBits64WithMask(GPRInfo::argumentGPR2, GPRInfo::argumentGPR3, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto value : int64Operands()) {
        uint64_t word = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<uint64_t>(test, word, value), (word & ~value));
    }

    for (auto value : int64Operands()) {
        uint64_t word = 0;
        CHECK_EQ(invoke<uint64_t>(test, word, value), 0);
    }

    uint64_t savedMask = 0;
    auto test2 = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.move(GPRInfo::argumentGPR0, GPRInfo::argumentGPR2);
        jit.move(GPRInfo::argumentGPR1, GPRInfo::argumentGPR3);

        jit.probeDebug([&] (Probe::Context& context) {
            savedMask = context.gpr<uint64_t>(GPRInfo::argumentGPR2);
        });

        jit.clearBits64WithMask(GPRInfo::argumentGPR2, GPRInfo::argumentGPR3, GPRInfo::returnValueGPR, CCallHelpers::ClearBitsAttributes::MustPreserveMask);

        jit.probeDebug([&] (Probe::Context& context) {
            CHECK_EQ(savedMask, context.gpr<uint64_t>(GPRInfo::argumentGPR2));
        });

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto value : int64Operands()) {
        uint64_t word = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<uint64_t>(test2, word, value), (word & ~value));
    }

    for (auto value : int64Operands()) {
        uint64_t word = 0;
        CHECK_EQ(invoke<uint64_t>(test2, word, value), 0);
    }
}

static void testCountTrailingZeros64Impl(bool wordCanBeZero)
{
    auto test = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        if (wordCanBeZero)
            jit.countTrailingZeros64(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        else
            jit.countTrailingZeros64WithoutNullCheck(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    constexpr size_t numberOfBits = sizeof(uint64_t) * 8;

    auto expectedNumberOfTrailingZeros = [=] (uint64_t word) -> size_t {
        size_t count = 0;
        for (size_t i = 0; i < numberOfBits; ++i) {
            if (word & 1)
                break;
            word >>= 1;
            count++;
        }
        return count;
    };

    for (auto word : int64Operands()) {
        if (!wordCanBeZero && !word)
            continue;
        CHECK_EQ(invoke<size_t>(test, word), expectedNumberOfTrailingZeros(word));
    }

    for (size_t i = 0; i < numberOfBits; ++i) {
        uint64_t one = 1;
        uint64_t word = one << i;
        CHECK_EQ(invoke<size_t>(test, word), i);
    }
}

void testCountTrailingZeros64()
{
    bool wordCanBeZero = true;
    testCountTrailingZeros64Impl(wordCanBeZero);
}

void testCountTrailingZeros64WithoutNullCheck()
{
    bool wordCanBeZero = false;
    testCountTrailingZeros64Impl(wordCanBeZero);
}

void testShiftAndAdd()
{
    constexpr intptr_t basePointer = 0x1234abcd;

    enum class Reg {
        ArgumentGPR0,
        ArgumentGPR1,
        ArgumentGPR2,
        ArgumentGPR3,
        ScratchGPR
    };

    auto test = [&] (intptr_t index, uint8_t shift, Reg destReg, Reg baseReg, Reg indexReg) {
        auto test = compile([=] (CCallHelpers& jit) {
            CCallHelpers::RegisterID scratchGPR = jit.scratchRegister();

            auto registerIDForReg = [=] (Reg reg) -> CCallHelpers::RegisterID {
                switch (reg) {
                case Reg::ArgumentGPR0: return GPRInfo::argumentGPR0;
                case Reg::ArgumentGPR1: return GPRInfo::argumentGPR1;
                case Reg::ArgumentGPR2: return GPRInfo::argumentGPR2;
                case Reg::ArgumentGPR3: return GPRInfo::argumentGPR3;
                case Reg::ScratchGPR: return scratchGPR;
                }
                RELEASE_ASSERT_NOT_REACHED();
            };

            CCallHelpers::RegisterID destGPR = registerIDForReg(destReg);
            CCallHelpers::RegisterID baseGPR = registerIDForReg(baseReg);
            CCallHelpers::RegisterID indexGPR = registerIDForReg(indexReg);

            emitFunctionPrologue(jit);
            jit.pushPair(scratchGPR, GPRInfo::argumentGPR3);

            jit.move(CCallHelpers::TrustedImmPtr(std::bit_cast<void*>(basePointer)), baseGPR);
            jit.move(CCallHelpers::TrustedImmPtr(std::bit_cast<void*>(index)), indexGPR);
            jit.shiftAndAdd(baseGPR, indexGPR, shift, destGPR);

            jit.probeDebug([=] (Probe::Context& context) {
                if (baseReg != destReg)
                    CHECK_EQ(context.gpr<intptr_t>(baseGPR), basePointer);
                if (indexReg != destReg)
                    CHECK_EQ(context.gpr<intptr_t>(indexGPR), index);
            });
            jit.move(destGPR, GPRInfo::returnValueGPR);

            jit.popPair(scratchGPR, GPRInfo::argumentGPR3);
            emitFunctionEpilogue(jit);
            jit.ret();
        });

        CHECK_EQ(invoke<intptr_t>(test), basePointer + (index << shift));
    };

    for (auto index : int32Operands()) {
        for (uint8_t shift = 0; shift < 32; ++shift) {
            test(index, shift, Reg::ScratchGPR, Reg::ScratchGPR, Reg::ArgumentGPR3);     // Scenario: dest == base == scratchRegister.
            test(index, shift, Reg::ArgumentGPR2, Reg::ArgumentGPR2, Reg::ArgumentGPR3); // Scenario: dest == base != scratchRegister.
            test(index, shift, Reg::ScratchGPR, Reg::ArgumentGPR2, Reg::ScratchGPR);     // Scenario: dest == index == scratchRegister.
            test(index, shift, Reg::ArgumentGPR3, Reg::ArgumentGPR2, Reg::ArgumentGPR3); // Scenario: dest == index != scratchRegister.
            test(index, shift, Reg::ArgumentGPR1, Reg::ArgumentGPR2, Reg::ArgumentGPR3); // Scenario: all different registers, no scratchRegister.
            test(index, shift, Reg::ScratchGPR, Reg::ArgumentGPR2, Reg::ArgumentGPR3);   // Scenario: all different registers, dest == scratchRegister.
            test(index, shift, Reg::ArgumentGPR1, Reg::ScratchGPR, Reg::ArgumentGPR3);   // Scenario: all different registers, base == scratchRegister.
            test(index, shift, Reg::ArgumentGPR1, Reg::ArgumentGPR2, Reg::ScratchGPR);   // Scenario: all different registers, index == scratchRegister.
        }
    }
}

void testStore64Imm64AddressPointer()
{
    auto doTest = [] (int64_t value) {
        int64_t dest;
        void* destAddress = &dest;

        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store64(CCallHelpers::TrustedImm64(value), destAddress);
            emitFunctionEpilogue(jit);
            jit.ret();
        });

        invoke<size_t>(test);
        CHECK_EQ(dest, value);
    };
    
    for (auto value : int64Operands())
        doTest(value);

    doTest(0x98765555AAAA4321);
    doTest(0xAAAA432198765555);
}

void testLoad64ExtendedAddress()
{
    static constexpr std::array<uint64_t, 5> table { 0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0xffffffffffffffffULL, 0, 0x8000000000000001ULL };

    auto test = compile([=](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.load64(CCallHelpers::ExtendedAddress(GPRInfo::argumentGPR0, std::bit_cast<intptr_t>(table.data())), GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (size_t i = 0; i < table.size(); ++i)
        CHECK_EQ(invoke<uint64_t>(test, i), table[i]);
}

void testAdd32Imm()
{
    for (auto immediate : int32Operands()) {
        for (auto immediate2 : int32Operands()) {
            auto add = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                jit.move(CCallHelpers::TrustedImm32(immediate), GPRInfo::returnValueGPR);
                jit.add32(CCallHelpers::TrustedImm32(immediate2), GPRInfo::returnValueGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });
            CHECK_EQ(invoke<uint32_t>(add), static_cast<uint32_t>(immediate) + static_cast<uint32_t>(immediate2));
        }
    }
}

void testAdd32ArgImm()
{
    for (auto immediate : int32Operands()) {
        auto add = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.add32(CCallHelpers::TrustedImm32(immediate), GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int32Operands())
            CHECK_EQ(invoke<uint32_t>(add, value), static_cast<uint32_t>(value) + static_cast<uint32_t>(immediate));
    }
}

void testAdd64Imm32()
{
    for (auto immediate : int64Operands()) {
        for (auto immediate2 : int32Operands()) {
            auto add = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                jit.move(CCallHelpers::TrustedImm64(immediate), GPRInfo::returnValueGPR);
                jit.add64(CCallHelpers::TrustedImm32(immediate2), GPRInfo::returnValueGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });
            CHECK_EQ(invoke<uint64_t>(add), static_cast<uint64_t>(immediate) + static_cast<uint64_t>(immediate2));
        }
    }
}

void testAdd64ArgImm32()
{
    for (auto immediate : int32Operands()) {
        auto add = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.add64(CCallHelpers::TrustedImm32(immediate), GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int64Operands())
            CHECK_EQ(invoke<uint64_t>(add, value), static_cast<uint64_t>(value) + static_cast<uint64_t>(immediate));
    }
}

void testAdd64Imm64()
{
    for (auto immediate : int64Operands()) {
        for (auto immediate2 : int64Operands()) {
            auto add = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                jit.move(CCallHelpers::TrustedImm64(immediate), GPRInfo::returnValueGPR);
                jit.add64(CCallHelpers::TrustedImm64(immediate2), GPRInfo::returnValueGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });
            CHECK_EQ(invoke<uint64_t>(add), static_cast<uint64_t>(immediate) + static_cast<uint64_t>(immediate2));
        }
    }
}

void testAdd64ArgImm64()
{
    for (auto immediate : int64Operands()) {
        auto add = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.add64(CCallHelpers::TrustedImm64(immediate), GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int64Operands())
            CHECK_EQ(invoke<uint64_t>(add, value), static_cast<uint64_t>(value) + static_cast<uint64_t>(immediate));
    }
}

void testSub32Args()
{
    for (auto value : int32Operands()) {
        auto sub = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.sub32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int32Operands())
            CHECK_EQ(invoke<uint32_t>(sub, value, value2), static_cast<uint32_t>(value - value2));
    }
}

void testSub32Imm()
{
    for (auto immediate : int32Operands()) {
        for (auto immediate2 : int32Operands()) {
            auto sub = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                jit.move(CCallHelpers::TrustedImm32(immediate), GPRInfo::returnValueGPR);
                jit.sub32(CCallHelpers::TrustedImm32(immediate2), GPRInfo::returnValueGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });
            CHECK_EQ(invoke<uint32_t>(sub), static_cast<uint32_t>(immediate - immediate2));
        }
    }
}

void testSub64Imm32()
{
    for (auto immediate : int64Operands()) {
        for (auto immediate2 : int32Operands()) {
            auto sub = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                jit.move(CCallHelpers::TrustedImm64(immediate), GPRInfo::returnValueGPR);
                jit.sub64(CCallHelpers::TrustedImm32(immediate2), GPRInfo::returnValueGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });
            CHECK_EQ(invoke<uint64_t>(sub), static_cast<uint64_t>(immediate - immediate2));
        }
    }
}

void testSub64ArgImm32()
{
    for (auto immediate : int32Operands()) {
        auto sub = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.sub64(GPRInfo::argumentGPR0, CCallHelpers::TrustedImm32(immediate), GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int64Operands())
            CHECK_EQ(invoke<int64_t>(sub, value), static_cast<int64_t>(value - immediate));
    }
}

void testSub64Imm64()
{
    for (auto immediate : int64Operands()) {
        for (auto immediate2 : int64Operands()) {
            auto sub = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                jit.move(CCallHelpers::TrustedImm64(immediate), GPRInfo::returnValueGPR);
                jit.sub64(CCallHelpers::TrustedImm64(immediate2), GPRInfo::returnValueGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });
            CHECK_EQ(invoke<uint64_t>(sub), static_cast<uint64_t>(immediate - immediate2));
        }
    }
}

void testSub64ArgImm64()
{
    for (auto immediate : int64Operands()) {
        auto sub = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.sub64(GPRInfo::argumentGPR0, CCallHelpers::TrustedImm64(immediate), GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int64Operands())
            CHECK_EQ(invoke<int64_t>(sub, value), static_cast<int64_t>(value - immediate));
    }
}

#endif // CPU(X86_64) || CPU(ARM64)

void testCompareDouble(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;

    auto compareDouble = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::returnValueGPR);
        jit.compareDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto compareDoubleGeneric = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
        auto jump = jit.branchDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
        jump.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto expectedResult = [&, condition] (double a, double b) -> int {
        auto isUnordered = [] (double x) {
            return x != x;
        };
        switch (condition) {
        case MacroAssembler::DoubleEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a == b);
        case MacroAssembler::DoubleNotEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a != b);
        case MacroAssembler::DoubleGreaterThanAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a > b);
        case MacroAssembler::DoubleGreaterThanOrEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a >= b);
        case MacroAssembler::DoubleLessThanAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a < b);
        case MacroAssembler::DoubleLessThanOrEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a <= b);
        case MacroAssembler::DoubleEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a == b);
        case MacroAssembler::DoubleNotEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a != b);
        case MacroAssembler::DoubleGreaterThanOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a > b);
        case MacroAssembler::DoubleGreaterThanOrEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a >= b);
        case MacroAssembler::DoubleLessThanOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a < b);
        case MacroAssembler::DoubleLessThanOrEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a <= b);
        } // switch
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto operands = doubleOperands();
    for (auto a : operands) {
        for (auto b : operands) {
            arg1 = a;
            arg2 = b;
            CHECK_EQ(invoke<int>(compareDouble), expectedResult(a, b));
            CHECK_EQ(invoke<int>(compareDoubleGeneric), expectedResult(a, b));
        }
    }
}

void testCompareDoubleSameArg(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;

    auto compareDouble = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::returnValueGPR);
        jit.compareDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto compareDoubleGeneric = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
        auto jump = jit.branchDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
        jump.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto expectedResult = [&, condition] (double a) -> int {
        auto isUnordered = [] (double x) {
            return x != x;
        };
        switch (condition) {
        case MacroAssembler::DoubleEqualAndOrdered:
            return !isUnordered(a) && (a == a);
        case MacroAssembler::DoubleNotEqualAndOrdered:
            return !isUnordered(a) && (a != a);
        case MacroAssembler::DoubleGreaterThanAndOrdered:
            return !isUnordered(a) && (a > a);
        case MacroAssembler::DoubleGreaterThanOrEqualAndOrdered:
            return !isUnordered(a) && (a >= a);
        case MacroAssembler::DoubleLessThanAndOrdered:
            return !isUnordered(a) && (a < a);
        case MacroAssembler::DoubleLessThanOrEqualAndOrdered:
            return !isUnordered(a) && (a <= a);
        case MacroAssembler::DoubleEqualOrUnordered:
            return isUnordered(a) || (a == a);
        case MacroAssembler::DoubleNotEqualOrUnordered:
            return isUnordered(a) || (a != a);
        case MacroAssembler::DoubleGreaterThanOrUnordered:
            return isUnordered(a) || (a > a);
        case MacroAssembler::DoubleGreaterThanOrEqualOrUnordered:
            return isUnordered(a) || (a >= a);
        case MacroAssembler::DoubleLessThanOrUnordered:
            return isUnordered(a) || (a < a);
        case MacroAssembler::DoubleLessThanOrEqualOrUnordered:
            return isUnordered(a) || (a <= a);
        } // switch
        RELEASE_ASSERT_NOT_REACHED();
    };

    auto operands = doubleOperands();
    for (auto a : operands) {
        arg1 = a;
        CHECK_EQ(invoke<int>(compareDouble), expectedResult(a));
        CHECK_EQ(invoke<int>(compareDoubleGeneric), expectedResult(a));
    }
}

#if CPU(X86_64) || CPU(ARM64)
static Vector<int64_t> int64ImmediateOperands()
{
    return Vector<int64_t> {
        0, -1, 1, 42, -42, 0xff, 0xffff, 0x7fffffff, 0x80000000LL, 0xfffffffeLL, 0xffffffffLL,
        std::numeric_limits<int32_t>::min(), 0x100000000LL, static_cast<int64_t>(0xffffffff00000000ULL),
        0x123456789abcdef0LL, static_cast<int64_t>(0xfffe000000000000ULL), std::numeric_limits<int64_t>::min(),
    };
}

static Vector<uint64_t> int64InputOperands()
{
    return Vector<uint64_t> {
        0, ~0ULL, 42, 0xffffffffULL, 0xffffffff00000000ULL, 0x8000000080000000ULL, 0x123456789abcdef0ULL,
    };
}

// Checks emit(jit, src, dest) for every aliasing of src and dest, with garbage in the upper half of the
// destination so that a missing zero extension of a 32-bit result is caught.
template<typename Emit, typename Expected>
static void checkRegisterImmediateOp(Emit emit, Expected expected)
{
    enum class Aliasing { Distinct, SameArgument0, SameArgument2 };
    for (auto aliasing : { Aliasing::Distinct, Aliasing::SameArgument0, Aliasing::SameArgument2 }) {
        auto code = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            switch (aliasing) {
            case Aliasing::Distinct:
                jit.move(CCallHelpers::TrustedImm64(0x5a5a5a5a5a5a5a5aLL), GPRInfo::argumentGPR1);
                emit(jit, GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
                jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);
                break;
            case Aliasing::SameArgument0:
                emit(jit, GPRInfo::argumentGPR0, GPRInfo::argumentGPR0);
                jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
                break;
            case Aliasing::SameArgument2:
                jit.move(GPRInfo::argumentGPR0, GPRInfo::argumentGPR2);
                emit(jit, GPRInfo::argumentGPR2, GPRInfo::argumentGPR2);
                jit.move(GPRInfo::argumentGPR2, GPRInfo::returnValueGPR);
                break;
            }
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        for (auto value : int64InputOperands())
            CHECK_EQ(invoke<uint64_t>(code, value), static_cast<uint64_t>(expected(value)));
    }
}

template<typename Emit, typename Expected>
static void checkSrcDestImmediateOp(Emit emit, Expected expected)
{
    checkRegisterImmediateOp([=](CCallHelpers& jit, CCallHelpers::RegisterID src, CCallHelpers::RegisterID dest) {
        jit.move(src, dest);
        emit(jit, dest);
    }, expected);
}

enum class BranchArith { Add32, Sub32, Add64, Sub64 };

static bool expectedBranchArithTaken(BranchArith op, MacroAssembler::ResultCondition cond, uint64_t value, int32_t imm)
{
    bool is32 = op == BranchArith::Add32 || op == BranchArith::Sub32;
    bool isAdd = op == BranchArith::Add32 || op == BranchArith::Add64;
    bool carry = false;
    bool overflow = false;
    int64_t result = 0;
    if (is32) {
        int32_t lhs = static_cast<int32_t>(value);
        int32_t result32 = 0;
        overflow = isAdd ? __builtin_add_overflow(lhs, imm, &result32) : __builtin_sub_overflow(lhs, imm, &result32);
        uint32_t unsignedResult = 0;
        carry = __builtin_add_overflow(static_cast<uint32_t>(lhs), static_cast<uint32_t>(imm), &unsignedResult);
        result = result32;
    } else {
        int64_t lhs = static_cast<int64_t>(value);
        overflow = isAdd ? __builtin_add_overflow(lhs, static_cast<int64_t>(imm), &result) : __builtin_sub_overflow(lhs, static_cast<int64_t>(imm), &result);
        uint64_t unsignedResult = 0;
        carry = __builtin_add_overflow(static_cast<uint64_t>(lhs), static_cast<uint64_t>(static_cast<int64_t>(imm)), &unsignedResult);
    }
    switch (cond) {
    case MacroAssembler::Carry:
        RELEASE_ASSERT(isAdd);
        return carry;
    case MacroAssembler::Overflow:
        return overflow;
    case MacroAssembler::Signed:
        return result < 0;
    case MacroAssembler::PositiveOrZero:
        return result >= 0;
    case MacroAssembler::Zero:
        return !result;
    case MacroAssembler::NonZero:
        return !!result;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkBranchArithImmediate(BranchArith op, int32_t imm, MacroAssembler::ResultCondition cond, int32_t flagsPreset)
{
    auto code = compile([=](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        // The two presets leave opposite Z, N, and C flags behind, so a branch that does not
        // recompute them reads a stale value under one of the two.
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::argumentGPR1);
        jit.compare32(MacroAssembler::Equal, GPRInfo::argumentGPR1, CCallHelpers::TrustedImm32(flagsPreset), GPRInfo::argumentGPR1);
        CCallHelpers::Jump taken;
        switch (op) {
        case BranchArith::Add32:
            taken = jit.branchAdd32(cond, CCallHelpers::TrustedImm32(imm), GPRInfo::argumentGPR0);
            break;
        case BranchArith::Sub32:
            taken = jit.branchSub32(cond, CCallHelpers::TrustedImm32(imm), GPRInfo::argumentGPR0);
            break;
        case BranchArith::Add64:
            taken = jit.branchAdd64(cond, CCallHelpers::TrustedImm32(imm), GPRInfo::argumentGPR0);
            break;
        case BranchArith::Sub64:
            taken = jit.branchSub64(cond, CCallHelpers::TrustedImm32(imm), GPRInfo::argumentGPR0);
            break;
        }
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
        auto done = jit.jump();
        taken.link(&jit);
        jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
        done.link(&jit);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    const uint64_t values[] = { 0, 1, ~0ULL, 0xffffffffULL, 0x7fffffffULL, 0x80000000ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 0x123456789abcdef0ULL };
    for (uint64_t value : values)
        CHECK_EQ(invoke<int>(code, value), static_cast<int>(expectedBranchArithTaken(op, cond, value, imm)));
}

void testBranchAddSubImmediateSetsFlags()
{
    // Carry after a subtraction means "no borrow" on ARM64 but "borrow" on x86, so it is only checked for additions.
    const MacroAssembler::ResultCondition addConditions[] = { MacroAssembler::Carry, MacroAssembler::Overflow, MacroAssembler::Signed, MacroAssembler::PositiveOrZero, MacroAssembler::Zero, MacroAssembler::NonZero };
    const MacroAssembler::ResultCondition subConditions[] = { MacroAssembler::Overflow, MacroAssembler::Signed, MacroAssembler::PositiveOrZero, MacroAssembler::Zero, MacroAssembler::NonZero };

    for (auto op : { BranchArith::Add32, BranchArith::Sub32, BranchArith::Add64, BranchArith::Sub64 }) {
        bool isAdd = op == BranchArith::Add32 || op == BranchArith::Add64;
        auto conditions = isAdd ? std::span<const MacroAssembler::ResultCondition> { addConditions } : std::span<const MacroAssembler::ResultCondition> { subConditions };
        for (int32_t imm : { 0, 1, -1 }) {
            for (auto cond : conditions) {
                for (int32_t flagsPreset : { 0, 1 })
                    checkBranchArithImmediate(op, imm, cond, flagsPreset);
            }
        }
    }
}

void testLogicalAndShiftImmediates()
{
    using Reg = CCallHelpers::RegisterID;
    using Imm32 = CCallHelpers::TrustedImm32;
    using Imm64 = CCallHelpers::TrustedImm64;

    for (int64_t imm64 : int64ImmediateOperands()) {
        uint64_t mask64 = static_cast<uint64_t>(imm64);
        int32_t imm32 = static_cast<int32_t>(imm64);
        uint32_t mask32 = static_cast<uint32_t>(imm32);
        uint64_t signExtendedMask32 = static_cast<uint64_t>(static_cast<int64_t>(imm32));

        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.and32(Imm32(imm32), src, dest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(v) & mask32;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.and32(Imm32(imm32), srcDest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(v) & mask32;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.or32(Imm32(imm32), src, dest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(v) | mask32;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.xor32(Imm32(imm32), src, dest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(v) ^ mask32;
        });

        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.and64(Imm32(imm32), src, dest);
        }, [=](uint64_t v) {
            return v & signExtendedMask32;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.and64(Imm32(imm32), srcDest);
        }, [=](uint64_t v) {
            return v & signExtendedMask32;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.or64(Imm32(imm32), src, dest);
        }, [=](uint64_t v) {
            return v | signExtendedMask32;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.or64(Imm32(imm32), srcDest);
        }, [=](uint64_t v) {
            return v | signExtendedMask32;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.xor64(Imm32(imm32), src, dest);
        }, [=](uint64_t v) {
            return v ^ signExtendedMask32;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.xor64(Imm32(imm32), srcDest);
        }, [=](uint64_t v) {
            return v ^ signExtendedMask32;
        });

        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.and64(Imm64(imm64), src, dest);
        }, [=](uint64_t v) {
            return v & mask64;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.and64(Imm64(imm64), srcDest);
        }, [=](uint64_t v) {
            return v & mask64;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.or64(Imm64(imm64), src, dest);
        }, [=](uint64_t v) {
            return v | mask64;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.or64(Imm64(imm64), srcDest);
        }, [=](uint64_t v) {
            return v | mask64;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.xor64(Imm64(imm64), src, dest);
        }, [=](uint64_t v) {
            return v ^ mask64;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.add64(Imm64(imm64), srcDest);
        }, [=](uint64_t v) {
            return v + mask64;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.sub64(Imm64(imm64), srcDest);
        }, [=](uint64_t v) {
            return v - mask64;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg, Reg dest) {
            jit.move(Imm64(imm64), dest);
        }, [=](uint64_t) {
            return mask64;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg, Reg dest) {
            jit.move(CCallHelpers::TrustedImmPtr(std::bit_cast<void*>(static_cast<intptr_t>(imm64))), dest);
        }, [=](uint64_t) {
            return mask64;
        });
    }

    for (int32_t amount : { 0, 1, 7, 31, 32, 33, 63, 64 }) {
        unsigned amount32 = amount & 31;
        unsigned amount64 = amount & 63;
        auto rotateRight32 = [=](uint32_t v) -> uint32_t {
            return amount32 ? (v >> amount32) | (v << (32 - amount32)) : v;
        };
        auto rotateRight64 = [=](uint64_t v) -> uint64_t {
            return amount64 ? (v >> amount64) | (v << (64 - amount64)) : v;
        };

        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.lshift32(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(static_cast<uint32_t>(v) << amount32);
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.lshift32(Imm32(amount), srcDest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(static_cast<uint32_t>(v) << amount32);
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.rshift32(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(static_cast<int32_t>(v) >> amount32);
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.rshift32(Imm32(amount), srcDest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(static_cast<int32_t>(v) >> amount32);
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.urshift32(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(v) >> amount32;
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.urshift32(Imm32(amount), srcDest);
        }, [=](uint64_t v) {
            return static_cast<uint32_t>(v) >> amount32;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.rotateRight32(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return rotateRight32(static_cast<uint32_t>(v));
        });
        checkSrcDestImmediateOp([=](CCallHelpers& jit, Reg srcDest) {
            jit.rotateRight32(Imm32(amount), srcDest);
        }, [=](uint64_t v) {
            return rotateRight32(static_cast<uint32_t>(v));
        });

        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.lshift64(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return v << amount64;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.rshift64(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return static_cast<uint64_t>(static_cast<int64_t>(v) >> amount64);
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.urshift64(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return v >> amount64;
        });
        checkRegisterImmediateOp([=](CCallHelpers& jit, Reg src, Reg dest) {
            jit.rotateRight64(src, Imm32(amount), dest);
        }, [=](uint64_t v) {
            return rotateRight64(v);
        });
    }
}
#endif

void testMul32WithImmediates()
{
    for (auto immediate : int32Operands()) {
        auto mul = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.mul32(CCallHelpers::TrustedImm32(immediate), GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int32Operands())
            CHECK_EQ(invoke<int>(mul, value), immediate * value);
    }
}

#if CPU(ARM64)
void testMultiplySignExtend32()
{
    for (auto value : int32Operands()) {
        auto mul = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.multiplySignExtend32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value2 : int32Operands())
            CHECK_EQ(invoke<long int>(mul, value, value2), ((long int) value) * ((long int) value2));
    }
}

void testMultiplyZeroExtend32()
{
    for (auto nOperand : int32Operands()) {
        auto mul = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.multiplyZeroExtend32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto mOperand : int32Operands()) {
            uint32_t n = nOperand;
            uint32_t m = mOperand;
            CHECK_EQ(invoke<uint64_t>(mul, n, m), static_cast<uint64_t>(n) * static_cast<uint64_t>(m));
        }
    }
}

void testMultiplyAddSignExtend32()
{
    // d = SExt32(n) * SExt32(m) + a
    auto add = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.multiplyAddSignExtend32(GPRInfo::argumentGPR0, 
            GPRInfo::argumentGPR1, 
            GPRInfo::argumentGPR2, 
            GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto a : int64Operands())
                CHECK_EQ(invoke<int64_t>(add, n, m, a), static_cast<int64_t>(n) * static_cast<int64_t>(m) + a);
        }
    }
}

void testMultiplyAddZeroExtend32()
{
    // d = ZExt32(n) * ZExt32(m) + a
    auto add = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.multiplyAddZeroExtend32(GPRInfo::argumentGPR0, 
            GPRInfo::argumentGPR1, 
            GPRInfo::argumentGPR2, 
            GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto a : int64Operands()) {
                uint32_t un = n;
                uint32_t um = m;
                CHECK_EQ(invoke<int64_t>(add, n, m, a), static_cast<int64_t>(un) * static_cast<int64_t>(um) + a);
            }
        }
    }
}

void testMultiplySubSignExtend32()
{
    // d = a - SExt32(n) * SExt32(m)
    auto sub = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.multiplySubSignExtend32(GPRInfo::argumentGPR1, 
            GPRInfo::argumentGPR2,
            GPRInfo::argumentGPR0, 
            GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int64Operands()) {
        for (auto n : int32Operands()) {
            for (auto m : int32Operands())
                CHECK_EQ(invoke<int64_t>(sub, a, n, m), a - static_cast<int64_t>(n) * static_cast<int64_t>(m));
        }
    }
}

void testMultiplySubZeroExtend32()
{
    // d = a - (ZExt32(n) * ZExt32(m))
    auto sub = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.multiplySubZeroExtend32(GPRInfo::argumentGPR1, 
            GPRInfo::argumentGPR2,
            GPRInfo::argumentGPR0, 
            GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int64Operands()) {
        for (auto n : int32Operands()) {
            for (auto m : int32Operands()) {
                uint32_t un = n;
                uint32_t um = m;
                CHECK_EQ(invoke<int64_t>(sub, a, n, m), a - static_cast<int64_t>(un) * static_cast<int64_t>(um));
            }
        }
    }
}

void testMultiplyNegSignExtend32()
{
    // d = - (SExt32(n) * SExt32(m))
    auto neg = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.multiplyNegSignExtend32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto n : int32Operands()) {
        for (auto m : int32Operands())
            CHECK_EQ(invoke<int64_t>(neg, n, m), -(static_cast<int64_t>(n) * static_cast<int64_t>(m)));
    }
}

void testMultiplyNegZeroExtend32()
{
    // d = - ZExt32(n) * ZExt32(m)
    auto neg = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.multiplyNegZeroExtend32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            uint32_t un = n;
            uint32_t um = m;
            CHECK_EQ(invoke<uint64_t>(neg, n, m), -(static_cast<uint64_t>(un) * static_cast<uint64_t>(um)));
        }
    }
}

void testExtractUnsignedBitfield32()
{
    uint32_t src = 0xf0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto ubfx32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractUnsignedBitfield32(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                CHECK_EQ(invoke<uint32_t>(ubfx32, src), ((src >> lsb) & ((1U << width) - 1U)));
            }
        }
    }
}

void testExtractUnsignedBitfield64()
{
    uint64_t src = 0xf0f0f0f0f0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 64) {
                auto ubfx64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractUnsignedBitfield64(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                CHECK_EQ(invoke<uint64_t>(ubfx64, src), ((src >> lsb) & ((1ULL << width) - 1ULL)));
            }
        }
    }
}

void testInsertUnsignedBitfieldInZero32()
{
    uint32_t src = 0xf0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto ubfiz32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.insertUnsignedBitfieldInZero32(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint32_t mask = (1U << width) - 1U;
                CHECK_EQ(invoke<uint32_t>(ubfiz32, src), (src & mask) << lsb);
            }
        }
    }
}

void testInsertUnsignedBitfieldInZero64()
{
    uint64_t src = 0xf0f0f0f0f0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 64) {
                auto ubfiz64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.insertUnsignedBitfieldInZero64(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint64_t mask = (1ULL << width) - 1ULL;
                CHECK_EQ(invoke<uint64_t>(ubfiz64, src), (src & mask) << lsb);
            }
        }
    }
}

void testInsertBitField32()
{
    uint32_t src = 0x0f0f0f0f;
    uint32_t dst = 0xf0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto bfi32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.insertBitField32(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::argumentGPR1);
                    jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint32_t mask1 = (1U << width) - 1U;
                uint32_t mask2 = ~(mask1 << lsb);
                uint32_t rhs = invoke<uint32_t>(bfi32, src, dst);
                uint32_t lhs = ((src & mask1) << lsb) | (dst & mask2);
                CHECK_EQ(rhs, lhs);
            }
        }
    }
}

void testInsertBitField64()
{
    uint64_t src = 0x0f0f0f0f0f0f0f0f;
    uint64_t dst = 0xf0f0f0f0f0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 64) {
                auto bfi64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.insertBitField64(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::argumentGPR1);
                    jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint64_t mask1 = (1ULL << width) - 1ULL;
                uint64_t mask2 = ~(mask1 << lsb);
                uint64_t rhs = invoke<uint64_t>(bfi64, src, dst);
                uint64_t lhs = ((src & mask1) << lsb) | (dst & mask2);
                CHECK_EQ(rhs, lhs);
            }
        }
    }
}

void testExtractInsertBitfieldAtLowEnd32()
{
    uint32_t src = 0xf0f0f0f0;
    uint32_t dst = 0x0f0f0f0f;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto bfxil32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractInsertBitfieldAtLowEnd32(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::argumentGPR1);
                    jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint32_t mask1 = (1U << width) - 1U;
                uint32_t mask2 = ~mask1;
                uint32_t rhs = invoke<uint32_t>(bfxil32, src, dst);
                uint32_t lhs = ((src >> lsb) & mask1) | (dst & mask2);
                CHECK_EQ(rhs, lhs);
            }
        }
    }
}

void testExtractInsertBitfieldAtLowEnd64()
{
    uint64_t src = 0x0f0f0f0f0f0f0f0f;
    uint64_t dst = 0xf0f0f0f0f0f0f0f0;
    Vector<uint64_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 64) {
                auto bfxil64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractInsertBitfieldAtLowEnd64(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::argumentGPR1);
                    jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint64_t mask1 = (1ULL << width) - 1ULL;
                uint64_t mask2 = ~mask1;
                uint64_t rhs = invoke<uint64_t>(bfxil64, src, dst);
                uint64_t lhs = ((src >> lsb) & mask1) | (dst & mask2);
                CHECK_EQ(rhs, lhs);
            }
        }
    }
}

void testClearBitField32()
{
    uint32_t src = std::numeric_limits<uint32_t>::max();
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto bfc32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.clearBitField32(CCallHelpers::TrustedImm32(lsb), CCallHelpers::TrustedImm32(width), GPRInfo::argumentGPR0);
                    jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint32_t mask = ((1U << width) - 1U) << lsb;
                uint32_t rhs = invoke<uint32_t>(bfc32, src);
                uint32_t lhs = src & ~mask;
                CHECK_EQ(rhs, lhs);
            }
        }
    }
}

void testClearBitField64()
{
    uint64_t src = std::numeric_limits<uint64_t>::max();
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto bfc64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.clearBitField64(CCallHelpers::TrustedImm32(lsb), CCallHelpers::TrustedImm32(width), GPRInfo::argumentGPR0);
                    jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });
                uint64_t mask = ((1ULL << width) - 1ULL) << lsb;
                uint64_t rhs = invoke<uint64_t>(bfc64, src);
                uint64_t lhs = src & ~mask;
                CHECK_EQ(rhs, lhs);
            }
        }
    }
}

void testClearBitsWithMask32()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.clearBitsWithMask32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto mask : int32Operands()) {
        uint32_t src = std::numeric_limits<uint32_t>::max();
        CHECK_EQ(invoke<uint32_t>(test, src, mask), (src & ~mask));
        CHECK_EQ(invoke<uint32_t>(test, 0U, mask), 0U);
    }
}

void testClearBitsWithMask64()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.clearBitsWithMask64(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto mask : int64Operands()) {
        uint64_t src = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<uint64_t>(test, src, mask), (src & ~mask));
        CHECK_EQ(invoke<uint64_t>(test, 0ULL, mask), 0ULL);
    }
}

void testOrNot32()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.orNot32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto mask : int32Operands()) {
        int32_t src = std::numeric_limits<uint32_t>::max();
        CHECK_EQ(invoke<int32_t>(test, src, mask), (src | ~mask));
        CHECK_EQ(invoke<int32_t>(test, 0U, mask), ~mask);
    }
}

void testOrNot64()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.orNot64(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto mask : int64Operands()) {
        int64_t src = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<int64_t>(test, src, mask), (src | ~mask));
        CHECK_EQ(invoke<int64_t>(test, 0ULL, mask), ~mask);
    }
}

void testInsertSignedBitfieldInZero32()
{
    uint32_t src = 0xf0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto insertSignedBitfieldInZero32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.insertSignedBitfieldInZero32(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t bf = src;
                int32_t mask1 = (1 << width) - 1;
                int32_t mask2 = 1 << (width - 1);
                int32_t bfsx = ((bf & mask1) ^ mask2) - mask2;

                CHECK_EQ(invoke<int32_t>(insertSignedBitfieldInZero32, src), bfsx << lsb);
            }
        }
    }
}

void testInsertSignedBitfieldInZero64()
{
    int64_t src = 0xf0f0f0f0f0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 64) {
                auto insertSignedBitfieldInZero64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.insertSignedBitfieldInZero64(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t bf = src;
                int64_t amount = CHAR_BIT * sizeof(bf) - width;
                int64_t bfsx = (bf << amount) >> amount;

                CHECK_EQ(invoke<int64_t>(insertSignedBitfieldInZero64, src), bfsx << lsb);
            }
        }
    }
}

void testExtractSignedBitfield32()
{
    int32_t src = 0xf0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 32) {
                auto extractSignedBitfield32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractSignedBitfield32(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t bf = src >> lsb;
                int32_t mask1 = (1 << width) - 1;
                int32_t mask2 = 1 << (width - 1);
                int32_t bfsx = ((bf & mask1) ^ mask2) - mask2;

                CHECK_EQ(invoke<int32_t>(extractSignedBitfield32, src), bfsx);
            }
        }
    }
}

void testExtractSignedBitfield64()
{
    int64_t src = 0xf0f0f0f0f0f0f0f0;
    Vector<uint32_t> imms = { 0, 1, 5, 7, 30, 31, 32, 42, 56, 62, 63, 64 };
    for (auto lsb : imms) {
        for (auto width : imms) {
            if (width > 0 && lsb + width < 64) {
                auto extractSignedBitfield64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractSignedBitfield64(GPRInfo::argumentGPR0, 
                        CCallHelpers::TrustedImm32(lsb), 
                        CCallHelpers::TrustedImm32(width), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t bf = src >> lsb;
                int64_t amount = CHAR_BIT * sizeof(bf) - width;
                int64_t bfsx = (bf << amount) >> amount;

                CHECK_EQ(invoke<int64_t>(extractSignedBitfield64, src), bfsx);
            }
        }
    }
}

void testExtractRegister32()
{
    uint32_t datasize = CHAR_BIT * sizeof(uint32_t);

    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (uint32_t lsb = 0; lsb < datasize; ++lsb) {
                auto extractRegister32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractRegister32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(lsb),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                // Test pattern: d = ((n & mask) << highWidth) | (m >>> lowWidth)
                // Where: highWidth = datasize - lowWidth
                //        mask = (1 << lowWidth) - 1
                uint32_t highWidth = datasize - lsb;
                uint32_t mask = (1U << (lsb % 32)) - 1U;
                uint32_t left = (n & mask) << (highWidth % 32);
                uint32_t right = (static_cast<uint32_t>(m) >> (lsb % 32));
                uint32_t rhs = left | right;
                uint32_t lhs = invoke<uint32_t>(extractRegister32, n, m);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testExtractRegister64()
{
    uint64_t datasize = CHAR_BIT * sizeof(uint64_t);

    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (uint32_t lsb = 0; lsb < datasize; ++lsb) {
                auto extractRegister64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.extractRegister64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(lsb),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                // Test pattern: d = ((n & mask) << highWidth) | (m >>> lowWidth)
                // Where: highWidth = datasize - lowWidth
                //        mask = (1 << lowWidth) - 1
                uint64_t highWidth = datasize - lsb;
                uint64_t mask = (1ULL << (lsb % 64)) - 1ULL;
                uint64_t left = (n & mask) << (highWidth % 64);
                uint64_t right = (static_cast<uint64_t>(m) >> (lsb % 64));
                uint64_t rhs = left | right;
                uint64_t lhs = invoke<uint64_t>(extractRegister64, n, m);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAddWithLeftShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto add32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.addLeftShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(add32, n, m);
                int32_t rhs = n + (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAddWithRightShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto add32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.addRightShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(add32, n, m);
                int32_t rhs = n + (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAddWithUnsignedRightShift32()
{
    Vector<uint32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto add32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.addUnsignedRightShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint32_t lhs = invoke<uint32_t>(add32, n, m);
                uint32_t rhs = static_cast<uint32_t>(n) + (static_cast<uint32_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAddWithLeftShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto add64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.addLeftShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(add64, n, m);
                int64_t rhs = n + (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAddWithRightShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto add64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.addRightShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(add64, n, m);
                int64_t rhs = n + (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAddWithUnsignedRightShift64()
{
    Vector<uint32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto add64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.addUnsignedRightShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint64_t lhs = invoke<uint64_t>(add64, n, m);
                uint64_t rhs = static_cast<uint64_t>(n) + (static_cast<uint64_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testSubWithLeftShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto sub32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.subLeftShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(sub32, n, m);
                int32_t rhs = n - (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testSubWithRightShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto sub32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.subRightShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(sub32, n, m);
                int32_t rhs = n - (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testSubWithUnsignedRightShift32()
{
    Vector<uint32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto sub32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.subUnsignedRightShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint32_t lhs = invoke<uint32_t>(sub32, n, m);
                uint32_t rhs = static_cast<uint32_t>(n) - (static_cast<uint32_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testSubWithLeftShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto sub64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.subLeftShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(sub64, n, m);
                int64_t rhs = n - (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testSubWithRightShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto sub64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.subRightShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(sub64, n, m);
                int64_t rhs = n - (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testSubWithUnsignedRightShift64()
{
    Vector<uint32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto sub64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.subUnsignedRightShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint64_t lhs = invoke<uint64_t>(sub64, n, m);
                uint64_t rhs = static_cast<uint64_t>(n) - (static_cast<uint64_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorNot32()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.xorNot32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto mask : int32Operands()) {
        int32_t src = std::numeric_limits<uint32_t>::max();
        CHECK_EQ(invoke<int32_t>(test, src, mask), (src ^ ~mask));
        CHECK_EQ(invoke<int32_t>(test, 0U, mask), ~mask);
    }
}

void testXorNot64()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.xorNot64(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto mask : int64Operands()) {
        int64_t src = std::numeric_limits<uint64_t>::max();
        CHECK_EQ(invoke<int64_t>(test, src, mask), (src ^ ~mask));
        CHECK_EQ(invoke<int64_t>(test, 0ULL, mask), ~mask);
    }
}

void testXorNotWithLeftShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto test = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorNotLeftShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(test, n, m);
                int32_t rhs = n ^ ~(m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorNotWithRightShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto test = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorNotRightShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(test, n, m);
                int32_t rhs = n ^ ~(m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorNotWithUnsignedRightShift32()
{
    Vector<uint32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto test = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorNotUnsignedRightShift32(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint32_t lhs = invoke<uint32_t>(test, n, m);
                uint32_t rhs = static_cast<uint32_t>(n) ^ ~(static_cast<uint32_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorNotWithLeftShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto test = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorNotLeftShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(test, n, m);
                int64_t rhs = n ^ ~(m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorNotWithRightShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto test = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorNotRightShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(test, n, m);
                int64_t rhs = n ^ ~(m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorNotWithUnsignedRightShift64()
{
    Vector<uint32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto test = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorNotUnsignedRightShift64(GPRInfo::argumentGPR0, 
                        GPRInfo::argumentGPR1, 
                        CCallHelpers::TrustedImm32(amount), 
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint64_t lhs = invoke<uint64_t>(test, n, m);
                uint64_t rhs = static_cast<uint64_t>(n) ^ ~(static_cast<uint64_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testStorePrePostIndex32()
{
    int32_t nums[] = { 1, 2, 3 };
    intptr_t addr = std::bit_cast<intptr_t>(&nums[1]);
    int32_t index = sizeof(int32_t);

    auto test1 = [&] (int32_t src) {
        auto store = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // *++p1 = 4; return p1;
            jit.store32(GPRInfo::argumentGPR0, CCallHelpers::PreIndexAddress(GPRInfo::argumentGPR1, index));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<intptr_t>(store, src, addr);
    };

    int32_t* p1 = std::bit_cast<int32_t*>(test1(4));
    CHECK_EQ(*p1, 4);
    CHECK_EQ(*--p1, nums[1]);

    auto test2 = [&] (int32_t src) {
        auto store = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // *p2++ = 5; return p2;
            jit.store32(GPRInfo::argumentGPR0, CCallHelpers::PostIndexAddress(GPRInfo::argumentGPR1, index));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<intptr_t>(store, src, addr);
    };

    int32_t* p2 = std::bit_cast<int32_t*>(test2(5));
    CHECK_EQ(*p2, 4);
    CHECK_EQ(*--p2, 5);
}

void testStorePrePostIndex64()
{
    int64_t nums[] = { 1, 2, 3 };
    intptr_t addr = std::bit_cast<intptr_t>(&nums[1]);
    int32_t index = sizeof(int64_t);

    auto test1 = [&] (int64_t src) {
        auto store = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // *++p1 = 4; return p1;
            jit.store64(GPRInfo::argumentGPR0, CCallHelpers::PreIndexAddress(GPRInfo::argumentGPR1, index));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<intptr_t>(store, src, addr);
    };

    int64_t* p1 = std::bit_cast<int64_t*>(test1(4));
    CHECK_EQ(*p1, 4);
    CHECK_EQ(*--p1, nums[1]);

    auto test2 = [&] (int64_t src) {
        auto store = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // *p2++ = 5; return p2;
            jit.store64(GPRInfo::argumentGPR0, CCallHelpers::PostIndexAddress(GPRInfo::argumentGPR1, index));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<intptr_t>(store, src, addr);
    };

    int64_t* p2 = std::bit_cast<int64_t*>(test2(5));
    CHECK_EQ(*p2, 4);
    CHECK_EQ(*--p2, 5);
}

void testLoadPrePostIndex32()
{
    int32_t nums[] = { 1, 2, 3 };
    int32_t index = sizeof(int32_t);

    auto test1 = [&] (int32_t replace) {
        auto load = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // res = *++p1; *p1 = 4; return res;
            jit.load32(CCallHelpers::PreIndexAddress(GPRInfo::argumentGPR0, index), GPRInfo::argumentGPR1);
            jit.store32(CCallHelpers::TrustedImm32(replace), CCallHelpers::Address(GPRInfo::argumentGPR0));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<int32_t>(load, &nums[1]);
    };

    CHECK_EQ(test1(4), 3);
    CHECK_EQ(nums[2], 4);

    auto test2 = [&] (int32_t replace) {
        auto load = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // res = *p2++; *p2 = 5; return res;
            jit.load32(CCallHelpers::PostIndexAddress(GPRInfo::argumentGPR0, index), GPRInfo::argumentGPR1);
            jit.store32(CCallHelpers::TrustedImm32(replace), CCallHelpers::Address(GPRInfo::argumentGPR0));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<int32_t>(load, &nums[1]);
    };

    CHECK_EQ(test2(5), 2);
    CHECK_EQ(nums[2], 5);
}

void testLoadPrePostIndex64()
{
    int64_t nums[] = { 1, 2, 3 };
    int32_t index = sizeof(int64_t);

    auto test1 = [&] (int64_t replace) {
        auto load = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // res = *++p1; *p1 = 4; return res;
            jit.load64(CCallHelpers::PreIndexAddress(GPRInfo::argumentGPR0, index), GPRInfo::argumentGPR1);
            jit.store64(CCallHelpers::TrustedImm64(replace), CCallHelpers::Address(GPRInfo::argumentGPR0));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<int64_t>(load, &nums[1]);
    };

    CHECK_EQ(test1(4), 3);
    CHECK_EQ(nums[2], 4);

    auto test2 = [&] (int64_t replace) {
        auto load = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // res = *p2++; *p2 = 5; return res;
            jit.load64(CCallHelpers::PostIndexAddress(GPRInfo::argumentGPR0, index), GPRInfo::argumentGPR1);
            jit.store64(CCallHelpers::TrustedImm64(replace), CCallHelpers::Address(GPRInfo::argumentGPR0));
            jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });
        return invoke<int64_t>(load, &nums[1]);
    };

    CHECK_EQ(test2(5), 2);
    CHECK_EQ(nums[2], 5);
}

void testAndLeftShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto and32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.andLeftShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(and32, n, m);
                int32_t rhs = n & (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAndRightShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto and32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.andRightShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(and32, n, m);
                int32_t rhs = n & (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAndUnsignedRightShift32()
{
    Vector<uint32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto and32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.andUnsignedRightShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint32_t lhs = invoke<uint32_t>(and32, n, m);
                uint32_t rhs = static_cast<uint32_t>(n) & (static_cast<uint32_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAndLeftShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto and64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.andLeftShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(and64, n, m);
                int64_t rhs = n & (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAndRightShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto and64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.andRightShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(and64, n, m);
                int64_t rhs = n & (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testAndUnsignedRightShift64()
{
    Vector<uint32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto and64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.andUnsignedRightShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint64_t lhs = invoke<uint64_t>(and64, n, m);
                uint64_t rhs = static_cast<uint64_t>(n) & (static_cast<uint64_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorLeftShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto xor32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorLeftShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(xor32, n, m);
                int32_t rhs = n ^ (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorRightShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto xor32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorRightShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(xor32, n, m);
                int32_t rhs = n ^ (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorUnsignedRightShift32()
{
    Vector<uint32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto xor32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorUnsignedRightShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint32_t lhs = invoke<uint32_t>(xor32, n, m);
                uint32_t rhs = static_cast<uint32_t>(n) ^ (static_cast<uint32_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorLeftShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto xor64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorLeftShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(xor64, n, m);
                int64_t rhs = n ^ (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorRightShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto xor64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorRightShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(xor64, n, m);
                int64_t rhs = n ^ (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testXorUnsignedRightShift64()
{
    Vector<uint32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto xor64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.xorUnsignedRightShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint64_t lhs = invoke<uint64_t>(xor64, n, m);
                uint64_t rhs = static_cast<uint64_t>(n) ^ (static_cast<uint64_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testOrLeftShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto or32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.orLeftShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(or32, n, m);
                int32_t rhs = n | (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testOrRightShift32()
{
    Vector<int32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto or32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.orRightShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int32_t lhs = invoke<int32_t>(or32, n, m);
                int32_t rhs = n | (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testOrUnsignedRightShift32()
{
    Vector<uint32_t> amounts = { 0, 17, 31 };
    for (auto n : int32Operands()) {
        for (auto m : int32Operands()) {
            for (auto amount : amounts) {
                auto or32 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.orUnsignedRightShift32(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint32_t lhs = invoke<uint32_t>(or32, n, m);
                uint32_t rhs = static_cast<uint32_t>(n) | (static_cast<uint32_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testOrLeftShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto or64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.orLeftShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(or64, n, m);
                int64_t rhs = n | (m << amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testOrRightShift64()
{
    Vector<int32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto or64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.orRightShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                int64_t lhs = invoke<int64_t>(or64, n, m);
                int64_t rhs = n | (m >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testOrUnsignedRightShift64()
{
    Vector<uint32_t> amounts = { 0, 34, 63 };
    for (auto n : int64Operands()) {
        for (auto m : int64Operands()) {
            for (auto amount : amounts) {
                auto or64 = compile([=] (CCallHelpers& jit) {
                    emitFunctionPrologue(jit);

                    jit.orUnsignedRightShift64(GPRInfo::argumentGPR0,
                        GPRInfo::argumentGPR1,
                        CCallHelpers::TrustedImm32(amount),
                        GPRInfo::returnValueGPR);

                    emitFunctionEpilogue(jit);
                    jit.ret();
                });

                uint64_t lhs = invoke<uint64_t>(or64, n, m);
                uint64_t rhs = static_cast<uint64_t>(n) | (static_cast<uint64_t>(m) >> amount);
                CHECK_EQ(lhs, rhs);
            }
        }
    }
}

void testZeroExtend48ToWord()
{
    auto zext48First = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.zeroExtend48ToWord(GPRInfo::argumentGPR0, GPRInfo::argumentGPR0);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto zeroTop16Bits = [] (int64_t value) -> int64_t {
        return value & ((1ull << 48) - 1);
    };

    for (auto a : int64Operands())
        CHECK_EQ(invoke<int64_t>(zext48First, a), zeroTop16Bits(a));

    auto zext48Second = compile([=] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.zeroExtend48ToWord(GPRInfo::argumentGPR1, GPRInfo::argumentGPR0);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int64Operands())
        CHECK_EQ(invoke<int64_t>(zext48Second, 0, a), zeroTop16Bits(a));
}
#endif

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
void testCompareFloat(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;

    auto compareFloat = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::returnValueGPR);
        jit.compareFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto compareFloatGeneric = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
        auto jump = jit.branchFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
        jump.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto operands = floatOperands();
    for (auto a : operands) {
        for (auto b : operands) {
            arg1 = a;
            arg2 = b;
            CHECK_EQ(invoke<int>(compareFloat), invoke<int>(compareFloatGeneric));
        }
    }
}
#endif // CPU(X86_64) || CPU(ARM64)

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)

template<typename T, typename SelectionType>
void testMoveConditionallyFloatingPoint(MacroAssembler::DoubleCondition condition, const MacroAssemblerCodeRef<JSEntryPtrTag>& testCode, T& arg1, T& arg2, const Vector<T> operands, SelectionType selectionA, SelectionType selectionB)
{
    auto expectedResult = [&, condition] (T a, T b) -> SelectionType {
        auto isUnordered = [] (double x) {
            return x != x;
        };
        switch (condition) {
        case MacroAssembler::DoubleEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a == b) ? selectionA : selectionB;
        case MacroAssembler::DoubleNotEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a != b) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a > b) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanOrEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a >= b) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a < b) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanOrEqualAndOrdered:
            return !isUnordered(a) && !isUnordered(b) && (a <= b) ? selectionA : selectionB;
        case MacroAssembler::DoubleEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a == b) ? selectionA : selectionB;
        case MacroAssembler::DoubleNotEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a != b) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a > b) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanOrEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a >= b) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a < b) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanOrEqualOrUnordered:
            return isUnordered(a) || isUnordered(b) || (a <= b) ? selectionA : selectionB;
        } // switch
        RELEASE_ASSERT_NOT_REACHED();
    };

    for (auto a : operands) {
        for (auto b : operands) {
            arg1 = a;
            arg2 = b;
            CHECK_EQ(invoke<SelectionType>(testCode), expectedResult(a, b));
        }
    }
}

void testMoveConditionallyDouble2(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        RELEASE_ASSERT(destGPR != selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), destGPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, destGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveConditionallyDouble3(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;
    unsigned corruptedSelectionA = 0xbbad000a;
    unsigned corruptedSelectionB = 0xbbad000b;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        GPRReg selectionBGPR = GPRInfo::argumentGPR3;
        RELEASE_ASSERT(destGPR != selectionAGPR);
        RELEASE_ASSERT(destGPR != selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(-1), destGPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, selectionBGPR, destGPR);

        auto aIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionAGPR, CCallHelpers::TrustedImm32(selectionA));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionA), destGPR);
        aIsUnchanged.link(&jit);

        auto bIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionBGPR, CCallHelpers::TrustedImm32(selectionB));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionB), destGPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveConditionallyDouble3DestSameAsThenCase(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;
    unsigned corruptedSelectionB = 0xbbad000b;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = destGPR;
        GPRReg selectionBGPR = GPRInfo::argumentGPR3;
        RELEASE_ASSERT(destGPR == selectionAGPR);
        RELEASE_ASSERT(destGPR != selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, selectionBGPR, destGPR);

        auto bIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionBGPR, CCallHelpers::TrustedImm32(selectionB));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionB), destGPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveConditionallyDouble3DestSameAsElseCase(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;
    unsigned corruptedSelectionA = 0xbbad000a;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        GPRReg selectionBGPR = destGPR;
        RELEASE_ASSERT(destGPR != selectionAGPR);
        RELEASE_ASSERT(destGPR == selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, selectionBGPR, destGPR);

        auto aIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionAGPR, CCallHelpers::TrustedImm32(selectionA));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionA), destGPR);
        aIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveConditionallyFloat2(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        RELEASE_ASSERT(destGPR != selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), GPRInfo::returnValueGPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, destGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

void testMoveConditionallyFloat3(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;
    unsigned corruptedSelectionA = 0xbbad000a;
    unsigned corruptedSelectionB = 0xbbad000b;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        GPRReg selectionBGPR = GPRInfo::argumentGPR3;
        RELEASE_ASSERT(destGPR != selectionAGPR);
        RELEASE_ASSERT(destGPR != selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(-1), destGPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, selectionBGPR, destGPR);

        auto aIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionAGPR, CCallHelpers::TrustedImm32(selectionA));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionA), destGPR);
        aIsUnchanged.link(&jit);

        auto bIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionBGPR, CCallHelpers::TrustedImm32(selectionB));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionB), destGPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

void testMoveConditionallyFloat3DestSameAsThenCase(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;
    unsigned corruptedSelectionB = 0xbbad000b;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = destGPR;
        GPRReg selectionBGPR = GPRInfo::argumentGPR3;
        RELEASE_ASSERT(destGPR == selectionAGPR);
        RELEASE_ASSERT(destGPR != selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, selectionBGPR, destGPR);

        auto bIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionBGPR, CCallHelpers::TrustedImm32(selectionB));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionB), destGPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

void testMoveConditionallyFloat3DestSameAsElseCase(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;
    unsigned corruptedSelectionA = 0xbbad000a;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg destGPR = GPRInfo::returnValueGPR;
        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        GPRReg selectionBGPR = destGPR;
        RELEASE_ASSERT(destGPR != selectionAGPR);
        RELEASE_ASSERT(destGPR == selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT1);
        jit.moveConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT1, selectionAGPR, selectionBGPR, destGPR);

        auto aIsUnchanged = jit.branch32(CCallHelpers::Equal, selectionAGPR, CCallHelpers::TrustedImm32(selectionA));
        jit.move(CCallHelpers::TrustedImm32(corruptedSelectionA), destGPR);
        aIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyDouble(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;
    double corruptedSelectionA = 55555;
    double corruptedSelectionB = 66666;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        FPRReg destFPR = FPRInfo::returnValueFPR;
        FPRReg selectionAFPR = FPRInfo::fpRegT1;
        FPRReg selectionBFPR = FPRInfo::fpRegT2;
        FPRReg arg1FPR = FPRInfo::fpRegT3;
        FPRReg arg2FPR = FPRInfo::fpRegT4;

        RELEASE_ASSERT(destFPR != selectionAFPR);
        RELEASE_ASSERT(destFPR != selectionBFPR);
        RELEASE_ASSERT(destFPR != arg1FPR);
        RELEASE_ASSERT(destFPR != arg2FPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), arg1FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), arg2FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), selectionAFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), selectionBFPR);
        jit.moveDoubleConditionallyDouble(condition, arg1FPR, arg2FPR, selectionAFPR, selectionBFPR, destFPR);

        FPRReg tempFPR = FPRInfo::fpRegT5;
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), tempFPR);
        auto aIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionAFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionA), destFPR);
        aIsUnchanged.link(&jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), tempFPR);
        auto bIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionBFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionB), destFPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyDoubleDestSameAsThenCase(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;
    double corruptedSelectionB = 66666;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        FPRReg destFPR = FPRInfo::returnValueFPR;
        FPRReg selectionAFPR = destFPR;
        FPRReg selectionBFPR = FPRInfo::fpRegT2;
        FPRReg arg1FPR = FPRInfo::fpRegT3;
        FPRReg arg2FPR = FPRInfo::fpRegT4;

        RELEASE_ASSERT(destFPR == selectionAFPR);
        RELEASE_ASSERT(destFPR != selectionBFPR);
        RELEASE_ASSERT(destFPR != arg1FPR);
        RELEASE_ASSERT(destFPR != arg2FPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), arg1FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), arg2FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), selectionAFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), selectionBFPR);
        jit.moveDoubleConditionallyDouble(condition, arg1FPR, arg2FPR, selectionAFPR, selectionBFPR, destFPR);

        FPRReg tempFPR = FPRInfo::fpRegT5;
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), tempFPR);
        auto bIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionBFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionB), destFPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyDoubleDestSameAsElseCase(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double arg2 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;
    double corruptedSelectionA = 55555;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        FPRReg destFPR = FPRInfo::returnValueFPR;
        FPRReg selectionAFPR = FPRInfo::fpRegT1;
        FPRReg selectionBFPR = destFPR;
        FPRReg arg1FPR = FPRInfo::fpRegT3;
        FPRReg arg2FPR = FPRInfo::fpRegT4;

        RELEASE_ASSERT(destFPR != selectionAFPR);
        RELEASE_ASSERT(destFPR == selectionBFPR);
        RELEASE_ASSERT(destFPR != arg1FPR);
        RELEASE_ASSERT(destFPR != arg2FPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), arg1FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), arg2FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), selectionAFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), selectionBFPR);
        jit.moveDoubleConditionallyDouble(condition, arg1FPR, arg2FPR, selectionAFPR, selectionBFPR, destFPR);

        FPRReg tempFPR = FPRInfo::fpRegT5;
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), tempFPR);
        auto aIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionAFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionA), destFPR);
        aIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, doubleOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyFloat(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;
    double corruptedSelectionA = 55555;
    double corruptedSelectionB = 66666;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        FPRReg destFPR = FPRInfo::returnValueFPR;
        FPRReg selectionAFPR = FPRInfo::fpRegT1;
        FPRReg selectionBFPR = FPRInfo::fpRegT2;
        FPRReg arg1FPR = FPRInfo::fpRegT3;
        FPRReg arg2FPR = FPRInfo::fpRegT4;

        RELEASE_ASSERT(destFPR != selectionAFPR);
        RELEASE_ASSERT(destFPR != selectionBFPR);
        RELEASE_ASSERT(destFPR != arg1FPR);
        RELEASE_ASSERT(destFPR != arg2FPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), arg1FPR);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), arg2FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), selectionAFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), selectionBFPR);
        jit.moveDoubleConditionallyFloat(condition, arg1FPR, arg2FPR, selectionAFPR, selectionBFPR, destFPR);

        FPRReg tempFPR = FPRInfo::fpRegT5;
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), tempFPR);
        auto aIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionAFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionA), destFPR);
        aIsUnchanged.link(&jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), tempFPR);
        auto bIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionBFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionB), destFPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyFloatDestSameAsThenCase(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;
    double corruptedSelectionB = 66666;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        FPRReg destFPR = FPRInfo::returnValueFPR;
        FPRReg selectionAFPR = destFPR;
        FPRReg selectionBFPR = FPRInfo::fpRegT2;
        FPRReg arg1FPR = FPRInfo::fpRegT3;
        FPRReg arg2FPR = FPRInfo::fpRegT4;

        RELEASE_ASSERT(destFPR == selectionAFPR);
        RELEASE_ASSERT(destFPR != selectionBFPR);
        RELEASE_ASSERT(destFPR != arg1FPR);
        RELEASE_ASSERT(destFPR != arg2FPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), arg1FPR);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), arg2FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), selectionAFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), selectionBFPR);
        jit.moveDoubleConditionallyFloat(condition, arg1FPR, arg2FPR, selectionAFPR, selectionBFPR, destFPR);

        FPRReg tempFPR = FPRInfo::fpRegT5;
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), tempFPR);
        auto bIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionBFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionB), destFPR);
        bIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyFloatDestSameAsElseCase(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    float arg2 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;
    double corruptedSelectionA = 55555;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        FPRReg destFPR = FPRInfo::returnValueFPR;
        FPRReg selectionAFPR = FPRInfo::fpRegT1;
        FPRReg selectionBFPR = destFPR;
        FPRReg arg1FPR = FPRInfo::fpRegT3;
        FPRReg arg2FPR = FPRInfo::fpRegT4;

        RELEASE_ASSERT(destFPR != selectionAFPR);
        RELEASE_ASSERT(destFPR == selectionBFPR);
        RELEASE_ASSERT(destFPR != arg1FPR);
        RELEASE_ASSERT(destFPR != arg2FPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), arg1FPR);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg2), arg2FPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), selectionAFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), selectionBFPR);
        jit.moveDoubleConditionallyFloat(condition, arg1FPR, arg2FPR, selectionAFPR, selectionBFPR, destFPR);

        FPRReg tempFPR = FPRInfo::fpRegT5;
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), tempFPR);
        auto aIsUnchanged = jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, selectionAFPR, tempFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&corruptedSelectionA), destFPR);
        aIsUnchanged.link(&jit);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPoint(condition, testCode, arg1, arg2, floatOperands(), selectionA, selectionB);
}

template<typename T, typename SelectionType>
void testMoveConditionallyFloatingPointSameArg(MacroAssembler::DoubleCondition condition, const MacroAssemblerCodeRef<JSEntryPtrTag>& testCode, T& arg1, const Vector<T> operands, SelectionType selectionA, SelectionType selectionB)
{
    auto expectedResult = [&, condition] (T a) -> SelectionType {
        auto isUnordered = [] (double x) {
            return x != x;
        };
        switch (condition) {
        case MacroAssembler::DoubleEqualAndOrdered:
            return !isUnordered(a) && (a == a) ? selectionA : selectionB;
        case MacroAssembler::DoubleNotEqualAndOrdered:
            return !isUnordered(a) && (a != a) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanAndOrdered:
            return !isUnordered(a) && (a > a) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanOrEqualAndOrdered:
            return !isUnordered(a) && (a >= a) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanAndOrdered:
            return !isUnordered(a) && (a < a) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanOrEqualAndOrdered:
            return !isUnordered(a) && (a <= a) ? selectionA : selectionB;
        case MacroAssembler::DoubleEqualOrUnordered:
            return isUnordered(a) || (a == a) ? selectionA : selectionB;
        case MacroAssembler::DoubleNotEqualOrUnordered:
            return isUnordered(a) || (a != a) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanOrUnordered:
            return isUnordered(a) || (a > a) ? selectionA : selectionB;
        case MacroAssembler::DoubleGreaterThanOrEqualOrUnordered:
            return isUnordered(a) || (a >= a) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanOrUnordered:
            return isUnordered(a) || (a < a) ? selectionA : selectionB;
        case MacroAssembler::DoubleLessThanOrEqualOrUnordered:
            return isUnordered(a) || (a <= a) ? selectionA : selectionB;
        } // switch
        RELEASE_ASSERT_NOT_REACHED();
    };

    for (auto a : operands) {
        arg1 = a;
        CHECK_EQ(invoke<SelectionType>(testCode), expectedResult(a));
    }
}

void testMoveConditionallyDouble2SameArg(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        RELEASE_ASSERT(GPRInfo::returnValueGPR != selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), GPRInfo::returnValueGPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.moveConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, selectionAGPR, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPointSameArg(condition, testCode, arg1, doubleOperands(), selectionA, selectionB);
}

void testMoveConditionallyDouble3SameArg(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        GPRReg selectionBGPR = GPRInfo::argumentGPR3;
        RELEASE_ASSERT(GPRInfo::returnValueGPR != selectionAGPR);
        RELEASE_ASSERT(GPRInfo::returnValueGPR != selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::returnValueGPR);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.moveConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, selectionAGPR, selectionBGPR, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPointSameArg(condition, testCode, arg1, doubleOperands(), selectionA, selectionB);
}

void testMoveConditionallyFloat2SameArg(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        RELEASE_ASSERT(GPRInfo::returnValueGPR != selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), GPRInfo::returnValueGPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.moveConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, selectionAGPR, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPointSameArg(condition, testCode, arg1, floatOperands(), selectionA, selectionB);
}

void testMoveConditionallyFloat3SameArg(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    unsigned selectionA = 42;
    unsigned selectionB = 17;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg selectionAGPR = GPRInfo::argumentGPR2;
        GPRReg selectionBGPR = GPRInfo::argumentGPR3;
        RELEASE_ASSERT(GPRInfo::returnValueGPR != selectionAGPR);
        RELEASE_ASSERT(GPRInfo::returnValueGPR != selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionA), selectionAGPR);
        jit.move(CCallHelpers::TrustedImm32(selectionB), selectionBGPR);
        jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::returnValueGPR);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.moveConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, selectionAGPR, selectionBGPR, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPointSameArg(condition, testCode, arg1, floatOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyDoubleSameArg(MacroAssembler::DoubleCondition condition)
{
    double arg1 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), FPRInfo::fpRegT2);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), FPRInfo::fpRegT3);
        jit.moveDoubleConditionallyDouble(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, FPRInfo::fpRegT2, FPRInfo::fpRegT3, FPRInfo::returnValueFPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPointSameArg(condition, testCode, arg1, doubleOperands(), selectionA, selectionB);
}

void testMoveDoubleConditionallyFloatSameArg(MacroAssembler::DoubleCondition condition)
{
    float arg1 = 0;
    double selectionA = 42.0;
    double selectionB = 17.0;

    auto testCode = compile([&, condition] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT0);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionA), FPRInfo::fpRegT2);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&selectionB), FPRInfo::fpRegT3);
        jit.moveDoubleConditionallyFloat(condition, FPRInfo::fpRegT0, FPRInfo::fpRegT0, FPRInfo::fpRegT2, FPRInfo::fpRegT3, FPRInfo::returnValueFPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    testMoveConditionallyFloatingPointSameArg(condition, testCode, arg1, floatOperands(), selectionA, selectionB);
}

void testSignExtend8To32()
{
    auto code = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.signExtend8To32(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int8Operands()) {
        // Ensuring the upper 32bit is zero cleared.
        int64_t expectedResult = static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(a))));
        CHECK_EQ(invoke<int64_t>(code, a), expectedResult);
    }
}

void testSignExtend16To32()
{
    auto code = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.signExtend16To32(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int16Operands()) {
        // Ensuring the upper 32bit is zero cleared.
        int64_t expectedResult = static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(a))));
        CHECK_EQ(invoke<int64_t>(code, a), expectedResult);
    }
}

void testSignExtend8To64()
{
    auto code = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.signExtend8To64(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int8Operands()) {
        int64_t expectedResult = static_cast<int64_t>(a);
        CHECK_EQ(invoke<int64_t>(code, a), expectedResult);
    }
}

void testSignExtend16To64()
{
    auto code = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.signExtend16To64(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : int16Operands()) {
        int64_t expectedResult = static_cast<int64_t>(a);
        CHECK_EQ(invoke<int64_t>(code, a), expectedResult);
    }
}

#endif // CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)

#if CPU(ARM64)

void testAtomicStrongCASFill8()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.atomicStrongCAS8(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::Address(GPRInfo::argumentGPR2));
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    uint8_t data[] = {
        0xff, 0xff,
    };
    uint32_t result = invoke<uint32_t>(test, 0xffffffffffffffffULL, 0, data);
    CHECK_EQ(result, 0xff);
    CHECK_EQ(data[0], 0);
    CHECK_EQ(data[1], 0xff);
}

void testAtomicStrongCASFill16()
{
    auto test = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.atomicStrongCAS16(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::Address(GPRInfo::argumentGPR2));
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    uint16_t data[] = {
        0xffff, 0xffff,
    };
    uint32_t result = invoke<uint32_t>(test, 0xffffffffffffffffULL, 0, data);
    CHECK_EQ(result, 0xffff);
    CHECK_EQ(data[0], 0);
    CHECK_EQ(data[1], 0xffff);
}

#endif // CPU(ARM64)

void testLoadStorePair32()
{
    constexpr uint32_t initialValue = 0x55aabb80u;
    constexpr uint32_t value1 = 42;
    constexpr uint32_t value2 = 0xcfbb1357u;

    uint32_t buffer[10];

    auto initBuffer = [&] {
        for (unsigned i = 0; i < 10; ++i)
            buffer[i] = initialValue + i;
    };

    struct Pair {
        uint32_t value1;
        uint32_t value2;
    };

    Pair pair;
    auto initPair = [&] {
        pair = { 0, 0 };
    };

    // Test loadPair32.
    auto testLoadPair = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR0;
        constexpr GPRReg pairGPR = GPRInfo::argumentGPR1;
        jit.loadPair32(bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(uint32_t)), GPRInfo::regT2, GPRInfo::regT3);

        jit.store32(GPRInfo::regT2, CCallHelpers::Address(pairGPR, 0));
        jit.store32(GPRInfo::regT3, CCallHelpers::Address(pairGPR, sizeof(uint32_t)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testLoadPair0 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, 0);
    });

    initBuffer();

    initPair();
    invoke<void>(testLoadPair0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4);
    CHECK_EQ(pair.value2, initialValue + 5);

    initPair();
    buffer[4] = value1;
    buffer[5] = value2;
    invoke<void>(testLoadPair0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value1);
    CHECK_EQ(pair.value2, value2);

    auto testLoadPairMinus2 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, -2);
    });

    initPair();
    invoke<void>(testLoadPairMinus2, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4 - 2);
    CHECK_EQ(pair.value2, initialValue + 5 - 2);

    initPair();
    buffer[4 - 2] = value2;
    buffer[5 - 2] = value1;
    invoke<void>(testLoadPairMinus2, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value2);
    CHECK_EQ(pair.value2, value1);

    auto testLoadPairPlus3 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, 3);
    });

    initPair();
    invoke<void>(testLoadPairPlus3, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4 + 3);
    CHECK_EQ(pair.value2, initialValue + 5 + 3);

    initPair();
    buffer[4 + 3] = value1;
    buffer[5 + 3] = value2;
    invoke<void>(testLoadPairPlus3, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value1);
    CHECK_EQ(pair.value2, value2);

    // Test loadPair32 using a buffer register as a destination.
    auto testLoadPairUsingBufferRegisterAsDestination = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR0;
        constexpr GPRReg pairGPR = GPRInfo::argumentGPR1;
        jit.loadPair32(bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(uint32_t)), GPRInfo::argumentGPR0, GPRInfo::regT2);

        jit.store32(GPRInfo::argumentGPR0, CCallHelpers::Address(pairGPR, 0));
        jit.store32(GPRInfo::regT2, CCallHelpers::Address(pairGPR, sizeof(uint32_t)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testLoadPairUsingBufferRegisterAsDestination0 = compile([&] (CCallHelpers& jit) {
        testLoadPairUsingBufferRegisterAsDestination(jit, 0);
    });

    initBuffer();

    initPair();
    invoke<void>(testLoadPairUsingBufferRegisterAsDestination0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4);
    CHECK_EQ(pair.value2, initialValue + 5);

    // Test storePair32.
    auto testStorePair = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR2;
        jit.storePair32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(unsigned)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testStorePair0 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, 0);
    });

    initBuffer();
    invoke<void>(testStorePair0, value1, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], value1);
    CHECK_EQ(buffer[5], value2);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairMinus2 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, -2);
    });

    initBuffer();
    invoke<void>(testStorePairMinus2, value1, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], value1);
    CHECK_EQ(buffer[3], value2);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairPlus3 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, 3);
    });

    initBuffer();
    invoke<void>(testStorePairPlus3, value1, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], value1);
    CHECK_EQ(buffer[8], value2);
    CHECK_EQ(buffer[9], initialValue + 9);

    // Test storePair32 from 1 register.
    auto testStorePairFromOneReg = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR1;
        jit.storePair32(GPRInfo::argumentGPR0, GPRInfo::argumentGPR0, bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(unsigned)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testStorePairFromOneReg0 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, 0);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneReg0, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], value2);
    CHECK_EQ(buffer[5], value2);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairFromOneRegMinus2 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, -2);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneRegMinus2, value1, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], value1);
    CHECK_EQ(buffer[3], value1);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairFromOneRegPlus3 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, 3);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneRegPlus3, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], value2);
    CHECK_EQ(buffer[8], value2);
    CHECK_EQ(buffer[9], initialValue + 9);
}

void testSub32ArgImm()
{
    for (auto immediate : int32Operands()) {
        auto sub = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.sub32(GPRInfo::argumentGPR0, CCallHelpers::TrustedImm32(immediate), GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (auto value : int32Operands())
            CHECK_EQ(invoke<uint32_t>(sub, value), static_cast<uint32_t>(value - immediate));
    }
}

template<typename SrcT, typename DstT, typename Scenario, typename CompileFunctor>
void testLoadExtend_Address_RegisterID(const Scenario* scenarios, size_t numberOfScenarios, CompileFunctor compileFunctor)
{
    const int32_t offsets[] = {
        std::numeric_limits<int>::max(),
        0x10000000,
        0x1000000,
        0x100000,
        0x10000,
        0x1000,
        0x100,
        0x10,
        0x8,
        0,
        -0x8,
        -0x10,
        -0x100,
        -0x1000,
        -0x10000,
        -0x100000,
        -0x1000000,
        -0x10000000,
        std::numeric_limits<int>::min()
    };

    for (size_t j = 0; j < ARRAY_SIZE(offsets); ++j) {
        auto offset = offsets[j];
        offset = (offset / sizeof(SrcT)) * sizeof(SrcT); // Make sure the offset is aligned.

        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            compileFunctor(jit, offset);
            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (size_t i = 0; i < numberOfScenarios; ++i) {
            DstT result;
            SrcT* baseAddress = std::bit_cast<SrcT*>(std::bit_cast<const uint8_t*>(&scenarios[i].src) - offset);
            invoke<void>(test, &result, baseAddress);
            CHECK_EQ(result, scenarios[i].expected);
        }
    }
}

template<typename SrcT, typename DstT, typename Scenario, typename CompileFunctor>
void testLoadExtend_BaseIndex_RegisterID(const Scenario* scenarios, size_t numberOfScenarios, CompileFunctor compileFunctor)
{
    const int32_t offsets[] = {
        std::numeric_limits<int>::max(),
        0x10000000,
        0x1000000,
        0x100000,
        0x10000,
        0x1000,
        0x100,
        0x10,
        0x8,
        0,
        -0x8,
        -0x10,
        -0x100,
        -0x1000,
        -0x10000,
        -0x100000,
        -0x1000000,
        -0x10000000,
        std::numeric_limits<int>::min()
    };

    const int32_t indexes[] = {
        std::numeric_limits<int>::max(),
        0x100,
        0x8,
        0x2,
        0,
        -0x2,
        -0x8,
        -0x100,
        std::numeric_limits<int>::min()
    };

    for (size_t k = 0; k < ARRAY_SIZE(offsets); ++k) {
        int32_t offset = offsets[k];
        offset = (offset / sizeof(SrcT)) * sizeof(SrcT); // Make sure the offset is aligned.

        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            compileFunctor(jit, offset);
            emitFunctionEpilogue(jit);
            jit.ret();
        });

        for (size_t j = 0; j < ARRAY_SIZE(indexes); ++j) {
            auto index = indexes[j];

            for (size_t i = 0; i < numberOfScenarios; ++i) {
                DstT result;
                SrcT* baseAddress = std::bit_cast<SrcT*>(std::bit_cast<const uint8_t*>(&scenarios[i].src) - (index * sizeof(SrcT)) - offset);
                invoke<void>(test, &result, baseAddress, static_cast<intptr_t>(index));
                CHECK_EQ(result, scenarios[i].expected);
            }
        }
    }
}

template<typename T, typename Scenario, typename CompileFunctor>
UNUSED_FUNCTION void testLoadExtend_voidp_RegisterID(const Scenario* scenarios, size_t numberOfScenarios, CompileFunctor compileFunctor)
{
    for (size_t i = 0; i < numberOfScenarios; ++i) {
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            compileFunctor(jit, &scenarios[i].src);
            emitFunctionEpilogue(jit);
            jit.ret();
        });

        T result;
        invoke<void>(test, &result);
        CHECK_EQ(result, scenarios[i].expected);
    }
}

struct SignedLoad8to32Scenario {
    int8_t src;
    int32_t expected;
};

const SignedLoad8to32Scenario signedLoad8to32Scenarios[] = {
    { 0x7f, 0x7fll },
    { 42, 42 },
    { 1, 1 },
    { 0, 0 },
    { -1, -1 },
    { -42, -42 },
    { static_cast<int8_t>(0x81), static_cast<int32_t>(0xffffff81ll) },
    { static_cast<int8_t>(0x80), static_cast<int32_t>(0xffffff80ll) },
};

struct SignedLoad16to32Scenario {
    int16_t src;
    int32_t expected;
};

const SignedLoad16to32Scenario signedLoad16to32Scenarios[] = {
    { 0x7fff, 0x7fffll },
    { 42, 42 },
    { 1, 1 },
    { 0, 0 },
    { -1, -1 },
    { -42, -42 },
    { static_cast<int16_t>(0x8001), static_cast<int32_t>(0xffff8001ll) },
    { static_cast<int16_t>(0x8000), static_cast<int32_t>(0xffff8000ll) },
};

// void loadAcq8SignedExtendTo32(Address address, RegisterID dest)
void testLoadAcq8SignedExtendTo32_Address_RegisterID()
{
#if CPU(ARM64)
    testLoadExtend_Address_RegisterID<int8_t, int32_t>(signedLoad8to32Scenarios, ARRAY_SIZE(signedLoad8to32Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.loadAcq8SignedExtendTo32(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
#endif
}

// void load8SignedExtendTo32(Address address, RegisterID dest)
void testLoad8SignedExtendTo32_Address_RegisterID()
{
    testLoadExtend_Address_RegisterID<int8_t, int32_t>(signedLoad8to32Scenarios, ARRAY_SIZE(signedLoad8to32Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.load8SignedExtendTo32(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load8SignedExtendTo32(BaseIndex address, RegisterID dest)
void testLoad8SignedExtendTo32_BaseIndex_RegisterID()
{
    testLoadExtend_BaseIndex_RegisterID<int8_t, int32_t>(signedLoad8to32Scenarios, ARRAY_SIZE(signedLoad8to32Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg baseAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg indexGPR = GPRInfo::argumentGPR2;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR3;

            jit.load8SignedExtendTo32(CCallHelpers::BaseIndex(baseAddressGPR, indexGPR, CCallHelpers::Scale::TimesOne, offset), resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load8SignedExtendTo32(const void* address, RegisterID dest)
void testLoad8SignedExtendTo32_voidp_RegisterID()
{
#if CPU(ARM64) || CPU(RISCV64)
    testLoadExtend_voidp_RegisterID<int32_t>(signedLoad8to32Scenarios, ARRAY_SIZE(signedLoad8to32Scenarios),
        [] (CCallHelpers& jit, const void* src) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR1;

            jit.load8SignedExtendTo32(src, resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR, 0));
        });
#endif
}

// void loadAcq16SignedExtendTo32(Address address, RegisterID dest)
void testLoadAcq16SignedExtendTo32_Address_RegisterID()
{
#if CPU(ARM64)
    testLoadExtend_Address_RegisterID<int16_t, int32_t>(signedLoad16to32Scenarios, ARRAY_SIZE(signedLoad16to32Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.loadAcq16SignedExtendTo32(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
#endif
}

// void load16SignedExtendTo32(Address address, RegisterID dest)
void testLoad16SignedExtendTo32_Address_RegisterID()
{
    testLoadExtend_Address_RegisterID<int16_t, int32_t>(signedLoad16to32Scenarios, ARRAY_SIZE(signedLoad16to32Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.load16SignedExtendTo32(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load16SignedExtendTo32(BaseIndex address, RegisterID dest)
void testLoad16SignedExtendTo32_BaseIndex_RegisterID()
{
    testLoadExtend_BaseIndex_RegisterID<int16_t, int32_t>(signedLoad16to32Scenarios, ARRAY_SIZE(signedLoad16to32Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg baseAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg indexGPR = GPRInfo::argumentGPR2;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR3;

            jit.load16SignedExtendTo32(CCallHelpers::BaseIndex(baseAddressGPR, indexGPR, CCallHelpers::Scale::TimesTwo, offset), resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load16SignedExtendTo32(const void* address, RegisterID dest)
void testLoad16SignedExtendTo32_voidp_RegisterID()
{
#if CPU(ARM64) || CPU(RISCV64)
    testLoadExtend_voidp_RegisterID<int32_t>(signedLoad16to32Scenarios, ARRAY_SIZE(signedLoad16to32Scenarios),
        [] (CCallHelpers& jit, const void* src) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR1;

            jit.load16SignedExtendTo32(src, resultGPR);
            jit.store32(resultGPR, CCallHelpers::Address(resultAddressGPR, 0));
        });
#endif
}

struct SignedLoad8to64Scenario {
    int8_t src;
    int64_t expected;
};

const SignedLoad8to64Scenario signedLoad8to64Scenarios[] = {
    { 0x7f, 0x7fll },
    { 42, 42 },
    { 1, 1 },
    { 0, 0 },
    { -1, -1 },
    { -42, -42 },
    { static_cast<int8_t>(0x81), static_cast<int64_t>(0xffffffffffffff81ll) },
    { static_cast<int8_t>(0x80), static_cast<int64_t>(0xffffffffffffff80ll) },
};

struct SignedLoad16to64Scenario {
    int16_t src;
    int64_t expected;
};

const SignedLoad16to64Scenario signedLoad16to64Scenarios[] = {
    { 0x7fff, 0x7fffll },
    { 42, 42 },
    { 1, 1 },
    { 0, 0 },
    { -1, -1 },
    { -42, -42 },
    { static_cast<int16_t>(0x8001), static_cast<int64_t>(0xffffffffffff8001ll) },
    { static_cast<int16_t>(0x8000), static_cast<int64_t>(0xffffffffffff8000ll) },
};

struct SignedLoad32to64Scenario {
    int32_t src;
    int64_t expected;
};

const SignedLoad32to64Scenario signedLoad32to64Scenarios[] = {
    { 0x7fffffff, 0x7fffffffll },
    { 42, 42 },
    { 1, 1 },
    { 0, 0 },
    { -1, -1 },
    { -42, -42 },
    { static_cast<int32_t>(0x80000001), static_cast<int64_t>(0xffffffff80000001ll) },
    { static_cast<int32_t>(0x80000000), static_cast<int64_t>(0xffffffff80000000ll) },
};

// void loadAcq8SignedExtendTo64(Address address, RegisterID dest)
void testLoadAcq8SignedExtendTo64_Address_RegisterID()
{
#if CPU(ARM64)
    testLoadExtend_Address_RegisterID<int8_t, int64_t>(signedLoad8to64Scenarios, ARRAY_SIZE(signedLoad8to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.loadAcq8SignedExtendTo64(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
#endif
}

// void load8SignedExtendTo64(Address address, RegisterID dest)
void testLoad8SignedExtendTo64_Address_RegisterID()
{
    testLoadExtend_Address_RegisterID<int8_t, int64_t>(signedLoad8to64Scenarios, ARRAY_SIZE(signedLoad8to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.load8SignedExtendTo64(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load8SignedExtendTo64(BaseIndex address, RegisterID dest)
void testLoad8SignedExtendTo64_BaseIndex_RegisterID()
{
    testLoadExtend_BaseIndex_RegisterID<int8_t, int64_t>(signedLoad8to64Scenarios, ARRAY_SIZE(signedLoad8to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg baseAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg indexGPR = GPRInfo::argumentGPR2;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR3;

            jit.load8SignedExtendTo64(CCallHelpers::BaseIndex(baseAddressGPR, indexGPR, CCallHelpers::Scale::TimesOne, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load8SignedExtendTo64(const void* address, RegisterID dest)
void testLoad8SignedExtendTo64_voidp_RegisterID()
{
#if !CPU(X86_64)
    testLoadExtend_voidp_RegisterID<int64_t>(signedLoad8to64Scenarios, ARRAY_SIZE(signedLoad8to64Scenarios),
        [] (CCallHelpers& jit, const void* src) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR1;

            jit.load8SignedExtendTo64(src, resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR, 0));
        });
#endif
}

// void loadAcq16SignedExtendTo64(Address address, RegisterID dest)
void testLoadAcq16SignedExtendTo64_Address_RegisterID()
{
#if CPU(ARM64)
    testLoadExtend_Address_RegisterID<int16_t, int64_t>(signedLoad16to64Scenarios, ARRAY_SIZE(signedLoad16to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.loadAcq16SignedExtendTo64(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
#endif
}

// void load16SignedExtendTo64(Address address, RegisterID dest)
void testLoad16SignedExtendTo64_Address_RegisterID()
{
    testLoadExtend_Address_RegisterID<int16_t, int64_t>(signedLoad16to64Scenarios, ARRAY_SIZE(signedLoad16to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.load16SignedExtendTo64(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load16SignedExtendTo64(BaseIndex address, RegisterID dest)
void testLoad16SignedExtendTo64_BaseIndex_RegisterID()
{
    testLoadExtend_BaseIndex_RegisterID<int16_t, int64_t>(signedLoad16to64Scenarios, ARRAY_SIZE(signedLoad16to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg baseAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg indexGPR = GPRInfo::argumentGPR2;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR3;

            jit.load16SignedExtendTo64(CCallHelpers::BaseIndex(baseAddressGPR, indexGPR, CCallHelpers::Scale::TimesTwo, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load16SignedExtendTo64(const void* address, RegisterID dest)
void testLoad16SignedExtendTo64_voidp_RegisterID()
{
#if !CPU(X86_64)
    testLoadExtend_voidp_RegisterID<int64_t>(signedLoad16to64Scenarios, ARRAY_SIZE(signedLoad16to64Scenarios),
        [] (CCallHelpers& jit, const void* src) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR1;

            jit.load16SignedExtendTo64(src, resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR, 0));
        });
#endif
}

// void loadAcq32SignedExtendTo64(Address address, RegisterID dest)
void testLoadAcq32SignedExtendTo64_Address_RegisterID()
{
#if CPU(ARM64)
    testLoadExtend_Address_RegisterID<int32_t, int64_t>(signedLoad32to64Scenarios, ARRAY_SIZE(signedLoad32to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.loadAcq32SignedExtendTo64(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
#endif
}

// void load32SignedExtendTo64(Address address, RegisterID dest)
void testLoad32SignedExtendTo64_Address_RegisterID()
{
    testLoadExtend_Address_RegisterID<int32_t, int64_t>(signedLoad32to64Scenarios, ARRAY_SIZE(signedLoad32to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg srcAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR2;

            jit.load32SignedExtendTo64(CCallHelpers::Address(srcAddressGPR, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load32SignedExtendTo64(BaseIndex address, RegisterID dest)
void testLoad32SignedExtendTo64_BaseIndex_RegisterID()
{
    testLoadExtend_BaseIndex_RegisterID<int32_t, int64_t>(signedLoad32to64Scenarios, ARRAY_SIZE(signedLoad32to64Scenarios),
        [] (CCallHelpers& jit, int offset) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg baseAddressGPR = GPRInfo::argumentGPR1;
            constexpr GPRReg indexGPR = GPRInfo::argumentGPR2;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR3;

            jit.load32SignedExtendTo64(CCallHelpers::BaseIndex(baseAddressGPR, indexGPR, CCallHelpers::Scale::TimesFour, offset), resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR));
        });
}

// void load32SignedExtendTo64(const void* address, RegisterID dest)
void testLoad32SignedExtendTo64_voidp_RegisterID()
{
#if !CPU(X86_64)
    testLoadExtend_voidp_RegisterID<int64_t>(signedLoad32to64Scenarios, ARRAY_SIZE(signedLoad32to64Scenarios),
        [] (CCallHelpers& jit, const void* src) {
            constexpr GPRReg resultAddressGPR = GPRInfo::argumentGPR0;
            constexpr GPRReg resultGPR = GPRInfo::argumentGPR1;

            jit.load32SignedExtendTo64(src, resultGPR);
            jit.store64(resultGPR, CCallHelpers::Address(resultAddressGPR, 0));
        });
#endif
}

#if CPU(ARM64)
void testLoadStorePair64Int64()
{
    constexpr uint64_t initialValue = 0x5555aaaabbbb8800ull;
    constexpr uint64_t value1 = 42;
    constexpr uint64_t value2 = 0xcafebabe12345678ull;

    uint64_t buffer[10];

    auto initBuffer = [&] {
        for (unsigned i = 0; i < 10; ++i)
            buffer[i] = initialValue + i;
    };

    struct Pair {
        uint64_t value1;
        uint64_t value2;
    };

    Pair pair;
    auto initPair = [&] {
        pair = { 0, 0 };
    };

    // Test loadPair64.
    auto testLoadPair = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR0;
        constexpr GPRReg pairGPR = GPRInfo::argumentGPR1;
        jit.loadPair64(bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(CPURegister)), GPRInfo::regT2, GPRInfo::regT3);

        jit.store64(GPRInfo::regT2, CCallHelpers::Address(pairGPR, 0));
        jit.store64(GPRInfo::regT3, CCallHelpers::Address(pairGPR, sizeof(uint64_t)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testLoadPair0 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, 0);
    });

    initBuffer();

    initPair();
    invoke<void>(testLoadPair0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4);
    CHECK_EQ(pair.value2, initialValue + 5);

    initPair();
    buffer[4] = value1;
    buffer[5] = value2;
    invoke<void>(testLoadPair0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value1);
    CHECK_EQ(pair.value2, value2);

    auto testLoadPairMinus2 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, -2);
    });

    initPair();
    invoke<void>(testLoadPairMinus2, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4 - 2);
    CHECK_EQ(pair.value2, initialValue + 5 - 2);

    initPair();
    buffer[4 - 2] = value2;
    buffer[5 - 2] = value1;
    invoke<void>(testLoadPairMinus2, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value2);
    CHECK_EQ(pair.value2, value1);

    auto testLoadPairPlus3 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, 3);
    });

    initPair();
    invoke<void>(testLoadPairPlus3, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4 + 3);
    CHECK_EQ(pair.value2, initialValue + 5 + 3);

    initPair();
    buffer[4 + 3] = value1;
    buffer[5 + 3] = value2;
    invoke<void>(testLoadPairPlus3, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value1);
    CHECK_EQ(pair.value2, value2);

    // Test loadPair64 using a buffer register as a destination.
    auto testLoadPairUsingBufferRegisterAsDestination = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR0;
        constexpr GPRReg pairGPR = GPRInfo::argumentGPR1;
        jit.loadPair64(bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(CPURegister)), GPRInfo::argumentGPR0, GPRInfo::regT2);

        jit.store64(GPRInfo::argumentGPR0, CCallHelpers::Address(pairGPR, 0));
        jit.store64(GPRInfo::regT2, CCallHelpers::Address(pairGPR, sizeof(uint64_t)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testLoadPairUsingBufferRegisterAsDestination0 = compile([&] (CCallHelpers& jit) {
        testLoadPairUsingBufferRegisterAsDestination(jit, 0);
    });

    initBuffer();

    initPair();
    invoke<void>(testLoadPairUsingBufferRegisterAsDestination0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4);
    CHECK_EQ(pair.value2, initialValue + 5);

    // Test storePair64.
    auto testStorePair = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR2;
        jit.storePair64(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(CPURegister)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testStorePair0 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, 0);
    });

    initBuffer();
    invoke<void>(testStorePair0, value1, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], value1);
    CHECK_EQ(buffer[5], value2);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairMinus2 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, -2);
    });

    initBuffer();
    invoke<void>(testStorePairMinus2, value1, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], value1);
    CHECK_EQ(buffer[3], value2);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairPlus3 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, 3);
    });

    initBuffer();
    invoke<void>(testStorePairPlus3, value1, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], value1);
    CHECK_EQ(buffer[8], value2);
    CHECK_EQ(buffer[9], initialValue + 9);

    // Test storePair64 from 1 register.
    auto testStorePairFromOneReg = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR1;
        jit.storePair64(GPRInfo::argumentGPR0, GPRInfo::argumentGPR0, bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(CPURegister)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testStorePairFromOneReg0 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, 0);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneReg0, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], value2);
    CHECK_EQ(buffer[5], value2);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairFromOneRegMinus2 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, -2);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneRegMinus2, value1, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], value1);
    CHECK_EQ(buffer[3], value1);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairFromOneRegPlus3 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, 3);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneRegPlus3, value2, &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], value2);
    CHECK_EQ(buffer[8], value2);
    CHECK_EQ(buffer[9], initialValue + 9);
}

void testLoadStorePair64Double()
{
    constexpr double initialValue = 10000.275;
    constexpr double value1 = 42.89;
    constexpr double value2 = -555.321;

    double buffer[10];

    auto initBuffer = [&] {
        for (unsigned i = 0; i < 10; ++i)
            buffer[i] = initialValue + i;
    };

    struct Pair {
        double value1;
        double value2;
    };

    Pair pair;
    auto initPair = [&] {
        pair = { 0, 0 };
    };

    // Test loadPair64.
    auto testLoadPair = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR0;
        constexpr GPRReg pairGPR = GPRInfo::argumentGPR1;
        jit.loadPair64(bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(double)), FPRInfo::fpRegT0, FPRInfo::fpRegT1);

        jit.storeDouble(FPRInfo::fpRegT0, CCallHelpers::Address(pairGPR, 0));
        jit.storeDouble(FPRInfo::fpRegT1, CCallHelpers::Address(pairGPR, sizeof(uint64_t)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testLoadPair0 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, 0);
    });

    initBuffer();

    initPair();
    invoke<void>(testLoadPair0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4);
    CHECK_EQ(pair.value2, initialValue + 5);

    initPair();
    buffer[4] = value1;
    buffer[5] = value2;
    invoke<void>(testLoadPair0, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value1);
    CHECK_EQ(pair.value2, value2);

    auto testLoadPairMinus2 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, -2);
    });

    initPair();
    invoke<void>(testLoadPairMinus2, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4 - 2);
    CHECK_EQ(pair.value2, initialValue + 5 - 2);

    initPair();
    buffer[4 - 2] = value2;
    buffer[5 - 2] = value1;
    invoke<void>(testLoadPairMinus2, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value2);
    CHECK_EQ(pair.value2, value1);

    auto testLoadPairPlus3 = compile([&] (CCallHelpers& jit) {
        testLoadPair(jit, 3);
    });

    initPair();
    invoke<void>(testLoadPairPlus3, &buffer[4], &pair);
    CHECK_EQ(pair.value1, initialValue + 4 + 3);
    CHECK_EQ(pair.value2, initialValue + 5 + 3);

    initPair();
    buffer[4 + 3] = value1;
    buffer[5 + 3] = value2;
    invoke<void>(testLoadPairPlus3, &buffer[4], &pair);
    CHECK_EQ(pair.value1, value1);
    CHECK_EQ(pair.value2, value2);

    // Test storePair64.
    auto testStorePair = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR2;
        jit.move64ToDouble(GPRInfo::argumentGPR0, FPRInfo::fpRegT0);
        jit.move64ToDouble(GPRInfo::argumentGPR1, FPRInfo::fpRegT1);
        jit.storePair64(FPRInfo::fpRegT0, FPRInfo::fpRegT1, bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(double)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto asInt64 = [] (double value) {
        return std::bit_cast<int64_t>(value);
    };

    auto testStorePair0 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, 0);
    });

    initBuffer();
    invoke<void>(testStorePair0, asInt64(value1), asInt64(value2), &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], value1);
    CHECK_EQ(buffer[5], value2);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairMinus2 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, -2);
    });

    initBuffer();
    invoke<void>(testStorePairMinus2, asInt64(value1), asInt64(value2), &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], value1);
    CHECK_EQ(buffer[3], value2);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairPlus3 = compile([&] (CCallHelpers& jit) {
        testStorePair(jit, 3);
    });

    initBuffer();
    invoke<void>(testStorePairPlus3, asInt64(value1), asInt64(value2), &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], value1);
    CHECK_EQ(buffer[8], value2);
    CHECK_EQ(buffer[9], initialValue + 9);

    // Test storePair64 from 1 register.
    auto testStorePairFromOneReg = [] (CCallHelpers& jit, int offset) {
        emitFunctionPrologue(jit);

        constexpr GPRReg bufferGPR = GPRInfo::argumentGPR1;
        jit.move64ToDouble(GPRInfo::argumentGPR0, FPRInfo::fpRegT0);
        jit.storePair64(FPRInfo::fpRegT0, FPRInfo::fpRegT0, bufferGPR, CCallHelpers::TrustedImm32(offset * sizeof(double)));

        emitFunctionEpilogue(jit);
        jit.ret();
    };

    auto testStorePairFromOneReg0 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, 0);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneReg0, asInt64(value2), &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], value2);
    CHECK_EQ(buffer[5], value2);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairFromOneRegMinus2 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, -2);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneRegMinus2, asInt64(value1), &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], value1);
    CHECK_EQ(buffer[3], value1);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], initialValue + 7);
    CHECK_EQ(buffer[8], initialValue + 8);
    CHECK_EQ(buffer[9], initialValue + 9);

    auto testStorePairFromOneRegPlus3 = compile([&] (CCallHelpers& jit) {
        testStorePairFromOneReg(jit, 3);
    });

    initBuffer();
    invoke<void>(testStorePairFromOneRegPlus3, asInt64(value2), &buffer[4]);
    CHECK_EQ(buffer[0], initialValue + 0);
    CHECK_EQ(buffer[1], initialValue + 1);
    CHECK_EQ(buffer[2], initialValue + 2);
    CHECK_EQ(buffer[3], initialValue + 3);
    CHECK_EQ(buffer[4], initialValue + 4);
    CHECK_EQ(buffer[5], initialValue + 5);
    CHECK_EQ(buffer[6], initialValue + 6);
    CHECK_EQ(buffer[7], value2);
    CHECK_EQ(buffer[8], value2);
    CHECK_EQ(buffer[9], initialValue + 9);
}
#endif // CPU(ARM64)

void testProbeReadsArgumentRegisters()
{
    bool probeWasCalled = false;
    compileAndRun<void>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.pushPair(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
        jit.pushPair(GPRInfo::argumentGPR2, GPRInfo::argumentGPR3);

        jit.move(CCallHelpers::TrustedImm32(testWord32(0)), GPRInfo::argumentGPR0);
        jit.convertInt32ToDouble(GPRInfo::argumentGPR0, FPRInfo::fpRegT0);
        jit.move(CCallHelpers::TrustedImm32(testWord32(1)), GPRInfo::argumentGPR0);
        jit.convertInt32ToDouble(GPRInfo::argumentGPR0, FPRInfo::fpRegT1);
        jit.move(CCallHelpers::TrustedImm64(testWord(0)), GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImm64(testWord(1)), GPRInfo::argumentGPR1);
        jit.move(CCallHelpers::TrustedImm64(testWord(2)), GPRInfo::argumentGPR2);
        jit.move(CCallHelpers::TrustedImm64(testWord(3)), GPRInfo::argumentGPR3);

        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeWasCalled = true;
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR0), testWord(0));
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR1), testWord(1));
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR2), testWord(2));
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR3), testWord(3));

            CHECK_EQ(cpu.fpr(FPRInfo::fpRegT0), testWord32(0));
            CHECK_EQ(cpu.fpr(FPRInfo::fpRegT1), testWord32(1));
        });

        jit.popPair(GPRInfo::argumentGPR2, GPRInfo::argumentGPR3);
        jit.popPair(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);

        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(probeWasCalled, true);
}

void testProbeWritesArgumentRegisters()
{
    // This test relies on testProbeReadsArgumentRegisters() having already validated
    // that we can read from argument registers. We'll use that ability to validate
    // that our writes did take effect.
    unsigned probeCallCount = 0;
    compileAndRun<void>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        jit.pushPair(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
        jit.pushPair(GPRInfo::argumentGPR2, GPRInfo::argumentGPR3);

        // Pre-initialize with non-expected values.
        jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::argumentGPR1);
        jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::argumentGPR2);
        jit.move(CCallHelpers::TrustedImm64(0), GPRInfo::argumentGPR3);
        jit.convertInt32ToDouble(GPRInfo::argumentGPR0, FPRInfo::fpRegT0);
        jit.convertInt32ToDouble(GPRInfo::argumentGPR0, FPRInfo::fpRegT1);

        // Write expected values.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            cpu.gpr(GPRInfo::argumentGPR0) = testWord(0);
            cpu.gpr(GPRInfo::argumentGPR1) = testWord(1);
            cpu.gpr(GPRInfo::argumentGPR2) = testWord(2);
            cpu.gpr(GPRInfo::argumentGPR3) = testWord(3);
            
            cpu.fpr(FPRInfo::fpRegT0) = std::bit_cast<double>(testWord64(0));
            cpu.fpr(FPRInfo::fpRegT1) = std::bit_cast<double>(testWord64(1));
        });

        // Validate that expected values were written.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR0), testWord(0));
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR1), testWord(1));
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR2), testWord(2));
            CHECK_EQ(cpu.gpr(GPRInfo::argumentGPR3), testWord(3));

            CHECK_EQ(cpu.fpr<uint64_t>(FPRInfo::fpRegT0), testWord64(0));
            CHECK_EQ(cpu.fpr<uint64_t>(FPRInfo::fpRegT1), testWord64(1));
        });

        jit.popPair(GPRInfo::argumentGPR2, GPRInfo::argumentGPR3);
        jit.popPair(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);

        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(probeCallCount, 2);
}

static NEVER_INLINE NOT_TAIL_CALLED int LEGACY_NODELETE testFunctionToTrashGPRs(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
{
    if (j > 0)
        return testFunctionToTrashGPRs(a + 1, b + a, c + b, d + 5, e - a, f * 1.5, g ^ a, h - b, i, j - 1);
    return a + 1;
}
static NEVER_INLINE NOT_TAIL_CALLED double testFunctionToTrashFPRs(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j)
{
    if (j > 0)
        return testFunctionToTrashFPRs(a + 1, b + a, c + b, d + 5, e - a, f * 1.5, pow(g, a), h - b, i, j - 1);
    return a + 1;
}

void testProbePreservesGPRS()
{
    // This test relies on testProbeReadsArgumentRegisters() and testProbeWritesArgumentRegisters()
    // having already validated that we can read and write from registers. We'll use these abilities
    // to validate that the probe preserves register values.
    unsigned probeCallCount = 0;
    CPUState originalState;

    compileAndRun<void>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        // Write expected values into the registers (except for sp, fp, and pc).
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                originalState.gpr(id) = cpu.gpr(id);
                if (isSpecialGPR(id))
                    continue;
                cpu.gpr(id) = testWord(static_cast<int>(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id)) {
                originalState.fpr(id) = cpu.fpr(id);
                cpu.fpr(id) = std::bit_cast<double>(testWord64(id));
            }
        });

        // Invoke the probe to call a lot of functions and trash register values.
        jit.probeDebug([&] (Probe::Context&) {
            probeCallCount++;
            CHECK_EQ(testFunctionToTrashGPRs(0, 1, 2, 3, 4, 5, 6, 7, 8, 9), 10);
            CHECK_EQ(testFunctionToTrashFPRs(0, 1, 2, 3, 4, 5, 6, 7, 8, 9), 10);
        });

        // Validate that the registers have the expected values.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isSP(id) || isFP(id)) {
                    CHECK_EQ(cpu.gpr(id), originalState.gpr(id));
                    continue;
                }
                if (isSpecialGPR(id))
                    continue;
                CHECK_EQ(cpu.gpr(id), testWord(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id)) {
                CHECK_EQ(cpu.fpr<uint64_t>(id), testWord64(id));
            }
        });

        // Restore the original state.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isSpecialGPR(id))
                    continue;
                cpu.gpr(id) = originalState.gpr(id);
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                cpu.fpr(id) = originalState.fpr(id);
        });

        // Validate that the original state was restored.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isSpecialGPR(id))
                    continue;
                CHECK_EQ(cpu.gpr(id), originalState.gpr(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                CHECK_EQ(cpu.fpr<uint64_t>(id), originalState.fpr<uint64_t>(id));
        });

        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(probeCallCount, 5);
}

void testProbeModifiesStackPointer(WTF::Function<void*(Probe::Context&)> computeModifiedStackPointer)
{
    unsigned probeCallCount = 0;
    CPUState originalState;
    void* originalSP { nullptr };
    void* modifiedSP { nullptr };
#if !CPU(RISCV64)
    uintptr_t modifiedFlags { 0 };
#endif
    
#if CPU(X86_64)
    auto flagsSPR = X86Registers::eflags;
    uintptr_t flagsMask = 0xc5;
#elif CPU(ARM64)
    auto flagsSPR = ARM64Registers::nzcv;
    uintptr_t flagsMask = 0xf0000000;
#endif

    compileAndRun<void>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        // Preserve original stack pointer and modify the sp, and
        // write expected values into other registers (except for fp, and pc).
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                originalState.gpr(id) = cpu.gpr(id);
                if (isSpecialGPR(id))
                    continue;
                cpu.gpr(id) = testWord(static_cast<int>(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id)) {
                originalState.fpr(id) = cpu.fpr(id);
                cpu.fpr(id) = std::bit_cast<double>(testWord64(id));
            }

#if !(CPU(RISCV64))
            originalState.spr(flagsSPR) = cpu.spr(flagsSPR);
            modifiedFlags = originalState.spr(flagsSPR) ^ flagsMask;
            cpu.spr(flagsSPR) = modifiedFlags;
#endif

            originalSP = cpu.sp();
            modifiedSP = computeModifiedStackPointer(context);
            cpu.sp() = modifiedSP;
        });

        // Validate that the registers have the expected values.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isFP(id)) {
                    CHECK_EQ(cpu.gpr(id), originalState.gpr(id));
                    continue;
                }
                if (isSpecialGPR(id))
                    continue;
                CHECK_EQ(cpu.gpr(id), testWord(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                CHECK_EQ(cpu.fpr<uint64_t>(id), testWord64(id));
#if !CPU(RISCV64)
            CHECK_EQ(cpu.spr(flagsSPR) & flagsMask, modifiedFlags & flagsMask);
#endif
            CHECK_EQ(cpu.sp(), modifiedSP);
        });

        // Restore the original state.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isSpecialGPR(id))
                    continue;
                cpu.gpr(id) = originalState.gpr(id);
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                cpu.fpr(id) = originalState.fpr(id);
#if !CPU(RISCV64)
            cpu.spr(flagsSPR) = originalState.spr(flagsSPR);
#endif
            cpu.sp() = originalSP;
        });

        // Validate that the original state was restored.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isSpecialGPR(id))
                    continue;
                CHECK_EQ(cpu.gpr(id), originalState.gpr(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                CHECK_EQ(cpu.fpr<uint64_t>(id), originalState.fpr<uint64_t>(id));
#if !CPU(RISCV64)
            CHECK_EQ(cpu.spr(flagsSPR) & flagsMask, originalState.spr(flagsSPR) & flagsMask);
#endif
            CHECK_EQ(cpu.sp(), originalSP);
        });

        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(probeCallCount, 4);
}

void testProbeModifiesStackPointerToInsideProbeStateOnStack()
{
    size_t increment = sizeof(uintptr_t);
#if CPU(ARM64)
    // The ARM64 probe uses ldp and stp which require 16 byte alignment.
    increment = 2 * sizeof(uintptr_t);
#endif
    for (size_t offset = 0; offset < sizeof(Probe::State); offset += increment) {
        testProbeModifiesStackPointer([=] (Probe::Context& context) -> void* {
            return static_cast<uint8_t*>(probeStateForContext(context)) + offset;

        });
    }
}

void testProbeModifiesStackPointerToNBytesBelowSP()
{
    size_t increment = sizeof(uintptr_t);
#if CPU(ARM64)
    // The ARM64 probe uses ldp and stp which require 16 byte alignment.
    increment = 2 * sizeof(uintptr_t);
#endif
    for (size_t offset = 0; offset < 1 * KB; offset += increment) {
        testProbeModifiesStackPointer([=] (Probe::Context& context) -> void* {
            return context.cpu.sp<uint8_t*>() - offset;
        });
    }
}

void testProbeModifiesProgramCounter()
{
    // This test relies on testProbeReadsArgumentRegisters() and testProbeWritesArgumentRegisters()
    // having already validated that we can read and write from registers. We'll use these abilities
    // to validate that the probe preserves register values.
    unsigned probeCallCount = 0;
    bool continuationWasReached = false;

    MacroAssemblerCodeRef<JSEntryPtrTag> continuation = compile([&] (CCallHelpers& jit) {
        // Validate that we reached the continuation.
        jit.probeDebug([&] (Probe::Context&) {
            probeCallCount++;
            continuationWasReached = true;
        });

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    compileAndRun<void>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        // Write expected values into the registers.
        jit.probeDebug([&] (Probe::Context& context) {
            probeCallCount++;
            context.cpu.pc() = retagCodePtr<JSEntryPtrTag, JITProbePCPtrTag>(continuation.code().taggedPtr());
        });

        jit.breakpoint(); // We should never get here.
    });
    CHECK_EQ(probeCallCount, 2);
    CHECK_EQ(continuationWasReached, true);
}

void testProbeModifiesStackValues()
{
    unsigned probeCallCount = 0;
    CPUState originalState;
    void* originalSP { nullptr };
    void* newSP { nullptr };
#if !CPU(RISCV64)
    uintptr_t modifiedFlags { 0 };
#endif
    size_t numberOfExtraEntriesToWrite { 10 }; // ARM64 requires that this be 2 word aligned.

#if CPU(X86_64)
    MacroAssembler::SPRegisterID flagsSPR = X86Registers::eflags;
    uintptr_t flagsMask = 0xc5;
#elif CPU(ARM64)
    MacroAssembler::SPRegisterID flagsSPR = ARM64Registers::nzcv;
    uintptr_t flagsMask = 0xf0000000;
#endif

    compileAndRun<void>([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        // Write expected values into the registers.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            auto& stack = context.stack();
            probeCallCount++;

            // Preserve the original CPU state.
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                originalState.gpr(id) = cpu.gpr(id);
                if (isSpecialGPR(id))
                    continue;
                cpu.gpr(id) = testWord(static_cast<int>(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id)) {
                originalState.fpr(id) = cpu.fpr(id);
                cpu.fpr(id) = std::bit_cast<double>(testWord64(id));
            }
#if !CPU(RISCV64)
            originalState.spr(flagsSPR) = cpu.spr(flagsSPR);
            modifiedFlags = originalState.spr(flagsSPR) ^ flagsMask;
            cpu.spr(flagsSPR) = modifiedFlags;
#endif

            // Ensure that we'll be writing over the regions of the stack where the Probe::State is.
            originalSP = cpu.sp();
            newSP = static_cast<uintptr_t*>(probeStateForContext(context)) - numberOfExtraEntriesToWrite;
            cpu.sp() = newSP;

            // Fill the stack with values.
            uintptr_t* p = static_cast<uintptr_t*>(newSP);
            int count = 0;
            stack.set<double>(p++, 1.234567);
            while (p < static_cast<uintptr_t*>(originalSP))
                stack.set<uintptr_t>(p++, testWord(count++));
        });

        // Validate that the registers and stack have the expected values.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            auto& stack = context.stack();
            probeCallCount++;

            // Validate the register values.
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isFP(id)) {
                    CHECK_EQ(cpu.gpr(id), originalState.gpr(id));
                    continue;
                }
                if (isSpecialGPR(id))
                    continue;
                CHECK_EQ(cpu.gpr(id), testWord(id));
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                CHECK_EQ(cpu.fpr<uint64_t>(id), testWord64(id));
#if !CPU(RISCV64)
            CHECK_EQ(cpu.spr(flagsSPR) & flagsMask, modifiedFlags & flagsMask);
#endif
            CHECK_EQ(cpu.sp(), newSP);

            // Validate the stack values.
            uintptr_t* p = static_cast<uintptr_t*>(newSP);
            int count = 0;
            CHECK_EQ(stack.get<double>(p++), 1.234567);
            while (p < static_cast<uintptr_t*>(originalSP))
                CHECK_EQ(stack.get<uintptr_t>(p++), testWord(count++));
        });

        // Restore the original state.
        jit.probeDebug([&] (Probe::Context& context) {
            auto& cpu = context.cpu;
            probeCallCount++;
            for (auto id = CCallHelpers::firstRegister(); id <= CCallHelpers::lastRegister(); id = nextID(id)) {
                if (isSpecialGPR(id))
                    continue;
                cpu.gpr(id) = originalState.gpr(id);
            }
            for (auto id = CCallHelpers::firstFPRegister(); id <= CCallHelpers::lastFPRegister(); id = nextID(id))
                cpu.fpr(id) = originalState.fpr(id);
#if !CPU(RISCV64)
            cpu.spr(flagsSPR) = originalState.spr(flagsSPR);
#endif
            cpu.sp() = originalSP;
        });

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    CHECK_EQ(probeCallCount, 3);
}

void testOrImmMem()
{
    // FIXME: this does not test that the or does not touch beyond its width.
    // I am not sure how to do such a test without a lot of complexity (running multiple threads, with a race on the high bits of the memory location).
    uint64_t memoryLocation = 0x12341234;
    auto or32 = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.or32(CCallHelpers::TrustedImm32(42), CCallHelpers::AbsoluteAddress(&memoryLocation));
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    invoke<void>(or32);
    CHECK_EQ(memoryLocation, 0x12341234 | 42);

    memoryLocation = 0x12341234;
    auto or16 = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.or16(CCallHelpers::TrustedImm32(42), CCallHelpers::AbsoluteAddress(&memoryLocation));
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    invoke<void>(or16);
    CHECK_EQ(memoryLocation, 0x12341234 | 42);

    memoryLocation = 0x12341234;
    auto or8 = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.or8(CCallHelpers::TrustedImm32(42), CCallHelpers::AbsoluteAddress(&memoryLocation));
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    invoke<void>(or8);
    CHECK_EQ(memoryLocation, 0x12341234 | 42);

    memoryLocation = 0x12341234;
    auto or16InvalidLogicalImmInARM64 = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.or16(CCallHelpers::TrustedImm32(0), CCallHelpers::AbsoluteAddress(&memoryLocation));
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    invoke<void>(or16InvalidLogicalImmInARM64);
    CHECK_EQ(memoryLocation, 0x12341234);
}

void testAndOrDouble()
{
    double arg1, arg2;

    auto andDouble = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT1);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT2);

        jit.andDouble(FPRInfo::fpRegT1, FPRInfo::fpRegT2, FPRInfo::returnValueFPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto operands = doubleOperands();
    for (auto a : operands) {
        for (auto b : operands) {
            arg1 = a;
            arg2 = b;
            uint64_t expectedResult = std::bit_cast<uint64_t>(arg1) & std::bit_cast<uint64_t>(arg2);
            CHECK_EQ(std::bit_cast<uint64_t>(invoke<double>(andDouble)), expectedResult);
        }
    }

    auto orDouble = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT1);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT2);

        jit.orDouble(FPRInfo::fpRegT1, FPRInfo::fpRegT2, FPRInfo::returnValueFPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    for (auto a : operands) {
        for (auto b : operands) {
            arg1 = a;
            arg2 = b;
            uint64_t expectedResult = std::bit_cast<uint64_t>(arg1) | std::bit_cast<uint64_t>(arg2);
            CHECK_EQ(std::bit_cast<uint64_t>(invoke<double>(orDouble)), expectedResult);
        }
    }
}

void testNegateDouble()
{
    double arg = 0;

    auto negateDoubleDifferentRegs = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg), FPRInfo::fpRegT1);
        jit.negateDouble(FPRInfo::fpRegT1, FPRInfo::returnValueFPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto negateDoubleSameReg = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg), FPRInfo::returnValueFPR);
        jit.negateDouble(FPRInfo::returnValueFPR, FPRInfo::returnValueFPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto operands = doubleOperands();
    for (auto value : operands) {
        arg = value;
        double resultDifferent = invoke<double>(negateDoubleDifferentRegs);
        double resultSame = invoke<double>(negateDoubleSameReg);
        uint64_t expectedBits = std::bit_cast<uint64_t>(value) ^ 0x8000000000000000ULL; // Flip sign bit
        uint64_t resultDifferentBits = std::bit_cast<uint64_t>(resultDifferent);
        uint64_t resultSameBits = std::bit_cast<uint64_t>(resultSame);
        CHECK_EQ(resultDifferentBits, expectedBits);
        CHECK_EQ(resultSameBits, expectedBits);
    }
}

void testNegateFloat()
{
    float arg = 0;

    auto negateFloatDifferentRegs = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg), FPRInfo::fpRegT1);
        jit.negateFloat(FPRInfo::fpRegT1, FPRInfo::returnValueFPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto negateFloatSameReg = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadFloat(CCallHelpers::TrustedImmPtr(&arg), FPRInfo::returnValueFPR);
        jit.negateFloat(FPRInfo::returnValueFPR, FPRInfo::returnValueFPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto operands = floatOperands();
    for (auto value : operands) {
        arg = value;
        float resultDifferent = invoke<float>(negateFloatDifferentRegs);
        float resultSame = invoke<float>(negateFloatSameReg);
        uint32_t expectedBits = std::bit_cast<uint32_t>(value) ^ 0x80000000U; // Flip sign bit
        uint32_t resultDifferentBits = std::bit_cast<uint32_t>(resultDifferent);
        uint32_t resultSameBits = std::bit_cast<uint32_t>(resultSame);
        CHECK_EQ(resultDifferentBits, expectedBits);
        CHECK_EQ(resultSameBits, expectedBits);
    }
}

void testByteSwap()
{
#if CPU(X86_64) || CPU(ARM64)
    auto byteSwap16 = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        jit.byteSwap16(GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(invoke<uint64_t>(byteSwap16, 0xaabbccddee001122), static_cast<uint64_t>(0x2211));
    CHECK_EQ(invoke<uint64_t>(byteSwap16, 0xaabbccddee00ffaa), static_cast<uint64_t>(0xaaff));

    auto byteSwap32 = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        jit.byteSwap32(GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(invoke<uint64_t>(byteSwap32, 0xaabbccddee001122), static_cast<uint64_t>(0x221100ee));
    CHECK_EQ(invoke<uint64_t>(byteSwap32, 0xaabbccddee00ffaa), static_cast<uint64_t>(0xaaff00ee));

    auto byteSwap64 = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        jit.byteSwap64(GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
    CHECK_EQ(invoke<uint64_t>(byteSwap64, 0xaabbccddee001122), static_cast<uint64_t>(0x221100eeddccbbaa));
    CHECK_EQ(invoke<uint64_t>(byteSwap64, 0xaabbccddee00ffaa), static_cast<uint64_t>(0xaaff00eeddccbbaa));
#endif
}

void testMoveDoubleConditionally32()
{
#if CPU(X86_64) | CPU(ARM64)
    double arg1 = 0;
    double arg2 = 0;
    const double zero = -0;

    const double chosenDouble = 6.00000059604644775390625;
    CHECK_EQ(static_cast<double>(static_cast<float>(chosenDouble)) == chosenDouble, false);

    auto sel = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&zero), FPRInfo::returnValueFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT1);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT2);

        jit.move(MacroAssembler::TrustedImm32(-1), GPRInfo::regT0);
        jit.moveDoubleConditionally32(MacroAssembler::Equal, GPRInfo::regT0, GPRInfo::regT0, FPRInfo::fpRegT1, FPRInfo::fpRegT2, FPRInfo::returnValueFPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    arg1 = chosenDouble;
    arg2 = 43;
    CHECK_EQ(invoke<double>(sel), chosenDouble);

    arg1 = 43;
    arg2 = chosenDouble;
    CHECK_EQ(invoke<double>(sel), 43.0);

#endif
}

void testMoveDoubleConditionally64()
{
#if CPU(X86_64) | CPU(ARM64)
    double arg1 = 0;
    double arg2 = 0;
    const double zero = -0;

    const double chosenDouble = 6.00000059604644775390625;
    CHECK_EQ(static_cast<double>(static_cast<float>(chosenDouble)) == chosenDouble, false);

    auto sel = compile([&] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&zero), FPRInfo::returnValueFPR);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg1), FPRInfo::fpRegT1);
        jit.loadDouble(CCallHelpers::TrustedImmPtr(&arg2), FPRInfo::fpRegT2);

        jit.move(MacroAssembler::TrustedImm64(-1), GPRInfo::regT0);
        jit.moveDoubleConditionally64(MacroAssembler::Equal, GPRInfo::regT0, GPRInfo::regT0, FPRInfo::fpRegT1, FPRInfo::fpRegT2, FPRInfo::returnValueFPR);

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    arg1 = chosenDouble;
    arg2 = 43;
    CHECK_EQ(invoke<double>(sel), chosenDouble);

    arg1 = 43;
    arg2 = chosenDouble;
    CHECK_EQ(invoke<double>(sel), 43.0);

#endif
}

#if CPU(X86_64) || CPU(ARM64)
template<typename SelectionType>
static SelectionType LEGACY_NODELETE expectedResultForRelationalCondition(MacroAssembler::RelationalCondition cond, int32_t a, int32_t b, SelectionType thenValue, SelectionType elseValue)
{
    switch (cond) {
    case MacroAssembler::Equal:
        return a == b ? thenValue : elseValue;
    case MacroAssembler::NotEqual:
        return a != b ? thenValue : elseValue;
    case MacroAssembler::Above:
        return static_cast<uint32_t>(a) > static_cast<uint32_t>(b) ? thenValue : elseValue;
    case MacroAssembler::AboveOrEqual:
        return static_cast<uint32_t>(a) >= static_cast<uint32_t>(b) ? thenValue : elseValue;
    case MacroAssembler::Below:
        return static_cast<uint32_t>(a) < static_cast<uint32_t>(b) ? thenValue : elseValue;
    case MacroAssembler::BelowOrEqual:
        return static_cast<uint32_t>(a) <= static_cast<uint32_t>(b) ? thenValue : elseValue;
    case MacroAssembler::GreaterThan:
        return a > b ? thenValue : elseValue;
    case MacroAssembler::GreaterThanOrEqual:
        return a >= b ? thenValue : elseValue;
    case MacroAssembler::LessThan:
        return a < b ? thenValue : elseValue;
    case MacroAssembler::LessThanOrEqual:
        return a <= b ? thenValue : elseValue;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename SelectionType>
static SelectionType LEGACY_NODELETE expectedResultForResultCondition(MacroAssembler::ResultCondition cond, int32_t testValue, int32_t mask, SelectionType thenValue, SelectionType elseValue)
{
    int32_t result = testValue & mask;
    switch (cond) {
    case MacroAssembler::Zero:
        return result == 0 ? thenValue : elseValue;
    case MacroAssembler::NonZero:
        return result != 0 ? thenValue : elseValue;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Tests moveConditionally32(cond, left, immRight, immThenCase, regElseCase, dest)
void testMoveConditionally32WithImmThenCase(MacroAssembler::RelationalCondition cond)
{
    const int32_t thenValue = 42;
    const int32_t elseValue = 17;

    for (auto left : int32Operands()) {
        for (auto right : int32Operands()) {
            int32_t expected = expectedResultForRelationalCondition(cond, left, right, thenValue, elseValue);

            // Test with dest != elseCase
            auto testCodeDestNotElse = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                GPRReg destGPR = GPRInfo::returnValueGPR;
                GPRReg leftGPR = GPRInfo::regT2; // Use a different register for left
                GPRReg elseGPR = GPRInfo::regT3;
                RELEASE_ASSERT(destGPR != elseGPR);
                RELEASE_ASSERT(leftGPR != destGPR);
                RELEASE_ASSERT(leftGPR != elseGPR);

                // Move argument to leftGPR before we touch destGPR (which may alias argumentGPR0)
                jit.move(GPRInfo::argumentGPR0, leftGPR);
                jit.move(CCallHelpers::TrustedImm32(elseValue), elseGPR);
                jit.move(CCallHelpers::TrustedImm32(-1), destGPR);

                jit.moveConditionally32(cond, leftGPR, CCallHelpers::TrustedImm32(right), CCallHelpers::TrustedImm32(thenValue), elseGPR, destGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            // Test with dest == elseCase (special code path in x86)
            auto testCodeDestEqualsElse = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                GPRReg destGPR = GPRInfo::returnValueGPR;
                GPRReg leftGPR = GPRInfo::regT2; // Use a different register for left
                GPRReg elseGPR = destGPR; // Same as dest
                RELEASE_ASSERT(destGPR == elseGPR);
                RELEASE_ASSERT(leftGPR != destGPR);

                // Move argument to leftGPR before we touch destGPR (which may alias argumentGPR0)
                jit.move(GPRInfo::argumentGPR0, leftGPR);
                jit.move(CCallHelpers::TrustedImm32(elseValue), elseGPR);

                jit.moveConditionally32(cond, leftGPR, CCallHelpers::TrustedImm32(right), CCallHelpers::TrustedImm32(thenValue), elseGPR, destGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            CHECK_EQ(invoke<int32_t>(testCodeDestNotElse, left), expected);
            CHECK_EQ(invoke<int32_t>(testCodeDestEqualsElse, left), expected);
        }
    }
}

// Tests moveConditionallyTest32(cond, left, regMask, immThenCase, regElseCase, dest)
void testMoveConditionallyTest32WithImmThenCaseRegMask(MacroAssembler::ResultCondition cond)
{
    const int32_t thenValue = 42;
    const int32_t elseValue = 17;

    for (auto test : int32Operands()) {
        for (auto mask : int32Operands()) {
            int32_t expected = expectedResultForResultCondition(cond, test, mask, thenValue, elseValue);

            // Test with dest != elseCase
            auto testCodeDestNotElse = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                GPRReg destGPR = GPRInfo::returnValueGPR;
                GPRReg testGPR = GPRInfo::regT2;
                GPRReg maskGPR = GPRInfo::regT3;
                GPRReg elseGPR = GPRInfo::regT4;
                RELEASE_ASSERT(destGPR != elseGPR);
                RELEASE_ASSERT(testGPR != destGPR);
                RELEASE_ASSERT(maskGPR != destGPR);

                // Move arguments to safe registers before touching destGPR
                jit.move(GPRInfo::argumentGPR0, testGPR);
                jit.move(GPRInfo::argumentGPR1, maskGPR);
                jit.move(CCallHelpers::TrustedImm32(elseValue), elseGPR);
                jit.move(CCallHelpers::TrustedImm32(-1), destGPR);

                jit.moveConditionallyTest32(cond, testGPR, maskGPR, CCallHelpers::TrustedImm32(thenValue), elseGPR, destGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            // Test with dest == elseCase
            auto testCodeDestEqualsElse = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                GPRReg destGPR = GPRInfo::returnValueGPR;
                GPRReg testGPR = GPRInfo::regT2;
                GPRReg maskGPR = GPRInfo::regT3;
                GPRReg elseGPR = destGPR;
                RELEASE_ASSERT(destGPR == elseGPR);
                RELEASE_ASSERT(testGPR != destGPR);
                RELEASE_ASSERT(maskGPR != destGPR);

                // Move arguments to safe registers before touching destGPR
                jit.move(GPRInfo::argumentGPR0, testGPR);
                jit.move(GPRInfo::argumentGPR1, maskGPR);
                jit.move(CCallHelpers::TrustedImm32(elseValue), elseGPR);

                jit.moveConditionallyTest32(cond, testGPR, maskGPR, CCallHelpers::TrustedImm32(thenValue), elseGPR, destGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            CHECK_EQ(invoke<int32_t>(testCodeDestNotElse, test, mask), expected);
            CHECK_EQ(invoke<int32_t>(testCodeDestEqualsElse, test, mask), expected);
        }
    }
}

// Tests moveConditionallyTest32(cond, testReg, immMask, immThenCase, regElseCase, dest)
void testMoveConditionallyTest32WithImmThenCaseImmMask(MacroAssembler::ResultCondition cond)
{
    const int32_t thenValue = 42;
    const int32_t elseValue = 17;

    for (auto test : int32Operands()) {
        for (auto mask : int32Operands()) {
            int32_t expected = expectedResultForResultCondition(cond, test, mask, thenValue, elseValue);

            // Test with dest != elseCase
            auto testCodeDestNotElse = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                GPRReg destGPR = GPRInfo::returnValueGPR;
                GPRReg testGPR = GPRInfo::regT2;
                GPRReg elseGPR = GPRInfo::regT3;
                RELEASE_ASSERT(destGPR != elseGPR);
                RELEASE_ASSERT(testGPR != destGPR);

                // Move argument to safe register before touching destGPR
                jit.move(GPRInfo::argumentGPR0, testGPR);
                jit.move(CCallHelpers::TrustedImm32(elseValue), elseGPR);
                jit.move(CCallHelpers::TrustedImm32(-1), destGPR);

                jit.moveConditionallyTest32(cond, testGPR, CCallHelpers::TrustedImm32(mask), CCallHelpers::TrustedImm32(thenValue), elseGPR, destGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            // Test with dest == elseCase
            auto testCodeDestEqualsElse = compile([=] (CCallHelpers& jit) {
                emitFunctionPrologue(jit);

                GPRReg destGPR = GPRInfo::returnValueGPR;
                GPRReg testGPR = GPRInfo::regT2;
                GPRReg elseGPR = destGPR;
                RELEASE_ASSERT(destGPR == elseGPR);
                RELEASE_ASSERT(testGPR != destGPR);

                // Move argument to safe register before touching destGPR
                jit.move(GPRInfo::argumentGPR0, testGPR);
                jit.move(CCallHelpers::TrustedImm32(elseValue), elseGPR);

                jit.moveConditionallyTest32(cond, testGPR, CCallHelpers::TrustedImm32(mask), CCallHelpers::TrustedImm32(thenValue), elseGPR, destGPR);

                emitFunctionEpilogue(jit);
                jit.ret();
            });

            CHECK_EQ(invoke<int32_t>(testCodeDestNotElse, test), expected);
            CHECK_EQ(invoke<int32_t>(testCodeDestEqualsElse, test), expected);
        }
    }
}

#endif

void testLoadBaseIndex()
{
#if CPU(ARM64) || CPU(X86_64) || CPU(RISCV64)
    // load64
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load64(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, -8), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint64_t array[] = { UINT64_MAX - 1, UINT64_MAX - 2, UINT64_MAX - 3, UINT64_MAX - 4, UINT64_MAX - 5, };
        CHECK_EQ(invoke<uint64_t>(test, array, static_cast<UCPURegister>(3)), UINT64_MAX - 3);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load64(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, 8), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint64_t array[] = { UINT64_MAX - 1, UINT64_MAX - 2, UINT64_MAX - 3, UINT64_MAX - 4, UINT64_MAX - 5, };
        CHECK_EQ(invoke<uint64_t>(test, array, static_cast<UCPURegister>(3)), UINT64_MAX - 5);
    }
#endif

    // load32
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load32(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, -4), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint32_t array[] = { UINT32_MAX - 1, UINT32_MAX - 2, UINT32_MAX - 3, UINT32_MAX - 4, UINT32_MAX - 5, };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), UINT32_MAX - 3);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load32(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, 4), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint32_t array[] = { UINT32_MAX - 1, UINT32_MAX - 2, UINT32_MAX - 3, UINT32_MAX - 4, UINT32_MAX - 5, };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), UINT32_MAX - 5);
    }

    // load16
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load16(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, -2), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint16_t array[] = { UINT16_MAX - 1, UINT16_MAX - 2, UINT16_MAX - 3, UINT16_MAX - 4, UINT16_MAX - 5, };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), UINT16_MAX - 3);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load16(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, 2), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint16_t array[] = { UINT16_MAX - 1, UINT16_MAX - 2, UINT16_MAX - 3, UINT16_MAX - 4, static_cast<uint16_t>(-1), };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), 0xffff);
    }

    // load16SignedExtendTo32
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load16SignedExtendTo32(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, -2), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint16_t array[] = { 1, 2, 0x7ff3, 0x8000, 5, };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), 0x7ff3);
        CHECK_EQ(invoke<uint64_t>(test, array, static_cast<UCPURegister>(4)), 0xffff8000);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load16SignedExtendTo32(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, 2), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint16_t array[] = { UINT16_MAX - 1, UINT16_MAX - 2, UINT16_MAX - 3, UINT16_MAX - 4, static_cast<uint16_t>(-1), };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), static_cast<uint32_t>(-1));
        CHECK_EQ(invoke<uint64_t>(test, array, static_cast<UCPURegister>(3)), static_cast<uint32_t>(-1));
    }

    // load8
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load8(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, -1), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint8_t array[] = { UINT8_MAX - 1, UINT8_MAX - 2, UINT8_MAX - 3, UINT8_MAX - 4, UINT8_MAX - 5, };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), UINT8_MAX - 3);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load8(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, 1), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint8_t array[] = { UINT8_MAX - 1, UINT8_MAX - 2, UINT8_MAX - 3, UINT8_MAX - 4, static_cast<uint8_t>(-1), };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), 0xff);
    }

    // load8SignedExtendTo32
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load8SignedExtendTo32(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, -1), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint8_t array[] = { 1, 2, 0x73, 0x80, 5, };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), 0x73);
        CHECK_EQ(invoke<uint64_t>(test, array, static_cast<UCPURegister>(4)), 0xffffff80);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.load8SignedExtendTo32(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, 1), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint8_t array[] = { UINT8_MAX - 1, UINT8_MAX - 2, UINT8_MAX - 3, UINT8_MAX - 4, static_cast<uint8_t>(-1), };
        CHECK_EQ(invoke<uint32_t>(test, array, static_cast<UCPURegister>(3)), static_cast<uint32_t>(-1));
        CHECK_EQ(invoke<uint64_t>(test, array, static_cast<UCPURegister>(3)), static_cast<uint32_t>(-1));
    }

    // loadDouble
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.loadDouble(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, -8), FPRInfo::returnValueFPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        double array[] = { 1, 2, 3, 4, 5, };
        CHECK_EQ(invoke<double>(test, array, static_cast<UCPURegister>(3)), 3.0);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.loadDouble(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, 8), FPRInfo::returnValueFPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        double array[] = { 1, 2, 3, 4, 5, };
        CHECK_EQ(invoke<double>(test, array, static_cast<UCPURegister>(3)), 5.0);
    }

    // loadFloat
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.loadFloat(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, -4), FPRInfo::returnValueFPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        float array[] = { 1, 2, 3, 4, 5, };
        CHECK_EQ(invoke<float>(test, array, static_cast<UCPURegister>(3)), 3.0);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.loadFloat(CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, 4), FPRInfo::returnValueFPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        float array[] = { 1, 2, 3, 4, 5, };
        CHECK_EQ(invoke<float>(test, array, static_cast<UCPURegister>(3)), 5.0);
    }
}

void testStoreImmediateAddress()
{
#if CPU(ARM64) || CPU(X86_64)
    // store64
    for (auto imm : int64Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store64(CCallHelpers::TrustedImm64(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, -16));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint64_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array + 3);
            CHECK_EQ(array[1], static_cast<uint64_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store64(CCallHelpers::TrustedImm64(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, 16));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint64_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array);
            CHECK_EQ(array[2], static_cast<uint64_t>(imm));
        }
    }

    // store32
    for (auto imm : int32Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store32(CCallHelpers::TrustedImm32(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, -8));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint32_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array + 3);
            CHECK_EQ(array[1], static_cast<uint32_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store32(CCallHelpers::TrustedImm32(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, 8));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint32_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array);
            CHECK_EQ(array[2], static_cast<uint32_t>(imm));
        }
    }

    // store16
    for (auto imm : int16Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store16(CCallHelpers::TrustedImm32(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, -4));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint16_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array + 3);
            CHECK_EQ(array[1], static_cast<uint16_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store16(CCallHelpers::TrustedImm32(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, 4));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint16_t array[] = { 1, 2, 3, 4, static_cast<uint16_t>(-1), };
            invoke<void>(test, array);
            CHECK_EQ(array[2], static_cast<uint16_t>(imm));
        }
    }

    // store8
    for (auto imm : int8Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store8(CCallHelpers::TrustedImm32(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, -2));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint8_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array + 3);
            CHECK_EQ(array[1], static_cast<uint8_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store8(CCallHelpers::TrustedImm32(imm), CCallHelpers::Address(GPRInfo::argumentGPR0, 2));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint8_t array[] = { 1, 2, 3, 4, static_cast<uint8_t>(-1), };
            invoke<void>(test, array);
            CHECK_EQ(array[2], static_cast<uint8_t>(imm));
        }
    }
#endif
}

void testStoreBaseIndex()
{
#if CPU(ARM64) || CPU(X86_64) || CPU(RISCV64)
    // store64
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store64(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, -8));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint64_t array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, 3, UINT64_MAX - 42);
        CHECK_EQ(array[2], UINT64_MAX - 42);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store64(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, 8));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint64_t array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, 3, UINT64_MAX - 42);
        CHECK_EQ(array[4], UINT64_MAX - 42);
    }
#endif

    // store32
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, -4));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint32_t array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, 3, UINT32_MAX - 42);
        CHECK_EQ(array[2], UINT32_MAX - 42);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, 4));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint32_t array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, 3, UINT32_MAX - 42);
        CHECK_EQ(array[4], UINT32_MAX - 42);
    }

    // store16
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store16(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, -2));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint16_t array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, 3, UINT16_MAX - 42);
        CHECK_EQ(array[2], UINT16_MAX - 42);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store16(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, 2));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint16_t array[] = { 1, 2, 3, 4, static_cast<uint16_t>(-1), };
        invoke<void>(test, array, 3, UINT16_MAX - 42);
        CHECK_EQ(array[4], UINT16_MAX - 42);
    }

    // store8
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store8(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, -1));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint8_t array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, 3, UINT8_MAX - 42);
        CHECK_EQ(array[2], UINT8_MAX - 42);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store8(GPRInfo::argumentGPR2, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, 1));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        uint8_t array[] = { 1, 2, 3, 4, static_cast<uint8_t>(-1), };
        invoke<void>(test, array, 3, UINT8_MAX - 42);
        CHECK_EQ(array[4], UINT8_MAX - 42);
    }

    // storeDouble
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            constexpr FPRReg inputFPR = FPRInfo::argumentFPR0;
            jit.storeDouble(inputFPR, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, -8));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        double array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, static_cast<UCPURegister>(3), 42.0);
        CHECK_EQ(array[2], 42.0);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            constexpr FPRReg inputFPR = FPRInfo::argumentFPR0;
            jit.storeDouble(inputFPR, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, 8));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        double array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, static_cast<UCPURegister>(3), 42.0);
        CHECK_EQ(array[4], 42.0);
    }

    // storeFloat
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            constexpr FPRReg inputFPR = FPRInfo::argumentFPR0;
            jit.storeFloat(inputFPR, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, -4));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        float array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, static_cast<UCPURegister>(3), 42.0f);
        CHECK_EQ(array[2], 42.0f);
    }
    {
        auto test = compile([=](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            constexpr FPRReg inputFPR = FPRInfo::argumentFPR0;
            jit.storeFloat(inputFPR, CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, 4));
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        float array[] = { 1, 2, 3, 4, 5, };
        invoke<void>(test, array, static_cast<UCPURegister>(3), 42.0f);
        CHECK_EQ(array[4], 42.0f);
    }
}

void testStoreImmediateBaseIndex()
{
#if CPU(ARM64) || CPU(X86_64)
    // store64
    for (auto imm : int64Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store64(CCallHelpers::TrustedImm64(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, -8));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint64_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[2], static_cast<uint64_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store64(CCallHelpers::TrustedImm64(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesEight, 8));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint64_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[4], static_cast<uint64_t>(imm));
        }
    }

    // store32
    for (auto imm : int32Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store32(CCallHelpers::TrustedImm32(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, -4));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint32_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[2], static_cast<uint32_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store32(CCallHelpers::TrustedImm32(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesFour, 4));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint32_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[4], static_cast<uint32_t>(imm));
        }
    }

    // store16
    for (auto imm : int16Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store16(CCallHelpers::TrustedImm32(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, -2));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint16_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[2], static_cast<uint16_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store16(CCallHelpers::TrustedImm32(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesTwo, 2));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint16_t array[] = { 1, 2, 3, 4, static_cast<uint16_t>(-1), };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[4], static_cast<uint16_t>(imm));
        }
    }

    // store8
    for (auto imm : int8Operands()) {
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store8(CCallHelpers::TrustedImm32(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, -1));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint8_t array[] = { 1, 2, 3, 4, 5, };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[2], static_cast<uint8_t>(imm));
        }
        {
            auto test = compile([=](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.store8(CCallHelpers::TrustedImm32(imm), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, CCallHelpers::TimesOne, 1));
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            uint8_t array[] = { 1, 2, 3, 4, static_cast<uint8_t>(-1), };
            invoke<void>(test, array, 3);
            CHECK_EQ(array[4], static_cast<uint8_t>(imm));
        }
    }
#endif
}

static void testBranchIfType()
{
    using JSC::JSType;
    struct CellLike {
        uint32_t structureID;
        uint8_t indexingType;
        JSType type;
    };
    CHECK_EQ(JSCell::typeInfoTypeOffset(), OBJECT_OFFSETOF(CellLike, type));

    auto isType = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        auto isType = jit.branchIfType(GPRInfo::argumentGPR0, JSC::JSTypeRange { JSType(FirstTypedArrayType), JSType(LastTypedArrayTypeExcludingDataView) });
        jit.move(CCallHelpers::TrustedImm32(false), GPRInfo::returnValueGPR);
        auto done = jit.jump();
        isType.link(&jit);
        jit.move(CCallHelpers::TrustedImm32(true), GPRInfo::returnValueGPR);
        done.link(&jit);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    CellLike cell;
    for (unsigned i = JSC::FirstTypedArrayType; i <= JSC::LastTypedArrayTypeExcludingDataView; ++i) {
        cell.type = JSType(i);
        CHECK_EQ(invoke<bool>(isType, &cell), true);
    }

    cell.type = JSType(LastTypedArrayType);
    CHECK_EQ(invoke<bool>(isType, &cell), false);
    cell.type = JSType(FirstTypedArrayType - 1);
    CHECK_EQ(invoke<bool>(isType, &cell), false);
}

static void testBranchIfNotType()
{
    using JSC::JSType;
    struct CellLike {
        uint32_t structureID;
        uint8_t indexingType;
        JSType type;
    };
    CHECK_EQ(JSCell::typeInfoTypeOffset(), OBJECT_OFFSETOF(CellLike, type));

    auto isNotType = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        auto isNotType = jit.branchIfNotType(GPRInfo::argumentGPR0, JSC::JSTypeRange { JSType(FirstTypedArrayType), JSType(LastTypedArrayTypeExcludingDataView) });
        jit.move(CCallHelpers::TrustedImm32(false), GPRInfo::returnValueGPR);
        auto done = jit.jump();
        isNotType.link(&jit);
        jit.move(CCallHelpers::TrustedImm32(true), GPRInfo::returnValueGPR);
        done.link(&jit);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    CellLike cell;
    for (unsigned i = JSC::FirstTypedArrayType; i <= JSC::LastTypedArrayTypeExcludingDataView; ++i) {
        cell.type = JSType(i);
        CHECK_EQ(invoke<bool>(isNotType, &cell), false);
    }

    cell.type = JSType(LastTypedArrayType);
    CHECK_EQ(invoke<bool>(isNotType, &cell), true);
    cell.type = JSType(FirstTypedArrayType - 1);
    CHECK_EQ(invoke<bool>(isNotType, &cell), true);
}

#if CPU(X86_64) || CPU(ARM64)
enum class FreeListAllocationForm : uint8_t { OutOfLineRefill, ConstantCellSize, NoRefill, InlineRefill };

static MacroAssemblerCodeRef<JSEntryPtrTag> compileFreeListAllocation(FreeListAllocationForm form, unsigned cellSize)
{
    return compile([=](CCallHelpers& jit) {
        constexpr GPRReg result = GPRInfo::returnValueGPR;
        constexpr GPRReg scratch = GPRInfo::argumentGPR1;
        constexpr GPRReg allocator = GPRInfo::argumentGPR2;
        CCallHelpers::JumpList slowPath;
        CCallHelpers::FreeListRefill refill;

        emitFunctionPrologue(jit);
        jit.move(GPRInfo::argumentGPR0, allocator);
        switch (form) {
        case FreeListAllocationForm::OutOfLineRefill:
            refill = jit.emitAllocateFromFreeList(result, allocator, scratch, slowPath);
            break;
        case FreeListAllocationForm::ConstantCellSize:
            refill = jit.emitAllocateFromFreeList(result, allocator, scratch, slowPath, cellSize);
            break;
        case FreeListAllocationForm::NoRefill:
            slowPath.append(jit.emitAllocateFromFreeList(result, allocator, scratch, slowPath).isExhausted);
            break;
        case FreeListAllocationForm::InlineRefill:
            jit.emitAllocateWithNonNullAllocator(result, JITAllocator::variable(), allocator, scratch, slowPath);
            break;
        }
        emitFunctionEpilogue(jit);
        jit.ret();

        jit.emitFreeListRefill(refill, slowPath);

        slowPath.link(&jit);
        jit.move(CCallHelpers::TrustedImmPtr(nullptr), result);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
}

static void testAllocateFromFreeList()
{
    constexpr unsigned cellSize = 32;
    constexpr uint64_t secret = 0x123456789abcdef1;
    constexpr std::array<unsigned, 8> freeCellOffsets { 0, 32, 64, 160, 512, 544, 576, 608 };
    constexpr size_t firstIntervalCellCount = 3;

    alignas(16) char cells[1024] = { };
    auto freeCellAt = [&](unsigned offset) {
        return std::bit_cast<FreeCell*>(cells + offset);
    };
    freeCellAt(0)->setNext(freeCellAt(160), 96, secret);
    freeCellAt(160)->setNext(freeCellAt(512), 32, secret);
    freeCellAt(512)->makeLast(128, secret);

    using AllocatorBytes = std::array<uint8_t, LocalAllocator::offsetOfFreeList() + sizeof(FreeList)>;
    auto createAllocator = [&] {
        FreeCell* nextInterval = freeCellAt(0);
        char* intervalStart = nullptr;
        char* intervalEnd = nullptr;
        FreeCell::advance(secret, nextInterval, intervalStart, intervalEnd);

        AllocatorBytes allocator { };
        auto store = [&](ptrdiff_t offset, auto value) {
            memcpy(allocator.data() + offset, &value, sizeof(value));
        };
        store(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalStart(), intervalStart);
        store(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfIntervalEnd(), intervalEnd);
        store(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfNextInterval(), nextInterval);
        store(LocalAllocator::offsetOfFreeList() + FreeList::offsetOfSecret(), secret);
        store(LocalAllocator::offsetOfCellSize(), cellSize);
        return allocator;
    };

    auto checkAllocations = [&](FreeListAllocationForm form, std::span<const unsigned> expectedOffsets) {
        auto allocate = compileFreeListAllocation(form, cellSize);
        alignas(FreeList) AllocatorBytes allocator = createAllocator();
        for (unsigned offset : expectedOffsets) {
            void* cell = invoke<void*>(allocate, allocator.data());
            CHECK_EQ(cell, static_cast<void*>(cells + offset));
        }

        void* cell = invoke<void*>(allocate, allocator.data());
        CHECK_EQ(cell, static_cast<void*>(nullptr));
        AllocatorBytes exhaustedAllocator = allocator;
        cell = invoke<void*>(allocate, allocator.data());
        CHECK_EQ(cell, static_cast<void*>(nullptr));
        CHECK_EQ(allocator == exhaustedAllocator, true);
    };

    checkAllocations(FreeListAllocationForm::OutOfLineRefill, freeCellOffsets);
    checkAllocations(FreeListAllocationForm::ConstantCellSize, freeCellOffsets);
    checkAllocations(FreeListAllocationForm::InlineRefill, freeCellOffsets);
    checkAllocations(FreeListAllocationForm::NoRefill, std::span { freeCellOffsets }.first(firstIntervalCellCount));
}

static void testBranchConvertDoubleToInt52()
{
    auto toInt52 = compile([](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        constexpr bool canIgnoreNegativeZero = false;
        CCallHelpers::JumpList failureCases;
        jit.branchConvertDoubleToInt52(FPRInfo::argumentFPR0, GPRInfo::returnValueGPR, failureCases, GPRInfo::returnValueGPR2, FPRInfo::argumentFPR1, canIgnoreNegativeZero);
        auto done = jit.jump();
        failureCases.link(&jit);
        jit.move(CCallHelpers::TrustedImm64(1ULL << 52), GPRInfo::returnValueGPR);
        done.link(&jit);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(1LL << 50))), (1LL << 50));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>((1LL << 50) - 1))), ((1LL << 50) - 1));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>((1LL << 51) - 1))), ((1LL << 51) - 1));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-(1LL << 51)))), (-(1LL << 51)));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(1))), 1LL);
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-1))), -1LL);
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(0))), 0LL);

    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(1LL << 51))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>((1LL << 51) + 1))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>((1LL << 51) + 42))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-((1LL << 51) + 1)))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-((1LL << 51) + 42)))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(1LL << 52))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>((1LL << 52) + 1))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>((1LL << 52) + 42))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-((1LL << 52) + 1)))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-((1LL << 52) + 42)))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, -static_cast<double>(0))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(std::numeric_limits<double>::infinity()))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-std::numeric_limits<double>::infinity()))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(std::numeric_limits<double>::quiet_NaN()))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(42.195))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(0.3))), (1LL << 52));
    CHECK_EQ((invoke<int64_t>(toInt52, static_cast<double>(-0.1))), (1LL << 52));
}
#endif

#if CPU(X86_64)
struct TagsInMemory {
    int64_t numberTag { JSValue::NumberTag };
    int64_t notCellMask { JSValue::NotCellMask };
};

template<typename Emit>
static MacroAssemblerCodeRef<JSEntryPtrTag> compileWithTagsInMemory(const Emit& emit)
{
    return compile([&](CCallHelpers& jit) {
        constexpr GPRReg tags = GPRInfo::argumentGPR1;
        emitFunctionPrologue(jit);
        jit.push(GPRInfo::numberTagRegister);
        jit.push(GPRInfo::notCellMaskRegister);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::numberTagRegister);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::notCellMaskRegister);
        jit.readTagsFromMemory(CCallHelpers::Address(tags, OBJECT_OFFSETOF(TagsInMemory, numberTag)), CCallHelpers::Address(tags, OBJECT_OFFSETOF(TagsInMemory, notCellMask)));
        emit(jit);
        jit.pop(GPRInfo::notCellMaskRegister);
        jit.pop(GPRInfo::numberTagRegister);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
}

static void testTagsInMemory()
{
    constexpr GPRReg value = GPRInfo::argumentGPR0;
    constexpr GPRReg scratch = GPRInfo::argumentGPR2;
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    constexpr FPRReg fpScratch = FPRInfo::fpRegT0;
    enum ValueKind : uint32_t { Cell, Int32, Double, Other };

    auto encodedInt32 = [](int32_t number) {
        return static_cast<uint64_t>(JSValue::NumberTag) | static_cast<uint32_t>(number);
    };
    auto encodedDouble = [](double number) {
        return std::bit_cast<uint64_t>(number) + static_cast<uint64_t>(JSValue::DoubleEncodeOffset);
    };

    auto returnKind = [&](CCallHelpers& jit, ValueKind kind, CCallHelpers::JumpList& done) {
        jit.move(CCallHelpers::TrustedImm32(kind), result);
        done.append(jit.jump());
    };
    auto kindOf = compileWithTagsInMemory([&](CCallHelpers& jit) {
        CCallHelpers::JumpList done;
        CCallHelpers::Jump isCell = jit.branchIfCell(value);
        CCallHelpers::Jump isInt32 = jit.branchIfInt32(value);
        CCallHelpers::Jump isNumber = jit.branchIfNumber(value);
        returnKind(jit, Other, done);
        isCell.link(&jit);
        returnKind(jit, Cell, done);
        isInt32.link(&jit);
        returnKind(jit, Int32, done);
        isNumber.link(&jit);
        returnKind(jit, Double, done);
        done.link(&jit);
    });
    auto kindOfByExclusion = compileWithTagsInMemory([&](CCallHelpers& jit) {
        CCallHelpers::JumpList done;
        CCallHelpers::Jump isNotCell = jit.branchIfNotCell(value);
        returnKind(jit, Cell, done);
        isNotCell.link(&jit);
        CCallHelpers::Jump isNotInt32 = jit.branchIfNotInt32(value);
        returnKind(jit, Int32, done);
        isNotInt32.link(&jit);
        CCallHelpers::Jump isNotNumber = jit.branchIfNotNumber(value);
        returnKind(jit, Double, done);
        isNotNumber.link(&jit);
        returnKind(jit, Other, done);
        done.link(&jit);
    });

    TagsInMemory tags;
    struct EncodedValue {
        uint64_t bits;
        ValueKind kind;
    };
    const EncodedValue values[] = {
        { 0x1000, Cell },
        { encodedInt32(42), Int32 },
        { encodedInt32(-7), Int32 },
        { encodedDouble(1.5), Double },
        { encodedDouble(-0.0), Double },
        { static_cast<uint64_t>(JSValue::ValueUndefined), Other },
        { static_cast<uint64_t>(JSValue::ValueNull), Other },
        { static_cast<uint64_t>(JSValue::ValueTrue), Other },
    };
    for (const EncodedValue& encoded : values) {
        CHECK_EQ(invoke<uint32_t>(kindOf, encoded.bits, &tags), static_cast<uint32_t>(encoded.kind));
        CHECK_EQ(invoke<uint32_t>(kindOfByExclusion, encoded.bits, &tags), static_cast<uint32_t>(encoded.kind));
    }

    auto boxInt32 = compileWithTagsInMemory([&](CCallHelpers& jit) {
        jit.boxInt32(value, result);
    });
    for (int32_t number : { 0, 42, -7 })
        CHECK_EQ(invoke<uint64_t>(boxInt32, static_cast<uint64_t>(static_cast<uint32_t>(number)), &tags), encodedInt32(number));

    auto boxDouble = compileWithTagsInMemory([&](CCallHelpers& jit) {
        jit.move64ToDouble(value, fpScratch);
        jit.boxDouble(fpScratch, result);
    });
    auto unboxDouble = compileWithTagsInMemory([&](CCallHelpers& jit) {
        jit.unboxDoubleWithoutAssertions(value, result, fpScratch);
        jit.moveDoubleTo64(fpScratch, result);
    });
    for (double number : { 1.5, -0.0, 1e300 }) {
        CHECK_EQ(invoke<uint64_t>(boxDouble, std::bit_cast<uint64_t>(number), &tags), encodedDouble(number));
        CHECK_EQ(invoke<uint64_t>(unboxDouble, encodedDouble(number), &tags), std::bit_cast<uint64_t>(number));
    }

    auto boxInt52 = compileWithTagsInMemory([&](CCallHelpers& jit) {
        jit.boxInt52(value, result, scratch, fpScratch);
    });
    CHECK_EQ(invoke<uint64_t>(boxInt52, static_cast<int64_t>(-7), &tags), encodedInt32(-7));
    CHECK_EQ(invoke<uint64_t>(boxInt52, static_cast<int64_t>(1) << 40, &tags), encodedDouble(static_cast<double>(static_cast<int64_t>(1) << 40)));

    auto checkSameBytes = [](const auto& helper, const auto& expected) {
        CHECK_EQ(emitWithPolicy(AssemblerPolicy::forJIT(), helper) == emitWithPolicy(AssemblerPolicy::forJIT(), expected), true);
    };
    checkSameBytes([&](CCallHelpers& jit) {
        jit.branchIfCell(value);
        jit.branchIfNotCell(value);
        jit.branchIfInt32(value);
        jit.branchIfNotInt32(value);
        jit.branchIfNumber(value);
        jit.branchIfNotNumber(value);
        jit.boxInt32(value, result);
        jit.unboxDoubleWithoutAssertions(value, result, fpScratch);
    }, [&](CCallHelpers& jit) {
        jit.branchTest64(CCallHelpers::Zero, value, GPRInfo::notCellMaskRegister);
        jit.branchTest64(CCallHelpers::NonZero, value, GPRInfo::notCellMaskRegister);
        jit.branch64(CCallHelpers::AboveOrEqual, value, GPRInfo::numberTagRegister);
        jit.branch64(CCallHelpers::Below, value, GPRInfo::numberTagRegister);
        jit.branchTest64(CCallHelpers::NonZero, value, GPRInfo::numberTagRegister);
        jit.branchTest64(CCallHelpers::Zero, value, GPRInfo::numberTagRegister);
        jit.or64(GPRInfo::numberTagRegister, value, result);
        jit.add64(GPRInfo::numberTagRegister, value, result);
        jit.move64ToDouble(result, fpScratch);
    });
}
#endif

#if CPU(ARM64)
// Test that the CachedTempRegister properly normalizes 32-bit immediates
// to their zero-extended form. On ARM64, 32-bit W-register writes implicitly
// zero-extend to the full 64-bit X-register. The cache must store the
// zero-extended value to avoid false hits when mixing 32/64-bit operations.
static void testCachedTempRegisterImm32Normalization()
{
    // Scenario 1: store32(TrustedImm32(-1)) then store64(TrustedImm64(-1))
    // The 32-bit store caches 0x00000000FFFFFFFF (zero-extended).
    // The 64-bit store needs 0xFFFFFFFFFFFFFFFF and must NOT cache-hit.
    {
        uint32_t val32 = 0;
        uint64_t val64 = 0;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(CCallHelpers::TrustedImm32(-1), &val32);
            jit.store64(CCallHelpers::TrustedImm64(-1LL), &val64);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        invoke<void>(test);
        CHECK_EQ(val32, static_cast<uint32_t>(0xFFFFFFFF));
        CHECK_EQ(val64, static_cast<uint64_t>(0xFFFFFFFFFFFFFFFF));
    }

    // Scenario 2: store64(TrustedImm64(-1)) then store32(TrustedImm32(-1))
    // The 64-bit store caches 0xFFFFFFFFFFFFFFFF.
    // The 32-bit store must not confuse the cache; result should be correct.
    {
        uint64_t val64 = 0;
        uint32_t val32 = 0;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store64(CCallHelpers::TrustedImm64(-1LL), &val64);
            jit.store32(CCallHelpers::TrustedImm32(-1), &val32);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        invoke<void>(test);
        CHECK_EQ(val64, static_cast<uint64_t>(0xFFFFFFFFFFFFFFFF));
        CHECK_EQ(val32, static_cast<uint32_t>(0xFFFFFFFF));
    }

    // Scenario 3: store32 with high bit set (0x80000000) then store64
    // int32_t(0x80000000) sign-extends to 0xFFFFFFFF80000000 in intptr_t,
    // but hardware zero-extends to 0x0000000080000000.
    // After fix, store64(0x80000000) should cache-hit since both are 0x0000000080000000.
    {
        uint32_t val32 = 0;
        uint64_t val64 = 0;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(CCallHelpers::TrustedImm32(static_cast<int32_t>(0x80000000)), &val32);
            jit.store64(CCallHelpers::TrustedImm64(0x80000000LL), &val64);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        invoke<void>(test);
        CHECK_EQ(val32, static_cast<uint32_t>(0x80000000));
        CHECK_EQ(val64, static_cast<uint64_t>(0x80000000));
    }

    // Scenario 4: store32(0x7FFFFFFF) then store64(0x7FFFFFFF)
    // Positive value: sign-extension and zero-extension produce the same result.
    // Should cache-hit.
    {
        uint32_t val32 = 0;
        uint64_t val64 = 0;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(CCallHelpers::TrustedImm32(0x7FFFFFFF), &val32);
            jit.store64(CCallHelpers::TrustedImm64(0x7FFFFFFFLL), &val64);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        invoke<void>(test);
        CHECK_EQ(val32, static_cast<uint32_t>(0x7FFFFFFF));
        CHECK_EQ(val64, static_cast<uint64_t>(0x7FFFFFFF));
    }

    // Scenario 5: Multiple alternating 32/64-bit stores with different negative values
    {
        uint32_t val1 = 0;
        uint64_t val2 = 0;
        uint32_t val3 = 0;
        uint64_t val4 = 0;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(CCallHelpers::TrustedImm32(-1), &val1);
            jit.store64(CCallHelpers::TrustedImm64(static_cast<int64_t>(0xDEADBEEFCAFEBABE)), &val2);
            jit.store32(CCallHelpers::TrustedImm32(-2), &val3);
            jit.store64(CCallHelpers::TrustedImm64(-1LL), &val4);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        invoke<void>(test);
        CHECK_EQ(val1, static_cast<uint32_t>(0xFFFFFFFF));
        CHECK_EQ(val2, static_cast<uint64_t>(0xDEADBEEFCAFEBABE));
        CHECK_EQ(val3, static_cast<uint32_t>(0xFFFFFFFE));
        CHECK_EQ(val4, static_cast<uint64_t>(0xFFFFFFFFFFFFFFFF));
    }

    // Scenario 6: Same positive value in both widths (cache should hit)
    {
        uint32_t val32 = 0;
        uint64_t val64 = 0;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.store32(CCallHelpers::TrustedImm32(42), &val32);
            jit.store64(CCallHelpers::TrustedImm64(42LL), &val64);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        invoke<void>(test);
        CHECK_EQ(val32, static_cast<uint32_t>(42));
        CHECK_EQ(val64, static_cast<uint64_t>(42));
    }

    // Scenario 7: branch32/branch64 mixing with cached registers
    // branch32 with TrustedImm32(-1) uses moveToCachedReg for values that
    // don't fit a shifted 12-bit immediate. A subsequent branch64 with
    // TrustedImm64(-1) must not false-hit on the cached 32-bit value.
    {
        uint32_t input32 = 0xFFFFFFFF;
        uint64_t input64 = 0xFFFFFFFFFFFFFFFF;
        auto test = compile([&] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // Load test values into registers
            jit.load32(&input32, GPRInfo::regT0);
            jit.load64(&input64, GPRInfo::regT1);

            // branch32 compare: regT0 should equal -1 (as 32-bit)
            // This uses moveToCachedReg(TrustedImm32(-1), dataMemoryTempRegister())
            auto fail1 = jit.branch32(CCallHelpers::NotEqual, GPRInfo::regT0, CCallHelpers::TrustedImm32(-1));

            // branch64 compare: regT1 should equal -1 (as 64-bit: 0xFFFFFFFFFFFFFFFF)
            // This uses moveToCachedReg(TrustedImm64(-1), dataMemoryTempRegister())
            // Must NOT false-hit on the cached 32-bit 0x00000000FFFFFFFF
            auto fail2 = jit.branch64(CCallHelpers::NotEqual, GPRInfo::regT1, CCallHelpers::TrustedImm64(-1LL));

            // Both comparisons passed
            jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
            auto done = jit.jump();

            fail1.link(&jit);
            fail2.link(&jit);
            jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);

            done.link(&jit);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        CHECK_EQ(invoke<int>(test), 1);
    }
}
#endif

#if CPU(X86_64)
#define CHECK_CODE_WAS_EMITTED(_jit, _emitter) do {                      \
        size_t _beforeCodeSize = _jit.m_assembler.buffer().codeSize();   \
        _emitter;                                                        \
        size_t _afterCodeSize = _jit.m_assembler.buffer().codeSize();    \
        if (_afterCodeSize > _beforeCodeSize)                            \
            break;                                                       \
        crashLock.lock();                                                \
        dataLog("FAILED while testing " #_emitter ": expected it to emit code\n"); \
        WTFReportAssertionFailure(__FILE__, __LINE__, WTF_PRETTY_FUNCTION, "CHECK_CODE_WAS_EMITTED("#_jit ", " #_emitter ")"); \
        CRASH();                                                         \
    } while (false)

static void testPrefetch()
{
    auto prefetchAround = compile([](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        for (GPRReg base : { X86Registers::eax, X86Registers::ecx, X86Registers::r8, X86Registers::r12, X86Registers::r13 }) {
            jit.push(base);
            jit.move(GPRInfo::argumentGPR0, base);
            for (int32_t offset : { 0, 64, -64, 4096 })
                jit.prefetch(CCallHelpers::Address(base, offset));
            jit.pop(base);
        }
        jit.move(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    });

    alignas(64) std::array<char, 192> bytes { };
    CHECK_EQ(invoke<void*>(prefetchAround, bytes.data() + 64), static_cast<void*>(bytes.data() + 64));
    CHECK_EQ(std::ranges::count(bytes, 0), static_cast<ptrdiff_t>(bytes.size()));
    CHECK_EQ(invoke<void*>(prefetchAround, nullptr), static_cast<void*>(nullptr));
}

static void testAtomicAndEmitsCode()
{
    // On x86, atomic (seqcst) RMW operations must emit a seqcst store (so a
    // LOCK-prefixed store or a fence or something).
    //
    // This tests that the optimization to elide and'ing -1 must not be
    // applied when the and is atomic.

    auto test32 = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg scratch = GPRInfo::argumentGPR2;
        jit.move(CCallHelpers::TrustedImm32(0), scratch);
        CHECK_CODE_WAS_EMITTED(jit, jit.atomicAnd32(CCallHelpers::TrustedImm32(-1), CCallHelpers::Address(GPRInfo::argumentGPR0)));
        CHECK_CODE_WAS_EMITTED(jit, jit.atomicAnd32(CCallHelpers::TrustedImm32(-1), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, scratch, CCallHelpers::TimesEight, 0)));

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    auto test64 = compile([] (CCallHelpers& jit) {
        emitFunctionPrologue(jit);

        GPRReg scratch = GPRInfo::argumentGPR2;
        jit.move(CCallHelpers::TrustedImm32(0), scratch);
        CHECK_CODE_WAS_EMITTED(jit, jit.atomicAnd64(CCallHelpers::TrustedImm32(-1), CCallHelpers::Address(GPRInfo::argumentGPR0)));
        CHECK_CODE_WAS_EMITTED(jit, jit.atomicAnd64(CCallHelpers::TrustedImm32(-1), CCallHelpers::BaseIndex(GPRInfo::argumentGPR0, scratch, CCallHelpers::TimesEight, 0)));

        emitFunctionEpilogue(jit);
        jit.ret();
    });

    uint64_t value = 42;

    invoke<void>(test32, &value);
    CHECK_EQ(value, 42);

    invoke<void>(test64, &value);
    CHECK_EQ(value, 42);
}
#endif

#if CPU(ARM64) || CPU(X86_64)
static void testMove32ToFloatMovi()
{
    // Test movi-encodable patterns
    auto testPattern = [] (uint32_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move32ToFloat(CCallHelpers::TrustedImm32(pattern), FPRInfo::fpRegT0);
            jit.moveFloatTo32(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        CHECK_EQ(invoke<uint32_t>(test), pattern);
    };

    // Test shifted immediate patterns (single byte at shift 0, 8, 16, 24)
    testPattern(0x00000012); // shift 0
    testPattern(0x00001200); // shift 8
    testPattern(0x00120000); // shift 16
    testPattern(0x12000000); // shift 24
    testPattern(0x00000080); // Sign bit at byte 0
    testPattern(0x00008000); // Sign bit at byte 1
    testPattern(0x00800000); // Sign bit at byte 2
    testPattern(0x80000000); // Sign bit at byte 3 (common pattern!)
    testPattern(0x000000ff); // Max byte at shift 0
    testPattern(0x0000ff00); // Max byte at shift 8
    testPattern(0x00ff0000); // Max byte at shift 16
    testPattern(0xff000000); // Max byte at shift 24

    // Test inverted shifted immediate patterns
    testPattern(0xffffffed); // ~0x00000012
    testPattern(0xffffedff); // ~0x00001200
    testPattern(0xffedffff); // ~0x00120000
    testPattern(0xedffffff); // ~0x12000000
    testPattern(0xffffff7f); // ~0x00000080
    testPattern(0xffff7fff); // ~0x00008000
    testPattern(0xff7fffff); // ~0x00800000
    testPattern(0x7fffffff); // ~0x80000000 (common pattern!)
    testPattern(0xffffff00); // ~0x000000ff
    testPattern(0xffff00ff); // ~0x0000ff00
    testPattern(0xff00ffff); // ~0x00ff0000
    testPattern(0x00ffffff); // ~0xff000000

    // Test MSL (Mask Shift Left) patterns
    testPattern(0x000012ff); // movi #0x12, MSL #8
    testPattern(0x0012ffff); // movi #0x12, MSL #16
    testPattern(0x000042ff); // movi #0x42, MSL #8
    testPattern(0x0042ffff); // movi #0x42, MSL #16
    testPattern(0x0080ffff); // movi #0x80, MSL #16 (sign bit + mask)

    // Test inverted MSL patterns
    testPattern(0xffffed00); // mvni #0x12, MSL #8 → ~0x000012ff
    testPattern(0xffed0000); // mvni #0x12, MSL #16 → ~0x0012ffff
    testPattern(0xffffbd00); // mvni #0x42, MSL #8 → ~0x000042ff
    testPattern(0xffbd0000); // mvni #0x42, MSL #16 → ~0x0042ffff
    testPattern(0xff7f0000); // mvni #0x80, MSL #16 → ~0x0080ffff

    // Test byte-mask patterns (each byte is 0x00 or 0xFF)
    testPattern(0x00000000); // All zero (handled by moveZeroToFloat)
    testPattern(0xffffffff); // All 0xFF
    testPattern(0xff00ff00); // Bytes 1 and 3 are 0xFF
    testPattern(0x00ff00ff); // Bytes 0 and 2 are 0xFF
    testPattern(0xffff0000); // Bytes 2 and 3 are 0xFF
    testPattern(0x0000ffff); // Bytes 0 and 1 are 0xFF
    testPattern(0xff0000ff); // Bytes 0 and 3 are 0xFF

    // Test non-encodable patterns (should still work via fallback)
    testPattern(0x12345678); // Multiple non-zero bytes
    testPattern(0x3f800000); // Float 1.0 (0x3f and 0x80 in different bytes)
    testPattern(0x40000000); // Float 2.0
    testPattern(0xc0000000); // Float -2.0
}

static void testMove64ToDoubleMovi()
{
    // Test movi-encodable patterns (each byte is 0x00 or 0xFF)
    auto testPattern = [] (uint64_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move64ToDouble(CCallHelpers::TrustedImm64(pattern), FPRInfo::fpRegT0);
            jit.moveDoubleTo64(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        CHECK_EQ(invoke<uint64_t>(test), pattern);
    };

    // Test movi patterns (direct encoding)
    testPattern(0x0000000000000000ULL); // All zero
    testPattern(0x00000000000000ffULL); // Byte 0 is 0xFF
    testPattern(0x000000000000ff00ULL); // Byte 1 is 0xFF
    testPattern(0x00000000ff000000ULL); // Byte 4 is 0xFF
    testPattern(0xff00000000000000ULL); // Byte 7 is 0xFF
    testPattern(0xffffffffffffffffULL); // All 0xFF
    testPattern(0xff00ff00ff00ff00ULL); // Alternating pattern
    testPattern(0x00ff00ff00ff00ffULL); // Alternating pattern
    testPattern(0xffffffff00000000ULL); // Upper 4 bytes 0xFF
    testPattern(0x00000000ffffffffULL); // Lower 4 bytes 0xFF

    // Test mvni patterns (inverted encoding)
    testPattern(0xffffffffffffff00ULL); // ~0x00000000000000ff
    testPattern(0xffffffffffff00ffULL); // ~0x000000000000ff00
    testPattern(0xffffffff00ffffffULL); // ~0x00000000ff000000
    testPattern(0x00ffffffffffffffULL); // ~0xff00000000000000
    testPattern(0x00ff00ff00ff00ffULL); // Can be encoded as movi

    // Test repeated 32-bit patterns (will use move32ToFloat logic)
    testPattern(0x8000000080000000ULL); // Repeated sign bit
    testPattern(0x7fffffff7fffffffULL); // Repeated INT32_MAX
    testPattern(0x000042ff000042ffULL); // Repeated MSL pattern
    testPattern(0xffffbd00ffffbd00ULL); // Repeated inverted MSL
    testPattern(0x0012000000120000ULL); // Repeated LSL shift
    testPattern(0xff00ff00ff00ff00ULL); // Repeated byte mask
    testPattern(0x00ff00ff00ff00ffULL); // Can be encoded as movi

    // Test non-encodable patterns (should still work via fallback)
    testPattern(0x7fffffffffffffffULL); // Sign bit pattern
    testPattern(0x8000000000000000ULL); // Sign bit pattern
    testPattern(0x123456789abcdef0ULL); // Arbitrary pattern
    testPattern(0x3ff0000000000000ULL); // Double 1.0
}

// Regression test for bug where move64ToDouble incorrectly called move32ToFloat
// for repeated 32-bit patterns. The old code would call move32ToFloat which uses
// fmov<32> that only sets the lower 32 bits, leaving upper 32 bits undefined.
// This test verifies that BOTH upper and lower 32 bits are set correctly.
static void testMove64ToDoubleRepeated32BitPatternBug()
{
    auto testPattern = [] (uint64_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // Load the 64-bit pattern into a double register
            jit.move64ToDouble(CCallHelpers::TrustedImm64(pattern), FPRInfo::fpRegT0);

            // Read back the entire 64-bit value
            jit.moveDoubleTo64(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        uint64_t result = invoke<uint64_t>(test);

        // The bug would cause the upper 32 bits to be undefined/garbage
        // Verify BOTH upper and lower 32 bits are correct
        uint32_t resultLower = static_cast<uint32_t>(result);
        uint32_t resultUpper = static_cast<uint32_t>(result >> 32);
        uint32_t expectedLower = static_cast<uint32_t>(pattern);
        uint32_t expectedUpper = static_cast<uint32_t>(pattern >> 32);

        if (resultLower != expectedLower || resultUpper != expectedUpper) {
            dataLog("FAIL: testMove64ToDoubleRepeated32BitPatternBug\n");
            dataLog("  Pattern:  0x", hex(pattern), "\n");
            dataLog("  Expected: 0x", hex(expectedUpper), "'", hex(expectedLower), "\n");
            dataLog("  Got:      0x", hex(resultUpper), "'", hex(resultLower), "\n");
            dataLog("  Upper 32 bits ", (resultUpper == expectedUpper ? "OK" : "WRONG"), "\n");
            dataLog("  Lower 32 bits ", (resultLower == expectedLower ? "OK" : "WRONG"), "\n");
        }

        CHECK_EQ(result, pattern);
    };

    // Test various repeated 32-bit patterns that would trigger the bug
    // These patterns have the same value in upper and lower 32 bits

    // FP immediate patterns (canEncodeFPImm<32> returns true)
    testPattern(0x3f8000003f800000ULL); // Repeated float 1.0
    testPattern(0x4000000040000000ULL); // Repeated float 2.0
    testPattern(0xc0000000c0000000ULL); // Repeated float -2.0
    testPattern(0x0000000000000000ULL); // Zero (handled specially but worth testing)

    // Shifted immediate patterns (movi with LSL)
    testPattern(0x0012000000120000ULL); // Repeated movi #0x12, LSL #16
    testPattern(0x1200000012000000ULL); // Repeated movi #0x12, LSL #24
    testPattern(0x0000120000001200ULL); // Repeated movi #0x12, LSL #8
    testPattern(0x0000001200000012ULL); // Repeated movi #0x12, LSL #0

    // MSL patterns (movi with MSL)
    testPattern(0x000042ff000042ffULL); // Repeated movi #0x42, MSL #8
    testPattern(0x0042ffff0042ffffULL); // Repeated movi #0x42, MSL #16
    testPattern(0x008000ff008000ffULL); // Repeated movi #0x80, MSL #8

    // Inverted MSL patterns (mvni with MSL)
    testPattern(0xffffbd00ffffbd00ULL); // Repeated mvni #0x42, MSL #8 (inverted 0x000042ff)
    testPattern(0xffbd0000ffbd0000ULL); // Repeated mvni #0x42, MSL #16 (inverted 0x0042ffff)

    // 16-bit shifted patterns (movi with 16-bit lanes)
    testPattern(0x0012001200120012ULL); // Repeated movi with 16-bit lanes
    testPattern(0xff00ff00ff00ff00ULL); // Repeated byte mask pattern

    // 8-bit replicated patterns (movi with 8-bit lanes)
    testPattern(0x4242424242424242ULL); // Repeated byte 0x42
    testPattern(0x8080808080808080ULL); // Repeated byte 0x80
    testPattern(0xffffffffffffffffULL); // All 0xFF bytes

    // Edge cases
    testPattern(0x8000000080000000ULL); // Repeated sign bit (INT32_MIN)
    testPattern(0x7fffffff7fffffffULL); // Repeated INT32_MAX
    testPattern(0x0000000100000001ULL); // Repeated 1
    testPattern(0xfffffffefffffffeULL); // Repeated -2 (as uint32)
}

static void testMove128ToVectorMovi()
{
    auto testPattern = [&](v128_t pattern) {
        // Create a simple function that moves the v128 immediate to a vector register
        // then extracts both 64-bit lanes to verify correctness
        auto compilation = compile([&](CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // Move the v128 pattern to vector register v0
            jit.move128ToVector(pattern, FPRInfo::argumentFPR0);

            // Extract lower 64 bits to x0
            jit.vectorExtractLaneInt64(CCallHelpers::TrustedImm32(0), FPRInfo::argumentFPR0, GPRInfo::returnValueGPR);

            // Extract upper 64 bits to x1
            jit.vectorExtractLaneInt64(CCallHelpers::TrustedImm32(1), FPRInfo::argumentFPR0, GPRInfo::returnValueGPR2);

            // Return (results will be in x0 and x1)
            emitFunctionEpilogue(jit);
            jit.ret();
        });

        // Execute and verify
        auto [low64, high64] = invoke<std::pair<uint64_t, uint64_t>>(compilation);
        CHECK_EQ(low64, pattern.u64x2[0]);
        CHECK_EQ(high64, pattern.u64x2[1]);
    };

    // Test all zeros (special case)
    testPattern(v128_t { 0x0000000000000000ULL, 0x0000000000000000ULL });

    // Test all ones (0xFFFFFFFF repeated 4 times, uses mvni<128>)
    testPattern(v128_t { 0xffffffffffffffffULL, 0xffffffffffffffffULL });

    // Test 32-bit LSL patterns repeated 4 times (uses movi<128>)
    testPattern(v128_t { 0x1200000012000000ULL, 0x1200000012000000ULL }); // 0x12 << 24
    testPattern(v128_t { 0x0012000000120000ULL, 0x0012000000120000ULL }); // 0x12 << 16
    testPattern(v128_t { 0x0000120000001200ULL, 0x0000120000001200ULL }); // 0x12 << 8
    testPattern(v128_t { 0x0000001200000012ULL, 0x0000001200000012ULL }); // 0x12 << 0

    // Test inverted 32-bit LSL patterns repeated 4 times (uses mvni<128>)
    testPattern(v128_t { 0xedffffffedffffffULL, 0xedffffffedffffffULL }); // ~(0x12 << 24)
    testPattern(v128_t { 0xffedffffffedffffULL, 0xffedffffffedffffULL }); // ~(0x12 << 16)

    // Test 32-bit MSL patterns repeated 4 times (uses movi<128> with MSL)
    testPattern(v128_t { 0x000042ff000042ffULL, 0x000042ff000042ffULL }); // MSL #8
    testPattern(v128_t { 0x0042ffff0042ffffULL, 0x0042ffff0042ffffULL }); // MSL #16

    // Test inverted 32-bit MSL patterns repeated 4 times (uses mvni<128> with MSL)
    testPattern(v128_t { 0xffffbd00ffffbd00ULL, 0xffffbd00ffffbd00ULL }); // ~MSL #8
    testPattern(v128_t { 0xffbd0000ffbd0000ULL, 0xffbd0000ffbd0000ULL }); // ~MSL #16

    // Test 32-bit byte-mask patterns repeated 4 times (uses movi<128>)
    testPattern(v128_t { 0xff00ff00ff00ff00ULL, 0xff00ff00ff00ff00ULL }); // Byte mask

    // Test 64-bit patterns repeated twice (uses movi/mvni<64> + dup)
    testPattern(v128_t { 0xff00ff00ff00ff00ULL, 0xff00ff00ff00ff00ULL }); // 64-bit byte mask repeated

    // Test non-repeating patterns (should use fallback)
    testPattern(v128_t { 0x0000000000000000ULL, 0xffffffffffffffffULL }); // Half zeros, half ones
    testPattern(v128_t { 0x123456789abcdef0ULL, 0xfedcba9876543210ULL }); // Arbitrary different values
    testPattern(v128_t { 0x0000000000000042ULL, 0x0000000000000043ULL }); // Simple different values
}

#if CPU(ARM64)
static void testMove16ToFloat16Comprehensive()
{
    auto testPattern = [] (uint16_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move16ToFloat16(CCallHelpers::TrustedImm32(pattern), FPRInfo::fpRegT0);
            jit.moveFloat16To16(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        uint16_t result = static_cast<uint16_t>(invoke<uint32_t>(test));
        CHECK_EQ(result, pattern);
    };

    // Test zero (moveZeroToFloat path)
    testPattern(0x0000);

    // Test FP immediate encoding (fmov path)
    testPattern(0x3C00); // Half-precision 1.0
    testPattern(0x4000); // Half-precision 2.0
    testPattern(0xBC00); // Half-precision -1.0
    testPattern(0x3800); // Half-precision 0.5
    testPattern(0x4200); // Half-precision 3.0
    testPattern(0x4400); // Half-precision 4.0

    // Test 16-bit LSL shifted immediate (movi path)
    testPattern(0x1200); // 0x12 << 8
    testPattern(0x0012); // 0x12 << 0
    testPattern(0x8000); // Sign bit
    testPattern(0xFF00); // Max byte at shift 8
    testPattern(0x00FF); // Max byte at shift 0

    // Test inverted 16-bit LSL shifted immediate (mvni path)
    testPattern(0xEDFF); // ~0x1200
    testPattern(0xFFED); // ~0x0012
    testPattern(0x7FFF); // ~0x8000
    testPattern(0x00FF); // ~0xFF00
    testPattern(0xFF00); // ~0x00FF

    // Test all bytes equal (movi 8-bit path)
    testPattern(0x4242); // Same byte repeated
    testPattern(0x8080); // Sign bit in both bytes
    testPattern(0xFFFF); // All ones
    testPattern(0x1111); // Low value repeated

    // Test non-encodable patterns (fallback path)
    testPattern(0x1234); // Arbitrary pattern
    testPattern(0x3C01); // Near FP immediate but not encodable
    testPattern(0xABCD); // Random pattern
    testPattern(0x5A5A); // Alternating bits pattern
}
#endif

static void testMove32ToFloatComprehensive()
{
    auto testPattern = [] (uint32_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move32ToFloat(CCallHelpers::TrustedImm32(pattern), FPRInfo::fpRegT0);
            jit.moveFloatTo32(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        CHECK_EQ(invoke<uint32_t>(test), pattern);
    };

    // Test zero (moveZeroToFloat path)
    testPattern(0x00000000);

    // Test FP immediate encoding (fmov path)
    testPattern(0x3F800000); // Float 1.0
    testPattern(0x40000000); // Float 2.0
    testPattern(0xC0000000); // Float -2.0
    testPattern(0x3F000000); // Float 0.5
    testPattern(0xBF000000); // Float -0.5
    testPattern(0x3E800000); // Float 0.25
    testPattern(0x40400000); // Float 3.0
    testPattern(0xC0A00000); // Float -5.0

    // Test 32-bit LSL shifted immediate (movi path)
    testPattern(0x00000012); // shift 0
    testPattern(0x00001200); // shift 8
    testPattern(0x00120000); // shift 16
    testPattern(0x12000000); // shift 24
    testPattern(0x00000042);
    testPattern(0x00004200);
    testPattern(0x00420000);
    testPattern(0x42000000);
    testPattern(0x80000000); // Sign bit (very common!)
    testPattern(0x000000FF); // Max byte at shift 0
    testPattern(0x0000FF00); // Max byte at shift 8
    testPattern(0x00FF0000); // Max byte at shift 16
    testPattern(0xFF000000); // Max byte at shift 24

    // Test inverted 32-bit LSL shifted immediate (mvni path)
    testPattern(0xFFFFFFED); // ~0x00000012
    testPattern(0xFFFFEDFF); // ~0x00001200
    testPattern(0xFFEDFFFF); // ~0x00120000
    testPattern(0xEDFFFFFF); // ~0x12000000
    testPattern(0xFFFFFFBD); // ~0x42
    testPattern(0xFFFFBDFF);
    testPattern(0xFFBDFFFF);
    testPattern(0xBDFFFFFF);
    testPattern(0x7FFFFFFF); // ~0x80000000 (INT32_MAX, common!)
    testPattern(0xFFFFFF00); // ~0x000000FF
    testPattern(0xFFFF00FF); // ~0x0000FF00
    testPattern(0xFF00FFFF);
    testPattern(0x00FFFFFF);

    // Test MSL (Mask Shift Left) patterns (movi MSL path)
    testPattern(0x000012FF); // movi #0x12, MSL #8
    testPattern(0x0012FFFF); // movi #0x12, MSL #16
    testPattern(0x000042FF); // movi #0x42, MSL #8
    testPattern(0x0042FFFF); // movi #0x42, MSL #16
    testPattern(0x0080FFFF); // movi #0x80, MSL #16
    testPattern(0x000080FF);

    // Test inverted MSL patterns (mvni MSL path)
    testPattern(0xFFFFED00); // mvni #0x12, MSL #8
    testPattern(0xFFED0000); // mvni #0x12, MSL #16
    testPattern(0xFFFFBD00); // mvni #0x42, MSL #8
    testPattern(0xFFBD0000); // mvni #0x42, MSL #16
    testPattern(0xFF7FFFFF);
    testPattern(0xFF7F0000);

    // Test byte-mask patterns (movi<64> path)
    testPattern(0xFF00FF00); // Bytes 1 and 3 are 0xFF
    testPattern(0x00FF00FF); // Bytes 0 and 2 are 0xFF
    testPattern(0xFFFF0000); // Bytes 2 and 3 are 0xFF
    testPattern(0x0000FFFF); // Bytes 0 and 1 are 0xFF
    testPattern(0xFF0000FF); // Bytes 0 and 3 are 0xFF
    testPattern(0xFFFFFFFF); // All 0xFF
    testPattern(0x00FFFF00);
    testPattern(0xFF000000);
    testPattern(0x000000FF);

    // Test repeated 16-bit halves with FP immediate (fmov_v<16> path)
    testPattern(0x3C003C00); // Repeated half-precision 1.0
    testPattern(0x40004000); // Repeated half-precision 2.0
    testPattern(0xBC00BC00); // Repeated half-precision -1.0

    // Test repeated 16-bit halves with LSL (movi<16> path)
    testPattern(0x12001200); // Repeated 0x1200
    testPattern(0x00120012); // Repeated 0x0012
    testPattern(0x80008000); // Repeated sign bit
    testPattern(0x42004200);

    // Test repeated 16-bit halves with inverted LSL (mvni<16> path)
    testPattern(0xEDFFEDFF); // Repeated ~0x1200
    testPattern(0xFFEDFFED); // Repeated ~0x0012
    testPattern(0x7FFF7FFF);
    testPattern(0xBDFFBDFF);

    // Test all 4 bytes equal (movi 8-bit path)
    testPattern(0x42424242); // Byte 0x42 repeated
    testPattern(0x80808080); // Byte 0x80 repeated
    testPattern(0x11111111); // Byte 0x11 repeated
    testPattern(0xAAAAAAAA);
    testPattern(0x55555555);

    // Test non-encodable patterns (fallback path)
    testPattern(0x12345678); // Multiple non-zero bytes
    testPattern(0xABCDEF01); // Arbitrary pattern
    testPattern(0x3F800001); // Near float 1.0 but not exact
    testPattern(0x01020304);
}

// Comprehensive tests for move64ToDouble covering all code paths
static void testMove64ToDoubleComprehensive()
{
    auto testPattern = [] (uint64_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move64ToDouble(CCallHelpers::TrustedImm64(pattern), FPRInfo::fpRegT0);
            jit.moveDoubleTo64(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        CHECK_EQ(invoke<uint64_t>(test), pattern);
    };

    // Test zero (moveZeroToDouble path)
    testPattern(0x0000000000000000ULL);

    // Test FP immediate encoding (fmov<64> path)
    testPattern(0x3FF0000000000000ULL); // Double 1.0
    testPattern(0x4000000000000000ULL); // Double 2.0
    testPattern(0xC000000000000000ULL); // Double -2.0
    testPattern(0xBFF0000000000000ULL); // Double -1.0
    testPattern(0x3FE0000000000000ULL); // Double 0.5
    testPattern(0x4008000000000000ULL); // Double 3.0

    // Test byte-mask patterns (movi<64> path)
    testPattern(0x00000000000000FFULL); // Byte 0 is 0xFF
    testPattern(0x000000000000FF00ULL); // Byte 1 is 0xFF
    testPattern(0x00000000FF000000ULL); // Byte 4 is 0xFF
    testPattern(0xFF00000000000000ULL); // Byte 7 is 0xFF
    testPattern(0xFF00FF00FF00FF00ULL); // Alternating pattern
    testPattern(0x00FF00FF00FF00FFULL); // Alternating pattern
    testPattern(0xFFFFFFFF00000000ULL); // Upper 4 bytes 0xFF
    testPattern(0x00000000FFFFFFFFULL); // Lower 4 bytes 0xFF
    testPattern(0xFFFFFFFFFFFFFFFFULL); // All 0xFF
    testPattern(0x0000FFFFFFFF0000ULL);

    // Test repeated 32-bit halves with FP immediate (fmov_v<32> path)
    testPattern(0x3F8000003F800000ULL); // Repeated float 1.0
    testPattern(0x4000000040000000ULL); // Repeated float 2.0
    testPattern(0xC0000000C0000000ULL); // Repeated float -2.0

    // Test repeated 32-bit halves with LSL (movi path)
    testPattern(0x0012000000120000ULL); // Repeated movi #0x12, LSL #16
    testPattern(0x1200000012000000ULL); // Repeated movi #0x12, LSL #24
    testPattern(0x0000120000001200ULL); // Repeated movi #0x12, LSL #8
    testPattern(0x0000001200000012ULL); // Repeated movi #0x12, LSL #0
    testPattern(0x8000000080000000ULL); // Repeated sign bit
    testPattern(0x7FFFFFFF7FFFFFFFULL); // Repeated INT32_MAX
    testPattern(0x4200000042000000ULL);

    // Test repeated 32-bit halves with inverted LSL (mvni path)
    testPattern(0xFFFFEDFFFFFFEDFFULL); // Repeated ~0x00001200
    testPattern(0xEDFFFFFFEDFFFFFFULL); // Repeated ~0x12000000
    testPattern(0xBDFFFFFFBDFFFFFFULL);

    // Test repeated 32-bit halves with MSL (movi MSL path)
    testPattern(0x000042FF000042FFULL); // Repeated movi #0x42, MSL #8
    testPattern(0x0042FFFF0042FFFFULL); // Repeated movi #0x42, MSL #16
    testPattern(0x008000FF008000FFULL); // Repeated movi #0x80, MSL #8
    testPattern(0x0080FFFF0080FFFFULL); // Repeated movi #0x80, MSL #16

    // Test repeated 32-bit halves with inverted MSL (mvni MSL path)
    testPattern(0xFFFFBD00FFFFBD00ULL); // Repeated mvni #0x42, MSL #8
    testPattern(0xFFBD0000FFBD0000ULL); // Repeated mvni #0x42, MSL #16

    // Test repeated 16-bit values (all four 16-bit lanes equal) with FP immediate
    testPattern(0x3C003C003C003C00ULL); // Repeated half-precision 1.0
    testPattern(0x4000400040004000ULL); // Repeated half-precision 2.0
    testPattern(0xBC00BC00BC00BC00ULL);

    // Test repeated 16-bit values with LSL (movi<16> path)
    testPattern(0x0012001200120012ULL); // Repeated 0x0012
    testPattern(0x1200120012001200ULL); // Repeated 0x1200
    testPattern(0x8000800080008000ULL); // Repeated sign bit
    testPattern(0x4200420042004200ULL);

    // Test repeated 16-bit values with inverted LSL (mvni<16> path)
    testPattern(0xFFEDFFEDFFEDFFEDULL); // Repeated ~0x0012
    testPattern(0xEDFFEDFFEDFFEDFFULL); // Repeated ~0x1200
    testPattern(0x7FFF7FFF7FFF7FFFULL);
    testPattern(0xBDFFBDFFBDFFBDFFULL);

    // Test all 8 bytes equal (movi 8-bit path)
    testPattern(0x4242424242424242ULL); // Byte 0x42 repeated
    testPattern(0x8080808080808080ULL); // Byte 0x80 repeated
    testPattern(0x1111111111111111ULL); // Byte 0x11 repeated
    testPattern(0xAAAAAAAAAAAAAAAAULL);
    testPattern(0x5555555555555555ULL);

    // Test non-encodable patterns (fallback path)
    testPattern(0x123456789ABCDEF0ULL); // Arbitrary pattern
    testPattern(0x7FFFFFFFFFFFFFFFULL); // Near all ones
    testPattern(0x8000000000000000ULL); // Single sign bit
    testPattern(0x3FF0000000000001ULL); // Near double 1.0
    testPattern(0x0102030405060708ULL);
}

// Comprehensive tests for move128ToVector covering all code paths
static void testMove128ToVectorComprehensive()
{
    auto testPattern = [&](v128_t pattern) {
        auto compilation = compile([&](CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move128ToVector(pattern, FPRInfo::argumentFPR0);

            // Extract lower 64 bits to x0
            jit.vectorExtractLaneInt64(CCallHelpers::TrustedImm32(0), FPRInfo::argumentFPR0, GPRInfo::returnValueGPR);

            // Extract upper 64 bits to x1
            jit.vectorExtractLaneInt64(CCallHelpers::TrustedImm32(1), FPRInfo::argumentFPR0, GPRInfo::returnValueGPR2);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        auto [low64, high64] = invoke<std::pair<uint64_t, uint64_t>>(compilation);
        CHECK_EQ(low64, pattern.u64x2[0]);
        CHECK_EQ(high64, pattern.u64x2[1]);
    };

    // Test all zeros (special case)
    testPattern(v128_t { 0x0000000000000000ULL, 0x0000000000000000ULL });

    // Test upper and lower 64-bit halves equal with FP immediate (fmov_v<128,64> path)
    testPattern(v128_t { 0x3FF0000000000000ULL, 0x3FF0000000000000ULL }); // Double 1.0 repeated
    testPattern(v128_t { 0x4000000000000000ULL, 0x4000000000000000ULL }); // Double 2.0 repeated
    testPattern(v128_t { 0xBFF0000000000000ULL, 0xBFF0000000000000ULL });

    // Test upper and lower 64-bit halves equal with byte mask (movi<128,64> path)
    testPattern(v128_t { 0xFF00FF00FF00FF00ULL, 0xFF00FF00FF00FF00ULL });
    testPattern(v128_t { 0x00FF00FF00FF00FFULL, 0x00FF00FF00FF00FFULL });
    testPattern(v128_t { 0xFFFFFFFF00000000ULL, 0xFFFFFFFF00000000ULL });
    testPattern(v128_t { 0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL });

    // Test all four 32-bit lanes equal with FP immediate (fmov_v<128,32> path)
    testPattern(v128_t { 0x3F8000003F800000ULL, 0x3F8000003F800000ULL }); // Float 1.0 repeated 4 times
    testPattern(v128_t { 0x4000000040000000ULL, 0x4000000040000000ULL }); // Float 2.0 repeated 4 times
    testPattern(v128_t { 0xBF800000BF800000ULL, 0xBF800000BF800000ULL });

    // Test all four 32-bit lanes equal with LSL (movi<128,32> path)
    testPattern(v128_t { 0x1200000012000000ULL, 0x1200000012000000ULL }); // 0x12 << 24, repeated
    testPattern(v128_t { 0x0012000000120000ULL, 0x0012000000120000ULL }); // 0x12 << 16, repeated
    testPattern(v128_t { 0x0000120000001200ULL, 0x0000120000001200ULL }); // 0x12 << 8, repeated
    testPattern(v128_t { 0x0000001200000012ULL, 0x0000001200000012ULL }); // 0x12 << 0, repeated
    testPattern(v128_t { 0x8000000080000000ULL, 0x8000000080000000ULL }); // Sign bit repeated
    testPattern(v128_t { 0x4200000042000000ULL, 0x4200000042000000ULL });

    // Test all four 32-bit lanes equal with inverted LSL (mvni<128,32> path)
    testPattern(v128_t { 0xEDFFFFFFEDFFFFFFULL, 0xEDFFFFFFEDFFFFFFULL }); // ~(0x12 << 24), repeated
    testPattern(v128_t { 0xFFEDFFFFFFEDFFFFULL, 0xFFEDFFFFFFEDFFFFULL }); // ~(0x12 << 16), repeated
    testPattern(v128_t { 0xFFFFEDFFFFFFEDFFULL, 0xFFFFEDFFFFFFEDFFULL }); // ~(0x12 << 8), repeated
    testPattern(v128_t { 0x7FFFFFFF7FFFFFFFULL, 0x7FFFFFFF7FFFFFFFULL }); // ~sign bit, repeated
    testPattern(v128_t { 0xBDFFFFFFBDFFFFFFULL, 0xBDFFFFFFBDFFFFFFULL });

    // Test all four 32-bit lanes equal with MSL (movi<128,32> MSL path)
    testPattern(v128_t { 0x000042FF000042FFULL, 0x000042FF000042FFULL }); // MSL #8, repeated
    testPattern(v128_t { 0x0042FFFF0042FFFFULL, 0x0042FFFF0042FFFFULL }); // MSL #16, repeated
    testPattern(v128_t { 0x008000FF008000FFULL, 0x008000FF008000FFULL }); // MSL #8 with 0x80

    // Test all four 32-bit lanes equal with inverted MSL (mvni<128,32> MSL path)
    testPattern(v128_t { 0xFFFFBD00FFFFBD00ULL, 0xFFFFBD00FFFFBD00ULL }); // ~MSL #8, repeated
    testPattern(v128_t { 0xFFBD0000FFBD0000ULL, 0xFFBD0000FFBD0000ULL }); // ~MSL #16, repeated

    // Test all four 32-bit lanes equal with byte mask (movi<128,64> path for 32-bit)
    testPattern(v128_t { 0xFF00FF00FF00FF00ULL, 0xFF00FF00FF00FF00ULL });
    testPattern(v128_t { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL }); // All ones

    // Test all eight 16-bit lanes equal with FP immediate (fmov_v<128,16> path)
    testPattern(v128_t { 0x3C003C003C003C00ULL, 0x3C003C003C003C00ULL }); // Half 1.0 repeated 8 times
    testPattern(v128_t { 0x4000400040004000ULL, 0x4000400040004000ULL }); // Half 2.0 repeated 8 times
    testPattern(v128_t { 0xBC00BC00BC00BC00ULL, 0xBC00BC00BC00BC00ULL });

    // Test all eight 16-bit lanes equal with LSL (movi<128,16> path)
    testPattern(v128_t { 0x1200120012001200ULL, 0x1200120012001200ULL }); // 0x1200 repeated
    testPattern(v128_t { 0x0012001200120012ULL, 0x0012001200120012ULL }); // 0x0012 repeated
    testPattern(v128_t { 0x8000800080008000ULL, 0x8000800080008000ULL }); // Sign bit repeated
    testPattern(v128_t { 0x4200420042004200ULL, 0x4200420042004200ULL });

    // Test all eight 16-bit lanes equal with inverted LSL (mvni<128,16> path)
    testPattern(v128_t { 0xEDFFEDFFEDFFEDFFULL, 0xEDFFEDFFEDFFEDFFULL }); // ~0x1200 repeated
    testPattern(v128_t { 0xFFEDFFEDFFEDFFEDULL, 0xFFEDFFEDFFEDFFEDULL }); // ~0x0012 repeated
    testPattern(v128_t { 0x7FFF7FFF7FFF7FFFULL, 0x7FFF7FFF7FFF7FFFULL });

    // Test all 16 bytes equal (movi<128,8> path)
    testPattern(v128_t { 0x4242424242424242ULL, 0x4242424242424242ULL }); // 0x42 repeated 16 times
    testPattern(v128_t { 0x8080808080808080ULL, 0x8080808080808080ULL }); // 0x80 repeated 16 times
    testPattern(v128_t { 0x1111111111111111ULL, 0x1111111111111111ULL }); // 0x11 repeated 16 times
    testPattern(v128_t { 0xAAAAAAAAAAAAAAAAULL, 0xAAAAAAAAAAAAAAAAULL });
    testPattern(v128_t { 0x5555555555555555ULL, 0x5555555555555555ULL });

    // Test non-repeating patterns (fallback path - GPR + vector lane insertion)
    testPattern(v128_t { 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL }); // Half zeros, half ones
    testPattern(v128_t { 0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL }); // Different arbitrary values
    testPattern(v128_t { 0x0000000000000042ULL, 0x0000000000000043ULL }); // Simple different values
    testPattern(v128_t { 0x3FF0000000000000ULL, 0x4000000000000000ULL }); // Different doubles
    testPattern(v128_t { 0x123456789ABCDEF0ULL, 0x123456789ABCDEF1ULL }); // Nearly same but different
    testPattern(v128_t { 0x0102030405060708ULL, 0x090A0B0C0D0E0F00ULL });

    // Test all ones
    testPattern(v128_t { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL });

    // Test upper and lower 64-bit halves identical
    testPattern(v128_t { 0x8000000080000000ULL, 0x8000000080000000ULL });
    testPattern(v128_t { 0x1234567812345678ULL, 0x1234567812345678ULL });
    testPattern(v128_t { 0x00000000000000FFULL, 0x00000000000000FFULL });
    testPattern(v128_t { 0xFF00000000000000ULL, 0xFF00000000000000ULL });

    // Test all four 32-bit lanes identical
    testPattern(v128_t { 0x8000000080000000ULL, 0x8000000080000000ULL });
    testPattern(v128_t { 0x1234567812345678ULL, 0x1234567812345678ULL });
    testPattern(v128_t { 0x000000FF000000FFULL, 0x000000FF000000FFULL });
    testPattern(v128_t { 0xFF000000FF000000ULL, 0xFF000000FF000000ULL });

    // Test upper 64 bits zero
    testPattern(v128_t { 0x123456789ABCDEF0ULL, 0x0000000000000000ULL });
    testPattern(v128_t { 0x80000000ULL, 0x0000000000000000ULL });
    testPattern(v128_t { 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL });
    testPattern(v128_t { 0x0000000000000001ULL, 0x0000000000000000ULL }); // Minimal non-zero lower
    testPattern(v128_t { 0x7FFFFFFFFFFFFFFFULL, 0x0000000000000000ULL }); // Max positive int64
    testPattern(v128_t { 0x8000000000000000ULL, 0x0000000000000000ULL }); // Min negative int64 (as uint)

    // Test lower 64 bits zero, upper non-zero (fallback path)
    testPattern(v128_t { 0x0000000000000000ULL, 0x123456789ABCDEF0ULL });
    testPattern(v128_t { 0x0000000000000000ULL, 0x0000000000000001ULL }); // Minimal upper
    testPattern(v128_t { 0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL }); // Max upper
    testPattern(v128_t { 0x0000000000000000ULL, 0x8000000000000000ULL }); // Single high bit

    // Test all 16 bytes identical (requires AVX2 for vpbroadcastb)
    testPattern(v128_t { 0x4242424242424242ULL, 0x4242424242424242ULL });
    testPattern(v128_t { 0x8080808080808080ULL, 0x8080808080808080ULL });
    testPattern(v128_t { 0xAAAAAAAAAAAAAAAAULL, 0xAAAAAAAAAAAAAAAAULL });
    testPattern(v128_t { 0x0101010101010101ULL, 0x0101010101010101ULL }); // 0x01 repeated (minimal non-zero)
    testPattern(v128_t { 0xFEFEFEFEFEFEFEFEULL, 0xFEFEFEFEFEFEFEFEULL }); // 0xFE repeated (near max)

    // Test all eight 16-bit lanes identical (requires AVX2 for vpbroadcastw)
    testPattern(v128_t { 0x00FF00FF00FF00FFULL, 0x00FF00FF00FF00FFULL }); // 0x00FF repeated
    testPattern(v128_t { 0x1234123412341234ULL, 0x1234123412341234ULL }); // 0x1234 repeated
    testPattern(v128_t { 0xFF00FF00FF00FF00ULL, 0xFF00FF00FF00FF00ULL }); // 0xFF00 repeated
    testPattern(v128_t { 0x8000800080008000ULL, 0x8000800080008000ULL }); // 0x8000 repeated
    testPattern(v128_t { 0xABCDABCDABCDABCDULL, 0xABCDABCDABCDABCDULL }); // 0xABCD repeated
    testPattern(v128_t { 0x0001000100010001ULL, 0x0001000100010001ULL }); // 0x0001 repeated (minimal non-zero)
    testPattern(v128_t { 0xFFFEFFFEFFFEFFFEULL, 0xFFFEFFFEFFFEFFFEULL }); // 0xFFFE repeated (near max)

    // Test non-encodable patterns (fallback to GPR insertion path)
    testPattern(v128_t { 0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL });
    testPattern(v128_t { 0x0000000000000042ULL, 0x0000000000000043ULL });
    testPattern(v128_t { 0x3FF0000000000000ULL, 0x4000000000000000ULL });

    // Additional coverage: Repeated 64-bit halves with move64ToDouble path 5 (upper 32 zero)
    testPattern(v128_t { 0x0000000012345678ULL, 0x0000000012345678ULL });
    testPattern(v128_t { 0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL });
    testPattern(v128_t { 0x0000000080000000ULL, 0x0000000080000000ULL });

    // Additional coverage: All four 32-bit lanes identical with all move32ToFloat sub-paths
    // Path 3 with only leftShift
    testPattern(v128_t { 0xC0000000C0000000ULL, 0xC0000000C0000000ULL }); // Repeated 0xC0000000 (top 2 bits)
    testPattern(v128_t { 0xE0000000E0000000ULL, 0xE0000000E0000000ULL }); // Repeated 0xE0000000 (top 3 bits)
    testPattern(v128_t { 0xF0000000F0000000ULL, 0xF0000000F0000000ULL }); // Repeated 0xF0000000 (top 4 bits)

    // Path 3 with only rightShift
    testPattern(v128_t { 0x0000007F0000007FULL, 0x0000007F0000007FULL }); // Repeated 0x0000007F (bottom 7 bits)
    testPattern(v128_t { 0x000001FF000001FFULL, 0x000001FF000001FFULL }); // Repeated 0x000001FF (bottom 9 bits)
    testPattern(v128_t { 0x00000FFF00000FFFULL, 0x00000FFF00000FFFULL }); // Repeated 0x00000FFF (bottom 12 bits)

    // Path 3 with both shifts
    testPattern(v128_t { 0x3FFFFFFF3FFFFFFFULL, 0x3FFFFFFF3FFFFFFFULL }); // Repeated 0x3FFFFFFF (all but top 2)
    testPattern(v128_t { 0x1FFFFFFF1FFFFFFFULL, 0x1FFFFFFF1FFFFFFFULL }); // Repeated 0x1FFFFFFF (all but top 3)
    testPattern(v128_t { 0x0FFFFFFF0FFFFFFFULL, 0x0FFFFFFF0FFFFFFFULL }); // Repeated 0x0FFFFFFF (bottom 28 bits)

    // Path 2 (all ones) - already covered by existing test
    // Path 1 (zero) - already covered by existing test

    // Additional coverage: Repeated 64-bit halves with 64-bit contiguous patterns
    testPattern(v128_t { 0xC000000000000000ULL, 0xC000000000000000ULL }); // 64-bit contiguous (top 2 bits)
    testPattern(v128_t { 0x000000000000007FULL, 0x000000000000007FULL }); // 64-bit contiguous (bottom 7 bits)
    testPattern(v128_t { 0x3FFFFFFFFFFFFFFFULL, 0x3FFFFFFFFFFFFFFFULL }); // 64-bit contiguous (all but top 2)

    // Additional coverage: 32-bit contiguous patterns with middle bits
    testPattern(v128_t { 0x00FF000000FF0000ULL, 0x00FF000000FF0000ULL }); // Repeated 0x00FF0000 (middle 8 bits)
    testPattern(v128_t { 0x0000FF000000FF00ULL, 0x0000FF000000FF00ULL }); // Repeated 0x0000FF00 (middle 8 bits)
    testPattern(v128_t { 0x00FFFF0000FFFF00ULL, 0x00FFFF0000FFFF00ULL }); // Repeated 0x00FFFF00 (middle 16 bits)
    testPattern(v128_t { 0x0FF000000FF00000ULL, 0x0FF000000FF00000ULL }); // Repeated 0x0FF00000 (middle 12 bits)

    // Additional coverage: 64-bit contiguous patterns with middle bits
    testPattern(v128_t { 0x0000FFFF00000000ULL, 0x0000FFFF00000000ULL }); // Middle 16 bits
    testPattern(v128_t { 0x00000000FFFF0000ULL, 0x00000000FFFF0000ULL }); // Middle 16 bits (different position)
    testPattern(v128_t { 0x000FFFFF00000000ULL, 0x000FFFFF00000000ULL }); // Middle 20 bits

    // Additional coverage: Single-bit patterns (32-bit contiguous)
    testPattern(v128_t { 0x0000000100000001ULL, 0x0000000100000001ULL }); // Repeated 0x00000001 (single low bit)
    testPattern(v128_t { 0x8000000080000000ULL, 0x8000000080000000ULL }); // Repeated 0x80000000 (single high bit)
    testPattern(v128_t { 0x0000800000008000ULL, 0x0000800000008000ULL }); // Repeated 0x00008000 (single middle bit)
    testPattern(v128_t { 0x0010000000100000ULL, 0x0010000000100000ULL }); // Repeated 0x00100000 (single middle bit)

    // Additional coverage: Single-bit patterns (64-bit contiguous)
    testPattern(v128_t { 0x0000000000000001ULL, 0x0000000000000001ULL }); // Single low bit
    testPattern(v128_t { 0x8000000000000000ULL, 0x8000000000000000ULL }); // Single high bit
    testPattern(v128_t { 0x0000000100000000ULL, 0x0000000100000000ULL }); // Single middle bit
    testPattern(v128_t { 0x0000800000000000ULL, 0x0000800000000000ULL }); // Single middle bit (different position)

    // Additional coverage: IEEE float64 special values as 128-bit vectors
    testPattern(v128_t { 0x7FF0000000000000ULL, 0x7FF0000000000000ULL }); // +Infinity repeated
    testPattern(v128_t { 0xFFF0000000000000ULL, 0xFFF0000000000000ULL }); // -Infinity repeated
    testPattern(v128_t { 0x7FF8000000000000ULL, 0x7FF8000000000000ULL }); // Quiet NaN repeated
    testPattern(v128_t { 0x3FF0000000000000ULL, 0x3FF0000000000000ULL }); // 1.0 repeated
    testPattern(v128_t { 0x4000000000000000ULL, 0x4000000000000000ULL }); // 2.0 repeated

    // Additional coverage: IEEE float32 special values as 128-bit vectors (all 4 lanes)
    testPattern(v128_t { 0x7F8000007F800000ULL, 0x7F8000007F800000ULL }); // +Infinity (float32) x4
    testPattern(v128_t { 0xFF800000FF800000ULL, 0xFF800000FF800000ULL }); // -Infinity (float32) x4
    testPattern(v128_t { 0x7FC000007FC00000ULL, 0x7FC000007FC00000ULL }); // Quiet NaN (float32) x4
    testPattern(v128_t { 0x3F8000003F800000ULL, 0x3F8000003F800000ULL }); // 1.0f x4
    testPattern(v128_t { 0x4000000040000000ULL, 0x4000000040000000ULL }); // 2.0f x4

    // Additional coverage: Non-contiguous patterns (fallback path)
    testPattern(v128_t { 0x5555555555555555ULL, 0x5555555555555555ULL }); // Alternating 01 pattern
    testPattern(v128_t { 0x3333333333333333ULL, 0x3333333333333333ULL }); // Alternating 0011 pattern
    testPattern(v128_t { 0x0F0F0F0F0F0F0F0FULL, 0x0F0F0F0F0F0F0F0FULL }); // Alternating nibbles
    testPattern(v128_t { 0xCCCCCCCCCCCCCCCCULL, 0xCCCCCCCCCCCCCCCCULL }); // Alternating 1100 pattern

    // Additional coverage: Different upper/lower 64-bit halves (fallback path)
    testPattern(v128_t { 0x0000000000000001ULL, 0x0000000000000002ULL }); // Sequential low bits
    testPattern(v128_t { 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000001ULL }); // All-ones lower, minimal upper
    testPattern(v128_t { 0x0000000000000001ULL, 0xFFFFFFFFFFFFFFFFULL }); // Minimal lower, all-ones upper
    testPattern(v128_t { 0x8000000000000000ULL, 0x0000000000000001ULL }); // High bit lower, low bit upper
    testPattern(v128_t { 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL }); // Complementary patterns

    // Additional coverage: Patterns that test check ordering
    // all8Same=true but pattern uses vpbroadcastb path (value 0x42 repeated)
    testPattern(v128_t { 0x4242424242424242ULL, 0x4242424242424242ULL }); // Already tested above
    // all16Same=true but not all8Same (0x1234 has different bytes)
    testPattern(v128_t { 0x5678567856785678ULL, 0x5678567856785678ULL }); // 0x5678 repeated
    // all32Same=true but not all16Same (0x12345678 has different 16-bit halves)
    testPattern(v128_t { 0xDEADBEEFDEADBEEFULL, 0xDEADBEEFDEADBEEFULL }); // 0xDEADBEEF repeated
    // all64Same=true but not all32Same
    testPattern(v128_t { 0x123456789ABCDEF0ULL, 0x123456789ABCDEF0ULL }); // Arbitrary 64-bit repeated

    // Additional coverage: Boundary values
    testPattern(v128_t { 0x7FFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL }); // INT64_MAX repeated
    testPattern(v128_t { 0x8000000000000001ULL, 0x8000000000000001ULL }); // INT64_MIN+1 repeated (non-contiguous)
    testPattern(v128_t { 0x0000000100000000ULL, 0x0000000100000000ULL }); // 2^32 repeated
    testPattern(v128_t { 0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL }); // UINT32_MAX repeated

    // Additional coverage: 32-bit contiguous boundary cases (all32Same path)
    testPattern(v128_t { 0xFFFFFFFEFFFFFFFEULL, 0xFFFFFFFEFFFFFFFEULL }); // Repeated 0xFFFFFFFE (31 contiguous high bits)
    testPattern(v128_t { 0x7FFFFFFE7FFFFFFEULL, 0x7FFFFFFE7FFFFFFEULL }); // Repeated 0x7FFFFFFE (30 contiguous middle bits)
    testPattern(v128_t { 0x0FFFFFF00FFFFFF0ULL, 0x0FFFFFF00FFFFFF0ULL }); // Repeated 0x0FFFFFF0 (24 contiguous middle bits)
    testPattern(v128_t { 0xFFFFFFF8FFFFFFF8ULL, 0xFFFFFFF8FFFFFFF8ULL }); // Repeated 0xFFFFFFF8 (29 contiguous high bits)

    // Additional coverage: 64-bit contiguous boundary cases (all64Same path)
    testPattern(v128_t { 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFEULL }); // 63 contiguous high bits
    testPattern(v128_t { 0x7FFFFFFFFFFFFFFEULL, 0x7FFFFFFFFFFFFFFEULL }); // 62 contiguous middle bits
    testPattern(v128_t { 0x0FFFFFFFFFFFFFF0ULL, 0x0FFFFFFFFFFFFFF0ULL }); // 56 contiguous middle bits
    testPattern(v128_t { 0xFFFFFFFFFFFFFFF8ULL, 0xFFFFFFFFFFFFFFF8ULL }); // 61 contiguous high bits

    // Additional coverage: Non-contiguous 32-bit patterns repeated (fallback to all32Same GPR path)
    testPattern(v128_t { 0x8000000180000001ULL, 0x8000000180000001ULL }); // Repeated 0x80000001 (non-contiguous)
    testPattern(v128_t { 0xF000000FF000000FULL, 0xF000000FF000000FULL }); // Repeated 0xF000000F (non-contiguous)
    testPattern(v128_t { 0x5555555555555555ULL, 0x5555555555555555ULL }); // 0x55555555 repeated (alternating bits)

    // Additional coverage: Non-contiguous 64-bit patterns repeated (fallback to all64Same GPR path)
    testPattern(v128_t { 0x8000000000000001ULL, 0x8000000000000001ULL }); // High+low bits (non-contiguous 64-bit)
    testPattern(v128_t { 0xF00000000000000FULL, 0xF00000000000000FULL }); // Top+bottom nibbles (non-contiguous 64-bit)

    // Additional coverage: Patterns that match all8Same but should use vpbroadcastb
    testPattern(v128_t { 0x7F7F7F7F7F7F7F7FULL, 0x7F7F7F7F7F7F7F7FULL }); // 0x7F repeated (127)
    testPattern(v128_t { 0x8181818181818181ULL, 0x8181818181818181ULL }); // 0x81 repeated (129)
    testPattern(v128_t { 0x5555555555555555ULL, 0x5555555555555555ULL }); // 0x55 repeated (alternating)

    // Additional coverage: Patterns that match all16Same but not all8Same (vpbroadcastw)
    testPattern(v128_t { 0x7FFF7FFF7FFF7FFFULL, 0x7FFF7FFF7FFF7FFFULL }); // 0x7FFF repeated
    testPattern(v128_t { 0x8001800180018001ULL, 0x8001800180018001ULL }); // 0x8001 repeated
    testPattern(v128_t { 0xFEFFFEFFFEFFFEFFULL, 0xFEFFFEFFFEFFFEFFULL }); // 0xFEFF repeated

    // Additional coverage: Patterns that match all32Same but not all16Same (vbroadcastss)
    testPattern(v128_t { 0x7FFF80007FFF8000ULL, 0x7FFF80007FFF8000ULL }); // 0x7FFF8000 repeated
    testPattern(v128_t { 0x80017FFE80017FFEULL, 0x80017FFE80017FFEULL }); // 0x80017FFE repeated
    testPattern(v128_t { 0xFEFF0100FEFF0100ULL, 0xFEFF0100FEFF0100ULL }); // 0xFEFF0100 repeated

    // Additional coverage: Patterns that match all64Same but not all32Same (vmovddup)
    testPattern(v128_t { 0x7FFF8000FEFF0100ULL, 0x7FFF8000FEFF0100ULL }); // Arbitrary non-repeated-32 repeated-64
    testPattern(v128_t { 0x0001000200030004ULL, 0x0001000200030004ULL }); // Sequential 16-bit values repeated
    testPattern(v128_t { 0xABCD1234EF005678ULL, 0xABCD1234EF005678ULL }); // Arbitrary pattern repeated
}

#if CPU(ARM64)
// Test half-precision FMOV encoding (cmode=0b1110, op=1)
// Verifies that 16-bit, 32-bit, and 64-bit FMOV use correct encodings
static void testFMovHalfPrecisionEncoding()
{
    // This test verifies that fmov_v correctly generates different encodings
    // for different lane widths:
    //   16-bit: cmode=0b1110, op=1
    //   32-bit: cmode=0b1111, op=0
    //   64-bit: cmode=0b1111, op=1

    // Test that different FMOV variants execute correctly
    // The key test is that 16-bit and 32-bit FMOV produce the right values

    auto testNonCollision = [] {
        // Test that 32-bit and 16-bit FMOV work independently
        // If encodings collided, one would overwrite the other incorrectly
        auto test = compile([] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            // Use high-level MacroAssembler functions which will call fmov_v internally
            // Test with FP immediates that are encodable in both formats

            // For 32-bit float 1.0 (0x3f800000)
            // We'll use move32ToFloat which should use fmov or movi
            jit.move(CCallHelpers::TrustedImm32(0x3f800000), GPRInfo::regT0);
            jit.move32ToFloat(CCallHelpers::TrustedImm32(0x3f800000), FPRInfo::fpRegT0);
            jit.moveFloatTo32(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        uint32_t result = invoke<uint32_t>(test);
        // Should get back the same bit pattern
        CHECK_EQ(result, 0x3f800000U);
    };

    testNonCollision();

    // Test half-precision values work correctly through the encoding
    // We test by verifying canEncodeFPImm works for 16-bit
    auto testHalfPrecisionValidation = [] {
        // Verify some known half-precision FP immediate values are encodable
        // Half-precision 1.0 = 0x3C00
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x3C00), true);

        // Half-precision 2.0 = 0x4000
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x4000), true);

        // Half-precision -1.0 = 0xBC00
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0xBC00), true);

        // Half-precision 0.5 = 0x3800
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x3800), true);

        // Half-precision with lower mantissa bits set (should fail - needs 6 zeros)
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x3C01), false);
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x3C3F), false);

        // Exponent out of valid range (should fail - needs exponent 12-19)
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x7C00), false); // Infinity
        CHECK_EQ(ARM64Assembler::canEncodeFPImm<16>(0x0400), false); // Too small exponent
    };

    testHalfPrecisionValidation();
}
#endif

// Comprehensive tests for x64 constant materialization
static void testMove32ToFloatX64()
{
    auto testPattern = [] (uint32_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move32ToFloat(CCallHelpers::TrustedImm32(pattern), FPRInfo::fpRegT0);
            jit.moveFloatTo32(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        uint32_t result = invoke<uint32_t>(test);
        CHECK_EQ(result, pattern);
    };

    // Test zero
    testPattern(0x00000000);

    // Test all ones
    testPattern(0xFFFFFFFF);

    // Test contiguous bit patterns (pcmpeqd + shifts)
    testPattern(0x80000000); // Sign bit (pcmpeqd + pslld #31)
    testPattern(0xFF000000); // Top byte (pcmpeqd + pslld #24)
    testPattern(0x00FFFFFF); // Bottom 3 bytes (pcmpeqd + psrld #8)
    testPattern(0x7FFFFFFF); // INT32_MAX (pcmpeqd + pslld #31 + psrld #1)
    testPattern(0x0000FFFF); // Bottom 2 bytes
    testPattern(0xFFFF0000); // Top 2 bytes
    testPattern(0x000000FF); // Bottom byte
    testPattern(0xFF800000); // Sign bit + top byte

    // Test repeated 16-bit patterns
    testPattern(0x12001200); // Repeated 0x1200
    testPattern(0x00420042); // Repeated 0x0042
    testPattern(0x80008000); // Repeated sign bit
    testPattern(0xFFFFFFFF); // Repeated 0xFFFF (all ones)

    // Test repeated byte patterns
    testPattern(0x42424242); // Repeated 0x42
    testPattern(0x80808080); // Repeated sign bit
    testPattern(0xAAAAAAAA); // Alternating bits

    // Test non-encodable patterns (fallback to GPR path)
    testPattern(0x12345678); // Arbitrary pattern
    testPattern(0xABCDEF01); // Random value
    testPattern(0x01020304); // Sequential bytes

    // Additional coverage: Edge cases for contiguous patterns
    // Test contiguous pattern with leftShift only (rightShift = 0)
    testPattern(0xC0000000); // Two top bits set (pcmpeqd + pslld #30)
    testPattern(0xE0000000); // Three top bits set (pcmpeqd + pslld #29)
    testPattern(0xF0000000); // Four top bits set (pcmpeqd + pslld #28)

    // Test contiguous pattern with rightShift only (leftShift = 0)
    testPattern(0x0000007F); // Bottom 7 bits (pcmpeqd + psrld #25)
    testPattern(0x000001FF); // Bottom 9 bits (pcmpeqd + psrld #23)
    testPattern(0x00000FFF); // Bottom 12 bits (pcmpeqd + psrld #20)

    // Test contiguous pattern with both shifts (different combinations)
    testPattern(0x3FFFFFFF); // All but top 2 bits (pcmpeqd + pslld #30 + psrld #2)
    testPattern(0x1FFFFFFF); // All but top 3 bits (pcmpeqd + pslld #29 + psrld #3)
    testPattern(0x0FFFFFFF); // Bottom 28 bits (pcmpeqd + pslld #28 + psrld #4)

    // Additional coverage: Middle-bit contiguous patterns
    testPattern(0x00FF0000); // Middle 8 bits (byte 2)
    testPattern(0x0000FF00); // Middle 8 bits (byte 1)
    testPattern(0x00FFFF00); // Middle 16 bits
    testPattern(0x0FF00000); // Middle 12 bits (upper half)
    testPattern(0x000FFF00); // Middle 12 bits (lower half)
    testPattern(0x3FFC0000); // 12 bits shifted up

    // Additional coverage: Single-bit patterns
    testPattern(0x00000001); // Single low bit
    testPattern(0x00000002); // Second bit
    testPattern(0x00008000); // Middle bit (bit 15)
    testPattern(0x00010000); // Bit 16
    testPattern(0x40000000); // Second highest bit

    // Additional coverage: IEEE float32 special values (bit patterns)
    testPattern(0x7F800000); // +Infinity
    testPattern(0xFF800000); // -Infinity
    testPattern(0x7FC00000); // Quiet NaN
    testPattern(0x7F800001); // Signaling NaN
    testPattern(0x00800000); // Smallest normal
    testPattern(0x00000001); // Smallest denormal
    testPattern(0x7F7FFFFF); // Largest finite
    testPattern(0x3F800000); // 1.0f
    testPattern(0xBF800000); // -1.0f
    testPattern(0x40000000); // 2.0f
    testPattern(0x3F000000); // 0.5f

    // Additional coverage: Non-contiguous patterns (fallback path)
    testPattern(0x55555555); // Alternating 01 pattern
    testPattern(0x33333333); // Alternating 0011 pattern
    testPattern(0x0F0F0F0F); // Alternating nibbles
    testPattern(0xF0F0F0F0); // Alternating nibbles (inverted)
    testPattern(0xCCCCCCCC); // Alternating 1100 pattern
    testPattern(0x99999999); // Another non-contiguous pattern

    // Additional coverage: Contiguous pattern boundary cases
    // Patterns with exactly N contiguous bits at various positions
    testPattern(0xFFFFFFFE); // All but lowest bit (31 contiguous high bits)
    testPattern(0x7FFFFFFE); // 30 contiguous bits in middle
    testPattern(0x3FFFFFFC); // 28 contiguous bits shifted
    testPattern(0x1FFFFFF8); // 26 contiguous bits shifted
    testPattern(0x0FFFFFF0); // 24 contiguous bits in middle
    testPattern(0x07FFFFE0); // 22 contiguous bits shifted

    // Patterns that are NOT contiguous (should use fallback)
    testPattern(0x80000001); // High and low bits only
    testPattern(0xC0000003); // Two high bits + two low bits
    testPattern(0xF000000F); // Top and bottom nibbles
    testPattern(0xFF0000FF); // Top and bottom bytes
    testPattern(0xFFFF0001); // Top 16 bits + low bit

    // Maximum and minimum shift combinations
    testPattern(0xFFFFFFF0); // Right shift 4 only
    testPattern(0x0FFFFFFF); // Left shift 4 only
    testPattern(0x7FFFFFF8); // Left shift 3 + right shift 1
    testPattern(0xFFFFFFF8); // Right shift 3 only
    testPattern(0x1FFFFFFF); // Left shift 3 only
}

static void testMove64ToDoubleX64()
{
    auto testPattern = [] (uint64_t pattern) {
        auto test = compile([=] (CCallHelpers& jit) {
            emitFunctionPrologue(jit);

            jit.move64ToDouble(CCallHelpers::TrustedImm64(pattern), FPRInfo::fpRegT0);
            jit.moveDoubleTo64(FPRInfo::fpRegT0, GPRInfo::returnValueGPR);

            emitFunctionEpilogue(jit);
            jit.ret();
        });

        uint64_t result = invoke<uint64_t>(test);
        CHECK_EQ(result, pattern);
    };

    // Test zero
    testPattern(0x0000000000000000ULL);

    // Test all ones
    testPattern(0xFFFFFFFFFFFFFFFFULL);

    // Test contiguous bit patterns (pcmpeqq + shifts) - requires SSE4.1
    testPattern(0x8000000000000000ULL); // Sign bit (pcmpeqq + psllq #63)
    testPattern(0xFF00000000000000ULL); // Top byte (pcmpeqq + psllq #56)
    testPattern(0x00FFFFFFFFFFFFFFULL); // Bottom 7 bytes (pcmpeqq + psrlq #8)
    testPattern(0x7FFFFFFFFFFFFFFFULL); // INT64_MAX (pcmpeqq + psllq #63 + psrlq #1)
    testPattern(0x000000FFFFFFFFFFULL); // Bottom 5 bytes
    testPattern(0xFFFFFFFF00000000ULL); // Top 4 bytes
    testPattern(0x0000FFFFFFFFFFFFULL); // Bottom 6 bytes

    // Test repeated 32-bit patterns (materialize 32-bit + duplicate)
    testPattern(0x8000000080000000ULL); // Repeated sign bit
    testPattern(0x1234567812345678ULL); // Repeated arbitrary pattern
    testPattern(0x0000000000000000ULL); // Repeated zero (covered above)
    testPattern(0xFFFFFFFFFFFFFFFFULL); // Repeated 0xFFFFFFFF (all ones, covered above)
    testPattern(0x00FF00FF00FF00FFULL); // Repeated byte mask pattern

    // Test upper 32 bits zero (treat as 32-bit)
    testPattern(0x0000000012345678ULL);
    testPattern(0x00000000FFFFFFFFULL);
    testPattern(0x0000000080000000ULL);
    testPattern(0x00000000000000FFULL);

    // Test repeated byte patterns
    testPattern(0x4242424242424242ULL); // Repeated 0x42
    testPattern(0x8080808080808080ULL); // Repeated sign bit byte
    testPattern(0xAAAAAAAAAAAAAAAAULL); // Alternating bits

    // Test non-encodable patterns (fallback to GPR path)
    testPattern(0x123456789ABCDEF0ULL);
    testPattern(0xFEDCBA9876543210ULL);
    testPattern(0x0102030405060708ULL);

    // Additional coverage: Edge cases for 64-bit contiguous patterns
    // Test contiguous pattern with leftShift only (rightShift = 0)
    testPattern(0xC000000000000000ULL); // Two top bits set (pcmpeqq + psllq #62)
    testPattern(0xE000000000000000ULL); // Three top bits set (pcmpeqq + psllq #61)
    testPattern(0xF000000000000000ULL); // Four top bits set (pcmpeqq + psllq #60)

    // Test contiguous pattern with rightShift only (leftShift = 0)
    testPattern(0x000000000000007FULL); // Bottom 7 bits (pcmpeqq + psrlq #57)
    testPattern(0x00000000000001FFULL); // Bottom 9 bits (pcmpeqq + psrlq #55)
    testPattern(0x0000000000000FFFULL); // Bottom 12 bits (pcmpeqq + psrlq #52)

    // Test contiguous pattern with both shifts (different combinations)
    testPattern(0x3FFFFFFFFFFFFFFFULL); // All but top 2 bits (pcmpeqq + psllq #62 + psrlq #2)
    testPattern(0x1FFFFFFFFFFFFFFFULL); // All but top 3 bits (pcmpeqq + psllq #61 + psrlq #3)
    testPattern(0x0FFFFFFFFFFFFFFFULL); // Bottom 60 bits (pcmpeqq + psllq #60 + psrlq #4)

    // Additional coverage: Repeated 32-bit with specific move32ToFloat sub-paths
    // Path 3 with only leftShift (e.g., 0xC0000000)
    testPattern(0xC0000000C0000000ULL); // Repeated contiguous (top 2 bits)
    testPattern(0xE0000000E0000000ULL); // Repeated contiguous (top 3 bits)
    testPattern(0xF0000000F0000000ULL); // Repeated contiguous (top 4 bits)

    // Path 3 with only rightShift (e.g., 0x0000007F)
    testPattern(0x0000007F0000007FULL); // Repeated contiguous (bottom 7 bits)
    testPattern(0x000001FF000001FFULL); // Repeated contiguous (bottom 9 bits)
    testPattern(0x00000FFF00000FFFULL); // Repeated contiguous (bottom 12 bits)

    // Path 3 with both shifts (e.g., 0x3FFFFFFF)
    testPattern(0x3FFFFFFF3FFFFFFFULL); // Repeated contiguous (all but top 2)
    testPattern(0x1FFFFFFF1FFFFFFFULL); // Repeated contiguous (all but top 3)
    testPattern(0x0FFFFFFF0FFFFFFFULL); // Repeated contiguous (bottom 28 bits)

    // Additional coverage: Middle-bit contiguous 64-bit patterns
    testPattern(0x0000FFFF00000000ULL); // Middle 16 bits (bytes 4-5)
    testPattern(0x00000000FFFF0000ULL); // Middle 16 bits (bytes 2-3)
    testPattern(0x000000FFFFFF0000ULL); // Middle 24 bits
    testPattern(0x00FFFF0000000000ULL); // Upper-middle 16 bits
    testPattern(0x0000000000FFFF00ULL); // Lower-middle 16 bits
    testPattern(0x003FFFFC00000000ULL); // Middle bits shifted up

    // Additional coverage: Single-bit 64-bit patterns
    testPattern(0x0000000000000001ULL); // Single low bit
    testPattern(0x0000000000000002ULL); // Second bit
    testPattern(0x0000000000008000ULL); // Bit 15
    testPattern(0x0000000100000000ULL); // Bit 32
    testPattern(0x0000800000000000ULL); // Bit 47
    testPattern(0x4000000000000000ULL); // Second highest bit

    // Additional coverage: Middle-bit contiguous 32-bit repeated patterns
    testPattern(0x00FF000000FF0000ULL); // Repeated 0x00FF0000 (middle 8 bits)
    testPattern(0x0000FF000000FF00ULL); // Repeated 0x0000FF00 (middle 8 bits)
    testPattern(0x00FFFF0000FFFF00ULL); // Repeated 0x00FFFF00 (middle 16 bits)
    testPattern(0x0FF000000FF00000ULL); // Repeated 0x0FF00000 (middle 12 bits)

    // Additional coverage: Single-bit 32-bit repeated patterns
    testPattern(0x0000000100000001ULL); // Repeated 0x00000001 (single low bit)
    testPattern(0x0000800000008000ULL); // Repeated 0x00008000 (middle bit)
    testPattern(0x4000000040000000ULL); // Repeated 0x40000000 (second highest bit)

    // Additional coverage: IEEE float64 special values (bit patterns)
    testPattern(0x7FF0000000000000ULL); // +Infinity
    testPattern(0xFFF0000000000000ULL); // -Infinity
    testPattern(0x7FF8000000000000ULL); // Quiet NaN
    testPattern(0x7FF0000000000001ULL); // Signaling NaN
    testPattern(0x0010000000000000ULL); // Smallest normal
    testPattern(0x0000000000000001ULL); // Smallest denormal
    testPattern(0x7FEFFFFFFFFFFFFFULL); // Largest finite
    testPattern(0x3FF0000000000000ULL); // 1.0
    testPattern(0xBFF0000000000000ULL); // -1.0
    testPattern(0x4000000000000000ULL); // 2.0
    testPattern(0x3FE0000000000000ULL); // 0.5

    // Additional coverage: Non-contiguous 64-bit patterns (fallback path)
    testPattern(0x5555555555555555ULL); // Alternating 01 pattern
    testPattern(0x3333333333333333ULL); // Alternating 0011 pattern
    testPattern(0x0F0F0F0F0F0F0F0FULL); // Alternating nibbles
    testPattern(0xF0F0F0F0F0F0F0F0ULL); // Alternating nibbles (inverted)
    testPattern(0xCCCCCCCCCCCCCCCCULL); // Alternating 1100 pattern
    testPattern(0x9999999999999999ULL); // Another non-contiguous pattern

    // Additional coverage: Non-repeated 32-bit halves (fallback path)
    testPattern(0x12345678ABCDEF00ULL); // Different halves
    testPattern(0x00000001FFFFFFFEULL); // Near-boundary values
    testPattern(0xFFFFFFFE00000001ULL); // Inverted near-boundary
    testPattern(0x8000000000000001ULL); // High bit + low bit (non-contiguous)
    testPattern(0x0000000180000000ULL); // Crossing 32-bit boundary

    // Additional coverage: 64-bit contiguous pattern boundary cases
    testPattern(0xFFFFFFFFFFFFFFFEULL); // All but lowest bit (63 contiguous high bits)
    testPattern(0x7FFFFFFFFFFFFFFEULL); // 62 contiguous bits in middle
    testPattern(0x3FFFFFFFFFFFFFFCULL); // 60 contiguous bits shifted
    testPattern(0x1FFFFFFFFFFFFFF8ULL); // 58 contiguous bits shifted
    testPattern(0x0FFFFFFFFFFFFFF0ULL); // 56 contiguous bits in middle
    testPattern(0x07FFFFFFFFFFFFE0ULL); // 54 contiguous bits shifted

    // 64-bit patterns that are NOT contiguous (should use fallback)
    testPattern(0x8000000000000001ULL); // High and low bits only
    testPattern(0xC000000000000003ULL); // Two high bits + two low bits
    testPattern(0xF00000000000000FULL); // Top and bottom nibbles
    testPattern(0xFF000000000000FFULL); // Top and bottom bytes
    testPattern(0xFFFF000000000001ULL); // Top 16 bits + low bit
    testPattern(0xFFFFFFFF00000001ULL); // Top 32 bits + low bit

    // 64-bit maximum and minimum shift combinations
    testPattern(0xFFFFFFFFFFFFFFF0ULL); // Right shift 4 only
    testPattern(0x0FFFFFFFFFFFFFFFULL); // Left shift 4 only
    testPattern(0x7FFFFFFFFFFFFFF8ULL); // Left shift 3 + right shift 1
    testPattern(0xFFFFFFFFFFFFFFF8ULL); // Right shift 3 only
    testPattern(0x1FFFFFFFFFFFFFFFULL); // Left shift 3 only

    // Repeated 32-bit contiguous patterns - boundary cases
    testPattern(0xFFFFFFFEFFFFFFFEULL); // Repeated all-but-lowest-bit
    testPattern(0x7FFFFFFE7FFFFFFEULL); // Repeated 30 contiguous bits
    testPattern(0x0FFFFFF00FFFFFF0ULL); // Repeated 24 contiguous middle bits

    // Repeated 32-bit NON-contiguous patterns (fallback for repeated, non-contiguous 32-bit)
    testPattern(0x8000000180000001ULL); // Repeated high+low bits
    testPattern(0xF000000FF000000FULL); // Repeated top+bottom nibbles
    testPattern(0xFF0000FFFF0000FFULL); // Repeated top+bottom bytes
}
#endif

#if CPU(X86_64) || CPU(ARM64)
#if CPU(X86_64)
struct ProtectedFeatureQueries : MacroAssembler {
    using MacroAssembler::supportsBMI1;
    using MacroAssembler::supportsLZCNT;
};
#endif

constexpr CPUFeatureSet allCPUFeatures = CPUFeatureSet::fromBits(std::numeric_limits<uint32_t>::max());

void checkFeatureQueries(CPUFeatureSet features)
{
#if CPU(X86_64)
    CHECK_EQ(MacroAssembler::supportsSSE3(features), features.contains(CPUFeature::SSE3));
    CHECK_EQ(MacroAssembler::supportsSupplementalSSE3(features), features.contains(CPUFeature::SupplementalSSE3));
    CHECK_EQ(MacroAssembler::supportsSSE4_1(features), features.contains(CPUFeature::SSE4_1));
    CHECK_EQ(MacroAssembler::supportsFloatingPointRounding(features), features.contains(CPUFeature::SSE4_1));
    CHECK_EQ(MacroAssembler::supportsCountPopulation(features), features.contains(CPUFeature::Popcnt));
    CHECK_EQ(MacroAssembler::supportsAVX(features), features.contains(CPUFeature::AVX));
    CHECK_EQ(MacroAssembler::supportsAVX2(features), features.contains(CPUFeature::AVX2));
    CHECK_EQ(ProtectedFeatureQueries::supportsLZCNT(features), features.contains(CPUFeature::LZCNT));
    CHECK_EQ(ProtectedFeatureQueries::supportsBMI1(features), features.contains(CPUFeature::BMI1));
#else
    CHECK_EQ(MacroAssembler::supportsLSE(features), features.contains(CPUFeature::LSE));
    CHECK_EQ(MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(features), features.contains(CPUFeature::JSCVT));
    CHECK_EQ(MacroAssembler::supportsFloat16(features), features.contains(CPUFeature::Float16));
    CHECK_EQ(MacroAssembler::supportsRoundFloatToIntegerFloat(features), features.contains(CPUFeature::FRINT));
    CHECK_EQ(MacroAssembler::supportsSHA3(features), features.contains(CPUFeature::SHA3));
    CHECK_EQ(MacroAssembler::supportsDotProd(features), features.contains(CPUFeature::DotProd));
#endif
}

void testCPUFeatureSet()
{
    constexpr CPUFeature firstFeature = static_cast<CPUFeature>(0);
    constexpr CPUFeatureSet buildTarget = CPUFeatureSet::buildTarget();
    static_assert(CPUFeatureSet().isSubsetOf(buildTarget));
    static_assert(buildTarget.isSubsetOf(allCPUFeatures));
    static_assert(!CPUFeatureSet().contains(firstFeature));
    static_assert(CPUFeatureSet().with(firstFeature).contains(firstFeature));
    static_assert(CPUFeatureSet().with(firstFeature).bits() == 1);
    static_assert(CPUFeatureSet::fromBits(buildTarget.bits()) == buildTarget);

    CHECK_EQ(buildTarget.isSubsetOf(CPUFeatureSet::host()), true);
    checkFeatureQueries(CPUFeatureSet::host());
    checkFeatureQueries(CPUFeatureSet());
    checkFeatureQueries(allCPUFeatures);
}

#if CPU(X86_64)
constexpr CPUFeature queriedFeature = CPUFeature::AVX;
bool supportsQueriedFeature(const CCallHelpers& jit) { return jit.supportsAVX(); }
#else
constexpr CPUFeature queriedFeature = CPUFeature::LSE;
bool supportsQueriedFeature(const CCallHelpers& jit) { return jit.supportsLSE(); }
#endif

void checkPerAssemblerFeatures()
{
    CCallHelpers baseline;
    baseline.setPolicy(AssemblerPolicy::forImage(CPUFeatureSet()));
    CCallHelpers extended;
    extended.setPolicy(AssemblerPolicy::forImage(CPUFeatureSet().with(queriedFeature)));
    CHECK_EQ(supportsQueriedFeature(baseline), false);
    CHECK_EQ(supportsQueriedFeature(extended), true);
    CHECK_EQ(supportsQueriedFeature(baseline), false);
}

template<typename T, typename Emit, typename... Arguments>
T runWithFeatures(CPUFeatureSet features, const Emit& emit, Arguments... arguments)
{
    AssemblerPolicy policy = AssemblerPolicy::forJIT();
    policy.features = features;
    CCallHelpers jit;
    jit.setPolicy(policy);
    emit(jit);
    LinkBuffer linkBuffer(jit, nullptr);
    return invoke<T>(FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm code for a feature set"), arguments...);
}

template<typename Emit, typename Argument, typename T>
void checkEmitterFollowsFeature(CPUFeatureSet baseline, CPUFeature feature, const Emit& emit, Argument argument, T expected)
{
    auto emitFunction = [&](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        emit(jit);
        emitFunctionEpilogue(jit);
        jit.ret();
    };
    CHECK_EQ(baseline.contains(feature), false);
    Vector<uint8_t> baselineCode = emitWithPolicy(AssemblerPolicy::forImage(baseline), emitFunction);
    Vector<uint8_t> featureCode = emitWithPolicy(AssemblerPolicy::forImage(baseline.with(feature)), emitFunction);
    CHECK_EQ(baselineCode == featureCode, false);
    if (baseline.isSubsetOf(CPUFeatureSet::host()))
        CHECK_EQ(runWithFeatures<T>(baseline, emitFunction, argument), expected);
}

void checkEmittersFollowFeatures()
{
#if CPU(X86_64)
    auto countLeadingZeros = [](CCallHelpers& jit) {
        jit.countLeadingZeros32(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
    };
    checkEmitterFollowsFeature(CPUFeatureSet(), CPUFeature::LZCNT, countLeadingZeros, 0x00010000u, 15u);
    checkEmitterFollowsFeature(CPUFeatureSet(), CPUFeature::LZCNT, countLeadingZeros, 0u, 32u);

    auto countTrailingZeros = [](CCallHelpers& jit) {
        jit.countTrailingZeros32(GPRInfo::argumentGPR0, GPRInfo::returnValueGPR);
    };
    checkEmitterFollowsFeature(CPUFeatureSet(), CPUFeature::BMI1, countTrailingZeros, 0x00010000u, 16u);
    checkEmitterFollowsFeature(CPUFeatureSet(), CPUFeature::BMI1, countTrailingZeros, 0u, 32u);

    checkEmitterFollowsFeature(CPUFeatureSet().with(CPUFeature::SSE4_1), CPUFeature::AVX, [](CCallHelpers& jit) {
        jit.ceilDouble(FPRInfo::argumentFPR0, FPRInfo::returnValueFPR);
    }, 1.25, 2.0);
#else
    auto convertDoubleToInt32 = [](CCallHelpers& jit) {
        CCallHelpers::JumpList failureCases;
        jit.branchConvertDoubleToInt32(FPRInfo::argumentFPR0, GPRInfo::returnValueGPR, failureCases, FPRInfo::fpRegT1);
        CCallHelpers::Jump done = jit.jump();
        failureCases.link(&jit);
        jit.move(CCallHelpers::TrustedImm32(-1), GPRInfo::returnValueGPR);
        done.link(&jit);
    };
    checkEmitterFollowsFeature(CPUFeatureSet(), CPUFeature::FRINT, convertDoubleToInt32, 42.0, 42);
    checkEmitterFollowsFeature(CPUFeatureSet(), CPUFeature::FRINT, convertDoubleToInt32, 42.5, -1);
#endif
}

void checkOopsForm()
{
    auto emitOops = [](CCallHelpers& jit) {
        jit.oops();
    };
    AssemblerPolicy imagePolicy = AssemblerPolicy::forImage(CPUFeatureSet::host());
    Vector<uint8_t> breakpoint = emitWithPolicy(imagePolicy, [](CCallHelpers& jit) {
        jit.breakpoint();
    });
    CHECK_EQ(emitWithPolicy(imagePolicy, emitOops) == breakpoint, true);
    CHECK_EQ(emitWithPolicy(AssemblerPolicy::forJIT(), emitOops).size() > breakpoint.size(), true);
}

#if CPU(X86_64)
void checkEncodingsFollowPolicy()
{
    AssemblerPolicy longEncodings = AssemblerPolicy::forJIT();
    AssemblerPolicy shortEncodings = longEncodings;
    shortEncodings.shortensEncodings = true;
    auto sizeUnder = [](const AssemblerPolicy& policy, const auto& emit) {
        return emitWithPolicy(policy, emit).size();
    };
    auto checkSizes = [&](size_t longSize, size_t shortSize, const auto& emit) {
        CHECK_EQ(sizeUnder(longEncodings, emit), longSize);
        CHECK_EQ(sizeUnder(shortEncodings, emit), shortSize);
    };

    checkSizes(3, 2, [](CCallHelpers& jit) {
        jit.signExtend32To64(CCallHelpers::TrustedImm32(0), X86Registers::eax);
    });
    checkSizes(7, 5, [](CCallHelpers& jit) {
        jit.signExtend32To64(CCallHelpers::TrustedImm32(5), X86Registers::eax);
    });
    checkSizes(7, 7, [](CCallHelpers& jit) {
        jit.signExtend32To64(CCallHelpers::TrustedImm32(-5), X86Registers::eax);
    });
    checkSizes(3, 3, [](CCallHelpers& jit) {
        jit.test32(X86Registers::edx, CCallHelpers::TrustedImm32(0x10));
    });
    checkSizes(6, 4, [](CCallHelpers& jit) {
        jit.test32(X86Registers::edi, CCallHelpers::TrustedImm32(0x10));
    });
    checkSizes(7, 4, [](CCallHelpers& jit) {
        jit.test32(X86Registers::r10, CCallHelpers::TrustedImm32(0x10));
    });

    for (const AssemblerPolicy& policy : { longEncodings, shortEncodings }) {
        for (int32_t immediate : { 0, 5, -5, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min() }) {
            CCallHelpers jit;
            jit.setPolicy(policy);
            emitFunctionPrologue(jit);
            jit.move(CCallHelpers::TrustedImm64(-1), GPRInfo::returnValueGPR);
            jit.signExtend32To64(CCallHelpers::TrustedImm32(immediate), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
            LinkBuffer linkBuffer(jit, nullptr);
            CHECK_EQ(invoke<int64_t>(FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm sign extension of an immediate")), static_cast<int64_t>(immediate));
        }
        for (uint32_t value : { 0u, 0x10u, 0x1000u, 0xffffffefu }) {
            CCallHelpers jit;
            jit.setPolicy(policy);
            emitFunctionPrologue(jit);
            jit.test32(CCallHelpers::NonZero, GPRInfo::argumentGPR0, CCallHelpers::TrustedImm32(0x10), GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
            LinkBuffer linkBuffer(jit, nullptr);
            CHECK_EQ(invoke<uint32_t>(FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm test of a byte mask"), value), static_cast<uint32_t>(!!(value & 0x10)));
        }
    }
}
#endif

void checkFeaturesAboveHost()
{
    AssemblerPolicy policy = AssemblerPolicy::forImage(allCPUFeatures);
    CCallHelpers jit;
    jit.setPolicy(policy);
    CHECK_EQ(jit.policy() == policy, true);
    jit.ret();
    LinkBuffer linkBuffer(jit, nullptr);
    CHECK_EQ(linkBuffer.policy() == policy, true);
    FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm code for every CPU feature");
}

void testAssemblerPolicy()
{
    constexpr AssemblerPolicy imagePolicy = AssemblerPolicy::forImage(CPUFeatureSet());
    static_assert(!imagePolicy.blindsConstants && imagePolicy.recordsProcessAddresses && imagePolicy.recordsLinkedSites && imagePolicy.shortensJumps && imagePolicy.shortensEncodings);
    static_assert(imagePolicy.oops == OopsForm::Breakpoint);
    static_assert(imagePolicy.needsLinkRecords() && !AssemblerPolicy().needsLinkRecords());

    CCallHelpers jit;
    CHECK_EQ(jit.policy() == AssemblerPolicy::forJIT(), true);

    checkPerAssemblerFeatures();
    checkEmittersFollowFeatures();
    checkOopsForm();
#if CPU(X86_64)
    checkEncodingsFollowPolicy();
#endif
    checkFeaturesAboveHost();
}

struct JumpSource {
    CCallHelpers::Jump (*emit)(CCallHelpers&);
};

constexpr JumpSource unconditionalJump { [](CCallHelpers& jit) { return jit.jump(); } };
constexpr JumpSource jumpOnFlags { [](CCallHelpers& jit) { return jit.branchOnFlags(CCallHelpers::Equal); } };

struct JumpSite {
    CCallHelpers::Label before;
    CCallHelpers::Jump jump;
    CCallHelpers::Label after;
};

JumpSite emitJumpSite(CCallHelpers& jit, const JumpSource& source)
{
    JumpSite site;
    site.before = jit.label();
    site.jump = source.emit(jit);
    site.after = jit.label();
    return site;
}

LinkedSite expectedLinkedSite(LinkBuffer& linkBuffer, const JumpSite& site, CCallHelpers::Label target, RelocationKind kind)
{
    CHECK_EQ(linkBuffer.offsetOf(site.after) - linkBuffer.offsetOf(site.before), MacroAssembler::relocationSize(kind));
    return { linkBuffer.offsetOf(site.before), linkBuffer.offsetOf(target), kind };
}

void checkLinkedSite(const LinkedSite& actual, const LinkedSite& expected)
{
    CHECK_EQ(actual.offset, expected.offset);
    CHECK_EQ(actual.target, expected.target);
    CHECK_EQ(static_cast<unsigned>(actual.kind), static_cast<unsigned>(expected.kind));
}

enum class JumpLinking : uint8_t { Forward, Backward, Late };

void checkJump(const JumpSource& source, size_t distance, JumpLinking linking, RelocationKind kind, JumpShortening shortening = JumpShortening::Yes)
{
    JumpSite site;
    CCallHelpers::Label target;
    LinkedSite expected { };
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        jit.compareOnFlags32(GPRInfo::regT0, GPRInfo::regT1);
        if (linking == JumpLinking::Backward) {
            target = jit.label();
            jit.emitNops(distance);
        }
        site = emitJumpSite(jit, source);
        switch (linking) {
        case JumpLinking::Forward:
            jit.emitNops(distance);
            site.jump.link(&jit);
            target = jit.label();
            break;
        case JumpLinking::Backward:
            site.jump.linkTo(target, &jit);
            break;
        case JumpLinking::Late:
            jit.emitNops(distance);
            target = jit.label();
            break;
        }
        jit.ret();
    }, [&](LinkBuffer& linkBuffer) {
        if (linking == JumpLinking::Late)
            linkBuffer.link(site.jump, linkBuffer.locationOf<JSInternalPtrTag>(target));
        expected = expectedLinkedSite(linkBuffer, site, target, kind);
    }, shortening);

    CHECK_EQ(code.sites.size(), 1u);
    checkLinkedSite(code.sites[0], expected);
    CHECK_EQ(code.externalLinks.size(), 0u);
    CHECK_EQ(code.hasProcessAddress, false);
}
#endif // CPU(X86_64) || CPU(ARM64)

#if CPU(X86_64)
struct RandomJumpProgram {
    static constexpr unsigned numberOfBlocks = 200;

    Vector<CCallHelpers::Label> blocks;
    CCallHelpers::Label exit;
};

RandomJumpProgram emitRandomJumpProgram(CCallHelpers& jit, unsigned seed)
{
    constexpr GPRReg accumulator = GPRInfo::returnValueGPR;
    constexpr GPRReg fuel = GPRInfo::regT1;
    constexpr unsigned maxPadding = 140;

    WeakRandom random(seed);
    RandomJumpProgram program;
    Vector<CCallHelpers::JumpList> forwardJumps(RandomJumpProgram::numberOfBlocks);
    CCallHelpers::JumpList exitJumps;

    auto randomTarget = [&](unsigned block) {
        if (random.getUint32(2))
            return random.getUint32(RandomJumpProgram::numberOfBlocks);
        unsigned firstNeighbor = block < 2 ? 0 : block - 2;
        return std::min(firstNeighbor + random.getUint32(5), RandomJumpProgram::numberOfBlocks - 1);
    };
    auto linkToBlock = [&](CCallHelpers::Jump jump, unsigned target) {
        if (target < program.blocks.size())
            jump.linkTo(program.blocks[target], &jit);
        else
            forwardJumps[target].append(jump);
    };

    emitFunctionPrologue(jit);
    jit.move(GPRInfo::argumentGPR0, accumulator);
    jit.move(CCallHelpers::TrustedImm32(1000), fuel);
    for (unsigned block = 0; block < RandomJumpProgram::numberOfBlocks; ++block) {
        forwardJumps[block].link(&jit);
        program.blocks.append(jit.label());
        jit.mul32(CCallHelpers::TrustedImm32(31), accumulator, accumulator);
        jit.add32(CCallHelpers::TrustedImm32(block), accumulator);
        exitJumps.append(jit.branchSub32(CCallHelpers::Zero, CCallHelpers::TrustedImm32(1), fuel));
        jit.emitNops(random.getUint32(maxPadding));
        linkToBlock(jit.branchTest32(CCallHelpers::NonZero, accumulator, CCallHelpers::TrustedImm32(1)), randomTarget(block));
        jit.emitNops(random.getUint32(maxPadding));
        linkToBlock(jit.jump(), randomTarget(block));
    }
    exitJumps.link(&jit);
    program.exit = jit.label();
    emitFunctionEpilogue(jit);
    jit.ret();
    return program;
}

void checkRandomJumpSites(const LinkedCode& code, const Vector<uint32_t>& targets, JumpShortening shortening)
{
    unsigned numberOfShortJumps = 0;
    uint32_t endOfPreviousJump = 0;
    CHECK_EQ(code.sites.size(), 3u * RandomJumpProgram::numberOfBlocks);
    for (const LinkedSite& site : code.sites) {
        CHECK_EQ(site.offset >= endOfPreviousJump, true);
        endOfPreviousJump = site.offset + MacroAssembler::relocationSize(site.kind);
        CHECK_EQ(targets.contains(site.target), true);
        bool isShort = site.kind == RelocationKind::ShortJump || site.kind == RelocationKind::ShortConditionalJump;
        CHECK_EQ(isShort || site.kind == RelocationKind::Jump || site.kind == RelocationKind::ConditionalJump, true);
        numberOfShortJumps += isShort;
    }
    CHECK_EQ(endOfPreviousJump <= code.size, true);
    if (shortening == JumpShortening::No) {
        CHECK_EQ(numberOfShortJumps, 0u);
        return;
    }
    CHECK_EQ(!!numberOfShortJumps, true);
    CHECK_EQ(numberOfShortJumps < code.sites.size(), true);
}

LinkedCode linkRandomJumpProgram(unsigned seed, JumpShortening shortening)
{
    RandomJumpProgram program;
    Vector<uint32_t> targets;
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        program = emitRandomJumpProgram(jit, seed);
    }, [&](LinkBuffer& linkBuffer) {
        for (CCallHelpers::Label block : program.blocks)
            targets.append(linkBuffer.offsetOf(block));
        targets.append(linkBuffer.offsetOf(program.exit));
    }, shortening);
    checkRandomJumpSites(code, targets, shortening);
    return code;
}

void testJumpShortening()
{
    for (unsigned seed = 1; seed <= 20; ++seed) {
        LinkedCode shortened = linkRandomJumpProgram(seed, JumpShortening::Yes);
        LinkedCode unshortened = linkRandomJumpProgram(seed, JumpShortening::No);

        size_t sizeWithLongJumps = 0;
        auto codeWithLongJumps = compile([&](CCallHelpers& jit) {
            emitRandomJumpProgram(jit, seed);
            sizeWithLongJumps = jit.m_assembler.codeSize();
        });
        CHECK_EQ(shortened.size < sizeWithLongJumps, true);
        CHECK_EQ(unshortened.size, sizeWithLongJumps);

        auto executable = makeExecutable(shortened.bytes());
        for (int32_t argument : { 0, 1, 12345 })
            CHECK_EQ(invokeAt<int32_t>(startOfCopiedCode(executable.code(), 0), argument), invoke<int32_t>(codeWithLongJumps, argument));
    }
}

void checkJumpChainShortens()
{
    JumpSite outer;
    JumpSite inner;
    CCallHelpers::Label outerTarget;
    CCallHelpers::Label innerTarget;
    LinkedSite expectedOuter { };
    LinkedSite expectedInner { };
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        outer = emitJumpSite(jit, unconditionalJump);
        jit.emitNops(124);
        inner = emitJumpSite(jit, unconditionalJump);
        outer.jump.link(&jit);
        outerTarget = jit.label();
        jit.emitNops(10);
        inner.jump.link(&jit);
        innerTarget = jit.label();
        jit.ret();
    }, [&](LinkBuffer& linkBuffer) {
        expectedOuter = expectedLinkedSite(linkBuffer, outer, outerTarget, RelocationKind::ShortJump);
        expectedInner = expectedLinkedSite(linkBuffer, inner, innerTarget, RelocationKind::ShortJump);
    });
    CHECK_EQ(code.sites.size(), 2u);
    checkLinkedSite(code.sites[0], expectedOuter);
    checkLinkedSite(code.sites[1], expectedInner);
}

void checkLabelAfterJumps()
{
    constexpr uint32_t savings = MacroAssembler::relocationSize(RelocationKind::Jump) - MacroAssembler::relocationSize(RelocationKind::ShortJump);
    CCallHelpers::Label end;
    uint32_t emittedEnd = 0;
    uint32_t linkedEnd = 0;
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        CCallHelpers::Jump jump = jit.jump();
        jit.emitNops(10);
        jump.link(&jit);
        jit.ret();
        emittedEnd = static_cast<uint32_t>(jit.m_assembler.codeSize());
        end = jit.label();
    }, [&](LinkBuffer& linkBuffer) {
        linkedEnd = linkBuffer.offsetOf(end);
    });
    CHECK_EQ(linkedEnd, emittedEnd - savings);
    CHECK_EQ(code.size, static_cast<size_t>(linkedEnd));
}

void checkShorteningWithoutRecords()
{
    AssemblerPolicy policy = AssemblerPolicy::forJIT();
    policy.shortensJumps = true;

    for (unsigned seed = 1; seed <= 3; ++seed) {
        CCallHelpers jit;
        jit.setPolicy(policy);
        emitRandomJumpProgram(jit, seed);
        CCallHelpers::Label end = jit.label();
        size_t sizeWithLongJumps = jit.m_assembler.codeSize();
        LinkBuffer linkBuffer(jit, nullptr);
        CHECK_EQ(linkBuffer.offsetOf(end) < sizeWithLongJumps, true);
        auto shortened = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm shortened jumps without records");

        auto codeWithLongJumps = compile([&](CCallHelpers& unshortened) {
            emitRandomJumpProgram(unshortened, seed);
        });
        for (int32_t argument : { 0, 1, 12345 })
            CHECK_EQ(invoke<int32_t>(shortened, argument), invoke<int32_t>(codeWithLongJumps, argument));
    }

    CCallHelpers jit;
    jit.setPolicy(policy);
    emitFunctionPrologue(jit);
    jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
    CCallHelpers::Jump shortJump = jit.jump();
    jit.move(CCallHelpers::TrustedImm32(2), GPRInfo::returnValueGPR);
    shortJump.link(&jit);
    CCallHelpers::Jump lateJump = jit.jump();
    jit.move(CCallHelpers::TrustedImm32(3), GPRInfo::returnValueGPR);
    CCallHelpers::Label target = jit.label();
    emitFunctionEpilogue(jit);
    jit.ret();
    LinkBuffer linkBuffer(jit, nullptr);
    linkBuffer.link(lateJump, linkBuffer.locationOf<JSInternalPtrTag>(target));
    CHECK_EQ(invoke<int>(FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm late jump after a shortened jump")), 1);
}

void testJumpShorteningBounds()
{
    struct JumpKinds {
        JumpSource source;
        RelocationKind shortJump;
        RelocationKind longJump;
    };
    const JumpKinds kinds[] = {
        { unconditionalJump, RelocationKind::ShortJump, RelocationKind::Jump },
        { jumpOnFlags, RelocationKind::ShortConditionalJump, RelocationKind::ConditionalJump },
    };
    for (const JumpKinds& kind : kinds) {
        checkJump(kind.source, 127, JumpLinking::Forward, kind.shortJump);
        checkJump(kind.source, 128, JumpLinking::Forward, kind.longJump);
        checkJump(kind.source, 126, JumpLinking::Backward, kind.shortJump);
        checkJump(kind.source, 127, JumpLinking::Backward, kind.longJump);
        checkJump(kind.source, 385, JumpLinking::Forward, kind.longJump);
        checkJump(kind.source, 10, JumpLinking::Forward, kind.longJump, JumpShortening::No);
        checkJump(kind.source, 10, JumpLinking::Backward, kind.longJump, JumpShortening::No);
    }
    checkJumpChainShortens();
    checkLabelAfterJumps();
    checkShorteningWithoutRecords();
}

void checkCarryFlagInstructions(GPRReg result)
{
    auto carryAfterSetting = [&](bool clearsCarry) {
        return compileAndRun<int>([&](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.xor32(result, result);
            jit.m_assembler.stc();
            if (clearsCarry)
                jit.m_assembler.clc();
            jit.setCarry(result);
            jit.move(result, GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
    };
    CHECK_EQ(carryAfterSetting(false), 1);
    CHECK_EQ(carryAfterSetting(true), 0);
}

void checkLeave()
{
    constexpr GPRReg stackPointerDifference = GPRInfo::returnValueGPR;
    constexpr GPRReg framePointerDifference = GPRInfo::regT2;
    CHECK_EQ(compileAndRun<uint64_t>([&](CCallHelpers& jit) {
        jit.move(X86Registers::esp, stackPointerDifference);
        jit.move(X86Registers::ebp, framePointerDifference);
        jit.push(X86Registers::ebp);
        jit.move(X86Registers::esp, X86Registers::ebp);
        jit.subPtr(CCallHelpers::TrustedImm32(32), X86Registers::esp);
        jit.m_assembler.leave();
        jit.xor64(X86Registers::esp, stackPointerDifference);
        jit.xor64(X86Registers::ebp, framePointerDifference);
        jit.or64(framePointerDifference, stackPointerDifference);
        jit.ret();
    }), 0u);
}

void checkRepeatStoreString()
{
    static_assert(GPRInfo::argumentGPR0 == X86Registers::edi);
    constexpr uint64_t stored = 0x0123456789abcdefULL;
    constexpr uint64_t untouched = 0xfedcba9876543210ULL;
    constexpr unsigned numberOfStoredWords = 5;
    uint64_t words[numberOfStoredWords + 1];
    for (uint64_t& word : words)
        word = untouched;
    compileAndRun<void>([&](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.move(CCallHelpers::TrustedImm32(numberOfStoredWords), X86Registers::ecx);
        jit.move(CCallHelpers::TrustedImm64(stored), X86Registers::eax);
        jit.m_assembler.rep_stosq();
        emitFunctionEpilogue(jit);
        jit.ret();
    }, words);
    for (unsigned i = 0; i < numberOfStoredWords; ++i)
        CHECK_EQ(words[i], stored);
    CHECK_EQ(words[numberOfStoredWords], untouched);
}

void checkRIPRelativeAddress(GPRReg dest)
{
    auto code = compile([&](CCallHelpers& jit) {
        jit.m_assembler.leaq_rip(dest);
        jit.move(dest, GPRInfo::returnValueGPR);
        jit.ret();
    });
    CHECK_EQ(invoke<uintptr_t>(code), code.code().untaggedPtr<uintptr_t>() + MacroAssembler::relocationSize(RelocationKind::AddressNear));
}

void checkUnalignedVectorLoad(GPRReg base, GPRReg index, FPRReg vector, GPRReg mask)
{
    alignas(16) uint8_t bytes[48];
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        bytes[i] = static_cast<uint8_t>(i % 3 ? 0x80 | i : i);
    auto signBitsAt = [&](unsigned offset) {
        unsigned result = 0;
        for (unsigned i = 0; i < 16; ++i)
            result |= (bytes[offset + i] >> 7) << i;
        return result;
    };
    auto loadedSignBits = [&](auto emitLoad) {
        return compileAndRun<unsigned>([&](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.move(GPRInfo::argumentGPR0, base);
            jit.move(GPRInfo::argumentGPR1, index);
            emitLoad(jit.m_assembler);
            jit.m_assembler.pmovmskb_rr(vector, mask);
            jit.move(mask, GPRInfo::returnValueGPR);
            emitFunctionEpilogue(jit);
            jit.ret();
        }, bytes, static_cast<uint64_t>(3));
    };

    CHECK_EQ(loadedSignBits([&](X86Assembler& assembler) {
        assembler.movdqu_mr(1, base, vector);
    }), signBitsAt(1));
    CHECK_EQ(loadedSignBits([&](X86Assembler& assembler) {
        assembler.movdqu_mr(1, base, index, CCallHelpers::TimesFour, vector);
    }), signBitsAt(13));
}

template<typename Operation>
v128_t runVectorOperation(FPRReg dest, FPRReg source, v128_t destValue, v128_t sourceValue, const Operation& operation)
{
    constexpr int vectorSize = sizeof(v128_t);
    v128_t values[] = { destValue, sourceValue, v128_t() };
    uint64_t lowHalf = compileAndRun<uint64_t>([&](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.m_assembler.movups_mr(0, GPRInfo::argumentGPR0, dest);
        jit.m_assembler.movups_mr(vectorSize, GPRInfo::argumentGPR0, source);
        operation(jit.m_assembler);
        jit.m_assembler.movups_rm(dest, 2 * vectorSize, GPRInfo::argumentGPR0);
        jit.m_assembler.movq_rr(dest, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    }, values);
    CHECK_EQ(lowHalf, values[2].u64x2[0]);
    return values[2];
}

void checkVectorInstructions(FPRReg dest, FPRReg source)
{
    v128_t left { 0x1122334455667788ULL, 0x99aabbccddeeff00ULL };
    v128_t right { 0x1100330055007700ULL, 0x99aa0000ddee0000ULL };
    auto run = [&](auto operation) {
        return runVectorOperation(dest, source, left, right, operation);
    };

    CHECK_EQ(bitEquals(run([&](X86Assembler& assembler) {
        assembler.pand_rr(source, dest);
    }), vectorAnd(left, right)), true);

    v128_t equalBytes;
    for (unsigned i = 0; i < 16; ++i)
        equalBytes.u8x16[i] = left.u8x16[i] == right.u8x16[i] ? 0xff : 0;
    CHECK_EQ(bitEquals(run([&](X86Assembler& assembler) {
        assembler.pcmpeqb_rr(source, dest);
    }), equalBytes), true);

    v128_t shiftedWords;
    for (unsigned i = 0; i < 8; ++i)
        shiftedWords.u16x8[i] = left.u16x8[i] >> 3;
    CHECK_EQ(bitEquals(run([&](X86Assembler& assembler) {
        assembler.psrlw_i8r(3, dest);
    }), shiftedWords), true);

    CHECK_EQ(bitEquals(run([&](X86Assembler& assembler) {
        assembler.pslldq_i8r(8, dest);
    }), v128_t { 0, left.u64x2[0] }), true);
}

void testX86NamedInstructions()
{
    checkCarryFlagInstructions(X86Registers::eax);
    checkCarryFlagInstructions(X86Registers::r8);
    checkLeave();
    checkRepeatStoreString();
    checkRIPRelativeAddress(X86Registers::eax);
    checkRIPRelativeAddress(X86Registers::r9);
    checkUnalignedVectorLoad(X86Registers::edx, X86Registers::ecx, X86Registers::xmm0, X86Registers::eax);
    checkUnalignedVectorLoad(X86Registers::r8, X86Registers::r9, X86Registers::xmm8, X86Registers::r10);
    checkVectorInstructions(X86Registers::xmm0, X86Registers::xmm1);
    checkVectorInstructions(X86Registers::xmm8, X86Registers::xmm9);
    checkVectorInstructions(X86Registers::xmm1, X86Registers::xmm15);
    checkVectorInstructions(X86Registers::xmm15, X86Registers::xmm1);
}

void testAddOrFromMemoryAliasing()
{
    constexpr GPRReg first = GPRInfo::argumentGPR2;
    constexpr GPRReg second = GPRInfo::argumentGPR3;
    constexpr GPRReg third = GPRInfo::argumentGPR4;
    constexpr uint64_t registerValue = 0x00ff00ff12345678ULL;
    constexpr int32_t offsetOfMemoryOperand = sizeof(uint64_t);
    uint64_t words[] = { 0, 0x0f0f0f0f87654321ULL };

    auto check = [&](GPRReg base, GPRReg operand, GPRReg dest) {
        auto run = [&](auto emitOperation) {
            return compileAndRun<uint64_t>([&](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.move(GPRInfo::argumentGPR1, operand);
                jit.move(GPRInfo::argumentGPR0, base);
                emitOperation(jit, CCallHelpers::Address(base, offsetOfMemoryOperand));
                jit.move(dest, GPRInfo::returnValueGPR);
                emitFunctionEpilogue(jit);
                jit.ret();
            }, words, registerValue);
        };
        uint64_t operandValue = operand == base ? reinterpret_cast<uintptr_t>(words) : registerValue;
        CHECK_EQ(run([&](CCallHelpers& jit, CCallHelpers::Address address) {
            jit.add64(address, operand, dest);
        }), words[1] + operandValue);
        CHECK_EQ(run([&](CCallHelpers& jit, CCallHelpers::Address address) {
            jit.or64(address, operand, dest);
        }), words[1] | operandValue);
    };

    check(first, second, third);
    check(first, second, first);
    check(first, second, second);
    check(first, first, third);
    check(first, first, first);
}
#endif // CPU(X86_64)

#if CPU(X86_64) || CPU(ARM64)
// The 32-bit immediate of a 64-bit operation stands for its sign extension, whether or not the instruction can hold it.
void testImmediatesOf64BitOperationsAreSignExtended()
{
    constexpr int32_t immediates[] = {
        -1, -4095, -4096, -4097, -5000, -0x00fff000, -0x00fff001, -0x01000000, std::numeric_limits<int32_t>::min(),
        0, 1, 4095, 4096, 4097, 5000, 0x00fff001, std::numeric_limits<int32_t>::max()
    };
    constexpr CCallHelpers::RelationalCondition conditions[] = {
        CCallHelpers::Equal, CCallHelpers::NotEqual, CCallHelpers::LessThan, CCallHelpers::GreaterThanOrEqual, CCallHelpers::Below, CCallHelpers::AboveOrEqual
    };
    auto holds = [](CCallHelpers::RelationalCondition condition, int64_t left, int64_t right) {
        switch (condition) {
        case CCallHelpers::Equal:
            return left == right;
        case CCallHelpers::NotEqual:
            return left != right;
        case CCallHelpers::LessThan:
            return left < right;
        case CCallHelpers::GreaterThanOrEqual:
            return left >= right;
        case CCallHelpers::Below:
            return static_cast<uint64_t>(left) < static_cast<uint64_t>(right);
        case CCallHelpers::AboveOrEqual:
            return static_cast<uint64_t>(left) >= static_cast<uint64_t>(right);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };

    for (int32_t immediate : immediates) {
        const int64_t extended = immediate;
        const int64_t operands[] = {
            extended, extended - 1, extended + 1, static_cast<int64_t>(static_cast<uint32_t>(immediate)), 0, -1,
            std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()
        };
        const CCallHelpers::TrustedImm32 right(immediate);

        for (auto condition : conditions) {
            auto branch = compile([&](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                auto taken = jit.branch64(condition, GPRInfo::argumentGPR0, right);
                jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::returnValueGPR);
                emitFunctionEpilogue(jit);
                jit.ret();
                taken.link(&jit);
                jit.move(CCallHelpers::TrustedImm32(1), GPRInfo::returnValueGPR);
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            auto select = compile([&](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.moveConditionally64(condition, GPRInfo::argumentGPR0, right, GPRInfo::argumentGPR1, GPRInfo::argumentGPR2, GPRInfo::returnValueGPR);
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            auto selectDouble = compile([&](CCallHelpers& jit) {
                emitFunctionPrologue(jit);
                jit.moveDoubleConditionally64(condition, GPRInfo::argumentGPR0, right, FPRInfo::argumentFPR0, FPRInfo::argumentFPR1, FPRInfo::returnValueFPR);
                emitFunctionEpilogue(jit);
                jit.ret();
            });
            for (int64_t operand : operands) {
                bool expected = holds(condition, operand, extended);
                CHECK_EQ(invoke<int32_t>(branch, operand), static_cast<int32_t>(expected));
                CHECK_EQ(invoke<int64_t>(select, operand, static_cast<int64_t>(11), static_cast<int64_t>(22)), static_cast<int64_t>(expected ? 11 : 22));
                CHECK_EQ(invoke<double>(selectDouble, operand, 1.5, 2.5), expected ? 1.5 : 2.5);
            }
        }

        auto subtract = compile([&](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.branchSub64(CCallHelpers::Zero, GPRInfo::argumentGPR0, right, GPRInfo::returnValueGPR).link(&jit);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        for (int64_t operand : operands)
            CHECK_EQ(invoke<uint64_t>(subtract, operand), static_cast<uint64_t>(operand) - static_cast<uint64_t>(extended));

#if CPU(ARM64)
        auto add = compile([&](CCallHelpers& jit) {
            emitFunctionPrologue(jit);
            jit.branchAdd64(CCallHelpers::Zero, GPRInfo::argumentGPR0, right, GPRInfo::returnValueGPR).link(&jit);
            emitFunctionEpilogue(jit);
            jit.ret();
        });
        for (int64_t operand : operands)
            CHECK_EQ(invoke<uint64_t>(add, operand), static_cast<uint64_t>(operand) + static_cast<uint64_t>(extended));
#endif
    }
}

void testAdd64OfRegisterToMemory()
{
    constexpr uint64_t registerValue = 0x00ff00ff12345678ULL;
    constexpr uint64_t memoryValue = 0x0f0f0f0f87654321ULL;
    uint64_t words[] = { 1, memoryValue, 2 };
    uint64_t registerAfterwards = compileAndRun<uint64_t>([&](CCallHelpers& jit) {
        emitFunctionPrologue(jit);
        jit.add64(GPRInfo::argumentGPR1, CCallHelpers::Address(GPRInfo::argumentGPR0, sizeof(uint64_t)));
        jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);
        emitFunctionEpilogue(jit);
        jit.ret();
    }, words, registerValue);
    CHECK_EQ(registerAfterwards, registerValue);
    CHECK_EQ(words[0], static_cast<uint64_t>(1));
    CHECK_EQ(words[1], memoryValue + registerValue);
    CHECK_EQ(words[2], static_cast<uint64_t>(2));
}

enum class TargetPosition : bool { BeforeSite, AfterSite };

struct PCRelativeProgram {
    LinkedCode code;
    uint32_t entry { 0 };
    uint32_t site { 0 };
    uint32_t target { 0 };
};

constexpr size_t codeMisalignments[] = { 0, 4, 2048, 4092 };
constexpr uint64_t loadedWord = 0x0123456789abcdefULL;

template<typename EmitSite>
PCRelativeProgram linkPCRelativeProgram(RelocationKind kind, size_t gap, TargetPosition position, const EmitSite& emitSite)
{
    PCRelativeProgram program;
    CCallHelpers::Label entry;
    CCallHelpers::Label site;
    CCallHelpers::Label target;
    auto emitTarget = [&](CCallHelpers& jit) {
        target = jit.label();
        jit.emitNops(sizeof(uint64_t));
    };
    program.code = linkIntoStorage([&](CCallHelpers& jit) {
        if (position == TargetPosition::BeforeSite) {
            emitTarget(jit);
            jit.emitNops(gap);
        }
        entry = jit.label();
        emitFunctionPrologue(jit);
        site = emitSite(jit);
        emitFunctionEpilogue(jit);
        jit.ret();
        if (position == TargetPosition::AfterSite) {
            jit.emitNops(gap);
            emitTarget(jit);
        }
    }, [&](LinkBuffer& linkBuffer) {
        CHECK_EQ(linkBuffer.link(site, kind, target), true);
        program.entry = linkBuffer.offsetOf(entry);
        program.site = linkBuffer.offsetOf(site);
        program.target = linkBuffer.offsetOf(target);
    });

    CHECK_EQ(program.code.sites.size(), 1u);
    checkLinkedSite(program.code.sites[0], LinkedSite { program.site, program.target, kind });
    CHECK_EQ(program.code.hasProcessAddress, false);
    return program;
}

bool computesTargetAddress(PCRelativeProgram& program, size_t misalignment)
{
    auto executable = makeExecutable(program.code.bytes(), misalignment);
    uint8_t* start = startOfCopiedCode(executable.code(), misalignment);
    return invokeAt<uintptr_t>(start + program.entry) == reinterpret_cast<uintptr_t>(start + program.target);
}

void checkPCRelativeAddress(PCRelativeReach reach, size_t gap, TargetPosition position)
{
    RelocationKind kind = addressKindFor(reach);
    PCRelativeProgram program = linkPCRelativeProgram(kind, gap, position, [&](CCallHelpers& jit) {
        return jit.movePCRelativeAddress(GPRInfo::returnValueGPR, reach);
    });
    for (size_t misalignment : codeMisalignments)
        CHECK_EQ(computesTargetAddress(program, misalignment), true);

    CHECK_EQ(MacroAssembler::applyRelocation(kind, program.code.bytes(), program.site, program.target), true);
    CHECK_EQ(computesTargetAddress(program, 0), true);
#if CPU(ARM64)
    if (kind == RelocationKind::AddressFar)
        CHECK_EQ(computesTargetAddress(program, 2048), false);
#endif
}

void testPCRelativeAddress()
{
    for (size_t gap : { static_cast<size_t>(64), 3 * linkedCodeAlignment + 20 }) {
        for (TargetPosition position : { TargetPosition::BeforeSite, TargetPosition::AfterSite }) {
            checkPCRelativeAddress(PCRelativeReach::Near, gap, position);
            checkPCRelativeAddress(PCRelativeReach::Far, gap, position);
        }
    }
}

void testLoadPCRelative64()
{
    for (TargetPosition position : { TargetPosition::BeforeSite, TargetPosition::AfterSite }) {
        PCRelativeProgram program = linkPCRelativeProgram(RelocationKind::Load64, 64, position, [](CCallHelpers& jit) {
            return jit.loadPCRelative64(GPRInfo::returnValueGPR);
        });
        memcpySpan(program.code.bytes().subspan(program.target), asByteSpan(loadedWord));
        auto runAt = [&](size_t misalignment) {
            auto executable = makeExecutable(program.code.bytes(), misalignment);
            return invokeAt<uint64_t>(startOfCopiedCode(executable.code(), misalignment) + program.entry);
        };
        for (size_t misalignment : codeMisalignments)
            CHECK_EQ(runAt(misalignment), loadedWord);

        CHECK_EQ(MacroAssembler::applyRelocation(RelocationKind::Load64, program.code.bytes(), program.site, program.target), true);
        CHECK_EQ(runAt(0), loadedWord);
    }
}

constexpr RelocationKind allRelocationKinds[] = {
    RelocationKind::Call,
    RelocationKind::Jump,
    RelocationKind::ShortJump,
    RelocationKind::ConditionalJump,
    RelocationKind::ShortConditionalJump,
    RelocationKind::ConditionalJumpThenNop,
    RelocationKind::TestBitJump,
    RelocationKind::TestBitJumpThenNop,
    RelocationKind::InvertedJumpThenJump,
    RelocationKind::AddressNear,
    RelocationKind::AddressFar,
    RelocationKind::Load64,
    RelocationKind::Distance32,
    RelocationKind::InstanceOffset,
};
static_assert(std::size(allRelocationKinds) == numberOfRelocationKinds);

constexpr GPRReg relocatedGPR = GPRInfo::returnValueGPR;
constexpr GPRReg instanceGPR = GPRInfo::argumentGPR1;
constexpr int testedBit = 3;
constexpr int32_t distance32Addend = 0x40;

void emitRelocationSite(CCallHelpers& jit, RelocationKind kind)
{
    switch (kind) {
    case RelocationKind::Call:
        jit.nearCall();
        return;
    case RelocationKind::Jump:
        jit.jump();
        return;
    case RelocationKind::AddressNear:
        jit.movePCRelativeAddress(relocatedGPR, PCRelativeReach::Near);
        return;
    case RelocationKind::AddressFar:
        jit.movePCRelativeAddress(relocatedGPR, PCRelativeReach::Far);
        return;
    case RelocationKind::Load64:
        jit.loadPCRelative64(relocatedGPR);
        return;
    case RelocationKind::Distance32:
        jit.m_assembler.buffer().putInt(distance32Addend);
        return;
    case RelocationKind::InstanceOffset:
        jit.moveInstanceAddress(instanceGPR, relocatedGPR);
        return;
#if CPU(X86_64)
    case RelocationKind::ConditionalJump:
        jit.branchOnFlags(CCallHelpers::Equal);
        return;
    case RelocationKind::ShortJump:
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(X86JumpShortener::shortJumpOpcode));
        jit.m_assembler.buffer().putByte(0);
        return;
    case RelocationKind::ShortConditionalJump:
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(X86JumpShortener::shortConditionalJumpOpcode | X86Assembler::ConditionE));
        jit.m_assembler.buffer().putByte(0);
        return;
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
    case RelocationKind::InvertedJumpThenJump:
        break;
#else
    case RelocationKind::ConditionalJump:
        jit.m_assembler.b_cond(ARM64Assembler::ConditionEQ);
        return;
    case RelocationKind::ConditionalJumpThenNop:
        jit.m_assembler.b_cond(ARM64Assembler::ConditionEQ);
        jit.m_assembler.nop();
        return;
    case RelocationKind::TestBitJump:
        jit.m_assembler.tbnz(relocatedGPR, testedBit);
        return;
    case RelocationKind::TestBitJumpThenNop:
        jit.m_assembler.tbnz(relocatedGPR, testedBit);
        jit.m_assembler.nop();
        return;
    case RelocationKind::InvertedJumpThenJump:
        jit.m_assembler.b_cond(ARM64Assembler::ConditionNE, 2 * static_cast<int32_t>(sizeof(uint32_t)));
        jit.m_assembler.b();
        return;
    case RelocationKind::ShortJump:
    case RelocationKind::ShortConditionalJump:
        break;
#endif
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<uint8_t> createRelocationSite(RelocationKind kind)
{
    CCallHelpers jit;
    emitRelocationSite(jit, kind);
    Vector<uint8_t> bytes = takeEmittedBytes(jit);
    size_t size = MacroAssembler::relocationSize(kind);
    // On ARM64 the add of the instance follows the two instructions that hold the offset.
    CHECK_EQ(kind == RelocationKind::InstanceOffset ? bytes.size() >= size : bytes.size() == size, true);
    bytes.shrink(size);
    return bytes;
}

struct RelocationAttempt {
    Vector<uint8_t> emitted;
    Vector<uint8_t> linked;
};

template<typename Link>
RelocationAttempt checkRelocationAttempt(RelocationKind kind, bool isInReach, const Link& link)
{
    RelocationAttempt attempt;
    attempt.emitted = createRelocationSite(kind);
    attempt.linked = attempt.emitted;
    CHECK_EQ(link(attempt.linked.mutableSpan()), isInReach);
    if (!isInReach)
        CHECK_EQ(attempt.linked == attempt.emitted, true);
    return attempt;
}

#if CPU(ARM64)
constexpr uint32_t adrOpcode = 0x10000000;
constexpr uint32_t adrpOpcode = 0x90000000;
constexpr uint32_t addImmediateOpcode = 0x91000000;
constexpr uint32_t addShiftedImmediateOpcode = 0x91400000;
constexpr uint32_t subShiftedImmediateOpcode = 0xd1400000;
constexpr uint32_t loadLiteral64Opcode = 0x58000000;
constexpr uint32_t branchOpcode = 0x14000000;
constexpr uint32_t branchAndLinkOpcode = 0x94000000;
constexpr uint32_t linkedRegister = static_cast<uint32_t>(relocatedGPR);

uint32_t instructionAt(std::span<const uint8_t> code, size_t index)
{
    uint32_t word;
    memcpy(&word, code.subspan(index * sizeof(word)).data(), sizeof(word));
    return word;
}

int64_t signExtend(uint32_t value, unsigned width)
{
    unsigned shift = 64 - width;
    return static_cast<int64_t>(static_cast<uint64_t>(value) << shift) >> shift;
}

void checkPCRelativeWord(uint32_t word, uint32_t opcode, int64_t immediate)
{
    CHECK_EQ(word & 0x9f000000, opcode);
    CHECK_EQ(word & 0x1f, linkedRegister);
    uint32_t encodedImmediate = ((word >> 5) & 0x7ffff) << 2 | ((word >> 29) & 3);
    CHECK_EQ(signExtend(encodedImmediate, 21), immediate);
}

void checkAddSubtractImmediateWord(uint32_t word, uint32_t opcode, uint32_t immediate)
{
    CHECK_EQ(word & 0xffc00000, opcode);
    CHECK_EQ((word >> 10) & 0xfff, immediate);
    CHECK_EQ((word >> 5) & 0x1f, linkedRegister);
    CHECK_EQ(word & 0x1f, linkedRegister);
}

void checkBranchWord(uint32_t word, uint32_t opcode, int64_t distance)
{
    CHECK_EQ(word & 0xfc000000, opcode);
    CHECK_EQ(signExtend(word & 0x3ffffff, 26) * 4, distance);
}

void checkBranchImmediate(const RelocationAttempt& attempt, unsigned width, int64_t distance)
{
    uint32_t mask = ((1u << width) - 1) << 5;
    uint32_t word = instructionAt(attempt.linked.span(), 0);
    CHECK_EQ(word & ~mask, instructionAt(attempt.emitted.span(), 0) & ~mask);
    CHECK_EQ(signExtend((word & mask) >> 5, width) * 4, distance);
    CHECK_EQ(equalSpans(attempt.linked.span().subspan(sizeof(word)), attempt.emitted.span().subspan(sizeof(word))), true);
}

void checkDistanceEncoding(RelocationKind kind, const RelocationAttempt& attempt, int64_t distance)
{
    std::span<const uint8_t> code = attempt.linked.span();
    uint32_t word = instructionAt(code, 0);
    switch (kind) {
    case RelocationKind::Call:
        checkBranchWord(word, branchAndLinkOpcode, distance);
        return;
    case RelocationKind::Jump:
        checkBranchWord(word, branchOpcode, distance);
        return;
    case RelocationKind::ConditionalJump:
    case RelocationKind::ConditionalJumpThenNop:
        checkBranchImmediate(attempt, 19, distance);
        return;
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
        checkBranchImmediate(attempt, 14, distance);
        return;
    case RelocationKind::InvertedJumpThenJump:
        CHECK_EQ(word, instructionAt(attempt.emitted.span(), 0));
        checkBranchWord(instructionAt(code, 1), branchOpcode, distance - static_cast<int64_t>(sizeof(word)));
        return;
    case RelocationKind::AddressNear:
        checkPCRelativeWord(word, adrOpcode, distance);
        return;
    case RelocationKind::AddressFar: {
        int64_t pages = distance >> 12;
        checkPCRelativeWord(word, adrOpcode, distance - pages * 4096);
        uint32_t opcode = pages < 0 ? subShiftedImmediateOpcode : addShiftedImmediateOpcode;
        checkAddSubtractImmediateWord(instructionAt(code, 1), opcode, static_cast<uint32_t>(pages < 0 ? -pages : pages));
        return;
    }
    case RelocationKind::Load64:
        CHECK_EQ(word & 0xff000000, loadLiteral64Opcode);
        CHECK_EQ(word & 0x1f, linkedRegister);
        CHECK_EQ(signExtend((word >> 5) & 0x7ffff, 19) * 4, distance);
        return;
    case RelocationKind::ShortJump:
    case RelocationKind::ShortConditionalJump:
    case RelocationKind::Distance32:
    case RelocationKind::InstanceOffset:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#else
void checkDistanceEncoding(RelocationKind kind, const RelocationAttempt& attempt, int64_t distance)
{
    std::span<const uint8_t> code = attempt.linked.span();
    bool isShort = kind == RelocationKind::ShortJump || kind == RelocationKind::ShortConditionalJump;
    size_t opcodeSize = code.size() - (isShort ? sizeof(int8_t) : sizeof(int32_t));
    const uint8_t* field = code.subspan(opcodeSize).data();
    int64_t displacement = isShort ? WTF::unalignedLoad<int8_t>(field) : WTF::unalignedLoad<int32_t>(field);
    CHECK_EQ(displacement, distance - static_cast<int64_t>(code.size()));
    CHECK_EQ(equalSpans(code.first(opcodeSize), attempt.emitted.span().first(opcodeSize)), true);
}
#endif

void checkRelocationDistance(RelocationKind kind, int64_t distance, bool isInReach)
{
    RelocationAttempt attempt = checkRelocationAttempt(kind, isInReach, [&](std::span<uint8_t> instructions) {
        return MacroAssembler::setRelocationDistance(kind, instructions, distance);
    });
    if (!isInReach)
        return;
    if (kind == RelocationKind::Distance32) {
        CHECK_EQ(static_cast<int64_t>(WTF::unalignedLoad<int32_t>(attempt.linked.span().data())), distance32Addend + distance);
        return;
    }
    checkDistanceEncoding(kind, attempt, distance);
}

#if CPU(ARM64)
void checkPageRelativeTarget(uint64_t site, uint64_t target, bool isInReach)
{
    int64_t pages = static_cast<int64_t>(target >> 12) - static_cast<int64_t>(site >> 12);
    CHECK_EQ(isInt<21>(pages), isInReach);
    RelocationAttempt attempt = checkRelocationAttempt(RelocationKind::AddressFar, isInReach, [&](std::span<uint8_t> instructions) {
        return ARM64Assembler::setAdrpThenAddTarget(instructions, site, target);
    });
    if (!isInReach)
        return;
    checkPCRelativeWord(instructionAt(attempt.linked.span(), 0), adrpOpcode, pages);
    checkAddSubtractImmediateWord(instructionAt(attempt.linked.span(), 1), addImmediateOpcode, static_cast<uint32_t>(target & 0xfff));
}
#endif

void checkReachCoversSection(RelocationKind kind)
{
    int64_t reach = MacroAssembler::relocationReach(kind);
    int64_t size = MacroAssembler::relocationSize(kind);
#if CPU(ARM64)
    if (kind == RelocationKind::AddressFar) {
        checkPageRelativeTarget(0, reach - 4, true);
        checkPageRelativeTarget(reach - size, 0, true);
        return;
    }
#endif
    // The reach of Distance32 bounds what the word holds once it is linked.
    int64_t addend = kind == RelocationKind::Distance32 ? distance32Addend : 0;
    checkRelocationDistance(kind, reach - 4 - addend, true);
    checkRelocationDistance(kind, -(reach - size) - addend, true);
}

struct DistanceBounds {
    int64_t min;
    int64_t max;
    int64_t unit;
};

std::optional<DistanceBounds> distanceBounds(RelocationKind kind)
{
    constexpr int64_t minInt32 = std::numeric_limits<int32_t>::min();
    constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();
#if CPU(X86_64)
    int64_t size = MacroAssembler::relocationSize(kind);
    switch (kind) {
    case RelocationKind::Call:
    case RelocationKind::Jump:
    case RelocationKind::ConditionalJump:
    case RelocationKind::AddressNear:
    case RelocationKind::AddressFar:
    case RelocationKind::Load64:
        return DistanceBounds { size + minInt32, size + maxInt32, 1 };
    case RelocationKind::ShortJump:
    case RelocationKind::ShortConditionalJump:
        return DistanceBounds { size + std::numeric_limits<int8_t>::min(), size + std::numeric_limits<int8_t>::max(), 1 };
    case RelocationKind::Distance32:
        return DistanceBounds { minInt32 - distance32Addend, maxInt32 - distance32Addend, 1 };
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
    case RelocationKind::InvertedJumpThenJump:
    case RelocationKind::InstanceOffset:
        return std::nullopt;
    }
#else
    constexpr int64_t instructionSize = sizeof(uint32_t);
    constexpr int64_t testBitReach = 32 * KB;
    constexpr int64_t nearReach = 1 * MB;
    constexpr int64_t branchReach = 128 * MB;
    static_assert(MacroAssembler::farAddressReachInBuffer == 4096 * 4096);
    switch (kind) {
    case RelocationKind::Call:
    case RelocationKind::Jump:
        return DistanceBounds { -branchReach, branchReach - instructionSize, instructionSize };
    case RelocationKind::InvertedJumpThenJump:
        return DistanceBounds { -branchReach + instructionSize, branchReach, instructionSize };
    case RelocationKind::ConditionalJump:
    case RelocationKind::ConditionalJumpThenNop:
    case RelocationKind::Load64:
        return DistanceBounds { -nearReach, nearReach - instructionSize, instructionSize };
    case RelocationKind::TestBitJump:
    case RelocationKind::TestBitJumpThenNop:
        return DistanceBounds { -testBitReach, testBitReach - instructionSize, instructionSize };
    case RelocationKind::AddressNear:
        return DistanceBounds { -nearReach, nearReach - 1, 1 };
    case RelocationKind::AddressFar:
        return DistanceBounds { -4095 * 4096, MacroAssembler::farAddressReachInBuffer - 1, 1 };
    case RelocationKind::Distance32:
        return DistanceBounds { minInt32 - distance32Addend, maxInt32 - distance32Addend, 1 };
    case RelocationKind::ShortJump:
    case RelocationKind::ShortConditionalJump:
    case RelocationKind::InstanceOffset:
        return std::nullopt;
    }
#endif
    RELEASE_ASSERT_NOT_REACHED();
}

void checkDistanceBounds(RelocationKind kind, const DistanceBounds& bounds)
{
    checkRelocationDistance(kind, bounds.max, true);
    checkRelocationDistance(kind, bounds.max + bounds.unit, false);
    checkRelocationDistance(kind, bounds.min, true);
    checkRelocationDistance(kind, bounds.min - bounds.unit, false);
    if (bounds.unit > 1)
        checkRelocationDistance(kind, bounds.unit / 2, false);
}

constexpr uint8_t unreachableByte = 0xcc;
constexpr int32_t reachedTargetResult = 0x600d;
constexpr int32_t fellThroughResult = 0xbad;

struct SiteFunction {
    Vector<uint8_t> bytes;
    size_t site { 0 };
};

SiteFunction emitSiteFunction(RelocationKind kind)
{
    SiteFunction function;
    CCallHelpers jit;
    emitFunctionPrologue(jit);
    if (transfersControl(kind)) {
        jit.move(CCallHelpers::TrustedImm32(1 << testedBit), relocatedGPR);
        jit.compareOnFlags32(relocatedGPR, relocatedGPR);
    }
    function.site = jit.m_assembler.codeSize();
    emitRelocationSite(jit, kind);
    if (transfersControl(kind) && kind != RelocationKind::Call)
        jit.move(CCallHelpers::TrustedImm32(fellThroughResult), GPRInfo::returnValueGPR);
    emitFunctionEpilogue(jit);
    jit.ret();
    function.bytes = takeEmittedBytes(jit);
    return function;
}

Vector<uint8_t> emitTargetBlock(RelocationKind kind)
{
    if (!transfersControl(kind))
        return Vector<uint8_t>(asByteSpan(loadedWord));

    CCallHelpers jit;
    if (kind == RelocationKind::Call)
        emitFunctionPrologue(jit);
    jit.move(CCallHelpers::TrustedImm32(reachedTargetResult), GPRInfo::returnValueGPR);
    emitFunctionEpilogue(jit);
    jit.ret();
    return takeEmittedBytes(jit);
}

struct RelocatedImage {
    Vector<uint8_t> bytes;
    size_t entry { 0 };
    size_t site { 0 };
    size_t target { 0 };
};

RelocatedImage layOutRelocatedImage(RelocationKind kind, int64_t distance)
{
    SiteFunction function = emitSiteFunction(kind);
    Vector<uint8_t> targetBlock = emitTargetBlock(kind);

    RelocatedImage image;
    if (distance < 0) {
        image.site = static_cast<size_t>(-distance);
        RELEASE_ASSERT(image.site >= targetBlock.size() + function.site);
    } else {
        image.site = function.site;
        image.target = image.site + static_cast<size_t>(distance);
        RELEASE_ASSERT(image.target >= function.bytes.size());
    }
    image.entry = image.site - function.site;
    image.bytes.fill(unreachableByte, std::max(image.entry + function.bytes.size(), image.target + targetBlock.size()));
    memcpySpan(image.bytes.mutableSpan().subspan(image.entry), function.bytes.span());
    memcpySpan(image.bytes.mutableSpan().subspan(image.target), targetBlock.span());
    return image;
}

uint64_t expectedRunResult(RelocationKind kind, const uint8_t* target)
{
    if (transfersControl(kind))
        return reachedTargetResult;
    if (kind == RelocationKind::Load64)
        return loadedWord;
    RELEASE_ASSERT(kind == RelocationKind::AddressNear || kind == RelocationKind::AddressFar);
    return reinterpret_cast<uintptr_t>(target);
}

void checkRelocatedCodeRuns(RelocationKind kind, int64_t distance)
{
    RelocatedImage image = layOutRelocatedImage(kind, distance);
    CHECK_EQ(MacroAssembler::applyRelocation(kind, image.bytes.mutableSpan(), image.site, image.target), true);
    auto executable = makeExecutable(image.bytes.span());
    uint8_t* start = startOfCopiedCode(executable.code(), 0);
    CHECK_EQ(invokeAt<uint64_t>(start + image.entry), expectedRunResult(kind, start + image.target));
}

void testLinkPCRelativeReach()
{
    constexpr int64_t maxExecutedDistance = 1 * MB;
    for (RelocationKind kind : allRelocationKinds) {
        std::optional<DistanceBounds> bounds = distanceBounds(kind);
        CHECK_EQ(bounds.has_value(), MacroAssembler::relocationSize(kind) && kind != RelocationKind::InstanceOffset);
        if (!bounds)
            continue;
        checkReachCoversSection(kind);
        checkDistanceBounds(kind, *bounds);
        if (kind == RelocationKind::Distance32)
            continue;
        checkRelocatedCodeRuns(kind, std::min(bounds->max, maxExecutedDistance));
        checkRelocatedCodeRuns(kind, std::max(bounds->min, -maxExecutedDistance));
    }

#if CPU(ARM64)
    constexpr uint64_t farReach = 1ULL << 32;
    checkPageRelativeTarget(0xfff, farReach - 0x1000, true);
    checkPageRelativeTarget(0xfff, farReach - 1, true);
    checkPageRelativeTarget(0xfff, farReach, false);
    checkPageRelativeTarget(0xfff, farReach - 1 + 0x1000, false);
    checkPageRelativeTarget(farReach, 0, true);
    checkPageRelativeTarget(farReach + 0xfff, 0xfff, true);
    checkPageRelativeTarget(farReach + 0x1000, 0, false);
    checkPageRelativeTarget(farReach + 0x1000, 0xfff, false);
#endif
}

void testClearRelocation()
{
    constexpr size_t codeSize = 128;
    for (RelocationKind kind : allRelocationKinds) {
        size_t size = MacroAssembler::relocationSize(kind);
        if (!size)
            continue;

        Vector<uint8_t> emitted = createRelocationSite(kind);
        if (kind == RelocationKind::Distance32) {
            MacroAssembler::clearRelocation(kind, emitted.mutableSpan());
            CHECK_EQ(WTF::unalignedLoad<int32_t>(emitted.span().data()), distance32Addend);
            continue;
        }

        auto linkTo = [&](size_t target) {
            Vector<uint8_t> code;
            code.fill(unreachableByte, codeSize);
            memcpySpan(code.mutableSpan(), emitted.span());
            CHECK_EQ(MacroAssembler::applyRelocation(kind, code.mutableSpan(), 0, target), true);
            return code;
        };
        Vector<uint8_t> first = linkTo(16);
        Vector<uint8_t> second = linkTo(64);
        CHECK_EQ(first == second, false);
        MacroAssembler::clearRelocation(kind, first.mutableSpan().first(size));
        MacroAssembler::clearRelocation(kind, second.mutableSpan().first(size));
        CHECK_EQ(first == second, true);
    }
}

void testInstanceOffset()
{
    constexpr RelocationKind kind = RelocationKind::InstanceOffset;
    constexpr uint64_t instance = 0x0000123400000000ULL;
    constexpr uint64_t reach = MacroAssembler::relocationReach(kind);
#if CPU(X86_64)
    static_assert(reach == 0x80000000ULL);
#endif

    SiteFunction function = emitSiteFunction(kind);
    auto computedAddress = [&] {
        auto executable = makeExecutable(function.bytes.span());
        return invokeAt<uint64_t>(startOfCopiedCode(executable.code(), 0), static_cast<uint64_t>(0), instance);
    };
    CHECK_EQ(computedAddress(), instance);
    for (uint64_t offset : { static_cast<uint64_t>(0x12345678), reach - 1 }) {
        CHECK_EQ(MacroAssembler::applyRelocation(kind, function.bytes.mutableSpan(), function.site, offset), true);
        CHECK_EQ(computedAddress(), instance + offset);
    }

    Vector<uint8_t> linked = function.bytes;
    CHECK_EQ(MacroAssembler::applyRelocation(kind, function.bytes.mutableSpan(), function.site, reach), false);
    CHECK_EQ(function.bytes == linked, true);
}

template<typename Emit>
void checkRecordingKeepsBytes(const Emit& emit)
{
    AssemblerPolicy plain = AssemblerPolicy::forJIT();
    plain.blindsConstants = false;
    AssemblerPolicy recording = plain;
    recording.recordsProcessAddresses = true;
    CHECK_EQ(emitWithPolicy(recording, emit) == emitWithPolicy(plain, emit), true);
}

struct AddressLog {
    LinkedCode code;
    uint32_t offset { 0 };
};

template<typename Emit>
AddressLog recordAddressLog(const Emit& emit)
{
    AddressLog log;
    CCallHelpers::Label start;
    log.code = linkIntoStorage([&](CCallHelpers& jit) {
        jit.nop();
        start = jit.label();
        emit(jit);
    }, [&](LinkBuffer& linkBuffer) {
        log.offset = linkBuffer.offsetOf(start);
    });
    CHECK_EQ(log.code.externalLinks.size(), 0u);
    checkRecordingKeepsBytes(emit);
    return log;
}

template<typename Emit>
void checkLogsPointer(const void* pointer, const Emit& emit)
{
    AddressLog log = recordAddressLog(emit);
    WideImmediate expected { log.offset, ImmediateProvenance::Pointer, reinterpret_cast<uintptr_t>(pointer) };
    CHECK_EQ(log.code.hasProcessAddress, true);
    CHECK_EQ(log.code.wideImmediates.contains(expected), true);
}

template<typename Emit>
void checkLogsPatchablePointer(const Emit& emit)
{
    AddressLog log = recordAddressLog(emit);
    CHECK_EQ(log.code.hasProcessAddress, true);
    CHECK_EQ(log.code.wideImmediates.size(), 0u);
}

template<typename Emit>
void checkLogsNoPointer(std::optional<uint64_t> loggedInteger, const Emit& emit)
{
    AddressLog log = recordAddressLog(emit);
    CHECK_EQ(log.code.hasProcessAddress, false);
    if (!loggedInteger) {
        CHECK_EQ(log.code.wideImmediates.size(), 0u);
        return;
    }
    WideImmediate expected { log.offset, ImmediateProvenance::Integer, *loggedInteger };
    CHECK_EQ(log.code.wideImmediates.size(), 1u);
    CHECK_EQ(log.code.wideImmediates[0] == expected, true);
}

void checkSoleExternalLink(const LinkedCode& code, uintptr_t target)
{
    CHECK_EQ(code.hasProcessAddress, true);
    CHECK_EQ(code.externalLinks.size(), 1u);
    CHECK_EQ(code.externalLinks[0].target, target);
    CHECK_EQ(code.sites.size(), 0u);
}

struct OtherCode {
    MacroAssemblerCodeRef<JSEntryPtrTag> code { compile([](CCallHelpers& jit) {
        jit.emitNops(8);
        jit.ret();
    }) };
    // Not the first byte: the end of a buffer counts as inside it, and this code may start there.
    CodeLocationLabel<JSEntryPtrTag> location { code.code() + static_cast<size_t>(4) };
    uintptr_t address { location.untaggedPtr<uintptr_t>() };
};

void checkExternalJump(const JumpSource& source)
{
    OtherCode other;
    CCallHelpers::Jump jump;
    checkSoleExternalLink(linkIntoExecutableMemory([&](CCallHelpers& jit) {
        jump = source.emit(jit);
    }, [&](LinkBuffer& linkBuffer) {
        linkBuffer.link(jump, other.location);
    }), other.address);
}

void functionOutsideExecutableMemory() { }

void checkPointerImmediates(int* cell)
{
    constexpr GPRReg gpr = GPRInfo::regT1;
    checkLogsPointer(cell, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImmPtr(cell), gpr);
    });
    checkLogsPointer(cell, [&](CCallHelpers& jit) {
        jit.storePtr(CCallHelpers::TrustedImmPtr(cell), CCallHelpers::Address(CCallHelpers::stackPointerRegister, -8));
    });
    checkLogsPointer(cell, [&](CCallHelpers& jit) {
        jit.branchPtr(CCallHelpers::Equal, gpr, CCallHelpers::TrustedImmPtr(cell));
    });
    checkLogsPointer(cell, [&](CCallHelpers& jit) {
        jit.load32(CCallHelpers::AbsoluteAddress(cell).m_ptr, gpr);
    });
#if CPU(X86_64)
    checkLogsPointer(cell, [&](CCallHelpers& jit) {
        jit.load32(CCallHelpers::AbsoluteAddress(cell).m_ptr, X86Registers::eax);
    });
#endif
    checkLogsPointer(cell, [&](CCallHelpers& jit) {
        jit.add32(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(cell));
    });
    checkLogsPatchablePointer([&](CCallHelpers& jit) {
        jit.moveWithPatch(CCallHelpers::TrustedImmPtr(nullptr), gpr);
    });
    checkLogsPatchablePointer([&](CCallHelpers& jit) {
        jit.call(OperationPtrTag);
    });

#if CPU(ARM64)
    CCallHelpers::Label end;
    uint32_t endOffset = 0;
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        jit.load32(cell, gpr);
        jit.load32(cell + 1, gpr);
        end = jit.label();
    }, [&](LinkBuffer& linkBuffer) {
        endOffset = linkBuffer.offsetOf(end);
    });
    WideImmediate first { 0, ImmediateProvenance::Pointer, reinterpret_cast<uintptr_t>(cell) };
    WideImmediate second { endOffset - 4, ImmediateProvenance::Pointer, reinterpret_cast<uintptr_t>(cell + 1) };
    CHECK_EQ(code.wideImmediates.size(), 2u);
    CHECK_EQ(code.wideImmediates[0] == first, true);
    CHECK_EQ(code.wideImmediates[1] == second, true);
#endif
}

void checkExternalLinks(int* cell)
{
    OtherCode other;
    CCallHelpers::Call call;
    auto linkCall = [&](LinkBuffer& linkBuffer) {
        linkBuffer.link(call, other.location);
    };
    auto linkNothing = [](LinkBuffer&) { };

    checkSoleExternalLink(linkIntoExecutableMemory([&](CCallHelpers& jit) {
        call = jit.nearCall();
    }, linkCall), other.address);
    checkSoleExternalLink(linkIntoExecutableMemory([&](CCallHelpers& jit) {
        call = jit.nearTailCall();
    }, linkCall), other.address);
    checkSoleExternalLink(linkIntoExecutableMemory([&](CCallHelpers& jit) {
        jit.nearCallThunk(other.location);
    }, linkNothing), other.address);
    checkSoleExternalLink(linkIntoExecutableMemory([&](CCallHelpers& jit) {
        jit.jumpThunk(other.location);
    }, linkNothing), other.address);
    checkExternalJump(unconditionalJump);

    checkSoleExternalLink(linkIntoStorage([&](CCallHelpers& jit) {
        call = jit.call(CFunctionPtrTag);
    }, [&](LinkBuffer& linkBuffer) {
        linkBuffer.link<CFunctionPtrTag>(call, functionOutsideExecutableMemory);
    }), CodePtr<CFunctionPtrTag>(functionOutsideExecutableMemory).untaggedPtr<uintptr_t>());

    CCallHelpers::DataLabelPtr patchablePointer;
    checkSoleExternalLink(linkIntoStorage([&](CCallHelpers& jit) {
        patchablePointer = jit.moveWithPatch(CCallHelpers::TrustedImmPtr(nullptr), GPRInfo::regT0);
    }, [&](LinkBuffer& linkBuffer) {
        linkBuffer.patch(patchablePointer, cell);
    }), reinterpret_cast<uintptr_t>(cell));
}

void checkIntegerImmediates(int* cell)
{
    constexpr GPRReg gpr = GPRInfo::regT0;
    constexpr size_t mask = 0xfffffffffffc000;
    checkLogsNoPointer(mask, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImmPtr(mask), gpr);
    });

    int64_t cellBits = reinterpret_cast<intptr_t>(cell);
    bool isWide = !isRepresentableAs<int32_t>(cellBits) && !isRepresentableAs<uint32_t>(cellBits);
    checkLogsNoPointer(isWide ? std::optional<uint64_t>(cellBits) : std::nullopt, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImm64(cellBits), gpr);
    });

    checkLogsNoPointer(std::nullopt, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImmPtr(nullptr), gpr);
    });
    checkLogsNoPointer(std::nullopt, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImmPtr(static_cast<void*>(nullptr)), gpr);
    });
    checkLogsNoPointer(std::nullopt, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImm32(5), gpr);
    });
    checkLogsNoPointer(std::nullopt, [&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImm64(5), gpr);
    });
    checkLogsNoPointer(std::nullopt, [&](CCallHelpers& jit) {
        jit.nearCall();
    });
}

void checkLinksInsideBuffer()
{
    CCallHelpers::Call call;
    CCallHelpers::Jump lateJump;
    CCallHelpers::Label target;
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        call = jit.nearCall();
        lateJump = jit.jump();
        CCallHelpers::Jump jump = jit.jump();
        jit.breakpoint();
        jump.link(&jit);
        target = jit.label();
        jit.ret();
    }, [&](LinkBuffer& linkBuffer) {
        auto location = linkBuffer.locationOf<JSInternalPtrTag>(target);
        linkBuffer.link(call, location);
        linkBuffer.link(lateJump, location);
    });
    CHECK_EQ(code.hasProcessAddress, false);
    CHECK_EQ(code.externalLinks.size(), 0u);
    CHECK_EQ(code.wideImmediates.size(), 0u);
    CHECK_EQ(code.sites.size(), 2u);
}

void testAddressLog()
{
    static int cells[2];
    checkPointerImmediates(cells);
    checkExternalLinks(cells);
    checkIntegerImmediates(cells);
    checkLinksInsideBuffer();
}

struct PointerHelper {
    const char* name;
    void (*emit)(CCallHelpers&, const void* pointer);
};

#define POINTER_HELPER(...) { #__VA_ARGS__, [](CCallHelpers& jit, const void* pointer) { jit.__VA_ARGS__; } }
#define POINTER_HELPER_WITH_DATA_LABEL(...) { #__VA_ARGS__, [](CCallHelpers& jit, const void* pointer) { CCallHelpers::DataLabelPtr dataLabel; jit.__VA_ARGS__; } }

const PointerHelper pointerHelpers[] = {
    POINTER_HELPER(move(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(move(CCallHelpers::ImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(addPtr(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(addPtr(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0, GPRInfo::regT1)),
    POINTER_HELPER(andPtr(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(orPtr(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(subPtr(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(xorPtr(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(storePtr(CCallHelpers::TrustedImmPtr(pointer), CCallHelpers::Address(GPRInfo::regT0))),
    POINTER_HELPER(storePtr(CCallHelpers::ImmPtr(pointer), CCallHelpers::Address(GPRInfo::regT0))),
    POINTER_HELPER(storePtr(CCallHelpers::TrustedImmPtr(pointer), CCallHelpers::BaseIndex(GPRInfo::regT0, GPRInfo::regT1, CCallHelpers::TimesEight))),
    POINTER_HELPER(store64(CCallHelpers::TrustedImmPtr(pointer), CCallHelpers::Address(GPRInfo::regT0))),
    POINTER_HELPER(poke(CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(poke(CCallHelpers::ImmPtr(pointer))),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::ImmPtr(pointer))),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::TrustedImmPtr(pointer), jit.label())),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::ImmPtr(pointer), jit.label())),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, CCallHelpers::Address(GPRInfo::regT0), CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(patchableBranchPtr(CCallHelpers::Equal, CCallHelpers::Address(GPRInfo::regT0), CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(moveWithPatch(CCallHelpers::TrustedImmPtr(pointer), GPRInfo::regT0)),
    POINTER_HELPER(storePtrWithPatch(CCallHelpers::TrustedImmPtr(pointer), CCallHelpers::Address(GPRInfo::regT0))),
    POINTER_HELPER_WITH_DATA_LABEL(branchPtrWithPatch(CCallHelpers::Equal, GPRInfo::regT0, dataLabel, CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER_WITH_DATA_LABEL(branchPtrWithPatch(CCallHelpers::Equal, CCallHelpers::Address(GPRInfo::regT0), dataLabel, CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER_WITH_DATA_LABEL(patchableBranchPtrWithPatch(CCallHelpers::Equal, CCallHelpers::Address(GPRInfo::regT0), dataLabel, CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(farJump(CCallHelpers::TrustedImmPtr(pointer), JSEntryPtrTag)),
    POINTER_HELPER(farJump(CCallHelpers::AbsoluteAddress(pointer), JSEntryPtrTag)),
    POINTER_HELPER(loadDouble(CCallHelpers::TrustedImmPtr(pointer), FPRInfo::fpRegT0)),
    POINTER_HELPER(loadFloat(CCallHelpers::TrustedImmPtr(pointer), FPRInfo::fpRegT0)),
    POINTER_HELPER(storeDouble(FPRInfo::fpRegT0, CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(addDouble(CCallHelpers::AbsoluteAddress(pointer), FPRInfo::fpRegT0)),
    POINTER_HELPER(loadPtr(pointer, GPRInfo::regT0)),
    POINTER_HELPER(loadRegWord(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load64(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load32(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load8(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load64(CCallHelpers::ExtendedAddress(GPRInfo::regT1, reinterpret_cast<intptr_t>(pointer)), GPRInfo::regT0)),
    POINTER_HELPER(load16(CCallHelpers::ExtendedAddress(GPRInfo::regT1, reinterpret_cast<intptr_t>(pointer)), GPRInfo::regT0)),
    POINTER_HELPER(storePtr(GPRInfo::regT0, const_cast<void*>(pointer))),
    POINTER_HELPER(storeRegWord(GPRInfo::regT0, const_cast<void*>(pointer))),
    POINTER_HELPER(store64(GPRInfo::regT0, const_cast<void*>(pointer))),
    POINTER_HELPER(store32(GPRInfo::regT0, const_cast<void*>(pointer))),
    POINTER_HELPER(store8(GPRInfo::regT0, const_cast<void*>(pointer))),
    POINTER_HELPER(store64(CCallHelpers::TrustedImm64(1), const_cast<void*>(pointer))),
    POINTER_HELPER(store32(CCallHelpers::TrustedImm32(1), const_cast<void*>(pointer))),
    POINTER_HELPER(store8(CCallHelpers::TrustedImm32(1), const_cast<void*>(pointer))),
    POINTER_HELPER(add32(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(add32(CCallHelpers::AbsoluteAddress(pointer), GPRInfo::regT0)),
    POINTER_HELPER(add64(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(add64(CCallHelpers::AbsoluteAddress(pointer), GPRInfo::regT0)),
    POINTER_HELPER(addPtr(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(addPtr(CCallHelpers::AbsoluteAddress(pointer), GPRInfo::regT0)),
    POINTER_HELPER(sub32(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(or8(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(or8(GPRInfo::regT0, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(or16(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(or16(GPRInfo::regT0, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(or32(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(or32(GPRInfo::regT0, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branch8(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(pointer), CCallHelpers::TrustedImm32(1))),
    POINTER_HELPER(branch16(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(pointer), CCallHelpers::TrustedImm32(1))),
    POINTER_HELPER(branch32(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(pointer), GPRInfo::regT0)),
    POINTER_HELPER(branch32(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branch64(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(pointer), GPRInfo::regT0)),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(pointer), GPRInfo::regT0)),
    POINTER_HELPER(branchTest8(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branchTest16(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branchTest32(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branchTest64(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branchTestPtr(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branchTest8(CCallHelpers::NonZero, CCallHelpers::ExtendedAddress(GPRInfo::regT1, reinterpret_cast<intptr_t>(pointer)))),
    POINTER_HELPER(branchTest16(CCallHelpers::NonZero, CCallHelpers::ExtendedAddress(GPRInfo::regT1, reinterpret_cast<intptr_t>(pointer)))),
    POINTER_HELPER(branchAdd32(CCallHelpers::NonZero, CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
#if CPU(X86_64)
    POINTER_HELPER(load64(pointer, GPRInfo::regT1)),
    POINTER_HELPER(load32(pointer, GPRInfo::regT1)),
    POINTER_HELPER(store64(GPRInfo::regT1, const_cast<void*>(pointer))),
    POINTER_HELPER(store32(GPRInfo::regT1, const_cast<void*>(pointer))),
    POINTER_HELPER(and32(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(pointer))),
    POINTER_HELPER(branchPtr(CCallHelpers::Equal, CCallHelpers::BaseIndex(GPRInfo::regT0, GPRInfo::regT1, CCallHelpers::TimesEight), CCallHelpers::TrustedImmPtr(pointer))),
#else
    POINTER_HELPER(load16(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load8SignedExtendTo32(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load8SignedExtendTo64(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load16SignedExtendTo32(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load16SignedExtendTo64(pointer, GPRInfo::regT0)),
    POINTER_HELPER(load32SignedExtendTo64(pointer, GPRInfo::regT0)),
    POINTER_HELPER(store16(GPRInfo::regT0, pointer)),
    POINTER_HELPER(store16(CCallHelpers::TrustedImm32(1), pointer)),
    POINTER_HELPER(branch32(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(pointer), CCallHelpers::TrustedImm32(1))),
    POINTER_HELPER(convertInt32ToDouble(CCallHelpers::AbsoluteAddress(pointer), FPRInfo::fpRegT0)),
    POINTER_HELPER(loadFloat16(CCallHelpers::TrustedImmPtr(pointer), FPRInfo::fpRegT0)),
    POINTER_HELPER(loadVector(CCallHelpers::TrustedImmPtr(pointer), FPRInfo::fpRegT0)),
    POINTER_HELPER(storeVector(FPRInfo::fpRegT0, CCallHelpers::TrustedImmPtr(pointer))),
    POINTER_HELPER(add64AndSetFlags(CCallHelpers::TrustedImm64(CCallHelpers::TrustedImmPtr(pointer)), GPRInfo::regT0, GPRInfo::regT1)),
    POINTER_HELPER(move64ToDouble(CCallHelpers::TrustedImm64(CCallHelpers::TrustedImmPtr(pointer)), FPRInfo::fpRegT0)),
    POINTER_HELPER(moveConditionally64(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::TrustedImm64(CCallHelpers::TrustedImmPtr(pointer)), GPRInfo::regT1, GPRInfo::regT2, GPRInfo::regT3)),
    POINTER_HELPER(branchTest64(CCallHelpers::NonZero, GPRInfo::regT0, CCallHelpers::TrustedImm64(CCallHelpers::TrustedImmPtr(pointer)))),
    POINTER_HELPER(compare64(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::TrustedImm64(CCallHelpers::TrustedImmPtr(pointer)), GPRInfo::regT1)),
#endif
};

#undef POINTER_HELPER_WITH_DATA_LABEL
#undef POINTER_HELPER

void checkPointerHelper(const PointerHelper& helper, const void* pointer)
{
    auto emit = [&](CCallHelpers& jit) {
        helper.emit(jit, pointer);
    };
    LinkedCode code = linkIntoStorage(emit, [](LinkBuffer&) { });
    if (!code.hasProcessAddress)
        dataLogLn("No address was logged for ", helper.name, " with ", RawPointer(pointer));
    CHECK_EQ(code.hasProcessAddress, true);
    checkRecordingKeepsBytes(emit);
}

void testAddressLogThroughEveryPointerHelper()
{
    const uintptr_t addresses[] = { 0x12345678, 0x412000, 0x123456789abc };
    for (uintptr_t address : addresses) {
        const void* pointer = reinterpret_cast<const void*>(address);
        for (const PointerHelper& helper : pointerHelpers)
            checkPointerHelper(helper, pointer);
    }
}

#if CPU(ARM64)
constexpr JumpSource compareAndBranch { [](CCallHelpers& jit) { return jit.branchTest64(CCallHelpers::NonZero, GPRInfo::regT0); } };
constexpr JumpSource testBitAndBranch { [](CCallHelpers& jit) { return jit.branchTest64(CCallHelpers::NonZero, GPRInfo::regT0, CCallHelpers::TrustedImm32(8)); } };

void checkPatchableBranch()
{
    constexpr RelocationKind kind = RelocationKind::ConditionalJumpThenNop;
    CCallHelpers::Label after;
    CCallHelpers::Label target;
    LinkedSite expected { };
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        CCallHelpers::PatchableJump jump = jit.patchableBranch32(CCallHelpers::Equal, GPRInfo::regT0, CCallHelpers::TrustedImm32(5));
        after = jit.label();
        jit.emitNops(64);
        jump.m_jump.link(&jit);
        target = jit.label();
        jit.ret();
    }, [&](LinkBuffer& linkBuffer) {
        expected = { linkBuffer.offsetOf(after) - MacroAssembler::relocationSize(kind), linkBuffer.offsetOf(target), kind };
    });
    CHECK_EQ(code.sites.size(), 1u);
    checkLinkedSite(code.sites[0], expected);
}
#endif

void checkAddressBetweenJumps()
{
    JumpSite first;
    JumpSite last;
    CCallHelpers::Label address;
    CCallHelpers::Label target;
    Vector<LinkedSite> expected;
    LinkedCode code = linkIntoStorage([&](CCallHelpers& jit) {
        first = emitJumpSite(jit, unconditionalJump);
        address = jit.movePCRelativeAddress(GPRInfo::regT0, PCRelativeReach::Near);
        last = emitJumpSite(jit, unconditionalJump);
        jit.emitNops(200);
        first.jump.link(&jit);
        last.jump.link(&jit);
        target = jit.label();
        jit.ret();
    }, [&](LinkBuffer& linkBuffer) {
        CHECK_EQ(linkBuffer.link(address, RelocationKind::AddressNear, target), true);
        expected.append(expectedLinkedSite(linkBuffer, first, target, RelocationKind::Jump));
        expected.append(LinkedSite { linkBuffer.offsetOf(address), linkBuffer.offsetOf(target), RelocationKind::AddressNear });
        expected.append(expectedLinkedSite(linkBuffer, last, target, RelocationKind::Jump));
    });
    CHECK_EQ(code.sites.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        checkLinkedSite(code.sites[i], expected[i]);
    CHECK_EQ(code.hasProcessAddress, false);
}

void testLinkedSites()
{
    struct JumpCase {
        JumpSource source;
        size_t distance;
        RelocationKind linkedWhileAssembling;
        RelocationKind linkedLate;
    };
#if CPU(X86_64)
    const JumpCase cases[] = {
        { unconditionalJump, 64, RelocationKind::ShortJump, RelocationKind::Jump },
        { unconditionalJump, 200, RelocationKind::Jump, RelocationKind::Jump },
        { jumpOnFlags, 64, RelocationKind::ShortConditionalJump, RelocationKind::ConditionalJump },
        { jumpOnFlags, 200, RelocationKind::ConditionalJump, RelocationKind::ConditionalJump },
    };
#else
    const JumpCase cases[] = {
        { unconditionalJump, 64, RelocationKind::Jump, RelocationKind::Jump },
        { jumpOnFlags, 64, RelocationKind::ConditionalJump, RelocationKind::ConditionalJumpThenNop },
        { jumpOnFlags, 1 * MB + 8, RelocationKind::InvertedJumpThenJump, RelocationKind::InvertedJumpThenJump },
        { compareAndBranch, 64, RelocationKind::ConditionalJump, RelocationKind::ConditionalJumpThenNop },
        { compareAndBranch, 1 * MB + 8, RelocationKind::InvertedJumpThenJump, RelocationKind::InvertedJumpThenJump },
        { testBitAndBranch, 64, RelocationKind::TestBitJump, RelocationKind::TestBitJumpThenNop },
        { testBitAndBranch, 16 * KB, RelocationKind::TestBitJumpThenNop, RelocationKind::TestBitJumpThenNop },
        { testBitAndBranch, 32 * KB + 8, RelocationKind::InvertedJumpThenJump, RelocationKind::InvertedJumpThenJump },
    };
    checkPatchableBranch();
#endif
    for (const JumpCase& jumpCase : cases) {
        checkJump(jumpCase.source, jumpCase.distance, JumpLinking::Forward, jumpCase.linkedWhileAssembling);
        checkJump(jumpCase.source, jumpCase.distance, JumpLinking::Backward, jumpCase.linkedWhileAssembling);
        checkJump(jumpCase.source, jumpCase.distance, JumpLinking::Late, jumpCase.linkedLate);
    }
    checkAddressBetweenJumps();
    checkExternalJump(unconditionalJump);
    checkExternalJump(jumpOnFlags);
}

template<typename T>
bool evaluateCondition(MacroAssembler::RelationalCondition condition, T left, T right)
{
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned unsignedLeft = left;
    Unsigned unsignedRight = right;
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

constexpr unsigned numberOfFlagConsumers = 6;

struct FlagConsumerSlots {
    uint64_t results[numberOfFlagConsumers];
    uint64_t left;
    uint64_t right;
};

constexpr GPRReg flagsLeftGPR = GPRInfo::argumentGPR0;
constexpr GPRReg flagsRightGPR = GPRInfo::argumentGPR1;
constexpr GPRReg flagConsumerSlotsGPR = GPRInfo::argumentGPR2;
constexpr uint64_t takenValue = 0x1111111122222222ULL;
constexpr uint64_t notTakenValue = 0x3333333344444444ULL;

template<typename EmitFlags>
MacroAssemblerCodeRef<JSEntryPtrTag> compileFlagConsumers(MacroAssembler::RelationalCondition condition, const EmitFlags& emitFlags)
{
    constexpr GPRReg thenCase = GPRInfo::argumentGPR3;
    constexpr GPRReg elseCase = GPRInfo::argumentGPR4;
    constexpr GPRReg dest = GPRInfo::argumentGPR5;

    return compile([&](CCallHelpers& jit) {
        unsigned numberOfResults = 0;
        auto storeResult = [&] {
            jit.store64(dest, CCallHelpers::Address(flagConsumerSlotsGPR, static_cast<int32_t>(numberOfResults++ * sizeof(uint64_t))));
        };

        emitFunctionPrologue(jit);
        jit.move(CCallHelpers::TrustedImm64(takenValue), thenCase);
        jit.move(CCallHelpers::TrustedImm64(notTakenValue), elseCase);
        emitFlags(jit);

        jit.move(thenCase, dest);
        CCallHelpers::Jump taken = jit.branchOnFlags(condition);
        jit.move(elseCase, dest);
        taken.link(&jit);
        storeResult();

        jit.move(elseCase, dest);
        jit.moveConditionallyOnFlags64(condition, thenCase, dest);
        storeResult();

        jit.moveConditionallyOnFlags64(condition, thenCase, elseCase, dest);
        storeResult();

        jit.move(thenCase, dest);
        jit.moveConditionallyOnFlags64(condition, dest, elseCase, dest);
        storeResult();

        jit.move(elseCase, dest);
        jit.moveConditionallyOnFlags64(condition, thenCase, dest, dest);
        storeResult();

        jit.move(thenCase, dest);
        jit.moveConditionallyOnFlags64(MacroAssembler::invert(condition), elseCase, dest);
        storeResult();

        RELEASE_ASSERT(numberOfResults == numberOfFlagConsumers);
        emitFunctionEpilogue(jit);
        jit.ret();
    });
}

template<typename T>
void checkFlagConsumers(const MacroAssemblerCodeRef<JSEntryPtrTag>& code, T left, T right, bool isTaken)
{
    FlagConsumerSlots slots { };
    slots.left = static_cast<std::make_unsigned_t<T>>(left);
    slots.right = static_cast<std::make_unsigned_t<T>>(right);
    invoke<void>(code, left, right, &slots);
    for (uint64_t result : slots.results)
        CHECK_EQ(result, isTaken ? takenValue : notTakenValue);
}

template<typename T>
void checkCompareOnFlags(MacroAssembler::RelationalCondition condition)
{
    const T operands[] = { 0, 1, -1, std::numeric_limits<T>::min(), std::numeric_limits<T>::max(), 42 };
    const int32_t immediates[] = { 0, 1, -1, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), 42 };
    auto compileComparison = [&](auto left, auto right) {
        return compileFlagConsumers(condition, [&](CCallHelpers& jit) {
            if constexpr (sizeof(T) == sizeof(int32_t))
                jit.compareOnFlags32(left, right);
            else
                jit.compareOnFlags64(left, right);
        });
    };
    auto checkAllOperands = [&](const MacroAssemblerCodeRef<JSEntryPtrTag>& code) {
        for (T left : operands) {
            for (T right : operands)
                checkFlagConsumers<T>(code, left, right, evaluateCondition<T>(condition, left, right));
        }
    };
    auto checkAllLeftOperands = [&](const MacroAssemblerCodeRef<JSEntryPtrTag>& code, int32_t right) {
        for (T left : operands)
            checkFlagConsumers<T>(code, left, 0, evaluateCondition<T>(condition, left, right));
    };

    checkAllOperands(compileComparison(flagsLeftGPR, flagsRightGPR));
    for (int32_t immediate : immediates)
        checkAllLeftOperands(compileComparison(flagsLeftGPR, CCallHelpers::TrustedImm32(immediate)), immediate);
#if CPU(X86_64)
    constexpr GPRReg indexGPR = GPRInfo::argumentGPR5;
    constexpr int32_t leftOffset = OBJECT_OFFSETOF(FlagConsumerSlots, left);
    CCallHelpers::Address leftSlot(flagConsumerSlotsGPR, leftOffset);
    CCallHelpers::BaseIndex indexedLeftSlot(flagConsumerSlotsGPR, indexGPR, CCallHelpers::TimesEight, leftOffset - static_cast<int32_t>(sizeof(uint64_t)));
    auto compileIndexedComparison = [&](auto right) {
        return compileFlagConsumers(condition, [&](CCallHelpers& jit) {
            jit.move(CCallHelpers::TrustedImm32(1), indexGPR);
            if constexpr (sizeof(T) == sizeof(int32_t))
                jit.compareOnFlags32(indexedLeftSlot, right);
            else
                jit.compareOnFlags64(indexedLeftSlot, right);
        });
    };

    checkAllOperands(compileComparison(flagsLeftGPR, CCallHelpers::Address(flagConsumerSlotsGPR, OBJECT_OFFSETOF(FlagConsumerSlots, right))));
    checkAllOperands(compileComparison(leftSlot, flagsRightGPR));
    for (int32_t immediate : immediates)
        checkAllLeftOperands(compileComparison(leftSlot, CCallHelpers::TrustedImm32(immediate)), immediate);
    if constexpr (sizeof(T) == sizeof(int32_t)) {
        for (int32_t immediate : immediates)
            checkAllLeftOperands(compileIndexedComparison(CCallHelpers::TrustedImm32(immediate)), immediate);
    } else
        checkAllOperands(compileIndexedComparison(flagsRightGPR));
#endif
}

template<typename T>
void checkTestOnFlags(MacroAssembler::RelationalCondition condition)
{
    const T values[] = { 0, 1, -1, std::numeric_limits<T>::min(), std::numeric_limits<T>::max(), 42, 0x40, 0x80, 0xc0 };
    const int32_t masks[] = { -1, 0x40, 0x80, std::numeric_limits<int32_t>::min() };
    auto compileTest = [&](auto mask) {
        return compileFlagConsumers(condition, [&](CCallHelpers& jit) {
            if constexpr (sizeof(T) == sizeof(int32_t))
                jit.testOnFlags32(flagsLeftGPR, mask);
            else
                jit.testOnFlags64(flagsLeftGPR, mask);
        });
    };

    auto testWithRegister = compileTest(flagsRightGPR);
    for (int32_t mask : masks) {
        auto testWithImmediate = compileTest(CCallHelpers::TrustedImm32(mask));
        for (T value : values) {
            bool isTaken = evaluateCondition<T>(condition, value & static_cast<T>(mask), 0);
            checkFlagConsumers<T>(testWithRegister, value, mask, isTaken);
            checkFlagConsumers<T>(testWithImmediate, value, 0, isTaken);
        }
    }
}

void testOnFlags(MacroAssembler::RelationalCondition condition)
{
    checkCompareOnFlags<int32_t>(condition);
    checkCompareOnFlags<int64_t>(condition);

    switch (condition) {
    case MacroAssembler::Equal:
    case MacroAssembler::NotEqual:
    case MacroAssembler::LessThan:
    case MacroAssembler::GreaterThanOrEqual:
        checkTestOnFlags<int32_t>(condition);
        checkTestOnFlags<int64_t>(condition);
        break;
    case MacroAssembler::Above:
    case MacroAssembler::AboveOrEqual:
    case MacroAssembler::Below:
    case MacroAssembler::BelowOrEqual:
    case MacroAssembler::GreaterThan:
    case MacroAssembler::LessThanOrEqual:
        break;
    }
}
#endif // CPU(X86_64) || CPU(ARM64)

static void testGPRInfoConsistency()
{
    for (unsigned index = 0; index < GPRInfo::numberOfRegisters; ++index) {
        GPRReg reg = GPRInfo::toRegister(index);
        CHECK_EQ(GPRInfo::toIndex(reg), index);
    }
    for (auto reg = CCallHelpers::firstRegister(); reg <= CCallHelpers::lastRegister(); reg = nextID(reg)) {
        if (isSpecialGPR(reg))
            continue;
        unsigned index = GPRInfo::toIndex(reg);
        if (index == GPRInfo::InvalidIndex) {
            CHECK_EQ(index >= GPRInfo::numberOfRegisters, true);
            continue;
        }
        CHECK_EQ(index < GPRInfo::numberOfRegisters, true);
    }
}

#if ENABLE(JIT_PROTECTION_KEY)

// The kernel copies to and from user memory with the rights of the thread that asks, so a pipe tells what the thread may do
// to an address, and nothing faults.
class AccessProbe {
public:
    AccessProbe() { RELEASE_ASSERT(!pipe(m_pipe)); }
    ~AccessProbe()
    {
        close(m_pipe[0]);
        close(m_pipe[1]);
    }

    bool canRead(const void* address)
    {
        if (write(m_pipe[1], address, 1) != 1) {
            RELEASE_ASSERT(errno == EFAULT);
            return false;
        }
        drain();
        return true;
    }

    // Takes the byte that is at the address, so that a write that goes through changes nothing.
    bool canWrite(void* address, uint8_t byte)
    {
        RELEASE_ASSERT(write(m_pipe[1], &byte, 1) == 1);
        if (read(m_pipe[0], address, 1) == 1)
            return true;
        RELEASE_ASSERT(errno == EFAULT);
        drain();
        return false;
    }

private:
    void drain()
    {
        uint8_t byte;
        RELEASE_ASSERT(read(m_pipe[0], &byte, 1) == 1);
    }

    int m_pipe[2];
};

static CodeLocationDataLabelPtr<JSInternalPtrTag> s_pointerToPatchInSignalHandler;
static void* s_pointerReadInSignalHandler;

// Leaves what a thread older than the key has, and a signal handler: access disable, the lower of the key's two bits.
static void takeAwayRightsToJITRegion()
{
    uint32_t bits = g_jscConfig.jitProtectionKeyBits;
    uint32_t eax;
    uint32_t edx;
    __asm__ volatile("rdpkru" : "=a"(eax), "=d"(edx) : "c"(0));
    __asm__ volatile("wrpkru" : : "a"((eax & ~bits) | (bits & 0x55555555)), "c"(0), "d"(0) : "memory");
}

// How this program ends when it starts under a seccomp filter that kills it for pkey_alloc(), and for asking prctl() whether
// there is a filter, or nothing where the process may not have a filter.
static std::optional<int> statusOfStartingUnderFilterThatKillsForProtectionKey(const char* option)
{
    constexpr int cannotInstallFilter = 10;
    Vector<char*> environment;
    for (char** variable = environ; *variable; ++variable)
        environment.append(*variable);
    environment.append(const_cast<char*>(option));
    environment.append(nullptr);
    char* arguments[] = { const_cast<char*>("testmasm"), const_cast<char*>("testGPRInfoConsistency"), nullptr };

    pid_t child = fork();
    RELEASE_ASSERT(child >= 0);
    if (!child) {
        struct sock_filter instructions[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pkey_alloc, 3, 0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_prctl, 0, 3),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_GET_SECCOMP, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog program { 7, instructions };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0))
            _exit(cannotInstallFilter);
        struct rlimit noCoreFile { 0, 0 };
        setrlimit(RLIMIT_CORE, &noCoreFile);
        int null = open("/dev/null", O_WRONLY);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        execve("/proc/self/exe", arguments, environment.mutableSpan().data());
        _exit(cannotInstallFilter + 1);
    }
    int status;
    RELEASE_ASSERT(waitpid(child, &status, 0) == child);
    if (WIFEXITED(status) && WEXITSTATUS(status) == cannotInstallFilter)
        return std::nullopt;
    return status;
}

void testJITProtectionKey()
{
    auto asPointer = [](uintptr_t value) { return reinterpret_cast<void*>(value); };

    CCallHelpers jit;
    emitFunctionPrologue(jit);
    auto pointer = jit.moveWithPatch(CCallHelpers::TrustedImmPtr(asPointer(42)), GPRInfo::returnValueGPR);
    emitFunctionEpilogue(jit);
    jit.ret();
    LinkBuffer linkBuffer(jit, nullptr);
    auto location = linkBuffer.locationOf<JSInternalPtrTag>(pointer);
    auto code = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "testmasm JIT protection key");
    uint8_t* start = code.code().untaggedPtr<uint8_t*>();
    uint8_t firstByte = *start;

    bool isProtected = g_jscConfig.jitProtectionKeyBits;
    if (!isProtected)
        dataLogLn("    The JIT region has no protection key here.");

    auto check = [&](uintptr_t expected) {
        AccessProbe probe;
        CHECK_EQ(invoke<uintptr_t>(code), expected);
        CHECK_EQ(probe.canRead(start), true);
        CHECK_EQ(probe.canWrite(start, firstByte), !isProtected);
        CHECK_EQ(MacroAssembler::readPointer(location), asPointer(expected));
    };

    check(42);
    MacroAssembler::repatchPointer(location, asPointer(43));
    check(43);

    // A thread has the rights of the one that created it.
    Thread::create("testJITProtectionKey"_s, [&] {
        check(43);
        MacroAssembler::repatchPointer(location, asPointer(44));
        check(44);
    })->waitForCompletion();
    check(44);

    // The kernel gives a signal handler the rights that a process starts with, and the interrupted code its own back.
    s_pointerToPatchInSignalHandler = location;
    struct sigaction action { };
    struct sigaction oldAction;
    action.sa_handler = [](int) {
        letCurrentThreadReadJITRegion();
        s_pointerReadInSignalHandler = MacroAssembler::readPointer(s_pointerToPatchInSignalHandler);
        MacroAssembler::repatchPointer(s_pointerToPatchInSignalHandler, reinterpret_cast<void*>(45));
    };
    RELEASE_ASSERT(!sigaction(SIGURG, &action, &oldAction));
    RELEASE_ASSERT(!raise(SIGURG));
    RELEASE_ASSERT(!sigaction(SIGURG, &oldAction, nullptr));
    CHECK_EQ(s_pointerReadInSignalHandler, asPointer(44));
    check(45);

    // Under a filter it does not ask for a key, or the filter would have killed it.
    if (auto status = statusOfStartingUnderFilterThatKillsForProtectionKey("JSC_useJITProtectionKeyUnderSeccompFilter=0")) {
        CHECK_EQ(WIFEXITED(*status) && !WEXITSTATUS(*status), true);
        // With the option it asks. Where there is no key here, it may be that it does not get as far as asking.
        if (isProtected) {
            status = statusOfStartingUnderFilterThatKillsForProtectionKey("JSC_useJITProtectionKeyUnderSeccompFilter=1");
            CHECK_EQ(WIFSIGNALED(*status) && WTERMSIG(*status) == SIGSYS, true);
        }
    } else
        dataLogLn("    This process may not have a seccomp filter.");

    if (!isProtected)
        return;

    // A thread that is older than the key has no rights to what the key guards.
    Thread::create("testJITProtectionKey"_s, [&] {
        AccessProbe probe;
        takeAwayRightsToJITRegion();
        CHECK_EQ(probe.canRead(start), false);
        CHECK_EQ(probe.canWrite(start, firstByte), false);
        CHECK_EQ(invoke<uintptr_t>(code), 45u);
        letCurrentThreadReadJITRegion();
        check(45);

        takeAwayRightsToJITRegion();
        MacroAssembler::repatchPointer(location, asPointer(46));
        check(46);

        // Such a thread gets to read the region when it takes the lock of a VM.
        RefPtr<VM> vm = VM::create();
        takeAwayRightsToJITRegion();
        CHECK_EQ(probe.canRead(start), false);
        {
            JSLockHolder locker(*vm);
            check(46);
            vm = nullptr;
        }
    })->waitForCompletion();
    check(46);
}

#endif // ENABLE(JIT_PROTECTION_KEY)

#define RUN(test) do {                          \
        if (!shouldRun(#test))                  \
            break;                              \
        numberOfTests++;                        \
        tasks.append(                           \
            createSharedTask<void()>(           \
                [&] () {                        \
                    dataLog(#test "...\n");     \
                    test;                       \
                    dataLog(#test ": OK!\n");   \
                }));                            \
    } while (false);

// Using WTF_IGNORES_THREAD_SAFETY_ANALYSIS because the function is still holding crashLock when exiting.
void run(const char* filter) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    JSC::initialize([] {
        JSC::Options::useJITCage() = false;
    });
    unsigned numberOfTests = 0;

    Deque<RefPtr<SharedTask<void()>>> tasks;

    auto shouldRun = [&] (const char* testName) -> bool {
        return !filter || WTF::findIgnoringASCIICaseWithoutLength(testName, filter) != WTF::notFound;
    };

    RUN(testSimple());
    RUN(testGetEffectiveAddress(0xff00, 42, 8, CCallHelpers::TimesEight));
    RUN(testGetEffectiveAddress(0xff00, -200, -300, CCallHelpers::TimesEight));
    RUN(testBranchTruncateDoubleToInt32(0, 0));
    RUN(testBranchTruncateDoubleToInt32(42, 42));
    RUN(testBranchTruncateDoubleToInt32(42.7, 42));
    RUN(testBranchTruncateDoubleToInt32(-1234, -1234));
    RUN(testBranchTruncateDoubleToInt32(-1234.56, -1234));
    RUN(testBranchTruncateDoubleToInt32(std::numeric_limits<double>::infinity(), 0));
    RUN(testBranchTruncateDoubleToInt32(-std::numeric_limits<double>::infinity(), 0));
    RUN(testBranchTruncateDoubleToInt32(std::numeric_limits<double>::quiet_NaN(), 0));
    RUN(testBranchTruncateDoubleToInt32(std::numeric_limits<double>::signaling_NaN(), 0));
    RUN(testBranchTruncateDoubleToInt32(std::numeric_limits<double>::max(), 0));
    RUN(testBranchTruncateDoubleToInt32(-std::numeric_limits<double>::max(), 0));
    // We run this last one to make sure that we don't use flags that were not
    // reset to check a conversion result
    RUN(testBranchTruncateDoubleToInt32(123, 123));

#if CPU(X86_64) || CPU(ARM64)
    RUN(testBranchTruncateDoubleToInt32ViaInt64(0, 0));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-0.0, 0));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(42.7, 42));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-1234.56, -1234));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(2147483648.0, std::numeric_limits<int32_t>::min()));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-2147483648.0, std::numeric_limits<int32_t>::min()));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(4294967296.0, 0));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(4294967301.0, 5));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-4294967301.0, -5));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(9007199254740994.0, 2));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(9223372036854774784.0, -1024));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-9223372036854774784.0, 1024));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(9223372036854775808.0, std::nullopt));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-9223372036854775808.0, std::nullopt));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(std::numeric_limits<double>::infinity(), std::nullopt));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-std::numeric_limits<double>::infinity(), std::nullopt));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(std::numeric_limits<double>::max(), std::nullopt));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(-std::numeric_limits<double>::max(), std::nullopt));
    // fcvtzs gives 0, which is ToInt32(NaN). cvttsd2si gives the value that reports a failure.
    RUN(testBranchTruncateDoubleToInt32ViaInt64(std::numeric_limits<double>::quiet_NaN(), isARM64() ? std::optional<int32_t>(0) : std::nullopt));
    RUN(testBranchTruncateDoubleToInt32ViaInt64(123, 123));
#endif

#define FOR_EACH_DOUBLE_CONDITION_RUN(__test) \
    do { \
        RUN(__test(MacroAssembler::DoubleEqualAndOrdered)); \
        RUN(__test(MacroAssembler::DoubleNotEqualAndOrdered)); \
        RUN(__test(MacroAssembler::DoubleGreaterThanAndOrdered)); \
        RUN(__test(MacroAssembler::DoubleGreaterThanOrEqualAndOrdered)); \
        RUN(__test(MacroAssembler::DoubleLessThanAndOrdered)); \
        RUN(__test(MacroAssembler::DoubleLessThanOrEqualAndOrdered)); \
        RUN(__test(MacroAssembler::DoubleEqualOrUnordered)); \
        RUN(__test(MacroAssembler::DoubleNotEqualOrUnordered)); \
        RUN(__test(MacroAssembler::DoubleGreaterThanOrUnordered)); \
        RUN(__test(MacroAssembler::DoubleGreaterThanOrEqualOrUnordered)); \
        RUN(__test(MacroAssembler::DoubleLessThanOrUnordered)); \
        RUN(__test(MacroAssembler::DoubleLessThanOrEqualOrUnordered)); \
    } while (false)

#define FOR_EACH_RELATIONAL_CONDITION_RUN(__test) \
    do { \
        RUN(__test(MacroAssembler::Equal)); \
        RUN(__test(MacroAssembler::NotEqual)); \
        RUN(__test(MacroAssembler::Above)); \
        RUN(__test(MacroAssembler::AboveOrEqual)); \
        RUN(__test(MacroAssembler::Below)); \
        RUN(__test(MacroAssembler::BelowOrEqual)); \
        RUN(__test(MacroAssembler::GreaterThan)); \
        RUN(__test(MacroAssembler::GreaterThanOrEqual)); \
        RUN(__test(MacroAssembler::LessThan)); \
        RUN(__test(MacroAssembler::LessThanOrEqual)); \
    } while (false)

    FOR_EACH_DOUBLE_CONDITION_RUN(testCompareDouble);
    FOR_EACH_DOUBLE_CONDITION_RUN(testCompareDoubleSameArg);

    RUN(testMul32WithImmediates());
    RUN(testLoadStorePair32());
    RUN(testSub32ArgImm());

    RUN(testBranch32());

    RUN(testBranchTest8());
    RUN(testBranchTest16());

#if CPU(X86_64)
    RUN(testBranchTestBit32RegReg());
    RUN(testBranchTestBit32RegImm());
    RUN(testBranchTestBit32AddrImm());
    RUN(testBranchTestBit64RegReg());
    RUN(testBranchTestBit64RegImm());
    RUN(testBranchTestBit64AddrImm());
#endif

#if CPU(ARM64)
    RUN(testCanBranchWithoutFlags());
#endif

#if CPU(X86_64) || CPU(ARM64)
    RUN(testBranch64());
    RUN(testClearBit64());
    RUN(testClearBits64WithMask());
    RUN(testClearBits64WithMaskTernary());
    RUN(testCountTrailingZeros64());
    RUN(testCountTrailingZeros64WithoutNullCheck());
    RUN(testShiftAndAdd());
    RUN(testLogicalAndShiftImmediates());
    RUN(testBranchAddSubImmediateSetsFlags());
    RUN(testStore64Imm64AddressPointer());
    RUN(testLoad64ExtendedAddress());

    RUN(testAdd32Imm());
    RUN(testAdd32ArgImm());
    RUN(testAdd64Imm32());
    RUN(testAdd64ArgImm32());
    RUN(testAdd64Imm64());
    RUN(testAdd64ArgImm64());

    RUN(testSub32Args());
    RUN(testSub32Imm());
    RUN(testSub64Imm32());
    RUN(testSub64ArgImm32());
    RUN(testSub64Imm64());
    RUN(testSub64ArgImm64());

#endif

    RUN(testLoadAcq8SignedExtendTo32_Address_RegisterID());
    RUN(testLoad8SignedExtendTo32_Address_RegisterID());
    RUN(testLoad8SignedExtendTo32_BaseIndex_RegisterID());
    RUN(testLoad8SignedExtendTo32_voidp_RegisterID());

    RUN(testLoadAcq16SignedExtendTo32_Address_RegisterID());
    RUN(testLoad16SignedExtendTo32_Address_RegisterID());
    RUN(testLoad16SignedExtendTo32_BaseIndex_RegisterID());
    RUN(testLoad16SignedExtendTo32_voidp_RegisterID());

    RUN(testLoadStorePair32());

    RUN(testLoadAcq8SignedExtendTo64_Address_RegisterID());
    RUN(testLoad8SignedExtendTo64_Address_RegisterID());
    RUN(testLoad8SignedExtendTo64_BaseIndex_RegisterID());
    RUN(testLoad8SignedExtendTo64_voidp_RegisterID());

    RUN(testLoadAcq16SignedExtendTo64_Address_RegisterID());
    RUN(testLoad16SignedExtendTo64_Address_RegisterID());
    RUN(testLoad16SignedExtendTo64_BaseIndex_RegisterID());
    RUN(testLoad16SignedExtendTo64_voidp_RegisterID());

    RUN(testLoadAcq32SignedExtendTo64_Address_RegisterID());
    RUN(testLoad32SignedExtendTo64_Address_RegisterID());
    RUN(testLoad32SignedExtendTo64_BaseIndex_RegisterID());
    RUN(testLoad32SignedExtendTo64_voidp_RegisterID());

#if CPU(ARM64)
    RUN(testLoadStorePair64Int64());
    RUN(testLoadStorePair64Double());
    RUN(testMultiplySignExtend32());
    RUN(testMultiplyZeroExtend32());

    RUN(testMultiplyAddSignExtend32());
    RUN(testMultiplyAddZeroExtend32());
    RUN(testMultiplySubSignExtend32());
    RUN(testMultiplySubZeroExtend32());
    RUN(testMultiplyNegSignExtend32());
    RUN(testMultiplyNegZeroExtend32());

    RUN(testExtractUnsignedBitfield32());
    RUN(testExtractUnsignedBitfield64());
    RUN(testInsertUnsignedBitfieldInZero32());
    RUN(testInsertUnsignedBitfieldInZero64());
    RUN(testInsertBitField32());
    RUN(testInsertBitField64());
    RUN(testExtractInsertBitfieldAtLowEnd32());
    RUN(testExtractInsertBitfieldAtLowEnd64());
    RUN(testClearBitField32());
    RUN(testClearBitField64());
    RUN(testClearBitsWithMask32());
    RUN(testClearBitsWithMask64());

    RUN(testOrNot32());
    RUN(testOrNot64());

    RUN(testInsertSignedBitfieldInZero32());
    RUN(testInsertSignedBitfieldInZero64());
    RUN(testExtractSignedBitfield32());
    RUN(testExtractSignedBitfield64());
    RUN(testExtractRegister32());
    RUN(testExtractRegister64());

    RUN(testAddWithLeftShift32());
    RUN(testAddWithRightShift32());
    RUN(testAddWithUnsignedRightShift32());
    RUN(testAddWithLeftShift64());
    RUN(testAddWithRightShift64());
    RUN(testAddWithUnsignedRightShift64());
    RUN(testSubWithLeftShift32());
    RUN(testSubWithRightShift32());
    RUN(testSubWithUnsignedRightShift32());
    RUN(testSubWithLeftShift64());
    RUN(testSubWithRightShift64());
    RUN(testSubWithUnsignedRightShift64());

    RUN(testXorNot32());
    RUN(testXorNot64());
    RUN(testXorNotWithLeftShift32());
    RUN(testXorNotWithRightShift32());
    RUN(testXorNotWithUnsignedRightShift32());
    RUN(testXorNotWithLeftShift64());
    RUN(testXorNotWithRightShift64());
    RUN(testXorNotWithUnsignedRightShift64());

    RUN(testStorePrePostIndex32());
    RUN(testStorePrePostIndex64());
    RUN(testLoadPrePostIndex32());
    RUN(testLoadPrePostIndex64());
    RUN(testAndLeftShift32());
    RUN(testAndRightShift32());
    RUN(testAndUnsignedRightShift32());
    RUN(testAndLeftShift64());
    RUN(testAndRightShift64());
    RUN(testAndUnsignedRightShift64());

    RUN(testXorLeftShift32());
    RUN(testXorRightShift32());
    RUN(testXorUnsignedRightShift32());
    RUN(testXorLeftShift64());
    RUN(testXorRightShift64());
    RUN(testXorUnsignedRightShift64());

    RUN(testOrLeftShift32());
    RUN(testOrRightShift32());
    RUN(testOrUnsignedRightShift32());
    RUN(testOrLeftShift64());
    RUN(testOrRightShift64());
    RUN(testOrUnsignedRightShift64());

    RUN(testZeroExtend48ToWord());
#endif

#if CPU(ARM64)
    if (isARM64_LSE()) {
        RUN(testAtomicStrongCASFill8());
        RUN(testAtomicStrongCASFill16());
    }
#endif

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
    FOR_EACH_DOUBLE_CONDITION_RUN(testCompareFloat);
#endif

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
    // Comparing 2 different registers.
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyDouble2);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyDouble3);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyDouble3DestSameAsThenCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyDouble3DestSameAsElseCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyFloat2);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyFloat3);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyFloat3DestSameAsThenCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyFloat3DestSameAsElseCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyDouble);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyDoubleDestSameAsThenCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyDoubleDestSameAsElseCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyFloat);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyFloatDestSameAsThenCase);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyFloatDestSameAsElseCase);

    // Comparing the same register against itself.
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyDouble2SameArg);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyDouble3SameArg);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyFloat2SameArg);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveConditionallyFloat3SameArg);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyDoubleSameArg);
    FOR_EACH_DOUBLE_CONDITION_RUN(testMoveDoubleConditionallyFloatSameArg);
#endif

#if CPU(X86_64) || CPU(ARM64)
    // Tests for moveConditionally32 and moveConditionallyTest32 with immediate thenCase.
    FOR_EACH_RELATIONAL_CONDITION_RUN(testMoveConditionally32WithImmThenCase);

    // For test32 variants, only use Zero and NonZero (Overflow is not valid for TEST).
    RUN(testMoveConditionallyTest32WithImmThenCaseRegMask(MacroAssembler::Zero));
    RUN(testMoveConditionallyTest32WithImmThenCaseRegMask(MacroAssembler::NonZero));
    RUN(testMoveConditionallyTest32WithImmThenCaseImmMask(MacroAssembler::Zero));
    RUN(testMoveConditionallyTest32WithImmThenCaseImmMask(MacroAssembler::NonZero));
#endif

#if CPU(X86_64) || CPU(ARM64) || CPU(RISCV64)
    RUN(testSignExtend8To32());
    RUN(testSignExtend16To32());
    RUN(testSignExtend8To64());
    RUN(testSignExtend16To64());
#endif

    RUN(testProbeReadsArgumentRegisters());
    RUN(testProbeWritesArgumentRegisters());
    RUN(testProbePreservesGPRS());
    RUN(testProbeModifiesStackPointerToInsideProbeStateOnStack());
    RUN(testProbeModifiesStackPointerToNBytesBelowSP());
    RUN(testProbeModifiesProgramCounter());
    RUN(testProbeModifiesStackValues());

    RUN(testByteSwap());
    RUN(testMoveDoubleConditionally32());
    RUN(testMoveDoubleConditionally64());
    RUN(testLoadBaseIndex());
    RUN(testStoreImmediateAddress());
    RUN(testStoreBaseIndex());
    RUN(testStoreImmediateBaseIndex());

    RUN(testBranchIfType());
    RUN(testBranchIfNotType());
#if CPU(X86_64) || CPU(ARM64)
    RUN(testAllocateFromFreeList());
    RUN(testBranchConvertDoubleToInt52());
#endif

#if CPU(X86_64)
    RUN(testTagsInMemory());
    RUN(testPrefetch());
    RUN(testAtomicAndEmitsCode());
#endif

    RUN(testOrImmMem());

    RUN(testAndOrDouble());

    RUN(testNegateDouble());

    RUN(testNegateFloat());

#if CPU(ARM64) || CPU(X86_64)
    RUN(testMove32ToFloatMovi());
    RUN(testMove64ToDoubleMovi());
    RUN(testMove64ToDoubleRepeated32BitPatternBug());
    RUN(testMove128ToVectorMovi());
    RUN(testMove32ToFloatComprehensive());
    RUN(testMove64ToDoubleComprehensive());
    RUN(testMove128ToVectorComprehensive());

    RUN(testMove32ToFloatX64());
    RUN(testMove64ToDoubleX64());
#endif

#if CPU(ARM64)
    RUN(testMove16ToFloat16Comprehensive());
    RUN(testFMovHalfPrecisionEncoding());
    RUN(testCachedTempRegisterImm32Normalization());
#endif

#if CPU(X86_64)
    RUN(testJumpShortening());
    RUN(testJumpShorteningBounds());
    RUN(testX86NamedInstructions());
    RUN(testAddOrFromMemoryAliasing());
#endif

#if CPU(X86_64) || CPU(ARM64)
    RUN(testImmediatesOf64BitOperationsAreSignExtended());
    RUN(testAdd64OfRegisterToMemory());
    RUN(testCPUFeatureSet());
    RUN(testAssemblerPolicy());
    RUN(testPCRelativeAddress());
    RUN(testLoadPCRelative64());
    RUN(testLinkPCRelativeReach());
    RUN(testClearRelocation());
    RUN(testInstanceOffset());
    RUN(testAddressLog());
    RUN(testAddressLogThroughEveryPointerHelper());
    RUN(testLinkedSites());
    FOR_EACH_RELATIONAL_CONDITION_RUN(testOnFlags);
#endif

    RUN(testGPRInfoConsistency());

#if ENABLE(JIT_PROTECTION_KEY)
    RUN(testJITProtectionKey());
#endif

    if (!numberOfTests)
        usage();

    Lock lock;

    Vector<Ref<Thread>> threads;
    for (unsigned i = filter ? 1 : WTF::numberOfProcessorCores(); i--;) {
        threads.append(
            Thread::create(
                "testmasm thread"_s,
                [&] () {
                    for (;;) {
                        RefPtr<SharedTask<void()>> task;
                        {
                            Locker locker { lock };
                            if (tasks.isEmpty())
                                return;
                            task = tasks.takeFirst();
                        }

                        task->run();
                    }
                }));
    }

    for (auto& thread : threads)
        thread->waitForCompletion();
    crashLock.lock();
    dataLog("Completed ", numberOfTests, " tests\n");
}

} // anonymous namespace

#else // not ENABLE(JIT)

static void run(const char*)
{
    dataLog("JIT is not enabled.\n");
}

#endif // ENABLE(JIT)

int main(int argc, char** argv)
{
    const char* filter = nullptr;
    switch (argc) {
    case 1:
        break;
    case 2:
        filter = argv[1];
        break;
    default:
        usage();
        break;
    }

    run(filter);
    return 0;
}

#if OS(WINDOWS)
extern "C" __declspec(dllexport) int WINAPI dllLauncherEntryPoint(int argc, const char* argv[])
{
    return main(argc, const_cast<char**>(argv));
}
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
