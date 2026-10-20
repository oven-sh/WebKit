/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

namespace JSC {

class CCallHelpers;

namespace B3 { namespace Air {

class Code;
struct GenerationContext;
struct Inst;

JS_EXPORT_PRIVATE void emitFrameSetup(CCallHelpers&, GenerationContext&, const Inst& enterFrame);
JS_EXPORT_PRIVATE void emitFrameTeardown(CCallHelpers&, const Code&);
JS_EXPORT_PRIVATE void emitCalleeSaveRestore(CCallHelpers&, const Code&);
JS_EXPORT_PRIVATE unsigned maxBytesOfFrameTeardown(const Code&);

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
