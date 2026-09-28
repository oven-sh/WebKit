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
};

// The scanner. It alone reads the source: what the parser needs to know is all in the tokens.
// True if it got to the end. Otherwise the error says why not, and the tokens are those before it and then TokenKind::Error.
bool tokenize(VM&, Arena&, StringView source, const ScanRange&, Vector<Token>&, Vector<SyntaxWarning>&, SyntaxError&);

} } // namespace JSC::Python
