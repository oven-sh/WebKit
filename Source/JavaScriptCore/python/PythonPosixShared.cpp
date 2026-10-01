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
#include "PythonPosix.h"

#if OS(UNIX)

#include "PythonCodecs.h"
#include <errno.h>
#include <wtf/SafeStrerror.h>

// The converters of posix, and what raises: Modules/posixmodule.c of CPython.

namespace JSC { namespace Python {

// ---- What raises

static JSValue raiseErrno(JSGlobalObject* globalObject, ThrowScope& scope, int errorNumber, JSValue filename, JSValue filename2)
{
    if (!filename2)
        return raiseOSError(globalObject, scope, errorNumber, filename);
    // PyErr_SetFromErrnoWithFilenameObjects()
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(errorNumber));
    arguments.append(jsString(vm, String::fromUTF8(safeStrerror(errorNumber).span())));
    arguments.append(filename);
    arguments.append(jsNumber(0));
    arguments.append(filename2);
    JSValue exception = call(globalObject, globalObject->pyRealm()->typeOSError(), arguments);
    RETURN_IF_EXCEPTION(scope, { });
    setContext(globalObject, asObject(exception));
    throwException(globalObject, scope, exception);
    return { };
}

JSValue raisePosixError(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return raiseErrno(globalObject, scope, errno, JSValue(), JSValue());
}

JSValue raisePathObjectError(JSGlobalObject* globalObject, ThrowScope& scope, JSValue path)
{
    return raiseErrno(globalObject, scope, errno, path, JSValue());
}

JSValue raisePathError(JSGlobalObject* globalObject, ThrowScope& scope, const PathArgument& path)
{
    return raiseErrno(globalObject, scope, errno, path.object, JSValue());
}

JSValue raisePathError(JSGlobalObject* globalObject, ThrowScope& scope, const PathArgument& path, const PathArgument& path2)
{
    return raiseErrno(globalObject, scope, errno, path.object, path2.object);
}

JSValue raiseArgumentUnavailable(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, ASCIILiteral argument)
{
    return raise(globalObject, scope, BuiltinType::NotImplementedError, concatenate(function.isNull() ? ""_s : function, function.isNull() ? ""_s : ": "_s, argument, " unavailable on this platform"_s));
}

void checkPathAndDirectory(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, const PathArgument& path, int directory)
{
    if (!path.hasNarrow && directory != defaultDirectoryDescriptor)
        raiseValueError(globalObject, scope, concatenate(function, ": can't specify dir_fd without matching path"_s));
}

void checkDirectoryAndDescriptor(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, int directory, int descriptor)
{
    if (directory != defaultDirectoryDescriptor && descriptor != -1)
        raiseValueError(globalObject, scope, concatenate(function, ": can't specify both dir_fd and fd"_s));
}

void checkDescriptorAndFollowing(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, int descriptor, bool followsSymlinks)
{
    if (descriptor >= 0 && !followsSymlinks)
        raiseValueError(globalObject, scope, concatenate(function, ": cannot use fd and follow_symlinks together"_s));
}

// ---- Converters

static bool hasIndex(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    return classify(value).isInt() || typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_index);
}

std::optional<int> toDescriptor(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (value.isBoolean() && !warn(globalObject, BuiltinType::RuntimeWarning, "bool is used as a file descriptor"_s))
        return std::nullopt;
    JSValue index = toInt(globalObject, value);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    auto number = tryInt64(index);
    bool isNegative = compareInts(index, jsNumber(0)) < 0;
    if ((!number && !isNegative) || (number && *number > std::numeric_limits<int>::max())) {
        raise(globalObject, scope, BuiltinType::OverflowError, "fd is greater than maximum"_s);
        return std::nullopt;
    }
    if (!number || *number < std::numeric_limits<int>::min()) {
        raise(globalObject, scope, BuiltinType::OverflowError, "fd is less than minimum"_s);
        return std::nullopt;
    }
    return static_cast<int>(*number);
}

std::optional<int> toDirectoryDescriptor(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value || isNone(value))
        return defaultDirectoryDescriptor;
    if (hasIndex(globalObject, value))
        RELEASE_AND_RETURN(scope, toDescriptor(globalObject, value));
    raiseTypeError(globalObject, scope, concatenate("argument should be integer or None, not "_s, typeOf(globalObject, value)->nameWithoutModule(globalObject)));
    return std::nullopt;
}

std::optional<int> toFileDescriptorOrFile(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    std::optional<int> descriptor;
    if (isInstance(globalObject, value, realm->typeInt())) {
        if (value.isBoolean() && !warn(globalObject, BuiltinType::RuntimeWarning, "bool is used as a file descriptor"_s))
            return std::nullopt;
        descriptor = toCInt(globalObject, value);
    } else {
        JSValue method = getAttributeIfPresent(globalObject, value, vm.pythonNames().attribute_fileno);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (!method) {
            raiseTypeError(globalObject, scope, "argument must be an int, or have a fileno() method."_s);
            return std::nullopt;
        }
        JSValue number = call(globalObject, method);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (!isInstance(globalObject, number, realm->typeInt())) {
            raiseTypeError(globalObject, scope, "fileno() returned a non-integer"_s);
            return std::nullopt;
        }
        descriptor = toCInt(globalObject, number);
    }
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (*descriptor < 0) {
        raiseValueError(globalObject, scope, concatenate("file descriptor cannot be a negative integer ("_s, *descriptor, ')'));
        return std::nullopt;
    }
    return descriptor;
}

