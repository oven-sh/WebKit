/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCompiler.h"

#include "JSCInlines.h"
#include "PyCodeNode.h"
#include "PyCompilerUnit.h"
#include "PyRuntime.h"
#include "PyTuple.h"
#include "PyType.h"
#include "UnlinkedFunctionCodeBlock.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeInfo);

// ---- Compiler ----

Compiler::Compiler(JSGlobalObject* globalObject, ParseTree& tree, ParserHost& host, const SourceCode& source, const String& filename, const CompilerOptions& options)
    : m_globalObject(globalObject)
    , m_vm(globalObject->vm())
    , m_runtime(Runtime::ensure(globalObject))
    , m_tree(tree)
    , m_host(host)
    , m_source(source)
    , m_filename(filename)
    , m_options(options)
{
}

Compiler::~Compiler() = default;

void Compiler::fail(const Range& range, const String& message)
{
    if (m_error)
        return;
    m_error.type = SyntaxErrorInfo::Type::SyntaxError;
    m_error.message = message;
    m_error.hasLocation = true;
    m_error.lineno = range.lineno;
    m_error.offset = range.colOffset + 1;
    m_error.hasEnd = true;
    m_error.endLineno = range.endLineno;
    m_error.endOffset = range.endColOffset + 1;
}

void Compiler::failWithoutLocation(SyntaxErrorInfo::Type type, const String& message)
{
    if (m_error)
        return;
    m_error.type = type;
    m_error.message = message;
}

void Compiler::warn(const Range& range, const String& message)
{
    if (m_error)
        return;
    ParserHost::Warning warning { ParserHost::WarningCategory::SyntaxWarning, message, range.lineno, range.colOffset, range.endLineno, range.endColOffset };
    switch (m_host.warn(warning)) {
    case ParserHost::WarningResult::Reported:
        return;
    case ParserHost::WarningResult::BecameException:
        fail(range, message);
        return;
    case ParserHost::WarningResult::OtherException:
        failWithoutLocation(SyntaxErrorInfo::Type::HostException, { });
        return;
    }
}

unsigned Compiler::offsetOf(int line, int column) const
{
    unsigned end = m_source.endOffset();
    if (line < 1 || column < 0)
        return 0;
    if (static_cast<size_t>(line) > m_tree.lineStarts.size())
        return end;
    unsigned lineEnd = static_cast<size_t>(line) < m_tree.lineStarts.size() ? m_tree.lineStarts[line] - 1 : end;
    return std::min<uint64_t>(static_cast<uint64_t>(m_tree.lineStarts[line - 1]) + column, lineEnd);
}

SourceCode Compiler::sourceFor(const Range& range) const
{
    if (m_tree.positionsAreArbitrary)
        return m_source;
    unsigned start = offsetOf(range.lineno, range.colOffset);
    unsigned end = std::max(start, offsetOf(range.endLineno, range.endColOffset));
    return SourceCode(RefPtr<SourceProvider> { m_source.provider() }, start, end);
}

const Identifier& Compiler::identifier(ASCIILiteral text)
{
    return m_tree.identifiers.makeIdentifier(m_vm, text.span8());
}

AST::Name Compiler::name(const String& text)
{
    if (text.is8Bit())
        return &m_tree.identifiers.makeIdentifier(m_vm, text.span8());
    return &m_tree.identifiers.makeIdentifier(m_vm, text.span16());
}

UnlinkedFunctionExecutable* Compiler::compile()
{
    DeferGC deferGC(m_vm);

    m_future.annotations = m_options.futureAnnotations;
    m_future.barryAsBDFL = m_options.futureBarryAsBDFL;
    m_future.allowTopLevelAwait = m_options.allowTopLevelAwait;
    if (!analyzeFutureStatements(m_tree.root, m_future, m_error))
        return nullptr;

    m_symbols = makeUnique<SymbolTable>(m_vm, m_tree, m_future);
    if (!m_symbols->build()) {
        m_error = m_symbols->error();
        return nullptr;
    }

    m_isInteractive = m_tree.root->kind == ModKind::Interactive;

    CodeSpec spec { UnitKind::Module, m_symbols->top(), name("<module>"_s), { }, 1 };
    auto* executable = compileCode(nullptr, spec, m_source, [&](Unit& unit) {
        unit.compileModule(m_tree.root);
    });
    return failed() ? nullptr : executable;
}

UnlinkedFunctionExecutable* Compiler::generate(const SourceCode& parentSource, const SourceCode& source, SourceParseMode parseMode, Ref<CodeInfo>&& info, unsigned parameterCount, bool isGeneratorBody, Function<void(BytecodeGenerator&)>&& emitter)
{
    ExecutableInfo executableInfo(false, PrivateBrandRequirement::None, false, ConstructorKind::None, JSParserScriptMode::Classic, SuperBinding::NotNeeded, parseMode, DerivedContextType::None, NeedsClassFieldInitializer::No, false, false, EvalContextType::None);
    auto* codeBlock = UnlinkedFunctionCodeBlock::create(m_vm, FunctionCode, executableInfo, { });
    ParserArena arena;
    PythonCodeNode node(arena, source, parameterCount, isGeneratorBody, WTF::move(emitter));
    ParserError error = BytecodeGenerator::generatePython(m_vm, &node, codeBlock);
    if (error.isValid())
        failWithoutLocation(SyntaxErrorInfo::Type::MemoryError, "the code is too large to compile"_s);
    return UnlinkedFunctionExecutable::createForPython(m_vm, parentSource, source, parseMode, WTF::move(info), parameterCount, codeBlock);
}

static String qualifiedName(Unit* parent, const Compiler::CodeSpec& spec, SymbolTable& symbols)
{
    using UnitKind = Compiler::UnitKind;
    const String& name = spec.name->string();
    if (spec.kind == UnitKind::Module)
        return name;

    String base;
    bool hasBase = false;
    Name functionName = spec.scope->functionName;
    if (parent->parent()) {
        Unit* enclosing = parent;
        if (enclosing->kind() == UnitKind::Annotations) {
            // What encloses the code is there only for the type parameters or the annotations.
            enclosing = enclosing->parent();
            if (!enclosing->parent())
                return name;
        }
        bool isGlobal = false;
        if (spec.kind == UnitKind::Function || spec.kind == UnitKind::AsyncFunction || spec.kind == UnitKind::Class) {
            Name mangled = symbols.mangle(enclosing->privateName(), enclosing->scope(), spec.name);
            isGlobal = enclosing->scope()->bindingOf(mangled) == Binding::GlobalExplicit;
        }
        if (!isGlobal) {
            hasBase = true;
            if (enclosing->kind() == UnitKind::Function || enclosing->kind() == UnitKind::AsyncFunction || enclosing->kind() == UnitKind::Lambda)
                base = makeString(enclosing->qualname(), ".<locals>"_s);
            else
                base = enclosing->qualname();
        }
        if (functionName) {
            base = hasBase ? makeString(base, '.', functionName->string()) : functionName->string();
            hasBase = true;
        }
    } else if (functionName) {
        base = functionName->string();
        hasBase = true;
    }
    return hasBase ? makeString(base, '.', name) : name;
}

UnlinkedFunctionExecutable* Compiler::compileCode(Unit* parent, const CodeSpec& spec, const SourceCode& parentSource, const Function<void(Unit&)>& emitBody)
{
    Scope* scope = spec.scope;
    Ref<CodeInfo> info = CodeInfo::create();
    info->name = *spec.name;
    info->qualname = qualifiedName(parent, spec, *m_symbols);
    info->filename = m_filename;
    info->firstLineNumber = spec.firstLineNumber;
    info->docstring = spec.docstring;
    info->takesNamespace = !scope->isFunctionLike();

    if (scope->isFunctionLike()) {
        info->flags.add({ CodeFlag::NewLocals, CodeFlag::Optimized });
        if (scope->isNested)
            info->flags.add(CodeFlag::Nested);
        if (scope->isGenerator)
            info->flags.add(scope->isCoroutine ? CodeFlag::AsyncGenerator : CodeFlag::Generator);
        else if (scope->isCoroutine)
            info->flags.add(CodeFlag::Coroutine);
        if (scope->hasVarargs)
            info->flags.add(CodeFlag::VarArgs);
        if (scope->hasVarKeywords)
            info->flags.add(CodeFlag::VarKeywords);
        if (!spec.docstring.isNull())
            info->flags.add(CodeFlag::HasDocstring);
        if (scope->isMethod)
            info->flags.add(CodeFlag::Method);
    } else if (scope->isCoroutine)
        info->flags.add(CodeFlag::Coroutine);
    if (m_future.annotations)
        info->flags.add(CodeFlag::FutureAnnotations);
    if (m_future.barryAsBDFL)
        info->flags.add(CodeFlag::FutureBarryAsBDFL);

    if (spec.arguments) {
        info->posOnlyCount = spec.arguments->posonlyargs.size();
        info->argCount = info->posOnlyCount + spec.arguments->args.size();
        info->kwOnlyCount = spec.arguments->kwonlyargs.size();
    } else if (scope->isFunctionLike()) {
        info->argCount = scope->parameters.size();
        info->posOnlyCount = spec.positionalOnlyCount;
    }
    for (Name parameter : scope->parameters)
        info->parameterNames.append(*parameter);
    ASSERT(info->takesNamespace || info->parameterNames.size() == info->parameterCount());

    SourceCode source = spec.kind == UnitKind::Module ? m_source : sourceFor(spec.range);
    unsigned parameterCount = info->takesNamespace ? 1 : info->parameterCount();

    if (!scope->isGenerator && !scope->isCoroutine) {
        return generate(parentSource, source, SourceParseMode::MethodMode, info.copyRef(), parameterCount, false, [&](BytecodeGenerator& generator) {
            Unit unit(*this, parent, spec, generator, info.get(), source, Unit::Part::Whole);
            unit.emitPrologue();
            emitBody(unit);
            unit.finishConstants();
        });
    }

    return generate(parentSource, source, SourceParseMode::MethodMode, info.copyRef(), parameterCount, false, [&](BytecodeGenerator& generator) {
        Unit binder(*this, parent, spec, generator, info.get(), source, Unit::Part::GeneratorBinder);
        binder.emitPrologue();
        auto* body = generate(source, source, SourceParseMode::GeneratorBodyMode, info.copyRef(), static_cast<unsigned>(JSGenerator::Argument::Frame), true, [&](BytecodeGenerator& bodyGenerator) {
            Unit unit(*this, parent, spec, bodyGenerator, info.get(), source, Unit::Part::GeneratorBody);
            unit.emitPrologue();
            emitBody(unit);
            unit.finishConstants();
        });
        unsigned index = generator.addPythonFunction(body);
        ASSERT_UNUSED(index, index == CodeInfo::generatorBodyIndex);
        binder.emitMakeGenerator();
    });
}

