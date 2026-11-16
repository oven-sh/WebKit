/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

JS_EXPORT_PRIVATE bool recordsOrder();
JS_EXPORT_PRIVATE ASCIILiteral startOrderRecording(const char* path);
JS_EXPORT_PRIVATE void finishOrderRecording();
JS_EXPORT_PRIVATE bool isRecordingOrder();
void recordUseOfRegExp(StringView pattern, uint16_t significantFlags, bool isInImage);
void recordRequestOfModule(uint32_t module);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
