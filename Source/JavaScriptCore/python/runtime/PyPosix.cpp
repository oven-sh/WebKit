/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyPosix.h"

#if !OS(WINDOWS)

#include "JSCInlines.h"
#include "PyCommandLine.h"
#include "PyExec.h"
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <termios.h>
#include <wtf/CryptographicallyRandomNumber.h>

#if OS(LINUX)
#include <sys/syscall.h>
#endif

#if OS(DARWIN)
#include <crt_externs.h>
#else
extern char** environ;
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module posix, which Lib/os.py is built on: here, what its files share, and the functions that are about neither files
// nor processes. (Modules/posixmodule.c in CPython.)

// ---- Errors ----

JSValue posixError(JSGlobalObject* globalObject, int errorNumber)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The call was interrupted by a signal, and that is what its handler raised.
    RETURN_IF_EXCEPTION(scope, { });
    return raiseOSError(globalObject, scope, errorNumber);
}

JSValue posixError(JSGlobalObject* globalObject)
{
    return posixError(globalObject, errno);
}

// ---- Results ----

JSValue posixFromUid(JSGlobalObject* globalObject, uid_t uid)
{
    if (uid == static_cast<uid_t>(-1))
        return jsNumber(-1);
    return posixMakeInt(globalObject, uid);
}

JSValue posixFromGid(JSGlobalObject* globalObject, gid_t gid)
{
    if (gid == static_cast<gid_t>(-1))
        return jsNumber(-1);
    return posixMakeInt(globalObject, gid);
}

JSValue posixFromDev(JSGlobalObject* globalObject, dev_t device)
{
    if (device == posixNoDevice)
        return jsNumber(-1);
    return makeInt(globalObject, static_cast<uint64_t>(device));
}

JSValue posixDecode(JSGlobalObject* globalObject, std::span<const char> bytes)
{
    return makeStr(globalObject, decodeFilesystem(byteCast<uint8_t>(bytes)));
}

JSValue posixDecode(JSGlobalObject* globalObject, const char* nulTerminated)
{
    return posixDecode(globalObject, std::span<const char> { nulTerminated, strlen(nulTerminated) });
}

JSValue posixFileName(JSGlobalObject* globalObject, std::span<const char> bytes, bool asBytes)
{
    if (asBytes)
        return PyBytes::create(globalObject, bytes);
    return posixDecode(globalObject, bytes);
}

// ---- Arguments ----

String posixTypeNameOfArgument(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? "None"_str : typeName(globalObject, value);
}

bool posixToInt(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted || *converted < std::numeric_limits<int>::min() || *converted > std::numeric_limits<int>::max()) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C int"_s);
        return false;
    }
    result = static_cast<int>(*converted);
    return true;
}

static bool posixToInt64(JSGlobalObject* globalObject, JSValue value, ASCIILiteral complaint, int64_t& result)
{
    if (value.isInt32()) [[likely]] {
        result = value.asInt32();
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, complaint);
        return false;
    }
    result = *converted;
    return true;
}

bool posixToLong(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    return posixToInt64(globalObject, value, "Python int too large to convert to C long"_s, result);
}

bool posixToLongLong(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    return posixToInt64(globalObject, value, "int too big to convert"_s, result);
}

bool posixToSsize(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    return posixToInt64(globalObject, value, "Python int too large to convert to C ssize_t"_s, result);
}

static bool posixToUnsignedOfType(JSGlobalObject* globalObject, JSValue value, ASCIILiteral type, uint64_t maximum, uint64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted || *converted > maximum) {
        raiseOverflowError(globalObject, scope, "Python int too large for C "_s, type);
        return false;
    }
    result = *converted;
    return true;
}

bool posixToUnsigned(JSGlobalObject* globalObject, JSValue value, unsigned& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t number;
    if (failed(scope, posixToUnsignedOfType(globalObject, value, "unsigned int"_s, std::numeric_limits<unsigned>::max(), number)))
        return false;
    result = static_cast<unsigned>(number);
    return true;
}

bool posixToUnsignedLongLong(JSGlobalObject* globalObject, JSValue value, uint64_t& result)
{
    return posixToUnsignedOfType(globalObject, value, "unsigned long long"_s, std::numeric_limits<uint64_t>::max(), result);
}

bool posixToUnsignedMask(JSGlobalObject* globalObject, JSValue value, unsigned& result)
{
    if (value.isInt32()) [[likely]] {
        result = static_cast<unsigned>(value.asInt32());
        return true;
    }
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (converted)
        result = static_cast<unsigned>(*converted);
    else {
        JSValue low = intAnd(globalObject, index, makeInt(globalObject, static_cast<uint32_t>(0xffffffffu)));
        RETURN_IF_EXCEPTION(scope, false);
        result = static_cast<unsigned>(tryInt64(low).value_or(0));
    }
    if (!converted || *converted < std::numeric_limits<int>::min() || *converted > std::numeric_limits<unsigned>::max())
        RELEASE_AND_RETURN(scope, warn(globalObject, runtime(globalObject).DeprecationWarning(), "integer value out of range"_s));
    return true;
}

bool posixToCount(JSGlobalObject* globalObject, JSValue value, ASCIILiteral name, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, posixToSsize(globalObject, value, result)))
        return false;
    if (result < 0) {
        raiseValueError(globalObject, scope, name, " cannot be negative"_s);
        return false;
    }
    return true;
}

