/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"

#include "AOTB3Policy.h"
#include "AOTInstance.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRelocationLowering.h"
#include "AOTStubRegisters.h"
#include "AirCode.h"
#include "B3BasicBlockInlines.h"
#include "B3CompilationScratch.h"
#include "B3Generate.h"
#include "B3InsertionSetInlines.h"
#include "B3PatchpointValue.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "GPRInfo.h"
#include "JSCJSValue.h"
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

static_assert(sizeOfNearCall == MacroAssembler::relocationSize(RelocationKind::Call));
static_assert(sizeOfJump == MacroAssembler::relocationSize(RelocationKind::Jump));

namespace {

using Address = CCallHelpers::Address;
using Label = CCallHelpers::Label;
using TrustedImm32 = CCallHelpers::TrustedImm32;
using TrustedImm64 = CCallHelpers::TrustedImm64;
using PointerTable = ImageRelocationLowering::PointerTable;

struct RecordedKind {
    RelocationKind kind;
    CodeSymbol symbol;
    void (*emit)(RelocationRecorder&, CodeSymbol);
};

class FakeInstance {
public:
    FakeInstance() { m_memory.fill(0, sizeInBytes / sizeof(uint64_t)); }

    uint8_t* address() { return reinterpret_cast<uint8_t*>(m_memory.mutableSpan().data()) + sizeInBytes / 2; }

    template<typename T>
    void set(ptrdiff_t offset, T value)
    {
        memcpy(address() + offset, &value, sizeof(T));
    }

private:
    static constexpr size_t sizeInBytes = 64 * KB;

    Vector<uint64_t> m_memory;
};

struct LoweringFixture {
    explicit LoweringFixture(PointerTable table)
        : recorder(jit, context.assemblerPolicy())
        , lowering(table, recorder)
        , proc(b3PolicyFor(context, FunctionBackEndTraits { .function = FunctionID(0u) }, lowering), &scratch)
    {
        Typed::pinRegisters(proc);
    }

    BuildContext context { wireOptions() };
    CCallHelpers jit;
    RelocationRecorder recorder;
    ImageRelocationLowering lowering;
    B3::CompilationScratch scratch;
    B3::Procedure proc;
};

struct LoweredProcedure {
    std::optional<B3::FailureKind> failure;
    Vector<uint8_t> bytes;
    Relocation callOfProcedure { };
    Vector<Relocation> relocations;
    uint32_t procedureOffset { 0 };
    bool hasFrameRanges { false };
};

} // anonymous namespace

static constexpr size_t sectionSize = 12 * KB;
static constexpr size_t unitOffset = 4 * KB;
static constexpr size_t targetOffset = 8 * KB + 64;
static constexpr size_t instanceOffset = 0x12348;
static constexpr int32_t distanceAddend = 12;
static constexpr CodeSymbol functionSymbol { SymbolSpace::Function, 7, 2 };
static constexpr CodeSymbol siteCacheSymbol { SymbolSpace::SiteCache, 9 };
static constexpr CodeSymbol procedureSymbol { SymbolSpace::Function, 0 };
static constexpr uint32_t slotOfFmod = 3;

static constexpr RecordedKind recordedKinds[] = {
    { RelocationKind::Call, functionSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.call(symbol); } },
    { RelocationKind::Jump, functionSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.jump(symbol); } },
    { RelocationKind::AddressNear, functionSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.moveAddress(symbol, GPRInfo::regT0, PCRelativeReach::Near); } },
    { RelocationKind::AddressFar, functionSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.moveAddress(symbol, GPRInfo::regT0, PCRelativeReach::Far); } },
    { RelocationKind::Load64, functionSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.load64(symbol, GPRInfo::regT0); } },
    { RelocationKind::Distance32, functionSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.emitDistance32(symbol, distanceAddend); } },
    { RelocationKind::InstanceOffset, siteCacheSymbol, [](RelocationRecorder& recorder, CodeSymbol symbol) { recorder.moveInstanceAddress(symbol, GPRInfo::regT1, GPRInfo::regT0); } },
};

template<typename Emit>
static std::expected<RelocatableCode, LinkFailure> emitRelocatable(const Emit& emit, BuildContext* context = nullptr)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, testAssemblerPolicy());
    emit(jit, recorder);
    return recorder.finalize(LinkBuffer::Profile::FTL, context);
}

template<typename Emit>
static Vector<uint8_t> emitBytes(const Emit& emit)
{
    auto code = emitRelocatable([&](CCallHelpers& jit, RelocationRecorder&) {
        emit(jit);
    });
    AOT_TEST_CHECK(code);
    AOT_TEST_CHECK(code->relocations.isEmpty());
    return WTF::move(code->bytes);
}

static void emitPrefix(CCallHelpers& jit)
{
    jit.move(TrustedImm32(1), GPRInfo::regT2);
}

static RelocatableCode emitAfterPrefix(const RecordedKind& recorded)
{
    auto code = emitRelocatable([&](CCallHelpers& jit, RelocationRecorder& recorder) {
        emitPrefix(jit);
        recorded.emit(recorder, recorded.symbol);
    });
    AOT_TEST_CHECK(code);
    return WTF::move(*code);
}