// _Py_Uid_Converter() and _Py_Gid_Converter(), which differ in a word. It is unsigned, and -1 will do all the same.
static std::optional<uint32_t> toID(JSGlobalObject* globalObject, JSValue value, ASCIILiteral what)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    static_assert(sizeof(uid_t) == sizeof(uint32_t) && sizeof(gid_t) == sizeof(uint32_t));
    JSValue index = toInt(globalObject, value);
    if (scope.exception()) {
        if (!scope.tryClearException())
            return std::nullopt;
        raiseTypeError(globalObject, scope, concatenate(what, " should be integer, not "_s, typeOf(globalObject, value)->nameWithoutModule(globalObject)));
        return std::nullopt;
    }
    auto number = tryInt64(index);
    if (number && *number == -1)
        return static_cast<uint32_t>(-1);
    // As in CPython, one that is too large for it and fits a long is said to be too small.
    if (compareInts(index, jsNumber(0)) < 0 || (number && *number > std::numeric_limits<uint32_t>::max())) {
        raise(globalObject, scope, BuiltinType::OverflowError, concatenate(what, " is less than minimum"_s));
        return std::nullopt;
    }
    if (!number) {
        raise(globalObject, scope, BuiltinType::OverflowError, concatenate(what, " is greater than maximum"_s));
        return std::nullopt;
    }
    return static_cast<uint32_t>(*number);
}

std::optional<uid_t> toUserID(JSGlobalObject* globalObject, JSValue value) { return toID(globalObject, value, "uid"_s); }
std::optional<gid_t> toGroupID(JSGlobalObject* globalObject, JSValue value) { return toID(globalObject, value, "gid"_s); }

JSValue intFromUserID(JSGlobalObject* globalObject, uid_t id)
{
    if (id == static_cast<uid_t>(-1))
        return jsNumber(-1);
    return intFromInt64(globalObject, id);
}

std::optional<CString> toFileSystemEncoded(JSGlobalObject* globalObject, JSValue value)
{
    return toFileSystemPath(globalObject, value, "embedded null byte"_s);
}

JSValue decodeFileSystemBytes(JSGlobalObject* globalObject, std::span<const char> characters)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSUint8Array* bytes = newBytes(globalObject, byteCast<uint8_t>(characters));
    RETURN_IF_EXCEPTION(scope, { });
    String text = decodeBytes(globalObject, bytes->span(), "utf-8"_s, "surrogateescape"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, text));
}

bool PathArgument::isBytes(JSGlobalObject* globalObject) const
{
    return object && isInstance(globalObject, object, globalObject->pyRealm()->typeBytes());
}

JSValue nameLike(JSGlobalObject* globalObject, const PathArgument& path, std::span<const char> characters)
{
    if (path.hasNarrow && path.isBytes(globalObject))
        return newBytes(globalObject, byteCast<uint8_t>(characters));
    if (path.options & PathArgument::MakesWide)
        return decodeBytesToObject(globalObject, byteCast<uint8_t>(characters), "utf-8"_s, "surrogatepass"_s);
    return decodeFileSystemBytes(globalObject, characters);
}

