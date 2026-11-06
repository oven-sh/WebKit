/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCompiler.h"

#include "JSCInlines.h"
#include "PyCodeNode.h"
#include "PyCompilerUnit.h"
#include "PyHooks.h"
#include "PyRuntime.h"
#include "PyStr.h"
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

// `return`, `break` and `continue` that leave a `finally` clause are warned of for the whole tree before a single name is looked at, as
// in CPython: what the symbol table or the compiler then finds wrong comes after the warnings, wherever it is.
class ControlFlowInFinally {
public:
    ControlFlowInFinally(Compiler& compiler, VM& vm)
        : c(compiler)
        , m_vm(vm)
    {
    }

    void statements(Seq<Stmt*> body)
    {
        for (Stmt* statement : body)
            visit(statement);
    }

private:
    enum class Context : uint8_t { None, Finally, Function, Loop };

    void statements(Seq<Stmt*> body, Context context)
    {
        SetForScope scope(m_context, context);
        statements(body);
    }

    void visit(Stmt* node)
    {
        // The compiler says so when it gets there.
        if (!m_vm.isSafeToRecurseSoft()) [[unlikely]]
            return;
        switch (node->kind) {
        case StmtKind::FunctionDef:
        case StmtKind::AsyncFunctionDef:
            return statements(node->as<FunctionDef>()->body, Context::Function);
        case StmtKind::ClassDef:
            return statements(node->as<ClassDef>()->body);
        case StmtKind::For:
        case StmtKind::AsyncFor:
            statements(node->as<For>()->body, Context::Loop);
            return statements(node->as<For>()->orelse);
        case StmtKind::While:
            statements(node->as<While>()->body, Context::Loop);
            return statements(node->as<While>()->orelse);
        case StmtKind::If:
            statements(node->as<If>()->body);
            return statements(node->as<If>()->orelse);
        case StmtKind::With:
        case StmtKind::AsyncWith:
            return statements(node->as<With>()->body);
        case StmtKind::Match:
            for (MatchCase* matchCase : node->as<Match>()->cases)
                statements(matchCase->body);
            return;
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* statement = node->as<AST::Try>();
            statements(statement->body);
            for (ExceptHandler* handler : statement->handlers)
                statements(handler->body);
            statements(statement->orelse);
            return statements(statement->finalbody, Context::Finally);
        }
        case StmtKind::Return:
            if (m_context == Context::Finally)
                c.warn(node->range, "'return' in a 'finally' block"_s);
            return;
        case StmtKind::Break:
        case StmtKind::Continue:
            if (m_context == Context::Finally)
                c.warn(node->range, node->kind == StmtKind::Break ? "'break' in a 'finally' block"_s : "'continue' in a 'finally' block"_s);
            return;
        default:
            return;
        }
    }

    Compiler& c;
    VM& m_vm;
    Context m_context { Context::None };
};