// ---- Unit ----

Unit::Unit(Compiler& compiler, Unit* parent, const CodeSpec& spec, BytecodeGenerator& generator, CodeInfo& info, const SourceCode& source, Part part)
    : c(compiler)
    , m_parent(parent)
    , g(generator)
    , m_info(info)
    , m_source(source)
    , m_scope(spec.scope)
    , m_kind(spec.kind)
    , m_part(part)
    , m_private(spec.privateName ? spec.privateName : parent ? parent->m_private : nullptr)
    , m_arguments(spec.arguments)
    , m_iterateFirstParameter(spec.iterateFirstParameter)
    , m_copyLocals(spec.scope->hasNamedExpression)
{
}

Unit::Arguments::Arguments(Unit& unit, unsigned count)
{
    m_registers.reserveInitialCapacity(count);
    for (unsigned i = 0; i < count; ++i) {
        m_registers.append(unit.g.newTemporary());
        ASSERT(!i || m_registers[i]->index() == m_registers[i - 1]->index() - 1);
    }
}

bool Unit::isSafeToRecurse()
{
    if (vm().isSafeToRecurse()) [[likely]]
        return true;
    c.failWithoutLocation(SyntaxErrorInfo::Type::RecursionError, "maximum recursion depth exceeded during compilation"_s);
    return false;
}

void Unit::line(int lineno)
{
    if (lineno <= 0 || m_sameLineDepth)
        return;
    // What follows a label may be reached from another line than the one before it.
    size_t position = g.instructions().size();
    if (lineno == m_markedLine && (position == m_markedPosition || g.lastOpcodeID() != op_debug))
        return;
    m_markedLine = lineno;
    m_markedPosition = position;
    g.addPythonMark(PythonMark::Line, lineMarkValue(lineno, position == m_statementStart));
}

void Unit::silentLine(int lineno)
{
    if (m_sameLineDepth)
        return;
    m_markedLine = lineno;
    m_markedPosition = g.instructions().size();
    g.addPythonMark(PythonMark::Line, lineMarkValue(lineno, false, true));
}

void Unit::noLine()
{
    m_markedLine = -1;
    g.addPythonMark(PythonMark::NoLine, 0);
}

void Unit::hiddenJump(Label& target)
{
    g.addPythonMark(PythonMark::NoLine, hiddenJumpMark);
    g.emitJump(target);
}

void Unit::loc(const Range& range)
{
    line(range.lineno);
    unsigned sourceStart = m_source.startOffset();
    unsigned start = std::max(c.offsetOf(range.lineno, range.colOffset), sourceStart);
    unsigned end = std::max(c.offsetOf(range.endLineno, range.endColOffset), start);
    g.emitExpressionInfo(JSTextPosition(start), JSTextPosition(start), JSTextPosition(end));
}

// A name the code looks up somewhere other than among its own variables: one of co_names.
RegisterID* Unit::usedName(Name name)
{
    if (!m_info.names.contains(*name))
        m_info.names.append(*name);
    return nameConstant(name);
}

// co_consts has the constants in the order in which the code first needs them. CPython then works out what it can from them,
// and what that gives comes after all the others.
void Unit::noteConstant(JSValue value, bool isUsed)
{
    VirtualRegister reg = constant(value)->virtualRegister();
    for (auto& known : m_info.constants) {
        if (known.reg == reg) {
            known.isUsed |= isUsed;
            return;
        }
    }
    m_info.constants.append({ reg, 0, isUsed });
}

void Unit::noteFoldedConstant(Expr* node, JSValue value)
{
    if (node->is<Constant>()) {
        noteConstant(value);
        return;
    }
    // What it was made from is not needed any more, and only counts if it is the first constant of all, which stays.
    while (m_info.constants.isEmpty()) {
        if (auto* literal = node->tryAs<Constant>())
            noteConstant(constantValue(literal->value), false);
        else if (auto* unary = node->tryAs<UnaryOp>())
            node = unary->operand;
        else if (auto* binary = node->tryAs<BinOp>())
            node = binary->left;
        else if (auto* tuple = node->tryAs<Tuple>(); tuple && !tuple->elts.isEmpty())
            node = tuple->elts[0];
        else
            break;
    }
    m_foldedConstants.append({ constant(value)->virtualRegister(), 0, true });
}

void Unit::noteCode(unsigned functionIndex)
{
    m_info.constants.append({ VirtualRegister(), functionIndex, true });
}

void Unit::finishConstants()
{
    for (auto& folded : m_foldedConstants) {
        bool isKnown = false;
        for (auto& known : m_info.constants) {
            if (known.reg == folded.reg) {
                known.isUsed = true;
                isKnown = true;
            }
        }
        if (!isKnown)
            m_info.constants.append(folded);
    }
    m_foldedConstants.clear();
}

RegisterID* Unit::nameConstant(Name name)
{
    return g.pythonConstant(g.addStringConstant(*name));
}

RegisterID* Unit::stringConstant(const String& string)
{
    return nameConstant(c.name(string));
}

RegisterID* Unit::load(RegisterID* dst, JSValue value)
{
    RegisterID* source = constant(value);
    return dst ? g.move(dst, source) : source;
}

RegisterID* Unit::callRuntime(RegisterID* dst, RuntimeFunction function, Arguments& arguments)
{
    return g.emitPyRuntime(dst, function, arguments.first(), arguments.size());
}

RegisterID* Unit::callRuntime(RegisterID* dst, RuntimeFunction function)
{
    return g.emitPyRuntime(dst, function, nullptr, 0);
}

RegisterID* Unit::callRuntime(RegisterID* dst, RuntimeFunction function, std::initializer_list<RegisterID*> operands)
{
    // One operand is a run of registers wherever it is, unless it is a constant.
    if (operands.size() == 1 && !(*operands.begin())->virtualRegister().isConstant())
        return g.emitPyRuntime(dst, function, *operands.begin(), 1);
    Vector<Reg, 4> held;
    for (RegisterID* operand : operands)
        held.append(operand);
    Arguments arguments(*this, operands.size());
    unsigned i = 0;
    for (RegisterID* operand : operands)
        g.move(arguments[i++], operand);
    return callRuntime(dst, function, arguments);
}

// ---- Variables ----

AST::Name Unit::mangle(Name name)
{
    return c.m_symbols->mangle(m_private, m_overlays.isEmpty() ? m_scope : m_overlays.last(), name);
}

void Unit::recordVariable(Name name, const Variable& variable, bool isHidden)
{
    CodeInfo::VariableKind kind = variable.kind == Variable::Kind::Fast ? CodeInfo::VariableKind::Local : variable.kind == Variable::Kind::Cell ? CodeInfo::VariableKind::Cell : CodeInfo::VariableKind::Free;
    m_info.variables.append({ *name, variable.reg->virtualRegister(), kind, isHidden });
}

Vector<AST::Name> Unit::freeVariablesOf(Scope* scope)
{
    Vector<Name> result;
    for (auto& symbol : scope->symbols) {
        if (symbol.binding == Binding::Free || (symbol.flags & DefFreeClass))
            result.append(symbol.name);
    }
    if (scope->readsConditionalAnnotationsCell && !result.contains(names().conditionalAnnotations))
        result.append(names().conditionalAnnotations);
    std::ranges::sort(result, [](Name a, Name b) { return codePointCompareLessThan(a->string(), b->string()); });
    return result;
}

// The variables of an inlined comprehension are variables of the code it is in, which nothing else can name.
void Unit::trackWhetherBound(Variable& variable)
{
    if (variable.kind == Variable::Kind::Fast && m_boundBitCount < 64)
        variable.boundBit = m_boundBitCount++;
}

void Unit::declareComprehensionVariables(Scope* scope)
{
    for (Scope* child : scope->children) {
        if (!child->isInlinedComprehension)
            continue;
        auto map = makeUnique<VariableMap>();
        for (auto& symbol : child->symbols) {
            if (symbol.binding != Binding::Local && symbol.binding != Binding::Cell)
                continue;
            // The first iterable is not passed in.
            if (symbol.name->impl() == names().dotZero->impl())
                continue;
            Variable variable { g.addVar(), symbol.binding == Binding::Cell ? Variable::Kind::Cell : Variable::Kind::Fast, true };
            // It loses its value where the comprehension ends, and gets none outside it.
            trackWhetherBound(variable);
            map->add(symbol.name->impl(), variable);
            recordVariable(symbol.name, variable, true);
        }
        m_variables.add(child, WTF::move(map));
        declareComprehensionVariables(child);
    }
}

