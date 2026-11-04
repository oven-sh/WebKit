/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "ParserArena.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyLexer.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _tokenize, which Lib/tokenize.py is built on: the lexer, a token at a time.
// (Python/Python-tokenize.c and Parser/tokenizer/readline_tokenizer.c in CPython are the reference.)

namespace TokenizeModule {

// The iterator is a record. Its number is the address of a Tokenizer, which the collector is told to delete with it;
// 0 once the tokens are over.
enum IteratorField : unsigned {
    IteratorReadline,
    IteratorEncoding, // Empty: readline() returns str.
    IteratorLastLine, // The text of the line of the last token, which the next is likely to share.
    IteratorFieldCount,
};

class Tokenizer final : public Lexer::LineSource {
    WTF_MAKE_TZONE_ALLOCATED(Tokenizer);
public:
    Tokenizer(JSGlobalObject* globalObject, PyRecord* iterator, bool extraTokens)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_vm(m_runtime.vm())
        , m_iterator(iterator)
        , m_extraTokens(extraTokens)
        , m_host(makeParserHost(globalObject, "<string>"_s))
        , m_lexer(m_vm, m_identifiers, *m_host, { }, extraTokens ? OptionSet<Lexer::Flag> { Lexer::Flag::ExtraTokens, Lexer::Flag::WithoutIdentifiers } : OptionSet<Lexer::Flag> { Lexer::Flag::WithoutIdentifiers })
    {
        m_lexer.setLineSource(this);
    }

    bool isRunning() const { return m_isRunning; }

    // The next token, as a tuple. `isLast`: nothing follows it. Empty: an exception.
    JSValue next(bool& isLast)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        SetForScope running(m_isRunning, true);
        Token token;
        m_lexer.lex(token);
        EXCEPTION_ASSERT(!scope.exception() || token.kind == TokenKind::ErrorToken);
        if (token.kind == TokenKind::ErrorToken) {
            scope.release();
            raiseError();
            return { };
        }
        isLast = token.kind == TokenKind::EndMarker;
        RELEASE_AND_RETURN(scope, makeToken(token));
    }

private:
    struct Line {
        uint32_t start; // In the text as readline() gave it.
        bool endsWithCarriageReturn; // "\r\n": the lexer is not shown the "\r".
        bool hasImplicitNewline; // It did not end with "\n": the lexer is shown one.
    };

    // ---- Lines ----

    // True: `bytes` is what readline() returned, in UTF-8; nothing if the text is over.
    bool callReadline(Vector<uint8_t>& bytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bytes.clear();
        JSValue line = call(m_globalObject, m_iterator->get(IteratorReadline));
        if (scope.exception())
            RELEASE_AND_RETURN(scope, clearPendingExceptionIfMatches(m_globalObject, m_runtime.StopIteration()));
        if (JSValue encoding = m_iterator->get(IteratorEncoding)) {
            if (!PyBytes::is(line)) {
                raiseTypeError(m_globalObject, scope, "readline() returned a non-bytes object"_s);
                return false;
            }
            line = bytesDecode(m_globalObject, line, asString(encoding), makeStr(m_vm, "replace"_s));
            RETURN_IF_EXCEPTION(scope, false);
        } else if (!isStr(line)) {
            raiseTypeError(m_globalObject, scope, "readline() returned a non-string object"_s);
            return false;
        }
        String text = strValue(line)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        auto encoded = tryEncodeUTF8(text);
        if (!encoded) {
            // As str.encode() would complain.
            strEncode(m_globalObject, strValue(line), nullptr, nullptr);
            ASSERT(scope.exception());
            return false;
        }
        bytes = WTF::move(*encoded);
        return true;
    }