UnlinkedFunctionExecutable* Compiler::compile()
{
    DeferGC deferGC(m_vm);

    m_future.annotations = m_options.futureAnnotations;
    m_future.barryAsBDFL = m_options.futureBarryAsBDFL;
    m_future.allowTopLevelAwait = m_options.allowTopLevelAwait;
    if (!analyzeFutureStatements(m_tree.root, m_future, m_error))
        return nullptr;

    if (m_tree.root->kind == ModKind::Module || m_tree.root->kind == ModKind::Interactive) {
        ControlFlowInFinally(*this, m_vm).statements(m_tree.root->body);
        if (failed())
            return nullptr;
    }

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

UnlinkedFunctionExecutable* Compiler::generate(const SourceCode& parentSource, const SourceCode& source, SourceParseMode parseMode, Ref<CodeInfo>&& info, unsigned parameterCount, bool isGeneratorBody, Function<void(BytecodeGenerator&)>&& emitter, unsigned* generatorSlotCount)
{
    ExecutableInfo executableInfo(false, PrivateBrandRequirement::None, false, ConstructorKind::None, JSParserScriptMode::Classic, SuperBinding::NotNeeded, parseMode, DerivedContextType::None, NeedsClassFieldInitializer::No, false, false, EvalContextType::None);
    auto* codeBlock = UnlinkedFunctionCodeBlock::create(m_vm, FunctionCode, executableInfo, { });
    ParserArena arena;
    PythonCodeNode node(arena, source, parameterCount, isGeneratorBody, [&](BytecodeGenerator& generator) {
        emitter(generator);
        if (failed())
            generator.abandonPythonCode();
    });
    ParserError error = BytecodeGenerator::generatePython(m_vm, &node, codeBlock);
    if (generatorSlotCount)
        *generatorSlotCount = node.generatorSlotCount();
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
    if (!parent)
        return compileCodeAnew(parent, spec, parentSource, emitBody);
    if (auto it = parent->m_compiledCode.find(spec.scope); it != parent->m_compiledCode.end())
        return it->value.executable;
    auto* executable = compileCodeAnew(parent, spec, parentSource, emitBody);
    parent->m_compiledCode.add(spec.scope, Unit::CompiledCode { executable, { }, 0 });
    return executable;
}

UnlinkedFunctionExecutable* Compiler::compileCodeAnew(Unit* parent, const CodeSpec& spec, const SourceCode& parentSource, const Function<void(Unit&)>& emitBody)
{
    Scope* scope = spec.scope;
    Ref<CodeInfo> info = CodeInfo::create();
    info->name = *spec.name;
    info->qualname = qualifiedName(parent, spec, *m_symbols);
    info->filename = m_filename;
    info->firstLineNumber = spec.firstLineNumber;
    info->docstring = spec.docstring;
    info->takesNamespace = !scope->isFunctionLike();
    info->sourceIsShown = m_options.sourceIsShown;
    info->isLambda = spec.kind == UnitKind::Lambda;

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
            unit.finishVariables();
        });
    }

    return generate(parentSource, source, SourceParseMode::MethodMode, info.copyRef(), parameterCount, false, [&](BytecodeGenerator& generator) {
        Unit binder(*this, parent, spec, generator, info.get(), source, Unit::Part::GeneratorBinder);
        binder.emitPrologue();
        unsigned slotCount = 0;
        auto* body = generate(source, source, SourceParseMode::GeneratorBodyMode, info.copyRef(), static_cast<unsigned>(JSGenerator::Argument::Frame), true, [&](BytecodeGenerator& bodyGenerator) {
            Unit unit(*this, parent, spec, bodyGenerator, info.get(), source, Unit::Part::GeneratorBody);
            unit.emitPrologue();
            emitBody(unit);
            unit.finishConstants();
            unit.finishVariables();
        }, &slotCount);
        unsigned index = generator.addPythonFunction(body);
        ASSERT_UNUSED(index, index == CodeInfo::generatorBodyIndex);
        binder.emitMakeGenerator(slotCount);
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

void Unit::line(const Range& range)
{
    if (range.lineno <= 0 || m_sameLineDepth)
        return;
    m_place = range;
    markLine(range.lineno);
}

void Unit::line(int lineno)
{
    if (lineno <= 0 || m_sameLineDepth)
        return;
    m_place = { };
    markLine(lineno);
}

void Unit::markLine(int lineno)
{
    // What follows a label may be reached from another line than the one before it.
    size_t position = g.instructions().size();
    size_t whereItBegins = std::exchange(m_whereLineBegins, notFound);
    if (whereItBegins < position && g.positionOfLastPythonLabel() <= whereItBegins && g.addPythonLineMarkAt(whereItBegins, lineMarkValue(lineno, false))) {
        m_markedLine = lineno;
        m_markedPosition = position;
        return;
    }
    if (lineno == m_markedLine && (position == m_markedPosition || g.positionOfLastPythonLabel() <= m_markedPosition))
        return;
    m_markedLine = lineno;
    m_markedPosition = position;
    g.addPythonMark(PythonMark::Line, lineMarkValue(lineno, position == m_statementStart));
}

void Unit::silentLine(const Range& range)
{
    if (m_sameLineDepth)
        return;
    m_markedLine = range.lineno;
    m_markedPosition = g.instructions().size();
    m_place = range;
    g.addPythonMark(PythonMark::Line, lineMarkValue(range.lineno, false, true));
}

void Unit::silentLine(int lineno)
{
    if (lineno <= 0)
        return;
    silentLine(Range { lineno, 0, lineno, 0 });
    m_place = { };
}

void Unit::isStillOnLine(const Range& range)
{
    if (m_sameLineDepth)
        return;
    m_markedLine = range.lineno;
    m_markedPosition = g.instructions().size();
    m_place = range;
}

void Unit::noLine()
{
    m_markedLine = -1;
    m_place = { };
    g.addPythonMark(PythonMark::NoLine, 0);
}

static bool endsRun(OpcodeID opcode)
{
    switch (opcode) {
    case op_jmp:
    case op_py_jmp_back:
    case op_ret:
    case op_py_ret:
    case op_py_generator_return:
    case op_throw:
    case op_unreachable:
        return true;
    default:
        return false;
    }
}

void Unit::jump(Label& target)
{
    // (`break` through a `finally` clause that returns. A jump would make it look as if the end of the loop could be reached.)
    if (cannotGetHere())
        return;
    if (&target == m_targetReplacedByCopy)
        return hiddenJump(target);
    if (m_place.lineno > 0 && m_place.lineno == m_markedLine)
        place(m_place);
    g.emitJump(target);
}

void Unit::hiddenJump(Label& target)
{
    g.addPythonMark(PythonMark::NoLine, hiddenJumpMark);
    g.emitJump(target);
}

// `takesInMore`: it stands for a jump to a jump.
void Unit::jump(Label& target, JumpIs jumpIs, bool takesInMore)
{
    if (cannotGetHere())
        return;
    // A jump that came of a constant is still a branch when CPython's compiler takes in what jumps lead to and first works out the lines. Later it goes where
    // this one goes, if this is still nothing but a jump, and the lines are worked out again; by then the jumps that lead nowhere have been dropped.
    bool isLedToByConstant = jumpOfConstantLeadsHere();
    auto lineno = m_targetsOfJumpsStillJumpedTo.contains(&target) || (isLedToByConstant && (takesInMore || jumpIs == JumpIs::Hidden)) ? std::nullopt : lineOfOnlyWayHere(JumpsHere::AreTakenIn);
    if (lineno) {
        silentLine(*lineno);
        if (!isLedToByConstant)
            g.addPythonMark(PythonMark::NoLine, jumpHasLineToItselfMark);
    }
    if (jumpIs == JumpIs::Hidden) {
        // It stands for a copy of what it leads to, which has its lines, behind what is left of the jump. Where ways meet it is of the line of one of them, so
        // that the line it leads to begins there.
        if (!lineno && m_markedLine == -1 && !m_sameLineDepth && g.positionOfLastPythonLabel() == g.instructions().size()) {
            auto& ways = g.branchesToLastPythonLabel().count ? g.branchesToLastPythonLabel() : g.jumpsToLastPythonLabel();
            if (ways.count)
                silentLine(g.pythonLineAt(ways.first));
        }
        return hiddenJump(target);
    }
    if (jumpIs != JumpIs::OfTheProgram)
        g.addPythonMark(PythonMark::NoLine, jumpIs == JumpIs::ToldOf ? jumpAsLaidOutMark : jumpForwardsAsLaidOutMark);
    jump(target);
}

// What comes next, if it is not a statement, is of the line of the jump or the branch that leads here, if that is the only way here.
// AreTakenIn: what comes next is a jump and nothing else, and what jumps to that has a copy of it in CPython.
std::optional<int> Unit::lineOfOnlyWayHere(JumpsHere jumpsHere)
{
    size_t position = g.instructions().size();
    // (Not behind a statement of which no instruction has come yet.)
    if (m_sameLineDepth || g.positionOfLastPythonLabel() != position || g.lineBeginsBehindLastPythonLabel())
        return std::nullopt;
    // What drops the iterator comes first, and is the only way if something else is jumped to, by a `break`.
    if (m_endOfForLoop.position == position)
        return m_endOfForLoop.hasBreak ? std::optional(m_endOfForLoop.lineno) : std::nullopt;
    auto& branches = g.branchesToLastPythonLabel();
    auto& jumps = g.jumpsToLastPythonLabel();
    // (The line after code that cannot be reached has been given an instruction, which cannot be reached either.)
    bool isFallenInto = !endsRun(g.opcodeOfLastInstruction()) && !(g.opcodeOfLastInstruction() == op_nop && m_endOfDeadCode + 1 == position);
    unsigned jumpCount = jumps.count;
    unsigned jump = jumps.first;
    if (jumpsHere == JumpsHere::AreTakenIn) {
        // All but those that stay.
        if (jumps.count > 2)
            return std::nullopt;
        bool firstStays = jumps.count && m_jumpsThatStay.contains(jumps.first);
        bool lastStays = jumps.count > 1 && m_jumpsThatStay.contains(jumps.last);
        jumpCount = firstStays + lastStays;
        jump = firstStays ? jumps.first : jumps.last;
    }
    if (branches.count + jumpCount != 1 || isFallenInto)
        return std::nullopt;
    int lineno = g.pythonLineAt(branches.count ? branches.first : jump);
    if (lineno < 0)
        return std::nullopt;
    return lineno;
}

// Where the statements end that a `try` or a `with` looks after, CPython's compiler has something that does nothing. It stays if it has a line of its own.
void Unit::endOfProtectedStatements(JumpsHere jumpsHere, Label* end)
{
    if (auto lineno = m_targetsOfJumpsStillJumpedTo.contains(end) || jumpOfConstantLeadsHere() ? std::nullopt : lineOfOnlyWayHere(jumpsHere)) {
        silentLine(*lineno);
        OpNop::emit(&g);
    }
}

// A function that comes to its end returns None, and that is of the line that was run last (f_lineno of the frame afterwards, what a
// 'return' event shows). CPython's compiler gets there by copying the return for each way that leads to it. So here: whatever would
// jump to the end of the last statement of a function returns there and then. Likewise for the jump at the end of the body of a loop:
// CPython's compiler has what would jump to it go where it goes, so that each such jump is of the line that was run last.
void Unit::leaveStatement(Label& end, Last isLast, JumpIs jumpIs)
{
    if (cannotGetHere())
        return;
    if (!isLast)
        return jump(end, isCopiedWhatComesAfterStatement() ? JumpIs::Hidden : jumpIs);
    Last leavingAs = isLast.inPlaceOfJump(jumpIs);
    unsigned jumpItStaysInFrontOf = 0;
    if (isLast.kind == Last::BeforeJump && !isReplacedByCopy(leavingAs, &jumpItStaysInFrontOf) && jumpItStaysInFrontOf == 1) {
        m_jumpsThatStay.append(g.instructions().size());
        return jump(end, jumpIs);
    }
    leave(leavingAs);
}

// Several ways meet there. What comes next, if it is not a statement, is of none of their lines. If a branch is the only way there, it is of the
// line of the branch.
void Unit::endOfStatement(Label& end, Last isLast, int lineOfBranch)
{
    m_targetsOfJumpsStillJumpedTo.removeAll(&end);
    if (!end.isBound())
        return;
    bool branchIsOnlyWay = isLast && end.unresolvedJumps().size() == 1 && cannotGetHere();
    g.emitLabel(end);
    if (branchIsOnlyWay) {
        m_whereLineOfBranchIsMarked = g.instructions().size();
        return isLast.kind == Last::BeforeJump ? silentLine(lineOfBranch) : line(lineOfBranch);
    }
    // (Not if a statement has begun here. What is marked may be the line of a branch that was the only way to the end of a statement in this one.)
    if (!m_sameLineDepth && m_markedLine != -1 && (m_markedPosition != g.instructions().size() || m_whereLineOfBranchIsMarked == g.instructions().size()))
        noLine();
}

// All the ways through what was just compiled have left it. (False may mean that it is not known.)
bool Unit::cannotGetHere()
{
    return g.positionOfLastPythonLabel() != g.instructions().size() && endsRun(g.opcodeOfLastInstruction());
}

// The statement may be in the body or in a handler of a `try` statement that is itself the last.
void Unit::emitImplicitReturn(Last isLast, const Range& range)
{
    ASSERT(isLast.kind == Last::OfFunction);
    SetForScope leavingAs(m_leavingAs, isLast);
    Vector<TryContext> suspended;
    unwindBlocks(range, isLast.blockCount, suspended);
    if (isLast.exception) {
        suspendTry(isLast.tryDepth, suspended);
        reraiseOutOfHandler(isLast.exception, isLast.previous);
    } else if (m_valueToReturn)
        g.emitReturn(m_valueToReturn);
    else if (m_endsOfClassBody)
        hiddenJump(endOfClassBodyForLine(m_markedLine));
    else
        emitReturnNone();
    resumeTry(WTF::move(suspended));
}

// What ends the body of a class is more than a return, and cannot be compiled before all of the body has been. There is one copy for each line
// that the body is left on, behind the body.
Label& Unit::endOfClassBodyForLine(int lineno)
{
    return labelForLine(*m_endsOfClassBody, lineno);
}

void Unit::emitImplicitContinue(unsigned loop, const Range& range)
{
    SetForScope leavingAs(m_leavingAs, Last { Last::OfLoopBody, loop });
    Vector<TryContext> suspended;
    unwindBlocks(range, loop + 1, suspended);
    suspendTry(m_blocks[loop].tryDepth, suspended);
    jump(*m_blocks[loop].continueTarget);
    resumeTry(WTF::move(suspended));
}

void Unit::emitImplicitJump(Last isLast, const Range& range)
{
    unsigned jumpItStaysInFrontOf = 0;
    bool isCopy = isReplacedByCopy(isLast, &jumpItStaysInFrontOf);
    bool staysAsInCPython = false;
    // It goes as far as the jump before that one goes.
    if (unsigned index = isLast.jumpsBefore - jumpItStaysInFrontOf + 1; jumpItStaysInFrontOf > 1 && isLast.targetsOfJumps[index]) {
        isLast.target = isLast.targetsOfJumps[index];
        isLast.blockCount = isLast.blockCountsOfJumps[index];
        isLast.tryDepth = isLast.tryDepthsOfJumps[index];
        staysAsInCPython = true;
    } else if (Label* target = jumpItStaysInFrontOf ? isLast.targetsOfJumps[isLast.jumpsBefore - jumpItStaysInFrontOf] : nullptr)
        m_targetsOfJumpsStillJumpedTo.append(target);
    SetForScope leavingAs(m_leavingAs, isLast);
    Vector<TryContext> suspended;
    unwindBlocks(range, isLast.blockCount, suspended);
    suspendTry(isLast.tryDepth, suspended);
    if (staysAsInCPython)
        m_jumpsThatStay.append(g.instructions().size());
    jump(*isLast.target, isCopy ? JumpIs::Hidden : isLast.jumpIs, isLast.jumpsBefore > (isLast.isInPlaceOfJump ? 0u : 1u));
    resumeTry(WTF::move(suspended));
}

void Unit::leave(Last isLast, const Range& range)
{
    switch (isLast.kind) {
    case Last::No:
        RELEASE_ASSERT_NOT_REACHED();
    case Last::OfFunction:
        return emitImplicitReturn(isLast, range);
    case Last::OfLoopBody:
        return emitImplicitContinue(isLast.loop, range);
    case Last::BeforeJump:
        return emitImplicitJump(isLast, range);
    }
}

// What comes after the statements that are about to be compiled is a jump to the label, unless they are the last of something already.
// `instructions`: how many CPython's compiler has for it, with the jump.
Last Unit::lastBeforeJump(Label& target, Last isLast, JumpIs jumpIs, unsigned instructions)
{
    if (isLast)
        return isLast.behindJump(jumpIs, instructions, &target, m_blocks.size(), g.tryDepth());
    Last result { Last::BeforeJump, 0, &target, static_cast<unsigned>(m_blocks.size()), static_cast<unsigned>(g.tryDepth()) };
    result.jumpIs = jumpIs;
    if (m_statementsAfter)
        result.instructionsAfterJump = instructionsUntilFunctionEnds(*m_statementsAfter, ComesFirst::Nothing);
    return result.behindJump(jumpIs, instructions, &target, m_blocks.size(), g.tryDepth());
}

// Whether a jump to the first of the jumps is. CPython's compiler goes through the code again and again. Each time, whatever ends with a jump takes
// in what the jump leads to, if that is of no line and ends with a jump itself, or is a short end of the function. What has taken in the end of the
// function is an end of the function, and may not be short any more.
// `jumpItStaysInFrontOf`: it is not, because one of the jumps, the first being 1, has taken in the end of the function and is not short. It leads there.
bool Unit::isReplacedByCopy(const Last& isLast, unsigned* jumpItStaysInFrontOf)
{
    ASSERT(isLast.kind == Last::BeforeJump);
    unsigned count = isLast.jumpsBefore;
    if (isLast.instructionsAfterJump >= tooManyToCopy || count > Last::maxJumps)
        return false;
    // The jump in question, the others, the end of the function.
    struct Run {
        unsigned size;
        unsigned target;
        bool endsFunction { false };
    };
    std::array<Run, Last::maxJumps + 2> runs;
    runs[0] = { 1, 1 };
    for (unsigned i = 0; i < count; ++i)
        runs[i + 1] = { isLast.instructionsOfJumps[count - 1 - i], i + 2 };
    runs[count + 1] = { isLast.instructionsAfterJump, 0, true };
    for (bool changed = true; std::exchange(changed, false);) {
        for (unsigned i = 0; i <= count; ++i) {
            Run& run = runs[i];
            if (run.endsFunction)
                continue;
            const Run& target = runs[run.target];
            if (target.endsFunction && target.size >= tooManyToCopy)
                continue;
            run = { run.size + target.size, target.target, target.endsFunction };
            changed = true;
        }
    }
    const Run& run = runs[isLast.isInPlaceOfJump || !count ? 0 : 1];
    if (run.endsFunction)
        return true;
    if (jumpItStaysInFrontOf && run.target <= count)
        *jumpItStaysInFrontOf = run.target;
    return false;
}

// Nothing is to be run on the way there.
bool Unit::canBranchTo(Last isLast)
{
    ASSERT(isLast.kind == Last::BeforeJump);
    // CPython's compiler has a branch to a jump go where the jump goes only if the jump is of that kind.
    if (isLast.jumpIs < JumpIs::ToldOf)
        return false;
    for (size_t i = isLast.blockCount; i < m_blocks.size(); ++i) {
        switch (m_blocks[i].kind) {
        case BlockKind::WhileLoop:
        case BlockKind::MatchSubject:
            break;
        default:
            return false;
        }
    }
    return true;
}

// Where the test of a statement goes if it fails, if a jump there is replaced by a copy of what is there: a chain of comparisons has one.
Label* Unit::targetReplacedByCopy(Label& end, Last isLast)
{
    if (!isLast)
        return isCopiedWhatComesAfterStatement() ? &end : nullptr;
    if (isLast.kind != Last::BeforeJump)
        return nullptr;
    if (!canBranchTo(isLast))
        return &end;
    return isReplacedByCopy(isLast) ? isLast.target : nullptr;
}

// jumpIf(test, target, false), for a statement after which, if the test fails and the statement is the last, the function ends. A test
// written over several lines fails on one of them, and that is the line the function returns on. In the body of a loop, CPython has no
// branches that go backwards: what would be one jumps over a jump.
void Unit::jumpIfOrLeave(Expr* test, Label& target, Last isLast, ReturnsByLine& returns, bool condition)
{
    if (isLast.kind == Last::OfLoopBody && !isLast.isBehindJump) {
        SetForScope branchTarget(m_branchTarget, BranchTarget { &target, { }, nullptr, isLast.loop, nullptr });
        return jumpIf(test, target, condition);
    }
    // Of a jump of the kind that tools are told of whichever way it goes, and what is run before it, each way there has a copy. At one of the other kind the ways meet.
    if (isLast.kind == Last::BeforeJump && canBranchTo(isLast))
        return jumpIf(test, *isLast.target, condition);
    // So they do where the end of the function has been copied behind it.
    if (isLast.kind == Last::BeforeJump && (isLast.jumpIs < JumpIs::ToldOf || isReplacedByCopy(isLast)))
        return jumpIf(test, target, condition);
    if (!isLast || (isLast.kind == Last::OfFunction && test->range.lineno == test->range.endLineno))
        return jumpIf(test, target, condition);
    SetForScope branchTarget(m_branchTarget, BranchTarget { &target, { }, &returns, { }, nullptr, isLast.kind != Last::OfFunction });
    jumpIf(test, target, condition);
}

Label& Unit::labelForLine(ReturnsByLine& labels, int lineno, const Range& place)
{
    size_t index = labels.findIf([&](auto& entry) {
        return entry.lineno == lineno && entry.place.colOffset == place.colOffset && entry.place.endLineno == place.endLineno && entry.place.endColOffset == place.endColOffset;
    });
    if (index == notFound) {
        index = labels.size();
        labels.append({ lineno, g.newLabel(), place });
    }
    return labels[index].label.get();
}

void Unit::emitReturnsByLine(ReturnsByLine& returns, Last isLast)
{
    for (auto& [lineno, label, placeOfBranch] : returns) {
        g.emitLabel(label.get());
        if (isLast.kind != Last::BeforeJump || isLast.jumpIs >= JumpIs::ToldOf)
            line(lineno);
        if (placeOfBranch.lineno)
            place(placeOfBranch);
        leave(isLast);
    }
}

// After the label that a branch goes to when it leaves the last statement of a function: it was on that line.
void Unit::lineOfImplicitReturn(int lineno, Last isLast)
{
    if (isLast)
        line(lineno);
}

void Unit::loc(const Range& range)
{
    line(range);
    place(range);
}

void Unit::place(const Range& range)
{
    unsigned sourceStart = m_source.startOffset();
    unsigned start = std::max(c.offsetOf(range.lineno, range.colOffset), sourceStart);
    unsigned end = std::max(c.offsetOf(range.endLineno, range.endColOffset), start);
    g.emitExpressionInfo(JSTextPosition(start), JSTextPosition(start), JSTextPosition(end));
}

// A name the code looks up somewhere other than among its own variables: one of co_names.
RegisterID* Unit::usedName(Name name)
{
    if (m_usedNames.add(name->impl()).isNewEntry)
        m_info.names.append(*name);
    return nameConstant(name);
}

// What tells a constant from every other: its type and what it is made of. False: it is the only one of its kind anyway.
// `isLikeNoOther`: there is a nan in it.
static bool appendKeyOfConstant(VM& vm, Vector<uint8_t, 64>& key, JSValue value, bool& isLikeNoOther)
{
    auto append = [&](auto number) {
        key.append(std::span { reinterpret_cast<const uint8_t*>(&number), sizeof(number) });
    };
    constexpr uint8_t isString = 0xfd;
    constexpr uint8_t isBigInt = 0xfe;
    constexpr uint8_t isItself = 0xff;
    auto appendItself = [&] {
        key.append(isItself);
        append(JSValue::encode(value));
    };
    if (!value || !value.isCell()) {
        isLikeNoOther |= value && value.isDouble() && std::isnan(value.asDouble());
        appendItself();
        return false;
    }
    JSCell* cell = value.asCell();
    if (auto* impl = cell->isString() ? asString(cell)->tryGetValueImpl() : nullptr) {
        key.append(isString);
        key.append(impl->is8Bit());
        append(impl->length());
        key.append(impl->is8Bit() ? asBytes(impl->span8()) : asBytes(impl->span16()));
        return true;
    }
    if (auto* bigInt = dynamicDowncast<JSBigInt>(cell)) {
        key.append(isBigInt);
        key.append(bigInt->sign());
        append(bigInt->length());
        for (unsigned i = 0; i < bigInt->length(); ++i)
            append(bigInt->digit(i));
        return true;
    }
    ConstantParts parts;
    if (!isPyObjectType(cell->type()) || !vm.isSafeToRecurse() || !constantParts(cell, parts) || parts.kind == ConstantParts::Kind::Ellipsis) {
        appendItself();
        return false;
    }
    isLikeNoOther |= std::isnan(parts.numbers[0]) || std::isnan(parts.numbers[1]);
    key.append(static_cast<uint8_t>(parts.kind));
    append(std::bit_cast<uint64_t>(parts.numbers[0]));
    append(std::bit_cast<uint64_t>(parts.numbers[1]));
    append(parts.bytes.size());
    key.append(parts.bytes);
    append(parts.items.size());
    for (JSValue item : parts.items)
        appendKeyOfConstant(vm, key, item, isLikeNoOther);
    return true;
}

JSCell* Unit::merged(JSCell* cell)
{
    if (cell->isString()) {
        // All but what the compiler has worked out: 'a' + 'b'.
        auto* impl = asString(cell)->tryGetValueImpl();
        if (impl && impl->isAtom()) [[likely]]
            return cell;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
        String string = asString(cell)->value(globalObject());
        return scope.exception() ? cell : g.addStringConstant(*c.name(string));
    }
    // It has been here before. (The key is as long as the constant.)
    if (g.hasPythonConstant(cell))
        return cell;
    Vector<uint8_t, 64> key;
    bool isLikeNoOther = false;
    if (!appendKeyOfConstant(vm(), key, cell, isLikeNoOther) || isLikeNoOther)
        return cell;
    return m_mergedConstants.add(String(byteCast<Latin1Character>(key.span())), cell).iterator->value;
}

// co_consts has the constants in the order in which the code first needs them. CPython then works out what it can from them,
// and what that gives comes after all the others.
void Unit::noteConstant(JSValue value, bool isUsed)
{
    noteConstant({ constant(value)->virtualRegister(), 0, isUsed });
}

void Unit::noteConstant(const CodeInfo::Constant& noted)
{
    bool isUsed = noted.isUsed && !m_cannotBeReached;
    auto result = m_indexOfConstant.add(noted.reg.offset(), m_info.constants.size());
    if (result.isNewEntry)
        m_info.constants.append({ noted.reg, noted.functionIndex, isUsed });
    else
        m_info.constants[result.iterator->value].isUsed |= isUsed;
}

void Unit::noteFoldedConstant(Expr* node, JSValue value)
{
    if (node->is<Constant>()) {
        noteConstant(value);
        return;
    }
    noteWhatConstantIsMadeOf(node);
    noteConstantMade(value);
}

// It is not needed any more. It has its place among the constants all the same, which counts if it is the first of all, which stays, or
// if something turns out to need it.
void Unit::noteWhatConstantIsMadeOf(Expr* node)
{
    if (!isSafeToRecurse())
        return;
    Seq<Expr*> items;
    if (auto* literal = node->tryAs<Constant>())
        return noteConstant(constantValue(literal->value), false);
    if (auto* name = node->tryAs<NameExpr>(); name && name->id->impl() == names().debug->impl())
        return noteConstant(jsBoolean(!c.m_options.optimize), false);
    if (auto* unary = node->tryAs<UnaryOp>())
        return noteWhatConstantIsMadeOf(unary->operand);
    if (auto* subscript = node->tryAs<Subscript>()) {
        noteWhatConstantIsMadeOf(subscript->value);
        return noteWhatConstantIsMadeOf(subscript->slice);
    }
    if (auto* slice = node->tryAs<Slice>())
        return noteConstant(constantSlice(slice), false);
    if (node->is<BinOp>()) {
        // 1 + 1 + 1... nests to the left, as deep as it is long.
        Vector<BinOp*, 8> operations;
        for (; node->is<BinOp>(); node = node->as<BinOp>()->left)
            operations.append(node->as<BinOp>());
        noteWhatConstantIsMadeOf(node);
        while (!operations.isEmpty())
            noteWhatConstantIsMadeOf(operations.takeLast()->right);
        return;
    }
    if (auto* tuple = node->tryAs<Tuple>())
        items = tuple->elts;
    else if (auto* list = node->tryAs<List>())
        items = list->elts;
    else if (auto* set = node->tryAs<Set>())
        items = set->elts;
    for (Expr* item : items)
        noteWhatConstantIsMadeOf(item);
}

void Unit::noteConstantMade(JSValue value)
{
    if (m_cannotBeReached)
        return;
    m_foldedConstants.append({ constant(value)->virtualRegister(), 0, true });
}


void Unit::finishConstants()
{
    for (auto& folded : m_foldedConstants)
        noteConstant(folded);
    m_foldedConstants.clear();
}

RegisterID* Unit::nameConstant(Name name)
{
    return g.pythonConstant(g.addStringConstant(*name));
}

RegisterID* Unit::nameAsValue(Name name)
{
    RegisterID* result = nameConstant(name);
    noteConstant({ result->virtualRegister(), 0, true });
    return result;
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
    // One operand is a run of registers wherever it is, among the constants too.
    if (operands.size() == 1)
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
    if (scope->readsClassDictCellAnyway())
        result.append(names().classDict);
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
        if (Symbol* symbol = m_scope->find(names().classCell->impl()); m_scope->needsClassClosure && symbol && symbol->binding == Binding::Free) {
            Variable variable { g.addVar(), Variable::Kind::Cell, true };
            m_classCellOfMethods = variable.reg;
            recordVariable(names().classCell, variable, false);
        } else if (m_scope->needsClassClosure)
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
            Variable variable { parameterRegisters[i], Variable::Kind::Fast, !!(symbol->flags & Deleted), !!(symbol->flags & AssignedInExpression) };
            map->add(name->impl(), variable);
            recordVariable(name, variable, false);
        }
        for (auto& symbol : m_scope->symbols) {
            if (symbol.binding == Binding::Local) {
                if (symbol.flags & DefParam)
                    continue;
                Variable variable { g.addVar(), Variable::Kind::Fast, true, !!(symbol.flags & AssignedInExpression) };
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
            g.emitPyRestoreRegisters(first, count, 0);
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
            else {
                size_t parameter = m_cellParameters.findIf([&](auto& parameter) { return *parameter.name == variable.name; });
                callRuntime(reg, RuntimeFunction::MakeCell, { parameter != notFound ? m_cellParameters[parameter].reg : emptyValue() });
            }
            break;
        case CodeInfo::VariableKind::Free:
            callRuntime(reg, RuntimeFunction::LoadClosure, { constant(jsNumber(closureIndex++)) });
            break;
        }
    }

    if (m_part == Part::GeneratorBody) {
        // throw() into one that has not started: it starts, and this is as far as it gets. BytecodeGeneratorification puts the switch of
        // the generator in the place of the jump, so that one which is started in the usual way runs nothing for this.
        Ref<Label> start = g.newLabel();
        g.pythonGeneratorStartsHere();
        g.emitPyJumpIfFalse(g.generatorResumeModeRegister(), start.get());
        callRuntimeForEffect(RuntimeFunction::ThrowIntoGenerator, { g.generatorValueRegister() });
        // In CPython all of a generator is in a handler, of no line, which an exception that leaves it comes through. Tools are told so,
        // and this is the place they are given.
        noLine();
        g.emitUnreachable();
        g.emitLabel(start.get());
    }

    g.addPythonMark(PythonMark::Start, 0);
    m_markedLine = -1;
    m_start = g.instructions().size();
}

// Their order is what co_varnames, locals(), f_locals and co_cellvars show. In CPython that is: the parameters; the other variables that no inner
// function uses, those of inlined comprehensions among them, in the order in which its compiler comes to emit code for them, and none
// that it emits no code for (`x: int`); those that an inner function uses, by name; those of outer functions, by name.
void Unit::finishVariables()
{
    UncheckedKeyHashMap<UniquedStringImpl*, size_t> indexOfParameter;
    for (size_t index = 0; index < m_info.parameterNames.size(); ++index)
        indexOfParameter.add(m_info.parameterNames[index].impl(), index);
    UncheckedKeyHashMap<int, unsigned> firstUse;
    for (auto& map : m_variables.values()) {
        for (auto& variable : map->values()) {
            if (variable.firstUse)
                firstUse.add(variable.reg->virtualRegister().offset(), variable.firstUse);
        }
    }

    m_info.variables.removeAllMatching([&](auto& variable) {
        if (variable.isHidden || variable.kind != CodeInfo::VariableKind::Local || firstUse.contains(variable.reg.offset()) || indexOfParameter.contains(variable.name.impl()))
            return false;
        Symbol* symbol = m_scope->find(variable.name.impl());
        return symbol && (symbol->flags & DefAnnot);
    });

    // There a variable of an inlined comprehension and a variable of the same name are one.
    UncheckedKeyHashMap<UniquedStringImpl*, unsigned> firstUseOfName;
    for (auto& variable : m_info.variables) {
        auto use = firstUse.find(variable.reg.offset());
        if (variable.kind != CodeInfo::VariableKind::Local || use == firstUse.end())
            continue;
        auto result = firstUseOfName.add(variable.name.impl(), use->value);
        result.iterator->value = std::min(result.iterator->value, use->value);
    }

    size_t parameterCount = m_info.parameterNames.size();
    auto rank = [&](const CodeInfo::Variable& variable) -> size_t {
        if (!variable.isHidden) {
            auto parameter = indexOfParameter.find(variable.name.impl());
            if (parameter != indexOfParameter.end())
                return parameter->value;
        }
        if (variable.kind == CodeInfo::VariableKind::Local) {
            auto use = firstUseOfName.find(variable.name.impl());
            return parameterCount + (use != firstUseOfName.end() ? use->value : m_usedVariableCount + 1);
        }
        if (variable.isHidden)
            return std::numeric_limits<size_t>::max();
        return parameterCount + m_usedVariableCount + 2 + static_cast<size_t>(variable.kind);
    };
    std::stable_sort(m_info.variables.begin(), m_info.variables.end(), [&](auto& a, auto& b) {
        size_t rankOfA = rank(a);
        size_t rankOfB = rank(b);
        if (rankOfA != rankOfB)
            return rankOfA < rankOfB;
        return !a.isHidden && a.kind != CodeInfo::VariableKind::Local && codePointCompareLessThan(a.name.string(), b.name.string());
    });
}

bool Unit::isParameterRegister(RegisterID* reg)
{
    if (m_part != Part::GeneratorBody || !isFunctionLike())
        return false;
    int local = reg->virtualRegister().toLocal();
    int first = g.firstPythonVariable();
    return local >= first && local < first + static_cast<int>(m_scope->parameters.size());
}

void Unit::emitMakeGenerator(unsigned slotCount)
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
            callRuntime(iterator.get(), RuntimeFunction::GetAIterOfLoop, { parameter });
        else
            g.emitPyGetIter(iterator.get(), parameter);
        callRuntime(result.get(), RuntimeFunction::MakeGenerator, { constant(jsNumber(slotCount)), iterator.get() });
    } else
        callRuntime(result.get(), RuntimeFunction::MakeGenerator, { constant(jsNumber(slotCount)) });
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
    if (m_classCellOfMethods && impl == names().classCell->impl())
        return m_classCellOfMethods;
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
        noteUse(*access.variable);
        if (access.variable->mayBeUnbound && !isKnownToBeBound(*access.variable)) {
            loc(range);
            g.emitTDZCheck(reg);
            noteBound(*access.variable);
        }
        if (dst)
            return g.move(dst, reg);
        // What is read now must not change if the variable is assigned to before the value is used.
        if (access.variable->isAssignedInExpression)
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
        noteUse(*access.variable);
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

