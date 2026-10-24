/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class DateBuiltinEmitter final : public BuiltinFamilyEmitter {
public:
    explicit DateBuiltinEmitter(CodeGenerator&);

    FOR_EACH_AOT_INLINE_BUILTIN_IN_DATE_BUILTIN(AOT_DECLARE_INLINE_BUILTIN_GENERATOR)

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
