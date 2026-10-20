/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeTemplates.h"

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static constexpr CodeSymbol placeholderSymbol { SymbolSpace::Function, 0 };

template<typename Emit>
static std::expected<RelocatableCode, LinkFailure> emitTemplate(const AssemblerPolicy& policy, BuildContext* context, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, policy);
    emit(jit, recorder);
    return recorder.finalize(LinkBuffer::Profile::Thunk, context);
}

static void emitVeneer(CCallHelpers& jit, RelocationRecorder& recorder)
{
#if CPU(ARM64)
    recorder.moveAddress(placeholderSymbol, ARM64Registers::ip0, PCRelativeReach::Far);
    jit.farJump(ARM64Registers::ip0, NoPtrTag);
#else
    UNUSED_PARAM(jit);
    recorder.jump(placeholderSymbol);
#endif
}

static bool hasOneRelocationAtStart(const RelocatableCode& code)
{
    return code.relocations.size() == 1 && !code.relocations[0].offset;
}

static void padWithNops(Vector<uint8_t>& bytes)
{
    size_t size = bytes.size();
    bytes.grow(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(size));
    fillWithNops(bytes.mutableSpan().subspan(size));
}

std::expected<CodeTemplates, LinkFailure> CodeTemplates::create(const AssemblerPolicy& policy, BuildContext* context)
{
    auto nearCall = emitTemplate(policy, context, [](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.call(placeholderSymbol);
    });
    if (!nearCall)
        return std::unexpected(nearCall.error());
    RELEASE_ASSERT(hasOneRelocationAtStart(*nearCall) && nearCall->bytes.size() == sizeOfNearCall);

    auto nearJump = emitTemplate(policy, context, [](CCallHelpers&, RelocationRecorder& recorder) {
        recorder.jump(placeholderSymbol);
    });
    if (!nearJump)
        return std::unexpected(nearJump.error());
    RELEASE_ASSERT(hasOneRelocationAtStart(*nearJump) && nearJump->bytes.size() == sizeOfJump);

    auto returnToCaller = emitTemplate(policy, context, [](CCallHelpers& jit, RelocationRecorder&) {
        jit.ret();
    });
    if (!returnToCaller)
        return std::unexpected(returnToCaller.error());
    RELEASE_ASSERT(returnToCaller->relocations.isEmpty());

    auto breakpoint = emitTemplate(policy, context, [](CCallHelpers& jit, RelocationRecorder&) {
        jit.breakpoint();
    });
    if (!breakpoint)
        return std::unexpected(breakpoint.error());
    RELEASE_ASSERT(breakpoint->relocations.isEmpty() && breakpoint->bytes.size() == codeOffsetUnit);

    auto veneer = emitTemplate(policy, context, emitVeneer);
    if (!veneer)
        return std::unexpected(veneer.error());
    RELEASE_ASSERT(hasOneRelocationAtStart(*veneer));
    padWithNops(veneer->bytes);

    return CodeTemplates {
        WTF::move(nearCall->bytes),
        WTF::move(nearJump->bytes),
        WTF::move(returnToCaller->bytes),
        WTF::move(breakpoint->bytes),
        WTF::move(*veneer),
    };
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
