/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PySymbolTable.h"

#include "VM.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

WTF_MAKE_TZONE_ALLOCATED_IMPL(Scope);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SymbolTable);

static void setError(SyntaxErrorInfo& error, const Range& range, const String& message)
{
    error = { };
    error.type = SyntaxErrorInfo::Type::SyntaxError;
    error.message = message;
    error.hasLocation = true;
    error.locationIsNotAnArgument = true;
    error.lineno = range.lineno;
    error.offset = range.colOffset + 1;
    error.hasEnd = true;
    error.endLineno = range.endLineno;
    error.endOffset = range.endColOffset + 1;
}

static bool isDocstring(Stmt* statement)
{
    if (statement->kind != StmtKind::Expr)
        return false;
    auto* constant = statement->as<ExprStmt>()->value->tryAs<Constant>();
    return constant && constant->value.isString();
}

static bool hasDocstring(Seq<Stmt*> body)
{
    return !body.isEmpty() && isDocstring(body.first());
}

bool analyzeFutureStatements(Mod* module, FutureFeatures& features, SyntaxErrorInfo& error)
{
    if (module->kind != ModKind::Module && module->kind != ModKind::Interactive)
        return true;
    // Only a docstring and other future statements may come before a future statement.
    unsigned i = hasDocstring(module->body) ? 1 : 0;
    for (; i < module->body.size(); ++i) {
        Stmt* statement = module->body[i];
        if (statement->kind != StmtKind::ImportFrom)
            return true;
        auto* import = statement->as<ImportFrom>();
        if (import->level || !import->module || import->module->string() != "__future__"_s)
            return true;
        for (Alias* alias : import->names) {
            auto& feature = alias->name->string();
            if (feature == "nested_scopes"_s || feature == "generators"_s || feature == "division"_s || feature == "absolute_import"_s
                || feature == "with_statement"_s || feature == "print_function"_s || feature == "unicode_literals"_s || feature == "generator_stop"_s)
                continue;
            if (feature == "barry_as_FLUFL"_s)
                features.barryAsBDFL = true;
            else if (feature == "annotations"_s)
                features.annotations = true;
            else {
                setError(error, alias->range, feature == "braces"_s ? String("not a chance"_s) : makeString("future feature "_s, feature, " is not defined"_s));
                return false;
            }
        }
        features.lastStatement = statement->range;
    }
    return true;
}

SymbolTable::Names::Names(VM& vm, IdentifierArena& arena)
{
    auto make = [&](ASCIILiteral text) { return &arena.makeIdentifier(vm, text.span8()); };
    top = make("top"_s);
    lambda = make("<lambda>"_s);
    genexpr = make("<genexpr>"_s);
    listcomp = make("<listcomp>"_s);
    setcomp = make("<setcomp>"_s);
    dictcomp = make("<dictcomp>"_s);
    annotate = make("__annotate__"_s);
    classCell = make("__class__"_s);
    classDict = make("__classdict__"_s);
    conditionalAnnotations = make("__conditional_annotations__"_s);
    typeParamsAttribute = make("__type_params__"_s);
    dotTypeParams = make(".type_params"_s);
    dotGenericBase = make(".generic_base"_s);
    dotDefaults = make(".defaults"_s);
    dotKwDefaults = make(".kwdefaults"_s);
    dotFormat = make(".format"_s);
    dotZero = make(".0"_s);
    super = make("super"_s);
    debug = make("__debug__"_s);
}

SymbolTable::SymbolTable(VM& vm, ParseTree& tree, const FutureFeatures& future)
    : m_vm(vm)
    , m_tree(tree)
    , m_future(future)
    , m_names(vm, tree.identifiers)
{
}

bool SymbolTable::fail(const Range& range, const String& message)
{
    setError(m_error, range, message);
    return false;
}

bool SymbolTable::failAtDirective(Scope* scope, Name name, const String& message)
{
    for (auto& directive : scope->directives) {
        if (directive.name->impl() == name->impl())
            return fail(directive.range, message);
    }
    return fail(scope->range, message);
}

// __private in class Spam is _Spam__private.
Name SymbolTable::mangle(Name privateName, Scope* scope, Name name)
{
    if (!privateName)
        return name;
    if (scope->mangledNames && !scope->mangledNames->contains(name->impl()))
        return name;
    StringView text = name->string();
    if (text.length() < 2 || text[0] != '_' || text[1] != '_')
        return name;
    if ((text[text.length() - 1] == '_' && text[text.length() - 2] == '_') || text.contains('.'))
        return name;
    StringView className = privateName->string();
    unsigned underscores = 0;
    while (underscores < className.length() && className[underscores] == '_')
        ++underscores;
    if (underscores == className.length())
        return name;
    String mangled = makeString('_', className.substring(underscores), text);
    if (mangled.is8Bit())
        return &m_tree.identifiers.makeIdentifier(m_vm, mangled.span8());
    return &m_tree.identifiers.makeIdentifier(m_vm, mangled.span16());
}

// ---- Scopes ----

Scope* SymbolTable::newScope(ScopeKind kind, Name name, const void* key, const Range& range)
{
    m_allScopes.append(makeUnique<Scope>(kind, name, range));
    Scope* scope = m_allScopes.last().get();
    if (m_current) {
        scope->isNested = m_current->isNested || m_current->isFunctionLike();
        scope->isMethod = m_current->kind == ScopeKind::Class && kind == ScopeKind::Function;
    }
    m_scopes.set(key, scope);
    return scope;
}

bool SymbolTable::enterExisting(Scope* scope, bool addToChildren)
{
    m_stack.append(scope);
    Scope* previous = m_current;
    if (previous) {
        // No assignment expression anywhere in the iterable of a comprehension, however deep.
        scope->comprehensionIterableDepth = previous->comprehensionIterableDepth;
        if (previous->mangledNames && scope->kind != ScopeKind::Class)
            scope->mangledNames = previous->mangledNames;
    }
    m_current = scope;

    // Annotations that become strings bind nothing.
    if (m_future.annotations && scope->kind == ScopeKind::Annotation)
        return true;
    if (addToChildren && previous)
        previous->children.append(scope);
    return true;
}

