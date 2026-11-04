/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, texts that are joined in one block and taken in by a join of texts in another: `const s = c ? "a" + n : "b" + m; return s + "!"`.
// A text that was joined of numbers and written texts alone has no identity, and joining it neither throws nor runs code. So the later join takes its parts, and the sweep drops the text that nobody reads any more.
// Nothing waits and nothing is noted: the record of the text lists its parts, which stand in front of it wherever it is named. At a suspension: the parts are values like any other.
// At a throw: nothing is moved, the later join stands where it is written. Withheld: each join takes the text as one part.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// The walk stands in a loop that was entered behind the record, so what is built here of the operands of the record is built in every turn.
bool Walker::standsInLoopEnteredBehind(Reference reference) const
{
    if (!reference.isValue())
        return false;
    for (const Frame& frame : m_frames) {
        if (frame.placesOfOpenLoops.isEmpty() || !frame.block)
            continue;
        for (LoopID loop = frame.index->loopOf(frame.block); loop; loop = frame.index->loop(loop).parent) {
            uint32_t place = frame.placesOfOpenLoops[loop.index()];
            if (place && reference.id().index() < m_openLoops[place - 1].firstRecord)
                return true;
        }
    }
    return false;
}

// In a loop that was entered since the text was joined every turn would spell the numbers anew. A text that was made as a key is a name that the run time has at hand.
bool isTextBuiltOfParts(Walker& walker, Reference text)
{
    if (!walker.has(Mechanism::TypedOperations) || !text || !text.isValue())
        return false;
    const Code& code = walker.code();
    const TypeArena& types = walker.types();
    const Operation& joined = code.at(text.id());
    if (!operationsThatJoinTexts.contains(joined.opcode) || !joined.type || !types.at(joined.type).isAnyString())
        return false;
    if (joined.opcode == Opcode::Strcat && joined.isLikelyKey())
        return false;

    for (ValueID part : joined.operands()) {
        if (!part)
            return false;
        const Operation& record = code.at(part);
        if (code.isConstant(part) && record.opcode == Opcode::String)
            continue;
        if (!record.type || isNeverType(record.type) || !types.at(record.type).hasOnlyPrimitives(Primitive::NumberLike))
            return false;
    }
    return !walker.standsInLoopEnteredBehind(text);
}

// By the only-user links of the body, back through casts and over the edges into one block. It decides what pays, never what is right: a text that somebody else reads is joined for him,
// and whoever took its parts beside that would spell the numbers a second time.
static bool isMentionedOnceOnEveryWayInKnownTexts(Walker& walker, const BodyIndex& index, ValueID text)
{
    while (!index.isParameter(text)) {
        if (!index.hasOneUse(text))
            return false;
        if (!opcodesThatStateTypes.contains(index.opcodeOf(text)))
            return true;
        text = index.value(text).operand(0);
        if (!text)
            return false;
    }

    BlockID block = index.blockOf(text);
    if (!index.hasOneUse(text) || block == entryBlock || index.isLoopHead(block) || index.isEnteredByHandlerEdge(block) || index.block(block).isHandlerBlock())
        return false;
    uint32_t parameter = *index.block(block).indexOfParameter(text);
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, block)) {
        std::span<const ValueID> arguments = edge.target.arguments();
        if (parameter >= arguments.size() || !index.hasOneUse(arguments[parameter]))
            return false;
    }
    return true;
}

// `parts`: one for each operand of the join, in their order. True: in the place of each text that the join alone reads and that was built of parts stand its parts.
bool takePartsOfTextsThatJoinAloneReads(Walker& walker, const BodyValue& join, Vector<Reference, 8>& parts, size_t mostParts)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Code& code = walker.code();
    Vector<Reference, 8> longer;
    size_t numberOfOperands = 0;
    bool hasTakenTextApart = false;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        size_t position = numberOfOperands++;
        if (position >= parts.size())
            return;
        Reference part = parts[position];
        if (!isTextBuiltOfParts(walker, part) || !isMentionedOnceOnEveryWayInKnownTexts(walker, index, operand)) {
            longer.append(part);
            return;
        }
        for (ValueID inner : code.at(part.id()).operands())
            longer.append(code.isConstant(inner) ? Reference::constant(inner) : Reference::value(inner));
        hasTakenTextApart = true;
    });
    if (!hasTakenTextApart || numberOfOperands != parts.size() || longer.size() > mostParts)
        return false;
    parts = WTF::move(longer);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
