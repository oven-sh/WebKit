/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

struct ByteOrder {
    std::optional<bool> isLittle;
    Word32 isLittleAtRunTime;
};
bool mayBeNegative(ValueID) const;
Pointer findBytes(CellPointer view, Integer offset, bool mayBeNegative, unsigned size, LBasicBlock otherwise);
LValue byteSwapped(LValue bits, unsigned size);
LValue inByteOrder(LValue bits, unsigned size, const ByteOrder&);
Operand loadBytes(Pointer, TypedArrayType, const ByteOrder&);
void storeBytes(Pointer, TypedArrayType, Operand, const ByteOrder&);
Word32 isNegativeBigInt(CellPointer);
Integer low64Of(CellPointer bigInt);
CellPointer newBigInt(Integer bits, bool isSigned);

std::optional<unsigned> bufferAccessorNamed(StringID) const;
Boxed getBufferAccessor(Boxed base, StringID, unsigned row, bool isCalled);
std::optional<Operand> lowerBufferAccess(ValueID buffer, StringID, std::optional<Boxed> method, const Arguments&, const Type& result);
