/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTRegExpSection.h"
#include "YarrJIT.h"

namespace JSC::AOT {

JS_EXPORT_PRIVATE void jitCompileThroughImage(StringView pattern, OptionSet<Yarr::Flags>, Yarr::YarrCodeBlock&);

JS_EXPORT_PRIVATE const uint8_t* loadRegExpSection(const RegExpSection&);

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
