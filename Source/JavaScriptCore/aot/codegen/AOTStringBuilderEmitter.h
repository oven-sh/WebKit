/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// A text that grows is appended to a builder: runtime/AOTStringBuilderStorage.h. Where the buffer has room, code writes the characters itself.
// A builder in the heap keeps its position in the cell. Of one in the frame (StackStringBuilder) each append takes the cursor as a value of Code, so in a register, and the last of a row stores the next one in the record for whoever has no such value.
class StringBuilderEmitter final : public OperationEmitter {
public:
    explicit StringBuilderEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateStringBuilderBegin(const ValueContext&);
    void generateStringBuilderLength(const ValueContext&);
    void generateStringBuilderToString(const ValueContext&);
    void generateStringBuilderFinish(const ValueContext&);
    void generateStringBuilderDiscard(const ValueContext&);
    void generateStringBuilderCheckpoint(const ValueContext&);
    void generateStringBuilderRollBack(const ValueContext&);
    void generateStringBuilderCheckLength(const ValueContext&);
    void generateStackStringBuilderBegin(const ValueContext&);
    void generateStackStringBuilderStart(const ValueContext&);
    void generateStackStringBuilderCursor(const ValueContext&);
    void generateStackStringBuilderLength(const ValueContext&);
    void generateStackStringBuilderToString(const ValueContext&);
    void generateStackStringBuilderFinish(const ValueContext&);
    void generateStackStringBuilderDiscard(const ValueContext&);

    // Of a builder in the frame.
    Pointer recordOf(const ValueContext& value) const { return Pointer(in<CellPointer>(value->builderOperand()).value()); }
    // Only a call moves the characters, so only a call changes it.
    Pointer limitOf(Pointer record);
    // For a cursor behind a write of at most StackStringBuilder::maxBytesWrittenPastLimit bytes: the cursor to go on with.
    Pointer advance(const OperationContext&, Pointer record, Pointer cursor);
    // What an append gives: the cursor behind its part. It is in the record from here on, unless the block goes on at once with a part that takes it: that part sees to its own.
    // `constantsWrittenAhead`: so many parts that the block generates next were written with this one. Each of them hands on the cursor that it takes and stores nothing.
    void defineCursorBehindPart(const ValueContext&, Pointer record, Pointer cursor, unsigned constantsWrittenAhead = 0);

    TypedPointer storageField(CellPointer builder, ptrdiff_t offset) { return m_out.address(m_heaps.AOTStringBuilderStorage_fields, builder.value(), offset); }
    TypedPointer bytesAt(Pointer cursor, ptrdiff_t offset = 0) { return m_out.address(m_heaps.stringBuilderBytes, cursor.value(), offset); }
    // In characters.
    Word32 lengthOf(CellPointer builder);

    // Where the next character goes, with `size` bytes in front of the limit. No text that ends in front of the limit is longer than a string may be.
    Pointer tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom);
    // Room for whole words that hold `length` characters. What appends fewer characters than it asks room for would be refused near the longest text while it fits: see tryReserve().
    Pointer reserve(const OperationContext&, CellPointer builder, uint32_t length);
    // The builder is read behind the last write through the cursor, so it outlives the cursor.
    void commit(CellPointer builder, Pointer cursor);

    // Each returns the cursor behind the characters. The first two write whole words, so up to seven bytes more.
    Pointer writeBytes(Pointer cursor, std::span<const Latin1Character>);
    // The value is below firstIntegerWrittenByCall.
    Pointer writeDigits(Pointer cursor, Word32 value);
    Pointer copyLatin1Characters(Pointer cursor, Pointer source, Word32 length);

    static constexpr unsigned maxInlineDigits = 8;
    static constexpr int64_t firstIntegerWrittenByCall = 100'000'000;

private:
    TypedPointer recordField(Pointer record, ptrdiff_t offset) { return m_out.address(m_heaps.AOTStackStringBuilder_fields, record.value(), offset); }
    Pointer limitOnStack(Pointer record);
    Pointer copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
