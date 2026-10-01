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
#include "PythonParser.h"

#include "PythonLexer.h"
#include "PythonText.h"
#include "PythonUnicodeType.h"
#include "VM.h"
#include <wtf/Scope.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

namespace {

// Recursive descent over the tokens. A function here is a rule of Grammar/python.gram in CPython, or a few of them, and gives null
// if what is there is not what it is for. It goes back and tries something else only where the grammar cannot be told apart by
// looking a token or two ahead: `with (`, and the words that are keywords only sometimes.
class Parser {
public:
    // `error` is the scanner's, if it has one.
    Parser(VM& vm, Arena& arena, const TokenBuffer& tokens, SyntaxError& error)
        : m_vm(vm)
        , m_arena(arena)
        , m_tokens(tokens)
        , m_error(error)
        , m_scannerError(std::exchange(error, { }))
    {
    }

    // Of tokens that are made as they are asked for.
    Parser(VM& vm, Arena& arena, TypedTokens& supply, SyntaxError& error)
        : m_vm(vm)
        , m_arena(arena)
        , m_tokens(supply.tokens())
        , m_supply(&supply)
        , m_error(error)
    {
    }

    // There was nothing there to parse, and there will be no more: _PyPegen_interactive_exit()
    bool isAtEndOfInput() const { return m_isAtEndOfInput; }

    // _PyPegen_run_parser(). If the source will not parse it is gone through again, and this time what is in the grammar only to be recognized as a mistake is tried as well, so that
    // there is something to say about it. Whichever of those is come to first is what is said. If none is, all that there is to say is how far the parser got the first time.
    Module* parseModule(Module::Kind kind)
    {
        m_lastLineIsEnded = kind == Module::Kind::Module;
        if (Module* module = parseModuleOnce(kind))
            return module;
        bool saysWhereLastStatementIs = false;
        auto sayWhereLastStatementIs = makeScopeExit([&] {
            if (!m_error || !saysWhereLastStatementIs)
                return;
            m_error.hasLastStatement = true;
            m_error.lastStatementLine = m_lastStatementLine;
            m_error.lastStatementColumn = m_lastStatementColumn;
            m_error.sourceIsGivenAnEnd = kind == Module::Kind::Module;
        });
        // It did parse, and there is no more to be said.
        if (m_isBadSingleStatement || m_isAtEndOfInput)
            return nullptr;
        unsigned lastToken = m_furthest;
        if (m_arena.allowsIncompleteInput && tokenizerIsAtEndOfSource()) {
            m_error = { };
            SetForScope scope(m_failsInEitherPass, true);
            failAtLastToken("incomplete input"_s, SyntaxError::Kind::IncompleteInputError);
            m_error.lineGoesOnToTheEnd = m_tokens[m_furthest].kind == TokenKind::Error && m_scannerError.isFromTokenizer;
            return nullptr;
        }
        saysWhereLastStatementIs = true;
        if (!m_error) {
            // reset_parser_state_for_error_pass()
            if (m_supply)
                m_supply->stopReading();
            m_lastStatementLine = 0;
            m_lastStatementColumn = 0;
            m_index = 0;
            m_isSecondPass = true;
            m_callsInvalidRules = true;
            m_keepsWhatIsParsed = true;
            m_memos = { };
            parseModuleOnce(kind);
        }

        // _Pypegen_set_syntax_error()
        if (m_error && !m_hasScannerError && m_scannerError && m_tokens[m_furthest].kind == TokenKind::Error)
            return nullptr;
        if (m_hasScannerError)
            return nullptr;
        bool hasSomethingToSay = !!m_error;
        if (!hasSomethingToSay) {
            failGenerically(m_tokens[lastToken]);
            if (m_error.kind == SyntaxError::Kind::IndentationError)
                return nullptr;
        }
        // _PyPegen_tokenize_full_source_to_check_for_errors(): what is wrong further on may be the reason. There is no further on to what is typed.
        if (!m_scannerError || m_arena.isTypedAtPrompt)
            return nullptr;
        // If there is something to say already, that is only done while the tokenizer has not come to the end of the source.
        if (hasSomethingToSay && tokenizerIsAtEndOfSource())
            return nullptr;
        if (m_scannerError.isFromTokenizer) {
            if (!m_scannerError.isInsideFString)
                m_error = m_scannerError;
            return nullptr;
        }
        // A bracket that was never closed is, if it was opened before the line that the parser got to.
        if (m_scannerError.openBracket && m_tokens[m_furthest].line > m_scannerError.openBracketLine) {
            unsigned tokenizerLine = m_scannerError.tokenizerLine;
            m_error = { SyntaxError::Kind::SyntaxError, true, concatenate('\'', m_scannerError.openBracket, "' was never closed"_s), m_scannerError.openBracketLine, static_cast<int>(m_scannerError.openBracketColumn), m_scannerError.openBracketLine, -1 };
            m_error.tokenizerLine = tokenizerLine;
            m_error.lastLineIsEnded = m_lastLineIsEnded;
        }
        return nullptr;
    }

    Statement* parseDefinitionAlone() { return at(SoftKeyword::Type) ? parseTypeAlias() : parseDefinition(); }
    Expression* parseExpressionAlone() { return parseExpression(); }

private:
    // Whether CPython's tokenizer, which is asked for one token at a time, would have come to the end of the source.
    bool tokenizerIsAtEndOfSource()
    {
        const Token& token = m_tokens[m_furthest];
        if (token.kind == TokenKind::Error)
            return m_scannerError.isAtEndOfSource;
        if (token.kind == TokenKind::EndMarker || token.isMadeOfTheEnd)
            return true;
        if (token.end != m_tokens.last().start)
            return false;
        // What says that something has ended, where the source has.
        if (token.kind == TokenKind::Dedent)
            return true;
        // It has if it has made a token of the last of the source: it looks at what comes next to see where most tokens end. That is the end of a line, if the source is taken to end with one.
        return !m_lastLineIsEnded && token.kind != TokenKind::String && token.kind != TokenKind::Newline;
    }

    // In the second pass the same thing is parsed over and over, as one thing and another is tried. As in CPython, what came of it is kept, by the token that it began at.
    struct Memo {
        Expression* result;
        unsigned end;
    };
    using MemoTable = UncheckedKeyHashMap<unsigned, Memo, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>>;

    Module* parseModuleOnce(Module::Kind kind)
    {
        auto* module = m_arena.create<Module>();
        module->kind = kind;
        bool ok = false;
        switch (kind) {
        case Module::Kind::Module: {
            Vector<Statement*, 16> body;
            ok = parseStatementsUntil(TokenKind::EndMarker, body);
            module->body = m_arena.copy(body);
            break;
        }
        case Module::Kind::Interactive: {
            // statement_newline
            Vector<Statement*, 16> body;
            if (at(TokenKind::Newline))
                body.append(make<Pass>(next()));
            else if (m_supply && at(TokenKind::EndMarker)) {
                m_isAtEndOfInput = true;
                return nullptr;
            } else if (!parseStatement(body))
                break;
            else {
                switch (body.last()->kind) {
                case Statement::Kind::FunctionDef:
                case Statement::Kind::ClassDef:
                case Statement::Kind::If:
                case Statement::Kind::With:
                case Statement::Kind::For:
                case Statement::Kind::Try:
                case Statement::Kind::While:
                case Statement::Kind::Match:
                    // What has blocks is followed by the end of a line. It has none of its own: this is the one that is made of the end of the source.
                    if (!consume(TokenKind::Newline))
                        return nullptr;
                    break;
                default:
                    break;
                }
            }
            // bad_single_statement(): there is to be nothing after what the tokenizer has been asked for but white space and comments. Of what is typed, that is the rest of what has been read.
            if (m_supply) {
                m_supply->stopReading();
                while (m_supply->fill()) { }
            }
            for (unsigned i = m_furthest + 1; i < m_tokens.size(); ++i) {
                TokenKind kind = m_tokens[i].kind;
                if (kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent || kind == TokenKind::EndMarker || kind == TokenKind::TypeComment)
                    continue;
                SetForScope scope(m_failsInEitherPass, true);
                failAtLastToken("multiple statements found while compiling a single statement"_s);
                m_isBadSingleStatement = true;
                return nullptr;
            }
            ok = true;
            module->body = m_arena.copy(body);
            break;
        }
        case Module::Kind::Expression:
            module->expression = parseExpressions();
            if (module->expression) {
                while (consume(TokenKind::Newline)) { }
                ok = at(TokenKind::EndMarker);
            }
            break;
        case Module::Kind::FunctionType: {
            // func_type
            Vector<Expression*, 8> types;
            if (!consume(TokenKind::LeftParenthesis) || !parseTypeExpressions(types) || !consume(TokenKind::RightParenthesis) || !consume(TokenKind::Arrow))
                break;
            module->argumentTypes = m_arena.copy(types);
            module->expression = parseExpression();
            if (module->expression) {
                while (consume(TokenKind::Newline)) { }
                ok = at(TokenKind::EndMarker);
            }
            break;
        }
        }
        Vector<TypeIgnore*, 4> typeIgnores;
        for (auto& comment : m_arena.typeIgnoreComments) {
            auto* typeIgnore = m_arena.create<TypeIgnore>();
            typeIgnore->line = comment.line;
            typeIgnore->tag = { comment.tag, false };
            typeIgnores.append(typeIgnore);
        }
        module->typeIgnores = m_arena.copy(typeIgnores);
        return ok && !m_error ? module : nullptr;
    }

    // type_expressions, which there may be none of. The stars are allowed and make no difference.
    bool parseTypeExpressions(Vector<Expression*, 8>& types)
    {
        bool sawStar = false;
        while (!at(TokenKind::RightParenthesis)) {
            bool isDoubleStar = consume(TokenKind::DoubleStar);
            if (!isDoubleStar && consume(TokenKind::Star)) {
                if (sawStar)
                    return false;
                sawStar = true;
            } else if (!isDoubleStar && sawStar)
                return false;
            Expression* type = parseExpression();
            if (!type)
                return false;
            types.append(type);
            // Nothing comes after **, and there is no comma after the last.
            if (isDoubleStar || !at(TokenKind::Comma))
                return true;
            next();
            if (at(TokenKind::RightParenthesis))
                return false;
        }
        return true;
    }

    // [TYPE_COMMENT]
    Text parseTypeComment()
    {
        if (!at(TokenKind::TypeComment))
            return { };
        return { next().text, false };
    }

    // ---- Tokens

    // _PyPegen_fill_token(), for as many as it takes
    NEVER_INLINE void fillAsFarAs(unsigned index)
    {
        while (index >= m_tokens.size() && m_supply->fill()) { }
        if (m_supply->isExhausted())
            m_scannerError = m_supply->error();
    }

    const Token& peek(unsigned ahead = 0)
    {
        if (m_index + ahead >= m_tokens.size() && m_supply) [[unlikely]]
            fillAsFarAs(m_index + ahead);
        unsigned index = std::min<unsigned>(m_index + ahead, m_tokens.size() - 1);
        m_furthest = std::max(m_furthest, index);
        // To ask the tokenizer for a token that it cannot give is the end of it, whatever the token was wanted for.
        if (m_tokens[index].kind == TokenKind::Error && !m_error) [[unlikely]] {
            m_error = m_scannerError;
            m_hasScannerError = true;
        }
        return m_tokens[index];
    }

    bool at(TokenKind kind) { return peek().kind == kind; }
    bool at(SoftKeyword keyword) { return peek().kind == TokenKind::Name && peek().softKeyword == keyword; }
    bool atAhead(unsigned ahead, TokenKind kind) { return peek(ahead).kind == kind; }

    const Token& next()
    {
        const Token& token = peek();
        if (m_index + 1 < m_tokens.size() || (m_supply && !m_supply->isExhausted()))
            ++m_index;
        return token;
    }

    bool consume(TokenKind kind)
    {
        if (!at(kind))
            return false;
        next();
        return true;
    }

    // The last one taken that is something one can see.
    const Token& previous()
    {
        unsigned index = m_index;
        while (index) {
            const Token& token = m_tokens[--index];
            switch (token.kind) {
            case TokenKind::Newline:
            case TokenKind::Indent:
            case TokenKind::Dedent:
            case TokenKind::EndMarker:
            case TokenKind::Error:
                continue;
            default:
                return token;
            }
        }
        return m_tokens[0];
    }

    // ---- Errors

    // While something is only being tried, being wrong is not an error.
    bool isSpeculating() const { return m_speculationDepth; }

    std::nullptr_t fail(String&& message, unsigned line, int column, unsigned endLine, int endColumn, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        if (m_error)
            return nullptr;
        // Most of what can be said is said by what is only tried the second time.
        if (!m_callsInvalidRules && !m_failsInEitherPass)
            return nullptr;
        if (!m_isSecondPass && isSpeculating())
            return nullptr;
        m_error = { kind, false, WTF::move(message), line, column, endLine, endColumn };
        m_error.tokenizerLine = m_tokens[m_furthest].endLine;
        m_error.lastLineIsEnded = m_lastLineIsEnded;
        return nullptr;
    }

    // For what CPython's parser says as soon as it comes to it: a token that the grammar insists on, and what is found wrong in making something of what has been parsed.
    template<typename... Arguments>
    std::nullptr_t failInEitherPass(Arguments&&... arguments)
    {
        SetForScope scope(m_failsInEitherPass, true);
        return fail(std::forward<Arguments>(arguments)...);
    }

    // These take up no room in the source, and CPython has them nowhere. What is kept as where they end is where the tokenizer was when it made them.
    static bool isNowhere(const Token& token)
    {
        return token.kind == TokenKind::Indent || token.kind == TokenKind::Dedent || token.kind == TokenKind::EndMarker || token.kind == TokenKind::Error || token.isMadeOfTheEnd;
    }

    // RAISE_SYNTAX_ERROR_KNOWN_LOCATION()
    std::nullptr_t fail(String&& message, const Token& token, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        if (isNowhere(token))
            return fail(WTF::move(message), token.line, -1, token.endLine, -1, kind);
        return fail(WTF::move(message), token.line, token.column, token.endLine, token.endColumn, kind);
    }

    // RAISE_SYNTAX_ERROR_KNOWN_RANGE()
    std::nullptr_t fail(String&& message, const Token& first, const Token& last)
    {
        return fail(WTF::move(message), first.line, first.column, last.endLine, last.endColumn);
    }

    std::nullptr_t fail(String&& message, const Node& first, const Token& last)
    {
        return fail(WTF::move(message), first.line, first.column, last.endLine, last.endColumn);
    }

    std::nullptr_t fail(String&& message, const Token& first, const Node& last)
    {
        return fail(WTF::move(message), first.line, first.column, last.endLine, last.endColumn);
    }

    // RAISE_SYNTAX_ERROR() and RAISE_INDENTATION_ERROR(): at the last token that the tokenizer has been asked for
    std::nullptr_t failAtLastToken(String&& message, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        const Token& token = m_tokens[m_furthest];
        if (isNowhere(token))
            return fail(WTF::move(message), token.line, static_cast<int>(token.endColumn) - 1, token.endLine, -2, kind);
        return fail(WTF::move(message), token.line, token.column, token.endLine, token.endColumn, kind);
    }

    // RAISE_SYNTAX_ERROR_STARTING_FROM(): from there to where the tokenizer is
    std::nullptr_t failStartingFrom(String&& message, unsigned line, unsigned column)
    {
        const Token& token = m_tokens[m_furthest];
        return fail(WTF::move(message), line, column, token.endLine, static_cast<int>(token.endColumn) - 1);
    }

    std::nullptr_t failStartingFrom(String&& message, const Token& token)
    {
        if (isNowhere(token)) {
            const Token& last = m_tokens[m_furthest];
            return fail(WTF::move(message), token.line, -1, last.endLine, static_cast<int>(last.endColumn) - 1);
        }
        return failStartingFrom(WTF::move(message), token.line, token.column);
    }
    std::nullptr_t failStartingFrom(String&& message, const Node& node) { return failStartingFrom(WTF::move(message), node.line, node.column); }

    std::nullptr_t fail(String&& message, const Node& node)
    {
        return fail(WTF::move(message), node.line, node.column, node.endLine, node.endColumn);
    }

    std::nullptr_t fail(String&& message, const Node& first, const Node& last)
    {
        return fail(WTF::move(message), first.line, first.column, last.endLine, last.endColumn);
    }

    std::nullptr_t fail(String&& message)
    {
        return fail(WTF::move(message), peek());
    }

    // Nothing better to say than where it stopped making sense.
    void failGenerically(const Token& token)
    {
        SetForScope scope(m_failsInEitherPass, true);
        if (token.kind == TokenKind::Indent || token.kind == TokenKind::Dedent) {
            failAtLastToken(token.kind == TokenKind::Indent ? "unexpected indent"_s : "unexpected unindent"_s, SyntaxError::Kind::IndentationError);
            return;
        }
        fail("invalid syntax"_s, token);
    }

    bool expect(TokenKind kind)
    {
        return consume(kind);
    }

    // Where the grammar insists on a colon, its absence is what is wrong. Elsewhere that is only said if the line ends there.
    enum class ColonIs : uint8_t { Insisted, Expected };
    bool expectColon(ColonIs colonIs = ColonIs::Expected)
    {
        if (consume(TokenKind::Colon))
            return true;
        if (colonIs == ColonIs::Insisted)
            failInEitherPass("expected ':'"_s, peek());
        else if (at(TokenKind::Newline))
            failAtLastToken("expected ':'"_s);
        return false;
    }

    bool isSafeToRecurse()
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return true;
        failInEitherPass("too many nested parentheses"_s, peek());
        return false;
    }

    template<typename Function>
    auto speculate(const Function& function)
    {
        unsigned index = m_index;
        ++m_speculationDepth;
        auto result = function();
        --m_speculationDepth;
        if (!result)
            m_index = index;
        return result;
    }

    // For trying something in the second pass: nothing is taken. What is found wrong on the way is an error all the same.
    template<typename Function>
    auto lookForWhatIsWrong(const Function& function)
    {
        unsigned index = m_index;
        auto result = function();
        m_index = index;
        return result;
    }

    template<typename Function>
    Expression* memoized(MemoTable& table, const Function& function)
    {
        if (!m_keepsWhatIsParsed) [[likely]]
            return function();
        if (m_error)
            return nullptr;
        unsigned index = m_index;
        if (auto iterator = table.find(index); iterator != table.end()) {
            m_index = iterator->value.end;
            return iterator->value.result;
        }
        Expression* result = function();
        if (!result)
            m_index = index;
        table.add(index, Memo { result, m_index });
        return result;
    }

