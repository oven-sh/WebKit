/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"

#include "AOTB3Policy.h"
#include "AOTConvention.h"
#include "AOTElideStackChecks.h"
#include "AOTInstance.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRelocationLowering.h"
#include "AOTStackCheckEmitter.h"
#include "AirCode.h"
#include "B3ArgumentRegValue.h"
#include "B3BasicBlockInlines.h"
#include "B3CompilationScratch.h"
#include "B3Const64Value.h"
#include "B3Generate.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3ProcedureInlines.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "StackManager.h"
#include <array>
#include <limits>
#include <wtf/StackPointer.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::PendingStackCheck;
using Typed::StackLimit;

namespace {

using Address = CCallHelpers::Address;
using TrustedImm32 = CCallHelpers::TrustedImm32;
using TrustedImm64 = CCallHelpers::TrustedImm64;

enum class ReadsMacroScratchRegister : bool { No, Yes };

struct StackCheckFixture {
    StackCheckFixture()
        : recorder(jit, context.assemblerPolicy())
        , lowering(ImageRelocationLowering::instanceRuntimeTable(), recorder)
        , proc(b3PolicyFor(context, FunctionBackEndTraits { .function = Typed::FunctionID(0u) }, lowering), &scratch)
    {
        Typed::pinRegisters(proc);
        Typed::setStackCheckGenerator(proc, Typed::StackCheck::Required, frame, state);
    }

    BuildContext context { ImageTests::wireOptions() };
    CCallHelpers jit;
    RelocationRecorder recorder;
    ImageRelocationLowering lowering;
    B3::CompilationScratch scratch;
    B3::Procedure proc;
    Typed::FrameRequirements frame;
    Typed::StackCheckState state;
};

struct CompiledProcedure {
    Vector<uint8_t> bytes;
    Vector<StackLimit, 1> limits;
    unsigned frameSize { 0 };
};

struct StackLimits {
    uintptr_t soft;
    uintptr_t trapAwareSoft;
};

} // anonymous namespace

static constexpr unsigned maxBytesOfSmallFrame = 4 * KB;
static constexpr unsigned smallSlotSize = 64;
static constexpr unsigned largeSlotSize = 16 * KB;
[[maybe_unused]] static constexpr unsigned macroScratchWord = numberOfDirectArgumentGPRs;
static constexpr unsigned numberOfPassedWords = numberOfDirectArgumentGPRs + 1;
static constexpr uint64_t didRun = 1;
static constexpr uint64_t didNotFit = 2;
static constexpr uintptr_t everyFrameFits = 0;
static constexpr uintptr_t noFrameFits = std::numeric_limits<uintptr_t>::max();
static constexpr CodeSymbol procedureSymbol { SymbolSpace::Function, 0 };

using PassedWords = std::array<uint64_t, numberOfPassedWords>;

static constexpr uint64_t passedWord(unsigned index)
{
    return 0x0101010101010101ull * (index + 1);
}

static Vector<GPRReg, 2> registersSavedByCaller()
{
    Vector<GPRReg, 2> registers { instanceGPR };
    for (GPRReg reg : unsavedCalleeSaveGPRs)
        registers.append(reg);
    return registers;
}

static void emitLoadOfPassedWords(CCallHelpers& jit, GPRReg words)
{
    auto wordAt = [&](unsigned index) {
        return Address(words, static_cast<int32_t>(index * sizeof(uint64_t)));
    };
#if CPU(X86_64)
    jit.load64(wordAt(macroScratchWord), CCallHelpers::s_scratchRegister);
#endif
    std::optional<unsigned> indexOfWords;
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (directArgumentGPR(index) == words) {
            indexOfWords = index;
            continue;
        }
        jit.load64(wordAt(index), directArgumentGPR(index));
    }
    AOT_TEST_CHECK(indexOfWords);
    jit.load64(wordAt(*indexOfWords), words);
}

// uint64_t (void* instance, const uint64_t* passedWords)
static void emitCaller(CCallHelpers& jit, RelocationRecorder& recorder)
{
    Vector<GPRReg, 2> saved = registersSavedByCaller();
    auto slotOf = [](size_t index) {
        return Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
    };
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(static_cast<int32_t>(WTF::roundUpToMultipleOf<16>(saved.size() * sizeof(CPURegister)))), CCallHelpers::stackPointerRegister);
    for (size_t index = 0; index < saved.size(); ++index)
        jit.storePtr(saved[index], slotOf(index));
    jit.move(GPRInfo::argumentGPR0, instanceGPR);
    emitLoadOfPassedWords(jit, GPRInfo::argumentGPR1);
    recorder.call(procedureSymbol);
    for (size_t index = 0; index < saved.size(); ++index)
        jit.loadPtr(slotOf(index), saved[index]);
    jit.emitFunctionEpilogue();
    jit.ret();
}

