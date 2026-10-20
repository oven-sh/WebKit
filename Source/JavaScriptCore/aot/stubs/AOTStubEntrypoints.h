/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubKey.h"
#include "JSExportMacros.h"
#include "MacroAssemblerCodeRef.h"

namespace JSC { namespace AOT {

struct Instance;

JS_EXPORT_PRIVATE void* addressOfStubInCurrentImage(StubKey);

CodePtr<JSEntryPtrTag> closureEntrypoint();
JS_EXPORT_PRIVATE CodePtr<JSEntryPtrTag> slimClosureEntrypoint();
CodePtr<JSEntryPtrTag> constructEntrypoint();

void initializeStubWords(Instance&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