Unit::Try Unit::beginTry(HandlerType type)
{
    size_t depth = g.tryDepth();
    Ref<Label> handler = g.newLabel();
    Ref<Label> start = g.newEmittedPythonRangeLabel();
    TryData* data = g.pushTry(start.get(), handler.get(), type);
    return { data, WTF::move(handler), depth };
}

void Unit::endTry(Try& tryRange)
{
    Ref<Label> end = g.newEmittedPythonRangeLabel();
    g.popTry(tryRange.data, end.get());
}

Unit::Reg Unit::beginHandler(Try& tryRange, Reg* previous, bool isOfLine)
{
    g.settlePythonLineMark();
    g.emitLabel(tryRange.handler.get());
    noLine();
    Reg thrown = newTemporary();
    if (previous)
        *previous = newTemporary();
    g.emitOutOfLinePyCatchHandler(thrown.get(), previous ? previous->get() : nullptr, tryRange.data, isOfLine);
    return thrown;
}

Unit::Reg Unit::beginExceptionHandler(Try& tryRange, Reg& previous, bool isOfLine)
{
    return beginHandler(tryRange, &previous, isOfLine);
}

void Unit::restoreHandledException(RegisterID* previous)
{
    callRuntimeForEffect(RuntimeFunction::LeaveHandler, { previous });
}

