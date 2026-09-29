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
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonIO.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include "PythonSequences.h"
#include <sys/resource.h>
#include <unistd.h>

// The module resource: Modules/resource.c of CPython. Like posix, it is for whoever embeds the engine to say whether a program is to have it.

namespace JSC { namespace Python {

namespace {

// py2rlim(). Nothing if it raised.
std::optional<rlim_t> toLimit(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    static_assert(sizeof(rlim_t) == sizeof(uint64_t));
    JSValue integer = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    Number number = classify(integer);
    bool isNegative = number.kind == Number::Kind::Small ? number.small < 0 : number.big->sign();
    if (isNegative) {
        // Only what is written that way where it stands for there being no limit
        auto small = tryInt64(integer);
        if (!small || static_cast<rlim_t>(*small) != RLIM_INFINITY) {
            raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
            return std::nullopt;
        }
        return static_cast<rlim_t>(*small);
    }
    auto digits = digitsOfInt(number);
    if (digits.size() > 1) {
        raise(globalObject, scope, BuiltinType::OverflowError, "Python int too large to convert to C rlim_t"_s);
        return std::nullopt;
    }
    return digits.isEmpty() ? 0 : digits[0];
}

// py2rlimit(). Nothing if it raised.
std::optional<struct rlimit> toLimits(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyTuple* limits = tupleFromIterable(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (limits->length() != 2) {
        raiseValueError(globalObject, scope, "expected a tuple of 2 integers"_s);
        return std::nullopt;
    }
    auto current = toLimit(globalObject, limits->at(0));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto maximum = toLimit(globalObject, limits->at(1));
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    struct rlimit result;
    result.rlim_cur = *current;
    result.rlim_max = *maximum;
    return result;
}

// rlim2py(): what stands for there being no limit has a sign, if the system writes it with all its bits set.
JSValue fromLimit(JSGlobalObject* globalObject, rlim_t value)
{
    if (value == RLIM_INFINITY)
        return intFromInt64(globalObject, static_cast<int64_t>(value));
    return intFromUInt64(globalObject, value);
}

JSValue fromLimits(JSGlobalObject* globalObject, const struct rlimit& limits)
{
    return PyTuple::create(globalObject, { fromLimit(globalObject, limits.rlim_cur), fromLimit(globalObject, limits.rlim_max) });
}

bool isResource(int resource) { return resource >= 0 && resource < static_cast<int>(RLIM_NLIMITS); }

} // anonymous namespace

// getrusage(who, /)
PYTHON_NATIVE(resourceGetUsage)
{
    NATIVE_PROLOGUE();
    auto who = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct rusage usage;
    if (getrusage(*who, &usage) == -1) {
        if (errno == EINVAL)
            return JSValue::encode(raiseValueError(globalObject, scope, "invalid who parameter"_s));
        return JSValue::encode(raisePosixError(globalObject, scope));
    }
    return JSValue::encode(newResourceUsage(globalObject, usage));
}

// getrlimit(resource, /)
PYTHON_NATIVE(resourceGetLimit)
{
    NATIVE_PROLOGUE();
    auto resource = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isResource(*resource))
        return JSValue::encode(raiseValueError(globalObject, scope, "invalid resource specified"_s));
    struct rlimit limits;
    if (getrlimit(*resource, &limits) == -1)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(fromLimits(globalObject, limits));
}

// setrlimit(resource, limits, /)
PYTHON_NATIVE(resourceSetLimit)
{
    NATIVE_PROLOGUE();
    auto resource = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isResource(*resource))
        return JSValue::encode(raiseValueError(globalObject, scope, "invalid resource specified"_s));
    if (!audit(globalObject, "resource.setrlimit"_s, jsNumber(*resource), args[1]))
        return { };
    auto limits = toLimits(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (setrlimit(*resource, &*limits) == -1) {
        if (errno == EINVAL)
            return JSValue::encode(raiseValueError(globalObject, scope, "current limit exceeds maximum limit"_s));
        if (errno == EPERM)
            return JSValue::encode(raiseValueError(globalObject, scope, "not allowed to raise maximum limit"_s));
        return JSValue::encode(raisePosixError(globalObject, scope));
    }
    RETURN_NONE();
}

#if OS(LINUX)
// prlimit(pid, resource, limits=None, /)
PYTHON_NATIVE(resourceProcessLimit)
{
    NATIVE_PROLOGUE();
    auto process = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto resource = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue given = args.size() > 2 ? args[2] : jsUndefined();
    if (!isResource(*resource))
        return JSValue::encode(raiseValueError(globalObject, scope, "invalid resource specified"_s));
    if (!audit(globalObject, "resource.prlimit"_s, jsNumber(*process), jsNumber(*resource), given))
        return { };
    struct rlimit old;
    int result;
    if (!isNone(given)) {
        auto limits = toLimits(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        result = prlimit(*process, static_cast<__rlimit_resource>(*resource), &*limits, &old);
    } else
        result = prlimit(*process, static_cast<__rlimit_resource>(*resource), nullptr, &old);
    if (result == -1) {
        if (errno == EINVAL)
            return JSValue::encode(raiseValueError(globalObject, scope, "current limit exceeds maximum limit"_s));
        return JSValue::encode(raisePosixError(globalObject, scope));
    }
    return JSValue::encode(fromLimits(globalObject, old));
}
#endif

PYTHON_NATIVE(resourceGetPageSize)
{
    UNUSED_PARAM(globalObject);
    UNUSED_PARAM(callFrame);
    return JSValue::encode(jsNumber(getpagesize()));
}

JSObject* createResourceModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "resource"_s);
    addFunction(globalObject, module, "getrusage"_s, resourceGetUsage);
    addFunction(globalObject, module, "getrlimit"_s, resourceGetLimit);
#if OS(LINUX)
    addFunction(globalObject, module, "prlimit"_s, resourceProcessLimit, 0, "($module, pid, resource, limits=None, /)"_s);
#endif
    addFunction(globalObject, module, "setrlimit"_s, resourceSetLimit);
    addFunction(globalObject, module, "getpagesize"_s, resourceGetPageSize);
    auto add = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };
    add("error"_s, realm->type(BuiltinType::OSError)->object());
    add("struct_rusage"_s, posixState(globalObject).resourceUsage->object());
#define ADD_INT(name) add(#name ""_s, jsNumber(static_cast<int>(name)))
#ifdef RLIMIT_CPU
    ADD_INT(RLIMIT_CPU);
#endif
#ifdef RLIMIT_FSIZE
    ADD_INT(RLIMIT_FSIZE);
#endif
#ifdef RLIMIT_DATA
    ADD_INT(RLIMIT_DATA);
#endif
#ifdef RLIMIT_STACK
    ADD_INT(RLIMIT_STACK);
#endif
#ifdef RLIMIT_CORE
    ADD_INT(RLIMIT_CORE);
#endif
#ifdef RLIMIT_NOFILE
    ADD_INT(RLIMIT_NOFILE);
#endif
#ifdef RLIMIT_OFILE
    ADD_INT(RLIMIT_OFILE);
#endif
#ifdef RLIMIT_VMEM
    ADD_INT(RLIMIT_VMEM);
#endif
#ifdef RLIMIT_AS
    ADD_INT(RLIMIT_AS);
#endif
#ifdef RLIMIT_RSS
    ADD_INT(RLIMIT_RSS);
#endif
#ifdef RLIMIT_NPROC
    ADD_INT(RLIMIT_NPROC);
#endif
#ifdef RLIMIT_MEMLOCK
    ADD_INT(RLIMIT_MEMLOCK);
#endif
#ifdef RLIMIT_SBSIZE
    ADD_INT(RLIMIT_SBSIZE);
#endif
    // Linux
#ifdef RLIMIT_MSGQUEUE
    ADD_INT(RLIMIT_MSGQUEUE);
#endif
#ifdef RLIMIT_NICE
    ADD_INT(RLIMIT_NICE);
#endif
#ifdef RLIMIT_RTPRIO
    ADD_INT(RLIMIT_RTPRIO);
#endif
#ifdef RLIMIT_RTTIME
    ADD_INT(RLIMIT_RTTIME);
#endif
#ifdef RLIMIT_SIGPENDING
    ADD_INT(RLIMIT_SIGPENDING);
#endif
#ifdef RUSAGE_SELF
    ADD_INT(RUSAGE_SELF);
#endif
#ifdef RUSAGE_CHILDREN
    ADD_INT(RUSAGE_CHILDREN);
#endif
#ifdef RUSAGE_BOTH
    ADD_INT(RUSAGE_BOTH);
#endif
#ifdef RUSAGE_THREAD
    ADD_INT(RUSAGE_THREAD);
#endif
    // FreeBSD
#ifdef RLIMIT_SWAP
    ADD_INT(RLIMIT_SWAP);
#endif
#ifdef RLIMIT_NPTS
    ADD_INT(RLIMIT_NPTS);
#endif
#ifdef RLIMIT_KQUEUES
    ADD_INT(RLIMIT_KQUEUES);
#endif
#undef ADD_INT
    add("RLIM_INFINITY"_s, fromLimit(globalObject, RLIM_INFINITY));
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
