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
#include "AOTGuardEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTInlineBuiltinEmitter.h"
#include "AOTModuleEmitter.h"
#include "AOTObjectCreationEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTOwnAccessEmitter.h"
#include "AOTPrivateAccessEmitter.h"
#include "AOTPromiseIntrinsicEmitter.h"
#include "AOTRepresentationEmitter.h"
#include "AOTResolvedCallEmitter.h"
#include "AOTRuntimeIntrinsicEmitter.h"
#include "AOTSavedFrameEmitter.h"
#include "AOTSlotAccessEmitter.h"
#include "AOTStringBuilderEmitter.h"
#include "AOTStringBuilderPartEmitter.h"
#include "AOTStringEmitter.h"
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

template<Opcode opcode>
void CodeGenerator::dispatchOperation(const ValueContext& value)
{
    switch (opcode) {
#include "AOTSchemaOperationDispatch.inc"
    }
}

const CodeGenerator::OperationDispatchers& CodeGenerator::operationDispatchers()
{
    static constexpr OperationDispatchers dispatchers = []<size_t... opcodes>(std::index_sequence<opcodes...>) {
        return OperationDispatchers {
            [](CodeGenerator& generator, const ValueContext& operation) {
                generator.dispatchOperation<static_cast<Opcode>(opcodes)>(operation);
            }...
        };
    }(std::make_index_sequence<numberOfEnumerators<Opcode>>());
    return dispatchers;
}

template<TerminatorKind kind>
void CodeGenerator::dispatchTerminator(const TerminatorContext& terminator)
{
    switch (kind) {
#include "AOTSchemaTerminatorDispatch.inc"
    }
}

const CodeGenerator::TerminatorDispatchers& CodeGenerator::terminatorDispatchers()
{
    static constexpr TerminatorDispatchers dispatchers = []<size_t... kinds>(std::index_sequence<kinds...>) {
        return TerminatorDispatchers {
            [](CodeGenerator& generator, const TerminatorContext& terminator) {
                generator.dispatchTerminator<static_cast<TerminatorKind>(kinds)>(terminator);
            }...
        };
    }(std::make_index_sequence<numberOfEnumerators<TerminatorKind>>());
    return dispatchers;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
