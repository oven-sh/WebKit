/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BytecodeIndex.h"
#include "LineColumn.h"

namespace JSC {

class CallFrame;
class CodeBlock;
class FunctionExecutable;
class ScriptExecutable;
class UnlinkedCodeBlock;

namespace AOT {

struct Data;
struct FunctionInfo;
struct Instance;
struct Slot;

// What a frame of code from the static compiler is a frame of: a function, in a realm. There need be nothing in memory that stands
// for it (SharedData).
struct FunctionRef {
    JS_EXPORT_PRIVATE static FunctionRef of(const CallFrame*);
    explicit operator bool() const { return !!instance; }

    // These ask for nothing to be made, take no lock, and can be asked on any thread that has the VM's stopped.
    const FunctionInfo& info() const;
    JS_EXPORT_PRIVATE Data* dataIfItHasAny() const;
    JS_EXPORT_PRIVATE ScriptExecutable* executable() const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* unlinkedCodeBlock() const;
    JS_EXPORT_PRIVATE CodeBlock* codeBlockIfThereIsOne() const;

    JS_EXPORT_PRIVATE LineColumn lineColumnFor(BytecodeIndex) const; // CodeBlock::lineColumnForBytecodeIndex()
    // Its own, which it gets now if it has been doing without. Not while the collector is at work, and on no thread but the VM's.
    JS_EXPORT_PRIVATE Data* ensureData() const;
    // For the functions that the function makes closures of.
    FunctionExecutable* functionDecl(unsigned) const;
    FunctionExecutable* functionExpr(unsigned) const;
    // The number that goes with the slot (ImageFunction::siteConstants()), which is one of the function's.
    uint32_t siteConstantOf(const Slot*) const;

    Instance* instance { nullptr };
    uint32_t index { 0 };
};

} } // namespace JSC::AOT
