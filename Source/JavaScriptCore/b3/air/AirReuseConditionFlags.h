/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 { namespace Air {

class Code;
struct Inst;

JS_EXPORT_PRIVATE bool reuseConditionFlags(Code&);

// x86-64: the last machine instruction of the operation leaves the zero and the sign flag as a test of its result would. The other flags are anybody's.
JS_EXPORT_PRIVATE bool setsFlagsToResult(const Inst&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
