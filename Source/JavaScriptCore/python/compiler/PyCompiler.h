/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BytecodeGenerator.h"
#include "BytecodeGeneratorInlines.h"
#include "PyBytecode.h"
#include "PyCodeInfo.h"
#include "PySymbolTable.h"

namespace JSC {
namespace Python {

class PyTuple;
class Runtime;
class Unit;

struct CompilerOptions {
    int optimize { 0 }; // 1: no asserts, and __debug__ is False. 2: no docstrings either.
    bool allowTopLevelAwait { true };
    // What the code calling compile() has in effect itself.
    bool futureAnnotations { false };
    bool futureBarryAsBDFL { false };
};

// From a syntax tree to bytecode. (Python/codegen.c and Python/compile.c in CPython are the reference for what the code must do.)
class Compiler {
    WTF_MAKE_NONCOPYABLE(Compiler);
public:
    // `source` is the text of the tree as the lexer read it, a byte to a character, so that the positions in the tree are offsets in it.
    Compiler(JSGlobalObject*, ParseTree&, ParserHost&, const SourceCode& source, const String& filename, const CompilerOptions&);
    ~Compiler();

    // The code of the module: a function of one parameter, the namespace to run in. Null: error() says why.
    UnlinkedFunctionExecutable* compile();
    const SyntaxErrorInfo& error() const { return m_error; }
    const FutureFeatures& futureFeatures() const { return m_future; }

    using Name = AST::Name;
    using Range = AST::Range;

    enum class UnitKind : uint8_t { Module, Class, Function, AsyncFunction, Lambda, Comprehension, Annotations };

    struct CodeSpec {
        UnitKind kind;
        Scope* scope;
        Name name;
        Range range; // Of the source of the code.
        int firstLineNumber;
        Name privateName { nullptr }; // Null: that of the enclosing code.
        AST::Arguments* arguments { nullptr };
        unsigned positionalOnlyCount { 0 }; // Without `arguments`: the parameters are those of the scope, all positional.
        String docstring { };
        // A generator expression: what it does with its parameter before it makes the generator.
        enum class IterateFirstParameter : uint8_t { No, Sync, Async };
        IterateFirstParameter iterateFirstParameter { IterateFirstParameter::No };
    };

private:
    friend class Unit;
    friend class ControlFlowInFinally;

    // Compiles a function, a class body or the like. `emitBody` emits what follows the binding of the arguments.
    UnlinkedFunctionExecutable* compileCode(Unit* parent, const CodeSpec&, const SourceCode& parentSource, const Function<void(Unit&)>& emitBody);
    UnlinkedFunctionExecutable* generate(const SourceCode& parentSource, const SourceCode&, SourceParseMode, Ref<CodeInfo>&&, unsigned parameterCount, bool isGeneratorBody, Function<void(BytecodeGenerator&)>&&, unsigned* generatorSlotCount = nullptr);

    void fail(const Range&, const String& message);
    void failWithoutLocation(SyntaxErrorInfo::Type, const String& message);
    bool failed() const { return !!m_error; }
    void warn(const Range&, const String& message);

    unsigned offsetOf(int line, int column) const;
    SourceCode sourceFor(const Range&) const;

    const Identifier& identifier(ASCIILiteral);
    Name name(ASCIILiteral text) { return &identifier(text); }
    Name name(const String&);

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    Runtime& m_runtime;
    ParseTree& m_tree;
    ParserHost& m_host;
    SourceCode m_source;
    String m_filename;
    CompilerOptions m_options;
    FutureFeatures m_future;
    std::unique_ptr<SymbolTable> m_symbols;
    SyntaxErrorInfo m_error;
    UncheckedKeyHashSet<const void*> m_warned; // Code in a `finally` clause is compiled more than once.
    bool m_isInteractive { false };
};

} // namespace Python
} // namespace JSC
