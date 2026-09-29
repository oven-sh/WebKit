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
#include "PythonBuiltins.h"

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PyTuple.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonLexer.h"
#include "PythonOperations.h"

// The module _tokenize: Python/Python-tokenize.c of CPython. It gives a program the tokens that the scanner makes out, which is the same scanner that the compiler has: see TokenStream. The module tokenize is written in
// Python over it.

namespace JSC { namespace Python {

namespace {

struct TokenizeModuleState final : NativeState {
    PYTHON_NATIVE_STATE(TokenizeModuleState);
    WriteBarrier<PyType> tokenizerIter;
};

template<typename Visitor> void TokenizeModuleState::visit(Visitor& visitor) { visitor.append(tokenizerIter); }

// The numbers that CPython has for the kinds of token: Include/internal/pycore_token.h.
enum TokenType : int {
    ENDMARKER = 0,
    NAME = 1,
    NUMBER = 2,
    STRING = 3,
    NEWLINE = 4,
    INDENT = 5,
    DEDENT = 6,
    OP = 55,
    TYPE_IGNORE = 56,
    TYPE_COMMENT = 57,
    FSTRING_START = 59,
    FSTRING_MIDDLE = 60,
    FSTRING_END = 61,
    TSTRING_START = 62,
    TSTRING_MIDDLE = 63,
    TSTRING_END = 64,
    COMMENT = 65,
    NL = 66,
};

int typeOf(TokenKind kind)
{
    switch (kind) {
    case TokenKind::EndMarker:
        return ENDMARKER;
    case TokenKind::Newline:
        return NEWLINE;
    case TokenKind::Indent:
        return INDENT;
    case TokenKind::Dedent:
        return DEDENT;
    case TokenKind::Name:
        return NAME;
    case TokenKind::Number:
        return NUMBER;
    case TokenKind::String:
        return STRING;
    case TokenKind::FStringStart:
        return FSTRING_START;
    case TokenKind::FStringMiddle:
        return FSTRING_MIDDLE;
    case TokenKind::FStringEnd:
        return FSTRING_END;
    case TokenKind::TStringStart:
        return TSTRING_START;
    case TokenKind::TStringMiddle:
        return TSTRING_MIDDLE;
    case TokenKind::TStringEnd:
        return TSTRING_END;
    case TokenKind::TypeComment:
        return TYPE_COMMENT;
    case TokenKind::TypeIgnore:
        return TYPE_IGNORE;
    case TokenKind::Comment:
        return COMMENT;
    case TokenKind::NonLogicalNewline:
        return NL;
    // A character that begins nothing is what _PyToken_OneChar() has no other word for.
    case TokenKind::Invalid:
        return OP;
    case TokenKind::LeftParenthesis:
        return 7;
    case TokenKind::RightParenthesis:
        return 8;
    case TokenKind::LeftBracket:
        return 9;
    case TokenKind::RightBracket:
        return 10;
    case TokenKind::Colon:
        return 11;
    case TokenKind::Comma:
        return 12;
    case TokenKind::Semicolon:
        return 13;
    case TokenKind::Plus:
        return 14;
    case TokenKind::Minus:
        return 15;
    case TokenKind::Star:
        return 16;
    case TokenKind::Slash:
        return 17;
    case TokenKind::VerticalBar:
        return 18;
    case TokenKind::Ampersand:
        return 19;
    case TokenKind::Less:
        return 20;
    case TokenKind::Greater:
        return 21;
    case TokenKind::Equal:
        return 22;
    case TokenKind::Dot:
        return 23;
    case TokenKind::Percent:
        return 24;
    case TokenKind::LeftBrace:
        return 25;
    case TokenKind::RightBrace:
        return 26;
    case TokenKind::EqualEqual:
        return 27;
    case TokenKind::NotEqual:
        return 28;
    case TokenKind::LessEqual:
        return 29;
    case TokenKind::GreaterEqual:
        return 30;
    case TokenKind::Tilde:
        return 31;
    case TokenKind::Circumflex:
        return 32;
    case TokenKind::LeftShift:
        return 33;
    case TokenKind::RightShift:
        return 34;
    case TokenKind::DoubleStar:
        return 35;
    case TokenKind::PlusEqual:
        return 36;
    case TokenKind::MinusEqual:
        return 37;
    case TokenKind::StarEqual:
        return 38;
    case TokenKind::SlashEqual:
        return 39;
    case TokenKind::PercentEqual:
        return 40;
    case TokenKind::AmpersandEqual:
        return 41;
    case TokenKind::VerticalBarEqual:
        return 42;
    case TokenKind::CircumflexEqual:
        return 43;
    case TokenKind::LeftShiftEqual:
        return 44;
    case TokenKind::RightShiftEqual:
        return 45;
    case TokenKind::DoubleStarEqual:
        return 46;
    case TokenKind::DoubleSlash:
        return 47;
    case TokenKind::DoubleSlashEqual:
        return 48;
    case TokenKind::At:
        return 49;
    case TokenKind::AtEqual:
        return 50;
    case TokenKind::Arrow:
        return 51;
    case TokenKind::Ellipsis:
        return 52;
    case TokenKind::ColonEqual:
        return 53;
    case TokenKind::Exclamation:
        return 54;
    default:
        // A stream has no keywords, which are names to it, and is not given what says that it stopped.
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool isStringLiteral(int type) { return type == STRING || type == FSTRING_MIDDLE || type == TSTRING_MIDDLE; }

struct TokenizerIterState final : NativeState, LineSource {
    PYTHON_NATIVE_STATE(TokenizerIterState);

    Result readLine(Vector<char16_t>&) final;

    WriteBarrier<Unknown> readline;
    String encoding; // Null if the lines are str.
    std::unique_ptr<TokenStream> stream;
    bool hasExtraTokens { false };
    bool isDone { false };
    // Whoever is asking for a token, for as long as it is.
    JSGlobalObject* globalObject { nullptr };
    // The line is kept, so as not to be made again for each token on it.
    WriteBarrier<Unknown> lastLine;
    int64_t lastLineNumber { 0 };
    int64_t lastEndLineNumber { 0 };
    int64_t byteColumnOffsetDifference { 0 };
};

template<typename Visitor>
void TokenizerIterState::visit(Visitor& visitor)
{
    visitor.append(readline);
    visitor.append(lastLine);
}

// tok_readline_string()
auto TokenizerIterState::readLine(Vector<char16_t>& buffer) -> Result
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue raw = call(globalObject, readline.get());
    if (scope.exception()) [[unlikely]]
        return catchException(globalObject, BuiltinType::StopIteration) ? Result::Line : Result::Failed;
    String line;
    if (!encoding.isNull()) {
        if (!isBytes(raw)) {
            raiseTypeError(globalObject, scope, "readline() returned a non-bytes object"_s);
            return Result::Failed;
        }
        line = decodeBytes(globalObject, *builtinBufferOf(raw), encoding, "replace"_s);
        RETURN_IF_EXCEPTION(scope, Result::Failed);
    } else {
        JSString* string = stringIn(raw);
        if (!string) {
            raiseTypeError(globalObject, scope, "readline() returned a non-string object"_s);
            return Result::Failed;
        }
        line = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, Result::Failed);
    }
    // PyUnicode_AsUTF8AndSize(), which half of a pair has no place in. What encodes it is left to say so.
    if (!line.is8Bit()) {
        auto units = line.span16();
        for (size_t i = 0; i < units.size(); ++i) {
            if (!U16_IS_SURROGATE(units[i]))
                continue;
            if (U16_IS_LEAD(units[i]) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1])) {
                ++i;
                continue;
            }
            encodeString(globalObject, jsString(vm, line), "utf-8"_s, "strict"_s);
            RETURN_IF_EXCEPTION(scope, Result::Failed);
        }
    }
    if (!buffer.tryReserveCapacity(buffer.size() + line.length() + 1)) {
        raiseMemoryError(globalObject, scope);
        return Result::Failed;
    }
    if (line.is8Bit())
        buffer.append(line.span8());
    else
        buffer.append(line.span16());
    return Result::Line;
}

int64_t lengthInUTF8(std::span<const char16_t> units)
{
    int64_t length = 0;
    for (char16_t c : units)
        length += c < 0x80 ? 1 : c < 0x800 ? 2 : U16_IS_SURROGATE(c) ? 2 : 3; // Half of the four that a pair takes.
    return length;
}

int64_t lengthInCharacters(std::span<const char16_t> units)
{
    int64_t length = 0;
    for (char16_t c : units)
        length += !U16_IS_TRAIL(c);
    return length;
}

JSValue textOf(JSGlobalObject* globalObject, std::span<const char16_t> units)
{
    return strOrMemoryError(globalObject, String(units));
}

// What is in the source between two places, or nothing if they are the wrong way about.
std::span<const char16_t> between(std::span<const char16_t> source, int64_t start, int64_t end)
{
    start = std::clamp<int64_t>(start, 0, source.size());
    end = std::clamp<int64_t>(end, start, source.size());
    return source.subspan(start, end - start);
}

JSValue raiseWithDetails(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type, const String& message, JSValue details)
{
    JSValue exception = call(globalObject, globalObject->pyRealm()->type(type)->object(), jsString(globalObject->vm(), message), details);
    RETURN_IF_EXCEPTION(scope, { });
    throwException(globalObject, scope, exception);
    return { };
}

// _syntaxerror_range(): what the tokenizer raises for itself.
void raiseFromTokenizer(JSGlobalObject* globalObject, TokenStream& stream, const SyntaxError& error)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto source = stream.source();
    unsigned lineStart = stream.startOfLine(error.line);
    unsigned lineEnd = lineStart;
    // strcspn(), which stops at the end of a C string as well.
    while (lineEnd < source.size() && source[lineEnd] != '\n' && source[lineEnd])
        ++lineEnd;
    auto line = source.subspan(lineStart, lineEnd - lineStart);
    auto inCharacters = [&] (int bytes) -> int {
        if (error.hasColumnsInBytes)
            return bytes;
        int used = 0;
        int count = 0;
        for (size_t i = 0; i < line.size() && used < bytes; ++i) {
            used += lengthInUTF8(line.subspan(i, 1));
            count += !U16_IS_TRAIL(line[i]);
        }
        return count + std::max(bytes - used, 0);
    };
    JSValue text = textOf(globalObject, line);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue details = PyTuple::create(globalObject, { jsNontrivialString(vm, "<string>"_s), intFromUInt64(globalObject, error.line), jsNumber(inCharacters(error.column + 1)), text, intFromUInt64(globalObject, error.line), jsNumber(inCharacters(error.endColumn + 1)) });
    scope.release();
    raiseWithDetails(globalObject, scope, BuiltinType::SyntaxError, error.message, details);
}

