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
#include "PythonPosixModule.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include <syslog.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>

// The module syslog: Modules/syslogmodule.c of CPython. Like posix, it is for whoever embeds the engine to say whether a program is to have it.

namespace JSC { namespace Python {

namespace {

// There is one log to a process, whatever there is more than one of besides.
struct Log {
    Lock lock;
    // openlog() keeps hold of what it is given and makes no copy, so this is what it was last given for as long as it may be looked at.
    CString identifier WTF_GUARDED_BY_LOCK(lock);
    bool isOpen WTF_GUARDED_BY_LOCK(lock) { false };
};

Log& theLog()
{
    static NeverDestroyed<Log> log;
    return log;
}

// syslog_get_argv(): what follows the last slash of sys.argv[0]. Null if there is no telling, which is no reason not to go on.
String nameOfProgram(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue argv = sysAttribute(globalObject, "argv"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* list = argv ? tryList(argv) : nullptr;
    if (!list || !list->length())
        return { };
    JSValue script = list->getIndexQuickly(0);
    if (!script || !stringIn(script))
        return { };
    String text = asString(stringIn(script))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.isEmpty())
        return { };
    size_t slash = text.reverseFind('/');
    return slash == notFound ? text : text.substring(slash + 1);
}

// syslog_openlog_impl(). `identifier` is null if none was given.
void openLog(JSGlobalObject* globalObject, String identifier, int64_t options, int64_t facility)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (identifier.isNull()) {
        identifier = nameOfProgram(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
    }
    std::optional<ByteVector> encoded;
    if (!identifier.isNull()) {
        // PyUnicode_AsUTF8()
        encoded = encodeString(globalObject, jsString(vm, identifier), "utf-8"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, void());
    }
    bool isAllowed = audit(globalObject, "syslog.openlog"_s, identifier.isNull() ? jsUndefined() : JSValue(jsString(vm, identifier)), intFromInt64(globalObject, options), intFromInt64(globalObject, facility));
    RETURN_IF_EXCEPTION(scope, void());
    if (!isAllowed)
        return;
    Log& log = theLog();
    Locker locker { log.lock };
    CString kept = encoded ? CString(byteCast<char>(encoded->span())) : CString();
    openlog(encoded ? kept.data() : nullptr, static_cast<int>(options), static_cast<int>(facility));
    // Not until it has hold of the new one is the old one let go of.
    log.identifier = WTF::move(kept);
    log.isOpen = true;
}

} // anonymous namespace

// openlog(ident=<unrepresentable>, logoption=0, facility=LOG_USER)
PYTHON_NATIVE(syslogOpenlog)
{
    NATIVE_PROLOGUE();
    String identifier;
    if (JSValue given = args.at(0)) {
        if (!stringIn(given))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("openlog() argument 'ident' must be str, not "_s, typeNameOfArgument(globalObject, given))));
        identifier = asString(stringIn(given))->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    int64_t options = 0;
    if (JSValue given = args.at(1)) {
        auto converted = toCLong(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        options = *converted;
    }
    int64_t facility = LOG_USER;
    if (JSValue given = args.at(2)) {
        auto converted = toCLong(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        facility = *converted;
    }
    openLog(globalObject, identifier, options, facility);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// syslog([priority=LOG_INFO,] message)
PYTHON_NATIVE(syslogSyslog)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "syslog() takes no keyword arguments"_s));
    if (args.size() < 1 || args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, "syslog.syslog requires 1 to 2 arguments"_s));
    int priority = LOG_INFO;
    bool hasPriority = args.size() == 2;
    if (hasPriority) {
        auto converted = toCIntOfFormat(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        priority = *converted;
    }
    auto message = toTextArgument(globalObject, args[hasPriority], "syslog"_s, hasPriority ? "argument 2"_s : "argument 1"_s);
    RETURN_IF_EXCEPTION(scope, { });
    bool isAllowed = audit(globalObject, "syslog.syslog"_s, jsNumber(priority), jsString(vm, *message));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isAllowed)
        return { };
    bool isOpen;
    {
        Locker locker { theLog().lock };
        isOpen = theLog().isOpen;
    }
    if (!isOpen) {
        openLog(globalObject, { }, 0, LOG_USER);
        RETURN_IF_EXCEPTION(scope, { });
    }
    CString bytes = message->utf8();
    Locker locker { theLog().lock };
    syslog(priority, "%s", bytes.data());
    RETURN_NONE();
}

