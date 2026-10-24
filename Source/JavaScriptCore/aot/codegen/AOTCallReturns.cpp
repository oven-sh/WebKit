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
#include "AOTPromisedResults.h"
#include "AOTTypeTestEmitter.h"
#include "AOTStubsCallFrames.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirGenerationContext.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3UpsilonValue.h"
#include "JSCJSValueInlines.h"

namespace JSC { namespace AOT { namespace Typed {

static void emitStoreOfResultToStackWord(CCallHelpers& jit, const B3::Air::Code& code, const B3::ValueRep& source, GPRReg scratch, uint32_t stackIndex)
{
    CCallHelpers::Address word = code.addressInFrame(offsetOfStackWordFromFrame(stackIndex));
    if (source.isGPR()) {
        jit.store64(source.gpr(), word);
        return;
    }
    if (source.isFPR()) {
        jit.storeDouble(source.fpr(), word);
        return;
    }
    if (source.isStack())
        jit.load64(code.addressInFrame(source.offsetFromFP()), scratch);
    else
        source.emitRestore(jit, scratch);
    jit.store64(scratch, word);
}

void ControlEmitter::generateReturn(const TerminatorContext& terminator)
{
    ValueID value = terminator->valueOperand();
    uint32_t numberOfResults = static_cast<uint32_t>(m_function.results.size());
    Vector<Operand, 4> operands;
    if (numberOfResults == 1) {
        if (value && leavesAnotherWayUnlessPromised(terminator, value))
            return;
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

// True where the return has been generated. The returns that seldom run share one test.
bool ControlEmitter::leavesAnotherWayUnlessPromised(const TerminatorContext& terminator, ValueID value)
{
    TypeID promised = inputs().promisedResults.of(m_id);
    if (!promised || typeOf(value).isSubsetOf(m_program.at(promised), m_program))
        return false;

    auto testAndReturn = [&](Boxed result, TypeID from) {
        TypeTestEmitter& tests = emitter<TypeTestEmitter>();
        LBasicBlock isPromised = newBlock();
        LBasicBlock isMisfit = newBlock();
        tests.emitBranch(result, tests.planFor(from, Representation::JSValue, promised), { normalEdge(isPromised), rareEdge(isMisfit) }, terminator.shape());
        m_out.appendTo(isMisfit);
        emitter<CallEmitter>().returnMisfit(result);
        m_out.appendTo(isPromised);
        Operand operand = result;
        emitter<CallEmitter>().returnValues(terminator, { &operand, 1 });
    };
    if (m_function.at(terminator.block()).frequency == BlockFrequency::Normal) {
        testAndReturn(in<Boxed>(value), m_function.at(value).type);
        return true;
    }

    if (!m_testedReturn) {
        LBasicBlock here = m_out.m_block;
        m_testedReturn = newBlock();
        m_out.appendTo(m_testedReturn);
        m_resultOfTestedReturn = m_out.phi(B3::Int64);
        testAndReturn(Boxed(m_resultOfTestedReturn), TypeTable::anything);
        m_out.appendTo(here);
    }
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), in<Boxed>(value).value(), m_resultOfTestedReturn);
    m_out.jump(m_testedReturn);
    return true;
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

    Vector<B3::ConstrainedValue, 8> inRegisters;
    Vector<B3::ConstrainedValue, 4> onStack;
    Vector<uint32_t, 4> stackIndices;
    for (size_t i = 0; i < results.size(); ++i) {
        LValue value = operands[i].value;
        switch (results[i].kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            inRegisters.append(B3::ConstrainedValue(value, B3::ValueRep::reg(results[i].reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            onStack.append(B3::ConstrainedValue(value->type() == B3::Int32 ? m_out.zeroExt(value, B3::Int64) : value, B3::ValueRep::ColdAny));
            stackIndices.append(results[i].stackIndex);
            break;
        }
    }
    RELEASE_ASSERT(stackIndices.isEmpty() || !spillsArgumentRegisters);

    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    for (const B3::ConstrainedValue& result : inRegisters)
        patchpoint->append(result);
    for (const B3::ConstrainedValue& result : onStack)
        patchpoint->append(result);
    if (!stackIndices.isEmpty())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setRunsWithoutFrame(stackIndices.isEmpty());
    patchpoint->setGenerator([numberInRegisters = static_cast<unsigned>(inRegisters.size()), stackIndices, spillsArgumentRegisters](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreOfResultToStackWord(jit, params.code(), params[numberInRegisters + i], params.gpScratch(0), stackIndices[i]);
        if (params.context().frameIsSetUp)
            B3::Air::emitFrameTeardown(jit, params.code(), jit.scratchRegister());
        if (spillsArgumentRegisters)
            emitReturnDroppingSpilledArgumentRegisters(jit);
        else
            jit.ret();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
