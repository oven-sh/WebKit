/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTAllocationEmitter.h"
#include "AOTArgumentsEmitter.h"
#include "AOTArrayBufferViewBuiltinEmitter.h"
#include "AOTArrayBuiltinEmitter.h"
#include "AOTArrayIntrinsicEmitter.h"
#include "AOTArrayStorageBuiltinEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTBindingEmitter.h"
#include "AOTBufferBuiltinEmitter.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCastEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTClassEmitter.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCollectionIntrinsicEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTDateBuiltinEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTEngineIntrinsicEmitter.h"
#include "AOTEntryEmitter.h"
#include "AOTEnvironmentEmitter.h"
#include "AOTErrorBuiltinEmitter.h"
#include "AOTEvalEmitter.h"
#include "AOTExceptionEmitter.h"
#include "AOTExpandedIterationEmitter.h"
#include "AOTForInEmitter.h"
#include "AOTFrameMapEmitter.h"
#include "AOTFunctionBuiltinEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTGuessedAccessEmitter.h"
#include "AOTHashTableBuiltinEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTInlineBuiltinEmitter.h"
#include "AOTIntlBuiltinEmitter.h"
#include "AOTMapSetBuiltinEmitter.h"
#include "AOTMathBuiltinEmitter.h"
#include "AOTModuleEmitter.h"
#include "AOTObjectBuiltinEmitter.h"
#include "AOTObjectCreationEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTOwnAccessEmitter.h"
#include "AOTOwnPropertyBuiltinEmitter.h"
#include "AOTPrivateAccessEmitter.h"
#include "AOTPromiseBuiltinEmitter.h"
#include "AOTPromiseIntrinsicEmitter.h"
#include "AOTRegExpBuiltinEmitter.h"
#include "AOTRepresentationEmitter.h"
#include "AOTResolvedCallEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeIntrinsicEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTStringBuiltinEmitter.h"
#include "AOTStringBuilderEmitter.h"
#include "AOTStringBuilderPartEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsOperations.h"
#include "AOTThrowEmitter.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeTestEmitter.h"
#include <memory>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

CodeGenerator::CodeGenerator(const FunctionInputs& inputs, Machine::Procedure& proc, CodeRecords& records)
    : m_inputs(inputs)
    , m_proc(proc)
    , m_records(records)
    , m_mentions(records.mentions)
    , m_typing { inputs.program.program, inputs.types, PositionTypes::knowingNothing() }
    , m_operationDispatchers(operationDispatchers())
    , m_terminatorDispatchers(terminatorDispatchers())
    , m_out(proc)
    , m_checksAllocation(proc.traits().checksAllocation)
    , m_checksEffects(AOT::hasFlag(inputs.build.options().testing, Wire::TestingSwitch::CheckEffectsAtRunTime))
    , m_understatedEffects(inputs.build.options().understatedEffects)
{
    if (proc.traits().notesRangesOfOrigins) {
        records.formsSaid.beginNoting();
        m_out.noteFormsIn(records.formsSaid);
    }
    m_notesForms = records.formsSaid.areNoted();
    m_watchesOperations = m_notesForms || m_checksAllocation || m_checksEffects;
    if (build().options().recorded.validates) [[unlikely]] {
        m_out.verifyReusedLoads({ heaps().collectorState.index(), heaps().entire(AbstractHeap::TypedArrayElements).index() }, [this](LValue readSameBits) {
            static constexpr OperationCall stop { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters };
            Origin origin = m_operation ? m_operation->origin() : Origin();
            uint32_t function = code().inlineFrames()[origin.inlineFrame].callee.index();
            build().statistics().count(StatisticID::ReusedLoadsVerified);
            m_out.check(m_out.isZero32(readSameBits))->setGenerator([records = &m_records, function, position = origin.position](CCallHelpers& jit, const Machine::PatchParameters& params) {
                if (params.frameIsSetUp()) {
                    AllowMacroScratchRegisterUsage allowScratch(jit);
                    jit.move(CCallHelpers::TrustedImm32(function), argumentGPROfPreservingCall(stop, AOT::Entry::operationAOTStopAtStaleLoad, 1));
                    jit.move(CCallHelpers::TrustedImm32(position), argumentGPROfPreservingCall(stop, AOT::Entry::operationAOTStopAtStaleLoad, 2));
                    records->stubCalls.call(jit, callOperationPreservingRegisters(stop), offsetInRuntimeTable(AOT::Entry::operationAOTStopAtStaleLoad), OptionalIndex());
                }
                jit.breakpoint();
            });
        });
    }
}

CodeGenerator::~CodeGenerator()
{
#define AOT_DESTROY_EMITTER(Class) \
    if constexpr (!std::is_trivially_destructible_v<Class>) { \
        if (Class* made = m_emitters.pointerTo##Class) \
            std::destroy_at(made); \
    }
    FOR_EACH_AOT_EMITTER(AOT_DESTROY_EMITTER)
#undef AOT_DESTROY_EMITTER
}

#define AOT_DEFINE_EMITTER_MAKER(Class) \
    template<> Class& CodeGenerator::makeEmitter<Class>() \
    { \
        Class* made = new (NotNull, m_proc.arena().allocate(sizeof(Class), alignof(Class))) Class(*this); \
        m_emitters.pointerTo##Class = made; \
        return *made; \
    }
FOR_EACH_AOT_EMITTER(AOT_DEFINE_EMITTER_MAKER)
#undef AOT_DEFINE_EMITTER_MAKER

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
