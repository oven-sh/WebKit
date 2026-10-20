/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTConvention.h"
#include "AOTStubsCallFrames.h"
#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirGenerationContext.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

static void emitStoreToStackWord(CCallHelpers& jit, const ValueRep& source, GPRReg scratch, uint32_t stackIndex)
{
    CCallHelpers::Address word(GPRInfo::callFrameRegister, static_cast<int32_t>(offsetOfStackWordFromFrame(stackIndex)));
    if (source.isGPR()) {
        jit.store64(source.gpr(), word);
        return;
    }
    if (source.isFPR()) {
        jit.storeDouble(source.fpr(), word);
        return;
    }
    source.emitRestore(jit, scratch);
    jit.store64(scratch, word);
}

void ControlEmitter::generateReturn(const TerminatorContext& terminator)
{
    ValueID value = terminator->valueOperand();
    uint32_t numberOfResults = static_cast<uint32_t>(m_function.results.size());
    Vector<Operand, 4> operands;
    if (numberOfResults == 1) {
        if (value)
            operands.append(get(value));
        else
            operands.append(emitter<ConstantEmitter>().constantOf(jsUndefined(), inputs().conventions.resultRepresentation(m_program, m_id)));
    } else {
        for (uint32_t index = 0; index < numberOfResults; ++index)
            operands.append(generator().resultAt(value, index));
    }
    emitter<CallEmitter>().returnValues(terminator, operands.span());
}

void ControlEmitter::generateReturnCall(const TerminatorContext& terminator)
{
    generator().generateReturnedCall(terminator, terminator->callOperand());
}

void CallEmitter::returnValues(const TerminatorContext&, std::span<const Operand> operands)
{
    const CallingConventions& conventions = inputs().conventions;
    ArgumentLocations locations = conventions.locationsOf(m_program, m_id);
    std::span<const ArgumentLocation> results = locations.results.span();
    bool spillsArgumentRegisters = conventions.of(m_program, m_id).spillsArgumentRegisters;
    RELEASE_ASSERT(operands.size() == results.size());
    for (size_t i = 0; i < results.size(); ++i)
        RELEASE_ASSERT(operands[i].representation == results[i].representation);

    if (!spillsArgumentRegisters && results.size() <= 1) {
        if (results.empty() || results[0].kind == ArgumentLocation::Kind::None) {
            m_out.retVoid();
            return;
        }
        RELEASE_ASSERT(results[0].reg == (isPassedInFPR(results[0].representation) ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR)));
        m_out.ret(operands[0].value);
        return;
    }

    Vector<ConstrainedValue, 8> inRegisters;
    Vector<ConstrainedValue, 4> onStack;
    Vector<uint32_t, 4> stackIndices;
    for (size_t i = 0; i < results.size(); ++i) {
        LValue value = operands[i].value;
        switch (results[i].kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            inRegisters.append(ConstrainedValue(value, ValueRep::reg(results[i].reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            onStack.append(ConstrainedValue(value->type() == Int32 ? m_out.zeroExt(value, Int64) : value, ValueRep::WarmAny));
            stackIndices.append(results[i].stackIndex);
            break;
        }
    }
    RELEASE_ASSERT(stackIndices.isEmpty() || !spillsArgumentRegisters);

    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    for (const ConstrainedValue& result : inRegisters)
        patchpoint->append(result);
    for (const ConstrainedValue& result : onStack)
        patchpoint->append(result);
    if (!stackIndices.isEmpty())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setRunsWithoutFrame(stackIndices.isEmpty());
    patchpoint->setGenerator([numberInRegisters = static_cast<unsigned>(inRegisters.size()), stackIndices, spillsArgumentRegisters](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreToStackWord(jit, params[numberInRegisters + i], params.gpScratch(0), stackIndices[i]);
        if (params.context().frameIsSetUp)
            Air::emitFrameTeardown(jit, params.code());
        if (spillsArgumentRegisters)
            emitReturnDroppingSpilledArgumentRegisters(jit);
        else
            jit.ret();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
