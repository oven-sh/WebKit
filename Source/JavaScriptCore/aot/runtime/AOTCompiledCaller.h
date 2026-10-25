/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSExportMacros.h"

namespace JSC {

class CallFrame;
class VM;

namespace AOT {

#if ENABLE(AOT)
JS_EXPORT_PRIVATE bool isOperationFrameRecord(VM&, const CallFrame* frameRecord);
JS_EXPORT_PRIVATE bool isTopFrameCompiled(VM&);
JS_EXPORT_PRIVATE bool isCallerCompiled(VM&, const CallFrame* calleeFrame);
JS_EXPORT_PRIVATE bool isCompiledCodePC(const void*);
#else
inline bool isOperationFrameRecord(VM&, const CallFrame*) { return false; }
inline bool isTopFrameCompiled(VM&) { return false; }
inline bool isCallerCompiled(VM&, const CallFrame*) { return false; }
#endif

} // namespace AOT
} // namespace JSC