void Unit::reraise(RegisterID* exception)
{
    callRuntimeForEffect(RuntimeFunction::Reraise, { exception });
}

// What the compiler itself does at the end of a handler when the exception is to go on. It stands outside the range of the handler's
// cleanup, which would do no more than this: catch it, put back what was being handled, and raise it a third time.
void Unit::reraiseOutOfHandler(RegisterID* exception, RegisterID* previous)
{
    // beginExceptionHandler() made them one after the other.
    if (previous->index() == exception->index() - 1) {
        Reg ignored = newTemporary();
        g.emitPyRuntime(ignored.get(), RuntimeFunction::ReraiseOutOfHandler, exception, 2);
        return;
    }
    callRuntimeForEffect(RuntimeFunction::ReraiseOutOfHandler, { exception, previous });
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
    m_info.hasLoopThatOwnsItsIterator |= block.ownsIterator;
    if (block.breakTarget && m_statementsAfter)
        block.instructionsAfterLoop = instructionsUntilFunctionEnds(*m_statementsAfter, ComesFirst::Nothing);
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
    if (!block.isMarked)
        return;
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
    g.addPythonMark(begins ? PythonMark::BlockBegin : PythonMark::BlockEnd, block.number, TracedBlock::encode(kind, block.saved ? block.saved->virtualRegister().offset() : VirtualRegister().offset(), block.ownsIterator));
}