// uid_t and gid_t are unsigned, yet -1 is a value of theirs.
static bool posixToId(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what, uint32_t& result)
{
    static_assert(sizeof(uid_t) == sizeof(uint32_t) && sizeof(gid_t) == sizeof(uint32_t));
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    if (!index) [[unlikely]] {
        if (takePendingException(globalObject))
            raiseTypeError(globalObject, scope, what, " should be integer, not "_s, typeName(globalObject, value));
        return false;
    }
    auto converted = tryInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, what, intSign(index) < 0 ? " is less than minimum"_s : " is greater than maximum"_s);
        return false;
    }
    // What is too large for 32 bits is "less than minimum" in CPython too.
    if (*converted < -1 || *converted > std::numeric_limits<uint32_t>::max()) {
        raiseOverflowError(globalObject, scope, what, " is less than minimum"_s);
        return false;
    }
    result = static_cast<uint32_t>(*converted);
    return true;
}

bool posixToUid(JSGlobalObject* globalObject, JSValue value, uid_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t id;
    if (failed(scope, posixToId(globalObject, value, "uid"_s, id)))
        return false;
    result = id;
    return true;
}

bool posixToGid(JSGlobalObject* globalObject, JSValue value, gid_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t id;
    if (failed(scope, posixToId(globalObject, value, "gid"_s, id)))
        return false;
    result = id;
    return true;
}

bool posixToDev(JSGlobalObject* globalObject, JSValue value, dev_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        if (auto converted = tryInt64(index); converted && *converted == -1) {
            result = posixNoDevice;
            return true;
        }
        raiseOverflowError(globalObject, scope, "can't convert negative int to unsigned"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "int too big to convert"_s);
        return false;
    }
    if (static_cast<uint64_t>(static_cast<dev_t>(*converted)) != *converted) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C dev_t"_s);
        return false;
    }
    result = static_cast<dev_t>(*converted);
    return true;
}

static bool posixWarnOfBoolDescriptor(JSGlobalObject* globalObject, JSValue value)
{
    if (!value.isBoolean())
        return true;
    return warn(globalObject, runtime(globalObject).RuntimeWarning(), "bool is used as a file descriptor"_s);
}

bool posixToFileDescriptor(JSGlobalObject* globalObject, JSValue value, int& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (isInt(value)) {
        if (!posixWarnOfBoolDescriptor(globalObject, value))
            return false;
        if (failed(scope, posixToInt(globalObject, value, fd)))
            return false;
    } else {
        JSValue method = getAttrIfPresent(globalObject, value, internString(vm, "fileno"_s));
        RETURN_IF_EXCEPTION(scope, false);
        if (!method) {
            raiseTypeError(globalObject, scope, "argument must be an int, or have a fileno() method."_s);
            return false;
        }
        JSValue number = call(globalObject, method);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isInt(number)) {
            raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, typeOf(globalObject, value)), ".fileno() must return an int, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, number)));
            return false;
        }
        if (failed(scope, posixToInt(globalObject, number, fd)))
            return false;
    }
    if (fd < 0) {
        raiseValueError(globalObject, scope, "file descriptor cannot be a negative integer ("_s, fd, ')');
        return false;
    }
    result = fd;
    return true;
}

// A file descriptor where a path may be given, or for dir_fd.
static bool posixToPathDescriptor(JSGlobalObject* globalObject, JSValue value, int& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!posixWarnOfBoolDescriptor(globalObject, value))
        return false;
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    auto converted = tryInt64(index);
    if (converted ? *converted > std::numeric_limits<int>::max() : intSign(index) > 0) {
        raiseOverflowError(globalObject, scope, "fd is greater than maximum"_s);
        return false;
    }
    if (!converted || *converted < std::numeric_limits<int>::min()) {
        raiseOverflowError(globalObject, scope, "fd is less than minimum"_s);
        return false;
    }
    result = static_cast<int>(*converted);
    return true;
}

bool posixToDirFd(JSGlobalObject* globalObject, JSValue value, int& result)
{
    if (!value || isNone(value)) {
        result = AT_FDCWD;
        return true;
    }
    if (!hasIndex(globalObject, value)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, "argument should be integer or None, not "_s, typeName(globalObject, value));
        return false;
    }
    return posixToPathDescriptor(globalObject, value, result);
}

bool posixEncodeStr(JSGlobalObject* globalObject, JSString* string, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (auto encoded = tryEncodeUTF8(text))
        result.assign(byteCast<char>(encoded->span()));
    else {
        // There are lone surrogates: those that stand for a byte become it, any other is an error.
        JSValue bytes = strEncode(globalObject, string, makeStr(vm, "utf-8"_s), makeStr(vm, "surrogateescape"_s));
        RETURN_IF_EXCEPTION(scope, false);
        result.assign(byteCast<char>(PyBytes::from(bytes)->span()));
    }
    return true;
}

JSValue posixFSPath(JSGlobalObject* globalObject, JSValue value)
{
    if (isStr(value) || PyBytes::is(value))
        return value;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(Special::FsPath);
    if (!method || isNone(method))
        return raiseTypeError(globalObject, scope, "expected str, bytes or os.PathLike object, not "_s, type->name());
    JSValue result = callSpecial(globalObject, method, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isStr(result) && !PyBytes::is(result))
        return raiseTypeError(globalObject, scope, "expected "_s, type->name(), ".__fspath__() to return str or bytes, not "_s, typeName(globalObject, result));
    return result;
}

