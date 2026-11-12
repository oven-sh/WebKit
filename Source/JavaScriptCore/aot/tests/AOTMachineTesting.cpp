/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineTesting.h"

#if ENABLE(AOT)

#include "AOTMachineDescription.h"
#include "AOTMachinePrinter.h"
#include "AOTMachineTarget.h"
#include "AOTNativeTest.h"
#include "AOTRelocation.h"
#include "AOTStubAssembler.h"
#include "AOTStubRegisters.h"
#include "AOTVector.h"
#include "AssemblerPolicy.h"
#include "CCallHelpers.h"
#include "CPU.h"
#include "CPUFeatureSet.h"
#include "ExecutableAllocator.h"
#include "ExecutableMemoryHandle.h"
#include "LinkBuffer.h"
#include "Options.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

static const AOTHeaps& heapsOfTestProcedures()
{
    static LazyNeverDestroyed<AOTHeaps> heaps;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        heaps.construct();
    });
    return heaps.get();
}

static ProcedureTraits traitsOfTestProcedure(ProcedureTraits traits)
{
    traits.checksAllocation = true;
    return traits;
}

enum class TestProcedureStage : uint8_t { Appended, Ordered, Scanned, Allocated, Framed };

struct TestProcedure::Storage {
    explicit Storage(const ProcedureTraits& traits)
        : procedure(arena, heapsOfTestProcedures().tree(), traits)
        , output(procedure)
    {
    }

    TestProcedureStage stage() const
    {
        if (!didPrepare)
            return TestProcedureStage::Appended;
        if (didFinishPreparing)
            return TestProcedureStage::Framed;
        switch (procedure.failure()->kind) {
        case FailureKind::MalformedProcedure:
        case FailureKind::AccessOutsideDeclaredEffects:
            return TestProcedureStage::Appended;
        case FailureKind::InnerPointerWithoutOwner:
            return TestProcedureStage::Scanned;
        case FailureKind::OutOfRegisters:
        case FailureKind::WrongValueInRegister:
        case FailureKind::WrongValueInSpillSlot:
        case FailureKind::FrameAddressAcrossSuspension:
        case FailureKind::NarrowReferenceWithoutPointer:
        case FailureKind::UpperHalfIsNotClear:
            return TestProcedureStage::Allocated;
        case FailureKind::MissingFrameSetup:
            return TestProcedureStage::Framed;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    String printed(TestProcedureStage needed, void (*print)(PrintStream&, const Procedure&)) const
    {
        if (stage() < needed)
            return { };
        StringPrintStream out;
        print(out, procedure);
        return out.toString();
    }

    Arena arena;
    Procedure procedure;
    Output output;
    RefPtr<ExecutableMemoryHandle> code;
    Vector<uint8_t> linkedCode;
    MacroAssemblerCodeRef<JSEntryPtrTag> wrapper;
    bool didPrepare { false };
    bool didFinishPreparing { false };
    bool didGenerate { false };
};

#if CPU(ARM64)
static constexpr std::array<GPRReg, 10> gprsKeptByTestWrapper {
    ARM64Registers::x19, ARM64Registers::x20, ARM64Registers::x21, ARM64Registers::x22, ARM64Registers::x23,
    ARM64Registers::x24, ARM64Registers::x25, ARM64Registers::x26, ARM64Registers::x27, ARM64Registers::x28,
};
static constexpr std::array<FPRReg, 8> fprsKeptByTestWrapper {
    ARM64Registers::q8, ARM64Registers::q9, ARM64Registers::q10, ARM64Registers::q11,
    ARM64Registers::q12, ARM64Registers::q13, ARM64Registers::q14, ARM64Registers::q15,
};
static_assert(std::ranges::contains(gprsKeptByTestWrapper, GPRInfo::numberTagRegister) && std::ranges::contains(gprsKeptByTestWrapper, GPRInfo::notCellMaskRegister));
#elif CPU(X86_64)
static constexpr std::array<GPRReg, 5> gprsKeptByTestWrapper { X86Registers::ebx, X86Registers::r12, X86Registers::r13, X86Registers::r14, X86Registers::r15 };
static constexpr std::array<FPRReg, 0> fprsKeptByTestWrapper { };
#endif
static_assert(std::ranges::contains(gprsKeptByTestWrapper, instanceGPR) && std::ranges::contains(gprsKeptByTestWrapper, cellCageBaseGPR));
static_assert(!std::ranges::contains(gprsKeptByTestWrapper, scratchGPR));
static_assert(std::ranges::all_of(unsavedCalleeSaveGPRs, [](GPRReg reg) {
    return std::ranges::contains(gprsKeptByTestWrapper, reg);
}));

static CCallHelpers::Address slotOfTestWrapper(size_t index)
{
    return CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
}

static MacroAssemblerCodeRef<JSEntryPtrTag> compileTestWrapper(uint8_t* entry, void* instance)
{
    constexpr size_t numberOfSlots = gprsKeptByTestWrapper.size() + fprsKeptByTestWrapper.size();
    constexpr size_t bytesOfSlots = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(numberOfSlots * sizeof(CPURegister));

    CCallHelpers jit;
    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(bytesOfSlots)), CCallHelpers::stackPointerRegister);
    size_t slot = 0;
    for (GPRReg reg : gprsKeptByTestWrapper)
        jit.storePtr(reg, slotOfTestWrapper(slot++));
    for (FPRReg reg : fprsKeptByTestWrapper)
        jit.storeDouble(reg, slotOfTestWrapper(slot++));

