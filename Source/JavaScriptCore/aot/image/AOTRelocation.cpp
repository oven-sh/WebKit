/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelocation.h"

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTImageFormat.h"
#include "AOTInstanceLayout.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT {

static_assert(sizeOfNearCall == MacroAssembler::relocationSize(RelocationKind::Call) && sizeOfJump == MacroAssembler::relocationSize(RelocationKind::Jump));
static_assert(MacroAssembler::relocationSize(RelocationKind::Distance32) == sizeof(int32_t) && MacroAssembler::relocationSize(RelocationKind::NumberWord) == sizeof(uint32_t));
static constexpr size_t reachOfInstanceWord = isARM64() ? static_cast<size_t>(std::numeric_limits<int32_t>::max()) : static_cast<size_t>(MacroAssembler::relocationReach(RelocationKind::InstanceAccess));
static_assert(InstanceLayout::reachAboveInstance <= reachOfInstanceWord && InstanceLayout::reachBelowInstance <= reachOfInstanceWord);

// LinkBuffer's shouldPerformBranchCompaction must stay true: on ARM64 only its compaction loop picks the encoding of a jump that was linked inside the assembler, and a jump without an encoding is never linked.
ImageLinkBuffer::ImageLinkBuffer(CCallHelpers& jit, LinkBuffer::Profile profile)
    : m_end(jit.label())
    , m_storage(roundUpToMultipleOf<sizeof(uint32_t)>(std::max<size_t>(jit.m_assembler.codeSize() + planBranchIslands(jit), sizeof(uint32_t))))
    , m_linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(m_storage.mutableSpan().data()), m_storage.size(), profile)
{
    RELEASE_ASSERT(jit.policy().recordsProcessAddresses);
}

size_t ImageLinkBuffer::planBranchIslands([[maybe_unused]] CCallHelpers& jit)
{
#if CPU(ARM64)
    const uint32_t reach = jit.policy().branchReach;
    if (jit.m_assembler.codeSize() <= reach / 2) [[likely]]
        return 0;
    m_branchIslands = jit.m_assembler.planBranchIslands(reach);
    if (m_branchIslands.islands.isEmpty())
        return 0;
    m_hasBranchIslands = true;
    jit.m_assembler.setBranchIslands(&m_branchIslands);
    return m_branchIslands.size();
#else
    return 0;
#endif
}

void ImageLinkBuffer::finalize(bool dumpsDisassembly, ASCIILiteral name)
{
    RELEASE_ASSERT(!didFailToAllocate() && !m_didFinalize);
    FINALIZE_CODE_IF(dumpsDisassembly, m_linkBuffer, JITThunkPtrTag, nullptr, "%s", name.characters());
    m_sizeOfCode = m_linkBuffer.offsetOf(m_end);
    m_didFinalize = true;
}

std::span<uint8_t> ImageLinkBuffer::bytes()
{
    RELEASE_ASSERT(m_didFinalize);
    return m_storage.mutableSpan().first(m_sizeOfCode);
}

Vector<uint8_t> ImageLinkBuffer::takeBytes()
{
    RELEASE_ASSERT(m_didFinalize);
    m_storage.shrink(m_sizeOfCode);
    return std::exchange(m_storage, { });
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
    AssemblerPolicy policyOfCode = policy;
    policyOfCode.recordsLinkedSites = false;
    jit.setPolicy(policyOfCode);
}

void RelocationRecorder::moveAddress(CodeSymbol symbol, GPRReg dest, PCRelativeReach reach)
{
    append(m_jit.movePCRelativeAddress(dest, reach), addressKindFor(reach), symbol);
}

void RelocationRecorder::load64(CodeSymbol symbol, GPRReg dest)
{
    append(m_jit.loadPCRelative64(dest), RelocationKind::Load64, symbol);
}

void RelocationRecorder::emitNumber32(CodeSymbol symbol)
{
    CCallHelpers::Label site = m_jit.label();
    m_jit.m_assembler.buffer().putInt(0);
    append(site, RelocationKind::NumberWord, symbol);
}

void RelocationRecorder::moveInstanceAddress(CodeSymbol symbol, GPRReg instance, GPRReg dest)
{
    append(m_jit.moveInstanceAddress(instance, dest), RelocationKind::InstanceOffset, symbol);
}

