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

#include "config.h"
#include "NonEmbeddedCode.h"

#if USE(BUN_JSC_ADDITIONS)

#include "CachedBytecode.h"
#include "Error.h"
#include "JSCConfig.h"
#include "JSGlobalObject.h"
#include "SourceProvider.h"
#include "StackVisitor.h"
#include "ThrowScope.h"
#include "VM.h"
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>

namespace JSC {
namespace NonEmbeddedCode {

static void reportIfNeeded(VM& vm, SourceProvider& provider, ASCIILiteral site)
{
    if (!Options::reportNonEmbeddedCode())
        return;

    static constexpr unsigned maxFrames = 16;
    StringBuilder report;
    report.append("[non-embedded code] "_s, site, ": "_s, provider.sourceURL().isEmpty() ? "(no source URL)"_s : StringView(provider.sourceURL()));
    if (CallFrame* callFrame = vm.topCallFrame) {
        unsigned frameCount = 0;
        StackVisitor::visit(callFrame, vm, [&](StackVisitor& visitor) {
            report.append("\n    at "_s, visitor->toString());
            return ++frameCount < maxFrames ? IterationStatus::Continue : IterationStatus::Done;
        });
    }

    static Lock reportedMessagesLock;
    static NeverDestroyed<HashSet<String>> reportedMessages;
    String message = report.toString();
    {
        Locker locker { reportedMessagesLock };
        if (!reportedMessages->add(message.isolatedCopy()).isNewEntry)
            return;
    }
    dataLogLn(message);
}

bool throwIfDisallowedSlow(JSGlobalObject* globalObject, ThrowScope& scope, SourceProvider& provider, ASCIILiteral site)
{
    if (provider.isBuiltinOrEmbedded())
        return false;
    reportIfNeeded(globalObject->vm(), provider, site);
    if (!Options::disallowNonEmbeddedCode())
        return false;
    const String& url = provider.sourceURL();
    throwException(globalObject, scope, createEvalError(globalObject, url.isEmpty() ? "Refused to run code that is not embedded in the executable"_str : makeString("Refused to run "_s, url, " because it is not embedded in the executable"_s)));
    return true;
}

bool throwIfEvalDisallowedSlow(JSGlobalObject* globalObject, ThrowScope& scope, const String& programSource)
{
    Ref provider = StringSourceProvider::create(programSource, SourceOrigin(), String(), SourceTaintedOrigin::Untainted);
    return throwIfDisallowedSlow(globalObject, scope, provider.get(), "eval"_s);
}

bool isBytecodeDisallowedSlow(VM& vm, SourceProvider& provider, const CachedBytecode& bytecode)
{
    auto isEmbeddedCode = g_jscConfig.isEmbeddedCode;
    if (isEmbeddedCode && isEmbeddedCode(provider.sourceBytes(), bytecode.span(), bytecode.entryOffset()))
        return false;
    reportIfNeeded(vm, provider, "bytecode"_s);
    return Options::disallowNonEmbeddedCode();
}

} // namespace NonEmbeddedCode
} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