bool SymbolTable::enter(ScopeKind kind, Name name, const void* key, const Range& range)
{
    enterExisting(newScope(kind, name, key, range), true);
    if (kind == ScopeKind::Annotation || kind == ScopeKind::TypeVariable || kind == ScopeKind::TypeAlias) {
        // The function takes the format to evaluate in.
        if (!addDefinition(m_names.dotFormat, DefParam, range) || !addDefinition(m_names.dotFormat, Use, range)) {
            exit();
            return false;
        }
    }
    return true;
}

void SymbolTable::exit()
{
    m_stack.removeLast();
    m_current = m_stack.isEmpty() ? nullptr : m_stack.last();
}

bool SymbolTable::enterTypeParameters(Name name, const void* key, bool hasDefaults, bool hasKwDefaults, bool isClass, const Range& range)
{
    bool inClass = m_current->kind == ScopeKind::Class;
    if (!enter(ScopeKind::TypeParameters, name, key, range))
        return false;
    if (inClass) {
        m_current->canSeeClassScope = true;
        if (!addDefinition(m_names.classDict, Use, range))
            return false;
    }
    if (isClass) {
        // Set when the tuple of type parameters is made, used when the bases are.
        if (!addDefinition(m_names.dotTypeParams, DefLocal, range) || !addDefinition(m_names.dotTypeParams, Use, range))
            return false;
        if (!addDefinition(m_names.dotGenericBase, DefLocal, range) || !addDefinition(m_names.dotGenericBase, Use, range))
            return false;
    }
    if (hasDefaults && !addDefinition(m_names.dotDefaults, DefParam, range))
        return false;
    if (hasKwDefaults && !addDefinition(m_names.dotKwDefaults, DefParam, range))
        return false;
    return true;
}

// ---- Definitions ----

bool SymbolTable::checkName(Name name, const Range& range, ExprContext context)
{
    if (name->impl() != m_names.debug->impl())
        return true;
    if (context == ExprContext::Store)
        return fail(range, "cannot assign to __debug__"_s);
    if (context == ExprContext::Del)
        return fail(range, "cannot delete __debug__"_s);
    return true;
}

bool SymbolTable::addDefinitionTo(Scope* scope, Name name, unsigned flag, const Range& range)
{
    Name mangled = mangle(m_private, m_current, name);
    Symbol& symbol = scope->add(mangled);
    if ((flag & DefParam) && (symbol.flags & DefParam))
        return fail(range, makeString("duplicate parameter '"_s, name->string(), "' in function definition"_s));
    if ((flag & DefTypeParam) && (symbol.flags & DefTypeParam))
        return fail(range, makeString("duplicate type parameter '"_s, name->string(), '\''));
    symbol.flags |= flag;
    if (scope->inComprehensionTarget) {
        // An iteration variable must not be what an assignment expression in the comprehension binds.
        if (symbol.flags & (DefGlobal | DefNonlocal))
            return fail(range, makeString("comprehension inner loop cannot rebind assignment expression target '"_s, name->string(), '\''));
        symbol.flags |= DefCompIter;
    }
    if (flag & DefParam)
        scope->parameters.append(mangled);
    else if (flag & DefGlobal)
        m_top->add(mangled).flags |= flag;
    return true;
}

bool SymbolTable::addDefinition(Name name, unsigned flag, const Range& range, ExprContext context)
{
    if ((flag & (DefParam | DefLocal | DefImport)) && !checkName(name, range, context))
        return false;
    if ((flag & DefTypeParam) && m_current->mangledNames)
        m_current->mangledNames->add(name->impl());
    return addDefinitionTo(m_current, name, flag, range);
}

uint16_t SymbolTable::lookup(Scope* scope, Name name)
{
    return scope->flagsOf(mangle(m_private, scope, name));
}

void SymbolTable::recordDirective(Name name, const Range& range)
{
    m_current->directives.append({ mangle(m_private, m_current, name), range });
}

// ---- First pass: what each scope does with each name ----

bool SymbolTable::build()
{
    Mod* module = m_tree.root;
    enter(ScopeKind::Module, m_names.top, module, { });
    m_top = m_current;
    switch (module->kind) {
    case ModKind::Module:
        m_current->hasDocstring = hasDocstring(module->body);
        [[fallthrough]];
    case ModKind::Interactive:
        if (!visitAll(module->body))
            return false;
        break;
    case ModKind::Expression:
        if (!visit(module->expression))
            return false;
        break;
    case ModKind::FunctionType:
        m_error.type = SyntaxErrorInfo::Type::SystemError;
        m_error.message = "this compiler does not handle FunctionTypes"_s;
        return false;
    }
    exit();

    NameSet free;
    NameSet global;
    NameSet typeParams;
    return analyzeBlock(m_top, nullptr, free, global, typeParams, nullptr);
}

static bool hasKeywordOnlyDefaults(Arguments* arguments)
{
    for (Expr* value : arguments->kwDefaults) {
        if (value)
            return true;
    }
    return false;
}

bool SymbolTable::visitParameters(Arguments* arguments)
{
    auto visitParams = [&](Seq<Arg*> params) {
        for (Arg* param : params) {
            if (!addDefinition(param->arg, DefParam, param->range))
                return false;
        }
        return true;
    };
    if (!visitParams(arguments->posonlyargs) || !visitParams(arguments->args) || !visitParams(arguments->kwonlyargs))
        return false;
    if (arguments->vararg) {
        if (!addDefinition(arguments->vararg->arg, DefParam, arguments->vararg->range))
            return false;
        m_current->hasVarargs = true;
    }
    if (arguments->kwarg) {
        if (!addDefinition(arguments->kwarg->arg, DefParam, arguments->kwarg->range))
            return false;
        m_current->hasVarKeywords = true;
    }
    return true;
}

// The annotation of a variable. Those of one scope share one function.
bool SymbolTable::visitAnnotation(Expr* annotation, const void* key)
{
    // In a function they are not evaluated at all.
    bool isUnevaluated = m_current->kind == ScopeKind::Function;

    // A module may be executed only in part, so which of its annotations apply is only known at run time.
    if (((m_current->kind == ScopeKind::Class && m_current->inConditionalBlock) || m_current->kind == ScopeKind::Module) && !m_current->hasConditionalAnnotations) {
        m_current->hasConditionalAnnotations = true;
        if (!addDefinition(m_names.conditionalAnnotations, Use, annotation->range))
            return false;
    }

    Scope* parent = m_current;
    if (!parent->annotationBlock) {
        if (!enter(ScopeKind::Annotation, m_names.annotate, key, annotation->range))
            return false;
        parent->annotationBlock = m_current;
        if (parent->kind == ScopeKind::Class && !m_future.annotations) {
            m_current->canSeeClassScope = true;
            parent->needsClassDict = true;
            if (!addDefinition(m_names.classDict, Use, annotation->range))
                return false;
        }
    } else
        enterExisting(parent->annotationBlock, false);
    if (parent->kind == ScopeKind::Class && parent->hasConditionalAnnotations)
        m_current->readsConditionalAnnotationsCell = true;

    m_current->inUnevaluatedAnnotation = isUnevaluated;
    bool result = visit(annotation);
    m_current->inUnevaluatedAnnotation = false;
    exit();
    return result;
}