bool posixToFSBytes(JSGlobalObject* globalObject, JSValue value, PosixBytes& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue path = posixFSPath(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (auto* bytes = PyBytes::tryFrom(path))
        result.assign(byteCast<char>(bytes->span()));
    else if (failed(scope, posixEncodeStr(globalObject, strValue(path), result)))
        return false;
    if (result.contains('\0')) {
        raiseValueError(globalObject, scope, "embedded null byte"_s);
        return false;
    }
    return true;
}

bool posixIsSequence(JSGlobalObject* globalObject, JSValue value)
{
    return !isDict(value) && typeOf(globalObject, value)->lookup(Special::GetItem);
}

bool posixGetWritableBuffer(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, unsigned position, BufferHolder& holder)
{
    if (holder.get(value, BufferWritable))
        return true;
    if (!takePendingException(globalObject))
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, function, "() argument "_s, position, " must be read-write bytes-like object, not "_s, posixTypeNameOfArgument(globalObject, value));
    return false;
}

bool PosixPath::convert(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool allowFd = m_options & AllowFd;
    bool nullable = m_options & Nullable;
    m_hasNarrow = false;
    m_isFd = false;
    m_isVirtual = false;
    m_fd = -1;
    if (!value)
        value = jsUndefined();
    m_object = value;
    if (nullable && isNone(value))
        return true;

    bool isText = isStr(value);
    bool isBytesObject = PyBytes::is(value);
    if (!isText && !isBytesObject) {
        if (allowFd && hasIndex(globalObject, value)) {
            if (!posixToPathDescriptor(globalObject, value, m_fd))
                return false;
            m_isFd = true;
            return true;
        }
        PyType* type = typeOf(globalObject, value);
        JSValue method = type->lookup(Special::FsPath);
        if (!method || isNone(method)) {
            ASCIILiteral expected = allowFd && nullable ? "string, bytes, os.PathLike, integer or None"_s
                : allowFd ? "string, bytes, os.PathLike or integer"_s
                : nullable ? "string, bytes, os.PathLike or None"_s
                : "string, bytes or os.PathLike"_s;
            raiseTypeError(globalObject, scope, m_function, ": "_s, m_argument, " should be "_s, expected, ", not "_s, type->name());
            return false;
        }
        JSValue result = callSpecial(globalObject, method, value);
        RETURN_IF_EXCEPTION(scope, false);
        isText = isStr(result);
        isBytesObject = PyBytes::is(result);
        if (!isText && !isBytesObject) {
            raiseTypeError(globalObject, scope, "expected "_s, type->name(), ".__fspath__() to return str or bytes, not "_s, typeName(globalObject, result));
            return false;
        }
        value = result;
        m_object = result;
    }

    if (isText) {
        if (m_options & AsText)
            return true;
        if (failed(scope, posixEncodeStr(globalObject, strValue(value), m_narrow)))
            return false;
    } else
        m_narrow.assign(byteCast<char>(PyBytes::from(value)->span()));
    if (!(m_options & Nonstrict) && m_narrow.contains('\0')) {
        raiseValueError(globalObject, scope, m_function, ": embedded null character in "_s, m_argument);
        return false;
    }
    m_hasNarrow = true;
    m_isVirtual = toVirtualPath(m_narrow.span(), m_virtualPath);
    if (Vector<char, 256> realPath; m_isVirtual && toCachePath(virtualPath(), realPath)) [[unlikely]] {
        // The directories it is in are there as far as anyone can tell: they are virtual ones.
        if (m_options & MakesNew)
            makeParentDirectories(realPath.span());
        m_narrow.assign(realPath.span());
        m_isVirtual = false;
    }
    if (m_isVirtual && !(m_options & AllowVirtual)) [[unlikely]] {
        auto change = m_options & MakesNew ? VirtualChange::MakesNew : m_options & MakesOrReplaces ? VirtualChange::MakesOrReplaces : VirtualChange::OfExisting;
        int errorNumber = errorForVirtualChange(virtualPath(), change);
        if (errorNumber == EROFS && (m_options & VirtualUnsupported))
            errorNumber = ENOTSUP;
        raiseOSError(globalObject, scope, errorNumber, m_object);
        return false;
    }
    return true;
}

JSValue PosixPath::error(JSGlobalObject* globalObject) const
{
    int errorNumber = errno;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // As in posixError().
    RETURN_IF_EXCEPTION(scope, { });
    return raiseOSError(globalObject, scope, errorNumber, m_object);
}

JSValue PosixPath::error(JSGlobalObject* globalObject, const PosixPath& second) const
{
    int errorNumber = errno;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseOSError(globalObject, scope, errorNumber, m_object, second.m_object);
}

bool posixPathAndDirFdInvalid(JSGlobalObject* globalObject, ASCIILiteral function, const PosixPath& path, int dirFd)
{
    if (path.narrow() || dirFd == AT_FDCWD)
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, function, ": can't specify dir_fd without matching path"_s);
    return true;
}

bool posixDirFdAndFdInvalid(JSGlobalObject* globalObject, ASCIILiteral function, int dirFd, int fd)
{
    if (dirFd == AT_FDCWD || fd == -1)
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, function, ": can't specify both dir_fd and fd"_s);
    return true;
}

bool posixFdAndFollowSymlinksInvalid(JSGlobalObject* globalObject, ASCIILiteral function, bool isFd, bool followSymlinks)
{
    if (!isFd || followSymlinks)
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, function, ": cannot use fd and follow_symlinks together"_s);
    return true;
}

bool posixDirFdAndFollowSymlinksInvalid(JSGlobalObject* globalObject, ASCIILiteral function, int dirFd, bool followSymlinks)
{
    if (dirFd == AT_FDCWD || followSymlinks)
        return false;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, function, ": cannot use dir_fd and follow_symlinks together"_s);
    return true;
}

