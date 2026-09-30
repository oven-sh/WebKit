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
#include "PythonLifecycle.h"

#if OS(UNIX)

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyRealm.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonImport.h"
#include "PythonOperations.h"
#include "PythonPlatform.h"
#include "PythonPosix.h"
#include "PythonSequences.h"
#include <unistd.h>
#if OS(DARWIN)
#include <mach-o/dyld.h>
#endif

// Where things are: Modules/getpath.c of CPython. What works it out is Modules/getpath.py, which is here as it is there, in lib. This gives it what it goes by, and a few functions to look at files with, runs it, and takes what it
// comes to.

namespace JSC { namespace Python {

namespace {

// The "U" of PyArg_ParseTuple(), as bytes to give to the system. Nothing if it raised.
std::optional<CString> pathArgument(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!value || !stringIn(value)) {
        raiseTypeError(globalObject, scope, concatenate("argument 1 must be str, not "_s, value ? typeNameOfArgument(globalObject, value) : String("nothing"_s)));
        return std::nullopt;
    }
    RELEASE_AND_RETURN(scope, toFileSystemEncoded(globalObject, value));
}

JSValue pathFrom(JSGlobalObject* globalObject, std::span<const char> bytes) { return decodeFileSystemBytes(globalObject, bytes); }

CString normalized(const CString& path)
{
    Vector<char> result = normalizePath(path.span());
    return CString(result.span());
}

// join_relfile() of CPython's Python/fileutils.c
CString joinRelativeFile(const CString& directory, const CString& file)
{
    if (!directory.length())
        return file;
    bool needsSeparator = directory.length() > 1 && directory.span().back() != '/';
    Vector<char> result;
    result.append(directory.span());
    if (needsSeparator)
        result.append('/');
    result.append(file.span());
    return CString(result.span());
}

std::optional<struct stat> statOf(const CString& path)
{
    struct stat result;
    if (::stat(path.data(), &result))
        return std::nullopt;
    return result;
}

} // anonymous namespace

#define PATH_ARGUMENT(name) \
    auto name##Converted = pathArgument(globalObject, args.at(0)); \
    RETURN_IF_EXCEPTION(scope, { }); \
    CString name = WTF::move(*name##Converted)

PYTHON_NATIVE(getPathAbspath)
{
    NATIVE_PROLOGUE();
    PATH_ARGUMENT(given);
    // _Py_abspath()
    CString path = normalized(given);
    if (path.length() && path.span()[0] == '/')
        RELEASE_AND_RETURN(scope, JSValue::encode(pathFrom(globalObject, path.span())));
    char directory[PATH_MAX];
    if (!getcwd(directory, sizeof(directory)))
        return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "failed to make path absolute"_s));
    if (!path.length() || path == "."_s)
        RELEASE_AND_RETURN(scope, JSValue::encode(pathFrom(globalObject, unsafeSpan(directory))));
    Vector<char> result;
    result.append(unsafeSpan(directory));
    result.append('/');
    result.append(path.span());
    RELEASE_AND_RETURN(scope, JSValue::encode(pathFrom(globalObject, result.span())));
}

// basename(path) and dirname(path)
PYTHON_NATIVE(getPathSplit)
{
    NATIVE_PROLOGUE();
    bool isDirectory = unpack<bool>(callFrame, 0);
    if (!stringIn(args.at(0)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "argument 1 must be str"_s));
    String path = stringIn(args.at(0))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    size_t separator = path.reverseFind('/');
    if (separator == notFound)
        return JSValue::encode(isDirectory ? JSValue(jsEmptyString(vm)) : args.at(0));
    return JSValue::encode(jsString(vm, isDirectory ? path.left(separator) : path.substring(separator + 1)));
}

PYTHON_NATIVE(getPathIsAbs)
{
    NATIVE_PROLOGUE();
    PATH_ARGUMENT(path);
    return JSValue::encode(jsBoolean(path.length() && path.span()[0] == '/'));
}

