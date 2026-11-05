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

// A function for each opcode: one that only hands on to its emitter sets up no frame for the sake of those that do more.
void CodeGenerator::dispatch(const ValueContext& value)
{
    using Dispatcher = void (*)(CodeGenerator&, const ValueContext&);
    static constexpr auto dispatchers = []<size_t... opcodes>(std::index_sequence<opcodes...>) {
        return std::array<Dispatcher, sizeof...(opcodes)> {
            [](CodeGenerator& generator, const ValueContext& operation) {
                generator.dispatchOperation<static_cast<Opcode>(opcodes)>(operation);
            }...
        };
    }(std::make_index_sequence<numberOfEnumerators<Opcode>>());
    dispatchers[static_cast<size_t>(value.opcode())](*this, value);
}

void CodeGenerator::dispatch(const TerminatorContext& terminator)
{
    switch (terminator.kind()) {
#include "AOTSchemaTerminatorDispatch.inc"
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
