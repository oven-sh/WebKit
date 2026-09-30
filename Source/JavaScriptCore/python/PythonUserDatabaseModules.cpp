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
#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonOperations.h"
#include "PythonPosix.h"
#include "PythonSequences.h"
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

// The modules pwd and grp: Modules/pwdmodule.c and Modules/grpmodule.c of CPython, which are as alike as what they are about. Like posix, they are for whoever embeds the engine to say whether a program is to have.

namespace JSC { namespace Python {

namespace {

struct UserDatabaseState final : NativeState {
    PYTHON_NATIVE_STATE(UserDatabaseState);
    WriteBarrier<PyType> passwd;
    WriteBarrier<PyType> group;
};

template<typename Visitor> void UserDatabaseState::visit(Visitor& visitor)
{
    visitor.append(passwd);
    visitor.append(group);
}

UserDatabaseState& stateOfUserDatabase(JSGlobalObject* globalObject)
{
    return globalObject->pyRealm()->moduleState<UserDatabaseState>();
}

PyType* ensureSequenceType(JSGlobalObject* globalObject, WriteBarrier<PyType>& slot, ASCIILiteral name, std::initializer_list<ASCIILiteral> fields)
{
    if (!slot) {
        PyRealm* realm = globalObject->pyRealm();
        PyType* type = createBuiltinType(globalObject, name, realm->typeTuple(), PyType::Layout::Tuple, PyType::IsSequence | PyType::IsDerivedFromBuiltin);
        slot.set(globalObject->vm(), realm, type);
        makeStructSequenceType(globalObject, type, std::span(fields.begin(), fields.size()), static_cast<unsigned>(fields.size()));
    }
    return slot.get();
}

// PyUnicode_DecodeFSDefault(), or None if there is nothing to decode
JSValue decodeOrNone(JSGlobalObject* globalObject, const char* characters)
{
    return characters ? decodeFileSystemBytes(globalObject, unsafeSpan(characters)) : jsUndefined();
}

// mkpwent()
JSValue newPasswd(JSGlobalObject* globalObject, const struct passwd& entry)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    MarkedArgumentBuffer values;
    auto addText = [&] (const char* characters) {
        if (!scope.exception())
            values.append(decodeOrNone(globalObject, characters));
    };
    addText(entry.pw_name);
    addText(entry.pw_passwd);
    values.append(intFromUserID(globalObject, entry.pw_uid));
    values.append(intFromUserID(globalObject, entry.pw_gid));
    addText(entry.pw_gecos);
    addText(entry.pw_dir);
    addText(entry.pw_shell);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newStructSequence(globalObject, stateOfUserDatabase(globalObject).passwd.get(), values));
}

// mkgrent()
JSValue newGroup(JSGlobalObject* globalObject, const struct group& entry)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    MarkedArgumentBuffer members;
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    for (char** member = entry.gr_mem; ; ++member) {
        // It need not be where a pointer is to be.
        char* name;
        memcpy(&name, member, sizeof(name));
        if (!name)
            break;
        members.append(decodeFileSystemBytes(globalObject, unsafeSpan(name)));
        RETURN_IF_EXCEPTION(scope, { });
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    MarkedArgumentBuffer values;
    values.append(decodeFileSystemBytes(globalObject, unsafeSpan(entry.gr_name)));
    RETURN_IF_EXCEPTION(scope, { });
    values.append(decodeOrNone(globalObject, entry.gr_passwd));
    RETURN_IF_EXCEPTION(scope, { });
    values.append(intFromUserID(globalObject, entry.gr_gid));
    values.append(newList(globalObject, members));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newStructSequence(globalObject, stateOfUserDatabase(globalObject).group.get(), values));
}

enum class Lookup : uint8_t { Found, NotFound, NoMemory };

