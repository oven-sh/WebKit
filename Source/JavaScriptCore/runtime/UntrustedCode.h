/*
 * Copyright (C) 2026 Anthropic PBC.
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

#if USE(BUN_JSC_ADDITIONS)

#include "Options.h"

namespace JSC {

class CachedBytecode;
class JSGlobalObject;
class SourceProvider;
class ThrowScope;
class VM;

// Options::refuseUntrustedCode() and Options::reportUntrustedCode(). A source is trusted for where its text is, not for
// what made it: SourceProvider::isTrusted() asks about the characters the parser reads, so a provider nobody thought of
// is not trusted. Nothing can be declared trusted, and there is no way back from the options once they are frozen.
namespace UntrustedCode {

enum class Site : uint8_t {
    Link, // ScriptExecutable::prepareForExecutionImpl(), which all new code passes on its way to a CodeBlock. The others only refuse earlier.
    Program, // Interpreter::executeProgram(), before the JSONP fast path (which runs no code of its own) and the global declarations.
    Module, // JSModuleLoader::makeModule(), before the parse and so before what the module imports is loaded.
    Bytecode, // decodeCodeBlockImpl() and decodeBuiltinFunction(). A CodeCache entry made from it would be handed to any provider of equal text.
};

ALWAYS_INLINE bool isChecked() { return Options::refuseUntrustedCode() || Options::reportUntrustedCode(); }

// These are for when isChecked(). They report what is not trusted, and return whether it is refused as well.
bool throwIfRefused(JSGlobalObject*, ThrowScope&, SourceProvider&, Site);
bool isBytecodeRefused(VM&, SourceProvider&, const CachedBytecode&);

} // namespace UntrustedCode

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