void Unit::declareVariables()
{
    auto map = makeUnique<VariableMap>();
    bool parametersAreLocals = m_part == Part::GeneratorBody;

    if (!isFunctionLike()) {
        m_namespace = parametersAreLocals ? g.addVar() : g.pythonParameter(0);
        m_info.namespaceRegister = m_namespace->virtualRegister();
        auto addCell = [&](Name name) {
            Variable variable { g.addVar(), Variable::Kind::Cell, true };
            map->add(name->impl(), variable);
            recordVariable(name, variable, false);
        };
        if (m_scope->needsClassClosure)
            addCell(names().classCell);
        if (m_scope->needsClassDict)
            addCell(names().classDict);
        if (m_scope->hasConditionalAnnotations && m_kind == UnitKind::Class)
            addCell(names().conditionalAnnotations);
    } else {
        // The parameters come first, in order: the body of a generator unpacks them from a tuple.
        Vector<RegisterID*, 8> parameterRegisters;
        for (unsigned i = 0; i < m_scope->parameters.size(); ++i)
            parameterRegisters.append(parametersAreLocals ? g.addVar() : g.pythonParameter(i));
        if (parametersAreLocals && m_iterateFirstParameter != CodeSpec::IterateFirstParameter::No)
            m_firstIterator = g.addVar();
        for (unsigned i = 0; i < m_scope->parameters.size(); ++i) {
            Name name = m_scope->parameters[i];
            Symbol* symbol = m_scope->find(name);
            if (symbol->binding == Binding::Cell) {
                m_cellParameters.append({ parameterRegisters[i], name });
                continue;
            }
            Variable variable { parameterRegisters[i], Variable::Kind::Fast, !!(symbol->flags & Deleted) };
            map->add(name->impl(), variable);
            recordVariable(name, variable, false);
        }
        for (auto& symbol : m_scope->symbols) {
            if (symbol.binding == Binding::Local) {
                if (symbol.flags & DefParam)
                    continue;
                Variable variable { g.addVar(), Variable::Kind::Fast, true };
                if (!(symbol.flags & Deleted))
                    trackWhetherBound(variable);
                map->add(symbol.name->impl(), variable);
                recordVariable(symbol.name, variable, false);
            } else if (symbol.binding == Binding::Cell) {
                Variable variable { g.addVar(), Variable::Kind::Cell, true };
                map->add(symbol.name->impl(), variable);
                recordVariable(symbol.name, variable, false);
            }
        }
    }

    for (Name name : freeVariablesOf(m_scope)) {
        Variable variable { g.addVar(), Variable::Kind::Free, true };
        map->add(name->impl(), variable);
        recordVariable(name, variable, false);
        m_info.freeVariables.append(*name);
    }

    m_variables.add(m_scope, WTF::move(map));
    declareComprehensionVariables(m_scope);
}

void Unit::emitPrologue()
{
    // Every frame begins with this, which also counts it, but for the body of a generator.
    unsigned simpleArgumentCountIncludingThis = !m_info.takesNamespace && m_info.parameterCount() == m_info.argCount ? m_info.argCount + 1 : 0;
    if (m_part == Part::GeneratorBinder) {
        g.emitPyBindArgs(simpleArgumentCountIncludingThis);
        return;
    }

    m_info.variables.clear();
    m_info.freeVariables.clear();
    declareVariables();
    if (isFunctionLike() && !m_info.docstring.isNull())
        noteConstant(g.addStringConstant(*c.name(m_info.docstring)));

    if (m_part == Part::Whole)
        g.emitPyBindArgs(simpleArgumentCountIncludingThis);
    else {
        // The generator was given the values of the parameters when it was made.
        unsigned count = isFunctionLike() ? m_scope->parameters.size() + !!m_firstIterator : 1;
        if (count) {
            // The first variable declared is the first parameter.
            RegisterID* first = isFunctionLike() ? g.pythonRegister(virtualRegisterForLocal(g.firstPythonVariable())) : m_namespace;
            Reg arguments = newTemporary();
            g.emitGetInternalField(arguments.get(), g.generatorRegister(), generatorArgumentsField);
            g.emitPyUnpack(first, arguments.get(), count, -1);
            g.emitPutInternalField(g.generatorRegister(), generatorArgumentsField, constant(jsUndefined()));
        }
    }

    // Nothing has a value yet.
    unsigned closureIndex = 0;
    for (auto& variable : m_info.variables) {
        RegisterID* reg = g.pythonRegister(variable.reg);
        switch (variable.kind) {
        case CodeInfo::VariableKind::Local:
            if (variable.reg.isLocal() && !isParameterRegister(reg))
                g.move(reg, emptyValue());
            break;
        case CodeInfo::VariableKind::Cell:
            if (variable.isHidden)
                g.move(reg, emptyValue()); // Made each time the comprehension is evaluated.
            else
                callRuntime(reg, RuntimeFunction::MakeCell, { emptyValue() });
            break;
        case CodeInfo::VariableKind::Free:
            callRuntime(reg, RuntimeFunction::LoadClosure, { constant(jsNumber(closureIndex++)) });
            break;
        }
    }
    for (auto& parameter : m_cellParameters) {
        Variable& variable = m_variables.get(m_scope)->find(parameter.name->impl())->value;
        g.emitPyStoreCell(variable.reg, parameter.reg);
    }

    // From here on their order is what locals(), f_locals and co_cellvars show. In CPython that is: the parameters; the other
    // variables that no inner function uses; those that one does, by name; those of outer functions, by name.
    auto rank = [&](const CodeInfo::Variable& variable) -> size_t {
        if (variable.isHidden)
            return std::numeric_limits<size_t>::max();
        size_t parameter = m_info.parameterNames.find(variable.name);
        if (parameter != notFound)
            return parameter;
        return m_info.parameterNames.size() + static_cast<size_t>(variable.kind);
    };
    std::stable_sort(m_info.variables.begin(), m_info.variables.end(), [&](auto& a, auto& b) {
        size_t rankOfA = rank(a);
        size_t rankOfB = rank(b);
        if (rankOfA != rankOfB)
            return rankOfA < rankOfB;
        return !a.isHidden && a.kind != CodeInfo::VariableKind::Local && codePointCompareLessThan(a.name.string(), b.name.string());
    });

    g.addPythonMark(PythonMark::Start, 0);
    m_markedLine = -1;
}

bool Unit::isParameterRegister(RegisterID* reg)
{
    if (m_part != Part::GeneratorBody || !isFunctionLike())
        return false;
    int local = reg->virtualRegister().toLocal();
    int first = g.firstPythonVariable();
    return local >= first && local < first + static_cast<int>(m_scope->parameters.size());
}

void Unit::emitMakeGenerator()
{
    ASSERT(m_part == Part::GeneratorBinder);
    Reg result = newTemporary();
    // A generator expression asks for the iterator of its first iterable when it is made, not when it is first resumed.
    // The parameter goes on being the iterable.
    if (m_iterateFirstParameter != CodeSpec::IterateFirstParameter::No) {
        RegisterID* parameter = g.pythonParameter(0);
        Reg iterator = newTemporary();
        loc(m_scope->range);
        if (m_iterateFirstParameter == CodeSpec::IterateFirstParameter::Async)
            callRuntime(iterator.get(), RuntimeFunction::GetAIter, { parameter });
        else
            g.emitPyGetIter(iterator.get(), parameter);
        callRuntime(result.get(), RuntimeFunction::MakeGenerator, { iterator.get() });
    } else
        callRuntime(result.get(), RuntimeFunction::MakeGenerator);
    g.emitReturn(result.get());
}

void Unit::emitReturnNone()
{
    noteConstant(jsUndefined());
    g.emitReturn(constant(jsUndefined()));
}

Unit::Access Unit::resolve(Name mangled)
{
    auto* impl = mangled->impl();
    auto variableIn = [&](Scope* scope) -> Variable* {
        auto* map = m_variables.get(scope);
        auto it = map->find(impl);
        return it == map->end() ? nullptr : &it->value;
    };

    for (size_t i = m_overlays.size(); i--;) {
        Scope* overlay = m_overlays[i];
        Symbol* symbol = overlay->find(impl);
        if (!symbol)
            continue;
        switch (symbol->binding) {
        case Binding::Local:
            return { Access::Kind::Fast, variableIn(overlay) };
        case Binding::Cell:
            return { Access::Kind::Cell, variableIn(overlay) };
        case Binding::GlobalExplicit:
            return { Access::Kind::Global };
        case Binding::GlobalImplicit:
            // The names of a class are not visible in a comprehension in its body.
            return { isFunctionLike() || m_kind == UnitKind::Class ? Access::Kind::Global : Access::Kind::Name };
        case Binding::Free:
        case Binding::Unknown:
            break;
        }
    }

    bool fromComprehension = !m_overlays.isEmpty();
    Symbol* symbol = m_scope->find(impl);
    Binding binding = symbol ? symbol->binding : Binding::Unknown;
    if (fromComprehension && !isFunctionLike()) {
        // What is free in the comprehension is a variable of a function around the class, whatever the name is in the class.
        if (Variable* variable = variableIn(m_scope))
            return { Access::Kind::Cell, variable };
    }
    switch (binding) {
    case Binding::Free:
        if (m_kind == UnitKind::Class && !fromComprehension)
            return { Access::Kind::ClassCell, variableIn(m_scope) };
        if (m_scope->canSeeClassScope)
            return { Access::Kind::ClassDictCell, variableIn(m_scope) };
        return { Access::Kind::Cell, variableIn(m_scope) };
    case Binding::Cell:
        return { Access::Kind::Cell, variableIn(m_scope) };
    case Binding::Local:
        if (isFunctionLike())
            return { Access::Kind::Fast, variableIn(m_scope) };
        return { Access::Kind::Name };
    case Binding::GlobalImplicit:
        if (!isFunctionLike())
            return { m_kind == UnitKind::Class && fromComprehension ? Access::Kind::Global : Access::Kind::Name };
        return { m_scope->canSeeClassScope ? Access::Kind::ClassDictGlobal : Access::Kind::Global };
    case Binding::GlobalExplicit:
        return { Access::Kind::Global };
    case Binding::Unknown:
        break;
    }
    return { Access::Kind::Name };
}

// The register with the cell of a variable that an enclosed function uses.
RegisterID* Unit::cellFor(Name name)
{
    auto* impl = name->impl();
    for (size_t i = m_overlays.size(); i--;) {
        Symbol* symbol = m_overlays[i]->find(impl);
        if (symbol && symbol->binding == Binding::Cell)
            return m_variables.get(m_overlays[i])->find(impl)->value.reg;
    }
    auto* map = m_variables.get(m_scope);
    auto it = map->find(impl);
    if (it == map->end() || it->value.kind == Variable::Kind::Fast) {
        c.failWithoutLocation(SyntaxErrorInfo::Type::SystemError, makeString("no cell for the free variable '"_s, name->string(), "' in "_s, m_info.name.string()));
        return emptyValue();
    }
    return it->value.reg;
}