bool posixToTimespec(JSGlobalObject* globalObject, JSValue value, time_t& seconds, long& nanoseconds)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto outOfRange = [&] {
        raiseOverflowError(globalObject, scope, "timestamp out of range for platform time_t"_s);
        return false;
    };
    nanoseconds = 0;
    if (hasIndex(globalObject, value)) {
        JSValue index = toIndex(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        auto converted = tryInt64(index);
        if (!converted)
            return outOfRange();
        seconds = static_cast<time_t>(*converted);
        return true;
    }
    double number = toDouble(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (std::isnan(number)) {
        raiseValueError(globalObject, scope, "Invalid value NaN (not a number)"_s);
        return false;
    }
    double whole;
    double fraction = std::floor(std::modf(number, &whole) * 1e9);
    if (fraction >= 1e9) {
        fraction -= 1e9;
        whole += 1;
    } else if (fraction < 0) {
        fraction += 1e9;
        whole -= 1;
    }
    constexpr double limit = 9223372036854775808.0;
    if (!(whole >= -limit && whole < limit))
        return outOfRange();
    seconds = static_cast<time_t>(whole);
    nanoseconds = static_cast<long>(fraction);
    return true;
}

// ---- Types ----

static constexpr TypeDef posixStateTypeDef { "posix._state"_s, { TypeFlag::DisallowInstantiation, TypeFlag::Internal }, { }, { }, { } };

PyDict* posixState(JSGlobalObject* globalObject)
{
    // The runtime keeps a type for each definition, and a type has a dict: somewhere to keep what must be found again.
    return runtime(globalObject).typeFor(posixStateTypeDef)->dict();
}

PyType* cachedStructSeqType(JSGlobalObject* globalObject, const StructSeqDef& def)
{
    VM& vm = getVM(globalObject);
    PyDict* state = posixState(globalObject);
    JSString* key = internString(vm, def.name);
    if (JSValue type = state->get(key))
        return PyType::from(type);
    PyType* type = makeStructSeqType(globalObject, def);
    state->set(vm, key, type);
    return type;
}

// ---- File descriptors ----

bool posixSetInheritable(int fd, bool inheritable)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    int newFlags = inheritable ? flags & ~FD_CLOEXEC : flags | FD_CLOEXEC;
    return newFlags == flags || fcntl(fd, F_SETFD, newFlags) >= 0;
}

bool posixFreeDescriptors(JSGlobalObject* globalObject)
{
    int error = errno;
    if (error != EMFILE && error != ENFILE)
        return false;
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (vm.exceptionForInspection() || rt.isRunningPendingCalls())
        return false;
    writeOutPendingOutput(globalObject); // Until then they are not to be collected.
    vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    rt.runPendingCalls();
    errno = error;
    return !vm.exceptionForInspection();
}

