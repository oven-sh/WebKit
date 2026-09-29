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


#pragma once

#include "PythonIO.h"

#if OS(UNIX)

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

// What the parts of posix share: the converters and what raises, of CPython's Modules/posixmodule.c.

namespace JSC { namespace Python {

PosixModuleState& posixState(JSGlobalObject*);
void addPosixFileFunctions(JSGlobalObject*, JSObject* module);
void addPosixProcessFunctions(JSGlobalObject*, JSObject* module);
void initializePosixFileTypes(JSGlobalObject*, PosixModuleState&);
void initializePosixProcessTypes(JSGlobalObject*, PosixModuleState&);
JSValue newResourceUsage(JSGlobalObject*, const struct rusage&); // A resource.struct_rusage

static constexpr int defaultDirectoryDescriptor = AT_FDCWD; // DEFAULT_DIR_FD

// path_t: an argument that is the name of a file, or, where that will do, an open file.
struct PathArgument {
    enum Option : unsigned {
        Nullable = 1, // It can be None.
        AllowsDescriptor = 2,
        NonStrict = 4, // It can have a zero in it.
    };

    PathArgument(ASCIILiteral functionName, ASCIILiteral argumentName, unsigned options = 0)
        : functionName(functionName)
        , argumentName(argumentName)
        , options(options)
    {
    }

    // path_converter(). False if it raised.
    bool convert(JSGlobalObject*, JSValue);

    // Null if it is None or an open file.
    const char* narrow() const { return hasNarrow ? bytes.data() : nullptr; }
    bool isBytes(JSGlobalObject*) const;

    ASCIILiteral functionName;
    ASCIILiteral argumentName;
    unsigned options;
    CString bytes;
    bool hasNarrow { false };
    int descriptor { -1 };
    bool isDescriptor { false };
    JSValue object; // What was given, or what its __fspath__() returned. Empty if it was None.
};

// posix_error(), path_error() and path_error2(): OSError, for what errno is.
JSValue raisePosixError(JSGlobalObject*, ThrowScope&);
JSValue raisePathError(JSGlobalObject*, ThrowScope&, const PathArgument&);
JSValue raisePathError(JSGlobalObject*, ThrowScope&, const PathArgument&, const PathArgument&);
JSValue raisePathObjectError(JSGlobalObject*, ThrowScope&, JSValue path);
// argument_unavailable_error()
JSValue raiseArgumentUnavailable(JSGlobalObject*, ThrowScope&, ASCIILiteral function, ASCIILiteral argument);

// Each is nothing if it raised.
std::optional<int> toDescriptor(JSGlobalObject*, JSValue); // _fd_converter()
std::optional<int> toDirectoryDescriptor(JSGlobalObject*, JSValue); // dir_fd_converter(). What was not given is None.
std::optional<int> toFileDescriptorOrFile(JSGlobalObject*, JSValue); // `fildes`: PyObject_AsFileDescriptor()
std::optional<uid_t> toUserID(JSGlobalObject*, JSValue); // _Py_Uid_Converter()
std::optional<gid_t> toGroupID(JSGlobalObject*, JSValue); // _Py_Gid_Converter()
std::optional<CString> toFileSystemEncoded(JSGlobalObject*, JSValue); // `unicode_fs_encoded`: PyUnicode_FSConverter()

// Strings as a program is given them: each ended by a zero, and after the last of them, null.
class StringArray {
public:
    void append(CString&& string) { m_strings.append(WTF::move(string)); }
    size_t size() const { return m_strings.size(); }
    const CString& at(size_t i) const { return m_strings[i]; }
    char** pointers()
    {
        m_pointers.shrink(0);
        for (auto& string : m_strings)
            m_pointers.append(const_cast<char*>(string.data()));
        m_pointers.append(nullptr);
        return m_pointers.mutableSpan().data();
    }

private:
    Vector<CString> m_strings;
    Vector<char*> m_pointers;
};
inline std::optional<int64_t> toFileOffset(JSGlobalObject* globalObject, JSValue value) { return toCLong(globalObject, value); } // Py_off_t_converter()
JSValue intFromUserID(JSGlobalObject*, uid_t); // _PyLong_FromUid(), and _PyLong_FromGid()

// PyUnicode_DecodeFSDefaultAndSize()
JSValue decodeFileSystemBytes(JSGlobalObject*, std::span<const char>);
// That, or bytes if that is what was asked with.
JSValue nameLike(JSGlobalObject*, const PathArgument&, std::span<const char>);

// _pystat_fromstructstat()
JSValue statResultFrom(JSGlobalObject*, const struct stat&);

// Calls what makes a system call until it is not interrupted, seeing each time to what interrupted it. False if that raised.
template<typename Result, typename Function>
bool retryIfInterrupted(JSGlobalObject* globalObject, Result& result, const Function& function)
{
    do {
        result = function();
    } while (result < 0 && errno == EINTR && checkSignals(globalObject));
    return !(result < 0 && errno == EINTR);
}

} } // namespace JSC::Python

#endif // OS(UNIX)
