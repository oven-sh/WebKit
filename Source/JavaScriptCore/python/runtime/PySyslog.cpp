/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <syslog.h>
#include <wtf/Lock.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module syslog. (Modules/syslogmodule.c in CPython is the reference.)

// The log is the process's, not the VM's. openlog(3) keeps the pointer it is given.
static Lock s_syslogLock;
static bool s_syslogIsOpen WTF_GUARDED_BY_LOCK(s_syslogLock);
static PosixBytes& syslogIdent()
{
    static NeverDestroyed<PosixBytes> ident;
    return ident;
}

// The name of the script: what follows the last slash of sys.argv[0].
static JSString* syslogDefaultIdent(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue argv = rt.sysDict()->get(internString(vm, "argv"_s));
    JSArray* list = argv ? tryAsList(argv) : nullptr;
    if (!list || !listLength(list) || !isStr(listGet(list, 0)))
        return nullptr;
    String script = stringOf(strValue(listGet(list, 0)));
    if (script.isEmpty())
        return nullptr;
    size_t slash = script.reverseFind('/');
    return makeStr(vm, slash == notFound ? script : script.substring(slash + 1));
}

static bool syslogOpen(JSGlobalObject* globalObject, JSString* ident, int64_t options, int64_t facility)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!ident)
        ident = syslogDefaultIdent(globalObject);
    if (failed(scope, audit(globalObject, "syslog.openlog"_s, ident, posixMakeInt(globalObject, options), posixMakeInt(globalObject, facility))))
        return false;
    PosixBytes encoded;
    if (ident && failed(scope, encoded.assign(globalObject, ident)))
        return false;
    Locker locker { s_syslogLock };
    // Closed first, so that nothing uses the previous name while it is overwritten.
    if (s_syslogIsOpen)
        closelog();
    syslogIdent().assign(encoded.span());
    openlog(ident ? syslogIdent().characters() : nullptr, static_cast<int>(options), static_cast<int>(facility));
    s_syslogIsOpen = true;
    return true;
}

static JSValue syslogOpenLog(JSGlobalObject* globalObject, JSValue identValue, JSValue optionsValue, JSValue facilityValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t options = 0;
    int64_t facility = LOG_USER;
    if (identValue && !isStr(identValue))
        return raiseTypeError(globalObject, scope, "openlog() argument 'ident' must be str, not "_s, posixTypeNameOfArgument(globalObject, identValue));
    if ((optionsValue && failed(scope, posixToLong(globalObject, optionsValue, options))) || (facilityValue && failed(scope, posixToLong(globalObject, facilityValue, facility))))
        return { };
    if (failed(scope, syslogOpen(globalObject, identValue ? strValue(identValue) : nullptr, options, facility)))
        return { };
    return jsUndefined();
}

// ([priority,] message)
static JSValue syslogSyslog(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.keywordCount())
        return raiseTypeError(globalObject, scope, "syslog() takes no keyword arguments"_s);
    if (args.size() != 1 && args.size() != 2)
        return raiseTypeError(globalObject, scope, "syslog.syslog requires 1 to 2 arguments"_s);
    int priority = LOG_INFO;
    if (args.size() == 2) {
        // As the format "i" words it.
        int64_t number;
        if (failed(scope, posixToLong(globalObject, args[0], number)))
            return { };
        if (number > INT_MAX)
            return raiseOverflowError(globalObject, scope, "signed integer is greater than maximum"_s);
        if (number < INT_MIN)
            return raiseOverflowError(globalObject, scope, "signed integer is less than minimum"_s);
        priority = static_cast<int>(number);
    }
    JSValue messageValue = args[args.size() - 1];
    if (!isStr(messageValue))
        return raiseTypeError(globalObject, scope, "syslog() argument "_s, args.size(), " must be str, not "_s, posixTypeNameOfArgument(globalObject, messageValue));
    PosixBytes message;
    if (failed(scope, message.assign(globalObject, strValue(messageValue))))
        return { };
    if (message.contains('\0'))
        return raiseValueError(globalObject, scope, "embedded null character"_s);
    if (failed(scope, audit(globalObject, "syslog.syslog"_s, jsNumber(priority), messageValue)))
        return { };
    bool isOpen;
    {
        Locker locker { s_syslogLock };
        isOpen = s_syslogIsOpen;
    }
    if (!isOpen && failed(scope, syslogOpen(globalObject, nullptr, 0, LOG_USER)))
        return { };
    syslog(priority, "%s", message.characters());
    return jsUndefined();
}

static JSValue syslogCloseLog(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, audit(globalObject, "syslog.closelog"_s)))
        return { };
    Locker locker { s_syslogLock };
    if (std::exchange(s_syslogIsOpen, false))
        closelog();
    return jsUndefined();
}

static JSValue syslogSetLogMask(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t mask;
    if (failed(scope, posixToLong(globalObject, value, mask)))
        return { };
    if (failed(scope, audit(globalObject, "syslog.setlogmask"_s, posixMakeInt(globalObject, mask))))
        return { };
    Locker locker { s_syslogLock };
    return jsNumber(setlogmask(static_cast<int>(mask)));
}

