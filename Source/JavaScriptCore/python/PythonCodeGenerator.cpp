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
#include "PythonCodeGenerator.h"

#include "BytecodeGenerator.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "BytecodeStructs.h"
#include "JSBigInt.h"
#include "PyCodeConstant.h"
#include "PythonASTOptimizer.h"
#include "PythonConstantFolding.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonUnparse.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGenerator.h"
#include "LinkTimeConstant.h"
#include "PythonCommonNames.h"
#include "TaggedArithmetic.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace Python {

// A Python syntax tree to JavaScriptCore bytecode. See README.md for how the one language is laid over the other.
//
// As in NodesCodegen.cpp, what emits an expression is given a register that the value would be welcome in, or none, and says which
// register it is in. A local variable's own register is only ever written by the last instruction of what is assigned to it.
class CodeGenerator {
public:
    CodeGenerator(BytecodeGenerator& generator, Arena& arena, SymbolTable& table, Block& block, const FunctionInfo& info, SyntaxError& error, unsigned& functionsBeforeError)
        : g(generator)
        , m_vm(generator.vm())
        , m_names(m_vm.pythonNames())
        , m_arena(arena)
        , m_table(table)
        , m_block(block)
        , m_info(info)
        , m_error(error)
        , m_functionsBeforeError(functionsBeforeError)
        , m_private(info.privateName.isNull() ? nullptr : &info.privateName)
    {
    }

    void generate(void* root)
    {
        m_details = makeUnique<CodeDetails>();
        for (auto& name : m_info.parameterNames) {
            // The mapping that the names of a class body are looked up in is a parameter of ours, and not one of Python's.
            if (!m_info.usesNamespace)
                noteVariableName(name);
        }
        generateKind(root);
        finishConstants();
        for (Symbol& symbol : m_block.symbols) {
            if (symbol.scope == NameScope::Cell)
                m_details->cellVariables.append(*symbol.name);
        }
        if (m_info.kind == CodeKind::Class) {
            if (m_block.needsClassClosure)
                addOnce(m_details->cellVariables, m_names.dunder_class);
            if (m_block.needsClassDict)
                addOnce(m_details->cellVariables, m_names.dunder_classdict);
        }
        auto byCodePoint = [] (auto& a, auto& b) { return codePointCompareLessThan(a.string(), b.string()); };
        std::ranges::sort(m_details->cellVariables, byCodePoint);
        // _PyCode_GetCellvars(): they are in the order of co_localsplusnames. A parameter that is a cell is among the variables there, which come first.
        std::ranges::stable_sort(m_details->cellVariables, { }, [&] (auto& name) { return std::min(m_details->variableNames.find(name), m_details->variableNames.size()); });
        describeFrame(byCodePoint);
        m_info.details = WTF::move(m_details);
    }

    // What a frame object needs to know.
    void describeFrame(auto& byCodePoint)
    {
        if (!m_info.isGeneratorBody)
            m_details->frameObjectRegister = g.m_pythonFrameObjectRegister->virtualRegister();
        m_details->scopeRegister = g.scopeRegister()->virtualRegister();
        HashMap<UniquedStringImpl*, unsigned> seen;
        auto add = [&] (const Identifier& name, bool isInEnvironment) {
            if (!seen.add(name.impl(), m_details->frameVariables.size()).isNewEntry)
                return;
            RegisterID* local = isInEnvironment ? nullptr : m_locals.get(name.impl());
            // allocateVariables()
            RELEASE_ASSERT(!local || !local->virtualRegister().isLocal() || local->virtualRegister().toLocal() < g.m_codeBlock->numVars());
            // What is no function has its variables somewhere else, but for these.
            m_details->frameVariables.append({ name, local ? local->virtualRegister() : VirtualRegister(), !isFunctionLike() });
        };
        for (auto& name : m_details->variableNames)
            add(name, m_block.scopeOf(name) == NameScope::Cell);
        if (isFunctionLike()) {
            for (auto& name : m_details->cellVariables)
                add(name, true);
            Vector<Identifier> freeVariables = m_info.freeVariables;
            std::ranges::sort(freeVariables, byCodePoint);
            for (auto& name : freeVariables)
                add(name, true);
        }
        for (auto& variable : m_details->comprehensionVariables)
            variable.frameVariable = seen.get(variable.name.impl());

        // What makes a generator of the code puts more instructions into it.
        g.m_codeBlock->addOffsetKeptElsewhere(m_details->enterOffset);
        if (m_details->firstTraceableOffset != std::numeric_limits<unsigned>::max())
            g.m_codeBlock->addOffsetKeptElsewhere(m_details->firstTraceableOffset);
        for (auto& variable : m_details->comprehensionVariables) {
            g.m_codeBlock->addOffsetKeptElsewhere(variable.begin);
            g.m_codeBlock->addOffsetKeptElsewhere(variable.end);
        }

        if (m_info.isGeneratorBody) {
            // Whatever has the frame of a suspended generator can see its variables, whether or not the generator will look at them again.
            g.m_localsToSaveAtEveryYield.append(m_details->scopeRegister);
            for (auto& variable : m_details->frameVariables) {
                if (variable.location.isValid())
                    g.m_localsToSaveAtEveryYield.append(variable.location);
            }
            for (auto& variable : m_details->comprehensionVariables) {
                if (variable.location.isValid())
                    g.m_localsToSaveAtEveryYield.append(variable.location);
            }
        }
    }

    // What was written begins here. Its arguments have been given to its parameters and its variables are where they are looked for.
    void emitEnter()
    {
        m_details->enterOffset = g.instructions().size();
        m_details->valueProfilesBeforeEnter = g.m_codeBlock->metadata().numValueProfiles();
        // Until it has got this far there is no frame to be told of, nor to be seen in a traceback.
        if (m_details->firstTraceableOffset < m_details->enterOffset)
            m_details->firstTraceableOffset = m_details->enterOffset;
        OpPyEnter::emit(&g, false);
        m_isArtificial = false;
    }

    void generateKind(void* root)
    {
        switch (m_info.kind) {
        case CodeKind::Module:
        case CodeKind::Expression:
        case CodeKind::Interactive:
            generateModule(*static_cast<Module*>(root));
            break;
        case CodeKind::Function: {
            auto& node = *static_cast<FunctionDef*>(root);
            collectDeletedNames(node.body);
            m_whereItBegins = &node;
            generateFunction([&] {
                emit(m_block.hasDocstring ? node.body.subspan(1) : node.body);
                m_endIsNeverComeTo = alwaysLeave(node.body);
                if (!m_numberOfLines)
                    mark(node);
            });
            break;
        }
        case CodeKind::Lambda: {
            auto& node = *static_cast<Lambda*>(root);
            generateFunction([&] {
                Reg value = emit(node.body);
                g.emitReturn(value.get());
            });
            break;
        }
        case CodeKind::Class:
            generateClassBody(*static_cast<ClassDef*>(root));
            break;
        case CodeKind::GeneratorExpression:
            generateGeneratorExpression(*static_cast<GeneratorExp*>(root));
            break;
        case CodeKind::Comprehension:
            generateComprehension(*static_cast<Expression*>(root));
            break;
        case CodeKind::Annotations:
            generateAnnotations(root);
            break;
        case CodeKind::TypeParameters:
            generateTypeParameters(*static_cast<Statement*>(root));
            break;
        case CodeKind::Evaluator:
            generateEvaluator(*static_cast<Statement*>(root));
            break;
        }
    }

private:
    using Reg = RefPtr<RegisterID>;

    // ---- Errors and positions

    void fail(String&& message, const Node& node)
    {
        noteFirstError();
        if (!m_error)
            m_error = { SyntaxError::Kind::SyntaxError, false, WTF::move(message), node.line, static_cast<int>(node.column), node.endLine, static_cast<int>(node.endColumn) };
    }

    void fail(SyntaxError::Kind kind, String&& message)
    {
        noteFirstError();
        if (!m_error) {
            m_error.kind = kind;
            m_error.message = WTF::move(message);
        }
    }

    void noteFirstError()
    {
        if (!m_error)
            m_functionsBeforeError = m_numberOfFunctions;
    }

