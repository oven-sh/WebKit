/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringEmitter.h"

#if ENABLE(AOT)

#include "AOTStubsStrings.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

CellPointer StringEmitter::concatenate(const OperationContext& context, std::span<const CellPointer> strings)
{
    if (strings.empty())
        return emptyString();

    CellPointer result = strings[0];
    for (size_t i = 1; i < strings.size();) {
        Vector<LValue, 3> operands { result.value(), strings[i++].value() };
        if (i < strings.size())
            operands.append(strings[i++].value());
        result = CellPointer(callStub(context, AOT::concatenate(static_cast<unsigned>(operands.size())), operands.span()).first);
    }
    return result;
}

CellPointer StringEmitter::concatenateValues(const OperationContext& context, std::span<const Boxed> primitives, bool isLikelyKey)
{
    return callOperation<CellPointer>(context, RuntimeOperation::Concatenate, { word32(static_cast<int32_t>(primitives.size())), argumentBuffer(primitives), word32(isLikelyKey) });
}

CellPointer StringEmitter::integerToString(const OperationContext& context, Word32 value)
{
    return integerToString(context, signExtended(value));
}

CellPointer StringEmitter::integerToString(const OperationContext& context, Integer value)
{
    return CellPointer(callStub(context, AOT::integerToString(), { value.value() }).first);
}

CellPointer StringEmitter::numberToString(const OperationContext& context, Number value)
{
    return callOperation<CellPointer>(context, RuntimeOperation::DoubleToString, { value });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
