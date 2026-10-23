/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

class StringBuilderEmitter final : public OperationEmitter {
public:
    explicit StringBuilderEmitter(CodeGenerator&);

    void generateStringBuilderBegin(const ValueContext&);
    void generateStringBuilderAppendBuiltin(const ValueContext&);
    void generateStringBuilderAppendJSON(const ValueContext&);
    void generateStringBuilderLength(const ValueContext&);
    void generateStringBuilderToString(const ValueContext&);
    void generateStringBuilderFinish(const ValueContext&);
    void generateStringBuilderDiscard(const ValueContext&);
    void generateStringBuilderCheckpoint(const ValueContext&);
    void generateStringBuilderRollBack(const ValueContext&);

    struct StackBuilder {
        B3::Air::StackSlot* slot { nullptr };
        B3::Variable* cursor { nullptr };
        bool readsLength { false };
    };
    // Null for a builder that is a StringBuilderStorage from the start.
    const StackBuilder* stackBuilderFor(ValueID builder);
    Pointer addressOf(const StackBuilder&);
    Pointer cursorOf(const StackBuilder&);
    void setCursor(const StackBuilder&, Pointer);
    Pointer limitOf(const StackBuilder&);
    // For a cursor behind a write of at most StackStringBuilder::maxBytesWrittenPastLimit bytes.
    void advance(const OperationContext&, const StackBuilder&, Pointer cursor);

    TypedPointer storageField(CellPointer builder, ptrdiff_t offset);
    TypedPointer bytesAt(Pointer cursor, ptrdiff_t offset = 0);
    Word32 lengthOf(CellPointer builder);

    Pointer tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom);
    // Room for whole words that hold `length` characters. What appends fewer characters than it asks room for would be refused near the longest text while it fits: see tryReserve().
    Pointer reserve(const OperationContext&, CellPointer builder, uint32_t length);
    Pointer reserve(const OperationContext&, CellPointer builder, Word32 size);
    void commit(CellPointer builder, Pointer cursor);

    Pointer writeBytes(Pointer cursor, std::span<const Latin1Character>);
    Pointer writeDigits(Pointer cursor, Word32 value);
    Pointer copyLatin1Characters(Pointer cursor, Pointer source, Word32 length);

    static constexpr unsigned maxInlineDigits = 8;
    static constexpr int64_t firstIntegerWrittenByCall = 100'000'000;

private:
    void findStackBuilders();
    TypedPointer stackBuilderField(const StackBuilder&, ptrdiff_t offset);
    Pointer copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length);

    static constexpr uint32_t noStackBuilder = UINT32_MAX;

    bool m_hasFoundStackBuilders { false };
    Vector<StackBuilder, 1> m_stackBuilders;
    Vector<uint32_t> m_stackBuilderOf;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
