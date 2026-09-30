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

#include "PythonPosix.h"

#if OS(UNIX)
#include <errno.h>
#include <unistd.h>
#endif

namespace JSC { namespace Python {

#if OS(UNIX)

void addPosixPathFunctions(JSGlobalObject*, JSObject*);
void addPosixDescriptorFunctions(JSGlobalObject*, JSObject*);
void addPosixScandir(JSGlobalObject*, JSObject*);
void addPosixIdentityFunctions(JSGlobalObject*, JSObject*);
void addPosixConstants(JSGlobalObject*, JSObject*);
JSValue newPosixPathconfNames(JSGlobalObject*);
JSValue newPosixConfstrNames(JSGlobalObject*);
JSValue newPosixSysconfNames(JSGlobalObject*);
JSValue newEnvironmentDict(JSGlobalObject*);

PosixModuleState& posixState(JSGlobalObject* globalObject)
{
    PosixModuleState& state = globalObject->pyRealm()->posixModule();
    if (state.statResult)
        return state;
    initializePosixFileTypes(globalObject, state);
    initializePosixProcessTypes(globalObject, state);
#if OS(LINUX)
    initializePosixLinuxTypes(globalObject, state);
#endif
    return state;
}

// ---- What the system is configured with

// conv_confname(): a name in one of the tables that the module has, or the number itself.
static std::optional<int> toConfigurationName(JSGlobalObject* globalObject, JSValue value, ASCIILiteral table)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (stringIn(value)) {
        JSValue names = getAttribute(globalObject, posixState(globalObject).module.get(), Identifier::fromString(vm, table));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        value = getItem(globalObject, names, value);
        if (scope.exception()) {
            if (scope.tryClearException())
                raiseValueError(globalObject, scope, "unrecognized configuration name"_s);
            return std::nullopt;
        }
    }
    if (!classify(value).isInt() && !typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index)) {
        raiseTypeError(globalObject, scope, "configuration names must be strings or integers"_s);
        return std::nullopt;
    }
    RELEASE_AND_RETURN(scope, toCInt(globalObject, value));
}

PYTHON_NATIVE(posixPathconf)
{
    NATIVE_PROLOGUE();
    PathArgument path("pathconf"_s, "path"_s, PathArgument::AllowsDescriptor);
    if (!path.convert(globalObject, args.at(0)))
        return { };
    auto name = toConfigurationName(globalObject, args.at(1), "pathconf_names"_s);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    long limit = path.isDescriptor ? ::fpathconf(path.descriptor, *name) : ::pathconf(path.narrow(), *name);
    if (limit == -1 && errno) {
        // It may be the name that is wrong, and it may be the file.
        if (errno == EINVAL)
            return JSValue::encode(raisePosixError(globalObject, scope));
        return JSValue::encode(raisePathError(globalObject, scope, path));
    }
    return JSValue::encode(intFromInt64(globalObject, limit));
}

PYTHON_NATIVE(posixFpathconf)
{
    NATIVE_PROLOGUE();
    auto descriptor = toFileDescriptorOrFile(globalObject, args.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    auto name = toConfigurationName(globalObject, args.at(1), "pathconf_names"_s);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    long limit = ::fpathconf(*descriptor, *name);
    if (limit == -1 && errno)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, limit));
}

PYTHON_NATIVE(posixConfstr)
{
    NATIVE_PROLOGUE();
    auto name = toConfigurationName(globalObject, args.at(0), "confstr_names"_s);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    size_t length = ::confstr(*name, nullptr, 0);
    if (!length) {
        if (errno)
            return JSValue::encode(raisePosixError(globalObject, scope));
        RETURN_NONE();
    }
    Vector<char> buffer(length);
    length = ::confstr(*name, buffer.mutableSpan().data(), buffer.size());
    RELEASE_AND_RETURN(scope, JSValue::encode(decodeFileSystemBytes(globalObject, buffer.span().first(length - 1))));
}