int posixDupNotInheritable(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

// ---- Constants ----

void addPosixConstants(JSGlobalObject* globalObject, PyModule* module, std::span<const PosixConstant> constants)
{
    for (auto& constant : constants)
        module->add(globalObject, constant.name, makeInt(globalObject, constant.value));
}

PyDict* posixConstantDict(JSGlobalObject* globalObject, std::span<const PosixConstant> constants)
{
    VM& vm = getVM(globalObject);
    PyDict* dict = PyDict::create(globalObject);
    for (auto& constant : constants)
        dict->set(vm, internString(vm, constant.name), makeInt(globalObject, constant.value));
    return dict;
}

// ---- The environment ----

static char** posixEnvironmentOfProcess()
{
#if OS(DARWIN)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

static JSValue posixCreateEnviron(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* dict = PyDict::create(globalObject);
    char** entry = posixEnvironmentOfProcess();
    if (!entry)
        return dict;
    for (; *entry; ++entry) {
        const char* equals = strchr(*entry, '=');
        if (!equals)
            continue;
        JSValue key = PyBytes::create(globalObject, std::span<const char> { *entry, static_cast<size_t>(equals - *entry) });
        JSValue value = PyBytes::create(globalObject, std::span<const char> { equals + 1, strlen(equals + 1) });
        // Of two with the same name, getenv() finds the first.
        dict->setDefault(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return dict;
}

static HostEnvironment s_hostEnvironment;

void setHostEnvironment(const HostEnvironment& environment)
{
    s_hostEnvironment = environment;
}

static String posixTextForHost(std::span<const char> bytes)
{
    return String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(bytes));
}

void posixDidChangeEnvironment(JSGlobalObject* globalObject, std::span<const Vector<char>> names)
{
    if (!s_hostEnvironment.didChange)
        return;
    // Nothing of the host's is called on another thread.
    if (Threads& threads = runtime(globalObject).threads(); !threads.isMainThread()) [[unlikely]] {
        for (auto& name : names)
            threads.mainThreadTellsHostOfVariable(name);
        return;
    }
    for (auto& name : names) {
        const char* value = getenv(name.span().data());
        s_hostEnvironment.didChange(globalObject, posixTextForHost(name.span().first(name.size() - 1)), value ? posixTextForHost(unsafeSpan(value)) : String());
    }
}

static void posixDidChangeVariable(JSGlobalObject* globalObject, const PosixBytes& name)
{
    Vector<char> copy { std::span { name.characters(), name.length() + 1 } };
    posixDidChangeEnvironment(globalObject, singleElementSpan(copy));
}

// posix.environ, which os.environ keeps its variables in. The program cannot put another in its place here. Null: nothing has imported posix.
static PyDict* posixEnviron(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue namespaces = rt.slot(Runtime::Slot::NativeModules);
    JSValue ownNamespace = namespaces ? PyDict::from(namespaces)->get(internString(vm, "posix"_s)) : JSValue();
    JSValue dict = ownNamespace ? PyDict::from(ownNamespace)->get(internString(vm, "environ"_s)) : JSValue();
    return dict ? PyDict::from(dict) : nullptr;
}

void hostDidChangeEnvironment(JSGlobalObject* globalObject, const String& name, const String& value, bool ofProcess)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // It will begin as what the host has then.
    PyDict* dict = globalObject->pythonRuntime() ? posixEnviron(globalObject) : nullptr;
    if (!dict)
        return;
    PosixBytes nameBytes;
    nameBytes.assign(byteCast<char>(name.utf8(StrictConversionReplacingUnpairedSurrogatesWithFFFD).span()));
    if (!nameBytes.length() || nameBytes.contains('=') || nameBytes.contains('\0'))
        return;
    JSValue key = PyBytes::create(globalObject, nameBytes.span());
    if (value.isNull()) {
        dict->remove(globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
        if (ofProcess)
            unsetenv(nameBytes.characters());
        return;
    }
    PosixBytes valueBytes;
    JSValue existing = dict->get(globalObject, key);
    RETURN_IF_EXCEPTION(scope, void());
    // Bytes that are not UTF-8 stay what they are when the host, which cannot tell them from others, says what it was told.
    if (auto* bytes = existing ? PyBytes::tryFrom(existing) : nullptr; bytes && posixTextForHost(byteCast<char>(bytes->span())) == value)
        valueBytes.assign(byteCast<char>(bytes->span()));
    else {
        // As for a child of the host's, the value ends at a NUL.
        valueBytes.assign(unsafeSpan(value.utf8(StrictConversionReplacingUnpairedSurrogatesWithFFFD).legacyCStringPointer()));
        dict->set(globalObject, key, PyBytes::create(globalObject, valueBytes.span()));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!ofProcess)
        return;
    if (const char* current = getenv(nameBytes.characters()); !current || strcmp(current, valueBytes.characters()))
        setenv(nameBytes.characters(), valueBytes.characters(), 1);
}

// posix.environ has been made of the environment of the process.
static void posixTakeEnvironmentOfHost(JSGlobalObject* globalObject, PyDict* dict)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!s_hostEnvironment.read)
        return;
    EnvironmentVariables variables;
    bool ofProcess = s_hostEnvironment.read(globalObject, variables);
    RETURN_IF_EXCEPTION(scope, void());
    HashSet<String> names;
    for (auto& variable : variables)
        names.add(variable.first);
    Vector<String> gone;
    JSValue key;
    JSValue value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        if (String name = posixTextForHost(byteCast<char>(PyBytes::from(key)->span())); !names.contains(name))
            gone.append(WTF::move(name));
    }
    for (auto& name : gone) {
        hostDidChangeEnvironment(globalObject, name, String(), ofProcess);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (auto& variable : variables) {
        hostDidChangeEnvironment(globalObject, variable.first, variable.second, ofProcess);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

static JSValue posixPutenv(JSGlobalObject* globalObject, JSValue nameValue, JSValue valueValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    PosixBytes value;
    if (!posixToFSBytes(globalObject, nameValue, name) || !posixToFSBytes(globalObject, valueValue, value))
        return { };
    if (isAudited(globalObject) && failed(scope, audit(globalObject, "os.putenv"_s, PyBytes::create(globalObject, name.span()), PyBytes::create(globalObject, value.span()))))
        return { };
    if (name.contains('='))
        return raiseValueError(globalObject, scope, "illegal environment variable name"_s);
    if (setenv(name.characters(), value.characters(), 1))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    posixDidChangeVariable(globalObject, name);
    return jsUndefined();
}

static JSValue posixUnsetenv(JSGlobalObject* globalObject, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixBytes name;
    if (!posixToFSBytes(globalObject, nameValue, name))
        return { };
    if (isAudited(globalObject) && failed(scope, audit(globalObject, "os.unsetenv"_s, PyBytes::create(globalObject, name.span()))))
        return { };
    if (unsetenv(name.characters()))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    posixDidChangeVariable(globalObject, name);
    return jsUndefined();
}

#if OS(LINUX)
static JSValue posixClearenv(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, audit(globalObject, "os._clearenv"_s)))
        return { };
    Vector<Vector<char>> names;
    for (char** entry = posixEnvironmentOfProcess(); entry && *entry; ++entry) {
        if (const char* equals = strchr(*entry, '=')) {
            names.append(Vector<char> { std::span<const char> { *entry, static_cast<size_t>(equals - *entry) } });
            names.last().append('\0');
        }
    }
    errno = 0;
    if (clearenv())
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    posixDidChangeEnvironment(globalObject, names.span());
    return jsUndefined();
}
#endif

// ---- Paths as text ----

static JSValue posixFspathFunction(JSGlobalObject* globalObject, JSValue path)
{
    return posixFSPath(globalObject, path);
}

// How many of the slashes at the start are the root: POSIX leaves the meaning of exactly two to the implementation.
template<typename CharacterType>
static size_t posixRootLength(std::span<const CharacterType> path)
{
    if (path.empty() || path[0] != '/')
        return 0;
    if (path.size() < 2 || path[1] != '/' || (path.size() > 2 && path[2] == '/'))
        return 1;
    return 2;
}

template<typename CharacterType>
static Vector<CharacterType> posixNormalizedPath(std::span<const CharacterType> path)
{
    Vector<CharacterType> result;
    size_t root = posixRootLength(path);
    struct Component {
        size_t start;
        size_t length;
    };
    Vector<Component, 32> components;
    auto isDotDot = [&](Component component) { return component.length == 2 && path[component.start] == '.' && path[component.start + 1] == '.'; };
    size_t position = root;
    while (position <= path.size()) {
        size_t end = position;
        while (end < path.size() && path[end] != '/')
            ++end;
        Component component { position, end - position };
        position = end + 1;
        if (!component.length || (component.length == 1 && path[component.start] == '.'))
            continue;
        if (!isDotDot(component) || (!root && components.isEmpty()) || (!components.isEmpty() && isDotDot(components.last())))
            components.append(component);
        else if (!components.isEmpty())
            components.removeLast();
    }
    for (size_t i = 0; i < root; ++i)
        result.append(static_cast<CharacterType>('/'));
    bool first = true;
    for (auto component : components) {
        if (!first)
            result.append(static_cast<CharacterType>('/'));
        first = false;
        result.append(path.subspan(component.start, component.length));
    }
    if (result.isEmpty())
        result.append(static_cast<CharacterType>('.'));
    return result;
}

// The text of a path argument that is a str.
static bool posixTextOf(JSGlobalObject* globalObject, const PosixPath& path, String& text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    text = strValue(path.object())->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static JSValue posixPathNormpath(JSGlobalObject* globalObject, JSValue pathValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("_path_normpath"_s, PosixPath::Nonstrict | PosixPath::AllowVirtual | PosixPath::AsText);
    if (failed(scope, path.convert(globalObject, pathValue)))
        return { };
    if (path.isBytes())
        RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, posixNormalizedPath(path.span()).span()));
    String text;
    if (failed(scope, posixTextOf(globalObject, path, text)))
        return { };
    if (text.is8Bit())
        RELEASE_AND_RETURN(scope, makeStr(globalObject, String(posixNormalizedPath(text.span8()).span())));
    RELEASE_AND_RETURN(scope, makeStr(globalObject, String(posixNormalizedPath(text.span16()).span())));
}