void Unit::unwindBlock(const Block& block, Vector<TryContext>& suspended)
{
    suspendTry(block.tryDepth, suspended);
    switch (block.kind) {
    case BlockKind::ForLoop:
        if (block.ownsIterator)
            callRuntimeForEffect(RuntimeFunction::ReleaseIterator, { block.saved });
        return;
    case BlockKind::WhileLoop:
    case BlockKind::TryExcept:
    case BlockKind::ExceptionHandler:
    case BlockKind::ExceptionGroupHandler:
        return;
    case BlockKind::AsyncForLoop:
        leaveAsyncFor(block.saved, nullptr);
        return;
    case BlockKind::InlinedComprehension:
    case BlockKind::MatchSubject:
        return;
    case BlockKind::FinallyTry: {
        // A `return` that runs the clause returns on the line that ran last, like a function that comes to its end: each way out of
        // the clause gets a copy of what is left to do. Not if that has lines of its own, another clause or the end of a `with`: then
        // the ways meet, as in CPython, or clauses in clauses would be copied once for each way through all of them.
        unsigned stays = m_leavingAs.kind == Last::OfLoopBody ? m_leavingAs.loop + 1 : m_leavingAs.blockCount;
        bool restHasLines = m_leavingAs && std::ranges::any_of(m_blocks.subspan(std::min<size_t>(stays, m_blocks.size())), [](const Block& other) {
            return other.kind == BlockKind::FinallyTry || other.kind == BlockKind::With || other.kind == BlockKind::AsyncWith;
        });
        Last isLast = m_leavingAs.onlyIf(!restHasLines);
        // What else there is to do comes before the jump, with which the clause is left. (Nothing: a branch to a jump back is turned round.)
        unsigned rest = instructionsOnTheWayOut(false, std::min<size_t>(stays, m_blocks.size()), m_blocks.size());
        if (isLast.kind == Last::BeforeJump || (isLast.kind == Last::OfLoopBody && rest))
            isLast = isLast.behindJump(isLast.jumpIs, 1 + rest, nullptr);
        statements(block.finalBody, 0, isLast);
        return;
    }
    case BlockKind::FinallyEnd:
        restoreHandledException(block.saved);
        return;
    case BlockKind::With:
    case BlockKind::AsyncWith: {
        Reg result = newTemporary();
        RegisterID* none = constant(jsUndefined());
        noteConstant(jsUndefined());
        emitMethodCall(result.get(), block.saved, block.savedSelf, { none, none, none }, block.range);
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

// Leaves all but the first `count` blocks.
void Unit::unwindBlocks(const Range& range, size_t count, Vector<TryContext>& suspended)
{
    if (m_blocks.size() <= count)
        return;
    Block top = m_blocks.last();
    if (top.kind == BlockKind::ExceptionGroupHandler) {
        fail(range, "'break', 'continue' and 'return' cannot appear in an except* block"_s);
        return;
    }
    // What the block runs on the way out is outside the block.
    markBlock(top, false);
    m_blocks.removeLast();
    // Behind a `finally` clause that never comes to its end, all there is to do is to look at what else is in the way.
    if (!cannotGetHere())
        unwindBlock(top, suspended);
    unwindBlocks(range, count, suspended);
    markBlock(top, true);
    m_blocks.append(top);
}

// ---- Statements ----

void Unit::statements(Seq<Stmt*> body, unsigned first, Last isLast)
{
    StatementsAfter after { body, first, isLast, static_cast<unsigned>(m_blocks.size()), m_statementsAfter, std::exchange(m_statementsEndWhereStatementEnds, false) };
    SetForScope statementsAfter(m_statementsAfter, &after);
    for (unsigned i = first; i < body.size(); ++i) {
        // After a `return`, say, or at the very beginning: what `if a and 0:` is in front of.
        if (!m_sameLineDepth && cannotGetHere())
            return deadStatements(body, i);
        m_statementIsLast = isLast.onlyIf(i + 1 == body.size());
        m_statement = body[i];
        after.first = i + 1;
        statement(body[i]);
    }
}

// CPython's compiler drops what cannot be reached, so that the code has no such lines: nobody can jump to them or find that they were
// never run, and has no constants that only they need. Here it stays, is of no line, and its constants count as not needed. There is no
// getting past it either: see cannotGetHere().
void Unit::deadStatements(Seq<Stmt*> body, unsigned first)
{
    if (first >= body.size())
        return;
    if (!m_sameLineDepth)
        noLine();
    SameLine sameLine(*this);
    SetForScope cannotBeReached(m_cannotBeReached, true);
    statements(body, first);
    g.emitUnreachable();
}

void Unit::deadExpr(Expr* node)
{
    deadCode([&] {
        expr(node);
    });
}

void Unit::statement(Stmt* node)
{
    if (!isSafeToRecurse())
        return;

    Last isLast = std::exchange(m_statementIsLast, { });
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
        return forLoop(node->as<For>(), isLast);
    case StmtKind::AsyncFor:
        return asyncForLoop(node->as<For>(), isLast);
    case StmtKind::While:
        return whileLoop(node->as<While>(), isLast);
    case StmtKind::If:
        return ifStatement(node->as<If>(), isLast);
    case StmtKind::With:
    case StmtKind::AsyncWith:
        return withStatement(node->as<With>(), 0, isLast);
    case StmtKind::Match:
        return matchStatement(node->as<Match>(), isLast);
    case StmtKind::Raise:
        return raiseStatement(node->as<Raise>());
    case StmtKind::Try:
    case StmtKind::TryStar:
        return tryStatement(node->as<AST::Try>(), isLast);
    case StmtKind::Assert:
        return assertStatement(node->as<Assert>(), isLast);
    case StmtKind::Import:
        return importStatement(node->as<Import>());
    case StmtKind::ImportFrom:
        return importFrom(node->as<ImportFrom>());
    case StmtKind::Global:
    case StmtKind::Nonlocal:
        return;
    case StmtKind::Pass:
        return line(node->range);
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
                line(target->range);
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
                // If all the targets are variables in registers, nothing of the program runs between the last value and the last store. Then
                // a variable that is no target can stay where it is, and the last value can be computed where it goes, unless
                // an earlier target is the same variable. (On several lines, a tool that is told of the lines sees the variables assigned to one by one.)
                unsigned count = targets->elts.size();
                Vector<Variable*, 8> variables;
                for (Expr* target : targets->elts) {
                    auto* name = target->tryAs<NameExpr>();
                    if (!name)
                        break;
                    Access access = resolve(mangle(name->id));
                    if (access.kind != Access::Kind::Fast)
                        break;
                    variables.append(access.variable);
                }
                bool areVariables = count && variables.size() == count;
                bool lastIsComputedInPlace = areVariables && variables.find(variables.last()) == count - 1 && node->range.lineno == node->range.endLineno;
                Vector<Reg, 8> evaluated;
                for (unsigned i = 0; i < count; ++i) {
                    Expr* item = values->elts[i];
                    auto* name = item->tryAs<NameExpr>();
                    if (item->is<Constant>() || (name && areVariables && !variables.contains(resolve(mangle(name->id)).variable)))
                        evaluated.append(expr(item));
                    else if (lastIsComputedInPlace && i + 1 == count)
                        evaluated.append(expr(item, variables.last()->reg));
                    else {
                        Reg temporary = newTemporary();
                        expr(item, temporary.get());
                        evaluated.append(WTF::move(temporary));
                    }
                }
                // Where CPython has what is left of making the tuple, and of taking it apart.
                line(values->range);
                line(targets->range);
                for (unsigned i = 0; i < count; ++i)
                    store(targets->elts[i], evaluated[i].get());
                return;
            }
        }
    }
    unsigned storedTemporary = 0;
    // variable[key] = f(): the store is the one use of the value. The value comes first, so what it is stored in can only be known
    // when it is made if nothing of the program runs in between.
    if (auto* target = node->targets.size() == 1 ? node->targets[0]->tryAs<Subscript>() : nullptr; target && makesTemporary(node->value)) {
        auto isAtHand = [&](Expr* operand) {
            if (!operand || operand->is<Constant>())
                return true;
            auto* name = operand->tryAs<NameExpr>();
            return name && resolve(mangle(name->id)).kind == Access::Kind::Fast;
        };
        auto* slice = target->slice->tryAs<Slice>();
        if (target->value->is<NameExpr>() && isAtHand(target->value) && (slice ? isAtHand(slice->lower) && isAtHand(slice->upper) && isAtHand(slice->step) : isAtHand(target->slice)))
            storedTemporary = beginTemporary(node->value);
    }
    // a, b = g(): to be taken apart is the one use of the value.
    unsigned unpackedTemporary = 0;
    if (node->targets.size() == 1 && (node->targets[0]->is<Tuple>() || node->targets[0]->is<List>()))
        unpackedTemporary = beginTemporaryOfLoop(node->value);
    Reg value = expr(node->value);
    // (Not before: the value may have targets of its own, those of a comprehension.)
    m_storedTemporary = storedTemporary;
    m_unpackedTemporary = unpackedTemporary;
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
        line(name->range);
        Reg left = loadName(name->id, nullptr, name->range);
        Reg right = expr(node->value);
        Access access = resolve(mangle(name->id));
        loc(node->range);
        if (access.kind == Access::Kind::Fast) {
            g.emitPyBinary(access.variable->reg, left.get(), right.get(), op);
            line(name->range);
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
        loc(rangeOfAttributeName(attribute));
        g.emitPyGetAttr(left.get(), base.get(), usedName(mangle(attribute->attr)));
        Reg right = expr(node->value);
        loc(node->range);
        g.emitPyBinary(left.get(), left.get(), right.get(), op);
        loc(rangeOfAttributeName(attribute));
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
        // As in assign().
        auto* name = target->tryAs<NameExpr>();
        Access access = name ? resolve(mangle(name->id)) : Access { Access::Kind::Name };
        Reg value = expr(node->value, access.kind == Access::Kind::Fast ? access.variable->reg : nullptr);
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

void Unit::ifStatement(If* node, Last isLast)
{
    Ref<Label> end = g.newLabel();
    if (node->orelse.isEmpty()) {
        ReturnsByLine returns;
        {
            SetForScope replacedByCopy(m_targetReplacedByCopy, targetReplacedByCopy(end.get(), isLast));
            jumpIfOrLeave(node->test, end.get(), isLast, returns);
        }
        int lineOfBranch = m_markedLine;
        m_statementsEndWhereStatementEnds = true;
        statements(node->body, 0, isLast);
        if ((isLast.kind == Last::OfFunction || !returns.isEmpty()) && !cannotGetHere())
            leave(isLast);
        emitReturnsByLine(returns, isLast);
        if (end->isBound()) {
            endOfStatement(end.get(), isLast, lineOfBranch);
            lineOfImplicitReturn(node->test->range.endLineno, isLast.onlyIf(isLast.kind == Last::OfFunction));
        }
        return;
    }
    Ref<Label> orelse = g.newLabel();
    jumpIf(node->test, orelse.get(), false);
    statements(node->body, 0, lastBeforeJump(end.get(), isLast, JumpIs::ToldOfForwards));
    leaveStatement(end.get(), isLast, JumpIs::ToldOfForwards);
    if (orelse->isBound())
        g.emitLabel(orelse.get());
    m_statementsEndWhereStatementEnds = true;
    statements(node->orelse, 0, isLast);
    endOfStatement(end.get());
}

void Unit::whileLoop(While* node, Last isLast)
{
    Ref<Label> loop = g.newLabel();
    Ref<Label> anchor = g.newLabel();
    Ref<Label> end = g.newLabel();

    g.settlePythonLineMark();
    g.emitLabel(loop.get());
    m_statementStart = g.instructions().size();
    pushBlock({ .kind = BlockKind::WhileLoop, .tryDepth = g.tryDepth(), .continueTarget = loop.ptr(), .breakTarget = end.ptr(), .isLast = isLast, .valueToReturn = m_valueToReturn });
    ReturnsByLine returns;
    {
        SetForScope replacedByCopy(m_targetReplacedByCopy, node->orelse.isEmpty() ? targetReplacedByCopy(anchor.get(), isLast) : nullptr);
        jumpIfOrLeave(node->test, anchor.get(), isLast.onlyIf(node->orelse.isEmpty()), returns);
    }
    int lineOfBranch = m_markedLine;
    statements(node->body, 0, { Last::OfLoopBody, static_cast<unsigned>(m_blocks.size() - 1) });
    jump(loop.get());
    popBlock();
    emitReturnsByLine(returns, isLast);

    if (anchor->isBound()) {
        bool isOnlyWay = anchor->unresolvedJumps().size() == 1;
        g.emitLabel(anchor.get());
        if (isLast.kind == Last::BeforeJump) {
            if (node->orelse.isEmpty() && isOnlyWay)
                silentLine(lineOfBranch);
        } else
            lineOfImplicitReturn(isLast.kind == Last::OfFunction ? node->test->range.endLineno : lineOfBranch, isLast.onlyIf(node->orelse.isEmpty() && (isLast.kind == Last::OfFunction || isOnlyWay)));
    }
    statements(node->orelse, 0, isLast);
    endOfStatement(end.get());
    // `while True:` has a branch to here, until CPython's compiler finds out.
    if (node->orelse.isEmpty() && constantTruth(node->test) == true)
        m_whereBranchOfConstantLeads = g.instructions().size();
}

void Unit::forLoop(For* node, Last isLast)
{
    Ref<Label> start = g.newLabel();
    Ref<Label> cleanup = g.newLabel();
    Ref<Label> end = g.newLabel();

    Reg iterator = newTemporary();
    unsigned temporary = beginTemporaryOfLoop(node->iter);
    {
        Reg iterable = exprToIterateOverOrSearch(node->iter);
        loc(node->iter->range);
        markTemporaryUsed(temporary, TemporaryUse::ForHeader, nullptr, true);
        g.emitPyGetIter(iterator.get(), iterable.get());
    }

    pushBlock({ .kind = BlockKind::ForLoop, .tryDepth = g.tryDepth(), .continueTarget = start.ptr(), .breakTarget = end.ptr(), .saved = iterator.get(), .isLast = isLast, .valueToReturn = m_valueToReturn, .ownsIterator = !!temporary });
    g.emitLabel(start.get());
    line(node->iter->range);
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
    } else
        line(node->target->range);
    statements(node->body, 0, { Last::OfLoopBody, static_cast<unsigned>(m_blocks.size() - 1) });
    if (!cannotGetHere())
        jump(start.get());
    bool hasBreak = m_blocks.last().hasBreak;
    popBlock();

    // Where CPython drops the iterator.
    g.emitLabel(cleanup.get());
    silentLine(node->iter->range);
    // A `break` leads to what comes after that, be it one that cannot be reached, or one that goes on from here. See lineOfOnlyWayHere().
    if (node->orelse.isEmpty())
        m_endOfForLoop = { g.instructions().size(), node->iter->range.lineno, hasBreak && !end->isBound() };
    statements(node->orelse, 0, isLast);
    endOfStatement(end.get());
}

void Unit::asyncForLoop(For* node, Last isLast)
{
    Ref<Label> start = g.newLabel();
    Ref<Label> end = g.newLabel();

    Reg iterator = newTemporary();
    {
        Reg iterable = expr(node->iter);
        loc(node->iter->range);
        callRuntime(iterator.get(), RuntimeFunction::GetAIterOfLoop, { iterable.get() });
    }

    Try whole = beginTry(HandlerType::SynthesizedCatch);
    g.emitLabel(start.get());
    line(node->iter->range);
    pushBlock({ .kind = BlockKind::AsyncForLoop, .tryDepth = whole.depthBefore, .continueTarget = start.ptr(), .breakTarget = end.ptr(), .saved = iterator.get(), .isLast = isLast, .valueToReturn = m_valueToReturn });

    // StopAsyncIteration from __anext__ ends the loop.
    Try guard = beginTry();
    Reg item = newTemporary();
    loc(node->range);
    callRuntime(item.get(), RuntimeFunction::GetANext, { iterator.get() });
    emitDelegate(item.get(), item.get());
    endTry(guard);
    g.emitPyAsyncForMark(asyncForGoesOnMark);

    store(node->target, item.get());
    item = nullptr;
    statements(node->body, 0, { Last::OfLoopBody, static_cast<unsigned>(m_blocks.size() - 1) });
    if (!cannotGetHere())
        jump(start.get());
    popBlock();

    Reg thrown = beginHandler(guard, nullptr, true);
    // The end of the loop is not told of as a line.
    silentLine(node->iter->range);
    g.emitPyAsyncForMark(asyncForEndsMark);
    loc(node->iter->range);
    callRuntimeForEffect(RuntimeFunction::EndAsyncFor, { thrown.get(), iterator.get() });
    thrown = nullptr;
    endTry(whole);
    statements(node->orelse, 0, isLast);
    leaveStatement(end.get(), isLast, JumpIs::Hidden);

    asyncForHandler(whole, iterator.get());
    endOfStatement(end.get());
}

// See getAIterOfLoop(). For all but an iterator of JS's there is nothing to await, and nothing is.
void Unit::leaveAsyncFor(RegisterID* iterator, RegisterID* exception)
{
    Ref<Label> done = g.newLabel();
    Reg closing = newTemporary();
    callRuntime(closing.get(), RuntimeFunction::LeaveAsyncFor, { iterator, exception ? exception : emptyValue() });
    g.addPythonMark(PythonMark::NoLine, hiddenJumpMark);
    g.emitPyJumpIfNone(closing.get(), done.get());
    emitAwait(closing.get(), closing.get(), 0);
    g.emitLabel(done.get());
}

void Unit::asyncForHandler(Try& whole, RegisterID* iterator)
{
    Reg previous;
    Reg exception = beginExceptionHandler(whole, previous);
    Try cleanup = beginTry();
    leaveAsyncFor(iterator, exception.get());
    endTry(cleanup);
    // It goes on as if from where it was raised.
    noLine();
    callRuntimeForEffect(RuntimeFunction::ReraiseUnseen, { exception.get(), previous.get() });

    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());
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
    Reg value;
    // CPython's compiler loads a constant when all that is on the way out is done, which tells in the order of co_consts.
    auto* literal = node->value ? node->value->tryAs<Constant>() : nullptr;
    if (literal) {
        line(literal->range);
        value = constant(constantValue(literal->value));
    } else if (node->value) {
        value = expr(node->value);
        // What runs on the way out may assign to the variable: a `finally` clause, the end of `except ... as variable`; an __exit__, or a
        // generator that the loop closes, that has got hold of the frame.
        bool mayBeAssignedTo = m_blocks.containsIf([](auto& block) {
            return block.kind == BlockKind::FinallyTry || block.kind == BlockKind::HandlerCleanup || block.kind == BlockKind::With || block.kind == BlockKind::AsyncWith || block.ownsIterator;
        });
        if (mayBeAssignedTo && !value->isTemporary() && !value->virtualRegister().isConstant())
            value = g.move(g.newTemporary(), value.get());
    } else
        value = constant(jsUndefined());
    // As in CPython: what leaves a `with` is of the line of the `with`, and nothing comes before it, unless the value begins on another line than the `return`.
    bool leavesWithFirst = !m_blocks.isEmpty() && (m_blocks.last().kind == BlockKind::With || m_blocks.last().kind == BlockKind::AsyncWith);
    if (!leavesWithFirst || !node->value || node->value->range.lineno != node->range.lineno)
        line(node->range);

    SetForScope leavingAs(m_leavingAs, Last { Last::OfFunction });
    SetForScope valueToReturn(m_valueToReturn, value.get());
    Vector<TryContext> suspended;
    // As in CPython, where what is on the way out comes from the constant that is returned.
    bool comesFromValue = node->value && node->value->is<Constant>() && node->value->range.lineno == node->range.lineno;
    unwindBlocks(comesFromValue ? node->value->range : node->range, 0, suspended);
    if (literal || !node->value)
        noteConstant({ value->virtualRegister(), 0, true });
    g.emitReturn(value.get());
    resumeTry(WTF::move(suspended));
}

