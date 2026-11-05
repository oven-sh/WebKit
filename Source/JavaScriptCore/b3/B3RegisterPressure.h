/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Bank.h"
#include <algorithm>

namespace JSC { namespace B3 {

class Procedure;
class Value;

struct RegisterBudget {
    unsigned gprs { 0 };
    unsigned fprs { 0 };

    unsigned of(Bank bank) const { return bank == GP ? gprs : fprs; }

    void restrictTo(const RegisterBudget& other)
    {
        gprs = std::min(gprs, other.gprs);
        fprs = std::min(fprs, other.fprs);
    }

    friend bool operator==(const RegisterBudget&, const RegisterBudget&) = default;
};

JS_EXPORT_PRIVATE RegisterBudget allocatableRegisters(const Procedure&);
JS_EXPORT_PRIVATE RegisterBudget registersPreservedBy(const Procedure&, const Value*);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