static size_t targetOf(RelocationKind kind)
{
    return kind == RelocationKind::InstanceOffset ? instanceOffset : targetOffset;
}

static Vector<uint8_t> placeInSection(std::span<const uint8_t> unit, std::span<const uint8_t> target)
{
    AOT_TEST_CHECK(unit.size() <= targetOffset - unitOffset);
    AOT_TEST_CHECK(target.size() <= sectionSize - targetOffset);
    Vector<uint8_t> section(FillWith { }, sectionSize, static_cast<uint8_t>(0));
    memcpySpan(section.mutableSpan().subspan(unitOffset), unit);
    if (!target.empty())
        memcpySpan(section.mutableSpan().subspan(targetOffset), target);
    return section;
}

static Vector<uint8_t> linkInSection(const RelocatableCode& unit, std::span<const uint8_t> target)
{
    AOT_TEST_CHECK(unit.relocations.size() == 1);
    Vector<uint8_t> section = placeInSection(unit.bytes.span(), target);
    AOT_TEST_CHECK(applyRelocation(section.mutableSpan(), unitOffset, unit.relocations[0], targetOf(unit.relocations[0].kind)));
    return section;
}

static Vector<uint8_t> returns42()
{
    return emitBytes([](CCallHelpers& jit) {
        jit.move(TrustedImm32(42), GPRInfo::returnValueGPR);
        jit.ret();
    });
}

static std::expected<RelocatableCode, LinkFailure> emitIntegerMove(int64_t value, BuildContext* context = nullptr)
{
    return emitRelocatable([value](CCallHelpers& jit, RelocationRecorder&) {
        jit.move(TrustedImm64(value), GPRInfo::regT0);
        jit.ret();
    }, context);
}

static void testProcessAddressFails()
{
    int local = 0;
    auto code = emitRelocatable([&local](CCallHelpers& jit, RelocationRecorder&) {
        jit.move(CCallHelpers::TrustedImmPtr(&local), GPRInfo::regT0);
        jit.ret();
    });
    AOT_TEST_CHECK(!code);
    AOT_TEST_CHECK(code.error() == LinkFailure::ProcessAddressInCode);
}

static void testIntegerPasses()
{
    int local = 0;
    AOT_TEST_CHECK(emitIntegerMove(reinterpret_cast<intptr_t>(&local)));
    AOT_TEST_CHECK(emitIntegerMove(0x123456789abc));
    AOT_TEST_CHECK(emitIntegerMove(1099511627776));
}

static void testValidationFindsMappedAddress()
{
    int local = 0;
    BuildContext context(wireOptions(TestOptions { .checks = Wire::CheckSet::Validate }));
    AOT_TEST_CHECK(emitIntegerMove(reinterpret_cast<intptr_t>(&local), &context));
    AOT_TEST_CHECK(count(context, InternalErrorKind::ProcessAddressInImageCode) == mappedIntegersAreAddresses);
}

static void testValidationPassesTags()
{
    BuildContext context(wireOptions(TestOptions { .checks = Wire::CheckSet::Validate }));
    AOT_TEST_CHECK(emitIntegerMove(JSValue::NumberTag, &context));
    AOT_TEST_CHECK(emitIntegerMove(JSValue::NotCellMask, &context));
    AOT_TEST_CHECK(!count(context, InternalErrorKind::ProcessAddressInImageCode));
}

static void testEmptyAssembler()
{
    auto code = emitRelocatable([](CCallHelpers&, RelocationRecorder&) { });
    AOT_TEST_CHECK(code);
    AOT_TEST_CHECK(code->bytes.isEmpty());
    AOT_TEST_CHECK(code->relocations.isEmpty());
}

static void testBytesEndAtLastInstruction()
{
    size_t sizeBeforeLink = 0;
    auto code = emitRelocatable([&](CCallHelpers& jit, RelocationRecorder&) {
        CCallHelpers::Jump skip = jit.branchTest32(CCallHelpers::NonZero, GPRInfo::argumentGPR0);
        jit.move(TrustedImm32(1), GPRInfo::returnValueGPR);
        skip.link(&jit);
        jit.ret();
        sizeBeforeLink = jit.m_assembler.codeSize();
    });
    AOT_TEST_CHECK(code);
    const Vector<uint8_t>& returnToCaller = testTemplates().returnToCaller;
    AOT_TEST_CHECK(code->bytes.size() <= sizeBeforeLink);
    AOT_TEST_CHECK(code->bytes.size() > returnToCaller.size());
    AOT_TEST_CHECK(equalSpans(code->bytes.span().last(returnToCaller.size()), returnToCaller.span()));
}

