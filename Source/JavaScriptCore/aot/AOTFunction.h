/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BytecodeIndex.h"
#include "CodeSpecializationKind.h"
#include "CodeType.h"
#include "Identifier.h"
#include "LineColumn.h"
#include "WriteBarrier.h"
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {

class CallFrame;
class CodeBlock;
class FunctionExecutable;
class ScriptExecutable;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;
struct UnlinkedHandlerInfo;
struct UnlinkedStringJumpTable;

namespace AOT {

struct Data;
struct FunctionFacts;
struct FunctionInfo;
struct Instance;
struct Slot;

// Where an object is made together with its first properties: which they are. It is what the bytecode says, for whoever makes the
// object the long way, without the bytecode. One word, and then one for each property.
struct AllocationPlan {
    static uint32_t encode(unsigned inlineCapacity, unsigned count) { return inlineCapacity << 16 | count; }
    static uint32_t encode(unsigned identifier, bool isDefined, bool isStrict) { return identifier << 2 | isDefined << 1 | isStrict; }

    explicit operator bool() const { return !!words; }
    unsigned inlineCapacity() const { return words[0] >> 16; }
    unsigned count() const { return words[0] & 0xffff; }
    unsigned identifier(unsigned i) const { return words[i + 1] >> 2; }
    bool isDefined(unsigned i) const { return words[i + 1] & 2; } // As NewObjectPlan::Property.
    bool isStrict(unsigned i) const { return words[i + 1] & 1; }

    const uint32_t* words { nullptr };
};

// What a frame of code from the static compiler is a frame of: a function, in a realm. There need be nothing in memory that stands
// for it (SharedData).
struct FunctionRef {
    JS_EXPORT_PRIVATE static FunctionRef of(const CallFrame*);
    JS_EXPORT_PRIVATE static FunctionRef of(CodeBlock*); // None, unless its code is the static compiler's.
    // What has run as this executable's code of this kind. None, if its realm is no more.
    JS_EXPORT_PRIVATE static FunctionRef of(VM&, FunctionExecutable*, CodeSpecializationKind);
    explicit operator bool() const { return !!instance; }

    // These ask for nothing to be made, take no lock, and can be asked on any thread that has the VM's stopped.
    const FunctionInfo& info() const;
    JS_EXPORT_PRIVATE Data* dataIfItHasAny() const;
    JS_EXPORT_PRIVATE ScriptExecutable* executable() const;
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* unlinkedCodeBlockIfThereIsOne() const; // There is, if there are no facts().
    JS_EXPORT_PRIVATE CodeBlock* codeBlockIfThereIsOne() const;
    const FunctionFacts* facts() const;
    // What the unlinked code says, whether it is there or not.
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isBuiltinFunction() const;
    JS_EXPORT_PRIVATE unsigned instructionsSize() const;
    JS_EXPORT_PRIVATE const UnlinkedHandlerInfo* handlerFor(unsigned bytecodeOffset) const;
    const UnlinkedStringJumpTable& stringSwitchJumpTable(unsigned) const;
    JS_EXPORT_PRIVATE const IdentifierSet& constantIdentifierSet(unsigned) const;
    // Of the body of an async function that is waiting in that state: where it goes on from. Nowhere in particular: the beginning.
    JS_EXPORT_PRIVATE BytecodeIndex resumePointOf(int32_t state) const;
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionDecls() const;
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functionExprs() const;

    JS_EXPORT_PRIVATE LineColumn lineColumnFor(BytecodeIndex) const; // CodeBlock::lineColumnForBytecodeIndex()
    // Decoded now, if it is not there. For what nothing else will do for, which is little. As for ensureData().
    JS_EXPORT_PRIVATE UnlinkedCodeBlock* ensureUnlinkedCodeBlock() const;
    UnlinkedCodeBlock* makeUnlinkedCodeBlockFromFacts() const;
    JS_EXPORT_PRIVATE CodeBlock* ensureCodeBlock() const;
    // Its own, which it gets now if it has been doing without. Not while the collector is at work, and on no thread but the VM's.
    JS_EXPORT_PRIVATE Data* ensureData() const;
    // For the functions that the function makes closures of.
    FunctionExecutable* functionDecl(unsigned) const;
    FunctionExecutable* functionExpr(unsigned) const;
    // The number that goes with the slot (ImageFunction::siteConstants()), which is one of the function's.
    uint32_t siteConstantOf(const Slot*) const;
    // What the source says there, and whether it is exactly that: of a program that goes without its text (Image::quoteAt()).
    JS_EXPORT_PRIVATE std::optional<std::pair<String, bool>> quoteAt(BytecodeIndex) const;
    // Whether the instruction there is one of those that construct: for whoever would look at it, if it were there to look at.
    JS_EXPORT_PRIVATE bool constructsAt(BytecodeIndex) const;
    AllocationPlan planOf(const Slot* firstOfSite) const; // None, if the code is not from an image: then there is bytecode.

    Instance* instance { nullptr };
    uint32_t index { 0 };
};

} } // namespace JSC::AOT
