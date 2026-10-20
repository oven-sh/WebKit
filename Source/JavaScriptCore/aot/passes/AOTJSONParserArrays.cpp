/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONParserBuilder.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

ParsedValue JSONParserBuilder::emitArray(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    RELEASE_ASSERT(node.children.size() == 1);
    emitRequireByte(cursor, '[', exits.rejected);
    ArrayLoop loop = beginArrayLoop(node, appendSkipPunctuator(cursor));

    ParsedValue element = emitNodeOrGenericValue(node.children[0], loop.cursorAtHeader, exits);
    builder().append(Opcode::AppendElement, { loop.array, element.value });
    emitElementSeparator(element.next, loop, exits.rejected);

    builder().moveToEndOf(loop.done);
    ValueID next = builder().append(Opcode::JSONSkipByte, { loop.cursorAtDone });
    m_rules.emitRules(node, loop.array, exits.rejected);
    return { loop.array, next };
}

JSONParserBuilder::ArrayLoop JSONParserBuilder::beginArrayLoop(const ResultNode& node, ValueID afterBracket)
{
    ArrayLoop loop;
    loop.array = builder().append(makeArrayOf(program(), node, { }));
    loop.header = builder().addBlock();
    loop.done = builder().addBlock();
    loop.cursorAtHeader = builder().addParameter(loop.header, cursorType());
    loop.cursorAtDone = builder().addParameter(loop.done, cursorType());

    BlockID isEmpty = builder().addBlock();
    BlockID hasElements = builder().addBlock();
    emitBranchOnByte(afterBracket, ']', isEmpty, hasElements);

    builder().moveToEndOf(isEmpty);
    builder().emitJump(loop.done, { afterBracket });

    builder().moveToEndOf(hasElements);
    builder().emitJump(loop.header, { afterBracket });

    builder().moveToEndOf(loop.header);
    builder().append(Opcode::LoopHint, { });
    return loop;
}

void JSONParserBuilder::emitElementSeparator(ValueID next, const ArrayLoop& loop, BlockID rejected)
{
    ValueID afterElement = appendSkipWhitespace(next);
    BlockID hasComma = builder().addBlock();
    BlockID hasNoComma = builder().addBlock();
    emitBranchOnByte(afterElement, ',', hasComma, hasNoComma);

    builder().moveToEndOf(hasComma);
    builder().emitJump(loop.header, { appendSkipPunctuator(afterElement) });

    builder().moveToEndOf(hasNoComma);
    emitRequireByte(afterElement, ']', rejected);
    builder().emitJump(loop.done, { afterElement });
}

ParsedValue JSONParserBuilder::emitTuple(const ResultNode& node, ValueID cursor, const ParseExits& exits)
{
    emitRequireByte(cursor, '[', exits.rejected);
    ValueID position = appendSkipPunctuator(cursor);

    Vector<ValueID, 4> elements;
    for (ResultNodeRef child : node.children) {
        if (!elements.isEmpty()) {
            emitRequireByte(position, ',', exits.rejected);
            position = appendSkipPunctuator(position);
        }
        ParsedValue element = emitNodeOrGenericValue(child, position, exits);
        elements.append(element.value);
        position = appendSkipWhitespace(element.next);
    }

    emitRequireByte(position, ']', exits.rejected);
    ValueID next = builder().append(Opcode::JSONSkipByte, { position });
    return { builder().append(makeArrayOf(program(), node, elements.span())), next };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
