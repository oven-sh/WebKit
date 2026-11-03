/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
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

#include "ConsoleTypes.h"
#include "JSExportMacros.h"
#include <wtf/CheckedRef.h>
#include <wtf/Forward.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>

namespace Inspector {
class ScriptArguments;
}

namespace JSC {

class ArgList;
class CallFrame;
class JSGlobalObject;

class ConsoleClient : public CanMakeWeakPtr<ConsoleClient>, public CanMakeThreadSafeCheckedPtr<ConsoleClient> {
    WTF_MAKE_TZONE_ALLOCATED(ConsoleClient);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ConsoleClient);
public:
    virtual ~ConsoleClient() { }

#if USE(BUN_JSC_ADDITIONS)
    // The arguments of a console call, read where the call left them: nothing is copied, and they
    // are valid until the client returns. A client that keeps them makes ScriptArguments from them
    // (Inspector::createScriptArguments).
    using MessageArguments = ArgList;
#else
    using MessageArguments = Ref<Inspector::ScriptArguments>&&;
#endif

    JS_EXPORT_PRIVATE static void printConsoleMessage(MessageSource, MessageType, MessageLevel, const String& message, const String& url, unsigned lineNumber, unsigned columnNumber);
    JS_EXPORT_PRIVATE static void printConsoleMessageWithArguments(MessageSource, MessageType, MessageLevel, JSC::JSGlobalObject*, Ref<Inspector::ScriptArguments>&&);

    void logWithLevel(JSGlobalObject*, MessageArguments, MessageLevel);
    void clear(JSGlobalObject*);
    void dir(JSGlobalObject*, MessageArguments);
    void dirXML(JSGlobalObject*, MessageArguments);
    void table(JSGlobalObject*, MessageArguments);
    void trace(JSGlobalObject*, MessageArguments);
    void assertion(JSGlobalObject*, MessageArguments);
    void group(JSGlobalObject*, MessageArguments);
    void groupCollapsed(JSGlobalObject*, MessageArguments);
    void groupEnd(JSGlobalObject*, MessageArguments);

    virtual void messageWithTypeAndLevel(MessageType, MessageLevel, JSC::JSGlobalObject*, MessageArguments) = 0;
    virtual void count(JSGlobalObject*, const String& label) = 0;
    virtual void countReset(JSGlobalObject*, const String& label) = 0;
    virtual void profile(JSGlobalObject*, const String& title) = 0;
    virtual void profileEnd(JSGlobalObject*, const String& title) = 0;
    virtual void takeHeapSnapshot(JSGlobalObject*, const String& title) = 0;
    virtual void time(JSGlobalObject*, const String& label) = 0;
    virtual void timeLog(JSGlobalObject*, const String& label, MessageArguments) = 0;
    virtual void timeEnd(JSGlobalObject*, const String& label) = 0;
    virtual void timeStamp(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) = 0;
    virtual void record(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) = 0;
    virtual void recordEnd(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) = 0;
    virtual void screenshot(JSGlobalObject*, Ref<Inspector::ScriptArguments>&&) = 0;

private:
    enum class ArgumentRequirement { No, Yes };
    void internalMessageWithTypeAndLevel(MessageType, MessageLevel, JSC::JSGlobalObject*, MessageArguments, ArgumentRequirement);
};

} // namespace JSC
