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

#include "PythonAST.h"
#include "PythonArena.h"
#include "PythonToken.h"
#include <wtf/text/StringView.h>

namespace JSC {

class VM;

namespace Python {

// Null if the source is not Python, and then the error says why not. The tree lives as long as the arena.
Module* parse(VM&, Arena&, StringView source, Module::Kind, Vector<SyntaxWarning>&, SyntaxError&);

// One statement that is typed at a prompt, which is read as far as it takes to parse it and no further: _PyParser_InteractiveASTFromFile(). Null, with no error, if there was nothing to read, and then `isAtEndOfInput`; or if
// reading failed, which the tokens say.
class TypedTokens;
Module* parseTyped(VM&, Arena&, TypedTokens&, SyntaxError&, bool& isAtEndOfInput);

// One definition (def or class), or one expression (a lambda or a generator expression), out of the middle of a source that has been
// parsed before, and so is known to be Python. `start` and `end` are what the node had for them then, and `line` for its line.
Statement* parseDefinition(VM&, Arena&, StringView source, unsigned start, unsigned end, unsigned line);
Expression* parseExpression(VM&, Arena&, StringView source, unsigned start, unsigned end, unsigned line);

} } // namespace JSC::Python
