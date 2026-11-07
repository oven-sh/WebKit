/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include <grp.h>
#include <pwd.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The modules pwd and grp: the users and the groups the system knows of. (Modules/pwdmodule.c, grpmodule.c in CPython.)

static JSValue pwdOptionalText(JSGlobalObject* globalObject, const char* text)
{
    return text ? posixDecode(globalObject, text) : jsUndefined();
}

// KeyError(message): the message is the key.
static JSValue pwdRaiseKeyError(JSGlobalObject* globalObject, const String& message)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseKeyError(globalObject, scope, makeStr(vm, message));
}

// The name to look up, for a function whose argument must be a str.
static bool pwdToName(JSGlobalObject* globalObject, JSValue value, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, posixEncodeStr(globalObject, strValue(value), result)))
        return false;
    if (result.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null byte"_s);
        return false;
    }
    return true;
}

// lookup(entry, buffer, size, result) is one of the getXXX_r() functions, which want room for the strings of the entry and
// answer ERANGE until it is enough. The entry found, or null.
template<typename Entry, typename Function>
static Entry* pwdLookUp(Entry& entry, Vector<char, 1024>& buffer, int sizeName, const Function& lookup)
{
    long size = sysconf(sizeName);
    buffer.grow(size > 0 ? static_cast<size_t>(size) : 1024);
    while (true) {
        Entry* result = nullptr;
        int status = lookup(&entry, buffer.mutableSpan().data(), buffer.size(), &result);
        if (status)
            result = nullptr;
        if (result || status != ERANGE)
            return result;
        buffer.grow(buffer.size() * 2);
    }
}

// ---- pwd ----

static constexpr ASCIILiteral pwdPasswdFields[] = { "pw_name"_s, "pw_passwd"_s, "pw_uid"_s, "pw_gid"_s, "pw_gecos"_s, "pw_dir"_s, "pw_shell"_s };
static constexpr StructSeqDef pwdPasswdDef { "pwd.struct_passwd"_s, pwdPasswdFields, 7 };

static JSValue pwdMakeEntry(JSGlobalObject* globalObject, const struct passwd& entry)
{
    return makeStructSeq(globalObject, structSeqTypeFor(globalObject, pwdPasswdDef), {
        pwdOptionalText(globalObject, entry.pw_name), pwdOptionalText(globalObject, entry.pw_passwd),
        posixFromUid(globalObject, entry.pw_uid), posixFromGid(globalObject, entry.pw_gid),
        pwdOptionalText(globalObject, entry.pw_gecos), pwdOptionalText(globalObject, entry.pw_dir), pwdOptionalText(globalObject, entry.pw_shell) });
}

static JSValue pwdGetpwuid(JSGlobalObject* globalObject, JSValue uidValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uid_t uid;
    if (failed(scope, posixToUid(globalObject, uidValue, uid))) {
        // A number no user can have is the number of no user.
        if (clearPendingExceptionIfMatches(globalObject, runtime(globalObject).OverflowError()))
            RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, "getpwuid(): uid not found"_s));
        return { };
    }
    struct passwd entry;
    Vector<char, 1024> buffer;
    auto* found = pwdLookUp(entry, buffer, _SC_GETPW_R_SIZE_MAX, [&](struct passwd* storage, char* characters, size_t size, struct passwd** result) {
        return getpwuid_r(uid, storage, characters, size, result);
    });
    if (!found) {
        if (uid == static_cast<uid_t>(-1))
            RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, "getpwuid(): uid not found: -1"_s));
        RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, makeString("getpwuid(): uid not found: "_s, uid)));
    }
    RELEASE_AND_RETURN(scope, pwdMakeEntry(globalObject, *found));
}

static JSValue pwdGetpwnam(JSGlobalObject* globalObject, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "getpwnam() argument must be str, not "_s, posixTypeNameOfArgument(globalObject, nameValue));
    PosixBytes name;
    if (failed(scope, pwdToName(globalObject, nameValue, name)))
        return { };
    struct passwd entry;
    Vector<char, 1024> buffer;
    auto* found = pwdLookUp(entry, buffer, _SC_GETPW_R_SIZE_MAX, [&](struct passwd* storage, char* characters, size_t size, struct passwd** result) {
        return getpwnam_r(name.characters(), storage, characters, size, result);
    });
    if (!found)
        RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, makeString("getpwnam(): name not found: "_s, reprForMessage(globalObject, nameValue))));
    RELEASE_AND_RETURN(scope, pwdMakeEntry(globalObject, *found));
}

