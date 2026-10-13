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

#include <JavaScriptCore/Options.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class CachedBytecode;
class JSGlobalObject;
class SourceProvider;
class ThrowScope;
class VM;

// Options::disallowNonEmbeddedCode() and Options::reportNonEmbeddedCode(). What is embedded: SourceProvider::isBuiltinOrEmbedded().
namespace NonEmbeddedCode {

ALWAYS_INLINE bool isDisallowedOrReported() { return Options::disallowNonEmbeddedCode() || Options::reportNonEmbeddedCode(); }

bool throwIfDisallowedSlow(JSGlobalObject*, ThrowScope&, SourceProvider&, ASCIILiteral site);
bool throwIfEvalDisallowedSlow(JSGlobalObject*, ThrowScope&, const String& programSource);
bool isBytecodeDisallowedSlow(VM&, SourceProvider&, const CachedBytecode&);

// These report what is not embedded, under the name of the site. They return whether it is disallowed as well.
ALWAYS_INLINE bool throwIfDisallowed(JSGlobalObject* globalObject, ThrowScope& scope, SourceProvider& provider, ASCIILiteral site)
{
    return isDisallowedOrReported() && throwIfDisallowedSlow(globalObject, scope, provider, site);
}

ALWAYS_INLINE bool throwIfEvalDisallowed(JSGlobalObject* globalObject, ThrowScope& scope, const String& programSource)
{
    return isDisallowedOrReported() && throwIfEvalDisallowedSlow(globalObject, scope, programSource);
}

ALWAYS_INLINE bool isBytecodeDisallowed(VM& vm, SourceProvider& provider, const CachedBytecode& bytecode)
{
    return isDisallowedOrReported() && isBytecodeDisallowedSlow(vm, provider, bytecode);
}

} // namespace NonEmbeddedCode

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