static JSValue posixPathSplitrootEx(JSGlobalObject* globalObject, JSValue pathValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("_path_splitroot_ex"_s, PosixPath::Nonstrict | PosixPath::AllowVirtual | PosixPath::AsText);
    if (!path.convert(globalObject, pathValue))
        return { };
    if (path.isBytes()) {
        auto span = path.span();
        size_t root = posixRootLength(span);
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { runtime(globalObject).emptyBytes(), PyBytes::create(globalObject, span.first(root)), PyBytes::create(globalObject, span.subspan(root)) }));
    }
    String text;
    if (failed(scope, posixTextOf(globalObject, path, text)))
        return { };
    unsigned root = static_cast<unsigned>(text.is8Bit() ? posixRootLength(text.span8()) : posixRootLength(text.span16()));
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { makeStr(globalObject, emptyString()), makeStr(globalObject, text.left(root)), makeStr(globalObject, text.substring(root)) }));
}

// ---- What the system says of itself ----

static constexpr ASCIILiteral posixUnameFields[] = { "sysname"_s, "nodename"_s, "release"_s, "version"_s, "machine"_s };
static constexpr StructSeqDef posixUnameResultDef { "posix.uname_result"_s, posixUnameFields, 5 };

static JSValue posixUname(JSGlobalObject* globalObject)
{
    struct utsname names;
    if (uname(&names) < 0)
        return posixError(globalObject);
    return makeStructSeq(globalObject, cachedStructSeqType(globalObject, posixUnameResultDef), {
        posixDecode(globalObject, names.sysname), posixDecode(globalObject, names.nodename), posixDecode(globalObject, names.release),
        posixDecode(globalObject, names.version), posixDecode(globalObject, names.machine) });
}

static constexpr ASCIILiteral posixTimesFields[] = { "user"_s, "system"_s, "children_user"_s, "children_system"_s, "elapsed"_s };
static constexpr StructSeqDef posixTimesResultDef { "posix.times_result"_s, posixTimesFields, 5 };

static JSValue posixTimes(JSGlobalObject* globalObject)
{
    struct tms process;
    errno = 0;
    clock_t elapsed = times(&process);
    if (elapsed == static_cast<clock_t>(-1))
        return posixError(globalObject);
    double ticksPerSecond = static_cast<double>(sysconf(_SC_CLK_TCK));
    auto inSeconds = [&](clock_t ticks) { return makeFloat(globalObject, static_cast<double>(ticks) / ticksPerSecond); };
    return makeStructSeq(globalObject, cachedStructSeqType(globalObject, posixTimesResultDef), {
        inSeconds(process.tms_utime), inSeconds(process.tms_stime), inSeconds(process.tms_cutime), inSeconds(process.tms_cstime), inSeconds(elapsed) });
}

static constexpr ASCIILiteral posixTerminalSizeFields[] = { "columns"_s, "lines"_s };
static constexpr StructSeqDef posixTerminalSizeDef { "os.terminal_size"_s, posixTerminalSizeFields, 2 };

static JSValue posixGetTerminalSize(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd = STDOUT_FILENO;
    if (fdValue && failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    if (!isatty(fd))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    struct winsize size;
    if (ioctl(fd, TIOCGWINSZ, &size))
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, makeStructSeq(globalObject, cachedStructSeqType(globalObject, posixTerminalSizeDef), { jsNumber(size.ws_col), jsNumber(size.ws_row) }));
}

// The name of a limit, for sysconf() and its like: an int, or a key of the table of that name.
static bool posixToConfigurationName(JSGlobalObject* globalObject, JSValue value, ASCIILiteral table, int& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isStr(value)) {
        JSValue names = posixState(globalObject)->get(internString(vm, table));
        JSValue number = names ? PyDict::from(names)->get(globalObject, value) : JSValue();
        RETURN_IF_EXCEPTION(scope, false);
        if (!number) {
            raiseValueError(globalObject, scope, "unrecognized configuration name"_s);
            return false;
        }
        value = number;
    }
    if (!hasIndex(globalObject, value)) {
        raiseTypeError(globalObject, scope, "configuration names must be strings or integers"_s);
        return false;
    }
    RELEASE_AND_RETURN(scope, posixToInt(globalObject, value, result));
}