// What each of the four does with getpwuid_r() or the like: it is given room for the strings, and more if it says that that was not enough. `find` is given the entry to fill in, the room, and where to say whether it found one.
template<typename Entry, typename Function>
Lookup lookUp(int nameOfSize, Entry& entry, Vector<char>& buffer, const Function& find)
{
    constexpr size_t defaultBufferSize = 1024;
    long suggested = sysconf(nameOfSize);
    size_t size = suggested == -1 ? defaultBufferSize : static_cast<size_t>(suggested);
    while (true) {
        if (!buffer.tryGrow(size))
            return Lookup::NoMemory;
        Entry* found = nullptr;
        int status = find(&entry, buffer.mutableSpan(), &found);
        if (!status && found)
            return Lookup::Found;
        if (status != ERANGE)
            return Lookup::NotFound;
        if (size > static_cast<size_t>(std::numeric_limits<int64_t>::max() >> 1))
            return Lookup::NoMemory;
        size <<= 1;
    }
}

// The `unicode` of Argument Clinic, then PyUnicode_EncodeFSDefault(), then PyBytes_AsStringAndSize() for the sake of there being no zero in it. Nothing if it raised.
// pwd keeps to the limited API and grp does not, and what Argument Clinic writes for the one says what was given otherwise than what it writes for the other.
std::optional<CString> toName(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral argument, bool isOfLimitedAPI)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!stringIn(value)) {
        String given = isOfLimitedAPI ? fullyQualifiedTypeName(globalObject, value) : String(typeNameOfArgument(globalObject, value));
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        raiseTypeError(globalObject, scope, concatenate(function, "() "_s, argument, " must be str, not "_s, given));
        return std::nullopt;
    }
    RELEASE_AND_RETURN(scope, toFileSystemEncoded(globalObject, value));
}

JSValue raiseNameNotFound(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, JSValue name)
{
    String shown = repr(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    return raise(globalObject, scope, BuiltinType::KeyError, concatenate(function, "(): name not found: "_s, shown));
}

} // anonymous namespace

// getpwuid(uidobj, /)
PYTHON_NATIVE(pwdGetpwuid)
{
    NATIVE_PROLOGUE();
    auto uid = toUserID(globalObject, args[0]);
    if (Exception* raised = scope.exception()) [[unlikely]] {
        // One that is too great to be anyone's is nobody's.
        if (isInstance(globalObject, raised->value(), realm->type(BuiltinType::OverflowError)) && scope.tryClearException())
            return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, "getpwuid(): uid not found"_s));
        return { };
    }
    struct passwd entry;
    Vector<char> buffer;
    switch (lookUp(_SC_GETPW_R_SIZE_MAX, entry, buffer, [&] (struct passwd* result, std::span<char> room, struct passwd** found) { return getpwuid_r(*uid, result, room.data(), room.size(), found); })) {
    case Lookup::Found:
        RELEASE_AND_RETURN(scope, JSValue::encode(newPasswd(globalObject, entry)));
    case Lookup::NoMemory:
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    case Lookup::NotFound:
        break;
    }
    String shown = str(globalObject, intFromUserID(globalObject, *uid));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, concatenate("getpwuid(): uid not found: "_s, shown)));
}

// getpwnam(name, /)
PYTHON_NATIVE(pwdGetpwnam)
{
    NATIVE_PROLOGUE();
    auto name = toName(globalObject, args[0], "getpwnam"_s, "argument"_s, true);
    RETURN_IF_EXCEPTION(scope, { });
    struct passwd entry;
    Vector<char> buffer;
    switch (lookUp(_SC_GETPW_R_SIZE_MAX, entry, buffer, [&] (struct passwd* result, std::span<char> room, struct passwd** found) { return getpwnam_r(name->data(), result, room.data(), room.size(), found); })) {
    case Lookup::Found:
        RELEASE_AND_RETURN(scope, JSValue::encode(newPasswd(globalObject, entry)));
    case Lookup::NoMemory:
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    case Lookup::NotFound:
        break;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(raiseNameNotFound(globalObject, scope, "getpwnam"_s, args[0])));
}

