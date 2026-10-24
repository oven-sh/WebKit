/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTSchemaInlineBuiltins.h"

namespace JSC { namespace AOT { namespace Typed {

class DateBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit DateBuiltinEmitter(CodeGenerator&);

    void generateDateGetField(const InlineCall&);
    void generateDateToISOString(const InlineCall&);
    void generateDateNow(const InlineCall&);
    void generateDateGetTime(const InlineCall&);
    void generateDateGetMilliseconds(const InlineCall&);
    void generateDateSetTime(const InlineCall&);

private:
    struct BitField {
        unsigned offset;
        unsigned width;
        bool isSigned;
        Range range;
    };

    static BitField bitFieldOf(DateField);
    TypedPointer fieldAt(CellPointer date, ptrdiff_t offset);
    Number timeValueOf(CellPointer date);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
