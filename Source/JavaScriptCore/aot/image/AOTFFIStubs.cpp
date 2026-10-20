/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFFIStubs.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTImageBuilder.h"
#include "AOTImageFormat.h"
#include "AOTPipeline.h"
#include "AOTRelocation.h"
#include "FFIStubEmitters.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

bool mayUseFFI(const Typed::PipelineOutput& pipeline)
{
    return flagsForUnseenCode(pipeline).contains(ImageFlag::AdmitsUnknownJavaScript);
}

#if FFI_STUBS_SUPPORTED

static constexpr size_t stubCodeAlignment = 16;
static constexpr CodeSymbol callbackContextSymbol { SymbolSpace::FFI, 0 };

FFIStubs generateFFIStubs(BuildContext& context)
{
    unsigned numberOfEntries = safeCast<unsigned>(context.limit(LimitID::ForeignCallbackStubs));

    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    CCallHelpers::Label invoke = jit.label();
    FFI::emitInvokeStub(jit);
    auto moveContextAddress = [&](CCallHelpers&, GPRReg destination) {
        recorder.moveAddress(callbackContextSymbol, destination, PCRelativeReach::Far);
    };
    Vector<CCallHelpers::Label> entries = FFI::emitCallbackEntries(jit, numberOfEntries, moveContextAddress);

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    if (buffer.didFailToAllocate()) {
        reportLinkFailure(LinkFailure::OutOfMemory, FunctionID(), context);
        return { };
    }
    buffer.finalize(false, "FFI stubs"_s);
    auto relocations = recorder.link(buffer, &context);
    if (!relocations) {
        reportLinkFailure(relocations.error(), FunctionID(), context);
        return { };
    }

    std::span<const uint8_t> code = buffer.bytes();
    size_t codeOffset = roundUpToMultipleOf<stubCodeAlignment>(FFI::StubDirectory::sizeWithEntries(numberOfEntries));
    size_t contextOffset = roundUpToMultipleOf<imagePageSize>(codeOffset + code.size());
    auto offsetOf = [&](CCallHelpers::Label label) {
        return safeCast<uint32_t>(codeOffset + buffer.linkBuffer().offsetOf(label));
    };

    FFIStubs result;
    result.codeAlignment = imagePageSize;
    result.callbackContextOffset = contextOffset;
    result.callbackContextSize = imagePageSize;
    result.code = Vector<uint8_t>(FillWith { }, contextOffset + imagePageSize, 0);
    std::span<uint8_t> bytes = result.code.mutableSpan();
    memcpySpan(bytes.subspan(codeOffset), code);

    FFI::StubDirectory directory { offsetOf(invoke), safeCast<uint32_t>(contextOffset), safeCast<uint32_t>(imagePageSize), numberOfEntries };
    memcpySpan(bytes, asByteSpan(directory));
    Vector<uint32_t> entryOffsets = entries.map(offsetOf);
    memcpySpan(bytes.subspan(sizeof(FFI::StubDirectory)), asByteSpan(entryOffsets.span()));

    uint64_t numberApplied = applyRelocations(bytes, codeOffset, relocations->span(), [&](const Relocation& relocation) -> std::optional<size_t> {
        RELEASE_ASSERT(relocation.codeSymbol() == callbackContextSymbol);
        return contextOffset;
    }, [&](const Relocation& relocation) {
        context.diagnostics().add(InternalError { .kind = InternalErrorKind::RelocationNotApplied, .count = relocation.offset });
    });
    RELEASE_ASSERT(context.diagnostics().hasErrors() || numberApplied == relocations->size());
    return result;
}

#else

FFIStubs generateFFIStubs(BuildContext&)
{
    return { };
}

#endif // FFI_STUBS_SUPPORTED

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
