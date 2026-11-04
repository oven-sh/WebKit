/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations with LoopInvariants: the builder of a text that grows lies in the frame of the unit (StackStringBuilder of runtime/AOTStringBuilderStorage.h). Everybody drafts the operations of a builder in the heap, and they become those of the frame here.
// Nothing is kept: the newest cursor is found among the records of the block in which the walk stands, and in any other block it is read out of the record in the frame, where the last part of a row has stored it.
// At a suspension: the slot is dead there, so the unit of a coroutine has no such builder. At a throw: behind a part that has thrown nobody uses the record, so no text lies in the frame under a handler that is handed it, or that goes on in its loop.
// Withheld: no text lies in a builder, and nobody asks.

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

// StringBuilderStorage::StringUse::LikelyKey.
static constexpr uint32_t stringUseOfTextInFrameReadAsKey = 1;

static constexpr OpcodeSet operationsThatGiveCursorsOfTextsInFrame = partsOfStackStringBuilder | OpcodeSet { Opcode::StackStringBuilderStart, Opcode::StackStringBuilderCursor };

// So a part is never dropped and never built in front of a loop, and a read of the cursor stays in every loop that appends.
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
    // Null: none is in sight.
    Reference cursor;
    // StackStringBuilderBegin stands in the block, and nothing was appended behind it.
    bool holdsNothingYet { false };
};

} // anonymous namespace

// A frame is not kept across a suspension. An append that throws leaves a record that nobody may read, and the text of JSON.stringify() is written through the cell of a builder.
bool textBegunHereMayLieInFrame(Walker& walker, BlockID, const GrowingText& text)
{
    constexpr bool textsMayLieInFrame = true;
    if (!textsMayLieInFrame)
        return false;
    if (opcodesWithoutEmitter().contains(Opcode::StackStringBuilderBegin))
        return false;
    if ((text.standsUnderHandlers && !everyHandlerLetsTextGo(walker, text)) || text.takesTextOfJSONStringify)
        return false;
    return walker.unit().role != CodeRole::CoroutineBody && !walker.program().at(walker.unit().function).isCoroutine();
}

// From the last record of the block back to the last that speaks of the builder. No cursor is carried past what may collect: the record in the frame has it.
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

// Null: unreachable or failed.
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

// The draft keeps what it says of the part: whoever has drafted an append has named the value, the text or the range.
bool becomeOperationOfStringBuilderInFrame(Walker& walker, TextInStringBuilder& text, Reference name, Draft& draft, Opcode inFrame)
{
    if (!text.liesInFrame)
        return false;

    Opcode drafted = draft.opcode;
    Reference value = OperandPosition::value(drafted) ? draft.valueOperand() : Reference();
    Reference string = OperandPosition::string(drafted) ? draft.stringOperand() : Reference();
    Reference start = OperandPosition::start(drafted) ? draft.startOperand() : Reference();
    Reference end = OperandPosition::end(drafted) ? draft.endOperand() : Reference();
    StringID constant = PayloadWord::string(drafted) ? draft.string() : StringID();
    bool isLikelyKey = FlagMask::isLikelyKey(drafted) && draft.isLikelyKey();
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
    if (inFrame == Opcode::StackStringBuilderFinish)
        draft.setStringUse(isLikelyKey ? stringUseOfTextInFrameReadAsKey : 0);
    return true;
}

// `let out = prefix; for (..) ..`: the string that was appended first, and the cursor behind it. A piece that was appended as a range of its string is never made, so it is no first part.
void noteCursorBehind(Walker& walker, TextInStringBuilder& text, Reference cursor)
{
    if (!text.liesInFrame || !cursor || !cursor.isValue())
        return;
    const Code& code = walker.code();
    const Operation& part = code.at(cursor.id());
    if (part.opcode != Opcode::StackStringBuilderAppendString || code.at(part.cursorOperand()).opcode != Opcode::StackStringBuilderStart)
        return;
    ValueID first = part.valueOperand();
    text.firstPart = code.isConstant(first) ? Reference::constant(first) : Reference::value(first);
    text.endOfFirstPart = cursor;
}

// `let out = prefix; for (..) if (..) out += x; return out`: where nothing with characters has followed the string that the builder was begun with, that string is the text: one operation compares the cursors and hands it back.
// It is not asked behind a part that always has characters. A builder in the heap keeps the string that it was begun with by itself: StringBuilderStorage::validLastString().
Reference lastPartOrFinishedText(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    static constexpr OpcodeSet partsThatAlwaysHaveCharacters {
        Opcode::StackStringBuilderAppendConstant, Opcode::StackStringBuilderAppendInteger, Opcode::StackStringBuilderAppendNumber, Opcode::StackStringBuilderAppendBoolean
    };
    constexpr bool handsBackStringThatTextBeganWith = true;
    if (!handsBackStringThatTextBeganWith)
        return { };
    if (!text.liesInFrame || !text.firstPart || !text.endOfFirstPart)
        return { };
    origin.isTailCall = false;
    Reference cursor = newestCursorOfTextInFrame(walker, name, origin);
    if (!cursor || partsThatAlwaysHaveCharacters.contains(walker.code().at(cursor.id()).opcode))
        return { };

    Draft finish(Opcode::StackStringBuilderFinish, origin);
    finish.setBuilderOperand(name);
    finish.setCursorOperand(cursor);
    finish.setFirstPartOperand(text.firstPart);
    finish.setEndOfFirstPartOperand(text.endOfFirstPart);
    finish.setStringUse(0);
    return walker.build(WTF::move(finish));
}

Reference finishTextInFrame(Walker& walker, TextInStringBuilder& text, Reference name, Origin origin)
{
    if (Reference string = lastPartOrFinishedText(walker, text, name, origin))
        return string;
    origin.isTailCall = false;
    Reference cursor = newestCursorOfTextInFrame(walker, name, origin);
    if (!cursor)
        return { };
    Draft finish(Opcode::StackStringBuilderFinish, origin);
    finish.setBuilderOperand(name);
    finish.setCursorOperand(cursor);
    finish.setStringUse(0);
    return walker.build(WTF::move(finish));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
