/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "FFIStubs.h"

#if USE(BUN_JSC_ADDITIONS) && FFI_STUBS_SUPPORTED

#include "CCallHelpers.h"
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace FFI {

using ContextAddressEmitter = ScopedLambda<void(CCallHelpers&, GPRReg destination)>;

JS_EXPORT_PRIVATE void emitInvokeStub(CCallHelpers&);
JS_EXPORT_PRIVATE Vector<CCallHelpers::Label> emitCallbackEntries(CCallHelpers&, unsigned numberOfEntries, const ContextAddressEmitter&);

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && FFI_STUBS_SUPPORTED
