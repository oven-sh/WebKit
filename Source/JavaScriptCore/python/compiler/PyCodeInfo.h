/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Identifier.h"
#include "PyBytecode.h"
#include "VirtualRegister.h"
#include <wtf/FixedVector.h>
#include <wtf/OptionSet.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>

namespace JSC {
namespace Python {

// The values are those of co_flags.
enum class CodeFlag : uint32_t {
    Optimized = 0x1, // The variables are in registers, not in a namespace.
    NewLocals = 0x2,
    VarArgs = 0x4,
    VarKeywords = 0x8,
    Nested = 0x10,
    Generator = 0x20,
    Coroutine = 0x80,
    IterableCoroutine = 0x100,
    AsyncGenerator = 0x200,
    HasDocstring = 0x4000000,
    Method = 0x8000000,
    FutureBarryAsBDFL = 0x400000,
    FutureAnnotations = 0x1000000,
};

class CodeMonitor;

// What a code object knows besides its bytecode. It hangs off the UnlinkedFunctionExecutable of the code.
class CodeInfo final : public RefCounted<CodeInfo> {
    WTF_MAKE_TZONE_ALLOCATED(CodeInfo);
public:
    static Ref<CodeInfo> create() { return adoptRef(*new CodeInfo); }
    JS_EXPORT_PRIVATE ~CodeInfo();

    enum class VariableKind : uint8_t {
        Local, // The register holds the value, or nothing.
        Cell, // The register holds a cell made by this code.
        Free, // The register holds a cell of the closure.
    };
    struct Variable {
        Identifier name;
        VirtualRegister reg;
        VariableKind kind;
        bool isHidden { false }; // Of an inlined comprehension: not one of the locals of the frame.
    };

    bool isGeneratorLike() const { return flags.containsAny({ CodeFlag::Generator, CodeFlag::Coroutine, CodeFlag::AsyncGenerator }); }
    unsigned parameterCount() const { return argCount + kwOnlyCount + flags.contains(CodeFlag::VarArgs) + flags.contains(CodeFlag::VarKeywords); }

    Identifier name;
    String qualname;
    String filename;
    String docstring; // Null: none.
    OptionSet<CodeFlag> flags;
    int firstLineNumber { 1 };
    // code.replace(co_firstlineno=n) moves every line of the copy: this is added to the lines that the compiler recorded.
    int lineDelta { 0 };

    // Positional parameters (those that are positional-only first), then keyword-only ones, then *args, then **kwargs.
    // They are the parameters of the JS function, in this order.
    unsigned argCount { 0 };
    unsigned posOnlyCount { 0 };
    unsigned kwOnlyCount { 0 };
    Vector<Identifier> parameterNames;

    // A module, a class body, or anything given to exec(): the one parameter is the namespace the names live in.
    bool takesNamespace { false };
    // There is a `for` over the value of a call: see beginTemporary() in the compiler. An exception that leaves such a loop has something to do.
    bool hasLoopThatOwnsItsIterator { false };
    // Once somebody has looked: blockPiecesOf() in PyTemporaries.cpp.
    bool knowsBlockPieces { false };
    FixedVector<BlockPiece> blockPieces;
    VirtualRegister namespaceRegister; // Where the running code keeps it.

    Vector<Variable> variables;
    Vector<Identifier> freeVariables; // In the order of the closure.
    Vector<Identifier> names; // co_names

    // co_consts, before those are left out that CPython has no use for (see codeGetConsts()).
    struct Constant {
        VirtualRegister reg; // One of the constants of the code block. Not valid: it is a code object,
        unsigned functionIndex { 0 }; // that of this one of the functions of the code block.
        bool isUsed { true }; // False: it only went into making another constant.
    };
    Vector<Constant> constants;
    // The constants are no longer those of the text: code.replace(co_consts=...).
    bool hasReplacedConstants { false };
    // It has put other code in the place of one of the functions. That has a text of its own, so marshal,
    // which writes one text for all the code, cannot write this.
    bool hasFunctionsFromElsewhere { false };

    // A generator function only binds its arguments. The body is another function, this index among those of the code block.
    static constexpr unsigned generatorBodyIndex = 0;

    // Once sys.monitoring has been asked to tell of anything that happens in the code: see runtime/PyMonitoring.h.
    std::unique_ptr<CodeMonitor> monitor;

private:
    JS_EXPORT_PRIVATE CodeInfo();
};

} // namespace Python
} // namespace JSC
