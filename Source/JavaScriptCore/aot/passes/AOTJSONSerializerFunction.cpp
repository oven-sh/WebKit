/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSerializerBuilder.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

DerivedSignature JSONSerializerBuilder::signatureOf(Emission& emission, LayoutID)
{
    return { { TypeTable::object, emission.stringBuilderType() }, { emission.outcomeType() } };
}

static DerivedSignature serializerSignature(const EmissionInputs& inputs, LayoutID layout)
{
    Function scratch;
    IRBuilder builder(inputs.analysisContext(), scratch, inputs.types);
    Emission emission(builder, inputs);
    return JSONSerializerBuilder::signatureOf(emission, layout);
}

Function buildSerializer(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    LayoutID layout(key.parameter);
    EmissionInputs inputs(derived, FunctionID());
    DerivedFunctionHead head = createDerivedFunction(inputs, FunctionRole::Serializer, CreatorRecord::Serializer { layout }, serializerSignature(inputs, layout));

    IRBuilder builder(inputs.analysisContext(), head.function, inputs.types);
    builder.moveToEndOf(head.entry);
    JSONSerializerBuilder serializer(builder, inputs, head.parameters[1]);

    BlockID differs = builder.addBlock(BlockFrequency::Rare);
    serializer.emitJumpUnless(builder.append(Opcode::JSONHasStackSpace, { }), differs);
    serializer.appendObjectOfLayout(head.parameters[0], layout, differs);
    builder.emitReturn(serializer.appendOutcome(JSONOutcome::Parsed));

    builder.moveToEndOf(differs);
    builder.emitReturn(serializer.appendOutcome(JSONOutcome::Unknown));
    return WTF::move(head.function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