    // The lexer knows nothing of exceptions. It is told that this failed, and whoever started it finds the exception pending.
    Result readLine(std::span<const uint8_t>& source) final
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        Result result = readLineOrRaise(source);
        EXCEPTION_ASSERT_UNUSED(scope, !!scope.exception() == (result == Result::Failed));
        return result;
    }

    Result readLineOrRaise(std::span<const uint8_t>& source)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        Vector<uint8_t> chunk;
        bool ok = callReadline(chunk);
        RETURN_IF_EXCEPTION(scope, Result::Failed);
        if (!ok)
            return Result::Failed;
        if (chunk.isEmpty())
            return Result::EndOfText;

        // One line, if readline() does what its name says.
        std::span<const uint8_t> rest = chunk.span();
        while (!rest.empty()) {
            auto* newline = static_cast<const uint8_t*>(memchr(rest.data(), '\n', rest.size()));
            auto line = newline ? rest.first(newline - rest.data() + 1) : rest;
            rest = rest.subspan(line.size());
            if (auto* null = static_cast<const uint8_t*>(memchr(line.data(), 0, line.size()))) {
                int lineno = m_lines.size() + 1;
                JSValue details = PyTuple::create(m_globalObject, { makeStr(m_vm, "<string>"_s), jsNumber(lineno), jsNumber(0), decode(line.first(null - line.data())), jsNumber(lineno), jsNumber(0) });
                raiseWithArgs(m_globalObject, scope, m_runtime.SyntaxError(), { makeStr(m_vm, "source code cannot contain null bytes"_s), details });
                return Result::Failed;
            }
            bool hasCarriageReturn = newline && line.size() >= 2 && line[line.size() - 2] == '\r';
            m_lines.append({ static_cast<uint32_t>(m_original.size()), hasCarriageReturn, !newline });
            m_original.append(line);
            m_text.append(line.first(line.size() - (hasCarriageReturn ? 2 : newline ? 1 : 0)));
            m_text.append('\n');
        }
        source = m_text.span();
        return Result::Line;
    }

    // ---- Positions ----

    std::span<const uint8_t> original(uint32_t start, uint32_t end) const { return m_original.span().subspan(start, end - start); }
    uint32_t originalLineStart(int line) const { return m_lines[line - 1].start; }
    // Past the line ending, if it has one.
    uint32_t originalLineEnd(int line) const { return static_cast<size_t>(line) < m_lines.size() ? m_lines[line].start : m_original.size(); }

    // Where in the text as it was given is what the lexer has at that offset, which is in that line or just past it.
    uint32_t originalOffset(int line, uint32_t offset) const
    {
        uint32_t column = offset - m_lexer.lineStartOffset(line);
        auto& info = m_lines[line - 1];
        uint32_t length = originalLineEnd(line) - info.start - info.endsWithCarriageReturn + info.hasImplicitNewline;
        return column >= length ? originalLineEnd(line) : std::min(info.start + column, originalLineEnd(line));
    }

    JSString* decode(std::span<const uint8_t> bytes) { return makeStr(m_vm, stringFromUTF8(bytes)); }
    static int countCharacters(std::span<const uint8_t> bytes) { return characterCount(bytes, bytes.size()); }

    // ---- Tokens ----

    JSValue makeToken(const Token& token)
    {
        int type = isKeyword(token.kind) ? static_cast<int>(TokenKind::Name) : static_cast<int>(token.kind);
        int currentLine = m_lexer.currentLine();
        bool isString = token.kind == TokenKind::String || token.kind == TokenKind::FStringMiddle || token.kind == TokenKind::TStringMiddle;
        bool isTrailing = token.kind == TokenKind::EndMarker || (token.kind == TokenKind::Dedent && m_lexer.status() == Lexer::Status::EndOfFile);
        bool hasImplicitNewline = currentLine && m_lines[currentLine - 1].hasImplicitNewline;
        bool hasCarriageReturn = currentLine && m_lines[currentLine - 1].endsWithCarriageReturn;
        int lineno = isString ? token.startPosition.line : currentLine;
        int endLineno = currentLine;
        JSString* empty = jsEmptyString(m_vm);

        // The line, or the lines of a string. That of the token before serves again if that token started on this line.
        JSString* line = empty;
        if (!(m_extraTokens && isTrailing) && currentLine) {
            if (currentLine != m_lastLineNumber) {
                // At the end of the text CPython's tokenizer has emptied its buffer.
                bool isEmpty = m_lexer.firstBufferedLine() > currentLine;
                m_iterator->set(m_vm, IteratorLastLine, isEmpty ? empty : decode(original(originalLineStart(lineno), originalLineEnd(currentLine))));
            }
            line = asString(m_iterator->get(IteratorLastLine));
        }
        m_lastLineNumber = lineno;

        JSString* text = empty;
        int column = -1;
        int endColumn = -1;
        if (token.hasColumns && currentLine) {
            uint32_t start = originalOffset(lineno, token.start);
            uint32_t end = originalOffset(endLineno, token.end);
            // A Newline ends before the "\n", which the lexer has where the text may have "\r\n".
            if (token.kind == TokenKind::Newline && hasCarriageReturn)
                ++end;
            text = decode(original(start, end));
            column = countCharacters(original(originalLineStart(lineno), start));
            endColumn = lineno == endLineno ? column + countCharacters(original(start, end)) : countCharacters(original(originalLineStart(endLineno), end));
            if (token.kind == TokenKind::NL && hasImplicitNewline)
                ++endColumn;
        }

        if (m_extraTokens) {
            if (isTrailing) {
                lineno = endLineno = lineno + 1;
                column = endColumn = 0;
            }
            if (type > static_cast<int>(TokenKind::Dedent) && type < static_cast<int>(TokenKind::Op))
                type = static_cast<int>(TokenKind::Op);
            else if (token.kind == TokenKind::Newline) {
                if (!hasImplicitNewline)
                    text = makeStr(m_vm, hasCarriageReturn ? "\r\n"_s : "\n"_s);
                ++endColumn;
            } else if (token.kind == TokenKind::NL && hasImplicitNewline)
                text = empty;
        }

        return PyTuple::create(m_globalObject, {
            jsNumber(type), text,
            PyTuple::create(m_globalObject, { jsNumber(lineno), jsNumber(column) }),
            PyTuple::create(m_globalObject, { jsNumber(endLineno), jsNumber(endColumn) }),
            line });
    }

    void raiseError()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyType* type = m_runtime.SyntaxError();
        ASCIILiteral message;
        int currentLine = m_lexer.currentLine();
        int firstLine = m_lexer.firstBufferedLine();
        // What CPython's tokenizer has in its buffer.
        bool bufferIsEmpty = firstLine > currentLine;
        uint32_t bufferStart = bufferIsEmpty ? m_original.size() : originalLineStart(firstLine);
        uint32_t bufferEnd = bufferIsEmpty ? m_original.size() : originalLineEnd(currentLine);
        bool hasImplicitNewline = !bufferIsEmpty && m_lines[currentLine - 1].hasImplicitNewline;

        switch (m_lexer.status()) {
        case Lexer::Status::Error:
        case Lexer::Status::EndOfFileInTripleQuotedString:
        case Lexer::Status::EndOfLineInString:
            scope.release();
            raiseSyntaxErrorInfo(m_globalObject, m_lexer.error(), "<string>"_s);
            return;
        case Lexer::Status::EndOfFile: {
            PyException* exception = newException(m_globalObject, type, "unexpected EOF in multi-line statement"_s);
            RETURN_IF_EXCEPTION(scope, void());
            auto set = [&](ASCIILiteral name, JSValue value) {
                setAttr(m_globalObject, exception, internString(m_vm, name), value);
                if (scope.exception())
                    takePendingException(m_globalObject);
            };
            set("lineno"_s, jsNumber(currentLine));
            set("offset"_s, jsNumber(bufferEnd - bufferStart + (hasImplicitNewline ? 1 : 0)));
            set("end_lineno"_s, jsNumber(currentLine));
            set("end_offset"_s, jsUndefined());
            set("filename"_s, makeStr(m_vm, "<string>"_s));
            raiseException(m_globalObject, scope, exception);
            return;
        }
        case Lexer::Status::Dedent:
            type = m_runtime.IndentationError();
            message = "unindent does not match any outer indentation level"_s;
            break;
        case Lexer::Status::TabSpace:
            type = m_runtime.TabError();
            message = "inconsistent use of tabs and spaces in indentation"_s;
            break;
        case Lexer::Status::TooDeep:
            type = m_runtime.IndentationError();
            message = "too many levels of indentation"_s;
            break;
        case Lexer::Status::LineContinuation:
            message = "unexpected character after line continuation character"_s;
            break;
        case Lexer::Status::Ok:
            message = "unknown tokenization error"_s;
            break;
        }

        auto text = original(bufferStart, bufferEnd);
        if (!hasImplicitNewline && !text.empty())
            text = text.first(text.size() - 1);
        JSValue details = PyTuple::create(m_globalObject, { makeStr(m_vm, "<string>"_s), jsNumber(currentLine), jsNumber(countCharacters(text) + 1), decode(text), jsUndefined(), jsUndefined() });
        raiseWithArgs(m_globalObject, scope, type, { makeStr(m_vm, message), details });
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    VM& m_vm;
    PyRecord* m_iterator; // Which owns this.
    bool m_extraTokens;
    bool m_isRunning { false };
    int m_lastLineNumber { 0 };
    Vector<uint8_t> m_original; // The lines as readline() gave them, in UTF-8.
    Vector<uint8_t> m_text; // As the lexer wants them: each ends with "\n".
    Vector<Line> m_lines;
    IdentifierArena m_identifiers;
    std::unique_ptr<ParserHost> m_host;
    Lexer m_lexer;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(Tokenizer);

