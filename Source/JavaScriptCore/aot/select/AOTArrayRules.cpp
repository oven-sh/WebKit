/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayRules.h"

#if ENABLE(AOT)

#include "AOTSelectionContext.h"

namespace JSC { namespace AOT { namespace Typed {

ArrayLiteralStorage chooseArrayLiteralStorage(SelectionContext& context, ValueID literal)
{
    const Value& value = context.function().at(literal);
    if (!value.elementsOperands().empty() || !value.holes().empty() || value.capacityOperand() || value.storage() != AllocationStorage::Heap)
        return ArrayLiteralStorage::Fresh;
    if (context.shapeAt(literal) != CodeShape::Fast)
        return ArrayLiteralStorage::Fresh;
    return ArrayLiteralStorage::Profiled;
}

void selectArrayLiteralStorage(SelectionContext& context)
{
    Function& function = context.pass().function;
    uint32_t numberOfSelectedLiterals = 0;
    for (ValueID literal : context.valuesWith(Opcode::NewArray)) {
        ArrayLiteralStorage storage = chooseArrayLiteralStorage(context, literal);
        if (storage == ArrayLiteralStorage::Fresh)
            continue;
        function.edit(literal).setLiteralStorage(storage);
        ++numberOfSelectedLiterals;
    }
    context.didSelectForms(SelectionRuleID::ArrayLiteralStorage, numberOfSelectedLiterals);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