    // Says where in the source what is emitted next comes from, for when it raises.
    void mark(const Node& node)
    {
        // What is never come to is nowhere.
        if (m_isNeverComeTo)
            return;
        // What was made of something else, and is nowhere in the source, is where whatever came before it is. That is where the function begins, if nothing has.
        if (!node.line || node.line == static_cast<unsigned>(-1)) {
            if (!m_numberOfLines && m_whereItBegins && !m_isArtificial)
                mark(*m_whereItBegins);
            return;
        }
        // This is the first thing to be done in a statement, so the statement is on its line. That holds though it be something that was not written, since where a thing is has to be said in the order that things are in.
        if (m_lineOfStatement) {
            g.emitExpressionInfo(*std::exchange(m_lineOfStatement, std::nullopt), JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end));
            m_lastMarked = node;
            m_lastMarkedLine = node.line;
            ++m_numberOfLines;
            return;
        }
        g.emitExpressionInfo(JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end));
        // What was not written is on no line. Where it is said to be from is for if it goes wrong.
        if (m_isArtificial)
            return;
        m_lastMarked = node;
        bool isOnAnotherLine = node.line != m_lastMarkedLine;
        m_lastMarkedLine = node.line;
        // What follows is from the same place, having nothing to say otherwise.
        if (isOnAnotherLine) {
            emitLine(LineKind::Line);
            ++m_numberOfLines;
        }
    }

    void emitLine(LineKind kind)
    {
        OpPyLine::emit(&g, static_cast<unsigned>(kind), m_jumpBlock);
    }

    // A statement begins with its op_py_line, so that frame.f_lineno = n can go to it: there is nothing before that for what comes after to depend on. Which line it is on is not known yet, being that of whatever
    // in it is done first, and mark() says when that is come to. What is between the two cannot go wrong, or it would have said where it is.
    void beginLineOfStatement()
    {
        if (m_isNeverComeTo || m_isArtificial || m_lineOfStatement)
            return;
        m_lineOfStatement = g.instructions().size();
        emitLine(LineKind::WhereItCanBeGoneOnFrom);
    }

    // The same, of what is not the beginning of a statement and is on a line of its own all the same.
    void markWhereItCanBeGoneOnFrom(const Node& node)
    {
        forgetLine();
        beginLineOfStatement();
        mark(node);
    }

    void noteWayOut(int completionType, VirtualRegister value, bool keepsValue, bool isOutOfLoop)
    {
        if (m_isNeverComeTo)
            return;
        for (WaysOut* ways : m_waysOut | std::views::reverse) {
            // A `break` leaves only what is in the loop.
            if (isOutOfLoop && ways->loopNesting != m_loopNesting)
                return;
            ways->leaves.append({ completionType, value, keepsValue, m_handledExceptions.size() > ways->handlerNesting });
            // It gets no further than a `finally` that has some other way to go.
            if (ways->isTheEnd)
                return;
        }
    }

    // See CodeDetails::JumpBlock.
    class JumpBlock {
        WTF_MAKE_NONCOPYABLE(JumpBlock);
    public:
        JumpBlock(CodeGenerator& generator, CodeDetails::JumpBlock::Kind kind, RegisterID* first = nullptr, RegisterID* second = nullptr, RegisterID* third = nullptr, bool isFallenInto = true)
            : m_generator(generator)
            , m_parent(generator.m_jumpBlock)
        {
            auto registerOf = [] (RegisterID* given) { return given ? given->virtualRegister() : VirtualRegister(); };
            generator.m_details->jumpBlocks.append({ kind, isFallenInto, m_parent, registerOf(first), registerOf(second), registerOf(third) });
            generator.m_jumpBlock = generator.m_details->jumpBlocks.size();
        }

        ~JumpBlock() { m_generator.m_jumpBlock = m_parent; }

    private:
        CodeGenerator& m_generator;
        unsigned m_parent;
    };

    // What comes next can be come to from some other line, whatever line was last written for: it is jumped to.
    void forgetLine() { m_lastMarkedLine = 0; }

    void emitLabel(Label& label)
    {
        g.emitLabel(label);
        forgetLine();
    }

    // Going round a loop again, to where the node is.
    void emitLineAfterBackwardJump(const Node& node)
    {
        if (m_isNeverComeTo)
            return;
        m_lastMarkedLine = node.line;
        g.emitExpressionInfo(JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end));
        emitLine(LineKind::AfterBackwardJump);
    }

    // update_start_location_to_match_attr(): if what has the attribute is on some earlier line, getting the attribute is on the line that its name is on.
    static Node locationOf(const Attribute& node)
    {
        Node location = node;
        if (node.line != node.endLine) {
            unsigned length = std::min<unsigned>(node.attribute->length(), node.endColumn);
            location.line = node.endLine;
            location.column = node.endColumn - length;
            location.start = node.attributeStart;
        }
        return location;
    }

    // For what cannot go wrong, and need only say where it is if that is a line of its own: co_lines().
    void markIfOnAnotherLine(const Node& node)
    {
        if (node.line != m_lastMarkedLine && !m_isInConstantDisplay && !m_isArtificial)
            mark(node);
    }

    // Whether CPython makes one constant of it, which is then all on the line that it begins on: a tuple of constants, or a list or a set of three or more.
    static bool isConstant(Expression* expression)
    {
        if (expression->is<Constant>())
            return true;
        auto* tuple = expression->tryAs<Tuple>();
        return tuple && std::ranges::all_of(tuple->elements, isConstant);
    }

    // What a condition comes to, if that is plain from the source.
    // `isComeTo` is whether this is asked because code is being made for it, so that it is among the constants, and not to find something out beforehand.
    std::optional<bool> constantTruth(Expression* expression, bool isComeTo = true)
    {
        if (auto* name = expression->tryAs<Name>()) {
            if (*name->id != m_names.dunder_debug)
                return std::nullopt;
            if (isComeTo)
                noteConstant({ m_info.optimizationLevel ? CodeDetails::Constant::Kind::False : CodeDetails::Constant::Kind::True }, ConstantUse::IsPartOfAnother);
            return !m_info.optimizationLevel;
        }
        if (auto* operation = expression->tryAs<UnaryOp>(); operation && operation->op == UnaryOperator::Not) {
            auto operand = constantTruth(operation->operand, isComeTo);
            return operand ? std::optional { !*operand } : std::nullopt;
        }
        auto* constant = expression->tryAs<Constant>();
        if (!constant)
            return std::nullopt;
        // It was come to, though nothing is done with it.
        if (isComeTo && constant->type != Constant::Type::Invalid)
            noteConstant(describeConstant(*constant), ConstantUse::IsPartOfAnother);
        switch (constant->type) {
        case Constant::Type::None:
        case Constant::Type::False:
            return false;
        case Constant::Type::True:
        case Constant::Type::Ellipsis:
            return true;
        case Constant::Type::Integer:
            return !!constant->integer;
        case Constant::Type::BigInteger:
            return true;
        case Constant::Type::Float:
        case Constant::Type::Imaginary:
            return !!constant->real;
        case Constant::Type::String:
        case Constant::Type::Bytes:
            return !constant->text->isEmpty();
        case Constant::Type::Complex:
            return constant->real || constant->imaginary;
        case Constant::Type::Tuple:
        case Constant::Type::FrozenSet:
            return !constant->elements.empty();
        case Constant::Type::Invalid:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Whether there is a `break` that leaves the loop that these are the body of.
    static bool hasBreak(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements) {
            switch (statement->kind) {
            case Statement::Kind::Break:
                return true;
            case Statement::Kind::If:
                if (hasBreak(statement->as<If>().body) || hasBreak(statement->as<If>().orElse))
                    return true;
                break;
            case Statement::Kind::With:
                if (hasBreak(statement->as<With>().body))
                    return true;
                break;
            case Statement::Kind::Try: {
                auto& node = statement->as<Try>();
                if (hasBreak(node.body) || hasBreak(node.orElse) || hasBreak(node.finalBody))
                    return true;
                for (ExceptHandler* handler : node.handlers) {
                    if (hasBreak(handler->body))
                        return true;
                }
                break;
            }
            case Statement::Kind::Match:
                for (MatchCase* matchCase : statement->as<Match>().cases) {
                    if (hasBreak(matchCase->body))
                        return true;
                }
                break;
            // One that is in a loop leaves that loop, but for what the loop does when it has run its course.
            case Statement::Kind::For:
                if (hasBreak(statement->as<For>().orElse))
                    return true;
                break;
            case Statement::Kind::While:
                if (hasBreak(statement->as<While>().orElse))
                    return true;
                break;
            default:
                break;
            }
        }
        return false;
    }

    // Whether what comes after these is never come to, as far as is plain from how they are written.
    bool alwaysLeave(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements) {
            if (alwaysLeaves(*statement))
                return true;
        }
        return false;
    }

    // It is asked of each statement by each thing that the statement is in, so what it comes to is kept.
    bool alwaysLeaves(Statement& statement)
    {
        switch (statement.kind) {
        case Statement::Kind::Return:
        case Statement::Kind::Raise:
        case Statement::Kind::Break:
        case Statement::Kind::Continue:
            return true;
        case Statement::Kind::If:
        case Statement::Kind::For:
        case Statement::Kind::While:
        case Statement::Kind::Try:
            break;
        default:
            return false;
        }
        if (auto known = m_alwaysLeaves.find(&statement); known != m_alwaysLeaves.end())
            return known->value;
        bool result = false;
        if (statement.is<If>())
            result = alwaysLeave(statement.as<If>().body) && alwaysLeave(statement.as<If>().orElse);
        else if (statement.is<While>()) {
            // What comes after a loop is come to by a `break`, or by way of its `else`.
            auto& node = statement.as<While>();
            result = (constantTruth(node.test, false) == std::optional { true } || alwaysLeave(node.orElse)) && !hasBreak(node.body);
        } else if (statement.is<For>())
            result = alwaysLeave(statement.as<For>().orElse) && !hasBreak(statement.as<For>().body);
        else
            result = alwaysLeave(statement.as<Try>().finalBody) || alwaysLeavesBeforeFinally(statement.as<Try>());
        m_alwaysLeaves.add(&statement, result);
        return result;
    }

    // Whether there is anything in these that CPython has an instruction for that could raise.
    bool canRaise(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements) {
            switch (statement->kind) {
            case Statement::Kind::Pass:
            case Statement::Kind::Global:
            case Statement::Kind::Nonlocal:
                continue;
            case Statement::Kind::Break:
            case Statement::Kind::Continue:
                return false;
            case Statement::Kind::Return:
                return statement->as<Return>().value && !constantOf(statement->as<Return>().value);
            case Statement::Kind::Expr:
                if (statement->as<Expr>().value->is<Constant>())
                    continue;
                return true;
            case Statement::Kind::Try: {
                auto& node = statement->as<Try>();
                if (canRaise(node.body) || (!alwaysLeave(node.body) && canRaise(node.orElse)) || canRaise(node.finalBody))
                    return true;
                if (alwaysLeaves(*statement))
                    return false;
                continue;
            }
            default:
                return true;
            }
        }
        return false;
    }

    bool alwaysLeavesBeforeFinally(Try& node)
    {
        bool allLeave = alwaysLeave(node.body) || alwaysLeave(node.orElse);
        for (ExceptHandler* handler : node.handlers)
            allLeave = allLeave && alwaysLeave(handler->body);
        return allLeave;
    }

    // Statements that are never come to, `if 0:` and the like. They are compiled, since that is how it is found out what is wrong with them, and jumped over. They have no
    // lines and no constants, as in CPython, which takes them out.
    void emitNeverComeTo(Sequence<Statement*> statements)
    {
        if (statements.empty())
            return;
        Ref<Label> after = g.newLabel();
        g.emitJump(after.get());
        {
            SetForScope isNeverComeTo(m_isNeverComeTo, true);
            emit(statements);
        }
        emitLabel(after.get());
    }

    // ---- Registers

    Reg destination(RegisterID* dst) { return dst ? dst : g.newTemporary(); }
    // For what writes its result before it has read all that it reads.
    Reg temporaryDestination(RegisterID* dst) { return dst && dst->isTemporary() ? dst : g.newTemporary(); }

    RegisterID* finish(RegisterID* dst, RegisterID* result)
    {
        if (!dst || dst == result)
            return result;
        return g.move(dst, result);
    }

    void emitInto(RegisterID* target, Expression* expression)
    {
        RegisterID* result = emit(expression, target);
        if (result != target)
            g.move(target, result);
    }

    // In a register that nothing else will write to while it is wanted.
    Reg emitToTemporary(Expression* expression)
    {
        Reg temporary = g.newTemporary();
        emitInto(temporary.get(), expression);
        return temporary;
    }

    RegisterID* constant(JSValue value) { return g.addConstantValue(value); }
    // intern_string_constants(): a str that could be a name is the one str that there is for the name, in whatever code it is written.
    JSString* stringFor(const Identifier& string) { return isInternedAsConstant(string.string()) ? internedString(m_vm, string) : g.addStringConstant(string); }
    RegisterID* stringConstant(const Identifier& string) { return g.addConstantValue(stringFor(string)); }
    RegisterID* none() { return constant(jsUndefined()); }

    // ---- The runtime

    RegisterID* runtime() { return g.moveLinkTimeConstant(nullptr, LinkTimeConstant::pyRuntimeFunctions); }

    // A call to a function of PythonRuntimeFunctions.h.
    RegisterID* emitRuntimeCall(RegisterID* dst, ASCIILiteral name, std::initializer_list<RegisterID*> arguments, const Node& node)
    {
        markIfOnAnotherLine(node);
        Reg function = g.newTemporary();
        g.emitGetById(function.get(), runtime(), Identifier::fromString(m_vm, name));
        CallArguments call(g, nullptr, arguments.size());
        g.emitLoad(call.thisRegister(), jsUndefined());
        unsigned i = 0;
        for (RegisterID* argument : arguments)
            g.move(call.argumentRegister(i++), argument);
        Reg result = destination(dst);
        return g.emitCall(result.get(), function.get(), NoExpectedFunction, call, JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end), DebuggableCall::No);
    }

    // The same, of registers that are one after another as the elements of a display are.
    RegisterID* emitRuntimeCall(RegisterID* dst, ASCIILiteral name, const Vector<Reg, 8>& arguments, const Node& node)
    {
        markIfOnAnotherLine(node);
        Reg function = g.newTemporary();
        g.emitGetById(function.get(), runtime(), Identifier::fromString(m_vm, name));
        CallArguments call(g, nullptr, arguments.size());
        g.emitLoad(call.thisRegister(), jsUndefined());
        for (unsigned i = 0; i < arguments.size(); ++i)
            g.move(call.argumentRegister(i), arguments[i].get());
        Reg result = destination(dst);
        return g.emitCall(result.get(), function.get(), NoExpectedFunction, call, JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end), DebuggableCall::No);
    }

    // ---- Python's opcodes

    // The next thing that an iterator has, or on to `exhausted`. A generator is gone on with by code that is compiled as this is, so that the one calls the other: pythonGeneratorNext() of builtins/GeneratorPrototype.js. What has
    // been found to come of it is remembered in one place, whichever way it is come by, since it is the one thing.
    void emitIteratorNext(RegisterID* value, RegisterID* iterator, Label& exhausted, const Node& node)
    {
        unsigned valueProfile = g.nextValueProfileIndex();
        Ref<Label> isNoGenerator = g.newLabel();
        Ref<Label> hasIt = g.newLabel();
        Reg test = g.newTemporary();
        OpPyIsResumedByCall::emit(&g, test.get(), iterator);
        g.emitJumpIfFalse(test.get(), isNoGenerator.get());
        {
            Reg function = g.newTemporary();
            g.moveLinkTimeConstant(function.get(), LinkTimeConstant::pythonGeneratorNext);
            CallArguments call(g, nullptr, 1);
            g.emitLoad(call.thisRegister(), jsUndefined());
            g.move(call.argumentRegister(0), iterator);
            g.emitExpressionInfo(JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end));
            OpCall::emit(&g, value, function.get(), call.argumentCountIncludingThis(), call.stackOffset(), valueProfile);
        }
        g.emitJump(hasIt.get());
        emitLabel(isNoGenerator.get());
        OpPyIterNext::emit(&g, value, iterator, valueProfile);
        emitLabel(hasIt.get());
        g.emitIsEmpty(test.get(), value);
        g.emitJumpIfTrue(test.get(), exhausted);
    }

    RegisterID* emitBinaryOperation(RegisterID* dst, BinaryOperator op, bool inPlace, RegisterID* left, RegisterID* right)
    {
        OpPyBinaryOp::emit(&g, dst, left, right, static_cast<unsigned>(op) | (inPlace ? inPlaceOperatorFlag : 0), g.nextValueProfileIndex());
        return dst;
    }

    RegisterID* emitCompare(RegisterID* dst, ComparisonOperator op, RegisterID* left, RegisterID* right)
    {
        OpPyCompareOp::emit(&g, dst, left, right, static_cast<unsigned>(op), g.nextValueProfileIndex());
        return dst;
    }

    RegisterID* emitGetAttribute(RegisterID* dst, RegisterID* base, const Identifier& name)
    {
        noteName(name);
        OpPyGetAttr::emit(&g, dst, base, g.addConstant(name), g.nextValueProfileIndex());
        return dst;
    }

    void emitSetAttribute(RegisterID* base, const Identifier& name, RegisterID* value)
    {
        noteName(name);
        OpPySetAttr::emit(&g, base, g.addConstant(name), value);
    }

    RegisterID* emitGetItem(RegisterID* dst, RegisterID* base, RegisterID* key)
    {
        OpPyGetItem::emit(&g, dst, base, key, g.nextValueProfileIndex());
        return dst;
    }

    void emitJumpIfTrue(RegisterID* value, Label& target)
    {
        Reg truth = g.newTemporary();
        OpPyToBool::emit(&g, truth.get(), value);
        emitBranchOn(truth.get(), target, true);
    }

    // A jump that was written, on what is true or false.
    void emitBranchOn(RegisterID* condition, Label& target, bool jumpsIfTrue)
    {
        if (!m_isArtificial && !m_isNeverComeTo)
            OpPyBranch::emit(&g, condition, jumpsIfTrue);
        // The two are to be next to each other.
        g.disablePeepholeOptimization();
        if (jumpsIfTrue)
            g.emitJumpIfTrue(condition, target);
        else
            g.emitJumpIfFalse(condition, target);
    }

    // One that depends on nothing, and goes forward.
    void emitJump(Label& target)
    {
        if (!m_isArtificial && !m_isNeverComeTo)
            OpPyJump::emit(&g);
        g.emitJump(target);
    }

    void emitJumpIfFalse(RegisterID* value, Label& target)
    {
        Reg truth = g.newTemporary();
        OpPyToBool::emit(&g, truth.get(), value);
        emitBranchOn(truth.get(), target, false);
    }

    // Jumps if the condition is (or is not) true, without making a value of `not`, `and` and `or`.
    void emitBranch(Expression* condition, Label& target, bool jumpIfTrue)
    {
        if (auto* unary = condition->tryAs<UnaryOp>(); unary && unary->op == UnaryOperator::Not)
            return emitBranch(unary->operand, target, !jumpIfTrue);
        if (auto* boolean = condition->tryAs<BoolOp>()) {
            // `a and b` is false as soon as one is, and `a or b` is true as soon as one is.
            bool shortCircuitsOn = boolean->op == BooleanOperator::Or;
            if (shortCircuitsOn == jumpIfTrue) {
                for (Expression* value : boolean->values)
                    emitBranch(value, target, jumpIfTrue);
                return;
            }
            Ref<Label> skip = g.newLabel();
            for (size_t i = 0; i + 1 < boolean->values.size(); ++i)
                emitBranch(boolean->values[i], skip.get(), shortCircuitsOn);
            emitBranch(boolean->values.back(), target, jumpIfTrue);
            emitLabel(skip.get());
            return;
        }
        if (auto* comparison = condition->tryAs<Compare>(); comparison && comparison->ops.size() == 1 && isNoneConstant(comparison->comparators[0])
            && (comparison->ops[0] == ComparisonOperator::Is || comparison->ops[0] == ComparisonOperator::IsNot)) {
            // x is None
            Reg value = emit(comparison->left);
            Reg isNone = g.newTemporary();
            g.emitIsUndefinedOrNull(isNone.get(), value.get());
            markIfOnAnotherLine(*condition);
            emitBranchOn(isNone.get(), target, (comparison->ops[0] == ComparisonOperator::Is) == jumpIfTrue);
            return;
        }
        Reg value = emit(condition);
        mark(*condition);
        if (jumpIfTrue)
            emitJumpIfTrue(value.get(), target);
        else
            emitJumpIfFalse(value.get(), target);
    }

    static bool isNoneConstant(Expression* expression)
    {
        auto* constant = expression->tryAs<Constant>();
        return constant && constant->type == Constant::Type::None;
    }

    // ---- Names

    const Identifier& mangle(const Identifier& name)
    {
        // Where the type parameters of a class are, only their names are mangled for it, and not what is in its bases.
        if (m_block.mangledNames && !m_block.mangledNames->names.contains(name.impl()))
            return name;
        return SymbolTable::mangle(m_vm, m_arena, m_private, name);
    }

    bool isFunctionLike() const { return m_block.isFunctionLike(); }
    bool keepsDocstrings() const { return m_info.optimizationLevel < 2; }

    enum class Where : uint8_t {
        Register, // A local variable of a function, that no other function uses.
        Closure, // A variable of an environment: this function's, or one that it is in.
        Global, // A property of the module's namespace.
        Namespace, // In a class body: an item of the namespace being filled in, and failing that a global.
        NamespaceOrClosure, // In a class body: the same, and failing that a variable of a function that the class is in.
    };

    struct Location {
        Where where;
        RegisterID* local { nullptr };
        bool isAlwaysBound { false };
    };

    static void addOnce(Vector<Identifier>& names, const Identifier& name)
    {
        if (!names.contains(name))
            names.append(name);
    }

    // For co_names and co_varnames, which have each name once, in the order that they were first come to. There can be as many as there are lines, so whether one is there already is not found by looking through.
    void noteName(const Identifier& name)
    {
        if (m_notedNames.add(name.impl()).isNewEntry)
            m_details->names.append(name);
    }

    void noteVariableName(const Identifier& name)
    {
        if (m_notedVariableNames.add(name.impl()).isNewEntry)
            m_details->variableNames.append(name);
    }

    // Where the name is, noting that it has been used, for co_varnames and co_names.
    Location locateAndNote(const Identifier& name)
    {
        Location location = locate(name);
        switch (location.where) {
        case Where::Register:
            if (m_locals.contains(name.impl()))
                noteVariableName(name);
            break;
        case Where::Closure:
            break;
        case Where::Global:
        case Where::Namespace:
        case Where::NamespaceOrClosure:
            noteName(name);
            break;
        }
        return location;
    }

    Location locate(const Identifier& name)
    {
        // The variables of a comprehension hide whatever else has their names.
        for (unsigned i = m_comprehensionScopes.size(); i--;) {
            auto iterator = m_comprehensionScopes[i].find(name.impl());
            if (iterator == m_comprehensionScopes[i].end())
                continue;
            if (iterator->value)
                return { Where::Register, iterator->value.get(), false };
            return { Where::Closure };
        }
        // And what else is named in one is what it would be if the comprehension were a function, as it once was, whatever what it is now part of has by the name: push_inlined_comprehension_state(). That
        // may be the variable of another comprehension that is over. And what is in a class does not see what the class has.
        if (!m_comprehensionBlocks.isEmpty()) {
            switch (m_comprehensionBlocks.last()->scopeOf(name)) {
            case NameScope::GlobalExplicit:
                return { Where::Global };
            case NameScope::GlobalImplicit:
                if (isFunctionLike() || m_info.kind == CodeKind::Class)
                    return { Where::Global };
                break;
            case NameScope::Free:
                if (m_info.kind == CodeKind::Class)
                    return { Where::Closure };
                break;
            default:
                break;
            }
        }
        bool isClass = m_info.usesNamespace;
        // What is in a class without being part of its body looks there for what is not its own.
        bool looksInClass = isClass || m_info.canSeeClassScope;
        switch (m_block.scopeOf(name)) {
        case NameScope::Local:
            if (isClass)
                return { Where::Namespace };
            if (!isFunctionLike())
                return { Where::Global };
            // In the body of a generator, the parameters are variables of the function that made it.
            if (RegisterID* local = m_locals.get(name.impl()))
                return { Where::Register, local, m_alwaysBound.contains(name.impl()) };
            return { Where::Closure };
        case NameScope::Cell:
            return { Where::Closure };
        case NameScope::Free:
            return { looksInClass ? Where::NamespaceOrClosure : Where::Closure };
        case NameScope::GlobalExplicit:
            return { Where::Global };
        case NameScope::GlobalImplicit:
        case NameScope::Unknown:
            return { looksInClass ? Where::Namespace : Where::Global };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void emitCheckBound(RegisterID* value, const Identifier& name, const Node& node)
    {
        mark(node);
        OpCheckTdz::emit(&g, value, stringConstant(name));
    }

    enum class Checked : bool { No, Yes };

    // What a class has for what is defined in it to mean the class by is nothing to its own body, where __class__ is a name like any other: that of a variable of a function that the class is in.
    Variable variableOfEnvironment(const Identifier& name)
    {
        if (m_info.kind == CodeKind::Class && m_comprehensionBlocks.isEmpty() && (name == m_names.dunder_class || name == m_names.dunder_classdict))
            return Variable(name);
        return g.variable(name);
    }

    // If it is not checked, there may be nothing in what it is loaded into.
    RegisterID* emitLoadClosure(RegisterID* dst, const Identifier& name, const Node& node, Checked checked = Checked::Yes)
    {
        Variable variable = variableOfEnvironment(name);
        Reg scope = g.emitResolveScope(nullptr, variable);
        Reg result = destination(dst);
        g.emitGetFromScope(result.get(), scope.get(), variable, ThrowIfNotFound);
        if (m_info.variablesGivenAsCells.contains(name)) [[unlikely]] {
            // What is there is a cell. A marker stands for there being nothing in it.
            emitRuntimeCall(result.get(), "cellGet"_s, { result.get() }, node);
            Ref<Label> hasValue = g.newLabel();
            OpJneqPtr::emit(&g, result.get(), marker(), hasValue->bind(&g));
            g.moveEmptyValue(result.get());
            emitLabel(hasValue.get());
        }
        if (checked == Checked::Yes)
            emitCheckBound(result.get(), name, node);
        return result.get();
    }

    // A variable of this function's or of one that it is in, or the marker if there is nothing in it.
    void emitLoadVariableOrMarker(RegisterID* dst, const Identifier& name, const Node& node)
    {
        Location location = locateAndNote(name);
        if (location.where == Where::Register)
            g.move(dst, location.local);
        else {
            RELEASE_ASSERT(location.where == Where::Closure);
            emitLoadClosure(dst, name, node, Checked::No);
        }
        Ref<Label> hasValue = g.newLabel();
        Reg isEmpty = g.newTemporary();
        g.emitJumpIfFalse(g.emitIsEmpty(isEmpty.get(), dst), hasValue.get());
        g.move(dst, marker());
        emitLabel(hasValue.get());
    }

    // With no value, it is left with nothing in it.
    void emitStoreClosure(const Identifier& name, RegisterID* value, const Node& node) { emitStoreClosure(variableOfEnvironment(name), name, value, node); }

    void emitStoreClosure(const Variable& variable, const Identifier& name, RegisterID* value, const Node& node)
    {
        Reg scope = g.emitResolveScope(nullptr, variable);
        if (m_info.variablesGivenAsCells.contains(name)) [[unlikely]] {
            Reg cell = g.newTemporary();
            g.emitGetFromScope(cell.get(), scope.get(), variable, ThrowIfNotFound);
            emitRuntimeCall(nullptr, "cellSet"_s, { cell.get(), value ? value : marker() }, node);
            return;
        }
        Reg empty;
        if (!value) {
            empty = g.newTemporary();
            g.moveEmptyValue(empty.get());
            value = empty.get();
        }
        g.emitPutToScope(scope.get(), variable, value, ThrowIfNotFound, InitializationMode::NotInitialization);
    }

    RegisterID* emitLoadName(RegisterID* dst, const Identifier& rawName, const Node& node)
    {
        // It cannot be assigned to, and is settled when the code is compiled.
        if (rawName == m_names.dunder_debug) {
            noteConstant({ m_info.optimizationLevel ? CodeDetails::Constant::Kind::False : CodeDetails::Constant::Kind::True });
            return g.emitLoad(dst, jsBoolean(!m_info.optimizationLevel));
        }
        const Identifier& name = mangle(rawName);
        Location location = locateAndNote(name);
        switch (location.where) {
        case Where::Register:
            markIfOnAnotherLine(node);
            if (!location.isAlwaysBound)
                emitCheckBound(location.local, name, node);
            // What it is now is what is wanted, and it could be changed before that is used: by an assignment expression further on, or by anything that is
            // called and gets hold of the frame, whose f_locals writes through.
            if (!dst)
                return g.move(g.newTemporary(), location.local);
            return finish(dst, location.local);
        case Where::Closure:
            return emitLoadClosure(dst, name, node);
        case Where::Global: {
            Reg result = destination(dst);
            mark(node);
            OpPyLoadGlobal::emit(&g, result.get(), m_globals.get(), m_builtins.get(), g.addConstant(name), g.nextValueProfileIndex());
            return result.get();
        }
        case Where::Namespace:
            return emitRuntimeCall(dst, m_info.canSeeClassScope ? "loadFromDictOrGlobals"_s : "loadName"_s, { m_namespace.get(), m_globals.get(), m_builtins.get(), stringConstant(name) }, node);
        case Where::NamespaceOrClosure: {
            Reg result = temporaryDestination(dst);
            emitRuntimeCall(result.get(), "loadFromNamespace"_s, { m_namespace.get(), stringConstant(name) }, node);
            Ref<Label> found = g.newLabel();
            OpJneqPtr::emit(&g, result.get(), marker(), found->bind(&g));
            emitLoadClosure(result.get(), name, node);
            emitLabel(found.get());
            return finish(dst, result.get());
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void emitStoreName(const Identifier& rawName, RegisterID* value, const Node& node)
    {
        markIfOnAnotherLine(node);
        const Identifier& name = mangle(rawName);
        Location location = locateAndNote(name);
        switch (location.where) {
        case Where::Register:
            if (location.local != value)
                g.move(location.local, value);
            return;
        case Where::Closure:
            emitStoreClosure(name, value, node);
            return;
        case Where::Global:
            g.emitDirectPutById(m_globals.get(), name, value);
            return;
        // What is free in a class is looked for in the class first. But it is given a value only if the class says that it is `nonlocal`, and then it is the variable that is.
        case Where::NamespaceOrClosure:
            emitStoreClosure(name, value, node);
            return;
        case Where::Namespace:
            mark(node);
            OpPySetItem::emit(&g, m_namespace.get(), stringConstant(name), value);
            return;
        }
    }

    void emitDeleteName(const Identifier& rawName, const Node& node)
    {
        const Identifier& name = mangle(rawName);
        Location location = locateAndNote(name);
        switch (location.where) {
        case Where::Register:
            emitCheckBound(location.local, name, node);
            g.moveEmptyValue(location.local);
            return;
        case Where::Closure:
        case Where::NamespaceOrClosure: {
            emitLoadClosure(nullptr, name, node);
            emitStoreClosure(name, nullptr, node);
            return;
        }
        case Where::Global:
            emitRuntimeCall(nullptr, "deleteGlobal"_s, { m_globals.get(), stringConstant(name) }, node);
            return;
        case Where::Namespace:
            emitRuntimeCall(nullptr, "deleteName"_s, { m_namespace.get(), stringConstant(name) }, node);
            return;
        }
    }

    // The register to have a value put straight into, if that is all that assigning it to the name takes. The name is in co_varnames from when it is stored to, which may be after the names in the value: `NotesName::Later` is for
    // what will call noteStore() then.
    enum class NotesName : bool { Later, Now };
    RegisterID* registerForStore(Expression* target, NotesName notesName = NotesName::Now)
    {
        auto* name = target->tryAs<Name>();
        if (!name)
            return nullptr;
        Location location = locate(mangle(*name->id));
        if (location.where != Where::Register)
            return nullptr;
        if (notesName == NotesName::Now)
            noteStore(target);
        return location.local;
    }

    void noteStore(Expression* target) { locateAndNote(mangle(*target->as<Name>().id)); }

    // ---- Constants

    // As the key of a table, where nothing and all ones stand for a place that is empty and one that has been emptied.
    static unsigned keyOf(const CodeDetails::Constant& constant)
    {
        unsigned hash = hashOfConstant(constant);
        return AlreadyHashed::avoidDeletedValue(hash ? hash : 1);
    }

    // co_consts is put together as CPython puts it together. What is written out in the source is added as it is first come to, whether or not anything comes to be made of it. What is worked out from that is added
    // afterwards, once all of the code has been gone through. Then what nothing loads is taken out again, but for the first, which might have been a docstring. An int from 0 to 255 is not loaded from there.
    enum class ConstantUse : uint8_t { IsPartOfAnother, IsLoaded };

    using NotedConstants = HashMap<unsigned, Vector<unsigned, 1>, AlreadyHashed>; // Which of them have a hash.

    static unsigned noteConstantIn(Vector<CodeDetails::Constant>& constants, Vector<bool>& isLoaded, NotedConstants& noted, CodeDetails::Constant&& constant, ConstantUse use)
    {
        bool loads = use == ConstantUse::IsLoaded && !isSmallInt(constant);
        auto& alike = noted.add(keyOf(constant), Vector<unsigned, 1>()).iterator->value;
        for (unsigned index : alike) {
            if (constants[index] == constant) {
                isLoaded[index] = isLoaded[index] || loads;
                return index;
            }
        }
        alike.append(constants.size());
        constants.append(WTF::move(constant));
        isLoaded.append(loads);
        return constants.size() - 1;
    }

    // Each once. As with the names, there can be as many as there are lines.
    unsigned noteConstant(CodeDetails::Constant&& constant, ConstantUse use = ConstantUse::IsLoaded)
    {
        if (m_isNeverComeTo)
            return 0;
        return noteConstantIn(m_details->constants, m_constantIsLoaded, m_notedConstants, WTF::move(constant), use);
    }

    unsigned noteWorkedOutConstant(CodeDetails::Constant&& constant, ConstantUse use)
    {
        if (m_isNeverComeTo || isSmallInt(constant))
            return 0;
        return noteConstantIn(m_workedOutConstants, m_workedOutConstantIsLoaded, m_notedWorkedOutConstants, WTF::move(constant), use);
    }

    // What stands, among the constants of the code, for one that is an object of a realm's. See PyCodeConstant.h.
    JSValue codeConstantFor(bool isWorkedOut, unsigned index)
    {
        // Nought is not something that can be a key.
        return m_codeConstants.ensure((static_cast<uint64_t>(index) + 1) << 1 | isWorkedOut, [&] {
            return PyCodeConstant::create(m_vm);
        }).iterator->value;
    }

    void finishConstants()
    {
        auto& constants = m_details->constants;
        // What was worked out, after what was written, unless it was written as well.
        Vector<unsigned> placeOfWorkedOut;
        for (unsigned i = 0; i < m_workedOutConstants.size(); ++i)
            placeOfWorkedOut.append(noteConstantIn(constants, m_constantIsLoaded, m_notedConstants, WTF::move(m_workedOutConstants[i]), m_workedOutConstantIsLoaded[i] ? ConstantUse::IsLoaded : ConstantUse::IsPartOfAnother));
        Vector<unsigned> place(constants.size());
        unsigned kept = 0;
        for (unsigned i = 0; i < constants.size(); ++i) {
            if (i && !m_constantIsLoaded[i])
                continue;
            place[i] = kept;
            if (kept != i)
                constants[kept] = WTF::move(constants[i]);
            ++kept;
        }
        constants.shrink(kept);
        for (auto& [key, cell] : m_codeConstants)
            cell->setIndex(place[key & 1 ? placeOfWorkedOut[(key >> 1) - 1] : (key >> 1) - 1]);
    }

    // What running off the end comes to. If the last thing was to return or to raise, and nothing jumps to after it, the end is not come to.
    void emitReturnAtEnd()
    {
        // It is there whether or not it is come to, so it is among the constants if there is no other.
        OpcodeID last = g.lastOpcodeID();
        bool isComeTo = last != op_py_ret && last != op_ret && last != op_throw && !m_endIsNeverComeTo;
        noteConstant({ CodeDetails::Constant::Kind::None }, isComeTo ? ConstantUse::IsLoaded : ConstantUse::IsPartOfAnother);
        // It is on no line of its own, and is said to be where the last thing that was written is.
        if (m_lastMarked.end)
            g.emitExpressionInfo(JSTextPosition(m_lastMarked.start), JSTextPosition(m_lastMarked.start), JSTextPosition(m_lastMarked.end));
        g.emitReturn(none());
    }

    // That of a function is the first of them.
    void noteDocstring()
    {
        if (!m_info.docstring.isNull())
            noteConstant({ CodeDetails::Constant::Kind::String, 10, false, 0, m_info.docstring });
    }

    static CodeDetails::Constant describeConstant(const Constant& node, bool isNegated = false)
    {
        using Kind = CodeDetails::Constant::Kind;
        bool isNegative = node.isNegative != isNegated;
        switch (node.type) {
        case Constant::Type::None:
            return { Kind::None };
        case Constant::Type::True:
            return { Kind::True };
        case Constant::Type::False:
            return { Kind::False };
        case Constant::Type::Ellipsis:
            return { Kind::Ellipsis };
        case Constant::Type::Integer:
            return { Kind::Integer, 10, isNegative && node.integer, node.integer };
        case Constant::Type::BigInteger:
            return { Kind::BigInteger, node.radix, isNegative, 0, node.text->string() };
        case Constant::Type::Float:
            return { Kind::Float, 10, false, std::bit_cast<uint64_t>(isNegated ? -node.real : node.real) };
        case Constant::Type::Imaginary:
            return { Kind::Imaginary, 10, false, std::bit_cast<uint64_t>(node.real) };
        case Constant::Type::String:
            return { Kind::String, 10, false, 0, node.text->string() };
        case Constant::Type::Bytes:
            return { Kind::Bytes, 10, false, 0, node.text->string() };
        case Constant::Type::Complex:
            return { Kind::Complex, 10, false, std::bit_cast<uint64_t>(node.real), { }, std::bit_cast<uint64_t>(node.imaginary) };
        case Constant::Type::Tuple:
        case Constant::Type::FrozenSet: {
            CodeDetails::Constant result { node.type == Constant::Type::Tuple ? Kind::Tuple : Kind::FrozenSet };
            for (Constant* element : node.elements)
                result.elements.append(describeConstant(*element));
            return result;
        }
        case Constant::Type::Invalid:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    RegisterID* emitConstant(RegisterID* dst, Constant& node)
    {
        markIfOnAnotherLine(node);
        return emitLoadOfConstant(dst, describeConstant(node), node, false);
    }

    // Notes it, and loads it. What is an object of a realm's is made when the code is linked, and is the same one each time.
    RegisterID* emitLoadOfConstant(RegisterID* dst, CodeDetails::Constant&& value, const Node& location, bool isWorkedOut)
    {
        if (isObjectOfRealm(value) && canBeShared(value) && !m_isNeverComeTo) {
            unsigned index = isWorkedOut ? noteWorkedOutConstant(WTF::move(value), ConstantUse::IsLoaded) : noteConstant(WTF::move(value));
            return g.emitLoad(dst, codeConstantFor(isWorkedOut, index));
        }
        RegisterID* result = emitValueOfConstant(dst, value, location);
        if (isWorkedOut)
            noteWorkedOutConstant(WTF::move(value), ConstantUse::IsLoaded);
        else
            noteConstant(WTF::move(value));
        return result;
    }

    const Identifier& identifierFor(const String& text)
    {
        if (text.is8Bit())
            return m_arena.identifiers().makeIdentifier(m_vm, text.span8());
        return m_arena.identifiers().makeIdentifier(m_vm, text.span16());
    }

    // What is no object, or is one of no realm in particular, is a constant of the engine's. The rest is made here and now.
    RegisterID* emitValueOfConstant(RegisterID* dst, const CodeDetails::Constant& value, const Node& location)
    {
        using Kind = CodeDetails::Constant::Kind;
        switch (value.kind) {
        case Kind::None:
            return g.emitLoad(dst, jsUndefined());
        case Kind::True:
            return g.emitLoad(dst, jsBoolean(true));
        case Kind::False:
            return g.emitLoad(dst, jsBoolean(false));
        case Kind::Ellipsis: {
            Reg result = destination(dst);
            return g.emitGetById(result.get(), runtime(), Identifier::fromString(m_vm, "Ellipsis"_s));
        }
        case Kind::Integer:
            if (value.bits <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + value.isNegative)
                return g.emitLoad(dst, jsNumber(static_cast<int32_t>(value.isNegative ? -static_cast<int64_t>(value.bits) : static_cast<int64_t>(value.bits))));
            // The generator knows a constant that it has seen before by the address of its digits, so they have to outlive it.
            return g.emitLoad(dst, g.addBigIntConstant(identifierFor(String::number(value.bits)), 10, value.isNegative));
        case Kind::BigInteger:
            return g.emitLoad(dst, g.addBigIntConstant(identifierFor(value.text), value.radix, value.isNegative));
        case Kind::Float:
            return g.emitLoad(dst, floatFromDouble(std::bit_cast<double>(value.bits)));
        case Kind::Imaginary:
            return emitRuntimeCall(dst, "newComplex"_s, { constant(jsDoubleNumber(purifyNaNKeepingPayload(std::bit_cast<double>(value.bits)))) }, location);
        case Kind::String:
            return g.emitLoad(dst, JSValue(stringFor(identifierFor(value.text))));
        case Kind::Bytes:
            return emitRuntimeCall(dst, "newBytes"_s, { stringConstant(identifierFor(value.text)) }, location);
        case Kind::Complex:
            return emitRuntimeCall(dst, "newComplex"_s, { constant(jsDoubleNumber(purifyNaNKeepingPayload(std::bit_cast<double>(value.imaginaryBits)))), constant(jsDoubleNumber(purifyNaNKeepingPayload(std::bit_cast<double>(value.bits)))) }, location);
        case Kind::Tuple:
        case Kind::FrozenSet: {
            if (!m_vm.isSafeToRecurse()) [[unlikely]] {
                fail("maximum recursion depth exceeded during compilation"_s, location);
                return g.emitLoad(dst, jsUndefined());
            }
            Vector<Reg, 8> elements;
            for (auto& element : value.elements)
                elements.append(emitValueOfConstant(g.newTemporary(), element, location));
            Reg result = destination(dst);
            emitNewTuple(result.get(), elements);
            if (value.kind == Kind::FrozenSet)
                emitRuntimeCall(result.get(), "newFrozenSet"_s, { result.get() }, location);
            return result.get();
        }
        case Kind::Slice: {
            Reg lower = emitValueOfConstant(g.newTemporary(), value.elements[0], location);
            Reg upper = emitValueOfConstant(g.newTemporary(), value.elements[1], location);
            Reg step = emitValueOfConstant(g.newTemporary(), value.elements[2], location);
            return emitRuntimeCall(dst, "newSlice"_s, { lower.get(), upper.get(), step.get() }, location);
        }
        case Kind::Code:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // ---- What can be worked out beforehand

    // What an expression comes to, if it is made of nothing but constants and that can be told now. See PythonConstantFolding.h. Each is asked about once, however deep it is in what else is asked about.
    const CodeDetails::Constant* constantOf(Expression* expression)
    {
        switch (expression->kind) {
        case Expression::Kind::Constant:
        case Expression::Kind::UnaryOp:
        case Expression::Kind::BinOp:
        case Expression::Kind::Tuple:
        case Expression::Kind::Subscript:
        case Expression::Kind::Slice:
            break;
        default:
            return nullptr;
        }
        if (auto known = m_constantOfExpression.find(expression); known != m_constantOfExpression.end())
            return known->value.get();
        std::optional<CodeDetails::Constant> value;
        if (m_vm.isSafeToRecurse()) [[likely]]
            value = workOutConstant(expression);
        return m_constantOfExpression.add(expression, value ? makeUniqueWithoutFastMallocCheck<CodeDetails::Constant>(WTF::move(*value)) : nullptr).iterator->value.get();
    }

    std::optional<CodeDetails::Constant> workOutConstant(Expression* expression)
    {
        switch (expression->kind) {
        case Expression::Kind::Constant:
            if (expression->as<Constant>().type == Constant::Type::Invalid)
                return std::nullopt;
            return describeConstant(expression->as<Constant>());
        case Expression::Kind::UnaryOp: {
            auto& node = expression->as<UnaryOp>();
            auto* operand = constantOf(node.operand);
            return operand ? foldUnaryOperation(node.op, *operand) : std::nullopt;
        }
        case Expression::Kind::BinOp: {
            auto& node = expression->as<BinOp>();
            auto* left = constantOf(node.left);
            auto* right = left ? constantOf(node.right) : nullptr;
            return right ? foldBinaryOperation(node.op, *left, *right) : std::nullopt;
        }
        case Expression::Kind::Tuple: {
            auto& node = expression->as<Tuple>();
            if (node.context != ExpressionContext::Load)
                return std::nullopt;
            CodeDetails::Constant result { CodeDetails::Constant::Kind::Tuple };
            for (Expression* element : node.elements) {
                auto* value = constantOf(element);
                if (!value)
                    return std::nullopt;
                result.elements.append(*value);
            }
            return result;
        }
        case Expression::Kind::Subscript: {
            auto& node = expression->as<Subscript>();
            if (node.context != ExpressionContext::Load)
                return std::nullopt;
            auto* value = constantOf(node.value);
            auto* index = value ? constantOf(node.slice) : nullptr;
            return index ? foldSubscript(*value, *index) : std::nullopt;
        }
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            CodeDetails::Constant result { CodeDetails::Constant::Kind::Slice };
            for (Expression* part : { node.lower, node.upper, node.step }) {
                auto* value = part ? constantOf(part) : nullptr;
                if (part && !value)
                    return std::nullopt;
                result.elements.append(part ? *value : CodeDetails::Constant { });
            }
            return result;
        }
        default:
            return std::nullopt;
        }
    }

    // CPython makes a slice of what is written out when it first comes to it, and nothing is kept of what it was made of. Of what has itself to be worked out, as -1 has, it makes none. Here one is made all the same.
    static bool isWrittenOut(const Slice& node)
    {
        return (!node.lower || node.lower->is<Constant>()) && (!node.upper || node.upper->is<Constant>()) && (!node.step || node.step->is<Constant>());
    }

    // What it was worked out from, and what was worked out on the way, in the order that CPython comes to them.
    void notePartsOfConstant(Expression* expression, bool isWhole)
    {
        if (auto* leaf = expression->tryAs<Constant>()) {
            noteConstant(describeConstant(*leaf), ConstantUse::IsPartOfAnother);
            return;
        }
        if (auto* slice = expression->tryAs<Slice>(); slice && isWrittenOut(*slice)) {
            if (!isWhole)
                noteConstant(CodeDetails::Constant(*constantOf(expression)), ConstantUse::IsPartOfAnother);
            return;
        }
        switch (expression->kind) {
        case Expression::Kind::UnaryOp:
            notePartsOfConstant(expression->as<UnaryOp>().operand, false);
            break;
        case Expression::Kind::BinOp:
            notePartsOfConstant(expression->as<BinOp>().left, false);
            notePartsOfConstant(expression->as<BinOp>().right, false);
            break;
        case Expression::Kind::Tuple:
            for (Expression* element : expression->as<Tuple>().elements)
                notePartsOfConstant(element, false);
            break;
        case Expression::Kind::Subscript:
            notePartsOfConstant(expression->as<Subscript>().value, false);
            notePartsOfConstant(expression->as<Subscript>().slice, false);
            break;
        case Expression::Kind::Slice:
            for (Expression* part : { expression->as<Slice>().lower, expression->as<Slice>().upper, expression->as<Slice>().step }) {
                if (part)
                    notePartsOfConstant(part, false);
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (!isWhole)
            noteWorkedOutConstant(CodeDetails::Constant(*constantOf(expression)), ConstantUse::IsPartOfAnother);
    }

    // optimize_lists_and_sets(): the tuple of what is in a list that is written out, or the frozenset of what is in a set, if those are all constants and there are no more of them than CPython would have on its stack.
    std::optional<CodeDetails::Constant> constantOfDisplay(Expression* expression)
    {
        static constexpr unsigned most = 30; // _PY_STACK_USE_GUIDELINE
        auto* list = expression->tryAs<List>();
        auto* set = expression->tryAs<Set>();
        if (!(list && list->context == ExpressionContext::Load) && !set)
            return std::nullopt;
        Sequence<Expression*> elements = list ? list->elements : set->elements;
        if (elements.size() > most)
            return std::nullopt;
        CodeDetails::Constant result { list ? CodeDetails::Constant::Kind::Tuple : CodeDetails::Constant::Kind::FrozenSet };
        for (Expression* element : elements) {
            auto* value = constantOf(element);
            if (!value)
                return std::nullopt;
            result.elements.append(*value);
        }
        return result;
    }

    RegisterID* emitConstantOfDisplay(RegisterID* dst, Expression* expression, CodeDetails::Constant&& value)
    {
        for (Expression* element : expression->is<List>() ? expression->as<List>().elements : expression->as<Set>().elements)
            notePartsOfConstant(element, false);
        markIfOnAnotherLine(*expression);
        return emitLoadOfConstant(dst, WTF::move(value), *expression, true);
    }

    // What is only going to be looked in, with `in`, or gone through. A list or a set that is written out for that need not be made each time.
    RegisterID* emitToLookInOrGoThrough(Expression* expression)
    {
        if (auto value = constantOfDisplay(expression))
            return emitConstantOfDisplay(nullptr, expression, WTF::move(*value));
        return emit(expression);
    }

    RegisterID* emitWorkedOutConstant(RegisterID* dst, Expression* expression, const CodeDetails::Constant& value)
    {
        notePartsOfConstant(expression, true);
        markIfOnAnotherLine(*expression);
        auto* slice = expression->tryAs<Slice>();
        return emitLoadOfConstant(dst, CodeDetails::Constant(value), *expression, !(slice && isWrittenOut(*slice)));
    }

    // ---- Expressions

    RegisterID* emit(Expression* expression, RegisterID* dst = nullptr)
    {
        unsigned lineBefore = m_lastMarkedLine;
        unsigned numberOfLines = m_numberOfLines;
        RegisterID* result = emitWithoutLine(expression, dst);
        // What is all on one line leaves off on that line, whichever way through it was taken, if it has been said that it is on it.
        if (!m_lastMarkedLine && expression->line == expression->endLine && (lineBefore == expression->line || m_numberOfLines != numberOfLines))
            m_lastMarkedLine = expression->line;
        return result;
    }

    RegisterID* emitWithoutLine(Expression* expression, RegisterID* dst)
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            fail("maximum recursion depth exceeded during compilation"_s, *expression);
            return g.emitLoad(dst, jsUndefined());
        }
        if (!expression->is<Constant>() && expression != m_tupleThatIsUnpacked) {
            if (auto* value = constantOf(expression))
                return emitWorkedOutConstant(dst, expression, *value);
        }
        switch (expression->kind) {
        case Expression::Kind::Constant:
            return emitConstant(dst, expression->as<Constant>());
        case Expression::Kind::Name:
            return emitLoadName(dst, *expression->as<Name>().id, *expression);
        case Expression::Kind::BinOp: {
            auto& node = expression->as<BinOp>();
            if (node.op == BinaryOperator::Mod) {
                if (Expression* joined = formatAsJoinedString(m_vm, m_arena, node))
                    return emit(joined, dst);
            }
            Reg left = emit(node.left);
            Reg right = emit(node.right);
            Reg result = destination(dst);
            mark(node);
            return emitBinaryOperation(result.get(), node.op, false, left.get(), right.get());
        }
        case Expression::Kind::UnaryOp: {
            auto& node = expression->as<UnaryOp>();
            Reg operand = emit(node.operand);
            Reg result = destination(dst);
            mark(node);
            OpPyUnaryOp::emit(&g, result.get(), operand.get(), static_cast<unsigned>(node.op), g.nextValueProfileIndex());
            return result.get();
        }
        case Expression::Kind::BoolOp: {
            auto& node = expression->as<BoolOp>();
            Reg result = temporaryDestination(dst);
            Ref<Label> end = g.newLabel();
            for (size_t i = 0; i < node.values.size(); ++i) {
                emitInto(result.get(), node.values[i]);
                if (i + 1 == node.values.size())
                    break;
                mark(node);
                if (node.op == BooleanOperator::And)
                    emitJumpIfFalse(result.get(), end.get());
                else
                    emitJumpIfTrue(result.get(), end.get());
            }
            emitLabel(end.get());
            return finish(dst, result.get());
        }
        case Expression::Kind::IfExp: {
            auto& node = expression->as<IfExp>();
            Reg result = temporaryDestination(dst);
            Ref<Label> otherwise = g.newLabel();
            Ref<Label> end = g.newLabel();
            // If it is plain which it will be, the other is compiled only to find what is wrong with it.
            auto truth = constantTruth(node.test);
            emitBranch(node.test, otherwise.get(), false);
            {
                SetForScope isNeverComeTo(m_isNeverComeTo, m_isNeverComeTo || truth == std::optional { false });
                emitInto(result.get(), node.body);
            }
            emitJump(end.get());
            emitLabel(otherwise.get());
            {
                SetForScope isNeverComeTo(m_isNeverComeTo, m_isNeverComeTo || truth == std::optional { true });
                emitInto(result.get(), node.orElse);
            }
            emitLabel(end.get());
            return finish(dst, result.get());
        }
        case Expression::Kind::Compare:
            return emitComparison(dst, expression->as<Compare>());
        case Expression::Kind::NamedExpr: {
            auto& node = expression->as<NamedExpr>();
            Reg value = emitToTemporary(node.value);
            auto& target = node.target->as<Name>();
            // Nothing sees to it that a tree which a program made says that the name is assigned to, and CPython does with it as it says.
            switch (target.context) {
            case ExpressionContext::Store:
                emitStoreName(*target.id, value.get(), node);
                break;
            case ExpressionContext::Del:
                emitDeleteName(*target.id, target);
                break;
            case ExpressionContext::Load:
                return emitLoadName(dst, *target.id, target);
            }
            return finish(dst, value.get());
        }
        case Expression::Kind::Attribute: {
            auto& node = expression->as<Attribute>();
            Reg base = emit(node.value);
            Reg result = destination(dst);
            mark(locationOf(node));
            return emitGetAttribute(result.get(), base.get(), mangle(*node.attribute));
        }
        case Expression::Kind::Subscript: {
            auto& node = expression->as<Subscript>();
            Reg base = emit(node.value);
            Reg key = emit(node.slice);
            Reg result = destination(dst);
            mark(node);
            return emitGetItem(result.get(), base.get(), key.get());
        }
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            Reg lower = node.lower ? Reg(emit(node.lower)) : Reg(none());
            Reg upper = node.upper ? Reg(emit(node.upper)) : Reg(none());
            Reg step = node.step ? Reg(emit(node.step)) : Reg(none());
            return emitRuntimeCall(dst, "newSlice"_s, { lower.get(), upper.get(), step.get() }, node);
        }
        case Expression::Kind::Tuple:
            return emitSequenceDisplay(dst, expression->as<Tuple>().elements, Display::Tuple, *expression);
        case Expression::Kind::List:
            return emitSequenceDisplay(dst, expression->as<List>().elements, Display::List, *expression);
        case Expression::Kind::Set:
            return emitSequenceDisplay(dst, expression->as<Set>().elements, Display::Set, *expression);
        case Expression::Kind::Dict:
            return emitDictDisplay(dst, expression->as<Dict>());
        case Expression::Kind::Call:
            return emitCall(dst, expression->as<Call>());
        case Expression::Kind::Lambda: {
            auto& node = expression->as<Lambda>();
            return emitFunction(dst, CodeKind::Lambda, m_arena.identifiers().makeIdentifier(m_vm, "<lambda>"_span8), node.arguments, node, &node);
        }
        case Expression::Kind::JoinedStr:
            return emitJoinedString(dst, expression->as<JoinedStr>());
        case Expression::Kind::FormattedValue: {
            // Only ever inside a JoinedStr, but a format specification can be one alone.
            return emitFormattedValue(dst, expression->as<FormattedValue>());
        }
        case Expression::Kind::ListComp: {
            auto& node = expression->as<ListComp>();
            return emitComprehension(dst, node, ComprehensionType::List, node.generators, node.element, nullptr);
        }
        case Expression::Kind::SetComp: {
            auto& node = expression->as<SetComp>();
            return emitComprehension(dst, node, ComprehensionType::Set, node.generators, node.element, nullptr);
        }
        case Expression::Kind::DictComp: {
            auto& node = expression->as<DictComp>();
            return emitComprehension(dst, node, ComprehensionType::Dict, node.generators, node.key, node.value);
        }
        case Expression::Kind::GeneratorExp:
            return emitGeneratorExpression(dst, expression->as<GeneratorExp>());
        case Expression::Kind::Yield:
            return emitYield(dst, expression->as<Yield>());
        case Expression::Kind::YieldFrom:
            return emitYieldFrom(dst, expression->as<YieldFrom>());
        case Expression::Kind::Starred:
            fail("can't use starred expression here"_s, *expression);
            return g.emitLoad(dst, jsUndefined());
        case Expression::Kind::Await:
            return emitAwait(dst, expression->as<Await>());
        case Expression::Kind::TemplateStr:
            return emitTemplateString(dst, expression->as<TemplateStr>());
        case Expression::Kind::Interpolation:
            return emitInterpolation(dst, expression->as<Interpolation>());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // a < b < c is a < b and b < c, with b evaluated once.
    RegisterID* emitComparison(RegisterID* dst, Compare& node)
    {
        if (node.ops.size() == 1) {
            Reg left = emit(node.left);
            if (isNoneConstant(node.comparators[0]) && (node.ops[0] == ComparisonOperator::Is || node.ops[0] == ComparisonOperator::IsNot)) {
                Reg result = destination(dst);
                g.emitIsUndefinedOrNull(result.get(), left.get());
                if (node.ops[0] == ComparisonOperator::IsNot)
                    g.emitUnaryOp<OpNot>(result.get(), result.get());
                return result.get();
            }
            bool looksIn = node.ops[0] == ComparisonOperator::In || node.ops[0] == ComparisonOperator::NotIn;
            Reg right = looksIn ? emitToLookInOrGoThrough(node.comparators[0]) : emit(node.comparators[0]);
            Reg result = destination(dst);
            mark(node);
            return emitCompare(result.get(), node.ops[0], left.get(), right.get());
        }
        Reg result = temporaryDestination(dst);
        Ref<Label> end = g.newLabel();
        Reg left = emitToTemporary(node.left);
        for (size_t i = 0; i < node.ops.size(); ++i) {
            // The last is looked in and is done with.
            bool looksIn = i + 1 == node.ops.size() && (node.ops[i] == ComparisonOperator::In || node.ops[i] == ComparisonOperator::NotIn);
            Reg right = looksIn ? Reg(emitToLookInOrGoThrough(node.comparators[i])) : emitToTemporary(node.comparators[i]);
            mark(node);
            emitCompare(result.get(), node.ops[i], left.get(), right.get());
            if (i + 1 < node.ops.size())
                emitJumpIfFalse(result.get(), end.get());
            left = right;
        }
        emitLabel(end.get());
        return finish(dst, result.get());
    }

    // ---- Displays

    enum class Display : uint8_t { Tuple, List, Set };

    static bool hasStarred(Sequence<Expression*> elements)
    {
        for (Expression* element : elements) {
            if (element->is<Starred>())
                return true;
        }
        return false;
    }

    // Each in a register of its own, one after another.
    void emitElements(Sequence<Expression*> elements, Vector<Reg, 8>& registers)
    {
        for (Expression* element : elements) {
            registers.append(g.newTemporary());
            emitInto(registers.last().get(), element);
        }
    }

    RegisterID* emitNewList(RegisterID* dst, const Vector<Reg, 8>& elements)
    {
        OpNewArray::emit(&g, dst, elements.isEmpty() ? VirtualRegister { 0 } : elements[0]->virtualRegister(), elements.size(), ArrayWithUndecided);
        return dst;
    }

    RegisterID* emitNewTuple(RegisterID* dst, const Vector<Reg, 8>& elements)
    {
        OpPyNewTuple::emit(&g, dst, elements.isEmpty() ? VirtualRegister { 0 } : elements[0]->virtualRegister(), elements.size());
        return dst;
    }

    // A list of the elements, some of which are *iterables to be taken apart.
    // `callee` is for f(*x), which says of what is wrong with x that it is wrong for f.
    RegisterID* emitListWithStarred(RegisterID* dst, Sequence<Expression*> elements, const Node& node, RegisterID* callee = nullptr)
    {
        size_t plain = 0;
        while (plain < elements.size() && !elements[plain]->is<Starred>())
            ++plain;
        {
            Vector<Reg, 8> registers;
            emitElements(elements.first(plain), registers);
            emitNewList(dst, registers);
        }
        for (size_t i = plain; i < elements.size(); ++i) {
            if (auto* starred = elements[i]->tryAs<Starred>()) {
                Reg iterable = emit(starred->value);
                emitRuntimeCall(nullptr, "listExtend"_s, { dst, iterable.get(), callee && elements.size() == 1 ? callee : marker() }, node);
            } else {
                Reg value = emit(elements[i]);
                emitRuntimeCall(nullptr, "listAppend"_s, { dst, value.get() }, node);
            }
        }
        return dst;
    }

    // What a constant is to the engine, if it is one of the engine's: a number that is not too large, a string, None, True or False.
    JSValue plainValueOf(const CodeDetails::Constant& value)
    {
        using Kind = CodeDetails::Constant::Kind;
        switch (value.kind) {
        case Kind::None:
            return jsUndefined();
        case Kind::True:
            return jsBoolean(true);
        case Kind::False:
            return jsBoolean(false);
        case Kind::Integer:
            if (value.bits > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + value.isNegative)
                return { };
            return jsNumber(static_cast<int32_t>(value.isNegative ? -static_cast<int64_t>(value.bits) : static_cast<int64_t>(value.bits)));
        case Kind::Float:
            return floatFromDouble(std::bit_cast<double>(value.bits));
        case Kind::String:
            return g.addStringConstant(identifierFor(value.text));
        default:
            return { };
        }
    }

    // [1, 2, 3] is what it is in JavaScript: an array that has what is in it in common with every other that the same code makes, until one of them is written to. Null if there is more to it than that.
    RegisterID* emitListOfPlainConstants(RegisterID* dst, Sequence<Expression*> elements, const Node& node)
    {
        if (elements.empty() || elements.size() > MAX_STORAGE_VECTOR_LENGTH)
            return nullptr;
        for (Expression* element : elements) {
            auto* value = constantOf(element);
            if (!value || isObjectOfRealm(*value) || value->kind == CodeDetails::Constant::Kind::BigInteger || (value->kind == CodeDetails::Constant::Kind::Integer && !plainValueOf(*value)))
                return nullptr;
        }
        ASSERT(m_vm.heap.isDeferred());
        auto* array = JSCellButterfly::tryCreate(m_vm, CopyOnWriteArrayWithContiguous, elements.size());
        if (!array)
            return nullptr;
        unsigned index = 0;
        for (Expression* element : elements)
            array->setIndex(m_vm, index++, plainValueOf(*constantOf(element)));
        // To CPython it is a copy of one constant if there are from 3 to 30 of them, and otherwise each is loaded.
        auto* expression = const_cast<Expression*>(static_cast<const Expression*>(&node));
        auto whole = elements.size() >= 3 ? constantOfDisplay(expression) : std::nullopt;
        for (Expression* element : elements) {
            notePartsOfConstant(element, !whole);
            if (whole)
                continue;
            if (element->is<Constant>())
                noteConstant(CodeDetails::Constant(*constantOf(element)));
            else
                noteWorkedOutConstant(CodeDetails::Constant(*constantOf(element)), ConstantUse::IsLoaded);
        }
        if (whole)
            noteWorkedOutConstant(WTF::move(*whole), ConstantUse::IsLoaded);
        markIfOnAnotherLine(node);
        return g.emitNewArrayBuffer(destination(dst).get(), array, CopyOnWriteArrayWithContiguous);
    }

    RegisterID* emitSequenceDisplay(RegisterID* dst, Sequence<Expression*> elements, Display display, const Node& node)
    {
        if (hasStarred(elements) && display == Display::Set) {
            // Each goes in as it is come to, so what cannot be in a set is found before what comes after it is evaluated.
            size_t plain = 0;
            while (!elements[plain]->is<Starred>())
                ++plain;
            Reg set = g.newTemporary();
            {
                Vector<Reg, 8> registers;
                emitElements(elements.first(plain), registers);
                emitRuntimeCall(set.get(), "newSet"_s, registers, node);
            }
            for (size_t i = plain; i < elements.size(); ++i) {
                auto* starred = elements[i]->tryAs<Starred>();
                Reg value = emit(starred ? starred->value : elements[i]);
                emitRuntimeCall(nullptr, starred ? "setUpdate"_s : "setAdd"_s, { set.get(), value.get() }, node);
            }
            return finish(dst, set.get());
        }
        if (hasStarred(elements)) {
            Reg list = g.newTemporary();
            emitListWithStarred(list.get(), elements, node);
            if (display == Display::List)
                return finish(dst, list.get());
            return emitRuntimeCall(dst, "listToTuple"_s, { list.get() }, node);
        }
        if (display == Display::List && !m_isNeverComeTo) {
            if (RegisterID* list = emitListOfPlainConstants(dst, elements, node))
                return list;
        }
        // Three or more constants are one constant, of which a copy is taken.
        if (display != Display::Tuple && elements.size() >= 3 && !m_isNeverComeTo) {
            auto* expression = const_cast<Expression*>(static_cast<const Expression*>(&node));
            if (auto value = constantOfDisplay(expression); value && canBeShared(*value)) {
                Reg whole = emitConstantOfDisplay(nullptr, expression, WTF::move(*value));
                return emitRuntimeCall(dst, display == Display::List ? "listOfTuple"_s : "setOfFrozenSet"_s, { whole.get() }, node);
            }
        }
        Vector<Reg, 8> registers;
        {
            bool isOneConstant = (display == Display::Tuple || elements.size() >= 3) && &node != m_tupleThatIsUnpacked && std::ranges::all_of(elements, isConstant);
            SetForScope isInConstantDisplay(m_isInConstantDisplay, m_isInConstantDisplay || isOneConstant);
            emitElements(elements, registers);
        }
        markIfOnAnotherLine(node);
        switch (display) {
        case Display::List:
            return emitNewList(destination(dst).get(), registers);
        case Display::Tuple:
            return emitNewTuple(destination(dst).get(), registers);
        case Display::Set:
            return emitRuntimeCall(dst, "newSet"_s, registers, node);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    RegisterID* emitDictDisplay(RegisterID* dst, Dict& node)
    {
        bool hasUnpacking = false;
        for (Expression* key : node.keys)
            hasUnpacking |= !key;
        if (!hasUnpacking && node.keys.size() <= 16) {
            // Keys and values by turns.
            Vector<Reg, 8> registers;
            for (size_t i = 0; i < node.keys.size(); ++i) {
                registers.append(g.newTemporary());
                emitInto(registers.last().get(), node.keys[i]);
                registers.append(g.newTemporary());
                emitInto(registers.last().get(), node.values[i]);
            }
            return emitRuntimeCall(dst, "newDict"_s, registers, node);
        }
        Reg dict = g.newTemporary();
        emitRuntimeCall(dict.get(), "newDict"_s, { }, node);
        for (size_t i = 0; i < node.keys.size(); ++i) {
            if (!node.keys[i]) {
                Reg mapping = emit(node.values[i]);
                emitRuntimeCall(nullptr, "dictUpdate"_s, { dict.get(), mapping.get() }, node);
                continue;
            }
            Reg key = emit(node.keys[i]);
            Reg value = emit(node.values[i]);
            mark(*node.keys[i]);
            OpPySetItem::emit(&g, dict.get(), key.get(), value.get());
        }
        return finish(dst, dict.get());
    }

    // ---- f-strings

    void checkConversion(int conversion)
    {
        if (conversion != -1 && conversion != 's' && conversion != 'r' && conversion != 'a')
            fail(SyntaxError::Kind::SystemError, concatenate("Unrecognized conversion character "_s, conversion));
    }

    RegisterID* emitFormattedValue(RegisterID* dst, FormattedValue& node)
    {
        Reg value = emit(node.value);
        checkConversion(node.conversion);
        Reg conversion = constant(jsNumber(node.conversion));
        if (!node.formatSpecification)
            return emitRuntimeCall(dst, "formatValue"_s, { value.get(), conversion.get() }, node);
        Reg specification = emit(node.formatSpecification);
        return emitRuntimeCall(dst, "formatValue"_s, { value.get(), conversion.get(), specification.get() }, node);
    }

    RegisterID* emitJoinedString(RegisterID* dst, JoinedStr& node)
    {
        if (node.values.empty())
            return g.emitLoad(dst, m_vm.propertyNames->emptyIdentifier);
        if (node.values.size() == 1)
            return emit(node.values[0], dst);
        Vector<Reg, 8> pieces;
        emitElements(node.values, pieces);
        Reg result = destination(dst);
        bool areAllStrings = std::ranges::all_of(node.values, [] (Expression* value) {
            auto* constant = value->tryAs<Constant>();
            return value->is<FormattedValue>() || (constant && constant->type == Constant::Type::String);
        });
        // Then this is JavaScript's concatenation. There is no writing anything else, but a tree that a program made can have it.
        if (areAllStrings)
            return g.emitStrcat(result.get(), pieces[0].get(), pieces.size());
        emitNewTuple(result.get(), pieces);
        return emitRuntimeCall(result.get(), "joinStrings"_s, { result.get() }, node);
    }

    // {value!r:specification} in a t"..."
    RegisterID* emitInterpolation(RegisterID* dst, Interpolation& node)
    {
        Reg value = emitToTemporary(node.value);
        Reg formatSpecification = node.formatSpecification ? Reg(emitToTemporary(node.formatSpecification)) : Reg(g.emitLoad(g.newTemporary(), m_vm.propertyNames->emptyIdentifier));
        checkConversion(node.conversion);
        RegisterID* conversion = none();
        if (node.conversion >= 0) {
            Latin1Character character = static_cast<Latin1Character>(node.conversion);
            conversion = stringConstant(m_arena.identifiers().makeIdentifier(m_vm, std::span<const Latin1Character> { &character, 1 }));
        }
        Reg source = emitConstant(g.newTemporary(), *node.source);
        return emitRuntimeCall(dst, "newInterpolation"_s, { value.get(), source.get(), conversion, formatSpecification.get() }, node);
    }

    // t"...". There is a string before, between and after the interpolations, if only an empty one.
    RegisterID* emitTemplateString(RegisterID* dst, TemplateStr& node)
    {
        Vector<Reg, 8> strings;
        auto addEmptyString = [&] {
            strings.append(g.newTemporary());
            g.emitLoad(strings.last().get(), m_vm.propertyNames->emptyIdentifier);
        };
        bool lastWasInterpolation = true;
        for (Expression* value : node.values) {
            if (value->is<Interpolation>()) {
                if (lastWasInterpolation)
                    addEmptyString();
                lastWasInterpolation = true;
                continue;
            }
            strings.append(emitToTemporary(value));
            lastWasInterpolation = false;
        }
        if (lastWasInterpolation)
            addEmptyString();
        Reg stringsTuple = g.newTemporary();
        emitNewTuple(stringsTuple.get(), strings);

        Vector<Reg, 8> interpolations;
        for (Expression* value : node.values) {
            if (value->is<Interpolation>())
                interpolations.append(emitToTemporary(value));
        }
        Reg interpolationsTuple = g.newTemporary();
        emitNewTuple(interpolationsTuple.get(), interpolations);
        return emitRuntimeCall(dst, "newTemplate"_s, { stringsTuple.get(), interpolationsTuple.get() }, node);
    }

    // ---- Calls

    RegisterID* marker() { return g.moveLinkTimeConstant(nullptr, LinkTimeConstant::pyBoundArgumentsMarker); }

    // With only the first so many of the arguments that there are registers for.
    // Whether it is a call that was written, of what is being called. If it is of something of the engine's that makes the call, that has been seen to.
    enum class Told : bool { No, Yes };

    RegisterID* emitRawCall(RegisterID* dst, RegisterID* function, CallArguments& call, unsigned argumentCount, const Node& node, Told told = Told::Yes)
    {
        Vector<Reg, CallFrame::headerSizeInRegisters> callFrame;
        for (int i = 0; i < CallFrame::headerSizeInRegisters; ++i)
            callFrame.append(g.newTemporary());
        mark(node);
        if (told == Told::Yes)
            emitTellOfCall(function, argumentCount ? call.argumentRegister(0) : nullptr, argumentCount ? ToldArgument::First : ToldArgument::None);
        OpCall::emit(&g, dst, function, argumentCount + 1, call.stackOffset(), g.nextValueProfileIndex());
        emitTellOfEndOfCall();
        return dst;
    }

    void emitTellOfCall(RegisterID* callee, RegisterID* argument, ToldArgument kind)
    {
        if (!m_isArtificial && !m_isNeverComeTo)
            OpPyCall::emit(&g, callee, argument ? argument : callee, static_cast<unsigned>(kind));
    }

    void emitTellOfEndOfCall()
    {
        if (!m_isArtificial && !m_isNeverComeTo)
            OpPyCalled::emit(&g);
    }

    // super() with no arguments means super(__class__, self). In CPython it is `super` that finds those, in the frame of what called it. Here it is handed them, if `super` is what is called: implicitSuper().
    // With no parameters there is nothing to hand it, and it says so itself.
    bool isImplicitSuper(Call& node)
    {
        auto* name = node.function->tryAs<Name>();
        if (!name || *name->id != "super"_s || !node.arguments.empty() || !node.keywords.empty())
            return false;
        if (!isFunctionLike() || m_info.parameterNames.isEmpty() || !m_info.positionalCount)
            return false;
        return locate(*name->id).where == Where::Global;
    }

    RegisterID* emitCall(RegisterID* dst, Call& node)
    {
        if (!node.keywords.empty() || hasStarred(node.arguments))
            return emitGeneralCall(dst, node);

        unsigned count = node.arguments.size();

        if (isImplicitSuper(node)) {
            Reg function = emitToTemporary(node.function);
            NameScope scopeOfClass = m_block.scopeOf(m_names.dunder_class);
            bool hasCell = scopeOfClass == NameScope::Free || scopeOfClass == NameScope::Cell;
            Reg classInCell = g.newTemporary();
            if (hasCell)
                emitLoadVariableOrMarker(classInCell.get(), m_names.dunder_class, node);
            else
                g.move(classInCell.get(), marker());
            Reg self = g.newTemporary();
            emitLoadVariableOrMarker(self.get(), m_info.parameterNames[0], node);
            mark(node);
            emitTellOfCall(function.get(), nullptr, ToldArgument::None);
            RegisterID* result = emitRuntimeCall(dst, "implicitSuper"_s, { function.get(), constant(jsBoolean(hasCell)), classInCell.get(), self.get() }, node);
            emitTellOfEndOfCall();
            return result;
        }

        auto* attribute = node.function->tryAs<Attribute>();
        if (!attribute) {
            Reg function = emitToTemporary(node.function);
            CallArguments call(g, nullptr, count);
            g.emitLoad(call.thisRegister(), jsUndefined());
            for (unsigned i = 0; i < count; ++i)
                emitInto(call.argumentRegister(i), node.arguments[i]);
            return emitRawCall(destination(dst).get(), function.get(), call, count, node);
        }

        // base.method(...). If it is a method of base's class, base goes in front of the arguments and no bound method is made.
        Reg base = emit(attribute->value);
        Reg function = g.newTemporary();
        CallArguments call(g, nullptr, count + 1);
        RegisterID* self = call.argumentRegister(0);
        mark(locationOf(*attribute));
        noteName(mangle(*attribute->attribute));
        OpPyLoadMethod::emit(&g, function.get(), self, base.get(), g.addConstant(mangle(*attribute->attribute)), g.nextValueProfileIndex());
        g.emitLoad(call.thisRegister(), jsUndefined());
        for (unsigned i = 0; i < count; ++i)
            emitInto(call.argumentRegister(i + 1), node.arguments[i]);

        Reg result = temporaryDestination(dst);
        Ref<Label> isNotMethod = g.newLabel();
        Ref<Label> done = g.newLabel();
        Reg isEmpty = g.newTemporary();
        g.emitIsEmpty(isEmpty.get(), self);
        g.emitJumpIfTrue(isEmpty.get(), isNotMethod.get());
        emitRawCall(result.get(), function.get(), call, count + 1, node);
        g.emitJump(done.get());
        emitLabel(isNotMethod.get());
        // If it is a function of JavaScript's, it expects what it was got from as `this`. Python's own make nothing of it.
        g.move(call.thisRegister(), base.get());
        for (unsigned i = 0; i < count; ++i)
            g.move(call.argumentRegister(i), call.argumentRegister(i + 1));
        emitRawCall(result.get(), function.get(), call, count, node);
        emitLabel(done.get());
        return finish(dst, result.get());
    }

    RegisterID* keywordNamesConstant(Sequence<Keyword*> keywords)
    {
        auto* names = JSCellButterfly::create(m_vm, CopyOnWriteArrayWithContiguous, keywords.size());
        for (unsigned i = 0; i < keywords.size(); ++i)
            names->setIndex(m_vm, i, internedString(m_vm, *keywords[i]->name));
        return constant(names);
    }

    // With keywords, *iterables or **mappings. The runtime works out which parameter each is for.
    // codegen_validate_keywords()
    void checkKeywords(Sequence<Keyword*> keywords)
    {
        for (size_t i = 0; i < keywords.size(); ++i) {
            for (size_t j = 0; keywords[i]->name && j < i; ++j) {
                if (keywords[j]->name && *keywords[j]->name == *keywords[i]->name)
                    fail(concatenate("keyword argument repeated: "_s, keywords[i]->name->string()), *keywords[i]);
            }
        }
    }

    RegisterID* emitGeneralCall(RegisterID* dst, Call& node)
    {
        checkKeywords(node.keywords);

        // base.function(...): the helpers are given base as their own `this`, to pass on.
        Reg base = g.newTemporary();
        Reg function = g.newTemporary();
        auto* attribute = node.function->tryAs<Attribute>();
        bool hasMappings = false;
        for (Keyword* keyword : node.keywords)
            hasMappings |= !keyword->name;

        if (!hasMappings && !hasStarred(node.arguments)) {
            // callKeywords(function, names, positional..., values of the keywords...)
            Reg helper = g.newTemporary();
            unsigned count = node.arguments.size() + node.keywords.size();
            // As without keywords: if it is a method of base's class, base goes in front of the arguments and no bound method is made.
            unsigned first = attribute ? 3 : 2;
            CallArguments call(g, nullptr, count + first);
            if (attribute) {
                emitInto(base.get(), attribute->value);
                mark(locationOf(*attribute));
                noteName(mangle(*attribute->attribute));
                OpPyLoadMethod::emit(&g, function.get(), call.argumentRegister(2), base.get(), g.addConstant(mangle(*attribute->attribute)), g.nextValueProfileIndex());
            } else {
                g.emitLoad(base.get(), jsUndefined());
                emitInto(function.get(), node.function);
            }
            g.emitGetById(helper.get(), runtime(), Identifier::fromString(m_vm, "callKeywords"_s));
            g.move(call.thisRegister(), base.get());
            g.move(call.argumentRegister(0), function.get());
            g.move(call.argumentRegister(1), keywordNamesConstant(node.keywords));
            unsigned i = first;
            for (Expression* argument : node.arguments)
                emitInto(call.argumentRegister(i++), argument);
            for (Keyword* keyword : node.keywords)
                emitInto(call.argumentRegister(i++), keyword->value);
            markIfOnAnotherLine(node);
            auto emitWithoutSelf = [&] (RegisterID* result) {
                // The first that is given, by name if none is given by position.
                emitTellOfCall(function.get(), count ? call.argumentRegister(2) : nullptr, count ? ToldArgument::First : ToldArgument::None);
                return emitRawCall(result, helper.get(), call, count + 2, node, Told::No);
            };
            if (!attribute)
                return emitWithoutSelf(destination(dst).get());

            Reg result = temporaryDestination(dst);
            Ref<Label> isNotMethod = g.newLabel();
            Ref<Label> done = g.newLabel();
            Reg isEmpty = g.newTemporary();
            g.emitIsEmpty(isEmpty.get(), call.argumentRegister(2));
            g.emitJumpIfTrue(isEmpty.get(), isNotMethod.get());
            emitTellOfCall(function.get(), call.argumentRegister(2), ToldArgument::First);
            emitRawCall(result.get(), helper.get(), call, count + 3, node, Told::No);
            g.emitJump(done.get());
            emitLabel(isNotMethod.get());
            for (unsigned i = 0; i < count; ++i)
                g.move(call.argumentRegister(2 + i), call.argumentRegister(3 + i));
            markIfOnAnotherLine(node);
            emitWithoutSelf(result.get());
            emitLabel(done.get());
            return finish(dst, result.get());
        }

        // With those it is got as any attribute is.
        if (attribute) {
            emitInto(base.get(), attribute->value);
            mark(locationOf(*attribute));
            emitGetAttribute(function.get(), base.get(), mangle(*attribute->attribute));
        } else {
            g.emitLoad(base.get(), jsUndefined());
            emitInto(function.get(), node.function);
        }

        // callSpread(function, a list of the positional arguments, a dict of the keywords or None)
        Reg positional = g.newTemporary();
        emitListWithStarred(positional.get(), node.arguments, node, function.get());
        Reg keywords = g.newTemporary();
        if (node.keywords.empty())
            g.emitLoad(keywords.get(), jsUndefined());
        else {
            emitRuntimeCall(keywords.get(), "newDict"_s, { }, node);
            for (Keyword* keyword : node.keywords) {
                Reg value = emit(keyword->value);
                if (keyword->name)
                    emitRuntimeCall(nullptr, "addKeyword"_s, { keywords.get(), stringConstant(*keyword->name), value.get(), function.get() }, *keyword);
                else
                    emitRuntimeCall(nullptr, "addKeywords"_s, { keywords.get(), value.get(), function.get() }, *keyword);
            }
        }
        Reg helper = g.newTemporary();
        g.emitGetById(helper.get(), runtime(), Identifier::fromString(m_vm, "callSpread"_s));
        CallArguments call(g, nullptr, 3);
        g.move(call.thisRegister(), base.get());
        g.move(call.argumentRegister(0), function.get());
        g.move(call.argumentRegister(1), positional.get());
        g.move(call.argumentRegister(2), keywords.get());
        markIfOnAnotherLine(node);
        emitTellOfCall(function.get(), positional.get(), ToldArgument::ListOfPositional);
        return emitRawCall(destination(dst).get(), helper.get(), call, 3, node, Told::No);
    }

    // ---- Making functions

    bool belongsToSomethingElse() const { return m_info.owner != OwnerKind::None; }

    // What comes before the name of what is in this. This is compiler_set_qualname() of CPython's Python/compile.c.
    const String& qualifiedNameInSource() const { return m_info.qualifiedNameInSource.isNull() ? m_info.qualifiedName : m_info.qualifiedNameInSource; }

    String ownQualifiedNamePrefix()
    {
        switch (m_info.kind) {
        case CodeKind::Module:
        case CodeKind::Expression:
        case CodeKind::Interactive:
            return emptyString();
        case CodeKind::Function:
        case CodeKind::Lambda:
            return concatenate(qualifiedNameInSource(), ".<locals>."_s);
        default:
            return concatenate(qualifiedNameInSource(), '.');
        }
    }

    // What is defined in what belongs to something else is named as if it were where that is.
    String qualifiedNameFor(const Identifier& name) { return concatenate(belongsToSomethingElse() ? m_info.qualifiedNamePrefix : ownQualifiedNamePrefix(), name.string()); }

    Ref<FunctionInfo> makeInfo(CodeKind kind, const Identifier& name, Arguments* arguments, Block& block, const Node& node)
    {
        auto info = adoptRef(*new FunctionInfo);
        info->kind = kind;
        // A `yield` where there is no function is found when it is come to.
        info->isGenerator = block.isGenerator && block.isFunctionLike();
        info->isCoroutine = block.isCoroutine && block.isFunctionLike();
        info->usesNamespace = kind == CodeKind::Class;
        info->isNested = block.isNested;
        info->isMethod = block.isMethod;
        info->hasDocstring = block.hasDocstring && keepsDocstrings();
        info->futureFeatures = m_info.futureFeatures;
        info->optimizationLevel = m_info.optimizationLevel;
        info->visibility = m_info.visibility;
        info->line = node.line;
        info->firstLine = node.line;
        if (kind == CodeKind::Function || kind == CodeKind::Class || kind == CodeKind::TypeParameters) {
            // What has the type parameters of a definition is compiled by itself, and knows where the definition begins.
            if (m_decoratorLine)
                info->firstLine = m_decoratorLine;
            else if (m_info.kind == CodeKind::TypeParameters)
                info->firstLine = m_info.firstLine;
        }
        info->name = name;
        // What is said to be global where it is defined is called what it would be called there. What has its type parameters comes between, and is looked past.
        bool isSaidToBeGlobal = (kind == CodeKind::Function || kind == CodeKind::Class)
            && (m_info.kind == CodeKind::TypeParameters ? m_info.definesWhatIsSaidToBeGlobal : m_block.scopeOf(mangle(name)) == NameScope::GlobalExplicit);
        info->qualifiedName = isSaidToBeGlobal ? name.string() : qualifiedNameFor(name);
        info->canSeeClassScope = block.canSeeClassScope;
        if (kind == CodeKind::Annotations || kind == CodeKind::TypeParameters || kind == CodeKind::Evaluator)
            info->qualifiedNamePrefix = ownQualifiedNamePrefix();
        if (kind == CodeKind::Class)
            info->privateName = name;
        else if (m_private)
            info->privateName = *m_private;
        for (Symbol& symbol : block.symbols) {
            // A class may have something of its own by the name of what is free in one of its methods. It is on its way through, all the same: what is in the class is compiled by itself, and this is how it
            // knows what there is outside.
            if (symbol.scope != NameScope::Free && !(symbol.flags & DefFreeClass))
                continue;
            info->freeVariables.append(*symbol.name);
            // What this has as a cell, what is in it finds as a cell.
            if (m_info.variablesGivenAsCells.contains(*symbol.name))
                info->variablesGivenAsCells.append(*symbol.name);
        }
        // The rest of what a piece that is compiled by itself cannot find out.
        if (block.canSeeClassScope) {
            if (m_block.type != BlockType::Class)
                info->namesSaidToBeGlobalInClass = m_info.namesSaidToBeGlobalInClass;
            else {
                for (Symbol& symbol : m_block.symbols) {
                    if (symbol.flags & DefGlobal)
                        info->namesSaidToBeGlobalInClass.append(*symbol.name);
                }
            }
        }
        if (block.mangledNames) {
            info->manglesOnlySomeNames = true;
            for (UniquedStringImpl* mangled : block.mangledNames->names)
                info->namesMangled.append(Identifier::fromUid(m_vm, mangled));
            std::ranges::sort(info->namesMangled, [] (auto& a, auto& b) { return codePointCompareLessThan(a.string(), b.string()); });
        }
        if (arguments) {
            auto add = [&] (Argument* argument) { info->parameterNames.append(mangle(*argument->name)); };
            for (Argument* argument : arguments->positionalOnly)
                add(argument);
            for (Argument* argument : arguments->positional)
                add(argument);
            for (Argument* argument : arguments->keywordOnly)
                add(argument);
            info->positionalOnlyCount = arguments->positionalOnly.size();
            info->positionalCount = arguments->positionalOnly.size() + arguments->positional.size();
            info->keywordOnlyCount = arguments->keywordOnly.size();
            if (arguments->variadic) {
                info->hasVariadic = true;
                add(arguments->variadic);
            }
            if (arguments->keywordVariadic) {
                info->hasKeywordVariadic = true;
                add(arguments->keywordVariadic);
            }
        }
        return info;
    }

    // A docstring with the indentation of the source taken out, as CPython's compiler takes it out.
    static String cleanDocstring(const String& original)
    {
        // Tabs are expanded first, to columns of eight.
        StringBuilder expanded;
        unsigned column = 0;
        for (char16_t c : StringView(original).codeUnits()) {
            if (c == '\t') {
                unsigned spaces = 8 - column % 8;
                for (unsigned i = 0; i < spaces; ++i)
                    expanded.append(' ');
                column += spaces;
                continue;
            }
            expanded.append(c);
            column = c == '\n' || c == '\r' ? 0 : column + 1;
        }
        String doc = expanded.toString();
        unsigned length = doc.length();

        // The least indentation of any line after the first that is not blank.
        unsigned p = 0;
        while (p < length && doc[p++] != '\n') { }
        unsigned margin = UINT_MAX;
        while (p < length) {
            unsigned start = p;
            while (p < length && doc[p] == ' ')
                ++p;
            if (p < length && doc[p] != '\n')
                margin = std::min(margin, p - start);
            while (p < length && doc[p++] != '\n') { }
        }
        if (margin == UINT_MAX)
            margin = 0;

        p = 0;
        while (p < length && doc[p] == ' ')
            ++p;
        if (!p && !margin)
            return doc.isNull() ? emptyString() : doc;
        StringBuilder result;
        auto copyLine = [&] {
            while (p < length) {
                char16_t c = doc[p++];
                result.append(c);
                if (c == '\n')
                    break;
            }
        };
        copyLine();
        while (p < length) {
            for (unsigned i = 0; i < margin && p < length && doc[p] == ' '; ++i)
                ++p;
            copyLine();
        }
        String cleaned = result.toString();
        return cleaned.isNull() ? emptyString() : cleaned;
    }

    static String docstringOf(Sequence<Statement*> body)
    {
        return cleanDocstring(body[0]->as<Expr>().value->as<Constant>().text->string());
    }

    // The function object for a piece of code, which is compiled when it is first called.
    RegisterID* emitNewFunction(RegisterID* dst, Ref<FunctionInfo>&& info, const Node& node, SourceParseMode parseMode = SourceParseMode::MethodMode)
    {
        const SourceCode& parentSource = g.m_scopeNode->source();
        unsigned parameterCount = info->isGeneratorBody ? static_cast<unsigned>(JSGenerator::Argument::NumberOfArguments) : info->parameterCount();
        FunctionMetadataNode metadata(JSTokenLocation(), JSTokenLocation(), node.start, node.start, node.start, info->visibility, StrictModeLexicallyScopedFeature, ConstructorKind::None, SuperBinding::NotNeeded, parameterCount, parseMode, false);
        metadata.finishParsing(SourceCode(parentSource.provider(), node.start, node.end), info->name, FunctionMode::FunctionExpression);
        auto* executable = UnlinkedFunctionExecutable::create(m_vm, parentSource, &metadata, UnlinkedNormalFunction, ConstructAbility::CannotConstruct, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, { }, std::nullopt, DerivedContextType::None, EvalContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);
        executable->setPythonInfo(WTF::move(info));
        unsigned index = g.m_codeBlock->addFunctionExpr(executable);
        m_numberOfFunctions = index + 1;
        if (isGeneratedLast(*executable->pythonInfo()))
            m_functionGeneratedLast = index;
        else
            noteConstant({ CodeDetails::Constant::Kind::Code, 10, false, index });
        OpNewFuncExp::emit(&g, dst, g.scopeRegister(), index);
        return dst;
    }

    // def and lambda: the defaults are evaluated now, and kept in the function.
    struct Defaults {
        Reg positional; // A tuple, or null.
        Reg keywordOnly; // A dict, or null.
    };

    Defaults emitDefaults(Arguments* arguments, const Node& node)
    {
        Reg defaults;
        std::optional<CodeDetails::Constant> constantDefaults { CodeDetails::Constant { CodeDetails::Constant::Kind::Tuple } };
        for (Expression* value : arguments->defaults) {
            auto* constant = constantDefaults ? constantOf(value) : nullptr;
            if (constant)
                constantDefaults->elements.append(*constant);
            else
                constantDefaults = std::nullopt;
        }
        if (!arguments->defaults.empty() && constantDefaults && canBeShared(*constantDefaults) && !m_isNeverComeTo) {
            for (Expression* value : arguments->defaults)
                notePartsOfConstant(value, false);
            markIfOnAnotherLine(node);
            defaults = emitLoadOfConstant(g.newTemporary(), WTF::move(*constantDefaults), node, true);
        } else if (!arguments->defaults.empty()) {
            Vector<Reg, 8> values;
            {
                SetForScope isInConstantDisplay(m_isInConstantDisplay, m_isInConstantDisplay || std::ranges::all_of(arguments->defaults, isConstant));
                emitElements(arguments->defaults, values);
            }
            markIfOnAnotherLine(node);
            defaults = g.newTemporary();
            emitNewTuple(defaults.get(), values);
        }
        Reg keywordDefaults;
        for (size_t i = 0; i < arguments->keywordOnly.size(); ++i) {
            if (!arguments->keywordDefaults[i])
                continue;
            if (!keywordDefaults) {
                keywordDefaults = g.newTemporary();
                emitRuntimeCall(keywordDefaults.get(), "newDict"_s, { }, node);
            }
            Reg value = emit(arguments->keywordDefaults[i]);
            OpPySetItem::emit(&g, keywordDefaults.get(), stringConstant(mangle(*arguments->keywordOnly[i]->name)), value.get());
        }
        return { defaults, keywordDefaults };
    }

    RegisterID* emitFunction(RegisterID* dst, CodeKind kind, const Identifier& name, Arguments* arguments, const Node& node, const void* blockKey)
    {
        Defaults defaults = emitDefaults(arguments, node);
        return emitFunctionWithDefaults(dst, kind, name, arguments, node, blockKey, defaults.positional.get(), defaults.keywordOnly.get());
    }

    RegisterID* emitFunctionWithDefaults(RegisterID* dst, CodeKind kind, const Identifier& name, Arguments* arguments, const Node& node, const void* blockKey, RegisterID* defaults, RegisterID* keywordDefaults)
    {
        Block* block = m_table.blockFor(blockKey);
        RELEASE_ASSERT(block);
        // What evaluates the annotations comes before the function itself.
        Reg annotate;
        if (Block* annotations = kind == CodeKind::Function ? m_table.blockFor(arguments) : nullptr; annotations && annotations->usesAnnotations) {
            annotate = g.newTemporary();
            emitNewAnnotateFunction(annotate.get(), *annotations, OwnerKind::Function, node);
            // Its code is beside the function's and is called so. It is itself said to be in the function, once it is the function's.
            g.emitDirectPutById(annotate.get(), m_names.private_qualname, constant(jsString(m_vm, concatenate(qualifiedNameFor(name), ".__annotate__"_s))));
        }
        Reg function = temporaryDestination(dst);
        auto info = makeInfo(kind, name, arguments, *block, node);
        if (block->hasDocstring && kind == CodeKind::Function && keepsDocstrings())
            info->docstring = docstringOf(static_cast<const FunctionDef&>(node).body);
        emitNewFunction(function.get(), WTF::move(info), node);
        if (annotate)
            g.emitDirectPutById(function.get(), m_names.private_annotate, annotate.get());
        if (defaults)
            g.emitDirectPutById(function.get(), m_names.private_defaults, defaults);
        if (keywordDefaults)
            g.emitDirectPutById(function.get(), m_names.private_kwdefaults, keywordDefaults);
        return finish(dst, function.get());
    }

    // ---- What a function does before its body

    // Names that can lose their value: `del x`, and `except E as x`, which ends with one.
    void collectDeletedNames(Sequence<Statement*> body)
    {
        for (Statement* statement : body)
            collectDeletedNames(*statement);
    }

    void collectDeletedNames(Statement& statement)
    {
        switch (statement.kind) {
        case Statement::Kind::Delete:
            for (Expression* target : statement.as<Delete>().targets)
                collectDeletedNames(*target);
            return;
        case Statement::Kind::For:
            collectDeletedNames(statement.as<For>().body);
            collectDeletedNames(statement.as<For>().orElse);
            return;
        case Statement::Kind::While:
            collectDeletedNames(statement.as<While>().body);
            collectDeletedNames(statement.as<While>().orElse);
            return;
        case Statement::Kind::If:
            collectDeletedNames(statement.as<If>().body);
            collectDeletedNames(statement.as<If>().orElse);
            return;
        case Statement::Kind::With:
            collectDeletedNames(statement.as<With>().body);
            return;
        case Statement::Kind::Match:
            for (MatchCase* matchCase : statement.as<Match>().cases)
                collectDeletedNames(matchCase->body);
            return;
        case Statement::Kind::Try: {
            auto& node = statement.as<Try>();
            collectDeletedNames(node.body);
            for (ExceptHandler* handler : node.handlers) {
                if (handler->name)
                    m_deletedNames.add(mangle(*handler->name).impl());
                collectDeletedNames(handler->body);
            }
            collectDeletedNames(node.orElse);
            collectDeletedNames(node.finalBody);
            return;
        }
        default:
            return;
        }
    }

    void collectDeletedNames(Expression& target)
    {
        if (auto* name = target.tryAs<Name>())
            m_deletedNames.add(mangle(*name->id).impl());
        else if (auto* tuple = target.tryAs<Tuple>()) {
            for (Expression* element : tuple->elements)
                collectDeletedNames(*element);
        } else if (auto* list = target.tryAs<List>()) {
            for (Expression* element : list->elements)
                collectDeletedNames(*element);
        }
    }

    void emitLoadGlobals()
    {
        for (auto [target, name] : { std::pair { &m_globals, &m_names.globals }, std::pair { &m_builtins, &m_names.builtins } }) {
            Variable variable = g.variable(*name);
            Reg scope = g.emitResolveScope(nullptr, variable);
            g.emitGetFromScope(target->get(), scope.get(), variable, ThrowIfNotFound);
        }
        m_details->globalsRegister = m_globals->virtualRegister();
        m_details->builtinsRegister = m_builtins->virtualRegister();
    }

    // An environment for the variables of this block that other functions use.
    void emitPushCells(const Vector<const Identifier*, 8>& names)
    {
        if (names.isEmpty())
            return;
        m_environments.append(makeUnique<VariableEnvironment>());
        VariableEnvironment& environment = *m_environments.last();
        auto add = [&] (const Identifier& name) {
            auto result = environment.add(name);
            result.iterator->value.setIsLet();
            result.iterator->value.setIsCaptured();
        };
        for (const Identifier* name : names)
            add(*name);
        add(m_names.cells);
        g.pushLexicalScopeInternal(environment, BytecodeGenerator::TDZCheckOptimization::DoNotOptimize, BytecodeGenerator::NestedScopeType::IsNested, nullptr, BytecodeGenerator::TDZRequirement::UnderTDZ, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::ScopeRegisterType::Block);
    }

    void emitPopCells(const Vector<const Identifier*, 8>& names)
    {
        if (names.isEmpty())
            return;
        g.popLexicalScopeInternal(*m_environments.last());
        m_environments.removeLast();
    }

    RegisterID* parameterRegister(unsigned index) { return &g.m_parameters[index + 1]; }

    // Gives each parameter its value, from arguments given by position. Anything else the caller has seen to: see README.md.
    void emitBindArguments(const Node& node)
    {
        unsigned positionalCount = m_info.positionalCount;
        Ref<Label> bound = g.newLabel();
        OpJeqPtr::emit(&g, g.thisRegister(), marker(), bound->bind(&g));

        Reg given = g.newTemporary();
        OpArgumentCount::emit(&g, given.get());
        Reg callee = g.newTemporary();
        g.move(callee.get(), &g.m_calleeRegister);

        // What is left over is where the parameters after the positional ones are, so it is taken before those are given values.
        Reg rest;
        if (m_info.hasVariadic) {
            rest = g.newTemporary();
            OpCreateRest::emit(&g, rest.get(), positionalCount);
        }

        Ref<Label> enough = g.newLabel();
        Reg isEnough = g.newTemporary();
        if (m_info.hasVariadic)
            g.emitBinaryOp<OpGreatereq>(isEnough.get(), given.get(), constant(JSC::jsNumber(positionalCount)), OperandTypes());
        else
            g.emitEqualityOp<OpStricteq>(isEnough.get(), given.get(), constant(JSC::jsNumber(positionalCount)));
        g.emitJumpIfTrue(isEnough.get(), enough.get());
        // Too few or too many. The runtime raises, or gives a tuple with the default for each parameter in that parameter's place. How
        // many have one is not known here: __defaults__ can be set.
        {
            Reg defaults = g.newTemporary();
            emitRuntimeCall(defaults.get(), "defaultsFor"_s, { callee.get(), given.get() }, node);
            for (unsigned i = 0; i < positionalCount; ++i) {
                Ref<Label> wasGiven = g.newLabel();
                Reg isGiven = g.newTemporary();
                g.emitBinaryOp<OpGreater>(isGiven.get(), given.get(), constant(JSC::jsNumber(i)), OperandTypes());
                g.emitJumpIfTrue(isGiven.get(), wasGiven.get());
                emitGetItem(parameterRegister(i), defaults.get(), constant(JSC::jsNumber(i)));
                emitLabel(wasGiven.get());
            }
        }
        emitLabel(enough.get());

        for (unsigned i = positionalCount; i < positionalCount + m_info.keywordOnlyCount; ++i)
            emitRuntimeCall(parameterRegister(i), "keywordDefault"_s, { callee.get(), constant(JSC::jsNumber(i)) }, node);
        if (m_info.hasVariadic)
            emitRuntimeCall(parameterRegister(m_info.variadicIndex()), "listToTuple"_s, { rest.get() }, node);
        if (m_info.hasKeywordVariadic)
            emitRuntimeCall(parameterRegister(m_info.keywordVariadicIndex()), "newDict"_s, { }, node);
        emitLabel(bound.get());
    }

    bool usesGlobals()
    {
        if (!isFunctionLike() || m_block.hasImport || m_block.hasClassDefinition || m_block.hasGlobalInComprehension)
            return true;
        // What evaluates the annotations of a module looks among the globals for which of them have been come to, and that is not a name that is written in it.
        if (m_info.kind == CodeKind::Annotations && (m_info.owner == OwnerKind::Module || m_info.owner == OwnerKind::Interactive))
            return true;
        for (Symbol& symbol : m_block.symbols) {
            if (symbol.scope == NameScope::GlobalImplicit || symbol.scope == NameScope::GlobalExplicit)
                return true;
        }
        return false;
    }

    // Registers for the globals, if anything is made of them, and for the local variables. It comes before anything else has a register. The variables are the first so many of the registers, and it is by that that it is
    // known which they are: they all have something in them from when the code is entered, whereas the rest have whatever was left there until they are given something.
    enum class HasLocalVariables : bool { No, Yes };
    void allocateVariables(HasLocalVariables hasLocalVariables)
    {
        if (usesGlobals()) {
            m_globals = g.addVar();
            m_builtins = g.addVar();
        }
        // A frame object looks there as it does at the variables.
        if (m_namespaceIsLoaded)
            m_namespace = g.addVar();
        // Those of the comprehensions that are part of this. A frame object sees them, so they are variables and not temporaries.
        for (Block* comprehension : m_block.inlinedComprehensions) {
            for (Symbol& symbol : comprehension->symbols) {
                if (isOwnVariableOfComprehension(symbol) && !isInEnvironmentOfComprehension(symbol))
                    m_comprehensionLocals.add({ comprehension, symbol.name->impl() }, g.addVar());
            }
        }
        if (hasLocalVariables == HasLocalVariables::No)
            return;
        HashSet<UniquedStringImpl*> parameters;
        for (auto& name : m_info.parameterNames)
            parameters.add(name.impl());
        for (Symbol& symbol : m_block.symbols) {
            if (symbol.scope == NameScope::Local && !parameters.contains(symbol.name->impl()))
                m_locals.set(symbol.name->impl(), g.addVar());
        }
    }

    static bool isOwnVariableOfComprehension(const Symbol& symbol)
    {
        return !(symbol.flags & DefParameter) && (symbol.flags & DefLocal) && !(symbol.flags & DefNonlocal) && !(symbol.flags & DefGlobal);
    }

    static bool isInEnvironmentOfComprehension(const Symbol& symbol)
    {
        return symbol.scope == NameScope::Cell || (symbol.flags & DefComprehensionCell);
    }

    // The local variables have nothing yet. And an environment for those that inner functions use.
    void emitDeclareVariables(bool parametersAreInEnvironment)
    {
        Vector<const Identifier*, 8> cells;
        HashSet<UniquedStringImpl*> parameters;
        for (auto& name : m_info.parameterNames)
            parameters.add(name.impl());

        for (Symbol& symbol : m_block.symbols) {
            bool isParameter = parameters.contains(symbol.name->impl());
            if (symbol.scope == NameScope::Cell) {
                // In the body of a generator, the parameters are the variables of the function that made it.
                if (!(isParameter && parametersAreInEnvironment))
                    cells.append(symbol.name);
                continue;
            }
            if (symbol.scope != NameScope::Local || isParameter)
                continue;
            g.moveEmptyValue(m_locals.get(symbol.name->impl()));
        }
        emitPushCells(cells);
    }

    template<typename EmitBody>
    void generateFunction(const EmitBody& emitBody)
    {
        Node& node = m_block.location;

        if (m_info.isGeneratorBody)
            return generateGeneratorBody(emitBody);

        // What only makes a generator has none. They are the body's.
        if (!m_info.isGenerator && !m_info.isCoroutine)
            allocateVariables(HasLocalVariables::Yes);
        emitBindArguments(node);
        m_details->firstTraceableOffset = g.instructions().size();

        if (m_info.isGenerator || m_info.isCoroutine) {
            // Nor is there one at all for this: the frame is the one that the body runs in.
            m_details->firstTraceableOffset = std::numeric_limits<unsigned>::max();
            // The function proper only makes the generator, or the coroutine, which is the same thing under another name. Its parameters are kept where the body, which is another function, finds them.
            Vector<const Identifier*, 8> cells;
            for (auto& name : m_info.parameterNames)
                cells.append(&name);
            emitPushCells(cells);
            for (unsigned i = 0; i < m_info.parameterNames.size(); ++i)
                emitStoreClosure(m_info.parameterNames[i], parameterRegister(i), node);

            emitEnter();
            auto info = m_info.copy();
            info->isGeneratorBody = true;
            Reg body = g.newTemporary();
            emitNewFunction(body.get(), WTF::move(info), node, SourceParseMode::GeneratorBodyMode);
            Reg generator = g.newTemporary();
            if (m_info.isCoroutine)
                emitRuntimeCall(generator.get(), "newCoroutine"_s, { body.get(), constant(jsBoolean(m_info.isGenerator)) }, node);
            else {
                g.emitNewGenerator(generator.get());
                g.emitPutInternalField(generator.get(), static_cast<unsigned>(JSGenerator::Field::Next), body.get());
            }
            // Python has no `this`. What is kept there is the function that made it, whose name it goes by.
            g.emitPutInternalField(generator.get(), static_cast<unsigned>(JSGenerator::Field::This), &g.m_calleeRegister);
            g.emitReturn(generator.get());
            return;
        }

        if (usesGlobals())
            emitLoadGlobals();
        for (unsigned i = 0; i < m_info.parameterNames.size(); ++i) {
            UniquedStringImpl* name = m_info.parameterNames[i].impl();
            m_locals.set(name, parameterRegister(i));
            if (!m_deletedNames.contains(name))
                m_alwaysBound.add(name);
        }
        emitDeclareVariables(false);
        for (unsigned i = 0; i < m_info.parameterNames.size(); ++i) {
            const Identifier& name = m_info.parameterNames[i];
            if (m_block.scopeOf(name) == NameScope::Cell)
                emitStoreClosure(name, parameterRegister(i), node);
        }
        emitEnter();
        noteDocstring();
        emitBody();
        emitReturnAtEnd();
    }

    // ---- Generators

    // The function that is called each time a generator is resumed. BytecodeGeneratorification makes it pick up where it left off.
    template<typename EmitBody>
    void generateGeneratorBody(const EmitBody& emitBody)
    {
        g.m_generatorRegister = &g.m_parameters[static_cast<unsigned>(JSGenerator::Argument::Generator)];
        g.m_needsGeneratorification = true;
        allocateVariables(HasLocalVariables::Yes);

        // Where the registers that are live across a yield are kept.
        JSC::SymbolTable* frameSymbolTable = JSC::SymbolTable::create(m_vm);
        frameSymbolTable->setScopeType(JSC::SymbolTable::ScopeType::VarScope);
        int frameSymbolTableIndex = constant(frameSymbolTable)->index();
        g.m_generatorFrameSymbolTable.set(m_vm, frameSymbolTable);
        g.m_generatorFrameSymbolTableIndex = frameSymbolTableIndex;
        OpCreateGeneratorFrameEnvironment::emit(&g, g.generatorFrameRegister(), g.scopeRegister(), VirtualRegister { frameSymbolTableIndex }, none());
        g.emitPutInternalField(g.generatorRegister(), static_cast<unsigned>(JSGenerator::Field::Frame), g.generatorFrameRegister());

        if (usesGlobals())
            emitLoadGlobals();
        emitDeclareVariables(true);
        emitEnter();

        // The first time, there is nowhere for a value or an exception to be sent to.
        Ref<Label> start = g.newLabel();
        Ref<Label> thrown = g.newLabel();
        g.emitJumpIfTrue(g.emitEqualityOp<OpStricteq>(g.newTemporary(), g.generatorResumeModeRegister(), g.emitLoad(nullptr, JSGenerator::ResumeMode::NormalMode)), start.get());
        g.emitJumpIfTrue(g.emitEqualityOp<OpStricteq>(g.newTemporary(), g.generatorResumeModeRegister(), g.emitLoad(nullptr, JSGenerator::ResumeMode::ThrowMode)), thrown.get());
        g.emitReturn(g.generatorValueRegister());
        emitLabel(thrown.get());
        {
            SetForScope isArtificial(m_isArtificial, true);
            mark(m_block.location);
        }
        g.emitThrow(g.generatorValueRegister());
        emitLabel(start.get());

        noteDocstring();
        {
            // What turns a StopIteration that gets out into a RuntimeError is one of them.
            NestedBlock block(*this, m_block.location);
            emitBody();
        }
        emitReturnAtEnd();
    }

    RegisterID* emitYield(RegisterID* dst, Yield& node)
    {
        if (!m_info.isGeneratorBody) {
            fail("'yield' outside function"_s, node);
            return g.emitLoad(dst, jsUndefined());
        }
        Reg value = node.value ? Reg(emitToTemporary(node.value)) : Reg(g.emitLoad(g.newTemporary(), jsUndefined()));
        mark(node);
        return g.move(destination(dst).get(), emitYieldValue(value.get(), node));
    }

    // In an asynchronous generator, what is yielded is marked, to tell it from what an await passes up on its way to the event loop.
    RegisterID* emitYieldValue(RegisterID* value, const Node& node)
    {
        if (!m_info.isCoroutine)
            return g.emitYield(value);
        Reg wrapped = g.newTemporary();
        emitRuntimeCall(wrapped.get(), "wrapAsyncYield"_s, { value }, node);
        return g.emitYield(wrapped.get());
    }

    enum class AwaitContext : uint8_t { Await, AsyncEnter, AsyncExit };

    // await value
    RegisterID* emitAwaitValue(RegisterID* dst, RegisterID* value, const Node& node, AwaitContext context = AwaitContext::Await)
    {
        Reg awaitable = g.newTemporary();
        emitRuntimeCall(awaitable.get(), "getAwaitable"_s, { value, constant(JSC::jsNumber(static_cast<unsigned>(context))) }, node);
        return emitDelegate(dst, awaitable.get(), node);
    }

    // False, with an error reported, if this is no place for something asynchronous.
    bool checkIsAsync(ASCIILiteral what, const Node& node)
    {
        if (m_info.isCoroutine && m_info.isGeneratorBody)
            return true;
        if (!isFunctionLike() && what == "'await'"_s)
            fail("'await' outside function"_s, node);
        else if (what == "asynchronous comprehension"_s)
            fail("asynchronous comprehension outside of an asynchronous function"_s, node);
        else
            fail(concatenate(what, " outside async function"_s), node);
        return false;
    }

    RegisterID* emitAwait(RegisterID* dst, Await& node)
    {
        if (!checkIsAsync("'await'"_s, node))
            return g.emitLoad(dst, jsUndefined());
        Reg value = emitToTemporary(node.value);
        return emitAwaitValue(dst, value.get(), node);
    }

    // The next item of an asynchronous iterator, or a jump if there are no more.
    void emitAsyncNext(RegisterID* dst, RegisterID* iterator, Label& exhausted, const Node& node)
    {
        emitTryCatch([&] {
            Reg awaitable = g.newTemporary();
            emitRuntimeCall(awaitable.get(), "getAsyncNext"_s, { iterator }, node);
            emitDelegate(dst, awaitable.get(), node);
        }, [&] (RegisterID* exception, RegisterID* thrown) {
            Reg type = g.newTemporary();
            g.emitGetById(type.get(), runtime(), Identifier::fromString(m_vm, "StopAsyncIteration"_s));
            Reg matches = g.newTemporary();
            emitCompare(matches.get(), ComparisonOperator::ExceptionMatch, exception, type.get());
            g.emitJumpIfTrue(matches.get(), exhausted);
            g.emitThrow(thrown);
        }, [] { });
    }

    // yield from iterable: everything that is sent to or thrown into this generator goes to that one, until it is done.
    RegisterID* emitYieldFrom(RegisterID* dst, YieldFrom& node)
    {
        if (!m_info.isGeneratorBody) {
            fail("'yield from' outside function"_s, node);
            return g.emitLoad(dst, jsUndefined());
        }
        if (m_info.isCoroutine) {
            fail("'yield from' inside async function"_s, node);
            return g.emitLoad(dst, jsUndefined());
        }
        Reg iterator = g.newTemporary();
        {
            Reg iterable = emit(node.value);
            emitRuntimeCall(iterator.get(), "getYieldFromIterator"_s, { iterable.get(), constant(jsBoolean(m_info.isIterableCoroutine)) }, node);
        }
        return emitDelegate(dst, iterator.get(), node);
    }

    // Everything that is sent to or thrown into this generator goes to the iterator, until it is done. The result is what it returned.
    RegisterID* emitDelegate(RegisterID* dst, RegisterID* iteratorRegister, const Node& node)
    {
        Reg iterator = iteratorRegister;
        Reg received = g.emitLoad(g.newTemporary(), jsUndefined());
        Reg wasThrown = g.emitLoad(g.newTemporary(), jsBoolean(false));
        Reg yielded = g.newTemporary();

        Ref<Label> loop = g.newLabel();
        Ref<Label> done = g.newLabel();
        emitLabel(loop.get());
        g.emitLoopHint();
        emitRuntimeCall(yielded.get(), "yieldFromStep"_s, { iterator.get(), received.get(), wasThrown.get(), g.generatorRegister() }, node);
        OpJeqPtr::emit(&g, yielded.get(), marker(), done->bind(&g));

        Ref<Label> catchLabel = g.newLabel();
        Ref<Label> tryStart = g.newEmittedLabel();
        TryData* tryData = g.pushTry(tryStart.get(), catchLabel.get(), HandlerType::Catch);
        g.move(received.get(), g.emitYield(yielded.get()));
        g.emitLoad(wasThrown.get(), jsBoolean(false));
        Ref<Label> tryEnd = g.newEmittedLabel();
        g.popTry(tryData, tryEnd.get());
        g.emitJump(loop.get());

        emitLabel(catchLabel.get());
        g.emitOutOfLineCatchHandler(received.get(), nullptr, tryData);
        g.restoreScopeRegister();
        g.emitLoad(wasThrown.get(), jsBoolean(true));
        g.emitJump(loop.get());

        emitLabel(done.get());
        return emitRuntimeCall(dst, "takeReturnValue"_s, { }, node);
    }

    // ---- Comprehensions

    // for target in iterator: [if condition]* ... innermost()
    template<typename Innermost>
    void emitComprehensionLoops(Sequence<Comprehension*> generators, size_t index, RegisterID* firstIterator, const Innermost& innermost)
    {
        if (index == generators.size())
            return innermost();
        Comprehension& generator = *generators[index];
        if (generator.isAsync && !checkIsAsync("asynchronous comprehension"_s, *generator.target))
            return;

        Reg iterator = firstIterator;
        if (!iterator) {
            iterator = g.newTemporary();
            Reg iterable = generator.isAsync ? emit(generator.iterable) : emitToLookInOrGoThrough(generator.iterable);
            mark(*generator.iterable);
            if (generator.isAsync)
                emitRuntimeCall(iterator.get(), "getAsyncIterator"_s, { iterable.get() }, *generator.iterable);
            else
                OpPyGetIter::emit(&g, iterator.get(), iterable.get());
        }
        std::optional<NestedBlock> block;
        if (generator.isAsync)
            block.emplace(*this, *generator.target);
        Ref<Label> loop = g.newLabel();
        Ref<Label> end = g.newLabel();
        emitLabel(loop.get());
        g.emitLoopHint();
        {
            Reg value = g.newTemporary();
            mark(*generator.iterable);
            if (generator.isAsync)
                emitAsyncNext(value.get(), iterator.get(), end.get(), *generator.iterable);
            else
                emitIteratorNext(value.get(), iterator.get(), end.get(), *generator.iterable);
            emitAssign(generator.target, value.get());
        }
        Ref<Label> again = g.newLabel();
        {
            JumpBlock loopBlock(*this, CodeDetails::JumpBlock::Kind::ComprehensionLoop, iterator.get());
            for (Expression* condition : generator.conditions)
                emitBranch(condition, again.get(), false);
            emitComprehensionLoops(generators, index + 1, nullptr, innermost);
        }
        emitLabel(again.get());
        emitLineAfterBackwardJump(*generator.iterable);
        g.emitJump(loop.get());
        emitLabel(end.get());
        // As CPython has it, what comes next follows on from the jump back, which is where the element is, and begins no line if it is on the same one.
        m_lastMarkedLine = m_comprehensionElementLine;
    }

    // [element for ...], {element for ...} and {key: value for ...} are part of the code they are in, with variables of their own.
    RegisterID* emitComprehension(RegisterID* dst, Expression& node, ComprehensionType type, Sequence<Comprehension*> generators, Expression* element, Expression* value)
    {
        Block* block = m_table.blockFor(&node);
        RELEASE_ASSERT(block);
        if (!block->isInlinedComprehension)
            return emitComprehensionFunction(dst, node, type, generators, *block);

        // The outermost iterable is evaluated outside.
        Reg iterator = g.newTemporary();
        {
            Reg iterable = generators[0]->isAsync ? emit(generators[0]->iterable) : emitToLookInOrGoThrough(generators[0]->iterable);
            mark(*generators[0]->iterable);
            if (generators[0]->isAsync)
                emitRuntimeCall(iterator.get(), "getAsyncIterator"_s, { iterable.get() }, *generators[0]->iterable);
            else
                OpPyGetIter::emit(&g, iterator.get(), iterable.get());
        }

        ComprehensionScope scope;
        Vector<const Identifier*, 8> cells;
        unsigned firstVariable = m_details->comprehensionVariables.size();
        for (Symbol& symbol : block->symbols) {
            if (!isOwnVariableOfComprehension(symbol))
                continue;
            // codegen_push_inlined_comprehension_locals()
            noteVariableName(*symbol.name);
            if (isInEnvironmentOfComprehension(symbol)) {
                cells.append(symbol.name);
                scope.add(symbol.name->impl(), nullptr);
                m_details->comprehensionVariables.append({ *symbol.name, VirtualRegister(), 0, 0 });
                continue;
            }
            Reg local = m_comprehensionLocals.get({ block, symbol.name->impl() });
            RELEASE_ASSERT(local);
            g.moveEmptyValue(local.get());
            scope.add(symbol.name->impl(), local);
            m_details->comprehensionVariables.append({ *symbol.name, local->virtualRegister(), 0, 0 });
        }
        unsigned variablesEnd = m_details->comprehensionVariables.size();
        emitPushCells(cells);
        for (unsigned i = firstVariable; i < variablesEnd; ++i)
            m_details->comprehensionVariables[i].begin = g.instructions().size();
        m_comprehensionScopes.append(WTF::move(scope));
        m_comprehensionBlocks.append(block);

        Reg result = g.newTemporary();
        {
            std::optional<JumpBlock> environment;
            if (!cells.isEmpty())
                environment.emplace(*this, CodeDetails::JumpBlock::Kind::Scope);
            JumpBlock comprehension(*this, CodeDetails::JumpBlock::Kind::Comprehension);
            emitFillComprehension(result.get(), node, type, generators, element, value, iterator.get());
        }

        m_comprehensionScopes.removeLast();
        m_comprehensionBlocks.removeLast();
        for (unsigned i = firstVariable; i < variablesEnd; ++i)
            m_details->comprehensionVariables[i].end = g.instructions().size();
        emitPopCells(cells);
        return finish(dst, result.get());
    }

    // Makes what it is a comprehension of, and goes round putting things in it.
    void emitFillComprehension(RegisterID* result, Expression& node, ComprehensionType type, Sequence<Comprehension*> generators, Expression* element, Expression* value, RegisterID* iterator)
    {
        switch (type) {
        case ComprehensionType::List:
            markIfOnAnotherLine(node);
            emitNewList(result, { });
            break;
        case ComprehensionType::Set:
            emitRuntimeCall(result, "newSet"_s, { }, node);
            break;
        case ComprehensionType::Dict:
            emitRuntimeCall(result, "newDict"_s, { }, node);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        SetForScope elementLine(m_comprehensionElementLine, element->line);
        emitComprehensionLoops(generators, 0, iterator, [&] {
            switch (type) {
            case ComprehensionType::List: {
                Reg item = emit(element);
                emitRuntimeCall(nullptr, "listAppend"_s, { result, item.get() }, *element);
                break;
            }
            case ComprehensionType::Set: {
                Reg item = emit(element);
                emitRuntimeCall(nullptr, "setAdd"_s, { result, item.get() }, *element);
                break;
            }
            case ComprehensionType::Dict: {
                Reg key = emit(element);
                Reg item = emit(value);
                mark(*element);
                OpPySetItem::emit(&g, result, key.get(), item.get());
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        });
    }

    // Where the names of a class can be seen without its being the body of the class, as in an annotation, a comprehension is a function that is called at once
    // with the outermost iterator, as a generator expression is everywhere. Its variables would otherwise be taken for the class's.
    RegisterID* emitComprehensionFunction(RegisterID* dst, Expression& node, ComprehensionType type, Sequence<Comprehension*> generators, Block& block)
    {
        auto spelled = type == ComprehensionType::List ? "<listcomp>"_span8 : type == ComprehensionType::Set ? "<setcomp>"_span8 : "<dictcomp>"_span8;
        auto info = makeInfo(CodeKind::Comprehension, m_arena.identifiers().makeIdentifier(m_vm, spelled), nullptr, block, node);
        info->parameterNames.append(Identifier::fromString(m_vm, ".0"_s));
        info->positionalCount = 1;
        info->positionalOnlyCount = 1;

        Reg function = g.newTemporary();
        emitNewFunction(function.get(), WTF::move(info), node);
        CallArguments call(g, nullptr, 1);
        g.emitLoad(call.thisRegister(), jsUndefined());
        {
            Reg iterable = generators[0]->isAsync ? emit(generators[0]->iterable) : emitToLookInOrGoThrough(generators[0]->iterable);
            mark(*generators[0]->iterable);
            if (generators[0]->isAsync)
                emitRuntimeCall(call.argumentRegister(0), "getAsyncIterator"_s, { iterable.get() }, *generators[0]->iterable);
            else
                OpPyGetIter::emit(&g, call.argumentRegister(0), iterable.get());
        }
        Reg result = destination(dst);
        emitRawCall(result.get(), function.get(), call, 1, node);
        if (block.isCoroutine)
            emitAwaitValue(result.get(), result.get(), node);
        return result.get();
    }

    void generateComprehension(Expression& node)
    {
        generateFunction([&] {
            // In a coroutine the parameters are variables of the function that made it.
            Reg iterator = m_info.isGeneratorBody ? Reg(emitLoadClosure(nullptr, m_info.parameterNames[0], node)) : Reg(parameterRegister(0));
            Reg result = g.newTemporary();
            if (auto* list = node.tryAs<ListComp>())
                emitFillComprehension(result.get(), node, ComprehensionType::List, list->generators, list->element, nullptr, iterator.get());
            else if (auto* set = node.tryAs<SetComp>())
                emitFillComprehension(result.get(), node, ComprehensionType::Set, set->generators, set->element, nullptr, iterator.get());
            else {
                auto& dict = node.as<DictComp>();
                emitFillComprehension(result.get(), node, ComprehensionType::Dict, dict.generators, dict.key, dict.value, iterator.get());
            }
            g.emitReturn(result.get());
        });
    }

    // (element for ...) is a generator function that is called at once, with the outermost iterator.
    RegisterID* emitGeneratorExpression(RegisterID* dst, GeneratorExp& node)
    {
        Block* block = m_table.blockFor(&node);
        RELEASE_ASSERT(block);
        const Identifier& name = m_arena.identifiers().makeIdentifier(m_vm, "<genexpr>"_span8);
        auto info = makeInfo(CodeKind::GeneratorExpression, name, nullptr, *block, node);
        info->parameterNames.append(Identifier::fromString(m_vm, ".0"_s));
        info->positionalCount = 1;
        info->positionalOnlyCount = 1;

        Reg function = g.newTemporary();
        emitNewFunction(function.get(), WTF::move(info), node);
        CallArguments call(g, nullptr, 1);
        g.emitLoad(call.thisRegister(), jsUndefined());
        {
            Reg iterable = node.generators[0]->isAsync ? emit(node.generators[0]->iterable) : emitToLookInOrGoThrough(node.generators[0]->iterable);
            mark(*node.generators[0]->iterable);
            if (node.generators[0]->isAsync)
                emitRuntimeCall(call.argumentRegister(0), "getAsyncIterator"_s, { iterable.get() }, *node.generators[0]->iterable);
            else
                OpPyGetIter::emit(&g, call.argumentRegister(0), iterable.get());
        }
        return emitRawCall(destination(dst).get(), function.get(), call, 1, node);
    }

    void generateGeneratorExpression(GeneratorExp& node)
    {
        generateFunction([&] {
            Reg iterator = emitLoadClosure(nullptr, m_info.parameterNames[0], node);
            SetForScope elementLine(m_comprehensionElementLine, node.element->line);
            emitComprehensionLoops(node.generators, 0, iterator.get(), [&] {
                Reg value = emitToTemporary(node.element);
                emitYieldValue(value.get(), node);
            });
        });
    }

    // ---- Assignment

    void emitAssign(Expression* target, RegisterID* value)
    {
        switch (target->kind) {
        case Expression::Kind::Name:
            emitStoreName(*target->as<Name>().id, value, *target);
            return;
        case Expression::Kind::Attribute: {
            auto& node = target->as<Attribute>();
            Reg base = emit(node.value);
            mark(locationOf(node));
            emitSetAttribute(base.get(), mangle(*node.attribute), value);
            return;
        }
        case Expression::Kind::Subscript: {
            auto& node = target->as<Subscript>();
            Reg base = emit(node.value);
            Reg key = emit(node.slice);
            mark(node);
            OpPySetItem::emit(&g, base.get(), key.get(), value);
            return;
        }
        case Expression::Kind::Tuple:
            return emitUnpack(target->as<Tuple>().elements, value, *target);
        case Expression::Kind::List:
            return emitUnpack(target->as<List>().elements, value, *target);
        case Expression::Kind::Starred:
            fail("starred assignment target must be in a list or tuple"_s, *target);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void emitUnpack(Sequence<Expression*> targets, RegisterID* value, const Node& node)
    {
        int starIndex = -1;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!targets[i]->is<Starred>())
                continue;
            if (starIndex >= 0)
                fail("multiple starred expressions in assignment"_s, node);
            // As many as CPython's instruction has room to say.
            else if (i >= (1 << 8) || targets.size() - i - 1 >= static_cast<size_t>(std::numeric_limits<int>::max() >> 8))
                fail("too many expressions in star-unpacking assignment"_s, node);
            starIndex = i;
        }
        Vector<Reg, 8> values;
        for (size_t i = 0; i < targets.size(); ++i)
            values.append(g.newTemporary());
        mark(node);
        OpPyUnpackSequence::emit(&g, values.isEmpty() ? VirtualRegister { 0 } : values[0]->virtualRegister(), values.size(), starIndex < 0 ? values.size() : starIndex, value);
        for (size_t i = 0; i < targets.size(); ++i) {
            Expression* target = targets[i];
            if (auto* starred = target->tryAs<Starred>())
                target = starred->value;
            emitAssign(target, values[i].get());
        }
    }

    void emitDelete(Expression* target)
    {
        switch (target->kind) {
        case Expression::Kind::Name:
            emitDeleteName(*target->as<Name>().id, *target);
            return;
        case Expression::Kind::Attribute: {
            auto& node = target->as<Attribute>();
            Reg base = emit(node.value);
            mark(locationOf(node));
            OpPyDelAttr::emit(&g, base.get(), g.addConstant(mangle(*node.attribute)));
            return;
        }
        case Expression::Kind::Subscript: {
            auto& node = target->as<Subscript>();
            Reg base = emit(node.value);
            Reg key = emit(node.slice);
            mark(node);
            OpPyDelItem::emit(&g, base.get(), key.get());
            return;
        }
        case Expression::Kind::Tuple:
            for (Expression* element : target->as<Tuple>().elements)
                emitDelete(element);
            return;
        case Expression::Kind::List:
            for (Expression* element : target->as<List>().elements)
                emitDelete(element);
            return;
        case Expression::Kind::Starred:
            // del *x, which there is no writing.
            fail("can't use starred expression here"_s, *target);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void emitAugmentedAssignment(AugAssign& node)
    {
        switch (node.target->kind) {
        case Expression::Kind::Name: {
            const Identifier& name = *node.target->as<Name>().id;
            Reg current = emitLoadName(nullptr, name, *node.target);
            Reg value = emit(node.value);
            RegisterID* local = registerForStore(node.target);
            Reg result = local ? Reg(local) : Reg(g.newTemporary());
            mark(node);
            emitBinaryOperation(result.get(), node.op, true, current.get(), value.get());
            emitStoreName(name, result.get(), *node.target);
            return;
        }
        case Expression::Kind::Attribute: {
            auto& target = node.target->as<Attribute>();
            const Identifier& name = mangle(*target.attribute);
            Reg base = emitToTemporary(target.value);
            Reg current = g.newTemporary();
            mark(locationOf(target));
            emitGetAttribute(current.get(), base.get(), name);
            Reg value = emit(node.value);
            mark(node);
            emitBinaryOperation(current.get(), node.op, true, current.get(), value.get());
            mark(locationOf(target));
            emitSetAttribute(base.get(), name, current.get());
            return;
        }
        case Expression::Kind::Subscript: {
            auto& target = node.target->as<Subscript>();
            Reg base = emitToTemporary(target.value);
            Reg key = emitToTemporary(target.slice);
            Reg current = g.newTemporary();
            mark(target);
            emitGetItem(current.get(), base.get(), key.get());
            Reg value = emit(node.value);
            mark(node);
            emitBinaryOperation(current.get(), node.op, true, current.get(), value.get());
            mark(target);
            OpPySetItem::emit(&g, base.get(), key.get(), current.get());
            return;
        }
        default:
            // [x] += y, which there is no writing. CPython numbers the kinds from 1.
            fail(SyntaxError::Kind::SystemError, concatenate("invalid node type ("_s, static_cast<unsigned>(node.target->kind) + 1, ") for augmented assignment"_s));
            return;
        }
    }

    // ---- Statements

    void emit(Sequence<Statement*> statements)
    {
        for (size_t i = 0; i < statements.size(); ++i) {
            emit(*statements[i]);
            // What comes after a `return` is not there, to CPython.
            if (i + 1 < statements.size() && !m_isNeverComeTo && alwaysLeaves(*statements[i]))
                return emitNeverComeTo(statements.subspan(i + 1));
        }
    }

    void emit(Statement& statement)
    {
        if (m_error)
            return;
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            return fail("maximum recursion depth exceeded during compilation"_s, statement);

        // The generator takes back an instruction that the next one makes pointless, and what has been said of where the next one is would then be wrong.
        g.disablePeepholeOptimization();
        // It begins a line, which is the line of whatever in it is done first: that of `b` in `a = (\n b)`. That says so when it is come to. These do something themselves
        // before anything that is in them does, or have nothing in them.
        forgetLine();
        unsigned numberOfLines = m_numberOfLines;
        if (!statement.is<Global>() && !statement.is<Nonlocal>())
            beginLineOfStatement();
        switch (statement.kind) {
        case Statement::Kind::Break:
        case Statement::Kind::Continue:
        case Statement::Kind::Try:
        case Statement::Kind::Import:
        case Statement::Kind::ImportFrom:
            mark(statement);
            break;
        case Statement::Kind::Return:
            if (!statement.as<Return>().value)
                mark(statement);
            break;
        default:
            break;
        }
        emitWithoutLine(statement);
        // Every line that there is a statement on is the line of some instruction, though the statement do nothing: co_lines(). `global` and `nonlocal` are not there to be run.
        if (m_numberOfLines == numberOfLines && !statement.is<Global>() && !statement.is<Nonlocal>() && !m_error)
            mark(statement);
    }

    void emitWithoutLine(Statement& statement)
    {
        switch (statement.kind) {
        case Statement::Kind::Expr: {
            Expression* value = statement.as<Expr>().value;
            if (m_info.kind == CodeKind::Interactive) {
                // At a prompt, what an expression comes to is shown.
                Reg result = emit(value);
                emitRuntimeCall(nullptr, "displayHook"_s, { result.get() }, statement);
                return;
            }
            // A docstring, or some other constant that does nothing. It is passed through all the same.
            if (value->is<Constant>())
                return;
            Reg ignored = emit(value);
            return;
        }
        case Statement::Kind::Assign: {
            auto& node = statement.as<Assign>();
            if (node.targets.size() == 1) {
                if (RegisterID* local = registerForStore(node.targets[0], NotesName::Later)) {
                    emitInto(local, node.value);
                    noteStore(node.targets[0]);
                    markIfOnAnotherLine(*node.targets[0]);
                    return;
                }
            }
            // As CPython has it, no tuple is made of two or three things that are to be taken apart again at once.
            auto elementsOf = [] (Expression* expression) -> std::optional<Sequence<Expression*>> {
                if (auto* tuple = expression->tryAs<Tuple>())
                    return tuple->elements;
                if (auto* list = expression->tryAs<List>())
                    return list->elements;
                return std::nullopt;
            };
            auto targets = node.targets.size() == 1 ? elementsOf(node.targets[0]) : std::nullopt;
            bool isTakenApart = targets && node.value->is<Tuple>() && targets->size() == node.value->as<Tuple>().elements.size() && targets->size() <= 3;
            SetForScope tupleThatIsUnpacked(m_tupleThatIsUnpacked, isTakenApart ? static_cast<const Node*>(node.value) : nullptr);
            Reg value = emitToTemporary(node.value);
            for (Expression* target : node.targets)
                emitAssign(target, value.get());
            return;
        }
        case Statement::Kind::AugAssign:
            return emitAugmentedAssignment(statement.as<AugAssign>());
        case Statement::Kind::AnnAssign: {
            return emitAnnotatedAssignment(statement.as<AnnAssign>());
        }
        case Statement::Kind::Return: {
            auto& node = statement.as<Return>();
            if (!isFunctionLike() || m_info.kind == CodeKind::Class)
                return fail("'return' outside function"_s, node);
            if (node.value && m_info.isCoroutine && m_info.isGenerator)
                return fail("'return' with value in async generator"_s, node);
            // By then CPython has gone on to the value, if it has nothing to do for it and it is on the same line.
            if (m_isInExceptStar)
                return fail("'break', 'continue' and 'return' cannot appear in an except* block"_s, node.value && node.value->is<Constant>() && node.value->line == node.line ? static_cast<Node&>(*node.value) : node);
            if (!node.value)
                noteConstant({ CodeDetails::Constant::Kind::None });
            if (!m_isNeverComeTo && !m_waysOut.isEmpty()) {
                auto* returned = node.value ? constantOf(node.value) : nullptr;
                JSValue plain = node.value ? (returned ? plainValueOf(*returned) : JSValue()) : jsUndefined();
                // A constant that is an object is one to CPython, and here it is like anything else that is worked out.
                noteWayOut(static_cast<int>(CompletionType::Return), plain ? constant(plain)->virtualRegister() : VirtualRegister(), !plain, false);
            }
            Reg value = node.value ? Reg(emitToTemporary(node.value)) : Reg(g.emitLoad(g.newTemporary(), jsUndefined()));
            markIfOnAnotherLine(node);
            if (!g.emitReturnViaFinallyIfNeeded(value.get())) {
                mark(node);
                g.emitReturn(value.get());
            }
            return;
        }
        case Statement::Kind::Delete:
            for (Expression* target : statement.as<Delete>().targets)
                emitDelete(target);
            return;
        case Statement::Kind::Pass:
        case Statement::Kind::Global:
        case Statement::Kind::Nonlocal:
            return;
        case Statement::Kind::If: {
            auto& node = statement.as<If>();
            if (auto truth = constantTruth(node.test)) {
                // The line is come to, though there is nothing to find out.
                mark(*node.test);
                emitNeverComeTo(*truth ? node.orElse : node.body);
                emit(*truth ? node.body : node.orElse);
                return;
            }
            Ref<Label> otherwise = g.newLabel();
            emitBranch(node.test, otherwise.get(), false);
            emit(node.body);
            if (node.orElse.empty()) {
                emitLabel(otherwise.get());
                return;
            }
            Ref<Label> end = g.newLabel();
            emitJump(end.get());
            emitLabel(otherwise.get());
            emit(node.orElse);
            emitLabel(end.get());
            return;
        }
        case Statement::Kind::While: {
            auto& node = statement.as<While>();
            Ref<Label> end = emitLoop([&] (LabelScope& scope) {
                auto truth = constantTruth(node.test);
                if (truth && !*truth) {
                    mark(*node.test);
                    NestedBlock block(*this, node);
                    emitNeverComeTo(node.body);
                    // A `continue` in it is never come to either, but it has to go somewhere.
                    emitLabel(*scope.continueTarget());
                    return;
                }
                Ref<Label> otherwise = g.newLabel();
                Ref<Label> head = g.newLabel();
                emitLabel(head.get());
                g.emitLoopHint();
                if (truth)
                    mark(*node.test);
                else
                    emitBranch(node.test, otherwise.get(), false);
                {
                    NestedBlock block(*this, node);
                    emit(node.body);
                }
                emitLabel(*scope.continueTarget());
                emitLineAfterBackwardJump(*node.test);
                g.emitJump(head.get());
                emitLabel(otherwise.get());
            });
            emit(node.orElse);
            emitLabel(end.get());
            return;
        }
        case Statement::Kind::For:
            return emitFor(statement.as<For>());
        case Statement::Kind::Break: {
            LabelScope* scope = g.breakTarget(Identifier());
            if (m_isInExceptStarOutsideLoop)
                return fail("'break', 'continue' and 'return' cannot appear in an except* block"_s, statement);
            if (!scope)
                return fail("'break' outside loop"_s, statement);
            noteWayOut(static_cast<int>(bytecodeOffsetToJumpID(g.instructions().size())), VirtualRegister(), false, true);
            if (!g.emitJumpViaFinallyIfNeeded(scope->scopeDepth(), scope->breakTarget())) {
                g.restoreScopeRegister(g.labelScopeDepthToLexicalScopeIndex(scope->scopeDepth()));
                emitJump(scope->breakTarget());
            }
            return;
        }
        case Statement::Kind::Continue: {
            LabelScope* scope = g.continueTarget(Identifier());
            if (m_isInExceptStarOutsideLoop)
                return fail("'break', 'continue' and 'return' cannot appear in an except* block"_s, statement);
            if (!scope)
                return fail("'continue' not properly in loop"_s, statement);
            noteWayOut(static_cast<int>(bytecodeOffsetToJumpID(g.instructions().size())), VirtualRegister(), false, true);
            if (!g.emitJumpViaFinallyIfNeeded(scope->scopeDepth(), *scope->continueTarget())) {
                g.restoreScopeRegister(g.labelScopeDepthToLexicalScopeIndex(scope->scopeDepth()));
                emitJump(*scope->continueTarget());
            }
            return;
        }
        case Statement::Kind::FunctionDef: {
            auto& node = statement.as<FunctionDef>();
            emitDecorated(node.decorators, *node.name, node, [&] (RegisterID* dst) {
                if (node.typeParameters.empty()) {
                    emitFunction(dst, CodeKind::Function, *node.name, node.arguments, node, &node);
                    return;
                }
                Defaults defaults = emitDefaults(node.arguments, node);
                emitCallTypeParameters(dst, OwnerKind::Function, *node.name, node.typeParameters, node, defaults.positional.get(), defaults.keywordOnly.get());
            });
            return;
        }
        case Statement::Kind::ClassDef:
            return emitClass(statement.as<ClassDef>());
        case Statement::Kind::Raise:
            return emitRaise(statement.as<Raise>());
        case Statement::Kind::Try:
            return emitTry(statement.as<Try>());
        case Statement::Kind::With:
            return emitWith(statement.as<With>(), 0);
        case Statement::Kind::Assert: {
            auto& node = statement.as<Assert>();
            if (m_info.optimizationLevel)
                return;
            Ref<Label> holds = g.newLabel();
            emitBranch(node.test, holds.get(), true);
            Reg message = node.message ? Reg(emit(node.message)) : Reg(marker());
            // It is what does not hold that is pointed at.
            emitRuntimeCall(nullptr, "raiseAssertionError"_s, { message.get() }, *node.test);
            emitLabel(holds.get());
            return;
        }
        case Statement::Kind::Import:
            return emitImport(statement.as<Import>());
        case Statement::Kind::ImportFrom:
            return emitImportFrom(statement.as<ImportFrom>());
        case Statement::Kind::Match:
            return emitMatch(statement.as<Match>());
        case Statement::Kind::TypeAlias: {
            auto& node = statement.as<TypeAlias>();
            const Identifier& name = *node.name->as<Name>().id;
            Reg alias = g.newTemporary();
            if (node.typeParameters.empty())
                emitNewTypeAlias(alias.get(), node, nullptr);
            else
                emitCallTypeParameters(alias.get(), OwnerKind::TypeAlias, name, node.typeParameters, node);
            emitStoreName(name, alias.get(), node);
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // ---- match

    // What a pattern has captured is kept aside, and given its name only once the whole pattern has matched.
    struct Capture {
        const Identifier* name;
        Reg value;
    };

    struct PatternContext {
        Vector<Capture, 4> captures;
        // Whether a pattern that always matches may stand here. It may not where there are others after it to try.
        bool allowIrrefutable { true };
    };

    void addCapture(PatternContext& context, const Identifier& name, RegisterID* value, const Node& node)
    {
        for (auto& capture : context.captures) {
            if (*capture.name == name)
                return fail(concatenate("multiple assignments to name '"_s, name.string(), "' in pattern"_s), node);
        }
        Reg copy = g.newTemporary();
        g.move(copy.get(), value);
        context.captures.append({ &name, WTF::move(copy) });
    }

    static bool isWildcard(Pattern& pattern)
    {
        return pattern.is<MatchAs>() && !pattern.as<MatchAs>().name;
    }

    static bool isStarWildcard(Pattern& pattern)
    {
        return pattern.is<MatchStar>() && !pattern.as<MatchStar>().name;
    }

    // The number that a constant is, if it is one: True is 1.
    static std::optional<double> numberOf(Constant& constant)
    {
        switch (constant.type) {
        case Constant::Type::True:
            return 1;
        case Constant::Type::False:
            return 0;
        case Constant::Type::Integer:
            return constant.isNegative ? -static_cast<double>(constant.integer) : static_cast<double>(constant.integer);
        case Constant::Type::Float:
            return constant.real;
        default:
            return std::nullopt;
        }
    }

    // The number that is written in a pattern: 1, -1, 1.5, 2j, 1+2j and so on, and True and False. CPython has worked these out by the time that it generates code.
    struct PatternNumber {
        // What is not an int is a float, or complex.
        bool isInt { false };
        bool isComplex { false };
        bool isBool { false };
        bool isNegative { false };
        uint64_t magnitude { 0 };
        String largeMagnitude; // In decimal, if it is too large for that.
        double real { 0 };
        double imaginary { 0 };

        // As two keys of a dict are the same.
        bool operator==(const PatternNumber& other) const
        {
            if (imaginary != other.imaginary)
                return false;
            if (isInt && other.isInt)
                return magnitude == other.magnitude && largeMagnitude == other.largeMagnitude && (isNegative == other.isNegative || isZero());
            if (!isInt && !other.isInt)
                return real == other.real;
            const PatternNumber& integer = isInt ? *this : other;
            double value = isInt ? other.real : real;
            // Only if the float is that very int.
            if (std::trunc(value) != value || !std::isfinite(value) || (std::signbit(value) != integer.isNegative && !integer.isZero()))
                return false;
            bool isLarge = std::abs(value) >= 18446744073709551616.0;
            if (isLarge != !integer.largeMagnitude.isNull())
                return false;
            if (!isLarge)
                return static_cast<uint64_t>(std::abs(value)) == integer.magnitude;
            // It is so many bits, and then nothing but noughts.
            int exponent;
            auto bits = static_cast<uint64_t>(std::ldexp(std::frexp(std::abs(value), &exponent), 53));
            StringBuilder binary;
            for (unsigned i = 53; i--;)
                binary.append(bits >> i & 1 ? '1' : '0');
            for (int i = 53; i < exponent; ++i)
                binary.append('0');
            return toDecimal(binary.toString(), 2) == integer.largeMagnitude;
        }

        bool isZero() const { return !magnitude && largeMagnitude.isNull(); }
        double toDouble() const
        {
            if (!isInt)
                return real;
            double result = largeMagnitude.isNull() ? static_cast<double>(magnitude) : largeMagnitude.toDouble();
            return isNegative ? -result : result;
        }

        String repr() const
        {
            if (isBool)
                return magnitude ? "True"_s : "False"_s;
            if (isInt && !largeMagnitude.isNull())
                return concatenate(isNegative ? "-"_s : ""_s, largeMagnitude);
            if (isInt)
                return concatenate(isNegative && magnitude ? "-"_s : ""_s, magnitude);
            if (!isComplex)
                return reprOfDouble(real);
            return reprOfComplex(real, imaginary);
        }
    };

    static std::optional<PatternNumber> patternNumberOf(Expression& expression)
    {
        if (auto* constant = expression.tryAs<Constant>()) {
            PatternNumber number;
            switch (constant->type) {
            case Constant::Type::True:
            case Constant::Type::False:
                number.isBool = true;
                number.isInt = true;
                number.magnitude = constant->type == Constant::Type::True;
                return number;
            case Constant::Type::Integer:
                number.isInt = true;
                number.magnitude = constant->integer;
                number.isNegative = constant->isNegative;
                return number;
            case Constant::Type::BigInteger:
                number.isInt = true;
                number.largeMagnitude = toDecimal(constant->text->string(), constant->radix);
                number.isNegative = constant->isNegative;
                // It may have been written at length.
                if (auto small = parseInteger<uint64_t>(number.largeMagnitude)) {
                    number.magnitude = *small;
                    number.largeMagnitude = { };
                }
                return number;
            case Constant::Type::Float:
                number.real = constant->real;
                return number;
            case Constant::Type::Imaginary:
                number.isComplex = true;
                number.imaginary = constant->real;
                return number;
            case Constant::Type::Complex:
                number.isComplex = true;
                number.real = constant->real;
                number.imaginary = constant->imaginary;
                return number;
            default:
                return std::nullopt;
            }
        }
        if (auto* unary = expression.tryAs<UnaryOp>()) {
            if (unary->op != UnaryOperator::USub)
                return std::nullopt;
            auto number = patternNumberOf(*unary->operand);
            if (!number || number->isBool)
                return std::nullopt;
            number->isNegative = !number->isNegative;
            number->real = -number->real;
            number->imaginary = -number->imaginary;
            return number;
        }
        if (auto* binary = expression.tryAs<BinOp>()) {
            if (binary->op != BinaryOperator::Add && binary->op != BinaryOperator::Sub)
                return std::nullopt;
            auto left = patternNumberOf(*binary->left);
            auto right = patternNumberOf(*binary->right);
            if (!left || !right || left->isComplex || left->isBool || !right->isComplex)
                return std::nullopt;
            PatternNumber number;
            number.isComplex = true;
            number.real = left->toDouble();
            number.real = binary->op == BinaryOperator::Add ? number.real + right->real : number.real - right->real;
            number.imaginary = binary->op == BinaryOperator::Add ? right->imaginary : -right->imaginary;
            return number;
        }
        return std::nullopt;
    }

    // Whether they would be the same key of a dict.
    static bool isSameConstant(Constant& a, Constant& b)
    {
        if (auto number = numberOf(a))
            return number == numberOf(b);
        if (a.type != b.type)
            return false;
        return a.type == Constant::Type::None || (a.text && b.text && *a.text == *b.text && a.isNegative == b.isNegative);
    }

    static String reprOfConstant(Constant& constant)
    {
        switch (constant.type) {
        case Constant::Type::None:
            return "None"_s;
        case Constant::Type::True:
            return "True"_s;
        case Constant::Type::False:
            return "False"_s;
        case Constant::Type::Integer:
            return String::number(constant.integer);
        case Constant::Type::Float:
            return reprOfDouble(constant.real);
        case Constant::Type::String:
            return reprOfString(constant.text->string());
        case Constant::Type::Bytes:
            return concatenate('b', reprOfString(constant.text->string()));
        default:
            return constant.text ? constant.text->string() : String();
        }
    }

    // A pattern inside another. There, one that always matches hides nothing.
    void emitSubpattern(Pattern& pattern, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        SetForScope allow(context.allowIrrefutable, true);
        emitPattern(pattern, subject, mismatch, context);
    }

    void emitJumpIfMarker(RegisterID* value, Label& target)
    {
        OpJeqPtr::emit(&g, value, marker(), target.bind(&g));
    }

    // The elements of a tuple that the runtime made, each in a register.
    Vector<Reg, 8> emitUnpackExactly(RegisterID* tuple, unsigned count)
    {
        Vector<Reg, 8> values;
        for (unsigned i = 0; i < count; ++i)
            values.append(g.newTemporary());
        if (count)
            OpPyUnpackSequence::emit(&g, values[0]->virtualRegister(), count, count, tuple);
        return values;
    }

    // Goes on if the subject matches, and jumps if it does not.
    void emitPattern(Pattern& pattern, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        if (m_error)
            return;
        switch (pattern.kind) {
        case Pattern::Kind::MatchValue: {
            Expression& expression = *pattern.as<MatchValue>().value;
            if (!expression.is<Constant>() && !expression.is<Attribute>() && !patternNumberOf(expression))
                return fail("patterns may only match literals and attribute lookups"_s, pattern);
            Reg value = emit(pattern.as<MatchValue>().value);
            Reg same = g.newTemporary();
            mark(pattern);
            emitCompare(same.get(), ComparisonOperator::Eq, subject, value.get());
            emitJumpIfFalse(same.get(), mismatch);
            return;
        }
        case Pattern::Kind::MatchSingleton: {
            Reg same = g.newTemporary();
            Constant::Type value = pattern.as<MatchSingleton>().value;
            if (value == Constant::Type::None)
                g.emitIsUndefinedOrNull(same.get(), subject);
            else
                emitCompare(same.get(), ComparisonOperator::Is, subject, constant(jsBoolean(value == Constant::Type::True)));
            g.emitJumpIfFalse(same.get(), mismatch);
            return;
        }
        case Pattern::Kind::MatchAs: {
            auto& node = pattern.as<MatchAs>();
            if (!node.pattern) {
                if (!context.allowIrrefutable) {
                    if (node.name)
                        return fail(concatenate("name capture '"_s, node.name->string(), "' makes remaining patterns unreachable"_s), node);
                    return fail("wildcard makes remaining patterns unreachable"_s, node);
                }
            } else
                emitPattern(*node.pattern, subject, mismatch, context);
            if (node.name)
                addCapture(context, *node.name, subject, node);
            return;
        }
        case Pattern::Kind::MatchOr:
            return emitOrPattern(pattern.as<MatchOr>(), subject, mismatch, context);
        case Pattern::Kind::MatchSequence:
            return emitSequencePattern(pattern.as<MatchSequence>(), subject, mismatch, context);
        case Pattern::Kind::MatchMapping:
            return emitMappingPattern(pattern.as<MatchMapping>(), subject, mismatch, context);
        case Pattern::Kind::MatchClass:
            return emitClassPattern(pattern.as<MatchClass>(), subject, mismatch, context);
        case Pattern::Kind::MatchStar:
            // The parser lets it be only in a sequence, which deals with it.
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void emitOrPattern(MatchOr& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        Ref<Label> matched = g.newLabel();
        // Whichever alternative matches, what it captured ends up here.
        Vector<Capture, 4> common;
        for (size_t i = 0; i < node.patterns.size(); ++i) {
            PatternContext alternative;
            alternative.allowIrrefutable = i + 1 == node.patterns.size() && context.allowIrrefutable;
            Ref<Label> next = g.newLabel();
            emitPattern(*node.patterns[i], subject, next.get(), alternative);
            if (m_error)
                return;
            if (!i) {
                for (auto& capture : alternative.captures)
                    common.append({ capture.name, g.newTemporary() });
            } else if (alternative.captures.size() != common.size())
                return fail("alternative patterns bind different names"_s, node);
            for (auto& capture : alternative.captures) {
                auto* target = common.findIf([&] (auto& candidate) { return *candidate.name == *capture.name; }) == notFound ? nullptr : &common[common.findIf([&] (auto& candidate) { return *candidate.name == *capture.name; })];
                if (!target)
                    return fail("alternative patterns bind different names"_s, node);
                g.move(target->value.get(), capture.value.get());
            }
            g.emitJump(matched.get());
            emitLabel(next.get());
        }
        g.emitJump(mismatch);
        emitLabel(matched.get());
        for (auto& capture : common)
            addCapture(context, *capture.name, capture.value.get(), node);
    }

    void emitSequencePattern(MatchSequence& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        unsigned size = node.patterns.size();
        int star = -1;
        bool onlyWildcards = true;
        bool starIsWildcard = false;
        for (unsigned i = 0; i < size; ++i) {
            Pattern& element = *node.patterns[i];
            if (element.is<MatchStar>()) {
                if (star >= 0)
                    return fail("multiple starred names in sequence pattern"_s, node);
                starIsWildcard = isStarWildcard(element);
                onlyWildcards &= starIsWildcard;
                star = i;
                continue;
            }
            onlyWildcards &= isWildcard(element);
        }

        // That it is a sequence, and how long: exactly so, or at least so if a star takes up the slack.
        Reg fits = g.newTemporary();
        emitRuntimeCall(fits.get(), "matchSequence"_s, { subject, constant(JSC::jsNumber(star < 0 ? size : size - 1)), constant(jsBoolean(star >= 0)) }, node);
        g.emitJumpIfFalse(fits.get(), mismatch);
        if (onlyWildcards)
            return;

        if (starIsWildcard) {
            // Only what is wanted is got, by its index.
            Reg length;
            for (unsigned i = 0; i < size; ++i) {
                Pattern& element = *node.patterns[i];
                if (static_cast<int>(i) == star || isWildcard(element))
                    continue;
                Reg index = g.newTemporary();
                if (static_cast<int>(i) < star)
                    g.emitLoad(index.get(), JSC::jsNumber(i));
                else {
                    if (!length) {
                        length = g.newTemporary();
                        emitRuntimeCall(length.get(), "length"_s, { subject }, node);
                    }
                    emitBinaryOperation(index.get(), BinaryOperator::Sub, false, length.get(), constant(JSC::jsNumber(size - i)));
                }
                Reg value = g.newTemporary();
                mark(element);
                emitGetItem(value.get(), subject, index.get());
                emitSubpattern(element, value.get(), mismatch, context);
            }
            return;
        }

        Vector<Reg, 8> values;
        for (unsigned i = 0; i < size; ++i)
            values.append(g.newTemporary());
        mark(node);
        OpPyUnpackSequence::emit(&g, values[0]->virtualRegister(), size, star < 0 ? size : star, subject);
        for (unsigned i = 0; i < size; ++i) {
            Pattern& element = *node.patterns[i];
            if (auto* starred = element.is<MatchStar>() ? &element.as<MatchStar>() : nullptr)
                addCapture(context, *starred->name, values[i].get(), element);
            else
                emitSubpattern(element, values[i].get(), mismatch, context);
        }
    }

    void emitMappingPattern(MatchMapping& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        unsigned size = node.keys.size();
        Reg fits = g.newTemporary();
        emitRuntimeCall(fits.get(), "matchMapping"_s, { subject, constant(JSC::jsNumber(size)) }, node);
        g.emitJumpIfFalse(fits.get(), mismatch);
        if (!size && !node.rest)
            return;

        // A key that is written out twice can be seen now. One that is looked up has to wait until it is.
        for (unsigned i = 0; i < size; ++i) {
            Expression& key = *node.keys[i];
            if (auto number = patternNumberOf(key)) {
                for (unsigned j = 0; j < i; ++j) {
                    if (auto other = patternNumberOf(*node.keys[j]); other && *number == *other)
                        return fail(concatenate("mapping pattern checks duplicate key ("_s, number->repr(), ')'), node);
                }
                continue;
            }
            auto* constant = key.tryAs<Constant>();
            if (!constant) {
                if (!key.is<Attribute>())
                    return fail("mapping pattern keys may only match literals and attribute lookups"_s, node);
                continue;
            }
            for (unsigned j = 0; j < i; ++j) {
                auto* other = node.keys[j]->tryAs<Constant>();
                if (other && isSameConstant(*constant, *other))
                    return fail(concatenate("mapping pattern checks duplicate key ("_s, reprOfConstant(*constant), ')'), node);
            }
        }

        Vector<Reg, 8> keyRegisters;
        emitElements(node.keys, keyRegisters);
        Reg keys = g.newTemporary();
        emitNewTuple(keys.get(), keyRegisters);
        Reg found = g.newTemporary();
        emitRuntimeCall(found.get(), "matchKeys"_s, { subject, keys.get() }, node);
        emitJumpIfMarker(found.get(), mismatch);
        auto values = emitUnpackExactly(found.get(), size);
        for (unsigned i = 0; i < size; ++i)
            emitSubpattern(*node.patterns[i], values[i].get(), mismatch, context);
        if (node.rest) {
            Reg rest = g.newTemporary();
            emitRuntimeCall(rest.get(), "matchRest"_s, { subject, keys.get() }, node);
            addCapture(context, *node.rest, rest.get(), node);
        }
    }

    void emitClassPattern(MatchClass& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        unsigned positional = node.patterns.size();
        unsigned keywords = node.keywordAttributes.size();
        for (unsigned i = 0; i < keywords; ++i) {
            for (unsigned j = i + 1; j < keywords; ++j) {
                if (*node.keywordAttributes[i] == *node.keywordAttributes[j])
                    return fail(concatenate("attribute name repeated in class pattern: "_s, node.keywordAttributes[i]->string()), *node.keywordPatterns[j]);
            }
        }
        auto* names = JSCellButterfly::create(m_vm, CopyOnWriteArrayWithContiguous, keywords);
        for (unsigned i = 0; i < keywords; ++i)
            names->setIndex(m_vm, i, g.addStringConstant(*node.keywordAttributes[i]));

        Reg cls = emitToTemporary(node.cls);
        Reg found = g.newTemporary();
        emitRuntimeCall(found.get(), "matchClass"_s, { subject, cls.get(), constant(JSC::jsNumber(positional)), constant(names) }, node);
        emitJumpIfMarker(found.get(), mismatch);
        auto values = emitUnpackExactly(found.get(), positional + keywords);
        for (unsigned i = 0; i < positional + keywords; ++i) {
            Pattern& element = i < positional ? *node.patterns[i] : *node.keywordPatterns[i - positional];
            if (!isWildcard(element))
                emitSubpattern(element, values[i].get(), mismatch, context);
        }
    }

    void emitMatch(Match& node)
    {
        Reg subject = emitToTemporary(node.subject);
        Ref<Label> end = g.newLabel();
        // codegen_match_inner(): CPython is done with what it is matching when it comes to a `case _:` that is the last of several, or has matched it with the pattern before that. With an earlier pattern, it is
        // done with it as it begins on what to do.
        Pattern& last = *node.cases.back()->pattern;
        size_t numberWithSubject = node.cases.size() - (node.cases.size() > 1 && last.is<MatchAs>() && !last.as<MatchAs>().pattern && !last.as<MatchAs>().name);
        for (size_t i = 0; i < node.cases.size(); ++i) {
            MatchCase& matchCase = *node.cases[i];
            Ref<Label> next = g.newLabel();
            PatternContext context;
            context.allowIrrefutable = matchCase.guard || i + 1 == node.cases.size();
            {
                std::optional<JumpBlock> subjectBlock;
                if (i < numberWithSubject)
                    subjectBlock.emplace(*this, CodeDetails::JumpBlock::Kind::Subject, subject.get());
                // `case _:` is come to, though there is nothing to it.
                markWhereItCanBeGoneOnFrom(*matchCase.pattern);
                emitPattern(*matchCase.pattern, subject.get(), next.get(), context);
                if (m_error)
                    return;
                for (auto& capture : context.captures)
                    emitStoreName(*capture.name, capture.value.get(), *matchCase.pattern);
                context.captures.clear();
                if (matchCase.guard)
                    emitBranch(matchCase.guard, next.get(), false);
                if (i + 1 < numberWithSubject) {
                    forgetLine();
                    beginLineOfStatement();
                }
            }
            emit(matchCase.body);
            emitJump(end.get());
            emitLabel(next.get());
        }
        emitLabel(end.get());
    }

    // A loop, a `with`, and each part of a `try`. CPython keeps a stack of them as it generates code, which has room for so many (CO_MAXBLOCKS), and a program that has more of them one
    // inside another is refused. There is no such stack here, and they are counted so that the same programs are.
    class NestedBlock {
        WTF_MAKE_NONCOPYABLE(NestedBlock);
    public:
        NestedBlock(CodeGenerator& generator, const Node& node)
            : m_generator(generator)
        {
            static constexpr unsigned maximum = 21;
            if (m_generator.m_nestedBlocks++ >= maximum)
                m_generator.fail("too many statically nested blocks"_s, node);
        }

        ~NestedBlock() { --m_generator.m_nestedBlocks; }

    private:
        CodeGenerator& m_generator;
    };

    void emitFor(For& node)
    {
        if (node.isAsync && !checkIsAsync("'async for'"_s, node))
            return;
        Reg iterator = g.newTemporary();
        {
            Reg iterable = node.isAsync ? emit(node.iterable) : emitToLookInOrGoThrough(node.iterable);
            mark(*node.iterable);
            if (node.isAsync)
                emitRuntimeCall(iterator.get(), "getAsyncIterator"_s, { iterable.get() }, *node.iterable);
            else
                OpPyGetIter::emit(&g, iterator.get(), iterable.get());
        }
        Ref<Label> end = emitLoop([&] (LabelScope& scope) {
            Ref<Label> exhausted = g.newLabel();
            Ref<Label> head = g.newLabel();
            emitLabel(head.get());
            g.emitLoopHint();
            {
                RegisterID* local = registerForStore(node.target);
                // Not straight into the variable: it keeps its last value when there is no more.
                Reg value = g.newTemporary();
                mark(*node.iterable);
                if (node.isAsync)
                    emitAsyncNext(value.get(), iterator.get(), exhausted.get(), *node.iterable);
                else
                    emitIteratorNext(value.get(), iterator.get(), exhausted.get(), *node.iterable);
                if (local)
                    g.move(local, value.get());
                else
                    emitAssign(node.target, value.get());
            }
            {
                NestedBlock block(*this, node);
                JumpBlock loop(*this, CodeDetails::JumpBlock::Kind::Loop, iterator.get());
                emit(node.body);
            }
            emitLabel(*scope.continueTarget());
            emitLineAfterBackwardJump(*node.iterable);
            g.emitJump(head.get());
            emitLabel(exhausted.get());
            if (!node.isAsync) {
                JumpBlock loopEnd(*this, CodeDetails::JumpBlock::Kind::LoopEnd);
                markWhereItCanBeGoneOnFrom(*node.iterable);
            }
        });
        emit(node.orElse);
        emitLabel(end.get());
    }

    // What `break` and `continue` are for. What comes after `else` is not part of it: the loop is over by then. Where `break` goes to is returned, for that to be put before.
    template<typename EmitLoop>
    Ref<Label> emitLoop(const EmitLoop& emit)
    {
        Ref<LabelScope> scope = g.newLabelScope(LabelScope::Loop);
        SetForScope mayBreak(m_isInExceptStarOutsideLoop, false);
        SetForScope loopNesting(m_loopNesting, m_loopNesting + 1);
        Ref<Label> end = scope->breakTarget();
        emit(scope.get());
        return end;
    }

    // @a @b def f: ... is f = a(b(f)). The decorators are evaluated first, from the top.
    template<typename EmitDefinition>
    void emitDecorated(Sequence<Expression*> decorators, const Identifier& name, const Node& node, const EmitDefinition& emitDefinition)
    {
        Vector<Reg, 4> functions;
        for (Expression* decorator : decorators)
            functions.append(emitToTemporary(decorator));
        Reg value = g.newTemporary();
        mark(node);
        {
            SetForScope decoratorLine(m_decoratorLine, decorators.empty() ? 0 : decorators[0]->line);
            emitDefinition(value.get());
        }
        for (unsigned i = functions.size(); i--;) {
            CallArguments call(g, nullptr, 1);
            g.emitLoad(call.thisRegister(), jsUndefined());
            g.move(call.argumentRegister(0), value.get());
            emitRawCall(value.get(), functions[i].get(), call, 1, *decorators[i]);
        }
        mark(node);
        emitStoreName(name, value.get(), node);
    }

    // ---- Exceptions

    void emitRaise(Raise& node)
    {
        if (!node.exception) {
            // The exception being handled, again. After `finally` that may be one that is on its way through, which is not known until then.
            if (!m_handledExceptions.isEmpty() && !m_isAfterFinally) {
                mark(node);
                g.emitThrow(m_handledExceptions.last());
                return;
            }
            emitRuntimeCall(nullptr, "reraise"_s, { }, node);
            return;
        }
        Reg exception = emit(node.exception);
        Reg cause = node.cause ? Reg(emit(node.cause)) : Reg(marker());
        emitRuntimeCall(nullptr, "raise"_s, { exception.get(), cause.get() }, node);
    }

    // try: body() finally: finalizer(). However the body is left, by falling off its end, an exception, return, break or continue.
    template<typename Body, typename Finalizer>
    void emitTryFinally(const Body& body, const Finalizer& finalizer)
    {
        Ref<Label> finallyLabel = g.newLabel();
        Ref<Label> finallyEndLabel = g.newLabel();
        FinallyContext context(g, finallyLabel.get());
        g.pushFinallyControlFlowScope(context);

        Ref<Label> tryLabel = g.newEmittedLabel();
        TryData* tryData = g.pushTry(tryLabel.get(), finallyLabel.get(), HandlerType::Finally);
        {
            JumpBlock protectedBlock(*this, CodeDetails::JumpBlock::Kind::Protected, context.completionTypeRegister(), context.completionValueRegister());
            body();
        }
        Ref<Label> tryEndLabel = g.newEmittedLabel();
        g.popTry(tryData, tryEndLabel.get());

        g.popFinallyControlFlowScope();
        g.emitOutOfLineFinallyHandler(context.completionValueRegister(), context.completionTypeRegister(), tryData);
        emitLabel(finallyLabel.get());
        g.restoreScopeRegister();
        if constexpr (std::is_invocable_v<Finalizer, RegisterID*, RegisterID*>)
            finalizer(context.completionTypeRegister(), context.completionValueRegister());
        else
            finalizer();
        g.emitFinallyCompletion(context, finallyEndLabel.get());
        emitLabel(finallyEndLabel.get());
    }

    // try: body() except: handler(the exception, the same as it was thrown) else: otherwise()
    // To let it go on its way, the handler throws the second, which is what says that it is the same throw and not a new one.
    template<typename Body, typename Handler, typename Otherwise>
    void emitTryCatch(const Body& body, const Handler& handler, const Otherwise& otherwise)
    {
        Ref<Label> catchLabel = g.newLabel();
        Ref<Label> endLabel = g.newLabel();
        Ref<Label> tryLabel = g.newEmittedLabel();
        TryData* tryData = g.pushTry(tryLabel.get(), catchLabel.get(), HandlerType::Catch);
        body();
        Ref<Label> tryEndLabel = g.newEmittedLabel();
        g.popTry(tryData, tryEndLabel.get());
        otherwise();
        emitJump(endLabel.get());

        emitLabel(catchLabel.get());
        Reg exception = g.newTemporary();
        Reg thrown = g.newTemporary();
        g.emitOutOfLineExceptionHandler(thrown.get(), exception.get(), nullptr, tryData);
        g.restoreScopeRegister();
        handler(exception.get(), thrown.get());
        emitLabel(endLabel.get());
    }

    // While it runs, `thrown` is the exception being handled, and whichever way it is left, what was being handled before is again.
    template<typename Body>
    void emitWhileHandling(RegisterID* thrown, const Node& node, const Body& body, RegisterID* exception = nullptr, bool isComeTo = true)
    {
        Reg previous = g.newTemporary();
        {
            SetForScope isArtificial(m_isArtificial, true);
            emitRuntimeCall(previous.get(), "pushHandledException"_s, { thrown }, node);
        }
        m_handledExceptions.append(thrown);
        emitTryFinally([&] {
            JumpBlock handler(*this, CodeDetails::JumpBlock::Kind::Handler, previous.get(), thrown, exception, isComeTo);
            SetForScope isAfterFinally(m_isAfterFinally, false);
            body();
        }, [&] {
            SetForScope isArtificial(m_isArtificial, true);
            emitRuntimeCall(nullptr, "popHandledException"_s, { previous.get() }, node);
        });
        m_handledExceptions.removeLast();
    }

    void emitTry(Try& node)
    {
        if (!node.finalBody.empty()) {
            WaysOut waysOut { { }, m_loopNesting, static_cast<unsigned>(m_handledExceptions.size()), alwaysLeave(node.finalBody) };
            emitTryFinally([&] {
                NestedBlock block(*this, node);
                m_waysOut.append(&waysOut);
                emitTryExcept(node);
                m_waysOut.removeLast();
            }, [&] (RegisterID* completionType, RegisterID* completionValue) {
                // If an exception is on its way through, it is what is being handled meanwhile.
                Reg previous = g.newTemporary();
                Reg wasThrown = g.newTemporary();
                g.emitEqualityOp<OpStricteq>(wasThrown.get(), completionType, constant(jsNumber(static_cast<int>(CompletionType::Throw))));
                {
                    SetForScope isArtificial(m_isArtificial, true);
                    emitRuntimeCall(previous.get(), "pushIfThrown"_s, { wasThrown.get(), completionValue }, node);
                }
                emitTryFinally([&] {
                    NestedBlock block(*this, node);
                    JumpBlock finally(*this, CodeDetails::JumpBlock::Kind::Finally, completionType, completionValue, previous.get(), !alwaysLeavesBeforeFinally(node));
                    m_details->jumpBlocks.last().leaves = WTF::move(waysOut.leaves);
                    SetForScope isAfterFinally(m_isAfterFinally, true);
                    emit(node.finalBody);
                }, [&] {
                    SetForScope isArtificial(m_isArtificial, true);
                    emitRuntimeCall(nullptr, "popHandledException"_s, { previous.get() }, node);
                });
            });
            return;
        }
        emitTryExcept(node);
    }

    void emitTryExcept(Try& node)
    {
        if (node.handlers.empty())
            return emit(node.body);
        if (node.isStar)
            return emitTryExceptStar(node);

        for (size_t i = 0; i + 1 < node.handlers.size(); ++i) {
            if (!node.handlers[i]->type)
                return fail("default 'except:' must be last"_s, *node.handlers[i]);
        }

        emitTryCatch([&] {
            NestedBlock block(*this, node);
            emit(node.body);
        }, [&] (RegisterID* exception, RegisterID* thrown) {
            emitWhileHandling(thrown, node, [&] {
                NestedBlock handlers(*this, node);
                Ref<Label> handled = g.newLabel();
                for (ExceptHandler* handler : node.handlers) {
                    Ref<Label> next = g.newLabel();
                    if (handler->type) {
                        JumpBlock matching(*this, CodeDetails::JumpBlock::Kind::Matching);
                        markWhereItCanBeGoneOnFrom(*handler->type);
                        Reg type = emit(handler->type);
                        Reg matches = g.newTemporary();
                        mark(*handler->type);
                        emitCompare(matches.get(), ComparisonOperator::ExceptionMatch, exception, type.get());
                        g.emitJumpIfFalse(matches.get(), next.get());
                    } else {
                        // `except:` has nothing to work out, but it is a line that is come to.
                        JumpBlock matching(*this, CodeDetails::JumpBlock::Kind::Matching);
                        markWhereItCanBeGoneOnFrom(*handler);
                    }
                    NestedBlock block(*this, *handler);
                    if (handler->name) {
                        // except E as name: the name is gone afterwards, so that the exception, which refers to the frame, can go too.
                        emitStoreName(*handler->name, exception, *handler);
                        emitTryFinally([&] {
                            emit(handler->body);
                        }, [&] {
                            SetForScope isArtificial(m_isArtificial, true);
                            emitStoreName(*handler->name, none(), *handler);
                            emitDeleteName(*handler->name, *handler);
                        });
                    } else
                        emit(handler->body);
                    emitJump(handled.get());
                    emitLabel(next.get());
                }
                // Nothing wanted it.
                g.emitThrow(thrown);
                emitLabel(handled.get());
            }, exception, canRaise(node.body));
        }, [&] {
            if (alwaysLeave(node.body))
                emitNeverComeTo(node.orElse);
            else
                emit(node.orElse);
        });
    }

    // try: ... except* E: ... Each clause is given the part of the exception that is for it, and they can all run. What they raise, and what none
    // of them took, is put together again at the end.
    void emitTryExceptStar(Try& node)
    {
        emitTryCatch([&] {
            NestedBlock block(*this, node);
            emit(node.body);
        }, [&] (RegisterID* original, RegisterID* thrown) {
            emitWhileHandling(thrown, node, [&] {
                NestedBlock handlers(*this, node);
                Reg results = g.newTemporary();
                emitNewList(results.get(), { });
                Reg rest = g.newTemporary();
                g.move(rest.get(), original);
                for (ExceptHandler* handler : node.handlers) {
                    // There is no writing that. A tree that a program made can have it, and CPython makes code of it that does not add up, and finds that it does not.
                    if (!handler->type) {
                        fail(SyntaxError::Kind::ValueError, "Invalid CFG, stack underflow"_s);
                        return;
                    }
                    Ref<Label> next = g.newLabel();
                    Reg match = g.newTemporary();
                    {
                        JumpBlock matching(*this, CodeDetails::JumpBlock::Kind::Matching);
                        Reg type = emit(handler->type);
                        Reg pair = g.newTemporary();
                        emitRuntimeCall(pair.get(), "matchExceptionGroup"_s, { rest.get(), type.get() }, *handler); // CHECK_EG_MATCH is where the whole handler is.
                        auto parts = emitUnpackExactly(pair.get(), 2);
                        g.move(match.get(), parts[0].get());
                        g.move(rest.get(), parts[1].get());
                    }
                    Reg isNone = g.newTemporary();
                    g.emitIsUndefinedOrNull(isNone.get(), match.get());
                    g.emitJumpIfTrue(isNone.get(), next.get());

                    emitTryCatch([&] {
                        Reg matchAsThrown = g.newTemporary();
                        emitRuntimeCall(matchAsThrown.get(), "asThrown"_s, { match.get() }, *handler);
                        emitWhileHandling(matchAsThrown.get(), *handler, [&] {
                            SetForScope noReturn(m_isInExceptStar, true);
                            SetForScope noBreak(m_isInExceptStarOutsideLoop, true);
                            NestedBlock block(*this, *handler);
                            if (handler->name) {
                                emitStoreName(*handler->name, match.get(), *handler);
                                emitTryFinally([&] {
                                    emit(handler->body);
                                }, [&] {
                                    SetForScope isArtificial(m_isArtificial, true);
                                    emitStoreName(*handler->name, none(), *handler);
                                    emitDeleteName(*handler->name, *handler);
                                });
                            } else
                                emit(handler->body);
                        });
                    }, [&] (RegisterID* raised, RegisterID*) {
                        SetForScope isArtificial(m_isArtificial, true);
                        emitRuntimeCall(nullptr, "listAppend"_s, { results.get(), raised }, *handler);
                    }, [] { });
                    emitLabel(next.get());
                }
                SetForScope isArtificial(m_isArtificial, true);
                emitRuntimeCall(nullptr, "listAppend"_s, { results.get(), rest.get() }, node);
                Reg toRaise = g.newTemporary();
                emitRuntimeCall(toRaise.get(), "prepareReraiseStar"_s, { original, results.get() }, node);
                Ref<Label> done = g.newLabel();
                Reg isNone = g.newTemporary();
                g.emitIsUndefinedOrNull(isNone.get(), toRaise.get());
                g.emitJumpIfTrue(isNone.get(), done.get());
                // It goes on from here as what was caught would have, whether or not it is the same object.
                emitRuntimeCall(toRaise.get(), "asThrown"_s, { toRaise.get() }, node);
                g.emitThrow(toRaise.get());
                emitLabel(done.get());
            });
        }, [&] {
            emit(node.orElse);
        });
    }

    // with a as x, b as y: body is with a as x: with b as y: body
    void emitWith(With& node, size_t index)
    {
        if (node.isAsync && !checkIsAsync("'async with'"_s, node))
            return;
        if (index == node.items.size())
            return emit(node.body);

        WithItem& item = *node.items[index];
        Node& position = *item.contextExpression;
        Reg manager = emitToTemporary(item.contextExpression);
        // __exit__ is looked up before __enter__ is called.
        Reg exit = g.newTemporary();
        RegisterID* isAsync = constant(jsBoolean(node.isAsync));
        emitRuntimeCall(exit.get(), "loadExit"_s, { manager.get(), isAsync }, position);
        Reg entered = g.newTemporary();
        emitRuntimeCall(entered.get(), "callEnter"_s, { manager.get(), isAsync }, position);
        if (node.isAsync)
            emitAwaitValue(entered.get(), entered.get(), position, AwaitContext::AsyncEnter);

        Reg finishedNormally = g.emitLoad(g.newTemporary(), jsBoolean(true));
        WaysOut waysOut { { }, m_loopNesting, static_cast<unsigned>(m_handledExceptions.size()) };
        emitTryFinally([&] {
            emitTryCatch([&] {
                NestedBlock block(*this, position);
                JumpBlock with(*this, CodeDetails::JumpBlock::Kind::With, exit.get(), finishedNormally.get());
                if (item.optionalVariables)
                    emitAssign(item.optionalVariables, entered.get());
                m_waysOut.append(&waysOut);
                emitWith(node, index + 1);
                m_waysOut.removeLast();
            }, [&] (RegisterID* exception, RegisterID* thrown) {
                // __exit__ is told of the exception, and may say that it has been dealt with.
                g.emitLoad(finishedNormally.get(), jsBoolean(false));
                emitWhileHandling(thrown, position, [&] {
                    Reg suppress = g.newTemporary();
                    emitRuntimeCall(suppress.get(), "callExit"_s, { exit.get(), exception }, position);
                    if (node.isAsync)
                        emitAwaitValue(suppress.get(), suppress.get(), position, AwaitContext::AsyncExit);
                    Ref<Label> suppressed = g.newLabel();
                    emitJumpIfTrue(suppress.get(), suppressed.get());
                    emitLine(LineKind::OfHandledException);
                    g.emitThrow(thrown);
                    emitLabel(suppressed.get());
                });
            }, [] { });
        }, [&] (RegisterID* completionType, RegisterID* completionValue) {
            Ref<Label> done = g.newLabel();
            g.emitJumpIfFalse(finishedNormally.get(), done.get());
            JumpBlock withExit(*this, CodeDetails::JumpBlock::Kind::WithExit, exit.get(), finishedNormally.get(), completionType, !alwaysLeave(node.body));
            m_details->jumpBlocks.last().fourth = completionValue->virtualRegister();
            m_details->jumpBlocks.last().leaves = WTF::move(waysOut.leaves);
            markWhereItCanBeGoneOnFrom(position);
            Reg result = g.newTemporary();
            emitRuntimeCall(result.get(), "callExit"_s, { exit.get(), none() }, position);
            if (node.isAsync)
                emitAwaitValue(result.get(), result.get(), position, AwaitContext::AsyncExit);
            emitLabel(done.get());
        });
    }

    // ---- Classes

    // class C(bases, keywords): body is C = __build_class__(a function that runs the body, "C", bases, keywords)
    void emitClass(ClassDef& node)
    {
        emitDecorated(node.decorators, *node.name, node, [&] (RegisterID* dst) {
            if (node.typeParameters.empty())
                emitBuildClass(dst, node, nullptr);
            else
                emitCallTypeParameters(dst, OwnerKind::Class, *node.name, node.typeParameters, node);
        });
    }

    // `genericBase` is what a generic class is derived from besides what it says.
    void emitBuildClass(RegisterID* dst, ClassDef& node, RegisterID* genericBase)
    {
        Block* block = m_table.blockFor(&node);
        RELEASE_ASSERT(block);
        {
            // It is whatever the builtins have by that name, and is called as anything is.
            Reg function = g.newTemporary();
            emitRuntimeCall(function.get(), "loadBuildClass"_s, { m_builtins.get() }, node);
            auto info = makeInfo(CodeKind::Class, *node.name, nullptr, *block, node);
            info->parameterNames.append(Identifier::fromString(m_vm, ".namespace"_s));
            info->positionalCount = 1;
            info->positionalOnlyCount = 1;
            Reg body = g.newTemporary();
            emitNewFunction(body.get(), WTF::move(info), node);
            noteConstant({ CodeDetails::Constant::Kind::String, 10, false, 0, node.name->string() });

            checkKeywords(node.keywords);
            Reg bases = g.newTemporary();
            emitListWithStarred(bases.get(), node.bases, node);
            if (genericBase)
                emitRuntimeCall(nullptr, "listAppend"_s, { bases.get(), genericBase }, node);
            Reg keywords = g.newTemporary();
            if (node.keywords.empty())
                g.emitLoad(keywords.get(), jsUndefined());
            else {
                emitRuntimeCall(keywords.get(), "newDict"_s, { }, node);
                for (Keyword* keyword : node.keywords) {
                    Reg value = emit(keyword->value);
                    if (keyword->name)
                        emitRuntimeCall(nullptr, "addKeyword"_s, { keywords.get(), stringConstant(*keyword->name), value.get(), function.get() }, *keyword);
                    else
                        emitRuntimeCall(nullptr, "addKeywords"_s, { keywords.get(), value.get(), function.get() }, *keyword);
                }
            }
            Reg positional = g.newTemporary();
            {
                // One after the other, as the elements of a display are.
                Vector<Reg, 8> first { g.newTemporary(), g.newTemporary() };
                g.move(first[0].get(), body.get());
                g.move(first[1].get(), stringConstant(*node.name));
                emitNewList(positional.get(), first);
            }
            emitRuntimeCall(nullptr, "listExtend"_s, { positional.get(), bases.get(), marker() }, node);
            Reg helper = g.newTemporary();
            g.emitGetById(helper.get(), runtime(), Identifier::fromString(m_vm, "callSpread"_s));
            CallArguments call(g, nullptr, 3);
            g.emitLoad(call.thisRegister(), jsUndefined());
            g.move(call.argumentRegister(0), function.get());
            g.move(call.argumentRegister(1), positional.get());
            g.move(call.argumentRegister(2), keywords.get());
            emitTellOfCall(function.get(), positional.get(), ToldArgument::ListOfPositional);
            emitRawCall(dst, helper.get(), call, 3, node, Told::No);
        }
    }

    // The function that runs the body of a class statement. It is given the namespace to fill in. It leaves there the cells that __class__ and __classdict__ are,
    // for type() to put the class in once there is one, and returns the first of them, or None.
    void generateClassBody(ClassDef& node)
    {
        m_namespace = parameterRegister(0);
        allocateVariables(HasLocalVariables::No);
        emitLoadGlobals();
        emitEnter();

        Vector<const Identifier*, 8> cells;
        if (m_block.needsClassClosure)
            cells.append(&m_names.dunder_class);
        if (m_block.needsClassDict)
            cells.append(&m_names.dunder_classdict);
        if (m_block.hasConditionalAnnotations)
            cells.append(&m_names.dunder_conditional_annotations);
        emitPushCells(cells);
        // Its own, which has just been made, and not that of a class that this one is in.
        if (m_block.needsClassDict)
            emitStoreClosure(g.variable(m_names.dunder_classdict), m_names.dunder_classdict, m_namespace.get(), node);
        if (m_block.hasConditionalAnnotations) {
            Reg set = g.newTemporary();
            emitRuntimeCall(set.get(), "newSet"_s, { }, node);
            emitStoreClosure(m_names.dunder_conditional_annotations, set.get(), node);
        }
        Reg environment = g.newTemporary();
        if (m_block.needsClassClosure || m_block.needsClassDict)
            g.move(environment.get(), g.scopeRegister());
        else
            g.emitLoad(environment.get(), jsUndefined());

        // What a class is given without anybody writing it is given as if it had been written, so it goes where the body says that the name is from: `nonlocal __firstlineno__`.
        auto store = [&] (const Identifier& name, RegisterID* value) {
            Location location = locateAndNote(name);
            switch (location.where) {
            case Where::Closure:
            case Where::NamespaceOrClosure:
                emitStoreClosure(name, value, node);
                return;
            case Where::Global:
                g.emitDirectPutById(m_globals.get(), name, value);
                return;
            case Where::Register:
            case Where::Namespace:
                OpPySetItem::emit(&g, m_namespace.get(), stringConstant(name), value);
                return;
            }
        };
        {
            // What comes first is where the class says that it begins, which is at the first of its decorators. That may be no part of what this is compiled from, and what is from nowhere is there.
            emitLine(LineKind::Line);
            ++m_numberOfLines;
            m_lastMarkedLine = m_info.firstLine;
            SetForScope isArtificial(m_isArtificial, true);
            noteName(m_names.dunder_name);
            Reg moduleName = g.newTemporary();
            emitRuntimeCall(moduleName.get(), "loadName"_s, { m_namespace.get(), m_globals.get(), m_builtins.get(), stringConstant(m_names.dunder_name) }, node);
            store(m_names.dunder_module, moduleName.get());
        }
        store(m_names.dunder_qualname, constant(jsString(m_vm, m_info.qualifiedName)));
        store(m_names.dunder_firstlineno, constant(JSC::jsNumber(m_info.firstLine)));
        if (!node.typeParameters.empty()) {
            Reg typeParameters = emitLoadClosure(nullptr, Identifier::fromString(m_vm, ".type_params"_s), node);
            store(m_names.dunder_type_params, typeParameters.get());
        }
        if (m_block.hasDocstring && keepsDocstrings()) {
            noteConstant({ CodeDetails::Constant::Kind::String, 10, false, 0, docstringOf(node.body) });
            store(m_names.dunder_doc, constant(jsString(m_vm, docstringOf(node.body))));
        }

        emitSetUpAnnotations(node);
        collectDeferredAnnotations(node.body, OwnerKind::Class);
        emit(node.body);
        if (Reg annotate = emitAnnotateFunctionForBody(OwnerKind::Class, node))
            store(m_names.dunder_annotate_func, annotate.get());
        {
            Vector<String> attributes;
            for (const Identifier* name : m_block.staticAttributes)
                attributes.append(name->string());
            std::ranges::sort(attributes, [] (const String& a, const String& b) { return codePointCompareLessThan(a, b); });
            Vector<Reg, 8> items;
            for (auto& attribute : attributes) {
                items.append(g.newTemporary());
                g.move(items.last().get(), constant(jsString(m_vm, attribute)));
            }
            Reg tuple = g.newTemporary();
            emitNewTuple(tuple.get(), items);
            store(m_names.dunder_static_attributes, tuple.get());
        }
        // Nobody wrote this, and it is on no line.
        SetForScope isArtificial(m_isArtificial, true);
        Reg cell = g.newTemporary();
        if (m_block.needsClassDict) {
            emitRuntimeCall(cell.get(), "cellOfVariable"_s, { environment.get(), stringConstant(m_names.dunder_classdict) }, node);
            store(m_names.dunder_classdictcell, cell.get());
        }
        if (m_block.needsClassClosure) {
            emitRuntimeCall(cell.get(), "cellOfVariable"_s, { environment.get(), stringConstant(m_names.dunder_class) }, node);
            store(m_names.dunder_classcell, cell.get());
        } else
            g.emitLoad(cell.get(), jsUndefined());
        g.emitReturn(cell.get());
    }

    // ---- Annotations
    //
    // They are evaluated when they are asked for, by a function that is made for it: PEP 649. This is codegen_function_annotations(), codegen_annassign() and
    // codegen_process_deferred_annotations() of CPython's Python/codegen.c, and what those call.

    bool hasFutureAnnotations() const { return m_info.futureFeatures & FutureAnnotations; }

    // An annotation of a variable of a class or a module, and if the statement is one that may not be come to, which of those it is.
    struct DeferredAnnotation {
        AnnAssign* statement;
        int conditionalIndex; // -1 if it is always come to.
    };

    // Those of a body, in order. What has them and what evaluates them are compiled at different times, and both go by this.
    void collectDeferredAnnotations(Sequence<Statement*> body, bool isConditional, Vector<DeferredAnnotation>& result, int& nextIndex)
    {
        for (Statement* statement : body) {
            switch (statement->kind) {
            case Statement::Kind::AnnAssign: {
                auto& node = statement->as<AnnAssign>();
                if (node.isSimple && node.target->is<Name>())
                    result.append({ &node, isConditional ? nextIndex++ : -1 });
                break;
            }
            case Statement::Kind::For:
                collectDeferredAnnotations(statement->as<For>().body, true, result, nextIndex);
                collectDeferredAnnotations(statement->as<For>().orElse, true, result, nextIndex);
                break;
            case Statement::Kind::While:
                collectDeferredAnnotations(statement->as<While>().body, true, result, nextIndex);
                collectDeferredAnnotations(statement->as<While>().orElse, true, result, nextIndex);
                break;
            case Statement::Kind::If:
                collectDeferredAnnotations(statement->as<If>().body, true, result, nextIndex);
                collectDeferredAnnotations(statement->as<If>().orElse, true, result, nextIndex);
                break;
            case Statement::Kind::With:
                collectDeferredAnnotations(statement->as<With>().body, true, result, nextIndex);
                break;
            case Statement::Kind::Match:
                for (MatchCase* matchCase : statement->as<Match>().cases)
                    collectDeferredAnnotations(matchCase->body, true, result, nextIndex);
                break;
            case Statement::Kind::Try: {
                auto& node = statement->as<Try>();
                collectDeferredAnnotations(node.body, true, result, nextIndex);
                for (ExceptHandler* handler : node.handlers)
                    collectDeferredAnnotations(handler->body, true, result, nextIndex);
                collectDeferredAnnotations(node.orElse, true, result, nextIndex);
                collectDeferredAnnotations(node.finalBody, true, result, nextIndex);
                break;
            }
            default:
                break;
            }
        }
    }

    Vector<DeferredAnnotation> deferredAnnotationsOf(Sequence<Statement*> body, bool isModule)
    {
        Vector<DeferredAnnotation> result;
        int nextIndex = 0;
        // A module may have run only in part, so all of its count as ones that may not be come to.
        collectDeferredAnnotations(body, isModule, result, nextIndex);
        return result;
    }

    void emitNewAnnotateFunction(RegisterID* dst, Block& block, OwnerKind owner, const Node& node)
    {
        auto info = makeInfo(CodeKind::Annotations, m_names.dunder_annotate, nullptr, block, node);
        info->owner = owner;
        // To the symbol table it is `.format`, so that an annotation can name the built-in function. It is `format` to whoever asks.
        info->parameterNames.append(Identifier::fromString(m_vm, "format"_s));
        info->positionalOnlyCount = 1;
        info->positionalCount = 1;
        emitNewFunction(dst, WTF::move(info), node);
    }

    // To be done before the body of a class or a module is gone through.
    void collectDeferredAnnotations(Sequence<Statement*> body, OwnerKind owner)
    {
        if (!hasFutureAnnotations() && m_block.annotationBlock)
            m_deferredAnnotations = deferredAnnotationsOf(body, owner != OwnerKind::Class);
    }

    // The __annotate__ of a class or a module. Null if there is nothing for it to evaluate.
    Reg emitAnnotateFunctionForBody(OwnerKind owner, const Node& node)
    {
        if (m_deferredAnnotations.isEmpty())
            return nullptr;
        Reg annotate = g.newTemporary();
        emitNewAnnotateFunction(annotate.get(), *m_block.annotationBlock, owner, node);
        return annotate;
    }

    // With `from __future__ import annotations` they are strings, and are put in __annotations__ as they are come to.
    void emitSetUpAnnotations(const Node& node)
    {
        if (hasFutureAnnotations() && m_block.usesAnnotations)
            emitRuntimeCall(nullptr, "setUpAnnotations"_s, { m_info.usesNamespace ? m_namespace.get() : m_globals.get(), constant(jsBoolean(m_info.usesNamespace)) }, node);
    }

    enum class AnnotationOf : uint8_t { Parameter, Variable };

    RegisterID* emitAnnotation(RegisterID* dst, Expression& annotation, AnnotationOf what)
    {
        if (hasFutureAnnotations()) {
            SyntaxError whyNot;
            String text = unparse(annotation, whyNot);
            if (text.isNull())
                fail(whyNot.kind, WTF::move(whyNot.message));
            return g.move(dst, constant(jsString(m_vm, text)));
        }
        // *args: *Ts, which is [value] = [*Ts].
        if (auto* starred = what == AnnotationOf::Parameter ? annotation.tryAs<Starred>() : nullptr) {
            Reg iterable = emitToTemporary(starred->value);
            mark(annotation);
            OpPyUnpackSequence::emit(&g, dst->virtualRegister(), 1, 1, iterable.get());
            return dst;
        }
        emitInto(dst, &annotation);
        return dst;
    }

    void emitEvaluateAndDiscard(Expression* expression)
    {
        if (expression)
            emitToTemporary(expression);
    }

    // x: T = value
    void emitAnnotatedAssignment(AnnAssign& node)
    {
        bool isInClassOrModule = !isFunctionLike();
        if (node.value) {
            Reg value = emitToTemporary(node.value);
            emitAssign(node.target, value.get());
        }
        switch (node.target->kind) {
        case Expression::Kind::Name: {
            if (!node.isSimple || !isInClassOrModule)
                break;
            const Identifier& name = mangle(*node.target->as<Name>().id);
            if (hasFutureAnnotations()) {
                Reg annotation = g.newTemporary();
                emitAnnotation(annotation.get(), *node.annotation, AnnotationOf::Variable);
                Reg annotations = emitLoadName(nullptr, m_names.dunder_annotations, node);
                mark(node);
                OpPySetItem::emit(&g, annotations.get(), stringConstant(name), annotation.get());
                break;
            }
            // That it has been come to is noted, for what evaluates the annotations.
            for (auto& deferred : m_deferredAnnotations) {
                if (deferred.statement != &node || deferred.conditionalIndex < 0)
                    continue;
                Reg set = m_info.kind == CodeKind::Class ? Reg(emitLoadClosure(nullptr, m_names.dunder_conditional_annotations, node)) : Reg(emitLoadName(nullptr, m_names.dunder_conditional_annotations, node));
                emitRuntimeCall(nullptr, "setAdd"_s, { set.get(), constant(jsNumber(deferred.conditionalIndex)) }, node);
            }
            break;
        }
        case Expression::Kind::Attribute:
            if (!node.value)
                emitEvaluateAndDiscard(node.target->as<Attribute>().value);
            break;
        case Expression::Kind::Subscript:
            if (!node.value) {
                emitEvaluateAndDiscard(node.target->as<Subscript>().value);
                emitEvaluateAndDiscard(node.target->as<Subscript>().slice);
            }
            break;
        default:
            fail(SyntaxError::Kind::SystemError, concatenate("invalid node type ("_s, static_cast<unsigned>(node.target->kind) + 1, ") for annotated assignment"_s));
            return;
        }
    }

    // def __annotate__(format, /)
    void generateAnnotations(void* root)
    {
        generateFunction([&] {
            const Node& node = m_block.location;
            emitRuntimeCall(nullptr, "checkAnnotationFormat"_s, { parameterRegister(0) }, node);
            // If they are strings there is nothing to look up, and the class has not kept its namespace for it.
            if (!hasFutureAnnotations())
                emitLoadClassNamespace(node);
            Reg annotations = g.newTemporary();
            emitRuntimeCall(annotations.get(), "newDict"_s, { }, node);
            auto add = [&] (const Identifier& name, Expression& annotation, AnnotationOf what) {
                Reg value = g.newTemporary();
                emitAnnotation(value.get(), annotation, what);
                OpPySetItem::emit(&g, annotations.get(), stringConstant(name), value.get());
            };

            if (m_info.owner == OwnerKind::Function) {
                auto& function = *static_cast<FunctionDef*>(root);
                Arguments& arguments = *function.arguments;
                auto addArgument = [&] (Argument* argument) {
                    if (argument && argument->annotation)
                        add(mangle(*argument->name), *argument->annotation, AnnotationOf::Parameter);
                };
                for (Argument* argument : arguments.positional)
                    addArgument(argument);
                for (Argument* argument : arguments.positionalOnly)
                    addArgument(argument);
                addArgument(arguments.variadic);
                for (Argument* argument : arguments.keywordOnly)
                    addArgument(argument);
                addArgument(arguments.keywordVariadic);
                if (function.returns)
                    add(Identifier::fromString(m_vm, "return"_s), *function.returns, AnnotationOf::Parameter);
            } else {
                bool isClass = m_info.owner == OwnerKind::Class;
                Sequence<Statement*> body = isClass ? static_cast<ClassDef*>(root)->body : static_cast<Module*>(root)->body;
                for (auto& deferred : deferredAnnotationsOf(body, !isClass)) {
                    Ref<Label> notComeTo = g.newLabel();
                    if (deferred.conditionalIndex >= 0) {
                        Reg set = isClass ? Reg(emitLoadClosure(nullptr, m_names.dunder_conditional_annotations, *deferred.statement)) : Reg(emitLoadName(nullptr, m_names.dunder_conditional_annotations, *deferred.statement));
                        Reg isIn = g.newTemporary();
                        mark(*deferred.statement);
                        emitCompare(isIn.get(), ComparisonOperator::In, constant(jsNumber(deferred.conditionalIndex)), set.get());
                        emitJumpIfFalse(isIn.get(), notComeTo.get());
                    }
                    add(mangle(*deferred.statement->target->as<Name>().id), *deferred.statement->annotation, AnnotationOf::Variable);
                    emitLabel(notComeTo.get());
                }
            }
            g.emitReturn(annotations.get());
        });
    }

    // ---- Type parameters, and the `type` statement
    //
    // def f[T](): ..., class C[T]: ... and type A[T] = ... have their type parameters for the variables of a function that is made for it and called at once. It
    // makes them, and then the function, the class or the alias, which it returns. What follows the colon or the equals sign of one, and what an alias is an alias
    // of, are worked out when they are asked for, each by a function of its own: PEP 695 and 696. This is codegen_type_params(), codegen_typealias() and parts of
    // codegen_function() and codegen_class() of CPython's Python/codegen.c.

    // What is in a class without being part of its body looks in the class for what is not its own. The class keeps its namespace for it.
    void emitLoadClassNamespace(const Node& node)
    {
        if (!m_info.canSeeClassScope)
            return;
        m_namespace = g.newTemporary();
        emitLoadClosure(m_namespace.get(), m_names.dunder_classdict, node);
    }

    void emitCallTypeParameters(RegisterID* dst, OwnerKind owner, const Identifier& name, Sequence<TypeParameter*> typeParameters, const Node& node, RegisterID* defaults = nullptr, RegisterID* keywordDefaults = nullptr)
    {
        Block* block = m_table.blockFor(typeParameters.data());
        RELEASE_ASSERT(block);
        auto info = makeInfo(CodeKind::TypeParameters, Identifier::fromString(m_vm, concatenate("<generic parameters of "_s, name.string(), '>')), nullptr, *block, node);
        info->owner = owner;
        info->definesWhatIsSaidToBeGlobal = m_block.scopeOf(mangle(name)) == NameScope::GlobalExplicit;
        if (owner == OwnerKind::Class)
            info->privateName = name;
        // The defaults of a function's parameters are evaluated where the statement is, and passed in.
        Vector<RegisterID*, 2> arguments;
        if (defaults) {
            info->parameterNames.append(Identifier::fromString(m_vm, ".defaults"_s));
            arguments.append(defaults);
        }
        if (keywordDefaults) {
            info->parameterNames.append(Identifier::fromString(m_vm, ".kwdefaults"_s));
            arguments.append(keywordDefaults);
        }
        info->positionalCount = arguments.size();
        info->positionalOnlyCount = arguments.size();

        Reg function = g.newTemporary();
        emitNewFunction(function.get(), WTF::move(info), node);
        CallArguments call(g, nullptr, arguments.size());
        g.emitLoad(call.thisRegister(), jsUndefined());
        for (unsigned i = 0; i < arguments.size(); ++i)
            g.move(call.argumentRegister(i), arguments[i]);
        emitRawCall(dst, function.get(), call, arguments.size(), node);
    }

    // def name(format=1, /): return expression
    void emitEvaluator(RegisterID* dst, const void* blockKey, const Identifier& name, OwnerKind owner, Evaluates evaluates, unsigned typeParameterIndex, const Node& ownerNode)
    {
        Block* block = m_table.blockFor(blockKey);
        RELEASE_ASSERT(block);
        auto info = makeInfo(CodeKind::Evaluator, name, nullptr, *block, ownerNode);
        info->owner = owner;
        info->evaluates = evaluates;
        info->typeParameterIndex = typeParameterIndex;
        info->parameterNames.append(Identifier::fromString(m_vm, ".format"_s));
        info->positionalOnlyCount = 1;
        info->positionalCount = 1;
        emitNewFunction(dst, WTF::move(info), ownerNode);
        Vector<Reg, 8> one { g.newTemporary() };
        g.emitLoad(one[0].get(), jsNumber(1));
        Reg defaults = g.newTemporary();
        emitNewTuple(defaults.get(), one);
        g.emitDirectPutById(dst, m_names.private_defaults, defaults.get());
    }

    // Makes each and gives it its name. The result is a tuple of them.
    Reg emitTypeParameters(Sequence<TypeParameter*> parameters, const Node& ownerNode)
    {
        Vector<Reg, 8> values;
        bool hasSeenDefault = false;
        for (unsigned i = 0; i < parameters.size(); ++i) {
            TypeParameter& parameter = *parameters[i];
            Reg value = g.newTemporary();
            const void* defaultKey = &parameter;
            switch (parameter.kind) {
            case TypeParameter::Kind::TypeVar: {
                Reg evaluator = g.newTemporary();
                if (parameter.bound)
                    emitEvaluator(evaluator.get(), &parameter, *parameter.name, m_info.owner, Evaluates::Bound, i, ownerNode);
                else
                    g.emitLoad(evaluator.get(), jsUndefined());
                emitRuntimeCall(value.get(), "newTypeVar"_s, { stringConstant(*parameter.name), evaluator.get(), constant(jsBoolean(parameter.bound && parameter.bound->is<Tuple>())) }, parameter);
                defaultKey = reinterpret_cast<const char*>(&parameter) + 1;
                break;
            }
            case TypeParameter::Kind::TypeVarTuple:
                emitRuntimeCall(value.get(), "newTypeVarTuple"_s, { stringConstant(*parameter.name) }, parameter);
                break;
            case TypeParameter::Kind::ParamSpec:
                emitRuntimeCall(value.get(), "newParamSpec"_s, { stringConstant(*parameter.name) }, parameter);
                break;
            }
            if (parameter.defaultValue) {
                hasSeenDefault = true;
                Reg evaluator = g.newTemporary();
                emitEvaluator(evaluator.get(), defaultKey, *parameter.name, m_info.owner, Evaluates::Default, i, ownerNode);
                emitRuntimeCall(nullptr, "setTypeParameterDefault"_s, { value.get(), evaluator.get() }, parameter);
            } else if (hasSeenDefault)
                fail(concatenate("non-default type parameter '"_s, parameter.name->string(), "' follows default type parameter"_s), parameter);
            emitStoreName(*parameter.name, value.get(), parameter);
            values.append(value);
        }
        Reg tuple = g.newTemporary();
        emitNewTuple(tuple.get(), values);
        return tuple;
    }

    void emitNewTypeAlias(RegisterID* dst, TypeAlias& node, RegisterID* typeParameters)
    {
        const Identifier& name = *node.name->as<Name>().id;
        Reg evaluator = g.newTemporary();
        emitEvaluator(evaluator.get(), &node, name, OwnerKind::TypeAlias, Evaluates::Value, 0, node);
        emitRuntimeCall(dst, "newTypeAlias"_s, { stringConstant(name), typeParameters ? typeParameters : none(), evaluator.get() }, node);
    }

    void generateTypeParameters(Statement& statement)
    {
        generateFunction([&] {
            emitLoadClassNamespace(statement);
            Reg result = g.newTemporary();
            switch (m_info.owner) {
            case OwnerKind::Function: {
                auto& node = statement.as<FunctionDef>();
                Reg typeParameters = emitTypeParameters(node.typeParameters, node);
                RegisterID* defaults = nullptr;
                RegisterID* keywordDefaults = nullptr;
                for (unsigned i = 0; i < m_info.parameterNames.size(); ++i)
                    (m_info.parameterNames[i] == ".defaults"_s ? defaults : keywordDefaults) = parameterRegister(i);
                emitFunctionWithDefaults(result.get(), CodeKind::Function, *node.name, node.arguments, node, &node, defaults, keywordDefaults);
                g.emitDirectPutById(result.get(), m_names.private_typeParams, typeParameters.get());
                break;
            }
            case OwnerKind::Class: {
                auto& node = statement.as<ClassDef>();
                Reg typeParameters = emitTypeParameters(node.typeParameters, node);
                // The body wants them, for __type_params__.
                emitStoreName(Identifier::fromString(m_vm, ".type_params"_s), typeParameters.get(), node);
                Reg genericBase = g.newTemporary();
                emitRuntimeCall(genericBase.get(), "subscriptGeneric"_s, { typeParameters.get() }, node);
                emitBuildClass(result.get(), node, genericBase.get());
                break;
            }
            case OwnerKind::TypeAlias: {
                auto& node = statement.as<TypeAlias>();
                Reg typeParameters = emitTypeParameters(node.typeParameters, node);
                emitNewTypeAlias(result.get(), node, typeParameters.get());
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            g.emitReturn(result.get());
        });
    }

    void generateEvaluator(Statement& statement)
    {
        generateFunction([&] {
            emitRuntimeCall(nullptr, "checkAnnotationFormat"_s, { parameterRegister(0) }, statement);
            emitLoadClassNamespace(statement);
            Expression* expression = nullptr;
            bool canBeStarred = false;
            if (m_info.evaluates == Evaluates::Value)
                expression = statement.as<TypeAlias>().value;
            else {
                auto parameters = m_info.owner == OwnerKind::Function ? statement.as<FunctionDef>().typeParameters : m_info.owner == OwnerKind::Class ? statement.as<ClassDef>().typeParameters : statement.as<TypeAlias>().typeParameters;
                TypeParameter& parameter = *parameters[m_info.typeParameterIndex];
                expression = m_info.evaluates == Evaluates::Bound ? parameter.bound : parameter.defaultValue;
                // *Ts = *tuple[int, str]
                canBeStarred = parameter.kind == TypeParameter::Kind::TypeVarTuple;
            }
            Reg value = g.newTemporary();
            if (auto* starred = expression->tryAs<Starred>(); starred && canBeStarred) {
                Reg iterable = emitToTemporary(starred->value);
                mark(*expression);
                OpPyUnpackSequence::emit(&g, value->virtualRegister(), 1, 1, iterable.get());
            } else
                emitInto(value.get(), expression);
            g.emitReturn(value.get());
        });
    }

    // ---- Imports

    // What __import__() is given for the locals: the namespace, where names are kept in one, and None in a function.
    RegisterID* localsForImport()
    {
        if (m_info.usesNamespace)
            return m_namespace.get();
        return isFunctionKind(m_info.kind) ? none() : m_globals.get();
    }

    void emitImportName(RegisterID* dst, RegisterID* name, RegisterID* fromList, unsigned level, Node& node)
    {
        emitRuntimeCall(dst, "importName"_s, { m_globals.get(), m_builtins.get(), localsForImport(), name, fromList, constant(JSC::jsNumber(level)) }, node);
    }

    void emitImport(Import& node)
    {
        for (Alias* alias : node.names) {
            // import a.b.c gives a.
            Reg module = g.newTemporary();
            noteName(*alias->name); // IMPORT_NAME
            emitImportName(module.get(), stringConstant(*alias->name), none(), 0, node);
            StringView name = alias->name->string();
            size_t dot = name.find('.');
            if (!alias->asName) {
                emitStoreName(dot == notFound ? *alias->name : Identifier::fromString(m_vm, name.left(dot).toString()), module.get(), node);
                continue;
            }
            // import a.b.c as d gives c, which is got from b, which is got from a.
            while (dot != notFound) {
                size_t start = dot + 1;
                dot = name.find('.', start);
                Identifier attribute = Identifier::fromString(m_vm, name.substring(start, dot == notFound ? name.length() - start : dot - start).toString());
                noteName(attribute); // IMPORT_FROM
                emitRuntimeCall(module.get(), "importFrom"_s, { module.get(), stringConstant(attribute) }, node);
            }
            emitStoreName(*alias->asName, module.get(), node);
        }
    }

    // `from __future__ import x` has been seen to by now, and is an import like any other besides.
    void emitImportFrom(ImportFrom& node)
    {
        Vector<Reg, 8> names;
        for (Alias* alias : node.names) {
            names.append(g.newTemporary());
            g.emitLoad(names.last().get(), *alias->name);
        }
        Reg fromList = g.newTemporary();
        emitNewTuple(fromList.get(), names);
        names.clear();

        Reg module = g.newTemporary();
        Reg moduleName = node.module ? Reg(stringConstant(*node.module)) : Reg(stringConstant(m_vm.propertyNames->emptyIdentifier));
        noteName(node.module ? *node.module : m_vm.propertyNames->emptyIdentifier); // IMPORT_NAME
        emitImportName(module.get(), moduleName.get(), fromList.get(), node.level, node);
        for (Alias* alias : node.names) {
            if (*alias->name == "*"_s) {
                emitRuntimeCall(nullptr, "importStar"_s, { module.get(), m_globals.get(), localsForImport() }, node);
                continue;
            }
            Reg value = g.newTemporary();
            noteName(*alias->name); // IMPORT_FROM
            emitRuntimeCall(value.get(), "importFrom"_s, { module.get(), stringConstant(*alias->name) }, node);
            emitStoreName(alias->asName ? *alias->asName : *alias->name, value.get(), node);
        }
    }

    // ---- Modules

    void generateModule(Module& module)
    {
        // With an `await` in it that is in no function, which compile() can be told to allow, running it makes a coroutine.
        if (m_info.isCoroutine) {
            m_namespaceIsLoaded = m_info.usesNamespace;
            generateFunction([&] {
                if (m_info.usesNamespace) {
                    emitLoadClosure(m_namespace.get(), m_info.parameterNames[0], m_block.location);
                    m_details->namespaceRegister = m_namespace->virtualRegister();
                }
                emitModuleBody(module);
            });
            return;
        }
        if (m_info.usesNamespace)
            m_namespace = parameterRegister(0);
        allocateVariables(HasLocalVariables::No);
        emitLoadGlobals();
        emitEnter();
        emitModuleBody(module);
    }

    void emitModuleBody(Module& module)
    {
        if (module.kind == Module::Kind::Expression) {
            Reg value = emit(module.expression);
            g.emitReturn(value.get());
            return;
        }
        // All of the source, which is what the annotations of a module are compiled from.
        Node whole = m_block.location;
        whole.line = 1;
        whole.start = 0;
        whole.end = g.m_scopeNode->source().provider()->source().length();
        if (m_block.hasConditionalAnnotations) {
            Reg set = g.newTemporary();
            emitRuntimeCall(set.get(), "newSet"_s, { }, whole);
            emitStoreName(m_names.dunder_conditional_annotations, set.get(), whole);
        }
        emitSetUpAnnotations(whole);
        // It is there from the start, and says of each annotation whether the statement that it is in has been come to.
        OwnerKind kind = module.kind == Module::Kind::Interactive ? OwnerKind::Interactive : OwnerKind::Module;
        collectDeferredAnnotations(module.body, kind);
        if (Reg annotate = emitAnnotateFunctionForBody(kind, whole))
            emitStoreName(m_names.dunder_annotate, annotate.get(), whole);
        if (m_block.hasDocstring && keepsDocstrings()) {
            noteConstant({ CodeDetails::Constant::Kind::String, 10, false, 0, docstringOf(module.body) });
            emitStoreName(m_names.dunder_doc, constant(jsString(m_vm, docstringOf(module.body))), *module.body[0]);
        }
        emit(module.body);
        m_endIsNeverComeTo = alwaysLeave(module.body);
        // With nothing in it, it is on the line before its first from beginning to end, and that is a line like another.
        if (!m_numberOfLines && module.body.empty()) {
            emitLine(LineKind::Line);
            ++m_numberOfLines;
        }
        if (m_functionGeneratedLast)
            noteConstant({ CodeDetails::Constant::Kind::Code, 10, false, *m_functionGeneratedLast });
        emitReturnAtEnd();
    }

    using ComprehensionScope = HashMap<UniquedStringImpl*, Reg>; // Null for a variable of an environment.

    BytecodeGenerator& g;
    VM& m_vm;
    std::unique_ptr<CodeDetails> m_details;
    HashSet<UniquedStringImpl*> m_notedNames;
    HashSet<UniquedStringImpl*> m_notedVariableNames;
    bool m_endIsNeverComeTo { false };
    HashMap<Statement*, bool> m_alwaysLeaves;
    // For each `try` with a `finally`, and each `with`, of which what comes first is being generated. See CodeDetails::JumpBlock::leaves.
    struct WaysOut {
        Vector<CodeDetails::JumpBlock::Leave> leaves;
        unsigned loopNesting;
        unsigned handlerNesting;
        bool isTheEnd { false }; // What comes after `finally` never comes to its end.
    };
    Vector<WaysOut*, 4> m_waysOut;
    unsigned m_loopNesting { 0 };
    std::optional<unsigned> m_lineOfStatement; // Where the op_py_line is that a statement began with, until it is known what line that is.
    unsigned m_jumpBlock { 0 }; // Which of m_details->jumpBlocks is being generated, counting from 1.
    NotedConstants m_notedConstants; // Of m_details->constants.
    Vector<bool> m_constantIsLoaded;
    Vector<CodeDetails::Constant> m_workedOutConstants;
    Vector<bool> m_workedOutConstantIsLoaded;
    NotedConstants m_notedWorkedOutConstants;
    HashMap<uint64_t, PyCodeConstant*> m_codeConstants; // By whether it was worked out, and which it is of those that were or were not.
    HashMap<Expression*, std::unique_ptr<CodeDetails::Constant>> m_constantOfExpression;
    unsigned m_nestedBlocks { 0 };
    bool m_isInExceptStar { false };
    bool m_isInExceptStarOutsideLoop { false }; // And not in a loop that is itself in the block.
    bool m_isNeverComeTo { false }; // In `if 0:` or the like.
    unsigned m_lastMarkedLine { 0 };
    bool m_isInConstantDisplay { false };
    unsigned m_numberOfLines { 0 }; // How many times op_py_line has been emitted.
    unsigned m_comprehensionElementLine { 0 };
    Node m_lastMarked;
    const Node* m_tupleThatIsUnpacked { nullptr };
    const Node* m_whereItBegins { nullptr }; // The `def` statement, in a function.
    bool m_isArtificial { true }; // In what has to be done that nobody wrote, which is on no line. So it is until what was written begins.
    unsigned m_decoratorLine { 0 }; // Of the first decorator of the definition that is being made, if it has any.
    CommonNames& m_names;
    Arena& m_arena;
    SymbolTable& m_table;
    Block& m_block;
    const FunctionInfo& m_info;
    SyntaxError& m_error;
    unsigned& m_functionsBeforeError;
    unsigned m_numberOfFunctions { 0 };
    std::optional<unsigned> m_functionGeneratedLast;
    const Identifier* m_private;
    Vector<DeferredAnnotation> m_deferredAnnotations; // Those of the class or the module that this is the code of.

    HashMap<UniquedStringImpl*, RegisterID*> m_locals;
    HashSet<UniquedStringImpl*> m_alwaysBound;
    HashSet<UniquedStringImpl*> m_deletedNames;
    Vector<ComprehensionScope, 2> m_comprehensionScopes;
    HashMap<std::pair<Block*, UniquedStringImpl*>, RegisterID*> m_comprehensionLocals;
    Vector<Block*, 2> m_comprehensionBlocks; // What each of those is the variables of.
    Vector<std::unique_ptr<VariableEnvironment>> m_environments;
    Vector<RegisterID*, 4> m_handledExceptions;
    bool m_isAfterFinally { false }; // In what comes after `finally`, and in no handler that is in that.
    Reg m_globals;
    Reg m_builtins;
    Reg m_namespace;
    bool m_namespaceIsLoaded { false }; // It is not what the code was called with, but is got from where that was put: the code of a module that is a coroutine.
};

// ---- ScopeNode

ScopeNode::ScopeNode(ParserArena& parserArena, const SourceCode& source, Arena& arena, SymbolTable& symbolTable, Block& block, const FunctionInfo& info, void* root)
    : JSC::ScopeNode(parserArena, JSTokenLocation(), JSTokenLocation(), source, nullptr, VariableEnvironment(), FunctionStack(), VariableEnvironment(), NoFeatures, StrictModeLexicallyScopedFeature, NoInnerArrowFunctionFeatures, 0)
    , m_arena(arena)
    , m_symbolTable(symbolTable)
    , m_block(block)
    , m_info(info)
    , m_root(root)
{
}

unsigned ScopeNode::parameterCount() const
{
    if (m_info.isGeneratorBody)
        return static_cast<unsigned>(JSGenerator::Argument::NumberOfArguments);
    return m_info.parameterCount();
}

void ScopeNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    CodeGenerator(generator, m_arena, m_symbolTable, m_block, m_info, m_error, m_functionsBeforeError).generate(m_root);
}

} } // namespace JSC::Python