    jit.move(CCallHelpers::TrustedImmPtr(instance), instanceGPR);
    jit.move(CCallHelpers::TrustedImm32(0), cellCageBaseGPR);
#if CPU(ARM64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.move(CCallHelpers::TrustedImmPtr(tagCodePtr<JITThunkPtrTag>(entry)), scratchGPR);
    jit.call(scratchGPR, JITThunkPtrTag);

    slot = 0;
    for (GPRReg reg : gprsKeptByTestWrapper)
        jit.loadPtr(slotOfTestWrapper(slot++), reg);
    for (FPRReg reg : fprsKeptByTestWrapper)
        jit.loadDouble(slotOfTestWrapper(slot++), reg);
    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer linkBuffer(jit, nullptr);
    return FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT machine test wrapper");
}

static constexpr size_t alignmentOfTestProcedureCode = 4 * KB;

static ASCIILiteral describeLinkFailureOfTestProcedure(LinkFailure failure)
{
    switch (failure) {
    case LinkFailure::ProcessAddressInCode:
        return "it holds an address of this process. A generator hands one over as a TrustedImm64, never as a TrustedImmPtr, and links no call to a place outside the code"_s;
    case LinkFailure::OutOfPCRelativeReach:
        return "a place in it is out of reach"_s;
    case LinkFailure::OutOfMemory:
        return "there is no memory for it"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

[[noreturn]] static void failToLinkTestProcedure(const Relocation& relocation, ASCIILiteral reason)
{
    dataLogLn("The code of a test procedure names symbol ", relocation.symbol, " of space ", static_cast<unsigned>(relocation.space), " at offset ", relocation.offset, ": ", reason);
    failNativeTest(__FILE__, __LINE__, "every relocation of a test procedure is applied");
}

Vector<uint8_t> layOutTestProcedure(const Procedure& procedure, ImageLinkBuffer& buffer, std::span<const Relocation> relocations)
{
    decltype(RareListsOfBody::constants) constants;
    describeConstants(procedure, constants);
    decltype(RareListsOfBody::jumpTables) jumpTables;
    describeJumpTables(procedure, buffer.linkBuffer(), jumpTables);

    std::span<const uint8_t> code = buffer.bytes();
    size_t size = code.size();
    auto reserve = [&](size_t alignment, size_t numberOfBytes) {
        size_t offset = roundUpToMultipleOf(alignment, size);
        size = offset + numberOfBytes;
        return offset;
    };
    Vector<size_t> constantOffsets(constants.size(), [&](size_t index) {
        const ConstantData& constant = constants[index];
        AOT_TEST_CHECK(hasOneBitSet(constant.alignment) && constant.alignment <= alignmentOfTestProcedureCode);
        return reserve(constant.alignment, constant.bytes.size());
    });
    Vector<size_t> jumpTableOffsets(jumpTables.size(), [&](size_t index) {
        return reserve(sizeof(int32_t), jumpTables[index].targets.size() * sizeof(int32_t));
    });

    Vector<uint8_t> image(FillWith { }, size, 0);
    std::span<uint8_t> bytes = image.mutableSpan();
    memcpySpan(bytes, code);
    for (size_t index = 0; index < constants.size(); ++index)
        memcpySpan(bytes.subspan(constantOffsets[index]), constants[index].bytes.span());
    for (size_t index = 0; index < jumpTables.size(); ++index) {
        size_t table = jumpTableOffsets[index];
        std::span<uint8_t> words = bytes.subspan(table);
        for (uint32_t target : jumpTables[index].targets) {
            AOT_TEST_CHECK(target < code.size());
            int64_t distance = static_cast<int64_t>(target) - static_cast<int64_t>(table);
            AOT_TEST_CHECK(isRepresentableAs<int32_t>(distance));
            WTF::unalignedStore<int32_t>(words.data(), static_cast<int32_t>(distance));
            words = words.subspan(sizeof(int32_t));
        }
    }

    auto resolve = [&](const Relocation& relocation) -> std::optional<size_t> {
        switch (relocation.space) {
        case SymbolSpace::Constant:
            if (relocation.symbol >= constantOffsets.size())
                failToLinkTestProcedure(relocation, "the procedure has no such constant"_s);
            return constantOffsets[relocation.symbol];
        case SymbolSpace::JumpTable:
            if (relocation.symbol >= jumpTableOffsets.size())
                failToLinkTestProcedure(relocation, "the procedure has no such jump table"_s);
            return jumpTableOffsets[relocation.symbol];
        case SymbolSpace::Function:
        case SymbolSpace::Stub:
        case SymbolSpace::ArgumentThunk:
        case SymbolSpace::Number:
        case SymbolSpace::InstanceWord:
        case SymbolSpace::RegExp:
        case SymbolSpace::WebAssembly:
        case SymbolSpace::FFI:
            failToLinkTestProcedure(relocation, "only the constants and the jump tables of the procedure have a place"_s);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto reportOutOfReach = [](const Relocation& relocation) {
        failToLinkTestProcedure(relocation, "the symbol is out of reach"_s);
    };
    uint64_t numberApplied = applyRelocations(bytes, 0, relocations, resolve, reportOutOfReach);
    AOT_TEST_CHECK(numberApplied == relocations.size());
    return image;
}

TestProcedure::TestProcedure(ProcedureTraits traits)
    : m_storage(makeUniqueWithoutFastMallocCheck<Storage>(traitsOfTestProcedure(traits)))
    , m_procedure(&m_storage->procedure)
    , m_out(&m_storage->output)
{
    m_out->appendTo(m_procedure->addEntrypoint(primaryEntry));
}

TestProcedure::~TestProcedure() = default;

const AOTHeaps& TestProcedure::heaps() const
{
    return heapsOfTestProcedures();
}

bool TestProcedure::prepare()
{
    if (!m_procedure->failure() && !m_storage->didPrepare) {
        m_storage->didPrepare = true;
        prepareForGeneration(*m_procedure);
        m_storage->didFinishPreparing = !m_procedure->failure();
    }
    return !m_procedure->failure();
}

bool TestProcedure::compile(void* instance, std::optional<uint32_t> branchReach)
{
    if (!prepare())
        return false;

    Storage& storage = *m_storage;
    Procedure& procedure = storage.procedure;
    CPUFeatureSet features = procedure.traits().features;
    AOT_TEST_CHECK(!storage.didGenerate);
    AOT_TEST_CHECK(features.isSubsetOf(CPUFeatureSet::host()));
    AOT_TEST_CHECK(ExecutableAllocator::singleton().isValid());
    storage.didGenerate = true;

    CCallHelpers jit;
    AssemblerPolicy policy = AssemblerPolicy::forImage(features);
    if (branchReach)
        policy.branchReach = *branchReach;
    RelocationRecorder recorder(jit, policy);
    readTagsFromInstance(jit);
    generate(procedure, jit, recorder);
    if (procedure.failure())
        return false;
    recorder.routeTransfersThroughEnd();
    emitPadding(jit);

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(Options::dumpDisassembly(), "AOT machine test"_s);
    std::expected<Vector<Relocation>, LinkFailure> relocations = recorder.link(buffer, nullptr);
    if (!relocations) {
        dataLogLn("The code of a test procedure does not link: ", describeLinkFailureOfTestProcedure(relocations.error()));
        failNativeTest(__FILE__, __LINE__, "the code of a test procedure links");
    }

    Vector<CodeEntry, 1> entries;
    describeEntries(procedure, buffer.linkBuffer(), entries);
    AOT_TEST_CHECK(!entries.isEmpty() && entries[0].id == primaryEntry && entries[0].offset < buffer.bytes().size());

    Vector<uint8_t> image = layOutTestProcedure(procedure, buffer, relocations->span());
    storage.code = ExecutableAllocator::singleton().allocate(image.size() + alignmentOfTestProcedureCode, JITCompilationMustSucceed);
    uint8_t* start = roundUpToMultipleOf<alignmentOfTestProcedureCode>(storage.code->start().untaggedPtr<uint8_t*>());
    performJITMemcpy<jitMemcpyRepatch>(start, image.span().data(), image.size());
    MacroAssembler::cacheFlush(start, image.size());

    storage.wrapper = compileTestWrapper(start + entries[0].offset, instance);
    storage.linkedCode = WTF::move(image);
    return true;
}

void* TestProcedure::entry() const
{
    AOT_TEST_CHECK(m_storage->code);
    return untagCFunctionPtr<JSEntryPtrTag>(m_storage->wrapper.code().taggedPtr());
}

std::span<const uint8_t> TestProcedure::linkedCode() const
{
    AOT_TEST_CHECK(m_storage->code);
    return m_storage->linkedCode.span();
}

String TestProcedure::printedProcedure() const
{
    return m_storage->printed(TestProcedureStage::Appended, printProcedure);
}

String TestProcedure::printedOrder() const
{
    return m_storage->printed(TestProcedureStage::Ordered, printOrder);
}

String TestProcedure::printedLiveness() const
{
    return m_storage->printed(TestProcedureStage::Scanned, printLiveness);
}

String TestProcedure::printedAllocation() const
{
    return m_storage->printed(TestProcedureStage::Allocated, printAllocation);
}

String TestProcedure::printedFrame() const
{
    return m_storage->printed(TestProcedureStage::Framed, printFrame);
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