    // ---- Nodes

    struct Mark {
        unsigned line;
        unsigned column;
        unsigned start;
    };

    Mark mark()
    {
        const Token& token = peek();
        return { token.line, token.column, token.start };
    }

    static Mark markOf(const Token& token) { return { token.line, token.column, token.start }; }
    static Mark markOf(const Node& node) { return { node.line, node.column, node.start }; }

    // A node that began at the mark and ends with the last token taken.
    template<typename T>
    T* make(Mark mark)
    {
        T* node = m_arena.create<T>();
        node->line = mark.line;
        node->column = mark.column;
        node->start = mark.start;
        const Token& last = previous();
        node->endLine = last.endLine;
        node->endColumn = last.endColumn;
        node->end = last.end;
        return node;
    }

    template<typename T>
    T* make(const Token& token)
    {
        T* node = m_arena.create<T>();
        setRange(*node, token, token);
        return node;
    }

    template<typename First, typename Last>
    static void setRange(Node& node, const First& first, const Last& last)
    {
        node.line = first.line;
        node.column = first.column;
        node.start = first.start;
        node.endLine = last.endLine;
        node.endColumn = last.endColumn;
        node.end = last.end;
    }

    const Identifier* makeIdentifier(const String& string)
    {
        if (string.is8Bit())
            return &m_arena.identifiers().makeIdentifier(m_vm, string.span8());
        return &m_arena.identifiers().makeIdentifier(m_vm, string.span16());
    }

    Name* makeName(const Token& token, ExpressionContext context = ExpressionContext::Load)
    {
        auto* name = make<Name>(token);
        name->id = token.text;
        name->context = context;
        return name;
    }

    // ---- Targets

