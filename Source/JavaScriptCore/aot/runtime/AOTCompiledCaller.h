/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSExportMacros.h"

namespace JSC {

class CallFrame;
class VM;
struct EntryFrame;

namespace AOT {

#if ENABLE(AOT)
JS_EXPORT_PRIVATE bool isOperationFrameRecord(VM&, const CallFrame* frameRecord);
JS_EXPORT_PRIVATE bool isTopFrameCompiled(VM&);
// Compiled code calls a frame itself, or calls an operation that enters the VM.
JS_EXPORT_PRIVATE bool isCallerCompiled(VM&, const CallFrame* calleeFrame, EntryFrame*);
JS_EXPORT_PRIVATE bool isCompiledCodePC(const void*);
#else
inline bool isOperationFrameRecord(VM&, const CallFrame*) { return false; }
inline bool isTopFrameCompiled(VM&) { return false; }
inline bool isCallerCompiled(VM&, const CallFrame*, EntryFrame*) { return false; }
#endif

} // namespace AOT
} // namespace JSC
