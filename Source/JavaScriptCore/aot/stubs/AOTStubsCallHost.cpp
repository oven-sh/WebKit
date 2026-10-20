/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallHost.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubHelpers.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCalls.h"
#include "FunctionRareData.h"
#include "InternalFunction.h"
#include "JSFunction.h"
#include "NativeExecutable.h"

namespace JSC { namespace AOT {

StubSignature signatureOfCallHostFunction(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallHostFunctionWithoutBoxedCall(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallInternalFunction(StubKey) { return callConventionSignature({ }); }

static void emitLoadExecutable(StubAssembler& jit, GPRReg executable)
{
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), executable);
    Jump hasRareData = jit.branchTestPtr(CCallHelpers::NonZero, executable, TrustedImm32(JSFunction::rareDataTag));
    Label hasExecutable = jit.label();
    jit.addOutOfLinePart([hasRareData, hasExecutable, executable](StubAssembler& jit) {
        hasRareData.link(&jit);
        jit.loadPtr(Address(executable, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), executable);
        jit.jump().linkTo(hasExecutable, &jit);
    });
}

void generateCallHostFunction(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg executable = registers.scratch;
    const Address nativeFunction(executable, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall));

    if (exists(callHostFunctionWithoutBoxedCall(count))) {
        emitLoadExecutable(jit, executable);
        jit.loadPtr(nativeFunction, registers.word);
        // Under validation every call reaches the function, where it is compared with the entry of the built-in.
        if (!jit.options().validates)
            emitBoxedCallDispatch(jit, registers.word, count, jit.allocateScratch(StubRegister::T9), executable);
        jit.bindEntry(callHostFunctionWithoutBoxedCall(count));
    }
    emitLoadExecutable(jit, executable);
    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, count, std::nullopt });
    callNativeFunctionAndReturn(jit, CodeSpecializationKind::CodeForCall, registers.word, Address(calleeGPR, JSCallee::offsetOfScopeChain()), nativeFunction);
}

static CodeSpecializationKind specializationKindOf(InternalFunctionEntry entry)
{
    switch (entry) {
    case InternalFunctionEntry::Call:
        return CodeSpecializationKind::CodeForCall;
    case InternalFunctionEntry::Construct:
        return CodeSpecializationKind::CodeForConstruct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateCallInternalFunction(StubAssembler& jit)
{
    const CodeSpecializationKind kind = specializationKindOf(static_cast<InternalFunctionEntry>(jit.key().form()));
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    jit.emitFunctionPrologue();
    emitHostCallFrame(jit, { calleeGPR, thisGPR, jit.key().count(), std::nullopt });
    callNativeFunctionAndReturn(jit, kind, scratch, Address(calleeGPR, InternalFunction::offsetOfGlobalObject()), Address(calleeGPR, InternalFunction::offsetOfNativeFunctionFor(kind)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