void RelocationRecorder::loadInstanceWord(CodeSymbol symbol, InstanceWordWidth width, GPRReg instance, GPRReg dest)
{
    switch (width) {
    case InstanceWordWidth::Byte:
        append(m_jit.loadInstanceWord8(instance, dest), RelocationKind::InstanceAccess, symbol);
        return;
    case InstanceWordWidth::Word32:
        append(m_jit.loadInstanceWord32(instance, dest), RelocationKind::InstanceAccess, symbol);
        return;
    case InstanceWordWidth::Word64:
        append(m_jit.loadInstanceWord64(instance, dest), RelocationKind::InstanceAccess, symbol);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RelocationRecorder::loadInstanceDouble(CodeSymbol symbol, GPRReg instance, FPRReg dest)
{
    append(m_jit.loadInstanceDouble(instance, dest), RelocationKind::InstanceAccess, symbol);
}

void RelocationRecorder::storeInstanceWord(CodeSymbol symbol, InstanceWordWidth width, GPRReg source, GPRReg instance)
{
#if CPU(ARM64)
    RELEASE_ASSERT(source != MacroAssembler::memoryTempRegister);
#endif
    switch (width) {
    case InstanceWordWidth::Byte:
        append(m_jit.storeInstanceWord8(source, instance), RelocationKind::InstanceAccess, symbol);
        return;
    case InstanceWordWidth::Word32:
        append(m_jit.storeInstanceWord32(source, instance), RelocationKind::InstanceAccess, symbol);
        return;
    case InstanceWordWidth::Word64:
        append(m_jit.storeInstanceWord64(source, instance), RelocationKind::InstanceAccess, symbol);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RelocationRecorder::storeInstanceDouble(CodeSymbol symbol, FPRReg source, GPRReg instance)
{
    append(m_jit.storeInstanceDouble(source, instance), RelocationKind::InstanceAccess, symbol);
}

bool RelocationRecorder::exceedsBranchReach() const
{
    return isARM64() && m_jit.m_assembler.codeSize() > m_jit.policy().branchReach / 2;
}

CCallHelpers::Label RelocationRecorder::returnAddressOf(uint32_t call) const
{
    RELEASE_ASSERT(m_pending[call].kind == RelocationKind::Call);
    const CCallHelpers::Label start = CCallHelpers::Jump(AssemblerLabel(0)).label();
    return CCallHelpers::Jump(AssemblerLabel(static_cast<uint32_t>(MacroAssembler::differenceBetween(start, m_pending[call].site) + sizeOfNearCall))).label();
}

void RelocationRecorder::routeTransfersThroughEnd()
{
#if CPU(ARM64)
    if (!exceedsBranchReach()) [[likely]]
        return;

    const CCallHelpers::Label start = CCallHelpers::Jump(AssemblerLabel(0)).label();
    auto labelOf = [&](CCallHelpers::Label site) {
        return AssemblerLabel(static_cast<uint32_t>(MacroAssembler::differenceBetween(start, site)));
    };
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> jumpOfSymbol;
    Vector<PendingRelocation, 32> others;
    Vector<PendingRelocation> jumps;
    for (const PendingRelocation& pending : m_pending) {
        if (pending.kind != RelocationKind::Call && pending.kind != RelocationKind::Jump) {
            others.append(pending);
            continue;
        }
        uint64_t key = static_cast<uint64_t>(pending.space) << 48 | static_cast<uint64_t>(pending.entry) << 32 | pending.symbol;
        uint32_t jump = jumpOfSymbol.ensure(key, [&] {
            CCallHelpers::Label site = m_jit.label();
            m_jit.nearTailCall();
            jumps.append(PendingRelocation { site, pending.symbol, pending.entry, RelocationKind::Jump, pending.space });
            return static_cast<uint32_t>(jumps.size() - 1);
        }).iterator->value;
        if (pending.kind == RelocationKind::Call)
            m_jit.m_assembler.linkNearCall(labelOf(pending.site), labelOf(jumps[jump].site));
        else
            m_jit.m_assembler.linkJump(labelOf(pending.site), labelOf(jumps[jump].site), ARM64Assembler::JumpNoCondition, ARM64Assembler::ConditionInvalid);
    }
    others.appendVector(jumps);
    m_pending = WTF::move(others);
#endif
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

namespace {

#if CPU(X86_64)

class OffsetsOfAscendingSites {
public:
    OffsetsOfAscendingSites(CCallHelpers& jit, ImageLinkBuffer& buffer)
        : m_jumps(jit.m_assembler.takeLinkedJumps())
        , m_start(CCallHelpers::Jump(AssemblerLabel(0)).label())
    {
        RELEASE_ASSERT(!buffer.linkBuffer().policy().recordsLinkedSites);
        findNextShortJump();
    }

    ALWAYS_INLINE uint32_t offsetOf(CCallHelpers::Label site)
    {
        uint32_t emittedOffset = static_cast<uint32_t>(MacroAssembler::differenceBetween(m_start, site));
        while (m_emittedEndOfNextShortJump <= emittedOffset) {
            m_shift = m_shiftBehindNextShortJump;
            ++m_nextJump;
            findNextShortJump();
        }
        return emittedOffset - m_shift;
    }

private:
    ALWAYS_INLINE void findNextShortJump()
    {
        std::span<const X86LinkedJump> jumps = m_jumps.span();
        while (m_nextJump < jumps.size() && !jumps[m_nextJump].isShort)
            ++m_nextJump;
        if (m_nextJump == jumps.size()) {
            m_emittedEndOfNextShortJump = std::numeric_limits<uint64_t>::max();
            return;
        }
        const X86LinkedJump& jump = jumps[m_nextJump];
        m_shiftBehindNextShortJump = m_shift + X86JumpShortener::savingsOf(jump.kind);
        m_emittedEndOfNextShortJump = static_cast<uint64_t>(jump.from) + m_shiftBehindNextShortJump;
    }

    Vector<X86LinkedJump> m_jumps;
    CCallHelpers::Label m_start;
    size_t m_nextJump { 0 };
    uint32_t m_shift { 0 };
    uint32_t m_shiftBehindNextShortJump { 0 };
    uint64_t m_emittedEndOfNextShortJump { 0 };
};

#else

class OffsetsOfAscendingSites {
public:
    OffsetsOfAscendingSites(CCallHelpers&, ImageLinkBuffer& buffer)
        : m_buffer(buffer)
        , m_sizeOfCode(static_cast<uint32_t>(buffer.bytes().size()))
    {
    }

    ALWAYS_INLINE uint32_t offsetOf(CCallHelpers::Label site)
    {
        uint32_t distanceToEnd = static_cast<uint32_t>(MacroAssembler::differenceBetween(site, m_buffer.end()));
        if (m_buffer.hasBranchIslands()) [[unlikely]]
            return m_buffer.linkBuffer().offsetOf(site);
        if (m_isBehindLastShortenedJump)
            return m_sizeOfCode - distanceToEnd;
        uint32_t offset = m_buffer.linkBuffer().offsetOf(site);
        m_isBehindLastShortenedJump = m_sizeOfCode - offset == distanceToEnd;
        return offset;
    }

private:
    ImageLinkBuffer& m_buffer;
    uint32_t m_sizeOfCode;
    bool m_isBehindLastShortenedJump { false };
};

#endif

} // anonymous namespace

std::expected<Vector<Relocation>, LinkFailure> RelocationRecorder::link(ImageLinkBuffer& buffer, BuildContext*)
{
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    if (linkBuffer.hasProcessAddress())
        return makeUnexpected(LinkFailure::ProcessAddressInCode);
    if (linkBuffer.hasBranchOutOfReach())
        return makeUnexpected(LinkFailure::OutOfPCRelativeReach);

    std::span<uint8_t> bytes = buffer.bytes();
    std::span<const PendingRelocation> pendingRelocations = m_pending.span();
    uint32_t previousOffset = 0;
    OffsetsOfAscendingSites offsets(m_jit, buffer);
    Vector<Relocation> relocations(pendingRelocations.size(), [&](size_t index) {
        const PendingRelocation& pending = pendingRelocations[index];
        uint32_t offset = offsets.offsetOf(pending.site);
        ASSERT(offset == linkBuffer.offsetOf(pending.site));
        RELEASE_ASSERT(offset >= previousOffset);
        previousOffset = offset;
        withKindAsConstant(pending.kind, [&](auto kind) ALWAYS_INLINE_LAMBDA {
            MacroAssembler::clearRelocation(kind, bytes.subspan(offset, MacroAssembler::relocationSize(kind)));
        });
        return Relocation { offset, pending.kind, pending.space, pending.entry, pending.symbol };
    });

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
        static_assert(scratchImmediateGPR != ARM64Registers::ip0);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
