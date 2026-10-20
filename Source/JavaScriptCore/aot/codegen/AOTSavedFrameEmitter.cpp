/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedFrameEmitter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallEmitter.h"
#include "AOTCoroutines.h"
#include "AOTFrameMapEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

CoroutineKind SavedFrameEmitter::kindOfFunction() const
{
    return coroutineOfBody(m_function).kind;
}

LValue SavedFrameEmitter::cellOrZero(ValueID driver)
{
    if (representationOf(driver) == Representation::Cell)
        return in<CellPointer>(driver).value();
    // Not folded by type: the site word names the location of this word, and a constant has none.
    Boxed boxed = in<Boxed>(driver);
    return m_out.select(isCell(boxed).value(), boxed.value(), m_out.int64Zero);
}

StubCallResult SavedFrameEmitter::suspend(const ValueContext& value, const Suspension& suspension)
{
    const StubCatalog& catalog = inputs().stubCatalog;
    const StubSummary& summary = catalog.summaryOf(suspension.key);
    const StubSignature& signature = summary.signature;
    RELEASE_ASSERT(signature.numberOfOperands == (suspension.secondOperand ? 3 : 2));
    Reg siteWordRegister = signature.operands[signature.numberOfOperands - 1];
    bool hasMode = signature.result == StubResult::TwoGPRs;
    RELEASE_ASSERT(hasMode || signature.result == StubResult::GPR);
    LType type = hasMode ? m_proc.addTuple({ B3::Int64, B3::Int64 }) : B3::Int64;

    RegisterSet lateClobbered = summary.clobbered;
    lateClobbered.merge(RegisterSet::macroClobberedGPRs());
    lateClobbered.add(siteWordRegister, IgnoreVectors);
    CallPatchpointRequest request {
        .resultType = type,
        .earlyClobbered = RegisterSet::macroClobberedGPRs(),
        .lateClobbered = lateClobbered,
        .effects = catalog.effectsOf(suspension.key),
        .argumentBufferAccess = ArgumentBufferAccess::ReadsAndWrites,
        .holdsUniqueRecord = true,
    };

    LValue driver = cellOrZero(suspension.driver);
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    B3::PatchpointValue* patchpoint = calls.createCallPatchpoint(value, request);
    patchpoint->append(B3::ConstrainedValue(suspension.firstOperand, B3::ValueRep::reg(signature.operands[0])));
    if (suspension.secondOperand)
        patchpoint->append(B3::ConstrainedValue(suspension.secondOperand, B3::ValueRep::reg(signature.operands[1])));
    unsigned driverChild = m_proc.resultCount(type) + patchpoint->numChildren();
    patchpoint->append(B3::ConstrainedValue(driver, B3::ValueRep::LateColdAny));
    FrameMapEmitter::Roots roots = emitter<FrameMapEmitter>().appendRoots(patchpoint, value);
    patchpoint->resultConstraints = { B3::ValueRep::reg(signature.firstResult) };
    if (hasMode)
        patchpoint->resultConstraints.append(B3::ValueRep::reg(signature.secondResult));

    uint32_t site = calls.addSite(value);
    patchpoint->setGenerator([records = &m_records, build = &build(), function = m_id, valueID = value.id(), key = suspension.key, site, roots, driverChild, siteWordGPR = siteWordRegister.gpr()](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        std::optional<uint64_t> word = FrameMapEmitter::siteWordFor(records->frameMaps, params, roots, driverChild);
        if (!word) {
            build->diagnostics().add(InternalError { .kind = InternalErrorKind::FrameMapNotEncodable, .function = function, .value = valueID });
            jit.breakpoint();
            return;
        }
        if (*word >> 32)
            jit.move(CCallHelpers::TrustedImm64(*word), siteWordGPR);
        else
            jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(*word)), siteWordGPR);
        records->stubCalls.call(jit, key, OptionalIndex { site });
    });
    calls.finishCallPatchpoint(value, patchpoint, site, request);

    if (!hasMode)
        return { patchpoint, nullptr };
    return { m_out.extract(patchpoint, 0), m_out.extract(patchpoint, 1) };
}

void SavedFrameEmitter::generateAwaitBySavingFrame(const ValueContext& value)
{
    ValueID awaited = value->awaitedOperand();
    ValueID driver = value->promiseOperand();
    switch (kindOfFunction()) {
    case CoroutineKind::AsyncFunction:
        define(value, Boxed(suspend(value, { awaitBySavingFrame(), in<Boxed>(awaited).value(), nullptr, driver }).first));
        return;
    case CoroutineKind::AsyncGenerator:
        define(value, Boxed(suspend(value, { AOT::suspendGenerator(GeneratorSuspension::AsyncAwait), cellOrZero(driver), in<Boxed>(awaited).value(), driver }).first));
        return;
    case CoroutineKind::Generator:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateAwaitCallBySavingFrame(const ValueContext& value)
{
    if (kindOfFunction() != CoroutineKind::AsyncFunction) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    Vector<Operand, 8> arguments;
    for (ValueID argument : value->argumentsOperands())
        arguments.append(get(argument));
    arguments.append(undefined());
    Boxed returned = emitter<CallEmitter>().callDirect(value, value->function(), arguments.span(), { .isTailCall = value->isDemotedTailCall() }).as<Boxed>();
    define(value, Boxed(suspend(value, { awaitCallBySavingFrame(), returned.value(), nullptr, value->promiseOperand() }).first));
}

void SavedFrameEmitter::suspendGenerator(const ValueContext& value, GeneratorSuspension form, Boxed yielded)
{
    ValueID generator = value->generatorOperand();
    defineYieldResults(value, suspend(value, { AOT::suspendGenerator(form), in<CellPointer>(generator).value(), yielded.value(), generator }));
}

void SavedFrameEmitter::defineYieldResults(const ValueContext& value, StubCallResult result)
{
    std::array<Operand, numberOfEnumerators<YieldResult>> results;
    results[static_cast<size_t>(YieldResult::Sent)] = Boxed(result.first);
    results[static_cast<size_t>(YieldResult::Mode)] = low32(Integer(result.second));
    defineResults(value, results);
}

void SavedFrameEmitter::generateYieldBySavingFrame(const ValueContext& value)
{
    switch (kindOfFunction()) {
    case CoroutineKind::Generator:
        suspendGenerator(value, GeneratorSuspension::Yield, in<Boxed>(value->valueOperand()));
        return;
    case CoroutineKind::AsyncGenerator:
        suspendGenerator(value, GeneratorSuspension::AsyncYield, in<Boxed>(value->valueOperand()));
        return;
    case CoroutineKind::AsyncFunction:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SavedFrameEmitter::generateAsyncGeneratorYield(const ValueContext& value)
{
    switch (kindOfFunction()) {
    case CoroutineKind::AsyncGenerator:
        suspendGenerator(value, GeneratorSuspension::AsyncYieldAfterAwait, in<Boxed>(value->valueOperand()));
        return;
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncFunction:
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
