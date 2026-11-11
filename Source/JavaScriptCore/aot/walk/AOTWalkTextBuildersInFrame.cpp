/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet operationsThatGiveCursorsOfTextsInFrame = partsOfStackStringBuilder | OpcodeSet { Opcode::StackStringBuilderStart, Opcode::StackStringBuilderCursor };

static_assert([] {
    bool everyPartWritesBuilder = true;
    partsOfStackStringBuilder.forEach([&](Opcode part) {
        everyPartWritesBuilder &= declaredBoundOf(part).writes.contains(AbstractHeap::StringBuilderContents);
    });
    return everyPartWritesBuilder;
}());
static_assert(declaredBoundOf(Opcode::StackStringBuilderCursor).reads.contains(AbstractHeap::StringBuilderContents));

namespace {

struct CursorInSightOfTextInFrame {
    Reference cursor;
    bool holdsNothingYet { false };
};

} // anonymous namespace

bool textBegunHereMayLieInFrame(Walker& walker, BlockID, const GrowingText& text)
{
    if (opcodesWithoutEmitter().contains(Opcode::StackStringBuilderBegin))
        return false;
    return !text.standsUnderHandlers || everyHandlerLetsTextGo(walker, text);
}

static CursorInSightOfTextInFrame findCursorInSightOfTextInFrame(const Walker& walker, Reference name)
{
    const Code& code = walker.code();
    for (ValueID id = code.lastOperationOf(walker.currentBlock()); id; id = code.at(id).previous) {
        const Operation& record = code.at(id);
        if (id == name.id())
            return { { }, record.opcode == Opcode::StackStringBuilderBegin };
        if (OperandPosition::builder(record.opcode) && record.builderOperand() == name.id()) {
            if (operationsThatGiveCursorsOfTextsInFrame.contains(record.opcode))
                return { Reference::value(id), false };
            if (record.opcode == Opcode::StackStringBuilderLength)
                return { Reference::value(record.cursorOperand()), false };
            return { };
        }
        if (record.traits().mayCollect())
            return { };
    }
    return { };
}

static Reference newestCursorOfTextInFrame(Walker& walker, Reference name, Origin origin)
{
    if (walker.isUnreachable() || !name.isValue())
        return { };
    CursorInSightOfTextInFrame inSight = findCursorInSightOfTextInFrame(walker, name);
    if (inSight.cursor)
        return inSight.cursor;
    Draft read(inSight.holdsNothingYet ? Opcode::StackStringBuilderStart : Opcode::StackStringBuilderCursor, origin);
    read.setBuilderOperand(name);
    return walker.build(WTF::move(read));
}

void becomeOperationOfStringBuilderInFrame(Walker& walker, StringInBuilder& text, Reference name, Draft& draft, Opcode inFrame)
{
    if (!text.liesInFrame)
        return;

    Opcode drafted = draft.opcode;
    Reference value = OperandPosition::value(drafted) ? draft.valueOperand() : Reference();
    Reference string = OperandPosition::string(drafted) ? draft.stringOperand() : Reference();
    Reference start = OperandPosition::start(drafted) ? draft.startOperand() : Reference();
    Reference end = OperandPosition::end(drafted) ? draft.endOperand() : Reference();
    StringID constant = PayloadWord::string(drafted) ? draft.string() : StringID();
    ExpectedType expected = draft.expected;
    ValueID inputValueGiven = draft.inputValueGiven;

    Origin originInFront = draft.origin();
    originInFront.isTailCall = false;
    Reference cursor = OperandPosition::cursor(inFrame) ? newestCursorOfTextInFrame(walker, name, originInFront) : Reference();

    draft.become(inFrame);
    draft.expected = expected;
    draft.inputValueGiven = inputValueGiven;
    if (OperandPosition::builder(inFrame))
        draft.setBuilderOperand(name);
    if (cursor)
        draft.setCursorOperand(cursor);
    if (value)
        draft.setValueOperand(value);
    if (string)
        draft.setStringOperand(string);
    if (start)
        draft.setStartOperand(start);
    if (end)
        draft.setEndOperand(end);
    if (constant)
        draft.setString(constant);
}

Reference finishTextInFrame(Walker& walker, Reference name, Origin origin)
{
    origin.isTailCall = false;
    Reference cursor = newestCursorOfTextInFrame(walker, name, origin);
    if (!cursor)
        return { };
    Draft finish(Opcode::StackStringBuilderFinish, origin);
    finish.setBuilderOperand(name);
    finish.setCursorOperand(cursor);
    return walker.build(WTF::move(finish));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
