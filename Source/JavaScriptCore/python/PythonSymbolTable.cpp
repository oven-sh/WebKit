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

#include "config.h"
#include "PythonSymbolTable.h"
#include "PythonText.h"

#include "VM.h"
#include <wtf/HexNumber.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(Block);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SymbolTable);

void Block::remove(const Identifier& name)
{
    auto iterator = index.find(name.impl());
    if (iterator == index.end())
        return;
    symbols.removeAt(iterator->value);
    index.clear();
    for (unsigned i = 0; i < symbols.size(); ++i)
        index.add(symbols[i].name->impl(), i);
}

const Identifier& SymbolTable::mangle(VM& vm, Arena& arena, const Identifier* className, const Identifier& name)
{
    StringView ident = name.string();
    if (!className || ident.length() < 2 || ident[0] != '_' || ident[1] != '_')
        return name;
    // Not __this__, and not the name of a package with a dot in it, which only an import statement has.
    if ((ident[ident.length() - 1] == '_' && ident[ident.length() - 2] == '_') || ident.contains('.'))
        return name;
    StringView owner = className->string();
    unsigned underscores = 0;
    while (underscores < owner.length() && owner[underscores] == '_')
        ++underscores;
    if (underscores == owner.length())
        return name;
    String mangled = concatenate('_', owner.substring(underscores), ident);
    if (mangled.is8Bit())
        return arena.identifiers().makeIdentifier(vm, mangled.span8());
    return arena.identifiers().makeIdentifier(vm, mangled.span16());
}

class SymbolTableBuilder {
public:
    SymbolTableBuilder(VM& vm, Arena& arena, SymbolTable& table, SyntaxError& error)
        : m_vm(vm)
        , m_arena(arena)
        , m_table(table)
        , m_error(error)
#define INITIALIZE(member, text) , member(arena.identifiers().makeIdentifier(vm, text ""_span8))
        INITIALIZE(m_top, "top")
        INITIALIZE(m_lambda, "lambda")
        INITIALIZE(m_genexpr, "genexpr")
        INITIALIZE(m_listcomp, "listcomp")
        INITIALIZE(m_setcomp, "setcomp")
        INITIALIZE(m_dictcomp, "dictcomp")
        INITIALIZE(m_annotate, "__annotate__")
        INITIALIZE(m_class, "__class__")
        INITIALIZE(m_classdict, "__classdict__")
        INITIALIZE(m_conditionalAnnotations, "__conditional_annotations__")
        INITIALIZE(m_typeParamsAttribute, "__type_params__")
        INITIALIZE(m_debug, "__debug__")
        INITIALIZE(m_super, "super")
        INITIALIZE(m_format, ".format")
        INITIALIZE(m_typeParams, ".type_params")
        INITIALIZE(m_genericBase, ".generic_base")
        INITIALIZE(m_defaults, ".defaults")
        INITIALIZE(m_kwdefaults, ".kwdefaults")
        INITIALIZE(m_firstImplicitArgument, ".0")
#undef INITIALIZE
    {
    }

    bool build(Module& module)
    {
        if (!findFutureStatements(module))
            return false;

        if (!enterBlock(m_top, BlockType::Module, &module, { }))
            return false;
        m_table.m_top = m_current;
        switch (module.kind) {
        case Module::Kind::Module:
            m_current->hasDocstring = hasDocstring(module.body);
            [[fallthrough]];
        case Module::Kind::Interactive:
            if (!visit(module.body))
                return false;
            break;
        case Module::Kind::Expression:
            if (!visit(module.expression))
                return false;
            break;
        }
        exitBlock();

        NameSet free;
        NameSet global;
        NameSet typeParameters;
        return analyzeBlock(*m_table.m_top, nullptr, free, global, typeParameters, nullptr);
    }

    bool buildFragment(Statement* statement, Expression* expression, const Vector<Identifier>& freeVariables, const Identifier* privateName, bool canSeeClassScope, bool isNested, FragmentIs fragmentIs)
    {
        m_fragmentIs = fragmentIs;
        // As if it were in a function whose only variables are those.
        if (!enterBlock(m_top, BlockType::Function, &m_table, { }))
            return false;
        Block& top = *m_current;
        top.canSeeClassScope = canSeeClassScope;
        top.isNested = isNested;
        m_table.m_top = &top;
        m_private = privateName;
        m_fragmentTop = &top;
        m_fragmentFreeVariables = &freeVariables;
        if (statement ? !visit(statement) : !visit(expression))
            return false;
        exitBlock();

        top.symbols.clear();
        top.index.clear();
        for (auto& name : freeVariables)
            top.add(name.impl()->is8Bit() ? m_arena.identifiers().makeIdentifier(m_vm, name.impl()->span8()) : m_arena.identifiers().makeIdentifier(m_vm, name.impl()->span16())).flags = DefLocal;

        NameSet free;
        NameSet global;
        NameSet typeParameters;
        return analyzeBlock(top, nullptr, free, global, typeParameters, nullptr);
    }

private:
    // ---- Errors

    bool fail(String&& message, const Node& location)
    {
        if (!m_error)
            m_error = { SyntaxError::Kind::SyntaxError, false, WTF::move(message), location.line, static_cast<int>(location.column), location.endLine, static_cast<int>(location.endColumn) };
        return false;
    }

