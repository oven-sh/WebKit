/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderCursor.h"
#include "AOTStringBuilderEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class JSONWriteEmitter final : public OperationEmitter {
public:
    explicit JSONWriteEmitter(CodeGenerator&);

    void generateStringBuilderReserve(const ValueContext&);
    void generateStringBuilderCommit(const ValueContext&);
    void generateJSONAppendKey(const ValueContext&);
    void generateJSONAppendToken(const ValueContext&);
    void generateJSONAppendBoolean(const ValueContext&);
    void generateJSONAppendInteger(const ValueContext&);
    void generateJSONAppendNumber(const ValueContext&);
    void generateJSONAppendDate(const ValueContext&);
    void generateJSONAppendString(const ValueContext&);
    void generateJSONAppendValue(const ValueContext&);

private:
    StringBuilderEmitter& builders() { return emitter<StringBuilderEmitter>(); }
    CellPointer builderOf(const ValueContext& value) const { return in<CellPointer>(value->builderOperand()); }
    Pointer cursorOf(const ValueContext& value) const { return in<Pointer>(value->cursorOperand()); }

    Pointer closeObjectOverComma(Pointer cursor);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