static JSValue posixSysconf(JSGlobalObject* globalObject, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int name;
    if (failed(scope, posixToConfigurationName(globalObject, nameValue, "sysconf_names"_s, name)))
        return { };
    errno = 0;
    long value = sysconf(name);
    if (value == -1 && errno)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, value));
}

#if !OS(ANDROID)
static JSValue posixConfstr(JSGlobalObject* globalObject, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int name;
    if (failed(scope, posixToConfigurationName(globalObject, nameValue, "confstr_names"_s, name)))
        return { };
    Vector<char, 256> buffer(256);
    errno = 0;
    size_t length = confstr(name, buffer.mutableSpan().data(), buffer.size());
    if (!length) {
        if (errno)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
        return jsUndefined();
    }
    if (length > buffer.size()) {
        buffer.grow(length);
        length = confstr(name, buffer.mutableSpan().data(), buffer.size());
    }
    RELEASE_AND_RETURN(scope, posixDecode(globalObject, buffer.span().first(length - 1)));
}
#endif

static JSValue posixFpathconf(JSGlobalObject* globalObject, JSValue fdValue, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    int name;
    if (!posixToFileDescriptor(globalObject, fdValue, fd) || failed(scope, posixToConfigurationName(globalObject, nameValue, "pathconf_names"_s, name)))
        return { };
    errno = 0;
    long limit = fpathconf(fd, name);
    if (limit == -1 && errno)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, limit));
}

static JSValue posixPathconf(JSGlobalObject* globalObject, JSValue pathValue, JSValue nameValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PosixPath path("pathconf"_s, PosixPath::AllowFd | PosixPath::VirtualUnsupported);
    int name;
    if (!path.convert(globalObject, pathValue) || failed(scope, posixToConfigurationName(globalObject, nameValue, "pathconf_names"_s, name)))
        return { };
    errno = 0;
    long limit = path.isFd() ? fpathconf(path.fd(), name) : pathconf(path.narrow(), name);
    if (limit == -1 && errno) {
        // EINVAL may be about the name as well as about the path.
        if (errno == EINVAL)
            RELEASE_AND_RETURN(scope, posixError(globalObject));
        RELEASE_AND_RETURN(scope, path.error(globalObject));
    }
    RELEASE_AND_RETURN(scope, posixMakeInt(globalObject, limit));
}

#if !OS(ANDROID)
static JSValue posixGetloadavg(JSGlobalObject* globalObject)
{
    double averages[3];
    if (getloadavg(averages, 3) != 3) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        return raise(globalObject, scope, runtime(globalObject).OSError(), "Load averages are unobtainable"_s);
    }
    return PyTuple::create(globalObject, { makeFloat(globalObject, averages[0]), makeFloat(globalObject, averages[1]), makeFloat(globalObject, averages[2]) });
}
#endif

static JSValue posixCpuCount(JSGlobalObject*)
{
    if (configuration().cpuCount > 0)
        return jsNumber(configuration().cpuCount);
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1)
        return jsUndefined();
    return jsNumber(static_cast<int32_t>(count));
}

static JSValue posixStrerror(JSGlobalObject* globalObject, JSValue codeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int code;
    if (failed(scope, posixToInt(globalObject, codeValue, code)))
        return { };
    RELEASE_AND_RETURN(scope, posixDecode(globalObject, strerror(code)));
}

static JSValue posixDeviceEncoding(JSGlobalObject* globalObject, JSValue fdValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int fd;
    if (failed(scope, posixToInt(globalObject, fdValue, fd)))
        return { };
    if (!isatty(fd))
        return jsUndefined();
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), "utf-8"_s));
}

// ---- Random bytes ----

static PyBytes* posixNewBytes(JSGlobalObject* globalObject, int64_t size)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyBytes* bytes = PyBytes::tryCreateUninitialized(vm, rt.structure<PyBytes>(), rt.bytesType(), static_cast<size_t>(size));
    if (!bytes) {
        raise(globalObject, scope, rt.MemoryError(), String());
        return nullptr;
    }
    bytes->mutableData()[size] = 0;
    return bytes;
}

static JSValue posixUrandom(JSGlobalObject* globalObject, JSValue sizeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size;
    if (failed(scope, posixToCount(globalObject, sizeValue, "size"_s, size)))
        return { };
    PyBytes* bytes = posixNewBytes(globalObject, size);
    if (!bytes)
        return { };
    cryptographicallyRandomValues(std::span<uint8_t> { bytes->mutableData(), static_cast<size_t>(size) });
    return bytes;
}

#if OS(LINUX)
static JSValue posixGetrandom(JSGlobalObject* globalObject, JSValue sizeValue, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size;
    int flags = 0;
    if (failed(scope, posixToSsize(globalObject, sizeValue, size)) || (flagsValue && failed(scope, posixToInt(globalObject, flagsValue, flags))))
        return { };
    if (size < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject, EINVAL));
    Vector<uint8_t> buffer;
    if (!buffer.tryGrow(static_cast<size_t>(size)))
        return raise(globalObject, scope, runtime(globalObject).MemoryError(), String());
    ssize_t count;
    do {
        // Not getrandom(), which is newer than the oldest C library this may run with.
        AllowThreads allow(globalObject);
        count = static_cast<ssize_t>(syscall(SYS_getrandom, buffer.mutableSpan().data(), buffer.size(), static_cast<unsigned>(flags)));
    } while (count < 0 && errno == EINTR && checkSignals(globalObject));
    if (count < 0)
        RELEASE_AND_RETURN(scope, posixError(globalObject));
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, buffer.span().first(static_cast<size_t>(count))));
}
#endif