static void testRecordsAscend()
{
    constexpr CodeSymbol constant { SymbolSpace::Constant, 7 };
    constexpr CodeSymbol jumpTable { SymbolSpace::JumpTable, 3 };
    auto code = emitRelocatable([&](CCallHelpers& jit, RelocationRecorder& recorder) {
        recorder.moveAddress(constant, GPRInfo::regT0, PCRelativeReach::Near);
        recorder.moveAddress(jumpTable, GPRInfo::regT1, PCRelativeReach::Far);
        jit.ret();
    });
    AOT_TEST_CHECK(code);
    AOT_TEST_CHECK(code->relocations.size() == 2);
    const Relocation& first = code->relocations[0];
    const Relocation& second = code->relocations[1];
    AOT_TEST_CHECK(first.offset < second.offset);
    AOT_TEST_CHECK(first.codeSymbol() == constant);
    AOT_TEST_CHECK(first.kind == RelocationKind::AddressNear);
    AOT_TEST_CHECK(second.codeSymbol() == jumpTable);
    AOT_TEST_CHECK(second.kind == RelocationKind::AddressFar);
}

static void testKindsAndSymbols()
{
    size_t prefixSize = emitBytes(emitPrefix).size();
    for (const RecordedKind& recorded : recordedKinds) {
        RelocatableCode code = emitAfterPrefix(recorded);
        AOT_TEST_CHECK(code.relocations.size() == 1);
        const Relocation& relocation = code.relocations[0];
        AOT_TEST_CHECK(relocation.kind == recorded.kind);
        AOT_TEST_CHECK(relocation.codeSymbol() == recorded.symbol);
        AOT_TEST_CHECK(relocation.offset == prefixSize);
        AOT_TEST_CHECK(relocation.offset + MacroAssembler::relocationSize(recorded.kind) <= code.bytes.size());
    }
}

static void testFieldIsCleared()
{
    for (const RecordedKind& recorded : recordedKinds) {
        if (recorded.kind == RelocationKind::Distance32)
            continue;
        RelocatableCode code = emitAfterPrefix(recorded);
        Vector<uint8_t> section = linkInSection(code, { });
        std::span<uint8_t> linked = section.mutableSpan().subspan(unitOffset, code.bytes.size());
        AOT_TEST_CHECK(!equalSpans(std::span<const uint8_t>(linked), code.bytes.span()));
        MacroAssembler::clearRelocation(recorded.kind, linked.subspan(code.relocations[0].offset, MacroAssembler::relocationSize(recorded.kind)));
        AOT_TEST_CHECK(equalSpans(std::span<const uint8_t>(linked), code.bytes.span()));
    }
}

static void testCallReturnsItsIndex()
{
    constexpr CodeSymbol callee { SymbolSpace::Stub, 4 };
    constexpr CodeSymbol target { SymbolSpace::ArgumentThunk, 5 };
    uint32_t call = 0;
    uint32_t jump = 0;
    auto code = emitRelocatable([&](CCallHelpers&, RelocationRecorder& recorder) {
        AOT_TEST_CHECK(!recorder.numberOfRelocations());
        recorder.moveAddress(functionSymbol, GPRInfo::regT0, PCRelativeReach::Far);
        call = recorder.call(callee);
        recorder.load64(functionSymbol, GPRInfo::regT0);
        jump = recorder.jump(target);
        AOT_TEST_CHECK(recorder.numberOfRelocations() == 4);
    });
    AOT_TEST_CHECK(code);
    AOT_TEST_CHECK(code->relocations.size() == 4);
    AOT_TEST_CHECK(call == 1);
    AOT_TEST_CHECK(code->relocations[call].kind == RelocationKind::Call);
    AOT_TEST_CHECK(code->relocations[call].codeSymbol() == callee);
    AOT_TEST_CHECK(jump == 3);
    AOT_TEST_CHECK(code->relocations[jump].kind == RelocationKind::Jump);
    AOT_TEST_CHECK(code->relocations[jump].codeSymbol() == target);
}

static void testDistance32()
{
    auto unit = emitRelocatable([](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.emitDistance32(functionSymbol, distanceAddend);
    });
    AOT_TEST_CHECK(unit);
    AOT_TEST_CHECK(unit->bytes.size() == sizeof(int32_t));
    AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(unit->bytes.span().data()) == distanceAddend);
    Vector<uint8_t> section = linkInSection(*unit, { });
    AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(section.span().subspan(unitOffset).data()) == static_cast<int32_t>(targetOffset - unitOffset) + distanceAddend);
}

static void testApplyAllCounts()
{
    auto unit = emitRelocatable([](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.emitDistance32({ SymbolSpace::Function, 0 }, 0);
        recorder.emitDistance32({ SymbolSpace::Function, 1 }, 0);
        recorder.emitDistance32({ SymbolSpace::Function, 2 }, 0);
    });
    AOT_TEST_CHECK(unit);
    Vector<uint8_t> section = placeInSection(unit->bytes.span(), { });
    unsigned numberOutOfReach = 0;
    uint64_t numberApplied = applyRelocations(section.mutableSpan(), unitOffset, unit->relocations.span(), [](const Relocation& relocation) -> std::optional<size_t> {
        if (relocation.symbol == 1)
            return std::nullopt;
        return targetOffset;
    }, [&](const Relocation&) {
        ++numberOutOfReach;
    });
    AOT_TEST_CHECK(numberApplied == 2);
    AOT_TEST_CHECK(!numberOutOfReach);
    for (const Relocation& relocation : unit->relocations) {
        int32_t expected = relocation.symbol == 1 ? 0 : static_cast<int32_t>(targetOffset - unitOffset - relocation.offset);
        AOT_TEST_CHECK(WTF::unalignedLoad<int32_t>(section.span().subspan(unitOffset + relocation.offset).data()) == expected);
    }
}

