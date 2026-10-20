/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC { namespace B3 { namespace Air {

class Code;

JS_EXPORT_PRIVATE bool foldTrapsIntoAccesses(Code&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
