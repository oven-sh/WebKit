/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFactRecords.h"
#include "AOTLinkTables.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndexTable;
class Program;

struct LocaleOperandOfRow {
    BuiltinID row;
    BuiltinCallMode mode;
    BuiltinOperand operand;
    bool mayReplaceLocale { false };

    friend bool operator==(const LocaleOperandOfRow&, const LocaleOperandOfRow&) = default;
};

struct RowsThatTakeLocales {
    explicit RowsThatTakeLocales(const BuiltinTable&);

    BitVector rows;
    Vector<LocaleOperandOfRow> operands;
};

struct LocalesPassedAtCalls {
    Vector<LocaleUse> uses;
    BitVector rowsWithCallsLookedAt;
    BitVector rowsCalledWhereNobodySeesOperands;
};

LocalesPassedAtCalls localesPassedAtCalls(const Program&, const BodyIndexTable&, const RowsThatTakeLocales&, std::span<const LinkedCallOfBuiltin>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
