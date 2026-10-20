/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelocation.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTImageFormat.h"
#include <algorithm>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

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

std::expected<Vector<Relocation>, LinkFailure> RelocationRecorder::link(ImageLinkBuffer& buffer, BuildContext* context)
{
    LinkBuffer& linkBuffer = buffer.linkBuffer();
    if (linkBuffer.hasProcessAddress())
        return makeUnexpected(LinkFailure::ProcessAddressInCode);
    if (context && context->validates())
        validateNoMappedAddress(linkBuffer, *context);

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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