template<PCRelativeReach reach>
static void testMoveAddress()
{
    auto unit = emitRelocatable([](CCallHelpers& jit, RelocationRecorder& recorder) {
        recorder.moveAddress(functionSymbol, GPRInfo::returnValueGPR, reach);
        jit.ret();
    });
    AOT_TEST_CHECK(unit);
    Vector<uint8_t> section = linkInSection(*unit, { });
    ExecutableCopy copy(section.span());
    AOT_TEST_CHECK(copy.call<uintptr_t>(unitOffset) == reinterpret_cast<uintptr_t>(copy.start() + targetOffset));
}

template<typename Result, typename Emit, typename... Arguments>
static Result runRelocatable(const Emit& emit, std::span<const uint8_t> target, Arguments... arguments)
{
    auto unit = emitRelocatable(emit);
    AOT_TEST_CHECK(unit);
    Vector<uint8_t> section = linkInSection(*unit, target);
    return ExecutableCopy(section.span()).call<Result>(unitOffset, arguments...);
}

static void testLoad64()
{
    constexpr uint64_t word = 0x1122334455667788;
    AOT_TEST_CHECK(word == runRelocatable<uint64_t>([](CCallHelpers& jit, RelocationRecorder& recorder) {
        recorder.load64(functionSymbol, GPRInfo::returnValueGPR);
        jit.ret();
    }, asByteSpan(word)));
}

static void testCall()
{
    Vector<uint8_t> target = returns42();
    AOT_TEST_CHECK(42 == runRelocatable<uint32_t>([](CCallHelpers& jit, RelocationRecorder& recorder) {
        jit.emitFunctionPrologue();
        recorder.call(functionSymbol);
        jit.emitFunctionEpilogue();
        jit.ret();
    }, target.span()));
}

static void testJump()
{
    Vector<uint8_t> target = returns42();
    AOT_TEST_CHECK(42 == runRelocatable<uint32_t>([](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.jump(functionSymbol);
    }, target.span()));
}

static void testInstanceOffset()
{
    constexpr uintptr_t instance = 0x7000000;
    AOT_TEST_CHECK(instance + instanceOffset == runRelocatable<uintptr_t>([](CCallHelpers& jit, RelocationRecorder& recorder) {
        recorder.moveInstanceAddress(siteCacheSymbol, GPRInfo::argumentGPR0, GPRInfo::argumentGPR1);
        jit.move(GPRInfo::argumentGPR1, GPRInfo::returnValueGPR);
        jit.ret();
    }, { }, instance));
}

#if CPU(ARM64)
static void testOutOfReach()
{
    auto unit = emitRelocatable([](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.moveAddress(functionSymbol, GPRInfo::regT0, PCRelativeReach::Near);
    });
    AOT_TEST_CHECK(unit);
    AOT_TEST_CHECK(unit->relocations.size() == 1);
    Vector<uint8_t> section;
    section.fill(0, 4 * MB);
    memcpySpan(section.mutableSubspan(0, unit->bytes.size()), unit->bytes.span());
    Vector<uint8_t> before = section;
    AOT_TEST_CHECK(!applyRelocation(section.mutableSpan(), 0, unit->relocations[0], 2 * MB + 64));
    AOT_TEST_CHECK(section == before);

    unsigned numberOutOfReach = 0;
    uint64_t numberApplied = applyRelocations(section.mutableSpan(), 0, unit->relocations.span(), [](const Relocation&) -> std::optional<size_t> {
        return 2 * MB + 64;
    }, [&](const Relocation& relocation) {
        AOT_TEST_CHECK(relocation == unit->relocations[0]);
        ++numberOutOfReach;
    });
    AOT_TEST_CHECK(!numberApplied);
    AOT_TEST_CHECK(numberOutOfReach == 1);
    AOT_TEST_CHECK(section == before);
}
#endif

static Vector<GPRReg, 2> registersSavedAroundProcedure()
{
    Vector<GPRReg, 2> registers { instanceGPR };
    for (GPRReg reg : unsavedCalleeSaveGPRs)
        registers.append(reg);
    return registers;
}

static void emitCallWithInstance(CCallHelpers& jit, RelocationRecorder& recorder)
{
    Vector<GPRReg, 2> saved = registersSavedAroundProcedure();
    auto slotOf = [](size_t index) {
        return Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
    };
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(static_cast<int32_t>(WTF::roundUpToMultipleOf<16>(saved.size() * sizeof(CPURegister)))), CCallHelpers::stackPointerRegister);
    for (size_t index = 0; index < saved.size(); ++index)
        jit.storePtr(saved[index], slotOf(index));
    jit.move(GPRInfo::argumentGPR0, instanceGPR);
    recorder.call(procedureSymbol);
    for (size_t index = 0; index < saved.size(); ++index)
        jit.loadPtr(slotOf(index), saved[index]);
    jit.emitFunctionEpilogue();
    jit.ret();
}

