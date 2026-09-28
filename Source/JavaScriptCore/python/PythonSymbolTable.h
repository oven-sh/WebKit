/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "PythonAST.h"
#include "PythonArena.h"
#include "PythonToken.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class VM;

namespace Python {

// What every name in a piece of source refers to. This is Python/symtable.c of CPython, which is where the language's rules for it
// are written down, and the `symtable` module has to give out what it finds.

using NameSet = HashSet<UniquedStringImpl*>;

struct RefCountedNameSet : RefCounted<RefCountedNameSet> {
    NameSet names;
};

// What is known about a name from how a block uses it.
enum SymbolFlag : unsigned {
    DefGlobal = 1, // global statement
    DefLocal = 2, // Assigned to.
    DefParameter = 2 << 1,
    DefNonlocal = 2 << 2, // nonlocal statement
    Use = 2 << 3,
    DefFreeClass = 2 << 5, // Free in a method of the class, which has something else by the same name.
    DefImport = 2 << 6,
    DefAnnotation = 2 << 7,
    DefComprehensionIteration = 2 << 8,
    DefTypeParameter = 2 << 9,
    DefComprehensionCell = 2 << 10, // A cell in a comprehension that has become part of this block.
    DefBound = DefLocal | DefParameter | DefImport,
};

// What it comes to.
enum class NameScope : uint8_t {
    Unknown = 0,
    Local = 1,
    GlobalExplicit = 2,
    GlobalImplicit = 3, // In no enclosing function, so it is the module's or a builtin.
    Free = 4, // An enclosing function's.
    Cell = 5, // Local, and an enclosed function uses it.
};

struct Symbol {
    const Identifier* name { nullptr };
    unsigned flags { 0 };
    NameScope scope { NameScope::Unknown };
};

enum class BlockType : uint8_t {
    Function,
    Class,
    Module,
    Annotation, // What evaluates the annotations of something, when they are asked for.
    TypeAlias, // type X = this
    TypeParameters, // What [T] of def f[T]() or class C[T] is in scope in.
    TypeVariable, // The bound, the constraints or the default of a type parameter.
};

enum class ComprehensionType : uint8_t { None, List, Dict, Set, Generator };

struct Block {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Block);

    bool isFunctionLike() const { return type != BlockType::Class && type != BlockType::Module; }

    Symbol* find(const Identifier& name)
    {
        auto iterator = index.find(name.impl());
        return iterator == index.end() ? nullptr : &symbols[iterator->value];
    }
    unsigned flagsOf(const Identifier& name)
    {
        Symbol* symbol = find(name);
        return symbol ? symbol->flags : 0;
    }
    NameScope scopeOf(const Identifier& name)
    {
        Symbol* symbol = find(name);
        return symbol ? symbol->scope : NameScope::Unknown;
    }
    Symbol& add(const Identifier& name)
    {
        auto result = index.add(name.impl(), symbols.size());
        if (result.isNewEntry)
            symbols.append(Symbol { &name, 0, NameScope::Unknown });
        return symbols[result.iterator->value];
    }
    void remove(const Identifier&);

    BlockType type { BlockType::Module };
    ComprehensionType comprehension { ComprehensionType::None };
    const Identifier* name { nullptr };
    Node location;

    Vector<Symbol> symbols; // In the order they were first seen.
    HashMap<UniquedStringImpl*, unsigned> index;
    Vector<const Identifier*> parameters;
    Vector<Block*> children;
    Block* annotationBlock { nullptr };

    struct Directive {
        const Identifier* name;
        Node location;
    };
    Vector<Directive> directives; // global and nonlocal statements, to point at when one is wrong.

    // Around a generic class, only its type parameters are mangled. Null where every name is.
    RefPtr<RefCountedNameSet> mangledNames;
    ASCIILiteral scopeInfo; // "a TypeVar bound" and so on, for messages.

    bool isNested : 1 { false };
    bool isGenerator : 1 { false };
    bool isCoroutine : 1 { false };
    bool usesAnnotations : 1 { false };
    bool hasVariadic : 1 { false };
    bool hasKeywordVariadic : 1 { false };
    bool returnsValue : 1 { false };
    bool needsClassClosure : 1 { false }; // A class whose methods use __class__, which super() does.
    bool needsClassDict : 1 { false };
    bool isInlinedComprehension : 1 { false };
    bool isVisitingComprehensionTarget : 1 { false };
    bool canSeeClassScope : 1 { false };
    bool hasDocstring : 1 { false };
    bool isMethod : 1 { false };
    bool hasConditionalAnnotations : 1 { false };
    bool isInConditionalBlock : 1 { false };
    bool isInUnevaluatedAnnotation : 1 { false };
    unsigned comprehensionIterableDepth { 0 };
};

enum FutureFeature : unsigned {
    FutureAnnotations = 1 << 0,
    FutureBarryAsFLUFL = 1 << 1,
    AllowTopLevelAwait = 1 << 2,
};

class SymbolTable {
    WTF_MAKE_TZONE_ALLOCATED(SymbolTable);
    WTF_MAKE_NONCOPYABLE(SymbolTable);
public:
    // Null if some use of a name is against the rules, and then the error says which.
    static std::unique_ptr<SymbolTable> build(VM&, Arena&, Module&, unsigned futureFeatures, SyntaxError&);

    // Of one definition or expression out of the middle of a source. What has to be known of the blocks it was in is which of its names
    // are variables of functions among them, and the class that private names are mangled for, if any. One of the two is null.
    static std::unique_ptr<SymbolTable> buildFragment(VM&, Arena&, Statement*, Expression*, const Vector<Identifier>& freeVariables, const Identifier* privateName, unsigned futureFeatures);

    Block& top() { return *m_top; }
    unsigned futureFeatures() const { return m_futureFeatures; }

    // The block that a node of the tree opens. Some nodes open two, and the second has the address after the node's for its key.
    Block* blockFor(const void* key) const { return m_blocksByNode.get(key); }

    // __x in a class C is _C__x.
    static const Identifier& mangle(VM&, Arena&, const Identifier* className, const Identifier& name);

private:
    friend class SymbolTableBuilder;
    SymbolTable() = default;

    Vector<std::unique_ptr<Block>> m_blocks;
    HashMap<const void*, Block*> m_blocksByNode;
    Block* m_top { nullptr };
    unsigned m_futureFeatures { 0 };
};

// For tests: as JSON, in the form of resources/dump-symtable.py.
String dumpSymbolTable(SymbolTable&);

} } // namespace JSC::Python
