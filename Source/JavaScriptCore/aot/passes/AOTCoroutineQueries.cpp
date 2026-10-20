/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFuseGeneratorLoops.h"

#if ENABLE(AOT)

#include "AOTCoroutines.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool startsGenerator(const Value& value, FunctionID body, uint32_t generatorParameter, ValueID generator)
{
    if (value.opcode != Opcode::CallDirect || value.function() != body)
        return false;
    std::span<const ValueID> arguments = value.argumentsOperands();
    return generatorParameter < arguments.size() && arguments[generatorParameter] == generator;
}

std::optional<GeneratorBodyDescription> describeGeneratorBody(const Program& program, const Function& caller, ValueID newGenerator)
{
    const Value& allocation = caller.at(newGenerator);
    RELEASE_ASSERT(allocation.opcode == Opcode::NewGenerator);

    const CreatorRecord::Coroutine* record = coroutineOf(program.at(allocation.function()));
    if (!record || record->kind != CoroutineKind::Generator || !record->promiseParameter)
        return std::nullopt;
    uint32_t generatorParameter = *record->promiseParameter;

    ValueID start;
    for (const Block& block : caller.blocks) {
        for (ValueID id : block.values) {
            if (!startsGenerator(caller.at(id), record->body, generatorParameter, newGenerator))
                continue;
            if (start)
                return std::nullopt;
            start = id;
        }
    }
    if (!start)
        return std::nullopt;

    Vector<ValueID, 8> entryArguments(caller.at(start).argumentsOperands());
    entryArguments[generatorParameter] = ValueID();
    return GeneratorBodyDescription {
        .body = record->body,
        .entryArguments = WTF::move(entryArguments),
        .generatorParameter = generatorParameter,
        .sentParameter = std::nullopt,
        .modeParameter = std::nullopt,
        .startAtCreation = start,
    };
}

ValueID appendAwait(IRBuilder& builder, ValueID awaited)
{
    Function& function = builder.function();
    if (function.isUnsplitCoroutine())
        return builder.append(Opcode::Await, { awaited });

    RELEASE_ASSERT(function.role == FunctionRole::CoroutineBody);
    switch (coroutineOfBody(function).kind) {
    case CoroutineKind::AsyncFunction:
    case CoroutineKind::AsyncGenerator:
        return builder.append(Opcode::AwaitBySavingFrame, { awaited, driverOf(function) });
    case CoroutineKind::Generator:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