template<typename AppendResult>
static LoweredProcedure compileProcedure(PointerTable table, const AppendResult& appendResult)
{
    LoweringFixture fixture(table);
    B3::Procedure& proc = fixture.proc;
    B3::BasicBlock* block = proc.addBlock();
    B3::Value* result = appendResult(proc, block);
    block->appendNew<B3::Value>(proc, B3::Return, B3::Origin(), result);
    B3::prepareForGeneration(proc);
    LoweredProcedure lowered;
    if (proc.report().failure) {
        lowered.failure = proc.report().failure->kind;
        return lowered;
    }

    CCallHelpers& jit = fixture.jit;
    emitCallWithInstance(jit, fixture.recorder);
    Label procedure = jit.label();
    B3::generate(proc, jit);
    AOT_TEST_CHECK(!proc.report().failure);

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::FTL);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "AOT relocation lowering test"_s);
    auto relocations = fixture.recorder.link(buffer, nullptr);
    AOT_TEST_CHECK(relocations);
    AOT_TEST_CHECK(!relocations->isEmpty());
    lowered.bytes.append(buffer.bytes());
    lowered.callOfProcedure = relocations->first();
    lowered.relocations.append(relocations->span().subspan(1));
    lowered.procedureOffset = buffer.linkBuffer().offsetOf(procedure);
    lowered.hasFrameRanges = !proc.code().frameRanges().isEmpty();
    AOT_TEST_CHECK(lowered.callOfProcedure.codeSymbol() == procedureSymbol);
    return lowered;
}

static LoweredProcedure compileRelocatable(PointerTable table, B3::RelocationSymbol symbol, int64_t addend)
{
    return compileProcedure(table, [&](B3::Procedure& proc, B3::BasicBlock* block) -> B3::Value* {
        return block->appendNew<B3::RelocatableValue>(proc, B3::Origin(), symbol, addend);
    });
}

static B3::Value* appendPatchpointThatNeedsFrame(B3::Procedure& proc, B3::BasicBlock* block)
{
    B3::PatchpointValue* patchpoint = block->appendNew<B3::PatchpointValue>(proc, B3::pointerType(), B3::Origin());
    patchpoint->effects = B3::Effects::none();
    patchpoint->resultConstraints = { B3::ValueRep::SomeRegister };
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.move(TrustedImm32(0), params[0].gpr());
    });
    return patchpoint;
}

static Vector<uint8_t> linkProcedure(const LoweredProcedure& lowered)
{
    AOT_TEST_CHECK(!lowered.failure);
    Vector<uint8_t> section = placeInSection(lowered.bytes.span(), { });
    AOT_TEST_CHECK(applyRelocation(section.mutableSpan(), unitOffset, lowered.callOfProcedure, unitOffset + lowered.procedureOffset));
    for (const Relocation& relocation : lowered.relocations)
        AOT_TEST_CHECK(applyRelocation(section.mutableSpan(), unitOffset, relocation, targetOf(relocation.kind)));
    return section;
}

static uint64_t runWithInstance(const LoweredProcedure& lowered, FakeInstance& instance)
{
    Vector<uint8_t> section = linkProcedure(lowered);
    return ExecutableCopy(section.span()).call<uint64_t>(unitOffset, static_cast<void*>(instance.address()));
}

static B3::RelocationSymbol symbolIn(RelocationSpace space, uint32_t index)
{
    return { static_cast<uint32_t>(space), index };
}

static B3::RelocationSymbol symbolOf(B3::RuntimeFunction function)
{
    return symbolIn(RelocationSpace::B3RuntimeFunction, static_cast<uint32_t>(function));
}

static std::optional<uint32_t> slotOfFmodAlone(B3::RuntimeFunction function)
{
    if (function == B3::RuntimeFunction::FmodDouble)
        return slotOfFmod;
    return std::nullopt;
}

static PointerTable runtimeTableWithFmod()
{
    PointerTable table = ImageRelocationLowering::instanceRuntimeTable();
    table.runtimeFunctionSlot = slotOfFmodAlone;
    return table;
}

static PointerTable pointerTableAt(int32_t offset)
{
    return { RelocationSpace::WebAssemblyPointers, instanceGPR, offset, false };
}

static void testRuntimeFunctionSlot()
{
    Vector<uint64_t> table;
    table.fill(0, 8);
    table[slotOfFmod] = 0x1234;
    FakeInstance instance;
    instance.set(AOT_INSTANCE_OFFSET(runtimeTable), table.mutableSpan().data());
    B3::RelocationSymbol symbol = symbolOf(B3::RuntimeFunction::FmodDouble);
    AOT_TEST_CHECK(runWithInstance(compileRelocatable(runtimeTableWithFmod(), symbol, 0), instance) == 0x1234);
    AOT_TEST_CHECK(runWithInstance(compileRelocatable(runtimeTableWithFmod(), symbol, 8), instance) == 0x123c);
}