static Tokenizer* tokenizerOf(PyRecord* iterator)
{
    return std::bit_cast<Tokenizer*>(static_cast<uintptr_t>(iterator->number));
}

static void destroyTokenizer(JSCell* cell)
{
    // The collector may be sweeping, when a cell cannot be asked what it is.
    auto* iterator = static_cast<PyRecord*>(cell);
    delete tokenizerOf(iterator);
    iterator->number = 0;
}

static JSValue iteratorNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "tokenizeriter() takes exactly 1 positional argument ("_s, args.size(), " given)"_s);
    JSValue extraTokens;
    JSValue encoding;
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String name = args.keywordName(i)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (name == "extra_tokens"_s)
            extraTokens = args.keywordValue(i);
        else if (name == "encoding"_s)
            encoding = args.keywordValue(i);
        else
            return raiseTypeError(globalObject, scope, "tokenizeriter() got an unexpected keyword argument '"_s, name, '\'');
    }
    if (!extraTokens)
        return raiseTypeError(globalObject, scope, "tokenizeriter() missing required argument 'extra_tokens' (pos 2)"_s);
    bool wantsExtraTokens = isTrue(globalObject, extraTokens);
    RETURN_IF_EXCEPTION(scope, { });
    if (encoding) {
        if (!isStr(encoding))
            return raiseTypeError(globalObject, scope, "tokenizeriter() argument 'encoding' must be str, not "_s, typeName(globalObject, encoding));
        String text = strValue(encoding)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (text.contains('\0'))
            return raiseValueError(globalObject, scope, "embedded null character"_s);
        encoding = strValue(encoding);
    }

    PyRecord* iterator = PyRecord::create(globalObject, PyType::from(cls), IteratorFieldCount);
    iterator->set(vm, IteratorReadline, args[0]);
    iterator->set(vm, IteratorEncoding, encoding);
    iterator->number = static_cast<int64_t>(std::bit_cast<uintptr_t>(new Tokenizer(globalObject, iterator, wantsExtraTokens)));
    vm.heap.addFinalizerForAnyThread(iterator, destroyTokenizer);
    return iterator;
}