// _tokenizer_error(): what it only stops for.
void raiseForStop(JSGlobalObject* globalObject, TokenStream& stream, const SyntaxError& error)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto source = stream.source();
    auto buffered = between(source, stream.bufferStart(), source.size());
    ASCIILiteral message = "unknown tokenization error"_s;
    BuiltinType type = BuiltinType::SyntaxError;
    switch (error.stop) {
    case SyntaxError::Stop::EndOfFile: {
        // PyErr_SyntaxLocationObject(), for a file that there is not
        JSValue exception = call(globalObject, globalObject->pyRealm()->type(BuiltinType::SyntaxError)->object(), jsNontrivialString(vm, "unexpected EOF in multi-line statement"_s));
        RETURN_IF_EXCEPTION(scope, void());
        auto set = [&] (ASCIILiteral name, JSValue value) { setAttribute(globalObject, exception, Identifier::fromString(vm, name), value); };
        set("lineno"_s, intFromUInt64(globalObject, stream.line()));
        RETURN_IF_EXCEPTION(scope, void());
        set("offset"_s, intFromInt64(globalObject, lengthInUTF8(buffered)));
        RETURN_IF_EXCEPTION(scope, void());
        set("end_lineno"_s, stream.line() ? intFromUInt64(globalObject, stream.line()) : jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
        set("end_offset"_s, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
        set("filename"_s, jsNontrivialString(vm, "<string>"_s));
        RETURN_IF_EXCEPTION(scope, void());
        throwException(globalObject, scope, exception);
        return;
    }
    case SyntaxError::Stop::Dedent:
        message = "unindent does not match any outer indentation level"_s;
        type = BuiltinType::IndentationError;
        break;
    case SyntaxError::Stop::TabSpace:
        message = "inconsistent use of tabs and spaces in indentation"_s;
        type = BuiltinType::TabError;
        break;
    case SyntaxError::Stop::TooDeep:
        message = "too many levels of indentation"_s;
        type = BuiltinType::IndentationError;
        break;
    case SyntaxError::Stop::LineContinuation:
        message = "unexpected character after line continuation character"_s;
        break;
    case SyntaxError::Stop::None:
        break;
    }
    // All that it is holding, without what ends the last line of it. Where it is said to be is one past that.
    auto line = buffered.first(buffered.size() ? buffered.size() - 1 : 0);
    JSValue text = textOf(globalObject, line);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue details = PyTuple::create(globalObject, { jsNontrivialString(vm, "<string>"_s), intFromUInt64(globalObject, stream.line()), intFromInt64(globalObject, lengthInCharacters(line) + 1), text, jsUndefined(), jsUndefined() });
    scope.release();
    raiseWithDetails(globalObject, scope, type, message, details);
}

// What the tokenizer warns of as it goes. False if something has been raised.
bool issueWarnings(JSGlobalObject* globalObject, TokenStream& stream)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (SyntaxWarning& warning : stream.takeWarnings()) {
        warnExplicit(globalObject, BuiltinType::SyntaxWarning, warning.message, "<string>"_s, warning.line);
        if (!scope.exception()) [[likely]]
            continue;
        // If such warnings are to be errors, it is a SyntaxError, which says more about where.
        if (!catchException(globalObject, BuiltinType::SyntaxWarning))
            return false;
        SyntaxError error;
        error.message = warning.errorMessage.isNull() ? warning.message : warning.errorMessage;
        error.line = warning.line;
        // _PyTokenizer_syntaxerror(): as far as the tokenizer has got, which is past an escape, and up to what a number has run into.
        error.column = error.endColumn = warning.isFromTokenizer ? static_cast<int>(warning.column) : static_cast<int>(warning.endColumn) - 1;
        scope.release();
        raiseFromTokenizer(globalObject, stream, error);
        return false;
    }
    return true;
}

} // anonymous namespace

