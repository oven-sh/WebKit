/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidatorBuilder.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t valueResultIndex = 1;

static DerivedSignature signatureOfNode(const EmissionInputs& inputs, const ResultNode& node)
{
    Function scratch;
    IRBuilder builder(inputs.analysisContext(), scratch, inputs.types);
    Emission emission(builder, inputs);
    return ValidatorBuilder::signatureOf(emission, node);
}

Function buildValidator(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    ResultNodeRef ref = nodeOfKey(key);
    EmissionInputs inputs(derived, { });
    ResultNode node = describeNode(inputs.program, ref);

    DerivedFunctionHead head = createDerivedFunction(inputs, FunctionRole::Validator,
        CreatorRecord::Validator { .type = node.type, .layout = node.layout, .expectedResult = ref.expectedResult(), .node = ref.part },
        signatureOfNode(inputs, node));

    IRBuilder builder(inputs.analysisContext(), head.function, inputs.types);
    builder.moveToEndOf(head.entry);
    ValidatorBuilder validator(builder, inputs);
    ValidationExits exits { builder.addBlock(BlockFrequency::Rare), builder.addBlock(BlockFrequency::Rare) };

    if (node.isContainer())
        validator.emitJumpUnless(builder.append(Opcode::JSONHasStackSpace, { }), exits.unknown);

    bool isInterpreted = decideNodeCode({ inputs.program, inputs.flow, inputs.build }, ref, NodeUse::CopiedFromValue).code == NodeCode::Interpreted;
    ValueID input = head.parameters[0];
    ValueID validated = isInterpreted ? validator.emitNode(ref, input, exits) : validator.emitNodeBody(ref, input, exits);
    validated = validator.emitTypeTest(validated, head.function.results[valueResultIndex].type, exits.unknown);
    validator.emitReturnOf({ validator.appendOutcome(JSONOutcome::Parsed), validated });

    builder.moveToEndOf(exits.rejected);
    validator.emitReturnOf({ validator.appendOutcome(JSONOutcome::Rejected), builder.appendUndefined() });

    builder.moveToEndOf(exits.unknown);
    validator.emitReturnOf({ validator.appendOutcome(JSONOutcome::Unknown), builder.appendUndefined() });
    return WTF::move(head.function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
