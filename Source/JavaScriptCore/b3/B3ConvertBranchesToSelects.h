/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

namespace JSC { namespace B3 {

class Procedure;

bool convertBranchesToSelects(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
