/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringBuilderEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class JSONWriteEmitter final : public OperationEmitter {
public:
    explicit JSONWriteEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    void generateStringBuilderReserve(const ValueContext&);
    void generateStringBuilderCommit(const ValueContext&);
    void generateJSONAppendKey(const ValueContext&);
    void generateJSONAppendToken(const ValueContext&);
    void generateJSONAppendBoolean(const ValueContext&);
    void generateJSONAppendInteger(const ValueContext& value) { appendNumber(value, true); }
    void generateJSONAppendNumber(const ValueContext& value) { appendNumber(value, false); }
    void generateJSONAppendString(const ValueContext&);
    void generateJSONAppendValue(const ValueContext&);
    void generateJSONAppendObject(const ValueContext&);
    void generateJSONEnterWatchedLevel(const ValueContext&);
    void generateJSONHasStackSpace(const ValueContext&);

private:
    StringBuilderEmitter& builders() { return emitter<StringBuilderEmitter>(); }
    CellPointer builderOf(const ValueContext& value) const { return in<CellPointer>(value->builderOperand()); }
    Pointer cursorOf(const ValueContext& value) const { return in<Pointer>(value->cursorOperand()); }
    void appendNumber(const ValueContext&, bool isInteger);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