PYTHON_NATIVE(getPathHasSuffix)
{
    NATIVE_PROLOGUE();
    if (!stringIn(args.at(0)) || !stringIn(args.at(1)))
        return JSValue::encode(raiseTypeError(globalObject, scope, "arguments must be str"_s));
    String path = stringIn(args.at(0))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String suffix = stringIn(args.at(1))->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(path.endsWith(suffix)));
}

enum class FileKind : uint8_t { Directory, File, ExecutableFile };

// isdir(path), isfile(path) and isxfile(path)
PYTHON_NATIVE(getPathIsKind)
{
    NATIVE_PROLOGUE();
    auto kind = unpack<FileKind>(callFrame, 0);
    PATH_ARGUMENT(path);
    auto found = statOf(path);
    if (!found)
        return JSValue::encode(jsBoolean(false));
    switch (kind) {
    case FileKind::Directory:
        return JSValue::encode(jsBoolean(S_ISDIR(found->st_mode)));
    case FileKind::File:
        return JSValue::encode(jsBoolean(S_ISREG(found->st_mode)));
    case FileKind::ExecutableFile:
        return JSValue::encode(jsBoolean(S_ISREG(found->st_mode) && (found->st_mode & 0111)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// joinpath(*paths). What comes before the last that is absolute is left out, and so is None.
PYTHON_NATIVE(getPathJoin)
{
    NATIVE_PROLOGUE();
    Vector<std::optional<CString>> parts;
    unsigned first = 0;
    for (unsigned i = 0; i < args.size(); ++i) {
        if (isNone(args[i])) {
            parts.append(std::nullopt);
            continue;
        }
        if (!stringIn(args[i]))
            return JSValue::encode(raiseTypeError(globalObject, scope, "all arguments to joinpath() must be str or None"_s));
        auto part = toFileSystemEncoded(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        if (part->length() && part->span()[0] == '/')
            first = i;
        parts.append(WTF::move(*part));
    }
    CString result;
    bool hasAny = false;
    for (unsigned i = first; i < parts.size(); ++i) {
        if (!parts[i])
            continue;
        result = hasAny && result.length() ? joinRelativeFile(result, *parts[i]) : *parts[i];
        hasAny = true;
    }
    if (!hasAny)
        return JSValue::encode(jsEmptyString(vm));
    RELEASE_AND_RETURN(scope, JSValue::encode(pathFrom(globalObject, normalized(result).span())));
}

// readlines(path): the lines of a file that is in UTF-8, without what ends them
PYTHON_NATIVE(getPathReadLines)
{
    NATIVE_PROLOGUE();
    PATH_ARGUMENT(path);
    FILE* file = fopen(path.data(), "rb");
    if (!file)
        return JSValue::encode(raiseOSError(globalObject, scope, errno));
    constexpr size_t most = 32 * 1024;
    Vector<char> buffer(most);
    size_t count = fread(buffer.mutableSpan().data(), 1, most, file);
    fclose(file);
    JSArray* lines = newList(globalObject);
    if (!count)
        return JSValue::encode(lines);
    if (count >= most)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::MemoryError, "cannot read file larger than 32KB during initialization"_s));
    auto text = buffer.span().first(count);
    auto append = [&] (std::span<const char> line) {
        JSValue decoded = pathFrom(globalObject, line);
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        listAppend(globalObject, lines, decoded);
    };
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n')
            continue;
        size_t end = i;
        while (end > start && text[end - 1] == '\r')
            --end;
        append(text.subspan(start, end - start));
        RETURN_IF_EXCEPTION(scope, { });
        start = i + 1;
    }
    if (start < text.size()) {
        append(text.subspan(start));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(lines);
}

// realpath(path): what a link is a link to, and so on. Only the file itself is looked at, and none of the directories on the way to it.
PYTHON_NATIVE(getPathRealPath)
{
    NATIVE_PROLOGUE();
    PATH_ARGUMENT(path);
    // As many as Linux will follow
    for (unsigned links = 0; links < 40; ++links) {
        char resolved[PATH_MAX + 1];
        ssize_t length = readlink(path.data(), resolved, PATH_MAX);
        if (length == -1)
            RELEASE_AND_RETURN(scope, JSValue::encode(pathFrom(globalObject, path.span())));
        CString target(std::span<const char>(resolved, static_cast<size_t>(length)));
        if (length && resolved[0] == '/') {
            path = target;
            continue;
        }
        auto span = path.span();
        size_t separator = span.size();
        while (separator && span[separator - 1] != '/')
            --separator;
        CString directory = separator ? CString(span.first(separator - 1)) : path;
        path = normalized(joinRelativeFile(directory, target));
    }
    return JSValue::encode(raise(globalObject, scope, BuiltinType::OSError, "maximum number of symbolic links reached"_s));
}

// warn(message)
PYTHON_NATIVE(getPathWarn)
{
    NATIVE_PROLOGUE();
    bool warns = unpack<bool>(callFrame, 0);
    if (!warns)
        RETURN_NONE();
    PATH_ARGUMENT(message);
    fprintf(stderr, "%s\n", message.data());
    RETURN_NONE();
}

// _PyConfig_InitPathConfig(). It may throw.
void computePathConfiguration(JSGlobalObject* globalObject, Configuration& configuration)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();

    auto stringOrNone = [&] (const String& text) -> JSValue { return text.isNull() ? jsUndefined() : JSValue(jsString(vm, text)); };
    auto nonEmptyOrNone = [&] (const String& text) -> JSValue { return text.isEmpty() ? jsUndefined() : JSValue(jsString(vm, text)); };
    auto listOf = [&] (const Vector<String>& strings) {
        MarkedArgumentBuffer items;
        for (auto& string : strings)
            items.append(jsString(vm, string));
        return newList(globalObject, items);
    };

    // As much of _PyConfig_AsDict() as it looks at
    PyDict* config = PyDict::create(globalObject);
    auto setConfig = [&] (ASCIILiteral key, JSValue value) { config->setString(globalObject, key, value); };
    setConfig("program_name"_s, stringOrNone(configuration.programName));
    setConfig("home"_s, stringOrNone(configuration.home));
    setConfig("pythonpath_env"_s, stringOrNone(configuration.searchPathFromEnvironment));
    setConfig("platlibdir"_s, stringOrNone(configuration.platformLibraryDirectory));
    setConfig("executable"_s, nonEmptyOrNone(configuration.executable));
    setConfig("base_executable"_s, nonEmptyOrNone(configuration.baseExecutable));
    setConfig("prefix"_s, nonEmptyOrNone(configuration.prefix));
    setConfig("exec_prefix"_s, nonEmptyOrNone(configuration.executablePrefix));
    setConfig("base_prefix"_s, nonEmptyOrNone(configuration.basePrefix));
    setConfig("base_exec_prefix"_s, nonEmptyOrNone(configuration.baseExecutablePrefix));
    setConfig("stdlib_dir"_s, nonEmptyOrNone(configuration.libraryDirectory));
    setConfig("module_search_paths"_s, listOf(configuration.moduleSearchPaths));
    setConfig("module_search_paths_set"_s, jsNumber(configuration.hasModuleSearchPaths));
    setConfig("use_environment"_s, jsNumber(configuration.usesEnvironment));
    setConfig("site_import"_s, jsNumber(configuration.importsSite));
    setConfig("safe_path"_s, jsNumber(configuration.hasSafePath));
    setConfig("isolated"_s, jsNumber(configuration.isIsolated));
    setConfig("_is_python_build"_s, jsNumber(0));
    setConfig("orig_argv"_s, listOf(configuration.originalArguments));

    PyDict* globals = PyDict::create(globalObject);
    auto set = [&] (ASCIILiteral key, JSValue value) { globals->setString(globalObject, key, value); };
    auto fromEnvironment = [&] (const char* name) -> JSValue {
        const char* value = getenv(name);
        return value ? pathFrom(globalObject, unsafeSpan(value)) : jsUndefined();
    };
    set("config"_s, config);
#if OS(DARWIN)
    set("os_name"_s, jsNontrivialString(vm, "darwin"_s));
#else
    set("os_name"_s, jsNontrivialString(vm, "posix"_s));
#endif
    set("WITH_NEXT_FRAMEWORK"_s, jsNumber(0));
    // What the system says that the process is running, where it can be asked.
    Vector<char> program;
#if OS(DARWIN)
    {
        uint32_t length = 256;
        program.grow(length + 1);
        if (_NSGetExecutablePath(program.mutableSpan().data(), &length)) {
            program.grow(length + 1);
            if (_NSGetExecutablePath(program.mutableSpan().data(), &length))
                program[0] = '\0';
        }
    }
#elif OS(LINUX)
    {
        program.grow(PATH_MAX + 1);
        ssize_t length = readlink("/proc/self/exe", program.mutableSpan().data(), PATH_MAX);
        program[std::max<ssize_t>(length, 0)] = '\0';
    }
#endif
    if (program.isEmpty() || program[0] != '/')
        program = { '\0' };
    // Where `make install` was told to put it, which CPython has built into it and falls back on when it cannot make out where it is from the name that it was started by. That is <prefix>/bin/python3, so it is
    // worked out from where the program is.
    JSValue prefix = jsNontrivialString(vm, "/usr/local"_s);
    if (auto path = unsafeSpan(program.span().data()); !path.empty()) {
        for (unsigned i = 0; i < 2 && !path.empty(); ++i)
            path = path.first(reverseFind(path, '/'));
        if (!path.empty()) {
            prefix = pathFrom(globalObject, path);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    set("PREFIX"_s, prefix);
    set("EXEC_PREFIX"_s, prefix);
    set("PYTHONPATH"_s, jsUndefined());
    set("VPATH"_s, jsUndefined());
    set("PLATLIBDIR"_s, jsNontrivialString(vm, "lib"_s));
    set("PYDEBUGEXT"_s, jsUndefined());
    set("VERSION_MAJOR"_s, jsNumber(PYTHON_VERSION_MAJOR));
    set("VERSION_MINOR"_s, jsNumber(PYTHON_VERSION_MINOR));
    set("PYWINVER"_s, jsUndefined());
    set("EXE_SUFFIX"_s, jsUndefined());
    set("ENV_PATH"_s, fromEnvironment("PATH"));
    RETURN_IF_EXCEPTION(scope, void());
    set("ENV_PYTHONHOME"_s, fromEnvironment("PYTHONHOME"));
    RETURN_IF_EXCEPTION(scope, void());
    set("ENV_PYTHONEXECUTABLE"_s, fromEnvironment("PYTHONEXECUTABLE"));
    RETURN_IF_EXCEPTION(scope, void());
    set("ENV___PYVENV_LAUNCHER__"_s, fromEnvironment("__PYVENV_LAUNCHER__"));
    RETURN_IF_EXCEPTION(scope, void());
    unsetenv("__PYVENV_LAUNCHER__");

    // progname_to_dict(), which asks only where that is how it is done
    JSValue realExecutable = jsUndefined();
#if OS(DARWIN)
    if (program[0]) {
        realExecutable = pathFrom(globalObject, unsafeSpan(program.span().data()));
        RETURN_IF_EXCEPTION(scope, void());
    }
#endif
    set("real_executable"_s, realExecutable);
    set("library"_s, jsUndefined());
    set("executable_dir"_s, jsUndefined());
    set("py_setpath"_s, jsUndefined());
    set("ABI_THREAD"_s, jsUndefined());
    set("winreg"_s, jsUndefined());
    set("__builtins__"_s, realm->builtinsModule());

    auto function = [&] (ASCIILiteral name, NativeFunction native, unsigned data, ASCIILiteral signature) {
        set(name, PyNativeFunction::create(vm, globalObject, 0, name, native, PyNativeFunction::Kind::Function, nullptr, data, ImplementationVisibility::Public, signature, PyNativeFunction::Arguments::AreNotChecked));
    };
    function("abspath"_s, getPathAbspath, 0, "(path, /)"_s);
    function("basename"_s, getPathSplit, pack(false), "(path, /)"_s);
    function("dirname"_s, getPathSplit, pack(true), "(path, /)"_s);
    function("hassuffix"_s, getPathHasSuffix, 0, "(path, suffix, /)"_s);
    function("isabs"_s, getPathIsAbs, 0, "(path, /)"_s);
    function("isdir"_s, getPathIsKind, pack(FileKind::Directory), "(path, /)"_s);
    function("isfile"_s, getPathIsKind, pack(FileKind::File), "(path, /)"_s);
    function("isxfile"_s, getPathIsKind, pack(FileKind::ExecutableFile), "(path, /)"_s);
    function("joinpath"_s, getPathJoin, 0, "(*paths)"_s);
    function("readlines"_s, getPathReadLines, 0, "(path, /)"_s);
    function("realpath"_s, getPathRealPath, 0, "(path, /)"_s);
    function("warn"_s, getPathWarn, pack(configuration.warnsOfPathConfiguration), "(message, /)"_s);

    JSValue compile = getStoredAttribute(vm, realm->builtinsModule(), Identifier::fromString(vm, "compile"_s));
    JSValue exec = getStoredAttribute(vm, realm->builtinsModule(), Identifier::fromString(vm, "exec"_s));
    JSValue code = call(globalObject, compile, newBytes(globalObject, getPathSource()), jsNontrivialString(vm, "<frozen getpath>"_s), jsNontrivialString(vm, "exec"_s));
    RETURN_IF_EXCEPTION(scope, void());
    call(globalObject, exec, code, globals);
    RETURN_IF_EXCEPTION(scope, void());

    // As much of _PyConfig_FromDict() as it can have changed
    auto take = [&] (String& target, ASCIILiteral key) {
        JSValue value = config->getString(globalObject, key);
        if (value && stringIn(value))
            target = stringIn(value)->value(globalObject);
    };
    take(configuration.programName, "program_name"_s);
    take(configuration.home, "home"_s);
    take(configuration.executable, "executable"_s);
    take(configuration.baseExecutable, "base_executable"_s);
    take(configuration.prefix, "prefix"_s);
    take(configuration.executablePrefix, "exec_prefix"_s);
    take(configuration.basePrefix, "base_prefix"_s);
    take(configuration.baseExecutablePrefix, "base_exec_prefix"_s);
    take(configuration.libraryDirectory, "stdlib_dir"_s);
    take(configuration.platformLibraryDirectory, "platlibdir"_s);
    RETURN_IF_EXCEPTION(scope, void());
    auto takeFlag = [&] (bool& target, ASCIILiteral key) {
        if (JSValue value = config->getString(globalObject, key))
            target = isTrue(globalObject, value);
    };
    takeFlag(configuration.importsSite, "site_import"_s);
    takeFlag(configuration.usesEnvironment, "use_environment"_s);
    takeFlag(configuration.hasSafePath, "safe_path"_s);
    takeFlag(configuration.isIsolated, "isolated"_s);
    takeFlag(configuration.hasModuleSearchPaths, "module_search_paths_set"_s);
    RETURN_IF_EXCEPTION(scope, void());
    if (JSValue paths = config->getString(globalObject, "module_search_paths"_s); paths && isInstance(globalObject, paths, realm->typeList())) {
        configuration.moduleSearchPaths.clear();
        JSArray* list = asList(paths);
        for (unsigned i = 0; i < list->length(); ++i) {
            JSValue item = listGet(globalObject, list, i);
            if (stringIn(item))
                configuration.moduleSearchPaths.append(stringIn(item)->value(globalObject));
        }
    }
}

} } // namespace JSC::Python

#endif // OS(UNIX)