bool SymbolTable::visitFunctionAnnotations(Stmt* statement, Arguments* arguments, Expr* returns, Scope* function)
{
    bool inClass = m_current->canSeeClassScope || m_current->kind == ScopeKind::Class;
    if (!enter(ScopeKind::Annotation, m_names.annotate, arguments, statement->range))
        return false;
    m_current->functionName = function->name;
    if (inClass) {
        m_current->canSeeClassScope = true;
        if (!addDefinition(m_names.classDict, Use, statement->range))
            return false;
    }
    auto visitOne = [&](Expr* annotation) {
        if (!annotation)
            return true;
        m_current->usesAnnotations = true;
        return visit(annotation);
    };
    auto visitParams = [&](Seq<Arg*> params) {
        for (Arg* param : params) {
            if (!visitOne(param->annotation))
                return false;
        }
        return true;
    };
    if (!visitParams(arguments->posonlyargs) || !visitParams(arguments->args))
        return false;
    if (arguments->vararg && !visitOne(arguments->vararg->annotation))
        return false;
    if (arguments->kwarg && !visitOne(arguments->kwarg->annotation))
        return false;
    if (!visitParams(arguments->kwonlyargs) || !visitOne(returns))
        return false;
    exit();
    return true;
}

bool SymbolTable::visitFunction(FunctionDef* function)
{
    if (!addDefinition(function->name, DefLocal, function->range))
        return false;
    if (!visitAll(function->args->defaults) || !visitAll(function->args->kwDefaults) || !visitAll(function->decoratorList))
        return false;
    bool isGeneric = !function->typeParams.isEmpty();
    if (isGeneric) {
        if (!enterTypeParameters(function->name, function->typeParams.begin(), !function->args->defaults.isEmpty(), hasKeywordOnlyDefaults(function->args), false, function->range))
            return false;
        if (!visitAll(function->typeParams))
            return false;
    }
    Scope* scope = newScope(ScopeKind::Function, function->name, function, function->range);
    scope->hasDocstring = hasDocstring(function->body);
    if (!visitFunctionAnnotations(function, function->args, function->returns, scope))
        return false;
    enterExisting(scope, true);
    if (function->isAsync())
        m_current->isCoroutine = true;
    if (!visitParameters(function->args) || !visitAll(function->body))
        return false;
    exit();
    if (isGeneric)
        exit();
    return true;
}

bool SymbolTable::checkKeywords(Seq<Keyword*> keywords)
{
    for (Keyword* keyword : keywords) {
        if (keyword->arg && !checkName(keyword->arg, keyword->range, ExprContext::Store))
            return false;
    }
    return true;
}

bool SymbolTable::visitClass(ClassDef* classDef)
{
    if (!addDefinition(classDef->name, DefLocal, classDef->range))
        return false;
    if (!visitAll(classDef->decoratorList))
        return false;
    Name savedPrivate = m_private;
    bool isGeneric = !classDef->typeParams.isEmpty();
    if (isGeneric) {
        if (!enterTypeParameters(classDef->name, classDef->typeParams.begin(), false, false, true, classDef->range))
            return false;
        m_private = classDef->name;
        m_current->mangledNames = std::make_shared<NameSet>();
        if (!visitAll(classDef->typeParams))
            return false;
    }
    if (!visitAll(classDef->bases) || !checkKeywords(classDef->keywords))
        return false;
    for (Keyword* keyword : classDef->keywords) {
        if (!visit(keyword->value))
            return false;
    }
    if (!enter(ScopeKind::Class, classDef->name, classDef, classDef->range))
        return false;
    m_private = classDef->name;
    if (isGeneric) {
        if (!addDefinition(m_names.typeParamsAttribute, DefLocal, classDef->range) || !addDefinition(m_names.dotTypeParams, Use, classDef->range))
            return false;
    }
    m_current->hasDocstring = hasDocstring(classDef->body);
    if (!visitAll(classDef->body))
        return false;
    exit();
    if (isGeneric)
        exit();
    m_private = savedPrivate;
    return true;
}

bool SymbolTable::checkLazyImport(Stmt* statement, ASCIILiteral what)
{
    if (m_current->inTryBlock)
        return fail(statement->range, makeString("lazy "_s, what, " not allowed inside try/except blocks"_s));
    if (m_current->kind == ScopeKind::Function)
        return fail(statement->range, makeString("lazy "_s, what, " not allowed inside functions"_s));
    if (m_current->kind == ScopeKind::Class)
        return fail(statement->range, makeString("lazy "_s, what, " not allowed inside classes"_s));
    return true;
}

bool SymbolTable::visit(Alias* alias)
{
    // `import a.b` binds a.
    Name name = alias->asname ? alias->asname : alias->name;
    if (name->string() == "*"_s) {
        if (m_current->kind != ScopeKind::Module)
            return fail(alias->range, "import * only allowed at module level"_s);
        return true;
    }
    size_t dot = name->string().find('.');
    if (dot != notFound) {
        String first = name->string().string().left(dot);
        name = first.is8Bit() ? &m_tree.identifiers.makeIdentifier(m_vm, first.span8()) : &m_tree.identifiers.makeIdentifier(m_vm, first.span16());
    }
    return addDefinition(name, DefImport, alias->range);
}

