/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTStringEmitter.h"

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

    TypedPointer storageField(CellPointer builder, ptrdiff_t offset);
    TypedPointer bytesAt(Pointer cursor, ptrdiff_t offset = 0);
    Word32 lengthOf(CellPointer builder);

    Pointer tryReserve(CellPointer builder, Word32 size, LBasicBlock whenThereIsNoRoom);
    Pointer reserve(const OperationContext&, CellPointer builder, Word32 size);
    void commit(CellPointer builder, Pointer cursor);

    Pointer writeBytes(Pointer cursor, std::span<const Latin1Character>);
    Pointer writeDigits(Pointer cursor, Word32 value);
    Pointer copyLatin1Characters(Pointer cursor, Pointer source, Word32 length);

    static constexpr unsigned maxInlineDigits = 8;
    static constexpr int64_t firstIntegerWrittenByCall = 100'000'000;

private:
    Pointer copyShortLatin1Characters(Pointer cursor, Pointer source, Word32 length);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