void Unit::breakOrContinue(Stmt* node, bool isBreak)
{
    line(node->range);
    size_t loop = m_blocks.reverseFindIf([](auto& block) {
        return block.kind == BlockKind::WhileLoop || block.kind == BlockKind::ForLoop || block.kind == BlockKind::AsyncForLoop;
    });
    if (loop == notFound) {
        // (What is in the way is looked at first.)
        Vector<TryContext> suspended;
        SetForScope leavingAs(m_leavingAs, Last { });
        unwindBlocks(node->range, 0, suspended);
        resumeTry(WTF::move(suspended));
        fail(node->range, isBreak ? "'break' outside loop"_s : "'continue' not properly in loop"_s);
        return;
    }
    m_blocks[loop].hasBreak |= isBreak;
    // (A jump would make it look as if the end of the loop could be reached.)
    if (m_cannotBeReached) {
        // What is in the way is looked at all the same.
        if (std::ranges::any_of(m_blocks.subspan(loop + 1), [](const Block& block) { return block.kind == BlockKind::ExceptionGroupHandler; }))
            fail(node->range, "'break', 'continue' and 'return' cannot appear in an except* block"_s);
        return;
    }
    if (!isBreak)
        return emitImplicitContinue(loop, node->range);
    // To leave a loop that is the last statement of the function is to return, and so on. A `return` in the loop that has got as far as a
    // `finally` with this `break` in it comes to nothing.
    // (What drops the iterator when a `for` loop ends comes before the end.)
    JumpIs jumpIs = m_blocks[loop].kind == BlockKind::ForLoop ? JumpIs::OfTheProgram : JumpIs::ToldOf;
    if (m_blocks[loop].isLast) {
        SetForScope valueToReturn(m_valueToReturn, m_blocks[loop].valueToReturn);
        return leave(m_blocks[loop].isLast.inPlaceOfJump(jumpIs), node->range);
    }
    Last leavingAs { Last::BeforeJump, 0, m_blocks[loop].breakTarget, static_cast<unsigned>(loop), static_cast<unsigned>(m_blocks[loop].tryDepth) };
    leavingAs.jumpIs = jumpIs;
    leavingAs.instructionsAfterJump = m_blocks[loop].instructionsAfterLoop;
    leave(leavingAs, node->range);
}