static void testTableAtOffset()
{
    FakeInstance instance;
    instance.set<uint64_t>(80, 77);
    LoweredProcedure lowered = compileRelocatable(pointerTableAt(64), symbolIn(RelocationSpace::WebAssemblyPointers, 2), 0);
    AOT_TEST_CHECK(lowered.relocations.isEmpty());
    AOT_TEST_CHECK(runWithInstance(lowered, instance) == 77);
}

static void testRuntimeFunctionInTableAtOffset()
{
    FakeInstance instance;
    instance.set<uint64_t>(64 + slotOfFmod * sizeof(void*), 78);
    PointerTable table = pointerTableAt(64);
    table.runtimeFunctionSlot = slotOfFmodAlone;
    AOT_TEST_CHECK(runWithInstance(compileRelocatable(table, symbolOf(B3::RuntimeFunction::FmodDouble), 0), instance) == 78);
}

static LoweredProcedure compileAddress(PointerTable table, RelocationSpace space, uint32_t index, int64_t addend, RelocationKind kind, SymbolSpace symbolSpace)
{
    LoweredProcedure lowered = compileRelocatable(table, symbolIn(space, index), addend);
    AOT_TEST_CHECK(!lowered.failure);
    AOT_TEST_CHECK(lowered.relocations.size() == 1);
    AOT_TEST_CHECK(lowered.relocations[0].kind == kind);
    AOT_TEST_CHECK(lowered.relocations[0].codeSymbol() == (CodeSymbol { symbolSpace, index }));
    AOT_TEST_CHECK(lowered.relocations[0].offset >= lowered.procedureOffset);
    return lowered;
}

static void testSiteCacheRelocation()
{
    compileAddress(ImageRelocationLowering::instanceRuntimeTable(), RelocationSpace::SiteCaches, 5, 0, RelocationKind::InstanceOffset, SymbolSpace::SiteCache);
}

static void testDataRelocations()
{
    for (PointerTable table : { ImageRelocationLowering::instanceRuntimeTable(), pointerTableAt(0) }) {
        compileAddress(table, RelocationSpace::Constants, 5, 0, RelocationKind::AddressFar, SymbolSpace::Constant);
        compileAddress(table, RelocationSpace::JumpTables, 6, 0, RelocationKind::AddressFar, SymbolSpace::JumpTable);
    }
}

static void testSiteCacheAddress()
{
    FakeInstance instance;
    for (int64_t addend : { 0, 8 }) {
        LoweredProcedure lowered = compileAddress(ImageRelocationLowering::instanceRuntimeTable(), RelocationSpace::SiteCaches, 5, addend, RelocationKind::InstanceOffset, SymbolSpace::SiteCache);
        AOT_TEST_CHECK(runWithInstance(lowered, instance) == reinterpret_cast<uint64_t>(instance.address()) + instanceOffset + addend);
    }
}

static void testDataAddress()
{
    FakeInstance instance;
    for (int64_t addend : { 0, -8 }) {
        LoweredProcedure lowered = compileAddress(ImageRelocationLowering::instanceRuntimeTable(), RelocationSpace::Constants, 5, addend, RelocationKind::AddressFar, SymbolSpace::Constant);
        Vector<uint8_t> section = linkProcedure(lowered);
        ExecutableCopy copy(section.span());
        AOT_TEST_CHECK(copy.call<uint64_t>(unitOffset, static_cast<void*>(instance.address())) == reinterpret_cast<uint64_t>(copy.start() + targetOffset) + addend);
    }
}

static void testDeclined()
{
    PointerTable instanceTable = ImageRelocationLowering::instanceRuntimeTable();
    const std::pair<PointerTable, B3::RelocationSymbol> cases[] = {
        { instanceTable, { 17, 0 } },
        { instanceTable, { static_cast<uint32_t>(RelocationSpace::JumpTables) + 1, 0 } },
        { instanceTable, symbolIn(RelocationSpace::WebAssemblyPointers, 0) },
        { instanceTable, symbolOf(B3::RuntimeFunction::FmodDouble) },
        { runtimeTableWithFmod(), symbolOf(B3::RuntimeFunction::CeilDouble) },
        { runtimeTableWithFmod(), symbolIn(RelocationSpace::B3RuntimeFunction, B3::numberOfRuntimeFunctions) },
        { pointerTableAt(0), symbolIn(RelocationSpace::WebAssemblyPointers, 1u << 28) },
        { pointerTableAt(0), symbolIn(RelocationSpace::SiteCaches, 0) },
    };
    for (const auto& [table, symbol] : cases)
        AOT_TEST_CHECK(compileRelocatable(table, symbol, 0).failure == B3::FailureKind::RelocationNotLowered);
}

static B3::Value* lowerAlone(LoweringFixture& fixture, B3::RelocationSymbol symbol)
{
    B3::Procedure& proc = fixture.proc;
    B3::BasicBlock* block = proc.addBlock();
    B3::RelocatableValue* relocatable = block->appendNew<B3::RelocatableValue>(proc, B3::Origin(), symbol, 0);
    B3::InsertionSet insertionSet(proc);
    B3::Value* lowered = fixture.lowering.lower(proc, insertionSet, 0, *relocatable);
    insertionSet.execute(block);
    return lowered;
}

