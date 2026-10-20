/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsDirectCall.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned endCallbackSlot = 2 * ChainShape::maxNumberOfLinks;
static constexpr unsigned numberOfOperandSlots = endCallbackSlot + 2;

static Function createStepWithoutBody(const Function& traversal)
{
    Function step;
    step.name = traversal.name;
    step.module = traversal.module;
    step.position = traversal.position;
    step.text = traversal.text;
    step.kind = traversal.kind;
    step.role = FunctionRole::ChainStep;
    step.flags = traversal.flags & FunctionFlags::IsHiddenInStackTraces;
    step.thisType = DeclaredType { .type = TypeTable::anything };
    step.results.append(DeclaredType { .type = TypeTable::anything });
    step.capabilities = initialCapabilitiesOf(step);
    step.creator = CreatorRecord::ChainStep { };
    return step;
}

Function buildChainStep(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    ChainShape shape = ChainShape::decode(key.parameter);
    const Function& traversal = inputs.program.at(key.target);

    Function step = createStepWithoutBody(traversal);
    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, step, inputs.types);
    builder.setOrigin({ traversal.position, 0 });
    BlockID block = builder.addBlock();
    auto addParameter = [&] {
        step.parameters.append(DeclaredType { .type = TypeTable::anything });
        return builder.addParameter(block, TypeTable::anything);
    };

    ValueID array = addParameter();
    ValueID start = addParameter();
    ValueID end = addParameter();
    std::array<ValueID, numberOfOperandSlots> slots { };
    for (unsigned link = 0; link < shape.numberOfLinks(); ++link) {
        slots[2 * link] = addParameter();
        slots[2 * link + 1] = addParameter();
    }
    if (endTakesCallback(shape.end))
        slots[endCallbackSlot] = addParameter();
    if (endTakesThisArgument(shape.end))
        slots[endCallbackSlot + 1] = addParameter();
    ValueID operand = endTakesOperand(shape) ? addParameter() : ValueID();

    builder.moveToEndOf(block);
    ValueID undefined = builder.appendUndefined();
    std::ranges::replace(slots, ValueID(), undefined);

    Value operandList = IRBuilder::make(Opcode::NewArray);
    operandList.setContents(TypeTable::anything);
    operandList.setElementsOperands(slots);
    ValueID operands = builder.append(WTF::move(operandList));
    ValueID shapeWord = builder.appendInteger(key.parameter);

    std::array<ValueID, 6> arguments { array, start, end, shapeWord, operands, operand ? operand : undefined };
    ValueID call = appendDirectCall(builder, { .callee = key.target, .arguments = arguments });
    step.at(call).setIsAlwaysInlined(true);
    emitReturnOfCall(builder, call);
    return step;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
