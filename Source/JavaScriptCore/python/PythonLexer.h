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

#include "PythonArena.h"
#include "PythonToken.h"
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringView.h>

namespace JSC {

class VM;

namespace Python {

// What part of a source is to be scanned. The whole of it, or a function that is being compiled when it is first called.
struct ScanRange {
    unsigned start { 0 };
    unsigned end { std::numeric_limits<unsigned>::max() };
    unsigned line { 1 };
    unsigned lineStart { 0 }; // Where the line that `start` is on begins.
    bool isInsideBrackets { false }; // A lambda or a generator expression between brackets, where a newline means nothing.
    // What is done about a last line that nothing ends.
    enum class LastLine : uint8_t {
        IsEnded, // For a module: it is as if the end of a line were there.
        IsLeft, // For an expression.
        IsEndedByTheEnd, // For what is typed at a prompt: the first time that the end of the source is come to, it is taken for the end of a line, whether or not there was one before it.
    };
    LastLine lastLine { LastLine::IsEnded };
};

// The scanner. It alone reads the source: what the parser needs to know is all in the tokens.
// True if it got to the end. Otherwise the error says why not, and the tokens are those before it and then TokenKind::Error.
bool tokenize(VM&, Arena&, StringView source, const ScanRange&, TokenBuffer&, Vector<SyntaxWarning>&, SyntaxError&);

// Where the scanner gets the source from if it is given a line at a time: what CPython's tokenizer has as `underflow`.
class LineSource {
public:
    virtual ~LineSource() = default;
    enum class Result : uint8_t {
        Line, // If nothing was added, that is the end.
        Failed, // Something has been raised.
        IsNotText, // What was read cannot be decoded, which is something wrong with the source. `whyNotText` says why.
    };
    String whyNotText;
    // Adds the next line to what there is.
    virtual Result readLine(Vector<char16_t>&) = 0;
};

// A token, and as much of what CPython's tokenizer knows on giving it as the module _tokenize goes by. Places are in code units of TokenStream::source().
struct StreamedToken {
    TokenKind kind { TokenKind::EndMarker };
    bool hasText { false }; // Some are nowhere.
    unsigned start { 0 };
    unsigned end { 0 };
    unsigned line { 0 }; // tok->lineno
    unsigned firstLine { 0 }; // tok->first_lineno
    unsigned lineStart { 0 }; // tok->line_start
    unsigned multiLineStart { 0 }; // tok->multi_line_start
    unsigned inputEnd { 0 }; // tok->inp
    bool hasImplicitNewline { false }; // tok->implicit_newline
    bool isAtEndOfFile { false }; // tok->done == E_EOF
    // CPython's tokenizer goes by bytes of UTF-8, and takes one of them for a token where it has no reason to think that it is part of anything. If it is the first of several, the token ends in the middle of a character:
    // the last that is between `start` and `end`.
    bool endsInMiddleOfCharacter { false };
};

// The same scanner, for whoever wants the tokens one at a time and as they are written, and has the source a line at a time. No more of the source is asked for than it takes to make out the next token. A line is
// whatever it was given for one: lines are counted by how many times it has asked.
class TokenStream {
    WTF_MAKE_TZONE_ALLOCATED(TokenStream);
    WTF_MAKE_NONCOPYABLE(TokenStream);
public:
    // With `hasExtraTokens` there are comments and the ends of lines that mean nothing, and less is found fault with: `tok_extra_tokens`.
    TokenStream(VM&, LineSource&, bool hasExtraTokens);
    ~TokenStream();

    enum class Result : uint8_t {
        Token,
        Error, // See error().
        Failed, // The LineSource did.
    };
    // After TokenKind::EndMarker, and after anything but a token, it is not to be asked again.
    Result next(StreamedToken&);

    // All that has been read.
    std::span<const char16_t> source() const;
    const SyntaxError& error() const;
    // What there is to warn of since this was last asked.
    Vector<SyntaxWarning> takeWarnings();
    // Of where it stopped: tok->lineno, tok->buf and tok->inp.
    unsigned line() const;
    unsigned bufferStart() const;
    // Where the line of that number begins.
    unsigned startOfLine(unsigned line) const;

private:
    struct Implementation;
    const std::unique_ptr<Implementation> m_implementation;
};

// The same scanner again, for the parser, of what is typed at a prompt. No more is asked for than the parser has asked for, and it is that which settles when a statement is over: when the parser wants no more. The arena is to
// say that it is typed.
class TypedTokens {
    WTF_MAKE_TZONE_ALLOCATED(TypedTokens);
    WTF_MAKE_NONCOPYABLE(TypedTokens);
public:
    TypedTokens(VM&, Arena&, LineSource&, Vector<SyntaxWarning>&);
    ~TypedTokens();

    const TokenBuffer& tokens() const;
    // Makes another, or a few if they come together. False if there are no more to be made, the last having been TokenKind::EndMarker or TokenKind::Error.
    bool fill();
    bool isExhausted() const;
    // From here on nothing is read, and what has been read is all that there is: IUNDERFLOW_STOP. It is for going over what is wrong to see what to say of it, which is no reason to ask for more.
    void stopReading();

    bool hasFailedToRead() const; // The LineSource did, and has raised something.
    const SyntaxError& error() const; // Why the last is TokenKind::Error, if it was not that.
    // All that has been read.
    std::span<const char16_t> source() const;

private:
    struct Implementation;
    const std::unique_ptr<Implementation> m_implementation;
};

} } // namespace JSC::Python
