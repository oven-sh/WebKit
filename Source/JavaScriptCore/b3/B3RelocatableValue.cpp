/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RelocatableValue.h"

#if ENABLE(B3_JIT)

#include "B3ConstPtrValue.h"
#include "B3Operations.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include "MathCommon.h"

namespace JSC { namespace B3 {

RelocatableValue::~RelocatableValue() = default;

void RelocatableValue::dumpMeta(CommaPrinter& comma, PrintStream& out) const
{
    if (m_symbol.space == runtimeFunctionSpace && m_symbol.index < numberOfRuntimeFunctions)
        out.print(comma, static_cast<RuntimeFunction>(m_symbol.index));
    else
        out.print(comma, "space = "_s, m_symbol.space, comma, "index = "_s, m_symbol.index);
    if (m_addend)
        out.print(comma, "addend = "_s, m_addend);
}

Value* addressOf(Procedure& proc, RuntimeFunction function, Origin origin)
{
    if (proc.policy().addressing.isPositionIndependent)
        return proc.add<RelocatableValue>(origin, RelocationSymbol { runtimeFunctionSpace, static_cast<uint32_t>(function) }, 0);

    switch (function) {
    case RuntimeFunction::FmodDouble:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::fmodDouble));
    case RuntimeFunction::CeilDouble:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::ceilDouble));
    case RuntimeFunction::CeilFloat:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::ceilFloat));
    case RuntimeFunction::FloorDouble:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::floorDouble));
    case RuntimeFunction::FloorFloat:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::floorFloat));
    case RuntimeFunction::TruncDouble:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::truncDouble));
    case RuntimeFunction::TruncFloat:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(Math::truncFloat));
    case RuntimeFunction::MemoryCopy:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(operationMemoryCopy));
    case RuntimeFunction::MemoryFill:
        return proc.add<ConstPtrValue>(origin, tagCFunction<OperationPtrTag>(operationMemoryFill));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