void Unit::raiseStatement(Raise* node)
{
    Arguments arguments(*this, 3);
    // `raise Error(...)`: if Error turns out to be the class of what is raised, nobody else has that yet.
    auto* made = node->exc && !node->cause ? node->exc->tryAs<Call>() : nullptr;
    if (made)
        call(made, arguments[0], arguments[2]);
    else {
        if (node->exc)
            expr(node->exc, arguments[0]);
        else
            g.move(arguments[0], emptyValue());
        g.move(arguments[2], emptyValue());
    }
    if (node->cause)
        expr(node->cause, arguments[1]);
    else
        g.move(arguments[1], emptyValue());
    loc(node->range);
    callRuntime(arguments[0], RuntimeFunction::Raise, arguments);
}

void Unit::tryStatement(AST::Try* node, Last isLast)
{
    line(node->range);
    if (!node->finalbody.isEmpty())
        return tryFinally(node, isLast);
    if (node->isStar())
        return tryStarExcept(node, isLast);
    tryExcept(node, isLast);
}

void Unit::tryFinally(AST::Try* node, Last isLast)
{
    Ref<Label> exit = g.newLabel();

    Try body = beginTry();
    // What comes after the statements is the clause.
    StatementsAfter clause { node->finalbody, 0, { }, static_cast<unsigned>(m_blocks.size()), m_statementsAfter, false, 1 };
    pushBlock({ .kind = BlockKind::FinallyTry, .tryDepth = body.depthBefore, .finalBody = node->finalbody });
    {
        SetForScope statementsAfter(m_statementsAfter, &clause);
        m_statementsEndWhereStatementEnds = node->handlers.isEmpty();
        if (node->handlers.isEmpty())
            statements(node->body);
        else if (node->isStar())
            tryStarExcept(node, { });
        else
            tryExcept(node, { });
    }
    endOfProtectedStatements();
    popBlock();
    endTry(body);

    if (!cannotGetHere()) {
        statements(node->finalbody, 0, lastBeforeJump(exit.get(), isLast, JumpIs::ToldOfForwards));
        leaveStatement(exit.get(), isLast, JumpIs::ToldOfForwards);
    }

    // An exception: it goes on after the clause has run.
    Reg previous;
    Reg exception = beginExceptionHandler(body, previous);
    Try cleanup = beginTry();
    pushBlock({ .kind = BlockKind::FinallyEnd, .tryDepth = cleanup.depthBefore, .saved = previous.get() });
    // It does so on the line that was run last. CPython's compiler copies what raises it again for each way that leads there.
    statements(node->finalbody, 0, { Last::OfFunction, 0, nullptr, static_cast<unsigned>(m_blocks.size()), static_cast<unsigned>(cleanup.depthBefore), exception.get(), previous.get() });
    popBlock();
    endTry(cleanup);
    if (!cannotGetHere())
        reraiseOutOfHandler(exception.get(), previous.get());

    // An exception in the clause itself.
    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    endOfStatement(exit.get());
}

void Unit::tryExcept(AST::Try* node, Last isLast)
{
    Ref<Label> end = g.newLabel();

    // What comes next is a jump over the handlers. CPython's compiler puts the handlers behind everything else, so there it is a jump to what follows, and is dropped.
    Last isLastOfBody = lastBeforeJump(end.get(), isLast, JumpIs::ToldOfForwards, node->orelse.isEmpty() ? 2 : 1);
    Last isLastOfHandler = lastBeforeJump(end.get(), isLast, JumpIs::ToldOfForwards, 3);
    Last isLastOfHandlerWithName = lastBeforeJump(end.get(), isLast, JumpIs::ToldOfForwards, 7);
    Try body = beginTry();
    pushBlock({ .kind = BlockKind::TryExcept, .tryDepth = body.depthBefore });
    statements(node->body, 0, isLastOfBody.onlyIf(node->orelse.isEmpty()));
    endOfProtectedStatements(node->orelse.isEmpty() ? JumpsHere::AreTakenIn : JumpsHere::Count, end.ptr());
    popBlock();
    endTry(body);
    if (cannotGetHere())
        deadStatements(node->orelse);
    else
        statements(node->orelse, 0, isLastOfBody);
    leaveStatement(end.get(), isLast, JumpIs::ToldOfForwards);

    Reg previous;
    Reg exception = beginExceptionHandler(body, previous);
    unsigned yieldPointsBefore = g.yieldPoints();
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
        // For `frame.f_lineno = n`: the line of `except` is further in than the clause. In CPython the exception is on the stack until it has been matched, so
        // from there a frame can go on in any of the clauses, and there is no way back. (It can go on in a handler within a clause too, which then
        // takes the exception for what was being handled before.)
        unsigned matching = ++m_blockCount;
        auto bitsOfMatching = TracedBlock::encode(TracedBlock::ExceptionHandler, exception->virtualRegister().offset(), false);
        g.addPythonMark(PythonMark::BlockBegin, matching, bitsOfMatching);
        line(handler->range);
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
            g.addPythonMark(PythonMark::BlockEnd, matching, bitsOfMatching);
            Try nameCleanup = beginTry();
            pushBlock({ .kind = BlockKind::HandlerCleanup, .tryDepth = cleanup.depthBefore, .saved = previous.get(), .name = handler->name, .range = handler->range, .isMarked = false });
            statements(handler->body, 0, isLastOfHandlerWithName);
            popBlock();
            endTry(nameCleanup);
            outsideTry(cleanup.depthBefore, [&] {
                if (cannotGetHere())
                    return;
                SameLine sameLine(*this);
                restoreHandledException(previous.get());
                storeName(handler->name, constant(jsUndefined()), handler->range);
                deleteName(handler->name, handler->range);
                leaveStatement(end.get(), isLast, JumpIs::ToldOfForwards);
            });

            Reg thrown = beginHandler(nameCleanup);
            SameLine sameLine(*this);
            storeName(handler->name, constant(jsUndefined()), handler->range);
            deleteName(handler->name, handler->range);
            outsideTry(cleanup.depthBefore, [&] {
                reraiseOutOfHandler(thrown.get(), previous.get());
            });
        } else {
            g.addPythonMark(PythonMark::BlockEnd, matching, bitsOfMatching);
            pushBlock({ .kind = BlockKind::HandlerCleanup, .tryDepth = cleanup.depthBefore, .saved = previous.get(), .isMarked = false });
            statements(handler->body, 0, isLastOfHandler);
            popBlock();
            outsideTry(cleanup.depthBefore, [&] {
                if (cannotGetHere())
                    return;
                restoreHandledException(previous.get());
                leaveStatement(end.get(), isLast, JumpIs::ToldOfForwards);
            });
        }
        g.emitLabel(next.get());
    }
    popBlock();

    if (g.yieldPoints() == yieldPointsBefore && std::ranges::none_of(node->handlers, [](ExceptHandler* handler) { return !!handler->name; }))
        g.pyCatchHandlerLeavesItUnseen(body.data);

    // No clause caught it.
    endTry(cleanup);
    if (count)
        line(node->handlers[count - 1]->range);
    reraiseOutOfHandler(exception.get(), previous.get());

    // An exception in a clause.
    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    endOfStatement(end.get());
}

