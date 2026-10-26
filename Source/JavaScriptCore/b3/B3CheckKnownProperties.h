/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 {

class Procedure;

JS_EXPORT_PRIVATE bool checkKnownProperties(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
