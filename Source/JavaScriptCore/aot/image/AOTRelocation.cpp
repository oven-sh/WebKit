/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelocation.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeTemplates.h"
#include "AOTConvention.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTRelocationLowering.h"
#include "B3ArgumentRegValue.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"

namespace JSC { namespace AOT {

static_assert(sizeOfNearCall == MacroAssembler::relocationSize(RelocationKind::Call) && sizeOfJump == MacroAssembler::relocationSize(RelocationKind::Jump));

ImageLinkBuffer::ImageLinkBuffer(CCallHelpers& jit, LinkBuffer::Profile profile)
    : m_end(jit.label())
    , m_storage(roundUpToMultipleOf<sizeof(uint32_t)>(std::max<size_t>(jit.m_assembler.codeSize(), sizeof(uint32_t))) / sizeof(uint32_t))
    , m_linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(m_storage.mutableSpan().data()), m_storage.sizeInBytes(), profile)
{
    RELEASE_ASSERT(jit.policy().recordsProcessAddresses);
}

void ImageLinkBuffer::finalize(bool dumpsDisassembly, ASCIILiteral name)
{
    RELEASE_ASSERT(!didFailToAllocate() && !m_didFinalize);
    FINALIZE_CODE_IF(dumpsDisassembly, m_linkBuffer, JITThunkPtrTag, nullptr, "%s", name.characters());
    m_didFinalize = true;
}

std::span<uint8_t> ImageLinkBuffer::bytes()
{
    RELEASE_ASSERT(m_didFinalize);
    // Not m_linkBuffer.size(): LinkBuffer fills the storage that jump shortening and branch compaction leave over with nops.
    return asMutableByteSpan(m_storage.mutableSpan()).first(m_linkBuffer.offsetOf(m_end));
}

void emitPadding(CCallHelpers& jit)
{
#if CPU(X86_64)
    constexpr uint8_t halt = 0xf4;
    jit.m_assembler.buffer().putByte(static_cast<int8_t>(halt));
#else
    jit.breakpoint();
#endif
}

RelocationRecorder::RelocationRecorder(CCallHelpers& jit, const AssemblerPolicy& policy)
    : m_jit(jit)
{
    RELEASE_ASSERT(policy.recordsProcessAddresses && !policy.blindsConstants);
    jit.setPolicy(policy);
}

uint32_t RelocationRecorder::append(CCallHelpers::Label site, RelocationKind kind, CodeSymbol symbol)
{
    uint32_t index = numberOfRelocations();
    m_pending.append({ site, kind, symbol });
    return index;
}

void RelocationRecorder::moveAddress(CodeSymbol symbol, GPRReg dest, PCRelativeReach reach)
{
    append(m_jit.movePCRelativeAddress(dest, reach), addressKindFor(reach), symbol);
}

void RelocationRecorder::load64(CodeSymbol symbol, GPRReg dest)
{
    append(m_jit.loadPCRelative64(dest), RelocationKind::Load64, symbol);
}

uint32_t RelocationRecorder::call(CodeSymbol symbol)
{
    CCallHelpers::Label site = m_jit.label();
    m_jit.nearCall();
    return append(site, RelocationKind::Call, symbol);
}

uint32_t RelocationRecorder::jump(CodeSymbol symbol)
{
    CCallHelpers::Label site = m_jit.label();
    m_jit.nearTailCall();
    return append(site, RelocationKind::Jump, symbol);
}

void RelocationRecorder::moveInstanceAddress(CodeSymbol symbol, GPRReg instance, GPRReg dest)
{
    RELEASE_ASSERT(symbol.space == SymbolSpace::SiteCache);
    append(m_jit.moveInstanceAddress(instance, dest), RelocationKind::InstanceOffset, symbol);
}

void RelocationRecorder::emitDistance32(CodeSymbol symbol, int32_t addend)
{
    CCallHelpers::Label site = m_jit.label();
    m_jit.m_assembler.buffer().putInt(addend);
    append(site, RelocationKind::Distance32, symbol);
}

std::expected<RelocatableCode, LinkFailure> RelocationRecorder::finalize(LinkBuffer::Profile profile, BuildContext* context)
{
    if (!m_jit.m_assembler.codeSize())
        return RelocatableCode { };

    ImageLinkBuffer buffer(m_jit, profile);
    if (buffer.didFailToAllocate())
        return makeUnexpected(LinkFailure::OutOfMemory);
    buffer.finalize(false, "AOT relocatable code"_s);

    auto relocations = link(buffer, context);
    if (!relocations)
        return makeUnexpected(relocations.error());
    return RelocatableCode { Vector<uint8_t>(buffer.bytes()), WTF::move(*relocations) };
}

std::expected<Vector<Relocation>, LinkFailure> RelocationRecorder::link(ImageLinkBuffer& buffer, BuildContext*)
{
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    if (linkBuffer.hasProcessAddress())
        return makeUnexpected(LinkFailure::ProcessAddressInCode);

    Vector<Relocation> relocations = m_pending.map([&](const PendingRelocation& pending) {
        return Relocation { linkBuffer.offsetOf(pending.site), pending.kind, pending.symbol.space, pending.symbol.entry, pending.symbol.index };
    });
    RELEASE_ASSERT(std::ranges::is_sorted(relocations, { }, &Relocation::offset));

    std::span<uint8_t> bytes = buffer.bytes();
    for (const Relocation& relocation : relocations)
        MacroAssembler::clearRelocation(relocation.kind, bytes.subspan(relocation.offset, MacroAssembler::relocationSize(relocation.kind)));

    m_pending.clear();
    return relocations;
}

static constexpr CodeSymbol placeholderSymbol { SymbolSpace::Function, 0 };

std::expected<CodeTemplates, LinkFailure> CodeTemplates::create(const AssemblerPolicy& policy, BuildContext* context)
{
    std::optional<LinkFailure> failure;
    auto emit = [&](const auto& emitCode) -> RelocatableCode {
        CCallHelpers jit;
        RelocationRecorder recorder(jit, policy);
        emitCode(jit, recorder);
        auto code = recorder.finalize(LinkBuffer::Profile::Thunk, context);
        if (code)
            return WTF::move(*code);
        if (!failure)
            failure = code.error();
        return { };
    };
    RelocatableCode nearCall = emit([](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.call(placeholderSymbol);
    });
    RelocatableCode nearJump = emit([](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.jump(placeholderSymbol);
    });
    RelocatableCode returnToCaller = emit([](CCallHelpers& jit, RelocationRecorder&) {
        jit.ret();
    });
    RelocatableCode breakpoint = emit([](CCallHelpers& jit, RelocationRecorder&) {
        emitPadding(jit);
    });
    RelocatableCode veneer = emit([]([[maybe_unused]] CCallHelpers& jit, RelocationRecorder& recorder) {
#if CPU(ARM64)
        recorder.moveAddress(placeholderSymbol, ARM64Registers::ip0, PCRelativeReach::Far);
        jit.farJump(ARM64Registers::ip0, NoPtrTag);
#else
        recorder.jump(placeholderSymbol);
#endif
    });
    if (failure)
        return std::unexpected(*failure);

    auto hasOneRelocationAtStart = [](const RelocatableCode& code) {
        return code.relocations.size() == 1 && !code.relocations[0].offset;
    };
    RELEASE_ASSERT(hasOneRelocationAtStart(nearCall) && nearCall.bytes.size() == sizeOfNearCall);
    RELEASE_ASSERT(hasOneRelocationAtStart(nearJump) && nearJump.bytes.size() == sizeOfJump);
    RELEASE_ASSERT(returnToCaller.relocations.isEmpty());
    RELEASE_ASSERT(breakpoint.relocations.isEmpty() && breakpoint.bytes.size() == codeOffsetUnit);
    RELEASE_ASSERT(hasOneRelocationAtStart(veneer));
    size_t sizeOfVeneer = veneer.bytes.size();
    veneer.bytes.grow(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(sizeOfVeneer));
    fillWithNops(veneer.bytes.mutableSpan().subspan(sizeOfVeneer));
    return CodeTemplates { WTF::move(nearCall.bytes), WTF::move(nearJump.bytes), WTF::move(returnToCaller.bytes), WTF::move(breakpoint.bytes), WTF::move(veneer) };
}

void CodeTemplates::fillWithBreakpoints(std::span<uint8_t> bytes) const
{
    RELEASE_ASSERT(!(bytes.size() % breakpoint.size()));
    if (breakpoint.size() == 1) {
        memsetSpan(bytes, breakpoint[0]);
        return;
    }
    for (; !bytes.empty(); bytes = bytes.subspan(breakpoint.size()))
        memcpySpan(bytes, breakpoint.span());
}

void fillWithNops(std::span<uint8_t> bytes)
{
#if CPU(ARM64)
    RELEASE_ASSERT(!(bytes.size() % sizeof(uint32_t)));
    ARM64Assembler::fillNops<memcpyRepatch>(bytes.data(), bytes.size());
#else
    X86Assembler::fillNops(bytes.data(), bytes.size());
#endif
}

static_assert(static_cast<uint32_t>(RelocationSpace::B3RuntimeFunction) == B3::runtimeFunctionSpace);

static constexpr int64_t slotSize = sizeof(void*);

static B3::Value* insertImmutableLoad(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, B3::Value* base, int32_t offset)
{
    B3::MemoryValue* load = insertionSet.insert<B3::MemoryValue>(index, B3::Load, B3::pointerType(), origin, base, offset);
    load->setControlDependent(false);
    load->setReadsMutability(B3::Mutability::Immutable);
    return load;
}

static B3::Value* insertAddPtr(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, B3::Value* base, int64_t offset)
{
    if (!offset)
        return base;
    B3::Value* constant = insertionSet.insertIntConstant(index, origin, B3::pointerType(), offset);
    return insertionSet.insert<B3::Value>(index, B3::Add, origin, base, constant);
}

auto ImageRelocationLowering::instanceRuntimeTable() -> PointerTable
{
    return {
        .space = RelocationSpace::B3RuntimeFunction,
        .base = instanceGPR,
        .offset = static_cast<int32_t>(AOT_INSTANCE_OFFSET(runtimeTable)),
        .offsetHoldsTablePointer = true,
        .runtimeFunctionSlot = nullptr,
    };
}

ImageRelocationLowering::ImageRelocationLowering(PointerTable table, RelocationRecorder& relocations)
    : m_table(table)
    , m_relocations(relocations)
{
    RELEASE_ASSERT(table.space == RelocationSpace::B3RuntimeFunction || table.space == RelocationSpace::WebAssemblyPointers);
}

B3::Value* ImageRelocationLowering::lower(B3::Procedure&, B3::InsertionSet& insertionSet, size_t index, const B3::RelocatableValue& value)
{
    B3::RelocationSymbol symbol = value.symbol();
    B3::Origin origin = value.origin();
    if (symbol.space > static_cast<uint32_t>(RelocationSpace::JumpTables))
        return nullptr;

    RelocationSpace space = static_cast<RelocationSpace>(symbol.space);
    B3::Value* address = nullptr;
    switch (space) {
    case RelocationSpace::B3RuntimeFunction: {
        if (!m_table.runtimeFunctionSlot || symbol.index >= B3::numberOfRuntimeFunctions)
            return nullptr;
        std::optional<uint32_t> slot = m_table.runtimeFunctionSlot(static_cast<B3::RuntimeFunction>(symbol.index));
        if (!slot)
            return nullptr;
        address = loadSlot(insertionSet, index, origin, *slot);
        break;
    }
    case RelocationSpace::WebAssemblyPointers:
        if (space != m_table.space)
            return nullptr;
        address = loadSlot(insertionSet, index, origin, symbol.index);
        break;
    case RelocationSpace::SiteCaches:
        if (m_table.space != RelocationSpace::B3RuntimeFunction)
            return nullptr;
        address = insertAddressOf(insertionSet, index, origin, { SymbolSpace::SiteCache, symbol.index });
        break;
    case RelocationSpace::Constants:
        address = insertAddressOf(insertionSet, index, origin, { SymbolSpace::Constant, symbol.index });
        break;
    case RelocationSpace::JumpTables:
        address = insertAddressOf(insertionSet, index, origin, { SymbolSpace::JumpTable, symbol.index });
        break;
    }
    if (!address)
        return nullptr;
    return insertAddPtr(insertionSet, index, origin, address, value.addend());
}

B3::Value* ImageRelocationLowering::loadSlot(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, uint32_t slot) const
{
    int64_t offset = slotSize * slot;
    if (!m_table.offsetHoldsTablePointer)
        offset += m_table.offset;
    if (!isRepresentableAs<int32_t>(offset))
        return nullptr;

    B3::Value* table = insertionSet.insert<B3::ArgumentRegValue>(index, origin, m_table.base);
    if (m_table.offsetHoldsTablePointer)
        table = insertImmutableLoad(insertionSet, index, origin, table, m_table.offset);
    return insertImmutableLoad(insertionSet, index, origin, table, static_cast<int32_t>(offset));
}

B3::Value* ImageRelocationLowering::insertAddressOf(B3::InsertionSet& insertionSet, size_t index, B3::Origin origin, CodeSymbol symbol)
{
    B3::PatchpointValue* patchpoint = insertionSet.insert<B3::PatchpointValue>(index, B3::pointerType(), origin);
    patchpoint->effects = B3::Effects::none();
    patchpoint->resultConstraints = { B3::ValueRep::SomeRegister };
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setGenerator([this, symbol](CCallHelpers&, const B3::StackmapGenerationParams& params) {
        if (symbol.space == SymbolSpace::SiteCache)
            m_relocations.moveInstanceAddress(symbol, instanceGPR, params[0].gpr());
        else
            m_relocations.moveAddress(symbol, params[0].gpr(), PCRelativeReach::Far);
    });
    return patchpoint;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