static void appendWritesToBothEndsOfSlot(B3::Procedure& proc, B3::BasicBlock* block, unsigned slotSize)
{
    B3::Value* base = block->appendNew<B3::SlotBaseValue>(proc, B3::Origin(), proc.addStackSlot(slotSize));
    B3::PatchpointValue* patchpoint = block->appendNew<B3::PatchpointValue>(proc, B3::Void, B3::Origin());
    patchpoint->appendSomeRegister(base);
    patchpoint->setGenerator([slotSize](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        GPRReg slot = params[0].gpr();
        jit.store64(slot, Address(slot));
        jit.store64(slot, Address(slot, static_cast<int32_t>(slotSize - sizeof(uint64_t))));
    });
}

// Word 0 is the address of the words that the procedure stores every other word it received to.
static void appendBody(B3::Procedure& proc, unsigned slotSize, ReadsMacroScratchRegister readsMacroScratchRegister)
{
    B3::BasicBlock* root = proc.addBlock();
    Vector<B3::Value*, numberOfPassedWords> received;
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index)
        received.append(root->appendNew<B3::ArgumentRegValue>(proc, B3::Origin(), directArgumentGPR(index)));
#if CPU(X86_64)
    if (readsMacroScratchRegister == ReadsMacroScratchRegister::Yes)
        received.append(root->appendNew<B3::ArgumentRegValue>(proc, B3::Origin(), CCallHelpers::s_scratchRegister));
#else
    AOT_TEST_CHECK(readsMacroScratchRegister == ReadsMacroScratchRegister::No);
#endif
    appendWritesToBothEndsOfSlot(proc, root, slotSize);
    for (unsigned index = 1; index < received.size(); ++index)
        root->appendNew<B3::MemoryValue>(proc, B3::Store, B3::Origin(), received[index], received[0], static_cast<int32_t>(index * sizeof(uint64_t)));
    root->appendNew<B3::Value>(proc, B3::Return, B3::Origin(), root->appendNew<B3::Const64Value>(proc, B3::Origin(), didRun));
}

static CompiledProcedure compile(unsigned slotSize, ReadsMacroScratchRegister readsMacroScratchRegister)
{
    StackCheckFixture fixture;
    B3::Procedure& proc = fixture.proc;
    appendBody(proc, slotSize, readsMacroScratchRegister);
    B3::prepareForGeneration(proc);
    AOT_TEST_CHECK(!proc.report().failure);

    CCallHelpers& jit = fixture.jit;
    emitCaller(jit, fixture.recorder);
    CCallHelpers::Label procedure = jit.label();
    B3::generate(proc, jit);
    AOT_TEST_CHECK(!proc.report().failure);

    CompiledProcedure compiled;
    compiled.frameSize = proc.code().frameSize();
    for (PendingStackCheck& check : fixture.state.pending) {
        compiled.limits.append(check.limit);
        check.doesNotFit.link(&jit);
    }
    jit.emitFunctionEpilogue();
    jit.move(TrustedImm64(didNotFit), GPRInfo::returnValueGPR);
    jit.ret();

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "AOT stack check test"_s);
    auto relocations = fixture.recorder.link(buffer, nullptr);
    AOT_TEST_CHECK(relocations && relocations->size() == 1 && relocations->first().codeSymbol() == procedureSymbol);
    compiled.bytes.append(buffer.bytes());
    AOT_TEST_CHECK(applyRelocation(compiled.bytes.mutableSpan(), 0, relocations->first(), buffer.linkBuffer().offsetOf(procedure)));
    return compiled;
}

static uint64_t run(const CompiledProcedure& compiled, StackLimits limits, PassedWords& received)
{
    constexpr size_t offsetOfMirror = AOT_INSTANCE_OFFSET(stackMirror);
    Vector<uint64_t> instance(FillWith { }, (offsetOfMirror + sizeof(StackManager::Mirror)) / sizeof(uint64_t) + 1, 0);
    uint8_t* mirror = reinterpret_cast<uint8_t*>(instance.mutableSpan().data()) + offsetOfMirror;
    memcpy(mirror + StackManager::Mirror::offsetOfSoftStackLimit(), &limits.soft, sizeof(uintptr_t));
    memcpy(mirror + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit(), &limits.trapAwareSoft, sizeof(uintptr_t));

    PassedWords passed;
    passed[0] = std::bit_cast<uint64_t>(received.data());
    for (unsigned index = 1; index < numberOfPassedWords; ++index)
        passed[index] = passedWord(index);
    received.fill(0);
    return ImageTests::ExecutableCopy(compiled.bytes.span()).call<uint64_t>(0, static_cast<void*>(instance.mutableSpan().data()), static_cast<const uint64_t*>(passed.data()));
}