bool SymbolTable::visit(Stmt* statement)
{
    if (!m_vm.isSafeToRecurse()) [[unlikely]] {
        m_error.type = SyntaxErrorInfo::Type::RecursionError;
        m_error.message = "maximum recursion depth exceeded during compilation"_s;
        return false;
    }

    // Whether the statements in it run depends on what happens at run time.
    auto visitConditionally = [&](auto&& visitBody) {
        Scope* scope = m_current;
        bool saved = scope->inConditionalBlock;
        scope->inConditionalBlock = true;
        bool result = visitBody();
        scope->inConditionalBlock = saved;
        return result;
    };

    switch (statement->kind) {
    case StmtKind::FunctionDef:
    case StmtKind::AsyncFunctionDef:
        return visitFunction(statement->as<FunctionDef>());
    case StmtKind::ClassDef:
        return visitClass(statement->as<ClassDef>());
    case StmtKind::TypeAlias: {
        auto* alias = statement->as<TypeAlias>();
        if (!visit(alias->name))
            return false;
        Name name = alias->name->as<NameExpr>()->id;
        bool inClass = m_current->kind == ScopeKind::Class;
        bool isGeneric = !alias->typeParams.isEmpty();
        if (isGeneric) {
            if (!enterTypeParameters(name, alias->typeParams.begin(), false, false, false, alias->range) || !visitAll(alias->typeParams))
                return false;
        }
        if (!enter(ScopeKind::TypeAlias, name, alias, alias->range))
            return false;
        m_current->canSeeClassScope = inClass;
        if (inClass && !addDefinition(m_names.classDict, Use, alias->value->range))
            return false;
        if (!visit(alias->value))
            return false;
        exit();
        if (isGeneric)
            exit();
        return true;
    }
    case StmtKind::Return:
        if (Expr* value = statement->as<Return>()->value) {
            if (!visit(value))
                return false;
            m_current->returnsValue = true;
        }
        return true;
    case StmtKind::Delete:
        return visitAll(statement->as<Delete>()->targets);
    case StmtKind::Assign:
        return visitAll(statement->as<Assign>()->targets) && visit(statement->as<Assign>()->value);
    case StmtKind::AnnAssign: {
        auto* assign = statement->as<AnnAssign>();
        m_current->usesAnnotations = true;
        if (auto* target = assign->target->tryAs<NameExpr>()) {
            uint16_t flags = lookup(m_current, target->id);
            if ((flags & (DefGlobal | DefNonlocal)) && m_current != m_top && assign->simple)
                return fail(assign->range, makeString("annotated name '"_s, target->id->string(), (flags & DefGlobal) ? "' can't be global"_s : "' can't be nonlocal"_s));
            if (assign->simple) {
                if (!addDefinition(target->id, DefAnnot | DefLocal, target->range))
                    return false;
            } else if (assign->value && !addDefinition(target->id, DefLocal, target->range))
                return false;
        } else if (!visit(assign->target))
            return false;
        if (!visitAnnotation(assign->annotation, reinterpret_cast<const char*>(m_current) + 1))
            return false;
        return !assign->value || visit(assign->value);
    }
    case StmtKind::AugAssign:
        return visit(statement->as<AugAssign>()->target) && visit(statement->as<AugAssign>()->value);
    case StmtKind::For:
    case StmtKind::AsyncFor: {
        auto* loop = statement->as<For>();
        if (loop->isAsync()) {
            if (allowsTopLevelAwait())
                m_current->isCoroutine = true;
            if (!m_current->isCoroutine)
                return fail(loop->range, "'async for' outside async function"_s);
        }
        if (!visit(loop->target) || !visit(loop->iter))
            return false;
        return visitConditionally([&] { return visitAll(loop->body) && visitAll(loop->orelse); });
    }
    case StmtKind::While: {
        auto* loop = statement->as<While>();
        if (!visit(loop->test))
            return false;
        return visitConditionally([&] { return visitAll(loop->body) && visitAll(loop->orelse); });
    }
    case StmtKind::If: {
        auto* conditional = statement->as<If>();
        if (!visit(conditional->test))
            return false;
        return visitConditionally([&] { return visitAll(conditional->body) && visitAll(conditional->orelse); });
    }
    case StmtKind::Match: {
        auto* match = statement->as<Match>();
        if (!visit(match->subject))
            return false;
        return visitConditionally([&] {
            for (MatchCase* matchCase : match->cases) {
                if (!visit(matchCase->pattern) || (matchCase->guard && !visit(matchCase->guard)) || !visitAll(matchCase->body))
                    return false;
            }
            return true;
        });
    }
    case StmtKind::Raise: {
        auto* raise = statement->as<Raise>();
        return (!raise->exc || visit(raise->exc)) && (!raise->cause || visit(raise->cause));
    }
    case StmtKind::Try:
    case StmtKind::TryStar: {
        auto* tryStatement = statement->as<Try>();
        return visitConditionally([&] {
            Scope* scope = m_current;
            bool saved = scope->inTryBlock;
            scope->inTryBlock = true;
            bool result = [&] {
                if (!visitAll(tryStatement->body))
                    return false;
                for (ExceptHandler* handler : tryStatement->handlers) {
                    if (handler->type && !visit(handler->type))
                        return false;
                    if (handler->name && !addDefinition(handler->name, DefLocal | Deleted, handler->range))
                        return false;
                    if (!visitAll(handler->body))
                        return false;
                }
                return visitAll(tryStatement->orelse) && visitAll(tryStatement->finalbody);
            }();
            scope->inTryBlock = saved;
            return result;
        });
    }
    case StmtKind::Assert: {
        auto* assertion = statement->as<Assert>();
        return visit(assertion->test) && (!assertion->msg || visit(assertion->msg));
    }
    case StmtKind::Import: {
        auto* import = statement->as<Import>();
        if (import->isLazy && !checkLazyImport(import, "import"_s))
            return false;
        return visitAll(import->names);
    }
    case StmtKind::ImportFrom: {
        auto* import = statement->as<ImportFrom>();
        if (import->isLazy) {
            if (!checkLazyImport(import, "from ... import"_s))
                return false;
            for (Alias* alias : import->names) {
                if (alias->name->string() == "*"_s)
                    return fail(import->range, "lazy from ... import * is not allowed"_s);
            }
        }
        if (!visitAll(import->names))
            return false;
        if (import->module && !import->level && import->module->string() == "__future__"_s) {
            auto& last = m_future.lastStatement;
            if (import->range.lineno > last.lineno || (import->range.lineno == last.endLineno && import->range.colOffset > last.endColOffset))
                return fail(import->range, "from __future__ imports must occur at the beginning of the file"_s);
        }
        return true;
    }
    case StmtKind::Global:
    case StmtKind::Nonlocal: {
        bool isGlobal = statement->kind == StmtKind::Global;
        auto word = isGlobal ? "global"_s : "nonlocal"_s;
        for (Name name : isGlobal ? statement->as<Global>()->names : statement->as<Nonlocal>()->names) {
            uint16_t flags = lookup(m_current, name);
            if (flags & DefParam)
                return fail(statement->range, makeString("name '"_s, name->string(), "' is parameter and "_s, word));
            if (flags & Use)
                return fail(statement->range, makeString("name '"_s, name->string(), "' is used prior to "_s, word, " declaration"_s));
            if (flags & DefAnnot)
                return fail(statement->range, makeString("annotated name '"_s, name->string(), "' can't be "_s, word));
            if (flags & DefLocal)
                return fail(statement->range, makeString("name '"_s, name->string(), "' is assigned to before "_s, word, " declaration"_s));
            if (!addDefinition(name, isGlobal ? DefGlobal : DefNonlocal, statement->range))
                return false;
            recordDirective(name, statement->range);
        }
        return true;
    }
    case StmtKind::Expr:
        return visit(statement->as<ExprStmt>()->value);
    case StmtKind::Pass:
    case StmtKind::Break:
    case StmtKind::Continue:
        return true;
    case StmtKind::With:
    case StmtKind::AsyncWith: {
        auto* with = statement->as<With>();
        if (with->isAsync()) {
            if (allowsTopLevelAwait())
                m_current->isCoroutine = true;
            if (!m_current->isCoroutine)
                return fail(with->range, "'async with' outside async function"_s);
        }
        return visitConditionally([&] {
            for (WithItem* item : with->items) {
                if (!visit(item->contextExpr) || (item->optionalVars && !visit(item->optionalVars)))
                    return false;
            }
            return visitAll(with->body);
        });
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `(y := f(x))` in a comprehension binds y in the scope that contains the comprehension.
bool SymbolTable::extendNamedExpressionScope(NameExpr* target)
{
    Name name = target->id;
    for (size_t i = m_stack.size(); i--;) {
        Scope* scope = m_stack[i];
        if (scope->comprehension != ComprehensionKind::None) {
            uint16_t flags = lookup(scope, name);
            if ((flags & DefCompIter) && (flags & DefLocal))
                return fail(target->range, makeString("assignment expression cannot rebind comprehension iteration variable '"_s, name->string(), '\''));
            continue;
        }
        switch (scope->kind) {
        case ScopeKind::Function:
            if (!addDefinition(name, (lookup(scope, name) & DefGlobal) ? DefGlobal : DefNonlocal, target->range))
                return false;
            recordDirective(name, target->range);
            return addDefinitionTo(scope, name, DefLocal, target->range);
        case ScopeKind::Module:
            if (!addDefinition(name, DefGlobal, target->range))
                return false;
            recordDirective(name, target->range);
            return addDefinitionTo(scope, name, DefGlobal, target->range);
        case ScopeKind::Class:
            return fail(target->range, "assignment expression within a comprehension cannot be used in a class body"_s);
        case ScopeKind::TypeParameters:
            return fail(target->range, "assignment expression within a comprehension cannot be used within the definition of a generic"_s);
        case ScopeKind::TypeAlias:
            return fail(target->range, "assignment expression within a comprehension cannot be used in a type alias"_s);
        case ScopeKind::TypeVariable:
            return fail(target->range, "assignment expression within a comprehension cannot be used in a TypeVar bound"_s);
        case ScopeKind::Annotation:
            break;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool SymbolTable::visitNamedExpression(NamedExpr* expression)
{
    if (m_current->comprehensionIterableDepth > 0)
        return fail(expression->range, "assignment expression cannot be used in a comprehension iterable expression"_s);
    if (m_current->comprehension != ComprehensionKind::None && !extendNamedExpressionScope(expression->target->as<NameExpr>()))
        return false;
    for (size_t i = m_stack.size(); i--;) {
        m_stack[i]->hasNamedExpression = true;
        if (m_stack[i]->comprehension == ComprehensionKind::None)
            break;
    }
    return visit(expression->value) && visit(expression->target);
}

bool SymbolTable::raiseIfAnnotationBlock(ASCIILiteral what, Expr* expression)
{
    switch (m_current->kind) {
    case ScopeKind::Annotation:
        return fail(expression->range, makeString(what, " cannot be used within an annotation"_s));
    case ScopeKind::TypeVariable:
        return fail(expression->range, makeString(what, " cannot be used within "_s, m_current->description));
    case ScopeKind::TypeAlias:
        return fail(expression->range, makeString(what, " cannot be used within a type alias"_s));
    case ScopeKind::TypeParameters:
        return fail(expression->range, makeString(what, " cannot be used within the definition of a generic"_s));
    default:
        return true;
    }
}

bool SymbolTable::raiseIfInComprehension(Expr* expression)
{
    switch (m_current->comprehension) {
    case ComprehensionKind::None:
        return true;
    case ComprehensionKind::List:
        return fail(expression->range, "'yield' inside list comprehension"_s);
    case ComprehensionKind::Set:
        return fail(expression->range, "'yield' inside set comprehension"_s);
    case ComprehensionKind::Dict:
        return fail(expression->range, "'yield' inside dict comprehension"_s);
    case ComprehensionKind::Generator:
        return fail(expression->range, "'yield' inside generator expression"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool SymbolTable::visit(Comprehension* clause)
{
    m_current->inComprehensionTarget = true;
    bool result = visit(clause->target);
    m_current->inComprehensionTarget = false;
    if (!result)
        return false;
    ++m_current->comprehensionIterableDepth;
    result = visit(clause->iter);
    --m_current->comprehensionIterableDepth;
    if (!result || !visitAll(clause->ifs))
        return false;
    if (clause->isAsync)
        m_current->isCoroutine = true;
    return true;
}

bool SymbolTable::visitComprehension(Expr* expression, Name scopeName, ComprehensionKind kind, Seq<Comprehension*> generators, Expr* element, Expr* value)
{
    bool isGenerator = kind == ComprehensionKind::Generator;
    Comprehension* outermost = generators.first();

    // The first iterable is evaluated in the enclosing scope.
    ++m_current->comprehensionIterableDepth;
    bool result = visit(outermost->iter);
    --m_current->comprehensionIterableDepth;
    if (!result)
        return false;

    if (!enter(ScopeKind::Function, scopeName, expression, expression->range))
        return false;
    m_current->comprehension = kind;
    if (outermost->isAsync)
        m_current->isCoroutine = true;
    // And its iterator is passed in.
    if (!addDefinition(m_names.dotZero, DefParam, m_current->range))
        return false;
    m_current->inComprehensionTarget = true;
    result = visit(outermost->target);
    m_current->inComprehensionTarget = false;
    if (!result || !visitAll(outermost->ifs))
        return false;
    for (unsigned i = 1; i < generators.size(); ++i) {
        if (!visit(generators[i]))
            return false;
    }
    if (value && !visit(value))
        return false;
    if (!visit(element))
        return false;
    m_current->isGenerator = isGenerator;
    bool isAsync = m_current->isCoroutine && !isGenerator;
    exit();
    if (isAsync) {
        if (!isInAsyncFunction() && m_current->comprehension == ComprehensionKind::None && !allowsTopLevelAwait())
            return fail(expression->range, "asynchronous comprehension outside of an asynchronous function"_s);
        m_current->isCoroutine = true;
    }
    return true;
}

bool SymbolTable::visit(Expr* expression)
{
    if (!m_vm.isSafeToRecurse()) [[unlikely]] {
        m_error.type = SyntaxErrorInfo::Type::RecursionError;
        m_error.message = "maximum recursion depth exceeded during compilation"_s;
        return false;
    }

    // a + b + c, a.b.c, a[b][c] and a()() nest to the left, as deep as they are long. So that is a loop: down to what is
    // evaluated first, and back up.
    Vector<Expr*, 8> chain;
    while (true) {
        Expr* first = nullptr;
        switch (expression->kind) {
        case ExprKind::BinOp:
            first = expression->as<BinOp>()->left;
            break;
        case ExprKind::Attribute: {
            auto* attribute = expression->as<Attribute>();
            if (!checkName(attribute->attr, attribute->range, attribute->ctx))
                return false;
            first = attribute->value;
            break;
        }
        case ExprKind::Subscript:
            first = expression->as<Subscript>()->value;
            break;
        case ExprKind::Call:
            first = expression->as<Call>()->func;
            break;
        default:
            break;
        }
        if (!first)
            break;
        chain.append(expression);
        expression = first;
    }
    if (!chain.isEmpty()) {
        if (!visit(expression))
            return false;
        while (!chain.isEmpty()) {
            Expr* node = chain.takeLast();
            switch (node->kind) {
            case ExprKind::BinOp:
                if (!visit(node->as<BinOp>()->right))
                    return false;
                break;
            case ExprKind::Subscript:
                if (!visit(node->as<Subscript>()->slice))
                    return false;
                break;
            case ExprKind::Call: {
                auto* call = node->as<Call>();
                if (!visitAll(call->args) || !checkKeywords(call->keywords))
                    return false;
                for (Keyword* keyword : call->keywords) {
                    if (!visit(keyword->value))
                        return false;
                }
                break;
            }
            default:
                break;
            }
        }
        return true;
    }

    switch (expression->kind) {
    case ExprKind::BinOp:
    case ExprKind::Attribute:
    case ExprKind::Subscript:
    case ExprKind::Call:
        RELEASE_ASSERT_NOT_REACHED();
    case ExprKind::NamedExpr:
        return raiseIfAnnotationBlock("named expression"_s, expression) && visitNamedExpression(expression->as<NamedExpr>());
    case ExprKind::BoolOp:
        return visitAll(expression->as<BoolOp>()->values);
    case ExprKind::UnaryOp:
        return visit(expression->as<UnaryOp>()->operand);
    case ExprKind::Lambda: {
        auto* lambda = expression->as<Lambda>();
        if (!visitAll(lambda->args->defaults) || !visitAll(lambda->args->kwDefaults))
            return false;
        if (!enter(ScopeKind::Function, m_names.lambda, lambda, lambda->range))
            return false;
        if (!visitParameters(lambda->args) || !visit(lambda->body))
            return false;
        exit();
        return true;
    }
    case ExprKind::IfExp: {
        auto* conditional = expression->as<IfExp>();
        return visit(conditional->test) && visit(conditional->body) && visit(conditional->orelse);
    }
    case ExprKind::Dict:
        return visitAll(expression->as<Dict>()->keys) && visitAll(expression->as<Dict>()->values);
    case ExprKind::Set:
        return visitAll(expression->as<Set>()->elts);
    case ExprKind::GeneratorExp: {
        auto* node = expression->as<GeneratorExp>();
        return visitComprehension(node, m_names.genexpr, ComprehensionKind::Generator, node->generators, node->elt, nullptr);
    }
    case ExprKind::ListComp: {
        auto* node = expression->as<ListComp>();
        return visitComprehension(node, m_names.listcomp, ComprehensionKind::List, node->generators, node->elt, nullptr);
    }
    case ExprKind::SetComp: {
        auto* node = expression->as<SetComp>();
        return visitComprehension(node, m_names.setcomp, ComprehensionKind::Set, node->generators, node->elt, nullptr);
    }
    case ExprKind::DictComp: {
        auto* node = expression->as<DictComp>();
        return visitComprehension(node, m_names.dictcomp, ComprehensionKind::Dict, node->generators, node->key, node->value);
    }
    case ExprKind::Yield:
    case ExprKind::YieldFrom: {
        if (!raiseIfAnnotationBlock("yield expression"_s, expression))
            return false;
        Expr* value = expression->is<Yield>() ? expression->as<Yield>()->value : expression->as<YieldFrom>()->value;
        if (value && !visit(value))
            return false;
        m_current->isGenerator = true;
        return raiseIfInComprehension(expression);
    }
    case ExprKind::Await:
        if (!raiseIfAnnotationBlock("await expression"_s, expression))
            return false;
        if (!allowsTopLevelAwait()) {
            if (!m_current->isFunctionLike())
                return fail(expression->range, "'await' outside function"_s);
            if (!isInAsyncFunction() && m_current->comprehension == ComprehensionKind::None)
                return fail(expression->range, "'await' outside async function"_s);
        }
        if (!visit(expression->as<Await>()->value))
            return false;
        m_current->isCoroutine = true;
        return true;
    case ExprKind::Compare:
        return visit(expression->as<Compare>()->left) && visitAll(expression->as<Compare>()->comparators);
    case ExprKind::FormattedValue: {
        auto* node = expression->as<FormattedValue>();
        return visit(node->value) && (!node->formatSpec || visit(node->formatSpec));
    }
    case ExprKind::Interpolation: {
        auto* node = expression->as<Interpolation>();
        return visit(node->value) && (!node->formatSpec || visit(node->formatSpec));
    }
    case ExprKind::JoinedStr:
        return visitAll(expression->as<JoinedStr>()->values);
    case ExprKind::TemplateStr:
        return visitAll(expression->as<TemplateStr>()->values);
    case ExprKind::Constant:
        return true;
    case ExprKind::Starred:
        return visit(expression->as<Starred>()->value);
    case ExprKind::Slice: {
        auto* slice = expression->as<Slice>();
        return (!slice->lower || visit(slice->lower)) && (!slice->upper || visit(slice->upper)) && (!slice->step || visit(slice->step));
    }
    case ExprKind::Name: {
        auto* name = expression->as<NameExpr>();
        if (m_current->inUnevaluatedAnnotation)
            return true;
        if (!addDefinition(name->id, name->ctx == ExprContext::Load ? Use : name->ctx == ExprContext::Del ? DefLocal | Deleted : DefLocal, name->range, name->ctx))
            return false;
        // super() needs the class it is called in.
        if (name->ctx == ExprContext::Load && m_current->isFunctionLike() && name->id->impl() == m_names.super->impl())
            return addDefinition(m_names.classCell, Use, name->range);
        return true;
    }
    case ExprKind::List:
        return visitAll(expression->as<List>()->elts);
    case ExprKind::Tuple:
        return visitAll(expression->as<Tuple>()->elts);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool SymbolTable::visitTypeParameterExpression(Expr* expression, Name name, const void* key, const Range& parameterRange, ASCIILiteral description)
{
    if (name->impl() == m_names.classDict->impl())
        return fail(parameterRange, makeString("reserved name '"_s, name->string(), "' cannot be used for type parameter"_s));
    if (!expression)
        return true;
    bool inClass = m_current->canSeeClassScope;
    if (!enter(ScopeKind::TypeVariable, name, key, expression->range))
        return false;
    m_current->canSeeClassScope = inClass;
    if (inClass && !addDefinition(m_names.classDict, Use, expression->range))
        return false;
    m_current->description = description;
    if (!visit(expression))
        return false;
    exit();
    return true;
}

bool SymbolTable::visit(TypeParam* param)
{
    if (!addDefinition(param->name, DefTypeParam | DefLocal, param->range))
        return false;
    switch (param->kind) {
    case TypeParamKind::TypeVar:
        if (!visitTypeParameterExpression(param->bound, param->name, param, param->range, param->bound && param->bound->is<Tuple>() ? "a TypeVar constraint"_s : "a TypeVar bound"_s))
            return false;
        return visitTypeParameterExpression(param->defaultValue, param->name, reinterpret_cast<const char*>(param) + 1, param->range, "a TypeVar default"_s);
    case TypeParamKind::TypeVarTuple:
        return visitTypeParameterExpression(param->defaultValue, param->name, param, param->range, "a TypeVarTuple default"_s);
    case TypeParamKind::ParamSpec:
        return visitTypeParameterExpression(param->defaultValue, param->name, param, param->range, "a ParamSpec default"_s);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool SymbolTable::visit(Pattern* pattern)
{
    switch (pattern->kind) {
    case PatternKind::MatchValue:
        return visit(pattern->as<MatchValue>()->value);
    case PatternKind::MatchSingleton:
        return true;
    case PatternKind::MatchSequence:
        return visitAll(pattern->as<MatchSequence>()->patterns);
    case PatternKind::MatchStar:
        return !pattern->as<MatchStar>()->name || addDefinition(pattern->as<MatchStar>()->name, DefLocal, pattern->range);
    case PatternKind::MatchMapping: {
        auto* mapping = pattern->as<MatchMapping>();
        if (!visitAll(mapping->keys) || !visitAll(mapping->patterns))
            return false;
        return !mapping->rest || addDefinition(mapping->rest, DefLocal, pattern->range);
    }
    case PatternKind::MatchClass: {
        auto* matchClass = pattern->as<MatchClass>();
        if (!visit(matchClass->cls) || !visitAll(matchClass->patterns))
            return false;
        for (unsigned i = 0; i < matchClass->kwdAttrs.size(); ++i) {
            if (!checkName(matchClass->kwdAttrs[i], matchClass->kwdPatterns[i]->range, ExprContext::Store))
                return false;
        }
        return visitAll(matchClass->kwdPatterns);
    }
    case PatternKind::MatchAs: {
        auto* as = pattern->as<MatchAs>();
        if (as->pattern && !visit(as->pattern))
            return false;
        return !as->name || addDefinition(as->name, DefLocal, pattern->range);
    }
    case PatternKind::MatchOr:
        return visitAll(pattern->as<MatchOr>()->patterns);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Second pass: where each name is bound ----
//
// bound: the names bound in enclosing functions. Null in a module.
// free: out, the names this scope and those it encloses leave for an enclosing function to bind.
// global: the names enclosing scopes declare global.

bool SymbolTable::analyzeName(Scope* scope, Symbol& symbol, NameSet* bound, NameSet& local, NameSet& free, NameSet& global, NameSet& typeParams, Scope* classEntry)
{
    auto* name = symbol.name->impl();
    unsigned flags = symbol.flags;
    if (flags & DefGlobal) {
        if (flags & DefNonlocal)
            return failAtDirective(scope, symbol.name, makeString("name '"_s, symbol.name->string(), "' is nonlocal and global"_s));
        symbol.binding = Binding::GlobalExplicit;
        global.add(name);
        if (bound)
            bound->remove(name);
        return true;
    }
    if (flags & DefNonlocal) {
        if (!bound)
            return failAtDirective(scope, symbol.name, "nonlocal declaration not allowed at module level"_s);
        if (!bound->contains(name))
            return failAtDirective(scope, symbol.name, makeString("no binding for nonlocal '"_s, symbol.name->string(), "' found"_s));
        if (typeParams.contains(name))
            return failAtDirective(scope, symbol.name, makeString("nonlocal binding not allowed for type parameter '"_s, symbol.name->string(), '\''));
        symbol.binding = Binding::Free;
        free.add(name);
        return true;
    }
    if (flags & DefBound) {
        symbol.binding = Binding::Local;
        local.add(name);
        global.remove(name);
        if (flags & DefTypeParam)
            typeParams.add(name);
        else
            typeParams.remove(name);
        return true;
    }
    // A scope that sees the namespace of a class looks a name of the class up there and in the globals,
    // not in the functions around the class.
    if (classEntry) {
        unsigned classFlags = classEntry->flagsOf(symbol.name);
        if (classFlags & DefGlobal) {
            symbol.binding = Binding::GlobalExplicit;
            return true;
        }
        if ((classFlags & DefBound) && !(classFlags & DefNonlocal)) {
            symbol.binding = Binding::GlobalImplicit;
            return true;
        }
    }
    if (bound && bound->contains(name)) {
        symbol.binding = Binding::Free;
        free.add(name);
        return true;
    }
    symbol.binding = Binding::GlobalImplicit;
    return true;
}

static bool isFreeInAnyChild(Scope* scope, Name name)
{
    for (Scope* child : scope->children) {
        // A comprehension that is itself part of the scope shares nothing with it through cells; what it encloses may.
        if (child->isInlinedComprehension ? isFreeInAnyChild(child, name) : child->bindingOf(name) == Binding::Free)
            return true;
    }
    return false;
}

// A comprehension that is not a generator runs as part of the enclosing scope. What it binds stays its own;
// what it leaves free and the enclosing scope binds is then an ordinary local there, not one to share through a cell.
void SymbolTable::inlineComprehension(Scope* parent, Scope* comprehension, NameSet& comprehensionFree)
{
    for (auto& symbol : comprehension->symbols) {
        if (symbol.binding != Binding::Free)
            continue;
        auto* name = symbol.name->impl();
        // These do not pass through a class scope, unless for an enclosed scope.
        if (parent->kind == ScopeKind::Class && (name == m_names.classCell->impl() || name == m_names.classDict->impl() || name == m_names.conditionalAnnotations->impl())) {
            symbol.binding = Binding::GlobalImplicit;
            if (!isFreeInAnyChild(comprehension, symbol.name))
                comprehensionFree.remove(name);
            continue;
        }
        Symbol* existing = parent->find(name);
        if (!existing || parent->kind == ScopeKind::Class)
            continue;
        if (symbol.flags & Use)
            existing->flags |= Use;
        if ((existing->flags & DefBound) && !isFreeInAnyChild(comprehension, symbol.name))
            comprehensionFree.remove(name);
    }
    comprehension->isInlinedComprehension = true;
}

bool SymbolTable::analyzeBlock(Scope* scope, NameSet* bound, NameSet& free, NameSet& global, NameSet& typeParams, Scope* classEntry)
{
    NameSet local;
    NameSet newBound;
    NameSet newGlobal;
    NameSet newFree;

    auto addAll = [](NameSet& to, const NameSet& from) {
        for (auto* name : from)
            to.add(name);
    };

    // The names of a class are not visible in the functions it encloses, so what those see is settled first.
    bool isClass = scope->kind == ScopeKind::Class;
    if (isClass) {
        addAll(newGlobal, global);
        if (bound)
            addAll(newBound, *bound);
    }

    for (auto& symbol : scope->symbols) {
        if (!analyzeName(scope, symbol, bound, local, free, global, typeParams, classEntry))
            return false;
    }

    if (!isClass) {
        if (scope->isFunctionLike())
            addAll(newBound, local);
        if (bound)
            addAll(newBound, *bound);
        addAll(newGlobal, global);
    } else {
        newBound.add(m_names.classCell->impl());
        newBound.add(m_names.classDict->impl());
        newBound.add(m_names.conditionalAnnotations->impl());
    }

    for (Scope* child : scope->children) {
        Scope* newClassEntry = nullptr;
        if (child->canSeeClassScope)
            newClassEntry = isClass ? scope : classEntry;

        NameSet childFree;
        if (!analyzeChildBlock(child, newBound, newFree, newGlobal, typeParams, newClassEntry, childFree))
            return false;
        if (child->comprehension != ComprehensionKind::None && !child->isGenerator && !scope->canSeeClassScope)
            inlineComprehension(scope, child, childFree);
        if (Symbol* symbol = child->canSeeClassScope ? child->find(m_names.classDict->impl()) : nullptr; symbol && symbol->binding == Binding::GlobalImplicit) {
            child->classDictIsShadowed = true;
            if (isClass)
                scope->needsClassDict = true;
        }
        addAll(newFree, childFree);
    }

    if (scope->isFunctionLike()) {
        // A local that an enclosed scope leaves free lives in a cell.
        for (auto& symbol : scope->symbols) {
            if (symbol.binding == Binding::Local && newFree.remove(symbol.name->impl()))
                symbol.binding = Binding::Cell;
        }
    } else if (isClass) {
        if (newFree.remove(m_names.classCell->impl()))
            scope->needsClassClosure = true;
        if (newFree.remove(m_names.classDict->impl()))
            scope->needsClassDict = true;
        if (newFree.remove(m_names.conditionalAnnotations->impl()))
            scope->hasConditionalAnnotations = true;
    }

    // What enclosed scopes leave free and this one does not bind passes through it.
    Vector<UniquedStringImpl*> passingThrough;
    for (auto* name : newFree)
        passingThrough.append(name);
    std::ranges::sort(passingThrough, [](auto* a, auto* b) { return codePointCompareLessThan(StringView(*a), StringView(*b)); });
    for (auto* name : passingThrough) {
        if (Symbol* existing = scope->find(name)) {
            // A method may use a name of an enclosing function that is also a name of the class.
            if (isClass || scope->canSeeClassScope)
                existing->flags |= DefFreeClass;
            continue;
        }
        if (bound && !bound->contains(name))
            continue;
        Name identifier = name->is8Bit() ? &m_tree.identifiers.makeIdentifier(m_vm, name->span8()) : &m_tree.identifiers.makeIdentifier(m_vm, name->span16());
        scope->add(identifier).binding = Binding::Free;
    }

    addAll(free, newFree);
    return true;
}

bool SymbolTable::analyzeChildBlock(Scope* scope, const NameSet& bound, const NameSet& free, const NameSet& global, const NameSet& typeParams, Scope* classEntry, NameSet& childFree)
{
    // A scope sees the bindings of those around it; its own do not flow back.
    NameSet childBound = bound;
    NameSet childGlobal = global;
    NameSet childTypeParams = typeParams;
    childFree = free;
    return analyzeBlock(scope, &childBound, childFree, childGlobal, childTypeParams, classEntry);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
