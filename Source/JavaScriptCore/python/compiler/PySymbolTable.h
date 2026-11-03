/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Identifier.h"
#include "PyParser.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC {
namespace Python {

// Which scope every name in a program belongs to. (Python/symtable.c in CPython is the reference.)

enum class ScopeKind : uint8_t {
    Module,
    Class,
    Function, // Also a lambda or a comprehension.
    Annotation, // The function that computes the annotations of a module, a class or a function.
    TypeVariable, // The bound or the default of a type parameter.
    TypeAlias, // The value of a `type` statement.
    TypeParameters, // What encloses a generic function, class or type alias.
};

enum class ComprehensionKind : uint8_t { None, List, Set, Dict, Generator };

enum SymbolFlag : uint16_t {
    DefGlobal = 1 << 0, // global statement
    DefLocal = 1 << 1, // Assigned in the block.
    DefParam = 1 << 2,
    DefNonlocal = 1 << 3, // nonlocal statement
    Use = 1 << 4,
    DefFreeClass = 1 << 5, // Free in a class, and also a name of the class.
    DefImport = 1 << 6,
    DefAnnot = 1 << 7,
    DefCompIter = 1 << 8, // An iteration variable of a comprehension.
    DefTypeParam = 1 << 9,
    Deleted = 1 << 10, // `del`, or what follows `as` in an `except` clause: it may lose its value.
    DefBound = DefLocal | DefParam | DefImport,
};

enum class Binding : uint8_t {
    Unknown,
    Local,
    GlobalExplicit,
    GlobalImplicit, // Not bound in any enclosing function: a global or a builtin.
    Free, // Bound in an enclosing function.
    Cell, // Local, and free in an enclosed scope.
};

struct Symbol {
    AST::Name name;
    uint16_t flags { 0 };
    Binding binding { Binding::Unknown };
};

using NameSet = UncheckedKeyHashSet<UniquedStringImpl*>;

class Scope {
    WTF_MAKE_TZONE_ALLOCATED(Scope);
    WTF_MAKE_NONCOPYABLE(Scope);
public:
    Scope(ScopeKind kind, AST::Name name, const AST::Range& range)
        : kind(kind)
        , name(name)
        , range(range)
    {
    }

    bool isFunctionLike() const { return kind != ScopeKind::Module && kind != ScopeKind::Class; }

    Symbol* find(AST::Name symbol) { return find(symbol->impl()); }
    Symbol* find(UniquedStringImpl* symbol)
    {
        auto it = m_indices.find(symbol);
        return it == m_indices.end() ? nullptr : &symbols[it->value];
    }
    uint16_t flagsOf(AST::Name symbol)
    {
        auto* found = find(symbol);
        return found ? found->flags : 0;
    }
    Binding bindingOf(AST::Name symbol)
    {
        auto* found = find(symbol);
        return found ? found->binding : Binding::Unknown;
    }
    Symbol& add(AST::Name symbol)
    {
        auto result = m_indices.add(symbol->impl(), symbols.size());
        if (result.isNewEntry)
            symbols.append({ symbol });
        return symbols[result.iterator->value];
    }

    ScopeKind kind;
    ComprehensionKind comprehension { ComprehensionKind::None };
    AST::Name name;
    AST::Range range;

    Vector<Symbol> symbols; // In the order they were first seen.
    Vector<AST::Name> parameters;
    Vector<Scope*> children;
    Scope* annotationBlock { nullptr };
    AST::Name functionName { nullptr }; // Annotation: of the function whose annotations these are.
    ASCIILiteral description; // TypeVariable: "a TypeVar bound", for messages.

    struct Directive {
        AST::Name name;
        AST::Range range;
    };
    Vector<Directive> directives; // global and nonlocal statements, for where to report what is wrong with them.
    // TypeParameters of a class: the only names in it that are mangled. Null: all are.
    std::shared_ptr<NameSet> mangledNames;

    bool isNested : 1 { false }; // Inside a function.
    bool isGenerator : 1 { false };
    bool isCoroutine : 1 { false };
    bool hasVarargs : 1 { false };
    bool hasVarKeywords : 1 { false };
    bool returnsValue : 1 { false };
    bool needsClassClosure : 1 { false }; // Class: a method refers to __class__, as super() does.
    bool needsClassDict : 1 { false }; // Class: an enclosed scope looks names up in the namespace of the class.
    bool canSeeClassScope : 1 { false };
    bool isInlinedComprehension : 1 { false }; // Compiled as part of the enclosing scope, with variables of its own.
    bool hasDocstring : 1 { false };
    bool isMethod : 1 { false };
    bool usesAnnotations : 1 { false };
    bool hasConditionalAnnotations : 1 { false };
    bool hasNamedExpression : 1 { false }; // A variable may change while an expression is evaluated.