static void testReturnsNull()
{
    LoweringFixture functionCode(ImageRelocationLowering::instanceRuntimeTable());
    AOT_TEST_CHECK(!lowerAlone(functionCode, { 17, 0 }));
    AOT_TEST_CHECK(lowerAlone(functionCode, symbolIn(RelocationSpace::SiteCaches, 0)));
    LoweringFixture webAssemblyCode(pointerTableAt(0));
    AOT_TEST_CHECK(!lowerAlone(webAssemblyCode, { 17, 0 }));
    AOT_TEST_CHECK(!lowerAlone(webAssemblyCode, symbolIn(RelocationSpace::SiteCaches, 0)));
}

static void testAddressesRunWithoutFrame()
{
    AOT_TEST_CHECK(compileProcedure(ImageRelocationLowering::instanceRuntimeTable(), appendPatchpointThatNeedsFrame).hasFrameRanges);
    for (RelocationSpace space : { RelocationSpace::SiteCaches, RelocationSpace::Constants, RelocationSpace::JumpTables }) {
        LoweringFixture fixture(ImageRelocationLowering::instanceRuntimeTable());
        B3::Value* lowered = lowerAlone(fixture, symbolIn(space, 1));
        AOT_TEST_CHECK(lowered);
        B3::PatchpointValue* patchpoint = lowered->as<B3::PatchpointValue>();
        AOT_TEST_CHECK(patchpoint);
        AOT_TEST_CHECK(patchpoint->runsWithoutFrame());

        LoweredProcedure leaf = compileRelocatable(ImageRelocationLowering::instanceRuntimeTable(), symbolIn(space, 1), 0);
        AOT_TEST_CHECK(!leaf.failure);
        AOT_TEST_CHECK(leaf.relocations.size() == 1);
        AOT_TEST_CHECK(!leaf.hasFrameRanges);
    }
}

static void testTemplateSizes()
{
    const CodeTemplates& templates = testTemplates();
    AOT_TEST_CHECK(templates.nearCall.size() == sizeOfNearCall);
    AOT_TEST_CHECK(templates.nearJump.size() == sizeOfJump);
    AOT_TEST_CHECK(templates.breakpoint.size() == codeOffsetUnit);
    AOT_TEST_CHECK(!templates.returnToCaller.isEmpty());
    AOT_TEST_CHECK(!(templates.returnToCaller.size() % codeOffsetUnit));
    AOT_TEST_CHECK(templates.sizeOfVeneer() == templates.veneer.bytes.size());
    AOT_TEST_CHECK(templates.sizeOfVeneer());
    AOT_TEST_CHECK(!(templates.sizeOfVeneer() % sizeof(uint32_t)));
    AOT_TEST_CHECK(MacroAssembler::isBreakpoint(const_cast<uint8_t*>(templates.breakpoint.span().data())));
}

static void testTemplateRelocations()
{
    const CodeTemplates& templates = testTemplates();
    AOT_TEST_CHECK(templates.veneer.relocations.size() == 1);
    const Relocation& relocation = templates.veneer.relocations[0];
    AOT_TEST_CHECK(!relocation.offset);
    AOT_TEST_CHECK(relocation.kind == (isARM64() ? RelocationKind::AddressFar : RelocationKind::Jump));
    AOT_TEST_CHECK(MacroAssembler::relocationSize(relocation.kind) <= templates.sizeOfVeneer());

    const std::pair<const Vector<uint8_t>&, RelocationKind> transfers[] = {
        { templates.nearCall, RelocationKind::Call },
        { templates.nearJump, RelocationKind::Jump },
    };
    for (const auto& [bytes, kind] : transfers) {
        Vector<uint8_t> cleared = bytes;
        MacroAssembler::clearRelocation(kind, cleared.mutableSpan());
        AOT_TEST_CHECK(cleared == bytes);
    }
}

static void testTemplatesAreEqualForEqualPolicies()
{
    auto templates = CodeTemplates::create(testAssemblerPolicy(), nullptr);
    AOT_TEST_CHECK(templates);
    const CodeTemplates& others = testTemplates();
    AOT_TEST_CHECK(templates->nearCall == others.nearCall);
    AOT_TEST_CHECK(templates->nearJump == others.nearJump);
    AOT_TEST_CHECK(templates->returnToCaller == others.returnToCaller);
    AOT_TEST_CHECK(templates->breakpoint == others.breakpoint);
    AOT_TEST_CHECK(templates->veneer.bytes == others.veneer.bytes);
    AOT_TEST_CHECK(templates->veneer.relocations == others.veneer.relocations);
}

