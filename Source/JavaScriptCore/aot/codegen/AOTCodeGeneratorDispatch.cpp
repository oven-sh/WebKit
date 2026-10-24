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
#include "AOTArrayIntrinsicEmitter.h"
#include "AOTBindingEmitter.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTCastEmitter.h"
#include "AOTClassEmitter.h"
#include "AOTCollectionIntrinsicEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTEngineIntrinsicEmitter.h"
#include "AOTEnvironmentEmitter.h"
#include "AOTEvalEmitter.h"
#include "AOTExceptionEmitter.h"
#include "AOTExpandedIterationEmitter.h"
#include "AOTForInEmitter.h"
#include "AOTFusedAccessEmitter.h"
#include "AOTGuardEmitter.h"
#include "AOTGuardValueEmitter.h"
#include "AOTHashTableBuiltinEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTInlineBuiltinEmitter.h"
#include "AOTJSONParseEmitter.h"
#include "AOTJSONWriteEmitter.h"
#include "AOTModuleEmitter.h"
#include "AOTObjectCreationEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTOwnAccessEmitter.h"
#include "AOTPrivateAccessEmitter.h"
#include "AOTPromiseIntrinsicEmitter.h"
#include "AOTRepresentationEmitter.h"
#include "AOTResolvedCallEmitter.h"
#include "AOTRuntimeIntrinsicEmitter.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSelectedOperatorEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTSlotRunEmitter.h"
#include "AOTStringBuilderEmitter.h"
#include "AOTStringBuilderPartEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStringRangeReadEmitter.h"
#include "AOTValidationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

void CodeGenerator::dispatch(const ValueContext& value)
{
    switch (value.opcode()) {
#include "AOTSchemaOperationDispatch.inc"
    }
}

void CodeGenerator::dispatch(const TerminatorContext& terminator)
{
    switch (terminator.kind()) {
#include "AOTSchemaTerminatorDispatch.inc"
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