void Unit::tryStarExcept(AST::Try* node, Last isLast)
{
    Ref<Label> orelse = g.newLabel();
    Ref<Label> end = g.newLabel();

    Try body = beginTry();
    pushBlock({ .kind = BlockKind::TryExcept, .tryDepth = body.depthBefore });
    statements(node->body, 0, isLast.onlyIf(node->orelse.isEmpty()));
    endOfProtectedStatements();
    popBlock();
    endTry(body);
    leaveStatement(orelse.get(), isLast.onlyIf(node->orelse.isEmpty()), JumpIs::ToldOfForwards);

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
        line(handler->range);
        if (handler->type) {
            Reg type = expr(handler->type);
            loc(handler->range);
            callRuntime(match.get(), RuntimeFunction::MatchExceptionGroup, { rest.get(), type.get() });
            callRuntime(rest.get(), RuntimeFunction::TakeExceptionGroupRest);
            g.emitPyJumpIfNone(match.get(), next.get());
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
        jump(next.get());

        Reg thrown = beginHandler(clause);
        callRuntime(thrown.get(), RuntimeFunction::CatchException, { thrown.get() });
        if (handler->name) {
            SameLine sameLine(*this);
            storeName(handler->name, constant(jsUndefined()), handler->range);
            deleteName(handler->name, handler->range);
        }
        g.emitPyListAppend(raised.get(), thrown.get());

        g.emitLabel(next.get());
        callRuntimeForEffect(RuntimeFunction::SetHandledException, { original.get() });
    }
    popBlock();

    g.emitPyListAppend(raised.get(), rest.get());
    Reg toRaise = newTemporary();
    callRuntime(toRaise.get(), RuntimeFunction::PrepareReraiseStar, { original.get(), raised.get() });
    endTry(cleanup);

    restoreHandledException(previous.get());
    g.emitPyJumpIfNone(toRaise.get(), end.get());
    callRuntimeForEffect(RuntimeFunction::ReraiseGroup, { toRaise.get() });

    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    bool bodyComesToItsEnd = orelse->isBound();
    g.emitLabel(orelse.get());
    if (!bodyComesToItsEnd)
        deadStatements(node->orelse);
    else {
        statements(node->orelse, 0, isLast);
        if (isLast.kind == Last::OfFunction && !cannotGetHere())
            emitImplicitReturn(isLast);
    }
    endOfStatement(end.get());
}

void Unit::withStatement(With* node, unsigned index, Last isLast)
{
    bool isAsync = node->isAsync();
    WithItem* item = node->items[index];
    const Range& range = item->contextExpr->range;
    Ref<Label> end = g.newLabel();

    // No bound methods are made: the functions are called with the manager as their first argument.
    Reg exit = newTemporary();
    Reg exitSelf = newTemporary();
    ASSERT(exitSelf->index() == exit->index() - 1); // TracedBlock::With
    Reg value = newTemporary();
    {
        Reg enterSelf = newTemporary();
        {
            Reg manager = expr(item->contextExpr);
            loc(range);
            g.emitPyGetMethod(exit.get(), exitSelf.get(), manager.get(), constant(jsNumber(static_cast<int>(isAsync ? SpecialMethodForWith::AExit : SpecialMethodForWith::Exit))));
            g.emitPyGetMethod(value.get(), enterSelf.get(), manager.get(), constant(jsNumber(static_cast<int>(isAsync ? SpecialMethodForWith::AEnter : SpecialMethodForWith::Enter))));
        }
        emitMethodCall(value.get(), value.get(), enterSelf.get(), { }, range);
    }
    if (isAsync)
        emitAwait(value.get(), value.get(), 1);

    Try body = beginTry();
    pushBlock({ .kind = isAsync ? BlockKind::AsyncWith : BlockKind::With, .tryDepth = body.depthBefore, .saved = exit.get(), .savedSelf = exitSelf.get(), .range = range });
    if (item->optionalVars)
        store(item->optionalVars, value.get());
    value = nullptr;
    if (index + 1 == node->items.size())
        statements(node->body);
    else
        withStatement(node, index + 1, { });
    endOfProtectedStatements();
    popBlock();
    endTry(body);

    {
        line(range);
        Reg result = newTemporary();
        RegisterID* none = constant(jsUndefined());
        noteConstant(jsUndefined());
        emitMethodCall(result.get(), exit.get(), exitSelf.get(), { none, none, none }, range);
        if (isAsync)
            emitAwait(result.get(), result.get(), 2);
    }
    leaveStatement(end.get(), isLast, JumpIs::ToldOf);

    // An exception: __exit__ says whether it goes on.
    Reg previous;
    Reg exception = beginExceptionHandler(body, previous, true);
    Try cleanup = beginTry();
    Reg result = newTemporary();
    loc(range);
    callRuntime(result.get(), RuntimeFunction::WithExit, { exit.get(), exitSelf.get(), exception.get() });
    if (isAsync)
        emitAwait(result.get(), result.get(), 2);
    Ref<Label> suppress = g.newLabel();
    g.emitPyJumpIfTrue(result.get(), suppress.get());
    endTry(cleanup);
    // It goes on as if from where it was raised: the frame stays on that line. To a tool, what raises it again is of the line of the
    // `with`: see Monitoring::fire().
    g.addPythonMark(PythonMark::Line, lineMarkValue(range.lineno, false));
    noLine();
    reraiseOutOfHandler(exception.get(), previous.get());

    g.emitLabel(suppress.get());
    line(range);
    restoreHandledException(previous.get());
    leaveStatement(end.get(), isLast, JumpIs::ToldOfForwards);

    Reg thrown = beginHandler(cleanup);
    restoreHandledException(previous.get());
    reraise(thrown.get());

    endOfStatement(end.get());
}

void Unit::assertStatement(Assert* node, Last isLast)
{
    if (auto* tuple = node->test->tryAs<Tuple>(); tuple && !tuple->elts.isEmpty() && c.m_warned.add(node).isNewEntry)
        warn(node->range, "assertion is always true, perhaps remove parentheses?"_s);
    if (c.m_options.optimize)
        return;
    Ref<Label> end = g.newLabel();
    ReturnsByLine returns;
    jumpIfOrLeave(node->test, end.get(), isLast.onlyIf(isLast.kind != Last::OfFunction), returns, true);
    int lineOfBranch = m_markedLine;
    auto fails = [&] {
        Arguments arguments(*this, 3);
        line(node->range);
        g.move(arguments[0], constant(runtime().AssertionError()));
        g.move(arguments[2], node->msg ? arguments[0] : emptyValue());
        if (node->msg) {
            Reg message = expr(node->msg);
            emitSimpleCall(arguments[0], arguments[0], { message.get() }, node->range);
        }
        g.move(arguments[1], emptyValue());
        loc(node->test->range);
        callRuntime(arguments[0], RuntimeFunction::Raise, arguments);
    };
    // `assert a or 1, message`
    if (!m_cannotBeReached && cannotGetHere())
        deadCode(fails);
    else
        fails();
    emitReturnsByLine(returns, isLast);
    if (isLast.kind == Last::BeforeJump)
        endOfStatement(end.get(), isLast, lineOfBranch);
    else if (end->isBound())
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
    String docstring(constant->value.string);
    // CPython goes through UTF-8 to clean it.
    if (!docstring.is8Bit() && !tryEncodeUTF8(docstring)) [[unlikely]] {
        c.failWithoutLocation(SyntaxErrorInfo::Type::UnicodeEncodeError, docstring);
        return { };
    }
    return cleanDocstring(docstring);
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
    noteConstant(jsNumber(2));
    g.emitPyCompare(isOther.get(), g.pythonParameter(0), constant(jsNumber(2)), CompareOperator::Gt);
    g.emitPyJumpIfFalse(isOther.get(), body.get());
    callRuntimeForEffect(RuntimeFunction::Raise, { constant(runtime().NotImplementedError()), emptyValue(), emptyValue() });
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
                g.emitPyJumpIfFalse(set.get(), notSet.get());
            }
            Reg value = unit.expr(statement->annotation);
            unit.line(statement->range.lineno);
            unit.loc(range);
            g.emitPySetItem(annotations.get(), unit.nameAsValue(unit.mangle(statement->target->as<NameExpr>()->id)), value.get());
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
    statements(body, first, { Last::OfFunction });
    // All of a module with nothing in it is of line 0, which whoever traces it is told of: a debugger stops there.
    if (body.isEmpty())
        g.addPythonMark(PythonMark::Line, lineMarkValue(0, true));
    emitReturnNone();
}

void Unit::compileFunctionBody(Seq<Stmt*> body)
{
    unsigned first = m_scope->hasDocstring ? 1 : 0;
    size_t start = g.instructions().size();
    statements(body, first, { Last::OfFunction });
    // A function with nothing in it, or only what no code comes of (`a: int`, `global b`), returns on the line it begins on.
    if (g.instructions().size() == start && m_markedLine == -1)
        line(m_info.firstLineNumber);
    emitReturnNone();
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