PYTHON_NATIVE(pwdGetpwall)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    MarkedArgumentBuffer entries;
    setpwent();
    while (struct passwd* entry = getpwent()) {
        entries.append(newPasswd(globalObject, *entry));
        if (scope.exception()) [[unlikely]]
            break;
    }
    endpwent();
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, entries)));
}

// getgrgid(id)
PYTHON_NATIVE(grpGetgrgid)
{
    NATIVE_PROLOGUE();
    auto gid = toGroupID(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    struct group entry;
    Vector<char> buffer;
    switch (lookUp(_SC_GETGR_R_SIZE_MAX, entry, buffer, [&] (struct group* result, std::span<char> room, struct group** found) { return getgrgid_r(*gid, result, room.data(), room.size(), found); })) {
    case Lookup::Found:
        RELEASE_AND_RETURN(scope, JSValue::encode(newGroup(globalObject, entry)));
    case Lookup::NoMemory:
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    case Lookup::NotFound:
        break;
    }
    String shown = str(globalObject, intFromUserID(globalObject, *gid));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, concatenate("getgrgid(): gid not found: "_s, shown)));
}

// getgrnam(name)
PYTHON_NATIVE(grpGetgrnam)
{
    NATIVE_PROLOGUE();
    auto name = toName(globalObject, args[0], "getgrnam"_s, "argument 'name'"_s, false);
    RETURN_IF_EXCEPTION(scope, { });
    struct group entry;
    Vector<char> buffer;
    switch (lookUp(_SC_GETGR_R_SIZE_MAX, entry, buffer, [&] (struct group* result, std::span<char> room, struct group** found) { return getgrnam_r(name->data(), result, room.data(), room.size(), found); })) {
    case Lookup::Found:
        RELEASE_AND_RETURN(scope, JSValue::encode(newGroup(globalObject, entry)));
    case Lookup::NoMemory:
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    case Lookup::NotFound:
        break;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(raiseNameNotFound(globalObject, scope, "getgrnam"_s, args[0])));
}

PYTHON_NATIVE(grpGetgrall)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(args);
    MarkedArgumentBuffer entries;
    setgrent();
    while (struct group* entry = getgrent()) {
        entries.append(newGroup(globalObject, *entry));
        if (scope.exception()) [[unlikely]]
            break;
    }
    endgrent();
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject, entries)));
}

JSObject* createPwdModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyType* type = ensureSequenceType(globalObject, stateOfUserDatabase(globalObject).passwd, "pwd.struct_passwd"_s, { "pw_name"_s, "pw_passwd"_s, "pw_uid"_s, "pw_gid"_s, "pw_gecos"_s, "pw_dir"_s, "pw_shell"_s });
    JSObject* module = newBuiltinModule(globalObject, "pwd"_s);
    addFunction(globalObject, module, "getpwuid"_s, pwdGetpwuid);
    addFunction(globalObject, module, "getpwnam"_s, pwdGetpwnam);
    addFunction(globalObject, module, "getpwall"_s, pwdGetpwall);
    module->putDirect(vm, Identifier::fromString(vm, "struct_passwd"_s), type->object());
    return module;
}

JSObject* createGrpModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyType* type = ensureSequenceType(globalObject, stateOfUserDatabase(globalObject).group, "grp.struct_group"_s, { "gr_name"_s, "gr_passwd"_s, "gr_gid"_s, "gr_mem"_s });
    JSObject* module = newBuiltinModule(globalObject, "grp"_s);
    addFunction(globalObject, module, "getgrgid"_s, grpGetgrgid);
    addFunction(globalObject, module, "getgrnam"_s, grpGetgrnam);
    addFunction(globalObject, module, "getgrall"_s, grpGetgrall);
    module->putDirect(vm, Identifier::fromString(vm, "struct_group"_s), type->object());
    return module;
}

} } // namespace JSC::Python

#endif // OS(UNIX)
