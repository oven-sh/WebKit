/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidationEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTFlowResults.h"
#include "AOTPlacement.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(argumentBufferAccessOf(RuntimeOperation::CloneObjectAs) == ArgumentBufferAccess::Reads);

ValidationEmitter::ValidationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void ValidationEmitter::generateIsPlainDataObject(const ValueContext& value)
{
    ValueID tested = value->valueOperand();
    if (!typeOf(tested).hasCells()) {
        defineTest(value, boolean(false));
        return;
    }
    defineTest(value, isNotZero(callOperation<Integer>(value, RuntimeOperation::IsPlainDataObject, { boxed(tested) })));
}

void ValidationEmitter::generateGetOwnDataProperty(const ValueContext& value)
{
    CellPointer object = in<CellPointer>(value->objectOperand());
    define(value, callOperation<Boxed>(value, RuntimeOperation::GetOwnDataProperty, { object, identifierNumber(value->key()) }));
}

void ValidationEmitter::generateCloneAs(const ValueContext& value)
{
    std::optional<Request> request = value->request();
    if (!request) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Vector<Boxed, 8> replacedValues;
    for (ValueID replaced : value->valuesOperands())
        replacedValues.append(boxed(replaced));

    Boxed source = boxCell(in<CellPointer>(value->sourceOperand()));
    Pointer values = argumentBuffer(replacedValues.span());
    define(value, callOperation<CellPointer>(value, RuntimeOperation::CloneObjectAs, { source, word32(static_cast<int32_t>(request->value())), values }));
}

void ValidationEmitter::generateExpectRows(const ValueContext& value)
{
    LayoutID layout = value->layout();
    const FlowResults& flow = inputs().flow;
    AllocationSiteID site = flow.typedResultSite(layout);
    uint32_t shape = m_placement.shapes().shapeOf(layout, site ? flow.at(site).allocationClass : AllocationClassID());
    if (!shape) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    callVoidOperation(value, RuntimeOperation::ExpectRows, { boxed(value->targetOperand()), word32(static_cast<int32_t>(shape)) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