static void checkReceivedArguments(const PassedWords& received)
{
    for (unsigned index = 1; index < numberOfDirectArgumentGPRs; ++index)
        AOT_TEST_CHECK(received[index] == passedWord(index));
}

static void checkReceivedNothing(const PassedWords& received)
{
    for (uint64_t word : received)
        AOT_TEST_CHECK(!word);
}

static CompiledProcedure compileWithLargeFrame()
{
    CompiledProcedure compiled = compile(largeSlotSize, ReadsMacroScratchRegister::No);
    AOT_TEST_CHECK(compiled.frameSize > maxBytesOfSmallFrame);
    AOT_TEST_CHECK(compiled.limits.size() == 1 && compiled.limits[0] == StackLimit::Soft);
    return compiled;
}

static CompiledProcedure compileWithSmallFrame(ReadsMacroScratchRegister readsMacroScratchRegister)
{
    CompiledProcedure compiled = compile(smallSlotSize, readsMacroScratchRegister);
    AOT_TEST_CHECK(compiled.frameSize <= maxBytesOfSmallFrame);
    AOT_TEST_CHECK(compiled.limits.size() == 1 && compiled.limits[0] == StackLimit::TrapAwareSoft);
    return compiled;
}

static void testLargeFrameKeepsArguments()
{
    PassedWords received;
    AOT_TEST_CHECK(run(compileWithLargeFrame(), { everyFrameFits, noFrameFits }, received) == didRun);
    checkReceivedArguments(received);
}

static void testLargeFrameIsCheckedBeforeItIsAllocated()
{
    if (framesAreThoseOfAddressSanitizer)
        return;
    CompiledProcedure compiled = compileWithLargeFrame();
    uintptr_t limitBetweenStackPointerAndFrameEnd = std::bit_cast<uintptr_t>(currentStackPointer()) - largeSlotSize / 2;
    PassedWords received;
    AOT_TEST_CHECK(run(compiled, { limitBetweenStackPointerAndFrameEnd, everyFrameFits }, received) == didNotFit);
    checkReceivedNothing(received);
}

static void testSmallFrameKeepsArguments()
{
    PassedWords received;
    AOT_TEST_CHECK(run(compileWithSmallFrame(ReadsMacroScratchRegister::No), { noFrameFits, everyFrameFits }, received) == didRun);
    checkReceivedArguments(received);
}

static void testSmallFrameDoesNotFit()
{
    if (framesAreThoseOfAddressSanitizer)
        return;
    PassedWords received;
    AOT_TEST_CHECK(run(compileWithSmallFrame(ReadsMacroScratchRegister::No), { everyFrameFits, noFrameFits }, received) == didNotFit);
    checkReceivedNothing(received);
}

#if CPU(X86_64)
static void testSmallFrameKeepsMacroScratchRegister()
{
    PassedWords received;
    AOT_TEST_CHECK(run(compileWithSmallFrame(ReadsMacroScratchRegister::Yes), { noFrameFits, everyFrameFits }, received) == didRun);
    checkReceivedArguments(received);
    AOT_TEST_CHECK(received[macroScratchWord] == passedWord(macroScratchWord));
}
#endif

std::span<const NativeTest> stackCheckTests()
{
    static constexpr NativeTest tests[] = {
        { "codegen/stack-check/large-frame-keeps-arguments"_s, ImageTests::whenExecutable<testLargeFrameKeepsArguments> },
        { "codegen/stack-check/large-frame-is-checked-before-it-is-allocated"_s, ImageTests::whenExecutable<testLargeFrameIsCheckedBeforeItIsAllocated> },
        { "codegen/stack-check/small-frame-keeps-arguments"_s, ImageTests::whenExecutable<testSmallFrameKeepsArguments> },
        { "codegen/stack-check/small-frame-does-not-fit"_s, ImageTests::whenExecutable<testSmallFrameDoesNotFit> },
#if CPU(X86_64)
        { "codegen/stack-check/small-frame-keeps-macro-scratch-register"_s, ImageTests::whenExecutable<testSmallFrameKeepsMacroScratchRegister> },
#endif
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
