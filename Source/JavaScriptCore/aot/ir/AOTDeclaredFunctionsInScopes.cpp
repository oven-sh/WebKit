/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDeclaredFunctionsInScopes.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

FunctionID functionDeclaredInScopeSlot(const Program& program, LayoutID layout, uint32_t index)
{
    const Layout& environment = program.at(layout);
    if (!environment.isEnvironment() || index >= environment.fields.size())
        return { };
    const Field& field = environment.fields[index];
    TypeID written = field.declared.written();
    if (!field.declared.isEstablished() || !field.flags.contains(FieldFlags::IsImmutable) || !written || holdsOnlyPrimitives(written))
        return { };
    Type type = program.at(written);
    if (!type.isJavaScriptValue() || type.primitives)
        return { };
    FunctionID function = singleFunctionOf(program, type);
    return function && isClassConstructorOf(program.at(function).kind) ? FunctionID() : function;
}

LayoutID scopeThatCaptureTakes(const Program& program, FunctionID function, uint32_t capture)
{
    const DeclaredCapture& declared = program.at(function).captures[capture];
    TypeID written = declared.written();
    if (!declared.isEstablished() || !written || holdsOnlyPrimitives(written))
        return { };
    Type type = program.at(written);
    return type.primitives ? LayoutID() : type.environment;
}

bool storeKeepsDeclaredFunctionOverItsOwnScope(const Program& program, const BodyIndex& index, const BodyValue& store)
{
    auto put = store.as<Opcode::PutEnvironmentField>();
    FunctionID function = functionDeclaredInScopeSlot(program, put.layout(), put.field());
    if (!function)
        return true;
    ValueID stored = index.stripCasts(put.valueOperand());
    if (index.isParameter(stored) || index.opcodeOf(stored) != Opcode::NewClosure)
        return false;
    auto made = index.value(stored).as<Opcode::NewClosure>();
    uint32_t numberOfCaptures = static_cast<uint32_t>(program.at(function).captures.size());
    if (made.function() != function || made.capturesOperands().size() != numberOfCaptures)
        return false;
    ValueID holder = index.stripCasts(put.environmentOperand());
    for (uint32_t capture = 0; capture < numberOfCaptures; ++capture) {
        if (scopeThatCaptureTakes(program, function, capture) == put.layout() && index.stripCasts(made.capturesOperands()[capture]) != holder)
            return false;
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
