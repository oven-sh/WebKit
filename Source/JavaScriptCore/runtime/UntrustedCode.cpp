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
#include "UntrustedCode.h"

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
namespace UntrustedCode {

static ASCIILiteral nameOf(Site site)
{
    switch (site) {
    case Site::Link:
        return "link"_s;
    case Site::Program:
        return "program"_s;
    case Site::Module:
        return "module"_s;
    case Site::Bytecode:
        return "bytecode"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportIfNeeded(VM& vm, SourceProvider& provider, Site site)
{
    if (!Options::reportUntrustedCode())
        return;

    static constexpr unsigned maxFrames = 16;
    StringBuilder report;
    report.append("[untrusted code] "_s, nameOf(site), ": "_s, provider.sourceURL().isEmpty() ? "(no source URL)"_s : StringView(provider.sourceURL()));
    if (CallFrame* callFrame = vm.topCallFrame) {
        unsigned frames = 0;
        StackVisitor::visit(callFrame, vm, [&](StackVisitor& visitor) {
            report.append("\n    at "_s, visitor->toString());
            return ++frames < maxFrames ? IterationStatus::Continue : IterationStatus::Done;
        });
    }

    static Lock lock;
    static NeverDestroyed<HashSet<String>> reported;
    String text = report.toString();
    {
        Locker locker { lock };
        if (!reported->add(text.isolatedCopy()).isNewEntry)
            return;
    }
    dataLogLn(text);
}

bool throwIfRefused(JSGlobalObject* globalObject, ThrowScope& scope, SourceProvider& provider, Site site)
{
    ASSERT(isChecked());
    if (provider.isTrusted())
        return false;
    reportIfNeeded(globalObject->vm(), provider, site);
    if (!Options::refuseUntrustedCode())
        return false;
    const String& url = provider.sourceURL();
    throwException(globalObject, scope, createEvalError(globalObject, url.isEmpty() ? "Refused to run untrusted code"_str : makeString("Refused to run untrusted code: "_s, url)));
    return true;
}

bool isBytecodeRefused(VM& vm, SourceProvider& provider, const CachedBytecode& bytecode)
{
    ASSERT(isChecked());
    auto isTrustedCode = g_jscConfig.isTrustedCode;
    if (isTrustedCode && isTrustedCode(provider.sourceBytes(), bytecode.span(), bytecode.entryOffset()))
        return false;
    reportIfNeeded(vm, provider, Site::Bytecode);
    return Options::refuseUntrustedCode();
}

} // namespace UntrustedCode
} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