static void testNearCallAndReturn()
{
    const CodeTemplates& templates = testTemplates();
    Vector<uint8_t> caller = emitBytes([](CCallHelpers& jit) {
        jit.emitFunctionPrologue();
    });
    uint32_t callOffset = static_cast<uint32_t>(caller.size());
    caller.appendVector(templates.nearCall);
    caller.appendVector(emitBytes([](CCallHelpers& jit) {
        jit.emitFunctionEpilogue();
        jit.ret();
    }));
    Vector<uint8_t> callee = emitBytes([](CCallHelpers& jit) {
        jit.move(TrustedImm32(42), GPRInfo::returnValueGPR);
    });
    callee.appendVector(templates.returnToCaller);

    RelocatableCode unit { WTF::move(caller), { Relocation { callOffset, RelocationKind::Call, SymbolSpace::Function, primaryEntry, 0 } } };
    Vector<uint8_t> section = linkInSection(unit, callee.span());
    AOT_TEST_CHECK(ExecutableCopy(section.span()).call<uint32_t>(unitOffset) == 42);
}

static void testNearJump()
{
    Vector<uint8_t> target = returns42();
    RelocatableCode unit { testTemplates().nearJump, { Relocation { 0, RelocationKind::Jump, SymbolSpace::Function, primaryEntry, 0 } } };
    Vector<uint8_t> section = linkInSection(unit, target.span());
    AOT_TEST_CHECK(ExecutableCopy(section.span()).call<uint32_t>(unitOffset) == 42);
}

static void testVeneerJumpsToTarget()
{
    Vector<uint8_t> target = returns42();
    Vector<uint8_t> section = linkInSection(testTemplates().veneer, target.span());
    AOT_TEST_CHECK(ExecutableCopy(section.span()).call<uint32_t>(unitOffset) == 42);
}

static void testNopsFallThrough()
{
    Vector<uint8_t> target = returns42();
    for (size_t size = codeOffsetUnit; size <= 16; size += codeOffsetUnit) {
        Vector<uint8_t> code(FillWith { }, size, static_cast<uint8_t>(0));
        fillWithNops(code.mutableSpan());
        code.appendVector(target);
        AOT_TEST_CHECK(ExecutableCopy(code.span()).call<uint32_t>(0) == 42);
    }
}

std::span<const NativeTest> emissionTests()
{
    static constexpr NativeTest tests[] = {
        { "image/relocation/process-address-fails"_s, testProcessAddressFails },
        { "image/relocation/integer-passes"_s, testIntegerPasses },
        { "image/relocation/validation-finds-mapped-address"_s, testValidationFindsMappedAddress },
        { "image/relocation/validation-passes-tags"_s, testValidationPassesTags },
        { "image/relocation/empty-assembler"_s, testEmptyAssembler },
        { "image/relocation/bytes-end-at-last-instruction"_s, testBytesEndAtLastInstruction },
        { "image/relocation/records-ascend"_s, testRecordsAscend },
        { "image/relocation/kinds-and-symbols"_s, testKindsAndSymbols },
        { "image/relocation/field-is-cleared"_s, testFieldIsCleared },
        { "image/relocation/call-returns-its-index"_s, testCallReturnsItsIndex },
        { "image/relocation/distance32"_s, testDistance32 },
        { "image/relocation/apply-all-counts"_s, testApplyAllCounts },
        { "image/relocation/move-address-near"_s, whenExecutable<testMoveAddress<PCRelativeReach::Near>> },
        { "image/relocation/move-address-far"_s, whenExecutable<testMoveAddress<PCRelativeReach::Far>> },
        { "image/relocation/load64"_s, whenExecutable<testLoad64> },
        { "image/relocation/call"_s, whenExecutable<testCall> },
        { "image/relocation/jump"_s, whenExecutable<testJump> },
        { "image/relocation/instance-offset"_s, whenExecutable<testInstanceOffset> },
#if CPU(ARM64)
        { "image/relocation/out-of-reach"_s, testOutOfReach },
#endif
        { "image/relocation-lowering/runtime-function-slot"_s, whenExecutable<testRuntimeFunctionSlot> },
        { "image/relocation-lowering/table-at-offset"_s, whenExecutable<testTableAtOffset> },
        { "image/relocation-lowering/runtime-function-in-table-at-offset"_s, whenExecutable<testRuntimeFunctionInTableAtOffset> },
        { "image/relocation-lowering/site-cache-relocation"_s, testSiteCacheRelocation },
        { "image/relocation-lowering/data-relocations"_s, testDataRelocations },
        { "image/relocation-lowering/site-cache-address"_s, whenExecutable<testSiteCacheAddress> },
        { "image/relocation-lowering/data-address"_s, whenExecutable<testDataAddress> },
        { "image/relocation-lowering/declined"_s, testDeclined },
        { "image/relocation-lowering/returns-null"_s, testReturnsNull },
        { "image/relocation-lowering/addresses-run-without-frame"_s, testAddressesRunWithoutFrame },
        { "image/templates/sizes"_s, testTemplateSizes },
        { "image/templates/relocations"_s, testTemplateRelocations },
        { "image/templates/equal-for-equal-policies"_s, testTemplatesAreEqualForEqualPolicies },
        { "image/templates/near-call-and-return"_s, whenExecutable<testNearCallAndReturn> },
        { "image/templates/near-jump"_s, whenExecutable<testNearJump> },
        { "image/templates/veneer-jumps-to-target"_s, whenExecutable<testVeneerJumpsToTarget> },
        { "image/templates/nops-fall-through"_s, whenExecutable<testNopsFallThrough> },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
