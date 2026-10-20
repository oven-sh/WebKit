/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>

namespace JSC {

class VM;

namespace AOT {

JS_EXPORT_PRIVATE void dumpObjectCensus(VM&, PrintStream&);
JS_EXPORT_PRIVATE void dumpObjectCensusIfRequested(VM&);
JS_EXPORT_PRIVATE void answerObjectCensusRequest(VM&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