static JSValue iteratorIter(JSGlobalObject*, JSValue self)
{
    return self;
}

static JSValue iteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* iterator = uncheckedDowncast<PyRecord>(self.asCell());
    Tokenizer* tokenizer = tokenizerOf(iterator);
    if (!tokenizer)
        return raiseStopIteration(globalObject, scope, makeStr(vm, "EOF"_s));
    if (tokenizer->isRunning())
        return raiseValueError(globalObject, scope, "tokenizer already executing"_s);
    bool isLast = false;
    JSValue token = tokenizer->next(isLast);
    // After an error CPython's tokenizer goes on from whatever state it is in. This one is over.
    if (!token || isLast)
        destroyTokenizer(iterator);
    ensureStillAliveHere(iterator);
    RETURN_IF_EXCEPTION(scope, { });
    return token;
}

static constexpr NativeDef iteratorMethods[] = {
    PY_NEW(iteratorNew, "($type, readline, /, *args, **kwargs)"),
    PY_SLOT("__iter__", iteratorIter, "($self, /)"),
    PY_SLOT("__next__", iteratorNext, "($self, /)"),
};

static constexpr TypeDef iteratorTypeDef { "_tokenize.TokenizerIter"_s, { TypeFlag::Immutable }, iteratorMethods, { }, { } };

} // namespace TokenizeModule

void initializeTokenizeModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "TokenizerIter"_s, runtime(globalObject).typeFor(TokenizeModule::iteratorTypeDef));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