    static ASCIILiteral describe(Expression& expression)
    {
        switch (expression.kind) {
        case Expression::Kind::Attribute:
            return "attribute"_s;
        case Expression::Kind::Subscript:
            return "subscript"_s;
        case Expression::Kind::Starred:
            return "starred"_s;
        case Expression::Kind::Name:
            return "name"_s;
        case Expression::Kind::List:
            return "list"_s;
        case Expression::Kind::Tuple:
            return "tuple"_s;
        case Expression::Kind::Lambda:
            return "lambda"_s;
        case Expression::Kind::Call:
            return "function call"_s;
        case Expression::Kind::BoolOp:
        case Expression::Kind::BinOp:
        case Expression::Kind::UnaryOp:
            return "expression"_s;
        case Expression::Kind::GeneratorExp:
            return "generator expression"_s;
        case Expression::Kind::Yield:
        case Expression::Kind::YieldFrom:
            return "yield expression"_s;
        case Expression::Kind::Await:
            return "await expression"_s;
        case Expression::Kind::ListComp:
            return "list comprehension"_s;
        case Expression::Kind::SetComp:
            return "set comprehension"_s;
        case Expression::Kind::DictComp:
            return "dict comprehension"_s;
        case Expression::Kind::Dict:
            return "dict literal"_s;
        case Expression::Kind::Set:
            return "set display"_s;
        case Expression::Kind::JoinedStr:
        case Expression::Kind::FormattedValue:
            return "f-string expression"_s;
        case Expression::Kind::TemplateStr:
        case Expression::Kind::Interpolation:
            return "t-string expression"_s;
        case Expression::Kind::Constant:
            switch (expression.as<Constant>().type) {
            case Constant::Type::None:
                return "None"_s;
            case Constant::Type::True:
                return "True"_s;
            case Constant::Type::False:
                return "False"_s;
            case Constant::Type::Ellipsis:
                return "ellipsis"_s;
            default:
                return "literal"_s;
            }
        case Expression::Kind::Compare:
            return "comparison"_s;
        case Expression::Kind::IfExp:
            return "conditional expression"_s;
        case Expression::Kind::NamedExpr:
            return "named expression"_s;
        case Expression::Kind::Slice:
            return "slice"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What was parsed as an expression turns out to be assigned to, or deleted. Gives the part of it that cannot be, if any.
    Expression* setContext(Expression& expression, ExpressionContext context)
    {
        switch (expression.kind) {
        case Expression::Kind::Name:
            expression.as<Name>().context = context;
            return nullptr;
        case Expression::Kind::Attribute:
            expression.as<Attribute>().context = context;
            return nullptr;
        case Expression::Kind::Subscript:
            expression.as<Subscript>().context = context;
            return nullptr;
        case Expression::Kind::Starred:
            if (context == ExpressionContext::Del)
                return &expression;
            expression.as<Starred>().context = context;
            return setContext(*expression.as<Starred>().value, context);
        case Expression::Kind::List:
            expression.as<List>().context = context;
            for (Expression* element : expression.as<List>().elements) {
                if (Expression* invalid = setContext(*element, context))
                    return invalid;
            }
            return nullptr;
        case Expression::Kind::Tuple:
            expression.as<Tuple>().context = context;
            for (Expression* element : expression.as<Tuple>().elements) {
                if (Expression* invalid = setContext(*element, context))
                    return invalid;
            }
            return nullptr;
        default:
            return &expression;
        }
    }

    bool makeTarget(Expression& expression, ExpressionContext context)
    {
        return !setContext(expression, context);
    }

    // _PyPegen_get_invalid_target(): the part of it that cannot be assigned to, or deleted
    enum class Targets : uint8_t { Star, Del, For };
    static Expression* invalidTarget(Expression& expression, Targets targets)
    {
        auto firstOf = [&] (auto& elements) -> Expression* {
            for (Expression* element : elements) {
                if (Expression* invalid = invalidTarget(*element, targets))
                    return invalid;
            }
            return nullptr;
        };
        switch (expression.kind) {
        case Expression::Kind::List:
            return firstOf(expression.as<List>().elements);
        case Expression::Kind::Tuple:
            return firstOf(expression.as<Tuple>().elements);
        case Expression::Kind::Starred:
            if (targets == Targets::Del)
                return &expression;
            return invalidTarget(*expression.as<Starred>().value, targets);
        case Expression::Kind::Compare:
            // The `a in b` of `for a in b` is a comparison to whatever has parsed it as an expression.
            if (targets == Targets::For) {
                if (expression.as<Compare>().ops[0] == ComparisonOperator::In)
                    return invalidTarget(*expression.as<Compare>().left, targets);
                return nullptr;
            }
            return &expression;
        case Expression::Kind::Name:
        case Expression::Kind::Subscript:
        case Expression::Kind::Attribute:
            return nullptr;
        default:
            return &expression;
        }
    }

    // RAISE_SYNTAX_ERROR_INVALID_TARGET()
    void failForInvalidTarget(Expression& expression, Targets targets)
    {
        if (Expression* invalid = invalidTarget(expression, targets))
            fail(concatenate(targets == Targets::Del ? "cannot delete "_s : "cannot assign to "_s, describe(*invalid)), *invalid);
    }

    // star_target
    Expression* parseStarTarget()
    {
        if (at(TokenKind::Star)) {
            Mark start = mark();
            next();
            if (at(TokenKind::Star))
                return nullptr;
            Expression* value = parseStarTarget();
            if (!value)
                return nullptr;
            auto* starred = make<Starred>(start);
            starred->value = value;
            starred->context = ExpressionContext::Store;
            return starred;
        }
        Expression* target = parsePrimary();
        if (!target || !makeTarget(*target, ExpressionContext::Store))
            return nullptr;
        return target;
    }

    // star_targets, which are followed by `in`.
    Expression* parseStarTargets()
    {
        Mark start = mark();
        Expression* first = parseStarTarget();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma))
            return first;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            // The last comma may have nothing after it.
            unsigned index = m_index;
            Expression* element = parseStarTarget();
            if (!element) {
                m_index = index;
                break;
            }
            elements.append(element);
        }
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        tuple->context = ExpressionContext::Store;
        return tuple;
    }

    // ---- Expressions

    template<typename ParseElement>
    Expression* parseTupleOrSingle(const ParseElement& parseElement, bool (Parser::*canStartElement)())
    {
        Mark start = mark();
        Expression* first = parseElement();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma))
            return first;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (!(this->*canStartElement)())
                break;
            unsigned index = m_index;
            Expression* element = parseElement();
            if (!element) {
                if (m_error)
                    return nullptr;
                m_index = index;
                break;
            }
            elements.append(element);
        }
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        return tuple;
    }

    // Whether an expression could begin here. After a comma, that says whether the comma was the last thing.
    bool canStartExpression()
    {
        switch (peek().kind) {
        case TokenKind::Name:
        case TokenKind::Number:
        case TokenKind::String:
        case TokenKind::FStringStart:
        case TokenKind::TStringStart:
        case TokenKind::LeftParenthesis:
        case TokenKind::LeftBracket:
        case TokenKind::LeftBrace:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Tilde:
        case TokenKind::Ellipsis:
        case TokenKind::KeywordNone:
        case TokenKind::KeywordTrue:
        case TokenKind::KeywordFalse:
        case TokenKind::KeywordNot:
        case TokenKind::KeywordLambda:
        case TokenKind::KeywordAwait:
            return true;
        default:
            return false;
        }
    }

    bool canStartStarExpression() { return at(TokenKind::Star) || canStartExpression(); }

    // expressions
    Expression* parseExpressions()
    {
        return parseTupleOrSingle([&] { return parseExpression(); }, &Parser::canStartExpression);
    }

    // star_expressions
    Expression* parseStarExpressions()
    {
        return parseTupleOrSingle([&] { return parseStarExpression(); }, &Parser::canStartStarExpression);
    }

    Expression* parseStarred()
    {
        Mark start = mark();
        next();
        Expression* value = parseBitwiseOr();
        if (!value)
            return nullptr;
        auto* starred = make<Starred>(start);
        starred->value = value;
        return starred;
    }

    // star_expression
    Expression* parseStarExpression()
    {
        return memoized(m_memos.starExpressions, [&] {
            if (at(TokenKind::Star))
                return parseStarred();
            return parseExpression();
        });
    }

    // star_named_expression
    Expression* parseStarNamedExpression()
    {
        if (at(TokenKind::Star))
            return parseStarred();
        return parseNamedExpression();
    }

    // annotated_rhs
    Expression* parseYieldOrStarExpressions()
    {
        if (at(TokenKind::KeywordYield))
            return parseYield();
        return parseStarExpressions();
    }

    // yield_expr
    Expression* parseYield()
    {
        Mark start = mark();
        next();
        unsigned index = m_index;
        if (consume(TokenKind::KeywordFrom)) {
            if (Expression* value = parseExpression()) {
                auto* yield = make<YieldFrom>(start);
                yield->value = value;
                return yield;
            }
            if (m_error)
                return nullptr;
            m_index = index;
        }
        Expression* value = nullptr;
        if (canStartStarExpression()) {
            value = parseStarExpressions();
            if (m_error)
                return nullptr;
            if (!value)
                m_index = index;
        }
        auto* yield = make<Yield>(start);
        yield->value = value;
        return yield;
    }

    // named_expression
    Expression* parseNamedExpression()
    {
        if (at(TokenKind::Name) && atAhead(1, TokenKind::ColonEqual)) {
            Mark start = mark();
            Name* target = makeName(next(), ExpressionContext::Store);
            next();
            Expression* value = parseExpression();
            if (!value)
                return nullptr;
            auto* named = make<NamedExpr>(start);
            named->target = target;
            named->value = value;
            return named;
        }
        if (m_callsInvalidRules) {
            recognizeInvalidNamedExpression();
            if (m_error)
                return nullptr;
        }
        Expression* expression = parseExpression();
        if (!expression || at(TokenKind::ColonEqual))
            return nullptr;
        return expression;
    }

    // invalid_named_expression
    void recognizeInvalidNamedExpression()
    {
        if (!m_invalidNamedExpressions.add(m_index).isNewEntry)
            return;
        lookForWhatIsWrong([&] {
            Expression* target = parseExpression();
            if (target && consume(TokenKind::ColonEqual) && parseExpression())
                fail(concatenate("cannot use assignment expressions with "_s, describe(*target)), *target);
            return false;
        });
        if (m_error)
            return;
        auto isFollowedByAnother = [&] { return at(TokenKind::Equal) || at(TokenKind::ColonEqual); };
        lookForWhatIsWrong([&] {
            if (!at(TokenKind::Name) || !atAhead(1, TokenKind::Equal))
                return false;
            const Token& name = next();
            next();
            Expression* value = parseBitwiseOr();
            if (value && !isFollowedByAnother())
                fail("invalid syntax. Maybe you meant '==' or ':=' instead of '='?"_s, name, *value);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            // !(list | tuple | genexp | 'True' | 'None' | 'False')
            if (at(TokenKind::KeywordTrue) || at(TokenKind::KeywordNone) || at(TokenKind::KeywordFalse))
                return false;
            if (at(TokenKind::LeftParenthesis) || at(TokenKind::LeftBracket)) {
                Expression* atom = lookForWhatIsWrong([&] { return parseAtom(); });
                if (m_error)
                    return false;
                if (atom && (atom->is<List>() || atom->is<GeneratorExp>() || (atom->is<Tuple>() && at(TokenKind::LeftParenthesis))))
                    return false;
            }
            Expression* target = parseBitwiseOr();
            if (target && consume(TokenKind::Equal) && parseBitwiseOr() && !isFollowedByAnother())
                fail(concatenate("cannot assign to "_s, describe(*target), " here. Maybe you meant '==' instead of '='?"_s), *target);
            return false;
        });
    }

    // expression
    Expression* parseExpression()
    {
        if (!isSafeToRecurse())
            return nullptr;
        return memoized(m_memos.expressions, [&] () -> Expression* {
            if (m_callsInvalidRules) {
                recognizeInvalidExpression();
                if (m_error)
                    return nullptr;
                recognizeLegacyExpression();
                if (m_error)
                    return nullptr;
            }
            return parseExpressionItself();
        });
    }

    // expression_without_invalid
    Expression* parseExpressionWithoutInvalid()
    {
        SetForScope scope(m_callsInvalidRules, false);
        return parseExpressionItself();
    }

    // _PyPegen_check_legacy_stmt()
    static bool isLegacyStatement(Expression& expression)
    {
        auto* name = expression.tryAs<Name>();
        return name && (*name->id == "print"_s || *name->id == "exec"_s);
    }

    // invalid_expression. As in what CPython generates from its grammar, when one of these is all there and yet has nothing to say, the rest are not tried.
    void recognizeInvalidExpression()
    {
        bool isDone = false;
        lookForWhatIsWrong([&] {
            if (!consume(TokenKind::String))
                return false;
            Expression* first = nullptr;
            Expression* last = nullptr;
            while (!at(TokenKind::String)) {
                unsigned index = m_index;
                Expression* expression = parseExpressionWithoutInvalid();
                if (!expression) {
                    m_index = index;
                    break;
                }
                if (!first)
                    first = expression;
                last = expression;
            }
            if (first && at(TokenKind::String))
                fail("invalid syntax. Is this intended to be part of the string?"_s, *first, *last);
            return false;
        });
        if (m_error)
            return;

        lookForWhatIsWrong([&] {
            // !(NAME STRING | SOFT_KEYWORD)
            if (at(TokenKind::Name) && (atAhead(1, TokenKind::String) || peek().softKeyword != SoftKeyword::None))
                return false;
            Expression* first = parseDisjunction();
            if (!first)
                return false;
            Expression* second = parseExpressionWithoutInvalid();
            if (!second)
                return false;
            isDone = true;
            if (!isLegacyStatement(*first) && m_tokens[m_index - 1].isInsideBrackets)
                fail("invalid syntax. Perhaps you forgot a comma?"_s, *first, *second);
            return false;
        });
        if (m_error || isDone)
            return;

        lookForWhatIsWrong([&] {
            Expression* body = parseDisjunction();
            if (!body || !consume(TokenKind::KeywordIf))
                return false;
            Expression* test = parseDisjunction();
            if (!test)
                return false;
            if (!at(TokenKind::KeywordElse) && !at(TokenKind::Colon)) {
                fail("expected 'else' after 'if' expression"_s, *body, *test);
                return false;
            }
            if (!consume(TokenKind::KeywordElse))
                return false;
            bool isExpression = lookForWhatIsWrong([&] { return parseExpression(); });
            if (!isExpression && !m_error)
                fail("expected expression after 'else', but statement is given"_s, peek());
            return false;
        });
        if (m_error)
            return;

        lookForWhatIsWrong([&] {
            if (!at(TokenKind::KeywordPass) && !at(TokenKind::KeywordBreak) && !at(TokenKind::KeywordContinue))
                return false;
            const Token& statement = next();
            if (consume(TokenKind::KeywordIf) && parseDisjunction() && consume(TokenKind::KeywordElse) && parseSimpleStatement())
                fail("expected expression before 'if', but statement is given"_s, statement);
            return false;
        });
        if (m_error)
            return;

        lookForWhatIsWrong([&] {
            if (!at(TokenKind::KeywordLambda))
                return false;
            const Token& lambda = next();
            if (!parseParameters(TokenKind::Colon, false) || !at(TokenKind::Colon))
                return false;
            const Token& colon = next();
            if (at(TokenKind::FStringMiddle) || at(TokenKind::TStringMiddle))
                fail(prefixed(at(TokenKind::TStringMiddle), "lambda expressions are not allowed without parentheses"_s), lambda, colon);
            return false;
        });
    }

    // invalid_legacy_expression
    void recognizeLegacyExpression()
    {
        lookForWhatIsWrong([&] {
            if (!at(TokenKind::Name) || atAhead(1, TokenKind::LeftParenthesis))
                return false;
            Name* name = makeName(next());
            Expression* rest = parseStarExpressions();
            if (rest && isLegacyStatement(*name))
                fail(concatenate("Missing parentheses in call to '"_s, name->id->string(), "'. Did you mean "_s, name->id->string(), "(...)?"_s), *name, *rest);
            return false;
        });
    }

    Expression* parseExpressionItself()
    {
        if (at(TokenKind::KeywordLambda))
            return parseLambda();
        Mark start = mark();
        Expression* body = parseDisjunction();
        if (!body || !at(TokenKind::KeywordIf))
            return body;
        // If it does not go on as it should, this much is an expression all the same.
        unsigned index = m_index;
        auto isOnlyBody = [&] {
            m_index = index;
            return body;
        };
        next();
        Expression* test = parseDisjunction();
        if (!test || !consume(TokenKind::KeywordElse))
            return isOnlyBody();
        Expression* orElse = parseExpression();
        if (!orElse)
            return isOnlyBody();
        auto* conditional = make<IfExp>(start);
        conditional->test = test;
        conditional->body = body;
        conditional->orElse = orElse;
        return conditional;
    }

    // lambdef
    Expression* parseLambda()
    {
        Mark start = mark();
        next();
        Arguments* arguments = parseParameters(TokenKind::Colon, false);
        if (!arguments || !consume(TokenKind::Colon))
            return nullptr;
        Expression* body = parseExpression();
        if (!body)
            return nullptr;
        auto* lambda = make<Lambda>(start);
        lambda->arguments = arguments;
        lambda->body = body;
        return lambda;
    }

    template<typename ParseOperand>
    Expression* parseBooleanOperation(TokenKind keyword, BooleanOperator op, const ParseOperand& parseOperand)
    {
        Mark start = mark();
        Expression* first = parseOperand();
        if (!first || !at(keyword))
            return first;
        Vector<Expression*, 8> values;
        values.append(first);
        while (at(keyword)) {
            unsigned index = m_index;
            next();
            Expression* value = parseOperand();
            if (!value) {
                if (m_error)
                    return nullptr;
                m_index = index;
                break;
            }
            values.append(value);
        }
        if (values.size() == 1)
            return first;
        auto* operation = make<BoolOp>(start);
        operation->op = op;
        operation->values = m_arena.copy(values);
        return operation;
    }

    Expression* parseDisjunction()
    {
        return memoized(m_memos.disjunctions, [&] { return parseBooleanOperation(TokenKind::KeywordOr, BooleanOperator::Or, [&] { return parseConjunction(); }); });
    }

    Expression* parseConjunction()
    {
        return memoized(m_memos.conjunctions, [&] { return parseBooleanOperation(TokenKind::KeywordAnd, BooleanOperator::And, [&] { return parseInversion(); }); });
    }

    Expression* parseInversion()
    {
        return memoized(m_memos.inversions, [&] { return parseInversionItself(); });
    }

    Expression* parseInversionItself()
    {
        if (!at(TokenKind::KeywordNot))
            return parseComparison();
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        Expression* operand = parseInversion();
        if (!operand)
            return nullptr;
        auto* operation = make<UnaryOp>(start);
        operation->op = UnaryOperator::Not;
        operation->operand = operand;
        return operation;
    }

    std::optional<ComparisonOperator> consumeComparisonOperator()
    {
        switch (peek().kind) {
        case TokenKind::EqualEqual:
            next();
            return ComparisonOperator::Eq;
        case TokenKind::NotEqual:
            // from __future__ import barry_as_FLUFL
            if (peek().isLessGreater != m_arena.usesLessGreater) {
                if (m_arena.usesLessGreater)
                    failInEitherPass("with Barry as BDFL, use '<>' instead of '!='"_s, peek());
                return std::nullopt;
            }
            next();
            return ComparisonOperator::NotEq;
        case TokenKind::Less:
            next();
            return ComparisonOperator::Lt;
        case TokenKind::LessEqual:
            next();
            return ComparisonOperator::LtE;
        case TokenKind::Greater:
            next();
            return ComparisonOperator::Gt;
        case TokenKind::GreaterEqual:
            next();
            return ComparisonOperator::GtE;
        case TokenKind::KeywordIn:
            next();
            return ComparisonOperator::In;
        case TokenKind::KeywordNot:
            if (!atAhead(1, TokenKind::KeywordIn))
                return std::nullopt;
            next();
            next();
            return ComparisonOperator::NotIn;
        case TokenKind::KeywordIs:
            next();
            if (consume(TokenKind::KeywordNot))
                return ComparisonOperator::IsNot;
            return ComparisonOperator::Is;
        default:
            return std::nullopt;
        }
    }

    Expression* parseComparison()
    {
        Mark start = mark();
        Expression* left = parseBitwiseOr();
        if (!left)
            return nullptr;
        Vector<ComparisonOperator, 4> ops;
        Vector<Expression*, 4> comparators;
        while (true) {
            unsigned index = m_index;
            auto op = consumeComparisonOperator();
            if (!op)
                break;
            Expression* comparator = parseBitwiseOr();
            if (!comparator) {
                if (m_error)
                    return nullptr;
                m_index = index;
                break;
            }
            ops.append(*op);
            comparators.append(comparator);
        }
        if (ops.isEmpty())
            return m_error ? nullptr : left;
        auto* compare = make<Compare>(start);
        compare->left = left;
        compare->ops = m_arena.copy(ops);
        compare->comparators = m_arena.copy(comparators);
        return compare;
    }

    struct BinaryOperatorInfo {
        BinaryOperator op;
        unsigned precedence;
    };

    // From bitwise_or, which binds loosest, down to term. All of them group to the left.
    static std::optional<BinaryOperatorInfo> binaryOperator(TokenKind kind)
    {
        switch (kind) {
        case TokenKind::VerticalBar:
            return BinaryOperatorInfo { BinaryOperator::BitOr, 1 };
        case TokenKind::Circumflex:
            return BinaryOperatorInfo { BinaryOperator::BitXor, 2 };
        case TokenKind::Ampersand:
            return BinaryOperatorInfo { BinaryOperator::BitAnd, 3 };
        case TokenKind::LeftShift:
            return BinaryOperatorInfo { BinaryOperator::LShift, 4 };
        case TokenKind::RightShift:
            return BinaryOperatorInfo { BinaryOperator::RShift, 4 };
        case TokenKind::Plus:
            return BinaryOperatorInfo { BinaryOperator::Add, 5 };
        case TokenKind::Minus:
            return BinaryOperatorInfo { BinaryOperator::Sub, 5 };
        case TokenKind::Star:
            return BinaryOperatorInfo { BinaryOperator::Mult, 6 };
        case TokenKind::Slash:
            return BinaryOperatorInfo { BinaryOperator::Div, 6 };
        case TokenKind::DoubleSlash:
            return BinaryOperatorInfo { BinaryOperator::FloorDiv, 6 };
        case TokenKind::Percent:
            return BinaryOperatorInfo { BinaryOperator::Mod, 6 };
        case TokenKind::At:
            return BinaryOperatorInfo { BinaryOperator::MatMult, 6 };
        default:
            return std::nullopt;
        }
    }

    Expression* parseBitwiseOr() { return parseBinaryOperation(1); }

    Expression* parseBinaryOperation(unsigned minimumPrecedence)
    {
        if (minimumPrecedence >= m_memos.binaryOperations.size())
            return parseFactor();
        return memoized(m_memos.binaryOperations[minimumPrecedence], [&] { return parseBinaryOperationItself(minimumPrecedence); });
    }

    Expression* parseBinaryOperationItself(unsigned minimumPrecedence)
    {
        Mark start = mark();
        Expression* left = parseFactor();
        if (!left)
            return nullptr;
        while (true) {
            auto info = binaryOperator(peek().kind);
            if (!info || info->precedence < minimumPrecedence)
                return left;
            unsigned index = m_index;
            next();
            // invalid_arithmetic
            if (m_callsInvalidRules && info->precedence >= 5 && at(TokenKind::KeywordNot)) {
                const Token& keyword = next();
                if (Expression* operand = parseInversion())
                    fail("'not' after an operator must be parenthesized"_s, keyword, *operand);
                return nullptr;
            }
            Expression* right = parseBinaryOperation(info->precedence + 1);
            if (!right) {
                if (m_error)
                    return nullptr;
                m_index = index;
                return left;
            }
            auto* operation = make<BinOp>(start);
            operation->left = left;
            operation->op = info->op;
            operation->right = right;
            left = operation;
        }
    }

    // factor
    Expression* parseFactor()
    {
        return memoized(m_memos.factors, [&] { return parseFactorItself(); });
    }

    Expression* parseFactorItself()
    {
        UnaryOperator op;
        switch (peek().kind) {
        case TokenKind::Plus:
            op = UnaryOperator::UAdd;
            break;
        case TokenKind::Minus:
            op = UnaryOperator::USub;
            break;
        case TokenKind::Tilde:
            op = UnaryOperator::Invert;
            break;
        default:
            return parsePower();
        }
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        // invalid_factor
        if (m_callsInvalidRules && at(TokenKind::KeywordNot)) {
            const Token& keyword = next();
            if (Expression* operand = parseFactor())
                fail("'not' after an operator must be parenthesized"_s, keyword, *operand);
            return nullptr;
        }
        Expression* operand = parseFactor();
        if (!operand)
            return nullptr;
        auto* operation = make<UnaryOp>(start);
        operation->op = op;
        operation->operand = operand;
        return operation;
    }

    // power
    Expression* parsePower()
    {
        Mark start = mark();
        Expression* left = parseAwaitPrimary();
        if (!left || !at(TokenKind::DoubleStar))
            return left;
        unsigned index = m_index;
        next();
        Expression* right = parseFactor();
        if (!right) {
            if (m_error)
                return nullptr;
            m_index = index;
            return left;
        }
        auto* operation = make<BinOp>(start);
        operation->left = left;
        operation->op = BinaryOperator::Pow;
        operation->right = right;
        return operation;
    }

    // await_primary
    Expression* parseAwaitPrimary()
    {
        return memoized(m_memos.awaitPrimaries, [&] { return parseAwaitPrimaryItself(); });
    }

    Expression* parseAwaitPrimaryItself()
    {
        if (!at(TokenKind::KeywordAwait))
            return parsePrimary();
        Mark start = mark();
        next();
        Expression* value = parsePrimary();
        if (!value)
            return nullptr;
        auto* await = make<Await>(start);
        await->value = value;
        return await;
    }

    // primary
    Expression* parsePrimary()
    {
        return memoized(m_memos.primaries, [&] { return parsePrimaryItself(); });
    }

    Expression* parsePrimaryItself()
    {
        Mark start = mark();
        Expression* value = parseAtom();
        while (value) {
            unsigned index = m_index;
            auto isAllThereIs = [&] () -> Expression* {
                if (m_error)
                    return nullptr;
                m_index = index;
                return value;
            };
            // f(x for x in y) is f and a generator expression, so one of those is looked for after whatever this is. And what is looked for when there is none may begin with any bracket:
            // invalid_comprehension. For a parenthesis parseArguments() sees to it.
            if (m_callsInvalidRules && (at(TokenKind::LeftBrace) || at(TokenKind::LeftBracket))) {
                recognizeInvalidComprehension();
                if (m_error)
                    return nullptr;
            }
            switch (peek().kind) {
            case TokenKind::Dot: {
                next();
                if (!at(TokenKind::Name))
                    return isAllThereIs();
                const Token& nameToken = next();
                auto* attribute = make<Attribute>(start);
                attribute->value = value;
                attribute->attribute = nameToken.text;
                attribute->attributeStart = nameToken.start;
                value = attribute;
                continue;
            }
            case TokenKind::LeftParenthesis: {
                Vector<Expression*, 8> arguments;
                Vector<Keyword*, 8> keywords;
                if (!parseArguments(arguments, keywords))
                    return isAllThereIs();
                auto* call = make<Call>(start);
                call->function = value;
                call->arguments = m_arena.copy(arguments);
                call->keywords = m_arena.copy(keywords);
                value = call;
                continue;
            }
            case TokenKind::LeftBracket: {
                next();
                Expression* slice = parseSlices();
                if (!slice || !expect(TokenKind::RightBracket))
                    return isAllThereIs();
                auto* subscript = make<Subscript>(start);
                subscript->value = value;
                subscript->slice = slice;
                value = subscript;
                continue;
            }
            default:
                return value;
            }
        }
        return nullptr;
    }

    bool atComprehension()
    {
        return at(TokenKind::KeywordFor) || (at(TokenKind::KeywordAsync) && atAhead(1, TokenKind::KeywordFor));
    }

    // assignment_expression | expression !':='
    Expression* parseAssignmentExpressionOrExpression()
    {
        if (at(TokenKind::Name) && atAhead(1, TokenKind::ColonEqual))
            return parseNamedExpression();
        unsigned index = m_index;
        Expression* expression = parseExpression();
        if (expression && !at(TokenKind::ColonEqual))
            return expression;
        m_index = index;
        return nullptr;
    }

    // starred_expression
    Expression* parseStarredExpression()
    {
        if (!at(TokenKind::Star))
            return nullptr;
        Mark start = mark();
        const Token& star = peek();
        if (m_callsInvalidRules) {
            // invalid_starred_expression_unpacking
            lookForWhatIsWrong([&] {
                next();
                if (!parseExpression() || !consume(TokenKind::Equal))
                    return false;
                if (Expression* value = parseExpression())
                    fail("cannot assign to iterable argument unpacking"_s, star, *value);
                return false;
            });
            if (m_error)
                return nullptr;
        }
        next();
        if (Expression* value = parseExpression()) {
            auto* starred = make<Starred>(start);
            starred->value = value;
            return starred;
        }
        // invalid_starred_expression
        failAtLastToken("Invalid star expression"_s);
        return nullptr;
    }

    // invalid_kwarg
    void recognizeInvalidKeywordArgument()
    {
        if ((at(TokenKind::KeywordTrue) || at(TokenKind::KeywordFalse) || at(TokenKind::KeywordNone)) && atAhead(1, TokenKind::Equal)) {
            fail(concatenate("cannot assign to "_s, at(TokenKind::KeywordTrue) ? "True"_s : at(TokenKind::KeywordFalse) ? "False"_s : "None"_s), peek(), peek(1));
            return;
        }
        bool isNamed = at(TokenKind::Name) && atAhead(1, TokenKind::Equal);
        lookForWhatIsWrong([&] {
            if (!isNamed)
                return false;
            const Token& name = next();
            const Token& equal = next();
            Vector<Comprehension*, 2> generators;
            if (parseExpression() && atComprehension() && parseComprehensionClauses(generators))
                fail("invalid syntax. Maybe you meant '==' or ':=' instead of '='?"_s, name, equal);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            if (isNamed)
                return false;
            Expression* expression = parseExpression();
            if (expression && at(TokenKind::Equal))
                fail("expression cannot contain assignment, perhaps you meant \"==\"?"_s, *expression, peek());
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            if (!at(TokenKind::DoubleStar))
                return false;
            const Token& stars = next();
            if (!parseExpression() || !consume(TokenKind::Equal))
                return false;
            if (Expression* value = parseExpression())
                fail("cannot assign to keyword argument unpacking"_s, stars, *value);
            return false;
        });
    }

    struct CallArguments {
        Vector<Expression*, 8> positional;
        Vector<Keyword*, 8> keywords;
        bool hasKeywordSection { false }; // kwargs, in the grammar. There may be *x in it as well as x=y and **x.
    };

    // What has been parsed already, so that it need not be parsed again.
    struct Parsed {
        Expression* expression { nullptr };
        unsigned start { 0 };
        unsigned end { 0 };
    };

    // args. It leaves off before whatever cannot be part of them, and is false if there are none.
    bool parseArgs(CallArguments& result) { return parseArgs(result, Parsed()); }
    bool parseArgs(CallArguments& result, Parsed first)
    {
        enum class Section : uint8_t { Positional, KeywordOrStarred, KeywordOrDoubleStarred };
        Section section = Section::Positional;

        // NAME '=' expression
        auto parseNamed = [&] {
            if (!at(TokenKind::Name) || !atAhead(1, TokenKind::Equal))
                return false;
            Mark start = mark();
            const Identifier* name = next().text;
            next();
            Expression* value = parseExpression();
            if (!value)
                return false;
            auto* keyword = make<Keyword>(start);
            keyword->name = name;
            keyword->value = value;
            result.keywords.append(keyword);
            return true;
        };
        auto parseOne = [&] {
            unsigned index = m_index;
            if (section == Section::Positional) {
                if (at(TokenKind::Star)) {
                    if (Expression* starred = parseStarredExpression()) {
                        result.positional.append(starred);
                        return true;
                    }
                } else {
                    Expression* value;
                    if (first.expression && first.start == index) {
                        value = first.expression;
                        m_index = first.end;
                    } else
                        value = parseAssignmentExpressionOrExpression();
                    if (value && !at(TokenKind::Equal)) {
                        result.positional.append(value);
                        return true;
                    }
                }
                if (m_error)
                    return false;
                m_index = index;
                section = Section::KeywordOrStarred;
            }
            if (section == Section::KeywordOrStarred) {
                if (m_callsInvalidRules) {
                    recognizeInvalidKeywordArgument();
                    if (m_error)
                        return false;
                }
                if (parseNamed())
                    return true;
                if (m_error)
                    return false;
                m_index = index;
                if (Expression* starred = parseStarredExpression()) {
                    result.positional.append(starred);
                    return true;
                }
                if (m_error)
                    return false;
                m_index = index;
                section = Section::KeywordOrDoubleStarred;
            }
            if (m_callsInvalidRules) {
                recognizeInvalidKeywordArgument();
                if (m_error)
                    return false;
            }
            if (parseNamed())
                return true;
            if (m_error)
                return false;
            m_index = index;
            if (!at(TokenKind::DoubleStar))
                return false;
            Mark start = mark();
            next();
            Expression* value = parseExpression();
            if (!value)
                return false;
            auto* keyword = make<Keyword>(start);
            keyword->value = value;
            result.keywords.append(keyword);
            return true;
        };

        bool hasAny = false;
        while (true) {
            unsigned index = m_index;
            if (hasAny && !consume(TokenKind::Comma))
                break;
            if (!parseOne()) {
                m_index = index;
                break;
            }
            hasAny = true;
            result.hasKeywordSection = section != Section::Positional;
        }
        return hasAny && !m_error;
    }

    // _PyPegen_get_last_comprehension_item()
    static Expression& lastItemOf(const Vector<Comprehension*, 2>& generators)
    {
        Comprehension& last = *generators.last();
        return last.conditions.size() ? *last.conditions[last.conditions.size() - 1] : *last.iterable;
    }

    // for_if_clauses, of which there is at least one
    bool parseSomeComprehensionClauses(Vector<Comprehension*, 2>& generators)
    {
        return atComprehension() && parseComprehensionClauses(generators);
    }

    // invalid_arguments
    void recognizeInvalidArguments()
    {
        bool isDone = false;
        lookForWhatIsWrong([&] {
            CallArguments arguments;
            if (!parseArgs(arguments) || !arguments.hasKeywordSection || !at(TokenKind::Comma))
                return false;
            const Token& comma = next();
            bool hasAny = false;
            while (true) {
                unsigned index = m_index;
                if (hasAny && !consume(TokenKind::Comma))
                    break;
                if (!parseStarredExpression() || at(TokenKind::Equal)) {
                    m_index = index;
                    break;
                }
                hasAny = true;
            }
            if (hasAny)
                failStartingFrom("iterable argument unpacking follows keyword argument unpacking"_s, comma);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            Expression* element = parseExpression();
            Vector<Comprehension*, 2> generators;
            if (!element || !parseSomeComprehensionClauses(generators) || !consume(TokenKind::Comma))
                return false;
            // [args | expression for_if_clauses]
            unsigned index = m_index;
            CallArguments rest;
            if (!parseArgs(rest) && !m_error) {
                m_index = index;
                Vector<Comprehension*, 2> others;
                if (!parseExpression() || !parseSomeComprehensionClauses(others))
                    m_index = index;
            }
            fail("Generator expression must be parenthesized"_s, *element, lastItemOf(generators));
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            if (!at(TokenKind::Name) || !atAhead(1, TokenKind::Equal))
                return false;
            const Token& name = next();
            const Token& equal = next();
            Vector<Comprehension*, 2> generators;
            if (parseExpression() && parseSomeComprehensionClauses(generators))
                fail("invalid syntax. Maybe you meant '==' or ':=' instead of '='?"_s, name, equal);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            // (args ',')?
            unsigned index = m_index;
            CallArguments arguments;
            if (!parseArgs(arguments) || !consume(TokenKind::Comma))
                m_index = index;
            if (m_error || !at(TokenKind::Name) || !atAhead(1, TokenKind::Equal))
                return false;
            const Token& name = next();
            const Token& equal = next();
            if (at(TokenKind::Comma) || at(TokenKind::RightParenthesis))
                fail("expected argument value expression"_s, name, equal);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            CallArguments arguments;
            Vector<Comprehension*, 2> generators;
            if (!parseArgs(arguments) || !parseSomeComprehensionClauses(generators))
                return false;
            // _PyPegen_nonparen_genexp_in_call()
            isDone = true;
            if (arguments.positional.size() > 1)
                fail("Generator expression must be parenthesized"_s, *arguments.positional.last(), lastItemOf(generators));
            return false;
        });
        if (m_error || isDone)
            return;
        lookForWhatIsWrong([&] {
            CallArguments arguments;
            if (!parseArgs(arguments) || !consume(TokenKind::Comma))
                return false;
            Expression* element = parseExpression();
            Vector<Comprehension*, 2> generators;
            if (element && parseSomeComprehensionClauses(generators))
                fail("Generator expression must be parenthesized"_s, *element, lastItemOf(generators));
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            CallArguments arguments;
            CallArguments rest;
            if (!parseArgs(arguments) || !consume(TokenKind::Comma) || !parseArgs(rest))
                return false;
            // _PyPegen_arguments_parsing_error()
            bool hasUnpacking = std::ranges::any_of(arguments.keywords, [] (Keyword* keyword) { return !keyword->name; });
            failAtLastToken(hasUnpacking ? "positional argument follows keyword argument unpacking"_s : "positional argument follows keyword argument"_s);
            return false;
        });
    }

    // genexp | '(' [arguments] ')'. What a class is derived from is not the first of those.
    enum class GeneratorIs : bool { NotAllowed, Allowed };
    bool parseArguments(Vector<Expression*, 8>& arguments, Vector<Keyword*, 8>& keywords, GeneratorIs generatorIs = GeneratorIs::Allowed)
    {
        Mark open = mark();
        unsigned openIndex = m_index;
        next();
        if (consume(TokenKind::RightParenthesis))
            return true;

        Parsed first;
        if (generatorIs == GeneratorIs::Allowed) {
            first.start = m_index;
            first.expression = at(TokenKind::Star) || at(TokenKind::DoubleStar) ? nullptr : parseAssignmentExpressionOrExpression();
            first.end = m_index;
            if (first.expression && atComprehension()) {
                // f(x for x in y): the parentheses of the call are those of the generator expression.
                Vector<Comprehension*, 2> generators;
                if (parseComprehensionClauses(generators) && consume(TokenKind::RightParenthesis)) {
                    auto* generator = make<GeneratorExp>(open);
                    generator->element = first.expression;
                    generator->generators = m_arena.copy(generators);
                    arguments.append(generator);
                    return true;
                }
            }
            if (m_error)
                return false;
            if (m_callsInvalidRules) {
                m_index = openIndex;
                recognizeInvalidComprehension();
                if (m_error)
                    return false;
            }
            m_index = openIndex + 1;
        }

        // What has once been found not to be arguments is not gone into again. That may have been found while what is wrong was not being looked for, and then it never is: CPython keeps what
        // comes of this rule of its grammar, and does not keep how it was come by.
        if (m_isSecondPass && m_notArguments.contains(openIndex))
            return false;
        CallArguments result;
        if (parseArgs(result, first)) {
            consume(TokenKind::Comma);
            if (consume(TokenKind::RightParenthesis)) {
                arguments = WTF::move(result.positional);
                keywords = WTF::move(result.keywords);
                return true;
            }
        }
        if (m_callsInvalidRules && !m_error) {
            m_index = openIndex + 1;
            recognizeInvalidArguments();
        }
        if (m_isSecondPass)
            m_notArguments.add(openIndex);
        return false;
    }

    // invalid_comprehension, at the bracket
    void recognizeInvalidComprehension()
    {
        bool isParenthesis = at(TokenKind::LeftParenthesis);
        lookForWhatIsWrong([&] {
            next();
            Expression* starred = parseStarredExpression();
            Vector<Comprehension*, 2> generators;
            if (starred && parseSomeComprehensionClauses(generators))
                fail("iterable unpacking cannot be used in comprehension"_s, *starred);
            return false;
        });
        if (m_error || isParenthesis)
            return;
        lookForWhatIsWrong([&] {
            next();
            Expression* first = parseStarNamedExpression();
            if (!first || !at(TokenKind::Comma))
                return false;
            const Token& comma = next();
            unsigned afterComma = m_index;
            // star_named_expressions
            Expression* last = nullptr;
            while (true) {
                unsigned index = m_index;
                if (last && !consume(TokenKind::Comma))
                    break;
                Expression* element = parseStarNamedExpression();
                if (!element) {
                    // The last comma may have nothing after it.
                    if (!last)
                        m_index = index;
                    break;
                }
                last = element;
            }
            if (m_error)
                return false;
            Vector<Comprehension*, 2> generators;
            if (last && parseSomeComprehensionClauses(generators)) {
                fail("did you forget parentheses around the comprehension target?"_s, *first, *last);
                return false;
            }
            if (m_error)
                return false;
            m_index = afterComma;
            generators.clear();
            if (parseSomeComprehensionClauses(generators))
                fail("did you forget parentheses around the comprehension target?"_s, *first, comma);
            return false;
        });
    }

    // slices
    Expression* parseSlices()
    {
        Mark start = mark();
        Expression* first = parseSliceOrStarred();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma) && !first->is<Starred>())
            return first;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (at(TokenKind::RightBracket))
                break;
            Expression* element = parseSliceOrStarred();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        return tuple;
    }

    Expression* parseSliceOrStarred()
    {
        Mark start = mark();
        if (at(TokenKind::Star))
            return parseStarredExpression();

        Expression* lower = nullptr;
        if (!at(TokenKind::Colon)) {
            lower = parseNamedExpression();
            if (!lower || !at(TokenKind::Colon))
                return lower;
        }
        next();
        auto atEndOfPart = [&] {
            return at(TokenKind::Colon) || at(TokenKind::Comma) || at(TokenKind::RightBracket);
        };
        Expression* upper = nullptr;
        if (!atEndOfPart()) {
            upper = parseExpression();
            if (!upper)
                return nullptr;
        }
        Expression* step = nullptr;
        if (consume(TokenKind::Colon) && !atEndOfPart()) {
            step = parseExpression();
            if (!step)
                return nullptr;
        }
        auto* slice = make<Slice>(start);
        slice->lower = lower;
        slice->upper = upper;
        slice->step = step;
        return slice;
    }

    Constant* makeConstant(const Token& token, Constant::Type type)
    {
        auto* constant = make<Constant>(token);
        constant->type = type;
        return constant;
    }

    Constant* makeNumber(const Token& token)
    {
        auto* constant = make<Constant>(token);
        switch (token.numberKind) {
        case NumberKind::Integer:
            constant->type = Constant::Type::Integer;
            constant->integer = token.integer;
            break;
        case NumberKind::BigInteger:
            constant->type = Constant::Type::BigInteger;
            constant->text = token.text;
            constant->radix = token.radix;
            break;
        case NumberKind::Float:
            constant->type = Constant::Type::Float;
            constant->real = token.real;
            break;
        case NumberKind::Imaginary:
            constant->type = Constant::Type::Imaginary;
            constant->real = token.real;
            break;
        }
        return constant;
    }

    // atom
    Expression* parseAtom()
    {
        switch (peek().kind) {
        case TokenKind::Name:
            return makeName(next());
        case TokenKind::KeywordTrue:
            return makeConstant(next(), Constant::Type::True);
        case TokenKind::KeywordFalse:
            return makeConstant(next(), Constant::Type::False);
        case TokenKind::KeywordNone:
            return makeConstant(next(), Constant::Type::None);
        case TokenKind::Ellipsis:
            return makeConstant(next(), Constant::Type::Ellipsis);
        case TokenKind::Number:
            return makeNumber(next());
        case TokenKind::String:
        case TokenKind::FStringStart:
        case TokenKind::TStringStart:
            return parseStrings();
        case TokenKind::LeftParenthesis:
            return parseParenthesized();
        case TokenKind::LeftBracket:
            return parseListDisplay();
        case TokenKind::LeftBrace:
            return parseBraceDisplay();
        default:
            return nullptr;
        }
    }

    // tuple | group | genexp
    Expression* parseParenthesized()
    {
        if (!isSafeToRecurse())
            return nullptr;
        unsigned index = m_index;
        Expression* result = parseParenthesizedItself();
        if (result || !m_callsInvalidRules || m_error)
            return result;
        // invalid_group
        m_index = index + 1;
        if (at(TokenKind::Star)) {
            Expression* starred = parseStarredExpression();
            if (starred && at(TokenKind::RightParenthesis))
                return fail("cannot use starred expression here"_s, *starred);
        } else if (at(TokenKind::DoubleStar)) {
            const Token& stars = next();
            if (parseExpression() && at(TokenKind::RightParenthesis))
                return fail("cannot use double starred expression here"_s, stars);
        }
        if (m_error)
            return nullptr;
        m_index = index;
        recognizeInvalidComprehension();
        return nullptr;
    }

    Expression* parseParenthesizedItself()
    {
        Mark start = mark();
        next();
        if (consume(TokenKind::RightParenthesis))
            return make<Tuple>(start);

        if (at(TokenKind::KeywordYield)) {
            Expression* yield = parseYield();
            if (!yield || !expect(TokenKind::RightParenthesis))
                return nullptr;
            yield->isParenthesized = true;
            return yield;
        }

        Expression* first = parseStarNamedExpression();
        if (!first)
            return nullptr;

        if (atComprehension()) {
            if (first->is<Starred>())
                return nullptr;
            Vector<Comprehension*, 2> generators;
            if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightParenthesis))
                return nullptr;
            auto* generator = make<GeneratorExp>(start);
            generator->element = first;
            generator->generators = m_arena.copy(generators);
            return generator;
        }

        if (at(TokenKind::RightParenthesis)) {
            if (first->is<Starred>())
                return nullptr;
            next();
            first->isParenthesized = true;
            return first;
        }

        if (!at(TokenKind::Comma))
            return nullptr;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (at(TokenKind::RightParenthesis))
                break;
            Expression* element = parseStarNamedExpression();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        if (!expect(TokenKind::RightParenthesis))
            return nullptr;
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        tuple->isParenthesized = true;
        return tuple;
    }

    // list | listcomp
    Expression* parseListDisplay()
    {
        if (!isSafeToRecurse())
            return nullptr;
        unsigned index = m_index;
        Expression* result = parseListDisplayItself();
        if (result || !m_callsInvalidRules || m_error)
            return result;
        m_index = index;
        recognizeInvalidComprehension();
        return nullptr;
    }

    Expression* parseListDisplayItself()
    {
        Mark start = mark();
        next();
        Vector<Expression*, 8> elements;
        while (!at(TokenKind::RightBracket)) {
            Expression* element = parseStarNamedExpression();
            if (!element)
                return nullptr;
            if (elements.isEmpty() && atComprehension()) {
                if (element->is<Starred>())
                    return nullptr;
                Vector<Comprehension*, 2> generators;
                if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightBracket))
                    return nullptr;
                auto* comprehension = make<ListComp>(start);
                comprehension->element = element;
                comprehension->generators = m_arena.copy(generators);
                return comprehension;
            }
            elements.append(element);
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightBracket))
            return nullptr;
        auto* list = make<List>(start);
        list->elements = m_arena.copy(elements);
        return list;
    }

    // dict | set | dictcomp | setcomp
    Expression* parseBraceDisplay()
    {
        if (!isSafeToRecurse())
            return nullptr;
        unsigned index = m_index;
        Expression* result = parseBraceDisplayItself();
        if (result || m_error)
            return result;
        // What CPython generates from its grammar leaves out, the first time, whatever alternative begins with something that is there only to be recognized as a mistake. This one begins with
        // the brace. So it is tried both times, and the first time nothing else is looked for on the way.
        // From here on the same things are parsed over and over.
        m_keepsWhatIsParsed = true;
        m_index = index + 1;
        recognizeInvalidPairs();
        if (m_error || !m_callsInvalidRules)
            return nullptr;
        // invalid_dict_comprehension
        m_index = index + 1;
        if (at(TokenKind::DoubleStar)) {
            const Token& stars = next();
            Vector<Comprehension*, 2> generators;
            if (parseBitwiseOr() && parseSomeComprehensionClauses(generators) && at(TokenKind::RightBrace))
                return fail("dict unpacking cannot be used in dict comprehension"_s, stars);
            if (m_error)
                return nullptr;
        }
        m_index = index;
        recognizeInvalidComprehension();
        return nullptr;
    }

    // The last two of invalid_kvpair, which are the last two of invalid_double_starred_kvpairs as well
    void recognizeInvalidValue()
    {
        lookForWhatIsWrong([&] {
            if (!parseExpression() || !consume(TokenKind::Colon) || !at(TokenKind::Star))
                return false;
            const Token& star = next();
            if (parseBitwiseOr()) {
                SetForScope scope(m_failsInEitherPass, true);
                failStartingFrom("cannot use a starred expression in a dictionary value"_s, star);
            }
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            if (!parseExpression() || !at(TokenKind::Colon))
                return false;
            const Token& colon = next();
            if (at(TokenKind::RightBrace) || at(TokenKind::Comma))
                failInEitherPass("expression expected after dictionary key and ':'"_s, colon);
            return false;
        });
    }

    // invalid_double_starred_kvpairs
    void recognizeInvalidPairs()
    {
        lookForWhatIsWrong([&] {
            // ','.double_starred_kvpair+ ','
            bool hasAny = false;
            while (true) {
                unsigned index = m_index;
                if (hasAny && !consume(TokenKind::Comma))
                    break;
                bool isPair = consume(TokenKind::DoubleStar) ? !!parseBitwiseOr() : parseExpression() && consume(TokenKind::Colon) && parseExpression();
                if (!isPair) {
                    m_index = index;
                    break;
                }
                hasAny = true;
            }
            if (m_error || !hasAny || !consume(TokenKind::Comma))
                return false;
            // invalid_kvpair
            lookForWhatIsWrong([&] {
                Expression* key = parseExpression();
                if (key && !at(TokenKind::Colon))
                    failInEitherPass("':' expected after dictionary key"_s, key->line, static_cast<int>(key->endColumn) - 1, key->endLine, -1);
                return false;
            });
            if (!m_error)
                recognizeInvalidValue();
            return false;
        });
        if (!m_error)
            recognizeInvalidValue();
    }

    Expression* parseBraceDisplayItself()
    {
        Mark start = mark();
        next();
        if (consume(TokenKind::RightBrace))
            return make<Dict>(start);

        Expression* first = nullptr;
        if (!at(TokenKind::DoubleStar)) {
            first = parseStarNamedExpression();
            if (!first)
                return nullptr;
        }

        if (first && !at(TokenKind::Colon)) {
            if (atComprehension()) {
                if (first->is<Starred>())
                    return nullptr;
                Vector<Comprehension*, 2> generators;
                if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightBrace))
                    return nullptr;
                auto* comprehension = make<SetComp>(start);
                comprehension->element = first;
                comprehension->generators = m_arena.copy(generators);
                return comprehension;
            }
            Vector<Expression*, 8> elements;
            elements.append(first);
            while (consume(TokenKind::Comma)) {
                if (at(TokenKind::RightBrace))
                    break;
                Expression* element = parseStarNamedExpression();
                if (!element)
                    return nullptr;
                elements.append(element);
            }
            if (!expect(TokenKind::RightBrace))
                return nullptr;
            auto* set = make<Set>(start);
            set->elements = m_arena.copy(elements);
            return set;
        }

        Vector<Expression*, 8> keys;
        Vector<Expression*, 8> values;
        Expression* key = first;
        while (true) {
            if (!key && at(TokenKind::DoubleStar)) {
                next();
                Expression* value = parseBitwiseOr();
                if (!value)
                    return nullptr;
                keys.append(nullptr);
                values.append(value);
            } else {
                if (!key) {
                    key = parseExpression();
                    if (!key)
                        return nullptr;
                }
                if (key->is<Starred>() || (key->is<NamedExpr>() && !key->isParenthesized))
                    return nullptr;
                if (!consume(TokenKind::Colon))
                    return nullptr;
                Expression* value = parseExpression();
                if (!value)
                    return nullptr;
                if (keys.isEmpty() && atComprehension()) {
                    Vector<Comprehension*, 2> generators;
                    if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightBrace))
                        return nullptr;
                    auto* comprehension = make<DictComp>(start);
                    comprehension->key = key;
                    comprehension->value = value;
                    comprehension->generators = m_arena.copy(generators);
                    return comprehension;
                }
                keys.append(key);
                values.append(value);
                key = nullptr;
            }
            if (!consume(TokenKind::Comma) || at(TokenKind::RightBrace))
                break;
        }
        if (!expect(TokenKind::RightBrace))
            return nullptr;
        auto* dict = make<Dict>(start);
        dict->keys = m_arena.copy(keys);
        dict->values = m_arena.copy(values);
        return dict;
    }

    // invalid_for_target, after the `for`
    void recognizeInvalidForTarget(unsigned index)
    {
        m_index = index;
        if (Expression* target = parseStarExpressions())
            failForInvalidTarget(*target, Targets::For);
    }

    // for_if_clauses
    bool parseComprehensionClauses(Vector<Comprehension*, 2>& generators)
    {
        while (atComprehension()) {
            auto* comprehension = m_arena.create<Comprehension>();
            comprehension->isAsync = consume(TokenKind::KeywordAsync);
            next();
            unsigned index = m_index;
            comprehension->target = parseStarTargets();
            if (!comprehension->target || !consume(TokenKind::KeywordIn)) {
                if (!m_callsInvalidRules || m_error)
                    return false;
                // invalid_for_if_clause
                m_index = index;
                bool hasVariables = false;
                while (parseBitwiseOr()) {
                    hasVariables = true;
                    if (!consume(TokenKind::Comma))
                        break;
                }
                if (m_error)
                    return false;
                if (hasVariables && !at(TokenKind::KeywordIn)) {
                    failAtLastToken("'in' expected after for-loop variables"_s);
                    return false;
                }
                recognizeInvalidForTarget(index);
                return false;
            }
            comprehension->iterable = parseDisjunction();
            if (!comprehension->iterable)
                return false;
            Vector<Expression*, 2> conditions;
            while (consume(TokenKind::KeywordIf)) {
                Expression* condition = parseDisjunction();
                if (!condition)
                    return false;
                conditions.append(condition);
            }
            comprehension->conditions = m_arena.copy(conditions);
            generators.append(comprehension);
        }
        return true;
    }

    // ---- Strings

    static bool isEmptyString(Expression& expression)
    {
        auto* constant = expression.tryAs<Constant>();
        return constant && constant->type == Constant::Type::String && constant->text->isEmpty();
    }

    Constant* makeStringConstant(const Token& token)
    {
        auto* constant = make<Constant>(token);
        constant->type = token.isBytes ? Constant::Type::Bytes : Constant::Type::String;
        constant->text = token.text;
        constant->hasUnicodePrefix = token.hasUnicodePrefix;
        return constant;
    }

    // Constants that stand next to each other are one constant.
    Constant* joinConstants(std::span<Expression*> constants, const Node& first, const Node& last)
    {
        StringBuilder builder;
        for (Expression* constant : constants)
            builder.append(constant->as<Constant>().text->string());
        auto* result = m_arena.create<Constant>();
        setRange(*result, first, last);
        result->type = constants[0]->as<Constant>().type;
        result->hasUnicodePrefix = constants[0]->as<Constant>().hasUnicodePrefix;
        result->text = makeIdentifier(builder.toString());
        return result;
    }

    // The pieces of strings that stand next to each other, at least one of which has expressions in it.
    Sequence<Expression*> joinPieces(const Vector<Expression*, 8>& strings)
    {
        Vector<Expression*, 16> flattened;
        for (Expression* string : strings) {
            if (auto* joined = string->tryAs<JoinedStr>())
                flattened.append(joined->values);
            else if (auto* templateString = string->tryAs<TemplateStr>())
                flattened.append(templateString->values);
            else
                flattened.append(string);
        }

        Vector<Expression*, 16> values;
        for (unsigned i = 0; i < flattened.size(); ++i) {
            Expression* piece = flattened[i];
            if (piece->is<Constant>()) {
                unsigned end = i + 1;
                while (end < flattened.size() && flattened[end]->is<Constant>())
                    ++end;
                if (end - i > 1) {
                    piece = joinConstants(flattened.mutableSpan().subspan(i, end - i), *flattened[i], *flattened[end - 1]);
                    i = end - 1;
                }
                if (isEmptyString(*piece))
                    continue;
            }
            values.append(piece);
        }
        return m_arena.copy(values);
    }

    // What _PyPegen_concatenate_strings does, of pieces that span from `first` to `last`.
    Expression* concatenateStrings(const Vector<Expression*, 8>& strings, const Node& first, const Node& last)
    {
        bool hasExpressions = false;
        bool hasText = false;
        bool hasBytes = false;
        for (Expression* string : strings) {
            if (auto* constant = string->tryAs<Constant>())
                (constant->type == Constant::Type::Bytes ? hasBytes : hasText) = true;
            else
                hasExpressions = true;
        }
        if ((hasText || hasExpressions) && hasBytes)
        {
            SetForScope scope(m_failsInEitherPass, true);
            return failAtLastToken("cannot mix bytes and nonbytes literals"_s);
        }

        if (!hasExpressions) {
            if (strings.size() == 1)
                return strings[0];
            Vector<Expression*, 8> copy = strings;
            return joinConstants(copy.mutableSpan(), first, last);
        }
        auto* joined = m_arena.create<JoinedStr>();
        setRange(*joined, first, last);
        joined->values = joinPieces(strings);
        return joined;
    }

    // strings
    Expression* parseStrings()
    {
        return memoized(m_memos.strings, [&] { return parseStringsItself(); });
    }

    Expression* parseStringsItself()
    {
        Vector<Expression*, 8> strings;
        bool isTemplate = at(TokenKind::TStringStart);
        while (true) {
            Expression* string = nullptr;
            if (at(TokenKind::String) && !isTemplate)
                string = makeStringConstant(next());
            else if (at(TokenKind::FStringStart) && !isTemplate)
                string = parseStringWithExpressions(false);
            else if (at(TokenKind::TStringStart) && isTemplate)
                string = parseStringWithExpressions(true);
            else if (at(TokenKind::String) || at(TokenKind::FStringStart) || at(TokenKind::TStringStart)) {
                Expression* other = at(TokenKind::String) ? makeStringConstant(next()) : parseStringWithExpressions(at(TokenKind::TStringStart));
                if (!other)
                    return nullptr;
                return fail("cannot mix t-string literals with string or bytes literals"_s, *strings.last(), *other);
            } else
                break;
            if (!string)
                return nullptr;
            strings.append(string);
        }

        if (!isTemplate)
            return concatenateStrings(strings, *strings.first(), *strings.last());
        auto* result = m_arena.create<TemplateStr>();
        setRange(*result, *strings.first(), *strings.last());
        result->values = joinPieces(strings);
        return result;
    }

    // fstring | tstring
    Expression* parseStringWithExpressions(bool isTemplate)
    {
        const Token& open = next();
        TokenKind middle = isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle;
        TokenKind close = isTemplate ? TokenKind::TStringEnd : TokenKind::FStringEnd;

        Vector<Expression*, 8> values;
        const Identifier* decodingError = nullptr;
        while (!at(close)) {
            if (at(middle)) {
                if (peek().hasDecodingError && !decodingError)
                    decodingError = peek().text;
                Constant* text = makeStringConstant(next());
                if (!isEmptyString(*text))
                    values.append(text);
                continue;
            }
            if (!at(TokenKind::LeftBrace))
                return nullptr;
            if (!parseReplacementField(isTemplate, isTemplate, values))
                return nullptr;
        }
        const Token& end = next();
        if (decodingError)
            return failInEitherPass(String(decodingError->string()), end);

        if (isTemplate) {
            auto* result = m_arena.create<TemplateStr>();
            setRange(*result, open, end);
            result->values = m_arena.copy(values);
            return result;
        }
        auto* result = m_arena.create<JoinedStr>();
        setRange(*result, open, end);
        result->values = m_arena.copy(values);
        return result;
    }

    String prefixed(bool isTemplate, ASCIILiteral message)
    {
        return concatenate(isTemplate ? 't' : 'f', "-string: "_s, message);
    }

    // fstring_replacement_field and its like. `isTemplate` is what kind of string this is in, for what to say when it is wrong, and
    // `makesInterpolation` is whether it is at the top of a t-string, and not in a format specification.
    bool parseReplacementField(bool isTemplate, bool makesInterpolation, Vector<Expression*, 8>& values)
    {
        if (!isSafeToRecurse())
            return false;
        Mark start = mark();
        next();

        switch (peek().kind) {
        case TokenKind::Equal:
            fail(prefixed(isTemplate, "valid expression required before '='"_s));
            return false;
        case TokenKind::Exclamation:
            fail(prefixed(isTemplate, "valid expression required before '!'"_s));
            return false;
        case TokenKind::Colon:
            fail(prefixed(isTemplate, "valid expression required before ':'"_s));
            return false;
        case TokenKind::RightBrace:
            fail(prefixed(isTemplate, "valid expression required before '}'"_s));
            return false;
        default:
            break;
        }

        const Token& first = peek();
        Expression* value = parseYieldOrStarExpressions();
        if (!value) {
            fail(prefixed(isTemplate, "expecting a valid expression after '{'"_s), first);
            return false;
        }

        bool isDebug = consume(TokenKind::Equal);

        // Whichever token comes right after the expression has its source.
        const Identifier* source = nullptr;
        unsigned debugEndLine = 0;
        unsigned debugEndColumn = 0;
        unsigned debugEnd = 0;

        int conversion = -1;
        bool hasConversion = at(TokenKind::Exclamation);
        if (hasConversion) {
            const Token& exclamation = next();
            if (!at(TokenKind::Name)) {
                fail(prefixed(isTemplate, at(TokenKind::Colon) || at(TokenKind::RightBrace) ? "missing conversion character"_s : "invalid conversion character"_s));
                return false;
            }
            const Token& name = next();
            if (name.start != exclamation.end) {
                failInEitherPass(prefixed(isTemplate, "conversion type must come right after the exclamation mark"_s), exclamation, name);
                return false;
            }
            StringView text = name.text->string();
            if (text.length() != 1 || (text[0] != 's' && text[0] != 'r' && text[0] != 'a')) {
                failInEitherPass(concatenate(isTemplate ? 't' : 'f', "-string: invalid conversion character '"_s, text, "': expected 's', 'r', or 'a'"_s), name);
                return false;
            }
            conversion = text[0];
            source = exclamation.expressionSource;
            debugEndLine = name.line;
            debugEndColumn = name.column;
            debugEnd = name.start;
        }

        Expression* formatSpecification = nullptr;
        if (at(TokenKind::Colon)) {
            const Token& colon = next();
            formatSpecification = parseFormatSpecification(isTemplate, colon);
            if (!formatSpecification)
                return false;
            if (!source) {
                source = colon.expressionSource;
                debugEndLine = formatSpecification->line;
                debugEndColumn = formatSpecification->column + 1;
                debugEnd = formatSpecification->start + 1;
            }
        } else if (isDebug && conversion == -1)
            conversion = 'r';

        if (!at(TokenKind::RightBrace)) {
            fail(prefixed(isTemplate, formatSpecification ? "expecting '}', or format specs"_s : hasConversion ? "expecting ':' or '}'"_s : isDebug ? "expecting '!', or ':', or '}'"_s : "expecting '=', or '!', or ':', or '}'"_s));
            return false;
        }
        const Token& close = next();
        if (!source) {
            source = close.expressionSource;
            debugEndLine = close.endLine;
            debugEndColumn = close.endColumn;
            debugEnd = close.end;
        }

        Expression* result;
        if (makesInterpolation) {
            auto* interpolation = make<Interpolation>(start);
            interpolation->value = value;
            interpolation->conversion = conversion;
            interpolation->formatSpecification = formatSpecification;
            // Without what is after it: white space, and the = of t"{x=}".
            StringView text = source->string();
            unsigned length = text.length();
            while (length && (text[length - 1] == '=' || isPythonWhitespace(text[length - 1])))
                --length;
            auto* sourceText = m_arena.create<Constant>();
            sourceText->type = Constant::Type::String;
            sourceText->text = makeIdentifier(text.left(length).toString());
            interpolation->source = sourceText;
            result = interpolation;
        } else {
            auto* formatted = make<FormattedValue>(start);
            formatted->value = value;
            formatted->conversion = conversion;
            formatted->formatSpecification = formatSpecification;
            result = formatted;
        }

        if (isDebug) {
            // f"{x=}" is "x=" and then x.
            auto* text = m_arena.create<Constant>();
            text->type = Constant::Type::String;
            text->text = source;
            text->line = start.line;
            text->column = start.column + 1;
            text->start = start.start + 1;
            text->endLine = debugEndLine;
            text->endColumn = debugEndColumn - 1;
            text->end = debugEnd - 1;
            values.append(text);
        }
        values.append(result);
        return true;
    }

    static bool isPythonWhitespace(char16_t c)
    {
        return Unicode::isWhitespace(c);
    }

    // fstring_full_format_spec, after its colon.
    Expression* parseFormatSpecification(bool isTemplate, const Token& colon)
    {
        TokenKind middle = isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle;
        Vector<Expression*, 8> pieces;
        while (true) {
            if (at(middle)) {
                if (peek().hasDecodingError)
                    return failInEitherPass(String(peek().text->string()), peek());
                Constant* text = makeStringConstant(next());
                if (!isEmptyString(*text))
                    pieces.append(text);
                continue;
            }
            if (!at(TokenKind::LeftBrace))
                break;
            // What f"{x=}" adds is kept together with it here, and taken apart when the pieces are joined.
            Vector<Expression*, 8> field;
            if (!parseReplacementField(isTemplate, false, field))
                return nullptr;
            if (field.size() == 1) {
                pieces.append(field[0]);
                continue;
            }
            auto* pair = m_arena.create<JoinedStr>();
            setRange(*pair, *field[1], *field[1]);
            pair->values = m_arena.copy(field);
            pieces.append(pair);
        }

        Node range;
        setRange(range, colon, previous());
        if (pieces.isEmpty() || (pieces.size() == 1 && pieces[0]->is<Constant>())) {
            auto* joined = m_arena.create<JoinedStr>();
            setRange(*joined, range, range);
            joined->values = m_arena.copy(pieces);
            return joined;
        }
        return concatenateStrings(pieces, range, range);
    }

    // ---- Parameters

    // param, and lambda_param
    Argument* parseParameter(bool allowsAnnotation, bool allowsStarAnnotation = false)
    {
        if (!at(TokenKind::Name))
            return nullptr;
        Mark start = mark();
        const Identifier* name = next().text;
        Expression* annotation = nullptr;
        if (allowsAnnotation && consume(TokenKind::Colon)) {
            annotation = allowsStarAnnotation ? parseStarExpression() : parseExpression();
            if (!annotation)
                return nullptr;
        }
        auto* argument = make<Argument>(start);
        argument->name = name;
        argument->annotation = annotation;
        return argument;
    }

    // parameters and lambda_parameters, which may be none at all. `terminator` is what comes after them.
    Arguments* parseParameters(TokenKind terminator, bool allowsAnnotations)
    {
        Vector<Argument*, 8> positionalOnly;
        Vector<Argument*, 8> positional;
        Vector<Argument*, 8> keywordOnly;
        Vector<Expression*, 8> defaults;
        Vector<Expression*, 8> keywordDefaults;
        auto* arguments = m_arena.create<Arguments>();
        bool sawSlash = false;
        bool sawStar = false;

        while (!at(terminator)) {
            // What a comment that comes next says the type of.
            Argument* parameter = nullptr;
            if (at(TokenKind::Slash)) {
                if (sawSlash)
                    return fail("/ may appear only once"_s);
                if (sawStar)
                    return fail("/ must be ahead of *"_s);
                if (positional.isEmpty()) {
                    if (m_callsInvalidRules && atAhead(1, TokenKind::Comma))
                        fail("at least one argument must precede /"_s);
                    return nullptr;
                }
                next();
                sawSlash = true;
                positionalOnly = std::exchange(positional, { });
                if (at(TokenKind::Star))
                    return fail("expected comma between / and *"_s);
            } else if (at(TokenKind::Star)) {
                if (sawStar) {
                    // It has to be followed as the first was.
                    const Token& star = next();
                    if (!at(TokenKind::Comma)) {
                        if (!parseParameter(allowsAnnotations) || (!at(TokenKind::Comma) && !at(terminator)))
                            return nullptr;
                    }
                    return fail("* argument may appear only once"_s, star);
                }
                const Token& star = next();
                sawStar = true;
                // Of a lambda it is said to be wherever the parser has got to.
                auto failForBareStar = [&] {
                    if (allowsAnnotations)
                        return fail("named arguments must follow bare *"_s, star);
                    return failAtLastToken("named arguments must follow bare *"_s);
                };
                if (at(TokenKind::Comma)) {
                    if (atAhead(1, terminator) || atAhead(1, TokenKind::DoubleStar))
                        return failForBareStar();
                    if (allowsAnnotations && atAhead(1, TokenKind::TypeComment)) {
                        m_index += 2;
                        return failAtLastToken("bare * has associated type comment"_s);
                    }
                } else {
                    if (at(terminator))
                        return failForBareStar();
                    arguments->variadic = parseParameter(allowsAnnotations, true);
                    if (!arguments->variadic)
                        return nullptr;
                    if (at(TokenKind::Equal)) {
                        if (arguments->variadic->annotation && arguments->variadic->annotation->is<Starred>())
                            return nullptr;
                        return fail("var-positional argument cannot have default value"_s);
                    }
                    parameter = arguments->variadic;
                }
            } else if (at(TokenKind::DoubleStar)) {
                next();
                arguments->keywordVariadic = parseParameter(allowsAnnotations);
                if (!arguments->keywordVariadic)
                    return nullptr;
                if (at(TokenKind::Equal))
                    return fail("var-keyword argument cannot have default value"_s);
                bool hasComma = consume(TokenKind::Comma);
                if (allowsAnnotations && at(TokenKind::TypeComment) && (hasComma || atAhead(1, terminator)))
                    arguments->keywordVariadic->typeComment = parseTypeComment();
                if (hasComma && !at(terminator)) {
                    if (at(TokenKind::Star) || at(TokenKind::DoubleStar) || at(TokenKind::Slash))
                        return fail("arguments cannot follow var-keyword argument"_s);
                    if (Argument* argument = parseParameter(allowsAnnotations))
                        return fail("arguments cannot follow var-keyword argument"_s, *argument);
                    return nullptr;
                }
                break;
            } else {
                if (at(TokenKind::LeftParenthesis)) {
                    // Only after parameters that are no more than names, and with nothing but such between the parentheses.
                    if (!m_callsInvalidRules || sawSlash || sawStar || !defaults.isEmpty())
                        return nullptr;
                    const Token& open = next();
                    bool hasAny = false;
                    while (parseParameter(allowsAnnotations)) {
                        hasAny = true;
                        if (!consume(TokenKind::Comma))
                            break;
                    }
                    if (hasAny && at(TokenKind::RightParenthesis))
                        fail(allowsAnnotations ? "Function parameters cannot be parenthesized"_s : "Lambda expression parameters cannot be parenthesized"_s, open, peek());
                    return nullptr;
                }
                Argument* argument = parseParameter(allowsAnnotations);
                if (!argument)
                    return nullptr;
                Expression* defaultValue = nullptr;
                if (consume(TokenKind::Equal)) {
                    if (at(TokenKind::Comma) || at(TokenKind::RightParenthesis))
                        return fail("expected default value expression"_s, previous());
                    defaultValue = parseExpression();
                    if (!defaultValue)
                        return nullptr;
                }
                if (sawStar) {
                    keywordOnly.append(argument);
                    keywordDefaults.append(defaultValue);
                } else {
                    if (defaultValue)
                        defaults.append(defaultValue);
                    else if (!defaults.isEmpty()) {
                        if (at(TokenKind::Comma) || at(terminator))
                            fail("parameter without a default follows parameter with a default"_s, *argument);
                        return nullptr;
                    }
                    positional.append(argument);
                }
                parameter = argument;
            }
            bool hasComma = consume(TokenKind::Comma);
            if (allowsAnnotations && parameter && at(TokenKind::TypeComment) && (hasComma || atAhead(1, terminator)))
                parameter->typeComment = parseTypeComment();
            if (!hasComma)
                break;
        }

        arguments->positionalOnly = m_arena.copy(positionalOnly);
        arguments->positional = m_arena.copy(positional);
        arguments->keywordOnly = m_arena.copy(keywordOnly);
        arguments->defaults = m_arena.copy(defaults);
        arguments->keywordDefaults = m_arena.copy(keywordDefaults);
        return arguments;
    }

    // type_params, if there are any.
    bool parseTypeParameters(Sequence<TypeParameter*>& result)
    {
        if (!at(TokenKind::LeftBracket))
            return true;
        next();
        if (at(TokenKind::RightBracket)) {
            failStartingFrom("Type parameter list cannot be empty"_s, peek());
            return false;
        }
        Vector<TypeParameter*, 4> parameters;
        while (!at(TokenKind::RightBracket)) {
            Mark start = mark();
            auto kind = TypeParameter::Kind::TypeVar;
            if (consume(TokenKind::Star))
                kind = TypeParameter::Kind::TypeVarTuple;
            else if (consume(TokenKind::DoubleStar))
                kind = TypeParameter::Kind::ParamSpec;
            if (!at(TokenKind::Name))
                return false;
            const Identifier* name = next().text;
            Expression* bound = nullptr;
            if (at(TokenKind::Colon)) {
                const Token& colon = next();
                bound = parseExpression();
                if (!bound)
                    return false;
                if (kind != TypeParameter::Kind::TypeVar) {
                    ASCIILiteral what = bound->is<Tuple>() ? "constraints"_s : "bound"_s;
                    failStartingFrom(concatenate("cannot use "_s, what, " with "_s, kind == TypeParameter::Kind::TypeVarTuple ? "TypeVarTuple"_s : "ParamSpec"_s), colon);
                    return false;
                }
            }
            Expression* defaultValue = nullptr;
            if (consume(TokenKind::Equal)) {
                defaultValue = kind == TypeParameter::Kind::TypeVarTuple ? parseStarExpression() : parseExpression();
                if (!defaultValue)
                    return false;
            }
            auto* parameter = make<TypeParameter>(start);
            parameter->kind = kind;
            parameter->name = name;
            parameter->bound = bound;
            parameter->defaultValue = defaultValue;
            parameters.append(parameter);
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightBracket))
            return false;
        result = m_arena.copy(parameters);
        return true;
    }

    // ---- Patterns

    // patterns
    Pattern* parsePatterns()
    {
        Mark start = mark();
        Pattern* first = parseMaybeStarPattern();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma)) {
            if (first->is<MatchStar>())
                return nullptr;
            return first;
        }
        Vector<Pattern*, 8> patterns;
        patterns.append(first);
        if (!parseRestOfSequencePattern(patterns, TokenKind::Colon))
            return nullptr;
        auto* sequence = make<MatchSequence>(start);
        sequence->patterns = m_arena.copy(patterns);
        return sequence;
    }

    // After the first of them: (',' maybe_star_pattern)* [',']
    bool parseRestOfSequencePattern(Vector<Pattern*, 8>& patterns, TokenKind terminator)
    {
        while (consume(TokenKind::Comma)) {
            if (at(terminator) || at(TokenKind::KeywordIf))
                break;
            // The last comma may have nothing after it.
            unsigned index = m_index;
            Pattern* pattern = parseMaybeStarPattern();
            if (!pattern) {
                if (m_error)
                    return false;
                m_index = index;
                break;
            }
            patterns.append(pattern);
        }
        return true;
    }

    // maybe_star_pattern
    Pattern* parseMaybeStarPattern()
    {
        if (!at(TokenKind::Star))
            return parsePattern();
        Mark start = mark();
        next();
        if (!at(TokenKind::Name))
            return nullptr;
        const Token& name = next();
        auto* star = make<MatchStar>(start);
        if (name.softKeyword != SoftKeyword::Underscore)
            star->name = name.text;
        return star;
    }

    // pattern
    Pattern* parsePattern()
    {
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        Pattern* pattern = parseOrPattern();
        if (!pattern || !consume(TokenKind::KeywordAs))
            return pattern;
        // pattern_capture_target
        if (!at(TokenKind::Name) || at(SoftKeyword::Underscore) || atAhead(1, TokenKind::Dot) || atAhead(1, TokenKind::LeftParenthesis) || atAhead(1, TokenKind::Equal)) {
            // invalid_as_pattern
            if (at(SoftKeyword::Underscore))
                return fail("cannot use '_' as a target"_s);
            if (!m_callsInvalidRules)
                return nullptr;
            if (Expression* target = parseExpression())
                fail(concatenate("cannot use "_s, describe(*target), " as pattern target"_s), *target);
            return nullptr;
        }
        const Token& name = next();
        auto* as = make<MatchAs>(start);
        as->pattern = pattern;
        as->name = name.text;
        return as;
    }

    // or_pattern
    Pattern* parseOrPattern()
    {
        Mark start = mark();
        Pattern* first = parseClosedPattern();
        if (!first || !at(TokenKind::VerticalBar))
            return first;
        Vector<Pattern*, 8> patterns;
        patterns.append(first);
        while (consume(TokenKind::VerticalBar)) {
            Pattern* pattern = parseClosedPattern();
            if (!pattern)
                return nullptr;
            patterns.append(pattern);
        }
        auto* orPattern = make<MatchOr>(start);
        orPattern->patterns = m_arena.copy(patterns);
        return orPattern;
    }

    // signed_number, and complex_number if what follows makes it one.
    Expression* parseNumberInPattern()
    {
        Mark start = mark();
        bool isNegative = consume(TokenKind::Minus);
        if (!at(TokenKind::Number))
            return nullptr;
        Expression* number = makeNumber(next());
        if (isNegative) {
            auto* negation = make<UnaryOp>(start);
            negation->op = UnaryOperator::USub;
            negation->operand = number;
            number = negation;
        }
        if (!at(TokenKind::Plus) && !at(TokenKind::Minus))
            return number;

        Constant& real = (isNegative ? number->as<UnaryOp>().operand : number)->as<Constant>();
        if (real.type == Constant::Type::Imaginary)
            return failInEitherPass("real number required in complex literal"_s, real);
        BinaryOperator op = next().kind == TokenKind::Plus ? BinaryOperator::Add : BinaryOperator::Sub;
        if (!at(TokenKind::Number))
            return nullptr;
        Constant* imaginary = makeNumber(next());
        if (imaginary->type != Constant::Type::Imaginary)
            return failInEitherPass("imaginary number required in complex literal"_s, *imaginary);
        auto* complex = make<BinOp>(start);
        complex->left = number;
        complex->op = op;
        complex->right = imaginary;
        return complex;
    }

    // name_or_attr
    Expression* parseNameOrAttribute()
    {
        Mark start = mark();
        Expression* value = makeName(next());
        while (consume(TokenKind::Dot)) {
            if (!at(TokenKind::Name))
                return nullptr;
            const Token& nameToken = next();
            auto* attribute = make<Attribute>(start);
            attribute->value = value;
            attribute->attribute = nameToken.text;
            attribute->attributeStart = nameToken.start;
            value = attribute;
        }
        return value;
    }

    template<typename T>
    Pattern* makeValuePattern(Mark start, T* value)
    {
        if (!value)
            return nullptr;
        auto* pattern = make<MatchValue>(start);
        pattern->value = value;
        return pattern;
    }

    Pattern* makeSingletonPattern(Constant::Type value)
    {
        auto* pattern = make<MatchSingleton>(next());
        pattern->value = value;
        return pattern;
    }

    // closed_pattern
    Pattern* parseClosedPattern()
    {
        Mark start = mark();
        switch (peek().kind) {
        case TokenKind::Number:
        case TokenKind::Minus:
            return makeValuePattern(start, parseNumberInPattern());
        case TokenKind::String:
        case TokenKind::FStringStart:
        case TokenKind::TStringStart:
            return makeValuePattern(start, parseStrings());
        case TokenKind::KeywordNone:
            return makeSingletonPattern(Constant::Type::None);
        case TokenKind::KeywordTrue:
            return makeSingletonPattern(Constant::Type::True);
        case TokenKind::KeywordFalse:
            return makeSingletonPattern(Constant::Type::False);
        case TokenKind::Name:
            return parseNamePattern();
        case TokenKind::LeftParenthesis:
        case TokenKind::LeftBracket:
            return parseGroupOrSequencePattern();
        case TokenKind::LeftBrace:
            return parseMappingPattern();
        default:
            return nullptr;
        }
    }

    // capture_pattern | wildcard_pattern | value_pattern | class_pattern
    Pattern* parseNamePattern()
    {
        Mark start = mark();
        if (!atAhead(1, TokenKind::Dot) && !atAhead(1, TokenKind::LeftParenthesis)) {
            if (atAhead(1, TokenKind::Equal))
                return nullptr;
            const Token& name = next();
            auto* capture = make<MatchAs>(start);
            if (name.softKeyword != SoftKeyword::Underscore)
                capture->name = name.text;
            return capture;
        }

        Expression* value = parseNameOrAttribute();
        if (!value)
            return nullptr;
        if (!at(TokenKind::LeftParenthesis)) {
            if (at(TokenKind::Equal))
                return nullptr;
            return makeValuePattern(start, value);
        }

        next();
        Vector<Pattern*, 8> patterns;
        Vector<const Identifier*, 8> keywordAttributes;
        Vector<Pattern*, 8> keywordPatterns;
        while (!at(TokenKind::RightParenthesis)) {
            if (at(TokenKind::Name) && atAhead(1, TokenKind::Equal)) {
                keywordAttributes.append(next().text);
                next();
                Pattern* pattern = parsePattern();
                if (!pattern)
                    return nullptr;
                keywordPatterns.append(pattern);
            } else {
                Pattern* pattern = parsePattern();
                if (!pattern)
                    return nullptr;
                if (!keywordPatterns.isEmpty()) {
                    // All that there are of them
                    if (!m_callsInvalidRules)
                        return nullptr;
                    Pattern* last = pattern;
                    while (at(TokenKind::Comma)) {
                        unsigned index = m_index;
                        next();
                        Pattern* another = at(TokenKind::Name) && atAhead(1, TokenKind::Equal) ? nullptr : parsePattern();
                        if (!another) {
                            m_index = index;
                            break;
                        }
                        last = another;
                    }
                    return fail("positional patterns follow keyword patterns"_s, *pattern, *last);
                }
                patterns.append(pattern);
            }
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightParenthesis))
            return nullptr;
        auto* pattern = make<MatchClass>(start);
        pattern->cls = value;
        pattern->patterns = m_arena.copy(patterns);
        pattern->keywordAttributes = m_arena.copy(keywordAttributes);
        pattern->keywordPatterns = m_arena.copy(keywordPatterns);
        return pattern;
    }

    // group_pattern | sequence_pattern
    Pattern* parseGroupOrSequencePattern()
    {
        Mark start = mark();
        bool isParenthesis = next().kind == TokenKind::LeftParenthesis;
        TokenKind close = isParenthesis ? TokenKind::RightParenthesis : TokenKind::RightBracket;
        Vector<Pattern*, 8> patterns;
        if (!at(close)) {
            Pattern* first = parseMaybeStarPattern();
            if (!first)
                return nullptr;
            // (pattern) is the pattern. (pattern,) and [pattern] are sequences of one.
            if (isParenthesis && at(close) && !first->is<MatchStar>()) {
                next();
                return first;
            }
            if (isParenthesis && !at(TokenKind::Comma))
                return nullptr;
            patterns.append(first);
            if (!parseRestOfSequencePattern(patterns, close))
                return nullptr;
        }
        if (!expect(close))
            return nullptr;
        auto* sequence = make<MatchSequence>(start);
        sequence->patterns = m_arena.copy(patterns);
        return sequence;
    }

    // mapping_pattern
    Pattern* parseMappingPattern()
    {
        Mark start = mark();
        next();
        Vector<Expression*, 8> keys;
        Vector<Pattern*, 8> patterns;
        const Identifier* rest = nullptr;
        while (!at(TokenKind::RightBrace)) {
            if (consume(TokenKind::DoubleStar)) {
                if (!at(TokenKind::Name) || at(SoftKeyword::Underscore))
                    return nullptr;
                rest = next().text;
                consume(TokenKind::Comma);
                break;
            }
            Expression* key = nullptr;
            switch (peek().kind) {
            case TokenKind::Number:
            case TokenKind::Minus:
                key = parseNumberInPattern();
                break;
            case TokenKind::String:
            case TokenKind::FStringStart:
            case TokenKind::TStringStart:
                key = parseStrings();
                break;
            case TokenKind::KeywordNone:
                key = makeConstant(next(), Constant::Type::None);
                break;
            case TokenKind::KeywordTrue:
                key = makeConstant(next(), Constant::Type::True);
                break;
            case TokenKind::KeywordFalse:
                key = makeConstant(next(), Constant::Type::False);
                break;
            case TokenKind::Name:
                if (atAhead(1, TokenKind::Dot))
                    key = parseNameOrAttribute();
                break;
            default:
                break;
            }
            if (!key || !expect(TokenKind::Colon))
                return nullptr;
            Pattern* pattern = parsePattern();
            if (!pattern)
                return nullptr;
            keys.append(key);
            patterns.append(pattern);
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightBrace))
            return nullptr;
        auto* mapping = make<MatchMapping>(start);
        mapping->keys = m_arena.copy(keys);
        mapping->patterns = m_arena.copy(patterns);
        mapping->rest = rest;
        return mapping;
    }

    // ---- Statements

    bool parseStatementsUntil(TokenKind terminator, Vector<Statement*, 16>& body)
    {
        while (!at(terminator)) {
            if (!parseStatement(body))
                return false;
        }
        return true;
    }

    // block. `after` and `line` are for saying what it should have been the block of.
    // What it is after is said by a rule for each kind of statement, none of which reckons with a comment that says what type something is.
    bool parseBlock(Sequence<Statement*>& result, ASCIILiteral after, unsigned line, bool isAfterTypeComment = false)
    {
        Vector<Statement*, 16> body;
        if (consume(TokenKind::Newline)) {
            if (!consume(TokenKind::Indent)) {
                failAtLastToken(isAfterTypeComment ? "expected an indented block"_str : concatenate("expected an indented block after "_s, after, " on line "_s, line), SyntaxError::Kind::IndentationError);
                return false;
            }
            if (!parseStatementsUntil(TokenKind::Dedent, body))
                return false;
            next();
        } else if (!parseSimpleStatements(body))
            return false;
        result = m_arena.copy(body);
        return true;
    }

    // statement
    bool parseStatement(Vector<Statement*, 16>& body)
    {
        if (!isSafeToRecurse())
            return false;
        Statement* statement = nullptr;
        switch (peek().kind) {
        case TokenKind::KeywordDef:
        case TokenKind::KeywordClass:
        case TokenKind::At:
            statement = parseDefinition();
            break;
        case TokenKind::KeywordAsync:
            if (atAhead(1, TokenKind::KeywordDef))
                statement = parseDefinition();
            else if (atAhead(1, TokenKind::KeywordWith))
                statement = parseWith();
            else if (atAhead(1, TokenKind::KeywordFor))
                statement = parseFor();
            break;
        case TokenKind::KeywordIf:
            statement = parseIf();
            break;
        case TokenKind::KeywordWith:
            statement = parseWith();
            break;
        case TokenKind::KeywordFor:
            statement = parseFor();
            break;
        case TokenKind::KeywordTry:
            statement = parseTry();
            break;
        case TokenKind::KeywordWhile:
            statement = parseWhile();
            break;
        case TokenKind::Name:
            if (at(SoftKeyword::Match)) {
                bool isMatch = false;
                statement = parseMatch(isMatch);
                if (isMatch)
                    break;
            }
            return parseSimpleStatements(body);
        default:
            return parseSimpleStatements(body);
        }
        if (!statement)
            return false;
        // _PyPegen_register_stmts(). Not a function: the second time through, the rule that is there to find what is wrong with one takes all of one that has nothing wrong with it, and what comes of that is no statement.
        if (m_callsInvalidRules && !statement->is<FunctionDef>() && m_lastStatementLine <= statement->line) {
            m_lastStatementLine = statement->line;
            m_lastStatementColumn = statement->column;
        }
        body.append(statement);
        return true;
    }

    // simple_stmts
    bool parseSimpleStatements(Vector<Statement*, 16>& body)
    {
        while (true) {
            Statement* statement = parseSimpleStatement();
            if (!statement)
                return false;
            body.append(statement);
            if (!consume(TokenKind::Semicolon))
                break;
            if (at(TokenKind::Newline))
                break;
        }
        return expect(TokenKind::Newline);
    }

    template<typename T>
    Statement* makeKeywordStatement()
    {
        return make<T>(next());
    }

    // simple_stmt
    Statement* parseSimpleStatement()
    {
        Mark start = mark();
        // An assignment is what is tried first, and so what is wrong with one is looked for before anything else is tried. Where it could be one, parseAssignmentOrExpression() sees to that.
        if (m_callsInvalidRules && (!at(TokenKind::Name) || at(SoftKeyword::Type)) && !canStartStarExpression()) {
            recognizeInvalidAssignment();
            if (m_error)
                return nullptr;
        }
        switch (peek().kind) {
        case TokenKind::KeywordPass:
            return makeKeywordStatement<Pass>();
        case TokenKind::KeywordBreak:
            return makeKeywordStatement<Break>();
        case TokenKind::KeywordContinue:
            return makeKeywordStatement<Continue>();
        case TokenKind::KeywordReturn: {
            next();
            Expression* value = nullptr;
            if (canStartStarExpression()) {
                value = parseStarExpressions();
                if (!value)
                    return nullptr;
            }
            auto* statement = make<Return>(start);
            statement->value = value;
            return statement;
        }
        case TokenKind::KeywordRaise: {
            next();
            Expression* exception = nullptr;
            Expression* cause = nullptr;
            if (canStartExpression()) {
                exception = parseExpression();
                if (!exception)
                    return nullptr;
                if (consume(TokenKind::KeywordFrom)) {
                    cause = parseExpression();
                    if (!cause)
                        return nullptr;
                }
            }
            auto* statement = make<Raise>(start);
            statement->exception = exception;
            statement->cause = cause;
            return statement;
        }
        case TokenKind::KeywordGlobal:
        case TokenKind::KeywordNonlocal: {
            bool isGlobal = next().kind == TokenKind::KeywordGlobal;
            Vector<const Identifier*, 8> names;
            do {
                if (!at(TokenKind::Name))
                    return nullptr;
                names.append(next().text);
            } while (consume(TokenKind::Comma));
            if (isGlobal) {
                auto* statement = make<Global>(start);
                statement->names = m_arena.copy(names);
                return statement;
            }
            auto* statement = make<Nonlocal>(start);
            statement->names = m_arena.copy(names);
            return statement;
        }
        case TokenKind::KeywordAssert: {
            next();
            Expression* test = parseExpression();
            if (!test)
                return nullptr;
            Expression* message = nullptr;
            if (consume(TokenKind::Comma)) {
                message = parseExpression();
                if (!message)
                    return nullptr;
            }
            auto* statement = make<Assert>(start);
            statement->test = test;
            statement->message = message;
            return statement;
        }
        case TokenKind::KeywordDel:
            return parseDelete();
        case TokenKind::KeywordImport:
            return parseImport();
        case TokenKind::KeywordFrom:
            return parseImportFrom();
        case TokenKind::KeywordYield: {
            Expression* value = parseYield();
            if (!value)
                return nullptr;
            auto* statement = make<Expr>(start);
            statement->value = value;
            return statement;
        }
        case TokenKind::Name:
            if (at(SoftKeyword::Type) && atAhead(1, TokenKind::Name) && (atAhead(2, TokenKind::Equal) || atAhead(2, TokenKind::LeftBracket))) {
                // Two names side by side begin nothing else, so what is wrong with it is what is wrong with a type statement.
                if (m_callsInvalidRules) {
                    recognizeInvalidAssignment();
                    if (m_error)
                        return nullptr;
                }
                return parseTypeAlias();
            }
            return parseAssignmentOrExpression();
        default:
            return parseAssignmentOrExpression();
        }
    }

    // type_alias
    Statement* parseTypeAlias()
    {
        Mark start = mark();
        next();
        Name* name = makeName(next(), ExpressionContext::Store);
        Sequence<TypeParameter*> typeParameters;
        if (!parseTypeParameters(typeParameters) || !expect(TokenKind::Equal))
            return nullptr;
        Expression* value = parseExpression();
        if (!value)
            return nullptr;
        // `type` is a name like any other unless the whole statement is this.
        if (!at(TokenKind::Newline) && !at(TokenKind::Semicolon))
            return nullptr;
        auto* alias = make<TypeAlias>(start);
        alias->name = name;
        alias->typeParameters = typeParameters;
        alias->value = value;
        return alias;
    }

    static std::optional<BinaryOperator> augmentedOperator(TokenKind kind)
    {
        switch (kind) {
        case TokenKind::PlusEqual:
            return BinaryOperator::Add;
        case TokenKind::MinusEqual:
            return BinaryOperator::Sub;
        case TokenKind::StarEqual:
            return BinaryOperator::Mult;
        case TokenKind::AtEqual:
            return BinaryOperator::MatMult;
        case TokenKind::SlashEqual:
            return BinaryOperator::Div;
        case TokenKind::PercentEqual:
            return BinaryOperator::Mod;
        case TokenKind::AmpersandEqual:
            return BinaryOperator::BitAnd;
        case TokenKind::VerticalBarEqual:
            return BinaryOperator::BitOr;
        case TokenKind::CircumflexEqual:
            return BinaryOperator::BitXor;
        case TokenKind::LeftShiftEqual:
            return BinaryOperator::LShift;
        case TokenKind::RightShiftEqual:
            return BinaryOperator::RShift;
        case TokenKind::DoubleStarEqual:
            return BinaryOperator::Pow;
        case TokenKind::DoubleSlashEqual:
            return BinaryOperator::FloorDiv;
        default:
            return std::nullopt;
        }
    }

    // assignment | star_expressions
    Statement* parseAssignmentOrExpression()
    {
        unsigned index = m_index;
        Statement* statement = parseAssignmentOrExpressionItself();
        if (!m_callsInvalidRules || m_error || (statement && !statement->is<Expr>()))
            return statement;
        // An assignment is tried before an expression by itself is, and so what is wrong with one is looked for before it is taken for that.
        unsigned end = m_index;
        m_index = index;
        recognizeInvalidAssignment();
        if (m_error)
            return nullptr;
        m_index = end;
        return statement;
    }

    // invalid_assignment
    void recognizeInvalidAssignment()
    {
        bool isDone = false;
        lookForWhatIsWrong([&] {
            // invalid_ann_assign_target
            if (!at(TokenKind::LeftParenthesis) && !at(TokenKind::LeftBracket))
                return false;
            bool isBracket = at(TokenKind::LeftBracket);
            Expression* target = parseAtom();
            if (!target || !(isBracket ? target->is<List>() : target->is<Tuple>() || target->is<List>()))
                return false;
            if (consume(TokenKind::Colon) && parseExpression())
                fail(concatenate("only single target (not "_s, describe(*target), ") can be annotated"_s), *target);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            Expression* first = parseStarNamedExpression();
            if (!first || !consume(TokenKind::Comma))
                return false;
            // star_named_expressions*
            while (true) {
                unsigned index = m_index;
                if (!parseStarNamedExpression()) {
                    m_index = index;
                    break;
                }
                consume(TokenKind::Comma);
            }
            if (!m_error && consume(TokenKind::Colon) && parseExpression())
                fail("only single target (not tuple) can be annotated"_s, *first);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            Expression* target = parseExpression();
            if (target && consume(TokenKind::Colon) && parseExpression())
                fail("illegal target for annotation"_s, *target);
            return false;
        });
        if (m_error)
            return;
        // (star_targets '=')*
        auto skipTargets = [&] {
            while (true) {
                unsigned index = m_index;
                if (!parseStarTargets() || !consume(TokenKind::Equal)) {
                    m_index = index;
                    return;
                }
            }
        };
        lookForWhatIsWrong([&] {
            skipTargets();
            Expression* target = parseStarExpressions();
            if (target && at(TokenKind::Equal)) {
                isDone = true;
                failForInvalidTarget(*target, Targets::Star);
            }
            return false;
        });
        if (m_error || isDone)
            return;
        lookForWhatIsWrong([&] {
            skipTargets();
            Expression* target = at(TokenKind::KeywordYield) ? parseYield() : nullptr;
            if (target && at(TokenKind::Equal))
                fail("assignment to yield expression not possible"_s, *target);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            Expression* target = parseStarExpressions();
            if (!target || !augmentedOperator(peek().kind))
                return false;
            next();
            if (parseYieldOrStarExpressions())
                fail(concatenate('\'', describe(*target), "' is an illegal expression for augmented assignment"_s), *target);
            return false;
        });
    }

    Statement* parseAssignmentOrExpressionItself()
    {
        Mark start = mark();
        Expression* first = parseStarExpressions();
        if (!first)
            return nullptr;

        if (at(TokenKind::Colon)) {
            if (!first->is<Name>() && !first->is<Attribute>() && !first->is<Subscript>())
                return nullptr;
            next();
            Expression* annotation = parseExpression();
            if (!annotation)
                return nullptr;
            setContext(*first, ExpressionContext::Store);
            Expression* value = nullptr;
            if (at(TokenKind::Equal)) {
                unsigned index = m_index;
                next();
                value = parseYieldOrStarExpressions();
                if (m_error)
                    return nullptr;
                if (!value)
                    m_index = index;
            }
            auto* statement = make<AnnAssign>(start);
            statement->target = first;
            statement->annotation = annotation;
            statement->value = value;
            statement->isSimple = first->is<Name>() && !first->isParenthesized;
            return statement;
        }

        if (at(TokenKind::Equal)) {
            Vector<Expression*, 4> targets;
            Expression* value = first;
            while (consume(TokenKind::Equal)) {
                if (!makeTarget(*value, ExpressionContext::Store))
                    return nullptr;
                targets.append(value);
                value = parseYieldOrStarExpressions();
                if (!value)
                    return nullptr;
            }
            Text typeComment = parseTypeComment();
            auto* statement = make<Assign>(start);
            statement->targets = m_arena.copy(targets);
            statement->value = value;
            statement->typeComment = typeComment;
            return statement;
        }

        if (auto op = augmentedOperator(peek().kind)) {
            if (!first->is<Name>() && !first->is<Attribute>() && !first->is<Subscript>())
                return nullptr;
            setContext(*first, ExpressionContext::Store);
            next();
            Expression* value = parseYieldOrStarExpressions();
            if (!value)
                return nullptr;
            auto* statement = make<AugAssign>(start);
            statement->target = first;
            statement->op = *op;
            statement->value = value;
            return statement;
        }

        auto* statement = make<Expr>(start);
        statement->value = first;
        return statement;
    }

    // del_stmt
    Statement* parseDelete()
    {
        Mark start = mark();
        next();
        unsigned index = m_index;
        // invalid_del_stmt
        auto isInvalid = [&] () -> Statement* {
            if (!m_callsInvalidRules || m_error)
                return nullptr;
            m_index = index;
            if (Expression* whole = parseStarExpressions())
                failForInvalidTarget(*whole, Targets::Del);
            return nullptr;
        };
        Vector<Expression*, 8> targets;
        do {
            if (at(TokenKind::Newline) || at(TokenKind::Semicolon))
                break;
            Expression* target = parsePrimary();
            if (!target || !makeTarget(*target, ExpressionContext::Del))
                return isInvalid();
            targets.append(target);
        } while (consume(TokenKind::Comma));
        if (targets.isEmpty() || (!at(TokenKind::Newline) && !at(TokenKind::Semicolon)))
            return isInvalid();
        auto* statement = make<Delete>(start);
        statement->targets = m_arena.copy(targets);
        return statement;
    }

    // dotted_name
    const Identifier* parseDottedName()
    {
        if (!at(TokenKind::Name))
            return nullptr;
        const Identifier* first = next().text;
        if (!at(TokenKind::Dot))
            return first;
        StringBuilder builder;
        builder.append(first->string());
        while (consume(TokenKind::Dot)) {
            if (!at(TokenKind::Name))
                return nullptr;
            builder.append('.', next().text->string());
        }
        return makeIdentifier(builder.toString());
    }

    // ['as' NAME]
    bool parseAsName(const Identifier*& result)
    {
        if (!consume(TokenKind::KeywordAs))
            return true;
        // invalid_dotted_as_name and invalid_import_from_as_name, which come first
        if (m_callsInvalidRules) {
            bool isName = at(TokenKind::Name) && (atAhead(1, TokenKind::Comma) || atAhead(1, TokenKind::RightParenthesis) || atAhead(1, TokenKind::Semicolon) || atAhead(1, TokenKind::Newline));
            if (!isName) {
                if (Expression* target = lookForWhatIsWrong([&] { return parseExpression(); }))
                    fail(concatenate("cannot use "_s, describe(*target), " as import target"_s), *target);
                if (m_error)
                    return false;
            }
        }
        if (!at(TokenKind::Name))
            return false;
        result = next().text;
        return true;
    }

    // import_name
    Statement* parseImport()
    {
        Mark start = mark();
        const Token& keyword = next();
        // invalid_import
        if (m_callsInvalidRules) {
            lookForWhatIsWrong([&] {
                do {
                    if (!parseDottedName())
                        return false;
                } while (consume(TokenKind::Comma));
                if (consume(TokenKind::KeywordFrom) && parseDottedName()) {
                    peek();
                    failStartingFrom("Did you mean to use 'from ... import ...' instead?"_s, keyword);
                }
                return false;
            });
            if (m_error)
                return nullptr;
            if (at(TokenKind::Newline))
                return failStartingFrom("Expected one or more names after 'import'"_s, peek());
        }
        Vector<Alias*, 4> names;
        do {
            Mark aliasStart = mark();
            const Identifier* name = parseDottedName();
            const Identifier* asName = nullptr;
            if (!name || !parseAsName(asName))
                return nullptr;
            auto* alias = make<Alias>(aliasStart);
            alias->name = name;
            alias->asName = asName;
            names.append(alias);
        } while (consume(TokenKind::Comma));
        auto* statement = make<Import>(start);
        statement->names = m_arena.copy(names);
        return statement;
    }

    // import_from
    Statement* parseImportFrom()
    {
        Mark start = mark();
        next();
        unsigned level = 0;
        while (true) {
            if (consume(TokenKind::Dot))
                ++level;
            else if (consume(TokenKind::Ellipsis))
                level += 3;
            else
                break;
        }
        const Identifier* module = nullptr;
        if (at(TokenKind::Name)) {
            module = parseDottedName();
            if (!module)
                return nullptr;
        } else if (!level)
            return nullptr;
        if (!expect(TokenKind::KeywordImport))
            return nullptr;

        Vector<Alias*, 8> names;
        if (at(TokenKind::Star)) {
            auto* alias = make<Alias>(next());
            alias->name = &m_arena.identifiers().makeIdentifier(m_vm, "*"_span8);
            names.append(alias);
        } else {
            if (at(TokenKind::Newline))
                return failStartingFrom("Expected one or more names after 'import'"_s, peek());
            bool isParenthesized = consume(TokenKind::LeftParenthesis);
            while (true) {
                if (!at(TokenKind::Name))
                    return nullptr;
                Mark aliasStart = mark();
                const Identifier* name = next().text;
                const Identifier* asName = nullptr;
                if (!parseAsName(asName))
                    return nullptr;
                auto* alias = make<Alias>(aliasStart);
                alias->name = name;
                alias->asName = asName;
                names.append(alias);
                if (!consume(TokenKind::Comma))
                    break;
                if (isParenthesized && at(TokenKind::RightParenthesis))
                    break;
                if (!isParenthesized && at(TokenKind::Newline))
                    return failAtLastToken("trailing comma not allowed without surrounding parentheses"_s);
            }
            if (isParenthesized && !expect(TokenKind::RightParenthesis))
                return nullptr;
        }

        // It changes how what follows is read. Whether there is such a feature, and whether this is a place for it, is for PythonSymbolTable.cpp to say.
        if (!level && module && *module == "__future__"_s) {
            for (Alias* alias : names) {
                if (*alias->name == "barry_as_FLUFL"_s)
                    m_arena.usesLessGreater = true;
            }
        }

        auto* statement = make<ImportFrom>(start);
        statement->module = module;
        statement->names = m_arena.copy(names);
        statement->level = level;
        return statement;
    }

    // ['else' ':' block]
    bool parseElse(Sequence<Statement*>& result)
    {
        if (!at(TokenKind::KeywordElse))
            return true;
        unsigned line = next().line;
        return expectColon(ColonIs::Insisted) && parseBlock(result, "'else' statement"_s, line);
    }

    // if_stmt, and elif_stmt
    Statement* parseIf()
    {
        Mark start = mark();
        bool isElif = next().kind == TokenKind::KeywordElif;
        Expression* test = parseNamedExpression();
        if (!test || !expectColon())
            return nullptr;
        Sequence<Statement*> body;
        if (!parseBlock(body, isElif ? "'elif' statement"_s : "'if' statement"_s, start.line))
            return nullptr;
        Sequence<Statement*> orElse;
        if (at(TokenKind::KeywordElif)) {
            if (!isSafeToRecurse())
                return nullptr;
            Statement* elif = parseIf();
            if (!elif)
                return nullptr;
            Vector<Statement*, 1> single { elif };
            orElse = m_arena.copy(single);
        } else {
            if (!parseElse(orElse))
                return nullptr;
            if (at(TokenKind::KeywordElif))
                return failAtLastToken("'elif' block follows an 'else' block"_s);
        }
        auto* statement = make<If>(start);
        statement->test = test;
        statement->body = body;
        statement->orElse = orElse;
        return statement;
    }

    // while_stmt
    Statement* parseWhile()
    {
        Mark start = mark();
        next();
        Expression* test = parseNamedExpression();
        if (!test || !expectColon())
            return nullptr;
        Sequence<Statement*> body;
        Sequence<Statement*> orElse;
        if (!parseBlock(body, "'while' statement"_s, start.line) || !parseElse(orElse))
            return nullptr;
        auto* statement = make<While>(start);
        statement->test = test;
        statement->body = body;
        statement->orElse = orElse;
        return statement;
    }

    // for_stmt
    Statement* parseFor()
    {
        Mark start = mark();
        bool isAsync = consume(TokenKind::KeywordAsync);
        next();
        unsigned index = m_index;
        Expression* target = parseStarTargets();
        if (!target || !consume(TokenKind::KeywordIn)) {
            if (m_callsInvalidRules && !m_error)
                recognizeInvalidForTarget(index);
            return nullptr;
        }
        Expression* iterable = parseStarExpressions();
        if (!iterable || !expectColon())
            return nullptr;
        Text typeComment = parseTypeComment();
        Sequence<Statement*> body;
        Sequence<Statement*> orElse;
        if (!parseBlock(body, "'for' statement"_s, start.line, !!typeComment) || !parseElse(orElse))
            return nullptr;
        auto* statement = make<For>(start);
        statement->typeComment = typeComment;
        statement->isAsync = isAsync;
        statement->target = target;
        statement->iterable = iterable;
        statement->body = body;
        statement->orElse = orElse;
        return statement;
    }

    // with_item
    WithItem* parseWithItem()
    {
        auto* item = m_arena.create<WithItem>();
        item->contextExpression = parseExpression();
        if (!item->contextExpression)
            return nullptr;
        if (at(TokenKind::KeywordAs)) {
            unsigned index = m_index;
            next();
            auto atEnd = [&] { return at(TokenKind::Comma) || at(TokenKind::RightParenthesis) || at(TokenKind::Colon); };
            item->optionalVariables = parseStarTarget();
            if (item->optionalVariables && atEnd())
                return item;
            item->optionalVariables = nullptr;
            // invalid_with_item
            if (m_callsInvalidRules && !m_error) {
                m_index = index + 1;
                Expression* target = parseExpression();
                if (target && atEnd())
                    failForInvalidTarget(*target, Targets::Star);
            }
            // The expression by itself is an item.
            m_index = index;
        }
        return item;
    }

    // with_stmt
    Statement* parseWith()
    {
        Mark start = mark();
        bool isAsync = consume(TokenKind::KeywordAsync);
        next();
        unsigned afterKeyword = m_index;

        Vector<WithItem*, 4> items;
        // with (a as b, c as d): or with (a, b): where the parentheses are those of a tuple, or with (a).b: and so on.
        bool isParenthesized = at(TokenKind::LeftParenthesis) && speculate([&] {
            next();
            do {
                if (at(TokenKind::RightParenthesis))
                    break;
                WithItem* item = parseWithItem();
                if (!item) {
                    items.clear();
                    return false;
                }
                items.append(item);
            } while (consume(TokenKind::Comma));
            if (!items.isEmpty() && consume(TokenKind::RightParenthesis) && at(TokenKind::Colon))
                return true;
            items.clear();
            return false;
        });
        bool hasItems = true;
        if (!isParenthesized) {
            do {
                WithItem* item = parseWithItem();
                if (!item) {
                    hasItems = false;
                    break;
                }
                items.append(item);
            } while (consume(TokenKind::Comma));
        }
        if (!hasItems || !consume(TokenKind::Colon)) {
            if (!m_callsInvalidRules || m_error)
                return nullptr;
            // invalid_with_stmt. Here what comes after `as` need not be followed by anything in particular.
            auto skipItems = [&] (bool isInParentheses) {
                do {
                    if (isInParentheses && at(TokenKind::RightParenthesis))
                        break;
                    if (!(isInParentheses ? parseExpressions() : parseExpression()))
                        return false;
                    if (at(TokenKind::KeywordAs)) {
                        unsigned index = m_index;
                        next();
                        if (!parseStarTarget())
                            m_index = index;
                    }
                } while (!m_error && consume(TokenKind::Comma));
                return !m_error;
            };
            m_index = afterKeyword;
            if (skipItems(false) && at(TokenKind::Newline))
                return failAtLastToken("expected ':'"_s);
            if (m_error)
                return nullptr;
            m_index = afterKeyword;
            if (consume(TokenKind::LeftParenthesis) && skipItems(true) && consume(TokenKind::RightParenthesis) && at(TokenKind::Newline))
                return failAtLastToken("expected ':'"_s);
            return nullptr;
        }
        Text typeComment = parseTypeComment();
        Sequence<Statement*> body;
        if (!parseBlock(body, "'with' statement"_s, start.line, !!typeComment))
            return nullptr;
        auto* statement = make<With>(start);
        statement->typeComment = typeComment;
        statement->isAsync = isAsync;
        statement->items = m_arena.copy(items);
        statement->body = body;
        return statement;
    }

    // invalid_except_stmt and invalid_except_star_stmt, after the `except` or the `except*`
    void recognizeInvalidExcept(unsigned index, bool isStar)
    {
        m_index = index;
        lookForWhatIsWrong([&] {
            Expression* first = parseExpression();
            if (first && consume(TokenKind::Comma) && parseExpressions() && consume(TokenKind::KeywordAs) && consume(TokenKind::Name) && at(TokenKind::Colon))
                failStartingFrom("multiple exception types must be parenthesized when using 'as'"_s, *first);
            return false;
        });
        if (m_error)
            return;
        lookForWhatIsWrong([&] {
            if (!parseExpression())
                return false;
            if (at(TokenKind::KeywordAs) && atAhead(1, TokenKind::Name)) {
                next();
                next();
            }
            if (at(TokenKind::Newline))
                failAtLastToken("expected ':'"_s);
            return false;
        });
        if (m_error)
            return;
        if (isStar ? at(TokenKind::Newline) || at(TokenKind::Colon) : at(TokenKind::Newline)) {
            failAtLastToken(isStar ? "expected one or more exception types"_s : "expected ':'"_s);
            return;
        }
        lookForWhatIsWrong([&] {
            if (!parseExpression() || !consume(TokenKind::KeywordAs))
                return false;
            Expression* target = parseExpression();
            if (!target || !consume(TokenKind::Colon))
                return false;
            Sequence<Statement*> body;
            if (parseBlock(body, isStar ? "'except*' statement"_s : "'except' statement"_s, 0))
                fail(concatenate("cannot use except"_s, isStar ? "*"_s : ""_s, " statement with "_s, describe(*target)), *target);
            return false;
        });
    }

    // try_stmt
    Statement* parseTry()
    {
        Mark start = mark();
        next();
        if (!expectColon(ColonIs::Insisted))
            return nullptr;
        Sequence<Statement*> body;
        unsigned afterColon = m_index;
        if (!parseBlock(body, "'try' statement"_s, start.line)) {
            // Where invalid_try_stmt looks for both kinds of `except`, it does not ask that there be anything before them. So what is wrong with the first of them is found, though nothing
            // can come of the whole.
            if (!m_callsInvalidRules || m_error)
                return nullptr;
            m_index = afterColon;
            if (!at(TokenKind::KeywordExcept))
                return nullptr;
        }

        Vector<ExceptHandler*, 4> handlers;
        bool isStar = false;
        while (at(TokenKind::KeywordExcept)) {
            Mark handlerStart = mark();
            const Token& keyword = next();
            bool handlerIsStar = at(TokenKind::Star);
            if (!handlers.isEmpty() && handlerIsStar != isStar) {
                // The last two of invalid_try_stmt. It has to be all there, as far as the colon.
                if (!m_callsInvalidRules)
                    return nullptr;
                const Token& last = handlerIsStar ? next() : keyword;
                bool hasType = !!lookForWhatIsWrong([&] { return parseExpression(); });
                if (m_error || (handlerIsStar && !hasType))
                    return nullptr;
                if (hasType) {
                    parseExpression();
                    if (at(TokenKind::KeywordAs) && atAhead(1, TokenKind::Name)) {
                        next();
                        next();
                    }
                }
                if (at(TokenKind::Colon))
                    fail("cannot have both 'except' and 'except*' on the same 'try'"_s, keyword, last);
                return nullptr;
            }
            consume(TokenKind::Star);
            isStar = handlerIsStar;

            unsigned afterKeyword = m_index;
            Expression* type = nullptr;
            const Identifier* name = nullptr;
            bool isWellFormed = true;
            if (!at(TokenKind::Colon)) {
                type = parseExpressions();
                if (type && at(TokenKind::KeywordAs)) {
                    if ((type->is<Tuple>() && !type->isParenthesized) || !atAhead(1, TokenKind::Name))
                        isWellFormed = false;
                    else {
                        next();
                        name = next().text;
                    }
                }
                isWellFormed &= !!type;
            } else if (isStar)
                isWellFormed = false;
            if (!isWellFormed || !at(TokenKind::Colon)) {
                if (m_callsInvalidRules && !m_error)
                    recognizeInvalidExcept(afterKeyword, isStar);
                return nullptr;
            }
            next();
            Sequence<Statement*> handlerBody;
            if (!parseBlock(handlerBody, isStar ? "'except*' statement"_s : "'except' statement"_s, handlerStart.line))
                return nullptr;
            auto* handler = make<ExceptHandler>(handlerStart);
            handler->type = type;
            handler->name = name;
            handler->body = handlerBody;
            handlers.append(handler);
        }

        Sequence<Statement*> orElse;
        if (!handlers.isEmpty() && !parseElse(orElse))
            return nullptr;

        Sequence<Statement*> finalBody;
        if (at(TokenKind::KeywordFinally)) {
            unsigned line = next().line;
            if (!expectColon(ColonIs::Insisted) || !parseBlock(finalBody, "'finally' statement"_s, line))
                return nullptr;
        } else if (handlers.isEmpty())
            return failAtLastToken("expected 'except' or 'finally' block"_s);

        auto* statement = make<Try>(start);
        statement->isStar = isStar;
        statement->body = body;
        statement->handlers = m_arena.copy(handlers);
        statement->orElse = orElse;
        statement->finalBody = finalBody;
        return statement;
    }

    // match_stmt, if that is what this is: `match` is a name like any other unless what follows says otherwise.
    Statement* parseMatch(bool& isMatch)
    {
        Mark start = mark();
        Expression* subject = speculate([&] () -> Expression* {
            next();
            Mark subjectStart = mark();
            Expression* first = parseStarNamedExpression();
            if (!first)
                return nullptr;
            if (at(TokenKind::Comma)) {
                Vector<Expression*, 8> elements;
                elements.append(first);
                while (consume(TokenKind::Comma)) {
                    if (at(TokenKind::Colon) || at(TokenKind::Newline))
                        break;
                    Expression* element = parseStarNamedExpression();
                    if (!element)
                        return nullptr;
                    elements.append(element);
                }
                auto* tuple = make<Tuple>(subjectStart);
                tuple->elements = m_arena.copy(elements);
                first = tuple;
            } else if (first->is<Starred>())
                return nullptr;
            // invalid_match_stmt
            if (at(TokenKind::Newline))
                return failAtLastToken("expected ':'"_s);
            if (!at(TokenKind::Colon) || !atAhead(1, TokenKind::Newline))
                return nullptr;
            return first;
        });
        if (!subject)
            return nullptr;

        isMatch = true;
        next();
        next();
        if (!consume(TokenKind::Indent))
            return failAtLastToken(concatenate("expected an indented block after 'match' statement on line "_s, start.line), SyntaxError::Kind::IndentationError);

        Vector<MatchCase*, 8> cases;
        while (at(SoftKeyword::Case)) {
            unsigned line = next().line;
            auto* matchCase = m_arena.create<MatchCase>();
            matchCase->pattern = parsePatterns();
            if (!matchCase->pattern)
                return nullptr;
            if (consume(TokenKind::KeywordIf)) {
                matchCase->guard = parseNamedExpression();
                if (!matchCase->guard)
                    return nullptr;
            }
            if (!expectColon() || !parseBlock(matchCase->body, "'case' statement"_s, line))
                return nullptr;
            cases.append(matchCase);
        }
        if (cases.isEmpty() || !expect(TokenKind::Dedent))
            return nullptr;
        auto* statement = make<Match>(start);
        statement->subject = subject;
        statement->cases = m_arena.copy(cases);
        return statement;
    }

    // function_def | class_def
    Statement* parseDefinition()
    {
        Vector<Expression*, 4> decorators;
        while (consume(TokenKind::At)) {
            Expression* decorator = parseNamedExpression();
            if (!decorator || !expect(TokenKind::Newline))
                return nullptr;
            decorators.append(decorator);
        }

        Mark start = mark();
        if (consume(TokenKind::KeywordClass)) {
            if (!at(TokenKind::Name))
                return nullptr;
            const Identifier* name = next().text;
            Sequence<TypeParameter*> typeParameters;
            if (!parseTypeParameters(typeParameters))
                return nullptr;
            Vector<Expression*, 8> bases;
            Vector<Keyword*, 8> keywords;
            if (at(TokenKind::LeftParenthesis) && !parseArguments(bases, keywords, GeneratorIs::NotAllowed))
                return nullptr;
            if (!expectColon())
                return nullptr;
            Sequence<Statement*> body;
            if (!parseBlock(body, "class definition"_s, start.line))
                return nullptr;
            auto* statement = make<ClassDef>(start);
            statement->name = name;
            statement->bases = m_arena.copy(bases);
            statement->keywords = m_arena.copy(keywords);
            statement->body = body;
            statement->decorators = m_arena.copy(decorators);
            statement->typeParameters = typeParameters;
            return statement;
        }

        bool isAsync = consume(TokenKind::KeywordAsync);
        if (!expect(TokenKind::KeywordDef) || !at(TokenKind::Name))
            return nullptr;
        const Identifier* name = next().text;
        Sequence<TypeParameter*> typeParameters;
        unsigned afterName = m_index;
        if (!parseTypeParameters(typeParameters))
            m_index = afterName;
        if (!consume(TokenKind::LeftParenthesis))
            return fail("expected '('"_s, peek());
        Arguments* arguments = parseParameters(TokenKind::RightParenthesis, true);
        if (!arguments || !expect(TokenKind::RightParenthesis))
            return nullptr;
        Expression* returns = nullptr;
        if (at(TokenKind::Arrow)) {
            unsigned index = m_index;
            next();
            returns = parseExpression();
            if (!returns)
                m_index = index;
        }
        if (!consume(TokenKind::Colon))
            return fail("expected ':'"_s, peek());
        // func_type_comment: on the same line, or on a line of its own before the block. When it is what is wrong that is looked for, what is looked for first is a line that ends and no block, and a comment
        // on a line of its own is taken for that: whatever is wrong further on, that is what is said.
        Text typeComment;
        if (m_arena.hasTypeComments && !m_callsInvalidRules && at(TokenKind::Newline) && atAhead(1, TokenKind::TypeComment) && atAhead(2, TokenKind::Newline) && atAhead(3, TokenKind::Indent)) {
            next();
            typeComment = parseTypeComment();
        } else if (at(TokenKind::TypeComment)) {
            if (m_callsInvalidRules && atAhead(1, TokenKind::Newline) && atAhead(2, TokenKind::TypeComment) && atAhead(3, TokenKind::Newline) && atAhead(4, TokenKind::Indent)) {
                m_index += 5;
                return failAtLastToken("Cannot have two type comments on def"_s);
            }
            typeComment = parseTypeComment();
        }
        Sequence<Statement*> body;
        if (!parseBlock(body, "function definition"_s, start.line, !!typeComment))
            return nullptr;
        auto* statement = make<FunctionDef>(start);
        statement->typeComment = typeComment;
        statement->isAsync = isAsync;
        statement->name = name;
        statement->arguments = arguments;
        statement->body = body;
        statement->decorators = m_arena.copy(decorators);
        statement->returns = returns;
        statement->typeParameters = typeParameters;
        return statement;
    }

    VM& m_vm;
    Arena& m_arena;
    const TokenBuffer& m_tokens;
    TypedTokens* m_supply { nullptr }; // Where more are to be had, if they are not all there.
    SyntaxError& m_error;
    SyntaxError m_scannerError;
    unsigned m_index { 0 };
    // The last token that has been looked at, which is how far CPython's tokenizer would have got, being asked for one token at a time.
    unsigned m_furthest { 0 };
    bool m_hasScannerError { false }; // m_error is m_scannerError.
    bool m_isSecondPass { false };
    // Whether what is in the grammar only to be recognized as a mistake is tried. In the second pass, but for a part of it here and there.
    bool m_callsInvalidRules { false };
    unsigned m_lastStatementLine { 0 };
    unsigned m_lastStatementColumn { 0 };
    bool m_failsInEitherPass { false };
    bool m_keepsWhatIsParsed { false };
    bool m_isBadSingleStatement { false };
    bool m_isAtEndOfInput { false };
    bool m_lastLineIsEnded { true };
    // For each rule of the grammar that CPython does this for: those that are marked (memo), and those that begin with themselves.
    struct Memos {
        MemoTable expressions;
        MemoTable starExpressions;
        MemoTable disjunctions;
        MemoTable conjunctions;
        MemoTable inversions;
        std::array<MemoTable, 7> binaryOperations; // By precedence: bitwise_or is 1 and term is 6.
        MemoTable factors;
        MemoTable awaitPrimaries;
        MemoTable primaries;
        MemoTable strings;
    };
    Memos m_memos;
    HashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_invalidNamedExpressions;
    HashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_notArguments;
    unsigned m_speculationDepth { 0 };
};

} // anonymous namespace