// ---- The hook of the prompt ----

static JSValue posixInputhook(JSGlobalObject*) { return jsNumber(0); }
static JSValue posixIsInputhookInstalled(JSGlobalObject*) { return jsBoolean(false); }

// ---- The module ----

static constexpr NativeDef posixFunctions[] = {
    PY_FUNCTION("_create_environ", posixCreateEnviron, "()"),
    PY_FUNCTION("putenv", posixPutenv, "(name, value, /)"),
    PY_FUNCTION("unsetenv", posixUnsetenv, "(name, /)"),
#if OS(LINUX)
    PY_FUNCTION("_clearenv", posixClearenv, "()"),
#endif
    PY_FUNCTION("fspath", posixFspathFunction, "(path)"),
    PY_FUNCTION("_path_normpath", posixPathNormpath, "(path)"),
    PY_FUNCTION("_path_splitroot_ex", posixPathSplitrootEx, "(path, /)"),
    PY_FUNCTION("uname", posixUname, "()"),
    PY_FUNCTION("times", posixTimes, "()"),
    PY_FUNCTION("get_terminal_size", posixGetTerminalSize, "(fd=<unrepresentable>, /)"),
    PY_FUNCTION("sysconf", posixSysconf, "(name, /)"),
#if !OS(ANDROID)
    PY_FUNCTION("confstr", posixConfstr, "(name, /)"),
#endif
    PY_FUNCTION("fpathconf", posixFpathconf, "(fd, name, /)"),
    PY_FUNCTION("pathconf", posixPathconf, "(path, name)"),
#if !OS(ANDROID)
    PY_FUNCTION("getloadavg", posixGetloadavg, "()"),
#endif
    PY_FUNCTION("cpu_count", posixCpuCount, "()"),
    PY_FUNCTION("strerror", posixStrerror, "(code, /)"),
    PY_FUNCTION("device_encoding", posixDeviceEncoding, "(fd)"),
    PY_FUNCTION("urandom", posixUrandom, "(size, /)"),
#if OS(LINUX)
    PY_FUNCTION("getrandom", posixGetrandom, "(size, flags=0)"),
#endif
    PY_FUNCTION("_inputhook", posixInputhook, "()"),
    PY_FUNCTION("_is_inputhook_installed", posixIsInputhookInstalled, "()"),
};

// What os.py asks about to tell which functions take dir_fd, a file descriptor, or follow_symlinks.
static constexpr ASCIILiteral posixHaveFunctions[] = {
#if OS(LINUX)
    "HAVE_EVENTFD"_s,
    "HAVE_TIMERFD_CREATE"_s,
#endif
    "HAVE_FACCESSAT"_s,
    "HAVE_FCHDIR"_s,
    "HAVE_FCHMOD"_s,
    "HAVE_FCHMODAT"_s,
    "HAVE_FCHOWN"_s,
    "HAVE_FCHOWNAT"_s,
#if OS(LINUX)
    "HAVE_FEXECVE"_s,
#endif
    "HAVE_FDOPENDIR"_s,
    "HAVE_FPATHCONF"_s,
    "HAVE_FSTATAT"_s,
    "HAVE_FSTATVFS"_s,
    "HAVE_FTRUNCATE"_s,
    "HAVE_FUTIMENS"_s,
    "HAVE_FUTIMES"_s,
#if OS(LINUX)
    "HAVE_FUTIMESAT"_s,
#endif
    "HAVE_LINKAT"_s,
#if OS(DARWIN)
    "HAVE_LCHFLAGS"_s,
    "HAVE_LCHMOD"_s,
#endif
    "HAVE_LCHOWN"_s,
    "HAVE_LSTAT"_s,
    "HAVE_LUTIMES"_s,
#if OS(LINUX)
    "HAVE_MEMFD_CREATE"_s,
#endif
    "HAVE_MKDIRAT"_s,
    "HAVE_MKFIFOAT"_s,
    "HAVE_MKNODAT"_s,
    "HAVE_OPENAT"_s,
    "HAVE_READLINKAT"_s,
    "HAVE_RENAMEAT"_s,
    "HAVE_SYMLINKAT"_s,
    "HAVE_UNLINKAT"_s,
    "HAVE_UTIMENSAT"_s,
#if OS(LINUX)
    "HAVE_PTSNAME_R"_s,
#endif
};

void initializePosixModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    module->addFunctions(globalObject, posixFunctions);
    addPosixFileFunctions(globalObject, module);
    addPosixDescriptorFunctions(globalObject, module);
    addPosixProcessFunctions(globalObject, module);
    addPosixConstants(globalObject, module);

    JSValue dict = posixCreateEnviron(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "environ"_s, dict);
    posixTakeEnvironmentOfHost(globalObject, PyDict::from(dict));
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "error"_s, rt.OSError());
    module->add(globalObject, "stat_result"_s, posixStatResultType(globalObject));
    module->add(globalObject, "uname_result"_s, cachedStructSeqType(globalObject, posixUnameResultDef));
    module->add(globalObject, "times_result"_s, cachedStructSeqType(globalObject, posixTimesResultDef));
    module->add(globalObject, "terminal_size"_s, cachedStructSeqType(globalObject, posixTerminalSizeDef));
    RETURN_IF_EXCEPTION(scope, void());

    JSArray* have = newList(globalObject);
    for (auto name : posixHaveFunctions) {
        listAppend(globalObject, have, makeStr(vm, name));
        RETURN_IF_EXCEPTION(scope, void());
    }
    module->add(globalObject, "_have_functions"_s, have);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // !OS(WINDOWS)
