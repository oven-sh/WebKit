/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFFIStubs.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCodeTemplates.h"
#include "AOTFlowResults.h"
#include "AOTImageBuilder.h"
#include "AOTImageFormat.h"
#include "AOTPipeline.h"
#include "AOTRelocation.h"
#include "AOTVector.h"
#include "FFIStubEmitters.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

// Whatever makes a foreign function or a callback is a member of Bun.FFI, which the module bun:ffi reads when it is loaded, or the native function behind cc().
bool mayUseFFI(const Typed::PipelineOutput& pipeline)
{
    if (flagsForUnseenCode(pipeline).contains(ImageFlag::AdmitsUnknownJavaScript))
        return true;
    const Typed::FlowResults& flow = *pipeline.flow;
    return !flow.usesOf(Builtins::BunFFI).isEmpty() || !flow.usesOf(Builtins::NativeNewRustFunctionFfiRsBun__FFI__cc).isEmpty();
}

#if FFI_STUBS_SUPPORTED

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
    WTF::Vector<CCallHelpers::Label> entries = FFI::emitCallbackEntries(jit, numberOfEntries, moveContextAddress);

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

    auto templates = CodeTemplates::create(context.assemblerPolicy(), &context);
    if (!templates) {
        reportLinkFailure(templates.error(), FunctionID(), context);
        return { };
    }

    std::span<const uint8_t> code = buffer.bytes();
    size_t contextOffset = roundUpToMultipleOf<imagePageSize>(code.size());
    auto offsetOf = [&](CCallHelpers::Label label) {
        return safeCast<uint32_t>(buffer.linkBuffer().offsetOf(label));
    };

    FFIStubs result;
    result.codeAlignment = imagePageSize;
    result.callbackContextOffset = contextOffset;
    result.callbackContextSize = imagePageSize;
    result.code = Vector<uint8_t>(FillWith { }, contextOffset + imagePageSize, 0);
    std::span<uint8_t> bytes = result.code.mutableSpan();
    memcpySpan(bytes, code);
    templates->fillWithBreakpoints(bytes.subspan(code.size(), contextOffset - code.size()));

    FFI::StubDirectory directory { offsetOf(invoke), safeCast<uint32_t>(contextOffset), safeCast<uint32_t>(imagePageSize), numberOfEntries };
    Vector<uint32_t> entryOffsets = entries.map(offsetOf);
    result.directory.append(asByteSpan(directory));
    result.directory.append(asByteSpan(entryOffsets.span()));

    uint64_t numberApplied = applyRelocations(bytes, 0, relocations->span(), [&](const Relocation& relocation) -> std::optional<size_t> {
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