Module* parse(VM& vm, Arena& arena, StringView source, Module::Kind kind, Vector<SyntaxWarning>& warnings, SyntaxError& error)
{
    TokenBuffer tokens;
    ScanRange range;
    range.lastLine = kind == Module::Kind::Module ? ScanRange::LastLine::IsEnded : kind == Module::Kind::Interactive ? ScanRange::LastLine::IsEndedByTheEnd : ScanRange::LastLine::IsLeft;
    tokenize(vm, arena, source, range, tokens, warnings, error);
    return Parser(vm, arena, tokens, error).parseModule(kind);
}

Module* parseTyped(VM& vm, Arena& arena, TypedTokens& tokens, SyntaxError& error, bool& isAtEndOfInput)
{
    Parser parser(vm, arena, tokens, error);
    Module* module = parser.parseModule(Module::Kind::Interactive);
    isAtEndOfInput = parser.isAtEndOfInput();
    return module;
}

static ScanRange rangeFor(StringView source, unsigned start, unsigned end, unsigned line, bool isInsideBrackets)
{
    unsigned lineStart = start;
    while (lineStart && source[lineStart - 1] != '\n' && source[lineStart - 1] != '\r')
        --lineStart;
    return { start, end, line, lineStart, isInsideBrackets };
}

Statement* parseDefinition(VM& vm, Arena& arena, StringView source, unsigned start, unsigned end, unsigned line)
{
    TokenBuffer tokens;
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    tokenize(vm, arena, source, rangeFor(source, start, end, line, false), tokens, warnings, error);
    return Parser(vm, arena, tokens, error).parseDefinitionAlone();
}

Expression* parseExpression(VM& vm, Arena& arena, StringView source, unsigned start, unsigned end, unsigned line)
{
    TokenBuffer tokens;
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    // If it goes over more than one line it is inside brackets, and if it does not it makes no difference.
    tokenize(vm, arena, source, rangeFor(source, start, end, line, true), tokens, warnings, error);
    return Parser(vm, arena, tokens, error).parseExpressionAlone();
}

} } // namespace JSC::Python
