/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWasmStubs.h"

#if ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTBuildContext.h"
#include "AOTWasmInterface.h"
#include "CCallHelpers.h"
#include "JSWebAssemblyInstance.h"
#include "WasmBinding.h"
#include "WasmCallingConvention.h"
#include "WasmModuleInformation.h"
#include "WasmTypeDefinition.h"

namespace JSC { namespace AOT {

static CCallHelpers::Address addressOfSlot(GPRReg table, uint32_t slot)
{
    return CCallHelpers::Address(table, safeCast<int32_t>(slot * sizeof(void*)));
}

static WasmStubCode finalizeStub(RelocationRecorder& recorder, BuildContext& context)
{
    auto code = recorder.finalize(LinkBuffer::Profile::WasmThunk, &context);
    if (!code)
        return makeUnexpected(code.error());
    RELEASE_ASSERT(code->relocations.isEmpty());
    return WTF::move(code->bytes);
}

WasmStubCode generateWasmThrowStub(BuildContext& context)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    jit.preserveReturnAddressAfterCall(GPRInfo::argumentGPR2);
    jit.move(GPRInfo::wasmContextInstancePointer, GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR0, JSWebAssemblyInstance::offsetOfVM()), GPRInfo::argumentGPR3);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR3, VM::topEntryFrameOffset()), GPRInfo::argumentGPR3);
    jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(GPRInfo::argumentGPR3);
    jit.prepareWasmCallOperation(GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::argumentGPR0, JSWebAssemblyInstance::offsetOfImageCodePointers()), GPRInfo::nonPreservedNonArgumentGPR0);
    jit.call(addressOfSlot(GPRInfo::nonPreservedNonArgumentGPR0, Wasm::ImagePointerTable::slotOf(Wasm::ImageCodeOperation::operationThrowExceptionFromOMG)), OperationPtrTag);
    jit.farJump(GPRInfo::returnValueGPR, ExceptionHandlerPtrTag);
    jit.breakpoint();
    return finalizeStub(recorder, context);
}

WasmStubCode generateWasmImportStub(const Wasm::ModuleInformation& info, unsigned importIndex, BuildContext& context)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    Wasm::emitWasmToWasm(jit, info, importIndex);
    return finalizeStub(recorder, context);
}

WasmStubCode generateWasmJSEntrypoint(const Wasm::RTT& rtt, uint32_t calleeSlot, BuildContext& context)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    RegisterAtOffsetList registersToSpill = rtt.icEntrypointSavedRegisters();
    bool isEmitted = rtt.emitJSToWasmICEntrypoint(jit, registersToSpill, nullptr, Wasm::RTT::ICEntrypointImagePointers { calleeSlot, Wasm::ImagePointerTable::genericJSEntrypoint });
    RELEASE_ASSERT(isEmitted);
    return finalizeStub(recorder, context);
}

WasmStubCode generateWasmInterpreterStub(uint32_t calleeSlot, BuildContext& context)
{
    GPRReg callee = Wasm::wasmCallingConvention().prologueScratchGPRs[0];
    GPRReg table = Wasm::wasmCallingConvention().prologueScratchGPRs[1];

    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    jit.loadPtr(CCallHelpers::Address(GPRInfo::wasmContextInstancePointer, JSWebAssemblyInstance::offsetOfImageCodePointers()), table);
    jit.loadPtr(addressOfSlot(table, calleeSlot), callee);
    // The call has happened and the prologue has not, as in emitWasmToWasm().
    jit.storeWasmCalleeToCalleeCallFrame(callee, safeCast<int>(sizeof(CallerFrameAndPC)) - safeCast<int>(prologueStackPointerDelta()));
    jit.loadPtr(addressOfSlot(table, Wasm::ImagePointerTable::interpreterEntrypoint), callee);
    jit.farJump(callee, WasmEntryPtrTag);
    return finalizeStub(recorder, context);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)