PYTHON_NATIVE(syslogCloselog)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    bool isAllowed = audit(globalObject, "syslog.closelog"_s);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isAllowed)
        return { };
    Log& log = theLog();
    Locker locker { log.lock };
    if (log.isOpen) {
        closelog();
        log.identifier = { };
        log.isOpen = false;
    }
    RETURN_NONE();
}

// setlogmask(maskpri, /)
PYTHON_NATIVE(syslogSetlogmask)
{
    NATIVE_PROLOGUE();
    auto mask = toCLong(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    bool isAllowed = audit(globalObject, "syslog.setlogmask"_s, intFromInt64(globalObject, *mask));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isAllowed)
        return { };
    return JSValue::encode(jsNumber(setlogmask(static_cast<int>(*mask))));
}

// What `1 << by` comes to where CPython is compiled for: an int is shifted by as much of `by` as says how far an int can be. C does not say.
static int32_t oneShiftedBy(int64_t by)
{
    return static_cast<int32_t>(1u << (static_cast<uint64_t>(by) & 31));
}

// LOG_MASK(pri, /)
PYTHON_NATIVE(syslogLogMask)
{
    NATIVE_PROLOGUE();
    auto priority = toCLong(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(oneShiftedBy(*priority)));
}

// LOG_UPTO(pri, /)
PYTHON_NATIVE(syslogLogUpTo)
{
    NATIVE_PROLOGUE();
    auto priority = toCLong(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsNumber(static_cast<int32_t>(static_cast<uint32_t>(oneShiftedBy(static_cast<int64_t>(static_cast<uint64_t>(*priority) + 1))) - 1)));
}

JSObject* createSyslogModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "syslog"_s);
    addFunction(globalObject, module, "openlog"_s, syslogOpenlog);
    addFunction(globalObject, module, "closelog"_s, syslogCloselog);
    addFunction(globalObject, module, "syslog"_s, syslogSyslog, 0, "($module, /, *args, **kwargs)"_s, PyNativeFunction::Arguments::AreNotChecked);
    addFunction(globalObject, module, "setlogmask"_s, syslogSetlogmask);
    addFunction(globalObject, module, "LOG_MASK"_s, syslogLogMask);
    addFunction(globalObject, module, "LOG_UPTO"_s, syslogLogUpTo);

#define ADD(name) module->putDirect(vm, Identifier::fromString(vm, #name ""_s), jsNumber(name))
    // Priorities
    ADD(LOG_EMERG);
    ADD(LOG_ALERT);
    ADD(LOG_CRIT);
    ADD(LOG_ERR);
    ADD(LOG_WARNING);
    ADD(LOG_NOTICE);
    ADD(LOG_INFO);
    ADD(LOG_DEBUG);
    // What openlog() may be asked for
    ADD(LOG_PID);
    ADD(LOG_CONS);
    ADD(LOG_NDELAY);
#ifdef LOG_ODELAY
    ADD(LOG_ODELAY);
#endif
#ifdef LOG_NOWAIT
    ADD(LOG_NOWAIT);
#endif
#ifdef LOG_PERROR
    ADD(LOG_PERROR);
#endif
    // Facilities
    ADD(LOG_KERN);
    ADD(LOG_USER);
    ADD(LOG_MAIL);
    ADD(LOG_DAEMON);
    ADD(LOG_AUTH);
    ADD(LOG_LPR);
    ADD(LOG_LOCAL0);
    ADD(LOG_LOCAL1);
    ADD(LOG_LOCAL2);
    ADD(LOG_LOCAL3);
    ADD(LOG_LOCAL4);
    ADD(LOG_LOCAL5);
    ADD(LOG_LOCAL6);
    ADD(LOG_LOCAL7);
    ADD(LOG_SYSLOG);
    ADD(LOG_CRON);
    ADD(LOG_UUCP);
    ADD(LOG_NEWS);
#ifdef LOG_AUTHPRIV
    ADD(LOG_AUTHPRIV);
#endif
#ifdef LOG_FTP
    ADD(LOG_FTP);
#endif
#ifdef LOG_NETINFO
    ADD(LOG_NETINFO);
#endif
#ifdef LOG_REMOTEAUTH
    ADD(LOG_REMOTEAUTH);
#endif
#ifdef LOG_INSTALL
    ADD(LOG_INSTALL);
#endif
#ifdef LOG_RAS
    ADD(LOG_RAS);
#endif
#ifdef LOG_LAUNCHD
    ADD(LOG_LAUNCHD);
#endif
#undef ADD
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