// TokenizerIter(readline, /, *, extra_tokens, encoding='utf-8')
PYTHON_NATIVE(tokenizerIterNew)
{
    NATIVE_PROLOGUE();
    bool hasExtraTokens = isTrue(globalObject, args.at(2));
    RETURN_IF_EXCEPTION(scope, { });
    String encoding;
    if (JSValue given = args.at(3)) {
        JSString* string = stringIn(given);
        if (!string)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("tokenizeriter() argument 'encoding' must be str, not "_s, typeNameOfArgument(globalObject, given))));
        encoding = string->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (encoding.contains('\0'))
            return JSValue::encode(raiseValueError(globalObject, scope, "embedded null character"_s));
    }
    auto* object = PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<TokenizerIterState>());
    auto& self = object->state<TokenizerIterState>();
    self.readline.set(vm, object, args[1]);
    self.encoding = encoding;
    self.hasExtraTokens = hasExtraTokens;
    self.stream = makeUnique<TokenStream>(vm, self, hasExtraTokens);
    return JSValue::encode(object);
}

PYTHON_NATIVE(tokenizerIterSelf)
{
    return JSValue::encode(callFrame->uncheckedArgument(0));
}

// tokenizeriter_next()
PYTHON_NATIVE(tokenizerIterNext)
{
    NATIVE_PROLOGUE();
    JSCell* cell = args[0].asCell();
    auto& self = stateOf<TokenizerIterState>(cell);
    if (self.isDone)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, "EOF"_s));
    TokenStream& stream = *self.stream;
    StreamedToken token;
    self.globalObject = globalObject;
    auto result = stream.next(token);
    self.globalObject = nullptr;
    // Once it has stopped it is over. CPython's goes on from wherever it had got to, and what comes of that means nothing.
    if (result != TokenStream::Result::Token)
        self.isDone = true;
    RETURN_IF_EXCEPTION(scope, { });
    if (!issueWarnings(globalObject, stream)) {
        self.isDone = true;
        return { };
    }
    if (result == TokenStream::Result::Error) {
        scope.release();
        if (stream.error().isFromTokenizer)
            raiseFromTokenizer(globalObject, stream, stream.error());
        else
            raiseForStop(globalObject, stream, stream.error());
        return { };
    }

    auto source = stream.source();
    int type = typeOf(token.kind);
    if (type == ENDMARKER)
        self.isDone = true;
    if (token.endsInMiddleOfCharacter) [[unlikely]] {
        // PyUnicode_FromStringAndSize(), of what is not UTF-8. What decodes it is left to say so.
        self.isDone = true;
        auto bytes = encodeString(globalObject, jsString(vm, String(between(source, token.start, token.end - 1))), "utf-8"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, { });
        char16_t last = source[token.end - 1];
        bytes->append(static_cast<uint8_t>(last < 0x800 ? 0xC0 | (last >> 6) : U16_IS_LEAD(last) ? 0xF0 | ((((last - 0xD800) >> 6) + 1) >> 2) : 0xE0 | (last >> 12)));
        scope.release();
        decodeBytes(globalObject, bytes->span(), "utf-8"_s, "strict"_s);
        return { };
    }
    JSValue text = token.hasText ? textOf(globalObject, between(source, token.start, token.end)) : JSValue(jsEmptyString(vm));
    RETURN_IF_EXCEPTION(scope, { });
    bool isTrailingToken = type == ENDMARKER || (type == DEDENT && token.isAtEndOfFile);

    int64_t lineStart = isStringLiteral(type) ? token.multiLineStart : token.lineStart;
    JSValue line;
    bool lineChanged = true;
    if (self.hasExtraTokens && isTrailingToken)
        line = jsEmptyString(vm);
    else if (token.line != self.lastLineNumber) {
        // _get_current_line()
        int64_t size = static_cast<int64_t>(token.inputEnd) - lineStart;
        if (size >= 1 && token.hasImplicitNewline)
            --size;
        line = textOf(globalObject, between(source, lineStart, lineStart + size));
        RETURN_IF_EXCEPTION(scope, { });
        self.lastLine.set(vm, cell, line);
        self.byteColumnOffsetDifference = 0;
    } else {
        line = self.lastLine.get();
        lineChanged = false;
        // There has been no line at all. CPython returns nothing and raises nothing, which is how an iterator says that it is over.
        if (!line)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    }

    int64_t lineNumber = isStringLiteral(type) ? token.firstLine : token.line;
    int64_t endLineNumber = token.line;
    int64_t column = -1;
    int64_t endColumn = -1;
    // _get_col_offsets()
    int64_t byteOffset = -1;
    if (token.hasText && token.start >= lineStart) {
        byteOffset = lengthInUTF8(between(source, lineStart, token.start));
        if (lineChanged) {
            column = lengthInCharacters(between(source, lineStart, token.start));
            self.byteColumnOffsetDifference = byteOffset - column;
        } else
            column = byteOffset - self.byteColumnOffsetDifference;
    }
    if (token.hasText && token.end >= token.lineStart) {
        if (lineNumber == endLineNumber) {
            auto whole = between(source, token.start, token.end);
            int64_t width = lengthInCharacters(whole);
            endColumn = column + width;
            self.byteColumnOffsetDifference += lengthInUTF8(whole) - width;
        } else {
            auto onLastLine = between(source, token.lineStart, token.end);
            endColumn = lengthInCharacters(onLastLine);
            self.byteColumnOffsetDifference += lengthInUTF8(onLastLine) - endColumn;
        }
    }
    self.lastLineNumber = lineNumber;
    self.lastEndLineNumber = endLineNumber;

    if (self.hasExtraTokens) {
        if (isTrailingToken) {
            lineNumber = endLineNumber = lineNumber + 1;
            column = endColumn = 0;
        }
        // As the module tokenize did it when it was all written in Python
        if (type > DEDENT && type < OP)
            type = OP;
        else if (type == NEWLINE) {
            if (!token.hasImplicitNewline)
                text = token.start < source.size() && source[token.start] == '\r' ? jsNontrivialString(vm, "\r\n"_s) : jsSingleCharacterString(vm, static_cast<Latin1Character>('\n'));
            ++endColumn;
        } else if (type == NL) {
            if (token.hasImplicitNewline)
                text = jsEmptyString(vm);
        }
    }
    auto place = [&] (int64_t line, int64_t column) { return PyTuple::create(globalObject, { intFromInt64(globalObject, line), intFromInt64(globalObject, column) }); };
    return JSValue::encode(PyTuple::create(globalObject, { jsNumber(type), text, place(lineNumber, column), place(endLineNumber, endColumn), line }));
}

JSObject* createTokenizeModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<TokenizeModuleState>();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    if (!state.tokenizerIter) {
        PyType* type = createBuiltinType(globalObject, "_tokenize.TokenizerIter"_s, realm->typeObject(), PyType::Layout::Native, 0);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.tokenizerIter.set(vm, realm, type);
        addGenericGetAttribute(globalObject, type);
        addMethods(globalObject, type, {
            { "__new__"_s, tokenizerIterNew, Kind::New, 0, "tokenizeriter(readline, /, *, extra_tokens, encoding=<unrepresentable>)"_s, Arguments::AreThoseOfTheClass },
            { "__iter__"_s, tokenizerIterSelf },
            { "__next__"_s, tokenizerIterNext },
        });
    }
    JSObject* module = newBuiltinModule(globalObject, "_tokenize"_s);
    module->putDirect(vm, Identifier::fromString(vm, "TokenizerIter"_s), state.tokenizerIter.get());
    return module;
}

} } // namespace JSC::Python
