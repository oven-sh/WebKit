/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInlineCall.h"
#include "AOTOperationEmitter.h"
#include "PropertyOffset.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class BuiltinFamilyEmitter : public OperationEmitter {
protected:
    explicit BuiltinFamilyEmitter(CodeGenerator&);

    Number numberOperand(ValueID);
    Integer integerOperand(ValueID);
    Word32 int32Operand(ValueID);
    Condition booleanOperand(ValueID);
    CellPointer cellOperand(ValueID);
    Boxed boxedOperand(const OperationContext&, ValueID);

    CellPointer objectOrElse(ValueID, LBasicBlock otherwise);
    CellPointer objectOrMiss(const InlineCall&, ValueID);
    void missUnless(const InlineCall&, Condition mustHold);

    Operand inRepresentation(const OperationContext&, Representation, Operand produced, const Type& producedType);
    void defineAs(const ValueContext&, Operand produced, const Type& producedType);

    void finish(const InlineCall&, Operand produced, const Type& producedType);
    void finish(const InlineCall&, Number);
    void finish(const InlineCall&, Integer, Range);
    void finish(const InlineCall&, Condition);
    void finish(const InlineCall&, CellPointer);
    void finish(const InlineCall&, Boxed);
    void finishWithUndefined(const InlineCall&);
    void finishWithResults(const InlineCall&, std::span<const Operand>);

    TypedPointer inlineProperty(CellPointer object, PropertyOffset);

private:
    CellPointer getCell(ValueID);
};

static_assert(sizeof(BuiltinFamilyEmitter) == sizeof(OperationEmitter));

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