static JSValue pwdGetpwall(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* result = newList(globalObject);
    setpwent();
    while (struct passwd* entry = getpwent()) {
        JSValue item = pwdMakeEntry(globalObject, *entry);
        if (scope.exception()) [[unlikely]]
            break;
        listAppend(globalObject, result, item);
        if (scope.exception()) [[unlikely]]
            break;
    }
    endpwent();
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static constexpr NativeDef pwdFunctions[] = {
    PY_FUNCTION("getpwuid", pwdGetpwuid, "(uidobj, /)"),
    PY_FUNCTION("getpwnam", pwdGetpwnam, "(name, /)"),
    PY_FUNCTION("getpwall", pwdGetpwall, "()"),
};

void initializePwdModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, pwdFunctions);
    module->add(globalObject, "struct_passwd"_s, structSeqTypeFor(globalObject, pwdPasswdDef));
}

// ---- grp ----

static constexpr ASCIILiteral grpGroupFields[] = { "gr_name"_s, "gr_passwd"_s, "gr_gid"_s, "gr_mem"_s };
static constexpr StructSeqDef grpGroupDef { "grp.struct_group"_s, grpGroupFields, 4 };

static JSValue grpMakeEntry(JSGlobalObject* globalObject, const struct group& entry)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    JSArray* members = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (char** member = entry.gr_mem; member && *member; ++member) {
        JSValue name = posixDecode(globalObject, *member);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, members, name);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue name = posixDecode(globalObject, entry.gr_name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue password = pwdOptionalText(globalObject, entry.gr_passwd);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue gid = posixFromGid(globalObject, entry.gr_gid);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStructSeq(globalObject, structSeqTypeFor(globalObject, grpGroupDef), { name, password, gid, members }));
}

static JSValue grpGetgrgid(JSGlobalObject* globalObject, JSValue idValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    gid_t gid;
    if (failed(scope, posixToGid(globalObject, idValue, gid)))
        return { };
    struct group entry;
    Vector<char, 1024> buffer;
    auto* found = pwdLookUp(entry, buffer, _SC_GETGR_R_SIZE_MAX, [&](struct group* storage, char* characters, size_t size, struct group** result) {
        return getgrgid_r(gid, storage, characters, size, result);
    });
    if (!found) {
        if (gid == static_cast<gid_t>(-1))
            RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, "getgrgid(): gid not found: -1"_s));
        RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, makeString("getgrgid(): gid not found: "_s, gid)));
    }
    RELEASE_AND_RETURN(scope, grpMakeEntry(globalObject, *found));
}

static JSValue grpGetgrnam(JSGlobalObject* globalObject, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(nameValue))
        return raiseTypeError(globalObject, scope, "getgrnam() argument 'name' must be str, not "_s, posixTypeNameOfArgument(globalObject, nameValue));
    PosixBytes name;
    if (failed(scope, pwdToName(globalObject, nameValue, name)))
        return { };
    struct group entry;
    Vector<char, 1024> buffer;
    auto* found = pwdLookUp(entry, buffer, _SC_GETGR_R_SIZE_MAX, [&](struct group* storage, char* characters, size_t size, struct group** result) {
        return getgrnam_r(name.characters(), storage, characters, size, result);
    });
    if (!found)
        RELEASE_AND_RETURN(scope, pwdRaiseKeyError(globalObject, makeString("getgrnam(): name not found: "_s, reprForMessage(globalObject, nameValue))));
    RELEASE_AND_RETURN(scope, grpMakeEntry(globalObject, *found));
}

static JSValue grpGetgrall(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* result = newList(globalObject);
    setgrent();
    while (struct group* entry = getgrent()) {
        JSValue item = grpMakeEntry(globalObject, *entry);
        if (scope.exception()) [[unlikely]]
            break;
        listAppend(globalObject, result, item);
        if (scope.exception()) [[unlikely]]
            break;
    }
    endgrent();
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static constexpr NativeDef grpFunctions[] = {
    PY_FUNCTION("getgrgid", grpGetgrgid, "(id)"),
    PY_FUNCTION("getgrnam", grpGetgrnam, "(name)"),
    PY_FUNCTION("getgrall", grpGetgrall, "()"),
};

void initializeGrpModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->addFunctions(globalObject, grpFunctions);
    module->add(globalObject, "struct_group"_s, structSeqTypeFor(globalObject, grpGroupDef));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