// The namespace of the enclosing class.
RegisterID* Unit::classDict(RegisterID* dst)
{
    return g.emitPyLoadCell(dst, cellFor(names().classDict), nameConstant(names().classDict), UnboundKind::Free);
}

RegisterID* Unit::loadName(Name name, RegisterID* dst, const Range& range)
{
    Name mangled = mangle(name);
    Access access = resolve(mangled);
    switch (access.kind) {
    case Access::Kind::Fast: {
        RegisterID* reg = access.variable->reg;
        if (access.variable->mayBeUnbound && !isKnownToBeBound(*access.variable)) {
            loc(range);
            g.emitTDZCheck(reg);
            noteBound(*access.variable);
        }
        if (dst)
            return g.move(dst, reg);
        // What is read now must not change if the variable is assigned to before the value is used.
        if (m_copyLocals)
            return g.move(g.newTemporary(), reg);
        return reg;
    }
    case Access::Kind::Cell:
        loc(range);
        return g.emitPyLoadCell(g.finalDestination(dst), access.variable->reg, nameConstant(mangled), access.variable->kind == Variable::Kind::Free ? UnboundKind::Free : UnboundKind::Local);
    case Access::Kind::ClassCell: {
        Reg result = g.finalDestination(dst);
        loc(range);
        return callRuntime(result.get(), RuntimeFunction::LoadFromDictOrCell, { m_namespace, nameConstant(mangled), access.variable->reg });
    }
    case Access::Kind::ClassDictCell: {
        Reg result = g.finalDestination(dst);
        Reg mapping = newTemporary();
        loc(range);
        classDict(mapping.get());
        return callRuntime(result.get(), RuntimeFunction::LoadFromDictOrCell, { mapping.get(), nameConstant(mangled), access.variable->reg });
    }
    case Access::Kind::Global:
        loc(range);
        return g.emitPyLoadGlobal(g.finalDestination(dst), usedName(mangled));
    case Access::Kind::ClassDictGlobal: {
        Reg result = g.finalDestination(dst);
        Reg mapping = newTemporary();
        loc(range);
        classDict(mapping.get());
        return callRuntime(result.get(), RuntimeFunction::LoadFromDictOrGlobals, { mapping.get(), usedName(mangled) });
    }
    case Access::Kind::Name:
        loc(range);
        return g.emitPyLoadName(g.finalDestination(dst), m_namespace, usedName(mangled));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Unit::storeName(Name name, RegisterID* value, const Range& range)
{
    Name mangled = mangle(name);
    Access access = resolve(mangled);
    switch (access.kind) {
    case Access::Kind::Fast:
        g.move(access.variable->reg, value);
        noteBound(*access.variable);
        return;
    case Access::Kind::Cell:
    case Access::Kind::ClassCell:
    case Access::Kind::ClassDictCell:
        g.emitPyStoreCell(access.variable->reg, value);
        return;
    case Access::Kind::Global:
    case Access::Kind::ClassDictGlobal:
        loc(range);
        g.emitPyStoreGlobal(usedName(mangled), value);
        return;
    case Access::Kind::Name:
        loc(range);
        g.emitPyStoreName(m_namespace, usedName(mangled), value);
        return;
    }
}

void Unit::deleteName(Name name, const Range& range)
{
    Name mangled = mangle(name);
    Access access = resolve(mangled);
    loc(range);
    switch (access.kind) {
    case Access::Kind::Fast:
        RELEASE_ASSERT(access.variable->boundBit < 0);
        g.emitTDZCheck(access.variable->reg);
        g.move(access.variable->reg, emptyValue());
        return;
    case Access::Kind::Cell:
    case Access::Kind::ClassCell:
    case Access::Kind::ClassDictCell:
        callRuntimeForEffect(RuntimeFunction::DeleteCell, { access.variable->reg, nameConstant(mangled), constant(jsNumber(static_cast<int>(access.variable->kind == Variable::Kind::Free ? UnboundKind::Free : UnboundKind::Local))) });
        return;
    case Access::Kind::Global:
    case Access::Kind::ClassDictGlobal:
        callRuntimeForEffect(RuntimeFunction::DeleteGlobal, { usedName(mangled) });
        return;
    case Access::Kind::Name:
        callRuntimeForEffect(RuntimeFunction::DeleteName, { m_namespace, usedName(mangled) });
        return;
    }
}

// ---- Blocks ----

Unit::Try Unit::beginTry()
{
    size_t depth = g.tryDepth();
    Ref<Label> handler = g.newLabel();
    Ref<Label> start = g.newEmittedLabel();
    TryData* data = g.pushTry(start.get(), handler.get(), HandlerType::Catch);
    return { data, WTF::move(handler), depth };
}

void Unit::endTry(Try& tryRange)
{
    Ref<Label> end = g.newEmittedLabel();
    g.popTry(tryRange.data, end.get());
}

Unit::Reg Unit::beginHandler(Try& tryRange)
{
    g.settlePythonLineMark();
    g.emitLabel(tryRange.handler.get());
    noLine();
    Reg thrown = newTemporary();
    g.emitOutOfLineCatchHandler(thrown.get(), nullptr, tryRange.data);
    return thrown;
}

Unit::Reg Unit::beginExceptionHandler(Try& tryRange, Reg& previous)
{
    Reg exception = beginHandler(tryRange);
    callRuntime(exception.get(), RuntimeFunction::CatchException, { exception.get() });
    previous = newTemporary();
    callRuntime(previous.get(), RuntimeFunction::SetHandledException, { exception.get() });
    return exception;
}

void Unit::restoreHandledException(RegisterID* previous)
{
    callRuntimeForEffect(RuntimeFunction::SetHandledException, { previous });
}

void Unit::reraise(RegisterID* exception)
{
    callRuntimeForEffect(RuntimeFunction::Reraise, { exception });
}

void Unit::suspendTry(size_t depth, Vector<TryContext>& suspended)
{
    Vector<TryContext> more = g.suspendTryContexts(depth);
    // The outermost first, which is the order to resume them in.
    more.appendVector(suspended);
    suspended = WTF::move(more);
}

bool Unit::inExceptionHandler() const
{
    for (auto& block : m_blocks) {
        switch (block.kind) {
        case BlockKind::TryExcept:
        case BlockKind::FinallyTry:
        case BlockKind::FinallyEnd:
        case BlockKind::ExceptionHandler:
        case BlockKind::ExceptionGroupHandler:
        case BlockKind::HandlerCleanup:
            return true;
        default:
            break;
        }
    }
    return false;
}

void Unit::pushBlock(Block&& block)
{
    block.number = ++m_blockCount;
    markBlock(block, true);
    m_blocks.append(WTF::move(block));
}

void Unit::popBlock()
{
    markBlock(m_blocks.last(), false);
    m_blocks.removeLast();
}

void Unit::markBlock(const Block& block, bool begins)
{
    TracedBlock::Kind kind;
    switch (block.kind) {
    case BlockKind::ForLoop:
    case BlockKind::AsyncForLoop:
        kind = TracedBlock::ForLoop;
        break;
    case BlockKind::With:
    case BlockKind::AsyncWith:
        kind = TracedBlock::With;
        break;
    case BlockKind::HandlerCleanup:
    case BlockKind::ExceptionHandler:
        kind = TracedBlock::ExceptionHandler;
        break;
    case BlockKind::FinallyEnd:
    case BlockKind::ExceptionGroupHandler:
        kind = TracedBlock::ExceptionHandlerWithMore;
        break;
    case BlockKind::InlinedComprehension:
    case BlockKind::MatchSubject:
        kind = TracedBlock::Other;
        break;
    case BlockKind::WhileLoop:
    case BlockKind::TryExcept:
    case BlockKind::FinallyTry:
        return;
    }
    g.addPythonMark(begins ? PythonMark::BlockBegin : PythonMark::BlockEnd, block.number, TracedBlock::encode(kind, block.saved ? block.saved->virtualRegister().offset() : VirtualRegister().offset()));
}

void Unit::unwindBlock(const Block& block, Vector<TryContext>& suspended)
{
    suspendTry(block.tryDepth, suspended);
    switch (block.kind) {
    case BlockKind::WhileLoop:
    case BlockKind::ForLoop:
    case BlockKind::AsyncForLoop:
    case BlockKind::TryExcept:
    case BlockKind::ExceptionHandler:
    case BlockKind::ExceptionGroupHandler:
        return;
    case BlockKind::InlinedComprehension:
    case BlockKind::MatchSubject:
        return;
    case BlockKind::FinallyTry:
        statements(block.finalBody);
        return;
    case BlockKind::FinallyEnd:
        restoreHandledException(block.saved);
        return;
    case BlockKind::With:
    case BlockKind::AsyncWith: {
        Reg result = newTemporary();
        RegisterID* none = constant(jsUndefined());
        emitSimpleCall(result.get(), block.saved, { none, none, none }, block.range);
        if (block.kind == BlockKind::AsyncWith)
            emitAwait(result.get(), result.get(), 2);
        return;
    }
    case BlockKind::HandlerCleanup:
        restoreHandledException(block.saved);
        if (block.name) {
            SameLine sameLine(*this);
            storeName(block.name, constant(jsUndefined()), block.range);
            deleteName(block.name, block.range);
        }
        return;
    }
}

std::optional<Unit::Block> Unit::unwindBlocks(const Range& range, bool stopAtLoop, Vector<TryContext>& suspended)
{
    if (m_blocks.isEmpty())
        return std::nullopt;
    Block top = m_blocks.last();
    if (top.kind == BlockKind::ExceptionGroupHandler) {
        fail(range, "'break', 'continue' and 'return' cannot appear in an except* block"_s);
        return std::nullopt;
    }
    if (stopAtLoop && (top.kind == BlockKind::WhileLoop || top.kind == BlockKind::ForLoop || top.kind == BlockKind::AsyncForLoop))
        return top;
    // What the block runs on the way out is outside the block.
    markBlock(top, false);
    m_blocks.removeLast();
    unwindBlock(top, suspended);
    auto result = unwindBlocks(range, stopAtLoop, suspended);
    markBlock(top, true);
    m_blocks.append(top);
    return result;
}

// ---- Statements ----

// There is a `break` in them that leaves the loop they are the body of.
static bool leaveLoop(Seq<Stmt*> body)
{
    for (Stmt* node : body) {
        switch (node->kind) {
        case StmtKind::Break:
            return true;
        case StmtKind::If:
            if (leaveLoop(node->as<If>()->body) || leaveLoop(node->as<If>()->orelse))
                return true;
            break;
        case StmtKind::With:
        case StmtKind::AsyncWith:
            if (leaveLoop(node->as<With>()->body))
                return true;
            break;
        case StmtKind::For:
        case StmtKind::AsyncFor:
            if (leaveLoop(node->as<For>()->orelse))
                return true;
            break;
        case StmtKind::While:
            if (leaveLoop(node->as<While>()->orelse))
                return true;
            break;
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* statement = node->as<AST::Try>();
            if (leaveLoop(statement->body) || leaveLoop(statement->orelse) || leaveLoop(statement->finalbody))
                return true;
            for (ExceptHandler* handler : statement->handlers) {
                if (leaveLoop(handler->body))
                    return true;
            }
            break;
        }
        case StmtKind::Match:
            for (MatchCase* matchCase : node->as<Match>()->cases) {
                if (leaveLoop(matchCase->body))
                    return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

bool Unit::neverGetsPast(Seq<Stmt*> body)
{
    for (Stmt* node : body) {
        if (neverGetsPast(node))
            return true;
    }
    return false;
}

bool Unit::neverGetsPast(Stmt* node)
{
    switch (node->kind) {
    case StmtKind::Return:
    case StmtKind::Raise:
    case StmtKind::Break:
    case StmtKind::Continue:
        return true;
    case StmtKind::If:
        return neverGetsPast(node->as<If>()->body) && neverGetsPast(node->as<If>()->orelse);
    case StmtKind::While:
        return constantTruth(node->as<While>()->test) == true && !leaveLoop(node->as<While>()->body);
    case StmtKind::Try:
    case StmtKind::TryStar: {
        auto* statement = node->as<AST::Try>();
        if (neverGetsPast(statement->finalbody))
            return true;
        if (!neverGetsPast(statement->orelse.isEmpty() ? statement->body : statement->orelse))
            return false;
        for (ExceptHandler* handler : statement->handlers) {
            if (!neverGetsPast(handler->body))
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

void Unit::statements(Seq<Stmt*> body, unsigned first)
{
    for (unsigned i = first; i < body.size(); ++i) {
        statement(body[i]);
        // CPython's compiler drops what cannot be reached, so that the code has no such lines: nobody can jump to them or find that
        // they were never run. Here it stays, and is of no line.
        if (i + 1 < body.size() && !m_sameLineDepth && neverGetsPast(body[i])) {
            noLine();
            SameLine sameLine(*this);
            while (++i < body.size())
                statement(body[i]);
        }
    }
}

void Unit::statement(Stmt* node)
{
    if (!isSafeToRecurse())
        return;

    m_statementStart = g.instructions().size();
    switch (node->kind) {
    case StmtKind::FunctionDef:
    case StmtKind::AsyncFunctionDef:
        return functionDefinition(node->as<FunctionDef>());
    case StmtKind::ClassDef:
        return classDefinition(node->as<ClassDef>());
    case StmtKind::TypeAlias:
        return typeAlias(node->as<AST::TypeAlias>());
    case StmtKind::Return:
        return returnStatement(node->as<Return>());
    case StmtKind::Delete:
        for (Expr* target : node->as<Delete>()->targets)
            deleteTarget(target);
        return;
    case StmtKind::Assign:
        return assign(node->as<Assign>());
    case StmtKind::AugAssign:
        return augmentedAssign(node->as<AugAssign>());
    case StmtKind::AnnAssign:
        return annotatedAssign(node->as<AnnAssign>());
    case StmtKind::For:
        return forLoop(node->as<For>());
    case StmtKind::AsyncFor:
        return asyncForLoop(node->as<For>());
    case StmtKind::While:
        return whileLoop(node->as<While>());
    case StmtKind::If:
        return ifStatement(node->as<If>());
    case StmtKind::With:
    case StmtKind::AsyncWith:
        return withStatement(node->as<With>(), 0);
    case StmtKind::Match:
        return matchStatement(node->as<Match>());
    case StmtKind::Raise:
        return raiseStatement(node->as<Raise>());
    case StmtKind::Try:
    case StmtKind::TryStar:
        return tryStatement(node->as<AST::Try>());
    case StmtKind::Assert:
        return assertStatement(node->as<Assert>());
    case StmtKind::Import:
        return importStatement(node->as<Import>());
    case StmtKind::ImportFrom:
        return importFrom(node->as<ImportFrom>());
    case StmtKind::Global:
    case StmtKind::Nonlocal:
        return;
    case StmtKind::Pass:
        return line(node->range.lineno);
    case StmtKind::Expr: {
        Expr* value = node->as<ExprStmt>()->value;
        if (c.m_isInteractive && m_kind == UnitKind::Module) {
            Reg result = expr(value);
            loc(node->range);
            callRuntimeForEffect(RuntimeFunction::PrintExpression, { result.get() });
            return;
        }
        exprForEffect(value);
        return;
    }
    case StmtKind::Break:
        return breakOrContinue(node, true);
    case StmtKind::Continue:
        return breakOrContinue(node, false);
    }
}

void Unit::assign(Assign* node)
{
    // `x = value` for a variable in a register computes the value right there.
    if (node->targets.size() == 1) {
        if (auto* target = node->targets[0]->tryAs<NameExpr>()) {
            Access access = resolve(mangle(target->id));
            if (access.kind == Access::Kind::Fast) {
                expr(node->value, access.variable->reg);
                noteBound(*access.variable);
                line(target->range.lineno);
                return;
            }
        }
        // `a, b = b, a` needs no tuple.
        auto* targets = node->targets[0]->tryAs<Tuple>();
        auto* values = node->value->tryAs<Tuple>();
        if (targets && values && targets->elts.size() == values->elts.size() && targets->elts.size() <= 8) {
            bool isPlain = true;
            for (Expr* item : targets->elts)
                isPlain &= !item->is<Starred>();
            for (Expr* item : values->elts)
                isPlain &= !item->is<Starred>();
            if (isPlain) {
                Vector<Reg, 8> temporaries;
                for (Expr* item : values->elts) {
                    Reg temporary = newTemporary();
                    expr(item, temporary.get());
                    temporaries.append(WTF::move(temporary));
                }
                for (unsigned i = 0; i < targets->elts.size(); ++i)
                    store(targets->elts[i], temporaries[i].get());
                return;
            }
        }
    }
    Reg value = expr(node->value);
    for (Expr* target : node->targets)
        store(target, value.get());
}

static BinaryOperator inPlaceOperator(Operator op)
{
    return static_cast<BinaryOperator>(static_cast<unsigned>(BinaryOperator::FirstInPlace) + static_cast<unsigned>(op));
}

void Unit::augmentedAssign(AugAssign* node)
{
    Expr* target = node->target;
    BinaryOperator op = inPlaceOperator(node->op);
    switch (target->kind) {
    case ExprKind::Name: {
        auto* name = target->as<NameExpr>();
        Reg left = loadName(name->id, nullptr, name->range);
        Reg right = expr(node->value);
        Access access = resolve(mangle(name->id));
        loc(node->range);
        if (access.kind == Access::Kind::Fast) {
            g.emitPyBinary(access.variable->reg, left.get(), right.get(), op);
            return;
        }
        Reg result = g.emitPyBinary(g.finalDestination(nullptr, left.get()), left.get(), right.get(), op);
        storeName(name->id, result.get(), name->range);
        return;
    }
    case ExprKind::Attribute: {
        auto* attribute = target->as<Attribute>();
        Reg base = expr(attribute->value);
        Reg left = newTemporary();
        loc(attribute->range);
        g.emitPyGetAttr(left.get(), base.get(), usedName(mangle(attribute->attr)));
        Reg right = expr(node->value);
        loc(node->range);
        g.emitPyBinary(left.get(), left.get(), right.get(), op);
        loc(attribute->range);
        g.emitPySetAttr(base.get(), usedName(mangle(attribute->attr)), left.get());
        return;
    }
    case ExprKind::Subscript: {
        auto* subscript = target->as<Subscript>();
        Reg base = expr(subscript->value);
        Reg key = expr(subscript->slice);
        Reg left = newTemporary();
        loc(subscript->range);
        g.emitPyGetItem(left.get(), base.get(), key.get());
        Reg right = expr(node->value);
        loc(node->range);
        g.emitPyBinary(left.get(), left.get(), right.get(), op);
        loc(subscript->range);
        g.emitPySetItem(base.get(), key.get(), left.get());
        return;
    }
    default:
        c.failWithoutLocation(SyntaxErrorInfo::Type::SystemError, makeString("invalid node type ("_s, static_cast<int>(node->target->kind) + 1, ") for augmented assignment"_s));
    }
}

void Unit::checkAnnotationTarget(Expr* node)
{
    // Everything in a subscript has to be defined, though nothing is done with it.
    if (auto* slice = node->tryAs<Slice>()) {
        for (Expr* part : { slice->lower, slice->upper, slice->step }) {
            if (part)
                exprForEffect(part);
        }
        return;
    }
    if (auto* tuple = node->tryAs<Tuple>()) {
        for (Expr* item : tuple->elts)
            checkAnnotationTarget(item);
        return;
    }
    Reg ignored = expr(node);
}

void Unit::annotatedAssign(AnnAssign* node)
{
    Expr* target = node->target;
    if (node->value) {
        Reg value = expr(node->value);
        store(target, value.get());
    }
    switch (target->kind) {
    case ExprKind::Name:
        // The annotation of a plain name in a module or a class is kept.
        if (!node->simple || (m_kind != UnitKind::Module && m_kind != UnitKind::Class))
            return;
        if (c.m_future.annotations) {
            Reg annotations = newTemporary();
            loc(node->range);
            g.emitPyLoadName(annotations.get(), m_namespace, nameConstant(c.name("__annotations__"_s)));
            g.emitPySetItem(annotations.get(), nameConstant(mangle(target->as<NameExpr>()->id)), stringConstant(annotationText(node->annotation)));
            return;
        }
        if (auto it = m_conditionalAnnotationIndices.find(node); it != m_conditionalAnnotationIndices.end() && it->value >= 0) {
            // Note that the statement was reached.
            Reg set = newTemporary();
            loc(node->range);
            Name name = names().conditionalAnnotations;
            if (m_kind == UnitKind::Class)
                g.emitPyLoadCell(set.get(), cellFor(name), nameConstant(name), UnboundKind::Local);
            else
                g.emitPyLoadName(set.get(), m_namespace, nameConstant(name));
            callRuntimeForEffect(RuntimeFunction::SetAdd, { set.get(), constant(jsNumber(it->value)) });
        }
        return;
    case ExprKind::Attribute:
        if (!node->value)
            Reg ignored = expr(target->as<Attribute>()->value);
        return;
    case ExprKind::Subscript:
        if (!node->value) {
            {
                Reg ignored = expr(target->as<Subscript>()->value);
            }
            checkAnnotationTarget(target->as<Subscript>()->slice);
        }
        return;
    default:
        c.failWithoutLocation(SyntaxErrorInfo::Type::SystemError, makeString("invalid node type ("_s, static_cast<int>(target->kind) + 1, ") for annotated assignment"_s));
    }
}

void Unit::ifStatement(If* node)
{
    Ref<Label> end = g.newLabel();
    if (node->orelse.isEmpty()) {
        jumpIf(node->test, end.get(), false);
        statements(node->body);
    } else {
        Ref<Label> orelse = g.newLabel();
        jumpIf(node->test, orelse.get(), false);
        statements(node->body);
        g.emitJump(end.get());
        g.emitLabel(orelse.get());
        statements(node->orelse);
    }
    g.emitLabel(end.get());
}

void Unit::whileLoop(While* node)
{
    Ref<Label> loop = g.newLabel();
    Ref<Label> anchor = g.newLabel();
    Ref<Label> end = g.newLabel();

    g.settlePythonLineMark();
    g.emitLabel(loop.get());
    m_statementStart = g.instructions().size();
    // The line a signal handler finds the frame at.
    loc(node->test->range);
    g.emitLoopHint();
    pushBlock({ .kind = BlockKind::WhileLoop, .tryDepth = g.tryDepth(), .continueTarget = loop.ptr(), .breakTarget = end.ptr() });
    m_finallyContexts.append({ false, true });
    jumpIf(node->test, anchor.get(), false);
    statements(node->body);
    g.emitJump(loop.get());
    m_finallyContexts.removeLast();
    popBlock();

    g.emitLabel(anchor.get());
    statements(node->orelse);
    g.emitLabel(end.get());
}

void Unit::forLoop(For* node)
{
    Ref<Label> start = g.newLabel();
    Ref<Label> cleanup = g.newLabel();
    Ref<Label> end = g.newLabel();

    Reg iterator = newTemporary();
    {
        Reg iterable = expr(node->iter);
        loc(node->iter->range);
        g.emitPyGetIter(iterator.get(), iterable.get());
    }

    pushBlock({ .kind = BlockKind::ForLoop, .tryDepth = g.tryDepth(), .continueTarget = start.ptr(), .breakTarget = end.ptr(), .saved = iterator.get() });
    m_finallyContexts.append({ false, true });
    g.emitLabel(start.get());
    line(node->iter->range.lineno);
    g.emitLoopHint();
    loc(node->iter->range);
    bool stored = false;
    if (auto* target = node->target->tryAs<NameExpr>()) {
        Access access = resolve(mangle(target->id));
        if (access.kind == Access::Kind::Fast) {
            g.emitPyForIter(access.variable->reg, iterator.get(), cleanup.get());
            noteBound(*access.variable);
            stored = true;
        }
    }
    if (!stored) {
        Reg item = newTemporary();
        g.emitPyForIter(item.get(), iterator.get(), cleanup.get());
        store(node->target, item.get());
    }
    line(node->target->range.lineno);
    statements(node->body);
    g.emitJump(start.get());
    m_finallyContexts.removeLast();
    popBlock();

    g.emitLabel(cleanup.get());
    statements(node->orelse);
    g.emitLabel(end.get());
}

void Unit::asyncForLoop(For* node)
{
    Ref<Label> start = g.newLabel();
    Ref<Label> end = g.newLabel();

    Reg iterator = newTemporary();
    {
        Reg iterable = expr(node->iter);
        loc(node->iter->range);
        callRuntime(iterator.get(), RuntimeFunction::GetAIter, { iterable.get() });
    }

    g.emitLabel(start.get());
    line(node->iter->range.lineno);
    g.emitLoopHint();
    pushBlock({ .kind = BlockKind::AsyncForLoop, .tryDepth = g.tryDepth(), .continueTarget = start.ptr(), .breakTarget = end.ptr(), .saved = iterator.get() });
    m_finallyContexts.append({ false, true });

    // StopAsyncIteration from __anext__ ends the loop.
    Try guard = beginTry();
    Reg item = newTemporary();
    loc(node->range);
    callRuntime(item.get(), RuntimeFunction::GetANext, { iterator.get() });
    emitDelegate(item.get(), item.get());
    endTry(guard);

    store(node->target, item.get());
    item = nullptr;
    statements(node->body);
    g.emitJump(start.get());
    m_finallyContexts.removeLast();
    popBlock();

    Reg thrown = beginHandler(guard);
    // The end of the loop is not told of as a line.
    silentLine(node->iter->range.lineno);
    loc(node->iter->range);
    callRuntimeForEffect(RuntimeFunction::EndAsyncFor, { thrown.get() });
    thrown = nullptr;
    statements(node->orelse);
    g.emitLabel(end.get());
}

void Unit::returnStatement(Return* node)
{
    if (!isFunctionLike()) {
        fail(node->range, "'return' outside function"_s);
        return;
    }
    if (node->value && isAsyncGenerator()) {
        fail(node->range, "'return' with value in async generator"_s);
        return;
    }
    if (!m_finallyContexts.isEmpty() && m_finallyContexts.last().inFinally && c.m_warned.add(node).isNewEntry)
        warn(node->range, "'return' in a 'finally' block"_s);

    Reg value;
    if (node->value) {
        value = expr(node->value);
        // A `finally` clause that runs on the way out may assign to the variable.
        if (!value->isTemporary() && !value->virtualRegister().isConstant() && !m_blocks.isEmpty())
            value = g.move(g.newTemporary(), value.get());
    } else
        value = constant(jsUndefined());
    line(node->range.lineno);

    Vector<TryContext> suspended;
    unwindBlocks(node->range, false, suspended);
    g.emitReturn(value.get());
    resumeTry(WTF::move(suspended));
}

void Unit::breakOrContinue(Stmt* node, bool isBreak)
{
    if (!m_finallyContexts.isEmpty() && m_finallyContexts.last().inFinally && !m_finallyContexts.last().inLoop && c.m_warned.add(node).isNewEntry)
        warn(node->range, isBreak ? "'break' in a 'finally' block"_s : "'continue' in a 'finally' block"_s);

    line(node->range.lineno);
    Vector<TryContext> suspended;
    auto loop = unwindBlocks(node->range, true, suspended);
    if (!loop)
        fail(node->range, isBreak ? "'break' outside loop"_s : "'continue' not properly in loop"_s);
    else {
        suspendTry(loop->tryDepth, suspended);
        g.emitJump(isBreak ? *loop->breakTarget : *loop->continueTarget);
    }
    resumeTry(WTF::move(suspended));
}

void Unit::raiseStatement(Raise* node)
{
    Arguments arguments(*this, 2);
    if (node->exc)
        expr(node->exc, arguments[0]);
    else
        g.move(arguments[0], emptyValue());
    if (node->cause)
        expr(node->cause, arguments[1]);
    else
        g.move(arguments[1], emptyValue());
    loc(node->range);
    callRuntime(arguments[0], RuntimeFunction::Raise, arguments);
}

void Unit::tryStatement(AST::Try* node)
{
    line(node->range.lineno);
    if (!node->finalbody.isEmpty())
        return tryFinally(node);
    if (node->isStar())
        return tryStarExcept(node);
    tryExcept(node);
}

void Unit::tryFinally(AST::Try* node)
{
    Ref<Label> exit = g.newLabel();

    Try body = beginTry();
    pushBlock({ .kind = BlockKind::FinallyTry, .tryDepth = body.depthBefore, .finalBody = node->finalbody });
    if (node->handlers.isEmpty())
        statements(node->body);
    else if (node->isStar())
        tryStarExcept(node);
    else
        tryExcept(node);
    popBlock();
    endTry(body);

    m_finallyContexts.append({ true, false });
    statements(node->finalbody);
    hiddenJump(exit.get());

    // An exception: it goes on after the clause has run.
    Reg previous;
    Reg exception = beginExceptionHandler(body, previous);
    Try cleanup = beginTry();
    pushBlock({ .kind = BlockKind::FinallyEnd, .tryDepth = cleanup.depthBefore, .saved = previous.get() });
    statements(node->finalbody);
    popBlock();
    m_finallyContexts.removeLast();
    reraise(exception.get());
    endTry(cleanup);

    // That, or an exception in the clause itself.
    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    g.emitLabel(exit.get());
}

void Unit::tryExcept(AST::Try* node)
{
    Ref<Label> end = g.newLabel();

    Try body = beginTry();
    pushBlock({ .kind = BlockKind::TryExcept, .tryDepth = body.depthBefore });
    statements(node->body);
    popBlock();
    endTry(body);
    statements(node->orelse);
    hiddenJump(end.get());

    Reg previous;
    Reg exception = beginExceptionHandler(body, previous);
    Try cleanup = beginTry();
    pushBlock({ .kind = BlockKind::ExceptionHandler, .tryDepth = cleanup.depthBefore, .saved = previous.get() });

    unsigned count = node->handlers.size();
    for (unsigned i = 0; i < count; ++i) {
        ExceptHandler* handler = node->handlers[i];
        if (!handler->type && i + 1 < count) {
            fail(handler->range, "default 'except:' must be last"_s);
            break;
        }
        Ref<Label> next = g.newLabel();
        m_statementStart = g.instructions().size();
        line(handler->range.lineno);
        if (handler->type) {
            Reg type = expr(handler->type);
            Reg matches = newTemporary();
            loc(handler->range);
            g.emitPyCompare(matches.get(), exception.get(), type.get(), CompareOperator::ExceptionMatch);
            g.emitPyJumpIfFalse(matches.get(), next.get());
        }
        if (handler->name) {
            // try:
            //     body
            // finally:
            //     name = None  # in case the body deleted it
            //     del name
            storeName(handler->name, exception.get(), handler->range);
            Try nameCleanup = beginTry();
            pushBlock({ .kind = BlockKind::HandlerCleanup, .tryDepth = cleanup.depthBefore, .saved = previous.get(), .name = handler->name, .range = handler->range });
            statements(handler->body);
            popBlock();
            endTry(nameCleanup);
            outsideTry(cleanup.depthBefore, [&] {
                SameLine sameLine(*this);
                restoreHandledException(previous.get());
                storeName(handler->name, constant(jsUndefined()), handler->range);
                deleteName(handler->name, handler->range);
                hiddenJump(end.get());
            });

            Reg thrown = beginHandler(nameCleanup);
            SameLine sameLine(*this);
            storeName(handler->name, constant(jsUndefined()), handler->range);
            deleteName(handler->name, handler->range);
            reraise(thrown.get());
        } else {
            pushBlock({ .kind = BlockKind::HandlerCleanup, .tryDepth = cleanup.depthBefore, .saved = previous.get() });
            statements(handler->body);
            popBlock();
            outsideTry(cleanup.depthBefore, [&] {
                restoreHandledException(previous.get());
                hiddenJump(end.get());
            });
        }
        g.emitLabel(next.get());
    }
    popBlock();

    // No clause caught it.
    if (count)
        line(node->handlers[count - 1]->range.lineno);
    reraise(exception.get());
    endTry(cleanup);

    // That, or an exception in a clause.
    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    g.emitLabel(end.get());
}

void Unit::tryStarExcept(AST::Try* node)
{
    Ref<Label> orelse = g.newLabel();
    Ref<Label> end = g.newLabel();

    Try body = beginTry();
    pushBlock({ .kind = BlockKind::TryExcept, .tryDepth = body.depthBefore });
    statements(node->body);
    popBlock();
    endTry(body);
    hiddenJump(orelse.get());

    Reg previous;
    Reg original = beginExceptionHandler(body, previous);
    Try cleanup = beginTry();
    pushBlock({ .kind = BlockKind::ExceptionGroupHandler, .tryDepth = cleanup.depthBefore, .saved = previous.get() });

    // What the clauses raise, and at the end what none of them caught.
    Reg raised = newTemporary();
    g.emitPyNewList(raised.get(), nullptr, 0);
    // What is left to catch.
    Reg rest = newTemporary();
    g.move(rest.get(), original.get());

    for (ExceptHandler* handler : node->handlers) {
        Ref<Label> next = g.newLabel();
        Reg match = newTemporary();
        m_statementStart = g.instructions().size();
        line(handler->range.lineno);
        if (handler->type) {
            Reg type = expr(handler->type);
            loc(handler->range);
            callRuntime(match.get(), RuntimeFunction::MatchExceptionGroup, { rest.get(), type.get() });
            callRuntime(rest.get(), RuntimeFunction::TakeExceptionGroupRest);
            Reg isNone = newTemporary();
            g.emitPyIsNone(isNone.get(), match.get());
            g.emitJumpIfTrue(isNone.get(), next.get());
        } else
            g.move(match.get(), rest.get());

        if (handler->name)
            storeName(handler->name, match.get(), handler->range);
        // The part that matched is what sys.exception() shows in the clause.
        callRuntimeForEffect(RuntimeFunction::SetHandledException, { match.get() });
        match = nullptr;

        Try clause = beginTry();
        pushBlock({ .kind = BlockKind::HandlerCleanup, .tryDepth = cleanup.depthBefore, .saved = previous.get(), .name = handler->name, .range = handler->range });
        statements(handler->body);
        popBlock();
        endTry(clause);
        if (handler->name) {
            SameLine sameLine(*this);
            storeName(handler->name, constant(jsUndefined()), handler->range);
            deleteName(handler->name, handler->range);
        }
        g.emitJump(next.get());

        Reg thrown = beginHandler(clause);
        callRuntime(thrown.get(), RuntimeFunction::CatchException, { thrown.get() });
        if (handler->name) {
            SameLine sameLine(*this);
            storeName(handler->name, constant(jsUndefined()), handler->range);
            deleteName(handler->name, handler->range);
        }
        callRuntimeForEffect(RuntimeFunction::ListAppend, { raised.get(), thrown.get() });

        g.emitLabel(next.get());
        callRuntimeForEffect(RuntimeFunction::SetHandledException, { original.get() });
    }
    popBlock();

    callRuntimeForEffect(RuntimeFunction::ListAppend, { raised.get(), rest.get() });
    Reg toRaise = newTemporary();
    callRuntime(toRaise.get(), RuntimeFunction::PrepareReraiseStar, { original.get(), raised.get() });
    endTry(cleanup);

    restoreHandledException(previous.get());
    {
        Reg isNone = newTemporary();
        g.emitPyIsNone(isNone.get(), toRaise.get());
        g.emitJumpIfTrue(isNone.get(), end.get());
    }
    reraise(toRaise.get());

    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    g.emitLabel(orelse.get());
    statements(node->orelse);
    g.emitLabel(end.get());
}

void Unit::withStatement(With* node, unsigned index)
{
    bool isAsync = node->isAsync();
    WithItem* item = node->items[index];
    const Range& range = item->contextExpr->range;
    Ref<Label> end = g.newLabel();

    Reg exit = newTemporary();
    Reg value = newTemporary();
    {
        Reg manager = expr(item->contextExpr);
        loc(range);
        callRuntime(exit.get(), RuntimeFunction::LoadSpecial, { manager.get(), constant(jsNumber(static_cast<int>(isAsync ? SpecialMethodForWith::AExit : SpecialMethodForWith::Exit))) });
        callRuntime(value.get(), RuntimeFunction::LoadSpecial, { manager.get(), constant(jsNumber(static_cast<int>(isAsync ? SpecialMethodForWith::AEnter : SpecialMethodForWith::Enter))) });
    }
    emitSimpleCall(value.get(), value.get(), { }, range);
    if (isAsync)
        emitAwait(value.get(), value.get(), 1);

    Try body = beginTry();
    pushBlock({ .kind = isAsync ? BlockKind::AsyncWith : BlockKind::With, .tryDepth = body.depthBefore, .saved = exit.get(), .range = range });
    if (item->optionalVars)
        store(item->optionalVars, value.get());
    value = nullptr;
    if (index + 1 == node->items.size())
        statements(node->body);
    else
        withStatement(node, index + 1);
    popBlock();
    endTry(body);

    {
        Reg result = newTemporary();
        RegisterID* none = constant(jsUndefined());
        emitSimpleCall(result.get(), exit.get(), { none, none, none }, range);
        if (isAsync)
            emitAwait(result.get(), result.get(), 2);
    }
    hiddenJump(end.get());

    // An exception: __exit__ says whether it goes on.
    Reg previous;
    Reg exception = beginExceptionHandler(body, previous);
    Try cleanup = beginTry();
    Reg result = newTemporary();
    loc(range);
    callRuntime(result.get(), RuntimeFunction::WithExit, { exit.get(), exception.get() });
    if (isAsync)
        emitAwait(result.get(), result.get(), 2);
    Ref<Label> suppress = g.newLabel();
    g.emitPyJumpIfTrue(result.get(), suppress.get());
    // It goes on as if from where it was raised.
    noLine();
    reraise(exception.get());
    endTry(cleanup);

    g.emitLabel(suppress.get());
    line(range.lineno);
    restoreHandledException(previous.get());
    hiddenJump(end.get());

    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    g.emitLabel(end.get());
}

void Unit::assertStatement(Assert* node)
{
    if (auto* tuple = node->test->tryAs<Tuple>(); tuple && !tuple->elts.isEmpty() && c.m_warned.add(node).isNewEntry)
        warn(node->range, "assertion is always true, perhaps remove parentheses?"_s);
    if (c.m_options.optimize)
        return;
    Ref<Label> end = g.newLabel();
    jumpIf(node->test, end.get(), true);
    Arguments arguments(*this, 2);
    g.move(arguments[0], constant(runtime().AssertionError()));
    if (node->msg) {
        Reg message = expr(node->msg);
        emitSimpleCall(arguments[0], arguments[0], { message.get() }, node->range);
    }
    g.move(arguments[1], emptyValue());
    loc(node->test->range);
    callRuntime(arguments[0], RuntimeFunction::Raise, arguments);
    g.emitLabel(end.get());
}

ImportMode Unit::importMode(bool isLazy, bool isStar)
{
    if (isLazy)
        return ImportMode::Lazy;
    if (inExceptionHandler() || m_kind != UnitKind::Module || isStar)
        return ImportMode::Eager;
    return ImportMode::Default;
}

void Unit::importStatement(Import* node)
{
    for (Alias* alias : node->names) {
        if (node->isLazy && m_kind != UnitKind::Module) {
            fail(node->range, "lazy imports only allowed in module scope"_s);
            return;
        }
        Reg module = newTemporary();
        loc(node->range);
        noteConstant(jsNumber(0));
        noteConstant(jsUndefined());
        callRuntime(module.get(), RuntimeFunction::ImportName, { usedName(alias->name), constant(jsUndefined()), constant(jsNumber(0)), constant(jsNumber(static_cast<int>(importMode(node->isLazy, false)))) });

        const String& name = alias->name->string();
        size_t dot = name.find('.');
        if (alias->asname) {
            // `import a.b.c as d` binds d to c.
            while (dot != notFound) {
                size_t start = dot + 1;
                dot = name.find('.', start);
                String attribute = name.substring(start, dot == notFound ? name.length() - start : dot - start);
                callRuntime(module.get(), RuntimeFunction::ImportFrom, { module.get(), usedName(c.name(attribute)) });
            }
            storeName(alias->asname, module.get(), node->range);
        } else
            storeName(dot == notFound ? alias->name : c.name(name.left(dot)), module.get(), node->range);
    }
}

void Unit::importFrom(ImportFrom* node)
{
    bool isStar = node->names[0]->name->string() == "*"_s;
    if (node->isLazy) {
        if (isStar) {
            fail(node->range, "cannot lazy import *"_s);
            return;
        }
        if (m_kind != UnitKind::Module) {
            fail(node->range, "lazy imports only allowed in module scope"_s);
            return;
        }
    }

    Vector<JSValue, 8> names;
    for (Alias* alias : node->names)
        names.append(g.addStringConstant(*alias->name));
    JSValue fromList = PyTuple::create(globalObject(), names.span());

    Reg module = newTemporary();
    loc(node->range);
    noteConstant(jsNumber(node->level));
    noteConstant(fromList);
    callRuntime(module.get(), RuntimeFunction::ImportName, { node->module ? usedName(node->module) : usedName(c.name(emptyString())), constant(fromList), constant(jsNumber(node->level)), constant(jsNumber(static_cast<int>(importMode(node->isLazy, isStar)))) });

    if (isStar) {
        callRuntimeForEffect(RuntimeFunction::ImportStar, { module.get(), m_namespace ? m_namespace : emptyValue() });
        return;
    }
    for (Alias* alias : node->names) {
        Reg value = newTemporary();
        loc(node->range);
        callRuntime(value.get(), RuntimeFunction::ImportFrom, { module.get(), usedName(alias->name) });
        storeName(alias->asname ? alias->asname : alias->name, value.get(), node->range);
    }
}

// ---- Modules ----

String Unit::cleanDocstring(const String& docstring)
{
    // As inspect.cleandoc(), but for the blank lines at either end: tabs become spaces, and the indentation that all lines
    // after the first have in common goes.
    StringBuilder expanded;
    unsigned column = 0;
    for (unsigned i = 0; i < docstring.length(); ++i) {
        char16_t ch = docstring[i];
        if (ch == '\t') {
            unsigned spaces = 8 - column % 8;
            for (unsigned j = 0; j < spaces; ++j)
                expanded.append(' ');
            column += spaces;
            continue;
        }
        expanded.append(ch);
        if (ch == '\n' || ch == '\r')
            column = 0;
        else if (!U16_IS_TRAIL(ch))
            ++column;
    }
    String text = expanded.toString();
    unsigned length = text.length();

    unsigned position = 0;
    while (position < length && text[position++] != '\n') { }
    unsigned margin = std::numeric_limits<unsigned>::max();
    while (position < length) {
        unsigned start = position;
        while (position < length && text[position] == ' ')
            ++position;
        if (position < length && text[position] != '\n')
            margin = std::min(margin, position - start);
        while (position < length && text[position++] != '\n') { }
    }
    if (margin == std::numeric_limits<unsigned>::max())
        margin = 0;

    position = 0;
    while (position < length && text[position] == ' ')
        ++position;
    if (!position && !margin)
        return text;

    StringBuilder result;
    auto copyLine = [&] {
        while (position < length) {
            char16_t ch = text[position++];
            result.append(ch);
            if (ch == '\n')
                break;
        }
    };
    copyLine();
    while (position < length) {
        for (unsigned i = 0; i < margin && position < length && text[position] == ' '; ++i)
            ++position;
        copyLine();
    }
    return result.toString();
}

// Null: none.
String Unit::docstringOf(Seq<Stmt*> body)
{
    if (body.isEmpty() || body[0]->kind != StmtKind::Expr || c.m_options.optimize >= 2)
        return { };
    auto* constant = body[0]->as<ExprStmt>()->value->tryAs<Constant>();
    if (!constant || !constant->value.isString())
        return { };
    return cleanDocstring(String(constant->value.string));
}

void Unit::collectDeferredAnnotations(Seq<Stmt*> body, bool isConditional)
{
    auto visitAll = [&](Seq<Stmt*> nested) { collectDeferredAnnotations(nested, true); };
    for (Stmt* node : body) {
        switch (node->kind) {
        case StmtKind::AnnAssign: {
            auto* assign = node->as<AnnAssign>();
            if (!assign->simple || !assign->target->is<NameExpr>())
                break;
            int index = isConditional || m_kind == UnitKind::Module ? m_nextConditionalAnnotationIndex++ : -1;
            m_deferredAnnotations.append({ assign, index });
            m_conditionalAnnotationIndices.add(assign, index);
            break;
        }
        case StmtKind::For:
        case StmtKind::AsyncFor:
            visitAll(node->as<For>()->body);
            visitAll(node->as<For>()->orelse);
            break;
        case StmtKind::While:
            visitAll(node->as<While>()->body);
            visitAll(node->as<While>()->orelse);
            break;
        case StmtKind::If:
            visitAll(node->as<If>()->body);
            visitAll(node->as<If>()->orelse);
            break;
        case StmtKind::With:
        case StmtKind::AsyncWith:
            visitAll(node->as<With>()->body);
            break;
        case StmtKind::Match:
            for (MatchCase* matchCase : node->as<Match>()->cases)
                visitAll(matchCase->body);
            break;
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* tryNode = node->as<AST::Try>();
            visitAll(tryNode->body);
            visitAll(tryNode->orelse);
            for (ExceptHandler* handler : tryNode->handlers)
                visitAll(handler->body);
            visitAll(tryNode->finalbody);
            break;
        }
        default:
            break;
        }
    }
}

// if .format > VALUE_WITH_FAKE_GLOBALS: raise NotImplementedError
void Unit::emitAnnotationsPrologue(const Range& range)
{
    Ref<Label> body = g.newLabel();
    Reg isOther = newTemporary();
    loc(range);
    g.emitPyCompare(isOther.get(), g.pythonParameter(0), constant(jsNumber(2)), CompareOperator::Gt);
    g.emitPyJumpIfFalse(isOther.get(), body.get());
    callRuntimeForEffect(RuntimeFunction::Raise, { constant(runtime().NotImplementedError()), emptyValue() });
    g.emitLabel(body.get());
}

// Defines __annotate__, the function that evaluates the annotations of the variables of the module or class.
void Unit::emitDeferredAnnotations(const Range& range)
{
    if (m_deferredAnnotations.isEmpty())
        return;
    Scope* scope = m_scope->annotationBlock;
    ASSERT(scope);

    CodeSpec spec { UnitKind::Annotations, scope, scope->name, m_kind == UnitKind::Module ? Range { 1, 0, static_cast<int>(c.m_tree.lineStarts.size()) + 1, 0 } : m_scope->range, range.lineno };
    spec.positionalOnlyCount = 1;
    bool inClass = m_kind == UnitKind::Class;
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        auto& g = unit.g;
        unit.emitAnnotationsPrologue(range);
        Reg annotations = unit.newTemporary();
        unit.callRuntime(annotations.get(), RuntimeFunction::BuildMap);
        for (auto& deferred : m_deferredAnnotations) {
            AnnAssign* statement = deferred.statement;
            Ref<Label> notSet = g.newLabel();
            if (deferred.conditionalIndex >= 0) {
                Name name = names().conditionalAnnotations;
                Reg set = unit.newTemporary();
                unit.loc(statement->range);
                if (inClass)
                    g.emitPyLoadCell(set.get(), unit.cellFor(name), unit.nameConstant(name), UnboundKind::Free);
                else
                    g.emitPyLoadGlobal(set.get(), unit.nameConstant(name));
                g.emitPyCompare(set.get(), unit.constant(jsNumber(deferred.conditionalIndex)), set.get(), CompareOperator::In);
                g.emitJumpIfFalse(set.get(), notSet.get());
            }
            Reg value = unit.expr(statement->annotation);
            unit.loc(statement->range);
            g.emitPySetItem(annotations.get(), unit.nameConstant(unit.mangle(statement->target->as<NameExpr>()->id)), value.get());
            g.emitLabel(notSet.get());
        }
        g.emitReturn(annotations.get());
    });
    // The parameter is shown as "format", a name that the annotations themselves may use for something else.
    executable->pythonCodeInfo()->parameterNames[0] = c.identifier("format"_s);

    Reg function = newTemporary();
    emitMakeFunction(function.get(), executable, scope, { });
    storeName(c.name(inClass ? "__annotate_func__"_s : "__annotate__"_s), function.get(), range);
}

void Unit::compileModule(Mod* module)
{
    Range start { 1, 0, 1, 0 };
    if (module->kind == ModKind::Expression) {
        Reg value = expr(module->expression);
        g.emitReturn(value.get());
        return;
    }

    Seq<Stmt*> body = module->body;
    if (m_scope->hasConditionalAnnotations) {
        Reg set = newTemporary();
        callRuntime(set.get(), RuntimeFunction::BuildSet);
        g.emitPyStoreName(m_namespace, nameConstant(names().conditionalAnnotations), set.get());
    }
    if (c.m_future.annotations) {
        if (m_scope->usesAnnotations)
            callRuntimeForEffect(RuntimeFunction::SetupAnnotations, { m_namespace });
    } else {
        collectDeferredAnnotations(body, false);
        emitDeferredAnnotations(start);
    }

    unsigned first = 0;
    if (!c.m_isInteractive && m_scope->hasDocstring) {
        first = 1;
        String docstring = docstringOf(body);
        if (!docstring.isNull()) {
            noteConstant(g.addStringConstant(*c.name(docstring)));
            storeName(c.name("__doc__"_s), stringConstant(docstring), body[0]->range);
        }
    }
    statements(body, first);
    // All of a module with nothing in it is of line 0, which whoever traces it is told of: a debugger stops there.
    if (body.isEmpty())
        g.addPythonMark(PythonMark::Line, lineMarkValue(0, true));
    emitReturnNone();
}

void Unit::compileFunctionBody(Seq<Stmt*> body)
{
    statements(body, m_scope->hasDocstring ? 1 : 0);
    emitReturnNone();
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
