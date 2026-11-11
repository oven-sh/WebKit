/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

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
    void generateStackStringBuilderBegin(const ValueContext&);
    void generateStackStringBuilderStart(const ValueContext&);
    void generateStackStringBuilderCursor(const ValueContext&);
    void generateStackStringBuilderLength(const ValueContext&);
    void generateStackStringBuilderToString(const ValueContext&);
    void generateStackStringBuilderFinish(const ValueContext&);
    void generateStackStringBuilderDiscard(const ValueContext&);

    Pointer recordOf(const ValueContext& value) const { return Pointer(in<CellPointer>(value->builderOperand()).value()); }
    Pointer limitOf(Pointer record);
    Pointer advance(const OperationContext&, Pointer record, Pointer cursor);
    void defineCursorBehindPart(const ValueContext&, Pointer record, Pointer cursor, unsigned constantsWrittenAhead = 0);

    TypedPointer storageField(CellPointer builder, ptrdiff_t offset) { return m_out.address(m_heaps.AOTStringBuilderStorage_fields, builder.value(), offset); }
    TypedPointer bytesAt(Pointer cursor, ptrdiff_t offset = 0) { return m_out.address(m_heaps.stringBuilderBytes, cursor.value(), offset); }
    Word32 lengthOf(CellPointer builder);

    Pointer tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom);
    Pointer reserve(const OperationContext&, CellPointer builder, uint32_t length);
    void commit(CellPointer builder, Pointer cursor);

    Pointer writeBytes(Pointer cursor, std::span<const Latin1Character>);
    Pointer writeDigits(Pointer cursor, Word32 value);
    Pointer copyLatin1Characters(Pointer cursor, Pointer source, Word32 length);

    static constexpr unsigned maxInlineDigits = 8;
    static constexpr int64_t firstIntegerWrittenByCall = 100'000'000;

private:
    TypedPointer recordField(Pointer record, ptrdiff_t offset) { return m_out.address(m_heaps.AOTStackStringBuilder_fields, record.value(), offset); }
    Pointer limitOnStack(Pointer record);
    void storeEmptyRecord(Pointer record);
    Pointer copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
