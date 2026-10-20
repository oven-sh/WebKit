/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParserBuilder.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static DerivedSignature signatureOfNode(const EmissionInputs& inputs, const ResultNode& node)
{
    Function scratch;
    IRBuilder builder(inputs.analysisContext(), scratch, inputs.types);
    Emission emission(builder, inputs);
    return JSONParserBuilder::signatureOf(emission, node);
}

static CreatorRecord creatorOf(ResultNodeRef ref, const ResultNode& node)
{
    CreatorRecord::Parser creator { .type = node.type, .layout = node.layout, .form = TypedResultForm::ParsedText };
    if (ref.isValidated()) {
        creator.expectedResult = ref.expectedResult();
        creator.node = ref.part;
    }
    return creator;
}

static ParsedValue emitOwnNode(JSONParserBuilder& parser, ResultNodeRef ref, ValueID cursor, const ParseExits& exits)
{
    const EmissionInputs& inputs = parser.inputs();
    switch (decideNodeCode({ inputs.program, inputs.flow, inputs.build }, ref, NodeUse::ParsedFromText).code) {
    case NodeCode::Interpreted:
        return parser.emitNode(ref, cursor, exits);
    case NodeCode::Compiled:
        return parser.emitNodeBody(ref, cursor, exits);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Function buildParserOf(ResultNodeRef ref, FunctionRole role, DerivedFunctionInputs& derived)
{
    EmissionInputs inputs(derived, { });
    ResultNode node = describeNode(inputs.program, ref);

    DerivedFunctionHead head = createDerivedFunction(inputs, role, creatorOf(ref, node), signatureOfNode(inputs, node));
    if (!ref.isValidated())
        head.function.results[1].predicted = node.type;

    IRBuilder builder(inputs.analysisContext(), head.function, inputs.types);
    builder.moveToEndOf(head.entry);
    JSONParserBuilder parser(builder, inputs);
    ValueID cursor = head.parameters[0];
    BlockID rejected = builder.addBlock(BlockFrequency::Rare);
    BlockID unknown = builder.addBlock(BlockFrequency::Rare);

    if (node.isContainer())
        parser.emitRequireStackSpace(unknown);
    ParsedValue parsed = emitOwnNode(parser, ref, cursor, { rejected, unknown });
    parser.emitReturnOf({ parser.appendOutcome(JSONOutcome::Parsed), parsed.value, parsed.next });

    builder.moveToEndOf(rejected);
    parser.emitReturnOf({ parser.appendOutcome(JSONOutcome::Rejected), builder.appendUndefined(), cursor });

    builder.moveToEndOf(unknown);
    parser.emitReturnOf({ parser.appendOutcome(JSONOutcome::Unknown), builder.appendUndefined(), cursor });
    return WTF::move(head.function);
}

Function buildParser(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    return buildParserOf(nodeOfKey(key), FunctionRole::Parser, derived);
}

Function buildValidatingParser(const DerivedFunctionKey& key, DerivedFunctionInputs& derived)
{
    return buildParserOf(nodeOfKey(key), FunctionRole::ValidatingParser, derived);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