bool PathArgument::convert(JSGlobalObject* globalObject, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto prefix = [&] { return concatenate(functionName.isNull() ? ""_s : functionName, functionName.isNull() ? ""_s : ": "_s); };
    ASCIILiteral name = argumentName.isNull() ? "path"_s : argumentName;

    if ((!given || isNone(given)) && (options & Nullable))
        return true;
    JSValue value = given;
    // Before __fspath__() is asked, so that what that returns is not taken for an open file.
    bool isIndex = (options & AllowsDescriptor) && hasIndex(globalObject, value);
    bool isByteString = isInstance(globalObject, value, realm->typeBytes());
    bool isText = !!stringIn(value);
    if (!isIndex && !isText && !isByteString) {
        JSValue self;
        JSValue method = lookupSpecial(globalObject, value, Identifier::fromString(vm, "__fspath__"_s), self);
        RETURN_IF_EXCEPTION(scope, false);
        if (!method || isNone(method)) {
            ASCIILiteral wanted = (options & AllowsDescriptor) && (options & Nullable) ? "string, bytes, os.PathLike, integer or None"_s
                : (options & AllowsDescriptor) ? "string, bytes, os.PathLike or integer"_s
                : (options & Nullable) ? "string, bytes, os.PathLike or None"_s : "string, bytes or os.PathLike"_s;
            raiseTypeError(globalObject, scope, concatenate(prefix(), name, " should be "_s, wanted, ", not "_s, typeOf(globalObject, value)->nameWithoutModule(globalObject)));
            return false;
        }
        JSValue result = self ? callMethod(globalObject, method, self) : call(globalObject, method);
        RETURN_IF_EXCEPTION(scope, false);
        isText = !!stringIn(result);
        isByteString = isInstance(globalObject, result, realm->typeBytes());
        if (!isText && !isByteString) {
            raiseTypeError(globalObject, scope, concatenate("expected "_s, typeOf(globalObject, value)->nameWithoutModule(globalObject), ".__fspath__() to return str or bytes, not "_s, typeOf(globalObject, result)->nameWithoutModule(globalObject)));
            return false;
        }
        value = result;
    }
    if (isIndex) {
        auto converted = toDescriptor(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        descriptor = *converted;
        isDescriptor = true;
        object = value;
        return true;
    }
    std::optional<ByteVector> encoded;
    std::span<const uint8_t> content;
    if (isText) {
        encoded = encodeString(globalObject, stringIn(value), "utf-8"_s, options & MakesWide ? "surrogatepass"_s : "surrogateescape"_s);
        RETURN_IF_EXCEPTION(scope, false);
        content = encoded->span();
    } else
        content = *builtinBufferOf(value);
    if (!(options & NonStrict) && WTF::find(content, static_cast<uint8_t>(0)) != notFound) {
        raiseValueError(globalObject, scope, concatenate(prefix(), "embedded null character in "_s, name));
        return false;
    }
    bytes = CString(byteCast<char>(content));
    hasNarrow = true;
    object = value;
    return true;
}

// ---- stat_result

// fill_time(): a time as a whole number of seconds, as a float, and in nanoseconds.
static void appendTime(JSGlobalObject* globalObject, int64_t seconds, uint64_t nanoseconds, JSValue& whole, JSValue& real, JSValue& total)
{
    whole = intFromInt64(globalObject, seconds);
    real = floatFromDouble(multiplyAdd(1e-9, static_cast<double>(nanoseconds), static_cast<double>(seconds)));
    JSValue inNanoseconds = binaryOperation(globalObject, BinaryOperator::Mult, false, whole, jsNumber(1000000000));
    total = binaryOperation(globalObject, BinaryOperator::Add, false, inNanoseconds, intFromUInt64(globalObject, nanoseconds));
}

// _PyLong_FromDev()
static JSValue intFromDevice(JSGlobalObject* globalObject, dev_t device)
{
#ifdef NODEV
    if (device == NODEV)
        return intFromInt64(globalObject, static_cast<int64_t>(device));
#endif
    return intFromUInt64(globalObject, static_cast<uint64_t>(device));
}

JSValue statResultFrom(JSGlobalObject* globalObject, const struct stat& status)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
#if OS(DARWIN)
    auto& accessed = status.st_atimespec;
    auto& modified = status.st_mtimespec;
    auto& changed = status.st_ctimespec;
#else
    auto& accessed = status.st_atim;
    auto& modified = status.st_mtim;
    auto& changed = status.st_ctim;
#endif
    JSValue whole[3];
    JSValue real[3];
    JSValue total[3];
    appendTime(globalObject, accessed.tv_sec, accessed.tv_nsec, whole[0], real[0], total[0]);
    appendTime(globalObject, modified.tv_sec, modified.tv_nsec, whole[1], real[1], total[1]);
    appendTime(globalObject, changed.tv_sec, changed.tv_nsec, whole[2], real[2], total[2]);
    RETURN_IF_EXCEPTION(scope, { });

    MarkedArgumentBuffer values;
    values.append(jsNumber(static_cast<int32_t>(status.st_mode)));
    values.append(intFromUInt64(globalObject, status.st_ino));
    values.append(intFromDevice(globalObject, status.st_dev));
    values.append(intFromInt64(globalObject, status.st_nlink));
    values.append(intFromUserID(globalObject, status.st_uid));
    values.append(intFromUserID(globalObject, status.st_gid));
    values.append(intFromInt64(globalObject, status.st_size));
    for (auto& group : { whole, real, total }) {
        for (unsigned i = 0; i < 3; ++i)
            values.append(group[i]);
    }
    values.append(intFromInt64(globalObject, status.st_blksize));
    values.append(intFromInt64(globalObject, status.st_blocks));
    values.append(intFromDevice(globalObject, status.st_rdev));
#if OS(DARWIN)
    values.append(intFromInt64(globalObject, static_cast<long>(status.st_flags)));
    values.append(intFromInt64(globalObject, static_cast<long>(status.st_gen)));
    values.append(floatFromDouble(multiplyAdd(static_cast<double>(status.st_birthtimespec.tv_nsec), 1e-9, static_cast<double>(static_cast<unsigned long>(status.st_birthtimespec.tv_sec)))));
#endif
    RELEASE_AND_RETURN(scope, newStructSequence(globalObject, posixState(globalObject).statResult.get(), values));
}

} } // namespace JSC::Python

#endif // OS(UNIX)
