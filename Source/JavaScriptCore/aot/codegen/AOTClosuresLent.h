/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;

enum class LoanOfClosure : uint8_t {
    None,
    ToFunctionEntered,
};

struct ClosureLent {
    LoanOfClosure loan { LoanOfClosure::None };
    ValueID call;
};

ClosureLent findLoanOfClosure(const Program&, const Facts&, const Code&, ValueID closure);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
