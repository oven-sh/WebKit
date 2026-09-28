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

#include <wtf/text/WTFString.h>

namespace JSC { namespace Python {

// What is wrong with a piece of source, as far as it takes to make the exception.
struct SyntaxError {
    enum class Kind : uint8_t { SyntaxError, IndentationError, TabError, IncompleteInputError };

    explicit operator bool() const { return !message.isNull(); }

    Kind kind { Kind::SyntaxError };
    bool isAtEndOfSource { false }; // The scanner stopped because the source ended: in brackets, in a string, or after a backslash.
    String message;
    unsigned line { 0 };
    // Columns are from 0 and in bytes of UTF-8. What a program is given is one more, and in characters. Some of what CPython gives is 0 or -1, which here is -1 or -2.
    int column { 0 };
    unsigned endLine { 0 };
    int endColumn { 0 };
    // In CPython there are two kinds of thing that its tokenizer can find wrong. One kind it raises an exception for itself. That comes before whatever its parser has found wrong earlier in the
    // source, and the line that goes with it is without the end of the line. For the other it only says that it can go no further, and the parser says why if it gets that far.
    bool isFromTokenizer { false };
    // It was in an f-string when it stopped, and so what the parser has found wrong is not to give way to it.
    bool isInsideFString { false };
    // The line that CPython's tokenizer, which is asked for one token at a time, would have got to. If that is beyond the line that is wrong, the line is fetched again, and comes without its end.
    unsigned tokenizerLine { 0 };
    // The line that goes with it is all that the tokenizer has read since the line began, which is the rest of the source: it was in a string that goes over more lines than one.
    bool lineGoesOnToTheEnd { false };
    // Whether a last line that nothing ends is taken to be ended. See ScanRange::LastLine.
    bool lastLineIsEnded { true };
    // Where the scanner stopped, the bracket that was opened last and not closed, if any.
    char openBracket { 0 };
    unsigned openBracketLine { 0 };
    unsigned openBracketColumn { 0 };
};

} } // namespace JSC::Python