    // While the tree is visited.
    bool inComprehensionTarget : 1 { false };
    bool inConditionalBlock : 1 { false };
    bool inTryBlock : 1 { false };
    bool inUnevaluatedAnnotation : 1 { false };
    int comprehensionIterableDepth { 0 };

private:
    UncheckedKeyHashMap<UniquedStringImpl*, unsigned> m_indices;
};

struct FutureFeatures {
    bool annotations { false };
    bool barryAsBDFL { false };
    bool allowTopLevelAwait { false };
    AST::Range lastStatement { -1, -1, -1, -1 };
};

// False: `error` says why.
bool analyzeFutureStatements(AST::Mod*, FutureFeatures&, SyntaxErrorInfo& error);

class SymbolTable {
    WTF_MAKE_TZONE_ALLOCATED(SymbolTable);
    WTF_MAKE_NONCOPYABLE(SymbolTable);
public:
    SymbolTable(VM&, ParseTree&, const FutureFeatures&);

    // False: error() says why.
    bool build();
    const SyntaxErrorInfo& error() const { return m_error; }

    Scope* top() const { return m_top; }
    // By the node that opens the scope. Where one node opens two, the second has `key + 1`.
    Scope* scopeFor(const void* key) const { return m_scopes.get(key); }

    AST::Name mangle(AST::Name privateName, Scope*, AST::Name);

    struct Names {
        explicit Names(VM&, IdentifierArena&);
        AST::Name top, lambda, genexpr, listcomp, setcomp, dictcomp, annotate, classCell, classDict, conditionalAnnotations,
            typeParamsAttribute, dotTypeParams, dotGenericBase, dotDefaults, dotKwDefaults, dotFormat, dotZero, super, debug;
    };
    const Names& names() const { return m_names; }

private:
    using Expr = AST::Expr;
    using Stmt = AST::Stmt;
    using Name = AST::Name;
    using Range = AST::Range;

    bool fail(const Range&, const String& message);
    bool failAtDirective(Scope*, Name, const String& message);

    Scope* newScope(ScopeKind, Name, const void* key, const Range&);
    bool enterExisting(Scope*, bool addToChildren);
    bool enter(ScopeKind, Name, const void* key, const Range&);
    void exit();
    bool enterTypeParameters(Name, const void* key, bool hasDefaults, bool hasKwDefaults, bool isClass, const Range&);

    bool addDefinition(Name, unsigned flag, const Range&, AST::ExprContext);
    bool addDefinition(Name name, unsigned flag, const Range& range) { return addDefinition(name, flag, range, flag == Use ? AST::ExprContext::Load : AST::ExprContext::Store); }
    bool addDefinitionTo(Scope*, Name, unsigned flag, const Range&);
    bool checkName(Name, const Range&, AST::ExprContext);
    uint16_t lookup(Scope*, Name);
    void recordDirective(Name, const Range&);

    bool visit(Stmt*);
    bool visit(Expr*);
    bool visit(AST::Pattern*);
    bool visit(AST::TypeParam*);
    bool visit(AST::Comprehension*);
    bool visit(AST::Alias*);
    template<typename T> bool visitAll(Seq<T*> items)
    {
        for (T* item : items) {
            if (item && !visit(item))
                return false;
        }
        return true;
    }
    bool visitFunction(AST::FunctionDef*);
    bool visitClass(AST::ClassDef*);
    bool visitParameters(AST::Arguments*);
    bool visitAnnotation(Expr*, const void* key);
    bool visitFunctionAnnotations(Stmt*, AST::Arguments*, Expr* returns, Scope* function);
    bool visitTypeParameterExpression(Expr*, Name, const void* key, const Range& parameterRange, ASCIILiteral description);
    bool visitComprehension(Expr*, Name scopeName, ComprehensionKind, Seq<AST::Comprehension*>, Expr* element, Expr* value);
    bool visitNamedExpression(AST::NamedExpr*);
    bool extendNamedExpressionScope(AST::NameExpr*);
    bool checkKeywords(Seq<AST::Keyword*>);
    bool checkLazyImport(Stmt*, ASCIILiteral what);
    bool raiseIfAnnotationBlock(ASCIILiteral what, Expr*);
    bool raiseIfInComprehension(Expr*);
    bool allowsTopLevelAwait() const { return m_future.allowTopLevelAwait && m_current->kind == ScopeKind::Module; }
    bool isInAsyncFunction() const { return m_current->kind == ScopeKind::Function && m_current->isCoroutine; }

    bool analyzeBlock(Scope*, NameSet* bound, NameSet& free, NameSet& global, NameSet& typeParams, Scope* classEntry);
    bool analyzeChildBlock(Scope*, const NameSet& bound, const NameSet& free, const NameSet& global, const NameSet& typeParams, Scope* classEntry, NameSet& childFree);
    bool analyzeName(Scope*, Symbol&, NameSet* bound, NameSet& local, NameSet& free, NameSet& global, NameSet& typeParams, Scope* classEntry);
    void inlineComprehension(Scope* parent, Scope* comprehension, NameSet& comprehensionFree);

    VM& m_vm;
    ParseTree& m_tree;
    FutureFeatures m_future;
    Names m_names;
    SyntaxErrorInfo m_error;

    Vector<std::unique_ptr<Scope>> m_allScopes;
    UncheckedKeyHashMap<const void*, Scope*> m_scopes;
    Vector<Scope*, 16> m_stack;
    Scope* m_top { nullptr };
    Scope* m_current { nullptr };
    Name m_private { nullptr }; // The name of the class being visited, which private names are mangled with.
};

} // namespace Python
} // namespace JSC