PYTHON_NATIVE(posixSysconf)
{
    NATIVE_PROLOGUE();
    auto name = toConfigurationName(globalObject, args.at(0), "sysconf_names"_s);
    RETURN_IF_EXCEPTION(scope, { });
    errno = 0;
    long value = ::sysconf(*name);
    if (value == -1 && errno)
        return JSValue::encode(raisePosixError(globalObject, scope));
    return JSValue::encode(intFromInt64(globalObject, value));
}

JSObject* createPosixModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    PosixModuleState& state = posixState(globalObject);
    JSObject* module = newBuiltinModule(globalObject, "posix"_s);
    state.module.set(vm, realm, module);
    auto set = [&] (ASCIILiteral name, JSValue value) { module->putDirect(vm, Identifier::fromString(vm, name), value); };

    addPosixPathFunctions(globalObject, module);
    addPosixDescriptorFunctions(globalObject, module);
    addPosixScandir(globalObject, module);
    addPosixIdentityFunctions(globalObject, module);
    addPosixProcessFunctions(globalObject, module);
#if OS(LINUX)
    addPosixLinuxFunctions(globalObject, module);
#endif
    addFunction(globalObject, module, "pathconf"_s, posixPathconf);
    addFunction(globalObject, module, "fpathconf"_s, posixFpathconf);
    addFunction(globalObject, module, "confstr"_s, posixConfstr);
    addFunction(globalObject, module, "sysconf"_s, posixSysconf);

    set("environ"_s, newEnvironmentDict(globalObject));
    addPosixConstants(globalObject, module);
    set("pathconf_names"_s, newPosixPathconfNames(globalObject));
    set("confstr_names"_s, newPosixConfstrNames(globalObject));
    set("sysconf_names"_s, newPosixSysconfNames(globalObject));
    set("error"_s, realm->typeOSError());
    set("waitid_result"_s, state.waitidResult.get());
#if OS(LINUX)
    set("sched_param"_s, state.schedulerParameter.get());
#endif
    set("stat_result"_s, state.statResult.get());
    set("statvfs_result"_s, state.statVFSResult.get());
    set("terminal_size"_s, state.terminalSize.get());
    set("DirEntry"_s, state.dirEntry.get());
    set("times_result"_s, state.timesResult.get());
    set("uname_result"_s, state.unameResult.get());

    // What os goes by to say which of its functions take a descriptor, a directory, or a link as it is.
    JSArray* have = newList(globalObject);
    for (ASCIILiteral name : {
        "HAVE_FACCESSAT"_s, "HAVE_FCHDIR"_s, "HAVE_FCHMOD"_s, "HAVE_FCHMODAT"_s, "HAVE_FCHOWN"_s, "HAVE_FCHOWNAT"_s,
#if OS(LINUX)
        "HAVE_FEXECVE"_s,
#endif
        "HAVE_FDOPENDIR"_s, "HAVE_FPATHCONF"_s, "HAVE_FSTATAT"_s, "HAVE_FSTATVFS"_s, "HAVE_FTRUNCATE"_s, "HAVE_FUTIMENS"_s, "HAVE_FUTIMES"_s, "HAVE_LINKAT"_s,
#if OS(DARWIN)
        "HAVE_LCHFLAGS"_s, "HAVE_LCHMOD"_s,
#endif
        "HAVE_LCHOWN"_s, "HAVE_LSTAT"_s, "HAVE_LUTIMES"_s, "HAVE_MKDIRAT"_s, "HAVE_MKFIFOAT"_s, "HAVE_MKNODAT"_s, "HAVE_OPENAT"_s, "HAVE_READLINKAT"_s, "HAVE_RENAMEAT"_s, "HAVE_SYMLINKAT"_s, "HAVE_UNLINKAT"_s,
        "HAVE_UTIMENSAT"_s })
        listAppend(globalObject, have, jsString(vm, String(name)));
    set("_have_functions"_s, have);
    return module;
}

#else

JSObject* createPosixModule(JSGlobalObject*)
{
    return nullptr;
}

#endif // OS(UNIX)

} } // namespace JSC::Python