    bool failAtDirective(Block& block, const Identifier& name, String&& message)
    {
        for (auto& directive : block.directives) {
            if (directive.name->impl() == name.impl())
                return fail(WTF::move(message), directive.location);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isSafeToRecurse(const Node& location)
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return true;
        return fail("maximum recursion depth exceeded during compilation"_s, location);
    }

    // ---- from __future__ import

    static bool hasDocstring(Sequence<Statement*> body)
    {
        if (body.empty() || !body[0]->is<Expr>())
            return false;
        auto* constant = body[0]->as<Expr>().value->tryAs<Constant>();
        return constant && constant->type == Constant::Type::String;
    }

public:
    // Only a docstring and others of their kind may come before them.
    bool findFutureStatements(Module& module)
    {
        if (module.kind == Module::Kind::Expression)
            return true;
        for (size_t i = hasDocstring(module.body) ? 1 : 0; i < module.body.size(); ++i) {
            Statement& statement = *module.body[i];
            if (!statement.is<ImportFrom>())
                return true;
            auto& import = statement.as<ImportFrom>();
            if (import.level || !import.module || *import.module != "__future__"_s)
                return true;
            for (Alias* alias : import.names) {
                static constexpr ASCIILiteral thatMakeNoDifference[] = { "nested_scopes"_s, "generators"_s, "division"_s, "absolute_import"_s, "with_statement"_s, "print_function"_s, "unicode_literals"_s, "generator_stop"_s };
                if (*alias->name == "annotations"_s)
                    m_table.m_futureFeatures |= FutureAnnotations;
                else if (*alias->name == "barry_as_FLUFL"_s)
                    m_table.m_futureFeatures |= FutureBarryAsFLUFL;
                else if (*alias->name == "braces"_s)
                    return fail("not a chance"_s, *alias);
                else if (std::ranges::none_of(thatMakeNoDifference, [&] (ASCIILiteral feature) { return *alias->name == feature; }))
                    return fail(concatenate("future feature "_s, alias->name->string(), " is not defined"_s), *alias);
            }
            m_lastFutureStatement = &statement;
        }
        return true;
    }

private:

    bool hasFutureAnnotations() const { return m_table.m_futureFeatures & FutureAnnotations; }
    bool allowsTopLevelAwait() const { return (m_table.m_futureFeatures & AllowTopLevelAwait) && m_current->type == BlockType::Module; }
    bool isInAsyncFunction() const { return m_current->type == BlockType::Function && m_current->isCoroutine; }

    // ---- Blocks

    Block* newBlock(const Identifier& name, BlockType type, const void* key, const Node& location)
    {
        auto owned = makeUnique<Block>();
        Block* block = owned.get();
        m_table.m_blocks.append(WTF::move(owned));
        m_table.m_blocksByNode.set(key, block);
        block->name = &name;
        block->type = type;
        block->location = location;
        if (m_current) {
            // What a fragment is taken to be in stands for whatever it is in, which need be no function.
            block->isNested = m_current->isNested || (m_current != m_fragmentTop && m_current->isFunctionLike());
            block->isMethod = m_current->type == BlockType::Class && type == BlockType::Function;
        }
        return block;
    }

    void enterExistingBlock(Block* block, bool addToChildren)
    {
        m_stack.append(block);
        Block* previous = m_current;
        if (previous) {
            // No assignment expression anywhere in the outermost iterable of a comprehension, however deep.
            block->comprehensionIterableDepth = previous->comprehensionIterableDepth;
            if (previous->mangledNames && block->type != BlockType::Class)
                block->mangledNames = previous->mangledNames;
        }
        m_current = block;
        // With from __future__ import annotations they are only ever strings.
        if (hasFutureAnnotations() && block->type == BlockType::Annotation)
            return;
        if (addToChildren && previous)
            previous->children.append(block);
    }

    bool enterBlock(const Identifier& name, BlockType type, const void* key, const Node& location)
    {
        enterExistingBlock(newBlock(name, type, key, location), true);
        if (type == BlockType::Annotation || type == BlockType::TypeVariable || type == BlockType::TypeAlias) {
            // What is asked of them, which the code that is made for them looks at.
            return addDefinition(m_format, DefParameter, location) && addDefinition(m_format, Use, location);
        }
        return true;
    }

    void exitBlock()
    {
        m_stack.removeLast();
        m_current = m_stack.isEmpty() ? nullptr : m_stack.last();
    }

    // ---- Names

    const Identifier& maybeMangle(Block& block, const Identifier& name)
    {
        if (block.mangledNames && !block.mangledNames->names.contains(name.impl()))
            return name;
        return SymbolTable::mangle(m_vm, m_arena, m_private, name);
    }

    unsigned lookup(Block& block, const Identifier& name) { return block.flagsOf(maybeMangle(block, name)); }
    unsigned lookup(const Identifier& name) { return lookup(*m_current, name); }

    bool addDefinitionTo(Block& block, const Identifier& name, unsigned flag, const Node& location)
    {
        const Identifier& mangled = maybeMangle(*m_current, name);
        Symbol& symbol = block.add(mangled);
        if ((flag & DefParameter) && (symbol.flags & DefParameter))
            return fail(concatenate("duplicate argument '"_s, name.string(), "' in function definition"_s), location);
        if ((flag & DefTypeParameter) && (symbol.flags & DefTypeParameter))
            return fail(concatenate("duplicate type parameter '"_s, name.string(), '\''), location);
        unsigned flags = symbol.flags | flag;
        if (block.isVisitingComprehensionTarget) {
            if (flags & (DefGlobal | DefNonlocal))
                return fail(concatenate("comprehension inner loop cannot rebind assignment expression target '"_s, name.string(), '\''), location);
            flags |= DefComprehensionIteration;
        }
        symbol.flags = flags;
        if (flag & DefParameter)
            block.parameters.append(&mangled);
        else if (flag & DefGlobal)
            m_table.m_top->add(mangled).flags |= flag;
        return true;
    }

    bool checkName(const Identifier& name, const Node& location, ExpressionContext context)
    {
        if (name.impl() != m_debug.impl())
            return true;
        if (context == ExpressionContext::Store)
            return fail("cannot assign to __debug__"_s, location);
        if (context == ExpressionContext::Del)
            return fail("cannot delete __debug__"_s, location);
        return true;
    }

    bool addDefinition(const Identifier& name, unsigned flag, const Node& location, ExpressionContext context)
    {
        if ((flag & (DefParameter | DefLocal | DefImport)) && !checkName(name, location, context))
            return false;
        if ((flag & DefTypeParameter) && m_current->mangledNames)
            m_current->mangledNames->names.add(name.impl());
        return addDefinitionTo(*m_current, name, flag, location);
    }

    bool addDefinition(const Identifier& name, unsigned flag, const Node& location)
    {
        return addDefinition(name, flag, location, flag == Use ? ExpressionContext::Load : ExpressionContext::Store);
    }

    void recordDirective(const Identifier& name, const Node& location)
    {
        m_current->directives.append({ &maybeMangle(*m_current, name), location });
    }

    // ---- The first pass: what each block does with each name

    template<typename T>
    bool visit(Sequence<T*> sequence)
    {
        for (T* item : sequence) {
            if (item && !visit(item))
                return false;
        }
        return true;
    }

    class ConditionalBlock {
    public:
        explicit ConditionalBlock(Block& block)
            : m_block(block)
            , m_previous(block.isInConditionalBlock)
        {
            block.isInConditionalBlock = true;
        }
        ~ConditionalBlock() { m_block.isInConditionalBlock = m_previous; }
    private:
        Block& m_block;
        bool m_previous;
    };

    // Directly, that is. A fragment is told whether it was.
    bool isInClassBody() const { return m_current->type == BlockType::Class || (m_current == m_fragmentTop && m_current->canSeeClassScope); }

    bool visitTypeParameters(Sequence<TypeParameter*> parameters)
    {
        return visit(parameters);
    }

    bool enterTypeParameterBlock(const Identifier& name, const void* key, bool hasDefaults, bool hasKeywordDefaults, bool isClass, const Node& location)
    {
        bool isInClass = isInClassBody();
        if (!enterBlock(name, BlockType::TypeParameters, key, location))
            return false;
        if (isInClass) {
            m_current->canSeeClassScope = true;
            if (!addDefinition(m_classdict, Use, location))
                return false;
        }
        if (isClass) {
            // Set when the tuple of type parameters is made, and used when the bases are.
            if (!addDefinition(m_typeParams, DefLocal, location) || !addDefinition(m_typeParams, Use, location))
                return false;
            if (!addDefinition(m_genericBase, DefLocal, location) || !addDefinition(m_genericBase, Use, location))
                return false;
        }
        if (hasDefaults && !addDefinition(m_defaults, DefParameter, location))
            return false;
        if (hasKeywordDefaults && !addDefinition(m_kwdefaults, DefParameter, location))
            return false;
        return true;
    }

    static bool hasKeywordOnlyDefaults(Arguments& arguments)
    {
        for (Expression* value : arguments.keywordDefaults) {
            if (value)
                return true;
        }
        return false;
    }

    bool visitFunctionDefinition(FunctionDef& node)
    {
        if (!addDefinition(*node.name, DefLocal, node))
            return false;
        Arguments& arguments = *node.arguments;
        if (!visit(arguments.defaults) || !visit(arguments.keywordDefaults) || !visit(node.decorators))
            return false;
        bool isGeneric = !node.typeParameters.empty();
        if (isGeneric) {
            // CPython asks whether there is a sequence of defaults, and there always is one.
            if (!enterTypeParameterBlock(*node.name, node.typeParameters.data(), true, hasKeywordOnlyDefaults(arguments), false, node))
                return false;
            if (!visitTypeParameters(node.typeParameters))
                return false;
        }
        Block* block = newBlock(*node.name, BlockType::Function, &node, node);
        block->hasDocstring = hasDocstring(node.body);
        if (!visitAnnotations(node, arguments, node.returns))
            return false;
        enterExistingBlock(block, true);
        if (node.isAsync)
            m_current->isCoroutine = true;
        if (!visit(arguments) || !visit(node.body))
            return false;
        exitBlock();
        if (isGeneric)
            exitBlock();
        return true;
    }

    bool visitClassDefinition(ClassDef& node)
    {
        if (!addDefinition(*node.name, DefLocal, node) || !visit(node.decorators))
            return false;
        const Identifier* previousPrivate = m_private;
        bool isGeneric = !node.typeParameters.empty();
        if (isGeneric) {
            if (!enterTypeParameterBlock(*node.name, node.typeParameters.data(), false, false, true, node))
                return false;
            m_private = node.name;
            m_current->mangledNames = adoptRef(*new RefCountedNameSet);
            if (!visitTypeParameters(node.typeParameters))
                return false;
        }
        if (!visit(node.bases) || !checkKeywords(node.keywords) || !visit(node.keywords))
            return false;
        m_current->hasClassDefinition = true;
        if (!enterBlock(*node.name, BlockType::Class, &node, node))
            return false;
        m_private = node.name;
        if (isGeneric) {
            if (!addDefinition(m_typeParamsAttribute, DefLocal, node) || !addDefinition(m_typeParams, Use, node))
                return false;
        }
        m_current->hasDocstring = hasDocstring(node.body);
        if (!visit(node.body))
            return false;
        exitBlock();
        if (isGeneric)
            exitBlock();
        m_private = previousPrivate;
        return true;
    }

    bool visitTypeAlias(TypeAlias& node)
    {
        if (!visit(node.name))
            return false;
        const Identifier& name = *node.name->as<Name>().id;
        bool isInClass = isInClassBody();
        bool isGeneric = !node.typeParameters.empty();
        if (isGeneric) {
            if (!enterTypeParameterBlock(name, node.typeParameters.data(), false, false, false, node))
                return false;
            if (!visitTypeParameters(node.typeParameters))
                return false;
        }
        if (!enterBlock(name, BlockType::TypeAlias, &node, node))
            return false;
        m_current->canSeeClassScope = isInClass;
        if (isInClass && !addDefinition(m_classdict, Use, *node.value))
            return false;
        if (!visit(node.value))
            return false;
        exitBlock();
        if (isGeneric)
            exitBlock();
        return true;
    }

    bool visitAnnotatedAssignment(AnnAssign& node)
    {
        m_current->usesAnnotations = true;
        if (auto* name = node.target->tryAs<Name>()) {
            unsigned flags = lookup(*name->id);
            if ((flags & (DefGlobal | DefNonlocal)) && m_current != m_table.m_top && node.isSimple)
                return fail(concatenate("annotated name '"_s, name->id->string(), flags & DefGlobal ? "' can't be global"_s : "' can't be nonlocal"_s), node);
            if (node.isSimple) {
                if (!addDefinition(*name->id, DefAnnotation | DefLocal, *name))
                    return false;
            } else if (node.value && !addDefinition(*name->id, DefLocal, *name))
                return false;
        } else if (!visit(node.target))
            return false;
        if (!visitAnnotation(*node.annotation))
            return false;
        return !node.value || visit(node.value);
    }

    bool visitScopeDeclaration(Sequence<const Identifier*> names, bool isGlobal, const Node& location)
    {
        ASCIILiteral kind = isGlobal ? "global"_s : "nonlocal"_s;
        for (const Identifier* name : names) {
            unsigned flags = lookup(*name);
            if (flags & DefParameter)
                return fail(concatenate("name '"_s, name->string(), "' is parameter and "_s, kind), location);
            if (flags & Use)
                return fail(concatenate("name '"_s, name->string(), "' is used prior to "_s, kind, " declaration"_s), location);
            if (flags & DefAnnotation)
                return fail(concatenate("annotated name '"_s, name->string(), "' can't be "_s, kind), location);
            if (flags & DefLocal)
                return fail(concatenate("name '"_s, name->string(), "' is assigned to before "_s, kind, " declaration"_s), location);
            if (!addDefinition(*name, isGlobal ? DefGlobal : DefNonlocal, location))
                return false;
            recordDirective(*name, location);
        }
        return true;
    }

    bool checkFutureImport(ImportFrom& node)
    {
        if (!node.module || node.level || *node.module != "__future__"_s)
            return true;
        // Where the last of them is, at the top of the file, is not known to a part of it. This was seen to when the whole was compiled.
        if (m_fragmentTop)
            return true;
        bool isAfter = !m_lastFutureStatement || node.line > m_lastFutureStatement->line
            || (node.line == m_lastFutureStatement->endLine && node.column > m_lastFutureStatement->endColumn);
        if (isAfter)
            return fail("from __future__ imports must occur at the beginning of the file"_s, node);
        return true;
    }

    bool visit(Statement* statement)
    {
        if (!isSafeToRecurse(*statement))
            return false;
        switch (statement->kind) {
        case Statement::Kind::FunctionDef:
            return visitFunctionDefinition(statement->as<FunctionDef>());
        case Statement::Kind::ClassDef:
            return visitClassDefinition(statement->as<ClassDef>());
        case Statement::Kind::TypeAlias:
            return visitTypeAlias(statement->as<TypeAlias>());
        case Statement::Kind::Return: {
            auto& node = statement->as<Return>();
            if (!node.value)
                return true;
            m_current->returnsValue = true;
            return visit(node.value);
        }
        case Statement::Kind::Delete:
            return visit(statement->as<Delete>().targets);
        case Statement::Kind::Assign: {
            auto& node = statement->as<Assign>();
            return visit(node.targets) && visit(node.value);
        }
        case Statement::Kind::AnnAssign:
            return visitAnnotatedAssignment(statement->as<AnnAssign>());
        case Statement::Kind::AugAssign: {
            auto& node = statement->as<AugAssign>();
            return visit(node.target) && visit(node.value);
        }
        case Statement::Kind::For: {
            auto& node = statement->as<For>();
            if (node.isAsync) {
                if (allowsTopLevelAwait())
                    m_current->isCoroutine = true;
                if (!m_current->isCoroutine)
                    return fail("'async for' outside async function"_s, node);
            }
            if (!visit(node.target) || !visit(node.iterable))
                return false;
            ConditionalBlock conditional(*m_current);
            return visit(node.body) && visit(node.orElse);
        }
        case Statement::Kind::While: {
            auto& node = statement->as<While>();
            if (!visit(node.test))
                return false;
            ConditionalBlock conditional(*m_current);
            return visit(node.body) && visit(node.orElse);
        }
        case Statement::Kind::If: {
            auto& node = statement->as<If>();
            if (!visit(node.test))
                return false;
            ConditionalBlock conditional(*m_current);
            return visit(node.body) && visit(node.orElse);
        }
        case Statement::Kind::Match: {
            auto& node = statement->as<Match>();
            if (!visit(node.subject))
                return false;
            ConditionalBlock conditional(*m_current);
            return visit(node.cases);
        }
        case Statement::Kind::Raise: {
            auto& node = statement->as<Raise>();
            return (!node.exception || visit(node.exception)) && (!node.cause || visit(node.cause));
        }
        case Statement::Kind::Try: {
            auto& node = statement->as<Try>();
            ConditionalBlock conditional(*m_current);
            return visit(node.body) && visit(node.handlers) && visit(node.orElse) && visit(node.finalBody);
        }
        case Statement::Kind::Assert: {
            auto& node = statement->as<Assert>();
            return visit(node.test) && (!node.message || visit(node.message));
        }
        case Statement::Kind::Import:
            m_current->hasImport = true;
            return visit(statement->as<Import>().names);
        case Statement::Kind::ImportFrom: {
            m_current->hasImport = true;
            auto& node = statement->as<ImportFrom>();
            return visit(node.names) && checkFutureImport(node);
        }
        case Statement::Kind::Global:
            return visitScopeDeclaration(statement->as<Global>().names, true, *statement);
        case Statement::Kind::Nonlocal:
            return visitScopeDeclaration(statement->as<Nonlocal>().names, false, *statement);
        case Statement::Kind::Expr:
            return visit(statement->as<Expr>().value);
        case Statement::Kind::Pass:
        case Statement::Kind::Break:
        case Statement::Kind::Continue:
            return true;
        case Statement::Kind::With: {
            auto& node = statement->as<With>();
            if (node.isAsync) {
                if (allowsTopLevelAwait())
                    m_current->isCoroutine = true;
                if (!m_current->isCoroutine)
                    return fail("'async with' outside async function"_s, node);
            }
            ConditionalBlock conditional(*m_current);
            return visit(node.items) && visit(node.body);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // x := in a comprehension binds x in what the comprehension is in.
    bool extendNamedExpressionScope(Name& target)
    {
        const Identifier& name = *target.id;
        for (unsigned i = m_stack.size(); i--;) {
            Block& block = *m_stack[i];
            if (block.comprehension != ComprehensionType::None) {
                unsigned flags = lookup(block, name);
                if ((flags & DefComprehensionIteration) && (flags & DefLocal))
                    return fail(concatenate("assignment expression cannot rebind comprehension iteration variable '"_s, name.string(), '\''), target);
                continue;
            }
            // What stands in for whatever a fragment is in has the variables that the fragment is known to use of it. Any other is a global.
            if (&block == m_fragmentTop && !m_fragmentFreeVariables->contains(name)) {
                if (!addDefinition(name, DefGlobal, target))
                    return false;
                recordDirective(name, target);
                return true;
            }
            switch (block.type) {
            case BlockType::Function:
                if (!addDefinition(name, lookup(block, name) & DefGlobal ? DefGlobal : DefNonlocal, target))
                    return false;
                recordDirective(name, target);
                return addDefinitionTo(block, name, DefLocal, target);
            case BlockType::Module:
                if (!addDefinition(name, DefGlobal, target))
                    return false;
                recordDirective(name, target);
                return addDefinitionTo(block, name, DefGlobal, target);
            case BlockType::Class:
                return fail("assignment expression within a comprehension cannot be used in a class body"_s, target);
            case BlockType::TypeParameters:
                return fail("assignment expression within a comprehension cannot be used within the definition of a generic"_s, target);
            case BlockType::TypeAlias:
                return fail("assignment expression within a comprehension cannot be used in a type alias"_s, target);
            case BlockType::TypeVariable:
                return fail("assignment expression within a comprehension cannot be used in a TypeVar bound"_s, target);
            case BlockType::Annotation:
                continue;
            }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What has no place in an annotation or in the definition of a type.
    bool failIfInAnnotation(ASCIILiteral what, const Node& location)
    {
        switch (m_current->type) {
        case BlockType::Annotation:
            return fail(concatenate(what, " cannot be used within an annotation"_s), location);
        case BlockType::TypeVariable:
            return fail(concatenate(what, " cannot be used within "_s, m_current->scopeInfo), location);
        case BlockType::TypeAlias:
            return fail(concatenate(what, " cannot be used within a type alias"_s), location);
        case BlockType::TypeParameters:
            return fail(concatenate(what, " cannot be used within the definition of a generic"_s), location);
        default:
            return true;
        }
    }

    bool visitYield(Expression* value, const Node& location)
    {
        if (!failIfInAnnotation("yield expression"_s, location))
            return false;
        if (value && !visit(value))
            return false;
        m_current->isGenerator = true;
        switch (m_current->comprehension) {
        case ComprehensionType::None:
            return true;
        case ComprehensionType::List:
            return fail("'yield' inside list comprehension"_s, location);
        case ComprehensionType::Set:
            return fail("'yield' inside set comprehension"_s, location);
        case ComprehensionType::Dict:
            return fail("'yield' inside dict comprehension"_s, location);
        case ComprehensionType::Generator:
            return fail("'yield' inside generator expression"_s, location);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool checkKeywords(Sequence<Keyword*> keywords)
    {
        for (Keyword* keyword : keywords) {
            if (keyword->name && !checkName(*keyword->name, *keyword, ExpressionContext::Store))
                return false;
        }
        return true;
    }

    bool visit(Expression* expression)
    {
        if (!isSafeToRecurse(*expression))
            return false;
        switch (expression->kind) {
        case Expression::Kind::NamedExpr: {
            auto& node = expression->as<NamedExpr>();
            if (!failIfInAnnotation("named expression"_s, node))
                return false;
            if (m_current->comprehensionIterableDepth)
                return fail("assignment expression cannot be used in a comprehension iterable expression"_s, node);
            if (m_current->comprehension != ComprehensionType::None && !extendNamedExpressionScope(node.target->as<Name>()))
                return false;
            return visit(node.value) && visit(node.target);
        }
        case Expression::Kind::BoolOp:
            return visit(expression->as<BoolOp>().values);
        case Expression::Kind::BinOp: {
            auto& node = expression->as<BinOp>();
            return visit(node.left) && visit(node.right);
        }
        case Expression::Kind::UnaryOp:
            return visit(expression->as<UnaryOp>().operand);
        case Expression::Kind::Lambda: {
            auto& node = expression->as<Lambda>();
            if (!visit(node.arguments->defaults) || !visit(node.arguments->keywordDefaults))
                return false;
            if (!enterBlock(m_lambda, BlockType::Function, &node, node))
                return false;
            if (!visit(*node.arguments) || !visit(node.body))
                return false;
            exitBlock();
            return true;
        }
        case Expression::Kind::IfExp: {
            auto& node = expression->as<IfExp>();
            return visit(node.test) && visit(node.body) && visit(node.orElse);
        }
        case Expression::Kind::Dict: {
            auto& node = expression->as<Dict>();
            return visit(node.keys) && visit(node.values);
        }
        case Expression::Kind::Set:
            return visit(expression->as<Set>().elements);
        case Expression::Kind::GeneratorExp: {
            auto& node = expression->as<GeneratorExp>();
            return visitComprehension(node, m_genexpr, ComprehensionType::Generator, node.generators, node.element, nullptr);
        }
        case Expression::Kind::ListComp: {
            auto& node = expression->as<ListComp>();
            return visitComprehension(node, m_listcomp, ComprehensionType::List, node.generators, node.element, nullptr);
        }
        case Expression::Kind::SetComp: {
            auto& node = expression->as<SetComp>();
            return visitComprehension(node, m_setcomp, ComprehensionType::Set, node.generators, node.element, nullptr);
        }
        case Expression::Kind::DictComp: {
            auto& node = expression->as<DictComp>();
            return visitComprehension(node, m_dictcomp, ComprehensionType::Dict, node.generators, node.key, node.value);
        }
        case Expression::Kind::Yield:
            return visitYield(expression->as<Yield>().value, *expression);
        case Expression::Kind::YieldFrom:
            return visitYield(expression->as<YieldFrom>().value, *expression);
        case Expression::Kind::Await: {
            if (!failIfInAnnotation("await expression"_s, *expression))
                return false;
            if (!allowsTopLevelAwait()) {
                if (!m_current->isFunctionLike())
                    return fail("'await' outside function"_s, *expression);
                if (!isInAsyncFunction() && m_current->comprehension == ComprehensionType::None)
                    return fail("'await' outside async function"_s, *expression);
            }
            if (!visit(expression->as<Await>().value))
                return false;
            m_current->isCoroutine = true;
            return true;
        }
        case Expression::Kind::Compare: {
            auto& node = expression->as<Compare>();
            return visit(node.left) && visit(node.comparators);
        }
        case Expression::Kind::Call: {
            auto& node = expression->as<Call>();
            return visit(node.function) && visit(node.arguments) && checkKeywords(node.keywords) && visit(node.keywords);
        }
        case Expression::Kind::FormattedValue: {
            auto& node = expression->as<FormattedValue>();
            return visit(node.value) && (!node.formatSpecification || visit(node.formatSpecification));
        }
        case Expression::Kind::Interpolation: {
            auto& node = expression->as<Interpolation>();
            return visit(node.value) && (!node.formatSpecification || visit(node.formatSpecification));
        }
        case Expression::Kind::JoinedStr:
            return visit(expression->as<JoinedStr>().values);
        case Expression::Kind::TemplateStr:
            return visit(expression->as<TemplateStr>().values);
        case Expression::Kind::Constant:
            return true;
        case Expression::Kind::Attribute: {
            auto& node = expression->as<Attribute>();
            if (node.context == ExpressionContext::Store && node.value->kind == Expression::Kind::Name && *node.value->as<Name>().id == "self"_s) {
                // Of the nearest class that this is inside of.
                for (unsigned i = m_stack.size() - 1; i--;) {
                    if (m_stack[i]->type != BlockType::Class)
                        continue;
                    if (!m_stack[i]->staticAttributes.contains(node.attribute))
                        m_stack[i]->staticAttributes.append(node.attribute);
                    break;
                }
            }
            return checkName(*node.attribute, node, node.context) && visit(node.value);
        }
        case Expression::Kind::Subscript: {
            auto& node = expression->as<Subscript>();
            return visit(node.value) && visit(node.slice);
        }
        case Expression::Kind::Starred:
            return visit(expression->as<Starred>().value);
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            return (!node.lower || visit(node.lower)) && (!node.upper || visit(node.upper)) && (!node.step || visit(node.step));
        }
        case Expression::Kind::Name: {
            auto& node = expression->as<Name>();
            if (m_current->isInUnevaluatedAnnotation)
                return true;
            bool isLoad = node.context == ExpressionContext::Load;
            if (!addDefinition(*node.id, isLoad ? Use : DefLocal, node, node.context))
                return false;
            // super() needs to know what class it is in.
            if (isLoad && m_current->isFunctionLike() && node.id->impl() == m_super.impl())
                return addDefinition(m_class, Use, node);
            return true;
        }
        case Expression::Kind::List:
            return visit(expression->as<List>().elements);
        case Expression::Kind::Tuple:
            return visit(expression->as<Tuple>().elements);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool visitTypeParameterExpression(Expression* expression, const Identifier& name, const void* key, TypeParameter& parameter, ASCIILiteral scopeInfo)
    {
        if (name.impl() == m_classdict.impl())
            return fail(concatenate("reserved name '"_s, name.string(), "' cannot be used for type parameter"_s), parameter);
        if (!expression)
            return true;
        bool isInClass = m_current->canSeeClassScope;
        if (!enterBlock(name, BlockType::TypeVariable, key, *expression))
            return false;
        m_current->canSeeClassScope = isInClass;
        if (isInClass && !addDefinition(m_classdict, Use, *expression))
            return false;
        m_current->scopeInfo = scopeInfo;
        if (!visit(expression))
            return false;
        exitBlock();
        return true;
    }

    bool visit(TypeParameter* parameter)
    {
        if (!addDefinition(*parameter->name, DefTypeParameter | DefLocal, *parameter))
            return false;
        const void* secondKey = reinterpret_cast<const char*>(parameter) + 1;
        switch (parameter->kind) {
        case TypeParameter::Kind::TypeVar: {
            ASCIILiteral boundInfo = parameter->bound && parameter->bound->is<Tuple>() ? "a TypeVar constraint"_s : "a TypeVar bound"_s;
            return visitTypeParameterExpression(parameter->bound, *parameter->name, parameter, *parameter, boundInfo)
                && visitTypeParameterExpression(parameter->defaultValue, *parameter->name, secondKey, *parameter, "a TypeVar default"_s);
        }
        case TypeParameter::Kind::TypeVarTuple:
            return visitTypeParameterExpression(parameter->defaultValue, *parameter->name, parameter, *parameter, "a TypeVarTuple default"_s);
        case TypeParameter::Kind::ParamSpec:
            return visitTypeParameterExpression(parameter->defaultValue, *parameter->name, parameter, *parameter, "a ParamSpec default"_s);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool visit(Pattern* pattern)
    {
        if (!isSafeToRecurse(*pattern))
            return false;
        switch (pattern->kind) {
        case Pattern::Kind::MatchValue:
            return visit(pattern->as<MatchValue>().value);
        case Pattern::Kind::MatchSingleton:
            return true;
        case Pattern::Kind::MatchSequence:
            return visit(pattern->as<MatchSequence>().patterns);
        case Pattern::Kind::MatchStar: {
            auto& node = pattern->as<MatchStar>();
            return !node.name || addDefinition(*node.name, DefLocal, node);
        }
        case Pattern::Kind::MatchMapping: {
            auto& node = pattern->as<MatchMapping>();
            return visit(node.keys) && visit(node.patterns) && (!node.rest || addDefinition(*node.rest, DefLocal, node));
        }
        case Pattern::Kind::MatchClass: {
            auto& node = pattern->as<MatchClass>();
            if (!visit(node.cls) || !visit(node.patterns))
                return false;
            for (size_t i = 0; i < node.keywordAttributes.size(); ++i) {
                if (!checkName(*node.keywordAttributes[i], *node.keywordPatterns[i], ExpressionContext::Store))
                    return false;
            }
            return visit(node.keywordPatterns);
        }
        case Pattern::Kind::MatchAs: {
            auto& node = pattern->as<MatchAs>();
            return (!node.pattern || visit(node.pattern)) && (!node.name || addDefinition(*node.name, DefLocal, node));
        }
        case Pattern::Kind::MatchOr:
            return visit(pattern->as<MatchOr>().patterns);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The annotation of a variable. All those of a block are evaluated together, by one block.
    bool visitAnnotation(Expression& annotation)
    {
        // Those of a function's variables never are.
        bool isUnevaluated = m_current->type == BlockType::Function;
        // Those of a module count as conditional, since it may have run only in part.
        bool isConditional = (m_current->type == BlockType::Class && m_current->isInConditionalBlock) || m_current->type == BlockType::Module;
        if (isConditional && !m_current->hasConditionalAnnotations) {
            m_current->hasConditionalAnnotations = true;
            if (!addDefinition(m_conditionalAnnotations, Use, annotation))
                return false;
        }
        Block* parent = m_current;
        if (!parent->annotationBlock) {
            if (!enterBlock(m_annotate, BlockType::Annotation, reinterpret_cast<const char*>(parent) + 1, annotation))
                return false;
            parent->annotationBlock = m_current;
            if (parent->type == BlockType::Class && !hasFutureAnnotations()) {
                m_current->canSeeClassScope = true;
                parent->needsClassDict = true;
                if (!addDefinition(m_classdict, Use, annotation))
                    return false;
            }
        } else
            enterExistingBlock(parent->annotationBlock, false);
        m_current->isInUnevaluatedAnnotation = isUnevaluated;
        bool result = visit(&annotation);
        m_current->isInUnevaluatedAnnotation = false;
        exitBlock();
        return result;
    }

    bool visitArgumentAnnotations(Sequence<Argument*> arguments)
    {
        for (Argument* argument : arguments) {
            if (!visitArgumentAnnotation(argument))
                return false;
        }
        return true;
    }

    bool visitArgumentAnnotation(Argument* argument)
    {
        if (!argument || !argument->annotation)
            return true;
        m_current->usesAnnotations = true;
        return visit(argument->annotation);
    }

    // Those of a function's parameters and of what it returns.
    bool visitAnnotations(FunctionDef& function, Arguments& arguments, Expression* returns)
    {
        bool isInClass = m_current->canSeeClassScope || m_current->type == BlockType::Class;
        if (!enterBlock(m_annotate, BlockType::Annotation, &arguments, function))
            return false;
        if (isInClass) {
            m_current->canSeeClassScope = true;
            if (!addDefinition(m_classdict, Use, function))
                return false;
        }
        if (!visitArgumentAnnotations(arguments.positionalOnly) || !visitArgumentAnnotations(arguments.positional)
            || !visitArgumentAnnotation(arguments.variadic) || !visitArgumentAnnotation(arguments.keywordVariadic)
            || !visitArgumentAnnotations(arguments.keywordOnly))
            return false;
        if (returns) {
            m_current->usesAnnotations = true;
            if (!visit(returns))
                return false;
        }
        exitBlock();
        return true;
    }

    bool visitParameters(Sequence<Argument*> arguments)
    {
        for (Argument* argument : arguments) {
            if (!addDefinition(*argument->name, DefParameter, *argument))
                return false;
        }
        return true;
    }

    bool visit(Arguments& arguments)
    {
        if (!visitParameters(arguments.positionalOnly) || !visitParameters(arguments.positional) || !visitParameters(arguments.keywordOnly))
            return false;
        if (arguments.variadic) {
            if (!addDefinition(*arguments.variadic->name, DefParameter, *arguments.variadic))
                return false;
            m_current->hasVariadic = true;
        }
        if (arguments.keywordVariadic) {
            if (!addDefinition(*arguments.keywordVariadic->name, DefParameter, *arguments.keywordVariadic))
                return false;
            m_current->hasKeywordVariadic = true;
        }
        return true;
    }

    bool visit(ExceptHandler* handler)
    {
        if (handler->type && !visit(handler->type))
            return false;
        if (handler->name && !addDefinition(*handler->name, DefLocal, *handler))
            return false;
        return visit(handler->body);
    }

    bool visit(WithItem* item)
    {
        return visit(item->contextExpression) && (!item->optionalVariables || visit(item->optionalVariables));
    }

    bool visit(MatchCase* matchCase)
    {
        return visit(matchCase->pattern) && (!matchCase->guard || visit(matchCase->guard)) && visit(matchCase->body);
    }

    bool visit(Keyword* keyword) { return visit(keyword->value); }

    bool visit(Alias* alias)
    {
        // import a.b binds a.
        const Identifier& name = alias->asName ? *alias->asName : *alias->name;
        if (name == "*"_s) {
            if (m_current->type != BlockType::Module)
                return fail("import * only allowed at module level"_s, *alias);
            return true;
        }
        StringView view = name.string();
        size_t dot = view.find('.');
        if (dot == notFound)
            return addDefinition(name, DefImport, *alias);
        String first = view.left(dot).toString();
        const Identifier& stored = first.is8Bit() ? m_arena.identifiers().makeIdentifier(m_vm, first.span8()) : m_arena.identifiers().makeIdentifier(m_vm, first.span16());
        return addDefinition(stored, DefImport, *alias);
    }

    bool visit(Comprehension* comprehension)
    {
        m_current->isVisitingComprehensionTarget = true;
        bool ok = visit(comprehension->target);
        m_current->isVisitingComprehensionTarget = false;
        if (!ok)
            return false;
        ++m_current->comprehensionIterableDepth;
        ok = visit(comprehension->iterable);
        --m_current->comprehensionIterableDepth;
        if (!ok || !visit(comprehension->conditions))
            return false;
        if (comprehension->isAsync)
            m_current->isCoroutine = true;
        return true;
    }

    bool visitComprehension(Expression& node, const Identifier& name, ComprehensionType type, Sequence<Comprehension*> generators, Expression* element, Expression* value)
    {
        bool isGenerator = type == ComprehensionType::Generator;
        Comprehension& outermost = *generators[0];
        // The outermost iterable is evaluated where the comprehension is, and handed to it.
        ++m_current->comprehensionIterableDepth;
        bool ok = visit(outermost.iterable);
        --m_current->comprehensionIterableDepth;
        if (!ok)
            return false;

        if (!enterBlock(name, BlockType::Function, &node, node))
            return false;
        m_current->comprehension = type;
        if (outermost.isAsync)
            m_current->isCoroutine = true;
        if (!addDefinition(m_firstImplicitArgument, DefParameter, m_current->location))
            return false;
        m_current->isVisitingComprehensionTarget = true;
        ok = visit(outermost.target);
        m_current->isVisitingComprehensionTarget = false;
        if (!ok || !visit(outermost.conditions) || !visit(generators.subspan(1)))
            return false;
        if (value && !visit(value))
            return false;
        if (!visit(element))
            return false;
        m_current->isGenerator = isGenerator;
        bool isAsync = m_current->isCoroutine && !isGenerator;
        exitBlock();

        if (isAsync && !isInAsyncFunction() && m_current->comprehension == ComprehensionType::None && !allowsTopLevelAwait())
            return fail("asynchronous comprehension outside of an asynchronous function"_s, node);
        if (isAsync)
            m_current->isCoroutine = true;
        return true;
    }

    // ---- The second pass: what each name is, given what the blocks around it bind

    using Scopes = HashMap<UniquedStringImpl*, NameScope>;

    static void unite(NameSet& into, const NameSet& from)
    {
        for (UniquedStringImpl* name : from)
            into.add(name);
    }

    bool analyzeName(Block& block, Scopes& scopes, const Identifier& identifier, unsigned flags, NameSet* bound, NameSet& local, NameSet& free, NameSet& global, NameSet& typeParameters, Block* classBlock)
    {
        UniquedStringImpl* name = identifier.impl();
        if (flags & DefGlobal) {
            if (flags & DefNonlocal)
                return failAtDirective(block, identifier, concatenate("name '"_s, identifier.string(), "' is nonlocal and global"_s));
            scopes.set(name, NameScope::GlobalExplicit);
            global.add(name);
            if (bound)
                bound->remove(name);
            return true;
        }
        if (flags & DefNonlocal) {
            if (!bound)
                return failAtDirective(block, identifier, "nonlocal declaration not allowed at module level"_s);
            if (!bound->contains(name) && m_fragmentIs != FragmentIs::WhatHasWhatIsCompiled)
                return failAtDirective(block, identifier, concatenate("no binding for nonlocal '"_s, identifier.string(), "' found"_s));
            if (typeParameters.contains(name))
                return failAtDirective(block, identifier, concatenate("nonlocal binding not allowed for type parameter '"_s, identifier.string(), '\''));
            scopes.set(name, NameScope::Free);
            free.add(name);
            return true;
        }
        if (flags & DefBound) {
            scopes.set(name, NameScope::Local);
            local.add(name);
            global.remove(name);
            if (flags & DefTypeParameter)
                typeParameters.add(name);
            else
                typeParameters.remove(name);
            return true;
        }
        // What can see into a class looks there and then in the module, and not in a function around the class that has the name too.
        if (classBlock) {
            unsigned classFlags = classBlock->flagsOf(identifier);
            if (classFlags & DefGlobal) {
                scopes.set(name, NameScope::GlobalExplicit);
                return true;
            }
            if ((classFlags & DefBound) && !(classFlags & DefNonlocal)) {
                scopes.set(name, NameScope::GlobalImplicit);
                return true;
            }
        }
        if (bound && bound->contains(name)) {
            scopes.set(name, NameScope::Free);
            free.add(name);
            return true;
        }
        scopes.set(name, NameScope::GlobalImplicit);
        return true;
    }

    static bool isFreeInAnyChild(Block& block, const Identifier& name)
    {
        for (Block* child : block.children) {
            if (child->scopeOf(name) == NameScope::Free)
                return true;
        }
        return false;
    }

    // A comprehension other than a generator expression is not a function of its own: its names become names of the block it is in.
    void inlineComprehension(Block& block, Block& comprehension, Scopes& scopes, NameSet& comprehensionFree, NameSet& inlinedCells)
    {
        bool removeClass = false;
        bool removeClassDict = false;
        bool removeConditionalAnnotations = false;
        for (Symbol& symbol : comprehension.symbols) {
            if (symbol.flags & DefParameter)
                continue;
            const Identifier& identifier = *symbol.name;
            UniquedStringImpl* name = identifier.impl();
            NameScope scope = symbol.scope;
            if (scope == NameScope::Cell || (symbol.flags & DefComprehensionCell))
                inlinedCells.add(name);
            // These do not go through a class unless something further in needs them.
            bool isClassCell = name == m_class.impl() || name == m_classdict.impl() || name == m_conditionalAnnotations.impl();
            if (scope == NameScope::Free && block.type == BlockType::Class && isClassCell) {
                scope = NameScope::GlobalImplicit;
                if (!isFreeInAnyChild(comprehension, identifier))
                    comprehensionFree.remove(name);
                if (name == m_class.impl())
                    removeClass = true;
                else if (name == m_conditionalAnnotations.impl())
                    removeConditionalAnnotations = true;
                else
                    removeClassDict = true;
            }
            Symbol* existing = block.find(identifier);
            if (!existing) {
                block.add(identifier).flags = symbol.flags;
                scopes.set(name, scope);
                continue;
            }
            // What was free in the comprehension and is local to the block is now simply local.
            if ((existing->flags & DefBound) && block.type != BlockType::Class && !isFreeInAnyChild(comprehension, identifier))
                comprehensionFree.remove(name);
        }
        if (removeClass)
            comprehension.remove(m_class);
        if (removeClassDict)
            comprehension.remove(m_classdict);
        if (removeConditionalAnnotations)
            comprehension.remove(m_conditionalAnnotations);
    }

    // What is local here and free further in is a cell here.
    static void analyzeCells(Scopes& scopes, NameSet& free, const NameSet& inlinedCells)
    {
        for (auto& entry : scopes) {
            if (entry.value != NameScope::Local)
                continue;
            if (!free.contains(entry.key) && !inlinedCells.contains(entry.key))
                continue;
            entry.value = NameScope::Cell;
            free.remove(entry.key);
        }
    }

    void dropClassFree(Block& block, NameSet& free)
    {
        if (free.remove(m_class.impl()))
            block.needsClassClosure = true;
        if (free.remove(m_classdict.impl()))
            block.needsClassDict = true;
        if (free.remove(m_conditionalAnnotations.impl()))
            block.hasConditionalAnnotations = true;
    }

    void updateSymbols(Block& block, const Scopes& scopes, NameSet* bound, const NameSet& free, const NameSet& inlinedCells, bool isClass)
    {
        for (Symbol& symbol : block.symbols) {
            if (inlinedCells.contains(symbol.name->impl()))
                symbol.flags |= DefComprehensionCell;
            symbol.scope = scopes.get(symbol.name->impl());
        }
        // What is free further in and not bound here is free here too, on its way out.
        for (UniquedStringImpl* name : free) {
            auto iterator = block.index.find(name);
            if (iterator != block.index.end()) {
                // A method's free variable, of the same name as something of the class's own.
                if (isClass)
                    block.symbols[iterator->value].flags |= DefFreeClass;
                continue;
            }
            if (bound && !bound->contains(name))
                continue;
            const Identifier& identifier = name->is8Bit() ? m_arena.identifiers().makeIdentifier(m_vm, name->span8()) : m_arena.identifiers().makeIdentifier(m_vm, name->span16());
            block.add(identifier).scope = NameScope::Free;
        }
    }

    bool analyzeBlock(Block& block, NameSet* bound, NameSet& free, NameSet& global, NameSet& typeParameters, Block* classBlock)
    {
        if (!isSafeToRecurse(block.location))
            return false;

        NameSet local;
        Scopes scopes;
        NameSet newGlobal;
        NameSet newFree;
        NameSet newBound;
        NameSet inlinedCells;

        // What a class binds is not visible in its methods, so what they see is settled before its own names are looked at.
        if (block.type == BlockType::Class) {
            unite(newGlobal, global);
            if (bound)
                unite(newBound, *bound);
        }

        for (Symbol& symbol : block.symbols) {
            if (!analyzeName(block, scopes, *symbol.name, symbol.flags, bound, local, free, global, typeParameters, classBlock))
                return false;
        }

        if (block.type != BlockType::Class) {
            if (block.isFunctionLike())
                unite(newBound, local);
            if (bound)
                unite(newBound, *bound);
            unite(newGlobal, global);
        } else {
            newBound.add(m_class.impl());
            newBound.add(m_classdict.impl());
            newBound.add(m_conditionalAnnotations.impl());
        }

        for (Block* child : block.children) {
            Block* childClassBlock = nullptr;
            if (child->canSeeClassScope)
                childClassBlock = block.type == BlockType::Class ? &block : classBlock;

            bool inlines = child->comprehension != ComprehensionType::None && !child->isGenerator && !block.canSeeClassScope;

            // A child sees what is bound around it, and what it does to these does not come back.
            NameSet childBound = newBound;
            NameSet childFree = newFree;
            NameSet childGlobal = newGlobal;
            NameSet childTypeParameters = typeParameters;
            if (!analyzeBlock(*child, &childBound, childFree, childGlobal, childTypeParameters, childClassBlock))
                return false;
            if (inlines) {
                inlineComprehension(block, *child, scopes, childFree, inlinedCells);
                child->isInlinedComprehension = true;
            }
            unite(newFree, childFree);
        }

        // The children of what has become part of this block are this block's.
        for (unsigned i = block.children.size(); i--;) {
            Block* child = block.children[i];
            if (!child->isInlinedComprehension)
                continue;
            block.children.removeAt(i);
            block.children.insertVector(i, child->children);
        }

        if (block.isFunctionLike())
            analyzeCells(scopes, newFree, inlinedCells);
        else if (block.type == BlockType::Class)
            dropClassFree(block, newFree);

        updateSymbols(block, scopes, bound, newFree, inlinedCells, block.type == BlockType::Class || block.canSeeClassScope);
        unite(free, newFree);
        return true;
    }

    VM& m_vm;
    Arena& m_arena;
    SymbolTable& m_table;
    SyntaxError& m_error;

    Block* m_current { nullptr };
    Vector<Block*, 16> m_stack;
    Block* m_fragmentTop { nullptr };
    const Vector<Identifier>* m_fragmentFreeVariables { nullptr };
    const Identifier* m_private { nullptr }; // The class that names are mangled for.
    Statement* m_lastFutureStatement { nullptr };

    const Identifier& m_top;
    const Identifier& m_lambda;
    const Identifier& m_genexpr;
    const Identifier& m_listcomp;
    const Identifier& m_setcomp;
    const Identifier& m_dictcomp;
    const Identifier& m_annotate;
    const Identifier& m_class;
    const Identifier& m_classdict;
    const Identifier& m_conditionalAnnotations;
    FragmentIs m_fragmentIs { FragmentIs::WhatIsCompiled };
    const Identifier& m_typeParamsAttribute;
    const Identifier& m_debug;
    const Identifier& m_super;
    const Identifier& m_format;
    const Identifier& m_typeParams;
    const Identifier& m_genericBase;
    const Identifier& m_defaults;
    const Identifier& m_kwdefaults;
    const Identifier& m_firstImplicitArgument;
};

std::unique_ptr<SymbolTable> SymbolTable::build(VM& vm, Arena& arena, Module& module, unsigned futureFeatures, SyntaxError& error)
{
    std::unique_ptr<SymbolTable> table { new SymbolTable };
    table->m_futureFeatures = futureFeatures;
    if (!SymbolTableBuilder(vm, arena, *table, error).build(module))
        return nullptr;
    return table;
}

std::optional<unsigned> SymbolTable::futureFeaturesOf(VM& vm, Arena& arena, Module& module, SyntaxError& error)
{
    SymbolTable table;
    if (!SymbolTableBuilder(vm, arena, table, error).findFutureStatements(module))
        return std::nullopt;
    return table.m_futureFeatures;
}

std::unique_ptr<SymbolTable> SymbolTable::buildFragment(VM& vm, Arena& arena, Statement* statement, Expression* expression, const Vector<Identifier>& freeVariables, const Identifier* privateName, unsigned futureFeatures, bool canSeeClassScope, bool isNested, FragmentIs fragmentIs)
{
    std::unique_ptr<SymbolTable> table { new SymbolTable };
    table->m_futureFeatures = futureFeatures;
    SyntaxError error;
    if (!SymbolTableBuilder(vm, arena, *table, error).buildFragment(statement, expression, freeVariables, privateName, canSeeClassScope, isNested, fragmentIs))
        return nullptr;
    return table;
}

static void dumpName(StringBuilder& out, const Identifier& name)
{
    out.append('"');
    for (char16_t c : StringView(name.string()).codeUnits()) {
        if (c >= ' ' && c <= '~')
            out.append(static_cast<Latin1Character>(c));
        else
            out.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
    }
    out.append('"');
}

static void dumpBlock(StringBuilder& out, Block& block)
{
    static constexpr ASCIILiteral types[] = { "function"_s, "class"_s, "module"_s, "annotation"_s, "type alias"_s, "type parameters"_s, "type variable"_s };
    out.append("{\"type\":\""_s, types[static_cast<unsigned>(block.type)], "\",\"name\":"_s);
    dumpName(out, *block.name);
    out.append(",\"line\":"_s, block.location.line, ",\"nested\":"_s, block.isNested ? '1' : '0', ",\"symbols\":{"_s);
    Vector<Symbol*> sorted;
    for (Symbol& symbol : block.symbols)
        sorted.append(&symbol);
    std::ranges::sort(sorted, [] (Symbol* a, Symbol* b) {
        return codePointCompareLessThan(a->name->string(), b->name->string());
    });
    bool isFirst = true;
    for (Symbol* symbol : sorted) {
        if (!isFirst)
            out.append(',');
        isFirst = false;
        dumpName(out, *symbol->name);
        out.append(':', symbol->flags | (static_cast<unsigned>(symbol->scope) << 12));
    }
    out.append("},\"varnames\":["_s);
    isFirst = true;
    for (const Identifier* name : block.parameters) {
        if (!isFirst)
            out.append(',');
        isFirst = false;
        dumpName(out, *name);
    }
    out.append("],\"children\":["_s);
    isFirst = true;
    for (Block* child : block.children) {
        if (!isFirst)
            out.append(',');
        isFirst = false;
        dumpBlock(out, *child);
    }
    out.append("]}"_s);
}

String dumpSymbolTable(SymbolTable& table)
{
    StringBuilder out;
    dumpBlock(out, table.top());
    return out.toString();
}

} } // namespace JSC::Python
