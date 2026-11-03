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
#include "AOTExpectedRowsEmitter.h"
#include "AOTForInEmitter.h"
#include "AOTFrameMapEmitter.h"
#include "AOTFunctionBuiltinEmitter.h"
#include "AOTFusedAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTGuardValueEmitter.h"
#include "AOTHashTableBuiltinEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTInlineBuiltinEmitter.h"
#include "AOTIntlBuiltinEmitter.h"
#include "AOTJSONParseEmitter.h"
#include "AOTJSONWriteEmitter.h"
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
#include "AOTRuntimeValidationEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSelectedOperatorEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTSlotRunEmitter.h"
#include "AOTStaticsEmitter.h"
#include "AOTStringBuilderEmitter.h"
#include "AOTStringBuilderPartEmitter.h"
#include "AOTStringBuiltinEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStringRangeReadEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTruthinessEmitter.h"
#include "AOTTypeTestEmitter.h"
#include <wtf/FastMalloc.h>

namespace JSC { namespace AOT { namespace Typed {

struct CodeGenerator::Emitters {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Emitters);

#define AOT_DECLARE_EMITTER_SLOT(Class) std::optional<Class> m_##Class;
    FOR_EACH_AOT_EMITTER(AOT_DECLARE_EMITTER_SLOT)
#undef AOT_DECLARE_EMITTER_SLOT
};

CodeGenerator::CodeGenerator(const FunctionInputs& inputs, B3::Procedure& proc, CodeRecords& records)
    : m_inputs(inputs)
    , m_proc(proc)
    , m_heaps(AOTHeaps::of(proc))
    , m_records(records)
    , m_out(proc)
    , m_emitters(makeUnique<Emitters>())
    , m_droppedGuard(droppedGuardIn(inputs.build.recordedOptions(), inputs.id))
    , m_validates(inputs.build.validates())
    , m_checksProofs(!inputs.build.validates() && inputs.build.recordedOptions().has(TestingSwitch::CheckProofsAtRunTime))
    , m_runsOutOfMemoryWhenCounted(inputs.build.recordedOptions().isSabotaged(Sabotage::MemoryRunsOutWhenCounted) || inputs.build.recordedOptions().isSabotaged(Sabotage::MemoryRunsOutWhenCountedByRule))
    , m_checksKeptRegisters(inputs.build.validates() && inputs.build.options().validatesBackEnd)
    , m_forcesGCSlowPaths(inputs.build.recordedOptions().has(TestingSwitch::ForceGCSlowPaths))
{
}

CodeGenerator::~CodeGenerator() = default;

#define AOT_DEFINE_EMITTER_ACCESSOR(Class) \
    template<> Class& CodeGenerator::emitter<Class>() \
    { \
        std::optional<Class>& slot = m_emitters->m_##Class; \
        if (!slot) \
            slot.emplace(*this); \
        return *slot; \
    }
FOR_EACH_AOT_EMITTER(AOT_DEFINE_EMITTER_ACCESSOR)
#undef AOT_DEFINE_EMITTER_ACCESSOR

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