static JSValue syslogMask(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t priority;
    if (failed(scope, posixToLong(globalObject, value, priority)))
        return { };
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, static_cast<int64_t>(static_cast<int>(1u << (priority & 31)))));
}

static JSValue syslogUpTo(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t priority;
    if (failed(scope, posixToLong(globalObject, value, priority)))
        return { };
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, static_cast<int64_t>(static_cast<int>((1u << ((priority + 1) & 31)) - 1))));
}

static constexpr NativeDef syslogFunctions[] = {
    PY_FUNCTION("openlog", syslogOpenLog, "(ident=<unrepresentable>, logoption=0, facility=LOG_USER)"),
    PY_FUNCTION("syslog", syslogSyslog, "(*args)"),
    PY_FUNCTION("closelog", syslogCloseLog, "()"),
    PY_FUNCTION("setlogmask", syslogSetLogMask, "(maskpri, /)"),
    PY_FUNCTION("LOG_MASK", syslogMask, "(pri, /)"),
    PY_FUNCTION("LOG_UPTO", syslogUpTo, "(pri, /)"),
};

#define SYSLOG_CONSTANT(name) { #name ""_s, static_cast<int64_t>(name) }

static constexpr PosixConstant syslogConstants[] = {
    SYSLOG_CONSTANT(LOG_EMERG),
    SYSLOG_CONSTANT(LOG_ALERT),
    SYSLOG_CONSTANT(LOG_CRIT),
    SYSLOG_CONSTANT(LOG_ERR),
    SYSLOG_CONSTANT(LOG_WARNING),
    SYSLOG_CONSTANT(LOG_NOTICE),
    SYSLOG_CONSTANT(LOG_INFO),
    SYSLOG_CONSTANT(LOG_DEBUG),
    SYSLOG_CONSTANT(LOG_PID),
    SYSLOG_CONSTANT(LOG_CONS),
    SYSLOG_CONSTANT(LOG_NDELAY),
#ifdef LOG_ODELAY
    SYSLOG_CONSTANT(LOG_ODELAY),
#endif
#ifdef LOG_NOWAIT
    SYSLOG_CONSTANT(LOG_NOWAIT),
#endif
#ifdef LOG_PERROR
    SYSLOG_CONSTANT(LOG_PERROR),
#endif
    SYSLOG_CONSTANT(LOG_KERN),
    SYSLOG_CONSTANT(LOG_USER),
    SYSLOG_CONSTANT(LOG_MAIL),
    SYSLOG_CONSTANT(LOG_DAEMON),
    SYSLOG_CONSTANT(LOG_AUTH),
    SYSLOG_CONSTANT(LOG_LPR),
    SYSLOG_CONSTANT(LOG_LOCAL0),
    SYSLOG_CONSTANT(LOG_LOCAL1),
    SYSLOG_CONSTANT(LOG_LOCAL2),
    SYSLOG_CONSTANT(LOG_LOCAL3),
    SYSLOG_CONSTANT(LOG_LOCAL4),
    SYSLOG_CONSTANT(LOG_LOCAL5),
    SYSLOG_CONSTANT(LOG_LOCAL6),
    SYSLOG_CONSTANT(LOG_LOCAL7),
    // Where one of these is unknown, mail's is as good as any.
#ifdef LOG_SYSLOG
    SYSLOG_CONSTANT(LOG_SYSLOG),
#else
    { "LOG_SYSLOG"_s, LOG_DAEMON },
#endif
#ifdef LOG_CRON
    SYSLOG_CONSTANT(LOG_CRON),
#else
    { "LOG_CRON"_s, LOG_DAEMON },
#endif
#ifdef LOG_UUCP
    SYSLOG_CONSTANT(LOG_UUCP),
#else
    { "LOG_UUCP"_s, LOG_MAIL },
#endif
#ifdef LOG_NEWS
    SYSLOG_CONSTANT(LOG_NEWS),
#else
    { "LOG_NEWS"_s, LOG_MAIL },
#endif
#ifdef LOG_AUTHPRIV
    SYSLOG_CONSTANT(LOG_AUTHPRIV),
#endif
#ifdef LOG_FTP
    SYSLOG_CONSTANT(LOG_FTP),
#endif
#ifdef LOG_NETINFO
    SYSLOG_CONSTANT(LOG_NETINFO),
#endif
#ifdef LOG_REMOTEAUTH
    SYSLOG_CONSTANT(LOG_REMOTEAUTH),
#endif
#ifdef LOG_INSTALL
    SYSLOG_CONSTANT(LOG_INSTALL),
#endif
#ifdef LOG_RAS
    SYSLOG_CONSTANT(LOG_RAS),
#endif
#ifdef LOG_LAUNCHD
    SYSLOG_CONSTANT(LOG_LAUNCHD),
#endif
};

void initializeSyslogModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, syslogFunctions);
    addPosixConstants(globalObject, module, syslogConstants);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
